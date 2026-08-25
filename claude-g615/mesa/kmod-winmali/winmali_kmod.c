/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 *
 * WinMali pan_kmod backend. Mirrors panthor_kmod.c's panthor_kmod_dev /
 * panthor_kmod_vm / panthor_kmod_bo shapes so the panthor_kmod_* public
 * helpers (panthor_kmod_get_csif_props, _get_flush_id, _vm_sync_handle,
 * etc.) consumed by pan_csf and panvk continue to function unchanged.
 *
 * Each DRM_IOCTL_PANTHOR_* call panvk/pan_csf would make is intercepted
 * by winmali_kmod_ioctl() and packed into a WINMALI_ESCAPE_* buffer.
 * Opcodes in WinMaliEscape.h are numbered to match enum drm_panthor_ioctl_id
 * (0..16) so the mapping is trivial; struct layouts are designed to be
 * drop-in replacements for the corresponding drm_panthor_* structs.
 */

#ifdef _WIN32

#include <intrin.h>   /* _ReturnAddress - names the destroy caller without symbols */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "drm-uapi/panthor_drm.h"

#include "util/list.h"
#include "util/os_mman.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/vma.h"

#include "WinMaliEscape.h"
#include "WinMaliWddmBridge.h"   /* Phase 1 WDDM-alloc bridge (struct + setter) */
#include "winmali_d3dkmt.h"
#include "winmali_kmod.h"

#include "pan_kmod.h"
#include "pan_kmod_backend.h"
#include "panthor_kmod.h"

/* --------------------------------------------------------------------- */
/*  Internal types                                                       */
/* --------------------------------------------------------------------- */

/* Defined below; declared here because winmali_kmod_bo_alloc must consult the
   same gate as vm_bind (allocation and addressing have to agree). */
static bool winmali_dxgk_addr_enabled(void);

/* Small dense BO handle allocator - defined lower down, but used by both
   bo_create (dxgk path) and bo_from_allocation. See its definition for why a
   D3DKMT handle must never be used as a panfrost BO handle. */
static uint32_t winmali_alloc_bo_handle(void);
/* kmt -> dense handle identity map; see the definitions further down. */
static void     winmali_record_kmt(uint32_t kmt, uint32_t handle);
static void     winmali_forget_kmt(uint32_t kmt);

struct winmali_kmod_va_collect {
   struct list_head node;
   uint64_t sync_point;
   uint64_t va;
   uint64_t size;
};

struct winmali_kmod_vm {
   struct pan_kmod_vm base;

   /* KMD-assigned VM id (used in WINMALI_VM_BIND etc.). Mirrors
    * pan_kmod_vm::handle, but we store it once here so panthor_kmod_*
    * helpers that container_of into us see the same field. */
   uint32_t vm_id;
   uint32_t pad0;

   /* Auto-VA management: mesa allocates VA from a userspace heap and
    * tells the KMD where to map. AUTO_VA + ASYNC unmap defers reclaim
    * to a sync point on the VM timeline. */
   struct {
      simple_mtx_t lock;
      struct util_vma_heap heap;
      struct list_head gc_list;
   } auto_va;

   /* Per-VM timeline sync object. PAN_KMOD_VM_FLAG_TRACK_ACTIVITY
    * callers (pan_csf, panvk) increment 'point' each time work is
    * submitted to this VM; the syncobj makes waits cheap. */
   struct {
      uint32_t handle;       /* WINMALI_SYNC_OBJ handle, timeline-capable */
      uint64_t point;        /* current sync point */
      simple_mtx_t lock;
   } sync;
};

struct winmali_kmod_bo {
   struct pan_kmod_bo base;

   /* Per-BO sync. If the BO has an exclusive_vm we reuse the VM's
    * sync handle (work touching the BO is already ordered against
    * the VM timeline). Otherwise we own a timeline handle here. */
   struct {
      uint32_t handle;
      uint64_t read_point;
      uint64_t write_point;
      /* ⛔★★★★★ EXPLICIT OWNERSHIP. TRUE only when WE minted this handle.
       *
       * bo_free used to decide ownership from `!bo->exclusive_vm`, which asks
       * the WRONG QUESTION. MEASURED 2026-08-18 in dwm:
       *   bo_free handle=1 exclusive_vm=000002C7BB1AFBC0 imported=0 destroy=0  (ok)
       *   bo_free handle=1 exclusive_vm=0000000000000000 imported=1 destroy=1  (!!)
       * An IMPORTED BO carries the VM's sync handle yet is registered with
       * exclusive_vm=NULL (pan_kmod_bo_init(..., NULL, ...) on the import path),
       * so the guard concluded it owned handle 1 and destroyed it - FOUR times.
       * dwm then signalled the dead handle 79 times and stopped compositing for
       * the whole boot: frozen desktop, no taskbar, dead input, StatSrcChg=0.
       *
       * "Has no exclusive_vm" and "minted its own syncobj" are NOT the same
       * predicate. Record the fact instead of re-deriving it. */
      bool     owns_handle;
      /* ⛔★★★★★ THE HANDLE **WE MINTED** - NOT the same thing as sync.handle.
       *
       * MEASURED 2026-08-18: an imported BO mints handle 18, and after the
       * first submit panthor_kmod_bo_attach_sync_point() (below) OVERWRITES
       * sync.handle with the VM's handle (1), because for a BO with no
       * exclusive_vm it "tracks the external handle directly". bo_free then
       * destroyed sync.handle - i.e. the VM's shared handle, killing dwm for
       * the whole boot - AND leaked the handle it had actually minted.
       *
       * sync.handle answers "what do I sync against right now?" and can be
       * someone else's. THIS answers "what must I free?" and is only ever
       * ours. They were one variable serving two meanings; that is the bug.
       * 0 = we minted nothing and must free nothing. */
      uint32_t owned_handle;
      /* ⛔ DOUBLE-FREE TRIPWIRE. The log shows FOUR bo_free calls each reporting
       * handle=1, but the KMD handle allocator is a boot-unique monotonic
       * counter that can mint a given handle only ONCE per boot
       * (WinMaliSync.c: g_WinMaliSyncHandleNext), and this function is the only
       * importer - so "four BOs that each minted handle 1" is IMPOSSIBLE.
       * The surviving explanations are one BO freed four times, or a BO whose
       * sync.handle was never assigned. `bo=%p` separates them at a glance:
       * the same pointer four times means a refcount bug, four pointers means
       * the handle came from somewhere this code does not know about. */
      uint32_t freed_magic;
   } sync;

   /* Phase 1: if non-zero this BO is backed by a dxgk allocation the WDDM
    * bridge created (via pfnAllocateCb); bo_free releases it through the
    * hook. Zero = escape-BoCreate-backed (baseline). */
   uint32_t wddm_kmt_handle;
   /* Persistent CPU mapping of the dxgk allocation (from pfnLockCb, held for
    * the BO's lifetime). NULL for escape-backed BOs (they map via BoMapCpu). */
   void *wddm_cpu_va;

   /* PROVENANCE OF base.handle. true = minted by the KMD's BoTable (escape
    * BoCreate), so it is safe to send back to the KMD; false = a UMD-synthetic
    * id from winmali_kmod_alloc_bo_handle() which the KMD's BoTable never
    * issued.
    *
    * WHY AN EXPLICIT BIT: every guard used to infer provenance from
    * "wddm_kmt_handle != 0". That inference dies here - imports now carry BOTH
    * a KMT handle AND a real KMD handle, so the old test would wrongly classify
    * them as synthetic. Inferring provenance from backing type is exactly how
    * the collision stayed invisible; store it explicitly instead.
    *
    * MEASURED CONSEQUENCE of getting this wrong (2026-08-15): two minters both
    * starting at 1 both wrote base.handle, panfrost indexes its BO arrays by
    * handle, mesa builds -DNDEBUG so assert(*slot == NULL) is compiled out, and
    * a 32768-byte dxgk import silently clobbered a 4096-byte escape BO's slot -
    * "pool alloc: UNWRITABLE ... claims size=32768 but only 4096 bytes are
    * mapped", descriptor allocation failing, and a black desktop. */
   bool handle_is_kmd;
};

struct winmali_kmod_dev {
   struct pan_kmod_dev base;
   uint32_t           *flush_id;       /* not used on Windows; left as NULL */
   struct {
      WINMALI_GPU_INFO              gpu;
      WINMALI_CSIF_INFO             csif;
      WINMALI_TIMESTAMP_INFO        timestamp;
      WINMALI_GROUP_PRIORITIES_INFO group_priorities;
   } props;

   /* Phase 1 WDDM-allocation bridge (see winmali_kmod.h). NULL alloc =
    * escape BoCreate (baseline). Installed by the C++ d3d10umd frontend. */
   struct winmali_wddm_alloc_ops wddm_alloc;
};

static struct winmali_kmod_dev *
to_winmali_dev(struct pan_kmod_dev *dev)
{
   return container_of(dev, struct winmali_kmod_dev, base);
}

static struct winmali_kmod_vm *
to_winmali_vm(struct pan_kmod_vm *vm)
{
   return container_of(vm, struct winmali_kmod_vm, base);
}

static struct winmali_kmod_bo *
to_winmali_bo(struct pan_kmod_bo *bo)
{
   return container_of(bo, struct winmali_kmod_bo, base);
}

/* --------------------------------------------------------------------- */
/*  Escape send helpers                                                  */
/* --------------------------------------------------------------------- */

/* Single-payload escape. Returns 0 or -errno. Payload is copied to/from
 * the escape buffer so in/out structs work verbatim. */
static int
escape_simple(uint32_t op, void *payload, size_t payload_size)
{
   size_t total = sizeof(WINMALI_ESCAPE_HEADER) + payload_size;
   total = WINMALI_ALIGN_UP(total, WINMALI_TAIL_ALIGN);

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   hdr->Magic       = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor    = WINMALI_ABI_MAJOR;
   hdr->AbiMinor    = WINMALI_ABI_MINOR;
   hdr->Op          = op;
   hdr->PayloadSize = (uint32_t)total;

   if (payload && payload_size)
      memcpy(buf + sizeof(WINMALI_ESCAPE_HEADER), payload, payload_size);

   int err = winmali_d3dkmt_escape(buf, total);

   if (err == 0 && payload && payload_size)
      memcpy(payload, buf + sizeof(WINMALI_ESCAPE_HEADER), payload_size);
   else if (err != 0)
      winmali_d3dkmt_log("wm: escape op 0x%x failed (%d)", op, err);

   free(buf);
   return err;
}

/* --------------------------------------------------------------------- */
/*  Forward declarations                                                 */
/* --------------------------------------------------------------------- */

extern const struct pan_kmod_ops winmali_kmod_ops;

static void winmali_kmod_dev_destroy(struct pan_kmod_dev *dev);
static struct pan_kmod_va_range
winmali_kmod_dev_query_user_va_range(const struct pan_kmod_dev *dev);

static struct pan_kmod_bo *
winmali_kmod_bo_alloc(struct pan_kmod_dev *dev,
                     struct pan_kmod_vm *exclusive_vm,
                     uint64_t size, uint32_t flags);
static void winmali_kmod_bo_free(struct pan_kmod_bo *bo);
static struct pan_kmod_bo *
winmali_kmod_bo_import(struct pan_kmod_dev *dev, uint32_t handle, uint64_t size);
static int winmali_kmod_bo_export(struct pan_kmod_bo *bo, int dmabuf_fd);
static off_t winmali_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo);
static int winmali_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev);
static bool winmali_kmod_bo_wait(struct pan_kmod_bo *bo, int64_t timeout_ns,
                                bool for_read_only_access);

static struct pan_kmod_vm *
winmali_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                      uint64_t va_start, uint64_t va_range);
static void winmali_kmod_vm_destroy(struct pan_kmod_vm *vm);
static int winmali_kmod_vm_bind(struct pan_kmod_vm *vm,
                               enum pan_kmod_vm_op_mode mode,
                               struct pan_kmod_vm_op *ops, uint32_t op_count);
static enum pan_kmod_vm_state
winmali_kmod_vm_query_state(struct pan_kmod_vm *vm);

static uint64_t winmali_kmod_query_timestamp(const struct pan_kmod_dev *dev);
static void winmali_kmod_bo_set_label(struct pan_kmod_dev *dev,
                                     struct pan_kmod_bo *bo,
                                     const char *label);

/* --------------------------------------------------------------------- */
/*  Device                                                               */
/* --------------------------------------------------------------------- */

struct pan_kmod_dev *
winmali_kmod_dev_create(int fd, uint32_t flags,
                        const drmVersionPtr version,
                        const struct pan_kmod_allocator *allocator)
{
   (void)version;
   int err = winmali_d3dkmt_open();
   if (err < 0) {
      errno = -err;
      return NULL;
   }

   struct winmali_kmod_dev *wdev = pan_kmod_alloc(allocator, sizeof(*wdev));
   if (!wdev) {
      winmali_d3dkmt_close();
      errno = ENOMEM;
      return NULL;
   }

   /* DevQuery: GpuInfo. Result lands at offset (header + args). */
   {
      size_t total = sizeof(WINMALI_ESCAPE_HEADER)
                   + sizeof(WINMALI_DEV_QUERY)
                   + sizeof(WINMALI_GPU_INFO);
      total = WINMALI_ALIGN_UP(total, WINMALI_TAIL_ALIGN);

      uint8_t *buf = (uint8_t *)calloc(1, total);
      if (!buf) {
         pan_kmod_free(allocator, wdev);
         winmali_d3dkmt_close();
         errno = ENOMEM;
         return NULL;
      }

      WINMALI_ESCAPE_HEADER *hdr  = (WINMALI_ESCAPE_HEADER *)buf;
      WINMALI_DEV_QUERY     *args = (WINMALI_DEV_QUERY *)(buf + sizeof(*hdr));
      uint32_t result_offset =
         (uint32_t)(sizeof(*hdr) + sizeof(*args));

      hdr->Magic       = WINMALI_ESCAPE_MAGIC;
      hdr->AbiMajor    = WINMALI_ABI_MAJOR;
      hdr->AbiMinor    = WINMALI_ABI_MINOR;
      hdr->Op          = WinMaliEscapeOp_DevQuery;
      hdr->PayloadSize = (uint32_t)total;

      args->Type           = WinMaliDevQuery_GpuInfo;
      args->Size           = sizeof(WINMALI_GPU_INFO);
      args->PointerOffset  = result_offset;

      int qerr = winmali_d3dkmt_escape(buf, total);
      if (qerr == 0)
         memcpy(&wdev->props.gpu, buf + result_offset,
                sizeof(wdev->props.gpu));
      free(buf);

      if (qerr < 0) {
         pan_kmod_free(allocator, wdev);
         winmali_d3dkmt_close();
         errno = -qerr;
         return NULL;
      }
   }

   /* Repeat for CSIF, timestamp, group priorities. Failures are
    * tolerated: CSIF would gate CSF queue submission, the rest are
    * informational. */
   {
      size_t total = sizeof(WINMALI_ESCAPE_HEADER)
                   + sizeof(WINMALI_DEV_QUERY)
                   + sizeof(WINMALI_CSIF_INFO);
      total = WINMALI_ALIGN_UP(total, WINMALI_TAIL_ALIGN);
      uint8_t *buf = (uint8_t *)calloc(1, total);
      if (buf) {
         WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
         WINMALI_DEV_QUERY *args = (WINMALI_DEV_QUERY *)(buf + sizeof(*hdr));
         uint32_t roff = (uint32_t)(sizeof(*hdr) + sizeof(*args));
         hdr->Magic = WINMALI_ESCAPE_MAGIC;
         hdr->AbiMajor = WINMALI_ABI_MAJOR;
         hdr->AbiMinor = WINMALI_ABI_MINOR;
         hdr->Op = WinMaliEscapeOp_DevQuery;
         hdr->PayloadSize = (uint32_t)total;
         args->Type = WinMaliDevQuery_CsifInfo;
         args->Size = sizeof(WINMALI_CSIF_INFO);
         args->PointerOffset = roff;
         if (winmali_d3dkmt_escape(buf, total) == 0)
            memcpy(&wdev->props.csif, buf + roff, sizeof(wdev->props.csif));
         free(buf);
      }
   }

   {
      size_t total = sizeof(WINMALI_ESCAPE_HEADER)
                   + sizeof(WINMALI_DEV_QUERY)
                   + sizeof(WINMALI_TIMESTAMP_INFO);
      total = WINMALI_ALIGN_UP(total, WINMALI_TAIL_ALIGN);
      uint8_t *buf = (uint8_t *)calloc(1, total);
      if (buf) {
         WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
         WINMALI_DEV_QUERY *args = (WINMALI_DEV_QUERY *)(buf + sizeof(*hdr));
         uint32_t roff = (uint32_t)(sizeof(*hdr) + sizeof(*args));
         hdr->Magic = WINMALI_ESCAPE_MAGIC;
         hdr->AbiMajor = WINMALI_ABI_MAJOR;
         hdr->AbiMinor = WINMALI_ABI_MINOR;
         hdr->Op = WinMaliEscapeOp_DevQuery;
         hdr->PayloadSize = (uint32_t)total;
         args->Type = WinMaliDevQuery_TimestampInfo;
         args->Size = sizeof(WINMALI_TIMESTAMP_INFO);
         args->PointerOffset = roff;
         if (winmali_d3dkmt_escape(buf, total) == 0)
            memcpy(&wdev->props.timestamp, buf + roff,
                   sizeof(wdev->props.timestamp));
         free(buf);
      }
   }

   {
      size_t total = sizeof(WINMALI_ESCAPE_HEADER)
                   + sizeof(WINMALI_DEV_QUERY)
                   + sizeof(WINMALI_GROUP_PRIORITIES_INFO);
      total = WINMALI_ALIGN_UP(total, WINMALI_TAIL_ALIGN);
      uint8_t *buf = (uint8_t *)calloc(1, total);
      if (buf) {
         WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
         WINMALI_DEV_QUERY *args = (WINMALI_DEV_QUERY *)(buf + sizeof(*hdr));
         uint32_t roff = (uint32_t)(sizeof(*hdr) + sizeof(*args));
         hdr->Magic = WINMALI_ESCAPE_MAGIC;
         hdr->AbiMajor = WINMALI_ABI_MAJOR;
         hdr->AbiMinor = WINMALI_ABI_MINOR;
         hdr->Op = WinMaliEscapeOp_DevQuery;
         hdr->PayloadSize = (uint32_t)total;
         args->Type = WinMaliDevQuery_GroupPrioritiesInfo;
         args->Size = sizeof(WINMALI_GROUP_PRIORITIES_INFO);
         args->PointerOffset = roff;
         if (winmali_d3dkmt_escape(buf, total) == 0)
            memcpy(&wdev->props.group_priorities, buf + roff,
                   sizeof(wdev->props.group_priorities));
         free(buf);
      }
   }

   /* Synthesize a drmVersion-shaped descriptor so pan_kmod_dev_init is
    * happy. We claim panthor uAPI compat, version (ABI_MAJOR.ABI_MINOR). */
   drmVersion synth_ver;
   memset(&synth_ver, 0, sizeof(synth_ver));
   synth_ver.version_major = WINMALI_ABI_MAJOR;
   synth_ver.version_minor = WINMALI_ABI_MINOR;
   synth_ver.name          = (char *)"panthor";
   synth_ver.name_len      = 7;

   pan_kmod_dev_init(&wdev->base, fd, flags, &synth_ver,
                     &winmali_kmod_ops, allocator);

   /* Mirror per-arch / per-product fields panthor caches in
    * pan_kmod_dev_props from gpu_info. */
   struct pan_kmod_dev_props *props = &wdev->base.props;
   props->gpu_id               = wdev->props.gpu.GpuId;
   props->shader_present       = wdev->props.gpu.ShaderPresent;
   props->tiler_features       = wdev->props.gpu.TilerFeatures;
   /* A zero TilerFeatures poisons every draw: pan_query decodes it to
    * max_levels=0, pan_select_tiler_hierarchy_mask() then computes
    * hierarchy_mask=0 and asserts (pan_desc.c "hierarchy_mask != 0 ||
    * PAN_ARCH >= 12"); in an assert-off build the zero mask reaches the
    * TILER_CONTEXT and the CS faults DATA_INVALID (the v33-v37 saga).
    * Old/degraded KMDs report 0 here - substitute the value the real
    * Mali-G610 TILER_FEATURES register reads and warn, so one bad cap
    * can't crash every D3D process on the box. */
   if (props->tiler_features == 0) {
      props->tiler_features = 0x809;   /* bin_size=1<<9, max_levels=8 */
      winmali_d3dkmt_log("wm: WARNING KMD reported TilerFeatures=0, "
                         "substituting 0x809 (G610 default)");
   }
   props->mem_features         = wdev->props.gpu.MemFeatures;
   props->mmu_features         = wdev->props.gpu.MmuFeatures;
   props->max_threads_per_wg   = wdev->props.gpu.ThreadMaxWorkgroupSize;
   props->max_threads_per_core = wdev->props.gpu.MaxThreads;
   for (unsigned i = 0; i < 4; i++)
      props->texture_features[i] = wdev->props.gpu.TextureFeatures[i];
   props->gpu_can_query_timestamp = wdev->props.timestamp.TimestampFrequency != 0;
   props->timestamp_frequency = wdev->props.timestamp.TimestampFrequency;
   props->allowed_group_priorities_mask =
      (enum pan_kmod_group_allow_priority_flags)
      wdev->props.group_priorities.AllowedMask;

   winmali_d3dkmt_log("wm: kmod dev up (gpu_id=0x%08x shader=0x%llx ts_freq=%llu)",
                      props->gpu_id,
                      (unsigned long long)props->shader_present,
                      (unsigned long long)props->timestamp_frequency);

   return &wdev->base;
}

static void
winmali_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   struct winmali_kmod_dev *wdev = to_winmali_dev(dev);

   pan_kmod_dev_cleanup(dev);
   pan_kmod_dev_free(dev, wdev);

   winmali_d3dkmt_close();
}

static struct pan_kmod_va_range
winmali_kmod_dev_query_user_va_range(const struct pan_kmod_dev *dev)
{
   const struct winmali_kmod_dev *wdev =
      container_of(dev, const struct winmali_kmod_dev, base);
   uint64_t va_bits = wdev->props.gpu.MmuFeatures & 0xff;
   return (struct pan_kmod_va_range){ .start = 0, .size = 1ull << va_bits };
}

/* --------------------------------------------------------------------- */
/*  Sync-object helpers (timeline-capable WinMali sync objects)          */
/* --------------------------------------------------------------------- */

/* The pan_kmod ioctl/escape path is dev-less (global winmali_d3dkmt
 * singleton), but the WDDM ops (render/fence hooks + cookie) are per-device.
 * Mirror them into a file-static so the dev-less syncobj + group_submit helpers
 * can reach them. Single-device bring-up. */
static struct winmali_wddm_alloc_ops g_wddm_ops;

/* Phase 4 R1: when the dxgk monitored-fence hooks are installed
 * (WINMALI_DXGK_FENCE, mirrored into g_wddm_ops), the syncobj helpers route
 * through them instead of the escape SyncObj ops - the first concrete step of
 * retiring the escape from the fully-dxgk path. NULL hooks = escape (baseline).
 * A monitored fence's timeline value maps 1:1 to the WinMali timeline point. */
/* ⛔⛔ FENCES MUST FOLLOW SUBMISSION. THIS WAS REJECTING EVERY GPU SUBMIT.
 *
 * The frontend installs the dxgk data plane unconditionally ("no env gates"),
 * but ADDRESSING and SUBMISSION are gated in this file (WINMALI_DXGK_ADDR /
 * _SUBMIT). Fences were not - so in the escape configuration we created dxgk
 * MONITORED FENCES and then handed their handles to escape GroupSubmit, whose
 * sync ops are resolved against the KMD's OWN syncobj table. That table never
 * issued them, so every submit carrying a wait died with ENOENT.
 *
 * MEASURED: SyncBadHandle = 0x40000800 - a D3DKMT-shaped handle (compare the
 * import logs: kmt=0x40000fc0), not a KMD syncobj handle, which are small
 * integers from a counter starting at 1. syncFail=151-407 per window, 671-1258
 * rejected submits, every one carrying syncs=3.
 *
 * ⚠ This is the same lesson already recorded as "allocation + addressing +
 * submission are ONE design" - fences belong to that set. Mixing a dxgk fence
 * with an escape submit cannot work in either direction.
 *
 * So: use the dxgk fence hooks ONLY when the dxgk submit path is active. Same
 * env var, read the same way, so the two can never disagree. */
static bool
winmali_use_dxgk_fences(void)
{
   static int s_use = -1;
   if (s_use < 0) {
      const char *e = getenv("WINMALI_DXGK_SUBMIT");
      s_use = (e && *e && *e != '0') ? 1 : 0;
      mesa_loge("wm: fence path = %s (must match the SUBMIT path, or every "
                "submit with a wait op dies with ENOENT)",
                s_use ? "dxgk monitored fences" : "escape syncobjs (default)");
   }
   return s_use != 0;
}

/* ⛔★★★★★ LIVE VM SYNC-HANDLE REGISTRY - THE DESKTOP-SAVING GUARD.
 *
 * MEASURED 2026-08-18 (dwm, clean boot): dwm creates VM id=1 with sync
 * handle=1, then bo_free destroys "handle 1" FOUR times, after which every
 * submit returns err=-2 (ENOENT) - 79 of them - and dwm stops compositing for
 * the entire boot. Frozen desktop, no taskbar, dead input.
 *
 * The VM's sync handle is shared by construction: csf_submit re-reads
 * panthor_kmod_vm_sync_handle(dev->kmod.vm) on EVERY submit. So ANY path that
 * destroys it takes the whole session down, permanently, from one call.
 * A shared handle with many potential destroyers needs a single owner, and the
 * VM is it.
 *
 * This registry makes that ownership enforceable rather than assumed: the VM
 * registers its handle at create and unregisters at destroy, and syncobj_destroy
 * refuses any handle currently owned by a live VM. Leaking a handle is a
 * bounded, silent cost; destroying this one costs the entire desktop.
 *
 * ⚠ This is a GUARD, not the diagnosis - it names the culprit in the log
 * (WMSYNCGUARD1 + caller RA) instead of hiding it. Keep reading that line even
 * once the desktop is stable. */
#define WINMALI_MAX_VM_SYNC 16
static uint32_t g_vm_sync_handles[WINMALI_MAX_VM_SYNC];

static void
winmali_vm_sync_register(uint32_t h)
{
   if (!h)
      return;
   for (unsigned i = 0; i < WINMALI_MAX_VM_SYNC; i++) {
      if (g_vm_sync_handles[i] == 0) {
         g_vm_sync_handles[i] = h;
         return;
      }
   }
   mesa_loge("wm: WMSYNCGUARD0 VM sync registry FULL (%u slots) - handle=%u is "
             "UNPROTECTED; a stray bo_free can now kill this VM",
             WINMALI_MAX_VM_SYNC, h);
}

static void
winmali_vm_sync_unregister(uint32_t h)
{
   for (unsigned i = 0; i < WINMALI_MAX_VM_SYNC; i++) {
      if (g_vm_sync_handles[i] == h) {
         g_vm_sync_handles[i] = 0;
         return;
      }
   }
}

static bool
winmali_vm_sync_is_live(uint32_t h)
{
   if (!h)
      return false;
   for (unsigned i = 0; i < WINMALI_MAX_VM_SYNC; i++)
      if (g_vm_sync_handles[i] == h)
         return true;
   return false;
}

static int
syncobj_create_timeline(uint32_t *out_handle)
{
   if (winmali_use_dxgk_fences() && g_wddm_ops.fence_create != NULL)
      return g_wddm_ops.fence_create(g_wddm_ops.cookie, out_handle);

   WINMALI_SYNC_OBJ_CREATE req = {
      .Flags = WINMALI_SYNC_OBJ_FLAG_TIMELINE,
      .InitialState = 0,
   };
   int err = escape_simple(WinMaliEscapeOp_SyncObjCreate, &req, sizeof(req));
   if (err)
      return err;
   *out_handle = req.Handle;
   /* ⛔ WMSYNCMINT1 - "WAS THIS HANDLE MINTED, OR COPIED?"
    *
    * The KMD allocator is a boot-unique monotonic counter that NEVER recycles
    * (WinMaliSync.c: g_WinMaliSyncHandleNext), so a given handle can be minted
    * at most ONCE per boot. Yet dwm's log shows FOUR distinct imported BOs each
    * holding handle=1 - which that allocator cannot produce. Either the counter
    * is not what it claims, or those BOs never minted and got handle 1 by copy.
    *
    * Printing every mint with its return address separates the two in one boot,
    * instead of a fifth round of reasoning about who could have copied it. */
   mesa_loge("wm: WMSYNCMINT1 syncobj_create_timeline -> handle=%u caller_ra=%p",
             *out_handle, _ReturnAddress());
   return 0;
}

static int
syncobj_destroy(uint32_t handle)
{
   /* ⛔★★★★★ THE REAL CHOKE POINT - THERE ARE **TWO** ESCAPE SENDERS.
    *
    * I first instrumented winmali_d3dkmt_syncobj_destroy (the d3dkmt helper the
    * drmSyncobjDestroy compat shim calls) and it logged ZERO destroys, while the
    * KMD ledger still recorded handle=1 being destroyed by an explicit
    * SyncObjDestroy escape from dwm's pid. Reason: THIS function builds the
    * escape itself via escape_simple() and never goes through that helper. Right
    * instinct, wrong function.
    *
    * ⇒ When instrumenting a "choke point", first PROVE it is the only one. Two
    * senders for the same escape op is exactly how a funnel stops being a funnel.
    *
    * Context: dwm's VM sync handle=1 is destroyed and then signalled 79 times
    * until dwm stops compositing for the whole boot. All three suspected callers
    * (vm_destroy x2 instrumented and never firing, bo_create+exclusive_vm,
    * bo_import which mints its own handle) checked out CORRECT - so the caller
    * is something not yet considered. _ReturnAddress() names it without symbols;
    * distinct RAs group the call sites. */
   mesa_loge("wm: WMSYNCDESTROY2 syncobj_destroy(handle=%u) caller_ra=%p dxgk=%d",
             handle, _ReturnAddress(),
             (winmali_use_dxgk_fences() && g_wddm_ops.fence_destroy != NULL) ? 1 : 0);

   /* ⛔★★★★★ REFUSE TO DESTROY A LIVE VM'S SYNC HANDLE.
    *
    * This one destroy costs the whole desktop: csf_submit re-reads the VM sync
    * handle every frame, so once it is gone EVERY submit returns ENOENT and dwm
    * stops compositing for the rest of the boot (measured: 79 consecutive).
    * Leaking a syncobj is bounded and silent; this is neither.
    *
    * The log names the caller (RA) so the guard reports the bug instead of
    * burying it - if this line ever fires, there is still a real ownership bug
    * upstream to fix. */
   if (winmali_vm_sync_is_live(handle)) {
      mesa_loge("wm: WMSYNCGUARD1 REFUSED destroy of handle=%u - it belongs to a "
                "LIVE VM and is shared by every submit. caller_ra=%p. This is a "
                "REAL ownership bug upstream; the guard only stops it taking the "
                "desktop down.", handle, _ReturnAddress());
      return 0;
   }

   if (winmali_use_dxgk_fences() && g_wddm_ops.fence_destroy != NULL) {
      g_wddm_ops.fence_destroy(g_wddm_ops.cookie, handle);
      return 0;
   }
   WINMALI_SYNC_OBJ_DESTROY req = { .Handle = handle };
   return escape_simple(WinMaliEscapeOp_SyncObjDestroy, &req, sizeof(req));
}

static int
syncobj_signal(uint32_t handle, uint64_t point)
{
   if (winmali_use_dxgk_fences() && g_wddm_ops.fence_signal != NULL)
      return g_wddm_ops.fence_signal(g_wddm_ops.cookie, handle, point);

   WINMALI_SYNC_OBJ_SIGNAL req = { .Handle = handle, .Point = point };
   return escape_simple(WinMaliEscapeOp_SyncObjSignal, &req, sizeof(req));
}

/* ★ Escape routed through the D3D RUNTIME'S device, not winmali's private one.
 *
 * escape_simple() sends on g_wm.device, a device winmali creates for itself.
 * Any op that resolves a D3DKMT handle via DxgkCbGetHandleData - notably
 * WinMaliEscapeOp_BoFromAllocation (0x81) - can only see allocations created by
 * the device the escape arrives on, so from the private device it can never
 * resolve a runtime-created swap-chain surface. That is why the import path
 * fell back to BoCreate and got FRESH pages, and why the GPU's output never
 * landed in the surface the present worker scans out.
 *
 * The frontend installs g_wddm_ops.escape, which issues the escape on the
 * device that actually created the allocation. Returns 0 or -errno; -ENOSYS if
 * no frontend is registered (e.g. the GL/VK paths). */
static int
escape_on_runtime_device(uint32_t op, void *payload, size_t payload_size)
{
   if (g_wddm_ops.escape == NULL)
      return -ENOSYS;

   size_t total = sizeof(WINMALI_ESCAPE_HEADER) + payload_size;
   total = WINMALI_ALIGN_UP(total, WINMALI_TAIL_ALIGN);

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   hdr->Magic       = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor    = WINMALI_ABI_MAJOR;
   hdr->AbiMinor    = WINMALI_ABI_MINOR;
   hdr->Op          = op;
   hdr->PayloadSize = (uint32_t)total;

   if (payload && payload_size)
      memcpy(buf + sizeof(WINMALI_ESCAPE_HEADER), payload, payload_size);

   int err = g_wddm_ops.escape(g_wddm_ops.cookie, buf, (uint32_t)total);
   if (err == 0)
      err = (int)hdr->Status;   /* the KMD reports per-op status in the header */

   if (err == 0 && payload && payload_size)
      memcpy(payload, buf + sizeof(WINMALI_ESCAPE_HEADER), payload_size);
   else if (err != 0)
      winmali_d3dkmt_log("wm: runtime-device escape op 0x%x failed (%d)", op, err);

   free(buf);
   return err;
}

static int
syncobj_timeline_wait(uint32_t handle, uint64_t point, int64_t timeout_ns)
{
   if (winmali_use_dxgk_fences() && g_wddm_ops.fence_wait != NULL)
      return g_wddm_ops.fence_wait(g_wddm_ops.cookie, handle, point);

   size_t header_size = sizeof(WINMALI_ESCAPE_HEADER);
   size_t payload_size = sizeof(WINMALI_SYNC_OBJ_WAIT);
   size_t handles_off = WINMALI_ALIGN_UP(header_size + payload_size,
                                         WINMALI_TAIL_ALIGN);
   size_t points_off  = WINMALI_ALIGN_UP(handles_off + sizeof(uint32_t),
                                         WINMALI_TAIL_ALIGN);
   size_t total       = WINMALI_ALIGN_UP(points_off + sizeof(uint64_t),
                                         WINMALI_TAIL_ALIGN);

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   WINMALI_SYNC_OBJ_WAIT *wait = (WINMALI_SYNC_OBJ_WAIT *)(buf + header_size);

   hdr->Magic       = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor    = WINMALI_ABI_MAJOR;
   hdr->AbiMinor    = WINMALI_ABI_MINOR;
   hdr->Op          = WinMaliEscapeOp_SyncObjWait;
   hdr->PayloadSize = (uint32_t)total;

   wait->Flags         = WINMALI_SYNC_OBJ_WAIT_FLAG_ALL;
   wait->Count         = 1;
   wait->HandlesOffset = (uint32_t)handles_off;
   wait->PointsOffset  = (uint32_t)points_off;
   wait->TimeoutNs     = timeout_ns;

   *(uint32_t *)(buf + handles_off) = handle;
   *(uint64_t *)(buf + points_off)  = point;

   int err = winmali_d3dkmt_escape(buf, total);
   free(buf);
   return err;
}

/* --------------------------------------------------------------------- */
/*  Buffer objects                                                       */
/* --------------------------------------------------------------------- */

static struct pan_kmod_bo *
winmali_kmod_bo_alloc(struct pan_kmod_dev *dev,
                      struct pan_kmod_vm *exclusive_vm,
                      uint64_t size, uint32_t flags)
{
   struct winmali_kmod_dev *wdev = to_winmali_dev(dev);
   struct winmali_kmod_bo *wbo = pan_kmod_dev_alloc(dev, sizeof(*wbo));
   if (!wbo) {
      errno = ENOMEM;
      return NULL;
   }

   uint32_t bo_handle = 0;
   uint32_t wddm_kmt  = 0;
   void    *wddm_cpu  = NULL;

   /* dxgk-native path (default whenever the WDDM bridge is installed): back the
    * BO with a REAL dxgk allocation, pinned resident + CPU-mapped by the C++
    * frontend (pfnAllocateCb/pfnLockCb). It is addressed via GpuMmu (vm_bind ->
    * pfnMapGpuVirtualAddressCb) and CPU-accessed through the locked VA - no
    * escape BoCreate, no BoFromAllocation aliasing. A failure is a HARD error,
    * surfaced to the caller rather than masked by an escape fallback. */
   if (wdev->wddm_alloc.alloc && winmali_dxgk_addr_enabled()) {
      uint32_t kmt = 0;
      void *cpu = NULL;
      int aerr = wdev->wddm_alloc.alloc(wdev->wddm_alloc.cookie, size,
                                        &kmt, &cpu);
      if (aerr || !kmt) {
         pan_kmod_dev_free(dev, wbo);
         errno = aerr ? -aerr : ENOMEM;
         return NULL;
      }
      wddm_kmt  = kmt;
      wddm_cpu  = cpu;
      /* NOT the KMT handle - see winmali_alloc_bo_handle(). A ~0x40000000
         handle makes panfrost allocate and repeatedly scan a 1 GiB array.
         ⚠ This is the ONLY remaining synthetic-handle minter. handle_is_kmd
         stays false so the guards below reject it loudly instead of letting it
         alias a real BoTable BO. */
      bo_handle = winmali_alloc_bo_handle();

      /* ⚠ THIS BRANCH IS THE LIVE ONE on the dev tablet: WINMALI_DXGK_ADDR=1 is
       * set machine-wide, so the "[default]" escape branch below does NOT run
       * for ordinary allocations.
       *
       * It had NO identity trace, which actively misled the investigation: the
       * trace lived only in the escape branch, so "asked=32768 appears 0 times
       * in a whole boot" was read as "that size is never requested" when it
       * really meant "the branch that requests it is untraced". Trace both
       * branches or absence of a line will be mistaken for absence of the
       * event. */
      winmali_d3dkmt_log("wm: bo_alloc[dxgk] obj=%p handle=%u size=%llu "
                         "kmt=0x%x cpu=%p flags=0x%x",
                         (void *)wbo, bo_handle, (unsigned long long)size,
                         wddm_kmt, wddm_cpu, flags);
   } else {
      /* No WDDM bridge (non-D3D build): escape BoCreate. */
      WINMALI_BO_CREATE req = {
         .Size  = size,
         .Flags = flags & 0xff,
         .ExclusiveVmId = exclusive_vm ? to_winmali_vm(exclusive_vm)->vm_id : 0,
      };
      int err = escape_simple(WinMaliEscapeOp_BoCreate, &req, sizeof(req));
      if (err) {
         pan_kmod_dev_free(dev, wbo);
         errno = -err;
         return NULL;
      }
      bo_handle = req.Handle;
      wbo->handle_is_kmd = true;   /* BoTable-issued: safe to send to the KMD */

      /* Size is IN/OUT: req.Size now holds what the kernel ACTUALLY created.
       * Record that, not `size` - this is what panthor_kmod.c does with
       * req.size, and skipping it is how mesa came to believe a BO was 32768
       * bytes when the kernel had made 4096. Rounding up is normal and silent;
       * anything SMALLER than requested is a real defect, so say so. */
      if (req.Size < size) {
         mesa_loge("winmali: BO %u UNDERSIZED - requested %llu bytes, kernel "
                   "created only %llu. Recording the kernel's size; callers "
                   "will see a smaller BO rather than writing past its end.",
                   bo_handle, (unsigned long long)size,
                   (unsigned long long)req.Size);
      }
      /* Identity trace - emitted BEFORE `size` is overwritten, so "asked" and
       * "kernel" are genuinely two different numbers.
       *
       * The pool reports a BO as handle=74/size=32768 while the kernel logs
       * handle=74 created from a 4096 request, and both the handle and the
       * size come from ONE pan_kmod_bo - so either two pan_kmod_bo objects
       * carry the same handle, or something rewrites the size afterwards.
       * Printing the object ADDRESS beside the handle separates those without
       * any cross-line inference: same handle + different pointer = aliasing;
       * same pointer + changed size = mutation. */
      winmali_d3dkmt_log("wm: bo_alloc obj=%p handle=%u asked=%llu kernel=%llu "
                         "flags=0x%x vm=%u",
                         (void *)wbo, bo_handle, (unsigned long long)size,
                         (unsigned long long)req.Size, flags,
                         exclusive_vm ? to_winmali_vm(exclusive_vm)->vm_id : 0u);

      if (req.Size != 0)
         size = req.Size;
   }

   /* Sync handle: reuse VM's if exclusive, otherwise mint a fresh
    * timeline so unrelated work doesn't block reads. */
   if (exclusive_vm) {
      wbo->sync.handle = to_winmali_vm(exclusive_vm)->sync.handle;
      wbo->sync.owns_handle = false;   /* SHARED - the VM owns it, never free it here */
      wbo->sync.owned_handle = 0;
   } else {
      uint32_t h = 0;
      int serr = syncobj_create_timeline(&h);
      if (serr) {
         if (wddm_kmt) {
            wdev->wddm_alloc.free(wdev->wddm_alloc.cookie, wddm_kmt);
         } else {
            WINMALI_BO_DESTROY d = { .Handle = bo_handle };
            escape_simple(WinMaliEscapeOp_BoDestroy, &d, sizeof(d));
         }
         pan_kmod_dev_free(dev, wbo);
         errno = -serr;
         return NULL;
      }
      wbo->sync.handle = h;
      wbo->sync.owns_handle = true;    /* we minted it, so we free it */
      wbo->sync.owned_handle = h;      /* remember WHICH one, sync.handle can be replaced */
   }
   wbo->sync.read_point  = 0;
   wbo->sync.write_point = 0;
   wbo->wddm_kmt_handle  = wddm_kmt;
   wbo->wddm_cpu_va      = wddm_cpu;

   pan_kmod_bo_init(&wbo->base, dev, exclusive_vm, size,
                    flags, bo_handle);
   return &wbo->base;
}

/* group_handle -> vm_id map for the Phase-3 render reroute (the group submit
 * ioctl carries the group handle; the CS submit descriptor needs the vm id). */
#define WINMALI_GROUP_VM_MAX 128
static struct { uint32_t group; uint32_t vm_id; bool used; }
   g_group_vm[WINMALI_GROUP_VM_MAX];
static simple_mtx_t g_group_vm_lock = SIMPLE_MTX_INITIALIZER;

static void
winmali_group_vm_record(uint32_t group, uint32_t vm_id)
{
   simple_mtx_lock(&g_group_vm_lock);
   for (int i = 0; i < WINMALI_GROUP_VM_MAX; i++) {
      if (!g_group_vm[i].used) {
         g_group_vm[i].group = group;
         g_group_vm[i].vm_id = vm_id;
         g_group_vm[i].used  = true;
         break;
      }
   }
   simple_mtx_unlock(&g_group_vm_lock);
}

static uint32_t
winmali_group_vm_lookup(uint32_t group)
{
   uint32_t vm = 0;
   simple_mtx_lock(&g_group_vm_lock);
   for (int i = 0; i < WINMALI_GROUP_VM_MAX; i++) {
      if (g_group_vm[i].used && g_group_vm[i].group == group) {
         vm = g_group_vm[i].vm_id;
         break;
      }
   }
   simple_mtx_unlock(&g_group_vm_lock);
   return vm;
}

void
winmali_kmod_set_wddm_alloc_ops(struct pan_kmod_dev *dev,
                                const struct winmali_wddm_alloc_ops *ops)
{
   struct winmali_kmod_dev *wdev = to_winmali_dev(dev);
   if (ops) {
      wdev->wddm_alloc = *ops;
      g_wddm_ops = *ops;
   } else {
      /* ⚠ DO NOT let one device's teardown disable dxgk addressing for the
       * others.
       *
       * DestroyDevice calls this with NULL, and it used to wipe the
       * process-global g_wddm_ops unconditionally. Any process that creates
       * more than one D3D device therefore lost dxgk ADDRESSING the moment the
       * FIRST device died - while the dxgk SUBMIT path stayed on (its gate is
       * separate). That split is fatal: mapping falls back to the escape
       * VmBind while the command stream is submitted through dxgk, so the GPU
       * reads a VA that was never mapped. Observed exactly:
       *     CSF fault: group AS2 exc=0xc3 TRANSLATION_FAULT_3 ADDR=0xfffffff8b000
       *     (that VA has no VmMap/VmBind line anywhere in the ring)
       *     map_va/GpuMmu lines: 0, while "submit path = PHASE3 dxgk render cb"
       * and every create_context then failed (0 of 19).
       *
       * Phase 2 (dxgk mapping) and Phase 3 (dxgk submit) are a MATCHED PAIR and
       * must never be split. Callers that know which device is going away
       * should use winmali_kmod_clear_wddm_alloc_ops() so a device that does
       * NOT own the installed hook leaves it alone. This unconditional form is
       * kept only for callers with no cookie to offer. */
      memset(&g_wddm_ops, 0, sizeof(g_wddm_ops));
      memset(&wdev->wddm_alloc, 0, sizeof(wdev->wddm_alloc));
   }
}

/* Remove the hook ONLY if `cookie` is the device that installed it.
 *
 * The pan_kmod device is shared per ADAPTER, so several D3D devices overwrite
 * each other's ops here. Clearing unconditionally on any DestroyDevice is what
 * disabled dxgk addressing for the surviving devices while dxgk submit stayed
 * enabled - the fatal Phase-2/Phase-3 split. */
void
winmali_kmod_clear_wddm_alloc_ops(struct pan_kmod_dev *dev, void *cookie)
{
   struct winmali_kmod_dev *wdev = to_winmali_dev(dev);

   if (wdev->wddm_alloc.cookie == cookie)
      memset(&wdev->wddm_alloc, 0, sizeof(wdev->wddm_alloc));

   if (g_wddm_ops.cookie == cookie)
      memset(&g_wddm_ops, 0, sizeof(g_wddm_ops));
}

static void
winmali_kmod_bo_free(struct pan_kmod_bo *bo)
{
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);

   /* ⛔★★★★★ THIS IS WHERE dwm's VM SYNC HANDLE DIES - FOUR TIMES OVER.
    *
    * MEASURED: dwm creates VM id=1 with sync handle=1, then
    *   syncobj_destroy(handle=1) x4, ALL from this one return address,
    * after which every submit fails ENOENT and dwm stops compositing for the
    * whole boot. A handle freed FOUR times is a SHARED handle being released by
    * multiple owners - `bo->exclusive_vm` is evidently NULL on BOs that carry
    * `vm->sync.handle`, so this guard does not protect it.
    *
    * The guard and the assignment use DIFFERENT predicates, which is the bug
    * shape: bo_create copies the VM's handle when `exclusive_vm` is non-NULL,
    * but ownership is later judged by `bo->exclusive_vm` as stored by
    * pan_kmod_bo_init - and the import path passes NULL there explicitly.
    *
    * Log the discriminator rather than reason about it a fourth time: print the
    * handle, the stored exclusive_vm, and whether the BO is IMPORTED. */
   mesa_loge("wm: WMBOFREE1 bo_free bo=%p handle=%u owned=%u exclusive_vm=%p "
             "imported=%d%s",
             (void *)wbo, wbo->sync.handle, wbo->sync.owned_handle,
             (void *)bo->exclusive_vm,
             (bo->flags & PAN_KMOD_BO_FLAG_IMPORTED) ? 1 : 0,
             (wbo->sync.freed_magic == 0xDEADBE1Fu)
                ? "  *** DOUBLE FREE - THIS BO WAS ALREADY FREED ***" : "");
   wbo->sync.freed_magic = 0xDEADBE1Fu;

   /* Free the handle WE MINTED - never sync.handle, which
      panthor_kmod_bo_attach_sync_point may have replaced with somebody else's
      (measured: the VM's handle 1, destroyed 4x, desktop dead for the boot). */
   if (wbo->sync.owned_handle != 0) {
      if (wbo->sync.owned_handle != wbo->sync.handle) {
         mesa_loge("wm: WMBOFREE2 bo=%p freeing MINTED handle=%u while "
                   "sync.handle=%u (it was retargeted by attach_sync_point) - "
                   "freeing the right one",
                   (void *)wbo, wbo->sync.owned_handle, wbo->sync.handle);
      }
      syncobj_destroy(wbo->sync.owned_handle);
      wbo->sync.owned_handle = 0;
      wbo->sync.owns_handle  = false;
   }

   /* Destroy the KMD BO whenever the handle is KMD-issued. This is no longer
      an either/or with the dxgk backing: an IMPORT now has BOTH a KMT handle
      (the runtime's allocation, which we must NOT free) and its own BoTable BO
      (which we MUST free, or it leaks nonpaged pages for the process lifetime).
      The old code keyed this on wddm_kmt_handle==0 and so would have leaked
      every import's BO. */
   if (wbo->handle_is_kmd) {
      WINMALI_BO_DESTROY d = { .Handle = bo->handle };
      escape_simple(WinMaliEscapeOp_BoDestroy, &d, sizeof(d));
   }

   if (wbo->wddm_kmt_handle) {
      /* Drop the kmt -> handle identity mapping, or a later allocation that
         reuses this KMT value would be deduped onto a dead BO. */
      winmali_forget_kmt(wbo->wddm_kmt_handle);

      /* Release the dxgk allocation ONLY if we created it - an IMPORTED
       * backbuffer belongs to the D3D runtime / DWM, so freeing it would tear
       * down their surface. */
      if (!(bo->flags & PAN_KMOD_BO_FLAG_IMPORTED)) {
         struct winmali_kmod_dev *wdev = to_winmali_dev(bo->dev);
         if (wdev->wddm_alloc.free)
            wdev->wddm_alloc.free(wdev->wddm_alloc.cookie, wbo->wddm_kmt_handle);
      }
   }

   pan_kmod_bo_cleanup(bo);
   pan_kmod_dev_free(bo->dev, wbo);
}

static struct pan_kmod_bo *
winmali_kmod_bo_import(struct pan_kmod_dev *dev, uint32_t handle, uint64_t size)
{
   /* WinMali doesn't expose dma-buf import paths; cross-process sharing
    * on Windows goes through D3DKMT shared handles and lands in
    * panfrost_winmali_winsys via WINSYS_HANDLE_TYPE_WIN32_KMT. The
    * pan_kmod path is for DRI3/Wayland-shaped imports we don't have. */
   (void)dev; (void)handle; (void)size;
   errno = ENOSYS;
   return NULL;
}

static int
winmali_kmod_bo_export(struct pan_kmod_bo *bo, int dmabuf_fd)
{
   (void)bo; (void)dmabuf_fd;
   errno = ENOSYS;
   return -1;
}

static off_t
winmali_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo)
{
   /* Same reasoning as winmali_kmod_bo_map_cpu(): a dxgk-backed BO is not in
      the KMD's escape BO table, and bo->handle is a synthetic dense id, so this
      escape would query the wrong object (or nothing). Fail cleanly. */
   if (to_winmali_bo(bo)->wddm_kmt_handle) {
      errno = ENXIO;
      return (off_t)-1;
   }

   WINMALI_BO_MMAP_OFFSET req = { .Handle = bo->handle };
   int err = escape_simple(WinMaliEscapeOp_BoMmapOffset, &req, sizeof(req));
   if (err) {
      errno = -err;
      return (off_t)-1;
   }
   return (off_t)req.Offset;
}

static int
winmali_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
   /* ⛔★★★★★ THIS USED TO `return 0`, AND THAT LOSES EVERY CPU WRITE.
    *
    * The old comment claimed "the KMD performs cache maintenance synchronously
    * inside BoCreate / BoMmapOffset", so nothing needed flushing here. Those run
    * ONCE - at allocation and at mapping - never after the app WRITES. And we report
    * MEMORY_ARCHITECTURE UMA=TRUE **CacheCoherent=FALSE**, so a CPU write sits in the
    * CPU cache until something cleans it.
    *
    * MEASURED (WMMAPW, 1.1.19.9): DirectWrite writes glyph runs into A8 textures and
    * the data is provably present at unmap - Explorer 442x66 with 25438/28288
    * non-zero at full range. Those glyphs never reach the panel, while everything the
    * GPU itself draws (A8 shape masks, the BGRA wallpaper) is perfect. That is the
    * signature of unflushed CPU writes: the GPU samples stale memory.
    * ⚠ A CPU-side read-back CANNOT detect this - it reads the same cached view the
    * writer used. Verify on the PANEL, not with a read-back.
    *
    * The KMD already implements the operation (escape op 15 BoSync, ForDevice =
    * "flush caches before GPU access") and pan_kmod already queues the ranges; this
    * backend simply never called it. Walk the list panthor walks and emit one escape;
    * the template is panthor_kmod_flush_bo_map_syncs().
    *
    * ⛔ handle_is_kmd IS MANDATORY, NOT DEFENSIVE: a synthetic id would aim the flush
    * at an UNRELATED BoTable BO. BoSetLabel skips those because a mislabel is merely
    * cosmetic - here it would corrupt another BO's coherency state, so skipping is
    * the only safe answer. */
   unsigned n = util_dynarray_num_elements(&dev->pending_bo_syncs.array,
                                           struct pan_kmod_deferred_bo_sync);
   if (n == 0)
      return 0;

   const size_t header = sizeof(WINMALI_ESCAPE_HEADER);
   const size_t ops_off = header + sizeof(WINMALI_BO_SYNC);
   size_t total = WINMALI_ALIGN_UP(ops_off + (size_t)n * sizeof(WINMALI_BO_SYNC_OP),
                                   WINMALI_TAIL_ALIGN);

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   WINMALI_BO_SYNC *args = (WINMALI_BO_SYNC *)(buf + header);
   WINMALI_BO_SYNC_OP *ops = (WINMALI_BO_SYNC_OP *)(buf + ops_off);

   uint32_t count = 0;
   util_dynarray_foreach(&dev->pending_bo_syncs.array,
                         struct pan_kmod_deferred_bo_sync, sync) {
      if (sync->bo == NULL || !to_winmali_bo(sync->bo)->handle_is_kmd)
         continue;
      ops[count].Handle = sync->bo->handle;
      /* The escape carries one direction per op. The direction that matters for
         correctness here is ForDevice (make CPU writes visible to the GPU), so ask
         for that in both cases rather than silently doing nothing for the
         FLUSH_AND_INVALIDATE flavour. */
      ops[count].Type = WinMaliBoSyncForDevice;
      ops[count].Offset = sync->start;
      ops[count].Size = sync->size;
      count++;
   }

   if (count == 0) {
      free(buf);
      return 0;
   }

   hdr->Magic = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor = WINMALI_ABI_MAJOR;
   hdr->AbiMinor = WINMALI_ABI_MINOR;
   hdr->Op = WinMaliEscapeOp_BoSync;
   hdr->PayloadSize = (uint32_t)total;
   args->OpsCount = count;
   args->OpsOffset = (uint32_t)ops_off;

   int err = winmali_d3dkmt_escape(buf, total);
   if (err) {
      static unsigned s_warned = 0;
      if ((s_warned++ % 64u) == 0u)
         mesa_loge("wm: WMBOSYNC1 BoSync escape failed err=%d for %u op(s) - CPU "
                   "writes may not be visible to the GPU", err, count);
   } else {
      static unsigned s_ok = 0;
      if ((s_ok++ % 256u) == 0u)
         mesa_loge("wm: WMBOSYNC2 flushed %u BO range(s) for device access "
                   "(call #%u)", count, s_ok);
   }
   free(buf);
   return err;
}

static bool
winmali_kmod_bo_wait(struct pan_kmod_bo *bo, int64_t timeout_ns,
                     bool for_read_only_access)
{
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);
   uint64_t point = for_read_only_access
                    ? wbo->sync.write_point
                    : MAX2(wbo->sync.read_point, wbo->sync.write_point);
   if (point == 0)
      return true;
   int err = syncobj_timeline_wait(wbo->sync.handle, point, timeout_ns);
   return err == 0;
}

static void
winmali_kmod_bo_set_label(struct pan_kmod_dev *dev,
                          struct pan_kmod_bo *bo, const char *label)
{
   (void)dev;
   size_t len = strnlen(label, WINMALI_BO_LABEL_MAX - 1);
   size_t header = sizeof(WINMALI_ESCAPE_HEADER);
   size_t payload = sizeof(WINMALI_BO_SET_LABEL);
   size_t lab_off = WINMALI_ALIGN_UP(header + payload, WINMALI_TAIL_ALIGN);
   size_t total = WINMALI_ALIGN_UP(lab_off + len + 1, WINMALI_TAIL_ALIGN);

   /* Confirmed-unguarded sender of bo->handle. A synthetic id here would
      relabel an unrelated BoTable BO - harmless to pixels but it corrupts the
      diagnostics we rely on, which is worse than useless when chasing an
      aliasing bug. Labels are advisory, so just skip. */
   if (!to_winmali_bo(bo)->handle_is_kmd)
      return;

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   WINMALI_BO_SET_LABEL *args = (WINMALI_BO_SET_LABEL *)(buf + header);
   hdr->Magic = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor = WINMALI_ABI_MAJOR;
   hdr->AbiMinor = WINMALI_ABI_MINOR;
   hdr->Op = WinMaliEscapeOp_BoSetLabel;
   hdr->PayloadSize = (uint32_t)total;
   args->Handle = bo->handle;
   args->LabelLen = (uint32_t)len;
   args->LabelOffset = (uint32_t)lab_off;
   memcpy(buf + lab_off, label, len);
   buf[lab_off + len] = '\0';

   (void)winmali_d3dkmt_escape(buf, total);
   free(buf);
}

void *
winmali_kmod_bo_map_cpu(struct pan_kmod_bo *bo, int prot)
{
   if (bo->flags & PAN_KMOD_BO_FLAG_NO_MMAP) {
      /* "mapping src-texture failed" is emitted by util_resource_copy_region and
       * names neither the BO nor the reason. Three different refusals below all
       * surface as that one line, so say which one fired. Rate-limited: this is
       * on a per-frame path. */
      static unsigned s_nommap = 0;
      if ((s_nommap++ % 128) == 0)
         mesa_loge("wm: bo_map_cpu REFUSED bo=%u size=%llu - NO_MMAP flag (seen %u)",
                   bo->handle, (unsigned long long)bo->size, s_nommap);
      return MAP_FAILED;
   }

   /* dxgk-native BOs are already CPU-mapped for their lifetime via pfnLockCb
    * (captured at alloc). Hand back that persistent VA - no escape. */
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);
   if (wbo->wddm_cpu_va)
      return wbo->wddm_cpu_va;

   /* ⚠ A dxgk-backed BO with NO cached CPU VA is NOT CPU-mappable, and must
    * NOT fall through to the escape below.
    *
    * IMPORTED surfaces deliberately set wddm_cpu_va = NULL ("GPU-only: DWM
    * composites it, not the CPU"). The escape addresses BOs by bo->handle,
    * which is now a small dense id minted by winmali_alloc_bo_handle() and
    * meaningless to the KMD's escape BO table. Falling through therefore either
    * fails - returning MAP_FAILED, which is (void *)-1 and thus NON-NULL, so a
    * caller's `if (!ptr)` check passes - or, worse, resolves a DIFFERENT BO that
    * happens to own that small handle and hands back a valid-looking CPU pointer
    * into the wrong buffer.
    *
    * Either way the caller gets a non-NULL pointer it cannot safely write, which
    * is exactly the observed crash shape: faults on writes into descriptor/pool
    * memory that had already passed a NULL check (pan_cmdstream.h:277,
    * pan_texture.c:420). Fail cleanly and let the caller handle it. */
   if (wbo->wddm_kmt_handle) {
      /* An IMPORTED surface that did not adopt a CPU VA. Since the adoption fix
       * this should be rare - when it fires, the UMD never registered that kmt
       * (or the size guard rejected it), which is a different bug from NO_MMAP
       * and from an escape failure. Name it. */
      static unsigned s_nova = 0;
      if ((s_nova++ % 64) == 0)
         mesa_loge("wm: bo_map_cpu REFUSED bo=%u kmt=0x%x size=%llu - imported "
                   "with NO adopted CPU VA (seen %u)",
                   bo->handle, wbo->wddm_kmt_handle,
                   (unsigned long long)bo->size, s_nova);
      errno = ENXIO;
      return MAP_FAILED;
   }

   WINMALI_BO_MAP_CPU req = {
      .Handle = bo->handle,
      .Prot   = (prot & PROT_READ ? 1 : 0) | (prot & PROT_WRITE ? 2 : 0),
   };
   int err = escape_simple(WinMaliEscapeOp_BoMapCpu, &req, sizeof(req));
   if (err) {
      errno = -err;
      return MAP_FAILED;
   }

   /* CONTRACT CHECK, on the same call that produced the pointer.
    *
    * The pool repeatedly caught BOs where mesa believed 32768 bytes but only
    * 4096 (sometimes 16384) were mapped, and every attempt to decide which
    * side was wrong by correlating log lines failed: BO handles restart at 1
    * on each adapter restart and the ring wraps, so equal handle numbers are
    * NOT the same BO, and a missing "BoCreate size=32768" line proves nothing.
    *
    * MappedBytes comes back from the kernel on THIS call for THIS handle, so
    * the comparison needs no correlation at all. Whoever is wrong, the caller
    * must not receive a pointer it would write past the end of - that is a
    * heap corruptor, and it silently passes any `if (!ptr)` check. */
   if (req.MappedBytes < bo->size) {
      mesa_loge("winmali: BO %u SHORT MAPPING - mesa believes size=%llu "
                "but the kernel mapped only %llu bytes at %p. "
                "Refusing the pointer (writing it would corrupt the heap). "
                "This is one call, one handle: no cross-line correlation.",
                bo->handle, (unsigned long long)bo->size,
                (unsigned long long)req.MappedBytes,
                (void *)(uintptr_t)req.CpuAddr);
      errno = EFAULT;
      return MAP_FAILED;
   }

   return (void *)(uintptr_t)req.CpuAddr;
}

int
winmali_kmod_present_to_hdc(struct pan_kmod_dev *dev,
                            struct pan_kmod_bo *bo,
                            void *hdc,
                            uint32_t format,
                            uint32_t width, uint32_t height,
                            uint32_t stride,
                            uint32_t dst_x, uint32_t dst_y)
{
   (void)dev;

   /* Confirmed-unguarded sender of bo->handle, and the most damaging one: the
      KMD BLITS from the BO this names. With a synthetic id it blits whatever
      BoTable BO happens to own that number - silent wrong pixels on screen,
      indistinguishable from a rendering bug. Post-fix every import carries a
      KMD handle so this path becomes correct for them; refuse anything else. */
   if (!to_winmali_bo(bo)->handle_is_kmd) {
      mesa_loge("winmali: REFUSING PresentToHdc of non-KMD handle %u - the KMD "
                "would blit an unrelated BO", bo->handle);
      return -ENODEV;
   }

   WINMALI_PRESENT_TO_HDC req = {
      .Hdc      = (uint64_t)(uintptr_t)hdc,
      .BoHandle = bo->handle,
      .Format   = format,
      .Width    = width,
      .Height   = height,
      .Stride   = stride,
      .DstX     = dst_x,
      .DstY     = dst_y,
   };
   return escape_simple(WinMaliEscapeOp_PresentToHdc, &req, sizeof(req));
}

/*
 * BO handles MUST be small and dense.
 *
 * panfrost indexes DENSE ARRAYS by BO handle. The worst is
 * panfrost_batch_get_bo_access() (pan_job.c), which grows batch->bos to
 * `handle + 1` entries and memsets the new span:
 *
 *     if (handle >= size) { grow = handle + 1 - size;
 *        memset(util_dynarray_grow(&batch->bos, pan_bo_access, grow), 0, grow); }
 *
 * A D3DKMT allocation handle is ~0x40000000 (observed: 0x40000B40 =
 * 1,073,743,168), so using it as the BO handle turns that into a ~1 GiB
 * allocation which is zeroed on creation and then WALKED IN FULL several times
 * per batch (pan_job cleanup, csf_get_sync_points, csf_attach_sync_points).
 * util_dynarray doubles on growth (~2 GiB realloc) and there are up to
 * PAN_MAX_BATCHES of them.
 *
 * MEASURED CONSEQUENCE: one CPU pinned at 100%, commit exhausted on a 12 GB
 * device, no frames, and the machine off the network entirely - twice. It only
 * became reachable once OpenResource started returning real shared surfaces for
 * DWM to bind into a batch, but it is latent for EVERY BO whenever
 * WINMALI_DXGK_ADDR is on, because that path also used the KMT handle.
 *
 * The KMT handle is preserved in wbo->wddm_kmt_handle, which is what the paths
 * that actually need it use (winmali_vm_bind_dxgk -> map_va, and the
 * wddm_alloc.free on destroy). Nothing requires bo->handle to equal it.
 */
/* Import handles start ABOVE the KMD's escape-BO handle range.
 *
 * dev->bo_map is ONE per-process identity array (pan_lookup_bo), but two
 * disjoint namespaces index it: escape-created BOs carry KMD adapter-global
 * handles that start at 1 (WinMaliBo.c 'Adapter->BoTable.NextHandle++'), and
 * KMT imports carry these. A counter also starting at 1 therefore COLLIDES, and
 * the two kinds always coexist in-process: dev->tiler_heap and
 * dev->sample_positions are created at adapter open, BEFORE the wddm_alloc ops
 * are installed at device create, so they take the escape branch even with
 * WINMALI_DXGK_ADDR=1. A collision makes pan_lookup_bo hand two different BOs
 * the same slot - one silently clobbers the other.
 *
 * 0x10000 is chosen to stay small: batch->bos is a DENSE array indexed by
 * handle, so the base is also the floor on its size (64 KiB per batch, memset
 * once - negligible, and six orders of magnitude below the 1 GiB that a raw
 * D3DKMT handle produced). It would take 65536 concurrently live escape BOs to
 * reach it, and the import path additionally verifies the slot is free. */
/* ⚠⚠ BASE IS 1, NOT 0x10000 - AND RAISING IT REQUIRES ANOTHER FIX FIRST.
 *
 * The collision described above is real but LATENT. Raising the base to 0x10000
 * to avoid it was MEASURED to kill the GPU outright: create_context went 5/0 ->
 * 0/20 with 13x CSF TRANSLATION_FAULT_3 on VAs that have no VmMap/VmBind line
 * anywhere in the ring.
 *
 * WHY: winmali_vm_bind_dxgk() pre-scans a batch and returns -ENOTSUP if ANY op's
 * BO lacks a KMT handle, so ONE escape-backed BO (dev->tiler_heap and
 * dev->sample_positions are created at adapter open, before the wddm ops are
 * installed) forces the WHOLE batch onto the escape VmBind. Escape VmBind
 * addresses BOs by bo->handle, which for a dxgk-backed BO is this synthetic id
 * and means nothing to the KMD's BoTable. With base 1 the synthetic id COLLIDES
 * with a real KMD handle, so the escape maps the WRONG BO - wrong data, but
 * something is mapped and the GPU runs. With base 0x10000 nothing maps at all
 * and the GPU faults immediately.
 *
 * So: base 1 restores function; the collision remains a latent hazard. The real
 * fix is to stop mixed batches falling back wholesale - route each op
 * individually (dxgk-backed -> map_va, escape-backed -> escape VmBind), or
 * install the wddm ops before the screen's own BOs are created so every BO is
 * dxgk-backed. Raise this base ONLY together with that change. */
#define WINMALI_IMPORT_HANDLE_BASE 0u

uint32_t
winmali_kmod_alloc_bo_handle(void)
{
   static uint32_t s_next_bo_handle = WINMALI_IMPORT_HANDLE_BASE;
   return (uint32_t)p_atomic_inc_return(&s_next_bo_handle);
}

static uint32_t
winmali_alloc_bo_handle(void)
{
   return winmali_kmod_alloc_bo_handle();
}

/*
 * ...but a dense handle is not enough on its own: it must also be STABLE per
 * dxgk allocation.
 *
 * pan_lookup_bo(dev, handle) is a per-device table keyed by BO handle, and
 * panfrost_resource_from_winmali_kmt() depends on it to DEDUP:
 *
 *     bo = pan_lookup_bo(dev, pan_kmod_bo_handle(kmod_bo));
 *     if (!bo->dev) { ...first import, initialise... }
 *     else          { pan_kmod_bo_put(kmod_bo); panfrost_bo_reference(bo); }
 *
 * The KMT handle used to provide that identity ("the KMT handle is the BO
 * identity"). Handing out a fresh counter value per import broke it: the same
 * D3DKMT allocation became several panfrost BOs, each mapping it into the VM
 * separately, which corrupted state and crashed LogonUI.exe at five unrelated
 * points in the render path (panfrost_emit_resources, panfrost_bo_cache_fetch,
 * emit_generic_plane, panfrost_emit_depth_stencil, panfrost_batch_add_bo_old).
 * Note pan_bo.c:72 asserts the looked-up slot is zeroed on first use - and
 * asserts are compiled out here, so a collision or a duplicate is SILENT.
 *
 * So: map KMT handle -> dense handle, and return the same dense handle for a
 * repeat import. Linear scan under a mutex is fine; this holds live shared
 * surfaces only (a handful), and entries are dropped when the BO is destroyed.
 */
static struct {
   simple_mtx_t lock;
   uint32_t      count;
   uint32_t      cap;
   struct { uint32_t kmt; uint32_t handle; } *ent;
} g_kmt_ids = { SIMPLE_MTX_INITIALIZER, 0, 0, NULL };

/* ★ kmt -> AllocId map, for escape op 0x92 (BoFromAllocId).
 *
 * The AllocId lives in the allocation's 'WMAl' private blob, which only the D3D
 * frontend sees; the kmod layer is handed just a KMT handle at import time. The
 * frontend registers the pairing right after pfnAllocateCb returns, so the
 * import can wrap the allocation's REAL pages instead of falling back to
 * fresh-page BoCreate (which is the documented cause of the black desktop).
 *
 * Small and insert-only, like g_kmt_ids: compositor surfaces only. */
static struct {
   simple_mtx_t lock;
   uint32_t      count;
   uint32_t      cap;
   struct { uint32_t kmt; uint32_t id; uint32_t id_hi;
            void *cpu; uint64_t size; uint32_t pitch; } *ent;
} g_kmt_alloc_ids = { SIMPLE_MTX_INITIALIZER, 0, 0, NULL };

/* ★ The locked CPU VA of a dxgk allocation, from pfnLockCb(LockEntire).
 *
 * THIS is how to wrap an allocation's REAL pages WITHOUT dxgk paging: the pages
 * are already resident and mapped into this process, so the KMD can build an MDL
 * straight from the user VA (escape 0x93 BoFromUserVa). No MapApertureSegment,
 * no ApertureMdl, no DxgkCbGetHandleData - none of which are available under
 * escape submission, because dxgk never learns the surface is needed and so
 * never pages it in (measured: 0x92 returns -EAGAIN forever, even on re-import). */
static void
winmali_record_alloc_cpu(uint32_t kmt, void *cpu, uint64_t size, uint32_t pitch)
{
   if (kmt == 0 || cpu == NULL || size == 0)
      return;

   simple_mtx_lock(&g_kmt_alloc_ids.lock);
   for (uint32_t i = 0; i < g_kmt_alloc_ids.count; i++) {
      if (g_kmt_alloc_ids.ent[i].kmt == kmt) {
         g_kmt_alloc_ids.ent[i].cpu   = cpu;
         g_kmt_alloc_ids.ent[i].size  = size;
         if (pitch) g_kmt_alloc_ids.ent[i].pitch = pitch;
         simple_mtx_unlock(&g_kmt_alloc_ids.lock);
         return;
      }
   }
   if (g_kmt_alloc_ids.count == g_kmt_alloc_ids.cap) {
      uint32_t ncap = g_kmt_alloc_ids.cap ? g_kmt_alloc_ids.cap * 2 : 32;
      void *n = realloc(g_kmt_alloc_ids.ent, ncap * sizeof(*g_kmt_alloc_ids.ent));
      if (!n) { simple_mtx_unlock(&g_kmt_alloc_ids.lock); return; }
      g_kmt_alloc_ids.ent = n;
      g_kmt_alloc_ids.cap = ncap;
   }
   memset(&g_kmt_alloc_ids.ent[g_kmt_alloc_ids.count], 0,
          sizeof(*g_kmt_alloc_ids.ent));
   g_kmt_alloc_ids.ent[g_kmt_alloc_ids.count].kmt  = kmt;
   g_kmt_alloc_ids.ent[g_kmt_alloc_ids.count].cpu  = cpu;
   g_kmt_alloc_ids.ent[g_kmt_alloc_ids.count].size = size;
   g_kmt_alloc_ids.ent[g_kmt_alloc_ids.count].pitch = pitch;
   g_kmt_alloc_ids.count++;
   simple_mtx_unlock(&g_kmt_alloc_ids.lock);
}

void
winmali_kmod_register_alloc_cpu(uint32_t kmt, void *cpu, uint64_t size,
                                uint32_t pitch)
{
   winmali_record_alloc_cpu(kmt, cpu, size, pitch);
}

static void
winmali_alloc_cpu_for_kmt(uint32_t kmt, void **cpu, uint64_t *size,
                          uint32_t *pitch)
{
   *cpu = NULL; *size = 0; if (pitch) *pitch = 0;
   simple_mtx_lock(&g_kmt_alloc_ids.lock);
   for (uint32_t i = 0; i < g_kmt_alloc_ids.count; i++) {
      if (g_kmt_alloc_ids.ent[i].kmt == kmt) {
         *cpu  = g_kmt_alloc_ids.ent[i].cpu;
         *size = g_kmt_alloc_ids.ent[i].size;
         if (pitch) *pitch = g_kmt_alloc_ids.ent[i].pitch;
         break;
      }
   }
   simple_mtx_unlock(&g_kmt_alloc_ids.lock);
}

void
winmali_kmod_register_alloc_id(uint32_t kmt, uint32_t id, uint32_t id_hi)
{
   if (kmt == 0 || (id == 0 && id_hi == 0))
      return;

   simple_mtx_lock(&g_kmt_alloc_ids.lock);
   for (uint32_t i = 0; i < g_kmt_alloc_ids.count; i++) {
      if (g_kmt_alloc_ids.ent[i].kmt == kmt) {
         g_kmt_alloc_ids.ent[i].id    = id;
         g_kmt_alloc_ids.ent[i].id_hi = id_hi;
         simple_mtx_unlock(&g_kmt_alloc_ids.lock);
         return;
      }
   }
   if (g_kmt_alloc_ids.count == g_kmt_alloc_ids.cap) {
      uint32_t ncap = g_kmt_alloc_ids.cap ? g_kmt_alloc_ids.cap * 2 : 32;
      void *n = realloc(g_kmt_alloc_ids.ent, ncap * sizeof(*g_kmt_alloc_ids.ent));
      if (!n) {
         simple_mtx_unlock(&g_kmt_alloc_ids.lock);
         return;                    /* degrades to the fresh-page path */
      }
      g_kmt_alloc_ids.ent = n;
      g_kmt_alloc_ids.cap = ncap;
   }
   g_kmt_alloc_ids.ent[g_kmt_alloc_ids.count].kmt   = kmt;
   g_kmt_alloc_ids.ent[g_kmt_alloc_ids.count].id    = id;
   g_kmt_alloc_ids.ent[g_kmt_alloc_ids.count].id_hi = id_hi;
   g_kmt_alloc_ids.count++;
   simple_mtx_unlock(&g_kmt_alloc_ids.lock);
}

static void
winmali_alloc_id_for_kmt(uint32_t kmt, uint32_t *id, uint32_t *id_hi)
{
   *id = 0; *id_hi = 0;
   simple_mtx_lock(&g_kmt_alloc_ids.lock);
   for (uint32_t i = 0; i < g_kmt_alloc_ids.count; i++) {
      if (g_kmt_alloc_ids.ent[i].kmt == kmt) {
         *id    = g_kmt_alloc_ids.ent[i].id;
         *id_hi = g_kmt_alloc_ids.ent[i].id_hi;
         break;
      }
   }
   simple_mtx_unlock(&g_kmt_alloc_ids.lock);
}

/* Record kmt -> handle. INSERT-ONLY: this map no longer MINTS anything.
 *
 * It used to allocate the handle itself, from a counter that also started at 1
 * and therefore collided with the KMD's BoTable (the measured black-desktop
 * bug). Identity now comes from the KMD in winmali_kmod_bo_from_allocation and
 * is merely recorded here, so the map's only remaining job is DEDUP: the
 * peek-first fast path in panfrost_resource_from_winmali_kmt depends on it,
 * because a repeat import would otherwise build a second BO and a second GPU VA
 * for the same surface.
 *
 * Failure to record therefore degrades to a wasteful duplicate, never to
 * corruption - strictly better than the old OOM path, which minted a fresh
 * synthetic id and could alias somebody else's BO. */
static void
winmali_record_kmt(uint32_t kmt, uint32_t handle)
{
   if (kmt == 0 || handle == 0)
      return;

   simple_mtx_lock(&g_kmt_ids.lock);
   for (uint32_t i = 0; i < g_kmt_ids.count; i++) {
      if (g_kmt_ids.ent[i].kmt == kmt) {
         /* Re-import of a KMT we already know: keep the first identity so the
            dedup fast path stays coherent. */
         simple_mtx_unlock(&g_kmt_ids.lock);
         return;
      }
   }
   if (g_kmt_ids.count == g_kmt_ids.cap) {
      uint32_t ncap = g_kmt_ids.cap ? g_kmt_ids.cap * 2 : 32;
      void *n = realloc(g_kmt_ids.ent, ncap * sizeof(*g_kmt_ids.ent));
      if (!n) {
         simple_mtx_unlock(&g_kmt_ids.lock);
         mesa_loge("winmali: kmt id map OOM - dedup lost for kmt=0x%x "
                   "(a repeat import will build a duplicate BO; not fatal)",
                   kmt);
         return;
      }
      g_kmt_ids.ent = n;
      g_kmt_ids.cap = ncap;
   }
   g_kmt_ids.ent[g_kmt_ids.count].kmt    = kmt;
   g_kmt_ids.ent[g_kmt_ids.count].handle = handle;
   g_kmt_ids.count++;
   simple_mtx_unlock(&g_kmt_ids.lock);
}

/* Look up WITHOUT allocating. The import path calls this first so that a repeat
 * import of a live surface can reuse the existing panfrost BO directly, instead
 * of minting a throwaway pan_kmod_bo wrapper and immediately pan_kmod_bo_put()
 * ing it. That put reached winmali_kmod_bo_free -> winmali_forget_kmt() and
 * DELETED the identity entry of a BO that was still alive, so the next import
 * allocated a fresh handle, found an empty slot, and vm_bind'd a SECOND GPU VA
 * for the same dxgk allocation - two panfrost BOs aliasing one allocation with
 * independent refcounts. Deterministic on every second import of a live
 * surface. With the peek, no throwaway wrapper is created and forget_kmt only
 * ever runs for the canonical BO. */
uint32_t
winmali_kmod_peek_handle_for_kmt(uint32_t kmt)
{
   uint32_t h = 0;

   if (kmt == 0)
      return 0;

   simple_mtx_lock(&g_kmt_ids.lock);
   for (uint32_t i = 0; i < g_kmt_ids.count; i++) {
      if (g_kmt_ids.ent[i].kmt == kmt) {
         h = g_kmt_ids.ent[i].handle;
         break;
      }
   }
   simple_mtx_unlock(&g_kmt_ids.lock);
   return h;
}

uint32_t
winmali_kmod_bo_kmt_handle(const struct pan_kmod_bo *bo)
{
   if (bo == NULL)
      return 0;
   return to_winmali_bo((struct pan_kmod_bo *)bo)->wddm_kmt_handle;
}

bool
winmali_kmod_bo_handle_is_kmd(const struct pan_kmod_bo *bo)
{
   if (bo == NULL)
      return false;
   return to_winmali_bo((struct pan_kmod_bo *)bo)->handle_is_kmd;
}

static void
winmali_forget_kmt(uint32_t kmt)
{
   if (kmt == 0)
      return;
   simple_mtx_lock(&g_kmt_ids.lock);
   for (uint32_t i = 0; i < g_kmt_ids.count; i++) {
      if (g_kmt_ids.ent[i].kmt == kmt) {
         g_kmt_ids.ent[i] = g_kmt_ids.ent[g_kmt_ids.count - 1];
         g_kmt_ids.count--;
         break;
      }
   }
   simple_mtx_unlock(&g_kmt_ids.lock);
}

struct pan_kmod_bo *
winmali_kmod_bo_from_allocation(struct pan_kmod_dev *dev,
                                uint32_t kmt_allocation_handle,
                                uint64_t size,
                                uint32_t flags)
{
   struct winmali_kmod_bo *wbo = pan_kmod_dev_alloc(dev, sizeof(*wbo));
   if (!wbo) {
      errno = ENOMEM;
      return NULL;
   }

   /* The allocation is an existing dxgk allocation (e.g. a DXGI swap-chain
    * backbuffer created by the D3D runtime). Address it via GpuMmu (vm_bind ->
    * pfnMapGpuVirtualAddressCb) - NO BoFromAllocation escape, so no cross-device
    * pfnEscapeCb round-trip (which crashed d3d11's escape telemetry) and no
    * page-alias handshake. The KMT handle is kept in wbo->wddm_kmt_handle; it is
    * deliberately NOT the BO handle (see winmali_alloc_bo_handle). */
   uint32_t alloc_id = 0, alloc_id_hi = 0;
   winmali_alloc_id_for_kmt(kmt_allocation_handle, &alloc_id, &alloc_id_hi);

   /* ⚠ UNCONDITIONAL, BEFORE ANY BRANCH. Measured 2026-08-15: op 0x92 never
    * fired AND its fallback log never fired either, because both sit inside
    * `if (alloc_id || alloc_id_hi)` and the lookup returned 0:0. With no line
    * outside that branch it was impossible to distinguish "the import was never
    * called" from "it was called but no AllocId was known". This separates them
    * in a single boot.
    *
    * Expected finding: the pairing is registered from CreateResource, i.e. for
    * allocations OUR UMD creates - but this import runs for surfaces the D3D
    * RUNTIME created (DXGI backbuffers), which never pass through pfnAllocateCb
    * here. If confirmed, register where the 'WMAl' blob is READ (OpenResource)
    * rather than where it is minted. */
   mesa_loge("wm: bo_from_allocation kmt=0x%x size=%llu -> allocId=%u:%u",
                      kmt_allocation_handle, (unsigned long long)size,
                      alloc_id_hi, alloc_id);

   uint32_t h = 0;
   int serr = syncobj_create_timeline(&h);
   if (serr) {
      pan_kmod_dev_free(dev, wbo);
      errno = -serr;
      return NULL;
   }
   wbo->sync.handle = h;
   wbo->sync.owns_handle = true;    /* we minted it, so we free it */
   wbo->sync.owned_handle = h;      /* remember WHICH one, sync.handle can be replaced */
   wbo->sync.read_point  = 0;
   wbo->sync.write_point = 0;
   wbo->wddm_kmt_handle  = kmt_allocation_handle;

   /* ⛔ THIS USED TO BE A HARD `NULL` WITH THE COMMENT "GPU-only: DWM composites
    * it, not the CPU" - AND THAT ASSUMPTION IS NOW THE THING BLOCKING PIXELS.
    *
    * Measured 2026-08-16, clean boot, real traffic: the GPU completes every
    * submit (CsSubmits 886 / CsOk 886 / CsTimeouts 0) and the surface the present
    * worker scans holds a full desktop (StatSrcNonBlack 646/646, 639 edges) - but
    * its change counter is FLAT. Rendering lands in panfrost's own GPU BOs;
    * delivery into this imported primary is a CPU copy
    * (panfrost_resource_copy_region), and that copy dies here: with
    * wddm_cpu_va == NULL, winmali_kmod_bo_map_cpu() refuses every KMT-imported
    * destination (it must - the escape addresses BOs by a handle meaningless for
    * an imported surface), so `mapping dst-texture failed` and the frame is
    * dropped. The scanned pages therefore keep whatever was in them at logon.
    *
    * The premise no longer holds: since escape 0x93 the UMD locks these surfaces
    * and registers the resulting user VA, and the KMD wraps THOSE EXACT PAGES as
    * the scanout source. So the CPU VA is both known and precisely the memory the
    * panel reads. Hand it back.
    *
    * ⚠ Only when the registered extent covers the whole BO. A short mapping here
    * is a heap corruptor that passes every `if (!ptr)` check - the same trap
    * bo_map_cpu already guards with its MappedBytes contract check. */
   {
      void *reg_cpu = NULL;
      uint64_t reg_size = 0;
      winmali_alloc_cpu_for_kmt(kmt_allocation_handle, &reg_cpu, &reg_size, NULL);
      /* ⛔★★★★★ COMPARE PAGE-GRANULAR, NOT BYTE-EXACT.
       *
       * `size` is the BO size, which the allocator PAGE-ROUNDS. `reg_size` is the
       * registered extent, which is the real pixel data (stride * height). For a
       * surface whose data is not a whole number of pages these differ by up to
       * one page, and a byte-exact `reg_size >= size` then rejects a surface
       * every byte of which IS registered.
       *
       * MEASURED 2026-08-17, dwm's own log:
       *   kmt=0xc0005740 NO usable CPU VA (reg_size=1483776 bo_size=1486848)
       *   OpenResource: OPENED shared surface 2944x126 stride=11776
       *   126 * 11776 == 1483776 exactly; the BO is 3072 B (< 1 page) larger.
       * That surface is 2944x126 - a full-width 126px strip, i.e. THE TASKBAR,
       * the one element that never appears on the panel. Every other import
       * adopted its VA and only this one was refused.
       *
       * Rounding UP is safe here specifically because the VA comes from a
       * probe-locked MDL and MDLs lock WHOLE PAGES: the bytes between reg_size
       * and the page boundary are really mapped, so a copy that runs to the end
       * of the final page cannot leave the locked region. This is NOT the "short
       * mapping" hazard the comment above warns about - that is a mapping which
       * ends before the real data does, which this still refuses. */
      const uint64_t reg_pages = (reg_size + 4095ull) & ~4095ull;
      if (reg_cpu != NULL && reg_pages >= size) {
         wbo->wddm_cpu_va = reg_cpu;
         mesa_loge("wm: imported kmt=0x%x ADOPTS registered CPU VA %p (%llu >= %llu)"
                   " - CPU delivery into the scanout source is now possible",
                   kmt_allocation_handle, reg_cpu,
                   (unsigned long long)reg_size, (unsigned long long)size);
      } else {
         wbo->wddm_cpu_va = NULL;
         mesa_loge("wm: imported kmt=0x%x has NO usable CPU VA (cpu=%p reg_size=%llu"
                   " bo_size=%llu) - CPU delivery will be refused for it",
                   kmt_allocation_handle, reg_cpu,
                   (unsigned long long)reg_size, (unsigned long long)size);
      }
   }

   /* ★★★ THE COLLISION FIX ★★★
    *
    * This used to call winmali_handle_for_kmt(), a SECOND handle minter that
    * also starts at 1 - while the KMD's BoTable mints 1,2,3... for escape BOs.
    * Both wrote pan_kmod_bo.handle, panfrost indexes util_sparse_array
    * handle_to_bo and pan_lookup_bo BY HANDLE, and mesa is built -DNDEBUG so
    * the assert(*slot == NULL) that guards this is compiled out. The second BO
    * silently overwrote the first.
    *
    * MEASURED (2026-08-15), matching by pan_kmod_bo ADDRESS, not handle number:
    *     bo_alloc obj=...4A1A7180 handle=23 asked=4096 kernel=4096  <- escape
    *     pool     obj=...4AF6AB90 handle=23 claims size=32768       <- import
    * Same handle, two objects. Downstream: the pool refuses the pointer,
    * descriptor allocation fails, nothing renders (black desktop), and callers
    * that skip the NULL check take c0000005 in LogonUI/Explorer.
    *
    * Fix: take the identity from the KMD's single boot-unique counter, by
    * creating a real escape BO to back this import. Every handle in the default
    * config is then KMD-issued and the second namespace is gone by construction.
    *
    * ⛔ WHY NOT JUST RENUMBER THE SYNTHETIC COUNTER: that is refuted approach
    * (A), measured to take create_context from 5/0 to 0/20 with 13x
    * TRANSLATION_FAULT_3. Escape VmBind addresses BOs by bo->handle, so a
    * number the BoTable never issued fails the batch with WMERR_ENOENT
    * ("VmBind MAP: BoHandle %u unknown") and EVERY presentable surface stops
    * existing. Disjointness is only safe when the import's MAP still resolves
    * to real pages in vm->Pt - which is what this BO provides.
    *
    * ⚠ KNOWN LIMITATION, deliberately accepted for this step: these are FRESH
    * pages, not the dxgk allocation's own pages, so the GPU's output does not
    * land in the surface the present worker copies from (it reads the real
    * allocation's ApertureMdl). This kills the corruption and the crashes but
    * not the black content. Putting pixels in the right pages requires
    * KMD-issued identity OF THE REAL PAGES - escape op 0x81
    * (WinMaliEscapeBoFromAllocation_, which wraps ka->ApertureMdl) - which is
    * the next step and slots into this exact call site. It is deliberately NOT
    * bundled here: 0x81 must be routed through the device that created the
    * allocation, and a cross-device pfnEscapeCb round-trip previously crashed
    * d3d11's escape telemetry. One variable at a time. */
   {
      /* ★★★ PREFER THE REAL PAGES (escape op 0x81, BoFromAllocation).
       *
       * The KNOWN LIMITATION described above - "these are FRESH pages, not the
       * dxgk allocation's own pages, so the GPU's output does not land in the
       * surface the present worker copies from ... this kills the corruption
       * and the crashes but NOT the black content" - is exactly this call site,
       * and 0x81 is the documented next step.
       *
       * The blocker was routing: 0x81 resolves its handle with
       * DxgkCbGetHandleData on pEscape->hDevice, and escape_simple() sends on
       * winmali's PRIVATE device, which never created the runtime's surfaces.
       * The frontend now services escapes on the device that DID create them
       * (g_wddm_ops.escape), so the handle resolves and the BO wraps
       * ka->ApertureMdl - the allocation's OWN pages.
       *
       * Fall back to the fresh-page BoCreate when no frontend bridge is
       * installed (GL/VK paths) or the KMD refuses: identity still comes from
       * the KMD's single counter either way, so the handle-collision fix above
       * is preserved. Black content is better than no content. */
      WINMALI_BO_CREATE req = {
         .Size          = size,
         .Flags         = flags & 0xff,
         .ExclusiveVmId = 0,
      };
      int cerr = -ENOSYS;
      bool used_real_pages = false;

      /* ⛔⛔ REFUTED APPROACH - OFF BY DEFAULT. Set WINMALI_IMPORT_0X81=1 to try.
       *
       * MEASURED 2026-08-15, two clean reboots (deploy-boot artifact excluded):
       *     LogonUI.exe <- d3d11.dll @ 0x8f240   x64 faults
       *     realPages=0/0  fellBack=0/0  (neither log line ever reached)
       *     faults 53/55 vs a 1-3 noise floor
       * The crash is INSIDE d3d11.dll during pfnEscapeCb, so the process dies
       * before either log line after the call - which is why the counters look
       * like "the code never ran".
       *
       * The comment above already warned that "a cross-device pfnEscapeCb
       * round-trip previously crashed d3d11's escape telemetry". I read that as
       * "the blocker is ROUTING" and assumed issuing the escape on the correct
       * device would remove the crash. It does not: d3d11's escape handling
       * itself is what breaks, whichever device is targeted.
       *
       * ⇒ Getting the allocation's REAL pages needs a route that is not a
       * UMD-side escape at all - e.g. have the KMD wrap ka->ApertureMdl during
       * a DDI it already services (OpenAllocation / CreateAllocation), and hand
       * the BO handle back through the 'WMAl' private blob the UMD already
       * reads. No pfnEscapeCb, no cross-device round-trip. */
      /* ★★★ PREFERRED: op 0x92 BoFromAllocId - the allocation's REAL pages,
       * with NO device dependency and NO pfnEscapeCb.
       *
       * This is what finally lets the GPU render into the surface the present
       * worker scans out. The fresh-page BoCreate below leaves the two in
       * different memory, which is the documented cause of the black desktop.
       *
       * AllocId comes from the 'WMAl' blob the UMD already stamps at
       * CreateResource, and the KMD resolves it adapter-globally via
       * WinMaliAllocLookupById (the same identity that took OpenAllocation from
       * 0/195 to 252/252). No DxgkCbGetHandleData, so winmali's own private
       * device is fine - which is exactly why this succeeds where 0x81 could
       * not. */
      /* 🎯🎯 FIRST CHOICE: op 0x93 BoFromUserVa - the allocation's REAL pages,
       * reached through the locked CPU VA instead of through dxgk paging.
       *
       * ⛔ WHY 0x92 IS NOT ENOUGH, measured over many clean boots:
       *     Stat0x92Calls>0, Stat0x92NotRes = ALL of them, realPages=0, nonBlack=0
       * i.e. the AllocId resolves fine, but `ka->ApertureMdl` is NULL every single
       * time. Only MapApertureSegment fills it, and dxgk runs that only for
       * allocations it believes the GPU needs - which it learns from a DMA
       * buffer's allocation list or MapGpuVirtualAddressCb. Under escape
       * submission it sees neither, so it NEVER pages these surfaces in. The
       * "retry on re-import" theory assumed the mapping would show up later; it
       * does not, on any of DWM's ~19 re-imports per boot.
       *
       * pfnLockCb(LockEntire) has ALREADY made the pages resident and mapped them
       * into this process - that CPU pointer is the same frames the present
       * worker scans out. The KMD can probe-and-lock that user range and wrap its
       * PFNs with no paging operation at all. */
      void    *lock_cpu  = NULL;
      uint64_t lock_size = 0;
      uint32_t lock_pitch = 0;
      winmali_alloc_cpu_for_kmt(kmt_allocation_handle, &lock_cpu, &lock_size,
                                &lock_pitch);
      if (lock_cpu != NULL && lock_size != 0) {
         WINMALI_BO_FROM_USER_VA fv;
         memset(&fv, 0, sizeof fv);
         fv.UserVa = (uint64_t)(uintptr_t)lock_cpu;
         fv.Size   = lock_size;
         fv.Flags  = flags & 0xff;
         /* ★ TELL THE KERNEL THE SHAPE. Without this the present worker copies
            every candidate at the panel pitch, which turns any surface with a
            different stride into a squashed partial frame. */
         fv.SrcPitch = lock_pitch;

         int verr = escape_simple(WinMaliEscapeOp_BoFromUserVa, &fv, sizeof fv);
         if (verr == 0 && fv.BoHandle != 0) {
            req.Handle = fv.BoHandle;
            req.Size   = fv.MappedBytes;   /* what the kernel actually locked */
            cerr = 0;
            used_real_pages = true;
            mesa_loge("wm: import kmt=0x%x va=%p -> bo=%u size=%llu "
                      "(REAL PAGES via 0x93) pitch=%u",
                      kmt_allocation_handle, lock_cpu, fv.BoHandle,
                      (unsigned long long)fv.MappedBytes, lock_pitch);
         } else {
            mesa_loge("wm: 0x93 BoFromUserVa va=%p size=%llu failed (%d)",
                      lock_cpu, (unsigned long long)lock_size, verr);
         }
      } else {
         mesa_loge("wm: no locked CPU VA recorded for kmt=0x%x - 0x93 skipped",
                   kmt_allocation_handle);
      }

      if (cerr != 0 && (alloc_id != 0 || alloc_id_hi != 0)) {
         WINMALI_BO_FROM_ALLOC_ID fa;
         memset(&fa, 0, sizeof fa);
         fa.AllocId   = alloc_id;
         fa.AllocIdHi = alloc_id_hi;
         fa.Flags     = flags & 0xff;

         int ferr = escape_simple(WinMaliEscapeOp_BoFromAllocId, &fa, sizeof fa);
         if (ferr == 0 && fa.BoHandle != 0) {
            req.Handle = fa.BoHandle;
            req.Size   = fa.Size;   /* the REAL mapped extent */
            cerr = 0;
            used_real_pages = true;
            mesa_loge("wm: import kmt=0x%x id=%u:%u -> bo=%u size=%llu "
                               "(REAL PAGES via 0x92)",
                               kmt_allocation_handle, alloc_id_hi, alloc_id,
                               fa.BoHandle, (unsigned long long)fa.Size);
         } else {
            mesa_loge("wm: 0x92 BoFromAllocId id=%u:%u failed (%d) - "
                               "falling back to fresh-page BoCreate",
                               alloc_id_hi, alloc_id, ferr);
         }
      }

      if (cerr != 0 && g_wddm_ops.escape != NULL &&
          getenv("WINMALI_IMPORT_0X81") != NULL) {
         WINMALI_BO_FROM_ALLOCATION fa;
         memset(&fa, 0, sizeof fa);
         fa.AllocationHandle = kmt_allocation_handle;
         fa.Flags            = flags & 0xff;
         fa.VmId             = 0;   /* caller maps via VmBind */

         int ferr = escape_on_runtime_device(WinMaliEscapeOp_BoFromAllocation,
                                             &fa, sizeof fa);
         if (ferr == 0 && fa.BoHandle != 0) {
            req.Handle = fa.BoHandle;
            req.Size   = fa.Size;    /* the REAL mapped extent, not our guess */
            cerr = 0;
            winmali_d3dkmt_log("wm: import kmt=0x%x -> bo=%u size=%llu "
                               "(REAL PAGES via 0x81)",
                               kmt_allocation_handle, fa.BoHandle,
                               (unsigned long long)fa.Size);
         } else {
            winmali_d3dkmt_log("wm: 0x81 BoFromAllocation kmt=0x%x failed (%d) "
                               "- falling back to fresh-page BoCreate",
                               kmt_allocation_handle, ferr);
         }
      }

      if (cerr != 0)
         cerr = escape_simple(WinMaliEscapeOp_BoCreate, &req, sizeof(req));
      if (cerr) {
         syncobj_destroy(h);
         pan_kmod_dev_free(dev, wbo);
         errno = -cerr;
         return NULL;
      }

      /* Size is IN/OUT - record what the kernel actually made, never the
         request. Recording the request is how mesa came to believe a BO was
         32768 bytes when the kernel had made 4096. */
      if (req.Size != 0)
         size = req.Size;

      wbo->handle_is_kmd = true;

      /* ★ DO NOT CACHE A PROVISIONAL (FRESH-PAGE) IMPORT.
       *
       * `winmali_record_kmt` is what makes a later import of the same KMT reuse
       * this BO. Right for a REAL-pages import - but caching a fresh-page
       * FALLBACK freezes the surface in the wrong memory for the rest of the
       * boot, so the GPU renders into pages the present worker never reads.
       *
       * ⛔ The original reason given here - "0x92 fails only with -EAGAIN, so a
       * later re-import will succeed once dxgk has run the paging op" - is
       * REFUTED. Under escape submission dxgk never runs that paging op at all,
       * so every one of DWM's ~19 re-imports per boot fails identically. The
       * retry is still worth keeping, but now because it gives 0x93 another
       * chance at a surface whose lock had not yet been recorded.
       *
       * Cost: a duplicate fresh-page BO per retry, bounded by the import count.
       * Harmless for identity - the handle still comes from the KMD's single
       * boot-unique counter, so this cannot resurrect the two-minter collision
       * fixed above. */
      if (used_real_pages) {
         winmali_record_kmt(kmt_allocation_handle, req.Handle);
      } else {
         mesa_loge("wm: PROVISIONAL import kmt=0x%x bo=%u (fresh pages) - NOT "
                   "caching, so a later import retries the real-pages path",
                   kmt_allocation_handle, req.Handle);
      }

      winmali_d3dkmt_log("wm: import obj=%p handle=%u kmt=0x%x size=%llu "
                         "(KMD-issued identity)",
                         (void *)wbo, req.Handle,
                         kmt_allocation_handle, (unsigned long long)size);

      /* Mark the BO as imported so callers don't try to re-export it, and so
         bo_free never releases the dxgk allocation - it belongs to the D3D
         runtime / DWM. The KMT handle lives in wbo->wddm_kmt_handle. */
      pan_kmod_bo_init(&wbo->base, dev, NULL, size,
                       flags | PAN_KMOD_BO_FLAG_IMPORTED,
                       req.Handle);
   }
   return &wbo->base;
}

/* --------------------------------------------------------------------- */
/*  Virtual memory                                                       */
/* --------------------------------------------------------------------- */

static struct pan_kmod_vm *
winmali_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                       uint64_t va_start, uint64_t va_range)
{
   struct winmali_kmod_vm *wvm = pan_kmod_dev_alloc(dev, sizeof(*wvm));
   if (!wvm) {
      errno = ENOMEM;
      return NULL;
   }

   WINMALI_VM_CREATE req = {
      .Flags = 0,
      .UserVaRange = va_range,
   };
   int err = escape_simple(WinMaliEscapeOp_VmCreate, &req, sizeof(req));
   if (err) {
      pan_kmod_dev_free(dev, wvm);
      errno = -err;
      return NULL;
   }

   wvm->vm_id = req.Id;

   if (flags & PAN_KMOD_VM_FLAG_AUTO_VA) {
      simple_mtx_init(&wvm->auto_va.lock, mtx_plain);
      util_vma_heap_init(&wvm->auto_va.heap, va_start, va_range);
      list_inithead(&wvm->auto_va.gc_list);
   }

   if (flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY) {
      uint32_t h = 0;
      int serr = syncobj_create_timeline(&h);
      if (serr) {
         if (flags & PAN_KMOD_VM_FLAG_AUTO_VA) {
            util_vma_heap_finish(&wvm->auto_va.heap);
            simple_mtx_destroy(&wvm->auto_va.lock);
         }
         WINMALI_VM_DESTROY d = { .Id = req.Id };
         escape_simple(WinMaliEscapeOp_VmDestroy, &d, sizeof(d));
         pan_kmod_dev_free(dev, wvm);
         errno = -serr;
         return NULL;
      }
      wvm->sync.handle = h;
      winmali_vm_sync_register(h);   /* protect it from any stray syncobj_destroy */
      wvm->sync.point  = 0;
      simple_mtx_init(&wvm->sync.lock, mtx_plain);
      mesa_loge("wm: WMVMSYNC1 vm_create id=%u TRACK_ACTIVITY -> sync handle=%u "
                "(flags=0x%x)", req.Id, h, flags);
   } else {
      /* ⛔★★★★★ NO TRACK_ACTIVITY => sync.handle IS NEVER ASSIGNED HERE.
       *
       * But csf_submit signals it unconditionally (pan_csf.c: `.handle =
       * vm_sync_handle, .timeline_value = cur_point + 1`), and the guard in
       * panthor_kmod_vm_sync_handle() is an assert() - COMPILED OUT in release.
       * So a VM created without this flag hands the submit path whatever
       * sync.handle happens to contain.
       *
       * That matches the measured failure precisely: dwm signals `handle=1` at
       * `point=10`; the KMD reports it NOT IN TABLE with 39-41 live objects; it
       * is never created OR destroyed in a 2300-line capture; and the KMD table
       * is initialised ONCE per boot and never torn down (SyncTblInits=1,
       * SyncTblTears=0) - so nothing removed it, nothing ever added it.
       * Deterministic: exactly 79 failures every boot. WAITs succeed (real
       * per-context handles); only the SIGNAL carries this one.
       *
       * ⚠ Zero it so the value is at least DEFINED, and log the branch. This is
       * diagnosis, not the fix - the fix is either to always create the VM sync
       * object, or to make csf_submit skip the VM signal when there is none. */
      wvm->sync.handle = 0;
      wvm->sync.point  = 0;
      mesa_loge("wm: WMVMSYNC1 vm_create id=%u WITHOUT TRACK_ACTIVITY "
                "(flags=0x%x) - NO VM sync object exists; anything signalling "
                "vm_sync_handle will hit ENOENT", req.Id, flags);
   }

   pan_kmod_vm_init(&wvm->base, dev, req.Id, flags,
                    PAN_PGSIZE_4K | PAN_PGSIZE_2M);
   return &wvm->base;
}

static void
winmali_kmod_vm_collect_freed_vas(struct winmali_kmod_vm *wvm)
{
   simple_mtx_assert_locked(&wvm->auto_va.lock);

   list_for_each_entry_safe(struct winmali_kmod_va_collect, c,
                            &wvm->auto_va.gc_list, node) {
      if (wvm->base.flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY) {
         simple_mtx_lock(&wvm->sync.lock);
         uint64_t reached = wvm->sync.point;
         simple_mtx_unlock(&wvm->sync.lock);
         if (c->sync_point > reached)
            break;
      }
      util_vma_heap_free(&wvm->auto_va.heap, c->va, c->size);
      list_del(&c->node);
      free(c);
   }
}

static void
winmali_kmod_vm_destroy(struct pan_kmod_vm *vm)
{
   struct winmali_kmod_vm *wvm = to_winmali_vm(vm);

   if (vm->flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY) {
      /* Unregister BEFORE destroying: the VM is the handle's legitimate owner,
         so it must lift its own guard to free it (and the guard must not be
         left standing over a dead handle). */
      winmali_vm_sync_unregister(wvm->sync.handle);
      mesa_loge("wm: WMVMSYNC2 vm_destroy id=%u releasing sync handle=%u - every "
                "submit still holding this VM is now dead",
                wvm->vm_id, wvm->sync.handle);
      syncobj_destroy(wvm->sync.handle);
      simple_mtx_destroy(&wvm->sync.lock);
   }

   if (vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA) {
      list_for_each_entry_safe(struct winmali_kmod_va_collect, c,
                               &wvm->auto_va.gc_list, node) {
         list_del(&c->node);
         free(c);
      }
      util_vma_heap_finish(&wvm->auto_va.heap);
      simple_mtx_destroy(&wvm->auto_va.lock);
   }

   WINMALI_VM_DESTROY req = { .Id = wvm->vm_id };
   escape_simple(WinMaliEscapeOp_VmDestroy, &req, sizeof(req));

   pan_kmod_dev_free(vm->dev, wvm);
}

/*
 * ⛔ ALLOCATION, ADDRESSING AND SUBMISSION MUST ALL PICK THE SAME SIDE.
 *
 * WinMaliWddmBridge.h documents this gate as WINMALI_DXGK_ADDR ("panfrost only
 * calls these when WINMALI_DXGK_ADDR is set AND the BO is dxgk-backed"), but the
 * code only ever tested `map_va != NULL` - the same never-wired "opt-in" as
 * Phase 3's WINMALI_DXGK_SUBMIT. Use the documented name.
 *
 * Two coherent designs exist, and mixing them is what broke rendering:
 *   dxgk-native  : KMT handle is the BO identity -> GpuMmu addressing -> dxgk submit
 *   escape-native: escape BO handle -> escape VmBind -> escape GroupSubmit
 *
 * Straddling fails in BOTH directions; we measured both on 2026-08-12:
 *   - escape submit + dxgk addressing: the CS lives in dxgk's page tables while
 *     the CSG runs in the escape VM's, so the GPU takes TRANSLATION_FAULT_2
 *     READ on the command stream's OWN va (CMD_PTR=0, no shader core ever powers).
 *   - escape addressing + dxgk allocation: the BO's identity is its KMT handle,
 *     which the KMD's BO table has never heard of:
 *         WARN VmBind MAP: BoHandle 1073743168 unknown   (0x40000B40)
 *     surfacing as CreateTexture2D -> E_OUTOFMEMORY, panfrost_batch_init failed.
 *
 * The dxgk SUBMIT path cannot carry a command stream today (a 504-byte CS
 * arrives as SubmitCommandVirtual dmaSize=0), so escape-native is the only
 * complete design and the ALLOCATOR must follow it too. One gate drives all
 * three so they can never disagree again.
 */
static bool
winmali_dxgk_addr_enabled(void)
{
   static int s = -1;
   if (s < 0) {
      const char *e = getenv("WINMALI_DXGK_ADDR");
      s = (e && *e && *e != '0') ? 1 : 0;
      winmali_d3dkmt_log("wm: addressing = %s",
                         s ? "dxgk GpuMmu (alloc + map via dxgk)"
                           : "escape (BoCreate + VmBind) [default]");
   }
   return s != 0;
}

static uint64_t
winmali_kmod_vm_alloc_va(struct winmali_kmod_vm *wvm, uint64_t size)
{
   simple_mtx_assert_locked(&wvm->auto_va.lock);
   /* The VA heap is page-aligned (4K). pan_kmod callers already
    * size-align. Use a 4K alignment shift = 12. */
   return util_vma_heap_alloc(&wvm->auto_va.heap, size, 0x1000);
}

/* Phase 2 (opt-in): route a vm_bind batch through dxgk GpuMmu addressing
 * (map_va/unmap_va -> pfnMapGpuVirtualAddressCb) instead of escape VmBind. The
 * map/unmap callbacks are synchronous (they block on the paging queue), so we
 * honor WAIT sync ops before and SIGNAL sync ops after each op, mirroring the
 * group_submit sync bridge. Returns 0 on full success, -ENOTSUP if the batch is
 * not routable this way (any MAP targets a non-dxgk BO) so the caller falls back
 * to the proven escape VmBind, or -errno on a hard mapping failure. */
static int
winmali_vm_bind_dxgk(struct winmali_kmod_vm *wvm,
                     struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   const bool auto_va = wvm->base.flags & PAN_KMOD_VM_FLAG_AUTO_VA;
   void *cookie = g_wddm_ops.cookie;

   /* Pre-scan: every MAP target must be a dxgk-backed BO, else not routable.
    * Done before any work so falling back to escape is clean (no double-map). */
   for (uint32_t i = 0; i < op_count; i++) {
      if (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP &&
          to_winmali_bo(ops[i].map.bo)->wddm_kmt_handle == 0)
         return -ENOTSUP;
   }

   if (auto_va)
      simple_mtx_lock(&wvm->auto_va.lock);

   int err = 0;
   for (uint32_t i = 0; i < op_count && !err; i++) {
      struct pan_kmod_vm_op *op = &ops[i];

      /* WAIT sync ops first. */
      for (uint32_t s = 0; s < op->syncs.count; s++) {
         const struct pan_kmod_sync_op *si = &op->syncs.array[s];
         if (si->type != PAN_KMOD_SYNC_TYPE_SIGNAL)
            syncobj_timeline_wait(si->handle, si->point, INT64_MAX);
      }

      switch (op->type) {
      case PAN_KMOD_VM_OP_TYPE_MAP: {
         if (auto_va && op->va.start == PAN_KMOD_VM_MAP_AUTO_VA) {
            op->va.start = winmali_kmod_vm_alloc_va(wvm, op->va.size);
            if (op->va.start == 0) { err = -ENOMEM; break; }
         }
         uint64_t va = op->va.start;
         int merr = g_wddm_ops.map_va(cookie,
                                      to_winmali_bo(op->map.bo)->wddm_kmt_handle,
                                      (uint64_t)op->map.bo_offset,
                                      op->va.size, &va);
         if (merr) { err = merr; break; }
         op->va.start = va;
         break;
      }
      case PAN_KMOD_VM_OP_TYPE_UNMAP:
         g_wddm_ops.unmap_va(cookie, op->va.start, op->va.size);
         /* Synchronous unmap -> reclaim the VA immediately (no async gc). */
         if (auto_va)
            util_vma_heap_free(&wvm->auto_va.heap, op->va.start, op->va.size);
         break;
      case PAN_KMOD_VM_OP_TYPE_SYNC_ONLY:
      default:
         break;
      }

      /* SIGNAL sync ops after (synchronous map/unmap already complete). */
      if (!err) {
         for (uint32_t s = 0; s < op->syncs.count; s++) {
            const struct pan_kmod_sync_op *si = &op->syncs.array[s];
            if (si->type == PAN_KMOD_SYNC_TYPE_SIGNAL)
               syncobj_signal(si->handle, si->point);
         }
      }
   }

   if (auto_va)
      simple_mtx_unlock(&wvm->auto_va.lock);
   return err;
}

static int
winmali_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                     struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   struct winmali_kmod_vm *wvm = to_winmali_vm(vm);
   const bool track_activity =
      vm->flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY;
   const bool auto_va = vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA;
   const bool async = mode == PAN_KMOD_VM_OP_MODE_ASYNC;
   uint32_t flags = async ? WINMALI_VM_BIND_FLAG_ASYNC : 0;

   if (op_count == 0)
      return 0;

   /*
    * ⛔ PHASE 2 AND PHASE 3 ARE A MATCHED PAIR. DO NOT SPLIT THEM.
    *
    * dxgk GpuMmu addressing maps a BO into DXGK's page tables (via
    * pfnMapGpuVirtualAddressCb). The escape GroupSubmit path, by contrast,
    * binds the CSG to the ESCAPE VM's page tables (escape VmBind -> VmMap:
    * AS<n>). Those are two different address spaces. A BO mapped in one is
    * simply absent from the other.
    *
    * Measured 2026-08-12, with Phase 3 forced off but Phase 2 left on:
    *   GroupSubmit ... stream_va=0xffffffd74000 size=504 -> 0xc00000b5
    *   CSF fault: group AS2 FAULTSTATUS=0x7c0002c2
    *              (exc=0xc2 TRANSLATION_FAULT_2, access=READ)
    *              ADDR=0x0000ffffffd74000        <-- the CS's OWN address
    *   CMD_PTR=0  BLOCKED_REASON=0(UNBLOCKED)  SHADER_READY=0
    * and no VmBind MAP / VmMap line for that VA anywhere in the ring, versus
    * both present for the csf_init CS at 0xfffffffe6000 which runs fine. The
    * GPU could not fetch a single instruction, which is also why no shader
    * core ever powered up - that was a symptom, not the cause.
    *
    * So the pairing is: dxgk map + dxgk submit, or escape map + escape submit.
    * The dxgk SUBMIT path does not actually deliver a command stream (a
    * 504-byte CS shows up as SubmitCommandVirtual dmaSize=0), so the escape
    * pair is the one that can render today. Default both OFF together.
    *
    * Long term a WDDM driver should use dxgk's GpuMmu for both; that needs the
    * dxgk submit path fixed first, and then WINMALI_DXGK_MAPVA=1 +
    * WINMALI_DXGK_SUBMIT=1 turns the pair back on as a unit.
    */
   if (g_wddm_ops.map_va != NULL && winmali_dxgk_addr_enabled()) {
      int rc = winmali_vm_bind_dxgk(wvm, ops, op_count);
      if (rc != -ENOTSUP)
         return rc;
   }

   /* Count total sync ops for tail sizing. */
   uint32_t total_syncs = 0;
   for (uint32_t i = 0; i < op_count; i++)
      total_syncs += ops[i].syncs.count;

   /* Layout:
    *   [HEADER][WINMALI_VM_BIND][WINMALI_VM_BIND_OP[op_count]]
    *     [WINMALI_SYNC_OP[total_syncs]]
    *
    * Each WINMALI_VM_BIND_OP's SyncsOffset points into the sync
    * region (a single contiguous sub-array). */
   size_t header_size = sizeof(WINMALI_ESCAPE_HEADER);
   size_t payload_size = sizeof(WINMALI_VM_BIND);
   size_t ops_off = WINMALI_ALIGN_UP(header_size + payload_size,
                                     WINMALI_TAIL_ALIGN);
   size_t syncs_off = WINMALI_ALIGN_UP(
      ops_off + (size_t)op_count * sizeof(WINMALI_VM_BIND_OP),
      WINMALI_TAIL_ALIGN);
   size_t total = WINMALI_ALIGN_UP(
      syncs_off + (size_t)total_syncs * sizeof(WINMALI_SYNC_OP),
      WINMALI_TAIL_ALIGN);

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   WINMALI_VM_BIND *vmb = (WINMALI_VM_BIND *)(buf + header_size);
   WINMALI_VM_BIND_OP *wops = (WINMALI_VM_BIND_OP *)(buf + ops_off);
   WINMALI_SYNC_OP *wsyncs = (WINMALI_SYNC_OP *)(buf + syncs_off);

   hdr->Magic       = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor    = WINMALI_ABI_MAJOR;
   hdr->AbiMinor    = WINMALI_ABI_MINOR;
   hdr->Op          = WinMaliEscapeOp_VmBind;
   hdr->PayloadSize = (uint32_t)total;

   vmb->VmId      = wvm->vm_id;
   vmb->Flags     = flags;
   vmb->OpsCount  = op_count;
   vmb->OpsOffset = (uint32_t)ops_off;

   uint32_t sync_cursor = 0;

   if (auto_va) {
      simple_mtx_lock(&wvm->auto_va.lock);
      winmali_kmod_vm_collect_freed_vas(wvm);
   }

   for (uint32_t i = 0; i < op_count; i++) {
      struct pan_kmod_vm_op *op = &ops[i];
      WINMALI_VM_BIND_OP *out = &wops[i];

      switch (op->type) {
      case PAN_KMOD_VM_OP_TYPE_MAP:
         /* ★ THE LIVE CHANNEL. This is how a synthetic handle reached the KMD's
          * BoTable and produced the measured collision: every import's MAP op
          * arrives here, and escape VmBind addresses BOs by bo->handle.
          *
          * After the bo_from_allocation fix every handle in the default config
          * is KMD-issued, so this never fires. Under WINMALI_DXGK_ADDR=1 (the
          * only remaining synthetic minter) it converts a SILENT WRONG-BO
          * MAPPING into a loud, attributable failure - which is strictly what
          * we want, since silently mapping a stranger's BO is the mechanism
          * behind both this bug and refuted approach (A)'s "with base 1
          * something maps" half. */
         if (!to_winmali_bo(op->map.bo)->handle_is_kmd) {
            mesa_loge("winmali: REFUSING VmBind MAP of a non-KMD handle %u "
                      "(kmt=0x%x va=0x%llx size=%llu) - the KMD's BoTable never "
                      "issued it, so the escape would map an unrelated BO",
                      op->map.bo->handle,
                      to_winmali_bo(op->map.bo)->wddm_kmt_handle,
                      (unsigned long long)op->va.start,
                      (unsigned long long)op->va.size);
            if (auto_va)
               simple_mtx_unlock(&wvm->auto_va.lock);
            free(buf);
            errno = ENOTSUP;
            return -ENOTSUP;
         }
         out->Flags = WINMALI_VM_BIND_OP_TYPE_MAP
                    | (op->map.bo->flags & 0xff);
         out->BoHandle = op->map.bo->handle;
         out->BoOffset = (uint64_t)op->map.bo_offset;
         if (auto_va && op->va.start == PAN_KMOD_VM_MAP_AUTO_VA) {
            op->va.start = winmali_kmod_vm_alloc_va(wvm, op->va.size);
            if (op->va.start == 0) {
               simple_mtx_unlock(&wvm->auto_va.lock);
               free(buf);
               return -ENOMEM;
            }
         }
         out->Va = op->va.start;
         out->Size = op->va.size;
         break;
      case PAN_KMOD_VM_OP_TYPE_UNMAP:
         out->Flags = WINMALI_VM_BIND_OP_TYPE_UNMAP;
         out->Va = op->va.start;
         out->Size = op->va.size;
         break;
      case PAN_KMOD_VM_OP_TYPE_SYNC_ONLY:
         out->Flags = WINMALI_VM_BIND_OP_TYPE_SYNC_ONLY;
         break;
      default:
         break;
      }

      /* Sync tail. */
      out->SyncsCount = op->syncs.count;
      out->SyncsOffset = op->syncs.count
                       ? (uint32_t)(syncs_off + sync_cursor * sizeof(WINMALI_SYNC_OP))
                       : 0;
      for (uint32_t s = 0; s < op->syncs.count; s++, sync_cursor++) {
         const struct pan_kmod_sync_op *si = &op->syncs.array[s];
         WINMALI_SYNC_OP *so = &wsyncs[sync_cursor];
         so->Flags = (si->type == PAN_KMOD_SYNC_TYPE_SIGNAL
                      ? WINMALI_SYNC_OP_SIGNAL : WINMALI_SYNC_OP_WAIT)
                   | WINMALI_SYNC_OP_HANDLE_TYPE_TIMELINE;
         so->Handle = si->handle;
         so->TimelineValue = si->point;
      }
   }

   int err = winmali_d3dkmt_escape(buf, total);
   free(buf);

   /* Queue auto-VA reclaim for async unmaps. */
   if (auto_va) {
      uint64_t latest_point = 0;
      if (track_activity) {
         simple_mtx_lock(&wvm->sync.lock);
         latest_point = wvm->sync.point;
         simple_mtx_unlock(&wvm->sync.lock);
      }
      for (uint32_t i = 0; i < op_count; i++) {
         if (ops[i].type != PAN_KMOD_VM_OP_TYPE_UNMAP)
            continue;
         if (err == 0 && async && track_activity) {
            struct winmali_kmod_va_collect *c =
               (struct winmali_kmod_va_collect *)calloc(1, sizeof(*c));
            if (c) {
               c->sync_point = latest_point;
               c->va = ops[i].va.start;
               c->size = ops[i].va.size;
               list_addtail(&c->node, &wvm->auto_va.gc_list);
               continue;
            }
            /* fallthrough: free immediately on allocation failure */
         }
         util_vma_heap_free(&wvm->auto_va.heap, ops[i].va.start,
                            ops[i].va.size);
      }
      simple_mtx_unlock(&wvm->auto_va.lock);
   }

   return err;
}

static enum pan_kmod_vm_state
winmali_kmod_vm_query_state(struct pan_kmod_vm *vm)
{
   struct winmali_kmod_vm *wvm = to_winmali_vm(vm);
   WINMALI_VM_GET_STATE req = { .VmId = wvm->vm_id };
   int err = escape_simple(WinMaliEscapeOp_VmGetState, &req, sizeof(req));
   if (err)
      return PAN_KMOD_VM_FAULTY;
   return req.State == WinMaliVmState_Usable
          ? PAN_KMOD_VM_USABLE : PAN_KMOD_VM_FAULTY;
}

/* --------------------------------------------------------------------- */
/*  Timestamp                                                            */
/* --------------------------------------------------------------------- */

static uint64_t
winmali_kmod_query_timestamp(const struct pan_kmod_dev *dev)
{
   (void)dev;
   size_t header = sizeof(WINMALI_ESCAPE_HEADER);
   size_t payload = sizeof(WINMALI_DEV_QUERY);
   size_t result = sizeof(WINMALI_TIMESTAMP_INFO);
   size_t roff = WINMALI_ALIGN_UP(header + payload, WINMALI_TAIL_ALIGN);
   size_t total = WINMALI_ALIGN_UP(roff + result, WINMALI_TAIL_ALIGN);

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return 0;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   WINMALI_DEV_QUERY *args = (WINMALI_DEV_QUERY *)(buf + header);
   hdr->Magic = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor = WINMALI_ABI_MAJOR;
   hdr->AbiMinor = WINMALI_ABI_MINOR;
   hdr->Op = WinMaliEscapeOp_DevQuery;
   hdr->PayloadSize = (uint32_t)total;
   args->Type = WinMaliDevQuery_TimestampInfo;
   args->Size = (uint32_t)result;
   args->PointerOffset = roff;

   uint64_t ts = 0;
   if (winmali_d3dkmt_escape(buf, total) == 0) {
      WINMALI_TIMESTAMP_INFO *ti = (WINMALI_TIMESTAMP_INFO *)(buf + roff);
      ts = ti->CurrentTimestamp;
   }
   free(buf);
   return ts;
}

/* --------------------------------------------------------------------- */
/*  pan_kmod_ops vtable                                                  */
/* --------------------------------------------------------------------- */

const struct pan_kmod_ops winmali_kmod_ops = {
   .dev_create               = winmali_kmod_dev_create,
   .dev_destroy              = winmali_kmod_dev_destroy,
   .dev_query_user_va_range  = winmali_kmod_dev_query_user_va_range,
   .bo_alloc                 = winmali_kmod_bo_alloc,
   .bo_free                  = winmali_kmod_bo_free,
   .bo_import                = winmali_kmod_bo_import,
   .bo_export                = winmali_kmod_bo_export,
   .bo_get_mmap_offset       = winmali_kmod_bo_get_mmap_offset,
   .flush_bo_map_syncs       = winmali_kmod_flush_bo_map_syncs,
   .bo_wait                  = winmali_kmod_bo_wait,
   .vm_create                = winmali_kmod_vm_create,
   .vm_destroy               = winmali_kmod_vm_destroy,
   .vm_bind                  = winmali_kmod_vm_bind,
   .vm_query_state           = winmali_kmod_vm_query_state,
   .query_timestamp          = winmali_kmod_query_timestamp,
   .bo_set_label             = winmali_kmod_bo_set_label,
};

/* --------------------------------------------------------------------- */
/*  Ioctl dispatch (called from pan_kmod_ioctl macro on WINMALI fd)      */
/* --------------------------------------------------------------------- */

static uint32_t
panthor_ioctl_to_escape_op(unsigned long request)
{
   /* DRM_IOWR / DRM_IO macros encode (cmd_nr) in the low byte. */
   unsigned nr = (unsigned)(request & 0xff);

   /* drm_panthor ioctls are DRM_COMMAND_BASE (0x40) + drm_panthor_ioctl_id
    * (0..16), and that id maps 1:1 onto WinMaliEscapeOp_* for the range the
    * KMD implements (DevQuery..BoQueryInfo). The previous 0x80 base was
    * wrong - it made GROUP_CREATE (nr=0x47) and every other panthor ioctl
    * return -ENOTTY, which blocked csf_init_context's GROUP_CREATE
    * (seen 2026-07-12: "csf_init: GROUP_CREATE failed ret=-25"). */
   if (nr >= 0x40 && nr <= 0x50)
      return (uint32_t)(nr - 0x40);

   return UINT32_MAX;
}

/* GROUP_CREATE / GROUP_SUBMIT / TILER_HEAP_CREATE are NOT layout-compatible
 * with their WINMALI_* escape structs: the drm structs pass array tails as
 * user pointers (drm_panthor_obj_array) and order some fields differently,
 * while the WinMali ABI uses byte-offset tails. Translate in-process (UMD
 * side) before escaping. The other panthor ioctls (GROUP_DESTROY,
 * GROUP_GET_STATE, TILER_HEAP_DESTROY, and the sub-structs queue_create /
 * sync_op) ARE layout-compatible and go through escape_simple. */

static int
winmali_ioctl_group_create(void *arg)
{
   struct drm_panthor_group_create *gc = arg;
   const struct drm_panthor_queue_create *qsrc =
      (const struct drm_panthor_queue_create *)(uintptr_t)gc->queues.array;
   uint32_t qcount = gc->queues.count;

   size_t header = sizeof(WINMALI_ESCAPE_HEADER);
   size_t queues_off = WINMALI_ALIGN_UP(header + sizeof(WINMALI_GROUP_CREATE),
                                        WINMALI_TAIL_ALIGN);
   size_t total = WINMALI_ALIGN_UP(
      queues_off + (size_t)qcount * sizeof(WINMALI_QUEUE_CREATE),
      WINMALI_TAIL_ALIGN);

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   WINMALI_GROUP_CREATE *body = (WINMALI_GROUP_CREATE *)(buf + header);
   hdr->Magic       = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor    = WINMALI_ABI_MAJOR;
   hdr->AbiMinor    = WINMALI_ABI_MINOR;
   hdr->Op          = WinMaliEscapeOp_GroupCreate;
   hdr->PayloadSize = (uint32_t)total;
   body->QueuesCount      = qcount;
   body->QueuesOffset     = (uint32_t)queues_off;
   body->MaxComputeCores  = gc->max_compute_cores;
   body->MaxFragmentCores = gc->max_fragment_cores;
   body->MaxTilerCores    = gc->max_tiler_cores;
   body->Priority         = gc->priority;
   body->ComputeCoreMask  = gc->compute_core_mask;
   body->FragmentCoreMask = gc->fragment_core_mask;
   body->TilerCoreMask    = gc->tiler_core_mask;
   body->VmId             = gc->vm_id;
   /* WINMALI_QUEUE_CREATE is layout-compatible with drm_panthor_queue_create. */
   if (qcount)
      memcpy(buf + queues_off, qsrc,
             (size_t)qcount * sizeof(WINMALI_QUEUE_CREATE));

   int err = winmali_d3dkmt_escape(buf, (uint32_t)total);
   if (err == 0) {
      gc->group_handle = body->GroupHandle;
      winmali_group_vm_record(body->GroupHandle, gc->vm_id);
   } else {
      winmali_d3dkmt_log("wm: GROUP_CREATE escape failed (%d)", err);
   }
   free(buf);
   return err;
}

static int
winmali_ioctl_tiler_heap_create(void *arg)
{
   struct drm_panthor_tiler_heap_create *thc = arg;

   /* Build the buffer manually: the KMD reads the body at exactly
    * sizeof(WINMALI_ESCAPE_HEADER) (36, not 8-aligned), but a
    * struct{hdr;body;} would 8-align the u64-containing body to offset 40,
    * shifting every field 4 bytes and making the KMD read VmId from zero
    * padding -> -EINVAL. Match the winmali_kmod.c escape helpers. */
   size_t header = sizeof(WINMALI_ESCAPE_HEADER);
   size_t total  = header + sizeof(WINMALI_TILER_HEAP_CREATE);
   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   WINMALI_TILER_HEAP_CREATE *body = (WINMALI_TILER_HEAP_CREATE *)(buf + header);
   hdr->Magic       = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor    = WINMALI_ABI_MAJOR;
   hdr->AbiMinor    = WINMALI_ABI_MINOR;
   hdr->Op          = WinMaliEscapeOp_TilerHeapCreate;
   hdr->PayloadSize = (uint32_t)total;
   body->VmId                 = thc->vm_id;
   body->ChunkSize            = thc->chunk_size;
   body->InitialChunkCount    = thc->initial_chunk_count;
   body->MaxChunks            = thc->max_chunks;
   body->TargetInFlightChunks = thc->target_in_flight;

   int err = winmali_d3dkmt_escape(buf, (uint32_t)total);
   if (err == 0) {
      thc->handle                  = body->HeapHandle;
      thc->first_heap_chunk_gpu_va = body->FirstChunkGpuVa;
      /* The KMD returns a distinct, mapped context-descriptor VA (or fails the
       * escape outright - it no longer hands back a placeholder). */
      thc->tiler_heap_ctx_gpu_va   = body->TilerHeapCtxGpuVa;
   } else {
      winmali_d3dkmt_log("wm: TILER_HEAP_CREATE escape failed (%d)", err);
   }
   free(buf);
   return err;
}

static int
winmali_ioctl_group_submit(void *arg)
{
   struct drm_panthor_group_submit *gs = arg;
   const struct drm_panthor_queue_submit *qsrc =
      (const struct drm_panthor_queue_submit *)(uintptr_t)gs->queue_submits.array;
   uint32_t nqs = gs->queue_submits.count;
   uint32_t total_syncs = 0, i;

   /* Phase 3 (opt-in): submit the command stream through the dxgk render
    * context (pfnRenderCb) instead of escape GroupSubmit, so rendering rides
    * dxgk-scheduled DMA buffers. Gated by WINMALI_DXGK_SUBMIT; default is the
    * proven escape path below.
    * Sync bridge: the KMD SubmitCommandVirtual -> CsfSubmitGroupStream path is
    * SYNCHRONOUS (it flushes and waits for CSG completion before returning), so
    * by the time g_wddm_ops.render() returns the GPU work is done. We therefore
    * honor WAIT sync ops before the submit (defensive; prior work has already
    * completed under this synchronous model) and SIGNAL sync ops after it, using
    * the same syncobj handles/points the escape GroupSubmit would have signalled.
    * This makes downstream mesa syncobj waits (pan_csf completion, fence export)
    * complete instead of hanging. */
   /*
    * ⛔ THE "opt-in" ABOVE WAS NEVER IMPLEMENTED. `WINMALI_DXGK_SUBMIT` appears
    * NOWHERE in the tree except that comment; the gate was merely "is
    * g_wddm_ops.render non-NULL", so Phase 3 ran ALWAYS and the "proven escape
    * path below" became dead code the moment the render callback registered.
    *
    * Measured on arch 11: mesa builds a real 504-byte command stream per draw
    * (`csf_submit_batch cs_size=504`), yet the KMD ring only ever shows
    * `GroupSubmit ... size=40` -- the csf_init stream, which runs BEFORE the
    * callback is registered. Render work went down this path instead, appearing
    * as `SubmitCommandVirtual ... dmaSize=8/16/0`, and never reached the GPU:
    * no fault, no completion, and mesa hangs on a syncobj that never signals.
    *
    * Make the code do what its own comment says: honour the env var, default
    * OFF (escape GroupSubmit).
    */
   static int s_dxgk_submit = -1;
   if (s_dxgk_submit < 0) {
      const char *e = getenv("WINMALI_DXGK_SUBMIT");
      s_dxgk_submit = (e && *e && *e != '0') ? 1 : 0;
      winmali_d3dkmt_log("wm: submit path = %s",
                         s_dxgk_submit ? "PHASE3 dxgk render cb"
                                       : "escape GroupSubmit (default)");
   }

   if (g_wddm_ops.render != NULL && s_dxgk_submit) {
      uint32_t vm_id = winmali_group_vm_lookup(gs->group_handle);
      int rerr = 0;

      /* WAIT ops first. */
      for (i = 0; i < nqs && !rerr; ++i) {
         const WINMALI_SYNC_OP *so =
            (const WINMALI_SYNC_OP *)(uintptr_t)qsrc[i].syncs.array;
         for (uint32_t s = 0; s < qsrc[i].syncs.count; ++s) {
            if (!(so[s].Flags & WINMALI_SYNC_OP_SIGNAL))
               syncobj_timeline_wait(so[s].Handle, so[s].TimelineValue,
                                     INT64_MAX);
         }
      }

      for (i = 0; i < nqs; ++i) {
         if (qsrc[i].stream_addr == 0 || qsrc[i].stream_size == 0)
            continue;
         rerr = g_wddm_ops.render(g_wddm_ops.cookie, vm_id,
                                  qsrc[i].stream_addr, qsrc[i].stream_size);
         if (rerr)
            break;
      }

      if (!rerr) {
         /* SIGNAL ops after: work is complete (synchronous submit). */
         for (i = 0; i < nqs; ++i) {
            const WINMALI_SYNC_OP *so =
               (const WINMALI_SYNC_OP *)(uintptr_t)qsrc[i].syncs.array;
            for (uint32_t s = 0; s < qsrc[i].syncs.count; ++s) {
               if (so[s].Flags & WINMALI_SYNC_OP_SIGNAL)
                  syncobj_signal(so[s].Handle, so[s].TimelineValue);
            }
         }
         return 0;
      }
      /* mesa_loge, not winmali_d3dkmt_log: the latter goes to the EDK2 ring,
         which is FILL-ONCE and long since full, so this has been invisible. */
      mesa_loge("wm: dxgk render submit failed (%d); "
                "falling back to escape GroupSubmit", rerr);
      /* fall through to the escape path */
   }

   for (i = 0; i < nqs; ++i)
      total_syncs += qsrc[i].syncs.count;

   size_t header    = sizeof(WINMALI_ESCAPE_HEADER);
   size_t qs_off    = WINMALI_ALIGN_UP(header + sizeof(WINMALI_GROUP_SUBMIT),
                                       WINMALI_TAIL_ALIGN);
   size_t flush_off = WINMALI_ALIGN_UP(
      qs_off + (size_t)nqs * sizeof(WINMALI_QUEUE_SUBMIT), WINMALI_TAIL_ALIGN);
   size_t syncs_off = WINMALI_ALIGN_UP(
      flush_off + (size_t)nqs * sizeof(uint32_t), WINMALI_TAIL_ALIGN);
   size_t total = WINMALI_ALIGN_UP(
      syncs_off + (size_t)total_syncs * sizeof(WINMALI_SYNC_OP),
      WINMALI_TAIL_ALIGN);

   uint8_t *buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
   WINMALI_GROUP_SUBMIT *body = (WINMALI_GROUP_SUBMIT *)(buf + header);
   WINMALI_QUEUE_SUBMIT *qdst = (WINMALI_QUEUE_SUBMIT *)(buf + qs_off);
   uint32_t *flush = (uint32_t *)(buf + flush_off);
   WINMALI_SYNC_OP *syncdst = (WINMALI_SYNC_OP *)(buf + syncs_off);
   uint32_t sync_cursor = 0;

   hdr->Magic       = WINMALI_ESCAPE_MAGIC;
   hdr->AbiMajor    = WINMALI_ABI_MAJOR;
   hdr->AbiMinor    = WINMALI_ABI_MINOR;
   hdr->Op          = WinMaliEscapeOp_GroupSubmit;
   hdr->PayloadSize = (uint32_t)total;
   body->GroupHandle          = gs->group_handle;
   body->QueueSubmitsCount    = nqs;
   body->QueueSubmitsOffset   = (uint32_t)qs_off;
   body->LatestFlushIdsCount  = nqs;
   body->LatestFlushIdsOffset = (uint32_t)flush_off;

   for (i = 0; i < nqs; ++i) {
      const struct drm_panthor_sync_op *ssrc =
         (const struct drm_panthor_sync_op *)(uintptr_t)qsrc[i].syncs.array;
      uint32_t sc = qsrc[i].syncs.count;

      qdst[i].QueueIndex        = qsrc[i].queue_index;
      qdst[i].StreamSize        = qsrc[i].stream_size;
      qdst[i].StreamAddr        = qsrc[i].stream_addr;
      qdst[i].SyncsCount        = sc;
      qdst[i].SyncsOffset       = (uint32_t)(syncs_off +
                                    (size_t)sync_cursor * sizeof(WINMALI_SYNC_OP));
      qdst[i].LatestFlushIdSlot = i;
      flush[i]                  = qsrc[i].latest_flush;
      /* WINMALI_SYNC_OP is layout-compatible with drm_panthor_sync_op. */
      if (sc)
         memcpy(syncdst + sync_cursor, ssrc,
                (size_t)sc * sizeof(WINMALI_SYNC_OP));
      sync_cursor += sc;
   }

   int err = winmali_d3dkmt_escape(buf, (uint32_t)total);
   if (err != 0) {
      /* ⛔ winmali_d3dkmt_log goes to the KMD's EDK2 ring, which is FILL-ONCE
       * and is almost certainly already full - so this failure has been
       * invisible. mesa_loge reaches MESA_LOG_FILE, the proven channel.
       * Carry the queue/group identity: a bad handle and a rejected command
       * stream are different bugs and must not look alike. */
      mesa_loge("wm: GROUP_SUBMIT escape FAILED err=%d group=%u queues=%u "
                "cs0=0x%llx size0=%u syncs=%u",
                err, (unsigned)body->GroupHandle, (unsigned)nqs,
                (unsigned long long)(nqs ? qdst[0].StreamAddr : 0ull),
                (unsigned)(nqs ? qdst[0].StreamSize : 0u),
                (unsigned)total_syncs);
   }
   free(buf);
   return err;
}

int
winmali_kmod_ioctl(unsigned long request, void *arg)
{
   if (!winmali_d3dkmt_is_open())
      return -ENODEV;

   uint32_t op = panthor_ioctl_to_escape_op(request);
   if (op == UINT32_MAX)
      return -ENOTTY;

   /* Ops whose drm struct carries user-pointer array tails or a divergent
    * field order need translation into the WinMali escape wire format. */
   switch (op) {
   case WinMaliEscapeOp_GroupCreate:
      return winmali_ioctl_group_create(arg);
   case WinMaliEscapeOp_GroupSubmit:
      return winmali_ioctl_group_submit(arg);
   case WinMaliEscapeOp_TilerHeapCreate:
      return winmali_ioctl_tiler_heap_create(arg);
   default:
      break;
   }

   /* Ops that carry a BO handle must NOT reach the KMD through this raw
    * passthrough. It is wrong twice: the handle may be a UMD-synthetic id the
    * BoTable never issued (the collision that produced the black desktop), and
    * the drm_panthor structs for these ops are NOT layout-compatible with their
    * WINMALI_* counterparts (obj_array tails vs offset tails), so the payload
    * would be misparsed even with a valid handle.
    *
    * Dead in this build - the only callers are panvk files and the build sets
    * vulkan-drivers=[] - but this is the last unfenced channel, so close it
    * rather than leave a trap for whenever panvk lights up. */
   switch (op) {
   case WinMaliEscapeOp_VmBind:
   case WinMaliEscapeOp_BoMmapOffset:
   case WinMaliEscapeOp_BoSetLabel:
      mesa_loge("winmali: refusing raw ioctl passthrough of BO-handle op %u - "
                "handle provenance is unchecked here and the drm struct layout "
                "does not match the escape wire format", op);
      return -ENOTSUP;
   default:
      break;
   }

   /* The struct size is in the high bits of the ioctl encoding. _IOWR
    * puts size in bits 16..29. The remaining ops (group/heap destroy,
    * group get-state) are layout-compatible with their WINMALI_* structs. */
   size_t payload_size = (size_t)((request >> 16) & 0x3fff);
   if (payload_size == 0)
      return -EINVAL;

   return escape_simple(op, arg, payload_size);
}

/* --------------------------------------------------------------------- */
/*  panthor_kmod_* public helpers                                        */
/*                                                                       */
/*  pan_csf.c and panvk-v10 reference these directly (not through the    */
/*  pan_kmod_ops vtable). On Linux they live in panthor_kmod.c; on       */
/*  Windows we mirror them here against winmali_kmod_vm / _bo state.    */
/* --------------------------------------------------------------------- */

const struct drm_panthor_csif_info *
panthor_kmod_get_csif_props(const struct pan_kmod_dev *dev)
{
   const struct winmali_kmod_dev *wdev =
      container_of(dev, const struct winmali_kmod_dev, base);
   /* WINMALI_CSIF_INFO is layout-compatible with drm_panthor_csif_info. */
   return (const struct drm_panthor_csif_info *)&wdev->props.csif;
}

uint32_t
panthor_kmod_get_flush_id(const struct pan_kmod_dev *dev)
{
   (void)dev;
   WINMALI_GET_FLUSH_ID req = { 0 };
   int err = escape_simple(WinMaliEscapeOp_GetFlushId, &req, sizeof(req));
   return err == 0 ? req.FlushId : 0;
}

int
panthor_kmod_bo_attach_sync_point(struct pan_kmod_bo *bo,
                                  uint32_t sync_handle, uint64_t sync_point,
                                  bool written)
{
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);

   /* ★ SYMBOLIZED from a LogonUI full dump: c0000005 at winmali_kmod.c:2230 -
    * the `bo->exclusive_vm` test below, with bo == NULL. Callers reach here
    * during teardown/eviction holding a BO that has already been released; this
    * is the same "a pointer that looks live is not one" family as the
    * panfrost_bo kmod_bo guards. Attaching a sync point to nothing is a no-op,
    * not a fatal condition - refuse it rather than fault inside the
    * compositor. */
   if (unlikely(bo == NULL || wbo == NULL)) {
      mesa_loge("panthor_kmod_bo_attach_sync_point: NULL bo (bo=%p wbo=%p "
                "handle=%u point=%llu) - ignoring",
                (void *)bo, (void *)wbo, sync_handle,
                (unsigned long long)sync_point);
      return -1;
   }

   /* For exclusive_vm BOs we share the VM's syncobj — caller passes
    * vm->sync.handle, and write/read points are tracked relative to
    * that timeline. For unshared BOs we use our own. The two cases
    * land here the same way: just update the local point counters. */
   if (bo->exclusive_vm) {
      assert(sync_handle ==
             to_winmali_vm(bo->exclusive_vm)->sync.handle);
   } else {
      /* For unshared BOs panthor uses syncobj_transfer to fold the
       * external sync into the BO's own handle. We don't have that
       * primitive on WinMali; the simplest model is to track the
       * external handle/point directly. Sync waits then use whichever
       * handle was last attached. */
      /* ⛔★★★★★ THIS RETARGETS sync.handle TO SOMEBODY ELSE'S HANDLE.
       *
       * The caller passes the VM's sync handle after a submit, so an imported
       * BO that minted handle 18 silently starts pointing at handle 1. That is
       * INTENDED for waiting - but it means sync.handle is NOT an ownership
       * record, and bo_free must never free it. It frees sync.owned_handle
       * instead. Measured cost of getting this wrong: dwm's handle 1 destroyed
       * 4x per boot, 79 ENOENT submits, no compositing for the whole session. */
      wbo->sync.handle = sync_handle;
   }

   if (sync_point > wbo->sync.read_point)
      wbo->sync.read_point = sync_point;
   if (written && sync_point > wbo->sync.write_point)
      wbo->sync.write_point = sync_point;

   return 0;
}

int
panthor_kmod_bo_get_sync_point(struct pan_kmod_bo *bo, uint32_t *sync_handle,
                               uint64_t *sync_point, bool read_only)
{
   /* ⚠ to_winmali_bo() is a container_of, so a NULL bo does NOT produce a NULL
    * wbo - it produces a small non-NULL offset, and the very next field read
    * faults. That is the measured LogonUI c0000005 at winmali_kmod.c:2002
    * (WinMaliUmd+0xadb5c on 1.0.7.8).
    *
    * A NULL bo reaches here when an import failed upstream (e.g. the KMT handle
    * collision now rejected in panfrost_resource_from_winmali_kmt) and the
    * caller carried the NULL resource forward. Report "nothing to sync on"
    * rather than dying: a surface we refused to import has, correctly, no sync
    * state, and the caller's zero-initialised outputs are the right answer. */
   if (bo == NULL) {
      if (sync_handle)
         *sync_handle = 0;
      if (sync_point)
         *sync_point = 0;
      return 0;
   }

   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);
   if (sync_handle)
      *sync_handle = wbo->sync.handle;
   if (sync_point) {
      *sync_point = read_only
         ? wbo->sync.write_point
         : MAX2(wbo->sync.read_point, wbo->sync.write_point);
   }
   return 0;
}

uint32_t
panthor_kmod_vm_sync_handle(struct pan_kmod_vm *vm)
{
   assert(vm->flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY);
   return to_winmali_vm(vm)->sync.handle;
}

uint64_t
panthor_kmod_vm_sync_lock(struct pan_kmod_vm *vm)
{
   struct winmali_kmod_vm *wvm = to_winmali_vm(vm);
   assert(vm->flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY);
   simple_mtx_lock(&wvm->sync.lock);
   return wvm->sync.point;
}

void
panthor_kmod_vm_sync_unlock(struct pan_kmod_vm *vm, uint64_t new_sync_point)
{
   struct winmali_kmod_vm *wvm = to_winmali_vm(vm);
   assert(vm->flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY);
   assert(new_sync_point >= wvm->sync.point);
   wvm->sync.point = new_sync_point;
   simple_mtx_unlock(&wvm->sync.lock);
}

#endif /* _WIN32 */

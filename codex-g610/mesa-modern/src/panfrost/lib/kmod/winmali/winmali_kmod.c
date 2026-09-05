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

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "drm-uapi/panthor_drm.h"

#include "util/list.h"
#include "util/log.h"
#include "util/os_mman.h"
#include "util/simple_mtx.h"
#include "util/u_idalloc.h"
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

   /* pan_kmod_bo::handle is a Mesa-local identity. Panfrost indexes dense
    * per-batch arrays with it, so a sparse native handle (in particular a
    * 0x4000xxxx D3DKMT allocation handle) must never be stored there.
    *
    * escape_handle is the private KMD BO-table handle returned by BoCreate.
    * wddm_kmt_handle is a dxgkrnl allocation handle. Exactly one is non-zero.
    */
   uint32_t escape_handle;

   /* Per-BO sync. If the BO has an exclusive_vm we reuse the VM's
    * sync handle (work touching the BO is already ordered against
    * the VM timeline). Otherwise we own a timeline handle here. */
   struct {
      uint32_t handle;
      uint64_t read_point;
      uint64_t write_point;
   } sync;

   /* Phase 1: if non-zero this BO is backed by a dxgk allocation the WDDM
    * bridge created (via pfnAllocateCb); bo_free releases it through the
    * hook. Zero = escape-BoCreate-backed (baseline). */
   uint32_t wddm_kmt_handle;
   /* Persistent CPU mapping of the dxgk allocation (from pfnLockCb, held for
    * the BO's lifetime). NULL for escape-backed BOs (they map via BoMapCpu). */
   void *wddm_cpu_va;

   /* Snapshot the allocation owner's release route.  A BO must be freed by
    * the same D3D device that created its KMT allocation, even while another
    * device is being created or destroyed in the process. */
   void *wddm_owner_cookie;
   void (*wddm_free)(void *cookie, uint32_t kmt_handle);
};

struct winmali_kmod_dev {
   struct pan_kmod_dev base;
   uint32_t           *flush_id;       /* not used on Windows; left as NULL */
   struct util_idalloc_mt bo_ids;      /* dense Mesa-local BO identities */
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

/* D3D runtime callbacks belong to one Device, while screen construction is
 * synchronous and may occur concurrently in several application threads.
 * Carry the callbacks into winmali_kmod_dev_create through TLS rather than a
 * process-global table, which could attach allocations to the wrong Device. */
static _Thread_local struct winmali_wddm_alloc_ops
   g_create_wddm_alloc_ops;

void
winmali_kmod_set_create_wddm_alloc_ops(
   const struct winmali_wddm_alloc_ops *ops)
{
   if (ops)
      g_create_wddm_alloc_ops = *ops;
   else
      memset(&g_create_wddm_alloc_ops, 0, sizeof(g_create_wddm_alloc_ops));
}

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
static void winmali_group_vm_forget_owner(struct winmali_kmod_dev *owner);
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

   /* Install the WDDM data plane before returning the kmod device to
    * panfrost_open_device(). Device and screen bootstrap immediately create
    * and VM-bind executable preload shaders, sample positions, tiler state,
    * and descriptor pools; all of them must therefore be real dxgk
    * allocations from their first allocation, not escape-private BOs that are
    * invisible from the WDDM process page-table root. */
   wdev->wddm_alloc = g_create_wddm_alloc_ops;
   if (wdev->wddm_alloc.alloc)
      winmali_d3dkmt_log("wm: bootstrap bridge dev=%p cookie=%p",
                         (void *)wdev, wdev->wddm_alloc.cookie);

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
   util_idalloc_mt_init(&wdev->bo_ids, 64, true);

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

   /* Context teardown should have destroyed every group first.  Purge any
    * failed/partial-create residue before the owner pointer becomes invalid. */
   winmali_group_vm_forget_owner(wdev);
   pan_kmod_dev_cleanup(dev);
   util_idalloc_mt_fini(&wdev->bo_ids);
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

/* Mesa's libdrm-shaped syncobj surface uses WinMali escape handles.  Keep the
 * helpers in that namespace.  A previous process-global callback table made
 * the meaning of a numeric handle depend on whichever D3D device most recently
 * installed its callbacks. */
#define WINMALI_BO_SYNC_BASE_POINT 1ull

static int
syncobj_create_timeline(uint32_t *out_handle, bool initially_signaled)
{
   WINMALI_SYNC_OBJ_CREATE req = {
      .Flags = WINMALI_SYNC_OBJ_FLAG_TIMELINE,
      .InitialState = initially_signaled ? 1 : 0,
   };
   int err = escape_simple(WinMaliEscapeOp_SyncObjCreate, &req, sizeof(req));
   if (err)
      return err;
   *out_handle = req.Handle;
   return 0;
}

static int
syncobj_destroy(uint32_t handle)
{
   WINMALI_SYNC_OBJ_DESTROY req = { .Handle = handle };
   return escape_simple(WinMaliEscapeOp_SyncObjDestroy, &req, sizeof(req));
}

static int
syncobj_signal(uint32_t handle, uint64_t point)
{
   WINMALI_SYNC_OBJ_SIGNAL req = { .Handle = handle, .Point = point };
   return escape_simple(WinMaliEscapeOp_SyncObjSignal, &req, sizeof(req));
}

static int
syncobj_transfer(uint32_t dst_handle, uint64_t dst_point,
                 uint32_t src_handle, uint64_t src_point)
{
   WINMALI_SYNC_OBJ_TRANSFER req = {
      .DstHandle = dst_handle,
      .SrcHandle = src_handle,
      .DstPoint = dst_point,
      .SrcPoint = src_point,
   };
   return escape_simple(WinMaliEscapeOp_SyncObjTransfer, &req, sizeof(req));
}

static int
syncobj_timeline_wait(uint32_t handle, uint64_t point, int64_t timeout_ns)
{
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

   uint32_t escape_handle = 0;
   uint32_t wddm_kmt  = 0;
   void    *wddm_cpu  = NULL;
   void    *wddm_owner_cookie = NULL;
   void   (*wddm_free)(void *, uint32_t) = NULL;

   /* dxgk-native path (default whenever the WDDM bridge is installed): back the
    * BO with a REAL dxgk allocation, pinned resident + CPU-mapped by the C++
    * frontend (pfnAllocateCb/pfnLockCb). It is addressed via GpuMmu (vm_bind ->
    * pfnMapGpuVirtualAddressCb) and CPU-accessed through the locked VA - no
    * escape BoCreate, no BoFromAllocation aliasing. A failure is a HARD error,
    * surfaced to the caller rather than masked by an escape fallback. */
   if (wdev->wddm_alloc.alloc) {
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
      wddm_owner_cookie = wdev->wddm_alloc.cookie;
      wddm_free = wdev->wddm_alloc.free;
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
      escape_handle = req.Handle;
   }

   assert((escape_handle != 0) != (wddm_kmt != 0));

   /* Sync handle: reuse VM's if exclusive, otherwise mint a fresh
    * timeline so unrelated work doesn't block reads. */
   if (exclusive_vm) {
      wbo->sync.handle = to_winmali_vm(exclusive_vm)->sync.handle;
   } else {
      uint32_t h = 0;
      int serr = syncobj_create_timeline(&h, true);
      if (serr) {
         if (wddm_kmt) {
            if (wddm_free)
               wddm_free(wddm_owner_cookie, wddm_kmt);
         } else {
            WINMALI_BO_DESTROY d = { .Handle = escape_handle };
            escape_simple(WinMaliEscapeOp_BoDestroy, &d, sizeof(d));
         }
         pan_kmod_dev_free(dev, wbo);
         errno = -serr;
         return NULL;
      }
      wbo->sync.handle = h;
   }
   /* pan_csf uses point zero to identify a binary syncobj.  WinMali keeps
    * non-exclusive BO synchronization in a timeline, so give that timeline a
    * signalled nonzero baseline from its first use.  Exclusive BOs share the
    * VM timeline and retain its zero-point initialization. */
   wbo->sync.read_point = wbo->sync.write_point =
      exclusive_vm ? 0 : WINMALI_BO_SYNC_BASE_POINT;
   wbo->escape_handle    = escape_handle;
   wbo->wddm_kmt_handle  = wddm_kmt;
   wbo->wddm_cpu_va      = wddm_cpu;
   wbo->wddm_owner_cookie = wddm_owner_cookie;
   wbo->wddm_free         = wddm_free;

   uint32_t local_handle = util_idalloc_mt_alloc(&wdev->bo_ids);
   assert(local_handle != 0);
   pan_kmod_bo_init(&wbo->base, dev, exclusive_vm, size, flags, local_handle);
   winmali_d3dkmt_log("wm: BO_ALLOC dev=%p local=%u escape=%u kmt=%u cookie=%p size=%llu cpu=%p",
                      (void *)wdev, local_handle, escape_handle, wddm_kmt,
                      wddm_owner_cookie, (unsigned long long)size, wddm_cpu);
   return &wbo->base;
}

/* A group belongs to the private kmod device/screen that created its VM.
 * Preserve that owner so a later D3D device cannot redirect this group through
 * its callback cookie. */
#define WINMALI_GROUP_VM_MAX 128
static struct {
   uint32_t group;
   uint32_t vm_id;
   struct winmali_kmod_dev *owner;
   bool used;
}
   g_group_vm[WINMALI_GROUP_VM_MAX];
static simple_mtx_t g_group_vm_lock = SIMPLE_MTX_INITIALIZER;

static void
winmali_group_vm_record(struct winmali_kmod_dev *owner,
                        uint32_t group, uint32_t vm_id)
{
   simple_mtx_lock(&g_group_vm_lock);
   for (int i = 0; i < WINMALI_GROUP_VM_MAX; i++) {
      if (!g_group_vm[i].used) {
         g_group_vm[i].group = group;
         g_group_vm[i].vm_id = vm_id;
         g_group_vm[i].owner = owner;
         g_group_vm[i].used  = true;
         break;
      }
   }
   simple_mtx_unlock(&g_group_vm_lock);
}

static bool
winmali_group_vm_lookup(uint32_t group, uint32_t *vm_id,
                        struct winmali_kmod_dev **owner)
{
   bool found = false;
   *vm_id = 0;
   *owner = NULL;
   simple_mtx_lock(&g_group_vm_lock);
   for (int i = 0; i < WINMALI_GROUP_VM_MAX; i++) {
      if (g_group_vm[i].used && g_group_vm[i].group == group) {
         *vm_id = g_group_vm[i].vm_id;
         *owner = g_group_vm[i].owner;
         found = true;
         break;
      }
   }
   simple_mtx_unlock(&g_group_vm_lock);
   return found;
}

static void
winmali_group_vm_forget(uint32_t group)
{
   simple_mtx_lock(&g_group_vm_lock);
   for (int i = 0; i < WINMALI_GROUP_VM_MAX; i++) {
      if (g_group_vm[i].used && g_group_vm[i].group == group) {
         memset(&g_group_vm[i], 0, sizeof(g_group_vm[i]));
         break;
      }
   }
   simple_mtx_unlock(&g_group_vm_lock);
}

static void
winmali_group_vm_forget_owner(struct winmali_kmod_dev *owner)
{
   simple_mtx_lock(&g_group_vm_lock);
   for (int i = 0; i < WINMALI_GROUP_VM_MAX; i++) {
      if (g_group_vm[i].used && g_group_vm[i].owner == owner) {
         winmali_d3dkmt_log("wm: stale group purged group=%u vm=%u owner=%p",
                            g_group_vm[i].group, g_group_vm[i].vm_id,
                            (void *)owner);
         memset(&g_group_vm[i], 0, sizeof(g_group_vm[i]));
      }
   }
   simple_mtx_unlock(&g_group_vm_lock);
}

void
winmali_kmod_set_wddm_alloc_ops(struct pan_kmod_dev *dev,
                                const struct winmali_wddm_alloc_ops *ops)
{
   struct winmali_kmod_dev *wdev = to_winmali_dev(dev);
   if (ops) {
      wdev->wddm_alloc = *ops;
      winmali_d3dkmt_log("wm: bridge install dev=%p cookie=%p",
                         (void *)wdev, ops->cookie);
   } else {
      winmali_d3dkmt_log("wm: bridge clear dev=%p cookie=%p",
                         (void *)wdev, wdev->wddm_alloc.cookie);
      memset(&wdev->wddm_alloc, 0, sizeof(wdev->wddm_alloc));
   }
}

static void
winmali_kmod_bo_free(struct pan_kmod_bo *bo)
{
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);
   struct winmali_kmod_dev *wdev = to_winmali_dev(bo->dev);
   uint32_t local_handle = bo->handle;
   uint64_t retire_point =
      MAX2(wbo->sync.read_point, wbo->sync.write_point);
   bool backing_quiesced = true;

   /* The Linux panthor submit ioctl retains references to every GEM object in
    * a job until that job retires.  WinMali submits an opaque GPUVA command
    * stream through WDDM instead, so dxgkrnl does not have that GEM object list
    * from which to provide the equivalent lifetime guarantee.  The sync point
    * attached by csf_attach_sync_points() is therefore the last authority on
    * whether this allocation can still be fetched by the GPU.
    *
    * Do this before destroying the BO's sync object or releasing either owned
    * or imported WDDM storage.  -EIO is a terminal fence result: the KMD has
    * failed the submission and stopped using its BOs, so retirement is still
    * complete.  Any other error does not prove quiescence; retain the backing
    * allocation in that exceptional case rather than create a GPU use-after-
    * free. */
   if (retire_point != 0) {
      int wait_err =
         syncobj_timeline_wait(wbo->sync.handle, retire_point, INT64_MAX);

      backing_quiesced = wait_err == 0 || wait_err == -EIO;
      if (!backing_quiesced) {
         winmali_d3dkmt_log("[WinMaliSync] BO_FREE retaining backing "
                            "local=%u kmt=%u sync=%u point=%llu err=%d",
                            local_handle, wbo->wddm_kmt_handle,
                            wbo->sync.handle,
                            (unsigned long long)retire_point, wait_err);
         mesa_loge("[WinMaliSync] BO_FREE retaining backing local=%u "
                   "kmt=%u sync=%u point=%llu err=%d",
                   local_handle, wbo->wddm_kmt_handle, wbo->sync.handle,
                   (unsigned long long)retire_point, wait_err);
      }
   }

   /* If the BO owns its sync handle (no exclusive_vm), tear it down. */
   if (!bo->exclusive_vm)
      syncobj_destroy(wbo->sync.handle);

   if (wbo->wddm_kmt_handle) {
      /* dxgk-backed BO: no escape KMD BO to destroy. Release the dxgk
       * allocation ONLY if we created it - an IMPORTED backbuffer belongs to
       * the D3D runtime / DWM, so freeing it would tear down their surface. */
      if (backing_quiesced && !(bo->flags & PAN_KMOD_BO_FLAG_IMPORTED)) {
         winmali_d3dkmt_log("wm: BO_FREE dev=%p local=%u kmt=%u cookie=%p size=%llu",
                            (void *)bo->dev, local_handle,
                            wbo->wddm_kmt_handle, wbo->wddm_owner_cookie,
                            (unsigned long long)bo->size);
         if (wbo->wddm_free)
            wbo->wddm_free(wbo->wddm_owner_cookie,
                           wbo->wddm_kmt_handle);
      }
   } else if (backing_quiesced) {
      WINMALI_BO_DESTROY d = { .Handle = wbo->escape_handle };
      escape_simple(WinMaliEscapeOp_BoDestroy, &d, sizeof(d));
   }

   pan_kmod_bo_cleanup(bo);
   util_idalloc_mt_free(&wdev->bo_ids, local_handle);
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
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);
   if (!wbo->escape_handle) {
      errno = ENOSYS;
      return (off_t)-1;
   }

   WINMALI_BO_MMAP_OFFSET req = { .Handle = wbo->escape_handle };
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
   (void)dev;
   /* pan_kmod tracks pending deferred BO syncs in dev->pending_bo_syncs;
    * panthor walks that list and emits one BO_SYNC ioctl. With user-mode
    * mappings unavailable on Windows (mmap is served by the KMD anyway),
    * there's nothing for the user side to flush. The KMD performs cache
    * maintenance synchronously inside BoCreate / BoMmapOffset. */
   return 0;
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
   /* A zero-timeout poll commonly returns -ETIMEDOUT while work is still in
    * flight; logging every such poll overwhelms DebugView and changes timing.
    * Preserve only failures from waits that were actually expected to block. */
   if (err && timeout_ns != 0) {
      winmali_d3dkmt_log("[WinMaliSync] BoWait failed local=%u sync=%u "
                         "chosen=%llu readOnly=%u timeout=%lld err=%d",
                         bo->handle, wbo->sync.handle,
                         (unsigned long long)point,
                         for_read_only_access ? 1u : 0u,
                         (long long)timeout_ns, err);
      mesa_loge("[WinMaliSync] BoWait failed local=%u sync=%u chosen=%llu "
                "readOnly=%u timeout=%lld err=%d",
                bo->handle, wbo->sync.handle, (unsigned long long)point,
                for_read_only_access ? 1u : 0u, (long long)timeout_ns, err);
   }
   return err == 0;
}

static void
winmali_kmod_bo_set_label(struct pan_kmod_dev *dev,
                          struct pan_kmod_bo *bo, const char *label)
{
   (void)dev;
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);

   /* A dxgk allocation is not present in WinMaliBo's private escape table.
    * There is no kernel BO object to label on this path. */
   if (!wbo->escape_handle)
      return;

   size_t len = strnlen(label, WINMALI_BO_LABEL_MAX - 1);
   size_t header = sizeof(WINMALI_ESCAPE_HEADER);
   size_t payload = sizeof(WINMALI_BO_SET_LABEL);
   size_t lab_off = WINMALI_ALIGN_UP(header + payload, WINMALI_TAIL_ALIGN);
   size_t total = WINMALI_ALIGN_UP(lab_off + len + 1, WINMALI_TAIL_ALIGN);

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
   args->Handle = wbo->escape_handle;
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
   if (bo->flags & PAN_KMOD_BO_FLAG_NO_MMAP)
      return MAP_FAILED;

   /* dxgk-native BOs are already CPU-mapped for their lifetime via pfnLockCb
    * (captured at alloc). Hand back that persistent VA - no escape. */
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);
   if (wbo->wddm_cpu_va)
      return wbo->wddm_cpu_va;
   if (wbo->wddm_kmt_handle) {
      errno = ENOSYS;
      return MAP_FAILED;
   }

   WINMALI_BO_MAP_CPU req = {
      .Handle = wbo->escape_handle,
      .Prot   = (prot & PROT_READ ? 1 : 0) | (prot & PROT_WRITE ? 2 : 0),
   };
   int err = escape_simple(WinMaliEscapeOp_BoMapCpu, &req, sizeof(req));
   if (err) {
      errno = -err;
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
   struct winmali_kmod_bo *wbo = to_winmali_bo(bo);
   if (!wbo->escape_handle)
      return -ENOTSUP;

   WINMALI_PRESENT_TO_HDC req = {
      .Hdc      = (uint64_t)(uintptr_t)hdc,
      .BoHandle = wbo->escape_handle,
      .Format   = format,
      .Width    = width,
      .Height   = height,
      .Stride   = stride,
      .DstX     = dst_x,
      .DstY     = dst_y,
   };
   return escape_simple(WinMaliEscapeOp_PresentToHdc, &req, sizeof(req));
}

struct pan_kmod_bo *
winmali_kmod_bo_from_allocation(struct pan_kmod_dev *dev,
                                uint32_t kmt_allocation_handle,
                                uint64_t size,
                                uint32_t flags,
                                void *cpu_va)
{
   assert(kmt_allocation_handle != 0);
   struct winmali_kmod_dev *wdev = to_winmali_dev(dev);
   struct winmali_kmod_bo *wbo = pan_kmod_dev_alloc(dev, sizeof(*wbo));
   if (!wbo) {
      errno = ENOMEM;
      return NULL;
   }

   /* The allocation is an existing dxgk allocation (e.g. a DXGI swap-chain
    * backbuffer created by the D3D runtime). Address it via GpuMmu (vm_bind ->
    * pfnMapGpuVirtualAddressCb) - NO BoFromAllocation escape, so no cross-device
    * pfnEscapeCb round-trip (which crashed d3d11's escape telemetry) and no
    * page-alias handshake. The KMT handle remains the native allocation
    * identity while base.handle is a dense Mesa-local identity. */
   uint32_t h = 0;
   int serr = syncobj_create_timeline(&h, true);
   if (serr) {
      pan_kmod_dev_free(dev, wbo);
      errno = -serr;
      return NULL;
   }
   wbo->sync.handle = h;
   /* This is a WinMali-owned timeline, not panthor's temporary binary
    * dma-buf sync container.  Point one is already signalled at creation. */
   wbo->sync.read_point = wbo->sync.write_point =
      WINMALI_BO_SYNC_BASE_POINT;
   wbo->escape_handle    = 0;
   wbo->wddm_kmt_handle  = kmt_allocation_handle;
   /* Borrow the mapping held by the D3D resource's lifetime LockCb. The
    * frontend releases the pipe resource before UnlockCb, so this pointer
    * remains valid for every panfrost CPU-map operation on this BO. */
   wbo->wddm_cpu_va      = cpu_va;
   wbo->wddm_owner_cookie = wdev->wddm_alloc.cookie;
   wbo->wddm_free         = NULL;   /* imported allocation is runtime-owned */

   /* Mark the BO as imported so callers don't try to re-export it. Use a
    * dense Mesa-local handle; the KMT allocation identity stays separate. */
   uint32_t local_handle = util_idalloc_mt_alloc(&wdev->bo_ids);
   assert(local_handle != 0);
   pan_kmod_bo_init(&wbo->base, dev, NULL, size,
                    flags | PAN_KMOD_BO_FLAG_IMPORTED, local_handle);

   /* This helper bypasses pan_kmod_bo_alloc(), whose wrapper normally inserts
    * a newly-created BO into handle_to_bo. Register it here so the matching
    * pan_kmod_bo_put() can clear the slot and destroy it safely. */
   struct pan_kmod_bo **slot =
      util_sparse_array_get(&dev->handle_to_bo.array, local_handle);
   if (!slot) {
      winmali_kmod_bo_free(&wbo->base);
      errno = ENOMEM;
      return NULL;
   }
   assert(*slot == NULL);
   *slot = &wbo->base;

   winmali_d3dkmt_log("wm: BO_IMPORT dev=%p local=%u kmt=%u size=%llu cpu=%p",
                      (void *)wdev, local_handle, kmt_allocation_handle,
                      (unsigned long long)size, cpu_va);
   return &wbo->base;
}

/* --------------------------------------------------------------------- */
/*  Virtual memory                                                       */
/* --------------------------------------------------------------------- */

static struct pan_kmod_vm *
winmali_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                       uint64_t va_start, uint64_t va_range)
{
   winmali_d3dkmt_log("wm: VM_CREATE enter dev=%p flags=0x%x start=0x%llx range=0x%llx",
                      dev, flags, (unsigned long long)va_start,
                      (unsigned long long)va_range);
   struct winmali_kmod_vm *wvm = pan_kmod_dev_alloc(dev, sizeof(*wvm));
   if (!wvm) {
      winmali_d3dkmt_log("wm: VM_CREATE allocation failed");
      errno = ENOMEM;
      return NULL;
   }
   winmali_d3dkmt_log("wm: VM_CREATE allocated wvm=%p", wvm);

   WINMALI_VM_CREATE req = {
      .Flags = 0,
      .UserVaRange = va_range,
   };
   winmali_d3dkmt_log("wm: VM_CREATE escape enter requested_range=0x%llx",
                      (unsigned long long)req.UserVaRange);
   int err = escape_simple(WinMaliEscapeOp_VmCreate, &req, sizeof(req));
   winmali_d3dkmt_log("wm: VM_CREATE escape exit err=%d id=%u granted=0x%llx",
                      err, req.Id, (unsigned long long)req.UserVaRange);
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
      winmali_d3dkmt_log("wm: VM_CREATE syncobj create enter vm=%u", req.Id);
      int serr = syncobj_create_timeline(&h, false);
      winmali_d3dkmt_log("wm: VM_CREATE syncobj create exit err=%d handle=%u",
                         serr, h);
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
      wvm->sync.point  = 0;
      simple_mtx_init(&wvm->sync.lock, mtx_plain);
   }

   pan_kmod_vm_init(&wvm->base, dev, req.Id, flags,
                    PAN_PGSIZE_4K | PAN_PGSIZE_2M);
   winmali_d3dkmt_log("wm: VM_CREATE exit success vm=%u base=%p", req.Id,
                      &wvm->base);
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
   struct winmali_kmod_dev *wdev = to_winmali_dev(wvm->base.dev);
   const struct winmali_wddm_alloc_ops *bridge = &wdev->wddm_alloc;
   void *cookie = bridge->cookie;

   /* Pre-scan: every MAP target must be a dxgk-backed BO, else not routable.
    * Done before any work so falling back to escape is clean (no double-map). */
   for (uint32_t i = 0; i < op_count; i++) {
      if (ops[i].type != PAN_KMOD_VM_OP_TYPE_MAP)
         continue;
      if (ops[i].map.bo->dev != wvm->base.dev) {
         winmali_d3dkmt_log("wm: VM_BIND owner mismatch vmdev=%p bodev=%p bo=%u",
                            (void *)wvm->base.dev, (void *)ops[i].map.bo->dev,
                            ops[i].map.bo->handle);
         return -EXDEV;
      }
      if (to_winmali_bo(ops[i].map.bo)->wddm_kmt_handle == 0)
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
         uint32_t map_flags = 0;
         /* Match panthor's protection contract: shader/code BOs are
          * read-only executable; every other BO is writable non-executable.
          * Do not pass raw pan_kmod flag bits across the bridge because their
          * layout is unrelated to either WDDM or WINMALI_VM_BIND_OP flags. */
         if (op->map.bo->flags & PAN_KMOD_BO_FLAG_EXECUTABLE)
            map_flags |= WINMALI_WDDM_MAP_READONLY |
                         WINMALI_WDDM_MAP_EXECUTE;
         int merr = bridge->map_va(cookie,
                                   to_winmali_bo(op->map.bo)->wddm_kmt_handle,
                                   (uint64_t)op->map.bo_offset,
                                   op->va.size, map_flags, &va);
         if (merr) { err = merr; break; }
         op->va.start = va;
         break;
      }
      case PAN_KMOD_VM_OP_TYPE_UNMAP:
         bridge->unmap_va(cookie, op->va.start, op->va.size);
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
winmali_kmod_vm_bind_impl(struct pan_kmod_vm *vm,
                          enum pan_kmod_vm_op_mode mode,
                          struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   struct winmali_kmod_vm *wvm = to_winmali_vm(vm);
   struct winmali_kmod_dev *wdev = to_winmali_dev(vm->dev);
   const bool track_activity =
      vm->flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY;
   const bool auto_va = vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA;
   const bool async = mode == PAN_KMOD_VM_OP_MODE_ASYNC;
   uint32_t flags = async ? WINMALI_VM_BIND_FLAG_ASYNC : 0;

   if (op_count == 0)
      return 0;

   /* dxgk GpuMmu addressing is the default: every dxgk-backed BO is mapped via
    * pfnMapGpuVirtualAddressCb. -ENOTSUP means the batch has a non-dxgk BO
    * (only possible in a non-WDDM build), which routes to escape VmBind below. */
   if (wdev->wddm_alloc.map_va != NULL) {
      int rc = winmali_vm_bind_dxgk(wvm, ops, op_count);
      if (rc != -ENOTSUP)
         return rc;
   }

   /* The escape VM_BIND path addresses objects in WinMaliBo's private table.
    * A dxgk-backed allocation has no entry there, so never substitute its
    * Mesa-local or KMT identity into this packet. */
   for (uint32_t i = 0; i < op_count; i++) {
      if (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP &&
          !to_winmali_bo(ops[i].map.bo)->escape_handle)
         return -ENOTSUP;
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
         out->Flags = WINMALI_VM_BIND_OP_TYPE_MAP;
         if (op->map.bo->flags & PAN_KMOD_BO_FLAG_EXECUTABLE)
            out->Flags |= WINMALI_VM_BIND_OP_MAP_READONLY;
         else
            out->Flags |= WINMALI_VM_BIND_OP_MAP_NOEXEC;
         if (op->map.bo->flags & PAN_KMOD_BO_FLAG_GPU_UNCACHED)
            out->Flags |= WINMALI_VM_BIND_OP_MAP_UNCACHED;
         out->BoHandle = to_winmali_bo(op->map.bo)->escape_handle;
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

static int
winmali_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                     struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   if (mode != PAN_KMOD_VM_OP_MODE_DEFER_TO_NEXT_IDLE_POINT)
      return winmali_kmod_vm_bind_impl(vm, mode, ops, op_count);

   if (!(vm->flags & PAN_KMOD_VM_FLAG_TRACK_ACTIVITY)) {
      mesa_loge("PAN_KMOD_VM_OP_MODE_DEFER_TO_NEXT_IDLE_POINT requires "
                "PAN_KMOD_VM_FLAG_TRACK_ACTIVITY");
      return -EINVAL;
   }

   if (op_count == 0)
      return 0;

   struct winmali_kmod_vm *wvm = to_winmali_vm(vm);

   /* panthor implements DEFER_TO_NEXT_IDLE_POINT as an asynchronous bind that
    * waits on the VM timeline point current when the bind is queued.  WDDM's
    * MapGpuVirtualAddress callback is synchronous and has no equivalent wait
    * list, so provide the same ordering by holding the VM submission lock,
    * waiting for that captured point, and only then issuing the bind/unbind.
    * Holding the lock prevents a new submission from entering the retiring VA
    * range between the wait and the unmap.
    *
    * -EIO is a terminal completion for the captured point: the KMD has failed
    * and retired that work.  Other errors do not establish an idle point and
    * must leave the mapping intact. */
   uint64_t idle_point = panthor_kmod_vm_sync_lock(vm);
   int wait_err =
      syncobj_timeline_wait(wvm->sync.handle, idle_point, INT64_MAX);
   if (wait_err != 0 && wait_err != -EIO) {
      panthor_kmod_vm_sync_unlock(vm, idle_point);
      winmali_d3dkmt_log("[WinMaliSync] VM deferred bind blocked vm=%u "
                         "sync=%u point=%llu err=%d",
                         wvm->vm_id, wvm->sync.handle,
                         (unsigned long long)idle_point, wait_err);
      mesa_loge("[WinMaliSync] VM deferred bind blocked vm=%u sync=%u "
                "point=%llu err=%d",
                wvm->vm_id, wvm->sync.handle,
                (unsigned long long)idle_point, wait_err);
      return wait_err;
   }

   int ret = winmali_kmod_vm_bind_impl(
      vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, ops, op_count);
   panthor_kmod_vm_sync_unlock(vm, idle_point);
   return ret;
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
winmali_ioctl_group_create(struct winmali_kmod_dev *owner, void *arg)
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
      winmali_group_vm_record(owner, body->GroupHandle, gc->vm_id);
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
winmali_ioctl_group_submit(struct winmali_kmod_dev *caller, void *arg)
{
   struct drm_panthor_group_submit *gs = arg;
   const struct drm_panthor_queue_submit *qsrc =
      (const struct drm_panthor_queue_submit *)(uintptr_t)gs->queue_submits.array;
   uint32_t nqs = gs->queue_submits.count;
   uint32_t total_syncs = 0, i;
   uint32_t vm_id = 0;
   struct winmali_kmod_dev *owner = NULL;
   bool have_owner = winmali_group_vm_lookup(gs->group_handle, &vm_id, &owner);

   if (caller != NULL && have_owner && caller != owner) {
      winmali_d3dkmt_log("wm: GROUP_SUBMIT owner mismatch group=%u caller=%p owner=%p",
                         gs->group_handle, (void *)caller, (void *)owner);
      return -EXDEV;
   }
   if (owner == NULL)
      owner = caller;

   /* Submit through the callback table owned by this group's private screen,
    * never through whichever D3D device happened to install callbacks last.
    * The whole sync list travels with the DMA private data. The KMD worker
    * performs WAIT operations before the real CS and SIGNAL operations after
    * terminal completion, preserving Mesa's BO lifetime without blocking the
    * caller in pfnSubmitCommandCb. */
   if (owner != NULL && owner->wddm_alloc.render != NULL) {
      const struct winmali_wddm_alloc_ops *bridge = &owner->wddm_alloc;
      int rerr = 0;
      uint32_t queued = 0;

      for (i = 0; i < nqs; ++i) {
         if (qsrc[i].stream_addr == 0 || qsrc[i].stream_size == 0)
            continue;
         rerr = bridge->render(bridge->cookie, vm_id,
                               qsrc[i].stream_addr, qsrc[i].stream_size,
                               (const void *)(uintptr_t)qsrc[i].syncs.array,
                               qsrc[i].syncs.count);
         if (rerr)
            break;
         ++queued;
      }

      if (!rerr && queued != 0)
         return 0;
      if (!rerr && queued == 0)
         rerr = -EINVAL;
      winmali_d3dkmt_log("wm: dxgk render submit failed (%d); "
                         "queued=%u", rerr, queued);
      /* This screen's allocations use dxgk GPUVAs. They do not exist in the
       * escape VM, so retrying this stream through GroupSubmit would execute
       * it against the wrong page tables and deterministically MMU-fault. */
      return rerr;
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
   if (err != 0)
      winmali_d3dkmt_log("wm: GROUP_SUBMIT escape failed (%d)", err);
   free(buf);
   return err;
}

int
winmali_kmod_ioctl_dev(struct pan_kmod_dev *dev,
                       unsigned long request, void *arg)
{
   if (!winmali_d3dkmt_is_open())
      return -ENODEV;

   uint32_t op = panthor_ioctl_to_escape_op(request);
   if (op == UINT32_MAX)
      return -ENOTTY;
   struct winmali_kmod_dev *owner = dev ? to_winmali_dev(dev) : NULL;

   /* Ops whose drm struct carries user-pointer array tails or a divergent
    * field order need translation into the WinMali escape wire format. */
   switch (op) {
   case WinMaliEscapeOp_GroupCreate:
      return winmali_ioctl_group_create(owner, arg);
   case WinMaliEscapeOp_GroupSubmit:
      return winmali_ioctl_group_submit(owner, arg);
   case WinMaliEscapeOp_TilerHeapCreate:
      return winmali_ioctl_tiler_heap_create(arg);
   default:
      break;
   }

   /* The struct size is in the high bits of the ioctl encoding. _IOWR
    * puts size in bits 16..29. The remaining ops (group/heap destroy,
    * group get-state) are layout-compatible with their WINMALI_* structs. */
   size_t payload_size = (size_t)((request >> 16) & 0x3fff);
   if (payload_size == 0)
      return -EINVAL;

   int err = escape_simple(op, arg, payload_size);
   if (err == 0 && op == WinMaliEscapeOp_GroupDestroy) {
      const struct drm_panthor_group_destroy *gd = arg;
      winmali_group_vm_forget(gd->group_handle);
   }
   return err;
}

int
winmali_kmod_ioctl(unsigned long request, void *arg)
{
   return winmali_kmod_ioctl_dev(NULL, request, arg);
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

   uint64_t attached_point = sync_point;

   /* Exclusive BOs share the VM timeline directly. Other BOs retain their
    * owned timeline and receive a nonblocking transfer of the VM fence. */
   if (bo->exclusive_vm) {
      assert(sync_handle ==
             to_winmali_vm(bo->exclusive_vm)->sync.handle);
   } else {
      attached_point = MAX2(wbo->sync.read_point, wbo->sync.write_point) + 1;
      int ret = syncobj_transfer(wbo->sync.handle, attached_point,
                                 sync_handle, sync_point);
      if (ret)
         return ret;
   }

   if (attached_point > wbo->sync.read_point)
      wbo->sync.read_point = attached_point;
   if (written && attached_point > wbo->sync.write_point)
      wbo->sync.write_point = attached_point;

   return 0;
}

int
panthor_kmod_bo_get_sync_point(struct pan_kmod_bo *bo, uint32_t *sync_handle,
                               uint64_t *sync_point, bool read_only)
{
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

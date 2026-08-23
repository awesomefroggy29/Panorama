/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 */

#ifdef _WIN32

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#include <windows.h>

#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

#include <d3dkmthk.h>

#include "WinMaliEscape.h"
#include "WinMaliDiagnostic.h"
#include "WinMaliWddmBridge.h"
#include "winmali_d3dkmt.h"

/* gdi32 thunk pointers + the singleton handle pair. */
typedef NTSTATUS(APIENTRY *PFN_OpenAdapterFromLuid)(D3DKMT_OPENADAPTERFROMLUID *);
typedef NTSTATUS(APIENTRY *PFN_CloseAdapter)(const D3DKMT_CLOSEADAPTER *);
typedef NTSTATUS(APIENTRY *PFN_CreateDevice)(D3DKMT_CREATEDEVICE *);
typedef NTSTATUS(APIENTRY *PFN_DestroyDevice)(const D3DKMT_DESTROYDEVICE *);
typedef NTSTATUS(APIENTRY *PFN_Escape)(const D3DKMT_ESCAPE *);
typedef NTSTATUS(APIENTRY *PFN_EnumAdapters2)(D3DKMT_ENUMADAPTERS2 *);

static struct {
   HMODULE       gdi32;
   PFN_OpenAdapterFromLuid open_adapter;
   PFN_CloseAdapter        close_adapter;
   PFN_CreateDevice        create_device;
   PFN_DestroyDevice       destroy_device;
   PFN_Escape              escape;
   PFN_EnumAdapters2       enum_adapters2;

   D3DKMT_HANDLE adapter;
   D3DKMT_HANDLE device;
   bool          open;
   unsigned      refs;
} g_wm;

static SRWLOCK g_wm_lock = SRWLOCK_INIT;

static void winmali_log_sink(enum mesa_log_level level, const char *tag,
                             const char *msg);

static int
escape_send(D3DKMT_HANDLE adapter, D3DKMT_HANDLE device, void *buf, uint32_t size)
{
   D3DKMT_ESCAPE esc;
   memset(&esc, 0, sizeof(esc));
   esc.hAdapter = adapter;
   esc.hDevice  = device;
   esc.Type     = D3DKMT_ESCAPE_DRIVERPRIVATE;
   esc.pPrivateDriverData    = buf;
   esc.PrivateDriverDataSize = size;

   NTSTATUS st = g_wm.escape(&esc);
   if (st != STATUS_SUCCESS)
      return -EIO;

   const WINMALI_ESCAPE_HEADER *hdr = (const WINMALI_ESCAPE_HEADER *)buf;
   if (hdr->Magic != WINMALI_ESCAPE_MAGIC)
      return -EPROTO;

   /* Status is already in -errno form by ABI convention. */
   return hdr->Status;
}

/* Probe one (adapter, device) pair for our handshake op. Records the
 * pair on g_wm and returns 0 if it's the WinMali KMD; otherwise returns
 * -errno without touching g_wm. */
static int
probe(D3DKMT_HANDLE adapter, D3DKMT_HANDLE device)
{
   struct {
      WINMALI_ESCAPE_HEADER hdr;
      WINMALI_OPEN_DEVICE   body;
   } buf;
   memset(&buf, 0, sizeof(buf));
   buf.hdr.Magic       = WINMALI_ESCAPE_MAGIC;
   buf.hdr.AbiMajor    = WINMALI_ABI_MAJOR;
   buf.hdr.AbiMinor    = WINMALI_ABI_MINOR;
   buf.hdr.Op          = WinMaliEscapeOp_OpenDevice;
   buf.hdr.PayloadSize = sizeof(buf);
   buf.body.UmdAbiMajor = WINMALI_ABI_MAJOR;
   buf.body.UmdAbiMinor = WINMALI_ABI_MINOR;

   int err = escape_send(adapter, device, &buf, sizeof(buf));
   if (err < 0)
      return err;

   /* Match. Stash the singleton. */
   g_wm.adapter = adapter;
   g_wm.device  = device;
   return 0;
}

/* Caller holds g_wm_lock exclusively. */
static void
winmali_d3dkmt_reset_locked(void)
{
   if (g_wm.open)
      mesa_log_set_extra_sink(NULL);
   if (g_wm.device && g_wm.destroy_device) {
      D3DKMT_DESTROYDEVICE dd = { 0 };
      dd.hDevice = g_wm.device;
      g_wm.destroy_device(&dd);
   }
   if (g_wm.adapter && g_wm.close_adapter) {
      D3DKMT_CLOSEADAPTER ca = { g_wm.adapter };
      g_wm.close_adapter(&ca);
   }
   if (g_wm.gdi32)
      FreeLibrary(g_wm.gdi32);
   memset(&g_wm, 0, sizeof(g_wm));
}

int
winmali_d3dkmt_open(void)
{
   AcquireSRWLockExclusive(&g_wm_lock);
   if (g_wm.open) {
      ++g_wm.refs;
      ReleaseSRWLockExclusive(&g_wm_lock);
      return 0;
   }

   g_wm.gdi32 = LoadLibraryW(L"gdi32.dll");
   if (!g_wm.gdi32) {
      ReleaseSRWLockExclusive(&g_wm_lock);
      return -ENOENT;
   }

#define RESOLVE(slot, sym, pfn_type) do {                                    \
   g_wm.slot = (pfn_type)GetProcAddress(g_wm.gdi32, "D3DKMT" #sym);          \
   if (!g_wm.slot) {                                                        \
      winmali_d3dkmt_reset_locked();                                        \
      ReleaseSRWLockExclusive(&g_wm_lock);                                  \
      return -ENOSYS;                                                       \
   }                                                                         \
} while (0)
   RESOLVE(open_adapter,    OpenAdapterFromLuid, PFN_OpenAdapterFromLuid);
   RESOLVE(close_adapter,   CloseAdapter,        PFN_CloseAdapter);
   RESOLVE(create_device,   CreateDevice,        PFN_CreateDevice);
   RESOLVE(destroy_device,  DestroyDevice,       PFN_DestroyDevice);
   RESOLVE(escape,          Escape,              PFN_Escape);
   RESOLVE(enum_adapters2,  EnumAdapters2,       PFN_EnumAdapters2);
#undef RESOLVE

   D3DKMT_ENUMADAPTERS2 info;
   memset(&info, 0, sizeof(info));
   /* First call: probe required buffer size. */
   NTSTATUS st = g_wm.enum_adapters2(&info);
   if (st != STATUS_SUCCESS && info.NumAdapters == 0) {
      winmali_d3dkmt_reset_locked();
      ReleaseSRWLockExclusive(&g_wm_lock);
      return -ENODEV;
   }

   /* +4 slop in case adapters appear between probe and fill. */
   D3DKMT_ADAPTERINFO *adapters = (D3DKMT_ADAPTERINFO *)calloc(
      (size_t)info.NumAdapters + 4, sizeof(D3DKMT_ADAPTERINFO));
   if (!adapters) {
      winmali_d3dkmt_reset_locked();
      ReleaseSRWLockExclusive(&g_wm_lock);
      return -ENOMEM;
   }
   info.pAdapters    = adapters;
   info.NumAdapters += 4;
   st = g_wm.enum_adapters2(&info);
   if (st != STATUS_SUCCESS) {
      free(adapters);
      winmali_d3dkmt_reset_locked();
      ReleaseSRWLockExclusive(&g_wm_lock);
      return -ENODEV;
   }

   int last_err = -ENODEV;
   for (UINT i = 0; i < info.NumAdapters; ++i) {
      D3DKMT_OPENADAPTERFROMLUID oa;
      memset(&oa, 0, sizeof(oa));
      oa.AdapterLuid = adapters[i].AdapterLuid;
      if (g_wm.open_adapter(&oa) != STATUS_SUCCESS)
         continue;

      D3DKMT_CREATEDEVICE cd;
      memset(&cd, 0, sizeof(cd));
      cd.hAdapter = oa.hAdapter;
      if (g_wm.create_device(&cd) != STATUS_SUCCESS) {
         D3DKMT_CLOSEADAPTER ca = { oa.hAdapter };
         g_wm.close_adapter(&ca);
         continue;
      }

      int err = probe(oa.hAdapter, cd.hDevice);
      if (err == 0) {
         /* Match. Don't free the handle pair. */
         free(adapters);
         g_wm.open = true;
         g_wm.refs = 1;
         /* From here the KMD's UART can carry our logs: register the
          * mesa_log mirror and announce ourselves so the serial shows
          * which process bound the adapter. */
         mesa_log_set_extra_sink(winmali_log_sink);
         winmali_d3dkmt_log("wm: adapter bound (hAdapter=0x%x hDevice=0x%x)",
                            g_wm.adapter, g_wm.device);
         ReleaseSRWLockExclusive(&g_wm_lock);
         return 0;
      }
      last_err = err;

      /* Not ours - tear down and try the next. */
      D3DKMT_DESTROYDEVICE dd = { 0 };
      dd.hDevice = cd.hDevice;
      g_wm.destroy_device(&dd);
      D3DKMT_CLOSEADAPTER ca = { oa.hAdapter };
      g_wm.close_adapter(&ca);
   }

   free(adapters);
   winmali_d3dkmt_reset_locked();
   ReleaseSRWLockExclusive(&g_wm_lock);
   return last_err;
}

void
winmali_d3dkmt_close(void)
{
   AcquireSRWLockExclusive(&g_wm_lock);
   if (g_wm.refs > 1) {
      --g_wm.refs;
      ReleaseSRWLockExclusive(&g_wm_lock);
      return;
   }
   winmali_d3dkmt_reset_locked();
   ReleaseSRWLockExclusive(&g_wm_lock);
}

int
winmali_d3dkmt_escape(void *buf, size_t size)
{
   if (!g_wm.open)
      return -ENODEV;
   if (size > UINT32_MAX)
      return -EINVAL;
   return escape_send(g_wm.adapter, g_wm.device, buf, (uint32_t)size);
}

bool
winmali_d3dkmt_is_open(void)
{
   return g_wm.open;
}

void
winmali_d3dkmt_diag_record(uint32_t batch_sequence,
                           uint64_t stream_gpu_va,
                           const void *stream_cpu,
                           uint32_t stream_size)
{
   struct {
      WINMALI_ESCAPE_HEADER hdr;
      WINMALI_DIAG_CONTROL control;
   } *buf;
   uint32_t captured;

   if (!g_wm.open || !stream_cpu || !stream_size)
      return;

   buf = (void *)calloc(1, sizeof(*buf));
   if (!buf)
      return;

   captured = stream_size < WINMALI_DIAG_STREAM_MAX
                 ? stream_size : WINMALI_DIAG_STREAM_MAX;
   buf->hdr.Magic = WINMALI_ESCAPE_MAGIC;
   buf->hdr.AbiMajor = WINMALI_ABI_MAJOR;
   buf->hdr.AbiMinor = WINMALI_ABI_MINOR;
   buf->hdr.Op = WinMaliEscapeOp_Diagnostic;
   buf->hdr.PayloadSize = sizeof(*buf);
   buf->control.Command = WinMaliDiagCommand_RecordUmd;
   buf->control.Record.Magic = WINMALI_DIAG_MAGIC;
   buf->control.Record.Version = WINMALI_DIAG_VERSION;
   buf->control.Record.HeaderSize =
      (uint16_t)offsetof(WINMALI_DIAG_RECORD, Stream);
   buf->control.Record.RecordSize = sizeof(buf->control.Record);
   buf->control.Record.BatchSequence = batch_sequence;
   buf->control.Record.StreamGpuVa = stream_gpu_va;
   buf->control.Record.StreamSize = stream_size;
   buf->control.Record.StreamCaptured = captured;
   memcpy(buf->control.Record.Stream, stream_cpu, captured);

   /* Recorder availability must never influence rendering. */
   (void)winmali_d3dkmt_escape(buf, sizeof(*buf));
   free(buf);
}

/* ---- UMD -> UART log mirroring (WinMaliEscapeOp_DebugLog) ------------- */

/* Muted after the first -EOPNOTSUPP so an old KMD costs one escape total. */
static bool g_log_unsupported;
/* Reentrancy guard: a failing DebugLog escape must not log its own
 * failure back into this path (escape_send errors go via mesa_log too). */
static _Thread_local bool g_in_log;

void
winmali_d3dkmt_log(const char *fmt, ...)
{
   struct {
      WINMALI_ESCAPE_HEADER hdr;
      WINMALI_DEBUG_LOG     body;
      char                  text[WINMALI_DEBUG_LOG_MAX];
   } buf;
   va_list va;
   int len;

   if (!g_wm.open || g_log_unsupported || g_in_log)
      return;

   memset(&buf, 0, sizeof(buf.hdr) + sizeof(buf.body));
   va_start(va, fmt);
   len = vsnprintf(buf.text, sizeof(buf.text), fmt, va);
   va_end(va);
   if (len < 0)
      return;
   if (len > (int)sizeof(buf.text))
      len = (int)sizeof(buf.text);

   buf.hdr.Magic       = WINMALI_ESCAPE_MAGIC;
   buf.hdr.AbiMajor    = WINMALI_ABI_MAJOR;
   buf.hdr.AbiMinor    = WINMALI_ABI_MINOR;
   buf.hdr.Op          = WinMaliEscapeOp_DebugLog;
   buf.hdr.PayloadSize = sizeof(buf);
   buf.body.TextLen    = (uint32_t)len;
   /* text[] has alignment 1, so it starts right after body. */
   buf.body.TextOffset = (uint32_t)(sizeof(buf.hdr) + sizeof(buf.body));

   g_in_log = true;
   int err = escape_send(g_wm.adapter, g_wm.device, &buf, sizeof(buf));
   g_in_log = false;

   if (err == -EOPNOTSUPP)
      g_log_unsupported = true;
}

/* mesa_log extra sink: every mesa_loge/logw/logi/logd in the process,
 * already formatted "tag: level: text". */
static void
winmali_log_sink(enum mesa_log_level level, const char *tag, const char *msg)
{
   (void)level;
   (void)tag;
   winmali_d3dkmt_log("%s", msg);
}

/* ---- drm_syncobj wiring (WinMaliEscapeOp_SyncObj*) -------------------- */
/* pan_context.c / pan_csf.c call libdrm's drmSyncobj* directly (not via the
 * pan_kmod backend). The winmali_drm_compat.h shims route here. Binary
 * syncobjs: WinMali create takes Flags=0 (binary) + InitialState. */

int
winmali_d3dkmt_syncobj_create(uint32_t drm_flags, uint32_t *out_handle)
{
   struct {
      WINMALI_ESCAPE_HEADER hdr;
      WINMALI_SYNC_OBJ_CREATE body;
   } buf;
   int err;

   if (!g_wm.open)
      return -ENODEV;

   memset(&buf, 0, sizeof(buf));
   buf.hdr.Magic       = WINMALI_ESCAPE_MAGIC;
   buf.hdr.AbiMajor    = WINMALI_ABI_MAJOR;
   buf.hdr.AbiMinor    = WINMALI_ABI_MINOR;
   buf.hdr.Op          = WinMaliEscapeOp_SyncObjCreate;
   buf.hdr.PayloadSize = sizeof(buf);
   buf.body.Flags      = 0; /* binary */
   /* DRM_SYNCOBJ_CREATE_SIGNALED == (1<<0). */
   buf.body.InitialState = (drm_flags & 0x1u) ? 1u : 0u;

   err = escape_send(g_wm.adapter, g_wm.device, &buf, sizeof(buf));
   if (err)
      return err;

   *out_handle = buf.body.Handle;
   return 0;
}

int
winmali_d3dkmt_syncobj_destroy(uint32_t handle)
{
   struct {
      WINMALI_ESCAPE_HEADER hdr;
      WINMALI_SYNC_OBJ_DESTROY body;
   } buf;

   if (!g_wm.open)
      return -ENODEV;

   memset(&buf, 0, sizeof(buf));
   buf.hdr.Magic       = WINMALI_ESCAPE_MAGIC;
   buf.hdr.AbiMajor    = WINMALI_ABI_MAJOR;
   buf.hdr.AbiMinor    = WINMALI_ABI_MINOR;
   buf.hdr.Op          = WinMaliEscapeOp_SyncObjDestroy;
   buf.hdr.PayloadSize = sizeof(buf);
   buf.body.Handle     = handle;

   return escape_send(g_wm.adapter, g_wm.device, &buf, sizeof(buf));
}

int
winmali_d3dkmt_syncobj_signal(uint32_t handle, uint64_t point)
{
   /* WINMALI_SYNC_OBJ_SIGNAL carries a uint64_t, so a plain
    * struct { hdr; body; } 8-aligns the body to offset 40 while the KMD
    * reads it at sizeof(WINMALI_ESCAPE_HEADER) == 36 - the KMD then saw
    * Handle=0 (header padding) and returned -ENOENT. Same bug class as
    * TILER_HEAP_CREATE (v12); this instance hid until the first draw
    * actually completed (2026-07-12) because the signal path only runs
    * after a successful submit. Place the body at exactly sizeof(hdr). */
   uint8_t buf[sizeof(WINMALI_ESCAPE_HEADER) + sizeof(WINMALI_SYNC_OBJ_SIGNAL)];
   WINMALI_ESCAPE_HEADER hdr;
   WINMALI_SYNC_OBJ_SIGNAL body;

   if (!g_wm.open)
      return -ENODEV;

   memset(&hdr, 0, sizeof(hdr));
   memset(&body, 0, sizeof(body));
   hdr.Magic       = WINMALI_ESCAPE_MAGIC;
   hdr.AbiMajor    = WINMALI_ABI_MAJOR;
   hdr.AbiMinor    = WINMALI_ABI_MINOR;
   hdr.Op          = WinMaliEscapeOp_SyncObjSignal;
   hdr.PayloadSize = sizeof(buf);
   body.Handle     = handle;
   body.Point      = point;
   memcpy(buf, &hdr, sizeof(hdr));
   memcpy(buf + sizeof(hdr), &body, sizeof(body));

   return escape_send(g_wm.adapter, g_wm.device, buf, sizeof(buf));
}

int
winmali_d3dkmt_syncobj_transfer(uint32_t dst, uint64_t dst_point,
                                uint32_t src, uint64_t src_point)
{
   uint8_t buf[sizeof(WINMALI_ESCAPE_HEADER) +
               sizeof(WINMALI_SYNC_OBJ_TRANSFER)];
   WINMALI_ESCAPE_HEADER hdr;
   WINMALI_SYNC_OBJ_TRANSFER body;

   if (!g_wm.open)
      return -ENODEV;

   memset(&hdr, 0, sizeof(hdr));
   memset(&body, 0, sizeof(body));
   hdr.Magic       = WINMALI_ESCAPE_MAGIC;
   hdr.AbiMajor    = WINMALI_ABI_MAJOR;
   hdr.AbiMinor    = WINMALI_ABI_MINOR;
   hdr.Op          = WinMaliEscapeOp_SyncObjTransfer;
   hdr.PayloadSize = sizeof(buf);
   body.DstHandle  = dst;
   body.SrcHandle  = src;
   body.DstPoint   = dst_point;
   body.SrcPoint   = src_point;
   memcpy(buf, &hdr, sizeof(hdr));
   memcpy(buf + sizeof(hdr), &body, sizeof(body));

   return escape_send(g_wm.adapter, g_wm.device, buf, sizeof(buf));
}

int
winmali_wddm_syncobj_arm(uint32_t handle, uint64_t *inout_point)
{
   uint8_t buf[sizeof(WINMALI_ESCAPE_HEADER) + sizeof(WINMALI_SYNC_OBJ_SIGNAL)];
   WINMALI_ESCAPE_HEADER hdr;
   WINMALI_SYNC_OBJ_SIGNAL body;
   int err;

   if (!g_wm.open)
      return -ENODEV;
   if (inout_point == NULL)
      return -EINVAL;

   memset(&hdr, 0, sizeof(hdr));
   memset(&body, 0, sizeof(body));
   hdr.Magic       = WINMALI_ESCAPE_MAGIC;
   hdr.AbiMajor    = WINMALI_ABI_MAJOR;
   hdr.AbiMinor    = WINMALI_ABI_MINOR;
   hdr.Op          = WinMaliEscapeOp_SyncObjArm;
   hdr.PayloadSize = sizeof(buf);
   body.Handle     = handle;
   body.Point      = *inout_point;
   memcpy(buf, &hdr, sizeof(hdr));
   memcpy(buf + sizeof(hdr), &body, sizeof(body));

   err = escape_send(g_wm.adapter, g_wm.device, buf, sizeof(buf));
   if (err == 0) {
      memcpy(&body, buf + sizeof(hdr), sizeof(body));
      *inout_point = body.Point;
   }
   return err;
}

int
winmali_wddm_syncobj_complete(uint32_t handle, uint64_t point)
{
   return winmali_d3dkmt_syncobj_signal(handle, point);
}

static int
syncobj_wait_impl(const uint32_t *handles, const uint64_t *points,
                  uint32_t count, int64_t timeout_ns, uint32_t wait_all,
                  uint32_t *first_signaled)
{
   size_t header_size = sizeof(WINMALI_ESCAPE_HEADER);
   size_t payload_size = sizeof(WINMALI_SYNC_OBJ_WAIT);
   size_t handles_off = WINMALI_ALIGN_UP(header_size + payload_size,
                                         WINMALI_TAIL_ALIGN);
   size_t handles_end = handles_off + (size_t)count * sizeof(uint32_t);
   size_t points_off = points
      ? WINMALI_ALIGN_UP(handles_end, WINMALI_TAIL_ALIGN)
      : 0;
   size_t total = WINMALI_ALIGN_UP(points
                                      ? points_off + (size_t)count * sizeof(uint64_t)
                                      : handles_end,
                                   WINMALI_TAIL_ALIGN);
   uint8_t *buf;
   int err;

   if (!g_wm.open)
      return -ENODEV;
   if (count == 0)
      return 0;

   buf = (uint8_t *)calloc(1, total);
   if (!buf)
      return -ENOMEM;

   {
      WINMALI_ESCAPE_HEADER *hdr = (WINMALI_ESCAPE_HEADER *)buf;
      WINMALI_SYNC_OBJ_WAIT *wait = (WINMALI_SYNC_OBJ_WAIT *)(buf + header_size);
      hdr->Magic       = WINMALI_ESCAPE_MAGIC;
      hdr->AbiMajor    = WINMALI_ABI_MAJOR;
      hdr->AbiMinor    = WINMALI_ABI_MINOR;
      hdr->Op          = WinMaliEscapeOp_SyncObjWait;
      hdr->PayloadSize = (uint32_t)total;
      wait->Flags         = wait_all ? WINMALI_SYNC_OBJ_WAIT_FLAG_ALL : 0u;
      wait->Count         = count;
      wait->HandlesOffset = (uint32_t)handles_off;
      wait->PointsOffset  = (uint32_t)points_off;
      wait->TimeoutNs     = timeout_ns;
      memcpy(buf + handles_off, handles, (size_t)count * sizeof(uint32_t));
      if (points)
         memcpy(buf + points_off, points, (size_t)count * sizeof(uint64_t));
   }

   err = escape_send(g_wm.adapter, g_wm.device, buf, (uint32_t)total);
   free(buf);

   if (err)
      return err;
   if (first_signaled)
      *first_signaled = 0;
   return 0;
}

int
winmali_d3dkmt_syncobj_wait(const uint32_t *handles, uint32_t count,
                            int64_t timeout_ns, uint32_t wait_all,
                            uint32_t *first_signaled)
{
   return syncobj_wait_impl(handles, NULL, count, timeout_ns, wait_all,
                            first_signaled);
}

int
winmali_d3dkmt_syncobj_timeline_wait(const uint32_t *handles,
                                     const uint64_t *points, uint32_t count,
                                     int64_t timeout_ns, uint32_t wait_all,
                                     uint32_t *first_signaled)
{
   if (points == NULL && count != 0)
      return -EINVAL;
   return syncobj_wait_impl(handles, points, count, timeout_ns, wait_all,
                            first_signaled);
}

#endif /* _WIN32 */

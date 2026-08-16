/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 */

#ifdef _WIN32

#include <errno.h>
#include <signal.h>
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
} g_wm;

static void winmali_log_sink(enum mesa_log_level level, const char *tag,
                             const char *msg);

/* abort() tracer - see the implementation block near the bottom of this file
 * for why it exists and, more importantly, for how to tell whether it RAN. */
static void winmali_abort_tracer_arm(void);

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

int
winmali_d3dkmt_open(void)
{
   if (g_wm.open)
      return 0;

   g_wm.gdi32 = LoadLibraryW(L"gdi32.dll");
   if (!g_wm.gdi32)
      return -ENOENT;

#define RESOLVE(slot, sym, pfn_type) do {                                    \
   g_wm.slot = (pfn_type)GetProcAddress(g_wm.gdi32, "D3DKMT" #sym);          \
   if (!g_wm.slot) { winmali_d3dkmt_close(); return -ENOSYS; }               \
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
      winmali_d3dkmt_close();
      return -ENODEV;
   }

   /* +4 slop in case adapters appear between probe and fill. */
   D3DKMT_ADAPTERINFO *adapters = (D3DKMT_ADAPTERINFO *)calloc(
      (size_t)info.NumAdapters + 4, sizeof(D3DKMT_ADAPTERINFO));
   if (!adapters) {
      winmali_d3dkmt_close();
      return -ENOMEM;
   }
   info.pAdapters    = adapters;
   info.NumAdapters += 4;
   st = g_wm.enum_adapters2(&info);
   if (st != STATUS_SUCCESS) {
      free(adapters);
      winmali_d3dkmt_close();
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
         /* From here the KMD's UART can carry our logs: register the
          * mesa_log mirror and announce ourselves so the serial shows
          * which process bound the adapter. */
         mesa_log_set_extra_sink(winmali_log_sink);
         winmali_d3dkmt_log("wm: adapter bound (hAdapter=0x%x hDevice=0x%x)",
                            g_wm.adapter, g_wm.device);
         /* MUST be after g_wm.open = true and after the escape thunk is
            resolved: the tracer snapshots the transport for itself here. */
         winmali_abort_tracer_arm();
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
   winmali_d3dkmt_close();
   return last_err;
}

void
winmali_d3dkmt_close(void)
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

/* ---- abort() tracer ---------------------------------------------------
 *
 * WHY: dwm.exe and LogonUI.exe die with c0000409 at WinMaliUmd+0x39a0f4.
 * That address is ucrt abort()'s `mov w0,#7 / brk #0xf003`
 * (__fastfail(FAST_FAIL_FATAL_APP_EXIT)) - so SOME path calls abort(), and
 * WER reports only the faulting frame, never the caller. NDEBUG is defined
 * here (b_ndebug=if-release + buildtype=release), so it is NOT an assert.
 * There are ~45 abort call sites in this DLL and static reasoning cannot
 * pick between them; this prints the actual stack.
 *
 * WHY IT WORKS (verified in the SDK's own ucrt sources and in our binary):
 *  - abort() does `__acrt_get_sigabrt_handler(); if (!= SIG_DFL) raise(SIGABRT);`
 *    BEFORE the fastfail. In our image that is `bl __acrt_get_sigabrt_handler
 *    / cbz x0, +0xc / mov w0,#0x16 / bl raise` - the cbz is exactly the test
 *    that today branches over the raise.
 *  - raise() calls `action(signum)` DIRECTLY, on the aborting thread, after
 *    releasing __acrt_signal_lock - so the backtrace is the real caller's and
 *    the handler cannot self-deadlock.
 *  - b_vscrt=mt: the CRT is INSIDE this DLL (no ucrtbase/api-ms-win-crt
 *    imports at all), so the handler slot our signal() writes is the same
 *    word our abort() reads. A handler registered here cannot be perturbed
 *    by, or perturb, the host process's own CRT.
 *  - Returning from the handler is fine and DELIBERATE: abort() then fastfails
 *    exactly as before, so the crash signature, WER grouping and every earlier
 *    triage step stay identical. We add evidence without moving the goalposts.
 *
 * ⚠ ONE SAMPLE PER PROCESS. raise() stores SIG_DFL back into the slot BEFORE
 * dispatching, so only the FIRST abort in a process is traced - on any thread.
 * If the trace names a site you did not expect, that is data, not a miss; the
 * next sample needs another process lifetime. TID is logged so a worker-thread
 * abort is distinguishable at a glance.
 *
 * ⚠ WHAT A SILENT RUN DOES *NOT* PROVE. Arming happens when the adapter binds,
 * which is inside panfrost_open_device - AFTER driParseConfigFiles. So an
 * abort in xmlconfig.c or in early threads_win32.c cannot be caught here, and
 * "armed, never fired" does NOT exonerate those. (Independently: rendertest.exe
 * walks that same early path without aborting, which is what actually
 * eliminated them.)
 *
 * ⚠ WE DO NOT ROUTE THROUGH winmali_d3dkmt_log(). It returns early on
 * !g_wm.open, and winmali_d3dkmt_close() memsets g_wm on four paths in
 * winmali_kmod.c (dev_destroy plus three dev_create failure paths, no
 * refcount). Routing through it would let one teardown mute this instrument
 * completely - and an empty ring would then read as "abort never happened",
 * which is the precise false negative this code exists to prevent. So the
 * tracer snapshots the escape thunk and handle pair at arm time and sends its
 * own buffer. It also reports the FIRST send's status on the second line, so
 * "channel was dead" is distinguishable from "nothing to say".
 */
static PFN_Escape    g_tr_escape;
static D3DKMT_HANDLE g_tr_adapter;
static D3DKMT_HANDLE g_tr_device;
static uintptr_t     g_tr_base;   /* our module's load address */
static uintptr_t     g_tr_end;    /* base + SizeOfImage */
static bool          g_tr_armed;

/* The escape payload lives in .data, NOT on the stack. abort() can be reached
 * with very little stack left, and a ~520-byte frame here would convert a
 * diagnosable abort into a silent stack overflow. Single-shot + doomed
 * process, so a shared static is safe. */
static struct {
   WINMALI_ESCAPE_HEADER hdr;
   WINMALI_DEBUG_LOG     body;
   char                  text[WINMALI_DEBUG_LOG_MAX];
} g_tr_buf;

extern IMAGE_DOS_HEADER __ImageBase;

static int
winmali_abort_emit_(const char *text, uint32_t len)
{
   D3DKMT_ESCAPE esc;
   NTSTATUS st;

   if (g_tr_escape == NULL)
      return -EIO;
   if (len > (uint32_t)sizeof(g_tr_buf.text))
      len = (uint32_t)sizeof(g_tr_buf.text);

   memset(&g_tr_buf, 0, sizeof(g_tr_buf));
   memcpy(g_tr_buf.text, text, len);

   g_tr_buf.hdr.Magic       = WINMALI_ESCAPE_MAGIC;
   g_tr_buf.hdr.AbiMajor    = WINMALI_ABI_MAJOR;
   g_tr_buf.hdr.AbiMinor    = WINMALI_ABI_MINOR;
   g_tr_buf.hdr.Op          = WinMaliEscapeOp_DebugLog;
   g_tr_buf.hdr.PayloadSize = sizeof(g_tr_buf);
   g_tr_buf.body.TextLen    = len;
   g_tr_buf.body.TextOffset = (uint32_t)(sizeof(g_tr_buf.hdr) +
                                         sizeof(g_tr_buf.body));

   memset(&esc, 0, sizeof(esc));
   esc.hAdapter              = g_tr_adapter;
   esc.hDevice               = g_tr_device;
   esc.Type                  = D3DKMT_ESCAPE_DRIVERPRIVATE;
   esc.pPrivateDriverData    = &g_tr_buf;
   esc.PrivateDriverDataSize = sizeof(g_tr_buf);

   st = g_tr_escape(&esc);
   return (st == STATUS_SUCCESS) ? 0 : -EIO;
}

/* Hand-rolled formatting on purpose: no vsnprintf in the handler. A %llx/%x
 * mismatch would print a plausible but WRONG address, and a wrong address
 * symbolizes to a confidently wrong function - worse than no output at all.
 * winmali_d3dkmt_log() has no printf format attribute in this build (the
 * PRINTFLIKE macro is inert without HAVE_FUNC_ATTRIBUTE_FORMAT), so the
 * compiler would not have caught it either. */
struct tr_sb { char *p; char *end; };

static void
tr_puts(struct tr_sb *sb, const char *s)
{
   while (*s != '\0' && sb->p < sb->end)
      *sb->p++ = *s++;
}

static void
tr_hex(struct tr_sb *sb, unsigned long long v)
{
   static const char digits[] = "0123456789abcdef";
   char tmp[16];
   int n = 0;

   if (v == 0ull) {
      tr_puts(sb, "0");
      return;
   }
   while (v != 0ull && n < (int)sizeof(tmp)) {
      tmp[n++] = digits[v & 0xfull];
      v >>= 4;
   }
   while (n-- > 0 && sb->p < sb->end)
      *sb->p++ = tmp[n];
}

static void
tr_dec(struct tr_sb *sb, unsigned long v)
{
   char tmp[20];
   int n = 0;

   if (v == 0ul) {
      tr_puts(sb, "0");
      return;
   }
   while (v != 0ul && n < (int)sizeof(tmp)) {
      tmp[n++] = (char)('0' + (v % 10ul));
      v /= 10ul;
   }
   while (n-- > 0 && sb->p < sb->end)
      *sb->p++ = tmp[n];
}

/* One frame, compact. Frames inside THIS module print as U+<rva>, which is
 * what llvm-symbolizer wants; anything else prints its absolute VA rather
 * than being silently attributed to us. */
static void
tr_frame(struct tr_sb *sb, uintptr_t addr)
{
   if (addr >= g_tr_base && addr < g_tr_end) {
      tr_puts(sb, " U+");
      tr_hex(sb, (unsigned long long)(addr - g_tr_base));
   } else {
      tr_puts(sb, " x");
      tr_hex(sb, (unsigned long long)addr);
   }
}

static void __cdecl
winmali_abort_tracer(int sig)
{
   void *frames[32];
   /* static, not stack: same reasoning as g_tr_buf - single-shot, and the
      process is already doomed, so the shared buffer costs nothing. */
   static char line[WINMALI_DEBUG_LOG_MAX];
   struct tr_sb sb;
   USHORT n;
   int i;
   int first;

   /* mesa_log must not re-enter us while we are emitting. */
   g_in_log = true;

   n = RtlCaptureStackBackTrace(0, (DWORD)(sizeof(frames) / sizeof(frames[0])),
                                frames, NULL);

   /* Line 1 carries the ANSWER: marker + the frames nearest the abort. The
      KMD ring is a fixed-size circular buffer that overwrites OLDEST first,
      so anything emitted later is what survives a wrap - putting the callers
      on the first line would make the answer the first thing evicted. */
   sb.p = line;
   sb.end = line + sizeof(line) - 1;
   tr_puts(&sb, "*** WMABORT abort() FIRED *** sig=");
   tr_dec(&sb, (unsigned long)sig);
   tr_puts(&sb, " tid=");
   tr_dec(&sb, (unsigned long)GetCurrentThreadId());
   tr_puts(&sb, " frames=");
   tr_dec(&sb, (unsigned long)n);
   tr_puts(&sb, " base=");
   tr_hex(&sb, (unsigned long long)g_tr_base);
   tr_puts(&sb, " bt:");
   for (i = 0; i < (int)n && i < 16; ++i)
      tr_frame(&sb, (uintptr_t)frames[i]);
   first = winmali_abort_emit_(line, (uint32_t)(sb.p - line));

   /* Line 2 reports line 1's delivery status, so a dead escape channel is
      distinguishable from a tracer that never ran. */
   sb.p = line;
   sb.end = line + sizeof(line) - 1;
   tr_puts(&sb, "*** WMABORT cont *** emit1=");
   tr_dec(&sb, (unsigned long)(first == 0 ? 0 : 1));
   tr_puts(&sb, " open=");
   tr_dec(&sb, (unsigned long)(g_wm.open ? 1 : 0));
   tr_puts(&sb, " bt:");
   for (i = 16; i < (int)n; ++i)
      tr_frame(&sb, (uintptr_t)frames[i]);
   (void)winmali_abort_emit_(line, (uint32_t)(sb.p - line));

   /* Deliberately return: abort() proceeds to __fastfail and the process dies
      with the same c0000409 at the same RVA as before. */
}

static void
winmali_abort_tracer_arm(void)
{
   const IMAGE_NT_HEADERS *nt;
   _crt_signal_t prev;
   char line[WINMALI_DEBUG_LOG_MAX];
   struct tr_sb sb;

   if (g_tr_armed)
      return;

   /* Snapshot the transport - see the g_wm.open discussion above. */
   g_tr_escape  = g_wm.escape;
   g_tr_adapter = g_wm.adapter;
   g_tr_device  = g_wm.device;

   g_tr_base = (uintptr_t)&__ImageBase;
   nt = (const IMAGE_NT_HEADERS *)(g_tr_base +
                                   (uintptr_t)__ImageBase.e_lfanew);
   g_tr_end = g_tr_base + (uintptr_t)nt->OptionalHeader.SizeOfImage;

   prev = signal(SIGABRT, winmali_abort_tracer);
   g_tr_armed = (prev != SIG_ERR);

   /* Calibration line. If this is absent from the ring the tracer is not
      installed OR the escape channel was already dead OR the ring wrapped -
      all three are possible, so absence alone concludes nothing. If it IS
      present and no "WMABORT abort() FIRED" follows, then abort() did not
      fire in this process while armed. */
   sb.p = line;
   sb.end = line + sizeof(line) - 1;
   tr_puts(&sb, "*** WMABORT armed *** ok=");
   tr_dec(&sb, (unsigned long)(g_tr_armed ? 1 : 0));
   tr_puts(&sb, " pid=");
   tr_dec(&sb, (unsigned long)GetCurrentProcessId());
   tr_puts(&sb, " base=");
   tr_hex(&sb, (unsigned long long)g_tr_base);
   tr_puts(&sb, " end=");
   tr_hex(&sb, (unsigned long long)g_tr_end);
   (void)winmali_abort_emit_(line, (uint32_t)(sb.p - line));
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
winmali_d3dkmt_syncobj_wait(const uint32_t *handles, uint32_t count,
                            int64_t timeout_ns, uint32_t wait_all,
                            uint32_t *first_signaled)
{
   size_t header_size = sizeof(WINMALI_ESCAPE_HEADER);
   size_t payload_size = sizeof(WINMALI_SYNC_OBJ_WAIT);
   size_t handles_off = WINMALI_ALIGN_UP(header_size + payload_size,
                                         WINMALI_TAIL_ALIGN);
   size_t total = WINMALI_ALIGN_UP(handles_off + (size_t)count * sizeof(uint32_t),
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
      wait->PointsOffset  = 0; /* binary wait */
      wait->TimeoutNs     = timeout_ns;
      memcpy(buf + handles_off, handles, (size_t)count * sizeof(uint32_t));
   }

   err = escape_send(g_wm.adapter, g_wm.device, buf, (uint32_t)total);
   free(buf);

   if (err)
      return err;
   if (first_signaled)
      *first_signaled = 0;
   return 0;
}

#endif /* _WIN32 */

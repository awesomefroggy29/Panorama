/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 *
 * Windows shim for the subset of <xf86drm.h>, <unistd.h>, <fcntl.h>, and
 * <linux/sync_file.h> that mesa's pan_kmod / pan_csf / panvk consume.
 *
 * Mesa is Unix-first; on Windows we don't have libdrm or POSIX I/O, but
 * we can stub or escape-route every API the panfrost subsystem expects.
 * Most of these shims either return -ENODEV (so a path that depended on
 * them fails predictably) or are wired to the winmali_kmod backend by
 * way of the WINMALI_FD_SENTINEL.
 *
 * The shims are intentionally header-only with __forceinline so we don't
 * collide with any library that actually defines these symbols.
 */
#pragma once

#if defined(_WIN32)

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <io.h>          /* MSVC ucrt _close / _read / _write declarations */
#include <sys/types.h>   /* _off_t, _ino_t for downstream <sys/stat.h> */
#include <time.h>        /* struct timespec, clock() */

/* Pull in Windows.h gated under the no-min-max / lean macros so it
 * doesn't poison consumers with WinGDI-isms. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WinMali device sentinel "fd". pan_kmod.h (which includes this shim on
 * _WIN32) compares dev->fd against this value to recognize a D3DKMT
 * WinMali device versus a real DRM fd, and it does so in inline helpers
 * that every translation unit compiles. The authoritative value lives in
 * rk3588-GPU/Shared/WinMaliEscape.h (the KMD/UMD ABI source of truth);
 * winmali_kmod.h also mirrors it. Define it here, guarded, so pan_kmod.h
 * always has it regardless of include order - winmali_kmod.h pulls in
 * pan_kmod.h *before* defining the sentinel itself, so relying on that
 * header alone leaves pan_kmod.h's inline uses undefined. */
#ifndef WINMALI_FD_SENTINEL
#define WINMALI_FD_SENTINEL (-442)
#endif

/* ssize_t isn't in standard MSVC headers. off_t comes from <sys/types.h>
 * (above), so we don't redefine it. */
#if !defined(_SSIZE_T_DEFINED) && !defined(ssize_t)
#  if defined(_WIN64)
typedef long long ssize_t;
#  else
typedef long ssize_t;
#  endif
#  define _SSIZE_T_DEFINED
#endif

/* --- POSIX file descriptor APIs ----------------------------------- */
/* The WinMali fd is a sentinel; nothing ever actually calls these on
 * it. Mesa pulls them in transitively. The MSVC ucrt already provides
 * _close/_read/_write/_lseek - we just map the POSIX names through. */

#ifndef O_RDONLY
#define O_RDONLY  0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY  0x0001
#endif
#ifndef O_RDWR
#define O_RDWR    0x0002
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x0
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* --- libdrm: types ------------------------------------------------ */

typedef struct {
   int     version_major;
   int     version_minor;
   int     version_patchlevel;
   int     name_len;
   char   *name;
   int     date_len;
   char   *date;
   int     desc_len;
   char   *desc;
} drmVersion, *drmVersionPtr;

/* libdrm's drmDevice structure - opaque on Linux, but mesa pokes at
 * a handful of fields (nodes, available_nodes, bustype). The instance
 * code on Windows synthesizes one of these to feed the panvk
 * try_create_for_drm callback. */
typedef struct _drmDevice {
   char **nodes;
   int    available_nodes;
   int    bustype;
   union {
      void *pci;
      void *usb;
      void *platform;
      void *host1x;
   } businfo;
   union {
      void *pci;
      void *usb;
      void *platform;
      void *host1x;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

/* DRM_NODE_* / DRM_BUS_* / DRM_RDWR enums from xf86drm.h. Values must
 * match libdrm so the rare cross-build artifact stays interpretable. */
#ifndef DRM_NODE_PRIMARY
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3
#endif

#ifndef DRM_BUS_PCI
#define DRM_BUS_PCI      0
#define DRM_BUS_USB      1
#define DRM_BUS_PLATFORM 2
#define DRM_BUS_HOST1X   3
#endif

/* DRM_RDWR is also defined by drm-uapi/drm.h on glibc-isms; only
 * define it here when drm.h hasn't been pulled in yet. */
#ifndef DRM_RDWR
#define DRM_RDWR O_RDWR
#endif

/* dev_t on MSVC ucrt is _dev_t (unsigned int). Provide it under the
 * POSIX name for code that takes st.st_rdev. */
#if !defined(_DEV_T_DEFINED_POSIX)
typedef _dev_t dev_t;
#define _DEV_T_DEFINED_POSIX
#endif

/* sys/sysmacros.h's major/minor macros. The render/primary rdev is
 * always zero on WinMali (no DRM node), so callers branch past these
 * before evaluating them, but the function still needs to compile. */
#ifndef major
#define major(d) ((unsigned int)(((d) >> 8) & 0xff))
#endif
#ifndef minor
#define minor(d) ((unsigned int)((d) & 0xff))
#endif

/* WinMali synthetic drm enumeration -------------------------------- *
 * The mesa vk_instance core iterates drmGetDevices2() and hands each
 * drmDevicePtr to a driver-supplied try_create_for_drm callback. On
 * Windows there's no DRM bus, but panvk's callback only inspects
 * available_nodes and bustype before calling pan_kmod_dev_create.
 * Synthesize one device per call so the loop produces a single
 * "platform" device that panvk will accept; the actual fd it sees
 * resolves to WINMALI_FD_SENTINEL inside pan_kmod_dev_create. */
static __forceinline int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
   (void)flags;
   if (max_devices < 1 || !devices)
      return 0;
   static char render_node[] = "winmali:render";
   static char primary_node[] = "winmali:primary";
   static char *nodes[3] = { primary_node, NULL, render_node };
   static drmDevice synthetic = {
      .nodes = nodes,
      .available_nodes = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER),
      .bustype = DRM_BUS_PLATFORM,
   };
   devices[0] = &synthetic;
   return 1;
}

static __forceinline void drmFreeDevices(drmDevicePtr devices[], int count)
{
   (void)devices; (void)count;
}

/* Single-device variant; called by some pps/perf paths. */
static __forceinline int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   (void)fd; (void)flags; (void)device;
   errno = ENOSYS;
   return -1;
}
static __forceinline void drmFreeDevice(drmDevicePtr *device) { (void)device; }

/* --- libdrm: stub thunks ----------------------------------------- */

/* The pan_kmod_ioctl macro in pan_kmod.h short-circuits to
 * winmali_kmod_ioctl when fd == WINMALI_FD_SENTINEL, so drmIoctl on
 * the WinMali fd never actually gets called. Provide a stub so the
 * link succeeds and any unguarded caller fails predictably. */
static __forceinline int drmIoctl(int fd, unsigned long request, void *arg)
{
   (void)fd; (void)request; (void)arg;
   errno = ENOSYS;
   return -1;
}

/* pan_kmod_dev_create() calls drmGetVersion before checking the fd
 * against our sentinel; we patched that path to skip the version
 * check for WinMali, so the stub here only matters for someone
 * holding a real drm fd which never happens on Windows. */
static __forceinline drmVersionPtr drmGetVersion(int fd)  { (void)fd; return NULL; }
static __forceinline void drmFreeVersion(drmVersionPtr v) { (void)v; }

/* DMA-BUF interop: not supported on Windows. */
static __forceinline int drmPrimeHandleToFD(int fd, uint32_t handle,
                                            uint32_t flags, int *prime_fd)
{
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
   errno = ENOSYS;
   return -1;
}
static __forceinline int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd; (void)prime_fd; (void)handle;
   errno = ENOSYS;
   return -1;
}
static __forceinline int drmCloseBufferHandle(int fd, uint32_t handle)
{
   (void)fd; (void)handle;
   return 0;
}

/* drm_syncobj surface. Binary syncobjs are wired to the WinMali
 * SyncObj* escapes (implemented in winmali/winmali_d3dkmt.c). Declared
 * extern here rather than via #include so the widely-included pan_kmod.h
 * doesn't drag the ABI header into every consumer. pan_context.c /
 * pan_csf.c call these libdrm entry points directly (not the pan_kmod
 * backend), so leaving Create stubbed made panfrost_create_context fail
 * at its first drmSyncobjCreate and return NULL (seen 2026-07-12: GL
 * dhglrc=0, D3D cso_create_context(NULL) crash). */
int winmali_d3dkmt_syncobj_create(uint32_t drm_flags, uint32_t *out_handle);
int winmali_d3dkmt_syncobj_destroy(uint32_t handle);
int winmali_d3dkmt_syncobj_signal(uint32_t handle, uint64_t point);
int winmali_d3dkmt_syncobj_wait(const uint32_t *handles, uint32_t count,
                                 int64_t timeout_ns, uint32_t wait_all,
                                 uint32_t *first_signaled);
int winmali_d3dkmt_syncobj_timeline_wait(const uint32_t *handles,
                                         const uint64_t *points,
                                         uint32_t count, int64_t timeout_ns,
                                         uint32_t wait_all,
                                         uint32_t *first_signaled);
int winmali_d3dkmt_syncobj_transfer(uint32_t dst, uint64_t dst_point,
                                    uint32_t src, uint64_t src_point);

static __forceinline int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   (void)fd;
   return winmali_d3dkmt_syncobj_create(flags, handle);
}
static __forceinline int drmSyncobjDestroy(int fd, uint32_t handle)
{
   (void)fd;
   return winmali_d3dkmt_syncobj_destroy(handle);
}
static __forceinline int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
   (void)fd; (void)handles; (void)count;
   errno = ENOSYS;
   return -1;
}
static __forceinline int drmSyncobjWait(int fd, uint32_t *handles, unsigned count,
                                        int64_t timeout, unsigned flags,
                                        uint32_t *first_signaled)
{
   (void)fd;
   /* DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL == (1<<0); its #define appears later
    * in this header, so use the literal to avoid an ordering dependency. */
   return winmali_d3dkmt_syncobj_wait(handles, count, timeout,
                                      flags & 0x1u, first_signaled);
}
static __forceinline int drmSyncobjTimelineWait(int fd, uint32_t *handles,
                                                uint64_t *points, unsigned count,
                                                int64_t timeout, unsigned flags,
                                                uint32_t *first_signaled)
{
   (void)fd;
   return winmali_d3dkmt_syncobj_timeline_wait(handles, points, count, timeout,
                                               flags & 0x1u, first_signaled);
}
static __forceinline int drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_pt,
                                            uint32_t src, uint64_t src_pt, unsigned flags)
{
   (void)fd; (void)flags;
   return winmali_d3dkmt_syncobj_transfer(dst, dst_pt, src, src_pt);
}
static __forceinline int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
   (void)fd; (void)obj_fd; (void)handle;
   errno = ENOSYS;
   return -1;
}
static __forceinline int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
   (void)fd; (void)handle; (void)obj_fd;
   errno = ENOSYS;
   return -1;
}
static __forceinline int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   (void)fd; (void)handle; (void)sync_file_fd;
   errno = ENOSYS;
   return -1;
}
static __forceinline int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   (void)fd; (void)handle; (void)sync_file_fd;
   errno = ENOSYS;
   return -1;
}
static __forceinline int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
   (void)fd;
   for (uint32_t i = 0; i < count; ++i) {
      int r = winmali_d3dkmt_syncobj_signal(handles[i], 1); /* binary: signalled */
      if (r)
         return r;
   }
   return 0;
}
static __forceinline int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                                                  uint64_t *points, uint32_t count)
{
   (void)fd;
   for (uint32_t i = 0; i < count; ++i) {
      int r = winmali_d3dkmt_syncobj_signal(handles[i], points[i]);
      if (r)
         return r;
   }
   return 0;
}
static __forceinline int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points,
                                         uint32_t count)
{
   (void)fd; (void)handles; (void)points; (void)count;
   errno = ENOSYS;
   return -1;
}

/* Sync file flag values (from <linux/sync_file.h>, copied here so
 * mesa's references compile). */
#ifndef DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL          (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT   (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE    (1 << 2)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE     (1 << 3)
#define DRM_SYNCOBJ_TRANSFER_FLAGS_EXPORT_FORK   (1 << 0)
#endif

/* --- strndup shim ------------------------------------------------- */
/* POSIX, not in MSVC ucrt. */
#include <string.h>
#include <stdlib.h>

static __forceinline char *strndup(const char *s, size_t n)
{
   if (!s)
      return NULL;
   size_t len = 0;
   while (len < n && s[len])
      len++;
   char *out = (char *)malloc(len + 1);
   if (!out)
      return NULL;
   memcpy(out, s, len);
   out[len] = '\0';
   return out;
}

/* --- asprintf shim ------------------------------------------------ */
/* GNU extension, not in MSVC. Use _vscprintf to size, then vsnprintf. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static __forceinline int vasprintf(char **out, const char *fmt, va_list ap)
{
   va_list ap_copy;
   va_copy(ap_copy, ap);
   int len = _vscprintf(fmt, ap_copy);
   va_end(ap_copy);
   if (len < 0) {
      *out = NULL;
      return -1;
   }
   char *buf = (char *)malloc((size_t)len + 1);
   if (!buf) {
      *out = NULL;
      return -1;
   }
   int written = vsnprintf(buf, (size_t)len + 1, fmt, ap);
   if (written < 0) {
      free(buf);
      *out = NULL;
      return -1;
   }
   *out = buf;
   return written;
}
static __forceinline int asprintf(char **out, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   int n = vasprintf(out, fmt, ap);
   va_end(ap);
   return n;
}

/* --- renderonly stubs --------------------------------------------- */
/* renderonly drives KMS dumb-buffer scanout on Linux/DRI; Windows
 * always goes through D3DKMT, so dev->ro stays NULL and every
 * renderonly_* path in pan_resource.c is unreachable at runtime.
 * We still need declarations so the gated-by-runtime call sites
 * compile. Returning NULL / false keeps the dead path honest. */

struct pipe_resource;
struct winsys_handle;
struct renderonly;
struct renderonly_scanout;

static __forceinline void renderonly_scanout_destroy(struct renderonly_scanout *s,
                                                     struct renderonly *ro)
{
   (void)s; (void)ro;
}
static __forceinline struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc,
                                          struct renderonly *ro,
                                          struct winsys_handle *out_handle)
{
   (void)rsc; (void)ro; (void)out_handle;
   return NULL;
}
static __forceinline struct renderonly_scanout *
renderonly_scanout_for_resource(struct pipe_resource *rsc,
                                struct renderonly *ro,
                                struct winsys_handle *out_handle)
{
   (void)rsc; (void)ro; (void)out_handle;
   return NULL;
}
static __forceinline bool renderonly_get_handle(struct renderonly_scanout *s,
                                                struct winsys_handle *handle)
{
   (void)s; (void)handle;
   return false;
}

/* --- pthread surface ---------------------------------------------- */
/* Panfrost / pan_kmod uses raw pthread mutexes. Map onto Windows
 * SRWLOCK — it's lock-free uncontended, lighter than CRITICAL_SECTION,
 * and the API is a 1:1 fit for the (init, lock, unlock, destroy) shape
 * mesa actually exercises. */

typedef SRWLOCK pthread_mutex_t;
typedef int     pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static __forceinline int
pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
   (void)attr;
   InitializeSRWLock(m);
   return 0;
}
static __forceinline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static __forceinline int pthread_mutex_lock(pthread_mutex_t *m)
{
   AcquireSRWLockExclusive(m);
   return 0;
}
static __forceinline int pthread_mutex_trylock(pthread_mutex_t *m)
{
   return TryAcquireSRWLockExclusive(m) ? 0 : EBUSY;
}
static __forceinline int pthread_mutex_unlock(pthread_mutex_t *m)
{
   ReleaseSRWLockExclusive(m);
   return 0;
}

/* --- clock_gettime / CLOCK_MONOTONIC ------------------------------ */
/* mesa uses CLOCK_MONOTONIC for BO cache LRU. QueryPerformanceCounter
 * has the same monotonic guarantee on Windows. */

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#endif

/* MSVC ucrt does declare struct timespec (via time.h above) so we just
 * borrow it. clockid_t isn't a real type on Windows. */
typedef int clockid_t;

static __forceinline int clock_gettime(clockid_t clk, struct timespec *ts)
{
   (void)clk;
   LARGE_INTEGER freq, now;
   if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&now)) {
      errno = EINVAL;
      return -1;
   }
   ts->tv_sec  = (time_t)(now.QuadPart / freq.QuadPart);
   /* Fixed-point nanoseconds without overflowing int64. */
   uint64_t rem = (uint64_t)(now.QuadPart % freq.QuadPart);
   ts->tv_nsec = (long)((rem * 1000000000ull) / (uint64_t)freq.QuadPart);
   return 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _WIN32 */

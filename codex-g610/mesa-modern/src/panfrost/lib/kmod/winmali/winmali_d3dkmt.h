/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 *
 * Internal D3DKMT transport for the WinMali pan_kmod backend.
 *
 * Wraps Windows' gdi32-exported D3DKMT thunks so the rest of the
 * backend never includes <d3dkmthk.h>. Holds the singleton (adapter
 * handle, device handle) pair we get from D3DKMTOpenAdapterFromLuid +
 * D3DKMTCreateDevice. There's one WinMali GPU per system on the target
 * hardware (Mali-G610 on RK3588), so the singleton model is fine.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the WinMali adapter:
 *   - Load gdi32.dll, resolve D3DKMT thunks.
 *   - Walk every WDDM adapter via D3DKMTEnumAdapters2.
 *   - For each: OpenAdapterFromLuid -> CreateDevice -> WinMaliEscapeOp_OpenDevice
 *     handshake. The first to return Status==0 is the WinMali KMD.
 *
 * Returns 0 on success, -errno on failure. Safe to call multiple times;
 * subsequent calls are a no-op until winmali_d3dkmt_close().
 */
int winmali_d3dkmt_open(void);

/* Tear down the singleton: D3DKMTDestroyDevice, D3DKMTCloseAdapter,
 * FreeLibrary(gdi32). No-op if not currently open. */
void winmali_d3dkmt_close(void);

/* Ship one escape buffer to the WinMali KMD via D3DKMT_ESCAPE with
 * Type=D3DKMT_ESCAPE_DRIVERPRIVATE. The buffer must already have a
 * filled-in WINMALI_ESCAPE_HEADER at offset 0; PayloadSize must equal
 * `size`. Returns 0 on success or -errno on failure, copying the KMD's
 * status (the int32_t in the header) into the return value when
 * positive (-errno convention: 0 = ok, negative = error).
 *
 * NOT thread-safe per se but D3DKMT_ESCAPE is serialised by the kernel
 * so concurrent calls just queue up. Callers needing ordering across
 * escapes should use higher-level locking. */
int winmali_d3dkmt_escape(void *buf, size_t size);

/* Return non-zero if winmali_d3dkmt_open() succeeded at least once. */
bool winmali_d3dkmt_is_open(void);

/* drm_syncobj operations routed to WinMaliEscapeOp_SyncObj*. The libdrm
 * drmSyncobj* shims in winmali_drm_compat.h call these (pan_context.c /
 * pan_csf.c use libdrm syncobjs directly, not the pan_kmod backend).
 * Binary syncobjs only. Return 0 / -errno. */
int winmali_d3dkmt_syncobj_create(uint32_t drm_flags, uint32_t *out_handle);
int winmali_d3dkmt_syncobj_destroy(uint32_t handle);
int winmali_d3dkmt_syncobj_wait(const uint32_t *handles, uint32_t count,
                                int64_t timeout_ns, uint32_t wait_all,
                                uint32_t *first_signaled);
int winmali_d3dkmt_syncobj_timeline_wait(const uint32_t *handles,
                                         const uint64_t *points,
                                         uint32_t count, int64_t timeout_ns,
                                         uint32_t wait_all,
                                         uint32_t *first_signaled);
/* Signal `handle` to `point` (binary: point>0 => signalled; timeline: raise). */
int winmali_d3dkmt_syncobj_signal(uint32_t handle, uint64_t point);
/* Install src/src_point as the pending fence in dst/dst_point without waiting. */
int winmali_d3dkmt_syncobj_transfer(uint32_t dst, uint64_t dst_point,
                                    uint32_t src, uint64_t src_point);

/* Mirror one formatted text line onto the KMD's serial log (UART) via
 * WinMaliEscapeOp_DebugLog - the UMD's only window on a headless DUT.
 * The KMD prefixes it "[WinMali-UMD pid=N]". No-op before the adapter
 * is open, if the KMD predates the op (-EOPNOTSUPP once, then muted),
 * or when re-entered from its own escape path. winmali_d3dkmt_open()
 * also registers a mesa_log extra sink so every mesa_loge/logw/logi in
 * the process flows here automatically. */
void winmali_d3dkmt_log(const char *fmt, ...);

/* Copy the finalized root CS into the KMD's bounded flight recorder before
 * the WDDM packet is queued. Diagnostic only: failure/old KMD is ignored and
 * never changes submission behavior. */
void winmali_d3dkmt_diag_record(uint32_t batch_sequence,
                                uint64_t stream_gpu_va,
                                const void *stream_cpu,
                                uint32_t stream_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

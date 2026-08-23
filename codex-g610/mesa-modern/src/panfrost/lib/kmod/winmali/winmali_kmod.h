/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 *
 * Public interface to the WinMali pan_kmod backend. Consumed by:
 *   - pan_kmod.c          (dispatch winmali_kmod_dev_create on
 *                          WINMALI_FD_SENTINEL)
 *   - pan_kmod_ioctl()    (route panthor ioctls through D3DKMT_ESCAPE)
 *   - panvk / pan_csf     (panthor_kmod_* helpers continue to work
 *                          because the backend exposes the same shapes)
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#include "kmod/pan_kmod.h"

/* Sentinel "fd" value used by mesa to recognize a WinMali (D3DKMT) device
 * versus a real DRM device. Must stay in sync with rk3588-GPU/Shared/
 * WinMaliEscape.h, which is the source of truth for the KMD/UMD ABI. We
 * mirror it here so consumers don't need that header on their include
 * path. The fd is never passed to a real syscall — pan_kmod_ioctl
 * intercepts it and routes through winmali_kmod_ioctl. */
#ifndef WINMALI_FD_SENTINEL
#define WINMALI_FD_SENTINEL (-442)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Constructor used by pan_kmod_dev_create when fd == WINMALI_FD_SENTINEL.
 * Opens the WinMali adapter, runs the OpenDevice handshake, queries
 * device properties, and returns a pan_kmod_dev that the rest of mesa
 * treats like a panthor_kmod_dev. The drmVersionPtr is unused on the
 * Windows path - we synthesize one inside if the pan_kmod_ops vtable
 * needs it. */
struct pan_kmod_dev *winmali_kmod_dev_create(
   int fd, uint32_t flags, const drmVersionPtr version,
   const struct pan_kmod_allocator *allocator);

struct winmali_wddm_alloc_ops;

/* Supply the per-D3D-device WDDM callbacks while an isolated Panfrost screen
 * is being constructed. The value is thread-local and is copied into the new
 * pan_kmod device before screen bootstrap allocates any GPU-visible BO. */
void winmali_kmod_set_create_wddm_alloc_ops(
   const struct winmali_wddm_alloc_ops *ops);

/* Route a panthor-shaped ioctl call through D3DKMT_ESCAPE. Called from
 * the pan_kmod_ioctl() macro intercept in pan_kmod.h when fd is the
 * WinMali sentinel. Returns 0 on success or -errno (matching libdrm's
 * drmIoctl convention). */
int winmali_kmod_ioctl(unsigned long request, void *arg);

/* Device-aware form used by Gallium.  The legacy fd-shaped ioctl entry point
 * cannot identify which private D3D screen owns a group because WinMali has no
 * real file descriptor. */
int winmali_kmod_ioctl_dev(struct pan_kmod_dev *dev,
                           unsigned long request, void *arg);

/* Wrap an existing D3DKMT allocation (the swap-chain backbuffer handed
 * back by pfnAllocateCb in the D3D10/11 UMD) as a pan_kmod_bo. The
 * compositor-visible kernel allocation becomes a BO the panfrost
 * driver can VM-bind and render to. cpu_va is borrowed from the frontend's
 * lifetime LockCb and remains frontend-owned. Returns NULL on failure with
 * errno set. */
struct pan_kmod_bo *winmali_kmod_bo_from_allocation(
   struct pan_kmod_dev *dev, uint32_t kmt_allocation_handle,
   uint64_t size, uint32_t flags, void *cpu_va);

/* Phase 1 WDDM-allocation bridge (struct winmali_wddm_alloc_ops +
 * winmali_kmod_set_wddm_alloc_ops) lives in the shared, dependency-free
 * header rk3588-GPU/Shared/WinMaliWddmBridge.h. It is NOT included here so
 * this header stays lightweight for the many panfrost TUs that pull in
 * winmali_kmod.h without the Shared/ dir on their include path. The consumers
 * that actually use the bridge (winmali_kmod.c, pan_resource.c, Device.cpp)
 * include WinMaliWddmBridge.h directly. */

/* Ship a BO's pixels to the supplied HDC via WinMaliEscapeOp_PresentToHdc
 * (op 0x90). KMD waits on the BO's last submit, CPU-maps the pages it
 * owns, and SetDIBitsToDevice's them to the HDC. Used by the GL
 * frontend (wgl_present) on SwapBuffers. Returns 0 on success,
 * -errno on failure.
 *
 * Format selects how the KMD interprets the raw bytes:
 *   1 = WinMaliPresentFormat_Bgra8888
 *   2 = WinMaliPresentFormat_Rgba8888
 *   3 = WinMaliPresentFormat_Rgb565 */
int winmali_kmod_present_to_hdc(struct pan_kmod_dev *dev,
                                struct pan_kmod_bo *bo,
                                void *hdc,
                                uint32_t format,
                                uint32_t width, uint32_t height,
                                uint32_t stride,
                                uint32_t dst_x, uint32_t dst_y);

/* Ask the KMD to expose a BO's pages in the calling process's
 * user-mode address space via WinMaliEscapeOp_BoMapCpu (op 0x86).
 * The POSIX mmap()-via-fd flow that pan_kmod_bo_mmap uses on Linux
 * can't work on Windows because our "fd" is the WinMali sentinel.
 * Returns the UM virtual address on success, MAP_FAILED on error.
 * prot is the same set of flags POSIX mmap takes (PROT_READ /
 * PROT_WRITE). */
void *winmali_kmod_bo_map_cpu(struct pan_kmod_bo *bo, int prot);

#ifdef __cplusplus
} /* extern "C" */
#endif

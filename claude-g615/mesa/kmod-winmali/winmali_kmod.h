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

/* Route a panthor-shaped ioctl call through D3DKMT_ESCAPE. Called from
 * the pan_kmod_ioctl() macro intercept in pan_kmod.h when fd is the
 * WinMali sentinel. Returns 0 on success or -errno (matching libdrm's
 * drmIoctl convention). */
int winmali_kmod_ioctl(unsigned long request, void *arg);

/* Wrap an existing D3DKMT allocation (the swap-chain backbuffer handed
 * back by pfnAllocateCb in the D3D10/11 UMD) as a pan_kmod_bo. The
 * compositor-visible kernel allocation becomes a BO the panfrost
 * driver can VM-bind and render to. Returns NULL on failure with errno
 * set. */
void winmali_kmod_register_alloc_id(uint32_t kmt, uint32_t id, uint32_t id_hi);

/* Record the CPU address pfnLockCb(LockEntire) returned for a dxgk allocation.
 * That pointer maps the allocation's REAL, resident pages into this process, so
 * the import path can hand it to escape 0x93 (BoFromUserVa) and wrap those exact
 * frames - the only route that works under escape submission, where dxgk never
 * aperture-maps the surface and 0x81/0x92 therefore cannot see its pages.
 * Call it wherever the allocation is locked, and pass the same size the lock
 * covers. Safe to call repeatedly; the newest pointer wins. */
/* pitch = the allocation's row stride in bytes. The KMD's present worker has
 * no other way to learn the shape of the surface it copies - 0x93 used to carry
 * only VA+size, so it assumed the PANEL pitch for everything. */
void winmali_kmod_register_alloc_cpu(uint32_t kmt, void *cpu, uint64_t size,
                                     uint32_t pitch);

struct pan_kmod_bo *winmali_kmod_bo_from_allocation(
   struct pan_kmod_dev *dev, uint32_t kmt_allocation_handle,
   uint64_t size, uint32_t flags);

/* KMT identity-map helper. The importer MUST peek first and reuse the existing
 * panfrost BO when the handle is already known and its bo_map slot is live:
 * creating a throwaway wrapper for a known kmt and then dropping it erases the
 * identity entry of a BO that is still alive. Returns 0 if unknown. */
uint32_t winmali_kmod_peek_handle_for_kmt(uint32_t kmt);

/* Which D3DKMT allocation does this BO wrap? 0 = not an import (an ordinary
 * escape- or dxgk-created BO).
 *
 * WHY THIS EXISTS: dev->bo_map is indexed by BO handle, and TWO independent
 * allocators mint into that one index space - the KMD's adapter-global BoTable
 * (WinMaliBo.c: NextHandle starts at 1) and our per-process import counter
 * (WINMALI_IMPORT_HANDLE_BASE, also effectively 1). They WILL collide. The
 * importer used to treat "the slot is occupied" as "this is the same
 * allocation", so a collision silently handed out an unrelated live BO (the
 * tiler heap, a pool chunk) as the shared surface; its refcount then went wrong
 * and it was freed under its real owner. That is a use-after-free, and it is
 * the measured LogonUI/ApplicationFrameHost crash: panfrost_bo_free() memsets
 * the struct, so a later panfrost_bo_handle() reads bo->kmod_bo->handle through
 * a NULL kmod_bo - "ldr w19,[x8,#0x10] with x8=0", handle being at offset 0x10
 * of pan_kmod_bo. Occupancy is NOT identity; callers must compare this. */
uint32_t winmali_kmod_bo_kmt_handle(const struct pan_kmod_bo *bo);

/* Provenance of bo->handle: true = minted by the KMD's BoTable (escape
 * BoCreate, or an import's KMD-issued identity), false = a UMD-synthetic id
 * that the BoTable never issued.
 *
 * Exposed so a diagnostic can CLASSIFY a failing BO on its own line instead of
 * correlating it against an allocation trace. The 256 KB log ring wraps, and an
 * allocation trace is always OLDER than the failure that follows it, so it is
 * the first thing evicted - three separate cross-line correlations in this
 * codebase were unsound for exactly that reason. Put the answer in the failing
 * line itself. */
bool winmali_kmod_bo_handle_is_kmd(const struct pan_kmod_bo *bo);

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

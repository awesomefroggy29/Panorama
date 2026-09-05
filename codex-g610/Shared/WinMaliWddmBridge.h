/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 *
 * Phase 1 WDDM-allocation bridge across the C <-> C++ boundary.
 *
 * The escape ABI lives in the C pan_kmod backend (winmali_kmod.c); the dxgk
 * allocation callbacks (pfnAllocateCb / pfnLockCb / pfnDeallocateCb) live in
 * the C++ d3d10umd frontend (Device). This tiny, dependency-free header
 * (no gallium, no pan_kmod, no windows.h) lets the frontend install a hook
 * so winmali_kmod_bo_alloc backs every BO with a REAL kernel-tracked dxgk
 * allocation instead of an escape-private BoCreate.
 *
 * It sits in rk3588-GPU/Shared/ because that dir is already on the include
 * path of BOTH the C++ frontend (d3d10umd) and the C backend (pan_kmod).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pan_kmod_dev;
struct pipe_screen;

/* GPU mapping protections carried across the pan_kmod C <-> WDDM C++
 * boundary.  These are bridge semantics, not pan_kmod or VM-bind flag values;
 * keeping a separate namespace prevents accidental bit-for-bit translation
 * between APIs whose flag layouts differ. */
#define WINMALI_WDDM_MAP_READONLY (1u << 0)
#define WINMALI_WDDM_MAP_EXECUTE  (1u << 1)

/* Arm/complete the private syncobjs carried by a WDDM render descriptor.
 * Arm runs before pfnRenderCb so a userspace wait cannot observe the old
 * signalled state while dxgkrnl is still queueing the DMA buffer. */
int winmali_wddm_syncobj_arm(uint32_t handle, uint64_t *inout_point);
int winmali_wddm_syncobj_complete(uint32_t handle, uint64_t point);

/* When installed, winmali_kmod_bo_alloc routes through these. NULL = escape
 * BoCreate (used by non-WDDM clients). A WDDM allocation failure is returned
 * to Mesa; it must never silently create an escape-private BO because that BO
 * would not exist in the dxgkrnl process page tables used for submission. */
struct winmali_wddm_alloc_ops {
   void *cookie;

   /* Phase 3 UMD half: submit a render command stream through a dxgk GPUVA
    * context (pfnSubmitCommandCb) instead of escape GroupSubmit. The CS
    * itself lives at stream_va; WINMALI_CS_SUBMIT_DESC is passed as validated
    * private submission data so DxgkDdiSubmitCommandVirtual can queue it on
    * the CSG. sync_ops is the
    * WINMALI_SYNC_OP array for this stream; the KMD executes waits before the
    * CS and signals after terminal completion. Returns 0 once safely queued.
    * Opt-in: panfrost only calls this when WINMALI_DXGK_SUBMIT is set;
    * otherwise the working escape GroupSubmit path is used. */
   int (*render)(void *cookie, uint32_t vm_id, uint64_t stream_va,
                 uint32_t stream_size, const void *sync_ops,
                 uint32_t sync_count);

   /* Phase 2 UMD half: map/unmap a dxgk allocation into the GPU virtual address
    * space via pfnMapGpuVirtualAddressCb / pfnFreeGpuVirtualAddressCb, retiring
    * the escape VmBind for BOs that are real dxgk allocations. A nonzero
    * *inout_va requests an explicit base; zero requests process-wide automatic
    * placement by VidMm. The selected address is returned through *inout_va.
    * map_flags is WINMALI_WDDM_MAP_* and must reflect the BO's GPU access contract. The
    * map is synchronised on the device's paging queue before returning. Opt-in:
    * panfrost only calls these when WINMALI_DXGK_ADDR is set AND the BO is
    * dxgk-backed (wddm_kmt_handle != 0); otherwise escape VmBind is used.
    * Returns 0 on success, non-zero (errno) on failure. */
   int (*map_va)(void *cookie, uint32_t kmt_handle, uint64_t offset_bytes,
                 uint64_t size, uint32_t map_flags, uint64_t *inout_va);
   void (*unmap_va)(void *cookie, uint64_t va, uint64_t size);

   /* Create `size` bytes of opaque backing as a real dxgk allocation, made
    * resident and CPU-mapped for its lifetime (LockCb). *out_kmt_handle = the
    * D3DKMT allocation handle; *out_cpu = the persistent CPU VA (so the BO
    * needs no escape BoMapCpu). Returns 0 on success, non-zero (errno) fail. */
   int (*alloc)(void *cookie, uint64_t size, uint32_t *out_kmt_handle,
                void **out_cpu);
   /* Release an allocation previously returned by alloc(). */
   void (*free)(void *cookie, uint32_t kmt_handle);

   /* Phase 4 R1 (opt-in WINMALI_DXGK_FENCE): dxgk monitored fences replacing
    * escape SyncObj (0x82/0x83/0x84/0x91) on the fully-dxgk path. A monitored
    * fence is a timeline (create with initial value 0, signal/wait a value),
    * matching the WinMali timeline syncobj semantics mesa uses. Per-process, so
    * no cross-process handle confusion with escape-syncobj processes (DWM). When
    * these are installed, winmali_kmod's syncobj helpers route through them
    * instead of the escape SyncObj ops. Returns 0 on success. */
   int (*fence_create)(void *cookie, uint32_t *out_handle);
   void (*fence_destroy)(void *cookie, uint32_t handle);
   int (*fence_signal)(void *cookie, uint32_t handle, uint64_t value);
   int (*fence_wait)(void *cookie, uint32_t handle, uint64_t value);
};

/* C backend setter — impl in panfrost/lib/kmod/winmali/winmali_kmod.c. */
void winmali_kmod_set_wddm_alloc_ops(struct pan_kmod_dev *dev,
                                     const struct winmali_wddm_alloc_ops *ops);

/* gallium-screen convenience — impl in panfrost drivers/panfrost/pan_resource.c
 * (which can reach pan_device(screen)->kmod.dev). Lets the d3d10umd C++
 * frontend register the hook with only a pipe_screen, no panfrost internals. */
void panfrost_winmali_set_wddm_alloc_ops(struct pipe_screen *screen,
                                         const struct winmali_wddm_alloc_ops *ops);

#ifdef __cplusplus
} /* extern "C" */
#endif

/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 */
#ifndef __PAN_WINMALI_PUBLIC_H__
#define __PAN_WINMALI_PUBLIC_H__

struct pipe_screen;
struct pipe_screen_config;
struct winmali_wddm_alloc_ops;

#ifdef __cplusplus
extern "C" {
#endif

/* Open the singleton WinMali panfrost screen on Windows.
 *
 * Equivalent of panfrost_drm_screen_create() but routed through the
 * WinMali pan_kmod backend (D3DKMT_ESCAPE-based) instead of DRM ioctls.
 * Called from the WGL OpenGL ICD and from the d3d10_rk3588 D3D11 UMD
 * target; both share the same per-process screen via
 * u_pipe_screen_lookup_or_create. */
struct pipe_screen *
panfrost_winmali_screen_create(const struct pipe_screen_config *config);

/* Create an uncached screen with its own panfrost_device, VM, and BO cache.
 * Required for a Windows D3D device because its Dxgk callbacks and allocation
 * handles have device lifetime, unlike the process-wide adapter screen. */
struct pipe_screen *
panfrost_winmali_screen_create_isolated(
   const struct pipe_screen_config *config,
   const struct winmali_wddm_alloc_ops *wddm_ops);

#ifdef __cplusplus
}
#endif

#endif /* __PAN_WINMALI_PUBLIC_H__ */

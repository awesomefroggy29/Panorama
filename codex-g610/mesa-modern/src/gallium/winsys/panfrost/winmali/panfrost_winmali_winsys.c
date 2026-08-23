/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 *
 * WinMali analogue of panfrost_drm_winsys.c. No DRM, no PRIME, no
 * renderonly - the WinMali pan_kmod backend talks D3DKMT_ESCAPE to a
 * Windows kernel-mode WDDM 2.0 miniport, so the "fd" is the sentinel
 * WINMALI_FD_SENTINEL and panfrost_create_screen() routes through the
 * winmali_kmod_dev_create() backend in pan_kmod_dev_create.
 */
#include "util/u_screen.h"

#include "panfrost/pan_public.h"
#include "kmod/winmali/winmali_kmod.h"
#include "WinMaliEscape.h"
#include "WinMaliWddmBridge.h"

#include "panfrost_winmali_public.h"

struct pipe_screen *
panfrost_winmali_screen_create(const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(WINMALI_FD_SENTINEL, config, NULL,
                                         panfrost_create_screen);
}

struct pipe_screen *
panfrost_winmali_screen_create_isolated(
   const struct pipe_screen_config *config,
   const struct winmali_wddm_alloc_ops *wddm_ops)
{
   /* panfrost_create_screen performs GPU-visible bootstrap allocations before
    * it returns. Publish this Device's callbacks for exactly the duration of
    * synchronous construction so those BOs use the same WDDM address space as
    * every later render submission. */
   winmali_kmod_set_create_wddm_alloc_ops(wddm_ops);
   struct pipe_screen *screen =
      panfrost_create_screen(WINMALI_FD_SENTINEL, config, NULL);
   winmali_kmod_set_create_wddm_alloc_ops(NULL);
   return screen;
}

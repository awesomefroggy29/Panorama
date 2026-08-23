/*
 * Copyright © 2026 WinMali project
 * SPDX-License-Identifier: MIT
 *
 * d3d10_create_screen() override for the WinMali D3D10/11 UMD path.
 *
 * The d3d10umd state tracker (gallium/frontends/d3d10umd/) calls this
 * symbol from Adapter::Open and uses the returned pipe_screen for the
 * entire D3D10/11 surface. The default d3d10umd target wraps softpipe
 * or llvmpipe; ours hands back the panfrost-on-WinMali screen so D3D11
 * ends up running on the Mali-G610 GPU via the WinMali pan_kmod
 * backend.
 */

#include <stddef.h>
#include "panfrost/winmali/panfrost_winmali_public.h"

struct pipe_screen *
d3d10_create_screen(void)
{
   return panfrost_winmali_screen_create(NULL);
}

/* D3D devices cannot share the cached adapter-capability screen: Panfrost's
 * screen object owns the VM and BO cache, while WDDM callbacks and allocation
 * handles are scoped to a D3D device. */
struct pipe_screen *
d3d10_create_device_screen(const struct winmali_wddm_alloc_ops *wddm_ops)
{
   return panfrost_winmali_screen_create_isolated(NULL, wddm_ops);
}

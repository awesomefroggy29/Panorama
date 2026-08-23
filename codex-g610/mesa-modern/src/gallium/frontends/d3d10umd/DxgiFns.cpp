/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * DxgiFns.cpp --
 *    DXGI related functions.
 */

#include <stdio.h>

#include "DxgiFns.h"
#include "Format.h"
#include "State.h"

#include "Debug.h"

#include "util/format/u_format.h"


/*
 * ----------------------------------------------------------------------
 *
 * _Present --
 *
 *    This is turned into kernel callbacks rather than directly emitted
 *    as fifo packets.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] DXGI Present ENTER device=%p src=%p dst=%p flags=0x%x\n",
               pPresentData ? (void *)(uintptr_t)pPresentData->hDevice : NULL,
               pPresentData ? (void *)(uintptr_t)pPresentData->hSurfaceToPresent : NULL,
               pPresentData ? (void *)(uintptr_t)pPresentData->hDstResource : NULL,
               pPresentData ? pPresentData->Flags.Value : 0);

   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);

   DebugPrintf("WinMali: DXGI Present flush-before ENTER device=%p\n", device);
   device->pipe->flush(device->pipe, NULL, 0);
   DebugPrintf("WinMali: DXGI Present flush-before EXIT device=%p\n", device);

   /* Redirected present: when hDstResource is non-zero the runtime wants us to
    * copy the source into that destination surface (the DWM-off / cross-adapter
    * redirected-present path, the _Present analogue of the windowed BltDXGI in
    * _Blt). Do the copy and we're done - no kernel present needed. */
   {
      struct pipe_resource *dst = CastPipeResource(pPresentData->hDstResource);
      struct pipe_resource *src = pSrcResource ? pSrcResource->resource : NULL;
      if (dst != NULL && src != NULL) {
         DebugPrintf("WinMali: DXGI Present redirected-copy ENTER src=%p dst=%p %ux%u\n",
                     src, dst, src->width0, src->height0);
         struct pipe_box box;
         box.x = 0; box.y = 0; box.z = 0;
         box.width  = src->width0;
         box.height = src->height0;
         box.depth  = 1;
         device->pipe->resource_copy_region(device->pipe, dst, 0, 0, 0, 0,
                                            src, 0, &box);
         DebugPrintf("WinMali: DXGI Present redirected-flush ENTER\n");
         device->pipe->flush(device->pipe, NULL, 0);
         DebugPrintf("WinMali: DXGI Present redirected-flush EXIT\n");
         DebugPrintf("WinMali: DXGI Present redirected-copy EXIT hr=0x%08x\n", S_OK);
         return S_OK;
      }
   }

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   /* Lazily create the dxgk kernel context on the FIRST present. Kept out
    * of CreateDevice on purpose: dxgk's first-real-context init currently
    * bugchecks on HW (v29), and eager creation put that in the path of
    * every D3D11 process. Here only a process that actually presents
    * (umd-smoke -window) reaches it. One attempt; failure latches. */
   if (!device->hContextTried) {
      DebugPrintf("WinMali: DXGI Present create-context ENTER device=%p cb=%p\n",
                  device, device->KTCallbacks.pfnCreateContextCb);
      device->hContextTried = TRUE;
      device->hContext = NULL;
      if (device->KTCallbacks.pfnCreateContextCb != NULL) {
         D3DDDICB_CREATECONTEXT ctxCb;
         memset(&ctxCb, 0, sizeof ctxCb);
         ctxCb.NodeOrdinal = 0;
         ctxCb.EngineAffinity = 0;
         HRESULT chr = device->KTCallbacks.pfnCreateContextCb(device->hDevice,
                                                              &ctxCb);
         if (SUCCEEDED(chr))
            device->hContext = ctxCb.hContext;
         else
            DebugPrintf("%s: pfnCreateContextCb failed 0x%08x (kernel present disabled)\n",
                        __func__, chr);
      } else {
         DebugPrintf("%s: pfnCreateContextCb is NULL (kernel present disabled)\n", __func__);
      }
      DebugPrintf("WinMali: DXGI Present create-context EXIT context=%p\n",
                  device->hContext);
   }

   /* Kernel present. The backbuffer is a real D3DKMT allocation that the
    * GPU rendered into via the BoFromAllocation alias, and WinMali submits
    * are synchronous, so the pixels are in the allocation once flush()
    * returns. hDstAllocation = 0 lets the runtime route the present to the
    * proper target (DWM redirection / cross-adapter surface); dxgk then
    * calls the KMD's DxgkDdiPresent, which records the blt executed by
    * SubmitCommand.
    *
    * _Present is a DXGI DDI entry point, so it must use the DXGI callback
    * table supplied in DXGIBaseDDI.  D3DDDI_DEVICECALLBACKS::pfnPresentCb
    * is the legacy D3D callback and is legitimately NULL on the WDDM 2+
    * runtime path used by Explorer. */
   if (pSrcResource->hKMAllocation && device->hContext &&
       device->pDXGIBaseCallbacks != NULL &&
       device->pDXGIBaseCallbacks->pfnPresentCb != NULL) {
      DXGIDDICB_PRESENT presentCb;
      memset(&presentCb, 0, sizeof presentCb);
      presentCb.hSrcAllocation = pSrcResource->hKMAllocation;
      presentCb.hDstAllocation = 0;
      presentCb.pDXGIContext = pPresentData->pDXGIContext;
      presentCb.hContext = device->hContext;
      presentCb.BroadcastContextCount = 0;
      DebugPrintf("WinMali: DXGI Present pfnPresentCb ENTER table=%p cb=%p srcAlloc=%p context=%p dxgiContext=%p\n",
                  device->pDXGIBaseCallbacks,
                  (void *)device->pDXGIBaseCallbacks->pfnPresentCb,
                  (void *)(uintptr_t)pSrcResource->hKMAllocation,
                  (void *)device->hContext,
                  pPresentData->pDXGIContext);
      HRESULT hr = device->pDXGIBaseCallbacks->pfnPresentCb(device->hDevice,
                                                           &presentCb);
      DebugPrintf("WinMali: DXGI Present pfnPresentCb EXIT hr=0x%08x srcAlloc=%p context=%p\n",
                  hr, (void *)(uintptr_t)pSrcResource->hKMAllocation,
                  (void *)device->hContext);
      if (FAILED(hr)) {
         DebugPrintf("%s: pfnPresentCb failed 0x%08x\n", __func__, hr);
      }
      return hr;
   }
   DebugPrintf("%s: kernel present unavailable (hKMAlloc=%p hContext=%p dxgiCallbacks=%p pfnPresentCb=%p) - failing\n",
               __func__, (void *)(uintptr_t)pSrcResource->hKMAllocation,
               (void *)device->hContext, device->pDXGIBaseCallbacks,
               device->pDXGIBaseCallbacks
                  ? (void *)device->pDXGIBaseCallbacks->pfnPresentCb
                  : NULL);
   return E_FAIL;
#endif

   /* flush_frontbuffer is the GL/GDI WSI path; the panfrost screen may not
    * implement it (NULL). Never call through a NULL screen vfunc - that would
    * crash inside DWM's UMD instance and hang the desktop. */
   if (device->pipe->screen->flush_frontbuffer != NULL) {
      DebugPrintf("WinMali: DXGI Present flush_frontbuffer ENTER\n");
      device->pipe->screen->flush_frontbuffer(device->pipe->screen, device->pipe,
         pSrcResource->resource, 0, 0, pPresentData->pDXGIContext, 0, NULL);
      DebugPrintf("WinMali: DXGI Present flush_frontbuffer EXIT\n");
   }

   DebugPrintf("[WinMaliFlow] DXGI Present EXIT hr=0x%08x\n", S_OK);
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _GetGammaCaps --
 *
 *    Return gamma capabilities.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_GetGammaCaps( DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *GetCaps )
{
   LOG_ENTRYPOINT();

   DXGI_GAMMA_CONTROL_CAPABILITIES *pCaps;

   pCaps = GetCaps->pGammaCapabilities;

   pCaps->ScaleAndOffsetSupported = false;
   pCaps->MinConvertedValue = 0.0;
   pCaps->MaxConvertedValue = 1.0;
   pCaps->NumGammaControlPoints = 17;

   for (UINT i = 0; i < pCaps->NumGammaControlPoints; i++) {
      pCaps->ControlPointPositions[i] = (float)i / (float)(pCaps->NumGammaControlPoints - 1);
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetDisplayMode --
 *
 *    Set the resource that is used to scan out to the display.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetDisplayMode( DXGI_DDI_ARG_SETDISPLAYMODE *SetDisplayMode )
{
   LOG_UNSUPPORTED_ENTRYPOINT();

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetResourcePriority --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetResourcePriority( DXGI_DDI_ARG_SETRESOURCEPRIORITY *SetResourcePriority )
{
   LOG_ENTRYPOINT();

   /* ignore */

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _QueryResourceResidency --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_QueryResourceResidency( DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *QueryResourceResidency )
{
   LOG_ENTRYPOINT();

   for (UINT i = 0; i < QueryResourceResidency->Resources; ++i) {
      QueryResourceResidency->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate a list of resources by recreating their views with
 *    the updated rotations.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_RotateResourceIdentities( DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *RotateResourceIdentities )
{
   LOG_ENTRYPOINT();

   if (RotateResourceIdentities->Resources <= 1) {
      return S_OK;
   }

   struct pipe_context *pipe = CastPipeDevice(RotateResourceIdentities->hDevice);
   struct pipe_screen *screen = pipe->screen;

   struct pipe_resource *resource0 = CastPipeResource(RotateResourceIdentities->pResources[0]);

   assert(resource0);
   LOG_UNSUPPORTED(resource0->last_level);

   /*
    * XXX: Copying is not very efficient, but it is much simpler than the
    * alternative of recreating all views.
    */

   struct pipe_resource *temp_resource;
   temp_resource = screen->resource_create(screen, resource0);
   assert(temp_resource);
   if (!temp_resource) {
      return E_OUTOFMEMORY;
   }

   struct pipe_box src_box;
   src_box.x = 0;
   src_box.y = 0;
   src_box.z = 0;
   src_box.width  = resource0->width0;
   src_box.height = resource0->height0;
   src_box.depth  = resource0->depth0;

   for (UINT i = 0; i < RotateResourceIdentities->Resources + 1; ++i) {
      struct pipe_resource *src_resource;
      struct pipe_resource *dst_resource;

      if (i < RotateResourceIdentities->Resources) {
         src_resource = CastPipeResource(RotateResourceIdentities->pResources[i]);
      } else {
         src_resource = temp_resource;
      }

      if (i > 0) {
         dst_resource = CastPipeResource(RotateResourceIdentities->pResources[i - 1]);
      } else {
         dst_resource = temp_resource;
      }

      assert(dst_resource);
      assert(src_resource);

      pipe->resource_copy_region(pipe,
                                 dst_resource,
                                 0, // dst_level
                                 0, 0, 0, // dst_x,y,z
                                 src_resource,
                                 0, // src_level
                                 &src_box);
   }

   pipe_resource_reference(&temp_resource, NULL);

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _Blt --
 *
 *    Do a blt between two subresources. Apply MSAA resolve, format
 *    conversion and stretching.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *Blt)
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] DXGI Blt ENTER device=%p src=%p dst=%p present=%u\n",
               Blt ? (void *)(uintptr_t)Blt->hDevice : NULL,
               Blt ? (void *)(uintptr_t)Blt->hSrcResource : NULL,
               Blt ? (void *)(uintptr_t)Blt->hDstResource : NULL,
               Blt ? Blt->Flags.Present : 0);

   /* This is the windowed-DWM present path: with DWM composition on, DXGI moves
    * the frame to the screen by calling BltDXGI (this DDI, Flags.Present set) to
    * copy the app back buffer (hSrcResource) into the DWM-shared destination
    * surface (hDstResource) that DWM composites. A render-only adapter has no
    * scanout, so this copy is the ONLY way pixels reach the screen. (Previously
    * this was a no-op, so DWM's surface stayed blank -> white window.) */
   Device *pDevice = CastDevice(Blt->hDevice);
   struct pipe_resource *dst = CastPipeResource(Blt->hDstResource);
   struct pipe_resource *src = CastPipeResource(Blt->hSrcResource);
   if (pDevice == NULL || dst == NULL || src == NULL) {
      DebugPrintf("WinMali: DXGI Blt EXIT no-op device=%p src=%p dst=%p hr=0x%08x\n",
                  pDevice, src, dst, S_OK);
      return S_OK;
   }

   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_blit_info info;
   memset(&info, 0, sizeof info);

   info.dst.resource = dst;
   info.dst.level    = 0;
   info.dst.format   = dst->format;
   info.dst.box.x    = (int)Blt->DstLeft;
   info.dst.box.y    = (int)Blt->DstTop;
   info.dst.box.z    = 0;
   info.dst.box.width  = (int)Blt->DstRight  - (int)Blt->DstLeft;
   info.dst.box.height = (int)Blt->DstBottom - (int)Blt->DstTop;
   info.dst.box.depth  = 1;

   info.src.resource = src;
   info.src.level    = 0;
   info.src.format   = src->format;
   info.src.box.x    = 0;
   info.src.box.y    = 0;
   info.src.box.z    = 0;
   info.src.box.width  = src->width0;
   info.src.box.height = src->height0;
   info.src.box.depth  = 1;

   info.mask   = PIPE_MASK_RGBA;
   info.filter = PIPE_TEX_FILTER_LINEAR;

   /* Same size + format -> a straight copy avoids the blit shader path. */
   if (info.dst.box.width == info.src.box.width &&
       info.dst.box.height == info.src.box.height &&
       dst->format == src->format) {
      pipe->resource_copy_region(pipe, dst, 0,
                                 (unsigned)Blt->DstLeft, (unsigned)Blt->DstTop, 0,
                                 src, 0, &info.src.box);
   } else {
      pipe->blit(pipe, &info);
   }
   DebugPrintf("WinMali: DXGI Blt flush ENTER src=%p dst=%p\n", src, dst);
   pipe->flush(pipe, NULL, 0);
   DebugPrintf("[WinMaliFlow] DXGI Blt EXIT hr=0x%08x\n", S_OK);
   return S_OK;
}

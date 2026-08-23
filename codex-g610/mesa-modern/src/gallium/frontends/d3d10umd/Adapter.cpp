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
 * Adpater.cpp --
 *    Driver entry point.
 */


#include "DriverIncludes.h"
#include "Device.h"
#include "State.h"

#include "Debug.h"

#include "util/u_memory.h"
#include "util/u_process.h"

#include <string.h>


EXTERN_C struct pipe_screen *
d3d10_create_screen(void);


static HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER hAdapter);

static unsigned long numAdapters = 0;
#if 0
static unsigned long memdbg_no = 0;
#endif

/*
 * ----------------------------------------------------------------------
 *
 * OpenAdapterCommon --
 *
 *    Common code for OpenAdapter10 and OpenAdapter10_2
 *
 * ----------------------------------------------------------------------
 */


static HRESULT
OpenAdapterCommon(__inout D3D10DDIARG_OPENADAPTER *pOpenData)   // IN
{
   DebugPrintf("[WinMaliFlow] OpenAdapter process=%s pid=%lu interface=0x%08x version=0x%08x\n",
               util_get_process_name() ? util_get_process_name() : "<unknown>",
               GetCurrentProcessId(), pOpenData->Interface, pOpenData->Version);
   DebugPrintf("[WinMaliUmdDiag] OpenAdapterCommon ENTER data=%p rtAdapter=%p interface=0x%08x version=0x%08x callbacks=%p table=%p\n",
               pOpenData, pOpenData->hRTAdapter.handle,
               pOpenData->Interface, pOpenData->Version,
               pOpenData->pAdapterCallbacks, pOpenData->pAdapterFuncs_2);
   if (pOpenData->pAdapterCallbacks) {
      DebugPrintf("[WinMaliUmdDiag] runtime callbacks: QueryAdapterInfo=%p GetMultisampleMethodList=%p",
                  pOpenData->pAdapterCallbacks->pfnQueryAdapterInfoCb,
                  pOpenData->pAdapterCallbacks->pfnGetMultisampleMethodListCb);
#if D3D_UMD_INTERFACE_VERSION >= D3D_UMD_INTERFACE_VERSION_WDDM2_4
      DebugPrintf(" QueryAdapterInfo2=%p",
                  pOpenData->pAdapterCallbacks->pfnQueryAdapterInfoCb2);
#endif
      DebugPrintf("\n");
   }
#if 0
   if (numAdapters == 0) {
      memdbg_no = debug_memory_begin();
   }
#endif
   ++numAdapters;

   Adapter *pAdaptor = (Adapter *)calloc(sizeof *pAdaptor, 1);
   if (!pAdaptor) {
      --numAdapters;
      DebugPrintf("[WinMaliUmdDiag] OpenAdapterCommon EXIT hr=0x%08lx reason=adapter-allocation\n", E_OUTOFMEMORY);
      return E_OUTOFMEMORY;
   }

   DebugPrintf("[WinMaliUmdDiag] OpenAdapterCommon: calling d3d10_create_screen adapter=%p ordinal=%u\n",
               pAdaptor, numAdapters);
   pAdaptor->screen = d3d10_create_screen();
   DebugPrintf("[WinMaliUmdDiag] OpenAdapterCommon: d3d10_create_screen returned screen=%p adapter=%p ordinal=%u\n",
               pAdaptor->screen, pAdaptor, numAdapters);
   if (!pAdaptor->screen) {
      free(pAdaptor);
      --numAdapters;
      DebugPrintf("[WinMaliUmdDiag] OpenAdapterCommon EXIT hr=0x%08lx reason=screen-create\n", E_OUTOFMEMORY);
      return E_OUTOFMEMORY;
   }

   pOpenData->hAdapter.pDrvPrivate = pAdaptor;

   pOpenData->pAdapterFuncs->pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
   pOpenData->pAdapterFuncs->pfnCreateDevice = CreateDevice;
   pOpenData->pAdapterFuncs->pfnCloseAdapter = CloseAdapter;
   DebugPrintf("[WinMaliUmdDiag] adapter result: drvAdapter=%p screen=%p table=%p\n",
               pOpenData->hAdapter.pDrvPrivate, pAdaptor->screen,
               pOpenData->pAdapterFuncs_2);
   DebugPrintf("[WinMaliUmdDiag] adapter funcs: CalcPrivateDeviceSize=%p CreateDevice=%p CloseAdapter=%p\n",
               pOpenData->pAdapterFuncs->pfnCalcPrivateDeviceSize,
               pOpenData->pAdapterFuncs->pfnCreateDevice,
               pOpenData->pAdapterFuncs->pfnCloseAdapter);
   DebugPrintf("[WinMaliUmdDiag] OpenAdapterCommon EXIT hr=0x%08lx\n", S_OK);

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * OpenAdapter10 --
 *
 *    The OpenAdapter10 function creates a graphics adapter object
 *    that is referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */


EXTERN_C HRESULT APIENTRY
OpenAdapter10(__inout D3D10DDIARG_OPENADAPTER *pOpenData)   // IN
{
   LOG_ENTRYPOINT();
   DebugPrintf("[WinMaliUmdDiag] OpenAdapter10 ENTER data=%p interface=0x%08x version=0x%08x\n",
               pOpenData, pOpenData ? pOpenData->Interface : 0,
               pOpenData ? pOpenData->Version : 0);

   /*
    * This is checked here and not on the common code because MSDN docs
    * state that it should be ignored on OpenAdapter10_2.
    */
   switch (pOpenData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
#if SUPPORT_D3D10_1
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
#endif
#if SUPPORT_D3D11
   case D3D11_0_DDI_INTERFACE_VERSION:
   case D3D11_0_7_DDI_INTERFACE_VERSION:
#endif
      break;
   default:
      if (0) {
         DebugPrintf("%s: unsupported interface version 0x%08x\n",
                     __func__, pOpenData->Interface);
      }
      DebugPrintf("[WinMaliUmdDiag] OpenAdapter10 EXIT hr=0x%08lx reason=unsupported-interface interface=0x%08x\n",
                  E_FAIL, pOpenData->Interface);
      return E_FAIL;
   }

   HRESULT hr = OpenAdapterCommon(pOpenData);
   DebugPrintf("[WinMaliUmdDiag] OpenAdapter10 EXIT hr=0x%08lx\n", hr);
   return hr;
}


static const UINT64
SupportedDDIInterfaceVersions[] = {
   D3D10_0_DDI_SUPPORTED,
   D3D10_0_x_DDI_SUPPORTED,
   D3D10_0_7_DDI_SUPPORTED,
#if SUPPORT_D3D10_1
   D3D10_1_DDI_SUPPORTED,
   D3D10_1_x_DDI_SUPPORTED,
   D3D10_1_7_DDI_SUPPORTED,
#endif
#if SUPPORT_D3D11
   D3D11_0_DDI_SUPPORTED,
   D3D11_0_7_DDI_SUPPORTED,
#endif
};


/*
 * ----------------------------------------------------------------------
 *
 * GetSupportedVersions --
 *
 *    Return a list of interface versions supported by the graphics
 *    adapter.
 *
 * ----------------------------------------------------------------------
 */

static HRESULT APIENTRY
GetSupportedVersions(D3D10DDI_HADAPTER hAdapter,
                     UINT32 *puEntries,
                     UINT64 *pSupportedDDIInterfaceVersions)
{
   LOG_ENTRYPOINT();
   UINT32 inputEntries = puEntries ? *puEntries : 0;
   DebugPrintf("[WinMaliUmdDiag] GetSupportedVersions ENTER adapter=%p entriesIn=%u versions=%p supportedCount=%u SUPPORT_D3D11=%u\n",
               hAdapter.pDrvPrivate, inputEntries,
               pSupportedDDIInterfaceVersions,
               (unsigned)ARRAYSIZE(SupportedDDIInterfaceVersions),
               (unsigned)SUPPORT_D3D11);

   if (pSupportedDDIInterfaceVersions &&
       *puEntries < ARRAYSIZE(SupportedDDIInterfaceVersions)) {
      DebugPrintf("[WinMaliUmdDiag] GetSupportedVersions EXIT hr=0x%08lx entriesOut=%u\n",
                  E_OUTOFMEMORY, (unsigned)ARRAYSIZE(SupportedDDIInterfaceVersions));
      return E_OUTOFMEMORY;
   }

   *puEntries = ARRAYSIZE(SupportedDDIInterfaceVersions);

   if (pSupportedDDIInterfaceVersions) {
      memcpy(pSupportedDDIInterfaceVersions,
             SupportedDDIInterfaceVersions,
             sizeof SupportedDDIInterfaceVersions);
      for (UINT32 i = 0; i < *puEntries; ++i)
         DebugPrintf("[WinMaliUmdDiag] GetSupportedVersions version[%u]=0x%016llx\n",
                     i, (unsigned long long)pSupportedDDIInterfaceVersions[i]);
   }
   DebugPrintf("[WinMaliUmdDiag] GetSupportedVersions EXIT hr=0x%08lx entriesOut=%u\n", S_OK, *puEntries);

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * GetCaps --
 *
 *    Return the capabilities of the graphics adapter.
 *
 * ----------------------------------------------------------------------
 */

static HRESULT APIENTRY
GetCaps(D3D10DDI_HADAPTER hAdapter,
        const D3D10_2DDIARG_GETCAPS *pData)
{
   LOG_ENTRYPOINT();
   DebugPrintf("[WinMaliUmdDiag] GetCaps ENTER adapter=%p data=%p type=%u info=%p output=%p size=%u\n",
               hAdapter.pDrvPrivate, pData, pData ? (unsigned)pData->Type : 0,
               pData ? pData->pInfo : NULL, pData ? pData->pData : NULL,
               pData ? pData->DataSize : 0);
   memset(pData->pData, 0, pData->DataSize);
   DebugPrintf("[WinMaliUmdDiag] GetCaps EXIT hr=0x%08lx type=%u size=%u\n",
               S_OK, (unsigned)pData->Type, pData->DataSize);
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * OpenAdapter10_2 --
 *
 *    The OpenAdapter10_2 function creates a graphics adapter object
 *    that is referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */


EXTERN_C HRESULT APIENTRY
OpenAdapter10_2(__inout D3D10DDIARG_OPENADAPTER *pOpenData)   // IN
{
   LOG_ENTRYPOINT();
   DebugPrintf("[WinMaliUmdDiag] OpenAdapter10_2 ENTER data=%p interface=0x%08x version=0x%08x\n",
               pOpenData, pOpenData ? pOpenData->Interface : 0,
               pOpenData ? pOpenData->Version : 0);

   HRESULT hr = OpenAdapterCommon(pOpenData);

   if (SUCCEEDED(hr)) {
      pOpenData->pAdapterFuncs_2->pfnGetSupportedVersions = GetSupportedVersions;
      pOpenData->pAdapterFuncs_2->pfnGetCaps = GetCaps;
      DebugPrintf("[WinMaliUmdDiag] adapter funcs2: CalcPrivateDeviceSize=%p CreateDevice=%p CloseAdapter=%p GetSupportedVersions=%p GetCaps=%p\n",
                  pOpenData->pAdapterFuncs_2->pfnCalcPrivateDeviceSize,
                  pOpenData->pAdapterFuncs_2->pfnCreateDevice,
                  pOpenData->pAdapterFuncs_2->pfnCloseAdapter,
                  pOpenData->pAdapterFuncs_2->pfnGetSupportedVersions,
                  pOpenData->pAdapterFuncs_2->pfnGetCaps);
   }

   DebugPrintf("[WinMaliUmdDiag] OpenAdapter10_2 EXIT hr=0x%08lx drvAdapter=%p\n",
               hr, pOpenData ? pOpenData->hAdapter.pDrvPrivate : NULL);
   return hr;
}


/*
 * ----------------------------------------------------------------------
 *
 * CloseAdapter --
 *
 *    The CloseAdapter function releases resources for a
 *    graphics adapter object.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
CloseAdapter(D3D10DDI_HADAPTER hAdapter)  // IN
{
   LOG_ENTRYPOINT();
   DebugPrintf("[WinMaliUmdDiag] CloseAdapter ENTER adapter=%p\n",
               hAdapter.pDrvPrivate);

   Adapter *pAdapter = CastAdapter(hAdapter);
   struct pipe_screen *screen = pAdapter->screen;
   screen->destroy(screen);
   free(pAdapter);

   --numAdapters;
#if 0
   if (numAdapters == 0) {
      debug_memory_end(memdbg_no);
   }
#endif
   DebugPrintf("[WinMaliUmdDiag] CloseAdapter EXIT hr=0x%08lx\n", S_OK);

   return S_OK;
}

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
 * Device.cpp --
 *    Functions that provide the 3D device functionality.
 */


#include "Draw.h"
#include "DxgiFns.h"
#include "InputAssembly.h"
#include "OutputMerger.h"
#include "Query.h"
#include "Rasterizer.h"
#include "Resource.h"
#include "Shader.h"
#include "State.h"
#include "Format.h"

#include "Debug.h"

#include "util/u_sampler.h"
#include "util/u_framebuffer.h"

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
#include <stdlib.h>                 /* getenv */
#include "WinMaliEscape.h"          /* WINMALI_ALLOCATION_PRIVATE */
#include "WinMaliWddmBridge.h"      /* winmali_wddm_alloc_ops + registration */

int
WinMaliWddmEnsurePagingQueue(Device *pDevice)
{
   if (!pDevice->hPagingQueueTried) {
      pDevice->hPagingQueueTried = TRUE;

      D3DDDICB_CREATEPAGINGQUEUE cpq;
      memset(&cpq, 0, sizeof cpq);
      cpq.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;

      HRESULT hr = E_NOINTERFACE;
      if (pDevice->KTCallbacks.pfnCreatePagingQueueCb != NULL)
         hr = pDevice->KTCallbacks.pfnCreatePagingQueueCb(pDevice->hDevice,
                                                           &cpq);
      if (SUCCEEDED(hr)) {
         pDevice->hPagingQueue = cpq.hPagingQueue;
         pDevice->hPagingFence = cpq.hSyncObject;
         DebugPrintf("WinMali: paging queue ready queue=%u fence=%u\n",
                     pDevice->hPagingQueue, pDevice->hPagingFence);
      } else {
         DebugPrintf("WinMali: paging-queue create failed 0x%08x\n", hr);
         pDevice->hPagingQueue = 0;
         pDevice->hPagingFence = 0;
      }
   }

   return pDevice->hPagingQueue != 0 && pDevice->hPagingFence != 0 ? 0 : -1;
}

int
WinMaliWddmWaitPagingFence(Device *pDevice, UINT64 value,
                           const char *operation, uint32_t kmt_handle)
{
   if (value == 0)
      return 0;

   if (pDevice->hPagingFence == 0 ||
       pDevice->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb == NULL) {
      DebugPrintf("WinMali: %s cannot wait paging fence alloc=%u value=%llu\n",
                  operation, kmt_handle, (unsigned long long)value);
      return -1;
   }

   D3DKMT_HANDLE fence = pDevice->hPagingFence;
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;
   memset(&wait, 0, sizeof wait);
   wait.ObjectCount       = 1;
   wait.ObjectHandleArray = &fence;
   wait.FenceValueArray   = &value;
   wait.hAsyncEvent       = NULL;

   HRESULT hr = pDevice->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb(
      pDevice->hDevice, &wait);
   if (FAILED(hr)) {
      DebugPrintf("WinMali: %s paging-fence wait failed alloc=%u value=%llu hr=0x%08x\n",
                  operation, kmt_handle, (unsigned long long)value, hr);
      return -1;
   }

   return 0;
}

void
WinMaliWddmEvictAllocation(Device *pDevice, uint32_t kmt_handle)
{
   if (kmt_handle == 0 || pDevice->KTCallbacks.pfnEvictCb == NULL)
      return;

   D3DKMT_HANDLE allocation = kmt_handle;
   D3DDDICB_EVICT evict;
   memset(&evict, 0, sizeof evict);
   evict.NumAllocations = 1;
   evict.AllocationList = &allocation;

   HRESULT hr = pDevice->KTCallbacks.pfnEvictCb(pDevice->hDevice, &evict);
   if (FAILED(hr)) {
      DebugPrintf("WinMali: Evict failed alloc=%u hr=0x%08x trim=%llu\n",
                  kmt_handle, hr,
                  (unsigned long long)evict.NumBytesToTrim);
   } else {
      DebugPrintf("WinMali: Evict complete alloc=%u\n", kmt_handle);
   }
}

/* Phase 1 WDDM-allocation bridge (C++ side). These closures over the device's
 * dxgk callbacks are installed on the panfrost pan_kmod backend so that
 * winmali_kmod_bo_alloc backs every BO with a real, kernel-tracked dxgk
 * allocation (created + pinned resident + CPU-mapped) addressed via GpuMmu,
 * instead of an escape-private BoCreate. Installed unconditionally for every
 * D3D device - the dxgk-native data plane is the only path. */

static int
WinMaliWddmAllocCb(void *cookie, uint64_t size, uint32_t *out_kmt_handle,
                   void **out_cpu)
{
   Device *pDevice = static_cast<Device *>(cookie);
   if (out_cpu)
      *out_cpu = NULL;

   WINMALI_ALLOCATION_PRIVATE priv;
   memset(&priv, 0, sizeof priv);
   priv.Magic = WINMALI_ALLOCATION_PRIVATE_MAGIC;
   priv.Size  = size;               /* opaque byte buffer; W/H/Stride = 0 */

   D3DDDI_ALLOCATIONINFO ai;
   memset(&ai, 0, sizeof ai);
   ai.pPrivateDriverData    = &priv;
   ai.PrivateDriverDataSize = sizeof priv;

   D3DDDICB_ALLOCATE allocCb;
   memset(&allocCb, 0, sizeof allocCb);
   allocCb.hResource       = NULL;  /* standalone, driver-owned allocation */
   allocCb.NumAllocations  = 1;
   allocCb.pAllocationInfo = &ai;

   HRESULT hr = pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &allocCb);
   if (FAILED(hr) || ai.hAllocation == 0)
      return -1;

   /* WDDM 2+ GPUVA mappings and residency are separate operations. Acquire one
    * residency reference for the BO's lifetime before mapping or submitting it.
    * LockCb below provides persistent CPU access; it is not a substitute for
    * adding the allocation to the device residency list. */
   if (WinMaliWddmEnsurePagingQueue(pDevice) != 0 ||
       pDevice->KTCallbacks.pfnMakeResidentCb == NULL) {
      DebugPrintf("WinMali: MakeResident unavailable alloc=%u\n",
                  ai.hAllocation);
      D3DKMT_HANDLE handleList[1] = { ai.hAllocation };
      D3DDDICB_DEALLOCATE dealloc;
      memset(&dealloc, 0, sizeof dealloc);
      dealloc.NumAllocations = 1;
      dealloc.HandleList = handleList;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
      return -1;
   }

   D3DKMT_HANDLE residentAllocation = ai.hAllocation;
   D3DDDI_MAKERESIDENT resident;
   memset(&resident, 0, sizeof resident);
   resident.hPagingQueue   = pDevice->hPagingQueue;
   resident.NumAllocations = 1;
   resident.AllocationList = &residentAllocation;

   hr = pDevice->KTCallbacks.pfnMakeResidentCb(pDevice->hDevice, &resident);
   if ((FAILED(hr) && hr != E_PENDING) || resident.NumAllocations != 1 ||
       WinMaliWddmWaitPagingFence(pDevice, resident.PagingFenceValue,
                                  "MakeResident", ai.hAllocation) != 0) {
      DebugPrintf("WinMali: MakeResident failed alloc=%u hr=0x%08x made=%u fence=%llu trim=%llu\n",
                  ai.hAllocation, hr, resident.NumAllocations,
                  (unsigned long long)resident.PagingFenceValue,
                  (unsigned long long)resident.NumBytesToTrim);
      if (resident.NumAllocations != 0)
         WinMaliWddmEvictAllocation(pDevice, ai.hAllocation);
      D3DKMT_HANDLE handleList[1] = { ai.hAllocation };
      D3DDDICB_DEALLOCATE dealloc;
      memset(&dealloc, 0, sizeof dealloc);
      dealloc.NumAllocations = 1;
      dealloc.HandleList = handleList;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
      return -1;
   }

   DebugPrintf("WinMali: MakeResident complete alloc=%u fence=%llu\n",
               ai.hAllocation,
               (unsigned long long)resident.PagingFenceValue);

   /* Pin resident so BoFromAllocation can snapshot the pages (the KMD
    * rejects it with EAGAIN otherwise). Held until the BO is freed. */
   D3DDDICB_LOCK lockCb;
   memset(&lockCb, 0, sizeof lockCb);
   lockCb.hAllocation      = ai.hAllocation;
   lockCb.Flags.LockEntire = 1;
   hr = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lockCb);
   if (FAILED(hr)) {
      WinMaliWddmEvictAllocation(pDevice, ai.hAllocation);
      D3DKMT_HANDLE handleList[1] = { ai.hAllocation };
      D3DDDICB_DEALLOCATE dealloc;
      memset(&dealloc, 0, sizeof dealloc);
      dealloc.hResource      = NULL;
      dealloc.NumAllocations = 1;
      dealloc.HandleList     = handleList;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
      return -1;
   }

   *out_kmt_handle = ai.hAllocation;
   /* pData is the persistent CPU VA of the pinned allocation; hand it back so
    * the BO needs no escape BoMapCpu. */
   if (out_cpu)
      *out_cpu = lockCb.pData;
   return 0;
}

static void
WinMaliWddmFreeCb(void *cookie, uint32_t kmt_handle)
{
   Device *pDevice = static_cast<Device *>(cookie);
   if (kmt_handle == 0)
      return;

   D3DDDICB_UNLOCK unlockCb;
   memset(&unlockCb, 0, sizeof unlockCb);
   unlockCb.NumAllocations = 1;
   unlockCb.phAllocations  = &kmt_handle;
   pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &unlockCb);

   /* Balance the BO-lifetime reference acquired by pfnMakeResidentCb before
    * releasing the allocation itself. */
   WinMaliWddmEvictAllocation(pDevice, kmt_handle);

   D3DKMT_HANDLE handleList[1] = { kmt_handle };
   D3DDDICB_DEALLOCATE dealloc;
   memset(&dealloc, 0, sizeof dealloc);
   dealloc.hResource      = NULL;
   dealloc.NumAllocations = 1;
   dealloc.HandleList     = handleList;
   pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
}

/* Establish the GPUVA context before the first allocation is mapped. VidMm
 * owns the process page tables, while SetRootPageTable associates those page
 * tables with this context in the KMD. Keeping one helper for map and submit
 * makes it impossible to queue a DXGK-addressed stream without first creating
 * the context whose root table the KMD must bind. */
static int
WinMaliWddmEnsureRenderContext(Device *pDevice)
{
   if (!pDevice->hRenderContextTried) {
      pDevice->hRenderContextTried = TRUE;
      D3DDDICB_CREATECONTEXTVIRTUAL ctxCb;
      memset(&ctxCb, 0, sizeof ctxCb);
      ctxCb.NodeOrdinal    = 0;
      ctxCb.EngineAffinity = 0;
      HRESULT chr = E_NOINTERFACE;
      if (pDevice->KTCallbacks.pfnCreateContextVirtualCb != NULL)
         chr = pDevice->KTCallbacks.pfnCreateContextVirtualCb(pDevice->hDevice,
                                                               &ctxCb);
      if (SUCCEEDED(chr)) {
         pDevice->hRenderContext = ctxCb.hContext;
         DebugPrintf("WinMali: virtual render-context ready context=%p\n",
                     pDevice->hRenderContext);
      } else {
         DebugPrintf("WinMali: virtual render-context create failed 0x%08x\n", chr);
         pDevice->hRenderContext = NULL;
      }
   }
   return pDevice->hRenderContext != NULL ? 0 : -1;
}

/* Phase 3 UMD half: submit a GPUVA command stream through dxgk. WDDM 2+
 * contexts which generate final GPU virtual addresses must use
 * pfnCreateContextVirtualCb + pfnSubmitCommandCb; pfnRenderCb is the legacy
 * patch-list path and routes to DxgkDdiSubmitCommand instead. */
static int
WinMaliWddmRenderCb(void *cookie, uint32_t vm_id, uint64_t stream_va,
                    uint32_t stream_size, const void *sync_ops,
                    uint32_t sync_count)
{
   Device *pDevice = static_cast<Device *>(cookie);

   DebugPrintf("[WinMaliFlow] WDDMRender ENTER device=%p vm=%u stream=0x%llx size=%u syncs=%u\n",
               pDevice, vm_id, (unsigned long long)stream_va, stream_size,
               sync_count);

   if (WinMaliWddmEnsureRenderContext(pDevice) != 0) {
      DebugPrintf("[WinMaliFlow] WDDMRender EXIT device=%p result=no-render-context\n",
                  pDevice);
      return -1;
   }

   if (sync_count > WINMALI_CS_SUBMIT_MAX_SYNCS ||
       (sync_count != 0 && sync_ops == NULL)) {
      DebugPrintf("WinMali: render sync list invalid count=%u ptr=%p\n",
                  sync_count, sync_ops);
      DebugPrintf("[WinMaliFlow] WDDMRender EXIT device=%p result=invalid-sync-list\n",
                  pDevice);
      return -1;
   }

   WINMALI_CS_SUBMIT_DESC desc;
   memset(&desc, 0, sizeof desc);
   desc.Magic       = WINMALI_CS_SUBMIT_DESC_MAGIC;
   desc.VmId        = vm_id;
   desc.StreamGpuVa = stream_va;
   desc.StreamSize  = stream_size;
   /* Run this CS against the dxgk render context's AS: the UMD maps all its
    * allocations via dxgk GpuMmu now, so that AS is where the geometry lives. */
   desc.Flags |= WINMALI_CS_SUBMIT_FLAG_DXGK_ADDR;
   desc.SyncCount = sync_count;
   if (sync_count != 0)
      memcpy(desc.SyncOps, sync_ops,
             sync_count * sizeof(WINMALI_SYNC_OP));

   /* Replace every binary signal syncobj's old fence before submission.
    * dxgkrnl may return from pfnSubmitCommandCb before it dispatches the packet to
    * SubmitCommandVirtual; arming later in the KMD leaves a window where an
    * immediate Mesa wait incorrectly observes the previous signalled state. */
   uint32_t armed_syncs = 0;
   for (uint32_t i = 0; i < desc.SyncCount; ++i) {
      if ((desc.SyncOps[i].Flags & WINMALI_SYNC_OP_SIGNAL) == 0)
         continue;
      if (winmali_wddm_syncobj_arm(desc.SyncOps[i].Handle,
                                   &desc.SyncOps[i].TimelineValue) != 0) {
         DebugPrintf("WinMali: render sync arm failed handle=%u index=%u\n",
                     desc.SyncOps[i].Handle, i);
         for (uint32_t j = 0; j < i; ++j) {
            if (desc.SyncOps[j].Flags & WINMALI_SYNC_OP_SIGNAL)
               winmali_wddm_syncobj_complete(
                  desc.SyncOps[j].Handle, desc.SyncOps[j].TimelineValue);
         }
         DebugPrintf("[WinMaliFlow] WDDMRender EXIT device=%p result=sync-arm-failed index=%u\n",
                     pDevice, i);
         return -1;
      }
      ++armed_syncs;
   }

   D3DDDICB_SUBMITCOMMAND submit;
   memset(&submit, 0, sizeof submit);
   submit.Commands                  = stream_va;
   submit.CommandLength             = stream_size;
   submit.BroadcastContextCount     = 1;
   submit.BroadcastContext[0]       = pDevice->hRenderContext;
   submit.pPrivateDriverData        = &desc;
   submit.PrivateDriverDataSize     = sizeof desc;
   submit.NumPrimaries              = 0;

   HRESULT hr = E_NOINTERFACE;
   if (pDevice->KTCallbacks.pfnSubmitCommandCb != NULL)
      hr = pDevice->KTCallbacks.pfnSubmitCommandCb(pDevice->hDevice, &submit);
   if (FAILED(hr)) {
      DebugPrintf("WinMali: pfnSubmitCommandCb failed 0x%08x\n", hr);
      /* The render never became GPU-visible. Retire the generations armed
       * above so Mesa can release the failed batch rather than deadlock. */
      if (armed_syncs != 0) {
         for (uint32_t i = 0; i < desc.SyncCount; ++i) {
            if (desc.SyncOps[i].Flags & WINMALI_SYNC_OP_SIGNAL)
               winmali_wddm_syncobj_complete(
                  desc.SyncOps[i].Handle, desc.SyncOps[i].TimelineValue);
         }
      }
      DebugPrintf("[WinMaliFlow] WDDMRender EXIT device=%p result=submit-failed hr=0x%08x\n",
                  pDevice, hr);
      return -1;
   }

   /* The KMD copied the stream and sync metadata into its scheduler queue.
    * It signals Mesa's supplied syncobjs only after the real CS reaches a
    * terminal state, so returning here neither blocks the application thread
    * nor permits batch BOs to be recycled prematurely. */
   DebugPrintf("[WinMaliFlow] WDDMRender EXIT device=%p vm=%u stream=0x%llx size=%u syncs=%u result=queued\n",
               pDevice, vm_id, (unsigned long long)stream_va, stream_size,
               sync_count);
   return 0;
}

/* Phase 2 UMD half: map a dxgk allocation into the GPU virtual address space via
 * pfnMapGpuVirtualAddressCb, retiring escape VmBind for dxgk-backed BOs. The map
 * is asynchronous on a paging queue; we create the queue lazily and block on its
 * monitored fence so the VA is live before returning to panfrost. Opt-in via
 * WINMALI_DXGK_ADDR (gated in winmali_kmod.c). */
static int
WinMaliWddmMapVaCb(void *cookie, uint32_t kmt_handle, uint64_t offset_bytes,
                   uint64_t size, uint32_t map_flags, uint64_t *inout_va)
{
   Device *pDevice = static_cast<Device *>(cookie);
   if (kmt_handle == 0)
      return -1;

   if (WinMaliWddmEnsureRenderContext(pDevice) != 0)
      return -1;

   if (WinMaliWddmEnsurePagingQueue(pDevice) != 0)
      return -1;

   D3DDDI_MAPGPUVIRTUALADDRESS m;
   memset(&m, 0, sizeof m);
   const uint64_t requested_va = *inout_va;
   m.hPagingQueue     = pDevice->hPagingQueue;
   /* Zero requests that VidMm select a process-wide free GPUVA. A nonzero
    * value is an explicit-address mapping and must be honored exactly. */
   m.BaseAddress      = (D3DGPU_VIRTUAL_ADDRESS)requested_va;
   m.hAllocation      = kmt_handle;
   m.OffsetInPages    = offset_bytes >> 12;
   m.SizeInPages      = (size + 0xFFF) >> 12;
   const bool read_only =
      (map_flags & WINMALI_WDDM_MAP_READONLY) != 0;
   const bool executable =
      (map_flags & WINMALI_WDDM_MAP_EXECUTE) != 0;
   /* D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE is affirmative: leaving Execute
    * clear makes VidMm supply DXGK_PTE.NoExecute=1.  Shader/code BOs must be
    * RX, while ordinary descriptors/data/command-storage remain RW+NX. */
   m.Protection.Write   = read_only ? 0 : 1;
   m.Protection.Execute = executable ? 1 : 0;
   HRESULT hr = pDevice->KTCallbacks.pfnMapGpuVirtualAddressCb(pDevice->hDevice, &m);
   /* E_PENDING is NOT a failure: the map was queued on the paging queue and
    * completes asynchronously - the VA is already assigned, and we block on
    * PagingFenceValue below before returning. Only a real error bails. */
   if (FAILED(hr) && hr != E_PENDING) {
      DebugPrintf("WinMali: MapGpuVirtualAddress failed 0x%08x (va=%llx)\n",
                  hr, (unsigned long long)*inout_va);
      return -1;
   }

   /* Block until the paging op is complete so the VA is usable on return. */
   if (WinMaliWddmWaitPagingFence(pDevice, m.PagingFenceValue,
                                  "MapGpuVirtualAddress", kmt_handle) != 0)
      return -1;

   if (m.VirtualAddress == 0) {
      DebugPrintf("WinMali: MapGpuVirtualAddress returned a zero VA (requested=%llx)\n",
                  (unsigned long long)*inout_va);
      return -1;
   }

   if (requested_va != 0 &&
       m.VirtualAddress != (D3DGPU_VIRTUAL_ADDRESS)requested_va) {
      DebugPrintf("WinMali: dxgk assigned VA %llx != requested %llx\n",
                  (unsigned long long)m.VirtualAddress,
                  (unsigned long long)requested_va);
   }
   *inout_va = (uint64_t)m.VirtualAddress;
   DebugPrintf("WinMali: GPUVA map ready alloc=%u va=%llx pages=%llu prot=%s%s flags=0x%x fence=%llu hr=0x%08x\n",
               kmt_handle, (unsigned long long)m.VirtualAddress,
               (unsigned long long)m.SizeInPages,
               read_only ? "R" : "RW", executable ? "X" : "NX", map_flags,
               (unsigned long long)m.PagingFenceValue, hr);
   return 0;
}

static void
WinMaliWddmUnmapVaCb(void *cookie, uint64_t va, uint64_t size)
{
   Device *pDevice = static_cast<Device *>(cookie);
   D3DDDICB_FREEGPUVIRTUALADDRESS f;
   memset(&f, 0, sizeof f);
   f.BaseAddress = (D3DGPU_VIRTUAL_ADDRESS)va;
   f.Size        = (D3DGPU_SIZE_T)size;
   pDevice->KTCallbacks.pfnFreeGpuVirtualAddressCb(pDevice->hDevice, &f);
}

#endif /* _WIN32 && HAVE_WINMALI_WDDM_KMOD */

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
EXTERN_C struct pipe_screen *
d3d10_create_device_screen(const struct winmali_wddm_alloc_ops *wddm_ops);
#else
EXTERN_C struct pipe_screen *
d3d10_create_device_screen(void);
#endif


static void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice);
static void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY Flush(D3D10DDI_HDEVICE hDevice);
static void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                               __out UINT *pFormatCaps);
static void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                          DXGI_FORMAT Format,
                                          UINT SampleCount,
                                          __out UINT *pNumQualityLevels);
static void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE hDevice, UINT Width, UINT Height);


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateDeviceSize --
 *
 *    The CalcPrivateDeviceSize function determines the size of a memory
 *    region that the user-mode display driver requires from the Microsoft
 *    Direct3D runtime to store frequently-accessed data.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,                          // IN
                      __in const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData) // IN
{
   DebugPrintf("[WinMaliUmdDiag] CalcPrivateDeviceSize ENTER adapter=%p data=%p interface=0x%08x version=0x%08x flags=0x%08x\n",
               hAdapter.pDrvPrivate, pData, pData ? pData->Interface : 0,
               pData ? pData->Version : 0, pData ? pData->Flags : 0);
   const SIZE_T size = sizeof(Device);
   DebugPrintf("[WinMaliUmdDiag] CalcPrivateDeviceSize EXIT size=%llu\n",
               (unsigned long long)size);
   return size;
}

/*
 * ----------------------------------------------------------------------
 *
 * CreateDevice --
 *
 *    The CreateDevice function creates a graphics context that is
 *    referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
CreateDevice(D3D10DDI_HADAPTER hAdapter,                 // IN
             __in D3D10DDIARG_CREATEDEVICE *pCreateData) // IN
{
   LOG_ENTRYPOINT();
   DebugPrintf("[WinMaliUmdDiag] CreateDevice ENTER adapter=%p data=%p interface=0x%08x version=0x%08x flags=0x%08x drvDevice=%p rtDevice=%p ktCallbacks=%p deviceFuncs=%p dxgiFuncs=%p umCallbacks=%p\n",
               hAdapter.pDrvPrivate, pCreateData,
               pCreateData ? pCreateData->Interface : 0,
               pCreateData ? pCreateData->Version : 0,
               pCreateData ? pCreateData->Flags : 0,
               pCreateData ? pCreateData->hDrvDevice.pDrvPrivate : NULL,
               pCreateData ? pCreateData->hRTDevice.handle : NULL,
               pCreateData ? pCreateData->pKTCallbacks : NULL,
               pCreateData ? pCreateData->pDeviceFuncs : NULL,
               pCreateData ? pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions : NULL,
               pCreateData ? pCreateData->pUMCallbacks : NULL);

   if (0) {
      DebugPrintf("hAdapter = %p\n", hAdapter);
      DebugPrintf("pKTCallbacks = %p\n", pCreateData->pKTCallbacks);
      DebugPrintf("p10_1DeviceFuncs = %p\n", pCreateData->p10_1DeviceFuncs);
      DebugPrintf("hDrvDevice = %p\n", pCreateData->hDrvDevice);
      DebugPrintf("DXGIBaseDDI = %p\n", pCreateData->DXGIBaseDDI);
      DebugPrintf("hRTCoreLayer = %p\n", pCreateData->hRTCoreLayer);
      DebugPrintf("pUMCallbacks = %p\n", pCreateData->pUMCallbacks);
   }

   switch (pCreateData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
#if SUPPORT_D3D10_1
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
#endif
      break;
   default:
      DebugPrintf("%s: unsupported interface version 0x%08x\n",
                  __func__, pCreateData->Interface);
      DebugPrintf("[WinMaliUmdDiag] CreateDevice EXIT hr=0x%08lx reason=unsupported-interface interface=0x%08x\n",
                  E_FAIL, pCreateData->Interface);
      return E_FAIL;
   }

   Device *pDevice = CastDevice(pCreateData->hDrvDevice);
   memset(pDevice, 0, sizeof *pDevice);

   /* The runtime callbacks and hDevice are the ownership root for every WDDM
    * allocation this Gallium device creates.  Publish them before creating
    * the private screen/context so context bootstrap can never borrow a stale
    * callback cookie from another D3D device. */
   pDevice->hRTCoreLayer = pCreateData->hRTCoreLayer;
   pDevice->hDevice = (HANDLE)pCreateData->hRTDevice.handle;
   pDevice->KTCallbacks = *pCreateData->pKTCallbacks;
   pDevice->UMCallbacks = *pCreateData->pUMCallbacks;
   pDevice->pDXGIBaseCallbacks = pCreateData->DXGIBaseDDI.pDXGIBaseCallbacks;

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   struct winmali_wddm_alloc_ops wddm_ops;
   memset(&wddm_ops, 0, sizeof wddm_ops);
   wddm_ops.cookie   = pDevice;
   wddm_ops.render   = WinMaliWddmRenderCb;
   wddm_ops.alloc    = WinMaliWddmAllocCb;
   wddm_ops.free     = WinMaliWddmFreeCb;
   wddm_ops.map_va   = WinMaliWddmMapVaCb;
   wddm_ops.unmap_va = WinMaliWddmUnmapVaCb;
   struct pipe_screen *screen = d3d10_create_device_screen(&wddm_ops);
#else
   struct pipe_screen *screen = d3d10_create_device_screen();
#endif
   if (!screen) {
      DebugPrintf("[WinMaliUmdDiag] CreateDevice EXIT hr=0x%08lx reason=device-screen\n",
                  E_OUTOFMEMORY);
      return E_OUTOFMEMORY;
   }
   pDevice->screen = screen;

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   {
      /* Mesa's CSF syncobjs remain on the escape control plane for now.  The
       * render callback itself supplies the WDDM GPU-ordered completion wait,
       * so an escape handle is never mistaken for a D3DKMT monitored fence. */
      DebugPrintf("WinMali: device bridge INSTALL device=%p screen=%p\n",
                  pDevice, screen);
      panfrost_winmali_set_wddm_alloc_ops(screen, &wddm_ops);
   }
#endif

   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) {
      DebugPrintf("[WinMaliUmdDiag] CreateDevice EXIT hr=0x%08lx reason=pipe-context\n",
                  E_OUTOFMEMORY);
      screen->destroy(screen);
      pDevice->screen = NULL;
      return E_OUTOFMEMORY;
   }
   pDevice->pipe = pipe;
   pDevice->cso = cso_create_context(pipe, CSO_NO_VBUF);
   if (!pDevice->cso) {
      pipe->destroy(pipe);
      pDevice->pipe = NULL;
      screen->destroy(screen);
      pDevice->screen = NULL;
      DebugPrintf("[WinMaliUmdDiag] CreateDevice EXIT hr=0x%08lx reason=cso-context\n",
                  E_OUTOFMEMORY);
      return E_OUTOFMEMORY;
   }

   /* Bind an empty (0-element) vertex-elements state up front so the driver
    * always has one. panfrost's panfrost_emit_vertex_data dereferences
    * ctx->vertex unconditionally, and a draw sourced purely from
    * SV_VertexID never sets an input layout - worse, the D3D runtime elides
    * a redundant IASetInputLayout(NULL) (device default is already NULL), so
    * the IaSetInputLayout DDI may never fire and update_velems' NULL-layout
    * path never runs. Establishing the empty state here makes ctx->vertex
    * valid from device creation regardless. (v1.0.0.40; v39 gated this on
    * velems_changed, which the elided callback left false -> NULL deref.) */
   {
      struct cso_velems_state empty;
      memset(&empty, 0, sizeof(empty));
      cso_set_vertex_elements(pDevice->cso, &empty);
   }

   pDevice->empty_vs = CreateEmptyShader(pDevice, MESA_SHADER_VERTEX);
   pDevice->empty_fs = CreateEmptyShader(pDevice, MESA_SHADER_FRAGMENT);


   pipe->bind_vs_state(pipe, pDevice->empty_vs);
   pipe->bind_fs_state(pipe, pDevice->empty_fs);

   pDevice->max_dual_source_render_targets =
         screen->caps.max_dual_source_render_targets;

   pDevice->draw_so_target = NULL;

   /* The dxgk kernel context needed for pfnPresentCb is created LAZILY on
    * the first _Present (DxgiFns.cpp), NOT here. Creating it eagerly made
    * EVERY D3D11 device creation walk dxgk's first-real-context init,
    * which currently bugchecks the machine (observed v1.0.0.29 on HW,
    * right after the context's DMA buffer got aperture-mapped) - eager
    * creation put that in the path of every desktop process. */

   if (0) {
      DebugPrintf("pDevice = %p\n", pDevice);
   }

   st_debug_parse();

   /*
    * Fill in the D3D10 DDI functions
    */
   D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnVsSetConstantBuffers = VsSetConstantBuffers;
   pDeviceFuncs->pfnPsSetShaderResources = PsSetShaderResources;
   pDeviceFuncs->pfnPsSetShader = PsSetShader;
   pDeviceFuncs->pfnPsSetSamplers = PsSetSamplers;
   pDeviceFuncs->pfnVsSetShader = VsSetShader;
   pDeviceFuncs->pfnDrawIndexed = DrawIndexed;
   pDeviceFuncs->pfnDraw = Draw;
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = ResourceUnmap;
   pDeviceFuncs->pfnPsSetConstantBuffers = PsSetConstantBuffers;
   pDeviceFuncs->pfnIaSetInputLayout = IaSetInputLayout;
   pDeviceFuncs->pfnIaSetVertexBuffers = IaSetVertexBuffers;
   pDeviceFuncs->pfnIaSetIndexBuffer = IaSetIndexBuffer;
   pDeviceFuncs->pfnDrawIndexedInstanced = DrawIndexedInstanced;
   pDeviceFuncs->pfnDrawInstanced = DrawInstanced;
   pDeviceFuncs->pfnDynamicResourceMapDiscard = ResourceMap;
   pDeviceFuncs->pfnDynamicResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnGsSetConstantBuffers = GsSetConstantBuffers;
   pDeviceFuncs->pfnGsSetShader = GsSetShader;
   pDeviceFuncs->pfnIaSetTopology = IaSetTopology;
   pDeviceFuncs->pfnStagingResourceMap = ResourceMap;
   pDeviceFuncs->pfnStagingResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnVsSetShaderResources = VsSetShaderResources;
   pDeviceFuncs->pfnVsSetSamplers = VsSetSamplers;
   pDeviceFuncs->pfnGsSetShaderResources = GsSetShaderResources;
   pDeviceFuncs->pfnGsSetSamplers = GsSetSamplers;
   pDeviceFuncs->pfnSetRenderTargets = SetRenderTargets;
   pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard = ShaderResourceViewReadAfterWriteHazard;
   pDeviceFuncs->pfnResourceReadAfterWriteHazard = ResourceReadAfterWriteHazard;
   pDeviceFuncs->pfnSetBlendState = SetBlendState;
   pDeviceFuncs->pfnSetDepthStencilState = SetDepthStencilState;
   pDeviceFuncs->pfnSetRasterizerState = SetRasterizerState;
   pDeviceFuncs->pfnQueryEnd = QueryEnd;
   pDeviceFuncs->pfnQueryBegin = QueryBegin;
   pDeviceFuncs->pfnResourceCopyRegion = ResourceCopyRegion;
   pDeviceFuncs->pfnResourceUpdateSubresourceUP = ResourceUpdateSubResourceUP;
   pDeviceFuncs->pfnSoSetTargets = SoSetTargets;
   pDeviceFuncs->pfnDrawAuto = DrawAuto;
   pDeviceFuncs->pfnSetViewports = SetViewports;
   pDeviceFuncs->pfnSetScissorRects = SetScissorRects;
   pDeviceFuncs->pfnClearRenderTargetView = ClearRenderTargetView;
   pDeviceFuncs->pfnClearDepthStencilView = ClearDepthStencilView;
   pDeviceFuncs->pfnSetPredication = SetPredication;
   pDeviceFuncs->pfnQueryGetData = QueryGetData;
   pDeviceFuncs->pfnFlush = Flush;
   pDeviceFuncs->pfnGenMips = GenMips;
   pDeviceFuncs->pfnResourceCopy = ResourceCopy;
   pDeviceFuncs->pfnResourceResolveSubresource = ResourceResolveSubResource;
   pDeviceFuncs->pfnResourceMap = ResourceMap;
   pDeviceFuncs->pfnResourceUnmap = ResourceUnmap;
   pDeviceFuncs->pfnResourceIsStagingBusy = ResourceIsStagingBusy;
   pDeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs;
   pDeviceFuncs->pfnCalcPrivateResourceSize = CalcPrivateResourceSize;
   pDeviceFuncs->pfnCalcPrivateOpenedResourceSize = CalcPrivateOpenedResourceSize;
   pDeviceFuncs->pfnCreateResource = CreateResource;
   pDeviceFuncs->pfnOpenResource = OpenResource;
   pDeviceFuncs->pfnDestroyResource = DestroyResource;
   pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize;
   pDeviceFuncs->pfnCreateShaderResourceView = CreateShaderResourceView;
   pDeviceFuncs->pfnDestroyShaderResourceView = DestroyShaderResourceView;
   pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = CalcPrivateRenderTargetViewSize;
   pDeviceFuncs->pfnCreateRenderTargetView = CreateRenderTargetView;
   pDeviceFuncs->pfnDestroyRenderTargetView = DestroyRenderTargetView;
   pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = CalcPrivateDepthStencilViewSize;
   pDeviceFuncs->pfnCreateDepthStencilView = CreateDepthStencilView;
   pDeviceFuncs->pfnDestroyDepthStencilView = DestroyDepthStencilView;
   pDeviceFuncs->pfnCalcPrivateElementLayoutSize = CalcPrivateElementLayoutSize;
   pDeviceFuncs->pfnCreateElementLayout = CreateElementLayout;
   pDeviceFuncs->pfnDestroyElementLayout = DestroyElementLayout;
   pDeviceFuncs->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize;
   pDeviceFuncs->pfnCreateBlendState = CreateBlendState;
   pDeviceFuncs->pfnDestroyBlendState = DestroyBlendState;
   pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = CalcPrivateDepthStencilStateSize;
   pDeviceFuncs->pfnCreateDepthStencilState = CreateDepthStencilState;
   pDeviceFuncs->pfnDestroyDepthStencilState = DestroyDepthStencilState;
   pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = CalcPrivateRasterizerStateSize;
   pDeviceFuncs->pfnCreateRasterizerState = CreateRasterizerState;
   pDeviceFuncs->pfnDestroyRasterizerState = DestroyRasterizerState;
   pDeviceFuncs->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
   pDeviceFuncs->pfnCreateVertexShader = CreateVertexShader;
   pDeviceFuncs->pfnCreateGeometryShader = CreateGeometryShader;
   pDeviceFuncs->pfnCreatePixelShader = CreatePixelShader;
   pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = CalcPrivateGeometryShaderWithStreamOutput;
   pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = CreateGeometryShaderWithStreamOutput;
   pDeviceFuncs->pfnDestroyShader = DestroyShader;
   pDeviceFuncs->pfnCalcPrivateSamplerSize = CalcPrivateSamplerSize;
   pDeviceFuncs->pfnCreateSampler = CreateSampler;
   pDeviceFuncs->pfnDestroySampler = DestroySampler;
   pDeviceFuncs->pfnCalcPrivateQuerySize = CalcPrivateQuerySize;
   pDeviceFuncs->pfnCreateQuery = CreateQuery;
   pDeviceFuncs->pfnDestroyQuery = DestroyQuery;
   pDeviceFuncs->pfnCheckFormatSupport = CheckFormatSupport;
   pDeviceFuncs->pfnCheckMultisampleQualityLevels = CheckMultisampleQualityLevels;
   pDeviceFuncs->pfnCheckCounterInfo = CheckCounterInfo;
   pDeviceFuncs->pfnCheckCounter = CheckCounter;
   pDeviceFuncs->pfnDestroyDevice = DestroyDevice;
   pDeviceFuncs->pfnSetTextFilterSize = SetTextFilterSize;
   if (pCreateData->Interface == D3D10_1_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_x_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D10_1_7_DDI_INTERFACE_VERSION) {
      D3D10_1DDI_DEVICEFUNCS *p10_1DeviceFuncs = pCreateData->p10_1DeviceFuncs;
      p10_1DeviceFuncs->pfnRelocateDeviceFuncs = RelocateDeviceFuncs1;
      p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize1;
      p10_1DeviceFuncs->pfnCreateShaderResourceView = CreateShaderResourceView1;
      p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize1;
      p10_1DeviceFuncs->pfnCreateBlendState = CreateBlendState1;
      p10_1DeviceFuncs->pfnResourceConvert = ResourceCopy;
      p10_1DeviceFuncs->pfnResourceConvertRegion = ResourceCopyRegion;
   }

   /*
    * Fill in DXGI DDI functions
    */
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent =
      _Present;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps =
      _GetGammaCaps;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode =
      _SetDisplayMode;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority =
      _SetResourcePriority;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency =
      _QueryResourceResidency;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities =
      _RotateResourceIdentities;
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt =
      _Blt;


   /* Return S_OK so DXGI ENABLES the shared-resource presentation path to
    * communicate with DWM. Returning DXGI_STATUS_NO_REDIRECTION opts out of
    * that path - which is fatal for a render-only adapter, because with no
    * scanout the DWM shared-surface blt (BltDXGI -> _Blt) is the only way a
    * rendered frame reaches the screen. With this on, DXGI sets up the shared
    * destination surface and drives BltDXGI; _Blt copies the backbuffer into it
    * and DWM composites it. */
   DebugPrintf("[WinMaliUmdDiag] CreateDevice EXIT hr=0x%08lx\n", S_OK);
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyDevice --
 *
 *    The DestroyDevice function destroys a graphics context.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyDevice(D3D10DDI_HDEVICE hDevice)   // IN
{
   unsigned i;

   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   pipe->flush(pipe, NULL, 0);

   for (i = 0; i < PIPE_MAX_SO_BUFFERS; ++i) {
      pipe_so_target_reference(&pDevice->so_targets[i], NULL);
   }
   if (pDevice->draw_so_target) {
      pipe_so_target_reference(&pDevice->draw_so_target, NULL);
   }

   pipe->bind_fs_state(pipe, NULL);
   pipe->bind_vs_state(pipe, NULL);
   cso_destroy_context(pDevice->cso);

   DeleteEmptyShader(pDevice, MESA_SHADER_FRAGMENT, pDevice->empty_fs);
   DeleteEmptyShader(pDevice, MESA_SHADER_VERTEX, pDevice->empty_vs);

   util_unreference_framebuffer_state(&pDevice->fb);

   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      if (!pDevice->vertex_buffers[i].is_user_buffer) {
         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);
      }
   }

   pipe_resource_reference(&pDevice->index_buffer, NULL);

   static struct pipe_sampler_view * sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   memset(sampler_views, 0, sizeof sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_VERTEX, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);
   pipe->set_sampler_views(pipe, MESA_SHADER_GEOMETRY, 0,
                           PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, sampler_views);

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   struct pipe_screen *device_screen = pDevice->screen;
#endif

   pipe->destroy(pipe);
   pDevice->pipe = NULL;

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   /* Destroy the private screen while this Device and all of its callbacks,
    * contexts, and paging queue are still valid.  Screen teardown evicts the
    * remaining BO cache and therefore is the last point that may call alloc,
    * unmap, or free through the device-owned bridge. */
   if (device_screen) {
      DebugPrintf("WinMali: device bridge DRAIN device=%p screen=%p\n",
                  pDevice, device_screen);
      device_screen->destroy(device_screen);
      pDevice->screen = NULL;
   }

   if (pDevice->hContext) {
      D3DDDICB_DESTROYCONTEXT dtxCb;
      memset(&dtxCb, 0, sizeof dtxCb);
      dtxCb.hContext = pDevice->hContext;
      pDevice->KTCallbacks.pfnDestroyContextCb(pDevice->hDevice, &dtxCb);
      pDevice->hContext = NULL;
   }

   if (pDevice->hRenderCompletionFence) {
      D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT dso;
      memset(&dso, 0, sizeof dso);
      dso.hSyncObject = pDevice->hRenderCompletionFence;
      pDevice->KTCallbacks.pfnDestroySynchronizationObjectCb(pDevice->hDevice,
                                                              &dso);
      pDevice->hRenderCompletionFence = 0;
   }

   /* Phase 3 render context (lazy). */
   if (pDevice->hRenderContext) {
      D3DDDICB_DESTROYCONTEXT dtxCb;
      memset(&dtxCb, 0, sizeof dtxCb);
      dtxCb.hContext = pDevice->hRenderContext;
      pDevice->KTCallbacks.pfnDestroyContextCb(pDevice->hDevice, &dtxCb);
      pDevice->hRenderContext = NULL;
   }

   /* Phase 2 paging queue (lazy). */
   if (pDevice->hPagingQueue) {
      D3DDDI_DESTROYPAGINGQUEUE dpq;
      memset(&dpq, 0, sizeof dpq);
      dpq.hPagingQueue = pDevice->hPagingQueue;
      pDevice->KTCallbacks.pfnDestroyPagingQueueCb(pDevice->hDevice, &dpq);
      pDevice->hPagingQueue = 0;
   }
#endif
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs --
 *
 *    The RelocateDeviceFuncs function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs1 --
 *
 *    The RelocateDeviceFuncs1 function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * Flush --
 *
 *    The Flush function submits outstanding hardware commands that
 *    are in the hardware command buffer to the display miniport driver.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Flush(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);

   DebugPrintf("[WinMaliFlow] D3D Flush ENTER device=%p pipe=%p\n",
               hDevice.pDrvPrivate, pipe);
   pipe->flush(pipe, NULL, 0);
   DebugPrintf("[WinMaliFlow] D3D Flush EXIT device=%p\n",
               hDevice.pDrvPrivate);
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckFormatSupport --
 *
 *    The CheckFormatSupport function retrieves the capabilites that
 *    the device has with the specified format.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckFormatSupport(D3D10DDI_HDEVICE hDevice, // IN
                   DXGI_FORMAT Format,       // IN
                   __out UINT *pFormatCaps)  // OUT
{
   //LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   *pFormatCaps = 0;

   enum pipe_format format = FormatTranslate(Format, false);
   if (format == PIPE_FORMAT_NONE) {
      *pFormatCaps = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
      return;
   }

   if (Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
      /*
       * We only need to support creation.
       * http://msdn.microsoft.com/en-us/library/windows/hardware/ff552818.aspx
       */
      return;
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_RENDER_TARGET)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
      }
#endif
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_SAMPLER_VIEW)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;

#if SUPPORT_MSAA
      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_RENDER_TARGET)) {
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
      }
#endif
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckMultisampleQualityLevels --
 *
 *    The CheckMultisampleQualityLevels function retrieves the number
 *    of quality levels that the device supports for the specified
 *    number of samples.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,        // IN
                              DXGI_FORMAT Format,              // IN
                              UINT SampleCount,                // IN
                              __out UINT *pNumQualityLevels)   // OUT
{
   //LOG_ENTRYPOINT();

   /* XXX: Disable MSAA */
   *pNumQualityLevels = 0;
}


/*
 * ----------------------------------------------------------------------
 *
 * SetTextFilterSize --
 *
 *    The SetTextFilterSize function sets the width and height
 *    of the monochrome convolution filter.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetTextFilterSize(D3D10DDI_HDEVICE hDevice,  // IN
                  UINT Width,                // IN
                  UINT Height)               // IN
{
   LOG_ENTRYPOINT();

   LOG_UNSUPPORTED(Width != 1 || Height != 1);
}

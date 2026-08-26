/*
 * WinMaliDdi.c - "real" implementations for DDIs that dxgk exercises
 * during adapter init and process create. Everything in here replaces
 * a STATUS_NOT_SUPPORTED stub from WinMaliDxgkStubs.c (the generator's
 * EXISTING map routes the wire-up here).
 *
 * Scope for this iteration: get past adapter init + first CreateDevice
 * cleanly so the system stays up + SSH-able. Allocation/Render/Patch are
 * still NOT_IMPLEMENTED until we have the user-mode side ready.
 */

#include "WinMaliKmd.h"
#include "WinMaliBo.h"
#include "WinMaliSync.h"
#include "WinMaliVm.h"
#include "WinMaliGroup.h"

#define WINMALI_KMD_PROCESS_MAGIC   'PrcW'
#define WINMALI_KMD_DEVICE_MAGIC    'DvcW'
#define WINMALI_KMD_CONTEXT_MAGIC   'CtxW'

/* Exported by ntoskrnl since XP but not declared in wdm.h. Returns the
   15-char image file name of the process (no path). */
NTKERNELAPI PCHAR PsGetProcessImageFileName(_In_ PEPROCESS Process);

/* TRUE when the CURRENT thread (DxgkDdiCreateDevice runs in the context of
   the D3DKMTCreateDevice caller) belongs to dwm.exe. */
static BOOLEAN
WinMaliCallerIsDwm_(VOID)
{
    PCHAR  img = PsGetProcessImageFileName(PsGetCurrentProcess());
    STRING a, b;

    if (img == NULL) {
        return FALSE;
    }
    RtlInitString(&a, img);
    RtlInitString(&b, "dwm.exe");
    return RtlEqualString(&a, &b, TRUE /* case-insensitive */);
}

/* Per-process kernel state. Returned as hKmdProcess from CreateProcess and
   referenced from every Device/Context the process opens. */
typedef struct _WINMALI_KMD_PROCESS {
    ULONG               Magic;
    PWINMALI_ADAPTER    Adapter;
    HANDLE              hDxgkProcess;    /* dxgk-owned, opaque */
    UINT                Flags;           /* mirror of DXGK_CREATEPROCESSFLAGS */
    ULONG               Pasid;           /* primary process address-space id (if any) */
    LONG                DeviceCount;     /* outstanding Devices on this process */
} WINMALI_KMD_PROCESS, *PWINMALI_KMD_PROCESS;

/* Per-device state. Each D3D11 process opens one+ devices; each device
   can own many contexts. We chain back to the owning process via
   OwnerProcess so cleanup ordering errors are visible. */
typedef struct _WINMALI_KMD_DEVICE {
    ULONG                 Magic;
    PWINMALI_ADAPTER      Adapter;
    PWINMALI_KMD_PROCESS  OwnerProcess;  /* may be NULL on pre-WDDM2 path */
    HANDLE                hRtDevice;
    UINT                  Flags;
    ULONG                 Pasid;
    LONG                  ContextCount;
} WINMALI_KMD_DEVICE, *PWINMALI_KMD_DEVICE;

/* Per-context state. Each context binds (lazily, via SetRootPageTable)
   to a Mali AS slot. dxgk hands hContext back to us for every submit, so
   the KMD_CONTEXT address must stay valid until DestroyContext. */
typedef struct _WINMALI_KMD_CONTEXT {
    ULONG               Magic;
    PWINMALI_ADAPTER    Adapter;
    PWINMALI_KMD_DEVICE OwnerDevice;
    HANDLE              hRtContext;
    UINT                NodeOrdinal;
    UINT                EngineAffinity;
    UINT                Flags;
    UINT64              SubmittedFence;
    UINT64              CompletedFence;
} WINMALI_KMD_CONTEXT, *PWINMALI_KMD_CONTEXT;

/* Validate a dxgk-supplied device handle (e.g. DXGKARG_ESCAPE.hDevice)
   and return it as an opaque ownership token for the escape-created
   resource tables (VM/BO/SyncObj/Group). NULL when the handle is absent
   or isn't one of ours - callers then create the resource unowned. */
PVOID
WinMaliDeviceOwnerToken(_In_opt_ HANDLE hDevice)
{
    PWINMALI_KMD_DEVICE dev = (PWINMALI_KMD_DEVICE)hDevice;
    if (dev != NULL && dev->Magic == WINMALI_KMD_DEVICE_MAGIC) {
        return dev;
    }
    return NULL;
}

/* Resolve a dxgk device handle to its adapter (NULL if not one of ours).
   Used by DDIs that only receive hDevice (e.g. OpenAllocation) but need
   the adapter's DxgkInterface callbacks. */
PWINMALI_ADAPTER
WinMaliAdapterFromDeviceHandle(_In_opt_ HANDLE hDevice)
{
    PWINMALI_KMD_DEVICE dev = (PWINMALI_KMD_DEVICE)hDevice;
    if (dev != NULL && dev->Magic == WINMALI_KMD_DEVICE_MAGIC) {
        return dev->Adapter;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiGetNodeMetadata                                                   */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_GETNODEMETADATA)
NTSTATUS
APIENTRY
WinMaliKmdGetNodeMetadata(
    IN_CONST_HANDLE          hAdapter,
    UINT                     NodeOrdinalAndAdapterIndex,
    OUT_PDXGKARG_GETNODEMETADATA pGetNodeMetadata)
{
    UINT nodeOrdinal;
    UINT physicalAdapterIndex;

    UNREFERENCED_PARAMETER(hAdapter);

    if (pGetNodeMetadata == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    nodeOrdinal          = DXGKNODEMETADATA_GETNODEORDINAL(NodeOrdinalAndAdapterIndex);
    physicalAdapterIndex = DXGKNODEMETADATA_GETPHYSICALADAPTERINDEX(NodeOrdinalAndAdapterIndex);
#else
    nodeOrdinal          = NodeOrdinalAndAdapterIndex;
    physicalAdapterIndex = 0;
#endif

    /* Two nodes: 0 = 3D (real render engine), 1 = COPY (paging engine).
       dxgkrnl requires the paging node (PHYSICALADAPTERCAPS.PagingNodeIndex)
       to be a copy/DMA engine, NOT the 3D node - pointing paging at node 0
       makes it bail at StartDevice, and pointing it out-of-range materialises
       a PHANTOM paging node whose per-process scheduler state is never
       initialised (NULL D[1] deref in VidSchiProfilePerformanceTick -> 0xD1).
       So we declare a real copy node 1 for paging; its buffers are handled
       CPU-side (BuildPagingBuffer), the submit just completes a fence. */
    /*
     * ★ MT6897 INSTRUMENTATION. This function is SILENT in the upstream source, and it is the
     * only place in the adapter-start window that returns STATUS_INVALID_PARAMETER on real
     * input rather than on a NULL-pointer guard. Our failure is exactly
     * STATUS_INVALID_PARAMETER at AddAdapter with a trace whose last line is QAI[47] -- which
     * is not where dxgk gave up, only where the driver last logged. Log every call so the
     * blind window becomes visible.
     */
    DbgPrint("[WinMali] GetNodeMetadata raw=0x%08x node=%u padapter=%u\n",
             (unsigned)NodeOrdinalAndAdapterIndex,
             (unsigned)nodeOrdinal, (unsigned)physicalAdapterIndex);

    if (physicalAdapterIndex != 0 || nodeOrdinal > 1) {
        DbgPrint("[WinMali] GetNodeMetadata -> STATUS_INVALID_PARAMETER "
                 "(we declare 2 nodes; dxgk asked beyond them)\n");
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(pGetNodeMetadata, sizeof(*pGetNodeMetadata));
    if (nodeOrdinal == 0) {
        pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;
        (VOID)RtlStringCbCopyW(pGetNodeMetadata->FriendlyName,
                               sizeof(pGetNodeMetadata->FriendlyName),
                               L"WinMali 3D");
    } else {
        pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_COPY;
        (VOID)RtlStringCbCopyW(pGetNodeMetadata->FriendlyName,
                               sizeof(pGetNodeMetadata->FriendlyName),
                               L"WinMali Paging");
    }
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    /* Must match PHYSICALADAPTERCAPS.Flags.GpuMmuSupported and DRIVERCAPS. */
    pGetNodeMetadata->GpuMmuSupported = TRUE;
    pGetNodeMetadata->IoMmuSupported  = FALSE;
#endif
    WINMALI_TRACE("GetNodeMetadata: node%u %s GpuMmu=1", nodeOrdinal,
                  (nodeOrdinal == 0) ? "3D" : "COPY/paging");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiCreateProcess / DestroyProcess                                     */
/* ------------------------------------------------------------------------ */

/* CreateProcess is called once per user-mode process when it first opens
   our adapter (via D3DKMT_OPENADAPTER). We allocate a per-process state
   blob that subsequent CreateDevice calls will link to via
   pCreateDevice->hKmdProcess. */
_Function_class_(DXGKDDI_CREATEPROCESS)
NTSTATUS
APIENTRY
WinMaliKmdCreateProcess(
    IN_CONST_HANDLE              hAdapter,
    INOUT_PDXGKARG_CREATEPROCESS pCreateProcess)
{
    PWINMALI_ADAPTER     adapter;
    PWINMALI_KMD_PROCESS proc;

    if (pCreateProcess == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    proc = (PWINMALI_KMD_PROCESS)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(*proc), WINMALI_POOL_TAG);
    if (proc == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(proc, sizeof(*proc));
    proc->Magic         = WINMALI_KMD_PROCESS_MAGIC;
    proc->Adapter       = adapter;
    proc->hDxgkProcess  = pCreateProcess->hDxgkProcess;
    proc->Flags         = pCreateProcess->Flags.Value;
    if (pCreateProcess->NumPasid > 0 && pCreateProcess->pPasid != NULL) {
        proc->Pasid = pCreateProcess->pPasid[0];
    }

    pCreateProcess->hKmdProcess = (HANDLE)proc;
    WINMALI_TRACE("CreateProcess: proc=%p hDxgk=%p flags=0x%x pasid=%u",
                  proc, proc->hDxgkProcess, proc->Flags, proc->Pasid);
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_DESTROYPROCESS)
NTSTATUS
APIENTRY
WinMaliKmdDestroyProcess(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_HANDLE hKmdProcess)
{
    PWINMALI_KMD_PROCESS proc = (PWINMALI_KMD_PROCESS)hKmdProcess;
    UNREFERENCED_PARAMETER(hAdapter);

    if (proc == NULL || proc->Magic != WINMALI_KMD_PROCESS_MAGIC) {
        return STATUS_INVALID_HANDLE;
    }
    if (proc->DeviceCount != 0) {
        WINMALI_WARN("DestroyProcess: proc=%p leaks %ld devices",
                     proc, proc->DeviceCount);
    }
    WINMALI_TRACE("DestroyProcess: proc=%p", proc);
    proc->Magic = 0;
    ExFreePoolWithTag(proc, WINMALI_POOL_TAG);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiCreateDevice / DestroyDevice                                       */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_CREATEDEVICE)
NTSTATUS
APIENTRY
WinMaliKmdCreateDevice(
    IN_CONST_HANDLE             hAdapter,
    INOUT_PDXGKARG_CREATEDEVICE pCreateDevice)
{
    PWINMALI_ADAPTER     adapter;
    PWINMALI_KMD_DEVICE  dev;
    PWINMALI_KMD_PROCESS owner = NULL;

    if (pCreateDevice == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    /* Bring-up gate: DWM adopting the render-only adapter is pure churn
       today (device/VM/BO create-teardown loops per composition probe).
       Reject its device up front so it stays on the Basic Display adapter.
       HISTORY: this gate was first added in v1.0.0.28 and reverted in v29
       because the box bugchecked with it in - but that crash was the
       DriverUnload-clobber 0x139 (dxgkrnl!DpiInitializeEx list corruption
       on same-boot reinstall), fixed in v1.0.0.32; the gate itself was
       innocent. Re-enabled in v1.0.0.35. Registry escape hatch (no
       rebuild): Services\WinMaliKmd\Parameters\AllowDwm = 1 + reboot. */
    if (!g_WinMaliAllowDwm && WinMaliCallerIsDwm_()) {
        static LONG blocked = 0;
        if ((InterlockedIncrement(&blocked) & 0x3F) == 1) {
            WINMALI_WARN("CreateDevice: dwm.exe blocked (AllowDwm=0, total=%ld)",
                         blocked);
        }
        return STATUS_ACCESS_DENIED;
    }

    /* WDDM 2.0+: dxgk hands us the per-process state we returned from
       CreateProcess via pCreateDevice->hKmdProcess. Validate it. */
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    if (pCreateDevice->hKmdProcess != NULL) {
        owner = (PWINMALI_KMD_PROCESS)pCreateDevice->hKmdProcess;
        if (owner->Magic != WINMALI_KMD_PROCESS_MAGIC) {
            WINMALI_WARN("CreateDevice: hKmdProcess=%p bad magic", owner);
            owner = NULL;
        }
    }
#endif

    dev = (PWINMALI_KMD_DEVICE)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(*dev), WINMALI_POOL_TAG);
    if (dev == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(dev, sizeof(*dev));
    dev->Magic        = WINMALI_KMD_DEVICE_MAGIC;
    /* ★ PAIR THESE TWO COUNTERS. VmCreates=8 / VmDestroys=2 says six AS slots
       leaked, but NOT why. If DevDestroys tracks DevCreates then dxgk IS tearing
       devices down and the fault is VM OWNERSHIP (a VM tagged to a device that
       never owned it, e.g. an adapter-level escape whose hDevice is NULL - the
       rundown bails on NULL). If DevDestroys lags, dxgk is not calling us at all
       and the leak is a lifetime/handle problem instead. Two very different
       fixes; one boot separates them. */
    (VOID)InterlockedIncrement(&g_WinMaliStatDevCreates);
    dev->Adapter      = adapter;
    dev->OwnerProcess = owner;
    dev->hRtDevice    = pCreateDevice->hDevice;
    dev->Flags        = pCreateDevice->Flags.Value;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    dev->Pasid        = pCreateDevice->Pasid;
#endif

    if (owner != NULL) {
        InterlockedIncrement(&owner->DeviceCount);
    }

    pCreateDevice->hDevice = (HANDLE)dev;
    WINMALI_TRACE("CreateDevice: dev=%p proc=%p rtDevice=%p flags=0x%x pasid=%u",
                  dev, owner, dev->hRtDevice, dev->Flags, dev->Pasid);
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_DESTROYDEVICE)
NTSTATUS
APIENTRY
WinMaliKmdDestroyDevice(
    IN_CONST_HANDLE hDevice)
{
    PWINMALI_KMD_DEVICE dev = (PWINMALI_KMD_DEVICE)hDevice;

    (VOID)InterlockedIncrement(&g_WinMaliStatDevDestroys);
    if (dev == NULL || dev->Magic != WINMALI_KMD_DEVICE_MAGIC) {
        (VOID)InterlockedIncrement(&g_WinMaliStatDevDestroyBadHandle);
        return STATUS_INVALID_HANDLE;
    }
    if (dev->ContextCount != 0) {
        WINMALI_WARN("DestroyDevice: dev=%p leaks %ld contexts",
                     dev, dev->ContextCount);
    }
    if (dev->OwnerProcess != NULL) {
        InterlockedDecrement(&dev->OwnerProcess->DeviceCount);
    }

    /* Run down every escape-created resource this device still owns. A UMD
       that dies (or just never sends the destroy ops) must not leak: each
       leaked VM pins a Mali AS slot and only 8 exist - six leaks and every
       VmCreate adapter-wide fails -ENOMEM until reboot (seen live
       2026-07-12: dwm/shell probes exhausted AS2..AS7, gl-smoke got 0
       pixel formats). Order: groups reference VMs, BOs may be VM-bound, so
       groups -> BOs -> syncobjs -> VMs. */
    {
        PWINMALI_ADAPTER adapter = dev->Adapter;
        if (adapter != NULL) {
            ULONG groups = WinMaliGroupRundownOwner(adapter, dev);
            ULONG bos    = WinMaliBoRundownOwner(adapter, dev);
            ULONG syncs  = WinMaliSyncObjRundownOwner(adapter, dev);
            ULONG vms    = WinMaliVmRundownOwner(adapter, dev);
            if ((groups | bos | syncs | vms) != 0) {
                WINMALI_TRACE("DestroyDevice rundown: dev=%p groups=%u bos=%u "
                              "syncs=%u vms=%u", dev, groups, bos, syncs, vms);
            }
        }
    }

    WINMALI_TRACE("DestroyDevice: dev=%p", dev);
    dev->Magic = 0;
    ExFreePoolWithTag(dev, WINMALI_POOL_TAG);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiCreateContext / DestroyContext                                     */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_CREATECONTEXT)
NTSTATUS
APIENTRY
WinMaliKmdCreateContext(
    IN_CONST_HANDLE              hDevice,
    INOUT_PDXGKARG_CREATECONTEXT pCreateContext)
{
    PWINMALI_KMD_DEVICE  dev;
    PWINMALI_KMD_CONTEXT ctx;

    if (pCreateContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    dev = (PWINMALI_KMD_DEVICE)hDevice;
    if (dev == NULL || dev->Magic != WINMALI_KMD_DEVICE_MAGIC) {
        return STATUS_INVALID_HANDLE;
    }

    /* ★ PROOF-OF-LIFE: did OUR UMD's _Present actually run?
     *
     * We have been counting presents in the KMD and assuming they come from our
     * UMD, but the present private data arrives with size 0, so that was never
     * verified. _Present is the ONLY place the UMD calls pfnCreateContextCb, and
     * it stamps WINMALI_PRESENT_PRIV_MAGIC here. A non-zero count proves
     * _Present ran; zero proves the presents we count are NOT ours.
     *
     * It also tells us whether ANY UMD->KMD private-data channel survives: if
     * this one arrives while the present one is 0 bytes, the problem is specific
     * to the present path, not to private data in general. */
    (VOID)InterlockedExchange(&g_WinMaliStatCtxPrivSize,
                              (LONG)pCreateContext->PrivateDriverDataSize);
    if (pCreateContext->pPrivateDriverData != NULL &&
        pCreateContext->PrivateDriverDataSize >= sizeof(WINMALI_PRESENT_PRIV)) {
        const WINMALI_PRESENT_PRIV* cm =
            (const WINMALI_PRESENT_PRIV*)pCreateContext->pPrivateDriverData;
        if (cm->Magic == WINMALI_PRESENT_PRIV_MAGIC) {
            (VOID)InterlockedIncrement(&g_WinMaliStatUmdPresentCtx);
        }
    }

    /* dxgk creates one "system" context per device for its own paging-buffer
       submits. It carries Flags.SystemContext=1 and uses NodeOrdinal=0x7FFF as
       a sentinel meaning "no real engine". Accept it without binding to a Mali
       AS slot (paging BPB submits are no-ops for us under CPU_VIRTUAL).
       For a non-system context we still only own node 0. */
    if (pCreateContext->Flags.SystemContext == 0 && pCreateContext->NodeOrdinal > 1) {
        WINMALI_WARN("CreateContext: rejecting NodeOrdinal=%u flags=0x%x (we own nodes 0=3D,1=paging)",
                     pCreateContext->NodeOrdinal, pCreateContext->Flags.Value);
        return STATUS_INVALID_PARAMETER;
    }

    ctx = (PWINMALI_KMD_CONTEXT)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(*ctx), WINMALI_POOL_TAG);
    if (ctx == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Magic          = WINMALI_KMD_CONTEXT_MAGIC;
    ctx->Adapter        = dev->Adapter;
    ctx->OwnerDevice    = dev;
    ctx->hRtContext     = pCreateContext->hContext;
    ctx->NodeOrdinal    = pCreateContext->NodeOrdinal;
    ctx->EngineAffinity = pCreateContext->EngineAffinity;
    ctx->Flags          = pCreateContext->Flags.Value;

    InterlockedIncrement(&dev->ContextCount);
    pCreateContext->hContext                            = (HANDLE)ctx;
    pCreateContext->ContextInfo.DmaBufferSize           = PAGE_SIZE;
    pCreateContext->ContextInfo.DmaBufferSegmentSet     = (1u << (WINMALI_SEGMENT_ID_SYSMEM - 1));
    /* Non-system contexts get DMA private data big enough for the present
       Blt descriptor (DxgkDdiPresent stashes a WINMALI_BLT_DESC there and
       SubmitCommand executes it). With 0 here, Present's size check always
       failed and presents silently became no-ops. System/paging contexts
       keep 0 so the paging path can never look like a blt. */
    /* ★ ALL CONTEXTS GET THE PRIVATE DATA, INCLUDING SYSTEM ONES.
     *
     * MEASURED 2026-08-15: every present DWM makes is rejected -
     *     presents=23  notblt=23  BLTS=0  nullcookie=0
     *     Flags.Value=0x00000001 (Blt IS bit 0 - confirmed against
     *     DXGK_PRESENTFLAGS in d3dkmddi.h, not assumed)
     *     allocList=1  dmaPriv=1  NumSrc=0  NumDst=0
     * With Blt set and both pointers valid, the ONLY remaining reject in
     * DxgkDdiPresent is `DmaBufferPrivateDataSize < sizeof(WINMALI_BLT_DESC)`.
     * Handing 0 to system contexts makes that check 0 < 40 => reject, and it
     * returns STATUS_SUCCESS, so the frame vanishes silently and the panel keeps
     * the mode-set image. DWM's presents arrive on such a context.
     *
     * The old reason for zeroing it - "so the paging path can never look like a
     * blt" - is already covered by WINMALI_BLT_DESC_MAGIC, which SubmitCommand
     * checks before executing anything (WinMaliDdi.c ~920/995). Size is the
     * wrong discriminator; the magic is the right one. */
    pCreateContext->ContextInfo.DmaBufferPrivateDataSize =
        (UINT)sizeof(WINMALI_BLT_DESC);
    /* System / paging contexts use static GPU VAs - no patching needed.
       Render contexts on a GpuMmu adapter also don't need patching since
       the UMD writes final GPU VAs directly into the DMA buffer. So set
       NoPatchingRequired unconditionally. */
    pCreateContext->ContextInfo.Caps.NoPatchingRequired = TRUE;
    if (pCreateContext->Flags.SystemContext) {
        /* System / kernel contexts: dxgk allocates the DMA buffer itself
           and provides no patch/alloc lists. */
        pCreateContext->ContextInfo.AllocationListSize    = 0;
        pCreateContext->ContextInfo.PatchLocationListSize = 0;
    } else {
        pCreateContext->ContextInfo.AllocationListSize    = 64;
        pCreateContext->ContextInfo.PatchLocationListSize = 128;
    }

    WINMALI_TRACE("CreateContext: ctx=%p dev=%p node=%u flags=0x%x%s",
                  ctx, dev, ctx->NodeOrdinal, ctx->Flags,
                  (pCreateContext->Flags.SystemContext) ? " [SYSTEM]" : "");
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_DESTROYCONTEXT)
NTSTATUS
APIENTRY
WinMaliKmdDestroyContext(
    IN_CONST_HANDLE hContext)
{
    PWINMALI_KMD_CONTEXT ctx = (PWINMALI_KMD_CONTEXT)hContext;

    if (ctx == NULL || ctx->Magic != WINMALI_KMD_CONTEXT_MAGIC) {
        return STATUS_INVALID_HANDLE;
    }
    /* Release the Mali AS slot bound to this context (if SetRootPageTable
       ever ran). MMU unbind is idempotent / a no-op when nothing was bound. */
    if (ctx->Adapter != NULL) {
        (VOID)WinMaliMmuUnbindContext(ctx->Adapter, ctx->hRtContext);
    }
    if (ctx->OwnerDevice != NULL) {
        InterlockedDecrement(&ctx->OwnerDevice->ContextCount);
    }
    WINMALI_TRACE("DestroyContext: ctx=%p", ctx);
    ctx->Magic = 0;
    ExFreePoolWithTag(ctx, WINMALI_POOL_TAG);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiSetRootPageTable / GetRootPageTableSize                            */
/* ------------------------------------------------------------------------ */

/* DXGKDDI_SETROOTPAGETABLE returns VOID and takes a SINGLE context per
   call. Address is {SegmentId, SegmentOffset} - we translate to a host
   physical address using the DmaSegment base, then bind to a Mali AS slot. */
_Function_class_(DXGKDDI_SETROOTPAGETABLE)
VOID
APIENTRY
WinMaliKmdSetRootPageTable(
    IN_CONST_HANDLE                       hAdapter,
    IN_CONST_PDXGKARG_SETROOTPAGETABLE    pArgs)
{
    PWINMALI_ADAPTER adapter;
    ULONG            asSlot = 0;
    NTSTATUS         bindSt;
    UINT64           rootPtPa = 0;

    if (pArgs == NULL) {
        return;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return;
    }

    if (pArgs->Address.SegmentId == WINMALI_SEGMENT_ID_SYSMEM &&
        adapter->DmaSegmentVa != NULL) {
        rootPtPa = (UINT64)adapter->DmaSegmentPhys.QuadPart
                 + (UINT64)pArgs->Address.SegmentOffset;
    } else {
        /* Unbind request (SegmentId=0) or unknown segment - pass 0 to
           WinMaliMmuBindContextRootPt which treats 0 as unbind. */
        rootPtPa = 0;
    }

    bindSt = WinMaliMmuBindContextRootPt(adapter,
                                         pArgs->hContext,
                                         rootPtPa,
                                         &asSlot);
    WINMALI_TRACE("SetRootPageTable: ctx=%p seg=%u off=0x%llx pa=0x%llx entries=%u -> AS%u (0x%08x)",
                  pArgs->hContext,
                  pArgs->Address.SegmentId,
                  (ULONGLONG)pArgs->Address.SegmentOffset,
                  (ULONGLONG)rootPtPa,
                  pArgs->NumEntries,
                  asSlot, bindSt);
}

/* Returns the root PT size in bytes and fills NumberOfPte with the root
   level's PTE count. Mali LPAE: 9 bits per level * 8 bytes per PTE =
   512 entries = 4 KiB = PAGE_SIZE. */
_Function_class_(DXGKDDI_GETROOTPAGETABLESIZE)
SIZE_T
APIENTRY
WinMaliKmdGetRootPageTableSize(
    IN_CONST_HANDLE                       hAdapter,
    INOUT_PDXGKARG_GETROOTPAGETABLESIZE   pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs != NULL) {
        pArgs->NumberOfPte = 512;   /* 9-bit top-level index */
    }
    return PAGE_SIZE;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiQueryCurrentFence                                                  */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_QUERYCURRENTFENCE)
NTSTATUS
APIENTRY
WinMaliKmdQueryCurrentFence(
    IN_CONST_HANDLE               hAdapter,
    INOUT_PDXGKARG_QUERYCURRENTFENCE pArgs)
{
    PWINMALI_ADAPTER adapter;
    if (pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    pArgs->CurrentFence = (UINT)adapter->GpuCompletedFence;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiSubmitCommand                                                     */
/* ------------------------------------------------------------------------ */

/* Present phase-2: execute a WINMALI_BLT_DESC recorded by DxgkDdiPresent.
   CPU copy from the rendered backbuffer allocation into the present target
   (DWM redirection / cross-adapter surface), row by row, honoring both
   pitches and the dst rect. Runs at PASSIVE_LEVEL (SubmitCommand comes from
   a dxgk worker); both allocations are resident here because VidMm makes
   present's allocation list resident before submitting the DMA buffer.
   Any inconsistency drops the frame with a warning - never the fence. */
static VOID
WinMaliPresentExecuteBlt_(_In_ const WINMALI_BLT_DESC* d)
{
    PWINMALI_KMD_ALLOCATION src = d->Src;
    PWINMALI_KMD_ALLOCATION dst = d->Dst;
    PUCHAR srcBase, dstBase;
    SIZE_T srcBytes, dstBytes;
    ULONG  srcPitch, dstPitch;
    LONG   width, height, r;
    const ULONG bpp = 4;   /* swapchain / GDI surfaces are 32bpp linear */

    if (src == NULL || dst == NULL ||
        src->Magic != WINMALI_KMD_ALLOC_MAGIC ||
        dst->Magic != WINMALI_KMD_ALLOC_MAGIC) {
        WINMALI_WARN("PresentBlt: bad allocation descriptors (src=%p dst=%p)", src, dst);
        return;
    }
    if (src->ApertureMdl == NULL || dst->ApertureMdl == NULL) {
        WINMALI_WARN("PresentBlt: not resident (srcMdl=%p dstMdl=%p) - frame dropped",
                     src->ApertureMdl, dst->ApertureMdl);
        return;
    }

    srcPitch = src->Pitch;
    dstPitch = dst->Pitch;
    if (srcPitch == 0 || dstPitch == 0) {
        WINMALI_WARN("PresentBlt: zero pitch (src=%u dst=%u) - frame dropped",
                     srcPitch, dstPitch);
        return;
    }

    srcBase = (PUCHAR)MmGetSystemAddressForMdlSafe(src->ApertureMdl,
                                                   NormalPagePriority | MdlMappingNoExecute);
    dstBase = (PUCHAR)MmGetSystemAddressForMdlSafe(dst->ApertureMdl,
                                                   NormalPagePriority | MdlMappingNoExecute);
    if (srcBase == NULL || dstBase == NULL) {
        WINMALI_WARN("PresentBlt: MDL map failed (src=%p dst=%p) - frame dropped",
                     srcBase, dstBase);
        return;
    }
    srcBase += (SIZE_T)src->ApertureMdlOffset << PAGE_SHIFT;
    dstBase += (SIZE_T)dst->ApertureMdlOffset << PAGE_SHIFT;
    srcBytes = (SIZE_T)src->AperturePageCount << PAGE_SHIFT;
    dstBytes = (SIZE_T)dst->AperturePageCount << PAGE_SHIFT;

    width  = d->DstRight - d->DstLeft;
    height = d->DstBottom - d->DstTop;
    if (width <= 0 || height <= 0 || d->SrcLeft < 0 || d->SrcTop < 0 ||
        d->DstLeft < 0 || d->DstTop < 0) {
        return;
    }

    /* This is a 1:1 blt (no stretch): clamp the copy extent to what the SOURCE
       actually holds so a dst rect larger than the rendered backbuffer can't
       walk us past the src rows/columns (the per-row bounds check below is the
       hard backstop; this keeps a mismatched present from tearing). */
    {
        LONG srcRows = (LONG)(srcBytes / srcPitch);
        LONG srcCols = (LONG)(srcPitch / bpp);
        if (d->SrcTop + height > srcRows) height = srcRows - d->SrcTop;
        if (d->SrcLeft + width  > srcCols) width  = srcCols - d->SrcLeft;
        if (width <= 0 || height <= 0) {
            WINMALI_WARN("PresentBlt: src too small (rows=%ld cols=%ld srcTop=%d srcLeft=%d) - dropped",
                         srcRows, srcCols, d->SrcTop, d->SrcLeft);
            return;
        }
    }

    for (r = 0; r < height; ++r) {
        SIZE_T srcOff = (SIZE_T)(d->SrcTop + r) * srcPitch +
                        (SIZE_T)d->SrcLeft * bpp;
        SIZE_T dstOff = (SIZE_T)(d->DstTop + r) * dstPitch +
                        (SIZE_T)d->DstLeft * bpp;
        SIZE_T rowBytes = (SIZE_T)width * bpp;

        if (srcOff + rowBytes > srcBytes || dstOff + rowBytes > dstBytes) {
            WINMALI_WARN("PresentBlt: clipped at row %ld/%ld (src %Iu+%Iu/%Iu dst %Iu+%Iu/%Iu)",
                         r, height, srcOff, rowBytes, srcBytes, dstOff, rowBytes, dstBytes);
            break;
        }
        RtlCopyMemory(dstBase + dstOff, srcBase + srcOff, rowBytes);
    }

    /* Confirm the copy actually ran (vs the silent drops above) so on-screen
       bring-up can be verified from kd without guessing. One line per present. */
    WINMALI_TRACE("PresentBlt: copied %ldx%ld px  src(pitch=%u bytes=%Iu)->dst(pitch=%u bytes=%Iu) "
                  "src[%d,%d] dst[%d,%d]",
                  width, height, srcPitch, srcBytes, dstPitch, dstBytes,
                  d->SrcLeft, d->SrcTop, d->DstLeft, d->DstTop);
}

/* ------------------------------------------------------------------------ */
/* Async submit scheduler                                                   */
/*                                                                          */
/* DxgkDdiSubmitCommand must NOT block the dxgk scheduler worker thread on   */
/* the CSF round-trip, and WDDM 3.2/GpuMmu REQUIRES real scheduling caps     */
/* (PreemptionAware/CancelCommandAware - dxgk won't even start the adapter   */
/* without them, and stubbing the DDIs while advertising them bugchecks the  */
/* scheduler). So: SubmitCommand enqueues here and returns immediately; a    */
/* dedicated KMD worker thread runs the (proven, synchronous) CSF NOP off    */
/* the scheduler thread and completes the fence via the DPC. PreemptCommand  */
/* enqueues an in-order marker so preemption reports the exact completed-    */
/* fence boundary (DMA-buffer-boundary granularity) with no race.           */
/* ------------------------------------------------------------------------ */

/* Push a completed/preempted fence onto the DPC-drained notify queue. */
static VOID
WinMaliSchedPushFence_(_Inout_ PWINMALI_ADAPTER a, _In_ UINT fence,
                       _In_ UINT node, _In_ UINT engine,
                       _In_ BOOLEAN isPreempt, _In_ UINT lastCompleted)
{
    KIRQL irql;
    ULONG next;
    KeAcquireSpinLock(&a->FenceQueueLock, &irql);
    next = (a->FenceQueueTail + 1) % WINMALI_FENCE_QUEUE_DEPTH;
    {
        ULONG used = (a->FenceQueueTail + WINMALI_FENCE_QUEUE_DEPTH - a->FenceQueueHead)
                     % WINMALI_FENCE_QUEUE_DEPTH;
        if (used > a->FenceQueuePeak) {
            a->FenceQueuePeak = used;
        }
    }
    if (next == a->FenceQueueHead) {
        /* Last resort only. Dropping the oldest entry loses a DMA_COMPLETED
           that dxgk is waiting for in sequence => 0x119. It must never happen;
           if the counter is ever nonzero the producer side is the bug, not the
           depth. Count it and SAY SO - the previous code did this silently. */
        a->FenceQueueHead = (a->FenceQueueHead + 1) % WINMALI_FENCE_QUEUE_DEPTH;
        a->FenceQueueDropped++;
        if ((a->FenceQueueDropped & 0x3F) == 1) {
            WINMALI_WARN("Sched: FENCE QUEUE OVERFLOW - dropped a DMA_COMPLETED "
                         "(total=%lu, depth=%u). This WILL bugcheck 0x119.",
                         a->FenceQueueDropped, WINMALI_FENCE_QUEUE_DEPTH);
        }
    }
    a->FenceQueue[a->FenceQueueTail].SubmissionFenceId    = fence;
    a->FenceQueue[a->FenceQueueTail].NodeOrdinal          = node;
    a->FenceQueue[a->FenceQueueTail].EngineOrdinal        = engine;
    a->FenceQueue[a->FenceQueueTail].IsPreempt            = isPreempt;
    a->FenceQueue[a->FenceQueueTail].LastCompletedFenceId = lastCompleted;
    a->FenceQueueTail = next;
    KeReleaseSpinLock(&a->FenceQueueLock, irql);
}

/* Enqueue a submit/preempt request for the worker. Returns FALSE if the ring
   is momentarily full (caller then completes inline so no fence is dropped). */
static BOOLEAN
WinMaliSchedEnqueue_(_Inout_ PWINMALI_ADAPTER a, _In_ UINT fence,
                     _In_ UINT node, _In_ UINT engine, _In_ BOOLEAN isPreempt)
{
    KIRQL   irql;
    ULONG   next;
    BOOLEAN queued = FALSE;

    /* No worker (SchedInit failed, or we're tearing down) => tell the caller
       to complete inline. Never enqueue into a queue nothing will drain - that
       would leave the fence forever incomplete and TDR the adapter. */
    if (a->SubmitWorkerThread == NULL ||
        InterlockedCompareExchange(&a->SubmitWorkerStop, 0, 0) != 0) {
        return FALSE;
    }

    KeAcquireSpinLock(&a->SubmitQueueLock, &irql);
    next = (a->SubmitQueueTail + 1) % WINMALI_SUBMIT_QUEUE_DEPTH;
    if (next != a->SubmitQueueHead) {
        a->SubmitQueue[a->SubmitQueueTail].FenceId       = fence;
        a->SubmitQueue[a->SubmitQueueTail].NodeOrdinal   = node;
        a->SubmitQueue[a->SubmitQueueTail].EngineOrdinal = engine;
        a->SubmitQueue[a->SubmitQueueTail].IsPreempt     = isPreempt;
        a->SubmitQueueTail = next;
        queued = TRUE;
    }
    KeReleaseSpinLock(&a->SubmitQueueLock, irql);
    if (queued) {
        KeSetEvent(&a->SubmitWorkerWake, IO_NO_INCREMENT, FALSE);
    }
    return queued;
}

/* Dedicated worker: drains the submit queue in order, runs the CSF NOP for
   real submits and completes their fences, and turns preempt markers into a
   DMA_PREEMPTED at the current completed-fence boundary. */
static VOID
WinMaliSchedWorker_(_In_ PVOID Context)
{
    PWINMALI_ADAPTER a = (PWINMALI_ADAPTER)Context;

    for (;;) {
        KeWaitForSingleObject(&a->SubmitWorkerWake, Executive, KernelMode, FALSE, NULL);
        if (InterlockedCompareExchange(&a->SubmitWorkerStop, 0, 0) != 0) {
            break;
        }
        for (;;) {
            KIRQL   irql;
            BOOLEAN have = FALSE;
            UINT    fence = 0, node = 0, engine = 0;
            BOOLEAN isPreempt = FALSE;

            KeAcquireSpinLock(&a->SubmitQueueLock, &irql);
            if (a->SubmitQueueHead != a->SubmitQueueTail) {
                fence     = a->SubmitQueue[a->SubmitQueueHead].FenceId;
                node      = a->SubmitQueue[a->SubmitQueueHead].NodeOrdinal;
                engine    = a->SubmitQueue[a->SubmitQueueHead].EngineOrdinal;
                isPreempt = a->SubmitQueue[a->SubmitQueueHead].IsPreempt;
                a->SubmitQueueHead = (a->SubmitQueueHead + 1) % WINMALI_SUBMIT_QUEUE_DEPTH;
                have = TRUE;
            }
            KeReleaseSpinLock(&a->SubmitQueueLock, irql);
            if (!have) {
                break;
            }

            if (isPreempt) {
                /* All prior real submits drained above (in order) and their
                   fences completed, so the preemption boundary is simply the
                   last completed fence. */
                UINT last = (UINT)InterlockedCompareExchange64(&a->SchedLastCompletedFence, 0, 0);
                WinMaliSchedPushFence_(a, fence, node, engine, TRUE, last);
            } else {
                /* ⛔ NO CSF NOP HERE ANY MORE.
                 *
                 * It never executed the DMA buffer - by the time we get here the
                 * work is already done (a present blt is a CPU RtlCopyMemory run
                 * in SubmitCommand; paging/copy-node buffers we don't run on the
                 * GPU at all). The NOP only proved the MCU was alive, and it did
                 * that while holding SubmitLock, i.e. serialized against the UMD's
                 * escape GroupSubmits on the single CSG slot.
                 *
                 * Once the UMD's real render work moved onto that path, each NOP
                 * waited up to 500 ms for the lock => this worker drained ~2
                 * fences/sec against a 60 Hz compositor => the fence queue
                 * overflowed => dropped completions => 0x119. CSF liveness is
                 * already proven at bring-up (WinMaliFw.c) and continuously by
                 * the escape path itself. Complete immediately. */
                InterlockedExchange64(&a->SchedLastCompletedFence, (LONG64)fence);
                WinMaliSchedPushFence_(a, fence, node, engine, FALSE, 0);
            }

            if (a->DxgkInterface.DxgkCbQueueDpc != NULL && a->DxgkHandle != NULL) {
                a->DxgkInterface.DxgkCbQueueDpc(a->DxgkHandle);
            }
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
WinMaliSchedInit(_Inout_ PWINMALI_ADAPTER Adapter)
{
    HANDLE            hThread = NULL;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS          st;

    if (Adapter->SubmitWorkerThread != NULL) {
        return STATUS_SUCCESS;   /* already up */
    }
    KeInitializeSpinLock(&Adapter->SubmitQueueLock);
    Adapter->SubmitQueueHead = 0;
    Adapter->SubmitQueueTail = 0;
    Adapter->SubmitWorkerStop = 0;
    Adapter->SchedLastCompletedFence = 0;
    KeInitializeEvent(&Adapter->SubmitWorkerWake, SynchronizationEvent, FALSE);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                              WinMaliSchedWorker_, Adapter);
    if (!NT_SUCCESS(st)) {
        WINMALI_WARN("SchedInit: PsCreateSystemThread 0x%08x", st);
        return st;
    }
    st = ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, *PsThreadType,
                                   KernelMode, &Adapter->SubmitWorkerThread, NULL);
    ZwClose(hThread);
    if (!NT_SUCCESS(st)) {
        /* Thread is running but we couldn't ref it: signal stop so it exits. */
        Adapter->SubmitWorkerThread = NULL;
        InterlockedExchange(&Adapter->SubmitWorkerStop, 1);
        KeSetEvent(&Adapter->SubmitWorkerWake, IO_NO_INCREMENT, FALSE);
        WINMALI_WARN("SchedInit: ObReferenceObjectByHandle 0x%08x", st);
        return st;
    }
    WINMALI_TRACE("SchedInit: async submit worker up");
    return STATUS_SUCCESS;
}

VOID
WinMaliSchedTeardown(_Inout_ PWINMALI_ADAPTER Adapter)
{
    PVOID th = Adapter->SubmitWorkerThread;
    if (th == NULL) {
        return;
    }
    InterlockedExchange(&Adapter->SubmitWorkerStop, 1);
    KeSetEvent(&Adapter->SubmitWorkerWake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(th, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(th);
    Adapter->SubmitWorkerThread = NULL;
    WINMALI_TRACE("SchedTeardown: async submit worker stopped");
}

/* DxgkDdiSubmitCommand - enqueue to the async worker (see above) and return.

   For a Blt present the pixel copy (WinMaliPresentExecuteBlt_) runs here
   first (fast CPU memcpy of an already-rendered backbuffer); only the CSF
   fence is deferred to the worker. Paging submits carry no private data and
   just get a fence. */
_Function_class_(DXGKDDI_SUBMITCOMMAND)
NTSTATUS
APIENTRY
WinMaliKmdSubmitCommand(
    IN_CONST_HANDLE                 hAdapter,
    IN_CONST_PDXGKARG_SUBMITCOMMAND pSubmit)
{
    PWINMALI_ADAPTER adapter;

    if (pSubmit == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    WINMALI_TRACE("SubmitCommand: fenceId=%u node=%u engine=%u dmaSize=%u start=0x%x end=0x%x",
                  pSubmit->SubmissionFenceId,
                  pSubmit->NodeOrdinal,
                  pSubmit->EngineOrdinal,
                  pSubmit->DmaBufferSize,
                  pSubmit->DmaBufferSubmissionStartOffset,
                  pSubmit->DmaBufferSubmissionEndOffset);

    /* Present phase-2: a magic'd blt descriptor in the DMA private data
       means this submit is a DxgkDdiPresent Blt - execute the pixel copy
       before fencing. Paging submits ride the system context whose
       DmaBufferPrivateDataSize is 0, so they can never enter here. */
    if (pSubmit->pDmaBufferPrivateData != NULL &&
        pSubmit->DmaBufferPrivateDataSize >= sizeof(WINMALI_BLT_DESC)) {
        PWINMALI_BLT_DESC blt = (PWINMALI_BLT_DESC)pSubmit->pDmaBufferPrivateData;
        if (blt->Magic == WINMALI_BLT_DESC_MAGIC) {
            WinMaliPresentExecuteBlt_(blt);
            blt->Magic = 0;   /* consume: a TDR resubmit must not replay stale pointers */
        }
    }

    /* Hand off to the async worker and RETURN IMMEDIATELY - never block the
       dxgk scheduler thread on the CSF round-trip. The worker runs the CSF
       NOP fence off-thread and completes it via the DPC (DMA_COMPLETED). The
       worker's SubmitLock-serialized CSF path also serializes with escape
       GroupSubmits on the single CSG slot.

       If the worker's queue is momentarily full, complete inline (the old
       synchronous path) so a fence is never dropped - a lost fence would wedge
       VidMm's paging wait and TDR the adapter. */
    adapter->GpuSubmittedFence = pSubmit->SubmissionFenceId;
    if (!WinMaliSchedEnqueue_(adapter, pSubmit->SubmissionFenceId,
                              pSubmit->NodeOrdinal, pSubmit->EngineOrdinal, FALSE)) {
        /* No CSF NOP on the inline fallback either - same SubmitLock stall,
           and this path runs on the dxgk scheduler thread where blocking is
           worst. See the worker above. */
        InterlockedExchange64(&adapter->SchedLastCompletedFence,
                              (LONG64)pSubmit->SubmissionFenceId);
        WinMaliSchedPushFence_(adapter, pSubmit->SubmissionFenceId,
                               pSubmit->NodeOrdinal, pSubmit->EngineOrdinal, FALSE, 0);
        if (adapter->DxgkInterface.DxgkCbQueueDpc != NULL && adapter->DxgkHandle != NULL) {
            adapter->DxgkInterface.DxgkCbQueueDpc(adapter->DxgkHandle);
        }
    }
    return STATUS_SUCCESS;
}

/* DxgkDdiSubmitCommandVirtual - the GpuMmu (GPU-virtual-address) submit path.
   Because we report GpuMmuSupported=1, dxgkrnl submits DMA buffers through THIS
   DDI, not the legacy physical DxgkDdiSubmitCommand - in particular the paging
   buffers for our copy node (node 1). A stubbed NOT_SUPPORTED here bugchecks
   0x119 (VIDEO_SCHEDULER_INTERNAL_ERROR, arg2=0xC00000BB). Behaviour mirrors
   WinMaliKmdSubmitCommand: run a present blt if tagged, then hand the fence to
   the async worker (inline fallback if the queue is full) and return. */
_Function_class_(DXGKDDI_SUBMITCOMMANDVIRTUAL)
NTSTATUS
APIENTRY
WinMaliKmdSubmitCommandVirtual(
    IN_CONST_HANDLE                        hAdapter,
    IN_CONST_PDXGKARG_SUBMITCOMMANDVIRTUAL pSubmit)
{
    PWINMALI_ADAPTER adapter;

    if (pSubmit == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    /* ⛔ PER-SUBMIT TRACING DROWNS THE ORACLE. Once the compositor drives us
       this fires ~30x/s and, with the DMA_COMPLETED line, puts ~60 lines/s into
       the boot-log ring. The ring holds ~3500 lines, so it wraps in under a
       minute and SCROLLS AWAY StartDevice / POST-ownership / VidPn - exactly the
       lines we need. The ring is our only kernel-debugger substitute; flooding
       it costs more than the trace is worth. Off unless TraceSubmit=1. */
    if (g_WinMaliTraceSubmit != 0)
    WINMALI_TRACE("SubmitCommandVirtual: fenceId=%u node=%u engine=%u dmaVa=0x%llx dmaSize=%u",
                  pSubmit->SubmissionFenceId,
                  pSubmit->NodeOrdinal,
                  pSubmit->EngineOrdinal,
                  (unsigned long long)pSubmit->DmaBufferVirtualAddress,
                  pSubmit->DmaBufferSize);

    /* Present phase-2 blt (same tagging as the physical path). Paging submits
       carry no private data so they never enter here. */
    if (pSubmit->pDmaBufferPrivateData != NULL &&
        pSubmit->DmaBufferPrivateDataSize >= sizeof(WINMALI_BLT_DESC)) {
        PWINMALI_BLT_DESC blt = (PWINMALI_BLT_DESC)pSubmit->pDmaBufferPrivateData;
        if (blt->Magic == WINMALI_BLT_DESC_MAGIC) {
            WinMaliPresentExecuteBlt_(blt);
            blt->Magic = 0;
        }
    }

    /* Phase 3 (dxgk DMA-buffer submission, KMD half): if the DMA buffer is
       tagged with a WINMALI_CS_SUBMIT_DESC, RUN the UMD's command stream on the
       CSG - resolve the named VM to its Mali AS slot and CALL the stream, the
       same way escape GroupSubmit does. This is what lets rendering move off
       the escape onto dxgk-scheduled DMA buffers. Inert for paging buffers (no
       CS tag). Runs inline like the present blt above; a later step can move it
       onto the async worker so it doesn't hold the scheduler thread. */
    if (pSubmit->pDmaBufferPrivateData != NULL &&
        pSubmit->DmaBufferPrivateDataSize >= sizeof(WINMALI_CS_SUBMIT_DESC)) {
        PWINMALI_CS_SUBMIT_DESC cs =
            (PWINMALI_CS_SUBMIT_DESC)pSubmit->pDmaBufferPrivateData;
        if (cs->Magic == WINMALI_CS_SUBMIT_DESC_MAGIC &&
            cs->StreamGpuVa != 0 && cs->StreamSize != 0) {
            /* Phase 2 coupling: when the UMD marks this CS as dxgk-addressed
               (WINMALI_DXGK_ADDR mapped its allocations via GpuMmu), the stream
               references VAs in THIS dxgk context's page table - run against the
               AS bound to it via SetRootPageTable. Otherwise (escape VmBind
               baseline) use the escape VM's AS. Both resolve to a Mali AS slot
               the CSG rebinds to before the CALL. The flag keeps the baseline
               correct even if dxgk pre-binds an (empty) context AS. */
            ULONG    asSlot = WINMALI_AS_SLOT_MAX;
            NTSTATUS schedSt;

            /* ⛔ THE SCHEDULER LOCK MUST SPAN EnsureAs -> READ AsSlot -> SUBMIT
               AS ONE UNIT. Binding an AS slot and then reading it back are two
               operations; between them another thread could evict the slot we
               just bound and hand it to a different process, and we would run
               this command stream against someone else's page table. Taking the
               lock only inside CsfSubmitGroupStream (as the first version did)
               is far too late. KMUTEX is recursive, so the nested acquire
               inside the submit is correct and intended. */
            schedSt = WinMaliCsfAcquireSchedLock(adapter);
            if (!NT_SUCCESS(schedSt)) {
                WINMALI_WARN("SubmitCommandVirtual: sched lock unavailable "
                             "(0x%08x) - dropping vm=%u", schedSt, cs->VmId);
            } else {
                if (cs->Flags & WINMALI_CS_SUBMIT_FLAG_DXGK_ADDR) {
                    asSlot = WinMaliMmuGetContextAsSlot(adapter, pSubmit->hContext);
                }
                if (asSlot >= WINMALI_AS_SLOT_MAX) {
                    PWINMALI_VM vm = WinMaliVmGet(adapter, cs->VmId);
                    if (vm != NULL) {
                        /* Bind an AS slot now if this VM does not hold one. VMs
                           are created unbound when the 6-slot pool is full, so
                           this is the normal path for the 7th client onward. It
                           also refreshes the LRU stamp, so an actively
                           submitting VM sorts newest and is picked last. */
                        (VOID)WinMaliMmuVmEnsureAs(adapter, &vm->Pt, asSlot);
                        asSlot = vm->Pt.AsSlot;
                        WinMaliVmPut(vm);
                    }
                }
                if (asSlot < WINMALI_AS_SLOT_MAX) {
                    NTSTATUS subSt;
                    static LONG s_ddiCalib = 0;

                    WinMaliBoFlushVm(adapter, cs->VmId, TRUE);   /* CPU->DRAM */
                    subSt = WinMaliCsfSubmitGroupStream(adapter, asSlot,
                                                        cs->StreamGpuVa, cs->StreamSize,
                                                        0u, 500u);
                    WinMaliBoFlushVm(adapter, cs->VmId, FALSE);  /* DRAM->CPU */

                    /* ★★ THIS STATUS WAS CAST TO (VOID). THAT IS WHY FIVE
                     * RENDER-MODE HANGS CAME BACK WITH NO RECORDED CAUSE.
                     *
                     * This is the HOT submit path - dxgk DMA buffers, i.e. dwm's
                     * actual rendering. Its result was discarded, so a failing
                     * submit here left no counter, no log and no latch. The
                     * escape path (WinMaliEscape.c) DOES latch its failures, but
                     * it is not the path doing the work.
                     *
                     * PROVEN, not assumed: 1.1.21.5 stamped a sentinel on the
                     * escape path's first success. After 2110 successful submits
                     * the sentinel had never fired - so the escape path was
                     * essentially idle while rendering went through here.
                     *
                     * ⛔ Latch into GLOBALS only - no registry write here. We
                     * hold AsSchedLock (a KMUTEX) and a FLUSHED write is disk
                     * I/O; stalling GPU scheduling on a disk flush would make a
                     * bad moment worse. WinMaliDriver.c's publish writes these
                     * out with the flushed writer at ~12 Hz, which is well
                     * inside the seconds a wedge takes.
                     *
                     * Sentinel 0xC0FFEE02 marks THIS path live, distinct from
                     * the escape path's 0xC0FFEE01, so a zero can never again be
                     * misread as "no failure". */
                    if (!NT_SUCCESS(subSt)) {
                        (VOID)InterlockedExchange(&g_WinMaliStatCsfLastStatus,
                                                  (LONG)subSt);
                        (VOID)InterlockedExchange(&g_WinMaliStatCsfLastAs,
                                                  (LONG)asSlot);
                        (VOID)InterlockedExchange(&g_WinMaliStatCsfLastSize,
                                                  (LONG)cs->StreamSize);
                        (VOID)InterlockedIncrement(&g_WinMaliStatCsfFailCount);
                        WINMALI_WARN("SubmitCommandVirtual: CSF submit FAILED "
                                     "0x%08x (as=%u va=0x%llx size=%u)",
                                     subSt, asSlot,
                                     (ULONGLONG)cs->StreamGpuVa, cs->StreamSize);
                    } else if (InterlockedCompareExchange(&s_ddiCalib, 1, 0) == 0) {
                        (VOID)InterlockedExchange(&g_WinMaliStatCsfLastStatus,
                                                  (LONG)0xC0FFEE02u);
                    }
                } else {
                    WINMALI_WARN("SubmitCommandVirtual: CS desc vm=%u -> no AS slot",
                                 cs->VmId);
                }
                WinMaliCsfReleaseSchedLock(adapter);
            }
            cs->Magic = 0;   /* consume */
        }
    }

    adapter->GpuSubmittedFence = pSubmit->SubmissionFenceId;
    if (!WinMaliSchedEnqueue_(adapter, pSubmit->SubmissionFenceId,
                              pSubmit->NodeOrdinal, pSubmit->EngineOrdinal, FALSE)) {
        /* No CSF NOP on the inline fallback either - same SubmitLock stall,
           and this path runs on the dxgk scheduler thread where blocking is
           worst. See the worker above. */
        InterlockedExchange64(&adapter->SchedLastCompletedFence,
                              (LONG64)pSubmit->SubmissionFenceId);
        WinMaliSchedPushFence_(adapter, pSubmit->SubmissionFenceId,
                               pSubmit->NodeOrdinal, pSubmit->EngineOrdinal, FALSE, 0);
        if (adapter->DxgkInterface.DxgkCbQueueDpc != NULL && adapter->DxgkHandle != NULL) {
            adapter->DxgkInterface.DxgkCbQueueDpc(adapter->DxgkHandle);
        }
    }
    return STATUS_SUCCESS;
}

/* DxgkDdiPreemptCommand - honor a preempt request at DMA-buffer boundary.
   We can't preempt mid-buffer; enqueue an in-order marker so the worker
   drains every already-submitted buffer (completing their fences) and then
   reports DMA_PREEMPTED with the exact completed-fence boundary. If the queue
   is full, report immediately against the last completed fence. */
_Function_class_(DXGKDDI_PREEMPTCOMMAND)
NTSTATUS
APIENTRY
WinMaliKmdPreemptCommand(
    IN_CONST_HANDLE                  hAdapter,
    IN_CONST_PDXGKARG_PREEMPTCOMMAND pPreempt)
{
    PWINMALI_ADAPTER adapter;

    if (pPreempt == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    WINMALI_TRACE("PreemptCommand: node=%u engine=%u preemptFence=%u",
                  pPreempt->NodeOrdinal, pPreempt->EngineOrdinal,
                  pPreempt->PreemptionFenceId);

    if (!WinMaliSchedEnqueue_(adapter, pPreempt->PreemptionFenceId,
                              pPreempt->NodeOrdinal, pPreempt->EngineOrdinal, TRUE)) {
        UINT last = (UINT)InterlockedCompareExchange64(&adapter->SchedLastCompletedFence, 0, 0);
        WinMaliSchedPushFence_(adapter, pPreempt->PreemptionFenceId,
                               pPreempt->NodeOrdinal, pPreempt->EngineOrdinal, TRUE, last);
        if (adapter->DxgkInterface.DxgkCbQueueDpc != NULL && adapter->DxgkHandle != NULL) {
            adapter->DxgkInterface.DxgkCbQueueDpc(adapter->DxgkHandle);
        }
    }
    return STATUS_SUCCESS;
}

/* DxgkDdiCancelCommand - cancel a queued-but-not-submitted DMA buffer. Our
   submits complete near-instantly through the worker, so by the time a cancel
   arrives there is nothing outstanding to cancel; acknowledge success. */
_Function_class_(DXGKDDI_CANCELCOMMAND)
NTSTATUS
APIENTRY
WinMaliKmdCancelCommand(
    IN_CONST_HANDLE                 hAdapter,
    IN_CONST_PDXGKARG_CANCELCOMMAND pCancel)
{
    UNREFERENCED_PARAMETER(pCancel);
    if (WinMaliAdapterFromDxgkHandle((PVOID)hAdapter) == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    WINMALI_TRACE("CancelCommand: nothing outstanding (async worker drains in order)");
    return STATUS_SUCCESS;
}

/* DxgkDdiQueryEngineStatus - the scheduler's health probe (we advertise
   SupportPerEngineTDR). Report the engine as responsive whenever completed
   has caught up to submitted, OR the worker is alive and draining: our
   submits are NOP fences that complete within the 500ms CSF bound, so the
   engine is always making progress. Reporting Responsive=1 keeps the
   scheduler from escalating a false hang to a TDR. */
_Function_class_(DXGKDDI_QUERYENGINESTATUS)
NTSTATUS
APIENTRY
WinMaliKmdQueryEngineStatus(
    IN_CONST_HANDLE                  hAdapter,
    INOUT_PDXGKARG_QUERYENGINESTATUS pArgs)
{
    PWINMALI_ADAPTER adapter;

    if (pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    /* Responsive if nothing is outstanding, or the worker thread is up (it
       drains in order and always completes each fence). */
    pArgs->EngineStatus.Value = 0;
    pArgs->EngineStatus.Responsive =
        (adapter->GpuCompletedFence >= adapter->GpuSubmittedFence ||
         adapter->SubmitWorkerThread != NULL) ? 1u : 0u;
    return STATUS_SUCCESS;
}

/* DxgkDdiResetEngine - per-engine TDR. We hold no persistent GPU state for a
   dxgk submit (paging work is done CPU-side in BuildPagingBuffer, present is
   a CPU blt, and the fence is a NOP), so "resetting" is just draining any
   queued fences to completion so no waiter is stranded; the next submit's
   unconditional CSG rebind re-establishes a clean slot. Nothing was aborted
   mid-execution (LastAbortedFenceId = 0). */
_Function_class_(DXGKDDI_RESETENGINE)
NTSTATUS
APIENTRY
WinMaliKmdResetEngine(
    IN_CONST_HANDLE            hAdapter,
    INOUT_PDXGKARG_RESETENGINE pReset)
{
    PWINMALI_ADAPTER adapter;
    KIRQL            irql;
    UINT             drained = 0;

    if (pReset == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    /* Drain any pending submit requests to COMPLETED so nothing is stranded.
       Dequeue under the lock (the worker uses the same lock, so we never
       double-consume an entry). Preempt markers are dropped - a reset
       supersedes an in-flight preempt. */
    for (;;) {
        UINT    fence = 0, node = 0, engine = 0;
        BOOLEAN isPreempt = FALSE, have = FALSE;
        KeAcquireSpinLock(&adapter->SubmitQueueLock, &irql);
        if (adapter->SubmitQueueHead != adapter->SubmitQueueTail) {
            fence     = adapter->SubmitQueue[adapter->SubmitQueueHead].FenceId;
            node      = adapter->SubmitQueue[adapter->SubmitQueueHead].NodeOrdinal;
            engine    = adapter->SubmitQueue[adapter->SubmitQueueHead].EngineOrdinal;
            isPreempt = adapter->SubmitQueue[adapter->SubmitQueueHead].IsPreempt;
            adapter->SubmitQueueHead =
                (adapter->SubmitQueueHead + 1) % WINMALI_SUBMIT_QUEUE_DEPTH;
            have = TRUE;
        }
        KeReleaseSpinLock(&adapter->SubmitQueueLock, irql);
        if (!have) {
            break;
        }
        if (!isPreempt) {
            InterlockedExchange64(&adapter->SchedLastCompletedFence, (LONG64)fence);
            WinMaliSchedPushFence_(adapter, fence, node, engine, FALSE, 0);
            ++drained;
        }
    }
    if (drained != 0 &&
        adapter->DxgkInterface.DxgkCbQueueDpc != NULL && adapter->DxgkHandle != NULL) {
        adapter->DxgkInterface.DxgkCbQueueDpc(adapter->DxgkHandle);
    }

    pReset->LastAbortedFenceId = 0;   /* nothing was executing on the GPU */
    WINMALI_TRACE("ResetEngine: node=%u engine=%u drained=%u pending fences",
                  pReset->NodeOrdinal, pReset->EngineOrdinal, drained);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiPresent - swap-chain present for the render-only adapter.          */
/*                                                                          */
/* For a Blt present, record a WINMALI_BLT_DESC (src back buffer + dst      */
/* present target + rects) in the DMA buffer private data. The private data */
/* rides the DMA buffer to DxgkDdiSubmitCommand, which recognises the magic */
/* and runs WinMaliPresentExecuteBlt_ (the phase-2 CPU row copy) before     */
/* fencing the submit - so the rendered backbuffer pixels actually reach    */
/* the present target. The copy lives in SubmitCommand (not here) so it is  */
/* fenced like any other submit and is guarded by a magic that the paging   */
/* NOP path (private-data size 0) can never hit.                            */
/* Flip / ColorFill / other flags: emit nothing and succeed.                */
/* ------------------------------------------------------------------------ */
/* Present outcome counters. Driver-global (not per-adapter) because this DDI
   receives a CONTEXT handle, not a device handle, and there is no
   context->adapter helper; global is sufficient since there is one adapter.
   They exist because this DDI had NO trace and NO counter, so "no Present
   lines in the ring" was mistaken for "DWM never presents" - it proved
   nothing. Both early-outs return STATUS_SUCCESS silently, so a present can
   vanish with no symptom anywhere. Calls == NotBlt + NullCookie + Blts. */
/* DxgkDdiOpenAllocation outcomes. Calls == Resolved + Failed; ById is the
   subset of Resolved that needed the AllocId fallback because DxgkCbGetHandleData
   returned nothing. A NULL cookie here is exactly what makes DxgkDdiPresent drop
   the frame - and this path had NO registry counter at all until now. */
/* 'PaWM' blob-hash binding: Registered = created with a hash id, ByPawm =
   opens that resolved through it. Both zero => the hypothesis is untested, not
   refuted. */
LONG g_WinMaliStatPawmRegistered    = 0;
LONG g_WinMaliStatOpenByPawm        = 0;
LONG g_WinMaliStatOpenCalls         = 0;
LONG g_WinMaliStatOpenResolved      = 0;
LONG g_WinMaliStatOpenById          = 0;
LONG g_WinMaliStatOpenFailed        = 0;
LONG g_WinMaliStatOpenLastId        = 0;
LONG g_WinMaliStatOpenFailPdMagic   = 0;
LONG g_WinMaliStatOpenFailPdSize    = 0;
LONG g_WinMaliStatPresentCalls      = 0;
LONG g_WinMaliStatPresentNotBlt     = 0;
LONG g_WinMaliStatPresentNullCookie = 0;
LONG g_WinMaliStatPresentBlts       = 0;
LONG g_WinMaliStatPresentFlips      = 0;
LONG g_WinMaliStatPresentLastFlags  = 0;
LONG g_WinMaliStatPresentLastCounts = 0;
LONG g_WinMaliStatPresentSrcHandle  = 0;
LONG g_WinMaliStatPresentDstHandle  = 0;
/* Full present geometry - never captured before. Rects packed two shorts per
   DWORD to stay inside the 127-value registry budget. */
LONG g_WinMaliStatPresSrcRectTL     = 0;
LONG g_WinMaliStatPresSrcRectBR     = 0;
LONG g_WinMaliStatPresDstRectTL     = 0;
LONG g_WinMaliStatPresDstRectBR     = 0;
LONG g_WinMaliStatPresSubRectCnt    = 0;
LONG g_WinMaliStatPresDmaSize       = 0;
LONG g_WinMaliStatPresMultipass     = 0;
LONG g_WinMaliStatPresColor         = 0;
LONG g_WinMaliStatPresentDmaPrivSize = 0;
LONG g_WinMaliStatPresentPrivSize   = 0;
LONG g_WinMaliStatPresentPrivHits   = 0;
LONG g_WinMaliStatUmdPresentCtx     = 0;
LONG g_WinMaliStatCtxPrivSize       = 0;
LONG g_WinMaliStatApWithContent     = 0;
LONG g_WinMaliStatApFirstIdx        = 0;
LONG g_WinMaliStatApAdopted         = 0;
LONG g_WinMaliStatApPrevSrc         = 0;
LONG g_WinMaliStat0x92Calls         = 0;
LONG g_WinMaliStat0x92NoEnt         = 0;
LONG g_WinMaliStat0x92NotRes        = 0;
LONG g_WinMaliStat0x92Ok            = 0;
LONG g_WinMaliStat0x93Calls         = 0;
LONG g_WinMaliStat0x93Ok            = 0;
LONG g_WinMaliStat0x93Fail          = 0;
/* The 0x93 BO the present worker last scanned out of, and how painted it was.
   uvBo != 0 means the panel is being fed from a user-VA import, i.e. a path
   that never needed dxgk to aperture-map anything. */
LONG g_WinMaliStatUvSrcBo           = 0;
LONG g_WinMaliStatStripBo           = 0;
LONG g_WinMaliStatStripRows         = 0;
LONG g_WinMaliStatStripBlits        = 0;
LONG g_WinMaliStatStripNB           = 0;
LONG g_WinMaliStatFbAllocs          = 0;
LONG g_WinMaliStatFbAllocLastW      = 0;
LONG g_WinMaliStatFbAllocLastH      = 0;
LONG g_WinMaliStatFbAllocLastPitch  = 0;
LONG g_WinMaliStatMpoCapsCalls      = 0;
LONG g_WinMaliStatMpoCapsSrc        = 0;
LONG g_WinMaliStatMpoCheckCalls     = 0;
LONG g_WinMaliStatMpoCheckPlanes    = 0;
LONG g_WinMaliStatMpoCheckPostComp  = 0;
LONG g_WinMaliStatMpoCheckSrc       = 0;
LONG g_WinMaliStatMpoSetCalls       = 0;
LONG g_WinMaliStatMpoSetPlanes      = 0;
LONG g_WinMaliStatPresentMpoFlag    = 0;
LONG g_WinMaliStatPresentRedirected = 0;
LONG g_WinMaliStatMpoPostCompCaps   = 0;
LONG g_WinMaliStatMpoPostPresent    = 0;
LONG g_WinMaliStatMpoPostLayer      = 0;
LONG g_WinMaliStatPresAl0Hi         = 0;
LONG g_WinMaliStatPresAl0Lo         = 0;
LONG g_WinMaliStatPresAl1Lo         = 0;
LONG g_WinMaliStatPresAl0NonNull    = 0;
LONG g_WinMaliStatPresAl1NonNull    = 0;
LONG g_WinMaliStatVmCreates         = 0;
LONG g_WinMaliStatVmDestroys        = 0;
LONG g_WinMaliStatVmLive            = 0;
LONG g_WinMaliStatVmCreateFail      = 0;
LONG g_WinMaliStatDevCreates        = 0;
LONG g_WinMaliStatDevDestroys       = 0;
LONG g_WinMaliStatDevDestroyBadHandle = 0;
LONG g_WinMaliStatVmOwnerNull       = 0;
LONG g_WinMaliStatVmDeferredBind    = 0;
LONG g_WinMaliStatAsEvictions       = 0;
LONG g_WinMaliStatAsLateBinds       = 0;
LONG g_WinMaliStatAsEnsureFail      = 0;
LONG g_WinMaliStatUvSrcHits         = 0;
LONG g_WinMaliStatUvSrcAsked        = 0;
LONG g_WinMaliStatUvSrcCands        = 0;
LONG g_WinMaliStatUvSrcTooSmall     = 0;
LONG g_WinMaliStatUvSrcPid          = 0;
LONG g_WinMaliStatUvSrcPages        = 0;
LONG g_WinMaliStatUvSrcNCand        = 0;
LONG g_WinMaliUvCandBo[4]           = { 0 };
LONG g_WinMaliUvCandPid[4]          = { 0 };
LONG g_WinMaliUvCandBox[4]          = { 0 };
LONG g_WinMaliUvCandHits[4]         = { 0 };
LONG g_WinMaliUvCandNonBlack[4]     = { 0 };
LONG g_WinMaliUvCandChg[4]          = { 0 };
LONG g_WinMaliUvCandScans[4]        = { 0 };
LONG g_WinMaliStatUvSrcUniform      = 0;
LONG g_WinMaliStatUvSrcChg          = 0;
LONG g_WinMaliStatUvSrcEvict        = 0;
LONG g_WinMaliStatUvRevokeBig       = 0;
LONG g_WinMaliTrapRevokeBig         = 0;
LONG g_WinMaliStatBoDedup           = 0;
LONG g_WinMaliStatUvPubBig          = 0;
LONG g_WinMaliStatBoDeref           = 0;
LONG g_WinMaliStatUvRevokeAny       = 0;
LONG g_WinMaliStatUvSrcEvictBig     = 0;
LONG g_WinMaliStatUvSrcEvictBusy    = 0;
LONG g_WinMaliStatUvSrcDropSmall    = 0;
/* Why GroupSubmit rejected a frame. NoGroup = a handle this adapter instance
   never issued (the stale-handle-across-restart class); FatalGroup = the group
   exists but the GPU already faulted it. Two very different bugs. */
LONG g_WinMaliStatSubmitNoGroup     = 0;
LONG g_WinMaliStatSubmitFatalGroup  = 0;
/* The REAL GPU-side submit failure, latched. Previously only reachable through
   the EDK2 ring (fill-once, full), so callers saw a bare EIO and nothing else. */
LONG g_WinMaliStatCsfLastStatus     = 0;
LONG g_WinMaliStatCsfLastAs         = 0;
LONG g_WinMaliStatCsfLastSize       = 0;
LONG g_WinMaliStatCsfLastVaLo       = 0;
LONG g_WinMaliStatCsfLastVaHi       = 0;
LONG g_WinMaliStatCsfFailCount      = 0;
/* Submits rejected at their SYNC OPS (a stale syncobj handle), as opposed to at
   the group check. Same ENOENT code, completely different table. */
LONG g_WinMaliStatSubmitSyncFail    = 0;
LONG g_WinMaliStatSubmitSyncErr     = 0;
/* The handle that got rejected. 0 = a BO with NO sync state (mesa emitting a
   wait on nothing), which is a different bug from a stale/destroyed handle. */
LONG g_WinMaliStatSyncBadHandle     = 0;
LONG g_WinMaliStatSyncBadFlags      = 0;
LONG g_WinMaliStatSyncWaitTimeout   = 0;
LONG g_WinMaliStatSubmitSignalFail  = 0;
/* ⛔ TRIGGER-ARMED RING SNAPSHOT. 0 = idle, 1 = ARMED (a signal ENOENT just
   happened), 2 = already captured. The failure burst is ~57 s after boot and
   WinRM only arrives ~70 s in, so a TIMER-published tail always misses it - even
   at 192 KB. The escape sets this to 1 the moment the first SIGNAL fails; the
   present worker snapshots the ring into its own registry value on the next tick
   (~2 s) and latches to 2 so the evidence is never overwritten. */
LONG g_WinMaliTrapSignalFail       = 0;

/* Same triggered-snapshot mechanism, third trigger: the FIRST CSF job wait
   timeout of the boot. Every timeout after the first is the STEADY STATE of an
   already-wedged GPU (want climbs, got frozen, same tag, same stream VA), and
   that is all a timer-published TraceTail can ever show - the 256 KB ring wraps
   long before WinRM is reachable. What has never been captured is what the ring
   held JUST BEFORE the first one, which is where the cause has to be. Latched
   here, snapshotted by the present worker on the next tick. */
LONG g_WinMaliTrapFirstTmo         = 0;

/* TILER_OOM servicing. StatTilerOom counts requests we answered; StatTilerOomFail
   counts those we could not satisfy (chunk cap, allocation failure, no heap on
   the VM) and had to ack with a NULL chunk. Before the fix both were structurally
   impossible to observe: the request was never read, so a heap exhaustion looked
   exactly like a healthy idle GPU with a frozen CsOk. */
LONG g_WinMaliStatTilerOomCount    = 0;
LONG g_WinMaliStatTilerOomFail     = 0;
/* Table rebuild census. Three theories assumed the per-adapter syncobj table is
   rebuilt mid-boot; nobody ever counted. Published so the answer is a number,
   not a ring-capture race. */
LONG g_WinMaliStatSyncTableInits    = 0;
LONG g_WinMaliStatSyncTableTeardowns = 0;
LONG g_WinMaliStatSignalErr         = 0;
LONG g_WinMaliStatSignalBadHandle   = 0;
LONG g_WinMaliFlipAllocW            = 0;
LONG g_WinMaliFlipAllocH            = 0;
LONG g_WinMaliFlipAllocPitch        = 0;
LONG g_WinMaliFlipAllocFmt          = 0;
LONG g_WinMaliFlipAllocSize         = 0;
LONG g_WinMaliFlipAllocId           = 0;
LONG g_WinMaliApMapOps             = 0;
LONG g_WinMaliApMapLastSize        = 0;
LONG g_WinMaliApMapMaxSize         = 0;
PVOID g_WinMaliFlipAlloc          = NULL;
LONG g_WinMaliStatFlipSrcUsed      = 0;
LONG g_WinMaliStatFlipSrcMapFail   = 0;
LONG g_WinMaliFlipWorkerMdl        = 0;
LONG g_WinMaliFlipWorkerPages      = 0;
/* Present-worker chunked-copy accounting. StatCopyAbort counts frames where the
   flip allocation moved mid-copy (we bail and leave the rest stale rather than
   fault); StatCopyDone is KB copied on the last pass, so a partial frame is
   visible as a number instead of only as a half-painted panel. */
LONG g_WinMaliStatCopyAbort        = 0;
LONG g_WinMaliStatCopyDone         = 0;
/* ⛔★★★★★ ONE SOURCE PER SWEEP.
   A wake copies a 2 MB slice, so a full 21,667,840 B frame takes ~11 wakes - and
   the selection used to re-run on EVERY wake, letting consecutive strips come
   from different surfaces. USER-OBSERVED on 1208: "the lines were changing
   places on every reprint". LatchBo holds the winner for the whole sweep;
   LatchDrop counts sweeps abandoned because the latched candidate died. */
LONG g_WinMaliUvLatchBo            = 0;
/* Last source we successfully adopted, kept ACROSS sweeps. On a tick where
   nothing qualifies we repaint from this instead of skipping the frame - the
   freeze half of the user-observed 'flash, render, freeze, repeat'. */
LONG g_WinMaliUvLastGoodBo         = 0;
LONG g_WinMaliStatUvHeldOver       = 0;
LONG g_WinMaliStatLatchDrop        = 0;
LONG g_WinMaliStatSweeps           = 0;   /* full frames completed */
/* Times WinMaliUvSrcRevoke gave up waiting for an in-flight copy to release its
   use-ref (1 s bound). Should be 0; anything else means a use-ref was leaked and
   a BO teardown proceeded while the worker might still have been reading. */
LONG g_WinMaliStatUvRevokeTimeout  = 0;
/* Times SetPowerState stopped the present worker. Was a pure stub: a remote
   shutdown was accepted and then hung for 20 minutes with the worker still
   painting through it, needing a hard reset. */
LONG g_WinMaliStatPowerStop        = 0;
/* Painted extent of the adopted source, percent of its own rows. Selection now
   prefers coverage over pure recency: on a live desktop the freshest
   size-qualifying surface is often a partial-update strip. */
LONG g_WinMaliStatUvSrcCoverage    = 0;
LONG g_WinMaliStatUvSrcMaxCov      = 0;
LONG g_WinMaliStatUvSrcMaxChg      = 0;
/* Denominator for MaxCov: candidates that passed the size gate this tick.
   MaxCov=0 with Qual=0 means 'nothing qualified', with Qual>0 means 'all blank'
   - opposite problems that read identically without this. */
LONG g_WinMaliStatUvSrcQual        = 0;
/* 1 = the scanout got a WRITE-COMBINED mapping, 0 = it fell back to UNCACHED.
   Uncached is ~1000x slower for a 21 MB frame (~20 s vs ~30 ms) and was the
   silent cause of the visible top-down paint AND of teardown hangs. */
LONG g_WinMaliScanoutWc            = 0;
LONG g_WinMaliScanoutWcOff         = 0;
LONG g_WinMaliScanoutAttr          = -1;
LONG g_WinMaliFlipHasMdl           = 0;
LONG g_WinMaliFlipMdlPages         = 0;
LONG g_WinMaliFlipAllocOk           = 0;
LONG g_WinMaliFlipAllocBad          = 0;
LONG g_WinMaliStatCsSubmitCount     = 0;
LONG g_WinMaliStatCsOkCount         = 0;
LONG g_WinMaliStatRingInsert        = 0;
LONG g_WinMaliStatRingExtract       = 0;
LONG g_WinMaliStatCsReq             = 0;
LONG g_WinMaliStatCsAck             = 0;
LONG g_WinMaliStatCsgReq            = 0;
LONG g_WinMaliStatCsgAck            = 0;
LONG g_WinMaliStatCsFatalInfoLo     = 0;
LONG g_WinMaliStatCsFatalInfoHi     = 0;
LONG g_WinMaliStatSeqnoLo           = 0;
LONG g_WinMaliStatTagLo             = 0;
/* The CSF timeout diagnosis. Every one of these is already gathered at the
   timeout site and was going ONLY to the fill-once EDK2 ring, so a hung submit
   reported itself as a bare STATUS_IO_TIMEOUT with the explanation discarded. */
LONG g_WinMaliStatCsTimeoutCount    = 0;
LONG g_WinMaliStatCsCmdPtrLo        = 0;
LONG g_WinMaliStatCsCmdPtrHi        = 0;
LONG g_WinMaliStatCsBlocked         = 0;
LONG g_WinMaliStatCsFault           = 0;
LONG g_WinMaliStatCsFatal           = 0;
LONG g_WinMaliStatCsCalleeRet       = 0;
LONG g_WinMaliStatAsFaultSt         = 0;
LONG g_WinMaliStatAsFaultLo         = 0;
LONG g_WinMaliStatAsFaultHi         = 0;
LONG g_WinMaliStatMcuFaultSt        = 0;
LONG g_WinMaliStatGpuFaultSt        = 0;
LONG g_WinMaliUvCandPages[4]        = { 0 };

/* The allocation DWM most recently FLIPPED to. The present worker copies from
   this in preference to the address-matched aperture allocation, because dxgk
   latches a scanout address only twice (at mode-set) so the address-based
   lookup is frozen on a surface nothing ever writes. Published as a plain
   pointer under Interlocked access - one adapter, and the worker only ever
   reads it. */
PVOID g_WinMaliFlipSourceAlloc = NULL;

/* ⛔★★★★★ EVERY EXIT FROM DxgkDdiPresent MUST EMIT A NON-EMPTY DMA BUFFER.
 *
 * MEASURED 2026-08-18 from a FULL-BOOT DxgKrnl ETW capture, event id=494:
 *   x25  "DMABufferLength > 0 || GetVidSchExport()->VidSchIsTDRPending(...)"
 *   x25  "... || static_cast<BYTE*>(pPresentDdiArg->pDmaBuffer) >
 *          static_cast<BYTE*>(pDmaBuffer)"
 * against StatPresentCalls = 25. A 25:25 match - EVERY call to this DDI fired
 * both asserts, because all four early-outs below returned STATUS_SUCCESS
 * without ever touching pPresent->pDmaBuffer. Only the Blt tail emitted one.
 *
 * THE CONTRACT: the driver translates the present into GPU commands in
 * pDmaBuffer and leaves the pointer PAST the last byte written; dxgk derives
 * DmaBufferLength from that delta and asserts it is non-zero. Our real payload
 * rides in pDmaBufferPrivateData (WINMALI_BLT_DESC, consumed by SubmitCommand),
 * so the DMA buffer only has to be non-empty - but it MUST be non-empty.
 *
 * ⚠ "Too small" is NOT skip-and-succeed - that shape is how this bug was born.
 * STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER makes dxgk re-issue with more room,
 * and is already the documented contract elsewhere in this driver
 * (see WinMaliPaging.c:16).
 */
static NTSTATUS
WinMaliPresentEmitDmaToken_(_Inout_ DXGKARG_PRESENT* pPresent)
{
    if (pPresent->pDmaBuffer == NULL || pPresent->DmaSize < sizeof(ULONG)) {
        /* Marker string doubles as the DEPLOY PROOF: grep the packaged .sys
           for WMDMATOK1. A hash cannot serve - signing changes it. */
        WINMALI_WARN("Present: WMDMATOK1 dma buffer too small (DmaSize=%u) "
                     "- returning INSUFFICIENT_DMA_BUFFER", pPresent->DmaSize);
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    *(ULONG UNALIGNED*)pPresent->pDmaBuffer = WINMALI_BLT_DESC_MAGIC;
    pPresent->pDmaBuffer = (PVOID)((PUCHAR)pPresent->pDmaBuffer + sizeof(ULONG));
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_PRESENT)
NTSTATUS
APIENTRY
WinMaliKmdPresent(
    IN_CONST_HANDLE        hContext,
    INOUT_PDXGKARG_PRESENT pPresent)
{
    PWINMALI_KMD_ALLOCATION src;
    PWINMALI_KMD_ALLOCATION dst;
    PWINMALI_BLT_DESC       desc;

    /* hContext is now USED: it is our PWINMALI_KMD_CONTEXT, and the by-AllocId
       source resolution below needs the adapter it carries. */
    if (pPresent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* ★ COUNT EVERY OUTCOME. This DDI had NO trace and NO counter, so "zero
     * Present lines in the ring" was read as "DWM never presents" when it
     * actually proved nothing - the classic absence-is-not-evidence trap.
     *
     * It matters because BOTH early-outs below return STATUS_SUCCESS silently:
     * the swap chain proceeds, DWM is happy, and nothing reaches the panel.
     * In particular hDeviceSpecificAllocation is exactly what
     * DxgkDdiOpenAllocation fills in, and that was NULL for 195/195 opens all
     * session - so every present almost certainly died on the src/dst check
     * with no symptom anywhere. The AllocId fix should have just unblocked it;
     * these counters are how we find out. */
    (VOID)InterlockedIncrement(&g_WinMaliStatPresentCalls);

    /* ★ CAPTURE THE ACTUAL FLAGS. Measured: 22 presents, ALL classified
     * "notblt", and the flip branch below fired ZERO times - so they are
     * neither Blt nor Flip, and every guess about which kind they are has been
     * wrong so far. Record the raw Flags word (and the allocation-list shape)
     * so the DDI itself says what DWM is asking for instead of us inferring. */
    (VOID)InterlockedExchange(&g_WinMaliStatPresentLastFlags,
                              (LONG)pPresent->Flags.Value);

    /* ★ TWO FREE COUNTERS THAT MAKE THE NEXT TWO EXPERIMENTS REGISTRY-ONLY.
     *
     * FlipWithMultiPlaneOverlay says the OS is driving us through the MPO path,
     * and it also names a UNION arm - the same union family that has already
     * cost us a wrong reading once (pAllocationList vs pAllocationInfo), so it
     * is worth knowing BEFORE dereferencing anything on an MPO present.
     *
     * RedirectedFlip is the documented route by which redirected-surface content
     * reaches a miniport. If it ever appears, the iflip pivot becomes a knob
     * change rather than a build. Counting both now costs nothing and saves a
     * whole deploy cycle later. */
    if (pPresent->Flags.FlipWithMultiPlaneOverlay) {
        (VOID)InterlockedIncrement(&g_WinMaliStatPresentMpoFlag);
    }
    if (pPresent->Flags.RedirectedFlip) {
        (VOID)InterlockedIncrement(&g_WinMaliStatPresentRedirected);
    }
    (VOID)InterlockedExchange(&g_WinMaliStatPresentLastCounts,
                              (LONG)((pPresent->NumSrcAllocations & 0xFFFF) |
                                     ((pPresent->NumDstAllocations & 0xFF) << 16) |
                                     ((pPresent->pAllocationList != NULL ? 1 : 0) << 24) |
                                     ((pPresent->pDmaBufferPrivateData != NULL ? 1 : 0) << 25)));

    /* ⛔ DO NOT REQUIRE NumSrcAllocations/NumDstAllocations >= 1.
     *
     * MEASURED via the registry counters (2026-08-15): every present DWM makes
     * arrives as
     *     Flags.Value = 0x00000001  (Blt)
     *     NumSrc = 0, NumDst = 0, pAllocationList != NULL, dmaPriv != NULL
     * so this guard rejected 22/22 of them ON THE COUNTS while the flags and
     * both pointers were perfectly valid - then returned STATUS_SUCCESS, so the
     * frame vanished with no error anywhere and the panel kept the mode-set
     * image. THAT is the black screen.
     *
     * Source and destination live at the FIXED indices
     * DXGK_PRESENT_SOURCE_INDEX / DXGK_PRESENT_DESTINATION_INDEX; the counts are
     * not how you validate them. The real check is the cookie NULL-test just
     * below, which only became meaningful once OpenAllocation started resolving
     * (it was NULL 195/195 before the AllocId work).
     *
     * ⚠ I previously "fixed" this by implementing Flags.Flip, on the theory that
     * DWM used the flip model - inferred from flipLatch=2 and never verified.
     * flips=0 refuted it. Do not re-add a Flip precondition without evidence. */
    if (!pPresent->Flags.Blt ||
        pPresent->pAllocationList == NULL ||
        pPresent->pDmaBufferPrivateData == NULL ||
        pPresent->DmaBufferPrivateDataSize < sizeof(WINMALI_BLT_DESC)) {
        /* ★ HANDLE THE FLIP MODEL.
         *
         * MEASURED: DWM presents exclusively via flips here
         * (present=24 / notblt=24 / blt=0). This function only ever implemented
         * Blt ("Flip/ColorFill: emit nothing and succeed"), so every one of
         * DWM's frames was discarded with STATUS_SUCCESS and the panel kept
         * showing the mode-set surface. That is the black screen.
         *
         * We have no display engine to program, so instead of scanning out the
         * flipped allocation we record it: the present worker copies from it
         * into the EDK2 framebuffer. Taking the SOURCE allocation is what makes
         * DWM's composed frame reach the panel.
         *
         * hDeviceSpecificAllocation is only non-NULL because OpenAllocation now
         * resolves (it was NULL 195/195 before the AllocId work), so this could
         * not have worked earlier even if the case had been implemented. */
        if (pPresent->Flags.Flip &&
            pPresent->pAllocationList != NULL &&
            pPresent->NumSrcAllocations >= 1) {
            /* Same union/stride correction as the Blt path below. */
            PWINMALI_KMD_ALLOCATION fsrc = (PWINMALI_KMD_ALLOCATION)
                pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
            if (fsrc != NULL && fsrc->Magic == WINMALI_KMD_ALLOC_MAGIC) {
                (VOID)InterlockedExchangePointer(&g_WinMaliFlipSourceAlloc, fsrc);
                (VOID)InterlockedIncrement(&g_WinMaliStatPresentFlips);
                return WinMaliPresentEmitDmaToken_(pPresent);
            }
        }

        /* Nothing we can (or need to) do; complete so the swap chain proceeds. */
        (VOID)InterlockedIncrement(&g_WinMaliStatPresentNotBlt);
        return WinMaliPresentEmitDmaToken_(pPresent);
    }

    /* ⛔★★★★★★ THIS IS THE DOCUMENTED CONTRACT. DO NOT "FIX" IT AGAIN.
     *
     * I switched these reads to `pAllocationInfo` on the theory that a WDDM2 /
     * GpuMmu driver is handed that union member (24 B vs 32 B stride, so indices
     * 1 and 2 would land mid-struct and yield garbage). It was WRONG, and the
     * MS documentation for DXGKARG_PRESENT says so in two places:
     *
     *   pAllocationInfo - "Reserved for system use. The display miniport driver
     *                      should IGNORE this member."
     *   pAllocationList - "The driver accesses the source allocation handle
     *                      through the hDeviceSpecificAllocation member of the
     *                      pAllocationList[DXGK_PRESENT_SOURCE_INDEX] element
     *                      (that is, element 1)... destination ... element 2."
     *
     * ⇒ pAllocationList at indices 1/2 was correct all along. REVERTED.
     *
     * ★★★ AND THE NULL IS NOT AN ERROR:
     *   "If a source or destination IS NOT PRESENT FOR THE OPERATION, the
     *    hDeviceSpecificAllocation member of the respective element is NULL."
     *   "The hDeviceSpecificAllocation member of the FIRST element (element 0)
     *    is ALWAYS NULL."
     * So StatPresentNullCookie counts presents that legitimately carry no
     * source/destination allocation - it is NOT proof of a binding failure.
     * MEASURED and consistent with that: OpenAllocation resolves 540/542, so
     * cookies bind fine; these presents simply do not reference allocations.
     *
     * ⚠ ALSO WRONG: NumSrcAllocations/NumDstAllocations are documented as "the
     * number of CONTEXT allocations in the source/destination" - nothing to do
     * with whether a blt source exists. Reading 0/0 as "the list is empty" was
     * an inference, not a fact. */
    src = (PWINMALI_KMD_ALLOCATION)
        pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
    dst = (PWINMALI_KMD_ALLOCATION)
        pPresent->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation;
    if (src == NULL || dst == NULL) {
        /* Capture WHAT the list actually holds. nullcookie=23 with NumSrc=0 and
           NumDst=0 says the list is empty, but "empty" could equally be a
           populated list at different indices - publish the raw hAllocation
           values so the DDI says which, instead of another inference. */
        (VOID)InterlockedExchange(&g_WinMaliStatPresentSrcHandle,
                (LONG)(ULONG_PTR)pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation);
        (VOID)InterlockedExchange(&g_WinMaliStatPresentDstHandle,
                (LONG)(ULONG_PTR)pPresent->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation);
        (VOID)InterlockedExchange(&g_WinMaliStatPresentDmaPrivSize,
                (LONG)pPresent->DmaBufferPrivateDataSize);

        /* ⛔★★★★★ CHARACTERISE THE PRESENT INSTEAD OF GUESSING AT IT.
         *
         * MEASURED with a trustworthy instrument at last: SRC and DST handles
         * are BOTH genuinely 0. Per the DXGKARG_PRESENT docs that means neither
         * a source nor a destination allocation is part of this operation - so
         * it is not an allocation-to-allocation blt, and every theory built on
         * "the cookies will not resolve" was chasing something that was never
         * there.
         *
         * What IS it, then? The struct carries geometry we have never once
         * looked at. Publish it: rects packed two-per-DWORD to stay inside the
         * registry budget, plus the DMA sizing and the multipass cursor. A
         * ColorFill would show Color set and Flags.ColorFill; a real blt shows
         * meaningful rects; a degenerate/synchronisation present shows empty
         * ones. That distinction is not inferable from what we log today. */
        (VOID)InterlockedExchange(&g_WinMaliStatPresSrcRectTL,
                (LONG)(((ULONG)(pPresent->SrcRect.left & 0xFFFF)) |
                       ((ULONG)(pPresent->SrcRect.top  & 0xFFFF) << 16)));
        (VOID)InterlockedExchange(&g_WinMaliStatPresSrcRectBR,
                (LONG)(((ULONG)(pPresent->SrcRect.right  & 0xFFFF)) |
                       ((ULONG)(pPresent->SrcRect.bottom & 0xFFFF) << 16)));
        (VOID)InterlockedExchange(&g_WinMaliStatPresDstRectTL,
                (LONG)(((ULONG)(pPresent->DstRect.left & 0xFFFF)) |
                       ((ULONG)(pPresent->DstRect.top  & 0xFFFF) << 16)));
        (VOID)InterlockedExchange(&g_WinMaliStatPresDstRectBR,
                (LONG)(((ULONG)(pPresent->DstRect.right  & 0xFFFF)) |
                       ((ULONG)(pPresent->DstRect.bottom & 0xFFFF) << 16)));
        (VOID)InterlockedExchange(&g_WinMaliStatPresSubRectCnt,
                (LONG)pPresent->SubRectCnt);
        (VOID)InterlockedExchange(&g_WinMaliStatPresDmaSize,
                (LONG)pPresent->DmaSize);
        (VOID)InterlockedExchange(&g_WinMaliStatPresMultipass,
                (LONG)pPresent->MultipassOffset);
        (VOID)InterlockedExchange(&g_WinMaliStatPresColor,
                (LONG)pPresent->Color);

        /* ★ RESOLVE THE SOURCE BY AllocId INSTEAD.
         *
         * MEASURED: the list is genuinely EMPTY (NumSrc=0, NumDst=0, both
         * pointers valid, cookies NULL at indices 1/2) - there is no other slot
         * the cookies are hiding in. The UMD therefore names the source in the
         * present private driver data, which the WDK documents as "Private
         * driver data to pass to DdiPresent". Same AllocId identity that already
         * makes OpenAllocation resolve 252/252.
         *
         * ⚠ This is pPresent->pPrivateDriverData - NOT pDmaBufferPrivateData
         * (ours, WINMALI_BLT_DESC). Different channel, easy to conflate. */
        (VOID)InterlockedExchange(&g_WinMaliStatPresentPrivSize,
                                  (LONG)pPresent->PrivateDriverDataSize);

        if (pPresent->pPrivateDriverData != NULL &&
            pPresent->PrivateDriverDataSize >= sizeof(WINMALI_PRESENT_PRIV)) {
            const WINMALI_PRESENT_PRIV* pp =
                (const WINMALI_PRESENT_PRIV*)pPresent->pPrivateDriverData;

            /* hContext IS our PWINMALI_KMD_CONTEXT (set in CreateContext), and
               it carries the adapter the AllocId table lives on. Validate the
               magic before trusting it. */
            PWINMALI_KMD_CONTEXT ctx = (PWINMALI_KMD_CONTEXT)hContext;
            PWINMALI_ADAPTER     ad  =
                (ctx != NULL && ctx->Magic == WINMALI_KMD_CONTEXT_MAGIC)
                    ? ctx->Adapter : NULL;

            if (pp->Magic == WINMALI_PRESENT_PRIV_MAGIC && pp->SrcAllocId != 0 &&
                ad != NULL) {
                PWINMALI_KMD_ALLOCATION fsrc =
                    WinMaliAllocLookupById(ad, pp->SrcAllocId, pp->SrcAllocIdHi);
                if (fsrc != NULL) {
                    /* Record it as the scanout source. The present worker
                       already copies from g_WinMaliFlipSourceAlloc, so naming
                       the source is enough to get composed pixels on screen. */
                    (VOID)InterlockedExchangePointer(&g_WinMaliFlipSourceAlloc,
                                                     fsrc);
                    (VOID)InterlockedIncrement(&g_WinMaliStatPresentPrivHits);
                    return WinMaliPresentEmitDmaToken_(pPresent);
                }
            }
        }

        /* ★★★ LOOK IN THE ALLOCATION LIST BEFORE GIVING UP.
         *
         * We are the ONLY display adapter on this machine (measured: one
         * Win32_VideoController, one PnP Display device, one active child), so
         * dwm's composed frame has nowhere to go EXCEPT through these presents -
         * yet all of them land here and we drop them.
         *
         * The reason we drop them is a COUNT check: NumSrcAllocations and
         * NumDstAllocations both read 0. But those two fields are WDDM2_0
         * additions to DXGKARG_PRESENT, and pAllocationList itself is NON-NULL
         * on every one of these presents. If dxgk populates the list without
         * setting the WDDM2_0 counts for this present shape, the frame has been
         * sitting one dereference away the whole time.
         *
         * ⚠ pAllocationList is a UNION with pAllocationInfo and
         * pPresentMultiPlaneOverlayInfo - the same union that has already
         * produced one wrong reading here - so publish the RAW contents and let
         * the numbers say which arm is live rather than assuming. Read-only, and
         * only on the path that is already failing. */
        {
            const DXGK_ALLOCATIONLIST *al = pPresent->pAllocationList;
            if (al != NULL) {
                (VOID)InterlockedExchange(&g_WinMaliStatPresAl0Hi,
                    (LONG)((ULONG_PTR)al[0].hDeviceSpecificAllocation >> 32));
                (VOID)InterlockedExchange(&g_WinMaliStatPresAl0Lo,
                    (LONG)((ULONG_PTR)al[0].hDeviceSpecificAllocation & 0xFFFFFFFFu));
                (VOID)InterlockedExchange(&g_WinMaliStatPresAl1Lo,
                    (LONG)((ULONG_PTR)al[1].hDeviceSpecificAllocation & 0xFFFFFFFFu));
                /* If either entry resolves to one of OUR allocations, the frame
                   is reachable and the count check is the only thing in the way. */
                if (al[0].hDeviceSpecificAllocation != NULL) {
                    (VOID)InterlockedIncrement(&g_WinMaliStatPresAl0NonNull);
                }
                if (al[1].hDeviceSpecificAllocation != NULL) {
                    (VOID)InterlockedIncrement(&g_WinMaliStatPresAl1NonNull);
                }
            }
        }

        /* THE suspected black-screen path: a NULL cookie here means
           OpenAllocation never bound this surface to one of ours. */
        (VOID)InterlockedIncrement(&g_WinMaliStatPresentNullCookie);
        return WinMaliPresentEmitDmaToken_(pPresent);
    }

    (VOID)InterlockedIncrement(&g_WinMaliStatPresentBlts);

    desc = (PWINMALI_BLT_DESC)pPresent->pDmaBufferPrivateData;
    desc->Magic      = WINMALI_BLT_DESC_MAGIC;
    desc->SubRectCnt = pPresent->SubRectCnt;
    desc->Src        = src;
    desc->Dst        = dst;
    desc->SrcLeft    = pPresent->SrcRect.left;
    desc->SrcTop     = pPresent->SrcRect.top;
    desc->DstLeft    = pPresent->DstRect.left;
    desc->DstTop     = pPresent->DstRect.top;
    desc->DstRight   = pPresent->DstRect.right;
    desc->DstBottom  = pPresent->DstRect.bottom;

    /* Render-only present blt (RosKmd model): there is no display engine and no
       DMA copy engine, so do the copy on the CPU right here. Both allocations
       must be resident (ApertureMdl set by BuildPagingBuffer MAP_APERTURE_SEGMENT)
       - the UMD pins the backbuffer via LockCb, and dxgk makes the destination
       resident for the present. If the destination is a cross-adapter surface we
       do not own pages for (windowed DWM redirection), ApertureMdl is NULL and we
       log + skip (that case needs a cross-adapter path, not this direct blt). */
    {
        PUCHAR srcBase = NULL, dstBase = NULL;

        if (src->ApertureMdl != NULL) {
            srcBase = (PUCHAR)MmGetSystemAddressForMdlSafe(
                src->ApertureMdl, NormalPagePriority | MdlMappingNoExecute);
            if (srcBase != NULL)
                srcBase += (SIZE_T)src->ApertureMdlOffset * PAGE_SIZE;
        }
        if (dst->ApertureMdl != NULL) {
            dstBase = (PUCHAR)MmGetSystemAddressForMdlSafe(
                dst->ApertureMdl, NormalPagePriority | MdlMappingNoExecute);
            if (dstBase != NULL)
                dstBase += (SIZE_T)dst->ApertureMdlOffset * PAGE_SIZE;
        }

        if (srcBase != NULL && dstBase != NULL &&
            src->Pitch != 0 && dst->Pitch != 0) {
            ULONG bpp = (dst->Width != 0) ? (dst->Pitch / dst->Width) : 4u;
            LONG  w   = pPresent->DstRect.right  - pPresent->DstRect.left;
            LONG  h   = pPresent->DstRect.bottom - pPresent->DstRect.top;
            LONG  y;

            if (bpp == 0) bpp = 4u;
            for (y = 0; y < h; ++y) {
                LONG   sy = pPresent->SrcRect.top  + y;
                LONG   dy = pPresent->DstRect.top  + y;
                SIZE_T sOff, dOff, rowBytes;

                if (sy < 0 || dy < 0 || w <= 0)
                    continue;
                sOff = (SIZE_T)sy * src->Pitch + (SIZE_T)pPresent->SrcRect.left * bpp;
                dOff = (SIZE_T)dy * dst->Pitch + (SIZE_T)pPresent->DstRect.left * bpp;
                rowBytes = (SIZE_T)w * bpp;
                /* Never read/write past either allocation. */
                if (sOff + rowBytes > src->Size || dOff + rowBytes > dst->Size)
                    break;
                RtlCopyMemory(dstBase + dOff, srcBase + sOff, rowBytes);
            }
            WINMALI_TRACE("Present: Blt COPIED %dx%d bpp=%u src=%p dst=%p "
                          "dst=[%d,%d..%d,%d]",
                          w, h, bpp, src, dst, pPresent->DstRect.left,
                          pPresent->DstRect.top, pPresent->DstRect.right,
                          pPresent->DstRect.bottom);
        } else {
            WINMALI_WARN("Present: Blt SKIPPED (src resident=%d dst resident=%d) "
                         "- destination likely cross-adapter (DWM), needs a "
                         "cross-adapter copy path",
                         srcBase != NULL, dstBase != NULL);
        }
    }

    /* Consume a DMA-buffer token so dxgk still routes this through the submit
       path (fence/complete); the copy above already happened synchronously. */
    return WinMaliPresentEmitDmaToken_(pPresent);
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiControlInterrupt - WDDM 2.x wants a real implementation; minimal. */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_CONTROLINTERRUPT)
NTSTATUS
APIENTRY
WinMaliKmdControlInterrupt(
    IN_CONST_HANDLE             hAdapter,
    IN_CONST_DXGK_INTERRUPT_TYPE InterruptType,
    IN_BOOLEAN                  EnableInterrupt)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(InterruptType);
    UNREFERENCED_PARAMETER(EnableInterrupt);
    /* IRQ enable/disable goes through Mali register writes from FW init.
       Just acknowledge the request here. */
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiResetFromTimeout / RestartFromTimeout - TDR. No-op success.        */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_RESETFROMTIMEOUT)
NTSTATUS
APIENTRY
WinMaliKmdResetFromTimeout(
    IN_CONST_HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    WINMALI_TRACE("ResetFromTimeout");
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_RESTARTFROMTIMEOUT)
NTSTATUS
APIENTRY
WinMaliKmdRestartFromTimeout(
    IN_CONST_HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    WINMALI_TRACE("RestartFromTimeout");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiCollectDbgInfo - return a tiny ASCII blob with our flags.          */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_COLLECTDBGINFO)
NTSTATUS
APIENTRY
WinMaliKmdCollectDbgInfo(
    IN_CONST_HANDLE                hAdapter,
    IN_CONST_PDXGKARG_COLLECTDBGINFO pArgs)
{
    PWINMALI_ADAPTER adapter;
    if (pArgs == NULL || pArgs->pBuffer == NULL || pArgs->BufferSize == 0) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    /* One-line summary; truncate to caller's buffer. */
    (VOID)RtlStringCbPrintfA((PCHAR)pArgs->pBuffer, pArgs->BufferSize,
        "WinMali flags=0x%08x regs=%u mcu=%u csf=%u",
        adapter->AdapterFlags,
        adapter->GpuRegsMapped,
        (adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_MCU_ALIVE) ? 1 : 0,
        (adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_CSF_JOBS) ? 1 : 0);
    return STATUS_SUCCESS;
}

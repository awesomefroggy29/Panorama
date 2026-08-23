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
    UINT64              CsfReservedGpuVa;
    BOOLEAN             CsfGpuVaReserved;
    /* Process-private subtree installed beneath the one root entry reserved
       at WINMALI_CSF_RESERVED_GPU_VA_BASE.  Hardware layout for this 1 GiB
       range is root -> ReservedL2[0] -> ReservedL1[0..511] -> leaf PTEs.
       Leaf zero contains a copy of the adapter-global CSF mappings; remaining
       leaves are allocated lazily for process-private tiler heaps. */
    FAST_MUTEX          ReservedPtLock;
    PVOID               ReservedL2PtVa;
    PHYSICAL_ADDRESS    ReservedL2PtPa;
    PVOID               ReservedL1PtVa;
    PHYSICAL_ADDRESS    ReservedL1PtPa;
    PVOID               ReservedLeafPtVa[512];
    PHYSICAL_ADDRESS    ReservedLeafPtPa[512];
    RTL_BITMAP          ReservedGpuVaBitmap;
    PULONG              ReservedGpuVaBitmapBuffer;
    ULONG               ReservedGpuVaPageCount;
    BOOLEAN             ReservedPtValid;
} WINMALI_KMD_PROCESS, *PWINMALI_KMD_PROCESS;

/* Per-device state. Declared before the process-reserved mapping helpers
   because their opaque owner token is this object. */
typedef struct _WINMALI_KMD_DEVICE {
    ULONG                 Magic;
    PWINMALI_ADAPTER      Adapter;
    PWINMALI_KMD_PROCESS  OwnerProcess;  /* may be NULL on pre-WDDM2 path */
    HANDLE                hRtDevice;
    UINT                  Flags;
    ULONG                 Pasid;
    LONG                  ContextCount;
} WINMALI_KMD_DEVICE, *PWINMALI_KMD_DEVICE;

BOOLEAN
WinMaliKmdGetProcessReservedPageTable(
    _In_opt_ HANDLE hKmdProcess,
    _Out_ UINT64* ReservedBase,
    _Out_ UINT64* RootChildPhysicalAddress)
{
    PWINMALI_KMD_PROCESS proc = (PWINMALI_KMD_PROCESS)hKmdProcess;

    if (ReservedBase == NULL || RootChildPhysicalAddress == NULL) {
        return FALSE;
    }
    *ReservedBase = 0;
    *RootChildPhysicalAddress = 0;
    if (proc == NULL || proc->Magic != WINMALI_KMD_PROCESS_MAGIC ||
        !proc->CsfGpuVaReserved || !proc->ReservedPtValid ||
        proc->ReservedL2PtVa == NULL ||
        proc->ReservedL2PtPa.QuadPart == 0) {
        return FALSE;
    }
    *ReservedBase = proc->CsfReservedGpuVa;
    *RootChildPhysicalAddress = (UINT64)proc->ReservedL2PtPa.QuadPart;
    return TRUE;
}

static VOID
WinMaliKmdFreeProcessReservedPt_(_Inout_ PWINMALI_KMD_PROCESS proc)
{
    ULONG i;

    if (proc == NULL) {
        return;
    }
    proc->ReservedPtValid = FALSE;
    if (proc->ReservedGpuVaBitmapBuffer != NULL) {
        ExFreePoolWithTag(proc->ReservedGpuVaBitmapBuffer, WINMALI_POOL_TAG);
        proc->ReservedGpuVaBitmapBuffer = NULL;
        proc->ReservedGpuVaPageCount = 0;
    }
    for (i = 0; i < RTL_NUMBER_OF(proc->ReservedLeafPtVa); ++i) {
        if (proc->ReservedLeafPtVa[i] != NULL) {
            MmFreeContiguousMemory(proc->ReservedLeafPtVa[i]);
            proc->ReservedLeafPtVa[i] = NULL;
            proc->ReservedLeafPtPa[i].QuadPart = 0;
        }
    }
    if (proc->ReservedL1PtVa != NULL) {
        MmFreeContiguousMemory(proc->ReservedL1PtVa);
        proc->ReservedL1PtVa = NULL;
        proc->ReservedL1PtPa.QuadPart = 0;
    }
    if (proc->ReservedL2PtVa != NULL) {
        MmFreeContiguousMemory(proc->ReservedL2PtVa);
        proc->ReservedL2PtVa = NULL;
        proc->ReservedL2PtPa.QuadPart = 0;
    }
}

static NTSTATUS
WinMaliKmdInitProcessReservedPt_(
    _In_ PWINMALI_ADAPTER adapter,
    _Inout_ PWINMALI_KMD_PROCESS proc)
{
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PUINT64 l2 = NULL;
    PUINT64 l1 = NULL;
    PUINT64 leaf0 = NULL;
    PULONG bitmapBuffer = NULL;
    ULONG pageCount = (ULONG)(WINMALI_CSF_RESERVED_GPU_VA_BYTES >> PAGE_SHIFT);
    ULONG bitmapBytes = ((pageCount + 31u) / 32u) * sizeof(ULONG);

    if (adapter == NULL || proc == NULL ||
        !adapter->CsfReservedPtValid ||
        adapter->CsfReservedL0PtVa == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    low.QuadPart = 0;
    high.QuadPart = (ULONG64)-1LL;
    l2 = (PUINT64)MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, low, high, low, MmNonCached);
    l1 = (PUINT64)MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, low, high, low, MmNonCached);
    leaf0 = (PUINT64)MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, low, high, low, MmNonCached);
    bitmapBuffer = (PULONG)ExAllocatePoolWithTag(
        NonPagedPoolNx, bitmapBytes, WINMALI_POOL_TAG);
    if (l2 == NULL || l1 == NULL || leaf0 == NULL || bitmapBuffer == NULL) {
        if (bitmapBuffer != NULL) {
            ExFreePoolWithTag(bitmapBuffer, WINMALI_POOL_TAG);
        }
        if (leaf0 != NULL) MmFreeContiguousMemory(leaf0);
        if (l1 != NULL) MmFreeContiguousMemory(l1);
        if (l2 != NULL) MmFreeContiguousMemory(l2);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(l2, PAGE_SIZE);
    RtlZeroMemory(l1, PAGE_SIZE);
    /* The shared adapter leaf contains only the ring, shader trampoline and
       sync mappings in the first 2 MiB.  Copy descriptors, not ownership. */
    RtlCopyMemory(leaf0, adapter->CsfReservedL0PtVa, PAGE_SIZE);

    proc->ReservedL2PtVa = l2;
    proc->ReservedL2PtPa = MmGetPhysicalAddress(l2);
    proc->ReservedL1PtVa = l1;
    proc->ReservedL1PtPa = MmGetPhysicalAddress(l1);
    proc->ReservedLeafPtVa[0] = leaf0;
    proc->ReservedLeafPtPa[0] = MmGetPhysicalAddress(leaf0);
    proc->ReservedGpuVaBitmapBuffer = bitmapBuffer;
    proc->ReservedGpuVaPageCount = pageCount;
    RtlInitializeBitMap(&proc->ReservedGpuVaBitmap, bitmapBuffer, pageCount);
    RtlClearAllBits(&proc->ReservedGpuVaBitmap);
    /* Leaf zero is the immutable shared CSF portion, not allocatable heap VA. */
    RtlSetBits(&proc->ReservedGpuVaBitmap, 0,
               (ULONG)(WINMALI_CSF_SHARED_GPU_VA_BYTES >> PAGE_SHIFT));

    l2[0] = ((UINT64)proc->ReservedL1PtPa.QuadPart &
             WINMALI_LPAE_PA_MASK) | 0x3ull;
    l1[0] = ((UINT64)proc->ReservedLeafPtPa[0].QuadPart &
             WINMALI_LPAE_PA_MASK) | 0x3ull;
    KeMemoryBarrier();
    proc->ReservedPtValid = TRUE;

    WINMALI_TRACE(
        "ProcessReservedPT: proc=%p L2=%p/0x%llx L1=%p/0x%llx leaf0=%p/0x%llx range=0x%llx-0x%llx",
        proc, l2, (ULONGLONG)proc->ReservedL2PtPa.QuadPart,
        l1, (ULONGLONG)proc->ReservedL1PtPa.QuadPart,
        leaf0, (ULONGLONG)proc->ReservedLeafPtPa[0].QuadPart,
        (ULONGLONG)WINMALI_CSF_RESERVED_GPU_VA_BASE,
        (ULONGLONG)(WINMALI_CSF_RESERVED_GPU_VA_BASE +
                    WINMALI_CSF_RESERVED_GPU_VA_BYTES - 1ull));
    return STATUS_SUCCESS;
}

static PWINMALI_KMD_PROCESS
WinMaliKmdProcessFromOwnerDevice_(_In_opt_ PVOID ownerDevice)
{
    PWINMALI_KMD_DEVICE dev = (PWINMALI_KMD_DEVICE)ownerDevice;
    PWINMALI_KMD_PROCESS proc;

    if (dev == NULL || dev->Magic != WINMALI_KMD_DEVICE_MAGIC) {
        return NULL;
    }
    proc = dev->OwnerProcess;
    if (proc == NULL || proc->Magic != WINMALI_KMD_PROCESS_MAGIC ||
        !proc->ReservedPtValid || !proc->CsfGpuVaReserved) {
        return NULL;
    }
    return proc;
}

NTSTATUS
WinMaliKmdAllocateProcessReservedGpuVa(
    _In_opt_ PVOID ownerDevice,
    _In_ UINT64 size,
    _In_ UINT64 alignment,
    _Out_ UINT64* gpuVa)
{
    PWINMALI_KMD_PROCESS proc;
    ULONG pages;
    ULONG alignmentPages;
    ULONG candidate;
    ULONG privateStart;

    if (gpuVa == NULL || size == 0 ||
        (size & (PAGE_SIZE - 1ull)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    *gpuVa = 0;
    proc = WinMaliKmdProcessFromOwnerDevice_(ownerDevice);
    if (proc == NULL) {
        return STATUS_NOT_SUPPORTED;
    }
    if (alignment < PAGE_SIZE) alignment = PAGE_SIZE;
    if ((alignment & (alignment - 1ull)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (size > WINMALI_CSF_RESERVED_GPU_VA_BYTES ||
        alignment > WINMALI_CSF_RESERVED_GPU_VA_BYTES) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pages = (ULONG)(size >> PAGE_SHIFT);
    alignmentPages = (ULONG)(alignment >> PAGE_SHIFT);
    privateStart = (ULONG)(WINMALI_CSF_SHARED_GPU_VA_BYTES >> PAGE_SHIFT);

    ExAcquireFastMutex(&proc->ReservedPtLock);
    candidate = (privateStart + alignmentPages - 1u) & ~(alignmentPages - 1u);
    while (candidate <= proc->ReservedGpuVaPageCount &&
           pages <= proc->ReservedGpuVaPageCount - candidate) {
        if (RtlAreBitsClear(&proc->ReservedGpuVaBitmap, candidate, pages)) {
            RtlSetBits(&proc->ReservedGpuVaBitmap, candidate, pages);
            ExReleaseFastMutex(&proc->ReservedPtLock);
            *gpuVa = proc->CsfReservedGpuVa +
                     ((UINT64)candidate << PAGE_SHIFT);
            return STATUS_SUCCESS;
        }
        if (candidate > MAXULONG - alignmentPages) break;
        candidate += alignmentPages;
    }
    ExReleaseFastMutex(&proc->ReservedPtLock);
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS
WinMaliKmdMapProcessReservedGpuVa(
    _Inout_ PWINMALI_ADAPTER adapter,
    _In_opt_ PVOID ownerDevice,
    _In_ UINT64 gpuVa,
    _In_reads_(pageCount) const PFN_NUMBER* pfns,
    _In_ ULONG pageCount,
    _In_ UINT64 attrs)
{
    PWINMALI_KMD_PROCESS proc;
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    ULONG mapped = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (adapter == NULL || pfns == NULL || pageCount == 0 ||
        (gpuVa & (PAGE_SIZE - 1ull)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    proc = WinMaliKmdProcessFromOwnerDevice_(ownerDevice);
    if (proc == NULL) {
        return STATUS_NOT_SUPPORTED;
    }
    if (gpuVa < proc->CsfReservedGpuVa + WINMALI_CSF_SHARED_GPU_VA_BYTES ||
        gpuVa + ((UINT64)pageCount << PAGE_SHIFT) < gpuVa ||
        gpuVa + ((UINT64)pageCount << PAGE_SHIFT) >
            proc->CsfReservedGpuVa + WINMALI_CSF_RESERVED_GPU_VA_BYTES) {
        return STATUS_INVALID_ADDRESS;
    }

    low.QuadPart = 0;
    high.QuadPart = (ULONG64)-1LL;
    ExAcquireFastMutex(&proc->ReservedPtLock);
    if (!RtlAreBitsSet(
            &proc->ReservedGpuVaBitmap,
            (ULONG)((gpuVa - proc->CsfReservedGpuVa) >> PAGE_SHIFT),
            pageCount)) {
        ExReleaseFastMutex(&proc->ReservedPtLock);
        return STATUS_INVALID_ADDRESS;
    }
    for (mapped = 0; mapped < pageCount; ++mapped) {
        UINT64 va = gpuVa + ((UINT64)mapped << PAGE_SHIFT);
        UINT leafIndex = (UINT)((va - proc->CsfReservedGpuVa) >> 21);
        UINT pteIndex = (UINT)((va >> PAGE_SHIFT) & 0x1ffull);
        PUINT64 leaf;

        if (leafIndex >= RTL_NUMBER_OF(proc->ReservedLeafPtVa)) {
            status = STATUS_INVALID_ADDRESS;
            break;
        }
        if (proc->ReservedLeafPtVa[leafIndex] == NULL) {
            PVOID page = MmAllocateContiguousMemorySpecifyCache(
                PAGE_SIZE, low, high, low, MmNonCached);
            if (page == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            RtlZeroMemory(page, PAGE_SIZE);
            proc->ReservedLeafPtVa[leafIndex] = page;
            proc->ReservedLeafPtPa[leafIndex] = MmGetPhysicalAddress(page);
            ((PUINT64)proc->ReservedL1PtVa)[leafIndex] =
                ((UINT64)proc->ReservedLeafPtPa[leafIndex].QuadPart &
                 WINMALI_LPAE_PA_MASK) | 0x3ull;
        }
        leaf = (PUINT64)proc->ReservedLeafPtVa[leafIndex];
        if (leaf[pteIndex] != 0) {
            status = STATUS_CONFLICTING_ADDRESSES;
            break;
        }
        leaf[pteIndex] = (((UINT64)pfns[mapped] << PAGE_SHIFT) &
                          WINMALI_LPAE_PA_MASK) | attrs;
    }

    if (!NT_SUCCESS(status)) {
        while (mapped != 0) {
            UINT64 va;
            UINT leafIndex;
            UINT pteIndex;
            --mapped;
            va = gpuVa + ((UINT64)mapped << PAGE_SHIFT);
            leafIndex = (UINT)((va - proc->CsfReservedGpuVa) >> 21);
            pteIndex = (UINT)((va >> PAGE_SHIFT) & 0x1ffull);
            ((PUINT64)proc->ReservedLeafPtVa[leafIndex])[pteIndex] = 0;
        }
    }
    KeMemoryBarrier();
    ExReleaseFastMutex(&proc->ReservedPtLock);

    if (NT_SUCCESS(status)) {
        (VOID)WinMaliMmuFlushUserAsSlots(adapter);
        WINMALI_TRACE(
            "ProcessReservedMap: proc=%p va=0x%llx pages=%u firstPa=0x%llx attrs=0x%llx",
            proc, (ULONGLONG)gpuVa, pageCount,
            (ULONGLONG)((UINT64)pfns[0] << PAGE_SHIFT),
            (ULONGLONG)attrs);
    }
    return status;
}

NTSTATUS
WinMaliKmdUnmapProcessReservedGpuVa(
    _Inout_ PWINMALI_ADAPTER adapter,
    _In_opt_ PVOID ownerDevice,
    _In_ UINT64 gpuVa,
    _In_ ULONG pageCount)
{
    PWINMALI_KMD_PROCESS proc;
    ULONG i;

    if (adapter == NULL || pageCount == 0 ||
        (gpuVa & (PAGE_SIZE - 1ull)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    proc = WinMaliKmdProcessFromOwnerDevice_(ownerDevice);
    if (proc == NULL) return STATUS_NOT_SUPPORTED;
    if (gpuVa < proc->CsfReservedGpuVa + WINMALI_CSF_SHARED_GPU_VA_BYTES ||
        gpuVa + ((UINT64)pageCount << PAGE_SHIFT) < gpuVa ||
        gpuVa + ((UINT64)pageCount << PAGE_SHIFT) >
            proc->CsfReservedGpuVa + WINMALI_CSF_RESERVED_GPU_VA_BYTES) {
        return STATUS_INVALID_ADDRESS;
    }

    ExAcquireFastMutex(&proc->ReservedPtLock);
    for (i = 0; i < pageCount; ++i) {
        UINT64 va = gpuVa + ((UINT64)i << PAGE_SHIFT);
        UINT leafIndex = (UINT)((va - proc->CsfReservedGpuVa) >> 21);
        UINT pteIndex = (UINT)((va >> PAGE_SHIFT) & 0x1ffull);
        if (leafIndex < RTL_NUMBER_OF(proc->ReservedLeafPtVa) &&
            proc->ReservedLeafPtVa[leafIndex] != NULL) {
            ((PUINT64)proc->ReservedLeafPtVa[leafIndex])[pteIndex] = 0;
        }
    }
    RtlClearBits(
        &proc->ReservedGpuVaBitmap,
        (ULONG)((gpuVa - proc->CsfReservedGpuVa) >> PAGE_SHIFT),
        pageCount);
    KeMemoryBarrier();
    ExReleaseFastMutex(&proc->ReservedPtLock);
    (VOID)WinMaliMmuFlushUserAsSlots(adapter);
    return STATUS_SUCCESS;
}

/* Per-context state. SetRootPageTable records the VidMm-owned root here but
   does not consume a hardware AS. The scheduler leases an AS only around
   actual CSF execution and parks the CSG before releasing it. */
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
    FAST_MUTEX          ResidencyLock;
    UINT64              RootPtPa;
    ULONG               AsSlot;
    LONG                AsLeaseCount;
    LONG                RefCount;
    LONG                Destroying;
    KEVENT              RefZeroEvent;
} WINMALI_KMD_CONTEXT, *PWINMALI_KMD_CONTEXT;

static VOID
WinMaliContextPut_(_In_opt_ PWINMALI_KMD_CONTEXT ctx)
{
    if (ctx != NULL) {
        LONG refs = InterlockedDecrement(&ctx->RefCount);
        if (refs == 0) {
            KeSetEvent(&ctx->RefZeroEvent, IO_NO_INCREMENT, FALSE);
        } else if (refs < 0) {
            WINMALI_ERROR("ContextPut: refcount underflow ctx=%p", ctx);
        }
    }
}

static BOOLEAN
WinMaliContextGet_(_In_opt_ PWINMALI_KMD_CONTEXT ctx)
{
    if (ctx == NULL || ctx->Magic != WINMALI_KMD_CONTEXT_MAGIC ||
        InterlockedCompareExchange(&ctx->Destroying, 0, 0) != 0) {
        return FALSE;
    }
    InterlockedIncrement(&ctx->RefCount);
    if (InterlockedCompareExchange(&ctx->Destroying, 0, 0) != 0) {
        WinMaliContextPut_(ctx);
        return FALSE;
    }
    return TRUE;
}

static NTSTATUS
WinMaliContextAcquireAs_(_Inout_ PWINMALI_KMD_CONTEXT ctx,
                         _Out_ PULONG outAs)
{
    NTSTATUS status;

    *outAs = WINMALI_AS_SLOT_MAX;
    ExAcquireFastMutex(&ctx->ResidencyLock);
    if (ctx->RootPtPa == 0) {
        ExReleaseFastMutex(&ctx->ResidencyLock);
        return STATUS_DEVICE_NOT_READY;
    }
    if (ctx->AsLeaseCount > 0 && ctx->AsSlot < WINMALI_AS_SLOT_MAX) {
        ++ctx->AsLeaseCount;
        *outAs = ctx->AsSlot;
        ExReleaseFastMutex(&ctx->ResidencyLock);
        return STATUS_SUCCESS;
    }

    status = WinMaliMmuBindContextRootPt(
        ctx->Adapter, (HANDLE)ctx, ctx->RootPtPa, &ctx->AsSlot);
    if (NT_SUCCESS(status)) {
        ctx->AsLeaseCount = 1;
        *outAs = ctx->AsSlot;
        WINMALI_ROUTINE("CONTEXT RESIDENCY: acquire ctx=%p root=0x%llx AS%u inUse=0x%x",
                     ctx, (ULONGLONG)ctx->RootPtPa, ctx->AsSlot,
                     ctx->Adapter->AsSlotInUseMask);
    }
    ExReleaseFastMutex(&ctx->ResidencyLock);
    return status;
}

static VOID
WinMaliContextReleaseAs_(_Inout_ PWINMALI_KMD_CONTEXT ctx,
                         _In_ ULONG asSlot)
{
    ExAcquireFastMutex(&ctx->ResidencyLock);
    if (ctx->AsLeaseCount <= 0 || ctx->AsSlot != asSlot ||
        asSlot >= WINMALI_AS_SLOT_MAX) {
        WINMALI_WARN("ContextReleaseAs: mismatched lease ctx=%p requested=AS%u current=AS%u leases=%ld",
                     ctx, asSlot, ctx->AsSlot, ctx->AsLeaseCount);
        ExReleaseFastMutex(&ctx->ResidencyLock);
        return;
    }
    --ctx->AsLeaseCount;
    if (ctx->AsLeaseCount == 0) {
        (VOID)WinMaliMmuUnbindContext(ctx->Adapter, (HANDLE)ctx);
        ctx->AsSlot = WINMALI_AS_SLOT_MAX;
        WINMALI_ROUTINE("CONTEXT RESIDENCY: release ctx=%p AS%u inUse=0x%x",
                     ctx, asSlot, ctx->Adapter->AsSlotInUseMask);
    }
    ExReleaseFastMutex(&ctx->ResidencyLock);
}

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
    if (physicalAdapterIndex != 0 || nodeOrdinal > 1) {
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
    DXGKARGCB_RESERVEGPUVIRTUALADDRESSRANGE reserve;
    NTSTATUS             status;

    if (pCreateProcess == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (!adapter->CsfReservedPtValid ||
        adapter->CsfReservedL2PtVa == NULL ||
        adapter->CsfReservedL2PtPhys.QuadPart == 0) {
        WINMALI_ERROR("CreateProcess: CSF reserved page-table subtree is not ready");
        return STATUS_DEVICE_NOT_READY;
    }

    proc = (PWINMALI_KMD_PROCESS)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(*proc), WINMALI_POOL_TAG);
    if (proc == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(proc, sizeof(*proc));
    ExInitializeFastMutex(&proc->ReservedPtLock);
    proc->Magic         = WINMALI_KMD_PROCESS_MAGIC;
    proc->Adapter       = adapter;
    proc->hDxgkProcess  = pCreateProcess->hDxgkProcess;
    proc->Flags         = pCreateProcess->Flags.Value;
    if (pCreateProcess->NumPasid > 0 && pCreateProcess->pPasid != NULL) {
        proc->Pasid = pCreateProcess->pPasid[0];
    }

    status = WinMaliKmdInitProcessReservedPt_(adapter, proc);
    if (!NT_SUCCESS(status)) {
        WINMALI_ERROR("CreateProcess: process reserved PT init failed 0x%08x",
                      status);
        ExFreePoolWithTag(proc, WINMALI_POOL_TAG);
        return status;
    }

    /* Reserve one complete hardware L2-entry coverage (1 GiB). The shared
       CSF mappings occupy its first 2 MiB and process-private KMD mappings
       (currently tiler heap context/chunks) occupy the remainder.
       Reservation is legal only while DxgkDdiCreateProcess is executing.
       AllowUserModeMapping stays clear: UMD allocations must never replace
       the ring/sync/trampoline mappings installed by the KMD. */
    if (adapter->DxgkInterface.DxgkCbReserveGpuVirtualAddressRange == NULL) {
        WINMALI_ERROR("CreateProcess: DxgkCbReserveGpuVirtualAddressRange unavailable");
        WinMaliKmdFreeProcessReservedPt_(proc);
        ExFreePoolWithTag(proc, WINMALI_POOL_TAG);
        return STATUS_NOT_SUPPORTED;
    }
    RtlZeroMemory(&reserve, sizeof(reserve));
    reserve.hDxgkProcess = pCreateProcess->hDxgkProcess;
    reserve.SizeInBytes = WINMALI_CSF_RESERVED_GPU_VA_BYTES;
    reserve.Alignment = (UINT)WINMALI_CSF_RESERVED_GPU_VA_BYTES;
    reserve.BaseAddress = WINMALI_CSF_RESERVED_GPU_VA_BASE;
    reserve.Flags = 0;
    status = adapter->DxgkInterface.DxgkCbReserveGpuVirtualAddressRange(
        adapter->DxgkHandle, &reserve);
    if (!NT_SUCCESS(status)) {
        WINMALI_ERROR(
            "CreateProcess: reserve CSF GPUVA base=0x%llx bytes=0x%llx failed 0x%08x",
            (ULONGLONG)WINMALI_CSF_RESERVED_GPU_VA_BASE,
            (ULONGLONG)WINMALI_CSF_RESERVED_GPU_VA_BYTES,
            status);
        WinMaliKmdFreeProcessReservedPt_(proc);
        ExFreePoolWithTag(proc, WINMALI_POOL_TAG);
        return status;
    }
    if (reserve.StartVirtualAddress != WINMALI_CSF_RESERVED_GPU_VA_BASE) {
        WINMALI_ERROR(
            "CreateProcess: CSF reservation returned unexpected base=0x%llx requested=0x%llx",
            (ULONGLONG)reserve.StartVirtualAddress,
            (ULONGLONG)WINMALI_CSF_RESERVED_GPU_VA_BASE);
        WinMaliKmdFreeProcessReservedPt_(proc);
        ExFreePoolWithTag(proc, WINMALI_POOL_TAG);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    proc->CsfReservedGpuVa = reserve.StartVirtualAddress;
    proc->CsfGpuVaReserved = TRUE;

    pCreateProcess->hKmdProcess = (HANDLE)proc;
    /* Process lifetime is a low-volume diagnostic breadcrumb. Keep it in
       Focused builds so UMD crashes/restarts can be correlated without the
       routine paging/allocation chatter enabled by Verbose. */
    WINMALI_DIAG("CreateProcess: proc=%p hDxgk=%p flags=0x%x pasid=%u CSF-reserved=0x%llx/0x%llx",
                 proc, proc->hDxgkProcess, proc->Flags, proc->Pasid,
                 (ULONGLONG)proc->CsfReservedGpuVa,
                 (ULONGLONG)WINMALI_CSF_RESERVED_GPU_VA_BYTES);
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
    WINMALI_DIAG("DestroyProcess: proc=%p", proc);
    WinMaliKmdFreeProcessReservedPt_(proc);
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

    /* DWM must be able to open this render-only adapter to consume and
       composite application surfaces; this does not give WinMali physical
       display ownership. Keep a registry-controlled recovery escape hatch:
       Services\WinMaliKmd\Parameters\AllowDwm = 0 + reboot. */
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

    if (dev == NULL || dev->Magic != WINMALI_KMD_DEVICE_MAGIC) {
        return STATUS_INVALID_HANDLE;
    }
    if (dev->ContextCount != 0) {
        WINMALI_WARN("DestroyDevice: dev=%p leaks %ld contexts",
                     dev, dev->ContextCount);
    }
    /* Run down every escape-created resource this device still owns. VM page
       tables are non-resident until execution now, but abandoned groups, BOs,
       sync objects and page tables still require deterministic cleanup.
       Order: groups reference VMs, BOs may be VM-bound, so
       heaps -> groups -> remaining BOs -> syncobjs -> VMs.  Keep the process
       reference counted by this device until heap teardown has removed its
       process-private reserved mappings. */
    {
        PWINMALI_ADAPTER adapter = dev->Adapter;
        if (adapter != NULL) {
            ULONG heaps  = WinMaliTilerHeapRundownOwner(adapter, dev);
            ULONG groups = WinMaliGroupRundownOwner(adapter, dev);
            ULONG bos    = WinMaliBoRundownOwner(adapter, dev);
            ULONG syncs  = WinMaliSyncObjRundownOwner(adapter, dev);
            ULONG vms    = WinMaliVmRundownOwner(adapter, dev);
            if ((heaps | groups | bos | syncs | vms) != 0) {
                WINMALI_TRACE("DestroyDevice rundown: dev=%p heaps=%u groups=%u bos=%u "
                              "syncs=%u vms=%u", dev, heaps, groups, bos, syncs, vms);
            }
        }
    }

    if (dev->OwnerProcess != NULL) {
        InterlockedDecrement(&dev->OwnerProcess->DeviceCount);
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

    WINMALI_TRACE("CreateContextDiag: ENTER hDevice=%p args=%p rtContext=%p node=%u engineAffinity=0x%x flags=0x%x",
                  hDevice, pCreateContext,
                  pCreateContext != NULL ? pCreateContext->hContext : NULL,
                  pCreateContext != NULL ? pCreateContext->NodeOrdinal : 0,
                  pCreateContext != NULL ? pCreateContext->EngineAffinity : 0,
                  pCreateContext != NULL ? pCreateContext->Flags.Value : 0);

    if (pCreateContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    dev = (PWINMALI_KMD_DEVICE)hDevice;
    if (dev == NULL || dev->Magic != WINMALI_KMD_DEVICE_MAGIC) {
        return STATUS_INVALID_HANDLE;
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
    ExInitializeFastMutex(&ctx->ResidencyLock);
    ctx->AsSlot         = WINMALI_AS_SLOT_MAX;
    ctx->RefCount       = 1;
    KeInitializeEvent(&ctx->RefZeroEvent, NotificationEvent, FALSE);

    InterlockedIncrement(&dev->ContextCount);
    pCreateContext->hContext                            = (HANDLE)ctx;
    pCreateContext->ContextInfo.DmaBufferSize           = PAGE_SIZE;
    pCreateContext->ContextInfo.DmaBufferSegmentSet     = (1u << (WINMALI_SEGMENT_ID_SYSMEM - 1));
    /* PhysicalAdapterCaps advertises node 1 as the adapter paging node.
       Associate every virtual render context with that node so VidMm routes
       its GPUVA page-table work through the topology we reported. */
    if (!pCreateContext->Flags.SystemContext &&
        pCreateContext->Flags.VirtualAddressing) {
        pCreateContext->ContextInfo.PagingCompanionNodeId = 1;
    }
    /* Non-system contexts get DMA private data big enough for the present
       Blt descriptor (DxgkDdiPresent stashes a WINMALI_BLT_DESC there and
       SubmitCommand executes it). With 0 here, Present's size check always
       failed and presents silently became no-ops. System/paging contexts
       keep 0 so the paging path can never look like a blt. */
    pCreateContext->ContextInfo.DmaBufferPrivateDataSize =
        pCreateContext->Flags.SystemContext ? 0
                                            : (UINT)max(sizeof(WINMALI_BLT_DESC),
                                                        sizeof(WINMALI_CS_SUBMIT_DESC));
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
    WINMALI_TRACE("CreateContextDiag: EXIT status=0x%08x drvContext=%p rtContext=%p dma=%u private=%u allocList=%u patchList=%u caps=0x%x pagingCompanion=%u",
                  STATUS_SUCCESS, pCreateContext->hContext, ctx->hRtContext,
                  pCreateContext->ContextInfo.DmaBufferSize,
                  pCreateContext->ContextInfo.DmaBufferPrivateDataSize,
                  pCreateContext->ContextInfo.AllocationListSize,
                  pCreateContext->ContextInfo.PatchLocationListSize,
                  pCreateContext->ContextInfo.Caps.Value,
                  pCreateContext->ContextInfo.PagingCompanionNodeId);
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
    InterlockedExchange(&ctx->Destroying, 1);
    if (InterlockedDecrement(&ctx->RefCount) != 0) {
        KeWaitForSingleObject(&ctx->RefZeroEvent, Executive, KernelMode,
                              FALSE, NULL);
    }
    /* Normally no lease survives a completed worker packet. If firmware did
       not acknowledge the safety park, reset/park before tearing down the
       final binding rather than disabling an AS still named by CSG0. */
    if (ctx->Adapter != NULL && ctx->AsSlot < WINMALI_AS_SLOT_MAX) {
        if (ctx->AsLeaseCount > 0) {
            (VOID)WinMaliCsfResetRenderEngine(ctx->Adapter);
        }
        (VOID)WinMaliMmuUnbindContext(ctx->Adapter, (HANDLE)ctx);
        ctx->AsSlot = WINMALI_AS_SLOT_MAX;
        ctx->AsLeaseCount = 0;
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
/* DxgkDdiRender - translate one validated WinMali UMD command into a dxgk  */
/* DMA buffer plus the private descriptor consumed by SubmitCommandVirtual. */
/* ------------------------------------------------------------------------ */

typedef struct _WINMALI_RENDER_DMA_TOKEN {
    ULONG Magic;
    ULONG StreamSize;
} WINMALI_RENDER_DMA_TOKEN;

_Function_class_(DXGKDDI_RENDER)
NTSTATUS
APIENTRY
WinMaliKmdRender(
    IN_CONST_HANDLE       hContext,
    INOUT_PDXGKARG_RENDER pRender)
{
    PWINMALI_KMD_CONTEXT ctx = (PWINMALI_KMD_CONTEXT)hContext;
    WINMALI_CS_SUBMIT_DESC cs;
    WINMALI_RENDER_DMA_TOKEN token;
    ULONG i;

    WINMALI_TRACE("Render: ctx=%p args=%p", hContext, pRender);

    if (ctx == NULL || ctx->Magic != WINMALI_KMD_CONTEXT_MAGIC) {
        WINMALI_ERROR("Render: invalid context=%p", hContext);
        return STATUS_INVALID_HANDLE;
    }
    if (pRender == NULL) {
        WINMALI_ERROR("Render: NULL arguments");
        return STATUS_INVALID_PARAMETER;
    }
    if (ctx->NodeOrdinal != 0) {
        WINMALI_ERROR("Render: context=%p is not a node-0 render context node=%u flags=0x%x",
                      ctx, ctx->NodeOrdinal, ctx->Flags);
        return STATUS_INVALID_HANDLE;
    }
    if (pRender->MultipassOffset != 0) {
        WINMALI_ERROR("Render: unexpected multipass offset=%u", pRender->MultipassOffset);
        return STATUS_INVALID_PARAMETER;
    }
    if (pRender->pCommand == NULL ||
        pRender->CommandLength != sizeof(WINMALI_CS_SUBMIT_DESC)) {
        WINMALI_ERROR("Render: invalid command ptr=%p length=%u expected=%Iu",
                      pRender->pCommand, pRender->CommandLength,
                      sizeof(WINMALI_CS_SUBMIT_DESC));
        return STATUS_INVALID_USER_BUFFER;
    }

    /* pCommand is an untrusted UMD buffer. Copy it exactly once into kernel
       storage under SEH, then validate only the owned copy. */
    __try {
        ProbeForRead((PVOID)pRender->pCommand, sizeof(cs), 1);
        RtlCopyMemory(&cs, pRender->pCommand, sizeof(cs));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        NTSTATUS status = GetExceptionCode();
        WINMALI_ERROR("Render: command-buffer probe/copy failed status=0x%08x",
                      status);
        return status;
    }

    if (cs.Magic != WINMALI_CS_SUBMIT_DESC_MAGIC ||
        cs.StreamGpuVa == 0 || cs.StreamSize == 0 || cs.Reserved != 0 ||
        (cs.Flags & ~WINMALI_CS_SUBMIT_FLAG_DXGK_ADDR) != 0 ||
        cs.SyncCount > WINMALI_CS_SUBMIT_MAX_SYNCS) {
        WINMALI_ERROR("Render: invalid desc magic=0x%08x vm=%u va=0x%llx size=%u "
                      "flags=0x%x syncs=%u reserved=0x%x",
                      cs.Magic, cs.VmId, (unsigned long long)cs.StreamGpuVa,
                      cs.StreamSize, cs.Flags, cs.SyncCount, cs.Reserved);
        return STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < cs.SyncCount; ++i) {
        if (cs.SyncOps[i].Handle == 0 ||
            (cs.SyncOps[i].Flags & ~WINMALI_SYNC_OP_SIGNAL) != 0) {
            WINMALI_ERROR("Render: invalid sync[%u] handle=%u flags=0x%x point=%llu",
                          i, cs.SyncOps[i].Handle, cs.SyncOps[i].Flags,
                          (unsigned long long)cs.SyncOps[i].TimelineValue);
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (pRender->pDmaBufferPrivateData == NULL ||
        pRender->DmaBufferPrivateDataSize < sizeof(cs)) {
        WINMALI_ERROR("Render: private DMA buffer too small ptr=%p size=%u expected=%Iu",
                      pRender->pDmaBufferPrivateData,
                      pRender->DmaBufferPrivateDataSize, sizeof(cs));
        return STATUS_INVALID_PARAMETER;
    }
    if (pRender->pDmaBuffer == NULL || pRender->DmaSize < sizeof(token)) {
        WINMALI_WARN("Render: DMA buffer depleted ptr=%p remaining=%u needed=%Iu",
                     pRender->pDmaBuffer, pRender->DmaSize, sizeof(token));
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    token.Magic      = WINMALI_CS_SUBMIT_DESC_MAGIC;
    token.StreamSize = cs.StreamSize;
    RtlCopyMemory(pRender->pDmaBuffer, &token, sizeof(token));
    RtlCopyMemory(pRender->pDmaBufferPrivateData, &cs, sizeof(cs));

    /* DXGKARG_RENDER requires pDmaBuffer to point at the first unused byte.
       Private data stays at its allocation base for SubmitCommandVirtual. */
    pRender->pDmaBuffer = (PVOID)((PUCHAR)pRender->pDmaBuffer + sizeof(token));
    pRender->MultipassOffset = pRender->CommandLength;

    WINMALI_TRACE("Render: accepted ctx=%p vm=%u va=0x%llx size=%u flags=0x%x "
                  "syncs=%u dmaBytes=%Iu privateBytes=%Iu",
                  ctx, cs.VmId, (unsigned long long)cs.StreamGpuVa,
                  cs.StreamSize, cs.Flags, cs.SyncCount,
                  sizeof(token), sizeof(cs));
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiSetRootPageTable / GetRootPageTableSize                            */
/* ------------------------------------------------------------------------ */

/* DXGKDDI_SETROOTPAGETABLE returns VOID and takes a SINGLE context per
   call. Address is {SegmentId, SegmentOffset}: implicit system-memory
   addresses (segment 0) already carry a physical address, while explicit
   segment addresses need translation through the DmaSegment base. */
_Function_class_(DXGKDDI_SETROOTPAGETABLE)
VOID
APIENTRY
WinMaliKmdSetRootPageTable(
    IN_CONST_HANDLE                       hAdapter,
    IN_CONST_PDXGKARG_SETROOTPAGETABLE    pArgs)
{
    PWINMALI_ADAPTER adapter;
    PWINMALI_KMD_CONTEXT ctx;
    UINT64           rootPtPa = 0;

    WINMALI_TRACE("SetRootPageTableDiag: ENTER hAdapter=%p args=%p ctx=%p seg=%u off=0x%llx entries=%u",
                  hAdapter, pArgs,
                  pArgs != NULL ? pArgs->hContext : NULL,
                  pArgs != NULL ? pArgs->Address.SegmentId : 0,
                  pArgs != NULL ? (ULONGLONG)pArgs->Address.SegmentOffset : 0,
                  pArgs != NULL ? pArgs->NumEntries : 0);

    if (pArgs == NULL) {
        WINMALI_WARN("SetRootPageTableDiag: EXIT args=NULL");
        return;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        WINMALI_WARN("SetRootPageTableDiag: EXIT invalid adapter=%p", hAdapter);
        return;
    }
    ctx = (PWINMALI_KMD_CONTEXT)pArgs->hContext;
    if (ctx == NULL || ctx->Magic != WINMALI_KMD_CONTEXT_MAGIC ||
        ctx->Adapter != adapter ||
        InterlockedCompareExchange(&ctx->Destroying, 0, 0) != 0) {
        WINMALI_WARN("SetRootPageTableDiag: EXIT invalid context=%p",
                     pArgs->hContext);
        return;
    }

    if (pArgs->Address.SegmentId == 0) {
        /* WDDM's implicit system-memory segment is SegmentId 0.  In that
           case SegmentOffset is already the GPU-visible physical address of
           the root page table.  Only the all-zero address is an unbind. */
        rootPtPa = (UINT64)pArgs->Address.SegmentOffset;
    } else if (pArgs->Address.SegmentId == WINMALI_SEGMENT_ID_SYSMEM) {
        UINT64 segmentOffset = (UINT64)pArgs->Address.SegmentOffset;

        /* Explicit segment addresses are relative to the contiguous backing
           store reported by QUERYSEGMENT.  Validate the complete root table,
           not just its first byte, before translating it to a host PA. */
        if (adapter->DmaSegmentVa == NULL ||
            segmentOffset > adapter->DmaSegmentBytes ||
            PAGE_SIZE > adapter->DmaSegmentBytes - segmentOffset) {
            WINMALI_WARN(
                "SetRootPageTableDiag: invalid segment-1 address off=0x%llx segmentBytes=0x%Ix",
                (ULONGLONG)segmentOffset,
                adapter->DmaSegmentBytes);
            return;
        }
        rootPtPa = (UINT64)adapter->DmaSegmentPhys.QuadPart + segmentOffset;
    } else {
        /* This DDI returns VOID, so an unknown segment cannot be reported to
           dxgkrnl.  Leave any existing context binding intact rather than
           silently converting a malformed address into an unbind request. */
        WINMALI_WARN(
            "SetRootPageTableDiag: unsupported segment=%u off=0x%llx ctx=%p",
            pArgs->Address.SegmentId,
            (ULONGLONG)pArgs->Address.SegmentOffset,
            pArgs->hContext);
        return;
    }

    if (rootPtPa != 0 && (rootPtPa & (PAGE_SIZE - 1ull)) != 0) {
        WINMALI_WARN("SetRootPageTableDiag: unaligned root pa=0x%llx ctx=%p",
                     (ULONGLONG)rootPtPa, ctx);
        return;
    }

    ExAcquireFastMutex(&ctx->ResidencyLock);
    if (ctx->AsLeaseCount != 0) {
        WINMALI_WARN("SetRootPageTable: ctx=%p root update while resident AS%u leases=%ld ignored",
                     ctx, ctx->AsSlot, ctx->AsLeaseCount);
        ExReleaseFastMutex(&ctx->ResidencyLock);
        return;
    }
    ctx->RootPtPa = rootPtPa;
    ExReleaseFastMutex(&ctx->ResidencyLock);

    WINMALI_DIAG("CONTEXT RESIDENCY: root recorded ctx=%p seg=%u off=0x%llx pa=0x%llx entries=%u (nonresident)",
                  pArgs->hContext,
                  pArgs->Address.SegmentId,
                  (ULONGLONG)pArgs->Address.SegmentOffset,
                  (ULONGLONG)rootPtPa,
                  pArgs->NumEntries);
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
    WINMALI_TRACE("GetRootPageTableSizeDiag: ENTER hAdapter=%p args=%p requestedPtes=%u physicalAdapter=%u",
                  hAdapter, pArgs,
                  pArgs != NULL ? pArgs->NumberOfPte : 0,
                  pArgs != NULL ? pArgs->PhysicalAdapterIndex : 0);
    if (pArgs != NULL) {
        pArgs->NumberOfPte = 512;   /* 9-bit top-level index */
    }
    WINMALI_TRACE("GetRootPageTableSizeDiag: EXIT bytes=%Iu ptes=%u",
                  (SIZE_T)PAGE_SIZE,
                  pArgs != NULL ? pArgs->NumberOfPte : 0);
    return PAGE_SIZE;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiQueryCurrentFence                                                  */
/* ------------------------------------------------------------------------ */

static BOOLEAN WinMaliSchedNodeValid_(_In_ UINT node, _In_ UINT engine);

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
    if (!WinMaliSchedNodeValid_(pArgs->NodeOrdinal, pArgs->EngineOrdinal)) {
        return STATUS_INVALID_PARAMETER;
    }
    pArgs->CurrentFence = (UINT)InterlockedCompareExchange64(
        &adapter->NodeSched[pArgs->NodeOrdinal].CompletedFence, 0, 0);
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
    PWINMALI_KMD_OPEN_ALLOCATION src = d->Src;
    PWINMALI_KMD_OPEN_ALLOCATION dst = d->Dst;
    PUCHAR srcBase, dstBase;
    SIZE_T srcBytes, dstBytes;
    ULONG  srcPitch, dstPitch;
    LONG   width, height, r;
    const ULONG bpp = 4;   /* swapchain / GDI surfaces are 32bpp linear */

    if (src == NULL || dst == NULL ||
        src->Magic != WINMALI_KMD_OPEN_ALLOC_MAGIC ||
        dst->Magic != WINMALI_KMD_OPEN_ALLOC_MAGIC) {
        WINMALI_WARN("PresentBlt: bad allocation descriptors (src=%p dst=%p)", src, dst);
        return;
    }
    if (src->Global == NULL || dst->Global == NULL ||
        src->Global->ApertureMdl == NULL ||
        dst->Global->ApertureMdl == NULL) {
        WINMALI_WARN("PresentBlt: not resident (srcMdl=%p dstMdl=%p) - frame dropped",
                     src->Global ? src->Global->ApertureMdl : NULL,
                     dst->Global ? dst->Global->ApertureMdl : NULL);
        return;
    }

    srcPitch = src->Pitch;
    dstPitch = dst->Pitch;
    if (srcPitch == 0 || dstPitch == 0) {
        WINMALI_WARN("PresentBlt: zero pitch (src=%u dst=%u) - frame dropped",
                     srcPitch, dstPitch);
        return;
    }

    srcBase = (PUCHAR)MmGetSystemAddressForMdlSafe(src->Global->ApertureMdl,
                                                   NormalPagePriority | MdlMappingNoExecute);
    dstBase = (PUCHAR)MmGetSystemAddressForMdlSafe(dst->Global->ApertureMdl,
                                                   NormalPagePriority | MdlMappingNoExecute);
    if (srcBase == NULL || dstBase == NULL) {
        WINMALI_WARN("PresentBlt: MDL map failed (src=%p dst=%p) - frame dropped",
                     srcBase, dstBase);
        return;
    }
    srcBase += (SIZE_T)src->Global->ApertureMdlOffset << PAGE_SHIFT;
    dstBase += (SIZE_T)dst->Global->ApertureMdlOffset << PAGE_SHIFT;
    srcBytes = (SIZE_T)src->Global->AperturePageCount << PAGE_SHIFT;
    dstBytes = (SIZE_T)dst->Global->AperturePageCount << PAGE_SHIFT;

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
/* dedicated KMD worker thread runs the real CSF stream off the scheduler   */
/* thread and reports its terminal fence via the DPC. PreemptCommand is an   */
/* urgent request honored at the next DMA-buffer boundary.                   */
/* ------------------------------------------------------------------------ */

static BOOLEAN
WinMaliSchedNodeValid_(_In_ UINT node, _In_ UINT engine)
{
    return (node < WINMALI_NODE_COUNT && engine == 0u) ? TRUE : FALSE;
}

/* Queue exactly one DPC while work is pending. NotifyDpcPending remains set
   while the DPC is queued/running; the DPC clears it only after an empty
   observation and closes the producer/consumer lost-wakeup race. */
VOID
WinMaliSchedRequestDpc(_Inout_ PWINMALI_ADAPTER a)
{
    if (a->DxgkHandle != NULL && a->DxgkInterface.DxgkCbQueueDpc != NULL &&
        InterlockedCompareExchange(&a->NotifyDpcPending, 1, 0) == 0) {
        /* FALSE means a DPC is already queued/running.  That is harmless:
           NotifyDpcPending plus the consumer-side recheck closes the race.
           Do not log here because the hardware ISR also calls this helper. */
        (VOID)a->DxgkInterface.DxgkCbQueueDpc(a->DxgkHandle);
    }
}

/* Push a completed/preempted/timeout event without ever overwriting an event
   dxgkrnl has not consumed. The queue is deliberately much deeper than the
   advertised HwQueuePacketCap. */
static BOOLEAN
WinMaliSchedPushFence_(_Inout_ PWINMALI_ADAPTER a, _In_ UINT fence,
                       _In_ UINT node, _In_ UINT engine,
                       _In_ BOOLEAN isPreempt, _In_ UINT lastCompleted,
                       _In_ NTSTATUS faultStatus)
{
    KIRQL irql;
    ULONG next;
    KeAcquireSpinLock(&a->FenceQueueLock, &irql);
    next = (a->FenceQueueTail + 1) % WINMALI_FENCE_QUEUE_DEPTH;
    if (next == a->FenceQueueHead) {
        KeReleaseSpinLock(&a->FenceQueueLock, irql);
        WINMALI_ERROR("Sched: notify queue full; refusing to overwrite fence=%u node=%u",
                      fence, node);
        return FALSE;
    }
    a->FenceQueue[a->FenceQueueTail].SubmissionFenceId    = fence;
    a->FenceQueue[a->FenceQueueTail].NodeOrdinal          = node;
    a->FenceQueue[a->FenceQueueTail].EngineOrdinal        = engine;
    a->FenceQueue[a->FenceQueueTail].IsPreempt            = isPreempt;
    a->FenceQueue[a->FenceQueueTail].IsFault              =
        (!isPreempt && !NT_SUCCESS(faultStatus)) ? TRUE : FALSE;
    a->FenceQueue[a->FenceQueueTail].LastCompletedFenceId = lastCompleted;
    a->FenceQueue[a->FenceQueueTail].FaultStatus          = faultStatus;
    a->FenceQueueTail = next;
    KeReleaseSpinLock(&a->FenceQueueLock, irql);
    return TRUE;
}

/* Discard notifications for a node after dxgkrnl has quiesced/flushed that
   engine for reset. Preserve the FIFO order of events belonging to the other
   node; reporting a pre-reset completion or preemption after reset would mix
   two scheduler generations and can make dxgmms2 reject its fence ID. */
static UINT
WinMaliSchedDiscardNotifyNode_(_Inout_ PWINMALI_ADAPTER a, _In_ UINT node)
{
    KIRQL irql;
    ULONG read;
    ULONG write;
    UINT discarded = 0;

    KeAcquireSpinLock(&a->FenceQueueLock, &irql);
    write = a->FenceQueueHead;
    for (read = a->FenceQueueHead; read != a->FenceQueueTail;
         read = (read + 1) % WINMALI_FENCE_QUEUE_DEPTH) {
        if (a->FenceQueue[read].NodeOrdinal == node) {
            ++discarded;
            continue;
        }
        if (write != read) {
            a->FenceQueue[write] = a->FenceQueue[read];
        }
        write = (write + 1) % WINMALI_FENCE_QUEUE_DEPTH;
    }
    a->FenceQueueTail = write;
    KeReleaseSpinLock(&a->FenceQueueLock, irql);
    return discarded;
}

static VOID
WinMaliSchedQueueTimeout_(_Inout_ PWINMALI_ADAPTER a, _In_ UINT fence,
                          _In_ UINT node, _In_ UINT engine,
                          _In_ NTSTATUS status)
{
    if (!WinMaliSchedNodeValid_(node, engine)) {
        WINMALI_ERROR("Sched: cannot report timeout for invalid node=%u engine=%u",
                      node, engine);
        return;
    }
    InterlockedExchange64(&a->NodeSched[node].LastAbortedFence, (LONG64)fence);
    if (InterlockedCompareExchange(&a->NodeSched[node].TdrPending, 1, 0) != 0) {
        WINMALI_WARN("Sched: suppressing duplicate timeout fence=%u node=%u status=0x%08x",
                     fence, node, status);
        return;
    }
    if (!WinMaliSchedPushFence_(a, fence, node, engine, FALSE, 0, status)) {
        WINMALI_ERROR("Sched: timeout event could not be queued fence=%u node=%u",
                      fence, node);
        return;
    }
    WinMaliSchedRequestDpc(a);
}

/* Retire Mesa's private timelines for a terminal submission. A failed CS is
   terminal too: the WDDM engine-timeout notification starts recovery while
   the sync signal lets Mesa release resources instead of hanging forever. */
static VOID
WinMaliSchedFinishSubmit_(_Inout_ PWINMALI_ADAPTER a,
                          _In_ const WINMALI_CS_SUBMIT_DESC* cs,
                          _In_ UINT fence,
                          _In_ NTSTATUS status)
{
    ULONG i;
    if (cs == NULL) {
        return;
    }
    for (i = 0; i < cs->SyncCount; ++i) {
        if (cs->SyncOps[i].Flags & WINMALI_SYNC_OP_SIGNAL) {
            WINMALI_DIAG("Sched: finish sync ENTER fence=%u status=0x%08x index=%u handle=%u point=%llu flags=0x%x",
                         fence, status, i, cs->SyncOps[i].Handle,
                         (unsigned long long)cs->SyncOps[i].TimelineValue,
                         cs->SyncOps[i].Flags);
            int err = NT_SUCCESS(status)
                ? WinMaliSyncObjSignal(a, cs->SyncOps[i].Handle,
                                       cs->SyncOps[i].TimelineValue)
                : WinMaliSyncObjFail(a, cs->SyncOps[i].Handle,
                                     cs->SyncOps[i].TimelineValue);
            if (err != 0) {
                WINMALI_ERROR("Sched: finish sync failed err=%d status=0x%08x handle=%u point=%llu fence=%u",
                              err, status, cs->SyncOps[i].Handle,
                              (unsigned long long)cs->SyncOps[i].TimelineValue,
                              fence);
            } else {
                WINMALI_DIAG("Sched: finish sync EXIT fence=%u index=%u handle=%u point=%llu err=0",
                             fence, i, cs->SyncOps[i].Handle,
                             (unsigned long long)cs->SyncOps[i].TimelineValue);
            }
        }
    }
}

/* Enqueue a submit request for the worker. A FALSE result is a scheduler
   invariant failure and must enter recovery; it is never a fake completion. */
static BOOLEAN
WinMaliSchedEnqueue_(_Inout_ PWINMALI_ADAPTER a, _In_ UINT fence,
                      _In_ UINT node, _In_ UINT engine,
                      _In_opt_ const WINMALI_CS_SUBMIT_DESC* cs,
                      _In_ ULONG asSlot,
                      _In_opt_ PWINMALI_KMD_CONTEXT context)
{
    KIRQL   irql;
    ULONG   next;
    BOOLEAN queued = FALSE;

    /* Never enqueue into a queue that has no live consumer. */
    if (!WinMaliSchedNodeValid_(node, engine) ||
        a->SubmitWorkerThread == NULL ||
        InterlockedCompareExchange(&a->SubmitWorkerStop, 0, 0) != 0 ||
        InterlockedCompareExchange(&a->SchedulerAccepting, 0, 0) == 0 ||
        InterlockedCompareExchange(&a->NodeSched[node].TdrPending, 0, 0) != 0) {
        return FALSE;
    }

    KeAcquireSpinLock(&a->SubmitQueueLock, &irql);
    next = (a->SubmitQueueTail + 1) % WINMALI_SUBMIT_QUEUE_DEPTH;
    if (next != a->SubmitQueueHead &&
        (context == NULL || WinMaliContextGet_(context))) {
        a->SubmitQueue[a->SubmitQueueTail].FenceId       = fence;
        a->SubmitQueue[a->SubmitQueueTail].NodeOrdinal   = node;
        a->SubmitQueue[a->SubmitQueueTail].EngineOrdinal = engine;
        a->SubmitQueue[a->SubmitQueueTail].HasCs         = (cs != NULL) ? TRUE : FALSE;
        a->SubmitQueue[a->SubmitQueueTail].AsSlot        = asSlot;
        a->SubmitQueue[a->SubmitQueueTail].Context       = context;
        a->SubmitQueue[a->SubmitQueueTail].ResetGeneration =
            InterlockedCompareExchange(&a->NodeSched[node].ResetGeneration, 0, 0);
        if (cs != NULL) {
            RtlCopyMemory(&a->SubmitQueue[a->SubmitQueueTail].Cs, cs, sizeof(*cs));
        } else {
            RtlZeroMemory(&a->SubmitQueue[a->SubmitQueueTail].Cs,
                          sizeof(a->SubmitQueue[a->SubmitQueueTail].Cs));
        }
        a->SubmitQueueTail = next;
        KeClearEvent(&a->SubmitWorkerIdle);
        KeClearEvent(&a->NodeSched[node].IdleEvent);
        queued = TRUE;
    }
    KeReleaseSpinLock(&a->SubmitQueueLock, irql);
    if (queued) {
        KeSetEvent(&a->SubmitWorkerWake, IO_NO_INCREMENT, FALSE);
    }
    return queued;
}

/* Caller holds SubmitQueueLock. Remove every queued packet for one node while
   preserving the order of all other nodes. */
static UINT
WinMaliSchedDrainNodeLocked_(_Inout_ PWINMALI_ADAPTER a, _In_ UINT node)
{
    ULONG read = a->SubmitQueueHead;
    ULONG write = a->SubmitQueueHead;
    UINT drained = 0;
    while (read != a->SubmitQueueTail) {
        ULONG next = (read + 1) % WINMALI_SUBMIT_QUEUE_DEPTH;
        if (a->SubmitQueue[read].NodeOrdinal == node) {
            WinMaliContextPut_((PWINMALI_KMD_CONTEXT)
                               a->SubmitQueue[read].Context);
            a->SubmitQueue[read].Context = NULL;
            ++drained;
        } else {
            if (write != read) {
                a->SubmitQueue[write] = a->SubmitQueue[read];
            }
            write = (write + 1) % WINMALI_SUBMIT_QUEUE_DEPTH;
        }
        read = next;
    }
    a->SubmitQueueTail = write;
    return drained;
}

/* Caller holds SubmitQueueLock. Publish node/global idle state from the same
   lock domain used by enqueue and dequeue. */
static VOID
WinMaliSchedUpdateIdleLocked_(_Inout_ PWINMALI_ADAPTER a)
{
    ULONG i;
    ULONG read;
    BOOLEAN queued[WINMALI_NODE_COUNT] = { FALSE, FALSE };
    for (read = a->SubmitQueueHead; read != a->SubmitQueueTail;
         read = (read + 1) % WINMALI_SUBMIT_QUEUE_DEPTH) {
        UINT node = a->SubmitQueue[read].NodeOrdinal;
        if (node < WINMALI_NODE_COUNT) queued[node] = TRUE;
    }
    for (i = 0; i < WINMALI_NODE_COUNT; ++i) {
        if (!queued[i] && InterlockedCompareExchange(
                &a->SubmitWorkerActiveNode, -1, -1) != (LONG)i) {
            KeSetEvent(&a->NodeSched[i].IdleEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    if (a->SubmitQueueHead == a->SubmitQueueTail &&
        InterlockedCompareExchange(&a->SubmitWorkerActive, 0, 0) == 0) {
        KeSetEvent(&a->SubmitWorkerIdle, IO_NO_INCREMENT, FALSE);
    }
}

static BOOLEAN
WinMaliSchedHandlePreempt_(_Inout_ PWINMALI_ADAPTER a)
{
    UINT node;
    for (node = 0; node < WINMALI_NODE_COUNT; ++node) {
        if (InterlockedCompareExchange(&a->NodeSched[node].PreemptPending, 0, 1) == 1) {
            KIRQL irql;
            UINT drained;
            UINT preemptFence = (UINT)InterlockedCompareExchange(
                &a->NodeSched[node].PreemptionFenceId, 0, 0);
            UINT last = (UINT)InterlockedCompareExchange64(
                &a->NodeSched[node].WorkerCompletedFence, 0, 0);
            KeAcquireSpinLock(&a->SubmitQueueLock, &irql);
            drained = WinMaliSchedDrainNodeLocked_(a, node);
            WinMaliSchedUpdateIdleLocked_(a);
            KeReleaseSpinLock(&a->SubmitQueueLock, irql);
            if (WinMaliSchedPushFence_(a, preemptFence, node, 0, TRUE, last,
                                      STATUS_SUCCESS)) {
                WinMaliSchedRequestDpc(a);
            }
            WINMALI_TRACE("Sched: preempted node=%u fence=%u last=%u dropped=%u",
                          node, preemptFence, last, drained);
            return TRUE;
        }
    }
    return FALSE;
}

/* Dedicated worker: drains the submit queue in order, runs the real CS for
   tagged render submits, and turns preempt markers into a
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
            BOOLEAN hasCs = FALSE;
            ULONG   asSlot = WINMALI_AS_SLOT_MAX;
            PWINMALI_KMD_CONTEXT submitContext = NULL;
            LONG    resetGeneration = 0;
            WINMALI_CS_SUBMIT_DESC cs;

            RtlZeroMemory(&cs, sizeof(cs));

            if (WinMaliSchedHandlePreempt_(a)) {
                continue;
            }

            KeAcquireSpinLock(&a->SubmitQueueLock, &irql);
            if (a->SubmitQueueHead != a->SubmitQueueTail) {
                node      = a->SubmitQueue[a->SubmitQueueHead].NodeOrdinal;
                if (node < WINMALI_NODE_COUNT &&
                    InterlockedCompareExchange(
                        &a->NodeSched[node].TdrPending, 0, 0) != 0) {
                    WinMaliSchedUpdateIdleLocked_(a);
                    KeReleaseSpinLock(&a->SubmitQueueLock, irql);
                    break;
                }
                fence     = a->SubmitQueue[a->SubmitQueueHead].FenceId;
                engine    = a->SubmitQueue[a->SubmitQueueHead].EngineOrdinal;
                hasCs     = a->SubmitQueue[a->SubmitQueueHead].HasCs;
                asSlot    = a->SubmitQueue[a->SubmitQueueHead].AsSlot;
                submitContext = (PWINMALI_KMD_CONTEXT)
                    a->SubmitQueue[a->SubmitQueueHead].Context;
                a->SubmitQueue[a->SubmitQueueHead].Context = NULL;
                resetGeneration = a->SubmitQueue[a->SubmitQueueHead].ResetGeneration;
                if (hasCs) {
                    RtlCopyMemory(&cs, &a->SubmitQueue[a->SubmitQueueHead].Cs,
                                  sizeof(cs));
                }
                a->SubmitQueueHead = (a->SubmitQueueHead + 1) % WINMALI_SUBMIT_QUEUE_DEPTH;
                InterlockedExchange(&a->SubmitWorkerActive, 1);
                InterlockedExchange(&a->SubmitWorkerActiveNode, (LONG)node);
                InterlockedExchange64(&a->NodeSched[node].ActiveFence, (LONG64)fence);
                have = TRUE;
            } else {
                /* Publish idle while holding the same lock used by enqueue.
                   This prevents enqueue from clearing the event just before
                   the worker incorrectly sets it again. */
                KeSetEvent(&a->SubmitWorkerIdle, IO_NO_INCREMENT, FALSE);
            }
            KeReleaseSpinLock(&a->SubmitQueueLock, irql);
            if (!have) {
                break;
            }

            {
                NTSTATUS st = STATUS_SUCCESS;
                ULONG i;
                PWINMALI_VM transientVm = NULL;
                BOOLEAN transientLeased = FALSE;
                BOOLEAN contextLeased = FALSE;
                BOOLEAN transientParked = FALSE;
                BOOLEAN executionGateHeld = FALSE;
                BOOLEAN diagBegan = FALSE;
                BOOLEAN preemptedBeforeExecution = FALSE;
                ULONGLONG executeStart100ns = KeQueryInterruptTime();

                WINMALI_ROUTINE("CSF WORKER BEGIN: fence=%u node=%u engine=%u hasCs=%u vm=%u AS%u stream=0x%llx size=%u syncs=%u",
                             fence, node, engine, hasCs, hasCs ? cs.VmId : 0,
                             asSlot, hasCs ? (ULONGLONG)cs.StreamGpuVa : 0,
                             hasCs ? cs.StreamSize : 0,
                             hasCs ? cs.SyncCount : 0);

                if (hasCs) {
                    for (i = 0; i < cs.SyncCount; ++i) {
                        if ((cs.SyncOps[i].Flags & WINMALI_SYNC_OP_SIGNAL) == 0) {
                            ULONG h = cs.SyncOps[i].Handle;
                            UINT64 point = cs.SyncOps[i].TimelineValue;
                            int err;
                            do {
                                /* Poll in bounded slices so ResetEngine can
                                   cancel a packet waiting on a dependency. */
                                err = WinMaliSyncObjWait(a, 1u, &h, &point,
                                                        TRUE, 10000000LL);
                            } while (err == -110 &&
                                     InterlockedCompareExchange(&a->SubmitWorkerStop, 0, 0) == 0 &&
                                     InterlockedCompareExchange(&a->NodeSched[node].PreemptPending, 0, 0) == 0 &&
                                     resetGeneration ==
                                     InterlockedCompareExchange(
                                         &a->NodeSched[node].ResetGeneration, 0, 0));
                            if (resetGeneration !=
                                InterlockedCompareExchange(
                                    &a->NodeSched[node].ResetGeneration, 0, 0) ||
                                InterlockedCompareExchange(&a->SubmitWorkerStop, 0, 0) != 0 ||
                                InterlockedCompareExchange(&a->NodeSched[node].PreemptPending, 0, 0) != 0) {
                                /* A pending preemption can cancel a packet
                                   while it is still waiting on a dependency.
                                   That packet never reached CSF and must not
                                   be reported as completed.  Keep this fact
                                   separate from the pending bit: a preemption
                                   can also arrive while CSF is already running,
                                   in which case the packet is allowed to finish
                                   at our DMA-buffer preemption boundary. */
                                if (resetGeneration ==
                                        InterlockedCompareExchange(
                                            &a->NodeSched[node].ResetGeneration,
                                            0, 0) &&
                                    InterlockedCompareExchange(
                                        &a->SubmitWorkerStop, 0, 0) == 0 &&
                                    InterlockedCompareExchange(
                                        &a->NodeSched[node].PreemptPending,
                                        0, 0) != 0) {
                                    preemptedBeforeExecution = TRUE;
                                }
                                st = STATUS_CANCELLED;
                                break;
                            }
                            if (err != 0) {
                                st = STATUS_INVALID_HANDLE;
                                WINMALI_ERROR("Sched: wait sync failed err=%d handle=%u point=%llu fence=%u",
                                              err, h, (unsigned long long)point, fence);
                                break;
                            }
                        }
                    }
                    if (NT_SUCCESS(st)) {
                        st = WinMaliMmuEnterTransientExecution(a);
                        executionGateHeld = NT_SUCCESS(st);
                    }
                    if (NT_SUCCESS(st)) {
                        /* Both address models are non-resident while idle.
                           A dxgk-addressed stream leases its recorded context
                           root; an escape-addressed stream leases its VM root. */
                        if (cs.Flags & WINMALI_CS_SUBMIT_FLAG_DXGK_ADDR) {
                            if (submitContext == NULL) {
                                st = STATUS_INVALID_HANDLE;
                            } else {
                                st = WinMaliContextAcquireAs_(submitContext,
                                                             &asSlot);
                                contextLeased = NT_SUCCESS(st);
                            }
                        } else {
                            transientVm = WinMaliVmGet(a, cs.VmId);
                            if (transientVm == NULL) {
                                st = STATUS_INVALID_HANDLE;
                            } else {
                                st = WinMaliMmuVmAcquireAs(
                                    a, &transientVm->Pt, &asSlot);
                                transientLeased = NT_SUCCESS(st);
                            }
                        }
                    }
                    if (NT_SUCCESS(st)) {
                        WinMaliDiagBeginSubmit(a, fence, cs.VmId, asSlot,
                                               cs.StreamGpuVa, cs.StreamSize);
                        diagBegan = TRUE;
                        WinMaliBoFlushVm(a, cs.VmId, TRUE);
                        if (transientVm != NULL || contextLeased) {
                            st = WinMaliCsfSubmitTransientGroupStream(
                                a, asSlot, cs.StreamGpuVa, cs.StreamSize,
                                0u, 500u, &transientParked);
                        } else {
                            st = WinMaliCsfSubmitGroupStream(a, asSlot,
                                                            cs.StreamGpuVa,
                                                            cs.StreamSize,
                                                            0u, 500u);
                        }
                        WinMaliBoFlushVm(a, cs.VmId, FALSE);
                        WINMALI_DIAG("CSF WORKER EXECUTED: fence=%u vm=%u AS%u stream=0x%llx size=%u status=0x%08x elapsed100ns=%llu",
                                     fence, cs.VmId, asSlot,
                                     (ULONGLONG)cs.StreamGpuVa, cs.StreamSize,
                                     st,
                                     (ULONGLONG)(KeQueryInterruptTime() -
                                                 executeStart100ns));
                    }
                    if (diagBegan) {
                        WinMaliDiagCompleteSubmit(a, st);
                    }
                    if (transientVm != NULL) {
                        if (transientLeased && transientParked) {
                            WinMaliMmuVmReleaseAs(a, &transientVm->Pt, asSlot);
                        } else if (transientLeased) {
                            WINMALI_WARN("Sched: retaining transient AS%u vm=%u because CSG park was not acknowledged",
                                         asSlot, cs.VmId);
                        }
                        WinMaliVmPut(transientVm);
                    }
                    if (contextLeased) {
                        if (transientParked) {
                            WinMaliContextReleaseAs_(submitContext, asSlot);
                        } else {
                            WINMALI_WARN("Sched: retaining transient AS%u ctx=%p because CSG park was not acknowledged",
                                         asSlot, submitContext);
                        }
                    }
                    if (executionGateHeld) {
                        WinMaliMmuLeaveTransientExecution(a);
                    }
                }
                if (resetGeneration !=
                    InterlockedCompareExchange(&a->NodeSched[node].ResetGeneration, 0, 0) ||
                    InterlockedCompareExchange(&a->SubmitWorkerStop, 0, 0) != 0) {
                    WINMALI_WARN("Sched: dropping stale fence=%u generation=%ld currentGeneration=%ld stopping=%ld",
                                 fence, resetGeneration,
                                 InterlockedCompareExchange(
                                     &a->NodeSched[node].ResetGeneration, 0, 0),
                                 InterlockedCompareExchange(
                                     &a->SubmitWorkerStop, 0, 0));
                } else if (preemptedBeforeExecution) {
                    /* The packet was removed from the software queue but was
                       never submitted to CSF.  WinMaliSchedHandlePreempt_
                       reports DMA_PREEMPTED and dxgkrnl can resubmit it. */
                    WINMALI_DIAG("Sched: fence=%u canceled before CSF for pending preemption",
                                 fence);
                } else if (!NT_SUCCESS(st)) {
                    WinMaliSchedFinishSubmit_(a, &cs, fence, st);
                    ++a->CsfSubmitFailures;
                    WINMALI_ERROR("Sched: real CS failed 0x%08x fence=%u vm=%u as=%u total=%u",
                                  st, fence, cs.VmId, asSlot,
                                  a->CsfSubmitFailures);
                    WinMaliSchedQueueTimeout_(a, fence, node, engine, st);
                } else {
                    /* Preemption is DMA-buffer-boundary based.  If CSF has
                       already completed this packet, it is a real completion
                       even when PreemptPending was raised while it ran.  Queue
                       DMA_COMPLETED first and advance WorkerCompletedFence;
                       the next worker iteration will then emit DMA_PREEMPTED
                       with this fence as LastCompletedFenceId and drain only
                       packets which never executed. */
                    WinMaliSchedFinishSubmit_(a, &cs, fence, STATUS_SUCCESS);
                    InterlockedExchange64(&a->NodeSched[node].WorkerCompletedFence,
                                          (LONG64)fence);
                    if (!WinMaliSchedPushFence_(a, fence, node, engine, FALSE, 0,
                                                STATUS_SUCCESS)) {
                        WinMaliSchedQueueTimeout_(a, fence, node, engine,
                                                  STATUS_INSUFFICIENT_RESOURCES);
                    } else {
                        WINMALI_DIAG("CSF FENCE QUEUED: fence=%u node=%u engine=%u elapsed100ns=%llu",
                                     fence, node, engine,
                                     (ULONGLONG)(KeQueryInterruptTime() -
                                                 executeStart100ns));
                    }
                }
            }

            InterlockedExchange64(&a->NodeSched[node].ActiveFence, 0);
            InterlockedExchange(&a->SubmitWorkerActive, 0);
            InterlockedExchange(&a->SubmitWorkerActiveNode, -1);
            KeAcquireSpinLock(&a->SubmitQueueLock, &irql);
            WinMaliSchedUpdateIdleLocked_(a);
            KeReleaseSpinLock(&a->SubmitQueueLock, irql);
            WinMaliSchedRequestDpc(a);
            WinMaliContextPut_(submitContext);

            /* Do not execute later packets on a node while dxgkrnl is
               recovering the failed packet. */
            if (InterlockedCompareExchange(&a->NodeSched[node].TdrPending, 0, 0) != 0) {
                break;
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
    Adapter->FenceQueueHead = 0;
    Adapter->FenceQueueTail = 0;
    Adapter->NotifyDpcPending = 0;
    Adapter->SubmitWorkerStop = 0;
    Adapter->SubmitWorkerActive = 0;
    Adapter->SubmitWorkerActiveNode = -1;
    Adapter->SchedulerAccepting = 1;
    KeInitializeEvent(&Adapter->SubmitWorkerWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Adapter->SubmitWorkerIdle, NotificationEvent, TRUE);
    {
        ULONG node;
        for (node = 0; node < WINMALI_NODE_COUNT; ++node) {
            RtlZeroMemory(&Adapter->NodeSched[node], sizeof(Adapter->NodeSched[node]));
            KeInitializeEvent(&Adapter->NodeSched[node].IdleEvent,
                              NotificationEvent, TRUE);
        }
    }

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
    KIRQL irql;
    if (th == NULL) {
        return;
    }
    InterlockedExchange(&Adapter->SchedulerAccepting, 0);
    InterlockedExchange(&Adapter->SubmitWorkerStop, 1);
    InterlockedIncrement(&Adapter->NodeSched[0].ResetGeneration);
    InterlockedIncrement(&Adapter->NodeSched[1].ResetGeneration);
    KeSetEvent(&Adapter->SubmitWorkerWake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(th, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(th);
    Adapter->SubmitWorkerThread = NULL;
    KeAcquireSpinLock(&Adapter->SubmitQueueLock, &irql);
    while (Adapter->SubmitQueueHead != Adapter->SubmitQueueTail) {
        WinMaliContextPut_((PWINMALI_KMD_CONTEXT)
            Adapter->SubmitQueue[Adapter->SubmitQueueHead].Context);
        Adapter->SubmitQueue[Adapter->SubmitQueueHead].Context = NULL;
        Adapter->SubmitQueueHead =
            (Adapter->SubmitQueueHead + 1) % WINMALI_SUBMIT_QUEUE_DEPTH;
    }
    KeReleaseSpinLock(&Adapter->SubmitQueueLock, irql);
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
       dxgk scheduler thread on the CSF round-trip. The worker runs the real
       CS off-thread and reports its terminal result through the DPC. The
       worker's SubmitLock-serialized CSF path also serializes with escape
       GroupSubmits on the single CSG slot.

       Queue exhaustion is reported through the TDR path; claiming completion
       for an unexecuted packet would corrupt dxgmms2's fence accounting. */
    if (!WinMaliSchedNodeValid_(pSubmit->NodeOrdinal, pSubmit->EngineOrdinal)) {
        return STATUS_INVALID_PARAMETER;
    }
    InterlockedExchange64(&adapter->NodeSched[pSubmit->NodeOrdinal].SubmittedFence,
                          (LONG64)pSubmit->SubmissionFenceId);
    if (!WinMaliSchedEnqueue_(adapter, pSubmit->SubmissionFenceId,
                               pSubmit->NodeOrdinal, pSubmit->EngineOrdinal,
                               NULL, WINMALI_AS_SLOT_MAX, NULL)) {
        /* The packet was accepted by dxgkrnl but was not queued. Never claim
           completion for work that did not execute; enter the normal TDR
           recovery path instead. */
        WinMaliSchedQueueTimeout_(adapter, pSubmit->SubmissionFenceId,
                                  pSubmit->NodeOrdinal, pSubmit->EngineOrdinal,
                                  STATUS_DEVICE_BUSY);
    }
    return STATUS_SUCCESS;
}

/* WDDM 2+ submits GPUVA command buffers directly from user mode. Validate the
   complete UMD-owned portion of the private data before dxgkrnl places the
   packet on a scheduler queue. No user pointers are accepted: every value in
   WINMALI_CS_SUBMIT_DESC is an integer, GPUVA, or opaque KMD-owned handle. */
_Function_class_(DXGKDDI_VALIDATESUBMITCOMMAND)
NTSTATUS
APIENTRY
WinMaliKmdValidateSubmitCommand(
    IN_CONST_HANDLE                     hAdapter,
    INOUT_PDXGKARG_VALIDATESUBMITCOMMAND pArgs)
{
    PWINMALI_ADAPTER adapter;
    PWINMALI_KMD_CONTEXT ctx;
    WINMALI_CS_SUBMIT_DESC cs;
    ULONG i;

    if (pArgs == NULL) {
        WINMALI_ERROR("ValidateSubmitCommand: NULL args");
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        WINMALI_ERROR("ValidateSubmitCommand: invalid adapter=%p", hAdapter);
        return STATUS_INVALID_HANDLE;
    }

    WINMALI_TRACE("ValidateSubmitCommand: commands=0x%llx length=%u flags=0x%x "
                  "contexts=%u private=%p/%u umd=%u",
                  (unsigned long long)pArgs->Commands, pArgs->CommandLength,
                  pArgs->Flags.Value, pArgs->ContextCount,
                  pArgs->pPrivateDriverData, pArgs->PrivateDriverDataSize,
                  pArgs->UmdPrivateDataSize);

    if (pArgs->Flags.HardwareQueueSubmission ||
        pArgs->ContextCount != 1 || pArgs->Context[0] == NULL) {
        WINMALI_ERROR("ValidateSubmitCommand: unsupported flags=0x%x contexts=%u first=%p",
                      pArgs->Flags.Value, pArgs->ContextCount,
                      pArgs->ContextCount != 0 ? pArgs->Context[0] : NULL);
        return STATUS_INVALID_PARAMETER;
    }

    ctx = (PWINMALI_KMD_CONTEXT)pArgs->Context[0];
    if (ctx->Magic != WINMALI_KMD_CONTEXT_MAGIC ||
        ctx->Adapter != adapter || ctx->OwnerDevice == NULL ||
        ctx->NodeOrdinal != 0) {
        WINMALI_ERROR("ValidateSubmitCommand: invalid context=%p magic=0x%x adapter=%p "
                      "owner=%p node=%u",
                      ctx, ctx->Magic, ctx->Adapter, ctx->OwnerDevice,
                      ctx->NodeOrdinal);
        return STATUS_INVALID_HANDLE;
    }

    if (pArgs->Commands == 0 || pArgs->CommandLength == 0 ||
        pArgs->pPrivateDriverData == NULL ||
        pArgs->UmdPrivateDataSize != sizeof(cs) ||
        pArgs->PrivateDriverDataSize < pArgs->UmdPrivateDataSize) {
        WINMALI_ERROR("ValidateSubmitCommand: invalid buffers commands=0x%llx length=%u "
                      "private=%p/%u umd=%u expected=%Iu",
                      (unsigned long long)pArgs->Commands,
                      pArgs->CommandLength, pArgs->pPrivateDriverData,
                      pArgs->PrivateDriverDataSize, pArgs->UmdPrivateDataSize,
                      sizeof(cs));
        return STATUS_INVALID_USER_BUFFER;
    }

    RtlCopyMemory(&cs, pArgs->pPrivateDriverData, sizeof(cs));
    if (cs.Magic != WINMALI_CS_SUBMIT_DESC_MAGIC ||
        cs.StreamGpuVa != pArgs->Commands ||
        cs.StreamSize != pArgs->CommandLength ||
        cs.StreamGpuVa == 0 || cs.StreamSize == 0 ||
        cs.Flags != WINMALI_CS_SUBMIT_FLAG_DXGK_ADDR ||
        cs.SyncCount > WINMALI_CS_SUBMIT_MAX_SYNCS ||
        cs.Reserved != 0) {
        WINMALI_ERROR("ValidateSubmitCommand: invalid desc magic=0x%x vm=%u "
                      "va=0x%llx/%llx size=%u/%u flags=0x%x syncs=%u reserved=0x%x",
                      cs.Magic, cs.VmId,
                      (unsigned long long)cs.StreamGpuVa,
                      (unsigned long long)pArgs->Commands,
                      cs.StreamSize, pArgs->CommandLength, cs.Flags,
                      cs.SyncCount, cs.Reserved);
        return STATUS_INVALID_USER_BUFFER;
    }

    for (i = 0; i < cs.SyncCount; ++i) {
        ULONG handleType = cs.SyncOps[i].Flags & WINMALI_SYNC_OP_HANDLE_TYPE_MASK;
        ULONG unknownFlags = cs.SyncOps[i].Flags &
            ~(WINMALI_SYNC_OP_SIGNAL | WINMALI_SYNC_OP_HANDLE_TYPE_MASK);
        BOOLEAN timeline;

        if (unknownFlags != 0 ||
            (handleType != WINMALI_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
             handleType != WINMALI_SYNC_OP_HANDLE_TYPE_TIMELINE)) {
            WINMALI_ERROR("ValidateSubmitCommand: invalid sync[%u] handle=%u "
                          "flags=0x%x point=%llu",
                          i, cs.SyncOps[i].Handle, cs.SyncOps[i].Flags,
                          (unsigned long long)cs.SyncOps[i].TimelineValue);
            return STATUS_INVALID_USER_BUFFER;
        }
        timeline = (handleType == WINMALI_SYNC_OP_HANDLE_TYPE_TIMELINE);
        if (!WinMaliSyncObjValidate(adapter, cs.SyncOps[i].Handle,
                                    NULL, timeline)) {
            WINMALI_ERROR("ValidateSubmitCommand: unknown/mismatched sync[%u] "
                          "handle=%u type=%u",
                          i, cs.SyncOps[i].Handle, handleType);
            return STATUS_INVALID_HANDLE;
        }
    }

    WINMALI_DIAG("ValidateSubmitCommand: accepted ctx=%p vm=%u va=0x%llx "
                  "size=%u syncs=%u",
                  ctx, cs.VmId, (unsigned long long)cs.StreamGpuVa,
                  cs.StreamSize, cs.SyncCount);
    return STATUS_SUCCESS;
}

/* DxgkDdiSubmitCommandVirtual - the GpuMmu (GPU-virtual-address) submit path.
   Because we report GpuMmuSupported=1, dxgkrnl submits DMA buffers through THIS
   DDI, not the legacy physical DxgkDdiSubmitCommand - in particular the paging
   buffers for our copy node (node 1). A stubbed NOT_SUPPORTED here bugchecks
   0x119 (VIDEO_SCHEDULER_INTERNAL_ERROR, arg2=0xC00000BB). Behaviour mirrors
   WinMaliKmdSubmitCommand: run a present blt if tagged, then hand the fence to
   the async worker and return. */
_Function_class_(DXGKDDI_SUBMITCOMMANDVIRTUAL)
NTSTATUS
APIENTRY
WinMaliKmdSubmitCommandVirtual(
    IN_CONST_HANDLE                        hAdapter,
    IN_CONST_PDXGKARG_SUBMITCOMMANDVIRTUAL pSubmit)
{
    PWINMALI_ADAPTER adapter;
    WINMALI_CS_SUBMIT_DESC csCopy;
    const WINMALI_CS_SUBMIT_DESC* queuedCs = NULL;
    ULONG asSlot = WINMALI_AS_SLOT_MAX;
    NTSTATUS csStatus = STATUS_SUCCESS;
    BOOLEAN needsTransientVm = FALSE;
    BOOLEAN needsTransientContext = FALSE;

    RtlZeroMemory(&csCopy, sizeof(csCopy));

    if (pSubmit == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    WINMALI_DIAG("SubmitCommandVirtual: fenceId=%u node=%u engine=%u ctx=%p dmaVa=0x%llx dmaSize=%u flags=0x%x private=%p/%u umd=%u",
                  pSubmit->SubmissionFenceId,
                  pSubmit->NodeOrdinal,
                  pSubmit->EngineOrdinal,
                  pSubmit->hContext,
                  (unsigned long long)pSubmit->DmaBufferVirtualAddress,
                  pSubmit->DmaBufferSize,
                  pSubmit->Flags.Value,
                  pSubmit->pDmaBufferPrivateData,
                  pSubmit->DmaBufferPrivateDataSize,
                  pSubmit->DmaBufferUmdPrivateDataSize);

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
            /* The descriptor records which page-table object the worker must
               make resident. SetRootPageTable and VmCreate no longer reserve
               AS slots while idle. */
            if (cs->Flags & WINMALI_CS_SUBMIT_FLAG_DXGK_ADDR) {
                PWINMALI_KMD_CONTEXT ctx =
                    (PWINMALI_KMD_CONTEXT)pSubmit->hContext;
                if (ctx != NULL && ctx->Magic == WINMALI_KMD_CONTEXT_MAGIC &&
                    ctx->Adapter == adapter && ctx->RootPtPa != 0 &&
                    InterlockedCompareExchange(&ctx->Destroying, 0, 0) == 0) {
                    needsTransientContext = TRUE;
                }
            } else {
                PWINMALI_VM vm = WinMaliVmGet(adapter, cs->VmId);
                if (vm != NULL) {
                    needsTransientVm = TRUE;
                    WinMaliVmPut(vm);
                }
            }
            if (cs->SyncCount > WINMALI_CS_SUBMIT_MAX_SYNCS) {
                WINMALI_ERROR("SubmitCommandVirtual: invalid sync count=%u", cs->SyncCount);
                csStatus = STATUS_INVALID_PARAMETER;
            } else if (needsTransientContext || needsTransientVm) {
                RtlCopyMemory(&csCopy, cs, sizeof(csCopy));
                queuedCs = &csCopy;
            } else {
                WINMALI_WARN("SubmitCommandVirtual: CS desc vm=%u -> no address space",
                             cs->VmId);
                RtlCopyMemory(&csCopy, cs, sizeof(csCopy));
                csStatus = STATUS_DEVICE_NOT_READY;
            }
        }
    }

    if (!WinMaliSchedNodeValid_(pSubmit->NodeOrdinal, pSubmit->EngineOrdinal)) {
        return STATUS_INVALID_PARAMETER;
    }
    InterlockedExchange64(&adapter->NodeSched[pSubmit->NodeOrdinal].SubmittedFence,
                          (LONG64)pSubmit->SubmissionFenceId);
    if (!NT_SUCCESS(csStatus)) {
        if (csCopy.SyncCount <= WINMALI_CS_SUBMIT_MAX_SYNCS) {
            WinMaliSchedFinishSubmit_(adapter, &csCopy,
                                      pSubmit->SubmissionFenceId, csStatus);
        }
        WinMaliSchedQueueTimeout_(adapter, pSubmit->SubmissionFenceId,
                                  pSubmit->NodeOrdinal, pSubmit->EngineOrdinal,
                                  csStatus);
        return STATUS_SUCCESS;
    }
    if (!WinMaliSchedEnqueue_(adapter, pSubmit->SubmissionFenceId,
                               pSubmit->NodeOrdinal, pSubmit->EngineOrdinal,
                               queuedCs, asSlot,
                               (PWINMALI_KMD_CONTEXT)pSubmit->hContext)) {
        NTSTATUS queueStatus = STATUS_DEVICE_BUSY;
        WinMaliSchedFinishSubmit_(adapter, queuedCs,
                                  pSubmit->SubmissionFenceId, queueStatus);
        WinMaliSchedQueueTimeout_(adapter, pSubmit->SubmissionFenceId,
                                  pSubmit->NodeOrdinal, pSubmit->EngineOrdinal,
                                  queueStatus);
    }
    return STATUS_SUCCESS;
}

/* DxgkDdiPreemptCommand - request urgent DMA-buffer-boundary preemption. */
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

    if (!WinMaliSchedNodeValid_(pPreempt->NodeOrdinal, pPreempt->EngineOrdinal)) {
        return STATUS_INVALID_PARAMETER;
    }
    InterlockedExchange(&adapter->NodeSched[pPreempt->NodeOrdinal].PreemptionFenceId,
                        (LONG)pPreempt->PreemptionFenceId);
    InterlockedExchange(&adapter->NodeSched[pPreempt->NodeOrdinal].PreemptPending, 1);
    KeSetEvent(&adapter->SubmitWorkerWake, IO_NO_INCREMENT, FALSE);
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

/* DxgkDdiQueryEngineStatus - the scheduler's health probe. A live worker is
   not itself proof of GPU progress: report responsive only when the completed
   fence has caught up to the submitted fence. */
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
    if (!WinMaliSchedNodeValid_(pArgs->NodeOrdinal, pArgs->EngineOrdinal)) {
        return STATUS_INVALID_PARAMETER;
    }
    pArgs->EngineStatus.Value = 0;
    pArgs->EngineStatus.Responsive =
        (InterlockedCompareExchange64(
             &adapter->NodeSched[pArgs->NodeOrdinal].CompletedFence, 0, 0) ==
         InterlockedCompareExchange64(
             &adapter->NodeSched[pArgs->NodeOrdinal].SubmittedFence, 0, 0) &&
         InterlockedCompareExchange(
             &adapter->NodeSched[pArgs->NodeOrdinal].TdrPending, 0, 0) == 0) ? 1u : 0u;
    return STATUS_SUCCESS;
}

/* DxgkDdiResetEngine - per-engine TDR. Invalidate the scheduler generation,
   wait until the bounded CSF operation has left the physical queue, and let
   dxgkrnl resubmit the aborted range. No pre-reset packet may complete a
   fence or private sync after this DDI returns. */
_Function_class_(DXGKDDI_RESETENGINE)
NTSTATUS
APIENTRY
WinMaliKmdResetEngine(
    IN_CONST_HANDLE            hAdapter,
    INOUT_PDXGKARG_RESETENGINE pReset)
{
    PWINMALI_ADAPTER adapter;
    KIRQL            irql;
    UINT             drained;
    UINT             discarded;
    UINT             node;
    NTSTATUS         resetStatus = STATUS_SUCCESS;
    LONG64           lastAborted;
    LONG64           lastCompleted;
    LONG64           lastSubmitted;

    if (pReset == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    node = pReset->NodeOrdinal;
    if (!WinMaliSchedNodeValid_(node, pReset->EngineOrdinal)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Invalidate the active generation first. The worker checks this after
       returning from its bounded CSF wait and must never complete a packet
       from the pre-reset hardware queue. Then discard queued packets; dxgkrnl
       resubmits the affected range according to LastAbortedFenceId. */
    InterlockedIncrement(&adapter->NodeSched[node].ResetGeneration);
    InterlockedExchange(&adapter->NodeSched[node].PreemptPending, 0);

    /* Drain pending submit requests without completing or signalling them.
       Dequeue under the lock (the worker uses the same lock, so we never
       double-consume an entry). Preempt markers are dropped - a reset
       supersedes an in-flight preempt. */
    KeAcquireSpinLock(&adapter->SubmitQueueLock, &irql);
    drained = WinMaliSchedDrainNodeLocked_(adapter, node);
    WinMaliSchedUpdateIdleLocked_(adapter);
    KeReleaseSpinLock(&adapter->SubmitQueueLock, irql);

    /* ResetEngine may return only after the hardware queue is empty. The CSF
       submit wait is bounded, and the worker sets Idle after it observes the
       invalidated generation and drops the stale terminal result. */
    if (InterlockedCompareExchange(&adapter->SubmitWorkerActiveNode, -1, -1) ==
        (LONG)node) {
        KeWaitForSingleObject(&adapter->NodeSched[node].IdleEvent, Executive,
                              KernelMode, FALSE, NULL);
    }

    /* dxgkrnl flushes queued DPCs before ResetEngine. Remove any node-local
       event produced while the worker was observing the reset generation so
       it cannot be delivered after the reset boundary is established. */
    discarded = WinMaliSchedDiscardNotifyNode_(adapter, node);

    if (node == 0u) {
        resetStatus = WinMaliCsfResetRenderEngine(adapter);
        if (!NT_SUCCESS(resetStatus)) {
            WINMALI_ERROR("ResetEngine: CSF reset failed 0x%08x", resetStatus);
            return resetStatus;
        }
    }

    /* The timeout DPC records the accepted packet that failed to retire.
       WDDM validates this value against its completed/submitted fence window;
       returning the old hard-coded zero could make dxgkrnl resubmit the
       already-failed packet or bugcheck once the completed fence was nonzero. */
    lastAborted = InterlockedExchange64(
        &adapter->NodeSched[node].LastAbortedFence, 0);
    lastCompleted = InterlockedCompareExchange64(
        &adapter->NodeSched[node].CompletedFence, 0, 0);
    lastSubmitted = InterlockedCompareExchange64(
        &adapter->NodeSched[node].SubmittedFence, 0, 0);
    /* Fence IDs are 32-bit and legally wrap. Validate the aborted fence in
       the modular [completed, submitted] window rather than comparing the
       zero-extended values as monotonically increasing 64-bit integers. */
    if ((UINT)((UINT)lastAborted - (UINT)lastCompleted) >
        (UINT)((UINT)lastSubmitted - (UINT)lastCompleted)) {
        WINMALI_WARN("ResetEngine: recorded aborted fence %llu outside valid [%llu,%llu]; using completed fence",
                     (unsigned long long)lastAborted,
                     (unsigned long long)lastCompleted,
                     (unsigned long long)lastSubmitted);
        lastAborted = lastCompleted;
    }
    pReset->LastAbortedFenceId = (UINT)lastAborted;

    /* A successful engine reset establishes a new scheduler recovery floor.
       dxgmms2 advances past LastAbortedFenceId and later requires
       DMA_PREEMPTED.LastCompletedFenceId and QueryCurrentFence to report the
       same boundary. Keep both the worker-side and published copies aligned;
       leaving them at their pre-reset value caused 0x119/1 (reported 0 while
       dxgmms2's required boundary was 4). */
    InterlockedExchange64(&adapter->NodeSched[node].WorkerCompletedFence,
                          (LONG64)pReset->LastAbortedFenceId);
    InterlockedExchange64(&adapter->NodeSched[node].CompletedFence,
                          (LONG64)pReset->LastAbortedFenceId);
    InterlockedExchange64(&adapter->NodeSched[node].ActiveFence, 0);
    InterlockedExchange(&adapter->NodeSched[node].PreemptionFenceId, 0);
    InterlockedExchange(&adapter->NodeSched[node].TdrPending, 0);
    KeSetEvent(&adapter->SubmitWorkerWake, IO_NO_INCREMENT, FALSE);
    WINMALI_TRACE("ResetEngine: node=%u engine=%u recoveryBoundary=%u drained=%u discardedNotify=%u",
                  pReset->NodeOrdinal, pReset->EngineOrdinal,
                  pReset->LastAbortedFenceId, drained, discarded);
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
_Function_class_(DXGKDDI_PRESENT)
NTSTATUS
APIENTRY
WinMaliKmdPresent(
    IN_CONST_HANDLE        hContext,
    INOUT_PDXGKARG_PRESENT pPresent)
{
    PWINMALI_KMD_OPEN_ALLOCATION src;
    PWINMALI_KMD_OPEN_ALLOCATION dst;
    PWINMALI_BLT_DESC       desc;

    UNREFERENCED_PARAMETER(hContext);
    if (pPresent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    WINMALI_DIAG("Flow Present ENTER ctx=%p flags=0x%x blt=%u flip=%u srcCount=%u dstCount=%u private=%p/%u dma=%p/%u",
                 hContext, pPresent->Flags.Value, pPresent->Flags.Blt,
                 pPresent->Flags.Flip, pPresent->NumSrcAllocations,
                 pPresent->NumDstAllocations, pPresent->pDmaBufferPrivateData,
                 pPresent->DmaBufferPrivateDataSize, pPresent->pDmaBuffer,
                 pPresent->DmaSize);

    if (!pPresent->Flags.Blt ||
        pPresent->pAllocationList == NULL ||
        pPresent->NumSrcAllocations < 1 ||
        pPresent->NumDstAllocations < 1 ||
        pPresent->pDmaBufferPrivateData == NULL ||
        pPresent->DmaBufferPrivateDataSize < sizeof(WINMALI_BLT_DESC)) {
        /* Nothing we can (or need to) do; complete so the swap chain proceeds. */
        WINMALI_DIAG("Flow Present EXIT ctx=%p result=no-blt-work", hContext);
        return STATUS_SUCCESS;
    }

    src = (PWINMALI_KMD_OPEN_ALLOCATION)
        pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation;
    dst = (PWINMALI_KMD_OPEN_ALLOCATION)
        pPresent->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].hDeviceSpecificAllocation;
    if (src == NULL || dst == NULL ||
        src->Magic != WINMALI_KMD_OPEN_ALLOC_MAGIC ||
        dst->Magic != WINMALI_KMD_OPEN_ALLOC_MAGIC) {
        WINMALI_ERROR("Present: invalid device allocations src=%p dst=%p", src, dst);
        return STATUS_INVALID_HANDLE;
    }

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

        if (src->Global != NULL && src->Global->ApertureMdl != NULL) {
            srcBase = (PUCHAR)MmGetSystemAddressForMdlSafe(
                src->Global->ApertureMdl,
                NormalPagePriority | MdlMappingNoExecute);
            if (srcBase != NULL)
                srcBase += (SIZE_T)src->Global->ApertureMdlOffset * PAGE_SIZE;
        }
        if (dst->Global != NULL && dst->Global->ApertureMdl != NULL) {
            dstBase = (PUCHAR)MmGetSystemAddressForMdlSafe(
                dst->Global->ApertureMdl,
                NormalPagePriority | MdlMappingNoExecute);
            if (dstBase != NULL)
                dstBase += (SIZE_T)dst->Global->ApertureMdlOffset * PAGE_SIZE;
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
            WINMALI_DIAG("Flow Present: Blt COPIED %dx%d bpp=%u src=%p dst=%p "
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
    if (pPresent->pDmaBuffer != NULL && pPresent->DmaSize >= sizeof(ULONG)) {
        *(ULONG UNALIGNED*)pPresent->pDmaBuffer = WINMALI_BLT_DESC_MAGIC;
        pPresent->pDmaBuffer = (PVOID)((PUCHAR)pPresent->pDmaBuffer + sizeof(ULONG));
    }
    WINMALI_DIAG("Flow Present EXIT ctx=%p result=blt-described src=%p dst=%p",
                 hContext, src, dst);
    return STATUS_SUCCESS;
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
/* DxgkDdiResetFromTimeout / RestartFromTimeout - adapter-wide TDR.          */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_RESETFROMTIMEOUT)
NTSTATUS
APIENTRY
WinMaliKmdResetFromTimeout(
    IN_CONST_HANDLE hAdapter)
{
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    KIRQL irql;
    NTSTATUS status;
    ULONG node;
    if (adapter == NULL) return STATUS_INVALID_HANDLE;

    WINMALI_TRACE("ResetFromTimeout: quiescing scheduler and CSF");
    InterlockedExchange(&adapter->SchedulerAccepting, 0);
    for (node = 0; node < WINMALI_NODE_COUNT; ++node) {
        InterlockedIncrement(&adapter->NodeSched[node].ResetGeneration);
        InterlockedExchange(&adapter->NodeSched[node].PreemptPending, 0);
    }
    KeAcquireSpinLock(&adapter->SubmitQueueLock, &irql);
    adapter->SubmitQueueHead = adapter->SubmitQueueTail;
    WinMaliSchedUpdateIdleLocked_(adapter);
    KeReleaseSpinLock(&adapter->SubmitQueueLock, irql);
    if (InterlockedCompareExchange(&adapter->SubmitWorkerActive, 0, 0) != 0) {
        KeWaitForSingleObject(&adapter->SubmitWorkerIdle, Executive,
                              KernelMode, FALSE, NULL);
    }
    status = WinMaliCsfResetRenderEngine(adapter);
    if (NT_SUCCESS(status)) {
        /* Adapter-wide reset retires every packet accepted before the reset.
           WDDM requires each node's last-completed boundary to advance to
           its last submitted fence. Synchronize every source used by
           QueryCurrentFence and DMA_PREEMPTED, and discard old-generation
           notifications before RestartFromTimeout reopens submission. */
        for (node = 0; node < WINMALI_NODE_COUNT; ++node) {
            LONG64 submitted = InterlockedCompareExchange64(
                &adapter->NodeSched[node].SubmittedFence, 0, 0);
            UINT discarded = WinMaliSchedDiscardNotifyNode_(adapter, node);
            InterlockedExchange64(
                &adapter->NodeSched[node].WorkerCompletedFence, submitted);
            InterlockedExchange64(
                &adapter->NodeSched[node].CompletedFence, submitted);
            InterlockedExchange64(&adapter->NodeSched[node].ActiveFence, 0);
            InterlockedExchange64(&adapter->NodeSched[node].LastAbortedFence, 0);
            InterlockedExchange(&adapter->NodeSched[node].PreemptionFenceId, 0);
            WINMALI_TRACE("ResetFromTimeout: node=%u recoveryBoundary=%u discardedNotify=%u",
                          node, (UINT)submitted, discarded);
        }
    }
    WINMALI_TRACE("ResetFromTimeout: CSF reset status=0x%08x", status);
    return status;
}

_Function_class_(DXGKDDI_RESTARTFROMTIMEOUT)
NTSTATUS
APIENTRY
WinMaliKmdRestartFromTimeout(
    IN_CONST_HANDLE hAdapter)
{
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    ULONG node;
    if (adapter == NULL) return STATUS_INVALID_HANDLE;
    for (node = 0; node < WINMALI_NODE_COUNT; ++node) {
        InterlockedExchange(&adapter->NodeSched[node].TdrPending, 0);
    }
    InterlockedExchange(&adapter->SchedulerAccepting, 1);
    KeSetEvent(&adapter->SubmitWorkerWake, IO_NO_INCREMENT, FALSE);
    WINMALI_TRACE("RestartFromTimeout: scheduler accepting work");
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
    /* Keep the crash-dump payload self-contained so a future scheduler fault
       can be diagnosed from a minidump without another live-KD session. */
    (VOID)RtlStringCbPrintfA((PCHAR)pArgs->pBuffer, pArgs->BufferSize,
        "WinMali flags=%08x regs=%u mcu=%u csf=%u "
        "n0[s=%u c=%u a=%u abort=%u tdr=%ld gen=%ld] "
        "n1[s=%u c=%u a=%u abort=%u tdr=%ld gen=%ld] "
        "sq=%u/%u fq=%u/%u activeNode=%ld accepting=%ld",
        adapter->AdapterFlags,
        adapter->GpuRegsMapped,
        (adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_MCU_ALIVE) ? 1 : 0,
        (adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_CSF_JOBS) ? 1 : 0,
        (UINT)InterlockedCompareExchange64(&adapter->NodeSched[0].SubmittedFence, 0, 0),
        (UINT)InterlockedCompareExchange64(&adapter->NodeSched[0].CompletedFence, 0, 0),
        (UINT)InterlockedCompareExchange64(&adapter->NodeSched[0].ActiveFence, 0, 0),
        (UINT)InterlockedCompareExchange64(&adapter->NodeSched[0].LastAbortedFence, 0, 0),
        InterlockedCompareExchange(&adapter->NodeSched[0].TdrPending, 0, 0),
        InterlockedCompareExchange(&adapter->NodeSched[0].ResetGeneration, 0, 0),
        (UINT)InterlockedCompareExchange64(&adapter->NodeSched[1].SubmittedFence, 0, 0),
        (UINT)InterlockedCompareExchange64(&adapter->NodeSched[1].CompletedFence, 0, 0),
        (UINT)InterlockedCompareExchange64(&adapter->NodeSched[1].ActiveFence, 0, 0),
        (UINT)InterlockedCompareExchange64(&adapter->NodeSched[1].LastAbortedFence, 0, 0),
        InterlockedCompareExchange(&adapter->NodeSched[1].TdrPending, 0, 0),
        InterlockedCompareExchange(&adapter->NodeSched[1].ResetGeneration, 0, 0),
        adapter->SubmitQueueHead, adapter->SubmitQueueTail,
        adapter->FenceQueueHead, adapter->FenceQueueTail,
        InterlockedCompareExchange(&adapter->SubmitWorkerActiveNode, -1, -1),
        InterlockedCompareExchange(&adapter->SchedulerAccepting, 0, 0));
    return STATUS_SUCCESS;
}

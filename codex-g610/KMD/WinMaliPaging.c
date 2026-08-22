/*
 * WinMaliPaging.c - DxgkDdiBuildPagingBuffer.
 *
 * dxgk emits one paging op per call (some chained via the same DMA
 * buffer). We declared PageTableUpdateMode=CPU_VIRTUAL in QueryAdapterInfo,
 * so UpdatePageTable hands us a kernel pointer into the page-table memory
 * and we write Mali LPAE entries directly.
 *
 * MAP/UNMAP_APERTURE_SEGMENT install/clear LPAE PTEs in the bring-up
 * AS's preallocated aperture window (see WinMaliMmu.c). Operations our
 * flat sysmem-only segment doesn't need (Transfer, Fill, Discard,
 * ReadPhysical, WritePhysical) are handled as no-op SUCCESS - there's
 * no second segment to migrate between.
 *
 * THE STATUS CONTRACT RULES EVERYTHING HERE: dxgmms2 accepts only
 * STATUS_SUCCESS or STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER from this
 * DDI - anything else is bugcheck 0x10E_b. And INSUFFICIENT_DMA_BUFFER
 * is a command ("flush the in-flight paging buffer, retry with a bigger
 * one"), only legal when the op actually carried a DMA buffer we could
 * not fit into. See the sanitizer + conditional NOP at the bottom of
 * WinMaliKmdBuildPagingBuffer.
 */

#include "WinMaliKmd.h"

/* The current failing render submission starts at this GPU VA.  Keep this
   diagnostic deliberately narrow: the initial VidMm population touches four
   512-entry tables, and dumping every PTE makes the useful ordering almost
   impossible to recover from DbgView. */
#define WINMALI_PAGING_DIAG_STREAM_GPU_VA (0x0000fffffffe6000ull)
#define WINMALI_PAGING_DIAG_FAULT_GPU_VA  (0x0000ffffffa26700ull)

/* DXGK_PTE stores the high 52 bits of a physical address: in other words,
   PageAddress/PageTableAddress are page frame numbers and the implicit low
   PAGE_SHIFT bits are zero.  Mali LPAE descriptors instead store the byte
   physical address in bits[47:12], so restore the omitted page offset before
   applying the hardware PA mask.

   The distinction is observable in the 24H2 runtime data.  For example,
   VidMm supplied PageTableAddress=0x3a4b8c for a table whose CPU mapping has
   physical address 0x3a4b8c000. */
static UINT64
WinMaliPagingPteAddressToPhysical_(_In_ UINT64 pageNumber)
{
    return (pageNumber << PAGE_SHIFT) & WINMALI_LPAE_PA_MASK;
}

static BOOLEAN
WinMaliPagingIsDiagnosticPte_(
    _In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* op,
    _In_ UINT entryOffset,
    _Out_opt_ UINT64* entryGpuVa,
    _Out_opt_ UINT64* targetGpuVa)
{
    static const UINT64 targets[] = {
        WINMALI_PAGING_DIAG_STREAM_GPU_VA,
        WINMALI_PAGING_DIAG_FAULT_GPU_VA
    };
    UINT shift;
    UINT64 va;
    ULONG targetIndex;

    if (op == NULL || op->PageTableLevel > 3u) {
        return FALSE;
    }

    /* WDDM numbers the leaf as level zero.  With 4 KiB pages and nine index
       bits at every level, one entry covers 4 KiB << (9 * level). */
    shift = 12u + (9u * op->PageTableLevel);
    va = op->FirstPteVirtualAddress + ((UINT64)entryOffset << shift);
    if (entryGpuVa != NULL) {
        *entryGpuVa = va;
    }
    for (targetIndex = 0; targetIndex < RTL_NUMBER_OF(targets);
         ++targetIndex) {
        UINT64 alignedTarget =
            targets[targetIndex] & ~((1ull << shift) - 1ull);
        if (va == alignedTarget) {
            if (targetGpuVa != NULL) {
                *targetGpuVa = targets[targetIndex];
            }
            return TRUE;
        }
    }
    return FALSE;
}

static VOID
WinMaliPagingTraceDiagnosticPte_(
    _In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* op,
    _In_ const DXGK_PTE* pte,
    _In_ UINT entryOffset,
    _In_ UINT64 encoded)
{
    PHYSICAL_ADDRESS tablePa;
    UINT64 entryGpuVa = 0;
    UINT64 targetGpuVa = 0;
    UINT absoluteIndex;

    if (!WinMaliPagingIsDiagnosticPte_(op, entryOffset, &entryGpuVa,
                                       &targetGpuVa)) {
        return;
    }

    absoluteIndex = op->StartIndex + entryOffset;
    tablePa = MmGetPhysicalAddress(op->PageTableAddress.CpuVirtual);

    WINMALI_DIAG(
        "PTW-DIAG: process=%p level=%u tableVa=%p tablePa=0x%llx "
        "start=%u offset=%u index=%u targetGpuVa=0x%llx "
        "entryGpuVa=0x%llx firstPteVa=0x%llx "
        "initial=%u repeat=%u alloc=%p allocOff=0x%llx",
        op->hProcess,
        op->PageTableLevel,
        op->PageTableAddress.CpuVirtual,
        (ULONGLONG)tablePa.QuadPart,
        op->StartIndex,
        entryOffset,
        absoluteIndex,
        (ULONGLONG)targetGpuVa,
        (ULONGLONG)entryGpuVa,
        (ULONGLONG)op->FirstPteVirtualAddress,
        op->Flags.InitialUpdate,
        op->Flags.Repeat,
        op->hAllocation,
        (ULONGLONG)op->AllocationOffsetInBytes);
    WINMALI_DIAG(
        "PTW-DIAG: level=%u index=%u rawFlags=0x%llx valid=%u zero=%u "
        "coherent=%u ro=%u nx=%u segment=%u large=%u adapter=%u "
        "ptPageSize=%u pageNumber=0x%llx resolvedPa=0x%llx "
        "encoded=0x%llx readback=0x%llx",
        op->PageTableLevel,
        absoluteIndex,
        (ULONGLONG)pte->Flags,
        (UINT)pte->Valid,
        (UINT)pte->Zero,
        (UINT)pte->CacheCoherent,
        (UINT)pte->ReadOnly,
        (UINT)pte->NoExecute,
        (UINT)pte->Segment,
        (UINT)pte->LargePage,
        (UINT)pte->PhysicalAdapterIndex,
        (UINT)pte->PageTablePageSize,
        (ULONGLONG)pte->PageTableAddress,
        (ULONGLONG)WinMaliPagingPteAddressToPhysical_(
            (UINT64)pte->PageTableAddress),
        (ULONGLONG)encoded,
        (ULONGLONG)((PUINT64)op->PageTableAddress.CpuVirtual)[absoluteIndex]);

    if (pte->Valid) {
        UINT64 pageNumber = (UINT64)pte->PageTableAddress;
        UINT64 physical = WinMaliPagingPteAddressToPhysical_(pageNumber);
        if (pageNumber == 0 || physical == 0 ||
            pageNumber > (WINMALI_LPAE_PA_MASK >> PAGE_SHIFT)) {
            WINMALI_ERROR(
                "PTW-DIAG: INVALID level=%u index=%u valid pageNumber=0x%llx resolvedPa=0x%llx",
                op->PageTableLevel, absoluteIndex,
                (ULONGLONG)pageNumber, (ULONGLONG)physical);
        }
        if (op->PageTableLevel != 0u && (encoded & 0x3ull) != 0x3ull) {
            WINMALI_ERROR(
                "PTW-DIAG: INVALID level=%u index=%u table descriptor=0x%llx type=0x%llx",
                op->PageTableLevel, absoluteIndex,
                (ULONGLONG)encoded, (ULONGLONG)(encoded & 0x3ull));
        }
    } else if (encoded != 0) {
        WINMALI_ERROR(
            "PTW-DIAG: INVALID level=%u index=%u invalid input encoded nonzero=0x%llx",
            op->PageTableLevel, absoluteIndex, (ULONGLONG)encoded);
    }
}

/* ARM-LPAE leaf encoding. See WINMALI_LPAE_L3_* in WinMaliMmu.h - we
   pick the variant per PTE based on the DXGK_PTE flags. */
static UINT64
WinMaliEncodeLpaePte_(_In_ const DXGK_PTE* pte)
{
    UINT64 attrs;

    if (!pte->Valid) {
        return 0ULL;
    }

    if (pte->ReadOnly) {
        attrs = pte->NoExecute ? WINMALI_LPAE_L3_PAGE_ATTR_RO_NX
                               : WINMALI_LPAE_L3_PAGE_ATTR_RO_EX;
    } else {
        attrs = pte->NoExecute ? WINMALI_LPAE_L3_PAGE_ATTR_RW_NX
                               : WINMALI_LPAE_L3_PAGE_ATTR_RW_EX;
    }

    /* DXGK_PTE::PageAddress is the page number (the high 52 PA bits).
       Restore its implicit zero low bits for Mali's byte-addressed PA field. */
    return WinMaliPagingPteAddressToPhysical_((UINT64)pte->PageAddress) | attrs;
}

/* Walk pPageTableEntries and write Mali LPAE entries. The GPU VA space is
   4-level 48-bit LPAE (see GPUMMUCAPS): dxgk populates level 0 with leaf page
   descriptors (real allocation pages) and levels 1..3 with TABLE descriptors
   pointing at the next-level page table (DXGK_PTE::PageTableAddress). Both
   DXGK_PTE address fields are page numbers; Mali descriptors need byte PAs. */
static NTSTATUS
WinMaliPagingUpdatePageTable_(
    _Inout_ PWINMALI_ADAPTER adapter,
    _In_    const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* op)
{
    PUINT64 pt;
    UINT    i;
    BOOLEAN leaf;
    UINT64  reservedBase = 0;
    UINT64  reservedRootChildPa = 0;
    BOOLEAN hasReservation;

    UNREFERENCED_PARAMETER(adapter);

    if (op->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL) {
        WINMALI_WARN("UpdatePageTable: unexpected UpdateMode=%d", op->UpdateMode);
        return STATUS_NOT_SUPPORTED;
    }
    if (op->PageTableAddress.CpuVirtual == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (op->Flags.Use64KBPages) {
        WINMALI_WARN("UpdatePageTable: 64KB pages unsupported (Mali LPAE 4K only)");
        return STATUS_NOT_SUPPORTED;
    }
    if (op->pPageTableEntries == NULL || op->NumPageTableEntries == 0) {
        return STATUS_SUCCESS;
    }

    pt = (PUINT64)op->PageTableAddress.CpuVirtual + op->StartIndex;
    leaf = (op->PageTableLevel == 0);
    hasReservation = WinMaliKmdGetProcessReservedPageTable(
        op->hProcess, &reservedBase, &reservedRootChildPa);

    for (i = 0; i < op->NumPageTableEntries; ++i) {
        const DXGK_PTE* pte = &op->pPageTableEntries[op->Flags.Repeat ? 0 : i];
        UINT64 encoded;
        if (leaf) {
            encoded = WinMaliEncodeLpaePte_(pte);
        } else if (pte->Valid) {
            /* Non-leaf: valid LPAE table descriptor -> next-level PT phys addr.
               Bits[1:0]=0b11 = valid table descriptor (see WinMaliMmu.c). */
            encoded = WinMaliPagingPteAddressToPhysical_(
                          (UINT64)pte->PageTableAddress) | 0x3ull;
        } else {
            encoded = 0ULL;
        }
        pt[i] = encoded;

        /* Dxgk's KMD reservation owns a process-root entry, not a VidMm
           level-1 entry. For our 48-bit layout root/L3 entry 1 represents
           0x8000000000. VidMm initializes that reserved entry invalid each
           time the root becomes resident; point it at our complete private
           L2->L1->L0 subtree on every such update. */
        if (op->PageTableLevel == 3 && hasReservation &&
            op->FirstPteVirtualAddress + ((UINT64)i << 39) == reservedBase) {
            if (reservedRootChildPa == 0) {
                WINMALI_ERROR(
                    "UpdatePageTable: reserved process root without private subtree process=%p va=0x%llx",
                    op->hProcess, (ULONGLONG)reservedBase);
                return STATUS_DEVICE_NOT_READY;
            }
            if (pte->Valid) {
                WINMALI_WARN(
                    "UpdatePageTable: reserved CSF root arrived VidMm-valid process=%p va=0x%llx pageTablePa=0x%llx; replacing owned entry",
                    op->hProcess,
                    (ULONGLONG)reservedBase,
                    (ULONGLONG)pte->PageTableAddress);
            }
            pt[i] = (reservedRootChildPa & WINMALI_LPAE_PA_MASK) | 0x3ull;
            encoded = pt[i];
            WINMALI_TRACE(
                "UpdatePageTable: installed process-reserved root process=%p va=0x%llx pt=%p index=%u L2Pa=0x%llx vidmmValid=%u init=%u",
                op->hProcess,
                (ULONGLONG)reservedBase,
                op->PageTableAddress.CpuVirtual,
                op->StartIndex + i,
                (ULONGLONG)reservedRootChildPa,
                pte->Valid,
                op->Flags.InitialUpdate);
        }

        /* Read back the exact descriptor after all normal/reserved overrides
           have run.  This is logging-only and intentionally does not repair
           an invalid VidMm input or reinterpret DXGK_PTE.Segment. */
        WinMaliPagingTraceDiagnosticPte_(op, pte, i, encoded);
    }
    KeMemoryBarrier();

    WINMALI_TRACE("UpdatePageTable: lvl=%u pt=%p start=%u count=%u repeat=%u alloc=%p process=%p firstPteVa=0x%llx reserved=%u",
                  op->PageTableLevel, op->PageTableAddress.CpuVirtual,
                  op->StartIndex, op->NumPageTableEntries, op->Flags.Repeat,
                  op->hAllocation, op->hProcess,
                  (ULONGLONG)op->FirstPteVirtualAddress,
                  hasReservation);
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliPagingFlushTlb_(
    _Inout_ PWINMALI_ADAPTER adapter,
    _In_    const DXGK_BUILDPAGINGBUFFER_FLUSHTLB* op)
{
    UNREFERENCED_PARAMETER(op);
    /* Mali AS_COMMAND_FLUSH_MEM hits TLB + L2; we flush every user AS we
       have bound. With per-process AS tracking we'd target only the AS
       attached to op->hProcess, but the over-flush is harmless and
       correctness > efficiency until UMD is exercising real workloads. */
    return WinMaliMmuFlushUserAsSlots(adapter);
}

/* Emit a Mali CSF "no-op" placeholder (8 bytes, all zero) into the DMA
   buffer. dxgk requires BPB to APPEND DMA INSTRUCTIONS that perform the
   requested op. We don't have a real CSF kernel-queue submit path yet, so
   we emit a zero placeholder so dxgk thinks work was queued; SubmitCommand
   then signals fence completion synchronously via the DPC. Returns FALSE
   if the buffer is too small (caller returns STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER). */
static BOOLEAN
WinMaliPagingEmitNop_(_Inout_ DXGKARG_BUILDPAGINGBUFFER* pBpb)
{
    const SIZE_T kNopBytes = 8;
    PUCHAR cursor = (PUCHAR)pBpb->pDmaBuffer;

    if (cursor == NULL || pBpb->DmaSize < kNopBytes) {
        return FALSE;
    }
    RtlZeroMemory(cursor, kNopBytes);
    pBpb->pDmaBuffer = (PVOID)(cursor + kNopBytes);
    pBpb->DmaSize   -= (UINT)kNopBytes;
    return TRUE;
}

/* Aperture pages map into the adapter-wide aperture VA window anchored at
   WINMALI_SYSMEM_GPU_BASE. We install LPAE PTEs into the current kernel AS
   (AS1) so the MCU can address them. Per-context render work uses a
   separate AS via SetRootPageTable - that path is independent.

   The anonymous struct inside DXGKARG_BUILDPAGINGBUFFER's union has no
   typedef in the 1809 WDK, so the helper takes the parent struct. */
static NTSTATUS
WinMaliPagingMapApertureSegment_(
    _Inout_ PWINMALI_ADAPTER             adapter,
    _In_    DXGKARG_BUILDPAGINGBUFFER*   pBpb)
{
    UINT64       gpuVaStart;
    PPFN_NUMBER  pfnArray;
    NTSTATUS     status = STATUS_SUCCESS;

    if (pBpb->MapApertureSegment.pMdl == NULL ||
        pBpb->MapApertureSegment.NumberOfPages == 0) {
        return STATUS_SUCCESS;
    }
    gpuVaStart = WINMALI_SYSMEM_GPU_BASE +
                 (UINT64)pBpb->MapApertureSegment.OffsetInPages * PAGE_SIZE;
    pfnArray = MmGetMdlPfnArray(pBpb->MapApertureSegment.pMdl);
    if (pfnArray == NULL) {
        WINMALI_WARN("MapApertureSegment: MmGetMdlPfnArray NULL");
        return STATUS_INVALID_PARAMETER;
    }
    /* One pass over the (scattered) MDL frames. The aperture window's L3
       tables are preallocated at MmuInit, so this cannot need memory. */
    status = WinMaliMmuMapGpuPfnRange(adapter,
                                      gpuVaStart,
                                      pfnArray + pBpb->MapApertureSegment.MdlOffset,
                                      (ULONG)pBpb->MapApertureSegment.NumberOfPages,
                                      WINMALI_LPAE_L3_PAGE_ATTR_RW_NX);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("MapApertureSegment: MapGpuPfnRange st=0x%08x", status);
        return status;
    }

    /* Stash the residency info on the allocation so the present blt
       (SubmitCommand) and BoFromAllocation (escape) can reach the pages.
       hAllocation is the WINMALI_KMD_ALLOCATION* we returned from
       CreateAllocation. The MDL is VidMm's; UNMAP clears the stash. */
    {
        PWINMALI_KMD_ALLOCATION ka =
            (PWINMALI_KMD_ALLOCATION)pBpb->MapApertureSegment.hAllocation;
        if (ka != NULL && ka->Magic == WINMALI_KMD_ALLOC_MAGIC) {
            ka->ApertureMdl       = pBpb->MapApertureSegment.pMdl;
            ka->ApertureMdlOffset = pBpb->MapApertureSegment.MdlOffset;
            ka->AperturePageCount = (ULONG)pBpb->MapApertureSegment.NumberOfPages;
            ka->GpuVa             = gpuVaStart;
        }
    }

    WINMALI_TRACE("MapApertureSegment: hAlloc=%p seg=%u off=0x%llx pages=%lu gpu_va=0x%llx",
                  pBpb->MapApertureSegment.hAllocation,
                  pBpb->MapApertureSegment.SegmentId,
                  (ULONGLONG)pBpb->MapApertureSegment.OffsetInPages,
                  (ULONG)pBpb->MapApertureSegment.NumberOfPages,
                  (ULONGLONG)gpuVaStart);
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliPagingUnmapApertureSegment_(
    _Inout_ PWINMALI_ADAPTER             adapter,
    _In_    DXGKARG_BUILDPAGINGBUFFER*   pBpb)
{
    UINT64   gpuVaStart;
    NTSTATUS status;

    if (pBpb->UnmapApertureSegment.NumberOfPages == 0) {
        return STATUS_SUCCESS;
    }
    gpuVaStart = WINMALI_SYSMEM_GPU_BASE +
                 (UINT64)pBpb->UnmapApertureSegment.OffsetInPages * PAGE_SIZE;

    /* Drop the residency stash - the MDL dies with this unmap. */
    {
        PWINMALI_KMD_ALLOCATION ka =
            (PWINMALI_KMD_ALLOCATION)pBpb->UnmapApertureSegment.hAllocation;
        if (ka != NULL && ka->Magic == WINMALI_KMD_ALLOC_MAGIC) {
            ka->ApertureMdl       = NULL;
            ka->ApertureMdlOffset = 0;
            ka->AperturePageCount = 0;
            ka->GpuVa             = 0;
        }
    }

    status = WinMaliMmuUnmapGpuRange(adapter,
                                     gpuVaStart,
                                     (ULONG)pBpb->UnmapApertureSegment.NumberOfPages);
    WINMALI_TRACE("UnmapApertureSegment: seg=%u off=0x%llx pages=%lu st=0x%08x",
                  pBpb->UnmapApertureSegment.SegmentId,
                  (ULONGLONG)pBpb->UnmapApertureSegment.OffsetInPages,
                  (ULONG)pBpb->UnmapApertureSegment.NumberOfPages, status);
    return status;
}

/* WRITE_PHYSICAL in the 1809 WDK only carries SegmentId + PhysicalAddress
   (the data lives elsewhere - we don't have a docs-supported path to it
   without WDDM 2.x extended structs). No-op success is the safe handler;
   the placeholder NOP appended below tells dxgk work was queued. */
static NTSTATUS
WinMaliPagingWritePhysical_(_In_ DXGKARG_BUILDPAGINGBUFFER* pBpb)
{
    WINMALI_TRACE("WritePhysical: seg=%u pa=0x%llx (no-op)",
                  pBpb->WritePhysical.SegmentId,
                  (ULONGLONG)pBpb->WritePhysical.PhysicalAddress.QuadPart);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DxgkDdiBuildPagingBuffer                                                  */
/* ------------------------------------------------------------------------ */

_Function_class_(DXGKDDI_BUILDPAGINGBUFFER)
NTSTATUS
APIENTRY
WinMaliKmdBuildPagingBuffer(
    IN_CONST_HANDLE               hAdapter,
    IN_PDXGKARG_BUILDPAGINGBUFFER pBpb)
{
    PWINMALI_ADAPTER adapter;
    NTSTATUS         status = STATUS_SUCCESS;

    WINMALI_TRACE("BuildPagingBufferDiag: ENTER hAdapter=%p args=%p op=%d dma=%p/%u private=%p/%u multipass=%u",
                  hAdapter, pBpb,
                  pBpb != NULL ? pBpb->Operation : -1,
                  pBpb != NULL ? pBpb->pDmaBuffer : NULL,
                  pBpb != NULL ? pBpb->DmaSize : 0,
                  pBpb != NULL ? pBpb->pDmaBufferPrivateData : NULL,
                  pBpb != NULL ? pBpb->DmaBufferPrivateDataSize : 0,
                  pBpb != NULL ? pBpb->MultipassOffset : 0);

    /* Status contract (dxgmms2): only STATUS_SUCCESS or
       STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER may leave this DDI. Anything
       else is bugcheck 0x10E subcode 0xb ("driver returned an invalid
       error code from BuildPagingBuffer"). Even the degenerate cases
       below therefore complete as no-op SUCCESS with a loud log. */
    if (pBpb == NULL) {
        WINMALI_ERROR("BuildPagingBuffer: pBpb NULL - no-op SUCCESS (0x10E_b contract)");
        WINMALI_TRACE("BuildPagingBufferDiag: EXIT status=0x%08x", STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        WINMALI_ERROR("BuildPagingBuffer: bad hAdapter - no-op SUCCESS (0x10E_b contract)");
        WINMALI_TRACE("BuildPagingBufferDiag: EXIT status=0x%08x", STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    switch (pBpb->Operation) {
    case DXGK_OPERATION_UPDATE_PAGE_TABLE:
        status = WinMaliPagingUpdatePageTable_(adapter, &pBpb->UpdatePageTable);
        break;

    case DXGK_OPERATION_FLUSH_TLB:
        status = WinMaliPagingFlushTlb_(adapter, &pBpb->FlushTlb);
        break;

    case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
        status = WinMaliPagingMapApertureSegment_(adapter, pBpb);
        break;

    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
        status = WinMaliPagingUnmapApertureSegment_(adapter, pBpb);
        break;

    case DXGK_OPERATION_WRITE_PHYSICAL:
        status = WinMaliPagingWritePhysical_(pBpb);
        break;

    case DXGK_OPERATION_TRANSFER:
    case DXGK_OPERATION_FILL:
    case DXGK_OPERATION_DISCARD_CONTENT:
    case DXGK_OPERATION_READ_PHYSICAL:
    case DXGK_OPERATION_SPECIAL_LOCK_TRANSFER:
    case DXGK_OPERATION_VIRTUAL_TRANSFER:
    case DXGK_OPERATION_VIRTUAL_FILL:
    case DXGK_OPERATION_INIT_CONTEXT_RESOURCE:
    case DXGK_OPERATION_NOTIFY_RESIDENCY:
    case DXGK_OPERATION_UPDATE_CONTEXT_ALLOCATION:
    case DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES:
        /* No-op success. Once UMD is exercising real workloads we'll
           need to flesh out the relevant ops; for now they're harmless. */
        WINMALI_TRACE("BuildPagingBuffer: op=%d (no-op)", pBpb->Operation);
        status = STATUS_SUCCESS;
        break;

    default:
        WINMALI_WARN("BuildPagingBuffer: unknown op=%d", pBpb->Operation);
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    /* Status sanitizer (bugcheck 0x10E_b contract, see top of function):
       any internal failure is logged LOUDLY and completed as a no-op
       SUCCESS. Worst case is a diagnosable GPU page fault later - never
       a VidMm bugcheck loop. */
    if (!NT_SUCCESS(status)) {
        WINMALI_ERROR("BuildPagingBuffer: op=%d failed 0x%08x - completing as "
                      "no-op SUCCESS (0x10E_b contract)",
                      pBpb->Operation, status);
        status = STATUS_SUCCESS;
    }

    /* Append the placeholder NOP ONLY when the op actually carries a DMA
       buffer. Ops completed CPU-side (MAP/UNMAP_APERTURE_SEGMENT in
       particular) often arrive with pDmaBuffer == NULL / DmaSize == 0;
       returning INSUFFICIENT_DMA_BUFFER for those is fatal: dxgmms2
       responds by flushing the in-flight paging buffer and re-acquiring
       a bigger one, and with no buffer in flight the re-acquire leaves
       the per-node current-DMA-buffer slot NULL - the retry's flush then
       AVs (bugcheck 0x7E in VIDMM_GLOBAL::FlushPagingBufferInternal).
       No DMA buffer => the op is already done => plain SUCCESS. */
    if (pBpb->pDmaBuffer != NULL && pBpb->DmaSize != 0) {
        if (!WinMaliPagingEmitNop_(pBpb)) {
            WINMALI_WARN("BuildPagingBuffer: DMA buffer too small for NOP (op=%d size=%u)",
                          pBpb->Operation, pBpb->DmaSize);
            WINMALI_TRACE("BuildPagingBufferDiag: EXIT status=0x%08x",
                          STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
    }
    WINMALI_TRACE("BuildPagingBufferDiag: EXIT op=%d status=0x%08x dmaRemaining=%u multipass=%u",
                  pBpb->Operation, status, pBpb->DmaSize,
                  pBpb->MultipassOffset);
    return status;
}

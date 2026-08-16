/*
 * WinMaliBo.h - BO (buffer object) lifecycle types and dispatch surface.
 *
 * A BO is a chunk of GPU-addressable sysmem pages tracked by a u32 handle
 * exposed to the UMD via WinMaliEscape ops 5/14/16/0x86. It mirrors
 * panthor's drm_panthor_bo handle model:
 *
 *   BoCreate(size, flags) -> handle           (Op 5)
 *   BoDestroy(handle)                         (Op 14)
 *   BoQueryInfo(handle) -> {size, flags, vm}  (Op 16)
 *   BoMapCpu(handle, prot) -> user VA         (Op 0x86)
 *
 * Pages come from MmAllocatePagesForMdlEx; the MDL is what we keep around.
 * GPU mappings are installed via WinMaliMmuMapGpuRange when a VmBind
 *
 * Handle table is per-adapter, a LIST_ENTRY chain protected by a KSPIN_LOCK.
 * Lookup is O(N) - fine for the dozens of BOs a typical UMD process holds;
 * if N grows we can switch to a hash table.
 */

#pragma once

#include <ntddk.h>

struct _WINMALI_ADAPTER;
typedef struct _WINMALI_ADAPTER WINMALI_ADAPTER, *PWINMALI_ADAPTER;

#define WINMALI_BO_MAGIC    'oBmW'  /* 'WmBo' little-endian */

typedef struct _WINMALI_BO {
    LIST_ENTRY          Link;
    ULONG               Magic;
    ULONG               Handle;          /* 1-based; 0 is "no handle" */
    SIZE_T              Size;            /* page-aligned bytes */
    ULONG               Flags;           /* WINMALI_BO_FLAG_* */
    ULONG               ExclusiveVmId;   /* 0 = shareable */
    PMDL                Mdl;             /* MmAllocatePagesForMdlEx output, or a
                                            hand-built PFN MDL when PagesImported */
    BOOLEAN             PagesImported;   /* pages belong to a dxgk allocation -
                                            free the MDL shell only, never the pages */
    PVOID               UserVa;          /* set by BoMapCpu; NULL otherwise */
    PEPROCESS           UserVaProcess;   /* process the UserVa belongs to */
    /* Set only by BoCreateFromUserVa: the MmProbeAndLockPages MDL that pins the
       user pages this BO wraps. Held for the BO's whole life - dropping it early
       would let the pages move out from under a live GPU mapping. Freed with
       MmUnlockPages + IoFreeMdl (NOT ExFreePool - IoAllocateMdl owns it). */
    PMDL                ProbeMdl;
    KMUTEX              Lock;            /* serializes map/unmap/destroy */
    LONG                RefCount;        /* future: GPU mapping refs */
    /* dxgk device (WINMALI_KMD_DEVICE*) whose escape created this BO;
       DestroyDevice runs down un-destroyed BOs so dead UMD processes
       don't leak pages. */
    PVOID               OwnerDevice;
    /* Diagnostic - set by BoSetLabel, optional. */
    CHAR                Label[64];
} WINMALI_BO, *PWINMALI_BO;

/* Per-adapter handle table state. Embedded in WINMALI_ADAPTER. */
typedef struct _WINMALI_BO_TABLE {
    KSPIN_LOCK          Lock;
    LIST_ENTRY          Head;
    /* DIAGNOSTIC ONLY - handles are minted by WinMaliBoAllocHandle_() from a
       driver-global counter, not from here.

       This field used to be the allocator and its comment claimed "monotonic;
       never reuses". That was true only WITHIN one adapter instance: the table
       is re-initialised on every adapter start, so handles restarted at 1 while
       UMD processes (dwm, LogonUI, explorer) lived on across restarts and kept
       BOs from the previous instance. panfrost indexes its BO arrays BY HANDLE
       and is built -DNDEBUG, so the collision silently aliased two BOs instead
       of tripping an assert. The comment is why nobody looked here. */
    LONG                NextHandle;      /* snapshot of the global at init */
    LONG                BoCount;
} WINMALI_BO_TABLE, *PWINMALI_BO_TABLE;

VOID WinMaliBoTableInit(_Out_ PWINMALI_BO_TABLE Table);
VOID WinMaliBoTableTeardown(_Inout_ PWINMALI_ADAPTER Adapter,
                            _Inout_ PWINMALI_BO_TABLE Table);

/* Create / destroy / lookup. Returns 0 on success, -errno on failure. */
/* OutActualSize receives the page-granule-rounded size actually created. The
   caller MUST report that back to user mode rather than echoing the request -
   a UMD that records a larger size than the kernel allocated will suballocate
   past the end of the mapping. */
int WinMaliBoCreate(_Inout_ PWINMALI_ADAPTER Adapter,
                    _In_ SIZE_T Size,
                    _In_ ULONG Flags,
                    _In_ ULONG ExclusiveVmId,
                    _In_opt_ PVOID OwnerDevice,
                    _Out_ ULONG* OutHandle,
                    _Out_opt_ UINT64* OutActualSize);

int WinMaliBoDestroy(_Inout_ PWINMALI_ADAPTER Adapter,
                     _In_ ULONG Handle);

/* Wrap externally-owned page frames (a resident dxgk allocation's pages)
   as a BO. Copies the PFN array into a hand-built MDL, so the BO stays
   self-consistent even after the source MDL dies - but the PAGES must stay
   pinned by the caller's side (the UMD keeps the allocation locked) for as
   long as the BO is mapped anywhere. */
int WinMaliBoCreateFromPfns(_Inout_ PWINMALI_ADAPTER Adapter,
                            _In_reads_(PageCount) const PFN_NUMBER* Pfns,
                            _In_ ULONG PageCount,
                            _In_ ULONG Flags,
                            _In_opt_ PVOID OwnerDevice,
                            _Out_ ULONG* OutHandle);

/* Wrap the pages behind a USER-MODE VA - the CPU address pfnLockCb(LockEntire)
   already handed the UMD for a dxgk allocation - as a BO.
   Probe-and-locks the range in the CALLING process and keeps that MDL alive for
   the BO's lifetime, so the frames cannot move while the GPU has them mapped.

   ★ This is the only import path that works under ESCAPE submission. 0x81/0x92
   both read `ka->ApertureMdl`, which only MapApertureSegment fills in - and dxgk
   runs that solely for allocations it believes the GPU needs, which it learns
   from a DMA-buffer allocation list or MapGpuVirtualAddressCb. Escape submits
   use neither, so those surfaces are never paged in and both ops return EAGAIN
   forever. The locked CPU VA needs none of that machinery.

   ⚠ Must be called at PASSIVE_LEVEL, in the process that owns UserVa, while the
   UMD still holds the allocation locked. */
int WinMaliBoCreateFromUserVa(_Inout_ PWINMALI_ADAPTER Adapter,
                              _In_ PVOID UserVa,
                              _In_ SIZE_T Size,
                              _In_ ULONG Flags,
                              _In_opt_ PVOID OwnerDevice,
                              _Out_ ULONG* OutHandle,
                              _Out_opt_ UINT64* OutMappedBytes);

/* ---- present-source candidates published by BoCreateFromUserVa -------------
   The present worker can only read allocations that have an ApertureMdl, and
   under escape submission dxgk never creates one - so it has nothing to copy
   however correct the rendering is. A 0x93 BO already holds a probe-locked MDL
   over exactly the pages DWM composes into; these publish it.

   AcquireBest RETURNS HOLDING THE TABLE LOCK when it returns non-NULL: the
   caller must Release after it has finished reading, and Revoke (from BO free)
   blocks until then. It returns NULL already unlocked. */
VOID  WinMaliUvSrcPublish(_In_ ULONG Handle, _In_ PMDL Mdl, _In_ SIZE_T Size);
VOID  WinMaliUvSrcRevoke(_In_ ULONG Handle);
PVOID WinMaliUvSrcAcquireBest(_In_ SIZE_T NeedBytes,
                              _In_ ULONG Stride,
                              _In_ ULONG Height,
                              _Out_ ULONG* OutHandle,
                              _Out_ ULONG* OutHits);
VOID  WinMaliUvSrcRelease(VOID);

/* Destroy every BO owned by OwnerDevice (device rundown). Returns the
   number destroyed. */
ULONG WinMaliBoRundownOwner(_Inout_ PWINMALI_ADAPTER Adapter,
                            _In_ PVOID OwnerDevice);

/* Returns the BO with refcount incremented. Caller must call WinMaliBoPut
   after use. Returns NULL if handle is unknown. */
PWINMALI_BO WinMaliBoGet(_In_ PWINMALI_ADAPTER Adapter,
                        _In_ ULONG Handle);

VOID WinMaliBoPut(_In_ PWINMALI_BO Bo);

/* CPU cache maintenance for a VM's BOs around a synchronous GPU submit
   (non-coherent GPU: GpuInfo coh=0, GPU mappings are non-cacheable while the
   BO backing is MmCached). Clean=TRUE flushes CPU writes to DRAM BEFORE the
   GPU reads input (vertices/shaders/descriptors/CS); Clean=FALSE invalidates
   the CPU cache AFTER the GPU writes output so a CPU readback sees fresh data.
   Touches every BO whose ExclusiveVmId == VmId or 0 (shared, e.g. tiler heap).
   Over-maintenance (flushing all, not just inputs/outputs) is safe. */
VOID WinMaliBoFlushVm(_Inout_ PWINMALI_ADAPTER Adapter,
                      _In_ ULONG VmId,
                      _In_ BOOLEAN Clean);

/* Map the BO's pages into the calling process's user-mode address space.
   prot is OR of bits 1 (read) and 2 (write). Returns 0/-errno; writes the
   resulting UM address into *OutUserVa. The mapping persists until the BO
   is destroyed (no separate unmap op yet). */
int WinMaliBoMapCpu(_Inout_ PWINMALI_ADAPTER Adapter,
                    _In_ ULONG Handle,
                    _In_ ULONG Prot,
                    _Out_ UINT64* OutUserVa,
                    _Out_ UINT64* OutMappedBytes);

/* Set the diagnostic label. Truncated to sizeof(Bo->Label)-1 with NUL. */
int WinMaliBoSetLabel(_Inout_ PWINMALI_ADAPTER Adapter,
                      _In_ ULONG Handle,
                      _In_reads_(LabelLen) const char* Label,
                      _In_ ULONG LabelLen);

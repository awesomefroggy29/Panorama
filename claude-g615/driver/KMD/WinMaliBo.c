/*
 * WinMaliBo.c - BO lifecycle implementation.
 *
 * Pages come from MmAllocatePagesForMdlEx (sysmem, non-contiguous, pinned).
 * The resulting MDL is what we track; the GPU MMU later maps individual
 * PFNs via VmBind/UPDATE_PAGE_TABLE. CPU mappings are produced on demand
 * via MmMapLockedPagesSpecifyCache(UserMode).
 *
 * Handle allocation is a monotonic counter - handles are never reused, so
 * a UMD that holds a stale handle gets a clean -ENOENT rather than
 * accidentally aliasing a fresh BO.
 */

#include "WinMaliKmd.h"
#include "WinMaliBo.h"

/* Local copy of Linux errnos (same values as WinMaliEscape.c). */
#define WMERR_OK          0
#define WMERR_EINVAL      -22
#define WMERR_ENOMEM      -12
#define WMERR_ENOENT      -2
#define WMERR_EBUSY       -16
#define WMERR_EACCES      -13
#define WMERR_EFAULT      -14

/*
 * ⛔⛔ BO HANDLES MUST NOT RESTART WHEN THE ADAPTER RESTARTS.
 *
 * This counter is deliberately DRIVER-GLOBAL, not per-BO-table. The table used
 * to seed NextHandle = 1 on every WinMaliBoTableInit, i.e. on every adapter
 * start - and UMD PROCESSES OUTLIVE ADAPTER RESTARTS (a dwm.exe was observed
 * still running 27 HOURS and dozens of adapter restarts later).
 *
 * The consequence was silent memory corruption, not a clean failure:
 *   * a surviving UMD still holds pan_kmod_bo objects with handles 1..N from
 *     the previous adapter instance;
 *   * the new instance hands out 1..N again;
 *   * panfrost indexes BOTH util_sparse_array handle_to_bo (pan_kmod.c) and
 *     pan_lookup_bo (pan_bo.c) BY HANDLE, and mesa is built -DNDEBUG, so the
 *     `assert(*slot == NULL)` guarding exactly this case is compiled out and
 *     the new BO silently overwrites the old slot;
 *   * a pan_kmod_bo then carries one allocation's size with another's kernel
 *     BO. Measured: "BO handle=71 claims size=32768 but only 4096 bytes are
 *     mapped", with the UMD's cpu pointer matching BoMapCpu's userVa exactly
 *     for a BO the kernel had created as 4096 bytes.
 *
 * Downstream that is a short mapping the pool refuses, so descriptor
 * allocation fails, nothing renders (black desktop), and callers that skip the
 * NULL check take an access violation - the observed c0000005 in LogonUI.exe
 * and Explorer.EXE inside WinMaliUmd.dll.
 *
 * Making the namespace unique per BOOT rather than per adapter instance turns
 * that corruption into a clean ENOENT: a stale handle now resolves to nothing
 * instead of to somebody else's BO.
 *
 * Handles must stay SMALL and DENSE - panfrost sizes a per-batch array to
 * handle+1 and refuses anything >= PANFROST_MAX_BO_HANDLE (1<<20). A plain
 * continuing counter satisfies that: it advances only with real allocations
 * (~100 per adapter instance), not with restarts.
 */
static volatile LONG g_WinMaliBoHandleNext = 1;  /* 0 = "invalid" */

/* One in a million, literally: warn before we reach the value at which
   panfrost starts refusing handles, rather than after rendering breaks. */
#define WINMALI_BO_HANDLE_WARN  (900000u)

static ULONG
WinMaliBoAllocHandle_(VOID)
{
    /* ExchangeAdd returns the PRE-increment value, so the first handle is 1. */
    LONG h = InterlockedExchangeAdd(&g_WinMaliBoHandleNext, 1);
    if ((ULONG)h == WINMALI_BO_HANDLE_WARN) {
        WINMALI_WARN("BO handle namespace reached %u - panfrost refuses "
                     "handles >= 1<<20 (it sizes a dense per-batch array by "
                     "handle). A reboot resets this; if it is climbing fast, "
                     "BOs are being created and never destroyed.",
                     (ULONG)h);
    }
    return (ULONG)h;
}

VOID
WinMaliBoTableInit(_Out_ PWINMALI_BO_TABLE Table)
{
    KeInitializeSpinLock(&Table->Lock);
    InitializeListHead(&Table->Head);
    /* NOT reset to 1 - see g_WinMaliBoHandleNext above. Kept only so the field
       reads sensibly in a debugger; handles come from the global allocator. */
    Table->NextHandle = g_WinMaliBoHandleNext;
    Table->BoCount = 0;
}

/* Free a BO's pages + structure. Caller must have removed it from the list
   already and dropped any user mapping. */
static VOID
WinMaliBoFree_(_Inout_ PWINMALI_BO Bo)
{
    if (Bo == NULL) {
        return;
    }

    /* ⛔★★★★★★ REVOKE FIRST. BEFORE ANY UNMAP. THIS ORDER IS THE WHOLE FIX.
     *
     * This call used to sit at the BOTTOM of the function, guarding only
     * MmUnlockPages - while the system-VA teardown for a 0x93 BO
     * (MmUnmapLockedPages on Mdl->MappedSystemVa, where Mdl == ProbeMdl) ran
     * about twenty lines EARLIER, holding no lock at all. That mapped system VA
     * is exactly the `srcVa` the present worker copies from, so a BO freed
     * mid-copy pulled the mapping out from under an active RtlCopyMemory.
     * A read through an unmapped system VA is not catchable - the machine is
     * simply gone, and UFS records no dump, so it looked like a mysterious hang.
     *
     * Revoke now marks the candidate dying and BLOCKS until the worker's
     * in-flight copy releases its use-ref, so every teardown below this line is
     * unobserved by definition. Safe to call unconditionally: it no-ops for a BO
     * that was never published as a candidate. */
    WinMaliUvSrcRevoke(Bo->Handle);

    if (Bo->UserVa != NULL && Bo->Mdl != NULL) {
        /* Only safe to unmap if we're still in the process that owns the
           mapping. If not, document the leak - the process is normally
           tearing down anyway (this only happens in adapter RemoveDevice
           or when the UMD failed to destroy its BO). */
        if (PsGetCurrentProcess() == Bo->UserVaProcess) {
            __try {
                MmUnmapLockedPages(Bo->UserVa, Bo->Mdl);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                WINMALI_WARN("BoFree: MmUnmapLockedPages raised 0x%08x for handle %u",
                             GetExceptionCode(), Bo->Handle);
            }
        } else {
            WINMALI_WARN("BoFree: handle %u UserVa=%p owned by foreign process; leaking mapping",
                         Bo->Handle, Bo->UserVa);
        }
        Bo->UserVa = NULL;
        Bo->UserVaProcess = NULL;
    }
    if (Bo->Mdl != NULL) {
        if (Bo->Mdl == Bo->ProbeMdl) {
            /* BoCreateFromUserVa: Mdl and ProbeMdl are the SAME MDL, and it came
               from IoAllocateMdl - freeing it here with ExFreePoolWithTag would
               use the wrong allocator AND double-free it below. Drop any system
               mapping, then leave it to the ProbeMdl arm. */
            if (Bo->Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) {
                MmUnmapLockedPages(Bo->Mdl->MappedSystemVa, Bo->Mdl);
            }
        } else if (Bo->PagesImported) {
            /* Hand-built PFN MDL over a dxgk allocation's pages: drop any
               system mapping we created, free the MDL shell, leave the
               pages alone (VidMm owns them). */
            if (Bo->Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) {
                MmUnmapLockedPages(Bo->Mdl->MappedSystemVa, Bo->Mdl);
            }
            ExFreePoolWithTag(Bo->Mdl, WINMALI_POOL_TAG);
        } else {
            MmFreePagesFromMdl(Bo->Mdl);
            ExFreePool(Bo->Mdl);
        }
        Bo->Mdl = NULL;
    }
    if (Bo->ProbeMdl != NULL) {
        /* Revoke already ran at the TOP of this function, before any unmap -
           see the note there. It used to be here, which was too late by twenty
           lines and cost every armed run. */
        /* The user pages this BO wrapped stay pinned until here. MmUnlockPages
           finds the owning process from the MDL itself, so it is safe even when
           rundown happens on another thread. IoFreeMdl, never ExFreePool. */
        __try {
            MmUnlockPages(Bo->ProbeMdl);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            WINMALI_WARN("BoFree: MmUnlockPages raised 0x%08x for handle %u",
                         GetExceptionCode(), Bo->Handle);
        }
        IoFreeMdl(Bo->ProbeMdl);
        Bo->ProbeMdl = NULL;
    }
    Bo->Magic = 0;
    ExFreePoolWithTag(Bo, WINMALI_POOL_TAG);
}

VOID
WinMaliBoTableTeardown(_Inout_ PWINMALI_ADAPTER Adapter,
                       _Inout_ PWINMALI_BO_TABLE Table)
{
    KIRQL oldIrql;
    LIST_ENTRY freeHead;
    PLIST_ENTRY entry;

    UNREFERENCED_PARAMETER(Adapter);
    InitializeListHead(&freeHead);

    /* Steal the whole list under the lock, then free outside. */
    KeAcquireSpinLock(&Table->Lock, &oldIrql);
    while (!IsListEmpty(&Table->Head)) {
        entry = RemoveHeadList(&Table->Head);
        InsertTailList(&freeHead, entry);
    }
    Table->BoCount = 0;
    KeReleaseSpinLock(&Table->Lock, oldIrql);

    while (!IsListEmpty(&freeHead)) {
        entry = RemoveHeadList(&freeHead);
        PWINMALI_BO bo = CONTAINING_RECORD(entry, WINMALI_BO, Link);
        WinMaliBoFree_(bo);
    }
}

int
WinMaliBoCreate(_Inout_ PWINMALI_ADAPTER Adapter,
                _In_ SIZE_T Size,
                _In_ ULONG Flags,
                _In_ ULONG ExclusiveVmId,
                _In_opt_ PVOID OwnerDevice,
                _Out_ ULONG* OutHandle,
                _Out_opt_ UINT64* OutActualSize)
{
    PWINMALI_BO bo;
    PHYSICAL_ADDRESS low, high, skip;
    KIRQL oldIrql;
    SIZE_T pageAligned;

    *OutHandle = 0;
    if (OutActualSize != NULL) {
        *OutActualSize = 0;
    }
    if (Size == 0) {
        return WMERR_EINVAL;
    }
    pageAligned = (Size + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);

    bo = (PWINMALI_BO)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*bo),
                                            WINMALI_POOL_TAG);
    if (bo == NULL) {
        return WMERR_ENOMEM;
    }
    RtlZeroMemory(bo, sizeof(*bo));
    bo->Magic = WINMALI_BO_MAGIC;
    bo->Size = pageAligned;
    bo->Flags = Flags;
    bo->ExclusiveVmId = ExclusiveVmId;
    bo->RefCount = 1;
    bo->OwnerDevice = OwnerDevice;
    KeInitializeMutex(&bo->Lock, 0);

    low.QuadPart  = 0;
    high.QuadPart = MAXLONGLONG;
    skip.QuadPart = 0;
    /* MUST be MmCached to MATCH the GPU MMU mapping, which is cacheable
       (VmBind pte attrs 0x743). A v46 experiment mapped these WriteCombined
       (Normal-NC) to "fix coherency" - it REGRESSED readback to all black,
       because a CPU non-cacheable + GPU cacheable alias on the same physical
       pages is architecturally undefined on ARM64. Cached GL clear->readback
       worked (v22: 51,102,153,255), proving the cached path is coherent
       enough here; the D3D-triangle black is a separate copy/coverage issue,
       not CPU/GPU cache coherency. Keep both sides cached. */
    bo->Mdl = MmAllocatePagesForMdlEx(low, high, skip, pageAligned,
                                      MmCached,
                                      MM_ALLOCATE_FULLY_REQUIRED);
    if (bo->Mdl == NULL ||
        MmGetMdlByteCount(bo->Mdl) < pageAligned) {
        if (bo->Mdl != NULL) {
            MmFreePagesFromMdl(bo->Mdl);
            ExFreePool(bo->Mdl);
        }
        ExFreePoolWithTag(bo, WINMALI_POOL_TAG);
        return WMERR_ENOMEM;
    }

    KeAcquireSpinLock(&Adapter->BoTable.Lock, &oldIrql);
    bo->Handle = WinMaliBoAllocHandle_();  /* boot-unique, NOT per-adapter */
    InsertTailList(&Adapter->BoTable.Head, &bo->Link);
    Adapter->BoTable.BoCount++;
    KeReleaseSpinLock(&Adapter->BoTable.Lock, oldIrql);

    *OutHandle = bo->Handle;
    if (OutActualSize != NULL) {
        /* Echo the REAL size back, the way drm_panthor_bo_create does. The
           caller must record this, not its request - see WINMALI_BO_CREATE. */
        *OutActualSize = (UINT64)bo->Size;
    }
    WINMALI_TRACE("BoCreate: handle=%u size=%llu (requested %llu) flags=0x%x vm=%u",
                  bo->Handle, (ULONGLONG)bo->Size, (ULONGLONG)Size,
                  bo->Flags, bo->ExclusiveVmId);
    return WMERR_OK;
}

int
WinMaliBoCreateFromPfns(_Inout_ PWINMALI_ADAPTER Adapter,
                        _In_reads_(PageCount) const PFN_NUMBER* Pfns,
                        _In_ ULONG PageCount,
                        _In_ ULONG Flags,
                        _In_opt_ PVOID OwnerDevice,
                        _Out_ ULONG* OutHandle)
{
    PWINMALI_BO bo;
    PMDL        mdl;
    SIZE_T      mdlBytes;
    KIRQL       oldIrql;

    *OutHandle = 0;
    if (Pfns == NULL || PageCount == 0) {
        return WMERR_EINVAL;
    }

    bo = (PWINMALI_BO)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*bo),
                                            WINMALI_POOL_TAG);
    if (bo == NULL) {
        return WMERR_ENOMEM;
    }
    RtlZeroMemory(bo, sizeof(*bo));
    bo->Magic = WINMALI_BO_MAGIC;
    bo->Size = (SIZE_T)PageCount << PAGE_SHIFT;
    bo->Flags = Flags;
    bo->ExclusiveVmId = 0;   /* shareable - the caller maps it via VmBind */
    bo->RefCount = 1;
    bo->OwnerDevice = OwnerDevice;
    bo->PagesImported = TRUE;
    KeInitializeMutex(&bo->Lock, 0);

    /* Hand-built MDL describing the imported frames. MmInitializeMdl with a
       NULL base VA gives ByteOffset 0 and the right page count; marking
       MDL_PAGES_LOCKED makes MmMapLockedPages* / MmGetSystemAddressForMdlSafe
       treat it like any pinned-pages MDL (same shape MmAllocatePagesForMdlEx
       produces). The pages themselves are pinned by the UMD's LockCb on the
       source allocation, NOT by this MDL. */
    mdlBytes = sizeof(MDL) + (SIZE_T)PageCount * sizeof(PFN_NUMBER);
    mdl = (PMDL)ExAllocatePoolWithTag(NonPagedPoolNx, mdlBytes,
                                      WINMALI_POOL_TAG);
    if (mdl == NULL) {
        ExFreePoolWithTag(bo, WINMALI_POOL_TAG);
        return WMERR_ENOMEM;
    }
    RtlZeroMemory(mdl, mdlBytes);
    MmInitializeMdl(mdl, NULL, (SIZE_T)PageCount << PAGE_SHIFT);
    RtlCopyMemory(MmGetMdlPfnArray(mdl), Pfns,
                  (SIZE_T)PageCount * sizeof(PFN_NUMBER));
    mdl->MdlFlags |= MDL_PAGES_LOCKED;
    bo->Mdl = mdl;

    KeAcquireSpinLock(&Adapter->BoTable.Lock, &oldIrql);
    bo->Handle = WinMaliBoAllocHandle_();  /* boot-unique, NOT per-adapter */
    InsertTailList(&Adapter->BoTable.Head, &bo->Link);
    Adapter->BoTable.BoCount++;
    KeReleaseSpinLock(&Adapter->BoTable.Lock, oldIrql);

    *OutHandle = bo->Handle;
    WINMALI_TRACE("BoCreateFromPfns: handle=%u pages=%u flags=0x%x",
                  bo->Handle, PageCount, bo->Flags);
    return WMERR_OK;
}

/* ============================================================================
 * PRESENT-SOURCE CANDIDATES PUBLISHED BY ESCAPE 0x93
 *
 * 🎯 THE READ SIDE OF THE SAME PROBLEM.
 *
 * Getting the GPU to render into the allocation's real pages is only half of
 * it: the present worker then has to READ those pages, and every route it has
 * today goes through `ka->ApertureMdl` - the flip-source branch, the
 * address-match loop, and the aperture sweep alike. Under escape submission
 * that field is NULL on every surface, so the worker has nothing to copy from
 * no matter how correct the rendering is. That is why apWithContent has stayed
 * 0 while blits climbed into the thousands.
 *
 * A 0x93 BO already owns a probe-locked MDL over exactly the pages DWM composes
 * into. Publishing those here gives the worker a source that owes dxgk nothing.
 *
 * ⚠ LOCKING: a PASSIVE-level mutex, not a spinlock, precisely so the worker can
 * hold it across a full-framebuffer copy. Revoke (from BoFree_) then BLOCKS
 * until the copy finishes, which is what makes the MDL safe to touch outside
 * the BO's own lifetime rules. Never take a BO lock while holding this.
 * ========================================================================== */
/* 64, not 16. MEASURED 36 imports in one boot against 16 slots, and the table
   recycles round-robin when full - so DWM's OTHER full-screen backbuffers were
   being evicted by the ~13 small surfaces that qualify for a slot but can never
   be a scanout source. Losing the freshest backbuffer is indistinguishable from
   "DWM stopped composing". */
#define WINMALI_UVSRC_SLOTS 64

/* A candidate must reach at least this much vertical coverage to be considered
   "the whole screen". MEASURED 2026-08-18: ranking on liveness alone followed a
   5%-covered partial render from another process (icons on black, animating),
   while ranking on coverage alone froze on a 94%-covered DEAD buffer. Neither
   signal alone identifies the composed desktop; this gate supplies the missing
   half. 50% is deliberately loose - the real desktop measures ~94% - so a frame
   still mid-composition is not disqualified. */
#define WINMALI_UVSRC_MIN_COVERAGE 50u

typedef struct _WINMALI_UVSRC_ENT {
    ULONG  Handle;      /* 0 = empty slot */
    PMDL   Mdl;         /* the BO's ProbeMdl - valid while Handle is live */
    SIZE_T Size;
    /* ★ WHO OWNS IT. The escape runs in the UMD's own process, so this is the
       process that locked the allocation. It is the difference between scanning
       out DWM's COMPOSED frame and scanning out one pre-composition layer -
       which is exactly the shape of "full desktop but no taskbar and no cursor",
       since both are added during composition (SetPointerShape returns
       NOT_SUPPORTED, i.e. Windows composites the cursor in software). */
    ULONG  Pid;
    /* ★ IS THIS SURFACE STILL BEING WRITTEN? A checksum of the sample grid from
       the previous scan, plus how many scans saw it CHANGE.
       "Full desktop, no taskbar, no cursor, never changes" is the signature of a
       FROZEN frame - one DWM composed once early in logon (before the taskbar
       existed, before the pointer moved) and then stopped writing. A live
       composed frame changes on essentially every tick. Content alone cannot
       tell those apart; only change over time can. */
    ULONG  LastSum;
    ULONG  Scans;
    ULONG  Changes;
    /* ★ THE TICK AT WHICH THIS SURFACE LAST CHANGED - the selector that
       actually matters. MEASURED: the winning surface changed 2 times in 7027
       scans, i.e. DWM composed into it a couple of times just after logon and
       then moved on. An idle desktop barely recomposes, so "most structure" just
       locks onto whichever stale frame looks busiest. "Most RECENTLY written"
       follows DWM across its backbuffers instead. */
    ULONG  LastChangeTick;
    /* ⛔★★★★★★ THE WEDGE LIVED HERE.
     *
     * The present worker copies out of this MDL's SYSTEM VA while holding NO
     * lock (holding one across a 2 MB copy wedges DWM, which needs the same
     * mutex from its 0x93 escape). WinMaliBoFree_ meanwhile did:
     *     line 127  MmUnmapLockedPages(Mdl->MappedSystemVa, Mdl)   <-- LOCK-FREE
     *     line 147  WinMaliUvSrcRevoke(Handle)                     <-- takes mutex
     * i.e. it tore down the very system mapping the worker was reading, twenty
     * lines BEFORE the call that was supposed to make that safe. The comment on
     * the Revoke says "blocks until the present worker has finished any copy in
     * progress" - it does, but only for MmUnlockPages; the system-VA unmap had
     * already happened. Reading an unmapped system VA is an UNCATCHABLE fault:
     * hard wedge, and with no dump mode on UFS, no bugcheck is ever recorded.
     *
     * MEASURED: with the aperture fallback still live (most ticks avoided this
     * path) a run reached 203 sweeps. With the aperture path gated off - forcing
     * every tick onto 0x93 - the very next boot died after TWO redraws. Maximal
     * exposure, immediate death: exactly this race.
     *
     * InUse is a use-count taken under the mutex at selection and dropped after
     * the copy. Revoke marks Dying (so no new use can start), then WAITS for
     * InUse to fall to zero before letting BoFree_ unmap anything. */
    LONG    InUse;
    BOOLEAN Dying;
    /* Row pitch in bytes as reported by the UMD via 0x93; 0 = unknown.
       Without it the worker assumes the PANEL pitch for every candidate. */
    ULONG   Pitch;
} WINMALI_UVSRC_ENT;

/* Monotonic scan counter, the timebase for LastChangeTick. */
static ULONG g_WinMaliUvSrcTick = 0;

static KMUTEX            g_WinMaliUvSrcLock;
static LONG              g_WinMaliUvSrcReady = 0;
static WINMALI_UVSRC_ENT g_WinMaliUvSrc[WINMALI_UVSRC_SLOTS];
static ULONG             g_WinMaliUvSrcNext = 0;

static VOID
WinMaliUvSrcInit_(VOID)
{
    /* One-time, whichever adapter/thread gets here first. A second
       KeInitializeMutex on a mutex someone is waiting on would corrupt it. */
    if (InterlockedCompareExchange(&g_WinMaliUvSrcReady, 1, 0) == 0) {
        KeInitializeMutex(&g_WinMaliUvSrcLock, 0);
        RtlZeroMemory(g_WinMaliUvSrc, sizeof(g_WinMaliUvSrc));
        InterlockedExchange(&g_WinMaliUvSrcReady, 2);
    }
    while (InterlockedCompareExchange(&g_WinMaliUvSrcReady, 2, 2) != 2) {
        /* Another thread is mid-init; it only takes a few instructions. */
    }
}

/* âââââ PUBLISH THE PICTURE, NOT ANOTHER SCORE.
 *
 * Five builds were justified by winner-only counters that structurally could not
 * separate "we chose wrong" from "nothing good is on offer". The field is now
 * published as numbers (StatUvC*), and this adds the one thing numbers cannot
 * carry: an actual decimated image of ONE candidate per capture, rotating, so the
 * live-but-near-black surface dwm is writing can be LOOKED AT instead of
 * theorised about. 184x115x3 = 63,480 B, once every 64 selection ticks. */
#define WINMALI_REVOKE_SNAP_BYTES (192u * 1024u)
PUCHAR g_WinMaliRevokeSnap    = NULL;
ULONG  g_WinMaliRevokeSnapLen = 0;

#define WINMALI_CT_W 184u
#define WINMALI_CT_H 115u
static PUCHAR g_WinMaliCandThumb        = NULL;
static LONG   g_WinMaliCandThumbBo      = 0;
static LONG   g_WinMaliCandThumbPid     = 0;
static LONG   g_WinMaliCandThumbPending = 0;

/* ⛔★ PUBLISH FROM **BOTH** UNLOCK SITES.
 *
 * The first version published only from WinMaliUvSrcRelease, which the present
 * worker calls only when a winner exists. On a boot where nothing qualifies -
 * exactly the boot you most want a picture of - AcquireBest unlocks itself and
 * returns NULL, Release is never called, and the image is never written. The
 * instrument sat behind the very condition it was built to explain.
 * Callers must NOT hold the mutex: WinMaliUvSrcPublish takes it from the 0x93
 * escape path, and a 62 KB registry write under it would block dwm's escape. */
/* ⛔★★★★★ PUBLISH THE GATE'S DECISION FOR **EVERY** SLOT, NOT JUST THE WINNERS.
 *
 * MEASURED (UMD 1.1.5.3): dwm creates TWO 2944x1840 render targets - its
 * composition flip chain - each a real locked kernel allocation. The KMD at the
 * same moment reports StatUvSrcNCand = **1**. So one of dwm's two back buffers
 * never becomes a candidate, and we therefore scan out a real dwm frame that is
 * simply not the current one.
 *
 * Why the second is missing cannot be answered by the counters we have:
 * StatUvSrcTooSmall is a GLOBAL total, so it cannot say WHICH slot was rejected
 * or what it was measured against. Publish the raw inputs to the gate per slot -
 * handle, pid, size, pitch, and the byte count actually required - so the
 * decision is reconstructable instead of inferred.
 * ★ A GLOBAL REJECT COUNTER CANNOT NAME THE THING IT REJECTED. */
/* ⛔★ COVER THE WHOLE TABLE, NOT A WINDOW OVER IT.
   WINMALI_UVSRC_SLOTS is 64. At 16 this blob showed the first 16 SLOT INDICES,
   and I read "16 slots, exactly one panel-sized" as proof that dwm's second
   flip buffer is never registered. It proves nothing of the sort if the second
   one lives at index >= 16. Same shape as reading a throttle as a count. */
#define WINMALI_SLOTREC_MAX 64u
typedef struct _WINMALI_SLOTREC {
    ULONG Handle;
    ULONG Pid;
    ULONG Pages;      /* Size >> PAGE_SHIFT */
    ULONG Pitch;
    ULONG NeedPages;  /* Height * pitch, in pages - what the gate demanded */
    ULONG Rej;        /* 0=qualified 1=too small 2=map failed 3=uniform */
} WINMALI_SLOTREC;
static WINMALI_SLOTREC g_WinMaliSlotRec[WINMALI_SLOTREC_MAX];
static ULONG           g_WinMaliSlotRecN = 0;
static LONG            g_WinMaliSlotRecPending = 0;

static VOID
WinMaliSlotRecPublish_(VOID)
{
    if (InterlockedExchange(&g_WinMaliSlotRecPending, 0) == 0) { return; }
    (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"UvSlots",
            g_WinMaliSlotRec,
            (ULONG)(g_WinMaliSlotRecN * sizeof(WINMALI_SLOTREC)));
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"UvSlotsN",
            g_WinMaliSlotRecN);
}

static VOID
WinMaliCandThumbPublish_(VOID)
{
    if (InterlockedExchange(&g_WinMaliCandThumbPending, 0) == 0 ||
        g_WinMaliCandThumb == NULL) {
        return;
    }
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"CandThumbBo",
            (ULONG)g_WinMaliCandThumbBo);
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"CandThumbPid",
            (ULONG)g_WinMaliCandThumbPid);
    {
        NTSTATUS st = WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"CandThumb",
                g_WinMaliCandThumb, WINMALI_CT_W * WINMALI_CT_H * 3u);
        /* ★ A WRITE THAT CAN FAIL MUST PUBLISH ITS STATUS. CandThumbBo landed
           while CandThumb did not, and with no status there was no way to tell a
           refused write from a capture that never ran. */
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"CandThumbSt",
                (ULONG)st);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"CandThumbLen",
                WINMALI_CT_W * WINMALI_CT_H * 3u);
    }
}

static VOID
WinMaliUvSrcLock_(VOID)
{
    WinMaliUvSrcInit_();
    (VOID)KeWaitForSingleObject(&g_WinMaliUvSrcLock, Executive, KernelMode,
                                FALSE, NULL);
}

static VOID
WinMaliUvSrcUnlock_(VOID)
{
    (VOID)KeReleaseMutex(&g_WinMaliUvSrcLock, FALSE);
}

VOID
WinMaliUvSrcPublish(_In_ ULONG Handle, _In_ PMDL Mdl, _In_ SIZE_T Size,
                    _In_ ULONG SrcPitch)
{
    ULONG i, slot;

    if (Handle == 0 || Mdl == NULL || Size == 0) {
        return;
    }
    WinMaliUvSrcLock_();
    slot = WINMALI_UVSRC_SLOTS;
    for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
        if (g_WinMaliUvSrc[i].Handle == Handle) { slot = i; break; }
        if (g_WinMaliUvSrc[i].Handle == 0 && slot == WINMALI_UVSRC_SLOTS) { slot = i; }
    }
    if (slot == WINMALI_UVSRC_SLOTS) {
        /* ⚠ FULL: EVICT A SMALL SURFACE FIRST, NEVER A FULL-SCREEN ONE.
           Blind round-robin threw away DWM's live backbuffers to make room for
           surfaces far too small to ever be a scanout source. Only if every slot
           holds a big surface do we fall back to round-robin. */
        /* ⛔★★★★★ THE FALLBACK WAS INVERTED, AND IT THREW AWAY dwm's FRAMES.
         *
         * The size guard above only fires when the INCOMING surface is bigger
         * than the smallest resident one. When a tiny scratch surface arrives at
         * a full table, `smallest.Size < Size` is FALSE, so it fell through to
         * blind round-robin - which happily evicted a 21 MB full-screen buffer to
         * make room for 4 KB of scratch. Exactly backwards, and it is why dwm's
         * two panel-sized flip buffers (bo=233, bo=235, both imported fine, 0
         * skipped / 0 failed) never survived in the table while ~24 churny small
         * imports did: only ONE panel-sized entry was ever left.
         *
         * Correct policy: evict the SMALLEST evictable slot, and if even that is
         * bigger than the newcomer then the NEWCOMER is the least valuable thing
         * in the room - drop it rather than displace something better.
         *
         * ⛔ AND NEVER TOUCH A SLOT THE WORKER IS READING. This function takes
         * the same mutex the present worker holds across its copy, but InUse /
         * Dying were not consulted at all, so a takeover could RtlZeroMemory a
         * slot mid-copy. That is the same class as the BoFree/Revoke race that
         * already wedged this machine once. */
        ULONG smallest = WINMALI_UVSRC_SLOTS;
        for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
            if (g_WinMaliUvSrc[i].InUse != 0 || g_WinMaliUvSrc[i].Dying) {
                continue;           /* in flight - not ours to reclaim */
            }
            if (smallest == WINMALI_UVSRC_SLOTS ||
                g_WinMaliUvSrc[i].Size < g_WinMaliUvSrc[smallest].Size) {
                smallest = i;
            }
        }
        if (smallest == WINMALI_UVSRC_SLOTS) {
            /* every slot is in use or dying: publishing would corrupt one */
            (VOID)InterlockedIncrement(&g_WinMaliStatUvSrcEvictBusy);
            WinMaliUvSrcUnlock_();
            return;
        }
        if (g_WinMaliUvSrc[smallest].Size > Size) {
            /* the newcomer is smaller than everything resident - it is the least
               plausible scanout source. Dropping it is strictly better than
               evicting a bigger one. */
            (VOID)InterlockedIncrement(&g_WinMaliStatUvSrcDropSmall);
            WinMaliUvSrcUnlock_();
            return;
        }
        slot = smallest;
        /* ★ Count evictions of a FULL-SCREEN-capable slot separately: if this
           number is ever non-zero the policy is still losing frames, and that is
           provable on the next boot instead of assumed. */
        if (g_WinMaliUvSrc[slot].Size >= (SIZE_T)WINMALI_FB_HEIGHT * WINMALI_FB_STRIDE) {
            (VOID)InterlockedIncrement(&g_WinMaliStatUvSrcEvictBig);
        }
        (VOID)InterlockedIncrement(&g_WinMaliStatUvSrcEvict);
    }
    /* A slot taken over from a DIFFERENT BO must not inherit that surface's
       liveness history, or a brand-new buffer looks freshly written. Only wipe
       on takeover - re-publishing the same handle must KEEP its history. */
    if (g_WinMaliUvSrc[slot].Handle != 0 &&
        g_WinMaliUvSrc[slot].Handle != Handle) {
        RtlZeroMemory(&g_WinMaliUvSrc[slot], sizeof(g_WinMaliUvSrc[slot]));
    }
    /* ⛔★★★★★ COUNT PANEL-CLASS PUBLISHES, TO PAIR WITH StatUvRevokeBig.
     *
     * The refcount fix was a no-op (StatBoDedup=0): dwm's two panel-sized
     * surfaces are DISTINCT allocations at distinct VAs, so nothing is closing a
     * shared alias early - dwm really does destroy them. That leaves two very
     * different worlds, and one number separates them:
     *
     *   PubBig - RevokeBig == 1  ->  dwm CHURNS surfaces; only one panel-sized
     *                                allocation ever EXISTS at a time, so the
     *                                table cannot hold two and "one flip stale"
     *                                is a consequence of dwm's lifecycle, not of
     *                                anything we do. Fix moves to the present /
     *                                flip-model contract.
     *   PubBig - RevokeBig >= 2  ->  several DO coexist and we are still losing
     *                                them somewhere between publish and scan.
     *
     * ★ WHEN TWO STORIES PREDICT DIFFERENT ARITHMETIC, PUBLISH THE ARITHMETIC. */
    if (Size >= (SIZE_T)WINMALI_FB_HEIGHT * WINMALI_FB_STRIDE) {
        (VOID)InterlockedIncrement(&g_WinMaliStatUvPubBig);
    }
    g_WinMaliUvSrc[slot].Handle = Handle;
    g_WinMaliUvSrc[slot].Mdl    = Mdl;
    g_WinMaliUvSrc[slot].Size   = Size;
    g_WinMaliUvSrc[slot].Pitch  = SrcPitch;
    g_WinMaliUvSrc[slot].Pid    = HandleToULong(PsGetCurrentProcessId());
    WinMaliUvSrcUnlock_();
}

/* Drop a use-count taken by AcquireBest/ReacquireVa. MUST be called once per
   successful acquisition, on every path, or Revoke below will stall its full
   timeout on every teardown. The worker guarantees this with a pending-use
   sweep at the top of each iteration. */
VOID
WinMaliUvSrcEndUse(_In_ ULONG Handle)
{
    ULONG i;

    if (Handle == 0 || InterlockedCompareExchange(&g_WinMaliUvSrcReady, 0, 0) == 0) {
        return;
    }
    WinMaliUvSrcLock_();
    for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
        if (g_WinMaliUvSrc[i].Handle == Handle) {
            if (g_WinMaliUvSrc[i].InUse > 0) {
                g_WinMaliUvSrc[i].InUse--;
            }
            break;
        }
    }
    WinMaliUvSrcUnlock_();
}


/* ⛔★★★★★ THE TASKBAR IS A SEPARATE LAYER, AND WE WERE DROPPING IT.
 *
 * MEASURED 2026-08-21, straight out of the UvSlots dump on a live desktop:
 *     idx=20  bo=344  pid=1252(dwm)  pages=414  pitch=11776  -> "TOO SMALL"
 * 414 pages = 1,695,744 B, and 1,695,744 / 11776 = EXACTLY 144 rows. That is
 * 2944x144 at the PANEL'S OWN PITCH - the taskbar strip, sitting in our
 * candidate table the whole time, correctly failing the full-screen size gate
 * and then discarded, because the worker composites exactly ONE candidate.
 *
 * So "no taskbar" was never a missing surface, a wrong selection, or a dwm
 * failure: we had the pixels and never drew them. AcquireBest is right to
 * reject a 144-row buffer as THE SCREEN; it is wrong to throw it away as a
 * LAYER.
 *
 * A strip is identified STRUCTURALLY, not guessed: its pitch equals the panel
 * pitch (so it spans the full width and needs no horizontal scaling) and its
 * height is a fraction of the panel. Anything at a different pitch is some
 * other surface and is left alone.
 *
 * Returns the mapped system VA and row count, having taken a use-ref the caller
 * MUST drop with WinMaliUvSrcEndUse - same contract as AcquireBest. Takes the
 * table lock only for the lookup, never across the copy (holding it there wedges
 * dwm, which needs the same mutex from the 0x93 escape path). */
PVOID
WinMaliUvSrcAcquireStrip(_In_ ULONG Stride,
                         _In_ ULONG PanelHeight,
                         _Out_ ULONG* OutHandle,
                         _Out_ ULONG* OutRows)
{
    ULONG  i;
    PVOID  bestVa   = NULL;
    ULONG  bestRows = 0;
    ULONG  bestIdx  = WINMALI_UVSRC_SLOTS;

    *OutHandle = 0;
    *OutRows   = 0;

    if (Stride == 0 || PanelHeight == 0 ||
        InterlockedCompareExchange(&g_WinMaliUvSrcReady, 0, 0) == 0) {
        return NULL;
    }

    WinMaliUvSrcLock_();
    for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
        ULONG rows;

        if (g_WinMaliUvSrc[i].Handle == 0 || g_WinMaliUvSrc[i].Mdl == NULL ||
            g_WinMaliUvSrc[i].Dying) {
            continue;
        }
        /* Full-width by construction: same bytes per row as the panel. */
        if (g_WinMaliUvSrc[i].Pitch != Stride) {
            continue;
        }
        rows = (ULONG)(g_WinMaliUvSrc[i].Size / Stride);
        /* A LAYER, not the screen: at least a few rows, strictly shorter than
           the panel (a full-height buffer is AcquireBest's job). */
        if (rows < 8u || rows >= PanelHeight) {
            continue;
        }
        /* Tallest wins - the taskbar is the biggest strip dwm publishes. */
        if (rows > bestRows) {
            bestRows = rows;
            bestIdx  = i;
        }
    }

    if (bestIdx < WINMALI_UVSRC_SLOTS) {
        bestVa = MmGetSystemAddressForMdlSafe(
                     g_WinMaliUvSrc[bestIdx].Mdl,
                     NormalPagePriority | MdlMappingNoExecute);
        if (bestVa != NULL) {
            /* ⛔★★★★★ INVALIDATE BEFORE READING - THE GPU IS NON-COHERENT.
             *
             * The scan path above already does this for the desktop candidate;
             * this one did not, and the result was EXACTLY what that predicts:
             * USER-OBSERVED 2026-08-21, the strip composited in the right place
             * at the right size (bo=368, 144 rows, 2436 blits) but rendering
             * BLACK - i.e. we read CPU cache lines the GPU's writes never
             * invalidated, which are zeros.
             * ★ A NEW READER OF GPU-WRITTEN MEMORY INHERITS EVERY COHERENCY
             *   RULE THE OLD ONE HAD; copying the selection logic without the
             *   invalidate copies half the contract. */
            if (g_WinMaliUvSrcInvalidate) {
                KeFlushIoBuffers(g_WinMaliUvSrc[bestIdx].Mdl, TRUE, TRUE);
            }

            /* ⛔ NEVER PAINT AN EMPTY LAYER OVER A GOOD FRAME.
             *
             * If the strip is uniformly black - not yet painted, or the wrong
             * buffer - compositing it REPLACES a correct region of the desktop
             * with black, and because the desktop copy is chunked across several
             * wakes the panel then alternates between the two. That is precisely
             * the reported "flickering between black and the background".
             * Refusing a blank layer degrades to the previous behaviour (no
             * taskbar) instead of actively damaging the frame. */
            {
                const UCHAR *px = (const UCHAR *)bestVa;
                ULONG        k, nb = 0;
                for (k = 0; k < 256u; k++) {
                    /* Spread the samples over the whole strip, not one row. */
                    SIZE_T off = ((SIZE_T)bestRows * Stride / 256u) * k;
                    if (off + 3u >= (SIZE_T)bestRows * Stride) { break; }
                    if (px[off] != 0 || px[off + 1] != 0 || px[off + 2] != 0) {
                        nb++;
                    }
                }
                (VOID)InterlockedExchange(&g_WinMaliStatStripNB, (LONG)nb);
                if (nb == 0) {
                    bestVa   = NULL;   /* blank - leave the desktop alone */
                    bestRows = 0;
                }
            }
        }
        if (bestVa != NULL) {
            /* Use-ref under the lock we still hold, so Revoke waits instead of
               letting BoFree unmap what we are about to read. */
            g_WinMaliUvSrc[bestIdx].InUse++;
            *OutHandle = g_WinMaliUvSrc[bestIdx].Handle;
            *OutRows   = bestRows;
            (VOID)InterlockedExchange(&g_WinMaliStatStripBo,
                                      (LONG)g_WinMaliUvSrc[bestIdx].Handle);
            (VOID)InterlockedExchange(&g_WinMaliStatStripRows, (LONG)bestRows);
        }
    }
    WinMaliUvSrcUnlock_();
    return bestVa;
}

/* ⛔★★★★★★ MUST COMPLETE BEFORE THE CALLER UNMAPS ANYTHING.
 *
 * Marks the slot dying so no new use can begin, then waits for any in-flight
 * copy to finish. The wait is BOUNDED: a leaked use-count must not turn a BO
 * teardown into a permanent hang - that would be trading one wedge for another.
 * A 2 MB copy at write-combined speed is sub-millisecond, so 1 s is enormous
 * headroom; if it ever expires we proceed anyway (no worse than the old
 * behaviour, which never waited at all) and count it so it is visible. */
VOID
WinMaliUvSrcRevoke(_In_ ULONG Handle)
{
    ULONG i, spins;

    if (Handle == 0 || InterlockedCompareExchange(&g_WinMaliUvSrcReady, 0, 0) == 0) {
        return;
    }
    WinMaliUvSrcLock_();
    for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
        if (g_WinMaliUvSrc[i].Handle != Handle) {
            continue;
        }
        g_WinMaliUvSrc[i].Dying = TRUE;   /* no new acquisitions from here on */

        for (spins = 0; g_WinMaliUvSrc[i].InUse > 0 && spins < 200; spins++) {
            LARGE_INTEGER delay;
            WinMaliUvSrcUnlock_();        /* the user needs this mutex to finish */
            delay.QuadPart = -50000LL;    /* 5 ms, relative */
            (VOID)KeDelayExecutionThread(KernelMode, FALSE, &delay);
            WinMaliUvSrcLock_();
            if (g_WinMaliUvSrc[i].Handle != Handle) {
                break;                    /* slot recycled underneath us */
            }
        }
        if (g_WinMaliUvSrc[i].Handle == Handle && g_WinMaliUvSrc[i].InUse > 0) {
            (VOID)InterlockedIncrement(&g_WinMaliStatUvRevokeTimeout);
        }
        if (g_WinMaliUvSrc[i].Handle == Handle) {
            /* ⛔★★★★★ A REVOKED SLOT READS AS **EMPTY**, NOT AS "DYING".
             *
             * Dying is only the transient state while BoFree waits for InUse to
             * drain; the zero below then returns the slot to the empty pool. So a
             * dump of the table shows `empty` for something that was, moments
             * ago, a live full-screen scanout candidate - which is exactly why
             * 38 empty slots and one panel-sized entry looked like "the second
             * flip buffer was never registered".
             *
             * MEASURED: dwm's two panel-sized buffers BOTH import (21,667,840 B
             * each, 0 skipped / 0 failed) and publish unconditionally, yet only
             * one is ever in the table, with the table 60% empty and eviction
             * never firing. Revocation is the only remaining way an entry leaves.
             *
             * Count it, and count full-screen ones separately: if a panel-sized
             * candidate is being revoked while dwm still owns it as a live back
             * buffer, that is the bug - same shape as the syncobj-ownership bug
             * where an import minted one handle and freed a different one. */
            if (g_WinMaliUvSrc[i].Size >=
                    (SIZE_T)WINMALI_FB_HEIGHT * WINMALI_FB_STRIDE) {
                (VOID)InterlockedIncrement(&g_WinMaliStatUvRevokeBig);
                /* ⛔★★★★★ LATCH A TRIGGERED RING SNAPSHOT.
                 *
                 * The line that says WHY this buffer is being freed is already in
                 * the DbgPrint ring - `KMT import COLLISION`, `import FAILED at
                 * vm_bind`, or `BoRundown ... owner device died`. It is simply
                 * unreadable after the fact: these revokes happen in the first
                 * seconds of boot, WinRM is not reachable until ~70 s, and the
                 * ring wraps. Two late reads showed COLLISION=0 / BoRundown=0,
                 * which proves NOTHING - the window no longer held the events.
                 *
                 * Same fix that finally caught the syncobj burst: latch here, let
                 * the present worker copy the ring into its OWN registry value on
                 * its next tick (registry I/O must not run under this mutex), and
                 * capture the FIRST occurrence only so later traffic cannot
                 * dilute it.
                 * ★ FOR A BOOT-TIME EVENT, CAPTURE MUST BE TRIGGERED BY THE EVENT,
                 *   NOT BY A LATER READ. */
                /* ⛔★★★★★ COPY THE RING **HERE**, NOT ON THE WORKER'S NEXT TICK.
                 *
                 * v1 latched here and let the present worker snapshot later. The
                 * trigger fired and captured 192 KB - and the revoke line was NOT
                 * in it, because the ring had already wrapped between the revoke
                 * and the worker's next pass (this KMD emits VmUnmap/BoDestroy/
                 * bo_free continuously). Decoupling the trigger from the event is
                 * not enough; the CAPTURE has to happen at the event too.
                 *
                 * So: memcpy the ring into a preallocated buffer right now - no
                 * registry I/O under this mutex, which is the constraint that
                 * pushed it to the worker in the first place - and let the worker
                 * merely FLUSH the buffer afterwards.
                 * ★ A SNAPSHOT IS ONLY AS TIMELY AS THE MOMENT IT IS TAKEN, NOT
                 *   THE MOMENT IT IS PUBLISHED. */
                /* ⛔ REVERTED: capturing the ring HERE ran WinMaliSerialSnapshot and a
                   pool allocation from inside WinMaliUvSrcRevoke - i.e. under the
                   UvSrc mutex, on the BoFree_ path. Suspect in the 1.1.6.x
                   FAILED_DRIVER_ENTRY regression; the counter below is enough. */
                (VOID)InterlockedCompareExchange(&g_WinMaliTrapRevokeBig, 1, 0);
                WINMALI_WARN("UvSrcRevoke: FULL-SCREEN slot revoked "
                             "bo=%u pid=%u pages=%u pitch=%u",
                             Handle, g_WinMaliUvSrc[i].Pid,
                             (ULONG)(g_WinMaliUvSrc[i].Size >> PAGE_SHIFT),
                             g_WinMaliUvSrc[i].Pitch);
            }
            (VOID)InterlockedIncrement(&g_WinMaliStatUvRevokeAny);
            RtlZeroMemory(&g_WinMaliUvSrc[i], sizeof(g_WinMaliUvSrc[i]));
        }
        break;
    }
    WinMaliUvSrcUnlock_();
}


/* ==========================================================================
 * UvPush - PUSH THE FINISHED FRAME, DO NOT LATCH IT.
 *
 * ⛔★★★★★ WHY THIS EXISTS. MEASURED 2026-08-24:
 *   dwm delivers the composed desktop by CPU COPY into a full-screen buffer,
 *   publishes it to the UvSrc table, and DESTROYS the wrapper ~1-2 ms later,
 *   12 times a second (StatUvPubBig/StatUvRevokeBig +524 per 840 worker ticks).
 *   The worker polls at 33 ms, so StatUvQual is ALWAYS 1 - only ever the one
 *   stale buffer dwm abandoned at boot, whose content never changes
 *   (StatUvC0Chg=0, and two FrameThumb grabs 45 s apart were BYTE-IDENTICAL,
 *   though the thumb itself renders a real desktop). We were reading a real
 *   frame - just a dead one. A poller cannot win this race; the producer has to
 *   hand us the frame.
 *
 * ⛔★★★★★ WHY THE DESTROY PATH AND NOWHERE ELSE.
 *   WinMaliBoDestroy has already popped this BO from the table under the
 *   spinlock and released it, so we are its SOLE owner and hold NO lock.
 *   WinMaliUvSrcRevoke has not run yet (it is inside WinMaliBoFree_), so Dying
 *   is clear and nothing is blocked on us. The pages are still probe-locked and
 *   cannot move until WE call WinMaliBoFree_. And dwm has locked, CPU-written
 *   and unlocked by now, so this is the FINISHED frame with zero lag.
 *   ⇒ Every lifetime question this investigation kept hitting is answered by
 *   the CALL SITE, not by a protocol, a pin, or a comment I would have to
 *   trust. That is the whole point - I have already shipped a use-after-free in
 *   this subsystem by trusting a comment instead of a lock.
 *
 * ⛔ WE READ THE USER VA, IN THE OWNING PROCESS - deliberately. A second kernel
 *   alias is exactly where the coherency trap lives (see the UvSrcInvalidate
 *   note above: an invalidate can discard the lines a CPU writer just dirtied).
 *   Same VA, same process, PIPT caches: no flush, no invalidate, no new PTEs.
 *
 * ⛔ Armed/Busy is a real DRAIN, not an assertion. Teardown clears Armed and
 *   waits for Busy to reach 0 before MmUnmapIoSpace.
 * ========================================================================== */
#define WINMALI_UVPUSH_MIN_GAP_100NS  (250LL * 1000LL)   /* 25 ms hard cap */

LONG    g_WinMaliUvPushBusy  = 0;
LONG64  g_WinMaliUvPushTick  = 0;   /* KeQueryInterruptTime of the last push */
LONG    g_WinMaliStatUvPush     = 0;
LONG    g_WinMaliStatUvPushRefG = 0;  /* refused: not panel-class at own pitch */
LONG    g_WinMaliStatUvPushRefP = 0;  /* refused: wrong process / no UserVa    */
LONG    g_WinMaliStatUvPushRefD = 0;  /* refused: disarmed / no ScanoutVa      */
LONG    g_WinMaliStatUvPushExc  = 0;  /* __except fired - MUST STAY 0          */
LONG    g_WinMaliStatUvPushNB   = 0;  /* last frame's non-black sample count, 0-64 */

VOID
WinMaliUvPushDrain(VOID)
{
    ULONG spins = 0;
    g_WinMaliUvPush = 0;                      /* disarm first, then drain */
    while (InterlockedCompareExchange(&g_WinMaliUvPushBusy, 0, 0) != 0 &&
           spins < 400u) {                    /* 400 x 5 ms = 2 s ceiling */
        LARGE_INTEGER d; d.QuadPart = -50000; /* 5 ms */
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &d);
        spins++;
    }
}

VOID
WinMaliUvPushFrame(_Inout_ PWINMALI_ADAPTER Adapter, _In_ PWINMALI_BO Bo)
{
    ULONG   gp, rows, r;
    SIZE_T  need;
    PUCHAR  dst;
    LONG64  now, last;

    if (g_WinMaliUvPush == 0 || Adapter == NULL || Bo == NULL) { return; }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)                   { return; }

    /* ⛔ SAME GATE AS AcquireBest - A FULL SCREEN AT THIS SURFACE'S OWN PITCH.
       Gating at the PANEL's pitch would let a wider surface through and paint
       only the top fraction of the glass, which is the "~50% fill" artefact
       this driver has already paid for once. */
    gp = (Bo->UserPitch != 0) ? Bo->UserPitch : WINMALI_FB_STRIDE;
    if (gp < WINMALI_FB_STRIDE) {
        (VOID)InterlockedIncrement(&g_WinMaliStatUvPushRefG);
        return;
    }
    need = (SIZE_T)WINMALI_FB_HEIGHT * gp;
    if (Bo->Size < need) {
        (VOID)InterlockedIncrement(&g_WinMaliStatUvPushRefG);
        return;
    }
    if (Bo->PushVa == NULL || Bo->PushProcess != PsGetCurrentProcess()) {
        (VOID)InterlockedIncrement(&g_WinMaliStatUvPushRefP);
        return;                       /* not our address space - never touch it */
    }

    /* Hard cap on how much of dwm's escape thread this can ever consume, so a
       churning client cannot turn 12/s into 200/s. A no-op at dwm's 12/s. */
    now  = (LONG64)KeQueryInterruptTime();
    last = InterlockedCompareExchange64(&g_WinMaliUvPushTick, 0, 0);
    if (last != 0 && (now - last) < WINMALI_UVPUSH_MIN_GAP_100NS) { return; }

    (VOID)InterlockedIncrement(&g_WinMaliUvPushBusy);
    if (g_WinMaliUvPush == 0 || Adapter->ScanoutVa == NULL) {
        (VOID)InterlockedIncrement(&g_WinMaliStatUvPushRefD);
        (VOID)InterlockedDecrement(&g_WinMaliUvPushBusy);
        return;
    }

    rows = WINMALI_FB_HEIGHT;
    /* BOTH bounds PROVEN, not clamped:
         src last byte = (rows-1)*gp + FB_STRIDE <= rows*gp = need <= Bo->Size
         dst last byte = rows*FB_STRIDE, checked against ScanoutBytes here. */
    if ((SIZE_T)rows * WINMALI_FB_STRIDE > Adapter->ScanoutBytes) {
        (VOID)InterlockedIncrement(&g_WinMaliStatUvPushRefD);
        (VOID)InterlockedDecrement(&g_WinMaliUvPushBusy);
        return;
    }
    dst = (PUCHAR)Adapter->ScanoutVa;

    /* ⛔★★★ IS THERE ANYTHING IN IT? The push demonstrably WORKS (9.5 frames/s
     * reach the panel, EXC=0) and the panel is BLACK, so the frames themselves
     * are blank. Sample before copying: an 8x8 grid of 32-bit pixels, 64 reads,
     * negligible next to a 21 MB copy. A pixel counts as painted if any of its
     * low 24 bits are set - alpha is ignored, because a fully transparent-but-
     * coloured surface is still content and a 0x00000000 surface is not.
     * ⛔ Inside the SAME __try as the copy: PushVa is the producer's VA and a
     *    stale one must not fault us here either. */
    {
        ULONG nb = 0, gx, gy;
        __try {
            for (gy = 0; gy < 8u; gy++) {
                ULONG yy = (WINMALI_FB_HEIGHT / 9u) * (gy + 1u);
                const UCHAR *rowp = (const UCHAR *)Bo->PushVa + (SIZE_T)yy * gp;
                for (gx = 0; gx < 8u; gx++) {
                    ULONG xx = (WINMALI_FB_WIDTH / 9u) * (gx + 1u);
                    ULONG px = *(const ULONG *)(rowp + (SIZE_T)xx * 4u);
                    if ((px & 0x00FFFFFFu) != 0u) { nb++; }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            (VOID)InterlockedIncrement(&g_WinMaliStatUvPushExc);
            (VOID)InterlockedDecrement(&g_WinMaliUvPushBusy);
            return;
        }
        (VOID)InterlockedExchange(&g_WinMaliStatUvPushNB, (LONG)nb);
        if (nb == 0u) {
            /* Blank. Painting it would BLACK OUT the panel - which is exactly the
               1.1.11.3 regression. Leave the glass to the worker and refuse. */
            (VOID)InterlockedIncrement(&g_WinMaliStatUvPushRefG);
            (VOID)InterlockedDecrement(&g_WinMaliUvPushBusy);
            return;
        }
    }

    __try {
        for (r = 0; r < rows; r++) {
            RtlCopyMemory(dst + (SIZE_T)r * WINMALI_FB_STRIDE,
                          (const UCHAR *)Bo->PushVa + (SIZE_T)r * gp,
                          WINMALI_FB_STRIDE);
        }
        (VOID)InterlockedExchange64(&g_WinMaliUvPushTick, now);
        (VOID)InterlockedIncrement(&g_WinMaliStatUvPush);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Probe-locked pages cannot fault; this catches a stale/hostile VA only.
           If it EVER fires the model is wrong - grade it as a STOP, not a nit. */
        (VOID)InterlockedIncrement(&g_WinMaliStatUvPushExc);
    }
    (VOID)InterlockedDecrement(&g_WinMaliUvPushBusy);
}

/* Is "prefer a candidate that is actually being written" enabled?
   Registry knob (UvPreferLive, default 1) so a bad first trial can be undone
   over WinRM without a rebuild - this changes which surface the panel shows,
   so it is exactly the sort of change that must be revertible. Cached: this is
   consulted per candidate per tick. */
static BOOLEAN
WinMaliUvPreferLive_(VOID)
{
    static LONG s_cached = -1;
    LONG v = InterlockedCompareExchange(&s_cached, -1, -1);

    if (v < 0) {
        ULONG r = 1;
        (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"UvPreferLive", 1, &r);
        v = (r != 0) ? 1 : 0;
        (VOID)InterlockedExchange(&s_cached, v);
    }
    return (v != 0) ? TRUE : FALSE;
}
PVOID
WinMaliUvSrcAcquireBest(_In_ SIZE_T NeedBytes,
                        _In_ ULONG Stride,
                        _In_ ULONG Height,
                        _Out_ ULONG* OutHandle,
                        _Out_ ULONG* OutHits,
                        _Out_opt_ ULONG* OutPitch,
                        _Out_opt_ SIZE_T* OutSize)
{
    ULONG  i, nCand = 0, bestPid = 0, bestChg = 0, bestTick = 0;
    PVOID  bestVa = NULL;
    ULONG  bestHits = 0;
    SIZE_T bestSize = 0;
    ULONG  bestIdx = WINMALI_UVSRC_SLOTS;
    ULONG  bestCoverage = 0;   /* winner's painted extent, percent of its rows */
    /* ⛔ THE WINNER'S COVERAGE ALONE CANNOT SAY WHY IT IS LOW. 0% could mean
       'selection picked badly' or 'nothing better was on offer' - opposite bugs
       needing opposite fixes. maxCoverage is the best coverage ANY candidate had
       this tick, so the pair splits them: max high + best low = we chose wrong;
       both low = the composed frame is not among the candidates at all. */
    ULONG  maxCoverage = 0;
    /* ⛔ SAME SPLIT AS maxCoverage, FOR CHANGE-COUNT - and this is the pair that
       decides the taskbar question. bestChg alone is ambiguous: 0 could mean
       "we picked a dead buffer while a live one was on offer" or "nothing on
       offer is being written at all". Those need opposite fixes (change the
       selector vs. find out where dwm actually composes). maxChanges is the best
       change-count ANY candidate had this tick, so:
          max high + best low -> we chose wrong
          both zero          -> the composed frame is NOT among the candidates */
    ULONG  maxChanges = 0;
    ULONG  nQual = 0;   /* candidates that passed the size gate THIS tick */
    ULONG  recN = 0;

    *OutHandle = 0;
    *OutHits   = 0;
    if (OutPitch != NULL) { *OutPitch = 0; }
    if (OutSize  != NULL) { *OutSize  = 0; }
    if (NeedBytes == 0) {
        return NULL;
    }
    (VOID)InterlockedIncrement(&g_WinMaliStatUvSrcAsked);

    /* Held until Release - see the locking note above. */
    WinMaliUvSrcLock_();
    g_WinMaliUvSrcTick++;
    recN = 0;   /* one complete picture of the table per sweep */

    for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
        PUCHAR base;
        ULONG  hits = 0, nonBlack = 0, y;   /* hits = TRANSITIONS, not non-black */
        ULONG  sum = 2166136261u, changes = 0, lastTick = 0;
        ULONG  lastNz = 0, coverage = 0;    /* how far down content reaches, % */
        /* â BOUNDING BOX OF NON-BLACK CONTENT. nonBlack alone cannot tell
           "a small painted rectangle" (damage-only rendering into a buffer whose
           previous content was not preserved) from "sparse content scattered over
           the whole screen" (composition running but producing black). Those need
           opposite fixes, and the count reads identically for both. */
        ULONG  firstNz = 0xFFFFFFFFu, firstCol = 0xFFFFFFFFu, lastCol = 0;
        WINMALI_SLOTREC *rec = NULL;
        ULONG  sp = 0, rows = 0;

        /* ⛔★★★★★ RECORD BEFORE EVERY `continue`, AND MAKE THE REASON A FIELD.
         *
         * This record used to live further down, inside the size-gate block -
         * i.e. AFTER the filter immediately below. So any slot that was empty,
         * Dying, or had a released Mdl was INVISIBLE to it, and I read "only one
         * panel-sized slot exists" off a dump that structurally could not show
         * the others. dwm's two flip buffers (bo=233/235) both import fine and
         * publish unconditionally, so they ARE in this table - the dump just
         * could not see them.
         *
         * Fifth instrument this session to sit behind the condition it existed to
         * measure. Record every slot, then let each rejection stamp its own
         * reason. */
        if (recN < WINMALI_SLOTREC_MAX) {
            rec = &g_WinMaliSlotRec[recN];
            rec->Handle    = g_WinMaliUvSrc[i].Handle;
            rec->Pid       = g_WinMaliUvSrc[i].Pid;
            rec->Pages     = (ULONG)(g_WinMaliUvSrc[i].Size >> PAGE_SHIFT);
            rec->Pitch     = g_WinMaliUvSrc[i].Pitch;
            rec->NeedPages = 0;
            rec->Rej       = 0;
            recN++;
        } else {
            rec = NULL;
        }

        if (g_WinMaliUvSrc[i].Handle == 0) {
            if (rec != NULL) { rec->Rej = 2; }        /* empty slot */
            continue;
        }
        if (g_WinMaliUvSrc[i].Mdl == NULL) {
            if (rec != NULL) { rec->Rej = 4; }        /* Mdl released */
            continue;
        }
        if (g_WinMaliUvSrc[i].Dying) {
            if (rec != NULL) { rec->Rej = 3; }        /* revoked - BoFree waiting */
            continue;   /* Dying: BoFree is waiting to unmap this - never adopt it */
        }
        /* ★ COUNT THE POPULATION SEPARATELY FROM THE WINNER. A bare "uvBo=0"
           cannot tell "nothing was ever published" from "published but all too
           small" from "big enough but unpainted" - three different bugs with
           three different fixes. Publish all three. */
        (VOID)InterlockedIncrement(&g_WinMaliStatUvSrcCands);
        {
            /* ⛔★★★★★ SIZE THE GATE BY THE CANDIDATE'S OWN PITCH.
             *
             * NeedBytes is the PANEL's frame size (Height x 11776). But a
             * candidate is laid out at ITS pitch, and the surfaces DWM actually
             * registers are pitch 11904 - so covering the same 1840 rows needs
             * 21,903,360 B, not 21,667,840. Comparing every candidate against a
             * single panel-derived constant asks the wrong question of anything
             * whose stride is not the panel's: it lets through a wide-pitch
             * buffer that is too SHORT to fill the screen, and (with a narrower
             * pitch) would reject one that is tall enough.
             * ⇒ Require enough bytes for a full screen AT THIS SLOT'S PITCH. */
            ULONG  gp   = (g_WinMaliUvSrc[i].Pitch != 0) ? g_WinMaliUvSrc[i].Pitch
                                                         : Stride;
            SIZE_T need = (SIZE_T)Height * gp;
            if (need < NeedBytes) { need = NeedBytes; }
            if (rec != NULL) {
                rec->Pitch     = gp;
                rec->NeedPages = (ULONG)(need >> PAGE_SHIFT);
                rec->Rej       = (g_WinMaliUvSrc[i].Size < need) ? 1u : 0u;
            }
            if (g_WinMaliUvSrc[i].Size < need) {
                (VOID)InterlockedIncrement(&g_WinMaliStatUvSrcTooSmall);
                continue;   /* cannot cover the panel at its own stride */
            }
        }
        /* ★ HOW MANY CANDIDATES ACTUALLY REACHED THE COVERAGE SCAN THIS TICK.
         *
         * Without this, maxCoverage=0 is AMBIGUOUS - it reads identically for
         * "every qualifying surface was blank" and "nothing qualified at all",
         * which are opposite problems. MEASURED: the first read of StatUvMaxCov
         * printed 0 on a tick with no winner (StatUvSrcPid/Pages both 0), and I
         * nearly took it as "the composed frame never reaches the KMD".
         * ⇒ A summary counter needs a DENOMINATOR or it is not an oracle. */
        nQual++;
        base = (PUCHAR)MmGetSystemAddressForMdlSafe(
                   g_WinMaliUvSrc[i].Mdl, NormalPagePriority | MdlMappingNoExecute);
        if (base == NULL) {
            continue;
        }

        /* ⛔ INVALIDATE BEFORE READING. THE GPU IS NON-COHERENT (coh=0).
         *
         * The GPU writes these pages through NON-CACHEABLE mappings, straight to
         * DRAM. This worker is a separate system thread that samples and copies
         * the same range ~30x a second and, until now, NEVER invalidated its own
         * cache - so once its lines held the zeroes the pages start life with,
         * it could keep reading those zeroes indefinitely no matter what the GPU
         * wrote underneath.
         *
         * That is precisely the two symptoms: the SHARED backbuffer reads
         * PERMANENTLY BLACK, and the render target reads FROZEN after 2-3
         * changes (the few moments an invalidate from a submit path happened to
         * touch those lines).
         *
         * WinMaliBoFlushVm(FALSE) exists and does this - but it is only ever
         * called from the two SUBMIT paths, in the submitting thread, never on
         * the read side. Reading non-coherent GPU-written memory without
         * invalidating is simply incorrect, whether or not it is the whole story
         * here.
         *
         * KeFlushIoBuffers(mdl, ReadOperation=TRUE, DmaToDevice=TRUE) is the
         * documented "device wrote it, CPU is about to read it" invalidate, and
         * it is the same call WinMaliBoFlushVm already uses. PASSIVE_LEVEL, and
         * cheap next to the 21 MB copy that follows. */
        /* ⚠ Now gated: correct when the GPU was the last writer, but once the
           frame is delivered by a CPU copy into these same pages an invalidate
           can discard lines that copy just dirtied, making a working delivery
           read as a frozen surface. Parameters\UvSrcInvalidate=0 flips it. */
        if (g_WinMaliUvSrcInvalidate) {
            KeFlushIoBuffers(g_WinMaliUvSrc[i].Mdl, TRUE, TRUE);
        }

        /* ⛔ DO NOT SCORE BY "COUNT OF NON-BLACK PIXELS". THAT IS WHAT PUT A
           FULLY WHITE SCREEN ON THE PANEL.
           That metric SATURATES: a blank all-white surface scores the maximum
           possible (every sample non-black), so it ties the real desktop and
           then WINS the tie for being smaller. Worse, it is non-deterministic
           across boots - the same binary showed the full desktop on one boot and
           solid white on the next, purely on which buffer existed.

           Count TRANSITIONS between adjacent samples instead. A uniform surface
           - white, black, or any flat fill - scores exactly 0 no matter how
           bright it is, while a composed desktop scores high. Same cost, and it
           measures "has structure" rather than "is not black".

           `hits` (non-black) is still computed and published, because dropping
           the old metric would make this run incomparable with every earlier
           one - but it is NO LONGER what selects. */
        {
            ULONG prev = 0;
            BOOLEAN havePrev = FALSE;
            /* ⛔★★★★★ SCAN EACH CANDIDATE AT **ITS OWN** PITCH.
             *
             * This walked every candidate at the caller's Stride - the PANEL
             * pitch, 11776 - but the surfaces DWM and Explorer actually register
             * are pitch 11904 (2976x1856, MEASURED as StatWinPitch). Sampling an
             * 11904-pitch buffer in 11776 steps drifts one row every ~92 rows, so
             * the "grid" sheared across the image and both the structure score and
             * the change-detect hash described a diagonal smear rather than the
             * frame. Every selection decision rested on that.
             * ★ The pitch has been available per-slot since the UMD started
             * reporting it; this is the code that was still assuming. */
            sp   = (g_WinMaliUvSrc[i].Pitch != 0) ? g_WinMaliUvSrc[i].Pitch
                                                   : Stride;
            rows = (ULONG)(g_WinMaliUvSrc[i].Size / sp);
            if (rows > Height) { rows = Height; }
            /* ⛔★★★★★ SAMPLE THE LAST ROW, OR THE BOX IS A CEILING.
             *
             * Stepping y += 97 over 1840 rows visits 0,97,...,1746 and NEVER the
             * bottom 93 rows. So lastNz saturates at 1746, coverage saturates at
             * (1746*100)/1840 = **94**, and the packed box saturates at
             * (1746/8)*8 = **1744** - for a COMPLETELY painted buffer.
             *
             * I read "painted rows 0..1744" off a full desktop and concluded the
             * bottom 1840-1744 = 96 rows were unpainted, which is exactly the
             * taskbar's height, and built a whole mechanism on the coincidence.
             * Both numbers were the instrument hitting its own ceiling. An older
             * note recording "coverage 94% => the bottom 6% is unpainted" is the
             * same artefact.
             *
             * ★ IF A METRIC'S MAXIMUM IS NOT THE MAXIMUM OF WHAT IT MEASURES, IT
             * WILL BE READ AS A FINDING. Visit the final row explicitly so a full
             * buffer reports the full extent. */
            for (y = 0; y < rows; ) {
                const ULONG *row = (const ULONG *)(base + (SIZE_T)y * sp);
                ULONG x;
                BOOLEAN rowNz = FALSE;
                for (x = 0; x < (sp / 4); x += 89) {
                    ULONG px = row[x] & 0x00FFFFFFu;
                    if (px != 0) {
                        nonBlack++; rowNz = TRUE;
                        if (x < firstCol) { firstCol = x; }
                        if (x > lastCol)  { lastCol  = x; }
                    }
                    if (havePrev && px != prev) { hits++; }
                    /* Order-sensitive so a permutation of the same pixels still
                       registers as a change. */
                    sum = (sum * 33u) ^ px;
                    prev = px;
                    havePrev = TRUE;
                }
                /* ★ HOW FAR DOWN THE SURFACE DOES CONTENT REACH? A fully composed
                   desktop paints to the bottom; a partial-update surface does not.
                   That distinction is the one thing size and freshness cannot make,
                   and MEASURED it is exactly what goes wrong: StatSrcLastRow=144
                   with 19 non-black rows means the latched winner held only a top
                   strip while 25,476 of 25,549 candidates were rejected on size
                   alone. */
                if (rowNz) { lastNz = y; if (firstNz == 0xFFFFFFFFu) { firstNz = y; } }
                /* advance, but never skip past the final row */
                if (y == rows - 1u) { break; }
                y += 97u;
                if (y >= rows) { y = rows - 1u; }
            }
            coverage = (rows != 0) ? ((lastNz * 100u) / rows) : 0u;
        }

        /* Liveness, tracked per slot across ticks. */
        if (g_WinMaliUvSrc[i].Scans != 0 && sum != g_WinMaliUvSrc[i].LastSum) {
            g_WinMaliUvSrc[i].Changes++;
            g_WinMaliUvSrc[i].LastChangeTick = g_WinMaliUvSrcTick;
        }
        g_WinMaliUvSrc[i].LastSum = sum;
        g_WinMaliUvSrc[i].Scans++;
        changes   = g_WinMaliUvSrc[i].Changes;
        lastTick  = g_WinMaliUvSrc[i].LastChangeTick;

        /* ★ SNAPSHOT THE WHOLE QUALIFYING FIELD, NOT JUST THE WINNER.
           "Full desktop, no taskbar, no cursor" is the signature of scanning out
           a PRE-COMPOSITION layer, and the only way to tell that from "the
           composed frame is what we have" is to see the runners-up: their owning
           PID, size and paint level. A winner alone cannot answer it. */
        if (nCand < RTL_NUMBER_OF(g_WinMaliUvCandBo)) {
            (VOID)InterlockedExchange(&g_WinMaliUvCandBo[nCand],
                                      (LONG)g_WinMaliUvSrc[i].Handle);
            (VOID)InterlockedExchange(&g_WinMaliUvCandPid[nCand],
                                      (LONG)g_WinMaliUvSrc[i].Pid);
            (VOID)InterlockedExchange(&g_WinMaliUvCandHits[nCand], (LONG)hits);
            (VOID)InterlockedExchange(&g_WinMaliUvCandNonBlack[nCand],
                                      (LONG)nonBlack);
            (VOID)InterlockedExchange(&g_WinMaliUvCandChg[nCand], (LONG)changes);
            (VOID)InterlockedExchange(&g_WinMaliUvCandScans[nCand],
                                      (LONG)g_WinMaliUvSrc[i].Scans);
            (VOID)InterlockedExchange(&g_WinMaliUvCandPages[nCand],
                                      (LONG)(g_WinMaliUvSrc[i].Size >> PAGE_SHIFT));
            /* Packed bounding box of painted content, 1 byte per edge:
               firstRow/8 | lastRow/8 | firstCol/16 | lastCol/16. */
            {
                ULONG fr = (firstNz  == 0xFFFFFFFFu) ? 0u : (firstNz  / 8u);
                ULONG lr = lastNz / 8u;
                ULONG fc = (firstCol == 0xFFFFFFFFu) ? 0u : (firstCol / 16u);
                ULONG lc = lastCol / 16u;
                if (fr > 255u) { fr = 255u; }
                if (lr > 255u) { lr = 255u; }
                if (fc > 255u) { fc = 255u; }
                if (lc > 255u) { lc = 255u; }
                (VOID)InterlockedExchange(&g_WinMaliUvCandBox[nCand],
                        (LONG)((fr << 24) | (lr << 16) | (fc << 8) | lc));
            }
            /* One candidate imaged per capture, rotating over the slots. */
            if ((g_WinMaliUvSrcTick % 64u) == 0u && sp != 0u && rows != 0u &&
                (ULONG)nCand == ((g_WinMaliUvSrcTick / 64u) % 3u)) {
                if (g_WinMaliCandThumb == NULL) {
                    g_WinMaliCandThumb = (PUCHAR)ExAllocatePoolWithTag(
                        NonPagedPoolNx, WINMALI_CT_W * WINMALI_CT_H * 3u, 'tCmW');
                }
                if (g_WinMaliCandThumb != NULL) {
                    ULONG width = sp / 4u;
                    ULONG stepX = (width > WINMALI_CT_W) ? (width / WINMALI_CT_W) : 1u;
                    ULONG stepY = (rows  > WINMALI_CT_H) ? (rows  / WINMALI_CT_H) : 1u;
                    ULONG tx, ty;
                    RtlZeroMemory(g_WinMaliCandThumb,
                                  WINMALI_CT_W * WINMALI_CT_H * 3u);
                    for (ty = 0; ty < WINMALI_CT_H; ty++) {
                        ULONG sy = ty * stepY;
                        if (sy >= rows) { break; }
                        for (tx = 0; tx < WINMALI_CT_W; tx++) {
                            ULONG sx = tx * stepX;
                            const UCHAR *pxs;
                            PUCHAR o;
                            if (sx >= width) { break; }
                            pxs = (const UCHAR *)(base + (SIZE_T)sy * sp +
                                                  (SIZE_T)sx * 4u);
                            o = g_WinMaliCandThumb +
                                ((SIZE_T)ty * WINMALI_CT_W + tx) * 3u;
                            o[0] = pxs[0]; o[1] = pxs[1]; o[2] = pxs[2];
                        }
                    }
                    g_WinMaliCandThumbBo  = (LONG)g_WinMaliUvSrc[i].Handle;
                    g_WinMaliCandThumbPid = (LONG)g_WinMaliUvSrc[i].Pid;
                    (VOID)InterlockedExchange(&g_WinMaliCandThumbPending, 1);
                }
            }
            nCand++;
        }

        /* ⛔ HARD REJECT A UNIFORM SURFACE. zero transitions = one flat colour
           across the whole grid. That is a cleared/blank buffer, never a composed
           desktop - and adopting one is exactly how the panel went solid white. */
        /* Track the best coverage on offer BEFORE any rejection, so a candidate
           that is dropped for being uniform still counts toward "was anything
           better available". */
        if (coverage > maxCoverage) { maxCoverage = coverage; }
        if (changes  > maxChanges)  { maxChanges  = changes;  }

        if (hits == 0) {
            (VOID)InterlockedIncrement(&g_WinMaliStatUvSrcUniform);
            continue;
        }

        /* ★ MOST RECENTLY WRITTEN WINS, structure only as a tie-break.
           Selecting on structure alone locks onto whichever stale frame happens
           to look busiest - measured chg=2/7027 on the winner, i.e. a frame DWM
           composed just after logon and abandoned. That is precisely why the
           taskbar (drawn later) and the cursor (moving constantly) never appear.
           Following the freshest surface tracks DWM across its backbuffers. */
        /* ⛔★★★★★★ COVERAGE FIRST, THEN FRESHNESS.
         *
         * "Most recently written wins" is right for tracking DWM across its
         * backbuffers, but on a LIVE desktop the most recently written surface is
         * usually a small partial-update one, and among the size-qualifying
         * candidates it can still be a buffer holding only a strip. MEASURED on
         * the running desktop: the winner had content to row 144 of 1820 while
         * the panel showed icons over white - a real frame underneath, and we
         * kept latching onto scratch.
         *
         * Coverage - how far down the surface content actually reaches - is the
         * one signal that separates "the composed screen" from "a piece of it",
         * and it costs nothing: the scan above already walks the rows.
         *
         * Bucketed to 10% so tiny scan-to-scan jitter cannot flip the winner and
         * reintroduce the mid-frame source changes the latch was added to stop;
         * within a bucket the freshest still wins, exactly as before. */
        {
            ULONG covBucket     = coverage / 10u;
            ULONG bestCovBucket = bestCoverage / 10u;
            /* ⛔★★★★★ LIVENESS **AND** COVERAGE - NEITHER ALONE IS THE KEY.
             *
             * Two measured failures, one in each direction:
             *
             * 1) coverage-only (<=1.1.3.3):  StatUvSrcChg=0 StatUvMaxChg=19,
             *    coverage 94. A fully-painted DEAD buffer outranked a live one,
             *    so the panel froze on the frame dwm had abandoned and the
             *    taskbar - drawn later - could never appear.
             * 2) liveness-only (1.1.3.4):    StatUvSrcChg=11 = StatUvMaxChg,
             *    but StatUvCoverage fell 94 -> 5 and StatUvSrcPid changed from
             *    dwm to another process. We faithfully followed the liveliest
             *    surface in the system: a 5%-covered partial render. The user
             *    saw shell32 icons on black, now ANIMATING instead of frozen.
             *
             * A 5%-covered surface cannot be the whole screen no matter who is
             * writing it, and a never-written surface cannot be the live screen
             * however complete it looks. So: a candidate must first be PLAUSIBLY
             * FULL-SCREEN to be eligible at all, and only among those does
             * liveness decide. Falls back to the old order if nothing clears the
             * bar, so a boot that has not composed yet still paints something. */
            BOOLEAN eligible     = (coverage     >= WINMALI_UVSRC_MIN_COVERAGE);
            BOOLEAN bestEligible = (bestCoverage >= WINMALI_UVSRC_MIN_COVERAGE);
            BOOLEAN live     = (changes > 0);
            BOOLEAN bestLive = (bestChg > 0);
            BOOLEAN better;

            if (!WinMaliUvPreferLive_()) {
                live = bestLive = FALSE;   /* knob off: exactly the old ranking */
                eligible = bestEligible = FALSE;
            }

            better =
                (bestVa == NULL) ||
                /* full-screen-ness first: a partial render is never the desktop */
                (eligible && !bestEligible) ||
                /* then liveness, among equally plausible screens */
                (eligible == bestEligible && live && !bestLive) ||
                /* then the original order, untouched */
                (eligible == bestEligible && live == bestLive &&
                 covBucket > bestCovBucket) ||
                (eligible == bestEligible && live == bestLive &&
                 covBucket == bestCovBucket && lastTick > bestTick) ||
                (eligible == bestEligible && live == bestLive &&
                 covBucket == bestCovBucket && lastTick == bestTick &&
                 hits > bestHits) ||
                (eligible == bestEligible && live == bestLive &&
                 covBucket == bestCovBucket && lastTick == bestTick &&
                 hits == bestHits && g_WinMaliUvSrc[i].Size < bestSize);
            if (!better) { continue; }
        }
        {
            bestVa    = base;
            bestHits  = hits;
            bestSize  = g_WinMaliUvSrc[i].Size;
            bestIdx   = i;
            bestCoverage = coverage;
            *OutHandle = g_WinMaliUvSrc[i].Handle;
            bestPid    = g_WinMaliUvSrc[i].Pid;
            bestChg    = changes;
            bestTick   = lastTick;
        }
    }

    /* ⚠ CLEAR THE UNUSED CANDIDATE SLOTS. They are published to the registry and
       are only overwritten up to nCand, so a tick with fewer candidates than the
       last one leaves STALE rows behind - which is exactly how a field of "1
       qualifying" printed three entries and read like three live surfaces. */
    for (i = nCand; i < RTL_NUMBER_OF(g_WinMaliUvCandBo); i++) {
        (VOID)InterlockedExchange(&g_WinMaliUvCandBo[i], 0);
        (VOID)InterlockedExchange(&g_WinMaliUvCandPid[i], 0);
        (VOID)InterlockedExchange(&g_WinMaliUvCandHits[i], 0);
        (VOID)InterlockedExchange(&g_WinMaliUvCandNonBlack[i], 0);
        (VOID)InterlockedExchange(&g_WinMaliUvCandChg[i], 0);
        (VOID)InterlockedExchange(&g_WinMaliUvCandScans[i], 0);
        (VOID)InterlockedExchange(&g_WinMaliUvCandPages[i], 0);
        (VOID)InterlockedExchange(&g_WinMaliUvCandBox[i], 0);
    }
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcChg, (LONG)bestChg);
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcPid,   (LONG)bestPid);
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcPages,
                              (LONG)(bestSize >> PAGE_SHIFT));
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcNCand, (LONG)nCand);
    /* The winner's painted extent - the new selection signal, published so a bad
       pick is visible as a number instead of only as a wrong picture. */
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcCoverage, (LONG)bestCoverage);
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcMaxCov, (LONG)maxCoverage);
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcMaxChg, (LONG)maxChanges);
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcQual, (LONG)nQual);

    g_WinMaliSlotRecN = recN;
    if (recN != 0) { (VOID)InterlockedExchange(&g_WinMaliSlotRecPending, 1); }
    *OutHits = bestHits;
    if (bestVa == NULL) {
        WinMaliUvSrcUnlock_();   /* nothing to hold the lock for */
        g_WinMaliSlotRecN = recN;
        if (recN != 0) { (VOID)InterlockedExchange(&g_WinMaliSlotRecPending, 1); }
        WinMaliCandThumbPublish_();
        WinMaliSlotRecPublish_();
    } else if (bestIdx < WINMALI_UVSRC_SLOTS) {
        if (OutPitch != NULL) { *OutPitch = g_WinMaliUvSrc[bestIdx].Pitch; }
        if (OutSize  != NULL) { *OutSize  = g_WinMaliUvSrc[bestIdx].Size; }
        /* ★ TAKE THE USE-REF UNDER THE LOCK WE STILL HOLD. From here until the
           caller's WinMaliUvSrcEndUse, WinMaliUvSrcRevoke will WAIT rather than
           let BoFree unmap the system VA the caller is about to read. */
        g_WinMaliUvSrc[bestIdx].InUse++;
    }
    return bestVa;
}

VOID
WinMaliUvSrcRelease(VOID)
{
    WinMaliUvSrcUnlock_();
    /* â REGISTRY I/O **AFTER** THE UNLOCK. WinMaliUvSrcPublish takes this same
       mutex from the 0x93 escape path, so a 63 KB write while holding it would
       block dwm inside its escape - the exact shape of the wedge that froze the
       machine when the worker held this lock across the frame copy. */
    /* ⛔ THIS is the path that actually runs. The first attempt armed the
       slot-record publish only in AcquireBest's bestVa==NULL branch - which
       almost never executes, because a candidate normally DOES qualify - so
       UvSlots read ABSENT while the sweep was recording fine. Fourth time this
       session that an instrument sat on a path that does not run. */
    if (g_WinMaliSlotRecN != 0) {
        (VOID)InterlockedExchange(&g_WinMaliSlotRecPending, 1);
    }
    WinMaliCandThumbPublish_();
    WinMaliSlotRecPublish_();
}

/* ⛔★★★★★ IS THE CANDIDATE WE SELECTED STILL ALIVE?
 *
 * The present worker used to hold this table's mutex across the whole 2 MB copy.
 * That WEDGED THE MACHINE: WinMaliUvSrcPublish takes the same mutex from the 0x93
 * ESCAPE path, so DWM blocked in its escape while we copied, and everything behind
 * DWM froze. (Symptom: the EDK2 spinner frozen while our bar kept flashing - the
 * worker was alive precisely because it owned the lock.)
 *
 * Releasing it immediately after selection fixed the wedge and introduced the
 * opposite bug: the entry can now be freed by BoFree (MmUnlockPages) WHILE we read
 * it. MEASURED 1207: StatDbgPass=18 / StatDbgDone=17 - died INSIDE pass 18 - with
 * StatUvSrcHits=0 and StatUvSrcBo=0, i.e. the candidate had gone away underneath
 * the copy.
 *
 * So: neither hold-throughout nor no-check. Re-validate BETWEEN SLICES with a
 * SHORT hold - selection stays protected, the copy does not monopolise the mutex,
 * and a candidate that disappears aborts the frame instead of faulting.
 *
 * Returns TRUE while Handle is still present with a live MDL. */
BOOLEAN
WinMaliUvSrcStillValid(_In_ ULONG Handle)
{
    ULONG   i;
    BOOLEAN ok = FALSE;

    if (Handle == 0) {
        return FALSE;
    }
    WinMaliUvSrcLock_();
    for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
        if (g_WinMaliUvSrc[i].Handle == Handle &&
            g_WinMaliUvSrc[i].Mdl    != NULL &&
            !g_WinMaliUvSrc[i].Dying) {
            ok = TRUE;
            break;
        }
    }
    WinMaliUvSrcUnlock_();
    return ok;
}

/* ⛔★★★★★ RE-RESOLVE A LATCHED CANDIDATE'S SYSTEM VA.
 *
 * AcquireBest picks a winner EVERY WAKE, and a wake copies only a 2 MB slice of
 * the 21,667,840 B frame - so a full sweep is ~11 wakes, each free to select a
 * DIFFERENT buffer. The scanout then holds strips composed at different instants
 * from different surfaces, which is precisely the reported symptom: "the lines
 * were changing places on every reprint".
 *
 * The cure is to latch ONE source for a whole sweep. That needs a way to get the
 * same VA back on later wakes without re-running the beauty contest - this.
 *
 * Returns NULL if the handle is no longer a live candidate, which is the signal
 * to abandon the sweep and re-select from the top. Takes the table lock only for
 * the lookup, never across the copy (holding it there wedges DWM). */
PVOID
WinMaliUvSrcReacquireVa(_In_ ULONG Handle, _Out_opt_ ULONG* OutPitch,
                        _Out_opt_ SIZE_T* OutSize)
{
    ULONG i;
    PVOID va = NULL;

    if (OutPitch != NULL) { *OutPitch = 0; }
    if (OutSize  != NULL) { *OutSize  = 0; }
    if (Handle == 0) {
        return NULL;
    }
    WinMaliUvSrcLock_();
    for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
        if (g_WinMaliUvSrc[i].Handle == Handle &&
            g_WinMaliUvSrc[i].Mdl    != NULL &&
            !g_WinMaliUvSrc[i].Dying) {
            /* Same mapping AcquireBest handed out: MmGetSystemAddressForMdlSafe
               is idempotent for the MDL's lifetime. */
            va = MmGetSystemAddressForMdlSafe(
                     g_WinMaliUvSrc[i].Mdl,
                     NormalPagePriority | MdlMappingNoExecute);
            if (va != NULL) {
                if (OutPitch != NULL) { *OutPitch = g_WinMaliUvSrc[i].Pitch; }
                if (OutSize  != NULL) { *OutSize  = g_WinMaliUvSrc[i].Size; }
                /* Same use-ref contract as AcquireBest - the caller is about to
                   read this VA, so Revoke must wait for it. */
                g_WinMaliUvSrc[i].InUse++;
            }
            break;
        }
    }
    WinMaliUvSrcUnlock_();
    return va;
}

/* ⛔★★★★★ ONE ALLOCATION = ONE BO = ONE SCANOUT SLOT.
 *
 * CAPTURED (triggered ring snapshot, 1.1.6.3): dwm's panel-sized imported
 * surface is destroyed by an ORDINARY alias release - mesa's pipe_resource
 * refcount reaches zero, bo_free runs, BoDestroy frees the BO, and
 * WinMaliUvSrcRevoke empties the scanout slot with it. No import collision, no
 * device rundown. It is simply that NOTHING in the chain refcounts the
 * allocation: RefCount existed but was decorative, so the FIRST close of ANY
 * alias tore out a live full-screen candidate. That is why dwm's two flip
 * buffers never coexisted in the table and the panel stayed one flip stale.
 *
 * Dedup on the identity the pages actually have - (process, UserVa, Size) - so
 * a re-import of the same allocation returns the SAME handle with a reference
 * instead of minting a second BO over the same frames.
 * ★ IF TWO NAMES CAN REFER TO ONE OBJECT, THE OBJECT NEEDS THE REFCOUNT, NOT
 *   THE NAMES. */
static PWINMALI_BO
WinMaliBoFindByUserVa_(_Inout_ PWINMALI_ADAPTER Adapter,
                       _In_ PVOID UserVa, _In_ SIZE_T Size)
{
    KIRQL       oldIrql;
    PLIST_ENTRY e;
    PWINMALI_BO hit = NULL;
    PEPROCESS   me  = PsGetCurrentProcess();

    KeAcquireSpinLock(&Adapter->BoTable.Lock, &oldIrql);
    for (e = Adapter->BoTable.Head.Flink; e != &Adapter->BoTable.Head;
         e = e->Flink) {
        PWINMALI_BO b = CONTAINING_RECORD(e, WINMALI_BO, Link);
        if (b->PagesImported && b->UserVa == UserVa &&
            b->UserVaProcess == me && b->Size == Size) {
            InterlockedIncrement(&b->RefCount);
            hit = b;
            break;
        }
    }
    KeReleaseSpinLock(&Adapter->BoTable.Lock, oldIrql);
    return hit;
}

int
WinMaliBoCreateFromUserVa(_Inout_ PWINMALI_ADAPTER Adapter,
                          _In_ PVOID UserVa,
                          _In_ SIZE_T Size,
                          _In_ ULONG Flags,
                          _In_ ULONG SrcPitch,
                          _In_opt_ PVOID OwnerDevice,
                          _Out_ ULONG* OutHandle,
                          _Out_opt_ UINT64* OutMappedBytes)
{
    PWINMALI_BO bo = NULL;
    PMDL        mdl = NULL;
    ULONG       pageCount;
    KIRQL       oldIrql;
    BOOLEAN     locked = FALSE;

    *OutHandle = 0;
    if (OutMappedBytes != NULL) {
        *OutMappedBytes = 0;
    }
    if (UserVa == NULL || Size == 0) {
        return WMERR_EINVAL;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return WMERR_EINVAL;
    }

    /* ⚠ Page alignment is not cosmetic. A BO's GPU VA is its MDL's first page,
       so a misaligned base would shift every pixel by ByteOffset bytes - the
       surface would render, just skewed, which is a far more confusing failure
       than a clean refusal. D3DKMT allocations are whole pages, so a LockCb
       pointer that is NOT page-aligned means the caller passed something other
       than lockCb.pData; say so loudly rather than papering over it. */
    if (((ULONG_PTR)UserVa & (PAGE_SIZE - 1)) != 0) {
        WINMALI_WARN("BoCreateFromUserVa: va=%p is not page aligned - refusing",
                     UserVa);
        return WMERR_EINVAL;
    }

    Size = ROUND_TO_PAGES(Size);
    pageCount = (ULONG)(Size >> PAGE_SHIFT);

    mdl = IoAllocateMdl(UserVa, (ULONG)Size, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        return WMERR_ENOMEM;
    }

    /* IoModifyAccess: the GPU renders INTO these pages. Probing for read only
       would leave them mapped copy-on-write and the GPU's writes would land in
       a private copy the scanout never sees - a black screen with every counter
       reporting success. */
    __try {
        MmProbeAndLockPages(mdl, UserMode, IoModifyAccess);
        locked = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WINMALI_WARN("BoCreateFromUserVa: probe/lock raised 0x%08x for va=%p size=%llu",
                     GetExceptionCode(), UserVa, (ULONGLONG)Size);
    }
    if (!locked) {
        IoFreeMdl(mdl);
        return WMERR_EFAULT;
    }

    bo = (PWINMALI_BO)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*bo),
                                            WINMALI_POOL_TAG);
    if (bo == NULL) {
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
        return WMERR_ENOMEM;
    }
    RtlZeroMemory(bo, sizeof(*bo));
    bo->Magic = WINMALI_BO_MAGIC;
    bo->Size = Size;
    bo->Flags = Flags;
    bo->ExclusiveVmId = 0;      /* shareable - caller maps it via VmBind */
    bo->RefCount = 1;
    bo->OwnerDevice = OwnerDevice;
    /* PagesImported: the frames belong to a dxgk allocation. BoFree_ must free
       the MDL shell only and never touch the pages - VidMm owns them. */
    bo->PagesImported = TRUE;
    bo->Mdl = mdl;
    bo->ProbeMdl = mdl;         /* same MDL: unlock+IoFreeMdl, not ExFreePool */
    KeInitializeMutex(&bo->Lock, 0);

    KeAcquireSpinLock(&Adapter->BoTable.Lock, &oldIrql);
    bo->Handle = WinMaliBoAllocHandle_();   /* boot-unique, NOT per-adapter */
    InsertTailList(&Adapter->BoTable.Head, &bo->Link);
    Adapter->BoTable.BoCount++;
    KeReleaseSpinLock(&Adapter->BoTable.Lock, oldIrql);

    /* Record the producer's OWN VA / process / pitch, in fields DEDICATED to the
       push. It is what lets the destroy-time push read the frame through DWM'S
       OWN CACHED MAPPING instead of minting a second kernel alias - which is
       where the coherency question lives, and which would also mean mapping 5290
       system PTEs per frame, the shape that hung .79.
       ⛔ NOT UserVa/UserVaProcess: those mean "BoMapCpu made a mapping and
       BoFree_ must MmUnmapLockedPages it" (WinMaliBo.c:119-136). Writing them
       here made BoFree_ unmap the PRODUCER's own address on every destroy and
       black-screened the box in 1.1.11.0, ungated by any knob. */
    bo->PushVa      = UserVa;
    bo->PushProcess = PsGetCurrentProcess();
    bo->UserPitch   = SrcPitch;

    /* Offer it to the present worker as a scanout source. Publishing every
       0x93 BO and scoring them by painted content is deliberate: nothing tells
       us WHICH surface DWM composed into (its presents are not ours and carry
       no allocation list), so identify it by the only property the primary must
       have - big enough, and painted. */
    WinMaliUvSrcPublish(bo->Handle, mdl, Size, SrcPitch);

    *OutHandle = bo->Handle;
    if (OutMappedBytes != NULL) {
        *OutMappedBytes = (UINT64)Size;
    }
    WINMALI_TRACE("BoCreateFromUserVa: handle=%u va=%p pages=%u flags=0x%x",
                  bo->Handle, UserVa, pageCount, bo->Flags);
    return WMERR_OK;
}

/* Find a BO by handle and pop it from the list under the lock. */
static PWINMALI_BO
WinMaliBoTakeByHandle_(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG Handle)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PWINMALI_BO found = NULL;

    KeAcquireSpinLock(&Adapter->BoTable.Lock, &oldIrql);
    for (entry = Adapter->BoTable.Head.Flink;
         entry != &Adapter->BoTable.Head;
         entry = entry->Flink) {
        PWINMALI_BO bo = CONTAINING_RECORD(entry, WINMALI_BO, Link);
        if (bo->Handle == Handle) {
            RemoveEntryList(&bo->Link);
            Adapter->BoTable.BoCount--;
            found = bo;
            break;
        }
    }
    KeReleaseSpinLock(&Adapter->BoTable.Lock, oldIrql);
    return found;
}

int
WinMaliBoDestroy(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG Handle)
{
    /* ⛔ REVERTED to the simple form. The refcounted variant (1.1.6.4) walked the
       BoTable under its spinlock and is the other prime suspect in the
       FAILED_DRIVER_ENTRY regression. It was ALSO measured to be a no-op
       (StatBoDedup=0: dwm's two panel buffers are distinct allocations, not
       aliases), so nothing of value is lost by removing it. */
    PWINMALI_BO bo = WinMaliBoTakeByHandle_(Adapter, Handle);
    if (bo == NULL) {
        return WMERR_ENOENT;
    }
    WINMALI_TRACE("BoDestroy: handle=%u size=%llu",
                  bo->Handle, (ULONGLONG)bo->Size);
    /* ⛔ ORDER IS THE WHOLE DESIGN: the frame is finished and the pages are
       still probe-locked and solely ours HERE. WinMaliBoFree_ is what runs
       WinMaliUvSrcRevoke and unlocks the pages, so this MUST precede it. */
    WinMaliUvPushFrame(Adapter, bo);
    WinMaliBoFree_(bo);
    return WMERR_OK;
}

ULONG
WinMaliBoRundownOwner(_Inout_ PWINMALI_ADAPTER Adapter, _In_ PVOID OwnerDevice)
{
    KIRQL oldIrql;
    LIST_ENTRY freeHead;
    PLIST_ENTRY entry, next;
    ULONG freed = 0;

    if (OwnerDevice == NULL) {
        return 0;
    }
    InitializeListHead(&freeHead);

    KeAcquireSpinLock(&Adapter->BoTable.Lock, &oldIrql);
    for (entry = Adapter->BoTable.Head.Flink;
         entry != &Adapter->BoTable.Head;
         entry = next) {
        PWINMALI_BO bo = CONTAINING_RECORD(entry, WINMALI_BO, Link);
        next = entry->Flink;
        if (bo->OwnerDevice == OwnerDevice) {
            RemoveEntryList(&bo->Link);
            Adapter->BoTable.BoCount--;
            InsertTailList(&freeHead, &bo->Link);
        }
    }
    KeReleaseSpinLock(&Adapter->BoTable.Lock, oldIrql);

    while (!IsListEmpty(&freeHead)) {
        PWINMALI_BO bo = CONTAINING_RECORD(RemoveHeadList(&freeHead),
                                           WINMALI_BO, Link);
        WINMALI_TRACE("BoRundown: handle=%u size=%llu (owner device died "
                      "without BoDestroy)", bo->Handle, (ULONGLONG)bo->Size);
        WinMaliBoFree_(bo);
        ++freed;
    }
    return freed;
}

PWINMALI_BO
WinMaliBoGet(_In_ PWINMALI_ADAPTER Adapter, _In_ ULONG Handle)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PWINMALI_BO found = NULL;

    KeAcquireSpinLock(&Adapter->BoTable.Lock, &oldIrql);
    for (entry = Adapter->BoTable.Head.Flink;
         entry != &Adapter->BoTable.Head;
         entry = entry->Flink) {
        PWINMALI_BO bo = CONTAINING_RECORD(entry, WINMALI_BO, Link);
        if (bo->Handle == Handle) {
            InterlockedIncrement(&bo->RefCount);
            found = bo;
            break;
        }
    }
    KeReleaseSpinLock(&Adapter->BoTable.Lock, oldIrql);
    return found;
}

VOID
WinMaliBoPut(_In_ PWINMALI_BO Bo)
{
    if (Bo != NULL) {
        InterlockedDecrement(&Bo->RefCount);
        /* Note: we don't auto-free at 0 here. BoDestroy is the explicit
           release path. Refcount is informational for now (used by VmBind
           to mark a BO as live-in-GPU). */
    }
}

int
WinMaliBoMapCpu(_Inout_ PWINMALI_ADAPTER Adapter,
                _In_ ULONG Handle,
                _In_ ULONG Prot,
                _Out_ UINT64* OutUserVa,
                _Out_ UINT64* OutMappedBytes)
{
    PWINMALI_BO bo;
    PVOID va;
    LOCK_OPERATION op;
    int err = WMERR_OK;

    *OutUserVa = 0;
    *OutMappedBytes = 0;
    bo = WinMaliBoGet(Adapter, Handle);
    if (bo == NULL) {
        return WMERR_ENOENT;
    }

    KeWaitForSingleObject(&bo->Lock, Executive, KernelMode, FALSE, NULL);

    if (bo->UserVa != NULL) {
        /* Already mapped in some process. We don't support remapping
           into different processes - the design assumes one UMD per BO. */
        if (bo->UserVaProcess == PsGetCurrentProcess()) {
            *OutUserVa = (UINT64)(ULONG_PTR)bo->UserVa;
            /* Report the length on THIS path too. It is the common one - the
               pool maps a BO once and every later map is a cache hit - so
               reporting only on a fresh map would leave the caller's contract
               check silently unarmed exactly where the bug was seen. */
            *OutMappedBytes = (UINT64)bo->Size;
            err = WMERR_OK;
        } else {
            err = WMERR_EBUSY;
        }
        goto out;
    }

    op = (Prot & 2) ? IoModifyAccess : IoReadAccess;
    /* MmCached to match both the page allocation and the GPU MMU mapping
       (see WinMaliBoCreate: a WriteCombined CPU alias vs the GPU's cacheable
       mapping is undefined on ARM64 and turned readback black). */
    __try {
        va = MmMapLockedPagesSpecifyCache(bo->Mdl,
                                          UserMode,
                                          MmCached,
                                          NULL,    /* OS picks address */
                                          FALSE,
                                          NormalPagePriority);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WINMALI_WARN("BoMapCpu: MmMapLockedPagesSpecifyCache raised 0x%08x",
                     GetExceptionCode());
        va = NULL;
    }

    if (va == NULL) {
        err = WMERR_ENOMEM;
        goto out;
    }
    bo->UserVa = va;
    bo->UserVaProcess = PsGetCurrentProcess();
    *OutUserVa = (UINT64)(ULONG_PTR)va;
    /* MmMapLockedPagesSpecifyCache maps exactly what the MDL describes, and
       BoCreate asserts MmGetMdlByteCount >= the page-aligned request, so
       bo->Size is the authoritative mapped length. */
    *OutMappedBytes = (UINT64)bo->Size;
    UNREFERENCED_PARAMETER(op);
    /* Publish the BO size AND the MDL's actual byte count beside the handle.
     *
     * WHY: the UMD's pool caught a BO whose CPU mapping covered only ONE page of
     * a 32 KB BO (the rest read back MEM_FREE). MmMapLockedPagesSpecifyCache
     * maps exactly what the MDL describes, so a short mapping means a short MDL
     * - but nothing ever published the MDL length, so the two sides could not be
     * compared. And correlating a UMD "size=32768" against a KMD "size=4096" by
     * handle number alone is worthless here: BoTable handles restart at 1 on
     * every device restart AND the log ring wraps, so the same handle recurs
     * across adapter instances. Printing all three on one line settles it
     * without any cross-line inference. */
    WINMALI_TRACE("BoMapCpu: handle=%u prot=0x%x imported=%u size=%llu "
                  "mdlBytes=%lu -> userVa=0x%llx",
                  Handle, Prot, (ULONG)bo->PagesImported,
                  (ULONGLONG)bo->Size,
                  bo->Mdl != NULL ? (ULONG)MmGetMdlByteCount(bo->Mdl) : 0ul,
                  *OutUserVa);

out:
    KeReleaseMutex(&bo->Lock, FALSE);
    WinMaliBoPut(bo);
    UNREFERENCED_PARAMETER(Adapter);
    return err;
}

VOID
WinMaliBoFlushVm(_Inout_ PWINMALI_ADAPTER Adapter,
                 _In_ ULONG VmId,
                 _In_ BOOLEAN Clean)
{
    KIRQL       oldIrql;
    PLIST_ENTRY entry;
    /* KeFlushIoBuffers ReadOperation: TRUE = device->memory (invalidate CPU
       cache so CPU sees GPU writes); FALSE = memory->device (clean CPU cache
       to DRAM so GPU sees CPU writes). MDL-based, so it works regardless of
       the (non-cacheable) GPU mapping attribute - alias-safe. */
    BOOLEAN readOp = Clean ? FALSE : TRUE;

    KeAcquireSpinLock(&Adapter->BoTable.Lock, &oldIrql);
    for (entry = Adapter->BoTable.Head.Flink;
         entry != &Adapter->BoTable.Head;
         entry = entry->Flink) {
        PWINMALI_BO bo = CONTAINING_RECORD(entry, WINMALI_BO, Link);
        if (bo->Mdl != NULL &&
            (bo->ExclusiveVmId == VmId || bo->ExclusiveVmId == 0)) {
            KeFlushIoBuffers(bo->Mdl, readOp, TRUE);
        }
    }
    KeReleaseSpinLock(&Adapter->BoTable.Lock, oldIrql);
}

int
WinMaliBoSetLabel(_Inout_ PWINMALI_ADAPTER Adapter,
                  _In_ ULONG Handle,
                  _In_reads_(LabelLen) const char* Label,
                  _In_ ULONG LabelLen)
{
    PWINMALI_BO bo = WinMaliBoGet(Adapter, Handle);
    ULONG copy;
    if (bo == NULL) {
        return WMERR_ENOENT;
    }
    copy = (LabelLen < sizeof(bo->Label) - 1) ? LabelLen : (ULONG)sizeof(bo->Label) - 1;
    RtlZeroMemory(bo->Label, sizeof(bo->Label));
    if (copy > 0 && Label != NULL) {
        RtlCopyMemory(bo->Label, Label, copy);
    }
    WinMaliBoPut(bo);
    return WMERR_OK;
}

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
        ULONG smallest = WINMALI_UVSRC_SLOTS;
        for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
            if (smallest == WINMALI_UVSRC_SLOTS ||
                g_WinMaliUvSrc[i].Size < g_WinMaliUvSrc[smallest].Size) {
                smallest = i;
            }
        }
        if (smallest != WINMALI_UVSRC_SLOTS &&
            g_WinMaliUvSrc[smallest].Size < Size) {
            slot = smallest;
        } else {
            slot = g_WinMaliUvSrcNext % WINMALI_UVSRC_SLOTS;
            g_WinMaliUvSrcNext++;
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
            RtlZeroMemory(&g_WinMaliUvSrc[i], sizeof(g_WinMaliUvSrc[i]));
        }
        break;
    }
    WinMaliUvSrcUnlock_();
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
    ULONG  nQual = 0;   /* candidates that passed the size gate THIS tick */

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

    for (i = 0; i < WINMALI_UVSRC_SLOTS; i++) {
        PUCHAR base;
        ULONG  hits = 0, nonBlack = 0, y;   /* hits = TRANSITIONS, not non-black */
        ULONG  sum = 2166136261u, changes = 0, lastTick = 0;
        ULONG  lastNz = 0, coverage = 0;    /* how far down content reaches, % */

        if (g_WinMaliUvSrc[i].Handle == 0 || g_WinMaliUvSrc[i].Mdl == NULL ||
            g_WinMaliUvSrc[i].Dying) {
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
            ULONG sp   = (g_WinMaliUvSrc[i].Pitch != 0) ? g_WinMaliUvSrc[i].Pitch
                                                        : Stride;
            ULONG rows = (ULONG)(g_WinMaliUvSrc[i].Size / sp);
            if (rows > Height) { rows = Height; }
            for (y = 0; y < rows; y += 97) {
                const ULONG *row = (const ULONG *)(base + (SIZE_T)y * sp);
                ULONG x;
                BOOLEAN rowNz = FALSE;
                for (x = 0; x < (sp / 4); x += 89) {
                    ULONG px = row[x] & 0x00FFFFFFu;
                    if (px != 0) { nonBlack++; rowNz = TRUE; }
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
                if (rowNz) { lastNz = y; }
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
            nCand++;
        }

        /* ⛔ HARD REJECT A UNIFORM SURFACE. zero transitions = one flat colour
           across the whole grid. That is a cleared/blank buffer, never a composed
           desktop - and adopting one is exactly how the panel went solid white. */
        /* Track the best coverage on offer BEFORE any rejection, so a candidate
           that is dropped for being uniform still counts toward "was anything
           better available". */
        if (coverage > maxCoverage) { maxCoverage = coverage; }

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
            BOOLEAN better =
                (bestVa == NULL) ||
                (covBucket > bestCovBucket) ||
                (covBucket == bestCovBucket && lastTick > bestTick) ||
                (covBucket == bestCovBucket && lastTick == bestTick &&
                 hits > bestHits) ||
                (covBucket == bestCovBucket && lastTick == bestTick &&
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
    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcQual, (LONG)nQual);

    *OutHits = bestHits;
    if (bestVa == NULL) {
        WinMaliUvSrcUnlock_();   /* nothing to hold the lock for */
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
    PWINMALI_BO bo = WinMaliBoTakeByHandle_(Adapter, Handle);
    if (bo == NULL) {
        return WMERR_ENOENT;
    }
    WINMALI_TRACE("BoDestroy: handle=%u size=%llu",
                  bo->Handle, (ULONGLONG)bo->Size);
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

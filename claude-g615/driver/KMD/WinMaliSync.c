/*
 * WinMaliSync.c - SyncObj implementation. KEVENT-backed.
 *
 * Binary mode:    KEVENT (NotificationEvent) + TimelineValue stays 0/1
 * Timeline mode:  KEVENT pulsed on each Signal + TimelineValue = current point
 *
 * Wait semantics:
 *   - Single binary handle: KeWaitForSingleObject on the KEVENT
 *   - Single timeline handle: re-check counter on each pulse
 *   - Multi-handle WaitAll/Any: KeWaitForMultipleObjects with KEVENT array
 *
 * Multi-handle TIMELINE waits aren't implemented yet (would need per-handle
 * re-evaluation loops). Single-handle timeline is enough for the basic
 * GroupSubmit fence model.
 */

#include "WinMaliKmd.h"
#include "WinMaliSync.h"

/* Exported by ntoskrnl but only PROTOTYPED in ntifs.h, which a dispmprt/wdm
   miniport cannot pull in cleanly alongside ntddk.h. Declare the export we
   need rather than reordering the whole include chain. Without this the
   compiler assumes "extern int" (C4013) and the call still links, so the
   wrong-return-type bug would be silent - hence the explicit BOOLEAN. */
NTKERNELAPI BOOLEAN NTAPI PsIsThreadTerminating(_In_ PETHREAD Thread);
/* Same reason: prototyped in ntifs.h only. Used by the syncobj rundown to ask
   whether a handle's owning PROCESS is still alive before freeing it.
   ⛔ PASSIVE_LEVEL ONLY - never call it under the table spin lock. */
NTKERNELAPI NTSTATUS NTAPI PsLookupProcessByProcessId(_In_ HANDLE ProcessId,
                                                      _Outptr_ PEPROCESS* Process);

#define WMERR_OK          0
#define WMERR_EINVAL      -22
#define WMERR_ENOMEM      -12
#define WMERR_ENOENT      -2
/* WMERR_ETIMEDOUT now lives in WinMaliSync.h - the escape layer needs it too. */

/* ⛔ THE SAME PER-ADAPTER-HANDLE BUG, FOR THE THIRD TIME.
 *
 * BO handles had it (fixed: WinMaliBoAllocHandle_). Group handles had it
 * (fixed: WinMaliGroupAllocHandle_). Syncobjs still did: this table is
 * re-initialised on every adapter start, so the counter restarts at 1 while
 * dwm and explorer SURVIVE the restart holding handles the previous instance
 * minted. dxgk restarts the adapter during ordinary mode-setting, so this does
 * not need anyone to bounce anything.
 *
 * MEASURED: every failing GROUP_SUBMIT carried syncs=3 and came back ENOENT,
 * while the group-not-found counter stayed at ZERO - because the group check
 * passed and it was WinMaliEscapeProcessSyncOps_ rejecting unknown syncobj
 * handles. 195 rejected submits in one window; the desktop cannot update
 * because every frame is refused at its sync ops.
 *
 * Worse than failing: a restarted counter can RE-ISSUE a live handle, so a
 * stale reference silently aliases someone else's syncobj - the same
 * silent-aliasing class that cost this project the black desktop via BO
 * handles. Boot-unique makes a stale handle fail as unknown, forever. */
static volatile LONG g_WinMaliSyncHandleNext = 1;   /* 0 = "invalid" */

static ULONG
WinMaliSyncAllocHandle_(VOID)
{
    /* ExchangeAdd returns the PRE-increment value, so the first handle is 1. */
    return (ULONG)InterlockedExchangeAdd(&g_WinMaliSyncHandleNext, 1);
}

/* ⛔★★★★★ DESTROY LEDGER - "WAS IT DESTROYED, AND BY WHOM?"
 *
 * `handle=1` is created SUCCESSFULLY (dwm's own log: "WMVMSYNC1 vm_create id=1
 * TRACK_ACTIVITY -> sync handle=1") yet the KMD later reports it NOT IN TABLE
 * among 39 live objects. The table is built ONCE and never torn down
 * (SyncTblInits=1, SyncTblTears=0), so no bulk free touched it - which forces
 * the conclusion that something called SyncObjDestroy on it explicitly.
 *
 * The destroy IS traced, but the failure is only noticed ~57 s later and the
 * ring (24 KB, then 192 KB) has long scrolled past it. FOUR capture attempts
 * came back empty for exactly that reason - the window, not the code.
 *
 * So keep a tiny driver-global ledger of the last 32 destroyed handles and the
 * pid that asked, and print the match INSIDE the ENOENT warning. The answer
 * then travels WITH the failure instead of needing a capture window to
 * coincide with it. */
#define WINMALI_SYNC_DESTROY_LOG 32
static volatile LONG g_WinMaliSyncDestroyIdx = 0;
static ULONG  g_WinMaliSyncDestroyedHandle[WINMALI_SYNC_DESTROY_LOG];
static HANDLE g_WinMaliSyncDestroyedPid[WINMALI_SYNC_DESTROY_LOG];
static PCSTR  g_WinMaliSyncDestroyedWhy[WINMALI_SYNC_DESTROY_LOG];

static VOID
WinMaliSyncNoteDestroyed_(_In_ ULONG Handle, _In_ PCSTR Why)
{
    LONG i = (InterlockedIncrement(&g_WinMaliSyncDestroyIdx) - 1) &
             (WINMALI_SYNC_DESTROY_LOG - 1);
    g_WinMaliSyncDestroyedHandle[i] = Handle;
    g_WinMaliSyncDestroyedPid[i]    = PsGetCurrentProcessId();
    g_WinMaliSyncDestroyedWhy[i]    = Why;
}

static BOOLEAN
WinMaliSyncWasDestroyed_(_In_ ULONG Handle, _Out_ HANDLE* Pid, _Out_ PCSTR* Why)
{
    LONG i;
    *Pid = NULL;
    *Why = "";
    for (i = 0; i < WINMALI_SYNC_DESTROY_LOG; ++i) {
        if (g_WinMaliSyncDestroyedWhy[i] != NULL &&
            g_WinMaliSyncDestroyedHandle[i] == Handle) {
            *Pid = g_WinMaliSyncDestroyedPid[i];
            *Why = g_WinMaliSyncDestroyedWhy[i];
            return TRUE;
        }
    }
    return FALSE;
}

/* Driver-global parking list for syncobjs that outlive an adapter instance.
   Defined here (above the first user) - see the note on
   WinMaliSyncObjTableTeardown for why they must survive. */
static KSPIN_LOCK  g_WinMaliSyncOrphanLock;
static LIST_ENTRY  g_WinMaliSyncOrphanHead;
static BOOLEAN     g_WinMaliSyncOrphanInit = FALSE;

static VOID
WinMaliSyncOrphanInit_(VOID)
{
    if (!g_WinMaliSyncOrphanInit) {
        KeInitializeSpinLock(&g_WinMaliSyncOrphanLock);
        InitializeListHead(&g_WinMaliSyncOrphanHead);
        g_WinMaliSyncOrphanInit = TRUE;
    }
}

VOID
WinMaliSyncObjTableInit(_Out_ PWINMALI_SYNCOBJ_TABLE Table)
{
    KIRQL oldIrql;
    ULONG restored = 0;

    (VOID)InterlockedIncrement(&g_WinMaliStatSyncTableInits);
    KeInitializeSpinLock(&Table->Lock);
    InitializeListHead(&Table->Head);
    /* DIAGNOSTIC ONLY - handles come from the driver-global counter above. */
    Table->NextHandle = (LONG)InterlockedCompareExchange(&g_WinMaliSyncHandleNext, 0, 0);
    Table->Count = 0;

    /* ★ RE-HOME THE SURVIVORS OF THE PREVIOUS ADAPTER INSTANCE. dwm and
     * explorer outlive an adapter restart still holding their handles; parking
     * those objects (see WinMaliSyncObjTableTeardown) and splicing them back
     * here is what keeps `handle=1` resolvable for the process that owns it.
     * Handles are boot-unique, so re-homing cannot collide with new ones. */
    WinMaliSyncOrphanInit_();
    KeAcquireSpinLock(&g_WinMaliSyncOrphanLock, &oldIrql);
    while (!IsListEmpty(&g_WinMaliSyncOrphanHead)) {
        PLIST_ENTRY e = RemoveHeadList(&g_WinMaliSyncOrphanHead);
        InsertTailList(&Table->Head, e);
        Table->Count++;
        ++restored;
    }
    KeReleaseSpinLock(&g_WinMaliSyncOrphanLock, oldIrql);
    if (restored != 0) {
        WINMALI_TRACE("WMSYNCADPT1 SyncObjTableInit: %u syncobj(s) RESTORED from "
                      "the previous adapter instance", restored);
    }
}

static VOID
WinMaliSyncObjFree_(_Inout_ PWINMALI_SYNCOBJ So)
{
    if (So != NULL) {
        So->Magic = 0;
        ExFreePoolWithTag(So, WINMALI_POOL_TAG);
    }
}

/* ⛔★★★★★ SYNCOBJS MUST OUTLIVE THE ADAPTER, NOT JUST THE DEVICE.
 *
 * dxgk RESTARTS THE ADAPTER during ordinary mode-setting, and dwm/explorer
 * survive that restart still holding handles the previous instance minted. The
 * handle counter was already made boot-unique for exactly this reason (see the
 * note at the top of this file) - so a stale handle fails cleanly instead of
 * aliasing someone else's object. But the OBJECTS were still destroyed here on
 * every adapter stop, which turns "fails cleanly" into "fails forever".
 *
 * MEASURED 2026-08-18: `handle=1` - the FIRST syncobj of the boot - was absent
 * from a table holding 38 live objects, and dwm was still advancing it at
 * `point=10`. Scoping ownership to the DEVICE (v1.1.0.9) correctly orphaned 3
 * objects but changed nothing for dwm (`SignalFail` stayed 79, `StatSrcChg`
 * stayed 0): the loss is at ADAPTER scope, one level up.
 *
 * So park survivors on a DRIVER-GLOBAL list instead of freeing them, and splice
 * them back when the next adapter instance initialises its table. Handles are
 * boot-unique, so re-homing them cannot collide. */
VOID
WinMaliSyncObjTableTeardown(_Inout_ PWINMALI_ADAPTER Adapter,
                            _Inout_ PWINMALI_SYNCOBJ_TABLE Table)
{
    KIRQL oldIrql;
    LIST_ENTRY candHead;
    PLIST_ENTRY entry;
    ULONG parked = 0, freed = 0;

    (VOID)InterlockedIncrement(&g_WinMaliStatSyncTableTeardowns);
    UNREFERENCED_PARAMETER(Adapter);
    WinMaliSyncOrphanInit_();
    InitializeListHead(&candHead);

    KeAcquireSpinLock(&Table->Lock, &oldIrql);
    while (!IsListEmpty(&Table->Head)) {
        entry = RemoveHeadList(&Table->Head);
        InsertTailList(&candHead, entry);
    }
    Table->Count = 0;
    KeReleaseSpinLock(&Table->Lock, oldIrql);

    /* PASSIVE_LEVEL only - PsLookupProcessByProcessId must not run under a
       spin lock. Same two-phase shape as WinMaliSyncObjRundownOwner. */
    while (!IsListEmpty(&candHead)) {
        PWINMALI_SYNCOBJ so = CONTAINING_RECORD(RemoveHeadList(&candHead),
                                                WINMALI_SYNCOBJ, Link);
        PEPROCESS proc = NULL;
        BOOLEAN   ownerAlive = FALSE;

        if (so->OwnerProcessId != NULL &&
            NT_SUCCESS(PsLookupProcessByProcessId(so->OwnerProcessId, &proc))) {
            ownerAlive = TRUE;
            ObDereferenceObject(proc);
        }
        if (ownerAlive) {
            so->OwnerDevice = NULL;   /* the device is gone; the process is not */
            KeAcquireSpinLock(&g_WinMaliSyncOrphanLock, &oldIrql);
            InsertTailList(&g_WinMaliSyncOrphanHead, &so->Link);
            KeReleaseSpinLock(&g_WinMaliSyncOrphanLock, oldIrql);
            ++parked;
            continue;
        }
        WinMaliSyncNoteDestroyed_(so->Handle, "adapter table teardown");
        WinMaliSyncObjFree_(so);
        ++freed;
    }
    WINMALI_TRACE("WMSYNCADPT1 SyncObjTableTeardown: %u parked for the next "
                  "adapter instance (owning process still alive), %u freed",
                  parked, freed);
}

int
WinMaliSyncObjCreate(_Inout_ PWINMALI_ADAPTER Adapter,
                     _In_ ULONG Flags,
                     _In_ ULONG InitialState,
                     _In_opt_ PVOID OwnerDevice,
                     _Out_ ULONG* OutHandle)
{
    PWINMALI_SYNCOBJ so;
    KIRQL oldIrql;

    *OutHandle = 0;
    so = (PWINMALI_SYNCOBJ)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*so),
                                                 WINMALI_POOL_TAG);
    if (so == NULL) {
        return WMERR_ENOMEM;
    }
    RtlZeroMemory(so, sizeof(*so));
    so->Magic         = WINMALI_SYNCOBJ_MAGIC;
    so->IsTimeline    = (Flags & WINMALI_SYNC_OBJ_FLAG_TIMELINE) ? TRUE : FALSE;
    so->TimelineValue = (InitialState != 0) ? 1 : 0;
    so->RefCount      = 1;
    so->OwnerDevice   = OwnerDevice;
    /* The lifetime owner. A process outlives its individual D3D devices and
       keeps using these handles - see the note on OwnerProcessId. */
    so->OwnerProcessId = PsGetCurrentProcessId();
    KeInitializeSpinLock(&so->Lock);
    KeInitializeEvent(&so->Event, NotificationEvent,
                      (InitialState != 0) ? TRUE : FALSE);

    KeAcquireSpinLock(&Adapter->SyncObjTable.Lock, &oldIrql);
    so->Handle = WinMaliSyncAllocHandle_();  /* boot-unique, NOT per-adapter */
    InsertTailList(&Adapter->SyncObjTable.Head, &so->Link);
    Adapter->SyncObjTable.Count++;
    KeReleaseSpinLock(&Adapter->SyncObjTable.Lock, oldIrql);

    *OutHandle = so->Handle;
    WINMALI_TRACE("SyncObjCreate: handle=%u timeline=%u initial=%u",
                  so->Handle, so->IsTimeline, InitialState);
    return WMERR_OK;
}

static PWINMALI_SYNCOBJ
WinMaliSyncObjTakeByHandle_(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG Handle)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PWINMALI_SYNCOBJ found = NULL;

    KeAcquireSpinLock(&Adapter->SyncObjTable.Lock, &oldIrql);
    for (entry = Adapter->SyncObjTable.Head.Flink;
         entry != &Adapter->SyncObjTable.Head;
         entry = entry->Flink) {
        PWINMALI_SYNCOBJ so = CONTAINING_RECORD(entry, WINMALI_SYNCOBJ, Link);
        if (so->Handle == Handle) {
            RemoveEntryList(&so->Link);
            Adapter->SyncObjTable.Count--;
            found = so;
            break;
        }
    }
    KeReleaseSpinLock(&Adapter->SyncObjTable.Lock, oldIrql);
    return found;
}

static PWINMALI_SYNCOBJ
WinMaliSyncObjGet_(_In_ PWINMALI_ADAPTER Adapter, _In_ ULONG Handle)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PWINMALI_SYNCOBJ found = NULL;

    KeAcquireSpinLock(&Adapter->SyncObjTable.Lock, &oldIrql);
    for (entry = Adapter->SyncObjTable.Head.Flink;
         entry != &Adapter->SyncObjTable.Head;
         entry = entry->Flink) {
        PWINMALI_SYNCOBJ so = CONTAINING_RECORD(entry, WINMALI_SYNCOBJ, Link);
        if (so->Handle == Handle) {
            InterlockedIncrement(&so->RefCount);
            found = so;
            break;
        }
    }
    KeReleaseSpinLock(&Adapter->SyncObjTable.Lock, oldIrql);
    return found;
}

ULONG
WinMaliSyncObjRundownOwner(_Inout_ PWINMALI_ADAPTER Adapter,
                           _In_ PVOID OwnerDevice)
{
    KIRQL oldIrql;
    LIST_ENTRY freeHead;
    PLIST_ENTRY entry, next;
    ULONG freed = 0;
    ULONG orphaned = 0;

    if (OwnerDevice == NULL) {
        return 0;
    }
    InitializeListHead(&freeHead);

    KeAcquireSpinLock(&Adapter->SyncObjTable.Lock, &oldIrql);
    for (entry = Adapter->SyncObjTable.Head.Flink;
         entry != &Adapter->SyncObjTable.Head;
         entry = next) {
        PWINMALI_SYNCOBJ so = CONTAINING_RECORD(entry, WINMALI_SYNCOBJ, Link);
        next = entry->Flink;
        if (so->OwnerDevice != OwnerDevice) {
            continue;
        }
        /* Phase 1: detach every candidate under the lock. The liveness test
           CANNOT run here - see the phase-2 note below. */
        RemoveEntryList(&so->Link);
        Adapter->SyncObjTable.Count--;
        InsertTailList(&freeHead, &so->Link);
    }
    KeReleaseSpinLock(&Adapter->SyncObjTable.Lock, oldIrql);

    /* ⛔★★★★★ PHASE 2 - DO NOT FREE A SYNCOBJ WHOSE **PROCESS** IS STILL ALIVE.
     *
     * The unconditional free here froze the desktop for an entire boot. dwm
     * creates a timeline syncobj, one of its D3D devices is later torn down, and
     * this rundown freed the object while dwm was still advancing it - measured
     * at `point=10`, i.e. ten successful signals in. Every later signal returned
     * ENOENT, GROUP_SUBMIT reported -2 even though `the CS RAN`, and dwm stopped
     * compositing permanently: no taskbar, dead input, StatSrcChg=0 over 11,521
     * scans.
     *
     * DRM/panfrost scope sync objects per FILE (process), and a process
     * legitimately outlives any single device. So: ORPHAN the object - detach it
     * from the dying device but PUT IT BACK in the table - and free it only once
     * its owning process is gone.
     *
     * ⛔ THIS LOOP MUST RUN OUTSIDE THE SPIN LOCK. KeAcquireSpinLock raises to
     * DISPATCH_LEVEL, and PsLookupProcessByProcessId requires PASSIVE_LEVEL -
     * calling it under the lock is an instant bugcheck. Hence the two phases:
     * detach under the lock, adjudicate without it, re-insert the survivors.
     *
     * ⚠ PIDs are reused, so a false "alive" is possible in principle. The cost
     * is a bounded leak until adapter teardown (which frees the whole table),
     * never a use-after-free. The trade is deliberately one-directional:
     * freeing early breaks a live compositor; freeing late costs a few hundred
     * bytes. */
    while (!IsListEmpty(&freeHead)) {
        PWINMALI_SYNCOBJ so = CONTAINING_RECORD(RemoveHeadList(&freeHead),
                                                WINMALI_SYNCOBJ, Link);
        PEPROCESS proc = NULL;
        BOOLEAN   ownerAlive = FALSE;

        if (so->OwnerProcessId != NULL &&
            NT_SUCCESS(PsLookupProcessByProcessId(so->OwnerProcessId, &proc))) {
            ownerAlive = TRUE;
            ObDereferenceObject(proc);
        }

        if (ownerAlive) {
            so->OwnerDevice = NULL;   /* orphaned: outlives this device */
            KeAcquireSpinLock(&Adapter->SyncObjTable.Lock, &oldIrql);
            InsertTailList(&Adapter->SyncObjTable.Head, &so->Link);
            Adapter->SyncObjTable.Count++;
            KeReleaseSpinLock(&Adapter->SyncObjTable.Lock, oldIrql);
            ++orphaned;
            continue;
        }

        WINMALI_TRACE("SyncObjRundown: handle=%u freed (owning PROCESS is gone)",
                      so->Handle);
        WinMaliSyncNoteDestroyed_(so->Handle, "rundown: owning process gone");
        WinMaliSyncObjFree_(so);
        ++freed;
    }
    if (orphaned != 0) {
        WINMALI_TRACE("WMSYNCOWN1 SyncObjRundown: %u syncobj(s) ORPHANED, not "
                      "freed - their process is still alive (this is the fix for "
                      "dwm's ENOENT storm); %u freed", orphaned, freed);
    }
    return freed;
}

/*
 * ---- BOUNDED, TERMINATION-AWARE WAIT ------------------------------------
 *
 * ⛔ NEVER CALL KeWaitForSingleObject(..., KernelMode, FALSE, NULL) ON A PATH A
 * USER THREAD CAN REACH THROUGH AN ESCAPE.
 *
 * That wait is infinite AND non-alertable, so nothing can break it out: not
 * Stop-Process -Force, not process rundown, not PnP. Measured 2026-08-12: a
 * rendertest process whose CS translation-faulted parked in exactly such a wait
 * and became unkillable (pid survived repeated -Force). Because it also held
 * fw->SubmitLock, every later submitter parked behind it. The next shutdown then
 * could not remove the device, the PnP lock stayed held, and the power
 * transition timed out at 300 s:
 *
 *     DRIVER_POWER_STATE_FAILURE 0x9F  P1=4 (timed out synchronising with PnP)
 *                                      P2=0x12C (= 300 s)  P3=<blocking thread>
 *
 * A wedged GPU must degrade to an ERROR, never to an unkillable thread. So wait
 * in slices and give up when the thread is being torn down or the device is
 * going away. Timeout==NULL means "no caller deadline", NOT "wait forever" - it
 * is still bounded by the abort checks.
 */
NTSTATUS
WinMaliWaitObjectAbortable(_In_ PVOID Object,
                           _In_opt_ PLARGE_INTEGER Timeout,
                           _In_ ULONG SliceMs)
{
    LARGE_INTEGER slice;
    LONGLONG      remaining = -1;   /* < 0 => no caller deadline */
    NTSTATUS      st;

    if (SliceMs == 0) {
        SliceMs = 50;
    }

    if (Timeout != NULL) {
        if (Timeout->QuadPart > 0) {
            /* Absolute deadline - we never generate these; pass through. */
            return KeWaitForSingleObject(Object, Executive, KernelMode, FALSE, Timeout);
        }
        remaining = -Timeout->QuadPart;     /* relative, in 100ns units */
    }

    for (;;) {
        LONGLONG thisSlice = (LONGLONG)SliceMs * 10000LL;
        if (remaining >= 0 && thisSlice > remaining) {
            thisSlice = remaining;
        }
        slice.QuadPart = -thisSlice;

        st = KeWaitForSingleObject(Object, Executive, KernelMode, FALSE, &slice);
        if (st != STATUS_TIMEOUT) {
            return st;                       /* acquired, or a real failure */
        }

        if (remaining >= 0) {
            remaining -= thisSlice;
            if (remaining <= 0) {
                return STATUS_TIMEOUT;
            }
        }

        if (PsIsThreadTerminating(PsGetCurrentThread())) {
            return STATUS_THREAD_IS_TERMINATING;
        }
        if (InterlockedCompareExchange(&g_WinMaliStopping, 0, 0) != 0) {
            return STATUS_DEVICE_REMOVED;
        }
    }
}

int
WinMaliSyncObjDestroy(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG Handle)
{
    PWINMALI_SYNCOBJ so = WinMaliSyncObjTakeByHandle_(Adapter, Handle);
    if (so == NULL) {
        return WMERR_ENOENT;
    }
    WINMALI_TRACE("SyncObjDestroy: handle=%u (explicit, pid=%p)", so->Handle, PsGetCurrentProcessId());
    WinMaliSyncNoteDestroyed_(so->Handle, "explicit SyncObjDestroy escape");
    WinMaliSyncObjFree_(so);
    return WMERR_OK;
}

int
WinMaliSyncObjSignal(_Inout_ PWINMALI_ADAPTER Adapter,
                     _In_ ULONG Handle,
                     _In_ UINT64 Point)
{
    PWINMALI_SYNCOBJ so = WinMaliSyncObjGet_(Adapter, Handle);
    KIRQL oldIrql;
    if (so == NULL) {
        /* ⛔★★★★★ NAME THE HANDLE. THIS IS WHY dwm STOPS COMPOSITING.
         *
         * MEASURED 2026-08-18: dwm's GROUP_SUBMIT dies `err=-2` and dwm then
         * stops compositing entirely - frozen desktop, no taskbar, dead input,
         * `StatSrcChg=3` over 22,321 scans. explorer is unaffected and keeps
         * presenting, and the GPU itself is healthy at the time.
         *
         * The published counters localised it to HERE and nowhere else:
         *   StatSubmitNoGroup=0  StatSubmitSyncFail=0  StatSubmitFatalGrp=0
         *   SignalFail=94  SignalErr=0xFFFFFFFE(-2)  SignalBadHnd=1
         * i.e. the submit's WAIT ops pass and the post-submit SIGNAL is what
         * fails, so `StatSubmitSyncFail=0` must NOT be read as "syncobjs fine".
         *
         * A bare ENOENT cannot distinguish the three real causes, which need
         * three different fixes:
         *   - the handle was DESTROYED under a live submitter
         *     (WinMaliSyncObjDestroy frees via TakeByHandle_ regardless of any
         *      outstanding RefCount taken by WinMaliSyncObjGet_), or
         *   - the owning device was torn down and WinMaliSyncObjRundownOwner
         *     bulk-freed it ("owner device died without SyncObjDestroy"), or
         *   - the handle never existed in THIS adapter instance.
         * Printing the handle, the live count and the caller lets the ring say
         * which - the events themselves scroll out of the 24 KB ring long
         * before the failure is noticed.
         */
        {
            HANDLE dpid = NULL;
            PCSTR  dwhy = "";
            if (WinMaliSyncWasDestroyed_(Handle, &dpid, &dwhy)) {
                WINMALI_WARN("WMSIGENOENT1 SyncObjSignal: handle=%u NOT IN TABLE "
                             "(live=%ld, point=%I64u, caller pid=%p) >>> IT WAS "
                             "DESTROYED by pid=%p via '%s' <<<",
                             Handle, Adapter->SyncObjTable.Count, Point,
                             PsGetCurrentProcessId(), dpid, dwhy);
            } else {
                /* Not in the last-32 ledger. Either it was destroyed long ago
                   and aged out, or it was NEVER REGISTERED - which would mean
                   SyncObjCreate handed the UMD a handle it never inserted. */
                WINMALI_WARN("WMSIGENOENT1 SyncObjSignal: handle=%u NOT IN TABLE "
                             "(live=%ld, point=%I64u, caller pid=%p) >>> NO "
                             "DESTROY RECORDED in the last %u - suspect it was "
                             "never registered <<<",
                             Handle, Adapter->SyncObjTable.Count, Point,
                             PsGetCurrentProcessId(),
                             (ULONG)WINMALI_SYNC_DESTROY_LOG);
            }
        }
        return WMERR_ENOENT;
    }
    KeAcquireSpinLock(&so->Lock, &oldIrql);
    if (so->IsTimeline) {
        if (Point > so->TimelineValue) {
            so->TimelineValue = Point;
        }
    } else {
        so->TimelineValue = 1;
    }
    KeReleaseSpinLock(&so->Lock, oldIrql);
    /* PulseEvent so any waiters re-check; for binary that's just "signal".
       NotificationEvent SetEvent semantics make it stay signalled until
       reset; we use SetEvent + the waiter re-checks on its own. */
    KeSetEvent(&so->Event, IO_NO_INCREMENT, FALSE);
    InterlockedDecrement(&so->RefCount);
    return WMERR_OK;
}

/* Translate panthor's "0 = poll, <0 = forever, >0 = ns" into a KeWait
   compatible LARGE_INTEGER. Returns NULL for "wait forever". */
static PLARGE_INTEGER
WinMaliSyncObjMakeTimeout_(_Inout_ LARGE_INTEGER* Storage,
                           _In_ INT64 TimeoutNs)
{
    if (TimeoutNs == 0) {
        Storage->QuadPart = 0;          /* poll */
        return Storage;
    }
    if (TimeoutNs < 0) {
        return NULL;                    /* infinite */
    }
    /* 100-ns units, relative (negative). */
    Storage->QuadPart = -(TimeoutNs / 100);
    if (Storage->QuadPart == 0) {
        Storage->QuadPart = -1;         /* at least 100ns */
    }
    return Storage;
}

int
WinMaliSyncObjWait(_Inout_ PWINMALI_ADAPTER Adapter,
                   _In_ ULONG Count,
                   _In_reads_(Count) const ULONG* Handles,
                   _In_reads_opt_(Count) const UINT64* Points,
                   _In_ BOOLEAN WaitAll,
                   _In_ INT64 TimeoutNs)
{
    PWINMALI_SYNCOBJ singleSo;
    LARGE_INTEGER timeout;
    PLARGE_INTEGER pTimeout;
    NTSTATUS st;
    int err = WMERR_OK;

    if (Count == 0 || Handles == NULL) {
        return WMERR_EINVAL;
    }
    pTimeout = WinMaliSyncObjMakeTimeout_(&timeout, TimeoutNs);

    if (Count == 1) {
        /* Every wait below goes through WinMaliWaitObjectAbortable, never a raw
           KeWaitForSingleObject(..., KernelMode, FALSE, NULL). See that helper. */
        singleSo = WinMaliSyncObjGet_(Adapter, Handles[0]);
        if (singleSo == NULL) {
            return WMERR_ENOENT;
        }

        if (singleSo->IsTimeline) {
            UINT64 target = (Points != NULL) ? Points[0] : 0;
            /* ⚠ pTimeout is a RELATIVE timeout, and this loop re-waits on every
               pulse that did not reach the target - so a stream of pulses
               restarts the clock forever and a "2 s" bound silently becomes
               unbounded. Anchor an absolute deadline once and shrink the
               relative timeout against it. Only when a finite one was asked
               for; TimeoutNs < 0 still means infinite for the escape op that
               genuinely wants it. */
            LARGE_INTEGER deadline;
            LARGE_INTEGER nowTick;
            BOOLEAN bounded = (TimeoutNs >= 0);
            if (bounded) {
                KeQuerySystemTimePrecise(&deadline);
                deadline.QuadPart += (TimeoutNs / 100);   /* ns -> 100 ns units */
            } else {
                deadline.QuadPart = 0;
            }
            /* Re-check loop: each pulse may not bring us to the target. */
            for (;;) {
                KIRQL oldIrql;
                BOOLEAN reached;
                KeAcquireSpinLock(&singleSo->Lock, &oldIrql);
                reached = (singleSo->TimelineValue >= target);
                KeReleaseSpinLock(&singleSo->Lock, oldIrql);
                if (reached) {
                    err = WMERR_OK;
                    break;
                }
                /* NotificationEvent stays signalled - reset before wait so
                   we only wake on a fresh signal. */
                KeClearEvent(&singleSo->Event);
                /* Re-check after clear to avoid lost-wakeup. */
                KeAcquireSpinLock(&singleSo->Lock, &oldIrql);
                reached = (singleSo->TimelineValue >= target);
                KeReleaseSpinLock(&singleSo->Lock, oldIrql);
                if (reached) {
                    err = WMERR_OK;
                    break;
                }
                if (bounded) {
                    KeQuerySystemTimePrecise(&nowTick);
                    if (nowTick.QuadPart >= deadline.QuadPart) {
                        err = WMERR_ETIMEDOUT;
                        break;
                    }
                    /* Negative = relative, in 100 ns units. */
                    timeout.QuadPart = -(deadline.QuadPart - nowTick.QuadPart);
                    pTimeout = &timeout;
                }
                st = WinMaliWaitObjectAbortable(&singleSo->Event, pTimeout, 100);
                if (st == STATUS_THREAD_IS_TERMINATING || st == STATUS_DEVICE_REMOVED) {
                    err = WMERR_ETIMEDOUT;
                    break;
                }
                if (st == STATUS_TIMEOUT) {
                    err = WMERR_ETIMEDOUT;
                    break;
                }
                if (!NT_SUCCESS(st)) {
                    err = WMERR_EINVAL;
                    break;
                }
                /* Loop and re-check counter. */
            }
        } else {
            st = WinMaliWaitObjectAbortable(&singleSo->Event, pTimeout, 100);
            err = (st == STATUS_TIMEOUT ||
                   st == STATUS_THREAD_IS_TERMINATING ||
                   st == STATUS_DEVICE_REMOVED) ? WMERR_ETIMEDOUT
                : (NT_SUCCESS(st)               ? WMERR_OK : WMERR_EINVAL);
        }
        InterlockedDecrement(&singleSo->RefCount);
        return err;
    }

    /* Multi-handle path: binary-only for now. Each handle's KEVENT joins
       the wait array. */
    {
        PVOID  objects[MAXIMUM_WAIT_OBJECTS];
        PWINMALI_SYNCOBJ refs[MAXIMUM_WAIT_OBJECTS];
        ULONG i;
        if (Count > MAXIMUM_WAIT_OBJECTS) {
            return WMERR_EINVAL;
        }
        for (i = 0; i < Count; ++i) {
            refs[i] = WinMaliSyncObjGet_(Adapter, Handles[i]);
            if (refs[i] == NULL) {
                while (i > 0) {
                    InterlockedDecrement(&refs[--i]->RefCount);
                }
                return WMERR_ENOENT;
            }
            if (refs[i]->IsTimeline) {
                /* Multi-handle timeline wait isn't implemented. */
                while (i + 1 > 0) {
                    InterlockedDecrement(&refs[i--]->RefCount);
                    if (i == (ULONG)-1) {
                        break;
                    }
                }
                return WMERR_EINVAL;
            }
            objects[i] = &refs[i]->Event;
        }
        st = KeWaitForMultipleObjects(Count, objects,
                                      WaitAll ? WaitAll : WaitAny,
                                      Executive, KernelMode, FALSE,
                                      pTimeout, NULL);
        for (i = 0; i < Count; ++i) {
            InterlockedDecrement(&refs[i]->RefCount);
        }
        if (st == STATUS_TIMEOUT) {
            return WMERR_ETIMEDOUT;
        }
        if (!NT_SUCCESS(st)) {
            return WMERR_EINVAL;
        }
        return WMERR_OK;
    }
}

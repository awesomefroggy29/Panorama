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

#define WMERR_OK          0
#define WMERR_EINVAL      -22
#define WMERR_ENOMEM      -12
#define WMERR_ENOENT      -2
#define WMERR_EIO         -5
#define WMERR_ETIMEDOUT   -110

struct _WINMALI_SYNC_TRANSFER {
    LIST_ENTRY          SourceLink;
    PWINMALI_SYNCOBJ    Destination; /* referenced until propagation */
    UINT64              SourcePoint;
    UINT64              DestinationPoint;
    BOOLEAN             SourceFailed;
};

static VOID WinMaliSyncObjCompleteDirect_(_Inout_ PWINMALI_SYNCOBJ So,
                                           _In_ UINT64 Point,
                                           _In_ BOOLEAN Failed);
static VOID WinMaliSyncObjCancelDependents_(_Inout_ PWINMALI_SYNCOBJ So);

VOID
WinMaliSyncObjTableInit(_Out_ PWINMALI_SYNCOBJ_TABLE Table)
{
    KeInitializeSpinLock(&Table->Lock);
    InitializeListHead(&Table->Head);
    Table->NextHandle = 1;
    Table->Count = 0;
}

static VOID
WinMaliSyncObjFree_(_Inout_ PWINMALI_SYNCOBJ So)
{
    if (So != NULL) {
        NT_ASSERT(IsListEmpty(&So->Dependents));
        So->Magic = 0;
        ExFreePoolWithTag(So, WINMALI_POOL_TAG);
    }
}

static VOID
WinMaliSyncObjPut_(_Inout_opt_ PWINMALI_SYNCOBJ So)
{
    if (So != NULL && InterlockedDecrement(&So->RefCount) == 0) {
        WinMaliSyncObjFree_(So);
    }
}

VOID
WinMaliSyncObjTableTeardown(_Inout_ PWINMALI_ADAPTER Adapter,
                            _Inout_ PWINMALI_SYNCOBJ_TABLE Table)
{
    KIRQL oldIrql;
    LIST_ENTRY freeHead;
    PLIST_ENTRY entry;

    UNREFERENCED_PARAMETER(Adapter);
    InitializeListHead(&freeHead);

    KeAcquireSpinLock(&Table->Lock, &oldIrql);
    while (!IsListEmpty(&Table->Head)) {
        entry = RemoveHeadList(&Table->Head);
        InterlockedExchange(
            &CONTAINING_RECORD(entry, WINMALI_SYNCOBJ, Link)->Removed, 1);
        InsertTailList(&freeHead, entry);
    }
    Table->Count = 0;
    KeReleaseSpinLock(&Table->Lock, oldIrql);

    while (!IsListEmpty(&freeHead)) {
        entry = RemoveHeadList(&freeHead);
        {
            PWINMALI_SYNCOBJ so =
                CONTAINING_RECORD(entry, WINMALI_SYNCOBJ, Link);
            WinMaliSyncObjCancelDependents_(so);
            WinMaliSyncObjPut_(so);
        }
    }
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
    so->BinarySubmittedValue = 0;
    so->BinaryCompletedValue = 0;
    so->RefCount      = 1;
    so->OwnerDevice   = OwnerDevice;
    KeInitializeSpinLock(&so->Lock);
    InitializeListHead(&so->Dependents);
    KeInitializeEvent(&so->Event, NotificationEvent,
                      (InitialState != 0) ? TRUE : FALSE);

    KeAcquireSpinLock(&Adapter->SyncObjTable.Lock, &oldIrql);
    so->Handle = (ULONG)(Adapter->SyncObjTable.NextHandle++);
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
            InterlockedExchange(&so->Removed, 1);
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

BOOLEAN
WinMaliSyncObjValidate(_Inout_ PWINMALI_ADAPTER Adapter,
                       _In_ ULONG Handle,
                       _In_opt_ PVOID OwnerDevice,
                       _In_ BOOLEAN ExpectTimeline)
{
    PWINMALI_SYNCOBJ so;
    BOOLEAN valid;

    if (Adapter == NULL || Handle == 0) {
        return FALSE;
    }

    so = WinMaliSyncObjGet_(Adapter, Handle);
    if (so == NULL) {
        return FALSE;
    }

    valid = (so->Magic == WINMALI_SYNCOBJ_MAGIC &&
             so->IsTimeline == ExpectTimeline &&
             (OwnerDevice == NULL || so->OwnerDevice == OwnerDevice));
    WinMaliSyncObjPut_(so);
    return valid;
}

ULONG
WinMaliSyncObjRundownOwner(_Inout_ PWINMALI_ADAPTER Adapter,
                           _In_ PVOID OwnerDevice)
{
    KIRQL oldIrql;
    LIST_ENTRY freeHead;
    PLIST_ENTRY entry, next;
    ULONG freed = 0;

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
        if (so->OwnerDevice == OwnerDevice) {
            RemoveEntryList(&so->Link);
            Adapter->SyncObjTable.Count--;
            InterlockedExchange(&so->Removed, 1);
            InsertTailList(&freeHead, &so->Link);
        }
    }
    KeReleaseSpinLock(&Adapter->SyncObjTable.Lock, oldIrql);

    while (!IsListEmpty(&freeHead)) {
        PWINMALI_SYNCOBJ so = CONTAINING_RECORD(RemoveHeadList(&freeHead),
                                                WINMALI_SYNCOBJ, Link);
        WINMALI_TRACE("SyncObjRundown: handle=%u (owner device died without "
                      "SyncObjDestroy)", so->Handle);
        WinMaliSyncObjCancelDependents_(so);
        WinMaliSyncObjPut_(so);
        ++freed;
    }
    return freed;
}

int
WinMaliSyncObjDestroy(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG Handle)
{
    PWINMALI_SYNCOBJ so = WinMaliSyncObjTakeByHandle_(Adapter, Handle);
    if (so == NULL) {
        return WMERR_ENOENT;
    }
    WINMALI_TRACE("SyncObjDestroy: handle=%u", so->Handle);
    WinMaliSyncObjCancelDependents_(so);
    WinMaliSyncObjPut_(so);
    return WMERR_OK;
}

static VOID
WinMaliSyncObjCompleteDirect_(_Inout_ PWINMALI_SYNCOBJ So,
                              _In_ UINT64 Point,
                              _In_ BOOLEAN Failed)
{
    KIRQL oldIrql;
    LIST_ENTRY ready;
    PLIST_ENTRY entry, next;
    UINT64 before;
    UINT64 after;
    BOOLEAN timeline;

    InitializeListHead(&ready);
    KeAcquireSpinLock(&So->Lock, &oldIrql);
    timeline = So->IsTimeline;
    before = timeline ? So->TimelineValue : So->BinaryCompletedValue;
    if (timeline) {
        if (Failed) {
            if (Point > So->TimelineFailedValue)
                So->TimelineFailedValue = Point;
        } else if (Point > So->TimelineValue) {
            So->TimelineValue = Point;
        }
    } else {
        if (Point == 0) {
            Point = So->BinarySubmittedValue;
        }
        if (Failed) {
            if (Point > So->BinaryFailedValue)
                So->BinaryFailedValue = Point;
        } else if (Point > So->BinaryCompletedValue) {
            So->BinaryCompletedValue = Point;
        }
    }

    after = timeline ? So->TimelineValue : So->BinaryCompletedValue;
    for (entry = So->Dependents.Flink;
         entry != &So->Dependents;
         entry = next) {
        PWINMALI_SYNC_TRANSFER transfer =
            CONTAINING_RECORD(entry, WINMALI_SYNC_TRANSFER, SourceLink);
        BOOLEAN reached;
        BOOLEAN sourceFailed;
        next = entry->Flink;
        if (timeline) {
            reached = So->TimelineValue >= transfer->SourcePoint;
            sourceFailed = So->TimelineFailedValue >= transfer->SourcePoint &&
                           transfer->SourcePoint != 0;
        } else {
            reached = So->BinaryCompletedValue >= transfer->SourcePoint;
            sourceFailed = So->BinaryFailedValue >= transfer->SourcePoint;
        }
        if (reached || sourceFailed) {
            RemoveEntryList(entry);
            /* Preserve whether this source point failed for the unlocked
               propagation pass. */
            transfer->SourceFailed = sourceFailed;
            InsertTailList(&ready, entry);
        }
    }
    KeReleaseSpinLock(&So->Lock, oldIrql);

    KeSetEvent(&So->Event, IO_NO_INCREMENT, FALSE);
    WINMALI_DIAG("SyncObj%s: handle=%u point=%llu timeline=%u value=%llu->%llu",
                 Failed ? "Fail" : "Signal", So->Handle,
                 (unsigned long long)Point, timeline,
                 (unsigned long long)before, (unsigned long long)after);

    while (!IsListEmpty(&ready)) {
        PWINMALI_SYNC_TRANSFER transfer = CONTAINING_RECORD(
            RemoveHeadList(&ready), WINMALI_SYNC_TRANSFER, SourceLink);
        WinMaliSyncObjCompleteDirect_(transfer->Destination,
                                      transfer->DestinationPoint,
                                      transfer->SourceFailed);
        WinMaliSyncObjPut_(transfer->Destination);
        ExFreePoolWithTag(transfer, WINMALI_POOL_TAG);
    }
}

int
WinMaliSyncObjSignal(_Inout_ PWINMALI_ADAPTER Adapter,
                     _In_ ULONG Handle,
                     _In_ UINT64 Point)
{
    PWINMALI_SYNCOBJ so = WinMaliSyncObjGet_(Adapter, Handle);
    if (so == NULL) {
        WINMALI_ERROR("SyncObjSignal: handle=%u point=%llu -> ENOENT",
                      Handle, (unsigned long long)Point);
        return WMERR_ENOENT;
    }
    WinMaliSyncObjCompleteDirect_(so, Point, FALSE);
    WinMaliSyncObjPut_(so);
    return WMERR_OK;
}

int
WinMaliSyncObjFail(_Inout_ PWINMALI_ADAPTER Adapter,
                   _In_ ULONG Handle,
                   _In_ UINT64 Point)
{
    PWINMALI_SYNCOBJ so = WinMaliSyncObjGet_(Adapter, Handle);
    if (so == NULL)
        return WMERR_ENOENT;
    WinMaliSyncObjCompleteDirect_(so, Point, TRUE);
    WinMaliSyncObjPut_(so);
    return WMERR_OK;
}

int
WinMaliSyncObjArmSignal(_Inout_ PWINMALI_ADAPTER Adapter,
                        _In_ ULONG Handle,
                        _Inout_ UINT64* InOutPoint)
{
    PWINMALI_SYNCOBJ so;
    KIRQL oldIrql;

    if (InOutPoint == NULL) {
        return WMERR_EINVAL;
    }
    so = WinMaliSyncObjGet_(Adapter, Handle);
    if (so == NULL) {
        return WMERR_ENOENT;
    }

    KeAcquireSpinLock(&so->Lock, &oldIrql);
    if (!so->IsTimeline) {
        ++so->BinarySubmittedValue;
        if (so->BinarySubmittedValue == 0) {
            /* Generation zero means "no GPU fence installed". */
            ++so->BinarySubmittedValue;
        }
        *InOutPoint = so->BinarySubmittedValue;
        KeClearEvent(&so->Event);
    }
    KeReleaseSpinLock(&so->Lock, oldIrql);

    WINMALI_TRACE("SyncObjArm: handle=%u point=%llu timeline=%u",
                  Handle, (unsigned long long)*InOutPoint, so->IsTimeline);
    WinMaliSyncObjPut_(so);
    return WMERR_OK;
}

static VOID
WinMaliSyncObjCancelDependents_(_Inout_ PWINMALI_SYNCOBJ So)
{
    KIRQL oldIrql;
    LIST_ENTRY cancelled;

    InitializeListHead(&cancelled);
    KeAcquireSpinLock(&So->Lock, &oldIrql);
    while (!IsListEmpty(&So->Dependents)) {
        PLIST_ENTRY entry = RemoveHeadList(&So->Dependents);
        InsertTailList(&cancelled, entry);
    }
    KeReleaseSpinLock(&So->Lock, oldIrql);

    while (!IsListEmpty(&cancelled)) {
        PWINMALI_SYNC_TRANSFER transfer = CONTAINING_RECORD(
            RemoveHeadList(&cancelled), WINMALI_SYNC_TRANSFER, SourceLink);
        WinMaliSyncObjCompleteDirect_(transfer->Destination,
                                      transfer->DestinationPoint, TRUE);
        WinMaliSyncObjPut_(transfer->Destination);
        ExFreePoolWithTag(transfer, WINMALI_POOL_TAG);
    }
}

int
WinMaliSyncObjTransfer(_Inout_ PWINMALI_ADAPTER Adapter,
                       _In_ ULONG DstHandle,
                       _In_ UINT64 DstPoint,
                       _In_ ULONG SrcHandle,
                       _In_ UINT64 SrcPoint)
{
    PWINMALI_SYNCOBJ source;
    PWINMALI_SYNCOBJ destination;
    PWINMALI_SYNC_TRANSFER transfer;
    KIRQL oldIrql;
    BOOLEAN reached;
    BOOLEAN failed;

    if (Adapter == NULL || DstHandle == 0 || SrcHandle == 0 ||
        DstHandle == SrcHandle) {
        return WMERR_EINVAL;
    }

    source = WinMaliSyncObjGet_(Adapter, SrcHandle);
    if (source == NULL)
        return WMERR_ENOENT;
    destination = WinMaliSyncObjGet_(Adapter, DstHandle);
    if (destination == NULL) {
        WinMaliSyncObjPut_(source);
        return WMERR_ENOENT;
    }

    transfer = (PWINMALI_SYNC_TRANSFER)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(*transfer), WINMALI_POOL_TAG);
    if (transfer == NULL) {
        WinMaliSyncObjPut_(destination);
        WinMaliSyncObjPut_(source);
        return WMERR_ENOMEM;
    }
    RtlZeroMemory(transfer, sizeof(*transfer));

    /* Replacing a binary destination fence creates a new generation. A
       timeline destination retains the caller-selected destination point. */
    KeAcquireSpinLock(&destination->Lock, &oldIrql);
    if (InterlockedCompareExchange(&destination->Removed, 0, 0) != 0) {
        KeReleaseSpinLock(&destination->Lock, oldIrql);
        ExFreePoolWithTag(transfer, WINMALI_POOL_TAG);
        WinMaliSyncObjPut_(destination);
        WinMaliSyncObjPut_(source);
        return WMERR_ENOENT;
    }
    if (!destination->IsTimeline) {
        ++destination->BinarySubmittedValue;
        if (destination->BinarySubmittedValue == 0)
            ++destination->BinarySubmittedValue;
        DstPoint = destination->BinarySubmittedValue;
        KeClearEvent(&destination->Event);
    }
    KeReleaseSpinLock(&destination->Lock, oldIrql);

    transfer->Destination = destination; /* transfer owns this reference */
    transfer->DestinationPoint = DstPoint;

    KeAcquireSpinLock(&source->Lock, &oldIrql);
    if (InterlockedCompareExchange(&source->Removed, 0, 0) != 0) {
        reached = FALSE;
        failed = TRUE;
    } else if (!source->IsTimeline && SrcPoint == 0) {
        SrcPoint = source->BinarySubmittedValue;
        reached = source->BinaryCompletedValue >= SrcPoint;
        failed = source->BinaryFailedValue >= SrcPoint;
    } else if (source->IsTimeline) {
        reached = source->TimelineValue >= SrcPoint;
        failed = source->TimelineFailedValue >= SrcPoint && SrcPoint != 0;
    } else {
        reached = source->BinaryCompletedValue >= SrcPoint;
        failed = source->BinaryFailedValue >= SrcPoint;
    }
    transfer->SourcePoint = SrcPoint;
    if (!reached && !failed)
        InsertTailList(&source->Dependents, &transfer->SourceLink);
    KeReleaseSpinLock(&source->Lock, oldIrql);

    WINMALI_DIAG("SyncObjTransfer: src=%u/%llu dst=%u/%llu pending=%u failed=%u",
                 SrcHandle, (unsigned long long)SrcPoint,
                 DstHandle, (unsigned long long)DstPoint,
                 (!reached && !failed), failed);

    if (reached || failed) {
        WinMaliSyncObjCompleteDirect_(destination, DstPoint, failed);
        WinMaliSyncObjPut_(destination);
        ExFreePoolWithTag(transfer, WINMALI_POOL_TAG);
    }
    WinMaliSyncObjPut_(source);
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
        WINMALI_ERROR("SyncObjWait: invalid args count=%u handles=%p", Count, Handles);
        return WMERR_EINVAL;
    }
    WINMALI_ROUTINE("SyncObjWait: ENTER count=%u waitAll=%u timeoutNs=%lld handle0=%u point0=%llu",
                 Count, WaitAll, (long long)TimeoutNs, Handles[0],
                 (unsigned long long)(Points != NULL ? Points[0] : 0));
    pTimeout = WinMaliSyncObjMakeTimeout_(&timeout, TimeoutNs);

    if (Count == 1) {
        singleSo = WinMaliSyncObjGet_(Adapter, Handles[0]);
        if (singleSo == NULL) {
            WINMALI_ERROR("SyncObjWait: EXIT handle=%u -> ENOENT", Handles[0]);
            return WMERR_ENOENT;
        }

        if (singleSo->IsTimeline) {
            UINT64 target = (Points != NULL) ? Points[0] : 0;
            /* Re-check loop: each pulse may not bring us to the target. */
            for (;;) {
                KIRQL oldIrql;
                BOOLEAN reached, failed;
                KeAcquireSpinLock(&singleSo->Lock, &oldIrql);
                reached = (singleSo->TimelineValue >= target);
                failed = (singleSo->TimelineFailedValue >= target && target != 0);
                KeReleaseSpinLock(&singleSo->Lock, oldIrql);
                if (failed) {
                    err = WMERR_EIO;
                    break;
                }
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
                failed = (singleSo->TimelineFailedValue >= target && target != 0);
                KeReleaseSpinLock(&singleSo->Lock, oldIrql);
                if (failed) {
                    err = WMERR_EIO;
                    break;
                }
                if (reached) {
                    err = WMERR_OK;
                    break;
                }
                st = KeWaitForSingleObject(&singleSo->Event, Executive,
                                           KernelMode, FALSE, pTimeout);
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
            UINT64 target;
            KIRQL oldIrql;

            KeAcquireSpinLock(&singleSo->Lock, &oldIrql);
            target = singleSo->BinarySubmittedValue;
            KeReleaseSpinLock(&singleSo->Lock, oldIrql);

            if (target == 0) {
                /* No GPU fence has replaced the initial binary state. */
                st = KeWaitForSingleObject(&singleSo->Event, Executive,
                                           KernelMode, FALSE, pTimeout);
                err = (st == STATUS_TIMEOUT) ? WMERR_ETIMEDOUT
                    : (NT_SUCCESS(st)        ? WMERR_OK : WMERR_EINVAL);
            } else {
                /* Capture the fence installed when the wait began. Later
                   submissions may replace the syncobj, but must not change
                   which generation this already-issued wait observes. */
                for (;;) {
                    BOOLEAN reached, failed;
                    KeAcquireSpinLock(&singleSo->Lock, &oldIrql);
                    reached = (singleSo->BinaryCompletedValue >= target);
                    failed = (singleSo->BinaryFailedValue >= target);
                    KeReleaseSpinLock(&singleSo->Lock, oldIrql);
                    if (failed) {
                        err = WMERR_EIO;
                        break;
                    }
                    if (reached) {
                        err = WMERR_OK;
                        break;
                    }
                    KeClearEvent(&singleSo->Event);
                    KeAcquireSpinLock(&singleSo->Lock, &oldIrql);
                    reached = (singleSo->BinaryCompletedValue >= target);
                    failed = (singleSo->BinaryFailedValue >= target);
                    KeReleaseSpinLock(&singleSo->Lock, oldIrql);
                    if (failed) {
                        err = WMERR_EIO;
                        break;
                    }
                    if (reached) {
                        err = WMERR_OK;
                        break;
                    }
                    st = KeWaitForSingleObject(&singleSo->Event, Executive,
                                               KernelMode, FALSE, pTimeout);
                    if (st == STATUS_TIMEOUT) {
                        err = WMERR_ETIMEDOUT;
                        break;
                    }
                    if (!NT_SUCCESS(st)) {
                        err = WMERR_EINVAL;
                        break;
                    }
                }
            }
        }
        {
            KIRQL oldIrql;
            UINT64 current;
            UINT64 failed;
            BOOLEAN timeline = singleSo->IsTimeline;
            KeAcquireSpinLock(&singleSo->Lock, &oldIrql);
            current = timeline ? singleSo->TimelineValue
                               : singleSo->BinaryCompletedValue;
            failed = timeline ? singleSo->TimelineFailedValue
                              : singleSo->BinaryFailedValue;
            KeReleaseSpinLock(&singleSo->Lock, oldIrql);
            WINMALI_ROUTINE("SyncObjWait: EXIT handle=%u point=%llu timeline=%u current=%llu failed=%llu err=%d",
                         Handles[0],
                         (unsigned long long)(Points != NULL ? Points[0] : 0),
                         timeline, (unsigned long long)current,
                         (unsigned long long)failed, err);
        }
        WinMaliSyncObjPut_(singleSo);
        return err;
    }

    /* Multi-handle path: binary-only for now. Each handle's KEVENT joins
       the wait array. */
    {
        PVOID  objects[MAXIMUM_WAIT_OBJECTS];
        PWINMALI_SYNCOBJ refs[MAXIMUM_WAIT_OBJECTS];
        ULONG i;
        if (Count > MAXIMUM_WAIT_OBJECTS) {
            WINMALI_ERROR("SyncObjWait: EXIT count=%u exceeds max=%u",
                          Count, MAXIMUM_WAIT_OBJECTS);
            return WMERR_EINVAL;
        }
        for (i = 0; i < Count; ++i) {
            refs[i] = WinMaliSyncObjGet_(Adapter, Handles[i]);
            if (refs[i] == NULL) {
                while (i > 0) {
                    WinMaliSyncObjPut_(refs[--i]);
                }
                WINMALI_ERROR("SyncObjWait: EXIT missing handle[%u]=%u",
                              i, Handles[i]);
                return WMERR_ENOENT;
            }
            if (refs[i]->IsTimeline) {
                /* Multi-handle timeline wait isn't implemented. */
                while (i + 1 > 0) {
                    WinMaliSyncObjPut_(refs[i--]);
                    if (i == (ULONG)-1) {
                        break;
                    }
                }
                WINMALI_ERROR("SyncObjWait: EXIT multi-handle timeline unsupported");
                return WMERR_EINVAL;
            }
            objects[i] = &refs[i]->Event;
        }
        st = KeWaitForMultipleObjects(Count, objects,
                                      WaitAll ? WaitAll : WaitAny,
                                      Executive, KernelMode, FALSE,
                                      pTimeout, NULL);
        for (i = 0; i < Count; ++i) {
            WinMaliSyncObjPut_(refs[i]);
        }
        if (st == STATUS_TIMEOUT) {
            WINMALI_DIAG("SyncObjWait: EXIT multi count=%u err=%d",
                         Count, WMERR_ETIMEDOUT);
            return WMERR_ETIMEDOUT;
        }
        if (!NT_SUCCESS(st)) {
            WINMALI_ERROR("SyncObjWait: EXIT multi count=%u waitStatus=0x%08x err=%d",
                          Count, st, WMERR_EINVAL);
            return WMERR_EINVAL;
        }
        WINMALI_DIAG("SyncObjWait: EXIT multi count=%u err=0", Count);
        return WMERR_OK;
    }
}

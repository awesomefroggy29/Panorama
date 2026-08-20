/*
 * WinMaliSync.h - SyncObj handle table + dispatch surface.
 *
 * SyncObjs are u32 handles exposed via WinMaliEscape ops 0x82..0x84. Two
 * flavours mirror drmSyncobj:
 *
 *   Binary (default): one signalled/unsignalled bit, backed by KEVENT.
 *                     Wait succeeds when signalled; signal sets the bit.
 *
 *   Timeline (FLAG_TIMELINE): a u64 counter that monotonically increases.
 *                     Wait succeeds when counter >= requested point.
 *                     Backed by KEVENT + counter under a spinlock; signaler
 *                     advances the counter and pulses the event so waiters
 *                     re-check.
 *
 * Handle table is per-adapter, mirroring WinMaliBo.c (LIST_ENTRY + KSPIN_LOCK
 * + monotonic counter). Refcount lets future code (queue submit) hold a
 * reference while a wait/signal is outstanding.
 */

#pragma once

#include <ntddk.h>

struct _WINMALI_ADAPTER;
typedef struct _WINMALI_ADAPTER WINMALI_ADAPTER, *PWINMALI_ADAPTER;

#define WINMALI_SYNCOBJ_MAGIC   'oSmW'  /* 'WmSo' little-endian */

typedef struct _WINMALI_SYNCOBJ {
    LIST_ENTRY          Link;
    ULONG               Magic;
    ULONG               Handle;
    BOOLEAN             IsTimeline;     /* TRUE for timeline-mode handles */
    KEVENT              Event;          /* NotificationEvent so all waiters wake */
    KSPIN_LOCK          Lock;           /* protects values below */
    UINT64              TimelineValue;  /* monotonic timeline value */
    /* A binary drm_syncobj represents the fence most recently installed in
       it, not a permanently-signalled bit.  Give every queued GPU signal a
       generation so completion of submission N cannot satisfy a wait for
       submission N+1 when both reuse the same binary handle. */
    UINT64              BinarySubmittedValue;
    UINT64              BinaryCompletedValue;
    UINT64              BinaryFailedValue;
    UINT64              TimelineFailedValue;
    LONG                RefCount;
    /* dxgk device (WINMALI_KMD_DEVICE*) whose escape created this handle;
       freed by DestroyDevice rundown if the UMD never destroys it. */
    PVOID               OwnerDevice;
} WINMALI_SYNCOBJ, *PWINMALI_SYNCOBJ;

typedef struct _WINMALI_SYNCOBJ_TABLE {
    KSPIN_LOCK          Lock;
    LIST_ENTRY          Head;
    LONG                NextHandle;
    LONG                Count;
} WINMALI_SYNCOBJ_TABLE, *PWINMALI_SYNCOBJ_TABLE;

VOID WinMaliSyncObjTableInit(_Out_ PWINMALI_SYNCOBJ_TABLE Table);
VOID WinMaliSyncObjTableTeardown(_Inout_ PWINMALI_ADAPTER Adapter,
                                 _Inout_ PWINMALI_SYNCOBJ_TABLE Table);

/* Returns 0/-errno. *OutHandle is set on success. */
int WinMaliSyncObjCreate(_Inout_ PWINMALI_ADAPTER Adapter,
                         _In_ ULONG Flags,
                         _In_ ULONG InitialState,
                         _In_opt_ PVOID OwnerDevice,
                         _Out_ ULONG* OutHandle);

int WinMaliSyncObjDestroy(_Inout_ PWINMALI_ADAPTER Adapter,
                          _In_ ULONG Handle);

/* Destroy every sync obj owned by OwnerDevice (device rundown). Returns
   the number destroyed. */
ULONG WinMaliSyncObjRundownOwner(_Inout_ PWINMALI_ADAPTER Adapter,
                                 _In_ PVOID OwnerDevice);

/* Signal one handle to the given timeline point. For binary handles,
   any non-zero Point is treated as "signal". Returns 0/-errno. */
int WinMaliSyncObjSignal(_Inout_ PWINMALI_ADAPTER Adapter,
                         _In_ ULONG Handle,
                         _In_ UINT64 Point);

/* Complete a queued fence with an execution error. Waiters for that exact
   generation wake and receive -EIO instead of mistaking the wake for GPU
   success. */
int WinMaliSyncObjFail(_Inout_ PWINMALI_ADAPTER Adapter,
                       _In_ ULONG Handle,
                       _In_ UINT64 Point);

/* Install a new pending GPU fence in a signal syncobj. Timeline handles keep
   their caller-supplied point. Binary handles receive a private generation in
   *InOutPoint and become unsignalled until that generation completes. */
int WinMaliSyncObjArmSignal(_Inout_ PWINMALI_ADAPTER Adapter,
                            _In_ ULONG Handle,
                            _Inout_ UINT64* InOutPoint);

/* Validate that a submission's opaque sync handle still names a live object
   and that the advertised handle type matches it. OwnerDevice is optional:
   WinMali submission syncs are adapter-wide because Mesa's escape transport
   can use a different D3DKMT device from its Direct3D render context. */
BOOLEAN WinMaliSyncObjValidate(_Inout_ PWINMALI_ADAPTER Adapter,
                               _In_ ULONG Handle,
                               _In_opt_ PVOID OwnerDevice,
                               _In_ BOOLEAN ExpectTimeline);

/* Wait on an array of handles with optional timeline points.
   Handles[i] is the handle, Points[i] is the timeline value to wait for
   (ignored for binary; 0 on binary means "any signal").
   WaitAll => all must reach; otherwise any one suffices.
   TimeoutNs == 0 polls, < 0 waits forever, > 0 waits up to TimeoutNs.
   Returns 0/-errno. */
int WinMaliSyncObjWait(_Inout_ PWINMALI_ADAPTER Adapter,
                       _In_ ULONG Count,
                       _In_reads_(Count) const ULONG* Handles,
                       _In_reads_opt_(Count) const UINT64* Points,
                       _In_ BOOLEAN WaitAll,
                       _In_ INT64 TimeoutNs);

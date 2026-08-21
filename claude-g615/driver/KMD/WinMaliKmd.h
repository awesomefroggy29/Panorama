
#pragma once

#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>

/* Redirects DbgPrint -> WinMaliLogPrint (kd + UART2 serial mirror) for
   every TU that includes this header. See WinMaliLog.h. */
#include "WinMaliLog.h"

#include "..\Shared\WinMaliEscape.h"   /* defines WINMALI_POOL_TAG */
#include "hw\WinMaliHw.h"
#include "WinMaliMmu.h"
#include "WinMaliAlloc.h"
#include "WinMaliBo.h"
#include "WinMaliSync.h"
#include "WinMaliVm.h"
#include "WinMaliGroup.h"
#include "WinMaliVidPn.h"

/* Driver-wide trace shim. WPP isn't wired yet; route everything through
   DbgPrint so the kdnet/COM trace shows what's happening. */
#define WINMALI_TRACE(fmt, ...) DbgPrint("[WinMali] " fmt "\n", ##__VA_ARGS__)
#define WINMALI_WARN(fmt, ...)  DbgPrint("[WinMali] WARN " fmt "\n", ##__VA_ARGS__)
#define WINMALI_ERROR(fmt, ...) DbgPrint("[WinMali] ERR  " fmt "\n", ##__VA_ARGS__)

/*
 * Per-adapter context. dxgk hands MiniportDeviceContext back to us on
 * every DDI entry; the magic guards against the "callback fired before
 * AddDevice / after RemoveDevice" race we want to crash loudly on
 * instead of dereferencing uninitialised memory.
 */
typedef struct _WINMALI_ADAPTER {
    ULONG               Magic;
#define WINMALI_ADAPTER_MAGIC   'MniW'   /* 'WinM' little-endian */

    PDEVICE_OBJECT      PhysicalDeviceObject;

    /* Captured in StartDevice; cleared in StopDevice. The DxgkHandle is
       the back-reference dxgkrnl wants on every DxgkCb*. */
    HANDLE              DxgkHandle;
    DXGKRNL_INTERFACE   DxgkInterface;
    DXGK_START_INFO     DxgkStartInfo;
    DXGK_DEVICE_INFO    DeviceInfo;

    BOOLEAN             Started;

    /* Resources parsed from ACPI (DXGK device-info). */
    PHYSICAL_ADDRESS    GpuRegsPhys;
    ULONG               GpuRegsSize;
    PVOID               GpuRegsVa;
    BOOLEAN             GpuRegsMapped;

    ULONG               GpuIrqVector;
    KIRQL               GpuIrqLevel;
    KAFFINITY           GpuIrqAffinity;
    KINTERRUPT_MODE     GpuIrqMode;
    BOOLEAN             GpuIrqShareable;
    BOOLEAN             InterruptConnected;

    /* Embedded HW context. RegsVa is NULL until BringupHardware succeeds;
       all accessors in WinMaliHw.c guard against it. */
    WINMALI_HW          Hw;

    /* Bring-up state. Bits in WINMALI_ADAPTER_FLAG_* set as each stage
       completes; the diag-passed bit is for the future escape channel. */
    ULONG               AdapterFlags;

    /* Counters maintained by the ISR. */
    LONG64              InterruptsTotal;
    LONG64              InterruptsHandled;
    LONG64              InterruptsSpurious;

    /* Contiguous non-cached heap: page tables, FW page tables, MMU scratch. */
    PVOID               MmuScratchHeapVa;
    PHYSICAL_ADDRESS    MmuScratchHeapPhys;
    SIZE_T              MmuScratchHeapBytes;
    BOOLEAN             GpuMmuAsBound;
    ULONG               GpuMmuBringupAs;

    /* Preallocated L3 tables backing the aperture GPU-VA window
       (WINMALI_SYSMEM_GPU_BASE .. +WINMALI_DMA_SEGMENT_BYTES) in the
       bring-up AS. One L3 per 2 MiB, wired into L2 at MmuInit so
       MAP_APERTURE_SEGMENT can never fail with a status dxgmms2
       bugchecks on (0x10E_b). Non-cached like the rest of the PTs. */
    PVOID               ApertureL3Va;
    PHYSICAL_ADDRESS    ApertureL3Phys;
    SIZE_T              ApertureL3Bytes;

    /* AS-slot allocator. AS0 is the CSF MCU; AS1 is the kernel bring-up
       address space. AS2..AS(N-1) are leased per dxgk context. */
    KSPIN_LOCK          AsSlotLock;
    ULONG               AsSlotInUseMask;     /* bit i set => AS i programmed */
    ULONG               AsSlotPresentMask;   /* mirror of GPU_AS_PRESENT */
#define WINMALI_AS_SLOT_MAX  16u
    struct {
        HANDLE  hContext;        /* dxgk-owned, opaque to us */
        UINT64  RootPtPa;        /* last programmed root-PT phys addr */
        BOOLEAN Bound;
    } AsBindings[WINMALI_AS_SLOT_MAX];

    /* CSF firmware image + MCU AS0 binding. */
    struct _WINMALI_FWCTX* FwCtx;
    BOOLEAN             FwMcuAsBound;
    BOOLEAN             GpuFwParkedForD3;

    /* Dedicated PopulatedFromSystemMemory segment for dxgk DMA buffers.
       Independent of MmuScratchHeap. Populated when QUERYSEGMENT3 starts
       publishing real segments; until then, fields are zero. */
    PVOID               DmaSegmentVa;
    PHYSICAL_ADDRESS    DmaSegmentPhys;
    SIZE_T              DmaSegmentBytes;

    /* Monotonic fences for submit / completion. Reset on TDR. */
    LONG64              GpuSubmittedFence;
    LONG64              GpuCompletedFence;
    LONG                NotifyDpcPending;

    /*
     * ---- PRESENT-PATH EVIDENCE ------------------------------------------
     * THE ORACLE PROBLEM: the panel shows a live image from the EDK2
     * framebuffer whether or not our present path runs, so "the screen looks
     * right" proves NOTHING. These counters are the only honest answer to
     * "did dxgk actually exercise the flip path", and StopDevice publishes
     * them to Parameters\Stat* so they are readable over WinRM.
     *
     * Written at DIRQL (SetVidPnSourceAddress can run up to PROFILE_LEVEL-1),
     * so Interlocked only -- no locks, no allocation, no paging.
     */
    LONG64              FlipLatchCount;      /* SetVidPnSourceAddress calls   */
    LONG64              LastFlipAddress;     /* dxgk's segment address, NOT a
                                                CPU PA -- do not conflate with
                                                WINMALI_FB_SCANOUT_PA         */
    LONG                LastFlipSegment;
    LONG                LastFlipFlags;
    LONG                SourceVisible;       /* SetVidPnSourceVisibility      */
    LONG                SourceVisibleEver;   /* latched: was it EVER made visible? */
    LONG64              VsyncArmCount;       /* ControlInterrupt(2|3) enables */
    LONG64              VsyncNotifyCount;    /* CRTC_VSYNC notifications sent */

    /*
     * GPU/MMU FAULT LATCH (written by the ISR).
     *
     * The ISR used to log the FIRST real fault per boot behind a one-shot and
     * then W1C-ack every later one with NO record at all. That is how a draw
     * could fault and still report seqno-complete: nothing counted the fault,
     * so nothing could attribute it to a submit. Measured consequence - every
     * draw faulting TRANSLATION_FAULT_3 while the UMD believed it rendered, so
     * the shell just span retrying (explorer burning a full core).
     *
     * COUNT every real fault and latch the LATEST one's details. A submit can
     * then sample the counter before and after and know whether IT faulted,
     * instead of relying on a boot-global flag - "sample counters INSIDE the
     * event", not once per boot.
     *
     * Written at DIRQL: Interlocked / naturally-aligned 32-bit stores only. No
     * locks, no allocation, no paging.
     */
    /*
     * APERTURE-MAPPED ALLOCATIONS, for the present blt.
     *
     * ⛔ THE BUG THIS EXISTS TO FIX: the present worker copied from
     * DmaSegmentVa + (flipAddr - segbase). That is WRONG for an aperture
     * segment. MapApertureSegment maps the ALLOCATION'S OWN MDL pages into the
     * GPU VA window; DmaSegmentVa is a driver-side buffer that nothing renders
     * into. So we copied an empty scratch buffer to the panel every frame -
     * measured as "segment is entirely BLACK across 134217728 bytes" while
     * blits climbed and skipped stayed 0. The output path was perfect; the
     * source was never the desktop.
     *
     * Record each aperture-mapped allocation so the worker can map flipAddr
     * back to the allocation whose pages actually hold the frame.
     */
    struct _WINMALI_KMD_ALLOCATION* ApertureAllocs[32];

    /* Allocations indexed by the UMD-supplied AllocId, so OpenAllocation can
       resolve without DxgkCbGetHandleData (0 of 195 opens resolved through it).
       A list, not an array: allocations are long-lived and few enough that a
       walk is cheap, and it cannot overflow the way a fixed table would. */
    LIST_ENTRY          AllocIdList;
    KSPIN_LOCK          AllocIdLock;

    /* AS_FAULTSTATUS as it read at MMU init, AFTER attempting a W1C clear.
       AS_FAULTSTATUS is sticky and survives device restarts, so "non-zero" does
       NOT mean "faulted": it must be compared against this baseline. Reading it
       as an absolute is what produced a phantom per-draw TRANSLATION_FAULT_3. */
    ULONG               FaultBaselineFst[WINMALI_MMU_AS_COUNT];

    LONG                FaultCount;          /* every real GPU/MMU fault      */
    LONG                FaultAs;             /* AS of the latest fault        */
    LONG                FaultFst;            /* AS_FAULTSTATUS of the latest  */
    LONG                FaultAddrLo;
    LONG                FaultAddrHi;
    LONG                FaultGpuStatus;      /* GPU_FAULT_STATUS if set       */

    /*
     * SYNTHESISED VSYNC. We own no DISP block and no real vsync line -- our one
     * ACPI interrupt is the Mali JOB (command-stream) IRQ. dxgk nevertheless
     * arms CRTC_VSYNC (92 times against a single flip, measured), and without a
     * notification the flip never retires.
     *
     * ⚠ This is a TIMER, not a scanout signal. It is paced at the panel's
     * nominal refresh and WILL drift relative to real scanout. Honest for v1,
     * where the panel is a static firmware framebuffer and nothing tears; it
     * must be replaced by a real DISP vsync before anything depends on the
     * phase being right.
     * ⚠ KTIMER/KDPC, NOT WdfTimerCreate(passive) -- that returns
     * STATUS_NOT_SUPPORTED on this platform.
     */
    /*
     * ---- V1 PRESENT: BLIT THE FLIPPED PRIMARY INTO THE LIVE SCANOUT --------
     *
     * We own no DISP block, so we cannot move a scanout base. But the panel is
     * already scanning out of the framebuffer EDK2 lit, at a FIXED CPU physical
     * address (WINMALI_FB_SCANOUT_PA). So "present" = copy the primary dxgk
     * flipped to us into that buffer.
     *
     * ⛔ Until this existed, NOTHING in the driver ever mapped or wrote
     * WINMALI_FB_SCANOUT_PA - it appeared only as a value we DESCRIBE to
     * ReleasePostDisplayOwnership. That is why arming the display path blanks
     * the panel: BasicDisplay stops painting, we take ownership, and we write
     * nothing at all.
     *
     * ⚠ The copy is ~21 MB. It must NOT run in SetVidPnSourceAddress (DIRQL)
     * nor in the vsync DPC (DISPATCH). The DPC only signals; a PASSIVE worker
     * does the copy.
     */
    /* Framebuffer as declared in _CRS (gpufb firmware). Zero on firmware that
       does not declare it - then the historical hardcoded PA is used. */
    PHYSICAL_ADDRESS    FbPhys;
    ULONGLONG           FbSize;

    PVOID               ScanoutMapVa;        /* MmMapIoSpace base of the FB resource */
    SIZE_T              ScanoutMapBytes;     /* ...and its length (unmap uses these) */
    PVOID               ScanoutVa;           /* = ScanoutMapVa + scanout offset */
    SIZE_T              ScanoutBytes;
    PVOID               PresentWorkerThread; /* PKTHREAD (referenced)          */
    KEVENT              PresentWake;
    KTIMER              PresentTimer;        /* our own ~30 Hz tick        */
    KDPC                PresentDpc;
    LONG                PresentStop;
    BOOLEAN             PresentInited;       /* timer+DPC really exist (0xD1 guard) */
    LONG64              ScanoutBltCount;     /* published: blits that ran      */
    LONG64              ScanoutBltSkipped;   /* published: unresolvable source */

    KTIMER              VsyncTimer;
    KDPC                VsyncDpc;
    LONG                VsyncEnabled;        /* armed by ControlInterrupt      */
    BOOLEAN             VsyncInit;           /* timer/DPC constructed          */

    /* Count of SubmitCommand calls whose CSF NOP round-trip failed and
       were completed CPU-side instead (degraded mode; see WinMaliDdi.c).
       Diagnostic only - nonzero means the MCU wasn't alive. */
    ULONG               CsfSubmitFailures;

    /* Pending fence-completion queue: produced by WinMaliKmdSubmitCommand,
       drained by WinMaliKmdDpcRoutine which calls DxgkCbNotifyInterrupt.
       Synchronous "GPU work done immediately" path for paging submits we
       don't actually run on the GPU yet. */
/* ⛔ 64 WAS NOT ENOUGH AND OVERFLOW USED TO DROP THE OLDEST ENTRY SILENTLY.
   A discarded DMA_COMPLETED is unrecoverable: dxgk's video scheduler tracks
   fences in order per node, so the next notify jumps the sequence and it
   bugchecks 0x119 VIDEO_SCHEDULER_INTERNAL_ERROR (P1=1, "invalid fence id",
   P2=reported, P3=expected). Seen for real: P2=1 P3=0xD - 13 outstanding,
   1 reported. Depth is now well past anything the compositor can have in
   flight, and overflow is LOUD + COUNTED rather than silent. */
#define WINMALI_FENCE_QUEUE_DEPTH  1024u
    KSPIN_LOCK          FenceQueueLock;
    ULONG               FenceQueueHead;
    ULONG               FenceQueueTail;
    ULONG               FenceQueueDropped;   /* must stay 0; published at StopDevice */
    ULONG               FenceQueuePeak;      /* high-water mark of queue occupancy */
    struct {
        UINT    SubmissionFenceId;
        UINT    NodeOrdinal;
        UINT    EngineOrdinal;
        BOOLEAN IsPreempt;              /* TRUE => notify DMA_PREEMPTED, not COMPLETED */
        UINT    LastCompletedFenceId;   /* preempt only: fence reached before preempt */
    } FenceQueue[WINMALI_FENCE_QUEUE_DEPTH];

    /* Async submit scheduler. DxgkDdiSubmitCommand used to BLOCK the dxgk
       scheduler worker on the CSF round-trip (and we lied about preemption
       caps to dodge the scheduler's preempt path -> 0xD1 in dxgmms2). Now
       SubmitCommand enqueues here and returns immediately; a dedicated KMD
       worker thread drains the queue, runs the (proven, synchronous) CSF
       submit off the scheduler thread, and completes the fence via the DPC.
       PreemptCommand enqueues an in-order marker so preemption reports the
       exact completed-fence boundary with no race. */
#define WINMALI_SUBMIT_QUEUE_DEPTH  64u
    KSPIN_LOCK          SubmitQueueLock;
    ULONG               SubmitQueueHead;
    ULONG               SubmitQueueTail;
    struct {
        UINT    FenceId;               /* submission fence, or preemption fence */
        UINT    NodeOrdinal;
        UINT    EngineOrdinal;
        BOOLEAN IsPreempt;
    } SubmitQueue[WINMALI_SUBMIT_QUEUE_DEPTH];
    PVOID               SubmitWorkerThread;   /* PKTHREAD (referenced) */
    KEVENT              SubmitWorkerWake;
    LONG                SubmitWorkerStop;
    LONG64              SchedLastCompletedFence;  /* worker-updated, in-order */

    /* Set after a successful bring-up so RemoveDevice can tell a clean
       stop/remove cycle from a never-started one. */
    BOOLEAN             StartDeviceEverSucceeded;

    /* Non-zero once StopDevice/RemoveDevice has begun. Read by
       WinMaliWaitObjectAbortable so in-flight escape waits give up instead of
       blocking PnP (a blocked PnP lock is the 0x9F P1=4 / P2=0x12C bugcheck). */

    /* UMD escape ABI: BO handle table. Initialized in AddDevice, drained
       in RemoveDevice. See WinMaliBo.c. */
    WINMALI_BO_TABLE        BoTable;

    /* UMD escape ABI: SyncObj handle table. See WinMaliSync.c. */
    WINMALI_SYNCOBJ_TABLE   SyncObjTable;

    /* UMD escape ABI: VM handle table (VmBind programs per-VM LPAE PTEs). */
    WINMALI_VM_TABLE        VmTable;

    /* UMD escape ABI: CSF queue Group handle table (GroupSubmit runs the
       UMD command streams on the CSG). */
    WINMALI_GROUP_TABLE     GroupTable;
} WINMALI_ADAPTER, *PWINMALI_ADAPTER;

/* Synthesised vsync source -- see the long comment on WinMaliVsyncDpc. */
VOID WinMaliVsyncInit(_Inout_ PWINMALI_ADAPTER Adapter);
VOID WinMaliVsyncSetEnabled(_Inout_ PWINMALI_ADAPTER Adapter, _In_ BOOLEAN Enable);
VOID WinMaliVsyncTeardown(_Inout_ PWINMALI_ADAPTER Adapter);

/* V1 present: map the live EDK2 scanout and blit the flipped primary into it.
   Without this the driver owns the display but writes nothing, so arming the
   display path simply blanks the panel. */
VOID WinMaliPresentInit(_Inout_ PWINMALI_ADAPTER Adapter);
VOID WinMaliPresentTeardown(_Inout_ PWINMALI_ADAPTER Adapter);

/* Bring-up state bits, used by the escape diagnostics channel + by
   bring-up code to gate features. */
#define WINMALI_ADAPTER_FLAG_MCU_ALIVE     0x00000001UL
#define WINMALI_ADAPTER_FLAG_MMU_READY     0x00000002UL
#define WINMALI_ADAPTER_FLAG_DIAG_PASSED   0x00000004UL
#define WINMALI_ADAPTER_FLAG_CSF_JOBS      0x00000008UL

/* Map a dxgk-supplied context (or hAdapter handle) back to our struct.
   Returns NULL on a bad-magic / NULL / mismatched-handle context so
   callers can fail with STATUS_INVALID_PARAMETER. */
PWINMALI_ADAPTER WinMaliAdapterFromContext(_In_opt_ const VOID* Context);
PWINMALI_ADAPTER WinMaliAdapterFromDxgkHandle(_In_opt_ const VOID* hAdapter);

/* ---------- DDI prototypes wired into DRIVER_INITIALIZATION_DATA ------- */

DRIVER_INITIALIZE                       DriverEntry;
VOID APIENTRY                           WinMaliKmdDxgkUnload(VOID);

/* Kill-switch (early in DriverEntry) - returns Disabled=FALSE on any error
   so a registry/I/O glitch never bricks the driver. */
NTSTATUS WinMaliReadKillSwitch(_In_ PUNICODE_STRING RegistryPath,
                               _Out_ BOOLEAN* Disabled);

/* Generic Services\WinMaliKmd\Parameters REG_DWORD reader; *Value keeps
   Default on any failure. */
/* Echo a value DriverEntry actually used back into Parameters, so a reader can
   prove which knob values are live rather than assuming a registry edit was
   picked up. See the comment on the definition. */
NTSTATUS WinMaliWriteParamsDword(_In_ PUNICODE_STRING RegistryPath,
                                 _In_ PCWSTR ValueName,
                                 _In_ ULONG Value);

/* Same, but ZwFlushKey's the hive before returning so the value SURVIVES A HANG.
   Lazy flushing loses exactly the values written just before a freeze - the ones
   that say what the driver was doing. Expensive: bounded use only. */
NTSTATUS WinMaliWriteParamsDwordFlushed(_In_ PUNICODE_STRING RegistryPath,
                                        _In_ PCWSTR ValueName,
                                        _In_ ULONG Value);

NTSTATUS WinMaliWriteParamsBinary(_In_ PUNICODE_STRING RegistryPath,
                                  _In_ PCWSTR ValueName,
                                  _In_reads_bytes_(Length) const VOID *Data,
                                  _In_ ULONG Length);

/* RegistryPath is only handed to DriverEntry, but DxgkDdiQueryInterface needs
   it to persist what dxgk asked for. Captured once at DriverEntry. */
extern UNICODE_STRING g_WinMaliRegPath;

/* Monotonic UMD-error counters (defined in WinMaliEscape.c), published to
   Parameters the moment they change. The 256 KB log ring wraps in ~2 minutes,
   so counting log lines is NOT a sound before/after oracle - two consecutive
   fresh-boot captures gave "10 refusals" and "wrapped, saw almost nothing".
   StatPoolRefusals is the number that must reach ZERO when the BO handle
   namespace collision is fixed. Driver-global, not per-adapter, so an adapter
   restart cannot reset them mid-experiment. */
/* DxgkDdiPresent outcome counters (WinMaliDdi.c). Calls == NotBlt +
   NullCookie + Blts. NullCookie is the suspected black-screen path: it means
   OpenAllocation never bound the surface, so the present silently succeeded
   without copying anything. */
extern LONG  g_WinMaliStatPresentFlips;
extern LONG  g_WinMaliStatPresentLastFlags;
extern LONG  g_WinMaliStatPresentLastCounts;
extern LONG  g_WinMaliStatPresentSrcHandle;
extern LONG  g_WinMaliStatPresentDstHandle;
extern LONG  g_WinMaliStatPresSrcRectTL;
extern LONG  g_WinMaliStatPresSrcRectBR;
extern LONG  g_WinMaliStatPresDstRectTL;
extern LONG  g_WinMaliStatPresDstRectBR;
extern LONG  g_WinMaliStatPresSubRectCnt;
extern LONG  g_WinMaliStatPresDmaSize;
extern LONG  g_WinMaliStatPresMultipass;
extern LONG  g_WinMaliStatPresColor;
extern LONG  g_WinMaliStatPresentDmaPrivSize;
extern LONG  g_WinMaliStatPresentPrivSize;
extern LONG  g_WinMaliStatPresentPrivHits;
extern LONG  g_WinMaliStatUmdPresentCtx;
extern LONG  g_WinMaliStatCtxPrivSize;
extern LONG  g_WinMaliStatApWithContent;
extern LONG  g_WinMaliStatApFirstIdx;
extern LONG  g_WinMaliStatApAdopted;
extern LONG  g_WinMaliStatApPrevSrc;
extern LONG  g_WinMaliStat0x92Calls;
extern LONG  g_WinMaliStat0x92NoEnt;
extern LONG  g_WinMaliStat0x92NotRes;
extern LONG  g_WinMaliStat0x92Ok;
/* Op 0x93 BoFromUserVa - the import path that does not need dxgk paging. */
extern LONG  g_WinMaliStat0x93Calls;
extern LONG  g_WinMaliStat0x93Ok;
extern LONG  g_WinMaliStat0x93Fail;
extern LONG  g_WinMaliStatUvSrcBo;
extern LONG  g_WinMaliStatUvSrcHits;
/* asked/cands/tooSmall exist so a `uvBo=0` can be TOLD APART: never asked vs
   nothing published vs all too small vs big enough but unpainted. */
extern LONG  g_WinMaliStatUvSrcAsked;
extern LONG  g_WinMaliStatUvSrcCands;
extern LONG  g_WinMaliStatUvSrcTooSmall;
/* Owning PID of the adopted source, and a snapshot of the whole qualifying
   field. dwm vs explorer is the difference between DWM's COMPOSED frame and one
   pre-composition layer - the latter can never carry the taskbar or the
   software-composited cursor. */
extern LONG  g_WinMaliStatUvSrcPid;
extern LONG  g_WinMaliStatUvSrcPages;
extern LONG  g_WinMaliStatUvSrcNCand;
extern LONG  g_WinMaliUvCandBo[4];
extern LONG  g_WinMaliUvCandPid[4];
extern LONG  g_WinMaliUvCandBox[4];
/* Hits = TRANSITIONS between adjacent samples (structure), NOT non-black count.
   The old non-black metric saturates - a blank white buffer scores maximum - and
   that is what put a solid white screen on the panel. NonBlack is kept only so
   runs stay comparable with earlier measurements; it does not select. */
extern LONG  g_WinMaliUvCandHits[4];
extern LONG  g_WinMaliUvCandNonBlack[4];
extern LONG  g_WinMaliStatUvSrcUniform;
/* Chg/Scans = how many scans saw the surface CHANGE. A frozen frame (composed
   once early in logon, before the taskbar existed and before the pointer moved)
   is indistinguishable from a live one by CONTENT - only by change over time. */
extern LONG  g_WinMaliUvCandChg[4];
extern LONG  g_WinMaliUvCandScans[4];
extern LONG  g_WinMaliStatUvSrcChg;
extern LONG  g_WinMaliStatUvSrcEvict;
extern LONG  g_WinMaliStatUvRevokeBig;
extern LONG  g_WinMaliTrapRevokeBig;
extern LONG  g_WinMaliStatBoDedup;
extern LONG  g_WinMaliStatUvPubBig;
extern LONG  g_WinMaliStatBoDeref;
extern ULONG g_WinMaliUvBoDedup;
extern ULONG g_WinMaliQaiDirectFlip;
extern ULONG g_WinMaliQaiMaxQueuedFlip;
extern PUCHAR g_WinMaliRevokeSnap;
extern ULONG  g_WinMaliRevokeSnapLen;
extern LONG  g_WinMaliStatUvRevokeAny;
extern LONG  g_WinMaliStatUvSrcEvictBig;
extern LONG  g_WinMaliStatUvSrcEvictBusy;
extern LONG  g_WinMaliStatUvSrcDropSmall;
extern LONG  g_WinMaliStatSubmitNoGroup;
extern LONG  g_WinMaliStatSubmitFatalGroup;
extern LONG  g_WinMaliStatCsfLastStatus;
extern LONG  g_WinMaliStatCsfLastAs;
extern LONG  g_WinMaliStatCsfLastSize;
extern LONG  g_WinMaliStatCsfLastVaLo;
extern LONG  g_WinMaliStatCsfLastVaHi;
extern LONG  g_WinMaliStatCsfFailCount;
extern LONG  g_WinMaliStatSubmitSyncFail;
extern LONG  g_WinMaliStatSubmitSyncErr;
extern LONG  g_WinMaliStatSyncBadHandle;
extern LONG  g_WinMaliStatSyncBadFlags;
/* A wait that RESOLVED but never fired. Distinct from SyncFail (unknown
   handle) on purpose: one says our bookkeeping is wrong, the other says the
   GPU is wedged, and they used to share a single ENOENT. */
extern LONG  g_WinMaliStatSyncWaitTimeout;
/* SIGNAL-phase sync failures: the CS ran to completion, then signalling failed.
   Distinct from StatSubmitSyncFail (a rejected WAIT) - different bug, own counter. */
extern LONG  g_WinMaliStatSubmitSignalFail;
extern LONG  g_WinMaliTrapSignalFail;
extern LONG  g_WinMaliTrapFirstTmo;
extern LONG  g_WinMaliStatTilerOomCount;
extern LONG  g_WinMaliStatTilerOomFail;
extern LONG  g_WinMaliStatSyncTableInits;
extern LONG  g_WinMaliStatSyncTableTeardowns;
extern LONG  g_WinMaliStatSignalErr;
extern LONG  g_WinMaliStatSignalBadHandle;
extern LONG  g_WinMaliStatCsTimeoutCount;
/* Ring pointers + req/ack pairs: the proof that the CS output iface we read the
   timeout diagnosis out of is a channel the FW actually writes. */
/* Identity of the allocation dxgk actually scans out, captured on every flip.
   The present source is otherwise chosen by a heuristic, which latched a
   pre-composition layer (desktop, no taskbar, no cursor, Chg 0). */
extern LONG  g_WinMaliFlipAllocW;
extern LONG  g_WinMaliFlipAllocH;
extern LONG  g_WinMaliFlipAllocPitch;
extern LONG  g_WinMaliFlipAllocFmt;
extern LONG  g_WinMaliFlipAllocSize;
extern LONG  g_WinMaliFlipAllocId;
extern LONG  g_WinMaliApMapOps;
extern LONG  g_WinMaliApMapLastSize;
extern LONG  g_WinMaliApMapMaxSize;
extern PVOID g_WinMaliFlipAlloc;   /* WINMALI_KMD_ALLOCATION* dxgk scans out */
extern LONG  g_WinMaliStatFlipSrcUsed;
extern LONG  g_WinMaliStatFlipSrcMapFail;
extern LONG  g_WinMaliFlipWorkerMdl;
extern LONG  g_WinMaliFlipWorkerPages;
extern LONG  g_WinMaliStatCopyAbort;  /* frames abandoned mid-copy (alloc moved) */
extern LONG  g_WinMaliStatCopyDone;   /* KB copied on the last pass */
extern LONG  g_WinMaliUvLatchBo;
extern LONG  g_WinMaliUvLastGoodBo;
extern LONG  g_WinMaliStatUvHeldOver;      /* uv candidate latched for the current sweep */
extern LONG  g_WinMaliStatLatchDrop;  /* sweeps abandoned - latched candidate died */
extern LONG  g_WinMaliStatSweeps;     /* full frames completed */
extern LONG  g_WinMaliStatUvRevokeTimeout; /* Revoke gave up waiting - must be 0 */
extern LONG  g_WinMaliStatPowerStop;  /* SetPowerState stopped the present worker */
extern LONG  g_WinMaliStatUvSrcQual;
extern LONG  g_WinMaliStatUvSrcMaxCov;
extern LONG  g_WinMaliStatUvSrcMaxChg;   /* best change-count of ANY candidate: pairs with StatUvSrcChg to split "chose wrong" from "nothing is being written" */
extern LONG  g_WinMaliStatUvSrcCoverage; /* adopted source painted extent, %% */

/* DRIVERCAPS VISIBILITY - see the long comment in WinMaliQai.c. These answer
   "do our caps land where dxgk actually reads them", which decides whether the
   cap experiments so far (NoScreenToScreenBlt, FlipImmediateMmIo) were genuine
   refutations or silent no-ops. If OutSize < CapsSizeof the running kernel has a
   SMALLER view of DXGK_DRIVERCAPS than our 26100 WDK, and our field offsets are
   addressing the wrong bytes. */
/* 0 (default) = do NOT call the WDDM1.x DxgkCbGetHandleData callback. It has
   never resolved a single allocation (0/542 measured; the AllocId table does
   100%), and dxgk logs "WDDM2 driver calls WDDM1.x DDI!" for it ~2x per rejected
   present - exactly the 2 calls OpenAllocation makes per allocation. */
extern ULONG g_WinMaliAllowHandleDataCb;

/* Segment description shape - see the long comment in WinMaliDriver.c.
   0=legacy aperture, 1=aperture+DirectFlip(default), 2=real memory segment.
   DXGK_SEGMENTFLAGS.DirectFlip (0x400) is what tells dxgk an allocation in this
   segment is permitted to be a flip target; we shipped it as 0 forever. */
extern ULONG g_WinMaliSegmentShape;

extern ULONG g_WinMaliQaiFlipCapsValue;
extern ULONG g_WinMaliQaiFlipCapsOff;
extern ULONG g_WinMaliQaiOutSize;
extern ULONG g_WinMaliQaiCapsSizeof;
extern ULONG g_WinMaliQaiSegFlags;   /* DXGK_SEGMENTFLAGS we sent; DirectFlip=0x400 */


extern LONG  g_WinMaliScanoutWc;      /* 1 = write-combined, 0 = UNCACHED (~1000x) */
extern LONG  g_WinMaliFlipHasMdl;
extern LONG  g_WinMaliFlipMdlPages;
extern LONG  g_WinMaliFlipAllocOk;
extern LONG  g_WinMaliFlipAllocBad;
extern LONG  g_WinMaliStatCsSubmitCount;
extern LONG  g_WinMaliStatCsOkCount;
extern LONG  g_WinMaliStatRingInsert;
extern LONG  g_WinMaliStatRingExtract;
extern LONG  g_WinMaliStatCsReq;
extern LONG  g_WinMaliStatCsAck;
extern LONG  g_WinMaliStatCsgReq;
extern LONG  g_WinMaliStatCsgAck;
extern LONG  g_WinMaliStatCsFatalInfoLo;
extern LONG  g_WinMaliStatCsFatalInfoHi;
extern LONG  g_WinMaliStatSeqnoLo;
extern LONG  g_WinMaliStatTagLo;
extern LONG  g_WinMaliStatCsCmdPtrLo;
extern LONG  g_WinMaliStatCsCmdPtrHi;
extern LONG  g_WinMaliStatCsBlocked;
extern LONG  g_WinMaliStatCsFault;
extern LONG  g_WinMaliStatCsFatal;
extern LONG  g_WinMaliStatCsCalleeRet;
extern LONG  g_WinMaliStatAsFaultSt;
extern LONG  g_WinMaliStatAsFaultLo;
extern LONG  g_WinMaliStatAsFaultHi;
extern LONG  g_WinMaliStatMcuFaultSt;
extern LONG  g_WinMaliStatGpuFaultSt;
extern LONG  g_WinMaliUvCandPages[4];
/* Allocation DWM last flipped to; the present worker copies from this. */
extern PVOID g_WinMaliFlipSourceAlloc;

extern LONG g_WinMaliStatPawmRegistered;
extern LONG g_WinMaliStatOpenByPawm;
extern LONG g_WinMaliStatOpenCalls;
extern LONG g_WinMaliStatOpenResolved;
extern LONG g_WinMaliStatOpenById;
extern LONG g_WinMaliStatOpenFailed;
extern LONG g_WinMaliStatOpenLastId;
extern LONG g_WinMaliStatOpenFailPdMagic;
extern LONG g_WinMaliStatOpenFailPdSize;
extern LONG g_WinMaliStatPresentCalls;
extern LONG g_WinMaliStatPresentNotBlt;
extern LONG g_WinMaliStatPresentNullCookie;
extern LONG g_WinMaliStatPresentBlts;

extern LONG g_WinMaliStatPoolRefusals;
extern LONG g_WinMaliStatShortMappings;
extern LONG g_WinMaliStatUndersizedBos;

extern ULONG g_WinMaliFeatureIface;
extern ULONG g_WinMaliVidPnSources;
extern ULONG g_WinMaliHybridRole;
extern ULONG g_WinMaliChildCfg;

/* Non-zero once teardown has begun (StopDevice/RemoveDevice). */
extern volatile LONG g_WinMaliStopping;
extern ULONG g_WinMaliPresentTestPattern;
/* DEFAULT 0 - LEAVE IT 0. 1 lets the worker read the flip alloc through VidMm's
   aperture MDL, which DEADLOCKS the machine (fault -> VidMm paging op -> serviced
   by us -> our worker is the blocked thread). Kept only to re-test on purpose. */
extern ULONG g_WinMaliFlipSrcFromVidMm;
/* FlipSrcFromAperture (default 0): allow the present worker to read a dxgk
   allocation's APERTURE MDL as the frame source. Same deadlock class as
   FlipSrcFromVidMm - it was simply never gated, and was mislabelled in the
   source as a 0x93 read. Leave 0; 0x93 (srcPath 3) is the only safe source. */
extern ULONG g_WinMaliFlipSrcFromAperture;
extern ULONG g_WinMaliPresentCopyBudgetKB; /* 0 = whole frame per wake */
extern LONG  g_WinMaliStatSkipNoSrc;
extern ULONG g_WinMaliPresentMarker;
extern ULONG g_WinMaliTraceSubmit;
extern ULONG g_WinMaliPresentTimerHz;
extern ULONG g_WinMaliFailOnGpuFault;
extern ULONG g_WinMaliTilerOomCtxSplice;
extern ULONG g_WinMaliPresentSettleMs;
extern ULONG g_WinMaliQaiFlipCaps;
/* UvSrcInvalidate (default 1): invalidate the scanout source before reading it.
   Correct while the GPU is the last writer (it writes non-coherently). ⚠ WRONG
   once delivery into that surface is a CPU copy - an invalidate can DISCARD the
   CPU's still-dirty lines, so the worker would read stale DRAM and the surface
   would look frozen even though the copy ran. A knob, so the two policies can be
   told apart in one deploy instead of two. */
extern ULONG g_WinMaliUvSrcInvalidate;

/* Bounded, termination-aware replacement for KeWaitForSingleObject on any path
   a user thread can reach through an escape. Timeout==NULL means "no caller
   deadline", still bounded by the abort checks. Returns
   STATUS_THREAD_IS_TERMINATING or STATUS_DEVICE_REMOVED when it gives up.
   See the long comment on the definition in WinMaliSync.c (bugcheck 0x9F). */
NTSTATUS WinMaliWaitObjectAbortable(_In_ PVOID Object,
                                    _In_opt_ PLARGE_INTEGER Timeout,
                                    _In_ ULONG SliceMs);

/* ChildUid for the single video-output child we describe when VidPnSources != 0.
   Driver-defined; dxgk uses it to key QueryChildStatus. */
#define WINMALI_CHILD_UID 0

NTSTATUS WinMaliReadParamsDword(_In_ PUNICODE_STRING RegistryPath,
                                _In_ PCWSTR ValueName,
                                _In_ ULONG Default,
                                _Out_ ULONG* Value);

/* Bring-up policy: reject dwm.exe's CreateDevice so the compositor stops
   churning devices/VMs/AS-slots on the render-only adapter. Read from
   Parameters\AllowDwm at DriverEntry (default 0 = blocked); set to 1 +
   reboot to let DWM back in without a rebuild. (v28's bugcheck-with-gate
   was the DriverUnload 0x139 bug, fixed v32 - not this.) */
extern ULONG g_WinMaliAllowDwm;
extern ULONG g_WinMaliBringupStage;

/* DXGK_DRIVERCAPS.MiscCaps.ComputeOnly. Read from Parameters\ComputeOnly at
   DriverEntry, DEFAULT 1 (the MT6897 fix -- see the long comment at the
   assignment in WinMaliQai.c).
   MEASURED 2026-08-11: with ComputeOnly=1 the adapter starts clean
   (CM_PROB_NONE, no DxgKrnl-Admin 549) but is enumerated by NEITHER DXGI nor
   DXCore, so no user-mode driver is ever loaded and OpenAdapter10 is never
   called. DXGI excludes MCDM adapters by design, and DXCore has nothing to
   offer because no Dx12UserModeDriverName (a D3D12 compute UMD) is registered.
   This knob exists to test ComputeOnly=0 without a rebuild each time; the
   oracle is DxgKrnl-Admin event id 549 (StartAdapter_AddAdapterFailed). */
extern ULONG g_WinMaliComputeOnly;

/* Auto-arm the kill-switch from inside DriverEntry. Used to prevent the
   second DriverEntry-after-teardown crash we hit on Win11 26100 dxgk - if
   the driver has been through one full bring-up + teardown cycle, dxgk's
   tracking lists are in an inconsistent state on retry. Arming the
   kill-switch makes the retry DriverEntry early-exit before touching
   anything. Manual recovery: unkill.ps1 -Reboot. */
NTSTATUS WinMaliArmKillSwitch(_In_ PUNICODE_STRING RegistryPath);

/* Resource parser + hardware bring-up. Defined in WinMaliBringup.c. */
NTSTATUS WinMaliParseResources(_Inout_ PWINMALI_ADAPTER Adapter);
NTSTATUS WinMaliBringupHardware(_Inout_ PWINMALI_ADAPTER Adapter);
VOID     WinMaliTeardownHardware(_Inout_ PWINMALI_ADAPTER Adapter);
NTSTATUS WinMaliConnectInterrupt(_Inout_ PWINMALI_ADAPTER Adapter);
VOID     WinMaliDisconnectInterrupt(_Inout_ PWINMALI_ADAPTER Adapter);

/* MMU + firmware bring-up. Defined in WinMaliMmu.c / WinMaliFw.c. */
NTSTATUS WinMaliFwInit(_Inout_ PWINMALI_ADAPTER Adapter);
VOID     WinMaliFwTeardown(_Inout_ PWINMALI_ADAPTER Adapter);

/* Submit a single no-op CSF job to the kernel queue. Used as a self-test
   after FwInit succeeds - if this returns STATUS_SUCCESS we know the MCU
   is alive, the CS iface is programmed, and the queue ring works end-to-end. */
NTSTATUS WinMaliCsfSubmitNopJob(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG TimeoutMs);

/* Real UMD command-stream submission: bind the CSG to the group's VM AS
   (GroupAsSlot) and execute the UMD's CS stream at StreamGpuVa. On failure
   the caller should fall back to WinMaliCsfSubmitNopJob. */
/* Grow the tiler heap of the VM bound to AsSlot by one chunk, to service a
   firmware TILER_OOM request. Defined in WinMaliEscape.c beside the heap
   creator (it reuses that file's static chunk allocator). */
NTSTATUS WinMaliTilerHeapGrow(_Inout_ PWINMALI_ADAPTER Adapter,
                              _In_    ULONG            AsSlot,
                              _Out_   UINT64*          OutChunkVa,
                              _Out_   ULONG*           OutChunkSize);

NTSTATUS WinMaliCsfSubmitGroupStream(_Inout_ PWINMALI_ADAPTER Adapter,
                                     _In_ ULONG   GroupAsSlot,
                                     _In_ UINT64  StreamGpuVa,
                                     _In_ ULONG   StreamSizeBytes,
                                     _In_ ULONG   LatestFlush,
                                     _In_ ULONG   TimeoutMs);

/* Cross-map the CSF ring/sync/shader pages into a UMD group's VM at the
   fixed low GPU VAs so the CSG can execute the group's stream in its own AS.
   Called from VmCreate; best-effort (DEVICE_NOT_READY before CSF boots). */
NTSTATUS WinMaliCsfMapIntoVm(_Inout_ PWINMALI_ADAPTER Adapter,
                             _Inout_ PWINMALI_VM_PT VmPt);

/* 256 MiB sysmem-backed segment that VIDMM publishes as "segment 1". */
NTSTATUS WinMaliVidmmAllocateSegment(_Inout_ PWINMALI_ADAPTER Adapter);
VOID     WinMaliVidmmFreeSegment(_Inout_ PWINMALI_ADAPTER Adapter);
NTSTATUS APIENTRY WinMaliKmdGetNodeMetadata(IN_CONST_HANDLE hAdapter, UINT NodeOrdinalAndAdapterIndex, OUT_PDXGKARG_GETNODEMETADATA pGetNodeMetadata);
NTSTATUS APIENTRY WinMaliKmdCreateProcess(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_CREATEPROCESS pCreateProcess);
NTSTATUS APIENTRY WinMaliKmdDestroyProcess(IN_CONST_HANDLE hAdapter, IN_CONST_HANDLE hKmdProcess);
NTSTATUS APIENTRY WinMaliKmdCreateDevice(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_CREATEDEVICE pCreateDevice);
NTSTATUS APIENTRY WinMaliKmdDestroyDevice(IN_CONST_HANDLE hDevice);
NTSTATUS APIENTRY WinMaliKmdCreateContext(IN_CONST_HANDLE hDevice, INOUT_PDXGKARG_CREATECONTEXT pCreateContext);
NTSTATUS APIENTRY WinMaliKmdDestroyContext(IN_CONST_HANDLE hContext);
VOID     APIENTRY WinMaliKmdSetRootPageTable(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SETROOTPAGETABLE pArgs);
SIZE_T   APIENTRY WinMaliKmdGetRootPageTableSize(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_GETROOTPAGETABLESIZE pArgs);
NTSTATUS APIENTRY WinMaliKmdQueryCurrentFence(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_QUERYCURRENTFENCE pArgs);
NTSTATUS APIENTRY WinMaliKmdControlInterrupt(IN_CONST_HANDLE hAdapter, IN_CONST_DXGK_INTERRUPT_TYPE InterruptType, IN_BOOLEAN EnableInterrupt);
NTSTATUS APIENTRY WinMaliKmdResetFromTimeout(IN_CONST_HANDLE hAdapter);
NTSTATUS APIENTRY WinMaliKmdRestartFromTimeout(IN_CONST_HANDLE hAdapter);
NTSTATUS APIENTRY WinMaliKmdCollectDbgInfo(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_COLLECTDBGINFO pArgs);
NTSTATUS APIENTRY WinMaliKmdBuildPagingBuffer(IN_CONST_HANDLE hAdapter, IN_PDXGKARG_BUILDPAGINGBUFFER pBpb);
NTSTATUS APIENTRY WinMaliKmdSubmitCommand(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SUBMITCOMMAND pSubmit);
NTSTATUS APIENTRY WinMaliKmdSubmitCommandVirtual(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SUBMITCOMMANDVIRTUAL pSubmit);
NTSTATUS APIENTRY WinMaliKmdPreemptCommand(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_PREEMPTCOMMAND pPreempt);
NTSTATUS APIENTRY WinMaliKmdCancelCommand(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_CANCELCOMMAND pCancel);
NTSTATUS APIENTRY WinMaliKmdQueryEngineStatus(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_QUERYENGINESTATUS pArgs);
NTSTATUS APIENTRY WinMaliKmdResetEngine(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_RESETENGINE pReset);
NTSTATUS APIENTRY WinMaliKmdEscape(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_ESCAPE pEscape);

/* WinMaliDdi.c: async submit scheduler lifecycle. Init creates the worker
   thread (call from StartDevice after CSF is up); Teardown stops+joins it
   (call from StopDevice before FW teardown). Safe to call Teardown if Init
   never ran / already torn down. */
NTSTATUS WinMaliSchedInit(_Inout_ PWINMALI_ADAPTER Adapter);
VOID     WinMaliSchedTeardown(_Inout_ PWINMALI_ADAPTER Adapter);

/* WinMaliDdi.c: validate a dxgk device handle (DXGKARG_ESCAPE.hDevice) and
   return it as the opaque owner token stamped on escape-created resources
   (VM/BO/SyncObj/Group) so DestroyDevice can run them down. NULL if the
   handle is absent or foreign. */
PVOID WinMaliDeviceOwnerToken(_In_opt_ HANDLE hDevice);

/* WinMaliDdi.c: resolve a dxgk device handle to its adapter (NULL if
   absent/foreign). For DDIs that only receive hDevice. */
PWINMALI_ADAPTER WinMaliAdapterFromDeviceHandle(_In_opt_ HANDLE hDevice);

/* Segment id constants. dxgk segment ids are 1-based; id 1 is the
   sysmem-backed segment we allocate at StartDevice. */
#define WINMALI_SEGMENT_ID_SYSMEM   1u
#define WINMALI_SEGMENT_COUNT       1u

/* GPU-side base addresses we report in DXGK_SEGMENTDESCRIPTOR*. dxgk
   uses this as a hint for where this segment occupies the GPU VA
   space. Must be above PHYSICALADAPTERCAPS.MinimumAddress (= 0x10000)
   and avoid the firmware reservations < 0x04040000. */
#define WINMALI_SYSMEM_GPU_BASE     0x0000000010000000ULL  /* 256 MiB */

NTSTATUS APIENTRY WinMaliKmdAddDevice(
    _In_     CONST PDEVICE_OBJECT PhysicalDeviceObject,
    _Outptr_ PVOID*               MiniportDeviceContext);

NTSTATUS APIENTRY WinMaliKmdStartDevice(
    _In_  CONST PVOID            MiniportDeviceContext,
    _In_  PDXGK_START_INFO       DxgkStartInfo,
    _In_  PDXGKRNL_INTERFACE     DxgkInterface,
    _Out_ PULONG                 NumberOfVideoPresentSources,
    _Out_ PULONG                 NumberOfChildren);

NTSTATUS APIENTRY WinMaliKmdStopDevice(
    _In_ CONST PVOID MiniportDeviceContext);

NTSTATUS APIENTRY WinMaliKmdRemoveDevice(
    _In_ CONST PVOID MiniportDeviceContext);

NTSTATUS APIENTRY WinMaliKmdDispatchIoRequest(
    _In_ CONST PVOID                MiniportDeviceContext,
    _In_ ULONG                      VidPnSourceId,
    _In_ PVIDEO_REQUEST_PACKET      VideoRequestPacket);

BOOLEAN APIENTRY WinMaliKmdInterruptRoutine(
    _In_ CONST PVOID MiniportDeviceContext,
    _In_ ULONG       MessageNumber);

VOID APIENTRY WinMaliKmdDpcRoutine(
    _In_ CONST PVOID MiniportDeviceContext);

NTSTATUS APIENTRY WinMaliKmdExchangePreStartInfo(
    _In_    CONST PVOID            MiniportDeviceContext,
    _Inout_ PDXGK_PRE_START_INFO   PreStartInfo);

NTSTATUS APIENTRY WinMaliKmdQueryChildRelations(
    _In_                             CONST PVOID            MiniportDeviceContext,
    _Out_writes_bytes_(ChildRelationsSize) PDXGK_CHILD_DESCRIPTOR ChildRelations,
    _In_                             ULONG                  ChildRelationsSize);

NTSTATUS APIENTRY WinMaliKmdQueryChildStatus(
    _In_    CONST PVOID         MiniportDeviceContext,
    _In_    PDXGK_CHILD_STATUS  ChildStatus,
    _In_    BOOLEAN             NonDestructiveOnly);

NTSTATUS APIENTRY WinMaliKmdQueryDeviceDescriptor(
    _In_  CONST PVOID                  MiniportDeviceContext,
    _In_  ULONG                        ChildUid,
    _Inout_ PDXGK_DEVICE_DESCRIPTOR    DeviceDescriptor);

NTSTATUS APIENTRY WinMaliKmdSetPowerState(
    _In_ CONST PVOID         MiniportDeviceContext,
    _In_ ULONG               DeviceUid,
    _In_ DEVICE_POWER_STATE  DevicePowerState,
    _In_ POWER_ACTION        ActionType);

NTSTATUS APIENTRY WinMaliKmdNotifyAcpiEvent(
    _In_  CONST PVOID         MiniportDeviceContext,
    _In_  DXGK_EVENT_TYPE     EventType,
    _In_  ULONG               Event,
    _In_  PVOID               Argument,
    _Out_ PULONG              AcpiFlags);

VOID APIENTRY WinMaliKmdResetDevice(
    _In_ CONST PVOID MiniportDeviceContext);

NTSTATUS APIENTRY WinMaliKmdQueryInterface(
    _In_ CONST PVOID    MiniportDeviceContext,
    _In_ PQUERY_INTERFACE QueryInterface);

VOID APIENTRY WinMaliKmdControlEtwLogging(
    _In_ BOOLEAN Enable,
    _In_ ULONG   Flags,
    _In_ UCHAR   Level);

NTSTATUS APIENTRY WinMaliKmdQueryAdapterInfo(
    _In_ CONST HANDLE                       hAdapter,
    _In_ CONST DXGKARG_QUERYADAPTERINFO*    pQueryAdapterInfo);







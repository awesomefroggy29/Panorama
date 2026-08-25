
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
    /* ⛔ SERIALIZES AS-SLOT OWNERSHIP CHANGES AGAINST SUBMISSION, and it lives on
       the ADAPTER, not in the CSF firmware context. The first version borrowed
       fw->SubmitLock, which silently degraded to "no lock at all" whenever
       Adapter->FwCtx was NULL - before firmware boot and after FW shutdown -
       even though AS scheduling needs only GpuRegsMapped and both submit paths
       call EnsureAs before any CSF call. KMUTEX, so nesting is safe. */
    KMUTEX              AsSchedLock;
    ULONG               AsSlotInUseMask;     /* bit i set => AS i programmed */
    ULONG               AsSlotPresentMask;   /* mirror of GPU_AS_PRESENT */
#define WINMALI_AS_SLOT_MAX  16u
    struct {
        HANDLE  hContext;        /* dxgk-owned, opaque to us */
        UINT64  RootPtPa;        /* last programmed root-PT phys addr */
        BOOLEAN Bound;
        /* Back-pointer to the VM leasing this slot, or NULL for the MCU/
           bringup/dxgk-context slots. Eviction keys off this: a slot with a
           NULL VmOwner is never a candidate, which is what keeps AS0 and AS1
           out of the victim pool without a special case. */
        struct _WINMALI_VM_PT* VmOwner;
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

/* MT6897 display-overlay flip. Defined in WinMaliDriver.c, used from the
   display-only present path in WinMaliVidPn.c.
   ⛔ g_WinMaliOvlRegsVa is mapped ONCE at StartDevice (MmMapIoSpaceEx is
   PASSIVE_LEVEL) and must never be re-mapped from the present path. */
extern ULONG  g_WinMaliQaiWddmVer;       /* 0 = compiled default; else reported verbatim */
extern ULONG  g_WinMaliOvlFlip;          /* 0=off 1=no-op writeback 2=inert */
extern PVOID  g_WinMaliOvlRegsVa;        /* OVL0 register block, or NULL */
/* Parameters\OvlFlipRender - drive the dual-pipe OVL hardware flip from the PRESENT
   WORKER so RENDER mode gets real page flips instead of a CPU blit. Requires
   OvlFlip=2. Default 0 = today's behaviour exactly. */
extern ULONG  g_WinMaliOvlFlipRender;
BOOLEAN WinMaliOvlFlipDual_(VOID);
extern LONG64 g_WinMaliOvlFrontPa;
extern PVOID  g_WinMaliOvlBackVa;        /* not wired yet - see apply141 note */
extern LONG64 g_WinMaliOvlBackPa;
extern LONG   g_WinMaliStatOvlFlipN;
extern LONG   g_WinMaliStatOvlFlipRej;
extern LONG   g_WinMaliStatOvlFlipBack;
extern PVOID  g_WinMaliOvl4RegsVa;       /* right-half overlay registers */
extern LONG   g_WinMaliStatOvlFlip2N;
extern LONG   g_WinMaliStatOvlFlip2Rej;
extern LONG   g_WinMaliStatOvlFlipRight;

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
extern LONG  g_WinMaliStatStripBo;
extern LONG  g_WinMaliStatStripRows;
extern LONG  g_WinMaliStatStripBlits;
extern LONG  g_WinMaliStatStripNB;
extern LONG  g_WinMaliStatFbAllocs;
extern LONG  g_WinMaliStatFbAllocLastW;
extern LONG  g_WinMaliStatFbAllocLastH;
extern LONG  g_WinMaliStatFbAllocLastPitch;
extern LONG  g_WinMaliStatMpoCapsCalls;
extern LONG  g_WinMaliStatMpoCapsSrc;
extern LONG  g_WinMaliStatMpoCheckCalls;
extern LONG  g_WinMaliStatMpoCheckPlanes;
extern LONG  g_WinMaliStatMpoCheckPostComp;
extern LONG  g_WinMaliStatMpoCheckSrc;
extern LONG  g_WinMaliStatMpoSetCalls;
extern LONG  g_WinMaliStatMpoSetPlanes;
extern LONG  g_WinMaliStatPresentMpoFlag;
extern LONG  g_WinMaliStatPresentRedirected;
extern LONG  g_WinMaliStatMpoPostCompCaps;
extern LONG  g_WinMaliStatMpoPostPresent;
extern LONG  g_WinMaliStatMpoPostLayer;
extern ULONG g_WinMaliShaderPwrOn;
extern LONG  g_WinMaliStatShaderPwrOn;
extern LONG  g_WinMaliStatPresAl0Hi;
extern LONG  g_WinMaliStatPresAl0Lo;
extern LONG  g_WinMaliStatPresAl1Lo;
extern LONG  g_WinMaliStatPresAl0NonNull;
extern LONG  g_WinMaliStatPresAl1NonNull;
extern LONG  g_WinMaliStatVmCreates;
extern LONG  g_WinMaliStatVmDestroys;
extern LONG  g_WinMaliStatVmLive;
extern LONG  g_WinMaliStatVmCreateFail;
extern LONG  g_WinMaliStatDevCreates;
extern LONG  g_WinMaliStatDevDestroys;
extern LONG  g_WinMaliStatDevDestroyBadHandle;
extern LONG  g_WinMaliStatVmOwnerNull;
extern LONG  g_WinMaliStatVmDeferredBind;
extern LONG  g_WinMaliStatAsEvictions;
extern LONG  g_WinMaliStatAsLateBinds;
extern LONG  g_WinMaliStatAsEnsureFail;
extern LONG  g_WinMaliStatVpTrail;

/* Timestamped, fill-once companion to the nibble trail. Defined in
   WinMaliVidPn.c; see the long comment there for the code legend and for why it
   is fill-once rather than a ring. */
/* 512 entries x 8 B = 4 KB. The window that matters is the ~1.8 s between
   StartDevice and the OS switching our source off, and DDIs like IsSupportedVidPn
   fire fast enough that 128 slots would fill before the interesting events
   happen. Fill-once still, so the boot sequence survives an hour of idling. */
/* MEASURED 2026-08-23: VpLogN = 1298 against a cap of 512. The log went blind at
   t=65.3 s - AFTER the boot window, so the 0-3 s conclusions survive, but it also
   revealed that THE OS NEVER STOPS RETRYING: bursts of modality enumeration at
   7.8 s, 8.6 s and 65.3 s, none of which converge. 2048 x 8 B = 16 KB, which the
   fill-once publisher already handles unchanged. */
#define WINMALI_VPLOG_MAX 2048u
typedef struct _WINMALI_VPLOG_ENT {
    ULONG Code;
    ULONG Ms;
} WINMALI_VPLOG_ENT;
extern WINMALI_VPLOG_ENT g_WinMaliVpLog[WINMALI_VPLOG_MAX];
extern LONG              g_WinMaliVpLogN;

/* Codes 0-15 go through WinMaliVpTrail_ and ALSO land in the 4-bit nibble trail.
   Codes >= 16 must use WinMaliVpLogOnly_: they would alias in the nibble trail
   (Code & 0xF) and corrupt the one instrument that has been reliable all along. */
VOID WinMaliVpLogOnly_(_In_ ULONG Code);
#define WINMALI_VPC_SRCADDR      16u
#define WINMALI_VPC_ISSUPPORTED  18u
#define WINMALI_VPC_COFUNC       19u
#define WINMALI_VPC_HWCAPS       20u
#define WINMALI_VPC_RECOMMEND    21u
#define WINMALI_VPC_VSYNC_ON     22u
#define WINMALI_VPC_VSYNC_OFF    23u
#define WINMALI_VPC_POWER        24u
#define WINMALI_VPC_VSYNC_FIRE   25u
/* ⛔★★★★★ COMMIT/COFUNC/VISIBILITY DETAIL - THE INSTRUMENT WAS BLIND.
 *
 * WinMaliKmdCommitVidPn logs ONLY on its two success paths, so
 * StatVpCommit="real 1 / empty 0" means "succeeded once", NOT "called once".
 * Five failure returns log nothing and count nothing - a commit that FAILED at
 * 1.65s is exactly as invisible as a commit that never happened, and a failed
 * commit aborts the whole OS mode-set transaction, leaving the source off
 * forever. That is the symptom we are chasing, and we could not see it.
 *
 * Same hole in EnumVidPnCofuncModality: "all 48 succeeded" rests on trail 0xC
 * never appearing, but 0xC is only reachable via the Done: label - several
 * early returns bypass it entirely.
 *
 * These are >= 0x2600 so they can never alias the 4-bit nibble trail, and the
 * low byte carries a SITE ID so the log names the exact failing line rather
 * than merely "it failed". With one reboot per experiment that is worth it. */
#define WINMALI_VPC_COMMIT_ENTER 0x2600u  /* | AffectedVidPnSourceId & 0xFF */
#define WINMALI_VPC_COMMIT_FAIL  0x2700u  /* | site id 1..4                 */
#define WINMALI_VPC_COFUNC_FAIL  0x2900u  /* | site id 1..5                 */
#define WINMALI_VPC_VIS_ID       0x2A00u  /* | VidPnSourceId & 0xFF         */
/* ⛔★★★★★ WHAT MODE IS dxgk ACTUALLY COMMITTING?
 *
 * IsSupportedVidPn validates the TOPOLOGY SHAPE ONLY - exactly one path,
 * source0 -> target0 - and never looks at the pinned MODES. CommitVidPn then
 * succeeds unconditionally, because with a fixed already-lit framebuffer there
 * is no hardware to program. Net effect: we answer "supported" and "committed"
 * for a VidPn at ANY resolution or refresh rate.
 *
 * If dxgk pins a mode that is not the panel's, we agree to it, and the OS then
 * finds the display unusable - which is exactly the observed end state
 * (Win32_VideoController reports a live 2944x1840@60 mode while
 * Win32_DesktopMonitor reports availability 8 = OFF LINE).
 *
 * These carry the geometry in the code word itself so it lands in the VpLog
 * timeline with a timestamp, costing no registry names and no registry I/O
 * inside a mode-set DDI. 2944 = 0xB80 and 1840 = 0x730 both fit in 12 bits. */
#define WINMALI_VPC_MODE_SRC     0x30000000u  /* | (cx << 12) | cy */
#define WINMALI_VPC_MODE_TGT     0x31000000u  /* | (cx << 12) | cy */
#define WINMALI_VPC_MODE_HZ      0x32000000u  /* | vsync Hz        */
#define WINMALI_VPC_MODE_NOPIN   0x33000000u  /* | 1=src 2=tgt (nothing pinned) */
/* ⛔★★★★★ THE COLD BOOT BRINGS THE SOURCE UP **TWICE**; A REBIND ONCE.
 *
 * MEASURED A/B, same driver, same log:
 *   rebind    : Commit, SetVidPnSourceAddress, Visibility TRUE   -> STAYS UP
 *   cold boot : Commit, SrcAddr, VisTRUE, then a SECOND SrcAddr + VisTRUE 30 ms
 *               later, then both torn down 1.46 s after that.
 * The second activation is the one that dies, and we cannot currently tell what
 * it is: SetVidPnSourceAddress logs no payload at all. These carry it.
 *
 * SRCADDR2: low 24 bits of the PrimaryAddress being latched. Two DIFFERENT
 * addresses would mean two different surfaces are being scanned out - i.e. the
 * second activation targets something else entirely.
 * SRCADDR_ID: VidPnSourceId | (Flags.Value << 8). The flags say whether dxgk
 * asked for ModeChange / FlipImmediate / FlipOnNextVSync, which distinguishes a
 * mode-set latch from a real flip. */
#define WINMALI_VPC_SRCADDR2     0x35000000u  /* | PrimaryAddress & 0xFFFFFF */
#define WINMALI_VPC_SRCADDR_ID   0x36000000u  /* | srcId | (Flags << 8)      */
#define WINMALI_VPC_COMMIT_VIDPN 0x37000000u  /* | low 24 bits of hFunctionalVidPn */
#define WINMALI_VPC_ISSUP_VIDPN  0x38000000u  /* | low 24 bits of hDesiredVidPn    */
/* ⛔★★★★★ WHAT IS IN THE **NEW** VidPns THAT WE CANNOT SATISFY?
 *
 * MEASURED: ~1.8 s after StartDevice the OS builds a brand-new set of VidPn
 * objects (handles jump from the 0xD2xxxx boot range to 0xC3xxxx), probes them,
 * commits NONE, and hides our source 0.35 s later. We answer IsSupportedVidPn
 * YES to all 49 and EnumVidPnCofuncModality succeeds every time - so from our
 * side nothing is refused, yet nothing is applied.
 *
 * EnumVidPnCofuncModality deliberately TOUCHES NOTHING when a mode is already
 * PINNED. If the OS pins something in the new VidPn that differs from the single
 * mode we publish, we would leave it alone, report success, and the topology
 * would never be committable - invisibly. These codes make the pinned state and
 * the pivot visible for every call. */
#define WINMALI_VPC_COFUNC_VIDPN 0x39000000u  /* | low 24 bits of hConstrainingVidPn */
#define WINMALI_VPC_COFUNC_PIVOT 0x3A000000u  /* | EnumPivotType | (srcId<<8) | (tgtId<<16) */
#define WINMALI_VPC_COFUNC_PINSRC 0x3B000000u /* | (cx<<12) | cy   (0xFFFFFF = none pinned) */
#define WINMALI_VPC_COFUNC_PINTGT 0x3C000000u
/* WARNING: EVERY MODE AND TIMING LENS IS NOW ELIMINATED BY MEASUREMENT.
 *
 * The monitor source mode set Windows derived from our EDID matches the target
 * mode we publish FIELD FOR FIELD, as fractions: 2944+56 = HTotal 3000,
 * 1840+10 = VTotal 1850, PixelClockRate 333,000,000, VSync 60/1,
 * HSync 111000/1, progressive. There is exactly ONE display adapter, our path
 * is present in every VidPn the OS builds (zero COFUNC_FAIL|4), and the pinned
 * modes it hands back are our own 2944x1840. And still nothing is committed.
 *
 * So the incompatibility is NOT in the mode. What is left unwritten is the rest
 * of the two structs the OS must complete before a path can be driven:
 *
 *  - D3DKMDT_VIDPN_TARGET_MODE's wire-format union. We write 2 bits of 32
 *    (Preference) and leave Rgb/YCbCr444/YCbCr422/YCbCr420/Intensity all ZERO,
 *    i.e. "this target can carry no wire format at all". The OS is required to
 *    select exactly one bit across those five fields.
 *  - The path's ContentTransformation SUPPORT masks. We advertise Identity
 *    only, while the persisted CCD configuration for this very monitor set
 *    (GraphicsDrivers\Configuration\LNV56550_01_07EA_B2) asks for Scaling=4,
 *    DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX.
 *
 * Both are "we declared less than the OS needs", and both are invisible in
 * every counter because nothing FAILS -- the path simply can never be
 * completed. TGTWIRE_IN is the free falsifier for the first: if dxgk
 * pre-populates the union in pfnCreateNewModeInfo it reads non-zero and that
 * theory dies on the same boot, with no rebuild. */
#define WINMALI_VPC_ISSUP_EMPTY   0x28000000u  /* IsSupportedVidPn saw numPaths==0  */
#define WINMALI_VPC_RECTOPO       0x2B000000u  /* RecommendVidPnTopology was called */
#define WINMALI_VPC_COFUNC_XFORM  0x3D000000u  /* | Scaling | (Rotation<<8) | (updOk<<16) */
#define WINMALI_VPC_TGTWIRE_IN    0x3E000000u  /* wire union AS HANDED TO US (low 24) */ /* | (cx<<12) | cy   (0xFFFFFF = none pinned) */
/* ⛔★★★★★ WHICH CAME FIRST: dwm DROPPING ITS PANEL RTs, OR THE OS HIDING US?
 *
 * The whole causal story rests on this and it has never been measured. The UMD
 * log proves dwm creates two 2944x1840 render targets and destroys both a few
 * calls after the taskbar strip is opened - but that log carries no timestamps,
 * only line order, so it cannot be lined up against
 * SetVidPnSourceVisibility(FALSE) at 1.68 s.
 *
 * Two readings fit every measurement taken so far and they demand opposite fixes:
 *   destroy BEFORE 1.68 s -> dwm gave up first, and the OS hid an unused source
 *   destroy AFTER  1.68 s -> the OS hid us first, and dwm moved off a dead adapter
 *
 * DxgkDdiDestroyAllocation runs in the KMD, so putting it on the VpLog timeline
 * settles it in ONE boot. Only FRAMEBUFFER-SIZED allocations are logged - the
 * panel is 2944*1840*4 = 21,667,840 B and dwm churns hundreds of small surfaces,
 * which would fill a 512-entry fill-once log before anything interesting. */
#define WINMALI_VPC_FBALLOC_KILL 0x34000000u  /* | size in 64KB units          */
/* ⛔★★★★★ THE ONE HALF OF THE SCALING TRANSACTION NEVER MEASURED.
 *
 * PROVEN on 1.1.10.1: a VidPn that arrives with Scaling = IDENTITY commits; one
 * that arrives UNPINNED is driven through pivot=SCALING / pivot=ROTATION and is
 * then ABANDONED, and 0.35 s later dxgk blanks the source it had already blanked
 * ahead of that mode set - and never un-blanks, because the transaction never
 * completes.
 *
 * We call pfnUpdatePathSupportInfo and log its RETURN STATUS, and we log the
 * Scaling the OS put on the path. We have NEVER logged the SUPPORT MASK the path
 * carries on entry - which, on call N+1, is exactly what call N wrote. So
 * "the OS ignores our support mask" and "our write never landed" have been
 * indistinguishable all along.
 *
 * Header note at d3dkmddi.h:7148 is explicit: pfnUpdatePathSupportInfo "updates
 * what the path SUPPORTS for transformations" - the OS owns the pinning. If the
 * mask reads back as we wrote it and the OS still refuses to pin, then the
 * support mask is exonerated and the fault is in what the OS needs BEFORE it will
 * pin at all.
 *   payload: ScalingSupport bits | RotationSupport bits << 8 | (weWrote << 16) */
#define WINMALI_VPC_COFUNC_SUPPORT 0x2C000000u

/* RecommendMonitorModes census. 0xFFFFFF = no interface, 0xFFFE = GetNumModes
   failed; otherwise low 16 = mode count, bits 16-23 = VideoPresentTargetId. */
#define WINMALI_VPC_RECMON       0x3F000000u
/* | (cx<<12) | cy ; bit23 set = a mode from the WALK, clear = the PREFERRED one */
#define WINMALI_VPC_RECMON_MODE  0x40000000u
/* bit23 set -> | (Origin<<8) | Preference   (a walked mode)
   bit22 set -> | AddMode status & 0xFFFF
   neither   -> 0 = NO_PREFERRED_MODE, 1 = has one, 2 = query failed */
#define WINMALI_VPC_RECMON_INFO  0x41000000u
/* Hot re-plug. | (phase << 16) | (status & 0xFFFF); phase 1=disconnect 2=connect */
#define WINMALI_VPC_REPLUG       0x42000000u

/* What the OS is actually PROPOSING in IsSupportedVidPn. The 39 candidate
   VidPns are the whole remaining mystery: we approve them all on topology and
   never look at the modes. 0x46..0x48 verified unused in the tree. */
#define WINMALI_VPC_ISSUP_SRC    0x46000000u  /* | (cx << 12) | cy, 0xFFFFFF = nothing pinned */
#define WINMALI_VPC_ISSUP_TGT    0x47000000u  /* | (cx << 12) | cy, 0xFFFFFF = nothing pinned */
#define WINMALI_VPC_ISSUP_XFORM  0x48000000u  /* | Scaling | (Rotation << 8)                  */
VOID WinMaliVpTrail_(_In_ ULONG Code);
extern LONG  g_WinMaliStatVpIsSupYes;
extern LONG  g_WinMaliStatVpIsSupNo;
extern LONG  g_WinMaliStatVpCommitPath;
extern LONG  g_WinMaliStatVpCommitEmpty;
extern LONG  g_WinMaliStatVpVisOn;
extern LONG  g_WinMaliStatVpVisOff;
extern LONG  g_WinMaliStatVpVisOnMs;
extern LONG  g_WinMaliStatVpVisOffMs;
extern ULONG64 g_WinMaliAdapterStartTick;
ULONG WinMaliMsSinceAdapterStart_(VOID);
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
extern ULONG g_WinMaliSrcModeFormat;
extern LONG  g_WinMaliQaiFlipQueueDepth;
/* ⛔★★★★★ DOES dxgk EVER ASK US FOR A SHARED PRIMARY SURFACE?
 *
 * In the blt model the DESTINATION of a desktop present is the shared primary.
 * WinMaliKmdGetStandardAllocationDriverData handles
 * D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE correctly - but nothing has
 * ever COUNTED it, only a WINMALI_TRACE into a ring that wraps long before
 * anyone reads it. So "dxgk never asked for a primary" and "it asked and we
 * answered" are currently indistinguishable, and if it never asks then
 * NumDstAllocations=0 and a NULL dst cookie are mechanically forced and every
 * theory about cookie binding is chasing a surface that was never created.
 * One counter per type, so the answer is a registry read forever after. */
extern LONG  g_WinMaliStatStdAllocPrimary;
extern LONG  g_WinMaliStatStdAllocShadow;
extern LONG  g_WinMaliStatStdAllocStaging;
extern LONG  g_WinMaliStatStdAllocGdi;
extern LONG  g_WinMaliStatStdAllocOther;
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
extern ULONG g_WinMaliQaiMpoCaps;
extern ULONG g_WinMaliQaiMpoWire;
extern ULONG g_WinMaliQaiMpoOff;
extern ULONG g_WinMaliQaiMpoPlanesOff;
extern ULONG g_WinMaliQaiCapsSizeof;
extern ULONG g_WinMaliQaiSegFlags;   /* DXGK_SEGMENTFLAGS we sent; DirectFlip=0x400 */


extern LONG  g_WinMaliScanoutWc;      /* 1=WC(Ex) 2=WC(legacy) 3=NOCACHE 4=default */
extern LONG  g_WinMaliScanoutWcOff;   /* Parameters\ScanoutWcOff: skip probe+WC entirely */
extern LONG  g_WinMaliScanoutAttr;    /* MmGetCacheAttribute of the winning mapping */
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
extern ULONG g_WinMaliTgtWireFmt;   /* target-mode wire format / MinimumVSyncFreq */
extern ULONG g_WinMaliPathXform;    /* breadth of the path transformation support */
extern ULONG g_WinMaliMonModeAdd;   /* add a driver-origin monitor source mode */
extern ULONG g_WinMaliUvPush;       /* push the finished frame from BO destroy */
extern ULONG g_WinMaliDodMode;      /* 1 = initialise as a display-only miniport */
extern ULONG g_WinMaliDodStage;     /* how far the display-only start got */
extern VOID  WinMaliDodStage(_In_ ULONG Stage);

/* ⛔ StatDodStage is a LADDER and can only name the furthest DDI reached. dxgk
   does not call the start DDIs in one fixed order, so the ladder hides most of
   what happened. This bitmask names EVERY display-only DDI that ran. */
#define WINMALI_DOD_DDI_CHILDSTATUS    0x00000001u
#define WINMALI_DOD_DDI_DEVICEDESC     0x00000002u
#define WINMALI_DOD_DDI_CONTAINERID    0x00000004u
#define WINMALI_DOD_DDI_ISSUPPORTED    0x00000008u
#define WINMALI_DOD_DDI_RECOMMENDVIDPN 0x00000010u
#define WINMALI_DOD_DDI_COFUNCMODALITY 0x00000020u
#define WINMALI_DOD_DDI_COMMITVIDPN    0x00000040u
#define WINMALI_DOD_DDI_UPDATEPATH     0x00000080u
#define WINMALI_DOD_DDI_SRCVISIBILITY  0x00000100u
#define WINMALI_DOD_DDI_MONITORMODES   0x00000200u
#define WINMALI_DOD_DDI_HWCAPABILITY   0x00000400u
#define WINMALI_DOD_DDI_PRESENTDISPONLY 0x00000800u
#define WINMALI_DOD_DDI_SETPOWERSTATE  0x00001000u
/* ★ THE MOST INFORMATIVE BIT: dxgk calls StopDevice when it GIVES UP on the
   adapter. Set = we returned cleanly and were rejected anyway; clear = we never
   came back out of a DDI. ProblemCode=43 cannot tell those two apart. */
#define WINMALI_DOD_DDI_STOPDEVICE     0x00002000u
/* â˜…â˜… THE ONE THAT WAS MISSING. An adapter that holds POST display ownership
   (StatPostOwned=1 - that is us) is torn down through THIS, not StopDevice. */
#define WINMALI_DOD_DDI_STOPANDRELEASE 0x00004000u
#define WINMALI_DOD_DDI_REMOVEDEVICE   0x00008000u
#define WINMALI_DOD_DDI_NOTIFYACPI     0x00010000u
#define WINMALI_DOD_DDI_RESETDEVICE    0x00020000u
#define WINMALI_DOD_DDI_SYSDISPENABLE  0x00040000u
#define WINMALI_DOD_DDI_QUERYINTERFACE 0x00080000u
/* Why did PresentDisplayOnly decline a frame? 1=no adapter 2=no args
   3=pSource NULL 4=no framebuffer 5=unsupported format/pitch. */
#define WINMALI_DODPRES_NOADAPTER  1u
#define WINMALI_DODPRES_NOARGS     2u
#define WINMALI_DODPRES_NOSOURCE   3u
#define WINMALI_DODPRES_NOFB       4u
#define WINMALI_DODPRES_BADFMT     5u
extern VOID  WinMaliDodPresentWhy(_In_ ULONG Code);
extern VOID  WinMaliDodPresentProof(_In_ ULONG Count, _In_ ULONG SrcPix, _In_ ULONG DstPix);
extern VOID  WinMaliDodPresentContent(_In_ ULONG GridN, _In_ ULONG SrcNZ, _In_ ULONG SrcOr,
                                      _In_ ULONG DstNZ, _In_ ULONG DstOr);
extern VOID  WinMaliDodPresentRect(_In_ ULONG RectN, _In_ ULONG SrcNZ, _In_ ULONG DstNZ,
                                   _In_ ULONG FullUs);
/* Parameters\DodFirstFull: 1 = copy the WHOLE frame once before honouring dirty
   rects. Windows only reports what CHANGED and assumes the rest of our scanout
   already matches its surface - it does not, it holds BasicDisplay's pixels. */
extern ULONG g_WinMaliDodFirstFull;
extern ULONG g_WinMaliDodDdiMask;
extern VOID  WinMaliDodDdi(_In_ ULONG Bit);
extern LONG  g_WinMaliStatDodPresent;
extern LONG  g_WinMaliStatDodFull;
extern LONG  g_WinMaliStatDodDirty;
extern LONG  g_WinMaliStatDodNoFb;
extern LONG  g_WinMaliStatDodBadFmt;
extern LONG  g_WinMaliStatDodExc;
extern LONG  g_WinMaliUvPushBusy;
extern LONG64 g_WinMaliUvPushTick;
extern LONG  g_WinMaliStatUvPush;
extern LONG  g_WinMaliStatUvPushRefG;
extern LONG  g_WinMaliStatUvPushRefP;
extern LONG  g_WinMaliStatUvPushRefD;
extern LONG  g_WinMaliStatUvPushExc;
extern LONG  g_WinMaliStatUvPushNB;
extern ULONG g_WinMaliHotReplugMs;  /* ms after StartDevice to fake a monitor re-plug */
extern ULONG g_WinMaliHpdMode;      /* 1 = force HpdAwarenessInterruptible */
/* WHO called SetVidPnSourceVisibility. MEASURED from ETW: the ON at 0.206 s comes
   from pid 872 and the OFF at 1.647 s from pid 1248 - two DIFFERENT processes, and
   1248 has exited by the time WinRM is reachable, so it cannot be named from user
   mode. The DDI runs at PASSIVE_LEVEL in the CALLER's context, so the driver can
   name it directly. 16 bytes: PsGetProcessImageFileName returns 15 chars + NUL. */
extern LONG  g_WinMaliVisOnPid;
extern LONG  g_WinMaliVisOffPid;
extern UCHAR g_WinMaliVisOnName[16];
extern UCHAR g_WinMaliVisOffName[16];
extern ULONG g_WinMaliVpLogVsync;   /* log vsync arm/disarm into VpLog (ring hog)  */
extern ULONG g_WinMaliKnobFuse;     /* clear experimental levers after ONE boot    */
extern ULONG g_WinMaliWorkerVisGate;/* worker blits only while the source is shown */

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
extern ULONG g_WinMaliFlipSrcPreferAperture;
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

NTSTATUS WinMaliCsfAcquireSchedLock(_Inout_ PWINMALI_ADAPTER Adapter);
VOID     WinMaliCsfReleaseSchedLock(_Inout_ PWINMALI_ADAPTER Adapter);
ULONG    WinMaliCsfCurrentAs(_In_ PWINMALI_ADAPTER Adapter);
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







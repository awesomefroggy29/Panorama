
#include "WinMaliKmd.h"
#include "WinMaliDxgkInitFill.h"
#include "WinMaliVersion.h"

static PWINMALI_ADAPTER g_WinMaliAdapter = NULL;

#define WM_TRACE_ENTER(name)         DbgPrint("[WinMali] >> " name "\n")
#define WM_TRACE_EXIT(name)          DbgPrint("[WinMali] << " name "\n")
#define WM_TRACE_EXIT_S(name, st)    DbgPrint("[WinMali] << " name " 0x%08x\n", (unsigned)(st))
#define WM_TRACE_EXIT_B(name, b)     DbgPrint("[WinMali] << " name " %s\n", (b) ? "TRUE" : "FALSE")

PWINMALI_ADAPTER
WinMaliAdapterFromContext(_In_opt_ const VOID* Context)
{
    PWINMALI_ADAPTER adapter = (PWINMALI_ADAPTER)Context;
    if (adapter == NULL || adapter->Magic != WINMALI_ADAPTER_MAGIC) {
        return NULL;
    }
    return adapter;
}

PWINMALI_ADAPTER
WinMaliAdapterFromDxgkHandle(_In_opt_ const VOID* hAdapter)
{
    PWINMALI_ADAPTER adapter = g_WinMaliAdapter;
    if (adapter == NULL || hAdapter == NULL) {
        return NULL;
    }
    if (adapter->DxgkHandle == (HANDLE)hAdapter) {
        return adapter;
    }
    if ((PVOID)adapter == (PVOID)hAdapter) {
        return adapter;
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Entry points                                                            */
/* ---------------------------------------------------------------------- */

/* Bring-up policy toggle, see WinMaliKmd.h. Default 1 = DWM allowed
   (re-enabled for on-screen/present bring-up; set the registry
   Services\WinMaliKmd\Parameters\AllowDwm=0 to block dwm again). */
ULONG g_WinMaliAllowDwm = 1;
/* MT6897 staged bring-up: 0=identity only, 1=+MMU/IRQ/VIDMM, 2=+CSF firmware.
   Default 2 = upstream behaviour. See the comment in StartDevice.
   The initialiser (and the WinMaliReadParamsDword default below) used to be 0,
   which contradicted this comment: a stock install with no registry value got
   NO MMU, NO VIDMM segment and NO CSF firmware boot, so every GroupSubmit
   returned STATUS_DEVICE_NOT_READY no matter how correct the UMD was. The
   staged escape hatch is unaffected -- set Parameters\BringupStage explicitly
   to 0 or 1 to get the reduced modes back. */
ULONG g_WinMaliBringupStage = 2;
/* DXGK_DRIVERCAPS.MiscCaps.ComputeOnly -- see WinMaliKmd.h and the long comment
   at the assignment in WinMaliQai.c. Default 1 = the MT6897 AddAdapter fix. */
ULONG g_WinMaliComputeOnly = 1;

/* Set the moment teardown starts so escape waits abandon rather than hold the
   PnP lock. Cleared on a fresh StartDevice so a stop/start cycle works. */
volatile LONG g_WinMaliStopping = 0;

/* Parameters\PresentTestPattern: paint colour bars + a moving white band into
   the live scanout INSTEAD of copying the primary. Diagnostic only - it answers
   "can we drive this panel at all", which scanoutBlits alone cannot, because
   copying an empty primary 2733 times produces identical counters to success.
   See the long note in WinMaliPresentWorker_. */
ULONG g_WinMaliPresentTestPattern = 0;

/* Parameters\FlipSrcFromVidMm (DEFAULT 0 - LEAVE IT 0).
   When 1, the present worker may read the flip allocation through VidMm's
   aperture MDL. That HANGS THE MACHINE: touching a non-resident page faults,
   VidMm queues a paging op to satisfy it, that op is serviced by this driver,
   and our worker is blocked inside the fault. Deadlock, no bugcheck.
   Proven by an armed boot with PresentTestPattern=1 (never reads srcVa): stable,
   dwm on our UMD, LiveFlipLatch 1->2, StatPresentCalls 0->23, GROUP_SUBMIT
   failures 0. Exists only so the finding can be re-tested on purpose. */
ULONG g_WinMaliFlipSrcFromVidMm = 0;

/* Parameters\FlipSrcFromAperture (DEFAULT 0 - LEAVE IT 0).
   The sibling of the knob above, and it existed unguarded for far too long. The
   fallback branch it now gates maps `src->ApertureMdl` - a dxgk allocation's
   aperture MDL - which is the SAME class of memory as the VidMm read above, and
   fails the same way: fault -> VidMm paging op -> serviced by this driver ->
   our worker is the blocked thread -> wedge with no bugcheck.
   It was mislabelled in the source as "srcPath = 2, 0x93 probe-locked BO",
   which is why it read as safe. It is not a 0x93 read; 0x93 is srcPath 3. */
ULONG g_WinMaliFlipSrcFromAperture = 0;
/* ★ Opt-in: read the ACTUAL primary (its aperture MDL) in preference to the 0x93
   candidate scan. Default 0 = today's behaviour exactly. See the long note at the
   use site. Settable by an offline hive edit, so a bad result costs a reboot. */
ULONG g_WinMaliFlipSrcPreferAperture = 0;

/* ============================================================================
 * THE ONE BOUND ON THE PRESENT SOURCE - `srcReadable`.
 *
 * 1.1.9.79 wedged the box because the aperture branch adopted a source and
 * then EVERY reader walked it with PANEL constants (WINMALI_FB_HEIGHT /
 * WINMALI_FB_STRIDE) or with `bytes`, which is the DESTINATION size. The copy
 * clamp was `max(srcBytes, bytes)` - a MAX, so a source SMALLER than the panel
 * could never lower the row count. That is the 1.1.9.69 defect exactly: a loop
 * bounded on the destination and never on the real mapped extent.
 *
 * From now on there is ONE number: srcReadable, the bytes readable FROM srcVa,
 * i.e. AFTER any `off` already folded into it. It is written at every site that
 * assigns srcVa and nowhere else. From it the worker derives, ONCE:
 *      srcRowPitch  - the SOURCE's row stride
 *      srcRowBytes  - bytes valid within each row (min of source and panel)
 *      srcMaxRows   - rows fully readable, also capped by the panel height
 * and the invariant every reader must obey is exactly:
 *      for r < srcMaxRows, [srcVa + r*srcRowPitch, + srcRowBytes) is mapped.
 *
 * NO READER MAY COMPUTE A SOURCE ADDRESS BY HAND. Go through WinMaliSrcRow_():
 * it returns NULL for any row outside the bound, so a reader that forgets to
 * check faults on a NULL page - an instant, attributable bugcheck at a known
 * line - instead of reading an unmapped kernel VA, which on this box is an
 * unrecoverable hang with no dump. If you add a reader of srcVa and it does not
 * call WinMaliSrcRow_(), it is wrong; grep this file for `srcVa +` and there
 * must be no hits outside this helper.
 * ========================================================================== */
static FORCEINLINE PUCHAR
WinMaliSrcRow_(_In_opt_ PUCHAR SrcVa,
               _In_ ULONG MaxRows,
               _In_ ULONG RowPitch,
               _In_ ULONG Row)
{
    if (SrcVa == NULL || RowPitch == 0 || Row >= MaxRows) {
        return NULL;               /* FAIL CLOSED - never a partial answer */
    }
    return SrcVa + (SIZE_T)Row * RowPitch;
}

/* Parameters\PresentCopyBudgetKB - bytes copied per worker wake, in KB.
   0/absent = the WHOLE frame in one wake (default). 2048 restores the old 2 MB
   throttle. The throttle existed for a hang that was later root-caused to the
   BoFree/system-VA race; its remaining effect is an ~11-wake window in which a
   short-lived DWM surface can be destroyed underneath us. */
ULONG g_WinMaliPresentCopyBudgetKB = 0;

/* Ticks where NO 0x93 candidate qualified and, with the aperture fallback gated
   off, we skipped the frame instead of reading memory we do not own. Expected to
   be LARGE (404 of 501 candidates were under-size last run) - that is the knob
   working, not a fault. */
LONG  g_WinMaliStatSkipNoSrc = 0;

/* Parameters\PresentMarker: stamp a colour-cycling square on every frame WE
   blit, so a desktop on the panel can be told apart from a stale framebuffer. */
ULONG g_WinMaliPresentMarker = 0;

/* Parameters\TraceSubmit: per-submit ring tracing. Default OFF - see the note
   at SubmitCommandVirtual; at ~60 lines/s it wraps the ring in under a minute
   and scrolls away StartDevice/POST/VidPn, the very lines we debug with. */
ULONG g_WinMaliTraceSubmit = 0;

/* Parameters\PresentTimerHz: 0 disarms the periodic present tick entirely.
   Default 30. Exists so the one periodic thing we added can be VARIED rather
   than guessed about - StatTeardownStage=99 proved the armed-window bugcheck
   is NOT in StopDevice, so the next step is isolation, not another theory. */
ULONG g_WinMaliPresentTimerHz = 30;

/* Parameters\AllowHandleDataCb: 1 = still call DxgkCbGetHandleData, 0 = skip it.
   DEFAULT 0.
   ★ WHY OFF BY DEFAULT - two independent measurements say it is pure liability:
     1. IT HAS NEVER ONCE WORKED. This file's own OpenAllocation comments record
        "resolves 0 of 195 opens" and "resolves ZERO, ALWAYS, MEASURED over 542
        opens"; 100% of successful binds come from the AllocId table instead
        (live counters: openCalls=1378 openResolved=1378 openFailed=0).
     2. dxgk COMPLAINS ABOUT IT, on the thread that then fails the present. ETW
        Microsoft-Windows-DxgKrnl id=494 emits "WDDM2 driver calls WDDM1.x DDI!"
        ~2x per rejected LogonUI present, same thread - and OpenAllocation calls
        DxgkCbGetHandleData exactly TWICE per allocation (DeviceSpecific=0 then
        the =1 fallback). We declare WDDMVersion=DXGKDDI_WDDMv3_2 and then use a
        1.x callback on the present path.
   So this removes a call that costs a WDDM-version violation and returns nothing.
   Knob kept so the old behaviour is one registry value away. */
ULONG g_WinMaliAllowHandleDataCb = 0;

/* Parameters\SegmentShape: how we describe our ONE memory segment to dxgk.
     0 = legacy: Aperture only                      (every build up to KMD_1236)
     1 = Aperture + DirectFlip + PopulatedFromSystemMemory        <-- DEFAULT
     2 = real MEMORY segment: Aperture=0, DirectFlip,
         PopulatedFromSystemMemory, CpuTranslatedAddress = DmaSegmentPhys
   ★ WHY. DXGK_SEGMENTFLAGS has a PER-SEGMENT DirectFlip bit (0x400,
   "supports direct flip"). We have always sent Flags.Value=0 plus
   Aperture|CacheCoherent|CpuVisible|ApplicationTarget|NonLocalBudgetGroup, so
   DirectFlip=0: we tell dxgk that NOTHING allocated in our only segment may be
   a flip target. dxgk then refuses LogonUI's RedirectedFlip with
   STATUS_INVALID_PARAMETER - which is the CORRECT response to that declaration.
   Measured: 9/9 rejected for us, while the stock driver accepts the identical
   present shape 200/200 on the same machine.
   Shape 2 is the honest description for a UMA part - the segment IS contiguous
   system memory and we already know its physical base (DmaSegmentPhys, used for
   the root page table) - but it changes how dxgk manages the segment, so it is
   NOT the default: shape 1 is the minimal delta that tests the DirectFlip
   hypothesis without disturbing the render path that currently works. */
ULONG g_WinMaliSegmentShape = 1;

/* Parameters\FailOnGpuFault: make a draw that FAULTED return a failed submit
   instead of reporting success.
   Default 0 (report + count only). A faulting draw used to be completely
   invisible - the ISR acked the IRQ, only the first fault per boot was logged,
   and the submit still returned STATUS_SUCCESS, so the UMD believed it had
   rendered and simply retried forever. Counting and reporting every fault is
   the fix; FAILING the submit on top of that is a real behaviour change that
   could crash-loop the compositor, so it stays opt-in until measured. */
ULONG g_WinMaliFailOnGpuFault = 0;

/* ParametersTilerOomCtxSplice: on TILER_OOM, ALSO splice the grown chunk into the
   heap context free list. panthor and kbase do NOT touch the context on grow - the
   chunk is handed to the FW only via CS_TILER_HEAP_START/END - so the same chunk
   would exist in two places. Default 0 = match the reference. Set 1 to restore the
   1.1.9.18 behaviour without a rebuild. */
ULONG g_WinMaliTilerOomCtxSplice = 0;

/* ParametersShaderPwrOn (DEFAULT 0 - OFF). 1 = write SHADER_PWRON explicitly
   when the cores are not READY before a submit. See WinMaliFwShaderPowerOn_:
   the MCU owns core power on CSF, so this races it and is a recovery tool, not
   standing behaviour. */
ULONG g_WinMaliShaderPwrOn = 0;
LONG  g_WinMaliStatShaderPwrOn = 0;

/* ParametersPresentSettleMs: how long the present worker sleeps before it starts
   scanning for a candidate. Default = WINMALI_PRESENT_SETTLE_MS (25000).

   ⛔★★★★★ THIS DELAY IS A PRIME SUSPECT FOR THE FROZEN DESKTOP.
   USER OBSERVATION 2026-08-21: BEFORE this delay existed, the desktop was LIVE -
   the test-mode watermark visibly moved up when the taskbar appeared - with only
   the taskbar itself invisible. AFTER it, the panel is ONE FROZEN FRAME.
   Mechanism: dwm creates its two 2944x1840 render targets, composites into them,
   and FREES them the moment the taskbar strip opens - which REVOKES them from the
   scanout candidate table (WinMaliBo.c:117). Sleeping 25 s means the worker first
   looks LONG AFTER dwm's live targets are gone, so it can only ever latch the one
   stale OPENED surface that survives. That is exactly the measured state:
   NCand=1, StatSrcChg=0, nonBlack=646/646 (fully painted but never changing).
   Knob so the window can be tuned without a rebuild - each rebuild costs a deploy
   and a human power-cycling the tablet. */
ULONG g_WinMaliPresentSettleMs = WINMALI_PRESENT_SETTLE_MS;

/* Parameters\PresentTickMs (DEFAULT 33 - a ~30 Hz SELF-TICK). 0 = the old
   behaviour, an INFINITE wait on PresentWake.

   ⛔★★★★★ THE WORKER MUST NOT DEPEND ON BEING WOKEN.
   The wait below used a NULL timeout, so the worker only ever ran when
   WinMaliVsyncDpc signalled PresentWake - and that signal is gated on
   FlipLatchCount != 0, i.e. on dxgk having LATCHED A FLIP. This OS presents in
   the BLT model (StatPresentLastFlags bit0=1, StatPresentFlips=0): there is no
   flip chain, so in the steady state nothing ever wakes the worker again. It
   parks forever on the first wait, and because the worker is ALSO the process
   that publishes every Stat and Live counter, all of them freeze at their last
   written value. MEASURED 2026-08-21: TraceTailBytes, StatUvSrcAsked (120) and
   LiveVsyncNotify (89) all frozen across 20 s, StatSweeps=0, the PresentMarker
   square frozen on the panel - one parked thread, presenting as a dozen
   separate "dead counter" mysteries.

   ⇒ In the 0x93-candidate scanout model the worker is a POLLER, not a flip
   responder. A timeout makes it self-tick, so it scans candidates and keeps
   publishing whether or not anything flips. PresentWake still short-circuits
   the wait, so a real flip is serviced immediately rather than up to one tick
   late. Set 0 to restore the old wait and re-test this deliberately. */
ULONG g_WinMaliPresentTickMs = 33;

/* Monotonic worker heartbeat + the last stage it reached, published from the
   worker's own periodic block. StatWorkerTicks CLIMBING is the proof the thread
   is alive; StatWorkerStage says where it got to on the last tick, which is the
   one thing a frozen counter could never distinguish (parked vs exited vs
   looping through an early-out). */
LONG  g_WinMaliStatWorkerTicks = 0;
LONG  g_WinMaliStatWorkerStage = 0;

/* Parameters\QaiFlipCaps: OVERRIDE DXGK_DRIVERCAPS.FlipCaps. 0 = compiled default
   (FlipOnVSyncMmIo|FlipImmediateMmIo = 0xA).

   Measured via ETW id=184: every LogonUI/dwm flip is FlipInterval=0 (IMMEDIATE)
   with Flags=0x8000 (RedirectedFlip) and returns 0xC000000D
   STATUS_INVALID_PARAMETER - refused by dxgk BEFORE reaching our DDI, 9 in 4 s,
   while SetVidPnSourceAddress ran twice in 287 s.
   DXGK_FLIPCAPS bit4 FlipIndependent = "Support MMIO flip to REDIRECTED SURFACES
   bypassing DWM Present" - exactly the shape being refused - and we never claim it.
   bit5 DdiPresentForIFlip = "Call DxgkDdiPresent when independent flip Present
   might be issued".
       0x1A = 0xA | FlipIndependent
       0x3A = 0xA | FlipIndependent | DdiPresentForIFlip

   SAFE FIELD: FlipCaps is already written on the live device and reads back
   (QueryFlipCaps=10). This is NOT the SupportDirectFlip/MaxQueuedFlipOnVSync
   write, which broke DriverEntry with PROBLEM 37 - do not retry that casually.
   HONESTY: our "flip" is a worker memcpy with no vsync sync. If claiming this
   makes dxgk issue flips we mishandle, set back to 0 - one reboot, no rebuild. */
ULONG g_WinMaliQaiFlipCaps = 0;
ULONG g_WinMaliUvSrcInvalidate = 1;

/* Record how far the display-only start got. No-op unless DodMode is set, so the
   render path pays nothing. Flushed: the whole point is to survive a failed
   adapter start and a possible PnP retry. */
VOID
WinMaliDodStage(_In_ ULONG Stage)
{
    if (g_WinMaliDodMode == 0) { return; }
    g_WinMaliDodStage = Stage;
    if (g_WinMaliRegPath.Length != 0) {
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodStage",
                                             0x8000u | (Stage & 0xFFu));
    }
    DbgPrint("[WinMali] ===== DOD STAGE %lu =====\n", (unsigned long)Stage);
}

/* Which display-only DDIs has dxgk actually called this boot? See the bit names
   in WinMaliKmd.h. Written FLUSHED, and only when the bit is NEW, so a DDI that
   is called hundreds of times costs exactly one registry write. */
ULONG g_WinMaliDodDdiMask = 0;

/* ⛔ THE FIRST reason PresentDisplayOnly declined a frame, flushed so it
   survives the adapter being torn down straight afterwards. FIRST only: this
   can be called at 60 Hz and a flushed registry write per frame would be a
   self-inflicted performance bug. 0x8000 sentinel so ABSENT can never read as
   code 0. */
VOID
WinMaliDodPresentWhy(_In_ ULONG Code)
{
    static LONG s_latched = 0;

    if (g_WinMaliDodMode == 0) { return; }
    if (InterlockedCompareExchange(&s_latched, 1, 0) != 0) { return; }
    if (g_WinMaliRegPath.Length != 0) {
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                             L"StatDodPresentWhy",
                                             0x8000u | (Code & 0xFFu));
    }
    DbgPrint("[WinMali] ===== DOD PRESENT declined, reason %lu =====\n",
             (unsigned long)Code);
}

/* Publish the present state, flushed, so it survives the adapter teardown that
   has followed every present so far. SrcPix/DstPix are the SAME pixel sampled
   before and after the copy: equal and non-zero proves the composed desktop
   physically reached the firmware framebuffer. */
/* How much of the frame is actually painted? Publishes the BEST frame seen so
   far - an early black frame must never hide a later good one. */
/* How long ONE full-frame copy actually costs, and how much of the DIRTY RECT
   is painted. The rect is where the content is - the whole-frame grid cannot
   see a 76x79 box. */
VOID
WinMaliDodPresentRect(_In_ ULONG RectN, _In_ ULONG SrcNZ, _In_ ULONG DstNZ,
                      _In_ ULONG FullUs)
{
    if (g_WinMaliDodMode == 0 || g_WinMaliRegPath.Length == 0) { return; }
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodRectN",     RectN);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodRectSrcNZ", SrcNZ);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodRectDstNZ", DstNZ);
    if (FullUs != 0) {
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodFullUs", FullUs);
    }
    /* ⛔ THE FLIP COUNTERS MUST BE PUBLISHED FROM HERE, NOT FROM THE WORKER.
       In display-only mode the present worker never runs, and the worker is what
       publishes every other counter - which is precisely why the trace ring reads
       empty in DOD. This callback is already bounded (1st and every 128th
       present), so three more flushed writes are affordable here and nowhere
       else. StatOvlFlipBack is the READBACK of the register AFTER the write:
       equal to what we wrote proves the write actually reached hardware. */
    if (g_WinMaliOvlFlip != 0) {
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlFlipN",
                                             (ULONG)g_WinMaliStatOvlFlipN);
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlFlipRej",
                                             (ULONG)g_WinMaliStatOvlFlipRej);
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlFlipBack",
                                             (ULONG)g_WinMaliStatOvlFlipBack);
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlFlip2N",
                                             (ULONG)g_WinMaliStatOvlFlip2N);
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlFlip2Rej",
                                             (ULONG)g_WinMaliStatOvlFlip2Rej);
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlFlipRight",
                                             (ULONG)g_WinMaliStatOvlFlipRight);
    }
    DbgPrint("[WinMali] ===== DOD RECT  src %lu/%lu  dst %lu/%lu   fullCopy=%lu us =====\n",
             (unsigned long)SrcNZ, (unsigned long)RectN,
             (unsigned long)DstNZ, (unsigned long)RectN, (unsigned long)FullUs);
}

VOID
WinMaliDodPresentContent(_In_ ULONG GridN, _In_ ULONG SrcNZ, _In_ ULONG SrcOr,
                         _In_ ULONG DstNZ, _In_ ULONG DstOr)
{
    static ULONG s_bestNZ = 0;

    if (g_WinMaliDodMode == 0 || g_WinMaliRegPath.Length == 0) { return; }
    if (SrcNZ < s_bestNZ) { return; }        /* keep the richest frame */
    s_bestNZ = SrcNZ;

    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodGridN",  GridN);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodSrcNZ",  SrcNZ);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodSrcOr",  SrcOr);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodDstNZ",  DstNZ);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodDstOr",  DstOr);
    DbgPrint("[WinMali] ===== DOD CONTENT  src %lu/%lu or=0x%08lX   "
             "dst %lu/%lu or=0x%08lX =====\n",
             (unsigned long)SrcNZ, (unsigned long)GridN, (unsigned long)SrcOr,
             (unsigned long)DstNZ, (unsigned long)GridN, (unsigned long)DstOr);
}

VOID
WinMaliDodPresentProof(_In_ ULONG Count, _In_ ULONG SrcPix, _In_ ULONG DstPix)
{
    if (g_WinMaliDodMode == 0 || g_WinMaliRegPath.Length == 0) { return; }
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodPresentN", Count);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodSrcPix",   SrcPix);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodDstPix",   DstPix);
    DbgPrint("[WinMali] ===== DOD PRESENT #%lu  srcPix=0x%08lX dstPix=0x%08lX %s =====\n",
             (unsigned long)Count, (unsigned long)SrcPix, (unsigned long)DstPix,
             (SrcPix == DstPix) ? "MATCH" : "MISMATCH");
}

VOID
WinMaliDodDdi(_In_ ULONG Bit)
{
    ULONG updated;

    if (g_WinMaliDodMode == 0) { return; }
    if ((g_WinMaliDodDdiMask & Bit) != 0) { return; }   /* already recorded */

    updated = (g_WinMaliDodDdiMask |= Bit);
    if (g_WinMaliRegPath.Length != 0) {
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatDodDdiMask",
                                             0x80000000u | updated);
    }
    DbgPrint("[WinMali] ===== DOD DDI bit 0x%08lX (mask now 0x%08lX) =====\n",
             (unsigned long)Bit, (unsigned long)updated);

    /* â˜…â˜…â˜…â˜…â˜… dxgk only calls StopDevice when it has GIVEN UP on the adapter.
       That is the exact moment the ring holds the whole story, so snapshot it
       into TraceTail now - nothing else in display-only mode ever will. */
    if ((Bit == WINMALI_DOD_DDI_STOPDEVICE ||
         Bit == WINMALI_DOD_DDI_STOPANDRELEASE ||
         Bit == WINMALI_DOD_DDI_REMOVEDEVICE) &&
        KeGetCurrentIrql() == PASSIVE_LEVEL &&
        g_WinMaliRegPath.Length != 0) {
        const ULONG kTraceBytes = 196608u;
        PUCHAR      buf;

        buf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, kTraceBytes, (ULONG)'gLmW');
        if (buf != NULL) {
            ULONG got = WinMaliSerialSnapshot(buf, kTraceBytes);
            if (got != 0) {
                (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"TraceTail",
                                               buf, got);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                              L"TraceTailBytes", got);
                DbgPrint("[WinMali] ===== DOD: published %lu trace bytes at "
                         "StopDevice =====\n", (unsigned long)got);
            }
            ExFreePoolWithTag(buf, (ULONG)'gLmW');
        }
    }
}

/* Parameters\UvPush - push the finished frame from the BO destroy path instead
   of polling for it. COMPILED DEFAULT 1 (ARMED) and burned to 0 by the one-shot
   fuse, because a knob that has to be SET is one that silently never runs on
   this box - that already cost four graded boots. */
ULONG g_WinMaliUvPush = 1;

/* Parameters\DodMode - initialise as a DISPLAY-ONLY miniport
   (DxgkInitializeDisplayOnlyDriver) instead of display+render (DxgkInitialize).
   Default 0 so the shipping behaviour is unchanged unless deliberately armed;
   fuse-burned like every other behavioural knob, so a boot that fails to start
   the adapter comes back disarmed on its own. */
ULONG g_WinMaliDodMode = 0;

/* Parameters\DodFirstFull - see WinMaliKmd.h. Default 1: without it the panel
   keeps whatever the previous owner left outside the dirty rects forever. */
ULONG g_WinMaliDodFirstFull = 1;
/* Written FLUSHED from the start path when DodMode is set. The trace ring is
   unreadable in display-only mode (the worker publishes it and never runs), so
   these are the only account of how far the adapter got. */
ULONG g_WinMaliDodStage = 0;
LONG  g_WinMaliStatDodPresent = 0;   /* successful PresentDisplayOnly calls   */
LONG  g_WinMaliStatDodFull    = 0;   /* full-frame copies                     */
LONG  g_WinMaliStatDodDirty   = 0;   /* dirty-rect copies                     */
LONG  g_WinMaliStatDodNoFb    = 0;   /* ScanoutVa NULL / framebuffer too small */
LONG  g_WinMaliStatDodBadFmt  = 0;   /* bpp != 4 or a non-positive pitch      */
LONG  g_WinMaliStatDodExc     = 0;   /* SEH fired - MUST STAY 0               */
ULONG g_WinMaliUvBoDedup = 1;   /* 0 = always mint a new BO (old behaviour) */
ULONG g_WinMaliQaiDirectFlip = 0;    /* 1 = claim DXGK_DRIVERCAPS.SupportDirectFlip */
ULONG g_WinMaliQaiMaxQueuedFlip = 0; /* N = claim MaxQueuedFlipOnVSync = N */
/* ⛔★★★★★ Parameters\SrcModeFormat: the PIXEL FORMAT of the one source mode we
 * publish. 0 = D3DDDIFMT_X8R8G8B8 (today), 1 = D3DDDIFMT_A8R8G8B8.
 *
 * MEASURED: dxgk's SetDisplayMode reports Format=22 (X8R8G8B8) while our own
 * FlipAllocFmt reads 21 (A8R8G8B8), and every surface dwm composes is
 * DXGI_FORMAT_B8G8R8A8_UNORM (fmt=87 in the UMD log). DirectFlip requires the
 * flip source to match the display mode; a mismatch is refused and falls back.
 *
 * ⚠ This is the LAST candidate on the list and it is NOT a strong one - a BGRX
 * primary with BGRA back buffers is the normal arrangement on real hardware and
 * flips fine there. It is a knob rather than an edit precisely so a negative
 * costs no rebuild, and so it can be put back in one write. */
ULONG g_WinMaliSrcModeFormat = 0;

/* Parameters\TgtWireFmt -- D3DKMDT_VIDPN_TARGET_MODE has three writable regions
 * past Id and we have only ever filled one. bit0 declares Rgb 8 bits per
 * component in the wire-format union; bit1 also writes MinimumVSyncFreq.
 * 0 reproduces today's bytes exactly. */
ULONG g_WinMaliTgtWireFmt = 0;

/* Parameters\PathXform -- 0 = Identity only (today). 1 = also advertise
 * Centered / Stretched / AspectRatioCenteredMax, which is what the persisted
 * CCD configuration for this monitor set actually asks for. Source and target
 * are the SAME size here, so every one of those scalings IS the identity
 * transform; this widens what we ACCEPT, not what we must implement. */
ULONG g_WinMaliPathXform = 0;
/* Parameters\MonModeAdd - add a driver-origin D3DKMDT_MONITOR_SOURCE_MODE from
   RecommendMonitorModes. Default 0: the census in that DDI has to come back
   first, because WMI already shows the set holding exactly our 2944x1840 mode. */
ULONG g_WinMaliMonModeAdd = 0;

/* â˜…â˜…â˜…â˜…â˜… THE KNOWN-GOOD DISPLAY CONFIGURATION - ONE DEFINITION EACH.
 * Used BOTH as the ReadParamsDword default AND as the value the one-shot fuse
 * burns back to. Never write either as a bare literal: a read default and a burn
 * value that drift apart is the defect this block exists to prevent.
 *
 * This is display-only over the MediaTek MT6897 display controller with real
 * dual-pipe OVL page-flips - measured 12032 presents / 12032 flips / 0 rejects,
 * and confirmed by eye. Render mode (DodMode=0) is NOT the safe fallback: its
 * VidPn source is switched off ~1.6 s into every boot and never comes back. */
#define WINMALI_DEF_COMPUTEONLY   0u   /* display DDIs must be wired          */
#define WINMALI_DEF_DODMODE       1u   /* display-only miniport               */
#define WINMALI_DEF_OVLFLIP       2u   /* dual-pipe double-buffered OVL flips */
#define WINMALI_DEF_MONMODEADD    1u
#define WINMALI_DEF_PATHXFORM     5u   /* bit0 scaling modes + bit2 split gate */
#define WINMALI_DEF_UVPUSH        0u
#define WINMALI_DEF_PRESENTHZ     0u   /* DOD is driven by PresentDisplayOnly  */
#define WINMALI_DEF_CHILDCFG      7u   /* VOT_DISPLAYPORT_EMBEDDED             */
#define WINMALI_DEF_VIDPNSOURCES  1u
/* Follow ONE of dwm's flip-chain buffers instead of re-running the contest every
   sweep. The buffers hold DIFFERENT frames, so a per-sweep contest alternates and
   that alternation IS the flashing the user sees. Paired with the freshness window
   below, which releases the incumbent once it stops being written. */
#define WINMALI_DEF_UVSTICKY      1u
#define WINMALI_DEF_UVSTICKYTICKS 4u   /* scans an incumbent may go unchanged */
/*
 * Parameters\HotReplugMs - ms after StartDevice at which to indicate a monitor
 * DISCONNECT and then, 200 ms later, a RECONNECT. 0 = off.
 *
 * WHY: MEASURED - the OS enables the plane at 0.195 s, sets 2944x1840 at 0.206 s,
 * then at 1.647 s turns the source off and the plane off and NEVER TOUCHES THE
 * DISPLAY AGAIN in 313,418 trace events. No error, no failing DDI, and it never
 * builds another apply VidPn. The driver is innocent on every channel we can
 * measure: 155/155 IsSupportedVidPn approvals, zero failure sites, failing
 * modality rounds byte-identical to the healthy one, timings matching the monitor
 * mode set exactly.
 *
 * Indicating a hot re-plug is the documented way to tell the OS "throw the
 * current configuration away and derive a new one" - the driver-side equivalent
 * of unplugging and replugging the panel, which is the standard fix for a stuck
 * display configuration. Doing it AFTER the shell is up means the OS rebuilds at
 * a moment when DWM already exists, unlike the boot-time derivation.
 *
 * ⛔ dxgk only honours a connection change on a child whose HpdAwareness is
 * INTERRUPTIBLE. We ship AlwaysConnected, so HpdMode=1 must be set with it or
 * the indication is legally ignored and the run measures nothing.
 */
ULONG g_WinMaliHotReplugMs = 0;
/* Parameters\HpdMode - 1 = force HpdAwarenessInterruptible on the child,
   independent of which ChildCfg variant is selected. */
ULONG g_WinMaliHpdMode = 0;
/* See the extern block in WinMaliKmd.h. FIRST occurrence only: the LAST value is a
   known trap here - teardown always ends with Visible=FALSE, so a healthy run also
   reports an OFF at StopDevice. */
LONG  g_WinMaliVisOnPid  = 0;
LONG  g_WinMaliVisOffPid = 0;
UCHAR g_WinMaliVisOnName[16]  = {0};
UCHAR g_WinMaliVisOffName[16] = {0};

/* Parameters\VpLogVsync -- vsync arm/disarm consumed ~280 of the 506 VpLog
 * entries this boot, against a 512 cap, so the ring filled at t=43 s and every
 * later event was silently dropped. Off by default; the vsync question is
 * answered and the ring is needed for the mode-set window. */
ULONG g_WinMaliVpLogVsync = 0;

/* Parameters\KnobFuse (default 1) -- see the fuse block in DriverEntry. Set 0
 * only when you want a lever to persist across several boots deliberately. */
ULONG g_WinMaliKnobFuse = 1;

/* Parameters\WorkerVisGate (default 1) -- the present worker must not blit
 * until the OS has made our source VISIBLE. See WinMaliPresentWorker_. */
ULONG g_WinMaliWorkerVisGate = 1;

/* --- DxgkDdiQueryInterface forensics -------------------------------------
 * When ComputeOnly=0, dxgk queries a run of display interfaces and then fails
 * AddAdapter with STATUS_INVALID_PARAMETER. The handler already DbgPrint'd the
 * GUIDs, but DbgPrint needs a kernel debugger, so in practice they were
 * unlogged -- and the handler's own comment says refusing everything is only
 * safe if nothing asked for is mandatory, "which we cannot know while the
 * GUIDs are unlogged".
 *
 * So persist them. The sequence is written to Parameters\QueryInterfaceLog as
 * REG_BINARY (one 20-byte record per query: GUID, Version, Size) and can be
 * decoded from user mode without a debugger. Reset at DriverEntry so the blob
 * always describes the CURRENT driver load.
 */
WCHAR          g_WinMaliRegPathBuf[512];
UNICODE_STRING g_WinMaliRegPath;

#define WINMALI_QI_LOG_MAX 32
typedef struct _WINMALI_QI_ENTRY {
    GUID   Interface;
    USHORT Version;
    USHORT Size;
} WINMALI_QI_ENTRY;
static WINMALI_QI_ENTRY g_WinMaliQiLog[WINMALI_QI_LOG_MAX];
static ULONG            g_WinMaliQiCount;

/* Answer GUID_WDDM_INTERFACE_FEATURE instead of refusing it. Knob so the
   change can be A/B'd against the exact same binary; Parameters\FeatureIface. */
ULONG g_WinMaliFeatureIface = 1;

/* Number of VidPN sources/children StartDevice reports. DEFAULT 0 = render-only,
 * which is what we actually are.
 *
 * WHY THE KNOB: with ComputeOnly=0 dxgk asks the exact same 8 QueryAdapterInfo
 * types as the working path, gets the same answers, and then gives up WITHOUT
 * asking anything further (measured -- the two boot-log traces are identical
 * through QAI[47] and diverge only after it). So the rejection is made from data
 * dxgk already holds, and the only differing input is DRIVERCAPS.MiscCaps.
 * ComputeOnly. The other thing it already holds is our source count: 0. A
 * non-MCDM adapter reporting no VidPN sources is a display adapter with no
 * display, which is a plausible thing for it to reject out of hand.
 *
 * ⚠ ONE-SHOT BY DESIGN: DriverEntry writes this value back to 0 immediately
 * after reading it. If reporting a source makes dxgk call into the VidPN DDIs --
 * which are ALL NULL (WinMaliDxgkStubsWire.h:39-112) -- the result could be a
 * bugcheck at device start, and that loops before the driver can be uninstalled.
 * Disarming on read means the reboot after any such crash comes up safe.
 */
ULONG g_WinMaliVidPnSources = 0;

/* DXGK_DRIVERCAPS hybrid role: 0 = neither, 1 = HybridDiscrete,
   2 = HybridIntegrated. See the long comment at the assignment in WinMaliQai.c.
   Default 0 preserves the behaviour every earlier build had. */
ULONG g_WinMaliHybridRole = 0;

/* Which MT6897 OVL blocks the read-only display probe may touch (bit0=OVL0 ...
   bit3=OVL3, bit4..7 = the pipe-1 OVLs). DEFAULT 1 = OVL0 ONLY: it is the
   overlay scanning the panel, so it is provably powered, while the others may
   be clock-gated - and reading a gated MTK block hangs the bus with no fault. */
ULONG g_WinMaliOvlProbeMask = 1;

/* 0 = never touch a display register (DEFAULT). 1 = write back the address the
   overlay already holds (a no-op flip, proves we can write it safely).
   2 = real double-buffered flip. Fuse-burned like every other lever here, so a
   bad value costs one boot and the next one comes up with it off. */
ULONG g_WinMaliOvlFlip = 0;
/* OVL0 registers, mapped once at StartDevice and kept for the life of the
   adapter. Mapping per flip would be both slow and illegal at raised IRQL. */
PVOID g_WinMaliOvlRegsVa = NULL;
/* Which carveout frame the overlay is currently scanning, and the back buffer
   VA we compose into when OvlFlip=2. */
ULONG  g_WinMaliUvSticky = WINMALI_DEF_UVSTICKY;
ULONG  g_WinMaliUvStickyTicks = WINMALI_DEF_UVSTICKYTICKS;
/* 1:1 framebuffer crop region - diagnostic eyes on the panel at full resolution.
   Defaults target the clock/tray corner of the taskbar. */
ULONG  g_WinMaliCropX = 2208;
ULONG  g_WinMaliCropY = 1696;
ULONG  g_WinMaliCropW = 736;
ULONG  g_WinMaliCropH = 144;
/* Sweeps between crop publishes. Live-tunable so a measurement can raise the sample
   rate to resolve fast events (the taskbar flash) and drop it back afterwards. */
ULONG  g_WinMaliCropEvery = 64;

/* Publish a 1:1 BGR crop of the surface about to reach the panel.
 *
 * Shared by BOTH present paths so display-only and render can be compared with the
 * same code over the same region - the control the render-only version lacked.
 * `Which` is recorded as FrameCropSrc (1=render worker, 2=DOD present) and every
 * publish bumps FrameCropSeq, so a reader can distinguish a fresh crop from one
 * left in the registry by a previous boot.
 *
 * Fail-closed: a short surface truncates the crop; it never reads past the bound. */
VOID
WinMaliPublishCrop_(_In_opt_ PUCHAR SrcVa,
                    _In_ ULONG MaxRows,
                    _In_ ULONG RowPitch,
                    _In_ ULONG RowBytes,
                    _In_ ULONG Which)
{
    static PUCHAR s_crop = NULL;
    static ULONG  s_tick = 0;
    static ULONG  s_seq  = 0;
    const ULONG   CROPMAX = 1572864u;          /* 1.5 MB ceiling */
    ULONG cw, ch, cx, cy, cols, ry, rx;

    if (SrcVa == NULL || RowPitch == 0u || RowBytes == 0u) { return; }
    if (g_WinMaliRegPath.Length == 0) { return; }
    /* â›” THE THROTTLE ALIASES FAST EVENTS. At 1-in-64 (~5.3 s) a taskbar that
     * alternates faster is sampled at arbitrary phase - three captures 30 s apart
     * read as a steady empty state while the panel was visibly flashing. Live so a
     * burst can resolve it and then be turned back down. */
    {
        ULONG every = g_WinMaliCropEvery;
        (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropEvery", every, &every);
        if (every < 4u) { every = 4u; }        /* never per-sweep: registry hammer */
        g_WinMaliCropEvery = every;
        if ((s_tick++ % every) != 0u) { return; }
    }

    /* Refresh the LIVE-TUNABLE SELECTOR KNOBS here, piggybacking this throttle.
     * The selector itself runs per candidate per scan - far too hot for a registry
     * read - but it only needs these to be roughly current. Making them live turns
     * a sticky-on/sticky-off A/B from two 5-8 minute boots into two captures in one
     * boot, which is the difference between running the experiment and guessing. */
    (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"UvSticky",
                                 g_WinMaliUvSticky, &g_WinMaliUvSticky);
    (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"UvStickyTicks",
                                 g_WinMaliUvStickyTicks, &g_WinMaliUvStickyTicks);
    if (g_WinMaliUvStickyTicks == 0u) { g_WinMaliUvStickyTicks = 4u; }
    /* Publish what is ACTUALLY in force, so a capture can be attributed to a
       setting instead of to whatever the arm step intended. */
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"LiveUvSticky",
                                  g_WinMaliUvSticky);
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"LiveUvStickyTicks",
                                  g_WinMaliUvStickyTicks);

    cx = g_WinMaliCropX; cy = g_WinMaliCropY;
    cw = g_WinMaliCropW; ch = g_WinMaliCropH;
    (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropX", cx, &cx);
    (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropY", cy, &cy);
    (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropW", cw, &cw);
    (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropH", ch, &ch);

    cols = RowBytes / 4u;
    if (cx >= cols)    { cx = 0u; }
    if (cy >= MaxRows) { cy = 0u; }
    if (cx + cw > cols)    { cw = cols - cx; }
    if (cy + ch > MaxRows) { ch = MaxRows - cy; }
    while (cw != 0u && ch != 0u && (SIZE_T)cw * ch * 3u > CROPMAX) { ch /= 2u; }
    if (cw == 0u || ch == 0u) { return; }

    if (s_crop == NULL) {
        s_crop = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, CROPMAX, 'cCmW');
    }
    if (s_crop == NULL) { return; }

    for (ry = 0; ry < ch; ++ry) {
        const UCHAR *srow = (const UCHAR *)
            WinMaliSrcRow_(SrcVa, MaxRows, RowPitch, cy + ry);
        if (srow == NULL) { ch = ry; break; }   /* fail closed - truncate */
        for (rx = 0; rx < cw; ++rx) {
            const UCHAR *px = srow + (SIZE_T)(cx + rx) * 4u;
            PUCHAR o = s_crop + ((SIZE_T)ry * cw + rx) * 3u;
            o[0] = px[0]; o[1] = px[1]; o[2] = px[2];
        }
    }
    if (ch == 0u) { return; }

    (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"FrameCrop",
                                   s_crop, (ULONG)((SIZE_T)cw * ch * 3u));
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FrameCropW", cw);
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FrameCropH", ch);
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FrameCropX", cx);
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FrameCropY", cy);
    /* Freshness and provenance - a blob alone cannot say which boot or which path
       produced it, and one stale crop was already misread as a control. */
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FrameCropSrc", Which);
    /* â›” WHICH BUFFER THIS IMAGE CAME FROM. The `bo=` a reader prints beside a
       capture is sampled LIVE while the image comes from the throttled publish, so
       the two do not describe the same instant - that mismatch has produced a wrong
       conclusion twice. Stamp the buffer INTO the publish. */
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FrameCropBo",
                                  (ULONG)InterlockedCompareExchange(
                                      &g_WinMaliStatUvSrcBo, 0, 0));
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FrameCropEvery",
                                  g_WinMaliCropEvery);
    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FrameCropSeq", ++s_seq);
}
ULONG  g_WinMaliUvPreferSettled = 0;
ULONG  g_WinMaliOvlFlipRender = 0;

/* Where the present worker composes. With OvlFlipRender armed it composes into the
   OVL BACK frame and the sweep-completion path flips to it; otherwise it writes
   straight into the scanout the firmware is already displaying, exactly as before.
   â›” Returning ScanoutVa unless the knob is on is what keeps the shipped display-only
   configuration byte-for-byte unchanged. */
static PVOID
WinMaliWorkerDst_(_In_ PWINMALI_ADAPTER a)
{
    if (g_WinMaliOvlFlipRender != 0 && g_WinMaliOvlFlip == 2) {
        /* â›”â˜…â˜…â˜…â˜…â˜… THE VA MUST MATCH THE **CURRENT** BACK PA.
         * g_WinMaliOvlBackVa is a fixed mapping of FRAME2, but the flip SWAPS
         * g_WinMaliOvlBackPa between FRAME1 and FRAME2. Returning the FRAME2 VA
         * unconditionally meant that from the first flip onward we composed into
         * FRAME2 while `back` said FRAME1 - so we flipped to a buffer nobody had
         * written, and the panel alternated between two different-age frames.
         * MEASURED 1.1.17.0: 8 flips, then silence, and a window showing the
         * wallpaper. */
        LONG64 back = InterlockedCompareExchange64(&g_WinMaliOvlBackPa, 0, 0);
        if (back == (LONG64)WINMALI_FB_FRAME2_PA && g_WinMaliOvlBackVa != NULL) {
            return g_WinMaliOvlBackVa;
        }
        /* FRAME1 is the live scanout EDK2 lit, so it is already mapped - no second
           MmMapIoSpaceEx, and no new failure mode. */
        if (back == (LONG64)WINMALI_FB_FRAME1_PA && a != NULL &&
            a->ScanoutVa != NULL) {
            return a->ScanoutVa;
        }
        /* Neither known frame: degrade to the plain blit (which gives a live
           desktop today) rather than composing into a buffer we cannot name.
           Counted on the SAME counter as a refused flip so it can never be a
           silent path. */
        (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlip2Rej);
    }
    return (a != NULL) ? a->ScanoutVa : NULL;
}
LONG64 g_WinMaliOvlFrontPa = 0;
PVOID  g_WinMaliOvlBackVa  = NULL;
LONG64 g_WinMaliOvlBackPa  = 0;
LONG   g_WinMaliStatOvlFlipN   = 0;
LONG   g_WinMaliStatOvlFlipRej = 0;
LONG   g_WinMaliStatOvlFlipBack = 0;
PVOID  g_WinMaliOvl4RegsVa      = NULL;   /* disp1-ovl0, the right half */
LONG   g_WinMaliStatOvlFlip2N   = 0;
LONG   g_WinMaliStatOvlFlip2Rej = 0;
LONG   g_WinMaliStatOvlFlipRight = 0;     /* readback of the RIGHT overlay */
/* 0 = off. 1 = probe DSI0. 3 = probe DSI0 and DSI1 (dual-pipe). Read-only. */
ULONG  g_WinMaliDsiProbe = 0;

/* Override for DXGK_DRIVERCAPS.WDDMVersion. 0 = report the compiled default
   (WDDMv2 in display-only, WDDMv3_2 in render). Any other value is reported
   verbatim. Exists because dxgkrnl branches on >= DXGKDDI_WDDMv2_3 (0x2300):
   below it a lost VidPn source is PAUSED and restored, at/above it the source
   is torn down and never comes back. See apply145.ps1. */
ULONG g_WinMaliQaiWddmVer = 0;

/* Milliseconds after adapter start at which to assert
   DxgkCbIndicateChildStatus(StatusConnection, Connected=TRUE). 0 = never (the
   compiled behaviour). Exists because dxgk zeroes our display plane when an
   internal lookup returns STATUS_GRAPHICS_MONITOR_NOT_CONNECTED, and the byte
   that lookup tests is set by exactly this callback. */
ULONG g_WinMaliIndicateConnMs = 0;

/* Child-descriptor variant selector for the QueryChildRelations matrix. See the
   switch in WinMaliKmdQueryChildRelations. 0 = baseline. */
ULONG g_WinMaliChildCfg = 0;

/*
 * Number of children reported by StartDevice, INDEPENDENT of the VidPN source
 * count. These used to be the same variable, which made the decisive question
 * untestable: dxgk dies at StartAdapter_DpiFdoEnumChildDevicesFailed, and we
 * could not tell whether it hates OUR CHILD or hates THIS DEVICE HAVING CHILDREN.
 *
 * -1 (default) means "follow VidPnSources", preserving the old behaviour so no
 * existing result is invalidated by the split.
 */
ULONG g_WinMaliChildCount = (ULONG)-1;

_Use_decl_annotations_
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA init;
    NTSTATUS                   status;

    /* UART2 serial log mirror FIRST so even this banner reaches the wire
       when running with kernel debugging off (see WinMaliLog.h). */
    (VOID)WinMaliSerialInit();

    /* ⛔★★★★★ SNAPSHOT THE **PREVIOUS BOOT'S** RING BEFORE WE OVERWRITE IT.
     *
     * The ring lives in EDK2-reserved physical memory and SURVIVES a warm reboot, so
     * at this instant it still holds the tail of the boot that just died. Every line
     * this driver is about to emit pushes that history out.
     *
     * This exists because render mode has hard-locked several times (user-confirmed)
     * and the trace was never readable afterwards: `TraceTail` is republished by the
     * present timer ~57 s into the NEW boot while WinRM only arrives ~70 s in, so the
     * evidence is gone before anything can read it. MEASURED exactly that today - a
     * post-hang read returned the new boot's DOD presents, not the lock.
     *
     * Same shape as the existing triggered snapshots: capture NOW, into a SEPARATE
     * value the timer-published TraceTail cannot clobber.
     *
     * ⚠ The tail is only as complete as the registry flush that preceded the lock; a
     * hard lock loses whatever had not committed. Absence of a smoking gun here is
     * NOT evidence of a clean shutdown. */
    {
        /* ⛔ 256 KB = THE WHOLE RING, not a quarter of it.
         * MEASURED with 64 KB: the capture came back holding only THIS boot's
         * FIRMWARE log (SCPBOOT bringup, MtkUfs, LVTS, mtkwifi) and our own banner -
         * EDK2 re-inits the ring and fills it before our driver ever loads, so the
         * most-recent 64 KB is always the firmware, never the session that died.
         * The previous Windows tail, if it survives at all, is further back. Take
         * the lot. */
        const ULONG kPrevBytes = 262144u;
        PUCHAR prev = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, kPrevBytes,
                                                    (ULONG)'pTmW');
        if (prev != NULL) {
            ULONG got = WinMaliSerialSnapshot(prev, kPrevBytes);
            if (got != 0 && RegistryPath != NULL) {
                (VOID)WinMaliWriteParamsBinary(RegistryPath, L"TracePrevBoot",
                                               prev, got);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"TracePrevBootBytes",
                                              got);
            }
            ExFreePoolWithTag(prev, (ULONG)'pTmW');
        }
    }

    DbgPrint("[WinMali] >> DriverEntry abi=%u.%u kmd v" WINMALI_KMD_VERSION_STR "\n",
             WINMALI_ABI_MAJOR, WINMALI_ABI_MINOR);

    /*
     * ---- KILL SWITCH -------------------------------------------------------
     * WinMaliReadKillSwitch() exists and is documented as the "shut the driver
     * off after a crashed boot" lever, but in the source as received it has NO
     * CALLER -- so the lever did nothing. On a machine where a bugcheck in
     * DriverEntry can loop before the driver can be uninstalled, this is the
     * only in-Windows abort we have. Wire it, and wire it FIRST: before
     * DxgkInitialize and before any GPU access.
     *
     * Disable from a WinRM session with:
     *   reg add HKLM\SYSTEM\CurrentControlSet\Services\WinMaliKmd\Parameters ^
     *       /v Disabled /t REG_DWORD /d 1 /f
     *
     * Returning a FAILING status stops the SCM considering the driver started.
     * ErrorControl = 0 in the INF, so boot continues regardless.
     */
    {
        BOOLEAN disabled = FALSE;

        (VOID)WinMaliReadKillSwitch(RegistryPath, &disabled);
        if (disabled) {
            DbgPrint("[WinMali] KILL SWITCH SET (Parameters\\Disabled=1) - not initializing\n");
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }

    (VOID)WinMaliReadParamsDword(RegistryPath, L"AllowDwm", 1, &g_WinMaliAllowDwm);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"BringupStage", 2, &g_WinMaliBringupStage);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"ComputeOnly",
                                 WINMALI_DEF_COMPUTEONLY, &g_WinMaliComputeOnly);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FeatureIface", 1, &g_WinMaliFeatureIface);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"HybridRole", 0, &g_WinMaliHybridRole);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"OvlProbeMask", 1,
                                 &g_WinMaliOvlProbeMask);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"OvlFlip",
                                 WINMALI_DEF_OVLFLIP, &g_WinMaliOvlFlip);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"DsiProbe", 0, &g_WinMaliDsiProbe);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"QaiWddmVer", 0, &g_WinMaliQaiWddmVer);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"IndicateConnMs", 0,
                                 &g_WinMaliIndicateConnMs);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"OvlFlipRender", 0,
                                 &g_WinMaliOvlFlipRender);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"UvPreferSettled", 0,
                                 &g_WinMaliUvPreferSettled);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"UvSticky", WINMALI_DEF_UVSTICKY,
                                 &g_WinMaliUvSticky);
    /* How many scans an incumbent may go unchanged before it is released. 0 uses the
       compiled default of 4. â›” NOT unbounded: `changes` is cumulative, so "has ever
       changed" would pin us to one buffer for ever - which is exactly the bug this
       fixes. */
    (VOID)WinMaliReadParamsDword(RegistryPath, L"UvStickyTicks", 4,
                                 &g_WinMaliUvStickyTicks);
    /* Crop region. Knob-driven so the region can be moved with four registry writes
       instead of a rebuild - at 5-8 minutes a boot, a baked region is unaffordable
       for something whose entire purpose is to be aimed repeatedly. */
    (VOID)WinMaliReadParamsDword(RegistryPath, L"CropX", 2208, &g_WinMaliCropX);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"CropY", 1696, &g_WinMaliCropY);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"CropW",  736, &g_WinMaliCropW);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"CropH",  144, &g_WinMaliCropH);
    if (g_WinMaliUvStickyTicks == 0) { g_WinMaliUvStickyTicks = 4; }
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffUvSticky",
                                  0x8000u | ((g_WinMaliUvSticky & 0xFu) << 8)
                                          | (g_WinMaliUvStickyTicks & 0xFFu));
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffUvSticky",
                                  0x8000u | (g_WinMaliUvSticky & 0xFFu));
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffUvPreferSettled",
                                  0x8000u | (g_WinMaliUvPreferSettled & 0xFFu));
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffOvlFlipRender",
                                  0x8000u | (g_WinMaliOvlFlipRender & 0xFFu));
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffIndicateConn",
                                  0x80000000u | (g_WinMaliIndicateConnMs & 0xFFFFu));
    /* Published with the usual 0x8000 sentinel so "ran as 0" and "never read"
       are distinguishable. The value itself is 16-bit, so publish it whole. */
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffQaiWddmVer",
                                  0x80000000u | (g_WinMaliQaiWddmVer & 0xFFFFu));
    /* Same 0x8000 sentinel rule as every other knob: "ran as 0" and "never read"
       must not be the same observation. */
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffOvlFlip",
                                  0x8000u | (g_WinMaliOvlFlip & 0xFFu));
    (VOID)WinMaliReadParamsDword(RegistryPath, L"AllowHandleDataCb", 0,
                                 &g_WinMaliAllowHandleDataCb);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"SegmentShape", 1,
                                 &g_WinMaliSegmentShape);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"TraceSubmit", 0, &g_WinMaliTraceSubmit);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentTimerHz",
                                 WINMALI_DEF_PRESENTHZ,
                                 &g_WinMaliPresentTimerHz);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FailOnGpuFault", 0,
                                 &g_WinMaliFailOnGpuFault);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"TilerOomCtxSplice", 0,
                                 &g_WinMaliTilerOomCtxSplice);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentSettleMs",
                                 WINMALI_PRESENT_SETTLE_MS,
                                 &g_WinMaliPresentSettleMs);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentTickMs", 33,
                                 &g_WinMaliPresentTickMs);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"ShaderPwrOn", 0,
                                 &g_WinMaliShaderPwrOn);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"QaiMpoCaps", 0,
                                 &g_WinMaliQaiMpoCaps);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"QaiMpoWire", 0,
                                 &g_WinMaliQaiMpoWire);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"QaiFlipCaps", 0,
                                 &g_WinMaliQaiFlipCaps);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"UvSrcInvalidate", 1,
                                 &g_WinMaliUvSrcInvalidate);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"UvPush",
                                 WINMALI_DEF_UVPUSH, &g_WinMaliUvPush);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"DodMode",
                                 WINMALI_DEF_DODMODE, &g_WinMaliDodMode);
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffDodMode",
                                  0x8000u | (g_WinMaliDodMode & 0xFFu));
    /* Confirmation only. The VOID GATE is StatFuseArmed bits 16-23: it already
       has a slot on this key and is written FLUSHED, so it survives the power
       cycle a wedge would force. EffUvPush is a brand-new name on a key far past
       its value cap and may silently fail to appear. */
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffUvPush",
                                  0x8000u | (g_WinMaliUvPush & 0xFFu));
    (VOID)WinMaliReadParamsDword(RegistryPath, L"UvBoDedup", 1,
                                 &g_WinMaliUvBoDedup);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"QaiDirectFlip", 0,
                                 &g_WinMaliQaiDirectFlip);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"QaiMaxQueuedFlip", 0,
                                 &g_WinMaliQaiMaxQueuedFlip);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"SrcModeFormat", 0,
                                 &g_WinMaliSrcModeFormat);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"TgtWireFmt", 0,
                                 &g_WinMaliTgtWireFmt);
    /* THE ONE-SHOT FUSE WAS A ONE-WAY TRAP AND IT COST FOUR BOOTS.
     *
     * MEASURED 2026-08-23: StatFuseArmed=0 on four consecutive boots even though
     * the compiled default here was 5 and the registry value had been DELETED.
     * Cause: the fuse below writes PathXform=0 with WinMaliWriteParamsDwordFlushed
     * - a COMMITTED write - and a committed registry value beats a compiled
     * default forever after. The delete, by contrast, was lazily flushed, and this
     * box loses lazy writes across its unclean power cycles. So the burn is
     * durable and the undo of the burn is not: the knob could never be re-armed
     * from the binary again, and four "PathXform changed nothing" results were
     * really "PathXform never ran".
     *
     * THE RULE THIS ESTABLISHES: on this box a knob name that has EVER been
     * burned is dead. Re-arming from a compiled default needs a name that has
     * never existed. Hence PathXform2.
     *
     * The one-shot safety property is deliberately KEPT: the fuse still burns
     * this value to 0 (flushed) so a boot that hangs comes back with the lever
     * off and reaches a desktop unaided. */
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PathXform2",
                                 WINMALI_DEF_PATHXFORM, &g_WinMaliPathXform);
    /* Published BEFORE the fuse burns it, with a 0x8000 sentinel so that "ran as
       0" and "never read" stop being the same observation. StatFuseArmed=0 is
       ambiguous - it also means the fuse was disabled. This is not. */
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffPathXform",
                                  0x8000u | (g_WinMaliPathXform & 0xFFu));
    (VOID)WinMaliReadParamsDword(RegistryPath, L"MonModeAdd",
                                 WINMALI_DEF_MONMODEADD, &g_WinMaliMonModeAdd);
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffMonModeAdd",
                                  0x8000u | (g_WinMaliMonModeAdd & 0xFFu));
    (VOID)WinMaliReadParamsDword(RegistryPath, L"HotReplugMs", 0,
                                 &g_WinMaliHotReplugMs);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"HpdMode", 0,
                                 &g_WinMaliHpdMode);
    /* Same 0x8000 sentinel rule as EffPathXform: "ran as 0" and "never read" must
       not be the same observation. HotReplugMs is published in units of 100 ms so
       it still fits the low byte. */
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffHotReplug",
                                  0x8000u | ((g_WinMaliHotReplugMs / 100u) & 0xFFu));
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffHpdMode",
                                  0x8000u | (g_WinMaliHpdMode & 0xFFu));
    (VOID)WinMaliReadParamsDword(RegistryPath, L"KnobFuse", 1,
                                 &g_WinMaliKnobFuse);
    /* Marker string so the PACKAGED binary can be proved to contain this change.
       â›” Never verify a deploy by hash - signing changes it. Grep for a string. */
    DbgPrint("[WinMali]    BAKED DEFAULTS v40 KNOWNGOOD: dod=%u ovl=%u xform=%u mon=%u "
             "uv=%u hz=%u cfg=%u src=%u\n",
             (unsigned)WINMALI_DEF_DODMODE, (unsigned)WINMALI_DEF_OVLFLIP,
             (unsigned)WINMALI_DEF_PATHXFORM, (unsigned)WINMALI_DEF_MONMODEADD,
             (unsigned)WINMALI_DEF_UVPUSH, (unsigned)WINMALI_DEF_PRESENTHZ,
             (unsigned)WINMALI_DEF_CHILDCFG, (unsigned)WINMALI_DEF_VIDPNSOURCES);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"WorkerVisGate", 1,
                                 &g_WinMaliWorkerVisGate);

    /* ---- THE ONE-SHOT FUSE ----------------------------------------------
     * An experimental lever now survives exactly ONE boot. We have just read
     * the value, so zero it back out and COMMIT that write before we can hang.
     * If this boot dies, the next one comes up with the lever off and reaches a
     * desktop by itself - no spare-install recovery, no offline hive edit.
     *
     * WARNING: DURABILITY MUST FOLLOW THE DIRECTION OF SAFETY. We flush the
     * DISARM and never the arm. Lazy flushing is an accidental safety net for
     * the arming direction and must not be "fixed"; here we are writing the
     * SAFE value, which is exactly the one that has to survive a freeze. */
    if (g_WinMaliKnobFuse != 0) {
        /* â›” BURN TO THE COMPILED DEFAULT, NEVER TO A BARE 0. Writing 0 here is
           what made the next boot come up in the BROKEN render path, and what
           kept PathXform2 permanently off despite a compiled default of 5. */
        if (g_WinMaliPathXform != WINMALI_DEF_PATHXFORM) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"PathXform2",
                                                 WINMALI_DEF_PATHXFORM);
        }
        if (g_WinMaliMonModeAdd != WINMALI_DEF_MONMODEADD) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"MonModeAdd",
                                                 WINMALI_DEF_MONMODEADD);
        }
        /* A re-plug that wedges the display must not repeat on the next boot. */
        if (g_WinMaliHotReplugMs != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"HotReplugMs", 0);
        }
        if (g_WinMaliHpdMode != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"HpdMode", 0);
        }
        if (g_WinMaliTgtWireFmt != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"TgtWireFmt", 0);
        }
        /* Count the burns so a silently-cleared knob is distinguishable from
           one that was never set - those look identical in the registry. */
        if (g_WinMaliUvPush != WINMALI_DEF_UVPUSH) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"UvPush",
                                                 WINMALI_DEF_UVPUSH);
        }
        /* ⛔ A display-only start that fails leaves the box on BasicDisplay with
           no accelerated adapter. RETRACTED 2026-08-25: this used to burn
           DodMode to 0 so that "the NEXT boot is the known good display+render
           path". RENDER IS NOT THE KNOWN GOOD PATH - its VidPn source is switched
           off ~1.6 s into every boot and never returns, which is the frozen
           desktop the user has been living with. Display-only IS the known good
           path, so the burn now goes to WINMALI_DEF_DODMODE and a boot that dies
           still recovers by itself, to a configuration that reaches a desktop. */
        if (g_WinMaliDodMode != WINMALI_DEF_DODMODE) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"DodMode",
                                                 WINMALI_DEF_DODMODE);
        }
        /* ⛔★★★ OvlFlip MUST BURN TOO. 1.1.14.1 shipped a comment claiming it was
         * "fuse-burned like every other lever" while this block did not touch it,
         * so a committed OvlFlip=1 would have survived every subsequent boot -
         * precisely the stale-knob trap the fuse exists to prevent, and the same
         * shape as the PathXform burn that once cost four boots.
         * ★ A COMMENT ASSERTING A SAFETY PROPERTY IS NOT THAT PROPERTY. */
        if (g_WinMaliOvlFlip != WINMALI_DEF_OVLFLIP) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"OvlFlip",
                                                 WINMALI_DEF_OVLFLIP);
        }
        /* ⛔ A wrong WDDM version can stop the adapter dead. Burn it so a bad
           value costs exactly ONE boot and the next one comes up compiled. */
        if (g_WinMaliQaiWddmVer != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"QaiWddmVer", 0);
        }
        /* Claiming the MPO cap KILLS adapter start unless SupportDirectFlip is also
           claimed (dxgkrnl DXGADAPTER::Initialize gate at 0x1400EA2C8 returns
           STATUS_INVALID_PARAMETER). A dead adapter cannot re-arm anything, so
           without these burns a single bad MPO boot leaves the machine with NO GPU
           driver on every boot afterwards - which is exactly what happened twice on
           2026-08-25. Their compiled default is 0, so burning to 0 IS burning to the
           default. Read at :777/:779/:799, i.e. before this block. */
        if (g_WinMaliQaiMpoCaps != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"QaiMpoCaps", 0);
        }
        if (g_WinMaliQaiMpoWire != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"QaiMpoWire", 0);
        }
        if (g_WinMaliQaiDirectFlip != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"QaiDirectFlip", 0);
        }
        /* Programs the DISPLAY CONTROLLER from the worker thread. A bad boot must
           cost exactly one boot, like every other lever. */
        if (g_WinMaliOvlFlipRender != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"OvlFlipRender", 0);
        }
        if (g_WinMaliUvPreferSettled != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"UvPreferSettled", 0);
        }
        if (g_WinMaliUvSticky != WINMALI_DEF_UVSTICKY) {
            /* â›” burn to the value we WANT, not to a bare 0 - with the live
               re-read in place, burning 0 here switches stickiness off a few
               seconds into every boot and brings the flashing back. */
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"UvSticky",
                                                 WINMALI_DEF_UVSTICKY);
        }
        if (g_WinMaliUvStickyTicks != WINMALI_DEF_UVSTICKYTICKS) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"UvStickyTicks",
                                                 WINMALI_DEF_UVSTICKYTICKS);
        }
        if (g_WinMaliIndicateConnMs != 0) {
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"IndicateConnMs", 0);
        }
        (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"StatFuseArmed",
            (g_WinMaliPathXform & 0xFFu) | ((g_WinMaliTgtWireFmt & 0xFFu) << 8)
          | ((g_WinMaliUvPush & 0xFFu) << 16) | ((g_WinMaliOvlFlip & 0xFFu) << 24));
    }
    (VOID)WinMaliReadParamsDword(RegistryPath, L"VpLogVsync", 0,
                                 &g_WinMaliVpLogVsync);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentMarker", 0, &g_WinMaliPresentMarker);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentTestPattern", 0,
                                 &g_WinMaliPresentTestPattern);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FlipSrcFromVidMm", 0,
                                 &g_WinMaliFlipSrcFromVidMm);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FlipSrcPreferAperture", 0,
                                 &g_WinMaliFlipSrcPreferAperture);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FlipSrcFromAperture", 0,
                                 &g_WinMaliFlipSrcFromAperture);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentCopyBudgetKB", 0,
                                 &g_WinMaliPresentCopyBudgetKB);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"ChildCfg",
                                 WINMALI_DEF_CHILDCFG, &g_WinMaliChildCfg);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"VidPnSources",
                                 WINMALI_DEF_VIDPNSOURCES, &g_WinMaliVidPnSources);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"ChildCount", (ULONG)-1, &g_WinMaliChildCount);
    if (g_WinMaliChildCount == (ULONG)-1) {
        g_WinMaliChildCount = g_WinMaliVidPnSources;
    }
    /*
     * ⛔⛔ THE ONE-SHOT DISARM USED TO LIVE HERE, AND IT CORRUPTED EVERY EXPERIMENT
     * THAT DEPENDED ON THIS KNOB.
     *
     * It wrote the value back to 0 the instant it was read, so a bugcheck could
     * not loop. But `pnputil /remove-device` + `/scan-devices` loads the driver
     * MORE THAN ONCE: the first load consumed the one-shot, and the load dxgk
     * actually started the adapter from then read 0.
     *
     * Proof, from dxgk's own ETW record scoped to our adapter, on a run where
     * VidPnSources=1 was set:
     *     id=25  pDxgAdapter=<ours>  NbVidPnSources=0
     *
     * Every "VidPnSources=1" result was therefore unverified. The risk it guarded
     * against never materialised either -- the failure mode has been a clean
     * CM_PROB_FAILED_POST_START every single time, never a bugcheck -- and the
     * kill switch (Parameters\Disabled=1, checked above) is the real safety net.
     * So the knob is now plain, and its EFFECTIVE value is published below.
     */
    /*
     * ---- ARMED-CONFIG BOOT GUARD --------------------------------------------
     *
     * NOT the one-shot disarm described above. That one keyed on the DRIVER
     * LOAD and was therefore wrong: remove-device + scan-devices loads us more
     * than once inside a single boot, so it disarmed before the load that
     * actually started the adapter. This keys on the BOOT.
     *
     * THE PROBLEM IT SOLVES: run_rendertest.ps1 arms the display path
     * (ComputeOnly=0, ChildCfg=6), tests, then restores render-only. If the
     * armed run BUGCHECKS, the restore never runs, so the next boot re-enters
     * the path that just crashed -> boot loop -> the machine has to be rescued
     * offline from the spare USB install. That has now cost two recovery cycles
     * (0xC0000094 in dxgmms2, and 0x119 in the video scheduler on 2026-08-12).
     *
     * THE RULE: an armed config is only honoured during the boot that armed it.
     *   - same boot   -> honour, and do NOT consume (all reloads agree)
     *   - later boot  -> the armed window did not survive; we crashed. Disarm.
     *
     * Boot identity = system time minus interrupt time (= time of boot). Within
     * one boot two loads differ only by clock slew (ms); across a boot they
     * differ by at least the previous uptime plus the restart (tens of seconds).
     * A 10 s tolerance separates those with a wide margin either way.
     *
     * Set ArmPersistent=1 to opt out once the display path is trustworthy.
     */
    {
        ULONG         persistent = 0, armLo = 0, armHi = 0;
        LARGE_INTEGER sysTime;
        ULONGLONG     bootStamp, armedStamp;
        LONGLONG      delta;

        KeQuerySystemTime(&sysTime);
        bootStamp = (ULONGLONG)sysTime.QuadPart - KeQueryInterruptTime();

        (VOID)WinMaliReadParamsDword(RegistryPath, L"ArmPersistent",  0, &persistent);
        (VOID)WinMaliReadParamsDword(RegistryPath, L"ArmBootStampLo", 0, &armLo);
        (VOID)WinMaliReadParamsDword(RegistryPath, L"ArmBootStampHi", 0, &armHi);
        armedStamp = ((ULONGLONG)armHi << 32) | (ULONGLONG)armLo;

        delta = (LONGLONG)(bootStamp - armedStamp);
        if (delta < 0) {
            delta = -delta;
        }

        if (g_WinMaliComputeOnly != 0 || g_WinMaliDodMode != 0) {
            /* Disarmed. DodMode != 0 counts as disarmed: display-only is the
               COMPILED DEFAULT and the shipping configuration, so a DOD boot is
               not an armed experiment. Without this test the guard reads every
               healthy reboot as a crashed render test and, on the third one,
               commits ComputeOnly=1 and refuses to load for good.
               Clear any stale stamp so the NEXT arm starts clean --
               otherwise an arm in a later boot would inherit an old stamp and
               disarm itself immediately. */
            if (armedStamp != 0) {
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampLo", 0);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampHi", 0);
            }
            /* A boot that reached here is healthy, so the escalation ladder must
               go back to the bottom. ArmDisarmCount had NO reset anywhere in the
               tree: once nonzero it stayed nonzero, and the next armed run would
               skip safe mode and full-disarm on its first stale stamp. */
            {
                ULONG disarmsOk = 0;
                (VOID)WinMaliReadParamsDword(RegistryPath, L"ArmDisarmCount", 0,
                                             &disarmsOk);
                if (disarmsOk != 0) {
                    (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmDisarmCount", 0);
                }
            }
        } else if (persistent != 0) {
            DbgPrint("[WinMali]    ARM: display path armed and ArmPersistent=1 -- boot guard OFF\n");
        } else if (armedStamp == 0) {
            /* First load of a freshly armed config: record which boot owns it. */
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"ArmBootStampLo",
                                          (ULONG)(bootStamp & 0xFFFFFFFFull));
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"ArmBootStampHi",
                                          (ULONG)(bootStamp >> 32));
            DbgPrint("[WinMali]    ARM: display path armed for THIS boot (stamp %I64u)\n",
                     bootStamp);
        } else if (delta > 100000000ll) {          /* 10 s in 100ns units */
            ULONG disarms = 0;
            (VOID)WinMaliReadParamsDword(RegistryPath, L"ArmDisarmCount", 0, &disarms);

            /*
             * ESCALATING FALLBACK. The old behaviour went straight to
             * render-only, which on gpufb firmware is not a safe harbour at all:
             * render-only cannot start (the framebuffer is in our _CRS), so the
             * guard turned "crashed once" into "no GPU, and a dead panel, on
             * every boot from now on" - a stable state only WinRM could escape.
             *
             * Instead, step down one rung at a time:
             *   1st unfinished armed boot -> stay ARMED but kill the periodic
             *      present tick (PresentTimerHz=0). That tick was ISOLATED as
             *      the 0xD1 crash source (30 Hz => crash 3/3, 0 => clean), so
             *      this drops the known-dangerous part while keeping a working
             *      display and a working GPU.
             *   2nd  -> give up and disarm. With the render-only refusal added
             *      below that now means "do not load", which leaves the firmware
             *      display intact and the machine reachable.
             */
            if (disarms == 0) {
                DbgPrint("[WinMali] ** ARM: armed config is from a PREVIOUS BOOT "
                         "(delta %I64d ms) -- the armed run did not finish. "
                         "SAFE MODE: staying armed but forcing PresentTimerHz=0 "
                         "(the isolated crash source). Next unfinished boot will "
                         "disarm completely.\n", (LONGLONG)(delta / 10000));
                g_WinMaliPresentTimerHz = 0;
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"PresentTimerHz", 0);
                /* Re-stamp for THIS boot: the armed config now belongs to us, so
                   a reload inside this same boot honours it instead of stepping
                   down again. */
                (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"ArmBootStampLo",
                                              (ULONG)(bootStamp & 0xFFFFFFFFull));
                (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"ArmBootStampHi",
                                              (ULONG)(bootStamp >> 32));
            } else {
                DbgPrint("[WinMali] ** ARM: armed config is from a PREVIOUS BOOT "
                         "(delta %I64d ms) and safe mode ALREADY FAILED "
                         "(ArmDisarmCount=%lu). Disarming completely.\n",
                         (LONGLONG)(delta / 10000), disarms);
                /* ⛔★★★★★ DISARM MUST LEAVE A **STARTABLE** DRIVER.
                 *
                 * This used to write VidPnSources=0 and ChildCount=0, which makes
                 * the adapter report ZERO video-present sources and zero children.
                 * dxgk then refuses to start it at all - device ProblemCode 37/43,
                 * ProblemStatus 0xC0000182. So every cycle became:
                 *     armed boot works -> stamp goes stale -> self-disarm ->
                 *     VidPnSources=0 -> next boot cannot start -> stuck.
                 * MEASURED 2026-08-20: found the box sitting at ChildCfg=0
                 * ComputeOnly=1 VidPnSources=0 / problem=37, written by THIS code,
                 * after an armed boot that had reported problem=0.
                 *
                 * Disarming the DISPLAY (ComputeOnly=1, ChildCfg=0) is the intent.
                 * Destroying the adapter's source/child enumeration is not, and it
                 * turns a safety mechanism into a brick.
                 * ★ A SAFE-MODE FALLBACK MUST ITSELF BE A CONFIGURATION THAT RUNS. */
                g_WinMaliComputeOnly  = 1;
                g_WinMaliChildCfg     = 0;
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ComputeOnly",  1);
                /* ChildCfg deliberately NOT persisted: it has no fuse burn, so a
                   committed 0 survives a manual ComputeOnly recovery and leaves a
                   child variant that was never validated for display-only armed
                   on every subsequent boot. The in-memory value above is enough. */
                /* VidPnSources / ChildCount deliberately LEFT ALONE - see above. */
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampLo", 0);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampHi", 0);
            }
            /* Publish it: "the test didn't crash" must be a measurement, not an
               assumption. A nonzero ArmDisarmCount means a previous armed run
               died before its restore step. */
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"ArmDisarmCount",
                                                 disarms + 1);
        } else {
            DbgPrint("[WinMali]    ARM: armed config belongs to THIS boot "
                     "(delta %I64d ms) -- honouring it\n", (LONGLONG)(delta / 10000));
        }
    }

    DbgPrint("[WinMali]    VidPnSources=%lu (persistent; kill switch is the safety net)\n",
             g_WinMaliVidPnSources);
    DbgPrint("[WinMali]    ComputeOnly=%lu (0 = advertise a normal render adapter)\n",
             g_WinMaliComputeOnly);

    /* Echo back what we ACTUALLY read, plus a nonce that changes every driver
       load. Without this there is no way to distinguish "the new knob value was
       used and did not help" from "DriverEntry never re-ran" -- restarting the
       device does not necessarily reload the driver. */
    /* Stash RegistryPath so QueryInterface can persist its GUID sequence, and
       clear last load's log so the blob always describes THIS load. */
    g_WinMaliQiCount = 0;
    RtlZeroMemory(g_WinMaliQiLog, sizeof(g_WinMaliQiLog));
    g_WinMaliRegPath.Buffer        = g_WinMaliRegPathBuf;
    g_WinMaliRegPath.Length        = 0;
    g_WinMaliRegPath.MaximumLength = sizeof(g_WinMaliRegPathBuf);
    if (RegistryPath != NULL && RegistryPath->Buffer != NULL &&
        RegistryPath->Length <= sizeof(g_WinMaliRegPathBuf)) {
        RtlCopyMemory(g_WinMaliRegPathBuf, RegistryPath->Buffer, RegistryPath->Length);
        g_WinMaliRegPath.Length = RegistryPath->Length;
    }

    {
        LARGE_INTEGER nonce = KeQueryPerformanceCounter(NULL);

        (VOID)WinMaliWriteParamsDword(RegistryPath, L"QueryInterfaceCount", 0);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"FeatureIfaceAccepted", 0);
        /* ⛔★★★★★ ZERO QueryCapsOutSz HERE, IN DriverEntry.
         * Its only writer was WinMaliPresentWorker_, which cannot run on a boot
         * where StartDevice never completes - so on every failed start it
         * reported the PREVIOUS boot's value. Two comments in this tree read
         * "QueryCapsOutSz==0 => dxgk never queried DRIVERCAPS" off exactly that
         * stale number. Zeroed here and republished from the DDI itself, it
         * finally testifies: nonzero afterwards == queried in THIS boot.
         * EffectiveQaiMpo: bit0 = DDI slots wired, bit1 = cap claimed. Without
         * it a boot cannot prove the knob was even read. */
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"QueryCapsOutSz", 0);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveQaiMpo",
                                      (g_WinMaliQaiMpoWire != 0 ? 1u : 0u) |
                                      (g_WinMaliQaiMpoCaps != 0 ? 2u : 0u));
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveFeatureIface",
                                      g_WinMaliFeatureIface);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveComputeOnly",
                                      g_WinMaliComputeOnly);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveHybridRole", g_WinMaliHybridRole);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveChildCfg", g_WinMaliChildCfg);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveVidPnSources", g_WinMaliVidPnSources);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveChildCount", g_WinMaliChildCount);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveFeatureIface", g_WinMaliFeatureIface);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveAllowDwm", g_WinMaliAllowDwm);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"EffectiveBringupStage",
                                      g_WinMaliBringupStage);
        (VOID)WinMaliWriteParamsDword(RegistryPath, L"BootNonce",
                                      nonce.LowPart);
    }
    DbgPrint("[WinMali]    BringupStage=%lu (0=identity 1=+mmu/vidmm 2=+csf-fw)\n",
             g_WinMaliBringupStage);
    DbgPrint("[WinMali]    AllowDwm=%lu (%s)\n", g_WinMaliAllowDwm,
             g_WinMaliAllowDwm ? "dwm.exe may create devices"
                               : "dwm.exe CreateDevice -> ACCESS_DENIED");

    /*
     * ---- DO NOT START AT ALL IF WE CANNOT BE A DISPLAY ----------------------
     *
     * On gpufb firmware the panel framebuffer is in OUR _CRS
     * (SsdtMali.asl: Memory32Fixed 0xFD619000, 0x014AB000). Two consequences,
     * both measured:
     *
     *   1. A render-only persona is ILLEGAL - dxgk rejects the adapter with
     *      CM_PROB_FAILED_POST_START after StartDevice has already returned.
     *      We cannot prevent that from inside StartDevice: every internal
     *      failure path there deliberately returns STATUS_SUCCESS (see :588-603),
     *      so dxgk always sees a started miniport and only then rejects it.
     *   2. By then the devnode has STARTED and claimed the _CRS framebuffer.
     *      Nothing else can paint the panel afterwards - BasicDisplay never gets
     *      the device, StopDevice/RemoveDevice do no ownership transfer, and
     *      DxgkDdiResetDevice (dxgk's restore-boot-display hook) is an empty
     *      stub here. The panel therefore freezes on the last firmware-drawn
     *      image: the "stuck spinner" with a fully booted, healthy Windows
     *      behind it. We never write a framebuffer pixel in this mode - the
     *      freeze is ownership, not scribbling.
     *
     * So a disarmed adapter delivers ZERO GPU function (it never starts, so no
     * escapes and no D3D) AND costs the user their screen. Refusing to load is
     * strictly better: DriverEntry failing happens BEFORE DxgkInitialize, the
     * devnode never starts, the framebuffer is never claimed, and the firmware
     * display survives. That is the measured-good state (it is exactly what the
     * Parameters\Disabled kill switch produces, and the screen works there).
     *
     * This is deliberately placed AFTER the arm guard and AFTER the echo of the
     * Effective values and BootNonce above, so the evidence trail still records
     * what this load decided and that DriverEntry really ran.
     *
     * Parameters\AllowRenderOnly=1 opts out, for a platform whose framebuffer is
     * NOT in our _CRS (e.g. RK3588) where render-only is a legitimate mode.
     */
    if (g_WinMaliComputeOnly != 0) {
        ULONG allowRenderOnly = 0;

        (VOID)WinMaliReadParamsDword(RegistryPath, L"AllowRenderOnly", 0,
                                     &allowRenderOnly);
        if (allowRenderOnly == 0) {
            DbgPrint("[WinMali] ** REFUSING TO LOAD: ComputeOnly=%lu (render-only) "
                     "cannot start on firmware that puts the framebuffer in our "
                     "_CRS - it would end in CM_PROB_FAILED_POST_START with the "
                     "panel frozen and NO GPU function. Not starting, so the "
                     "boot display survives. Set Parameters\\AllowRenderOnly=1 "
                     "to override.\n", g_WinMaliComputeOnly);
            (VOID)WinMaliWriteParamsDword(RegistryPath, L"StatRefusedRenderOnly", 1);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        DbgPrint("[WinMali]    ComputeOnly=%lu but AllowRenderOnly=1 - proceeding "
                 "(expect CM_PROB_FAILED_POST_START on gpufb firmware)\n",
                 g_WinMaliComputeOnly);
    }
    (VOID)WinMaliWriteParamsDword(RegistryPath, L"StatRefusedRenderOnly", 0);

    /* ==================================================================
     * DISPLAY-ONLY START (Parameters\DodMode=1).
     *
     * ⛔ THIS RETURNS. Everything below is the display+RENDER path and must not
     * also run - DxgkInitialize and DxgkInitializeDisplayOnlyDriver are
     * alternatives, not a sequence.
     *
     * WHY THIS SHAPE: there is no display engine on this part. The panel is
     * scanned by firmware from a fixed framebuffer and pixels move only by CPU
     * copy. Presenting ourselves as display+render makes dwm build a flip chain
     * that can never complete, which is why it releases our VidPn source at
     * 1.647 s and why dxgk never submits a real flip. A display-only miniport is
     * the SUPPORTED Windows shape for firmware-owned scanout - it is what
     * BasicDisplay is - and DxgkDdiPresentDisplayOnly hands us the composed
     * desktop as a CPU pointer, which is exactly what our copy consumes.
     *
     * Every function below already existed and is already debugged; only
     * PresentDisplayOnly is new.
     * ================================================================== */
    if (g_WinMaliDodMode != 0) {
        KMDDOD_INITIALIZATION_DATA dod;
        NTSTATUS dodStatus;

        RtlZeroMemory(&dod, sizeof(dod));
        {
            /* See the block comment: dod.Version is what dxgk reads BEFORE
               StartDevice to decide which interfaces to demand. KMDDOD has no
               version block above WDDM2_0, so sizeof(dod) is unchanged. */
            ULONG initVer = 0;
            (VOID)WinMaliReadParamsDword(RegistryPath, L"DodInitVersion", 0, &initVer);
            if (initVer == 0) { initVer = DXGKDDI_INTERFACE_VERSION_WDDM2_0; }
            dod.Version = initVer;
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"StatDodInitVer", initVer);
            DbgPrint("[WinMali]    DISPLAY-ONLY: dod.Version = 0x%08X "
                     "(WDK default would be 0x%08X)\n",
                     (unsigned)initVer, (unsigned)DXGKDDI_INTERFACE_VERSION);
        }

        dod.DxgkDdiAddDevice                = WinMaliKmdAddDevice;
        dod.DxgkDdiStartDevice              = WinMaliKmdStartDevice;
        dod.DxgkDdiStopDevice               = WinMaliKmdStopDevice;
        dod.DxgkDdiRemoveDevice             = WinMaliKmdRemoveDevice;
        dod.DxgkDdiDispatchIoRequest        = WinMaliKmdDispatchIoRequest;
        dod.DxgkDdiInterruptRoutine         = WinMaliKmdInterruptRoutine;
        dod.DxgkDdiDpcRoutine               = WinMaliKmdDpcRoutine;
        dod.DxgkDdiQueryChildRelations      = WinMaliKmdQueryChildRelations;
        dod.DxgkDdiQueryChildStatus         = WinMaliKmdQueryChildStatus;
        dod.DxgkDdiQueryDeviceDescriptor    = WinMaliKmdQueryDeviceDescriptor;
        dod.DxgkDdiSetPowerState            = WinMaliKmdSetPowerState;
        dod.DxgkDdiNotifyAcpiEvent          = WinMaliKmdNotifyAcpiEvent;
        dod.DxgkDdiResetDevice              = WinMaliKmdResetDevice;
        dod.DxgkDdiUnload                   = WinMaliKmdDxgkUnload;
        dod.DxgkDdiQueryInterface           = WinMaliKmdQueryInterface;
        dod.DxgkDdiControlEtwLogging        = WinMaliKmdControlEtwLogging;
        dod.DxgkDdiQueryAdapterInfo         = WinMaliKmdQueryAdapterInfo;
        dod.DxgkDdiSetPalette               = NULL;      /* 32bpp only, no LUT */
        dod.DxgkDdiSetPointerPosition       = WinMaliKmdSetPointerPosition;
        dod.DxgkDdiSetPointerShape          = WinMaliKmdSetPointerShape;
        dod.DxgkDdiEscape                   = WinMaliKmdEscape;
        dod.DxgkDdiCollectDbgInfo           = WinMaliKmdCollectDbgInfo;
        dod.DxgkDdiIsSupportedVidPn         = WinMaliKmdIsSupportedVidPn;
        dod.DxgkDdiRecommendFunctionalVidPn = WinMaliKmdRecommendFunctionalVidPn;
        dod.DxgkDdiEnumVidPnCofuncModality  = WinMaliKmdEnumVidPnCofuncModality;
        dod.DxgkDdiSetVidPnSourceVisibility = WinMaliKmdSetVidPnSourceVisibility;
        dod.DxgkDdiCommitVidPn              = WinMaliKmdCommitVidPn;
        dod.DxgkDdiUpdateActiveVidPnPresentPath = WinMaliKmdUpdateActiveVidPnPresentPath;
        dod.DxgkDdiRecommendMonitorModes    = WinMaliKmdRecommendMonitorModes;
        dod.DxgkDdiGetScanLine              = WinMaliKmdGetScanLine;
        dod.DxgkDdiQueryVidPnHWCapability   = WinMaliKmdQueryVidPnHWCapability;

        /* THE ONE NEW DDI - the composed desktop, as a CPU pointer. */
        dod.DxgkDdiPresentDisplayOnly       = WinMaliKmdPresentDisplayOnly;

        dod.DxgkDdiStopDeviceAndReleasePostDisplayOwnership =
            WinMaliKmdStopDeviceAndReleasePostDisplayOwnership;
        dod.DxgkDdiSystemDisplayEnable      = WinMaliKmdSystemDisplayEnable;
        dod.DxgkDdiSystemDisplayWrite       = WinMaliKmdSystemDisplayWrite;
        dod.DxgkDdiGetChildContainerId      = WinMaliKmdGetChildContainerId;
        dod.DxgkDdiControlInterrupt         = WinMaliKmdControlInterrupt;

        /* ⛔ TAG THE LOAD. DriverEntry runs MORE THAN ONCE per boot: when the DOD
           start fails, the fuse has already burned DodMode, PnP retries, and the
           retry comes up render and OVERWRITES every value. Without a load
           counter the failing load leaves no trace at all - which is exactly
           what happened on 1.1.12.0. */
        {
            ULONG loadN = 0;
            (VOID)WinMaliReadParamsDword(RegistryPath, L"StatLoadCount", 0, &loadN);
            loadN++;
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"StatLoadCount", loadN);
            (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"StatDodLoadN", loadN);
            DbgPrint("[WinMali] ===== DISPLAY-ONLY START (load #%lu) =====\n",
                     (unsigned long)loadN);
        }
        DbgPrint("[WinMali]    DISPLAY-ONLY: calling "
                 "DxgkInitializeDisplayOnlyDriver (Version=0x%08x sizeof=%lu)\n",
                 (unsigned)dod.Version, (unsigned long)sizeof(dod));
        dodStatus = DxgkInitializeDisplayOnlyDriver(DriverObject, RegistryPath, &dod);
        DbgPrint("[WinMali] ===== DxgkInitializeDisplayOnlyDriver -> 0x%08X =====\n",
                 (unsigned)dodStatus);
        /* ⛔ 0x8000 SENTINEL: a value that was merely CLEARED reads 0 and would
           otherwise be indistinguishable from STATUS_SUCCESS. Never again. */
        (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"StatDodInit",
                                             0x80000000u | ((ULONG)dodStatus & 0xFFFFu));
        if (!NT_SUCCESS(dodStatus)) {
            DbgPrint("[WinMali] << DriverEntry DOD init FAILED 0x%08X\n",
                     (unsigned)dodStatus);
        }
        return dodStatus;
    }

    /* Tag this load too. If the DOD start fails and PnP retries, the retry lands
       HERE with DodMode already burned to 0 - and StatRenderLoadN vs StatDodLoadN
       then says plainly which load did what, instead of leaving me to infer it
       from a trace ring that survives reboots and mixes boots together. */
    {
        ULONG loadN = 0;
        (VOID)WinMaliReadParamsDword(RegistryPath, L"StatLoadCount", 0, &loadN);
        loadN++;
        (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"StatLoadCount", loadN);
        (VOID)WinMaliWriteParamsDwordFlushed(RegistryPath, L"StatRenderLoadN", loadN);
        DbgPrint("[WinMali] ===== RENDER START (load #%lu) =====\n",
                 (unsigned long)loadN);
    }

    WinMaliDxgkPatchInitializationData(&init);

    DbgPrint("[WinMali]    init.Version=0x%08x sizeof(init)=%lu\n",
             (unsigned)init.Version, (unsigned long)sizeof(init));
    DbgPrint("[WinMali]    AddDevice=%p Escape=%p QueryAdapterInfo=%p\n",
             init.DxgkDdiAddDevice, init.DxgkDdiEscape, init.DxgkDdiQueryAdapterInfo);
    DbgPrint("[WinMali]    CreateProcess=%p CreateDevice=%p CreateContext=%p\n",
             init.DxgkDdiCreateProcess, init.DxgkDdiCreateDevice, init.DxgkDdiCreateContext);

    DbgPrint("[WinMali]    calling DxgkInitialize...\n");
    status = DxgkInitialize(DriverObject, RegistryPath, &init);
    DbgPrint("[WinMali]    DxgkInitialize returned 0x%08x\n", status);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[WinMali] << DriverEntry DxgkInitialize=0x%08x\n", status);
        return status;
    }

    /* Do NOT touch DriverObject->DriverUnload here. DxgkInitialize installed
       dxgkrnl's own unload routine, which unlinks this driver from dxgk's
       global driver list and then calls our DxgkDdiUnload below. Overwriting
       it (as pre-v32 code did) skips that unlink, so an uninstall+reinstall
       on the same boot re-entered DpiInitializeEx with a freed entry still
       on the list -> bugcheck 0x139 LIST_ENTRY corruption (seen live
       2026-07-12, dxgkrnl!DpiInitializeEx+0x77c). */
    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrint("[WinMali] << DriverEntry STATUS_SUCCESS\n");
    return STATUS_SUCCESS;
}

/* DXGKDDI_UNLOAD (init->DxgkDdiUnload): dxgkrnl calls this from ITS
   DriverUnload after unhooking us. Last code of ours that runs. */
VOID APIENTRY
WinMaliKmdDxgkUnload(VOID)
{
    WM_TRACE_ENTER("DxgkUnload");
    WM_TRACE_EXIT("DxgkUnload");
    WinMaliSerialTeardown();   /* after the last print */
}

/* ---------------------------------------------------------------------- */
/* Device lifecycle                                                        */
/* ---------------------------------------------------------------------- */

NTSTATUS APIENTRY
WinMaliKmdAddDevice(
    _In_     CONST PDEVICE_OBJECT PhysicalDeviceObject,
    _Outptr_ PVOID*               MiniportDeviceContext)
{
    PWINMALI_ADAPTER adapter;
    WM_TRACE_ENTER("AddDevice");

    if (MiniportDeviceContext == NULL || PhysicalDeviceObject == NULL) {
        WM_TRACE_EXIT_S("AddDevice", STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    adapter = (PWINMALI_ADAPTER)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(*adapter), WINMALI_POOL_TAG);
    if (adapter == NULL) {
        WM_TRACE_EXIT_S("AddDevice", STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(adapter, sizeof(*adapter));
    adapter->Magic                = WINMALI_ADAPTER_MAGIC;
    adapter->PhysicalDeviceObject = PhysicalDeviceObject;
    KeInitializeSpinLock(&adapter->FenceQueueLock);
    KeInitializeSpinLock(&adapter->AllocIdLock);
    InitializeListHead(&adapter->AllocIdList);
    WinMaliBoTableInit(&adapter->BoTable);
    /* ⛔★★★★★ MEASURE THE ASSUMPTION UNDER THREE FAILED THEORIES.
     *
     * `handle=1` - the first syncobj after driver load - is absent from a table
     * holding 39-41 live objects, and is NEVER created or destroyed anywhere in
     * a 2252-line capture. Every explanation so far assumed the per-adapter
     * table is REBUILT during a boot (device rundown, then adapter teardown).
     * Two fixes were built on that and both failed their oracle
     * (SignalFail stayed 79, StatSrcChg stayed 0), and WMSYNCADPT1 never even
     * appeared - which is equally consistent with "the teardown never runs".
     *
     * That assumption has never been measured. Count and log every table
     * init/teardown so the next theory starts from a fact. */
    {
        static volatile LONG s_adapterInstance = 0;
        LONG inst = InterlockedIncrement(&s_adapterInstance);
        WINMALI_WARN("WMADPT1 AdapterStart: instance #%ld - initialising the "
                     "per-adapter syncobj table (handles are boot-unique and do "
                     "NOT restart)", inst);
    }
    WinMaliSyncObjTableInit(&adapter->SyncObjTable);
    WinMaliVmTableInit(&adapter->VmTable);
    WinMaliGroupTableInit(&adapter->GroupTable);

    g_WinMaliAdapter       = adapter;
    *MiniportDeviceContext = adapter;

    DbgPrint("[WinMali]    AddDevice ctx=%p pdo=%p\n", adapter, PhysicalDeviceObject);
    WM_TRACE_EXIT_S("AddDevice", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

/* MT6897 display-overlay register probe - READ ONLY. Provenance of every
   constant is in the apply140.ps1 header comment. */
#define WINMALI_OVL_COUNT      8u
#define WINMALI_OVL_REGS_PER   8u
static const ULONG g_WinMaliOvlBase[WINMALI_OVL_COUNT] = {
    0x14402000u, 0x14403000u, 0x14404000u, 0x14405000u,   /* disp-ovl0..3  */
    0x14602000u, 0x14603000u, 0x14604000u, 0x14605000u    /* disp1-ovl0..3 */
};
/* Offsets quoted from mediatek_v2/mtk_disp_ovl.c - do NOT substitute guesses. */
static const ULONG g_WinMaliOvlOff[WINMALI_OVL_REGS_PER] = {
    0x000u,  /* DISP_REG_OVL_STA          :65  */
    0x008u,  /* DISP_REG_OVL_INTSTA       :83  */
    0x020u,  /* DISP_REG_OVL_ROI_SIZE     :92  */
    0x02Cu,  /* DISP_REG_OVL_SRC_CON      :110 */
    0x038u,  /* DISP_REG_OVL_L0_SRC_SIZE  :294 */
    0x03Cu,  /* DISP_REG_OVL_L0_OFFSET    :293 */
    0xF40u,  /* L0 ADDR     = data->addr + 0x20*0   :263 */
    0xF4Cu   /* L0 ADDR MSB = 0x0f4c     + 0x20*0   :278 */
};

static VOID
WinMaliOvlProbe_(VOID)
{
    ULONG regs[WINMALI_OVL_COUNT * WINMALI_OVL_REGS_PER];
    ULONG i, r;
    ULONG match = 0;

    if (g_WinMaliOvlProbeMask == 0 || g_WinMaliRegPath.Length == 0) {
        return;
    }
    /* ⛔ NOT 0xFFFFFFFF. WinMaliHw.c:81-92 already establishes that an all-ones
       read on this SoC means "the block is still power/clock gated" - so using
       0xFFFFFFFF as the not-probed filler would make a GATED overlay
       indistinguishable from one we never looked at, which is exactly the
       ambiguity this probe exists to remove. */
    for (i = 0; i < WINMALI_OVL_COUNT * WINMALI_OVL_REGS_PER; i++) {
        regs[i] = 0xDEADC0DEu;          /* sentinel = "not probed" */
    }

    for (i = 0; i < WINMALI_OVL_COUNT; i++) {
        PHYSICAL_ADDRESS pa;
        volatile UCHAR  *va;

        if ((g_WinMaliOvlProbeMask & (1u << i)) == 0) {
            continue;
        }
        /* Breadcrumb BEFORE the map, flushed: if mapping or the first read
           hangs the bus this value is the only record of which block did it. */
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlStep",
                                             0x1000u | i);
        pa.QuadPart = (LONGLONG)g_WinMaliOvlBase[i];
        va = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
        if (va == NULL) {
            DbgPrint("[WinMali]    OVL probe: map 0x%08x FAILED\n", g_WinMaliOvlBase[i]);
            continue;
        }
        for (r = 0; r < WINMALI_OVL_REGS_PER; r++) {
            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlStep",
                                                 0x2000u | (i << 4) | r);
            /* READ ONLY - never write. */
            regs[i * WINMALI_OVL_REGS_PER + r] =
                READ_REGISTER_ULONG((volatile ULONG *)(va + g_WinMaliOvlOff[r]));
        }
        MmUnmapIoSpace((PVOID)va, 0x1000);

        DbgPrint("[WinMali]    OVL%u @0x%08x STA=0x%08x INTSTA=0x%08x ROI=0x%08x "
                 "SRC_CON=0x%08x L0SZ=0x%08x L0OFF=0x%08x ADDR=0x%08x MSB=0x%08x\n",
                 i, g_WinMaliOvlBase[i],
                 regs[i*WINMALI_OVL_REGS_PER+0], regs[i*WINMALI_OVL_REGS_PER+1],
                 regs[i*WINMALI_OVL_REGS_PER+2], regs[i*WINMALI_OVL_REGS_PER+3],
                 regs[i*WINMALI_OVL_REGS_PER+4], regs[i*WINMALI_OVL_REGS_PER+5],
                 regs[i*WINMALI_OVL_REGS_PER+6], regs[i*WINMALI_OVL_REGS_PER+7]);

        /* THE ANSWER: the overlay whose layer-0 address is our scanout base is
           the one feeding the panel. Recorded as i+1 so 0 stays "no match". */
        if (regs[i*WINMALI_OVL_REGS_PER+6] == (ULONG)WINMALI_FB_SCANOUT_PA) {
            match = i + 1u;
        }
    }

    /* ⛔ KEEP THE MAPPING IF WE ARE GOING TO FLIP. MmMapIoSpaceEx is a
       PASSIVE_LEVEL operation and the flip path runs from the present, so the
       VA must already exist. Mapped exactly once, never unmapped while the
       adapter lives. Only OVL0 - the overlay the probe proved is scanning out. */
    if (g_WinMaliOvlFlip != 0 && g_WinMaliOvlRegsVa == NULL) {
        PHYSICAL_ADDRESS opa;
        opa.QuadPart = (LONGLONG)WINMALI_OVL0_PA;
        g_WinMaliOvlRegsVa = MmMapIoSpaceEx(opa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
        DbgPrint("[WinMali]    OVL flip armed: mode=%u regsVa=%p\n",
                 g_WinMaliOvlFlip, g_WinMaliOvlRegsVa);
    }
    /* Mode 2 additionally needs the RIGHT-half overlay and a back frame. Both
       mapped once, at PASSIVE_LEVEL, and never from the present path. */
    if (g_WinMaliOvlFlip == 2 && g_WinMaliOvl4RegsVa == NULL) {
        PHYSICAL_ADDRESS p4, pb;
        p4.QuadPart = (LONGLONG)WINMALI_OVL4_PA;
        g_WinMaliOvl4RegsVa = MmMapIoSpaceEx(p4, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
        pb.QuadPart = (LONGLONG)WINMALI_FB_FRAME2_PA;
        /* PAYLOAD, not STRIDE - the 0x400 difference between them would map 1 KB
           of the next region for no reason. */
        g_WinMaliOvlBackVa = MmMapIoSpaceEx(pb, (SIZE_T)WINMALI_FB_FRAME_PAYLOAD,
                                            PAGE_READWRITE | PAGE_NOCACHE);
        g_WinMaliOvlBackPa = (LONG64)WINMALI_FB_FRAME2_PA;
        /* The probe just read OVL0, so we know which frame is live right now. */
        g_WinMaliOvlFrontPa = (LONG64)WINMALI_FB_FRAME1_PA;
        DbgPrint("[WinMali]    OVL mode2: ovl4=%p backVa=%p backPa=0x%llx front=0x%llx\n",
                 g_WinMaliOvl4RegsVa, g_WinMaliOvlBackVa,
                 (ULONGLONG)g_WinMaliOvlBackPa, (ULONGLONG)g_WinMaliOvlFrontPa);
    }
    /* ⛔★★★★★ BLOCKER 1 FIX - THE ONLY SOUND "DID THE WRITE LAND" TEST.
     * Writing L0_ADDR's own value back and reading it again proves NOTHING: a
     * dropped write and a landed write produce identical observables. MTK
     * reserves a scratch register for exactly this - DISP_REG_OVL_DUMMY_REG
     * (0x200), which appears at mtk_disp_ovl.c:209 and NOWHERE ELSE in their
     * driver, so nothing reads or acts on it. Write magic, read back, restore.
     * Back == magic && Back != Orig is a TRUE proof, with zero scanout effect.
     * ⛔ Restore the original unconditionally - leaving our magic behind would
     *    be a change to hardware state we did not need to make. */
    if (g_WinMaliOvlFlip != 0 && g_WinMaliOvlRegsVa != NULL) {
        volatile ULONG *dmy = (volatile ULONG *)((PUCHAR)g_WinMaliOvlRegsVa + 0x200u);
        ULONG orig, back;
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlStep", 0x3500u);
        orig = READ_REGISTER_ULONG(dmy);
        WRITE_REGISTER_ULONG(dmy, 0x574D0001u);          /* 'WM' + 1 */
        back = READ_REGISTER_ULONG(dmy);
        WRITE_REGISTER_ULONG(dmy, orig);                 /* restore, always */
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlDummyOrig", orig);
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlDummyBack", back);
        DbgPrint("[WinMali]    OVL dummy-reg write test: orig=0x%08x wrote=0x574D0001 "
                 "back=0x%08x -> writes %s\n", orig, back,
                 (back == 0x574D0001u) ? "LAND" : "DO NOT LAND");
    }
    /* ---- READ-ONLY DSI PROBE: latching or self-clearing? ---------------- */
    if (g_WinMaliDsiProbe != 0) {
        static const ULONG dsiBase[2] = { 0x1400D000u, 0x1420D000u };
        ULONG  d, k;
        ULONG  samples[2][8];
        ULONG  misc[2][3];
        for (d = 0; d < 2u; d++) {
            volatile UCHAR *dva;
            PHYSICAL_ADDRESS dpa;
            for (k = 0; k < 8u; k++) { samples[d][k] = 0xDEADC0DEu; }
            misc[d][0] = misc[d][1] = misc[d][2] = 0xDEADC0DEu;
            if ((g_WinMaliDsiProbe & (1u << d)) == 0) { continue; }
            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlStep",
                                                 0x5000u | d);
            dpa.QuadPart = (LONGLONG)dsiBase[d];
            dva = (volatile UCHAR *)MmMapIoSpaceEx(dpa, 0x1000,
                                                   PAGE_READWRITE | PAGE_NOCACHE);
            if (dva == NULL) { continue; }
            misc[d][0] = READ_REGISTER_ULONG((volatile ULONG *)(dva + 0x08)); /* INTEN     */
            misc[d][1] = READ_REGISTER_ULONG((volatile ULONG *)(dva + 0x2C)); /* VACT_NL   */
            misc[d][2] = READ_REGISTER_ULONG((volatile ULONG *)(dva + 0x160));/* STATE_DBG6*/
            /* 8 samples of INTSTA. A LATCHED bit reads identical every time; a
               live one varies across a ~60 Hz frame. KeStallExecutionProcessor
               is legal here (PASSIVE) and 2 ms x 8 spans a frame at 60 Hz. */
            for (k = 0; k < 8u; k++) {
                samples[d][k] = READ_REGISTER_ULONG((volatile ULONG *)(dva + 0x0C));
                KeStallExecutionProcessor(2000);   /* microseconds */
            }
            MmUnmapIoSpace((PVOID)dva, 0x1000);
            DbgPrint("[WinMali]    DSI%u @0x%08x INTEN=0x%08x VACT_NL=0x%08x DBG6=0x%08x\n",
                     d, dsiBase[d], misc[d][0], misc[d][1], misc[d][2]);
            DbgPrint("[WinMali]    DSI%u INTSTA x8: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                     d, samples[d][0], samples[d][1], samples[d][2], samples[d][3],
                     samples[d][4], samples[d][5], samples[d][6], samples[d][7]);
        }
        {
            ULONG blob[2 * 11];
            ULONG i2;
            for (d = 0; d < 2u; d++) {
                for (i2 = 0; i2 < 8u; i2++) { blob[d * 11 + i2] = samples[d][i2]; }
                blob[d * 11 + 8] = misc[d][0];
                blob[d * 11 + 9] = misc[d][1];
                blob[d * 11 + 10] = misc[d][2];
            }
            (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"StatDsiDump",
                                           blob, (ULONG)sizeof(blob));
        }
        (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlStep", 0x5900u);
    }
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlStep", 0x3000u);
    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath, L"StatOvlMatch", match);
    (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"StatOvlDump",
                                   regs, (ULONG)sizeof(regs));
    DbgPrint("[WinMali]    OVL probe done: mask=0x%x match=%u (0 = none matched "
             "0x%08x)\n", g_WinMaliOvlProbeMask, match, (ULONG)WINMALI_FB_SCANOUT_PA);
}

NTSTATUS APIENTRY
WinMaliKmdStartDevice(
    _In_  CONST PVOID            MiniportDeviceContext,
    _In_  PDXGK_START_INFO       DxgkStartInfo,
    _In_  PDXGKRNL_INTERFACE     DxgkInterface,
    _Out_ PULONG                 NumberOfVideoPresentSources,
    _Out_ PULONG                 NumberOfChildren)
{
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromContext(MiniportDeviceContext);
    NTSTATUS         status;
    WM_TRACE_ENTER("StartDevice");
    g_WinMaliAdapterStartTick = KeQueryInterruptTime();
    WinMaliVpTrail_(8);   /* adapter restarts show up in the VidPn trail */

    if (adapter == NULL || DxgkStartInfo == NULL || DxgkInterface == NULL ||
        NumberOfVideoPresentSources == NULL || NumberOfChildren == NULL) {
        WM_TRACE_EXIT_S("StartDevice", STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    adapter->DxgkStartInfo = *DxgkStartInfo;
    adapter->DxgkInterface = *DxgkInterface;
    adapter->DxgkHandle    = DxgkInterface->DeviceHandle;
    adapter->Started       = TRUE;

    /* Construct the synthesised vsync source. It stays DISARMED until dxgk
       actually asks for CRTC_VSYNC through ControlInterrupt, and it is only
       meaningful for a display adapter -- an MCDM adapter never arms it, so
       the known-good compute-only path is unaffected either way. */
    if (g_WinMaliComputeOnly == 0) {
        WinMaliVsyncInit(adapter);
        /* Map the live scanout + start the present worker. Must follow the DMA
           segment setup: the blit source is DmaSegmentVa + (flipAddr - segbase). */
        WinMaliPresentInit(adapter);
    }

    /* Render-only (the shipping configuration) reports zero of both. The two
       counts are separate knobs: see g_WinMaliChildCount for why. */
    *NumberOfVideoPresentSources = g_WinMaliVidPnSources;
    *NumberOfChildren            = g_WinMaliChildCount;
    DbgPrint("[WinMali]    StartDevice reporting VidPnSources=%lu Children=%lu\n",
             g_WinMaliVidPnSources, g_WinMaliChildCount);

    WinMaliDodStage(3);
    DbgPrint("[WinMali]    StartDevice ctx=%p handle=%p\n",
             adapter, adapter->DxgkHandle);

    /*
     * ★★★ CLAIM THE POST DISPLAY.
     *
     * A display miniport does not become the POST (firmware framebuffer) owner
     * passively -- it ASKS, here, with DxgkCbAcquirePostDisplayOwnership. We
     * never called it, and that is very likely why dxgk will not let us own an
     * embedded panel:
     *
     *   sweeping the child's InterfaceTechnology splits the failure exactly on
     *   embedded-vs-external. INTERNAL / DP_EMBEDDED / LVDS all fail at
     *   StartAdapter_DpiFdoEnumChildDevicesFailed(15); HDMI / DP_EXTERNAL fail
     *   at 3 instead. dxgk plainly gates embedded panels on POST ownership --
     *   it even has reasons FdoIsNotAPostDevice(14) and
     *   PostDeviceIsRenderOnly(20) -- and on this machine BasicDisplay holds it
     *   (Windows refuses to disable that device at all: "Not supported").
     *
     * Only when acting as a display: an MCDM adapter has no business taking the
     * panel, and the ComputeOnly path is known-good and must stay byte-identical.
     *
     * The returned DXGK_DISPLAY_INFORMATION is also an independent CHECK on the
     * framebuffer geometry WinMaliVidPn.c currently hardcodes (2944x1840,
     * stride 11776, scanout 0xFD619400). If dxgk disagrees, the hardcoded
     * constants are wrong and the VidPN modes built from them are fiction.
     */
    if (g_WinMaliComputeOnly == 0 &&
        adapter->DxgkInterface.DxgkCbAcquirePostDisplayOwnership != NULL) {
        DXGK_DISPLAY_INFORMATION di;
        NTSTATUS                 postStatus;

        RtlZeroMemory(&di, sizeof(di));
        postStatus = adapter->DxgkInterface.DxgkCbAcquirePostDisplayOwnership(
                         adapter->DxgkHandle, &di);
        /*
         * ⛔ NT_SUCCESS HERE DOES NOT MEAN WE OWN THE POST DISPLAY.
         *
         * Measured 2026-08-12: the call returns SUCCESS with an ENTIRELY EMPTY
         * DXGK_DISPLAY_INFORMATION - 0x0, pitch 0, phys 0x0, TargetId
         * 0xFFFFFFFF - and we logged "POST display ACQUIRED" on top of it for
         * weeks. A real POST owner gets the live EDK2 framebuffer back
         * (2944x1840, pitch 11776, phys 0xFD619400). An all-zero struct means
         * BasicDisplay still owns the firmware framebuffer and we do not.
         *
         * This matters far beyond a log line: dxgk only lets the POST owner
         * enumerate an EMBEDDED panel child, which is exactly why
         * VOT_INTERNAL / DP_EMBEDDED / LVDS die at
         * StartAdapter_DpiFdoEnumChildDevicesFailed(15) while HDMI /
         * DP_EXTERNAL get further. So judge ownership on the DATA, not the
         * status.
         */
        /* PUBLISH IT. The ring is not a reliable place to keep this: once the
           compositor drives us the per-submit traces wrap it in under a minute
           and this line is gone. A registry value survives, and is readable
           over WinRM with no kernel debugger and no timing race. */
        if (g_WinMaliRegPath.Length != 0) {
            BOOLEAN owned = (BOOLEAN)(NT_SUCCESS(postStatus) &&
                                      di.Width != 0 && di.Height != 0 &&
                                      di.PhysicAddress.QuadPart != 0);
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPostOwned", owned ? 1u : 0u);
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPostWidth",  (ULONG)di.Width);
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPostHeight", (ULONG)di.Height);
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPostPitch",  (ULONG)di.Pitch);
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPostPhysLo",
                                          (ULONG)(di.PhysicAddress.QuadPart & 0xFFFFFFFF));
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPostPhysHi",
                                          (ULONG)((ULONG64)di.PhysicAddress.QuadPart >> 32));
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPostStatus",
                                          (ULONG)postStatus);
        }

        if (NT_SUCCESS(postStatus) &&
            (di.Width == 0 || di.Height == 0 || di.PhysicAddress.QuadPart == 0)) {
            DbgPrint("[WinMali] ** POST display NOT OWNED: call succeeded but returned an "
                     "EMPTY display info (%ux%u pitch=%u phys=0x%llx target=0x%x). "
                     "BasicDisplay still owns the firmware framebuffer, so dxgk will "
                     "REFUSE an embedded-panel child (reason 15).\n",
                     (unsigned)di.Width, (unsigned)di.Height, (unsigned)di.Pitch,
                     (unsigned long long)di.PhysicAddress.QuadPart,
                     (unsigned)di.TargetId);
        } else if (NT_SUCCESS(postStatus)) {
            DbgPrint("[WinMali]    POST display ACQUIRED: %ux%u pitch=%u fmt=%u "
                     "phys=0x%llx target=%u acpi=%u\n",
                     (unsigned)di.Width, (unsigned)di.Height, (unsigned)di.Pitch,
                     (unsigned)di.ColorFormat,
                     (unsigned long long)di.PhysicAddress.QuadPart,
                     (unsigned)di.TargetId, (unsigned)di.AcpiId);
        } else {
            /* Not fatal: report it and carry on so we can see what dxgk does
               next. A hard failure here would hide the very evidence we want. */
            DbgPrint("[WinMali]    POST display NOT acquired: 0x%08x "
                     "(we are not the POST device)\n",
                     (unsigned)postStatus);
        }
    }

    /* ===================================================================
     * DISPLAY-ONLY: STOP HERE. Everything below brings up the GPU, and a
     * display-only miniport does not have one - it has a framebuffer that
     * firmware already scans out. Continuing would also publish a 256 MiB
     * VidMm segment that a DOD must not have, and would call VidMm/scheduler
     * callbacks that are absent from the reduced DXGKRNL_INTERFACE dxgk gives
     * a display-only driver.
     *
     * MEASURED: without this, StatDodStage stops at 3 (StartDevice entered,
     * never returned) and the adapter ends at CM_PROB_FAILED_POST_START.
     *
     * POST display ownership above is KEPT deliberately - it is what makes this
     * adapter the owner of the panel, and DxgkDdiPresentDisplayOnly copies into
     * exactly that framebuffer.
     * =================================================================== */
    /* ⛔★★ THE OVL PROBE MUST RUN IN **BOTH** MODES, SO IT GOES ABOVE THIS
     * RETURN. 1.1.14.0 called it only at the very end of the render path, and
     * display-only returns right here - so on the DOD boot the probe never ran
     * and StatOvlStep/StatOvlMatch kept whatever the deploy script had left in
     * them. The register dump still arrived, because the pnputil rebind happened
     * while DodMode was still fuse-burned to 0 (i.e. on the RENDER path), which
     * made it look like two instruments had failed when the code was simply
     * never reached on the boot being measured.
     * ★ A PROBE PLACED AFTER A MODE-SPECIFIC RETURN MEASURES ONE MODE ONLY. */
    WinMaliOvlProbe_();

    if (g_WinMaliDodMode != 0) {
        WinMaliDodStage(4);
        DbgPrint("[WinMali]    DISPLAY-ONLY StartDevice OK: sources=%lu children=%lu "
                 "postOwned=%u - GPU bring-up deliberately SKIPPED\n",
                 g_WinMaliVidPnSources, g_WinMaliChildCount,
                 (unsigned)(adapter->ScanoutVa != NULL));
        WM_TRACE_EXIT_S("StartDevice", STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    /* Real bring-up. Parse ACPI _CRS, map MMIO, probe GPU_ID. Lifted from
       the BadDriver where this was the part that actually worked. */
    status = WinMaliParseResources(adapter);
    if (!NT_SUCCESS(status)) {
        WINMALI_ERROR("ParseResources failed 0x%08x", status);
        /* Return success anyway so dxgk doesn't immediately tear us down
           via a hot-error path. We continue with no MMIO; subsequent
           registers reads return 0 and the warning is in the log. */
        WM_TRACE_EXIT_S("StartDevice", STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    status = WinMaliBringupHardware(adapter);
    if (!NT_SUCCESS(status)) {
        WINMALI_ERROR("BringupHardware failed 0x%08x", status);
        WM_TRACE_EXIT_S("StartDevice", STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    /* Re-seed the AS-slot allocator now that GPU_AS_PRESENT has been read. */
    WinMaliMmuInitAsAllocator(adapter);

    /*
     * ---- STAGED BRING-UP (MT6897 port) -------------------------------------
     * g_WinMaliBringupStage gates how far StartDevice goes, so the first boot
     * on this platform can prove the dxgk path (AddAdapter, StartDevice, QAI,
     * the display-DDI all-or-none sweep) with the GPU touched only for its
     * identity registers. Everything beyond stage 0 either writes GPU state or
     * DMAs into it, and a fault there is a bugcheck we would have to diagnose
     * with no prior evidence that the plumbing above it even works.
     *
     *   0 = MMIO map + GPU_ID probe only
     *   1 = + MMU init, interrupt record, VIDMM segment
     *   2 = + CSF firmware load and MCU boot   (full, the upstream behaviour)
     *
     * Set via HKLM\SYSTEM\CurrentControlSet\Services\WinMaliKmd\Parameters
     * REG_DWORD "BringupStage". Defaults to 2 so behaviour is unchanged for
     * anyone who does not set it.
     */

    /* MMU scratch heap + AS1 bind. Lifted from panthor_mmu_init. */
    if (adapter->GpuRegsMapped && g_WinMaliBringupStage >= 1) {
        NTSTATUS mmuStatus = WinMaliMmuInit(adapter);
        if (!NT_SUCCESS(mmuStatus)) {
            WINMALI_WARN("WinMaliMmuInit failed 0x%08x (AS1 not bound)",
                         mmuStatus);
        }
    }

    /* Record IRQ availability for the FW init path. The ISR itself is
       bound by dxgk via init->DxgkDdiInterruptRoutine. */
    {
        NTSTATUS irqStatus = (g_WinMaliBringupStage >= 1)
                             ? WinMaliConnectInterrupt(adapter) : STATUS_SUCCESS;
        if (!NT_SUCCESS(irqStatus)) {
            WINMALI_WARN("WinMaliConnectInterrupt failed 0x%08x", irqStatus);
        }
    }

    /* 256 MiB sysmem segment for dxgk. Allocated even if FW init later
       fails, so VIDMM still sees a memory pool and the cap walk has
       real numbers to point at. */
    {
        NTSTATUS segStatus = (g_WinMaliBringupStage >= 1)
                             ? WinMaliVidmmAllocateSegment(adapter) : STATUS_SUCCESS;
        if (!NT_SUCCESS(segStatus)) {
            WINMALI_WARN("WinMaliVidmmAllocateSegment failed 0x%08x", segStatus);
        }
    }

    /* CSF firmware boot. Reads \SystemRoot\System32\drivers\mali_csffw.bin,
       parses the panthor binary, loads sections to MCU AS0, sets
       MCU_CONTROL=AUTO and polls MCU_STATUS for ENABLED. */
    if (adapter->GpuRegsMapped && g_WinMaliBringupStage >= 2) {
        NTSTATUS fwStatus = WinMaliFwInit(adapter);
        if (!NT_SUCCESS(fwStatus)) {
            WINMALI_WARN("WinMaliFwInit failed 0x%08x (CSF firmware not active)",
                         fwStatus);
        }
    }

    /* No-op self-test removed - it was useful while we were still proving
       MCU boot end-to-end, but now FW init itself signals MCU_ALIVE and
       CSF_JOBS, and we want every subsequent boot to land in a clean idle
       state with proper IRQ delivery, not a synchronous test submission. */

    /* Bring up the async submit worker (drains DxgkDdiSubmitCommand /
       PreemptCommand off the dxgk scheduler thread). Non-fatal if it fails -
       SubmitCommand falls back to inline completion. */
    (VOID)WinMaliSchedInit(adapter);

    adapter->StartDeviceEverSucceeded = TRUE;
    /* Fresh start: escape waits are allowed to block again. */
    InterlockedExchange(&g_WinMaliStopping, 0);
    WINMALI_TRACE("StartDevice OK: regs=%u mmu=%u mcu_alive=%u csf_jobs=%u",
                  adapter->GpuRegsMapped,
                  (adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_MMU_READY) ? 1 : 0,
                  (adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_MCU_ALIVE) ? 1 : 0,
                  (adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_CSF_JOBS) ? 1 : 0);
    WM_TRACE_EXIT_S("StartDevice", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdStopDevice(_In_ CONST PVOID MiniportDeviceContext)
{
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromContext(MiniportDeviceContext);
    WM_TRACE_ENTER("StopDevice");
    WinMaliDodDdi(WINMALI_DOD_DDI_STOPDEVICE);
    WinMaliVpTrail_(9);
    if (adapter == NULL) {
        WM_TRACE_EXIT_S("StopDevice", STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * ---- TEARDOWN PROGRESS MARKER ---------------------------------------
     *
     * Every crash tonight has been INSIDE StopDevice, and we cannot see where:
     * UFS has no dump mode, and the boot-log ring is published by mtkcpufreq
     * (dev install only) and is rewritten by EDK2 on the next boot, so nothing
     * logged survives the bugcheck. THE REGISTRY DOES.
     *
     * So stamp the stage before each teardown step. After a crash, read
     * Parameters\StatTeardownStage: the value is the last stage that STARTED,
     * which localises the fault to one step instead of one function. A clean
     * stop ends at 99.
     *
     * This is the "publish the counter, do not trust the log" rule applied to
     * crash localisation - and it is cheap: StopDevice is PASSIVE_LEVEL and
     * runs once per experiment.
     */
#define WM_TEARDOWN_STAGE(n)                                                   \
    do {                                                                       \
        if (g_WinMaliRegPath.Length != 0) {                                    \
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,                   \
                                          L"StatTeardownStage", (ULONG)(n));   \
        }                                                                      \
    } while (0)

    WM_TEARDOWN_STAGE(1);

    /*
     * PUBLISH THE PRESENT-PATH EVIDENCE BEFORE TEARING ANYTHING DOWN.
     *
     * The panel shows a live firmware image whether or not our flip path ever
     * ran, so nothing visible can tell us if dxgk exercised it. These counters
     * can -- but only if they leave the driver. An unpublished counter is
     * indistinguishable from one that never incremented, which is a trap this
     * project has already paid for more than once.
     *
     * StopDevice runs on every remove-device/scan-devices cycle, i.e. on every
     * experiment, and it is PASSIVE_LEVEL, so the registry write is legal here
     * (the counters themselves are bumped at DIRQL via Interlocked).
     */
    /* Announce teardown BEFORE anything blocking, so any escape thread already
       parked in WinMaliWaitObjectAbortable gives up now instead of holding the
       PnP lock until the 300 s power-transition timeout (0x9F P1=4). */
    InterlockedExchange(&g_WinMaliStopping, 1);

    /* Stop the vsync timer FIRST: its DPC touches DxgkInterface/DxgkHandle,
       both of which are about to be zeroed below. A timer still firing into a
       torn-down adapter is a bugcheck, and on this machine that costs a
       physical recovery. */
    WM_TEARDOWN_STAGE(2);
    WinMaliVsyncTeardown(adapter);
    /* ⚠ NOT here: teardown nulls ScanoutVa, and the PRESENT-V1 line below
       prints it - which reported "va=0" on a run that had just done 66
       successful blits. Publish the evidence FIRST, tear down after. */

    DbgPrint("[WinMali]    PRESENT-PATH: flips=%lld vsyncArms=%lld vsyncNotifies=%lld "
             "lastAddr=0x%llx seg=%ld flags=0x%lx visible=%ld\n",
             adapter->FlipLatchCount, adapter->VsyncArmCount, adapter->VsyncNotifyCount,
             (unsigned long long)adapter->LastFlipAddress,
             adapter->LastFlipSegment, adapter->LastFlipFlags,
             adapter->SourceVisible);
    /* Scheduler health. FenceQueueDropped MUST be 0: any nonzero value means we
       threw away a DMA_COMPLETED dxgk was waiting for, which is the 0x119
       VIDEO_SCHEDULER_INTERNAL_ERROR we took on 2026-08-12. Peak tells us how
       close the queue actually runs to its depth, so "it didn't overflow" is a
       measurement rather than an assumption. */
    DbgPrint("[WinMali]    SCHED: fenceQueuePeak=%lu/%u dropped=%lu csfSubmitFailures=%lu\n",
             adapter->FenceQueuePeak, WINMALI_FENCE_QUEUE_DEPTH,
             adapter->FenceQueueDropped, adapter->CsfSubmitFailures);
    /* V1 PRESENT EVIDENCE. scanoutBlits>0 is the ONLY proof that anything of
       ours ever reached the panel. skipped>0 means a flip address failed to
       resolve into our DMA segment - a translation bug that would otherwise
       present as nothing but a blank screen. visibleEver answers "was the
       source EVER made visible", which the last-value SourceVisible cannot
       (teardown always ends at 0). */
    DbgPrint("[WinMali]    PRESENT-V1: scanoutBlits=%lld skipped=%lld visibleEver=%ld va=%p\n",
             adapter->ScanoutBltCount, adapter->ScanoutBltSkipped,
             adapter->SourceVisibleEver, adapter->ScanoutVa);
    /*
     * ⛔ DO NOT LET A NO-OP INSTANCE ERASE A REAL ONE.
     *
     * Every StartDevice allocates a FRESH adapter, and StopDevice publishes
     * THAT instance's counters. The restore sequence is remove-device then
     * scan-devices, and on gpufb the scan's render-only StartDevice fails
     * POST-START - so its StopDevice fired straight after and wrote its own
     * zeros over the armed run's real numbers.
     *
     * Measured 2026-08-12: the user watched the marker square painting at
     * 30 Hz while the published StatScanoutBlits read 0. Both were "true" -
     * the zeros belonged to a different adapter instance. This is the twin of
     * "an unpublished counter is indistinguishable from one that never
     * incremented": a counter OVERWRITTEN by a later no-op instance also
     * reads as never-incremented.
     *
     * So an instance that did nothing at all publishes nothing.
     */
    if (g_WinMaliRegPath.Length != 0 &&
        (adapter->FlipLatchCount   != 0 || adapter->ScanoutBltCount != 0 ||
         adapter->VsyncNotifyCount != 0 || adapter->ScanoutBltSkipped != 0 ||
         adapter->SourceVisibleEver != 0)) {
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatScanoutBlits",
                                      (ULONG)adapter->ScanoutBltCount);
        /* â›” THE OVL FLIP COUNTERS ARE OTHERWISE DOD-ONLY. They are published from
           WinMaliDodPresentRect, which render mode never reaches, so on a render boot
           they show a stale value for ever and "the flips stopped" is indistinguishable
           from "nobody wrote the number down". Publish them from the worker, which does
           run in render. Same names, so no new value is created on a key that is at its
           127-name cap. */
        /* â›” The flip counters were published HERE, which is DxgkDdiStopDevice - it
           runs only at teardown, so they read stale for three whole builds. Moved to
           the live worker publish (search StatScanoutBlits with a->ScanoutBltCount). */
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatScanoutSkipped",
                                      (ULONG)adapter->ScanoutBltSkipped);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatSourceVisibleEver",
                                      (ULONG)adapter->SourceVisibleEver);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatFenceQueuePeak",
                                      adapter->FenceQueuePeak);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatFenceQueueDropped",
                                      adapter->FenceQueueDropped);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatFlipLatches",
                                      (ULONG)adapter->FlipLatchCount);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatVsyncArms",
                                      (ULONG)adapter->VsyncArmCount);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatVsyncNotifies",
                                      (ULONG)adapter->VsyncNotifyCount);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatLastFlipLo",
                                      (ULONG)(adapter->LastFlipAddress & 0xFFFFFFFF));
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatLastFlipHi",
                                      (ULONG)((ULONG64)adapter->LastFlipAddress >> 32));
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatSourceVisible",
                                      (ULONG)adapter->SourceVisible);
    }

    /* Present worker + scanout mapping go here, AFTER the evidence above has
       been printed and published (see the note at VsyncTeardown). */
    WM_TEARDOWN_STAGE(3);
    WinMaliPresentTeardown(adapter);

    /* Stop the async submit worker BEFORE tearing down the FW/CSF so it can't
       be mid-CSF-submit while the ring/MCU goes away. */
    WM_TEARDOWN_STAGE(4);
    WinMaliSchedTeardown(adapter);
    WM_TEARDOWN_STAGE(5);
    WinMaliFwTeardown(adapter);
    WM_TEARDOWN_STAGE(6);
    WinMaliDisconnectInterrupt(adapter);
    WM_TEARDOWN_STAGE(7);
    WinMaliMmuTeardown(adapter);
    WM_TEARDOWN_STAGE(8);
    WinMaliVidmmFreeSegment(adapter);
    WM_TEARDOWN_STAGE(9);
    WinMaliTeardownHardware(adapter);
    adapter->Started = FALSE;
    RtlZeroMemory(&adapter->DxgkInterface, sizeof(adapter->DxgkInterface));
    adapter->DxgkHandle = NULL;

    /* 99 = StopDevice completed. Any other value read back after a crash is
       the stage that was executing when we died. */
    WM_TEARDOWN_STAGE(99);
    WM_TRACE_EXIT_S("StopDevice", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdRemoveDevice(_In_ CONST PVOID MiniportDeviceContext)
{
    WinMaliDodDdi(WINMALI_DOD_DDI_REMOVEDEVICE);
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromContext(MiniportDeviceContext);
    WM_TRACE_ENTER("RemoveDevice");
    if (adapter == NULL) {
        WM_TRACE_EXIT_S("RemoveDevice", STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * ⛔⛔ NEVER FREE A STRUCTURE CONTAINING TIMERS/DPCs WITHOUT CANCELLING
     * THEM FIRST - AND DO NOT ASSUME StopDevice RAN.
     *
     * dxgk does NOT guarantee a StopDevice before RemoveDevice. The case that
     * bit us: StartDevice SUCCEEDS (and WinMaliPresentInit arms the present
     * timer) and the device then fails POST-START, which is exactly what
     * render-only does on the gpufb firmware (CM_PROB_FAILED_POST_START).
     * dxgk goes straight to RemoveDevice, we freed the adapter with a live
     * timer in it, and the next tick dispatched through freed memory:
     *     0xD1  P2=0x02 (IRQL 2)  P3=0x08 (EXECUTE)  P1==P4
     *
     * Isolated by varying Parameters\PresentTimerHz: 30 Hz -> crash, 1 Hz ->
     * crash (so NOT rate-dependent), 0 -> clean. Only "armed at all" matters.
     * Unreachable on the older gpudod firmware because render-only started
     * fine there, so RemoveDevice was always preceded by a StopDevice.
     *
     * Both teardowns are idempotent (each guards on its own init flag), so
     * calling them here costs nothing when StopDevice already ran.
     */
    WinMaliVsyncTeardown(adapter);
    WinMaliPresentTeardown(adapter);

    /* Drain UMD escape state the caller didn't explicitly destroy. Order
       matters: Groups reference VMs (CSG slots may still be claimed); VMs
       own LPAE PT pages whose teardown also releases AS slots. BOs and
       SyncObjs are independent. */
    WinMaliGroupTableTeardown(adapter, &adapter->GroupTable);
    WinMaliVmTableTeardown(adapter, &adapter->VmTable);
    WinMaliBoTableTeardown(adapter, &adapter->BoTable);
    /* Pair of the WMADPT1 line at adapter start - see the note there. If this
       never appears in a boot capture, the syncobj table is NOT being rebuilt
       and every "the table was torn down under dwm" theory is dead. */
    WINMALI_WARN("WMADPT1 AdapterStop: tearing down the per-adapter syncobj "
                 "table (count=%ld)", adapter->SyncObjTable.Count);
    WinMaliSyncObjTableTeardown(adapter, &adapter->SyncObjTable);

    if (g_WinMaliAdapter == adapter) {
        g_WinMaliAdapter = NULL;
    }
    adapter->Magic = 0;
    ExFreePoolWithTag(adapter, WINMALI_POOL_TAG);
    WM_TRACE_EXIT_S("RemoveDevice", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* DDI stubs                                                              */
/* ---------------------------------------------------------------------- */

NTSTATUS APIENTRY
WinMaliKmdDispatchIoRequest(
    _In_ CONST PVOID                MiniportDeviceContext,
    _In_ ULONG                      VidPnSourceId,
    _In_ PVIDEO_REQUEST_PACKET      VideoRequestPacket)
{
    WM_TRACE_ENTER("DispatchIoRequest");
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(VideoRequestPacket);
    WM_TRACE_EXIT_S("DispatchIoRequest", STATUS_NOT_SUPPORTED);
    return STATUS_NOT_SUPPORTED;
}

BOOLEAN APIENTRY
WinMaliKmdInterruptRoutine(
    _In_ CONST PVOID MiniportDeviceContext,
    _In_ ULONG       MessageNumber)
{
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromContext(MiniportDeviceContext);
    ULONG gpuRaw, jobRaw, mmuRaw;
    BOOLEAN handled = FALSE;

    UNREFERENCED_PARAMETER(MessageNumber);

    /* No DbgPrint here - this fires at DIRQL and can repeat fast. */

    if (adapter == NULL || !adapter->GpuRegsMapped) {
        return FALSE;
    }

    /* Read the three IRQ block RAWSTAT registers. Any non-zero bit means
       the GPU is asserting that line. Ack by writing the same bits to
       the matching _CLEAR register. Without this the line stays high,
       the OS re-dispatches the ISR forever, DPCs starve, watchdog fires. */
    gpuRaw = WinMaliHwRead32(&adapter->Hw, WINMALI_REG_GPU_IRQ_RAWSTAT);
    jobRaw = WinMaliHwRead32(&adapter->Hw, WINMALI_REG_JOB_INT_RAWSTAT);
    mmuRaw = WinMaliHwRead32(&adapter->Hw, WINMALI_REG_MMU_INT_RAWSTAT);

    /* FAULT DECODE (fault-detection audit 2026-07-13). The ISR used to W1C-ack
       every bit and read no fault-status register, so a real GPU/MMU fault was
       acknowledged and silently discarded ("clean completion, black output").
       Read the fault-status registers and log the FIRST REAL fault per boot.
       CRITICAL: only a NON-ZERO fault-status register is a real fault - the
       MMU_INT bit 17 (0x20000) is a benign per-AS command-completion notify
       that fires every healthy boot (FAULTSTATUS reads 0). Gating the one-shot
       on FAULTSTATUS!=0 stops that benign bit from consuming the latch and
       hiding a later real fault. WinMaliLogPrint -> polled UART is DIRQL-safe. */
    if ((gpuRaw & (WINMALI_GPU_IRQ_FAULT | WINMALI_GPU_IRQ_PROTM_FAULT)) || mmuRaw != 0) {
        static LONG s_isrFaultReported = 0;
        ULONG gpFs = (gpuRaw & (WINMALI_GPU_IRQ_FAULT | WINMALI_GPU_IRQ_PROTM_FAULT))
                   ? WinMaliHwRead32(&adapter->Hw, WINMALI_REG_GPU_FAULT_STATUS) : 0u;
        ULONG asHit = 0xFFu, asFs = 0u, as;
        for (as = 0; as < 8u; ++as) {
            if (mmuRaw & ((1u << as) | (1u << (as + 16)))) {
                ULONG fs = WinMaliHwRead32(&adapter->Hw, WINMALI_REG_AS_FAULTSTATUS(as));
                if (fs != 0u) { asHit = as; asFs = fs; break; }
            }
        }
        if (gpFs != 0u || asFs != 0u) {
            ULONG addrLo = 0, addrHi = 0;

            if (asFs != 0u) {
                addrLo = WinMaliHwRead32(&adapter->Hw, WINMALI_REG_AS_FAULTADDR_LO(asHit));
                addrHi = WinMaliHwRead32(&adapter->Hw, WINMALI_REG_AS_FAULTADDR_HI(asHit));
            } else {
                addrLo = WinMaliHwRead32(&adapter->Hw, WINMALI_REG_GPU_FAULT_ADDR_LO);
                addrHi = WinMaliHwRead32(&adapter->Hw, WINMALI_REG_GPU_FAULT_ADDR_HI);
            }

            /* LATCH the latest fault, then COUNT it. Order matters: the details
               must be visible before the count moves, or a submit that sees the
               new count could read stale details. */
            (VOID)InterlockedExchange(&adapter->FaultAs,        (LONG)asHit);
            (VOID)InterlockedExchange(&adapter->FaultFst,       (LONG)asFs);
            (VOID)InterlockedExchange(&adapter->FaultAddrLo,    (LONG)addrLo);
            (VOID)InterlockedExchange(&adapter->FaultAddrHi,    (LONG)addrHi);
            (VOID)InterlockedExchange(&adapter->FaultGpuStatus, (LONG)gpFs);
            KeMemoryBarrier();
            (VOID)InterlockedIncrement(&adapter->FaultCount);

            /* Logging stays rate-limited - this is DIRQL and a storm of faults
               must not wedge the box on a polled UART - but the COUNT above is
               exact regardless of how much we print. */
            if (InterlockedCompareExchange(&s_isrFaultReported, 1, 0) == 0) {
                if (gpFs != 0u) {
                    WinMaliLogPrint("[WinMali] ISR GPU FAULT: raw=0x%08x FAULT_STATUS=0x%08x ADDR=0x%08x%08x\n",
                                    gpuRaw, gpFs, addrHi, addrLo);
                }
                if (asFs != 0u) {
                    WinMaliLogPrint("[WinMali] ISR MMU FAULT AS%u: mmu_raw=0x%08x FAULTSTATUS=0x%08x ADDR=0x%08x%08x\n",
                                    asHit, mmuRaw, asFs, addrHi, addrLo);
                }
            }
        }
    }

    if (gpuRaw != 0) {
        WinMaliHwWrite32(&adapter->Hw, WINMALI_REG_GPU_IRQ_CLEAR, gpuRaw);
        handled = TRUE;
    }
    if (jobRaw != 0) {
        WinMaliHwWrite32(&adapter->Hw, WINMALI_REG_JOB_INT_CLEAR, jobRaw);
        handled = TRUE;
    }
    if (mmuRaw != 0) {
        WinMaliHwWrite32(&adapter->Hw, WINMALI_REG_MMU_INT_CLEAR, mmuRaw);
        handled = TRUE;
    }

    if (handled) {
        InterlockedIncrement64(&adapter->InterruptsHandled);
    } else {
        InterlockedIncrement64(&adapter->InterruptsSpurious);
    }

    /* Storm telemetry. An interrupt storm is otherwise INVISIBLE on the
       serial log (this routine deliberately never prints) while starving
       the rest of the system - 2026-07-10 the box dropped SSH the moment
       the adapter went live with the MCU up, with a totally silent trace.
       Print the first interrupt ever and then every 8192nd: a healthy
       adapter logs a handful of lines a day, a storm testifies at ~60
       lines/s. WinMaliLogPrint -> polled UART write is IRQL-safe and
       time-bounded, so a few ms at DIRQL per 8192 interrupts is an
       acceptable price for the evidence. */
    {
        LONG64 total = InterlockedIncrement64(&adapter->InterruptsTotal);
        if (total == 1 || (total & 0x1FFF) == 0) {
            WinMaliLogPrint("[WinMali] ISR: total=%lld gpu=0x%08x job=0x%08x mmu=0x%08x\n",
                            total, gpuRaw, jobRaw, mmuRaw);
        }
    }

    return handled;
}

VOID APIENTRY
WinMaliKmdDpcRoutine(_In_ CONST PVOID MiniportDeviceContext)
{
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromContext(MiniportDeviceContext);
    KIRQL            oldIrql;

    /* Runs at DISPATCH_LEVEL via DxgkCbNotifyDpc / DxgkCbQueueDpc. Drains
       the synchronous fence-completion queue populated by SubmitCommand,
       calling DxgkCbNotifyInterrupt(DMA_COMPLETED) for each pending entry. */
    if (adapter == NULL) {
        return;
    }

    InterlockedExchange(&adapter->NotifyDpcPending, 0);

    if (adapter->DxgkInterface.DxgkCbNotifyInterrupt == NULL ||
        adapter->DxgkHandle == NULL) {
        return;
    }

    KeAcquireSpinLock(&adapter->FenceQueueLock, &oldIrql);
    while (adapter->FenceQueueHead != adapter->FenceQueueTail) {
        DXGKARGCB_NOTIFY_INTERRUPT_DATA notify;
        UINT    fenceId   = adapter->FenceQueue[adapter->FenceQueueHead].SubmissionFenceId;
        UINT    nodeOrd   = adapter->FenceQueue[adapter->FenceQueueHead].NodeOrdinal;
        UINT    engineOrd = adapter->FenceQueue[adapter->FenceQueueHead].EngineOrdinal;
        BOOLEAN isPreempt = adapter->FenceQueue[adapter->FenceQueueHead].IsPreempt;
        UINT    lastDone  = adapter->FenceQueue[adapter->FenceQueueHead].LastCompletedFenceId;
        adapter->FenceQueueHead = (adapter->FenceQueueHead + 1) % WINMALI_FENCE_QUEUE_DEPTH;
        KeReleaseSpinLock(&adapter->FenceQueueLock, oldIrql);

        RtlZeroMemory(&notify, sizeof(notify));
        if (isPreempt) {
            notify.InterruptType                        = DXGK_INTERRUPT_DMA_PREEMPTED;
            notify.DmaPreempted.PreemptionFenceId       = fenceId;
            notify.DmaPreempted.LastCompletedFenceId    = lastDone;
            notify.DmaPreempted.NodeOrdinal             = nodeOrd;
            notify.DmaPreempted.EngineOrdinal           = engineOrd;
            adapter->DxgkInterface.DxgkCbNotifyInterrupt(adapter->DxgkHandle, &notify);
            WINMALI_TRACE("DPC: notified DMA_PREEMPTED preemptFence=%u lastCompleted=%u node=%u",
                          fenceId, lastDone, nodeOrd);
        } else {
            notify.InterruptType                       = DXGK_INTERRUPT_DMA_COMPLETED;
            notify.DmaCompleted.SubmissionFenceId      = fenceId;
            notify.DmaCompleted.NodeOrdinal            = nodeOrd;
            notify.DmaCompleted.EngineOrdinal          = engineOrd;
            adapter->DxgkInterface.DxgkCbNotifyInterrupt(adapter->DxgkHandle, &notify);
            adapter->GpuCompletedFence = fenceId;
            /* Silent by default - see the note at SubmitCommandVirtual: this
               pair floods the ring and scrolls the boot evidence away. */
            if (g_WinMaliTraceSubmit != 0) {
                WINMALI_TRACE("DPC: notified DMA_COMPLETED fence=%u node=%u engine=%u",
                              fenceId, nodeOrd, engineOrd);
            }
        }

        KeAcquireSpinLock(&adapter->FenceQueueLock, &oldIrql);
    }
    KeReleaseSpinLock(&adapter->FenceQueueLock, oldIrql);

    /* Tell dxgk it can poll our QueryCurrentFence and process the
       completions we just announced. */
    if (adapter->DxgkInterface.DxgkCbNotifyDpc != NULL) {
        adapter->DxgkInterface.DxgkCbNotifyDpc(adapter->DxgkHandle);
    }
}

/*
 * ---- SYNTHESISED VSYNC ---------------------------------------------------
 *
 * dxgk arms CRTC_VSYNC and then waits for a notification to retire each flip.
 * We have no display interrupt, so this DPC stands in for one.
 *
 * ⚠ HONEST LIMITATION: this is a free-running timer at the panel's nominal
 * refresh. It is NOT locked to scanout and will drift. For v1 -- a static
 * firmware framebuffer that nothing is tearing against -- that is acceptable,
 * and it is the difference between a flip that retires and a desktop that
 * waits forever. It must be replaced by a real DISP vsync before anything
 * depends on vsync PHASE rather than merely on vsync HAPPENING.
 *
 * Runs at DISPATCH_LEVEL. DxgkCbNotifyInterrupt from a DPC is the pattern this
 * driver already uses for DMA_COMPLETED (WinMaliKmdDpcRoutine), so it is
 * established here, not invented.
 */
static VOID
WinMaliVsyncDpc(_In_ PKDPC Dpc, _In_opt_ PVOID Context,
                _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    PWINMALI_ADAPTER adapter = (PWINMALI_ADAPTER)Context;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA notify;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (adapter == NULL || adapter->Magic != WINMALI_ADAPTER_MAGIC) { return; }
    if (InterlockedCompareExchange(&adapter->VsyncEnabled, 1, 1) != 1) { return; }
    if (adapter->DxgkHandle == NULL ||
        adapter->DxgkInterface.DxgkCbNotifyInterrupt == NULL) {
        return;
    }

    RtlZeroMemory(&notify, sizeof(notify));

    /* ⛔★★★★★ A DISPLAY-ONLY ADAPTER MUST REPORT DISPLAYONLY_VSYNC.
     * d3dkmddi.h: the CrtcVsync arm carries VidPnTargetId + PhysicalAddress +
     * PhysicalAdapterMask ("Physical Address of displaying buffer" - a FLIP
     * RETIREMENT); the DisplayOnlyVsync arm (DXGK_INTERRUPT_DISPLAYONLY_VSYNC=5)
     * carries ONLY VidPnTargetId, precisely because a DOD has no flip to retire.
     * dxgk watches this: TdrDodVSyncDelay fails a display-only adapter that
     * stops reporting vsync, and it reports the failure through
     * DpiFdoReportDeviceFailure - the ONE IoInvalidateDeviceState site in
     * dxgkrnl that emits no ETW and no event log. The device then takes
     * IRP_MN_SURPRISE_REMOVAL + IRP_MN_REMOVE_DEVICE, which is exactly what
     * decoding ETW event 126 shows happening to us. */
    if (g_WinMaliDodMode != 0) {
        notify.InterruptType = DXGK_INTERRUPT_DISPLAYONLY_VSYNC;
        notify.DisplayOnlyVsync.VidPnTargetId = WINMALI_TARGET_ID;
        adapter->DxgkInterface.DxgkCbNotifyInterrupt(adapter->DxgkHandle, &notify);
        /* ⛔★★★★★ NotifyInterrupt only QUEUES it. DxgkCbNotifyDpc is what makes
         * dxgk PROCESS the queue - and without it dxgk sees NO vsync at all and
         * its display-only watchdog removes us (MEASURED: BltQueueWakeReason=3
         * on our LUID, then StopDev+Remove ~80 ms later). The DMA fence path in
         * WinMaliKmdDpcRoutine has always made both calls; this notifier was the
         * only one missing its partner. */
        if (adapter->DxgkInterface.DxgkCbNotifyDpc != NULL) {
            adapter->DxgkInterface.DxgkCbNotifyDpc(adapter->DxgkHandle);
        }
        (VOID)InterlockedIncrement64(&adapter->VsyncNotifyCount);
        return;
    }

    notify.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    notify.CrtcVsync.VidPnTargetId = WINMALI_TARGET_ID;
    /* ⛔ ECHO THE ADDRESS dxgk PROGRAMMED, VERBATIM. This is what retires the
       flip. It is an address in OUR SEGMENT (measured: 0x10000000, the aperture
       base) -- NOT WINMALI_FB_SCANOUT_PA, which is a CPU physical address for
       the firmware framebuffer. Substituting one for the other is a multi-GB
       error and the flip would never match. */
    notify.CrtcVsync.PhysicalAddress.QuadPart = adapter->LastFlipAddress;
    notify.CrtcVsync.PhysicalAdapterMask      = 0;

    adapter->DxgkInterface.DxgkCbNotifyInterrupt(adapter->DxgkHandle, &notify);
    /* Same pairing as above and as WinMaliKmdDpcRoutine: queue, then tell dxgk
       to drain the queue. A CRTC_VSYNC that is never processed cannot retire a
       flip either, so this was a latent defect on the render path too. */
    if (adapter->DxgkInterface.DxgkCbNotifyDpc != NULL) {
        adapter->DxgkInterface.DxgkCbNotifyDpc(adapter->DxgkHandle);
    }
    (VOID)InterlockedIncrement64(&adapter->VsyncNotifyCount);
    /* Only the first few: this fires at 30 Hz and would otherwise fill the
       512-entry fill-once log before anything interesting happened. Three
       timestamps are enough to prove vsync was LIVE in the fatal window - a
       zero here would mean the flip could never retire no matter what else
       is right. */
    {
        static LONG s_vsyncLogged = 0;
        if (InterlockedIncrement(&s_vsyncLogged) <= 3) {
            WinMaliVpLogOnly_(WINMALI_VPC_VSYNC_FIRE);
        }
    }

    if (adapter->DxgkInterface.DxgkCbNotifyDpc != NULL) {
        adapter->DxgkInterface.DxgkCbNotifyDpc(adapter->DxgkHandle);
    }

    /* Kick the present worker: something has been flipped and the source is
       visible, so the live scanout is stale. KeSetEvent is legal here (we are
       at DISPATCH_LEVEL); the ~21 MB copy itself is NOT, hence the worker. */
    if (adapter->PresentWorkerThread != NULL &&
        adapter->ScanoutVa != NULL &&
        InterlockedCompareExchange(&adapter->SourceVisible, 1, 1) == 1 &&
        InterlockedCompareExchange64(&adapter->FlipLatchCount, 0, 0) != 0) {
        KeSetEvent(&adapter->PresentWake, IO_NO_INCREMENT, FALSE);
    }
}

/*
 * V1 PRESENT WORKER.
 *
 * Copies the primary dxgk last flipped to us into the framebuffer the panel is
 * actually scanning out of. Runs at PASSIVE_LEVEL because the copy is ~21 MB.
 *
 * ADDRESS TRANSLATION, the part that is easy to get catastrophically wrong:
 *   pArgs->PrimaryAddress (latched as LastFlipAddress) is an address IN OUR
 *   SEGMENT, not a CPU physical address. Our segment is
 *   PopulatedFromSystemMemory backed by DmaSegmentVa, so the CPU address is
 *   DmaSegmentVa + (flipAddr - segment base). Measured segment base is the
 *   aperture base 0x10000000. Conflating this with WINMALI_FB_SCANOUT_PA is a
 *   multi-GB error, which is why every read is bounds-checked against
 *   DmaSegmentBytes and a failure is COUNTED (ScanoutBltSkipped) rather than
 *   silently producing garbage on the panel.
 */
static VOID
WinMaliPresentWorker_(_In_ PVOID Context)
{
    PWINMALI_ADAPTER a = (PWINMALI_ADAPTER)Context;

    /* ⛔★★★★★ HOLD OFF UNTIL BOOT HAS SETTLED.
     *
     * THE CRITICAL PATH: dwm can only be put on our UMD by an ARMED BOOT - it is
     * a PROTECTED process and cannot be restarted on a running system (taskkill,
     * Stop-Process in a job, a fully detached process and a session logoff ALL
     * left the pid unchanged, 2026-08-17). But every armed boot HANGS at the
     * logon spinner, showing the partial top-band paint first.
     *
     * The discriminator: arming at RUNTIME is fine - two full 100 s windows with
     * no hang - while arming at BOOT dies within seconds. The difference is what
     * else is happening: at boot, LogonUI, dwm init and our StartDevice all land
     * at once, and we immediately start pushing ~21 MB frames into the very
     * framebuffer the firmware/DSI is still using.
     *
     * So do nothing for the first WINMALI_PRESENT_SETTLE_MS after the worker
     * starts. If the boot hang disappears, early-boot contention IS the cause and
     * the fix is a proper readiness gate rather than a delay. If it still hangs,
     * this is refuted cheaply and the cause is elsewhere. */
    {
        LARGE_INTEGER settle;
        if (g_WinMaliPresentSettleMs != 0) {
            settle.QuadPart = -((LONGLONG)g_WinMaliPresentSettleMs * 10000LL);
            (VOID)KeDelayExecutionThread(KernelMode, FALSE, &settle);
        }
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                L"StatPresentSettleMs", (ULONG)g_WinMaliPresentSettleMs);
    }

    /* ⛔★★★★★★ CLAIM THE REGISTRY BUDGET FOR THE COUNTERS THAT MATTER.
     *
     * `Parameters` caps at 127 values; this driver publishes 164 DISTINCT names.
     * So 37 of them can never be CREATED - and which 37 is decided purely by the
     * order they are first written in. Every counter added recently landed late
     * in that order and therefore read <absent> forever, which is
     * indistinguishable from "the code never ran".
     *
     * That has produced FOUR wrong conclusions in one session: StatUvSrcAsked=0
     * (read as "AcquireBest never ran" when it had run 1674 times), StatUvProbe,
     * StatPowerStop, and StatUvMaxCov. Each time I reasoned about the driver from
     * a number the registry had silently refused to store.
     *
     * Creating them HERE, once, before the worker publishes anything else,
     * reserves their slots permanently: after any purge the first names rewritten
     * are these. The later writes then just update values that already exist.
     * ⇒ An <absent> among these now genuinely means the code did not run. */
    {
        static const PCWSTR reserve[] = {
            L"StatPresSrcHReal",L"StatPresDstHReal",L"StatApWithContent2",
            L"StatPresSrcRectTL",L"StatPresSrcRectBR",L"StatPresDstRectTL",
            L"StatPresDstRectBR",L"StatPresSubRectCnt",L"StatPresDmaSize",
            L"StatPresMultipass",L"StatPresColor",
            L"StatUmdPresentCtx2",
            L"StatPawmReg",    L"StatOpenByPawm", L"StatOpenCalls",  L"StatOpenResolved",
            L"StatOpenById",   L"StatOpenFailed",
            L"StatOpenFailPdMagic", L"StatOpenFailPdSize", L"StatPresentBlts",
            L"StatPresentCalls2", L"StatPresentNullCookie2",
            L"StatUvMaxCov",   L"StatUvQual",     L"StatUvHeldOver",L"StatUvCoverage",
            L"StatUvMaxChg",
            L"StatWinPitch",
            L"StatWinRows",
            L"StatSrcLastRow", L"StatSrcFirstRow",L"StatSrcRowsNZ", L"StatSrcPathMeas",
            L"StatSrcProbePitch", L"StatSweeps",
            L"StatLatchDrop",  L"StatCopyAbort",  L"StatCopyKB",    L"StatUvRevokeTo",
            L"StatSkipNoSrc",  L"StatPowerStop",  L"StatUvSrcAsked",L"StatUvSrcCands",
            L"StatUvSrcTooSmall", L"StatUvSrcUniform", L"StatUvSrcBo", L"StatUvSrcPid",
            L"StatUvSrcPages", L"Stat0x93Calls",  L"Stat0x93Ok",    L"StatPresentCalls",
            L"LiveFlipLatch",  L"StatScanoutWc",
            L"SyncTblInits",   L"SyncTblTears",
            L"FrameThumbW",    L"FrameThumbH",
            L"CandThumbBo",    L"CandThumbPid",  L"CandThumbSt",  L"CandThumbLen",
            L"PanelThumbW",    L"PanelThumbH",
            L"UvSlotsN",       L"StatUvEvictBig", L"StatUvEvictBusy", L"StatUvDropSmall",
            /* ⛔ NET ZERO NEW NAMES. This key is far past the point where a new
               value name is guaranteed a slot, and an unreserved name silently
               fails to appear - which has already produced four wrong
               conclusions. StatBoDedup/StatBoDeref are defined and published but
               have NO incrementer anywhere in the tree, so they are structurally
               0 forever; their slots are free. */
            L"StatUvPush",      L"StatUvPushBad",
            L"StatUvRevokeBig", L"StatUvRevokeAny", L"TraceAtRevokeBigBytes",
            L"TraceAtFirstTmoBytes",
            L"StatTilerOom",   L"StatTilerOomFail",
            /* Reserved here for the same reason as everything else in this list:
               this key is far past the point where a NEW name is guaranteed a
               slot, and an unreserved name silently fails to appear - which has
               already produced four wrong conclusions. */
            L"StatVisOnPid",   L"StatVisOffPid", L"StatVisOnName", L"StatVisOffName",
            /* StatBoDedup / StatBoDeref removed: no incrementer exists anywhere
               in the tree, so they published a constant 0 while holding two slots
               on a key that has none to spare. Their slots now carry StatUvPush /
               StatUvPushBad above. */
            L"StatUvPubBig",
            L"StatWorkerTicks", L"StatWorkerStage",
            L"StatStripBo",     L"StatStripRows",  L"StatStripBlits", L"StatStripNB",
            L"StatFbAllocs",    L"StatFbAllocLastW", L"StatFbAllocLastH", L"StatFbAllocLastPitch",
            L"StatMpoCapsCalls", L"StatMpoCheckCalls", L"StatMpoCheckPlanes", L"StatMpoSetCalls",
            L"StatMpoPostCompCaps", L"StatMpoPostPresent", L"StatShaderPwrOn",
            L"StatPresAl0Lo", L"StatPresAl0Hi", L"StatPresAl1Lo",
            L"StatPresAl0NonNull", L"StatPresAl1NonNull",
            L"StatVmCreates", L"StatVmDestroys", L"StatVmLive", L"StatVmCreateFail",
            /* StatDevCreates/StatDevDestroys/StatVmOwnerNull retired: they answered
               their question (DestroyDevice DOES run; no VM has a NULL owner) and
               the name table is a hard 127 slots. */
            L"StatFencePair",
            L"StatVmDeferredBind", L"StatAsEvictions", L"StatAsLateBinds",
            L"StatAsEnsureFail",
            L"StatVpTrail", L"StatVpIsSup", L"StatVpCommit", L"StatVpVis",
            L"StatVpVisMs",
            L"StatPresentMpoFlag", L"StatPresentRedirected", L"QaiMpoOff", L"QaiMpoPlanesOff",
            L"StatDmaSegBytes",
            L"StatPresentFlips", L"StatPresentLastFlags",
            L"StatUvC0Box",    L"StatUvC1Box",   L"StatUvC2Box",  L"StatUvC3Box",
        };
        ULONG ri;
        for (ri = 0; ri < RTL_NUMBER_OF(reserve); ri++) {
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, reserve[ri], 0);
        }
    }

    for (;;) {
        /* SELF-TICK: see the PresentTickMs note. A NULL timeout here parked the
           worker forever in the BLT model, because the only signaller requires a
           latched flip. PresentWake still cuts the wait short, so a real flip is
           serviced with no added latency. */
        LARGE_INTEGER tick;
        PLARGE_INTEGER pTick = NULL;
        if (g_WinMaliPresentTickMs != 0) {
            tick.QuadPart = -((LONGLONG)g_WinMaliPresentTickMs * 10000LL);
            pTick = &tick;
        }
        (VOID)KeWaitForSingleObject(&a->PresentWake, Executive, KernelMode,
                                    FALSE, pTick);
        if (InterlockedCompareExchange(&a->PresentStop, 0, 0) != 0) {
            break;
        }
        /* Heartbeat: incremented before ANY early-out below, so a climbing value
           means the thread is running even on ticks that paint nothing. */
        (VOID)InterlockedIncrement(&g_WinMaliStatWorkerTicks);
        (VOID)InterlockedExchange(&g_WinMaliStatWorkerStage, 1);

        /* ⛔⛔⛔ THE WORKER SUPPRESSION LIVED HERE AND IT HUNG THE BOX (1.1.11.0).
         * A `continue` at this point is ABOVE the s_pendingUse use-ref sweep, so
         * it leaks a use-ref; WinMaliUvSrcRevoke then blocks for its FULL timeout
         * on EVERY BO teardown, and dwm tears one down 12x/s => total stall, black
         * screen. It also sits above the counter-publish block, so every counter
         * freezes at once and the failure cannot even be diagnosed.
         * The design that specified it said "immediately after the s_pendingUse
         * sweep, NEVER before it". Deliberately shipping the push WITHOUT any
         * suppression until it is proven; the cost is stale repaints over each
         * pushed frame (flicker), which is a cosmetic problem, not a hang. */

        /* ---- ASSERT "MONITOR CONNECTED" -----------------------------------
         * 🎯 dxgk zeroes our display plane when an internal lookup returns
         * STATUS_GRAPHICS_MONITOR_NOT_CONNECTED (0xC01E0338). The deciding
         * instruction in that lookup is `ldrb w8,[target+0x2F8] ; cbnz w8 ->
         * success` - a per-target "monitor connected" byte that reads ZERO for
         * us. This callback is what sets it.
         * ⛔ Connect ONLY - no disconnect first. The old hot-replug below sends
         * disconnect->reconnect to force re-enumeration and was already refuted;
         * this is a single positive assertion at a byte we can now name.
         * Same placement rules as the replug: PASSIVE worker, once per boot,
         * ahead of every early-out so a skipped frame cannot skip it. */
        if (g_WinMaliIndicateConnMs != 0) {
            static ULONG s_indState = 0;
            if (s_indState == 0 &&
                a->DxgkInterface.DxgkCbIndicateChildStatus != NULL &&
                WinMaliMsSinceAdapterStart_() >= g_WinMaliIndicateConnMs) {
                DXGK_CHILD_STATUS cs;
                NTSTATUS rs;
                RtlZeroMemory(&cs, sizeof(cs));
                cs.Type              = StatusConnection;
                cs.ChildUid          = WINMALI_CHILD_UID;
                cs.HotPlug.Connected = TRUE;
                rs = a->DxgkInterface.DxgkCbIndicateChildStatus(a->DxgkHandle, &cs);
                s_indState = 1;
                DbgPrint("[WinMali]    INDICATE CONNECTED -> 0x%08X (at %lu ms)\n",
                         (unsigned)rs, (unsigned long)WinMaliMsSinceAdapterStart_());
                if (g_WinMaliRegPath.Length != 0) {
                    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                            L"StatIndConnRs", (ULONG)rs);
                }
            }
        }

        /* ---- HOT RE-PLUG: make the OS rebuild the display configuration ----
         * Runs on the worker thread (PASSIVE_LEVEL; the callback allows up to
         * DISPATCH_LEVEL) and exactly ONCE per boot. Placed immediately after the
         * heartbeat and before every early-out, so a skipped frame cannot skip it
         * - the same rule the counters above are written under. */
        if (g_WinMaliHotReplugMs != 0) {
            static ULONG s_replugState = 0;
            static ULONG s_replugAtMs  = 0;
            if (s_replugState < 2 &&
                a->DxgkInterface.DxgkCbIndicateChildStatus != NULL) {
                ULONG nowMs = WinMaliMsSinceAdapterStart_();
                if (s_replugState == 0 && nowMs >= g_WinMaliHotReplugMs) {
                    DXGK_CHILD_STATUS cs;
                    NTSTATUS rs;
                    RtlZeroMemory(&cs, sizeof(cs));
                    cs.Type = StatusConnection;
                    cs.ChildUid = WINMALI_CHILD_UID;
                    cs.HotPlug.Connected = FALSE;
                    rs = a->DxgkInterface.DxgkCbIndicateChildStatus(
                             a->DxgkHandle, &cs);
                    WinMaliVpLogOnly_(WINMALI_VPC_REPLUG | (1u << 16) |
                                      ((ULONG)rs & 0xFFFFu));
                    DbgPrint("[WinMali]    HOT REPLUG: disconnect -> 0x%08X\n",
                             (unsigned)rs);
                    s_replugAtMs = nowMs;
                    s_replugState = 1;
                } else if (s_replugState == 1 && nowMs >= s_replugAtMs + 200u) {
                    DXGK_CHILD_STATUS cs;
                    NTSTATUS rs;
                    RtlZeroMemory(&cs, sizeof(cs));
                    cs.Type = StatusConnection;
                    cs.ChildUid = WINMALI_CHILD_UID;
                    cs.HotPlug.Connected = TRUE;
                    rs = a->DxgkInterface.DxgkCbIndicateChildStatus(
                             a->DxgkHandle, &cs);
                    WinMaliVpLogOnly_(WINMALI_VPC_REPLUG | (2u << 16) |
                                      ((ULONG)rs & 0xFFFFu));
                    DbgPrint("[WinMali]    HOT REPLUG: reconnect  -> 0x%08X\n",
                             (unsigned)rs);
                    s_replugState = 2;
                }
            }
        }

        /* ★ PUBLISH BEFORE ANY `continue` — THE INSTRUMENT MUST NOT SIT BEHIND
         * THE CONDITION IT MEASURES.
         *
         * The previous attempt put these writes inside the pixel sampler, which
         * is gated on (s_srcTick % 120)==1 AND lives after several early-outs
         * (ScanoutVa NULL, flipAddr < segBase, offset out of range, srcVa NULL
         * -> ScanoutBltSkipped++ ; continue). Every one of those paths is
         * silent, so when the worker started taking one, ALL counters read
         * <absent> and there was no way to tell a broken publish from a broken
         * present path. Both instruments were blind at once - the EDK2 ring is
         * fill-once and was already full.
         *
         * Here it runs unconditionally at the top of every tick, so the numbers
         * exist even when the frame is skipped. ScanoutBlits vs ScanoutSkipped
         * then says WHICH path is being taken, which is the thing that was
         * impossible to see. PASSIVE_LEVEL (system thread), so the registry
         * write is legal; every 120th tick keeps it to ~4 s at 30 Hz. */
        {
            static LONG64 s_pubTick = 0;
            if ((InterlockedIncrement64(&s_pubTick) % 120) == 1) {
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentCalls",      (ULONG)g_WinMaliStatPresentCalls);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentNotBlt",     (ULONG)g_WinMaliStatPresentNotBlt);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatApAdopted",         (ULONG)g_WinMaliStatApAdopted);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatApWithContent",     (ULONG)g_WinMaliStatApWithContent);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatApBestIdx",         (ULONG)g_WinMaliStatApFirstIdx);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"Stat0x92Calls",         (ULONG)g_WinMaliStat0x92Calls);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"Stat0x92NoEnt",         (ULONG)g_WinMaliStat0x92NoEnt);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"Stat0x92NotRes",        (ULONG)g_WinMaliStat0x92NotRes);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"Stat0x92Ok",            (ULONG)g_WinMaliStat0x92Ok);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"Stat0x93Calls",         (ULONG)g_WinMaliStat0x93Calls);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"Stat0x93Ok",            (ULONG)g_WinMaliStat0x93Ok);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"Stat0x93Fail",          (ULONG)g_WinMaliStat0x93Fail);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvSrcBo",           (ULONG)g_WinMaliStatUvSrcBo);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvSrcHits",         (ULONG)g_WinMaliStatUvSrcHits);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatWorkerTicks",       (ULONG)g_WinMaliStatWorkerTicks);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatStripBo",           (ULONG)g_WinMaliStatStripBo);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatStripRows",         (ULONG)g_WinMaliStatStripRows);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatStripBlits",        (ULONG)g_WinMaliStatStripBlits);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatStripNB",           (ULONG)g_WinMaliStatStripNB);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatFbAllocs",          (ULONG)g_WinMaliStatFbAllocs);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatFbAllocLastW",      (ULONG)g_WinMaliStatFbAllocLastW);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatFbAllocLastH",      (ULONG)g_WinMaliStatFbAllocLastH);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatFbAllocLastPitch",  (ULONG)g_WinMaliStatFbAllocLastPitch);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatWorkerStage",       (ULONG)g_WinMaliStatWorkerStage);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvSrcAsked",        (ULONG)g_WinMaliStatUvSrcAsked);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvSrcCands",        (ULONG)g_WinMaliStatUvSrcCands);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvSrcTooSmall",     (ULONG)g_WinMaliStatUvSrcTooSmall);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvSrcPid",          (ULONG)g_WinMaliStatUvSrcPid);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvSrcPages",        (ULONG)g_WinMaliStatUvSrcPages);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvSrcNCand",        (ULONG)g_WinMaliStatUvSrcNCand);
                /* ★ THE NEW SELECTION SIGNAL. Published in the PERIODIC block,
                   not the copy block, so it is still readable on a tick where no
                   source qualified - which is precisely when it matters. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvCoverage",        (ULONG)g_WinMaliStatUvSrcCoverage);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvMaxCov",          (ULONG)g_WinMaliStatUvSrcMaxCov);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvMaxChg",          (ULONG)g_WinMaliStatUvSrcMaxChg);
                    /* ⛔ PUBLISH THE RUNNERS-UP, NOT JUST THE WINNER.
                     *
                     * The capture site in WinMaliBo.c already says why: "full
                     * desktop, no taskbar, no cursor" is the signature of
                     * scanning out a PRE-COMPOSITION layer, and only the field
                     * of candidates can tell that from "the composed frame is
                     * all we have". The arrays were filled and then never
                     * published, so the question stayed unanswerable.
                     *
                     * MEASURED 2026-08-18 (1.1.3.5): winner coverage 94 with
                     * chg=0, while some candidate had chg=10 - i.e. NOTHING on
                     * offer is both full-screen and live. Whether dwm's real
                     * composition target is simply absent from this list is the
                     * next thing to establish, and this is how. */
                    {
                        ULONG ci;
                        for (ci = 0; ci < RTL_NUMBER_OF(g_WinMaliUvCandBo); ci++) {
                            WCHAR nm[24];
                            (VOID)RtlStringCchPrintfW(nm, RTL_NUMBER_OF(nm),
                                                      L"StatUvC%uBo", ci);
                            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, nm,
                                    (ULONG)g_WinMaliUvCandBo[ci]);
                            (VOID)RtlStringCchPrintfW(nm, RTL_NUMBER_OF(nm),
                                                      L"StatUvC%uPid", ci);
                            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, nm,
                                    (ULONG)g_WinMaliUvCandPid[ci]);
                            (VOID)RtlStringCchPrintfW(nm, RTL_NUMBER_OF(nm),
                                                      L"StatUvC%uChg", ci);
                            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, nm,
                                    (ULONG)g_WinMaliUvCandChg[ci]);
                            (VOID)RtlStringCchPrintfW(nm, RTL_NUMBER_OF(nm),
                                                      L"StatUvC%uNB", ci);
                            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, nm,
                                    (ULONG)g_WinMaliUvCandNonBlack[ci]);
                            /* Packed painted-content bbox: fr|lr|fc|lc, one
                               byte each (rows/8, cols/16). nonBlack cannot
                               separate a small painted rectangle from sparse
                               content over the whole screen; this can. */
                            (VOID)RtlStringCchPrintfW(nm, RTL_NUMBER_OF(nm),
                                                      L"StatUvC%uBox", ci);
                            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, nm,
                                    (ULONG)g_WinMaliUvCandBox[ci]);
                        }
                    }
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvQual",            (ULONG)g_WinMaliStatUvSrcQual);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvEvictBig",  (ULONG)g_WinMaliStatUvSrcEvictBig);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvEvictBusy", (ULONG)g_WinMaliStatUvSrcEvictBusy);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvDropSmall", (ULONG)g_WinMaliStatUvSrcDropSmall);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvRevokeBig", (ULONG)g_WinMaliStatUvRevokeBig);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvRevokeAny", (ULONG)g_WinMaliStatUvRevokeAny);
                /* StatBoDedup / StatBoDeref publishes removed with their
                   reservations: constant 0, no incrementer in the tree. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvPubBig",    (ULONG)g_WinMaliStatUvPubBig);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvHeldOver",        (ULONG)g_WinMaliStatUvHeldOver);
                /* ★ Did our DRIVERCAPS land where dxgk reads them? Two cap
                   experiments have now changed nothing, and this file's own
                   comment records dxgk reading 0x8 from a field we set to 0.
                   QueryCapsOutSz < QueryCapsSizeof means the running kernel has
                   a SMALLER DXGK_DRIVERCAPS than our 26100 WDK header, so our
                   FIELD_OFFSETs address the wrong bytes and BOTH refutations
                   are no-ops rather than evidence. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QueryFlipCaps",         g_WinMaliQaiFlipCapsValue);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QaiFlipQueueDepth",     (ULONG)g_WinMaliQaiFlipQueueDepth);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatStdAllocPrimary",   (ULONG)g_WinMaliStatStdAllocPrimary);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatStdAllocShadow",    (ULONG)g_WinMaliStatStdAllocShadow);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatStdAllocGdi",       (ULONG)g_WinMaliStatStdAllocGdi);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QueryCapsOff",          g_WinMaliQaiFlipCapsOff);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QueryCapsOutSz",        g_WinMaliQaiOutSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QaiMpoOff",             g_WinMaliQaiMpoOff);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QaiMpoPlanesOff",       g_WinMaliQaiMpoPlanesOff);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatMpoCapsCalls",      (ULONG)g_WinMaliStatMpoCapsCalls);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatMpoCheckCalls",     (ULONG)g_WinMaliStatMpoCheckCalls);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatMpoCheckPlanes",    (ULONG)g_WinMaliStatMpoCheckPlanes);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatMpoSetCalls",       (ULONG)g_WinMaliStatMpoSetCalls);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatMpoPostCompCaps",   (ULONG)g_WinMaliStatMpoPostCompCaps);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatMpoPostPresent",    (ULONG)g_WinMaliStatMpoPostPresent);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatShaderPwrOn",       (ULONG)g_WinMaliStatShaderPwrOn);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresAl0Lo",         (ULONG)g_WinMaliStatPresAl0Lo);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresAl0Hi",         (ULONG)g_WinMaliStatPresAl0Hi);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresAl1Lo",         (ULONG)g_WinMaliStatPresAl1Lo);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresAl0NonNull",    (ULONG)g_WinMaliStatPresAl0NonNull);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresAl1NonNull",    (ULONG)g_WinMaliStatPresAl1NonNull);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatVmCreates",         (ULONG)g_WinMaliStatVmCreates);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatVmDestroys",        (ULONG)g_WinMaliStatVmDestroys);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatVmLive",            (ULONG)g_WinMaliStatVmLive);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatVmCreateFail",      (ULONG)g_WinMaliStatVmCreateFail);
                /* ⛔ THE BLT MODEL RETIRES A PRESENT ON DMA COMPLETION, NOT ON VSYNC.
                   Flips==0 here, so a healthy vsync stream proves nothing about
                   whether presents are completing. If Submitted runs ahead of
                   Completed and stays there, dwm is blocked waiting for a buffer
                   we never released - which is exactly the shape of "24 presents
                   then silence forever". Equal values clear the whole theory in
                   one read. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatFencePair",
                        (((ULONG)a->GpuSubmittedFence & 0xFFFFu) << 16)
                        | ((ULONG)a->GpuCompletedFence & 0xFFFFu));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatVmDeferredBind",    (ULONG)g_WinMaliStatVmDeferredBind);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatAsEvictions",       (ULONG)g_WinMaliStatAsEvictions);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatAsLateBinds",       (ULONG)g_WinMaliStatAsLateBinds);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatAsEnsureFail",      (ULONG)g_WinMaliStatAsEnsureFail);
                /* Order, not just counts: see the StatVpTrail comment in
                   WinMaliVidPn.c. Read StatVpTrail as hex, oldest nibble left. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatVpTrail",  (ULONG)g_WinMaliStatVpTrail);
                /* WHO turned the source on, and who turned it off. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatVisOnPid",
                        (ULONG)InterlockedCompareExchange(&g_WinMaliVisOnPid, 0, 0));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatVisOffPid",
                        (ULONG)InterlockedCompareExchange(&g_WinMaliVisOffPid, 0, 0));
                (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"StatVisOnName",
                        g_WinMaliVisOnName, sizeof(g_WinMaliVisOnName));
                (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"StatVisOffName",
                        g_WinMaliVisOffName, sizeof(g_WinMaliVisOffName));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvPush",    (ULONG)g_WinMaliStatUvPush);
                {   /* refG | refP<<8 | refD<<16 | EXC<<24, each saturating.
                       ⛔ BITS 24-31 MUST READ 0. The __except can only fire if
                       the lifetime model is wrong; anything else there is a STOP. */
                    ULONG gG = (ULONG)g_WinMaliStatUvPushRefG; if (gG > 255u) gG = 255u;
                    ULONG gP = (ULONG)g_WinMaliStatUvPushRefP; if (gP > 255u) gP = 255u;
                    /* ⛔ BITS 16-23 ARE NOW THE NON-BLACK SAMPLE COUNT (0-64), not
                       refD: refD has been 0 on every single run, and this key has
                       no spare value names. Blank-frame refusals fold into refG. */
                    ULONG gD = (ULONG)g_WinMaliStatUvPushNB;  if (gD > 255u) gD = 255u;
                    ULONG gE = (ULONG)g_WinMaliStatUvPushExc;  if (gE > 255u) gE = 255u;
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatUvPushBad", gG | (gP << 8) | (gD << 16) | (gE << 24));
                }
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatVpIsSup",
                        (((ULONG)g_WinMaliStatVpIsSupYes & 0xFFFFu) << 16)
                        | ((ULONG)g_WinMaliStatVpIsSupNo & 0xFFFFu));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatVpCommit",
                        (((ULONG)g_WinMaliStatVpCommitPath & 0xFFFFu) << 16)
                        | ((ULONG)g_WinMaliStatVpCommitEmpty & 0xFFFFu));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatVpVis",
                        (((ULONG)g_WinMaliStatVpVisOn & 0xFFFFu) << 16)
                        | ((ULONG)g_WinMaliStatVpVisOff & 0xFFFFu));
                /* When, in ms since StartDevice, visibility last went on and
                   last went off. Boot-relative and monotonic: KeQueryInterruptTime
                   is immune to this box's RTC resetting every power cycle. The
                   gap between them is the whole question - a switch-off seconds
                   after logon and one that lands when the shell finishes
                   starting have completely different causes. */
                /* ⚠ HUNDREDTHS, NOT MILLISECONDS. Both halves are /100 to fit a
                   16-bit field, so a raw read is 100x too small - I decoded this
                   as "13.9 s" once and built a whole theory on a number that was
                   really 23 minutes. Multiply by 100 before believing it. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatVpVisMs",
                        (((ULONG)g_WinMaliStatVpVisOnMs / 100u) << 16)
                        | (((ULONG)g_WinMaliStatVpVisOffMs / 100u) & 0xFFFFu));
                /* The ordered, TIMESTAMPED version of StatVpTrail. Fill-once, so
                   it still holds the boot sequence an hour later. */
                {
                    LONG  vn  = InterlockedCompareExchange(&g_WinMaliVpLogN, 0, 0);
                    ULONG cnt = (vn <= 0) ? 0u
                              : (((ULONG)vn > WINMALI_VPLOG_MAX) ? WINMALI_VPLOG_MAX
                                                                 : (ULONG)vn);
                    static ULONG s_vpPublished = 0;
                    if (cnt != s_vpPublished) {
                        s_vpPublished = cnt;
                        (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"VpLog",
                                g_WinMaliVpLog, cnt * sizeof(WINMALI_VPLOG_ENT));
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"VpLogN",
                                (ULONG)vn);
                    }
                }
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentMpoFlag",    (ULONG)g_WinMaliStatPresentMpoFlag);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentRedirected", (ULONG)g_WinMaliStatPresentRedirected);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QueryCapsSizeof",       g_WinMaliQaiCapsSizeof);
                /* ★ The segment flags we actually sent. DirectFlip = 0x400 is the
                   per-segment permission for an allocation to be a flip target;
                   we shipped it as 0 until SegmentShape was added. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QuerySegFlags",         g_WinMaliQaiSegFlags);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QuerySegShape",         g_WinMaliSegmentShape);

                /* ★★★★★ PUBLISH THE DRIVER'S OWN TRACE.
                 *
                 * Every DbgPrint here is redirected into the EDK2 log ring
                 * (WinMaliLog.h -> WinMaliSerial.c), and until now NOTHING could
                 * read it back: mtkcpufreq's BootLog is a one-shot snapshot from
                 * its own load time (measured: 25 WinMali lines, ending mid-QAI,
                 * byte-identical across reads) and mtkcpufreq cannot be
                 * restarted. So the driver has been narrating to a void, and
                 * every number we wanted got re-published as its own DWORD -
                 * including QuerySegFlags, which duplicates a value
                 * QAI[QUERYSEGMENT3 fill] was already printing.
                 *
                 * Throttled hard: this copies 24 KB and hits the registry, so
                 * once every 64 publish ticks (~2 s at 30 Hz), not per frame. */
                {
                    static PUCHAR s_traceBuf   = NULL;
                    static ULONG  s_traceTick  = 0;
                    /* ⛔★★★★★ 24 KB IS TOO SMALL TO CATCH AN EARLY-BOOT BURST.
                     *
                     * MEASURED 2026-08-18: dwm's 79 syncobj-SIGNAL ENOENT
                     * failures all happen ~57 s after boot and then STOP
                     * (SignalFail freezes at 79 while CsOk keeps climbing).
                     * WinRM only becomes reachable ~70 s after boot, so by the
                     * time anything can read TraceTail the whole burst has
                     * already scrolled out of a 24 KB window - the ring holds
                     * roughly 280 dense lines, i.e. SECONDS of traffic.
                     * Every attempt to capture WMSIGENOENT1 came back empty for
                     * exactly this reason, not because the code never ran.
                     *
                     * The underlying EDK2 ring is 256 KB; only this published
                     * SLICE was small. 192 KB covers the whole logon window and
                     * still fits comfortably in a registry binary value.
                     * ⚠ Costs a 192 KB pool allocation + a registry write every
                     * 64 ticks (~2 s). Acceptable for a bring-up driver; revisit
                     * if it ever ships. */
                    const  ULONG  kTraceBytes  = 196608u;

                    if ((s_traceTick++ % 64u) == 0u) {
                        if (s_traceBuf == NULL) {
                            s_traceBuf = (PUCHAR)ExAllocatePoolWithTag(
                                NonPagedPoolNx, kTraceBytes, 'gLmW');
                        }
                        if (s_traceBuf != NULL) {
                            ULONG got = WinMaliSerialSnapshot(s_traceBuf, kTraceBytes);
                            if (got != 0) {
                                (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath,
                                        L"TraceTail", s_traceBuf, got);
                                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                        L"TraceTailBytes", got);
                            }
                        }
                    }

                    /* ⛔★★★★★ THE TRIGGERED SNAPSHOT - CHECKED EVERY TICK, NOT
                     * EVERY 64. The escape latches g_WinMaliTrapSignalFail 0->1
                     * on the FIRST syncobj-SIGNAL ENOENT. Grab the ring NOW,
                     * into a SEPARATE value so the timer-published TraceTail
                     * cannot overwrite it, and latch to 2 so we capture the
                     * first failure only - later traffic must not dilute it.
                     *
                     * This exists because a timer-published tail provably cannot
                     * catch this burst: it lands ~57 s after boot, WinRM arrives
                     * ~70 s in, and the ring is only 256 KB. Four capture
                     * attempts came back with 0 hits for exactly that reason. */
                    /* Same shape, different trigger: the FIRST full-screen
                       scanout slot to be revoked. That is the event that empties
                       dwm's flip chain from the table, and its cause line sits a
                       few hundred lines above it in this same ring. */
                    /* ⛔★★★★★ THIS TRAP WAS DEAD FOR ITS ENTIRE LIFE - AN
                     * OFF-BY-ONE BETWEEN THE LATCH AND THE PUBLISHER.
                     *
                     * WinMaliBo.c:804 latches  0 -> 1.
                     * This block used to require state 2 ("captured"), which was
                     * set by a ring capture AT the revoke site - and that capture
                     * was REVERTED (it ran a 192 KB snapshot + pool allocation
                     * under the UvSrc mutex, on the BoFree path). Nothing was left
                     * to move 1 -> 2, so the publish could never fire.
                     *
                     * MEASURED 2026-08-20: StatUvRevokeBig = 4 while
                     * TraceAtRevokeBigBytes = 0. Four full-screen scanout slots
                     * were revoked and the instrument built to explain WHY
                     * published nothing, on every boot, silently.
                     *
                     * Now it matches the SignalFail trap that DOES work: latch
                     * 1 -> 2 here and take the snapshot HERE, at PASSIVE level and
                     * outside the UvSrc mutex. The cause line sits a few hundred
                     * lines above the revoke in this same ring, so a snapshot one
                     * tick later (~33 ms) still contains it.
                     * ★ A TRAP WHOSE TWO HALVES DISAGREE ABOUT THE STATE NUMBER IS
                     *   INDISTINGUISHABLE FROM "THE EVENT NEVER HAPPENED". */
                    if (InterlockedCompareExchange(&g_WinMaliTrapRevokeBig, 2, 1) == 1) {
                        if (s_traceBuf == NULL) {
                            s_traceBuf = (PUCHAR)ExAllocatePoolWithTag(
                                NonPagedPoolNx, kTraceBytes, 'gLmW');
                        }
                        if (s_traceBuf != NULL) {
                            ULONG got = WinMaliSerialSnapshot(s_traceBuf, kTraceBytes);
                            if (got != 0) {
                                (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath,
                                        L"TraceAtRevokeBig", s_traceBuf, got);
                                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                        L"TraceAtRevokeBigBytes", got);
                            }
                        }
                    }
                    /* Third trigger, same discipline: the FIRST CSF job wait
                       timeout (WinMaliFw.c latches it 0->1). Its own value name
                       so neither the 64-tick TraceTail nor the other two traps
                       can overwrite the one snapshot that spans the transition
                       from a working GPU to a wedged one. */
                    if (InterlockedCompareExchange(&g_WinMaliTrapFirstTmo, 2, 1) == 1) {
                        if (s_traceBuf == NULL) {
                            s_traceBuf = (PUCHAR)ExAllocatePoolWithTag(
                                NonPagedPoolNx, kTraceBytes, 'gLmW');
                        }
                        if (s_traceBuf != NULL) {
                            ULONG got = WinMaliSerialSnapshot(s_traceBuf, kTraceBytes);
                            if (got != 0) {
                                (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath,
                                        L"TraceAtFirstTmo", s_traceBuf, got);
                                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                        L"TraceAtFirstTmoBytes", got);
                            }
                        }
                    }
                    if (InterlockedCompareExchange(&g_WinMaliTrapSignalFail, 2, 1) == 1) {
                        if (s_traceBuf == NULL) {
                            s_traceBuf = (PUCHAR)ExAllocatePoolWithTag(
                                NonPagedPoolNx, kTraceBytes, 'gLmW');
                        }
                        if (s_traceBuf != NULL) {
                            ULONG got = WinMaliSerialSnapshot(s_traceBuf, kTraceBytes);
                            if (got != 0) {
                                (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath,
                                        L"TraceAtSignalFail", s_traceBuf, got);
                                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                        L"TraceAtSignalFailBytes", got);
                            }
                        }
                    }
                }
                {
                    /* One row per qualifying candidate: bo / pid / hits / pages.
                       Named individually so a plain registry read decodes them
                       without any packing convention to get wrong later. */
                    ULONG ci = 0;

                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatUvSrcUniform", (ULONG)g_WinMaliStatUvSrcUniform);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatUvSrcChg", (ULONG)g_WinMaliStatUvSrcChg);
                    /* Why frames are being rejected at submit. */
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatSubmitNoGroup", (ULONG)g_WinMaliStatSubmitNoGroup);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatSubmitFatalGrp", (ULONG)g_WinMaliStatSubmitFatalGroup);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsfFailCount",  (ULONG)g_WinMaliStatCsfFailCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatSubmitSyncFail", (ULONG)g_WinMaliStatSubmitSyncFail);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatSubmitSyncErr",  (ULONG)g_WinMaliStatSubmitSyncErr);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"SyncBadHandle", (ULONG)g_WinMaliStatSyncBadHandle);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"SyncBadFlags",  (ULONG)g_WinMaliStatSyncBadFlags);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"SyncWaitTmo",   (ULONG)g_WinMaliStatSyncWaitTimeout);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"SignalFail",    (ULONG)g_WinMaliStatSubmitSignalFail);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"SyncTblInits",  (ULONG)g_WinMaliStatSyncTableInits);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"SyncTblTears",  (ULONG)g_WinMaliStatSyncTableTeardowns);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"SignalErr",     (ULONG)g_WinMaliStatSignalErr);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"SignalBadHnd",  (ULONG)g_WinMaliStatSignalBadHandle);
                    /* The CSF timeout diagnosis - CMD_PTR says WHERE the command
                       stream parked, BLOCKED_REASON says WHY, CsFault/CsFatal
                       cover the command-stream-only errors that leave the MMU
                       and GPU banks reading zero. */
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsTimeouts",   (ULONG)g_WinMaliStatCsTimeoutCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsCmdPtrLo",   (ULONG)g_WinMaliStatCsCmdPtrLo);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsCmdPtrHi",   (ULONG)g_WinMaliStatCsCmdPtrHi);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsBlocked",    (ULONG)g_WinMaliStatCsBlocked);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsFault",      (ULONG)g_WinMaliStatCsFault);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsFatal",      (ULONG)g_WinMaliStatCsFatal);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsCalleeRet",  (ULONG)g_WinMaliStatCsCalleeRet);
                    /* What dxgk actually scans out, vs the surface our heuristic
                       picked. If FlipW/H match the panel and FlipAllocId differs
                       from the winner BO's alloc id, the heuristic is on the
                       wrong buffer and that is why there is no taskbar. */
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"ApMapOps",     (ULONG)g_WinMaliApMapOps);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"ApMapMaxSize", (ULONG)g_WinMaliApMapMaxSize);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"ApMapLastSize",(ULONG)g_WinMaliApMapLastSize);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipAllocW",   (ULONG)g_WinMaliFlipAllocW);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipAllocH",   (ULONG)g_WinMaliFlipAllocH);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipAllocPitch",(ULONG)g_WinMaliFlipAllocPitch);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipAllocFmt", (ULONG)g_WinMaliFlipAllocFmt);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipAllocSize",(ULONG)g_WinMaliFlipAllocSize);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipAllocId",  (ULONG)g_WinMaliFlipAllocId);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipAllocOk",  (ULONG)g_WinMaliFlipAllocOk);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipAllocBad", (ULONG)g_WinMaliFlipAllocBad);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsSubmits",    (ULONG)g_WinMaliStatCsSubmitCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsOk",         (ULONG)g_WinMaliStatCsOkCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatTilerOom",     (ULONG)g_WinMaliStatTilerOomCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatTilerOomFail", (ULONG)g_WinMaliStatTilerOomFail);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"RingInsert",   (ULONG)g_WinMaliStatRingInsert);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"RingExtract",  (ULONG)g_WinMaliStatRingExtract);
                    /* ★ THE SCANOUT ALLOCATION, SAMPLED FROM THE WORKER.
                     * At flip time this always read NULL - flips happen at
                     * mode-set and residency lands later, so the sample was
                     * simply taken too early (dxgk then mapped that very
                     * allocation: ApMapMaxSize == its exact size). The worker
                     * runs ~30x a second and sees the steady state. Non-zero
                     * here means the COMPOSED frame - taskbar and cursor
                     * included - is finally reachable. */
                    {
                        PWINMALI_KMD_ALLOCATION fa =
                            (PWINMALI_KMD_ALLOCATION)InterlockedCompareExchangePointer(
                                (PVOID*)&g_WinMaliFlipAlloc, NULL, NULL);
                        if (fa != NULL && fa->Magic == WINMALI_KMD_ALLOC_MAGIC) {
                            (VOID)InterlockedExchange(&g_WinMaliFlipWorkerMdl,
                                (LONG)(fa->ApertureMdl != NULL ? 1 : 0));
                            (VOID)InterlockedExchange(&g_WinMaliFlipWorkerPages,
                                (LONG)fa->AperturePageCount);
                        }
                    }
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipSrcUsed",  (ULONG)g_WinMaliStatFlipSrcUsed);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipSrcMapFail",(ULONG)g_WinMaliStatFlipSrcMapFail);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipWorkerMdl", (ULONG)g_WinMaliFlipWorkerMdl);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"FlipWorkerPages",(ULONG)g_WinMaliFlipWorkerPages);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsReq",        (ULONG)g_WinMaliStatCsReq);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsAck",        (ULONG)g_WinMaliStatCsAck);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsgReq",       (ULONG)g_WinMaliStatCsgReq);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsgAck",       (ULONG)g_WinMaliStatCsgAck);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsFatalInfoLo",(ULONG)g_WinMaliStatCsFatalInfoLo);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsFatalInfoHi",(ULONG)g_WinMaliStatCsFatalInfoHi);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsSeqnoLo",    (ULONG)g_WinMaliStatSeqnoLo);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsTagLo",      (ULONG)g_WinMaliStatTagLo);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"AsFaultSt",    (ULONG)g_WinMaliStatAsFaultSt);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"AsFaultLo",    (ULONG)g_WinMaliStatAsFaultLo);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"AsFaultHi",    (ULONG)g_WinMaliStatAsFaultHi);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"McuFaultSt",   (ULONG)g_WinMaliStatMcuFaultSt);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"GpuFaultSt",   (ULONG)g_WinMaliStatGpuFaultSt);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsfLastStatus", (ULONG)g_WinMaliStatCsfLastStatus);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsfLastAs",     (ULONG)g_WinMaliStatCsfLastAs);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsfLastSize",   (ULONG)g_WinMaliStatCsfLastSize);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsfLastVaLo",   (ULONG)g_WinMaliStatCsfLastVaLo);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsfLastVaHi",   (ULONG)g_WinMaliStatCsfLastVaHi);

                    /* ★ THE FLIP PATH, READABLE WHILE THE MACHINE IS UP.
                     *
                     * FlipLatchCount / VsyncArm / VsyncNotify / SourceVisible
                     * are published ONLY from StopDevice today, and that publish
                     * has never yielded anything but <absent> - so the single
                     * most important number on the next front (does dxgk EVER
                     * latch a NEW scanout address?) cannot currently be read at
                     * all, on a running machine.
                     *
                     * ⚠ DELIBERATELY NEW VALUE NAMES: StopDevice keeps Stat*,
                     * these are Live*. One value name, ONE writer - two writers
                     * on one name silently reverted an entire afternoon of
                     * measurements once already. */
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveFlipLatch",   (ULONG)a->FlipLatchCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveVsyncArm",    (ULONG)a->VsyncArmCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveVsyncNotify", (ULONG)a->VsyncNotifyCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveSrcVisible",  (ULONG)a->SourceVisible);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveSrcVisEver",  (ULONG)a->SourceVisibleEver);
                    /* Split, so a plain DWORD read decodes the address with no
                       packing convention to misremember later. */
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveFlipAddrLo",
                            (ULONG)((ULONG64)a->LastFlipAddress & 0xFFFFFFFFull));
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveFlipAddrHi",
                            (ULONG)(((ULONG64)a->LastFlipAddress >> 32) & 0xFFFFFFFFull));
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveScanoutBlts", (ULONG)a->ScanoutBltCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"LiveScanoutSkip", (ULONG)a->ScanoutBltSkipped);

                    /* ⛔★★★★★★ THESE 28 VALUES WERE COSTING US EVERY NEW COUNTER.
                     *
                     * `Parameters` caps at 127 values. This block published
                     * 7 fields x 4 candidate slots = 28 of them EVERY tick, and
                     * with the rest of the telemetry the key regenerates to ~204
                     * within a minute of every boot. Past the cap, CREATING a new
                     * value silently fails - so any counter added late in the
                     * publish order can never exist, and its absence is
                     * indistinguishable from "the code never ran".
                     *
                     * That ambiguity has now cost four wrong readings in one
                     * session: StatUvSrcAsked=0 (read as "AcquireBest never ran"
                     * when it had run 1674 times), StatUvProbe, StatPowerStop,
                     * and StatUvMaxCov. Each time I reasoned about driver
                     * behaviour from a number the registry had refused to store.
                     *
                     * The candidate rows are also superseded: they predate the
                     * per-slot Pitch/Size/coverage data, and the winner's own
                     * figures plus StatUvMaxCov answer the question they were
                     * added for ("was something better available?") in 2 values
                     * instead of 28.
                     *
                     * ⇒ Deleted. Budget is a correctness property of the
                     * instrument, not a tidiness concern. */
                    UNREFERENCED_PARAMETER(ci);
                }
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatApPrevSrc",         (ULONG)g_WinMaliStatApPrevSrc);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentNullCookie", (ULONG)g_WinMaliStatPresentNullCookie);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentBlts",       (ULONG)g_WinMaliStatPresentBlts);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatOpenCalls",         (ULONG)g_WinMaliStatOpenCalls);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPawmReg",           (ULONG)g_WinMaliStatPawmRegistered);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatOpenByPawm",        (ULONG)g_WinMaliStatOpenByPawm);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatOpenResolved",      (ULONG)g_WinMaliStatOpenResolved);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatOpenById",          (ULONG)g_WinMaliStatOpenById);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatOpenFailed",        (ULONG)g_WinMaliStatOpenFailed);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatOpenFailPdMagic",   (ULONG)g_WinMaliStatOpenFailPdMagic);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatOpenFailPdSize",    (ULONG)g_WinMaliStatOpenFailPdSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentFlips",      (ULONG)g_WinMaliStatPresentFlips);
                /* Proof-of-life for OUR UMD's _Present, under names NOTHING else
                   writes. The previous attempt rode on StatPresentFlips /
                   StatPresentNotBlt and was clobbered by the sampler's duplicate
                   publish, so its "0" carried no information at all. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUmdPresentCtx",     (ULONG)g_WinMaliStatUmdPresentCtx);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatCtxPrivSize",       (ULONG)g_WinMaliStatCtxPrivSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentPrivSz",     (ULONG)g_WinMaliStatPresentPrivSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentPrivHit",    (ULONG)g_WinMaliStatPresentPrivHits);
                /* Blits vs Skipped is how a silent early-out becomes visible. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatScanoutBlits",      (ULONG)a->ScanoutBltCount);
                /* â›” THE FLIP COUNTERS BELONG HERE, NOT IN StopDevice. This block
                   demonstrably runs - StatScanoutBlits climbs 12-15/s - whereas the
                   teardown copy never ran during operation and made the flip path
                   unobservable for three builds.
                   StatOvlFlipN  = how many times the flip gate was EVALUATED
                   StatOvlFlipRej= the gate's outcome (1=BackVa NULL 2=OvlFlip!=2
                                   3=FlipRender off 4=gate PASSED)
                   â›” StatOvlStep is NOT touched: the mode-1 path already writes it,
                   and one value name must have exactly ONE writer. */
                if (g_WinMaliOvlFlip != 0) {
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatOvlFlipN",    (ULONG)g_WinMaliStatOvlFlipN);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatOvlFlipRej",  (ULONG)g_WinMaliStatOvlFlipRej);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatOvlFlip2N",   (ULONG)g_WinMaliStatOvlFlip2N);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatOvlFlip2Rej", (ULONG)g_WinMaliStatOvlFlip2Rej);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatOvlFlipBack", (ULONG)g_WinMaliStatOvlFlipBack);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatOvlFlipRight",(ULONG)g_WinMaliStatOvlFlipRight);
                }
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatScanoutSkipped",    (ULONG)a->ScanoutBltSkipped);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatFlipAddrLo",
                        (ULONG)((UINT64)InterlockedCompareExchange64(&a->LastFlipAddress, 0, 0) & 0xFFFFFFFFu));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentPrivSize",   (ULONG)g_WinMaliStatPresentPrivSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentPrivHits",   (ULONG)g_WinMaliStatPresentPrivHits);
                /* ⛔★★★★★★ THESE TWO NAMES WERE OVERLOADED AND IT COST HOURS.
                 *
                 * They used to publish `0x5000 + someOtherCounter` under names
                 * that say "Handle", while DxgkDdiPresent ALSO wrote the real
                 * handles into the same globals - one value name, TWO writers,
                 * the exact trap this codebase already warns about, done
                 * deliberately to save registry slots and labelled OVERLOADED in
                 * a trailing comment I did not read.
                 *
                 * EVERY "handle" quoted this session - 20480, 21202, 21898,
                 * 20794, 0x53A2 - was this overload: src constant because
                 * ApWithContent=0, dst climbing because it is the UMD's _Present
                 * context count. It produced "handles that change while the
                 * present count is frozen", which I chased as a real anomaly, and
                 * it made the DDI's actual handles INVISIBLE for the whole
                 * investigation.
                 *
                 * ⇒ Overloaded names are not a slot-saving trick, they are a
                 * measurement that lies. The cap is fixed properly now (reserved
                 * slots), so publish the truth under names with ONE writer each.
                 * The old names keep their overloaded meaning ONLY because older
                 * notes quote them; the *Real names are the ones to trust. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresSrcHReal",      (ULONG)g_WinMaliStatPresentSrcHandle);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresSrcRectTL",     (ULONG)g_WinMaliStatPresSrcRectTL);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresSrcRectBR",     (ULONG)g_WinMaliStatPresSrcRectBR);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresDstRectTL",     (ULONG)g_WinMaliStatPresDstRectTL);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresDstRectBR",     (ULONG)g_WinMaliStatPresDstRectBR);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresSubRectCnt",    (ULONG)g_WinMaliStatPresSubRectCnt);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresDmaSize",       (ULONG)g_WinMaliStatPresDmaSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresMultipass",     (ULONG)g_WinMaliStatPresMultipass);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresColor",         (ULONG)g_WinMaliStatPresColor);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresDstHReal",      (ULONG)g_WinMaliStatPresentDstHandle);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatApWithContent2",    (ULONG)g_WinMaliStatApWithContent);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUmdPresentCtx2",    (ULONG)g_WinMaliStatUmdPresentCtx);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentDmaPrivSize",(ULONG)g_WinMaliStatPresentDmaPrivSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatBltDescSize",       (ULONG)(0x5000u + (ULONG)g_WinMaliStatApFirstIdx));   /* OVERLOADED: +0x5000 = bestIdx */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentLastFlags",  (ULONG)g_WinMaliStatPresentLastFlags);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentLastCounts", (ULONG)g_WinMaliStatPresentLastCounts);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatFlipSrcSet",
                        (ULONG)(InterlockedCompareExchangePointer(
                                    (PVOID volatile *)&g_WinMaliFlipSourceAlloc, NULL, NULL) != NULL));
            }
        }

        if (a->ScanoutVa == NULL || a->DmaSegmentVa == NULL) {
            continue;
        }

        /* THE READINESS GATE. See Parameters\WorkerVisGate.
           The test pattern deliberately bypasses it: proving the OUTPUT path is
           good must not depend on the OS having accepted a source. */
        if (g_WinMaliWorkerVisGate != 0 && g_WinMaliPresentTestPattern == 0 &&
            InterlockedCompareExchange(&a->SourceVisible, 1, 1) != 1) {
            (VOID)InterlockedIncrement64(&a->ScanoutBltSkipped);
            continue;
        }

        {
            UINT64 flipAddr = (UINT64)InterlockedCompareExchange64(&a->LastFlipAddress, 0, 0);
            UINT64 segBase  = WINMALI_SYSMEM_GPU_BASE;
            SIZE_T offset;
            SIZE_T bytes    = a->ScanoutBytes;

            if (flipAddr < segBase) {
                (VOID)InterlockedIncrement64(&a->ScanoutBltSkipped);
                continue;
            }
            offset = (SIZE_T)(flipAddr - segBase);
            if (offset >= a->DmaSegmentBytes ||
                bytes > a->DmaSegmentBytes - offset) {
                /* The primary is not (fully) inside the segment we can address.
                   Never copy a partial/garbage frame to the panel. */
                (VOID)InterlockedIncrement64(&a->ScanoutBltSkipped);
                continue;
            }

            /*
             * TEST PATTERN (Parameters\PresentTestPattern=1).
             *
             * WHY THIS EXISTS: scanoutBlits=2733 with skipped=0 proves we WROTE
             * 2733 frames. It does NOT prove the frames were right, and it does
             * not prove the source had anything in it - dxgk latched
             * FlipLatchCount=1, i.e. it set the primary address exactly ONCE.
             * If Windows never put a desktop on our (phantom external) monitor,
             * we faithfully copied an EMPTY primary 2733 times and the counters
             * would look identical to success.
             *
             * So paint something only we could have produced: vertical colour
             * bars plus a white band that MOVES one step per frame. Bars prove
             * geometry/stride/format; the moving band proves the output is live
             * rather than one stale frame left on the panel. This separates "can
             * we drive this panel at all" from "does Windows give us content" -
             * worth knowing BEFORE re-flashing firmware to chase POST ownership.
             */
            if (g_WinMaliPresentTestPattern != 0) {
                ULONG y, x;
                ULONG frame = (ULONG)InterlockedIncrement64(&a->ScanoutBltCount);
                ULONG band  = (frame * 8u) % WINMALI_FB_HEIGHT;
                for (y = 0; y < WINMALI_FB_HEIGHT; ++y) {
                    ULONG *row = (ULONG *)((PUCHAR)a->ScanoutVa +
                                           (SIZE_T)y * WINMALI_FB_STRIDE);
                    if (y >= band && y < band + 24u) {
                        for (x = 0; x < WINMALI_FB_WIDTH; ++x) { row[x] = 0x00FFFFFFu; }
                        continue;
                    }
                    for (x = 0; x < WINMALI_FB_WIDTH; ++x) {
                        /* 8 bars: red, green, blue, yellow, cyan, magenta, white, black */
                        static const ULONG bars[8] = {
                            0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00FFFF00u,
                            0x0000FFFFu, 0x00FF00FFu, 0x00FFFFFFu, 0x00000000u
                        };
                        row[x] = bars[(x * 8u) / WINMALI_FB_WIDTH];
                    }
                }
                continue;
            }

            /*
             * SOURCE SAMPLER. The test pattern proved the OUTPUT path works
             * (bars + moving band reached the panel), and reaching that code at
             * all proves dxgk latched a VALID primary address - the bounds
             * checks above `continue` out otherwise. So flips happen and blits
             * happen, yet the panel is black, which leaves exactly one question:
             * IS THERE ANYTHING IN THE PRIMARY WE ARE COPYING?
             *
             * A counter cannot answer that (WinMaliDriver.c:1331 already records
             * scanoutBlits=2733 with skipped=0 while the output was empty). So
             * sample the SOURCE itself: OR every pixel of a sparse grid together
             * and count the non-black ones. all-zero => Windows renders nothing
             * into this primary and the bug is upstream of us; non-zero => the
             * content is there and our copy or scanout geometry is wrong.
             *
             * Sparse (every 97th row, every 89th pixel - coprime with the width
             * so the grid does not alias a repeating pattern) and rate-limited,
             * because this runs per frame on the present thread.
             */
            /*
             * ---- COPY FROM THE ALLOCATION'S OWN PAGES ----------------------
             *
             * ⛔ THE BLACK SCREEN WAS HERE. This used to copy from
             * DmaSegmentVa + offset. That is wrong for an APERTURE segment:
             * MapApertureSegment maps the ALLOCATION'S OWN MDL pages into the
             * GPU VA window (WinMaliPaging.c), and DmaSegmentVa is a
             * driver-side buffer that nothing ever renders into. So every frame
             * faithfully copied an empty scratch buffer onto the panel.
             *
             * MEASURED, which is the only reason this was found: the source
             * sampler reported nonBlack=0 for 646 pixels while blits climbed
             * past 3000 with skipped=0, and a sweep of the WHOLE segment came
             * back "entirely BLACK across 134217728 bytes". Meanwhile the test
             * pattern - which writes the DESTINATION directly - showed perfect
             * bars on the panel. Output good, source empty.
             *
             * Find the allocation whose aperture GPU VA covers the flip address
             * and read ITS pages. Fall back to skipping (counted) rather than
             * copying the wrong memory: a black panel is bad, a garbage one is
             * worse and much harder to diagnose.
             */
            {
                PWINMALI_KMD_ALLOCATION src = NULL;
                PUCHAR                  srcVa = NULL;
                ULONG                   s;
                /* ⛔★★★★★ THE USE-REF LEAK SWEEP.
                 *
                 * AcquireBest and ReacquireVa both take a use-ref that
                 * WinMaliUvSrcRevoke blocks on, so it MUST be dropped once per
                 * acquisition. The release below sits after the copy, but this
                 * block has several `continue` paths and one missed path would
                 * stall every future BO teardown for the full 1 s timeout.
                 * Rather than audit every exit, sweep at the TOP of the next
                 * iteration: worst case a ref is held ~one tick (~30 ms) longer
                 * than needed, and a leak can never accumulate.
                 * ★ Only one worker thread touches this, so a plain static is
                 * sufficient - and the sweep runs BEFORE any new acquisition, so
                 * it can never drop a ref this iteration just took. */
                static ULONG            s_pendingUse = 0;
                if (s_pendingUse != 0) {
                    WinMaliUvSrcEndUse(s_pendingUse);
                    s_pendingUse = 0;
                }

                /* ⛔★★★★★ YIELD THE PANEL TO A FRESH PUSH - AND ONLY HERE.
                 *
                 * UvPush writes dwm's finished frame straight to the panel from
                 * the BO destroy path. If the worker also repaints its (stale)
                 * candidate on its own tick, the two alternate on the glass and
                 * the panel STROBES between the live desktop and the old frame.
                 * USER-OBSERVED on 1.1.11.2: "epileptic flashing... between
                 * desktop and black".
                 *
                 * PLACEMENT IS THE WHOLE SAFETY ARGUMENT:
                 *   - the s_pendingUse sweep is DONE, so no use-ref is in flight
                 *     and none can be leaked by this `continue`;
                 *   - the counter-publish block is far above, so the instruments
                 *     keep publishing on every skipped tick.
                 * 1.1.11.0 put this after the heartbeat instead - above BOTH -
                 * and black-screened the box.
                 *
                 * 250 ms window: dwm pushes at ~9-12/s, so a live producer holds
                 * the panel continuously and a dead one hands it back after a
                 * quarter second - the frozen-desktop behaviour, not a black one. */
                if (g_WinMaliUvPush != 0) {
                    LONG64 pushT = InterlockedCompareExchange64(&g_WinMaliUvPushTick, 0, 0);
                    if (pushT != 0 &&
                        ((LONG64)KeQueryInterruptTime() - pushT) < (2500LL * 1000LL)) {
                        (VOID)InterlockedIncrement64(&a->ScanoutBltSkipped);
                        continue;
                    }
                }
                /* ⛔★★★★★ WHICH BRANCH ACTUALLY SET srcVa. Set it WHERE srcVa is
                 * assigned - never inferred afterwards.
                 *
                 * My first attempt at this computed the "source path" in the copy
                 * block from whether the flip allocation happened to be non-NULL:
                 *     (ga != NULL && gaVa != NULL) ? 1 : 2
                 * `ga` is read there unconditionally, so it reported 1 (VidMm)
                 * even with FlipSrcFromVidMm=0 and the VidMm branch compiled out
                 * of the path. It measured "does that allocation exist", not
                 * "did we read from it" - and I nearly concluded the knob was
                 * broken. Same trap as StatUvSrcChg: a counter next to the thing
                 * it claims to describe is not the same as a counter ON it.
                 *
                 * 0 = none, 1 = VidMm aperture MDL, 2 = 0x93 probe-locked BO. */
                ULONG                   srcPath = 0;
                /* Handle of the uv candidate srcVa came from, so the copy loop can
                   re-check it is still live between slices (we no longer hold the
                   table lock across the copy - that wedged DWM). 0 = not from uv. */
                ULONG                   uvHandleForCopy = 0;
                /* Row pitch of the adopted source, 0 = unknown/assume panel pitch. */
                ULONG                   srcPitch = 0;
                /* Real byte size of the adopted source. Clamping rows by the
                   FRAME size instead is only a guaranteed LOWER bound: measured
                   StatWinRows=1820 against a surface that actually holds 1856
                   rows, so 20 rows of panel went unpainted for no reason.
                   Over-trusting it walks off a locked MDL, so only widen when
                   the source itself reports a larger extent. */
                SIZE_T                  srcBytes = 0;
                /* THE AUTHORITATIVE BOUND - see WinMaliSrcRow_() above.
                   Bytes readable FROM srcVa (mapped extent MINUS any offset
                   already added into srcVa). 0 == unknown, and unknown means we
                   do not read at all. Derived row geometry follows it. */
                SIZE_T                  srcReadable = 0;
                ULONG                   srcRowPitch = 0;
                ULONG                   srcRowBytes = 0;
                ULONG                   srcMaxRows  = 0;
                /* TRUE => srcVa points into a 0x93 BO and we hold the candidate
                   table lock, which must be released after the copy. */
                BOOLEAN                 uvHeld = FALSE;

                /* ★★ APERTURE SWEEP: DOES DWM'S COMPOSED FRAME EXIST ANYWHERE
                 * WE CAN REACH?
                 *
                 * We cannot ask the present path which surface DWM composed
                 * into: those presents do not come from our UMD at all (PROVED -
                 * UmdPresentCtx=0) and they carry an empty allocation list, so
                 * g_WinMaliFlipSourceAlloc is never set and the lookup below
                 * always falls back to the frozen mode-set surface, which
                 * nothing ever writes.
                 *
                 * So stop asking and LOOK. Sample every aperture allocation for
                 * non-zero content. This separates two very different worlds:
                 *   >0 with content => DWM DOES render into memory we can
                 *      address; we are copying the wrong allocation and the fix
                 *      is identification.
                 *    0 with content => DWM's output never reaches our aperture
                 *      at all - a far deeper problem, and no amount of
                 *      present-path work would ever have fixed it.
                 * Cheap: one DWORD per page, early-out at 4 hits, every 120th
                 * frame only. */
                {
                    static LONG64 s_sweepTick = 0;
                    if ((InterlockedIncrement64(&s_sweepTick) % 120) == 1) {
                        ULONG withContent = 0, firstIdx = 0xFFFF, i;
                        PWINMALI_KMD_ALLOCATION bestCand = NULL;
                        ULONG bestIdx = 0xFFFF, bestPages = 0, bestHits = 0;
                        const ULONG needPages =
                            (ULONG)((a->ScanoutBytes + PAGE_SIZE - 1) / PAGE_SIZE);

                        for (i = 0; i < RTL_NUMBER_OF(a->ApertureAllocs); ++i) {
                            PWINMALI_KMD_ALLOCATION k = a->ApertureAllocs[i];
                            PUCHAR kb;
                            ULONG  hits = 0, n, pages;

                            if (k == NULL || k->ApertureMdl == NULL ||
                                k->AperturePageCount == 0) {
                                continue;
                            }
                            /* ⛔⛔⛔ NEVER MAP AN MDL FROM THIS SWEEP. MEASURED, TWICE.
                             *
                             * Calling MmGetSystemAddressForMdlSafe() here maps
                             * VidMm's MDLs (they are generally NOT already
                             * system-mapped) and consumes a system resource that
                             * only VidMm releases. It does NOT hang the box - it
                             * starves the graphics stack, and that surfaces as
                             * allocation failures elsewhere:
                             *
                             *   at ~5.5 min      no mapping (1133)   mapping (1134)
                             *   beyond-the-BO           11               117
                             *   vm_bind failures         1                29
                             *   CreateResource:541       0                32
                             *   UMD faults               4                31
                             *   dwm                stable 4.4 min   crash-looping
                             *
                             * Size-gating it to "only plausible primaries" was
                             * NOT enough - a couple of MDLs per sweep still did
                             * it. Read only what is already mapped, full stop.
                             *
                             * ⚠ The cost is that this sweep is usually BLIND
                             * (apWithContent=0) and nothing gets adopted. That is
                             * a real limitation, not a fix - finding the primary
                             * must be done WITHOUT mapping, e.g. from the
                             * flipAddr/GpuVa lookup the copy path already uses on
                             * the one MDL it legitimately maps per frame. */
                            if ((k->ApertureMdl->MdlFlags &
                                 (MDL_MAPPED_TO_SYSTEM_VA |
                                  MDL_SOURCE_IS_NONPAGED_POOL)) == 0) {
                                continue;
                            }
                            kb = (PUCHAR)k->ApertureMdl->MappedSystemVa;
                            if (kb == NULL) {
                                continue;
                            }
                            /* ⚠ AperturePageCount is NOT a total to subtract the
                               offset from. Per WinMaliPaging.c:189 the pair is
                                 ApertureMdlOffset = where the run STARTS in the MDL
                                 AperturePageCount = how many pages are MAPPED
                               so the run is exactly [MdlOffset, +AperturePageCount).
                               An earlier version here did
                               `pages = AperturePageCount - ApertureMdlOffset`,
                               which silently under-scanned every surface. */
                            kb   += (SIZE_T)k->ApertureMdlOffset * PAGE_SIZE;
                            pages = k->AperturePageCount;

                            /* ★ SCORE THE WHOLE SURFACE - DO NOT EARLY-OUT.
                             *
                             * This used to stop at 4 hits, so EVERY painted
                             * allocation scored identically and "best"
                             * degenerated into "whichever came first". That is
                             * why coverage varied per boot - 15%, 20%, 94% -
                             * purely on which partial layer happened to be
                             * latched first; from the outside it looked like
                             * the driver "falling back to something lesser".
                             * Count real coverage so the fullest surface wins
                             * deterministically. */
                            for (n = 0; n < pages; ++n) {
                                if (*(const ULONG *)(kb + (SIZE_T)n * PAGE_SIZE) != 0) {
                                    ++hits;
                                }
                            }
                            if (hits != 0) {
                                ++withContent;
                                if (firstIdx == 0xFFFF) {
                                    firstIdx = i;
                                }
                                /* Candidate for the primary: at least a full
                                   framebuffer, ranked by how much of it is
                                   actually painted. Ties break toward the
                                   smaller allocation, so an oversized heap
                                   holding junk cannot outrank a real primary. */
                                if (pages >= needPages &&
                                    (bestCand == NULL || hits > bestHits ||
                                     (hits == bestHits && pages < bestPages))) {
                                    bestCand  = k;
                                    bestHits  = hits;
                                    bestPages = pages;
                                    bestIdx   = i;
                                }
                            }
                        }
                        (VOID)InterlockedExchange(&g_WinMaliStatApWithContent,
                                                  (LONG)withContent);
                        (VOID)InterlockedExchange(&g_WinMaliStatApFirstIdx,
                                                  (LONG)bestIdx);

                        /* ★★★ ADOPT THE BEST CANDIDATE AS THE SCANOUT SOURCE.
                         *
                         * MEASURED: 23 of 32 aperture allocations hold real
                         * content while the one the frozen mode-set address
                         * resolves to is empty - DWM's composed frame IS in
                         * memory we can reach; we were reading the wrong
                         * allocation. Nothing can tell us WHICH: the presents
                         * are not ours and carry no allocation list. So
                         * discriminate on the only properties the primary must
                         * have - painted, and at least a framebuffer in size.
                         *
                         * ⚠ HEURISTIC, deliberately. Worst case we scan out the
                         * wrong surface, which is a VISIBLE and therefore
                         * diagnosable failure, unlike the silent black screen it
                         * replaces.
                         *
                         * ⛔ CAS-AGAINST-NULL WAS ITSELF A BUG. Measured
                         * bestIdx=0 (a valid candidate) together with adopted=0,
                         * which can only mean g_WinMaliFlipSourceAlloc was
                         * ALREADY non-NULL - so the CAS lovingly protected an
                         * existing source that samples BLACK, and we copied it
                         * forever. Nothing legitimately sets that pointer: the
                         * flip branch needs Flags.Flip (never set, flips=0) and
                         * the AllocId branch never fires (PrivHits=0), so an
                         * existing value has no claim to being right. Adopt
                         * unconditionally - a painted, correctly-sized surface
                         * beats whatever happens to be there. */
                        (VOID)InterlockedExchange(&g_WinMaliStatApPrevSrc,
                                (LONG)(ULONG_PTR)InterlockedCompareExchangePointer(
                                    (PVOID volatile *)&g_WinMaliFlipSourceAlloc,
                                    NULL, NULL));
                        if (bestCand != NULL) {
                            (VOID)InterlockedExchangePointer(
                                (PVOID volatile *)&g_WinMaliFlipSourceAlloc,
                                bestCand);
                            (VOID)InterlockedExchange(&g_WinMaliStatApAdopted,
                                                      (LONG)(bestIdx + 1));
                        }
                    }
                }

                /* 🏆 TRUE FIRST CHOICE: THE ALLOCATION DXGK ACTUALLY SCANS OUT.
                 *
                 * Everything below this is a HEURISTIC for a question the OS
                 * answers directly. DxgkDdiSetVidPnSourceAddress hands us
                 * hAllocation - our own WINMALI_KMD_ALLOCATION for the surface
                 * being scanned out - on every flip. Scoring imported surfaces
                 * by "painted and freshest" instead latched DWM's RENDER TARGET,
                 * which is why the panel showed a full desktop with NO TASKBAR
                 * and NO CURSOR: those are composited into the flip primary,
                 * which the heuristic never even considered.
                 *
                 * This became usable only with the dxgk-native gates on. With
                 * escape submit, VidMm never grants residency to anything
                 * (measured ApMapOps == 0), so the primary had no MDL and this
                 * branch could not exist. With ADDR/SUBMIT=1: ApMapOps 104,
                 * ApMapMaxSize 21,667,840, FlipWorkerMdl 1, FlipWorkerPages 5290
                 * - an exact match for the primary.
                 *
                 * ⚠ The MDL is VidMm's and is only valid while the allocation is
                 * resident; UNMAP_APERTURE_SEGMENT clears the stash. Re-read it
                 * every tick and never cache the mapped VA across ticks.
                 * ⚠ Require the full extent - a partial mapping would be read as
                 * a short surface, the same class of bug as the 262-row clamp. */
                /* ⛔★★★★★★ READING VidMm's APERTURE MDL HANGS THE MACHINE.
                 *
                 * PROVEN 2026-08-17 by running an armed boot with
                 * `Parameters\PresentTestPattern=1`, which paints colour bars and
                 * NEVER touches srcVa. The panel showed clean bars with a smoothly
                 * moving band, the box stayed up - and everything that had looked
                 * independently broken all session came right at once:
                 *     dwm ON OUR UMD · LiveFlipLatch 1 -> 2 (pinned at 1 for
                 *     every previous build) · StatPresentCalls 0 -> 23 ·
                 *     GROUP_SUBMIT failures 0 · 1929/1929 submits · 0 crashes.
                 * One cause, four symptoms.
                 *
                 * MECHANISM (fits a HANG with no bugcheck): this VA maps memory
                 * VidMm owns. Touching a page that is not resident faults; VidMm
                 * must page it in; it queues a paging operation; that operation is
                 * serviced by THIS driver - while our worker thread is blocked
                 * inside the fault. Deadlock, no crash, machine wedged.
                 *
                 * ⇒ NEVER read VidMm memory from the present worker. Read only
                 * pages WE probe-locked ourselves - that is the 0x93 BoFromUserVa
                 * path (MmProbeAndLockPages), which previously delivered a full
                 * desktop at nonBlack 646/646.
                 *
                 * `FlipSrcFromVidMm` (default 0) exists only to re-test this
                 * deliberately; leave it 0. Earlier today I made this VidMm path
                 * take PRIORITY over 0x93 - that is what introduced the hang. */
                if (srcVa == NULL && g_WinMaliFlipSrcFromVidMm != 0) {
                    PWINMALI_KMD_ALLOCATION fa =
                        (PWINMALI_KMD_ALLOCATION)InterlockedCompareExchangePointer(
                            (PVOID volatile *)&g_WinMaliFlipAlloc, NULL, NULL);
                    if (fa != NULL && fa->Magic == WINMALI_KMD_ALLOC_MAGIC &&
                        fa->ApertureMdl != NULL &&
                        (SIZE_T)fa->AperturePageCount * PAGE_SIZE >= bytes) {
                        /* ⛔ READ THE CACHED VA - DO NOT MAP HERE.
                           This used to call MmGetSystemAddressForMdlSafe on
                           VidMm's MDL every tick (~30/s). VidMm can tear that
                           MDL down between the NULL check above and the call,
                           and the machine reboot-looped: desktop, then instant
                           reset, repeatedly. The mapping now happens once in
                           MAP_APERTURE_SEGMENT where the MDL is known valid,
                           and UNMAP clears ApertureVa before ApertureMdl. */
                        PUCHAR fv = (PUCHAR)fa->ApertureVa;
                        if (fv != NULL) {
                            /* ★ INVALIDATE BEFORE READING - the 0x93 path already
                               does this and this one did not.
                               ApertureVa is a CACHED system mapping. Whoever last
                               wrote these pages (the GPU, or the display engine)
                               did not do it through this CPU's caches, so without
                               an invalidate we can read the bytes that were there
                               when the mapping was created and never see an
                               update - a surface that is non-black, structured,
                               and FROZEN, which is exactly the symptom we have
                               been chasing. WinMaliUvSelectSrc flushes its MDL for
                               precisely this reason (WinMaliBo.c). Same call, same
                               argument order, on the path we actually take.
                               Safe at any IRQL. */
                            if (fa->ApertureMdl != NULL) {
                                KeFlushIoBuffers(fa->ApertureMdl, TRUE, TRUE);
                            }
                            /* The stash records where in the MDL this allocation
                               starts; AperturePageCount is a COUNT, not a total. */
                            srcVa = fv + ((SIZE_T)fa->ApertureMdlOffset * PAGE_SIZE);
                            /* The gate above already proved
                               AperturePageCount*PAGE_SIZE >= bytes, and nothing
                               beyond the stash offset is added to srcVa, so the
                               whole run is readable from here. */
                            srcReadable = (SIZE_T)fa->AperturePageCount * PAGE_SIZE;
                            srcPath = 1;   /* VidMm aperture MDL */
                            (VOID)InterlockedIncrement(&g_WinMaliStatFlipSrcUsed);

                            /* ★★★★★ PUBLISH THE SOURCE'S GEOMETRY, NOT JUST THAT
                             * BYTES MOVED.
                             *
                             * Every counter here measures whether a copy HAPPENED;
                             * none measure whether it was the right SHAPE. A 4 MB
                             * 1024x1024 surface smeared across a 2944x1840 scanout
                             * at pitch 11776 reports as a perfect success - Blts
                             * climbs, FlipSrcUsed climbs, StatSrcNonBlack finds
                             * content - while the panel shows a slanted band over
                             * the top ~19% (4194304/11776 = 356 of 1840 rows, and
                             * src pitch 4096 vs dst 11776 walks the diagonal).
                             * The user's photo of the panel showed exactly that.
                             *
                             * So publish what we are ABOUT TO COPY FROM. If this
                             * reports 1024x1024 pitch 4096 while we copy `bytes`
                             * at WINMALI_FB_STRIDE, the mismatch IS the picture.
                             * Rate-limited to the sampler's cadence; StatGeo* so
                             * the deploy's ^Stat purge sweeps it (a Src* name
                             * would escape the deny-list and risk the 127 cap). */
                            {
                                static LONG64 s_geoTick = 0;
                                if ((InterlockedIncrement64(&s_geoTick) % 120) == 1) {
                                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                            L"StatGeoW",     (ULONG)fa->Width);
                                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                            L"StatGeoH",     (ULONG)fa->Height);
                                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                            L"StatGeoPitch", (ULONG)fa->Pitch);
                                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                            L"StatGeoPages", (ULONG)fa->AperturePageCount);
                                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                            L"StatGeoBytes", (ULONG)bytes);
                                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                            L"StatGeoDstPitch", (ULONG)WINMALI_FB_STRIDE);
                                }
                            }
                        } else {
                            (VOID)InterlockedIncrement(&g_WinMaliStatFlipSrcMapFail);
                        }
                    }
                }

                /* 🎯 FALLBACK: A PAINTED 0x93 BO.
                 *
                 * ⛔ THIS USED TO BE A LAST RESORT, AFTER the aperture branches,
                 * AND THAT MADE IT DEAD CODE. Measured with gates off, nonce
                 * 1143: `sampled=646` (so a source WAS found) with `uvBo=0` (so
                 * this never ran) and `nonBlack=0`. The aperture path "succeeds"
                 * every frame - on the frozen mode-set surface that nothing ever
                 * writes, which is the empty source this whole investigation has
                 * been reading. Being second in line meant losing to it forever.
                 *
                 * A 0x93 candidate is strictly better evidence: it is a surface
                 * a UMD process LOCKED and handed us the CPU VA of - i.e. a real
                 * DWM backbuffer - and we only adopt it when it is actually
                 * PAINTED (hits > 0) and at least framebuffer size. Fall through
                 * to the aperture branches when no candidate qualifies, so this
                 * can only ever add a source, never remove one.
                 *
                 * ⚠ AcquireBest returns HOLDING the table lock; release after
                 * the copy. */
                {
                    ULONG uvHandle = 0, uvHits = 0;
                    PVOID uv = NULL;
                    ULONG latched;

                    /* ⛔★★★★★ PROVE THIS BLOCK RUNS - DO NOT INFER IT.
                     *
                     * MEASURED 1208: StatUvSrcAsked=0 and StatUvSrcCands=0 while
                     * Stat0x93Calls=81/81 OK and StatDbgSrcPath=2. i.e. DWM
                     * registered 81 surfaces, and the consumer of those surfaces
                     * apparently never ran - yet this block is UNCONDITIONAL and
                     * sits above the aperture branch that did run. That is a flat
                     * contradiction, so one of the two readings is lying.
                     *
                     * StatUvSrcAsked is incremented INSIDE AcquireBest and only
                     * AFTER its `NeedBytes == 0` early return, so it cannot
                     * distinguish "never called" from "called with bytes==0" -
                     * and a counter that cannot split its outcomes is not an
                     * oracle. These two write from OUTSIDE any branch:
                     *   StatUvProbe = 0xC0DE  - this block executed, full stop
                     *   StatUvBytes = bytes   - what we actually passed in
                     * Absent probe  => the block never runs (or the key is full).
                     * Probe + bytes=0 => AcquireBest bailed before counting.
                     * Probe + bytes=21667840 => it ran and Asked's channel is
                     * broken, which would make every Uv* zero this session
                     * meaningless. Flushed, first 3 passes only. */
                    {
                        static LONG s_uvProbe = 0;
                        if (InterlockedIncrement(&s_uvProbe) <= 3) {
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatUvProbe", 0xC0DEu);
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatUvBytes", (ULONG)bytes);
                        }
                    }

                    latched = (ULONG)InterlockedCompareExchange(
                                  &g_WinMaliUvLatchBo, 0, 0);

                    /* ⛔★★★★★ ONE SOURCE PER SWEEP - DO NOT RE-SELECT MID-FRAME.
                     *
                     * A wake copies at most 2 MB, so a 21,667,840 B frame needs
                     * ~11 wakes. This block used to run AcquireBest on every one
                     * of them, and AcquireBest deliberately picks the MOST
                     * RECENTLY WRITTEN surface - so consecutive strips of a
                     * single scanout came from DIFFERENT buffers composed at
                     * different instants.
                     *
                     * USER-OBSERVED, 1208: "the lines were changing places on
                     * every reprint". That is not tearing in the ordinary sense;
                     * it is a mosaic, and it is what re-selecting mid-frame
                     * must produce.
                     *
                     * So: latch the winner when a sweep starts and re-resolve
                     * that same handle for the rest of the sweep. The latch is
                     * dropped when the sweep wraps (next frame gets a fresh,
                     * newer surface) or when the candidate dies. */
                    if (latched != 0) {
                        uv = WinMaliUvSrcReacquireVa(latched, &srcPitch, &srcBytes);
                        if (uv != NULL) {
                            uvHandle = latched;
                            uvHits   = 1;   /* it qualified when it was latched */
                        } else {
                            /* Candidate freed under us - restart the sweep from
                               the top rather than splicing in a different one. */
                            (VOID)InterlockedExchange(&g_WinMaliUvLatchBo, 0);
                            (VOID)InterlockedIncrement(&g_WinMaliStatLatchDrop);
                            latched = 0;
                        }
                    }
                    if (uv == NULL) {
                        (VOID)InterlockedExchange(&g_WinMaliStatWorkerStage, 3);
                        uv = WinMaliUvSrcAcquireBest(bytes,
                                                     WINMALI_FB_STRIDE,
                                                     WINMALI_FB_HEIGHT,
                                                     &uvHandle, &uvHits, &srcPitch, &srcBytes);
                        (VOID)InterlockedExchange(&g_WinMaliStatUvSrcHits,
                                                  (LONG)uvHits);
                        uvHeld = (uv != NULL);
                    }
                    /* ⛔★★★★★★ HOLD THE LAST GOOD SOURCE — THIS IS THE STUTTER.
                     *
                     * USER-OBSERVED with PresentMarker=1: the marker "flashes
                     * quickly, then the render freezes, then continues - rinse and
                     * repeat". MEASURED, same cadence: `StatUvQual` alternates
                     * 1,0,1,0 tick to tick. On a qual=0 tick NOTHING qualifies,
                     * srcVa stays NULL, and the worker takes the `continue` below
                     * (StatSkipNoSrc) - so it paints nothing at all and the panel
                     * sits frozen until a candidate happens to qualify again.
                     *
                     * Skipping was the right call when the alternative was reading
                     * aperture memory we do not own. It is the WRONG call when we
                     * still hold a perfectly good probe-locked surface from the
                     * last sweep: a slightly stale frame is strictly better than a
                     * frozen one, and it is the same memory we were legitimately
                     * reading a moment ago.
                     *
                     * Prefer a fresh winner whenever there is one (this only runs
                     * when there is not), and re-resolve through the normal path so
                     * the use-ref and Dying checks still apply - if the surface has
                     * since been freed this yields NULL and we skip as before. */
                    if (uv == NULL) {
                        ULONG lastGood = (ULONG)InterlockedCompareExchange(
                                             &g_WinMaliUvLastGoodBo, 0, 0);
                        if (lastGood != 0) {
                            uv = WinMaliUvSrcReacquireVa(lastGood, &srcPitch, &srcBytes);
                            if (uv != NULL) {
                                uvHandle = lastGood;
                                uvHits   = 1;   /* it qualified when it was adopted */
                                (VOID)InterlockedIncrement(&g_WinMaliStatUvHeldOver);
                            } else {
                                (VOID)InterlockedExchange(&g_WinMaliUvLastGoodBo, 0);
                            }
                        }
                    }
                    if (uv != NULL && uvHits > 0) {
                        srcVa  = (PUCHAR)uv;
                        /* ⛔★★★★★ THIS IS THE SOURCE THAT IS ACTUALLY USED (3).
                         *
                         * I instrumented the two aperture branches (1 = VidMm,
                         * 2 = aperture-by-AllocId) and MISSED THIS ONE, because my
                         * grep was `srcVa = ` and this line is `srcVa  = ` with two
                         * spaces. StatDbgSrcPath then read 0 on a pass that had
                         * definitely copied - and the counter was RIGHT: neither
                         * instrumented branch had run.
                         * ★ When a well-formed counter reports something
                         * "impossible", suspect the MODEL before the counter. */
                        srcPath = 3;   /* 0x93 uv candidate, AcquireBest */
                        /* AcquireBest / ReacquireVa set srcBytes to the BO's
                           Size, and only return a candidate whose Size covers a
                           full screen AT ITS OWN PITCH. A proven bound, not an
                           assumption. */
                        srcReadable = srcBytes;
                        uvHandleForCopy = uvHandle;   /* re-checked between slices */
                        /* Whichever of AcquireBest/ReacquireVa produced `uv` took
                           a use-ref on this handle. Record it so it is dropped -
                           by the release after the copy, or failing that by the
                           sweep at the top of the next iteration. */
                        s_pendingUse = uvHandle;

                        /* ⛔★★★★★★ RELEASE THE TABLE LOCK NOW - DO NOT HOLD IT
                         * ACROSS THE COPY. THIS IS THE WEDGE.
                         *
                         * AcquireBest returns holding g_WinMaliUvSrcLock (a
                         * KMUTEX) and the old code released it at the END of the
                         * whole block - line ~2121 to ~2564, spanning the pixel
                         * sampler, the 2 MB copy, AND (as of today) five
                         * ZwFlushKey calls, each a synchronous DISK WRITE.
                         *
                         * The other user of that mutex is WinMaliUvSrcPublish,
                         * called from the **0x93 escape path** - i.e. from DWM and
                         * from every app registering a surface. So while this
                         * worker copies and flushes, DWM BLOCKS in its escape, and
                         * everything upstream of DWM blocks behind it.
                         *
                         * That is exactly the reported symptom: the EDK2 spinner
                         * FROZEN while our blue bar keeps flashing. The worker is
                         * alive because it OWNS the lock; the system is wedged
                         * because it does not. And my own flushed instrumentation
                         * widened the hold window, which is why the failure point
                         * kept moving (13 -> 20 -> 40 passes) - I was aggravating
                         * the bug I was measuring.
                         *
                         * Safe to release early: the pages behind srcVa are
                         * PROBE-LOCKED by the 0x93 BO (MmProbeAndLockPages) and
                         * stay resident until BoFree, so the copy does not need
                         * the table lock - only the SELECTION did.
                         *
                         * ⚠ Only AcquireBest returns holding the lock. The latched
                         * path came from WinMaliUvSrcReacquireVa, which takes and
                         * drops it internally - releasing again there would be an
                         * unowned release. Hence the uvHeld guard. */
                        if (uvHeld) {
                            WinMaliUvSrcRelease();
                            uvHeld = FALSE;
                        }
                        /* Latch this winner for the rest of the sweep. */
                        (VOID)InterlockedExchange(&g_WinMaliUvLatchBo,
                                                  (LONG)uvHandle);
                        /* Remember it beyond this sweep so a tick where nothing
                           qualifies can keep painting instead of freezing. */
                        (VOID)InterlockedExchange(&g_WinMaliUvLastGoodBo,
                                                  (LONG)uvHandle);
                        (VOID)InterlockedExchange(&g_WinMaliStatUvSrcBo,
                                                  (LONG)uvHandle);
                    } else if (uv != NULL) {
                        /* Candidates exist but none is painted - hand the lock
                           back and let the aperture branches try.
                           ⚠ A use-ref was still taken by whichever call produced
                           `uv`, and we are NOT adopting it, so drop it right here
                           - s_pendingUse is never set on this path and the sweep
                           would not know to. */
                        if (uvHeld) {
                            WinMaliUvSrcRelease();
                            uvHeld = FALSE;
                        }
                        if (uvHandle != 0) {
                            WinMaliUvSrcEndUse(uvHandle);
                        }
                    }
                }

                /* ★ PREFER THE ALLOCATION DWM LAST FLIPPED TO.
                 *
                 * MEASURED: DWM presents via the FLIP model, not Blt
                 * (present=24 / notblt=24 / blt=0), and dxgk latches a scanout
                 * address only TWICE, at mode-set - so `flipAddr` is frozen at
                 * the mode-set surface (0x10000000) and the aperture lookup
                 * below always resolves the same allocation, whose contents
                 * nothing ever writes (nonBlack=0 of 646, forever).
                 *
                 * WinMaliKmdPresent now records the allocation each flip targets.
                 * Copying from THAT is what actually puts DWM's composed frame
                 * on the panel. Fall through to the address-based lookup when no
                 * flip has been seen yet (e.g. before the first present). */
                {
                    PWINMALI_KMD_ALLOCATION fs =
                        (PWINMALI_KMD_ALLOCATION)InterlockedCompareExchangePointer(
                            (PVOID volatile *)&g_WinMaliFlipSourceAlloc, NULL, NULL);
                    if (fs != NULL &&
                        fs->Magic == WINMALI_KMD_ALLOC_MAGIC &&
                        fs->ApertureMdl != NULL) {
                        src = fs;
                    }
                }

                for (s = 0; src == NULL && s < RTL_NUMBER_OF(a->ApertureAllocs); ++s) {
                    PWINMALI_KMD_ALLOCATION k = a->ApertureAllocs[s];
                    if (k == NULL || k->ApertureMdl == NULL || k->GpuVa == 0) {
                        continue;
                    }
                    if (flipAddr >= k->GpuVa &&
                        flipAddr < k->GpuVa + (UINT64)k->AperturePageCount * PAGE_SIZE) {
                        src = k;
                        break;
                    }
                }

                /* ⚠ `srcVa == NULL &&` IS LOAD-BEARING: without it the aperture
                   branch overwrites a 0x93 source we already adopted above, and
                   we would silently go back to scanning out the empty surface. */
                /* ⛔★★★★★★ THIS IS AN APERTURE READ, NOT A 0x93 READ.
                 *
                 * The srcPath=2 label below says "0x93 probe-locked BO (uv
                 * candidate)". IT IS NOT. This branch maps `src->ApertureMdl` -
                 * a dxgk allocation's aperture MDL - which is the SAME class of
                 * memory as the VidMm branch above that is gated off with a
                 * six-paragraph warning saying it HANGS THE MACHINE. I believed
                 * the comment instead of the code and left the dangerous read
                 * wide open on the fallback path.
                 *
                 * MEASURED 1209: StatUvSrcTooSmall=404 of 501 candidates, so the
                 * uv selection FAILS most ticks and falls through to exactly this
                 * branch; StatDbgSrcPath=2 for the whole logged window. The run
                 * reached 203 full sweeps - far longer than any before - and still
                 * wedged. Every remaining wedge candidate points here.
                 *
                 * Same treatment as VidMm: gate it, default OFF. If no 0x93
                 * candidate qualifies we now SKIP the frame (the `srcVa == NULL`
                 * continue below) instead of reading memory we do not own.
                 * A skipped frame is a stale panel; an aperture read is a dead box.
                 * `FlipSrcFromAperture=1` re-enables it to re-test deliberately. */
                /* ★★★ PREFER THE REAL PRIMARY, opt-in via FlipSrcPreferAperture.
                 *
                 * MEASURED 1.1.9.77: the driver holds a mapped MDL for the ACTUAL
                 * primary and never reads it - FlipAllocOk=2, FlipAllocSize=21667840
                 * (exactly one frame), FlipWorkerMdl=1, FlipWorkerPages=5290,
                 * ApMapOps=24 - while StatDbgSrcPath=3 and FlipSrcUsed=0. The 0x93
                 * candidate we blit instead (bo=149, dwm, panel-sized) is FROZEN,
                 * although dwm composites on every mouse move (+12%% CPU, +26 submits).
                 *
                 * ⛔ `srcVa == NULL` alone can never express "prefer": the uv scan runs
                 * FIRST and almost always succeeds, so this branch was unreachable in
                 * practice. `src` is not resolved until a few lines above, which is why
                 * the preference has to be applied HERE and not before the uv scan -
                 * my first attempt sat above the assignment and read a NULL src.
                 *
                 * ⚠ Default 0 keeps today's exact behaviour. */
                /* ⛔★★★★★ THE 1.1.9.79 REBIND HANG WAS THIS CONDITION.
                 *
                 * .79 armed the read with `(FromAperture || PreferAperture)`, so
                 * PreferAperture alone BOTH overrode a validated 0x93 source AND
                 * opened the read that FlipSrcFromAperture exists to keep shut.
                 * Worse, it called MmGetSystemAddressForMdlSafe on VidMm's MDL
                 * from the worker ~20x/s - the exact operation the ApertureVa
                 * comment in WinMaliAlloc.h forbids BY NAME because doing it
                 * per-tick REBOOT-LOOPED this box once already. I wrote that
                 * warning and then walked straight into it.
                 *
                 * Now:
                 *  - FlipSrcFromAperture is the ONLY thing that can ARM the read.
                 *    PreferAperture expresses preference and nothing else, so
                 *    re-testing the real primary takes TWO deliberate knobs and
                 *    one knob can never open this again.
                 *  - Magic + MDL + the same size gate the VidMm sibling carries.
                 *    Undersized => not adopted => srcVa stays NULL => the counted
                 *    skip below (fail closed).
                 *  - The mapping is the CACHED ApertureVa, taken once where VidMm
                 *    guaranteed the MDL valid (WinMaliPaging.c MAP_APERTURE_SEGMENT)
                 *    and NULLed BEFORE ApertureMdl at UNMAP. We never map here.
                 *  - KeFlushIoBuffers first: a CACHED mapping of memory the GPU
                 *    wrote non-coherently reads stale (black) without it. */
                if ((srcVa == NULL || g_WinMaliFlipSrcPreferAperture != 0) &&
                    g_WinMaliFlipSrcFromAperture != 0 &&
                    src != NULL &&
                    src->Magic == WINMALI_KMD_ALLOC_MAGIC &&
                    src->ApertureMdl != NULL &&
                    (SIZE_T)src->AperturePageCount * PAGE_SIZE >= bytes) {
                    PUCHAR base = (PUCHAR)src->ApertureVa;
                    if (base != NULL) {
                        if (src->ApertureMdl != NULL) {
                            KeFlushIoBuffers(src->ApertureMdl, TRUE, TRUE);
                        }
                        /* Byte offset into the allocation. For an address-matched
                           allocation that is (flipAddr - GpuVa); for a
                           flip-recorded one the frame starts at the beginning of
                           the allocation and flipAddr is unrelated to it (it is
                           still the frozen mode-set address), so use 0. */
                        SIZE_T apBytes = (SIZE_T)src->AperturePageCount * PAGE_SIZE;
                        SIZE_T off = 0;
                        if (src->GpuVa != 0 &&
                            flipAddr >= src->GpuVa &&
                            flipAddr < src->GpuVa + (UINT64)apBytes) {
                            off = (SIZE_T)(flipAddr - src->GpuVa);
                        }
                        if (off >= apBytes) { off = 0; }   /* cannot happen; do not trust it */
                        srcVa = base + (SIZE_T)src->ApertureMdlOffset * PAGE_SIZE + off;
                        /* srcVa was ADVANCED by off, so what remains readable
                           from it is the run MINUS off. Getting this wrong is
                           how .79 handed the copy loop a 21 MB budget over a
                           mapping that no longer had 21 MB in front of it. */
                        srcReadable = apBytes - off;
                        srcPath = 2;   /* APERTURE MDL of a dxgk allocation (gated) */
                        /* ⛔ RESET THE SOURCE METADATA. Until now this branch only ran
                           when the uv scan had FAILED, so srcPitch/srcBytes/
                           uvHandleForCopy were all still zero and nobody noticed they
                           are not set here. With FlipSrcPreferAperture we OVERRIDE a
                           successful uv result, so leaving them behind would apply one
                           surface's pitch and length to a completely different
                           allocation - and make the copy loop re-validate a uv handle
                           that is no longer the source it is reading. */
                        srcPitch        = WINMALI_FB_STRIDE;
                        srcBytes        = srcReadable;   /* never the raw run */
                        uvHandleForCopy = 0;
                        (VOID)InterlockedIncrement(&g_WinMaliStatFlipSrcUsed);
                    }
                }

                if (srcVa == NULL) {
                    /* No aperture allocation covers this address (or the MDL
                       could not be mapped). Do not fall back to DmaSegmentVa -
                       that is exactly the bug being fixed.
                       ★ With the aperture branch gated off this is now the NORMAL
                       outcome whenever no 0x93 candidate qualifies (404 of 501
                       were under-size last run), so count it: a huge SkipNoSrc
                       with a healthy StatSweeps means we are simply idling
                       between good frames, which is the intended behaviour. */
                    (VOID)InterlockedIncrement64(&a->ScanoutBltSkipped);
                    (VOID)InterlockedIncrement(&g_WinMaliStatSkipNoSrc);
                    continue;
                }

                /* ⛔★★★★★ ESTABLISH THE BOUND ONCE, HERE, AND FAIL CLOSED.
                 *
                 * Every reader below - both samplers, the copy loop and the
                 * thumbnail - used to walk the source with WINMALI_FB_HEIGHT /
                 * WINMALI_FB_STRIDE, or with `bytes`, the DESTINATION size. None
                 * of them could ever be NARROWED by a source smaller than the
                 * panel; that is how a 4 MB atlas became a 21 MB read off the
                 * end of a mapping. These three values are computed HERE and
                 * nowhere else, and the copy block below CONSUMES them rather
                 * than recomputing its own.
                 *
                 * If srcReadable is 0 the extent is unknown, and unknown means
                 * we do not read: skip the frame, counted, exactly as a missing
                 * source already does. A stale panel is a cosmetic miss; an
                 * unmapped kernel read on this box is a dead box with no dump.
                 *
                 * srcMaxRows is capped by WINMALI_FB_HEIGHT because that is the
                 * DESTINATION's limit - the old code started maxRows there and
                 * only ever lowered it, so this is behaviour-preserving for the
                 * 0x93 path (selection guarantees Size >= Height*pitch, so the
                 * cap still lands on 1840 exactly as before). */
                srcRowPitch = (srcPitch != 0) ? srcPitch : WINMALI_FB_STRIDE;
                srcRowBytes = (srcRowPitch < WINMALI_FB_STRIDE)
                            ? srcRowPitch : WINMALI_FB_STRIDE;
                srcMaxRows  = (srcReadable >= (SIZE_T)srcRowBytes)
                            ? (ULONG)(((srcReadable - (SIZE_T)srcRowBytes)
                                       / srcRowPitch) + 1)
                            : 0;
                if (srcMaxRows > WINMALI_FB_HEIGHT) {
                    srcMaxRows = WINMALI_FB_HEIGHT;
                }
                if (srcReadable == 0 || srcMaxRows == 0) {
                    (VOID)InterlockedIncrement64(&a->ScanoutBltSkipped);
                    (VOID)InterlockedIncrement(&g_WinMaliStatSkipNoSrc);
                    continue;      /* no bound => no read */
                }

                /* ⛔★★★★★ WHERE DOES THE SOURCE'S CONTENT END?
                 *
                 * THE ONE MEASUREMENT THAT SPLITS THE REMAINING HYPOTHESIS.
                 * The user reports the panel filling to ~50% and no further, on
                 * build after build. MEASURED 1209: the copy loop writes all
                 * 21,667,840 bytes (StatDbgPass==StatDbgDone, StatCopyAbort=0,
                 * StatSweeps=203 complete frames), and PresentTestPattern=1 paints
                 * clean bars over the WHOLE panel without ever touching srcVa.
                 * So the destination and the copy are both proven good for the
                 * full extent, and the deficit must be in what the SOURCE holds.
                 *
                 * Walk the source top to bottom and record the LAST row that has
                 * any non-black sample:
                 *   StatSrcLastRow  ~920  => the source is half-painted. The 50%
                 *                            is DWM's surface, not our copy, and
                 *                            the fix is upstream in selection.
                 *   StatSrcLastRow ~1840  => the source is full and something
                 *                            downstream eats the bottom half.
                 * StatSrcRowsNZ counts how many sampled rows had content, so a
                 * sparse/striped source cannot masquerade as a full one, and
                 * StatSrcFirstRow catches an offset (content starting mid-buffer).
                 *
                 * ⚠ ONLY on srcPath 3 - pages WE probe-locked. Walking an aperture
                 * or VidMm mapping is the deadlock this driver already has two
                 * knobs to prevent. Every 120th tick; 1840/8 rows x 16 columns is
                 * ~3.7k reads, negligible beside the 2 MB copy it sits next to. */
                /* ⚠ THIS WAS `srcPath == 3` ONLY - and that gate meant the ONE path
                 * that sees DWM's ACTUAL FLIP TARGET was the one path we refused
                 * to measure, so its extent had to be eyeballed off the panel.
                 *
                 * The gate existed because reading an aperture/VidMm mapping was
                 * believed to deadlock the worker. REFUTED 2026-08-17: with the
                 * BoFree/system-VA race fixed, FlipSrcFromVidMm=1 ran with the
                 * marker updating rapidly and no hang. That old verdict was the
                 * BoFree race in disguise - the aperture path had survived 203
                 * sweeps while the 0x93-only build died in 2 redraws, the opposite
                 * of what a deadlocking read predicts.
                 * ⇒ Measure whichever source we actually adopted, and publish
                 * WHICH one it was so the extent is never attributed to the wrong
                 * buffer. */
                if (srcVa != NULL) {
                    static LONG64 s_extTick = 0;
                    /* ⛔★★★★★ WALK THE SOURCE AT THE SOURCE'S PITCH.
                     *
                     * This probe used WINMALI_FB_STRIDE - the PANEL pitch - to
                     * step rows in a buffer whose real pitch is something else.
                     * MEASURED: the source is 2976x1856 at pitch 11904, not
                     * 2944x1840 at 11776, so every "row" this probe looked at was
                     * offset from a real one and its answer (StatSrcLastRow=984)
                     * described the probe's own striding, not the content.
                     * ★ A measurement taken with the wrong geometry is not a
                     * weaker measurement, it is a different question. */
                    const ULONG probePitch = srcRowPitch;
                    if ((InterlockedIncrement64(&s_extTick) % 120) == 1) {
                        ULONG y, x, first = 0xFFFFFFFFu, last = 0, rowsNz = 0;
                        /* Rows and columns come from THE BOUND, never from
                           `bytes` (the destination) or the panel height. The
                           old column step (probePitch/64) also produced a ZERO
                           step for any pitch < 64; 16 samples per row is what it
                           actually intended. */
                        ULONG probeRows = srcMaxRows;
                        ULONG probeCols = srcRowBytes / 4u;
                        ULONG colStep   = probeCols / 16u;
                        if (colStep == 0) { colStep = 1; }
                        for (y = 0; y < probeRows; y += 8) {
                            const ULONG *row = (const ULONG *)
                                WinMaliSrcRow_(srcVa, srcMaxRows, srcRowPitch, y);
                            BOOLEAN nz = FALSE;
                            if (row == NULL) { break; }
                            for (x = 0; x < probeCols; x += colStep) {
                                if ((row[x] & 0x00FFFFFFu) != 0) { nz = TRUE; break; }
                            }
                            if (nz) {
                                rowsNz++;
                                last = y;
                                if (first == 0xFFFFFFFFu) { first = y; }
                            }
                        }
                        if (first == 0xFFFFFFFFu) { first = 0; }
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcLastRow",  last);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcFirstRow", first);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcRowsNZ",   rowsNz);
                        /* WHICH source the extent above describes: 1=VidMm flip
                           target (DWM's real primary), 2=aperture, 3=0x93 layer.
                           Without it the numbers get attributed to the wrong
                           buffer, which is how "52%" crossed paths by eye. */
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcPathMeas", srcPath);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcProbePitch", probePitch);
                    }
                }

                /* Prove the source now has content. Same sparse grid as the
                   sampler that found the bug, so the numbers are comparable:
                   it read nonBlack=0 out of 646 from DmaSegmentVa. */
                {
                    static LONG64 s_srcTick = 0;
                    /* ★ LIVENESS FOR *THIS* SOURCE PATH.
                     *
                     * StatUvSrcChg is written in WinMaliUvSelectSrc (WinMaliBo.c),
                     * i.e. the 0x93 user-VA heuristic. The worker now prefers
                     * dxgk's own flip allocation, and Blts == FlipSrcUsed on every
                     * tick proves that branch wins every time - so UvSelectSrc is
                     * never called and StatUvSrcChg sits FROZEN at a stale value.
                     * Reading it as "the surface never repaints" is reading a
                     * counter on a branch we no longer take.
                     *
                     * This hash rides the sampler that is already looking at the
                     * bytes we actually copy, so it answers the real question:
                     * does the content of the scanned surface change over time?
                     * One value name, ONE writer - published nowhere else. */
                    static ULONG   s_srcHash    = 0;
                    static ULONG   s_srcChg     = 0;
                    static BOOLEAN s_haveSrcHash = FALSE;
                    if ((InterlockedIncrement64(&s_srcTick) % 120) == 1) {
                        ULONG orAll = 0, nonBlack = 0, sampled = 0, y, x;
                        ULONG srcHash = 2166136261u;
                        /* ★ WHERE is the content? The user reports ~15% on the
                         * RIGHT side with the taskbar entirely absent, which is a
                         * very different bug from "content is scattered". Build a
                         * 4x4 occupancy bitmask (bit = 4*row + col, row 0 = top,
                         * col 0 = left) so the DRIVER says which cells are
                         * painted instead of us inferring it from a description.
                         * A right-hand strip lights columns 2-3 only; a stride
                         * error lights a diagonal; correct-but-dark lights the
                         * cells that actually hold UI. */
                        ULONG cellMask = 0;
                        /* ★ EDGES, NOT JUST nonBlack. `nonBlack` cannot tell a
                         * composed desktop from a SOLID WHITE buffer - both score
                         * 646/646 - and a white panel therefore reads as a
                         * perfect result. Transitions between adjacent samples
                         * score 0 for any flat fill, whatever its brightness.
                         * Both are published; edges is the honest one. */
                        ULONG edges = 0, prevPx = 0;
                        BOOLEAN havePrev = FALSE;
                        /* ⛔ Walk at the SOURCE's pitch, bounded by THE BOUND.
                           The hard-coded WINMALI_FB_HEIGHT/WINMALI_FB_STRIDE
                           here was the single worst reader in the worker: it
                           walked a mapping 21 MB deep with no source bound of
                           any kind. Cell divisors stay panel-relative so the
                           published mask keeps its meaning. */
                        ULONG sampCols = srcRowBytes / 4u;
                        if (sampCols > WINMALI_FB_WIDTH) { sampCols = WINMALI_FB_WIDTH; }
                        for (y = 0; y < srcMaxRows; y += 97) {
                            const ULONG *row = (const ULONG *)
                                WinMaliSrcRow_(srcVa, srcMaxRows, srcRowPitch, y);
                            if (row == NULL) { break; }
                            for (x = 0; x < sampCols; x += 89) {
                                ULONG px = row[x] & 0x00FFFFFFu;
                                orAll |= px;
                                if (px != 0) {
                                    ULONG cr = (y * 4u) / WINMALI_FB_HEIGHT;
                                    ULONG cc = (x * 4u) / WINMALI_FB_WIDTH;
                                    nonBlack++;
                                    cellMask |= (1u << ((cr * 4u) + cc));
                                }
                                if (havePrev && px != prevPx) { edges++; }
                                prevPx   = px;
                                havePrev = TRUE;
                                /* Order-sensitive, so a permutation of the same
                                   pixels still registers as a change. */
                                srcHash = (srcHash * 33u) ^ px;
                                sampled++;
                            }
                        }
                        if (s_haveSrcHash && srcHash != s_srcHash) { s_srcChg++; }
                        s_srcHash     = srcHash;
                        s_haveSrcHash = TRUE;
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcEdges", (ULONG)edges);
                        /* Tagged with bit31 so it can never be confused with the
                         * value this name used to carry (see the two-writer and
                         * repurposing traps that already cost a session). */
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcCellMask", 0x80000000u | cellMask);
                        /* flipLatch / visible / visibleEver are the ONLY honest
                           evidence about SetVidPnSourceAddress and
                           SetVidPnSourceVisibility: both DDIs run at DIRQL and
                           cannot log, and the StopDevice publish only fires if
                           the instance "did something" (it produced nothing but
                           <absent> when queried). Carrying them on the present
                           worker's own line makes them readable continuously,
                           on a path that is already running.

                           This is the open question: flip has never moved off
                           0x10000000 (== our aperture alloc's own GpuVa) even
                           now that OpenAllocation resolves 100%, so either dxgk
                           never latches an address (flipLatch stays 0) or it
                           latches and the source is never made visible. */
                        WINMALI_TRACE("PRESENT-SRC: alloc=%p gpu_va=0x%llx "
                                      "flip=0x%llx sampled=%lu nonBlack=%lu "
                                      "OR=0x%06x blits=%lld skipped=%lld "
                                      "flipLatch=%lld visible=%ld everVisible=%ld "
                                      "present=%ld/notblt=%ld/nullcookie=%ld/blt=%ld/flip=%ld "
                                      "uvBo=%ld uvHits=%ld",
                                      src, (ULONGLONG)(src ? src->GpuVa : 0),
                                      (ULONGLONG)flipAddr, sampled, nonBlack,
                                      orAll, a->ScanoutBltCount,
                                      a->ScanoutBltSkipped,
                                      a->FlipLatchCount, a->SourceVisible,
                                      a->SourceVisibleEver,
                                      g_WinMaliStatPresentCalls,
                                      g_WinMaliStatPresentNotBlt,
                                      g_WinMaliStatPresentNullCookie,
                                      g_WinMaliStatPresentBlts,
                                      g_WinMaliStatPresentFlips,
                                      g_WinMaliStatUvSrcBo,
                                      g_WinMaliStatUvSrcHits);

                        /* ★ PUBLISH TO THE REGISTRY TOO.
                         *
                         * The EDK2 ring is FILL-ONCE: it stops dead at 262,112
                         * bytes and then accepts nothing, so once it is full
                         * this trace can never be seen again - and it can still
                         * hold a PREVIOUS boot's lines, which made a live fix
                         * read as dead twice. Registry values are wrap-proof and
                         * stale-proof, and this runs at PASSIVE_LEVEL on a
                         * once-per-120-blits path, so the cost is irrelevant.
                         * These are the numbers that answer whether the flip
                         * path works: nonBlack > 0 and flip > 0 is the win. */
                        /* ⛔⛔ DO NOT REPUBLISH THE PRESENT COUNTERS HERE.
                         *
                         * StatPresentCalls/NotBlt/NullCookie/Blts/Flips are
                         * ALREADY published by the block at the top of the
                         * worker loop. Writing them again from this sampler made
                         * the LAST writer win, so anything changed up there was
                         * silently reverted before it could ever be read. That
                         * invalidated a whole afternoon of measurements - an
                         * aperture sweep, an adoption result, and a "_Present is
                         * never called" proof ALL decoded to the old values,
                         * because each rode on one of these five names.
                         *
                         * RULE: one value name, ONE writer. StatSrcNonBlack and
                         * StatSrcSampled stay because they are computed here and
                         * published nowhere else. */
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcNonBlack", (ULONG)nonBlack);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcSampled", (ULONG)sampled);
                        /* Scans is the channel proof: if StatSrcScans climbs but
                           StatSrcChg does not, the surface really is static. If
                           Scans is <absent> or frozen, this sampler never ran and
                           NEITHER number means anything (the Parameters key caps
                           at 127 values and silently drops new names). */
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcChg", (ULONG)s_srcChg);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcScans", (ULONG)(ULONG64)s_srcTick);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                L"StatSrcHash", (ULONG)srcHash);
                    }
                }

                /* ⛔★★★★★ DO NOT HOLD A 21 MB memcpy OVER MEMORY VidMm OWNS.
                 *
                 * This used to be one flat RtlCopyMemory of ~21 MB. srcVa is a
                 * system VA mapped over VidMm's aperture MDL - memory ANOTHER
                 * COMPONENT is free to evict, unmap or repurpose. A single copy
                 * of that size takes many milliseconds, and if the allocation
                 * moves underneath us mid-copy the CPU faults or stalls with the
                 * machine wedged.
                 *
                 * USER-OBSERVED, and this is what identified it: when armed, the
                 * panel paints SLOWLY, leaving a band of bright lines across the
                 * TOP and black below - then the box HANGS. It did this more than
                 * once and **painted LESS the second time**. A flat copy fills the
                 * framebuffer ROW-MAJOR TOP-DOWN, so that band is literally HOW
                 * FAR THE COPY GOT before the machine died: taller = died later.
                 * ⇒ A symptom whose MAGNITUDE varies between runs is a PROGRESS
                 * INDICATOR, not a geometry error. (I twice mis-read it as a
                 * stride bug, then as a crash-time artifact.)
                 *
                 * So: copy in slices, and between slices re-check that the
                 * allocation we started from still holds the SAME mapping. If it
                 * moved, abort and COUNT it - a partial frame is a cosmetic miss,
                 * a wedged machine costs a spare-install recovery. */
                {
                    /* ⛔★★★★★ NEVER PUSH THE WHOLE FRAME IN ONE WAKE.
                     *
                     * With the scanout mapped UNCACHED a full 21 MB frame is a
                     * MULTI-SECOND sustained write burst into the very memory the
                     * DSI controller is scanning out. Every armed run so far has
                     * frozen mid-paint, and the user reports it stops at NEARLY
                     * THE SAME PLACE each time - a constant offset at a constant
                     * copy rate means a constant ELAPSED TIME, i.e. something with
                     * a timeout fires. It is not a fault at a bad address (that
                     * would stop somewhere different every run).
                     *
                     * So bound the burst: copy at most WINMALI_COPY_BUDGET bytes
                     * per wake and resume from s_copyOff next time. The frame
                     * still completes, just spread over several wakes, and the
                     * interconnect is never monopolised for seconds at a stretch.
                     * This is BOTH the mitigation and the experiment: if bounding
                     * the burst stops the hang, the burst was the cause. */
                    /* ⛔★★★★★★ THE 2 MB THROTTLE IS WHY WE LOSE THE SOURCE.
                     *
                     * A full frame at 2 MB/wake takes ~11 wakes. MEASURED on the
                     * lock screen: Stat0x93Calls climbs ~3.4/s (a FRESH surface
                     * registered several times a second), StatLatchDrop climbs in
                     * step, and StatUvHeldOver stayed 0 - on every tick where
                     * nothing qualified, the previous source was ALSO already
                     * freed. DWM/LogonUI compose into short-lived buffers and
                     * destroy them, so we cannot finish an 11-wake copy inside one
                     * buffer's lifetime: we paint part of a frame, lose it, skip,
                     * repeat. USER-OBSERVED as "flash, render, freeze, rinse and
                     * repeat".
                     *
                     * The throttle was added when a full-frame copy appeared to
                     * cause hangs - but that hang was later ROOT-CAUSED to BoFree
                     * unmapping the system VA mid-copy (fixed: Revoke runs first
                     * and blocks on a use-ref). The reason for it is gone; the
                     * cost is not.
                     *
                     * Knob, so this is tunable without a rebuild and the old
                     * behaviour is one registry write away:
                     *   PresentCopyBudgetKB absent/0 -> whole frame in one wake
                     *   PresentCopyBudgetKB = 2048   -> the old 2 MB throttle
                     * ⚠ This is an EXPERIMENT with a prediction, not an assumption
                     * that bigger is better: StatLatchDrop should fall toward 0.
                     * If it stays high, the surface dies faster than a single copy
                     * takes and the fix is elsewhere entirely. */
                    SIZE_T budget = (g_WinMaliPresentCopyBudgetKB != 0)
                                  ? ((SIZE_T)g_WinMaliPresentCopyBudgetKB * 1024u)
                                  : bytes;
                    /* ⛔★★★★★★ COPY ROW BY ROW AT THE SOURCE'S OWN PITCH.
                     *
                     * This was a FLAT byte copy: dst[o] = src[o] for o over the
                     * whole frame. That is only correct when the source pitch
                     * equals the panel pitch - and MEASURED, it does not. DWM and
                     * LogonUI register 2946x1842 surfaces with stride 24576
                     * (their own mesa log says so), which passes the only gate
                     * there is (Size 22,093,824 >= frame 21,667,840) while being
                     * a different shape entirely. Copied flat at 11776 the image
                     * is squeezed into the top of the panel: measured
                     * StatSrcLastRow=984 of 1840, i.e. the ~50% fill seen on the
                     * glass all session.
                     *
                     * ⚠ CLAMP THE ROW COUNT BY WHAT THE SOURCE CAN SUPPLY. At a
                     * pitch wider than the panel's, a full 1840 rows needs far
                     * more memory than the buffer holds, and walking off the end
                     * of a probe-locked MDL is an uncatchable fault - the exact
                     * class of bug that has been killing this box. We only KNOW
                     * the source is at least `bytes` long (selection guarantees
                     * Size >= NeedBytes), so bound rows by that and never by the
                     * surface's own claimed height. */
                    /* ⛔★★★★★ THE COPY NO LONGER COMPUTES ITS OWN BOUND.
                     * `avail = (srcBytes > bytes) ? srcBytes : bytes` was a MAX:
                     * a source SHORTER than the panel selected `bytes`, the
                     * DESTINATION size, so maxRows stayed 1840 and the loop read
                     * 21,667,840 bytes out of whatever `src` happened to be.
                     * These three now come from the single bound established at
                     * source-adoption time, and they are const so no later edit
                     * can quietly widen them. */
                    const ULONG  dstPitch = WINMALI_FB_STRIDE;
                    const ULONG  effPitch = srcRowPitch;
                    const ULONG  rowBytes = srcRowBytes;
                    const ULONG  maxRows  = srcMaxRows;
                    static ULONG s_copyRow = 0;
                    BOOLEAN      restartSweep = FALSE;
                    ULONG        rowsPerWake = (ULONG)(budget / dstPitch);
                    ULONG        startRow, endRow, r, doneRows = 0;
                    if (rowsPerWake == 0) { rowsPerWake = 1; }
                    startRow = (s_copyRow < maxRows) ? s_copyRow : 0;
                    endRow   = startRow + rowsPerWake;
                    if (endRow > maxRows) { endRow = maxRows; }
                    /* Kept so the existing forensics below read unchanged. */
                    SIZE_T start = (SIZE_T)startRow * dstPitch;
                    SIZE_T limit = (SIZE_T)(endRow - startRow) * dstPitch;
                    SIZE_T done  = 0;
                    PWINMALI_KMD_ALLOCATION ga = (PWINMALI_KMD_ALLOCATION)
                        InterlockedCompareExchangePointer(
                            (PVOID volatile *)&g_WinMaliFlipAlloc, NULL, NULL);
                    PVOID gaVa = (ga != NULL) ? ga->ApertureVa : NULL;

                    /* ⛔★★★★★ FORENSIC BREADCRUMBS - FLUSHED, BOUNDED.
                     *
                     * The only symptom of this bug is a HANG, so every previous
                     * armed boot cost a full recovery cycle and returned ONE bit
                     * of information. Worse, lazily-written counters die with the
                     * box: StatPresentSettleMs was definitely written and still
                     * read <absent> afterwards.
                     *
                     * So before touching the source at all, commit WHAT WE ARE
                     * ABOUT TO DO, flushed to disk:
                     *   StatDbgPass    - which pass this is (did we get here?)
                     *   StatDbgSrcPath - 1 = VidMm aperture MDL, 2 = 0x93 probe-
                     *                    locked, 0 = neither (should not copy)
                     *   StatDbgSrcLo   - low 32 bits of srcVa, so a bad/blank VA
                     *                    is visible rather than inferred
                     *   StatDbgBytes   - how much this pass intends to copy
                     * Then StatCopyKB below records how far it ACTUALLY got.
                     *
                     * First 8 passes only: ZwFlushKey is expensive and the box
                     * dies within seconds, so 8 is ample and cannot itself become
                     * the performance problem. */
                    {
                        static LONG s_dbgPass = 0;
                        LONG pass = InterlockedIncrement(&s_dbgPass);
                        /* ⚠ CAP RAISED 8 -> 40. The user COUNTED 13 blue flashes
                         * before the freeze (one flash = one 2 MB pass; a full
                         * 21,667,840 B frame is ~11 passes). With the cap at 8,
                         * every logged pass completed and the hang happened past
                         * the end of the record - the run cost a recovery cycle
                         * and could not say where it died. 40 covers ~3.6 frames.
                         * ⇒ Size a bounded log to the OBSERVED failure point, not
                         * to a guess about how fast it will fail. */
                        if (pass <= 40) {
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatDbgPass", (ULONG)pass);
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatDbgSrcPath", srcPath);   /* set AT the assignment */
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatDbgSrcLo", (ULONG)((ULONG_PTR)srcVa & 0xFFFFFFFFu));
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatDbgBytes", (ULONG)limit);
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatDbgOff", (ULONG)(start >> 10));  /* KB into the frame */
                        }
                    }

                    (VOID)InterlockedExchange(&g_WinMaliStatWorkerStage, 4);
                    for (r = startRow; r < endRow; r++) {

                        /* ⛔★★★★★ TEARDOWN MUST NOT WAIT FOR THIS COPY.
                         *
                         * StopDevice does KeWaitForSingleObject on this worker
                         * thread. With the scanout mapped UNCACHED a frame takes
                         * ~20 s, so Disable-PnpDevice blocked for that long, PnP
                         * timed out, and the machine HUNG - twice on 2026-08-17,
                         * both times inside deploy_hot's adapter bounce, which
                         * then cost a spare-install recovery each.
                         *
                         * A half-painted frame is cosmetic. A teardown that
                         * cannot complete takes the box down. Bail immediately
                         * when asked to stop. */
                        if (InterlockedCompareExchange(&a->PresentStop, 1, 1) == 1) {
                            (VOID)InterlockedIncrement(&g_WinMaliStatCopyAbort);
                            break;
                        }

                        /* ⛔★★★★★ RE-VALIDATE THE uv CANDIDATE BETWEEN SLICES.
                         *
                         * We release the table lock right after selection (holding
                         * it across the copy wedges DWM, which needs the same mutex
                         * from the 0x93 escape). That leaves a window where BoFree
                         * can MmUnlockPages the very buffer we are reading.
                         *
                         * MEASURED 1207: StatDbgPass=18 / StatDbgDone=17 - died
                         * INSIDE pass 18, with StatUvSrcHits=0 and StatUvSrcBo=0:
                         * the candidate had gone away underneath the copy. I had
                         * traded a wedge for a use-after-unlock.
                         *
                         * A SHORT re-check between 256 KB slices keeps the mutex
                         * hold tiny (selection only) while making a vanished
                         * candidate an aborted frame instead of a dead machine. */
                        if (srcPath == 3 && uvHandleForCopy != 0 &&
                            !WinMaliUvSrcStillValid(uvHandleForCopy)) {
                            (VOID)InterlockedIncrement(&g_WinMaliStatCopyAbort);
                            (VOID)InterlockedExchange(&g_WinMaliStatWorkerStage, 42);
                            /* ★ RESTART THE SWEEP, don't resume it. Resuming would
                               fill the remainder of this frame from a DIFFERENT
                               surface - the mosaic we are removing. Drop the latch
                               and rewind so the next wake re-selects and starts a
                               clean frame from the top. */
                            (VOID)InterlockedExchange(&g_WinMaliUvLatchBo, 0);
                            restartSweep = TRUE;
                            break;   /* candidate freed mid-copy - drop this frame */
                        }

                        /* Re-validate between slices. Only meaningful when the
                           source came from the flip allocation; for other source
                           paths ga is NULL and we simply copy through. */
                        /* ⛔★★★★★ GUARD ONLY THE PATH IT PROTECTS.
                         *
                         * The comment above says "for other source paths ga is
                         * NULL and we simply copy through". That is FALSE in
                         * practice: `ga` is read from g_WinMaliFlipAlloc
                         * unconditionally, and dxgk HAS latched a flip
                         * (LiveFlipLatch=2), so ga is non-NULL even when the
                         * source we are actually copying is a 0x93 candidate
                         * (srcPath 3) that has nothing to do with it. And under
                         * escape submission dxgk never aperture-maps that
                         * allocation, so ga->ApertureMdl is NULL FOREVER - which
                         * made this abort fire on the FIRST row of EVERY tick.
                         *
                         * MEASURED 1.1.9.30: StatCopyAbort +210 per 10 s (one per
                         * tick), StatCopyKB=0, while srcPath=3, StatUvCoverage=99
                         * and StatDbgBytes=21667840 - i.e. a full, 99%-painted
                         * desktop was selected and then discarded before a single
                         * byte was copied. On the glass: the marker square
                         * flashing over an untouched EDK2 boot image, because the
                         * marker is stamped after the copy loop regardless.
                         *
                         * srcVa for path 3 is a system VA over pages WE
                         * probe-locked; its liveness is covered by the
                         * WinMaliUvSrcStillValid check above. This one is only
                         * meaningful for the flip-allocation paths (1 and 2).
                         * ★ A GUARD MUST BE SCOPED TO THE THING IT GUARDS - an
                         *   unscoped one silently disables a path it never
                         *   examined. */
                        if (ga != NULL && srcPath != 3) {
                            PWINMALI_KMD_ALLOCATION now =
                                (PWINMALI_KMD_ALLOCATION)InterlockedCompareExchangePointer(
                                    (PVOID volatile *)&g_WinMaliFlipAlloc, NULL, NULL);
                            if (now != ga ||
                                ga->Magic != WINMALI_KMD_ALLOC_MAGIC ||
                                ga->ApertureMdl == NULL ||
                                ga->ApertureVa != gaVa) {
                                (VOID)InterlockedIncrement(&g_WinMaliStatCopyAbort);
                                (VOID)InterlockedExchange(&g_WinMaliStatWorkerStage, 41);
                                break;   /* leave the rest of the frame stale */
                            }
                        }

                        /* ⛔★★★★★ COPY AT `start + done`, NOT `done`.
                         *
                         * This read `ScanoutVa + done` / `srcVa + done`, ignoring
                         * `start` entirely - so every pass rewrote the SAME first
                         * 2 MB no matter what s_copyOff said. */
                        /* ★ EACH ROW AT ITS OWN PITCH ON EACH SIDE. dst advances
                           by the panel pitch, src by the SOURCE's pitch. When the
                           two are equal this is byte-for-byte what the old flat
                           copy did, so a UMD that reports no pitch is unaffected. */
                        {
                            PUCHAR srcRow =
                                WinMaliSrcRow_(srcVa, maxRows, effPitch, r);
                            if (srcRow == NULL) {
                                /* Unreachable while r < endRow <= maxRows, and
                                   that is the point: if a future edit breaks the
                                   invariant we stop and COUNT it instead of
                                   reading an unmapped VA. */
                                (VOID)InterlockedIncrement(&g_WinMaliStatCopyAbort);
                                break;
                            }
                            RtlCopyMemory((PUCHAR)WinMaliWorkerDst_(a) +
                                              (SIZE_T)r * dstPitch,
                                          srcRow, rowBytes);
                        }
                        doneRows++;
                        done += dstPitch;
                    }

                    /* ⛔★★★★★★ ADVANCE THE CURSOR. THIS WAS MISSING AND IT IS MY BUG.
                     *
                     * `start = s_copyOff` was read every pass but s_copyOff was
                     * NEVER updated, so the worker re-copied the first 2 MB of the
                     * frame forever - about 178 rows, the top ~10% of the panel.
                     *
                     * MEASURED (1205): StatDbgOff = 0 on pass 40, StatDbgPass =
                     * StatDbgDone = 40. Forty successful copies, all of the same
                     * strip.
                     *
                     * ⇒ THAT is the "flashing blue bar at the top": not a partial
                     * frame, not a progress indicator, not a stride error - the
                     * only region we ever wrote, repainted ~11x/s. I read that band
                     * as evidence for three different theories today (a 4 MB
                     * surface at the wrong pitch, a copy dying partway, then
                     * boot-time contention) when it was a missing `+=`.
                     * ★ A symptom that survives every hypothesis you throw at it
                     * deserves suspicion of the instrument, not another theory. */
                    s_copyRow = restartSweep ? 0 : (startRow + doneRows);
                    if (s_copyRow >= maxRows) {
                        s_copyRow = 0;                           /* wrap to the top */
                        /* ★ SWEEP COMPLETE - DROP THE LATCH so the next frame
                           re-runs the contest and picks up whatever DWM has
                           composed since. Holding the latch across frames would
                           freeze the panel on one surface forever; dropping it
                           mid-frame is what produced the mosaic. The latch's
                           lifetime is EXACTLY one frame, by construction. */
                        (VOID)InterlockedExchange(&g_WinMaliUvLatchBo, 0);
                        (VOID)InterlockedIncrement(&g_WinMaliStatSweeps);
                        (VOID)InterlockedExchange(&g_WinMaliStatWorkerStage, 5);

                        /* ★★★ COMPOSITE THE TASKBAR STRIP OVER THE FINISHED FRAME.
                         *
                         * The desktop copy above is ONE candidate. dwm publishes
                         * the taskbar as a SEPARATE full-width partial-height
                         * layer (measured: bo=344, 414 pages @ pitch 11776 =
                         * exactly 2944x144), which AcquireBest correctly rejects
                         * as "not the screen" - and which we then threw away.
                         * That is the whole reason the panel has never shown a
                         * taskbar: not a missing surface, not a bad selection,
                         * we simply never drew the pixels we already had.
                         *
                         * Done HERE, at sweep completion, so it lands on top of a
                         * COMPLETE frame rather than being overwritten by the
                         * remaining slices of a partial one. Bottom-anchored:
                         * that is where the strip belongs on a default Windows
                         * desktop, and a wrong guess is visible immediately
                         * rather than silent.
                         *
                         * ⚠ If this ever shows the strip in the wrong place, the
                         * fix is to carry the layer's Y offset through the 0x93
                         * publish - NOT to widen the guess. */
                        {
                            ULONG stripBo = 0, stripRows = 0;
                            PVOID stripVa = WinMaliUvSrcAcquireStrip(
                                                WINMALI_FB_STRIDE, WINMALI_FB_HEIGHT,
                                                &stripBo, &stripRows);
                            if (stripVa != NULL && stripRows != 0 &&
                                stripRows < WINMALI_FB_HEIGHT &&
                                WinMaliWorkerDst_(a) != NULL) {
                                ULONG dstTop = WINMALI_FB_HEIGHT - stripRows;
                                RtlCopyMemory(
                                    (PUCHAR)WinMaliWorkerDst_(a) +
                                        (SIZE_T)dstTop * WINMALI_FB_STRIDE,
                                    stripVa,
                                    (SIZE_T)stripRows * WINMALI_FB_STRIDE);
                                (VOID)InterlockedIncrement(&g_WinMaliStatStripBlits);
                            }
                            if (stripBo != 0) {
                                WinMaliUvSrcEndUse(stripBo);
                            }
                        }

                        /* â˜…â˜…â˜… THE FRAME IS COMPLETE - FLIP THE PANEL TO IT.
                         * Issued HERE and only here: the sweep-completion point is
                         * the one moment a WHOLE frame exists in the back buffer, so
                         * we can never flip to a half-composed image. Off unless
                         * OvlFlipRender is armed, which keeps display-only - where
                         * the flip already happens in PresentDisplayOnly - untouched
                         * and avoids flipping twice for one frame. */
                        /* â›” INSTRUMENT THE GATE ITSELF. Every input is recorded before
                         * the branch, so "the helper was not called" can name its own
                         * reason instead of being inferred. Reuses three names that are
                         * dead in mode 2 - the key is at its 127-value cap. */
                        {
                            ULONG gcode;
                            if (g_WinMaliOvlFlipRender == 0)      { gcode = 3; }
                            else if (g_WinMaliOvlFlip != 2)       { gcode = 2; }
                            else if (g_WinMaliOvlBackVa == NULL)  { gcode = 1; }
                            else                                  { gcode = 4; }
                            (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlipN);
                            (VOID)InterlockedExchange(&g_WinMaliStatOvlFlipRej,
                                                      (LONG)gcode);
                            if (gcode == 4) {
                                (VOID)WinMaliOvlFlipDual_();
                            }
                        }
                    }
                    /* ⛔★★★★★ SHIP A THUMBNAIL OF WHAT WE ACTUALLY SCAN OUT.
                     *
                     * The user can see the panel; I cannot, and every attempt to
                     * screenshot from the host failed on environment (WinRM is
                     * session 0, so window enumeration and CopyFromScreen see
                     * the wrong desktop; schtasks /IT reported SUCCESS and never
                     * ran the payload; Add-Type -TypeDefinition needs a compiler
                     * this ARM64 image lacks).
                     *
                     * A host screenshot would only show what WINDOWS believes it
                     * drew. This shows what actually reaches the panel - and the
                     * open question is precisely whether those differ (the
                     * taskbar is laid out, proven by the test-mode watermark
                     * moving up, yet its pixels never appear).
                     *
                     * 8x decimation: 2944x1840 -> 368x230, one pixel sampled per
                     * 8x8 block, packed BGR = 253 KB. No averaging - the point is
                     * "what is there", not fidelity. Throttled to once every 64
                     * sweeps so it costs nothing in steady state. */
                    {
                        static PUCHAR s_thumb = NULL;
                        static ULONG  s_thumbTick = 0;
                        const ULONG   TW = 368u, TH = 230u, STEP = 8u;
                        if (srcVa != NULL && effPitch != 0 &&
                            (s_thumbTick++ % 64u) == 0u) {
                            if (s_thumb == NULL) {
                                s_thumb = (PUCHAR)ExAllocatePoolWithTag(
                                    NonPagedPoolNx, TW * TH * 3u, 'bTmW');
                            }
                            if (s_thumb != NULL) {
                                ULONG tx, ty;
                                /* The row bound was already honoured; the COLUMN
                                   bound was not - tx*STEP*4 reaches 11,747 B,
                                   past the end of any row whose pitch is under
                                   the panel's. */
                                ULONG thumbCols = srcRowBytes / 4u;
                                for (ty = 0; ty < TH; ++ty) {
                                    ULONG sy = ty * STEP;
                                    const UCHAR *srow = (const UCHAR *)
                                        WinMaliSrcRow_(srcVa, maxRows, effPitch, sy);
                                    if (srow == NULL) break;
                                    for (tx = 0; tx < TW; ++tx) {
                                        ULONG sx = tx * STEP;
                                        const UCHAR* px;
                                        if (sx >= thumbCols) break;
                                        px = srow + (SIZE_T)sx * 4u;
                                        PUCHAR o = s_thumb + ((SIZE_T)ty * TW + tx) * 3u;
                                        o[0] = px[0]; o[1] = px[1]; o[2] = px[2];
                                    }
                                }
                                (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath,
                                        L"FrameThumb", s_thumb, TW * TH * 3u);
                                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                        L"FrameThumbW", TW);
                                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                        L"FrameThumbH", TH);
                            }
                        }
                    }
                    /* â›”â˜…â˜…â˜…â˜…â˜… A 1:1 CROP - THE THUMBNAIL IS 8x DECIMATED AND CANNOT
                     * SHOW A GLYPH. The user reports the taskbar clock and tray
                     * icons missing; at 8x the whole taskbar is 18 px tall, so every
                     * judgement I made from FrameThumb about TEXT was made from an
                     * image that could not contain text. Publish real pixels.
                     *
                     * Region comes from registry so it can be re-aimed without a
                     * rebuild. Bounds clamped against the same srcRowBytes/maxRows
                     * the thumbnail had to learn to respect. */
                    WinMaliPublishCrop_(srcVa, maxRows, effPitch, srcRowBytes, 1u);
#if 0  /* superseded by WinMaliPublishCrop_ */
                    {
                        static PUCHAR s_crop = NULL;
                        static ULONG  s_cropTick = 0;
                        const ULONG   CROPMAX = 1572864u;   /* 1.5 MB ceiling */
                        ULONG cw = g_WinMaliCropW, ch = g_WinMaliCropH;
                        if (srcVa != NULL && effPitch != 0 &&
                            (s_cropTick++ % 64u) == 0u) {
                            ULONG cols = srcRowBytes / 4u;
                            ULONG cx = g_WinMaliCropX, cy = g_WinMaliCropY;
                            /* â›”â˜… RE-AIM WITHOUT A REBOOT. Reading these only at init
                             * meant every new region cost a 5-8 minute boot, so the
                             * crop would in practice stay wherever it was first
                             * aimed. Same live-read pattern the worker already uses
                             * for VsyncFreeRun, on the same 1-in-64 throttle as the
                             * publish. A zeroed region falls through the existing
                             * `if (cw != 0u && ch != 0u)` below and publishes
                             * nothing - no new control flow needed. */
                            (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropX", cx, &cx);
                            (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropY", cy, &cy);
                            (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropW", cw, &cw);
                            (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"CropH", ch, &ch);
                            /* clamp the window into the real surface */
                            if (cx >= cols) { cx = 0u; }
                            if (cy >= maxRows) { cy = 0u; }
                            if (cx + cw > cols)    { cw = cols - cx; }
                            if (cy + ch > maxRows) { ch = maxRows - cy; }
                            while (cw != 0u && ch != 0u &&
                                   (SIZE_T)cw * ch * 3u > CROPMAX) { ch /= 2u; }
                            if (cw != 0u && ch != 0u) {
                                if (s_crop == NULL) {
                                    s_crop = (PUCHAR)ExAllocatePoolWithTag(
                                        NonPagedPoolNx, CROPMAX, 'cCmW');
                                }
                                if (s_crop != NULL) {
                                    ULONG ry, rx;
                                    for (ry = 0; ry < ch; ++ry) {
                                        const UCHAR *srow = (const UCHAR *)
                                            WinMaliSrcRow_(srcVa, maxRows, effPitch,
                                                           cy + ry);
                                        if (srow == NULL) { ch = ry; break; }
                                        for (rx = 0; rx < cw; ++rx) {
                                            const UCHAR *px =
                                                srow + (SIZE_T)(cx + rx) * 4u;
                                            PUCHAR o = s_crop +
                                                ((SIZE_T)ry * cw + rx) * 3u;
                                            o[0] = px[0]; o[1] = px[1]; o[2] = px[2];
                                        }
                                    }
                                    if (ch != 0u) {
                                        (VOID)WinMaliWriteParamsBinary(
                                            &g_WinMaliRegPath, L"FrameCrop",
                                            s_crop, (ULONG)((SIZE_T)cw * ch * 3u));
                                        (VOID)WinMaliWriteParamsDword(
                                            &g_WinMaliRegPath, L"FrameCropW", cw);
                                        (VOID)WinMaliWriteParamsDword(
                                            &g_WinMaliRegPath, L"FrameCropH", ch);
                                        (VOID)WinMaliWriteParamsDword(
                                            &g_WinMaliRegPath, L"FrameCropX", cx);
                                        (VOID)WinMaliWriteParamsDword(
                                            &g_WinMaliRegPath, L"FrameCropY", cy);
                                    }
                                }
                            }
                        }
                    }
#endif
                    (VOID)InterlockedExchange(&g_WinMaliStatCopyDone,
                                              (LONG)(done >> 10));   /* KB copied */
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatCopyAbort", (ULONG)g_WinMaliStatCopyAbort);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatCopyKB",    (ULONG)g_WinMaliStatCopyDone);
                    /* ★ THE ORACLE FOR THE LATCH. StatSweeps counts FULL frames
                       delivered from a SINGLE source; StatLatchDrop counts frames
                       abandoned because that source died. Sweeps climbing with
                       LatchDrop low = one coherent frame per paint (the mosaic is
                       gone). Sweeps stuck at 0 with LatchDrop climbing = no
                       candidate survives a whole frame, which is a different bug
                       and needs a different fix than tearing. */
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatSweeps",    (ULONG)g_WinMaliStatSweeps);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatUvRevokeTo", (ULONG)g_WinMaliStatUvRevokeTimeout);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatSkipNoSrc",  (ULONG)g_WinMaliStatSkipNoSrc);
                    /* ★ WHAT SHAPE DID WE ACTUALLY COPY? StatWinPitch is the
                       source pitch the UMD reported (0 = it did not), and
                       StatWinRows is how many rows the buffer could supply at
                       that pitch. Rows well under 1840 means the surface cannot
                       cover the panel at its own pitch - a UMD-side geometry bug,
                       not something the KMD can paper over. */
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatWinPitch",  effPitch);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatWinRows",   maxRows);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatPowerStop", (ULONG)g_WinMaliStatPowerStop);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"StatLatchDrop", (ULONG)g_WinMaliStatLatchDrop);
                    /* ★ THE COPY IS DONE - DROP THE USE-REF NOW. Holding it any
                       longer needlessly blocks a BO teardown that is waiting on
                       it. The sweep at the top of the next iteration is only the
                       backstop for paths that never reach here. */
                    if (s_pendingUse != 0) {
                        WinMaliUvSrcEndUse(s_pendingUse);
                        s_pendingUse = 0;
                    }
                    /* ★ FLUSHED completion marker for the first few passes. Paired
                     * with StatDbgPass above this is the decisive read after a
                     * hang: if StatDbgPass is N but StatDbgDone is N-1, the box
                     * died INSIDE pass N's copy - which finally distinguishes
                     * "the copy hangs" from "something after the copy hangs".
                     * Without a flush both counters vanish and the hang tells us
                     * nothing, which is what the last two recovery cycles cost. */
                    {
                        static LONG s_dbgDone = 0;
                        LONG d = InterlockedIncrement(&s_dbgDone);
                        if (d <= 40) {
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatDbgDone", (ULONG)d);
                            (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                    L"StatDbgDoneKB", (ULONG)(done >> 10));
                        }
                    }
                }

                /* Release only AFTER the copy - holding the candidate table
                   across it is what stops a BO free unlocking these pages
                   mid-memcpy. */
                if (uvHeld) {
                    WinMaliUvSrcRelease();
                    uvHeld = FALSE;
                }
            }
            {
                LONG64 n = InterlockedIncrement64(&a->ScanoutBltCount);

                /* ⛔★★★★★ IMAGE THE **PANEL**, NOT JUST THE 0x93 SOURCE.
                 *
                 * FrameThumb is written inside the 0x93 candidate copy. In the
                 * configuration that actually WORKS (dwm composing in WARP, dxgk
                 * flipping the real primary, this worker blitting it) that path is
                 * idle - StatUvSrcNCand=0 - so FrameThumb read **0% non-black while
                 * the user was looking at a full desktop with a taskbar**. The one
                 * instrument that is supposed to end arguments was contradicting the
                 * panel, because it was imaging a source nobody was writing.
                 *
                 * ScanoutVa is the panel itself: whatever we sample here is, by
                 * construction, what is on screen. Same 8x decimation and the same
                 * 1-in-64 throttle as FrameThumb.
                 * ★ AN INSTRUMENT MUST BE ATTACHED TO THE PATH THAT IS LIVE. */
                {
                    static PUCHAR s_panel = NULL;
                    static ULONG  s_panelTick = 0;
                    const ULONG   PW = 368u, PH = 230u, PSTEP = 8u;
                    if (a->ScanoutVa != NULL && (s_panelTick++ % 64u) == 0u) {
                        if (s_panel == NULL) {
                            s_panel = (PUCHAR)ExAllocatePoolWithTag(
                                NonPagedPoolNx, PW * PH * 3u, 'nPmW');
                        }
                        if (s_panel != NULL) {
                            ULONG py, px;
                            for (py = 0; py < PH; ++py) {
                                ULONG sy = py * PSTEP;
                                if (sy >= WINMALI_FB_HEIGHT) { break; }
                                for (px = 0; px < PW; ++px) {
                                    ULONG sx = px * PSTEP;
                                    const UCHAR *sp2;
                                    PUCHAR o;
                                    if (sx >= WINMALI_FB_WIDTH) { break; }
                                    sp2 = (const UCHAR *)((PUCHAR)a->ScanoutVa +
                                            (SIZE_T)sy * WINMALI_FB_STRIDE +
                                            (SIZE_T)sx * 4u);
                                    o = s_panel + ((SIZE_T)py * PW + px) * 3u;
                                    o[0] = sp2[0]; o[1] = sp2[1]; o[2] = sp2[2];
                                }
                            }
                            (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath,
                                    L"PanelThumb", s_panel, PW * PH * 3u);
                            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                    L"PanelThumbW", PW);
                            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                                    L"PanelThumbH", PH);
                        }
                    }
                }

                /*
                 * LIVE MARKER (Parameters\PresentMarker=1).
                 *
                 * ⛔ "I saw the desktop flash up, but I'm not sure it wasn't the
                 * OLD FRAMEBUFFER" is the single most expensive ambiguity in
                 * this project - the panel shows a live EDK2 image whether or
                 * not we ever write it, and it has already produced one false
                 * "we rendered the desktop".
                 *
                 * So stamp every frame WE blit with something the stale buffer
                 * cannot contain: a square whose colour advances each frame.
                 *   marker present + cycling => the image on screen IS ours, live
                 *   desktop but NO marker    => stale buffer, not us
                 * That distinction is not obtainable from any counter, because
                 * blits>0 only proves we wrote memory, not what reached the eye.
                 */
                if (g_WinMaliPresentMarker != 0) {
                    static const ULONG cyc[6] = {
                        0x00FF0000u, 0x00FFFF00u, 0x0000FF00u,
                        0x0000FFFFu, 0x000000FFu, 0x00FF00FFu
                    };
                    ULONG col = cyc[(ULONG)(n % 6)];
                    ULONG y, x;
                    for (y = 0; y < 160u && y < WINMALI_FB_HEIGHT; ++y) {
                        ULONG *row = (ULONG *)((PUCHAR)a->ScanoutVa +
                                               (SIZE_T)y * WINMALI_FB_STRIDE);
                        for (x = 0; x < 160u && x < WINMALI_FB_WIDTH; ++x) {
                            row[x] = col;
                        }
                    }
                }
            }
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Independent present tick. DISPATCH_LEVEL: signal only, never copy here. */
static VOID
WinMaliPresentDpc_(_In_ PKDPC Dpc, _In_opt_ PVOID Ctx,
                   _In_opt_ PVOID A1, _In_opt_ PVOID A2)
{
    PWINMALI_ADAPTER adapter = (PWINMALI_ADAPTER)Ctx;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(A1);
    UNREFERENCED_PARAMETER(A2);

    if (adapter == NULL || adapter->Magic != WINMALI_ADAPTER_MAGIC) { return; }
    if (adapter->ScanoutVa == NULL || adapter->PresentWorkerThread == NULL) { return; }
    /* Only while dxgk has made the source visible AND something has been
       flipped: otherwise the primary contents are not ours to show. */
    /* The test pattern deliberately IGNORES both gates: its whole purpose is to
       run when Windows has given us nothing (no flip, source never visible),
       which is exactly the case we cannot otherwise distinguish from success. */
    if (g_WinMaliPresentTestPattern == 0) {
        if (InterlockedCompareExchange(&adapter->SourceVisible, 1, 1) != 1) { return; }
        if (InterlockedCompareExchange64(&adapter->FlipLatchCount, 0, 0) == 0) { return; }
    }

    KeSetEvent(&adapter->PresentWake, IO_NO_INCREMENT, FALSE);
}

VOID
WinMaliPresentInit(_Inout_ PWINMALI_ADAPTER Adapter)
{
    HANDLE            hThread = NULL;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS          st;
    PHYSICAL_ADDRESS  pa;

    if (Adapter == NULL || Adapter->PresentWorkerThread != NULL) { return; }

    {
        SIZE_T frameBytes = (SIZE_T)WINMALI_FB_STRIDE * WINMALI_FB_HEIGHT;
        SIZE_T mapBytes;
        SIZE_T scanoutOff = 0;

        /*
         * ⛔ MAP THE ASSIGNED RESOURCE, NOT A HARDCODED ADDRESS.
         *
         * WINMALI_FB_SCANOUT_PA (0xFD619400) is 0x400 INTO the framebuffer
         * region, so mapping a whole frame from there overruns the end of the
         * region by 1 KB. That went unnoticed while nothing owned the range;
         * the moment the gpufb firmware declared it in our _CRS the bounds
         * became enforced, MmMapIoSpaceEx started FAILING, and the entire
         * present path silently switched off (PRESENT-V1: va=0, no "scanout
         * mapped" line). The firmware was right and we were wrong.
         *
         * So: when the firmware gives us the resource, map exactly that and
         * treat the scanout as an OFFSET within it. When it does not (older
         * builds e.g. gpudod), fall back to the historical hardcoded map.
         */
        if (Adapter->FbSize != 0 && Adapter->FbPhys.QuadPart != 0) {
            pa       = Adapter->FbPhys;
            mapBytes = (SIZE_T)Adapter->FbSize;
            if ((ULONGLONG)WINMALI_FB_SCANOUT_PA >= (ULONGLONG)Adapter->FbPhys.QuadPart) {
                scanoutOff = (SIZE_T)((ULONGLONG)WINMALI_FB_SCANOUT_PA -
                                      (ULONGLONG)Adapter->FbPhys.QuadPart);
            }
            if (scanoutOff >= mapBytes || frameBytes > mapBytes - scanoutOff) {
                WINMALI_WARN("Present: frame %Iu at +0x%Ix does not fit the %Iu-byte "
                             "framebuffer resource - refusing to present",
                             frameBytes, scanoutOff, mapBytes);
                return;
            }
        } else {
            /* ⛔⛔ ALIGN DOWN. WINMALI_FB_SCANOUT_PA is 0xFD619400 - 0x400 INTO a
             * page - and MmMapIoSpaceEx REFUSES A WRITE-COMBINED MAPPING OF AN
             * UNALIGNED PHYSICAL BASE while happily granting the same range
             * uncached. That one detail is why every frame this project has ever
             * painted went through the ~1000x-slower uncached path: ~20 s per
             * 21 MB frame instead of ~30 ms, which is why the copy had to be
             * sliced across ~11 worker wakes and why the panel shows a TORN
             * frame (photographed - new content above the slice boundary, the
             * previous frame below).
             *
             * MEASURED (StatWcProbe=0x7): one page at the region base, one page
             * at the PAGE-ALIGNED scanout base, and MmMapIoSpace(MmWriteCombined)
             * ALL succeed. Write-combining was available the whole time; only
             * the alignment of this one request was wrong.
             *
             * So map from the page-aligned base and carry the remainder as an
             * offset - exactly what the firmware-resource branch above already
             * does. */
            ULONGLONG rawPa = (ULONGLONG)WINMALI_FB_SCANOUT_PA;
            ULONGLONG alnPa = rawPa & ~((ULONGLONG)PAGE_SIZE - 1);
            scanoutOff  = (SIZE_T)(rawPa - alnPa);
            pa.QuadPart = (LONGLONG)alnPa;
            /* ⛔⛔ REVERTED TO ROUND-UP - THIS IS THE KNOWN-GOOD MAPPING.
             *
             * 1.1.9.68 booted with round-UP. 1.1.9.69 changed it to round-DOWN
             * (to obtain a write-combined mapping that does not overrun the end
             * of the framebuffer region) and the box HUNG THE INSTANT THE DRIVER
             * LOADED - three times, each costing a USB-spare recovery.
             *
             * I found and fixed one genuine overrun that change created (the row
             * loop bounded on WINMALI_FB_HEIGHT rather than on the mapped
             * extent) and the hang SURVIVED it. So round-DOWN breaks something
             * else I have not identified, and the honest move is to go back to
             * the mapping that demonstrably boots rather than keep guessing on a
             * machine that costs a physical recovery per attempt.
             *
             * Write-combining is still worth having - StatWcMaxKb proved a
             * full-frame WC map SUCCEEDS at the aligned base - but it must be
             * pursued without changing the extent the rest of the driver assumes. */
            mapBytes    = (SIZE_T)(((scanoutOff + frameBytes) + (PAGE_SIZE - 1)) &
                                   ~((SIZE_T)PAGE_SIZE - 1));
        }
        /* ⛔ THE WC CAPABILITY PROBE HAS BEEN REMOVED - IT ANSWERED ITS QUESTION.
         *
         * It established the fact we needed: StatWcMaxKb reached 21160 KB, i.e.
         * a WRITE-COMBINED mapping of the FULL 21,667,840-byte frame succeeds at
         * the page-aligned base. Keeping it would cost ten map/unmap cycles of
         * up to 21 MB inside StartDevice on every boot - thousands of system
         * PTEs churned for an answer we already have, on a path that has just
         * hung the machine twice. An instrument that has reported should be
         * taken out of the aircraft.
         *
         * ⛔ If it is ever reinstated: do NOT add its value names to the worker's
         * reserve list. The worker reserves by WRITING ZERO and starts AFTER
         * PresentInit, so reserving them silently erased the results and produced
         * a convincing 0x0 that read as "write-combining is impossible here". The
         * give-away was StatFbPhysLo=0 with the page-aligned bit CLEAR: zero IS
         * page-aligned, so that result could not have come from this code. */
        if (g_WinMaliRegPath.Length != 0) {
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatFbPhysLo",
                                          (ULONG)((ULONGLONG)pa.QuadPart & 0xFFFFFFFFu));
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatFbMapBytes",
                                          (ULONG)mapBytes);
        }


        /* ⛔⛔ WRITE-COMBINED OR THE MACHINE IS UNUSABLE - SO TRY MORE THAN ONCE.
         *
         * MEASURED 2026-08-22: StatScanoutWc = 0, i.e. every frame this project
         * has ever painted went through the UNCACHED fallback. That is not a
         * degraded mode, it is the cause of a whole cluster of symptoms:
         * uncached MMIO writes on ARM64 run at a few MB/s, so 21,667,840 bytes
         * takes ~20 s. It is why the copy had to be sliced into 2 MB bursts at
         * all, why a frame is spread over ~11 worker wakes, and therefore why
         * the panel shows a TORN frame - new content above the slice boundary,
         * the previous frame below (photographed).
         *
         * One refused attempt was being treated as the final answer. Ladder it:
         *
         *  1. MmMapIoSpaceEx(PAGE_WRITECOMBINE) - what we always tried.
         *  2. MmMapIoSpace(MmWriteCombined) - the OLDER api. It takes a
         *     MEMORY_CACHING_TYPE rather than PTE protection bits and goes down
         *     a different path in the memory manager, so it can succeed where
         *     the Ex form is refused. Costs one call to find out.
         *  3. Uncached, as before - but now it is the THIRD answer, not the
         *     first, and StatScanoutWc records WHICH rung won so this can never
         *     again be invisible.
         */
        if (g_WinMaliScanoutWcOff == 0) {
            Adapter->ScanoutMapVa = MmMapIoSpaceEx(pa, mapBytes,
                                                   PAGE_READWRITE | PAGE_WRITECOMBINE);
        }
        if (Adapter->ScanoutMapVa != NULL) {
            (VOID)InterlockedExchange(&g_WinMaliScanoutWc, 1);
        } else {
            if (g_WinMaliScanoutWcOff == 0) {
                Adapter->ScanoutMapVa = MmMapIoSpace(pa, mapBytes, MmWriteCombined);
            }
            if (Adapter->ScanoutMapVa != NULL) {
                (VOID)InterlockedExchange(&g_WinMaliScanoutWc, 2);
                WINMALI_WARN("Present: MmMapIoSpaceEx(WRITECOMBINE) refused but "
                             "MmMapIoSpace(MmWriteCombined) SUCCEEDED for 0x%llx "
                             "(%Iu bytes) - full-speed scanout after all",
                             (ULONGLONG)pa.QuadPart, mapBytes);
            }
        }
        if (Adapter->ScanoutMapVa == NULL) {
            /* ⛔⛔ THE "UNCACHED FALLBACK" MAY NEVER HAVE BEEN UNCACHED.
             *
             * PAGE_NOCACHE and PAGE_WRITECOMBINE are OPTIONAL MODIFIERS to
             * MmMapIoSpaceEx's protection argument. Bare PAGE_READWRITE is
             * therefore the DEFAULT (cached) request - not the uncached one the
             * old comment here asserted for months. So the last rung was
             * mislabelled, and the "~20 s per frame" figure attached to it was
             * arithmetic from an assumed write rate, never a measurement.
             *
             * That matters far beyond speed: if this mapping is CACHED then the
             * driver has never cleaned the D-cache after a blit, and the DSI has
             * been scanning out STALE DRAM. "We blit and the panel does not
             * change" is better explained by that than by slowness.
             *
             * So: try NOCACHE explicitly first, keep bare PAGE_READWRITE as a
             * distinct LAST rung, and record which one won - then read the PTE
             * attribute back with MmGetCacheAttribute rather than believing any
             * of these labels. */
            /* ⛔⛔ DO NOT REQUEST PAGE_NOCACHE HERE. I added it as a rung and it
             * HUNG THE BOX INSTANTLY, twice. A genuinely uncached 21 MB frame
             * really does take ~20 s to write, and the worker holds the CPU for
             * all of it. The historical bare-PAGE_READWRITE mapping never hung -
             * which is itself the evidence that it was CACHED, not uncached, and
             * that the "~1000x uncached" story attached to it for months was
             * wrong. Keep the mapping that works and record what it actually is. */
            Adapter->ScanoutMapVa = MmMapIoSpaceEx(pa, mapBytes, PAGE_READWRITE);
            (VOID)InterlockedExchange(&g_WinMaliScanoutWc, 4);
            WINMALI_WARN("Present: write-combined REFUSED for 0x%llx (%Iu bytes) - "
                         "fell back to rung %ld (3=NOCACHE, 4=default/cached)",
                         (ULONGLONG)pa.QuadPart, mapBytes, g_WinMaliScanoutWc);
        }
        /* ⛔⛔ MmGetCacheAttribute REMOVED - IT HUNG THE MACHINE.
         *
         * Added in 1.1.9.70 purely as a diagnostic ("what attribute did we
         * actually get?"). It is documented for SYSTEM MEMORY, and this is a
         * DEVICE physical address from the framebuffer resource - not RAM, and
         * not necessarily described in the OS memory map at all. Calling it here
         * hung the box on driver load in .70, .72, .73 and .74.
         *
         * ⛔ THIS WAS THE SECOND OF TWO SEPARATE HANGS I INTRODUCED, and it is
         * why reverting the first one (the round-down mapping, 1.1.9.69) did not
         * bring the machine back: I fixed one cause and left the other in place,
         * then reported the revert as if it should have worked. Nothing in the
         * driver reads the result - it was information for me, and it cost four
         * USB-spare recoveries. */
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                L"StatScanoutWc", (ULONG)g_WinMaliScanoutWc);
        if (Adapter->ScanoutMapVa == NULL) {
            WINMALI_WARN("Present: could not map framebuffer 0x%llx (%Iu bytes) - "
                         "the panel will stay blank while we own the display",
                         (ULONGLONG)pa.QuadPart, mapBytes);
            return;
        }
        Adapter->ScanoutMapBytes = mapBytes;
        Adapter->ScanoutVa       = (PUCHAR)Adapter->ScanoutMapVa + scanoutOff;
        /* Back to the known-good extent: the mapping covers a whole frame again,
           so ScanoutBytes is simply frameBytes as it was before 1.1.9.69. The
           destination row-bound added alongside stays - a no-op while these are
           equal, and a guard if they ever diverge again. */
        Adapter->ScanoutBytes = frameBytes;
        DbgPrint("[WinMali]    PRESENT: framebuffer resource pa=0x%llx map=%Iu va=%p "
                 "scanout=+0x%Ix\n",
                 (ULONGLONG)pa.QuadPart, mapBytes, Adapter->ScanoutMapVa, scanoutOff);
    }
    DbgPrint("[WinMali]    PRESENT: scanout mapped pa=0x%llx va=%p bytes=%Iu (%ux%u stride=%u)\n",
             (ULONGLONG)WINMALI_FB_SCANOUT_PA, Adapter->ScanoutVa,
             Adapter->ScanoutBytes, WINMALI_FB_WIDTH, WINMALI_FB_HEIGHT,
             WINMALI_FB_STRIDE);

    KeInitializeEvent(&Adapter->PresentWake, SynchronizationEvent, FALSE);
    Adapter->PresentStop = 0;

    /*
     * ⛔ DO NOT DRIVE PRESENTATION FROM THE VSYNC DPC ALONE.
     *
     * Measured 2026-08-12: across a 45 s armed window dxgk armed CRTC_VSYNC
     * only 3 times and we sent 67 notifications - about 1.5 Hz, not 60. Since
     * the blit was kicked solely from the vsync DPC, the panel was repainted
     * ~1.5 times a second, which is exactly the "flashing" the user sees rather
     * than a steady desktop.
     *
     * dxgk arms CRTC_VSYNC for ITS OWN purposes (retiring flips); it is not a
     * promise to call us at refresh rate. So run our own timer: while the
     * source is visible, the live scanout is refreshed on our schedule.
     *
     * Paced at ~30 Hz deliberately: each pass copies ~21 MB, so 60 Hz would be
     * ~1.3 GB/s of memcpy for a desktop that mostly is not changing. 30 Hz is
     * the honest v1 trade until we can tell when the primary actually changed.
     */
    KeInitializeDpc(&Adapter->PresentDpc, WinMaliPresentDpc_, Adapter);
    KeInitializeTimerEx(&Adapter->PresentTimer, SynchronizationTimer);
    /*
     * ⛔ SET ONLY ONCE THE TIMER AND DPC GENUINELY EXIST.
     *
     * Everything above this point can early-return - in particular the
     * MmMapIoSpaceEx failure. PresentTeardown must then NOT touch
     * PresentTimer/PresentDpc, because KeCancelTimer / KeRemoveQueueDpc on an
     * UNINITIALISED KTIMER/KDPC dispatch through a garbage routine pointer at
     * DISPATCH_LEVEL:
     *     0xD1  P2=0x02 (IRQL 2)  P3=0x08 (EXECUTE)  P1==P4
     * taken three times on 2026-08-12. It only became reachable once the gpufb
     * firmware put the framebuffer into our own _CRS and the mapping began to
     * fail, so the early return was finally exercised.
     *
     * WinMaliVsyncTeardown has guarded itself with VsyncInit since day one.
     * This is that same flag - copied properly this time.
     */
    Adapter->PresentInited = TRUE;

    /*
     * Parameters\PresentTimerHz = 0 DISARMS the periodic present tick.
     *
     * WHY IT EXISTS: the armed display path bugchecks ~70 s in, and
     * StatTeardownStage=99 proved it is NOT in StopDevice, so my teardown
     * theory was wrong. The present timer is the one periodic thing we added,
     * so make it a VARIABLE instead of guessing again: run armed with the timer
     * off and the answer is unambiguous either way -
     *   no crash  => the timer/DPC/worker is the culprit
     *   crash     => it is not ours, stop looking here
     * (Nothing paints with the timer off, which is fine - this run is about
     * isolating the fault, not about pixels.)
     */
    if (g_WinMaliPresentTimerHz != 0) {
        LARGE_INTEGER due;
        LONG period = (LONG)(1000u / g_WinMaliPresentTimerHz);
        if (period < 5) { period = 5; }
        due.QuadPart = -((LONGLONG)period * 10000LL);
        KeSetTimerEx(&Adapter->PresentTimer, due, period, &Adapter->PresentDpc);
    }

    /* Publish the mapping result AT START. StopDevice may never run (that is
       exactly what happened), so anything only published at teardown is lost. */
    if (g_WinMaliRegPath.Length != 0) {
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatScanoutMapped", 1);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPresentTimerHz",
                                      g_WinMaliPresentTimerHz);
    }
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                              WinMaliPresentWorker_, Adapter);
    if (!NT_SUCCESS(st)) {
        WINMALI_WARN("Present: worker thread create failed 0x%08x", st);
        return;
    }
    (VOID)ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, NULL, KernelMode,
                                    &Adapter->PresentWorkerThread, NULL);
    ZwClose(hThread);
}

VOID
WinMaliPresentTeardown(_Inout_ PWINMALI_ADAPTER Adapter)
{
    PVOID th;
    if (Adapter == NULL) { return; }

    /* ⛔ CANCEL THE TIMER FIRST. Its DPC captures the adapter pointer, so a
       periodic timer left running past RemoveDevice is a use-after-free with a
       33 ms fuse. KeCancelTimer + KeFlushQueuedDpcs guarantees no DPC is in
       flight before anything it touches is torn down. */
    /* ⛔ ONLY IF PresentInit ACTUALLY CREATED THEM. Cancelling an uninitialised
       KTIMER/KDPC dispatches through garbage at DISPATCH_LEVEL -> 0xD1 (IRQL 2,
       EXECUTE, P1==P4), taken three times on 2026-08-12 once the gpufb firmware
       made the scanout mapping fail and the early return in PresentInit was
       finally reachable.
       Dequeue one that is ALREADY queued as well: KeFlushQueuedDpcs waits for it
       to RUN, which is not what we want when what it touches is going away.
       This is the sequence WinMaliVsyncTeardown has always used, guard included. */
    if (Adapter->PresentInited) {
        (VOID)KeCancelTimer(&Adapter->PresentTimer);
        (VOID)KeRemoveQueueDpc(&Adapter->PresentDpc);
        KeFlushQueuedDpcs();
        Adapter->PresentInited = FALSE;
    }

    th = Adapter->PresentWorkerThread;
    Adapter->PresentWorkerThread = NULL;
    if (th != NULL) {
        InterlockedExchange(&Adapter->PresentStop, 1);
        KeSetEvent(&Adapter->PresentWake, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(th, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(th);
    }
    /* ⚠ Unmap the MAP BASE and MAP LENGTH. ScanoutVa is base+0x400 and
       ScanoutBytes is one frame - passing those to MmUnmapIoSpace would
       free a different range than we mapped. */
    if (Adapter->ScanoutMapVa != NULL) {
        /* ⛔ THE PUSH RUNS ON dwm's THREAD, NOT OURS. Disarm it and WAIT for any
           in-flight copy to finish before the mapping it writes into disappears,
           or teardown races a live RtlCopyMemory into freed PTEs. */
        WinMaliUvPushDrain();
        MmUnmapIoSpace(Adapter->ScanoutMapVa, Adapter->ScanoutMapBytes);
        Adapter->ScanoutMapVa = NULL;
        Adapter->ScanoutMapBytes = 0;
        Adapter->ScanoutVa = NULL;
    }
}

/* Read once per adapter: is the free-running vsync heartbeat enabled?
   Registry knob so it can be cleared over WinRM without a rebuild - a 60 Hz DPC
   is real load and the CSF path has known timeout sensitivity. Default ON. */
static BOOLEAN
WinMaliVsyncFreeRunEnabled(_In_ PWINMALI_ADAPTER Adapter)
{
    ULONG v = 1;   /* default ON */

    UNREFERENCED_PARAMETER(Adapter);

    (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"VsyncFreeRun", 1, &v);
    return (v != 0) ? TRUE : FALSE;
}
VOID
WinMaliVsyncSetEnabled(_Inout_ PWINMALI_ADAPTER Adapter, _In_ BOOLEAN Enable);

VOID
WinMaliVsyncInit(_Inout_ PWINMALI_ADAPTER Adapter)
{
    if (Adapter == NULL || Adapter->VsyncInit) { return; }
    KeInitializeDpc(&Adapter->VsyncDpc, WinMaliVsyncDpc, Adapter);
    KeInitializeTimerEx(&Adapter->VsyncTimer, SynchronizationTimer);
    Adapter->VsyncInit = TRUE;

    /* Start the heartbeat immediately when free-running. dxgk only ARMS vsync
       when it has a flip to retire, and there is no flip until DWM composes -
       which it will not do without vblank. Starting here breaks that circle at
       adapter init rather than waiting for a trigger that never comes. */
    if (WinMaliVsyncFreeRunEnabled(Adapter)) {
        ULONG dodFree = 0;

        if (g_WinMaliDodMode != 0) {
            /* ⛔ DEFAULT 1, NOT 0. 1.1.12.7 defaulted this OFF and that is what
               starves dxgk's DOD vsync watchdog. Set DodVsyncFreeRun=0 to get
               the old (fatal) behaviour back for an A/B without a rebuild. */
            (VOID)WinMaliReadParamsDword(&g_WinMaliRegPath, L"DodVsyncFreeRun",
                                         1, &dodFree);
        }
        if (g_WinMaliDodMode != 0 && dodFree == 0) {
            /* â›” A DOD HAS NO CRTC AND NO FLIP TO RETIRE. CrtcVsync carries a
               PhysicalAddress (the displaying buffer); DisplayOnlyVsync carries
               only VidPnTargetId. Firing CRTC_VSYNC from a DPC during adapter
               start, for a target dxgk has not registered yet, is a claim about
               hardware we do not have. Let ControlInterrupt arm it if dxgk ever
               actually asks. */
            DbgPrint("[WinMali]    VSYNC: free-running heartbeat SUPPRESSED "
                     "(DodVsyncFreeRun=0) - dxgk's DOD vsync watchdog WILL "
                     "fail this adapter\n");
        } else {
            WinMaliVsyncSetEnabled(Adapter, TRUE);
            DbgPrint("[WinMali]    VSYNC: free-running heartbeat started at init "
                     "(VsyncFreeRun=1)\n");
        }
    }
}

/* ⛔★★★★★ FREE-RUNNING VSYNC - BREAKS THE COMPOSITION STARVATION DEADLOCK.
 *
 * MEASURED 2026-08-18 (UMD 1.1.3.0, WDDM2_0 live, desktop rendering):
 *     LiveVsyncNotify +20 over 20 s  =  1 Hz   (a 60 Hz panel needs ~1200)
 *     LiveVsyncArm    +2  over 20 s
 *     StatSrcChg       0             =  dwm never recomposes
 * while Explorer WAS presenting (_Present calls=673) and dwm HAD the taskbar
 * strip open (OPENED shared surface 2944x144). Every piece was in place except
 * the heartbeat.
 *
 * THE DEADLOCK: DWM composes on vblank. Our synthesised vsync only ran while
 * dxgk had ARMED it, and dxgk arms it to retire a flip - but there is no flip
 * because dwm never composed. no vblank -> no composite -> no flip -> no arm ->
 * no vblank. A desktop with a taskbar clock must recompose at least once a
 * minute; ours only moved when the user physically pressed a key.
 *
 * Real hardware does not work the way the old code assumed: the CRTC keeps
 * scanning out and raising vblank whether or not anyone has asked for it.
 * ControlInterrupt gates whether we REPORT it, not whether it HAPPENS. Keeping
 * the timer free-running matches that, and costs one DPC per frame.
 *
 * ⚠ Gated on an env-equivalent registry knob (VsyncFreeRun, default ON) so it
 * can be turned off from WinRM without a rebuild if it destabilises anything -
 * a 60 Hz DPC is a real load and the CSF path has known timeout sensitivity.
 * When OFF the behaviour is exactly the previous arm/disarm.
 * ⚠ Still a free-running TIMER, not a real DISP vsync: fine for "vsync
 * HAPPENING", NOT trustworthy for vsync PHASE. Do not build tear-free
 * scanout on top of this. */
VOID
WinMaliVsyncSetEnabled(_Inout_ PWINMALI_ADAPTER Adapter, _In_ BOOLEAN Enable)
{
    LARGE_INTEGER due;

    if (Adapter == NULL || !Adapter->VsyncInit) { return; }

    if (Enable) {
        if (InterlockedExchange(&Adapter->VsyncEnabled, 1) == 1) { return; }
        /* ~60 Hz. Period is in ms for KeSetTimerEx; due time negative = relative
           100ns units. Nominal only -- see the drift caveat above. */
        due.QuadPart = -166667LL;
        KeSetTimerEx(&Adapter->VsyncTimer, due, 17, &Adapter->VsyncDpc);
        DbgPrint("[WinMali]    VSYNC: timer armed (~60 Hz, SYNTHESISED)\n");
    } else {
        /* Free-run: dxgk disarms whenever it has no flip to retire, which
           starves DWM's vblank wait and stops composition entirely (see the
           note above). Honour the disable only when the knob is off. */
        if (WinMaliVsyncFreeRunEnabled(Adapter)) {
            return;
        }
        if (InterlockedExchange(&Adapter->VsyncEnabled, 0) == 0) { return; }
        (VOID)KeCancelTimer(&Adapter->VsyncTimer);
        DbgPrint("[WinMali]    VSYNC: timer disarmed\n");
    }
}

VOID
WinMaliVsyncTeardown(_Inout_ PWINMALI_ADAPTER Adapter)
{
    if (Adapter == NULL || !Adapter->VsyncInit) { return; }
    InterlockedExchange(&Adapter->VsyncEnabled, 0);
    (VOID)KeCancelTimer(&Adapter->VsyncTimer);
    /* Flush any DPC already queued before we let the adapter go away. */
    (VOID)KeRemoveQueueDpc(&Adapter->VsyncDpc);
    KeFlushQueuedDpcs();
    Adapter->VsyncInit = FALSE;
}

/* WDDM 2.2+ pre-start handshake. dxgk calls this between AddDevice and
   StartDevice to negotiate boot-display preservation (smooth transition).
   For a render-only adapter without a boot display, both output bits stay
   zero. Returning NOT_SUPPORTED here tears the adapter down on WDDM 2.5.

   NOT WIRED on WDDM 3.2: this DDI is part of CreateDisplayCore's
   display-DDI all-or-none sweep - a displayless adapter must leave it
   NULL or AddAdapter fails with STATUS_REVISION_MISMATCH (event 549).
   Kept compiled for the day we grow a display core; see NULL_FIELDS in
   Tools\gen-dxgk-stubs.py. */
NTSTATUS APIENTRY
WinMaliKmdExchangePreStartInfo(
    _In_                              CONST PVOID            MiniportDeviceContext,
    _Inout_                           PDXGK_PRE_START_INFO   PreStartInfo)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    WM_TRACE_ENTER("ExchangePreStartInfo");
    if (PreStartInfo != NULL) {
        PreStartInfo->Output = 0;   /* render-only: no boot display */
    }
    WM_TRACE_EXIT_S("ExchangePreStartInfo", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdQueryChildRelations(
    _In_                             CONST PVOID            MiniportDeviceContext,
    _Out_writes_bytes_(ChildRelationsSize) PDXGK_CHILD_DESCRIPTOR ChildRelations,
    _In_                             ULONG                  ChildRelationsSize)
{
    ULONG slots;

    WM_TRACE_ENTER("QueryChildRelations");
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    /* Stage 6 = the caps negotiation was ACCEPTED. dxgk only enumerates
       children once it is satisfied with the adapter, so reaching here means
       PHYSICALADAPTERCAPS and friends are behind us. */
    if (g_WinMaliDodMode != 0 && g_WinMaliDodStage < 6) { WinMaliDodStage(6); }

    if (ChildRelations == NULL || ChildRelationsSize == 0) {
        WM_TRACE_EXIT_S("QueryChildRelations", STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(ChildRelations, ChildRelationsSize);
    slots = ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR);

    /* Render-only -- the shipping configuration. StartDevice reported zero
       children, so the array is nothing but the zeroed terminator. */
    if (g_WinMaliChildCount == 0) {
        DbgPrint("[WinMali]    QueryChildRelations: 0 children (render-only), slots=%lu\n",
                 slots);
        WM_TRACE_EXIT_S("QueryChildRelations", STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }

    /*
     * Experiment path (VidPnSources != 0). Zeroing ALONE was the bug: it left
     * ChildDeviceType = TypeUninitialized (0), which is not a describable child,
     * and dxgk gave up exactly here -- the boot-log trace showed
     * StartDevice -> QueryChildRelations -> StopDevice with nothing in between.
     *
     * dxgk sizes the buffer as (NumberOfChildren + 1) entries so the final slot
     * stays zeroed as a terminator; hence the >= 2 requirement for one child.
     */
    if (slots < 2) {
        WM_TRACE_EXIT_S("QueryChildRelations", STATUS_BUFFER_TOO_SMALL);
        return STATUS_BUFFER_TOO_SMALL;
    }

    /*
     * dxgk accepts this child and then STOPS, without calling QueryChildStatus or
     * any VidPN DDI -- so it is failing while processing the CHILD itself, and the
     * fault is somewhere in these few fields. Each candidate is a one-line change
     * but every install costs a version bump, so they are selected by
     * Parameters\ChildCfg instead. One binary, whole matrix.
     *
     *   0 = TypeVideoOutput  / VOT_INTERNAL / AlwaysConnected / uid 0  (baseline)
     *   1 =  ... uid 1                (is ChildUid 0 reserved?)
     *   2 =  ... HpdAwarenessInterruptible
     *   3 =  ... VOT_OTHER
     *   4 = TypeIntegratedDisplay     (what an on-board panel arguably is)
     *   5 = TypeOther
     */
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
        D3DKMDT_VOT_INTERNAL;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness =
        D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    ChildRelations[0].AcpiUid  = 0;
    ChildRelations[0].ChildUid = WINMALI_CHILD_UID;

    /* HpdMode is ORTHOGONAL to the ChildCfg variant: dxgk only honours a
       connection change on an INTERRUPTIBLE child, and the variant we ship
       (cfg 7 = DP_EMBEDDED) sets AlwaysConnected. Applied before the switch so
       any variant can be combined with it. */
    if (g_WinMaliHpdMode != 0) {
        ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
    }

    switch (g_WinMaliChildCfg) {
    case 1:
        ChildRelations[0].ChildUid = 1;
        break;
    case 2:
        ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
        break;

    /*
     * cfg 6..9: the UNTESTED axis. The original matrix only ever compared
     * VOT_INTERNAL against VOT_OTHER, and VOT_OTHER is not a real video output
     * (it fails earlier, at reason 3). So "a valid output that is not INTERNAL"
     * was never tried.
     *
     * This matters because VOT_INTERNAL asserts "I am the built-in panel", and
     * the built-in panel is by definition the boot display -- which on this
     * machine belongs to BasicDisplay, not to us. dxgk tracks exactly that
     * (StartAdapter_FdoIsNotAPostDevice = 14, PostDeviceIsRenderOnly = 20).
     * If dxgk refuses an INTERNAL child from a non-POST adapter, an external
     * technology should behave differently -- and if it does not, POST-device
     * ownership is exonerated and the fault lies elsewhere entirely.
     */
    case 6:
        ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
            D3DKMDT_VOT_HDMI;
        break;
    case 7:
        ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
            D3DKMDT_VOT_DISPLAYPORT_EMBEDDED;
        break;
    case 8:
        ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
            D3DKMDT_VOT_DISPLAYPORT_EXTERNAL;
        break;
    case 9:
        /* LVDS: an internal panel link, but NOT the magic 0x80000000 value.
           Separates "internal-ness" from the specific INTERNAL constant. */
        ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
            D3DKMDT_VOT_LVDS;
        break;
    case 3:
        ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
            D3DKMDT_VOT_OTHER;
        break;
    case 4:
        ChildRelations[0].ChildDeviceType = TypeIntegratedDisplay;
        RtlZeroMemory(&ChildRelations[0].ChildCapabilities.Type,
                      sizeof(ChildRelations[0].ChildCapabilities.Type));
        ChildRelations[0].ChildCapabilities.Type.IntegratedDisplayChild.InterfaceTechnology =
            D3DKMDT_VOT_INTERNAL;
        ChildRelations[0].ChildCapabilities.Type.IntegratedDisplayChild.DescriptorLength = 0;
        break;
    case 5:
        ChildRelations[0].ChildDeviceType = TypeOther;
        RtlZeroMemory(&ChildRelations[0].ChildCapabilities.Type,
                      sizeof(ChildRelations[0].ChildCapabilities.Type));
        ChildRelations[0].ChildCapabilities.Type.Other.MustBeZero = 0;
        break;
    default:
        break;
    }

    DbgPrint("[WinMali]    QueryChildRelations: 1 child cfg=%lu type=%u tech=0x%x hpd=%u uid=%u slots=%lu\n",
             g_WinMaliChildCfg,
             (unsigned)ChildRelations[0].ChildDeviceType,
             (unsigned)ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology,
             (unsigned)ChildRelations[0].ChildCapabilities.HpdAwareness,
             (unsigned)ChildRelations[0].ChildUid, slots);
    /* Stage 7 = we came back OUT of QueryChildRelations with SUCCESS. Stage 6
       is set on ENTRY, so without this "we hung inside it" and "we returned
       cleanly and dxgk stopped anyway" are indistinguishable - and they need
       opposite fixes. */
    if (g_WinMaliDodMode != 0 && g_WinMaliDodStage < 7) { WinMaliDodStage(7); }
    WM_TRACE_EXIT_S("QueryChildRelations", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdQueryChildStatus(
    _In_    CONST PVOID         MiniportDeviceContext,
    _In_    PDXGK_CHILD_STATUS  ChildStatus,
    _In_    BOOLEAN             NonDestructiveOnly)
{
    WM_TRACE_ENTER("QueryChildStatus");
    WinMaliDodDdi(WINMALI_DOD_DDI_CHILDSTATUS);
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);

    if (ChildStatus == NULL) {
        WM_TRACE_EXIT_S("QueryChildStatus", STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    /* Only meaningful once we describe a child; render-only reports none. */
    if (g_WinMaliChildCount == 0 || ChildStatus->ChildUid != WINMALI_CHILD_UID) {
        WM_TRACE_EXIT_S("QueryChildStatus", STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    switch (ChildStatus->Type) {
    case StatusConnection:
        /* HpdAwarenessAlwaysConnected, so this is a constant. */
        ChildStatus->HotPlug.Connected = TRUE;
        DbgPrint("[WinMali]    QueryChildStatus: connection -> CONNECTED\n");
        WM_TRACE_EXIT_S("QueryChildStatus", STATUS_SUCCESS);
        return STATUS_SUCCESS;

    case StatusRotation:
        ChildStatus->Rotation.Angle = 0;
        WM_TRACE_EXIT_S("QueryChildStatus", STATUS_SUCCESS);
        return STATUS_SUCCESS;

    default:
        DbgPrint("[WinMali]    QueryChildStatus: type %u unhandled\n",
                 (unsigned)ChildStatus->Type);
        WM_TRACE_EXIT_S("QueryChildStatus", STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS APIENTRY
WinMaliKmdQueryDeviceDescriptor(
    _In_  CONST PVOID                  MiniportDeviceContext,
    _In_  ULONG                        ChildUid,
    _Inout_ PDXGK_DEVICE_DESCRIPTOR    DeviceDescriptor)
{
    NTSTATUS status;

    WM_TRACE_ENTER("QueryDeviceDescriptor");
    WinMaliDodDdi(WINMALI_DOD_DDI_DEVICEDESC);
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    /*
     * Returning STATUS_MONITOR_NO_DESCRIPTOR here is what killed every
     * ComputeOnly=0 attempt: the dxgk ETW trace carries exactly one graphics
     * failure among 23,375 events -- 0xC01E0401
     * STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED -- and dxgk then abandons
     * the adapter without ever calling QueryChildStatus or a single VidPN DDI.
     * The panel has no EDID ROM, so we synthesise one describing exactly the
     * mode the VidPN DDIs publish.
     */
    /* Gate on CHILDREN, not on VidPN sources: a descriptor describes the child,
       and the two counts became independent knobs. Keyed on VidPnSources this
       refused a descriptor in exactly the ChildCount=1 / VidPnSources=0 cell
       that exists to separate them -- the cell would have tested nothing. */
    if (g_WinMaliChildCount == 0 || ChildUid != WINMALI_CHILD_UID ||
        DeviceDescriptor == NULL) {
        WM_TRACE_EXIT_S("QueryDeviceDescriptor", STATUS_MONITOR_NO_DESCRIPTOR);
        return STATUS_MONITOR_NO_DESCRIPTOR;
    }

    {
        /* ⛔ DescriptorLength is _Inout_ (dispmprt.h:296-300): the caller says how
           much it wants, the driver reports how much it SUPPLIED. We used to copy
           fewer bytes than asked and leave the count untouched, so dxgk parsed
           stale trailing bytes as if they were EDID. */
        ULONG supplied = 0;
        status = WinMaliEdidCopy(DeviceDescriptor->DescriptorOffset,
                                 DeviceDescriptor->DescriptorLength,
                                 DeviceDescriptor->DescriptorBuffer,
                                 &supplied);
        if (NT_SUCCESS(status)) {
            DeviceDescriptor->DescriptorLength = supplied;
        }
    }
    DbgPrint("[WinMali]    QueryDeviceDescriptor: uid=%lu off=%lu len=%lu -> 0x%08x\n",
             ChildUid, DeviceDescriptor->DescriptorOffset,
             DeviceDescriptor->DescriptorLength, (unsigned)status);
    WM_TRACE_EXIT_S("QueryDeviceDescriptor", status);
    return status;
}

NTSTATUS APIENTRY
WinMaliKmdSetPowerState(
    _In_ CONST PVOID         MiniportDeviceContext,
    _In_ ULONG               DeviceUid,
    _In_ DEVICE_POWER_STATE  DevicePowerState,
    _In_ POWER_ACTION        ActionType)
{
    PWINMALI_ADAPTER a = (PWINMALI_ADAPTER)MiniportDeviceContext;

    WM_TRACE_ENTER("SetPowerState");
    WinMaliDodDdi(WINMALI_DOD_DDI_SETPOWERSTATE);
    WinMaliVpLogOnly_(WINMALI_VPC_POWER);
    UNREFERENCED_PARAMETER(DeviceUid);

    /* ⛔★★★★★★ STOP THE PRESENT WORKER ON SHUTDOWN. THIS WAS A PURE STUB.
     *
     * MEASURED 2026-08-17: a remote `shutdown /r` was ACCEPTED and the box then
     * sat in "a system shutdown is in progress" (rc 1115) for twenty minutes -
     * reachable over WinRM throughout, with StatSweeps climbing 1282 -> 1320 ->
     * 1391 THROUGH the shutdown. The present worker was still copying frames
     * into the scanout while Windows was trying to tear the machine down, and it
     * took a hard reset to recover.
     *
     * PresentStop was only ever set from StopDevice. A shutdown need not route
     * through StopDevice, but it ALWAYS lowers device power, and this DDI is
     * where that arrives - so this is the right place to stop touching the
     * framebuffer.
     *
     * ⚠ Deliberately ONE-WAY: never clear PresentStop here. A worker that
     * resumes painting because something briefly returned to D0 mid-teardown is
     * the same hang again, and nothing needs the worker once power has dropped -
     * the next real D0 arrives with a fresh StartDevice. */
    if (a != NULL && (DevicePowerState != PowerDeviceD0 ||
                      ActionType == PowerActionShutdown ||
                      ActionType == PowerActionShutdownReset ||
                      ActionType == PowerActionShutdownOff)) {
        (VOID)InterlockedExchange(&a->PresentStop, 1);
        (VOID)InterlockedIncrement(&g_WinMaliStatPowerStop);
    }

    WM_TRACE_EXIT_S("SetPowerState", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdNotifyAcpiEvent(
    _In_  CONST PVOID         MiniportDeviceContext,
    _In_  DXGK_EVENT_TYPE     EventType,
    _In_  ULONG               Event,
    _In_  PVOID               Argument,
    _Out_ PULONG              AcpiFlags)
{
    WM_TRACE_ENTER("NotifyAcpiEvent");
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Argument);
    if (AcpiFlags != NULL) {
        *AcpiFlags = 0;
    }
    WM_TRACE_EXIT_S("NotifyAcpiEvent", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

VOID APIENTRY
WinMaliKmdResetDevice(_In_ CONST PVOID MiniportDeviceContext)
{
    WinMaliDodDdi(WINMALI_DOD_DDI_RESETDEVICE);
    WM_TRACE_ENTER("ResetDevice");
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    WM_TRACE_EXIT("ResetDevice");
}

/* --- GUID_WDDM_INTERFACE_FEATURE ------------------------------------------
 * Captured on silicon (KMD 1.0.0.91, ComputeOnly=0): dxgk issues exactly nine
 * DxgkDdiQueryInterface calls and then fails AddAdapter. Eight are optional
 * display features (I2C, DISPLAYMUX_2, OPM/OPM_2/OPM_2_JTP/OPM_3, MIRACAST).
 * The ninth and eighth are the SAME interface -- GUID_WDDM_INTERFACE_FEATURE --
 * asked for at v2 and then retried at v1. A version fallback immediately before
 * giving up is what a MANDATORY interface looks like.
 *
 * (This corrects the older comment in WinMaliQai.c, which guessed those last two
 * queries were "the display-core interface twice". They are not; DisplayCore is
 * a different GUID entirely.)
 *
 * Answering it costs nothing: we support no optional features, so
 * QueryFeatureSupport reports "not supported" for everything and
 * QueryFeatureInterface refuses. The point is to answer the QUERY rather than
 * refuse the interface itself.
 *
 * Declared locally rather than via DEFINE_GUID/initguid.h to avoid dragging in
 * a GUID lib for one comparison.
 */
static const GUID WinMaliGuidWddmFeature =
    { 0x94bb3993, 0xc6c3, 0x4da7, { 0x89, 0x49, 0xa1, 0x13, 0x82, 0x32, 0xe7, 0x59 } };

static VOID WinMaliFeatureIfaceRef(_In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static VOID WinMaliFeatureIfaceDeref(_In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

_Check_return_
static NTSTATUS APIENTRY
WinMaliKmdQueryFeatureSupport(
    _In_ CONST HANDLE                     hAdapter,
    _Inout_ DXGKARG_QUERYFEATURESUPPORT  *pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* We implement no optional WDDM features. Report that honestly and
       succeed -- reporting cleanly is the contract; refusing the query is not. */
    pArgs->MinSupportedVersion      = 0;
    pArgs->MaxSupportedVersion      = 0;
    pArgs->SupportedByDriver        = FALSE;
    pArgs->SupportedOnCurrentConfig = FALSE;
    return STATUS_SUCCESS;
}

_Check_return_
static NTSTATUS APIENTRY
WinMaliKmdQueryFeatureInterface(
    _In_ CONST HANDLE                       hAdapter,
    _Inout_ DXGKARG_QUERYFEATUREINTERFACE  *pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    /* No feature interfaces implemented; QueryFeatureSupport already said so. */
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY
WinMaliKmdQueryInterface(
    _In_ CONST PVOID    MiniportDeviceContext,
    _In_ PQUERY_INTERFACE QueryInterface)
{

    /*
     * ★ MT6897 INSTRUMENTATION. We refuse EVERY interface dxgk asks for. On an
     * ACPI-enumerated ARM64 adapter (ours: ACPI\ARMH5655, _CCA=0) dxgk may be asking for
     * something it genuinely requires -- e.g. a DMA-adapter interface -- where on a
     * differently-enumerated reference platform it either would not ask or would be
     * satisfied elsewhere. Refusing everything is only safe if nothing asked for is
     * mandatory, and we cannot know that while the GUIDs are unlogged.
     */
    if (QueryInterface != NULL && QueryInterface->InterfaceType != NULL) {
        /* Persist the sequence -- DbgPrint alone needs a kernel debugger, which
           is exactly why these GUIDs stayed unknown. Writing on every call keeps
           the blob correct even though we cannot know which query is the last
           one before dxgk gives up. */
        if (g_WinMaliQiCount < WINMALI_QI_LOG_MAX) {
            g_WinMaliQiLog[g_WinMaliQiCount].Interface = *QueryInterface->InterfaceType;
            g_WinMaliQiLog[g_WinMaliQiCount].Version   = (USHORT)QueryInterface->Version;
            g_WinMaliQiLog[g_WinMaliQiCount].Size      = (USHORT)QueryInterface->Size;
            g_WinMaliQiCount++;
        }
        if (KeGetCurrentIrql() == PASSIVE_LEVEL && g_WinMaliRegPath.Length != 0) {
            (VOID)WinMaliWriteParamsBinary(&g_WinMaliRegPath, L"QueryInterfaceLog",
                                           g_WinMaliQiLog,
                                           g_WinMaliQiCount * sizeof(g_WinMaliQiLog[0]));
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"QueryInterfaceCount",
                                          g_WinMaliQiCount);
        }

        /* Answer the WDDM feature interface (v1 -- the v2 layout is not in this
           WDK, and dxgk already falls back to v1 on its own). */
        if (g_WinMaliFeatureIface != 0 &&
            RtlEqualMemory(QueryInterface->InterfaceType, &WinMaliGuidWddmFeature, sizeof(GUID)) &&
            QueryInterface->Version == DXGK_FEATURE_INTERFACE_VERSION_1 &&
            QueryInterface->Size >= sizeof(DXGKDDI_FEATURE_INTERFACE) &&
            QueryInterface->Interface != NULL) {

            PDXGKDDI_FEATURE_INTERFACE fi = (PDXGKDDI_FEATURE_INTERFACE)QueryInterface->Interface;

            RtlZeroMemory(fi, sizeof(*fi));
            fi->Size                  = sizeof(DXGKDDI_FEATURE_INTERFACE);
            fi->Version               = DXGK_FEATURE_INTERFACE_VERSION_1;
            fi->Context               = MiniportDeviceContext;
            fi->InterfaceReference    = WinMaliFeatureIfaceRef;
            fi->InterfaceDereference  = WinMaliFeatureIfaceDeref;
            fi->QueryFeatureSupport   = WinMaliKmdQueryFeatureSupport;
            fi->QueryFeatureInterface = WinMaliKmdQueryFeatureInterface;

            if (KeGetCurrentIrql() == PASSIVE_LEVEL && g_WinMaliRegPath.Length != 0) {
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"FeatureIfaceAccepted", 1);
            }
            DbgPrint("[WinMali] QueryInterface WDDM_FEATURE v1 -> ACCEPTED\n");
            return STATUS_SUCCESS;
        }

        DbgPrint("[WinMali] QueryInterface {%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x}"
                 " ver=%u size=%u -> NOT_SUPPORTED\n",
                 QueryInterface->InterfaceType->Data1,
                 QueryInterface->InterfaceType->Data2,
                 QueryInterface->InterfaceType->Data3,
                 QueryInterface->InterfaceType->Data4[0], QueryInterface->InterfaceType->Data4[1],
                 QueryInterface->InterfaceType->Data4[2], QueryInterface->InterfaceType->Data4[3],
                 QueryInterface->InterfaceType->Data4[4], QueryInterface->InterfaceType->Data4[5],
                 QueryInterface->InterfaceType->Data4[6], QueryInterface->InterfaceType->Data4[7],
                 (unsigned)QueryInterface->Version, (unsigned)QueryInterface->Size);
    } else {
        WM_TRACE_EXIT_S("QueryInterface(null)", STATUS_NOT_SUPPORTED);
    }
    return STATUS_NOT_SUPPORTED;
}

VOID APIENTRY
WinMaliKmdControlEtwLogging(
    _In_ BOOLEAN Enable,
    _In_ ULONG   Flags,
    _In_ UCHAR   Level)
{
    UNREFERENCED_PARAMETER(Enable);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Level);
}

/* WinMaliKmdQueryAdapterInfo moved to WinMaliQai.c - cap-walk handler. */









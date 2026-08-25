/*
 * WinMaliVidPn.c — the WDDM display (VidPN) DDIs.
 *
 * WHY THIS EXISTS: Windows will not expose this GPU to DirectX unless the adapter
 * presents itself as a DISPLAY adapter. Measured, one DDI at a time: dxgk accepts
 * each piece we supply and then stops at the next NULL entry. With a valid child
 * descriptor in place, the VidPN set is what it wants next.
 *
 * POSTURE: we support EXACTLY ONE configuration — source 0 -> target 0, one fixed
 * mode (see WinMaliVidPn.h), scanning out of the framebuffer our UEFI firmware
 * already lit. There is no hardware page-flip and no DISP programming yet, so the
 * rule everywhere is: accept the one valid configuration, reject everything else
 * with the RIGHT status, and never fail a turn-off.
 *
 * ⛔ We cannot get a crash dump on this machine (UFS has no dump mode), so a
 * bugcheck here costs a physical recovery. The hazards called out in comments
 * below are the ones that kill device start; do not "simplify" them away.
 */

#include "WinMaliKmd.h"
#include "WinMaliVidPn.h"

/* Visible state shared with SetVidPnSourceAddress, which can run at DIRQL.
   ⛔ Interlocked only -- a fast mutex/ERESOURCE taken here and again there
   bugchecks immediately. */
static volatile LONG g_WinMaliSourceVisible = 1;
/* ⛔ Do NOT seed this with WINMALI_FB_SCANOUT_PA. It holds the address dxgk
   programs IN OUR SEGMENT; WINMALI_FB_SCANOUT_PA is a CPU physical address for
   the firmware framebuffer. Seeding it with the latter made the "no flip yet"
   default indistinguishable from a real flip, and in the wrong address space.
   Zero means "dxgk has not programmed a flip". */
static volatile LONG64 g_WinMaliScanoutPa   = 0;

/* ---------------------------------------------------------------------- */
/* helpers                                                                 */
/* ---------------------------------------------------------------------- */

/* Obtain the per-call VidPN interface. ⚠ hVidPn, the interface pointer and every
   handle/mode/path derived from them are valid ONLY until this DDI returns --
   never cache them. */
static NTSTATUS
WinMaliAcquireVidPnIface(_In_ PWINMALI_ADAPTER Adapter,
                         _In_ D3DKMDT_HVIDPN hVidPn,
                         _Out_ CONST DXGK_VIDPN_INTERFACE **ppIface)
{
    *ppIface = NULL;
    if (Adapter == NULL || Adapter->DxgkInterface.DxgkCbQueryVidPnInterface == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    return Adapter->DxgkInterface.DxgkCbQueryVidPnInterface(
               hVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, ppIface);
}

/* Fill our one source mode. ⛔ Do NOT RtlZeroMemory the whole struct:
   pfnCreateNewModeInfo hands back an object whose Id is already OS-assigned and
   is the FIRST member. Zeroing it also poisons Type/ColorBasis/PVAM. */
static VOID
WinMaliFillSourceMode(_Inout_ D3DKMDT_VIDPN_SOURCE_MODE *pMode)
{
    pMode->Type = D3DKMDT_RMT_GRAPHICS;
    pMode->Format.Graphics.PrimSurfSize.cx      = WINMALI_FB_WIDTH;
    pMode->Format.Graphics.PrimSurfSize.cy      = WINMALI_FB_HEIGHT;
    pMode->Format.Graphics.VisibleRegionSize.cx = WINMALI_FB_WIDTH;
    pMode->Format.Graphics.VisibleRegionSize.cy = WINMALI_FB_HEIGHT;
    pMode->Format.Graphics.Stride               = WINMALI_FB_STRIDE;
    /* See Parameters\SrcModeFormat in WinMaliDriver.c. Default 0 keeps today's
       X8R8G8B8 exactly, so an unset knob is a no-op. */
    pMode->Format.Graphics.PixelFormat          =
        (g_WinMaliSrcModeFormat != 0) ? D3DDDIFMT_A8R8G8B8 : D3DDDIFMT_X8R8G8B8;
    pMode->Format.Graphics.ColorBasis           = WINMALI_COLOR_BASIS;
    pMode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

/* Fill our one target mode's signal info.
   ⛔ ScanLineOrdering is a 3-bit bitfield that ALIASES a full-width member. Zero
   the block first, then write ONLY the AdditionalSignalInfo arm -- assigning the
   full-width alias afterwards clobbers VSyncFreqDivider, and VSSLO_OTHER (255)
   would silently truncate to 7. */
static VOID
WinMaliFillSignalInfo(_Out_ D3DKMDT_VIDEO_SIGNAL_INFO *pSig)
{
    RtlZeroMemory(pSig, sizeof(*pSig));
    pSig->VideoStandard     = D3DKMDT_VSS_OTHER;
    /* ⛔ TotalSize is the BLANKED total, not the active area. It used to equal
       ActiveSize (zero blanking), which forced a pixel clock of 325,017,600 Hz -
       a value an EDID DTD cannot encode (10 kHz granularity), so the EDID said
       325,020,000 and the two descriptions of this one mode disagreed. Windows
       reported the resulting monitor mode back to us as
       pixelrate=325020000 vsync=2031375/33856, non-cofunctional with this
       target mode, and committed an empty topology instead. */
    pSig->TotalSize.cx      = WINMALI_H_TOTAL;
    pSig->TotalSize.cy      = WINMALI_V_TOTAL;
    pSig->ActiveSize.cx     = WINMALI_FB_WIDTH;
    pSig->ActiveSize.cy     = WINMALI_FB_HEIGHT;
    pSig->VSyncFreq.Numerator   = WINMALI_VSYNC_NUM;
    pSig->VSyncFreq.Denominator = WINMALI_VSYNC_DEN;
    pSig->HSyncFreq.Numerator   = WINMALI_HSYNC_NUM;
    pSig->HSyncFreq.Denominator = WINMALI_HSYNC_DEN;
    pSig->PixelRate         = WINMALI_PIXEL_RATE;
    pSig->ScanLineOrdering  = D3DDDI_VSSLO_PROGRESSIVE;
}

static VOID
WinMaliFillTargetMode(_Inout_ D3DKMDT_VIDPN_TARGET_MODE *pMode)
{
    WinMaliFillSignalInfo(&pMode->VideoSignalInfo);

    /* FREE FALSIFIER -- read the union BEFORE we touch it. If dxgk pre-populates
       the wire-format masks in pfnCreateNewModeInfo this reads non-zero and the
       "we declare no wire format" theory is dead with no rebuild. */
    WinMaliVpLogOnly_(WINMALI_VPC_TGTWIRE_IN |
        (ULONG)(pMode->WireFormatAndPreference.Value & 0x00FFFFFFu));

    /* WARNING: Preference is bits 0-1 of the SAME 32-bit word as the wire-format
       masks. Write it FIRST, then only the disjoint Rgb field -- assigning
       .Value afterwards would clear the preference we just set. */
    pMode->Preference = D3DKMDT_MP_PREFERRED;
    if ((g_WinMaliTgtWireFmt & 1u) != 0) {
        pMode->WireFormatAndPreference.Rgb = D3DKMDT_BITS_PER_COMPONENT_08;
    }
    if ((g_WinMaliTgtWireFmt & 2u) != 0) {
        /* The nominal rate, not a floor we cannot hold: our vsync is synthesised
           from a KTIMER at exactly this rate. */
        pMode->MinimumVSyncFreq.Numerator   = WINMALI_VSYNC_NUM;
        pMode->MinimumVSyncFreq.Denominator = WINMALI_VSYNC_DEN;
    }
}

/* Publish our single mode into a fresh source mode set and assign it.
   ⛔ On SUCCESS pfnAddMode RELEASES the mode info ("rendering it inaccessible") --
   releasing it again is a double free, and reading ->Id afterwards reads garbage.
   Capture Id before AddMode; null the pointer inside the success branch. */
static NTSTATUS
WinMaliPublishSourceModeSet(_In_ PWINMALI_ADAPTER Adapter,
                            _In_ CONST DXGK_VIDPN_INTERFACE *pIface,
                            _In_ D3DKMDT_HVIDPN hVidPn,
                            _In_ D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet  = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pSetIface = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE *pMode  = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Adapter);

    status = pIface->pfnCreateNewSourceModeSet(hVidPn, SourceId, &hSet, &pSetIface);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = pSetIface->pfnCreateNewModeInfo(hSet, &pMode);
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    WinMaliFillSourceMode(pMode);

    status = pSetIface->pfnAddMode(hSet, pMode);
    if (NT_SUCCESS(status)) {
        pMode = NULL;            /* AddMode consumed it -- see comment above */
    } else {
        goto Cleanup;
    }

    status = pIface->pfnAssignSourceModeSet(hVidPn, SourceId, hSet);
    if (NT_SUCCESS(status)) {
        hSet = NULL;             /* ownership transferred to the VidPN */
    }

Cleanup:
    if (pMode != NULL) {
        (VOID)pSetIface->pfnReleaseModeInfo(hSet, pMode);
    }
    if (hSet != NULL) {
        (VOID)pIface->pfnReleaseSourceModeSet(hVidPn, hSet);
    }
    return status;
}

static NTSTATUS
WinMaliPublishTargetModeSet(_In_ PWINMALI_ADAPTER Adapter,
                            _In_ CONST DXGK_VIDPN_INTERFACE *pIface,
                            _In_ D3DKMDT_HVIDPN hVidPn,
                            _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pSetIface = NULL;
    D3DKMDT_VIDPN_TARGET_MODE *pMode = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Adapter);

    status = pIface->pfnCreateNewTargetModeSet(hVidPn, TargetId, &hSet, &pSetIface);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = pSetIface->pfnCreateNewModeInfo(hSet, &pMode);
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    WinMaliFillTargetMode(pMode);

    status = pSetIface->pfnAddMode(hSet, pMode);
    if (NT_SUCCESS(status)) {
        pMode = NULL;
    } else {
        goto Cleanup;
    }

    status = pIface->pfnAssignTargetModeSet(hVidPn, TargetId, hSet);
    if (NT_SUCCESS(status)) {
        hSet = NULL;
    }

Cleanup:
    if (pMode != NULL) {
        (VOID)pSetIface->pfnReleaseModeInfo(hSet, pMode);
    }
    if (hSet != NULL) {
        (VOID)pIface->pfnReleaseTargetModeSet(hVidPn, hSet);
    }
    return status;
}

/* ---------------------------------------------------------------------- */
/* DDIs                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * VidPn transaction trail.
 *
 * The desktop is stuck on one frame and LiveSrcVisible reads 0 while
 * LiveSrcVisEver reads 1: the OS made our source visible, then switched it off
 * and never switched it back. What it did in between decides the fix, and that
 * is a question about ORDER, which free-running counters cannot answer - "went
 * invisible, then committed an empty topology" (the OS turning the display off)
 * and "committed a topology, then went invisible" (a mode set we mishandled)
 * produce identical counts.
 *
 * Soeach event pushes a 4-bit code into one DWORD: StatVpTrail carries the LAST
 * EIGHT events IN ORDER, oldest nibble leftmost. Read it as hex.
 *   3 = CommitVidPn (empty/off)      4 = CommitVidPn (real path)
 *   5 = Visibility TRUE              6 = Visibility FALSE
 *   7 = UpdateActiveVidPnPresentPath
 *   8 = StartDevice                  9 = StopDevice
 *
 * ⛔ IsSupportedVidPn IS DELIBERATELY NOT IN THE TRAIL. It was, and it is polled
 * continuously - 43 calls in one boot flooded all eight slots (0x11111111) and
 * erased every rare event the trail exists to capture. A ring this small must
 * only ever record events that are RARE. Its counts are kept separately.
 *
 * StartDevice/StopDevice are in the trail because the adapter can restart
 * mid-boot, and all of these counters are GLOBALS shared across instances: "2
 * visible-ON and 2 visible-OFF" reads completely differently if an adapter stop
 * sits between them.
 * Registry names are nearly exhausted (121 of a hard 127), which is the other
 * reason this is one value rather than seven.
 */
LONG g_WinMaliStatVpTrail       = 0;
LONG g_WinMaliStatVpIsSupYes    = 0;
LONG g_WinMaliStatVpIsSupNo     = 0;
LONG g_WinMaliStatVpCommitPath  = 0;
LONG g_WinMaliStatVpCommitEmpty = 0;
LONG g_WinMaliStatVpVisOn       = 0;
LONG g_WinMaliStatVpVisOff      = 0;
LONG g_WinMaliStatVpVisOnMs     = 0;
LONG g_WinMaliStatVpVisOffMs    = 0;

/* ⛔★★★★★ THE NIBBLE TRAIL HOLDS EIGHT EVENTS AND NO TIMES.
 *
 * StatVpTrail told us the ORDER (ON,ON,OFF,OFF,commit,ON,OFF,OFF) and StatVpVis
 * confirmed 3 ON / 4 OFF, so those eight nibbles happen to be the COMPLETE
 * history - but only the LAST on and the LAST off carry a timestamp, and both of
 * those turned out to be a user-triggered GPU reset 23 minutes in. The pair that
 * actually froze the desktop has no time attached to it at all, so we cannot say
 * whether it lands at logon, at shell start, or at taskbar arrival - and those
 * have completely different causes.
 *
 * This is a FILL-ONCE log: it records the first WINMALI_VPLOG_MAX events after
 * StartDevice and then stops. Fill-once is deliberate - the interesting window is
 * the first minute of a boot, and a ring would let 50 minutes of idle churn evict
 * exactly the events being hunted. g_WinMaliVpLogN keeps counting past the cap so
 * "how many events happened" and "how many we kept" stay distinguishable.
 *
 * Written at DIRQL from the visibility/address paths: no allocation, no lock, one
 * InterlockedIncrement. The slot is claimed before it is filled, so a reader can
 * see a half-written entry - it is published from the worker at PASSIVE and read
 * offline, which makes that harmless. */
/* Exported by ntoskrnl since XP but not declared in wdm.h. Returns the 15-char
   image file name (no path). Same declaration as WinMaliDdi.c:24. */
NTKERNELAPI PCHAR PsGetProcessImageFileName(_In_ PEPROCESS Process);

/* Same reason as the line above: declared in ntddk.h, which this TU does not pull
   in (build failed C4013). Needed to name the REAL caller of
   SetVidPnSourceVisibility - PsGetCurrentProcess() follows KeStackAttachProcess
   and would name dxgk's attach target (csrss) instead. */
NTKERNELAPI NTSTATUS PsLookupProcessByProcessId(_In_ HANDLE ProcessId,
                                                _Outptr_ PEPROCESS *Process);

/* Storage for the log declared in WinMaliKmd.h. */
WINMALI_VPLOG_ENT g_WinMaliVpLog[WINMALI_VPLOG_MAX];
LONG              g_WinMaliVpLogN = 0;
ULONG64 g_WinMaliAdapterStartTick = 0;

/* Milliseconds since StartDevice. KeQueryInterruptTime is monotonic from BOOT
   and is immune to this box's RTC resetting to ~18:59 every power cycle, which
   has already made me misattribute one log. */
ULONG
WinMaliMsSinceAdapterStart_(VOID)
{
    ULONG64 now = KeQueryInterruptTime();
    if (g_WinMaliAdapterStartTick == 0 || now < g_WinMaliAdapterStartTick) {
        return 0;
    }
    return (ULONG)((now - g_WinMaliAdapterStartTick) / 10000ull);
}

/* Lock-free: runs at DIRQL on the visibility/address paths. */
VOID
WinMaliVpTrail_(_In_ ULONG Code)
{
    LONG old, neu;
    do {
        old = g_WinMaliStatVpTrail;
        neu = (LONG)(((ULONG)old << 4) | (Code & 0xFu));
    } while (InterlockedCompareExchange(&g_WinMaliStatVpTrail, neu, old) != old);

    /* Same hook, richer record. Every interesting DDI already calls this, so the
       log costs no new call sites and cannot drift out of sync with the trail. */
    WinMaliVpLogOnly_(Code);
}

/* Append to the timestamped log WITHOUT touching the 4-bit nibble trail.
   Lock-free and DIRQL-safe: one InterlockedIncrement, no allocation, no wait.
   KeQueryInterruptTime is callable at any IRQL. */
VOID
WinMaliVpLogOnly_(_In_ ULONG Code)
{
    LONG i = InterlockedIncrement(&g_WinMaliVpLogN) - 1;
    if (i >= 0 && i < (LONG)WINMALI_VPLOG_MAX) {
        g_WinMaliVpLog[i].Ms   = WinMaliMsSinceAdapterStart_();
        g_WinMaliVpLog[i].Code = Code;   /* Code LAST: a reader that sees a
                                            non-zero Code has a valid Ms */
    }
}


/* The OUT BOOLEAN is the answer channel. ⛔ Never return a STATUS_GRAPHICS_* code
   to mean "unsupported" -- that is a hard failure, not an answer. */
NTSTATUS APIENTRY
WinMaliKmdIsSupportedVidPn(_In_ CONST HANDLE hAdapter,
                           _Inout_ DXGKARG_ISSUPPORTEDVIDPN *pArgs)
{
    WinMaliVpLogOnly_(WINMALI_VPC_ISSUPPORTED);
    WinMaliDodDdi(WINMALI_DOD_DDI_ISSUPPORTED);
    if (pArgs != NULL) {
        WinMaliVpLogOnly_(WINMALI_VPC_ISSUP_VIDPN |
            (ULONG)((ULONG_PTR)pArgs->hDesiredVidPn & 0x00FFFFFFull));
    }
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromDxgkHandle(hAdapter);
    CONST DXGK_VIDPN_INTERFACE *pIface = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopo = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pTopoIface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath = NULL;
    SIZE_T numPaths = 0;
    NTSTATUS status;

    if (adapter == NULL || pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    pArgs->IsVidPnSupported = FALSE;      /* FIRST -- every early exit is "no" */

    status = WinMaliAcquireVidPnIface(adapter, pArgs->hDesiredVidPn, &pIface);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = pIface->pfnGetTopology(pArgs->hDesiredVidPn, &hTopo, &pTopoIface);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = pTopoIface->pfnGetNumPaths(hTopo, &numPaths);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (numPaths == 0) {
        /*
         * The EMPTY VidPN is always supported -- it is the "display off" /
         * sleep / adapter-stop topology, and it is trivially extendable to a
         * functional VidPN.
         *
         * ⛔ This used to fall into the `!= 1` case below and report
         * IsVidPnSupported = FALSE, which CONTRADICTS CommitVidPn: that DDI
         * explicitly accepts the empty topology so a turn-off can succeed.
         * dxgk calls THIS one first, so the disagreement failed the whole
         * mode-set transaction before CommitVidPn was ever reached.
         */
        pArgs->IsVidPnSupported = TRUE;
        (VOID)InterlockedIncrement(&g_WinMaliStatVpIsSupYes);
        /* WARNING: this YES is counted in the SAME counter as a real approval,
           so "49 YES / 0 NO" cannot distinguish "we approved 49 real topologies"
           from "the OS asked about 49 empty ones". Mark it. */
        WinMaliVpLogOnly_(WINMALI_VPC_ISSUP_EMPTY);
        return STATUS_SUCCESS;
    }
    if (numPaths != 1) {
        /* Not ours, but a perfectly legal question. Costs no acquire. */
        (VOID)InterlockedIncrement(&g_WinMaliStatVpIsSupNo);
        return STATUS_SUCCESS;
    }

    /* ⛔ pfnAcquirePathInfo(0,0) rather than the First/Next enumeration: the
       enumeration terminator is SEVERITY_SUCCESS, so a `while (NT_SUCCESS(...))`
       loop never ends and leaks an OS object per pass. */
    status = pTopoIface->pfnAcquirePathInfo(hTopo, WINMALI_SOURCE_ID,
                                            WINMALI_TARGET_ID, &pPath);
    if (status == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY) {
        (VOID)InterlockedIncrement(&g_WinMaliStatVpIsSupNo);
        return STATUS_SUCCESS;            /* one path, but not source0->target0 */
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* ⛔ CENSUS ONLY - THE ANSWER BELOW IS UNCHANGED.
     * The OS proposes 39 distinct VidPns and we approve every one on topology
     * alone, never inspecting the pinned modes. Before changing WHAT we answer,
     * record WHAT WE ARE BEING ASKED: if the candidates carry modes we cannot
     * do, our blanket YES is feeding the search; if they carry OUR mode, then
     * IsSupportedVidPn is exonerated and the search is about something else.
     * Same acquire/release discipline as WinMaliKmdCommitVidPn. */
    WinMaliVpLogOnly_(WINMALI_VPC_ISSUP_XFORM |
        ((ULONG)pPath->ContentTransformation.Scaling  & 0xFFu) |
        (((ULONG)pPath->ContentTransformation.Rotation & 0xFFu) << 8));
    {
        D3DKMDT_HVIDPNSOURCEMODESET hSrcSet = NULL;
        CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pSrcIf = NULL;
        CONST D3DKMDT_VIDPN_SOURCE_MODE *pSrcPin = NULL;
        D3DKMDT_HVIDPNTARGETMODESET hTgtSet = NULL;
        CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pTgtIf = NULL;
        CONST D3DKMDT_VIDPN_TARGET_MODE *pTgtPin = NULL;

        if (NT_SUCCESS(pIface->pfnAcquireSourceModeSet(pArgs->hDesiredVidPn,
                                                       WINMALI_SOURCE_ID,
                                                       &hSrcSet, &pSrcIf))) {
            if (NT_SUCCESS(pSrcIf->pfnAcquirePinnedModeInfo(hSrcSet, &pSrcPin)) &&
                pSrcPin != NULL) {
                WinMaliVpLogOnly_(WINMALI_VPC_ISSUP_SRC |
                    (((ULONG)pSrcPin->Format.Graphics.PrimSurfSize.cx & 0xFFFu) << 12) |
                     ((ULONG)pSrcPin->Format.Graphics.PrimSurfSize.cy & 0xFFFu));
                (VOID)pSrcIf->pfnReleaseModeInfo(hSrcSet, pSrcPin);
            } else {
                WinMaliVpLogOnly_(WINMALI_VPC_ISSUP_SRC | 0xFFFFFFu);
            }
            (VOID)pIface->pfnReleaseSourceModeSet(pArgs->hDesiredVidPn, hSrcSet);
        }
        if (NT_SUCCESS(pIface->pfnAcquireTargetModeSet(pArgs->hDesiredVidPn,
                                                       WINMALI_TARGET_ID,
                                                       &hTgtSet, &pTgtIf))) {
            if (NT_SUCCESS(pTgtIf->pfnAcquirePinnedModeInfo(hTgtSet, &pTgtPin)) &&
                pTgtPin != NULL) {
                WinMaliVpLogOnly_(WINMALI_VPC_ISSUP_TGT |
                    (((ULONG)pTgtPin->VideoSignalInfo.ActiveSize.cx & 0xFFFu) << 12) |
                     ((ULONG)pTgtPin->VideoSignalInfo.ActiveSize.cy & 0xFFFu));
                (VOID)pTgtIf->pfnReleaseModeInfo(hTgtSet, pTgtPin);
            } else {
                WinMaliVpLogOnly_(WINMALI_VPC_ISSUP_TGT | 0xFFFFFFu);
            }
            (VOID)pIface->pfnReleaseTargetModeSet(pArgs->hDesiredVidPn, hTgtSet);
        }
    }

    pArgs->IsVidPnSupported = TRUE;
    (VOID)InterlockedIncrement(&g_WinMaliStatVpIsSupYes);
    (VOID)pTopoIface->pfnReleasePathInfo(hTopo, pPath);
    return STATUS_SUCCESS;
}

/* We have exactly one mode; there is nothing to recommend. ⚠ The header's own
   comment names STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN, which does not exist in
   ntstatus.h -- do not copy it. */
NTSTATUS APIENTRY
WinMaliKmdRecommendFunctionalVidPn(_In_ CONST HANDLE hAdapter,
                                   _In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pArgs)
{
    WinMaliVpLogOnly_(WINMALI_VPC_RECOMMEND);
    WinMaliDodDdi(WINMALI_DOD_DDI_RECOMMENDVIDPN);
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

/* ⛔ Never touch or release hFallbackTopology -- it is the OS's, and there is no
   topology release anywhere in the interface. */
NTSTATUS APIENTRY
WinMaliKmdRecommendVidPnTopology(_In_ CONST HANDLE hAdapter,
                                 _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pArgs)
{
    /* We refuse to propose a topology. Whether the OS even ASKS has never been
       recorded, and "the driver was asked to propose and said no" is a very
       different story from "the driver was never asked". */
    WinMaliVpLogOnly_(WINMALI_VPC_RECTOPO);
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    return STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY;
}

/*
 * The hard one. Per endpoint:
 *   - skip it entirely if it is the PIVOT (the OS is holding it fixed);
 *   - if a mode is already PINNED there, TOUCH NOTHING. Assigning a fresh set
 *     returns STATUS_GRAPHICS_PINNED_MODE_MUST_REMAIN_IN_SET, because the modes
 *     dxgk pinned are keyed by OS-assigned Id and a new set cannot contain them;
 *   - only when nothing is pinned do we publish our single mode.
 */
NTSTATUS APIENTRY
WinMaliKmdEnumVidPnCofuncModality(_In_ CONST HANDLE hAdapter,
                                  _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pArgs)
{
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromDxgkHandle(hAdapter);
    WinMaliVpLogOnly_(WINMALI_VPC_COFUNC);
    WinMaliDodDdi(WINMALI_DOD_DDI_COFUNCMODALITY);
    /* See WINMALI_VPC_COFUNC_VIDPN. Which VidPn, and what is the OS holding
       fixed? A pivot tells us which end it considers already decided. */
    if (pArgs != NULL) {
        WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_VIDPN |
            (ULONG)((ULONG_PTR)pArgs->hConstrainingVidPn & 0x00FFFFFFull));
        WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_PIVOT |
            ((ULONG)pArgs->EnumPivotType & 0xFFu) |
            (((ULONG)pArgs->EnumPivot.VidPnSourceId & 0xFFu) << 8) |
            (((ULONG)pArgs->EnumPivot.VidPnTargetId & 0xFFu) << 16));
    }
    CONST DXGK_VIDPN_INTERFACE *pIface = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopo = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pTopoIface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath = NULL;
    D3DKMDT_VIDPN_PRESENT_PATH local;
    NTSTATUS status;

    if (adapter == NULL || pArgs == NULL) {
        WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_FAIL | 1u);
        return STATUS_INVALID_PARAMETER;
    }

    status = WinMaliAcquireVidPnIface(adapter, pArgs->hConstrainingVidPn, &pIface);
    if (!NT_SUCCESS(status)) {
        WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_FAIL | 2u);
        return status;
    }
    status = pIface->pfnGetTopology(pArgs->hConstrainingVidPn, &hTopo, &pTopoIface);
    if (!NT_SUCCESS(status)) {
        WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_FAIL | 3u);
        return status;
    }
    /*
     * ⛔⛔ NULL-DEREF GUARD. `!NT_SUCCESS()` IS NOT ENOUGH HERE.
     *
     * Several D3DKMDT graphics codes are SEVERITY_SUCCESS -- e.g.
     * STATUS_GRAPHICS_DATASET_IS_EMPTY (0x401E034B) -- so they PASS
     * NT_SUCCESS() while leaving pPath NULL, and the very next statement
     * dereferences pPath->VidPnSourceId. That is a bugcheck, and on this
     * machine a bugcheck costs a physical recovery (UFS has no dump mode).
     *
     * This file already knows the trap: WINMALI_IS_MODE_NOT_PINNED below does
     * exactly this test for the pinned-mode call. It simply was not applied
     * here. Everything in this DDI had never executed until today.
     *
     * An empty topology is also not an error -- there is simply nothing to
     * enumerate, and reporting failure fails the whole OS mode-set transaction.
     */
    {
        SIZE_T numPaths = 0;

        status = pTopoIface->pfnGetNumPaths(hTopo, &numPaths);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (numPaths == 0) {
            return STATUS_SUCCESS;
        }
    }

    status = pTopoIface->pfnAcquirePathInfo(hTopo, WINMALI_SOURCE_ID,
                                            WINMALI_TARGET_ID, &pPath);
    if (status == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY) {
        /* ⛔ OUR PATH IS NOT IN THIS VidPN. This is logged, not silent: if the
           OS is enumerating configurations that do not contain our source at
           all, that is the whole answer to why it switched us off - and it is
           indistinguishable from "success" in every counter we had. */
        WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_FAIL | 4u);
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
        WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_FAIL | 5u);
        return status;
    }
    if (pPath == NULL) {
        /* SEVERITY_SUCCESS with no path. Do not touch it. */
        return STATUS_SUCCESS;
    }

    /* ---- source ---- */
    if (!(pArgs->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE &&
          pArgs->EnumPivot.VidPnSourceId == pPath->VidPnSourceId)) {

        D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
        CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pSetIface = NULL;
        CONST D3DKMDT_VIDPN_SOURCE_MODE *pPinned = NULL;

        status = pIface->pfnAcquireSourceModeSet(pArgs->hConstrainingVidPn,
                                                 pPath->VidPnSourceId,
                                                 &hSet, &pSetIface);
        if (NT_SUCCESS(status)) {
            NTSTATUS pinStatus = pSetIface->pfnAcquirePinnedModeInfo(hSet, &pPinned);

            /* WHAT is pinned here, if anything? 0xFFFFFF = nothing pinned, which
               is the case where we publish our own mode. Anything else means the
               OS has already decided, and we must match it or the VidPn dies. */
            WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_PINSRC |
                ((NT_SUCCESS(pinStatus) && pPinned != NULL)
                    ? ((((ULONG)pPinned->Format.Graphics.PrimSurfSize.cx & 0xFFFu) << 12) |
                        ((ULONG)pPinned->Format.Graphics.PrimSurfSize.cy & 0xFFFu))
                    : 0x00FFFFFFu));

            /* ⛔ MODE_NOT_PINNED is SEVERITY_SUCCESS -- test it explicitly. */
            if (WINMALI_IS_MODE_NOT_PINNED(pinStatus) ||
                (NT_SUCCESS(pinStatus) && pPinned == NULL)) {
                /* release BEFORE creating a replacement set */
                (VOID)pIface->pfnReleaseSourceModeSet(pArgs->hConstrainingVidPn, hSet);
                hSet = NULL;
                status = WinMaliPublishSourceModeSet(adapter, pIface,
                                                     pArgs->hConstrainingVidPn,
                                                     pPath->VidPnSourceId);
            } else if (NT_SUCCESS(pinStatus)) {
                (VOID)pSetIface->pfnReleaseModeInfo(hSet, pPinned);
            } else {
                /* A HARD pin failure matched neither arm and was silently
                   dropped -- the mode set was then left unpublished while we
                   went on to report success. */
                status = pinStatus;
            }
            if (hSet != NULL) {
                (VOID)pIface->pfnReleaseSourceModeSet(pArgs->hConstrainingVidPn, hSet);
            }
            if (!NT_SUCCESS(status)) {
                goto Done;
            }
        } else {
            /*
             * ⛔ THIS `else` WAS MISSING, AND ITS ABSENCE MANUFACTURED A LIE.
             * On a failed pfnAcquireSourceModeSet we fell straight through to
             * the target block, where `status` is overwritten by the next call
             * (and finally by pfnUpdatePathSupportInfo, which succeeds). The DDI
             * then returned STATUS_SUCCESS having published NO source mode set.
             * On a driver whose only evidence channel is a log ring, a false
             * SUCCESS points the next investigation at the wrong DDI entirely.
             */
            goto Done;
        }
    }

    /* ---- target ---- */
    if (!(pArgs->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET &&
          pArgs->EnumPivot.VidPnTargetId == pPath->VidPnTargetId)) {

        D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
        CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pSetIface = NULL;
        CONST D3DKMDT_VIDPN_TARGET_MODE *pPinned = NULL;

        status = pIface->pfnAcquireTargetModeSet(pArgs->hConstrainingVidPn,
                                                 pPath->VidPnTargetId,
                                                 &hSet, &pSetIface);
        if (NT_SUCCESS(status)) {
            NTSTATUS pinStatus = pSetIface->pfnAcquirePinnedModeInfo(hSet, &pPinned);

            WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_PINTGT |
                ((NT_SUCCESS(pinStatus) && pPinned != NULL)
                    ? ((((ULONG)pPinned->VideoSignalInfo.ActiveSize.cx & 0xFFFu) << 12) |
                        ((ULONG)pPinned->VideoSignalInfo.ActiveSize.cy & 0xFFFu))
                    : 0x00FFFFFFu));

            if (WINMALI_IS_MODE_NOT_PINNED(pinStatus) ||
                (NT_SUCCESS(pinStatus) && pPinned == NULL)) {
                (VOID)pIface->pfnReleaseTargetModeSet(pArgs->hConstrainingVidPn, hSet);
                hSet = NULL;
                status = WinMaliPublishTargetModeSet(adapter, pIface,
                                                     pArgs->hConstrainingVidPn,
                                                     pPath->VidPnTargetId);
            } else if (NT_SUCCESS(pinStatus)) {
                (VOID)pSetIface->pfnReleaseModeInfo(hSet, pPinned);
            } else {
                status = pinStatus;          /* see the source block above */
            }
            if (hSet != NULL) {
                (VOID)pIface->pfnReleaseTargetModeSet(pArgs->hConstrainingVidPn, hSet);
            }
            if (!NT_SUCCESS(status)) {
                goto Done;
            }
        } else {
            goto Done;               /* see the source block above */
        }
    }

    /* ---- path support ----
       ⛔ Copy to a writable local. Casting away const and writing into dxgk's own
       path object corrupts OS state. Claim Identity only: that is consistent with
       reporting DriverScaling/DriverRotation = 0 from QueryVidPnHWCapability. */
    /* ⛔★★★★★ PATCH A - THE MEASUREMENT BLIND SPOT WAS MINE.
     *
     * Every "Scaling=UNPINNED Rotation=UNPINNED" I have reported came from the
     * NOPIVOT / SOURCE / TARGET calls - i.e. from BEFORE the OS pins anything.
     * The only window in which a pinned value could ever appear is
     * pivot=SCALING / pivot=ROTATION, and the ONLY reader of
     * pPath->ContentTransformation lives INSIDE the block those pivots skip.
     * So "the OS never pins" was never measured; it was assumed from a log that
     * is structurally incapable of showing the opposite.
     *
     * Sample it here, outside the gate, on EVERY call. Read-only, PASSIVE_LEVEL,
     * no dxgk call, cannot fail. bit23 tags this as the pre-gate sample so it can
     * never be confused with the post-update entry (which uses only bit16). */
    WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_XFORM |
        ((ULONG)pPath->ContentTransformation.Scaling  & 0xFFu) |
        (((ULONG)pPath->ContentTransformation.Rotation & 0xFFu) << 8) |
        (0x80u << 16));

    /* ⛔★★★★ PATCH B - THE PIVOT EXEMPTION IS PER-ASPECT, NOT PER-CALL.
     *
     * d3dkmddi.h describes the exemption as "the path whose scaling OR rotation
     * support does not need to be updated" - one ASPECT, not the whole call. And
     * d3dkmdt.h says the pivot is the only object guaranteed unchanged;
     * everything else "could have been invalidated". Both support masks are
     * defined as "support given the CURRENTLY PINNED MODES" - and the pinned
     * state is exactly what has just changed when the OS drives these pivots.
     *
     * Today one fused predicate skips the entire block for BOTH pivots, so on a
     * pivot=SCALING call we also fail to refresh ROTATION support, and vice
     * versa - and we never call pfnUpdatePathSupportInfo at all on those calls.
     * Gated on PathXform bit2 so it is reversible from the registry with no
     * rebuild, and so the one-shot fuse clears it if a boot goes wrong. */
    {
        BOOLEAN idsMatch = (BOOLEAN)
            (pArgs->EnumPivot.VidPnSourceId == pPath->VidPnSourceId &&
             pArgs->EnumPivot.VidPnTargetId == pPath->VidPnTargetId);
        BOOLEAN pivScal = (BOOLEAN)
            ((pArgs->EnumPivotType == D3DKMDT_EPT_SCALING) && idsMatch);
        BOOLEAN pivRot  = (BOOLEAN)
            ((pArgs->EnumPivotType == D3DKMDT_EPT_ROTATION) && idsMatch);
        BOOLEAN splitGate = (BOOLEAN)((g_WinMaliPathXform & 4u) != 0);

    if (splitGate || !(pivScal || pivRot)) {

        local = *pPath;
        local.ContentTransformation.ScalingSupport.Identity  = 1;
        local.ContentTransformation.RotationSupport.Identity = 1;
        /* See Parameters\PathXform. Source size == target size, so Centered,
           Stretched and AspectRatioCenteredMax all degenerate to the identity
           transform -- there is nothing extra to implement, and refusing them
           is what leaves a path the OS cannot complete. */
        /* bit0 = the extra SCALING modes. This is the one the evidence points
           at: the only VidPn ever committed arrived with Scaling already pinned
           to IDENTITY, while every VidPn the OS abandons arrives UNPINNED and is
           then driven through pivot=SCALING / pivot=ROTATION without ever
           resolving. A path whose Scaling stays UNPINNED is not committable.
           Source size == target size here, so all three are the identity
           transform - nothing extra to implement. */
        if ((g_WinMaliPathXform & 1u) != 0 && !pivScal) {
            local.ContentTransformation.ScalingSupport.Centered               = 1;
            local.ContentTransformation.ScalingSupport.Stretched              = 1;
            local.ContentTransformation.ScalingSupport.AspectRatioCenteredMax = 1;
        }
        /* bit1 = Offset0, DELIBERATELY SEPARATE. It is a WDDM1_3
           path-independent-rotation claim and it obliges DDIs we do not
           implement. Do not fold it back in. */
        if ((g_WinMaliPathXform & 2u) != 0) {
            local.ContentTransformation.RotationSupport.Offset0               = 1;
        }
        /* See WINMALI_VPC_COFUNC_SUPPORT. Pack the 5 scaling bits and the
           rotation bits BOTH as they arrive (i.e. what a previous call left) and
           as we are about to write them. */
        {
            ULONG inMask =
                ((ULONG)pPath->ContentTransformation.ScalingSupport.Identity            << 0) |
                ((ULONG)pPath->ContentTransformation.ScalingSupport.Centered            << 1) |
                ((ULONG)pPath->ContentTransformation.ScalingSupport.Stretched           << 2) |
                ((ULONG)pPath->ContentTransformation.ScalingSupport.AspectRatioCenteredMax << 3) |
                ((ULONG)pPath->ContentTransformation.ScalingSupport.Custom              << 4);
            ULONG inRot =
                ((ULONG)pPath->ContentTransformation.RotationSupport.Identity  << 0) |
                ((ULONG)pPath->ContentTransformation.RotationSupport.Rotate90  << 1) |
                ((ULONG)pPath->ContentTransformation.RotationSupport.Rotate180 << 2) |
                ((ULONG)pPath->ContentTransformation.RotationSupport.Rotate270 << 3);
            ULONG outMask =
                ((ULONG)local.ContentTransformation.ScalingSupport.Identity            << 0) |
                ((ULONG)local.ContentTransformation.ScalingSupport.Centered            << 1) |
                ((ULONG)local.ContentTransformation.ScalingSupport.Stretched           << 2) |
                ((ULONG)local.ContentTransformation.ScalingSupport.AspectRatioCenteredMax << 3) |
                ((ULONG)local.ContentTransformation.ScalingSupport.Custom              << 4);
            WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_SUPPORT |
                              (inMask & 0xFFu) |
                              ((inRot & 0xFFu) << 8) |
                              ((outMask & 0xFFu) << 16));
        }

        status = pTopoIface->pfnUpdatePathSupportInfo(hTopo, &local);
        /* WHAT the OS had already put on the path, and whether the update took.
           A pinned Scaling we do not support cannot be reported by any counter
           we own, because nothing about it FAILS. */
        WinMaliVpLogOnly_(WINMALI_VPC_COFUNC_XFORM |
            ((ULONG)pPath->ContentTransformation.Scaling  & 0xFFu) |
            (((ULONG)pPath->ContentTransformation.Rotation & 0xFFu) << 8) |
            ((NT_SUCCESS(status) ? 1u : 0u) << 16));
    }
    }

Done:
    (VOID)pTopoIface->pfnReleasePathInfo(hTopo, pPath);
    /* A cofunc-modality failure fails the WHOLE OS mode-set transaction, and it
       is rare, so it belongs in the trail. If the source goes invisible right
       after one of these, the cause is us. */
    if (!NT_SUCCESS(status)) {
        WinMaliVpTrail_(0xCu);
    }
    return status;
}

/*
 * ⛔⛔ THE TURN-OFF PATHS MUST SUCCEED. dxgk commits an EMPTY topology (and/or no
 * pinned mode) on monitor-off, sleep and adapter stop. Failing those fails the OS
 * display transaction and Windows stops the adapter -- the exact "accepted then
 * stopped" symptom we are trying to escape.
 */
NTSTATUS APIENTRY
WinMaliKmdCommitVidPn(_In_ CONST HANDLE hAdapter,
                      _In_ CONST DXGKARG_COMMITVIDPN *CONST pArgs)
{
    PWINMALI_ADAPTER adapter = WinMaliAdapterFromDxgkHandle(hAdapter);
    CONST DXGK_VIDPN_INTERFACE *pIface = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopo = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pTopoIface = NULL;
    SIZE_T numPaths = 0;
    NTSTATUS status;

    WinMaliDodDdi(WINMALI_DOD_DDI_COMMITVIDPN);

    /* ENTRY marker - see WINMALI_VPC_COMMIT_ENTER. Logged before ANY validation
       so "dxgk asked us to commit" is recorded even when we then refuse. */
    WinMaliVpLogOnly_(WINMALI_VPC_COMMIT_ENTER |
                      (pArgs != NULL ? (pArgs->AffectedVidPnSourceId & 0xFFu)
                                     : 0xFEu));
    /* WHICH VidPn? If the cold boot's second activation belongs to a DIFFERENT
       hVidPn than the one we committed, that is the whole answer. */
    if (pArgs != NULL) {
        WinMaliVpLogOnly_(WINMALI_VPC_COMMIT_VIDPN |
            (ULONG)((ULONG_PTR)pArgs->hFunctionalVidPn & 0x00FFFFFFull));
    }

    if (adapter == NULL || pArgs == NULL) {
        WinMaliVpLogOnly_(WINMALI_VPC_COMMIT_FAIL | 1u);
        return STATUS_INVALID_PARAMETER;
    }
    /* ⛔ Do NOT compare against D3DDDI_ID_NOTAPPLICABLE: it is (UINT)0, which
       collides with our only valid source id. */
    if (pArgs->AffectedVidPnSourceId != WINMALI_SOURCE_ID &&
        pArgs->AffectedVidPnSourceId != D3DDDI_ID_ALL) {
        WinMaliVpLogOnly_(WINMALI_VPC_COMMIT_FAIL | 2u);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }

    status = WinMaliAcquireVidPnIface(adapter, pArgs->hFunctionalVidPn, &pIface);
    if (!NT_SUCCESS(status)) {
        WinMaliVpLogOnly_(WINMALI_VPC_COMMIT_FAIL | 3u);
        return status;
    }
    status = pIface->pfnGetTopology(pArgs->hFunctionalVidPn, &hTopo, &pTopoIface);
    if (!NT_SUCCESS(status)) {
        WinMaliVpLogOnly_(WINMALI_VPC_COMMIT_FAIL | 4u);
        return status;
    }
    status = pTopoIface->pfnGetNumPaths(hTopo, &numPaths);
    if (!NT_SUCCESS(status)) {
        WinMaliVpLogOnly_(WINMALI_VPC_COMMIT_FAIL | 5u);
        return status;
    }
    if (numPaths == 0) {
        (VOID)InterlockedIncrement(&g_WinMaliStatVpCommitEmpty);
        WinMaliVpTrail_(3);
        DbgPrint("[WinMali]    CommitVidPn: empty topology (display off) -> OK\n");
        return STATUS_SUCCESS;                       /* turn-off path */
    }

    /* ⛔★★★★★ RECORD THE PINNED MODES. See WINMALI_VPC_MODE_SRC.
     * Read-only: this does not change what we accept, it only makes visible
     * WHAT we have been accepting. PASSIVE_LEVEL (VidPn interface calls are
     * passive-only) and this DDI runs a couple of times per boot, so the two
     * extra acquires cost nothing. Every handle is released on every path. */
    {
        D3DKMDT_HVIDPNSOURCEMODESET hSrcSet = NULL;
        CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pSrcIf = NULL;
        CONST D3DKMDT_VIDPN_SOURCE_MODE *pSrcPin = NULL;
        D3DKMDT_HVIDPNTARGETMODESET hTgtSet = NULL;
        CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pTgtIf = NULL;
        CONST D3DKMDT_VIDPN_TARGET_MODE *pTgtPin = NULL;

        if (NT_SUCCESS(pIface->pfnAcquireSourceModeSet(pArgs->hFunctionalVidPn,
                                                       WINMALI_SOURCE_ID,
                                                       &hSrcSet, &pSrcIf))) {
            NTSTATUS ps = pSrcIf->pfnAcquirePinnedModeInfo(hSrcSet, &pSrcPin);
            if (NT_SUCCESS(ps) && pSrcPin != NULL) {
                WinMaliVpLogOnly_(WINMALI_VPC_MODE_SRC |
                    (((ULONG)pSrcPin->Format.Graphics.PrimSurfSize.cx & 0xFFFu) << 12) |
                    ((ULONG)pSrcPin->Format.Graphics.PrimSurfSize.cy & 0xFFFu));
                (VOID)pSrcIf->pfnReleaseModeInfo(hSrcSet, pSrcPin);
            } else {
                WinMaliVpLogOnly_(WINMALI_VPC_MODE_NOPIN | 1u);
            }
            (VOID)pIface->pfnReleaseSourceModeSet(pArgs->hFunctionalVidPn, hSrcSet);
        }

        if (NT_SUCCESS(pIface->pfnAcquireTargetModeSet(pArgs->hFunctionalVidPn,
                                                       WINMALI_TARGET_ID,
                                                       &hTgtSet, &pTgtIf))) {
            NTSTATUS pt = pTgtIf->pfnAcquirePinnedModeInfo(hTgtSet, &pTgtPin);
            if (NT_SUCCESS(pt) && pTgtPin != NULL) {
                ULONG num = pTgtPin->VideoSignalInfo.VSyncFreq.Numerator;
                ULONG den = pTgtPin->VideoSignalInfo.VSyncFreq.Denominator;
                WinMaliVpLogOnly_(WINMALI_VPC_MODE_TGT |
                    (((ULONG)pTgtPin->VideoSignalInfo.ActiveSize.cx & 0xFFFu) << 12) |
                    ((ULONG)pTgtPin->VideoSignalInfo.ActiveSize.cy & 0xFFFu));
                WinMaliVpLogOnly_(WINMALI_VPC_MODE_HZ |
                                  ((den != 0) ? (num / den) : 0xFFFFu));
                (VOID)pTgtIf->pfnReleaseModeInfo(hTgtSet, pTgtPin);
            } else {
                WinMaliVpLogOnly_(WINMALI_VPC_MODE_NOPIN | 2u);
            }
            (VOID)pIface->pfnReleaseTargetModeSet(pArgs->hFunctionalVidPn, hTgtSet);
        }
    }

    /* Scanout is a fixed, already-live framebuffer: there is nothing to program.
       Validating and succeeding is the whole job until DISP is brought up. */
    (VOID)InterlockedIncrement(&g_WinMaliStatVpCommitPath);
    WinMaliVpTrail_(4);
    DbgPrint("[WinMali]    CommitVidPn: %u path(s), src=%u -> OK (fixed scanout)\n",
             (unsigned)numPaths, (unsigned)pArgs->AffectedVidPnSourceId);
    return STATUS_SUCCESS;
}

/* ⛔ The path arrives BY VALUE -- it was never acquired, so it must NEVER be
   released. Compare field-by-field, never memcmp: the struct carries a 256-byte
   OEMCopyProtection array and a live pointer union. */
NTSTATUS APIENTRY
WinMaliKmdUpdateActiveVidPnPresentPath(_In_ CONST HANDLE hAdapter,
                                       _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pArgs)
{
    CONST D3DKMDT_VIDPN_PRESENT_PATH *p;

    WinMaliDodDdi(WINMALI_DOD_DDI_UPDATEPATH);
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    p = &pArgs->VidPnPresentPathInfo;

    if (p->VidPnSourceId != WINMALI_SOURCE_ID || p->VidPnTargetId != WINMALI_TARGET_ID) {
        WinMaliVpTrail_(0xDu);
        return STATUS_GRAPHICS_INVALID_VIDPN_PRESENT_PATH;
    }
    /* WARNING: ADVERTISE AND ACCEPT MUST AGREE. EnumVidPnCofuncModality now
       claims Centered / Stretched / AspectRatioCenteredMax when PathXform is on;
       rejecting one here after claiming it would fail the mode set at the last
       step, which is strictly worse than never claiming it. All three are the
       identity transform at equal source and target size. */
    if (p->ContentTransformation.Scaling != D3DKMDT_VPPS_IDENTITY &&
        p->ContentTransformation.Scaling != D3DKMDT_VPPS_UNPINNED &&
        p->ContentTransformation.Scaling != D3DKMDT_VPPS_NOTSPECIFIED &&
        !(g_WinMaliPathXform != 0 &&
          (p->ContentTransformation.Scaling == D3DKMDT_VPPS_CENTERED ||
           p->ContentTransformation.Scaling == D3DKMDT_VPPS_STRETCHED ||
           p->ContentTransformation.Scaling == D3DKMDT_VPPS_ASPECTRATIOCENTEREDMAX))) {
        WinMaliVpTrail_(0xDu);
        return STATUS_GRAPHICS_INVALID_VIDPN_PRESENT_PATH;
    }
    if (p->ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY &&
        p->ContentTransformation.Rotation != D3DKMDT_VPPR_UNPINNED &&
        p->ContentTransformation.Rotation != D3DKMDT_VPPR_NOTSPECIFIED) {
        WinMaliVpTrail_(0xDu);
        return STATUS_GRAPHICS_INVALID_VIDPN_PRESENT_PATH;
    }
    /* Content (incl. D3DKMDT_VPPC_VIDEO), ImportanceOrdinal, colour basis and the
       gamma ramp are accepted and ignored -- rejecting VPPC_VIDEO breaks playback,
       and GammaRamp.Data.pRaw must never be dereferenced. */
    WinMaliVpTrail_(7);
    return STATUS_SUCCESS;
}

/* No sanctioned "cannot blank" status exists -- the documented return set is
   STATUS_SUCCESS alone. Honour the observable half: while invisible, scanout must
   not be updated. */
NTSTATUS APIENTRY
WinMaliKmdSetVidPnSourceVisibility(_In_ CONST HANDLE hAdapter,
                                   _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pArgs)
{
    WinMaliDodDdi(WINMALI_DOD_DDI_SRCVISIBILITY);
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pArgs->VidPnSourceId != WINMALI_SOURCE_ID &&
        pArgs->VidPnSourceId != D3DDDI_ID_ALL) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    InterlockedExchange(&g_WinMaliSourceVisible, pArgs->Visible ? 1 : 0);
    {
        PWINMALI_ADAPTER adapter = WinMaliAdapterFromDxgkHandle(hAdapter);
        if (adapter != NULL) {
            InterlockedExchange(&adapter->SourceVisible, pArgs->Visible ? 1 : 0);
            /* ⚠ THE LAST VALUE IS A TRAP. Teardown always ends with
               Visible=FALSE, so a perfectly healthy run also reports 0 at
               StopDevice - I nearly read "visible=0" as "dxgk never made us
               visible", which it does not prove. Latch whether it was EVER
               true; that is the question actually being asked. */
            /* Distinguish a per-source call from a broadcast: dxgk uses
               D3DDDI_ID_ALL for blanket enable/disable, and "the OS turned OUR
               source off" is a different fact from "the OS blanked everything".
               Separate trail codes cost nothing - the nibble was free - where a
               new registry value would not have been (125 of 127 names used). */
            BOOLEAN all = (pArgs->VidPnSourceId == D3DDDI_ID_ALL);
            /* The raw id alongside the 5/6/0xA/0xB nibble: D3DDDI_ID_ALL reads
               unmistakably as 0x2AFF, so a targeted blank can never be confused
               with a blanket one. */
            WinMaliVpLogOnly_(WINMALI_VPC_VIS_ID |
                              (pArgs->VidPnSourceId & 0xFFu));
            ULONG   nowMs = WinMaliMsSinceAdapterStart_();
            /* ⛔★★★ NAME THE CALLER. This DDI is PASSIVE_LEVEL and runs in the
             * CALLER's context, so PsGetCurrentProcess names whoever is driving
             * the source. MEASURED from ETW: the ON comes from pid 872 and the
             * OFF from pid 1248 - DIFFERENT PROCESSES - and 1248 has already
             * exited by the time WinRM is reachable (~70 s), so it can never be
             * identified from user mode. The RTC also jumps mid-boot on this box,
             * so file timestamps cannot be used to map it either.
             * Capture ONLY, no registry I/O in a DDI: the worker publishes.
             * FIRST occurrence only - teardown always ends with an OFF, and the
             * last value would therefore always name the teardown. */
            {
                /* ⛔⛔★★★★★ v2 "FIXED" THIS BY MAKING BOTH VALUES CONSISTENTLY WRONG.
                 *
                 * v1 took the pid from PsGetCurrentProcessId() and the name from
                 * PsGetCurrentProcess(); they disagreed (pid 1260 reported as
                 * "csrss.exe" while 1260 was demonstrably dwm). v2 took BOTH from
                 * PsGetCurrentProcess() so they would agree - and they now agree
                 * on the WRONG PROCESS: PsGetCurrentProcess()/IoGetCurrentProcess()
                 * follows KeStackAttachProcess, and dxgk ATTACHES to csrss to do
                 * display work. v2 duly reported csrss for BOTH the enable and the
                 * disable, while the ETW trace - whose Execution ProcessID is the
                 * THREAD's own Cid, immune to the attach - showed two DIFFERENT
                 * callers (872 enables, 1248 disables).
                 * ★★★ MAKING TWO READINGS AGREE IS NOT THE SAME AS MAKING THEM
                 *     CORRECT. The disagreement was the SIGNAL.
                 *
                 * v3: the caller is the THREAD's process. PsGetCurrentProcessId()
                 * is attach-immune, so look the object up FROM THAT ID and name
                 * that. Record the attached process separately so the two can
                 * never be conflated again. PASSIVE_LEVEL, so the lookup is legal;
                 * the reference MUST be dropped. */
                PEPROCESS attached = PsGetCurrentProcess();
                PEPROCESS proc = NULL;
                PCHAR img = NULL;
                LONG  cur = HandleToLong(PsGetCurrentProcessId());
                if (NT_SUCCESS(PsLookupProcessByProcessId(PsGetCurrentProcessId(), &proc)) &&
                    proc != NULL) {
                    img = PsGetProcessImageFileName(proc);
                } else {
                    proc = NULL;
                }
                LONG *slotPid  = pArgs->Visible ? &g_WinMaliVisOnPid  : &g_WinMaliVisOffPid;
                UCHAR *slotName = pArgs->Visible ? g_WinMaliVisOnName : g_WinMaliVisOffName;
                if (InterlockedCompareExchange(slotPid, cur, 0) == 0) {
                    ULONG ci;
                    for (ci = 0; ci < 15u; ci++) {
                        slotName[ci] = (img != NULL) ? (UCHAR)img[ci] : (UCHAR)0;
                        if (slotName[ci] == 0) { break; }
                    }
                    slotName[15] = 0;
                    DbgPrint("[WinMali]    SetVidPnSourceVisibility(%s) CALLER pid=%u \"%s\" "
                             "(attached-to \"%s\")\n",
                             pArgs->Visible ? "TRUE" : "FALSE",
                             (unsigned)cur, (img != NULL) ? img : "?",
                             (attached != NULL && PsGetProcessImageFileName(attached) != NULL)
                                 ? PsGetProcessImageFileName(attached) : "?");
                }
                /* ⛔ PsLookupProcessByProcessId TAKES A REFERENCE. Leaking one per
                   visibility change would pin a dead EPROCESS forever. */
                if (proc != NULL) { ObDereferenceObject(proc); }
            }
            if (pArgs->Visible) {
                InterlockedExchange(&adapter->SourceVisibleEver, 1);
                (VOID)InterlockedIncrement(&g_WinMaliStatVpVisOn);
                (VOID)InterlockedExchange(&g_WinMaliStatVpVisOnMs, (LONG)nowMs);
                WinMaliVpTrail_(all ? 0xAu : 5u);
            } else {
                (VOID)InterlockedIncrement(&g_WinMaliStatVpVisOff);
                (VOID)InterlockedExchange(&g_WinMaliStatVpVisOffMs, (LONG)nowMs);
                WinMaliVpTrail_(all ? 0xBu : 6u);
            }
        }
    }
    return STATUS_SUCCESS;
}

/*
 * ⛔ RUNS UP TO PROFILE_LEVEL-1 (DIRQL). The prose comment in the header claiming
 * PASSIVE_LEVEL is stale and wrong. Keep this body and everything it touches
 * NONPAGED: no waits, no allocations, and NO VidPN interface calls (all of those
 * are PASSIVE_LEVEL-only).
 */
NTSTATUS APIENTRY
WinMaliKmdSetVidPnSourceAddress(_In_ CONST HANDLE hAdapter,
                                _In_ CONST DXGKARG_SETVIDPNSOURCEADDRESS *pArgs)
{
    WinMaliVpLogOnly_(WINMALI_VPC_SRCADDR);
    /* See WINMALI_VPC_SRCADDR2 in WinMaliKmd.h. Logged BEFORE the NULL check so a
       malformed call still leaves a trace. */
    if (pArgs != NULL) {
        WinMaliVpLogOnly_(WINMALI_VPC_SRCADDR2 |
            (ULONG)(pArgs->PrimaryAddress.QuadPart & 0x00FFFFFFull));
        WinMaliVpLogOnly_(WINMALI_VPC_SRCADDR_ID |
            ((ULONG)pArgs->VidPnSourceId & 0xFFu) |
            (((ULONG)pArgs->Flags.Value & 0xFFFFu) << 8));
    }
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pArgs->VidPnSourceId != WINMALI_SOURCE_ID) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }

    /*
     * v1: LATCH, do not program. The panel scans out of the framebuffer EDK2
     * lit; we own no DISP block and cannot move a scanout base. There is no
     * documented failure path here, and this can run at DIRQL, so the only
     * correct behaviour is to record and succeed.
     *
     * ⚠ pArgs->PrimaryAddress is dxgk's address IN OUR SEGMENT, not a CPU
     * physical address. It must never be compared with or overwritten by
     * WINMALI_FB_SCANOUT_PA -- those live in different address spaces and
     * conflating them is a multi-GB error waiting for its first consumer.
     * (g_WinMaliScanoutPa used to be SEEDED with WINMALI_FB_SCANOUT_PA, which
     * baked exactly that confusion into the default value.)
     *
     * FlipLatchCount is the ORACLE: nonzero proves dxgk drove the present
     * path. StopDevice publishes it to Parameters\StatFlipLatches.
     */
    {
        PWINMALI_ADAPTER adapter = WinMaliAdapterFromDxgkHandle(hAdapter);

        InterlockedExchange64(&g_WinMaliScanoutPa,
                              (LONG64)pArgs->PrimaryAddress.QuadPart);
        if (adapter != NULL) {
            InterlockedExchange64(&adapter->LastFlipAddress,
                                  (LONG64)pArgs->PrimaryAddress.QuadPart);
            InterlockedExchange(&adapter->LastFlipSegment,
                                (LONG)pArgs->PrimarySegment);
            InterlockedExchange(&adapter->LastFlipFlags,
                                (LONG)pArgs->Flags.Value);
            (VOID)InterlockedIncrement64(&adapter->FlipLatchCount);
        }

        /* ★ dxgk NAMES THE SURFACE IT SCANS OUT - AND WE HAVE BEEN IGNORING IT.
         *
         * The present source is currently CHOSEN BY A HEURISTIC (UvSrcAcquireBest
         * scores imported surfaces and picks the freshest). Measured result: a
         * full desktop with NO taskbar and NO cursor, StatSrcNonBlack 646/646
         * with Chg 0 - i.e. we latched a PRE-COMPOSITION layer that DWM painted
         * once and abandoned. A heuristic cannot tell that from the composed
         * frame; dxgk can, and does, on every single flip.
         *
         * pArgs->hAllocation is the hDeviceSpecificAllocation we handed back from
         * DxgkDdiOpenAllocation, i.e. our own WINMALI_KMD_ALLOCATION. Record its
         * identity so the worker can be pointed at the RIGHT surface instead of
         * guessing at it.
         *
         * ⚠ DIRQL-safe: interlocked stores only, no registry, no dereference
         * beyond the magic check. Recording ONLY for now - the worker is not
         * rewired until these numbers prove the handle resolves. */
        {
            PWINMALI_KMD_ALLOCATION fa = (PWINMALI_KMD_ALLOCATION)pArgs->hAllocation;
            if (fa != NULL && fa->Magic == WINMALI_KMD_ALLOC_MAGIC) {
                (VOID)InterlockedExchange(&g_WinMaliFlipAllocW,      (LONG)fa->Width);
                (VOID)InterlockedExchange(&g_WinMaliFlipAllocH,      (LONG)fa->Height);
                (VOID)InterlockedExchange(&g_WinMaliFlipAllocPitch,  (LONG)fa->Pitch);
                (VOID)InterlockedExchange(&g_WinMaliFlipAllocFmt,    (LONG)fa->Format);
                (VOID)InterlockedExchange(&g_WinMaliFlipAllocSize,   (LONG)(ULONG)fa->Size);
                (VOID)InterlockedExchange(&g_WinMaliFlipAllocId,     (LONG)fa->AllocId);
                /* ★ THE WHOLE QUESTION IN ONE BIT: do we have its PAGES?
                 *
                 * We only DESCRIBE this allocation at CreateAllocation - VidMm
                 * owns the memory, and the only way we ever see an MDL for it is
                 * BuildPagingBuffer MAP_APERTURE_SEGMENT. Under escape submit
                 * dxgk is not expected to aperture-map at all, which would leave
                 * ApertureMdl NULL forever and make the true scanout surface
                 * unreadable no matter how well we identify it.
                 *
                 * If this reads non-NULL, the present worker can be pointed
                 * straight at the composed frame and the taskbar/cursor problem
                 * is over. If it reads NULL, identifying the surface was never
                 * going to be enough and the next move is getting its pages
                 * (UMD-side lock + 0x93, or a real aperture mapping). */
                /* ⚠ SAMPLED AT THE WRONG MOMENT. Flips happen at mode-set (2 of
                   them), residency arrives later - so reading ApertureMdl HERE
                   reports NULL even though dxgk went on to map this very
                   allocation (ApMapMaxSize == 21,667,840 == its size). Keep the
                   pointer instead and let the present worker, which runs
                   continuously, look at the MDL when it actually matters. */
                (VOID)InterlockedExchangePointer((PVOID*)&g_WinMaliFlipAlloc, fa);
                (VOID)InterlockedExchange(&g_WinMaliFlipHasMdl,
                                          (LONG)(fa->ApertureMdl != NULL ? 1 : 0));
                (VOID)InterlockedExchange(&g_WinMaliFlipMdlPages,
                                          (LONG)fa->AperturePageCount);
                (VOID)InterlockedIncrement(&g_WinMaliFlipAllocOk);
            } else {
                /* NULL or wrong magic: either dxgk flips an allocation we never
                   opened, or OpenAllocation handed back NULL for it (it does, on
                   the path that knowingly returns success with a NULL cookie). */
                (VOID)InterlockedIncrement(&g_WinMaliFlipAllocBad);
            }
        }
    }
    return STATUS_SUCCESS;
}

/* ⛔ D3DKMDT_VIDPN_HW_CAPABILITY has no `Value` alias -- zero the struct. */
NTSTATUS APIENTRY
WinMaliKmdQueryVidPnHWCapability(_In_ CONST HANDLE hAdapter,
                                 _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pArgs)
{
    WinMaliVpLogOnly_(WINMALI_VPC_HWCAPS);
    WinMaliDodDdi(WINMALI_DOD_DDI_HWCAPABILITY);
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pArgs->SourceId != WINMALI_SOURCE_ID) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    if (pArgs->TargetId != WINMALI_TARGET_ID) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }
    RtlZeroMemory(&pArgs->VidPnHWCaps, sizeof(pArgs->VidPnHWCaps));
    return STATUS_SUCCESS;
}

/* THIS DDI WAS THE LAST COMPLETELY BLIND SURFACE IN THE DRIVER.
 *
 * MEASURED: the OS converges on a committable VidPn at t=0.157 s and never again
 * after the monitor devnode DISPLAY\LNV5655 is created at t=1.3 s. The one input
 * that exists at 1.3 s and does not at 0.157 s is the MONITOR OBJECT, and the
 * only driver-facing surface on that new input is this function - which was
 * UNREFERENCED_PARAMETER x2 + return STATUS_SUCCESS with NO log marker at all.
 * "It adds nothing" was never measured; we did not even know if dxgk called it.
 *
 * The old comment here was also stale: it claimed "there is no EDID", which the
 * synthetic EDID further down replaced long ago.
 *
 * READ FIRST, mutate second, so the census stays honest even with the add armed.
 *
 * STATUS_GRAPHICS_NO_PREFERRED_MODE (0x401E031E) IS NT_SUCCESS-TRUE. Testing it
 *   with NT_SUCCESS() alone reports "there IS a preferred mode" when there is
 *   not - the same trap already documented in WinMaliVidPn.h.
 * pfnAddMode RELEASES the mode info ON SUCCESS. Release it only on failure. */
NTSTATUS APIENTRY
WinMaliKmdRecommendMonitorModes(_In_ CONST HANDLE hAdapter,
                                _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pArgs)
{
    CONST DXGK_MONITORSOURCEMODESET_INTERFACE *pI;
    CONST D3DKMDT_MONITOR_SOURCE_MODE *pMode = NULL;
    SIZE_T   num = 0;
    NTSTATUS st;
    ULONG    walked = 0;

    WinMaliDodDdi(WINMALI_DOD_DDI_MONITORMODES);
    UNREFERENCED_PARAMETER(hAdapter);

    if (pArgs == NULL || pArgs->pMonitorSourceModeSetInterface == NULL) {
        WinMaliVpLogOnly_(WINMALI_VPC_RECMON | 0xFFFFFFu);
        return STATUS_SUCCESS;
    }
    pI = pArgs->pMonitorSourceModeSetInterface;

    /* 1. HOW MANY MODES does the set hold AT THE MOMENT OF THE FAILING
          DERIVATION? WMI can only ever show this long afterwards. */
    if (pI->pfnGetNumModes != NULL &&
        NT_SUCCESS(pI->pfnGetNumModes(pArgs->hMonitorSourceModeSet, &num))) {
        WinMaliVpLogOnly_(WINMALI_VPC_RECMON | ((ULONG)num & 0xFFFFu) |
                          (((ULONG)pArgs->VideoPresentTargetId & 0xFFu) << 16));
    } else {
        WinMaliVpLogOnly_(WINMALI_VPC_RECMON | 0xFFFEu);
    }

    /* 2. IS THERE A PREFERRED MODE? A set with no preferred mode is exactly what
          a selection that cannot converge looks like from in here. */
    if (pI->pfnAcquirePreferredModeInfo != NULL) {
        st = pI->pfnAcquirePreferredModeInfo(pArgs->hMonitorSourceModeSet, &pMode);
        WinMaliVpLogOnly_(WINMALI_VPC_RECMON_INFO |
            ((st == STATUS_GRAPHICS_NO_PREFERRED_MODE) ? 0u
                                                       : (NT_SUCCESS(st) ? 1u : 2u)));
        if (st == STATUS_SUCCESS && pMode != NULL) {
            WinMaliVpLogOnly_(WINMALI_VPC_RECMON_MODE |
                (((ULONG)pMode->VideoSignalInfo.ActiveSize.cx & 0xFFFu) << 12) |
                 ((ULONG)pMode->VideoSignalInfo.ActiveSize.cy & 0xFFFu));
            if (pI->pfnReleaseModeInfo != NULL) {
                (VOID)pI->pfnReleaseModeInfo(pArgs->hMonitorSourceModeSet, pMode);
            }
        }
        pMode = NULL;
    }

    /* 3. WALK the set. Origin discriminates OUR EDID (MONITORDESCRIPTOR=2) from
          the no-EDID default profile (DEFAULTMONITORPROFILE=1) - and a default
          profile here would mean the intersection with our 2944x1840 is empty. */
    if (pI->pfnAcquireFirstModeInfo != NULL &&
        NT_SUCCESS(pI->pfnAcquireFirstModeInfo(pArgs->hMonitorSourceModeSet, &pMode))) {
        while (pMode != NULL && walked < 16u) {
            CONST D3DKMDT_MONITOR_SOURCE_MODE *pNext = NULL;
            WinMaliVpLogOnly_(WINMALI_VPC_RECMON_MODE | 0x800000u |
                (((ULONG)pMode->VideoSignalInfo.ActiveSize.cx & 0xFFFu) << 12) |
                 ((ULONG)pMode->VideoSignalInfo.ActiveSize.cy & 0xFFFu));
            WinMaliVpLogOnly_(WINMALI_VPC_RECMON_INFO | 0x800000u |
                (((ULONG)pMode->Origin & 0xFFu) << 8) |
                 ((ULONG)pMode->Preference & 0xFFu));
            walked++;
            if (pI->pfnAcquireNextModeInfo == NULL) {
                if (pI->pfnReleaseModeInfo != NULL) {
                    (VOID)pI->pfnReleaseModeInfo(pArgs->hMonitorSourceModeSet, pMode);
                }
                break;
            }
            st = pI->pfnAcquireNextModeInfo(pArgs->hMonitorSourceModeSet, pMode, &pNext);
            if (pI->pfnReleaseModeInfo != NULL) {
                (VOID)pI->pfnReleaseModeInfo(pArgs->hMonitorSourceModeSet, pMode);
            }
            pMode = NT_SUCCESS(st) ? pNext : NULL;
        }
    }

    /* 4. THE FIX, knob-gated and OFF by default. WMI already shows the set
          holding exactly our 2944x1840 mode, so the census in steps 1-3 has to
          come back before spending a boot on the add. */
    if (g_WinMaliMonModeAdd != 0 && pI->pfnCreateNewModeInfo != NULL &&
        pI->pfnAddMode != NULL) {
        D3DKMDT_MONITOR_SOURCE_MODE *pNew = NULL;
        st = pI->pfnCreateNewModeInfo(pArgs->hMonitorSourceModeSet, &pNew);
        if (NT_SUCCESS(st) && pNew != NULL) {
            /* Do NOT RtlZeroMemory: Id is OS-assigned and is the FIRST member.
               Same rule as WinMaliFillSourceMode. */
            WinMaliFillSignalInfo(&pNew->VideoSignalInfo);
            pNew->ColorBasis = WINMALI_COLOR_BASIS;
            pNew->ColorCoeffDynamicRanges.FirstChannel  = 8;
            pNew->ColorCoeffDynamicRanges.SecondChannel = 8;
            pNew->ColorCoeffDynamicRanges.ThirdChannel  = 8;
            pNew->ColorCoeffDynamicRanges.FourthChannel = 0;
            pNew->Origin     = D3DKMDT_MCO_DRIVER;
            pNew->Preference = D3DKMDT_MP_PREFERRED;
            st = pI->pfnAddMode(pArgs->hMonitorSourceModeSet, pNew);
            /* STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET here is BENIGN and is
               itself the answer: it proves the EDID twin was already present. */
            WinMaliVpLogOnly_(WINMALI_VPC_RECMON_INFO | 0x400000u |
                              ((ULONG)st & 0xFFFFu));
            if (!NT_SUCCESS(st) && pI->pfnReleaseModeInfo != NULL) {
                (VOID)pI->pfnReleaseModeInfo(pArgs->hMonitorSourceModeSet, pNew);
            }
        }
    }
    return STATUS_SUCCESS;
}

/* ======================================================================
 * WinMaliOvlFlipDual_ - POINT BOTH HALVES OF THE PANEL AT THE BACK FRAME.
 *
 * Extracted from the mode-2 block in PresentDisplayOnly so the PRESENT WORKER can
 * call it too: that block is inside the KMDOD DDI, so RENDER mode - which has a live
 * composited desktop as of 2026-08-25 - could never reach it and fell back to a CPU
 * blit at ~15 fps.
 *
 * Self-contained: reads the live OVL0 address itself rather than taking it from the
 * caller, so it is safe from either path.
 *
 * â›” WHITELIST BOTH PIPES. This writes a physical address into a display controller.
 * Only the three known EDK2 carveout frames are ever programmed; anything else means
 * our idea of the scanout has drifted from the hardware's and we must refuse, not
 * guess. A wrong address here shows garbage on the glass or hangs the display bus.
 * â›” The caller must have composed a COMPLETE frame into g_WinMaliOvlBackVa first.
 * ====================================================================== */
BOOLEAN
WinMaliOvlFlipDual_(VOID)
{
    volatile ULONG *l0, *r0;
    ULONG cur, curR, back;
    BOOLEAN okL, okR;

    /* â›” EVERY NON-FLIP PATH MUST BE COUNTED. 1.1.17.0 had two silent early returns
       here, so "8 flips then nothing, and Rej=0" was undiagnosable - the one thing
       this driver's own comments warn about repeatedly. */
    if (g_WinMaliOvlRegsVa == NULL || g_WinMaliOvl4RegsVa == NULL) {
        (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlip2Rej);
        return FALSE;
    }
    back = (ULONG)InterlockedCompareExchange64(&g_WinMaliOvlBackPa, 0, 0);
    if (back != (ULONG)WINMALI_FB_FRAME1_PA &&
        back != (ULONG)WINMALI_FB_FRAME2_PA) {
        /* Includes the back==0 case. Whitelisted on the way OUT as well as on the
           way in: we never program an address we cannot name. */
        (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlip2Rej);
        return FALSE;
    }

    l0 = (volatile ULONG *)((PUCHAR)g_WinMaliOvlRegsVa  + WINMALI_OVL0_L0_ADDR);
    r0 = (volatile ULONG *)((PUCHAR)g_WinMaliOvl4RegsVa + WINMALI_OVL0_L0_ADDR);
    cur  = READ_REGISTER_ULONG(l0);
    curR = READ_REGISTER_ULONG(r0);

    okL = (cur == (ULONG)WINMALI_FB_FRAME0_PA) ||
          (cur == (ULONG)WINMALI_FB_FRAME1_PA) ||
          (cur == (ULONG)WINMALI_FB_FRAME2_PA);
    okR = (curR == (ULONG)(WINMALI_FB_FRAME0_PA + WINMALI_OVL_RIGHT_OFF)) ||
          (curR == (ULONG)(WINMALI_FB_FRAME1_PA + WINMALI_OVL_RIGHT_OFF)) ||
          (curR == (ULONG)(WINMALI_FB_FRAME2_PA + WINMALI_OVL_RIGHT_OFF));

    if (!okL || !okR || back == cur) {
        (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlip2Rej);
        return FALSE;
    }

    WRITE_REGISTER_ULONG((volatile ULONG *)((PUCHAR)g_WinMaliOvlRegsVa +
                         WINMALI_OVL0_L0_ADDR_MSB), 0);
    WRITE_REGISTER_ULONG(l0, back);
    WRITE_REGISTER_ULONG((volatile ULONG *)((PUCHAR)g_WinMaliOvl4RegsVa +
                         WINMALI_OVL0_L0_ADDR_MSB), 0);
    WRITE_REGISTER_ULONG(r0, back + WINMALI_OVL_RIGHT_OFF);

    (VOID)InterlockedExchange(&g_WinMaliStatOvlFlipBack,
                              (LONG)READ_REGISTER_ULONG(l0));
    (VOID)InterlockedExchange(&g_WinMaliStatOvlFlipRight,
                              (LONG)READ_REGISTER_ULONG(r0));
    (VOID)InterlockedExchange64(&g_WinMaliOvlBackPa, (LONG64)g_WinMaliOvlFrontPa);
    (VOID)InterlockedExchange64(&g_WinMaliOvlFrontPa, (LONG64)back);
    (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlip2N);
    return TRUE;
}

/* ======================================================================
 * DxgkDdiPresentDisplayOnly - THE DISPLAY-ONLY PRESENT PATH.
 *
 * dxgk gives us the finished desktop in SYSTEM MEMORY:
 *   pSource        - CPU pointer to the composed image  (PASSIVE_LEVEL)
 *   Pitch          - source stride in bytes
 *   BytesPerPixel  - 4 for the 32bpp formats we publish
 *   pDirtyRect[]   - what actually changed; empty means "all of it"
 *   pMoves[]       - screen-to-screen scrolls we may treat as dirty
 *
 * We copy into the firmware framebuffer at WINMALI_FB_STRIDE. That is the ONLY
 * route pixels take to this panel, and it is the same copy the present worker
 * has been doing successfully all along - just fed by the OS instead of guessed
 * at from a candidate table.
 *
 * ⛔ CLAMP EVERY RECT. dxgk's rects are in source coordinates and are normally
 * inside the mode, but a single unclamped rect writes past the framebuffer and
 * corrupts whatever follows it in physical memory. Both axes, both edges.
 * ⛔ SEH around the source read: pSource is dxgk's mapping, not ours.
 * ====================================================================== */
/* Latched exactly once: the first present pays for the full frame, every
   later present takes the cheap dirty-rect path. */
static LONG s_WinMaliDodFullDone = 0;

NTSTATUS APIENTRY
WinMaliKmdPresentDisplayOnly(_In_ CONST HANDLE hAdapter,
                             _In_ CONST DXGKARG_PRESENT_DISPLAYONLY *pArgs)
{
    PWINMALI_ADAPTER a = WinMaliAdapterFromDxgkHandle(hAdapter);
    CONST UCHAR *src;
    PUCHAR       dst;
    LONG         srcPitch;
    ULONG        bpp, i, rows, copyBytes;
    BOOLEAN      forceFull = FALSE;
    LARGE_INTEGER t0, t1, freq;
    ULONG        fullUs = 0;

    WinMaliDodDdi(WINMALI_DOD_DDI_PRESENTDISPONLY);

    /* ⛔ DUMP THE ARGUMENTS BEFORE ANY TEST. The ring is published at teardown,
       so these lines survive; without them "declined" does not say WHAT was
       wrong with the frame. Bounded: the first 8 calls only. */
    {
        static LONG s_dump = 0;
        if (InterlockedIncrement(&s_dump) <= 8) {
            DbgPrint("[WinMali]    PresentDisplayOnly: a=%p pArgs=%p src=%p "
                     "pitch=%ld bpp=%lu moves=%lu dirty=%lu\n",
                     a, pArgs,
                     (pArgs != NULL) ? pArgs->pSource : NULL,
                     (pArgs != NULL) ? (long)pArgs->Pitch : -1,
                     (pArgs != NULL) ? (unsigned long)pArgs->BytesPerPixel : 0,
                     (pArgs != NULL) ? (unsigned long)pArgs->NumMoves : 0,
                     (pArgs != NULL) ? (unsigned long)pArgs->NumDirtyRects : 0);
            if (pArgs != NULL && pArgs->NumDirtyRects != 0 && pArgs->pDirtyRect != NULL) {
                DbgPrint("[WinMali]      dirty[0] = (%ld,%ld)..(%ld,%ld)\n",
                         (long)pArgs->pDirtyRect[0].left,  (long)pArgs->pDirtyRect[0].top,
                         (long)pArgs->pDirtyRect[0].right, (long)pArgs->pDirtyRect[0].bottom);
            }
        }
    }

    /* ⛔⛔ NEVER FAIL THE ADAPTER OVER A SINGLE FRAME.
     * These used to return STATUS_INVALID_PARAMETER / a silent STATUS_SUCCESS.
     * dxgk may tear an adapter down over a failed present - and the device does
     * end at CM_PROB_FAILED_POST_START with RemoveDevice immediately after the
     * one present we get. A display-only driver that cannot paint a frame
     * should DROP the frame and stay alive. Every drop is now named. */
    if (a == NULL)              { WinMaliDodPresentWhy(WINMALI_DODPRES_NOADAPTER); return STATUS_SUCCESS; }
    if (pArgs == NULL)          { WinMaliDodPresentWhy(WINMALI_DODPRES_NOARGS);    return STATUS_SUCCESS; }
    if (pArgs->pSource == NULL) { WinMaliDodPresentWhy(WINMALI_DODPRES_NOSOURCE);  return STATUS_SUCCESS; }
    if (a->ScanoutVa == NULL)   { WinMaliDodPresentWhy(WINMALI_DODPRES_NOFB);
                                  (VOID)InterlockedIncrement(&g_WinMaliStatDodNoFb);
                                  return STATUS_SUCCESS; }

    src      = (CONST UCHAR *)pArgs->pSource;
    dst      = (PUCHAR)a->ScanoutVa;
    srcPitch = pArgs->Pitch;
    bpp      = pArgs->BytesPerPixel;

    /* We publish only 32bpp modes; anything else would need a converter we do
       not have, and silently copying it would paint garbage. */
    if (bpp != 4 || srcPitch <= 0) {
        (VOID)InterlockedIncrement(&g_WinMaliStatDodBadFmt);
        return STATUS_SUCCESS;
    }
    if ((SIZE_T)WINMALI_FB_HEIGHT * WINMALI_FB_STRIDE > a->ScanoutBytes) {
        (VOID)InterlockedIncrement(&g_WinMaliStatDodNoFb);
        return STATUS_SUCCESS;
    }

    copyBytes = WINMALI_FB_WIDTH * 4u;
    if ((ULONG)srcPitch < copyBytes) { copyBytes = (ULONG)srcPitch; }

    /* â›” ONE FORCED FULL FRAME. Windows reports only what CHANGED and assumes
       the rest of our scanout already matches its surface. It does not - it
       holds the previous owner's pixels. Latched, so exactly one frame pays. */
    if (g_WinMaliDodFirstFull != 0 &&
        InterlockedCompareExchange(&s_WinMaliDodFullDone, 1, 0) == 0) {
        forceFull = TRUE;
    }
    /* ⛔ MODE 2: COMPOSE INTO THE BACK FRAME, AND ALWAYS IN FULL.
       dst was set to the LIVE frame above; redirect it to whichever of our two
       frames is NOT currently being scanned out. The full copy is mandatory -
       see the apply143 header: the back frame is one frame stale, so a
       dirty-rect-only copy would leave the rest of it showing the frame before
       last and the display would decay into a patchwork. */
    if (g_WinMaliOvlFlip == 2 && g_WinMaliOvlBackVa != NULL &&
        g_WinMaliOvlRegsVa != NULL && g_WinMaliOvl4RegsVa != NULL) {
        dst = (g_WinMaliOvlFrontPa == (LONG64)WINMALI_FB_FRAME1_PA)
                  ? (PUCHAR)g_WinMaliOvlBackVa      /* front=F1 -> compose F2 */
                  : (PUCHAR)a->ScanoutVa;           /* front=F2 -> compose F1 */
        forceFull = TRUE;
    }

    __try {
        if (forceFull || (pArgs->NumDirtyRects == 0 && pArgs->NumMoves == 0)) {
            /* Full frame - the first present after a mode set, and the safe
               fallback whenever dxgk does not narrow it down for us. */
            /* â›” TIME IT. StatScanoutWc=0 means this mapping is the UNCACHED
               fallback, and 21 MB of uncached writes may be pathologically
               slow. Publishing the real number ends that guess either way. */
            t0 = KeQueryPerformanceCounter(&freq);
            for (rows = 0; rows < WINMALI_FB_HEIGHT; rows++) {
                RtlCopyMemory(dst + (SIZE_T)rows * WINMALI_FB_STRIDE,
                              src + (SIZE_T)rows * srcPitch,
                              copyBytes);
            }
            t1 = KeQueryPerformanceCounter(NULL);
            if (freq.QuadPart != 0) {
                fullUs = (ULONG)(((t1.QuadPart - t0.QuadPart) * 1000000LL) / freq.QuadPart);
            }
            (VOID)InterlockedIncrement(&g_WinMaliStatDodFull);
        } else {
            /* Dirty rects only. A move is just a destination that changed, so
               treating both as dirty is correct if not optimal. */
            for (i = 0; i < pArgs->NumMoves; i++) {
                RECT r = pArgs->pMoves[i].DestRect;
                LONG y, l = r.left, t = r.top, rr = r.right, b = r.bottom;
                if (l < 0) { l = 0; }  if (t < 0) { t = 0; }
                if (rr > (LONG)WINMALI_FB_WIDTH)  { rr = (LONG)WINMALI_FB_WIDTH; }
                if (b  > (LONG)WINMALI_FB_HEIGHT) { b  = (LONG)WINMALI_FB_HEIGHT; }
                if (rr <= l || b <= t) { continue; }
                for (y = t; y < b; y++) {
                    RtlCopyMemory(dst + (SIZE_T)y * WINMALI_FB_STRIDE + (SIZE_T)l * 4u,
                                  src + (SIZE_T)y * srcPitch          + (SIZE_T)l * 4u,
                                  (SIZE_T)(rr - l) * 4u);
                }
            }
            for (i = 0; i < pArgs->NumDirtyRects; i++) {
                RECT r = pArgs->pDirtyRect[i];
                LONG y, l = r.left, t = r.top, rr = r.right, b = r.bottom;
                if (l < 0) { l = 0; }  if (t < 0) { t = 0; }
                if (rr > (LONG)WINMALI_FB_WIDTH)  { rr = (LONG)WINMALI_FB_WIDTH; }
                if (b  > (LONG)WINMALI_FB_HEIGHT) { b  = (LONG)WINMALI_FB_HEIGHT; }
                if (rr <= l || b <= t) { continue; }
                for (y = t; y < b; y++) {
                    RtlCopyMemory(dst + (SIZE_T)y * WINMALI_FB_STRIDE + (SIZE_T)l * 4u,
                                  src + (SIZE_T)y * srcPitch          + (SIZE_T)l * 4u,
                                  (SIZE_T)(rr - l) * 4u);
                }
            }
            (VOID)InterlockedIncrement(&g_WinMaliStatDodDirty);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* pSource is dxgk's mapping. If this EVER fires the contract is not what
           we think it is - grade it as a stop, not a nit. */
        (VOID)InterlockedIncrement(&g_WinMaliStatDodExc);
        WinMaliDodPresentWhy(6u);   /* 6 = SEH fired inside the copy */
        return STATUS_SUCCESS;
    }

    (VOID)InterlockedIncrement(&g_WinMaliStatDodPresent);

    /* ---- REAL FLIP ---------------------------------------------------------
     * PASSIVE_LEVEL here, mapping already established at StartDevice.
     * ⛔ We only ever write one of our three known carveout frames, and only if
     * the overlay CURRENTLY holds one of them. If anything else owns the panel
     * we refuse and count it rather than fight for it. */
    if (g_WinMaliOvlFlip != 0 && g_WinMaliOvlRegsVa != NULL) {
        volatile ULONG *l0 = (volatile ULONG *)((PUCHAR)g_WinMaliOvlRegsVa +
                                                WINMALI_OVL0_L0_ADDR);
        ULONG cur = READ_REGISTER_ULONG(l0);
        BOOLEAN known = (cur == (ULONG)WINMALI_FB_FRAME0_PA) ||
                        (cur == (ULONG)WINMALI_FB_FRAME1_PA) ||
                        (cur == (ULONG)WINMALI_FB_FRAME2_PA);
        if (!known) {
            (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlipRej);
        } else if (g_WinMaliOvlFlip == 1) {
            /* SAFE PROBE: write back exactly what is already there. Nothing on
               the glass may change; the point is to prove the write lands. */
            /* ⛔★★ THE FIRST WRITE TO THIS BLOCK IS THE ONE UNKNOWN OPERATION IN
             * THE WHOLE CHANGE. The read probe stamps a flushed breadcrumb before
             * every access because a gated-block access hangs the bus leaving no
             * record; the first WRITE had none, so a wedge here would have been
             * unattributable. PASSIVE_LEVEL here, and it flushes exactly once. */
            {
                static LONG s_firstWrite = 0;
                if (InterlockedCompareExchange(&s_firstWrite, 1, 0) == 0) {
                    (VOID)WinMaliWriteParamsDwordFlushed(&g_WinMaliRegPath,
                                                         L"StatOvlStep", 0x4000u);
                }
            }
            WRITE_REGISTER_ULONG(l0, cur);
            (VOID)InterlockedExchange(&g_WinMaliStatOvlFlipBack,
                                      (LONG)READ_REGISTER_ULONG(l0));
            (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlipN);
        } else {
            /* MODE 2: (a) and (b) from the old note are now wired - frame2 is
             * mapped and the copy above went into the back frame IN FULL - so
             * this branch is live. Point BOTH halves of the dual-pipe panel at
             * the frame we just composed, then swap the roles.
             * ⛔ BOTH, or the right half stays on the previous buffer. */
            ULONG back = (ULONG)g_WinMaliOvlBackPa;
            volatile ULONG *r0 = (volatile ULONG *)((PUCHAR)g_WinMaliOvl4RegsVa +
                                                    WINMALI_OVL0_L0_ADDR);
            ULONG curR = (g_WinMaliOvl4RegsVa != NULL) ? READ_REGISTER_ULONG(r0) : 0;
            BOOLEAN okR = (curR == (ULONG)(WINMALI_FB_FRAME0_PA + WINMALI_OVL_RIGHT_OFF)) ||
                          (curR == (ULONG)(WINMALI_FB_FRAME1_PA + WINMALI_OVL_RIGHT_OFF)) ||
                          (curR == (ULONG)(WINMALI_FB_FRAME2_PA + WINMALI_OVL_RIGHT_OFF));
            if (back == 0 || back == cur || g_WinMaliOvl4RegsVa == NULL || !okR) {
                (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlip2Rej);
            } else {
                WRITE_REGISTER_ULONG((volatile ULONG *)((PUCHAR)g_WinMaliOvlRegsVa +
                                     WINMALI_OVL0_L0_ADDR_MSB), 0);
                WRITE_REGISTER_ULONG(l0, back);
                WRITE_REGISTER_ULONG((volatile ULONG *)((PUCHAR)g_WinMaliOvl4RegsVa +
                                     WINMALI_OVL0_L0_ADDR_MSB), 0);
                WRITE_REGISTER_ULONG(r0, back + WINMALI_OVL_RIGHT_OFF);
                (VOID)InterlockedExchange(&g_WinMaliStatOvlFlipBack,
                                          (LONG)READ_REGISTER_ULONG(l0));
                (VOID)InterlockedExchange(&g_WinMaliStatOvlFlipRight,
                                          (LONG)READ_REGISTER_ULONG(r0));
                /* Swap: what we just showed becomes the front, the old front is
                   the next back. */
                (VOID)InterlockedExchange64(&g_WinMaliOvlBackPa,
                                            (LONG64)g_WinMaliOvlFrontPa);
                (VOID)InterlockedExchange64(&g_WinMaliOvlFrontPa, (LONG64)back);
                (VOID)InterlockedIncrement(&g_WinMaliStatOvlFlip2N);
            }
        }
    }

    /* ⛔ THE PROOF. Counting calls cannot tell us whether PIXELS MOVED - read
       the same pixel out of BOTH ends of the copy and publish both. Equal and
       non-zero means the composed desktop is physically in the framebuffer the
       firmware scans out. Bounded to the 1st and every 128th present. */
    {
        static LONG s_proofN = 0;
        LONG n = InterlockedIncrement(&s_proofN);

        if (n <= 8 || (n % 128) == 0) {
            SIZE_T midRow = (SIZE_T)(WINMALI_FB_HEIGHT / 2u);
            SIZE_T midCol = (SIZE_T)(WINMALI_FB_WIDTH  / 2u) * 4u;
            ULONG  srcPix = 0xFFFFFFFFu;
            ULONG  dstPix = 0xFFFFFFFFu;
            ULONG  gridN = 0, srcNZ = 0, srcOr = 0, dstNZ = 0, dstOr = 0;
            ULONG  gy, gx;

            __try {
                srcPix = *(CONST volatile ULONG *)(src + midRow * (SIZE_T)srcPitch + midCol);
                dstPix = *(CONST volatile ULONG *)(dst + midRow * WINMALI_FB_STRIDE + midCol);

                /* ⛔ ONE PIXEL CANNOT TELL "black frame" FROM "blank surface".
                   Walk a coprime-ish grid over the WHOLE frame so no periodic
                   pattern can alias it. Alpha is masked off: a desktop surface
                   is commonly 0xFF000000 where nothing is drawn, and counting
                   that as "painted" would be the same mistake in reverse. */
                for (gy = 0; gy < WINMALI_FB_HEIGHT; gy += 97u) {
                    for (gx = 0; gx < WINMALI_FB_WIDTH; gx += 89u) {
                        ULONG sp = *(CONST volatile ULONG *)
                                       (src + (SIZE_T)gy * (SIZE_T)srcPitch + (SIZE_T)gx * 4u);
                        ULONG dp = *(CONST volatile ULONG *)
                                       (dst + (SIZE_T)gy * WINMALI_FB_STRIDE + (SIZE_T)gx * 4u);
                        gridN++;
                        srcOr |= sp;  dstOr |= dp;
                        if ((sp & 0x00FFFFFFu) != 0) { srcNZ++; }
                        if ((dp & 0x00FFFFFFu) != 0) { dstNZ++; }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                srcPix = 0xDEAD0001u;   /* the sample faulted, not the copy */
            }
            WinMaliDodPresentProof((ULONG)n, srcPix, dstPix);
            if (gridN != 0) {
                WinMaliDodPresentContent(gridN, srcNZ, srcOr, dstNZ, dstOr);
            }

            /* â˜… SAMPLE WHERE THE CONTENT IS. The whole-frame grid cannot see a
               76x79 box; the dirty rect IS the painted region. */
            if (pArgs->NumDirtyRects != 0 && pArgs->pDirtyRect != NULL) {
                RECT  r = pArgs->pDirtyRect[0];
                ULONG rN = 0, rSrc = 0, rDst = 0;
                LONG  ry, rx;

                __try {
                    for (ry = r.top; ry < r.bottom && ry < (LONG)WINMALI_FB_HEIGHT; ry += 4) {
                        for (rx = r.left; rx < r.right && rx < (LONG)WINMALI_FB_WIDTH; rx += 4) {
                            ULONG sp = *(CONST volatile ULONG *)
                                (src + (SIZE_T)ry * (SIZE_T)srcPitch + (SIZE_T)rx * 4u);
                            ULONG dp = *(CONST volatile ULONG *)
                                (dst + (SIZE_T)ry * WINMALI_FB_STRIDE + (SIZE_T)rx * 4u);
                            rN++;
                            if ((sp & 0x00FFFFFFu) != 0) { rSrc++; }
                            if ((dp & 0x00FFFFFFu) != 0) { rDst++; }
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { rN = 0; }
                if (rN != 0) { WinMaliDodPresentRect(rN, rSrc, rDst, fullUs); }
            } else if (fullUs != 0) {
                WinMaliDodPresentRect(0, 0, 0, fullUs);
            }
        }
    }

    /* THE CONTROL GROUP. Without this the crop existed only on the render path, so
     * "the clock region is empty" had nothing to be empty COMPARED TO - and the
     * blob left in the registry by the previous render boot read exactly like a
     * display-only sample. Same helper, same region, tagged Which=2. */
    WinMaliPublishCrop_((PUCHAR)src, WINMALI_FB_HEIGHT, (ULONG)srcPitch,
                        copyBytes, 2u);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Synthetic EDID                                                          */
/* ---------------------------------------------------------------------- */
/*
 * WHY: the ETW trace of a failing start contains exactly ONE graphics failure
 * status among 23,375 dxgk events --
 *   0xC01E0401 STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED
 *   "Specified display adapter child device does not support descriptor exposure."
 * We were returning STATUS_MONITOR_NO_DESCRIPTOR from QueryDeviceDescriptor
 * because the panel has no EDID ROM, and dxgk treats that as "this child cannot
 * expose a descriptor" and abandons the adapter -- which is why it never reached
 * QueryChildStatus or any VidPN DDI.
 *
 * The panel is a fixed internal one whose geometry we know from our own firmware,
 * so we synthesise a minimal EDID 1.4 describing exactly the mode the VidPN DDIs
 * publish.
 *
 * ⚠ THE BLANKING IS NOT COSMETIC. This used to say "blanking is zero because
 * TotalSize == ActiveSize" - which was true and was the bug: zero blanking
 * forces a pixel clock (2944*1840*60 = 325,017,600 Hz) that an EDID DTD cannot
 * encode, because that field is in 10 kHz units. The DTD then carried a
 * different number from the signal info and no mode could be cofunctional.
 * The mode now has real blanking chosen so the clock is an exact multiple of
 * 10 kHz, and BOTH descriptions are generated from the same constants.
 *
 * Checksum is computed at init, never hand-written: byte 127 must make the whole
 * block sum to 0 mod 256, and a hand-summed constant is a silent trap.
 */
#define WINMALI_EDID_SIZE 128

static UCHAR g_WinMaliEdid[WINMALI_EDID_SIZE] = {
    /* 00-07 header */              0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,
    /* 08-09 mfr "LNV" */           0x31,0xD6,
    /* 10-11 product 0x5655 (LE) */ 0x55,0x56,
    /* 12-15 serial */              0x00,0x00,0x00,0x00,
    /* 16 week, 17 year(2026) */    0x01,0x24,
    /* 18-19 EDID 1.4 */            0x01,0x04,
    /* 20 digital, 8bpc */          0xA0,
    /* 21-22 size cm (27x17) */     0x1B,0x11,
    /* 23 gamma 2.2 */              0x78,
    /* 24 features: preferred tim */0x02,
    /* 25-34 chromaticity (sRGB) */ 0xEE,0x91,0xA3,0x54,0x4C,0x99,0x26,0x0F,0x50,0x54,
    /* 35-37 established timings */ 0x00,0x00,0x00,
    /* 38-53 standard timings: unused */
                                    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
                                    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    /* 54-71 DTD1: FILLED IN AT INIT from the WINMALI_* timing constants.
       ⛔ Hand-encoded bytes are exactly how this drifted from the signal info
       by 2,400 Hz and killed every topology. See WinMaliEdidBuildDtd_(). */
    0,0, 0, 0, 0, 0, 0, 0, 0,0,0,0,
    0x0E,0xAA,0x10,                 /* image size 270x170 mm                 */
    0x00,0x00,                      /* borders                               */
    0x1E,                           /* digital separate, +hsync +vsync       */
    /* 72-89 DTD2 -> monitor name "Mali G615" */
    0x00,0x00,0x00,0xFC,0x00,
    'M','a','l','i',' ','G','6','1','5','\n',' ',' ',' ',
    /* 90-107 DTD3 -> unused */
    0x00,0x00,0x00,0x10,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 108-125 DTD4 -> unused */
    0x00,0x00,0x00,0x10,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 126 extensions, 127 checksum (filled at init) */
    0x00,0x00
};
static BOOLEAN g_WinMaliEdidReady = FALSE;

C_ASSERT(sizeof(g_WinMaliEdid) == WINMALI_EDID_SIZE);

/*
 * Build DTD1 from the WINMALI_* timing constants.
 *
 * ⛔ THIS EXISTS BECAUSE HAND-ENCODED DTD BYTES DRIFTED FROM THE SIGNAL INFO.
 * The old block hard-coded pixclk 32502 (325,020,000 Hz) while
 * WinMaliFillSignalInfo published 325,017,600 Hz - a 2,400 Hz disagreement that
 * made the monitor's only source mode non-cofunctional with our target mode, so
 * the OS committed an empty topology and DWM had nothing to composite to.
 * Deriving both from one set of constants makes that class of bug impossible.
 */
static VOID
WinMaliEdidBuildDtd_(_Out_writes_(18) UCHAR *d)
{
    const ULONG pixclk = (ULONG)WINMALI_EDID_PIXCLK_10K;
    const ULONG hact   = WINMALI_FB_WIDTH,  hbl = WINMALI_H_BLANK;
    const ULONG vact   = WINMALI_FB_HEIGHT, vbl = WINMALI_V_BLANK;
    const ULONG hso    = WINMALI_H_SYNC_OFF, hsw = WINMALI_H_SYNC_WIDTH;
    const ULONG vso    = WINMALI_V_SYNC_OFF, vsw = WINMALI_V_SYNC_WIDTH;

    d[0]  = (UCHAR)(pixclk & 0xFF);          /* pixel clock, 10 kHz, LE */
    d[1]  = (UCHAR)((pixclk >> 8) & 0xFF);
    d[2]  = (UCHAR)(hact & 0xFF);
    d[3]  = (UCHAR)(hbl  & 0xFF);
    d[4]  = (UCHAR)(((hact >> 8) & 0xF) << 4 | ((hbl >> 8) & 0xF));
    d[5]  = (UCHAR)(vact & 0xFF);
    d[6]  = (UCHAR)(vbl  & 0xFF);
    d[7]  = (UCHAR)(((vact >> 8) & 0xF) << 4 | ((vbl >> 8) & 0xF));
    d[8]  = (UCHAR)(hso & 0xFF);
    d[9]  = (UCHAR)(hsw & 0xFF);
    d[10] = (UCHAR)(((vso & 0xF) << 4) | (vsw & 0xF));
    d[11] = (UCHAR)((((hso >> 8) & 0x3) << 6) | (((hsw >> 8) & 0x3) << 4) |
                    (((vso >> 4) & 0x3) << 2) |  ((vsw >> 4) & 0x3));
    /* d[12..17] (image size, borders, flags) stay as initialised. */
}

VOID
WinMaliEdidInit(VOID)
{
    ULONG i;
    UCHAR sum = 0;

    if (g_WinMaliEdidReady) {
        return;
    }

    WinMaliEdidBuildDtd_(&g_WinMaliEdid[54]);
    DbgPrint("[WinMali]    EDID DTD: %ux%u @%u Hz  htotal=%u vtotal=%u  "
             "pixclk=%llu Hz (%u x10kHz, EXACT)\n",
             WINMALI_FB_WIDTH, WINMALI_FB_HEIGHT, WINMALI_VSYNC_NUM,
             WINMALI_H_TOTAL, WINMALI_V_TOTAL,
             (ULONGLONG)WINMALI_PIXEL_RATE, (unsigned)WINMALI_EDID_PIXCLK_10K);

    g_WinMaliEdid[127] = 0;
    for (i = 0; i < WINMALI_EDID_SIZE; i++) {
        sum = (UCHAR)(sum + g_WinMaliEdid[i]);
    }
    g_WinMaliEdid[127] = (UCHAR)((256 - sum) & 0xFF);
    g_WinMaliEdidReady = TRUE;

    /* re-sum as a self-check: a valid EDID sums to 0 mod 256 */
    sum = 0;
    for (i = 0; i < WINMALI_EDID_SIZE; i++) {
        sum = (UCHAR)(sum + g_WinMaliEdid[i]);
    }
    DbgPrint("[WinMali]    EDID ready: checksum=0x%02x verify_sum=%u (must be 0)\n",
             g_WinMaliEdid[127], (unsigned)sum);
}

_Success_(return == STATUS_SUCCESS)
NTSTATUS
WinMaliEdidCopy(_In_ ULONG Offset, _In_ ULONG Length, _Out_writes_bytes_(Length) PVOID Buffer,
                _Out_ ULONG *pSupplied)
{
    if (Buffer == NULL || Length == 0 || pSupplied == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *pSupplied = 0;
    WinMaliEdidInit();
    /* ⛔ PAST THE END IS "NO MORE DATA", NOT "BAD REQUEST".
       dxgk walks the descriptor in blocks and probes with a generous length;
       we advertise zero extensions (EDID byte 126 = 0) so only block 0 exists.
       STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA (0xC01D0008, ntstatus.h:17170) is
       the sentinel a conforming monitor driver returns; STATUS_INVALID_PARAMETER
       says "your request was malformed" and invites dxgk to discard the whole
       descriptor as bad rather than merely finished. */
    if (Offset >= WINMALI_EDID_SIZE) {
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    }
    /* ⛔ CLAMP an oversized read instead of refusing it. dxgk's first probe asks
       for 256 bytes and we hold 128; refusing that probe was answering "bad
       request" to a perfectly legal question. */
    if (Offset + Length > WINMALI_EDID_SIZE) {
        Length = WINMALI_EDID_SIZE - Offset;
    }
    RtlCopyMemory(Buffer, &g_WinMaliEdid[Offset], Length);
    *pSupplied = Length;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Remaining display-family DDIs (were NULL)                               */
/* ---------------------------------------------------------------------- */
/*
 * dxgk stops right after QueryChildRelations no matter what we vary, and these
 * are the display-family entries still missing. The pointed one is
 * StopDeviceAndReleasePostDisplayOwnership: it exists precisely FOR a driver
 * that takes over the firmware's POST framebuffer, which is exactly us.
 * SystemDisplayEnable/Write are the bugcheck-time display path a display driver
 * is expected to provide.
 */

NTSTATUS APIENTRY
WinMaliKmdSetPointerPosition(_In_ CONST HANDLE hAdapter,
                             _In_ CONST DXGKARG_SETPOINTERPOSITION *pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) { return STATUS_INVALID_PARAMETER; }
    if (pArgs->VidPnSourceId != WINMALI_SOURCE_ID) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    return STATUS_SUCCESS;      /* no cursor plane; OS composites in software */
}

NTSTATUS APIENTRY
WinMaliKmdSetPointerShape(_In_ CONST HANDLE hAdapter,
                          _In_ CONST DXGKARG_SETPOINTERSHAPE *pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) { return STATUS_INVALID_PARAMETER; }
    /* NOT_SUPPORTED is the documented "no hardware cursor" answer. SUCCESS would
       claim we had shown a cursor we never drew. */
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY
WinMaliKmdGetScanLine(_In_ CONST HANDLE hAdapter,
                      _Inout_ DXGKARG_GETSCANLINE *pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) { return STATUS_INVALID_PARAMETER; }
    if (pArgs->VidPnTargetId != WINMALI_TARGET_ID) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }
    /* We cannot read the DISP line counter yet; a permanent vblank is the
       conventional answer and stops callers waiting on a scanline that never
       advances. */
    pArgs->InVerticalBlank = TRUE;
    pArgs->ScanLine        = 0;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdGetChildContainerId(_In_ PVOID MiniportDeviceContext,
                              _In_ ULONG ChildUid,
                              _Inout_ PDXGK_CHILD_CONTAINER_ID ContainerId)
{
    /* A fixed, arbitrary-but-stable GUID naming the physical enclosure this
       panel lives in. It must not change across boots: Windows groups devices
       into a physical "container" by this value, and a GUID that moves would
       make the panel look like a different piece of hardware each time. */
    static CONST GUID kWinMaliPanelContainer =
        { 0x6d2f9a41, 0x8b17, 0x4c3e,
          { 0x9a, 0x55, 0x21, 0xe0, 0x74, 0x33, 0xbc, 0x08 } };

    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    if (ContainerId == NULL) { return STATUS_INVALID_PARAMETER; }
    if (ChildUid != WINMALI_CHILD_UID) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }

    WinMaliDodDdi(WINMALI_DOD_DDI_CONTAINERID);
    ContainerId->ContainerId = kWinMaliPanelContainer;

    /* EldInfo is the HDMI/DP Ethernet-Like-Data used to match an audio
       endpoint to this display. There is no audio path over an internal panel,
       so zero is the correct answer rather than an invented one. */
    ContainerId->EldInfo.PortId           = 0;
    ContainerId->EldInfo.ManufacturerName = 0;
    ContainerId->EldInfo.ProductCode      = 0;

    DbgPrint("[WinMali]    GetChildContainerId: uid %lu\n", ChildUid);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdUpdateMonitorLinkInfo(_In_ CONST HANDLE hAdapter,
                                _Inout_ DXGKARG_UPDATEMONITORLINKINFO *pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) { return STATUS_INVALID_PARAMETER; }
    if (pArgs->VideoPresentTargetId != WINMALI_TARGET_ID) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }

    /*
     * "Reports static per monitor capabilities" (d3dkmddi.h). The OS fills
     * MonitorLinkInfo with its own overrides on the way in and we may narrow
     * them. We have no link hardware to describe yet -- the panel is lit by
     * EDK2 and we scan out of its framebuffer -- so accepting the OS view
     * unchanged is the honest answer. Claiming capabilities we cannot deliver
     * here would be a lie that surfaces later as a mode we cannot set.
     */
    DbgPrint("[WinMali]    UpdateMonitorLinkInfo: target %u, accepting OS defaults\n",
             (unsigned)pArgs->VideoPresentTargetId);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdStopDeviceAndReleasePostDisplayOwnership(
    _In_  CONST PVOID                    MiniportDeviceContext,
    _In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
    _Out_ PDXGK_DISPLAY_INFORMATION      DisplayInfo)
{
    WinMaliDodDdi(WINMALI_DOD_DDI_STOPANDRELEASE);
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(TargetId);
    if (DisplayInfo == NULL) { return STATUS_INVALID_PARAMETER; }

    /* We never took exclusive ownership -- EDK2 lit the panel and we scan out of
       the same buffer -- so describing it and succeeding is the honest answer. */
    RtlZeroMemory(DisplayInfo, sizeof(*DisplayInfo));
    DisplayInfo->Width                  = WINMALI_FB_WIDTH;
    DisplayInfo->Height                 = WINMALI_FB_HEIGHT;
    DisplayInfo->Pitch                  = WINMALI_FB_STRIDE;
    DisplayInfo->ColorFormat            = D3DDDIFMT_X8R8G8B8;
    DisplayInfo->TargetId               = WINMALI_TARGET_ID;
    DisplayInfo->PhysicAddress.QuadPart = (LONGLONG)WINMALI_FB_SCANOUT_PA;
    DbgPrint("[WinMali]    ReleasePostDisplayOwnership: %ux%u pitch=%u pa=0x%llx\n",
             WINMALI_FB_WIDTH, WINMALI_FB_HEIGHT, WINMALI_FB_STRIDE,
             (unsigned long long)WINMALI_FB_SCANOUT_PA);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY
WinMaliKmdSystemDisplayEnable(_In_  PVOID                                MiniportDeviceContext,
                              _In_  D3DDDI_VIDEO_PRESENT_TARGET_ID       TargetId,
                              _In_  PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                              _Out_ UINT                                *Width,
                              _Out_ UINT                                *Height,
                              _Out_ D3DDDIFORMAT                        *ColorFormat)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    if (Width == NULL || Height == NULL || ColorFormat == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Width       = WINMALI_FB_WIDTH;
    *Height      = WINMALI_FB_HEIGHT;
    *ColorFormat = D3DDDIFMT_X8R8G8B8;
    return STATUS_SUCCESS;
}

VOID APIENTRY
WinMaliKmdSystemDisplayWrite(_In_ PVOID MiniportDeviceContext,
                             _In_ PVOID Source,
                             _In_ UINT  SourceWidth,
                             _In_ UINT  SourceHeight,
                             _In_ UINT  SourceStride,
                             _In_ UINT  PositionX,
                             _In_ UINT  PositionY)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(Source);
    UNREFERENCED_PARAMETER(SourceWidth);
    UNREFERENCED_PARAMETER(SourceHeight);
    UNREFERENCED_PARAMETER(SourceStride);
    UNREFERENCED_PARAMETER(PositionX);
    UNREFERENCED_PARAMETER(PositionY);
    /* Runs at HIGH_LEVEL after a bugcheck. Blitting needs a mapping we do not
       hold at that IRQL -- do nothing rather than fault inside the bugcheck path
       and lose the stop code, which on this machine is the only crash evidence
       we ever get (UFS has no dump mode). */
}

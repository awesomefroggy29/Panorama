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
    pMode->Format.Graphics.PixelFormat          = D3DDDIFMT_X8R8G8B8;
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
    pMode->Preference = D3DKMDT_MP_PREFERRED;
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

/* The OUT BOOLEAN is the answer channel. ⛔ Never return a STATUS_GRAPHICS_* code
   to mean "unsupported" -- that is a hard failure, not an answer. */
NTSTATUS APIENTRY
WinMaliKmdIsSupportedVidPn(_In_ CONST HANDLE hAdapter,
                           _Inout_ DXGKARG_ISSUPPORTEDVIDPN *pArgs)
{
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
        return STATUS_SUCCESS;
    }
    if (numPaths != 1) {
        /* Not ours, but a perfectly legal question. Costs no acquire. */
        return STATUS_SUCCESS;
    }

    /* ⛔ pfnAcquirePathInfo(0,0) rather than the First/Next enumeration: the
       enumeration terminator is SEVERITY_SUCCESS, so a `while (NT_SUCCESS(...))`
       loop never ends and leaks an OS object per pass. */
    status = pTopoIface->pfnAcquirePathInfo(hTopo, WINMALI_SOURCE_ID,
                                            WINMALI_TARGET_ID, &pPath);
    if (status == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY) {
        return STATUS_SUCCESS;            /* one path, but not source0->target0 */
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    pArgs->IsVidPnSupported = TRUE;
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
    CONST DXGK_VIDPN_INTERFACE *pIface = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopo = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pTopoIface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath = NULL;
    D3DKMDT_VIDPN_PRESENT_PATH local;
    NTSTATUS status;

    if (adapter == NULL || pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = WinMaliAcquireVidPnIface(adapter, pArgs->hConstrainingVidPn, &pIface);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = pIface->pfnGetTopology(pArgs->hConstrainingVidPn, &hTopo, &pTopoIface);
    if (!NT_SUCCESS(status)) {
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
        /* Our path simply is not in this VidPN. Nothing to constrain. */
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
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
    if (!((pArgs->EnumPivotType == D3DKMDT_EPT_SCALING ||
           pArgs->EnumPivotType == D3DKMDT_EPT_ROTATION) &&
          pArgs->EnumPivot.VidPnSourceId == pPath->VidPnSourceId &&
          pArgs->EnumPivot.VidPnTargetId == pPath->VidPnTargetId)) {

        local = *pPath;
        local.ContentTransformation.ScalingSupport.Identity  = 1;
        local.ContentTransformation.RotationSupport.Identity = 1;
        status = pTopoIface->pfnUpdatePathSupportInfo(hTopo, &local);
    }

Done:
    (VOID)pTopoIface->pfnReleasePathInfo(hTopo, pPath);
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

    if (adapter == NULL || pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* ⛔ Do NOT compare against D3DDDI_ID_NOTAPPLICABLE: it is (UINT)0, which
       collides with our only valid source id. */
    if (pArgs->AffectedVidPnSourceId != WINMALI_SOURCE_ID &&
        pArgs->AffectedVidPnSourceId != D3DDDI_ID_ALL) {
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }

    status = WinMaliAcquireVidPnIface(adapter, pArgs->hFunctionalVidPn, &pIface);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = pIface->pfnGetTopology(pArgs->hFunctionalVidPn, &hTopo, &pTopoIface);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = pTopoIface->pfnGetNumPaths(hTopo, &numPaths);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (numPaths == 0) {
        DbgPrint("[WinMali]    CommitVidPn: empty topology (display off) -> OK\n");
        return STATUS_SUCCESS;                       /* turn-off path */
    }

    /* Scanout is a fixed, already-live framebuffer: there is nothing to program.
       Validating and succeeding is the whole job until DISP is brought up. */
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

    UNREFERENCED_PARAMETER(hAdapter);
    if (pArgs == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    p = &pArgs->VidPnPresentPathInfo;

    if (p->VidPnSourceId != WINMALI_SOURCE_ID || p->VidPnTargetId != WINMALI_TARGET_ID) {
        return STATUS_GRAPHICS_INVALID_VIDPN_PRESENT_PATH;
    }
    if (p->ContentTransformation.Scaling != D3DKMDT_VPPS_IDENTITY &&
        p->ContentTransformation.Scaling != D3DKMDT_VPPS_UNPINNED &&
        p->ContentTransformation.Scaling != D3DKMDT_VPPS_NOTSPECIFIED) {
        return STATUS_GRAPHICS_INVALID_VIDPN_PRESENT_PATH;
    }
    if (p->ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY &&
        p->ContentTransformation.Rotation != D3DKMDT_VPPR_UNPINNED &&
        p->ContentTransformation.Rotation != D3DKMDT_VPPR_NOTSPECIFIED) {
        return STATUS_GRAPHICS_INVALID_VIDPN_PRESENT_PATH;
    }
    /* Content (incl. D3DKMDT_VPPC_VIDEO), ImportanceOrdinal, colour basis and the
       gamma ramp are accepted and ignored -- rejecting VPPC_VIDEO breaks playback,
       and GammaRamp.Data.pRaw must never be dereferenced. */
    return STATUS_SUCCESS;
}

/* No sanctioned "cannot blank" status exists -- the documented return set is
   STATUS_SUCCESS alone. Honour the observable half: while invisible, scanout must
   not be updated. */
NTSTATUS APIENTRY
WinMaliKmdSetVidPnSourceVisibility(_In_ CONST HANDLE hAdapter,
                                   _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pArgs)
{
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
            if (pArgs->Visible) {
                InterlockedExchange(&adapter->SourceVisibleEver, 1);
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
    }
    return STATUS_SUCCESS;
}

/* ⛔ D3DKMDT_VIDPN_HW_CAPABILITY has no `Value` alias -- zero the struct. */
NTSTATUS APIENTRY
WinMaliKmdQueryVidPnHWCapability(_In_ CONST HANDLE hAdapter,
                                 _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pArgs)
{
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

/* We return STATUS_MONITOR_NO_DESCRIPTOR from QueryDeviceDescriptor (there is no
   EDID), so the monitor source mode set has no descriptor-derived modes. Ship the
   accept-and-add-nothing form first; only fill a D3DKMDT_MONITOR_SOURCE_MODE if
   dxgk fails to converge without one. */
NTSTATUS APIENTRY
WinMaliKmdRecommendMonitorModes(_In_ CONST HANDLE hAdapter,
                                _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pArgs)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
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
WinMaliEdidCopy(_In_ ULONG Offset, _In_ ULONG Length, _Out_writes_bytes_(Length) PVOID Buffer)
{
    if (Buffer == NULL || Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    WinMaliEdidInit();
    if (Offset >= WINMALI_EDID_SIZE || Offset + Length > WINMALI_EDID_SIZE) {
        /* dxgk walks the descriptor in blocks; anything past block 0 does not
           exist because we advertise zero extensions (EDID byte 126 = 0). */
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(Buffer, &g_WinMaliEdid[Offset], Length);
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

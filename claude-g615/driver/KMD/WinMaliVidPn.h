/*
 * WinMaliVidPn.h — the ONE display mode, defined once.
 *
 * Every VidPN DDI reads these. They must never be duplicated as literals: if
 * EnumVidPnCofuncModality publishes one ColorBasis and CommitVidPn validates a
 * different one, the OS pins a mode we then reject and the adapter never starts.
 *
 * Geometry comes from our own firmware (tb375fcPkg/tb375fc.dsc): LK/EDK2 leaves a
 * live NT36532 scanout at 2944x1840x32 with stride 11776.
 *
 * Timing self-check that must hold, or the OS answers with
 * STATUS_GRAPHICS_INVALID_FREQUENCY (0xC01E030A):
 *     PIXELRATE / TOTAL_CX == HSYNC        325017600 / 2944 = 110400 exactly
 *     HSYNC      / TOTAL_CY == VSYNC       110400    / 1840 = 60     exactly
 */
#pragma once

#define WINMALI_FB_WIDTH        2944u
#define WINMALI_FB_HEIGHT       1840u
#define WINMALI_FB_STRIDE       11776u          /* bytes, = WIDTH * 4 */
#define WINMALI_FB_BPP          32u
#define WINMALI_FB_SCANOUT_PA   0xFD619400ULL   /* live buffer, LK-STAGE path */

/*
 * ---- MODE TIMING: THE EDID AND THE SIGNAL INFO MUST AGREE EXACTLY ----------
 *
 * ⛔ ZERO BLANKING WAS UNREPRESENTABLE AND BROKE THE WHOLE TOPOLOGY.
 *
 * The old mode used TotalSize == ActiveSize (no blanking), giving
 *     2944 x 1840 x 60 = 325,017,600 Hz.
 * An EDID detailed-timing descriptor stores the pixel clock in units of 10 kHz,
 * and 325,017,600 / 10,000 = 32501.76 - NOT AN INTEGER, so that clock cannot be
 * encoded. The DTD therefore carried 32502 (= 325,020,000 Hz) while
 * WinMaliFillSignalInfo published 325,017,600, and the derived refresh became
 * 60.000443 Hz instead of 60/1.
 *
 * MEASURED CONSEQUENCE (2026-08-12), read back from Windows itself via
 * WmiMonitorListedSupportedSourceModes on the live box:
 *     NumOfMonitorSourceModes = 1
 *     2944x1840  pixelrate=325020000  vsync=2031375/33856
 * i.e. the monitor's ONLY source mode disagreed with our target mode in both
 * pixel rate and refresh. Nothing can be cofunctional across that, so the OS
 * fell back to committing an EMPTY topology ("display off") - which is why
 * SetVidPnSourceVisibility(TRUE) never came and flips never exceeded 1.
 *
 * FIX: give the mode REAL blanking, chosen so the clock lands exactly on a
 * 10 kHz boundary:
 *     HTOTAL 3000 (2944 active + 56 blank), VTOTAL 1850 (1840 + 10)
 *     3000 * 1850 * 60 = 333,000,000 Hz = 33300 x 10 kHz  EXACTLY
 * Both the EDID DTD and WinMaliFillSignalInfo now derive from these same
 * constants, and the C_ASSERTs below make an unrepresentable clock a BUILD
 * failure rather than a silent 2.4 kHz disagreement.
 *
 * ⚠ This changes only what we DESCRIBE. We own no CRTC, so nothing is actually
 * clocked at 333 MHz - the panel keeps scanning out exactly as before. The
 * blanking exists so the description is legal and self-consistent.
 */
#define WINMALI_H_TOTAL         3000u           /* 2944 active + 56 blanking  */
#define WINMALI_V_TOTAL         1850u           /* 1840 active + 10 blanking  */
#define WINMALI_H_BLANK         (WINMALI_H_TOTAL - WINMALI_FB_WIDTH)   /* 56 */
#define WINMALI_V_BLANK         (WINMALI_V_TOTAL - WINMALI_FB_HEIGHT)  /* 10 */

/* Blanking split into porches/sync. Values are conventional and only have to
   fit inside the blank interval; the C_ASSERTs enforce that. */
#define WINMALI_H_SYNC_OFF      16u             /* front porch                */
#define WINMALI_H_SYNC_WIDTH    16u
#define WINMALI_V_SYNC_OFF      3u
#define WINMALI_V_SYNC_WIDTH    4u

#define WINMALI_VSYNC_NUM       60u
#define WINMALI_VSYNC_DEN       1u
#define WINMALI_HSYNC_NUM       (WINMALI_VSYNC_NUM * WINMALI_V_TOTAL)  /* 111000 */
#define WINMALI_HSYNC_DEN       1u
#define WINMALI_PIXEL_RATE      333000000ULL

/* The EDID DTD pixel-clock field, in 10 kHz units. Must be exact. */
#define WINMALI_EDID_PIXCLK_10K (WINMALI_PIXEL_RATE / 10000ULL)         /* 33300 */

/* Not settled by the headers (SRGB vs SCRGB). What matters is that exactly ONE
   constant is read everywhere; an 8-bit-per-channel desktop primary is
   gamma-encoded, which is what D3DKMDT_CB_SRGB describes. */
#define WINMALI_COLOR_BASIS     D3DKMDT_CB_SRGB

/* Our only source/target ids. */
#define WINMALI_SOURCE_ID       0u
#define WINMALI_TARGET_ID       0u

C_ASSERT(WINMALI_FB_STRIDE == WINMALI_FB_WIDTH * 4u);

/* Timing is self-consistent: clock = htotal * vtotal * vrefresh. */
C_ASSERT(WINMALI_PIXEL_RATE == (ULONGLONG)WINMALI_H_TOTAL * WINMALI_V_TOTAL * WINMALI_VSYNC_NUM);
C_ASSERT(WINMALI_HSYNC_NUM  == WINMALI_PIXEL_RATE / WINMALI_H_TOTAL);

/* ⛔ THE ONE THAT MATTERS: an EDID DTD stores the pixel clock in 10 kHz units,
   so a rate that is not a whole multiple of 10 kHz CANNOT be described. That is
   exactly how the old 325,017,600 Hz mode ended up disagreeing with its own
   EDID by 2,400 Hz and made every topology non-cofunctional. Fail the BUILD
   rather than ship a mode the EDID cannot express. */
C_ASSERT((WINMALI_PIXEL_RATE % 10000ULL) == 0);
C_ASSERT(WINMALI_EDID_PIXCLK_10K <= 0xFFFFULL);   /* DTD field is 16-bit */

/* Porches must fit inside the blanking, or the DTD is self-contradictory. */
C_ASSERT(WINMALI_H_TOTAL > WINMALI_FB_WIDTH);
C_ASSERT(WINMALI_V_TOTAL > WINMALI_FB_HEIGHT);
C_ASSERT(WINMALI_H_SYNC_OFF + WINMALI_H_SYNC_WIDTH <= WINMALI_H_BLANK);
C_ASSERT(WINMALI_V_SYNC_OFF + WINMALI_V_SYNC_WIDTH <= WINMALI_V_BLANK);

/* EDID encodes active/blank as 12-bit (8 low + 4 high) and the sync fields as
   10/6-bit. Anything wider would be silently truncated into a different mode. */
C_ASSERT(WINMALI_FB_WIDTH  <= 0xFFFu && WINMALI_H_BLANK <= 0xFFFu);
C_ASSERT(WINMALI_FB_HEIGHT <= 0xFFFu && WINMALI_V_BLANK <= 0xFFFu);
C_ASSERT(WINMALI_H_SYNC_OFF <= 0x3FFu && WINMALI_H_SYNC_WIDTH <= 0x3FFu);
C_ASSERT(WINMALI_V_SYNC_OFF <= 0x3Fu  && WINMALI_V_SYNC_WIDTH <= 0x3Fu);

/*
 * ⛔ SEVERITY_SUCCESS GRAPHICS CODES.
 * STATUS_GRAPHICS_MODE_NOT_PINNED (0x401E0307), _DATASET_IS_EMPTY (0x401E034B)
 * and _NO_MORE_ELEMENTS_IN_DATASET (0x401E034C) all have SEVERITY_SUCCESS, so
 * NT_SUCCESS() is TRUE for them. `if (!NT_SUCCESS(s)) goto Cleanup;` therefore
 * sails straight past "nothing is pinned" and dereferences an out-pointer the
 * callee never wrote -- on the first EnumVidPnCofuncModality of every device
 * start. Test for them explicitly.
 */
#define WINMALI_IS_MODE_NOT_PINNED(s)  ((s) == STATUS_GRAPHICS_MODE_NOT_PINNED)

NTSTATUS APIENTRY WinMaliKmdIsSupportedVidPn(_In_ CONST HANDLE hAdapter,
    _Inout_ DXGKARG_ISSUPPORTEDVIDPN *pArgs);
NTSTATUS APIENTRY WinMaliKmdRecommendFunctionalVidPn(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pArgs);
NTSTATUS APIENTRY WinMaliKmdRecommendVidPnTopology(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pArgs);
NTSTATUS APIENTRY WinMaliKmdEnumVidPnCofuncModality(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pArgs);
NTSTATUS APIENTRY WinMaliKmdCommitVidPn(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_COMMITVIDPN *CONST pArgs);
NTSTATUS APIENTRY WinMaliKmdUpdateActiveVidPnPresentPath(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pArgs);
NTSTATUS APIENTRY WinMaliKmdSetVidPnSourceVisibility(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pArgs);
NTSTATUS APIENTRY WinMaliKmdSetVidPnSourceAddress(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_SETVIDPNSOURCEADDRESS *pArgs);
NTSTATUS APIENTRY WinMaliKmdQueryVidPnHWCapability(_In_ CONST HANDLE hAdapter,
    _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pArgs);
NTSTATUS APIENTRY WinMaliKmdRecommendMonitorModes(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pArgs);

NTSTATUS APIENTRY WinMaliKmdSetPointerPosition(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_SETPOINTERPOSITION *pArgs);
NTSTATUS APIENTRY WinMaliKmdSetPointerShape(_In_ CONST HANDLE hAdapter,
    _In_ CONST DXGKARG_SETPOINTERSHAPE *pArgs);
NTSTATUS APIENTRY WinMaliKmdGetScanLine(_In_ CONST HANDLE hAdapter,
    _Inout_ DXGKARG_GETSCANLINE *pArgs);
NTSTATUS APIENTRY WinMaliKmdStopDeviceAndReleasePostDisplayOwnership(
    _In_ CONST PVOID MiniportDeviceContext,
    _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
    _Out_ PDXGK_DISPLAY_INFORMATION DisplayInfo);
NTSTATUS APIENTRY WinMaliKmdSystemDisplayEnable(_In_ PVOID MiniportDeviceContext,
    _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
    _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
    _Out_ UINT *Width, _Out_ UINT *Height, _Out_ D3DDDIFORMAT *ColorFormat);
VOID APIENTRY WinMaliKmdSystemDisplayWrite(_In_ PVOID MiniportDeviceContext,
    _In_ PVOID Source, _In_ UINT SourceWidth, _In_ UINT SourceHeight,
    _In_ UINT SourceStride, _In_ UINT PositionX, _In_ UINT PositionY);

/* dxgk carries a dedicated adapter-start failure reason for this DDI being
   absent -- StartAdapter_DxgkDdiUpdateMonitorLinkInfoIsNull (reason 19), read
   straight out of dxgkrnl.sys's own value map. So it is not optional for a
   display adapter, and leaving it NULL is a wall waiting behind reason 15. */
NTSTATUS APIENTRY WinMaliKmdUpdateMonitorLinkInfo(_In_ CONST HANDLE hAdapter,
    _Inout_ DXGKARG_UPDATEMONITORLINKINFO *pArgs);

/* Called PER CHILD while dxgk builds the child devnode -- i.e. inside the exact
   stage that is failing (StartAdapter_DpiFdoEnumChildDevicesFailed). Unlike
   QueryChildStatus and UpdateMonitorLinkInfo it has no dedicated "IsNull"
   failure reason, so a NULL here would surface as a generic NOT_SUPPORTED --
   which is precisely the status we get. */
NTSTATUS APIENTRY WinMaliKmdGetChildContainerId(_In_ PVOID MiniportDeviceContext,
    _In_ ULONG ChildUid, _Inout_ PDXGK_CHILD_CONTAINER_ID ContainerId);

/* Synthetic EDID for the fixed internal panel -- see the long comment in
   WinMaliVidPn.c. dxgk rejects a child that cannot expose a descriptor
   (0xC01E0401), which is what STATUS_MONITOR_NO_DESCRIPTOR was causing. */
VOID WinMaliEdidInit(VOID);
NTSTATUS WinMaliEdidCopy(_In_ ULONG Offset, _In_ ULONG Length,
                         _Out_writes_bytes_(Length) PVOID Buffer);



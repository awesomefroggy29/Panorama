
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

/* Parameters\FailOnGpuFault: make a draw that FAULTED return a failed submit
   instead of reporting success.
   Default 0 (report + count only). A faulting draw used to be completely
   invisible - the ISR acked the IRQ, only the first fault per boot was logged,
   and the submit still returned STATUS_SUCCESS, so the UMD believed it had
   rendered and simply retried forever. Counting and reporting every fault is
   the fix; FAILING the submit on top of that is a real behaviour change that
   could crash-loop the compositor, so it stays opt-in until measured. */
ULONG g_WinMaliFailOnGpuFault = 0;

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
    (VOID)WinMaliReadParamsDword(RegistryPath, L"ComputeOnly", 1, &g_WinMaliComputeOnly);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FeatureIface", 1, &g_WinMaliFeatureIface);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"HybridRole", 0, &g_WinMaliHybridRole);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"TraceSubmit", 0, &g_WinMaliTraceSubmit);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentTimerHz", 30,
                                 &g_WinMaliPresentTimerHz);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FailOnGpuFault", 0,
                                 &g_WinMaliFailOnGpuFault);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentMarker", 0, &g_WinMaliPresentMarker);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentTestPattern", 0,
                                 &g_WinMaliPresentTestPattern);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"ChildCfg", 0, &g_WinMaliChildCfg);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"VidPnSources", 0, &g_WinMaliVidPnSources);
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

        if (g_WinMaliComputeOnly != 0) {
            /* Disarmed. Clear any stale stamp so the NEXT arm starts clean --
               otherwise an arm in a later boot would inherit an old stamp and
               disarm itself immediately. */
            if (armedStamp != 0) {
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampLo", 0);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampHi", 0);
            }
        } else if (persistent != 0) {
            DbgPrint("[WinMali]    ARM: display path armed and ArmPersistent=1 -- boot guard OFF\n");
        } else if (armedStamp == 0) {
            /* First load of a freshly armed config: record which boot owns it. */
            (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampLo",
                                          (ULONG)(bootStamp & 0xFFFFFFFFull));
            (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampHi",
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
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampLo",
                                              (ULONG)(bootStamp & 0xFFFFFFFFull));
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampHi",
                                              (ULONG)(bootStamp >> 32));
            } else {
                DbgPrint("[WinMali] ** ARM: armed config is from a PREVIOUS BOOT "
                         "(delta %I64d ms) and safe mode ALREADY FAILED "
                         "(ArmDisarmCount=%lu). Disarming completely.\n",
                         (LONGLONG)(delta / 10000), disarms);
                g_WinMaliComputeOnly  = 1;
                g_WinMaliVidPnSources = 0;
                g_WinMaliChildCount   = 0;
                g_WinMaliChildCfg     = 0;
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ComputeOnly",  1);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"VidPnSources", 0);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ChildCount",   0);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ChildCfg",     0);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampLo", 0);
                (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmBootStampHi", 0);
            }
            /* Publish it: "the test didn't crash" must be a measurement, not an
               assumption. A nonzero ArmDisarmCount means a previous armed run
               died before its restore step. */
            (VOID)WinMaliWriteParamsDword(RegistryPath, L"ArmDisarmCount", disarms + 1);
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
    WinMaliSyncObjTableInit(&adapter->SyncObjTable);
    WinMaliVmTableInit(&adapter->VmTable);
    WinMaliGroupTableInit(&adapter->GroupTable);

    g_WinMaliAdapter       = adapter;
    *MiniportDeviceContext = adapter;

    DbgPrint("[WinMali]    AddDevice ctx=%p pdo=%p\n", adapter, PhysicalDeviceObject);
    WM_TRACE_EXIT_S("AddDevice", STATUS_SUCCESS);
    return STATUS_SUCCESS;
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
    (VOID)InterlockedIncrement64(&adapter->VsyncNotifyCount);

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

    for (;;) {
        KeWaitForSingleObject(&a->PresentWake, Executive, KernelMode, FALSE, NULL);
        if (InterlockedCompareExchange(&a->PresentStop, 0, 0) != 0) {
            break;
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
                {
                    /* One row per qualifying candidate: bo / pid / hits / pages.
                       Named individually so a plain registry read decodes them
                       without any packing convention to get wrong later. */
                    static const PCWSTR boN[4]   = { L"UvCand0Bo",   L"UvCand1Bo",
                                                     L"UvCand2Bo",   L"UvCand3Bo" };
                    static const PCWSTR pidN[4]  = { L"UvCand0Pid",  L"UvCand1Pid",
                                                     L"UvCand2Pid",  L"UvCand3Pid" };
                    static const PCWSTR hitN[4]  = { L"UvCand0Hits", L"UvCand1Hits",
                                                     L"UvCand2Hits", L"UvCand3Hits" };
                    static const PCWSTR pgN[4]   = { L"UvCand0Pages",L"UvCand1Pages",
                                                     L"UvCand2Pages",L"UvCand3Pages" };
                    static const PCWSTR nbN[4]   = { L"UvCand0NonBlk",L"UvCand1NonBlk",
                                                     L"UvCand2NonBlk",L"UvCand3NonBlk" };
                    static const PCWSTR chN[4]   = { L"UvCand0Chg",  L"UvCand1Chg",
                                                     L"UvCand2Chg",  L"UvCand3Chg" };
                    static const PCWSTR scN[4]   = { L"UvCand0Scans",L"UvCand1Scans",
                                                     L"UvCand2Scans",L"UvCand3Scans" };
                    ULONG ci;
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
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsSubmits",    (ULONG)g_WinMaliStatCsSubmitCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"CsOk",         (ULONG)g_WinMaliStatCsOkCount);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"RingInsert",   (ULONG)g_WinMaliStatRingInsert);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                            L"RingExtract",  (ULONG)g_WinMaliStatRingExtract);
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

                    for (ci = 0; ci < 4; ++ci) {
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, boN[ci],
                                (ULONG)g_WinMaliUvCandBo[ci]);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, pidN[ci],
                                (ULONG)g_WinMaliUvCandPid[ci]);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, hitN[ci],
                                (ULONG)g_WinMaliUvCandHits[ci]);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, pgN[ci],
                                (ULONG)g_WinMaliUvCandPages[ci]);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, nbN[ci],
                                (ULONG)g_WinMaliUvCandNonBlack[ci]);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, chN[ci],
                                (ULONG)g_WinMaliUvCandChg[ci]);
                        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, scN[ci],
                                (ULONG)g_WinMaliUvCandScans[ci]);
                    }
                }
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatApPrevSrc",         (ULONG)g_WinMaliStatApPrevSrc);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentNullCookie", (ULONG)g_WinMaliStatPresentNullCookie);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentBlts",       (ULONG)g_WinMaliStatPresentBlts);
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
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatScanoutSkipped",    (ULONG)a->ScanoutBltSkipped);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatFlipAddrLo",
                        (ULONG)((UINT64)InterlockedCompareExchange64(&a->LastFlipAddress, 0, 0) & 0xFFFFFFFFu));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentPrivSize",   (ULONG)g_WinMaliStatPresentPrivSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentPrivHits",   (ULONG)g_WinMaliStatPresentPrivHits);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentSrcHandle",  (ULONG)(0x5000u + (ULONG)g_WinMaliStatApWithContent));   /* OVERLOADED: +0x5000 = apertureWithContent */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatPresentDstHandle",  (ULONG)(0x5000u + (ULONG)g_WinMaliStatUmdPresentCtx));   /* OVERLOADED: +0x5000 = UMD _Present ctx */
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

                /* 🎯 FIRST CHOICE: A PAINTED 0x93 BO.
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
                    PVOID uv = WinMaliUvSrcAcquireBest(bytes,
                                                       WINMALI_FB_STRIDE,
                                                       WINMALI_FB_HEIGHT,
                                                       &uvHandle, &uvHits);
                    (VOID)InterlockedExchange(&g_WinMaliStatUvSrcHits,
                                              (LONG)uvHits);
                    if (uv != NULL && uvHits > 0) {
                        srcVa  = (PUCHAR)uv;
                        uvHeld = TRUE;
                        (VOID)InterlockedExchange(&g_WinMaliStatUvSrcBo,
                                                  (LONG)uvHandle);
                    } else if (uv != NULL) {
                        /* Candidates exist but none is painted - hand the lock
                           back and let the aperture branches try. */
                        WinMaliUvSrcRelease();
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
                if (srcVa == NULL && src != NULL) {
                    PUCHAR base = (PUCHAR)MmGetSystemAddressForMdlSafe(
                                      src->ApertureMdl, NormalPagePriority | MdlMappingNoExecute);
                    if (base != NULL) {
                        /* Byte offset into the allocation. For an address-matched
                           allocation that is (flipAddr - GpuVa); for a
                           flip-recorded one the frame starts at the beginning of
                           the allocation and flipAddr is unrelated to it (it is
                           still the frozen mode-set address), so use 0. */
                        SIZE_T off = 0;
                        if (src->GpuVa != 0 &&
                            flipAddr >= src->GpuVa &&
                            flipAddr < src->GpuVa + (UINT64)src->AperturePageCount * PAGE_SIZE) {
                            off = (SIZE_T)(flipAddr - src->GpuVa);
                        }
                        srcVa = base + (SIZE_T)src->ApertureMdlOffset * PAGE_SIZE + off;
                    }
                }

                if (srcVa == NULL) {
                    /* No aperture allocation covers this address (or the MDL
                       could not be mapped). Do not fall back to DmaSegmentVa -
                       that is exactly the bug being fixed. */
                    (VOID)InterlockedIncrement64(&a->ScanoutBltSkipped);
                    continue;
                }

                /* Prove the source now has content. Same sparse grid as the
                   sampler that found the bug, so the numbers are comparable:
                   it read nonBlack=0 out of 646 from DmaSegmentVa. */
                {
                    static LONG64 s_srcTick = 0;
                    if ((InterlockedIncrement64(&s_srcTick) % 120) == 1) {
                        ULONG orAll = 0, nonBlack = 0, sampled = 0, y, x;
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
                        for (y = 0; y < WINMALI_FB_HEIGHT; y += 97) {
                            const ULONG *row =
                                (const ULONG *)(srcVa + (SIZE_T)y * WINMALI_FB_STRIDE);
                            for (x = 0; x < WINMALI_FB_WIDTH; x += 89) {
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
                                sampled++;
                            }
                        }
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
                    }
                }

                /* Source and destination are both WINMALI_FB_STRIDE-pitched
                   X8R8G8B8 at the panel's native size, so this is one flat
                   copy. */
                RtlCopyMemory(a->ScanoutVa, srcVa, bytes);

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
            pa.QuadPart = (LONGLONG)WINMALI_FB_SCANOUT_PA;
            mapBytes    = frameBytes;
        }

        /* Write-combined: this is a framebuffer, and a fully-cached mapping
           would leave the panel showing stale lines until something flushed. */
        Adapter->ScanoutMapVa = MmMapIoSpaceEx(pa, mapBytes,
                                               PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (Adapter->ScanoutMapVa == NULL) {
            /* Fall back to plain uncached rather than giving up on presenting. */
            Adapter->ScanoutMapVa = MmMapIoSpaceEx(pa, mapBytes, PAGE_READWRITE);
        }
        if (Adapter->ScanoutMapVa == NULL) {
            WINMALI_WARN("Present: could not map framebuffer 0x%llx (%Iu bytes) - "
                         "the panel will stay blank while we own the display",
                         (ULONGLONG)pa.QuadPart, mapBytes);
            return;
        }
        Adapter->ScanoutMapBytes = mapBytes;
        Adapter->ScanoutVa       = (PUCHAR)Adapter->ScanoutMapVa + scanoutOff;
        Adapter->ScanoutBytes    = frameBytes;
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
        MmUnmapIoSpace(Adapter->ScanoutMapVa, Adapter->ScanoutMapBytes);
        Adapter->ScanoutMapVa = NULL;
        Adapter->ScanoutMapBytes = 0;
        Adapter->ScanoutVa = NULL;
    }
}

VOID
WinMaliVsyncInit(_Inout_ PWINMALI_ADAPTER Adapter)
{
    if (Adapter == NULL || Adapter->VsyncInit) { return; }
    KeInitializeDpc(&Adapter->VsyncDpc, WinMaliVsyncDpc, Adapter);
    KeInitializeTimerEx(&Adapter->VsyncTimer, SynchronizationTimer);
    Adapter->VsyncInit = TRUE;
}

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

    status = WinMaliEdidCopy(DeviceDescriptor->DescriptorOffset,
                             DeviceDescriptor->DescriptorLength,
                             DeviceDescriptor->DescriptorBuffer);
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
    WM_TRACE_ENTER("SetPowerState");
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceUid);
    UNREFERENCED_PARAMETER(DevicePowerState);
    UNREFERENCED_PARAMETER(ActionType);
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









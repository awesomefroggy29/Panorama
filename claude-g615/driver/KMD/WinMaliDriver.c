
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
ULONG g_WinMaliUvSrcInvalidate = 1;

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
    (VOID)WinMaliReadParamsDword(RegistryPath, L"AllowHandleDataCb", 0,
                                 &g_WinMaliAllowHandleDataCb);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"SegmentShape", 1,
                                 &g_WinMaliSegmentShape);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"TraceSubmit", 0, &g_WinMaliTraceSubmit);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentTimerHz", 30,
                                 &g_WinMaliPresentTimerHz);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FailOnGpuFault", 0,
                                 &g_WinMaliFailOnGpuFault);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"UvSrcInvalidate", 1,
                                 &g_WinMaliUvSrcInvalidate);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentMarker", 0, &g_WinMaliPresentMarker);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentTestPattern", 0,
                                 &g_WinMaliPresentTestPattern);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FlipSrcFromVidMm", 0,
                                 &g_WinMaliFlipSrcFromVidMm);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"FlipSrcFromAperture", 0,
                                 &g_WinMaliFlipSrcFromAperture);
    (VOID)WinMaliReadParamsDword(RegistryPath, L"PresentCopyBudgetKB", 0,
                                 &g_WinMaliPresentCopyBudgetKB);
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
        settle.QuadPart = -((LONGLONG)WINMALI_PRESENT_SETTLE_MS * 10000LL);
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &settle);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                L"StatPresentSettleMs", (ULONG)WINMALI_PRESENT_SETTLE_MS);
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
            L"StatWinPitch",
            L"StatWinRows",
            L"StatSrcLastRow", L"StatSrcFirstRow",L"StatSrcRowsNZ", L"StatSrcPathMeas",
            L"StatSrcProbePitch", L"StatSweeps",
            L"StatLatchDrop",  L"StatCopyAbort",  L"StatCopyKB",    L"StatUvRevokeTo",
            L"StatSkipNoSrc",  L"StatPowerStop",  L"StatUvSrcAsked",L"StatUvSrcCands",
            L"StatUvSrcTooSmall", L"StatUvSrcUniform", L"StatUvSrcBo", L"StatUvSrcPid",
            L"StatUvSrcPages", L"Stat0x93Calls",  L"Stat0x93Ok",    L"StatPresentCalls",
            L"LiveFlipLatch",  L"StatScanoutWc",
        };
        ULONG ri;
        for (ri = 0; ri < RTL_NUMBER_OF(reserve); ri++) {
            (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, reserve[ri], 0);
        }
    }

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
                /* ★ THE NEW SELECTION SIGNAL. Published in the PERIODIC block,
                   not the copy block, so it is still readable on a tick where no
                   source qualified - which is precisely when it matters. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvCoverage",        (ULONG)g_WinMaliStatUvSrcCoverage);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvMaxCov",          (ULONG)g_WinMaliStatUvSrcMaxCov);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"StatUvQual",            (ULONG)g_WinMaliStatUvSrcQual);
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
                        L"QueryCapsOff",          g_WinMaliQaiFlipCapsOff);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QueryCapsOutSz",        g_WinMaliQaiOutSize);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QueryCapsSizeof",       g_WinMaliQaiCapsSizeof);
                /* ★ The segment flags we actually sent. DirectFlip = 0x400 is the
                   per-segment permission for an allocation to be a flip target;
                   we shipped it as 0 until SegmentShape was added. */
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QuerySegFlags",         g_WinMaliQaiSegFlags);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath,
                        L"QuerySegShape",         g_WinMaliSegmentShape);
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
                if (srcVa == NULL && src != NULL && g_WinMaliFlipSrcFromAperture != 0) {
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
                        srcPath = 2;   /* APERTURE MDL of a dxgk allocation (gated) */
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
                    const ULONG probePitch =
                        (srcPitch != 0) ? srcPitch : WINMALI_FB_STRIDE;
                    if ((InterlockedIncrement64(&s_extTick) % 120) == 1) {
                        ULONG y, x, first = 0xFFFFFFFFu, last = 0, rowsNz = 0;
                        ULONG probeRows = (ULONG)(bytes / probePitch);
                        if (probeRows > WINMALI_FB_HEIGHT) { probeRows = WINMALI_FB_HEIGHT; }
                        for (y = 0; y < probeRows; y += 8) {
                            const ULONG *row =
                                (const ULONG *)(srcVa + (SIZE_T)y * probePitch);
                            BOOLEAN nz = FALSE;
                            for (x = 0; x < (probePitch / 4); x += (probePitch / 64)) {
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
                    const ULONG  dstPitch = WINMALI_FB_STRIDE;
                    ULONG        effPitch = (srcPitch != 0) ? srcPitch : dstPitch;
                    ULONG        rowBytes = (effPitch < dstPitch) ? effPitch : dstPitch;
                    ULONG        maxRows  = WINMALI_FB_HEIGHT;
                    static ULONG s_copyRow = 0;
                    BOOLEAN      restartSweep = FALSE;
                    ULONG        rowsPerWake = (ULONG)(budget / dstPitch);
                    ULONG        startRow, endRow, r, doneRows = 0;
                    if (effPitch == 0) { effPitch = dstPitch; rowBytes = dstPitch; }
                    {
                        SIZE_T avail = (srcBytes > bytes) ? srcBytes : bytes;
                        SIZE_T cap = (avail >= rowBytes)
                                   ? ((avail - rowBytes) / effPitch) + 1 : 0;
                        if (cap < maxRows) { maxRows = (ULONG)cap; }
                    }
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
                        if (ga != NULL) {
                            PWINMALI_KMD_ALLOCATION now =
                                (PWINMALI_KMD_ALLOCATION)InterlockedCompareExchangePointer(
                                    (PVOID volatile *)&g_WinMaliFlipAlloc, NULL, NULL);
                            if (now != ga ||
                                ga->Magic != WINMALI_KMD_ALLOC_MAGIC ||
                                ga->ApertureMdl == NULL ||
                                ga->ApertureVa != gaVa) {
                                (VOID)InterlockedIncrement(&g_WinMaliStatCopyAbort);
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
                        RtlCopyMemory((PUCHAR)a->ScanoutVa + (SIZE_T)r * dstPitch,
                                      srcVa + (SIZE_T)r * effPitch,
                                      rowBytes);
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
                    }
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
        if (Adapter->ScanoutMapVa != NULL) {
            (VOID)InterlockedExchange(&g_WinMaliScanoutWc, 1);
        } else {
            /* ⛔★★★★★ THE UNCACHED FALLBACK IS A ~1000x SLOWDOWN, AND IT WAS
             * SILENT.
             *
             * Uncached MMIO writes on ARM64 run at a few MB/s, so a 21,667,840-byte
             * frame takes on the order of TWENTY SECONDS instead of ~30 ms. That
             * is not a slow frame, it is a wedged machine:
             *   - the panel visibly paints TOP-DOWN over ~20 s (user-observed;
             *     being able to SEE it paint at all is itself proof the mapping
             *     is uncached - write-combined would land in well under a second)
             *   - teardown does KeWaitForSingleObject(PresentWorkerThread), so
             *     Disable-PnpDevice BLOCKS for as long as the copy runs, PnP
             *     times out and the box hangs. Two of three hangs on 2026-08-17
             *     happened during exactly that call, inside deploy_hot.
             *
             * Which attribute won was never recorded, so a catastrophic
             * performance cliff looked identical to a healthy mapping. Count it. */
            (VOID)InterlockedExchange(&g_WinMaliScanoutWc, 0);
            Adapter->ScanoutMapVa = MmMapIoSpaceEx(pa, mapBytes, PAGE_READWRITE);
            WINMALI_WARN("Present: WRITE-COMBINED map REFUSED for 0x%llx (%Iu bytes) - "
                         "falling back to UNCACHED. Expect ~20 s per frame and a "
                         "teardown that blocks that long.",
                         (ULONGLONG)pa.QuadPart, mapBytes);
        }
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
    PWINMALI_ADAPTER a = (PWINMALI_ADAPTER)MiniportDeviceContext;

    WM_TRACE_ENTER("SetPowerState");
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









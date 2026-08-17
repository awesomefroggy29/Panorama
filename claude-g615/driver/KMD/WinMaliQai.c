/*
 * WinMaliQai.c - DxgkDdiQueryAdapterInfo.
 *
 * Real cap-walk implementation. With MMU + the 256 MiB sysmem segment
 * allocated at StartDevice, we now publish numbers dxgk can act on:
 *
 *   DRIVERCAPS                 -> render-only WDDM 3.2 + GpuMmu + VA
 *   WDDMDEVICECAPS             -> WDDMv3_2 echo (pre-StartDevice)
 *   PHYSICAL_MEMORY_CAPS       -> 2^PA_bits-1 from MMU_FEATURES (pre-Start)
 *   IOMMU_CAPS                 -> none (GPU has its own MMU, no SMMU)
 *   HARDWARERESERVEDRANGES(2)  -> zero ranges
 *   PHYSICALADAPTERCAPS        -> our DxgkHandle + GpuMmu, no paging node
 *   GPUMMUCAPS                 -> 30-bit VA, 2-level page table, NX/RO
 *   PAGETABLELEVELDESC         -> 9 idx bits per level, segment id 1
 *   QUERYSEGMENT3              -> 1 segment, sysmem-backed, CpuVisible
 *   QUERYSEGMENT4              -> same data, newer descriptor layout
 *   HISTORYBUFFERPRECISION     -> 64-bit
 *
 * Bounded copy-out (RtlZeroMemory dest first, copy min(outSz, sizeof)) is
 * used everywhere so kernels with smaller struct views than our 26100 WDK
 * can't smash stack/heap by writing past their buffer.
 *
 * Anything we haven't implemented yet falls through to STATUS_NOT_IMPLEMENTED
 * with a log line so kd shows exactly what dxgk still wants from us.
 */

#include "WinMaliKmd.h"

#define WM_QAI_OK(name)  DbgPrint("[WinMali]    QAI[%s] OK\n", name)

/* ★★★★★ ARE OUR DRIVERCAPS EVEN LANDING WHERE dxgk READS THEM?
 *
 * Two cap experiments have now "refuted" cleanly (NoScreenToScreenBlt /
 * NoOverlapScreenBlt, then FlipImmediateMmIo) - each rebuilt, deployed, cold
 * booted, and measured, each changing NOTHING. That is a suspicious pattern, and
 * this file already carries an unexplained anomaly saying why it might be:
 *
 *   "dxgk reported reading 0x8 from a field this code sets to 0"
 *
 * plus the header note above - we compile DXGK_DRIVERCAPS from the 26100 WDK,
 * but the running kernel may have a SMALLER OR DIFFERENT view of that struct. If
 * so, FIELD_OFFSET(DXGK_DRIVERCAPS, FlipCaps) computed from OUR header does not
 * address FlipCaps in THEIRS, every cap we set lands in the wrong field, and
 * every cap-based refutation so far is VOID rather than informative.
 *
 * These are DbgPrint'ed already, which is useless on a box with no kernel
 * debugger. Publish them so a plain registry read answers it.
 * Names start with "Query" so the deploy's deny-list purges them as driver
 * OUTPUT rather than treating them as knobs. */
ULONG g_WinMaliQaiFlipCapsValue = 0;
ULONG g_WinMaliQaiFlipCapsOff   = 0;
ULONG g_WinMaliQaiOutSize       = 0;
ULONG g_WinMaliQaiCapsSizeof    = 0;
/* The segment flags we actually handed dxgk. Same discipline as the caps above:
   the DbgPrint is useless without a kernel debugger, so publish it. Expect
   DirectFlip (0x400) set once SegmentShape >= 1. */
ULONG g_WinMaliQaiSegFlags      = 0;

NTSTATUS APIENTRY
WinMaliKmdQueryAdapterInfo(
    _In_ CONST HANDLE                       hAdapter,
    _In_ CONST DXGKARG_QUERYADAPTERINFO*    pQueryAdapterInfo)
{
    if (pQueryAdapterInfo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* NODEPERFDATA/ADAPTERPERFDATA are polled ~once per second per node
       for as long as the adapter is up. With the serial mirror active,
       printing the entry line for them means dxgkrnl's telemetry worker
       synchronously drains a UART line at 115200 baud every poll -
       forever - which reads as periodic whole-system stalls. The case
       handlers below are already silent for these; the entry print must
       be too. */
    if (pQueryAdapterInfo->Type != DXGKQAITYPE_NODEPERFDATA &&
        pQueryAdapterInfo->Type != DXGKQAITYPE_ADAPTERPERFDATA) {
        DbgPrint("[WinMali]    QAI[%u] enter outsz=%u\n",
                 (unsigned)pQueryAdapterInfo->Type,
                 (unsigned)pQueryAdapterInfo->OutputDataSize);
    }

    switch (pQueryAdapterInfo->Type) {

    case DXGKQAITYPE_UMDRIVERPRIVATE: {
        /* The D3D runtime forwards KMTQAITYPE_UMDRIVERPRIVATE here while
           creating a device on the adapter, with a runtime-chosen buffer
           size. Our mesa d3d10umd-based UMD never reads this blob (the
           KMD<->UMD contract is the escape ABI, negotiated via
           WinMaliEscapeOp_OpenDevice) - but the QUERY must succeed for
           any size or device creation dies before the UMD even loads.
           Zero-filled = "no private data". */
        if (pQueryAdapterInfo->pOutputData == NULL ||
            pQueryAdapterInfo->OutputDataSize == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(pQueryAdapterInfo->pOutputData,
                      pQueryAdapterInfo->OutputDataSize);
        DbgPrint("[WinMali]    QAI[UMDRIVERPRIVATE] OK size=%u (zero blob)\n",
                 (unsigned)pQueryAdapterInfo->OutputDataSize);
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS* caps  = (DXGK_DRIVERCAPS*)pQueryAdapterInfo->pOutputData;
        SIZE_T           outSz = pQueryAdapterInfo->OutputDataSize;
        SIZE_T           lim   = (outSz < sizeof(*caps)) ? outSz : sizeof(*caps);

        if (caps == NULL || outSz == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(caps, outSz);

#define WM_SET(field, val)                                                       \
        do {                                                                     \
            if (FIELD_OFFSET(DXGK_DRIVERCAPS, field) + sizeof((caps)->field) <= lim) { \
                (caps)->field = (val);                                           \
            }                                                                    \
        } while (0)

        /* Win11 24H2 dxgkrnl refuses to register a new third-party
           adapter declaring WDDM 2.5 (event 549, AddAdapterFailed,
           STATUS_REVISION_MISMATCH). Must match init.Version in
           WinMaliDxgkInitFill.c (WDDM 3.2 on the 26100 WDK) and the
           WDDMDEVICECAPS echo below. */
        WM_SET(WDDMVersion, DXGKDDI_WDDMv3_2);
        if (FIELD_OFFSET(DXGK_DRIVERCAPS, HighestAcceptableAddress) +
            sizeof(caps->HighestAcceptableAddress) <= lim) {
            caps->HighestAcceptableAddress.QuadPart = (ULONG64)-1;
        }
        WM_SET(MaxAllocationListSlotId, 7);
        WM_SET(ApertureSegmentCommitLimit, 0);
        WM_SET(SupportNonVGA, TRUE);
        /*
         * Hybrid role. WDDM 1.3 has an explicit declaration for "this GPU
         * renders but owns no display, and another adapter does the scanout":
         * DXGK_DRIVERCAPS.HybridDiscrete (surfaced to user mode as
         * D3DKMT_WDDM_1_3_CAPS.IsHybridDiscreteGPU). That is structurally what
         * we are -- Mali-G615 has no display engine at all; the panel is driven
         * by the MediaTek DISP block / EDK2 framebuffer.
         *
         * Neither field was ever set, so with ComputeOnly=0 dxgk had no way to
         * know we are a render-only half of a pair, and (measured) rejected the
         * adapter without asking anything further.
         *
         * Knob: 0 = declare neither (previous behaviour), 1 = HybridDiscrete,
         * 2 = HybridIntegrated. One binary, whole matrix.
         */
        if (g_WinMaliHybridRole == 1) {
            WM_SET(HybridDiscrete, TRUE);
        } else if (g_WinMaliHybridRole == 2) {
            WM_SET(HybridIntegrated, TRUE);
        }
        WM_SET(SupportSmoothRotation, TRUE);
        WM_SET(SupportPerEngineTDR, 1);
        WM_SET(SupportRuntimePowerManagement, FALSE);

        if (FIELD_OFFSET(DXGK_DRIVERCAPS, PresentationCaps) +
            sizeof(caps->PresentationCaps) <= lim) {
            caps->PresentationCaps.SupportSoftwareDeviceBitmaps = TRUE;
            /* ⛔★★★★★★ THESE TWO REFUSED THE EXACT PRESENT LOGONUI NEEDS.
             *
             *   NoScreenToScreenBlt - "Driver does not support kernel mode
             *                          Present WITHIN THE SAME ALLOCATION"
             *   NoOverlapScreenBlt  - "...within the same allocation with
             *                          OVERLAPPED SrcRect and DstRect"
             *
             * MEASURED 2026-08-17 (KMD 1226 published the full present geometry):
             *     SrcRect = (1434,1405)..(1510,1484)
             *     DstRect = (1434,1405)..(1510,1484)   <- IDENTICAL
             *     SubRectCnt=1, no stretch
             * a 76x79 SELF-BLT - the logon spinner refreshing one screen region.
             * Same allocation, maximally overlapped rects: precisely what both
             * caps declare we cannot do.
             *
             * So dxgk cannot route the spinner to us as a blt. It falls back to a
             * RedirectedFlip (ETW id=184, Flags=0x8000 = RedirectedFlip), which
             * it then REJECTS with STATUS_INVALID_PARAMETER for want of a
             * destination - and LogonUI spins ~2,350 dxgk calls/s on the retry,
             * so LOGON NEVER COMPLETES. Disarming the driver releases it, every
             * time (user-observed 3x).
             *
             * We can genuinely service this: the present blt executor is a CPU
             * RtlCopyMemory (WinMaliPresentExecuteBlt_), so a copy inside one
             * allocation is ordinary work - and with Src==Dst it is a no-op.
             * ⚠ For PARTIALLY overlapping rects a forward row copy can smear;
             * identical rects cannot. If a real overlapping case shows up,
             * ExecuteBlt must choose its copy direction.
             * ⚠ This CLAIMS a capability. If the blt path cannot honour it we get
             * corruption instead of a stall - a different failure - so judge this
             * on the PANEL as well as the counters. */
            caps->PresentationCaps.NoScreenToScreenBlt          = FALSE;
            caps->PresentationCaps.NoOverlapScreenBlt           = FALSE;
            caps->PresentationCaps.MaxTextureWidthShift         = 3;
            caps->PresentationCaps.MaxTextureHeightShift        = 3;
        }
        if (FIELD_OFFSET(DXGK_DRIVERCAPS, FlipCaps) +
            sizeof(caps->FlipCaps) <= lim) {
            /*
             * ★★★★★★★ dxgk SAYS WHAT IT WANTS HERE, IN WORDS.
             *
             * ETW (Microsoft-Windows-DxgKrnl, event id 494) immediately before
             * StartAdapter_AddAdapterFailed / STATUS_INVALID_PARAMETER:
             *
             *   "Driver reports WDDM version 2.0 or higher but does not support
             *    FlipOnVSyncMmIo cap. The caps value is 0x8"
             *   "Failed to create ADAPTER_RENDER on adapter 0xffff9c8d92edd000"
             *
             * So FlipOnVSyncMmIo (bit1, 0x2) is MANDATORY for any WDDM >= 2.0
             * adapter -- it is required even to create the RENDER adapter. The
             * MCDM path escaped it because compute-only adapters are exempt,
             * which is exactly why ComputeOnly=1 has started cleanly for days.
             *
             * The old comment here ("render-only has no scanout, must not claim
             * a flip capability") was reasonable and wrong: dxgk does not treat
             * this as optional.
             *
             * ⚠ This is a real promise: dxgk will call DxgkDdiSetVidPnSourceAddress
             * at the vsync interrupt to move the scanout base. v1 scans out of the
             * framebuffer EDK2 already lit, so honouring it means recording the
             * address; it must NOT claim FlipOnVSyncWithNoWait/FlipInterval/
             * FlipIndependent, which we genuinely cannot do.
             *
             * MCDM keeps 0 -- do not disturb the configuration that works.
             */
            caps->FlipCaps.Value = 0;
            if (g_WinMaliComputeOnly == 0) {
                caps->FlipCaps.FlipOnVSyncMmIo = 1;

                /* ★★★★★ THE FLIP LogonUI ASKS FOR IS **IMMEDIATE**, AND WE HAVE
                 * NEVER CLAIMED WE COULD DO ONE.
                 *
                 * MEASURED 2026-08-17 (UMD_1232, armed, stalled at logon). ETW
                 * id=184 decoded against the provider manifest - its fields are
                 *   hContext, hWindow, VidPnSourceId, FlipInterval, Flags,
                 *   ReturnStatus, hSrcAllocHandle, hDstAllocHandle
                 * and EVERY LogonUI present reads:
                 *   FlipInterval = 0        (D3DDDI_FLIPINTERVAL_IMMEDIATE)
                 *   Flags        = 0x8000   (RedirectedFlip)
                 *   ReturnStatus = 0xC000000D  STATUS_INVALID_PARAMETER
                 * 9 in 4 s, alternating across BOTH buffers of a two-buffer
                 * swapchain, while DxgkDdiSetVidPnSourceAddress was called just
                 * TWICE in 287 s. The flips are refused before reaching us.
                 *
                 * d3dkmddi.h: FlipImmediateMmIo = "Support Flip as mmio
                 * immediate". Without it dxgk has no route to service a
                 * FLIPINTERVAL_IMMEDIATE request at all.
                 *
                 * ⚠ IS THIS HONEST? More honest than what we already claim.
                 * Our "flip" records an address and a worker memcpys pixels into
                 * the firmware framebuffer; there is NO vsync synchronisation
                 * anywhere in it (the vsync we report is a KTIMER, not a scanout
                 * signal). "Immediate" describes that better than "at the vsync
                 * interrupt" does.
                 *
                 * ⚠ Corroborated by our own history: the id=494 text that forced
                 * FlipOnVSyncMmIo on read "does not support FlipOnVSyncMmIo cap.
                 * The caps value is 0x8" - and 0x8 IS this bit. It was once set
                 * ALONE and dxgk demanded bit1 as well. The answer was always
                 * BOTH: 0x2 | 0x8 = 0xA.
                 *
                 * Still NOT claimed, because we genuinely cannot do them:
                 * FlipOnVSyncWithNoWait, FlipInterval (TWO/THREE/FOUR),
                 * FlipIndependent, DdiPresentForIFlip. */
                caps->FlipCaps.FlipImmediateMmIo = 1;
            }
            /* Publish what we ACTUALLY sent, plus the offset we wrote it at.
               dxgk reported reading 0x8 from a field this code sets to 0, and
               that discrepancy is not something to resolve by reasoning. */
            g_WinMaliQaiFlipCapsValue = (ULONG)caps->FlipCaps.Value;
            g_WinMaliQaiFlipCapsOff   = (ULONG)FIELD_OFFSET(DXGK_DRIVERCAPS, FlipCaps);
            g_WinMaliQaiOutSize       = (ULONG)pQueryAdapterInfo->OutputDataSize;
            g_WinMaliQaiCapsSizeof    = (ULONG)sizeof(DXGK_DRIVERCAPS);
            DbgPrint("[WinMali]    QAI[DRIVERCAPS] FlipCaps=0x%08x @off=%u outsz=%u sizeof(caps)=%u\n",
                     (unsigned)caps->FlipCaps.Value,
                     (unsigned)FIELD_OFFSET(DXGK_DRIVERCAPS, FlipCaps),
                     (unsigned)pQueryAdapterInfo->OutputDataSize,
                     (unsigned)sizeof(DXGK_DRIVERCAPS));
        }
        if (FIELD_OFFSET(DXGK_DRIVERCAPS, SchedulingCaps) +
            sizeof(caps->SchedulingCaps) <= lim) {
            /* WDDM 3.2 / GpuMmu REQUIRES these scheduling caps - dxgkrnl
               rejects the adapter at start (queries DRIVERCAPS then tears the
               device down) if they're cleared. They are NOT optional, so we
               must actually IMPLEMENT the DDIs behind them: DxgkDdiPreempt
               Command + DXGK_INTERRUPT_DMA_PREEMPTED (async submit scheduler,
               WinMaliDdi.c) and DxgkDdiCancelCommand. Advertising them while
               stubbing the DDIs is what bugchecked dxgmms2 (0xD1 in the
               preemption path); the fix is the real implementation, not
               clearing the caps. */
            caps->SchedulingCaps.MultiEngineAware       = 1;
            caps->SchedulingCaps.VSyncPowerSaveAware    = 1;
            caps->SchedulingCaps.PreemptionAware        = 1;   /* WDDM 1.2+ */
            caps->SchedulingCaps.NoDmaPatching          = 1;   /* WDDM 1.2+ */
            caps->SchedulingCaps.CancelCommandAware     = 1;
            caps->SchedulingCaps.LowIrqlPreemptCommand  = 1;   /* WDDM 2.0+ */
            caps->SchedulingCaps.No64BitAtomics         = 1;   /* WDDM 2.0+ */
        }
        if (FIELD_OFFSET(DXGK_DRIVERCAPS, MemoryManagementCaps) +
            sizeof(caps->MemoryManagementCaps) <= lim) {
            caps->MemoryManagementCaps.CrossAdapterResource       = 1;
            caps->MemoryManagementCaps.CrossAdapterResourceTexture = 1;
            caps->MemoryManagementCaps.CrossAdapterResourceScanout = 1;
            caps->MemoryManagementCaps.SectionBackedPrimary       = 0;
            caps->MemoryManagementCaps.VirtualAddressingSupported = 1;
            caps->MemoryManagementCaps.GpuMmuSupported            = 1;
            caps->MemoryManagementCaps.OutOfOrderLock             = 1;
            caps->MemoryManagementCaps.NonCpuVisiblePrimary       = 1; 
            caps->MemoryManagementCaps.PagingNode                 = 1;  /* copy node 1 (matches PHYSICALADAPTERCAPS.PagingNodeIndex) */
            caps->MemoryManagementCaps.IoMmuSupported             = 0;
        }
        if (FIELD_OFFSET(DXGK_DRIVERCAPS, PreemptionCaps) +
            sizeof(caps->PreemptionCaps) <= lim) {
            /* DMA-BUFFER-boundary only: we cannot preempt mid-buffer (no
               hardware for primitive/dispatch-boundary save-restore). We run
               each DMA buffer to completion and honor a preempt request by
               reporting DMA_PREEMPTED at the next buffer boundary (see the
               async submit worker + DxgkDdiPreemptCommand). Claiming the finer
               PRIMITIVE/DISPATCH granularity here made the scheduler set up
               mid-buffer preemption state we never satisfied -> 0xD1 in
               dxgmms2's preemption path. This is the honest, implementable
               level. */
            caps->PreemptionCaps.GraphicsPreemptionGranularity =
                D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
            caps->PreemptionCaps.ComputePreemptionGranularity  =
                D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        }

        /* GPU VA span. Must enclose the per-process VA we accept in
           SetRootPageTable. Aligned with GPUMMUCAPS.VirtualAddressBitCount
           and PAGETABLELEVELDESC (4 levels x 9 idx bits + 12 page = 48 bits) -
           the native Mali LPAE 48-bit space. panfrost/mesa address the full
           range (tiler at 0x40000000, per-context BOs top-down near 2^48), so
           a smaller span makes MapGpuVirtualAddress reject every real VA. */
        WM_SET(InternalGpuVirtualAddressRangeStart,
               (D3DGPU_VIRTUAL_ADDRESS)0x0000000000010000ULL);
        WM_SET(InternalGpuVirtualAddressRangeEnd,
               (D3DGPU_VIRTUAL_ADDRESS)0x0000FFFFFFFFFFFFULL);

        if (FIELD_OFFSET(DXGK_DRIVERCAPS, GpuEngineTopology) +
            sizeof(caps->GpuEngineTopology) <= lim) {
            /* Two nodes (3D + COPY/paging), matching PHYSICALADAPTERCAPS
               NumExecutionNodes=2 and GetNodeMetadata. Reporting 1 here hid the
               copy node dxgk uses for cross-adapter present copies. */
            caps->GpuEngineTopology.NbAsymetricProcessingNodes = 2;
        }
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_4)
        if (FIELD_OFFSET(DXGK_DRIVERCAPS, MiscCaps) +
            sizeof(caps->MiscCaps) <= lim) {
            /* ⛔★★★★★ EXPERIMENT 2026-08-17: SupportContextlessPresent was 1,
             * with no comment justifying it - conspicuous in a file where every
             * other cap carries its reasoning. It is the prime suspect for the
             * ONE rejection that actually blocks DWM's flips.
             *
             * MEASURED (DxgKrnl ETW id=494, grouped BY PID, 10 s):
             *     x49 "There should be at least one broadcast context passed to
             *          signal."   evtPid=1240 = DWM
             * and it CONTINUES after the UMD-side BroadcastContextCount fix,
             * because DWM does not present through our UMD at all (it called
             * _Present ONCE; Explorer called it 193 times). DWM presents via the
             * KERNEL path - so the context contract that matters is the one WE
             * declare here, not anything the UMD does.
             *
             * This bit tells dxgk "presents need no context". If dxgk takes us up
             * on it for DWM's flips, the present arrives contextless, and
             * whatever must be SIGNALLED afterwards has no context to broadcast
             * to - which is that message, verbatim.
             *
             * ✅🏆 RESULT: KEEP IT AT 0. **THE PANEL SHOWED A FULL DESKTOP WITH A
             * TASKBAR** on this build (user-observed, 2026-08-17) - the taskbar
             * had NEVER rendered before in this whole effort ("full desktop, no
             * taskbar, no cursor" was the standing signature).
             *
             * ⛔★★★★★ AND THE COUNTER I CHOSE MISSED IT. I declared this bit
             * "innocent" because dwm's id=494 broadcast count did not move
             * (39 vs 49, inside run-to-run noise) and had already written the
             * restore-to-1 edit. The ETW message count was simply not measuring
             * what changed; the PANEL was. **When the deliverable is pixels, the
             * panel is the oracle** - a log-message rate is a proxy, and a proxy
             * that disagrees with the artifact is the proxy being wrong.
             *
             * ⚠ Still unexplained and worth understanding rather than cargo-
             * culting: the broadcast rejections continue at the same rate, so
             * whatever this bit changed, it was not that. Do not assume the two
             * are related. */
            caps->MiscCaps.SupportContextlessPresent = 0;
            caps->MiscCaps.Detachable                = 0;  /* not eGPU */

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_6)
            /*
             * ★★ MT6897 FIX: declare this adapter COMPUTE-ONLY (MCDM).
             *
             * Leaving every display-family DDI NULL makes the adapter *look* render-only in
             * the DDI table, but it does not TELL dxgk that it has no display. Without this
             * bit dxgk still treats the adapter as display-capable and tries to construct a
             * display core for it: on this machine it queried nine display interfaces in a
             * row via DxgkDdiQueryInterface -- I2C, I2C_V2, BRIGHTNESS_2, BRIGHTNESS_3, OPM,
             * OPM_2_JTP, and the display-core interface twice (v2 then a v1 fallback) -- the
             * driver refused all of them, and dxgk then failed AddAdapter with
             * STATUS_INVALID_PARAMETER (DxgKrnl-Admin 549, StartAdapter_AddAdapterFailed)
             * AFTER a fully successful StartDevice and caps sweep.
             *
             * This also explains why the same code works on the RK3588 reference machine:
             * that box is HEADLESS, so there is no display topology for dxgk to pursue. This
             * tablet has a live framebuffer and monitor (Microsoft Basic Display Driver +
             * Generic Monitor), so dxgk follows the display-core path and we must opt out of
             * it explicitly.
             *
             * ComputeOnly is DXGK_DRIVERCAPS.MiscCaps.ComputeOnly, added in WDDM 2.6
             * (d3dkmddi.h:2472). It is the formal MCDM "no display" declaration.
             */
            caps->MiscCaps.ComputeOnly = (g_WinMaliComputeOnly != 0) ? 1 : 0;
#endif
        }
#endif
#undef WM_SET
        WM_QAI_OK("DRIVERCAPS");
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_PHYSICALADAPTERCAPS: {
        DXGK_PHYSICALADAPTERCAPS phys;
        PWINMALI_ADAPTER         adapter;
        SIZE_T                   outSz = pQueryAdapterInfo->OutputDataSize;
        SIZE_T                   copyLen;
        const DXGK_QUERYPHYSICALADAPTERCAPSIN* physIn;

        if (pQueryAdapterInfo->pOutputData == NULL || outSz == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (pQueryAdapterInfo->pInputData != NULL &&
            pQueryAdapterInfo->InputDataSize >= sizeof(DXGK_QUERYPHYSICALADAPTERCAPSIN)) {
            physIn = (const DXGK_QUERYPHYSICALADAPTERCAPSIN*)pQueryAdapterInfo->pInputData;
            if (physIn->PhysicalAdapterIndex != 0) {
                return STATUS_INVALID_PARAMETER;
            }
        }
        adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
        if (adapter == NULL || adapter->DxgkHandle == NULL) {
            return STATUS_DEVICE_NOT_READY;
        }

        RtlZeroMemory(&phys, sizeof(phys));
        /* Two nodes: 0 = 3D render engine, 1 = COPY/paging engine.
           dxgkrnl requires PagingNodeIndex to name a copy/DMA engine (not the
           3D node - =0 bails at StartDevice on Win11 ARM64) AND a node that
           actually exists (=NumExecutionNodes, out of range, materialises a
           PHANTOM paging node whose per-process scheduler state is left NULL
           -> 0xD1 in VidSchiProfilePerformanceTick when it is first
           scheduled). So node 1 is a real declared copy engine; GetNodeMetadata
           reports it, CreateContext accepts it, and its paging buffers are
           serviced CPU-side (BuildPagingBuffer). */
        phys.NumExecutionNodes         = 2;
        phys.PagingNodeIndex           = 1;   /* the real copy node */
        phys.DxgkPhysicalAdapterHandle = adapter->DxgkHandle;
        phys.Flags.Value               = 0;
        phys.Flags.GpuMmuSupported     = 1;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_1)
        phys.VPRPagingNode             = 1;   /* no VPR; keep off the phantom */
#endif

        RtlZeroMemory(pQueryAdapterInfo->pOutputData, outSz);
        copyLen = (outSz < sizeof(phys)) ? outSz : sizeof(phys);
        RtlCopyMemory(pQueryAdapterInfo->pOutputData, &phys, copyLen);
        WM_QAI_OK("PHYSICALADAPTERCAPS");
        return STATUS_SUCCESS;
    }

    /* ------------------------------------------------------------------
       Pre-StartDevice caps. dxgkrnl 26100 issues WDDMDEVICECAPS(29),
       PHYSICAL_MEMORY_CAPS(34), IOMMU_CAPS(35) and
       HARDWARERESERVEDRANGES2(36) between AddDevice and StartDevice.
       PHYSICAL_MEMORY_CAPS is load-bearing: the WDDM 3.x System Memory
       Manager (dxgkrnl!DpiFdoCreateSysMmAdapter) gates adapter creation
       on it, and a zero HighestVisibleAddress means "no addressable
       memory" - dxgk then calls RemoveDevice WITHOUT EVER CALLING
       StartDevice (no 549 event; a related 549 shows {Not Enough
       Quota}). So these cannot ride the zero-fill default at the
       bottom of this switch.
       ------------------------------------------------------------------ */

    case DXGKQAITYPE_WDDMDEVICECAPS: {
        /* Must echo DRIVERCAPS.WDDMVersion exactly (header contract). */
        DXGK_WDDMDEVICECAPS* dc    = (DXGK_WDDMDEVICECAPS*)pQueryAdapterInfo->pOutputData;
        SIZE_T               outSz = pQueryAdapterInfo->OutputDataSize;

        if (dc == NULL || outSz < sizeof(*dc)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(dc, outSz);
        dc->WDDMVersion = DXGKDDI_WDDMv3_2;
        WM_QAI_OK("WDDMDEVICECAPS=WDDMv3_2");
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_PHYSICAL_MEMORY_CAPS: {
        /* Honest PA reach: HighestVisibleAddress = 2^PA_bits - 1, PA
           width from MMU_FEATURES[15:8]. This query lands before
           StartDevice maps MMIO, so the register snapshot may not
           exist yet - fall back to the Mali-G610 architectural value
           (PA = 40 bits) in that case. */
        DXGK_PHYSICAL_MEMORY_CAPS pmc;
        PWINMALI_ADAPTER          adapter;
        ULONG                     paBits = 0;
        SIZE_T                    outSz  = pQueryAdapterInfo->OutputDataSize;
        SIZE_T                    copyLen;

        if (pQueryAdapterInfo->pOutputData == NULL || outSz == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
        if (adapter != NULL) {
            paBits = WINMALI_MMU_FEATURES_PA_BITS(adapter->Hw.MmuFeatures);
        }
        if (paBits == 0 || paBits > 48) {
            paBits = 40;    /* Mali-G610 MMU_FEATURES value; pre-MMIO fallback */
        }

        RtlZeroMemory(&pmc, sizeof(pmc));
        pmc.HighestVisibleAddress.QuadPart = (LONGLONG)((1ULL << paBits) - 1u);

        RtlZeroMemory(pQueryAdapterInfo->pOutputData, outSz);
        copyLen = (outSz < sizeof(pmc)) ? outSz : sizeof(pmc);
        RtlCopyMemory(pQueryAdapterInfo->pOutputData, &pmc, copyLen);
        DbgPrint("[WinMali]    QAI[PHYSICAL_MEMORY_CAPS] OK pa_bits=%lu highest=0x%llx\n",
                 paBits, (unsigned long long)pmc.HighestVisibleAddress.QuadPart);
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_IOMMU_CAPS: {
        /* All-zero is the honest answer: the RK3588's Mali sits on the
           SoC interconnect with its own GPU MMU; there is no Windows-
           managed IOMMU/SMMU in front of it (and we set
           MemoryManagementCaps.IoMmuSupported = 0 to match). */
        DXGK_IOMMU_CAPS iommu;
        SIZE_T          outSz = pQueryAdapterInfo->OutputDataSize;
        SIZE_T          copyLen;

        if (pQueryAdapterInfo->pOutputData == NULL || outSz == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(&iommu, sizeof(iommu));

        RtlZeroMemory(pQueryAdapterInfo->pOutputData, outSz);
        copyLen = (outSz < sizeof(iommu)) ? outSz : sizeof(iommu);
        RtlCopyMemory(pQueryAdapterInfo->pOutputData, &iommu, copyLen);
        WM_QAI_OK("IOMMU_CAPS=none");
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_HARDWARERESERVEDRANGES:
    case DXGKQAITYPE_HARDWARERESERVEDRANGES2: {
        /* No hardware-reserved system RAM: firmware carve-outs on
           RK3588 are handled by UEFI/ACPI before Windows boots, and
           our segment is plain contiguous sysmem we allocate
           ourselves. NumRanges = 0; dxgk ignores pPhysicalRanges when
           the count is zero (don't zero the pointer - dxgk owns it). */
        DXGK_HARDWARERESERVEDRANGES* hrr =
            (DXGK_HARDWARERESERVEDRANGES*)pQueryAdapterInfo->pOutputData;

        if (hrr == NULL ||
            pQueryAdapterInfo->OutputDataSize < sizeof(*hrr)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        hrr->NumRanges = 0;
        WM_QAI_OK("HARDWARERESERVEDRANGES(2)=0");
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_GPUMMUCAPS: {
        DXGK_GPUMMUCAPS               caps;
        SIZE_T                        outSz = pQueryAdapterInfo->OutputDataSize;
        SIZE_T                        copyLen;
        const DXGK_QUERYGPUMMUCAPSIN* in;

        if (pQueryAdapterInfo->pInputData == NULL ||
            pQueryAdapterInfo->InputDataSize < sizeof(DXGK_QUERYGPUMMUCAPSIN)) {
            return STATUS_INVALID_PARAMETER;
        }
        in = (const DXGK_QUERYGPUMMUCAPSIN*)pQueryAdapterInfo->pInputData;
        if (in->PhysicalAdapterIndex != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        if (pQueryAdapterInfo->pOutputData == NULL || outSz == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        RtlZeroMemory(&caps, sizeof(caps));

        caps.PageTableUpdateMode                       = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
        caps.ReadOnlyMemorySupported                   = 1;
        caps.NoExecuteMemorySupported                  = 1;
        caps.ZeroInPteSupported                        = 1;
        caps.VirtualAddressBitCount                    = 48;
        caps.PageTableLevelCount                       = 4;
        caps.ExplicitPageTableInvalidation             = 1;   /* we drive MMU AS_TLB invalidate registers */
        caps.CacheCoherentMemorySupported              = 1;
        caps.PageTableUpdateRequireAddressSpaceIdle    = 0;   /* live PT updates OK */
        caps.DualPteSupported                          = 0;   /* Mali LPAE has no dual-PT */
        caps.LeafPageTableSizeFor64KPagesInBytes       = 0;   /* no 64K pages */

        RtlZeroMemory(pQueryAdapterInfo->pOutputData, outSz);
        copyLen = (outSz < sizeof(caps)) ? outSz : sizeof(caps);
        RtlCopyMemory(pQueryAdapterInfo->pOutputData, &caps, copyLen);
        WM_QAI_OK("GPUMMUCAPS");
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_PAGETABLELEVELDESC: {
        const DXGK_QUERYPAGETABLELEVELDESCIN* in;
        DXGK_PAGE_TABLE_LEVEL_DESC            desc;
        SIZE_T                                outSz = pQueryAdapterInfo->OutputDataSize;
        SIZE_T                                copyLen;
        const UINT                            kLevels  = 4;
        const UINT                            kIdxBits = 9;

        if (pQueryAdapterInfo->pInputData == NULL ||
            pQueryAdapterInfo->InputDataSize < sizeof(DXGK_QUERYPAGETABLELEVELDESCIN)) {
            return STATUS_INVALID_PARAMETER;
        }
        in = (const DXGK_QUERYPAGETABLELEVELDESCIN*)pQueryAdapterInfo->pInputData;
        if (in->PhysicalAdapterIndex != 0 || in->LevelIndex >= kLevels) {
            return STATUS_INVALID_PARAMETER;
        }
        if (pQueryAdapterInfo->pOutputData == NULL || outSz == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        RtlZeroMemory(&desc, sizeof(desc));
        desc.PageTableIndexBitCount             = kIdxBits;
        desc.PageTableSegmentId                 = WINMALI_SEGMENT_ID_SYSMEM;
        desc.PagingProcessPageTableSegmentId    = WINMALI_SEGMENT_ID_SYSMEM;
        desc.PageTableSizeInBytes               = (UINT)PAGE_SIZE;
        /*
         * ⛔ MUST NOT BE ZERO. This was 0 with the comment "use segment page
         * size" -- an ASSUMPTION that 0 means "default", not a documented
         * contract. dxgmms2 divides by the alignment when placing page tables,
         * so a zero here is a divide-by-zero in the video memory manager:
         *   0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED in dxgmms2.sys,
         *   P1 = 0xC0000094 STATUS_INTEGER_DIVIDE_BY_ZERO.
         * It only bites once REAL RENDERING allocates and maps buffers, which
         * is why it survived every earlier compute/display test and appeared
         * the moment draws started producing 504-byte command streams. The
         * zero-size ALLOCATION clamp in WinMaliAlloc.c does not cover this --
         * different field, different path.
         * Page tables are page-aligned; say so explicitly.
         */
        desc.PageTableAlignmentInBytes          = (UINT)PAGE_SIZE;

        RtlZeroMemory(pQueryAdapterInfo->pOutputData, outSz);
        copyLen = (outSz < sizeof(desc)) ? outSz : sizeof(desc);
        RtlCopyMemory(pQueryAdapterInfo->pOutputData, &desc, copyLen);
        DbgPrint("[WinMali]    QAI[PAGETABLELEVELDESC level=%u] OK\n", in->LevelIndex);
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_QUERYSEGMENT3: {
        /* QUERYSEGMENT* is a TWO-PASS call:
             pass 1 (probe): pSegmentDescriptor == NULL. We set NbSegment +
                             PagingBuffer* fields and return SUCCESS. dxgk
                             then allocates space for descriptors.
             pass 2 (fill):  pSegmentDescriptor != NULL. We write the
                             descriptor array.
           Returning STATUS_INVALID_PARAMETER on a probe call trips a dxgk
           NT_ASSERT in chk builds (and silently corrupts state in fre). */
        DXGK_QUERYSEGMENTOUT3*   qo;
        DXGK_SEGMENTDESCRIPTOR3* segs;
        PWINMALI_ADAPTER         adapter;
        const UINT               kNumSegs = WINMALI_SEGMENT_COUNT;

        adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
        if (adapter == NULL) {
            return STATUS_DEVICE_NOT_READY;
        }
        if (adapter->DmaSegmentVa == NULL || adapter->DmaSegmentBytes == 0) {
            WINMALI_WARN("QUERYSEGMENT3: sysmem segment not allocated");
            return STATUS_DEVICE_NOT_READY;
        }
        if (pQueryAdapterInfo->pOutputData == NULL ||
            pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT3)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        qo = (DXGK_QUERYSEGMENTOUT3*)pQueryAdapterInfo->pOutputData;
        qo->NbSegment                   = kNumSegs;

        qo->PagingBufferSegmentId       = WINMALI_SEGMENT_ID_SYSMEM;
        qo->PagingBufferSize            = 64 * 1024;
        qo->PagingBufferPrivateDataSize = 0;

        segs = qo->pSegmentDescriptor;
        if (segs != NULL) {
            RtlZeroMemory(segs, sizeof(DXGK_SEGMENTDESCRIPTOR3) * kNumSegs);
            segs[0].Flags.Value                     = 0;
            segs[0].Flags.CacheCoherent             = 1;
            segs[0].Flags.CpuVisible                = 1;
            segs[0].Flags.ApplicationTarget         = 1;
            segs[0].Flags.NonLocalBudgetGroup       = 1;
            segs[0].Flags.LocalBudgetGroup          = 0;
            segs[0].BaseAddress.QuadPart            = WINMALI_SYSMEM_GPU_BASE;
            segs[0].CpuTranslatedAddress.QuadPart   = 0;   /* Aperture: dxgk pins pages on demand */

            /* ★★★★★ DirectFlip IS A **PER-SEGMENT** PERMISSION, AND WE HAVE
             * ALWAYS SENT IT AS 0.
             *
             * DXGK_SEGMENTFLAGS.DirectFlip (0x400, "supports direct flip") says
             * whether an allocation living in THIS segment is allowed to be a
             * flip target. We have exactly one segment and shipped it without
             * that bit, i.e. we told dxgk nothing we host may ever be flipped
             * to - and dxgk then refused LogonUI's RedirectedFlip with
             * STATUS_INVALID_PARAMETER, which is the correct response to that
             * declaration. Measured 9/9 rejected here vs 200/200 accepted for the
             * stock driver on the same box with the identical present shape.
             * See Parameters\SegmentShape for the full matrix. */
            if (g_WinMaliSegmentShape >= 1) {
                segs[0].Flags.DirectFlip                = 1;
                segs[0].Flags.PopulatedFromSystemMemory = 1;
            }
            if (g_WinMaliSegmentShape >= 2) {
                /* Describe it as what it actually is: contiguous system memory
                   whose physical base we already know and use for the root page
                   table. Not an aperture. */
                segs[0].Flags.Aperture                = 0;
                segs[0].CpuTranslatedAddress.QuadPart = adapter->DmaSegmentPhys.QuadPart;
            } else {
                segs[0].Flags.Aperture                = 1;
            }

            segs[0].Size                            = adapter->DmaSegmentBytes;
            segs[0].NbOfBanks                       = 0;
            segs[0].pBankRangeTable                 = NULL;
            segs[0].CommitLimit                     = adapter->DmaSegmentBytes;
            segs[0].SystemMemoryEndAddress          = 0;
            g_WinMaliQaiSegFlags = (ULONG)segs[0].Flags.Value;
            DbgPrint("[WinMali]    QAI[QUERYSEGMENT3 fill] OK (shape=%lu flags=0x%08x, %lu MiB)\n",
                     g_WinMaliSegmentShape, (unsigned)segs[0].Flags.Value,
                     (ULONG)(adapter->DmaSegmentBytes >> 20));
        } else {
            DbgPrint("[WinMali]    QAI[QUERYSEGMENT3 probe] NbSegment=%u\n", kNumSegs);
        }
        return STATUS_SUCCESS;
    }

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
    case DXGKQAITYPE_QUERYSEGMENT4: {
        /* Same two-pass dance as QUERYSEGMENT3 - probe first, fill second. */
        DXGK_QUERYSEGMENTOUT4*   qo;
        DXGK_SEGMENTDESCRIPTOR4* segs;
        PWINMALI_ADAPTER         adapter;
        const UINT               kNumSegs = WINMALI_SEGMENT_COUNT;

        adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
        if (adapter == NULL || adapter->DmaSegmentVa == NULL) {
            return STATUS_DEVICE_NOT_READY;
        }
        if (pQueryAdapterInfo->pInputData != NULL &&
            pQueryAdapterInfo->InputDataSize >= sizeof(DXGK_QUERYSEGMENTIN4)) {
            const DXGK_QUERYSEGMENTIN4* sin =
                (const DXGK_QUERYSEGMENTIN4*)pQueryAdapterInfo->pInputData;
            if (sin->PhysicalAdapterIndex != 0) {
                return STATUS_INVALID_PARAMETER;
            }
        }
        if (pQueryAdapterInfo->pOutputData == NULL ||
            pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT4)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        qo = (DXGK_QUERYSEGMENTOUT4*)pQueryAdapterInfo->pOutputData;
        qo->NbSegment                   = kNumSegs;
        qo->PagingBufferSegmentId       = WINMALI_SEGMENT_ID_SYSMEM;
        qo->PagingBufferSize            = 64 * 1024;
        qo->PagingBufferPrivateDataSize = 0;

        segs = (DXGK_SEGMENTDESCRIPTOR4*)qo->pSegmentDescriptor;  /* BYTE* in header */
        if (segs != NULL) {
            RtlZeroMemory(segs, sizeof(DXGK_SEGMENTDESCRIPTOR4) * kNumSegs);
            segs[0].Flags.Value                     = 0;
            segs[0].Flags.CacheCoherent             = 1;
            segs[0].Flags.CpuVisible                = 1;
            segs[0].Flags.ApplicationTarget         = 1;
            segs[0].Flags.NonLocalBudgetGroup       = 1;
            segs[0].Flags.LocalBudgetGroup          = 0;
            segs[0].BaseAddress.QuadPart            = WINMALI_SYSMEM_GPU_BASE;
            segs[0].CpuTranslatedAddress.QuadPart   = 0;

            /* ★ Must MATCH QUERYSEGMENT3 exactly - dxgk reads whichever it
               prefers, and a driver that describes the same segment two
               different ways is worse than one that gets it wrong once.
               See the long comment in the QUERYSEGMENT3 case above. */
            if (g_WinMaliSegmentShape >= 1) {
                segs[0].Flags.DirectFlip                = 1;
                segs[0].Flags.PopulatedFromSystemMemory = 1;
            }
            if (g_WinMaliSegmentShape >= 2) {
                segs[0].Flags.Aperture                = 0;
                segs[0].CpuTranslatedAddress.QuadPart = adapter->DmaSegmentPhys.QuadPart;
            } else {
                segs[0].Flags.Aperture                = 1;
            }

            segs[0].Size                            = adapter->DmaSegmentBytes;
            segs[0].CommitLimit                     = adapter->DmaSegmentBytes;
            segs[0].SystemMemoryEndAddress          = 0;
            g_WinMaliQaiSegFlags = (ULONG)segs[0].Flags.Value;
            segs[0].NumInvalidMemoryRanges          = 0;
            DbgPrint("[WinMali]    QAI[QUERYSEGMENT4 fill] OK (shape=%lu flags=0x%08x, %lu MiB)\n",
                     g_WinMaliSegmentShape, (unsigned)segs[0].Flags.Value,
                     (ULONG)(adapter->DmaSegmentBytes >> 20));
        } else {
            DbgPrint("[WinMali]    QAI[QUERYSEGMENT4 probe] NbSegment=%u\n", kNumSegs);
        }
        return STATUS_SUCCESS;
    }
#endif

    case DXGKQAITYPE_HISTORYBUFFERPRECISION: {
        /* Report 64-bit fence precision so dxgk uses our wider monotonic
           counters and doesn't truncate to 32 bits. */
        UINT* prec = (UINT*)pQueryAdapterInfo->pOutputData;
        if (prec == NULL || pQueryAdapterInfo->OutputDataSize < sizeof(*prec)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *prec = 64;
        WM_QAI_OK("HISTORYBUFFERPRECISION=64");
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        /*
         * ★★★ PRIME SUSPECT for StartAdapter_DpiFdoEnumChildDevicesFailed.
         *
         * This used to unconditionally return STATUS_NOT_SUPPORTED with the
         * comment "render-only - no display extension". That comment stopped
         * being true the moment ComputeOnly=0 makes us claim to BE a display,
         * and STATUS_NOT_SUPPORTED is 0xC00000BB -- byte-for-byte the status
         * dxgk reports when it gives up on our adapter.
         *
         * It fits every measured fact: the query is display-only (so the
         * working MCDM path never asks it and cannot regress), it is a
         * QueryAdapterInfo type rather than a child DDI (which is exactly why
         * QueryChildStatus and QueryDeviceDescriptor are never reached), and
         * the status matches exactly.
         *
         * "I support none of these" is SUCCESS with a zeroed cap block, not a
         * refusal to answer. We genuinely support none of them: no secure
         * display, no virtual modes, no HDR scanout, no USB4 targets.
         */
        if (g_WinMaliComputeOnly != 0) {
            /* Keep the known-good MCDM path byte-identical. */
            return STATUS_NOT_SUPPORTED;
        }
        if (pQueryAdapterInfo->pOutputData == NULL ||
            pQueryAdapterInfo->OutputDataSize == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(pQueryAdapterInfo->pOutputData,
                      pQueryAdapterInfo->OutputDataSize);
        DbgPrint("[WinMali]    QAI DISPLAY_DRIVERCAPS_EXTENSION size=%u -> SUCCESS (no caps claimed)\n",
                 (unsigned)pQueryAdapterInfo->OutputDataSize);
        return STATUS_SUCCESS;

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_4)
    case DXGKQAITYPE_ADAPTERPERFDATA_CAPS: {
        /* dxgk asks this to learn power/thermal monitoring limits. We don't
           expose perf counters from the Mali MMIO yet; report zeros for
           bandwidth/fan and reasonable thermal headroom (TemperatureMax 105C,
           Warning 95C in deci-Celsius units, 1 = 0.1C). */
        DXGK_ADAPTER_PERFDATACAPS caps;
        SIZE_T outSz = pQueryAdapterInfo->OutputDataSize;
        SIZE_T copyLen;

        if (pQueryAdapterInfo->pOutputData == NULL || outSz == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(&caps, sizeof(caps));
        caps.MaxMemoryBandwidth = 0;
        caps.MaxPCIEBandwidth   = 0;     /* SoC integrated - no PCI-E */
        caps.MaxFanRPM          = 0;     /* no fan / not exposed */
        caps.TemperatureMax     = 1050;  /* 105.0 C */
        caps.TemperatureWarning = 950;   /* 95.0 C */

        RtlZeroMemory(pQueryAdapterInfo->pOutputData, outSz);
        copyLen = (outSz < sizeof(caps)) ? outSz : sizeof(caps);
        RtlCopyMemory(pQueryAdapterInfo->pOutputData, &caps, copyLen);
        WM_QAI_OK("ADAPTERPERFDATA_CAPS");
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_NODEPERFDATA:
    case DXGKQAITYPE_ADAPTERPERFDATA: {
        /* Task Manager / dxgk telemetry polls these roughly once per
           second per node for as long as the adapter is up - seeing them
           loop in kd means the adapter STARTED and is being monitored.
           We don't read clocks/thermals from the Mali MMIO yet, so
           zero-fill (renders as 0 MHz / no sensor). Deliberately silent:
           a log line here floods the kd ring at steady state. */
        if (pQueryAdapterInfo->pOutputData == NULL ||
            pQueryAdapterInfo->OutputDataSize == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(pQueryAdapterInfo->pOutputData,
                      pQueryAdapterInfo->OutputDataSize);
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_GPUVERSION: {
        DXGK_GPUVERSION ver;
        SIZE_T outSz = pQueryAdapterInfo->OutputDataSize;
        SIZE_T copyLen;

        if (pQueryAdapterInfo->pOutputData == NULL || outSz == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(&ver, sizeof(ver));
        (VOID)RtlStringCbCopyW(ver.BiosVersion,
                               sizeof(ver.BiosVersion),
                               L"Mali CSF FW 1.1.0");
        (VOID)RtlStringCbCopyW(ver.GpuArchitecture,
                               sizeof(ver.GpuArchitecture),
                               L"Mali-G610 Valhall");
        RtlZeroMemory(pQueryAdapterInfo->pOutputData, outSz);
        copyLen = (outSz < sizeof(ver)) ? outSz : sizeof(ver);
        RtlCopyMemory(pQueryAdapterInfo->pOutputData, &ver, copyLen);
        WM_QAI_OK("GPUVERSION");
        return STATUS_SUCCESS;
    }
#endif // WDDM 2.4+

    default:
        /* For *small* (<= 64 byte) unknown query types - typically future
           WDDM extensions probing for capabilities - return SUCCESS with a
           zero-filled output. NOT_IMPLEMENTED can be interpreted by newer
           dxgkrnl as "driver is broken"; SUCCESS-with-zeros means "I know
           the query but I don't claim that capability". We've seen this
           pattern matter for QAI[47] on Win11 26100 ARM64.
           For large unknown structs we still NOT_IMPLEMENTED to avoid
           false claims about complex caps. */
        if (pQueryAdapterInfo->pOutputData != NULL &&
            pQueryAdapterInfo->OutputDataSize > 0 &&
            pQueryAdapterInfo->OutputDataSize <= 64) {
            RtlZeroMemory(pQueryAdapterInfo->pOutputData,
                          pQueryAdapterInfo->OutputDataSize);
            DbgPrint("[WinMali]    QAI[%u] UNKNOWN size=%u -> SUCCESS (zero-filled)\n",
                     (unsigned)pQueryAdapterInfo->Type,
                     (unsigned)pQueryAdapterInfo->OutputDataSize);
            return STATUS_SUCCESS;
        }
        DbgPrint("[WinMali]    QAI[%u] UNHANDLED size=%u -> NOT_IMPLEMENTED\n",
                 (unsigned)pQueryAdapterInfo->Type,
                 (unsigned)pQueryAdapterInfo->OutputDataSize);
        return STATUS_NOT_IMPLEMENTED;
    }
}

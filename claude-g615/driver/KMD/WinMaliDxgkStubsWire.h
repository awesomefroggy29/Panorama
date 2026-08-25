/* paste-in body for WinMaliDxgkPatchInitializationData() */

    init->DxgkDdiAddDevice                                        = WinMaliKmdAddDevice;
    init->DxgkDdiStartDevice                                      = WinMaliKmdStartDevice;
    init->DxgkDdiStopDevice                                       = WinMaliKmdStopDevice;
    init->DxgkDdiRemoveDevice                                     = WinMaliKmdRemoveDevice;
    init->DxgkDdiDispatchIoRequest                                = WinMaliKmdDispatchIoRequest;
    init->DxgkDdiInterruptRoutine                                 = WinMaliKmdInterruptRoutine;
    init->DxgkDdiDpcRoutine                                       = WinMaliKmdDpcRoutine;
    init->DxgkDdiQueryChildRelations                              = WinMaliKmdQueryChildRelations;
    init->DxgkDdiQueryChildStatus                                 = WinMaliKmdQueryChildStatus;
    init->DxgkDdiQueryDeviceDescriptor                            = WinMaliKmdQueryDeviceDescriptor;
    init->DxgkDdiSetPowerState                                    = WinMaliKmdSetPowerState;
    init->DxgkDdiNotifyAcpiEvent                                  = WinMaliKmdNotifyAcpiEvent;
    init->DxgkDdiResetDevice                                      = WinMaliKmdResetDevice;
    init->DxgkDdiUnload                                           = WinMaliKmdDxgkUnload;
    init->DxgkDdiQueryInterface                                   = WinMaliKmdQueryInterface;
    init->DxgkDdiControlEtwLogging                                = WinMaliKmdControlEtwLogging;
    init->DxgkDdiQueryAdapterInfo                                 = WinMaliKmdQueryAdapterInfo;
    init->DxgkDdiCreateDevice                                     = WinMaliKmdCreateDevice;
    init->DxgkDdiCreateAllocation                                 = WinMaliKmdCreateAllocation;
    init->DxgkDdiDestroyAllocation                                = WinMaliKmdDestroyAllocation;
    init->DxgkDdiDescribeAllocation                               = WinMaliKmdDescribeAllocation;
    init->DxgkDdiGetStandardAllocationDriverData                  = WinMaliKmdGetStandardAllocationDriverData;
    init->DxgkDdiAcquireSwizzlingRange                            = WinMaliKmdStub_AcquireSwizzlingRange;
    init->DxgkDdiReleaseSwizzlingRange                            = WinMaliKmdStub_ReleaseSwizzlingRange;
    init->DxgkDdiPatch                                            = WinMaliKmdStub_Patch;
    init->DxgkDdiSubmitCommand                                    = WinMaliKmdSubmitCommand;
    init->DxgkDdiPreemptCommand                                   = WinMaliKmdPreemptCommand;
    init->DxgkDdiBuildPagingBuffer                                = WinMaliKmdBuildPagingBuffer;
    init->DxgkDdiSetPalette                                       = NULL;
    init->DxgkDdiSetPointerPosition                               = WinMaliKmdSetPointerPosition;
    init->DxgkDdiSetPointerShape                                  = WinMaliKmdSetPointerShape;
    init->DxgkDdiResetFromTimeout                                 = WinMaliKmdResetFromTimeout;
    init->DxgkDdiRestartFromTimeout                               = WinMaliKmdRestartFromTimeout;
    init->DxgkDdiEscape                                           = WinMaliKmdEscape;
    init->DxgkDdiCollectDbgInfo                                   = WinMaliKmdCollectDbgInfo;
    init->DxgkDdiQueryCurrentFence                                = WinMaliKmdQueryCurrentFence;
    init->DxgkDdiIsSupportedVidPn                                 = WinMaliKmdIsSupportedVidPn;
    init->DxgkDdiRecommendFunctionalVidPn                         = WinMaliKmdRecommendFunctionalVidPn;
    init->DxgkDdiEnumVidPnCofuncModality                          = WinMaliKmdEnumVidPnCofuncModality;
    init->DxgkDdiSetVidPnSourceAddress                            = WinMaliKmdSetVidPnSourceAddress;
    init->DxgkDdiSetVidPnSourceVisibility                         = WinMaliKmdSetVidPnSourceVisibility;
    init->DxgkDdiCommitVidPn                                      = WinMaliKmdCommitVidPn;
    init->DxgkDdiUpdateActiveVidPnPresentPath                     = WinMaliKmdUpdateActiveVidPnPresentPath;
    init->DxgkDdiRecommendMonitorModes                            = WinMaliKmdRecommendMonitorModes;
    init->DxgkDdiRecommendVidPnTopology                           = WinMaliKmdRecommendVidPnTopology;
    init->DxgkDdiGetScanLine                                      = WinMaliKmdGetScanLine;
    init->DxgkDdiStopCapture                                      = NULL;
    init->DxgkDdiControlInterrupt                                 = WinMaliKmdControlInterrupt;
    init->DxgkDdiCreateOverlay                                    = NULL;
    init->DxgkDdiDestroyDevice                                    = WinMaliKmdDestroyDevice;
    init->DxgkDdiOpenAllocation                                   = WinMaliKmdOpenAllocation;
    init->DxgkDdiCloseAllocation                                  = WinMaliKmdCloseAllocation;
    init->DxgkDdiRender                                           = WinMaliKmdStub_Render;
    init->DxgkDdiPresent                                          = WinMaliKmdPresent;
    init->DxgkDdiUpdateOverlay                                    = NULL;
    init->DxgkDdiFlipOverlay                                      = NULL;
    init->DxgkDdiDestroyOverlay                                   = NULL;
    init->DxgkDdiCreateContext                                    = WinMaliKmdCreateContext;
    init->DxgkDdiDestroyContext                                   = WinMaliKmdDestroyContext;
    init->DxgkDdiLinkDevice                                       = NULL;
    init->DxgkDdiSetDisplayPrivateDriverFormat                    = NULL;
    init->DxgkDdiRenderKm                                         = WinMaliKmdStub_RenderKm;
    init->DxgkDdiQueryVidPnHWCapability                           = WinMaliKmdQueryVidPnHWCapability;
    init->DxgkDdiUpdateMonitorLinkInfo                            = WinMaliKmdUpdateMonitorLinkInfo;

    /*
     * ⛔⛔ MCDM AND THE DISPLAY DDIs ARE MUTUALLY EXCLUSIVE.
     *
     * MEASURED (KMD 1.0.0.96): wiring the VidPN DDIs non-NULL while
     * ComputeOnly=1 made dxgk reject an adapter that had been starting cleanly
     * for days -- CM_PROB_FAILED_POST_START on the configuration that WORKED,
     * and the VidPN DDIs were never called even once. Declaring "I am compute
     * only, I have no display" in DXGK_DRIVERCAPS.MiscCaps while simultaneously
     * offering a display DDI table is self-contradictory, and dxgk validates it.
     *
     * So the table must agree with the caps: present the display DDIs ONLY when
     * we are actually claiming to be a display adapter.
     */
    /* ⛔⛔★★★★★ THE MCDM GUARD MOVED TO THE END OF THIS FUNCTION.
     *
     * It used to sit HERE, in the middle, and four later unconditional
     * assignments simply overwrote it -- StopDeviceAndReleasePostDisplayOwnership,
     * SystemDisplayEnable, SystemDisplayWrite and GetChildContainerId were all
     * live under ComputeOnly=1 despite the guard, and UpdateMonitorLinkInfo was
     * force-NULLed under BOTH modes by a stale line further down.
     *
     * A guard that runs before the assignments it is guarding is not a guard.
     * WinMaliApplyMcdmExclusions() now runs LAST so nothing can defeat it.
     */
    init->DxgkDdiSetPowerComponentFState                          = WinMaliKmdStub_SetPowerComponentFState;
    init->DxgkDdiQueryDependentEngineGroup                        = WinMaliKmdStub_QueryDependentEngineGroup;
    init->DxgkDdiQueryEngineStatus                                = WinMaliKmdQueryEngineStatus;
    init->DxgkDdiResetEngine                                      = WinMaliKmdResetEngine;
    init->DxgkDdiStopDeviceAndReleasePostDisplayOwnership         = WinMaliKmdStopDeviceAndReleasePostDisplayOwnership;
    init->DxgkDdiSystemDisplayEnable                              = WinMaliKmdSystemDisplayEnable;
    init->DxgkDdiSystemDisplayWrite                               = WinMaliKmdSystemDisplayWrite;
    init->DxgkDdiCancelCommand                                    = WinMaliKmdCancelCommand;
    init->DxgkDdiGetChildContainerId                              = WinMaliKmdGetChildContainerId;
    init->DxgkDdiPowerRuntimeControlRequest                       = WinMaliKmdStub_PowerRuntimeControlRequest;
    init->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay       = NULL;
    init->DxgkDdiNotifySurpriseRemoval                            = WinMaliKmdStub_NotifySurpriseRemoval;
    init->DxgkDdiGetNodeMetadata                                  = WinMaliKmdGetNodeMetadata;
    init->DxgkDdiSetPowerPState                                   = WinMaliKmdStub_SetPowerPState;
    init->DxgkDdiControlInterrupt2                                = WinMaliKmdStub_ControlInterrupt2;
    init->DxgkDdiCheckMultiPlaneOverlaySupport                    = NULL;
    init->DxgkDdiCalibrateGpuClock                                = WinMaliKmdStub_CalibrateGpuClock;
    init->DxgkDdiFormatHistoryBuffer                              = WinMaliKmdStub_FormatHistoryBuffer;
    init->DxgkDdiRenderGdi                                        = WinMaliKmdStub_RenderGdi;
    init->DxgkDdiSubmitCommandVirtual                             = WinMaliKmdSubmitCommandVirtual;
    init->DxgkDdiSetRootPageTable                                 = WinMaliKmdSetRootPageTable;
    init->DxgkDdiGetRootPageTableSize                             = WinMaliKmdGetRootPageTableSize;
    init->DxgkDdiMapCpuHostAperture                               = WinMaliKmdStub_MapCpuHostAperture;
    init->DxgkDdiUnmapCpuHostAperture                             = WinMaliKmdStub_UnmapCpuHostAperture;
    init->DxgkDdiCheckMultiPlaneOverlaySupport2                   = NULL;
    init->DxgkDdiCreateProcess                                    = WinMaliKmdCreateProcess;
    init->DxgkDdiDestroyProcess                                   = WinMaliKmdDestroyProcess;
    init->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay2      = NULL;
    init->DxgkDdiPowerRuntimeSetDeviceHandle                      = WinMaliKmdStub_PowerRuntimeSetDeviceHandle;
    init->DxgkDdiSetStablePowerState                              = WinMaliKmdStub_SetStablePowerState;
    init->DxgkDdiSetVideoProtectedRegion                          = WinMaliKmdStub_SetVideoProtectedRegion;
    /* ⛔★★★★★ GATE THE WIRING, NOT JUST THE CAP.
       MEASURED 1.1.9.39: wiring these three unconditionally gave device CODE 43
       (FAILED_POST_START) with QueryCapsOutSz==0 and QueryInterfaceCount 22->9 -
       i.e. adapter start aborted BEFORE dxgk ever read DRIVERCAPS, so the cap
       write was never even reached and QaiMpoCaps could not undo it. Filling a
       DDI slot is itself a claim about the interface contract, and it is made
       EARLIER than any capability query. Gate both halves on the same knob or
       the recovery path is a lie. */
    /* ★ "|| Caps" is NOT redundant: it preserves the invariant that the cap can
       never be claimed with NULL slots behind it, while allowing the one arm
       that has never been run - Wire=1 / Caps=0, i.e. 5-of-5 slots with no cap.
       1.1.9.39 tested 3-of-5 slots with no cap, which is a different thing. */
    if (g_WinMaliQaiMpoWire != 0 || g_WinMaliQaiMpoCaps != 0) {
        /* WDDM2_1 unit: all THREE together (dispmprt.h:2893-2895). */
        init->DxgkDdiCheckMultiPlaneOverlaySupport3               = WinMaliKmdCheckMultiPlaneOverlaySupport3;
        init->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3  = WinMaliKmdSetVidPnSourceAddressWithMultiPlaneOverlay3;
        init->DxgkDdiPostMultiPlaneOverlayPresent                 = WinMaliKmdPostMultiPlaneOverlayPresent;
    } else {
        init->DxgkDdiCheckMultiPlaneOverlaySupport3               = NULL;
        init->DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3  = NULL;
        init->DxgkDdiPostMultiPlaneOverlayPresent                 = NULL;
    }
    init->DxgkDdiValidateUpdateAllocationProperty                 = WinMaliKmdStub_ValidateUpdateAllocationProperty;
    init->DxgkDdiControlModeBehavior                              = NULL;
    /* ⛔ The unconditional "= NULL" that used to sit here silently undid the
       wiring at the top of this function -- it ran LATER and won. Every result
       recorded as "UpdateMonitorLinkInfo wired, no change" actually shipped a
       NULL pointer and tested nothing. See the ordering note at the guard. */
    init->DxgkDdiCreateHwContext                                  = WinMaliKmdStub_CreateHwContext;
    init->DxgkDdiDestroyHwContext                                 = WinMaliKmdStub_DestroyHwContext;
    init->DxgkDdiCreateHwQueue                                    = WinMaliKmdStub_CreateHwQueue;
    init->DxgkDdiDestroyHwQueue                                   = WinMaliKmdStub_DestroyHwQueue;
    init->DxgkDdiSubmitCommandToHwQueue                           = WinMaliKmdStub_SubmitCommandToHwQueue;
    init->DxgkDdiSwitchToHwContextList                            = WinMaliKmdStub_SwitchToHwContextList;
    init->DxgkDdiResetHwEngine                                    = WinMaliKmdStub_ResetHwEngine;
    init->DxgkDdiCreatePeriodicFrameNotification                  = NULL;
    init->DxgkDdiDestroyPeriodicFrameNotification                 = NULL;
    init->DxgkDdiSetTimingsFromVidPn                              = NULL;
    init->DxgkDdiSetTargetGamma                                   = NULL;
    init->DxgkDdiSetTargetContentType                             = NULL;
    init->DxgkDdiSetTargetAnalogCopyProtection                    = NULL;
    init->DxgkDdiSetTargetAdjustedColorimetry                     = NULL;
    init->DxgkDdiDisplayDetectControl                             = NULL;
    init->DxgkDdiQueryConnectionChange                            = NULL;
    init->DxgkDdiExchangePreStartInfo                             = NULL;
    /* WDDM2_2 pair: BOTH together (dispmprt.h:2934-2935). The v3 arg structs
       carry post-composition payloads, so declaring Check3/Set3 without a
       post-composition caps source is an incoherent claim. */
    init->DxgkDdiGetMultiPlaneOverlayCaps                         =
        (g_WinMaliQaiMpoWire != 0 || g_WinMaliQaiMpoCaps != 0)
            ? WinMaliKmdGetMultiPlaneOverlayCaps : NULL;
    init->DxgkDdiGetPostCompositionCaps                           =
        (g_WinMaliQaiMpoWire != 0 || g_WinMaliQaiMpoCaps != 0)
            ? WinMaliKmdGetPostCompositionCaps : NULL;
    init->DxgkDdiUpdateHwContextState                             = WinMaliKmdStub_UpdateHwContextState;
    init->DxgkDdiCreateProtectedSession                           = WinMaliKmdStub_CreateProtectedSession;
    init->DxgkDdiDestroyProtectedSession                          = WinMaliKmdStub_DestroyProtectedSession;
    init->DxgkDdiSetSchedulingLogBuffer                           = WinMaliKmdStub_SetSchedulingLogBuffer;
    init->DxgkDdiSetupPriorityBands                               = WinMaliKmdStub_SetupPriorityBands;
    init->DxgkDdiNotifyFocusPresent                               = WinMaliKmdStub_NotifyFocusPresent;
    init->DxgkDdiSetContextSchedulingProperties                   = WinMaliKmdStub_SetContextSchedulingProperties;
    init->DxgkDdiSuspendContext                                   = WinMaliKmdStub_SuspendContext;
    init->DxgkDdiResumeContext                                    = WinMaliKmdStub_ResumeContext;
    init->DxgkDdiSetVirtualMachineData                            = WinMaliKmdStub_SetVirtualMachineData;
    init->DxgkDdiBeginExclusiveAccess                             = WinMaliKmdStub_BeginExclusiveAccess;
    init->DxgkDdiEndExclusiveAccess                               = WinMaliKmdStub_EndExclusiveAccess;
    init->DxgkDdiQueryDiagnosticTypesSupport                      = WinMaliKmdStub_QueryDiagnosticTypesSupport;
    init->DxgkDdiControlDiagnosticReporting                       = WinMaliKmdStub_ControlDiagnosticReporting;
    init->DxgkDdiResumeHwEngine                                   = WinMaliKmdStub_ResumeHwEngine;
    init->DxgkDdiSignalMonitoredFence                             = WinMaliKmdStub_SignalMonitoredFence;
    init->DxgkDdiPresentToHwQueue                                 = WinMaliKmdStub_PresentToHwQueue;
    init->DxgkDdiValidateSubmitCommand                            = WinMaliKmdStub_ValidateSubmitCommand;
    init->DxgkDdiSetTargetAdjustedColorimetry2                    = NULL;
    init->DxgkDdiSetTrackedWorkloadPowerLevel                     = WinMaliKmdStub_SetTrackedWorkloadPowerLevel;
    init->DxgkDdiSaveMemoryForHotUpdate                           = WinMaliKmdStub_SaveMemoryForHotUpdate;
    init->DxgkDdiRestoreMemoryForHotUpdate                        = WinMaliKmdStub_RestoreMemoryForHotUpdate;
    init->DxgkDdiCollectDiagnosticInfo                            = WinMaliKmdStub_CollectDiagnosticInfo;
    init->DxgkDdiControlInterrupt3                                = WinMaliKmdStub_ControlInterrupt3;
    init->DxgkDdiSetFlipQueueLogBuffer                            = NULL;
    init->DxgkDdiUpdateFlipQueueLog                               = NULL;
    init->DxgkDdiCancelQueuedFlips                                = NULL;
    init->DxgkDdiSetInterruptTargetPresentId                      = NULL;
    init->DxgkDdiSetAllocationBackingStore                        = WinMaliKmdStub_SetAllocationBackingStore;
    init->DxgkDdiCreateCpuEvent                                   = WinMaliKmdStub_CreateCpuEvent;
    init->DxgkDdiDestroyCpuEvent                                  = WinMaliKmdStub_DestroyCpuEvent;
    init->DxgkDdiCancelFlips                                      = NULL;
    init->DxgkDdiCreateNativeFence                                = WinMaliKmdStub_CreateNativeFence;
    init->DxgkDdiDestroyNativeFence                               = WinMaliKmdStub_DestroyNativeFence;
    init->DxgkDdiUpdateMonitoredValues                            = WinMaliKmdStub_UpdateMonitoredValues;
    init->DxgkDdiUpdateCurrentValuesFromCpu                       = WinMaliKmdStub_UpdateCurrentValuesFromCpu;
    init->DxgkDdiCreateDoorbell                                   = WinMaliKmdStub_CreateDoorbell;
    init->DxgkDdiConnectDoorbell                                  = WinMaliKmdStub_ConnectDoorbell;
    init->DxgkDdiDisconnectDoorbell                               = WinMaliKmdStub_DisconnectDoorbell;
    init->DxgkDdiDestroyDoorbell                                  = WinMaliKmdStub_DestroyDoorbell;
    init->DxgkDdiNotifyWorkSubmission                             = WinMaliKmdStub_NotifyWorkSubmission;
    init->DxgkDdiCreateMemoryBasis                                = WinMaliKmdStub_CreateMemoryBasis;
    init->DxgkDdiDestroyMemoryBasis                               = WinMaliKmdStub_DestroyMemoryBasis;
    init->DxgkDdiStartDirtyTracking                               = WinMaliKmdStub_StartDirtyTracking;
    init->DxgkDdiStopDirtyTracking                                = WinMaliKmdStub_StopDirtyTracking;
    init->DxgkDdiQueryDirtyBitData                                = WinMaliKmdStub_QueryDirtyBitData;
    init->DxgkDdiPrepareLiveMigration                             = WinMaliKmdStub_PrepareLiveMigration;
    init->DxgkDdiSaveImmutableMigrationData                       = WinMaliKmdStub_SaveImmutableMigrationData;
    init->DxgkDdiSaveMutableMigrationData                         = WinMaliKmdStub_SaveMutableMigrationData;
    init->DxgkDdiEndLiveMigration                                 = WinMaliKmdStub_EndLiveMigration;
    init->DxgkDdiRestoreImmutableMigrationData                    = WinMaliKmdStub_RestoreImmutableMigrationData;
    init->DxgkDdiRestoreMutableMigrationData                      = WinMaliKmdStub_RestoreMutableMigrationData;
    init->DxgkDdiWriteVirtualizedInterrupt                        = WinMaliKmdStub_WriteVirtualizedInterrupt;
    init->DxgkDdiSetVirtualGpuResources2                          = WinMaliKmdStub_SetVirtualGpuResources2;
    init->DxgkDdiSetVirtualFunctionPauseState                     = WinMaliKmdStub_SetVirtualFunctionPauseState;
    init->DxgkDdiOpenNativeFence                                  = WinMaliKmdStub_OpenNativeFence;
    init->DxgkDdiCloseNativeFence                                 = WinMaliKmdStub_CloseNativeFence;
    init->DxgkDdiSetNativeFenceLogBuffer                          = WinMaliKmdStub_SetNativeFenceLogBuffer;
    init->DxgkDdiUpdateNativeFenceLogs                            = WinMaliKmdStub_UpdateNativeFenceLogs;
    init->DxgkDdiCollectDbgInfo2                                  = WinMaliKmdStub_CollectDbgInfo2;
    init->DxgkDdiNotifyContextPriorityChange                      = WinMaliKmdStub_NotifyContextPriorityChange;
    init->DxgkDdiResetDisplayEngine                               = NULL;

    /*
     * ⛔⛔ MCDM AND THE DISPLAY DDIs ARE MUTUALLY EXCLUSIVE -- APPLIED LAST.
     *
     * MEASURED (KMD 1.0.0.96): wiring the VidPN DDIs non-NULL while
     * ComputeOnly=1 made dxgk reject an adapter that had been starting cleanly
     * for days -- CM_PROB_FAILED_POST_START on the configuration that WORKED,
     * and the VidPN DDIs were never called even once. Declaring "I am compute
     * only, I have no display" in DXGK_DRIVERCAPS.MiscCaps while simultaneously
     * offering a display DDI table is self-contradictory, and dxgk validates it.
     *
     * ★★★★★ THIS MUST BE THE LAST THING THE FUNCTION DOES. When these exclusions
     * lived in the middle of the table, four later assignments silently overwrote
     * them and the guard was a no-op for exactly the DDIs it was written to
     * suppress. Anything added below this block is UNGUARDED -- add it above.
     */
    if (g_WinMaliComputeOnly != 0) {
        init->DxgkDdiIsSupportedVidPn             = NULL;
        init->DxgkDdiRecommendFunctionalVidPn     = NULL;
        init->DxgkDdiEnumVidPnCofuncModality      = NULL;
        init->DxgkDdiSetVidPnSourceAddress        = NULL;
        init->DxgkDdiSetVidPnSourceVisibility     = NULL;
        init->DxgkDdiCommitVidPn                  = NULL;
        init->DxgkDdiUpdateActiveVidPnPresentPath = NULL;
        init->DxgkDdiRecommendVidPnTopology       = NULL;
        init->DxgkDdiRecommendMonitorModes        = NULL;
        init->DxgkDdiQueryVidPnHWCapability       = NULL;
        init->DxgkDdiSetPointerPosition           = NULL;
        init->DxgkDdiSetPointerShape              = NULL;
        init->DxgkDdiGetScanLine                  = NULL;
        init->DxgkDdiStopDeviceAndReleasePostDisplayOwnership = NULL;
        init->DxgkDdiSystemDisplayEnable          = NULL;
        init->DxgkDdiSystemDisplayWrite           = NULL;
        init->DxgkDdiUpdateMonitorLinkInfo        = NULL;
        init->DxgkDdiGetChildContainerId          = NULL;
    }

    /* Publish what the table ACTUALLY ended up as, so a wiring bug shows up in
       the log instead of masquerading as an experimental result. This is how
       the dead-store above was missed for a whole round of tests. */
    DbgPrint("[WinMali]    DDI table: ComputeOnly=%lu UpdateMonitorLinkInfo=%s "
             "GetChildContainerId=%s QueryChildStatus=%s SysDisplayEnable=%s\n",
             g_WinMaliComputeOnly,
             init->DxgkDdiUpdateMonitorLinkInfo ? "set" : "NULL",
             init->DxgkDdiGetChildContainerId   ? "set" : "NULL",
             init->DxgkDdiQueryChildStatus      ? "set" : "NULL",
             init->DxgkDdiSystemDisplayEnable   ? "set" : "NULL");



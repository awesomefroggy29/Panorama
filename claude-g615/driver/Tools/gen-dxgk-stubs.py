#!/usr/bin/env python3

import re
import sys
from pathlib import Path


def _find_wdk_include():
    """Newest installed WDK Include dir that ships the km\\ headers.

    Matches the WindowsTargetPlatformVersion the .vcxproj files target
    (currently the 26100 Windows 11 WDK) but falls back to whatever is
    installed so this tool keeps working across SDK bumps.
    """
    root = Path(r"F:\Program Files\Windows Kits\10\Include")
    if root.is_dir():
        versions = sorted(
            (d for d in root.iterdir()
             if re.match(r"^10\.0\.\d+\.\d+$", d.name)
             and (d / "km" / "dispmprt.h").is_file()),
            key=lambda d: [int(x) for x in d.name.split(".")],
            reverse=True,
        )
        if versions:
            return versions[0]
    # Last-resort hard default; will raise clearly below if absent.
    return root / "10.0.26100.0"


WDK_INCLUDE = _find_wdk_include()
DISPMPRT_H  = WDK_INCLUDE / "km" / "dispmprt.h"
D3DKMDDI_H  = WDK_INCLUDE / "shared" / "d3dkmddi.h"

# Names that already have hand-written implementations in WinMaliDriver.c.
# We skip stubs for these and use the existing functions in the wire-up.
EXISTING = {
    # Lifecycle / power / ACPI - hand-written in WinMaliDriver.c
    "DxgkDdiAddDevice":              "WinMaliKmdAddDevice",
    "DxgkDdiStartDevice":            "WinMaliKmdStartDevice",
    "DxgkDdiStopDevice":             "WinMaliKmdStopDevice",
    "DxgkDdiRemoveDevice":           "WinMaliKmdRemoveDevice",
    "DxgkDdiDispatchIoRequest":      "WinMaliKmdDispatchIoRequest",
    "DxgkDdiInterruptRoutine":       "WinMaliKmdInterruptRoutine",
    "DxgkDdiDpcRoutine":             "WinMaliKmdDpcRoutine",
    "DxgkDdiQueryChildRelations":    "WinMaliKmdQueryChildRelations",
    "DxgkDdiQueryChildStatus":       "WinMaliKmdQueryChildStatus",
    "DxgkDdiQueryDeviceDescriptor":  "WinMaliKmdQueryDeviceDescriptor",
    "DxgkDdiSetPowerState":          "WinMaliKmdSetPowerState",
    "DxgkDdiNotifyAcpiEvent":        "WinMaliKmdNotifyAcpiEvent",
    "DxgkDdiResetDevice":            "WinMaliKmdResetDevice",
    "DxgkDdiUnload":                 "WinMaliKmdDxgkUnload",
    "DxgkDdiQueryInterface":         "WinMaliKmdQueryInterface",
    "DxgkDdiControlEtwLogging":      "WinMaliKmdControlEtwLogging",
    "DxgkDdiQueryAdapterInfo":       "WinMaliKmdQueryAdapterInfo",   # WinMaliQai.c
    # Real DDIs - WinMaliDdi.c
    "DxgkDdiGetNodeMetadata":        "WinMaliKmdGetNodeMetadata",
    "DxgkDdiCreateProcess":          "WinMaliKmdCreateProcess",
    "DxgkDdiDestroyProcess":         "WinMaliKmdDestroyProcess",
    "DxgkDdiCreateDevice":           "WinMaliKmdCreateDevice",
    "DxgkDdiDestroyDevice":          "WinMaliKmdDestroyDevice",
    "DxgkDdiCreateContext":          "WinMaliKmdCreateContext",
    "DxgkDdiDestroyContext":         "WinMaliKmdDestroyContext",
    "DxgkDdiSetRootPageTable":       "WinMaliKmdSetRootPageTable",
    "DxgkDdiGetRootPageTableSize":   "WinMaliKmdGetRootPageTableSize",
    "DxgkDdiQueryCurrentFence":      "WinMaliKmdQueryCurrentFence",
    "DxgkDdiControlInterrupt":       "WinMaliKmdControlInterrupt",
    "DxgkDdiResetFromTimeout":       "WinMaliKmdResetFromTimeout",
    "DxgkDdiRestartFromTimeout":     "WinMaliKmdRestartFromTimeout",
    "DxgkDdiCollectDbgInfo":         "WinMaliKmdCollectDbgInfo",
    # Allocation lifecycle - WinMaliAlloc.c
    "DxgkDdiCreateAllocation":       "WinMaliKmdCreateAllocation",
    "DxgkDdiDestroyAllocation":      "WinMaliKmdDestroyAllocation",
    "DxgkDdiOpenAllocation":         "WinMaliKmdOpenAllocation",
    "DxgkDdiCloseAllocation":        "WinMaliKmdCloseAllocation",
    "DxgkDdiDescribeAllocation":     "WinMaliKmdDescribeAllocation",
    "DxgkDdiGetStandardAllocationDriverData": "WinMaliKmdGetStandardAllocationDriverData",
    # Paging - WinMaliPaging.c
    "DxgkDdiBuildPagingBuffer":      "WinMaliKmdBuildPagingBuffer",
    # Submit - WinMaliDdi.c (synchronous fence-completion path)
    "DxgkDdiSubmitCommand":          "WinMaliKmdSubmitCommand",
    # NOTE: DxgkDdiExchangePreStartInfo is deliberately NOT wired even
    # though WinMaliKmdExchangePreStartInfo exists in WinMaliDriver.c -
    # it's part of CreateDisplayCore's display-DDI all-or-none sweep
    # (see NULL_FIELDS below). Re-wire it only if we ever grow a
    # display core.
    "DxgkDdiEscape":                 "WinMaliKmdEscape",
}

# The struct-field-name -> typedef-name mapping.  Pulled from dispmprt.h's
# DRIVER_INITIALIZATION_DATA. Order matches the struct's declaration order.
# Reserved/PVOID slots (DescribePageTable etc.) are omitted - they get
# memset(0) by RtlZeroMemory and don't need stubs.
INIT_FIELDS = [
    # Always
    ("DxgkDdiAddDevice",                       "DXGKDDI_ADD_DEVICE"),
    ("DxgkDdiStartDevice",                     "DXGKDDI_START_DEVICE"),
    ("DxgkDdiStopDevice",                      "DXGKDDI_STOP_DEVICE"),
    ("DxgkDdiRemoveDevice",                    "DXGKDDI_REMOVE_DEVICE"),
    ("DxgkDdiDispatchIoRequest",               "DXGKDDI_DISPATCH_IO_REQUEST"),
    ("DxgkDdiInterruptRoutine",                "DXGKDDI_INTERRUPT_ROUTINE"),
    ("DxgkDdiDpcRoutine",                      "DXGKDDI_DPC_ROUTINE"),
    ("DxgkDdiQueryChildRelations",             "DXGKDDI_QUERY_CHILD_RELATIONS"),
    ("DxgkDdiQueryChildStatus",                "DXGKDDI_QUERY_CHILD_STATUS"),
    ("DxgkDdiQueryDeviceDescriptor",           "DXGKDDI_QUERY_DEVICE_DESCRIPTOR"),
    ("DxgkDdiSetPowerState",                   "DXGKDDI_SET_POWER_STATE"),
    ("DxgkDdiNotifyAcpiEvent",                 "DXGKDDI_NOTIFY_ACPI_EVENT"),
    ("DxgkDdiResetDevice",                     "DXGKDDI_RESET_DEVICE"),
    ("DxgkDdiUnload",                          "DXGKDDI_UNLOAD"),
    ("DxgkDdiQueryInterface",                  "DXGKDDI_QUERY_INTERFACE"),
    ("DxgkDdiControlEtwLogging",               "DXGKDDI_CONTROL_ETW_LOGGING"),
    ("DxgkDdiQueryAdapterInfo",                "DXGKDDI_QUERYADAPTERINFO"),
    ("DxgkDdiCreateDevice",                    "DXGKDDI_CREATEDEVICE"),
    ("DxgkDdiCreateAllocation",                "DXGKDDI_CREATEALLOCATION"),
    ("DxgkDdiDestroyAllocation",               "DXGKDDI_DESTROYALLOCATION"),
    ("DxgkDdiDescribeAllocation",              "DXGKDDI_DESCRIBEALLOCATION"),
    ("DxgkDdiGetStandardAllocationDriverData", "DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA"),
    ("DxgkDdiAcquireSwizzlingRange",           "DXGKDDI_ACQUIRESWIZZLINGRANGE"),
    ("DxgkDdiReleaseSwizzlingRange",           "DXGKDDI_RELEASESWIZZLINGRANGE"),
    ("DxgkDdiPatch",                           "DXGKDDI_PATCH"),
    ("DxgkDdiSubmitCommand",                   "DXGKDDI_SUBMITCOMMAND"),
    ("DxgkDdiPreemptCommand",                  "DXGKDDI_PREEMPTCOMMAND"),
    ("DxgkDdiBuildPagingBuffer",               "DXGKDDI_BUILDPAGINGBUFFER"),
    ("DxgkDdiSetPalette",                      "DXGKDDI_SETPALETTE"),
    ("DxgkDdiSetPointerPosition",              "DXGKDDI_SETPOINTERPOSITION"),
    ("DxgkDdiSetPointerShape",                 "DXGKDDI_SETPOINTERSHAPE"),
    ("DxgkDdiResetFromTimeout",                "DXGKDDI_RESETFROMTIMEOUT"),
    ("DxgkDdiRestartFromTimeout",              "DXGKDDI_RESTARTFROMTIMEOUT"),
    ("DxgkDdiEscape",                          "DXGKDDI_ESCAPE"),
    ("DxgkDdiCollectDbgInfo",                  "DXGKDDI_COLLECTDBGINFO"),
    ("DxgkDdiQueryCurrentFence",               "DXGKDDI_QUERYCURRENTFENCE"),
    ("DxgkDdiIsSupportedVidPn",                "DXGKDDI_ISSUPPORTEDVIDPN"),
    ("DxgkDdiRecommendFunctionalVidPn",        "DXGKDDI_RECOMMENDFUNCTIONALVIDPN"),
    ("DxgkDdiEnumVidPnCofuncModality",         "DXGKDDI_ENUMVIDPNCOFUNCMODALITY"),
    ("DxgkDdiSetVidPnSourceAddress",           "DXGKDDI_SETVIDPNSOURCEADDRESS"),
    ("DxgkDdiSetVidPnSourceVisibility",        "DXGKDDI_SETVIDPNSOURCEVISIBILITY"),
    ("DxgkDdiCommitVidPn",                     "DXGKDDI_COMMITVIDPN"),
    ("DxgkDdiUpdateActiveVidPnPresentPath",    "DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH"),
    ("DxgkDdiRecommendMonitorModes",           "DXGKDDI_RECOMMENDMONITORMODES"),
    ("DxgkDdiRecommendVidPnTopology",          "DXGKDDI_RECOMMENDVIDPNTOPOLOGY"),
    ("DxgkDdiGetScanLine",                     "DXGKDDI_GETSCANLINE"),
    ("DxgkDdiStopCapture",                     "DXGKDDI_STOPCAPTURE"),
    ("DxgkDdiControlInterrupt",                "DXGKDDI_CONTROLINTERRUPT"),
    ("DxgkDdiCreateOverlay",                   "DXGKDDI_CREATEOVERLAY"),
    ("DxgkDdiDestroyDevice",                   "DXGKDDI_DESTROYDEVICE"),
    ("DxgkDdiOpenAllocation",                  "DXGKDDI_OPENALLOCATIONINFO"),
    ("DxgkDdiCloseAllocation",                 "DXGKDDI_CLOSEALLOCATION"),
    ("DxgkDdiRender",                          "DXGKDDI_RENDER"),
    ("DxgkDdiPresent",                         "DXGKDDI_PRESENT"),
    ("DxgkDdiUpdateOverlay",                   "DXGKDDI_UPDATEOVERLAY"),
    ("DxgkDdiFlipOverlay",                     "DXGKDDI_FLIPOVERLAY"),
    ("DxgkDdiDestroyOverlay",                  "DXGKDDI_DESTROYOVERLAY"),
    ("DxgkDdiCreateContext",                   "DXGKDDI_CREATECONTEXT"),
    ("DxgkDdiDestroyContext",                  "DXGKDDI_DESTROYCONTEXT"),
    ("DxgkDdiLinkDevice",                      "DXGKDDI_LINK_DEVICE"),
    ("DxgkDdiSetDisplayPrivateDriverFormat",   "DXGKDDI_SETDISPLAYPRIVATEDRIVERFORMAT"),
    # WIN7 / WDDM 1.2
    ("DxgkDdiRenderKm",                        "DXGKDDI_RENDER"),
    ("DxgkDdiQueryVidPnHWCapability",          "DXGKDDI_QUERYVIDPNHWCAPABILITY"),
    # WIN8 / WDDM 1.3
    ("DxgkDdiSetPowerComponentFState",         "DXGKDDISETPOWERCOMPONENTFSTATE"),
    ("DxgkDdiQueryDependentEngineGroup",       "DXGKDDI_QUERYDEPENDENTENGINEGROUP"),
    ("DxgkDdiQueryEngineStatus",               "DXGKDDI_QUERYENGINESTATUS"),
    ("DxgkDdiResetEngine",                     "DXGKDDI_RESETENGINE"),
    ("DxgkDdiStopDeviceAndReleasePostDisplayOwnership", "DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP"),
    ("DxgkDdiSystemDisplayEnable",             "DXGKDDI_SYSTEM_DISPLAY_ENABLE"),
    ("DxgkDdiSystemDisplayWrite",              "DXGKDDI_SYSTEM_DISPLAY_WRITE"),
    ("DxgkDdiCancelCommand",                   "DXGKDDI_CANCELCOMMAND"),
    ("DxgkDdiGetChildContainerId",             "DXGKDDI_GET_CHILD_CONTAINER_ID"),
    ("DxgkDdiPowerRuntimeControlRequest",      "DXGKDDIPOWERRUNTIMECONTROLREQUEST"),
    ("DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay", "DXGKDDI_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY"),
    ("DxgkDdiNotifySurpriseRemoval",           "DXGKDDI_NOTIFY_SURPRISE_REMOVAL"),
    # WDDM 1.3
    ("DxgkDdiGetNodeMetadata",                 "DXGKDDI_GETNODEMETADATA"),
    ("DxgkDdiSetPowerPState",                  "DXGKDDISETPOWERPSTATE"),
    ("DxgkDdiControlInterrupt2",               "DXGKDDI_CONTROLINTERRUPT2"),
    ("DxgkDdiCheckMultiPlaneOverlaySupport",   "DXGKDDI_CHECKMULTIPLANEOVERLAYSUPPORT"),
    ("DxgkDdiCalibrateGpuClock",               "DXGKDDI_CALIBRATEGPUCLOCK"),
    ("DxgkDdiFormatHistoryBuffer",             "DXGKDDI_FORMATHISTORYBUFFER"),
    # WDDM 2.0
    ("DxgkDdiRenderGdi",                       "DXGKDDI_RENDERGDI"),
    ("DxgkDdiSubmitCommandVirtual",            "DXGKDDI_SUBMITCOMMANDVIRTUAL"),
    ("DxgkDdiSetRootPageTable",                "DXGKDDI_SETROOTPAGETABLE"),
    ("DxgkDdiGetRootPageTableSize",            "DXGKDDI_GETROOTPAGETABLESIZE"),
    ("DxgkDdiMapCpuHostAperture",              "DXGKDDI_MAPCPUHOSTAPERTURE"),
    ("DxgkDdiUnmapCpuHostAperture",            "DXGKDDI_UNMAPCPUHOSTAPERTURE"),
    ("DxgkDdiCheckMultiPlaneOverlaySupport2",  "DXGKDDI_CHECKMULTIPLANEOVERLAYSUPPORT2"),
    ("DxgkDdiCreateProcess",                   "DXGKDDI_CREATEPROCESS"),
    ("DxgkDdiDestroyProcess",                  "DXGKDDI_DESTROYPROCESS"),
    ("DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay2", "DXGKDDI_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY2"),
    ("DxgkDdiPowerRuntimeSetDeviceHandle",     "DXGKDDI_POWERRUNTIMESETDEVICEHANDLE"),
    ("DxgkDdiSetStablePowerState",             "DXGKDDI_SETSTABLEPOWERSTATE"),
    ("DxgkDdiSetVideoProtectedRegion",         "DXGKDDI_SETVIDEOPROTECTEDREGION"),
    # WDDM 2.1
    ("DxgkDdiCheckMultiPlaneOverlaySupport3",  "DXGKDDI_CHECKMULTIPLANEOVERLAYSUPPORT3"),
    ("DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3", "DXGKDDI_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3"),
    ("DxgkDdiPostMultiPlaneOverlayPresent",    "DXGKDDI_POSTMULTIPLANEOVERLAYPRESENT"),
    ("DxgkDdiValidateUpdateAllocationProperty", "DXGKDDI_VALIDATEUPDATEALLOCATIONPROPERTY"),
    ("DxgkDdiControlModeBehavior",             "DXGKDDI_CONTROLMODEBEHAVIOR"),
    ("DxgkDdiUpdateMonitorLinkInfo",           "DXGKDDI_UPDATEMONITORLINKINFO"),
    # WDDM 2.2
    ("DxgkDdiCreateHwContext",                 "DXGKDDI_CREATEHWCONTEXT"),
    ("DxgkDdiDestroyHwContext",                "DXGKDDI_DESTROYHWCONTEXT"),
    ("DxgkDdiCreateHwQueue",                   "DXGKDDI_CREATEHWQUEUE"),
    ("DxgkDdiDestroyHwQueue",                  "DXGKDDI_DESTROYHWQUEUE"),
    ("DxgkDdiSubmitCommandToHwQueue",          "DXGKDDI_SUBMITCOMMANDTOHWQUEUE"),
    ("DxgkDdiSwitchToHwContextList",           "DXGKDDI_SWITCHTOHWCONTEXTLIST"),
    ("DxgkDdiResetHwEngine",                   "DXGKDDI_RESETHWENGINE"),
    ("DxgkDdiCreatePeriodicFrameNotification", "DXGKDDI_CREATEPERIODICFRAMENOTIFICATION"),
    ("DxgkDdiDestroyPeriodicFrameNotification", "DXGKDDI_DESTROYPERIODICFRAMENOTIFICATION"),
    ("DxgkDdiSetTimingsFromVidPn",             "DXGKDDI_SETTIMINGSFROMVIDPN"),
    ("DxgkDdiSetTargetGamma",                  "DXGKDDI_SETTARGETGAMMA"),
    ("DxgkDdiSetTargetContentType",            "DXGKDDI_SETTARGETCONTENTTYPE"),
    ("DxgkDdiSetTargetAnalogCopyProtection",   "DXGKDDI_SETTARGETANALOGCOPYPROTECTION"),
    ("DxgkDdiSetTargetAdjustedColorimetry",    "DXGKDDI_SETTARGETADJUSTEDCOLORIMETRY"),
    ("DxgkDdiDisplayDetectControl",            "DXGKDDI_DISPLAYDETECTCONTROL"),
    ("DxgkDdiQueryConnectionChange",           "DXGKDDI_QUERYCONNECTIONCHANGE"),
    ("DxgkDdiExchangePreStartInfo",            "DXGKDDI_EXCHANGEPRESTARTINFO"),
    ("DxgkDdiGetMultiPlaneOverlayCaps",        "DXGKDDI_GETMULTIPLANEOVERLAYCAPS"),
    ("DxgkDdiGetPostCompositionCaps",          "DXGKDDI_GETPOSTCOMPOSITIONCAPS"),
    # WDDM 2.3
    ("DxgkDdiUpdateHwContextState",            "DXGKDDI_UPDATEHWCONTEXTSTATE"),
    ("DxgkDdiCreateProtectedSession",          "DXGKDDI_CREATEPROTECTEDSESSION"),
    ("DxgkDdiDestroyProtectedSession",         "DXGKDDI_DESTROYPROTECTEDSESSION"),
    # WDDM 2.4
    ("DxgkDdiSetSchedulingLogBuffer",          "DXGKDDI_SETSCHEDULINGLOGBUFFER"),
    ("DxgkDdiSetupPriorityBands",              "DXGKDDI_SETUPPRIORITYBANDS"),
    ("DxgkDdiNotifyFocusPresent",              "DXGKDDI_NOTIFYFOCUSPRESENT"),
    ("DxgkDdiSetContextSchedulingProperties",  "DXGKDDI_SETCONTEXTSCHEDULINGPROPERTIES"),
    ("DxgkDdiSuspendContext",                  "DXGKDDI_SUSPENDCONTEXT"),
    ("DxgkDdiResumeContext",                   "DXGKDDI_RESUMECONTEXT"),
    ("DxgkDdiSetVirtualMachineData",           "DXGKDDI_SETVIRTUALMACHINEDATA"),
    ("DxgkDdiBeginExclusiveAccess",            "DXGKDDI_BEGINEXCLUSIVEACCESS"),
    ("DxgkDdiEndExclusiveAccess",              "DXGKDDI_ENDEXCLUSIVEACCESS"),
    ("DxgkDdiQueryDiagnosticTypesSupport",     "DXGKDDI_QUERYDIAGNOSTICTYPESSUPPORT"),
    ("DxgkDdiControlDiagnosticReporting",      "DXGKDDI_CONTROLDIAGNOSTICREPORTING"),
    ("DxgkDdiResumeHwEngine",                  "DXGKDDI_RESUMEHWENGINE"),
    # WDDM 2.5
    ("DxgkDdiSignalMonitoredFence",            "DXGKDDI_SIGNALMONITOREDFENCE"),
    ("DxgkDdiPresentToHwQueue",                "DXGKDDI_PRESENTTOHWQUEUE"),
    ("DxgkDdiValidateSubmitCommand",           "DXGKDDI_VALIDATESUBMITCOMMAND"),
    ("DxgkDdiSetTargetAdjustedColorimetry2",   "DXGKDDI_SETTARGETADJUSTEDCOLORIMETRY2"),
    ("DxgkDdiSetTrackedWorkloadPowerLevel",    "DXGKDDI_SETTRACKEDWORKLOADPOWERLEVEL"),
    # WDDM 2.6  (Reserved3 slot omitted - zero-filled)
    ("DxgkDdiSaveMemoryForHotUpdate",          "DXGKDDI_SAVEMEMORYFORHOTUPDATE"),
    ("DxgkDdiRestoreMemoryForHotUpdate",       "DXGKDDI_RESTOREMEMORYFORHOTUPDATE"),
    ("DxgkDdiCollectDiagnosticInfo",           "DXGKDDI_COLLECTDIAGNOSTICINFO"),
    # WDDM 2.7
    ("DxgkDdiControlInterrupt3",               "DXGKDDI_CONTROLINTERRUPT3"),
    # WDDM 2.9
    ("DxgkDdiSetFlipQueueLogBuffer",           "DXGKDDI_SETFLIPQUEUELOGBUFFER"),
    ("DxgkDdiUpdateFlipQueueLog",              "DXGKDDI_UPDATEFLIPQUEUELOG"),
    ("DxgkDdiCancelQueuedFlips",               "DXGKDDI_CANCELQUEUEDFLIPS"),
    ("DxgkDdiSetInterruptTargetPresentId",     "DXGKDDI_SETINTERRUPTTARGETPRESENTID"),
    # WDDM 3.0
    ("DxgkDdiSetAllocationBackingStore",       "DXGKDDI_SETALLOCATIONBACKINGSTORE"),
    ("DxgkDdiCreateCpuEvent",                  "DXGKDDI_CREATECPUEVENT"),
    ("DxgkDdiDestroyCpuEvent",                 "DXGKDDI_DESTROYCPUEVENT"),
    ("DxgkDdiCancelFlips",                     "DXGKDDI_CANCELFLIPS"),
    # WDDM 3.1  (Reserved4 slot omitted - zero-filled)
    ("DxgkDdiCreateNativeFence",               "DXGKDDI_CREATENATIVEFENCE"),
    ("DxgkDdiDestroyNativeFence",              "DXGKDDI_DESTROYNATIVEFENCE"),
    ("DxgkDdiUpdateMonitoredValues",           "DXGKDDI_UPDATEMONITOREDVALUES"),
    ("DxgkDdiUpdateCurrentValuesFromCpu",      "DXGKDDI_UPDATECURRENTVALUESFROMCPU"),
    ("DxgkDdiCreateDoorbell",                  "DXGKDDI_CREATEDOORBELL"),
    ("DxgkDdiConnectDoorbell",                 "DXGKDDI_CONNECTDOORBELL"),
    ("DxgkDdiDisconnectDoorbell",              "DXGKDDI_DISCONNECTDOORBELL"),
    ("DxgkDdiDestroyDoorbell",                 "DXGKDDI_DESTROYDOORBELL"),
    ("DxgkDdiNotifyWorkSubmission",            "DXGKDDI_NOTIFYWORKSUBMISSION"),
    # WDDM 3.2
    ("DxgkDdiCreateMemoryBasis",               "DXGKDDI_CREATEMEMORYBASIS"),
    ("DxgkDdiDestroyMemoryBasis",              "DXGKDDI_DESTROYMEMORYBASIS"),
    ("DxgkDdiStartDirtyTracking",              "DXGKDDI_STARTDIRTYTRACKING"),
    ("DxgkDdiStopDirtyTracking",               "DXGKDDI_STOPDIRTYTRACKING"),
    ("DxgkDdiQueryDirtyBitData",               "DXGKDDI_QUERYDIRTYBITDATA"),
    ("DxgkDdiPrepareLiveMigration",            "DXGKDDI_PREPARELIVEMIGRATION"),
    ("DxgkDdiSaveImmutableMigrationData",      "DXGKDDI_SAVEIMMUTABLEMIGRATIONDATA"),
    ("DxgkDdiSaveMutableMigrationData",        "DXGKDDI_SAVEMUTABLEMIGRATIONDATA"),
    ("DxgkDdiEndLiveMigration",                "DXGKDDI_ENDLIVEMIGRATION"),
    ("DxgkDdiRestoreImmutableMigrationData",   "DXGKDDI_RESTOREIMMUTABLEMIGRATIONDATA"),
    ("DxgkDdiRestoreMutableMigrationData",     "DXGKDDI_RESTOREMUTABLEMIGRATIONDATA"),
    ("DxgkDdiWriteVirtualizedInterrupt",       "DXGKDDI_WRITEVIRTUALIZEDINTERRUPT"),
    ("DxgkDdiSetVirtualGpuResources2",         "DXGKDDI_SETVIRTUALGPURESOURCES2"),
    ("DxgkDdiSetVirtualFunctionPauseState",    "DXGKDDI_SETVIRTUALFUNCTIONPAUSESTATE"),
    ("DxgkDdiOpenNativeFence",                 "DXGKDDI_OPENNATIVEFENCE"),
    ("DxgkDdiCloseNativeFence",                "DXGKDDI_CLOSENATIVEFENCE"),
    ("DxgkDdiSetNativeFenceLogBuffer",         "DXGKDDI_SETNATIVEFENCELOGBUFFER"),
    ("DxgkDdiUpdateNativeFenceLogs",           "DXGKDDI_UPDATENATIVEFENCELOGS"),
    ("DxgkDdiCollectDbgInfo2",                 "DXGKDDI_COLLECTDBGINFO2"),
    ("DxgkDdiNotifyContextPriorityChange",     "DXGKDDI_NOTIFYCONTEXTPRIORITYCHANGE"),
    ("DxgkDdiResetDisplayEngine",              "DXGKDDI_RESETDISPLAYENGINE"),
]


# Fields that should be NULL (not stubbed) for a render-only WDDM driver.
#
# dxgk's AddAdapter (ADAPTER_DISPLAY::CreateDisplayCore, dxgkrnl
# 10.0.26100.8655) sees the classic VidPN DDI set NULL and takes the
# displayless-adapter path, which then enforces an ALL-OR-NONE sweep:
# every display-family DDI in the adapter table must be NULL or the
# whole AddAdapter fails with STATUS_REVISION_MISMATCH (0xC0000059,
# DxgKrnl-Admin event 549, StartAdapter_AddAdapterFailed) AFTER a fully
# successful StartDevice + QAI sweep. A "harmless" NOT_SUPPORTED stub
# makes the adapter look partially display-capable = incoherent driver
# model = rollback.
#
# RULE: a render-only KMD leaves EVERY display-related DDI NULL. When a
# future WDK adds new display DDIs, they go straight into this set,
# never into the stub list.
NULL_FIELDS = {
    # Cursor / palette / pointer
    "DxgkDdiSetPalette",
    "DxgkDdiSetPointerPosition",
    "DxgkDdiSetPointerShape",
    # VidPN topology (we have no display targets)
    "DxgkDdiIsSupportedVidPn",
    "DxgkDdiRecommendFunctionalVidPn",
    "DxgkDdiEnumVidPnCofuncModality",
    "DxgkDdiSetVidPnSourceAddress",
    "DxgkDdiSetVidPnSourceVisibility",
    "DxgkDdiCommitVidPn",
    "DxgkDdiUpdateActiveVidPnPresentPath",
    "DxgkDdiRecommendMonitorModes",
    "DxgkDdiRecommendVidPnTopology",
    "DxgkDdiGetScanLine",
    "DxgkDdiStopCapture",
    "DxgkDdiQueryVidPnHWCapability",
    # Overlay
    "DxgkDdiCreateOverlay",
    "DxgkDdiUpdateOverlay",
    "DxgkDdiFlipOverlay",
    "DxgkDdiDestroyOverlay",
    # Linked Display Adapter
    "DxgkDdiLinkDevice",
    "DxgkDdiSetDisplayPrivateDriverFormat",
    # PnP stop / post-display ownership / system display
    "DxgkDdiStopDeviceAndReleasePostDisplayOwnership",
    "DxgkDdiSystemDisplayEnable",
    "DxgkDdiSystemDisplayWrite",
    # Child container (no children)
    "DxgkDdiGetChildContainerId",
    # Per-monitor signaling we don't generate
    "DxgkDdiUpdateMonitorLinkInfo",
    "DxgkDdiSetTargetAdjustedColorimetry",
    "DxgkDdiDisplayDetectControl",
    "DxgkDdiQueryConnectionChange",
    # Display-family DDIs caught by the CreateDisplayCore all-or-none
    # sweep (live-KD confirmed: these four stubs at +0x488..+0x4A0 of
    # the adapter table were the lone survivors making AddAdapter fail).
    "DxgkDdiSetTimingsFromVidPn",
    "DxgkDdiSetTargetGamma",
    "DxgkDdiSetTargetContentType",
    "DxgkDdiSetTargetAnalogCopyProtection",
    "DxgkDdiSetTargetAdjustedColorimetry2",
    # Multi-plane overlay family (display path only)
    "DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay",
    "DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay2",
    "DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3",
    "DxgkDdiCheckMultiPlaneOverlaySupport",
    "DxgkDdiCheckMultiPlaneOverlaySupport2",
    "DxgkDdiCheckMultiPlaneOverlaySupport3",
    "DxgkDdiPostMultiPlaneOverlayPresent",
    "DxgkDdiGetMultiPlaneOverlayCaps",
    "DxgkDdiGetPostCompositionCaps",
    # Periodic frame notification / mode behavior (display path only)
    "DxgkDdiCreatePeriodicFrameNotification",
    "DxgkDdiDestroyPeriodicFrameNotification",
    "DxgkDdiControlModeBehavior",
    # Pre-start display info exchange - part of the sweep (+0x318)
    "DxgkDdiExchangePreStartInfo",
    # WDDM 2.9+ flip-queue / display-engine set. Render-only: no flips,
    # no display engine. Same "NULL means capability absent" rule as the
    # VidPN set above - a stub here is a false claim dxgk may act on.
    "DxgkDdiSetFlipQueueLogBuffer",
    "DxgkDdiUpdateFlipQueueLog",
    "DxgkDdiCancelQueuedFlips",
    "DxgkDdiSetInterruptTargetPresentId",
    "DxgkDdiCancelFlips",
    "DxgkDdiResetDisplayEngine",
}

# Hand-curated map for stubs we DO NOT auto-generate but instead wire to
# specific helpers (e.g. DxgkDdiEscape) — currently empty.
HAND_WIRED = {}


# ----------------------------------------------------------------------------
# Parser
# ----------------------------------------------------------------------------

# Match the typedef declaration of a DDI typedef:
#   typedef
#       _Check_return_
#       _Function_class_DXGK_(DXGKDDI_X)
#       _IRQL_requires_(PASSIVE_LEVEL)
#   NTSTATUS
#   APIENTRY
#   DXGKDDI_X(
#       args...
#       );
#
# Or for VOID:
#   typedef
#       _Function_class_DXGK_(DXGKDDI_X)
#   VOID
#   APIENTRY
#   DXGKDDI_X(
#       args...
#       );
TYPEDEF_OPEN = re.compile(r"^typedef\s*$")
APIENTRY_NAME = re.compile(r"^APIENTRY\s*$")
TYPEDEF_NAME_OPEN = re.compile(r"^([A-Za-z_][A-Za-z0-9_]+)\s*\(\s*$")
RETURN_TYPE_LINE = re.compile(r"^(NTSTATUS|VOID|BOOLEAN|ULONG|UINT|LONG|HANDLE|SIZE_T|PVOID|HRESULT)\s*$")


def parse_typedefs(text):
    """Return dict: typedef_name -> {rettype, args}."""
    typedefs = {}
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        if TYPEDEF_OPEN.match(lines[i]):
            # Walk forward up to ~12 lines to find the pattern:
            #   <rettype>
            #   APIENTRY
            #   NAME(
            j = i + 1
            rettype = None
            name = None
            args_start = None
            window = min(i + 20, len(lines))
            while j < window:
                line = lines[j].rstrip()
                if RETURN_TYPE_LINE.match(line):
                    rettype = line.strip()
                elif APIENTRY_NAME.match(line):
                    pass
                elif rettype is not None:
                    m = TYPEDEF_NAME_OPEN.match(line)
                    if m:
                        name = m.group(1)
                        args_start = j + 1
                        break
                j += 1
            if name and args_start is not None:
                # Read args until ');' - either on its own line, or glued
                # to the last parameter (e.g. DXGKDDI_COLLECTDIAGNOSTICINFO
                # in the 26100 dispmprt.h ends "... pCollectDiagnosticInfo);").
                arg_lines = []
                k = args_start
                while k < len(lines):
                    line = lines[k]
                    if re.match(r"^\s*\)\s*;\s*$", line):
                        break
                    m_inline = re.match(r"^(.*\S)\s*\)\s*;\s*$", line)
                    if m_inline:
                        arg_lines.append(m_inline.group(1))
                        break
                    arg_lines.append(line)
                    k += 1
                args_text = "\n".join(arg_lines).rstrip()
                # Only strip the trailing comma at the very end (last param), not per line.
                args_text = re.sub(r",\s*$", "", args_text)
                typedefs[name] = {"rettype": rettype, "args": args_text}
                i = k + 1
                continue
        i += 1
    return typedefs


def emit_stub(typedef_name, stub_func_name, info):
    rettype = info["rettype"]
    args = info["args"]

    # Extract parameter names for UNREFERENCED_PARAMETER.
    # Some params span multiple lines (long IN_CONST_PDXGKARG_X macros
    # often break before the variable name), so we can't split per-line.
    # Split on top-level commas instead, then take the last identifier
    # from each resulting decl.
    flat = re.sub(r"//.*", "", args)               # strip // comments
    flat = re.sub(r"/\*.*?\*/", "", flat, flags=re.DOTALL)
    # Collapse whitespace
    flat = re.sub(r"\s+", " ", flat).strip()
    # Split at commas that are NOT inside parens (the param list itself
    # is already inside the outer parens which we stripped).
    parts = []
    depth = 0
    cur = []
    for ch in flat:
        if ch == "(":
            depth += 1; cur.append(ch)
        elif ch == ")":
            depth -= 1; cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur).strip()); cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur).strip())

    param_names = []
    for p in parts:
        if not p:
            continue
        tokens = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", p)
        if not tokens:
            continue
        param_names.append(tokens[-1])

    # Body: ENTER print, UNREFERENCED_PARAMETERs, EXIT print, return.
    # Print spam is fine for now (user explicitly OK'd it) so we can prove
    # which stubs the kernel actually walks vs which it leaves alone.
    short = stub_func_name.replace("WinMaliKmdStub_", "")
    body_lines = ["{"]
    body_lines.append(f'    DbgPrint("[WinMali] >> {short}\\n");')
    for p in param_names:
        body_lines.append(f"    UNREFERENCED_PARAMETER({p});")
    if rettype == "NTSTATUS":
        body_lines.append(f'    DbgPrint("[WinMali] << {short} STATUS_NOT_SUPPORTED\\n");')
        body_lines.append("    return STATUS_NOT_SUPPORTED;")
    elif rettype == "BOOLEAN":
        body_lines.append(f'    DbgPrint("[WinMali] << {short} FALSE\\n");')
        body_lines.append("    return FALSE;")
    elif rettype == "VOID":
        body_lines.append(f'    DbgPrint("[WinMali] << {short}\\n");')
    elif rettype == "SIZE_T":
        body_lines.append(f'    DbgPrint("[WinMali] << {short} 0\\n");')
        body_lines.append("    return 0;")
    else:
        body_lines.append(f'    DbgPrint("[WinMali] << {short} 0\\n");')
        body_lines.append(f"    return ({rettype})0;")
    body_lines.append("}")
    body = "\n".join(body_lines)

    return f"""_Function_class_({typedef_name})
{rettype}
APIENTRY
{stub_func_name}(
{args}
    )
{body}
"""


def emit_proto(typedef_name, stub_func_name, info):
    rettype = info["rettype"]
    args = info["args"]
    return f"{rettype} APIENTRY {stub_func_name}(\n{args}\n    );\n"


def main():
    if not DISPMPRT_H.exists() or not D3DKMDDI_H.exists():
        print(f"ERR: WDK headers not at {WDK_INCLUDE}", file=sys.stderr)
        sys.exit(1)

    text = DISPMPRT_H.read_text() + "\n" + D3DKMDDI_H.read_text()
    typedefs = parse_typedefs(text)

    # Verify every typedef we want is present
    missing = []
    for _, t in INIT_FIELDS:
        if t not in typedefs:
            missing.append(t)
    if missing:
        print("Missing typedefs (parse failure?):", file=sys.stderr)
        for m in missing:
            print(f"  {m}", file=sys.stderr)
        sys.exit(2)

    here = Path(__file__).resolve().parent
    kmd_dir = here.parent / "KMD"

    stubs_c = ['#include "WinMaliKmd.h"', '#include "WinMaliDxgkStubs.h"', '']
    stubs_h = ['#pragma once', '#include "WinMaliKmd.h"', '']
    wire_lines = []

    seen_typedefs = set()
    for field_name, typedef_name in INIT_FIELDS:
        if field_name in EXISTING:
            wired = EXISTING[field_name]
            if wired:
                wire_lines.append(f"    init->{field_name:<55} = {wired};")
            continue

        if field_name in NULL_FIELDS:
            # Render-only driver -> dxgk wants these absent. Don't even
            # emit a stub body; just wire NULL.
            wire_lines.append(f"    init->{field_name:<55} = NULL;")
            continue

        stub_name = "WinMaliKmdStub_" + field_name.replace("DxgkDdi", "", 1)
        info = typedefs[typedef_name]

        # Only emit body once even if the same typedef is used twice
        # (e.g., DxgkDdiRender + DxgkDdiRenderKm both use DXGKDDI_RENDER).
        # Distinguish via stub_name.
        stubs_h.append(emit_proto(typedef_name, stub_name, info))
        stubs_c.append(emit_stub(typedef_name, stub_name, info))
        wire_lines.append(f"    init->{field_name:<55} = {stub_name};")

    (kmd_dir / "WinMaliDxgkStubs.c").write_text("\n".join(stubs_c))
    (kmd_dir / "WinMaliDxgkStubs.h").write_text("\n".join(stubs_h))
    (kmd_dir / "WinMaliDxgkStubsWire.h").write_text(
        "/* paste-in body for WinMaliDxgkPatchInitializationData() */\n\n"
        + "\n".join(wire_lines) + "\n")

    print(f"Generated {len(INIT_FIELDS)} fields:")
    print(f"  existing wires : {sum(1 for f,_ in INIT_FIELDS if f in EXISTING)}")
    print(f"  new stubs      : {sum(1 for f,_ in INIT_FIELDS if f not in EXISTING)}")
    print(f"Files in {kmd_dir}:")
    print("  WinMaliDxgkStubs.c")
    print("  WinMaliDxgkStubs.h")
    print("  WinMaliDxgkStubsWire.h")


if __name__ == "__main__":
    main()


#include "WinMaliKmd.h"
#include "WinMaliDxgkStubs.h"

_Function_class_(DXGKDDI_ACQUIRESWIZZLINGRANGE)
NTSTATUS
APIENTRY
WinMaliKmdStub_AcquireSwizzlingRange(
    IN_CONST_HANDLE                         hAdapter,
    INOUT_PDXGKARG_ACQUIRESWIZZLINGRANGE    pAcquireSwizzlingRange
    )
{
    DbgPrint("[WinMali] >> AcquireSwizzlingRange\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pAcquireSwizzlingRange);
    DbgPrint("[WinMali] << AcquireSwizzlingRange STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RELEASESWIZZLINGRANGE)
NTSTATUS
APIENTRY
WinMaliKmdStub_ReleaseSwizzlingRange(
    IN_CONST_HANDLE                             hAdapter,
    IN_CONST_PDXGKARG_RELEASESWIZZLINGRANGE     pReleaseSwizzlingRange
    )
{
    DbgPrint("[WinMali] >> ReleaseSwizzlingRange\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pReleaseSwizzlingRange);
    DbgPrint("[WinMali] << ReleaseSwizzlingRange STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_PATCH)
NTSTATUS
APIENTRY
WinMaliKmdStub_Patch(
    IN_CONST_HANDLE             hAdapter,
    IN_CONST_PDXGKARG_PATCH     pPatch
    )
{
    /* dxgk calls Patch to resolve allocation references in a DMA buffer before
       it is submitted - notably the present DMA buffer built by DxgkDdiPresent
       (src/dst allocations). Returning NOT_SUPPORTED here fails the whole
       submit right after our synchronous present copy, so present never
       completes. This driver does NOT need real address patching: the present
       blt is done on the CPU in DxgkDdiPresent, and render submits use GPU VAs
       through SubmitCommandVirtual (no patch locations to resolve). So this is
       a valid no-op - acknowledge success and let the pipeline proceed. */
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pPatch);
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_PREEMPTCOMMAND)
NTSTATUS
APIENTRY
WinMaliKmdStub_PreemptCommand(
    IN_CONST_HANDLE                     hAdapter,
    IN_CONST_PDXGKARG_PREEMPTCOMMAND    pPreemptCommand
    )
{
    DbgPrint("[WinMali] >> PreemptCommand\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pPreemptCommand);
    DbgPrint("[WinMali] << PreemptCommand STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RENDER)
NTSTATUS
APIENTRY
WinMaliKmdStub_Render(
    IN_CONST_HANDLE         hContext,
    INOUT_PDXGKARG_RENDER   pRender
    )
{
    DbgPrint("[WinMali] >> Render\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pRender);
    DbgPrint("[WinMali] << Render STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_PRESENT)
NTSTATUS
APIENTRY
WinMaliKmdStub_Present(
    IN_CONST_HANDLE         hContext,
    INOUT_PDXGKARG_PRESENT  pPresent
    )
{
    DbgPrint("[WinMali] >> Present\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pPresent);
    DbgPrint("[WinMali] << Present STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RENDER)
NTSTATUS
APIENTRY
WinMaliKmdStub_RenderKm(
    IN_CONST_HANDLE         hContext,
    INOUT_PDXGKARG_RENDER   pRender
    )
{
    DbgPrint("[WinMali] >> RenderKm\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pRender);
    DbgPrint("[WinMali] << RenderKm STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDISETPOWERCOMPONENTFSTATE)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetPowerComponentFState(
    IN_CONST_HANDLE DriverContext,
    UINT            ComponentIndex,
    UINT            FState
    )
{
    DbgPrint("[WinMali] >> SetPowerComponentFState\n");
    UNREFERENCED_PARAMETER(DriverContext);
    UNREFERENCED_PARAMETER(ComponentIndex);
    UNREFERENCED_PARAMETER(FState);
    DbgPrint("[WinMali] << SetPowerComponentFState STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_QUERYDEPENDENTENGINEGROUP)
NTSTATUS
APIENTRY
WinMaliKmdStub_QueryDependentEngineGroup(
    IN_CONST_HANDLE                             hAdapter,
    INOUT_DXGKARG_QUERYDEPENDENTENGINEGROUP     pQueryDependentEngineGroup
    )
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pQueryDependentEngineGroup == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* One engine per node; resetting a node affects only that node. */
    pQueryDependentEngineGroup->DependentNodeOrdinalMask =
        (1ULL << pQueryDependentEngineGroup->NodeOrdinal);
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_QUERYENGINESTATUS)
NTSTATUS
APIENTRY
WinMaliKmdStub_QueryEngineStatus(
    IN_CONST_HANDLE                     hAdapter,
    INOUT_PDXGKARG_QUERYENGINESTATUS    pQueryEngineStatus
    )
{
    DbgPrint("[WinMali] >> QueryEngineStatus\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pQueryEngineStatus);
    DbgPrint("[WinMali] << QueryEngineStatus STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RESETENGINE)
NTSTATUS
APIENTRY
WinMaliKmdStub_ResetEngine(
    IN_CONST_HANDLE             hAdapter,
    INOUT_PDXGKARG_RESETENGINE  pResetEngine
    )
{
    DbgPrint("[WinMali] >> ResetEngine\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pResetEngine);
    DbgPrint("[WinMali] << ResetEngine STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CANCELCOMMAND)
NTSTATUS
APIENTRY
WinMaliKmdStub_CancelCommand(
    IN_CONST_HANDLE                 hAdapter,
    IN_CONST_PDXGKARG_CANCELCOMMAND pCancelCommand
    )
{
    DbgPrint("[WinMali] >> CancelCommand\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCancelCommand);
    DbgPrint("[WinMali] << CancelCommand STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDIPOWERRUNTIMECONTROLREQUEST)
NTSTATUS
APIENTRY
WinMaliKmdStub_PowerRuntimeControlRequest(
    IN_CONST_HANDLE DriverContext,
    IN              LPCGUID PowerControlCode,
    IN OPTIONAL     PVOID InBuffer,
    IN              SIZE_T InBufferSize,
    OUT OPTIONAL    PVOID OutBuffer,
    IN              SIZE_T OutBufferSize,
    OUT OPTIONAL    PSIZE_T BytesReturned
    )
{
    DbgPrint("[WinMali] >> PowerRuntimeControlRequest\n");
    UNREFERENCED_PARAMETER(DriverContext);
    UNREFERENCED_PARAMETER(PowerControlCode);
    UNREFERENCED_PARAMETER(InBuffer);
    UNREFERENCED_PARAMETER(InBufferSize);
    UNREFERENCED_PARAMETER(OutBuffer);
    UNREFERENCED_PARAMETER(OutBufferSize);
    UNREFERENCED_PARAMETER(BytesReturned);
    DbgPrint("[WinMali] << PowerRuntimeControlRequest STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_NOTIFY_SURPRISE_REMOVAL)
NTSTATUS
APIENTRY
WinMaliKmdStub_NotifySurpriseRemoval(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGK_SURPRISE_REMOVAL_TYPE RemovalType
    )
{
    DbgPrint("[WinMali] >> NotifySurpriseRemoval\n");
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(RemovalType);
    DbgPrint("[WinMali] << NotifySurpriseRemoval STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDISETPOWERPSTATE)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetPowerPState(
    IN_CONST_HANDLE DriverContext,
    UINT ComponentIndex,
    UINT RequestedComponentPState
    )
{
    DbgPrint("[WinMali] >> SetPowerPState\n");
    UNREFERENCED_PARAMETER(DriverContext);
    UNREFERENCED_PARAMETER(ComponentIndex);
    UNREFERENCED_PARAMETER(RequestedComponentPState);
    DbgPrint("[WinMali] << SetPowerPState STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CONTROLINTERRUPT2)
NTSTATUS
APIENTRY
WinMaliKmdStub_ControlInterrupt2(
    IN_CONST_HANDLE                      hAdapter,
    IN_CONST_DXGKARG_CONTROLINTERRUPT2   InterruptControl
    )
{
    UNREFERENCED_PARAMETER(hAdapter);

    /*
     * ⚠ THREE VERSIONS OF THIS DDI EXISTED WITH THREE DIFFERENT ANSWERS:
     *   v1 (WinMaliDdi.c)  -- blanket STATUS_SUCCESS
     *   v2 (here)          -- blanket STATUS_NOT_SUPPORTED
     *   v3 (below)         -- selective, accepts the vsync types
     * dxgk should pick v3, but "should" is an assumption, and it is the
     * assumption that decides whether the vsync arm ever reaches us. If the arm
     * lands on v2, a blanket refusal reproduces exactly the retry storm that
     * flooded the log ring off the end of its buffer.
     *
     * So v2 mirrors v3's accept-set, and says which version was actually
     * called -- ONCE, because this DDI is polled and an unconditional print
     * here is what wrapped the ring in the first place.
     */
    {
        static BOOLEAN s_announced = FALSE;
        if (!s_announced) {
            s_announced = TRUE;
            DbgPrint("[WinMali]    ControlInterrupt2 IS THE ENTRY POINT dxgk CALLS "
                     "(type=%u)\n", (unsigned)InterruptControl.InterruptType);
        }
    }

    switch (InterruptControl.InterruptType) {
    case DXGK_INTERRUPT_CRTC_VSYNC:
    case DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY:
    case DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2:
    case DXGK_INTERRUPT_DISPLAYONLY_VSYNC:
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

_Function_class_(DXGKDDI_CALIBRATEGPUCLOCK)
NTSTATUS
APIENTRY
WinMaliKmdStub_CalibrateGpuClock(
    IN_CONST_HANDLE                             hAdapter,
    IN UINT32                                   NodeOrdinal,
    IN UINT32                                   EngineOrdinal,
    OUT_PDXGKARG_CALIBRATEGPUCLOCK	            pClockCalibration
    )
{
    LARGE_INTEGER qpc;
    LARGE_INTEGER freq;
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);
    UNREFERENCED_PARAMETER(EngineOrdinal);
    if (pClockCalibration == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* No dedicated GPU timestamp register is wired; model GPU time as the
       system performance counter (same clock the escape TimestampInfo uses).
       CPU == GPU counter -> identity correlation. */
    qpc = KeQueryPerformanceCounter(&freq);
    RtlZeroMemory(pClockCalibration, sizeof(*pClockCalibration));
    pClockCalibration->GpuFrequency    = (ULONGLONG)freq.QuadPart;
    pClockCalibration->GpuClockCounter = (ULONGLONG)qpc.QuadPart;
    pClockCalibration->CpuClockCounter = (ULONGLONG)qpc.QuadPart;
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_FORMATHISTORYBUFFER)
NTSTATUS
APIENTRY
WinMaliKmdStub_FormatHistoryBuffer(
    IN_CONST_HANDLE                 hContext,
    IN DXGKARG_FORMATHISTORYBUFFER* pFormatData
    )
{
    UNREFERENCED_PARAMETER(hContext);
    if (pFormatData == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* We advertise HistoryBufferPrecision, so dxgkrnl may ask us to format a
       GPU-timestamp history buffer on demand. We have no GPU-written history;
       present a well-formed empty formatted buffer. */
    if (pFormatData->pFormattedBuffer != NULL && pFormatData->FormattedBufferSize != 0) {
        RtlZeroMemory(pFormatData->pFormattedBuffer, pFormatData->FormattedBufferSize);
    }
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_RENDERGDI)
NTSTATUS
APIENTRY
WinMaliKmdStub_RenderGdi(
    IN_CONST_HANDLE          hContext,
    INOUT_PDXGKARG_RENDERGDI pRenderGdi
    )
{
    DbgPrint("[WinMali] >> RenderGdi\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pRenderGdi);
    DbgPrint("[WinMali] << RenderGdi STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

/* NOTE: DxgkDdiSubmitCommandVirtual is implemented for real as
   WinMaliKmdSubmitCommandVirtual (WinMaliDdi.c) and wired in
   WinMaliDxgkStubsWire.h - a GpuMmu driver submits DMA buffers through it, so
   a NOT_SUPPORTED stub here would bugcheck 0x119. The stub was removed. */

_Function_class_(DXGKDDI_MAPCPUHOSTAPERTURE)
NTSTATUS
APIENTRY
WinMaliKmdStub_MapCpuHostAperture(
    IN_CONST_HANDLE                      hAdapter,
    IN_CONST_PDXGKARG_MAPCPUHOSTAPERTURE pArgs
    )
{
    DbgPrint("[WinMali] >> MapCpuHostAperture\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << MapCpuHostAperture STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_UNMAPCPUHOSTAPERTURE)
NTSTATUS
APIENTRY
WinMaliKmdStub_UnmapCpuHostAperture(
    IN_CONST_HANDLE                         hAdapter,
    IN_CONST_PDXGKARG_UNMAPCPUHOSTAPERTURE  pArgs
    )
{
    DbgPrint("[WinMali] >> UnmapCpuHostAperture\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << UnmapCpuHostAperture STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_POWERRUNTIMESETDEVICEHANDLE)
NTSTATUS
APIENTRY
WinMaliKmdStub_PowerRuntimeSetDeviceHandle(
    IN_CONST_HANDLE DriverContext,
    IN HANDLE PoDeviceHandle
    )
{
    DbgPrint("[WinMali] >> PowerRuntimeSetDeviceHandle\n");
    UNREFERENCED_PARAMETER(DriverContext);
    UNREFERENCED_PARAMETER(PoDeviceHandle);
    DbgPrint("[WinMali] << PowerRuntimeSetDeviceHandle STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SETSTABLEPOWERSTATE)
VOID
APIENTRY
WinMaliKmdStub_SetStablePowerState(
    IN_CONST_HANDLE                        hAdapter,
    IN_CONST_PDXGKARG_SETSTABLEPOWERSTATE  pArgs
    )
{
    DbgPrint("[WinMali] >> SetStablePowerState\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << SetStablePowerState\n");
}

_Function_class_(DXGKDDI_SETVIDEOPROTECTEDREGION)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetVideoProtectedRegion(
    IN_CONST_HANDLE                             hAdapter,
    IN_CONST_PDXGKARG_SETVIDEOPROTECTEDREGION   pArgs
    )
{
    DbgPrint("[WinMali] >> SetVideoProtectedRegion\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << SetVideoProtectedRegion STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_VALIDATEUPDATEALLOCATIONPROPERTY)
NTSTATUS
APIENTRY
WinMaliKmdStub_ValidateUpdateAllocationProperty(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_VALIDATEUPDATEALLOCPROPERTY pValidateUpdateAllocProperty
    )
{
    DbgPrint("[WinMali] >> ValidateUpdateAllocationProperty\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pValidateUpdateAllocProperty);
    DbgPrint("[WinMali] << ValidateUpdateAllocationProperty STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CREATEHWCONTEXT)
NTSTATUS
APIENTRY
WinMaliKmdStub_CreateHwContext(
    IN_CONST_HANDLE                 hDevice,
    INOUT_PDXGKARG_CREATEHWCONTEXT  pCreateContext
    )
{
    DbgPrint("[WinMali] >> CreateHwContext\n");
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCreateContext);
    DbgPrint("[WinMali] << CreateHwContext STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_DESTROYHWCONTEXT)
NTSTATUS
APIENTRY
WinMaliKmdStub_DestroyHwContext(
    IN_CONST_HANDLE     hHwContext
    )
{
    DbgPrint("[WinMali] >> DestroyHwContext\n");
    UNREFERENCED_PARAMETER(hHwContext);
    DbgPrint("[WinMali] << DestroyHwContext STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CREATEHWQUEUE)
NTSTATUS
APIENTRY
WinMaliKmdStub_CreateHwQueue(
    IN_CONST_HANDLE                 hHwContext,
    INOUT_PDXGKARG_CREATEHWQUEUE    pCreateHwQueue
    )
{
    DbgPrint("[WinMali] >> CreateHwQueue\n");
    UNREFERENCED_PARAMETER(hHwContext);
    UNREFERENCED_PARAMETER(pCreateHwQueue);
    DbgPrint("[WinMali] << CreateHwQueue STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_DESTROYHWQUEUE)
NTSTATUS
APIENTRY
WinMaliKmdStub_DestroyHwQueue(
    IN_CONST_HANDLE     hHwQueue
    )
{
    DbgPrint("[WinMali] >> DestroyHwQueue\n");
    UNREFERENCED_PARAMETER(hHwQueue);
    DbgPrint("[WinMali] << DestroyHwQueue STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SUBMITCOMMANDTOHWQUEUE)
NTSTATUS
APIENTRY
WinMaliKmdStub_SubmitCommandToHwQueue(
    IN_CONST_HANDLE                             hAdapter,
    IN_CONST_PDXGKARG_SUBMITCOMMANDTOHWQUEUE    pSubmitCommandToHwQueue
    )
{
    DbgPrint("[WinMali] >> SubmitCommandToHwQueue\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSubmitCommandToHwQueue);
    DbgPrint("[WinMali] << SubmitCommandToHwQueue STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SWITCHTOHWCONTEXTLIST)
NTSTATUS
APIENTRY
WinMaliKmdStub_SwitchToHwContextList(
    IN_CONST_HANDLE                             hAdapter,
    IN_CONST_PDXGKARG_SWITCHTOHWCONTEXTLIST pHwContextList
    )
{
    DbgPrint("[WinMali] >> SwitchToHwContextList\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pHwContextList);
    DbgPrint("[WinMali] << SwitchToHwContextList STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RESETHWENGINE)
NTSTATUS
APIENTRY
WinMaliKmdStub_ResetHwEngine(
    IN_CONST_HANDLE                 hAdapter,
    INOUT_PDXGKARG_RESETHWENGINE    pResetHwEngine
    )
{
    DbgPrint("[WinMali] >> ResetHwEngine\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pResetHwEngine);
    DbgPrint("[WinMali] << ResetHwEngine STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_UPDATEHWCONTEXTSTATE)
NTSTATUS
APIENTRY
WinMaliKmdStub_UpdateHwContextState(
    IN_CONST_HANDLE                        hAdapter,
    IN_CONST_PDXGKARG_UPDATEHWCONTEXTSTATE pUpdateHwContextState
    )
{
    DbgPrint("[WinMali] >> UpdateHwContextState\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pUpdateHwContextState);
    DbgPrint("[WinMali] << UpdateHwContextState STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CREATEPROTECTEDSESSION)
NTSTATUS
APIENTRY
WinMaliKmdStub_CreateProtectedSession(
    IN_CONST_HANDLE                       hAdapter,
    INOUT_PDXGKARG_CREATEPROTECTEDSESSION pCreateProtectedSession
    )
{
    DbgPrint("[WinMali] >> CreateProtectedSession\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCreateProtectedSession);
    DbgPrint("[WinMali] << CreateProtectedSession STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_DESTROYPROTECTEDSESSION)
NTSTATUS
APIENTRY
WinMaliKmdStub_DestroyProtectedSession(
    IN_CONST_HANDLE                       hAdapter,
    IN_CONST_HANDLE                       hProtectedSession // in: Driver generated handle driver returned at DxgkDdiCreateProtectedSession.
    )
{
    DbgPrint("[WinMali] >> DestroyProtectedSession\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(hProtectedSession);
    DbgPrint("[WinMali] << DestroyProtectedSession STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SETSCHEDULINGLOGBUFFER)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetSchedulingLogBuffer(
    IN_CONST_HANDLE                             hAdapter,
    IN_CONST_PDXGKARG_SETSCHEDULINGLOGBUFFER    pSetSchedulingLogBuffer
    )
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pSetSchedulingLogBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Accept the per-node/engine scheduling log buffer. dxgkrnl allocates it and
       records CPU-side context-switch / profiling events there (this is exactly
       the buffer VidSchiProfilePerformanceTick writes to). We have no GPU-side
       markers to program (submission is escape-driven), so accepting is
       sufficient and lets dxgkrnl finish wiring its per-engine profiling state
       instead of leaving the per-engine log-buffer pointer NULL. */
    DbgPrint("[WinMali] SetSchedulingLogBuffer node=%u eng=%u entries=%u cpuVa=%p ACCEPT\n",
             pSetSchedulingLogBuffer->NodeOrdinal,
             pSetSchedulingLogBuffer->EngineOrdinal,
             pSetSchedulingLogBuffer->NumberOfEntries,
             (PVOID)pSetSchedulingLogBuffer->LogBufferCpuVa);
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_SETUPPRIORITYBANDS)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetupPriorityBands(
    IN_CONST_HANDLE                         hAdapter,
    IN_CONST_PDXGKARG_SETUPPRIORITYBANDS    pSetupPriorityBands
    )
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pSetupPriorityBands == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* We advertise priority scheduling in DRIVERCAPS.SchedulingCaps, so dxgkrnl
       configures its priority bands through us at init. We schedule uniformly
       (single CSG), so there is no hardware band state to program; accept the
       configuration so dxgkrnl's scheduler state is consistent with the caps we
       reported. */
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_NOTIFYFOCUSPRESENT)
NTSTATUS
APIENTRY
WinMaliKmdStub_NotifyFocusPresent(
    IN_CONST_HANDLE                         hAdapter
    )
{
    UNREFERENCED_PARAMETER(hAdapter);
    /* Informational: the focus window presented. Nothing to do; acknowledge. */
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_SETCONTEXTSCHEDULINGPROPERTIES)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetContextSchedulingProperties(
    IN_CONST_HANDLE                                     hAdapter,
    IN_CONST_PDXGKARG_SETCONTEXTSCHEDULINGPROPERTIES    pSetContextSchedulingProperties
    )
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pSetContextSchedulingProperties == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Accept per-context scheduling properties (priority band / quantum /
       grace period). Our scheduler treats contexts uniformly; record nothing
       but succeed so dxgkrnl considers the context fully schedulable. */
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_SUSPENDCONTEXT)
NTSTATUS
APIENTRY
WinMaliKmdStub_SuspendContext(
    IN_CONST_HANDLE                     hAdapter,
    IN_CONST_PDXGKARG_SUSPENDCONTEXT    pSuspendContext
    )
{
    DbgPrint("[WinMali] >> SuspendContext\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSuspendContext);
    DbgPrint("[WinMali] << SuspendContext STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RESUMECONTEXT)
NTSTATUS
APIENTRY
WinMaliKmdStub_ResumeContext(
    IN_CONST_HANDLE                 hAdapter,
    IN_CONST_PDXGKARG_RESUMECONTEXT pResumeContext
    )
{
    DbgPrint("[WinMali] >> ResumeContext\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pResumeContext);
    DbgPrint("[WinMali] << ResumeContext STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SETVIRTUALMACHINEDATA)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetVirtualMachineData(
    IN_CONST_HANDLE                         hAdapter,
    IN_CONST_PDXGKARG_SETVIRTUALMACHINEDATA Args
    )
{
    DbgPrint("[WinMali] >> SetVirtualMachineData\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Args);
    DbgPrint("[WinMali] << SetVirtualMachineData STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_BEGINEXCLUSIVEACCESS)
NTSTATUS
APIENTRY
WinMaliKmdStub_BeginExclusiveAccess(
    IN_CONST_HANDLE                  hAdapter,
    IN_PDXGKARG_BEGINEXCLUSIVEACCESS pBeginExclusiveAccess
    )
{
    DbgPrint("[WinMali] >> BeginExclusiveAccess\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pBeginExclusiveAccess);
    DbgPrint("[WinMali] << BeginExclusiveAccess STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_ENDEXCLUSIVEACCESS)
NTSTATUS
APIENTRY
WinMaliKmdStub_EndExclusiveAccess(
    IN_CONST_HANDLE                hAdapter,
    IN_PDXGKARG_ENDEXCLUSIVEACCESS pEndExclusiveAccess
    )
{
    DbgPrint("[WinMali] >> EndExclusiveAccess\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pEndExclusiveAccess);
    DbgPrint("[WinMali] << EndExclusiveAccess STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_QUERYDIAGNOSTICTYPESSUPPORT)
NTSTATUS
APIENTRY
WinMaliKmdStub_QueryDiagnosticTypesSupport(
    IN_CONST_PVOID                              MiniportDeviceContext,
    INOUT_PDXGKARG_QUERYDIAGNOSTICTYPESSUPPORT  pArgQueryDiagnosticTypesSupport
    )
{
    DbgPrint("[WinMali] >> QueryDiagnosticTypesSupport\n");
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(pArgQueryDiagnosticTypesSupport);
    DbgPrint("[WinMali] << QueryDiagnosticTypesSupport STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CONTROLDIAGNOSTICREPORTING)
NTSTATUS
APIENTRY
WinMaliKmdStub_ControlDiagnosticReporting(
    IN_CONST_PVOID                          MiniportDeviceContext,
    IN_PDXGKARG_CONTROLDIAGNOSTICREPORTING  pArgControlDiagnosticReporting
    )
{
    DbgPrint("[WinMali] >> ControlDiagnosticReporting\n");
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(pArgControlDiagnosticReporting);
    DbgPrint("[WinMali] << ControlDiagnosticReporting STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RESUMEHWENGINE)
NTSTATUS
APIENTRY
WinMaliKmdStub_ResumeHwEngine(
    IN_CONST_HANDLE                 hAdapter,
    INOUT_PDXGKARG_RESUMEHWENGINE   pResumeHwEngine
    )
{
    DbgPrint("[WinMali] >> ResumeHwEngine\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pResumeHwEngine);
    DbgPrint("[WinMali] << ResumeHwEngine STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SIGNALMONITOREDFENCE)
NTSTATUS
APIENTRY
WinMaliKmdStub_SignalMonitoredFence(
    IN_CONST_HANDLE                     hContext,
    INOUT_PDXGKARG_SIGNALMONITOREDFENCE pSignalMonitoredFence
    )
{
    DbgPrint("[WinMali] >> SignalMonitoredFence\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pSignalMonitoredFence);
    DbgPrint("[WinMali] << SignalMonitoredFence STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_PRESENTTOHWQUEUE)
NTSTATUS
APIENTRY
WinMaliKmdStub_PresentToHwQueue(
    IN_CONST_HANDLE         hHwQueue,
    INOUT_PDXGKARG_PRESENT  pPresent
    )
{
    DbgPrint("[WinMali] >> PresentToHwQueue\n");
    UNREFERENCED_PARAMETER(hHwQueue);
    UNREFERENCED_PARAMETER(pPresent);
    DbgPrint("[WinMali] << PresentToHwQueue STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_VALIDATESUBMITCOMMAND)
NTSTATUS
APIENTRY
WinMaliKmdStub_ValidateSubmitCommand(
    IN_CONST_HANDLE          hContext,
    INOUT_PDXGKARG_VALIDATESUBMITCOMMAND pArgs
    )
{
    DbgPrint("[WinMali] >> ValidateSubmitCommand\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << ValidateSubmitCommand STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SETTRACKEDWORKLOADPOWERLEVEL)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetTrackedWorkloadPowerLevel(
    IN_CONST_HANDLE                 hContext,
    INOUT_PDXGKARG_SETTRACKEDWORKLOADPOWERLEVEL   pTrackedWorkloadPowerLevel
    )
{
    DbgPrint("[WinMali] >> SetTrackedWorkloadPowerLevel\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pTrackedWorkloadPowerLevel);
    DbgPrint("[WinMali] << SetTrackedWorkloadPowerLevel STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SAVEMEMORYFORHOTUPDATE)
NTSTATUS
APIENTRY
WinMaliKmdStub_SaveMemoryForHotUpdate(
    IN_CONST_HANDLE                 hContext,
    IN_CONST_PDXGKARG_SAVEMEMORYFORHOTUPDATE pArgs
    )
{
    DbgPrint("[WinMali] >> SaveMemoryForHotUpdate\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << SaveMemoryForHotUpdate STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RESTOREMEMORYFORHOTUPDATE)
NTSTATUS
APIENTRY
WinMaliKmdStub_RestoreMemoryForHotUpdate(
    IN_CONST_HANDLE                             hContext,
    IN_CONST_PDXGKARG_RESTOREMEMORYFORHOTUPDATE pArgs
    )
{
    DbgPrint("[WinMali] >> RestoreMemoryForHotUpdate\n");
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << RestoreMemoryForHotUpdate STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_COLLECTDIAGNOSTICINFO)
NTSTATUS
APIENTRY
WinMaliKmdStub_CollectDiagnosticInfo(
    IN_CONST_PDEVICE_OBJECT PhysicalDeviceObject,
    INOUT_PDXGKARG_COLLECTDIAGNOSTICINFO pCollectDiagnosticInfo
    )
{
    DbgPrint("[WinMali] >> CollectDiagnosticInfo\n");
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    UNREFERENCED_PARAMETER(pCollectDiagnosticInfo);
    DbgPrint("[WinMali] << CollectDiagnosticInfo STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CONTROLINTERRUPT3)
NTSTATUS
APIENTRY
WinMaliKmdStub_ControlInterrupt3(
    IN_CONST_HANDLE                      hAdapter,
    IN_CONST_PDXGKARG_CONTROLINTERRUPT3  InterruptControl
    )
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (InterruptControl == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * ★★★★★★ THIS IS THE ARM/DISARM FOR THE VSYNC INTERRUPT, AND REFUSING IT WAS
     * ACTIVELY HARMFUL.
     *
     * It used to return STATUS_NOT_SUPPORTED unconditionally. Two consequences,
     * both measured:
     *
     *  1. We now declare FlipCaps.FlipOnVSyncMmIo, i.e. "I flip at the vsync
     *     interrupt". dxgk then tries to ARM that very interrupt through here.
     *     Promising the flip and refusing the interrupt it rides on is a
     *     contradiction, and the present path can never complete.
     *  2. dxgk RETRIES. The refusal loop flooded the driver log ring with
     *     >>/<< ControlInterrupt3 pairs and pushed the whole adapter-start
     *     sequence out of it -- which is why a ring capture came back holding
     *     2737 lines and not one DriverEntry.
     *
     * Accepting is honest for the interrupt types we can actually service, and
     * NOT_SUPPORTED stays the honest answer for the rest (an unknown type
     * silently "succeeding" would be a lie dxgk acts on).
     *
     * ⚠ We do not yet own a DISP/vsync line -- the panel is scanned out by the
     * framebuffer EDK2 lit. So this records intent; delivering actual
     * DXGK_INTERRUPT_CRTC_VSYNC notifications is the next piece of work.
     */
    switch (InterruptControl->InterruptType) {

    case DXGK_INTERRUPT_CRTC_VSYNC:
    case DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY:
    case DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2:
        /* ⚠ DXGK_VSYNC_ENABLE == 0 (d3dkmddi.h). Any `if (CrtcVsyncState)`
           test is therefore INVERTED. Always compare against the enum. */
        DbgPrint("[WinMali]    ControlInterrupt3: CRTC_VSYNC type=%u state=%u(%s) src=%u -> OK\n",
                 (unsigned)InterruptControl->InterruptType,
                 (unsigned)InterruptControl->CrtcVsyncState,
                 (InterruptControl->CrtcVsyncState == DXGK_VSYNC_ENABLE) ? "ENABLE" : "not-enable",
                 (unsigned)InterruptControl->VidPnSourceId);
        {
            PWINMALI_ADAPTER a = WinMaliAdapterFromDxgkHandle((HANDLE)hAdapter);
            if (a != NULL) {
                /* ⚠ DXGK_VSYNC_ENABLE == 0, so compare the enum -- never test
                   truthiness, which would invert the meaning exactly. */
                BOOLEAN enable =
                    (InterruptControl->CrtcVsyncState == DXGK_VSYNC_ENABLE);
                /* Timestamped, so the arm/disarm lines up against the moment the
                   OS switches our source off. A DISARM right before that switch-off
                   means dxgk gave up on the flip; an ARM still standing means it
                   never asked us to retire one. */
                /* See Parameters\VpLogVsync. These two consumed ~280 of 506
                   entries against a 512 cap, so the ring filled mid-boot and
                   dropped everything after t=43 s. */
                if (g_WinMaliVpLogVsync != 0) {
                    WinMaliVpLogOnly_(enable ? WINMALI_VPC_VSYNC_ON
                                             : WINMALI_VPC_VSYNC_OFF);
                }
                if (enable) { (VOID)InterlockedIncrement64(&a->VsyncArmCount); }
                if (g_WinMaliDodMode != 0) {
                    /* ⛔★★★★★ DISPLAY-ONLY: RECORD THE INTENT, KEEP THE
                     * HEARTBEAT RUNNING.
                     * A KMDOD reports DXGK_INTERRUPT_DISPLAYONLY_VSYNC (=5),
                     * which is NOT the interrupt dxgk is arming/disarming here.
                     * WinMaliVsyncDpc gates on VsyncEnabled alone, so obeying a
                     * CRTC_VSYNC disarm silences the DISPLAY-ONLY vsync too -
                     * and dxgk's DOD vsync watchdog then removes the adapter.
                     * MEASURED: BltQueueWakeReason=3 on our LUID, then
                     * StopDeviceAndReleasePostDisplayOwnership +80 ms and
                     * DdiRemoveDevice +98 ms, silently.
                     * ⛔ Do NOT "fix" this by raising TdrDodVSyncDelay - that
                     * watchdog is what keeps the display stack alive when the
                     * vsync stops; setting it to 900 s HUNG THE MACHINE and
                     * needed an offline registry repair from the USB spare. */
                    if (!enable) {
                        DbgPrint("[WinMali]    ControlInterrupt3: display-only - "
                                 "CRTC_VSYNC disarm NOTED, DISPLAYONLY_VSYNC "
                                 "heartbeat KEPT RUNNING\n");
                    } else {
                        WinMaliVsyncSetEnabled(a, TRUE);
                    }
                } else {
                    WinMaliVsyncSetEnabled(a, enable);
                }
            }
        }
        return STATUS_SUCCESS;

    case DXGK_INTERRUPT_DISPLAYONLY_VSYNC:
        DbgPrint("[WinMali]    ControlInterrupt3: DISPLAYONLY_VSYNC state=%u -> OK\n",
                 (unsigned)InterruptControl->InterruptState);
        return STATUS_SUCCESS;

    default:
        /* Log ONCE per type rather than per call: this DDI is polled, and the
           unconditional print here is what wrapped the ring. */
        {
            static ULONG s_loggedMask = 0;
            ULONG bit = 1u << (((ULONG)InterruptControl->InterruptType) & 31u);
            if ((s_loggedMask & bit) == 0) {
                s_loggedMask |= bit;
                DbgPrint("[WinMali]    ControlInterrupt3: type=%u unhandled -> NOT_SUPPORTED (logged once)\n",
                         (unsigned)InterruptControl->InterruptType);
            }
        }
        return STATUS_NOT_SUPPORTED;
    }
}

_Function_class_(DXGKDDI_SETALLOCATIONBACKINGSTORE)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetAllocationBackingStore(
    IN_CONST_HANDLE                 hAdapter,
    IN_CONST_PDXGKARG_SETALLOCATIONBACKINGSTORE pArgs
    )
{
    DbgPrint("[WinMali] >> SetAllocationBackingStore\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << SetAllocationBackingStore STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CREATECPUEVENT)
NTSTATUS
APIENTRY
WinMaliKmdStub_CreateCpuEvent(
    IN_CONST_HANDLE             hAdapter,
    INOUT_PDXGKARG_CREATECPUEVENT pArgs
    )
{
    DbgPrint("[WinMali] >> CreateCpuEvent\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << CreateCpuEvent STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_DESTROYCPUEVENT)
NTSTATUS
APIENTRY
WinMaliKmdStub_DestroyCpuEvent(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_HANDLE hKmdCpuEvent
    )
{
    DbgPrint("[WinMali] >> DestroyCpuEvent\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(hKmdCpuEvent);
    DbgPrint("[WinMali] << DestroyCpuEvent STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CREATENATIVEFENCE)
NTSTATUS
APIENTRY
WinMaliKmdStub_CreateNativeFence(
    IN_CONST_HANDLE                     hAdapter,
    INOUT_PDXGKARG_CREATENATIVEFENCE    pCreateNativeFence
    )
{
    DbgPrint("[WinMali] >> CreateNativeFence\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCreateNativeFence);
    DbgPrint("[WinMali] << CreateNativeFence STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_DESTROYNATIVEFENCE)
NTSTATUS
APIENTRY
WinMaliKmdStub_DestroyNativeFence(
    INOUT_PDXGKARG_DESTROYNATIVEFENCE pDestroyNativeFence
    )
{
    DbgPrint("[WinMali] >> DestroyNativeFence\n");
    UNREFERENCED_PARAMETER(pDestroyNativeFence);
    DbgPrint("[WinMali] << DestroyNativeFence STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_UPDATEMONITOREDVALUES)
NTSTATUS
APIENTRY
WinMaliKmdStub_UpdateMonitoredValues(
    IN_CONST_PDXGKARG_UPDATEMONITOREDVALUES pUpdateMonitoredValues
    )
{
    DbgPrint("[WinMali] >> UpdateMonitoredValues\n");
    UNREFERENCED_PARAMETER(pUpdateMonitoredValues);
    DbgPrint("[WinMali] << UpdateMonitoredValues STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_UPDATECURRENTVALUESFROMCPU)
NTSTATUS
APIENTRY
WinMaliKmdStub_UpdateCurrentValuesFromCpu(
    IN_CONST_PDXGKARG_UPDATECURRENTVALUESFROMCPU pUpdateCurrentValuesFromCpu
    )
{
    DbgPrint("[WinMali] >> UpdateCurrentValuesFromCpu\n");
    UNREFERENCED_PARAMETER(pUpdateCurrentValuesFromCpu);
    DbgPrint("[WinMali] << UpdateCurrentValuesFromCpu STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CREATEDOORBELL)
NTSTATUS
APIENTRY
WinMaliKmdStub_CreateDoorbell(
    INOUT_PDXGKARG_CREATEDOORBELL pArgs
    )
{
    DbgPrint("[WinMali] >> CreateDoorbell\n");
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << CreateDoorbell STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CONNECTDOORBELL)
NTSTATUS
APIENTRY
WinMaliKmdStub_ConnectDoorbell(
    INOUT_PDXGKARG_CONNECTDOORBELL pArgs
    )
{
    DbgPrint("[WinMali] >> ConnectDoorbell\n");
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << ConnectDoorbell STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_DISCONNECTDOORBELL)
NTSTATUS
APIENTRY
WinMaliKmdStub_DisconnectDoorbell(
    INOUT_PDXGKARG_DISCONNECTDOORBELL pArgs
    )
{
    DbgPrint("[WinMali] >> DisconnectDoorbell\n");
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << DisconnectDoorbell STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_DESTROYDOORBELL)
NTSTATUS
APIENTRY
WinMaliKmdStub_DestroyDoorbell(
    INOUT_PDXGKARG_DESTROYDOORBELL pArgs
    )
{
    DbgPrint("[WinMali] >> DestroyDoorbell\n");
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << DestroyDoorbell STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_NOTIFYWORKSUBMISSION)
NTSTATUS
APIENTRY
WinMaliKmdStub_NotifyWorkSubmission(
    INOUT_PDXGKARG_NOTIFYWORKSUBMISSION pArgs
    )
{
    DbgPrint("[WinMali] >> NotifyWorkSubmission\n");
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << NotifyWorkSubmission STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CREATEMEMORYBASIS)
HANDLE
APIENTRY
WinMaliKmdStub_CreateMemoryBasis(
    IN_CONST_HANDLE                     hAdapter,
    IN_CONST_PDXGKARG_CREATEMEMORYBASIS pArgs
    )
{
    DbgPrint("[WinMali] >> CreateMemoryBasis\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << CreateMemoryBasis 0\n");
    return (HANDLE)0;
}

_Function_class_(DXGKDDI_DESTROYMEMORYBASIS)
NTSTATUS
APIENTRY
WinMaliKmdStub_DestroyMemoryBasis(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_HANDLE hMemoryBasis
    )
{
    DbgPrint("[WinMali] >> DestroyMemoryBasis\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(hMemoryBasis);
    DbgPrint("[WinMali] << DestroyMemoryBasis STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_STARTDIRTYTRACKING)
NTSTATUS
APIENTRY
WinMaliKmdStub_StartDirtyTracking(
    IN_CONST_HANDLE  hAdapter,
    IN_CONST_HANDLE  hMemoryBasis
    )
{
    DbgPrint("[WinMali] >> StartDirtyTracking\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(hMemoryBasis);
    DbgPrint("[WinMali] << StartDirtyTracking STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_STOPDIRTYTRACKING)
NTSTATUS
APIENTRY
WinMaliKmdStub_StopDirtyTracking(
    IN_CONST_HANDLE  hAdapter,
    IN_CONST_HANDLE  hMemoryBasis
    )
{
    DbgPrint("[WinMali] >> StopDirtyTracking\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(hMemoryBasis);
    DbgPrint("[WinMali] << StopDirtyTracking STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_QUERYDIRTYBITDATA)
NTSTATUS
APIENTRY
WinMaliKmdStub_QueryDirtyBitData(
    IN_CONST_HANDLE                     hAdapter,
    INOUT_PDXGKARG_QUERYDIRTYBITDATA    pArgs
    )
{
    DbgPrint("[WinMali] >> QueryDirtyBitData\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << QueryDirtyBitData STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_PREPARELIVEMIGRATION)
NTSTATUS
APIENTRY
WinMaliKmdStub_PrepareLiveMigration(
    IN_CONST_HANDLE                                 hAdapter,
    IN_CONST_PDXGKARG_GPUP_PREPARE_LIVE_MIGRATION   pArgs
    )
{
    DbgPrint("[WinMali] >> PrepareLiveMigration\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << PrepareLiveMigration STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SAVEIMMUTABLEMIGRATIONDATA)
NTSTATUS
APIENTRY
WinMaliKmdStub_SaveImmutableMigrationData(
    IN_CONST_HANDLE                                     hAdapter,
    INOUT_PDXGKARG_GPUP_SAVE_IMMUTABLE_MIGRATION_DATA   pArgs
    )
{
    DbgPrint("[WinMali] >> SaveImmutableMigrationData\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << SaveImmutableMigrationData STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SAVEMUTABLEMIGRATIONDATA)
NTSTATUS
APIENTRY
WinMaliKmdStub_SaveMutableMigrationData(
    IN_CONST_HANDLE                                    hAdapter,
    INOUT_PDXGKARG_GPUP_SAVE_MUTABLE_MIGRATION_DATA    pArgs
    )
{
    DbgPrint("[WinMali] >> SaveMutableMigrationData\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << SaveMutableMigrationData STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_ENDLIVEMIGRATION)
NTSTATUS
APIENTRY
WinMaliKmdStub_EndLiveMigration(
    IN_CONST_HANDLE hAdapter,
    UINT            vfIndex
    )
{
    DbgPrint("[WinMali] >> EndLiveMigration\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(vfIndex);
    DbgPrint("[WinMali] << EndLiveMigration STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RESTOREIMMUTABLEMIGRATIONDATA)
NTSTATUS
APIENTRY
WinMaliKmdStub_RestoreImmutableMigrationData(
    IN_CONST_HANDLE                                         hAdapter,
    IN_CONST_PDXGKARG_GPUP_RESTORE_IMMUTABLE_MIGRATION_DATA pArgs
    )
{
    DbgPrint("[WinMali] >> RestoreImmutableMigrationData\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << RestoreImmutableMigrationData STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_RESTOREMUTABLEMIGRATIONDATA)
NTSTATUS
APIENTRY
WinMaliKmdStub_RestoreMutableMigrationData(
    IN_CONST_HANDLE                                         hAdapter,
    IN_CONST_PDXGKARG_GPUP_RESTORE_MUTABLE_MIGRATION_DATA   pArgs
    )
{
    DbgPrint("[WinMali] >> RestoreMutableMigrationData\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << RestoreMutableMigrationData STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_WRITEVIRTUALIZEDINTERRUPT)
NTSTATUS
APIENTRY
WinMaliKmdStub_WriteVirtualizedInterrupt(
    IN_CONST_HANDLE                                 hAdapter,
    IN_CONST_PDXGKARG_GPUP_WRITE_VIRTUALIZED_MSIX   pArgs
    )
{
    DbgPrint("[WinMali] >> WriteVirtualizedInterrupt\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << WriteVirtualizedInterrupt STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SETVIRTUALGPURESOURCES2)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetVirtualGpuResources2(
    IN_CONST_HANDLE                             hAdapter,
    IN_CONST_PDXGKARG_SETVIRTUALGPURESOURCES2   pArgs
    )
{
    DbgPrint("[WinMali] >> SetVirtualGpuResources2\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << SetVirtualGpuResources2 STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SETVIRTUALFUNCTIONPAUSESTATE)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetVirtualFunctionPauseState(
    IN_CONST_HANDLE                                 hAdapter,
    IN_CONST_PDXGKARG_SETVIRTUALFUNCTIONPAUSESTATE  pArgs
    )
{
    DbgPrint("[WinMali] >> SetVirtualFunctionPauseState\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    DbgPrint("[WinMali] << SetVirtualFunctionPauseState STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_OPENNATIVEFENCE)
NTSTATUS
APIENTRY
WinMaliKmdStub_OpenNativeFence(
    IN_CONST_HANDLE                   hAdapter,
    INOUT_PDXGKARG_OPENNATIVEFENCE    pOpenNativeFence
    )
{
    DbgPrint("[WinMali] >> OpenNativeFence\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pOpenNativeFence);
    DbgPrint("[WinMali] << OpenNativeFence STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_CLOSENATIVEFENCE)
NTSTATUS
APIENTRY
WinMaliKmdStub_CloseNativeFence(
    IN_CONST_HANDLE                   hAdapter,
    INOUT_PDXGKARG_CLOSENATIVEFENCE   pCloseNativeFence
    )
{
    DbgPrint("[WinMali] >> CloseNativeFence\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCloseNativeFence);
    DbgPrint("[WinMali] << CloseNativeFence STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_SETNATIVEFENCELOGBUFFER)
NTSTATUS
APIENTRY
WinMaliKmdStub_SetNativeFenceLogBuffer(
    IN_CONST_PDXGKARG_SETNATIVEFENCELOGBUFFER pSetNativeFenceLogBuffer
    )
{
    DbgPrint("[WinMali] >> SetNativeFenceLogBuffer\n");
    UNREFERENCED_PARAMETER(pSetNativeFenceLogBuffer);
    DbgPrint("[WinMali] << SetNativeFenceLogBuffer STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_UPDATENATIVEFENCELOGS)
NTSTATUS
APIENTRY
WinMaliKmdStub_UpdateNativeFenceLogs(
    IN_CONST_PDXGKARG_UPDATENATIVEFENCELOGS pUpdateNativeFenceLog
    )
{
    DbgPrint("[WinMali] >> UpdateNativeFenceLogs\n");
    UNREFERENCED_PARAMETER(pUpdateNativeFenceLog);
    DbgPrint("[WinMali] << UpdateNativeFenceLogs STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_COLLECTDBGINFO2)
NTSTATUS
APIENTRY
WinMaliKmdStub_CollectDbgInfo2(
    IN_CONST_HANDLE                 hAdapter,
    INOUT_PDXGKARG_COLLECTDBGINFO2  pCollectDbgInfo2
    )
{
    DbgPrint("[WinMali] >> CollectDbgInfo2\n");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCollectDbgInfo2);
    DbgPrint("[WinMali] << CollectDbgInfo2 STATUS_NOT_SUPPORTED\n");
    return STATUS_NOT_SUPPORTED;
}

_Function_class_(DXGKDDI_NOTIFYCONTEXTPRIORITYCHANGE)
NTSTATUS
APIENTRY
WinMaliKmdStub_NotifyContextPriorityChange(
    IN_CONST_HANDLE                                 hAdapter,
    IN_CONST_PDXGKARG_NOTIFYCONTEXTPRIORITYCHANGE   pNotifyContextPriorityChange
    )
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (pNotifyContextPriorityChange == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Informational: a context's priority changed. We schedule uniformly, so
       there is nothing to reprogram; acknowledge so dxgkrnl proceeds. */
    return STATUS_SUCCESS;
}

/*
 * ======================================================================
 *  MULTIPLANE OVERLAY - CYCLE 1 PROBE
 *
 * ⛔★★★★★ WHY THIS EXISTS, AND WHY IT REFUSES EVERYTHING.
 *
 * DWM's steady-state composition is UNREACHABLE by a display miniport: the
 * composition surface lives only inside D3DKMT_FLIPMODEL_PRESENTHISTORYTOKEN,
 * PresentHistoryTokenOnly presents generate ZERO driver calls, and nothing in
 * the entire kernel DDI surface names a present-history token. Chasing it was
 * dead from the start.
 *
 * MPO is the ONE contractual channel where the OS hands a miniport N separate
 * buffers with placement rects and per-plane blend. That is exactly the shape
 * this driver already has: we hold dwm's 2944x1840 desktop and the 2944x144
 * taskbar strip as separate BOs and already CPU-composite two layers per sweep.
 * So MPO would replace our 0x93 scavenging heuristic with the OS TELLING us
 * which buffers to draw and where.
 *
 * ⚠ THIS BUILD DELIBERATELY CHANGES NO BEHAVIOUR. Check3 reports Supported=FALSE
 * for every configuration, so the OS composes exactly as it does today and we
 * cannot break the picture. The only question being asked is whether dxgkrnl
 * CALLS these at all on a BLT-model adapter with a software scanout - which no
 * amount of header reading can answer, because MPO presents arrive through a
 * separate thunk and our frozen present counter is not evidence either way.
 *
 * ⚠ SupportMultiPlaneOverlay sits IMMEDIATELY BESIDE SupportDirectFlip in
 * DXGK_DRIVERCAPS, and setting that one previously produced device PROBLEM 37.
 * Hence: caps gated on Parameters\QaiMpoCaps, and the DDIs implemented BEFORE
 * the cap is claimed - never the reverse, because claiming a cap makes dxgk call
 * a DDI that would otherwise be NULL.
 * ======================================================================
 */

_Function_class_(DXGKDDI_GETMULTIPLANEOVERLAYCAPS)
NTSTATUS
APIENTRY
WinMaliKmdGetMultiPlaneOverlayCaps(
    IN_CONST_HANDLE                          hAdapter,
    IN_OUT_PDXGKARG_GETMULTIPLANEOVERLAYCAPS pCaps
    )
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (pCaps == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)InterlockedIncrement(&g_WinMaliStatMpoCapsCalls);

    /* Two RGB planes at 1:1, no stretch, no YUV, no rotation. That is not a
       conservative guess - it is precisely what the present worker can honestly
       do: a row-wise CPU copy of one full-screen plane plus one strip, both at
       the panel's own pitch. Anything more would be advertising a scanout engine
       we do not have. */
    pCaps->MaxPlanes         = 2;
    pCaps->MaxRGBPlanes      = 2;
    pCaps->MaxYUVPlanes      = 0;
    pCaps->OverlayCaps.Value = 0;
    /* NO FLOAT LITERALS IN KERNEL CODE: MaxStretchFactor/MaxShrinkFactor are
       `float`, and assigning 1.0f pulls in _fltused, which fails the link
       (LNK2001) because a driver must not touch the FPU without an explicit
       state save. 1.0f is IEEE-754 0x3F800000; copy the bit pattern. */
    {
        static const ULONG kOneF = 0x3F800000u;
        RtlCopyMemory(&pCaps->MaxStretchFactor, &kOneF, sizeof(kOneF));
        RtlCopyMemory(&pCaps->MaxShrinkFactor,  &kOneF, sizeof(kOneF));
    }

    (VOID)InterlockedExchange(&g_WinMaliStatMpoCapsSrc, (LONG)pCaps->VidPnSourceId);
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_CHECKMULTIPLANEOVERLAYSUPPORT3)
NTSTATUS
APIENTRY
WinMaliKmdCheckMultiPlaneOverlaySupport3(
    IN_CONST_HANDLE                                hAdapter,
    IN_OUT_PDXGKARG_CHECKMULTIPLANEOVERLAYSUPPORT3 pCheck
    )
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (pCheck == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)InterlockedIncrement(&g_WinMaliStatMpoCheckCalls);
    (VOID)InterlockedExchange(&g_WinMaliStatMpoCheckPlanes, (LONG)pCheck->PlaneCount);
    (VOID)InterlockedExchange(&g_WinMaliStatMpoCheckPostComp,
                              (LONG)pCheck->PostCompositionCount);

    /* ★ RECORD WHAT THE OS IS PROPOSING. If DWM ever offers us the taskbar as a
       plane, the plane count and the first plane's source id are what say so -
       and they are the whole reason this probe is worth a boot. */
    if (pCheck->PlaneCount != 0 && pCheck->ppPlanes != NULL &&
        pCheck->ppPlanes[0] != NULL) {
        (VOID)InterlockedExchange(&g_WinMaliStatMpoCheckSrc,
                                  (LONG)pCheck->ppPlanes[0]->VidPnSourceId);
    }

    /* Refuse every configuration this cycle. Supported=FALSE is a CONTRACTUAL
       answer, not a failure: the OS falls back to composing as it does today, so
       a wrong guess here cannot damage the picture. Cycle 2 turns this into real
       acceptance once we know the call even happens. */
    pCheck->Supported = FALSE;
    RtlZeroMemory(&pCheck->ReturnInfo, sizeof(pCheck->ReturnInfo));
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3)
NTSTATUS
APIENTRY
WinMaliKmdSetVidPnSourceAddressWithMultiPlaneOverlay3(
    IN_CONST_HANDLE                                             hAdapter,
    IN_OUT_PDXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3  pSet
    )
{
    UNREFERENCED_PARAMETER(hAdapter);

    /* TRIPWIRE ONLY. While Check3 refuses everything this must never be called;
       if it fires anyway, that is a finding about how dxgk sequences MPO, and it
       is far better to learn it from a counter than from a bugcheck. Retiring an
       MPO flip needs CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2, which we do not send
       yet - so accept and count, change nothing. */
    if (pSet == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    (VOID)InterlockedIncrement(&g_WinMaliStatMpoSetCalls);
    (VOID)InterlockedExchange(&g_WinMaliStatMpoSetPlanes, (LONG)pSet->PlaneCount);
    return STATUS_SUCCESS;
}

/* ⛔★★★★★ THE TWO SLOTS WHOSE ABSENCE CAUSED CODE 43.
 *
 * MEASURED 1.1.9.39: wiring Check3 + Set3 + GetMultiPlaneOverlayCaps and leaving
 * the rest NULL aborted adapter start before any capability query
 * (QueryCapsOutSz==0, QueryInterfaceCount 22->9), event 549
 * StartAdapter_AddAdapterFailed / STATUS_INVALID_PARAMETER.
 *
 * The MPO family is declared in CONTIGUOUS VERSION-GUARDED UNITS, and we split
 * both of them:
 *   dispmprt.h:2893-2895  Check3 + Set3 + PostMultiPlaneOverlayPresent   (WDDM2_1)
 *   dispmprt.h:2934-2935  GetMultiPlaneOverlayCaps + GetPostCompositionCaps (WDDM2_2)
 * We wired 2 of the first 3 and 1 of the second 2. Worse, BOTH v3 argument
 * structs carry post-composition payloads whose ONLY caps source is
 * GetPostCompositionCaps - which we left NULL, i.e. we accepted arguments we
 * had declared no ability to describe.
 *
 * ★ A DDI FAMILY IS A UNIT. Filling part of one is not a smaller claim, it is an
 *   INCOHERENT claim - and dxgk validates the table before it ever asks what we
 *   can do.
 */

_Function_class_(DXGKDDI_GETPOSTCOMPOSITIONCAPS)
NTSTATUS
APIENTRY
WinMaliKmdGetPostCompositionCaps(
    IN_CONST_HANDLE                        hAdapter,
    IN_OUT_PDXGKARG_GETPOSTCOMPOSITIONCAPS pCaps
    )
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (pCaps == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    (VOID)InterlockedIncrement(&g_WinMaliStatMpoPostCompCaps);

    /* No post-composition scaling: the worker copies row-for-row at the panel's
       own pitch. 1:1 both ways is the honest answer, and it is the same answer
       GetMultiPlaneOverlayCaps gives - the two MUST agree or the pair is as
       incoherent as the split family was.
       ⛔ Bit patterns, not float literals: 1.0f in kernel code pulls in _fltused
       and fails the link (LNK2001). */
    {
        static const ULONG kOneF = 0x3F800000u;
        RtlCopyMemory(&pCaps->MaxStretchFactor, &kOneF, sizeof(kOneF));
        RtlCopyMemory(&pCaps->MaxShrinkFactor,  &kOneF, sizeof(kOneF));
    }
    return STATUS_SUCCESS;
}

_Function_class_(DXGKDDI_POSTMULTIPLANEOVERLAYPRESENT)
NTSTATUS
APIENTRY
WinMaliKmdPostMultiPlaneOverlayPresent(
    IN_CONST_HANDLE                                hAdapter,
    IN_CONST_PDXGKARG_POSTMULTIPLANEOVERLAYPRESENT pPost
    )
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (pPost == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Completion notification for an MPO present. While Check3 refuses every
       configuration this should never fire; counting it costs nothing and makes
       "did the OS actually drive an MPO present" a number rather than a guess. */
    (VOID)InterlockedIncrement(&g_WinMaliStatMpoPostPresent);
    (VOID)InterlockedExchange(&g_WinMaliStatMpoPostLayer, (LONG)pPost->LayerIndex);
    return STATUS_SUCCESS;
}

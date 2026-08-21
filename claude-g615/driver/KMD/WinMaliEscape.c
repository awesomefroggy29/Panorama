/*
 * WinMaliEscape.c - DxgkDdiEscape dispatcher for the WinMali UMD<->KMD ABI.
 *
 * The ABI itself is defined in Shared/WinMaliEscape.h. Wire format:
 *
 *   [0]                       WINMALI_ESCAPE_HEADER  (magic, ABI, op, status)
 *   [sizeof(header)]          Op-specific argument struct
 *   [trailing region]         Variable-length tails (sync ops, vm-bind ops, ...)
 *                             addressed by byte offsets recorded in the args.
 *
 * The escape buffer originates in the UMD process via D3DKMTEscape with
 * Type=D3DKMT_ESCAPE_DRIVERPRIVATE. dxgkrnl probes the buffer into a
 * KM-readable mapping before handing it to us, but offsets inside the
 * buffer still come from user mode and we must bounds-check them before
 * dereferencing.
 *
 * Convention (mirroring pan_kmod_ioctl):
 *   - NTSTATUS return is STATUS_SUCCESS once we've recognised the buffer
 *     (i.e. magic OK). All errors land in hdr->Status as -errno.
 *   - Magic mismatch is the only path that returns a non-SUCCESS NTSTATUS,
 *     so foreign D3DKMT_ESCAPE buffers don't accidentally see hdr->Status
 *     overwritten.
 *
 * This file implements the foundation:
 *   Op 0x80 OpenDevice  - ABI handshake (validates major, returns identity)
 *   Op 0    DevQuery    - WinMaliDevQuery_GpuInfo (others stubbed)
 */

#include "WinMaliKmd.h"
#include "hw/WinMaliHw.h"
#include "WinMaliBo.h"
#include "WinMaliSync.h"
#include "WinMaliVm.h"
#include "WinMaliGroup.h"

/* Linux errno values - escape Status is -errno on failure. */
#define WMERR_OK          0
#define WMERR_EINVAL      -22
#define WMERR_ENOMEM      -12
#define WMERR_ENOENT      -2
#define WMERR_ENOSYS      -38
#define WMERR_EOPNOTSUPP  -95
#define WMERR_ENODEV      -19
#define WMERR_EBUSY       -16
#define WMERR_EIO         -5
#define WMERR_EAGAIN      -11

/* Identity string returned in WINMALI_OPEN_DEVICE. */
#define WINMALI_KMD_NAME    "WinMali-rk3588"

#define WM_QAI_OK_TRACE(name)  WINMALI_TRACE("Escape " name ": OK")

/* Build id - bumps when the dispatcher / op surface changes meaningfully.
   Compatibility is governed by ABI major/minor; this is purely a debug aid
   so a UMD can log "I'm talking to KMD build X". */
#define WINMALI_KMD_BUILD_ID  0x20260521u

/* Bounds-check that [offset, offset+size) lies inside the escape buffer.
   All offsets in our ABI are byte offsets from the start of
   pPrivateDriverData. UMD-controlled values - validate aggressively. */
static BOOLEAN
WinMaliEscapeRangeOk_(_In_ const DXGKARG_ESCAPE* pEscape,
                      _In_ ULONG offset,
                      _In_ ULONG size)
{
    ULONG end;
    if (size == 0) {
        return TRUE;
    }
    end = offset + size;
    if (end < offset) {           /* overflow */
        return FALSE;
    }
    if (end > pEscape->PrivateDriverDataSize) {
        return FALSE;
    }
    return TRUE;
}

/* Return a typed pointer to a substructure inside the escape buffer at the
   given byte offset, after verifying the range fits. NULL on failure. */
static PVOID
WinMaliEscapeAt_(_In_ const DXGKARG_ESCAPE* pEscape,
                 _In_ ULONG offset,
                 _In_ ULONG size)
{
    if (!WinMaliEscapeRangeOk_(pEscape, offset, size)) {
        return NULL;
    }
    return (PUCHAR)pEscape->pPrivateDriverData + offset;
}

/* -------------------------------------------------------------------------- */
/*  Op 0x80 - OpenDevice                                                       */
/* -------------------------------------------------------------------------- */

static int
WinMaliEscapeOpenDevice_(
    _In_    PWINMALI_ADAPTER       adapter,
    _Inout_ WINMALI_ESCAPE_HEADER* hdr,
    _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_OPEN_DEVICE* args;
    ULONG argOffset = sizeof(*hdr);

    UNREFERENCED_PARAMETER(adapter);

    args = (WINMALI_OPEN_DEVICE*)WinMaliEscapeAt_(pEscape, argOffset, sizeof(*args));
    if (args == NULL) {
        WINMALI_WARN("Escape OpenDevice: payload too small (size=%u, need >=%zu)",
                     pEscape->PrivateDriverDataSize, sizeof(*hdr) + sizeof(*args));
        return WMERR_EINVAL;
    }

    if (args->UmdAbiMajor != WINMALI_ABI_MAJOR) {
        WINMALI_WARN("Escape OpenDevice: UMD ABI major=%u, KMD major=%u",
                     args->UmdAbiMajor, WINMALI_ABI_MAJOR);
        return WMERR_EINVAL;
    }

    args->KmdAbiMajor = WINMALI_ABI_MAJOR;
    args->KmdAbiMinor = WINMALI_ABI_MINOR;
    args->KmdBuildId  = WINMALI_KMD_BUILD_ID;
    RtlZeroMemory(args->KmdName, sizeof(args->KmdName));
    RtlCopyMemory(args->KmdName, WINMALI_KMD_NAME, sizeof(WINMALI_KMD_NAME));

    WINMALI_TRACE("Escape OpenDevice: handshake OK (UMD %u.%u, KMD %u.%u, build 0x%08x)",
                  args->UmdAbiMajor, args->UmdAbiMinor,
                  args->KmdAbiMajor, args->KmdAbiMinor,
                  args->KmdBuildId);
    return WMERR_OK;
}

/* -------------------------------------------------------------------------- */
/*  Op 0 - DevQuery (Type = GpuInfo)                                           */
/* -------------------------------------------------------------------------- */

static int
WinMaliEscapeFillGpuInfo_(_In_  PWINMALI_ADAPTER  adapter,
                          _Out_ WINMALI_GPU_INFO* out)
{
    const WINMALI_HW* hw = &adapter->Hw;
    ULONG shaderLo = 0, shaderHi = 0;
    ULONG l2Lo = 0, l2Hi = 0;

    RtlZeroMemory(out, sizeof(*out));

    /* Cached at WinMaliHwProbeIdentity. */
    out->GpuId        = hw->GpuId;
    out->CsfId        = hw->CsfId;
    out->L2Features   = hw->L2Features;
    out->MmuFeatures  = hw->MmuFeatures;
    out->AsPresent    = hw->AsPresent;

    /* Re-read live registers for fields we don't cache. Safe even if regs
       aren't mapped (WinMaliHwRead32 returns 0). */
    if (hw->RegsVa != NULL) {
        shaderLo = WinMaliHwRead32(hw, WINMALI_REG_GPU_SHADER_PRESENT_LO);
        shaderHi = WinMaliHwRead32(hw, WINMALI_REG_GPU_SHADER_PRESENT_HI);
        l2Lo     = WinMaliHwRead32(hw, WINMALI_REG_GPU_L2_PRESENT_LO);
        l2Hi     = WinMaliHwRead32(hw, WINMALI_REG_GPU_L2_PRESENT_HI);

        /* Capability registers the UMD builds real state from. These were
           left ZERO until v1.0.0.37 with a "UMD falls back to defaults"
           assumption - WRONG for TilerFeatures: mesa derives the tiler
           hierarchy mask from it, and tiler_features==0 emitted
           hierarchy_mask=0 TILER_CONTEXT descriptors that the CS frontend
           rejects with DATA_INVALID_FAULT (0x58). That was the sole M2
           real-draw blocker (FAULT_INFO pointed at the tiler ctx). */
        out->TilerFeatures          = WinMaliHwRead32(hw, WINMALI_REG_GPU_TILER_FEATURES);
        out->MemFeatures            = WinMaliHwRead32(hw, WINMALI_REG_GPU_MEM_FEATURES);
        out->ThreadFeatures         = WinMaliHwRead32(hw, WINMALI_REG_GPU_THREAD_FEATURES);
        out->MaxThreads             = WinMaliHwRead32(hw, WINMALI_REG_GPU_THREAD_MAX_THREADS);
        out->ThreadMaxWorkgroupSize = WinMaliHwRead32(hw, WINMALI_REG_GPU_THREAD_MAX_WORKGROUP_SIZE);
        out->ThreadMaxBarrierSize   = WinMaliHwRead32(hw, WINMALI_REG_GPU_THREAD_MAX_BARRIER_SIZE);
        out->CoherencyFeatures      = WinMaliHwRead32(hw, WINMALI_REG_GPU_COHERENCY_FEATURES);
        out->TextureFeatures[0]     = WinMaliHwRead32(hw, WINMALI_REG_GPU_TEXTURE_FEATURES(0));
        out->TextureFeatures[1]     = WinMaliHwRead32(hw, WINMALI_REG_GPU_TEXTURE_FEATURES(1));
        out->TextureFeatures[2]     = WinMaliHwRead32(hw, WINMALI_REG_GPU_TEXTURE_FEATURES(2));
        out->TextureFeatures[3]     = WinMaliHwRead32(hw, WINMALI_REG_GPU_TEXTURE_FEATURES(3));
    }
    out->ShaderPresent = ((UINT64)shaderHi << 32) | shaderLo;
    out->L2Present     = ((UINT64)l2Hi << 32)     | l2Lo;

    WINMALI_TRACE("GpuInfo caps: tiler=0x%08x mem=0x%08x thread=0x%08x "
                  "max_thr=%u wg=%u barrier=%u coh=0x%08x tex0=0x%08x",
                  out->TilerFeatures, out->MemFeatures, out->ThreadFeatures,
                  out->MaxThreads, out->ThreadMaxWorkgroupSize,
                  out->ThreadMaxBarrierSize, out->CoherencyFeatures,
                  out->TextureFeatures[0]);

    /* TilerPresent is 0 on G610 (no separate tiler-present register).
       CorePresent is unused on v10 (MBZ per ABI doc). */
    return WMERR_OK;
}

static int
WinMaliEscapeDevQuery_(
    _In_    PWINMALI_ADAPTER       adapter,
    _Inout_ WINMALI_ESCAPE_HEADER* hdr,
    _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_DEV_QUERY* args;
    ULONG argOffset = sizeof(*hdr);

    args = (WINMALI_DEV_QUERY*)WinMaliEscapeAt_(pEscape, argOffset, sizeof(*args));
    if (args == NULL) {
        return WMERR_EINVAL;
    }

    switch (args->Type) {
    case WinMaliDevQuery_GpuInfo: {
        WINMALI_GPU_INFO info;
        WINMALI_GPU_INFO* dst;
        ULONG offset;

        if (args->PointerOffset > 0xFFFFFFFFu) {
            return WMERR_EINVAL;
        }
        offset = (ULONG)args->PointerOffset;

        /* Probe-size path: UMD sets Size=0 to discover required size. */
        if (args->Size == 0) {
            args->Size = sizeof(info);
            return WMERR_OK;
        }
        if (args->Size < sizeof(info)) {
            args->Size = sizeof(info);
            return WMERR_EINVAL;
        }
        dst = (WINMALI_GPU_INFO*)WinMaliEscapeAt_(pEscape, offset, sizeof(info));
        if (dst == NULL) {
            return WMERR_EINVAL;
        }
        (void)WinMaliEscapeFillGpuInfo_(adapter, &info);
        RtlCopyMemory(dst, &info, sizeof(info));
        args->Size = sizeof(info);
        WINMALI_TRACE("Escape DevQuery GpuInfo: GpuId=0x%08x CsfId=0x%08x shader=0x%llx",
                      info.GpuId, info.CsfId, (ULONGLONG)info.ShaderPresent);
        return WMERR_OK;
    }
    case WinMaliDevQuery_CsifInfo: {
        /* CSF interface dimensions. These match what the Mali G610's CSF
           firmware exposes via the shared interface page. Values here are
           the architecture defaults; ideally we'd read them from the live
           iface struct cached at WinMaliFwInit time, but until that's
           plumbed through, the constants are correct for G610. */
        WINMALI_CSIF_INFO info;
        WINMALI_CSIF_INFO* dst;
        ULONG offset;

        if (args->PointerOffset > 0xFFFFFFFFu) return WMERR_EINVAL;
        offset = (ULONG)args->PointerOffset;
        if (args->Size == 0) { args->Size = sizeof(info); return WMERR_OK; }
        if (args->Size < sizeof(info)) { args->Size = sizeof(info); return WMERR_EINVAL; }
        dst = (WINMALI_CSIF_INFO*)WinMaliEscapeAt_(pEscape, offset, sizeof(info));
        if (dst == NULL) return WMERR_EINVAL;

        RtlZeroMemory(&info, sizeof(info));
        /* Field order matches drm_panthor_csif_info (see WinMaliEscape.h).
           csg_slot, cs_slot, cs_reg, scoreboard, unpreserved, pad. */
        info.CsGroupCount          = 4;   /* csg_slot_count: 4 parallel CSGs */
        info.CsCount               = 8;   /* cs_slot_count: per-group CS */
        info.CsRegCount            = 96;  /* cs_reg_count: per-CS register file */
        info.ScoreboardSlotCount   = 8;   /* scoreboard_slot_count */
        info.CsUnpreservedRegCount = 4;   /* unpreserved_cs_reg_count (kernel-reserved) */
        RtlCopyMemory(dst, &info, sizeof(info));
        args->Size = sizeof(info);
        WM_QAI_OK_TRACE("DevQuery CsifInfo");
        return WMERR_OK;
    }

    case WinMaliDevQuery_TimestampInfo: {
        WINMALI_TIMESTAMP_INFO info;
        WINMALI_TIMESTAMP_INFO* dst;
        ULONG offset;
        LARGE_INTEGER perfCount, perfFreq;

        if (args->PointerOffset > 0xFFFFFFFFu) return WMERR_EINVAL;
        offset = (ULONG)args->PointerOffset;
        if (args->Size == 0) { args->Size = sizeof(info); return WMERR_OK; }
        if (args->Size < sizeof(info)) { args->Size = sizeof(info); return WMERR_EINVAL; }
        dst = (WINMALI_TIMESTAMP_INFO*)WinMaliEscapeAt_(pEscape, offset, sizeof(info));
        if (dst == NULL) return WMERR_EINVAL;

        /* Until the GPU timestamp register is wired, expose the system QPC
           as a stand-in. UMDs that compute relative deltas will work; ones
           that need GPU-side timestamps will need this revisited. */
        perfCount = KeQueryPerformanceCounter(&perfFreq);
        RtlZeroMemory(&info, sizeof(info));
        info.TimestampFrequency = (UINT64)perfFreq.QuadPart;
        info.CurrentTimestamp   = (UINT64)perfCount.QuadPart;
        info.TimestampOffset    = 0;
        RtlCopyMemory(dst, &info, sizeof(info));
        args->Size = sizeof(info);
        WM_QAI_OK_TRACE("DevQuery TimestampInfo");
        return WMERR_OK;
    }

    case WinMaliDevQuery_GroupPrioritiesInfo: {
        WINMALI_GROUP_PRIORITIES_INFO info;
        WINMALI_GROUP_PRIORITIES_INFO* dst;
        ULONG offset;

        if (args->PointerOffset > 0xFFFFFFFFu) return WMERR_EINVAL;
        offset = (ULONG)args->PointerOffset;
        if (args->Size == 0) { args->Size = sizeof(info); return WMERR_OK; }
        if (args->Size < sizeof(info)) { args->Size = sizeof(info); return WMERR_EINVAL; }
        dst = (WINMALI_GROUP_PRIORITIES_INFO*)WinMaliEscapeAt_(pEscape, offset, sizeof(info));
        if (dst == NULL) return WMERR_EINVAL;

        RtlZeroMemory(&info, sizeof(info));
        info.AllowedMask = 0x0F;  /* all four priority levels (Low..Realtime) */
        RtlCopyMemory(dst, &info, sizeof(info));
        args->Size = sizeof(info);
        WM_QAI_OK_TRACE("DevQuery GroupPrioritiesInfo");
        return WMERR_OK;
    }

    default:
        WINMALI_WARN("Escape DevQuery: unknown type %u", args->Type);
        return WMERR_EINVAL;
    }

    UNREFERENCED_PARAMETER(hdr);
}

static int
WinMaliEscapeBoCreate_(_In_    PWINMALI_ADAPTER       adapter,
                       _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                       _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_BO_CREATE* args =
        (WINMALI_BO_CREATE*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    ULONG handle = 0;
    int err;
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    {
        UINT64 actual = 0;
        err = WinMaliBoCreate(adapter, (SIZE_T)args->Size, args->Flags,
                              args->ExclusiveVmId,
                              WinMaliDeviceOwnerToken(pEscape->hDevice),
                              &handle, &actual);
        if (err == WMERR_OK) {
            args->Handle = handle;
            args->Pad    = 0;
            /* IN/OUT: hand back the size we really created, so the UMD records
               the truth instead of its request (see WINMALI_BO_CREATE). */
            args->Size   = actual;
        }
    }
    return err;
}

/* Op 0x81 - wrap a resident D3DKMT allocation (DXGI swapchain backbuffer /
   shared surface) as a panfrost BO so the UMD renders straight into the
   DWM-visible pages. The UMD pins the allocation with LockCb before calling
   (residency is what populates ApertureMdl via MAP_APERTURE_SEGMENT), so
   EAGAIN here means "not resident - lock it first". */
static int
WinMaliEscapeBoFromAllocation_(_In_    PWINMALI_ADAPTER       adapter,
                               _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                               _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_BO_FROM_ALLOCATION* args =
        (WINMALI_BO_FROM_ALLOCATION*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    PWINMALI_KMD_ALLOCATION ka;
    PPFN_NUMBER pfns;
    ULONG handle = 0;
    int err;

    if (args == NULL || args->AllocationHandle == 0) {
        return WMERR_EINVAL;
    }
    if (adapter->DxgkInterface.DxgkCbGetHandleData == NULL) {
        return WMERR_EOPNOTSUPP;
    }

    /* Resolve the caller's D3DKMT allocation handle to our CreateAllocation
       struct. The UMD routes THIS escape through the runtime device's
       pfnEscapeCb, so pEscape->hDevice is the same device that created the
       allocation and DxgkCbGetHandleData resolves the handle natively. */
    {
        DXGKARGCB_GETHANDLEDATA ghd;
        RtlZeroMemory(&ghd, sizeof(ghd));
        ghd.hObject = (D3DKMT_HANDLE)args->AllocationHandle;
        ghd.Type    = DXGK_HANDLE_ALLOCATION;
        ka = (PWINMALI_KMD_ALLOCATION)
            adapter->DxgkInterface.DxgkCbGetHandleData(&ghd);
    }
    if (ka == NULL || ka->Magic != WINMALI_KMD_ALLOC_MAGIC) {
        WINMALI_WARN("BoFromAllocation: hAlloc=0x%x did not resolve (ka=%p)",
                     args->AllocationHandle, ka);
        return WMERR_ENOENT;
    }
    if (ka->ApertureMdl == NULL || ka->AperturePageCount == 0) {
        WINMALI_WARN("BoFromAllocation: hAlloc=0x%x not resident (UMD must LockCb first)",
                     args->AllocationHandle);
        return WMERR_EAGAIN;
    }

    pfns = MmGetMdlPfnArray(ka->ApertureMdl);
    if (pfns == NULL) {
        return WMERR_EINVAL;
    }

    /* ✅ THIS IS CORRECT AS WRITTEN - DO NOT "FIX" IT BY SUBTRACTING THE OFFSET.
     *
     * I nearly shipped exactly that regression. The two fields are NOT
     * (total, start) - see WinMaliPaging.c:189:
     *     ApertureMdlOffset = MapApertureSegment.MdlOffset      (where the run
     *                                                            STARTS in the MDL)
     *     AperturePageCount = MapApertureSegment.NumberOfPages  (how many pages
     *                                                            are MAPPED)
     * so `pfns + MdlOffset` for `AperturePageCount` pages is precisely the
     * mapped run. Subtracting the offset from the count would under-map every
     * imported surface by MdlOffset pages - i.e. manufacture the very
     * "BO smaller than the resource" symptom we are chasing. */
    err = WinMaliBoCreateFromPfns(adapter,
                                  pfns + ka->ApertureMdlOffset,
                                  ka->AperturePageCount,
                                  args->Flags,
                                  WinMaliDeviceOwnerToken(pEscape->hDevice),
                                  &handle);
    if (err != WMERR_OK) {
        return err;
    }

    args->BoHandle = handle;
    args->Size     = (UINT64)ka->AperturePageCount << PAGE_SHIFT;
    args->GpuVa    = 0;   /* VmId==0: caller maps via VmBind */
    WINMALI_TRACE("BoFromAllocation: hAlloc=0x%x -> bo=%u pages=%u (%ux%u pitch=%u)",
                  args->AllocationHandle, handle, ka->AperturePageCount,
                  ka->Width, ka->Height, ka->Pitch);
    return WMERR_OK;
}

/*
 * Op 0x92 - BoFromAllocId.
 *
 * Identical outcome to BoFromAllocation (a BO wrapping the allocation's REAL
 * ApertureMdl pages, so the GPU renders into the surface the present worker
 * scans out) but resolved by the ADAPTER-GLOBAL AllocId instead of a D3DKMT
 * handle.
 *
 * ⛔ Why this op exists at all: 0x81 uses DxgkCbGetHandleData, which only
 * resolves handles belonging to the device the escape arrived on. The UMD's
 * escapes go over winmali's own private device, which never created the D3D
 * runtime's surfaces - and routing them through the runtime's device instead
 * CRASHES d3d11 inside pfnEscapeCb (measured: LogonUI <- d3d11.dll @0x8f240,
 * 53-55 faults against a 1-3 noise floor). AllocId sidesteps the whole problem:
 * it is the same identity that already makes OpenAllocation resolve 252/252.
 */
static int
WinMaliEscapeBoFromAllocId_(_In_    PWINMALI_ADAPTER       adapter,
                            _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                            _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_BO_FROM_ALLOC_ID* args =
        (WINMALI_BO_FROM_ALLOC_ID*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    PWINMALI_KMD_ALLOCATION ka;
    PPFN_NUMBER pfns;
    ULONG handle = 0;
    int err;

    if (args == NULL || (args->AllocId == 0 && args->AllocIdHi == 0)) {
        return WMERR_EINVAL;
    }

    /* ★ RECORD THE REJECTION REASON WHERE IT SURVIVES.
     *
     * The UMD already logs the errno, but that needs C:\mesalogs readable and
     * the box reachable - and the tablet has gone offline mid-measurement more
     * than once (a failing DOCK, not the driver). These counters are published
     * to Services\WinMaliKmd\Parameters by the present worker, so the answer is
     * available from a plain registry read on the next boot.
     *
     * The distinction decides the fix: ENOENT => the AllocId never reached the
     * KMD's id table; EAGAIN => it did, but MapApertureSegment has not run yet,
     * so the pages are not mapped at import time and wrapping must be deferred
     * (or back-filled when the aperture mapping happens). */
    (VOID)InterlockedIncrement(&g_WinMaliStat0x92Calls);

    ka = WinMaliAllocLookupById(adapter, args->AllocId, args->AllocIdHi);
    if (ka == NULL || ka->Magic != WINMALI_KMD_ALLOC_MAGIC) {
        (VOID)InterlockedIncrement(&g_WinMaliStat0x92NoEnt);
        WINMALI_WARN("BoFromAllocId: id=%u:%u did not resolve",
                     args->AllocIdHi, args->AllocId);
        return WMERR_ENOENT;
    }
    if (ka->ApertureMdl == NULL || ka->AperturePageCount == 0) {
        (VOID)InterlockedIncrement(&g_WinMaliStat0x92NotRes);
        WINMALI_WARN("BoFromAllocId: id=%u:%u not resident (mdl=%p pages=%u)",
                     args->AllocIdHi, args->AllocId,
                     (PVOID)ka->ApertureMdl, ka->AperturePageCount);
        return WMERR_EAGAIN;
    }
    (VOID)InterlockedIncrement(&g_WinMaliStat0x92Ok);

    pfns = MmGetMdlPfnArray(ka->ApertureMdl);
    if (pfns == NULL) {
        return WMERR_EINVAL;
    }

    /* ⚠ pfns + MdlOffset for the FULL AperturePageCount - the pair is
       (where the run starts, how many pages are mapped), NOT (start, total).
       Do not subtract the offset from the count; see WinMaliPaging.c. */
    err = WinMaliBoCreateFromPfns(adapter,
                                  pfns + ka->ApertureMdlOffset,
                                  ka->AperturePageCount,
                                  args->Flags,
                                  WinMaliDeviceOwnerToken(pEscape->hDevice),
                                  &handle);
    if (err != WMERR_OK) {
        return err;
    }

    args->BoHandle = handle;
    args->Size     = (UINT64)ka->AperturePageCount << PAGE_SHIFT;
    WINMALI_TRACE("BoFromAllocId: id=%u:%u -> bo=%u pages=%u (%ux%u pitch=%u)",
                  args->AllocIdHi, args->AllocId, handle, ka->AperturePageCount,
                  ka->Width, ka->Height, ka->Pitch);
    return WMERR_OK;
}

/*
 * Op 0x93 - BoFromUserVa.
 *
 * 🎯 The import path that does not need dxgk's cooperation.
 *
 * 0x81 and 0x92 both end at `ka->ApertureMdl`, and NOTHING the UMD can do fills
 * that in under escape submission: only MapApertureSegment does, and dxgk issues
 * it only for allocations it believes the GPU needs - which it learns from a DMA
 * buffer's allocation list or pfnMapGpuVirtualAddressCb, neither of which an
 * escape submit uses. Measured end state: 0x92 EAGAIN on every surface, every
 * boot, including after re-import; realPages 0, nonBlack 0.
 *
 * But the UMD already holds a CPU pointer to those exact frames - pfnLockCb
 * (LockEntire) returned one, and the pages stay resident until UnlockCb. So we
 * probe-and-lock that user range here and wrap the PFNs behind it. Same pages
 * the present worker scans out, no paging operation involved.
 */
static int
WinMaliEscapeBoFromUserVa_(_In_    PWINMALI_ADAPTER       adapter,
                           _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                           _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_BO_FROM_USER_VA* args =
        (WINMALI_BO_FROM_USER_VA*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    ULONG  handle = 0;
    UINT64 mapped = 0;
    int    err;

    UNREFERENCED_PARAMETER(hdr);

    if (args == NULL || args->UserVa == 0 || args->Size == 0) {
        return WMERR_EINVAL;
    }
    (VOID)InterlockedIncrement(&g_WinMaliStat0x93Calls);

    err = WinMaliBoCreateFromUserVa(adapter,
                                    (PVOID)(ULONG_PTR)args->UserVa,
                                    (SIZE_T)args->Size,
                                    args->Flags,
                                    /* Row pitch, or 0 from a UMD built before the
                                       field existed - the worker then falls back
                                       to assuming the panel pitch, which is the
                                       behaviour that produced the half-height
                                       smear. See WINMALI_BO_FROM_USER_VA. */
                                    args->SrcPitch,
                                    WinMaliDeviceOwnerToken(pEscape->hDevice),
                                    &handle,
                                    &mapped);
    if (err != WMERR_OK) {
        (VOID)InterlockedIncrement(&g_WinMaliStat0x93Fail);
        WINMALI_WARN("BoFromUserVa: va=0x%llx size=%llu failed err=%d",
                     args->UserVa, args->Size, err);
        return err;
    }
    (VOID)InterlockedIncrement(&g_WinMaliStat0x93Ok);

    args->BoHandle    = handle;
    args->MappedBytes = mapped;
    WINMALI_TRACE("BoFromUserVa: va=0x%llx size=%llu -> bo=%u mapped=%llu",
                  args->UserVa, args->Size, handle, mapped);
    return WMERR_OK;
}

static int
WinMaliEscapeBoDestroy_(_In_    PWINMALI_ADAPTER       adapter,
                        _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                        _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_BO_DESTROY* args =
        (WINMALI_BO_DESTROY*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    return WinMaliBoDestroy(adapter, args->Handle);
}

static int
WinMaliEscapeBoQueryInfo_(_In_    PWINMALI_ADAPTER       adapter,
                          _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                          _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_BO_QUERY_INFO* args =
        (WINMALI_BO_QUERY_INFO*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    PWINMALI_BO bo;
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    bo = WinMaliBoGet(adapter, args->Handle);
    if (bo == NULL) {
        return WMERR_ENOENT;
    }
    args->Size           = bo->Size;
    args->Flags          = bo->Flags;
    args->ExclusiveVmId  = bo->ExclusiveVmId;
    WinMaliBoPut(bo);
    return WMERR_OK;
}

static int
WinMaliEscapeBoSetLabel_(_In_    PWINMALI_ADAPTER       adapter,
                         _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                         _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_BO_SET_LABEL* args =
        (WINMALI_BO_SET_LABEL*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    const char* label;
    if (args == NULL || args->LabelLen > WINMALI_BO_LABEL_MAX) {
        return WMERR_EINVAL;
    }
    label = (const char*)WinMaliEscapeAt_(pEscape, args->LabelOffset, args->LabelLen);
    if (label == NULL && args->LabelLen != 0) {
        return WMERR_EINVAL;
    }
    return WinMaliBoSetLabel(adapter, args->Handle, label, args->LabelLen);
}

static int
WinMaliEscapeBoMapCpu_(_In_    PWINMALI_ADAPTER       adapter,
                       _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                       _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_BO_MAP_CPU* args =
        (WINMALI_BO_MAP_CPU*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    UINT64 va = 0;
    UINT64 mapped = 0;
    int err;
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    err = WinMaliBoMapCpu(adapter, args->Handle, args->Prot, &va, &mapped);
    if (err == WMERR_OK) {
        args->CpuAddr    = va;
        args->MappedBytes = mapped;
    }
    return err;
}

/* Op 0x8F - mirror a UMD-supplied text line onto the serial log. The UMD
   is headless on the DUT; this is its only window. Escapes arrive in the
   calling process's context, so PsGetCurrentProcessId names the process
   (dwm vs smoke tool) without any extra plumbing. */
/* Monotonic UMD-error counters, published to Parameters as they happen. They
   are driver-global rather than per-adapter so an adapter restart cannot reset
   them mid-experiment - the thing being measured spans UMD process lifetimes. */
LONG g_WinMaliStatPoolRefusals   = 0;
LONG g_WinMaliStatShortMappings  = 0;
LONG g_WinMaliStatUndersizedBos  = 0;

/* Tiny substring search - the WDK's string.h is not usable here and the needle
   is always a short ASCII literal. Returns TRUE if Needle occurs in Hay. */
static BOOLEAN
WinMaliStrContains_(_In_z_ const char* Hay, _In_z_ const char* Needle)
{
    ULONG i, j;
    if (Hay == NULL || Needle == NULL || Needle[0] == '\0') {
        return FALSE;
    }
    for (i = 0; Hay[i] != '\0'; ++i) {
        for (j = 0; Needle[j] != '\0' && Hay[i + j] == Needle[j]; ++j) {
            /* keep matching */
        }
        if (Needle[j] == '\0') {
            return TRUE;
        }
    }
    return FALSE;
}

static int
WinMaliEscapeDebugLog_(_In_    PWINMALI_ADAPTER       adapter,
                       _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                       _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_DEBUG_LOG* args =
        (WINMALI_DEBUG_LOG*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    const char* text;
    char line[WINMALI_DEBUG_LOG_MAX + 1];
    ULONG len, i;

    UNREFERENCED_PARAMETER(adapter);

    if (args == NULL) {
        return WMERR_EINVAL;
    }
    len = args->TextLen;
    if (len > WINMALI_DEBUG_LOG_MAX) {
        len = WINMALI_DEBUG_LOG_MAX;
    }
    text = (const char*)WinMaliEscapeAt_(pEscape, args->TextOffset, len);
    if (text == NULL && len != 0) {
        return WMERR_EINVAL;
    }

    /* Copy + sanitize: the buffer is UMD-controlled; keep it printable
       and NUL-terminated so it can't corrupt the serial stream. */
    for (i = 0; i < len; ++i) {
        char c = text[i];
        line[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    /* Drop trailing dots that came from a stray \n/\r. */
    while (i > 0 && line[i - 1] == '.') {
        --i;
    }
    line[i] = '\0';

    WinMaliLogPrint("[WinMali-UMD pid=%u] %s\n",
                    (ULONG)(ULONG_PTR)PsGetCurrentProcessId(), line);

    /* WRAP-PROOF COUNTERS.
     *
     * The 256 KB log ring holds ~2700 lines and wraps in ~2 minutes of desktop
     * activity. The pool refusals all occur in the first seconds of each UMD
     * process, so whether they appear in a captured ring is a coin flip: two
     * consecutive fresh-boot captures gave "10 refusals, handles 23/27/40/42/71"
     * and "wrapped, lowest handle 71". Counting log lines is therefore NOT a
     * sound before/after oracle - absence of the message means nothing.
     *
     * Publish monotonic counts to the registry instead, immediately (this
     * escape runs at PASSIVE_LEVEL in the caller's process context, and these
     * events are rare - ~10 per boot - so the write cost is irrelevant).
     * StatPoolRefusals is the number that must go to ZERO when the handle
     * namespace collision is fixed. */
    if (WinMaliStrContains_(line, "UNWRITABLE")) {
        LONG n = InterlockedIncrement(&g_WinMaliStatPoolRefusals);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatPoolRefusals",
                                      (ULONG)n);
    }
    if (WinMaliStrContains_(line, "SHORT MAPPING")) {
        LONG n = InterlockedIncrement(&g_WinMaliStatShortMappings);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatShortMappings",
                                      (ULONG)n);
    }
    if (WinMaliStrContains_(line, "UNDERSIZED")) {
        LONG n = InterlockedIncrement(&g_WinMaliStatUndersizedBos);
        (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatUndersizedBos",
                                      (ULONG)n);
    }
    return WMERR_OK;
}

static int
WinMaliEscapeSyncObjCreate_(_In_    PWINMALI_ADAPTER       adapter,
                            _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                            _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_SYNC_OBJ_CREATE* args =
        (WINMALI_SYNC_OBJ_CREATE*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    ULONG handle = 0;
    int err;
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    err = WinMaliSyncObjCreate(adapter, args->Flags, args->InitialState,
                               WinMaliDeviceOwnerToken(pEscape->hDevice), &handle);
    if (err == WMERR_OK) {
        args->Handle = handle;
        args->Pad    = 0;
    }
    return err;
}

static int
WinMaliEscapeSyncObjDestroy_(_In_    PWINMALI_ADAPTER       adapter,
                             _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                             _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_SYNC_OBJ_DESTROY* args =
        (WINMALI_SYNC_OBJ_DESTROY*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    return WinMaliSyncObjDestroy(adapter, args->Handle);
}

static int
WinMaliEscapeSyncObjSignal_(_In_    PWINMALI_ADAPTER       adapter,
                            _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                            _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_SYNC_OBJ_SIGNAL* args =
        (WINMALI_SYNC_OBJ_SIGNAL*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    return WinMaliSyncObjSignal(adapter, args->Handle, args->Point);
}

static int
WinMaliEscapeVmCreate_(_In_    PWINMALI_ADAPTER       adapter,
                       _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                       _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_VM_CREATE* args =
        (WINMALI_VM_CREATE*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    ULONG id = 0;
    UINT64 granted = 0;
    int err;
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    err = WinMaliVmCreate(adapter, args->Flags, args->UserVaRange,
                          WinMaliDeviceOwnerToken(pEscape->hDevice),
                          &id, &granted);
    if (err == WMERR_OK) {
        args->Id          = id;
        args->UserVaRange = granted;
    }
    return err;
}

static int
WinMaliEscapeVmDestroy_(_In_    PWINMALI_ADAPTER       adapter,
                        _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                        _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_VM_DESTROY* args =
        (WINMALI_VM_DESTROY*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    return WinMaliVmDestroy(adapter, args->Id);
}

static int
WinMaliEscapeVmGetState_(_In_    PWINMALI_ADAPTER       adapter,
                         _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                         _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_VM_GET_STATE* args =
        (WINMALI_VM_GET_STATE*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    WINMALI_VM_RUN_STATE state;
    int err;
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    err = WinMaliVmGetState(adapter, args->VmId, &state);
    if (err == WMERR_OK) {
        args->State = (state == WinMaliVmRun_Usable) ? WinMaliVmState_Usable
                                                     : WinMaliVmState_Unusable;
    }
    return err;
}

/* Walk a VM_BIND_OP array, installing/removing PTEs into the VM's PT. */
static int
WinMaliEscapeVmBind_(_In_    PWINMALI_ADAPTER       adapter,
                     _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                     _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_VM_BIND* args =
        (WINMALI_VM_BIND*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    const WINMALI_VM_BIND_OP* ops;
    PWINMALI_VM vm;
    ULONG i;
    int err = WMERR_OK;

    if (args == NULL) {
        return WMERR_EINVAL;
    }
    if (args->OpsCount == 0) {
        return WMERR_OK;
    }
    ops = (const WINMALI_VM_BIND_OP*)WinMaliEscapeAt_(
        pEscape, args->OpsOffset, args->OpsCount * sizeof(*ops));
    if (ops == NULL) {
        return WMERR_EINVAL;
    }
    vm = WinMaliVmGet(adapter, args->VmId);
    if (vm == NULL) {
        return WMERR_ENOENT;
    }
    if (!vm->PtInitialized) {
        WinMaliVmPut(vm);
        return WMERR_EINVAL;
    }

    for (i = 0; i < args->OpsCount; ++i) {
        const WINMALI_VM_BIND_OP* op = &ops[i];
        ULONG opType = op->Flags & WINMALI_VM_BIND_OP_TYPE_MASK;
        if (opType == WINMALI_VM_BIND_OP_TYPE_MAP) {
            PWINMALI_BO bo;
            PPFN_NUMBER pfnArray;
            ULONG pageCount;
            UINT64 attrs;
            NTSTATUS st;

            bo = WinMaliBoGet(adapter, op->BoHandle);
            if (bo == NULL) {
                WINMALI_WARN("VmBind MAP: BoHandle %u unknown", op->BoHandle);
                err = WMERR_ENOENT;
                break;
            }
            if (op->BoOffset != 0 || op->Size > bo->Size) {
                /* BoOffset not yet supported (would need offset-aware MDL
                   walk); reject for now to surface UMD mistakes early. */
                WinMaliBoPut(bo);
                WINMALI_WARN("VmBind MAP: BoOffset=%llu size=%llu (bo size=%llu) - "
                             "BoOffset != 0 not yet supported",
                             (ULONGLONG)op->BoOffset, (ULONGLONG)op->Size,
                             (ULONGLONG)bo->Size);
                err = WMERR_EINVAL;
                break;
            }
            pfnArray = MmGetMdlPfnArray(bo->Mdl);
            if (pfnArray == NULL) {
                WinMaliBoPut(bo);
                err = WMERR_EINVAL;
                break;
            }
            pageCount = (ULONG)((op->Size + PAGE_SIZE - 1) >> PAGE_SHIFT);

            /* Attribute mapping from drm_panthor_vm_bind_op_flags. */
            if (op->Flags & WINMALI_VM_BIND_OP_MAP_READONLY) {
                attrs = (op->Flags & WINMALI_VM_BIND_OP_MAP_NOEXEC)
                          ? WINMALI_LPAE_L3_PAGE_ATTR_RO_NX
                          : WINMALI_LPAE_L3_PAGE_ATTR_RO_EX;
            } else {
                attrs = (op->Flags & WINMALI_VM_BIND_OP_MAP_NOEXEC)
                          ? WINMALI_LPAE_L3_PAGE_ATTR_RW_NX
                          : WINMALI_LPAE_L3_PAGE_ATTR_RW_EX;
            }

            st = WinMaliMmuVmMap(adapter, &vm->Pt, op->Va, pfnArray,
                                 pageCount, attrs);
            WinMaliBoPut(bo);
            if (!NT_SUCCESS(st)) {
                err = (st == STATUS_INSUFFICIENT_RESOURCES) ? WMERR_ENOMEM
                                                            : WMERR_EINVAL;
                break;
            }
            WINMALI_TRACE("VmBind MAP: vm=%u bo=%u va=0x%llx pages=%u attrs=0x%llx",
                          vm->Id, op->BoHandle, (ULONGLONG)op->Va,
                          pageCount, (ULONGLONG)attrs);
        } else if (opType == WINMALI_VM_BIND_OP_TYPE_UNMAP) {
            ULONG pageCount = (ULONG)((op->Size + PAGE_SIZE - 1) >> PAGE_SHIFT);
            NTSTATUS st = WinMaliMmuVmUnmap(adapter, &vm->Pt, op->Va, pageCount);
            if (!NT_SUCCESS(st)) {
                err = WMERR_EINVAL;
                break;
            }
            WINMALI_TRACE("VmBind UNMAP: vm=%u va=0x%llx pages=%u",
                          vm->Id, (ULONGLONG)op->Va, pageCount);
        } else if (opType == WINMALI_VM_BIND_OP_TYPE_SYNC_ONLY) {
            /* SYNC_ONLY: no PT work, just sync ops. We'd process op->SyncsOffset
               here once sync integration with SyncObjs lands. */
            WINMALI_TRACE("VmBind SYNC_ONLY: vm=%u (sync arrays not yet processed)",
                          vm->Id);
        } else {
            WINMALI_WARN("VmBind: unknown op type 0x%x (flags=0x%x)",
                         opType, op->Flags);
            err = WMERR_EINVAL;
            break;
        }
    }

    WinMaliVmPut(vm);
    return err;
}

static int
WinMaliEscapeGroupCreate_(_In_    PWINMALI_ADAPTER       adapter,
                          _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                          _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_GROUP_CREATE* args =
        (WINMALI_GROUP_CREATE*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    const WINMALI_QUEUE_CREATE* queues;
    ULONG handle = 0;
    int err;
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    if (args->QueuesCount == 0 || args->QueuesCount > WINMALI_GROUP_MAX_QUEUES) {
        return WMERR_EINVAL;
    }
    queues = (const WINMALI_QUEUE_CREATE*)WinMaliEscapeAt_(
        pEscape, args->QueuesOffset,
        args->QueuesCount * sizeof(WINMALI_QUEUE_CREATE));
    if (queues == NULL) {
        return WMERR_EINVAL;
    }
    err = WinMaliGroupCreate(adapter, args, queues, args->QueuesCount,
                             WinMaliDeviceOwnerToken(pEscape->hDevice), &handle);
    if (err == WMERR_OK) {
        args->GroupHandle = handle;
    }
    return err;
}

static int
WinMaliEscapeGroupDestroy_(_In_    PWINMALI_ADAPTER       adapter,
                           _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                           _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_GROUP_DESTROY* args =
        (WINMALI_GROUP_DESTROY*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    return WinMaliGroupDestroy(adapter, args->GroupHandle);
}

static int
WinMaliEscapeGroupGetState_(_In_    PWINMALI_ADAPTER       adapter,
                            _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                            _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_GROUP_GET_STATE* args =
        (WINMALI_GROUP_GET_STATE*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    ULONG state = 0, fatal = 0;
    int err;
    if (args == NULL) {
        return WMERR_EINVAL;
    }
    err = WinMaliGroupGetState(adapter, args->GroupHandle, &state, &fatal);
    if (err == WMERR_OK) {
        args->StateFlags  = state;
        args->FatalQueues = fatal;
    }
    return err;
}

/* Process the sync-op tail of a queue submit. Direction == 0: wait; ==1: signal.
   Skips ops whose direction bit doesn't match. Returns -errno on failure. */
static int
WinMaliEscapeProcessSyncOps_(_Inout_ PWINMALI_ADAPTER adapter,
                             _In_    const WINMALI_SYNC_OP* ops,
                             _In_    ULONG count,
                             _In_    BOOLEAN isSignal)
{
    ULONG i;
    int err = WMERR_OK;
    for (i = 0; i < count; ++i) {
        BOOLEAN opIsSignal = (ops[i].Flags & WINMALI_SYNC_OP_SIGNAL) ? TRUE : FALSE;
        if (opIsSignal != isSignal) {
            continue;
        }
        if (isSignal) {
            int e = WinMaliSyncObjSignal(adapter, ops[i].Handle,
                                         ops[i].TimelineValue);
            if (e != WMERR_OK && err == WMERR_OK) {
                err = e;
            }
        } else {
            ULONG h = ops[i].Handle;
            UINT64 p = ops[i].TimelineValue;
            /* ⛔ NEVER INFINITE. This wait runs on the CALLER's thread inside
               DxgkDdiEscape, so a syncobj that never signals - which is exactly
               what a wedged GPU produces - parks a user thread in the kernel
               with no way out. That is the recorded 0x9F P2=0x12C class: the
               process becomes unkillable, PnP can never take the device lock,
               and a GPU that merely stopped drawing takes the whole machine
               down instead. A wedged GPU must not take Windows down.

               2 s is well past any real frame and well inside the compositor's
               own tolerance; on expiry we return the timeout as a submit
               failure, which the UMD already knows how to survive. */
            int e = WinMaliSyncObjWait(adapter, 1u, &h, &p,
                                       FALSE, WINMALI_SYNC_WAIT_MAX_NS);
            if (e != WMERR_OK) {
                if (e == WMERR_ETIMEDOUT) {
                    (VOID)InterlockedIncrement(&g_WinMaliStatSyncWaitTimeout);
                }
                err = e;
                break;
            }
        }
    }
    return err;
}

/* Allocate `Size` bytes of device-owned physical memory, map its pages
   into `Vm` at `GpuVa` (data, non-executable), zero the first page, and -
   if HasNext - stamp the u64 chunk-link at offset 0. Used to build the
   tiler heap's context descriptor and chunk ring.

   The backing store is a normal BO (OwnerDevice set), so device rundown /
   VM teardown reclaim it exactly like any UMD allocation - no bespoke
   free path, no leak. Returns the BO handle in *OutHandle for optional
   explicit teardown. The GPU-side mapping lives in the VM's PT and is
   released when the VM is torn down. */
static NTSTATUS
WinMaliHeapAllocMapped_(_Inout_ PWINMALI_ADAPTER adapter,
                        _Inout_ PWINMALI_VM      vm,
                        _In_opt_ PVOID           ownerDevice,
                        _In_    ULONG            size,
                        _In_    UINT64           gpuVa,
                        _In_    BOOLEAN          hasNext,
                        _In_    UINT64           nextValue,
                        _Out_   ULONG*           outHandle)
{
    ULONG        handle = 0;
    PWINMALI_BO  bo;
    PPFN_NUMBER  pfns;
    PVOID        kva;
    ULONG        pages;
    NTSTATUS     st;
    int          err;

    *outHandle = 0;

    if ((size & (PAGE_SIZE - 1)) != 0 || size == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* size is already asserted page-aligned above, so actual == size here. */
    err = WinMaliBoCreate(adapter, (SIZE_T)size, 0u, 0u, ownerDevice, &handle, NULL);
    if (err != WMERR_OK) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    bo = WinMaliBoGet(adapter, handle);
    if (bo == NULL) {
        (void)WinMaliBoDestroy(adapter, handle);
        return STATUS_UNSUCCESSFUL;
    }

    pages = (ULONG)(bo->Size / PAGE_SIZE);
    pfns  = MmGetMdlPfnArray(bo->Mdl);
    st = WinMaliMmuVmMap(adapter, &vm->Pt, gpuVa, pfns, pages,
                         WINMALI_LPAE_L3_PAGE_ATTR_RW_NX);
    if (!NT_SUCCESS(st)) {
        WinMaliBoPut(bo);
        (void)WinMaliBoDestroy(adapter, handle);
        return st;
    }

    /* One-time init of the descriptor/header: map the backing pages into
       system space, zero the leading page (covers the 32-byte heap context
       and the 64-byte chunk header - MBZ fields), stamp the chunk link if
       requested, then drop the transient kernel mapping. The GPU-visible
       mapping in the VM PT is unaffected. */
    kva = MmMapLockedPagesSpecifyCache(bo->Mdl, KernelMode, MmCached,
                                       NULL, FALSE, NormalPagePriority);
    if (kva == NULL) {
        (void)WinMaliMmuVmUnmap(adapter, &vm->Pt, gpuVa, pages);
        WinMaliBoPut(bo);
        (void)WinMaliBoDestroy(adapter, handle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(kva, PAGE_SIZE);
    if (hasNext) {
        *(volatile UINT64 UNALIGNED*)kva = nextValue;
    }
    MmUnmapLockedPages(kva, bo->Mdl);

    WinMaliBoPut(bo);
    *outHandle = handle;
    return STATUS_SUCCESS;
}

/* TilerHeapCreate real path: allocate the 32-byte heap context descriptor
   plus `InitialChunkCount` chunks of `ChunkSize` each, map them into the
   group's VM, and link the chunks newest->oldest exactly as panthor does
   (hdr->next = (prev_chunk_va & ~0xFFF) | (chunk_size >> 12); last chunk
   next=0). `first_chunk_gpu_va` is the newest (last-allocated) chunk.

   Returns STATUS_SUCCESS with *OutCtxVa / *OutFirstChunkVa filled, or a
   failure the caller turns into an escape error (TilerHeapCreate no longer
   hands back a placeholder heap). */
static NTSTATUS
WinMaliTilerHeapCreateReal_(_Inout_ PWINMALI_ADAPTER adapter,
                            _In_    PVOID            ownerDevice,
                            _In_    ULONG            vmId,
                            _In_    ULONG            chunkSize,
                            _In_    ULONG            initialChunkCount,
                            _Out_   UINT64*          outCtxVa,
                            _Out_   UINT64*          outFirstChunkVa)
{
    PWINMALI_VM vm;
    NTSTATUS    st;
    ULONG       ctxHandle = 0;
    UINT64      ctxVa;
    UINT64      firstChunkVa = 0;
    UINT64      prevChunkVa  = 0;
    ULONG       i;
    ULONG       count = initialChunkCount;

    *outCtxVa = 0;
    *outFirstChunkVa = 0;

    /* Sanity: chunk_size must be page-aligned and sane (panthor allows
       256K..2M power-of-two; we only require page alignment + a cap so a
       bogus UMD value can't exhaust memory). Cap initial chunks too. */
    if ((chunkSize & (PAGE_SIZE - 1)) != 0 ||
        chunkSize < 0x1000u || chunkSize > 0x200000u) {
        return STATUS_INVALID_PARAMETER;
    }
    if (count == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (count > 16u) {
        count = 16u;  /* bound the up-front allocation; growth is on-demand */
    }

    vm = WinMaliVmGet(adapter, vmId);
    if (vm == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!vm->PtInitialized) {
        WinMaliVmPut(vm);
        return STATUS_DEVICE_NOT_READY;
    }

    /* Heap context: one page (holds the cache-line-aligned 32-byte
       descriptor; the GPU manages the rest). */
    ctxVa = vm->KmdVaNext;
    st = WinMaliHeapAllocMapped_(adapter, vm, ownerDevice, PAGE_SIZE,
                                 ctxVa, FALSE, 0, &ctxHandle);
    if (!NT_SUCCESS(st)) {
        WinMaliVmPut(vm);
        return st;
    }
    vm->KmdVaNext += PAGE_SIZE;

    /* Chunks: allocate `count`, each linked to the previous. panthor adds
       new chunks to the front of the list, so chunk[k].next = chunk[k-1],
       and first_chunk = the last one allocated. */
    for (i = 0; i < count; ++i) {
        ULONG  chunkHandle = 0;
        UINT64 va       = vm->KmdVaNext;
        UINT64 nextVal  = (prevChunkVa != 0)
                          ? ((prevChunkVa & ~0xFFFull) | ((UINT64)chunkSize >> 12))
                          : 0ull;
        st = WinMaliHeapAllocMapped_(adapter, vm, ownerDevice, chunkSize,
                                     va, TRUE, nextVal, &chunkHandle);
        if (!NT_SUCCESS(st)) {
            /* Partial failure: the context + already-mapped chunks are BOs
               owned by the device and get reclaimed at rundown/VM teardown.
               Report failure so the caller uses the metadata fallback. */
            WinMaliVmPut(vm);
            return st;
        }
        prevChunkVa  = va;
        firstChunkVa = va;               /* newest = first_chunk_gpu_va */
        vm->KmdVaNext = va + chunkSize;
    }

    /* ★ RECORD THE HEAP SO IT CAN BE GROWN LATER.
       Without this the heap was write-once: created with `count` chunks and no
       way to extend it, so the first TILER_OOM parked the CS forever. See
       WinMaliTilerHeapGrow below and WinMaliCsfServiceTilerOom_ in WinMaliFw.c. */
    vm->HeapCtxVa         = ctxVa;
    vm->HeapCtxHandle     = ctxHandle;
    vm->HeapFirstChunkVa  = firstChunkVa;
    vm->HeapChunkSize     = chunkSize;
    vm->HeapChunkCount    = count;
    vm->HeapOwnerDevice   = ownerDevice;
    /* Bound total growth. panthor caps heaps too; an unbounded grow on a
       runaway tiler would eat all of system memory instead of wedging one
       context. 128 chunks at the usual 2 MB is 256 MB, far past anything the
       desktop needs, and the cap failing is still strictly better than today's
       permanent wedge (we ack with a NULL chunk and the CS faults cleanly). */
    vm->HeapMaxChunks     = 128u;

    WinMaliVmPut(vm);
    *outCtxVa = ctxVa;
    *outFirstChunkVa = firstChunkVa;
    return STATUS_SUCCESS;
}

/* Point the tiler heap CONTEXT at a new head chunk.
 *
 * ⛔★★★★★ THIS IS THE PIECE THAT MAKES A GROWN CHUNK REACHABLE.
 *
 * The command stream does `cs_heap_set(ctx_va)` (pan_csf.c:487-490) so the
 * FIRMWARE walks the 32-byte heap context descriptor - NOT any driver-side
 * bookkeeping - to find chunks. Growing the heap by allocating a chunk and
 * linking it new->old updates only the new chunk's own header; nothing points
 * AT the new chunk, so it is orphaned and the FW is no better off.
 *
 * MEASURED 1.1.9.15: 38 TILER_OOM requests, 38 chunks allocated and mapped
 * successfully (OOMfail=0), our ack landing every time (the FW toggles its ACK
 * bit and we match it) - and CsOk still frozen, with the FW re-asking once per
 * retry forever. That is precisely what handing over an unreachable chunk looks
 * like.
 *
 * Chunk pointers use panthor's packed encoding, the same one
 * WinMaliTilerHeapCreateReal_ stamps into each chunk header:
 *     (va & ~0xFFF) | (chunk_size >> 12)
 * and the context's first field is that pointer. */
static NTSTATUS
WinMaliHeapCtxSetFirstChunk_(_Inout_ PWINMALI_ADAPTER adapter,
                             _In_    ULONG            ctxHandle,
                             _In_    UINT64           encodedChunk,
                             _Out_opt_ UINT64*        outPrevEncoded)
{
    PWINMALI_BO bo;
    PVOID       kva;

    if (ctxHandle == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    bo = WinMaliBoGet(adapter, ctxHandle);
    if (bo == NULL || bo->Mdl == NULL) {
        if (bo != NULL) { WinMaliBoPut(bo); }
        return STATUS_NOT_FOUND;
    }
    kva = MmMapLockedPagesSpecifyCache(bo->Mdl, KernelMode, MmCached,
                                       NULL, FALSE, NormalPagePriority);
    if (kva == NULL) {
        WinMaliBoPut(bo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* ⛔★★★★★ READ THE FW'S CURRENT HEAD, DO NOT TRUST OUR BOOKKEEPING.
       The FIRMWARE pops chunks off this free list and rewrites the head itself
       (pan_csf.c:1256 "Gather freed heap chunks and add them to the heap context
       free list"). vm->HeapFirstChunkVa is therefore STALE the moment the FW
       consumes one, and linking new->stale CORRUPTS the list - which is what
       1.1.9.17 did: 38 chunks handed over, every one acked, stream never resumed.
       Read-modify-write the real head instead. */
    if (outPrevEncoded != NULL) {
        *outPrevEncoded = *(volatile UINT64 UNALIGNED*)kva;
    }
    *(volatile UINT64 UNALIGNED*)kva = encodedChunk;
    KeMemoryBarrier();
    MmUnmapLockedPages(kva, bo->Mdl);
    WinMaliBoPut(bo);
    return STATUS_SUCCESS;
}

/* Service a firmware TILER_OOM: allocate ONE more chunk for the tiler heap of
   the VM bound to `asSlot`, link it at the HEAD of the newest->oldest chunk
   list exactly as WinMaliTilerHeapCreateReal_ builds it, and hand the caller
   its GPU VA and size.

   ★ WHY THIS EXISTS (measured 2026-08-20). The FW parks the command stream and
   raises CS_ACK bit 26 when the tiler heap runs out of chunks, and resumes only
   once the host links in a chunk and toggles CS_REQ bit 26 to match. The driver
   had TilerHeapCreate and TilerHeapDestroy but NO grow path, and
   WINMALI_CS_REQ_MASK masked bit 26 out of every REQ write, so the request could
   be neither satisfied nor acknowledged. First exhaustion = permanent wedge:
   CsAck=0x04000011 vs CsReq=0x00000511, CsOk frozen, CsTimeouts climbing, and
   every fault bank reading zero because TILER_OOM is a REQUEST, not a fault.
   panthor's equivalent is panthor_heap_grow().

   Returns STATUS_INSUFFICIENT_RESOURCES at the cap or on allocation failure.
   ⚠ The caller must ACKNOWLEDGE the request either way (with a zero chunk on
   failure) - an unacknowledged TILER_OOM parks the CS forever, which is strictly
   worse than letting it fault. */
NTSTATUS
WinMaliTilerHeapGrow(_Inout_ PWINMALI_ADAPTER adapter,
                     _In_    ULONG            asSlot,
                     _Out_   UINT64*          outChunkVa,
                     _Out_   ULONG*           outChunkSize)
{
    PWINMALI_VM vm;
    NTSTATUS    st;
    ULONG       chunkHandle = 0;
    UINT64      va;
    UINT64      nextVal;

    *outChunkVa   = 0;
    *outChunkSize = 0;

    vm = WinMaliVmGetByAsSlot(adapter, asSlot);
    if (vm == NULL) {
        return STATUS_NOT_FOUND;
    }
    if (!vm->PtInitialized || vm->HeapChunkSize == 0) {
        WinMaliVmPut(vm);
        return STATUS_NOT_FOUND;            /* no tiler heap on this VM */
    }
    if (vm->HeapChunkCount >= vm->HeapMaxChunks) {
        WINMALI_WARN("TilerHeapGrow: vm=%u at chunk cap %u - refusing to grow",
                     vm->Id, vm->HeapMaxChunks);
        WinMaliVmPut(vm);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    va = vm->KmdVaNext;
    /* Header left ZERO here on purpose - the real 'next' is the head we are about
       to read out of the context, which only the swap below can tell us. */
    UNREFERENCED_PARAMETER(nextVal);
    st = WinMaliHeapAllocMapped_(adapter, vm, vm->HeapOwnerDevice,
                                 vm->HeapChunkSize, va, FALSE, 0ull,
                                 &chunkHandle);
    if (!NT_SUCCESS(st)) {
        WINMALI_WARN("TilerHeapGrow: vm=%u chunk alloc failed 0x%08x", vm->Id, st);
        WinMaliVmPut(vm);
        return st;
    }

    vm->KmdVaNext        = va + vm->HeapChunkSize;
    vm->HeapFirstChunkVa = va;
    vm->HeapChunkCount  += 1;


    /* ★ MAKE THE CHUNK REACHABLE. Linking new->old only fills in the NEW chunk's
       own header; nothing points AT it until the heap context's head pointer is
       updated, and the context is what `cs_heap_set` hands the firmware. Skipping
       this is why 1.1.9.15 allocated 38 chunks that the FW could not use. */
    if (g_WinMaliTilerOomCtxSplice != 0) {
        /* ⚠ OFF BY DEFAULT. panthor and kbase hand the grown chunk to the FW ONLY via
           CS_TILER_HEAP_START/END; they do NOT touch the heap context on grow.
           Splicing here as well puts the same chunk in two places, which can corrupt
           the free list the FW is walking. Knob so both behaviours are testable
           without a rebuild (ParametersTilerOomCtxSplice=1 restores 1.1.9.18). */
        UINT64   encoded = (va & ~0xFFFull) | ((UINT64)vm->HeapChunkSize >> 12);
        UINT64   prevEncoded = 0;
        NTSTATUS cst = WinMaliHeapCtxSetFirstChunk_(adapter, vm->HeapCtxHandle,
                                                    encoded, &prevEncoded);
        if (!NT_SUCCESS(cst)) {
            WINMALI_WARN("TilerHeapGrow: vm=%u ctx head update FAILED 0x%08x - the "
                         "new chunk is UNREACHABLE by the FW", vm->Id, cst);
        } else {
            /* Link the new chunk to whatever the FIRMWARE had as head. Same helper:
               both the context and a chunk header hold the packed pointer at
               offset 0. Doing it after the swap is safe because the FW is parked
               waiting for the acknowledgement we have not sent yet. */
            (VOID)WinMaliHeapCtxSetFirstChunk_(adapter, chunkHandle, prevEncoded, NULL);
            WINMALI_TRACE("TilerHeapGrow: ctx head 0x%llx -> 0x%llx (new chunk 0x%llx, "
                          "prev head kept as its next)",
                          (ULONGLONG)prevEncoded, (ULONGLONG)encoded, (ULONGLONG)va);
        }
    }
    *outChunkVa   = va;
    *outChunkSize = vm->HeapChunkSize;
    WINMALI_TRACE("TilerHeapGrow: vm=%u as=%u new chunk va=0x%llx size=%u count=%u/%u",
                  vm->Id, asSlot, (ULONGLONG)va, vm->HeapChunkSize,
                  vm->HeapChunkCount, vm->HeapMaxChunks);
    WinMaliVmPut(vm);
    return STATUS_SUCCESS;
}

/* GroupSubmit: walk queue submits, process pre-submit sync waits, run the
   UMD's real command stream in the group's VM (NOP fallback on fault),
   process post-submit sync signals. */
static int
WinMaliEscapeGroupSubmit_(_In_    PWINMALI_ADAPTER       adapter,
                          _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                          _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_GROUP_SUBMIT* args =
        (WINMALI_GROUP_SUBMIT*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    const WINMALI_QUEUE_SUBMIT* submits;
    ULONG i;
    int err = WMERR_OK;

    if (args == NULL) {
        return WMERR_EINVAL;
    }
    if (args->QueueSubmitsCount == 0) {
        return WMERR_OK;
    }
    submits = (const WINMALI_QUEUE_SUBMIT*)WinMaliEscapeAt_(
        pEscape, args->QueueSubmitsOffset,
        args->QueueSubmitsCount * sizeof(*submits));
    if (submits == NULL) {
        return WMERR_EINVAL;
    }

    /* Confirm the group exists; we don't dereference it further yet. */
    {
        ULONG sf, fq;
        int e = WinMaliGroupGetState(adapter, args->GroupHandle, &sf, &fq);
        if (e != WMERR_OK) {
            /* ★ SAY WHY. A bare ENOENT on submit is ambiguous between "the UMD
             * sent rubbish" and "this handle belonged to a PREVIOUS adapter
             * instance" - and the second is what actually happens, because dwm
             * and explorer survive an adapter restart while the group table is
             * rebuilt. Naming the live handle range makes that unmistakable. */
            (VOID)InterlockedIncrement(&g_WinMaliStatSubmitNoGroup);
            WINMALI_WARN("GroupSubmit: group=%u UNKNOWN to this adapter instance "
                         "(count=%ld) - a stale handle from a previous instance "
                         "looks exactly like this",
                         args->GroupHandle, adapter->GroupTable.Count);
            return e;
        }
        if (sf & WINMALI_GROUP_STATE_FATAL_FAULT) {
            (VOID)InterlockedIncrement(&g_WinMaliStatSubmitFatalGroup);
            WINMALI_WARN("GroupSubmit: group=%u is in FATAL_FAULT (state=0x%x "
                         "faultyQueues=0x%x) - refusing", args->GroupHandle, sf, fq);
            return WMERR_EINVAL;
        }
    }

    for (i = 0; i < args->QueueSubmitsCount; ++i) {
        const WINMALI_QUEUE_SUBMIT* qs = &submits[i];
        const WINMALI_SYNC_OP* syncs = NULL;
        if (qs->SyncsCount > 0) {
            syncs = (const WINMALI_SYNC_OP*)WinMaliEscapeAt_(
                pEscape, qs->SyncsOffset, qs->SyncsCount * sizeof(*syncs));
            if (syncs == NULL) {
                err = WMERR_EINVAL;
                break;
            }
        }
        if (syncs != NULL) {
            err = WinMaliEscapeProcessSyncOps_(adapter, syncs, qs->SyncsCount, FALSE);
            if (err != WMERR_OK) {
                /* ★ COUNT SYNC-OP REJECTIONS SEPARATELY.
                 *
                 * This is where 195 submits per window were actually dying, and
                 * it was invisible: the group check passed (so the group counter
                 * stayed 0) and the caller only ever saw a bare ENOENT, which
                 * reads identically to "unknown group". A stale SYNCOBJ handle
                 * and a stale GROUP handle are different bugs in different
                 * tables and must never again share one indistinguishable code. */
                (VOID)InterlockedIncrement(&g_WinMaliStatSubmitSyncFail);
                (VOID)InterlockedExchange(&g_WinMaliStatSubmitSyncErr, (LONG)err);
                /* ★ NAME THE OFFENDING HANDLE. "sync ops rejected" was still
                   ambiguous: handle 0 (a BO with no sync state - the measured
                   cause), a stale handle from a previous adapter instance, and
                   a genuinely destroyed syncobj all read identically. Publish
                   the first handle in the batch and how many ops it carried. */
                if (syncs != NULL && qs->SyncsCount > 0) {
                    (VOID)InterlockedExchange(&g_WinMaliStatSyncBadHandle,
                                              (LONG)syncs[0].Handle);
                    (VOID)InterlockedExchange(&g_WinMaliStatSyncBadFlags,
                                              (LONG)syncs[0].Flags);
                }
                WINMALI_WARN("GroupSubmit: group=%u queue=%u SYNC OPS rejected "
                             "err=%d (count=%u first handle=%u flags=0x%x) - "
                             "handle 0 means a BO with NO sync state, which is "
                             "NOT the same as a stale handle",
                             args->GroupHandle, qs->QueueIndex, err, qs->SyncsCount,
                             (syncs && qs->SyncsCount) ? syncs[0].Handle : 0u,
                             (syncs && qs->SyncsCount) ? syncs[0].Flags  : 0u);
                break;
            }
        }

        /* Execute the UMD's REAL command stream. Resolve the group's VM
           address space, bind the CSG to it, and run the CS at qs->StreamAddr
           (which, along with every buffer it references, lives in that VM).
           On any failure - AS rebind not acked, GPU MMU fault, timeout - return
           the error to the UMD. We deliberately do NOT fall back to a NOP self-
           test: signalling the completion fence without doing the work lets the
           UMD proceed on an uninitialized heap / stale GPU state and freeze
           later. A clean failure the UMD can see beats a fake success. */
        {
            NTSTATUS csfSt = STATUS_INVALID_PARAMETER;
            ULONG    asSlot = WINMALI_AS_SLOT_MAX;
            ULONG    vmId   = 0;

            if (WinMaliGroupGetVmId(adapter, args->GroupHandle, &vmId) == WMERR_OK) {
                PWINMALI_VM vm = WinMaliVmGet(adapter, vmId);
                if (vm != NULL) {
                    asSlot = vm->Pt.AsSlot;
                    WinMaliVmPut(vm);
                }
            }

            if (asSlot >= WINMALI_AS_SLOT_MAX || qs->StreamAddr == 0 || qs->StreamSize == 0) {
                WINMALI_WARN("GroupSubmit: group=%u queue=%u no runnable stream "
                             "(as=%u stream_va=0x%llx size=%u)",
                             args->GroupHandle, qs->QueueIndex, asSlot,
                             (ULONGLONG)qs->StreamAddr, qs->StreamSize);
                err = WMERR_EINVAL;
                break;
            }

            /* Non-coherent GPU cache maintenance (coh=0): CLEAN this VM's BOs
               so the CPU-written CS / vertices / shaders / descriptors reach
               DRAM before the GPU (non-cacheable) reads them. Without this the
               GPU reads stale/zero input and the draw renders nothing (black)
               even though it "completes" - the MMU-audit root-cause. Submit is
               synchronous, so we invalidate after it returns (below) so a CPU
               readback of the render target sees the GPU's output. */
            WinMaliBoFlushVm(adapter, vmId, TRUE /* clean CPU->DRAM */);

            csfSt = WinMaliCsfSubmitGroupStream(adapter, asSlot, qs->StreamAddr,
                                                qs->StreamSize, 0u, 500u);
            WinMaliBoFlushVm(adapter, vmId, FALSE /* invalidate DRAM->CPU */);
            if (!NT_SUCCESS(csfSt)) {
                WINMALI_WARN("GroupSubmit: group=%u queue=%u REAL stream failed 0x%08x "
                             "(as=%u stream_va=0x%llx size=%u)",
                             args->GroupHandle, qs->QueueIndex, csfSt, asSlot,
                             (ULONGLONG)qs->StreamAddr, qs->StreamSize);
                /* Record the faulted queue so the UMD's QueryGroupState sees a
                   non-zero FatalQueueMask (panthor parity) and can reset the
                   context instead of proceeding on a wedged group. */
                (VOID)WinMaliGroupMarkFatalQueue(adapter, args->GroupHandle,
                                                 qs->QueueIndex);
                /* ★ PUBLISH THE REAL GPU FAILURE WHERE IT CAN BE READ.
                 *
                 * The WARN above carries everything worth knowing - the NTSTATUS
                 * from the CSF submit, the AS slot, the stream VA and size - and
                 * goes ONLY to the EDK2 ring, which is FILL-ONCE and is long
                 * since full. So the actual reason every frame is rejected has
                 * been invisible; all the caller ever saw was a bare EIO.
                 * These are latched (last value wins) plus a count, so a single
                 * registry read gives the status and how often it is happening. */
                (VOID)InterlockedExchange(&g_WinMaliStatCsfLastStatus, (LONG)csfSt);
                (VOID)InterlockedExchange(&g_WinMaliStatCsfLastAs,     (LONG)asSlot);
                (VOID)InterlockedExchange(&g_WinMaliStatCsfLastSize,   (LONG)qs->StreamSize);
                (VOID)InterlockedExchange(&g_WinMaliStatCsfLastVaLo,
                                          (LONG)(ULONG)(qs->StreamAddr & 0xFFFFFFFFull));
                (VOID)InterlockedExchange(&g_WinMaliStatCsfLastVaHi,
                                          (LONG)(ULONG)((qs->StreamAddr >> 32) & 0xFFFFFFFFull));
                (VOID)InterlockedIncrement(&g_WinMaliStatCsfFailCount);
                err = (csfSt == STATUS_DEVICE_NOT_READY) ? WMERR_EINVAL : WMERR_EIO;
                break;
            }
            WINMALI_TRACE("GroupSubmit: group=%u queue=%u REAL stream ran "
                          "(as=%u stream_va=0x%llx size=%u syncs=%u)",
                          args->GroupHandle, qs->QueueIndex, asSlot,
                          (ULONGLONG)qs->StreamAddr, qs->StreamSize, qs->SyncsCount);
        }

        if (syncs != NULL) {
            int e = WinMaliEscapeProcessSyncOps_(adapter, syncs, qs->SyncsCount, TRUE);
            if (e != WMERR_OK && err == WMERR_OK) {
                /* ⛔ THE ONE ENOENT PATH WITH NO COUNTER.
                 *
                 * The WAIT phase above is counted (StatSubmitSyncFail); this,
                 * the SIGNAL phase, was not - so a submit whose command stream
                 * ran to completion and then failed to signal reported a bare
                 * ENOENT to the UMD while EVERY kernel counter stayed 0.
                 * Measured: mesa logging ~8 GROUP_SUBMIT ret=-2 per 2 min with
                 * StatSubmitSyncFail/NoGroup/CsfFailCount all reading 0, and
                 * CsOk climbing the whole time. Contradictory counters at the
                 * two ends of one interface localise the bug to the code
                 * between them - and this was the only uninstrumented line in
                 * it. Count it, and name the handle. */
                (VOID)InterlockedIncrement(&g_WinMaliStatSubmitSignalFail);
                (VOID)InterlockedExchange(&g_WinMaliStatSignalErr, (LONG)e);
                /* ⛔★★★★★ ARM THE RING SNAPSHOT ON THE **FIRST** FAILURE.
                 *
                 * Every attempt to capture this burst from a timer-published
                 * tail came back empty - and NOT because the code never ran.
                 * The 79 failures all land ~57 s after boot, WinRM only becomes
                 * reachable ~70 s in, and the traffic in between scrolls even a
                 * 192 KB slice. Enlarging the buffer is a dead end: the ring
                 * itself is only 256 KB (WINMALI_RING_BYTES 0x40000).
                 *
                 * So TRIGGER instead of POLL: latch 0 -> 1 here, and let the
                 * present worker snapshot the ring into its own registry value
                 * on the next tick (~2 s) and latch to 2, so the first failure's
                 * context is preserved and never overwritten by later traffic.
                 * WMSIGENOENT1 (WinMaliSync.c) prints the missing handle, the
                 * live table count and the calling pid - this is what finally
                 * delivers it. */
                (VOID)InterlockedCompareExchange(&g_WinMaliTrapSignalFail, 1, 0);
                if (qs->SyncsCount > 0) {
                    ULONG k;
                    /* Report the first SIGNAL-direction handle, not syncs[0] -
                       syncs[0] is usually a WAIT and would name the wrong one. */
                    for (k = 0; k < qs->SyncsCount; ++k) {
                        if (syncs[k].Flags & WINMALI_SYNC_OP_SIGNAL) {
                            (VOID)InterlockedExchange(&g_WinMaliStatSignalBadHandle,
                                                      (LONG)syncs[k].Handle);
                            break;
                        }
                    }
                }
                WINMALI_WARN("GroupSubmit: group=%u queue=%u the CS RAN but the "
                             "SIGNAL sync ops failed err=%d (count=%u) - this is a "
                             "different bug from a rejected WAIT",
                             args->GroupHandle, qs->QueueIndex, e, qs->SyncsCount);
                err = e;
            }
        }
    }

    return err;
}

static int
WinMaliEscapeSyncObjWait_(_In_    PWINMALI_ADAPTER       adapter,
                          _Inout_ WINMALI_ESCAPE_HEADER* hdr,
                          _In_    const DXGKARG_ESCAPE*  pEscape)
{
    WINMALI_SYNC_OBJ_WAIT* args =
        (WINMALI_SYNC_OBJ_WAIT*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
    const ULONG*  handles;
    const UINT64* points;
    BOOLEAN waitAll;
    if (args == NULL || args->Count == 0) {
        return WMERR_EINVAL;
    }
    handles = (const ULONG*)WinMaliEscapeAt_(pEscape, args->HandlesOffset,
                                             args->Count * sizeof(ULONG));
    if (handles == NULL) {
        return WMERR_EINVAL;
    }
    points = NULL;
    if (args->PointsOffset != 0) {
        points = (const UINT64*)WinMaliEscapeAt_(pEscape, args->PointsOffset,
                                                  args->Count * sizeof(UINT64));
        if (points == NULL) {
            return WMERR_EINVAL;
        }
    }
    waitAll = (args->Flags & WINMALI_SYNC_OBJ_WAIT_FLAG_ALL) ? TRUE : FALSE;
    {
        /* ⛔ SAME HAZARD AS THE SUBMIT PATH, DIFFERENT DOOR. The UMD reaches
           here with INT64_MAX (panfrost_bo_wait(bo, INT64_MAX) -> bo_wait ->
           syncobj_timeline_wait), and a negative TimeoutNs means "infinite" to
           WinMaliSyncObjWait. Either way a wedged GPU parks a user thread in
           the kernel forever: unkillable process, PnP lock never released,
           machine down. The caller's timeout is honoured up to a ceiling; past
           that it is a hang, not a wait. */
        INT64 t = args->TimeoutNs;
        if (t < 0 || t > WINMALI_SYNC_WAIT_MAX_NS) {
            t = WINMALI_SYNC_WAIT_MAX_NS;
        }
        return WinMaliSyncObjWait(adapter, args->Count, handles, points,
                                  waitAll, t);
    }
}

/* -------------------------------------------------------------------------- */
/*  DxgkDdiEscape entry                                                        */
/* -------------------------------------------------------------------------- */

_Function_class_(DXGKDDI_ESCAPE)
NTSTATUS
APIENTRY
WinMaliKmdEscape(
    IN_CONST_HANDLE          hAdapter,
    IN_CONST_PDXGKARG_ESCAPE pEscape)
{
    PWINMALI_ADAPTER       adapter;
    WINMALI_ESCAPE_HEADER* hdr;

    if (pEscape == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pEscape->pPrivateDriverData == NULL ||
        pEscape->PrivateDriverDataSize < sizeof(WINMALI_ESCAPE_HEADER)) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = WinMaliAdapterFromDxgkHandle((PVOID)hAdapter);
    if (adapter == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    hdr = (WINMALI_ESCAPE_HEADER*)pEscape->pPrivateDriverData;

    /* Foreign escape buffer (some other driver's UMD probing every D3DKMT
       adapter via D3DKMTEscape)? Magic mismatch is the one path where we
       MUST return non-SUCCESS so the calling UMD sees STATUS_INVALID_PARAMETER
       and moves on without us clobbering its private buffer. */
    if (hdr->Magic != WINMALI_ESCAPE_MAGIC) {
        return STATUS_INVALID_PARAMETER;
    }

    /* From here on we own the buffer and report status via hdr->Status. */
    if (hdr->AbiMajor != WINMALI_ABI_MAJOR) {
        WINMALI_WARN("Escape: ABI major mismatch (UMD=%u, KMD=%u)",
                     hdr->AbiMajor, WINMALI_ABI_MAJOR);
        hdr->Status = WMERR_EINVAL;
        return STATUS_SUCCESS;
    }
    if (hdr->PayloadSize > pEscape->PrivateDriverDataSize) {
        WINMALI_WARN("Escape: PayloadSize %u > buffer %u",
                     hdr->PayloadSize, pEscape->PrivateDriverDataSize);
        hdr->Status = WMERR_EINVAL;
        return STATUS_SUCCESS;
    }

    switch (hdr->Op) {
    case WinMaliEscapeOp_OpenDevice:
        hdr->Status = WinMaliEscapeOpenDevice_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_DevQuery:
        hdr->Status = WinMaliEscapeDevQuery_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_BoCreate:
        hdr->Status = WinMaliEscapeBoCreate_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_BoDestroy:
        hdr->Status = WinMaliEscapeBoDestroy_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_BoQueryInfo:
        hdr->Status = WinMaliEscapeBoQueryInfo_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_BoSetLabel:
        hdr->Status = WinMaliEscapeBoSetLabel_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_BoMapCpu:
        hdr->Status = WinMaliEscapeBoMapCpu_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_DebugLog:
        hdr->Status = WinMaliEscapeDebugLog_(adapter, hdr, pEscape);
        break;

    case WinMaliEscapeOp_SyncObjCreate:
        hdr->Status = WinMaliEscapeSyncObjCreate_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_SyncObjDestroy:
        hdr->Status = WinMaliEscapeSyncObjDestroy_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_SyncObjSignal:
        hdr->Status = WinMaliEscapeSyncObjSignal_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_SyncObjWait:
        hdr->Status = WinMaliEscapeSyncObjWait_(adapter, hdr, pEscape);
        break;

    case WinMaliEscapeOp_VmCreate:
        hdr->Status = WinMaliEscapeVmCreate_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_VmDestroy:
        hdr->Status = WinMaliEscapeVmDestroy_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_VmGetState:
        hdr->Status = WinMaliEscapeVmGetState_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_VmBind:
        hdr->Status = WinMaliEscapeVmBind_(adapter, hdr, pEscape);
        break;

    case WinMaliEscapeOp_GroupCreate:
        hdr->Status = WinMaliEscapeGroupCreate_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_GroupDestroy:
        hdr->Status = WinMaliEscapeGroupDestroy_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_GroupGetState:
        hdr->Status = WinMaliEscapeGroupGetState_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_GroupSubmit:
        hdr->Status = WinMaliEscapeGroupSubmit_(adapter, hdr, pEscape);
        break;

    case WinMaliEscapeOp_TilerHeapCreate: {
        /* Allocate the heap-context descriptor + initial chunk ring, map them
           into the group's VM, and return real GPU VAs the tiler can walk.
           On failure we return an error (not a fake unmapped placeholder VA):
           a bogus heap would only fault later during real geometry, so the
           UMD is better off seeing the failure. */
        WINMALI_TILER_HEAP_CREATE* args =
            (WINMALI_TILER_HEAP_CREATE*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
        if (args == NULL) {
            hdr->Status = WMERR_EINVAL;
        } else if (args->VmId == 0 || args->InitialChunkCount == 0) {
            hdr->Status = WMERR_EINVAL;
        } else {
            UINT64   realCtxVa = 0, realFirstVa = 0;
            NTSTATUS heapSt = WinMaliTilerHeapCreateReal_(
                                  adapter, WinMaliDeviceOwnerToken(pEscape->hDevice),
                                  args->VmId, args->ChunkSize,
                                  args->InitialChunkCount,
                                  &realCtxVa, &realFirstVa);
            if (NT_SUCCESS(heapSt)) {
                args->HeapHandle = (ULONG)InterlockedIncrement(
                                       &adapter->GroupTable.NextHandle) | 0x80000000u;
                args->FirstChunkGpuVa   = realFirstVa;
                args->TilerHeapCtxGpuVa = realCtxVa;
                WINMALI_TRACE("TilerHeapCreate: handle=0x%x vm=%u chunk=%u count=%u "
                              "ctx_va=0x%llx first_chunk_va=0x%llx",
                              args->HeapHandle, args->VmId, args->ChunkSize,
                              args->InitialChunkCount,
                              (ULONGLONG)realCtxVa, (ULONGLONG)realFirstVa);
                hdr->Status = WMERR_OK;
            } else {
                WINMALI_WARN("TilerHeapCreate: vm=%u real heap alloc failed 0x%08x",
                             args->VmId, heapSt);
                hdr->Status = (heapSt == STATUS_INSUFFICIENT_RESOURCES)
                                  ? WMERR_ENOMEM : WMERR_EINVAL;
            }
        }
        break;
    }
    case WinMaliEscapeOp_TilerHeapDestroy: {
        WINMALI_TILER_HEAP_DESTROY* args =
            (WINMALI_TILER_HEAP_DESTROY*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
        if (args == NULL) {
            hdr->Status = WMERR_EINVAL;
        } else {
            WINMALI_TRACE("TilerHeapDestroy: handle=0x%x (metadata-only)", args->HeapHandle);
            hdr->Status = WMERR_OK;
        }
        break;
    }
    case WinMaliEscapeOp_BoMmapOffset: {
        /* On Windows mesa uses BoMapCpu (Op 0x86), not mmap(). This op
           exists for source-compat with pan_kmod's "get the magic offset
           to pass to mmap" pattern. Returning the BO handle as the offset
           is opaque and unique - mesa doesn't actually dereference it on
           Windows. */
        WINMALI_BO_MMAP_OFFSET* args =
            (WINMALI_BO_MMAP_OFFSET*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
        PWINMALI_BO bo;
        if (args == NULL) {
            hdr->Status = WMERR_EINVAL;
        } else {
            bo = WinMaliBoGet(adapter, args->Handle);
            if (bo == NULL) {
                hdr->Status = WMERR_ENOENT;
            } else {
                args->Offset = ((UINT64)args->Handle) << 12;  /* page-aligned synthetic */
                WinMaliBoPut(bo);
                hdr->Status = WMERR_OK;
            }
        }
        break;
    }
    case WinMaliEscapeOp_BoSync: {
        /* Mali on the G610 with CacheCoherent segments is coherent with
           the CPU caches the way we've configured the MMU (memattr_lo=0x4c).
           No explicit flush is required. If we later add non-coherent
           segments we'll need to call KeInvalidateRangeAllCaches or similar
           per op. Acknowledge the request so the UMD doesn't error. */
        WINMALI_TRACE("Escape BoSync: no-op (caches are coherent)");
        hdr->Status = WMERR_OK;
        break;
    }

    case WinMaliEscapeOp_GetFlushId: {
        WINMALI_GET_FLUSH_ID* args =
            (WINMALI_GET_FLUSH_ID*)WinMaliEscapeAt_(pEscape, sizeof(*hdr), sizeof(*args));
        if (args == NULL) {
            hdr->Status = WMERR_EINVAL;
        } else {
            args->FlushId = 0;
            args->Pad     = 0;
            hdr->Status   = WMERR_OK;
        }
        break;
    }

    case WinMaliEscapeOp_BoFromAllocation:
        hdr->Status = WinMaliEscapeBoFromAllocation_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_BoFromAllocId:
        hdr->Status = WinMaliEscapeBoFromAllocId_(adapter, hdr, pEscape);
        break;
    case WinMaliEscapeOp_BoFromUserVa:
        hdr->Status = WinMaliEscapeBoFromUserVa_(adapter, hdr, pEscape);
        break;

    /* PresentToHdc stays unimplemented: a KMD cannot blit to a GDI HDC.
       On-screen output rides the DXGI path (pfnPresentCb -> DxgkDdiPresent
       Blt -> SubmitCommand CPU copy). */
    case WinMaliEscapeOp_PresentToHdc:
        WINMALI_TRACE("Escape op 0x%x not yet implemented", hdr->Op);
        hdr->Status = WMERR_EOPNOTSUPP;
        break;

    default:
        WINMALI_WARN("Escape: unknown op 0x%x", hdr->Op);
        hdr->Status = WMERR_EOPNOTSUPP;
        break;
    }

    return STATUS_SUCCESS;
}

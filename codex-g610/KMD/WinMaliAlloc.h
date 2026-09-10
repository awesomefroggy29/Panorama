/*
 * WinMaliAlloc.h - allocation lifecycle types + DDI prototypes.
 *
 * Shared between WinMaliAlloc.c (impl), WinMaliPaging.c (future, paging
 * buffer ops touch the same kmd allocation handles), and any future
 * present/render code that needs to read the alloc's GpuVa / Pitch /
 * dimensions.
 *
 * UMD ↔ KMD allocation private-data ABI is locked here. Any version
 * bump must be reflected in Mesa-modern's render-only winsys.
 */

#pragma once

#include <ntddk.h>
#include <dispmprt.h>
#include <d3dkmddi.h>
#include "../Shared/WinMaliEscape.h"

/* ---------------- Allocation usage bits (UMD <-> KMD ABI) ---------------- */
/* WINMALI_ALLOC_PRIV and usage bits live in WinMaliEscape.h because the UMD
   must decode the same blob when opening a cross-process allocation. */
typedef WINMALI_ALLOC_PRIV* PWINMALI_ALLOC_PRIV;

/* ---------------- WINMALI_KMD_ALLOCATION - per-alloc kernel state -------- */

#define WINMALI_KMD_ALLOC_MAGIC      'AllW'
#define WINMALI_ALLOC_IDENTITY_MAX   64u

typedef struct _WINMALI_KMD_ALLOCATION {
    ULONG               Magic;        /* 'AllW' */
    struct _WINMALI_ADAPTER* Adapter;
    LIST_ENTRY          RegistryEntry;
    BOOLEAN             RegistryLinked;
    D3DKMT_HANDLE       RuntimeHandle;
    HANDLE              RuntimeHandleProcessId;
    ULONGLONG           RegistryId;
    HANDLE              CreateProcessId;
    HANDLE              CreateThreadId;
    UINT                PrivateIdentitySize;
    UCHAR               PrivateIdentity[WINMALI_ALLOC_IDENTITY_MAX];
    HANDLE              hResource;    /* dxgk's hResource or NULL */
    SIZE_T              Size;         /* page-aligned bytes */
    UINT                Alignment;
    UINT                Width;
    UINT                Height;
    UINT                Pitch;        /* row stride in bytes */
    UINT                Format;       /* D3DDDIFORMAT (or DXGI_FORMAT for
                                         UMD 'WMAl' backbuffer allocs) */
    UINT                Usage;        /* WINMALI_ALLOC_USAGE_* */
    /* Residency state (filled by BuildPagingBuffer when the alloc gets a
       GPU VA / sysmem page set). Until then both are zero. */
    UINT64              GpuVa;
    PHYSICAL_ADDRESS    PhysicalBase;
    LONG                ReferenceCount; /* create owner + device opens */
#if defined(WINMALI_HANDLE_LIFECYCLE_PROBE)
    ULONGLONG           LifecycleId;
    ULONG               AllocationPrivateHash;
    ULONG               ResourcePrivateHash;
#endif
    /* Aperture residency, stashed by BuildPagingBuffer MAP_APERTURE_SEGMENT
       and cleared at UNMAP. The MDL is VidMm's - it is only valid while the
       allocation is mapped (present blts and BoFromAllocation both require
       the allocation to be resident; the UMD pins backbuffers via LockCb). */
    PMDL                ApertureMdl;
    ULONG               ApertureMdlOffset;   /* pages into the MDL */
    ULONG               AperturePageCount;
} WINMALI_KMD_ALLOCATION, *PWINMALI_KMD_ALLOCATION;

/* ---------------- WINMALI_KMD_OPEN_ALLOCATION - per-device state -------- */

/* DxgkDdiOpenAllocation creates one of these for each device that opens an
   allocation. It is deliberately distinct from WINMALI_KMD_ALLOCATION:
   hDeviceSpecificAllocation is device-scoped, while the handle returned by
   DxgkDdiCreateAllocation is adapter/global allocation state owned by VidMm. */
#define WINMALI_KMD_OPEN_ALLOC_MAGIC 'OllW'

typedef struct _WINMALI_KMD_OPEN_ALLOCATION {
    ULONG                       Magic;
    struct _WINMALI_ADAPTER*    Adapter;
    HANDLE                      hDevice;
    D3DKMT_HANDLE               hAllocation;
    SIZE_T                      Size;
    UINT                        Alignment;
    UINT                        Width;
    UINT                        Height;
    UINT                        Pitch;
    UINT                        Format;
    UINT                        Usage;
    BOOLEAN                     ReadOnly;
    /* Non-device-specific allocation state returned by CreateAllocation.
       OpenAllocation owns one reference until CloseAllocation. */
    PWINMALI_KMD_ALLOCATION     Global;
} WINMALI_KMD_OPEN_ALLOCATION, *PWINMALI_KMD_OPEN_ALLOCATION;

/* ---------------- Present (Blt) descriptor ------------------------------ */
/* DxgkDdiPresent stashes this in the DMA buffer's private data; the paging
   NOP path in SubmitCommand ignores buffers whose private data doesn't start
   with this magic, so present can't regress memory management. Kept in
   private data (not the DMA buffer proper) so the src/dst kernel pointers
   survive Present -> SubmitCommand without going through dxgk patching. */
#define WINMALI_BLT_DESC_MAGIC   0x746C4257u  /* 'WBlt' */

typedef struct _WINMALI_BLT_DESC {
    ULONG                    Magic;      /* WINMALI_BLT_DESC_MAGIC */
    ULONG                    SubRectCnt;
    PWINMALI_KMD_ALLOCATION  Src;        /* referenced rendered back buffer */
    PWINMALI_KMD_ALLOCATION  Dst;        /* referenced present target */
    LONG                     SrcLeft, SrcTop;
    LONG                     DstLeft, DstTop, DstRight, DstBottom;
} WINMALI_BLT_DESC, *PWINMALI_BLT_DESC;

/* WINMALI_CS_SUBMIT_DESC (Phase 3) is defined in the shared header
   Shared/WinMaliEscape.h so the UMD and KMD agree on it. */

/* ---------------- DDI prototypes ---------------------------------------- */

NTSTATUS APIENTRY WinMaliKmdCreateAllocation(
    IN_CONST_HANDLE                 hAdapter,
    INOUT_PDXGKARG_CREATEALLOCATION pCreateAllocation);

NTSTATUS APIENTRY WinMaliKmdPresent(
    IN_CONST_HANDLE         hContext,
    INOUT_PDXGKARG_PRESENT  pPresent);

NTSTATUS APIENTRY WinMaliKmdDestroyAllocation(
    IN_CONST_HANDLE                     hAdapter,
    IN_CONST_PDXGKARG_DESTROYALLOCATION pDestroyAllocation);

NTSTATUS APIENTRY WinMaliKmdOpenAllocation(
    IN_CONST_HANDLE                  hDevice,
    IN_CONST_PDXGKARG_OPENALLOCATION pOpenAllocation);

NTSTATUS APIENTRY WinMaliKmdCloseAllocation(
    IN_CONST_HANDLE                   hDevice,
    IN_CONST_PDXGKARG_CLOSEALLOCATION pCloseAllocation);

NTSTATUS APIENTRY WinMaliKmdDescribeAllocation(
    IN_CONST_HANDLE                   hAdapter,
    INOUT_PDXGKARG_DESCRIBEALLOCATION pDescribeAllocation);

NTSTATUS APIENTRY WinMaliKmdGetStandardAllocationDriverData(
    IN_CONST_HANDLE                                hAdapter,
    INOUT_PDXGKARG_GETSTANDARDALLOCATIONDRIVERDATA pGetStandardAllocationDriverData);

#include "WinMaliKmd.h"
#include "WinMaliMmu.h"
#include "winmali_shader_programs.h"

#define WINMALI_FW_IMAGE_MAX_BYTES   (4u * 1024u * 1024u)
#define WINMALI_FW_PT_POOL_BYTES     (1024u * 1024u)
#define WINMALI_FW_MAX_SECTIONS      24u

#define CSF_FW_BINARY_HEADER_MAGIC        0xc3f13a6eu
#define CSF_FW_BINARY_HEADER_MAJOR_MAX   0u

#define CSF_FW_BINARY_ENTRY_TYPE(ehdr)       ((ULONG)(ehdr)&0xffu)
#define CSF_FW_BINARY_ENTRY_SIZE(ehdr)       (((ULONG)(ehdr) >> 8) & 0xffu)
#define CSF_FW_BINARY_ENTRY_OPTIONAL         (1u << 31)
#define CSF_FW_BINARY_ENTRY_TYPE_IFACE        0u

#define CSF_FW_BINARY_IFACE_ENTRY_RD_WR                 (1u << 1)
#define CSF_FW_BINARY_IFACE_ENTRY_RD_EX                 (1u << 2)
#define CSF_FW_BINARY_IFACE_ENTRY_RD_CACHE_MODE_MASK    (3u << 3)
#define CSF_FW_BINARY_IFACE_ENTRY_RD_SHARED              (1u << 30)
#define CSF_FW_BINARY_IFACE_ENTRY_RD_ZERO               (1u << 31)
#define CSF_FW_BINARY_IFACE_ENTRY_RD_PROT               (1u << 5)
#define CSF_FW_BINARY_IFACE_ENTRY_RD_SUPPORTED_FLAGS     0xC000003Fu

#define CSF_MCU_SHARED_REGION_START       0x04000000ul

#define GLB_CFG_PROGRESS_TIMER          (1u << 1)
#define GLB_CFG_ALLOC_EN                (1u << 2)
#define GLB_CFG_POWEROFF_TIMER          (1u << 3)
#define GLB_PING                         (1u << 8)
#define GLB_IDLE_EN                      (1u << 10)
#define GLB_IDLE                         (1u << 26)
#define GLB_TIMER_VAL(x)                 ((ULONG)(x)&0x7fffffffu)
#define GLB_TIMER_SOURCE_GPU_COUNTER     (1u << 31)

#define PROGRESS_TIMEOUT_CYCLES          (5ull * 500ull * 1024ull * 1024ull)
#define PROGRESS_TIMEOUT_SCALE_SHIFT     10u

#define WINMALI_CSF_GROUP_CONTROL_OFFSET   0x1000u
#define WINMALI_CSF_STREAM_CONTROL_OFFSET  0x40u
#define WINMALI_CSF_MIN_CS_PER_CSG         8u
#define WINMALI_CSF_MIN_CSGS               3u
#define WINMALI_CSF_MAX_CSGS               31u
#define WINMALI_CSF_MAX_CS_PER_CSG         32u
#define WINMALI_CSF_UNPRESERVED_REG_COUNT  4u

#define WINMALI_CS_STATE_MASK              7u
#define WINMALI_CS_STATE_STOP              0u
#define WINMALI_CS_STATE_START             1u
#define WINMALI_CS_EXTRACT_EVENT           (1u << 4)
#define WINMALI_CS_IDLE_SYNC_WAIT          (1u << 8)
#define WINMALI_CS_IDLE_EMPTY              (1u << 10)
/* ⛔★★★★★ BIT 26 IS THE ONE THAT WEDGED THE GPU FOR THE WHOLE PROJECT.
 *
 * The FW raises CS_ACK bit 26 (TILER_OOM) when the tiler heap runs out of
 * chunks, then PARKS the command stream until the host links in a new chunk and
 * toggles CS_REQ bit 26 to match. Nothing in this driver ever read it, and
 * WINMALI_CS_REQ_MASK below omits it, so WinMaliCsfCsUpdateReqs_ strips bit 26
 * from every REQ write - the request could not be acknowledged even in principle.
 * There was also no tiler-heap grow path at all (Create + Destroy only), so
 * there was nothing to hand back. First exhaustion = permanent wedge.
 *
 * MEASURED at the FIRST timeout of a boot 2026-08-20, and identically 2026-08-18:
 *     CsAck = 0x04000011  (bit26 SET)      CsReq = 0x00000511  (bit26 clear)
 *     CsOk frozen at 1146, CsTimeouts climbing, CsfLastAs=6
 *     bit30 FATAL = 0/0, bit31 FAULT = 0/0, every MMU/GPU fault bank zero
 * The zeros are not "nothing is wrong" - TILER_OOM is a REQUEST, not a fault.
 *
 * ⚠ WINMALI_CS_REQ_MASK is deliberately left UNCHANGED. Adding TILER_OOM to it
 * would let WinMaliCsfCsUpdateReqs_ (called on every CSG start) write bit 26 as
 * 0 and so CLEAR a pending acknowledgement. WinMaliCsfServiceTilerOom_ writes
 * csIn->Req directly instead, which is the only place that bit should move.
 * bit30/bit31 are defined for decoding only: acking a real FAULT/FATAL is a
 * behaviour change needing its own measurement, and both read 0 at the wedge. */
#define WINMALI_CS_TILER_OOM               (1u << 26)
#define WINMALI_CS_FATAL                   (1u << 30)
#define WINMALI_CS_FAULT                   (1u << 31)
#define WINMALI_CS_REQ_MASK                (WINMALI_CS_STATE_MASK | WINMALI_CS_EXTRACT_EVENT \
                                              | WINMALI_CS_IDLE_SYNC_WAIT | WINMALI_CS_IDLE_EMPTY)

#define WINMALI_CS_CONFIG_PRIORITY(x)      ((ULONG)(x)&0xfu)
#define WINMALI_CS_CONFIG_DOORBELL(x)      (((ULONG)(x)&0xffu) << 8)

#define WINMALI_CSG_STATE_MASK             7u
#define WINMALI_CSG_STATE_TERMINATE        0u
#define WINMALI_CSG_STATE_START            1u
#define WINMALI_CSG_STATE_SUSPEND          2u
#define WINMALI_CSG_STATE_RESUME           3u
#define WINMALI_CSG_ENDPOINT_CONFIG        (1u << 4)
#define WINMALI_CSG_STATUS_UPDATE          (1u << 5)
#define WINMALI_CSG_SYNC_UPDATE            (1u << 28)
#define WINMALI_CSG_IDLE                   (1u << 29)
#define WINMALI_CSG_PROGRESS_TIMER_EVENT   (1u << 31)
#define WINMALI_CSG_EVT_MASK               (WINMALI_CSG_SYNC_UPDATE | WINMALI_CSG_IDLE | WINMALI_CSG_PROGRESS_TIMER_EVENT)
#define WINMALI_CSG_REQ_MASK               (WINMALI_CSG_STATE_MASK | WINMALI_CSG_ENDPOINT_CONFIG)

#define WINMALI_CSG_EP_REQ_COMPUTE(x)      ((ULONG)(x)&0xffu)
#define WINMALI_CSG_EP_REQ_FRAGMENT(x)     (((ULONG)(x)&0xffu) << 8)
#define WINMALI_CSG_EP_REQ_TILER(x)        (((ULONG)(x)&0xfu) << 16)
#define WINMALI_CSG_EP_REQ_PRIORITY(x)     (((ULONG)(x)&0xfu) << 28)

#define WINMALI_CSF_RING_BYTES             (64u * 1024u)
//
// "Tag" slot inside the same 4 KiB sync page. The trampoline does a second
// SYNC_ADD64 on this address with a fixed pattern; the kernel reads it back
// post-job to obtain a *second*, independent witness that the GPU actually
// committed real bytes the CPU can observe — not just a counter that could
// in principle be racing. Sync object lives in the first 16 bytes; offset
// 64 keeps a comfortable cacheline gap.
//
#define WINMALI_CSF_TAG_OFFSET             64u
#define WINMALI_CSF_TAG_PATTERN            0x00000000CAFEBABEull
#define WINMALI_CSF_QUEUE_IFACE_BYTES      (8u * 1024u)
#define WINMALI_CSF_NUM_INSTRS_PER_SLOT    16u

#define WINMALI_CSF_RING_GPU_VA            0x410000ull
#define WINMALI_CSF_SHADER_GPU_VA          0x420000ull
#define WINMALI_CSF_SYNC_GPU_VA            0x421000ull

//
// Per-Mesa v10.xml the SPD lives at the start of the descriptor page;
// the Local Storage struct must be 64 B aligned; the FAU table only needs
// 16 B alignment. Using offsets 0 / 64 / 128 satisfies all three with
// room to spare in a single 4 KiB page.
//
#define WINMALI_CSF_DESC_SPD_OFFSET        0u
#define WINMALI_CSF_DESC_TSD_OFFSET        64u
#define WINMALI_CSF_DESC_FAU_OFFSET        128u

#define WINMALI_CSF_REAL_SHADER_PATTERN    0x07060504ul

//
// Mali Valhall Shader Program Descriptor (CSF, v10).
// Mirrors mesa/src/panfrost/lib/genxml/v10.xml struct "Shader Program"
// (size=8 dwords, align=32). For our minimal compute test:
//   word0 = Type=Shader(8) | Stage=Compute(1)<<4 = 0x18
//   word1 = Preload  (bit 0 = preload R48-R49 from FAU)
//   word2/3 = Binary GPU VA (48-bit address in low bits)
//   words 4..7 = padding / zero
//
#pragma pack(push, 4)
typedef struct _WINMALI_VALHALL_SPD {
    UINT32 Word0_TypeStageFlags;
    UINT32 Word1_Preload;
    UINT64 Binary;
    UINT64 Reserved0;
    UINT64 Reserved1;
} WINMALI_VALHALL_SPD;
C_ASSERT(sizeof(WINMALI_VALHALL_SPD) == 32);

//
// Mali Valhall Local Storage / Thread Storage Descriptor (CSF, v10).
// Mirrors mesa/src/panfrost/lib/genxml/v10.xml struct "Local Storage"
// (size=8 dwords, align=64). For 1×1×1 / no TLS / no WLS:
//   word0 = TLS Size = 0
//   word1 = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM (= 0x80000000 sentinel)
//   word2/3 = TLS Base = 0
//   word4/5 = WLS Base = 0
//   word6/7 = padding
//
#define WINMALI_LOCAL_STORAGE_NO_WORKGROUP_MEM   0x80000000ul

typedef struct _WINMALI_VALHALL_LOCAL_STORAGE {
    UINT32 TlsSize;
    UINT32 WlsWord;
    UINT64 TlsBase;
    UINT64 WlsBase;
    UINT64 Pad;
} WINMALI_VALHALL_LOCAL_STORAGE;
C_ASSERT(sizeof(WINMALI_VALHALL_LOCAL_STORAGE) == 32);
#pragma pack(pop)

#define WINMALI_VALHALL_DESC_TYPE_SHADER     8u
#define WINMALI_VALHALL_SHADER_STAGE_COMPUTE 1u
#define WINMALI_VALHALL_PRELOAD_R48_R49      (1u << 0)

#pragma pack(push, 1)

typedef struct _WINMALI_FW_BINARY_HDR {
    ULONG Magic;
    UCHAR Minor;
    UCHAR Major;
    USHORT Padding1;
    ULONG VersionHash;
    ULONG Padding2;
    ULONG Size;
} WINMALI_FW_BINARY_HDR;

typedef struct _WINMALI_FW_BINARY_SECTION_ENTRY_HDR {
    ULONG Flags;
    struct {
        ULONG Start;
        ULONG End;
    } Va;
    struct {
        ULONG Start;
        ULONG End;
    } Data;
} WINMALI_FW_BINARY_SECTION_ENTRY_HDR;

typedef struct _WINMALI_PANTHOR_FW_GLOBAL_CONTROL_IFACE {
    ULONG Version;
    ULONG Features;
    ULONG InputVa;
    ULONG OutputVa;
    ULONG GroupNum;
    ULONG GroupStride;
    ULONG PerfcntSize;
    ULONG InstrFeatures;
} WINMALI_PANTHOR_FW_GLOBAL_CONTROL_IFACE;

typedef struct _WINMALI_PANTHOR_FW_GLOBAL_OUTPUT_IFACE {
    ULONG Ack;
    ULONG Reserved1;
    ULONG DoorbellAck;
    ULONG Reserved2;
    ULONG HaltStatus;
    ULONG PerfcntStatus;
    ULONG PerfcntInsert;
} WINMALI_PANTHOR_FW_GLOBAL_OUTPUT_IFACE;

typedef struct _WINMALI_PANTHOR_FW_GLOBAL_INPUT_IFACE {
    ULONG Req;
    ULONG AckIrqMask;
    ULONG DoorbellReq;
    ULONG Reserved1;
    ULONG ProgressTimer;
    ULONG PoweroffTimer;
    UINT64 CoreEnMask;
    ULONG Reserved2;
    ULONG PerfcntAs;
    UINT64 PerfcntBase;
    ULONG PerfcntExtract;
    ULONG Reserved3[3];
    ULONG PerfcntConfig;
    ULONG PerfcntCsgSelect;
    ULONG PerfcntFwEnable;
    ULONG PerfcntCsgEnable;
    ULONG PerfcntCsfEnable;
    ULONG PerfcntShaderEnable;
    ULONG PerfcntTilerEnable;
    ULONG PerfcntMmuL2Enable;
    ULONG Reserved4[8];
    ULONG IdleTimer;
} WINMALI_PANTHOR_FW_GLOBAL_INPUT_IFACE;

#pragma pack(pop)

#pragma pack(push, 4)

typedef struct _WINMALI_PANTHOR_FW_CS_CONTROL_IFACE {
    ULONG Features;
    ULONG InputVa;
    ULONG OutputVa;
} WINMALI_PANTHOR_FW_CS_CONTROL_IFACE;

typedef struct _WINMALI_PANTHOR_FW_CS_INPUT_IFACE {
    ULONG Req;
    ULONG Config;
    ULONG Reserved1;
    ULONG AckIrqMask;
    UINT64 RingbufBase;
    ULONG RingbufSize;
    ULONG Reserved2;
    UINT64 HeapStart;
    UINT64 HeapEnd;
    UINT64 RingbufInput;
    UINT64 RingbufOutput;
    ULONG InstrConfig;
    ULONG InstrbufSize;
    UINT64 InstrbufBase;
    UINT64 InstrbufOffsetPtr;
} WINMALI_PANTHOR_FW_CS_INPUT_IFACE;

typedef struct _WINMALI_PANTHOR_FW_CS_OUTPUT_IFACE {
    ULONG Ack;
    ULONG Reserved1[15];
    UINT64 StatusCmdPtr;
    ULONG StatusWait;
    ULONG StatusReqResource;
    UINT64 StatusWaitSyncPtr;
    ULONG StatusWaitSyncValue;
    ULONG StatusScoreboards;
    ULONG StatusBlockedReason;
    ULONG StatusWaitSyncValueHi;
    ULONG Reserved2[6];
    ULONG Fault;
    ULONG Fatal;
    UINT64 FaultInfo;
    UINT64 FatalInfo;
    ULONG Reserved3[10];
    ULONG HeapVtStart;
    ULONG HeapVtEnd;
    ULONG Reserved4;
    ULONG HeapFragEnd;
    UINT64 HeapAddress;
} WINMALI_PANTHOR_FW_CS_OUTPUT_IFACE;

typedef struct _WINMALI_PANTHOR_FW_CSG_CONTROL_IFACE {
    ULONG Features;
    ULONG InputVa;
    ULONG OutputVa;
    ULONG SuspendSize;
    ULONG ProtmSuspendSize;
    ULONG StreamNum;
    ULONG StreamStride;
} WINMALI_PANTHOR_FW_CSG_CONTROL_IFACE;

typedef struct _WINMALI_PANTHOR_FW_CSG_INPUT_IFACE {
    ULONG Req;
    ULONG AckIrqMask;
    ULONG DoorbellReq;
    ULONG CsIrqAck;
    ULONG Reserved1[4];
    UINT64 AllowCompute;
    UINT64 AllowFragment;
    ULONG AllowOther;
    ULONG EndpointReq;
    ULONG Reserved2[2];
    UINT64 SuspendBuf;
    UINT64 ProtmSuspendBuf;
    ULONG Config;
    ULONG IterTraceConfig;
} WINMALI_PANTHOR_FW_CSG_INPUT_IFACE;

typedef struct _WINMALI_PANTHOR_FW_CSG_OUTPUT_IFACE {
    ULONG Ack;
    ULONG Reserved1;
    ULONG DoorbellAck;
    ULONG CsIrqReq;
    ULONG StatusEndpointCurrent;
    ULONG StatusEndpointReq;
    ULONG StatusState;
    ULONG ResourceDep;
} WINMALI_PANTHOR_FW_CSG_OUTPUT_IFACE;

typedef struct _WINMALI_FW_RINGBUF_INPUT_IFACE {
    UINT64 Insert;
    UINT64 Extract;
} WINMALI_FW_RINGBUF_INPUT_IFACE;

typedef struct _WINMALI_FW_RINGBUF_OUTPUT_IFACE {
    UINT64 Extract;
    ULONG Active;
    ULONG Reserved0;
} WINMALI_FW_RINGBUF_OUTPUT_IFACE;

typedef struct _WINMALI_PANTHOR_SYNCOBJ64 {
    UINT64 Seqno;
    ULONG Status;
    ULONG Pad;
} WINMALI_PANTHOR_SYNCOBJ64;

#pragma pack(pop)

typedef struct _WINMALI_FW_SECTION {
    ULONG              Flags;
    ULONG              VaStart;
    ULONG              VaEnd;
    PVOID              BackingVa;
    PHYSICAL_ADDRESS   BackingPa;
    SIZE_T             BackingBytes;
    PUCHAR             InitCopy;
    SIZE_T             InitSize;
} WINMALI_FW_SECTION;

typedef struct _WINMALI_FW_ITER {
    PUCHAR Data;
    SIZE_T Size;
    SIZE_T Offset;
} WINMALI_FW_ITER;

typedef struct _WINMALI_FWCTX {
    PVOID              Image;
    SIZE_T             ImageSize;
    ULONG              SectionCount;
    WINMALI_FW_SECTION Sections[WINMALI_FW_MAX_SECTIONS];
    ULONG              SharedIndex;
    UINT64             L2Mask;
    PVOID              PtPoolVa;
    PHYSICAL_ADDRESS   PtPoolPa;
    SIZE_T             PtPoolBytes;
    SIZE_T             PtUsed;
    PUINT64            RootHostVa;
    UINT64             RootTablePa;
    ULONG              VaBits;
    KSPIN_LOCK         GlbLock;
    volatile WINMALI_PANTHOR_FW_GLOBAL_CONTROL_IFACE* GlbControl;
    volatile WINMALI_PANTHOR_FW_GLOBAL_INPUT_IFACE* GlbInput;
    volatile WINMALI_PANTHOR_FW_GLOBAL_OUTPUT_IFACE* GlbOutput;

    // --- CSF slot interfaces (panthor_fw_init_ifaces + bootstrap queue) ---
    PWINMALI_ADAPTER    OwnerAdapter;
    BOOLEAN             CsfIfaceValid;
    BOOLEAN             CsfBootValid;
    ULONG               CsfGroupNum;
    ULONG               CsfGroupStride;
    ULONG               CsfStreamNum;
    ULONG               CsfStreamStride;
    ULONG               CsRegCount;
    ULONG               CsUnpreservedRegCount;
    ULONG               SbSlotCount;
    UINT64              McuDynNextVa;
    KSPIN_LOCK          CsfIfaceLock;

    volatile WINMALI_PANTHOR_FW_CSG_CONTROL_IFACE* Csg0Control;
    volatile WINMALI_PANTHOR_FW_CSG_INPUT_IFACE*   Csg0Input;
    volatile WINMALI_PANTHOR_FW_CSG_OUTPUT_IFACE*  Csg0Output;
    volatile WINMALI_PANTHOR_FW_CS_CONTROL_IFACE*  Cs00Control;
    volatile WINMALI_PANTHOR_FW_CS_INPUT_IFACE*    Cs00Input;
    volatile WINMALI_PANTHOR_FW_CS_OUTPUT_IFACE*   Cs00Output;

    PVOID                      HostRingVa;
    PHYSICAL_ADDRESS           RingPa;
    volatile WINMALI_FW_RINGBUF_INPUT_IFACE*  QRingIn;
    volatile WINMALI_FW_RINGBUF_OUTPUT_IFACE* QRingOut;
    UINT64                     McuQueueIfaceVa;
    PVOID                      HostQueueIfaceVa;
    PHYSICAL_ADDRESS           QueueIfacePa;

    PVOID                      HostSyncVa;
    PHYSICAL_ADDRESS           SyncPa;

    PVOID                      HostSuspendVa;
    SIZE_T                     SuspendBytes;
    PHYSICAL_ADDRESS           SuspendPa;
    UINT64                     McuSuspendVa;

    PVOID                      HostProtmVa;
    SIZE_T                     ProtmBytes;
    PHYSICAL_ADDRESS           ProtmPa;
    UINT64                     McuProtmVa;

    PVOID                      HostShaderVa;
    PHYSICAL_ADDRESS           ShaderPa;

    UINT64                     KqJobSeq;

    // Which MMU AS the single bring-up CSG slot is currently bound to. Set
    // to the bring-up AS after bootstrap; real UMD group submits rebind it
    // to the group's VM AS so the CSG executes the UMD's stream (and all its
    // referenced buffers) in the group's own address space. Serialized by
    // the escape dispatch (one GroupSubmit at a time).
    ULONG                      CurrentCsgAs;

    // Endpoint parameters captured at bootstrap (constant for the device):
    // the shader-core mask and its popcount. Reused when the CSG slot is
    // reprogrammed for a UMD group so the bind path doesn't re-read MMIO.
    UINT64                     ShaderCoreMask;
    ULONG                      MaxCores;

    // TRUE once the bring-up CSG slot has been STATE_START'd (by bootstrap or
    // a group bind) and not since TERMINATE'd. The per-group bind path uses
    // this to decide whether it must TERMINATE the currently-bound group
    // before reprogramming the slot for a new group (a clean STOP->reprogram
    // ->START cycle, as panthor does - never a live AS swap on a running slot).
    BOOLEAN                    CsgStarted;

    // Serializes the whole bind->submit->wait sequence. There is ONE bring-up
    // CSG slot + ring shared by every process, but dxgk delivers Escape
    // concurrently across devices; without this, two processes interleave
    // CSG TERMINATE/START and ring writes and corrupt each other's submit.
    KMUTEX                     SubmitLock;
} WINMALI_FWCTX;

static UINT64
WinMaliFwMakeL3Pte_(_In_ UINT64 physPa, _In_ UINT64 attrs)
{
    // Combines the 4 KiB-aligned output address with the caller-chosen L3
    // attribute mask. Callers pass one of the WINMALI_LPAE_L3_PAGE_ATTR_*
    // variants from WinMaliMmu.h (RW_EX / RO_EX / RW_NX / RO_NX), tightened
    // per FW section in WinMaliFwBuildPtForSections_.
    return (physPa & WINMALI_LPAE_PA_MASK) | attrs;
}

//
// Derive the right L3 PTE attribute mask from a CSF FW section's flags.
// This mirrors panthor's flags_to_prot() in panthor_fw.c — RD/WR/EX bits
// in the FW binary header become AP[2] / PXN+UXN in the page descriptor.
//
// Section flags we observe on Mali-G610 / mali_csffw.bin:
//   0x09           = RD + cache_mode=CACHED                  → RO + NX
//   0x0d           = RD + EX + cache_mode=CACHED             → RO + EX (code)
//   0x8000000b     = RD + WR + cache_mode=CACHED + ZERO      → RW + NX (BSS data)
//   0x80000009     = RD + cache_mode=CACHED + ZERO           → RO + NX
//   0xc000001b     = RD + WR + cache_mode=CC + SHARED + ZERO → RW + NX (shared iface)
//
static UINT64
WinMaliFwSectionPteAttrs_(_In_ ULONG SectionFlags)
{
    BOOLEAN writable   = (SectionFlags & CSF_FW_BINARY_IFACE_ENTRY_RD_WR) != 0u;
    BOOLEAN executable = (SectionFlags & CSF_FW_BINARY_IFACE_ENTRY_RD_EX) != 0u;

    if (writable && executable) {
        return WINMALI_LPAE_L3_PAGE_ATTR_RW_EX;
    }
    if (writable && !executable) {
        return WINMALI_LPAE_L3_PAGE_ATTR_RW_NX;
    }
    if (!writable && executable) {
        return WINMALI_LPAE_L3_PAGE_ATTR_RO_EX;
    }
    return WINMALI_LPAE_L3_PAGE_ATTR_RO_NX;
}

static const CHAR*
WinMaliFwSectionPermsName_(_In_ UINT64 Attrs)
{
    BOOLEAN ro = (Attrs & WINMALI_LPAE_L3_AP_RO) != 0ull;
    BOOLEAN nx = (Attrs & WINMALI_LPAE_L3_NX)    == WINMALI_LPAE_L3_NX;
    if ( ro &&  nx) return "RO+NX";
    if ( ro && !nx) return "RO+EX";
    if (!ro &&  nx) return "RW+NX";
    return "RW+EX";
}

//
// Decoders for AS_FAULTSTATUS — Mali MMU AArch64 fault classes.
//
// ✅ SOURCE (VERIFIED 2026-08-13): extracted from kbase_gpu_exception_name() in
// the STOCK ANDROID vendor driver for this exact SoC —
// /vendor_dlkm/lib/modules/mali_kbase_mt6897_r44.ko, pulled off the rooted
// reference tablet. The module ships unstripped, so the mapping was read out of
// the compiler's own jump table rather than guessed:
//
//     sub  w8, w0, #5              idx  = code - 5   (valid 0x05..0xEB)
//     ldrb w11, [x9, x8]           byte table @ .rodata+0xd180
//     add  x10, x10, x11, lsl #2   case = 0x5f0cc + 4*tbl[idx]
//     br   x10                     ... each case: adrp/add x0, str ; ret
//
// (see scratchpad/decode_exc.py). Every name below is that binary's string.
// Self-check on the decode: 0xC2 came out "TRANSLATION_FAULT at level 2",
// matching the TRANSLATION_FAULT_2 we have actually observed on the wire, and
// the 0xC0|level / 0xC8|level / 0xE4|level families fall exactly where the
// documented Mali MMU encoding puts them.
//
// Deliberately NOT mapped, because r44 does not map them either: 0xD0-0xD3
// (TRANSTAB_BUS_FAULT — Midgard/Bifrost-era, gone in CSF), 0xD8 (there is no
// level-0 access flag) and 0xEC-0xEF (MEM_ATTR_NONCACHE). We used to print
// names for all nine; a name on a code this GPU cannot emit is a trap, not a
// convenience.
//
static const CHAR*
WinMaliFwDecodeMmuExc_(_In_ ULONG FaultStatus)
{
    switch (FaultStatus & 0xffu) {
    // GPU-level faults. These share the exception-type encoding space with the
    // MMU classes and DO reach AS_FAULTSTATUS, so they belong here too.
    case 0x80: return "GPU_BUS_FAULT";
    case 0x88: return "GPU_SHAREABILITY_FAULT";
    case 0x89: return "SYSTEM_SHAREABILITY_FAULT";
    case 0x8A: return "GPU_CACHEABILITY_FAULT";
    case 0xC0: return "TRANSLATION_FAULT_0";
    case 0xC1: return "TRANSLATION_FAULT_1";
    case 0xC2: return "TRANSLATION_FAULT_2";
    case 0xC3: return "TRANSLATION_FAULT_3";
    case 0xC4: return "TRANSLATION_FAULT";      // r44 prints no level for 0xC4
    case 0xC8: return "PERM_FAULT_0";
    case 0xC9: return "PERM_FAULT_1";
    case 0xCA: return "PERM_FAULT_2";
    case 0xCB: return "PERM_FAULT_3";
    case 0xD9: return "ACCESS_FLAG_1";
    case 0xDA: return "ACCESS_FLAG_2";
    case 0xDB: return "ACCESS_FLAG_3";
    case 0xE0: return "ADDR_SIZE_FAULT_IN";
    case 0xE4: return "ADDR_SIZE_FAULT_OUT_0";
    case 0xE5: return "ADDR_SIZE_FAULT_OUT_1";
    case 0xE6: return "ADDR_SIZE_FAULT_OUT_2";
    case 0xE7: return "ADDR_SIZE_FAULT_OUT_3";
    case 0xE8: return "MEM_ATTR_FAULT_0";
    case 0xE9: return "MEM_ATTR_FAULT_1";
    case 0xEA: return "MEM_ATTR_FAULT_2";
    case 0xEB: return "MEM_ATTR_FAULT_3";
    default:   return "UNKNOWN";
    }
}

static const CHAR*
WinMaliFwDecodeAccessType_(_In_ ULONG FaultStatus)
{
    // panthor_regs.h: AS_FAULTSTATUS_ACCESS_TYPE_MASK = 0x3 << 8.
    switch ((FaultStatus >> 8) & 0x3u) {
    case 0: return "ATOMIC";
    case 1: return "EXEC";
    case 2: return "READ";
    case 3: return "WRITE";
    }
    return "?";
}

//
// CS_FAULT / CS_FATAL bits[7:0] carry an exception type describing an error
// raised BY THE COMMAND STREAM, which is a different register bank from
// AS_FAULTSTATUS above. Decoding matters because the two disagree: a CS that
// dies with CS_FATAL=0x40 leaves every MMU/GPU fault register at zero, so a
// report keyed only on those prints "fault: NONE" over a GPU that plainly
// faulted. That exact line cost us a wrong reading tonight.
//
// ✅ SOURCE (VERIFIED 2026-08-13): same extraction as WinMaliFwDecodeMmuExc_
// above — kbase_gpu_exception_name() out of the stock mali_kbase_mt6897_r44.ko.
// This table was previously written from the Arm CSF spec and carried an
// UNVERIFIED warning; the binary confirmed all but one entry and supplied the
// vendor's exact spelling. Corrections it forced:
//
//   0x41  was MISSING entirely — a CS_UNRECOVERABLE printed "UNRECOGNISED".
//   0x44/0x49/0x4A carry a FATAL_ prefix in the vendor naming: these are
//         CS_FATAL-only codes, and the prefix is the tell that the CS is dead
//         rather than merely faulted.
//
// One trap worth recording: grepping the .ko's strings for "CS_ENDPOINT_FAULT"
// and "CS_CALL_STACK_OVERFLOW" hits, which looks like they are separate codes.
// They are not — .rodata.str1.1 tail-merges, so those are the suffixes of the
// FATAL_ strings and have no relocation of their own. Only the jump table says
// what a code actually maps to.
//
static const CHAR*
WinMaliFwDecodeCsExc_(_In_ ULONG Exc)
{
    switch (Exc & 0xffu) {
    case 0x00: return "NONE";
    case 0x0F: return "CS_RESOURCE_TERMINATED";
    case 0x40: return "CS_CONFIG_FAULT (CS/queue descriptor misconfigured)";
    case 0x41: return "CS_UNRECOVERABLE";
    case 0x44: return "FATAL_CS_ENDPOINT_FAULT";
    case 0x48: return "CS_BUS_FAULT";
    case 0x49: return "FATAL_CS_INVALID_INSTRUCTION";
    case 0x4A: return "FATAL_CS_CALL_STACK_OVERFLOW";
    case 0x4B: return "CS_INHERIT_FAULT";
    case 0x50: return "INSTR_INVALID_PC";
    case 0x51: return "INSTR_INVALID_ENC";
    case 0x55: return "INSTR_BARRIER_FAULT";
    case 0x58: return "DATA_INVALID_FAULT";
    case 0x59: return "TILE_RANGE_FAULT";
    case 0x5A: return "ADDR_RANGE_FAULT";
    case 0x5B: return "IMPRECISE_FAULT";
    case 0x68: return "FIRMWARE_INTERNAL_ERROR";
    case 0x69: return "RESOURCE_EVICTION_TIMEOUT";
    default:
        // A CS exception can also carry a GPU/MMU-level code (the encoding
        // space is shared), so fall through to that decoder before giving up.
        // Its default is "UNKNOWN", which is still honest.
        return WinMaliFwDecodeMmuExc_(Exc);
    }
}

static NTSTATUS
WinMaliFwIterRead_(_Inout_ WINMALI_FW_ITER* It, _Out_writes_bytes_(Len) PVOID Dst, _In_ SIZE_T Len)
{
    SIZE_T n;
    if (It == NULL || Dst == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    n = It->Offset + Len;
    if (n > It->Size || n < It->Offset) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(Dst, It->Data + It->Offset, Len);
    It->Offset = n;
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwIterSubInit_(_Inout_ WINMALI_FW_ITER* Parent, _Out_ WINMALI_FW_ITER* Sub, _In_ SIZE_T PayloadBytes)
{
    SIZE_T n = Parent->Offset + PayloadBytes;
    if (n > Parent->Size || n < Parent->Offset) {
        return STATUS_INVALID_PARAMETER;
    }
    Sub->Data   = Parent->Data + Parent->Offset;
    Sub->Size   = PayloadBytes;
    Sub->Offset = 0;
    Parent->Offset = n;
    return STATUS_SUCCESS;
}

static PVOID
WinMaliFwMcuVaToCpu_(_In_ WINMALI_FW_SECTION* S, _In_ ULONG McuVa)
{
    if (S == NULL || S->BackingVa == NULL) {
        return NULL;
    }
    if (McuVa < S->VaStart || McuVa >= S->VaEnd) {
        return NULL;
    }
    return (PUCHAR)S->BackingVa + (SIZE_T)(((UINT64)McuVa) - (UINT64)S->VaStart);
}

static NTSTATUS
WinMaliFwPtAllocPage_(_Inout_ WINMALI_FWCTX* Fw, _Out_ UINT64* Phys, _Out_ PVOID* HostVa)
{
    if (Fw->PtUsed + 4096u > Fw->PtPoolBytes) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *HostVa = (PUCHAR)Fw->PtPoolVa + Fw->PtUsed;
    RtlZeroMemory(*HostVa, 4096);
    *Phys = Fw->PtPoolPa.QuadPart + (UINT64)Fw->PtUsed;
    Fw->PtUsed += 4096u;
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwPtEnsureChild_(
    _Inout_ WINMALI_FWCTX* Fw,
    _Inout_ PUINT64       ParentTable,
    _In_ ULONG            Idx,
    _Out_ PUINT64*        OutChildHost,
    _Out_ UINT64*         OutChildPhys)
{
    UINT64 ent = ParentTable[Idx];
    if ((ent & 3ull) == 3ull && (ent & 0x0000fffffffff000ull) != 0ull) {
        UINT64 childPa = ent & 0x0000fffffffff000ull;
        *OutChildPhys = childPa;
        *OutChildHost =
            (PUINT64)((PUCHAR)Fw->PtPoolVa + (SIZE_T)(childPa - Fw->PtPoolPa.QuadPart));
        return STATUS_SUCCESS;
    }
    return WinMaliFwPtAllocPage_(Fw, OutChildPhys, (PVOID*)OutChildHost);
}

static NTSTATUS
WinMaliFwPtMap4k_(
    _Inout_ WINMALI_FWCTX* Fw,
    _In_ UINT64            McuVa,
    _In_ UINT64            PhysPa,
    _In_ UINT64            Attrs)
{
    NTSTATUS status;
    PUINT64  t;
    ULONG    level;
    UINT64   childPa;
    PUINT64  childHost;

    if (Fw->RootHostVa == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    t = Fw->RootHostVa;
    for (level = 0; level < 3u; ++level) {
        ULONG shift = 39u - (9u * level);
        ULONG idx   = (ULONG)((McuVa >> shift) & 0x1ffull);
        status = WinMaliFwPtEnsureChild_(Fw, t, idx, &childHost, &childPa);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (t[idx] == 0) {
            t[idx] = childPa | 3ull;
            KeMemoryBarrier();
        }
        t = childHost;
    }
    {
        ULONG i3 = (ULONG)((McuVa >> 12) & 0x1ffull);
        t[i3] = WinMaliFwMakeL3Pte_(PhysPa, Attrs);
        KeMemoryBarrier();
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwBuildPtForSections_(_Inout_ WINMALI_FWCTX* Fw)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG    si;
    UINT64   va;
    ULONG    totalPagesMapped = 0;

    for (si = 0; si < Fw->SectionCount; ++si) {
        WINMALI_FW_SECTION* s = &Fw->Sections[si];
        ULONG               pagesThisSection = 0;
        UINT64              attrs;
        if (s->BackingVa == NULL || s->VaEnd <= s->VaStart) {
            WINMALI_TRACE(
                "FW: PT skip section[%lu] (no backing) VA=[0x%08x..0x%08x)",
                si,
                s->VaStart,
                s->VaEnd);
            continue;
        }
        attrs = WinMaliFwSectionPteAttrs_(s->Flags);
        for (va = s->VaStart; va < (UINT64)s->VaEnd; va += 4096ull) {
            UINT64 off  = va - (UINT64)s->VaStart;
            UINT64 phys = s->BackingPa.QuadPart + off;
            status        = WinMaliFwPtMap4k_(Fw, va, phys, attrs);
            if (!NT_SUCCESS(status)) {
                WINMALI_WARN(
                    "FW: PT map failed at section[%lu] VA=0x%llx PA=0x%llx status=0x%08x (PtUsed=%lu/%lu KiB)",
                    si,
                    (ULONGLONG)va,
                    (ULONGLONG)phys,
                    status,
                    (ULONG)(Fw->PtUsed / 1024u),
                    (ULONG)(Fw->PtPoolBytes / 1024u));
                return status;
            }
            pagesThisSection++;
        }
        totalPagesMapped += pagesThisSection;
        WINMALI_TRACE(
            "FW: PT mapped section[%lu] VA=[0x%08x..0x%08x) pages=%lu flags=0x%08x perms=%s pte_attr=0x%016llx backing_pa=0x%llx",
            si,
            s->VaStart,
            s->VaEnd,
            pagesThisSection,
            s->Flags,
            WinMaliFwSectionPermsName_(attrs),
            (ULONGLONG)attrs,
            (ULONGLONG)s->BackingPa.QuadPart);
    }
    WINMALI_TRACE(
        "FW: PT build done sections=%lu pages=%lu PtUsed=%lu/%lu KiB",
        Fw->SectionCount,
        totalPagesMapped,
        (ULONG)(Fw->PtUsed / 1024u),
        (ULONG)(Fw->PtPoolBytes / 1024u));
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwPollPwrTransClear_(_In_ PWINMALI_HW Hw, _In_ ULONG RegLo, _In_ UINT64 Mask, _In_ ULONG TimeoutUs)
{
    ULONG i;
    ULONG waited;
    for (i = 0; i < 2u; ++i) {
        ULONG mask32 = (ULONG)(Mask >> (i * 32));
        ULONG reg    = RegLo + (i * 4u);
        if (mask32 == 0) {
            continue;
        }
        for (waited = 0; waited < TimeoutUs; waited += 10) {
            ULONG v = WinMaliHwRead32(Hw, reg);
            if ((mask32 & v) == 0) {
                break;
            }
            KeStallExecutionProcessor(10);
        }
        if (waited >= TimeoutUs) {
            return STATUS_IO_TIMEOUT;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwPollL2Ready_(_In_ PWINMALI_HW Hw, _In_ UINT64 Mask, _In_ ULONG TimeoutUs)
{
    ULONG i;
    ULONG waited;
    for (i = 0; i < 2u; ++i) {
        ULONG mask32 = (ULONG)(Mask >> (i * 32));
        ULONG reg    = WINMALI_REG_L2_READY_LO + (i * 4u);
        if (mask32 == 0) {
            continue;
        }
        for (waited = 0; waited < TimeoutUs; waited += 10) {
            ULONG v = WinMaliHwRead32(Hw, reg);
            if ((mask32 & v) == mask32) {
                break;
            }
            KeStallExecutionProcessor(10);
        }
        if (waited >= TimeoutUs) {
            return STATUS_IO_TIMEOUT;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwGpuL2PowerOn_(_Inout_ PWINMALI_ADAPTER Adapter, _Out_ UINT64* OutMask)
{
    NTSTATUS status;
    UINT64   l2Present;
    UINT64   mask;
    ULONG    lo;
    ULONG    hi;

    lo = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_L2_PRESENT_LO);
    hi = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_L2_PRESENT_HI);
    l2Present = ((UINT64)hi << 32) | (UINT64)lo;

    if (l2Present == 1ull) {
        mask = 1ull;
    } else if (l2Present == 0ull) {
        mask = 1ull;
        WINMALI_WARN("FW L2: GPU_L2_PRESENT is 0; using mask=1");
    } else {
        mask = (~(l2Present - 1ull)) & (l2Present - 2ull);
    }
    *OutMask = mask;

    /* Report COHERENCY_FEATURES only - do NOT write GPU_COHERENCY_PROTOCOL.
       Confirmed this GPU reads features=0x00000000 (no ACE-lite/ACE support),
       i.e. genuinely non-coherent, so CPU-side cache maintenance around submit
       is the correct model (see WinMaliBoFlushVm). The v49 protocol WRITE was
       reverted with the soft-reset as part of restoring the working cold boot. */
    {
        ULONG cohFeat = WinMaliHwRead32(&Adapter->Hw,
                                        WINMALI_REG_GPU_COHERENCY_FEATURES);
        WINMALI_TRACE("FW: coherency features=0x%08x (non-coherent; no protocol write)",
                      cohFeat);
    }

    /*
     * ★ MT6897 INSTRUMENTATION. "L2 power-on failed 0xc00000b5" is ambiguous: there are TWO
     * polls here and both return STATUS_IO_TIMEOUT, so the message alone does not say whether
     * the GPU never finished a PRIOR power transition or never asserted L2_READY after our
     * PWRON write. Those have completely different causes. Log the mask and the live registers
     * at each step so the next attempt is aimed rather than guessed.
     *
     * Context: firmware has now powered ALL 14 MFG domains (RPC leaves 0x7EDE) and the GPU
     * runs at 265 MHz, and this still times out -- so it is neither a dark shader domain nor
     * a starved clock. Both were tested on hardware and refuted.
     */
    WINMALI_TRACE("FW-L2: mask=0x%llx L2_PRESENT=%08x/%08x READY=%08x/%08x PWRTRANS=%08x/%08x",
                  mask,
                  WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_L2_PRESENT_LO),
                  WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_L2_PRESENT_HI),
                  WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_L2_READY_LO),
                  WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_L2_READY_HI),
                  WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_L2_PWRTRANS_LO),
                  WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_L2_PWRTRANS_HI));

    status = WinMaliFwPollPwrTransClear_(&Adapter->Hw, WINMALI_REG_L2_PWRTRANS_LO, mask, 20000);
    if (!NT_SUCCESS(status)) {
        WINMALI_TRACE("FW-L2: *** PWRTRANS never cleared *** PWRTRANS=%08x GPU_STATUS=%08x",
                      WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_L2_PWRTRANS_LO),
                      WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_STATUS));
        return status;
    }
    if ((mask & 0xffffffffull) != 0) {
        WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_L2_PWRON_LO, (ULONG)(mask & 0xffffffffull));
    }
    if ((mask >> 32) != 0) {
        WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_L2_PWRON_HI, (ULONG)(mask >> 32));
    }
    status = WinMaliFwPollL2Ready_(&Adapter->Hw, mask, 20000);
    if (!NT_SUCCESS(status)) {
        WINMALI_TRACE("FW-L2: *** L2_READY never asserted *** READY=%08x PWRTRANS=%08x "
                      "GPU_STATUS=%08x GPU_FAULT=%08x IRQ_RAW=%08x",
                      WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_L2_READY_LO),
                      WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_L2_PWRTRANS_LO),
                      WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_STATUS),
                      WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_FAULT_STATUS),
                      WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_IRQ_RAWSTAT));
    }
    return status;
}
/* ⛔★★★★★ EXPLICIT SHADER-CORE POWER-ON - OPT-IN RECOVERY, NOT THE NORM.
 *
 * MEASURED 2026-08-22 from the first-timeout trap: SHADER_READY(any)=0x0 while
 * SHADER_PRESENT=0x130012, with L2_READY=1 and TILER_READY=1. Three tiler-only
 * submits succeeded and the FIRST REAL DRAW parked forever - CMD_PTR=0,
 * BLOCKED_REASON=0, and every fault bank (MMU, GPU, CS_FAULT, CS_FATAL) zero.
 * A hang with NO fault bits is a POWER problem, not a command-stream problem;
 * every previous theory here treated the CS as misprogrammed.
 *
 * On CSF the MCU owns core power and normally powers them on demand, so this is
 * DISABLED unless Parameters\ShaderPwrOn is set: writing PWRON behind the
 * firmware's back races it for a domain it believes it owns, which is its own
 * class of bug. Use it to confirm the diagnosis and to recover a GPU whose cores
 * never came up (seen after a hot rebind), not as standing behaviour.
 *
 * Mirrors WinMaliFwGpuL2PowerOn_ exactly: wait for any in-flight transition to
 * settle, write PWRON, then POLL READY - never assume the write took. */
static NTSTATUS
WinMaliFwShaderPowerOn_(_Inout_ PWINMALI_ADAPTER Adapter)
{
    UINT64   present;
    ULONG    readyLo = 0, i;
    NTSTATUS status;

    if (g_WinMaliShaderPwrOn == 0) {
        return STATUS_SUCCESS;   /* not armed - leave the MCU alone */
    }

    present = ((UINT64)WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_SHADER_PRESENT_HI) << 32)
            |  (UINT64)WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_SHADER_PRESENT_LO);
    if (present == 0) {
        WINMALI_WARN("SHADER PWRON: SHADER_PRESENT is 0 - no cores to power");
        return STATUS_UNSUCCESSFUL;
    }

    if (WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_SHADER_READY_LO) != 0) {
        return STATUS_SUCCESS;   /* already up - do not disturb the MCU */
    }

    WINMALI_TRACE("SHADER PWRON: present=0x%llx ready=0 - powering cores explicitly",
                  present);

    status = WinMaliFwPollPwrTransClear_(&Adapter->Hw, WINMALI_REG_SHADER_PWRTRANS_LO,
                                         present, 20000);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("SHADER PWRON: PWRTRANS never cleared (%08x) - not writing PWRON",
                     WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_SHADER_PWRTRANS_LO));
        return status;
    }

    if ((present & 0xffffffffull) != 0) {
        WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_SHADER_PWRON_LO,
                         (ULONG)(present & 0xffffffffull));
    }
    if ((present >> 32) != 0) {
        WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_SHADER_PWRON_HI,
                         (ULONG)(present >> 32));
    }

    /* Poll READY rather than trusting the write: the whole point is that the
       cores were not coming up, so fire-and-forget would prove nothing. */
    for (i = 0; i < 20000u; ++i) {
        readyLo = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_SHADER_READY_LO);
        if (readyLo != 0) {
            break;
        }
        KeStallExecutionProcessor(1);
    }

    (VOID)InterlockedExchange(&g_WinMaliStatShaderPwrOn, (LONG)readyLo);
    if (readyLo == 0) {
        WINMALI_WARN("SHADER PWRON: *** READY still 0 after explicit PWRON *** "
                     "present=0x%llx pwrtrans=%08x - the cores refuse to come up",
                     present,
                     WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_SHADER_PWRTRANS_LO));
        return STATUS_IO_TIMEOUT;
    }
    WINMALI_TRACE("SHADER PWRON: cores UP, READY=0x%08x after %u polls", readyLo, i);
    return STATUS_SUCCESS;
}


static NTSTATUS
WinMaliFwGpuL2PowerOff_(_Inout_ PWINMALI_ADAPTER Adapter, _In_ UINT64 Mask)
{
    NTSTATUS status;

    status = WinMaliFwPollPwrTransClear_(&Adapter->Hw, WINMALI_REG_L2_PWRTRANS_LO, Mask, 20000);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if ((Mask & 0xffffffffull) != 0) {
        WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_L2_PWROFF_LO, (ULONG)(Mask & 0xffffffffull));
    }
    if ((Mask >> 32) != 0) {
        WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_L2_PWROFF_HI, (ULONG)(Mask >> 32));
    }
    status = WinMaliFwPollPwrTransClear_(&Adapter->Hw, WINMALI_REG_L2_PWRTRANS_LO, Mask, 20000);
    return status;
}

static NTSTATUS
WinMaliFwReadFileImage_(_Out_ PVOID* OutBuf, _Out_ SIZE_T* OutSize)
{
    NTSTATUS          status;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    path;
    IO_STATUS_BLOCK   iosb;
    HANDLE            h = NULL;
    FILE_STANDARD_INFORMATION fsi;
    PVOID             buf = NULL;

    *OutBuf  = NULL;
    *OutSize = 0;

    RtlInitUnicodeString(&path, L"\\SystemRoot\\System32\\drivers\\mali_csffw.bin");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwCreateFile(
        &h,
        GENERIC_READ | SYNCHRONIZE,
        &oa,
        &iosb,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL,
        0);
    if (!NT_SUCCESS(status)) {
        WINMALI_TRACE("FW: open mali_csffw.bin -> 0x%08x", status);
        return status;
    }

    status = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        goto out_close;
    }

    *OutSize = (SIZE_T)fsi.EndOfFile.QuadPart;
    if (*OutSize == 0 || *OutSize > WINMALI_FW_IMAGE_MAX_BYTES) {
        status = STATUS_FILE_INVALID;
        goto out_close;
    }

    buf = ExAllocatePoolWithTag(NonPagedPoolNx, *OutSize, WINMALI_POOL_TAG);  /* 1809 WDK: ExAllocatePool2 not present */
    if (buf == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out_close;
    }

    status = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)*OutSize, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(buf, WINMALI_POOL_TAG);
        buf = NULL;
        *OutSize = 0;
        goto out_close;
    }

    *OutBuf = buf;

out_close:
    ZwClose(h);
    return status;
}

static VOID
WinMaliFwGlbUpdateReqs_(
    _Inout_ PKSPIN_LOCK                               Lk,
    _Inout_ volatile WINMALI_PANTHOR_FW_GLOBAL_INPUT_IFACE* In,
    _In_ ULONG                                        Val,
    _In_ ULONG                                        Mask)
{
    KIRQL irql;
    ULONG cur;
    ULONG n;
    KeAcquireSpinLock(Lk, &irql);
    cur = In->Req;
    n   = (cur & ~Mask) | (Val & Mask);
    In->Req = n;
    KeReleaseSpinLock(Lk, irql);
}

static VOID
WinMaliFwGlbToggleReqs_(
    _Inout_ PKSPIN_LOCK                                 Lk,
    _Inout_ volatile WINMALI_PANTHOR_FW_GLOBAL_INPUT_IFACE* In,
    _In_ volatile WINMALI_PANTHOR_FW_GLOBAL_OUTPUT_IFACE* Out,
    _In_ ULONG                                          Mask)
{
    KIRQL irql;
    ULONG cur;
    ULONG o;
    ULONG n;
    KeAcquireSpinLock(Lk, &irql);
    cur = In->Req;
    o   = Out->Ack;
    n   = ((o ^ Mask) & Mask) | (cur & ~Mask);
    In->Req = n;
    KeReleaseSpinLock(Lk, irql);
}

static VOID
WinMaliFwInitGlobalIface_(_Inout_ PWINMALI_ADAPTER Adapter, _Inout_ WINMALI_FWCTX* Fw)
{
    UINT64 shader;
    ULONG  lo = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_SHADER_PRESENT_LO);
    ULONG  hi = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_SHADER_PRESENT_HI);

    shader = ((UINT64)hi << 32) | (UINT64)lo;

    Fw->GlbInput->CoreEnMask = shader;
    Fw->GlbInput->PoweroffTimer = GLB_TIMER_VAL(~0u) | GLB_TIMER_SOURCE_GPU_COUNTER;
    Fw->GlbInput->ProgressTimer =
        (ULONG)(PROGRESS_TIMEOUT_CYCLES >> PROGRESS_TIMEOUT_SCALE_SHIFT);
    Fw->GlbInput->IdleTimer = GLB_TIMER_VAL(~0u) | GLB_TIMER_SOURCE_GPU_COUNTER;
    Fw->GlbInput->AckIrqMask =
        GLB_CFG_ALLOC_EN | GLB_PING | GLB_CFG_PROGRESS_TIMER | GLB_CFG_POWEROFF_TIMER
        | GLB_IDLE_EN | GLB_IDLE;

    WinMaliFwGlbUpdateReqs_(&Fw->GlbLock, Fw->GlbInput, GLB_IDLE_EN, GLB_IDLE_EN);
    WinMaliFwGlbToggleReqs_(
        &Fw->GlbLock,
        Fw->GlbInput,
        Fw->GlbOutput,
        GLB_CFG_ALLOC_EN | GLB_CFG_POWEROFF_TIMER | GLB_CFG_PROGRESS_TIMER);

    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_CSF_DOORBELL(0), 1u);
}

static VOID
WinMaliFwInitMcuDynVa_(_Inout_ WINMALI_FWCTX* Fw)
{
    ULONG si;
    UINT64 maxEnd = 0;
    for (si = 0; si < Fw->SectionCount; ++si) {
        if (Fw->Sections[si].VaEnd > maxEnd) {
            maxEnd = Fw->Sections[si].VaEnd;
        }
    }
    Fw->McuDynNextVa = (maxEnd + 65535ull) & ~65535ull;
    if (Fw->McuDynNextVa < maxEnd) {
        Fw->McuDynNextVa = maxEnd;
    }
}

static PVOID
WinMaliFwIfaceFwToCpu_(_In_ WINMALI_FWCTX* Fw, _In_ ULONG McuVa)
{
    WINMALI_FW_SECTION* sh;
    if (Fw->SharedIndex >= Fw->SectionCount) {
        return NULL;
    }
    sh = &Fw->Sections[Fw->SharedIndex];
    if (sh->BackingVa == NULL || McuVa < sh->VaStart || McuVa >= sh->VaEnd) {
        return NULL;
    }
    return (PUCHAR)sh->BackingVa + (SIZE_T)(((UINT64)McuVa) - (UINT64)sh->VaStart);
}

static NTSTATUS
WinMaliFwMcuMapContiguous_(
    _Inout_ WINMALI_FWCTX* Fw,
    _In_ PVOID              HostVa,
    _In_ PHYSICAL_ADDRESS    HostPa,
    _In_ SIZE_T              Bytes,
    _In_ UINT64              Align,
    _In_ UINT64              Attrs,
    _Out_ UINT64*            OutMcuVa)
{
    NTSTATUS status;
    UINT64   base;
    SIZE_T   pages;
    SIZE_T   pi;

    if (OutMcuVa == NULL || HostVa == NULL || Bytes == 0 || Align < 4096ull) {
        return STATUS_INVALID_PARAMETER;
    }
    base = (Fw->McuDynNextVa + Align - 1ull) & ~(Align - 1ull);
    pages  = (Bytes + 4095u) / 4096u;
    for (pi = 0; pi < pages; ++pi) {
        status = WinMaliFwPtMap4k_(
            Fw,
            base + (UINT64)pi * 4096ull,
            HostPa.QuadPart + (UINT64)pi * 4096ull,
            Attrs);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    Fw->McuDynNextVa = base + (UINT64)pages * 4096ull;
    *OutMcuVa = base;
    UNREFERENCED_PARAMETER(HostVa);
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwInitCsfIfaces_(_Inout_ WINMALI_FWCTX* Fw)
{
    ULONG gn;
    ULONG gs;
    ULONG csg;
    ULONG cs;

    if (Fw->GlbControl == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    gn = Fw->GlbControl->GroupNum;
    gs = Fw->GlbControl->GroupStride;
    if (gn < WINMALI_CSF_MIN_CSGS || gn > WINMALI_CSF_MAX_CSGS || gs == 0u || gs > 0x100000u) {
        WINMALI_WARN("FW: invalid CSF group layout (group_num=%lu stride=%lu)", gn, gs);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    KeInitializeSpinLock(&Fw->CsfIfaceLock);
    KeInitializeMutex(&Fw->SubmitLock, 0);
    Fw->CsfGroupNum    = gn;
    Fw->CsfGroupStride = gs;

    for (csg = 0; csg < gn; ++csg) {
        ULONG               off = WINMALI_CSF_GROUP_CONTROL_OFFSET + csg * gs;
        volatile WINMALI_PANTHOR_FW_CSG_CONTROL_IFACE* cctl;
        volatile WINMALI_PANTHOR_FW_CSG_INPUT_IFACE*   cin;
        volatile WINMALI_PANTHOR_FW_CSG_OUTPUT_IFACE*  cout;
        ULONG sn;
        ULONG st;

        cctl = (volatile WINMALI_PANTHOR_FW_CSG_CONTROL_IFACE*)((PUCHAR)Fw->GlbControl + off);
        if (cctl->StreamNum < WINMALI_CSF_MIN_CS_PER_CSG || cctl->StreamNum > WINMALI_CSF_MAX_CS_PER_CSG) {
            WINMALI_WARN("FW: CSG%u bad stream_num=%lu", csg, cctl->StreamNum);
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        if (csg == 0) {
            Fw->CsfStreamNum    = cctl->StreamNum;
            Fw->CsfStreamStride = cctl->StreamStride;
        } else {
            volatile WINMALI_PANTHOR_FW_CSG_CONTROL_IFACE* c0 =
                (volatile WINMALI_PANTHOR_FW_CSG_CONTROL_IFACE*)((PUCHAR)Fw->GlbControl
                                                                 + WINMALI_CSF_GROUP_CONTROL_OFFSET);
            if (cctl->Features != c0->Features || cctl->SuspendSize != c0->SuspendSize
                || cctl->ProtmSuspendSize != c0->ProtmSuspendSize || cctl->StreamNum != c0->StreamNum
                || cctl->StreamStride != c0->StreamStride) {
                WINMALI_WARN("FW: CSG%u control differs from CSG0 (Panthor expects identical slots)", csg);
                return STATUS_INVALID_IMAGE_FORMAT;
            }
        }

        cin  = (volatile WINMALI_PANTHOR_FW_CSG_INPUT_IFACE*)WinMaliFwIfaceFwToCpu_(Fw, cctl->InputVa);
        cout = (volatile WINMALI_PANTHOR_FW_CSG_OUTPUT_IFACE*)WinMaliFwIfaceFwToCpu_(Fw, cctl->OutputVa);
        if (cin == NULL || cout == NULL) {
            WINMALI_WARN("FW: CSG%u iface VA invalid (in=%08x out=%08x)", csg, cctl->InputVa, cctl->OutputVa);
            return STATUS_INVALID_PARAMETER;
        }

        sn = cctl->StreamNum;
        st = cctl->StreamStride;
        for (cs = 0; cs < sn; ++cs) {
            ULONG                                           soff =
                WINMALI_CSF_GROUP_CONTROL_OFFSET + csg * gs + WINMALI_CSF_STREAM_CONTROL_OFFSET + cs * st;
            volatile WINMALI_PANTHOR_FW_CS_CONTROL_IFACE* sc;
            volatile WINMALI_PANTHOR_FW_CS_INPUT_IFACE*   si_;
            volatile WINMALI_PANTHOR_FW_CS_OUTPUT_IFACE* so;

            sc = (volatile WINMALI_PANTHOR_FW_CS_CONTROL_IFACE*)((PUCHAR)Fw->GlbControl + soff);
            si_ = (volatile WINMALI_PANTHOR_FW_CS_INPUT_IFACE*)WinMaliFwIfaceFwToCpu_(Fw, sc->InputVa);
            so  = (volatile WINMALI_PANTHOR_FW_CS_OUTPUT_IFACE*)WinMaliFwIfaceFwToCpu_(Fw, sc->OutputVa);
            if (si_ == NULL || so == NULL) {
                WINMALI_WARN("FW: CSG%u CS%u iface invalid", csg, cs);
                return STATUS_INVALID_PARAMETER;
            }
            if (csg == 0 && cs == 0) {
                ULONG feat = sc->Features;
                Fw->CsRegCount             = (feat & 0xffu) + 1u;
                Fw->CsUnpreservedRegCount  = WINMALI_CSF_UNPRESERVED_REG_COUNT;
                Fw->SbSlotCount            = (feat >> 8) & 0xffu;
                Fw->Csg0Control            = (volatile WINMALI_PANTHOR_FW_CSG_CONTROL_IFACE*)cctl;
                Fw->Csg0Input              = cin;
                Fw->Csg0Output             = cout;
                Fw->Cs00Control            = sc;
                Fw->Cs00Input              = si_;
                Fw->Cs00Output             = so;
            } else if (cs > 0 || csg > 0) {
                volatile WINMALI_PANTHOR_FW_CS_CONTROL_IFACE* s0 = Fw->Cs00Control;
                if (s0 != NULL && sc->Features != s0->Features) {
                    WINMALI_WARN("FW: CS features mismatch (CSG%u CS%u)", csg, cs);
                    return STATUS_INVALID_IMAGE_FORMAT;
                }
            }
        }
    }

    Fw->CsfIfaceValid = TRUE;
    return STATUS_SUCCESS;
}

static VOID
WinMaliFwGlbToggleDoorbellMask_(
    _Inout_ PWINMALI_ADAPTER Adapter,
    _Inout_ WINMALI_FWCTX*  Fw,
    _In_ ULONG              CsgMask)
{
    KIRQL irql;
    ULONG cur;
    ULONG o;
    ULONG n;
    UNREFERENCED_PARAMETER(Adapter);
    KeAcquireSpinLock(&Fw->GlbLock, &irql);
    cur = Fw->GlbInput->DoorbellReq;
    o   = Fw->GlbOutput->DoorbellAck;
    n   = ((o ^ CsgMask) & CsgMask) | (cur & ~CsgMask);
    Fw->GlbInput->DoorbellReq = n;
    KeReleaseSpinLock(&Fw->GlbLock, irql);
}

static VOID
WinMaliCsfCsgUpdateReqs_(
    _Inout_ WINMALI_FWCTX* Fw,
    _Inout_ volatile WINMALI_PANTHOR_FW_CSG_INPUT_IFACE* In,
    _In_ ULONG Val,
    _In_ ULONG Mask)
{
    KIRQL irql;
    ULONG cur;
    ULONG n;
    KeAcquireSpinLock(&Fw->CsfIfaceLock, &irql);
    cur = In->Req;
    n   = (cur & ~Mask) | (Val & Mask);
    In->Req = n;
    KeReleaseSpinLock(&Fw->CsfIfaceLock, irql);
}

static VOID
WinMaliCsfCsgToggleDoorbellReqs_(_Inout_ WINMALI_FWCTX* Fw, _In_ ULONG CsMask)
{
    KIRQL irql;
    ULONG cur;
    ULONG o;
    ULONG n;
    KeAcquireSpinLock(&Fw->CsfIfaceLock, &irql);
    cur = Fw->Csg0Input->DoorbellReq;
    o   = Fw->Csg0Output->DoorbellAck;
    n   = ((o ^ CsMask) & CsMask) | (cur & ~CsMask);
    Fw->Csg0Input->DoorbellReq = n;
    KeReleaseSpinLock(&Fw->CsfIfaceLock, irql);
}

static VOID
WinMaliCsfCsUpdateReqs_(
    _Inout_ WINMALI_FWCTX* Fw,
    _Inout_ volatile WINMALI_PANTHOR_FW_CS_INPUT_IFACE* In,
    _In_ ULONG Val,
    _In_ ULONG Mask)
{
    KIRQL irql;
    ULONG cur;
    ULONG n;
    KeAcquireSpinLock(&Fw->CsfIfaceLock, &irql);
    cur = In->Req;
    n   = (cur & ~Mask) | (Val & Mask);
    In->Req = n;
    KeReleaseSpinLock(&Fw->CsfIfaceLock, irql);
}

static NTSTATUS
WinMaliFwCsgWaitReqAck_(
    _In_ WINMALI_FWCTX* Fw,
    _In_ ULONG          ReqMask,
    _In_ ULONG          TimeoutMs,
    _Out_opt_ ULONG*    AckedBits)
{
    ULONG   i;
    ULONG   req0;
    LARGE_INTEGER t;

    if (Fw->Csg0Input == NULL || Fw->Csg0Output == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeMemoryBarrier();
    req0 = Fw->Csg0Input->Req & ReqMask;

    for (i = 0; i < TimeoutMs; ++i) {
        ULONG ack = Fw->Csg0Output->Ack & ReqMask;
        if (ack == (req0 & ReqMask)) {
            if (AckedBits != NULL) {
                *AckedBits = ack;
            }
            return STATUS_SUCCESS;
        }
        t.QuadPart = -10000LL;
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    if (AckedBits != NULL) {
        *AckedBits = Fw->Csg0Output->Ack & ReqMask;
    }
    return STATUS_IO_TIMEOUT;
}

static ULONG
WinMaliPopCount64_(_In_ UINT64 m)
{
    ULONG n = 0;
    while (m != 0ull) {
        n += (ULONG)(m & 1ull);
        m >>= 1;
    }
    return n;
}

/*
 * Program CSG slot 0 (and its single CS slot) for MMU address space `AsSlot`
 * and issue a clean STATE_START, waiting for the CSG req/ack. This is the
 * panthor "csg_slot_prog + STATE_START" sequence factored out so both the
 * bring-up self-test and per-group binds run the IDENTICAL programming - the
 * only thing that varies is the AS written into csgIn->Config.
 *
 * MUST be called only on a slot that is NOT currently STATE_START'd (i.e.
 * after bootstrap's first call, or after WinMaliCsfTerminateCsg_). panthor
 * never reprograms a running slot; a clean STOP->reprogram->START is what
 * makes the FW re-read config/endpoints/CS-ring for the new group. On
 * success the caller sets Fw->CurrentCsgAs = AsSlot and Fw->CsgStarted.
 */
static NTSTATUS
WinMaliCsfProgramAndStartCsg_(_Inout_ PWINMALI_ADAPTER Adapter,
                              _Inout_ WINMALI_FWCTX*   Fw,
                              _In_ ULONG               AsSlot)
{
    volatile WINMALI_PANTHOR_FW_CS_INPUT_IFACE*  csIn;
    volatile WINMALI_PANTHOR_FW_CSG_INPUT_IFACE* csgIn;
    ULONG    epVal;
    ULONG    csgAck;
    ULONG    acked = 0;
    NTSTATUS status;

    if (Fw->Cs00Input == NULL || Fw->Csg0Input == NULL || Fw->Csg0Output == NULL
        || Fw->QRingIn == NULL || Fw->QRingOut == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    csIn  = Fw->Cs00Input;
    csgIn = Fw->Csg0Input;

    /* Program the CS slot (queue ring). Present an EMPTY ring at the FW's
       current extract position - Insert == Extract == QRingOut->Extract.
       Do NOT force Insert to 0: the ring insert/extract are continuous
       counters shared across binds (the bring-up NOP self-test already
       advanced them). Zeroing Insert while the FW's Extract is non-zero
       leaves Insert < Extract, so the trampoline we append lands BEHIND the
       extract pointer and the FW never executes it (submit then times out
       with the ring looking empty and no fault). */
    Fw->QRingIn->Extract  = Fw->QRingOut->Extract;
    Fw->QRingIn->Insert   = Fw->QRingOut->Extract;
    csIn->RingbufBase     = WINMALI_CSF_RING_GPU_VA;
    csIn->RingbufSize     = WINMALI_CSF_RING_BYTES;
    csIn->RingbufInput    = Fw->McuQueueIfaceVa;
    csIn->RingbufOutput   = Fw->McuQueueIfaceVa + 4096ull;
    csIn->HeapStart       = 0;
    csIn->HeapEnd         = 0;
    csIn->InstrConfig     = 0;
    csIn->InstrbufSize    = 0;
    csIn->InstrbufBase    = 0;
    csIn->InstrbufOffsetPtr = 0;
    csIn->Config          = WINMALI_CS_CONFIG_PRIORITY(0u) | WINMALI_CS_CONFIG_DOORBELL(1u);
    csIn->AckIrqMask      = 0xffffffffu;

    WinMaliCsfCsUpdateReqs_(
        Fw,
        csIn,
        WINMALI_CS_IDLE_SYNC_WAIT | WINMALI_CS_IDLE_EMPTY | WINMALI_CS_STATE_START | WINMALI_CS_EXTRACT_EVENT,
        WINMALI_CS_REQ_MASK);

    /* Program the CSG slot: endpoints + the target AS + suspend buffers. */
    csgIn->AllowCompute   = Fw->ShaderCoreMask;
    csgIn->AllowFragment  = Fw->ShaderCoreMask;
    /* AllowOther = tiler mask = 1 on G610 (single tiler). Hardcoded 1 (the
       long-working value); the v52 TILER_PRESENT-read variant was reverted
       with the other bring-up-path changes while restoring the cold boot. */
    csgIn->AllowOther     = 1u;
    epVal                 = WINMALI_CSG_EP_REQ_COMPUTE(Fw->MaxCores)
                          | WINMALI_CSG_EP_REQ_FRAGMENT(Fw->MaxCores)
                          | WINMALI_CSG_EP_REQ_TILER(1u)
                          | WINMALI_CSG_EP_REQ_PRIORITY(0xFu);
    csgIn->EndpointReq    = epVal;
    csgIn->Config         = AsSlot;
    csgIn->SuspendBuf     = Fw->McuSuspendVa;
    csgIn->ProtmSuspendBuf = Fw->McuProtmVa;
    csgIn->AckIrqMask     = 0xffffffffu;

    /* Arm the CS doorbell (queue_mask = bit 0), then request STATE_START +
       ENDPOINT_CONFIG (toggled vs the current ack), ring the global doorbell
       and wait for both to ack. */
    WinMaliCsfCsgToggleDoorbellReqs_(Fw, 1u);

    csgAck = Fw->Csg0Output->Ack;
    WinMaliCsfCsgUpdateReqs_(
        Fw,
        csgIn,
        WINMALI_CSG_STATE_START | ((csgAck ^ WINMALI_CSG_ENDPOINT_CONFIG) & WINMALI_CSG_ENDPOINT_CONFIG),
        WINMALI_CSG_REQ_MASK);

    WinMaliFwGlbToggleDoorbellMask_(Adapter, Fw, 1u);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_CSF_DOORBELL(0), 1u);

    status = WinMaliFwCsgWaitReqAck_(Fw, WINMALI_CSG_REQ_MASK, 5000u, &acked);
    if (NT_SUCCESS(status)) {
        Fw->CurrentCsgAs = AsSlot;
        Fw->CsgStarted   = TRUE;
    } else {
        WINMALI_WARN("CSF: ProgramAndStartCsg AS=%u req/ack wait failed 0x%08x (acked=0x%08x)",
                     AsSlot, status, acked);
    }
    return status;
}

/*
 * TERMINATE CSG slot 0 (STATE_TERMINATE=0) and wait for the ack. Tears the
 * slot down so it can be cleanly reprogrammed for a different group's AS.
 * No-op if the slot isn't currently started.
 */
static NTSTATUS
WinMaliCsfTerminateCsg_(_Inout_ PWINMALI_ADAPTER Adapter, _Inout_ WINMALI_FWCTX* Fw)
{
    ULONG    acked = 0;
    NTSTATUS status;

    if (!Fw->CsgStarted) {
        return STATUS_SUCCESS;
    }
    if (Fw->Csg0Input == NULL || Fw->Csg0Output == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* State field -> TERMINATE (0). Leave ENDPOINT_CONFIG bit as-is (its ack
       parity must not be disturbed). Ring the global doorbell and wait for the
       state ack. */
    WinMaliCsfCsgUpdateReqs_(Fw, Fw->Csg0Input,
                             WINMALI_CSG_STATE_TERMINATE, WINMALI_CSG_STATE_MASK);
    WinMaliFwGlbToggleDoorbellMask_(Adapter, Fw, 1u);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_CSF_DOORBELL(0), 1u);

    status = WinMaliFwCsgWaitReqAck_(Fw, WINMALI_CSG_STATE_MASK, 1000u, &acked);
    WINMALI_TRACE("CSF: TerminateCsg (was AS=%u) ack=0x%08x st=0x%08x",
                  Fw->CurrentCsgAs, acked, status);
    /* Whatever the ack, treat the slot as no longer started - the caller
       reprograms it fresh next. */
    Fw->CsgStarted = FALSE;
    return status;
}

static NTSTATUS
WinMaliCsfBootstrap_(_Inout_ PWINMALI_ADAPTER Adapter, _Inout_ WINMALI_FWCTX* Fw)
{
    NTSTATUS           status;
    PHYSICAL_ADDRESS   low;
    PHYSICAL_ADDRESS   high;
    UINT64             shaderMask;
    ULONG              maxCores;

    if (!Fw->CsfIfaceValid || Fw->Cs00Input == NULL || Fw->Csg0Input == NULL || Fw->Csg0Control == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (!Adapter->GpuMmuAsBound) {
        WINMALI_WARN("CSF bootstrap: GPU bring-up AS not bound");
        return STATUS_DEVICE_NOT_READY;
    }

    low.QuadPart  = 0;
    high.QuadPart = (ULONG64)-1LL;

    Fw->HostRingVa = MmAllocateContiguousMemorySpecifyCache(
        WINMALI_CSF_RING_BYTES,
        low,
        high,
        low,
        MmNonCached);
    if (Fw->HostRingVa == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Fw->RingPa = MmGetPhysicalAddress(Fw->HostRingVa);
    RtlZeroMemory(Fw->HostRingVa, WINMALI_CSF_RING_BYTES);

    // Ring buffer holds CSF stream bytes that the MCU will execute via the
    // top-level ringbuf doorbell — they are *not* called via a CALL into
    // an EX-tagged page, so RW + NX is the correct permission. The MCU
    // walks the ring through a separate fetch path, not the shader-EX
    // mapping; we tag the *callee* stream below as RW + EX instead.
    status = WinMaliMmuMapGpuRange(
        Adapter,
        WINMALI_CSF_RING_GPU_VA,
        Fw->RingPa,
        WINMALI_CSF_RING_BYTES / 4096u,
        WINMALI_LPAE_L3_PAGE_ATTR_RW_NX);
    if (!NT_SUCCESS(status)) {
        goto err_ring;
    }

    Fw->HostQueueIfaceVa = MmAllocateContiguousMemorySpecifyCache(
        WINMALI_CSF_QUEUE_IFACE_BYTES,
        low,
        high,
        low,
        MmNonCached);
    if (Fw->HostQueueIfaceVa == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_ring_unmap;
    }
    RtlZeroMemory(Fw->HostQueueIfaceVa, WINMALI_CSF_QUEUE_IFACE_BYTES);
    Fw->QueueIfacePa         = MmGetPhysicalAddress(Fw->HostQueueIfaceVa);
    Fw->QRingIn              = (volatile WINMALI_FW_RINGBUF_INPUT_IFACE*)Fw->HostQueueIfaceVa;
    Fw->QRingOut             = (volatile WINMALI_FW_RINGBUF_OUTPUT_IFACE*)((PUCHAR)Fw->HostQueueIfaceVa + 4096u);
    // Queue iface is two 4 KiB shared pages (insert/extract producer/consumer
    // doorbell + size words) — pure CPU↔MCU control data, never executed.
    status                   = WinMaliFwMcuMapContiguous_(
        Fw,
        Fw->HostQueueIfaceVa,
        Fw->QueueIfacePa,
        WINMALI_CSF_QUEUE_IFACE_BYTES,
        4096ull,
        WINMALI_LPAE_L3_PAGE_ATTR_RW_NX,
        &Fw->McuQueueIfaceVa);
    if (!NT_SUCCESS(status)) {
        goto err_qif_unmap;
    }

    Fw->HostSyncVa = MmAllocateContiguousMemorySpecifyCache(4096u, low, high, low, MmNonCached);
    if (Fw->HostSyncVa == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_qif_unmap;
    }
    RtlZeroMemory(Fw->HostSyncVa, 4096u);
    Fw->SyncPa = MmGetPhysicalAddress(Fw->HostSyncVa);
    // Syncobj page: 64-bit counter + status; the trampoline does
    // SYNC_ADD64 here. Memory access only, never instruction fetch.
    status     = WinMaliMmuMapGpuRange(
        Adapter,
        WINMALI_CSF_SYNC_GPU_VA,
        Fw->SyncPa,
        1u,
        WINMALI_LPAE_L3_PAGE_ATTR_RW_NX);
    if (!NT_SUCCESS(status)) {
        goto err_sync;
    }

    Fw->SuspendBytes = (SIZE_T)Fw->Csg0Control->SuspendSize;
    if (Fw->SuspendBytes == 0) {
        Fw->SuspendBytes = 4096u;
    }
    Fw->SuspendBytes = (Fw->SuspendBytes + 4095u) & ~(SIZE_T)4095u;
    Fw->HostSuspendVa = MmAllocateContiguousMemorySpecifyCache(Fw->SuspendBytes, low, high, low, MmNonCached);
    if (Fw->HostSuspendVa == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_sync_unmap;
    }
    RtlZeroMemory(Fw->HostSuspendVa, Fw->SuspendBytes);
    Fw->SuspendPa = MmGetPhysicalAddress(Fw->HostSuspendVa);
    // Suspend buffer holds CSG context state saved by the MCU on
    // suspend/resume — opaque scratch, NX is correct.
    status        = WinMaliFwMcuMapContiguous_(
        Fw,
        Fw->HostSuspendVa,
        Fw->SuspendPa,
        Fw->SuspendBytes,
        4096ull,
        WINMALI_LPAE_L3_PAGE_ATTR_RW_NX,
        &Fw->McuSuspendVa);
    if (!NT_SUCCESS(status)) {
        goto err_free_susp;
    }

    Fw->ProtmBytes = (SIZE_T)Fw->Csg0Control->ProtmSuspendSize;
    if (Fw->ProtmBytes == 0) {
        Fw->ProtmBytes = 4096u;
    }
    Fw->ProtmBytes = (Fw->ProtmBytes + 4095u) & ~(SIZE_T)4095u;
    Fw->HostProtmVa = MmAllocateContiguousMemorySpecifyCache(Fw->ProtmBytes, low, high, low, MmNonCached);
    if (Fw->HostProtmVa == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_free_susp;
    }
    RtlZeroMemory(Fw->HostProtmVa, Fw->ProtmBytes);
    Fw->ProtmPa = MmGetPhysicalAddress(Fw->HostProtmVa);
    // Protected-mode suspend buffer — same role as Suspend but for
    // protected-mode contexts. NX is correct.
    status      = WinMaliFwMcuMapContiguous_(
        Fw,
        Fw->HostProtmVa,
        Fw->ProtmPa,
        Fw->ProtmBytes,
        4096ull,
        WINMALI_LPAE_L3_PAGE_ATTR_RW_NX,
        &Fw->McuProtmVa);
    if (!NT_SUCCESS(status)) {
        goto err_free_protm;
    }

    Fw->HostShaderVa = MmAllocateContiguousMemorySpecifyCache(4096u, low, high, low, MmNonCached);
    if (Fw->HostShaderVa == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_free_protm;
    }
    RtlCopyMemory(Fw->HostShaderVa, WinMaliShaderNop, sizeof(WinMaliShaderNop));
    Fw->ShaderPa = MmGetPhysicalAddress(Fw->HostShaderVa);
    // Escape NOP path: callee CSF stream the trampoline CALLs; must be EX.
    status       = WinMaliMmuMapGpuRange(
        Adapter,
        WINMALI_CSF_SHADER_GPU_VA,
        Fw->ShaderPa,
        1u,
        WINMALI_LPAE_L3_PAGE_ATTR_RW_EX);
    if (!NT_SUCCESS(status)) {
        goto err_shader;
    }


    {
        ULONG lo = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_SHADER_PRESENT_LO);
        ULONG hi = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_SHADER_PRESENT_HI);
        shaderMask = ((UINT64)hi << 32) | (UINT64)lo;
    }
    maxCores = WinMaliPopCount64_(shaderMask);
    if (maxCores < 1u) {
        maxCores = 1u;
    }

    /* Endpoint params are constant for the device; cache them so per-group
       CSG binds reprogram the slot without re-reading MMIO. */
    Fw->ShaderCoreMask = shaderMask;
    Fw->MaxCores       = maxCores;

    /* Bind + start CSG slot 0 in the bring-up AS via the SAME programming path
       every UMD group bind uses (WinMaliCsfProgramAndStartCsg_). */
    Fw->CsgStarted = FALSE;
    status = WinMaliCsfProgramAndStartCsg_(Adapter, Fw, Adapter->GpuMmuBringupAs);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("CSF bootstrap: CSG0 program+start failed 0x%08x", status);
        goto err_shader_unmap;
    }

    Fw->KqJobSeq     = 0ull;
    Fw->CsfBootValid = TRUE;

    WINMALI_TRACE(
        "CSF bootstrap OK: ring gpu_va=0x%llx (%lu KiB) qiface mcu_va=0x%llx "
        "sync gpu_va=0x%llx shader gpu_va=0x%llx susp=%lu B protm=%lu B "
        "shader_mask=0x%016llx max_cores=%lu csg_as=%u csg_ack=0x%08x",
        (ULONGLONG)WINMALI_CSF_RING_GPU_VA,
        (ULONG)(WINMALI_CSF_RING_BYTES / 1024u),
        (ULONGLONG)Fw->McuQueueIfaceVa,
        (ULONGLONG)WINMALI_CSF_SYNC_GPU_VA,
        (ULONGLONG)WINMALI_CSF_SHADER_GPU_VA,
        (ULONG)Fw->SuspendBytes,
        (ULONG)Fw->ProtmBytes,
        (ULONGLONG)shaderMask,
        maxCores,
        Fw->CurrentCsgAs,
        (ULONG)(Fw->Csg0Output ? Fw->Csg0Output->Ack : 0u));

    return STATUS_SUCCESS;

err_shader_unmap:
    (VOID)WinMaliMmuUnmapGpuRange(Adapter, WINMALI_CSF_SHADER_GPU_VA, 1u);
err_shader:
    if (Fw->HostShaderVa != NULL) {
        MmFreeContiguousMemory(Fw->HostShaderVa);
        Fw->HostShaderVa = NULL;
    }
err_free_protm:
    if (Fw->HostProtmVa != NULL) {
        MmFreeContiguousMemory(Fw->HostProtmVa);
        Fw->HostProtmVa = NULL;
    }
err_free_susp:
    if (Fw->HostSuspendVa != NULL) {
        MmFreeContiguousMemory(Fw->HostSuspendVa);
        Fw->HostSuspendVa = NULL;
    }
err_sync_unmap:
    (VOID)WinMaliMmuUnmapGpuRange(Adapter, WINMALI_CSF_SYNC_GPU_VA, 1u);
err_sync:
    if (Fw->HostSyncVa != NULL) {
        MmFreeContiguousMemory(Fw->HostSyncVa);
        Fw->HostSyncVa = NULL;
    }
err_qif_unmap:
    if (Fw->HostQueueIfaceVa != NULL) {
        MmFreeContiguousMemory(Fw->HostQueueIfaceVa);
        Fw->HostQueueIfaceVa = NULL;
        Fw->QRingIn          = NULL;
        Fw->QRingOut         = NULL;
    }
err_ring_unmap:
    (VOID)WinMaliMmuUnmapGpuRange(Adapter, WINMALI_CSF_RING_GPU_VA, WINMALI_CSF_RING_BYTES / 4096u);
err_ring:
    if (Fw->HostRingVa != NULL) {
        MmFreeContiguousMemory(Fw->HostRingVa);
        Fw->HostRingVa = NULL;
    }
    return status;
}

static VOID
WinMaliCsfTeardown_(_Inout_ PWINMALI_ADAPTER Adapter, _Inout_ WINMALI_FWCTX* Fw)
{
    if (Fw == NULL || Adapter == NULL) {
        return;
    }
    (VOID)WinMaliMmuUnmapGpuRange(Adapter, WINMALI_CSF_RING_GPU_VA, WINMALI_CSF_RING_BYTES / 4096u);
    (VOID)WinMaliMmuUnmapGpuRange(Adapter, WINMALI_CSF_SYNC_GPU_VA, 1u);
    (VOID)WinMaliMmuUnmapGpuRange(Adapter, WINMALI_CSF_SHADER_GPU_VA, 1u);
    Fw->CsfBootValid = FALSE;
    if (Fw->HostRingVa != NULL) {
        MmFreeContiguousMemory(Fw->HostRingVa);
        Fw->HostRingVa = NULL;
    }
    if (Fw->HostQueueIfaceVa != NULL) {
        MmFreeContiguousMemory(Fw->HostQueueIfaceVa);
        Fw->HostQueueIfaceVa = NULL;
        Fw->QRingIn          = NULL;
        Fw->QRingOut         = NULL;
    }
    if (Fw->HostSyncVa != NULL) {
        MmFreeContiguousMemory(Fw->HostSyncVa);
        Fw->HostSyncVa = NULL;
    }
    if (Fw->HostSuspendVa != NULL) {
        MmFreeContiguousMemory(Fw->HostSuspendVa);
        Fw->HostSuspendVa = NULL;
    }
    if (Fw->HostProtmVa != NULL) {
        MmFreeContiguousMemory(Fw->HostProtmVa);
        Fw->HostProtmVa = NULL;
    }
    if (Fw->HostShaderVa != NULL) {
        MmFreeContiguousMemory(Fw->HostShaderVa);
        Fw->HostShaderVa = NULL;
    }
    Adapter->AdapterFlags &= (ULONG)~WINMALI_ADAPTER_FLAG_CSF_JOBS;
}

//
// Re-arm the CSG slot if the firmware has put it in idle/eviction limbo.
//
// What the firmware does after a job completes (observed empirically and
// confirmed against panthor's panthor_sched.c::process_csg_irq_locked +
// queue_run_job paths):
//
//   1. Queue extract catches up to insert => CS becomes idle.
//   2. FW signals CSG_IDLE (BIT 29) and CSG_SYNC_UPDATE (BIT 28) in CSG.Ack.
//   3. FW deallocates the endpoints (compute/fragment/tiler) and sets
//      Csg0Output->StatusEndpointCurrent = 0. StatusState reads as 0.
//   4. The kernel is expected to ACK CSG_EVT_MASK and, if it has new work
//      to schedule, re-toggle CSG_STATE_START + CSG_ENDPOINT_CONFIG and
//      ring the *global* doorbell so the FW re-binds endpoints.
//
// If we skip step 4, every submission after the first lands in a queue
// the FW won't service: the per-CS doorbell is ignored because no slots
// are bound, the new work sits in the ring, and the kernel times out
// waiting for the syncobj to advance — which is exactly what the previous
// run showed (ring ins=256 ext=128, ack=0x30000011, ep_cur=0, no fault).
//
// We unconditionally run the rearm here on every submission. It's cheap
// (a handful of MMIOs) and harmless if the slot is already running: the
// req=ack-toggle dance is idempotent and only causes an actual transition
// when the FW has signalled idle.
//
static NTSTATUS
WinMaliCsfWakeCsgIfIdle_(_Inout_ PWINMALI_ADAPTER Adapter, _Inout_ WINMALI_FWCTX* Fw)
{
    ULONG     ack;
    ULONG     state;
    ULONG     epCur;
    ULONG     evtPending;
    ULONG     reqOld;
    ULONG     reqNew;
    KIRQL     irql;
    NTSTATUS  status;
    ULONG     acked = 0;

    if (Fw == NULL || Fw->Csg0Input == NULL || Fw->Csg0Output == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    KeMemoryBarrier();
    ack        = Fw->Csg0Output->Ack;
    state      = Fw->Csg0Output->StatusState;
    epCur      = Fw->Csg0Output->StatusEndpointCurrent;
    evtPending = ack & WINMALI_CSG_EVT_MASK;

    //
    // No idle bits, endpoints still allocated => slot is hot. Common path
    // on the very first submit and on back-to-back submits faster than the
    // FW's idle hysteresis. Nothing to do.
    //
    if (evtPending == 0u && epCur != 0u) {
        return STATUS_SUCCESS;
    }

    //
    // ACK every event bit the FW has flagged, re-toggle ENDPOINT_CONFIG
    // (forces re-evaluation of compute/fragment/tiler resources from the
    // CsgInput->EndpointReq values we set at bootstrap), and re-issue
    // STATE_START. All in a single Req write so the FW sees a consistent
    // view.
    //
    KeAcquireSpinLock(&Fw->CsfIfaceLock, &irql);
    reqOld = Fw->Csg0Input->Req;
    reqNew = reqOld;
    reqNew = (reqNew & ~WINMALI_CSG_EVT_MASK) | (ack & WINMALI_CSG_EVT_MASK);
    reqNew = (reqNew & ~WINMALI_CSG_ENDPOINT_CONFIG)
             | ((ack ^ WINMALI_CSG_ENDPOINT_CONFIG) & WINMALI_CSG_ENDPOINT_CONFIG);
    reqNew = (reqNew & ~WINMALI_CSG_STATE_MASK) | WINMALI_CSG_STATE_START;
    Fw->Csg0Input->Req = reqNew;
    KeReleaseSpinLock(&Fw->CsfIfaceLock, irql);

    //
    // The CSG global doorbell mask must also be toggled so the FW notices
    // the request, and the global doorbell at index 0 must be rung.
    //
    WinMaliFwGlbToggleDoorbellMask_(Adapter, Fw, 1u);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_CSF_DOORBELL(0), 1u);

    status = WinMaliFwCsgWaitReqAck_(Fw, WINMALI_CSG_REQ_MASK, 1000u, &acked);
    WINMALI_TRACE(
        "CSF wake: state=0x%08x ep_cur=0x%08x evt=0x%08x req 0x%08x->0x%08x ack=0x%08x acked=0x%08x st=0x%08x",
        state, epCur, evtPending, reqOld, reqNew, ack, acked, status);
    return status;
}

/* Bind the single CSG slot to MMU address space `AsSlot` so the MCU executes
 * a UMD group's stream in the group's own VM. panthor never swaps a running
 * CSG's AS in place (csg_slot_prog_locked bails if the group is ACTIVE); the
 * FW only re-reads config/endpoints/CS-ring on a fresh STATE_START out of a
 * stopped slot. So when the target AS differs from the currently-bound one we
 * do a clean TERMINATE -> reprogram -> START cycle rather than rewriting
 * Config on a live slot (the live-swap left the CSG in a state where the
 * group's HEAP_SET init CS hung with no fault - see UMD bring-up findings).
 * Serialized by the escape dispatch. WINMALI_AS_SLOT_MAX == "no rebind". */
static NTSTATUS
WinMaliCsfRebindCsgAs_(_Inout_ PWINMALI_ADAPTER Adapter,
                       _Inout_ WINMALI_FWCTX*   Fw,
                       _In_ ULONG               AsSlot)
{
    NTSTATUS status;

    if (AsSlot >= WINMALI_AS_SLOT_MAX) {
        return STATUS_SUCCESS;   /* sentinel: caller wants no rebind */
    }
    if (Fw->Csg0Input == NULL || Fw->Csg0Output == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Deliberately DO NOT skip when AsSlot == CurrentCsgAs && CsgStarted.
     * The tracked (CurrentCsgAs, CsgStarted) can diverge from the actual
     * hardware CSG binding: the MCU deallocates the slot's endpoints when the
     * CSG goes idle between submits (and a fault/recovery leaves the slot
     * stopped), yet CsgStarted stays TRUE. A stale skip then ran the next
     * group's stream against the WRONG address space -> MMU TRANSLATION_FAULT
     * reading the group's own descriptors (e.g. the tiler context), which
     * stalled the AS and wedged the whole CSG/tiler for every later context
     * (single bad draw took down the GPU until reboot). Submits are serialized
     * by SubmitLock and the previous one has completed (seqno signalled) before
     * we get here, so the slot is idle: a clean TERMINATE->reprogram->START per
     * submit is cheap insurance that the slot is provably bound to THIS group's
     * VM before we run its stream. Correctness over the skip's micro-opt. */

    /* ⛔★★★★★ NEVER TEAR DOWN A SLOT THAT STILL HAS WORK QUEUED.
     *
     * The comment above states this function's precondition: "the previous one
     * has completed (seqno signalled) before we get here, so the slot is idle".
     * ⛔ THAT IS FALSE THE MOMENT A SUBMIT TIMES OUT. A timed-out submit has NOT
     * completed and its job is STILL IN THE RING, but we terminate anyway - so
     * the first timeout poisons every submit after it and the GPU never recovers.
     *
     * MEASURED LIVE 2026-08-18 at the stall, with a frozen desktop on screen:
     *     CsOk FROZEN at 942, CsTimeouts climbing, CsfLastStatus=0xC00000B5
     *     RingInsert-RingExtract = 128   (one job queued, never consumed)
     *     CsFatal=0 CsFault=0 GpuFaultSt=0 AsFaultSt=0 McuFaultSt=0  (no fault)
     *     one CSG slot rebound across THREE address spaces as apps started:
     *     AS=5 -> AS=1 -> AS=5 -> AS=1 -> AS=6, and the timeout was on AS 6.
     *
     * Give the FW a bounded chance to drain first. If it still has not consumed
     * the bytes, say so loudly rather than silently destroying the context that
     * job was going to run in - "the ring looking empty and no fault" is the
     * exact symptom WinMaliCsfProgramAndStartCsg_ already warns about.
     *
     * ⚠ Diagnostic + mitigation only; it does NOT fix the underlying handshake.
     * The WMTERMBUSY1 marker is also the deploy proof for this build.
     */
    if (Fw->QRingIn != NULL && Fw->QRingOut != NULL) {
        UINT64 ins = Fw->QRingIn->Insert;
        UINT64 ext = Fw->QRingOut->Extract;
        if (ins != ext) {
            ULONG spins;
            for (spins = 0; spins < 100u && ins != ext; ++spins) {
                LARGE_INTEGER d;
                d.QuadPart = -10000;   /* 1 ms, relative */
                (VOID)KeDelayExecutionThread(KernelMode, FALSE, &d);
                ext = Fw->QRingOut->Extract;
            }
            if (ins != ext) {
                WINMALI_WARN("CSF: WMTERMBUSY1 terminating AS %u->%u with %I64u B "
                             "STILL QUEUED (insert=%I64u extract=%I64u) - the FW "
                             "never consumed it; this poisons the next submit",
                             Fw->CurrentCsgAs, AsSlot, ins - ext, ins, ext);
            } else {
                WINMALI_TRACE("CSF: WMTERMBUSY1 ring drained after wait "
                              "(AS %u->%u)", Fw->CurrentCsgAs, AsSlot);
            }
        }
    }

    /* Tear the current group off the slot (if any), then program + start the
       slot fresh for the target AS. */
    status = WinMaliCsfTerminateCsg_(Adapter, Fw);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("CSF: bind AS %u->%u: terminate failed 0x%08x",
                     Fw->CurrentCsgAs, AsSlot, status);
        /* Fall through and try to start anyway - the slot is marked stopped. */
    }

    status = WinMaliCsfProgramAndStartCsg_(Adapter, Fw, AsSlot);
    WINMALI_TRACE("CSF: bind CSG -> AS %u (start st=0x%08x)", AsSlot, status);
    return status;
}

/* Recovery for an unhandled page fault on a user AS. The faulting
 * transaction stays stalled at the MMU until the AS is torn down, which is
 * what wedged the CSG after v25's ATOMIC@0 draw fault (the next TERMINATE
 * timed out, and every later submit on any group failed). Mirror panthor's
 * irq-handler recovery (panthor_mmu.c): clear the AS's MMU irq, then
 * disable the AS - the disable aborts the stalled transactions and kills
 * the faulted job. Because WinMali binds an AS to its VM eagerly for the
 * VM's whole lifetime, re-enable translation from the recorded root PT so
 * the next submit on this VM runs normally instead of hitting an unmapped
 * AS (panthor gets the same effect lazily on the next vm_active). */
static VOID
WinMaliCsfRecoverFaultedAs_(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG AsSlot)
{
    UINT64 rootPa = 0;
    UINT64 transcfg = 0, memattr = 0;
    KIRQL  irql;

    if (AsSlot >= WINMALI_AS_SLOT_MAX) {
        return;
    }

    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_MMU_INT_CLEAR, 1u << AsSlot);

    if (AsSlot == Adapter->GpuMmuBringupAs) {
        rootPa = (UINT64)Adapter->MmuScratchHeapPhys.QuadPart;
    } else {
        KeAcquireSpinLock(&Adapter->AsSlotLock, &irql);
        if (Adapter->AsBindings[AsSlot].Bound) {
            rootPa = Adapter->AsBindings[AsSlot].RootPtPa;
        }
        KeReleaseSpinLock(&Adapter->AsSlotLock, irql);
    }

    (VOID)WinMaliMmuAsDisable(Adapter, AsSlot);
    if (rootPa != 0) {
        WinMaliMmuGetDefaultAsParams(Adapter, &transcfg, &memattr);
        (VOID)WinMaliMmuAsEnable(Adapter, AsSlot, rootPa, transcfg, memattr);
        WINMALI_WARN("CSF fault recovery: AS%u disabled + re-enabled (root=0x%llx)",
                     AsSlot, rootPa);
    } else {
        WINMALI_WARN("CSF fault recovery: AS%u disabled (no bound VM)", AsSlot);
    }
}

/* Forward decl: the post-service check inside the OOM handler needs a real
   status refresh, and that helper is defined below it. */
static VOID WinMaliCsfRequestStatusUpdate_(_Inout_ PWINMALI_ADAPTER Adapter,
                                           _Inout_ WINMALI_FWCTX*   Fw);

/* Service a firmware TILER_OOM request on CS slot 0.
 *
 * THE BUG THIS FIXES. When the tiler heap runs out of chunks the FW sets
 * CS_ACK bit 26 and PARKS the command stream, waiting for the host to link in a
 * fresh chunk and toggle CS_REQ bit 26 to match. This driver never read the bit,
 * had no way to write it (WINMALI_CS_REQ_MASK strips it), and had no heap grow
 * path to satisfy it - so the first heap exhaustion of a boot wedged the GPU for
 * the rest of that boot. Every diagnostic pointed at "nothing is wrong" because
 * TILER_OOM is a REQUEST, not a fault: all MMU/GPU/CS fault banks read zero.
 *
 * Called from the submit wait loop, so a stream that parks mid-wait is unblocked
 * and the SAME submit goes on to complete - no retry, no terminate, no rebind.
 *
 * ⚠ We acknowledge even when we cannot supply a chunk (cap hit, allocation
 * failed, no heap on this VM). An unacknowledged request parks the stream
 * forever; acking with a null chunk makes the tiler op fail loudly instead,
 * which is recoverable. Never leave the request pending.
 *
 * Returns TRUE if a request was pending and has now been acknowledged. */
static BOOLEAN
WinMaliCsfServiceTilerOom_(_Inout_ PWINMALI_ADAPTER Adapter,
                           _Inout_ WINMALI_FWCTX*   Fw,
                           _In_    ULONG            CsgAsSlot)
{
    volatile WINMALI_PANTHOR_FW_CS_INPUT_IFACE*  csIn;
    volatile WINMALI_PANTHOR_FW_CS_OUTPUT_IFACE* csOut;
    ULONG    ack, req, irqReq = 0;
    UINT64   chunkVa   = 0;
    ULONG    chunkSize = 0;
    NTSTATUS st;

    csIn  = Fw->Cs00Input;
    csOut = Fw->Cs00Output;
    if (csIn == NULL || csOut == NULL) {
        return FALSE;
    }

    ack = csOut->Ack;
    req = csIn->Req;
    if (((ack ^ req) & WINMALI_CS_TILER_OOM) == 0u) {
        return FALSE;                        /* no request pending */
    }

    (VOID)InterlockedIncrement(&g_WinMaliStatTilerOomCount);

    st = WinMaliTilerHeapGrow(Adapter, CsgAsSlot, &chunkVa, &chunkSize);
    if (NT_SUCCESS(st) && chunkVa != 0) {
        /* ⛔★★★★★ THE FW WANTS THE **PACKED** CHUNK POINTER IN **BOTH** FIELDS.
         * panthor (cs_slot_process_tiler_oom) and kbase both write the SAME encoded
         * value - (va & ~0xFFF) | (chunk_size >> 12) - into CS_TILER_HEAP_START and
         * CS_TILER_HEAP_END. 1.1.9.15-21 wrote the RAW va into HeapStart and
         * va+size into HeapEnd, so the low 12 bits encoded a chunk size of ZERO.
         * That matches the symptom exactly: the ack is honoured (the FW never
         * re-asks, StatTilerOom stays 1) but the chunk is unusable, so the tiler
         * stays parked and WAIT(scoreboard 2) never signals.
         * ★ The driver already knew this encoding - WinMaliTilerHeapGrow computes it
         * for the ctx head - it just did not use it here. */
        UINT64 packed = (chunkVa & ~0xFFFull) | ((UINT64)chunkSize >> 12);
        csIn->HeapStart = packed;
        csIn->HeapEnd   = packed;
    } else {
        csIn->HeapStart = 0;
        csIn->HeapEnd   = 0;
        (VOID)InterlockedIncrement(&g_WinMaliStatTilerOomFail);
        WINMALI_WARN("CSF: WMTILEROOM2 TILER_OOM on AS %u could not be satisfied "
                     "(0x%08x) - acking with a NULL chunk so the CS makes progress "
                     "instead of parking forever", CsgAsSlot, st);
    }
    KeMemoryBarrier();

    /* Toggle only bit 26, under CsfIfaceLock. The per-call mask is what makes
       this safe: the global WINMALI_CS_REQ_MASK omits TILER_OOM on purpose, so
       the CSG-start path can never clear a pending acknowledgement. */
    WinMaliCsfCsUpdateReqs_(Fw, csIn, ack, WINMALI_CS_TILER_OOM);

    /* ⛔★★★★★ THE MISSING STEP: TELL THE FW **WHICH CS** NEEDS ATTENTION.
     *
     * kbase's tiler-OOM resume is:
     *     heap_start/end -> CS_REQ(masked toggle) -> **CSG_DB_REQ ^= BIT(cs_id)**
     *     -> GLB_DB_REQ -> CSF_DOORBELL(0)
     * We performed every step EXCEPT the per-CS CSG_DB_REQ toggle. That bit is the
     * only channel that says *which CS in the group* has a host response pending;
     * the user doorbell (DOORBELL(1)) means only "INSERT advanced".
     *
     * ⚠ AND IT INVALIDATES THE READING THAT RETIRED THE HANDOFF WORK. I concluded
     * "the chunk is accepted at the protocol level" because csReq came to equal
     * csAck - but WE wrote csReq. The FW never confirmed anything, and a parked CS
     * cannot re-ask (re-asking means toggling CS_ACK). So the four chunk-delivery
     * configurations that "all wedged identically" were CONFOUNDED: they varied the
     * payload while the wake-up sequence stayed constant and incomplete.
     *
     * This helper already existed with exactly kbase's semantics
     * (n = ((ack ^ mask) & mask) | (cur & ~mask)) and had ONE caller: CSG start. */
    WinMaliCsfCsgToggleDoorbellReqs_(Fw, 1u);
    KeMemoryBarrier();

    /* ⛔★★★★★ ALSO ACK THE CSG-LEVEL PER-CS INTERRUPT, OR THE FW NEVER
     * RE-DISPATCHES THE STREAM.
     *
     * Toggling CS_REQ bit 26 above tells the FW WHAT WE DID about the request.
     * This tells it we have HANDLED THE INTERRUPT for this CS slot. panthor's
     * sched_process_csg_irq_locked mirrors csg_iface->output->cs_irq_req into
     * ->input->cs_irq_ack for exactly this reason, and until 2026-08-20 nothing
     * in this driver read or wrote either field - grep found the two struct
     * members and ZERO uses.
     *
     * MEASURED with the CS-level ack alone (1.1.9.15): StatTilerOom tracked
     * CsTimeouts exactly (1/2, 4/5, 7/8, 10/11, 17/18, 20/21) - one OOM serviced
     * per timeout, chunks allocated successfully every time (OOMfail=0), and CsOk
     * still frozen at 1173. So the CS-level ack DOES clear the request (it never
     * spins), but the stream is never resumed. An unacked CSG interrupt is the
     * remaining half of the handshake. */
    irqReq = Fw->Csg0Output->CsIrqReq;
    Fw->Csg0Input->CsIrqAck = irqReq;
    KeMemoryBarrier();
    KeMemoryBarrier();
    /* Ring BOTH doorbells: 1 = the CS whose request we answered, 0 = the CSG
       whose per-CS interrupt we just acked. panthor rings the CSG doorbell after
       mirroring cs_irq_req; ringing only the CS doorbell leaves the group-level
       notification undelivered. */
    /* ⛔ SAME DELIVERY BUG AS THE STATUS-UPDATE PATH: DOORBELL(0) is not seen by
       the FW unless the global doorbell mask is toggled first. Measured there as
       req=0x31 ack=0x11 acked=0x0 - "never delivered", not "refused". */
    WinMaliFwGlbToggleDoorbellMask_(Adapter, Fw, 1u);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_CSF_DOORBELL(1), 1u);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_CSF_DOORBELL(0), 1u);

    /* kbase reads these as INPUTS to the OOM decision: it rejects the request if
       (frag_end > vt_end) || (vt_end >= vt_start), and computes
       pending_frag_count = vt_end - frag_end. We had ZERO readers for any of them.
       pending_frag_count is the number that says whether this is the
       tiler-waits-for-a-fragment-pass deadlock that incremental rendering exists
       to break. */
    WINMALI_WARN("CSF: WMTILEROOM1 serviced TILER_OOM as=%u chunk=0x%llx size=%u "
                 "(csAck=0x%08x csReq=0x%08x csgIrqReq=0x%08x dbReq=0x%08x dbAck=0x%08x) "
                 "heap: addr=0x%llx vtStart=%u vtEnd=%u fragEnd=%u pendingFrag=%d inFlight=%d",
                 CsgAsSlot, (ULONGLONG)chunkVa, chunkSize, ack, req, irqReq,
                 Fw->Csg0Input->DoorbellReq, Fw->Csg0Output->DoorbellAck,
                 (ULONGLONG)csOut->HeapAddress, csOut->HeapVtStart, csOut->HeapVtEnd,
                 csOut->HeapFragEnd,
                 (int)(csOut->HeapVtEnd - csOut->HeapFragEnd),
                 (int)(csOut->HeapVtStart - csOut->HeapFragEnd));

    /* ★ DID THE TILER ACTUALLY RESUME? This is the question four builds of chunk
       plumbing could not answer. Give the FW a moment, refresh the status page for
       real, and report where the CS is now. If CMD_PTR is UNCHANGED and still
       BLOCKED on scoreboard 2, then the chunk was accepted at the protocol level
       (the FW stops re-asking) but the tiler did NOT restart - which separates
       "our handoff is malformed" from "the tiler needs something other than memory".
       Runs at most once per boot (one OOM), and we are at PASSIVE here: the submit
       wait loop calls KeDelayExecutionThread. */
    {
        LARGE_INTEGER d;
        d.QuadPart = -500000;          /* 50 ms, relative */
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &d);
        WinMaliCsfRequestStatusUpdate_(Adapter, Fw);
        WINMALI_WARN("CSF: WMOOMPOST1 after service: CMD_PTR=0x%llx blocked=%u "
                     "wait=0x%08x sb=0x%08x csAck=0x%08x csReq=0x%08x seqno=%llu",
                     (ULONGLONG)Fw->Cs00Output->StatusCmdPtr,
                     Fw->Cs00Output->StatusBlockedReason & 0xFu,
                     Fw->Cs00Output->StatusWait,
                     Fw->Cs00Output->StatusScoreboards,
                     Fw->Cs00Output->Ack, Fw->Cs00Input->Req,
                     (ULONGLONG)0);
    }
    return TRUE;
}

/* Ask the firmware to REFRESH the CS/CSG status page, then wait briefly for the ack.
 *
 * ⛔★★★★★ WITHOUT THIS, EVERY Status* FIELD IS AN UNINITIALISED STRUCT.
 *
 * `WINMALI_CSG_STATUS_UPDATE` (bit 5) has existed since the CSF bring-up and had
 * exactly ONE occurrence in the whole driver - its own #define. Nothing ever
 * requested a status update, so `Cs00Output->StatusCmdPtr`, `StatusBlockedReason`,
 * `StatusWait`, `StatusScoreboards` and `HeapAddress` are whatever the page
 * happened to contain. Reading 0 from them means "nobody filled this in", NOT
 * "the CS never ran".
 *
 * That misreading cost a root cause: on 2026-08-18 `CMD_PTR=0 BLOCKED_REASON=0`
 * was taken as proof the command stream never started, which "refuted" TILER_OOM
 * for two days. The registers that ARE real - CsAck/CsReq, read directly from the
 * FW interface - said the opposite the whole time.
 *
 * Same handshake shape as the CSG state requests: toggle the bit relative to ACK,
 * ring the CSG doorbell, wait for the ack to match. Bounded at 100 ms because this
 * runs on the timeout path, which is already slow, and a FW that will not answer a
 * status request is itself the finding. */
static VOID
WinMaliCsfRequestStatusUpdate_(_Inout_ PWINMALI_ADAPTER Adapter,
                               _Inout_ WINMALI_FWCTX*   Fw)
{
    ULONG ack;
    ULONG acked = 0;

    if (Fw->Csg0Input == NULL || Fw->Csg0Output == NULL) {
        return;
    }
    ack = Fw->Csg0Output->Ack;
    WinMaliCsfCsgUpdateReqs_(Fw, Fw->Csg0Input,
                             (ack ^ WINMALI_CSG_STATUS_UPDATE),
                             WINMALI_CSG_STATUS_UPDATE);
    KeMemoryBarrier();
    /* ⛔ THE GLOBAL DOORBELL MASK MUST BE TOGGLED FIRST, exactly as the CSG
     * state path does (WinMaliCsfProgramAndStartCsg_). 1.1.9.20 rang DOORBELL(0)
     * WITHOUT it and the FW never looked: measured
     *   WMSTATUPD1 req=0x00000031 ack=0x00000011 acked=0x00000000
     * i.e. our REQ bit 5 set, the FW's ACK bit 5 never moved in 100 ms - while the
     * SAME firmware was acking the rebind handshake on every retry. The request was
     * not refused, it was never delivered. */
    WinMaliFwGlbToggleDoorbellMask_(Adapter, Fw, 1u);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_CSF_DOORBELL(0), 1u);
    (VOID)WinMaliFwCsgWaitReqAck_(Fw, WINMALI_CSG_STATUS_UPDATE, 100u, &acked);
    WINMALI_TRACE("CSF: WMSTATUPD1 status update req=0x%08x ack=0x%08x acked=0x%08x",
                  Fw->Csg0Input->Req, Fw->Csg0Output->Ack, acked);
}

static NTSTATUS
WinMaliCsfSubmitStreamCall_(
    _Inout_ PWINMALI_ADAPTER Adapter,
    _Inout_ WINMALI_FWCTX*   Fw,
    _In_ UINT64              StreamGpuVa,
    _In_ ULONG               StreamSizeBytes,
    _In_ ULONG               LatestFlush,
    _In_ ULONG               CsgAsSlot,
    _In_ ULONG               TimeoutMs)
{
    UINT64             addrReg;
    UINT64             valReg;
    UINT64             syncGpuVa  = WINMALI_CSF_SYNC_GPU_VA;
    UINT64             tagGpuVa   = WINMALI_CSF_SYNC_GPU_VA + WINMALI_CSF_TAG_OFFSET;
    UINT64             jobSeq;
    ULONG              ringMask;
    UINT64             insertCont;   /* the continuous counter, unmasked */
    UINT64             ringInsert;   /* insertCont & ringMask: the copy offset */
    UINT64             ringExtract;
    SIZE_T             padBytes;     /* NOP filler when the block would cross the seam */
    UINT64             callInstrs[WINMALI_CSF_NUM_INSTRS_PER_SLOT];
    volatile WINMALI_PANTHOR_SYNCOBJ64* sync;
    volatile UINT64*   tagSlot;
    UINT64             tagBefore;
    ULONG              sbWaitMask;
    ULONG              i;
    LARGE_INTEGER      t;
    LONG               faultsBefore;
    NTSTATUS           status = STATUS_SUCCESS;

    if (!Fw->CsfBootValid || Fw->HostRingVa == NULL || Fw->QRingIn == NULL || Fw->QRingOut == NULL
        || Fw->Cs00Input == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Bind the CSG to the requested address space (a UMD group's VM, or the
     * bring-up AS for the NOP self-test). If the rebind fails to ack, don't
     * run the stream in the wrong AS - bail so the caller can fall back. */
    status = WinMaliCsfRebindCsgAs_(Adapter, Fw, CsgAsSlot);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("CSF submit: CSG AS rebind to %u failed 0x%08x", CsgAsSlot, status);
        return status;
    }

    addrReg = (UINT64)Fw->CsRegCount - (UINT64)Fw->CsUnpreservedRegCount;
    valReg  = addrReg + 2ull;
    if (Fw->SbSlotCount == 0u) {
        sbWaitMask = 1u;
    } else if (Fw->SbSlotCount >= 16u) {
        sbWaitMask = 0xffffu;
    } else {
        sbWaitMask = (1u << Fw->SbSlotCount) - 1u;
    }

    jobSeq = ++Fw->KqJobSeq;
    sync   = (volatile WINMALI_PANTHOR_SYNCOBJ64*)Fw->HostSyncVa;
    /* Tag slot: instr 9 adds TAG_PATTERN here AFTER the callee CALL returns.
       Snapshot it now; if it moves by the timeout, the callee stream ran to
       completion and the stall is on the post-callee sync path - otherwise
       the callee (the UMD's CS) faulted or hung inside the CALL. */
    tagSlot   = (volatile UINT64*)((PUCHAR)Fw->HostSyncVa + WINMALI_CSF_TAG_OFFSET);
    tagBefore = *tagSlot;

    //
    // Trampoline (panthor-style + extended). 16 slots, cacheline-aligned.
    //
    // The tag write is placed IMMEDIATELY after the CALL, BEFORE the WAIT(all),
    // so it witnesses "the callee CALL returned" independently of whether the
    // callee's async scoreboard work (tiling etc.) ever completes. This splits
    // the two failure modes at the timeout:
    //   * tag moved, seqno didn't -> CALL returned; the WAIT(all) hung on the
    //     callee's scoreboard (async GPU work never signalled).
    //   * tag unchanged           -> the CALL itself never returned (the callee
    //     CS couldn't execute in this CSG context at all).
    //
    //   0  MOVE32  val_reg, latest_flush          (cs.latest_flush load)
    //   1  FLUSH_CACHE2.clean_inv_all signal=0    (wait sb_slot 0 covers it)
    //   2  MOVE48  addr_reg, stream_gpu_va        (callee CSF stream VA)
    //   3  MOVE32  val_reg,  stream_size_bytes
    //   4  WAIT(0)                                (waits for FLUSH_CACHE2)
    //   5  CALL    [addr_reg:addr_reg+1], val_reg (executes callee stream)
    //
    //   --- witness: CALL returned (no WAIT dependency) ---
    //   6  MOVE48  addr_reg, tag_gpu_va
    //   7  MOVE48  val_reg,  TAG_PATTERN
    //   8  SYNC_ADD64 [addr_reg:addr_reg+1] += val_reg
    //
    //   --- primary signal: callee async done, then bump the syncobj ---
    //   9  MOVE48  addr_reg, sync_gpu_va
    //  10  MOVE48  val_reg,  1
    //  11  WAIT(all)                              (callee scoreboard work done)
    //  12  SYNC_ADD64 [addr_reg:addr_reg+1] += val_reg
    //
    //  13  (NOP)
    //  14  (NOP)
    //  15  ERROR_BARRIER
    //
    RtlZeroMemory(callInstrs, sizeof(callInstrs));
    callInstrs[0]  = (2ull  << 56) | (valReg  << 48) | (UINT64)LatestFlush;
    callInstrs[1]  = (36ull << 56) | (0ull    << 48) | (valReg << 40) | (0ull << 16) | 0x233ull;
    callInstrs[2]  = (1ull  << 56) | (addrReg << 48) | StreamGpuVa;
    callInstrs[3]  = (2ull  << 56) | (valReg  << 48) | (UINT64)StreamSizeBytes;
    callInstrs[4]  = (3ull  << 56) | (1ull << 16);
    callInstrs[5]  = (32ull << 56) | (addrReg << 40) | (valReg << 32);
    callInstrs[6]  = (1ull  << 56) | (addrReg << 48) | tagGpuVa;
    callInstrs[7]  = (1ull  << 56) | (valReg  << 48) | WINMALI_CSF_TAG_PATTERN;
    callInstrs[8]  = (51ull << 56) | (0ull    << 48) | (addrReg << 40) | (valReg << 32) | (0ull << 16) | 1ull;
    callInstrs[9]  = (1ull  << 56) | (addrReg << 48) | syncGpuVa;
    callInstrs[10] = (1ull  << 56) | (valReg  << 48) | 1ull;
    callInstrs[11] = (3ull  << 56) | ((UINT64)sbWaitMask << 16);
    callInstrs[12] = (51ull << 56) | (0ull    << 48) | (addrReg << 40) | (valReg << 32) | (0ull << 16) | 1ull;
    callInstrs[13] = 0ull; // explicit NOP — opcode 0 is NOP per v10.xml CEU Opcode
    callInstrs[14] = 0ull; // explicit NOP
    callInstrs[15] = (47ull << 56);

    /* ⛔ INSERT/EXTRACT ARE CONTINUOUS COUNTERS, NOT RING OFFSETS.
     *
     * The mask belongs on the COPY (where in the 64 KiB buffer the bytes go),
     * never on the value written back. The previous code wrote the WRAPPED
     * offset back into Insert, which silently truncates the counter the moment
     * the FW's Extract passes WINMALI_CSF_RING_BYTES - and then Insert < Extract
     * forever.
     *
     * That is not a new failure mode: WinMaliCsfProgramAndStartCsg_ already
     * documents it for the zero-Insert case - "the trampoline we append lands
     * BEHIND the extract pointer and the FW never executes it (submit then times
     * out with the ring looking empty and no fault)". Masking is the same bug,
     * just deferred to the 512th submit.
     *
     * ⚠ NOT claimed as the cause of the timeouts measured on 2026-08-16: those
     * happened ~6 submits into a fresh boot, with Extract far below 64 KiB where
     * the mask is a no-op. This is a latent bug fixed on its own terms. */
    ringMask    = (ULONG)(WINMALI_CSF_RING_BYTES - 1u);
    insertCont  = Fw->QRingIn->Insert;                    /* continuous */
    ringInsert  = insertCont & (UINT64)ringMask;           /* offset, for the copy */
    ringExtract = Fw->QRingOut->Extract;

    /* ⛔★★★★★ A RING MUST WRAP. REFUSING AT THE SEAM WEDGES THE GPU PERMANENTLY.
     *
     * The old guard returned STATUS_BUFFER_OVERFLOW whenever the 128-byte block
     * would not fit between the wrapped insert offset and the END of the 64 KiB
     * ring - i.e. it treated the seam as an error instead of wrapping. Because
     * Insert is NOT advanced on that path, the offset stays in the dead zone,
     * so EVERY subsequent submit fails too. It is a one-way wedge.
     *
     * MEASURED LIVE 2026-08-18, with a full desktop up:
     *     RingInsert   = 131048   -> offset 65512   (dead zone starts at 65409)
     *     RingExtract  = 130920
     *     outstanding  = 128 bytes of 65536         <-- the ring was EMPTY
     *     CsOk         = 955 ... then CsfFailCount  = 21956, all 0x80000005
     *     dwm AND explorer: "panfrost_batch_submit failed: 5", forever
     * 955 submits worked, then Insert landed 24 bytes short of the end and the
     * GPU never accepted another job. Retrying made it worse, not better:
     * WinMaliCsfProgramAndStartCsg_ reseeds Insert from the FW's Extract on every
     * rebind, so the "reset the slot and retry" path re-entered the SAME dead
     * offset each time - 21,956 times.
     *
     * ⚠ The offset is not 128-aligned (65512 = 65536-24) because Insert is
     * seeded from the FW's Extract after the bring-up NOP self-test, so you
     * cannot assume the block lands on a tidy boundary and "just fits".
     *
     * FIX: pad the tail to the seam with NOPs and restart the block at 0.
     * Opcode 0 is NOP (v10.xml CEU Opcode - same fact instrs 13/14 rely on), so
     * zero-fill is executable filler. Padding is consumed by the CONTINUOUS
     * counter, which keeps Insert monotonic - never masked, see the note above.
     * The block therefore stays contiguous; nothing is split across the seam.
     *
     * The overflow return now means what it says: the ring is genuinely FULL
     * (unconsumed bytes + what we are about to add exceed its capacity).
     */
    padBytes = 0;
    if (ringInsert + sizeof(callInstrs) > WINMALI_CSF_RING_BYTES) {
        padBytes = (SIZE_T)((UINT64)WINMALI_CSF_RING_BYTES - ringInsert);
    }
    if ((insertCont + padBytes + sizeof(callInstrs)) - ringExtract >
        (UINT64)WINMALI_CSF_RING_BYTES) {
        WINMALI_WARN("CSF: ring genuinely FULL - insert=%I64u extract=%I64u pad=%Iu",
                     insertCont, ringExtract, padBytes);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (padBytes != 0) {
        RtlZeroMemory((PUCHAR)Fw->HostRingVa + (SIZE_T)ringInsert, padBytes);
        insertCont += padBytes;
        ringInsert  = 0;
        WINMALI_TRACE("CSF: WMRINGWRAP1 padded %Iu B of NOP to the seam; insert now %I64u",
                      padBytes, insertCont);
    }

    RtlCopyMemory((PUCHAR)Fw->HostRingVa + (SIZE_T)ringInsert, callInstrs, sizeof(callInstrs));
    KeMemoryBarrier();

    Fw->QRingIn->Extract = ringExtract;
    Fw->QRingIn->Insert  = insertCont + sizeof(callInstrs);
    KeMemoryBarrier();

    //
    // Re-arm the CSG slot AFTER the new work is in the ring. If we wake
    // before the ring update, the FW briefly re-allocates endpoints,
    // observes extract == insert (queue still empty), and immediately
    // re-idles, leaving us back where we started. Doing the wake here
    // means that when the FW processes ENDPOINT_CONFIG / re-evaluates
    // the queue, it sees outstanding work and keeps the endpoints live.
    //
    // The wake also toggles the global doorbell mask for CSG slot 0 and
    // rings DOORBELL(0). Combined with the per-CS doorbell on DOORBELL(1)
    // below, this covers both the "CSG was idle" path (FW needs the
    // global doorbell to notice the wake) and the "CSG still hot" path
    // (per-CS doorbell is enough; the wake no-ops). The per-CS doorbell
    // is harmless and idempotent in either case.
    //
    (VOID)WinMaliCsfWakeCsgIfIdle_(Adapter, Fw);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_CSF_DOORBELL(1), 1u);

    /* RECOVERY HOOK: if the cores are down, bring them up BEFORE the draw. The
       probe below is read-only and was only ever a diagnostic; this acts on
       what it found. No-op unless Parameters\ShaderPwrOn is set, and a no-op
       too when the cores are already READY - we never fight the MCU for a
       domain that is working. */
    if (StreamSizeBytes > 64u) {
        (VOID)WinMaliFwShaderPowerOn_(Adapter);
    }
    /* SHADER-CORE POWER PROBE (diagnostic 2026-07-13). "clear works, shaded
       draw is black, no fault" points at the shader cores never executing. On
       CSF the MCU powers cores on demand; if it never does, the CS completes
       and writes empty (clear) tiles. Sample SHADER_READY/PWRTRANS in a tight
       burst right after the doorbell to catch the (microsecond-scale) power-up
       window - the 10ms poll below would miss it. One-shot per boot, real
       draws only (StreamSizeBytes>64 skips the 40-byte tiler-heap init CS).
       SHADER_READY(any)=0 across a real draw while L2_READY!=0 == cores never
       came up == the root cause. Read-only. */
    if (StreamSizeBytes > 64u) {
        static BOOLEAN s_shaderProbeReported = FALSE;
        if (!s_shaderProbeReported) {
            UINT64 shReadyAny = 0, shTransAny = 0;
            ULONG tlReadyAny = 0;
            ULONG s;
            s_shaderProbeReported = TRUE;
            for (s = 0; s < 200000u; ++s) {
                shReadyAny |= ((UINT64)WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_SHADER_READY_HI) << 32)
                            | WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_SHADER_READY_LO);
                shTransAny |= ((UINT64)WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_SHADER_PWRTRANS_HI) << 32)
                            | WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_SHADER_PWRTRANS_LO);
                /* TILER is a SEPARATE power domain from the shader cores. A
                   shaded draw uses RUN_IDVS (tiler binning) then RUN_FRAGMENT;
                   a clear uses only RUN_FRAGMENT. -render(clear)=OK but every
                   shaded draw is black with no fault -> the tiler may never
                   power up (MCU only powers cores it thinks are needed). If
                   TILER_READY(any)=0 across a real draw while SHADER_READY!=0,
                   the tiler never came up == binning never happened == the
                   root cause. Read-only. */
                tlReadyAny |= WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_TILER_READY_LO);
                if (sync->Seqno >= jobSeq) {
                    break;
                }
            }
            ULONG l2Ready = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_L2_READY_LO);
            ULONG tlPresent = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_TILER_PRESENT_LO);
            WINMALI_WARN("SHADER PROBE (first real draw size=%u iters=%lu): "
                         "SHADER_READY(any)=0x%llx SHADER_PWRTRANS(any)=0x%llx "
                         "L2_READY=0x%08x SHADER_PRESENT=0x%llx "
                         "TILER_READY(any)=0x%08x TILER_PRESENT=0x%08x",
                         StreamSizeBytes, s, (ULONGLONG)shReadyAny, (ULONGLONG)shTransAny,
                         l2Ready, (ULONGLONG)Fw->ShaderCoreMask,
                         tlReadyAny, tlPresent);
            /* ⛔ THIS PROBE HAS ALWAYS BEEN UNREADABLE. It answers the single
               most important question on this front - do the shader cores and
               the tiler ever power up for a real draw - and it wrote ONLY to
               the EDK2 ring, which is fill-once and has been full for a long
               time. So a probe that fired correctly on every boot has never
               once been read. Publish it: PASSIVE here (the wait loop below
               calls KeDelayExecutionThread), new value names, one writer. */
            if (g_WinMaliRegPath.Length != 0) {
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeReadyLo",
                                              (ULONG)(shReadyAny & 0xFFFFFFFFull));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeReadyHi",
                                              (ULONG)((shReadyAny >> 32) & 0xFFFFFFFFull));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeTransLo",
                                              (ULONG)(shTransAny & 0xFFFFFFFFull));
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeL2Ready", l2Ready);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeTilerRdy", tlReadyAny);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeTilerPre", tlPresent);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeIters", (ULONG)s);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeSize", StreamSizeBytes);
                (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"ShProbeCoreLo",
                                              (ULONG)(Fw->ShaderCoreMask & 0xFFFFFFFFull));
            }
        }
    }

    /* Snapshot the ISR's fault counter BEFORE waiting, so a fault can be
       attributed to THIS draw instead of to the boot. */
    faultsBefore = InterlockedCompareExchange(&Adapter->FaultCount, 0, 0);

    /* ⛔ PUBLISH THE RING STATE ON EVERY SUBMIT, NOT ONLY ON TIMEOUT.
     *
     * These lived exclusively in the timeout branch, so a WORKING driver
     * published nothing and every value read 0 - identical to "not
     * instrumented". The first verification run after the ring fix was
     * therefore unreadable: Insert/Extract/seqno all 0 with no timeout, which
     * proves neither success nor idleness. Log OUTSIDE the branch.
     *
     * These are the pre-submit values; the timeout branch re-publishes the
     * post-wait ones. Cheap: three interlocked stores, no registry I/O. */
    (VOID)InterlockedExchange(&g_WinMaliStatRingInsert,
                              (LONG)(ULONG)(Fw->QRingIn->Insert & 0xFFFFFFFFull));
    (VOID)InterlockedExchange(&g_WinMaliStatRingExtract,
                              (LONG)(ULONG)(Fw->QRingOut->Extract & 0xFFFFFFFFull));
    (VOID)InterlockedExchange(&g_WinMaliStatSeqnoLo,
                              (LONG)(ULONG)(sync->Seqno & 0xFFFFFFFFull));
    (VOID)InterlockedIncrement(&g_WinMaliStatCsSubmitCount);

    for (i = 0; i < TimeoutMs; ++i) {
        KeMemoryBarrier();
        /* ★ UNBLOCK A PARKED STREAM INSTEAD OF TIMING OUT ON IT.
           The FW parks the CS on TILER_OOM and waits for the host to supply a
           tiler chunk. Servicing it here lets THIS submit run to completion -
           no retry, no terminate, no rebind. Before this existed the wait simply
           expired and the "recovery" path tore down a slot that was only ever
           waiting for memory, which is why one exhaustion wedged the GPU for the
           rest of the boot. Cost is two volatile reads per millisecond. */
        (VOID)WinMaliCsfServiceTilerOom_(Adapter, Fw, CsgAsSlot);
        if (sync->Seqno >= jobSeq) {
            /* A COMPLETION COUNTER. "no timeouts" is not evidence of success -
               it is also what an idle GPU looks like. d(CsOk) > 0 with
               d(CsTimeouts) == 0 is the only reading that means the command
               stream is actually running to completion. */
            (VOID)InterlockedIncrement(&g_WinMaliStatCsOkCount);
            (VOID)InterlockedExchange(&g_WinMaliStatSeqnoLo,
                                      (LONG)(ULONG)(sync->Seqno & 0xFFFFFFFFull));
            (VOID)InterlockedExchange(&g_WinMaliStatRingExtract,
                                      (LONG)(ULONG)(Fw->QRingOut->Extract & 0xFFFFFFFFull));
            /* SUCCESS by seqno - but the seqno only proves the caller CS
               reached its post-WAIT SYNC_ADD. An ASYNC fault (shader core,
               a sibling CS, a speculative/parallel MMU access) can raise AND be
               W1C-acked by the ISR while completion still signals, so a draw
               that actually faulted looks "clean" and renders nothing.
               ⛔ THE PREVIOUS VERSION OF THIS CHECK WAS ITSELF SILENT: it
               reported only the FIRST fault per boot, so once one draw faulted
               every later one was invisible - and the UMD, seeing success,
               retried forever (measured: explorer burning a full core while the
               panel stayed black).
               Now: the ISR COUNTS every fault, so a change across this wait
               proves THIS draw faulted. Report every occurrence (rate-limited
               for the polled UART) and PUBLISH the count so it is readable
               without the log ring, which wraps. */
            LONG  faultsAfter = InterlockedCompareExchange(&Adapter->FaultCount, 0, 0);
            ULONG gfst    = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTSTATUS(CsgAsSlot));
            ULONG mfst    = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTSTATUS(WINMALI_MMU_MCU_AS));
            ULONG gpfs    = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_FAULT_STATUS);
            ULONG csFault = Fw->Cs00Output->Fault;
            ULONG csFatal = Fw->Cs00Output->Fatal;
            /* ⛔ "AS_FAULTSTATUS != 0" IS NOT A FAULT. That register is sticky
               and survives device restarts, so a single old fault made this
               check fire on EVERY submit forever, at an identical address, with
               the ISR counter proving no fault had occurred (faults 0->0). A
               live fault is ONLY the counter moving. The register is still worth
               reporting - but as corroboration, and only when it differs from
               the baseline captured at MMU init. */
            BOOLEAN thisDrawFaulted = (faultsAfter != faultsBefore);
            BOOLEAN gfstIsNew =
                (CsgAsSlot < WINMALI_MMU_AS_COUNT) &&
                (gfst != Adapter->FaultBaselineFst[CsgAsSlot]);
            BOOLEAN mfstIsNew =
                (gfst == 0) &&
                (mfst != Adapter->FaultBaselineFst[WINMALI_MMU_MCU_AS]);
            /* CS Fault/Fatal and GPU_FAULT_STATUS are per-submit state, not
               sticky across restarts, so they count on their own. */
            BOOLEAN anyFaultState = (gpfs || csFault || csFatal ||
                                     gfstIsNew || mfstIsNew);

            if (thisDrawFaulted || anyFaultState) {
                /* first 8, then every 256th - the count stays exact either way */
                if (faultsAfter <= 8 || (faultsAfter % 256) == 0) {
                    WINMALI_WARN("CSF POST-SUBMIT FAULT on a seqno-'complete' draw "
                                 "(THIS draw: %s; faults %d->%d; FST vs baseline: %s): "
                                 "group AS%u FST=0x%08x "
                                 "(exc=0x%02x %s access=%s) ADDR=0x%08x%08x",
                                 thisDrawFaulted ? "YES" : "NO - not a live fault",
                                 faultsBefore, faultsAfter,
                                 gfstIsNew ? "CHANGED" : "unchanged (sticky)",
                                 CsgAsSlot, gfst, (ULONG)(gfst & 0xffu),
                                 WinMaliFwDecodeMmuExc_(gfst), WinMaliFwDecodeAccessType_(gfst),
                                 WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTADDR_HI(CsgAsSlot)),
                                 WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTADDR_LO(CsgAsSlot)));
                    WINMALI_WARN("CSF POST-SUBMIT FAULT: latched AS%d FST=0x%08x ADDR=0x%08x%08x | "
                                 "MCU-AS FST=0x%08x GPU_FAULT=0x%08x CS Fault=0x%08x Fatal=0x%08x "
                                 "FaultInfo=0x%llx FatalInfo=0x%llx",
                                 Adapter->FaultAs, (ULONG)Adapter->FaultFst,
                                 (ULONG)Adapter->FaultAddrHi, (ULONG)Adapter->FaultAddrLo,
                                 mfst, gpfs, csFault, csFatal,
                                 (ULONGLONG)Fw->Cs00Output->FaultInfo,
                                 (ULONGLONG)Fw->Cs00Output->FatalInfo);
                }
                /* We are at PASSIVE here (the loop calls KeDelayExecutionThread),
                   so publishing is legal. Readable over WinRM, survives the ring
                   wrapping, and answers "is anything faulting right now?". */
                if (g_WinMaliRegPath.Length != 0) {
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatGpuFaultCount",
                                                  (ULONG)faultsAfter);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatGpuFaultFst",
                                                  (ULONG)Adapter->FaultFst);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatGpuFaultAddrLo",
                                                  (ULONG)Adapter->FaultAddrLo);
                    (VOID)WinMaliWriteParamsDword(&g_WinMaliRegPath, L"StatGpuFaultAddrHi",
                                                  (ULONG)Adapter->FaultAddrHi);
                }

                /* Parameters\FailOnGpuFault=1 turns a faulting draw into a FAILED
                   submit, so the UMD stops believing it rendered. Default 0:
                   surfacing it is the fix asked for, but failing every draw is a
                   behaviour change that could crash-loop the compositor, and that
                   should be a measured decision rather than a side effect. */
                if (g_WinMaliFailOnGpuFault && thisDrawFaulted) {
                    return STATUS_DEVICE_DATA_ERROR;
                }
            }
            return STATUS_SUCCESS;
        }
        t.QuadPart = -10000LL;
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    status = STATUS_IO_TIMEOUT;
    {
        /* Pinpoint the stall. The tag-slot delta says whether the callee CALL
           returned; the per-AS MMU fault registers say what VA the GPU choked
           on (the UMD's CS lives in CsgAsSlot; the FW/MCU runs in the MCU AS). */
        UINT64 tagAfter = *tagSlot;
        BOOLEAN calleeReturned = (tagAfter != tagBefore);
        ULONG  gfst  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTSTATUS(CsgAsSlot));
        ULONG  gflo  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTADDR_LO(CsgAsSlot));
        ULONG  gfhi  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTADDR_HI(CsgAsSlot));
        ULONG  mfst  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTSTATUS(WINMALI_MMU_MCU_AS));
        ULONG  mflo  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTADDR_LO(WINMALI_MMU_MCU_AS));
        ULONG  mfhi  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTADDR_HI(WINMALI_MMU_MCU_AS));
        ULONG  gpfs  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_FAULT_STATUS);
        ULONG  gpflo = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_FAULT_ADDR_LO);
        ULONG  gpfhi = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_FAULT_ADDR_HI);

        WINMALI_WARN("CSF job wait timeout (seq want=%llu got=%llu) tag 0x%llx->0x%llx : %s",
                     jobSeq, sync->Seqno, (ULONGLONG)tagBefore, (ULONGLONG)tagAfter,
                     calleeReturned
                       ? "callee CALL RETURNED - WAIT(all) hung on the callee's async "
                         "scoreboard (tiler/heap work never signalled)"
                       : "callee CALL did NOT return - CS parked mid-stream (see "
                         "CMD_PTR/BLOCKED_REASON below)");

        /* Where + why the CS parked. STATUS_* in the CS output iface is the
           FW's live view of the CS slot: CMD_PTR is the CS program counter
           (a VA into the trampoline or the callee stream), BLOCKED_REASON
           names the stall. This is the primary witness - v25's "ATOMIC@0
           fault" was a misread of an all-zero FAULTSTATUS (access-type 0
           decodes as ATOMIC); fault lines below only print when real. */
        /* ★ MAKE THE STATUS PAGE REAL BEFORE READING IT. */
        WinMaliCsfRequestStatusUpdate_(Adapter, Fw);
        {
            UINT64 cmdPtr  = Fw->Cs00Output->StatusCmdPtr;
            ULONG  blocked = Fw->Cs00Output->StatusBlockedReason & 0xFu;
            static const char* kBlocked[8] = {
                "UNBLOCKED", "WAIT", "PROGRESS_WAIT", "SYNC_WAIT",
                "DEFERRED",  "RESOURCE", "FLUSH", "?7" };
            WINMALI_WARN("CSF CS status: CMD_PTR=0x%llx BLOCKED_REASON=%u(%s) "
                         "WAIT=0x%08x REQ_RESOURCE=0x%08x SCOREBOARDS=0x%08x",
                         (ULONGLONG)cmdPtr, blocked, kBlocked[blocked],
                         Fw->Cs00Output->StatusWait,
                         Fw->Cs00Output->StatusReqResource,
                         Fw->Cs00Output->StatusScoreboards);
            WINMALI_WARN("CSF CS status: WAIT_SYNC ptr=0x%llx value=0x%08x  "
                         "FAULT=0x%08x FATAL=0x%08x  HEAP_ADDRESS=0x%llx",
                         (ULONGLONG)Fw->Cs00Output->StatusWaitSyncPtr,
                         Fw->Cs00Output->StatusWaitSyncValue,
                         Fw->Cs00Output->Fault, Fw->Cs00Output->Fatal,
                         (ULONGLONG)Fw->Cs00Output->HeapAddress);
            /* FAULT[7:0]=exception type (0x58=DATA_INVALID_FAULT etc.),
               FAULT_INFO carries the fault context incl. the offending CS
               address - the key to localizing which instruction/descriptor
               the frontend choked on. NOTE the FAULT/FAULT_INFO pair is
               LATCHED (we never ack CS_FAULT), so within one boot only the
               first fault report is trustworthy. */
            WINMALI_WARN("CSF CS status: FAULT_INFO=0x%llx FATAL_INFO=0x%llx",
                         (ULONGLONG)Fw->Cs00Output->FaultInfo,
                         (ULONGLONG)Fw->Cs00Output->FatalInfo);

            /* ★ PUBLISH THE TIMEOUT DIAGNOSIS WHERE IT CAN ACTUALLY BE READ.
             *
             * Everything needed to explain a hung submit is gathered right here
             * - CMD_PTR, BLOCKED_REASON, CS_FAULT/FATAL, the MMU banks, and
             * whether the callee CALL returned - and ALL of it goes only to
             * WINMALI_WARN, i.e. the EDK2 ring, which is FILL-ONCE and long
             * since full. So a timeout has been reporting itself as a bare
             * STATUS_IO_TIMEOUT with the entire explanation discarded.
             *
             * Latched (last-value-wins) plus a count, so one registry read gives
             * the reason and how often it happens. Same discipline as the other
             * published counters: new value names, exactly ONE writer. */
            /* ★★★★★ ONE-SHOT: ONLY THE FIRST TIMEOUT CARRIES INFORMATION.
             *
             * MEASURED 2026-08-20 on a live wedge: every retry after the first
             * prints an IDENTICAL report - want climbs 1178,1179,1180... while
             * got stays pinned at 1154, same tag, same stream_va, CMD_PTR=0,
             * BLOCKED_REASON=0(UNBLOCKED), every fault bank zero, and the FW
             * consuming exactly 40 B of each 128 B block we write. That steady
             * state is all a timer-published TraceTail can ever contain, because
             * the 256 KB ring wraps many times before WinRM is reachable (~70 s),
             * and it says nothing about the cause: it is the shape of an
             * already-wedged GPU, not the transition into one.
             *
             * So dump the whole state ONCE, on the first timeout only, then latch
             * the trap so the present worker snapshots the ring around it into
             * TraceAtFirstTmo. What that snapshot buys is the part nobody has
             * seen: the lines covering the ~1150 submits that SUCCEEDED right
             * before this one, which is the only available comparison.
             *
             * ⚠ This is a WITNESS, not a fix - it changes no submit behaviour. */
            if (InterlockedCompareExchange(&g_WinMaliTrapFirstTmo, 1, 0) == 0) {
                WINMALI_WARN("CSF: WMFIRSTTMO1 FIRST TIMEOUT OF BOOT after %d ok submits: "
                             "seq want=%llu got=%llu callee=%s as=%u pid=%p "
                             "stream_va=0x%llx size=%u insert=%llu extract=%llu "
                             "cmdptr=0x%llx blocked=%u wait=0x%08x sb=0x%08x "
                             "csfault=0x%08x csfatal=0x%08x faultinfo=0x%llx "
                             "asfault=0x%08x@0x%08x%08x mcufault=0x%08x gpufault=0x%08x "
                             "|| ISR-LATCHED: faultCount=%d as=%d fst=0x%08x addr=0x%08x%08x",
                             (int)g_WinMaliStatCsOkCount,
                             jobSeq, sync->Seqno,
                             calleeReturned ? "RETURNED" : "NO-RETURN",
                             CsgAsSlot, PsGetCurrentProcessId(),
                             (ULONGLONG)StreamGpuVa, StreamSizeBytes,
                             (ULONGLONG)Fw->QRingIn->Insert,
                             (ULONGLONG)Fw->QRingOut->Extract,
                             (ULONGLONG)cmdPtr, blocked,
                             Fw->Cs00Output->StatusWait,
                             Fw->Cs00Output->StatusScoreboards,
                             Fw->Cs00Output->Fault, Fw->Cs00Output->Fatal,
                             (ULONGLONG)Fw->Cs00Output->FaultInfo,
                             gfst, gfhi, gflo, mfst, gpfs,
                             (int)InterlockedCompareExchange(&Adapter->FaultCount, 0, 0),
                             (int)Adapter->FaultAs, (ULONG)Adapter->FaultFst,
                             (ULONG)Adapter->FaultAddrHi, (ULONG)Adapter->FaultAddrLo);
            }

            (VOID)InterlockedIncrement(&g_WinMaliStatCsTimeoutCount);
            (VOID)InterlockedExchange(&g_WinMaliStatCsCmdPtrLo,
                                      (LONG)(ULONG)(Fw->Cs00Output->StatusCmdPtr & 0xFFFFFFFFull));
            (VOID)InterlockedExchange(&g_WinMaliStatCsCmdPtrHi,
                                      (LONG)(ULONG)((Fw->Cs00Output->StatusCmdPtr >> 32) & 0xFFFFFFFFull));
            (VOID)InterlockedExchange(&g_WinMaliStatCsBlocked,
                                      (LONG)(Fw->Cs00Output->StatusBlockedReason & 0xFu));
            (VOID)InterlockedExchange(&g_WinMaliStatCsFault,  (LONG)Fw->Cs00Output->Fault);
            (VOID)InterlockedExchange(&g_WinMaliStatCsFatal,  (LONG)Fw->Cs00Output->Fatal);
            (VOID)InterlockedExchange(&g_WinMaliStatCsCalleeRet, (LONG)(calleeReturned ? 1 : 0));
            (VOID)InterlockedExchange(&g_WinMaliStatAsFaultSt,  (LONG)gfst);
            (VOID)InterlockedExchange(&g_WinMaliStatAsFaultLo,  (LONG)gflo);
            (VOID)InterlockedExchange(&g_WinMaliStatAsFaultHi,  (LONG)gfhi);
            (VOID)InterlockedExchange(&g_WinMaliStatMcuFaultSt, (LONG)mfst);
            (VOID)InterlockedExchange(&g_WinMaliStatGpuFaultSt, (LONG)gpfs);

            /* ⛔ IS THIS CS OUTPUT IFACE EVEN A LIVE CHANNEL?
             *
             * The whole diagnosis above is read out of Fw->Cs00Output, and the
             * measured picture is CMD_PTR=0, BLOCKED=0, FAULT=0 with only a
             * LATCHED FATAL - i.e. indistinguishable from a page the FW never
             * writes. A zero is only evidence if the channel is proven live,
             * and four broken channels in one session already read as "the code
             * never ran".
             *
             * The ring pointers are that proof, and they need no cooperation
             * from the FW's status block: WE write Insert, the FW writes
             * Extract. If Extract catches up to Insert the FW fetched and
             * consumed our trampoline; if it stays behind, the FW never looked
             * at the ring at all - and then CMD_PTR/BLOCKED/FATAL are not a
             * misconfigured CS, they are an unwritten page.
             *
             * These two numbers split the entire front in one read:
             *   Extract == Insert -> FW ran it; the CALL is what hung
             *   Extract <  Insert -> FW never fetched it; scheduler/doorbell */
            (VOID)InterlockedExchange(&g_WinMaliStatRingInsert,
                                      (LONG)(ULONG)(Fw->QRingIn->Insert & 0xFFFFFFFFull));
            (VOID)InterlockedExchange(&g_WinMaliStatRingExtract,
                                      (LONG)(ULONG)(Fw->QRingOut->Extract & 0xFFFFFFFFull));
            (VOID)InterlockedExchange(&g_WinMaliStatCsReq,  (LONG)Fw->Cs00Input->Req);
            (VOID)InterlockedExchange(&g_WinMaliStatCsAck,  (LONG)Fw->Cs00Output->Ack);
            (VOID)InterlockedExchange(&g_WinMaliStatCsgReq, (LONG)Fw->Csg0Input->Req);
            (VOID)InterlockedExchange(&g_WinMaliStatCsgAck, (LONG)Fw->Csg0Output->Ack);
            (VOID)InterlockedExchange(&g_WinMaliStatCsFatalInfoLo,
                                      (LONG)(ULONG)(Fw->Cs00Output->FatalInfo & 0xFFFFFFFFull));
            (VOID)InterlockedExchange(&g_WinMaliStatCsFatalInfoHi,
                                      (LONG)(ULONG)((Fw->Cs00Output->FatalInfo >> 32) & 0xFFFFFFFFull));
            /* The seqno the trampoline should have bumped, and the tag it
               should have stamped: two host-visible words the GPU writes
               DIRECTLY, bypassing the FW status block entirely. If either
               moves, the CS executed regardless of what the status page says. */
            (VOID)InterlockedExchange(&g_WinMaliStatSeqnoLo,
                                      (LONG)(ULONG)(sync->Seqno & 0xFFFFFFFFull));
            (VOID)InterlockedExchange(&g_WinMaliStatTagLo,
                                      (LONG)(ULONG)(tagAfter & 0xFFFFFFFFull));
        }

        if (gfst != 0u || mfst != 0u || gpfs != 0u) {
            WINMALI_WARN("CSF fault: group AS%u FAULTSTATUS=0x%08x (exc=0x%02x %s, access=%s) ADDR=0x%08x%08x",
                         CsgAsSlot, gfst, (ULONG)(gfst & 0xffu),
                         WinMaliFwDecodeMmuExc_(gfst), WinMaliFwDecodeAccessType_(gfst),
                         gfhi, gflo);
            WINMALI_WARN("CSF fault: MCU AS%u FAULTSTATUS=0x%08x ADDR=0x%08x%08x  GPU_FAULT=0x%08x ADDR=0x%08x%08x",
                         WINMALI_MMU_MCU_AS, mfst, mfhi, mflo, gpfs, gpfhi, gpflo);
        } else {
            /* The MMU/GPU banks being zero does NOT mean the GPU is happy - a
               command-stream error reports in CS_FAULT/CS_FATAL only. Say which
               of the two is true instead of collapsing both to "NONE". */
            ULONG csFault = Fw->Cs00Output->Fault & 0xffu;
            ULONG csFatal = Fw->Cs00Output->Fatal & 0xffu;

            if (csFault != 0u || csFatal != 0u) {
                WINMALI_WARN("CSF fault: MMU/GPU banks are zero, but THE COMMAND STREAM FAULTED: "
                             "CS_FAULT exc=0x%02x (%s)  CS_FATAL exc=0x%02x (%s)",
                             csFault, WinMaliFwDecodeCsExc_(csFault),
                             csFatal, WinMaliFwDecodeCsExc_(csFatal));
            } else {
                WINMALI_WARN("CSF fault: NONE (MMU/GPU banks AND CS_FAULT/CS_FATAL all zero)");
            }
        }

        /* A faulted user AS keeps its transaction stalled and wedges the CSG
           (the follow-up TERMINATE times out). Break the stall now so the
           caller's reset-to-bring-up rebind can actually complete. */
        if (gfst != 0u) {
            WinMaliCsfRecoverFaultedAs_(Adapter, CsgAsSlot);
        }
    }
    return status;
}

NTSTATUS
WinMaliCsfSubmitNopJob(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG TimeoutMs)
{
    WINMALI_FWCTX* fw;

    if (Adapter == NULL || Adapter->FwCtx == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    fw = (WINMALI_FWCTX*)Adapter->FwCtx;
    if ((Adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_CSF_JOBS) == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    /* Serialize on the single CSG slot with escape GroupSubmits. The NOP fence
       now runs on the async submit worker thread (WinMaliSchedWorker_), which
       can race a UMD escape GroupSubmit; both rebind + drive the one CSG, so
       take SubmitLock exactly like WinMaliCsfSubmitGroupStream does. */
    {
        NTSTATUS st;
        /* Bounded: a wedged holder must not park us forever (see 0x9F note in
           WinMaliWaitObjectAbortable). Give up rather than block teardown. */
        st = WinMaliWaitObjectAbortable(&fw->SubmitLock, NULL, 100);
        if (!NT_SUCCESS(st) || st == STATUS_TIMEOUT) {
            WINMALI_WARN("CSF NOP: could not take SubmitLock (0x%08x) - skipping", st);
            return STATUS_DEVICE_NOT_READY;
        }
        st = WinMaliCsfSubmitStreamCall_(
            Adapter,
            fw,
            WINMALI_CSF_SHADER_GPU_VA,
            (ULONG)sizeof(WinMaliShaderNop),
            0u,
            Adapter->GpuMmuBringupAs,   /* NOP self-test runs in the bring-up AS */
            TimeoutMs);
        KeReleaseMutex(&fw->SubmitLock, FALSE);
        return st;
    }
}

/*
 * Real UMD command-stream submission. Runs the UMD's CS stream (StreamGpuVa,
 * in the group's VM) by binding the shared CSG slot to the group's MMU AS and
 * executing the CALL trampoline. The CSF ring/sync pages are cross-mapped
 * into every group VM (WinMaliCsfMapIntoVm) so the trampoline resolves.
 *
 * Serialized against other processes via fw->SubmitLock (one shared CSG slot).
 * A single submit failing does NOT wedge the driver: the clean CSG bind
 * (TERMINATE->reprogram->START) resets the slot on the next submit, so the
 * next context still succeeds. On failure we just reset the slot to the
 * bring-up AS and return the error to this caller.
 */
NTSTATUS
WinMaliCsfSubmitGroupStream(_Inout_ PWINMALI_ADAPTER Adapter,
                            _In_ ULONG   GroupAsSlot,
                            _In_ UINT64  StreamGpuVa,
                            _In_ ULONG   StreamSizeBytes,
                            _In_ ULONG   LatestFlush,
                            _In_ ULONG   TimeoutMs)
{
    WINMALI_FWCTX* fw;
    NTSTATUS       st;

    if (Adapter == NULL || Adapter->FwCtx == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    fw = (WINMALI_FWCTX*)Adapter->FwCtx;
    if ((Adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_CSF_JOBS) == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (GroupAsSlot >= WINMALI_AS_SLOT_MAX || StreamGpuVa == 0 || StreamSizeBytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* One CSG slot + one ring, but concurrent escapes across processes.
       Serialize the entire bind/submit/wait so submits don't corrupt each
       other. Runs at PASSIVE_LEVEL (the wait poll uses KeDelayExecutionThread),
       so a KMUTEX keeps us at PASSIVE while other submitters block here. */
    /* ⛔ Bounded + abortable. This is the escape path a USER THREAD enters, and
       the raw infinite non-alertable wait that used to be here is what made a
       wedged rendertest unkillable and then bugchecked the shutdown with
       0x9F (P1=4, P2=0x12C = the 300 s PnP timeout). Fail the submit instead. */
    st = WinMaliWaitObjectAbortable(&fw->SubmitLock, NULL, 100);
    if (!NT_SUCCESS(st) || st == STATUS_TIMEOUT) {
        WINMALI_WARN("CSF: SubmitLock unavailable (0x%08x) - aborting submit "
                     "(thread terminating or device stopping)", st);
        return STATUS_DEVICE_NOT_READY;
    }

    st = WinMaliCsfSubmitStreamCall_(
             Adapter, fw, StreamGpuVa, StreamSizeBytes, LatestFlush,
             GroupAsSlot, TimeoutMs);
    if (!NT_SUCCESS(st)) {
        WINMALI_WARN("CSF: group stream submit failed 0x%08x (as=%u) - resetting "
                     "slot to bring-up AS; next submit retries", st, GroupAsSlot);
        (VOID)WinMaliCsfRebindCsgAs_(Adapter, fw, Adapter->GpuMmuBringupAs);
    }

    KeReleaseMutex(&fw->SubmitLock, FALSE);
    return st;
}

/*
 * Cross-map the bring-up CSF ring, sync page and NOP-shader page into a UMD
 * group's VM at the same fixed low GPU VAs the CSG expects (RING/SYNC/SHADER
 * _GPU_VA). Those VAs (0x41x000) are far below the UMD user VA window
 * (0x10000000+) and the very-high UMD scratch (0xfffff...), so they never
 * collide. Called from WinMaliVmCreate after the VM's page tables exist.
 * Best-effort: returns DEVICE_NOT_READY if CSF isn't booted yet; the submit
 * path then falls back to the NOP self-test.
 */
NTSTATUS
WinMaliCsfMapIntoVm(_Inout_ PWINMALI_ADAPTER Adapter, _Inout_ struct _WINMALI_VM_PT* VmPt)
{
    WINMALI_FWCTX* fw;
    PFN_NUMBER     ringPfns[WINMALI_CSF_RING_BYTES / 4096u];
    PFN_NUMBER     syncPfn;
    PFN_NUMBER     shaderPfn;
    ULONG          ringPages;
    ULONG          i;
    UINT64         ringPfn0;
    NTSTATUS       status;

    if (Adapter == NULL || VmPt == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    fw = (WINMALI_FWCTX*)Adapter->FwCtx;
    if (fw == NULL || !fw->CsfBootValid || fw->HostRingVa == NULL ||
        fw->HostSyncVa == NULL || fw->HostShaderVa == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    ringPages = WINMALI_CSF_RING_BYTES / 4096u;
    ringPfn0  = (UINT64)(fw->RingPa.QuadPart >> PAGE_SHIFT);
    for (i = 0; i < ringPages; ++i) {
        ringPfns[i] = (PFN_NUMBER)(ringPfn0 + i);
    }
    status = WinMaliMmuVmMap(Adapter, VmPt, WINMALI_CSF_RING_GPU_VA,
                             ringPfns, ringPages,
                             WINMALI_LPAE_L3_PAGE_ATTR_RW_NX);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("CsfMapIntoVm: ring map failed 0x%08x", status);
        return status;
    }

    syncPfn = (PFN_NUMBER)(fw->SyncPa.QuadPart >> PAGE_SHIFT);
    status  = WinMaliMmuVmMap(Adapter, VmPt, WINMALI_CSF_SYNC_GPU_VA,
                              &syncPfn, 1u, WINMALI_LPAE_L3_PAGE_ATTR_RW_NX);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("CsfMapIntoVm: sync map failed 0x%08x", status);
        return status;
    }

    shaderPfn = (PFN_NUMBER)(fw->ShaderPa.QuadPart >> PAGE_SHIFT);
    status    = WinMaliMmuVmMap(Adapter, VmPt, WINMALI_CSF_SHADER_GPU_VA,
                                &shaderPfn, 1u, WINMALI_LPAE_L3_PAGE_ATTR_RW_EX);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("CsfMapIntoVm: shader map failed 0x%08x", status);
        return status;
    }

    WINMALI_TRACE("CsfMapIntoVm: AS%u ring/sync/shader mapped at 0x%llx/0x%llx/0x%llx",
                  VmPt->AsSlot, (ULONGLONG)WINMALI_CSF_RING_GPU_VA,
                  (ULONGLONG)WINMALI_CSF_SYNC_GPU_VA,
                  (ULONGLONG)WINMALI_CSF_SHADER_GPU_VA);
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwPollSharedFwVersion_(_In_ WINMALI_FWCTX* Fw, _In_ ULONG TimeoutMs)
{
    WINMALI_FW_SECTION* sh;
    ULONG               i;
    LARGE_INTEGER       t;

    if (Fw->SharedIndex >= Fw->SectionCount) {
        return STATUS_NOT_FOUND;
    }
    sh = &Fw->Sections[Fw->SharedIndex];
    if (sh->BackingVa == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < TimeoutMs; ++i) {
        volatile ULONG* verWord = (volatile ULONG*)sh->BackingVa;
        KeMemoryBarrier();
        if (*verWord != 0) {
            return STATUS_SUCCESS;
        }
        t.QuadPart = -10000LL; // 1 ms
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    return STATUS_DEVICE_NOT_READY;
}

static NTSTATUS
WinMaliFwSetupGlbIface_(_Inout_ WINMALI_FWCTX* Fw)
{
    WINMALI_FW_SECTION* sh;
    volatile WINMALI_PANTHOR_FW_GLOBAL_CONTROL_IFACE* ctl;

    if (Fw->SharedIndex >= Fw->SectionCount) {
        return STATUS_NOT_FOUND;
    }
    sh  = &Fw->Sections[Fw->SharedIndex];
    ctl = (volatile WINMALI_PANTHOR_FW_GLOBAL_CONTROL_IFACE*)sh->BackingVa;
    if (ctl->Version == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    KeMemoryBarrier();

    Fw->GlbControl = ctl;
    Fw->GlbInput = (volatile WINMALI_PANTHOR_FW_GLOBAL_INPUT_IFACE*)WinMaliFwMcuVaToCpu_(
        sh,
        ctl->InputVa);
    Fw->GlbOutput = (volatile WINMALI_PANTHOR_FW_GLOBAL_OUTPUT_IFACE*)WinMaliFwMcuVaToCpu_(
        sh,
        ctl->OutputVa);
    if (Fw->GlbInput == NULL || Fw->GlbOutput == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeInitializeSpinLock(&Fw->GlbLock);
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwLoadSectionEntry_(
    _Inout_ PWINMALI_ADAPTER Adapter,
    _Inout_ WINMALI_FWCTX*   Fw,
    _In_reads_bytes_(ImageSize) PUCHAR ImageBase,
    _In_ SIZE_T               ImageSize,
    _Inout_ WINMALI_FW_ITER* Sub,
    _In_ ULONG                Ehdr)
{
    NTSTATUS                           status;
    WINMALI_FW_BINARY_SECTION_ENTRY_HDR hdr;
    ULONG                              nameLen;
    PHYSICAL_ADDRESS                   low;
    PHYSICAL_ADDRESS                   high;
    PVOID                              backing = NULL;
    SIZE_T                             secBytes;
    WINMALI_FW_SECTION*                dst;

    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Ehdr);

    status = WinMaliFwIterRead_(Sub, &hdr, sizeof(hdr));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (hdr.Data.End < hdr.Data.Start || hdr.Va.End < hdr.Va.Start) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (hdr.Data.End > ImageSize) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (((hdr.Va.Start & 0xfffu) != 0) || ((hdr.Va.End & 0xfffu) != 0)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (hdr.Flags & ~CSF_FW_BINARY_IFACE_ENTRY_RD_SUPPORTED_FLAGS) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (hdr.Flags & CSF_FW_BINARY_IFACE_ENTRY_RD_PROT) {
        return STATUS_SUCCESS;
    }
    if (hdr.Va.Start == CSF_MCU_SHARED_REGION_START
        && !(hdr.Flags & CSF_FW_BINARY_IFACE_ENTRY_RD_SHARED)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    nameLen = (ULONG)(Sub->Size - Sub->Offset);
    if (Fw->SectionCount >= WINMALI_FW_MAX_SECTIONS) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    dst         = &Fw->Sections[Fw->SectionCount];
    dst->Flags  = hdr.Flags;
    dst->VaStart = hdr.Va.Start;
    dst->VaEnd   = hdr.Va.End;

    if (nameLen > 0) {
        if (Sub->Offset + (SIZE_T)nameLen > Sub->Size) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        Sub->Offset += (SIZE_T)nameLen;
    }

    secBytes = (SIZE_T)((UINT64)hdr.Va.End - (UINT64)hdr.Va.Start);
    if (secBytes > 0) {
        low.QuadPart  = 0;
        high.QuadPart = (ULONG64)-1LL;
        backing = MmAllocateContiguousMemorySpecifyCache(
            secBytes,
            low,
            high,
            low,
            MmNonCached);
        if (backing == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(backing, secBytes);
        dst->BackingVa    = backing;
        dst->BackingPa    = MmGetPhysicalAddress(backing);
        dst->BackingBytes = secBytes;

        if (hdr.Data.End > hdr.Data.Start) {
            SIZE_T ds = (SIZE_T)(hdr.Data.End - hdr.Data.Start);
            RtlCopyMemory(backing, ImageBase + hdr.Data.Start, ds);
        }
    }

    if (hdr.Va.Start == CSF_MCU_SHARED_REGION_START) {
        Fw->SharedIndex = Fw->SectionCount;
    }

    WINMALI_TRACE(
        "FW: section[%lu] VA=[0x%08x..0x%08x) sz=%lu flags=0x%08x data=[0x%x..0x%x) backing_pa=0x%llx %s",
        Fw->SectionCount,
        dst->VaStart,
        dst->VaEnd,
        (ULONG)secBytes,
        dst->Flags,
        hdr.Data.Start,
        hdr.Data.End,
        (ULONGLONG)dst->BackingPa.QuadPart,
        (hdr.Va.Start == CSF_MCU_SHARED_REGION_START) ? "(SHARED)" : "");

    Fw->SectionCount++;
    return STATUS_SUCCESS;
}

static NTSTATUS
WinMaliFwParseImage_(_Inout_ PWINMALI_ADAPTER Adapter, _Inout_ WINMALI_FWCTX* Fw)
{
    NTSTATUS              status = STATUS_SUCCESS;
    WINMALI_FW_ITER       it;
    WINMALI_FW_BINARY_HDR hdr;

    it.Data   = (PUCHAR)Fw->Image;
    it.Size   = Fw->ImageSize;
    it.Offset = 0;

    status = WinMaliFwIterRead_(&it, &hdr, sizeof(hdr));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (hdr.Magic != CSF_FW_BINARY_HEADER_MAGIC) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (hdr.Major != CSF_FW_BINARY_HEADER_MAJOR_MAX) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (hdr.Size > it.Size || hdr.Size < sizeof(hdr)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    it.Size = (SIZE_T)hdr.Size;

    Fw->SharedIndex = 0xFFFFFFFFu;

    while (it.Offset < (SIZE_T)hdr.Size) {
        ULONG            ehdr;
        SIZE_T           entrySize;
        WINMALI_FW_ITER sub;

        status = WinMaliFwIterRead_(&it, &ehdr, sizeof(ehdr));
        if (!NT_SUCCESS(status)) {
            return status;
        }

        entrySize = (SIZE_T)CSF_FW_BINARY_ENTRY_SIZE(ehdr);
        if (entrySize < sizeof(ehdr) || (entrySize % 4u) != 0) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        if (CSF_FW_BINARY_ENTRY_TYPE(ehdr) == CSF_FW_BINARY_ENTRY_TYPE_IFACE) {
            status = WinMaliFwIterSubInit_(&it, &sub, entrySize - sizeof(ehdr));
            if (!NT_SUCCESS(status)) {
                return status;
            }
            status = WinMaliFwLoadSectionEntry_(Adapter, Fw, (PUCHAR)Fw->Image, Fw->ImageSize, &sub, ehdr);
            if (!NT_SUCCESS(status)) {
                return status;
            }
        } else {
            SIZE_T skip = entrySize - sizeof(ehdr);
            if (it.Offset + skip > it.Size) {
                return STATUS_INVALID_IMAGE_FORMAT;
            }
            if ((ehdr & CSF_FW_BINARY_ENTRY_OPTIONAL) == 0) {
                WINMALI_WARN("FW: unsupported mandatory entry type %lu", CSF_FW_BINARY_ENTRY_TYPE(ehdr));
                return STATUS_INVALID_IMAGE_FORMAT;
            }
            it.Offset += skip;
        }
    }

    if (Fw->SharedIndex == 0xFFFFFFFFu || Fw->SectionCount == 0) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    return STATUS_SUCCESS;
}

static VOID
WinMaliFwReleaseResources_(_Inout_opt_ WINMALI_FWCTX* Fw)
{
    ULONG i;

    if (Fw == NULL) {
        return;
    }
    for (i = 0; i < Fw->SectionCount; ++i) {
        if (Fw->Sections[i].BackingVa != NULL) {
            MmFreeContiguousMemory(Fw->Sections[i].BackingVa);
            Fw->Sections[i].BackingVa = NULL;
        }
    }
    if (Fw->PtPoolVa != NULL) {
        MmFreeContiguousMemory(Fw->PtPoolVa);
        Fw->PtPoolVa = NULL;
    }
    if (Fw->Image != NULL) {
        ExFreePoolWithTag(Fw->Image, WINMALI_POOL_TAG);
        Fw->Image = NULL;
    }
}

NTSTATUS
WinMaliFwInit(_Inout_ PWINMALI_ADAPTER Adapter)
{
    NTSTATUS           status;
    WINMALI_FWCTX*     fw = NULL;
    PHYSICAL_ADDRESS   low;
    PHYSICAL_ADDRESS   high;
    UINT64             transcfg = 0;
    UINT64             memattr  = 0;
    ULONG              spins;
    BOOLEAN            booted   = FALSE;
    BOOLEAN            timedout = FALSE;

    if (Adapter == NULL || !Adapter->GpuRegsMapped) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Adapter->FwCtx != NULL) {
        return STATUS_SUCCESS;
    }

    fw = (WINMALI_FWCTX*)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*fw), WINMALI_POOL_TAG);  /* 1809 WDK */
    if (fw == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(fw, sizeof(*fw));
    fw->OwnerAdapter = Adapter;

    /* NO GPU soft-reset here. v49 added an unconditional soft-reset and it
       REGRESSED cold boot: the CSF NOP self-test then failed (csf_jobs=0) so
       every real GroupSubmit returned DEVICE_NOT_READY. panthor deliberately
       does NOT soft-reset on the cold first-probe (only on warm resume) - a
       cold reset throws away the UEFI-initialised GPU/MCU state the bring-up
       depends on. Left as a no-op; revisit only for a warm-reinstall-gated
       path if that ever proves necessary. */

    status = WinMaliFwGpuL2PowerOn_(Adapter, &fw->L2Mask);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("FW: L2 power-on failed 0x%08x", status);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }

    status = WinMaliFwReadFileImage_(&fw->Image, &fw->ImageSize);
    if (!NT_SUCCESS(status)) {
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }

    status = WinMaliFwParseImage_(Adapter, fw);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("FW: parse mali_csffw.bin failed 0x%08x", status);
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }

    low.QuadPart  = 0;
    high.QuadPart = (ULONG64)-1LL;
    fw->PtPoolVa = MmAllocateContiguousMemorySpecifyCache(
        WINMALI_FW_PT_POOL_BYTES,
        low,
        high,
        low,
        MmNonCached);
    if (fw->PtPoolVa == NULL) {
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    fw->PtPoolPa    = MmGetPhysicalAddress(fw->PtPoolVa);
    fw->PtPoolBytes = WINMALI_FW_PT_POOL_BYTES;
    fw->PtUsed      = 0;

    status = WinMaliFwPtAllocPage_(fw, &fw->RootTablePa, (PVOID*)&fw->RootHostVa);
    if (!NT_SUCCESS(status)) {
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }

    fw->VaBits = Adapter->Hw.MmuFeatures & 0xFFu;
    if (fw->VaBits < 32u || fw->VaBits > 48u) {
        fw->VaBits = 48u;
    }

    status = WinMaliFwBuildPtForSections_(fw);
    if (!NT_SUCCESS(status)) {
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }

    WinMaliFwInitMcuDynVa_(fw);

    /*
     * ---- QUIESCE THE MCU BEFORE WE TOUCH ITS ADDRESS SPACE ------------------
     *
     * ⛔ THIS IS NOT A COLD BOOT. Our EDK2 already powers Mali and boots the CSF
     * firmware during UEFI ("MCU alive" is part of the LK-stage bring-up), so by
     * the time this driver runs the MCU is ALREADY EXECUTING a different
     * firmware image. The old code went straight to MCU_CONTROL=AUTO on the
     * comment "panthor_fw_start only writes AUTO (no DISABLE first on cold
     * boot)" - true for panthor on Linux, where the GPU really is cold, and
     * false here.
     *
     * Repointing AS0 (below) at OUR page tables while the MCU is mid-execution
     * from EDK2's image, then kicking AUTO, is a route to
     *     MCU_STATUS=3 (FATAL)  ->  WinMaliFwInit failed 0xc00000b5
     * which is exactly what we measured on 2026-08-12. With no CSF firmware,
     * every GroupSubmit returns 0xC00000A3 DEVICE_NOT_READY, csf_init fails,
     * pipe context_create returns NULL, CreateDevice fails, and dwm.exe dies.
     *
     * So: stop the MCU and WAIT for it to actually be DISABLED first. Log the
     * state we FOUND it in either way - we have never actually measured that,
     * and it decides whether EDK2 left it running.
     */
    {
        ULONG mcuStatus0  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_MCU_STATUS);
        ULONG mcuControl0 = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_MCU_CONTROL);
        ULONG i;

        DbgPrint("[WinMali]    FW: MCU state ON ENTRY: STATUS=%lu (%s) CONTROL=0x%08x\n",
                 mcuStatus0,
                 (mcuStatus0 == WINMALI_MCU_STATUS_DISABLED) ? "DISABLED" :
                 (mcuStatus0 == WINMALI_MCU_STATUS_ENABLED)  ? "ENABLED (EDK2 left it running)" :
                 (mcuStatus0 == WINMALI_MCU_STATUS_HALT)     ? "HALT" :
                 (mcuStatus0 == WINMALI_MCU_STATUS_FATAL)    ? "FATAL" : "?",
                 mcuControl0);

        if (mcuStatus0 != WINMALI_MCU_STATUS_DISABLED) {
            WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_MCU_CONTROL,
                             WINMALI_MCU_CONTROL_DISABLE);
            for (i = 0; i < 500u; ++i) {      /* up to ~500 ms */
                LARGE_INTEGER t;
                if (WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_MCU_STATUS) ==
                    WINMALI_MCU_STATUS_DISABLED) {
                    break;
                }
                t.QuadPart = -10000LL;        /* 1 ms */
                (VOID)KeDelayExecutionThread(KernelMode, FALSE, &t);
            }
            DbgPrint("[WinMali]    FW: MCU quiesce -> STATUS=%lu after %lu ms\n",
                     WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_MCU_STATUS), i);
        }
    }

    WinMaliMmuGetDefaultAsParams(Adapter, &transcfg, &memattr);
    (VOID)WinMaliMmuAsDisable(Adapter, WINMALI_MMU_MCU_AS);
    status = WinMaliMmuAsEnable(Adapter, WINMALI_MMU_MCU_AS, fw->RootTablePa, transcfg, memattr);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("FW: AS0 enable failed 0x%08x", status);
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }
    Adapter->FwMcuAsBound = TRUE;

    /* Clear stale IRQs from any prior driver instance / reset, then mask
       everything BEFORE we kick MCU_CONTROL. The boot poll below uses
       JOB_INT_RAWSTAT (raw, mask-independent) so we still see GLOBAL_IF
       even with the mask at 0. Once boot is confirmed we re-arm the
       per-block masks to the WinMali default set (handled by ISR). */
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_GPU_IRQ_CLEAR, 0xffffffffu);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_JOB_INT_CLEAR, 0xffffffffu);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_MMU_INT_CLEAR, 0xffffffffu);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_GPU_IRQ_MASK,  0u);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_JOB_INT_MASK,  0u);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_MMU_INT_MASK,  0u);

    // `panthor_fw_start` only writes MCU_CONTROL_AUTO (no DISABLE first on cold boot).
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_MCU_CONTROL, WINMALI_MCU_CONTROL_AUTO);

    //
    // Wait for the firmware to come alive.
    //
    // Panthor blocks on a wake_event that the IRQ handler signals when
    // GLOBAL_IF fires. Because Dxgkrnl already binds our ISR (and the ISR
    // unconditionally W1Cs JOB_INT_STAT to ack any pending bit), the IRQ
    // can — and on real hardware does — race ahead of this polling loop:
    // we'd see RAWSTAT == 0 even though the FW already booted. That's
    // exactly what we observed: shared[0] != 0 yet RAWSTAT/STAT were 0.
    //
    // The architecturally deterministic signal is the shared global
    // interface header itself: the firmware writes its `version` word as
    // part of its init handshake before raising GLOBAL_IF, and that word
    // never goes back to zero until reset. So we treat *either* an early
    // RAWSTAT hit (we won the race) *or* shared[0] != 0 (FW init clearly
    // ran, ISR may have already cleared the IRQ) as proof of life. Same
    // 1 s budget as panthor.
    //
    {
        volatile ULONG* sharedVer = NULL;
        if (fw->SharedIndex < fw->SectionCount
            && fw->Sections[fw->SharedIndex].BackingVa != NULL) {
            sharedVer = (volatile ULONG*)fw->Sections[fw->SharedIndex].BackingVa;
        }

        for (spins = 0; spins < 1000u; ++spins) {
            ULONG raw = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_JOB_INT_RAWSTAT);
            if ((raw & WINMALI_JOB_INT_GLOBAL_IF) != 0) {
                WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_JOB_INT_CLEAR, raw);
                booted = TRUE;
                break;
            }
            KeMemoryBarrier();
            if (sharedVer != NULL && *sharedVer != 0u) {
                booted = TRUE;
                break;
            }
            {
                LARGE_INTEGER t;
                t.QuadPart = -10000LL; // 1 ms
                (VOID)KeDelayExecutionThread(KernelMode, FALSE, &t);
            }
        }
    }

    if (!booted) {
        timedout = TRUE;
    }

    if (timedout) {
        ULONG mcu    = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_MCU_STATUS);
        ULONG jraw   = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_JOB_INT_RAWSTAT);
        ULONG jstat  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_JOB_INT_STAT);
        ULONG jmask  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_JOB_INT_MASK);
        ULONG mctl   = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_MCU_CONTROL);
        ULONG as0fst = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTSTATUS(WINMALI_MMU_MCU_AS));
        ULONG as0flo = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTADDR_LO(WINMALI_MMU_MCU_AS));
        ULONG as0fhi = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_AS_FAULTADDR_HI(WINMALI_MMU_MCU_AS));
        ULONG gpufst = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_FAULT_STATUS);
        ULONG gpuflo = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_FAULT_ADDR_LO);
        ULONG gpfhi  = WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_GPU_FAULT_ADDR_HI);
        ULONG sh0    = 0;
        UINT64 faultVa = ((UINT64)as0fhi << 32) | (UINT64)as0flo;
        ULONG  si;
        BOOLEAN faultCovered = FALSE;

        if (fw->SharedIndex < fw->SectionCount
            && fw->Sections[fw->SharedIndex].BackingVa != NULL) {
            sh0 = *(volatile ULONG*)fw->Sections[fw->SharedIndex].BackingVa;
        }

        WINMALI_WARN(
            "FW: MCU boot failed MCU_STATUS=%lu MCU_CONTROL=0x%08x "
            "JOB raw=0x%08x stat=0x%08x mask=0x%08x "
            "AS0_FAULT=0x%08x (exc=0x%02x %s, access=%s) "
            "AS0_ADDR=0x%08x%08x GPU_FAULT=0x%08x GPU_ADDR=0x%08x%08x shared[0]=0x%08x",
            mcu,
            mctl,
            jraw,
            jstat,
            jmask,
            as0fst,
            (ULONG)(as0fst & 0xffu),
            WinMaliFwDecodeMmuExc_(as0fst),
            WinMaliFwDecodeAccessType_(as0fst),
            as0fhi,
            as0flo,
            gpufst,
            gpfhi,
            gpuflo,
            sh0);

        WINMALI_WARN(
            "FW: dumping %lu sections (PtUsed=%lu/%lu KiB) — fault VA=0x%llx",
            fw->SectionCount,
            (ULONG)(fw->PtUsed / 1024u),
            (ULONG)(fw->PtPoolBytes / 1024u),
            (ULONGLONG)faultVa);
        for (si = 0; si < fw->SectionCount; ++si) {
            WINMALI_FW_SECTION* s = &fw->Sections[si];
            BOOLEAN covers        = (faultVa >= s->VaStart && faultVa < (UINT64)s->VaEnd);
            if (covers) {
                faultCovered = TRUE;
            }
            WINMALI_WARN(
                "  sect[%lu] VA=[0x%08x..0x%08x) flags=0x%08x backing_pa=0x%llx %s",
                si,
                s->VaStart,
                s->VaEnd,
                s->Flags,
                (ULONGLONG)s->BackingPa.QuadPart,
                covers ? "<-- COVERS FAULT" : "");
        }
        if (!faultCovered) {
            WINMALI_WARN(
                "FW: fault VA 0x%llx is NOT covered by any loaded section — FW expects an "
                "unmapped region (likely a CONFIG/TRACE_BUFFER entry, or PROT section we skipped)",
                (ULONGLONG)faultVa);
        } else {
            // Re-walk the PT for the fault VA to see exactly where translation died.
            UINT64  pa = 0;
            PUINT64 t = fw->RootHostVa;
            ULONG   level;
            BOOLEAN walkOk = TRUE;
            for (level = 0; level < 3u && walkOk; ++level) {
                ULONG  shift = 39u - (9u * level);
                ULONG  idx   = (ULONG)((faultVa >> shift) & 0x1ffull);
                UINT64 ent   = (t != NULL) ? t[idx] : 0ull;
                if ((ent & 3ull) != 3ull || (ent & 0x0000fffffffff000ull) == 0ull) {
                    WINMALI_WARN(
                        "  PT walk: L%lu[%lu] = 0x%016llx (invalid table) -- translation dies here",
                        level,
                        idx,
                        (ULONGLONG)ent);
                    walkOk = FALSE;
                    break;
                }
                pa = ent & 0x0000fffffffff000ull;
                t  = (PUINT64)((PUCHAR)fw->PtPoolVa + (SIZE_T)(pa - fw->PtPoolPa.QuadPart));
            }
            if (walkOk) {
                ULONG  i3       = (ULONG)((faultVa >> 12) & 0x1ffull);
                UINT64 page     = t[i3];
                ULONG  valid    = (ULONG)((page & 3ull) == 3ull);
                ULONG  attrIdx  = (ULONG)((page >> 2) & 7ull);
                ULONG  ap       = (ULONG)((page >> 6) & 3ull);
                ULONG  sh       = (ULONG)((page >> 8) & 3ull);
                ULONG  af       = (ULONG)((page >> 10) & 1ull);
                ULONG  ng       = (ULONG)((page >> 11) & 1ull);
                ULONG  pxn      = (ULONG)((page >> 53) & 1ull);
                ULONG  uxn      = (ULONG)((page >> 54) & 1ull);
                const CHAR* apS = (ap == 0) ? "RW-EL1"
                                : (ap == 1) ? "RW-EL0+1"
                                : (ap == 2) ? "RO-EL1"
                                            : "RO-EL0+1";
                WINMALI_WARN(
                    "  PT walk: L3[%lu] = 0x%016llx valid=%lu AP=%lu (%s) "
                    "AttrIdx=%lu SH=%lu AF=%lu nG=%lu PXN=%lu UXN=%lu",
                    i3,
                    (ULONGLONG)page,
                    valid,
                    ap, apS,
                    attrIdx,
                    sh,
                    af,
                    ng,
                    pxn,
                    uxn);
            }
        }

        (VOID)WinMaliMmuAsDisable(Adapter, WINMALI_MMU_MCU_AS);
        Adapter->FwMcuAsBound = FALSE;
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return STATUS_IO_TIMEOUT;
    }

    status = WinMaliFwPollSharedFwVersion_(fw, 2000u);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN(
            "FW: shared iface version still 0 after boot (MCU_STATUS=%lu JOB_RAW=0x%08x JOB_STAT=0x%08x)",
            WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_MCU_STATUS),
            WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_JOB_INT_RAWSTAT),
            WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_JOB_INT_STAT));
        (VOID)WinMaliMmuAsDisable(Adapter, WINMALI_MMU_MCU_AS);
        Adapter->FwMcuAsBound = FALSE;
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }

    status = WinMaliFwSetupGlbIface_(fw);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("FW: global iface setup failed 0x%08x", status);
        (VOID)WinMaliMmuAsDisable(Adapter, WINMALI_MMU_MCU_AS);
        Adapter->FwMcuAsBound = FALSE;
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }

    status = WinMaliFwInitCsfIfaces_(fw);
    if (!NT_SUCCESS(status)) {
        WINMALI_WARN("FW: CSF slot iface init failed 0x%08x", status);
        (VOID)WinMaliMmuAsDisable(Adapter, WINMALI_MMU_MCU_AS);
        Adapter->FwMcuAsBound = FALSE;
        WinMaliFwReleaseResources_(fw);
        (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);
        ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
        return status;
    }

    WinMaliFwInitGlobalIface_(Adapter, fw);

    if (Adapter->GpuMmuAsBound) {
        NTSTATUS csfSt = WinMaliCsfBootstrap_(Adapter, fw);
        if (!NT_SUCCESS(csfSt)) {
            WINMALI_WARN("FW: CSF kernel-queue bootstrap failed 0x%08x (MCU still alive)", csfSt);
            WinMaliCsfTeardown_(Adapter, fw);
        } else {
            Adapter->AdapterFlags |= WINMALI_ADAPTER_FLAG_CSF_JOBS;
        }
    } else {
        WINMALI_WARN("FW: skipping CSF bootstrap (GPU MMU bring-up AS not bound)");
    }

    Adapter->FwCtx = fw;
    Adapter->AdapterFlags |= WINMALI_ADAPTER_FLAG_MCU_ALIVE;

    /* Re-arm proper IRQ masks now that MCU is alive and the ISR can ack
       things sanely. We enable the fault/event bits we know how to ack;
       the ISR clears every block's RAWSTAT each time so even bits we
       don't expect won't pile up. Mask values mirror panthor's defaults:
         GPU_IRQ_MASK   - fault bits + reset-completed + power-events
         JOB_INT_MASK   - GLOBAL_IF (bit 31) for MCU events
         MMU_INT_MASK   - 0xFFFF for the 16 AS-level fault bits
       Anything we haven't characterised stays masked. */
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_GPU_IRQ_MASK, 0x000000FFu);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_JOB_INT_MASK, WINMALI_JOB_INT_GLOBAL_IF);
    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_MMU_INT_MASK, 0x0000FFFFu);
    WINMALI_TRACE("IRQ masks armed: GPU=0x000000FF JOB=0x80000000 MMU=0x0000FFFF");

    /* One-shot NOP round-trip self-test. Bootstrap above only proves the
       queue/iface SETUP; this proves the MCU actually executes a kernel-
       queue CALL and advances the seqno. If it can't, DEMOTE to degraded
       mode now (clear CSF_JOBS) so DxgkDdiSubmitCommand's NOP attempt
       early-outs instantly instead of eating its wait-timeout on every
       paging submit - dxgk serializes those, so per-submit waits stack
       up into whole-UI freezes (seen live 2026-07-10: Start menu froze
       the system while every submit sat in the NOP wait). */
    if ((Adapter->AdapterFlags & WINMALI_ADAPTER_FLAG_CSF_JOBS) != 0) {
        NTSTATUS nopSt = WinMaliCsfSubmitNopJob(Adapter, 1000u);
        if (NT_SUCCESS(nopSt)) {
            WINMALI_TRACE("FW: CSF NOP self-test PASS (seqno round-trip live)");
        } else {
            Adapter->AdapterFlags &= (ULONG)~WINMALI_ADAPTER_FLAG_CSF_JOBS;
            WINMALI_WARN("FW: CSF NOP self-test FAILED 0x%08x - demoting to "
                         "degraded submits (fence completes CPU-side)", nopSt);
        }
    }

    //
    // Single deterministic post-init line so we always know exactly which
    // capabilities went live, regardless of which sub-step logged what.
    //
    {
        ULONG flags    = Adapter->AdapterFlags;
        BOOLEAN mcuOk  = (flags & WINMALI_ADAPTER_FLAG_MCU_ALIVE) != 0;
        BOOLEAN jobsOk = (flags & WINMALI_ADAPTER_FLAG_CSF_JOBS) != 0;

        WINMALI_TRACE(
            "FW: MCU up, iface v%u.%u.%u sections=%lu shared_idx=%lu "
            "AdapterFlags=0x%08x [%s%s%s] csf_boot=%s gpu_mmu_as=%s",
            (fw->GlbControl->Version >> 24) & 0xffu,
            (fw->GlbControl->Version >> 16) & 0xffu,
            fw->GlbControl->Version & 0xffffu,
            fw->SectionCount,
            (ULONG)fw->SharedIndex,
            flags,
            mcuOk  ? "MCU_ALIVE "  : "",
            jobsOk ? "CSF_JOBS "   : "",
            (!mcuOk && !jobsOk) ? "(none) " : "",
            fw->CsfBootValid       ? "yes" : "no",
            Adapter->GpuMmuAsBound ? "bound" : "unbound");
    }

    return STATUS_SUCCESS;
}

VOID
WinMaliFwTeardown(_Inout_ PWINMALI_ADAPTER Adapter)
{
    WINMALI_FWCTX* fw;

    if (Adapter == NULL) {
        return;
    }
    fw = (WINMALI_FWCTX*)Adapter->FwCtx;
    if (fw == NULL) {
        return;
    }

    WinMaliCsfTeardown_(Adapter, fw);

    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_MCU_CONTROL, WINMALI_MCU_CONTROL_DISABLE);
    {
        ULONG s;
        for (s = 0; s < 10000u; ++s) {
            if (WinMaliHwRead32(&Adapter->Hw, WINMALI_REG_MCU_STATUS) == WINMALI_MCU_STATUS_DISABLED) {
                break;
            }
            KeStallExecutionProcessor(10);
        }
    }

    WinMaliHwWrite32(&Adapter->Hw, WINMALI_REG_JOB_INT_MASK, 0);

    if (Adapter->FwMcuAsBound) {
        (VOID)WinMaliMmuAsDisable(Adapter, WINMALI_MMU_MCU_AS);
        Adapter->FwMcuAsBound = FALSE;
    }

    (VOID)WinMaliFwGpuL2PowerOff_(Adapter, fw->L2Mask);

    Adapter->FwCtx = NULL;
    Adapter->AdapterFlags &= (ULONG)~WINMALI_ADAPTER_FLAG_MCU_ALIVE;

    WinMaliFwReleaseResources_(fw);
    ExFreePoolWithTag(fw, WINMALI_POOL_TAG);
}

/* ---------------------------------------------------------------------------
   AS-slot scheduling serialization.

   ⛔ THE ORIGINAL EVICTION CODE ASSERTED AN INVARIANT IT DID NOT ENFORCE. It
   claimed "the driver binds a single CSG slot to one AS at a time, so no other
   VM can have work in flight", and evicted on that basis. But EnsureAs ran
   OUTSIDE fw->SubmitLock, so a second thread could be anywhere in a submit
   while the first reprogrammed its address space - and DestroyDevice could free
   the victim VM entirely while the evictor still held a raw pointer to it.
   An audit of the eviction path found six critical defects, all rooted in that
   one unenforced sentence.

   The invariant is now real: AS-slot ownership changes and submissions take the
   SAME lock. fw->SubmitLock is a KMUTEX and KMUTEX acquisition is recursive per
   thread, so a caller may hold it across EnsureAs and then call
   WinMaliCsfSubmitGroupStream, which takes it again - that nests correctly and
   is why no separate lock is introduced here.

   ★ A claimed invariant is worth exactly as much as the lock that enforces it.
   --------------------------------------------------------------------------- */

NTSTATUS
WinMaliCsfAcquireSchedLock(_Inout_ PWINMALI_ADAPTER Adapter)
{
    if (Adapter == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* ⛔ A REAL DEADLINE, NOT NULL. WinMaliWaitObjectAbortable takes the slice
       length as its third argument, NOT a timeout - passing NULL for Timeout
       means wait FOREVER in 1s slices. An unbounded wait here would park the
       dxgk scheduler thread exactly the way the present worker's infinite wait
       once froze the whole desktop. 2s is far longer than any legitimate
       submit (those bound themselves at 500ms) and short enough that a wedged
       GPU cannot hold the system. */
    {
        LARGE_INTEGER deadline;
        deadline.QuadPart = -20000000LL;    /* 2000 ms, relative */
        return WinMaliWaitObjectAbortable(&Adapter->AsSchedLock, &deadline, 100);
    }
}

VOID
WinMaliCsfReleaseSchedLock(_Inout_ PWINMALI_ADAPTER Adapter)
{
    if (Adapter == NULL) {
        return;
    }
    (VOID)KeReleaseMutex(&Adapter->AsSchedLock, FALSE);
}

/* The AS the MCU's CSG is currently bound to, or WINMALI_AS_SLOT_MAX if none.
   Never evict this one: the firmware may still page-walk through it even with
   no submit of ours outstanding. */
ULONG
WinMaliCsfCurrentAs(_In_ PWINMALI_ADAPTER Adapter)
{
    WINMALI_FWCTX *fw;
    if (Adapter == NULL || Adapter->FwCtx == NULL) {
        return WINMALI_AS_SLOT_MAX;
    }
    fw = (WINMALI_FWCTX*)Adapter->FwCtx;
    return fw->CsgStarted ? fw->CurrentCsgAs : WINMALI_AS_SLOT_MAX;
}

/*
 * WinMaliSerial.c - log sink for MT6897 / Lenovo TB375FC.
 *
 * ORIGINAL (RK3588): this file drove UART2 @ 0xFEB50000, a Synopsys DW-APB
 * 8250. That base is RK3588-specific. On MT6897 0xFEB50000 is not a UART and
 * not even a mapped peripheral window -- writing there at DIRQL is a
 * synchronous external abort, i.e. a bugcheck inside DriverEntry, which is the
 * single worst failure mode available to us (it loops before the driver can be
 * uninstalled). The port therefore replaces the backend entirely rather than
 * retargeting a base address.
 *
 * REPLACEMENT: append into the EDK2 boot-log ring that our own firmware
 * already publishes. This is strictly safer than any UART -- it is plain
 * cached DRAM stores with no polling, so it cannot spin, cannot wedge, and
 * cannot fault on a gated clock -- and it lands the driver's output in the
 * SAME buffer we already read firmware logs from over WinRM. That is the only
 * reason this driver is observable at all: there is no kernel debugger
 * attached and DbgPrint otherwise goes nowhere.
 *
 * Ring layout (must match FrameBufferSerialPortLib.c:313-323 exactly):
 *   +0x00  magic 'MLOG' = 0x474F4C4D
 *   +0x04  capacity
 *   +0x08  write cursor
 *   +0x0C  wrapped flag
 *   +0x20  payload, capacity = 0x40000 - 0x20
 *
 * Reserved by PlatformMemoryMapLib.c as "Boot Log Ring" at 0x48300000, 256 KB,
 * WRITE_BACK. We map it CACHED (PAGE_READWRITE, no PAGE_NOCACHE) to match that
 * attribute -- mtkcpufreq does the same. A NOCACHE alias of a WRITE_BACK region
 * is a mismatched-attribute mapping on ARM64 and is architecturally undefined.
 *
 * Read it from Windows with:
 *   (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\mtkcpufreq).BootLog
 */

#define WINMALI_LOG_NO_REDIRECT   /* this file talks to the REAL DbgPrint */
#include "WinMaliLog.h"

#include <ntstrsafe.h>

#define WINMALI_RING_PHYS   0x48300000ULL
#define WINMALI_RING_BYTES  0x40000u
#define WINMALI_RING_HDR    0x20u
#define WINMALI_RING_MAGIC  0x474F4C4Du            /* 'MLOG' */
#define WINMALI_RING_CAP    (WINMALI_RING_BYTES - WINMALI_RING_HDR)

static volatile ULONG* g_Ring;      /* header base; NULL => sink disabled */
static LONG            g_RingLock;

NTSTATUS
WinMaliSerialInit(VOID)
{
#if !WINMALI_LOG_TO_UART
    /* WinDbg/kd mode: don't map anything; WinMaliSerialWrite no-ops. */
    return STATUS_SUCCESS;
#else
    PHYSICAL_ADDRESS pa;

    if (g_Ring != NULL) {
        return STATUS_SUCCESS;
    }

    pa.QuadPart = (LONGLONG)WINMALI_RING_PHYS;

    /* CACHED on purpose -- see header comment. */
    g_Ring = (volatile ULONG*)MmMapIoSpaceEx(pa, WINMALI_RING_BYTES, PAGE_READWRITE);
    if (g_Ring == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * Do NOT initialise the header here. Firmware is the first writer and has
     * already laid it out this boot; re-initialising would discard the entire
     * EDK2 boot log we depend on for diagnosing the firmware half. If the magic
     * is absent something is wrong with our assumptions about this region --
     * refuse to write rather than clobber 256 KB of unknown memory.
     */
    if (g_Ring[0] != WINMALI_RING_MAGIC) {
        DbgPrint("[WinMali] boot-log ring magic %08x != MLOG - logging disabled\n",
                 g_Ring[0]);
        MmUnmapIoSpace((PVOID)g_Ring, WINMALI_RING_BYTES);
        g_Ring = NULL;
        return STATUS_SUCCESS;      /* not fatal: we simply have no log sink */
    }

    return STATUS_SUCCESS;
#endif
}

VOID
WinMaliSerialTeardown(VOID)
{
    if (g_Ring != NULL) {
        MmUnmapIoSpace((PVOID)g_Ring, WINMALI_RING_BYTES);
        g_Ring = NULL;
    }
}

VOID
WinMaliSerialWrite(_In_z_ const char* Text)
{
    volatile UCHAR* pay;
    const char*     p;
    ULONG           cur;

    if (g_Ring == NULL || Text == NULL) {
        return;
    }

    /*
     * Interlocked flag, not a spinlock: DIRQL callers (ISR traces) are legal
     * here and normal spinlocks are not. BOUNDED acquisition -- if the holder
     * is a preempted PASSIVE thread and we are spinning at DISPATCH/DIRQL on
     * the same CPU it can never release, so an unbounded spin is a livelock.
     * Losing a log line beats hanging. (Unchanged from the original.)
     */
    {
        ULONG tries;
        for (tries = 0; ; ++tries) {
            if (InterlockedCompareExchange(&g_RingLock, 1, 0) == 0) {
                break;
            }
            if (tries >= 500000u) {
                return;             /* drop the line */
            }
            YieldProcessor();
        }
    }

    pay = (volatile UCHAR*)g_Ring + WINMALI_RING_HDR;
    cur = g_Ring[2];
    if (cur >= WINMALI_RING_CAP) {
        cur = 0;
    }

    for (p = Text; *p != '\0'; ++p) {
        pay[cur] = (UCHAR)*p;
        if (++cur >= WINMALI_RING_CAP) {
            cur       = 0;
            g_Ring[3] = 1;          /* wrapped */
        }
    }

    g_Ring[2] = cur;

    InterlockedExchange(&g_RingLock, 0);
}

ULONG
WinMaliLogPrint(_In_z_ _Printf_format_string_ const char* Format, ...)
{
    char    buf[512];
    va_list ap;

    va_start(ap, Format);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(buf, sizeof(buf), Format, ap))) {
        buf[sizeof(buf) - 1] = '\0';   /* truncated is fine */
    }
    va_end(ap);

    DbgPrint("%s", buf);      /* real DbgPrint - kd path when attached */
    WinMaliSerialWrite(buf);  /* boot-log ring - the path we actually read */
    return 0;
}

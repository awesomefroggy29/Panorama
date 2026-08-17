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

/*
 * ★★★★★ READ OUR OWN TRACE BACK. THE WHOLE DRIVER LOG HAS BEEN UNREADABLE.
 *
 * Every DbgPrint in this driver is redirected here (WinMaliLog.h) and lands in
 * the EDK2 ring at 0x48300000. The ring is LIVE, but the only reader we had was
 * mtkcpufreq's `BootLog` registry value - and that is a ONE-SHOT snapshot taken
 * when mtkcpufreq loaded, so it contains only the handful of lines we had
 * emitted by then (measured: 25 WinMali lines, ending mid-QAI) and never
 * updates. mtkcpufreq also cannot be restarted ("cannot be stopped"), so there
 * is no way to re-take it.
 *
 * Consequence, and it shaped a whole session: with the trace invisible, every
 * value we wanted had to be re-published as a registry DWORD counter - to the
 * point of adding QuerySegFlags to read a number the driver was ALREADY
 * printing in QAI[QUERYSEGMENT3 fill]. This function ends that.
 *
 * ⚠ DELIBERATELY LOCK-FREE. Writers can be at DIRQL and take the interlocked
 * flag; holding it across a 32 KB copy from a PASSIVE thread would make them
 * spin 500k times and DROP LINES - losing exactly the trace we came for. A torn
 * byte at the boundary is a cosmetic defect in a log; a dropped line is missing
 * evidence.
 */
ULONG
WinMaliSerialSnapshot(_Out_writes_bytes_(BufBytes) PUCHAR Buf,
                      _In_ ULONG BufBytes)
{
    volatile const UCHAR* pay;
    ULONG cur, wrapped, avail, take, start, i;

    if (g_Ring == NULL || Buf == NULL || BufBytes == 0) {
        return 0;
    }
    if (g_Ring[0] != WINMALI_RING_MAGIC) {
        return 0;                      /* not our ring - never guess */
    }

    pay     = (volatile const UCHAR*)g_Ring + WINMALI_RING_HDR;
    cur     = g_Ring[2];
    wrapped = g_Ring[3];
    if (cur >= WINMALI_RING_CAP) {
        cur = 0;
    }

    avail = wrapped ? WINMALI_RING_CAP : cur;
    take  = (BufBytes < avail) ? BufBytes : avail;
    if (take == 0) {
        return 0;
    }

    /* Newest data ends AT the cursor, so walk back `take` bytes from it. */
    start = (cur + WINMALI_RING_CAP - take) % WINMALI_RING_CAP;
    for (i = 0; i < take; ++i) {
        Buf[i] = pay[(start + i) % WINMALI_RING_CAP];
    }
    return take;
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

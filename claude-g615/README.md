# claude-g615 — WinMali on Mali-G615 (Valhall, MT6897)

Windows-on-ARM WDDM display+render driver for Mali, running on a **Lenovo TB375FC**
(MediaTek Dimensity 8300 / MT6897, **Mali-G615 MC5**), booted via a custom EDK2.

This folder is a read-only-ish scratchpad: the source as it stands today, so the
codex-g610 side (and its LLM) can diff against it. It is not a buildable tree on
its own — see *What is NOT here*.

---

## ⚠ G610 vs G615 — read this first, it will bite you

**G615 is Valhall arch 11. G610 is arch 10.** Panfrost/Mesa has `v10` and `v12`
codepaths and **skips 11 entirely**, and it fails *silently* — no error, just
wrong or absent behaviour. Our tree brackets it: where arch 11 is missing we
clone the v10 path and diff against v12 to decide each field.

So a fix that is correct on your G610 may be a no-op or actively wrong here, and
vice versa. When comparing code, check the arch gate before assuming divergence
is a bug.

Authoritative sources we use for GPU ID decode / CSF iface / MMU:
- ARM DDK source `gpu_mali/mali_avalon/mali-r49p1` (not for shader descriptors —
  those live in the blob).
- The stock vendor `mali_kbase_mt6897_r44.ko`, which is **unstripped** — exception
  tables and register offsets were decoded out of its jump tables rather than
  guessed. ⚠ `.rodata` tail-merges strings, so a `strings` grep is a hypothesis,
  not the table.

---

## Layout

```
driver/            the WDDM kernel-mode driver (our code)
  KMD/             miniport: dxgk DDIs, MMU/VM, BO table, CSF firmware, escapes
  Shared/          the UMD<->KMD escape ABI (WinMaliEscape.h) — the contract
  UMD/, Mesa/      thin UMD shim + Mesa project glue
  Tools/           gen-dxgk-stubs.py  ⛔ DO NOT RUN — see below
mesa/
  kmod-winmali/    our pan_kmod backend (the Mesa side of the escape ABI)
  winmali-mesa-workingtree.patch   everything else we changed in Mesa
```

`mesa/winmali-mesa-workingtree.patch` is `git diff` of an uncommitted working
tree on top of **`DarkFire01/mesa`** (gitlab.freedesktop.org), commit `0f7e82d`
("WIP") — a WoA Mesa fork, not upstream. It touches ~40 files across
`gallium/drivers/panfrost`, `gallium/frontends/d3d10umd`, and `panfrost/lib`.

⛔ **Never run `Tools/gen-dxgk-stubs.py`.** The dxgk stubs are hand-maintained now;
regenerating them overwrites real implementations with stubs.

---

## How it actually works right now

Two things are worth understanding before reading the code, because neither is
the obvious design:

**1. Submission goes through a private escape, not the dxgk-native path.**
There are two gates, `WINMALI_DXGK_ADDR` and `WINMALI_DXGK_SUBMIT`, and on this
device both are **off**: allocation/addressing/submission all ride our own
`DxgkDdiEscape` ABI. Allocation + addressing + submission + **fences** are one
design — mixing halves is the single most productive source of bugs in this
project (see *Root causes* below).

⚠ Gate state is a **registry/env fact, not a source fact**. The `[default]`
comment in the source describes the build, not the machine you are looking at.

**2. The scanout source is a user VA the KMD wraps.**
Escape `0x93 BoFromUserVa` takes the UMD's locked CPU VA (from `pfnLockCb`),
`MmProbeAndLockPages` it, and the present worker scans out of that same
probe-locked MDL. This is how a full desktop reaches the panel. It exists
because under escape-submit dxgk never aperture-maps, so `ApertureMdl` is NULL
forever and every aperture-based read path (escapes 0x81 / 0x92) is dead.

`DxgkDdiPresent` is **never called** — DWM uses our UMD to *render*, not to
present, and presents via FLIP. Don't tune the present path; it's exonerated.

---

## Root causes found (both verified on hardware, 2026-08-16)

**A. The CS ring `Insert` pointer was written back masked.**
`Insert`/`Extract` are continuous 64-bit counters, not ring offsets. The mask
belongs on the *copy* (where bytes land in the 64 KiB buffer), never on the
value written back. Once the firmware's `Extract` passed `RING_BYTES`,
`Insert & 0xFFFF` == 0 and we wrote `Insert = 0x80` while `Extract = 0x10000`,
putting Insert permanently 65,408 bytes *behind* Extract. The FW then sees no
work and every submit times out.

Measured before the fix:
```
RingInsert 0x00000080   RingExtract 0x00010000
seqno      0x00000200 (512)   and 512 * 128 B = 0x10000   <- they agree
CMD_PTR 0  BLOCKED 0  CS_FAULT 0  FATAL_INFO 0
```
The GPU had executed **512 trampolines correctly** and then stopped forever.
After the fix, with real compositor traffic: `CsSubmits 906->961`,
`CsOk 906->960`, `CsTimeouts 0`, `Insert 0x1E080 > Extract 0x1E000`.

If your G610 tree has a submit path that "works for a few seconds then goes
black", check this first — 512 jobs is a few seconds of compositing.

**B. Fences were not gated, so every submit was rejected before reaching the GPU.**
Mesa created **dxgk monitored fences** (the D3D frontend installs the dxgk data
plane unconditionally, "no env gates") while submitting through the **escape**
path, whose sync ops resolve against the KMD's own syncobj table. Result:
`ENOENT` on every submit carrying a wait op. The tell was unmistakable once
logged: `SyncBadHandle = 0x40000800` — a D3DKMT-shaped handle, when KMD syncobj
handles are small integers from a counter starting at 1.

Fix: gate the fence hooks on the same switch as submission
(`winmali_use_dxgk_fences()` in `mesa/kmod-winmali/winmali_kmod.c`).

---

## Traps that cost us days — worth checking in your tree

- **`CS_FATAL` is latched and never acked.** A non-zero value is not evidence of
  a *current* fault. `CS_FATAL = 0x40` decoded to `CS_CONFIG_FAULT` and sent us
  hunting a CS misconfiguration; it was a stale latch from bring-up.
  `FATAL_INFO = 0` was the tell. Only the first fatal of a boot means anything.
- **An all-zero status block is indistinguishable from an unwritten page.**
  `CMD_PTR=0, BLOCKED=0, FAULT=0` reads as "misconfigured CS" and equally as
  "nothing ever wrote here". The **ring pointers settle it without the firmware's
  cooperation**: we write `Insert`, the FW writes `Extract`.
- **A counter published only in the failure branch cannot show success.** Our
  ring/seqno values lived in the timeout branch, so a *working* driver published
  all zeros — identical to "not instrumented". Pair every failure counter with a
  completion counter (`CsOk` vs `CsTimeouts`).
- **Handle tables minted from a per-adapter counter reset to 1** on every adapter
  start, while DWM/explorer survive the restart still holding old handles. Worse
  than failing: a restarted counter can re-issue a handle that is still live and
  silently alias someone else's object. All three tables (BO, group, syncobj) are
  now boot-unique.
- **Shader/tiler power is NOT our problem here** — measured
  `SHADER_READY == SHADER_PRESENT (0x130012)`, `L2_READY 1`, `TILER_READY 1`
  across a real draw. If you are chasing "cores never came up", measure it before
  believing it.
- **An idle desktop submits nothing.** Any "the bug is fixed" reading taken on an
  idle desktop is a dead window that looks exactly like success.

---

## Open threads (as of this upload)

1. Mesa still logs ~8 `GROUP_SUBMIT failed ret=-2 (ENOENT)` per 2 min while the
   KMD's counters (`StatSubmitSyncFail`, `StatSubmitNoGroup`, `CsfFailCount`) all
   read 0. Contradictory counters at the two ends of one interface — the bug is
   in the code *between* them, on a path with no counter yet.
2. The scanned present source is full of content (`StatSrcNonBlack 646/646`,
   `StatSrcEdges 639`) but its change counter is flat — rendering completes, yet
   that surface isn't being updated.
3. Taskbar and mouse cursor are not composited.
4. `texture_map` refuses KMT-imported destinations by design, which kills the CPU
   delivery fallback.

## What is NOT here

- No binaries, no firmware blobs, no certificates, no build output.
- No Mesa tree — only our patch and our kmod backend.
- The upstream base for the patch is recorded above; you need that fork to apply it.
- Build uses the EWDK (kit 10.0.26100.0). `Tools/gen-dxgk-stubs.py` hardcodes a
  local `F:\Program Files\Windows Kits\10\Include` path — that is the mounted EWDK
  ISO, not a machine secret.

## Licensing note

`driver/` is our own work. `mesa/` is derived from Mesa (MIT) via the
DarkFire01 fork. Exception-code tables in `KMD/WinMaliFw.c` were *decoded from*
(not copied out of) the vendor kbase module. No ARM DDK source is vendored here.

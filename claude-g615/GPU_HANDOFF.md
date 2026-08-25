# WinMali / Mali-G615 on Windows-on-ARM — GPU Handoff

**Device:** Lenovo TB375FC tablet · MediaTek MT6897 (Dimensity 8300) · Mali-G615 MC5
**Driver:** `WinMaliKmd.sys` (WDDM miniport) + `WinMaliUmd.dll` (Mesa gallium `d3d10umd` frontend over panfrost)
**Version at time of writing:** 1.1.21.1
**Last updated:** 2026-08-25, late evening
**Goal:** fully working graphics, daily-driver ready.

This document is written so a fresh session can pick the work up without re-deriving
anything. It records what works, what is broken, **what has been refuted** (so nobody
re-chases it), and how to operate the hardware safely.

---

## 1. The two modes — this is the central concept

The driver ships one binary that runs in two completely different shapes, selected by
the `DodMode` registry knob:

| | `DodMode=1` — **display-only (DOD)** | `DodMode=0` — **render** |
|---|---|---|
| `EffDodMode` reads | `0x8001` | `0x8000` |
| Who composites the desktop | **WARP** (CPU, in dwm) | **the Mali GPU** |
| What our driver does | scans out dwm's finished framebuffer | provides a full D3D render path *and* scans out |
| Desktop quality | **perfect, user-confirmed** | broken — see §4 |
| Stability | good | hangs intermittently |

**DOD is the known-good fallback.** If the machine needs to be usable, boot DOD.

**Render mode is the goal**, because it is the only path where the GPU actually
accelerates anything.

### The render-mode unlock

`QaiWddmVer = 0x2000` (WDDM 2.0). This is the single most important discovery in the
project. `dxgkrnl` branches on the reported `DXGK_DRIVERCAPS.WDDMVersion`:

- `>= 0x2300` → dxgk takes a **demolition path** and tears the adapter down.
- `0x2000` → the demolition path is eliminated entirely.

⛔ Do **not** try `0x2200` — it is not on dxgk's whitelist and hangs at the LogonUI
background.

### `KnobFuse` — why your knob "didn't work"

`KnobFuse=1` is a one-shot safety fuse: after one boot, experimental knobs are burned
back to their baked `WINMALI_DEF_*` defaults. `WINMALI_DEF_DODMODE = 1`, so **render
mode lasts exactly one boot and must be re-armed every time**:

```
DodMode=0  QaiWddmVer=8192  PresentTimerHz=12
```

The fuse burns *to the constant*, not to a bare 0 — that distinction was itself a bug
fix (see §5).

---

## 2. Where things live

| What | Path |
|---|---|
| KMD source | `C:\woa_build\winmali\KMD\` |
| Shared headers | `C:\woa_build\winmali\Shared\` ⚠ **exists twice in the tree — edit both** |
| Mesa/panfrost + UMD frontend | `C:\woa_build\mesa-woa\src\` |
| winmali kmod backend | `.../src/panfrost/lib/kmod/winmali/winmali_kmod.c` |
| D3D10 UMD frontend | `.../src/gallium/frontends/d3d10umd/` |
| Staged package | `C:\woa_build\winmali_pkg\` |
| Knobs / counters (device) | `HKLM\SYSTEM\CurrentControlSet\Services\WinMaliKmd\Parameters` (~433 values) |

⚠ The device **class key** (`Control\Class\{4d36e968-...}\0000`) holds only ~14
INF-installed values. It is **not** where the knobs live. Reading it and concluding
"all the knobs were wiped" is a mistake that has been made.

---

## 3. Build → deploy chain

```
bk.cmd  (KMD)  /  bu.cmd  (UMD)   →   stage_winmali.ps1   →   deploy_clean_umd.ps1
```

**Version bumping — both, together, always.** `WinMaliVersion.h` **and** the vcxproj
`<Inf><TimeStamp>` must be bumped in lockstep. dxgkrnl bugchecks device start on any
`.sys`-vs-INF mismatch (`DpiFdoValidateKmdAndPnpVersionMatch`). And `pnputil`
**silently refuses an equal-or-older `DriverVer`** — "Added driver packages: 0", device
stays on the old one, and every subsequent measurement is about the wrong binary.

**Assertions that must pass before you believe a deploy:**

1. `MSBUILD_EXIT=0` — note `bk.cmd` does **not** propagate msbuild's exit code, so the
   shell's exit code is meaningless. Grep the log.
2. The `.sys` **SHA256 changed** and the new version string is present, the old absent.
3. The **INF `DriverVer`** in the staged package reads the new version.
4. The on-device binaries **hash-match** the staged package.
5. The DriverEntry banner in the trace ring names the new version (see §7).

⛔ The EWDK is an **ISO mount**; a host reboot loses `F:` and the build silently no-ops.
`bk.cmd` probes for the drive letter, but verify anyway. Kit: `10.0.26100.0`.

⛔ Installing a package does **not** rebind the device. In practice `pnputil /add-driver
/install` *has* been observed to restart the device live (`StatLoadCount` increments and
the trace ring shows a second DriverEntry) — but user-mode processes keep the **old UMD**
mapped until they restart, so a reboot is still needed for a UMD change to reach
dwm/explorer.

---

## 4. THE CURRENT FRONT — what is actually broken

Measured on 1.1.21.1, render mode, all validity gates passed.

### 4.1 Symptom A — window client areas are a solid, changing colour

A window's **client rect** renders as one saturated colour, different on every publish:
magenta → terracotta → teal → green. In the *same* crop, the **chrome is pixel-identical
frame to frame** — title bar, menu bar, the small app icon in full colour detail, window
border. This is the "flashing disco desktop".

A *uniform* fill over exactly the client rect means **the clear lands and the content
draws do not**.

### 4.2 Symptom B — the taskbar tray/clock is missing

Same driver, same 1:1 region, same code path:

- **DOD:** full taskbar — Start, Search + its text, all pinned icons, chevron, network,
  volume, clock, date.
- **Render:** pinned icons **perfect**; Start logo renders **white** instead of blue;
  the Search magnifier renders as a **white blob**; "Search" text, the clock, the date,
  the chevron and the network icons are **absent**.

Symptom B is very likely Symptom A at small scale, not a separate bug.

### 4.3 The strongest lead: it is SPATIAL, and the tiling/modifier path is in it

`PAN_MESA_DEBUG=linear` sets `PAN_DBG_LINEAR` (`pan_util.h` `0x1000`), honoured by
`panfrost_best_modifier()` (`pan_resource.c:1071`), forcing `DRM_FORMAT_MOD_LINEAR`.
Set it machine-wide at `HKLM\...\Session Manager\Environment` so dwm inherits it at boot.

**The artifact mutated — this was not a null result:**

- tiled: Start logo = a solid **white blob**.
- linear: Start logo = **torn into displaced pieces**, a blue square adrift to the left.

⇒ **The content exists and is being placed at the wrong offsets.**

**Working hypothesis (unproven).** `PAN_DBG_LINEAR` only applies where *we* choose the
modifier (`modifier == DRM_FORMAT_MOD_INVALID`). An **imported/shared** surface keeps its
own. So forcing linear creates a **writer/reader modifier mismatch** where both sides
previously agreed on tiled — which is why it got *worse*, and why agreement-on-tiled
yields a flat colour instead.

**⇒ Next step: find where a shared surface's modifier fails to propagate across the
open/import path.** Do not force another global flag — measure why the two sides
disagree. (Forcing successive gates is exactly how the coherency chase wasted a session;
see §6.)

⛔ `PAN_MESA_DEBUG` persists in the machine environment across reboots. `KnobFuse` does
**not** cover it. Clear it explicitly.

### 4.4 The GPU itself is exonerated

```
CsSubmits = 14111    CsOk = 14111
GpuFaultCount, CsFault, CsFatal, CsTimeouts,
SubmitSyncErr, SubmitSyncFail, SyncWaitTmo, CsfFailCount   ALL ZERO
StatTilerOom = 3     StatTilerOomFail = 0   (serviced correctly)
```

Every submit completes. The hardware draws exactly what it is asked to draw. **What it
is asked is wrong.** Do not go hunting GPU faults.

### 4.5 THE UMD LOG IS THE RIGHT INSTRUMENT — AND IT NARROWS THIS SHARPLY

`MESA_LOG_FILE = C:\umdlog\wm` is already set machine-wide on the tablet, and
`C:\umdlog` holds ~506 per-process logs (`wm.<proc>.<pid>.log`). **This is the proven
UMD channel** — `mesa_loge` goes to `OutputDebugStringA` *and* this file.

⛔ `mesa_loge` output does **NOT** reach the KMD's EDK2 trace ring. Grepping
`TraceTail`/`TracePrevBoot` for UMD strings returns zero because the channel is not
live there, not because nothing fired. (Fifth instance of the calibration trap.)

**Measured from `wm.dwm.exe.<pid>.log` on a render-mode boot:**

- **Every `OpenResource` SUCCEEDS**, with correct strides — no `WSI pitch too small`,
  no `WSI pitch not properly aligned`, no `KMT import FAILED`. dwm opens, among others:
  - `fmt=87` (BGRA8): `2944x1840 stride=11776` (primary), `2976x1856 stride=11904`,
    `2944x144 stride=11776` (**the taskbar strip**), `1024x1024 stride=4096` (atlases)
  - `fmt=65` (**A8 — the glyph atlases**): `704x704 stride=704`, `1984x1248 stride=1984`,
    `1984x1344 stride=1984`, `32x32 stride=64`
  ⇒ The import path is **clean**, and the A8 glyph atlases *are* reaching dwm.
- **dwm really is compositing:** `WMCSOFF` shows a repeating command-stream pattern of
  `draws=1`, `draws=2`, `draws=59`. It is not a no-draw case.
- **`CCR CENSUS: calls=17857 toPrimary(dstBO>=16MB)=17755`** — ~99% of
  `ResourceCopyRegion` calls target the 21,667,840-byte primary.
- **`WMDIRTY`**: `_Present1 calls=8897 withRects=8877 maxRects=1 surfaces=1 dst=0x0`.
- **`WMGAPMAX`**: max render-to-render gap **1375 ms**.

⛔ `WMPRESPATH redirected-copy=0 kernel-present=8897` is **NOT a fault** — the
redirected branch only runs when `hDstResource` is non-zero, and dwm always passes 0,
which the source already records as measured. Do not "fix" this.

#### The hypothesis this points at — NOT yet proven

Two routes carry pixels to the primary, and they appear to differ in correctness:

| Route | Mechanism | Observed |
|---|---|---|
| `ResourceCopyRegion` → primary | a **copy** | wallpaper, desktop icons, labels — **correct** |
| dwm draw sampling an **imported** shared surface | a **texture fetch** | client areas, tray glyphs — **constant colour** |

⇒ Working theory: **sampling an imported shared surface returns a constant**, while
copying the same memory is fine. That fits every observation at once — client rects
solid, A8 glyph content missing or blown out, and dwm's own chrome and icons correct
because dwm rasterises those itself rather than sampling an import.

**⇒ The thing to inspect is the texture/sampler-view descriptor built for an imported
BO** — its base address, row stride and format — not the import call, which is proven
to succeed.

⚠ Status: the routing split is **measured**; the "sampling returns a constant" step is
**inferred**. Prove it before building on it. The `.5 kmt=` / `WMRTID` / `bo_from_allocation`
lines in the same log give the addresses to check against.

✅ The 0-byte client logs on the hung boot are **not** evidence that clients skipped the
GPU. Calibrated: across ~506 logs, **every** client normally logs — Explorer 32/32
non-empty (largest 191 KB), WindowsTerminal 34/34 (331 KB), StartMenuExperienceHost
33/33. The empty files are stdio buffering lost when the box hard-hung.

### 4.6 THE TRANSPORT IS AN ATLAS, AND THE A8 ONE IS WHERE IT BREAKS

Comparing `Explorer` and `dwm` on the same render-mode boot separates the two roles
cleanly.

**Explorer never presents a window-sized surface.** Every present source it has:

| count | present source | bytes/px |
|---|---|---|
| 36× | `704x704 stride=704` | **1 → A8** |
| 3× | `1024x1024 stride=4096` | 4 → BGRA |
| 1× | `1024x1024 stride=4096` | 4 → BGRA |

**dwm presents exactly one thing, 279×:** `2944x1840 stride=11776` — the full screen.

⇒ Explorer's taskbar/tray content reaches dwm **as atlases**, not as a window surface:
a **704×704 A8 alpha/glyph atlas** plus 1024×1024 BGRA colour atlases. dwm then *opens*
those very surfaces (`kmt=0x40001080 704x704 stride=704 fmt=65` appears in dwm's
`OpenResource` list) and samples them in its `draws=59` composition stream.

**This lines the symptom up exactly.** Everything sourced from a **BGRA** atlas renders
correctly — pinned icons, wallpaper, window chrome. Everything sourced from the **A8**
atlas is wrong in the specific way an alpha mask goes wrong when its coverage reads as
1.0 or 0.0: the Start logo is **solid white**, the search magnifier is a **solid white
blob**, and the clock, date, chevron and network glyphs are **absent**.

⚠ This is NOT the old "text doesn't render" or "A8 format unsupported" claim — both are
refuted and stay refuted (`A8_UNORM` reports `0x4507`, SAMPLE+RT, and desktop label
glyphs render perfectly). The refined statement is narrower and about transport:
**an A8 atlas written in one process and sampled in another comes back wrong.**

#### A second, independent oddity in the same log

`SetRenderTargets` census, sampled every 256 calls:

- **dwm** binds `rt0=2944x1840` on essentially every sampled call — healthy.
- **Explorer** binds `NumRTVs=1` with **`rt0=0x0`** (a NULL resource — the format string
  is `rt0=%ux%u` and prints `0x0` when the pointer is NULL) on **every** sample from
  #257 through #4865, while `WMCSOFF` shows it still issuing `draws=4` per stream.
  Only the very first calls (#5 `1024x1024`, #8 `64x320`) bind a real target.

#### ⛔ …and that second lead is now REFUTED — it was the sampling alias

1.1.21.2 replaced the sampled look with an **unsampled census** — every
`SetRenderTargets` call is counted, and the totals are printed on the existing line
(`CENSUS real=… null=… zeroSlots=…`). Counters only; no new dereference, no array
indexing.

Measured on a render-mode boot:

```
wm.dwm.exe.log       CENSUS real=174  null=60  zeroSlots=23
wm.Explorer.EXE.log  CENSUS real=2    null=2   zeroSlots=4
wm.LogonUI.exe.log   CENSUS real=2    null=3   zeroSlots=3
```

**Explorer binds real render targets.** It is *not* persistently drawing into NULL —
the every-256th sample was simply landing on the unbind every time, exactly the alias
flagged above. ⇒ Do not re-chase this. The lesson is the general one:

> A periodic sampler against a periodic workload can manufacture a perfect,
> completely false correlation. When a census shows the *same* value on every
> sample, suspect the sampler before the subject.

⚠ Explorer's totals are small because an idle desktop barely renders — those are the
first-8 unconditional prints. A busier steady-state read is still worth taking, but the
"always NULL" claim is dead either way.

⚠ `0x00304507` (BGRA) vs `0x00004507` (A8/R8) in `CheckFormatSupport` differ only by
`0x00300000` = the two multisample bits. Already refuted as a cause; not a lead.

---

## 5. Solved — root causes worth remembering

- **Logon hang:** `Present` returned SUCCESS without emitting a DMA buffer. dxgk assert
  id=494 carries its own text.
- **Frozen desktop:** an `abort()` on a TGSI `SWITCH` in the shader translator killed
  every GPU client.
- **GPU wedge:** a missing per-CS `CSG_DB_REQ` doorbell meant fragment jobs never
  completed. A wedge then poisons every subsequent submit.
- **Fault-free CSF timeout** = shader cores unpowered, not a fault.
- **Taskbar flashing:** dwm keeps four buffers holding *different* frames, and a
  per-sweep contest alternated between them. Fixed with selector hysteresis (`UvSticky`)
  plus a freshness window (`UvStickyTicks=4`). Before: `bo 63,63,63,63,64,62`. After:
  **bo=63 won 20/20**.
- **A self-destruct I shipped:** making `UvSticky` live-read meant the driver re-read the
  value the fuse had just burned to 0, switching sticky off seconds into every boot.
  Fixed by baking `WINMALI_DEF_UVSTICKY = 1` and burning **to** it.
- **`bo_free` destroyed dwm's shared sync handle** — one variable answering two questions.
- **The 52% ceiling was our own AFBC union clamp**, not hardware.
- **Source pitch is not panel pitch** (2944 wide, stride 11776).
- **`GPU_COHERENCY_FEATURES = 0x00000000`** — no ACE/ACE-lite. The GPU is genuinely
  non-coherent, so CPU-side cache maintenance around submit is the correct model
  (`WinMaliBoFlushVm`). Its own comment records that skipping it makes draws render
  nothing — **do not weaken it while optimising it.**

---

## 6. REFUTED — do not re-chase these

Each was measured, with the channel calibrated first.

| Hypothesis | How it died |
|---|---|
| **"Text/glyphs don't render"** | **Refuted.** "Recycle Bin" and "Microsoft" desktop labels — real DirectWrite glyphs — render **perfectly** in render mode. ⚠ An earlier retirement of this used the M365 icon label as evidence; that label is **baked into the icon bitmap** and never tested text at all. Right verdict, worthless evidence. |
| **"The tray hides in a dwm buffer we don't select"** | **Refuted.** With `UvSticky=0` the selector free-ran across **bo=62, 63, 64**; all three are **pixel-identical** in the taskbar strip. |
| **SM5 opcode holes in the UMD** | Refuted — the UMD is clean; the corruption was the KMD permanent latch. |
| **`translateBlend()` returning D3D enums** | Real bug, but **latent**: gated behind `!max_dual_source_render_targets` and only for SRC1 factors; panfrost sets that to 1. Re-verified in source, not assumed. |
| **MPO / independent flip queue** | Cap needs `SupportDirectFlip`; there is no independent flip queue ⇒ dead end. |
| **`LiveSrcVisible` as an oracle** | Frozen on a branch the worker no longer takes. Use `StatSrcChg` / `StatSrcScans`. |
| **`SetDisplayModeCb` E_FAIL** | Red herring — dwm presents only 1024×1024 atlases. |
| **dwm's composition surface** | Unreachable. Stop chasing it. |
| **The MediaTek write-back gap** | `panfrost_best_modifier` can never return that modifier. |
| **AFBC on A8** | `PAN_AFBC_MODE_INVALID` for arch ≥ 7. |
| Also refuted | aperture deadlock · SetPowerState · hold-over · `pAllocationInfo` union · `'PaWM'` AllocId · NoScreenToScreenBlt · UMD `hDstAlloc` · feature level · FlipImmediateMmIo · per-op `vm_bind` routing |

### The method failure worth internalising

The coherency theory was chased through four layers, **forcing each refusing gate open**,
with zero evidence anything reached the GPU. The stop signal was `WMBOSYNC` reading 0 on
three consecutive builds.

> **When a chain refuses, measure *why* it refuses. Do not force the next gate.**
> And never change a global allocation flag to reach one path.

---

## 7. Instruments

- **The KMD's whole `DbgPrint` stream is readable.** EDK2 ring at PA `0x48300000`,
  surfaced as `Parameters\TraceTail` (+ `TraceTailBytes`). `TracePrevBoot` snapshots
  256 KB at `DriverEntry`. ⚠ 64 KB was too small — EDK2 refills the ring before our
  driver loads. **This is how you prove which binary is loaded**: the DriverEntry banner
  carries `kmd v<version>`.
- **`FrameCrop`** — a 1:1 crop of the real scanout, published with `FrameCropSeq` /
  `FrameCropSrc` / `FrameCropBo` provenance stamps. **It is live re-aimable**:
  `WinMaliPublishCrop_()` re-reads `CropX/Y/W/H` on *every* publish
  (`WinMaliDriver.c:725`). ⚠ `cropshot.ps1` prints "takes effect on the next driver load"
  — **that comment is stale.** Budget ≤ ~323,840 pixels per crop.
- **`FrameThumb`** — whole screen, 8× decimated (368×230). Good for locating windows.
- The user has confirmed these images match what is physically on the panel.

### Measurement discipline (each lesson cost real time)

- ⛔ **A zero is only evidence if the channel is proven live.** This trap has now fired
  **four** times: `ResourceMap: 0`, `WMBOSYNC: 0`, and most recently `strings` not being
  installed in Git Bash — `strings -a dll | grep -c TAG` returned 0 four times in a row
  and was reported as a verification. Use `grep -ac TAG file` and **always calibrate with
  a string that must be present**.
  > A result that agrees perfectly with the hypothesis deserves *more* scrutiny, not less.
- ⛔ **Validity gate before any oracle.** Confirm the knob is still set and `BootNonce`
  changed. An absent knob and a disabled feature produce identical counters.
- ⛔ `Set-ItemProperty` is lazily flushed and **a knob can simply vanish across a hard
  power cycle**. Use `setknob.ps1` (RegistryKey.Flush → RegFlushKey), then re-open a
  fresh handle to read back.
- ⛔ **Never measure on the first boot after a deploy** (BO cache list poisoning).
- ⛔ Full-mask ETW helps cause the hangs. 3–5 s, narrow mask, `Get-WinEvent -Oldest`
  mandatory.
- ⛔ A grep that matches your own tag is not a finding (`fault` matched `de-fault`, 46
  times; `SRC1` matched `WMPRESSRC1`, 156 times). Read the lines.
- ⛔ A background task's log is **not evidence until the task has an exit code.**

---

## 8. Operating the tablet — safety rules

**Connection:** `192.168.1.220`, WinRM over SSL, user `User`, password `0`.
ICMP is enabled and calibrated, so **ping is now a valid liveness oracle** — a total ARP
failure means the box is genuinely off the network. (Before ICMP was enabled, every rule
was `enabled=False` and ping failures meant nothing; that produced one false "power cycle
needed" request.)

| ⛔ Never | Why |
|---|---|
| `Disable-PnpDevice` | hung the box 5× |
| `pnputil /remove-device` on the display adapter | — |
| kill `explorer` | (killing `dwm` is fine) |
| `BasicDisplay\Start=4` | hangs the boot |
| `TdrDodVSyncDelay` | — |
| SPFT "Format all" | — |
| `New-Item -Force` on an existing registry key | **deletes every value** — done twice |
| `Start-Process shutdown` over WinRM | does nothing. Use `Restart-Computer -Force` *inside* an `Invoke-Command -UseSSL` session, then **assert the boot stamp and `BootNonce` changed** |

- **Disarm the present worker (`PresentTimerHz=0`) before any live rebind**, and
  remember to restore it — the driver reads it at load, so setting it back afterwards
  does not re-arm the current session.
- Deploy + reboot is 5–8 minutes. Always background it.
- WinRM needs ~70 s after boot. Poll, don't assume.
- **WinRM is session 0.** Desktop-scoped probes fail; `Get-Process | MainWindowTitle`
  returns empty. That is why `FrameCrop`/`FrameThumb` exist.
- Two Windows installs exist; the dev install is guarded by `devgate.ps1`
  (`Assert-DevInstall`). **Boot time going backwards means you are on the spare.**
- Git Bash → WSL → PowerShell quoting mangles `$variables` and backslashes. **Write the
  script to a file, then run the file.**

---

## 9. Known-live bugs, filed but unfixed

1. **`ShaderParse.c:328` — CUSTOMDATA integer underflow.** `count = *curr - 2` taken from
   the shader blob; a length token of 0 or 1 underflows to `0xFFFFFFFE` →
   `MALLOC(~17 GB)` → NULL → unconditional `memcpy(NULL, ...)`. A large-but-allocatable
   count instead **over-reads past the blob into unrelated heap, which is baked into a
   constbuf the shader can read back.** The only guard is an `assert()`, removed by
   `b_ndebug`.
2. **`WinMaliBo.c:2197` — DISPATCH_LEVEL lock convoy.** `WinMaliBoFlushVm` holds the
   BoTable spin lock across an unbounded walk calling `KeFlushIoBuffers` per MDL.
   `BoCreateFromDxgkAllocation` and `BoCreateFromUserVa` both set `ExclusiveVmId = 0`, so
   **every imported dxgk surface matches regardless of `VmId`** — the ~21 MB primary
   alone is ~330k cache-maintenance ops — and it runs **twice per submit** while every
   other CPU spins on the same lock.
   ⚠ An adversarial verifier **rejected** the claim that this causes `0x133`/`0x101`
   bugchecks as over-reach, while confirming the real consequence (a multi-ms system-wide
   convoy per submit). Its link to the render-mode stalls is **plausible, not proven**.
   ⛔ Do not weaken the flush while optimising it — see §5 on coherency.

### Note on `KeFlushIoBuffers`

The BO MDLs are hand-built with `MmInitializeMdl(mdl, NULL, ...)` — `StartVa` NULL, no
system mapping — which raises the question of whether the flush can do virtual-address
cache maintenance at all. **Partially answered:** it is a *real exported function* on
ARM64 under our NTDDI, not the compiled-out macro older kits use, so the cheapest version
of "the flush does nothing" is refuted. Whether it can service a VA-less MDL is **still
open** and needs an ARM64 `ntoskrnl` disassembly. A copy has been pulled to the
scratchpad as `ntoskrnl_arm64.exe`.

---

## 10. Suggested next steps, in order

1. **Chase the modifier mismatch** (§4.3). Find where a shared surface's modifier is not
   propagated across open/import. This is the live lead and it explains both symptoms.
2. Fix the two filed bugs in §9.
3. Settle the `KeFlushIoBuffers`-on-VA-less-MDL question by disassembly.
4. Render-mode hardware flip is an **optimisation, not a blocker** — the OVL flip cannot
   fix render mode, and that trap has already been walked into once. dxgk only observes
   `SetVidPnSourceAddress` → our ack → `CRTC_VSYNC` echoing that address, all of which we
   already do correctly. **Before building, ask what the other side can actually
   observe.**

---

## 11. Collaboration

`github.com/awesomefroggy29/Panorama` — shared between claude-g615 (this work, Mali-G615,
**arch 11**) and mrfrogy's codex-g610 (**arch 10**). Split by layer.
⛔ Only `claude-g615/` is ever staged here. Every other driver stays in a private repo.
⛔ A force-push does **not** delete secrets from history.

Worth sending across: the TGSI `SWITCH` `abort()` fix, the AS-slot use-after-free warning,
and that `AllowDwm` was passed weeks ago (his "enable dwm" milestone is one we are past).

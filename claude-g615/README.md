# claude-g615 — WinMali on Mali-G615 (MT6897 / Dimensity 8300)

Checkpoint **2026-08-25 (evening)**. Same ancestor as `codex-g610` (`WinMali-rk3588.sln`),
taken further on the **display/WDDM** side. `driver/` is the KMD+UMD tree;
`mesa/winmali-mesa-workingtree.patch` is a **source-only** diff against upstream mesa.

## Hardware difference that matters

| | codex-g610 | claude-g615 |
|---|---|---|
| GPU | Mali-G610, Valhall **arch 10** | Mali-G615, Valhall **arch 11** |
| panfrost/panvk support | good (RK3588 is the reference board) | **arch 11 is a gap — panfrost skips it and fails SILENTLY** |

⚠ Arch-11 code paths fail *quietly*. Do not assume an arch-10 result carries over.

---

## ✅ WHERE THIS IS NOW

### 1. Display-only (KMDOD) — a complete, shipping desktop
Device `OK / CM_PROB_NONE`. Full Windows 11 desktop: taskbar, tray, **live clock**, icons —
filmed and verified. **Real dual-pipe hardware page flips on the MT6897 OVL**, one flip per
present, `StatOvlFlip2Rej = 0`, thousands per boot.
**It survives power cycles untouched** — the working configuration is the *compiled default*
(KMD 1.1.16.1+), so an empty registry boots to a working desktop.

### 2. Render+display — a LIVE, GPU-COMPOSITED DESKTOP (new, 2026-08-25)
The full miniport path now reaches a **live desktop**: real wallpaper, full taskbar, multiple
composited windows, animating content. Windows Terminal runs with **only `WinMaliUmd.dll`
loaded — no WARP** — so applications genuinely render on Mali and reach the panel.

**The unlock was reporting WDDM 2.0** (`DXGK_DRIVERCAPS.WDDMVersion = DXGKDDI_WDDMv2`):
```
StatFbAllocs (dwm primary create/destroy churn)   ~2000 per boot  ->  13
```
At the compiled 3.2 dwm destroyed its primary ~9/s forever and never composited. At 2.0 it
keeps the primary and composes normally. That single cap change is the difference between a
frozen pre-taskbar frame and a working desktop.

⛔ **`LiveSrcVisible` WAS NEVER THE RIGHT ORACLE.** It reads **0** while the panel is
demonstrably live — the present worker paints regardless of the OS source-visibility flag.
Months of judging render mode by that flag were judging the wrong thing. Use instead:
`StatSrcChg` (source content changing), `StatScanoutBlits`, `StatSweeps`, `StatWorkerTicks`.

**Hardware flips now work in render mode.** The dual-pipe OVL flip used to live inside
`WinMaliKmdPresentDisplayOnly` — the KMDOD DDI — and was therefore *structurally unreachable*
from render. 1.1.17.x lifts it into `WinMaliOvlFlipDual_()`, called from the present worker at
sweep completion behind the fuse-burned `OvlFlipRender`. Measured: `StatOvlFlipN` tracks
`StatSweeps` 1:1 with zero rejects.

### The flashing taskbar — fixed (1.1.18.x)
dwm keeps **four** full-screen buffers holding **different** frames. A per-sweep contest
between them picks a different one as ticks shift — measured across six captures as
`bo 63,63,63,63,64,62`. Showing frame N, then N−2, then N−1 *is* the flashing. Follow one
buffer and it becomes video again:

```
before:  bo=63 won 11/20 samples, bo=62 won 9/20     taskbar blinks in and out
after :  bo=63 won 20/20                             steady
```

Two traps that fix exposed, both worth stealing:

* **"Still live" must mean *changed recently*, not *ever changed*.** `changes` is cumulative,
  so `live = (changes > 0)` is true for ever after the first write and the incumbent can never
  be released. That is not stability, it is a frozen frame — user-visible as a taskbar that
  stopped flashing while the clock stopped ticking. Hence the freshness window
  (`UvStickyTicks`).
* **A knob's lifetime and its fuse value are one design.** Making `UvSticky` live-read meant
  the driver re-read the value the one-shot fuse had just burned to `0`, switching stickiness
  off seconds into every boot. Bake the wanted value as a `WINMALI_DEF_*` default and burn
  **to it**, never to a bare literal.

### Still open: the tray/clock, and it is *not* a text-rendering bug
A controlled A/B — same driver, same 1:1 region `(2208,1696) 736×144`, same code path, only
the mode differs:

```
display-only : chevron, OneDrive, network, volume, "17:54:27 / 25/08/2026"
render       : completely empty
```

Display-only composites on **WARP** (CPU); render composites on **Mali**. So the defect is in
our GPU path — and that asymmetry is what makes the display-only capture a true control.

Traced to a specific mechanism, **not yet fixed**: glyphs are CPU-rasterised by DirectWrite and
written into `A8_UNORM` textures through `ResourceMap` — not by GPU draws, and not via
`UpdateSubresourceUP` (measured across all nine GPU clients: every upload is a constant/vertex
buffer or a tiny strip, nothing atlas-sized). The data is **provably present at unmap** —
Explorer's 442×66 glyph run comes back `25438/28288` non-zero at full range — and never reaches
the panel, while everything the GPU itself draws (A8 shape masks, the BGRA wallpaper) is
pixel-perfect. We report `MEMORY_ARCHITECTURE UMA=TRUE CacheCoherent=FALSE`, and gallium
panfrost never issues cache maintenance: every caller of `pan_kmod_queue_bo_map_sync()` lives
under `src/panfrost/vulkan/`. **Unflushed CPU writes** is the leading explanation and fits every
observation.

⛔ **Forcing that chain open is a mistake we already made.** Setting
`PAN_KMOD_BO_FLAG_WB_MMAP` on every CPU-mappable BO, and flushing on every texture unmap,
produced several black-screen boots — dwm crashed and restarted with *no explorer at all*.
Changing a **global allocation flag** to reach one refusing code path was the error, and the
stop signal had been there for three builds: the flush counter read **0** every time.
**When a chain refuses, measure why it refuses — do not force the next gate.**

### Refuted by measurement (do not re-chase)
Each with the channel calibrated *first*, so the zero meant something:
SM5 opcode holes (0 `WMCSOPC1` in 106 logs) · glyph-atlas format support (`R8_UNORM` and
`A8_UNORM` both `0x4507`, SAMPLE+RT) · present-fence timeouts (`WMPRESFENCE1` = 0) · A8 and
BGRA render targets (dumped: crisp shapes, correct wallpaper) · AFBC on A8
(`PAN_AFBC_MODE_INVALID` for arch ≥ 7) · the MediaTek write-back gap (real, but
`panfrost_best_modifier` can never return that modifier).

### Diagnostics worth stealing
* **A 1:1 framebuffer crop, live re-aimable by registry** (`CropX/Y/W/H`, `CropEvery`) — any
  region of the panel at full resolution without a rebuild. The 8× thumbnail *cannot* show a
  glyph (the taskbar survives as 18 px), which is why it misled for hours. Every capture is
  stamped `FrameCropSeq` / `FrameCropSrc` / `FrameCropBo`, because **a registry blob outlives
  its boot and its age is invisible in its content** — a stale render-mode crop once
  impersonated a display-only control.
* **`TracePrevBoot`** — the whole **256 KB** DbgPrint ring snapshotted at `DriverEntry`, before
  this boot overwrites it. 64 KB was not enough: it only ever caught the *firmware* log, since
  EDK2 refills the ring before our driver loads.
* Counters over the map/upload paths (`WMMAPW`, `WMUPL`, `WMMAP1`).

---

## 🏆 dxgkrnl findings — useful to ANY WDDM driver author

Both found by disassembling `dxgkrnl.sys` 10.0.26100.9168 and letting it name its own gate,
after months of capability sweeps produced nothing.

### The MPO capability gate — `SupportMultiPlaneOverlay` requires `SupportDirectFlip`
```asm
DXGADAPTER::Initialize, gate #5
0x1400EA2C8  ldrb w8,[x19,#0xBF3]      ; = SupportDirectFlip
             if zero -> log 0x140385A10:
             "Driver reports SupportMultiPlaneOverlay cap but DirectFlip is not supported."
             -> 0xC000000D STATUS_INVALID_PARAMETER
```
`SupportDirectFlip` is the byte **adjacent** to `SupportMultiPlaneOverlay` (`d3dkmddi.h:2441`
vs `:2442`). Claiming MPO without it kills adapter start with no useful event.
**Confirmed on hardware: device `problem 43 → 0`, `QueryInterfaceCount 9 → 22`.**

### …and then dxgk withdraws it again, for a good reason
```asm
0x1400EABE8  ldr  x8,[x19,#0xCB0]      ; m_pRenderCore (NON-null for us)
0x1400EABFC  ldr  x8,[x8,#0x290]       ; virtual predicate
0x1400EAC14  strh wzr,[x22]            ; clears BOTH 0xBF3 and 0xBF4 in ONE 16-bit store
```
The predicate reads dxgk's **normalized flip state** (`adapter+0x738..0x768`), not a caps byte,
and tests independent/queued-flip readiness — exactly the bits we withhold because we cannot do
them. ⇒ **dxgk correctly refuses MPO to a render adapter with no flip queue.** The flip engine
has to live where the display controller is.

### The VidPn source **ownership** gate — why the source never came back
dxgk's CDD primary restore (`0x140117000`) reads the source owner type and runs **only** for
owner ∈ {`UNOWNED`=0, `EXCLUSIVEGDI`=3} (`0x1401170C8: cmp w19,#0; ccmpne w19,#3,#4; beq`).
In render mode **dwm holds SHARED/EXCLUSIVE**, so dxgk takes the *"Ownership has been changed
to…"* fall-through and **by design does nothing** — the WDDM owner is presumed responsible for
re-lighting itself. Process attribution matches exactly: source OFF by `dwm.exe`, ON by
`csrss.exe`. ⇒ **The gate was never a capability**, which is why ~30 refutations found nothing
wrong with anything we publish.

---

## The MT6897 display pipeline (this is a real display engine)

Mali-G615 has none — true of every Mali — but the SoC does, and we drive it:
```
MMSYS 0x14000000   MUTEX 0x14001000   DSI0 0x1400d000
OVL0  0x14402000 (disp0)              OVL0 of disp1 0x14602000
  ADDR_BASE 0x0f40  ADDR(n)=0x0f40+0x20*n  ADDR_MSB(n)=0x0f4c+0x20*n
  STA 0x000  INTSTA 0x008  ROI_SIZE 0x020 (h<<16|w)  SRC_CON 0x02c
  L0_SRC_SIZE 0x038  L0_OFFSET 0x03c   DUMMY_REG 0x200 (scratch, safe write probe)
```
Dual-pipe: OVL0 covers columns 0–1482 from `0xFD619400`; OVL4 covers 1461–2943 from
`0xFD61AAD4` (offset **5844** = 1461 px × 4 B), 22-column merge overlap. Double-buffered:
write both ADDR registers, then swap front/back.
⛔ Whitelist every address you program — only the three known EDK2 carveout frames.

---

## ★ Measurement lessons that cost real boots (arch-independent)

- **A counter is only evidence if its WRITER runs in the mode under test.** This cost four
  separate wrong conclusions in one day. Most `Stat*` values here are published by the present
  worker; on a boot where the adapter fails, or in a mode where that publisher never runs, they
  hold whatever they last held. **Zero every counter you intend to read, in the arming step** —
  an unzeroed counter is indistinguishable from a stale one — and read a liveness counter
  (`StatWorkerTicks`) beside anything you interpret.
- **A counter with no control group is not a finding.** The "DWM allocation storm" looked
  damning until measured on the path that *works*, where it looks identical.
- **On a frozen-frame bug, any on-screen element that predates the freeze proves nothing.**
- **A one-shot fuse must burn to the compiled DEFAULT, not to literal 0** — and the read default
  and the burn value must be the *same named constant*. Ours drifted (`PathXform2` read 5,
  burned 0), so a lever was silently off for days.
- **When you promote a configuration from experiment to default, every mechanism that treats it
  as exceptional becomes a bug.** An armed-config boot guard here would have committed
  `ComputeOnly=1` and permanently disabled the adapter by the third clean boot.
- **A stub in a DDI slot is a false claim dxgk acts on**, and filling a slot is itself a
  capability claim — made *earlier* than any cap.
- **Read `DxgKrnl-Admin` event 549 before theorising** about an adapter-start failure; it names
  the status and reason.
- **No float literals in KMD code** — `1.0f` pulls in `_fltused` and fails the link. Write the
  IEEE bit pattern (`0x3F800000`).
- **Disassemble dxgkrnl and let it name its own gate.** Two hard blockers fell in one afternoon
  that capability sweeps had not touched in months.

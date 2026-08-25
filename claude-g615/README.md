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

**Still open:** delivery is the worker's CPU blit (~15 fps). The dual-pipe OVL hardware flip
lived inside `WinMaliKmdPresentDisplayOnly` — the KMDOD DDI — and is therefore *structurally
unreachable* from render mode. 1.1.17.x lifts it into `WinMaliOvlFlipDual_()` called from the
present worker at sweep completion, behind the fuse-burned knob `OvlFlipRender`.

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

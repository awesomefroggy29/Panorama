# claude-g615 — WinMali on Mali-G615 (MT6897 / Dimensity 8300)

Snapshot **2026-08-25**. Same ancestor as `codex-g610` (`WinMali-rk3588.sln`), taken
further on the **display/WDDM** side. `driver/` is the KMD+UMD tree;
`mesa/winmali-mesa-workingtree.patch` is a **source-only** diff against upstream mesa
(no build artefacts).

## Hardware difference that matters

| | codex-g610 | claude-g615 |
|---|---|---|
| GPU | Mali-G610, Valhall **arch 10** | Mali-G615, Valhall **arch 11** |
| panfrost/panvk support | good (RK3588 is the reference board) | **arch 11 is a gap — panfrost skips it and fails SILENTLY** |

⚠ Arch-11 code paths fail *quietly*, not loudly. Do not assume a working arch-10
result carries over without checking.

---

## ✅ WHAT WORKS TODAY

**A real Windows desktop, rendered over Mali, scanned out by the MediaTek display
controller, surviving power cycles with no intervention.**

- **Display-only miniport (KMDOD)** is the shipping shape. Device `OK / CM_PROB_NONE`.
- **Real dual-pipe hardware page flips on the MT6897 OVL.** Measured every boot:
  `StatDodPresentN == StatOvlFlip2N` with `StatOvlFlip2Rej = 0` — one hardware flip per
  present, thousands per boot, zero rejects.
- **The configuration is the compiled default** (KMD 1.1.16.2), so an empty registry boots
  to a working desktop and a failed experiment self-heals on the next boot.

### ⛔ CORRECTION TO THE PREVIOUS README
It said *"There is **no page flip and no OVL**… pixels reach the panel by CPU memcpy."*
**That is no longer true, and the reasoning behind it was wrong.** Mali-G615 has no display
engine (true of every Mali), but **the MT6897 SoC has a complete MediaTek DISP pipeline** and
we now drive it directly:

```
MMSYS  0x14000000   MUTEX 0x14001000   DSI0 0x1400d000
OVL0   0x14402000   (disp0)            OVL0 of disp1 at 0x14602000
  DISP_REG_OVL_ADDR_BASE 0x0f40   ADDR(n) = 0x0f40 + 0x20*n   ADDR_MSB(n) = 0x0f4c + 0x20*n
  STA 0x000  INTSTA 0x008  ROI_SIZE 0x020 (h<<16|w)  SRC_CON 0x02c
  L0_SRC_SIZE 0x038  L0_OFFSET 0x03c   DUMMY_REG 0x200 (scratch, safe write probe)
```
The panel is **dual-pipe**: OVL0 covers columns 0–1482 from `0xFD619400`, OVL4 covers
1461–2943 from `0xFD61AAD4` (offset **5844** = 1461 px × 4 B), with a 22-column merge overlap.
Double-buffered: write both ADDR registers, then swap front/back.

---

## 🔴 WHAT DOES NOT WORK — render+display mode

With `DodMode=0` (full render+display miniport) the desktop appears for ~1.6 s and then
freezes: our VidPn source is switched off and never re-acquired.

**The measured shape (KMD 1.1.16.x, many boots):**
```
CommitVidPn OK at 0.19 s, Visibility TRUE      <- the ONLY commit in 65 s
Visibility FALSE at ~1.7 s                     <- never returns
StatPresentCalls = 25..38   Blts = 0   NullCookie = all of them
StatFencePair: Submitted == Completed, LAG = 0, every submit error 0
dwm UMD log: presents are 1024x1024 atlases, toPrimary(dstBO>=16MB) = 0
dwm has BOTH WinMaliUmd.dll and d3d10warp.dll loaded, in every boot
```

### ⛔ Eliminated by measurement (do not re-open without new evidence)
| killed | how |
|---|---|
| VidPn negotiation / scaling / monitor mode set / EDID | display-only re-acquires the source `FALSE→TRUE` in **5 ms with NO CommitVidPn between** ⇒ the re-acquire never goes through VidPn |
| persisted CCD config demanding `Scaling=4` | claimed it (`out.scal=0x0F`) *and* deleted the unsatisfiable no-EDID monitor set ⇒ **identical** counts 161/152/1 |
| `STATUS_GRAPHICS_MONITOR_NOT_CONNECTED` | `DxgkCbIndicateChildStatus(Connected=TRUE)` returned **0** and the source still died |
| "our presents never complete" | fence Submitted == Completed, lag 0 |
| "the DWM allocation storm" | dwm churns full-screen surfaces on the **working** display-only path too — it was never evidence |
| D3D feature level / DDI ladder | identical at `0x73` (FL10_1) and `0x77` (FL11_0) |
| a second display adapter | exactly ONE class-Display devnode |

⚠ **`Blts=0` / `NullCookie=N` is NORMAL.** DWM's composition target is unreachable by *any*
display miniport by OS design: it lives only inside `D3DKMT_FLIPMODEL_PRESENTHISTORYTOKEN`, and
`PresentHistoryTokenOnly` presents make **zero driver calls**. A frozen `StatPresentCalls` is
not evidence dwm stopped presenting.

---

## 🏆 THE MPO FINDING — useful to any WDDM driver author

Claiming `DXGK_DRIVERCAPS.SupportMultiPlaneOverlay` killed adapter start
(`CM_PROB_FAILED_POST_START`, `STATUS_INVALID_PARAMETER`, no useful event). Found by
disassembling `dxgkrnl.sys` 10.0.26100.9168, `DXGADAPTER::Initialize`:

```asm
; gate #5
0x1400EA2C8  ldrb w8,[x19,#0xBF3]      ; = SupportDirectFlip
             if zero -> log 0x140385A10:
             "Driver reports SupportMultiPlaneOverlay cap but DirectFlip is not supported."
             -> 0xC000000D STATUS_INVALID_PARAMETER
```
**`SupportMultiPlaneOverlay` requires `SupportDirectFlip` to be claimed with it.**
Confirmed on hardware: device `problem 43 → 0`, `QueryInterfaceCount 9 → 22`.

**But it then dead-ends, and the reason is instructive:**
```asm
0x1400EABE8  ldr  x8,[x19,#0xCB0]      ; m_pRenderCore  (NON-null for us)
0x1400EABFC  ldr  x8,[x8,#0x290]       ; virtual predicate
0x1400EAC10  cbnz w8,skip
0x1400EAC14  strh wzr,[x22]            ; clears BOTH 0xBF3 and 0xBF4 in one 16-bit store
```
The predicate reads dxgk's **normalized flip state** (`adapter+0x738..0x768`), not a caps byte,
and tests independent/queued-flip readiness. We deliberately withhold `FlipIndependent` /
`DdiPresentForIFlip` / `FlipOnVSyncWithNoWait` because we genuinely cannot do them.
⇒ **dxgk is correctly refusing to expose MPO to a render adapter with no independent flip
queue.** Claiming those bits without the mechanism yields device problem 37/43, not MPO.

**Conclusion:** the flip engine has to live where the display controller actually is. The
correct shape — and what WoA Snapdragon does — is a single render+display miniport whose
`SetVidPnSourceAddress` programs real display-controller flip registers. We already do exactly
that in display-only mode; the work is moving it into the render path.

---

## ★ Lessons that cost real boots (shared, arch-independent)

- **A counter with no control group is not a finding.** The "allocation storm" and
  "`toPrimary=0`" both looked damning until measured on the path that *works*, where they look
  identical.
- **A zero is only evidence if the channel is proven live.** Most `Stat*` values are published
  by the present worker; on a boot where the adapter fails, the worker never runs and every one
  of them is stale. Read a liveness counter (`StatWorkerTicks`) beside anything you interpret.
- **A one-shot fuse must burn to the compiled DEFAULT, not to literal 0** — and the read default
  and the burn value must be the *same named constant*. Ours drifted (`PathXform2` read 5, burned
  0), so a lever was silently off for days and several "that changed nothing" results were really
  "that never ran".
- **When you promote a configuration from experiment to default, every mechanism that treats it
  as exceptional becomes a bug.** An armed-config boot guard here would have committed
  `ComputeOnly=1` and permanently disabled the adapter by the third clean boot.
- **A stub in a DDI slot is a false claim dxgk acts on.** Filling a slot is itself a capability
  claim, and it is made *earlier* than any cap.
- **Read `DxgKrnl-Admin` event 549 before theorising about an adapter-start failure** — it names
  the status and reason directly.
- **No float literals in KMD code**: `1.0f` pulls in `_fltused` and fails the link. Write the
  IEEE bit pattern (`0x3F800000`).

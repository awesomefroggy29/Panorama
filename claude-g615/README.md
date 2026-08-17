# claude-g615 — WinMali on Mali-G615 (MT6897 / Dimensity 8300)

Snapshot 2026-08-17. Same ancestor as `codex-g610` (`WinMali-rk3588.sln`), taken
further on the **display/WDDM** side. `driver/` is the KMD+UMD tree;
`mesa/winmali-mesa-workingtree.patch` is a **source-only** diff against upstream
mesa (no build artefacts).

## Hardware difference that matters

| | codex-g610 | claude-g615 |
|---|---|---|
| GPU | Mali-G610, Valhall **arch 10** | Mali-G615, Valhall **arch 11** |
| panfrost/panvk support | good (RK3588 is the reference board) | **arch 11 is a gap — panfrost skips it and fails SILENTLY** |

⚠ Arch-11 code paths fail *quietly*, not loudly. Do not assume a working arch-10
result carries over without checking.

## State on G615

**Works**
- Mali executes real command streams (CSF: GROUP_CREATE, tiler heap, submits
  complete, syncobj waits return 0).
- The composited desktop/lock-screen reaches the panel at **full height**.
- Driver loads clean: `device=OK / CM_PROB_NONE`, dwm + LogonUI both map our UMD.

**Blocked — logon never completes while the driver is armed**
dxgk rejects every LogonUI present *before it reaches the driver*:
```
ETW Microsoft-Windows-DxgKrnl id=184
  FlipInterval=0 (IMMEDIATE)  Flags=0x8000 (RedirectedFlip)
  ReturnStatus=0xC000000D STATUS_INVALID_PARAMETER      9/9 rejected
```
Only 2 `DxgkDdiSetVidPnSourceAddress` calls land in 287 s against 6093 vsyncs.
Disarm the driver and logon completes, every time.

**Controlled A/B (same box, same OS, same LogonUI, our driver disarmed):**
the known-good display driver accepts **200/200** presents including 5 of the
*identical* RedirectedFlip+IMMEDIATE shape. ⇒ the present is fine; **our adapter
is the cause**.

## 🔴 SHARED BUG — BOTH TREES HAVE IT

dxgk emits, on the *same thread* as each rejected present, ~2 per rejection:

```
ETW id=494:  "WDDM2 driver calls WDDM1.x DDI!"   (EventData1 = 88)
```

Both trees declare `WDDMVersion = DXGKDDI_WDDMv3_2` and then call the WDDM 1.x
callback **`DxgkCbGetHandleData`**:

* `codex-g610`  — 7 calls / 2 files
* `claude-g615` — 16 calls / 4 files (Alloc, Ddi, Escape, Kmd.h)

This is the strongest current lead for the present rejection, and it is a
**shared** fix: replace `DxgkCbGetHandleData` with the private driver data the
WDDM2 DDIs already hand you.

## Refuted (measured, not assumed)

1. D3D feature level — nothing ever requests D3D10.1/D3D11 from us.
2. `FlipCaps.FlipImmediateMmIo` — set it, dxgk still returns `0xC000000D`.
3. "our caps never reach dxgk" — `QueryCapsOutSz == QueryCapsSizeof == 592`,
   `FlipCaps` at offset 60, value `0xA` confirmed written.
4. `NoScreenToScreenBlt` / `NoOverlapScreenBlt`.
5. `DxgkDdiSetPowerState` as a teardown hook (never called).
6. "we scan out the wrong buffer" — dxgk names the surface every flip.

## Notes for anyone reading the display path

- There is **no page flip and no OVL**. `SetVidPnSourceAddress` only *records*
  the address; pixels reach the panel by **CPU memcpy into the firmware
  framebuffer** (`0xFD619400`, `MmMapIoSpaceEx`) that EDK2 lit and the firmware
  DISP block still scans out. All three `SetVidPnSourceAddressWithMultiPlaneOverlay*`
  DDIs are NULL.
- A recent real bug worth knowing: the read-overrun guard in
  `panfrost_resource_copy_region` read `slices[].tiled_or_linear.row_stride_B` on
  an **AFBC** surface. `afbc` and `tiled_or_linear` are a **union**, so it read an
  AFBC header field (24576) instead of the real pitch (11784), invented a 975-row
  limit for an 1842-row image and truncated every frame to **52.9%** of the
  screen. Fixed by bounding on the resource extent and only trusting
  `row_stride_B` when `modifier == DRM_FORMAT_MOD_LINEAR`.

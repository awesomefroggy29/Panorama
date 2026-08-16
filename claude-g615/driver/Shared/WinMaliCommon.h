/*
 * WinMaliCommon.h - RECONSTRUCTED for the MT6897 port.
 *
 * The upstream zip ships Shared\WinMaliEscape.h and Shared\WinMaliWddmBridge.h
 * but NOT WinMaliCommon.h, so UMD\WinMaliUmd.h fails to compile as received.
 * The UMD is a thin stub whose only job is to exist and export
 * OpenAdapter10_2 -- dxgkrnl REQUIRES InstalledDisplayDrivers /
 * UserModeDriverName to resolve to a real DLL or it fails the adapter with
 * Code 43 (see WinMaliKmd.inx:212-214), even for a render-only driver that
 * will never execute a draw.
 *
 * This header therefore only needs to supply whatever small shared
 * definitions the stub references. Keep it minimal and additive: if the real
 * WinMaliCommon.h turns up later, prefer it.
 */

#pragma once

#include "WinMaliEscape.h"

/* ABI version pair, mirrored from the KMD side (WinMaliKmd.h). */
#ifndef WINMALI_ABI_MAJOR
#define WINMALI_ABI_MAJOR   1
#endif
#ifndef WINMALI_ABI_MINOR
#define WINMALI_ABI_MINOR   0
#endif

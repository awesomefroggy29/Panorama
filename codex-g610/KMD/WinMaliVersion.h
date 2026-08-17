/*
 * WinMaliVersion.h
 *
 * Single source for the KMD file version, consumed by:
 *   - WinMaliKmd.rc  (VERSIONINFO FILEVERSION on WinMaliKmd.sys)
 *   - WinMaliDriver.c (DriverEntry banner, so kd shows which binary
 *     is loaded; __DATE__/__TIME__ are unavailable - the 26100 WDK
 *     compiles deterministic, so those macros don't expand)
 *
 * build.ps1 supplies all four components to both the C/RC compiler and
 * StampInf.  The fallbacks below keep direct Visual Studio builds valid,
 * but deployment packages should be made through build.ps1 so every build
 * receives a newer PnP rank.  dxgkrnl 26100 requires the SYS file version
 * and stamped INF DriverVer to match exactly.
 */
#pragma once

#ifndef WINMALI_VERSION_MAJOR
#define WINMALI_VERSION_MAJOR 1
#endif
#ifndef WINMALI_VERSION_MINOR
#define WINMALI_VERSION_MINOR 0
#endif
#ifndef WINMALI_VERSION_BUILD
#define WINMALI_VERSION_BUILD 0
#endif
#ifndef WINMALI_VERSION_REVISION
#define WINMALI_VERSION_REVISION 85
#endif

#define WINMALI_VERSION_STRINGIZE_INNER(x) #x
#define WINMALI_VERSION_STRINGIZE(x) WINMALI_VERSION_STRINGIZE_INNER(x)

#define WINMALI_KMD_VERSION \
    WINMALI_VERSION_MAJOR, WINMALI_VERSION_MINOR, \
    WINMALI_VERSION_BUILD, WINMALI_VERSION_REVISION
#define WINMALI_KMD_VERSION_STR \
    WINMALI_VERSION_STRINGIZE(WINMALI_VERSION_MAJOR) "." \
    WINMALI_VERSION_STRINGIZE(WINMALI_VERSION_MINOR) "." \
    WINMALI_VERSION_STRINGIZE(WINMALI_VERSION_BUILD) "." \
    WINMALI_VERSION_STRINGIZE(WINMALI_VERSION_REVISION)

#pragma once

#ifdef __MINGW32__
#undef WIN32_LEAN_AND_MEAN /* for DEFINE_GUID macro */
#define _NO_OLDNAMES       /* avoid defining ssize_t */
#include <stdio.h>         /* for vsnprintf */
#undef fileno              /* we redefine this in vm_basic_defs.h */
#endif

#include <windows.h>
#include "util/u_debug.h"

#ifdef __cplusplus
extern "C" {
#endif


#define ST_DEBUG_OLD_TEX_OPS   (1 <<  0)
#define ST_DEBUG_TGSI          (1 <<  1)


#if MESA_DEBUG
extern unsigned st_debug;
#else
#define st_debug 0
#endif


#if MESA_DEBUG
void st_debug_parse(void);
#else
#define st_debug_parse() ((void)0)
#endif


#if defined(WINMALI_LOG_LIFECYCLE) && !defined(WINMALI_DEBUG_IMPLEMENTATION)
#define DebugPrintf(...) ((void)0)
#else
void
DebugPrintf(const char *format, ...);
#endif

#if defined(WINMALI_LOG_GLYPH)
#define WINMALI_TEXT_TRACE(fmt, ...) \
   DebugPrintf("[WinMaliText] " fmt "\n", ##__VA_ARGS__)
#else
#define WINMALI_TEXT_TRACE(...) ((void)0)
#endif

#if defined(WINMALI_SHADER_CAPTURE)
#define WINMALI_CAPTURE_TRACE(fmt, ...) \
   DebugPrintf("[WinMaliCapture] " fmt "\n", ##__VA_ARGS__)
void WinMaliCaptureRecord(const char *format, ...);
#else
#define WINMALI_CAPTURE_TRACE(...) ((void)0)
#define WinMaliCaptureRecord(...) ((void)0)
#endif


void
CheckHResult(HRESULT hr, const char *function, unsigned line);


#define CHECK_NTSTATUS(status) \
   CheckNTStatus(status, __func__, __LINE__)


#define CHECK_HRESULT(hr) \
   CheckHResult(hr, __func__, __LINE__)


void
AssertFail(const char *expr, const char *file, unsigned line, const char *function);

#ifndef NDEBUG
#define ASSERT(expr) ((expr) ? (void)0 : AssertFail(#expr, __FILE__, __LINE__, __func__))
#else
#define ASSERT(expr) do { } while (0 && (expr))
#endif


#if 0 && !defined(NDEBUG)
#define LOG_ENTRYPOINT() DebugPrintf("%s\n", __func__)
#else
#define LOG_ENTRYPOINT() (void)0
#endif

#define LOG_UNSUPPORTED_ENTRYPOINT() DebugPrintf("%s XXX\n", __func__)

#define LOG_UNSUPPORTED(expr) \
   do { if (expr) DebugPrintf("%s:%d XXX %s\n", __func__, __LINE__, #expr); } while(0)


#ifdef __cplusplus
}
#endif

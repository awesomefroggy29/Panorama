#define WINMALI_DEBUG_IMPLEMENTATION 1
#include "Debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(WINMALI_PRESENT_PROBE) && defined(HAVE_WINMALI_WDDM_KMOD)
#include "panfrost/lib/kmod/winmali/winmali_d3dkmt.h"
#endif

#if defined(WINMALI_SHADER_CAPTURE)
void
WinMaliCaptureRecord(const char *format, ...)
{
   char temp[MAX_PATH] = {0}, directory[MAX_PATH], process[MAX_PATH] = {0};
   if (!GetTempPathA(ARRAY_SIZE(temp), temp))
      return;
   snprintf(directory, sizeof(directory), "%sWinMaliShaderCapture", temp);
   if (!CreateDirectoryA(directory, NULL) &&
       GetLastError() != ERROR_ALREADY_EXISTS)
      return;

   GetModuleFileNameA(NULL, process, ARRAY_SIZE(process));
   const char *base = strrchr(process, '\\');
   if (base)
      memmove(process, base + 1, strlen(base + 1) + 1);
   for (char *p = process; *p; ++p) {
      if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
             *p == '.'))
         *p = '_';
   }

   char path[MAX_PATH];
   snprintf(path, sizeof(path), "%s\\%s-%lu-index.log", directory, process,
            GetCurrentProcessId());
   HANDLE file = CreateFileA(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;

   char line[2048];
   va_list ap;
   va_start(ap, format);
   int length = vsnprintf(line, sizeof(line) - 3, format, ap);
   va_end(ap);
   if (length < 0) {
      CloseHandle(file);
      return;
   }
   size_t used = (size_t)length < sizeof(line) - 3
                    ? (size_t)length : sizeof(line) - 3;
   line[used++] = '\r';
   line[used++] = '\n';
   line[used] = '\0';
   DWORD written;
   WriteFile(file, line, (DWORD)used, &written, NULL);
   CloseHandle(file);
}
#endif


#if MESA_DEBUG

unsigned st_debug = 0;

static const
struct debug_named_value st_debug_flags[] = {
   {"oldtexops", ST_DEBUG_OLD_TEX_OPS, "oldtexops"},
   {"tgsi", ST_DEBUG_TGSI, "tgsi"},
   DEBUG_NAMED_VALUE_END
};
void
st_debug_parse(void)
{
   st_debug = debug_get_flags_option("ST_DEBUG", st_debug_flags, st_debug);
}

#endif


void
DebugPrintf(const char *format, ...)
{
#if defined(WINMALI_LOG_DISABLED)
    (void)format;
    return;
#endif
#if defined(WINMALI_PRESENT_PROBE) && defined(HAVE_WINMALI_WDDM_KMOD)
    /* Initialization failures must survive Focused filtering and reach gpu.log.
     * Do not forward general per-draw chatter through the escape transport. */
    if (format &&
        (strncmp(format, "[WinMaliInit]", 13) == 0 ||
         strncmp(format, "[WinMaliUmdDiag] CreateDevice", 28) == 0)) {
        char initLine[2048];
        va_list initArgs;
        va_start(initArgs, format);
        vsnprintf(initLine, sizeof(initLine), format, initArgs);
        va_end(initArgs);
        OutputDebugStringA(initLine);
        winmali_d3dkmt_log("%s", initLine);
        return;
    }
#endif
#if defined(WINMALI_LOG_LIFECYCLE)
    (void)format;
    return;
#endif
#if defined(WINMALI_LOG_GLYPH)
    static const char text_tag[] = "[WinMaliText]";
    if (format == NULL ||
        strncmp(format, text_tag, sizeof(text_tag) - 1) != 0)
        return;
#endif
#if defined(WINMALI_SHADER_CAPTURE)
    static const char capture_tag[] = "[WinMaliCapture]";
    if (format == NULL ||
        strncmp(format, capture_tag, sizeof(capture_tag) - 1) != 0)
        return;
#endif
#if defined(WINMALI_LOG_FOCUSED)
    /* Historical adapter/callback-table dumps are useful for ABI bring-up but
     * dominate routine captures. Keep all untagged failures and the explicit
     * WinMali render/sync/DXGI breadcrumbs. */
    static const char noisy_tag[] = "[WinMaliUmdDiag]";
    if (format != NULL &&
        strncmp(format, noisy_tag, sizeof(noisy_tag) - 1) == 0)
        return;

    /* These are per-call/per-draw chatter.  Submission outcomes, failures,
     * DXGI boundaries, and untagged error messages remain visible. */
    static const char *const noisy_flow[] = {
       "[WinMaliFlow] D3D Flush ",
       "[WinMaliFlow] D3D Draw ",
       "[WinMaliFlow] DrawIndexed",
       "[WinMaliFlow] CreateResource ",
       "[WinMaliFlow] DestroyResource ",
       "[WinMaliFlow] ClearRT ",
       "[WinMaliFlow] ClearDS ",
    };
    if (format != NULL) {
       for (unsigned i = 0; i < sizeof(noisy_flow) / sizeof(noisy_flow[0]); ++i) {
          const size_t length = strlen(noisy_flow[i]);
          if (strncmp(format, noisy_flow[i], length) == 0)
             return;
       }
    }
#endif
    char buf[4096];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);

    OutputDebugStringA(buf);
}


/**
 * Produce a human readable message from HRESULT.
 *
 * @sa http://msdn.microsoft.com/en-us/library/ms679351(VS.85).aspx
 */
void
CheckHResult(HRESULT hr, const char *function, unsigned line)
{
   if (FAILED(hr)) {
      LPSTR lpMessageBuffer = NULL;

      FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                     FORMAT_MESSAGE_FROM_SYSTEM,
                     NULL,
                     hr,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     (LPSTR)&lpMessageBuffer,
                     0,
                     NULL);

      DebugPrintf("%s: %u: 0x%08lX: %s", function, line, hr, lpMessageBuffer);

      LocalFree(lpMessageBuffer);
   }
}


void
AssertFail(const char *expr,
           const char *file,
           unsigned line,
           const char *function)
{
   DebugPrintf("%s:%u:%s: Assertion `%s' failed.\n", file, line, function, expr);
#if defined(__GNUC__)
   __asm("int3");
#elif defined(_MSC_VER)
   __debugbreak();
#else
   DebugBreak();
#endif
}

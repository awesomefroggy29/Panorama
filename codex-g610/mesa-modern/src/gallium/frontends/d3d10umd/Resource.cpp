/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Resource.cpp --
 *    Functions that manipulate GPU resources.
 */


#include "Resource.h"
#include <stdio.h>
#include <string.h>

#include "Format.h"
#include "State.h"
#include "Query.h"

#include "Debug.h"

#include "util/u_math.h"
#include "util/u_inlines.h"
#include "util/u_rect.h"
#include "util/u_surface.h"
#include "util/format/u_format.h"

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
#include "frontend/winsys_handle.h"
#include "drm-uapi/drm_fourcc.h"  /* DRM_FORMAT_MOD_INVALID */
#include "WinMaliEscape.h"        /* WINMALI_ALLOCATION_PRIVATE */
#endif


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateResourceSize --
 *
 *    The CalcPrivateResourceSize function determines the size of
 *    the user-mode display driver's private region of memory
 *    (that is, the size of internal driver structures, not the
 *    size of the resource video memory).
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateResourceSize(D3D10DDI_HDEVICE hDevice,                                // IN
                        __in const D3D10DDIARG_CREATERESOURCE *pCreateResource)  // IN
{
   LOG_ENTRYPOINT();

   return sizeof(Resource);
}


static pipe_resource_usage
translate_resource_usage(UINT usage)
{
   pipe_resource_usage resource_usage = PIPE_USAGE_DEFAULT;

   switch (usage) {
   case D3D10_DDI_USAGE_DEFAULT:
      resource_usage = PIPE_USAGE_DEFAULT;
      break;
   case D3D10_DDI_USAGE_IMMUTABLE:
      resource_usage = PIPE_USAGE_IMMUTABLE;
      break;
   case D3D10_DDI_USAGE_DYNAMIC:
      resource_usage = PIPE_USAGE_DYNAMIC;
      break;
   case D3D10_DDI_USAGE_STAGING:
      resource_usage = PIPE_USAGE_STAGING;
      break;
   default:
      assert(0);
      break;
   }

   return resource_usage;
}


static unsigned
translate_resource_flags(UINT flags)
{
   unsigned bind = 0;

   if (flags & D3D10_DDI_BIND_VERTEX_BUFFER)
      bind |= PIPE_BIND_VERTEX_BUFFER;

   if (flags & D3D10_DDI_BIND_INDEX_BUFFER)
      bind |= PIPE_BIND_INDEX_BUFFER;

   if (flags & D3D10_DDI_BIND_CONSTANT_BUFFER)
      bind |= PIPE_BIND_CONSTANT_BUFFER;

   if (flags & D3D10_DDI_BIND_SHADER_RESOURCE)
      bind |= PIPE_BIND_SAMPLER_VIEW;

   if (flags & D3D10_DDI_BIND_RENDER_TARGET)
      bind |= PIPE_BIND_RENDER_TARGET;

   if (flags & D3D10_DDI_BIND_DEPTH_STENCIL)
      bind |= PIPE_BIND_DEPTH_STENCIL;

   if (flags & D3D10_DDI_BIND_STREAM_OUTPUT)
      bind |= PIPE_BIND_STREAM_OUTPUT;

   if (flags & D3D10_DDI_BIND_PRESENT)
      bind |= PIPE_BIND_DISPLAY_TARGET;

   return bind;
}


static enum pipe_texture_target
translate_texture_target( D3D10DDIRESOURCE_TYPE ResourceDimension,
                             UINT ArraySize)
{
   assert(ArraySize >= 1);
   switch(ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      assert(ArraySize == 1);
      return PIPE_BUFFER;
   case D3D10DDIRESOURCE_TEXTURE1D:
      return ArraySize > 1 ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
   case D3D10DDIRESOURCE_TEXTURE2D:
      return ArraySize > 1 ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
   case D3D10DDIRESOURCE_TEXTURE3D:
      assert(ArraySize == 1);
      return PIPE_TEXTURE_3D;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      assert(ArraySize % 6 == 0);
      return ArraySize > 6 ? PIPE_TEXTURE_CUBE_ARRAY : PIPE_TEXTURE_CUBE;
   default:
      assert(0);
      return PIPE_TEXTURE_1D;
   }
}


static void
subResourceBox(struct pipe_resource *resource, // IN
                 UINT SubResource,  // IN
                 unsigned *pLevel, // OUT
                 struct pipe_box *pBox)   // OUT
{
   UINT MipLevels = resource->last_level + 1;
   unsigned layer;
   unsigned width;
   unsigned height;
   unsigned depth;

   *pLevel = SubResource % MipLevels;
   layer = SubResource / MipLevels;

   width  = u_minify(resource->width0,  *pLevel);
   height = u_minify(resource->height0, *pLevel);
   depth  = u_minify(resource->depth0,  *pLevel);

   pBox->x = 0;
   pBox->y = 0;
   pBox->z = 0 + layer;
   pBox->width  = width;
   pBox->height = height;
   pBox->depth  = depth;
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateResource --
 *
 *    The CreateResource function creates a resource.
 *
 * ----------------------------------------------------------------------
 */

static bool
WinMaliIsGlyphFormat(enum pipe_format format)
{
   return format == PIPE_FORMAT_A8_UNORM || format == PIPE_FORMAT_R8_UNORM;
}

/* Deliberately sample rather than scan the whole atlas.  This is diagnostic
 * evidence that coverage bytes reached the UMD, not a content checksum. */
static uint32_t
WinMaliGlyphSampleHash(const void *data, unsigned rowPitch,
                       unsigned width, unsigned height)
{
   if (!data || !width || !height)
      return 0;

   const uint8_t *bytes = (const uint8_t *)data;
   uint32_t hash = 2166136261u;
   const unsigned rows = MIN2(height, 16u);
   const unsigned columns = MIN2(width, 64u);

   for (unsigned y = 0; y < rows; ++y) {
      const unsigned sourceY = (uint64_t)y * height / rows;
      const uint8_t *row = bytes + (size_t)sourceY * rowPitch;
      for (unsigned x = 0; x < columns; ++x) {
         const unsigned sourceX = (uint64_t)x * width / columns;
         hash = (hash ^ row[sourceX]) * 16777619u;
      }
   }

   return hash;
}

/* Diagnostic only: characterize small synchronized texture readbacks after
 * Gallium has flushed and waited for the GPU writer.  Scan logical row bytes
 * only (never row padding) and cap the total size so this does not turn normal
 * desktop operation into another high-volume trace. */
static void
WinMaliLogSmallReadback(const struct pipe_resource *resource,
                        const struct pipe_box *box, unsigned level,
                        const void *data, unsigned rowPitch)
{
   if (!resource || !box || !data || resource->target == PIPE_BUFFER ||
       box->width <= 0 || box->height <= 0 || box->depth != 1)
      return;

   const unsigned rowBytes =
      util_format_get_stride(resource->format, box->width);
   const unsigned rows =
      util_format_get_nblocksy(resource->format, box->height);
   const uint64_t logicalBytes = (uint64_t)rowBytes * rows;
   if (!rowBytes || !rows || rowPitch < rowBytes ||
       logicalBytes > 256u * 1024u)
      return;

   static LONG sequence;
   const LONG seq = InterlockedIncrement(&sequence);
   if (seq > 512)
      return;

   const uint8_t *bytes = (const uint8_t *)data;
   uint32_t hash = 2166136261u;
   uint64_t nonzero = 0;
   uint64_t laneSum[4] = {0, 0, 0, 0};

   for (unsigned y = 0; y < rows; ++y) {
      const uint8_t *row = bytes + (size_t)y * rowPitch;
      for (unsigned x = 0; x < rowBytes; ++x) {
         const uint8_t value = row[x];
         hash = (hash ^ value) * 16777619u;
         nonzero += value != 0;
         laneSum[x & 3] += value;
      }
   }

   DebugPrintf("[WinMaliReadback] MapData seq=%ld resource=%p format=%s "
               "level=%u box=%dx%dx%d rowPitch=%u rowBytes=%u bytes=%llu "
               "hash=0x%08x nonzero=%llu lanes=%llu,%llu,%llu,%llu\n",
               seq, resource, util_format_name(resource->format), level,
               box->width, box->height, box->depth, rowPitch, rowBytes,
               (unsigned long long)logicalBytes, hash,
               (unsigned long long)nonzero,
               (unsigned long long)laneSum[0],
               (unsigned long long)laneSum[1],
               (unsigned long long)laneSum[2],
               (unsigned long long)laneSum[3]);
}

static void
WinMaliDiagnosticProcessName(char *name, size_t nameSize)
{
   char path[MAX_PATH] = {0};
   DWORD length = GetModuleFileNameA(NULL, path, ARRAY_SIZE(path));
   const char *base = path;
   if (length) {
      const char *slash = strrchr(path, '\\');
      if (slash)
         base = slash + 1;
   }
   snprintf(name, nameSize, "%s", *base ? base : "unknown");
   for (char *p = name; *p; ++p) {
      if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
             *p == '.'))
         *p = '_';
   }
}

static bool
WinMaliDiagnosticWriteBmp(const char *process, const char *stage,
                          const void *data, unsigned width, unsigned height,
                          unsigned rowPitch, char *writtenPath,
                          size_t writtenPathSize)
{
   char temp[MAX_PATH] = {0};
   char directory[MAX_PATH] = {0};
   if (!GetTempPathA(ARRAY_SIZE(temp), temp))
      return false;

#if defined(WINMALI_SHADER_CAPTURE)
   snprintf(directory, sizeof(directory), "%sWinMaliShaderCapture", temp);
#else
   snprintf(directory, sizeof(directory), "%sWinMaliCapture", temp);
#endif
   if (!CreateDirectoryA(directory, NULL) &&
       GetLastError() != ERROR_ALREADY_EXISTS)
      return false;

   snprintf(writtenPath, writtenPathSize, "%s\\%s-%lu-%s-%ux%u.bmp",
            directory, process, GetCurrentProcessId(), stage, width, height);
   HANDLE file = CreateFileA(writtenPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return false;

   BITMAPFILEHEADER fileHeader = {0};
   BITMAPINFOHEADER infoHeader = {0};
   const DWORD imageBytes = width * height * 4u;
   fileHeader.bfType = 0x4d42;
   fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
   fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
   infoHeader.biSize = sizeof(infoHeader);
   infoHeader.biWidth = (LONG)width;
   infoHeader.biHeight = -(LONG)height; /* mapped data is top-down */
   infoHeader.biPlanes = 1;
   infoHeader.biBitCount = 32;
   infoHeader.biCompression = BI_RGB;
   infoHeader.biSizeImage = imageBytes;

   DWORD written = 0;
   bool ok = WriteFile(file, &fileHeader, sizeof(fileHeader), &written, NULL) &&
             written == sizeof(fileHeader) &&
             WriteFile(file, &infoHeader, sizeof(infoHeader), &written, NULL) &&
             written == sizeof(infoHeader);
   const uint8_t *bytes = (const uint8_t *)data;
   const DWORD rowBytes = width * 4u;
   for (unsigned y = 0; ok && y < height; ++y) {
      ok = WriteFile(file, bytes + (size_t)y * rowPitch, rowBytes,
                     &written, NULL) && written == rowBytes;
   }
   CloseHandle(file);
   return ok;
}

bool
WinMaliDiagnosticCaptureResource(struct pipe_context *pipe,
                                 struct pipe_resource *resource,
                                 const char *stage)
{
   if (!pipe || !resource || !stage || resource->target == PIPE_BUFFER ||
       resource->format != PIPE_FORMAT_B8G8R8A8_UNORM ||
       !resource->width0 || !resource->height0 || resource->depth0 != 1)
      return false;

   struct pipe_box box;
   u_box_2d(0, 0, resource->width0, resource->height0, &box);
   struct pipe_transfer *transfer = NULL;
#if defined(WINMALI_SHADER_CAPTURE)
   WINMALI_CAPTURE_TRACE("Pixel ENTER stage=%s resource=%p size=%ux%u",
                         stage, resource, resource->width0, resource->height0);
#else
   DebugPrintf("[WinMaliPixel] Capture ENTER stage=%s resource=%p size=%ux%u\n",
               stage, resource, resource->width0, resource->height0);
#endif
   void *map = pipe->texture_map(pipe, resource, 0, PIPE_MAP_READ, &box,
                                 &transfer);
   if (!map || !transfer) {
#if defined(WINMALI_SHADER_CAPTURE)
      WINMALI_CAPTURE_TRACE("Pixel EXIT stage=%s resource=%p result=map-failed",
                            stage, resource);
      WinMaliCaptureRecord("pixel stage=%s resource=%p result=map-failed",
                           stage, resource);
#else
      DebugPrintf("[WinMaliPixel] Capture EXIT stage=%s resource=%p result=map-failed\n",
                  stage, resource);
#endif
      return false;
   }

   const unsigned width = resource->width0;
   const unsigned height = resource->height0;
   uint32_t hash = 2166136261u;
   uint64_t nonzeroBytes = 0, nonzeroPixels = 0, alphaPixels = 0;
   uint64_t laneSum[4] = {0, 0, 0, 0};
   const uint8_t *bytes = (const uint8_t *)map;
   for (unsigned y = 0; y < height; ++y) {
      const uint8_t *row = bytes + (size_t)y * transfer->stride;
      for (unsigned x = 0; x < width; ++x) {
         const uint8_t *pixel = row + x * 4u;
         bool any = false;
         for (unsigned lane = 0; lane < 4; ++lane) {
            hash = (hash ^ pixel[lane]) * 16777619u;
            nonzeroBytes += pixel[lane] != 0;
            laneSum[lane] += pixel[lane];
            any |= pixel[lane] != 0;
         }
         nonzeroPixels += any;
         alphaPixels += pixel[3] != 0;
      }
   }

   char process[MAX_PATH] = {0};
   char path[MAX_PATH] = {0};
   WinMaliDiagnosticProcessName(process, sizeof(process));
   bool saved = WinMaliDiagnosticWriteBmp(process, stage, map, width, height,
                                           transfer->stride, path,
                                           sizeof(path));
#if defined(WINMALI_SHADER_CAPTURE)
   WINMALI_CAPTURE_TRACE("Pixel DATA stage=%s process=%s pid=%lu "
                         "resource=%p size=%ux%u stride=%u hash=0x%08x "
                         "nonzeroPixels=%llu alphaPixels=%llu bmp=%s saved=%u",
                         stage, process, GetCurrentProcessId(), resource,
                         width, height, transfer->stride, hash,
                         (unsigned long long)nonzeroPixels,
                         (unsigned long long)alphaPixels,
                         saved ? path : "none", saved ? 1u : 0u);
   WinMaliCaptureRecord("pixel stage=%s resource=%p size=%ux%u stride=%u "
                        "hash=0x%08x nonzeroBytes=%llu nonzeroPixels=%llu "
                        "alphaPixels=%llu lanes=%llu,%llu,%llu,%llu "
                        "bmp=%s saved=%u",
                        stage, resource, width, height, transfer->stride, hash,
                        (unsigned long long)nonzeroBytes,
                        (unsigned long long)nonzeroPixels,
                        (unsigned long long)alphaPixels,
                        (unsigned long long)laneSum[0],
                        (unsigned long long)laneSum[1],
                        (unsigned long long)laneSum[2],
                        (unsigned long long)laneSum[3], saved ? path : "none",
                        saved ? 1u : 0u);
#else
   DebugPrintf("[WinMaliPixel] Capture DATA stage=%s process=%s pid=%lu "
               "resource=%p size=%ux%u stride=%u hash=0x%08x "
               "nonzeroBytes=%llu nonzeroPixels=%llu alphaPixels=%llu "
               "lanes=%llu,%llu,%llu,%llu bmp=%s saved=%u\n",
               stage, process, GetCurrentProcessId(), resource, width, height,
               transfer->stride, hash,
               (unsigned long long)nonzeroBytes,
               (unsigned long long)nonzeroPixels,
               (unsigned long long)alphaPixels,
               (unsigned long long)laneSum[0],
               (unsigned long long)laneSum[1],
               (unsigned long long)laneSum[2],
               (unsigned long long)laneSum[3], saved ? path : "none",
               saved ? 1u : 0u);
#endif

   pipe->texture_unmap(pipe, transfer);
#if defined(WINMALI_SHADER_CAPTURE)
   WINMALI_CAPTURE_TRACE("Pixel EXIT stage=%s resource=%p result=success",
                         stage, resource);
#else
   DebugPrintf("[WinMaliPixel] Capture EXIT stage=%s resource=%p result=success\n",
               stage, resource);
#endif
   return true;
}

#if defined(WINMALI_SHADER_CAPTURE)
bool
WinMaliCaptureShaderInput(struct pipe_context *pipe,
                          struct pipe_sampler_view *view,
                          const char *stage)
{
   if (!pipe || !view || !view->texture || !stage)
      return false;

   struct pipe_resource *resource = view->texture;
   if (view->format == PIPE_FORMAT_B8G8R8A8_UNORM &&
       resource->target != PIPE_BUFFER)
      return WinMaliDiagnosticCaptureResource(pipe, resource, stage);

   if (view->format != PIPE_FORMAT_R8_UINT ||
       resource->target != PIPE_BUFFER)
      return false;

   const unsigned offset = view->u.buf.offset;
   unsigned size = view->u.buf.size;
   if (offset >= resource->width0)
      return false;
   if (!size || size > resource->width0 - offset)
      size = resource->width0 - offset;

   struct pipe_transfer *transfer = NULL;
   const uint8_t *map = (const uint8_t *)pipe_buffer_map_range(
      pipe, resource, offset, size, PIPE_MAP_READ, &transfer);
   if (!map || !transfer) {
      WINMALI_CAPTURE_TRACE("Input EXIT stage=%s format=R8_UINT "
                            "offset=%u size=%u result=map-failed",
                            stage, offset, size);
      WinMaliCaptureRecord("input stage=%s format=R8_UINT offset=%u "
                           "size=%u result=map-failed",
                           stage, offset, size);
      return false;
   }

   uint64_t hash = UINT64_C(1469598103934665603);
   uint64_t nonzero = 0, sum = 0;
   for (unsigned i = 0; i < size; ++i) {
      hash = (hash ^ map[i]) * UINT64_C(1099511628211);
      nonzero += map[i] != 0;
      sum += map[i];
   }

   char temp[MAX_PATH] = {0}, directory[MAX_PATH] = {0};
   char process[MAX_PATH] = {0}, path[MAX_PATH] = {0};
   bool saved = false;
   if (GetTempPathA(ARRAY_SIZE(temp), temp)) {
      snprintf(directory, sizeof(directory), "%sWinMaliShaderCapture", temp);
      if (CreateDirectoryA(directory, NULL) ||
          GetLastError() == ERROR_ALREADY_EXISTS) {
         WinMaliDiagnosticProcessName(process, sizeof(process));
         snprintf(path, sizeof(path), "%s\\%s-%lu-%s.bin", directory,
                  process, GetCurrentProcessId(), stage);
         HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
         if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            saved = size <= MAXDWORD &&
                    WriteFile(file, map, (DWORD)size, &written, NULL) &&
                    written == size;
            CloseHandle(file);
         }
      }
   }

   pipe_buffer_unmap(pipe, transfer);
   WINMALI_CAPTURE_TRACE("Input DATA stage=%s format=R8_UINT offset=%u "
                         "size=%u hash=%016llx nonzero=%llu sum=%llu "
                         "file=%s saved=%u",
                         stage, offset, size, (unsigned long long)hash,
                         (unsigned long long)nonzero, (unsigned long long)sum,
                         saved ? path : "none", saved ? 1u : 0u);
   WinMaliCaptureRecord("input stage=%s format=R8_UINT offset=%u size=%u "
                        "hash=%016llx nonzero=%llu sum=%llu file=%s saved=%u",
                        stage, offset, size, (unsigned long long)hash,
                        (unsigned long long)nonzero, (unsigned long long)sum,
                        saved ? path : "none", saved ? 1u : 0u);
   return saved;
}

bool
WinMaliCaptureBufferRange(struct pipe_context *pipe,
                          struct pipe_resource *resource,
                          unsigned offset, unsigned size,
                          const char *stage)
{
   if (!pipe || !resource || resource->target != PIPE_BUFFER || !stage ||
       offset >= resource->width0)
      return false;

   size = MIN2(size, resource->width0 - offset);
   if (!size)
      return false;

   struct pipe_transfer *transfer = NULL;
   const uint8_t *map = (const uint8_t *)pipe_buffer_map_range(
      pipe, resource, offset, size, PIPE_MAP_READ, &transfer);
   if (!map || !transfer) {
      WINMALI_CAPTURE_TRACE("Buffer result=map-failed stage=%s resource=%p "
                            "offset=%u size=%u", stage, resource, offset,
                            size);
      WinMaliCaptureRecord("buffer stage=%s resource=%p offset=%u size=%u "
                           "result=map-failed", stage, resource, offset,
                           size);
      return false;
   }

   uint64_t hash = UINT64_C(1469598103934665603);
   uint64_t nonzero = 0;
   for (unsigned i = 0; i < size; ++i) {
      hash = (hash ^ map[i]) * UINT64_C(1099511628211);
      nonzero += map[i] != 0;
   }

   char temp[MAX_PATH] = {0}, directory[MAX_PATH] = {0};
   char process[MAX_PATH] = {0}, path[MAX_PATH] = {0};
   bool saved = false;
   if (GetTempPathA(ARRAY_SIZE(temp), temp)) {
      snprintf(directory, sizeof(directory), "%sWinMaliShaderCapture", temp);
      if (CreateDirectoryA(directory, NULL) ||
          GetLastError() == ERROR_ALREADY_EXISTS) {
         WinMaliDiagnosticProcessName(process, sizeof(process));
         snprintf(path, sizeof(path), "%s\\%s-%lu-%s.bin", directory,
                  process, GetCurrentProcessId(), stage);
         HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
         if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            saved = WriteFile(file, map, size, &written, NULL) &&
                    written == size;
            CloseHandle(file);
         }
      }
   }

   pipe_buffer_unmap(pipe, transfer);
   WINMALI_CAPTURE_TRACE("Buffer stage=%s resource=%p offset=%u size=%u "
                         "hash=%016llx nonzero=%llu file=%s saved=%u",
                         stage, resource, offset, size,
                         (unsigned long long)hash,
                         (unsigned long long)nonzero,
                         saved ? path : "none", saved ? 1u : 0u);
   WinMaliCaptureRecord("buffer stage=%s resource=%p offset=%u size=%u "
                        "hash=%016llx nonzero=%llu file=%s saved=%u",
                        stage, resource, offset, size,
                        (unsigned long long)hash,
                        (unsigned long long)nonzero,
                        saved ? path : "none", saved ? 1u : 0u);
   return saved;
}
#endif

void APIENTRY
CreateResource(D3D10DDI_HDEVICE hDevice,                                // IN
               __in const D3D10DDIARG_CREATERESOURCE *pCreateResource,  // IN
               D3D10DDI_HRESOURCE hResource,                            // IN
               D3D10DDI_HRTRESOURCE hRTResource)                        // IN
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] CreateResource ENTER device=%p resource=%p rtResource=%p dim=%u usage=%u bind=0x%x map=0x%x misc=0x%x format=%u size=%ux%ux%u mips=%u array=%u primary=%p\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate,
               (void *)(uintptr_t)hRTResource.handle,
               pCreateResource->ResourceDimension, pCreateResource->Usage,
               pCreateResource->BindFlags, pCreateResource->MapFlags,
               pCreateResource->MiscFlags, pCreateResource->Format,
               pCreateResource->pMipInfoList[0].TexelWidth,
               pCreateResource->pMipInfoList[0].TexelHeight,
               pCreateResource->pMipInfoList[0].TexelDepth,
               pCreateResource->MipLevels, pCreateResource->ArraySize,
               pCreateResource->pPrimaryDesc);

   if ((pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) ||
       (pCreateResource->pPrimaryDesc &&
        pCreateResource->pPrimaryDesc->DriverFlags & DXGI_DDI_PRIMARY_OPTIONAL)) {

      DebugPrintf("%s(%dx%dx%d hResource=%p)\n",
	       __func__,
	       pCreateResource->pMipInfoList[0].TexelWidth,
	       pCreateResource->pMipInfoList[0].TexelHeight,
	       pCreateResource->pMipInfoList[0].TexelDepth,
	       hResource.pDrvPrivate);
      DebugPrintf("  ResourceDimension = %u\n",
	       pCreateResource->ResourceDimension);
      DebugPrintf("  Usage = %u\n",
	       pCreateResource->Usage);
      DebugPrintf("  BindFlags = 0x%x\n",
	       pCreateResource->BindFlags);
      DebugPrintf("  MapFlags = 0x%x\n",
	       pCreateResource->MapFlags);
      DebugPrintf("  MiscFlags = 0x%x\n",
	       pCreateResource->MiscFlags);
      DebugPrintf("  Format = %s\n",
	       FormatToName(pCreateResource->Format));
      DebugPrintf("  SampleDesc.Count = %u\n", pCreateResource->SampleDesc.Count);
      DebugPrintf("  SampleDesc.Quality = %u\n", pCreateResource->SampleDesc.Quality);
      DebugPrintf("  MipLevels = %u\n", pCreateResource->MipLevels);
      DebugPrintf("  ArraySize = %u\n", pCreateResource->ArraySize);
      DebugPrintf("  pPrimaryDesc = %p\n", pCreateResource->pPrimaryDesc);
      if (pCreateResource->pPrimaryDesc) {
	 DebugPrintf("    Flags = 0x%x\n",
		  pCreateResource->pPrimaryDesc->Flags);
	 DebugPrintf("    VidPnSourceId = %u\n", pCreateResource->pPrimaryDesc->VidPnSourceId);
	 DebugPrintf("    ModeDesc.Width = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.Width);
	 DebugPrintf("    ModeDesc.Height = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.Height);
	 DebugPrintf("    ModeDesc.Format = %u)\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Format);
	 DebugPrintf("    ModeDesc.RefreshRate.Numerator = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.RefreshRate.Numerator);
	 DebugPrintf("    ModeDesc.RefreshRate.Denominator = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.RefreshRate.Denominator);
	 DebugPrintf("    ModeDesc.ScanlineOrdering = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.ScanlineOrdering);
	 DebugPrintf("    ModeDesc.Rotation = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Rotation);
	 DebugPrintf("    ModeDesc.Scaling = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Scaling);
	 DebugPrintf("    DriverFlags = 0x%x\n",
		  pCreateResource->pPrimaryDesc->DriverFlags);
      }

   }

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   Resource *pResource = CastResource(hResource);

   memset(pResource, 0, sizeof *pResource);

#if 0
   if (pCreateResource->pPrimaryDesc) {
      pCreateResource->pPrimaryDesc->DriverFlags = DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT;
      if (!(pCreateResource->pPrimaryDesc->DriverFlags & DXGI_DDI_PRIMARY_OPTIONAL)) {
         // http://msdn.microsoft.com/en-us/library/windows/hardware/ff568846.aspx
         SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
         return;
      }
   }
#endif

   pResource->Format = pCreateResource->Format;
   pResource->MipLevels = pCreateResource->MipLevels;

   struct pipe_resource templat;

   memset(&templat, 0, sizeof templat);

   templat.target     = translate_texture_target( pCreateResource->ResourceDimension,
                                                  pCreateResource->ArraySize );
   pResource->buffer = templat.target == PIPE_BUFFER;

   if (pCreateResource->Format == DXGI_FORMAT_UNKNOWN) {
      assert(pCreateResource->ResourceDimension == D3D10DDIRESOURCE_BUFFER);
      templat.format = PIPE_FORMAT_R8_UINT;
   } else {
      BOOL bindDepthStencil = !!(pCreateResource->BindFlags & D3D10_DDI_BIND_DEPTH_STENCIL);
      templat.format = FormatTranslate(pCreateResource->Format, bindDepthStencil);
   }

   pResource->glyphDiagnostic = WinMaliIsGlyphFormat(templat.format);
   if (pResource->glyphDiagnostic) {
      DebugPrintf("[WinMaliGlyph] ResourceCreate drv=%p dxgi=%u pipe=%s size=%ux%u "
                  "mips=%u array=%u usage=%u bind=0x%x map=0x%x initial=%u\n",
                  hResource.pDrvPrivate, pCreateResource->Format,
                  util_format_name(templat.format),
                  pCreateResource->pMipInfoList[0].TexelWidth,
                  pCreateResource->pMipInfoList[0].TexelHeight,
                  pCreateResource->MipLevels, pCreateResource->ArraySize,
                  pCreateResource->Usage, pCreateResource->BindFlags,
                  pCreateResource->MapFlags,
                  pCreateResource->pInitialDataUP ? 1u : 0u);
   }

   templat.width0     = pCreateResource->pMipInfoList[0].TexelWidth;
   templat.height0    = pCreateResource->pMipInfoList[0].TexelHeight;
   templat.depth0     = pCreateResource->pMipInfoList[0].TexelDepth;
   templat.array_size = pCreateResource->ArraySize;
   templat.last_level = pCreateResource->MipLevels - 1;
   templat.nr_samples = pCreateResource->SampleDesc.Count;
   templat.nr_storage_samples = pCreateResource->SampleDesc.Count;
   templat.bind       = translate_resource_flags(pCreateResource->BindFlags);
   templat.usage      = translate_resource_usage(pCreateResource->Usage);

   if (templat.target != PIPE_BUFFER) {
      if (!screen->is_format_supported(screen,
                                       templat.format,
                                       templat.target,
                                       templat.nr_samples,
                                       templat.nr_storage_samples,
                                       templat.bind)) {
         debug_printf("%s: unsupported format %s\n",
                     __func__, util_format_name(templat.format));
         SetError(hDevice, E_OUTOFMEMORY);
         return;
      }
   }

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   /*
    * WinMali D3D11 backbuffer / shared-resource hook.
    *
    * DXGI swap-chain backbuffers and cross-process shared textures must
    * live as canonical D3DKMTCreateAllocation handles because the
    * desktop compositor reads from kernel-tracked allocations only —
    * escape-managed BOs are invisible to DWM. For these resources:
    *
    *   1. pfnAllocateCb asks the kernel runtime for a real D3DKMT
    *      allocation, with WINMALI_ALLOCATION_PRIVATE telling the KMD
    *      how many bytes of physical backing to reserve.
    *   2. The returned D3DKMT_HANDLE is handed to panfrost via
    *      screen->resource_from_handle(WINSYS_HANDLE_TYPE_WIN32_KMT),
    *      which calls into panfrost_winmali_winsys -> BoFromAllocation
    *      (op 0x81) to wrap the kernel allocation as a panfrost BO.
    *   3. panfrost renders into that BO; DWM composites from the same
    *      kernel allocation. No CPU copy needed.
    */
   /* A DXGI swap-chain backbuffer carries D3D10_DDI_BIND_PRESENT but is
    * neither MISC_SHARED nor a primary; it STILL must be a kernel-tracked
    * D3DKMT allocation, because _Present hands its hKMAllocation to the
    * runtime's pfnPresentCb and DWM composites from that allocation. Without
    * this, hKMAllocation stays 0, _Present falls back to the GL PresentToHdc
    * escape (invisible to DWM) and the window shows white. So any presentable
    * surface gets a real allocation the GPU renders into via BoFromAllocation. */
   bool isPrimaryOrShared =
      (pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) ||
      (pCreateResource->BindFlags & D3D10_DDI_BIND_PRESENT) ||
      (pCreateResource->pPrimaryDesc &&
       (pCreateResource->pPrimaryDesc->DriverFlags & DXGI_DDI_PRIMARY_OPTIONAL));

   if (isPrimaryOrShared && templat.target != PIPE_BUFFER) {
      DebugPrintf("%s: presentable/shared resource -> creating KM allocation "
                  "(bind=0x%x misc=0x%x primary=%p)\n", __func__,
                  pCreateResource->BindFlags, pCreateResource->MiscFlags,
                  pCreateResource->pPrimaryDesc);
      Device *pDevice = CastDevice(hDevice);
      unsigned bpp = util_format_get_blocksize(templat.format);
      if (bpp == 0) {
         DebugPrintf("%s: cannot import backbuffer with zero-block format %s\n",
                     __func__, util_format_name(templat.format));
         SetError(hDevice, E_INVALIDARG);
         return;
      }

      /* Linear layout for compositor-visible surfaces. */
      unsigned stride = templat.width0 * bpp;
      uint64_t allocSize = (uint64_t)stride * templat.height0;

      WINMALI_ALLOCATION_PRIVATE allocPriv;
      memset(&allocPriv, 0, sizeof allocPriv);
      allocPriv.Magic  = WINMALI_ALLOCATION_PRIVATE_MAGIC;
      allocPriv.Size   = allocSize;
      allocPriv.Width  = templat.width0;
      allocPriv.Height = templat.height0;
      allocPriv.Format = pCreateResource->Format;
      allocPriv.Stride = stride;

      D3DDDI_ALLOCATIONINFO allocInfo;
      memset(&allocInfo, 0, sizeof allocInfo);
      allocInfo.pPrivateDriverData    = &allocPriv;
      allocInfo.PrivateDriverDataSize = sizeof allocPriv;
      if (pCreateResource->pPrimaryDesc) {
         allocInfo.Flags.Primary = 1;
         allocInfo.VidPnSourceId = pCreateResource->pPrimaryDesc->VidPnSourceId;
      }

      D3DDDICB_ALLOCATE allocCb;
      memset(&allocCb, 0, sizeof allocCb);
      allocCb.hResource       = hRTResource.handle;
      allocCb.NumAllocations  = 1;
      allocCb.pAllocationInfo = &allocInfo;

      HRESULT hr = pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &allocCb);
      if (FAILED(hr) || allocInfo.hAllocation == 0) {
         DebugPrintf("%s: pfnAllocateCb failed 0x%08x\n", __func__, hr);
         SetError(hDevice, FAILED(hr) ? hr : E_OUTOFMEMORY);
         return;
      }

      pResource->ownsKMAllocation = true;
      pResource->hRTResource      = hRTResource.handle;
      pResource->hKMResource      = allocCb.hKMResource;
      pResource->hKMAllocation    = allocInfo.hAllocation;

      /* GPU residency and CPU locking are distinct WDDM contracts. LockCb
       * supplies the persistent CPU mapping, but does not put the allocation
       * on this device's GPU residency list. Without MakeResident, VidMm can
       * complete MapGpuVirtualAddress after installing only invalid leaf PTEs.
       * Hold both references until DestroyResource. */
      if (WinMaliWddmEnsurePagingQueue(pDevice) != 0 ||
          pDevice->KTCallbacks.pfnMakeResidentCb == NULL) {
         DebugPrintf("%s: MakeResident unavailable alloc=%u\n", __func__,
                     pResource->hKMAllocation);
         D3DDDICB_DEALLOCATE dealloc;
         memset(&dealloc, 0, sizeof dealloc);
         dealloc.hResource = pResource->hRTResource;
         pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
         pResource->ownsKMAllocation = false;
         pResource->hRTResource      = NULL;
         pResource->hKMResource      = 0;
         pResource->hKMAllocation    = 0;
         SetError(hDevice, E_NOINTERFACE);
         return;
      }

      D3DKMT_HANDLE residentAllocation = pResource->hKMAllocation;
      D3DDDI_MAKERESIDENT resident;
      memset(&resident, 0, sizeof resident);
      resident.hPagingQueue   = pDevice->hPagingQueue;
      resident.NumAllocations = 1;
      resident.AllocationList = &residentAllocation;
      hr = pDevice->KTCallbacks.pfnMakeResidentCb(pDevice->hDevice,
                                                   &resident);
      if ((FAILED(hr) && hr != E_PENDING) || resident.NumAllocations != 1 ||
          WinMaliWddmWaitPagingFence(pDevice, resident.PagingFenceValue,
                                     "Resource MakeResident",
                                     pResource->hKMAllocation) != 0) {
         DebugPrintf("%s: MakeResident failed alloc=%u hr=0x%08x made=%u fence=%llu trim=%llu\n",
                     __func__, pResource->hKMAllocation, hr,
                     resident.NumAllocations,
                     (unsigned long long)resident.PagingFenceValue,
                     (unsigned long long)resident.NumBytesToTrim);
         if (resident.NumAllocations != 0)
            WinMaliWddmEvictAllocation(pDevice, pResource->hKMAllocation);
         D3DDDICB_DEALLOCATE dealloc;
         memset(&dealloc, 0, sizeof dealloc);
         dealloc.hResource = pResource->hRTResource;
         pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
         pResource->ownsKMAllocation = false;
         pResource->hRTResource      = NULL;
         pResource->hKMResource      = 0;
         pResource->hKMAllocation    = 0;
         SetError(hDevice, FAILED(hr) && hr != E_PENDING ? hr : E_FAIL);
         return;
      }
      pResource->kmAllocationResident = true;
      DebugPrintf("%s: KM allocation resident=%u fence=%llu\n", __func__,
                  pResource->hKMAllocation,
                  (unsigned long long)resident.PagingFenceValue);

      void *lockedCpuAddress = NULL;
      {
         D3DDDICB_LOCK lockCb;
         memset(&lockCb, 0, sizeof lockCb);
         lockCb.hAllocation = pResource->hKMAllocation;
         lockCb.Flags.LockEntire = 1;
         hr = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lockCb);
         if (FAILED(hr) || lockCb.pData == NULL) {
            HRESULT mapHr = FAILED(hr) ? hr : E_OUTOFMEMORY;
            DebugPrintf("%s: pfnLockCb failed/returned no mapping hr=0x%08x data=%p\n",
                        __func__, hr, lockCb.pData);
            if (SUCCEEDED(hr)) {
               D3DDDICB_UNLOCK unlockCb;
               memset(&unlockCb, 0, sizeof unlockCb);
               unlockCb.NumAllocations = 1;
               unlockCb.phAllocations = &pResource->hKMAllocation;
               pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &unlockCb);
            }
            if (pResource->kmAllocationResident) {
               WinMaliWddmEvictAllocation(pDevice,
                                          pResource->hKMAllocation);
               pResource->kmAllocationResident = false;
            }
            D3DDDICB_DEALLOCATE dealloc;
            memset(&dealloc, 0, sizeof dealloc);
            dealloc.hResource = pResource->hRTResource;
            pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
            pResource->ownsKMAllocation = false;
            pResource->hRTResource      = NULL;
            pResource->hKMResource      = 0;
            pResource->hKMAllocation    = 0;
            SetError(hDevice, mapHr);
            return;
         }
         lockedCpuAddress = lockCb.pData;
         pResource->kmAllocationLocked = true;
         DebugPrintf("%s: locked KM allocation=%u cpu=%p\n", __func__,
                     pResource->hKMAllocation, lockedCpuAddress);
      }

      struct winsys_handle whandle;
      memset(&whandle, 0, sizeof whandle);
      whandle.type     = WINSYS_HANDLE_TYPE_WIN32_KMT;
      whandle.handle   = (HANDLE)(uintptr_t)allocInfo.hAllocation;
      whandle.stride   = stride;
      whandle.offset   = 0;
      whandle.format   = templat.format;
      whandle.modifier = DRM_FORMAT_MOD_INVALID;
      /* WINSYS_HANDLE_TYPE_WIN32_KMT is WinMali-private. Carry the CPU VA
       * returned by the lifetime LockCb alongside the KMT handle so the
       * imported panfrost BO can service CPU maps without incorrectly using
       * escape BoMapCpu (the allocation is not in the escape BO table). */
      whandle.com_obj  = lockedCpuAddress;

      pResource->resource = screen->resource_from_handle(
         screen, &templat, &whandle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);

      if (!pResource->resource) {
         DebugPrintf("%s: resource_from_handle failed for DXGI backbuffer\n",
                     __func__);
         if (pResource->kmAllocationLocked) {
            D3DDDICB_UNLOCK unlockCb;
            memset(&unlockCb, 0, sizeof unlockCb);
            unlockCb.NumAllocations = 1;
            unlockCb.phAllocations  = &pResource->hKMAllocation;
            pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &unlockCb);
            pResource->kmAllocationLocked = false;
         }
         if (pResource->kmAllocationResident) {
            WinMaliWddmEvictAllocation(pDevice,
                                       pResource->hKMAllocation);
            pResource->kmAllocationResident = false;
         }
         D3DDDICB_DEALLOCATE dealloc;
         memset(&dealloc, 0, sizeof dealloc);
         dealloc.hResource = pResource->hRTResource;
         pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
         pResource->ownsKMAllocation = false;
         pResource->hRTResource      = NULL;
         pResource->hKMResource      = 0;
         pResource->hKMAllocation    = 0;
         SetError(hDevice, E_OUTOFMEMORY);
         return;
      }
   } else
#endif /* _WIN32 && HAVE_WINMALI_WDDM_KMOD */
   {
      pResource->resource = screen->resource_create(screen, &templat);
   }

   if (!pResource->resource) {
      DebugPrintf("%s: failed to create resource\n", __func__);
      SetError(hDevice, E_OUTOFMEMORY);
      return;
   }

   pResource->NumSubResources = pCreateResource->MipLevels * pCreateResource->ArraySize;
   pResource->transfers = (struct pipe_transfer **)calloc(pResource->NumSubResources,
                                                          sizeof *pResource->transfers);

   if (pCreateResource->pInitialDataUP) {
      if (pResource->buffer) {
         assert(pResource->NumSubResources == 1);
         const D3D10_DDIARG_SUBRESOURCE_UP* pInitialDataUP =
               &pCreateResource->pInitialDataUP[0];

         unsigned level;
         struct pipe_box box;
         subResourceBox(pResource->resource, 0, &level, &box);

         struct pipe_transfer *transfer;
         void *map;
         map = pipe->buffer_map(pipe,
                                pResource->resource,
                                level,
                                PIPE_MAP_WRITE |
                                PIPE_MAP_UNSYNCHRONIZED,
                                &box,
                                &transfer);
         assert(map);
         if (map) {
            memcpy(map, pInitialDataUP->pSysMem, box.width);
            pipe_buffer_unmap(pipe, transfer);
         }
      } else {
         for (UINT SubResource = 0; SubResource < pResource->NumSubResources; ++SubResource) {
            const D3D10_DDIARG_SUBRESOURCE_UP* pInitialDataUP =
                  &pCreateResource->pInitialDataUP[SubResource];

            unsigned level;
            struct pipe_box box;
            subResourceBox(pResource->resource, SubResource, &level, &box);

            if (pResource->glyphDiagnostic) {
               DebugPrintf("[WinMaliGlyph] InitialUpload drv=%p sub=%u level=%u "
                           "box=%dx%dx%d rowPitch=%u slicePitch=%u hash=0x%08x\n",
                           hResource.pDrvPrivate, SubResource, level,
                           box.width, box.height, box.depth,
                           pInitialDataUP->SysMemPitch,
                           pInitialDataUP->SysMemSlicePitch,
                           WinMaliGlyphSampleHash(pInitialDataUP->pSysMem,
                              pInitialDataUP->SysMemPitch,
                              box.width, box.height));
            }

            struct pipe_transfer *transfer;
            void *map;
            map = pipe->texture_map(pipe,
                                    pResource->resource,
                                    level,
                                    PIPE_MAP_WRITE |
                                    PIPE_MAP_UNSYNCHRONIZED,
                                    &box,
                                    &transfer);
            assert(map);
            if (map) {
               for (int z = 0; z < box.depth; ++z) {
                  uint8_t *dst = (uint8_t*)map + z*transfer->layer_stride;
                  const uint8_t *src = (const uint8_t*)pInitialDataUP->pSysMem + z*pInitialDataUP->SysMemSlicePitch;
                  util_copy_rect(dst,
                                 templat.format,
                                 transfer->stride,
                                 0, 0, box.width, box.height,
                                 src,
                                 pInitialDataUP->SysMemPitch,
                                 0, 0);
               }
               pipe_texture_unmap(pipe, transfer);
            }
         }
      }
   }
   DebugPrintf("[WinMaliFlow] CreateResource EXIT device=%p resource=%p pipeResource=%p kmResource=%u kmAllocation=%u ownsKM=%u shared=%u present=%u primary=%u\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate, pResource->resource,
               pResource->hKMResource, pResource->hKMAllocation,
               pResource->ownsKMAllocation ? 1u : 0u,
               (pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) ? 1u : 0u,
               (pCreateResource->BindFlags & D3D10_DDI_BIND_PRESENT) ? 1u : 0u,
               pCreateResource->pPrimaryDesc ? 1u : 0u);
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateOpenedResourceSize --
 *
 *    The CalcPrivateOpenedResourceSize function determines the size
 *    of the user-mode display driver's private shared region of memory
 *    (that is, the size of internal driver structures, not the size
 *    of the resource video memory) for an opened resource.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE hDevice,                             // IN
                              __in const D3D10DDIARG_OPENRESOURCE *pOpenResource)   // IN
{
   return sizeof(Resource);
}


/*
 * ----------------------------------------------------------------------
 *
 * OpenResource --
 *
 *    The OpenResource function opens a shared resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
OpenResource(D3D10DDI_HDEVICE hDevice,                            // IN
             __in const D3D10DDIARG_OPENRESOURCE *pOpenResource,  // IN
             D3D10DDI_HRESOURCE hResource,                        // IN
             D3D10DDI_HRTRESOURCE hRTResource)                    // IN
{
   LOG_ENTRYPOINT();
   DebugPrintf("[WinMaliFlow] OpenResource ENTER device=%p resource=%p rtResource=%p allocations=%u privateSize=%u\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate,
               (void *)(uintptr_t)hRTResource.handle,
               pOpenResource ? pOpenResource->NumAllocations : 0,
               pOpenResource ? pOpenResource->PrivateDriverDataSize : 0);

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   Resource *pResource = CastResource(hResource);
   Device *pDevice = CastDevice(hDevice);
   struct pipe_screen *screen = CastPipeContext(hDevice)->screen;

   if (!pOpenResource || !hResource.pDrvPrivate ||
       pOpenResource->NumAllocations != 1 ||
       !pOpenResource->pOpenAllocationInfo ||
       pOpenResource->pOpenAllocationInfo[0].hAllocation == 0) {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT invalid arguments/open-allocation count\n");
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   const D3DDDI_OPENALLOCATIONINFO *openInfo =
      &pOpenResource->pOpenAllocationInfo[0];
   const void *privateData = openInfo->pPrivateDriverData;
   UINT privateSize = openInfo->PrivateDriverDataSize;
   uint32_t magic = 0;
   uint32_t width = 0, height = 0, pitch = 0, format = 0, usage = 0;
   uint64_t allocationSize = 0;

   if (privateData && privateSize >= sizeof magic)
      memcpy(&magic, privateData, sizeof magic);

   if (magic == WINMALI_ALLOCATION_PRIVATE_MAGIC &&
       privateSize >= sizeof(WINMALI_ALLOCATION_PRIVATE)) {
      const WINMALI_ALLOCATION_PRIVATE *priv =
         (const WINMALI_ALLOCATION_PRIVATE *)privateData;
      width = priv->Width;
      height = priv->Height;
      pitch = priv->Stride;
      format = priv->Format;
      usage = WINMALI_ALLOC_USAGE_RENDER_TARGET;
      allocationSize = priv->Size;
   } else if (magic == WINMALI_ALLOC_PRIV_MAGIC &&
              privateSize >= sizeof(WINMALI_ALLOC_PRIV)) {
      const WINMALI_ALLOC_PRIV *priv =
         (const WINMALI_ALLOC_PRIV *)privateData;
      if (priv->Version != WINMALI_ALLOC_PRIV_VERSION) {
         DebugPrintf("[WinMaliFlow] OpenResource EXIT unsupported allocation metadata version=%u\n",
                     priv->Version);
         SetError(hDevice, E_INVALIDARG);
         return;
      }
      width = priv->Width;
      height = priv->Height;
      pitch = priv->Pitch;
      format = priv->Format;
      usage = priv->Usage;
      allocationSize = priv->Size;
   } else {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT missing/unknown allocation metadata ptr=%p size=%u magic=0x%08x\n",
                  privateData, privateSize, magic);
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   enum pipe_format pipeFormat = FormatTranslate((DXGI_FORMAT)format, false);
   unsigned bytesPerPixel = util_format_get_blocksize(pipeFormat);
   uint64_t minimumPitch = (uint64_t)width * bytesPerPixel;
   uint64_t minimumSize = (uint64_t)pitch * height;
   if (!width || !height || !pitch || !bytesPerPixel ||
       pitch < minimumPitch || minimumSize > UINT64_MAX - 4095 ||
       (allocationSize && allocationSize < minimumSize)) {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT invalid metadata %ux%u pitch=%u format=%u bpp=%u size=%llu minimum=%llu\n",
                  width, height, pitch, format, bytesPerPixel,
                  (unsigned long long)allocationSize,
                  (unsigned long long)minimumSize);
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   unsigned bind = PIPE_BIND_SAMPLER_VIEW;
   if (usage & (WINMALI_ALLOC_USAGE_RENDER_TARGET |
                WINMALI_ALLOC_USAGE_PRIMARY))
      bind |= PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET;
   if (usage & WINMALI_ALLOC_USAGE_DEPTH_STENCIL)
      bind |= PIPE_BIND_DEPTH_STENCIL;

   struct pipe_resource templat;
   memset(&templat, 0, sizeof templat);
   templat.target = PIPE_TEXTURE_2D;
   templat.format = pipeFormat;
   templat.width0 = width;
   templat.height0 = height;
   templat.depth0 = 1;
   templat.array_size = 1;
   templat.last_level = 0;
   templat.nr_samples = 1;
   templat.nr_storage_samples = 1;
   templat.bind = bind;
   templat.usage = (usage & WINMALI_ALLOC_USAGE_STAGING) ?
      PIPE_USAGE_STAGING : PIPE_USAGE_DEFAULT;

   if (!screen->is_format_supported(screen, templat.format, templat.target,
                                    templat.nr_samples,
                                    templat.nr_storage_samples,
                                    templat.bind)) {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT unsupported format=%u pipeFormat=%u bind=0x%x\n",
                  format, pipeFormat, bind);
      SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
      return;
   }

   memset(pResource, 0, sizeof *pResource);
   pResource->Format = (DXGI_FORMAT)format;
   pResource->MipLevels = 1;
   pResource->NumSubResources = 1;
   pResource->buffer = false;
   pResource->hRTResource = hRTResource.handle;
   pResource->hKMResource = pOpenResource->hKMResource.handle;
   pResource->hKMAllocation = openInfo->hAllocation;
   pResource->ownsKMAllocation = false;

   HRESULT hr = E_FAIL;
   D3DKMT_HANDLE residentAllocation = pResource->hKMAllocation;
   D3DDDI_MAKERESIDENT resident;
   D3DDDICB_LOCK lockCb;
   struct winsys_handle whandle;
   memset(&resident, 0, sizeof resident);
   memset(&lockCb, 0, sizeof lockCb);
   memset(&whandle, 0, sizeof whandle);

   if (WinMaliWddmEnsurePagingQueue(pDevice) != 0 ||
       !pDevice->KTCallbacks.pfnMakeResidentCb ||
       !pDevice->KTCallbacks.pfnLockCb) {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT required residency/lock callback unavailable\n");
      SetError(hDevice, E_NOINTERFACE);
      goto fail;
   }

   resident.hPagingQueue = pDevice->hPagingQueue;
   resident.NumAllocations = 1;
   resident.AllocationList = &residentAllocation;
   hr = pDevice->KTCallbacks.pfnMakeResidentCb(pDevice->hDevice, &resident);
   if ((FAILED(hr) && hr != E_PENDING) || resident.NumAllocations != 1 ||
       WinMaliWddmWaitPagingFence(pDevice, resident.PagingFenceValue,
                                  "OpenResource MakeResident",
                                  pResource->hKMAllocation) != 0) {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT MakeResident failed alloc=%u hr=0x%08x made=%u fence=%llu\n",
                  pResource->hKMAllocation, hr, resident.NumAllocations,
                  (unsigned long long)resident.PagingFenceValue);
      if (resident.NumAllocations != 0)
         WinMaliWddmEvictAllocation(pDevice, pResource->hKMAllocation);
      SetError(hDevice, FAILED(hr) && hr != E_PENDING ? hr : E_FAIL);
      goto fail;
   }
   pResource->kmAllocationResident = true;

   lockCb.hAllocation = pResource->hKMAllocation;
   lockCb.Flags.LockEntire = 1;
   hr = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lockCb);
   if (FAILED(hr) || !lockCb.pData) {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT Lock failed alloc=%u hr=0x%08x data=%p\n",
                  pResource->hKMAllocation, hr, lockCb.pData);
      SetError(hDevice, FAILED(hr) ? hr : E_OUTOFMEMORY);
      goto fail;
   }
   pResource->kmAllocationLocked = true;

   whandle.type = WINSYS_HANDLE_TYPE_WIN32_KMT;
   whandle.handle = (HANDLE)(uintptr_t)pResource->hKMAllocation;
   whandle.stride = pitch;
   whandle.offset = 0;
   whandle.format = pipeFormat;
   whandle.modifier = DRM_FORMAT_MOD_INVALID;
   whandle.com_obj = lockCb.pData;

   pResource->resource = screen->resource_from_handle(
      screen, &templat, &whandle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   if (!pResource->resource) {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT resource_from_handle failed alloc=%u\n",
                  pResource->hKMAllocation);
      SetError(hDevice, E_OUTOFMEMORY);
      goto fail;
   }

   pResource->transfers = (struct pipe_transfer **)calloc(
      pResource->NumSubResources, sizeof *pResource->transfers);
   if (!pResource->transfers) {
      DebugPrintf("[WinMaliFlow] OpenResource EXIT transfer table allocation failed\n");
      SetError(hDevice, E_OUTOFMEMORY);
      goto fail;
   }

   DebugPrintf("[WinMaliFlow] OpenResource EXIT success device=%p resource=%p pipeResource=%p kmResource=%u kmAllocation=%u %ux%u pitch=%u format=%u usage=0x%x cpu=%p\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate,
               pResource->resource, pResource->hKMResource,
               pResource->hKMAllocation, width, height, pitch, format, usage,
               lockCb.pData);
   return;

fail:
   pipe_resource_reference(&pResource->resource, NULL);
   if (pResource->kmAllocationLocked) {
      D3DDDICB_UNLOCK unlockCb;
      memset(&unlockCb, 0, sizeof unlockCb);
      unlockCb.NumAllocations = 1;
      unlockCb.phAllocations = &pResource->hKMAllocation;
      pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &unlockCb);
      pResource->kmAllocationLocked = false;
   }
   if (pResource->kmAllocationResident) {
      WinMaliWddmEvictAllocation(pDevice, pResource->hKMAllocation);
      pResource->kmAllocationResident = false;
   }
   pResource->hKMAllocation = 0;
   pResource->hKMResource = 0;
   pResource->hRTResource = NULL;
#else
   SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
#endif
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyResource --
 *
 *    The DestroyResource function destroys the specified resource
 *    object. The resource object can be destoyed only if it is not
 *    currently bound to a display device, and if all views that
 *    refer to the resource are also destroyed.
 *
 * ----------------------------------------------------------------------
 */


void APIENTRY
DestroyResource(D3D10DDI_HDEVICE hDevice,       // IN
                D3D10DDI_HRESOURCE hResource)   // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);
   DebugPrintf("[WinMaliFlow] DestroyResource ENTER device=%p resource=%p pipeResource=%p kmResource=%u kmAllocation=%u ownsKM=%u\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate, pResource->resource,
               pResource->hKMResource, pResource->hKMAllocation,
               pResource->ownsKMAllocation ? 1u : 0u);

   if (pResource->so_target) {
      pipe_so_target_reference(&pResource->so_target, NULL);
   }

   for (UINT SubResource = 0;
        pResource->transfers && SubResource < pResource->NumSubResources;
        ++SubResource) {
      if (pResource->transfers[SubResource]) {
         if (pResource->buffer) {
            pipe_buffer_unmap(pipe, pResource->transfers[SubResource]);
         } else {
            pipe_texture_unmap(pipe, pResource->transfers[SubResource]);
         }
         pResource->transfers[SubResource] = NULL;
      }
   }
   free(pResource->transfers);

   pipe_resource_reference(&pResource->resource, NULL);

#if defined(_WIN32) && defined(HAVE_WINMALI_WDDM_KMOD)
   /* Release residency/CPU-map references for both allocations created by
    * this UMD and borrowed allocations imported by OpenResource. Only the
    * creator owns the final pfnDeallocateCb operation. */
   if (pResource->hKMAllocation) {
      Device *pDevice = CastDevice(hDevice);
      if (pResource->kmAllocationLocked) {
         D3DDDICB_UNLOCK unlockCb;
         memset(&unlockCb, 0, sizeof unlockCb);
         unlockCb.NumAllocations = 1;
         unlockCb.phAllocations  = &pResource->hKMAllocation;
         pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &unlockCb);
         pResource->kmAllocationLocked = false;
      }
      if (pResource->kmAllocationResident) {
         WinMaliWddmEvictAllocation(pDevice,
                                    pResource->hKMAllocation);
         pResource->kmAllocationResident = false;
      }
      if (pResource->ownsKMAllocation) {
         D3DDDICB_DEALLOCATE dealloc;
         memset(&dealloc, 0, sizeof dealloc);
         /* This allocation was created as part of hRTResource. Releasing the
          * runtime resource destroys its allocation(s) and the corresponding
          * kernel resource in one operation; individually freeing the
          * allocation left the kernel resource behind. */
         dealloc.hResource = pResource->hRTResource;
         pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &dealloc);
      }
      pResource->ownsKMAllocation = false;
      pResource->hRTResource      = NULL;
      pResource->hKMAllocation    = 0;
      pResource->hKMResource      = 0;
   }
#endif
   DebugPrintf("[WinMaliFlow] DestroyResource EXIT device=%p resource=%p\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate);
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceMap --
 *
 *    The ResourceMap function maps a subresource of a resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceMap(D3D10DDI_HDEVICE hDevice,                                // IN
            D3D10DDI_HRESOURCE hResource,                            // IN
            UINT SubResource,                                        // IN
            D3D10_DDI_MAP DDIMap,                                    // IN
            UINT Flags,                                              // IN
            __out D3D10DDI_MAPPED_SUBRESOURCE *pMappedSubResource)   // OUT
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);
   struct pipe_resource *resource = pResource->resource;
   DebugPrintf("[WinMaliFlow] ResourceMap ENTER device=%p resource=%p pipeResource=%p sub=%u map=%u flags=0x%x\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate, resource,
               SubResource, DDIMap, Flags);

   unsigned usage;
   switch (DDIMap) {
   case D3D10_DDI_MAP_READ:
      usage = PIPE_MAP_READ;
      break;
   case D3D10_DDI_MAP_READWRITE:
      usage = PIPE_MAP_READ | PIPE_MAP_WRITE;
      break;
   case D3D10_DDI_MAP_WRITE:
      usage = PIPE_MAP_WRITE;
      break;
   case D3D10_DDI_MAP_WRITE_DISCARD:
      usage = PIPE_MAP_WRITE;
      if (resource->last_level == 0 && resource->array_size == 1) {
         usage |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
      } else {
         usage |= PIPE_MAP_DISCARD_RANGE;
      }
      break;
   case D3D10_DDI_MAP_WRITE_NOOVERWRITE:
      usage = PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED;
      break;
   default:
      assert(0);
      DebugPrintf("[WinMaliFlow] ResourceMap EXIT device=%p resource=%p result=invalid-map\n",
                  hDevice.pDrvPrivate, hResource.pDrvPrivate);
      return;
   }

   assert(SubResource < pResource->NumSubResources);

   unsigned level;
   struct pipe_box box;
   subResourceBox(resource, SubResource, &level, &box);

   assert(!pResource->transfers[SubResource]);

   void *map;
   if (pResource->buffer) {
      map = pipe->buffer_map(pipe,
                             resource,
                             level,
                             usage,
                             &box,
                             &pResource->transfers[SubResource]);
   } else {
      map = pipe->texture_map(pipe,
                              resource,
                              level,
                              usage,
                              &box,
                              &pResource->transfers[SubResource]);
   }
   if (!map) {
      DebugPrintf("%s: failed to map resource\n", __func__);
      SetError(hDevice, E_FAIL);
      DebugPrintf("[WinMaliFlow] ResourceMap EXIT device=%p resource=%p result=map-failed\n",
                  hDevice.pDrvPrivate, hResource.pDrvPrivate);
      return;
   }

   pMappedSubResource->pData = map;
   pMappedSubResource->RowPitch = pResource->transfers[SubResource]->stride;
   pMappedSubResource->DepthPitch = pResource->transfers[SubResource]->layer_stride;
   if (usage & PIPE_MAP_READ) {
      WinMaliLogSmallReadback(resource, &box, level, map,
                              pMappedSubResource->RowPitch);
   }
   DebugPrintf("[WinMaliFlow] ResourceMap EXIT device=%p resource=%p data=%p rowPitch=%u depthPitch=%u\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate,
               pMappedSubResource->pData, pMappedSubResource->RowPitch,
               pMappedSubResource->DepthPitch);
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceUnmap --
 *
 *    The ResourceUnmap function unmaps a subresource of a resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceUnmap(D3D10DDI_HDEVICE hDevice,      // IN
              D3D10DDI_HRESOURCE hResource,  // IN
              UINT SubResource)              // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);

   assert(SubResource < pResource->NumSubResources);
   DebugPrintf("[WinMaliFlow] ResourceUnmap ENTER device=%p resource=%p pipeResource=%p sub=%u transfer=%p\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate, pResource->resource,
               SubResource, pResource->transfers[SubResource]);

   if (pResource->transfers[SubResource]) {
      if (pResource->buffer) {
         pipe_buffer_unmap(pipe, pResource->transfers[SubResource]);
      } else {
         pipe_texture_unmap(pipe, pResource->transfers[SubResource]);
      }
      pResource->transfers[SubResource] = NULL;
   }
   DebugPrintf("[WinMaliFlow] ResourceUnmap EXIT device=%p resource=%p\n",
               hDevice.pDrvPrivate, hResource.pDrvPrivate);
}


/*
 *----------------------------------------------------------------------
 *
 * areResourcesCompatible --
 *
 *      Check whether two resources can be safely passed to
 *      pipe_context::resource_copy_region method.
 *
 * Results:
 *      As above.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
areResourcesCompatible(const struct pipe_resource *src_resource, // IN
                       const struct pipe_resource *dst_resource) // IN
{
   if (src_resource->format == dst_resource->format) {
      /*
       * Trivial.
       */

      return true;
   } else if (src_resource->target == PIPE_BUFFER &&
              dst_resource->target == PIPE_BUFFER) {
      /*
       * Buffer resources are merely a collection of bytes.
       */

      return true;
   } else {
      /*
       * Check whether the formats are supported by
       * the resource_copy_region method.
       */

      const struct util_format_description *src_format_desc;
      const struct util_format_description *dst_format_desc;

      src_format_desc = util_format_description(src_resource->format);
      dst_format_desc = util_format_description(dst_resource->format);

      assert(src_format_desc->block.width  == dst_format_desc->block.width);
      assert(src_format_desc->block.height == dst_format_desc->block.height);
      assert(src_format_desc->block.bits   == dst_format_desc->block.bits);

      return util_is_format_compatible(src_format_desc, dst_format_desc);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceCopy --
 *
 *    The ResourceCopy function copies an entire source
 *    resource to a destination resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceCopy(D3D10DDI_HDEVICE hDevice,          // IN
             D3D10DDI_HRESOURCE hDstResource,   // IN
             D3D10DDI_HRESOURCE hSrcResource)   // IN
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] ResourceCopy ENTER device=%p dst=%p src=%p\n",
               hDevice.pDrvPrivate, hDstResource.pDrvPrivate,
               hSrcResource.pDrvPrivate);

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      DebugPrintf("[WinMaliFlow] ResourceCopy EXIT device=%p skipped=predicate\n",
                  hDevice.pDrvPrivate);
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   Resource *pSrcResource = CastResource(hSrcResource);
   struct pipe_resource *dst_resource = pDstResource->resource;
   struct pipe_resource *src_resource = pSrcResource->resource;
   bool compatible;

   assert(dst_resource->target == src_resource->target);
   assert(dst_resource->width0 == src_resource->width0);
   assert(dst_resource->height0 == src_resource->height0);
   assert(dst_resource->depth0 == src_resource->depth0);
   assert(dst_resource->last_level == src_resource->last_level);
   assert(dst_resource->array_size == src_resource->array_size);

   compatible = areResourcesCompatible(src_resource, dst_resource);

   /* could also use one 3d copy for arrays */
   for (unsigned layer = 0; layer < dst_resource->array_size; ++layer) {
      for (unsigned level = 0; level <= dst_resource->last_level; ++level) {
         struct pipe_box box;
         box.x = 0;
         box.y = 0;
         box.z = 0 + layer;
         box.width  = u_minify(dst_resource->width0,  level);
         box.height = u_minify(dst_resource->height0, level);
         box.depth  = u_minify(dst_resource->depth0,  level);

	 if (compatible) {
            pipe->resource_copy_region(pipe,
                                       dst_resource, level,
                                       0, 0, layer,
                                       src_resource, level,
                                       &box);
         } else {
            util_resource_copy_region(pipe,
                                      dst_resource, level,
                                      0, 0, layer,
                                      src_resource, level,
                                      &box);
         }
      }
   }
   DebugPrintf("[WinMaliFlow] ResourceCopy EXIT device=%p dstPipe=%p srcPipe=%p compatible=%u\n",
               hDevice.pDrvPrivate, dst_resource, src_resource,
               compatible ? 1u : 0u);
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceCopyRegion --
 *
 *    The ResourceCopyRegion function copies a source subresource
 *    region to a location on a destination subresource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceCopyRegion(D3D10DDI_HDEVICE hDevice,                // IN
                   D3D10DDI_HRESOURCE hDstResource,         // IN
                   UINT DstSubResource,                     // IN
                   UINT DstX,                               // IN
                   UINT DstY,                               // IN
                   UINT DstZ,                               // IN
                   D3D10DDI_HRESOURCE hSrcResource,         // IN
                   UINT SrcSubResource,                     // IN
                   __in_opt const D3D10_DDI_BOX *pSrcBox)   // IN (optional)
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] ResourceCopyRegion ENTER device=%p dst=%p dstSub=%u xyz=%u,%u,%u src=%p srcSub=%u box=%p\n",
               hDevice.pDrvPrivate, hDstResource.pDrvPrivate, DstSubResource,
               DstX, DstY, DstZ, hSrcResource.pDrvPrivate, SrcSubResource,
               pSrcBox);

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      DebugPrintf("[WinMaliFlow] ResourceCopyRegion EXIT device=%p skipped=predicate\n",
                  hDevice.pDrvPrivate);
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   Resource *pSrcResource = CastResource(hSrcResource);
   struct pipe_resource *dst_resource = pDstResource->resource;
   struct pipe_resource *src_resource = pSrcResource->resource;

   unsigned dst_level = DstSubResource % (dst_resource->last_level + 1);
   unsigned dst_layer = DstSubResource / (dst_resource->last_level + 1);
   unsigned src_level = SrcSubResource % (src_resource->last_level + 1);
   unsigned src_layer = SrcSubResource / (src_resource->last_level + 1);

   struct pipe_box src_box;
   if (pSrcBox) {
      src_box.x = pSrcBox->left;
      src_box.y = pSrcBox->top;
      src_box.z = pSrcBox->front + src_layer;
      src_box.width  = pSrcBox->right  - pSrcBox->left;
      src_box.height = pSrcBox->bottom - pSrcBox->top;
      src_box.depth  = pSrcBox->back   - pSrcBox->front;
   } else {
      src_box.x = 0;
      src_box.y = 0;
      src_box.z = 0 + src_layer;
      src_box.width  = u_minify(src_resource->width0,  src_level);
      src_box.height = u_minify(src_resource->height0, src_level);
      src_box.depth  = u_minify(src_resource->depth0,  src_level);
   }

   if (areResourcesCompatible(src_resource, dst_resource)) {
      pipe->resource_copy_region(pipe,
                                 dst_resource, dst_level,
                                 DstX, DstY, DstZ + dst_layer,
                                 src_resource, src_level,
                                 &src_box);
   } else {
      util_resource_copy_region(pipe,
                                dst_resource, dst_level,
                                DstX, DstY, DstZ + dst_layer,
                                src_resource, src_level,
                                &src_box);
   }
   DebugPrintf("[WinMaliFlow] ResourceCopyRegion EXIT device=%p dstPipe=%p srcPipe=%p box=%d,%d,%d %dx%dx%d\n",
               hDevice.pDrvPrivate, dst_resource, src_resource,
               src_box.x, src_box.y, src_box.z, src_box.width,
               src_box.height, src_box.depth);
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceResolveSubResource --
 *
 *    The ResourceResolveSubResource function resolves
 *    multiple samples to one pixel.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceResolveSubResource(D3D10DDI_HDEVICE hDevice,        // IN
                           D3D10DDI_HRESOURCE hDstResource, // IN
                           UINT DstSubResource,             // IN
                           D3D10DDI_HRESOURCE hSrcResource, // IN
                           UINT SrcSubResource,             // IN
                           DXGI_FORMAT ResolveFormat)       // IN
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   DebugPrintf("[WinMaliFlow] ResourceResolve device=%p dst=%p dstSub=%u src=%p srcSub=%u format=%u result=unsupported\n",
               hDevice.pDrvPrivate, hDstResource.pDrvPrivate, DstSubResource,
               hSrcResource.pDrvPrivate, SrcSubResource, ResolveFormat);
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceIsStagingBusy --
 *
 *    The ResourceIsStagingBusy function determines whether a
 *    resource is currently being used by the graphics pipeline.
 *
 * ----------------------------------------------------------------------
 */

BOOL APIENTRY
ResourceIsStagingBusy(D3D10DDI_HDEVICE hDevice,       // IN
                      D3D10DDI_HRESOURCE hResource)   // IN
{
   LOG_ENTRYPOINT();

   /* ignore */

   return false;
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceReadAfterWriteHazard --
 *
 *    The ResourceReadAfterWriteHazard function informs the user-mode
 *    display driver that the specified resource was used as an output
 *    from the graphics processing unit (GPU) and that the resource
 *    will be used as an input to the GPU.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceReadAfterWriteHazard(D3D10DDI_HDEVICE hDevice,      // IN
                             D3D10DDI_HRESOURCE hResource)  // IN
{
   LOG_ENTRYPOINT();

   /* Not actually necessary */
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceUpdateSubResourceUP --
 *
 *    The ResourceUpdateSubresourceUP function updates a
 *    destination subresource region from a source
 *    system memory region.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceUpdateSubResourceUP(D3D10DDI_HDEVICE hDevice,                // IN
                            D3D10DDI_HRESOURCE hDstResource,         // IN
                            UINT DstSubResource,                     // IN
                            __in_opt const D3D10_DDI_BOX *pDstBox,   // IN
                            __in const void *pSysMemUP,              // IN
                            UINT RowPitch,                           // IN
                            UINT DepthPitch)                         // IN
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] ResourceUpdate ENTER device=%p dst=%p sub=%u box=%p sysmem=%p rowPitch=%u depthPitch=%u\n",
               hDevice.pDrvPrivate, hDstResource.pDrvPrivate, DstSubResource,
               pDstBox, pSysMemUP, RowPitch, DepthPitch);

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      DebugPrintf("[WinMaliFlow] ResourceUpdate EXIT device=%p skipped=predicate\n",
                  hDevice.pDrvPrivate);
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   struct pipe_resource *dst_resource = pDstResource->resource;

   unsigned level;
   struct pipe_box box;

   if (pDstBox) {
      UINT DstMipLevels = dst_resource->last_level + 1;
      level = DstSubResource % DstMipLevels;
      unsigned dst_layer = DstSubResource / DstMipLevels;
      box.x = pDstBox->left;
      box.y = pDstBox->top;
      box.z = pDstBox->front + dst_layer;
      box.width  = pDstBox->right  - pDstBox->left;
      box.height = pDstBox->bottom - pDstBox->top;
      box.depth  = pDstBox->back   - pDstBox->front;
   } else {
      subResourceBox(dst_resource, DstSubResource, &level, &box);
   }

   if (pDstResource->glyphDiagnostic) {
      const UINT update = ++pDstResource->glyphUpdateCount;
      if (update <= 16 || (update & (update - 1)) == 0) {
         DebugPrintf("[WinMaliGlyph] Update drv=%p pipe=%p n=%u sub=%u level=%u "
                     "box=%d,%d,%d %dx%dx%d rowPitch=%u depthPitch=%u hash=0x%08x\n",
                     hDstResource.pDrvPrivate, dst_resource, update,
                     DstSubResource, level, box.x, box.y, box.z,
                     box.width, box.height, box.depth, RowPitch, DepthPitch,
                     WinMaliGlyphSampleHash(pSysMemUP, RowPitch,
                                            box.width, box.height));
      }
   }

   struct pipe_transfer *transfer;
   void *map;
   if (pDstResource->buffer) {
      map = pipe->buffer_map(pipe,
                              dst_resource,
                              level,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box,
                              &transfer);
   } else {
      map = pipe->texture_map(pipe,
                              dst_resource,
                              level,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box,
                              &transfer);
   }
   assert(map);
   if (map) {
      for (int z = 0; z < box.depth; ++z) {
         uint8_t *dst = (uint8_t*)map + z*transfer->layer_stride;
         const uint8_t *src = (const uint8_t*)pSysMemUP + z*DepthPitch;
         util_copy_rect(dst,
                        dst_resource->format,
                        transfer->stride,
                        0, 0, box.width, box.height,
                        src,
                        RowPitch,
                        0, 0);
      }
      if (pDstResource->buffer) {
         pipe_buffer_unmap(pipe, transfer);
      } else {
         pipe_texture_unmap(pipe, transfer);
      }
   }
   DebugPrintf("[WinMaliFlow] ResourceUpdate EXIT device=%p dstPipe=%p level=%u box=%d,%d,%d %dx%dx%d map=%p\n",
               hDevice.pDrvPrivate, dst_resource, level,
               box.x, box.y, box.z, box.width, box.height, box.depth, map);
}

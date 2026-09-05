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
 * Draw.h --
 *    Functions that render 3D primitives.
 */


#include "Draw.h"
#include "State.h"
#include "Shader.h"
#include "Resource.h"

#include "Debug.h"

#include "util/u_draw.h"
#include "util/u_memory.h"

static unsigned
ClampedUAdd(unsigned a,
            unsigned b)
{
   unsigned c = a + b;
   if (c < a) {
      return 0xffffffff;
   }
   return c;
}


/* stride is required in order to set the element data */
static void
update_velems(Device *pDevice)
{
   if (!pDevice->velems_changed)
      return;

   if(pDevice->element_layout) {
      struct cso_velems_state *state = &pDevice->element_layout->state;
      for (unsigned i = 0; i < state->count; i++)
         state->velems[i].src_stride = pDevice->vertex_strides[state->velems[i].vertex_buffer_index];
      cso_set_vertex_elements(pDevice->cso, state);
   } else {
      /* No input layout (e.g. a draw sourced entirely from SV_VertexID with
       * no vertex buffers). D3D allows this, but the driver still expects a
       * bound vertex-elements state: panfrost's panfrost_emit_vertex_data
       * dereferences ctx->vertex unconditionally, so leaving it NULL faults
       * on the first Draw. Bind an empty (0-element) state. */
      struct cso_velems_state empty;
      memset(&empty, 0, sizeof(empty));
      cso_set_vertex_elements(pDevice->cso, &empty);
   }

   pDevice->velems_changed = false;
}

/*
 * We have to resolve the stream output state for empty geometry shaders.
 * In particular we've remapped the output indices when translating the
 * shaders so now the register_index variables in the stream output
 * state are incorrect and we need to remap them back to the correct
 * state.
 */
static void
ResolveState(Device *pDevice)
{
   ShaderResolveVertexInputTypes(pDevice);

   if (pDevice->bound_empty_gs && pDevice->bound_vs &&
       pDevice->bound_vs->state.tokens) {
      Shader *gs = pDevice->bound_empty_gs;
      Shader *vs = pDevice->bound_vs;
      bool remapped = false;
      struct pipe_context *pipe = pDevice->pipe;
      if (!gs->output_resolved) {
         for (unsigned i = 0; i < gs->state.stream_output.num_outputs; ++i) {
            unsigned mapping =
               ShaderFindOutputMapping(vs, gs->state.stream_output.output[i].register_index);
            if (mapping != gs->state.stream_output.output[i].register_index) {
               gs->state.stream_output.output[i].register_index = mapping;
               remapped = true;
            }
         }
         if (remapped) {
            pipe->delete_gs_state(pipe, gs->handle);
            gs->handle = pipe->create_gs_state(pipe, &gs->state);
         }
         gs->output_resolved = true;
      }
      pipe->bind_gs_state(pipe, gs->handle);
   }
   update_velems(pDevice);

   if (pDevice->vbuffers_changed) {
      cso_set_vertex_buffers(pDevice->cso, PIPE_MAX_ATTRIBS, pDevice->vertex_buffers);
      pDevice->vbuffers_changed = false;
   }
}


#if defined(WINMALI_LOG_GLYPH)
/* Correlate the unusual R8_UINT buffer seen in the affected XAML processes
 * with the complete state of the draw that consumes it.  This deliberately
 * ignores unrelated draws so the Glyph profile remains usable in DbgView. */
static void
WinMaliTextTraceDraw(Device *pDevice, const char *kind,
                     unsigned elementCount, unsigned instanceCount)
{
   uint64_t r8UintSlots = 0;
   uint64_t companionSlots = 0;
   struct pipe_resource *r8UintResource = NULL;
   struct pipe_resource *companionResource = NULL;

   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS && i < 64; ++i) {
      struct pipe_sampler_view *view =
         pDevice->sampler_views[MESA_SHADER_FRAGMENT][i];
      struct pipe_resource *texture = view ? view->texture : NULL;
      if (!view || !texture)
         continue;

      if (view->format == PIPE_FORMAT_R8_UINT ||
          texture->format == PIPE_FORMAT_R8_UINT) {
         r8UintSlots |= 1ull << i;
         if (!r8UintResource)
            r8UintResource = texture;
      }

      if ((view->format == PIPE_FORMAT_B8G8R8A8_UNORM ||
           texture->format == PIPE_FORMAT_B8G8R8A8_UNORM) &&
          texture->width0 == 256 && texture->height0 == 80) {
         companionSlots |= 1ull << i;
         if (!companionResource)
            companionResource = texture;
      }
   }

   if (!r8UintSlots)
      return;

   const unsigned sequence = ++pDevice->textDrawSequence;
   if (pDevice->textDrawLogged >= 128) {
      if (pDevice->textDrawLogged == 128) {
         WINMALI_TEXT_TRACE("DrawState device=%p further glyph-style draws suppressed",
                            pDevice);
         ++pDevice->textDrawLogged;
      }
      return;
   }
   ++pDevice->textDrawLogged;

   struct pipe_surface *surface =
      pDevice->fb.nr_cbufs && pDevice->fb.cbufs[0].texture
         ? &pDevice->fb.cbufs[0] : NULL;
   struct pipe_resource *target = surface ? surface->texture : NULL;
   BlendState *blend = pDevice->textBoundBlend;
   Shader *ps = pDevice->textBoundPs;

   WINMALI_TEXT_TRACE(
      "DrawState seq=%u kind=%s elements=%u instances=%u device=%p "
      "ps=%p psState=%p blend=%p blendId=%u dual=%u enable=%u mask=0x%x "
      "rgb=%u,%u,%u alpha=%u,%u,%u r8Slots=0x%llx r8=%p "
      "r8Target=%u r8Size=%ux%u companionSlots=0x%llx companion=%p "
      "rt=%p rtFormat=%u rtSize=%ux%u",
      sequence, kind, elementCount, instanceCount, pDevice,
      ps, ps ? ps->handle : NULL, blend,
      blend ? blend->diagnosticId : 0,
      blend ? blend->dualSource : 0,
      blend ? blend->blendEnable : 0,
      blend ? blend->colorMask : 0,
      blend ? blend->rgbFunc : 0,
      blend ? blend->rgbSrcFactor : 0,
      blend ? blend->rgbDstFactor : 0,
      blend ? blend->alphaFunc : 0,
      blend ? blend->alphaSrcFactor : 0,
      blend ? blend->alphaDstFactor : 0,
      (unsigned long long)r8UintSlots, r8UintResource,
      r8UintResource ? r8UintResource->target : 0,
      r8UintResource ? r8UintResource->width0 : 0,
      r8UintResource ? r8UintResource->height0 : 0,
      (unsigned long long)companionSlots, companionResource,
      target, target ? target->format : 0,
      target ? target->width0 : 0, target ? target->height0 : 0);
}
#else
#define WinMaliTextTraceDraw(...) ((void)0)
#endif

#if defined(WINMALI_SHADER_CAPTURE)
static unsigned
WinMaliCaptureTraceDraw(Device *device, const char *kind,
                        unsigned elements, unsigned instances,
                        unsigned first, int baseVertex,
                        unsigned startInstance, bool indexed)
{
   Shader *ps = device->captureBoundPs;
   Shader *vs = device->bound_vs;
   if (!ps || !ps->captureDxbcHash)
      return 0;

   bool hasR8Uint = false;
   uint64_t r8Slots = 0, bgraSlots = 0;
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS && i < 64; ++i) {
      struct pipe_sampler_view *view =
         device->sampler_views[MESA_SHADER_FRAGMENT][i];
      if (!view || !view->texture)
         continue;
      if (view->format == PIPE_FORMAT_R8_UINT) {
         hasR8Uint = true;
         r8Slots |= UINT64_C(1) << i;
      }
      if (view->format == PIPE_FORMAT_B8G8R8A8_UNORM)
         bgraSlots |= UINT64_C(1) << i;
   }
   if (!hasR8Uint)
      return 0;

   /* Glyph rendering deliberately reuses one shader for many different
    * characters.  Shader-hash deduplication hid the later broken draws, so
    * capture a bounded sequence instead.  The bound keeps disk and mapping
    * overhead finite while retaining enough consecutive draws to compare a
    * good glyph with the first malformed one. */
   const unsigned sequence = ++device->captureDrawSequence;
   if (sequence > 64)
      return 0;

   struct pipe_resource *target =
      device->fb.nr_cbufs && device->fb.cbufs[0].texture
         ? device->fb.cbufs[0].texture : NULL;
   char stage[128];
   if (target && target->format == PIPE_FORMAT_B8G8R8A8_UNORM) {
      snprintf(stage, sizeof(stage), "seq-%03u-rt-before", sequence);
      WinMaliDiagnosticCaptureResource(device->pipe, target, stage);
   }

   const BlendState *blend = device->captureBoundBlend;
   WINMALI_CAPTURE_TRACE(
      "OM seq=%u primitive=%u blend=%p id=%u enable=%u dual=%u mask=0x%x "
      "rgb=%u,%u,%u alpha=%u,%u,%u factor=%g,%g,%g,%g sampleMask=0x%x",
      sequence, device->primitive, blend, blend ? blend->diagnosticId : 0,
      blend ? blend->blendEnable : 0, blend ? blend->dualSource : 0,
      blend ? blend->colorMask : 0, blend ? blend->rgbFunc : 0,
      blend ? blend->rgbSrcFactor : 0, blend ? blend->rgbDstFactor : 0,
      blend ? blend->alphaFunc : 0, blend ? blend->alphaSrcFactor : 0,
      blend ? blend->alphaDstFactor : 0, device->captureBlendFactor[0],
      device->captureBlendFactor[1], device->captureBlendFactor[2],
      device->captureBlendFactor[3], device->captureSampleMask);
   WinMaliCaptureRecord(
      "om seq=%u primitive=%u blend=%p id=%u enable=%u dual=%u mask=0x%x "
      "rgb=%u,%u,%u alpha=%u,%u,%u factor=%g,%g,%g,%g sampleMask=0x%x",
      sequence, device->primitive, blend, blend ? blend->diagnosticId : 0,
      blend ? blend->blendEnable : 0, blend ? blend->dualSource : 0,
      blend ? blend->colorMask : 0, blend ? blend->rgbFunc : 0,
      blend ? blend->rgbSrcFactor : 0, blend ? blend->rgbDstFactor : 0,
      blend ? blend->alphaFunc : 0, blend ? blend->alphaSrcFactor : 0,
      blend ? blend->alphaDstFactor : 0, device->captureBlendFactor[0],
      device->captureBlendFactor[1], device->captureBlendFactor[2],
      device->captureBlendFactor[3], device->captureSampleMask);

   for (unsigned i = 0; i < device->captureViewportCount; ++i) {
      const struct pipe_viewport_state *vp = &device->captureViewports[i];
      WINMALI_CAPTURE_TRACE(
         "Viewport seq=%u slot=%u scale=%g,%g,%g translate=%g,%g,%g",
         sequence, i, vp->scale[0], vp->scale[1], vp->scale[2],
         vp->translate[0], vp->translate[1], vp->translate[2]);
      WinMaliCaptureRecord(
         "viewport seq=%u slot=%u scale=%g,%g,%g translate=%g,%g,%g",
         sequence, i, vp->scale[0], vp->scale[1], vp->scale[2],
         vp->translate[0], vp->translate[1], vp->translate[2]);
   }
   for (unsigned i = 0; i < device->captureScissorCount; ++i) {
      const struct pipe_scissor_state *sc = &device->captureScissors[i];
      WINMALI_CAPTURE_TRACE("Scissor seq=%u slot=%u rect=%u,%u-%u,%u",
                            sequence, i, sc->minx, sc->miny, sc->maxx,
                            sc->maxy);
      WinMaliCaptureRecord("scissor seq=%u slot=%u rect=%u,%u-%u,%u",
                           sequence, i, sc->minx, sc->miny, sc->maxx,
                           sc->maxy);
   }

   /* Snapshot only the shader inputs that distinguish the broken glyph path:
    * the R8 metadata/glyph buffer and any BGRA atlas or source texture. */
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS && i < 64; ++i) {
      struct pipe_sampler_view *view =
         device->sampler_views[MESA_SHADER_FRAGMENT][i];
      if (!view || (view->format != PIPE_FORMAT_R8_UINT &&
                    view->format != PIPE_FORMAT_B8G8R8A8_UNORM))
         continue;
      char inputStage[128];
      snprintf(inputStage, sizeof(inputStage), "seq-%03u-ps-slot-%u-%s",
               sequence, i,
               view->format == PIPE_FORMAT_R8_UINT ? "r8" : "bgra");
      WinMaliCaptureShaderInput(device->pipe, view, inputStage);
   }

   const struct cso_velems_state *layout =
      device->element_layout ? &device->element_layout->state : NULL;
   WINMALI_CAPTURE_TRACE("IA seq=%u layout=%p elements=%u", sequence, layout,
                         layout ? layout->count : 0);
   WinMaliCaptureRecord("ia seq=%u layout=%p elements=%u", sequence, layout,
                        layout ? layout->count : 0);
   if (layout) {
      for (unsigned i = 0; i < layout->count; ++i) {
         const struct pipe_vertex_element *ve = &layout->velems[i];
         WINMALI_CAPTURE_TRACE(
            "IAElement seq=%u reg=%u slot=%u offset=%u stride=%u format=%u "
            "divisor=%u", sequence, i, ve->vertex_buffer_index,
            ve->src_offset, ve->src_stride, ve->src_format,
            ve->instance_divisor);
         WinMaliCaptureRecord(
            "ia-element seq=%u reg=%u slot=%u offset=%u stride=%u format=%u "
            "divisor=%u", sequence, i, ve->vertex_buffer_index,
            ve->src_offset, ve->src_stride, ve->src_format,
            ve->instance_divisor);
      }
   }

   for (unsigned i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      const struct pipe_vertex_buffer *vb = &device->vertex_buffers[i];
      struct pipe_resource *resource =
         vb->is_user_buffer ? NULL : vb->buffer.resource;
      if (!resource)
         continue;
      const unsigned offset = MIN2(vb->buffer_offset, resource->width0);
      const unsigned available = resource->width0 - offset;
      const unsigned bytes = MIN2(available, 4u * 1024u * 1024u);
      WINMALI_CAPTURE_TRACE(
         "VertexBuffer seq=%u slot=%u resource=%p offset=%u stride=%u "
         "resourceBytes=%u captureBytes=%u user=%u", sequence, i, resource,
         vb->buffer_offset, device->vertex_strides[i], resource->width0,
         bytes, vb->is_user_buffer ? 1u : 0u);
      WinMaliCaptureRecord(
         "vertex-buffer seq=%u slot=%u resource=%p offset=%u stride=%u "
         "resourceBytes=%u captureBytes=%u user=%u", sequence, i, resource,
         vb->buffer_offset, device->vertex_strides[i], resource->width0,
         bytes, vb->is_user_buffer ? 1u : 0u);
      if (bytes) {
         snprintf(stage, sizeof(stage), "seq-%03u-vb-%u", sequence, i);
         WinMaliCaptureBufferRange(device->pipe, resource, offset, bytes,
                                   stage);
      }
   }

   if (indexed && device->index_buffer && device->index_size) {
      uint64_t offset64 = (uint64_t)device->ib_offset +
                          (uint64_t)first * device->index_size;
      uint64_t bytes64 = (uint64_t)elements * device->index_size;
      if (offset64 < device->index_buffer->width0) {
         unsigned offset = (unsigned)offset64;
         unsigned bytes = (unsigned)MIN2(
            bytes64, (uint64_t)device->index_buffer->width0 - offset64);
         WINMALI_CAPTURE_TRACE(
            "IndexBuffer seq=%u resource=%p offset=%u size=%u indexSize=%u",
            sequence, device->index_buffer, offset, bytes,
            device->index_size);
         WinMaliCaptureRecord(
            "index-buffer seq=%u resource=%p offset=%u size=%u indexSize=%u",
            sequence, device->index_buffer, offset, bytes,
            device->index_size);
         snprintf(stage, sizeof(stage), "seq-%03u-ib", sequence);
         WinMaliCaptureBufferRange(device->pipe, device->index_buffer, offset,
                                   bytes, stage);
      }
   }

   for (unsigned shader = 0; shader < MESA_SHADER_STAGES; ++shader) {
      for (unsigned slot = 0; slot < PIPE_MAX_CONSTANT_BUFFERS; ++slot) {
         const struct pipe_constant_buffer *cb =
            &device->captureConstantBuffers[shader][slot];
         if (!cb->buffer)
            continue;
         const unsigned bytes = MIN2(cb->buffer_size, 1024u * 1024u);
         WINMALI_CAPTURE_TRACE(
            "ConstantBuffer seq=%u shader=%u slot=%u resource=%p offset=%u "
            "size=%u captureBytes=%u", sequence, shader, slot, cb->buffer,
            cb->buffer_offset, cb->buffer_size, bytes);
         WinMaliCaptureRecord(
            "constant-buffer seq=%u shader=%u slot=%u resource=%p offset=%u "
            "size=%u captureBytes=%u", sequence, shader, slot, cb->buffer,
            cb->buffer_offset, cb->buffer_size, bytes);
         if (bytes) {
            snprintf(stage, sizeof(stage), "seq-%03u-cb-%u-%u", sequence,
                     shader, slot);
            WinMaliCaptureBufferRange(device->pipe, cb->buffer,
                                      cb->buffer_offset, bytes, stage);
         }
      }
   }

   WINMALI_CAPTURE_TRACE(
        "Draw seq=%u processPid=%lu kind=%s elements=%u instances=%u "
        "first=%u baseVertex=%d startInstance=%u indexed=%u ps=%p "
        "psDxbc=%016llx psTgsi=%016llx vs=%p vsDxbc=%016llx "
        "vsTgsi=%016llx r8Slots=%016llx bgraSlots=%016llx "
       "rtFormat=%u rt=%ux%u",
        sequence, GetCurrentProcessId(), kind, elements, instances, first,
        baseVertex, startInstance, indexed ? 1u : 0u, ps,
       (unsigned long long)ps->captureDxbcHash,
       (unsigned long long)ps->captureTgsiHash,
       vs, (unsigned long long)(vs ? vs->captureDxbcHash : 0),
       (unsigned long long)(vs ? vs->captureTgsiHash : 0),
      (unsigned long long)r8Slots, (unsigned long long)bgraSlots,
      target ? target->format : 0, target ? target->width0 : 0,
      target ? target->height0 : 0);
   WinMaliCaptureRecord(
        "draw seq=%u pid=%lu kind=%s elements=%u instances=%u first=%u "
        "baseVertex=%d startInstance=%u indexed=%u ps=%p "
        "psDxbc=%016llx psTgsi=%016llx vs=%p vsDxbc=%016llx "
        "vsTgsi=%016llx r8Slots=%016llx bgraSlots=%016llx "
       "rtFormat=%u rt=%ux%u",
        sequence, GetCurrentProcessId(), kind, elements, instances, first,
        baseVertex, startInstance, indexed ? 1u : 0u, ps,
       (unsigned long long)ps->captureDxbcHash,
       (unsigned long long)ps->captureTgsiHash,
       vs, (unsigned long long)(vs ? vs->captureDxbcHash : 0),
       (unsigned long long)(vs ? vs->captureTgsiHash : 0),
      (unsigned long long)r8Slots, (unsigned long long)bgraSlots,
      target ? target->format : 0, target ? target->width0 : 0,
       target ? target->height0 : 0);
   return sequence;
}

static void
WinMaliCaptureTraceResult(Device *device, unsigned sequence,
                           const char *kind)
{
   if (!sequence)
      return;

   Shader *ps = device->captureBoundPs;
   struct pipe_resource *target =
      device->fb.nr_cbufs && device->fb.cbufs[0].texture
         ? device->fb.cbufs[0].texture : NULL;
   if (!ps || !target || target->format != PIPE_FORMAT_B8G8R8A8_UNORM)
      return;

   char stage[96];
   snprintf(stage, sizeof(stage), "seq-%03u-rt-after-%s", sequence, kind);
   WinMaliDiagnosticCaptureResource(device->pipe, target, stage);
}
#else
#define WinMaliCaptureTraceDraw(...) 0u
#define WinMaliCaptureTraceResult(...) ((void)0)
#endif


static struct pipe_resource *
create_null_index_buffer(struct pipe_context *ctx, uint num_indices,
                         unsigned *restart_index, unsigned *index_size,
                         unsigned *ib_offset)
{
   unsigned buf_size = num_indices * sizeof(unsigned);
   unsigned *buf = (unsigned*)MALLOC(buf_size);
   struct pipe_resource *ibuf;

   memset(buf, 0, buf_size);

   ibuf = pipe_buffer_create_with_data(ctx,
                                       PIPE_BIND_INDEX_BUFFER,
                                       PIPE_USAGE_IMMUTABLE,
                                       buf_size, buf);
   *index_size = 4;
   *restart_index = 0xffffffff;
   *ib_offset = 0;

   FREE(buf);

   return ibuf;
}

static bool
d3d_primitive_uses_strip_cut(enum mesa_prim primitive)
{
   /* Direct3D 10 defines the all-ones index as a cut/restart marker for
    * strip topologies. It remains an ordinary index for list topologies. */
   switch (primitive) {
   case MESA_PRIM_LINE_STRIP:
   case MESA_PRIM_TRIANGLE_STRIP:
   case MESA_PRIM_LINE_STRIP_ADJACENCY:
   case MESA_PRIM_TRIANGLE_STRIP_ADJACENCY:
      return true;
   default:
      return false;
   }
}

/*
 * ----------------------------------------------------------------------
 *
 * Draw --
 *
 *    The Draw function draws nonindexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Draw(D3D10DDI_HDEVICE hDevice,   // IN
     UINT VertexCount,           // IN
     UINT StartVertexLocation)   // IN
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] D3D Draw ENTER device=%p vertices=%u start=%u\n",
               hDevice.pDrvPrivate, VertexCount, StartVertexLocation);

   Device *pDevice = CastDevice(hDevice);

   ResolveState(pDevice);
   WinMaliTextTraceDraw(pDevice, "Draw", VertexCount, 1);
   unsigned capture = WinMaliCaptureTraceDraw(
      pDevice, "Draw", VertexCount, 1, StartVertexLocation, 0, 0, false);

   assert(pDevice->primitive < MESA_PRIM_COUNT);
   util_draw_arrays(pDevice->pipe,
                    pDevice->primitive,
                     StartVertexLocation,
                     VertexCount);
   WinMaliCaptureTraceResult(pDevice, capture, "draw");
   DebugPrintf("[WinMaliFlow] D3D Draw EXIT device=%p\n",
               hDevice.pDrvPrivate);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexed --
 *
 *    The DrawIndexed function draws indexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexed(D3D10DDI_HDEVICE hDevice,  // IN
            UINT IndexCount,           // IN
            UINT StartIndexLocation,   // IN
            INT BaseVertexLocation)    // IN
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] D3D DrawIndexed ENTER device=%p indices=%u start=%u base=%d\n",
               hDevice.pDrvPrivate, IndexCount, StartIndexLocation,
               BaseVertexLocation);

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   struct pipe_resource *null_ib = NULL;
   unsigned restart_index = pDevice->restart_index;
   unsigned index_size = pDevice->index_size;
   unsigned ib_offset = pDevice->ib_offset;

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   /* XXX I don't think draw still needs this? */
   if (!pDevice->index_buffer) {
      null_ib =
         create_null_index_buffer(pDevice->pipe,
                                  StartIndexLocation + IndexCount,
                                  &restart_index, &index_size, &ib_offset);
   }

   ResolveState(pDevice);
   WinMaliTextTraceDraw(pDevice, "DrawIndexed", IndexCount, 1);
   unsigned capture = WinMaliCaptureTraceDraw(
      pDevice, "DrawIndexed", IndexCount, 1, StartIndexLocation,
      BaseVertexLocation, 0, true);

   util_draw_init_info(&info);
   info.index_size = index_size;
   info.mode = pDevice->primitive;
   draw.start = ClampedUAdd(StartIndexLocation, ib_offset / index_size);
   draw.count = IndexCount;
   info.index.resource = null_ib ? null_ib : pDevice->index_buffer;
   draw.index_bias = BaseVertexLocation;
   info.primitive_restart = d3d_primitive_uses_strip_cut(pDevice->primitive);
   info.restart_index = restart_index;

   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, NULL, &draw, 1);
   WinMaliCaptureTraceResult(pDevice, capture, "draw-indexed");

   if (null_ib) {
      pipe_resource_reference(&null_ib, NULL);
   }
   DebugPrintf("[WinMaliFlow] D3D DrawIndexed EXIT device=%p\n",
               hDevice.pDrvPrivate);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawInstanced --
 *
 *    The DrawInstanced function draws particular instances
 *    of nonindexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawInstanced(D3D10DDI_HDEVICE hDevice,      // IN
              UINT VertexCountPerInstance,   // IN
              UINT InstanceCount,            // IN
              UINT StartVertexLocation,      // IN
              UINT StartInstanceLocation)    // IN
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] DrawInstanced ENTER device=%p vertices=%u instances=%u startVertex=%u startInstance=%u\n",
               hDevice.pDrvPrivate, VertexCountPerInstance, InstanceCount,
               StartVertexLocation, StartInstanceLocation);

   Device *pDevice = CastDevice(hDevice);

   if (!InstanceCount) {
      DebugPrintf("[WinMaliFlow] DrawInstanced EXIT device=%p skipped=zero-instances\n",
                  hDevice.pDrvPrivate);
      return;
   }

   ResolveState(pDevice);
   WinMaliTextTraceDraw(pDevice, "DrawInstanced", VertexCountPerInstance,
                        InstanceCount);
   unsigned capture = WinMaliCaptureTraceDraw(
      pDevice, "DrawInstanced", VertexCountPerInstance, InstanceCount,
      StartVertexLocation, 0, StartInstanceLocation, false);

   assert(pDevice->primitive < MESA_PRIM_COUNT);
   util_draw_arrays_instanced(pDevice->pipe,
                              pDevice->primitive,
                              StartVertexLocation,
                              VertexCountPerInstance,
                               StartInstanceLocation,
                               InstanceCount);
   WinMaliCaptureTraceResult(pDevice, capture, "draw-instanced");
   DebugPrintf("[WinMaliFlow] DrawInstanced EXIT device=%p\n",
               hDevice.pDrvPrivate);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexedInstanced --
 *
 *    The DrawIndexedInstanced function draws particular
 *    instances of indexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexedInstanced(D3D10DDI_HDEVICE hDevice,   // IN
                     UINT IndexCountPerInstance, // IN
                     UINT InstanceCount,         // IN
                     UINT StartIndexLocation,    // IN
                     INT BaseVertexLocation,     // IN
                     UINT StartInstanceLocation) // IN
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] DrawIndexedInstanced ENTER device=%p indices=%u instances=%u startIndex=%u base=%d startInstance=%u\n",
               hDevice.pDrvPrivate, IndexCountPerInstance, InstanceCount,
               StartIndexLocation, BaseVertexLocation, StartInstanceLocation);

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   struct pipe_resource *null_ib = NULL;
   unsigned restart_index = pDevice->restart_index;
   unsigned index_size = pDevice->index_size;
   unsigned ib_offset = pDevice->ib_offset;

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   if (!InstanceCount) {
      DebugPrintf("[WinMaliFlow] DrawIndexedInstanced EXIT device=%p skipped=zero-instances\n",
                  hDevice.pDrvPrivate);
      return;
   }

   /* XXX I don't think draw still needs this? */
   if (!pDevice->index_buffer) {
      null_ib =
         create_null_index_buffer(pDevice->pipe,
                                  StartIndexLocation + IndexCountPerInstance,
                                  &restart_index, &index_size, &ib_offset);
   }

   ResolveState(pDevice);
   WinMaliTextTraceDraw(pDevice, "DrawIndexedInstanced",
                        IndexCountPerInstance, InstanceCount);
   unsigned capture = WinMaliCaptureTraceDraw(
      pDevice, "DrawIndexedInstanced", IndexCountPerInstance, InstanceCount,
      StartIndexLocation, BaseVertexLocation, StartInstanceLocation, true);

   util_draw_init_info(&info);
   info.index_size = index_size;
   info.mode = pDevice->primitive;
   draw.start = ClampedUAdd(StartIndexLocation, ib_offset / index_size);
   draw.count = IndexCountPerInstance;
   info.index.resource = null_ib ? null_ib : pDevice->index_buffer;
   draw.index_bias = BaseVertexLocation;
   info.start_instance = StartInstanceLocation;
   info.instance_count = InstanceCount;
   info.primitive_restart = d3d_primitive_uses_strip_cut(pDevice->primitive);
   info.restart_index = restart_index;

   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, NULL, &draw, 1);
   WinMaliCaptureTraceResult(pDevice, capture, "draw-indexed-instanced");

   if (null_ib) {
      pipe_resource_reference(&null_ib, NULL);
   }
   DebugPrintf("[WinMaliFlow] DrawIndexedInstanced EXIT device=%p\n",
               hDevice.pDrvPrivate);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawAuto --
 *
 *    The DrawAuto function works similarly to the Draw function,
 *    except DrawAuto is used for the special case where vertex
 *    data is written through the stream-output unit and then
 *    recycled as a vertex buffer. The driver determines the number
 *    of primitives, in part, by how much data was written to the
 *    buffer through stream output.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawAuto(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   DebugPrintf("[WinMaliFlow] DrawAuto ENTER device=%p\n",
               hDevice.pDrvPrivate);

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_indirect_info indirect;


   if (!pDevice->draw_so_target) {
      LOG_UNSUPPORTED("DrawAuto without a set source buffer!");
      DebugPrintf("[WinMaliFlow] DrawAuto EXIT device=%p skipped=no-so-target\n",
                  hDevice.pDrvPrivate);
      return;
   }

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   ResolveState(pDevice);
   WinMaliTextTraceDraw(pDevice, "DrawAuto", 0, 1);
   unsigned capture = WinMaliCaptureTraceDraw(
      pDevice, "DrawAuto", 0, 1, 0, 0, 0, false);

   util_draw_init_info(&info);
   info.mode = pDevice->primitive;
   memset(&indirect, 0, sizeof indirect);
   indirect.count_from_stream_output = pDevice->draw_so_target;

   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, &indirect, NULL, 1);
   WinMaliCaptureTraceResult(pDevice, capture, "draw-auto");
   DebugPrintf("[WinMaliFlow] DrawAuto EXIT device=%p\n",
               hDevice.pDrvPrivate);
}

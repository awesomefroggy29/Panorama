/*
 * Copyright (C) 2024 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir_builder.h"
#include "pan_nir.h"

/*
 * load_vertex_id_zero_base() is supposed to return the zero-based
 * vertex ID, which is then offset by load_first_vertex() to get
 * an absolute vertex ID. At the same time, when we're in a Vulkan
 * environment, load_first_vertex() also encodes the vertexOffset
 * passed to the indexed draw.
 *
 * Midgard/Bifrost have a sligtly different semantics, where
 * load_first_vertex() returns vertexOffset + minVertexIdInIndexRange,
 * and load_vertex_id_zero_base() returns an ID that needs to be offset
 * by this vertexOffset + minVertexIdInIndexRange to get the absolute
 * vertex ID. Everything works fine as long as all the load_first_vertex()
 * and load_vertex_id_zero_base() calls are coming from the
 * load_vertex_id() lowering. But as mentioned above, that's no longer
 * the case in Vulkan, where gl_BaseVertexARB will be turned into
 * load_first_vertex() and expect a value of vertexOffset in an
 * indexed draw context.
 *
 * This pass is turning load_vertex_id() calls into
 * load_raw_vertex_id_pan() + load_raw_vertex_offset_pan().
 */

static bool
lower_load_vertex_id(nir_builder *b, nir_intrinsic_instr *intr,
                     UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_vertex_id)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_iadd(b, nir_load_raw_vertex_id_pan(b),
                                        nir_load_raw_vertex_offset_pan(b)));
   return true;
}

bool
pan_nir_lower_vertex_id(nir_shader *shader)
{
   return nir_shader_intrinsics_pass(shader, lower_load_vertex_id,
                                     nir_metadata_control_flow, NULL);
}

/*
 * GL never produces load_vertex_id_zero_base() (st/mesa uses
 * vertex_id_zero_based=false), so the backends only implement
 * load_vertex_id(). The D3D10 UMD frontend reaches us through
 * tgsi_to_nir, which turns TGSI_SEMANTIC_VERTEXID_NOBASE (SM4
 * SV_VertexID) into load_vertex_id_zero_base() - unhandled in
 * bifrost_compile ("Unhandled intrinsic load_vertex_id_zero_base").
 *
 * Per the pan semantics described above, the absolute ID is the raw ID
 * offset by raw_vertex_offset, so the zero-based ID is
 * load_vertex_id() - load_raw_vertex_offset_pan(). Run this BEFORE
 * pan_nir_lower_vertex_id so the arch<=7 path composes (the iadd/isub
 * pair folds away).
 */

static bool
lower_load_vertex_id_zero_base(nir_builder *b, nir_intrinsic_instr *intr,
                               UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_vertex_id_zero_base)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_isub(b, nir_load_vertex_id(b),
                                        nir_load_raw_vertex_offset_pan(b)));
   return true;
}

bool
pan_nir_lower_vertex_id_zero_base(nir_shader *shader)
{
   return nir_shader_intrinsics_pass(shader, lower_load_vertex_id_zero_base,
                                     nir_metadata_control_flow, NULL);
}

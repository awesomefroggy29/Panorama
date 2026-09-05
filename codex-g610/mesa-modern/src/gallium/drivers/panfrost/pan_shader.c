/*
 * Copyright (C) 2025 Arm Ltd.
 * Copyright (c) 2022 Amazon.com, Inc. or its affiliates.
 * Copyright (C) 2019-2022 Collabora, Ltd.
 * Copyright (C) 2019 Red Hat Inc.
 * Copyright (C) 2018 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "pan_shader.h"
#include "nir/tgsi_to_nir.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#include "util/u_dual_blend.h"
#include "util/log.h"
#include "util/mesa-blake3.h"
#include "nir_builder.h"
#include "nir_serialize.h"
#include "pan_bo.h"
#include "pan_context.h"
#include "pan_compiler.h"
#include "pan_nir.h"
#include "pan_trace.h"
#include "compiler/bifrost/bifrost_compile.h"
#include "shader_enums.h"

#if defined(WINMALI_SHADER_CAPTURE) || defined(WINMALI_GLYPH_PIPELINE_PROBE) || \
    defined(WINMALI_ATTRIBUTE_PATH_PROBE) || \
    defined(WINMALI_LOOKUP_SPLIT_PROBE) || \
    defined(WINMALI_IDVS_HANDOFF_PROBE)
#include <windows.h>
#include <stdio.h>
#include <string.h>
#endif

#if defined(WINMALI_SHADER_CAPTURE)
#include "tgsi/tgsi_parse.h"
#endif

/* The broad shader-stage trace served its purpose. Keep its call sites for
 * easy reactivation, but leave only the focused TGSI diagnostic enabled. */
#define WM_SHADER_INIT_LOG(...) do { } while (0)

#if defined(WINMALI_LOG_FOCUSED) || \
    defined(WINMALI_FRAG_W_BYPASS_PROBE) || \
    defined(WINMALI_LD_VAR_FALLBACK_PROBE)
/* Exact DirectWrite/XAML fragment shader used by the missing shell glyphs.
 * Keep the variant diagnostic narrowly scoped so a normal focused capture
 * gains only one line per cache/variant decision. */
static bool
winmali_varying_diag_fragment_shape(const nir_shader *shader)
{
   static const uint32_t glyph_fs_blake3[8] = {
      0x7d33757c, 0x58e6468f, 0x91062436, 0x935f41a4,
      0xf3b8f348, 0x19ed5d0b, 0xb30a1f59, 0x7ca838ba,
   };

   return shader->info.stage == MESA_SHADER_FRAGMENT &&
          _mesa_printed_blake3_equal(shader->info.source_blake3,
                                     glyph_fs_blake3);
}
#endif

#if defined(WINMALI_FRAG_W_BYPASS_PROBE)
/* bi_opcodes.h is generated in the private Bifrost compiler build directory
 * and cannot be included by the Gallium panfrost target because its own
 * bifrost.h dependency is not on this target's include path.  Keep the ABI
 * value named here; gen_disasm.py/bi_opcodes.h defines FRAG_W as 2. */
enum { WINMALI_BI_VARYING_NAME_FRAG_W = 2 };

/*
 * Single-variable diagnostic for the exact DirectWrite/XAML glyph shader.
 *
 * pan_nir_lower_noperspective() implements noperspective interpolation by
 * multiplying fragment inputs by FRAG_W.  The paired captured vertex shader
 * writes position.w == 1.0 after viewport lowering, so FRAG_W must also be
 * 1.0 for this draw.  Replacing only this shader's special varying load with
 * 1.0 therefore preserves the shader's intended result while separating a
 * bad FRAG_W value from a bad generic-varying/texture path.
 */
static bool
winmali_frag_w_bypass(nir_builder *b, nir_intrinsic_instr *intrin, void *data)
{
   unsigned *replaced = data;

   if (intrin->intrinsic != nir_intrinsic_load_var_special_pan ||
       nir_intrinsic_flags(intrin) != WINMALI_BI_VARYING_NAME_FRAG_W)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);
   nir_def_rewrite_uses(&intrin->def, nir_imm_float(b, 1.0f));
   (*replaced)++;
   return true;
}
#endif

#if defined(WINMALI_SHADER_CAPTURE)
static uint64_t
winmali_capture_hash(const void *data, size_t size)
{
   const uint8_t *bytes = data;
   uint64_t hash = UINT64_C(1469598103934665603);
   for (size_t i = 0; i < size; ++i)
      hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
   return hash;
}

static bool
winmali_capture_directory(char *directory, size_t size)
{
   char temp[MAX_PATH] = {0};
   if (!GetTempPathA(ARRAY_SIZE(temp), temp))
      return false;
   snprintf(directory, size, "%sWinMaliShaderCapture", temp);
   return CreateDirectoryA(directory, NULL) ||
          GetLastError() == ERROR_ALREADY_EXISTS;
}

static void
winmali_capture_process_name(char *name, size_t size)
{
   char path[MAX_PATH] = {0};
   GetModuleFileNameA(NULL, path, ARRAY_SIZE(path));
   const char *base = strrchr(path, '\\');
   snprintf(name, size, "%s", base ? base + 1 : path);
   for (char *p = name; *p; ++p) {
      if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
             *p == '.'))
         *p = '_';
   }
}

static bool
winmali_capture_target_process(void)
{
   char name[MAX_PATH];
   winmali_capture_process_name(name, sizeof(name));
   return _stricmp(name, "StartMenuExperienceHost.exe") == 0 ||
          _stricmp(name, "SearchHost.exe") == 0 ||
          _stricmp(name, "ShellExperienceHost.exe") == 0 ||
          _stricmp(name, "ShellHost.exe") == 0 ||
          _stricmp(name, "Explorer.exe") == 0 ||
          _stricmp(name, "SystemSettings.exe") == 0;
}

static void
winmali_capture_append_index(const char *directory, const char *process,
                             const char *line)
{
   char path[MAX_PATH];
   snprintf(path, sizeof(path), "%s\\%s-%lu-index.log", directory, process,
            GetCurrentProcessId());
   HANDLE file = CreateFileA(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      return;
   DWORD written;
   WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
   CloseHandle(file);
}

static void
winmali_capture_nir_hash(const nir_shader *nir, char hash[65])
{
   struct blob blob;
   uint8_t digest[BLAKE3_KEY_LEN];
   blob_init(&blob);
   nir_serialize(&blob, nir, false);
   _mesa_blake3_compute(blob.data, blob.size, digest);
   blob_finish(&blob);
   for (unsigned i = 0; i < ARRAY_SIZE(digest); ++i)
      snprintf(hash + (i * 2), 3, "%02x", digest[i]);
   hash[64] = '\0';
}

static bool
winmali_capture_nir(const nir_shader *nir, const char *stage,
                    const char *parent, char hash[65])
{
   if (!winmali_capture_target_process())
      return false;
   char directory[MAX_PATH], process[MAX_PATH], path[MAX_PATH];
   winmali_capture_nir_hash(nir, hash);
   if (!winmali_capture_directory(directory, sizeof(directory)))
      return false;
   winmali_capture_process_name(process, sizeof(process));
   const char *shader_stage = mesa_shader_stage_name(nir->info.stage);
   snprintf(path, sizeof(path), "%s\\%s-%lu-%s-%.16s-%s.nir.txt",
            directory, process, GetCurrentProcessId(), shader_stage, hash,
            stage);
   FILE *file = fopen(path, "w");
   if (!file)
      return false;
   nir_print_shader((nir_shader *)nir, file);
   fclose(file);
   char index[1024];
   snprintf(index, sizeof(index),
             "nir process=%s pid=%lu shaderStage=%s stage=%s hash=%s "
             "parent=%s file=%s\r\n",
             process, GetCurrentProcessId(), shader_stage, stage, hash,
             parent ? parent : "none", path);
   winmali_capture_append_index(directory, process, index);
   mesa_logi("[WinMaliCapture] NIR process=%s pid=%lu stage=%s hash=%s "
             "parent=%s file=%s", process, GetCurrentProcessId(), stage,
             hash, parent ? parent : "none", path);
   return true;
}

static void
winmali_capture_binary(const struct util_dynarray *binary,
                       mesa_shader_stage stage, const char hash[65])
{
   char directory[MAX_PATH], process[MAX_PATH], path[MAX_PATH];
   if (!winmali_capture_target_process() || !binary->size ||
       !winmali_capture_directory(directory, sizeof(directory)))
      return;
   winmali_capture_process_name(process, sizeof(process));
   const char *shader_stage = mesa_shader_stage_name(stage);
   snprintf(path, sizeof(path), "%s\\%s-%lu-%s-%.16s-valhall.bin",
            directory, process, GetCurrentProcessId(), shader_stage, hash);
   HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_FILE_EXISTS)
         mesa_logi("[WinMaliCapture] BinaryOpenFailed path=%s error=%lu",
                   path, GetLastError());
      return;
   }
   DWORD written = 0;
   bool ok = binary->size <= MAXDWORD &&
             WriteFile(file, binary->data, (DWORD)binary->size, &written, NULL) &&
             written == binary->size;
   CloseHandle(file);
   char index[1024];
   snprintf(index, sizeof(index),
             "binary process=%s pid=%lu shaderStage=%s hash=%s bytes=%zu "
             "write=%u file=%s\r\n",
             process, GetCurrentProcessId(), shader_stage, hash, binary->size,
             ok, path);
   winmali_capture_append_index(directory, process, index);
   mesa_logi("[WinMaliCapture] Binary hash=%s bytes=%zu write=%u file=%s",
             hash, binary->size, ok, path);
}
#endif

#if defined(WINMALI_GLYPH_PIPELINE_PROBE) || \
    defined(WINMALI_ATTRIBUTE_PATH_PROBE) || \
    defined(WINMALI_LOOKUP_SPLIT_PROBE) || \
    defined(WINMALI_IDVS_HANDOFF_PROBE)
/* This probe is deliberately visual and narrowly targeted.  The affected
 * DirectWrite/XAML fragment shader reads packed glyph metadata from a texel
 * buffer and then samples a 2D lookup texture.  The captured shader has one
 * FRAG_RESULT_DATA0 output; dual-source blending is not part of its compiled
 * variant.  Replacing its color store proves whether the raster path reaches
 * fragment execution, while DCE removes the suspect fetch path.
 *
 * The matching vertex shader is identified by its stable source BLAKE3.  Its
 * position is replaced with a vertex-ID-generated full-screen quad and its
 * other outputs are zeroed.  That preserves the real indexed-instanced IDVS
 * launch and index consumption while removing every vertex/instance-buffer
 * dependency.  Red output therefore distinguishes a broken attribute-fetch
 * path from a failure to launch/rasterize the indexed IDVS draw itself. */
static bool
winmali_glyph_probe_target_process(void)
{
   char path[MAX_PATH] = {0};
   if (!GetModuleFileNameA(NULL, path, ARRAY_SIZE(path)))
      return false;

   const char *name = strrchr(path, '\\');
   name = name ? name + 1 : path;
   return _stricmp(name, "StartMenuExperienceHost.exe") == 0 ||
          _stricmp(name, "ShellExperienceHost.exe") == 0 ||
          _stricmp(name, "ShellHost.exe") == 0 ||
          _stricmp(name, "Explorer.exe") == 0 ||
          _stricmp(name, "SystemSettings.exe") == 0;
}

static bool
winmali_glyph_probe_texture_shape(const nir_shader *shader)
{
   bool buffer = false;
   bool lookup_2d = false;

   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;

            const nir_tex_instr *tex = nir_instr_as_tex(instr);
            buffer |= tex->sampler_dim == GLSL_SAMPLER_DIM_BUF;
            lookup_2d |= tex->sampler_dim == GLSL_SAMPLER_DIM_2D;
         }
      }
   }

   return buffer && lookup_2d;
}

static bool
winmali_glyph_probe_vertex_shape(const nir_shader *shader)
{
   /* Captured DirectWrite/XAML glyph VS:
    * source_blake3: {0x15937609, 0x0d3ac49c, 0x2ac8cbb7, 0xa361c29e,
    *                 0x1e7587fc, 0x1938546b, 0x590d1b17, 0xcf3fd97d}
    *
    * Use the source identity rather than a broad input/output-count heuristic;
    * this is a destructive visual probe and must never affect unrelated UI
    * shaders in the selected processes. */
   static const uint32_t glyph_vs_blake3[8] = {
      0x15937609, 0x0d3ac49c, 0x2ac8cbb7, 0xa361c29e,
      0x1e7587fc, 0x1938546b, 0x590d1b17, 0xcf3fd97d,
   };

   return shader->info.stage == MESA_SHADER_VERTEX &&
          _mesa_printed_blake3_equal(shader->info.source_blake3,
                                     glyph_vs_blake3);
}

#if defined(WINMALI_LOOKUP_SPLIT_PROBE)
/* Exact DirectWrite/XAML glyph fragment shader captured on Windows 11 24H2.
 * Keep this destructive visual probe away from every other buffer/texture
 * shader used by Explorer and the shell. */
static bool
winmali_lookup_probe_fragment_shape(const nir_shader *shader)
{
   static const uint32_t glyph_fs_blake3[8] = {
      0x7d33757c, 0x58e6468f, 0x91062436, 0x935f41a4,
      0xf3b8f348, 0x19ed5d0b, 0xb30a1f59, 0x7ca838ba,
   };

   return shader->info.stage == MESA_SHADER_FRAGMENT &&
          _mesa_printed_blake3_equal(shader->info.source_blake3,
                                     glyph_fs_blake3) &&
          winmali_glyph_probe_texture_shape(shader);
}

struct winmali_lookup_probe {
   nir_tex_instr *metadata;
   nir_tex_instr *atlas;
   unsigned metadata_count;
   unsigned atlas_count;
   unsigned stores;
};

static void
winmali_lookup_probe_find_fetches(nir_shader *shader,
                                  struct winmali_lookup_probe *probe)
{
   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;

            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF) {
               probe->metadata = tex;
               probe->metadata_count++;
            } else if (tex->sampler_dim == GLSL_SAMPLER_DIM_2D) {
               probe->atlas = tex;
               probe->atlas_count++;
            }
         }
      }
   }
}

/* Preserve both real fetches, add one fixed-coordinate fetch through the same
 * atlas descriptor, and encode their results as an opaque diagnostic color:
 *
 *   R: the real R8_UINT metadata value is nonzero
 *   G: the real metadata-selected atlas sample has nonzero alpha
 *   B: atlas texel (128, original row) has any nonzero component
 *
 * The captured 256x80 lookup texture has known nonzero data at x=128 in both
 * rows used by this shader.  Thus blue proves the 2D descriptor/fetch path is
 * independently operational, even if the metadata-selected coordinate is
 * wrong.  Alpha also carries the three result bits as a unique opacity:
 * 1/8 baseline + R/8 + G/4 + B/2.  This keeps every result visible and remains
 * diagnostic when the Windows blend state treats the shader as a coverage
 * mask and does not preserve its RGB.  There are no readbacks, draw-path logs,
 * or CPU/GPU synchronization waits. */
static bool
winmali_lookup_probe_replace_output(nir_builder *b,
                                    nir_intrinsic_instr *intrin,
                                    void *data)
{
   struct winmali_lookup_probe *probe = data;
   if (intrin->intrinsic != nir_intrinsic_store_output ||
       probe->metadata_count != 1 || probe->atlas_count != 1)
      return false;

   nir_io_semantics semantics = nir_intrinsic_io_semantics(intrin);
   if (semantics.location != FRAG_RESULT_DATA0 ||
       semantics.dual_source_blend_index != 0)
      return false;

   int coord_index =
      nir_tex_instr_src_index(probe->atlas, nir_tex_src_coord);
   if (coord_index < 0)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *metadata = nir_channel(b, &probe->metadata->def, 0);
   nir_def *actual_atlas_alpha = nir_channel(b, &probe->atlas->def, 3);
   nir_def *actual_coord = probe->atlas->src[coord_index].src.ssa;
   nir_def *fixed_coord =
      nir_vec2(b, nir_imm_int(b, 128), nir_channel(b, actual_coord, 1));
   nir_def *fixed_atlas =
      nir_txf(b, fixed_coord,
              .lod = nir_imm_int(b, 0),
              .texture_index = probe->atlas->texture_index,
              .sampler_index = probe->atlas->sampler_index,
              .dim = probe->atlas->sampler_dim,
              .is_array = probe->atlas->is_array,
              .dest_type = probe->atlas->dest_type);

   nir_def *fixed_nonzero = nir_imm_false(b);
   for (unsigned i = 0; i < MIN2(fixed_atlas->num_components, 4); ++i)
      fixed_nonzero =
         nir_ior(b, fixed_nonzero,
                 nir_fneu_imm(b, nir_channel(b, fixed_atlas, i), 0.0f));

   nir_def *metadata_nonzero = nir_ine_imm(b, metadata, 0);
   nir_def *actual_nonzero =
      nir_fgt_imm(b, actual_atlas_alpha, 0.0f);
   nir_def *metadata_signal = nir_b2f32(b, metadata_nonzero);
   nir_def *actual_signal = nir_b2f32(b, actual_nonzero);
   nir_def *fixed_signal = nir_b2f32(b, fixed_nonzero);
   nir_def *alpha =
      nir_fadd(b, nir_imm_float(b, 0.125f),
               nir_fadd(b, nir_fmul_imm(b, metadata_signal, 0.125f),
                        nir_fadd(b, nir_fmul_imm(b, actual_signal, 0.25f),
                                 nir_fmul_imm(b, fixed_signal, 0.5f))));
   nir_def *color = nir_vec4(b, metadata_signal, actual_signal,
                             fixed_signal, alpha);
   nir_src_rewrite(&intrin->src[0], color);
   probe->stores++;
   return true;
}
#endif

struct winmali_glyph_vertex_probe {
   unsigned position_stores;
   unsigned varying_stores;
};

static bool
winmali_glyph_probe_replace_vertex_outputs(nir_builder *b,
                                           nir_intrinsic_instr *intrin,
                                           void *data)
{
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   struct winmali_glyph_vertex_probe *probe = data;
   nir_io_semantics semantics = nir_intrinsic_io_semantics(intrin);
   b->cursor = nir_before_instr(&intrin->instr);

   if (semantics.location == VARYING_SLOT_POS) {
      /* The six real R16 indices are 0,1,2,0,2,3.  Generate the four clip-space
       * corners from the post-index vertex ID, keeping index fetch and IDVS
       * execution in the experiment while bypassing all vertex buffers. */
      nir_def *vertex = nir_load_vertex_id_zero_base(b);
      nir_def *left = nir_ior(b, nir_ieq_imm(b, vertex, 0),
                              nir_ieq_imm(b, vertex, 3));
      nir_def *top = nir_ior(b, nir_ieq_imm(b, vertex, 0),
                             nir_ieq_imm(b, vertex, 1));
      nir_def *x = nir_bcsel(b, left, nir_imm_float(b, -1.0f),
                             nir_imm_float(b, 1.0f));
      nir_def *y = nir_bcsel(b, top, nir_imm_float(b, 1.0f),
                             nir_imm_float(b, -1.0f));
      nir_def *position = nir_vec4(b, x, y, nir_imm_float(b, 0.0f),
                                   nir_imm_float(b, 1.0f));
      nir_src_rewrite(&intrin->src[0], position);
      probe->position_stores++;
   } else {
      /* The paired constant-red FS no longer consumes varyings.  Zero their
       * stores so DCE can remove every original per-vertex/per-instance load. */
      nir_def *old = intrin->src[0].ssa;
      nir_src_rewrite(&intrin->src[0],
                      nir_imm_zero(b, old->num_components, old->bit_size));
      probe->varying_stores++;
   }

   return true;
}

static bool
winmali_glyph_probe_replace_output(nir_builder *b,
                                   nir_intrinsic_instr *intrin,
                                   void *data)
{
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   nir_io_semantics semantics = nir_intrinsic_io_semantics(intrin);
   if (semantics.location != FRAG_RESULT_DATA0)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);
   nir_def *color = semantics.dual_source_blend_index == 0
      ? nir_imm_vec4(b, 1.0f, 0.0f, 0.0f, 1.0f)
      : nir_imm_vec4(b, 1.0f, 1.0f, 1.0f, 1.0f);
   nir_src_rewrite(&intrin->src[0], color);
   (*(unsigned *)data)++;
   return true;
}

#if defined(WINMALI_ATTRIBUTE_PATH_PROBE)
/* Split the known indexed-instanced glyph draw into four independently useful
 * vertex-path experiments, selected by instance_id & 3.  All coordinates stay
 * inside the draw's real 279..334 x 15..25 scissor on the 1024x832 RK3588
 * target, so the diagnostic does not depend on changing rasterizer state:
 *
 *   0: indexed launch + vertex ID only (no vertex buffer dependency)
 *   1: per-vertex attribute 0 (R16G16_SINT corner) only
 *   2: per-instance attributes 1/2 (R16G16_SINT rectangle) + vertex ID
 *   3: the shader's original combined position
 *
 * The paired fragment probe writes opaque red.  The four results therefore
 * identify the first bad vertex-input boundary in one install without capture
 * files, resource readbacks, or broad shader matching. */
static nir_def *
winmali_attr_probe_pixel_position(nir_builder *b, nir_def *pixel_x,
                                  nir_def *pixel_y)
{
   nir_def *clip_x =
      nir_fadd(b, nir_fmul(b, pixel_x, nir_imm_float(b, 2.0f / 1024.0f)),
               nir_imm_float(b, -1.0f));
   nir_def *clip_y =
      nir_fadd(b, nir_fmul(b, pixel_y, nir_imm_float(b, -2.0f / 832.0f)),
               nir_imm_float(b, 1.0f));

   return nir_vec4(b, clip_x, clip_y, nir_imm_float(b, 0.5f),
                   nir_imm_float(b, 1.0f));
}

static bool
winmali_attr_probe_replace_vertex_outputs(nir_builder *b,
                                          nir_intrinsic_instr *intrin,
                                          void *data)
{
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   struct winmali_glyph_vertex_probe *probe = data;
   nir_io_semantics semantics = nir_intrinsic_io_semantics(intrin);
   b->cursor = nir_before_instr(&intrin->instr);

   if (semantics.location != VARYING_SLOT_POS) {
      /* The constant-red fragment shader consumes no varyings. */
      nir_def *old = intrin->src[0].ssa;
      nir_src_rewrite(&intrin->src[0],
                      nir_imm_zero(b, old->num_components, old->bit_size));
      probe->varying_stores++;
      return true;
   }

   nir_def *original = intrin->src[0].ssa;
   nir_def *vertex = nir_load_vertex_id_zero_base(b);
   nir_def *instance = nir_load_instance_id(b);
   nir_def *mode = nir_iand_imm(b, instance, 3);

   /* Indexed quad corners for the real 0,1,2,0,2,3 index sequence. */
   nir_def *left = nir_ior(b, nir_ieq_imm(b, vertex, 0),
                           nir_ieq_imm(b, vertex, 3));
   nir_def *top = nir_ior(b, nir_ieq_imm(b, vertex, 0),
                          nir_ieq_imm(b, vertex, 1));
   nir_def *corner_x = nir_bcsel(b, left, nir_imm_float(b, 0.0f),
                                 nir_imm_float(b, 1.0f));
   nir_def *corner_y = nir_bcsel(b, top, nir_imm_float(b, 0.0f),
                                 nir_imm_float(b, 1.0f));

   /* Path 0: index/IDVS launch only, at x=280..287. */
   nir_def *launch_x =
      nir_fadd(b, nir_imm_float(b, 280.0f),
               nir_fmul(b, corner_x, nir_imm_float(b, 7.0f)));
   nir_def *launch_y =
      nir_fadd(b, nir_imm_float(b, 16.0f),
               nir_fmul(b, corner_y, nir_imm_float(b, 8.0f)));
   nir_def *launch_pos =
      winmali_attr_probe_pixel_position(b, launch_x, launch_y);

   /* Path 1: consume only slot 0's signed integer corner values, at
    * x=292..299.  The explicit integer destination mirrors the D3D input
    * layout and prevents a hidden float interpretation in this probe. */
   nir_def *attr0 =
      nir_load_input(b, 4, 32, nir_imm_int(b, 0), .base = 0,
                     .dest_type = nir_type_int32);
   nir_def *attr0_x = nir_i2f32(b, nir_channel(b, attr0, 0));
   nir_def *attr0_y = nir_i2f32(b, nir_channel(b, attr0, 1));
   nir_def *vertex_x =
      nir_fadd(b, nir_imm_float(b, 292.0f),
               nir_fmul(b, attr0_x, nir_imm_float(b, 7.0f)));
   nir_def *vertex_y =
      nir_fadd(b, nir_imm_float(b, 16.0f),
               nir_fmul(b, attr0_y, nir_imm_float(b, 8.0f)));
   nir_def *vertex_pos =
      winmali_attr_probe_pixel_position(b, vertex_x, vertex_y);

   /* Path 2: consume only the two per-instance rectangle attributes. */
   nir_def *attr1 =
      nir_load_input(b, 4, 32, nir_imm_int(b, 0), .base = 1,
                     .dest_type = nir_type_int32);
   nir_def *attr2 =
      nir_load_input(b, 4, 32, nir_imm_int(b, 0), .base = 2,
                     .dest_type = nir_type_int32);
   nir_def *origin_x = nir_i2f32(b, nir_channel(b, attr1, 0));
   nir_def *origin_y = nir_i2f32(b, nir_channel(b, attr1, 1));
   nir_def *end_x = nir_i2f32(b, nir_channel(b, attr2, 0));
   nir_def *end_y = nir_i2f32(b, nir_channel(b, attr2, 1));
   nir_def *instance_x =
      nir_fadd(b, origin_x,
               nir_fmul(b, corner_x, nir_fsub(b, end_x, origin_x)));
   nir_def *instance_y =
      nir_fadd(b, origin_y,
               nir_fmul(b, corner_y, nir_fsub(b, end_y, origin_y)));
   nir_def *instance_pos =
      winmali_attr_probe_pixel_position(b, instance_x, instance_y);

   nir_def *position =
      nir_bcsel(b, nir_ieq_imm(b, mode, 0), launch_pos,
                nir_bcsel(b, nir_ieq_imm(b, mode, 1), vertex_pos,
                          nir_bcsel(b, nir_ieq_imm(b, mode, 2), instance_pos,
                                    original)));
   nir_src_rewrite(&intrin->src[0], position);
   probe->position_stores++;
   return true;
}
#endif
#endif

static struct panfrost_uncompiled_shader *
panfrost_alloc_shader(const nir_shader *nir)
{
   struct panfrost_uncompiled_shader *so =
      rzalloc(NULL, struct panfrost_uncompiled_shader);

   simple_mtx_init(&so->lock, mtx_plain);
   util_dynarray_init(&so->variants, so);

   so->nir = nir;

   /* Serialize the NIR to a binary blob that we can hash for the disk
    * cache. Drop unnecessary information (like variable names) so the
    * serialized NIR is smaller, and also to let us detect more isomorphic
    * shaders when hashing, increasing cache hits.
    */
   struct blob blob;
   blob_init(&blob);
   nir_serialize(&blob, nir, true);
   _mesa_blake3_compute(blob.data, blob.size, so->nir_blake3);
   blob_finish(&blob);

   return so;
}

static struct panfrost_compiled_shader *
panfrost_alloc_variant(struct panfrost_uncompiled_shader *so)
{
   return util_dynarray_grow(&so->variants, struct panfrost_compiled_shader, 1);
}

static bool
lower_load_poly_line_smooth_enabled(nir_builder *b, nir_intrinsic_instr *intrin,
                                    void *data)
{
   if (intrin->intrinsic != nir_intrinsic_load_poly_line_smooth_enabled)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);
   nir_def_replace(&intrin->def, nir_imm_true(b));
   return true;
}

/* From the OpenGL 4.6 spec 14.3.1:
 *
 *    If MULTISAMPLE is disabled, multisample rasterization of all primitives
 *    is equivalent to single-sample (fragment-center) rasterization, except
 *    that the fragment coverage value is set to full coverage.
 *
 * So always use the original sample mask when multisample is disabled */
static bool
lower_sample_mask_writes(nir_builder *b, nir_intrinsic_instr *intrin,
                         void *data)
{
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   if (nir_intrinsic_io_semantics(intrin).location != FRAG_RESULT_SAMPLE_MASK)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *orig = nir_load_sample_mask(b);
   nir_def *new = nir_b32csel(b, nir_load_multisampled_pan(b),
                               intrin->src[0].ssa, orig);
   nir_src_rewrite(&intrin->src[0], new);

   return true;
}

static bool
lower_tgsi_dual_source_output(nir_builder *b,
                              nir_intrinsic_instr *intrin,
                              void *data)
{
   (void)b;

   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   nir_io_semantics semantics = nir_intrinsic_io_semantics(intrin);

   /* The D3D10 frontend represents o1 as TGSI COLOR[1].  tgsi_to_nir has
    * to preserve that as MRT1 because shader creation has no blend state.
    * Once a dual-source blend variant is selected, reinterpret precisely
    * that output as the secondary source for RT0. */
   if (semantics.location != FRAG_RESULT_DATA1 ||
       semantics.dual_source_blend_index != 0)
      return false;

   semantics.location = FRAG_RESULT_DATA0;
   semantics.dual_source_blend_index = 1;
   nir_intrinsic_set_io_semantics(intrin, semantics);
   if (data)
      (*(unsigned *)data)++;
   return true;
}

static void
panfrost_shader_compile(struct panfrost_screen *screen, const nir_shader *ir,
                        struct util_debug_callback *dbg,
                        const struct pan_varying_layout *varying_layout,
                        struct panfrost_shader_key *key, unsigned req_local_mem,
                        struct panfrost_shader_binary *out)
{
   PAN_TRACE_FUNC(PAN_TRACE_GL_SHADER);

   struct panfrost_device *dev = pan_device(&screen->base);

   nir_shader *s = nir_shader_clone(NULL, ir);

#if defined(WINMALI_SHADER_CAPTURE)
   char capture_parent[65] = {0};
   char capture_final[65] = {0};
   if ((s->info.stage == MESA_SHADER_VERTEX ||
        s->info.stage == MESA_SHADER_FRAGMENT) &&
       winmali_capture_target_process())
      winmali_capture_nir_hash(ir, capture_parent);
#endif

   /* While graphics shaders are preprocessed at CSO create time, compute
    * kernels are not preprocessed until they're cloned since the driver does
    * not get ownership of the NIR from compute CSOs. Do this preprocessing now.
    * Compute CSOs call this function during create time, so preprocessing
    * happens at CSO create time regardless.
    */
   if (mesa_shader_stage_is_compute(s->info.stage)) {
      pan_preprocess_nir(s, panfrost_device_gpu_id(dev));
   }

   struct pan_compile_inputs inputs = {
      .gpu_id = panfrost_device_gpu_id(dev),
      .gpu_variant = dev->kmod.dev->props.gpu_variant,
   };

   /* Lower this early so the backends don't have to worry about it */
   if (s->info.stage == MESA_SHADER_VERTEX) {
      /* No IDVS for internal XFB shaders */
      inputs.no_idvs = s->info.has_transform_feedback_varyings;

#if defined(WINMALI_GLYPH_PIPELINE_PROBE)
      if (winmali_glyph_probe_target_process() &&
          winmali_glyph_probe_vertex_shape(s)) {
         struct winmali_glyph_vertex_probe probe = {0};
         bool replaced = false;
         NIR_PASS(replaced, s, nir_shader_intrinsics_pass,
                  winmali_glyph_probe_replace_vertex_outputs,
                  nir_metadata_control_flow, &probe);

         /* The normal preprocessing pass ran when the shader state was
          * created, before this compile-time probe introduced the system
          * value.  Perform the same required Panfrost lowering here so the
          * backend sees vertex_id - raw_vertex_offset, not the unsupported
          * load_vertex_id_zero_base intrinsic. */
         NIR_PASS(_, s, pan_nir_lower_vertex_id_zero_base);

         /* Eliminate the original attribute/instance loads after all shader
          * outputs have been detached from them.  The lowered vertex-ID
          * expression remains live and continues to exercise indexed IDVS. */
         bool progress;
         do {
            bool dce_progress = false;
            bool cf_progress = false;
            NIR_PASS(dce_progress, s, nir_opt_dce);
            NIR_PASS(cf_progress, s, nir_opt_dead_cf);
            progress = dce_progress || cf_progress;
         } while (progress);
         nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
         mesa_logi("[WinMaliGlyphProbe] ConstantPosition shader=%p "
                   "replaced=%u positionStores=%u varyingStores=%u "
                   "source=vertex-id quad=fullscreen indexed-idvs=preserved",
                   ir, replaced, probe.position_stores,
                   probe.varying_stores);
      }
#endif

#if defined(WINMALI_ATTRIBUTE_PATH_PROBE)
      if (winmali_glyph_probe_target_process() &&
          winmali_glyph_probe_vertex_shape(s)) {
         struct winmali_glyph_vertex_probe probe = {0};
         bool replaced = false;
         NIR_PASS(replaced, s, nir_shader_intrinsics_pass,
                  winmali_attr_probe_replace_vertex_outputs,
                  nir_metadata_control_flow, &probe);

         /* The probe introduces vertex_id_zero_base after normal CSO-time
          * preprocessing, so perform the required Panfrost lowering here. */
         NIR_PASS(_, s, pan_nir_lower_vertex_id_zero_base);

         bool progress;
         do {
            bool dce_progress = false;
            bool cf_progress = false;
            NIR_PASS(dce_progress, s, nir_opt_dce);
            NIR_PASS(cf_progress, s, nir_opt_dead_cf);
            progress = dce_progress || cf_progress;
         } while (progress);
         nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
         mesa_logi("[WinMaliAttrProbe] FourPath shader=%p replaced=%u "
                   "positionStores=%u varyingStores=%u modes="
                   "launch,vertex0,instance12,original target=1024x832",
                   ir, replaced, probe.position_stores,
                   probe.varying_stores);
      }
#endif

      if (s->info.has_transform_feedback_varyings) {
         NIR_PASS(_, s, nir_opt_constant_folding);
         NIR_PASS(_, s, nir_io_add_intrinsic_xfb_info);
         NIR_PASS(_, s, pan_nir_lower_xfb);
      }
   }

   /* nir_opt_varyings is replacing all flat highp types with float32, we need
    * to figure out the varying types ourselves */
   inputs.trust_varying_flat_highp_types = false;
   inputs.varying_layout = varying_layout;

#if defined(WINMALI_LD_VAR_FALLBACK_PROBE)
   if (winmali_varying_diag_fragment_shape(s) && varying_layout != NULL) {
      /*
       * Force the documented descriptor-backed fallback for only the exact
       * DirectWrite/XAML glyph shader.  Without a concrete producer layout,
       * pan_nir_lower_fs_inputs() emits LD_VAR instead of LD_VAR_BUF.  The
       * normal draw-state path then supplies the varying descriptors from the
       * still-current VS layout; no VS output, packet stride, or application
       * state is changed.
       */
      inputs.varying_layout = NULL;
      mesa_logi("[WinMaliLdVarProbe] ForceGeneric shader=%p "
                "originalKnown=0x%x count=%u genericSize=%u",
                s, varying_layout->known, varying_layout->count,
                varying_layout->generic_size_B);
   }
#endif

   if (s->info.stage == MESA_SHADER_FRAGMENT) {
      if (key->fs.dual_source_blend) {
         bool lowered_dual_source = false;
         unsigned rewritten_outputs = 0;
         NIR_PASS(lowered_dual_source, s, nir_shader_intrinsics_pass,
                  lower_tgsi_dual_source_output,
                  nir_metadata_control_flow, &rewritten_outputs);
         nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
#if defined(WINMALI_LOG_GLYPH)
         mesa_logi("[WinMaliText] DualSourceRewrite shader=%p changed=%u "
                   "stores=%u outputs=0x%llx dual=%u",
                   ir, lowered_dual_source, rewritten_outputs,
                   (unsigned long long)s->info.outputs_written,
                   s->info.fs.color_is_dual_source);
#endif
         mesa_logi("[WinMaliFlow] DualSourceVariant shader=%p lowered=%u outputs=0x%llx dual=%u",
                   ir, lowered_dual_source,
                   (unsigned long long)s->info.outputs_written,
                   s->info.fs.color_is_dual_source);
      }

#if defined(WINMALI_GLYPH_PIPELINE_PROBE) || \
    defined(WINMALI_ATTRIBUTE_PATH_PROBE)
      if (winmali_glyph_probe_target_process() &&
          winmali_glyph_probe_texture_shape(s)) {
         unsigned stores = 0;
         bool replaced = false;
         NIR_PASS(replaced, s, nir_shader_intrinsics_pass,
                  winmali_glyph_probe_replace_output,
                  nir_metadata_control_flow, &stores);

         /* Once the output no longer depends on either texture operation,
          * eliminate the dead R8 LD_CVT and 2D TEX_FETCH.  This makes a red
          * result an unambiguous proof of fragment execution rather than a
          * fetch which happened to return a convenient value. */
         bool progress;
         do {
            bool dce_progress = false;
            bool cf_progress = false;
            NIR_PASS(dce_progress, s, nir_opt_dce);
            NIR_PASS(cf_progress, s, nir_opt_dead_cf);
            progress = dce_progress || cf_progress;
         } while (progress);
         nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
#if defined(WINMALI_ATTRIBUTE_PATH_PROBE)
         mesa_logi("[WinMaliAttrProbe] ConstantOutput shader=%p "
                   "dualSource=%u replaced=%u stores=%u primary=red",
                   ir, key->fs.dual_source_blend, replaced, stores);
#else
         mesa_logi("[WinMaliGlyphProbe] ConstantOutput shader=%p "
                   "dualSource=%u replaced=%u stores=%u primary=red",
                   ir, key->fs.dual_source_blend, replaced, stores);
#endif
      }
#endif

#if defined(WINMALI_LOOKUP_SPLIT_PROBE)
      if (winmali_glyph_probe_target_process() &&
          winmali_lookup_probe_fragment_shape(s)) {
         struct winmali_lookup_probe probe = {0};
         winmali_lookup_probe_find_fetches(s, &probe);
         bool replaced = false;
         NIR_PASS(replaced, s, nir_shader_intrinsics_pass,
                  winmali_lookup_probe_replace_output,
                  nir_metadata_control_flow, &probe);
         nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
         mesa_logi("[WinMaliLookupProbe] Split shader=%p replaced=%u "
                   "stores=%u metadataFetches=%u atlasFetches=%u "
                   "channels=R8-nonzero,actual-alpha,fixed-atlas "
                   "alpha=1/8+R/8+G/4+B/2",
                   ir, replaced, probe.stores, probe.metadata_count,
                   probe.atlas_count);
      }
#endif

      if (key->fs.nr_cbufs_for_fragcolor) {
         NIR_PASS(_, s, panfrost_nir_remove_fragcolor_stores,
                  key->fs.nr_cbufs_for_fragcolor);
      }

      if (key->fs.sprite_coord_enable) {
         NIR_PASS(_, s, nir_lower_texcoord_replace_late,
                  key->fs.sprite_coord_enable,
                  true /* point coord is sysval */);
         /* Lower load_point_coord if present */
         NIR_PASS(_, s, pan_nir_lower_var_special_pan);
      }

      if (key->fs.clip_plane_enable) {
         NIR_PASS(_, s, nir_lower_clip_fs, key->fs.clip_plane_enable,
                  false, true);
      }

      if (key->fs.line_smooth) {
         NIR_PASS(_, s, nir_lower_poly_line_smooth, 16);
         NIR_PASS(_, s, nir_shader_intrinsics_pass,
                  lower_load_poly_line_smooth_enabled,
                  nir_metadata_control_flow, key);
         NIR_PASS(_, s, nir_lower_alu);
      }

      NIR_PASS(_, s, nir_shader_intrinsics_pass,
               lower_sample_mask_writes, nir_metadata_control_flow, NULL);

      if (s->info.fs.accesses_pixel_local_storage)
         NIR_PASS(_, s, panfrost_nir_lower_pls, screen);
   }

   if (dev->arch <= 5 && s->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, s, pan_nir_lower_framebuffer, key->fs.rt_formats,
               pan_raw_format_mask_midgard(key->fs.rt_formats), 0,
               panfrost_device_gpu_prod_id(dev) < 0x700);
   }

   /* Lower resource indices */
   NIR_PASS(_, s, panfrost_nir_lower_res_indices, inputs.gpu_id);

   pan_postprocess_nir(s, &inputs, &out->info);

#if defined(WINMALI_FRAG_W_BYPASS_PROBE)
   if (winmali_varying_diag_fragment_shape(s)) {
      unsigned replaced = 0;

      NIR_PASS(_, s, nir_shader_intrinsics_pass, winmali_frag_w_bypass,
               nir_metadata_control_flow, &replaced);
      NIR_PASS(_, s, nir_opt_dce);
      nir_shader_gather_info(s, nir_shader_get_entrypoint(s));

      mesa_logi("[WinMaliFragWProbe] Bypass shader=%p replaced=%u "
                "value=1.0 scope=exact-glyph-fs",
                s, replaced);
   }
#endif

   if (s->info.stage == MESA_SHADER_VERTEX) {
      NIR_PASS(_, s, nir_inline_sysval,
               nir_intrinsic_load_noperspective_varyings_pan,
               key->vs.noperspective_varyings);
   }

   if (dev->arch >= 9 && mesa_shader_stage_is_compute(s->info.stage)) {
      out->info.cs.allow_merging_workgroups = valhall_can_merge_workgroups(s);
   }

   NIR_PASS(_, s, panfrost_nir_lower_sysvals, dev->arch, &out->sysvals);

   /* For now, we only allow pushing the default UBO 0, and the sysval UBO (if
    * present). Both of these are mapped on the CPU, but other UBOs are not.
    * When we switch to pushing UBOs with a compute kernel (or CSF instructions)
    * we can relax this. */
   assert(s->info.first_ubo_is_default_ubo);
   inputs.pushable_ubos = BITFIELD_BIT(0);

   if (out->sysvals.sysval_count != 0) {
      inputs.pushable_ubos |= BITFIELD_BIT(PAN_UBO_SYSVALS);
   }

   if (dev->arch >= 9) {
      /* Always enable this for GL, it avoids crashes when using unbound
       * resources. */
      inputs.robust_descriptors = true;
   }

#if defined(WINMALI_SHADER_CAPTURE)
   if (s->info.stage == MESA_SHADER_VERTEX ||
       s->info.stage == MESA_SHADER_FRAGMENT)
      winmali_capture_nir(s, "compiler-input", capture_parent, capture_final);
#endif

   out->binary = UTIL_DYNARRAY_INIT;
   screen->vtbl.compile_shader(s, &inputs, &out->binary, &out->info);

#if defined(WINMALI_SHADER_CAPTURE)
   if (s->info.stage == MESA_SHADER_VERTEX ||
       s->info.stage == MESA_SHADER_FRAGMENT)
      winmali_capture_binary(&out->binary, s->info.stage, capture_final);
#endif

   /* Report stats only if we really got the shader compiled */
   if (out->binary.size > 0) {
      if (s->info.stage == MESA_SHADER_VERTEX &&
          out->info.vs.secondary_offset) {
         pan_stats_util_debug(dbg, "MESA_SHADER_POSITION",
                              &out->info.stats);
         pan_stats_util_debug(dbg, "MESA_SHADER_VERTEX",
                              &out->info.stats_idvs_varying);
      } else {
         pan_stats_util_debug(dbg, mesa_shader_stage_name(s->info.stage),
                              &out->info.stats);
      }
   }

   assert(req_local_mem >= out->info.wls_size);
   out->info.wls_size = req_local_mem;

   /* In both clone and tgsi_to_nir paths, the shader is ralloc'd against
    * a NULL context
    */
   ralloc_free(s);
}

static void
panfrost_shader_get(struct pipe_screen *pscreen,
                    struct panfrost_pool *shader_pool,
                    struct panfrost_pool *desc_pool,
                    struct panfrost_uncompiled_shader *uncompiled,
                    struct util_debug_callback *dbg,
                    struct panfrost_compiled_shader *state,
                    unsigned req_local_mem)
{
   struct panfrost_screen *screen = pan_screen(pscreen);
   struct panfrost_device *dev = pan_device(pscreen);

   struct panfrost_shader_binary res = {0};

   /* Try to retrieve the variant from the disk cache. If that fails,
    * compile a new variant and store in the disk cache for later reuse.
    */
   bool cache_hit = panfrost_disk_cache_retrieve(screen->disk_cache, uncompiled,
                                                 &state->key, &res);
   if (!cache_hit) {

      const struct pan_varying_layout *varying_layout = NULL;

      /* Fragment variants are keyed by the layout of the vertex shader they
       * are used with.  In particular, separate shaders never receive that
       * layout through pipe_context::link_shader, so consulting the mutable
       * uncompiled FS object here incorrectly leaves them on the generic
       * LD_VAR path.  Compile from the layout recorded in this exact variant
       * key so the binary, in-memory cache key, and disk-cache key agree.
       */
      if (uncompiled->nir->info.stage == MESA_SHADER_FRAGMENT &&
          state->key.fs.vs_varying_layout.known != 0)
         varying_layout = &state->key.fs.vs_varying_layout;
      else if (uncompiled->vs_varying_layout.known != 0)
         varying_layout = &uncompiled->vs_varying_layout;
      panfrost_shader_compile(screen, uncompiled->nir, dbg, varying_layout,
                              &state->key, req_local_mem, &res);

      panfrost_disk_cache_store(screen->disk_cache, uncompiled, &state->key,
                                &res);
   }

   state->info = res.info;
   state->sysvals = res.sysvals;

#if defined(WINMALI_LOG_FOCUSED)
   if (winmali_varying_diag_fragment_shape(uncompiled->nir)) {
      const struct pan_varying_layout *layout =
         &state->key.fs.vs_varying_layout;
      const struct pan_varying_slot *var0 =
         pan_varying_layout_find_slot(layout, VARYING_SLOT_VAR0);
      const struct pan_varying_slot *var1 =
         pan_varying_layout_find_slot(layout, VARYING_SLOT_VAR1);

      mesa_logi("[WinMaliVarying] FSVariant shader=%p cacheHit=%u "
                "layoutKnown=0x%x count=%u genericSize=%u "
                "var0=%d/%u/%u/%u var1=%d/%u/%u/%u "
                "usesGenericLdVar=%u expected=%s",
                uncompiled, cache_hit, layout->known, layout->count,
                layout->generic_size_B,
                var0 ? var0->offset : -1, var0 ? var0->section : 0,
                var0 ? var0->ncomps : 0, var0 ? var0->alu_type : 0,
                var1 ? var1->offset : -1, var1 ? var1->section : 0,
                var1 ? var1->ncomps : 0, var1 ? var1->alu_type : 0,
                state->info.bifrost.uses_ld_var,
                state->info.bifrost.uses_ld_var ? "LD_VAR" : "LD_VAR_BUF");
   }
#endif

   if (res.binary.size) {
      size_t binary_size =
         util_dynarray_num_elements(&res.binary, uint8_t);

      state->bin = panfrost_pool_take_ref(
         shader_pool,
         pan_pool_upload_aligned(&shader_pool->base, res.binary.data,
                                 res.binary.size, 128));

      bool is_vertex =
         uncompiled->nir->info.stage == MESA_SHADER_VERTEX;

      mesa_logi("[WinMaliShader] Upload stage=%s state=%p bytes=%llu "
                "gpu=0x%llx bo=%p handle=%u boGpu=0x%llx boSize=%llu "
                "noPsiz=0x%x secondaryEnable=%u secondary=0x%x",
                mesa_shader_stage_name(uncompiled->nir->info.stage), state,
                (unsigned long long)binary_size,
                (unsigned long long)state->bin.gpu, state->bin.bo,
                state->bin.bo ? (unsigned)panfrost_bo_handle(state->bin.bo) : 0,
                state->bin.bo ? (unsigned long long)state->bin.bo->ptr.gpu : 0,
                state->bin.bo
                   ? (unsigned long long)panfrost_bo_size(state->bin.bo)
                   : 0,
                is_vertex ? state->info.vs.no_psiz_offset : 0,
                is_vertex ? state->info.vs.secondary_enable : 0,
                is_vertex ? state->info.vs.secondary_offset : 0);
   }

   util_dynarray_fini(&res.binary);

   /* Don't upload RSD for fragment shaders since they need draw-time
    * merging for e.g. depth/stencil/alpha. RSDs are replaced by simpler
    * shader program descriptors on Valhall, which can be preuploaded even
    * for fragment shaders. */
   bool upload =
      !(uncompiled->nir->info.stage == MESA_SHADER_FRAGMENT && dev->arch <= 7);
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] Prepare ENTER state=%p stage=%s "
             "upload=%u sysvals=%u",
             state, mesa_shader_stage_name(uncompiled->nir->info.stage),
             upload, state->sysvals.sysval_count);
   screen->vtbl.prepare_shader(state, desc_pool, upload);
#if defined(WINMALI_IDVS_HANDOFF_PROBE) && PAN_ARCH < 12
   if (winmali_glyph_probe_vertex_shape(uncompiled->nir)) {
      mesa_logi("[WinMaliIDVS] Compile state=%p idvs=%u binary=0x%llx "
                "noPsiz=0x%x secondaryEnable=%u "
                "secondaryOffset=0x%x positionRegs=%u secondaryRegs=%u "
                "genericSize=%u descBase=0x%llx",
                state, state->info.vs.idvs,
                (unsigned long long)state->bin.gpu,
                state->info.vs.no_psiz_offset,
                state->info.vs.secondary_enable,
                state->info.vs.secondary_offset,
                state->info.work_reg_count,
                state->info.vs.secondary_work_reg_count,
                state->info.varyings.formats.generic_size_B,
                (unsigned long long)state->state.gpu);
   }
#endif
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] Prepare EXIT state=%p desc=0x%llx "
             "sysvals=%u",
             state, (unsigned long long)state->state.gpu,
             state->sysvals.sysval_count);

   WM_SHADER_INIT_LOG("[WinMaliShaderInit] AnalyzeSysvals CALL state=%p count=%u",
             state, state->sysvals.sysval_count);
   panfrost_analyze_sysvals(state);
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] ShaderGet EXIT state=%p dirty3d=0x%x "
             "dirtyShader=0x%x",
             state, state->dirty_3d, state->dirty_shader);
}

static void
panfrost_build_vs_key(struct panfrost_context *ctx,
                      struct panfrost_vs_key *key,
                      struct panfrost_uncompiled_shader *uncompiled)
{
   struct panfrost_uncompiled_shader *fs = ctx->uncompiled[MESA_SHADER_FRAGMENT];

   assert(fs != NULL && "too early");
   key->noperspective_varyings = fs->noperspective_varyings;
}

static void
panfrost_build_fs_key(struct panfrost_context *ctx,
                      struct panfrost_fs_key *key,
                      struct panfrost_uncompiled_shader *uncompiled)
{
   const nir_shader *nir = uncompiled->nir;

   struct panfrost_device *dev = pan_device(ctx->base.screen);
   struct pipe_framebuffer_state *fb = &ctx->pipe_framebuffer;
   struct pipe_rasterizer_state *rast = (void *)ctx->rasterizer;

   /* gl_FragColor lowering needs the number of colour buffers */
   if (uncompiled->fragcolor_lowered) {
      key->nr_cbufs_for_fragcolor = fb->nr_cbufs;
   }

   /* Point sprite lowering needed on Bifrost and newer */
   if (dev->arch >= 6 && rast && ctx->active_prim == MESA_PRIM_POINTS) {
      key->sprite_coord_enable = rast->sprite_coord_enable;
   }

   /* User clip plane lowering needed everywhere */
   if (rast) {
      key->clip_plane_enable = rast->clip_plane_enable;

      if (u_reduced_prim(ctx->active_prim) == MESA_PRIM_LINES)
         key->line_smooth = rast->line_smooth;
   }

   /* The currently bound VS is the producer for this draw, including when
    * the NIR is marked separate_shader and Gallium skips link_shader().
    * panfrost_bind_vs_state() already refreshes the FS variant, so key the
    * variant with that producer's concrete layout instead of the generally
    * unknown layout stored on a separate FS object.
    */
   struct panfrost_uncompiled_shader *vs =
      ctx->uncompiled[MESA_SHADER_VERTEX];
   key->vs_varying_layout =
      vs ? vs->vs_varying_layout : uncompiled->vs_varying_layout;

   key->dual_source_blend =
      ctx->blend && ctx->blend->base.rt[0].blend_enable &&
      util_blend_state_is_dual(&ctx->blend->base, 0);

#if defined(WINMALI_LOG_GLYPH)
   const bool writes_data1 =
      (nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DATA1)) != 0;
   if (key->dual_source_blend || writes_data1) {
      mesa_logi("[WinMaliText] VariantKey shader=%p outputs=0x%llx "
                "writesData1=%u dualBlend=%u blend=%p",
                uncompiled, (unsigned long long)nir->info.outputs_written,
                writes_data1, key->dual_source_blend, ctx->blend);
   }
#endif

   if (dev->arch <= 5) {
      u_foreach_bit(i, (nir->info.outputs_read >> FRAG_RESULT_DATA0)) {
         enum pipe_format fmt = PIPE_FORMAT_R8G8B8A8_UNORM;

         if ((fb->nr_cbufs > i) && fb->cbufs[i].texture)
            fmt = fb->cbufs[i].format;

         if (pan_blendable_formats_v6[fmt].internal)
            fmt = PIPE_FORMAT_NONE;

         key->rt_formats[i] = fmt;
      }
   }
}

static void
panfrost_build_key(struct panfrost_context *ctx,
                   struct panfrost_shader_key *key,
                   struct panfrost_uncompiled_shader *uncompiled)
{
   const nir_shader *nir = uncompiled->nir;

   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      panfrost_build_vs_key(ctx, &key->vs, uncompiled);
      break;
   case MESA_SHADER_FRAGMENT:
      panfrost_build_fs_key(ctx, &key->fs, uncompiled);
      break;
   default:
      break;
   }
}

static struct panfrost_compiled_shader *
panfrost_new_variant_locked(struct panfrost_context *ctx,
                            struct panfrost_uncompiled_shader *uncompiled,
                            struct panfrost_shader_key *key)
{
   struct panfrost_device *dev = pan_device(ctx->base.screen);
   struct panfrost_compiled_shader *prog = panfrost_alloc_variant(uncompiled);

   *prog = (struct panfrost_compiled_shader){
      .key = *key,
      .stream_output = uncompiled->stream_output,
   };

   panfrost_shader_get(ctx->base.screen, &ctx->shaders, &ctx->descs, uncompiled,
                       &ctx->base.debug, prog, 0);

   prog->earlyzs = pan_earlyzs_analyze(&prog->info, dev->arch);

   return prog;
}

static void
panfrost_bind_shader_state(struct pipe_context *pctx, void *hwcso,
                           mesa_shader_stage type)
{
   struct panfrost_context *ctx = pan_context(pctx);
   ctx->uncompiled[type] = hwcso;
   ctx->prog[type] = NULL;

   ctx->dirty |= PAN_DIRTY_TLS_SIZE;
   ctx->dirty_shader[type] |= PAN_DIRTY_STAGE_SHADER;

   if (hwcso)
      panfrost_update_shader_variant(ctx, type);
}

void
panfrost_update_shader_variant(struct panfrost_context *ctx,
                               mesa_shader_stage type)
{
   /* No shader variants for compute */
   if (type == MESA_SHADER_COMPUTE)
      return;

   /* We need linking information, defer this */
   if ((type == MESA_SHADER_FRAGMENT && !ctx->uncompiled[MESA_SHADER_VERTEX]) ||
       (type == MESA_SHADER_VERTEX && !ctx->uncompiled[MESA_SHADER_FRAGMENT]))
      return;

   /* Also defer, happens with GALLIUM_HUD */
   if (!ctx->uncompiled[type])
      return;

   /* Match the appropriate variant */
   struct panfrost_uncompiled_shader *uncompiled = ctx->uncompiled[type];
   struct panfrost_compiled_shader *old_compiled = ctx->prog[type];
   struct panfrost_compiled_shader *compiled = NULL;

   simple_mtx_lock(&uncompiled->lock);

   struct panfrost_shader_key key = {0};
   panfrost_build_key(ctx, &key, uncompiled);

   util_dynarray_foreach(&uncompiled->variants, struct panfrost_compiled_shader,
                         so) {
      if (memcmp(&key, &so->key, sizeof(key)) == 0) {
         compiled = so;
         break;
      }
   }

   if (compiled == NULL)
      compiled = panfrost_new_variant_locked(ctx, uncompiled, &key);

   ctx->prog[type] = compiled;

   /*
    * A variant can change without the shader CSO itself being rebound.  In
    * particular, binding a VS selects a new linked FS variant because Valhall
    * LD_VAR_BUF instructions bake the producer's varying layout into the FS.
    * Likewise, draw-state keys can select another variant at draw time.
    *
    * The old code updated ctx->prog[] but left the stage clean.  The command
    * stream could consequently retain the previous shader program and its
    * resource table even though ctx->prog[] pointed at the new variant.
    * Dirtify every state item whose contents are derived from the compiled
    * variant whenever that pointer changes.
    */
   if (compiled != old_compiled) {
      ctx->dirty |= PAN_DIRTY_TLS_SIZE | compiled->dirty_3d;
      ctx->dirty_shader[type] |= compiled->dirty_shader;
   }

   simple_mtx_unlock(&uncompiled->lock);
}

static void
panfrost_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
   panfrost_bind_shader_state(pctx, hwcso, MESA_SHADER_VERTEX);

   /* Fragment shaders are linked with vertex shaders */
   struct panfrost_context *ctx = pan_context(pctx);
   panfrost_update_shader_variant(ctx, MESA_SHADER_FRAGMENT);
}

static void
panfrost_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
   panfrost_bind_shader_state(pctx, hwcso, MESA_SHADER_FRAGMENT);

   /* Vertex shaders are linked with fragment shaders */
   struct panfrost_context *ctx = pan_context(pctx);
   panfrost_update_shader_variant(ctx, MESA_SHADER_VERTEX);
}

static int
glsl_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static struct panfrost_shader_key
panfrost_default_shader_key(struct panfrost_uncompiled_shader *so)
{
   struct panfrost_shader_key key = {0};

   if (so->nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* gl_FragColor lowering needs the number of colour buffers on desktop
      * GL, where it acts as an implicit broadcast to all colour buffers.
      *
      * However, gl_FragColor is a legacy feature, so assume that if
      * gl_FragColor is used, there is only a single render target. The
      * implicit broadcast is neither especially useful nor required by GLES.
      */
      if (so->fragcolor_lowered)
         key.fs.nr_cbufs_for_fragcolor = 1;

      key.fs.vs_varying_layout = so->vs_varying_layout;
   }

   return key;
}

static void *
panfrost_create_shader_state(struct pipe_context *pctx,
                             const struct pipe_shader_state *cso)
{
   PAN_TRACE_FUNC(PAN_TRACE_GL_SHADER);

   WM_SHADER_INIT_LOG("[WinMaliShaderInit] CreateState ENTER pctx=%p cso=%p "
             "irType=%u tokens=%p",
             pctx, cso, cso->type, cso->tokens);

   nir_shader *nir;
   if (cso->type == PIPE_SHADER_IR_TGSI) {
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] TGSItoNIR ENTER pctx=%p tokens=%p",
                pctx, cso->tokens);
      nir = tgsi_to_nir(cso->tokens, pctx->screen, false);
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] TGSItoNIR EXIT pctx=%p tokens=%p "
                "nir=%p stage=%d",
                pctx, cso->tokens, nir, nir ? nir->info.stage : -1);
   } else {
      nir = cso->ir.nir;
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] CreateState NIR input pctx=%p nir=%p "
                "stage=%d",
                pctx, nir, nir ? nir->info.stage : -1);
   }

#if defined(WINMALI_SHADER_CAPTURE)
   uint64_t capture_tgsi_hash = 0;
   char capture_initial[65] = {0};
   char capture_lowered[65] = {0};
   if (winmali_capture_target_process() &&
       cso->type == PIPE_SHADER_IR_TGSI &&
       nir && (nir->info.stage == MESA_SHADER_VERTEX ||
               nir->info.stage == MESA_SHADER_FRAGMENT)) {
      unsigned count = tgsi_num_tokens(cso->tokens);
      capture_tgsi_hash = winmali_capture_hash(
         cso->tokens, (size_t)count * sizeof(*cso->tokens));
      winmali_capture_nir(nir, "tgsi-output", NULL, capture_initial);
   }
#endif

   WM_SHADER_INIT_LOG("[WinMaliShaderInit] AllocShader CALL nir=%p stage=%d",
             nir, nir ? nir->info.stage : -1);
   struct panfrost_uncompiled_shader *so = panfrost_alloc_shader(nir);
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] AllocShader EXIT nir=%p so=%p", nir, so);

   /* The driver gets ownership of the nir_shader for graphics. The NIR is
    * ralloc'd. Free the NIR when we free the uncompiled shader.
    */
   ralloc_steal(so, nir);

   so->stream_output = cso->stream_output;
   so->nir = nir;

   WM_SHADER_INIT_LOG("[WinMaliShaderInit] CreateState lowering ENTER so=%p nir=%p "
             "stage=%d pls=%u outputs=0x%llx separate=%u",
             so, nir, nir->info.stage,
             nir->info.stage == MESA_SHADER_FRAGMENT
                ? nir->info.fs.accesses_pixel_local_storage : 0,
             (unsigned long long)nir->info.outputs_written,
             nir->info.separate_shader);

   /* PLS lowering is not taken care of by glsl_to_nir(), so do it here. */
   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       nir->info.fs.accesses_pixel_local_storage) {
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] PLS lowering ENTER so=%p nir=%p", so,
                nir);
      /* Try to optimize the case where inout PLS vars are never
       * read/written to. Needs to be called before
       * nir_lower_io_vars_to_temporaries() because the copy_derefs
       * inserted there prevent us from detecting PLS usage.
       */
      NIR_PASS(_, nir, nir_downgrade_pls_vars);

      /* Lower PLS vars to temporaries before we lower IOs. */
      NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries,
               nir_shader_get_entrypoint(nir), nir_var_any_pixel_local);

      /* We need to lower all the copy_deref's introduced by lower_io_to-
       * _temporaries before calling nir_lower_io.
       */
      NIR_PASS(_, nir, nir_split_var_copies);
      NIR_PASS(_, nir, nir_lower_var_copies);
      NIR_PASS(_, nir, nir_lower_global_vars_to_local);

      /* Lower all PLS IOs. */
      NIR_PASS(_, nir, nir_lower_io, nir_var_any_pixel_local, glsl_type_size,
               0);

      /* Lower and remove dead derefs and variables to clean up the IR. */
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);

      /* Re-run gather_info() to get the latest accesses_pixel_local_storage
       * state.
       */
      nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] PLS lowering EXIT so=%p nir=%p", so,
                nir);
   }

   /* gl_FragColor needs to be lowered before lowering I/O, do that now */
   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       nir->info.outputs_written & BITFIELD_BIT(FRAG_RESULT_COLOR)) {

      WM_SHADER_INIT_LOG("[WinMaliShaderInit] FragColor lowering ENTER so=%p nir=%p "
                "dualSource=%u",
                so, nir, nir->info.fs.color_is_dual_source);
      NIR_PASS(_, nir, nir_lower_fragcolor,
               nir->info.fs.color_is_dual_source ? 1 : 8);
      so->fragcolor_lowered = true;
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] FragColor lowering EXIT so=%p nir=%p",
                so, nir);
   }

   /* Then run the suite of lowering and optimization, including I/O lowering */
   struct panfrost_device *dev = pan_device(pctx->screen);
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] PreprocessNIR ENTER so=%p nir=%p gpu=0x%x",
             so, nir, panfrost_device_gpu_id(dev));
   pan_preprocess_nir(nir, panfrost_device_gpu_id(dev));
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] PreprocessNIR EXIT so=%p nir=%p", so, nir);

   WM_SHADER_INIT_LOG("[WinMaliShaderInit] LowerIndirect ENTER so=%p nir=%p", so,
             nir);
   NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
            nir_var_shader_in | nir_var_shader_out, UINT32_MAX);
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] LowerIndirect EXIT so=%p nir=%p", so, nir);
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] LowerIO ENTER so=%p nir=%p", so, nir);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            glsl_type_size, nir_lower_io_use_interpolated_input_intrinsics);
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] LowerIO EXIT so=%p nir=%p", so, nir);
   /* nir_lower_io just computes offsets based on the original deref and
    * lower_indirect_derefs ensures that the array derefs have a constant
    * index.  Constant-fold to get us actual constants in in load/store
    * instructions.
    */
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] ConstantFold ENTER so=%p nir=%p", so, nir);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   WM_SHADER_INIT_LOG("[WinMaliShaderInit] ConstantFold EXIT so=%p nir=%p", so, nir);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] CollectFsVaryings ENTER so=%p nir=%p",
                so, nir);
      so->noperspective_varyings =
         pan_nir_collect_noperspective_varyings_fs(nir);
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] CollectFsVaryings EXIT so=%p nir=%p "
                "mask=0x%x",
                so, nir, so->noperspective_varyings);
   }

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      struct pan_varying_layout *varying_layout = &so->vs_varying_layout;
      pan_varying_collect_formats(varying_layout, nir,
                                  panfrost_device_gpu_id(dev),
                                  false, /* trust_varying_flat_highp_types */
                                  false /* lower_mediump */);
      pan_build_varying_layout_compact(varying_layout, nir,
                                       panfrost_device_gpu_id(dev));
   }

#if defined(WINMALI_SHADER_CAPTURE)
   if (nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_FRAGMENT) {
      winmali_capture_nir(nir, "driver-lowered", capture_initial,
                          capture_lowered);
      mesa_logi("[WinMaliCapture] Pipeline shaderStage=%s tgsi=%016llx "
                "initialNir=%s loweredNir=%s shader=%p",
                mesa_shader_stage_name(nir->info.stage),
                (unsigned long long)capture_tgsi_hash, capture_initial,
                capture_lowered, so);
      char directory[MAX_PATH], process[MAX_PATH], index[1024];
      if (winmali_capture_directory(directory, sizeof(directory))) {
         winmali_capture_process_name(process, sizeof(process));
         snprintf(index, sizeof(index),
                   "pipeline process=%s pid=%lu shaderStage=%s tgsi=%016llx "
                   "initialNir=%s loweredNir=%s shader=%p\r\n",
                   process, GetCurrentProcessId(),
                   mesa_shader_stage_name(nir->info.stage),
                   (unsigned long long)capture_tgsi_hash, capture_initial,
                  capture_lowered, so);
         winmali_capture_append_index(directory, process, index);
      }
   }
#endif

   /* If this shader uses transform feedback, compile the transform
    * feedback program. This is a special shader variant.
    */
   struct panfrost_context *ctx = pan_context(pctx);

   if (so->nir->xfb_info) {
      so->xfb = calloc(1, sizeof(struct panfrost_compiled_shader));
      so->xfb->key.vs.is_xfb = true;

      panfrost_shader_get(ctx->base.screen, &ctx->shaders, &ctx->descs, so,
                          &ctx->base.debug, so->xfb, 0);

      /* Since transform feedback is handled via the transform
       * feedback program, the original program no longer uses XFB
       */
      nir->info.has_transform_feedback_varyings = false;
   }

   /* If we're not using separate shaders, the FS can use VS varying_layout to
    * optimize loads (LD_VAR_BUF instead of LD_VAR).  Gallium won't provide us
    * with the VS directly, so we need to delay the default variant compilation
    * until link time
    */
   if (nir->info.stage == MESA_SHADER_FRAGMENT && !nir->info.separate_shader) {
      WM_SHADER_INIT_LOG("[WinMaliShaderInit] CreateState EXIT so=%p stage=%d "
                "result=deferred-link",
                so, nir->info.stage);
      return so;
   }

   /* Compile the program. We don't use vertex shader keys, so there will
    * be no further vertex shader variants. We do have fragment shader
    * keys, but we can still compile with a default key that will work most
    * of the time.
    */
   struct panfrost_shader_key key = panfrost_default_shader_key(so);

   /* Creating a CSO is single-threaded, so it's ok to use the
    * locked function without explicitly taking the lock. Creating a
    * default variant acts as a precompile.
    */
   panfrost_new_variant_locked(ctx, so, &key);

   WM_SHADER_INIT_LOG("[WinMaliShaderInit] CreateState EXIT so=%p stage=%d "
             "result=precompiled variants=%u",
             so, nir->info.stage,
             util_dynarray_num_elements(&so->variants,
                                        struct panfrost_compiled_shader));
   return so;
}

static void
panfrost_delete_shader_state(struct pipe_context *pctx, void *so)
{
   struct panfrost_uncompiled_shader *cso =
      (struct panfrost_uncompiled_shader *)so;

   util_dynarray_foreach(&cso->variants, struct panfrost_compiled_shader, so) {
      panfrost_bo_unreference(so->bin.bo);
      panfrost_bo_unreference(so->state.bo);
      panfrost_bo_unreference(so->linkage.bo);
   }

   if (cso->xfb) {
      panfrost_bo_unreference(cso->xfb->bin.bo);
      panfrost_bo_unreference(cso->xfb->state.bo);
      panfrost_bo_unreference(cso->xfb->linkage.bo);
      free(cso->xfb);
   }

   simple_mtx_destroy(&cso->lock);

   ralloc_free(so);
}

static void
panfrost_link_shader(struct pipe_context *pctx, void** handles)
{
   struct panfrost_context *ctx = pan_context(pctx);
   struct panfrost_uncompiled_shader *vs = handles[MESA_SHADER_VERTEX];
   struct panfrost_uncompiled_shader *fs = handles[MESA_SHADER_FRAGMENT];

   if (!fs || fs->nir->info.separate_shader)
      return;

   /* We only handle VS and FS for now, it's not clear how varying layout will
    * fit when more shader types are supported.  So assert those are the only
    * shaders present.
    */
   for (unsigned i = 0; i < MESA_SHADER_MESH_STAGES; i++) {
      if (i != MESA_SHADER_VERTEX && i != MESA_SHADER_FRAGMENT)
         assert(handles[i] == NULL);
   }

   /* Only copy the varying layout if we have a VS, sometimes we don't have one
    * (e.g. fixed-function VS), in those cases we just compile a default FS.
    */
   if (vs)
      fs->vs_varying_layout = vs->vs_varying_layout;

   simple_mtx_lock(&fs->lock);

   struct panfrost_shader_key key = panfrost_default_shader_key(fs);
   panfrost_new_variant_locked(ctx, fs, &key);

   simple_mtx_unlock(&fs->lock);
}

/*
 * Create a compute CSO. As compute kernels do not require variants, they are
 * precompiled, creating both the uncompiled and compiled shaders now.
 */
static void *
panfrost_create_compute_state(struct pipe_context *pctx,
                              const struct pipe_compute_state *cso)
{
   struct panfrost_context *ctx = pan_context(pctx);
   struct panfrost_uncompiled_shader *so = panfrost_alloc_shader(cso->prog);
   struct panfrost_compiled_shader *v = panfrost_alloc_variant(so);
   memset(v, 0, sizeof *v);

   assert(cso->ir_type == PIPE_SHADER_IR_NIR && "TGSI kernels unsupported");

   panfrost_shader_get(pctx->screen, &ctx->shaders, &ctx->descs, so,
                       &ctx->base.debug, v, cso->static_shared_mem);

   /* The NIR becomes invalid after this. For compute kernels, we never
    * need to access it again. Don't keep a dangling pointer around.
    */
   ralloc_free((void *)so->nir);
   so->nir = NULL;

   return so;
}

static void
panfrost_bind_compute_state(struct pipe_context *pipe, void *cso)
{
   struct panfrost_context *ctx = pan_context(pipe);
   struct panfrost_uncompiled_shader *uncompiled = cso;

   ctx->uncompiled[MESA_SHADER_COMPUTE] = uncompiled;

   ctx->prog[MESA_SHADER_COMPUTE] =
      uncompiled ? util_dynarray_begin(&uncompiled->variants) : NULL;
}

static void
panfrost_get_compute_state_info(struct pipe_context *pipe, void *cso,
                                struct pipe_compute_state_object_info *info)
{
   struct panfrost_device *dev = pan_device(pipe->screen);
   struct panfrost_uncompiled_shader *uncompiled = cso;
   struct panfrost_compiled_shader *cs =
      util_dynarray_begin(&uncompiled->variants);

   info->max_threads = pan_compute_max_thread_count(&dev->kmod.dev->props,
                                                    cs->info.work_reg_count);
   info->private_memory = cs->info.tls_size;
   info->simd_sizes = pan_subgroup_size(dev->arch);
   info->preferred_simd_size = info->simd_sizes;
}

void
panfrost_shader_context_init(struct pipe_context *pctx)
{
   pctx->create_vs_state = panfrost_create_shader_state;
   pctx->delete_vs_state = panfrost_delete_shader_state;
   pctx->bind_vs_state = panfrost_bind_vs_state;

   pctx->create_fs_state = panfrost_create_shader_state;
   pctx->delete_fs_state = panfrost_delete_shader_state;
   pctx->bind_fs_state = panfrost_bind_fs_state;

   pctx->link_shader = panfrost_link_shader;

   pctx->create_compute_state = panfrost_create_compute_state;
   pctx->bind_compute_state = panfrost_bind_compute_state;
   pctx->get_compute_state_info = panfrost_get_compute_state_info;
   pctx->delete_compute_state = panfrost_delete_shader_state;
}

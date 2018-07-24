/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_atomic.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "intel/compiler/brw_compiler.h"
#include "intel/compiler/brw_nir.h"
#include "iris_context.h"

static unsigned
get_new_program_id(struct iris_screen *screen)
{
   return p_atomic_inc_return(&screen->program_id);
}

struct iris_uncompiled_shader {
   struct pipe_shader_state base;
   unsigned program_id;

   /** Bitfield of (1 << IRIS_NOS_*) flags. */
   unsigned nos;
};

// XXX: need unify_interfaces() at link time...

static void *
iris_create_shader_state(struct pipe_context *ctx,
                         const struct pipe_shader_state *state)
{
   //struct iris_context *ice = (struct iris_context *)ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;

   assert(state->type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = state->ir.nir;

   struct iris_uncompiled_shader *ish =
      calloc(1, sizeof(struct iris_uncompiled_shader));
   if (!ish)
      return NULL;

   nir = brw_preprocess_nir(screen->compiler, nir);

   ish->program_id = get_new_program_id(screen);
   ish->base.type = PIPE_SHADER_IR_NIR;
   ish->base.ir.nir = nir;
   memcpy(&ish->base.stream_output, &state->stream_output,
          sizeof(struct pipe_stream_output_info));

   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      // XXX: NOS
      break;
   case MESA_SHADER_TESS_CTRL:
      // XXX: NOS
      break;
   case MESA_SHADER_TESS_EVAL:
      // XXX: NOS
      break;
   case MESA_SHADER_GEOMETRY:
      // XXX: NOS
      break;
   case MESA_SHADER_FRAGMENT:
      ish->nos |= IRIS_NOS_FRAMEBUFFER |
                  IRIS_NOS_DEPTH_STENCIL_ALPHA |
                  IRIS_NOS_RASTERIZER |
                  IRIS_NOS_BLEND;
      break;
   case MESA_SHADER_COMPUTE:
      // XXX: NOS
      break;
   default:
      break;
   }

   return ish;
}

static void
iris_delete_shader_state(struct pipe_context *ctx, void *state)
{
   struct iris_uncompiled_shader *ish = state;

   ralloc_free(ish->base.ir.nir);
   free(ish);
}

static void
bind_state(struct iris_context *ice,
           struct iris_uncompiled_shader *ish,
           gl_shader_stage stage)
{
   uint64_t dirty_bit = IRIS_DIRTY_UNCOMPILED_VS << stage;
   const uint64_t nos = ish ? ish->nos : 0;

   ice->shaders.uncompiled[stage] = ish;
   ice->state.dirty |= dirty_bit;

   for (int i = 0; i < IRIS_NOS_COUNT; i++) {
      if (nos & (1 << i))
         ice->state.dirty_for_nos[i] |= dirty_bit;
      else
         ice->state.dirty_for_nos[i] &= ~dirty_bit;
   }
}

static void
iris_bind_vs_state(struct pipe_context *ctx, void *state)
{
   bind_state((void *) ctx, state, MESA_SHADER_VERTEX);
}

static void
iris_bind_tcs_state(struct pipe_context *ctx, void *state)
{
   bind_state((void *) ctx, state, MESA_SHADER_TESS_CTRL);
}

static void
iris_bind_tes_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (!!state != !!ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL])
      ice->state.dirty |= IRIS_DIRTY_URB;

   bind_state((void *) ctx, state, MESA_SHADER_TESS_EVAL);
}

static void
iris_bind_gs_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (!!state != !!ice->shaders.uncompiled[MESA_SHADER_GEOMETRY])
      ice->state.dirty |= IRIS_DIRTY_URB;

   bind_state((void *) ctx, state, MESA_SHADER_GEOMETRY);
}

static void
iris_bind_fs_state(struct pipe_context *ctx, void *state)
{
   bind_state((void *) ctx, state, MESA_SHADER_FRAGMENT);
}

/**
 * Sets up the starting offsets for the groups of binding table entries
 * common to all pipeline stages.
 *
 * Unused groups are initialized to 0xd0d0d0d0 to make it obvious that they're
 * unused but also make sure that addition of small offsets to them will
 * trigger some of our asserts that surface indices are < BRW_MAX_SURFACES.
 */
static uint32_t
assign_common_binding_table_offsets(const struct gen_device_info *devinfo,
                                    const struct nir_shader *nir,
                                    struct brw_stage_prog_data *prog_data,
                                    uint32_t next_binding_table_offset)
{
   const struct shader_info *info = &nir->info;

   if (info->num_textures) {
      prog_data->binding_table.texture_start = next_binding_table_offset;
      prog_data->binding_table.gather_texture_start = next_binding_table_offset;
      next_binding_table_offset += info->num_textures;
   } else {
      prog_data->binding_table.texture_start = 0xd0d0d0d0;
      prog_data->binding_table.gather_texture_start = 0xd0d0d0d0;
   }

   int num_ubos = info->num_ubos + (nir->num_uniforms > 0 ? 1 : 0);

   if (num_ubos) {
      //assert(info->num_ubos <= BRW_MAX_UBO);
      prog_data->binding_table.ubo_start = next_binding_table_offset;
      next_binding_table_offset += num_ubos;
   } else {
      prog_data->binding_table.ubo_start = 0xd0d0d0d0;
   }

   if (info->num_ssbos || info->num_abos) {
      //assert(info->num_abos <= BRW_MAX_ABO);
      //assert(info->num_ssbos <= BRW_MAX_SSBO);
      prog_data->binding_table.ssbo_start = next_binding_table_offset;
      next_binding_table_offset += info->num_abos + info->num_ssbos;
   } else {
      prog_data->binding_table.ssbo_start = 0xd0d0d0d0;
   }

   prog_data->binding_table.shader_time_start = 0xd0d0d0d0;

   if (info->num_images) {
      prog_data->binding_table.image_start = next_binding_table_offset;
      next_binding_table_offset += info->num_images;
   } else {
      prog_data->binding_table.image_start = 0xd0d0d0d0;
   }

   /* This may or may not be used depending on how the compile goes. */
   prog_data->binding_table.pull_constants_start = next_binding_table_offset;
   next_binding_table_offset++;

   /* Plane 0 is just the regular texture section */
   prog_data->binding_table.plane_start[0] = prog_data->binding_table.texture_start;

   prog_data->binding_table.plane_start[1] = next_binding_table_offset;
   next_binding_table_offset += info->num_textures;

   prog_data->binding_table.plane_start[2] = next_binding_table_offset;
   next_binding_table_offset += info->num_textures;

   /* prog_data->base.binding_table.size will be set by brw_mark_surface_used. */

   //assert(next_binding_table_offset <= BRW_MAX_SURFACES);
   return next_binding_table_offset;
}

static void
iris_setup_uniforms(const struct brw_compiler *compiler,
                    void *mem_ctx,
                    nir_shader *nir,
                    struct brw_stage_prog_data *prog_data)
{
   prog_data->nr_params = nir->num_uniforms;
   prog_data->param = rzalloc_array(mem_ctx, uint32_t, prog_data->nr_params);

   nir_foreach_variable(var, &nir->uniforms) {
      const unsigned components = glsl_get_components(var->type);

      for (unsigned i = 0; i < components; i++) {
         prog_data->param[var->data.driver_location] =
            var->data.driver_location;
      }
   }

   // XXX: vs clip planes?
   brw_nir_analyze_ubo_ranges(compiler, nir, NULL, prog_data->ubo_ranges);
}

static void
iris_setup_push_uniform_range(const struct brw_compiler *compiler,
                              struct brw_stage_prog_data *prog_data)
{
   if (prog_data->nr_params) {
      for (int i = 3; i > 0; i--)
         prog_data->ubo_ranges[i] = prog_data->ubo_ranges[i - 1];

      prog_data->ubo_ranges[0] = (struct brw_ubo_range) {
         .block = 0,
         .start = 0,
         .length = DIV_ROUND_UP(prog_data->nr_params, 8),
      };
   }
}

static bool
iris_compile_vs(struct iris_context *ice,
                struct iris_uncompiled_shader *ish,
                const struct brw_vs_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_vs_prog_data *vs_prog_data =
      rzalloc(mem_ctx, struct brw_vs_prog_data);
   struct brw_vue_prog_data *vue_prog_data = &vs_prog_data->base;
   struct brw_stage_prog_data *prog_data = &vue_prog_data->base;

   assert(ish->base.type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = ish->base.ir.nir;

   // XXX: alt mode
   assign_common_binding_table_offsets(devinfo, nir, prog_data, 0);

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data);

   brw_compute_vue_map(devinfo,
                       &vue_prog_data->vue_map, nir->info.outputs_written,
                       nir->info.separate_shader);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_vs(compiler, &ice->dbg, mem_ctx, key, vs_prog_data,
                     nir, -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile vertex shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   iris_setup_push_uniform_range(compiler, prog_data);

   uint32_t *so_decls =
      ice->vtbl.create_so_decl_list(&ish->base.stream_output,
                                    &vue_prog_data->vue_map);

   iris_upload_and_bind_shader(ice, IRIS_CACHE_VS, key, program, prog_data,
                               so_decls);

   ralloc_free(mem_ctx);
   return true;
}

static void
iris_update_compiled_vs(struct iris_context *ice)
{
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_VERTEX];

   struct brw_vs_prog_key key = { .program_string_id = ish->program_id };
   ice->vtbl.populate_vs_key(ice, &key);

   if (iris_bind_cached_shader(ice, IRIS_CACHE_VS, &key))
      return;

   UNUSED bool success = iris_compile_vs(ice, ish, &key);
}

const struct shader_info *
iris_get_shader_info(const struct iris_context *ice, gl_shader_stage stage)
{
   const struct iris_uncompiled_shader *ish = ice->shaders.uncompiled[stage];

   if (!ish)
      return NULL;

   const nir_shader *nir = ish->base.ir.nir;
   return &nir->info;
}

/**
 * Get the union of TCS output and TES input slots.
 *
 * TCS and TES need to agree on a common URB entry layout.  In particular,
 * the data for all patch vertices is stored in a single URB entry (unlike
 * GS which has one entry per input vertex).  This means that per-vertex
 * array indexing needs a stride.
 *
 * SSO requires locations to match, but doesn't require the number of
 * outputs/inputs to match (in fact, the TCS often has extra outputs).
 * So, we need to take the extra step of unifying these on the fly.
 */
static void
get_unified_tess_slots(const struct iris_context *ice,
                       uint64_t *per_vertex_slots,
                       uint32_t *per_patch_slots)
{
   const struct shader_info *tcs =
      iris_get_shader_info(ice, MESA_SHADER_TESS_CTRL);
   const struct shader_info *tes =
      iris_get_shader_info(ice, MESA_SHADER_TESS_EVAL);

   *per_vertex_slots = tes->inputs_read;
   *per_patch_slots = tes->patch_inputs_read;

   if (tcs) {
      *per_vertex_slots |= tcs->inputs_read;
      *per_patch_slots |= tcs->patch_inputs_read;
   }
}

static bool
iris_compile_tcs(struct iris_context *ice,
                 struct iris_uncompiled_shader *ish,
                 const struct brw_tcs_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_tcs_prog_data *tcs_prog_data =
      rzalloc(mem_ctx, struct brw_tcs_prog_data);
   struct brw_vue_prog_data *vue_prog_data = &tcs_prog_data->base;
   struct brw_stage_prog_data *prog_data = &vue_prog_data->base;

   assert(ish->base.type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = ish->base.ir.nir;

   assign_common_binding_table_offsets(devinfo, nir, prog_data, 0);

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_tcs(compiler, &ice->dbg, mem_ctx, key, tcs_prog_data, nir,
                      -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile evaluation shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   iris_setup_push_uniform_range(compiler, prog_data);

   iris_upload_and_bind_shader(ice, IRIS_CACHE_TCS, key, program, prog_data,
                               NULL);

   ralloc_free(mem_ctx);
   return true;
}

static void
iris_update_compiled_tcs(struct iris_context *ice)
{
   struct iris_uncompiled_shader *tcs =
      ice->shaders.uncompiled[MESA_SHADER_TESS_CTRL];
   struct iris_uncompiled_shader *tes =
      ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL];

   assert(!(tes && !tcs));

   if (!tcs) {
      iris_unbind_shader(ice, IRIS_CACHE_TCS);
      return;
   }

   const struct shader_info *tes_info =
      iris_get_shader_info(ice, MESA_SHADER_TESS_EVAL);
   struct brw_tcs_prog_key key = {
      .program_string_id = tcs->program_id,
      .tes_primitive_mode = tes_info->tess.primitive_mode,
   };
   get_unified_tess_slots(ice, &key.outputs_written,
                          &key.patch_outputs_written);
   ice->vtbl.populate_tcs_key(ice, &key);

   if (iris_bind_cached_shader(ice, IRIS_CACHE_TCS, &key))
      return;

   UNUSED bool success = iris_compile_tcs(ice, tcs, &key);
}

static bool
iris_compile_tes(struct iris_context *ice,
                 struct iris_uncompiled_shader *ish,
                 const struct brw_tes_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_tes_prog_data *tes_prog_data =
      rzalloc(mem_ctx, struct brw_tes_prog_data);
   struct brw_vue_prog_data *vue_prog_data = &tes_prog_data->base;
   struct brw_stage_prog_data *prog_data = &vue_prog_data->base;

   assert(ish->base.type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = ish->base.ir.nir;

   assign_common_binding_table_offsets(devinfo, nir, prog_data, 0);

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data);

   struct brw_vue_map input_vue_map;
   brw_compute_tess_vue_map(&input_vue_map, key->inputs_read,
                            key->patch_inputs_read);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_tes(compiler, &ice->dbg, mem_ctx, key, &input_vue_map,
                      tes_prog_data, nir, NULL, -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile evaluation shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   iris_setup_push_uniform_range(compiler, prog_data);

   uint32_t *so_decls =
      ice->vtbl.create_so_decl_list(&ish->base.stream_output,
                                    &vue_prog_data->vue_map);

   iris_upload_and_bind_shader(ice, IRIS_CACHE_TES, key, program, prog_data,
                               so_decls);

   ralloc_free(mem_ctx);
   return true;
}

static void
iris_update_compiled_tes(struct iris_context *ice)
{
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_TESS_EVAL];

   if (!ish) {
      iris_unbind_shader(ice, IRIS_CACHE_TES);
      return;
   }

   struct brw_tes_prog_key key = { .program_string_id = ish->program_id };
   get_unified_tess_slots(ice, &key.inputs_read, &key.patch_inputs_read);
   ice->vtbl.populate_tes_key(ice, &key);

   if (iris_bind_cached_shader(ice, IRIS_CACHE_TES, &key))
      return;

   UNUSED bool success = iris_compile_tes(ice, ish, &key);
}

static bool
iris_compile_gs(struct iris_context *ice,
                struct iris_uncompiled_shader *ish,
                const struct brw_gs_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_gs_prog_data *gs_prog_data =
      rzalloc(mem_ctx, struct brw_gs_prog_data);
   struct brw_vue_prog_data *vue_prog_data = &gs_prog_data->base;
   struct brw_stage_prog_data *prog_data = &vue_prog_data->base;

   assert(ish->base.type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = ish->base.ir.nir;

   assign_common_binding_table_offsets(devinfo, nir, prog_data, 0);

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data);

   brw_compute_vue_map(devinfo,
                       &vue_prog_data->vue_map, nir->info.outputs_written,
                       nir->info.separate_shader);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_gs(compiler, &ice->dbg, mem_ctx, key, gs_prog_data, nir,
                     NULL, -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile geometry shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   iris_setup_push_uniform_range(compiler, prog_data);

   uint32_t *so_decls =
      ice->vtbl.create_so_decl_list(&ish->base.stream_output,
                                    &vue_prog_data->vue_map);

   iris_upload_and_bind_shader(ice, IRIS_CACHE_GS, key, program, prog_data,
                               so_decls);

   ralloc_free(mem_ctx);
   return true;
}


static void
iris_update_compiled_gs(struct iris_context *ice)
{
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_GEOMETRY];

   if (!ish) {
      iris_unbind_shader(ice, IRIS_CACHE_GS);
      return;
   }

   struct brw_gs_prog_key key = { .program_string_id = ish->program_id };
   ice->vtbl.populate_gs_key(ice, &key);

   if (iris_bind_cached_shader(ice, IRIS_CACHE_GS, &key))
      return;

   UNUSED bool success = iris_compile_gs(ice, ish, &key);
}

static bool
iris_compile_fs(struct iris_context *ice,
                struct iris_uncompiled_shader *ish,
                const struct brw_wm_prog_key *key,
                struct brw_vue_map *vue_map)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;
   void *mem_ctx = ralloc_context(NULL);
   struct brw_wm_prog_data *fs_prog_data =
      rzalloc(mem_ctx, struct brw_wm_prog_data);
   struct brw_stage_prog_data *prog_data = &fs_prog_data->base;

   assert(ish->base.type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = ish->base.ir.nir;

   // XXX: alt mode
   assign_common_binding_table_offsets(devinfo, nir, prog_data,
                                       MAX2(key->nr_color_regions, 1));

   iris_setup_uniforms(compiler, mem_ctx, nir, prog_data);

   char *error_str = NULL;
   const unsigned *program =
      brw_compile_fs(compiler, &ice->dbg, mem_ctx, key, fs_prog_data,
                     nir, NULL, -1, -1, -1, true, false, vue_map, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile fragment shader: %s\n", error_str);
      ralloc_free(mem_ctx);
      return false;
   }

   //brw_alloc_stage_scratch(brw, &brw->wm.base, prog_data.base.total_scratch);

   iris_setup_push_uniform_range(compiler, prog_data);

   iris_upload_and_bind_shader(ice, IRIS_CACHE_FS, key, program, prog_data,
                               NULL);

   ralloc_free(mem_ctx);
   return true;
}

static void
iris_update_compiled_fs(struct iris_context *ice)
{
   struct iris_uncompiled_shader *ish =
      ice->shaders.uncompiled[MESA_SHADER_FRAGMENT];
   struct brw_wm_prog_key key = { .program_string_id = ish->program_id };
   ice->vtbl.populate_fs_key(ice, &key);

   if (iris_bind_cached_shader(ice, IRIS_CACHE_FS, &key))
      return;

   UNUSED bool success =
      iris_compile_fs(ice, ish, &key, ice->shaders.last_vue_map);
}

static struct iris_compiled_shader *
last_vue_shader(struct iris_context *ice)
{
   if (ice->shaders.prog[MESA_SHADER_GEOMETRY])
      return ice->shaders.prog[MESA_SHADER_GEOMETRY];

   if (ice->shaders.prog[MESA_SHADER_TESS_EVAL])
      return ice->shaders.prog[MESA_SHADER_TESS_EVAL];

   return ice->shaders.prog[MESA_SHADER_VERTEX];
}

static void
update_last_vue_map(struct iris_context *ice,
                    struct brw_stage_prog_data *prog_data)
{
   struct brw_vue_prog_data *vue_prog_data = (void *) prog_data;
   struct brw_vue_map *vue_map = &vue_prog_data->vue_map;
   struct brw_vue_map *old_map = ice->shaders.last_vue_map;
   const uint64_t changed_slots =
      (old_map ? old_map->slots_valid : 0ull) ^ vue_map->slots_valid;

   if (changed_slots & VARYING_BIT_VIEWPORT) {
      // XXX: could use ctx->Const.MaxViewports for old API efficiency
      ice->state.num_viewports =
         (vue_map->slots_valid & VARYING_BIT_VIEWPORT) ? IRIS_MAX_VIEWPORTS : 1;
      ice->state.dirty |= IRIS_DIRTY_CLIP |
                          IRIS_DIRTY_SF_CL_VIEWPORT |
                          IRIS_DIRTY_SCISSOR_RECT |
                          IRIS_DIRTY_UNCOMPILED_FS;
      // XXX: CC_VIEWPORT?
   }

   if (changed_slots || (old_map && old_map->separate != vue_map->separate)) {
      ice->state.dirty |= IRIS_DIRTY_SBE;
   }

   ice->shaders.last_vue_map = &vue_prog_data->vue_map;
}

static struct brw_vue_prog_data *
get_vue_prog_data(struct iris_context *ice, gl_shader_stage stage)
{
   if (!ice->shaders.prog[stage])
      return NULL;

   return (void *) ice->shaders.prog[stage]->prog_data;
}

void
iris_update_compiled_shaders(struct iris_context *ice)
{
   const uint64_t dirty = ice->state.dirty;

   struct brw_vue_prog_data *old_prog_datas[4];
   if (!(dirty & IRIS_DIRTY_URB)) {
      for (int i = MESA_SHADER_VERTEX; i <= MESA_SHADER_GEOMETRY; i++)
         old_prog_datas[i] = get_vue_prog_data(ice, i);
   }

   if (dirty & IRIS_DIRTY_UNCOMPILED_VS)
      iris_update_compiled_vs(ice);
   if (dirty & IRIS_DIRTY_UNCOMPILED_TCS)
      iris_update_compiled_tcs(ice);
   if (dirty & IRIS_DIRTY_UNCOMPILED_TES)
      iris_update_compiled_tes(ice);
   if (dirty & IRIS_DIRTY_UNCOMPILED_GS)
      iris_update_compiled_gs(ice);

   struct iris_compiled_shader *shader = last_vue_shader(ice);
   update_last_vue_map(ice, shader->prog_data);
   if (ice->state.streamout != shader->streamout) {
      ice->state.streamout = shader->streamout;
      ice->state.dirty |= IRIS_DIRTY_SO_DECL_LIST | IRIS_DIRTY_STREAMOUT;
   }

   if (dirty & IRIS_DIRTY_UNCOMPILED_FS)
      iris_update_compiled_fs(ice);
   // ...

   if (!(dirty & IRIS_DIRTY_URB)) {
      for (int i = MESA_SHADER_VERTEX; i <= MESA_SHADER_GEOMETRY; i++) {
         struct brw_vue_prog_data *old = old_prog_datas[i];
         struct brw_vue_prog_data *new = get_vue_prog_data(ice, i);
         if (!!old != !!new ||
             (new && new->urb_entry_size != old->urb_entry_size)) {
            ice->state.dirty |= IRIS_DIRTY_URB;
            break;
         }
      }
   }
}

void
iris_init_program_functions(struct pipe_context *ctx)
{
   ctx->create_vs_state  = iris_create_shader_state;
   ctx->create_tcs_state = iris_create_shader_state;
   ctx->create_tes_state = iris_create_shader_state;
   ctx->create_gs_state  = iris_create_shader_state;
   ctx->create_fs_state  = iris_create_shader_state;

   ctx->delete_vs_state  = iris_delete_shader_state;
   ctx->delete_tcs_state = iris_delete_shader_state;
   ctx->delete_tes_state = iris_delete_shader_state;
   ctx->delete_gs_state  = iris_delete_shader_state;
   ctx->delete_fs_state  = iris_delete_shader_state;

   ctx->bind_vs_state  = iris_bind_vs_state;
   ctx->bind_tcs_state = iris_bind_tcs_state;
   ctx->bind_tes_state = iris_bind_tes_state;
   ctx->bind_gs_state  = iris_bind_gs_state;
   ctx->bind_fs_state  = iris_bind_fs_state;
}

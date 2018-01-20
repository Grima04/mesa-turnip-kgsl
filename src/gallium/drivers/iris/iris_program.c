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
   //NIR_PASS_V(nir, brw_nir_lower_uniforms, true);

   ish->program_id = get_new_program_id(screen);
   ish->base.type = PIPE_SHADER_IR_NIR;
   ish->base.ir.nir = nir;

   return ish;
}

static void
iris_delete_shader_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_uncompiled_shader *ish = hwcso;

   ralloc_free(ish->base.ir.nir);
   free(ish);
}

static void
iris_bind_vs_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->shaders.progs[MESA_SHADER_VERTEX] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_VS;
}

static void
iris_bind_tcs_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->shaders.progs[MESA_SHADER_TESS_CTRL] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_TCS;
}

static void
iris_bind_tes_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->shaders.progs[MESA_SHADER_TESS_EVAL] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_TES;
}

static void
iris_bind_gs_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->shaders.progs[MESA_SHADER_GEOMETRY] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_GS;
}

static void
iris_bind_fs_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->shaders.progs[MESA_SHADER_FRAGMENT] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_FS;
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
                                    const struct shader_info *info,
                                    struct brw_stage_prog_data *prog_data,
                                    uint32_t next_binding_table_offset)
{
   prog_data->binding_table.texture_start = next_binding_table_offset;
   prog_data->binding_table.gather_texture_start = next_binding_table_offset;
   next_binding_table_offset += info->num_textures;

   if (info->num_ubos) {
      //assert(info->num_ubos <= BRW_MAX_UBO);
      prog_data->binding_table.ubo_start = next_binding_table_offset;
      next_binding_table_offset += info->num_ubos;
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

static bool
iris_compile_vs(struct iris_context *ice,
                struct iris_uncompiled_shader *ish,
                const struct brw_vs_prog_key *key)
{
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct brw_compiler *compiler = screen->compiler;
   const struct gen_device_info *devinfo = &screen->devinfo;
   const unsigned *program;
   struct brw_vs_prog_data vs_prog_data;
   struct brw_stage_prog_data *prog_data = &vs_prog_data.base.base;
   void *mem_ctx = ralloc_context(NULL);

   assert(ish->base.type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = ish->base.ir.nir;

   memset(&vs_prog_data, 0, sizeof(vs_prog_data));

   // XXX: alt mode
   assign_common_binding_table_offsets(devinfo, &nir->info, prog_data, 0);
   brw_compute_vue_map(devinfo,
                       &vs_prog_data.base.vue_map, nir->info.outputs_written,
                       nir->info.separate_shader);

   char *error_str = NULL;
   program = brw_compile_vs(compiler, &ice->dbg, mem_ctx, key, &vs_prog_data,
                            nir, -1, &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile vertex shader: %s\n", error_str);

      ralloc_free(mem_ctx);
      return false;
   }

   /* The param and pull_param arrays will be freed by the shader cache. */
   ralloc_steal(NULL, prog_data->param);
   ralloc_steal(NULL, prog_data->pull_param);
   iris_upload_cache(ice, IRIS_CACHE_VS, key, sizeof(*key), program,
                     prog_data->program_size, prog_data, sizeof(vs_prog_data),
                     &ice->shaders.prog_offset[MESA_SHADER_VERTEX],
                     &ice->shaders.prog_data[MESA_SHADER_VERTEX]);
   ralloc_free(mem_ctx);

   return true;
}

static void
iris_populate_vs_key(struct iris_context *ice, struct brw_vs_prog_key *key)
{
   memset(key, 0, sizeof(*key));
}

static void
iris_update_compiled_vs(struct iris_context *ice)
{
   struct brw_vs_prog_key key;
   iris_populate_vs_key(ice, &key);

   if (iris_search_cache(ice, IRIS_CACHE_VS, &key, sizeof(key), IRIS_DIRTY_VS,
                         &ice->shaders.prog_offset[MESA_SHADER_VERTEX],
                         &ice->shaders.prog_data[MESA_SHADER_VERTEX]))
      return;

   UNUSED bool success =
      iris_compile_vs(ice, ice->shaders.progs[MESA_SHADER_VERTEX], &key);
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
   const unsigned *program;
   struct brw_wm_prog_data fs_prog_data;
   struct brw_stage_prog_data *prog_data = &fs_prog_data.base;
   void *mem_ctx = ralloc_context(NULL);

   assert(ish->base.type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = ish->base.ir.nir;

   memset(&fs_prog_data, 0, sizeof(fs_prog_data));

   // XXX: alt mode
   assign_common_binding_table_offsets(devinfo, &nir->info, prog_data,
                                       MAX2(key->nr_color_regions, 1));

   char *error_str = NULL;
   program = brw_compile_fs(compiler, &ice->dbg, mem_ctx, key, &fs_prog_data,
                            nir, NULL, -1, -1, -1, true, false, vue_map,
                            &error_str);
   if (program == NULL) {
      dbg_printf("Failed to compile fragment shader: %s\n", error_str);

      ralloc_free(mem_ctx);
      return false;
   }

   //brw_alloc_stage_scratch(brw, &brw->wm.base, prog_data.base.total_scratch);

   /* The param and pull_param arrays will be freed by the shader cache. */
   ralloc_steal(NULL, prog_data->param);
   ralloc_steal(NULL, prog_data->pull_param);
   #if 0
   brw_upload_cache(&brw->cache, BRW_CACHE_FS_PROG,
                    key, sizeof(struct brw_wm_prog_key),
                    program, prog_data.base.program_size,
                    &prog_data, sizeof(prog_data),
                    &brw->wm.base.prog_offset, &brw->wm.base.prog_data);
   #endif

   ralloc_free(mem_ctx);

   return true;
}

static void
iris_populate_fs_key(struct iris_context *ice, struct brw_wm_prog_key *key)
{
   memset(key, 0, sizeof(*key));

   /* XXX: dirty flags? */
   struct pipe_framebuffer_state *fb = &ice->state.framebuffer;
   //struct iris_depth_stencil_alpha_state *zsa = ice->state.framebuffer;
   // XXX: can't access iris structs outside iris_state.c :(
   // XXX: maybe just move these to iris_state.c, honestly...they're more
   // about state than programs...

   key->nr_color_regions = fb->nr_cbufs;

   // key->force_dual_color_blend for unigine
#if 0
   //key->replicate_alpha = fb->nr_cbufs > 1 && alpha test or alpha to coverage
   if (cso_rast->multisample) {
      key->persample_interp =
         ctx->Multisample.SampleShading &&
         (ctx->Multisample.MinSampleShadingValue *
          _mesa_geometric_samples(ctx->DrawBuffer) > 1);

      key->multisample_fbo = fb->samples > 1;
   }
#endif

   key->coherent_fb_fetch = true;
}

static void
iris_update_compiled_fs(struct iris_context *ice)
{
   struct brw_wm_prog_key key;
   iris_populate_fs_key(ice, &key);

   return; // XXX: need to fix FS compiles

   UNUSED bool success =
      iris_compile_fs(ice, ice->shaders.progs[MESA_SHADER_FRAGMENT], &key,
                      ice->shaders.last_vue_map);
}

static void
update_last_vue_map(struct iris_context *ice)
{
#if 0
   struct brw_vue_prog_data *vue_prog_data;

   if (ice->shaders.progs[MESA_SHADER_GEOMETRY])
      vue_prog_data = brw_vue_prog_data(brw->gs.base.prog_data);
   else if (ice->shaders.progs[MESA_SHADER_TESS_EVAL])
      vue_prog_data = brw_vue_prog_data(brw->tes.base.prog_data);
   else
      vue_prog_data = brw_vue_prog_data(brw->vs.base.prog_data);

   brw->vue_map_geom_out = vue_prog_data->vue_map;
#endif
}

void
iris_update_compiled_shaders(struct iris_context *ice)
{
   iris_update_compiled_vs(ice);
   update_last_vue_map(ice);
   iris_update_compiled_fs(ice);
   // ...
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

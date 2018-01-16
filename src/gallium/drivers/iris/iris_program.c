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

   ice->progs[MESA_SHADER_VERTEX] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_VS;
}

static void
iris_bind_tcs_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->progs[MESA_SHADER_TESS_CTRL] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_TCS;
}

static void
iris_bind_tes_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->progs[MESA_SHADER_TESS_EVAL] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_TES;
}

static void
iris_bind_gs_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->progs[MESA_SHADER_GEOMETRY] = hwcso;
   ice->state.dirty |= IRIS_DIRTY_UNCOMPILED_GS;
}

static void
iris_bind_fs_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   ice->progs[MESA_SHADER_FRAGMENT] = hwcso;
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
   struct brw_vs_prog_data prog_data;
   struct brw_stage_prog_data *stage_prog_data = &prog_data.base.base;
   void *mem_ctx = ralloc_context(NULL);

   assert(ish->base.type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = ish->base.ir.nir;

   memset(&prog_data, 0, sizeof(prog_data));

   // XXX: alt mode
   assign_common_binding_table_offsets(devinfo, &nir->info,
                                       &prog_data.base.base, 0);
   brw_compute_vue_map(devinfo,
                       &prog_data.base.vue_map, nir->info.outputs_written,
                       nir->info.separate_shader);

   char *error_str;
   program = brw_compile_vs(compiler, ice, mem_ctx, key, &prog_data,
                            nir, -1, &error_str);
   if (program == NULL) {
      fprintf(stderr, "Failed to compile vertex shader: %s\n", error_str);

      ralloc_free(mem_ctx);
      return false;
   }

   /* The param and pull_param arrays will be freed by the shader cache. */
   ralloc_steal(NULL, prog_data.base.base.param);
   ralloc_steal(NULL, prog_data.base.base.pull_param);
   //brw_upload_cache(&brw->cache, BRW_CACHE_VS_PROG,
                    //key, sizeof(struct brw_vs_prog_key),
                    //program, prog_data.base.base.program_size,
                    //&prog_data, sizeof(prog_data),
                    //&brw->vs.base.prog_offset, &brw->vs.base.prog_data);
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

   UNUSED bool success =
      iris_compile_vs(ice, ice->progs[MESA_SHADER_VERTEX], &key);
}

void
iris_update_compiled_shaders(struct iris_context *ice)
{
   iris_update_compiled_vs(ice);
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

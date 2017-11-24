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

static void *
iris_create_shader_state(struct pipe_context *ctx,
                         const struct pipe_shader_state *state)
{
   struct iris_context *ice = (struct iris_context *)ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;

   assert(state->type == PIPE_SHADER_IR_NIR);

   nir_shader *nir = state->ir.nir;

   struct iris_uncompiled_shader *cso =
      calloc(1, sizeof(struct iris_uncompiled_shader));
   if (!cso)
      return NULL;

   nir = brw_preprocess_nir(screen->compiler, nir);

   cso->program_id = get_new_program_id(screen);
   cso->base.type = PIPE_SHADER_IR_NIR;
   cso->base.ir.nir = nir;

   return cso;
}

static void
iris_delete_shader_state(struct pipe_context *ctx, void *hwcso)
{
   struct iris_uncompiled_shader *cso = hwcso;

   ralloc_free(cso->base.ir.nir);
   free(cso);
}

void
iris_init_program_functions(struct pipe_context *ctx)
{
   ctx->create_vs_state = iris_create_shader_state;
   ctx->create_tcs_state = iris_create_shader_state;
   ctx->create_tes_state = iris_create_shader_state;
   ctx->create_gs_state = iris_create_shader_state;
   ctx->create_fs_state = iris_create_shader_state;

   ctx->delete_vs_state = iris_delete_shader_state;
   ctx->delete_tcs_state = iris_delete_shader_state;
   ctx->delete_tes_state = iris_delete_shader_state;
   ctx->delete_gs_state = iris_delete_shader_state;
   ctx->delete_fs_state = iris_delete_shader_state;
}

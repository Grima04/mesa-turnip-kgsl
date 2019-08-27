/**************************************************************************
 *
 * Copyright 2019 Red Hat.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **************************************************************************/
#include "util/u_memory.h"
#include "util/simple_list.h"

#include "tgsi/tgsi_parse.h"
#include "gallivm/lp_bld_debug.h"
#include "lp_state_cs.h"
#include "lp_context.h"
#include "lp_debug.h"
#include "lp_state.h"

static void *
llvmpipe_create_compute_state(struct pipe_context *pipe,
                                     const struct pipe_compute_state *templ)
{
   struct lp_compute_shader *shader;

   shader = CALLOC_STRUCT(lp_compute_shader);
   if (!shader)
      return NULL;

   assert(templ->ir_type == PIPE_SHADER_IR_TGSI);
   shader->base.tokens = tgsi_dup_tokens(templ->prog);

   lp_build_tgsi_info(shader->base.tokens, &shader->info);
   make_empty_list(&shader->variants);

   return shader;
}

static void
llvmpipe_bind_compute_state(struct pipe_context *pipe,
                            void *cs)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);

   if (llvmpipe->cs == cs)
      return;

   llvmpipe->cs = (struct lp_compute_shader *)cs;
   llvmpipe->cs_dirty |= LP_CSNEW_CS;
}

/**
 * Remove shader variant from two lists: the shader's variant list
 * and the context's variant list.
 */
static void
llvmpipe_remove_cs_shader_variant(struct llvmpipe_context *lp,
                                  struct lp_compute_shader_variant *variant)
{
   if ((LP_DEBUG & DEBUG_CS) || (gallivm_debug & GALLIVM_DEBUG_IR)) {
      debug_printf("llvmpipe: del cs #%u var %u v created %u v cached %u "
                   "v total cached %u inst %u total inst %u\n",
                   variant->shader->no, variant->no,
                   variant->shader->variants_created,
                   variant->shader->variants_cached,
                   lp->nr_cs_variants, variant->nr_instrs, lp->nr_cs_instrs);
   }

   gallivm_destroy(variant->gallivm);

   /* remove from shader's list */
   remove_from_list(&variant->list_item_local);
   variant->shader->variants_cached--;

   /* remove from context's list */
   remove_from_list(&variant->list_item_global);
   lp->nr_fs_variants--;
   lp->nr_fs_instrs -= variant->nr_instrs;

   FREE(variant);
}

static void
llvmpipe_delete_compute_state(struct pipe_context *pipe,
                              void *cs)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_compute_shader *shader = cs;
   struct lp_cs_variant_list_item *li;

   /* Delete all the variants */
   li = first_elem(&shader->variants);
   while(!at_end(&shader->variants, li)) {
      struct lp_cs_variant_list_item *next = next_elem(li);
      llvmpipe_remove_cs_shader_variant(llvmpipe, li->base);
      li = next;
   }
   tgsi_free_tokens(shader->base.tokens);
   FREE(shader);
}

void
llvmpipe_init_compute_funcs(struct llvmpipe_context *llvmpipe)
{
   llvmpipe->pipe.create_compute_state = llvmpipe_create_compute_state;
   llvmpipe->pipe.bind_compute_state = llvmpipe_bind_compute_state;
   llvmpipe->pipe.delete_compute_state = llvmpipe_delete_compute_state;
}

void
lp_csctx_destroy(struct lp_cs_context *csctx)
{
   FREE(csctx);
}

struct lp_cs_context *lp_csctx_create(struct pipe_context *pipe)
{
   struct lp_cs_context *csctx;

   csctx = CALLOC_STRUCT(lp_cs_context);
   if (!csctx)
      return NULL;

   csctx->pipe = pipe;
   return csctx;
}

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
#include "util/os_time.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_parse.h"
#include "gallivm/lp_bld_debug.h"
#include "lp_state_cs.h"
#include "lp_context.h"
#include "lp_debug.h"
#include "lp_state.h"
#include "lp_perf.h"

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

static void
make_variant_key(struct llvmpipe_context *lp,
                 struct lp_compute_shader *shader,
                 struct lp_compute_shader_variant_key *key)
{
   memset(key, 0, shader->variant_key_size);
}

static void
dump_cs_variant_key(const struct lp_compute_shader_variant_key *key)
{
   debug_printf("cs variant %p:\n", (void *) key);
}

static void
lp_debug_cs_variant(const struct lp_compute_shader_variant *variant)
{
   debug_printf("llvmpipe: Compute shader #%u variant #%u:\n",
                variant->shader->no, variant->no);
   tgsi_dump(variant->shader->base.tokens, 0);
   dump_cs_variant_key(&variant->key);
   debug_printf("\n");
}

static struct lp_compute_shader_variant *
generate_variant(struct llvmpipe_context *lp,
                 struct lp_compute_shader *shader,
                 const struct lp_compute_shader_variant_key *key)
{
   struct lp_compute_shader_variant *variant;
   char module_name[64];

   variant = CALLOC_STRUCT(lp_compute_shader_variant);
   if (!variant)
      return NULL;

   snprintf(module_name, sizeof(module_name), "cs%u_variant%u",
            shader->no, shader->variants_created);

   variant->gallivm = gallivm_create(module_name, lp->context);
   if (!variant->gallivm) {
      FREE(variant);
      return NULL;
   }

   variant->shader = shader;
   variant->list_item_global.base = variant;
   variant->list_item_local.base = variant;
   variant->no = shader->variants_created++;

   memcpy(&variant->key, key, shader->variant_key_size);

   if ((LP_DEBUG & DEBUG_CS) || (gallivm_debug & GALLIVM_DEBUG_IR)) {
      lp_debug_cs_variant(variant);
   }

   lp_jit_init_cs_types(variant);

   gallivm_free_ir(variant->gallivm);
   return variant;
}

static void
lp_cs_ctx_set_cs_variant( struct lp_cs_context *csctx,
                          struct lp_compute_shader_variant *variant)
{
   csctx->cs.current.variant = variant;
}

static void
llvmpipe_update_cs(struct llvmpipe_context *lp)
{
   struct lp_compute_shader *shader = lp->cs;

   struct lp_compute_shader_variant_key key;
   struct lp_compute_shader_variant *variant = NULL;
   struct lp_cs_variant_list_item *li;

   make_variant_key(lp, shader, &key);

   /* Search the variants for one which matches the key */
   li = first_elem(&shader->variants);
   while(!at_end(&shader->variants, li)) {
      if(memcmp(&li->base->key, &key, shader->variant_key_size) == 0) {
         variant = li->base;
         break;
      }
      li = next_elem(li);
   }

   if (variant) {
      /* Move this variant to the head of the list to implement LRU
       * deletion of shader's when we have too many.
       */
      move_to_head(&lp->cs_variants_list, &variant->list_item_global);
   }
   else {
      /* variant not found, create it now */
      int64_t t0, t1, dt;
      unsigned i;
      unsigned variants_to_cull;

      if (LP_DEBUG & DEBUG_CS) {
         debug_printf("%u variants,\t%u instrs,\t%u instrs/variant\n",
                      lp->nr_cs_variants,
                      lp->nr_cs_instrs,
                      lp->nr_cs_variants ? lp->nr_cs_instrs / lp->nr_cs_variants : 0);
      }

      /* First, check if we've exceeded the max number of shader variants.
       * If so, free 6.25% of them (the least recently used ones).
       */
      variants_to_cull = lp->nr_cs_variants >= LP_MAX_SHADER_VARIANTS ? LP_MAX_SHADER_VARIANTS / 16 : 0;

      if (variants_to_cull ||
          lp->nr_cs_instrs >= LP_MAX_SHADER_INSTRUCTIONS) {
         if (gallivm_debug & GALLIVM_DEBUG_PERF) {
            debug_printf("Evicting CS: %u cs variants,\t%u total variants,"
                         "\t%u instrs,\t%u instrs/variant\n",
                         shader->variants_cached,
                         lp->nr_cs_variants, lp->nr_cs_instrs,
                         lp->nr_cs_instrs / lp->nr_cs_variants);
         }

         /*
          * We need to re-check lp->nr_cs_variants because an arbitrarliy large
          * number of shader variants (potentially all of them) could be
          * pending for destruction on flush.
          */

         for (i = 0; i < variants_to_cull || lp->nr_cs_instrs >= LP_MAX_SHADER_INSTRUCTIONS; i++) {
            struct lp_cs_variant_list_item *item;
            if (is_empty_list(&lp->cs_variants_list)) {
               break;
            }
            item = last_elem(&lp->cs_variants_list);
            assert(item);
            assert(item->base);
            llvmpipe_remove_cs_shader_variant(lp, item->base);
         }
      }
      /*
       * Generate the new variant.
       */
      t0 = os_time_get();
      variant = generate_variant(lp, shader, &key);
      t1 = os_time_get();
      dt = t1 - t0;
      LP_COUNT_ADD(llvm_compile_time, dt);
      LP_COUNT_ADD(nr_llvm_compiles, 2);  /* emit vs. omit in/out test */

      /* Put the new variant into the list */
      if (variant) {
         insert_at_head(&shader->variants, &variant->list_item_local);
         insert_at_head(&lp->cs_variants_list, &variant->list_item_global);
         lp->nr_cs_variants++;
         lp->nr_cs_instrs += variant->nr_instrs;
         shader->variants_cached++;
      }
   }
   /* Bind this variant */
   lp_cs_ctx_set_cs_variant(lp->csctx, variant);
}

static void
llvmpipe_cs_update_derived(struct llvmpipe_context *llvmpipe)
{
   if (llvmpipe->cs_dirty & (LP_CSNEW_CS))
      llvmpipe_update_cs(llvmpipe);

   llvmpipe->cs_dirty = 0;
}

static void llvmpipe_launch_grid(struct pipe_context *pipe,
                                 const struct pipe_grid_info *info)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);

   llvmpipe_cs_update_derived(llvmpipe);
}

void
llvmpipe_init_compute_funcs(struct llvmpipe_context *llvmpipe)
{
   llvmpipe->pipe.create_compute_state = llvmpipe_create_compute_state;
   llvmpipe->pipe.bind_compute_state = llvmpipe_bind_compute_state;
   llvmpipe->pipe.delete_compute_state = llvmpipe_delete_compute_state;
   llvmpipe->pipe.launch_grid = llvmpipe_launch_grid;
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

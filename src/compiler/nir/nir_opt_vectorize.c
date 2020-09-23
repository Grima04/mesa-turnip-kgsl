/*
 * Copyright © 2015 Connor Abbott
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "nir.h"
#include "nir_vla.h"
#include "nir_builder.h"
#include "util/u_dynarray.h"

#define HASH(hash, data) XXH32(&data, sizeof(data), hash)

static uint32_t
hash_src(uint32_t hash, const nir_src *src)
{
   assert(src->is_ssa);
   void *hash_data = nir_src_is_const(*src) ? NULL : src->ssa;

   return HASH(hash, hash_data);
}

static uint32_t
hash_alu_src(uint32_t hash, const nir_alu_src *src)
{
   assert(!src->abs && !src->negate);

   /* intentionally don't hash swizzle */

   return hash_src(hash, &src->src);
}

static uint32_t
hash_alu(uint32_t hash, const nir_alu_instr *instr)
{
   hash = HASH(hash, instr->op);

   hash = HASH(hash, instr->dest.dest.ssa.bit_size);

   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++)
      hash = hash_alu_src(hash, &instr->src[i]);

   return hash;
}

static uint32_t
hash_instr(const void *data)
{
   const nir_instr *instr = (nir_instr *) data;
   uint32_t hash = 0;

   switch (instr->type) {
   case nir_instr_type_alu:
      return hash_alu(hash, nir_instr_as_alu(instr));
   default:
      unreachable("bad instruction type");
   }
}

static bool
srcs_equal(const nir_src *src1, const nir_src *src2)
{
   assert(src1->is_ssa);
   assert(src2->is_ssa);

   return src1->ssa == src2->ssa ||
      nir_src_is_const(*src1) == nir_src_is_const(*src2);
}

static bool
alu_srcs_equal(const nir_alu_src *src1, const nir_alu_src *src2)
{
   assert(!src1->abs);
   assert(!src1->negate);
   assert(!src2->abs);
   assert(!src2->negate);

   return srcs_equal(&src1->src, &src2->src);
}

static bool
instrs_equal(const void *data1, const void *data2)
{
   const nir_instr *instr1 = (nir_instr *) data1;
   const nir_instr *instr2 = (nir_instr *) data2;
   switch (instr1->type) {
   case nir_instr_type_alu: {
      nir_alu_instr *alu1 = nir_instr_as_alu(instr1);
      nir_alu_instr *alu2 = nir_instr_as_alu(instr2);

      if (alu1->op != alu2->op)
         return false;

      if (alu1->dest.dest.ssa.bit_size != alu2->dest.dest.ssa.bit_size)
         return false;

      for (unsigned i = 0; i < nir_op_infos[alu1->op].num_inputs; i++) {
         if (!alu_srcs_equal(&alu1->src[i], &alu2->src[i]))
            return false;
      }

      return true;
   }

   default:
      unreachable("bad instruction type");
   }
}

static bool
instr_can_rewrite(nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      /* Don't try and vectorize mov's. Either they'll be handled by copy
       * prop, or they're actually necessary and trying to vectorize them
       * would result in fighting with copy prop.
       */
      if (alu->op == nir_op_mov)
         return false;

      if (nir_op_infos[alu->op].output_size != 0)
         return false;

      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         if (nir_op_infos[alu->op].input_sizes[i] != 0)
            return false;
      }

      return true;
   }

   /* TODO support phi nodes */
   default:
      break;
   }

   return false;
}

/*
 * Tries to combine two instructions whose sources are different components of
 * the same instructions into one vectorized instruction. Note that instr1
 * should dominate instr2.
 */

static nir_instr *
instr_try_combine(struct nir_shader *nir, nir_instr *instr1, nir_instr *instr2,
                  nir_opt_vectorize_cb filter, void *data)
{
   assert(instr1->type == nir_instr_type_alu);
   assert(instr2->type == nir_instr_type_alu);
   nir_alu_instr *alu1 = nir_instr_as_alu(instr1);
   nir_alu_instr *alu2 = nir_instr_as_alu(instr2);

   assert(alu1->dest.dest.ssa.bit_size == alu2->dest.dest.ssa.bit_size);
   unsigned alu1_components = alu1->dest.dest.ssa.num_components;
   unsigned alu2_components = alu2->dest.dest.ssa.num_components;
   unsigned total_components = alu1_components + alu2_components;

   if (total_components > 4)
      return NULL;

   if (nir->options->vectorize_vec2_16bit &&
       (total_components > 2 || alu1->dest.dest.ssa.bit_size != 16))
      return NULL;

   if (filter && !filter(&alu1->instr, &alu2->instr, data))
      return NULL;

   nir_builder b;
   nir_builder_init(&b, nir_cf_node_get_function(&instr1->block->cf_node));
   b.cursor = nir_after_instr(instr1);

   nir_alu_instr *new_alu = nir_alu_instr_create(b.shader, alu1->op);
   nir_ssa_dest_init(&new_alu->instr, &new_alu->dest.dest,
                     total_components, alu1->dest.dest.ssa.bit_size, NULL);
   new_alu->dest.write_mask = (1 << total_components) - 1;

   /* If either channel is exact, we have to preserve it even if it's
    * not optimal for other channels.
    */
   new_alu->exact = alu1->exact || alu2->exact;

   /* If all channels don't wrap, we can say that the whole vector doesn't
    * wrap.
    */
   new_alu->no_signed_wrap = alu1->no_signed_wrap && alu2->no_signed_wrap;
   new_alu->no_unsigned_wrap = alu1->no_unsigned_wrap && alu2->no_unsigned_wrap;

   for (unsigned i = 0; i < nir_op_infos[alu1->op].num_inputs; i++) {
      /* handle constant merging case */
      if (alu1->src[i].src.ssa != alu2->src[i].src.ssa) {
         nir_const_value *c1 = nir_src_as_const_value(alu1->src[i].src);
         nir_const_value *c2 = nir_src_as_const_value(alu2->src[i].src);
         assert(c1 && c2);
         nir_const_value value[NIR_MAX_VEC_COMPONENTS];
         unsigned bit_size = alu1->src[i].src.ssa->bit_size;

         for (unsigned j = 0; j < total_components; j++) {
            value[j].u64 = j < alu1_components ?
                              c1[alu1->src[i].swizzle[j]].u64 :
                              c2[alu2->src[i].swizzle[j - alu1_components]].u64;
         }
         nir_ssa_def *def = nir_build_imm(&b, total_components, bit_size, value);

         new_alu->src[i].src = nir_src_for_ssa(def);
         for (unsigned j = 0; j < total_components; j++)
            new_alu->src[i].swizzle[j] = j;
         continue;
      }

      new_alu->src[i].src = alu1->src[i].src;

      for (unsigned j = 0; j < alu1_components; j++)
         new_alu->src[i].swizzle[j] = alu1->src[i].swizzle[j];

      for (unsigned j = 0; j < alu2_components; j++) {
         new_alu->src[i].swizzle[j + alu1_components] =
            alu2->src[i].swizzle[j];
      }
   }

   nir_builder_instr_insert(&b, &new_alu->instr);

   unsigned swiz[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; i++)
      swiz[i] = i;
   nir_ssa_def *new_alu1 = nir_swizzle(&b, &new_alu->dest.dest.ssa, swiz,
                                       alu1_components);

   for (unsigned i = 0; i < alu2_components; i++)
      swiz[i] += alu1_components;
   nir_ssa_def *new_alu2 = nir_swizzle(&b, &new_alu->dest.dest.ssa, swiz,
                                       alu2_components);

   nir_foreach_use_safe(src, &alu1->dest.dest.ssa) {
      if (src->parent_instr->type == nir_instr_type_alu) {
         /* For ALU instructions, rewrite the source directly to avoid a
          * round-trip through copy propagation.
          */

         nir_instr_rewrite_src(src->parent_instr, src,
                               nir_src_for_ssa(&new_alu->dest.dest.ssa));
      } else {
         nir_instr_rewrite_src(src->parent_instr, src,
                               nir_src_for_ssa(new_alu1));
      }
   }

   nir_foreach_if_use_safe(src, &alu1->dest.dest.ssa) {
      nir_if_rewrite_condition(src->parent_if, nir_src_for_ssa(new_alu1));
   }

   assert(list_is_empty(&alu1->dest.dest.ssa.uses));
   assert(list_is_empty(&alu1->dest.dest.ssa.if_uses));

   nir_foreach_use_safe(src, &alu2->dest.dest.ssa) {
      if (src->parent_instr->type == nir_instr_type_alu) {
         /* For ALU instructions, rewrite the source directly to avoid a
          * round-trip through copy propagation.
          */

         nir_alu_instr *use = nir_instr_as_alu(src->parent_instr);

         unsigned src_index = 5;
         for (unsigned i = 0; i < nir_op_infos[use->op].num_inputs; i++) {
            if (&use->src[i].src == src) {
               src_index = i;
               break;
            }
         }
         assert(src_index != 5);

         nir_instr_rewrite_src(src->parent_instr, src,
                               nir_src_for_ssa(&new_alu->dest.dest.ssa));

         for (unsigned i = 0;
              i < nir_ssa_alu_instr_src_components(use, src_index); i++) {
            use->src[src_index].swizzle[i] += alu1_components;
         }
      } else {
         nir_instr_rewrite_src(src->parent_instr, src,
                               nir_src_for_ssa(new_alu2));
      }
   }

   nir_foreach_if_use_safe(src, &alu2->dest.dest.ssa) {
      nir_if_rewrite_condition(src->parent_if, nir_src_for_ssa(new_alu2));
   }

   assert(list_is_empty(&alu2->dest.dest.ssa.uses));
   assert(list_is_empty(&alu2->dest.dest.ssa.if_uses));

   nir_instr_remove(instr1);
   nir_instr_remove(instr2);

   return &new_alu->instr;
}

static struct set *
vec_instr_set_create(void)
{
   return _mesa_set_create(NULL, hash_instr, instrs_equal);
}

static void
vec_instr_set_destroy(struct set *instr_set)
{
   _mesa_set_destroy(instr_set, NULL);
}

static bool
vec_instr_set_add_or_rewrite(struct nir_shader *nir, struct set *instr_set,
                             nir_instr *instr,
                             nir_opt_vectorize_cb filter, void *data)
{
   if (!instr_can_rewrite(instr))
      return false;

   struct set_entry *entry = _mesa_set_search(instr_set, instr);
   if (entry) {
      nir_instr *old_instr = (nir_instr *) entry->key;
      _mesa_set_remove(instr_set, entry);
      nir_instr *new_instr = instr_try_combine(nir, old_instr, instr,
                                               filter, data);
      if (new_instr) {
         _mesa_set_add(instr_set, new_instr);
         return true;
      }
   }

   _mesa_set_add(instr_set, instr);
   return false;
}

static bool
vectorize_block(struct nir_shader *nir, nir_block *block,
                struct set *instr_set,
                nir_opt_vectorize_cb filter, void *data)
{
   bool progress = false;

   nir_foreach_instr_safe(instr, block) {
      if (vec_instr_set_add_or_rewrite(nir, instr_set, instr, filter, data))
         progress = true;
   }

   for (unsigned i = 0; i < block->num_dom_children; i++) {
      nir_block *child = block->dom_children[i];
      progress |= vectorize_block(nir, child, instr_set, filter, data);
   }

   nir_foreach_instr_reverse(instr, block) {
      if (instr->type != nir_instr_type_alu)
         continue;

      struct set_entry *entry = _mesa_set_search(instr_set, instr);
      if (entry)
         _mesa_set_remove(instr_set, entry);
   }

   return progress;
}

static bool
nir_opt_vectorize_impl(struct nir_shader *nir, nir_function_impl *impl,
                       nir_opt_vectorize_cb filter, void *data)
{
   struct set *instr_set = vec_instr_set_create();

   nir_metadata_require(impl, nir_metadata_dominance);

   bool progress = vectorize_block(nir, nir_start_block(impl), instr_set,
                                   filter, data);

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   vec_instr_set_destroy(instr_set);
   return progress;
}

bool
nir_opt_vectorize(nir_shader *shader, nir_opt_vectorize_cb filter,
                  void *data)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl)
         progress |= nir_opt_vectorize_impl(shader, function->impl, filter, data);
   }

   return progress;
}

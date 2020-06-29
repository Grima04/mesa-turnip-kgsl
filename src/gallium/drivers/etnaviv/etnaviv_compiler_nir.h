/*
 * Copyright (c) 2020 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Jonathan Marek <jonathan@marek.ca>
 */

#ifndef H_ETNAVIV_COMPILER_NIR
#define H_ETNAVIV_COMPILER_NIR

#include "compiler/nir/nir.h"

#define compile_error(ctx, args...) ({ \
   printf(args); \
   ctx->error = true; \
   assert(0); \
})

enum {
   BYPASS_DST = 1,
   BYPASS_SRC = 2,
};

static inline bool is_sysval(nir_instr *instr)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   return intr->intrinsic == nir_intrinsic_load_front_face ||
          intr->intrinsic == nir_intrinsic_load_frag_coord;
}

/* get unique ssa/reg index for nir_src */
static inline unsigned
src_index(nir_function_impl *impl, nir_src *src)
{
   return src->is_ssa ? src->ssa->index : (src->reg.reg->index + impl->ssa_alloc);
}

/* get unique ssa/reg index for nir_dest */
static inline unsigned
dest_index(nir_function_impl *impl, nir_dest *dest)
{
   return dest->is_ssa ? dest->ssa.index : (dest->reg.reg->index + impl->ssa_alloc);
}

static inline void
update_swiz_mask(nir_alu_instr *alu, nir_dest *dest, unsigned *swiz, unsigned *mask)
{
   if (!swiz)
      return;

   bool is_vec = dest != NULL;
   unsigned swizzle = 0, write_mask = 0;
   for (unsigned i = 0; i < 4; i++) {
      /* channel not written */
      if (!(alu->dest.write_mask & (1 << i)))
         continue;
      /* src is different (only check for vecN) */
      if (is_vec && alu->src[i].src.ssa != &dest->ssa)
         continue;

      unsigned src_swiz = is_vec ? alu->src[i].swizzle[0] : alu->src[0].swizzle[i];
      swizzle |= (*swiz >> src_swiz * 2 & 3) << i * 2;
      /* this channel isn't written through this chain */
      if (*mask & (1 << src_swiz))
         write_mask |= 1 << i;
   }
   *swiz = swizzle;
   *mask = write_mask;
}

static nir_dest *
real_dest(nir_dest *dest, unsigned *swiz, unsigned *mask)
{
   if (!dest || !dest->is_ssa)
      return dest;

   bool can_bypass_src = !list_length(&dest->ssa.if_uses);
   nir_instr *p_instr = dest->ssa.parent_instr;

   /* if used by a vecN, the "real" destination becomes the vecN destination
    * lower_alu guarantees that values used by a vecN are only used by that vecN
    * we can apply the same logic to movs in a some cases too
    */
   nir_foreach_use(use_src, &dest->ssa) {
      nir_instr *instr = use_src->parent_instr;

      /* src bypass check: for now only deal with tex src mov case
       * note: for alu don't bypass mov for multiple uniform sources
       */
      switch (instr->type) {
      case nir_instr_type_tex:
         if (p_instr->type == nir_instr_type_alu &&
             nir_instr_as_alu(p_instr)->op == nir_op_mov) {
            break;
         }
      default:
         can_bypass_src = false;
         break;
      }

      if (instr->type != nir_instr_type_alu)
         continue;

      nir_alu_instr *alu = nir_instr_as_alu(instr);

      switch (alu->op) {
      case nir_op_vec2:
      case nir_op_vec3:
      case nir_op_vec4:
         assert(list_length(&dest->ssa.if_uses) == 0);
         nir_foreach_use(use_src, &dest->ssa)
            assert(use_src->parent_instr == instr);

         update_swiz_mask(alu, dest, swiz, mask);
         break;
      case nir_op_mov: {
         switch (dest->ssa.parent_instr->type) {
         case nir_instr_type_alu:
         case nir_instr_type_tex:
            break;
         default:
            continue;
         }
         if (list_length(&dest->ssa.if_uses) || list_length(&dest->ssa.uses) > 1)
            continue;

         update_swiz_mask(alu, NULL, swiz, mask);
         break;
      };
      default:
         continue;
      }

      assert(!(instr->pass_flags & BYPASS_SRC));
      instr->pass_flags |= BYPASS_DST;
      return real_dest(&alu->dest.dest, swiz, mask);
   }

   if (can_bypass_src && !(p_instr->pass_flags & BYPASS_DST)) {
      p_instr->pass_flags |= BYPASS_SRC;
      return NULL;
   }

   return dest;
}

/* if instruction dest needs a register, return nir_dest for it */
static inline nir_dest *
dest_for_instr(nir_instr *instr)
{
   nir_dest *dest = NULL;

   switch (instr->type) {
   case nir_instr_type_alu:
      dest = &nir_instr_as_alu(instr)->dest.dest;
      break;
   case nir_instr_type_tex:
      dest = &nir_instr_as_tex(instr)->dest;
      break;
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic == nir_intrinsic_load_uniform ||
          intr->intrinsic == nir_intrinsic_load_ubo ||
          intr->intrinsic == nir_intrinsic_load_input ||
          intr->intrinsic == nir_intrinsic_load_instance_id)
         dest = &intr->dest;
   } break;
   case nir_instr_type_deref:
      return NULL;
   default:
      break;
   }
   return real_dest(dest, NULL, NULL);
}

struct live_def {
   nir_instr *instr;
   nir_dest *dest; /* cached dest_for_instr */
   unsigned live_start, live_end; /* live range */
};

unsigned
etna_live_defs(nir_function_impl *impl, struct live_def *defs, unsigned *live_map);

#endif

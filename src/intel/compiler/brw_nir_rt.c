/*
 * Copyright © 2020 Intel Corporation
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
 */

#include "brw_nir_rt.h"
#include "nir_builder.h"

static bool
resize_deref(nir_builder *b, nir_deref_instr *deref,
             unsigned num_components, unsigned bit_size)
{
   assert(deref->dest.is_ssa);
   if (deref->dest.ssa.num_components == num_components &&
       deref->dest.ssa.bit_size == bit_size)
      return false;

   /* NIR requires array indices have to match the deref bit size */
   if (deref->dest.ssa.bit_size != bit_size &&
       (deref->deref_type == nir_deref_type_array ||
        deref->deref_type == nir_deref_type_ptr_as_array)) {
      b->cursor = nir_before_instr(&deref->instr);
      assert(deref->arr.index.is_ssa);
      nir_ssa_def *idx;
      if (nir_src_is_const(deref->arr.index)) {
         idx = nir_imm_intN_t(b, nir_src_as_int(deref->arr.index), bit_size);
      } else {
         idx = nir_i2i(b, deref->arr.index.ssa, bit_size);
      }
      nir_instr_rewrite_src(&deref->instr, &deref->arr.index,
                            nir_src_for_ssa(idx));
   }

   deref->dest.ssa.num_components = num_components;
   deref->dest.ssa.bit_size = bit_size;

   return true;
}

static bool
resize_function_temp_derefs(nir_shader *shader)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   bool progress = false;

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_deref)
            continue;

         nir_deref_instr *deref = nir_instr_as_deref(instr);

         /* We're going to lower all function_temp memory to scratch using
          * 64-bit addresses.  We need to resize all our derefs first or else
          * nir_lower_explicit_io will have a fit.
          */
         if (nir_deref_mode_is(deref, nir_var_function_temp) &&
             resize_deref(&b, deref, 1, 64))
            progress = true;
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return progress;
}

static void
lower_rt_scratch(nir_shader *nir)
{
   /* First, we to ensure all the local variables have explicit types. */
   NIR_PASS_V(nir, nir_lower_vars_to_explicit_types,
              nir_var_function_temp,
              glsl_get_natural_size_align_bytes);

   NIR_PASS_V(nir, resize_function_temp_derefs);

   /* Now, lower those variables to 64-bit global memory access */
   NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_function_temp,
              nir_address_format_64bit_global);
}

void
brw_nir_lower_raygen(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_RAYGEN);
   lower_rt_scratch(nir);
}

void
brw_nir_lower_any_hit(nir_shader *nir, const struct gen_device_info *devinfo)
{
   assert(nir->info.stage == MESA_SHADER_ANY_HIT);
   lower_rt_scratch(nir);
}

void
brw_nir_lower_closest_hit(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_CLOSEST_HIT);
   lower_rt_scratch(nir);
}

void
brw_nir_lower_miss(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_MISS);
   lower_rt_scratch(nir);
}

void
brw_nir_lower_callable(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_CALLABLE);
   lower_rt_scratch(nir);
}

void
brw_nir_lower_combined_intersection_any_hit(nir_shader *intersection,
                                            const nir_shader *any_hit,
                                            const struct gen_device_info *devinfo)
{
   assert(intersection->info.stage == MESA_SHADER_INTERSECTION);
   assert(any_hit == NULL || any_hit->info.stage == MESA_SHADER_ANY_HIT);
   lower_rt_scratch(intersection);
}

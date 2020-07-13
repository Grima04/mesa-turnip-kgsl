/*
 * Copyright © 2020 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "tu_private.h"
#include "nir_builder.h"

/* Some a6xx variants cannot support a non-contiguous multiview mask. Instead,
 * inside the shader something like this needs to be inserted:
 *
 * gl_Position = ((1ull << gl_ViewIndex) & view_mask) ? gl_Position : vec4(0.);
 *
 * Scan backwards until we find the gl_Position write (there should only be
 * one).
 */
static bool
lower_multiview_mask(nir_function_impl *impl, uint32_t mask)
{
   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block_reverse(block, impl) {
      nir_foreach_instr_reverse(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_store_deref)
            continue;

         nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
         if (deref->mode != nir_var_shader_out)
            continue;

         nir_variable *var = nir_deref_instr_get_variable(deref);
         if (var->data.location != VARYING_SLOT_POS)
            continue;

         assert(intrin->src[1].is_ssa);
         nir_ssa_def *orig_src = intrin->src[1].ssa;
         b.cursor = nir_before_instr(instr);

         /* ((1ull << gl_ViewIndex) & mask) != 0 */
         nir_ssa_def *cmp =
            nir_i2b(&b, nir_iand(&b, nir_imm_int(&b, mask),
                                  nir_ishl(&b, nir_imm_int(&b, 1),
                                           nir_load_view_index(&b))));

         nir_ssa_def *src = nir_bcsel(&b, cmp, orig_src, nir_imm_float(&b, 0.));
         nir_instr_rewrite_src(instr, &intrin->src[1], nir_src_for_ssa(src));

         nir_metadata_preserve(impl, nir_metadata_block_index |
                                     nir_metadata_dominance);
         return true;
      }
   }

   nir_metadata_preserve(impl, nir_metadata_all);
   return false;
}

bool
tu_nir_lower_multiview(nir_shader *nir, uint32_t mask, struct tu_device *dev)
{
   nir_function_impl *entrypoint = nir_shader_get_entrypoint(nir);

   if (!dev->physical_device->supports_multiview_mask &&
       !util_is_power_of_two_or_zero(mask + 1)) {
      return lower_multiview_mask(entrypoint, mask);
   }

   nir_metadata_preserve(entrypoint, nir_metadata_all);
   return false;
}


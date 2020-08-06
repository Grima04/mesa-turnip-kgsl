/*
 * Copyright (c) 2020 Intel Corporation
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
#include "brw_nir_rt_builder.h"

static void
lower_rt_intrinsics_impl(nir_function_impl *impl,
                         const struct gen_device_info *devinfo)
{
   nir_builder build;
   nir_builder_init(&build, impl);
   nir_builder *b = &build;

   b->cursor = nir_before_block(nir_start_block(b->impl));

   struct brw_nir_rt_globals_defs globals;
   brw_nir_rt_load_globals(b, &globals);

   nir_ssa_def *hotzone_addr = brw_nir_rt_sw_hotzone_addr(b, devinfo);
   nir_ssa_def *hotzone = nir_load_global(b, hotzone_addr, 16, 4, 32);

   nir_ssa_def *thread_stack_base_addr = brw_nir_rt_sw_stack_addr(b, devinfo);
   nir_ssa_def *stack_base_offset = nir_channel(b, hotzone, 0);
   nir_ssa_def *stack_base_addr =
      nir_iadd(b, thread_stack_base_addr, nir_u2u64(b, stack_base_offset));
   ASSERTED bool seen_scratch_base_ptr_load = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

         b->cursor = nir_after_instr(&intrin->instr);

         nir_ssa_def *sysval = NULL;
         switch (intrin->intrinsic) {
         case nir_intrinsic_load_scratch_base_ptr:
            assert(nir_intrinsic_base(intrin) == 1);
            seen_scratch_base_ptr_load = true;
            sysval = stack_base_addr;
            break;

         case nir_intrinsic_btd_stack_push_intel: {
            int32_t stack_size = nir_intrinsic_range(intrin);
            if (stack_size > 0) {
               nir_ssa_def *child_stack_offset =
                  nir_iadd_imm(b, stack_base_offset, stack_size);
               nir_store_global(b, hotzone_addr, 16, child_stack_offset, 0x1);
            }
            nir_instr_remove(instr);
            break;
         }

         case nir_intrinsic_btd_resume_intel:
            /* This is the first "interesting" instruction */
            assert(block == nir_start_block(impl));
            assert(!seen_scratch_base_ptr_load);

            int32_t stack_size = nir_intrinsic_range(intrin);
            if (stack_size > 0) {
               stack_base_offset =
                  nir_iadd_imm(b, stack_base_offset, -stack_size);
               nir_store_global(b, hotzone_addr, 16, stack_base_offset, 0x1);
               stack_base_addr = nir_iadd(b, thread_stack_base_addr,
                                          nir_u2u64(b, stack_base_offset));
            }
            nir_instr_remove(instr);
            break;

         case nir_intrinsic_load_ray_base_mem_addr_intel:
            sysval = globals.base_mem_addr;
            break;

         case nir_intrinsic_load_ray_hw_stack_size_intel:
            sysval = nir_imul_imm(b, globals.hw_stack_size, 64);
            break;

         case nir_intrinsic_load_ray_sw_stack_size_intel:
            sysval = nir_imul_imm(b, globals.sw_stack_size, 64);
            break;

         case nir_intrinsic_load_ray_num_dss_rt_stacks_intel:
            sysval = globals.num_dss_rt_stacks;
            break;

         case nir_intrinsic_load_callable_sbt_addr_intel:
            sysval = globals.call_sbt_addr;
            break;

         case nir_intrinsic_load_callable_sbt_stride_intel:
            sysval = globals.call_sbt_stride;
            break;

         case nir_intrinsic_load_btd_resume_sbt_addr_intel:
            /* The call stack handler is just the first in our resume SBT */
            sysval = globals.resume_sbt_addr;
            break;

         default:
            continue;
         }

         if (sysval) {
            nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                                     nir_src_for_ssa(sysval));
            nir_instr_remove(&intrin->instr);
         }
      }
   }

   nir_metadata_preserve(impl, nir_metadata_block_index |
                               nir_metadata_dominance);
}

void
brw_nir_lower_rt_intrinsics(nir_shader *nir,
                            const struct gen_device_info *devinfo)
{
   nir_foreach_function(function, nir) {
      if (function->impl)
         lower_rt_intrinsics_impl(function->impl, devinfo);
   }
}

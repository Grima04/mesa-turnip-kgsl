/*
 * Copyright © 2019 Intel Corporation
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

#include "anv_nir.h"
#include "nir_builder.h"
#include "compiler/brw_nir.h"
#include "util/mesa-sha1.h"

void
anv_nir_compute_push_layout(const struct anv_physical_device *pdevice,
                            bool robust_buffer_access,
                            nir_shader *nir,
                            struct brw_stage_prog_data *prog_data,
                            struct anv_pipeline_bind_map *map,
                            void *mem_ctx)
{
   const struct brw_compiler *compiler = pdevice->compiler;
   memset(map->push_ranges, 0, sizeof(map->push_ranges));

   bool has_const_ubo = false;
   unsigned push_start = UINT_MAX, push_end = 0;
   nir_foreach_function(function, nir) {
      if (!function->impl)
         continue;

      nir_foreach_block(block, function->impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_load_ubo:
               if (nir_src_is_const(intrin->src[0]) &&
                   nir_src_is_const(intrin->src[1]))
                  has_const_ubo = true;
               break;

            case nir_intrinsic_load_push_constant: {
               unsigned base = nir_intrinsic_base(intrin);
               unsigned range = nir_intrinsic_range(intrin);
               push_start = MIN2(push_start, base);
               push_end = MAX2(push_end, base + range);
               break;
            }

            default:
               break;
            }
         }
      }
   }

   const bool has_push_intrinsic = push_start <= push_end;

   const bool push_ubo_ranges =
      (pdevice->info.gen >= 8 || pdevice->info.is_haswell) &&
      has_const_ubo && nir->info.stage != MESA_SHADER_COMPUTE;

   if (push_ubo_ranges && robust_buffer_access) {
      /* We can't on-the-fly adjust our push ranges because doing so would
       * mess up the layout in the shader.  When robustBufferAccess is
       * enabled, we have to manually bounds check our pushed UBO accesses.
       */
      const uint32_t ubo_size_start =
         offsetof(struct anv_push_constants, push_ubo_sizes);
      const uint32_t ubo_size_end = ubo_size_start + (4 * sizeof(uint32_t));
      push_start = MIN2(push_start, ubo_size_start);
      push_end = MAX2(push_end, ubo_size_end);
   }

   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      /* For compute shaders, we always have to have the subgroup ID.  The
       * back-end compiler will "helpfully" add it for us in the last push
       * constant slot.  Yes, there is an off-by-one error here but that's
       * because the back-end will add it so we want to claim the number of
       * push constants one dword less than the full amount including
       * gl_SubgroupId.
       */
      assert(push_end <= offsetof(struct anv_push_constants, cs.subgroup_id));
      push_end = offsetof(struct anv_push_constants, cs.subgroup_id);
   }

   /* Align push_start down to a 32B boundary and make it no larger than
    * push_end (no push constants is indicated by push_start = UINT_MAX).
    */
   push_start = MIN2(push_start, push_end);
   push_start = align_down_u32(push_start, 32);

   /* For vec4 our push data size needs to be aligned to a vec4 and for
    * scalar, it needs to be aligned to a DWORD.
    */
   const unsigned align = compiler->scalar_stage[nir->info.stage] ? 4 : 16;
   nir->num_uniforms = ALIGN(push_end - push_start, align);
   prog_data->nr_params = nir->num_uniforms / 4;
   prog_data->param = rzalloc_array(mem_ctx, uint32_t, prog_data->nr_params);

   struct anv_push_range push_constant_range = {
      .set = ANV_DESCRIPTOR_SET_PUSH_CONSTANTS,
      .start = push_start / 32,
      .length = DIV_ROUND_UP(push_end - push_start, 32),
   };

   /* Mapping from brw_ubo_range to anv_push_range */
   int push_range_idx_map[4] = { -1, -1, -1, -1 };

   if (push_ubo_ranges) {
      brw_nir_analyze_ubo_ranges(compiler, nir, NULL, prog_data->ubo_ranges);

      /* We can push at most 64 registers worth of data.  The back-end
       * compiler would do this fixup for us but we'd like to calculate
       * the push constant layout ourselves.
       */
      unsigned total_push_regs = push_constant_range.length;
      for (unsigned i = 0; i < 4; i++) {
         if (total_push_regs + prog_data->ubo_ranges[i].length > 64)
            prog_data->ubo_ranges[i].length = 64 - total_push_regs;
         total_push_regs += prog_data->ubo_ranges[i].length;
      }
      assert(total_push_regs <= 64);

      int n = 0;

      if (push_constant_range.length > 0)
         map->push_ranges[n++] = push_constant_range;

      for (int i = 0; i < 4; i++) {
         struct brw_ubo_range *ubo_range = &prog_data->ubo_ranges[i];
         if (ubo_range->length == 0)
            continue;

         if (n >= 4 || (n == 3 && compiler->constant_buffer_0_is_relative)) {
            memset(ubo_range, 0, sizeof(*ubo_range));
            continue;
         }

         const struct anv_pipeline_binding *binding =
            &map->surface_to_descriptor[ubo_range->block];

         push_range_idx_map[i] = n;
         map->push_ranges[n++] = (struct anv_push_range) {
            .set = binding->set,
            .index = binding->index,
            .dynamic_offset_index = binding->dynamic_offset_index,
            .start = ubo_range->start,
            .length = ubo_range->length,
         };
      }
   } else {
      /* For Ivy Bridge, the push constants packets have a different
       * rule that would require us to iterate in the other direction
       * and possibly mess around with dynamic state base address.
       * Don't bother; just emit regular push constants at n = 0.
       *
       * In the compute case, we don't have multiple push ranges so it's
       * better to just provide one in push_ranges[0].
       */
      map->push_ranges[0] = push_constant_range;
   }

   if (has_push_intrinsic || (push_ubo_ranges && robust_buffer_access)) {
      nir_foreach_function(function, nir) {
         if (!function->impl)
            continue;

         nir_builder b;
         nir_builder_init(&b, function->impl);

         nir_foreach_block(block, function->impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;

               nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
               switch (intrin->intrinsic) {
               case nir_intrinsic_load_ubo: {
                  if (!robust_buffer_access)
                     break;

                  if (!nir_src_is_const(intrin->src[0]) ||
                      !nir_src_is_const(intrin->src[1]))
                     break;

                  uint32_t index = nir_src_as_uint(intrin->src[0]);
                  uint64_t offset = nir_src_as_uint(intrin->src[1]);
                  uint32_t size = intrin->num_components *
                                  (intrin->dest.ssa.bit_size / 8);

                  int ubo_range_idx = -1;
                  for (unsigned i = 0; i < 4; i++) {
                     if (prog_data->ubo_ranges[i].length > 0 &&
                         prog_data->ubo_ranges[i].block == index) {
                        ubo_range_idx = i;
                        break;
                     }
                  }

                  if (ubo_range_idx < 0)
                     break;

                  const struct brw_ubo_range *range =
                     &prog_data->ubo_ranges[ubo_range_idx];
                  const uint32_t range_end =
                     (range->start + range->length) * 32;

                  if (range_end < offset || offset + size <= range->start)
                     break;

                  b.cursor = nir_after_instr(&intrin->instr);

                  assert(push_range_idx_map[ubo_range_idx] >= 0);
                  const uint32_t ubo_size_offset =
                     offsetof(struct anv_push_constants, push_ubo_sizes) +
                     push_range_idx_map[ubo_range_idx] * sizeof(uint32_t);

                  nir_intrinsic_instr *load_size =
                     nir_intrinsic_instr_create(b.shader,
                                                nir_intrinsic_load_uniform);
                  load_size->src[0] = nir_src_for_ssa(nir_imm_int(&b, 0));
                  nir_intrinsic_set_base(load_size,
                                         ubo_size_offset - push_start);
                  nir_intrinsic_set_range(load_size, 4);
                  nir_intrinsic_set_type(load_size, nir_type_uint32);
                  load_size->num_components = 1;
                  nir_ssa_dest_init(&load_size->instr, &load_size->dest,
                                    1, 32, NULL);
                  nir_builder_instr_insert(&b, &load_size->instr);

                  /* Do the size checks per-component.  Thanks to scalar block
                   * layout, we could end up with a single vector straddling a
                   * 32B boundary.
                   *
                   * We intentionally push a size starting from the UBO
                   * binding in the descriptor set rather than starting from
                   * the started of the pushed range.  This prevents us from
                   * accidentally flagging things as out-of-bounds due to
                   * roll-over if a vector access crosses the push range
                   * boundary.
                   *
                   * We align up to 32B so that we can get better CSE.
                   *
                   * We check
                   *
                   *    offset + size - 1 < push_ubo_sizes[i]
                   *
                   * rather than
                   *
                   *    offset + size <= push_ubo_sizes[i]
                   *
                   * because it properly returns OOB for the case where
                   * offset + size == 0.
                   */
                  nir_const_value last_byte_const[NIR_MAX_VEC_COMPONENTS];
                  for (unsigned c = 0; c < intrin->dest.ssa.num_components; c++) {
                     assert(intrin->dest.ssa.bit_size % 8 == 0);
                     const unsigned comp_size_B = intrin->dest.ssa.bit_size / 8;
                     const uint32_t comp_last_byte =
                        align_u32(offset + (c + 1) * comp_size_B,
                                  ANV_UBO_BOUNDS_CHECK_ALIGNMENT) - 1;
                     last_byte_const[c] =
                        nir_const_value_for_uint(comp_last_byte, 32);
                  }
                  nir_ssa_def *last_byte =
                     nir_build_imm(&b, intrin->dest.ssa.num_components, 32,
                                   last_byte_const);
                  nir_ssa_def *in_bounds =
                     nir_ult(&b, last_byte, &load_size->dest.ssa);

                  nir_ssa_def *zero =
                     nir_imm_zero(&b, intrin->dest.ssa.num_components,
                                      intrin->dest.ssa.bit_size);
                  nir_ssa_def *value =
                     nir_bcsel(&b, in_bounds, &intrin->dest.ssa, zero);
                  nir_ssa_def_rewrite_uses_after(&intrin->dest.ssa,
                                                 nir_src_for_ssa(value),
                                                 value->parent_instr);
                  break;
               }

               case nir_intrinsic_load_push_constant:
                  intrin->intrinsic = nir_intrinsic_load_uniform;
                  nir_intrinsic_set_base(intrin,
                                         nir_intrinsic_base(intrin) -
                                         push_start);
                  break;

               default:
                  break;
               }
            }
         }
      }
   }

   /* Now that we're done computing the push constant portion of the
    * bind map, hash it.  This lets us quickly determine if the actual
    * mapping has changed and not just a no-op pipeline change.
    */
   _mesa_sha1_compute(map->push_ranges,
                      sizeof(map->push_ranges),
                      map->push_sha1);
}

void
anv_nir_validate_push_layout(struct brw_stage_prog_data *prog_data,
                             struct anv_pipeline_bind_map *map)
{
#ifndef NDEBUG
   unsigned prog_data_push_size = DIV_ROUND_UP(prog_data->nr_params, 8);
   for (unsigned i = 0; i < 4; i++)
      prog_data_push_size += prog_data->ubo_ranges[i].length;

   unsigned bind_map_push_size = 0;
   for (unsigned i = 0; i < 4; i++)
      bind_map_push_size += map->push_ranges[i].length;

   /* We could go through everything again but it should be enough to assert
    * that they push the same number of registers.  This should alert us if
    * the back-end compiler decides to re-arrange stuff or shrink a range.
    */
   assert(prog_data_push_size == bind_map_push_size);
#endif
}

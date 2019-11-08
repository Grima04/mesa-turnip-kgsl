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
#include "compiler/brw_nir.h"

void
anv_compute_push_layout(const struct anv_physical_device *pdevice,
                        struct brw_stage_prog_data *prog_data,
                        struct anv_pipeline_bind_map *map)
{
   struct anv_push_range push_constant_range = {
      .set = ANV_DESCRIPTOR_SET_PUSH_CONSTANTS,
      .length = DIV_ROUND_UP(prog_data->nr_params, 8),
   };

   if (pdevice->info.gen >= 8 || pdevice->info.is_haswell) {
      /* The Skylake PRM contains the following restriction:
       *
       *    "The driver must ensure The following case does not occur
       *     without a flush to the 3D engine: 3DSTATE_CONSTANT_* with
       *     buffer 3 read length equal to zero committed followed by a
       *     3DSTATE_CONSTANT_* with buffer 0 read length not equal to
       *     zero committed."
       *
       * To avoid this, we program the buffers in the highest slots.
       * This way, slot 0 is only used if slot 3 is also used.
       */
      int n = 3;

      for (int i = 3; i >= 0; i--) {
         const struct brw_ubo_range *ubo_range = &prog_data->ubo_ranges[i];
         if (ubo_range->length == 0)
            continue;

         const struct anv_pipeline_binding *binding =
            &map->surface_to_descriptor[ubo_range->block];

         map->push_ranges[n--] = (struct anv_push_range) {
            .set = binding->set,
            .index = binding->index,
            .dynamic_offset_index = binding->dynamic_offset_index,
            .start = ubo_range->start,
            .length = ubo_range->length,
         };
      }

      if (push_constant_range.length > 0)
         map->push_ranges[n--] = push_constant_range;
   } else {
      /* For Ivy Bridge, the push constants packets have a different
       * rule that would require us to iterate in the other direction
       * and possibly mess around with dynamic state base address.
       * Don't bother; just emit regular push constants at n = 0.
       */
      map->push_ranges[0] = push_constant_range;
   }
}

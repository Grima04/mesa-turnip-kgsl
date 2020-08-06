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

void
brw_nir_lower_raygen(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_RAYGEN);
}

void
brw_nir_lower_any_hit(nir_shader *nir, const struct gen_device_info *devinfo)
{
   assert(nir->info.stage == MESA_SHADER_ANY_HIT);
}

void
brw_nir_lower_closest_hit(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_CLOSEST_HIT);
}

void
brw_nir_lower_miss(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_MISS);
}

void
brw_nir_lower_callable(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_CALLABLE);
}

void
brw_nir_lower_combined_intersection_any_hit(nir_shader *intersection,
                                            const nir_shader *any_hit,
                                            const struct gen_device_info *devinfo)
{
   assert(intersection->info.stage == MESA_SHADER_INTERSECTION);
   assert(any_hit == NULL || any_hit->info.stage == MESA_SHADER_ANY_HIT);
}

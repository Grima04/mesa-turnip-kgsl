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
#include "brw_nir_rt_builder.h"

/** Insert the appropriate return instruction at the end of the shader */
void
brw_nir_lower_shader_returns(nir_shader *shader)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   /* Reserve scratch space at the start of the shader's per-thread scratch
    * space for the return BINDLESS_SHADER_RECORD address and data payload.
    * When a shader is called, the calling shader will write the return BSR
    * address in this region of the callee's scratch space.
    *
    * We could also put it at the end of the caller's scratch space.  However,
    * doing this way means that a shader never accesses its caller's scratch
    * space unless given an explicit pointer (such as for ray payloads).  It
    * also makes computing the address easier given that we want to apply an
    * alignment to the scratch offset to ensure we can make alignment
    * assumptions in the called shader.
    *
    * This isn't needed for ray-gen shaders because they end the thread and
    * never return to the calling trampoline shader.
    */
   assert(shader->scratch_size == 0);
   if (shader->info.stage != MESA_SHADER_RAYGEN)
      shader->scratch_size = BRW_BTD_STACK_CALLEE_DATA_SIZE;

   nir_builder b;
   nir_builder_init(&b, impl);

   set_foreach(impl->end_block->predecessors, block_entry) {
      struct nir_block *block = (void *)block_entry->key;
      b.cursor = nir_after_block_before_jump(block);

      switch (shader->info.stage) {
      case MESA_SHADER_RAYGEN:
         /* A raygen shader is always the root of the shader call tree.  When
          * it ends, we retire the bindless stack ID and no further shaders
          * will be executed.
          */
         brw_nir_btd_retire(&b);
         break;

      case MESA_SHADER_ANY_HIT:
         /* The default action of an any-hit shader is to accept the ray
          * intersection.
          */
         nir_accept_ray_intersection(&b);
         break;

      case MESA_SHADER_CALLABLE:
      case MESA_SHADER_MISS:
      case MESA_SHADER_CLOSEST_HIT:
         /* Callable, miss, and closest-hit shaders don't take any special
          * action at the end.  They simply return back to the previous shader
          * in the call stack.
          */
         brw_nir_btd_return(&b);
         break;

      case MESA_SHADER_INTERSECTION:
         unreachable("TODO");

      default:
         unreachable("Invalid callable shader stage");
      }

      assert(impl->end_block->predecessors->entries == 1);
      break;
   }

   nir_metadata_preserve(impl, nir_metadata_block_index |
                               nir_metadata_dominance);
}

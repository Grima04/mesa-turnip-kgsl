/*
 * Copyright (C) 2019 Collabora, Ltd.
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
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

/**
 * @file
 *
 * Implements framebuffer format conversions in software, specifically for
 * blend shaders on Midgard/Bifrost. load_output/store_output (derefs more
 * correctly -- pre I/O lowering) normally for the fragment stage within the
 * blend shader will operate with purely vec4 float ("nir") encodings. This
 * lowering stage, to be run before I/O is lowered, converts the native
 * framebuffer format to a NIR encoding after loads and vice versa before
 * stores. This pass is designed for a single render target; Midgard duplicates
 * blend shaders for MRT to simplify everything.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nir_lower_blend.h"

static nir_ssa_def *
nir_float_to_native(nir_builder *b, nir_ssa_def *c_float)
{
   /* First, we scale from [0, 1] to [0, 255.0] */
   nir_ssa_def *scaled = nir_fmul_imm(b, nir_fsat(b, c_float), 255.0);

   /* Next, we type convert */
   nir_ssa_def *converted = nir_u2u8(b, nir_f2u32(b,
            nir_fround_even(b, scaled)));

   return converted;
}

static nir_ssa_def *
nir_native_to_float(nir_builder *b, nir_ssa_def *c_native)
{
   /* First, we convert up from u8 to f32 */
   nir_ssa_def *converted = nir_u2f32(b, nir_u2u32(b, c_native));

   /* Next, we scale down from [0, 255.0] to [0, 1] */
   nir_ssa_def *scaled = nir_fsat(b, nir_fmul_imm(b, converted, 1.0/255.0));

   return scaled;
}

void
nir_lower_framebuffer(nir_shader *shader)
{
   /* Blend shaders are represented as special fragment shaders */
   assert(shader->info.stage == MESA_SHADER_FRAGMENT);

   nir_foreach_function(func, shader) {
      nir_foreach_block(block, func->impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            bool is_load = intr->intrinsic == nir_intrinsic_load_deref;
            bool is_store = intr->intrinsic == nir_intrinsic_store_deref;

            if (!(is_load || is_store))
               continue;

            /* Don't worry about MRT */
            nir_variable *var = nir_intrinsic_get_var(intr, 0);

            if (var->data.location != FRAG_RESULT_COLOR)
               continue;

            nir_builder b;
            nir_builder_init(&b, func->impl);

            if (is_store) {
               /* For stores, add conversion before */
               b.cursor = nir_before_instr(instr);

               /* Grab the input color */
               nir_ssa_def *c_nir = nir_ssa_for_src(&b, intr->src[1], 4);

               /* Format convert */
               nir_ssa_def *converted = nir_float_to_native(&b, c_nir);

               /* Rewrite to use a native store by creating a new intrinsic */
               nir_intrinsic_instr *new =
                  nir_intrinsic_instr_create(shader, nir_intrinsic_store_raw_output_pan);
               new->src[0] = nir_src_for_ssa(converted);

               /* TODO: What about non-RGBA? Is that different? */
               new->num_components = 4;

               nir_builder_instr_insert(&b, &new->instr);

               /* (And finally removing the old) */
               nir_instr_remove(instr);
            } else {
               /* For loads, add conversion after */
               b.cursor = nir_after_instr(instr);

               /* Rewrite to use a native load by creating a new intrinsic */

               nir_intrinsic_instr *new =
                  nir_intrinsic_instr_create(shader, nir_intrinsic_load_raw_output_pan);

               new->num_components = 4;

               unsigned bitsize = 8;
               nir_ssa_dest_init(&new->instr, &new->dest, 4, bitsize, NULL);
               nir_builder_instr_insert(&b, &new->instr);

               /* Convert the raw value */
               nir_ssa_def *raw = &new->dest.ssa;
               nir_ssa_def *converted = nir_native_to_float(&b, raw);

               /* Rewrite to use the converted value */
               nir_src rewritten = nir_src_for_ssa(converted);
               nir_ssa_def_rewrite_uses_after(&intr->dest.ssa, rewritten, instr);

               /* Finally, remove the old load */
               nir_instr_remove(instr);
            }
         }
      }

      nir_metadata_preserve(func->impl, nir_metadata_block_index |
                            nir_metadata_dominance);
   }
}

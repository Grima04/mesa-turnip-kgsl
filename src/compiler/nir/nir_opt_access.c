/*
 * Copyright © 2019 Valve Corporation
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

#include "nir.h"

/* This pass optimizes GL access qualifiers. So far it does two things:
 *
 * - Infer readonly when it's missing.
 * - Infer ACCESS_CAN_REORDER when the following are true:
 *   - Either there are no writes, or ACCESS_NON_WRITEABLE is set. In either
 *     case there are no writes to the underlying memory.
 *   - ACCESS_VOLATILE is not set.
 *
 * If these conditions are true, then image and buffer reads may be treated as
 * if they were uniform buffer reads, i.e. they may be arbitrarily moved,
 * combined, rematerialized etc.
 */

struct access_state {
   struct set *vars_written;
   bool images_written;
   bool buffers_written;
};

static void
gather_intrinsic(struct access_state *state, nir_intrinsic_instr *instr)
{
   nir_variable *var;
   switch (instr->intrinsic) {
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic_add:
   case nir_intrinsic_image_deref_atomic_imin:
   case nir_intrinsic_image_deref_atomic_umin:
   case nir_intrinsic_image_deref_atomic_imax:
   case nir_intrinsic_image_deref_atomic_umax:
   case nir_intrinsic_image_deref_atomic_and:
   case nir_intrinsic_image_deref_atomic_or:
   case nir_intrinsic_image_deref_atomic_xor:
   case nir_intrinsic_image_deref_atomic_exchange:
   case nir_intrinsic_image_deref_atomic_comp_swap:
   case nir_intrinsic_image_deref_atomic_fadd:
      var = nir_intrinsic_get_var(instr, 0);

      /* In OpenGL, buffer images use normal buffer objects, whereas other
       * image types use textures which cannot alias with buffer objects.
       * Therefore we have to group buffer samplers together with SSBO's.
       */
      if (glsl_get_sampler_dim(glsl_without_array(var->type)) ==
          GLSL_SAMPLER_DIM_BUF)
         state->buffers_written = true;
      else
         state->images_written = true;

      if (var->data.mode == nir_var_uniform)
         _mesa_set_add(state->vars_written, var);
      break;

   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_atomic_add:
   case nir_intrinsic_bindless_image_atomic_imin:
   case nir_intrinsic_bindless_image_atomic_umin:
   case nir_intrinsic_bindless_image_atomic_imax:
   case nir_intrinsic_bindless_image_atomic_umax:
   case nir_intrinsic_bindless_image_atomic_and:
   case nir_intrinsic_bindless_image_atomic_or:
   case nir_intrinsic_bindless_image_atomic_xor:
   case nir_intrinsic_bindless_image_atomic_exchange:
   case nir_intrinsic_bindless_image_atomic_comp_swap:
   case nir_intrinsic_bindless_image_atomic_fadd:
      if (nir_intrinsic_image_dim(instr) == GLSL_SAMPLER_DIM_BUF)
         state->buffers_written = true;
      else
         state->images_written = true;
      break;

   case nir_intrinsic_store_deref:
   case nir_intrinsic_deref_atomic_add:
   case nir_intrinsic_deref_atomic_imin:
   case nir_intrinsic_deref_atomic_umin:
   case nir_intrinsic_deref_atomic_imax:
   case nir_intrinsic_deref_atomic_umax:
   case nir_intrinsic_deref_atomic_and:
   case nir_intrinsic_deref_atomic_or:
   case nir_intrinsic_deref_atomic_xor:
   case nir_intrinsic_deref_atomic_exchange:
   case nir_intrinsic_deref_atomic_comp_swap:
   case nir_intrinsic_deref_atomic_fadd:
   case nir_intrinsic_deref_atomic_fmin:
   case nir_intrinsic_deref_atomic_fmax:
   case nir_intrinsic_deref_atomic_fcomp_swap:
      var = nir_intrinsic_get_var(instr, 0);
      if (var->data.mode != nir_var_mem_ssbo)
         break;

      _mesa_set_add(state->vars_written, var);
      state->buffers_written = true;
      break;

   default:
      break;
   }
}

static bool
process_variable(struct access_state *state, nir_variable *var)
{
   const struct glsl_type *type = glsl_without_array(var->type);
   if (var->data.mode != nir_var_mem_ssbo &&
       !(var->data.mode == nir_var_uniform && glsl_type_is_image(type)))
      return false;

   /* Ignore variables we've already marked */
   if (var->data.access & ACCESS_CAN_REORDER)
      return false;

   if (!(var->data.access & ACCESS_NON_WRITEABLE)) {
      if ((var->data.access & ACCESS_RESTRICT) &&
          !_mesa_set_search(state->vars_written, var)) {
         var->data.access |= ACCESS_NON_WRITEABLE;
         return true;
      }

      bool is_buffer = var->data.mode == nir_var_mem_ssbo ||
                       glsl_get_sampler_dim(type) == GLSL_SAMPLER_DIM_BUF;
      if (is_buffer ? !state->buffers_written : !state->images_written) {
         var->data.access |= ACCESS_NON_WRITEABLE;
         return true;
      }
   }

   return false;
}

static bool
update_access(struct access_state *state, nir_intrinsic_instr *instr, bool is_image, bool is_buffer)
{
   enum gl_access_qualifier access = nir_intrinsic_access(instr);

   bool is_memory_readonly = access & ACCESS_NON_WRITEABLE;

   if (instr->intrinsic != nir_intrinsic_bindless_image_load) {
      const nir_variable *var = nir_intrinsic_get_var(instr, 0);
      is_memory_readonly |= var->data.access & ACCESS_NON_WRITEABLE;
   }

   is_memory_readonly |= is_buffer ? !state->buffers_written : !state->images_written;

   if (is_memory_readonly)
      access |= ACCESS_NON_WRITEABLE;
   if (!(access & ACCESS_VOLATILE) && is_memory_readonly)
      access |= ACCESS_CAN_REORDER;

   bool progress = nir_intrinsic_access(instr) != access;
   nir_intrinsic_set_access(instr, access);
   return progress;
}

static bool
process_intrinsic(struct access_state *state, nir_intrinsic_instr *instr)
{
   switch (instr->intrinsic) {
   case nir_intrinsic_bindless_image_load:
      return update_access(state, instr, true,
                           nir_intrinsic_image_dim(instr) == GLSL_SAMPLER_DIM_BUF);

   case nir_intrinsic_load_deref: {
      nir_variable *var = nir_intrinsic_get_var(instr, 0);
      if (var->data.mode != nir_var_mem_ssbo)
         return false;

      return update_access(state, instr, false, true);
   }

   case nir_intrinsic_image_deref_load: {
      nir_variable *var = nir_intrinsic_get_var(instr, 0);

      bool is_buffer =
         glsl_get_sampler_dim(glsl_without_array(var->type)) == GLSL_SAMPLER_DIM_BUF;

      return update_access(state, instr, true, is_buffer);
   }

   default:
      return false;
   }
}

static bool
opt_access_impl(struct access_state *state,
                nir_function_impl *impl)
{
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic)
            progress |= process_intrinsic(state,
                                          nir_instr_as_intrinsic(instr));
      }
   }

   if (progress) {
      nir_metadata_preserve(impl,
                            nir_metadata_block_index |
                            nir_metadata_dominance |
                            nir_metadata_live_ssa_defs |
                            nir_metadata_loop_analysis);
   }


   return progress;
}

bool
nir_opt_access(nir_shader *shader)
{
   struct access_state state = {
      .vars_written = _mesa_pointer_set_create(NULL),
   };

   bool var_progress = false;
   bool progress = false;

   nir_foreach_function(func, shader) {
      if (func->impl) {
         nir_foreach_block(block, func->impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type == nir_instr_type_intrinsic)
                  gather_intrinsic(&state, nir_instr_as_intrinsic(instr));
            }
         }
      }
   }

   nir_foreach_variable_with_modes(var, shader, nir_var_uniform |
                                                nir_var_mem_ubo |
                                                nir_var_mem_ssbo)
      var_progress |= process_variable(&state, var);

   nir_foreach_function(func, shader) {
      if (func->impl) {
         progress |= opt_access_impl(&state, func->impl);

         /* If we make a change to the uniforms, update all the impls. */
         if (var_progress) {
            nir_metadata_preserve(func->impl,
                                  nir_metadata_block_index |
                                  nir_metadata_dominance |
                                  nir_metadata_live_ssa_defs |
                                  nir_metadata_loop_analysis);
         }
      }
   }

   progress |= var_progress;

   _mesa_set_destroy(state.vars_written, NULL);
   return progress;
}

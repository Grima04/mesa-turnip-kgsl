/*
 * Copyright © 2019 Red Hat.
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

#include "val_private.h"
#include "nir.h"
#include "nir_builder.h"
#include "val_lower_vulkan_resource.h"

static bool
lower_vulkan_resource_index(const nir_instr *instr, const void *data_cb)
{
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      if (intrin->intrinsic == nir_intrinsic_vulkan_resource_index)
         return true;
   }
   if (instr->type == nir_instr_type_tex) {
      return true;
   }
   return false;
}

static nir_ssa_def *lower_vri_intrin_vri(struct nir_builder *b,
                                           nir_instr *instr, void *data_cb)
{
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   unsigned desc_set_idx = nir_intrinsic_desc_set(intrin);
   unsigned binding_idx = nir_intrinsic_binding(intrin);
   struct val_pipeline_layout *layout = data_cb;
   struct val_descriptor_set_binding_layout *binding = &layout->set[desc_set_idx].layout->binding[binding_idx];
   int value = 0;
   bool is_ubo = (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                  binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);

   for (unsigned s = 0; s < desc_set_idx; s++) {
     if (is_ubo)
       value += layout->set[s].layout->stage[b->shader->info.stage].const_buffer_count;
     else
       value += layout->set[s].layout->stage[b->shader->info.stage].shader_buffer_count;
   }
   if (is_ubo)
     value += binding->stage[b->shader->info.stage].const_buffer_index + 1;
   else
     value += binding->stage[b->shader->info.stage].shader_buffer_index;
   if (nir_src_is_const(intrin->src[0])) {
      value += nir_src_comp_as_int(intrin->src[0], 0);
      return nir_imm_int(b, value);
   } else
      return nir_iadd_imm(b, intrin->src[0].ssa, value);
}

static int lower_vri_instr_tex_deref(nir_tex_instr *tex,
                                     nir_tex_src_type deref_src_type,
                                     gl_shader_stage stage,
                                     struct val_pipeline_layout *layout)
{
   int deref_src_idx = nir_tex_instr_src_index(tex, deref_src_type);

   if (deref_src_idx < 0)
      return -1;

   nir_deref_instr *deref_instr = nir_src_as_deref(tex->src[deref_src_idx].src);
   nir_variable *var = nir_deref_instr_get_variable(deref_instr);
   unsigned desc_set_idx = var->data.descriptor_set;
   unsigned binding_idx = var->data.binding;
   int value = 0;
   struct val_descriptor_set_binding_layout *binding = &layout->set[desc_set_idx].layout->binding[binding_idx];
   nir_tex_instr_remove_src(tex, deref_src_idx);
   for (unsigned s = 0; s < desc_set_idx; s++) {
      if (deref_src_type == nir_tex_src_sampler_deref)
         value += layout->set[s].layout->stage[stage].sampler_count;
      else
         value += layout->set[s].layout->stage[stage].sampler_view_count;
   }
   if (deref_src_type == nir_tex_src_sampler_deref)
      value += binding->stage[stage].sampler_index;
   else
      value += binding->stage[stage].sampler_view_index;

   if (deref_instr->deref_type == nir_deref_type_array) {
      if (nir_src_is_const(deref_instr->arr.index))
         value += nir_src_as_uint(deref_instr->arr.index);
      else {
         if (deref_src_type == nir_tex_src_sampler_deref)
            nir_tex_instr_add_src(tex, nir_tex_src_sampler_offset, deref_instr->arr.index);
         else
            nir_tex_instr_add_src(tex, nir_tex_src_texture_offset, deref_instr->arr.index);
      }
   }
   if (deref_src_type == nir_tex_src_sampler_deref)
      tex->sampler_index = value;
   else
      tex->texture_index = value;
   return value;
}

static void lower_vri_instr_tex(struct nir_builder *b,
                                nir_tex_instr *tex, void *data_cb)
{
   struct val_pipeline_layout *layout = data_cb;
   int tex_value = 0;

   lower_vri_instr_tex_deref(tex, nir_tex_src_sampler_deref, b->shader->info.stage, layout);
   tex_value = lower_vri_instr_tex_deref(tex, nir_tex_src_texture_deref, b->shader->info.stage, layout);
   if (tex_value >= 0)
      b->shader->info.textures_used |= (1 << tex_value);
}

static nir_ssa_def *lower_vri_instr(struct nir_builder *b,
                                    nir_instr *instr, void *data_cb)
{
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      if (intrin->intrinsic == nir_intrinsic_vulkan_resource_index)
         return lower_vri_intrin_vri(b, instr, data_cb);
   }
   if (instr->type == nir_instr_type_tex)
      lower_vri_instr_tex(b, nir_instr_as_tex(instr), data_cb);
   return NULL;
}

void val_lower_pipeline_layout(const struct val_device *device,
                               struct val_pipeline_layout *layout,
                               nir_shader *shader)
{
   nir_shader_lower_instructions(shader, lower_vulkan_resource_index, lower_vri_instr, layout);
   nir_foreach_uniform_variable(var, shader) {
      const struct glsl_type *type = var->type;
      enum glsl_base_type base_type =
         glsl_get_base_type(glsl_without_array(type));
      unsigned desc_set_idx = var->data.descriptor_set;
      unsigned binding_idx = var->data.binding;
      struct val_descriptor_set_binding_layout *binding = &layout->set[desc_set_idx].layout->binding[binding_idx];
      int value = 0;
      var->data.descriptor_set = 0;
      if (base_type == GLSL_TYPE_SAMPLER) {
         if (binding->type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            for (unsigned s = 0; s < desc_set_idx; s++)
               value += layout->set[s].layout->stage[shader->info.stage].sampler_count;
            value += binding->stage[shader->info.stage].sampler_index;
         } else {
            for (unsigned s = 0; s < desc_set_idx; s++)
               value += layout->set[s].layout->stage[shader->info.stage].sampler_view_count;
            value += binding->stage[shader->info.stage].sampler_view_index;
         }
         var->data.binding = value;
      }
      if (base_type == GLSL_TYPE_IMAGE) {
         var->data.descriptor_set = 0;
         for (unsigned s = 0; s < desc_set_idx; s++)
           value += layout->set[s].layout->stage[shader->info.stage].image_count;
         value += binding->stage[shader->info.stage].image_index;
         var->data.binding = value;
      }
   }
}

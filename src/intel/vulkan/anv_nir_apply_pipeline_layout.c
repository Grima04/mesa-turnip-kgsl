/*
 * Copyright © 2015 Intel Corporation
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
#include "program/prog_parameter.h"
#include "nir/nir_builder.h"
#include "compiler/brw_nir.h"

/* Sampler tables don't actually have a maximum size but we pick one just so
 * that we don't end up emitting too much state on-the-fly.
 */
#define MAX_SAMPLER_TABLE_SIZE 128
#define BINDLESS_OFFSET        255

struct apply_pipeline_layout_state {
   const struct anv_physical_device *pdevice;

   nir_shader *shader;
   nir_builder builder;

   struct anv_pipeline_layout *layout;
   bool add_bounds_checks;

   bool uses_constants;
   uint8_t constants_offset;
   struct {
      bool desc_buffer_used;
      uint8_t desc_offset;

      uint8_t *use_count;
      uint8_t *surface_offsets;
      uint8_t *sampler_offsets;
   } set[MAX_SETS];
};

static void
add_binding(struct apply_pipeline_layout_state *state,
            uint32_t set, uint32_t binding)
{
   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   if (state->set[set].use_count[binding] < UINT8_MAX)
      state->set[set].use_count[binding]++;

   /* Only flag the descriptor buffer as used if there's actually data for
    * this binding.  This lets us be lazy and call this function constantly
    * without worrying about unnecessarily enabling the buffer.
    */
   if (anv_descriptor_size(bind_layout))
      state->set[set].desc_buffer_used = true;
}

static void
add_deref_src_binding(struct apply_pipeline_layout_state *state, nir_src src)
{
   nir_deref_instr *deref = nir_src_as_deref(src);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   add_binding(state, var->data.descriptor_set, var->data.binding);
}

static void
add_tex_src_binding(struct apply_pipeline_layout_state *state,
                    nir_tex_instr *tex, nir_tex_src_type deref_src_type)
{
   int deref_src_idx = nir_tex_instr_src_index(tex, deref_src_type);
   if (deref_src_idx < 0)
      return;

   add_deref_src_binding(state, tex->src[deref_src_idx].src);
}

static void
get_used_bindings_block(nir_block *block,
                        struct apply_pipeline_layout_state *state)
{
   nir_foreach_instr_safe(instr, block) {
      switch (instr->type) {
      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_vulkan_resource_index:
            add_binding(state, nir_intrinsic_desc_set(intrin),
                        nir_intrinsic_binding(intrin));
            break;

         case nir_intrinsic_image_deref_load:
         case nir_intrinsic_image_deref_store:
         case nir_intrinsic_image_deref_atomic_add:
         case nir_intrinsic_image_deref_atomic_min:
         case nir_intrinsic_image_deref_atomic_max:
         case nir_intrinsic_image_deref_atomic_and:
         case nir_intrinsic_image_deref_atomic_or:
         case nir_intrinsic_image_deref_atomic_xor:
         case nir_intrinsic_image_deref_atomic_exchange:
         case nir_intrinsic_image_deref_atomic_comp_swap:
         case nir_intrinsic_image_deref_size:
         case nir_intrinsic_image_deref_samples:
         case nir_intrinsic_image_deref_load_param_intel:
         case nir_intrinsic_image_deref_load_raw_intel:
         case nir_intrinsic_image_deref_store_raw_intel:
            add_deref_src_binding(state, intrin->src[0]);
            break;

         case nir_intrinsic_load_constant:
            state->uses_constants = true;
            break;

         default:
            break;
         }
         break;
      }
      case nir_instr_type_tex: {
         nir_tex_instr *tex = nir_instr_as_tex(instr);
         add_tex_src_binding(state, tex, nir_tex_src_texture_deref);
         add_tex_src_binding(state, tex, nir_tex_src_sampler_deref);
         break;
      }
      default:
         continue;
      }
   }
}

static void
lower_res_index_intrinsic(nir_intrinsic_instr *intrin,
                          struct apply_pipeline_layout_state *state)
{
   nir_builder *b = &state->builder;

   b->cursor = nir_before_instr(&intrin->instr);

   uint32_t set = nir_intrinsic_desc_set(intrin);
   uint32_t binding = nir_intrinsic_binding(intrin);

   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   uint32_t surface_index = state->set[set].surface_offsets[binding];
   uint32_t array_size = bind_layout->array_size;

   nir_ssa_def *array_index = nir_ssa_for_src(b, intrin->src[0], 1);
   if (nir_src_is_const(intrin->src[0]) || state->add_bounds_checks)
      array_index = nir_umin(b, array_index, nir_imm_int(b, array_size - 1));

   nir_ssa_def *index;
   if (bind_layout->data & ANV_DESCRIPTOR_INLINE_UNIFORM) {
      /* This is an inline uniform block.  Just reference the descriptor set
       * and use the descriptor offset as the base.
       */
      index = nir_imm_ivec2(b, state->set[set].desc_offset,
                               bind_layout->descriptor_offset);
   } else {
      /* We're using nir_address_format_32bit_index_offset */
      index = nir_vec2(b, nir_iadd_imm(b, array_index, surface_index),
                          nir_imm_int(b, 0));
   }

   assert(intrin->dest.is_ssa);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, nir_src_for_ssa(index));
   nir_instr_remove(&intrin->instr);
}

static void
lower_res_reindex_intrinsic(nir_intrinsic_instr *intrin,
                            struct apply_pipeline_layout_state *state)
{
   nir_builder *b = &state->builder;

   b->cursor = nir_before_instr(&intrin->instr);

   /* For us, the resource indices are just indices into the binding table and
    * array elements are sequential.  A resource_reindex just turns into an
    * add of the two indices.
    */
   assert(intrin->src[0].is_ssa && intrin->src[1].is_ssa);
   nir_ssa_def *old_index = intrin->src[0].ssa;
   nir_ssa_def *offset = intrin->src[1].ssa;

   nir_ssa_def *new_index =
      nir_vec2(b, nir_iadd(b, nir_channel(b, old_index, 0), offset),
                  nir_channel(b, old_index, 1));

   assert(intrin->dest.is_ssa);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, nir_src_for_ssa(new_index));
   nir_instr_remove(&intrin->instr);
}

static void
lower_load_vulkan_descriptor(nir_intrinsic_instr *intrin,
                             struct apply_pipeline_layout_state *state)
{
   nir_builder *b = &state->builder;

   b->cursor = nir_before_instr(&intrin->instr);

   /* We follow the nir_address_format_32bit_index_offset model */
   assert(intrin->src[0].is_ssa);
   nir_ssa_def *index = intrin->src[0].ssa;

   assert(intrin->dest.is_ssa);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, nir_src_for_ssa(index));
   nir_instr_remove(&intrin->instr);
}

static void
lower_get_buffer_size(nir_intrinsic_instr *intrin,
                      struct apply_pipeline_layout_state *state)
{
   nir_builder *b = &state->builder;

   b->cursor = nir_before_instr(&intrin->instr);

   assert(intrin->src[0].is_ssa);
   nir_ssa_def *index = intrin->src[0].ssa;

   /* We're following the nir_address_format_32bit_index_offset model so the
    * binding table index is the first component of the address.  The
    * back-end wants a scalar binding table index source.
    */
   nir_instr_rewrite_src(&intrin->instr, &intrin->src[0],
                         nir_src_for_ssa(nir_channel(b, index, 0)));
}

static nir_ssa_def *
build_descriptor_load(nir_deref_instr *deref, unsigned offset,
                      unsigned num_components, unsigned bit_size,
                      struct apply_pipeline_layout_state *state)
{
   nir_variable *var = nir_deref_instr_get_variable(deref);

   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   unsigned array_size =
      state->layout->set[set].layout->binding[binding].array_size;

   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   nir_builder *b = &state->builder;

   nir_ssa_def *desc_buffer_index =
      nir_imm_int(b, state->set[set].desc_offset);

   nir_ssa_def *desc_offset =
      nir_imm_int(b, bind_layout->descriptor_offset + offset);
   if (deref->deref_type != nir_deref_type_var) {
      assert(deref->deref_type == nir_deref_type_array);

      const unsigned descriptor_size = anv_descriptor_size(bind_layout);
      nir_ssa_def *arr_index = nir_ssa_for_src(b, deref->arr.index, 1);
      if (state->add_bounds_checks)
         arr_index = nir_umin(b, arr_index, nir_imm_int(b, array_size - 1));

      desc_offset = nir_iadd(b, desc_offset,
                             nir_imul_imm(b, arr_index, descriptor_size));
   }

   nir_intrinsic_instr *desc_load =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_ubo);
   desc_load->src[0] = nir_src_for_ssa(desc_buffer_index);
   desc_load->src[1] = nir_src_for_ssa(desc_offset);
   desc_load->num_components = num_components;
   nir_ssa_dest_init(&desc_load->instr, &desc_load->dest,
                     num_components, bit_size, NULL);
   nir_builder_instr_insert(b, &desc_load->instr);

   return &desc_load->dest.ssa;
}

static void
lower_image_intrinsic(nir_intrinsic_instr *intrin,
                      struct apply_pipeline_layout_state *state)
{
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);

   nir_builder *b = &state->builder;
   b->cursor = nir_before_instr(&intrin->instr);

   if (intrin->intrinsic == nir_intrinsic_image_deref_load_param_intel) {
      b->cursor = nir_instr_remove(&intrin->instr);

      const unsigned param = nir_intrinsic_base(intrin);

      nir_ssa_def *desc =
         build_descriptor_load(deref, param * 16,
                               intrin->dest.ssa.num_components,
                               intrin->dest.ssa.bit_size, state);

      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, nir_src_for_ssa(desc));
   } else {
      nir_variable *var = nir_deref_instr_get_variable(deref);

      unsigned set = var->data.descriptor_set;
      unsigned binding = var->data.binding;
      unsigned binding_offset = state->set[set].surface_offsets[binding];
      unsigned array_size =
         state->layout->set[set].layout->binding[binding].array_size;

      nir_ssa_def *index = NULL;
      if (deref->deref_type != nir_deref_type_var) {
         assert(deref->deref_type == nir_deref_type_array);
         index = nir_ssa_for_src(b, deref->arr.index, 1);
         if (state->add_bounds_checks)
            index = nir_umin(b, index, nir_imm_int(b, array_size - 1));
      } else {
         index = nir_imm_int(b, 0);
      }

      index = nir_iadd_imm(b, index, binding_offset);
      nir_rewrite_image_intrinsic(intrin, index, false);
   }
}

static void
lower_load_constant(nir_intrinsic_instr *intrin,
                    struct apply_pipeline_layout_state *state)
{
   nir_builder *b = &state->builder;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_ssa_def *index = nir_imm_int(b, state->constants_offset);
   nir_ssa_def *offset = nir_iadd(b, nir_ssa_for_src(b, intrin->src[0], 1),
                                  nir_imm_int(b, nir_intrinsic_base(intrin)));

   nir_intrinsic_instr *load_ubo =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_ubo);
   load_ubo->num_components = intrin->num_components;
   load_ubo->src[0] = nir_src_for_ssa(index);
   load_ubo->src[1] = nir_src_for_ssa(offset);
   nir_ssa_dest_init(&load_ubo->instr, &load_ubo->dest,
                     intrin->dest.ssa.num_components,
                     intrin->dest.ssa.bit_size, NULL);
   nir_builder_instr_insert(b, &load_ubo->instr);

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                            nir_src_for_ssa(&load_ubo->dest.ssa));
   nir_instr_remove(&intrin->instr);
}

static void
lower_tex_deref(nir_tex_instr *tex, nir_tex_src_type deref_src_type,
                unsigned *base_index,
                struct apply_pipeline_layout_state *state)
{
   int deref_src_idx = nir_tex_instr_src_index(tex, deref_src_type);
   if (deref_src_idx < 0)
      return;

   nir_deref_instr *deref = nir_src_as_deref(tex->src[deref_src_idx].src);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   unsigned array_size =
      state->layout->set[set].layout->binding[binding].array_size;

   nir_tex_src_type offset_src_type;
   if (deref_src_type == nir_tex_src_texture_deref) {
      offset_src_type = nir_tex_src_texture_offset;
      *base_index = state->set[set].surface_offsets[binding];
   } else {
      assert(deref_src_type == nir_tex_src_sampler_deref);
      offset_src_type = nir_tex_src_sampler_offset;
      *base_index = state->set[set].sampler_offsets[binding];
   }

   nir_ssa_def *index = NULL;
   if (deref->deref_type != nir_deref_type_var) {
      assert(deref->deref_type == nir_deref_type_array);

      if (nir_src_is_const(deref->arr.index)) {
         unsigned arr_index = nir_src_as_uint(deref->arr.index);
         *base_index += MIN2(arr_index, array_size - 1);
      } else {
         nir_builder *b = &state->builder;

         /* From VK_KHR_sampler_ycbcr_conversion:
          *
          * If sampler Y’CBCR conversion is enabled, the combined image
          * sampler must be indexed only by constant integral expressions when
          * aggregated into arrays in shader code, irrespective of the
          * shaderSampledImageArrayDynamicIndexing feature.
          */
         assert(nir_tex_instr_src_index(tex, nir_tex_src_plane) == -1);

         index = nir_ssa_for_src(b, deref->arr.index, 1);

         if (state->add_bounds_checks)
            index = nir_umin(b, index, nir_imm_int(b, array_size - 1));
      }
   }

   if (index) {
      nir_instr_rewrite_src(&tex->instr, &tex->src[deref_src_idx].src,
                            nir_src_for_ssa(index));
      tex->src[deref_src_idx].src_type = offset_src_type;
   } else {
      nir_tex_instr_remove_src(tex, deref_src_idx);
   }
}

static uint32_t
tex_instr_get_and_remove_plane_src(nir_tex_instr *tex)
{
   int plane_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_plane);
   if (plane_src_idx < 0)
      return 0;

   unsigned plane = nir_src_as_uint(tex->src[plane_src_idx].src);

   nir_tex_instr_remove_src(tex, plane_src_idx);

   return plane;
}

static void
lower_tex(nir_tex_instr *tex, struct apply_pipeline_layout_state *state)
{
   state->builder.cursor = nir_before_instr(&tex->instr);

   unsigned plane = tex_instr_get_and_remove_plane_src(tex);

   lower_tex_deref(tex, nir_tex_src_texture_deref,
                   &tex->texture_index, state);
   tex->texture_index += plane;

   lower_tex_deref(tex, nir_tex_src_sampler_deref,
                   &tex->sampler_index, state);
   tex->sampler_index += plane;

   /* The backend only ever uses this to mark used surfaces.  We don't care
    * about that little optimization so it just needs to be non-zero.
    */
   tex->texture_array_size = 1;
}

static void
apply_pipeline_layout_block(nir_block *block,
                            struct apply_pipeline_layout_state *state)
{
   nir_foreach_instr_safe(instr, block) {
      switch (instr->type) {
      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_vulkan_resource_index:
            lower_res_index_intrinsic(intrin, state);
            break;
         case nir_intrinsic_vulkan_resource_reindex:
            lower_res_reindex_intrinsic(intrin, state);
            break;
         case nir_intrinsic_load_vulkan_descriptor:
            lower_load_vulkan_descriptor(intrin, state);
            break;
         case nir_intrinsic_get_buffer_size:
            lower_get_buffer_size(intrin, state);
            break;
         case nir_intrinsic_image_deref_load:
         case nir_intrinsic_image_deref_store:
         case nir_intrinsic_image_deref_atomic_add:
         case nir_intrinsic_image_deref_atomic_min:
         case nir_intrinsic_image_deref_atomic_max:
         case nir_intrinsic_image_deref_atomic_and:
         case nir_intrinsic_image_deref_atomic_or:
         case nir_intrinsic_image_deref_atomic_xor:
         case nir_intrinsic_image_deref_atomic_exchange:
         case nir_intrinsic_image_deref_atomic_comp_swap:
         case nir_intrinsic_image_deref_size:
         case nir_intrinsic_image_deref_samples:
         case nir_intrinsic_image_deref_load_param_intel:
         case nir_intrinsic_image_deref_load_raw_intel:
         case nir_intrinsic_image_deref_store_raw_intel:
            lower_image_intrinsic(intrin, state);
            break;
         case nir_intrinsic_load_constant:
            lower_load_constant(intrin, state);
            break;
         default:
            break;
         }
         break;
      }
      case nir_instr_type_tex:
         lower_tex(nir_instr_as_tex(instr), state);
         break;
      default:
         continue;
      }
   }
}

struct binding_info {
   uint32_t binding;
   uint8_t set;
   uint16_t score;
};

static int
compare_binding_infos(const void *_a, const void *_b)
{
   const struct binding_info *a = _a, *b = _b;
   if (a->score != b->score)
      return b->score - a->score;

   if (a->set != b->set)
      return a->set - b->set;

   return a->binding - b->binding;
}

void
anv_nir_apply_pipeline_layout(const struct anv_physical_device *pdevice,
                              bool robust_buffer_access,
                              struct anv_pipeline_layout *layout,
                              nir_shader *shader,
                              struct brw_stage_prog_data *prog_data,
                              struct anv_pipeline_bind_map *map)
{
   struct apply_pipeline_layout_state state = {
      .pdevice = pdevice,
      .shader = shader,
      .layout = layout,
      .add_bounds_checks = robust_buffer_access,
   };

   void *mem_ctx = ralloc_context(NULL);

   for (unsigned s = 0; s < layout->num_sets; s++) {
      const unsigned count = layout->set[s].layout->binding_count;
      state.set[s].use_count = rzalloc_array(mem_ctx, uint8_t, count);
      state.set[s].surface_offsets = rzalloc_array(mem_ctx, uint8_t, count);
      state.set[s].sampler_offsets = rzalloc_array(mem_ctx, uint8_t, count);
   }

   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      nir_foreach_block(block, function->impl)
         get_used_bindings_block(block, &state);
   }

   for (unsigned s = 0; s < layout->num_sets; s++) {
      if (state.set[s].desc_buffer_used) {
         map->surface_to_descriptor[map->surface_count] =
            (struct anv_pipeline_binding) {
               .set = ANV_DESCRIPTOR_SET_DESCRIPTORS,
               .binding = s,
            };
         state.set[s].desc_offset = map->surface_count;
         map->surface_count++;
      }
   }

   if (state.uses_constants) {
      state.constants_offset = map->surface_count;
      map->surface_to_descriptor[map->surface_count].set =
         ANV_DESCRIPTOR_SET_SHADER_CONSTANTS;
      map->surface_count++;
   }

   unsigned used_binding_count = 0;
   for (uint32_t set = 0; set < layout->num_sets; set++) {
      struct anv_descriptor_set_layout *set_layout = layout->set[set].layout;
      for (unsigned b = 0; b < set_layout->binding_count; b++) {
         if (state.set[set].use_count[b] == 0)
            continue;

         used_binding_count++;
      }
   }

   struct binding_info *infos =
      rzalloc_array(mem_ctx, struct binding_info, used_binding_count);
   used_binding_count = 0;
   for (uint32_t set = 0; set < layout->num_sets; set++) {
      struct anv_descriptor_set_layout *set_layout = layout->set[set].layout;
      for (unsigned b = 0; b < set_layout->binding_count; b++) {
         if (state.set[set].use_count[b] == 0)
            continue;

         struct anv_descriptor_set_binding_layout *binding =
               &layout->set[set].layout->binding[b];

         /* Do a fixed-point calculation to generate a score based on the
          * number of uses and the binding array size.  We shift by 7 instead
          * of 8 because we're going to use the top bit below to make
          * everything which does not support bindless super higher priority
          * than things which do.
          */
         uint16_t score = ((uint16_t)state.set[set].use_count[b] << 7) /
                          binding->array_size;

         /* If the descriptor type doesn't support bindless then put it at the
          * beginning so we guarantee it gets a slot.
          */
         if (!anv_descriptor_supports_bindless(pdevice, binding, true) ||
             !anv_descriptor_supports_bindless(pdevice, binding, false))
            score |= 1 << 15;

         infos[used_binding_count++] = (struct binding_info) {
            .set = set,
            .binding = b,
            .score = score,
         };
      }
   }

   /* Order the binding infos based on score with highest scores first.  If
    * scores are equal we then order by set and binding.
    */
   qsort(infos, used_binding_count, sizeof(struct binding_info),
         compare_binding_infos);

   for (unsigned i = 0; i < used_binding_count; i++) {
      unsigned set = infos[i].set, b = infos[i].binding;
      struct anv_descriptor_set_binding_layout *binding =
            &layout->set[set].layout->binding[b];

      const uint32_t array_size = binding->array_size;

      if (binding->data & ANV_DESCRIPTOR_SURFACE_STATE) {
         if (map->surface_count + array_size > MAX_BINDING_TABLE_SIZE ||
             anv_descriptor_requires_bindless(pdevice, binding, false)) {
            /* If this descriptor doesn't fit in the binding table or if it
             * requires bindless for some reason, flag it as bindless.
             */
            assert(anv_descriptor_supports_bindless(pdevice, binding, false));
            state.set[set].surface_offsets[b] = BINDLESS_OFFSET;
         } else {
            state.set[set].surface_offsets[b] = map->surface_count;
            struct anv_sampler **samplers = binding->immutable_samplers;
            for (unsigned i = 0; i < binding->array_size; i++) {
               uint8_t planes = samplers ? samplers[i]->n_planes : 1;
               for (uint8_t p = 0; p < planes; p++) {
                  map->surface_to_descriptor[map->surface_count++] =
                     (struct anv_pipeline_binding) {
                        .set = set,
                        .binding = b,
                        .index = i,
                        .plane = p,
                     };
               }
            }
         }
         assert(map->surface_count <= MAX_BINDING_TABLE_SIZE);
      }

      if (binding->data & ANV_DESCRIPTOR_SAMPLER_STATE) {
         if (map->sampler_count + array_size > MAX_SAMPLER_TABLE_SIZE ||
             anv_descriptor_requires_bindless(pdevice, binding, true)) {
            /* If this descriptor doesn't fit in the binding table or if it
             * requires bindless for some reason, flag it as bindless.
             */
            assert(anv_descriptor_supports_bindless(pdevice, binding, true));
            state.set[set].sampler_offsets[b] = BINDLESS_OFFSET;
         } else {
            state.set[set].sampler_offsets[b] = map->sampler_count;
            struct anv_sampler **samplers = binding->immutable_samplers;
            for (unsigned i = 0; i < binding->array_size; i++) {
               uint8_t planes = samplers ? samplers[i]->n_planes : 1;
               for (uint8_t p = 0; p < planes; p++) {
                  map->sampler_to_descriptor[map->sampler_count++] =
                     (struct anv_pipeline_binding) {
                        .set = set,
                        .binding = b,
                        .index = i,
                        .plane = p,
                     };
               }
            }
         }
      }
   }

   nir_foreach_variable(var, &shader->uniforms) {
      const struct glsl_type *glsl_type = glsl_without_array(var->type);

      if (!glsl_type_is_image(glsl_type))
         continue;

      enum glsl_sampler_dim dim = glsl_get_sampler_dim(glsl_type);

      const uint32_t set = var->data.descriptor_set;
      const uint32_t binding = var->data.binding;
      const uint32_t array_size =
         layout->set[set].layout->binding[binding].array_size;

      if (state.set[set].use_count[binding] == 0)
         continue;

      if (state.set[set].surface_offsets[binding] >= MAX_BINDING_TABLE_SIZE)
         continue;

      struct anv_pipeline_binding *pipe_binding =
         &map->surface_to_descriptor[state.set[set].surface_offsets[binding]];
      for (unsigned i = 0; i < array_size; i++) {
         assert(pipe_binding[i].set == set);
         assert(pipe_binding[i].binding == binding);
         assert(pipe_binding[i].index == i);

         if (dim == GLSL_SAMPLER_DIM_SUBPASS ||
             dim == GLSL_SAMPLER_DIM_SUBPASS_MS)
            pipe_binding[i].input_attachment_index = var->data.index + i;

         pipe_binding[i].write_only =
            (var->data.image.access & ACCESS_NON_READABLE) != 0;
      }
   }

   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      nir_builder_init(&state.builder, function->impl);
      nir_foreach_block(block, function->impl)
         apply_pipeline_layout_block(block, &state);
      nir_metadata_preserve(function->impl, nir_metadata_block_index |
                                            nir_metadata_dominance);
   }

   ralloc_free(mem_ctx);
}

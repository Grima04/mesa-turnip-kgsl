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
#include "util/mesa-sha1.h"
#include "util/set.h"

/* Sampler tables don't actually have a maximum size but we pick one just so
 * that we don't end up emitting too much state on-the-fly.
 */
#define MAX_SAMPLER_TABLE_SIZE 128
#define BINDLESS_OFFSET        255

struct apply_pipeline_layout_state {
   const struct anv_physical_device *pdevice;

   const struct anv_pipeline_layout *layout;
   bool add_bounds_checks;
   nir_address_format ssbo_addr_format;
   nir_address_format ubo_addr_format;

   /* Place to flag lowered instructions so we don't lower them twice */
   struct set *lowered_instrs;

   bool uses_constants;
   bool has_dynamic_buffers;
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

static bool
get_used_bindings(UNUSED nir_builder *_b, nir_instr *instr, void *_state)
{
   struct apply_pipeline_layout_state *state = _state;

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
      case nir_intrinsic_image_deref_atomic_imin:
      case nir_intrinsic_image_deref_atomic_umin:
      case nir_intrinsic_image_deref_atomic_imax:
      case nir_intrinsic_image_deref_atomic_umax:
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
      break;
   }

   return false;
}

static nir_intrinsic_instr *
find_descriptor_for_index_src(nir_src src,
                              struct apply_pipeline_layout_state *state)
{
   nir_intrinsic_instr *intrin = nir_src_as_intrinsic(src);

   while (intrin && intrin->intrinsic == nir_intrinsic_vulkan_resource_reindex)
      intrin = nir_src_as_intrinsic(intrin->src[0]);

   if (!intrin || intrin->intrinsic != nir_intrinsic_vulkan_resource_index)
      return NULL;

   return intrin;
}

static bool
descriptor_has_bti(nir_intrinsic_instr *intrin,
                   struct apply_pipeline_layout_state *state)
{
   assert(intrin->intrinsic == nir_intrinsic_vulkan_resource_index);

   uint32_t set = nir_intrinsic_desc_set(intrin);
   uint32_t binding = nir_intrinsic_binding(intrin);
   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   uint32_t surface_index;
   if (bind_layout->data & ANV_DESCRIPTOR_INLINE_UNIFORM)
      surface_index = state->set[set].desc_offset;
   else
      surface_index = state->set[set].surface_offsets[binding];

   /* Only lower to a BTI message if we have a valid binding table index. */
   return surface_index < MAX_BINDING_TABLE_SIZE;
}

static nir_intrinsic_instr *
nir_deref_find_descriptor(nir_deref_instr *deref,
                          struct apply_pipeline_layout_state *state)
{
   while (1) {
      /* Nothing we will use this on has a variable */
      assert(deref->deref_type != nir_deref_type_var);

      nir_deref_instr *parent = nir_src_as_deref(deref->parent);
      if (!parent)
         break;

      deref = parent;
   }
   assert(deref->deref_type == nir_deref_type_cast);

   nir_intrinsic_instr *intrin = nir_src_as_intrinsic(deref->parent);
   if (!intrin || intrin->intrinsic != nir_intrinsic_load_vulkan_descriptor)
      return false;

   return find_descriptor_for_index_src(intrin->src[0], state);
}

static nir_ssa_def *
build_binding_triple(nir_builder *b, nir_intrinsic_instr *intrin,
                     uint32_t *set, uint32_t *binding)
{
   if (intrin->intrinsic == nir_intrinsic_vulkan_resource_reindex) {
      nir_ssa_def *index =
         build_binding_triple(b, nir_src_as_intrinsic(intrin->src[0]),
                              set, binding);

      b->cursor = nir_before_instr(&intrin->instr);
      return nir_iadd(b, index, nir_ssa_for_src(b, intrin->src[1], 1));
   } else {
      assert(intrin->intrinsic == nir_intrinsic_vulkan_resource_index);

      *set = nir_intrinsic_desc_set(intrin);
      *binding = nir_intrinsic_binding(intrin);

      b->cursor = nir_before_instr(&intrin->instr);
      return nir_ssa_for_src(b, intrin->src[0], 1);
   }
}

static nir_ssa_def *
build_index_offset_for_res_reindex(nir_builder *b, nir_intrinsic_instr *intrin,
                                   struct apply_pipeline_layout_state *state)
{
   /* The recursion here is a bit weird because we build the chain of add
    * instructions at each reindex but we take the surface index and array
    * size from the final load_vulkan_resource_index in the chain.
    */
   uint32_t set = UINT32_MAX, binding = UINT32_MAX;
   nir_ssa_def *array_index = build_binding_triple(b, intrin, &set, &binding);

   assert(set < MAX_SETS);
   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   b->cursor = nir_before_instr(&intrin->instr);

   if (bind_layout->data & ANV_DESCRIPTOR_INLINE_UNIFORM) {
      assert(nir_src_as_uint(nir_src_for_ssa(array_index)) == 0);
      return nir_imm_ivec2(b, state->set[set].desc_offset,
                              bind_layout->descriptor_offset);
   } else {
      uint32_t surface_index = state->set[set].surface_offsets[binding];
      uint32_t array_size = bind_layout->array_size;

      if (nir_src_is_const(nir_src_for_ssa(array_index)) ||
          state->add_bounds_checks)
         array_index = nir_umin(b, array_index, nir_imm_int(b, array_size - 1));

      return nir_vec2(b, nir_iadd_imm(b, array_index, surface_index),
                         nir_imm_int(b, 0));
   }
}

static nir_ssa_def *
build_index_offset_for_deref(nir_builder *b, nir_deref_instr *deref,
                             struct apply_pipeline_layout_state *state)
{
   nir_deref_instr *parent = nir_deref_instr_parent(deref);
   if (parent) {
      nir_ssa_def *addr = build_index_offset_for_deref(b, parent, state);

      b->cursor = nir_before_instr(&deref->instr);
      return nir_explicit_io_address_from_deref(b, deref, addr,
                                                nir_address_format_32bit_index_offset);
   }

   nir_intrinsic_instr *load_desc = nir_src_as_intrinsic(deref->parent);
   assert(load_desc->intrinsic == nir_intrinsic_load_vulkan_descriptor);

   return build_index_offset_for_res_reindex(b,
      nir_src_as_intrinsic(load_desc->src[0]), state);
}

static bool
try_lower_direct_buffer_intrinsic(nir_builder *b,
                                  nir_intrinsic_instr *intrin, bool is_atomic,
                                  struct apply_pipeline_layout_state *state)
{
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   if (!nir_deref_mode_is_one_of(deref, nir_var_mem_ubo | nir_var_mem_ssbo))
      return false;

   nir_intrinsic_instr *desc = nir_deref_find_descriptor(deref, state);
   if (desc == NULL) {
      /* We should always be able to find the descriptor for UBO access. */
      assert(nir_deref_mode_is_one_of(deref, nir_var_mem_ssbo));
      return false;
   }

   if (nir_deref_mode_is(deref, nir_var_mem_ssbo)) {
      /* 64-bit atomics only support A64 messages so we can't lower them to
       * the index+offset model.
       */
      if (is_atomic && nir_dest_bit_size(intrin->dest) == 64)
         return false;

      /* Normal binding table-based messages can't handle non-uniform access
       * so we have to fall back to A64.
       */
      if (nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM)
         return false;

      if (!descriptor_has_bti(desc, state))
         return false;
   }

   nir_ssa_def *addr = build_index_offset_for_deref(b, deref, state);

   b->cursor = nir_before_instr(&intrin->instr);
   nir_lower_explicit_io_instr(b, intrin, addr,
                               nir_address_format_32bit_index_offset);
   return true;
}

static bool
lower_direct_buffer_instr(nir_builder *b, nir_instr *instr, void *_state)
{
   struct apply_pipeline_layout_state *state = _state;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_deref:
   case nir_intrinsic_store_deref:
      return try_lower_direct_buffer_intrinsic(b, intrin, false, state);

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
   case nir_intrinsic_deref_atomic_fmin:
   case nir_intrinsic_deref_atomic_fmax:
   case nir_intrinsic_deref_atomic_fcomp_swap:
      return try_lower_direct_buffer_intrinsic(b, intrin, true, state);

   case nir_intrinsic_get_ssbo_size: {
      /* The get_ssbo_size intrinsic always just takes a
       * index/reindex intrinsic.
       */
      nir_intrinsic_instr *desc =
         find_descriptor_for_index_src(intrin->src[0], state);
      if (desc == NULL || !descriptor_has_bti(desc, state))
         return false;

      nir_ssa_def *io = build_index_offset_for_res_reindex(b,
         nir_src_as_intrinsic(intrin->src[0]), state);

      b->cursor = nir_before_instr(&intrin->instr);

      nir_ssa_def *index = nir_channel(b, io, 0);
      nir_instr_rewrite_src(&intrin->instr, &intrin->src[0],
                            nir_src_for_ssa(index));
      _mesa_set_add(state->lowered_instrs, intrin);
      return true;
   }

   default:
      return false;
   }
}

static nir_address_format
desc_addr_format(VkDescriptorType desc_type,
                 struct apply_pipeline_layout_state *state)
{
   return (desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) ?
           state->ssbo_addr_format : state->ubo_addr_format;
}

static bool
lower_res_index_intrinsic(nir_builder *b, nir_intrinsic_instr *intrin,
                          struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_before_instr(&intrin->instr);

   uint32_t set = nir_intrinsic_desc_set(intrin);
   uint32_t binding = nir_intrinsic_binding(intrin);
   const VkDescriptorType desc_type = nir_intrinsic_desc_type(intrin);

   /* All UBO access should have been lowered before we get here */
   assert(desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
          desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);

   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   uint32_t surface_index = state->set[set].surface_offsets[binding];
   uint32_t array_size = bind_layout->array_size;

   nir_ssa_def *index;
   if (state->pdevice->has_a64_buffer_access) {
      /* We store the descriptor offset as 16.8.8 where the top 16 bits are
       * the offset into the descriptor set, the next 8 are the binding table
       * index of the descriptor buffer, and the bottom 8 bits are the offset
       * (in bytes) into the dynamic offset table.
       */
      assert(bind_layout->dynamic_offset_index < MAX_DYNAMIC_BUFFERS);
      uint32_t dynamic_offset_index = 0xff; /* No dynamic offset */
      if (bind_layout->dynamic_offset_index >= 0) {
         dynamic_offset_index =
            state->layout->set[set].dynamic_offset_start +
            bind_layout->dynamic_offset_index;
      }

      const uint32_t desc_offset =
         bind_layout->descriptor_offset << 16 |
         (uint32_t)state->set[set].desc_offset << 8 |
         dynamic_offset_index;

      if (state->add_bounds_checks) {
         assert(desc_addr_format(desc_type, state) ==
                nir_address_format_64bit_bounded_global);
         assert(intrin->dest.ssa.num_components == 4);
         assert(intrin->dest.ssa.bit_size == 32);
         index = nir_vec4(b, nir_imm_int(b, desc_offset),
                             nir_ssa_for_src(b, intrin->src[0], 1),
                             nir_imm_int(b, array_size - 1),
                             nir_ssa_undef(b, 1, 32));
      } else {
         assert(desc_addr_format(desc_type, state) ==
                nir_address_format_64bit_global);
         assert(intrin->dest.ssa.num_components == 1);
         assert(intrin->dest.ssa.bit_size == 64);
         index = nir_pack_64_2x32_split(b, nir_imm_int(b, desc_offset),
                                           nir_ssa_for_src(b, intrin->src[0], 1));
      }
   } else {
      assert(desc_addr_format(desc_type, state) ==
             nir_address_format_32bit_index_offset);
      assert(intrin->dest.ssa.num_components == 2);
      assert(intrin->dest.ssa.bit_size == 32);
      assert(array_size > 0 && array_size <= UINT16_MAX);
      assert(surface_index <= UINT16_MAX);
      uint32_t packed = ((array_size - 1) << 16) | surface_index;
      index = nir_vec2(b, nir_ssa_for_src(b, intrin->src[0], 1),
                          nir_imm_int(b, packed));
   }

   assert(intrin->dest.is_ssa);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, index);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_res_reindex_intrinsic(nir_builder *b, nir_intrinsic_instr *intrin,
                            struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_before_instr(&intrin->instr);

   const VkDescriptorType desc_type = nir_intrinsic_desc_type(intrin);

   /* For us, the resource indices are just indices into the binding table and
    * array elements are sequential.  A resource_reindex just turns into an
    * add of the two indices.
    */
   assert(intrin->src[0].is_ssa && intrin->src[1].is_ssa);
   nir_ssa_def *old_index = intrin->src[0].ssa;
   nir_ssa_def *offset = intrin->src[1].ssa;

   nir_ssa_def *new_index;
   switch (desc_addr_format(desc_type, state)) {
   case nir_address_format_64bit_bounded_global:
      /* See also lower_res_index_intrinsic() */
      assert(intrin->dest.ssa.num_components == 4);
      assert(intrin->dest.ssa.bit_size == 32);
      new_index = nir_vec4(b, nir_channel(b, old_index, 0),
                              nir_iadd(b, nir_channel(b, old_index, 1),
                                          offset),
                              nir_channel(b, old_index, 2),
                              nir_ssa_undef(b, 1, 32));
      break;

   case nir_address_format_64bit_global: {
      /* See also lower_res_index_intrinsic() */
      assert(intrin->dest.ssa.num_components == 1);
      assert(intrin->dest.ssa.bit_size == 64);
      nir_ssa_def *base = nir_unpack_64_2x32_split_x(b, old_index);
      nir_ssa_def *arr_idx = nir_unpack_64_2x32_split_y(b, old_index);
      new_index = nir_pack_64_2x32_split(b, base, nir_iadd(b, arr_idx, offset));
      break;
   }

   case nir_address_format_32bit_index_offset:
      assert(intrin->dest.ssa.num_components == 2);
      assert(intrin->dest.ssa.bit_size == 32);
      new_index = nir_vec2(b, nir_iadd(b, nir_channel(b, old_index, 0), offset),
                              nir_channel(b, old_index, 1));
      break;

   default:
      unreachable("Uhandled address format");
   }

   assert(intrin->dest.is_ssa);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, new_index);
   nir_instr_remove(&intrin->instr);

   return true;
}

static nir_ssa_def *
build_ssbo_descriptor_load(nir_builder *b, const VkDescriptorType desc_type,
                           nir_ssa_def *index,
                           struct apply_pipeline_layout_state *state)
{
   nir_ssa_def *desc_offset, *array_index;
   switch (desc_addr_format(desc_type, state)) {
   case nir_address_format_64bit_bounded_global:
      /* See also lower_res_index_intrinsic() */
      desc_offset = nir_channel(b, index, 0);
      array_index = nir_umin(b, nir_channel(b, index, 1),
                                nir_channel(b, index, 2));
      break;

   case nir_address_format_64bit_global:
      /* See also lower_res_index_intrinsic() */
      desc_offset = nir_unpack_64_2x32_split_x(b, index);
      array_index = nir_unpack_64_2x32_split_y(b, index);
      break;

   default:
      unreachable("Unhandled address format for SSBO");
   }

   /* The desc_offset is actually 16.8.8 */
   nir_ssa_def *desc_buffer_index =
      nir_extract_u8(b, desc_offset, nir_imm_int(b, 1));
   nir_ssa_def *desc_offset_base =
      nir_extract_u16(b, desc_offset, nir_imm_int(b, 1));

   /* Compute the actual descriptor offset */
   const unsigned descriptor_size =
      anv_descriptor_type_size(state->pdevice, desc_type);
   desc_offset = nir_iadd(b, desc_offset_base,
                             nir_imul_imm(b, array_index, descriptor_size));

   nir_ssa_def *desc_load =
      nir_load_ubo(b, 4, 32, desc_buffer_index, desc_offset,
                   .align_mul = 8,
                   .align_offset = 0,
                   .range_base = 0,
                   .range = ~0);

   return desc_load;
}

static bool
lower_load_vulkan_descriptor(nir_builder *b, nir_intrinsic_instr *intrin,
                             struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_before_instr(&intrin->instr);

   const VkDescriptorType desc_type = nir_intrinsic_desc_type(intrin);

   assert(intrin->dest.is_ssa);
   nir_foreach_use(src, &intrin->dest.ssa) {
      if (src->parent_instr->type != nir_instr_type_deref)
         continue;

      nir_deref_instr *cast = nir_instr_as_deref(src->parent_instr);
      assert(cast->deref_type == nir_deref_type_cast);
      switch (desc_type) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         cast->cast.align_mul = ANV_UBO_ALIGNMENT;
         cast->cast.align_offset = 0;
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         cast->cast.align_mul = ANV_SSBO_ALIGNMENT;
         cast->cast.align_offset = 0;
         break;

      default:
         break;
      }
   }

   assert(intrin->src[0].is_ssa);
   nir_ssa_def *index = intrin->src[0].ssa;

   nir_ssa_def *desc;
   if (state->pdevice->has_a64_buffer_access &&
       (desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
        desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) {
      desc = build_ssbo_descriptor_load(b, desc_type, index, state);

      nir_address_format addr_format = desc_addr_format(desc_type, state);
      if (addr_format == nir_address_format_64bit_global)
         desc = nir_pack_64_2x32(b, nir_channels(b, desc, 0x3));

      if (state->has_dynamic_buffers) {
         /* This shader has dynamic offsets and we have no way of knowing
          * (save from the dynamic offset base index) if this buffer has a
          * dynamic offset.
          */
         nir_ssa_def *desc_offset, *array_index;
         switch (addr_format) {
         case nir_address_format_64bit_bounded_global:
            /* See also lower_res_index_intrinsic() */
            desc_offset = nir_channel(b, index, 0);
            array_index = nir_umin(b, nir_channel(b, index, 1),
                                      nir_channel(b, index, 2));
            break;

         case nir_address_format_64bit_global:
            /* See also lower_res_index_intrinsic() */
            desc_offset = nir_unpack_64_2x32_split_x(b, index);
            array_index = nir_unpack_64_2x32_split_y(b, index);
            break;

         default:
            unreachable("Unhandled address format for SSBO");
         }

         nir_ssa_def *dyn_offset_base =
            nir_extract_u8(b, desc_offset, nir_imm_int(b, 0));
         nir_ssa_def *dyn_offset_idx =
            nir_iadd(b, dyn_offset_base, array_index);
         if (state->add_bounds_checks) {
            dyn_offset_idx = nir_umin(b, dyn_offset_idx,
                                         nir_imm_int(b, MAX_DYNAMIC_BUFFERS));
         }

         nir_ssa_def *dyn_load =
            nir_load_push_constant(b, 1, 32, nir_imul_imm(b, dyn_offset_idx, 4),
                                   .base = offsetof(struct anv_push_constants, dynamic_offsets),
                                   .range = MAX_DYNAMIC_BUFFERS * 4);

         nir_ssa_def *dynamic_offset =
            nir_bcsel(b, nir_ieq_imm(b, dyn_offset_base, 0xff),
                         nir_imm_int(b, 0), dyn_load);

         switch (addr_format) {
         case nir_address_format_64bit_bounded_global: {
            /* The dynamic offset gets added to the base pointer so that we
             * have a sliding window range.
             */
            nir_ssa_def *base_ptr =
               nir_pack_64_2x32(b, nir_channels(b, desc, 0x3));
            base_ptr = nir_iadd(b, base_ptr, nir_u2u64(b, dynamic_offset));
            desc = nir_vec4(b, nir_unpack_64_2x32_split_x(b, base_ptr),
                               nir_unpack_64_2x32_split_y(b, base_ptr),
                               nir_channel(b, desc, 2),
                               nir_channel(b, desc, 3));
            break;
         }

         case nir_address_format_64bit_global:
            desc = nir_iadd(b, desc, nir_u2u64(b, dynamic_offset));
            break;

         default:
            unreachable("Unhandled address format for SSBO");
         }
      }
   } else {
      nir_ssa_def *array_index = nir_channel(b, index, 0);
      nir_ssa_def *packed = nir_channel(b, index, 1);
      nir_ssa_def *array_max = nir_ushr_imm(b, packed, 16);
      nir_ssa_def *surface_index = nir_iand_imm(b, packed, 0xffff);

      if (state->add_bounds_checks)
         array_index = nir_umin(b, array_index, array_max);

      desc = nir_vec2(b, nir_iadd(b, surface_index, array_index),
                         nir_imm_int(b, 0));
   }

   assert(intrin->dest.is_ssa);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, desc);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_get_ssbo_size(nir_builder *b, nir_intrinsic_instr *intrin,
                    struct apply_pipeline_layout_state *state)
{
   if (_mesa_set_search(state->lowered_instrs, intrin))
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   const VkDescriptorType desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

   assert(intrin->src[0].is_ssa);
   nir_ssa_def *index = intrin->src[0].ssa;

   if (state->pdevice->has_a64_buffer_access) {
      nir_ssa_def *desc = build_ssbo_descriptor_load(b, desc_type, index, state);
      nir_ssa_def *size = nir_channel(b, desc, 2);
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, size);
      nir_instr_remove(&intrin->instr);
   } else {
      /* We're following the nir_address_format_32bit_index_offset model so
       * the binding table index is the first component of the address.  The
       * back-end wants a scalar binding table index source.
       */
      nir_instr_rewrite_src(&intrin->instr, &intrin->src[0],
                            nir_src_for_ssa(nir_channel(b, index, 0)));
   }

   return true;
}

static nir_ssa_def *
build_descriptor_load(nir_builder *b, nir_deref_instr *deref, unsigned offset,
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

   nir_ssa_def *desc_load =
      nir_load_ubo(b, num_components, bit_size, desc_buffer_index, desc_offset,
                   .align_mul = 8,
                   .align_offset =  offset % 8,
                   .range_base = 0,
                   .range = ~0);

   return desc_load;
}

static bool
lower_image_intrinsic(nir_builder *b, nir_intrinsic_instr *intrin,
                      struct apply_pipeline_layout_state *state)
{
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   unsigned binding_offset = state->set[set].surface_offsets[binding];

   b->cursor = nir_before_instr(&intrin->instr);

   ASSERTED const bool use_bindless = state->pdevice->has_bindless_images;

   if (intrin->intrinsic == nir_intrinsic_image_deref_load_param_intel) {
      b->cursor = nir_instr_remove(&intrin->instr);

      assert(!use_bindless); /* Otherwise our offsets would be wrong */
      const unsigned param = nir_intrinsic_base(intrin);

      nir_ssa_def *desc =
         build_descriptor_load(b, deref, param * 16,
                               intrin->dest.ssa.num_components,
                               intrin->dest.ssa.bit_size, state);

      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, desc);
   } else if (binding_offset > MAX_BINDING_TABLE_SIZE) {
      const bool write_only =
         (var->data.access & ACCESS_NON_READABLE) != 0;
      nir_ssa_def *desc =
         build_descriptor_load(b, deref, 0, 2, 32, state);
      nir_ssa_def *handle = nir_channel(b, desc, write_only ? 1 : 0);
      nir_rewrite_image_intrinsic(intrin, handle, true);
   } else {
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

   return true;
}

static bool
lower_load_constant(nir_builder *b, nir_intrinsic_instr *intrin,
                    struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_instr_remove(&intrin->instr);

   /* Any constant-offset load_constant instructions should have been removed
    * by constant folding.
    */
   assert(!nir_src_is_const(intrin->src[0]));
   nir_ssa_def *offset = nir_iadd_imm(b, nir_ssa_for_src(b, intrin->src[0], 1),
                                      nir_intrinsic_base(intrin));

   nir_ssa_def *data;
   if (state->pdevice->use_softpin) {
      unsigned load_size = intrin->dest.ssa.num_components *
                           intrin->dest.ssa.bit_size / 8;
      unsigned load_align = intrin->dest.ssa.bit_size / 8;

      assert(load_size < b->shader->constant_data_size);
      unsigned max_offset = b->shader->constant_data_size - load_size;
      offset = nir_umin(b, offset, nir_imm_int(b, max_offset));

      nir_ssa_def *const_data_base_addr = nir_pack_64_2x32_split(b,
         nir_load_reloc_const_intel(b, ANV_SHADER_RELOC_CONST_DATA_ADDR_LOW),
         nir_load_reloc_const_intel(b, ANV_SHADER_RELOC_CONST_DATA_ADDR_HIGH));

      data = nir_load_global_constant(b, nir_iadd(b, const_data_base_addr,
                                                     nir_u2u64(b, offset)),
                                      load_align,
                                      intrin->dest.ssa.num_components,
                                      intrin->dest.ssa.bit_size);
   } else {
      nir_ssa_def *index = nir_imm_int(b, state->constants_offset);

      data = nir_load_ubo(b, intrin->num_components, intrin->dest.ssa.bit_size,
                          index, offset,
                          .align_mul = intrin->dest.ssa.bit_size / 8,
                          .align_offset =  0,
                          .range_base = nir_intrinsic_base(intrin),
                          .range = nir_intrinsic_range(intrin));
   }

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, data);

   return true;
}

static void
lower_tex_deref(nir_builder *b, nir_tex_instr *tex,
                nir_tex_src_type deref_src_type,
                unsigned *base_index, unsigned plane,
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

   unsigned binding_offset;
   if (deref_src_type == nir_tex_src_texture_deref) {
      binding_offset = state->set[set].surface_offsets[binding];
   } else {
      assert(deref_src_type == nir_tex_src_sampler_deref);
      binding_offset = state->set[set].sampler_offsets[binding];
   }

   nir_tex_src_type offset_src_type;
   nir_ssa_def *index = NULL;
   if (binding_offset > MAX_BINDING_TABLE_SIZE) {
      const unsigned plane_offset =
         plane * sizeof(struct anv_sampled_image_descriptor);

      nir_ssa_def *desc =
         build_descriptor_load(b, deref, plane_offset, 2, 32, state);

      if (deref_src_type == nir_tex_src_texture_deref) {
         offset_src_type = nir_tex_src_texture_handle;
         index = nir_channel(b, desc, 0);
      } else {
         assert(deref_src_type == nir_tex_src_sampler_deref);
         offset_src_type = nir_tex_src_sampler_handle;
         index = nir_channel(b, desc, 1);
      }
   } else {
      if (deref_src_type == nir_tex_src_texture_deref) {
         offset_src_type = nir_tex_src_texture_offset;
      } else {
         assert(deref_src_type == nir_tex_src_sampler_deref);
         offset_src_type = nir_tex_src_sampler_offset;
      }

      *base_index = binding_offset + plane;

      if (deref->deref_type != nir_deref_type_var) {
         assert(deref->deref_type == nir_deref_type_array);

         if (nir_src_is_const(deref->arr.index)) {
            unsigned arr_index = MIN2(nir_src_as_uint(deref->arr.index), array_size - 1);
            struct anv_sampler **immutable_samplers =
               state->layout->set[set].layout->binding[binding].immutable_samplers;
            if (immutable_samplers) {
               /* Array of YCbCr samplers are tightly packed in the binding
                * tables, compute the offset of an element in the array by
                * adding the number of planes of all preceding elements.
                */
               unsigned desc_arr_index = 0;
               for (int i = 0; i < arr_index; i++)
                  desc_arr_index += immutable_samplers[i]->n_planes;
               *base_index += desc_arr_index;
            } else {
               *base_index += arr_index;
            }
         } else {
            /* From VK_KHR_sampler_ycbcr_conversion:
             *
             * If sampler Y’CBCR conversion is enabled, the combined image
             * sampler must be indexed only by constant integral expressions
             * when aggregated into arrays in shader code, irrespective of
             * the shaderSampledImageArrayDynamicIndexing feature.
             */
            assert(nir_tex_instr_src_index(tex, nir_tex_src_plane) == -1);

            index = nir_ssa_for_src(b, deref->arr.index, 1);

            if (state->add_bounds_checks)
               index = nir_umin(b, index, nir_imm_int(b, array_size - 1));
         }
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

static nir_ssa_def *
build_def_array_select(nir_builder *b, nir_ssa_def **srcs, nir_ssa_def *idx,
                       unsigned start, unsigned end)
{
   if (start == end - 1) {
      return srcs[start];
   } else {
      unsigned mid = start + (end - start) / 2;
      return nir_bcsel(b, nir_ilt(b, idx, nir_imm_int(b, mid)),
                       build_def_array_select(b, srcs, idx, start, mid),
                       build_def_array_select(b, srcs, idx, mid, end));
   }
}

static void
lower_gen7_tex_swizzle(nir_builder *b, nir_tex_instr *tex, unsigned plane,
                       struct apply_pipeline_layout_state *state)
{
   assert(state->pdevice->info.gen == 7 && !state->pdevice->info.is_haswell);
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF ||
       nir_tex_instr_is_query(tex) ||
       tex->op == nir_texop_tg4 || /* We can't swizzle TG4 */
       (tex->is_shadow && tex->is_new_style_shadow))
      return;

   int deref_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   assert(deref_src_idx >= 0);

   nir_deref_instr *deref = nir_src_as_deref(tex->src[deref_src_idx].src);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   if ((bind_layout->data & ANV_DESCRIPTOR_TEXTURE_SWIZZLE) == 0)
      return;

   b->cursor = nir_before_instr(&tex->instr);

   const unsigned plane_offset =
      plane * sizeof(struct anv_texture_swizzle_descriptor);
   nir_ssa_def *swiz =
      build_descriptor_load(b, deref, plane_offset, 1, 32, state);

   b->cursor = nir_after_instr(&tex->instr);

   assert(tex->dest.ssa.bit_size == 32);
   assert(tex->dest.ssa.num_components == 4);

   /* Initializing to undef is ok; nir_opt_undef will clean it up. */
   nir_ssa_def *undef = nir_ssa_undef(b, 1, 32);
   nir_ssa_def *comps[8];
   for (unsigned i = 0; i < ARRAY_SIZE(comps); i++)
      comps[i] = undef;

   comps[ISL_CHANNEL_SELECT_ZERO] = nir_imm_int(b, 0);
   if (nir_alu_type_get_base_type(tex->dest_type) == nir_type_float)
      comps[ISL_CHANNEL_SELECT_ONE] = nir_imm_float(b, 1);
   else
      comps[ISL_CHANNEL_SELECT_ONE] = nir_imm_int(b, 1);
   comps[ISL_CHANNEL_SELECT_RED] = nir_channel(b, &tex->dest.ssa, 0);
   comps[ISL_CHANNEL_SELECT_GREEN] = nir_channel(b, &tex->dest.ssa, 1);
   comps[ISL_CHANNEL_SELECT_BLUE] = nir_channel(b, &tex->dest.ssa, 2);
   comps[ISL_CHANNEL_SELECT_ALPHA] = nir_channel(b, &tex->dest.ssa, 3);

   nir_ssa_def *swiz_comps[4];
   for (unsigned i = 0; i < 4; i++) {
      nir_ssa_def *comp_swiz = nir_extract_u8(b, swiz, nir_imm_int(b, i));
      swiz_comps[i] = build_def_array_select(b, comps, comp_swiz, 0, 8);
   }
   nir_ssa_def *swiz_tex_res = nir_vec(b, swiz_comps, 4);

   /* Rewrite uses before we insert so we don't rewrite this use */
   nir_ssa_def_rewrite_uses_after(&tex->dest.ssa,
                                  swiz_tex_res,
                                  swiz_tex_res->parent_instr);
}

static bool
lower_tex(nir_builder *b, nir_tex_instr *tex,
          struct apply_pipeline_layout_state *state)
{
   unsigned plane = tex_instr_get_and_remove_plane_src(tex);

   /* On Ivy Bridge and Bay Trail, we have to swizzle in the shader.  Do this
    * before we lower the derefs away so we can still find the descriptor.
    */
   if (state->pdevice->info.gen == 7 && !state->pdevice->info.is_haswell)
      lower_gen7_tex_swizzle(b, tex, plane, state);

   b->cursor = nir_before_instr(&tex->instr);

   lower_tex_deref(b, tex, nir_tex_src_texture_deref,
                   &tex->texture_index, plane, state);

   lower_tex_deref(b, tex, nir_tex_src_sampler_deref,
                   &tex->sampler_index, plane, state);

   return true;
}

static bool
apply_pipeline_layout(nir_builder *b, nir_instr *instr, void *_state)
{
   struct apply_pipeline_layout_state *state = _state;

   switch (instr->type) {
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_vulkan_resource_index:
         return lower_res_index_intrinsic(b, intrin, state);
      case nir_intrinsic_vulkan_resource_reindex:
         return lower_res_reindex_intrinsic(b, intrin, state);
      case nir_intrinsic_load_vulkan_descriptor:
         return lower_load_vulkan_descriptor(b, intrin, state);
      case nir_intrinsic_get_ssbo_size:
         return lower_get_ssbo_size(b, intrin, state);
      case nir_intrinsic_image_deref_load:
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
      case nir_intrinsic_image_deref_size:
      case nir_intrinsic_image_deref_samples:
      case nir_intrinsic_image_deref_load_param_intel:
      case nir_intrinsic_image_deref_load_raw_intel:
      case nir_intrinsic_image_deref_store_raw_intel:
         return lower_image_intrinsic(b, intrin, state);
      case nir_intrinsic_load_constant:
         return lower_load_constant(b, intrin, state);
      default:
         return false;
      }
      break;
   }
   case nir_instr_type_tex:
      return lower_tex(b, nir_instr_as_tex(instr), state);
   default:
      return false;
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
                              const struct anv_pipeline_layout *layout,
                              nir_shader *shader,
                              struct anv_pipeline_bind_map *map)
{
   void *mem_ctx = ralloc_context(NULL);

   struct apply_pipeline_layout_state state = {
      .pdevice = pdevice,
      .layout = layout,
      .add_bounds_checks = robust_buffer_access,
      .ssbo_addr_format = anv_nir_ssbo_addr_format(pdevice, robust_buffer_access),
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .lowered_instrs = _mesa_pointer_set_create(mem_ctx),
   };

   for (unsigned s = 0; s < layout->num_sets; s++) {
      const unsigned count = layout->set[s].layout->binding_count;
      state.set[s].use_count = rzalloc_array(mem_ctx, uint8_t, count);
      state.set[s].surface_offsets = rzalloc_array(mem_ctx, uint8_t, count);
      state.set[s].sampler_offsets = rzalloc_array(mem_ctx, uint8_t, count);
   }

   nir_shader_instructions_pass(shader, get_used_bindings,
                                nir_metadata_all, &state);

   for (unsigned s = 0; s < layout->num_sets; s++) {
      if (state.set[s].desc_buffer_used) {
         map->surface_to_descriptor[map->surface_count] =
            (struct anv_pipeline_binding) {
               .set = ANV_DESCRIPTOR_SET_DESCRIPTORS,
               .index = s,
            };
         state.set[s].desc_offset = map->surface_count;
         map->surface_count++;
      }
   }

   if (state.uses_constants && !pdevice->use_softpin) {
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
      const struct anv_descriptor_set_layout *set_layout = layout->set[set].layout;
      for (unsigned b = 0; b < set_layout->binding_count; b++) {
         if (state.set[set].use_count[b] == 0)
            continue;

         const struct anv_descriptor_set_binding_layout *binding =
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
      const struct anv_descriptor_set_binding_layout *binding =
            &layout->set[set].layout->binding[b];

      const uint32_t array_size = binding->array_size;

      if (binding->dynamic_offset_index >= 0)
         state.has_dynamic_buffers = true;

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
            if (binding->dynamic_offset_index < 0) {
               struct anv_sampler **samplers = binding->immutable_samplers;
               for (unsigned i = 0; i < binding->array_size; i++) {
                  uint8_t planes = samplers ? samplers[i]->n_planes : 1;
                  for (uint8_t p = 0; p < planes; p++) {
                     map->surface_to_descriptor[map->surface_count++] =
                        (struct anv_pipeline_binding) {
                           .set = set,
                           .index = binding->descriptor_index + i,
                           .plane = p,
                        };
                  }
               }
            } else {
               for (unsigned i = 0; i < binding->array_size; i++) {
                  map->surface_to_descriptor[map->surface_count++] =
                     (struct anv_pipeline_binding) {
                        .set = set,
                        .index = binding->descriptor_index + i,
                        .dynamic_offset_index =
                           layout->set[set].dynamic_offset_start +
                           binding->dynamic_offset_index + i,
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
             *
             * We also make large sampler arrays bindless because we can avoid
             * using indirect sends thanks to bindless samplers being packed
             * less tightly than the sampler table.
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
                        .index = binding->descriptor_index + i,
                        .plane = p,
                     };
               }
            }
         }
      }
   }

   nir_foreach_uniform_variable(var, shader) {
      const struct glsl_type *glsl_type = glsl_without_array(var->type);

      if (!glsl_type_is_image(glsl_type))
         continue;

      enum glsl_sampler_dim dim = glsl_get_sampler_dim(glsl_type);

      const uint32_t set = var->data.descriptor_set;
      const uint32_t binding = var->data.binding;
      const struct anv_descriptor_set_binding_layout *bind_layout =
            &layout->set[set].layout->binding[binding];
      const uint32_t array_size = bind_layout->array_size;

      if (state.set[set].use_count[binding] == 0)
         continue;

      if (state.set[set].surface_offsets[binding] >= MAX_BINDING_TABLE_SIZE)
         continue;

      struct anv_pipeline_binding *pipe_binding =
         &map->surface_to_descriptor[state.set[set].surface_offsets[binding]];
      for (unsigned i = 0; i < array_size; i++) {
         assert(pipe_binding[i].set == set);
         assert(pipe_binding[i].index == bind_layout->descriptor_index + i);

         if (dim == GLSL_SAMPLER_DIM_SUBPASS ||
             dim == GLSL_SAMPLER_DIM_SUBPASS_MS)
            pipe_binding[i].input_attachment_index = var->data.index + i;

         /* NOTE: This is a uint8_t so we really do need to != 0 here */
         pipe_binding[i].write_only =
            (var->data.access & ACCESS_NON_READABLE) != 0;
      }
   }

   /* Before we do the normal lowering, we look for any SSBO operations
    * that we can lower to the BTI model and lower them up-front.  The BTI
    * model can perform better than the A64 model for a couple reasons:
    *
    *  1. 48-bit address calculations are potentially expensive and using
    *     the BTI model lets us simply compute 32-bit offsets and the
    *     hardware adds the 64-bit surface base address.
    *
    *  2. The BTI messages, because they use surface states, do bounds
    *     checking for us.  With the A64 model, we have to do our own
    *     bounds checking and this means wider pointers and extra
    *     calculations and branching in the shader.
    *
    * The solution to both of these is to convert things to the BTI model
    * opportunistically.  The reason why we need to do this as a pre-pass
    * is for two reasons:
    *
    *  1. The BTI model requires nir_address_format_32bit_index_offset
    *     pointers which are not the same type as the pointers needed for
    *     the A64 model.  Because all our derefs are set up for the A64
    *     model (in case we have variable pointers), we have to crawl all
    *     the way back to the vulkan_resource_index intrinsic and build a
    *     completely fresh index+offset calculation.
    *
    *  2. Because the variable-pointers-capable lowering that we do as part
    *     of apply_pipeline_layout_block is destructive (It really has to
    *     be to handle variable pointers properly), we've lost the deref
    *     information by the time we get to the load/store/atomic
    *     intrinsics in that pass.
    */
   nir_shader_instructions_pass(shader, lower_direct_buffer_instr,
                                nir_metadata_block_index |
                                nir_metadata_dominance,
                                &state);

   /* We just got rid of all the direct access.  Delete it so it's not in the
    * way when we do our indirect lowering.
    */
   nir_opt_dce(shader);

   nir_shader_instructions_pass(shader, apply_pipeline_layout,
                                nir_metadata_block_index |
                                nir_metadata_dominance,
                                &state);

   ralloc_free(mem_ctx);

   /* Now that we're done computing the surface and sampler portions of the
    * bind map, hash them.  This lets us quickly determine if the actual
    * mapping has changed and not just a no-op pipeline change.
    */
   _mesa_sha1_compute(map->surface_to_descriptor,
                      map->surface_count * sizeof(struct anv_pipeline_binding),
                      map->surface_sha1);
   _mesa_sha1_compute(map->sampler_to_descriptor,
                      map->sampler_count * sizeof(struct anv_pipeline_binding),
                      map->sampler_sha1);
}

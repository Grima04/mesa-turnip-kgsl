/*
 * Copyright © 2019 Raspberry Pi
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

#include "vk_util.h"

#include "v3dv_debug.h"
#include "v3dv_private.h"

#include "vk_format_info.h"

#include "common/v3d_debug.h"

#include "compiler/nir/nir_builder.h"

#include "vulkan/util/vk_format.h"

#include "broadcom/cle/v3dx_pack.h"

VkResult
v3dv_CreateShaderModule(VkDevice _device,
                        const VkShaderModuleCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkShaderModule *pShaderModule)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_shader_module *module;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   module = vk_alloc2(&device->alloc, pAllocator,
                       sizeof(*module) + pCreateInfo->codeSize, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (module == NULL)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   module->size = pCreateInfo->codeSize;
   memcpy(module->data, pCreateInfo->pCode, module->size);

   _mesa_sha1_compute(module->data, module->size, module->sha1);

   *pShaderModule = v3dv_shader_module_to_handle(module);

   return VK_SUCCESS;
}

void
v3dv_DestroyShaderModule(VkDevice _device,
                         VkShaderModule _module,
                         const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_shader_module, module, _module);

   if (!module)
      return;

   vk_free2(&device->alloc, pAllocator, module);
}

static void
destroy_pipeline_stage(struct v3dv_device *device,
                       struct v3dv_pipeline_stage *p_stage,
                       const VkAllocationCallbacks *pAllocator)
{
   v3dv_bo_free(device, p_stage->assembly_bo);

   vk_free2(&device->alloc, pAllocator, p_stage);
}

void
v3dv_DestroyPipeline(VkDevice _device,
                     VkPipeline _pipeline,
                     const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_pipeline, pipeline, _pipeline);

   if (!pipeline)
      return;

   /* FIXME: we can't just use a loop over mesa stage due the bin, would be
    * good to find an alternative.
    */
   destroy_pipeline_stage(device, pipeline->vs, pAllocator);
   destroy_pipeline_stage(device, pipeline->vs_bin, pAllocator);
   destroy_pipeline_stage(device, pipeline->fs, pAllocator);

   if (pipeline->default_attribute_values) {
      v3dv_bo_free(device, pipeline->default_attribute_values);
      pipeline->default_attribute_values = NULL;
   }

   vk_free2(&device->alloc, pAllocator, pipeline);
}

static const struct spirv_to_nir_options default_spirv_options =  {
   .caps = { false },
   .ubo_addr_format = nir_address_format_32bit_index_offset,
   .ssbo_addr_format = nir_address_format_32bit_index_offset,
   .phys_ssbo_addr_format = nir_address_format_64bit_global,
   .push_const_addr_format = nir_address_format_logical,
   .shared_addr_format = nir_address_format_32bit_offset,
   .frag_coord_is_sysval = false,
};

const nir_shader_compiler_options v3dv_nir_options = {
   .lower_all_io_to_temps = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_bitfield_insert_to_shifts = true,
   .lower_bitfield_extract_to_shifts = true,
   .lower_bitfield_reverse = true,
   .lower_bit_count = true,
   .lower_cs_local_id_from_index = true,
   .lower_ffract = true,
   .lower_fmod = true,
   .lower_pack_unorm_2x16 = true,
   .lower_pack_snorm_2x16 = true,
   .lower_pack_unorm_4x8 = true,
   .lower_pack_snorm_4x8 = true,
   .lower_unpack_unorm_4x8 = true,
   .lower_unpack_snorm_4x8 = true,
   .lower_pack_half_2x16 = true,
   .lower_unpack_half_2x16 = true,
   /* FIXME: see if we can avoid the uadd_carry and usub_borrow lowering and
    * get the tests to pass since it might produce slightly better code.
    */
   .lower_uadd_carry = true,
   .lower_usub_borrow = true,
   /* FIXME: check if we can use multop + umul24 to implement mul2x32_64
    * without lowering.
    */
   .lower_mul_2x32_64 = true,
   .lower_fdiv = true,
   .lower_find_lsb = true,
   .lower_ffma16 = true,
   .lower_ffma32 = true,
   .lower_ffma64 = true,
   .lower_flrp32 = true,
   .lower_fpow = true,
   .lower_fsat = true,
   .lower_fsqrt = true,
   .lower_ifind_msb = true,
   .lower_isign = true,
   .lower_ldexp = true,
   .lower_mul_high = true,
   .lower_wpos_pntc = true,
   .lower_rotate = true,
   .lower_to_scalar = true,
   .vertex_id_zero_based = false, /* FIXME: to set this to true, the intrinsic
                                   * needs to be supported */
};

#define OPT(pass, ...) ({                                  \
   bool this_progress = false;                             \
   NIR_PASS(this_progress, nir, pass, ##__VA_ARGS__);      \
   if (this_progress)                                      \
      progress = true;                                     \
   this_progress;                                          \
})

static void
nir_optimize(nir_shader *nir,
             struct v3dv_pipeline_stage *stage,
             bool allow_copies)
{
   bool progress;

   do {
      progress = false;
      OPT(nir_split_array_vars, nir_var_function_temp);
      OPT(nir_shrink_vec_array_vars, nir_var_function_temp);
      OPT(nir_opt_deref);
      OPT(nir_lower_vars_to_ssa);
      if (allow_copies) {
         /* Only run this pass in the first call to nir_optimize.  Later calls
          * assume that we've lowered away any copy_deref instructions and we
          * don't want to introduce any more.
          */
         OPT(nir_opt_find_array_copies);
      }
      OPT(nir_opt_copy_prop_vars);
      OPT(nir_opt_dead_write_vars);
      OPT(nir_opt_combine_stores, nir_var_all);

      OPT(nir_lower_alu_to_scalar, NULL, NULL);

      OPT(nir_copy_prop);
      OPT(nir_lower_phis_to_scalar);

      OPT(nir_copy_prop);
      OPT(nir_opt_dce);
      OPT(nir_opt_cse);
      OPT(nir_opt_combine_stores, nir_var_all);

      /* Passing 0 to the peephole select pass causes it to convert
       * if-statements that contain only move instructions in the branches
       * regardless of the count.
       *
       * Passing 1 to the peephole select pass causes it to convert
       * if-statements that contain at most a single ALU instruction (total)
       * in both branches.
       */
      OPT(nir_opt_peephole_select, 0, false, false);
      OPT(nir_opt_peephole_select, 8, false, true);

      OPT(nir_opt_intrinsics);
      OPT(nir_opt_idiv_const, 32);
      OPT(nir_opt_algebraic);
      OPT(nir_opt_constant_folding);

      OPT(nir_opt_dead_cf);

      OPT(nir_opt_if, false);
      OPT(nir_opt_conditional_discard);

      OPT(nir_opt_remove_phis);
      OPT(nir_opt_undef);
      OPT(nir_lower_pack);
   } while (progress);

   OPT(nir_remove_dead_variables, nir_var_function_temp, NULL);
}

static void
preprocess_nir(nir_shader *nir,
               struct v3dv_pipeline_stage *stage)
{
   /* Make sure we lower variable initializers on output variables so that
    * nir_remove_dead_variables below sees the corresponding stores
    */
   NIR_PASS_V(nir, nir_lower_variable_initializers, nir_var_shader_out);

   /* Now that we've deleted all but the main function, we can go ahead and
    * lower the rest of the variable initializers.
    */
   NIR_PASS_V(nir, nir_lower_variable_initializers, ~0);

   /* Split member structs.  We do this before lower_io_to_temporaries so that
    * it doesn't lower system values to temporaries by accident.
    */
   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_split_per_member_structs);

   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      NIR_PASS_V(nir, nir_lower_io_to_vector, nir_var_shader_out);
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS_V(nir, nir_lower_input_attachments,
                 &(nir_input_attachment_options) {
                    .use_fragcoord_sysval = false,
                       });
   }

   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_in |
              nir_var_shader_out | nir_var_system_value | nir_var_mem_shared,
              NULL);

   NIR_PASS_V(nir, nir_propagate_invariant);
   NIR_PASS_V(nir, nir_lower_io_to_temporaries,
              nir_shader_get_entrypoint(nir), true, false);

   NIR_PASS_V(nir, nir_lower_system_values);
   NIR_PASS_V(nir, nir_lower_clip_cull_distance_arrays);

   NIR_PASS_V(nir, nir_lower_alu_to_scalar, NULL, NULL);

   NIR_PASS_V(nir, nir_normalize_cubemap_coords);

   NIR_PASS_V(nir, nir_lower_global_vars_to_local);

   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_split_struct_vars, nir_var_function_temp);

   nir_optimize(nir, stage, true);

   NIR_PASS_V(nir, nir_lower_load_const_to_scalar);

   /* Lower a bunch of stuff */
   NIR_PASS_V(nir, nir_lower_var_copies);

   NIR_PASS_V(nir, nir_lower_indirect_derefs, nir_var_shader_in |
              nir_var_shader_out |
              nir_var_function_temp, UINT32_MAX);

   NIR_PASS_V(nir, nir_lower_array_deref_of_vec,
              nir_var_mem_ubo | nir_var_mem_ssbo,
              nir_lower_direct_array_deref_of_vec_load);

   /* Get rid of split copies */
   nir_optimize(nir, stage, false);
}

static nir_shader *
shader_module_compile_to_nir(struct v3dv_device *device,
                             struct v3dv_pipeline_stage *stage)
{
   nir_shader *nir;
   const nir_shader_compiler_options *nir_options = &v3dv_nir_options;

   uint32_t *spirv = (uint32_t *) stage->module->data;
   assert(stage->module->size % 4 == 0);

   if (V3D_DEBUG & V3D_DEBUG_DUMP_SPIRV)
      v3dv_print_spirv(stage->module->data, stage->module->size, stderr);

   uint32_t num_spec_entries = 0;
   struct nir_spirv_specialization *spec_entries = NULL;

   const struct spirv_to_nir_options spirv_options = default_spirv_options;
   nir = spirv_to_nir(spirv, stage->module->size / 4,
                      spec_entries, num_spec_entries,
                      stage->stage, stage->entrypoint,
                      &spirv_options, nir_options);
   assert(nir->info.stage == stage->stage);
   nir_validate_shader(nir, "after spirv_to_nir");

   if (V3D_DEBUG & (V3D_DEBUG_NIR |
                    v3d_debug_flag_for_shader_stage(stage->stage))) {
      fprintf(stderr, "Initial form: %s prog %d NIR:\n",
              gl_shader_stage_name(stage->stage),
              stage->program_id);
      nir_print_shader(nir, stderr);
      fprintf(stderr, "\n");
   }

   free(spec_entries);

   /* We have to lower away local variable initializers right before we
    * inline functions.  That way they get properly initialized at the top
    * of the function and not at the top of its caller.
    */
   NIR_PASS_V(nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS_V(nir, nir_lower_returns);
   NIR_PASS_V(nir, nir_inline_functions);
   NIR_PASS_V(nir, nir_opt_deref);

   /* Pick off the single entrypoint that we want */
   foreach_list_typed_safe(nir_function, func, node, &nir->functions) {
      if (func->is_entrypoint)
         func->name = ralloc_strdup(func, "main");
      else
         exec_node_remove(&func->node);
   }
   assert(exec_list_length(&nir->functions) == 1);

   /* Vulkan uses the separate-shader linking model */
   nir->info.separate_shader = true;

   preprocess_nir(nir, stage);

   return nir;
}

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static unsigned
descriptor_map_add(struct v3dv_descriptor_map *map,
                   int set,
                   int binding,
                   int array_index,
                   int array_size)
{
   assert(array_index < array_size);

   unsigned index = 0;
   for (unsigned i = 0; i < map->num_desc; i++) {
      if (set == map->set[i] &&
          binding == map->binding[i] &&
          array_index == map->array_index[i]) {
         assert(array_size == map->array_size[i]);
         return index;
      }
      index++;
   }

   assert(index == map->num_desc);

   map->set[map->num_desc] = set;
   map->binding[map->num_desc] = binding;
   map->array_index[map->num_desc] = array_index;
   map->array_size[map->num_desc] = array_size;
   map->num_desc++;

   return index;
}


static void
lower_load_push_constant(nir_builder *b, nir_intrinsic_instr *instr,
                         struct v3dv_pipeline *pipeline)
{
   assert(instr->intrinsic == nir_intrinsic_load_push_constant);

   /* FIXME: next assert it not something that should happen in general, just
    * to catch any test example under that case and deal with it
    */
   assert(nir_intrinsic_base(instr) == 0);

   instr->intrinsic = nir_intrinsic_load_uniform;
}

/* Gathers info from the intrinsic (set and binding) and then lowers it so it
 * could be used by the v3d_compiler */
static void
lower_vulkan_resource_index(nir_builder *b,
                            nir_intrinsic_instr *instr,
                            struct v3dv_pipeline *pipeline,
                            const struct v3dv_pipeline_layout *layout)
{
   assert(instr->intrinsic == nir_intrinsic_vulkan_resource_index);

   nir_const_value *const_val = nir_src_as_const_value(instr->src[0]);

   unsigned set = nir_intrinsic_desc_set(instr);
   unsigned binding = nir_intrinsic_binding(instr);
   struct v3dv_descriptor_set_layout *set_layout = layout->set[set].layout;
   struct v3dv_descriptor_set_binding_layout *binding_layout =
      &set_layout->binding[binding];
   unsigned index = 0;

   switch (nir_intrinsic_desc_type(instr)) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
      struct v3dv_descriptor_map *descriptor_map =
         nir_intrinsic_desc_type(instr) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ?
         &pipeline->ubo_map : &pipeline->ssbo_map;

      if (!const_val)
         unreachable("non-constant vulkan_resource_index array index");

      index = descriptor_map_add(descriptor_map, set, binding,
                                 const_val->u32,
                                 binding_layout->array_size);

      if (nir_intrinsic_desc_type(instr) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
         /* skip index 0 which is used for push constants */
         index++;
      }
      break;
   }

   default:
      unreachable("unsupported desc_type for vulkan_resource_index");
      break;
   }

   nir_ssa_def_rewrite_uses(&instr->dest.ssa,
                            nir_src_for_ssa(nir_imm_int(b, index)));
   nir_instr_remove(&instr->instr);
}

static bool
lower_intrinsic(nir_builder *b, nir_intrinsic_instr *instr,
                struct v3dv_pipeline *pipeline,
                const struct v3dv_pipeline_layout *layout)
{
   switch (instr->intrinsic) {

   case nir_intrinsic_load_push_constant:
      lower_load_push_constant(b, instr, pipeline);
      pipeline->use_push_constants = true;
      return true;

   case nir_intrinsic_vulkan_resource_index:
      lower_vulkan_resource_index(b, instr, pipeline, layout);
      return true;

   default:
      return false;
   }
}

static bool
lower_impl(nir_function_impl *impl,
           struct v3dv_pipeline *pipeline,
           const struct v3dv_pipeline_layout *layout)
{
   nir_builder b;
   nir_builder_init(&b, impl);
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         b.cursor = nir_before_instr(instr);
         switch (instr->type) {
         case nir_instr_type_intrinsic:
            progress |=
               lower_intrinsic(&b, nir_instr_as_intrinsic(instr), pipeline, layout);
            break;
         default:
            break;
         }
      }
   }

   return progress;
}

static bool
lower_pipeline_layout_info(nir_shader *shader,
                           struct v3dv_pipeline *pipeline,
                           const struct v3dv_pipeline_layout *layout)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl)
         progress |= lower_impl(function->impl, pipeline, layout);
   }

   return progress;
}


static void
lower_fs_io(nir_shader *nir)
{
   /* Our backend doesn't handle array fragment shader outputs */
   NIR_PASS_V(nir, nir_lower_io_arrays_to_elements_no_indirects, false);
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_out, NULL);

   nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs,
                               MESA_SHADER_FRAGMENT);

   nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs,
                               MESA_SHADER_FRAGMENT);

   NIR_PASS_V(nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
              type_size_vec4, 0);
}

static void
lower_vs_io(struct nir_shader *nir)
{
   NIR_PASS_V(nir, nir_lower_io_arrays_to_elements_no_indirects, false);

   nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs,
                               MESA_SHADER_VERTEX);

   nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs,
                               MESA_SHADER_VERTEX);

   /* FIXME: if we call nir_lower_io, we get a crash later. Likely because it
    * overlaps with v3d_nir_lower_io. Need further research though.
    */
}

static void
shader_debug_output(const char *message, void *data)
{
   /* FIXME: We probably don't want to debug anything extra here, and in fact
    * the compiler is not using this callback too much, only as an alternative
    * way to debug out the shaderdb stats, that you can already get using
    * V3D_DEBUG=shaderdb. Perhaps it would make sense to revisit the v3d
    * compiler to remove that callback.
    */
}

static void
pipeline_populate_v3d_key(struct v3d_key *key,
                          const VkGraphicsPipelineCreateInfo *pCreateInfo,
                          const struct v3dv_pipeline_stage *p_stage)
{
   /* default value. Would be override on the vs/gs populate methods when GS
    * gets supported
    */
   key->is_last_geometry_stage = true;

   /* Vulkan provides a way to define clip distances, but not clip planes, so
    * we understand that this would be always zero. Probably would need to be
    * revisited based on all the clip related extensions available.
    */
   key->ucp_enables = 0;

   key->environment = V3D_ENVIRONMENT_VULKAN;
}

/* FIXME: anv maps to hw primitive type. Perhaps eventually we would do the
 * same. For not using prim_mode that is the one already used on v3d
 */
static const enum pipe_prim_type vk_to_pipe_prim_type[] = {
   [VK_PRIMITIVE_TOPOLOGY_POINT_LIST] = PIPE_PRIM_POINTS,
   [VK_PRIMITIVE_TOPOLOGY_LINE_LIST] = PIPE_PRIM_LINES,
   [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP] = PIPE_PRIM_LINE_STRIP,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST] = PIPE_PRIM_TRIANGLES,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP] = PIPE_PRIM_TRIANGLE_STRIP,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN] = PIPE_PRIM_TRIANGLE_FAN,
   [VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY] = PIPE_PRIM_LINES_ADJACENCY,
   [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY] = PIPE_PRIM_LINE_STRIP_ADJACENCY,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY] = PIPE_PRIM_TRIANGLES_ADJACENCY,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY] = PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY,
};

static const enum pipe_logicop vk_to_pipe_logicop[] = {
   [VK_LOGIC_OP_CLEAR] = PIPE_LOGICOP_CLEAR,
   [VK_LOGIC_OP_AND] = PIPE_LOGICOP_AND,
   [VK_LOGIC_OP_AND_REVERSE] = PIPE_LOGICOP_AND_REVERSE,
   [VK_LOGIC_OP_COPY] = PIPE_LOGICOP_COPY,
   [VK_LOGIC_OP_AND_INVERTED] = PIPE_LOGICOP_AND_INVERTED,
   [VK_LOGIC_OP_NO_OP] = PIPE_LOGICOP_NOOP,
   [VK_LOGIC_OP_XOR] = PIPE_LOGICOP_XOR,
   [VK_LOGIC_OP_OR] = PIPE_LOGICOP_OR,
   [VK_LOGIC_OP_NOR] = PIPE_LOGICOP_NOR,
   [VK_LOGIC_OP_EQUIVALENT] = PIPE_LOGICOP_EQUIV,
   [VK_LOGIC_OP_INVERT] = PIPE_LOGICOP_INVERT,
   [VK_LOGIC_OP_OR_REVERSE] = PIPE_LOGICOP_OR_REVERSE,
   [VK_LOGIC_OP_COPY_INVERTED] = PIPE_LOGICOP_COPY_INVERTED,
   [VK_LOGIC_OP_OR_INVERTED] = PIPE_LOGICOP_OR_INVERTED,
   [VK_LOGIC_OP_NAND] = PIPE_LOGICOP_NAND,
   [VK_LOGIC_OP_SET] = PIPE_LOGICOP_SET,
};

static void
pipeline_populate_v3d_fs_key(struct v3d_fs_key *key,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const struct v3dv_pipeline_stage *p_stage)
{
   memset(key, 0, sizeof(*key));

   pipeline_populate_v3d_key(&key->base, pCreateInfo, p_stage);

   const VkPipelineInputAssemblyStateCreateInfo *ia_info =
      pCreateInfo->pInputAssemblyState;
   uint8_t topology = vk_to_pipe_prim_type[ia_info->topology];

   key->is_points = (topology == PIPE_PRIM_POINTS);
   key->is_lines = (topology >= PIPE_PRIM_LINES &&
                    topology <= PIPE_PRIM_LINE_STRIP);

   /* Vulkan doesn't appear to specify (anv does the same) */
   key->clamp_color = false;

   const VkPipelineColorBlendStateCreateInfo *cb_info =
      pCreateInfo->pColorBlendState;

   key->logicop_func = cb_info && cb_info->logicOpEnable == VK_TRUE ?
                       vk_to_pipe_logicop[cb_info->logicOp] :
                       PIPE_LOGICOP_COPY;

   const VkPipelineMultisampleStateCreateInfo *ms_info =
      pCreateInfo->pMultisampleState;

   /* FIXME: msaa not supported yet (although we add some of the code to
    * translate vk sample info in advance)
    */
   key->msaa = false;
   if (key->msaa & (ms_info != NULL)) {
      uint32_t sample_mask = 0xffff;

      if (ms_info->pSampleMask)
         sample_mask = ms_info->pSampleMask[0] & 0xffff;

      key->sample_coverage = (sample_mask != (1 << V3D_MAX_SAMPLES) - 1);
      key->sample_alpha_to_coverage = ms_info->alphaToCoverageEnable;
      key->sample_alpha_to_one = ms_info->alphaToOneEnable;
   }

   const VkPipelineDepthStencilStateCreateInfo *ds_info =
      pCreateInfo->pDepthStencilState;

   key->depth_enabled = (ds_info == NULL ? false : ds_info->depthTestEnable);

   /* Vulkan doesn't support alpha test */
   key->alpha_test = false;
   key->alpha_test_func = COMPARE_FUNC_NEVER;

   /* FIXME: placeholder. Final value for swap_color_rb depends on the format
    * of the surface to be used.
    */
   key->swap_color_rb = false;

   const struct v3dv_render_pass *pass =
      v3dv_render_pass_from_handle(pCreateInfo->renderPass);
   const struct v3dv_subpass *subpass = p_stage->pipeline->subpass;
   for (uint32_t i = 0; i < subpass->color_count; i++) {
      const uint32_t att_idx = subpass->color_attachments[i].attachment;
      if (att_idx == VK_ATTACHMENT_UNUSED)
         continue;

      key->cbufs |= 1 << i;

      VkFormat fb_format = pass->attachments[att_idx].desc.format;
      enum pipe_format fb_pipe_format = vk_format_to_pipe_format(fb_format);

      /* If logic operations are enabled then we might emit color reads and we
       * need to know the color buffer format and swizzle for that
       */
      if (key->logicop_func != PIPE_LOGICOP_COPY) {
         key->color_fmt[i].format = fb_pipe_format;
         key->color_fmt[i].swizzle = v3dv_get_format_swizzle(fb_format);
      }

      const struct util_format_description *desc =
         vk_format_description(fb_format);

      if (desc->channel[0].type == UTIL_FORMAT_TYPE_FLOAT &&
          desc->channel[0].size == 32) {
         key->f32_color_rb |= 1 << i;
      }

      if (p_stage->nir->info.fs.untyped_color_outputs) {
         if (util_format_is_pure_uint(fb_pipe_format))
            key->uint_color_rb |= 1 << i;
         else if (util_format_is_pure_sint(fb_pipe_format))
            key->int_color_rb |= 1 << i;
      }

      if (key->is_points) {
         /* FIXME: The mask would need to be computed based on the shader
          * inputs. On gallium it is done at st_atom_rasterizer
          * (sprite_coord_enable). anv seems (need to confirm) to do that on
          * genX_pipeline (PointSpriteTextureCoordinateEnable). Would be also
          * better to have tests to guide filling the mask.
          */
         key->point_sprite_mask = 0;

         /* Vulkan mandates upper left. */
         key->point_coord_upper_left = true;
      }
   }

   /* FIXME: we understand that this is used on GL to configure fixed-function
    * two side lighting support, and not make sense for Vulkan. Need to
    * confirm though.
    */
   key->light_twoside = false;

   /* FIXME: ditto, although for flat lighting. Again, neet to confirm.*/
   key->shade_model_flat = false;
}

static void
pipeline_populate_v3d_vs_key(struct v3d_vs_key *key,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const struct v3dv_pipeline_stage *p_stage)
{
   memset(key, 0, sizeof(*key));

   pipeline_populate_v3d_key(&key->base, pCreateInfo, p_stage);

   /* Vulkan doesn't appear to specify (anv does the same) */
   key->clamp_color = false;

   /* Vulkan specifies a point size per vertex, so true for if the prim are
    * points, like on ES2)
    */
   const VkPipelineInputAssemblyStateCreateInfo *ia_info =
      pCreateInfo->pInputAssemblyState;
   uint8_t topology = vk_to_pipe_prim_type[ia_info->topology];

   /* FIXME: not enough to being PRIM_POINTS, on gallium the full check is
    * PIPE_PRIM_POINTS && v3d->rasterizer->base.point_size_per_vertex */
   key->per_vertex_point_size = (topology == PIPE_PRIM_POINTS);

   key->is_coord = p_stage->is_coord;
   if (p_stage->is_coord) {
      /* The only output varying on coord shaders are for transform
       * feedback. Set to 0 as VK_EXT_transform_feedback is not supported.
       */
      key->num_used_outputs = 0;
   } else {
      struct v3dv_pipeline *pipeline = p_stage->pipeline;
      key->num_used_outputs = pipeline->fs->prog_data.fs->num_inputs;
      STATIC_ASSERT(sizeof(key->used_outputs) ==
                    sizeof(pipeline->fs->prog_data.fs->input_slots));
      memcpy(key->used_outputs, pipeline->fs->prog_data.fs->input_slots,
             sizeof(key->used_outputs));
   }
}

/*
 * Creates the pipeline_stage for the coordinate shader. Initially a clone of
 * the vs pipeline_stage, with is_coord to true;
 */
static struct v3dv_pipeline_stage*
pipeline_stage_create_vs_bin(const struct v3dv_pipeline_stage *src,
                             const VkAllocationCallbacks *pAllocator)
{
   struct v3dv_device *device = src->pipeline->device;

   struct v3dv_pipeline_stage *p_stage =
      vk_zalloc2(&device->alloc, pAllocator, sizeof(*p_stage), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   p_stage->pipeline = src->pipeline;
   assert(src->stage == MESA_SHADER_VERTEX);
   p_stage->stage = src->stage;
   p_stage->entrypoint = src->entrypoint;
   p_stage->module = src->module;
   p_stage->nir = src->nir;

   p_stage->is_coord = true;

   return p_stage;
}

/* FIXME: right now this just asks for an bo for the exact size of the qpu
 * assembly. It would be good to be slighly smarter and having one "all
 * shaders" bo per pipeline, so each p_stage would save their offset on
 * such. That is really relevant due the fact that bo are always aligned to
 * 4096, so that would allow to use less memory.
 *
 * For now one-bo per-assembly would work.
 */
static void
upload_assembly(struct v3dv_pipeline_stage *p_stage,
                const void *data,
                uint32_t size)
{
   const char *name = NULL;
   /* We are uploading the assembly just once, so at this point we shouldn't
    * have any bo
    */
   assert(p_stage->assembly_bo == NULL);
   struct v3dv_device *device = p_stage->pipeline->device;

   switch (p_stage->stage) {
   case MESA_SHADER_VERTEX:
      name = (p_stage->is_coord == true) ? "coord_shader_assembly" :
         "vertex_shader_assembly";
      break;
   case MESA_SHADER_FRAGMENT:
      name = "fragment_shader_assembly";
      break;
   default:
      unreachable("Stage not supported\n");
      break;
   };

   struct v3dv_bo *bo = v3dv_bo_alloc(device, size, name);
   if (!bo) {
      fprintf(stderr, "failed to allocate memory for shader\n");
      abort();
   }

   bool ok = v3dv_bo_map(device, bo, size);
   if (!ok) {
      fprintf(stderr, "failed to map source shader buffer\n");
      abort();
   }

   memcpy(bo->map, data, size);

   v3dv_bo_unmap(device, bo);

   p_stage->assembly_bo = bo;
}

static void
compile_pipeline_stage(struct v3dv_pipeline_stage *p_stage)
{
   struct v3dv_physical_device *physical_device =
      &p_stage->pipeline->device->instance->physicalDevice;
   const struct v3d_compiler *compiler = physical_device->compiler;

   /* We don't support variants (and probably will never support them) */
   int variant_id = 0;

   /* Note that we are assigning program_id slightly differently that
    * v3d. Here we are assigning one per pipeline stage, so vs and vs_bin
    * would have a different program_id, while v3d would have the same for
    * both. For the case of v3dv, it is more natural to have an id this way,
    * as right now we are using it for debugging, not for shader-db.
    */
   p_stage->program_id = physical_device->next_program_id++;

   if (V3D_DEBUG & (V3D_DEBUG_NIR |
                    v3d_debug_flag_for_shader_stage(p_stage->stage))) {
      fprintf(stderr, "Just before v3d_compile: %s prog %d NIR:\n",
              gl_shader_stage_name(p_stage->stage),
              p_stage->program_id);
      nir_print_shader(p_stage->nir, stderr);
      fprintf(stderr, "\n");
   }

   uint64_t *qpu_insts;
   uint32_t qpu_insts_size;

   qpu_insts = v3d_compile(compiler,
                           &p_stage->key.base, &p_stage->prog_data.base,
                           p_stage->nir,
                           shader_debug_output, NULL,
                           p_stage->program_id,
                           variant_id,
                           &qpu_insts_size);

   if (!qpu_insts) {
      fprintf(stderr, "Failed to compile %s prog %d NIR to VIR\n",
              gl_shader_stage_name(p_stage->stage),
              p_stage->program_id);
   } else {
      upload_assembly(p_stage, qpu_insts, qpu_insts_size);
   }

   free(qpu_insts);
}

/* FIXME: C&P from st, common place? */
static void
st_nir_opts(nir_shader *nir)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS_V(nir, nir_lower_vars_to_ssa);

      /* Linking deals with unused inputs/outputs, but here we can remove
       * things local to the shader in the hopes that we can cleanup other
       * things. This pass will also remove variables with only stores, so we
       * might be able to make progress after it.
       */
      NIR_PASS(progress, nir, nir_remove_dead_variables,
               (nir_variable_mode)(nir_var_function_temp |
                                   nir_var_shader_temp |
                                   nir_var_mem_shared),
               NULL);

      NIR_PASS(progress, nir, nir_opt_copy_prop_vars);
      NIR_PASS(progress, nir, nir_opt_dead_write_vars);

      if (nir->options->lower_to_scalar) {
         NIR_PASS_V(nir, nir_lower_alu_to_scalar, NULL, NULL);
         NIR_PASS_V(nir, nir_lower_phis_to_scalar);
      }

      NIR_PASS_V(nir, nir_lower_alu);
      NIR_PASS_V(nir, nir_lower_pack);
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);
      if (nir_opt_trivial_continues(nir)) {
         progress = true;
         NIR_PASS(progress, nir, nir_copy_prop);
         NIR_PASS(progress, nir, nir_opt_dce);
      }
      NIR_PASS(progress, nir, nir_opt_if, false);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_peephole_select, 8, true, true);

      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);

      NIR_PASS(progress, nir, nir_opt_undef);
      NIR_PASS(progress, nir, nir_opt_conditional_discard);
   } while (progress);
}

static void
link_shaders(nir_shader *producer, nir_shader *consumer)
{
   assert(producer);
   assert(consumer);

   if (producer->options->lower_to_scalar) {
      NIR_PASS_V(producer, nir_lower_io_to_scalar_early, nir_var_shader_out);
      NIR_PASS_V(consumer, nir_lower_io_to_scalar_early, nir_var_shader_in);
   }

   nir_lower_io_arrays_to_elements(producer, consumer);

   st_nir_opts(producer);
   st_nir_opts(consumer);

   if (nir_link_opt_varyings(producer, consumer))
      st_nir_opts(consumer);

   NIR_PASS_V(producer, nir_remove_dead_variables, nir_var_shader_out, NULL);
   NIR_PASS_V(consumer, nir_remove_dead_variables, nir_var_shader_in, NULL);

   if (nir_remove_unused_varyings(producer, consumer)) {
      NIR_PASS_V(producer, nir_lower_global_vars_to_local);
      NIR_PASS_V(consumer, nir_lower_global_vars_to_local);

      st_nir_opts(producer);
      st_nir_opts(consumer);

      /* Optimizations can cause varyings to become unused.
       * nir_compact_varyings() depends on all dead varyings being removed so
       * we need to call nir_remove_dead_variables() again here.
       */
      NIR_PASS_V(producer, nir_remove_dead_variables, nir_var_shader_out, NULL);
      NIR_PASS_V(consumer, nir_remove_dead_variables, nir_var_shader_in, NULL);
   }
}

static void
pipeline_lower_nir(struct v3dv_pipeline *pipeline,
                   struct v3dv_pipeline_stage *p_stage,
                   struct v3dv_pipeline_layout *layout)
{
   nir_shader_gather_info(p_stage->nir, nir_shader_get_entrypoint(p_stage->nir));

   /* Apply the actual pipeline layout to UBOs, SSBOs, and textures */
   NIR_PASS_V(p_stage->nir, lower_pipeline_layout_info, pipeline, layout);
}

static VkResult
pipeline_compile_graphics(struct v3dv_pipeline *pipeline,
                          const VkGraphicsPipelineCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator)
{
   struct v3dv_pipeline_stage *stages[MESA_SHADER_STAGES] = { };
   struct v3dv_device *device = pipeline->device;

   /* First pass to get the the common info from the shader and the nir
    * shader. We don't care of the coord shader for now.
    */
   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      const VkPipelineShaderStageCreateInfo *sinfo = &pCreateInfo->pStages[i];
      gl_shader_stage stage = vk_to_mesa_shader_stage(sinfo->stage);

      struct v3dv_pipeline_stage *p_stage =
         vk_zalloc2(&device->alloc, pAllocator, sizeof(*p_stage), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

      p_stage->pipeline = pipeline;
      p_stage->stage = stage;
      if (stage == MESA_SHADER_VERTEX)
         p_stage->is_coord = false;
      p_stage->entrypoint = sinfo->pName;
      p_stage->module = v3dv_shader_module_from_handle(sinfo->module);

      pipeline->active_stages |= sinfo->stage;

      /* FIXME: when cache support is in place, first check if for the given
       * spirv module and options, we already have a nir shader.
       */
      p_stage->nir = shader_module_compile_to_nir(pipeline->device, p_stage);

      stages[stage] = p_stage;
   }

   /* Add a no-op fragment shader if needed */
   if (!stages[MESA_SHADER_FRAGMENT]) {
      nir_builder b;
      nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_FRAGMENT,
                                     &v3dv_nir_options);
      b.shader->info.name = ralloc_strdup(b.shader, "noop_fs");

      struct v3dv_pipeline_stage *p_stage =
         vk_zalloc2(&device->alloc, pAllocator, sizeof(*p_stage), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

      p_stage->pipeline = pipeline;
      p_stage->stage = MESA_SHADER_FRAGMENT;
      p_stage->entrypoint = "main";
      p_stage->module = 0;
      p_stage->nir = b.shader;

      stages[MESA_SHADER_FRAGMENT] = p_stage;
      pipeline->active_stages |= MESA_SHADER_FRAGMENT;
   }

   /* Linking */
   struct v3dv_pipeline_stage *next_stage = NULL;
   for (int stage = MESA_SHADER_STAGES - 1; stage >= 0; stage--) {
      if (stages[stage] == NULL || stages[stage]->entrypoint == NULL)
         continue;

      struct v3dv_pipeline_stage *p_stage = stages[stage];

      switch(stage) {
      case MESA_SHADER_VERTEX:
         link_shaders(p_stage->nir, next_stage->nir);
         break;
      case MESA_SHADER_FRAGMENT:
         /* FIXME: not doing any specific linking stuff here yet */
         break;
      default:
         unreachable("not supported shader stage");
      }

      next_stage = stages[stage];
   }

   /* Compiling to vir */
   for (int stage = MESA_SHADER_STAGES - 1; stage >= 0; stage--) {
      if (stages[stage] == NULL || stages[stage]->entrypoint == NULL)
         continue;

      struct v3dv_pipeline_stage *p_stage = stages[stage];

      pipeline_lower_nir(pipeline, p_stage, pipeline->layout);

      switch(stage) {
      case MESA_SHADER_VERTEX:
         /* Right now we only support pipelines with both vertex and fragment
          * shader.
          */
         assert(pipeline->fs);

         pipeline->vs = p_stage;

         pipeline->vs_bin = pipeline_stage_create_vs_bin(pipeline->vs, pAllocator);

         /* FIXME: likely this to be moved to a gather info method to a full
          * struct inside pipeline_stage
          */
         const VkPipelineInputAssemblyStateCreateInfo *ia_info =
            pCreateInfo->pInputAssemblyState;
         pipeline->vs->topology = vk_to_pipe_prim_type[ia_info->topology];

         lower_vs_io(p_stage->nir);

         /* Note that at this point we would compile twice, one for vs and
          * other for vs_bin. For now we are maintaining two pipeline_stage
          * and two keys. Eventually we could reuse the key.
          */
         pipeline_populate_v3d_vs_key(&pipeline->vs->key.vs, pCreateInfo, pipeline->vs);
         pipeline_populate_v3d_vs_key(&pipeline->vs_bin->key.vs, pCreateInfo, pipeline->vs_bin);

         compile_pipeline_stage(pipeline->vs);
         compile_pipeline_stage(pipeline->vs_bin);
         break;
      case MESA_SHADER_FRAGMENT:
         pipeline->fs = p_stage;

         pipeline_populate_v3d_fs_key(&p_stage->key.fs, pCreateInfo,
                             p_stage);

         lower_fs_io(p_stage->nir);

         compile_pipeline_stage(pipeline->fs);
         break;
      default:
         unreachable("not supported shader stage");
      }
   }

   /* FIXME: values below are default when non-GS is available. Would need to
    * provide real values if GS gets supported
    */
   pipeline->vpm_cfg_bin.As = 1;
   pipeline->vpm_cfg_bin.Ve = 0;
   pipeline->vpm_cfg_bin.Vc = pipeline->vs_bin->prog_data.vs->vcm_cache_size;

   pipeline->vpm_cfg.As = 1;
   pipeline->vpm_cfg.Ve = 0;
   pipeline->vpm_cfg.Vc = pipeline->vs->prog_data.vs->vcm_cache_size;

   return VK_SUCCESS;
}

static unsigned
v3dv_dynamic_state_mask(VkDynamicState state)
{
   switch(state) {
   case VK_DYNAMIC_STATE_VIEWPORT:
      return V3DV_DYNAMIC_VIEWPORT;
   case VK_DYNAMIC_STATE_SCISSOR:
      return V3DV_DYNAMIC_SCISSOR;
   case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
      return V3DV_DYNAMIC_STENCIL_COMPARE_MASK;
   case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
      return V3DV_DYNAMIC_STENCIL_WRITE_MASK;
   case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
      return V3DV_DYNAMIC_STENCIL_REFERENCE;
   case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
      return V3DV_DYNAMIC_BLEND_CONSTANTS;
   default:
      unreachable("Unhandled dynamic state");
   }
}

static void
pipeline_init_dynamic_state(struct v3dv_pipeline *pipeline,
                            const VkGraphicsPipelineCreateInfo *pCreateInfo)
{
   pipeline->dynamic_state = default_dynamic_state;
   struct v3dv_dynamic_state *dynamic = &pipeline->dynamic_state;

   /* Create a mask of enabled dynamic states */
   uint32_t dynamic_states = 0;
   if (pCreateInfo->pDynamicState) {
      uint32_t count = pCreateInfo->pDynamicState->dynamicStateCount;
      for (uint32_t s = 0; s < count; s++) {
         dynamic_states |=
            v3dv_dynamic_state_mask(pCreateInfo->pDynamicState->pDynamicStates[s]);
      }
   }

   /* For any pipeline states that are not dynamic, set the dynamic state
    * from the static pipeline state.
    *
    * Notice that we don't let the number of viewports and scissort rects to
    * be set dynamically, so these are always copied from the pipeline state.
    */
   dynamic->viewport.count = pCreateInfo->pViewportState->viewportCount;
   if (!(dynamic_states & V3DV_DYNAMIC_VIEWPORT)) {
      assert(pCreateInfo->pViewportState);

      typed_memcpy(dynamic->viewport.viewports,
                   pCreateInfo->pViewportState->pViewports,
                   pCreateInfo->pViewportState->viewportCount);

      for (uint32_t i = 0; i < dynamic->viewport.count; i++) {
         v3dv_viewport_compute_xform(&dynamic->viewport.viewports[i],
                                     dynamic->viewport.scale[i],
                                     dynamic->viewport.translate[i]);
      }
   }

   dynamic->scissor.count = pCreateInfo->pViewportState->scissorCount;
   if (!(dynamic_states & V3DV_DYNAMIC_SCISSOR)) {
      typed_memcpy(dynamic->scissor.scissors,
                   pCreateInfo->pViewportState->pScissors,
                   pCreateInfo->pViewportState->scissorCount);
   }

   if (pCreateInfo->pDepthStencilState != NULL) {

      if (!(dynamic_states & V3DV_DYNAMIC_STENCIL_COMPARE_MASK)) {
         dynamic->stencil_compare_mask.front =
            pCreateInfo->pDepthStencilState->front.compareMask;
         dynamic->stencil_compare_mask.back =
            pCreateInfo->pDepthStencilState->back.compareMask;
      }

      if (!(dynamic_states & V3DV_DYNAMIC_STENCIL_WRITE_MASK)) {
         dynamic->stencil_write_mask.front =
            pCreateInfo->pDepthStencilState->front.writeMask;
         dynamic->stencil_write_mask.back =
            pCreateInfo->pDepthStencilState->back.writeMask;
      }

      if (!(dynamic_states & V3DV_DYNAMIC_STENCIL_REFERENCE)) {
         dynamic->stencil_reference.front =
            pCreateInfo->pDepthStencilState->front.reference;
         dynamic->stencil_reference.back =
            pCreateInfo->pDepthStencilState->back.reference;
      }
   }

   if (pCreateInfo->pColorBlendState &&
       !(dynamic_states & V3DV_DYNAMIC_BLEND_CONSTANTS)) {
      memcpy(dynamic->blend_constants,
             pCreateInfo->pColorBlendState->blendConstants,
             sizeof(dynamic->blend_constants));
   }

   pipeline->dynamic_state.mask = dynamic_states;
}

static uint8_t
blend_factor(VkBlendFactor factor, bool dst_alpha_one, bool *needs_constants)
{
   switch (factor) {
   case VK_BLEND_FACTOR_ZERO:
   case VK_BLEND_FACTOR_ONE:
   case VK_BLEND_FACTOR_SRC_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
   case VK_BLEND_FACTOR_DST_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
   case VK_BLEND_FACTOR_SRC_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
      return factor;
   case VK_BLEND_FACTOR_CONSTANT_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
   case VK_BLEND_FACTOR_CONSTANT_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
      *needs_constants = true;
      return factor;
   case VK_BLEND_FACTOR_DST_ALPHA:
      return dst_alpha_one ? V3D_BLEND_FACTOR_ONE :
                             V3D_BLEND_FACTOR_DST_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
      return dst_alpha_one ? V3D_BLEND_FACTOR_ZERO :
                             V3D_BLEND_FACTOR_INV_DST_ALPHA;
   case VK_BLEND_FACTOR_SRC1_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
   case VK_BLEND_FACTOR_SRC1_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
      assert(!"Invalid blend factor: dual source blending not supported.");
   default:
      assert(!"Unknown blend factor.");
   }

   /* Should be handled by the switch, added to avoid a "end of non-void
    * function" error
    */
   unreachable("Unknown blend factor.");
}

static void
pack_blend(struct v3dv_pipeline *pipeline,
           const VkPipelineColorBlendStateCreateInfo *cb_info)
{
   /* By default, we are not enabling blending and all color channel writes are
    * enabled. Color write enables are independent of whether blending is
    * enabled or not.
    *
    * Vulkan specifies color write masks so that bits set correspond to
    * enabled channels. Our hardware does it the other way around.
    */
   pipeline->blend.enables = 0;
   pipeline->blend.color_write_masks = 0; /* All channels enabled */

   if (!cb_info)
      return;

   assert(pipeline->subpass);
   if (pipeline->subpass->color_count == 0)
      return;

   pipeline->blend.needs_color_constants = false;
   uint32_t color_write_masks = 0;
   for (uint32_t i = 0; i < cb_info->attachmentCount; i++) {
      const VkPipelineColorBlendAttachmentState *b_state =
         &cb_info->pAttachments[i];

      assert(i < pipeline->subpass->color_count);

      uint32_t attachment_idx =
         pipeline->subpass->color_attachments[i].attachment;
      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      color_write_masks |= (~b_state->colorWriteMask & 0xf) << (4 * i);

      if (!b_state->blendEnable)
         continue;

      VkAttachmentDescription *desc =
         &pipeline->pass->attachments[attachment_idx].desc;
      const struct v3dv_format *format = v3dv_get_format(desc->format);
      bool dst_alpha_one = (format->swizzle[3] == PIPE_SWIZZLE_1);

      uint8_t rt_mask = 1 << i;
      pipeline->blend.enables |= rt_mask;

      v3dv_pack(pipeline->blend.cfg[i], BLEND_CFG, config) {
         config.render_target_mask = rt_mask;

         config.color_blend_mode = b_state->colorBlendOp;
         config.color_blend_dst_factor =
            blend_factor(b_state->dstColorBlendFactor, dst_alpha_one,
                         &pipeline->blend.needs_color_constants);
         config.color_blend_src_factor =
            blend_factor(b_state->srcColorBlendFactor, dst_alpha_one,
                         &pipeline->blend.needs_color_constants);

         config.alpha_blend_mode = b_state->alphaBlendOp;
         config.alpha_blend_dst_factor =
            blend_factor(b_state->dstAlphaBlendFactor, dst_alpha_one,
                         &pipeline->blend.needs_color_constants);
         config.alpha_blend_src_factor =
            blend_factor(b_state->srcAlphaBlendFactor, dst_alpha_one,
                         &pipeline->blend.needs_color_constants);
      }
   }

   if (pipeline->blend.needs_color_constants) {
      v3dv_pack(pipeline->blend.constant_color, BLEND_CONSTANT_COLOR, color) {
         color.red_f16 = _mesa_float_to_half(cb_info->blendConstants[0]);
         color.green_f16 = _mesa_float_to_half(cb_info->blendConstants[1]);
         color.blue_f16 = _mesa_float_to_half(cb_info->blendConstants[2]);
         color.alpha_f16 = _mesa_float_to_half(cb_info->blendConstants[3]);
      }
   }

   pipeline->blend.color_write_masks = color_write_masks;
}

/* This requires that pack_blend() had been called before so we can set
 * the overall blend enable bit in the CFG_BITS packet.
 */
static void
pack_cfg_bits(struct v3dv_pipeline *pipeline,
              const VkPipelineDepthStencilStateCreateInfo *ds_info,
              const VkPipelineRasterizationStateCreateInfo *rs_info)
{
   assert(sizeof(pipeline->cfg_bits) == cl_packet_length(CFG_BITS));

   v3dv_pack(pipeline->cfg_bits, CFG_BITS, config) {
      config.enable_forward_facing_primitive =
         rs_info ? !(rs_info->cullMode & VK_CULL_MODE_FRONT_BIT) : false;

      config.enable_reverse_facing_primitive =
         rs_info ? !(rs_info->cullMode & VK_CULL_MODE_BACK_BIT) : false;

      /* Seems like the hardware is backwards regarding this setting... */
      config.clockwise_primitives =
         rs_info ? rs_info->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE : false;

      config.enable_depth_offset = rs_info ? rs_info->depthBiasEnable: false;

      /* This is required to pass line rasterization tests in CTS while
       * exposing, at least, a minimum of 4-bits of subpixel precision
       * (the minimum requirement).
       */
      config.line_rasterization = 1; /* perp end caps */

      /* FIXME: oversample_mode postponed until msaa gets supported */
      config.rasterizer_oversample_mode = false;

      /* From the Vulkan spec:
       *
       *   "Provoking Vertex:
       *
       *       The vertex in a primitive from which flat shaded attribute
       *       values are taken. This is generally the “first” vertex in the
       *       primitive, and depends on the primitive topology."
       *
       * First vertex is the Direct3D style for provoking vertex. OpenGL uses
       * the last vertex by default.
       */
      config.direct3d_provoking_vertex = true;

      config.blend_enable = pipeline->blend.enables != 0;

      /* Disable depth/stencil if we don't have a D/S attachment */
      bool has_ds_attachment =
         pipeline->subpass->ds_attachment.attachment != VK_ATTACHMENT_UNUSED;

      /* Note: ez state may update based on the compiled FS, along with zsa */
      config.early_z_updates_enable = false;
      if (ds_info && ds_info->depthTestEnable && has_ds_attachment) {
         config.z_updates_enable = true;
         config.early_z_enable = false;
         config.depth_test_function = ds_info->depthCompareOp;
      } else {
         config.depth_test_function = VK_COMPARE_OP_ALWAYS;
      }

      config.stencil_enable =
         ds_info ? ds_info->stencilTestEnable && has_ds_attachment: false;
   };
}

static uint32_t
translate_stencil_op(enum pipe_stencil_op op)
{
   switch (op) {
   case VK_STENCIL_OP_KEEP:
      return V3D_STENCIL_OP_KEEP;
   case VK_STENCIL_OP_ZERO:
      return V3D_STENCIL_OP_ZERO;
   case VK_STENCIL_OP_REPLACE:
      return V3D_STENCIL_OP_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
      return V3D_STENCIL_OP_INCR;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
      return V3D_STENCIL_OP_DECR;
   case VK_STENCIL_OP_INVERT:
      return V3D_STENCIL_OP_INVERT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:
      return V3D_STENCIL_OP_INCWRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:
      return V3D_STENCIL_OP_DECWRAP;
   default:
      unreachable("bad stencil op");
   }
}

static void
pack_single_stencil_cfg(struct v3dv_pipeline *pipeline,
                        uint8_t *stencil_cfg,
                        bool is_front,
                        bool is_back,
                        const VkStencilOpState *stencil_state)
{
   /* From the Vulkan spec:
    *
    *   "Reference is an integer reference value that is used in the unsigned
    *    stencil comparison. The reference value used by stencil comparison
    *    must be within the range [0,2^s-1] , where s is the number of bits in
    *    the stencil framebuffer attachment, otherwise the reference value is
    *    considered undefined."
    *
    * In our case, 's' is always 8, so we clamp to that to prevent our packing
    * functions to assert in debug mode if they see larger values.
    *
    * If we have dynamic state we need to make sure we set the corresponding
    * state bits to 0, since cl_emit_with_prepacked ORs the new value with
    * the old.
    */
   const uint8_t write_mask =
      pipeline->dynamic_state.mask & V3DV_DYNAMIC_STENCIL_WRITE_MASK ?
         0 : stencil_state->writeMask & 0xff;

   const uint8_t compare_mask =
      pipeline->dynamic_state.mask & V3DV_DYNAMIC_STENCIL_COMPARE_MASK ?
         0 : stencil_state->compareMask & 0xff;

   const uint8_t reference =
      pipeline->dynamic_state.mask & V3DV_DYNAMIC_STENCIL_COMPARE_MASK ?
         0 : stencil_state->reference & 0xff;

   v3dv_pack(stencil_cfg, STENCIL_CFG, config) {
      config.front_config = is_front;
      config.back_config = is_back;
      config.stencil_write_mask = write_mask;
      config.stencil_test_mask = compare_mask;
      config.stencil_test_function = stencil_state->compareOp;
      config.stencil_pass_op = translate_stencil_op(stencil_state->passOp);
      config.depth_test_fail_op = translate_stencil_op(stencil_state->depthFailOp);
      config.stencil_test_fail_op = translate_stencil_op(stencil_state->failOp);
      config.stencil_ref_value = reference;
   }
}

static void
pack_stencil_cfg(struct v3dv_pipeline *pipeline,
                 const VkPipelineDepthStencilStateCreateInfo *ds_info)
{
   assert(sizeof(pipeline->stencil_cfg) == 2 * cl_packet_length(STENCIL_CFG));

   if (!ds_info || !ds_info->stencilTestEnable)
      return;

   if (pipeline->subpass->ds_attachment.attachment == VK_ATTACHMENT_UNUSED)
      return;

   const uint32_t dynamic_stencil_states = V3DV_DYNAMIC_STENCIL_COMPARE_MASK |
                                           V3DV_DYNAMIC_STENCIL_WRITE_MASK |
                                           V3DV_DYNAMIC_STENCIL_REFERENCE;


   /* If front != back or we have dynamic stencil state we can't emit a single
    * packet for both faces.
    */
   bool needs_front_and_back = false;
   if ((pipeline->dynamic_state.mask & dynamic_stencil_states) ||
       memcmp(&ds_info->front, &ds_info->back, sizeof(ds_info->front)))
      needs_front_and_back = true;

   /* If the front and back configurations are the same we can emit both with
    * a single packet.
    */
   pipeline->emit_stencil_cfg[0] = true;
   if (!needs_front_and_back) {
      pack_single_stencil_cfg(pipeline, pipeline->stencil_cfg[0],
                              true, true, &ds_info->front);
   } else {
      pipeline->emit_stencil_cfg[1] = true;
      pack_single_stencil_cfg(pipeline, pipeline->stencil_cfg[0],
                              true, false, &ds_info->front);
      pack_single_stencil_cfg(pipeline, pipeline->stencil_cfg[1],
                              false, true, &ds_info->back);
   }
}

static bool
stencil_op_is_no_op(const VkStencilOpState *stencil)
{
   return stencil->depthFailOp == VK_STENCIL_OP_KEEP &&
          stencil->compareOp == VK_COMPARE_OP_ALWAYS;
}

static void
pipeline_set_ez_state(struct v3dv_pipeline *pipeline,
                      const VkPipelineDepthStencilStateCreateInfo *ds_info)
{
   if (!ds_info || !ds_info->depthTestEnable) {
      pipeline->ez_state = VC5_EZ_DISABLED;
      return;
   }

   switch (ds_info->depthCompareOp) {
   case VK_COMPARE_OP_LESS:
   case VK_COMPARE_OP_LESS_OR_EQUAL:
      pipeline->ez_state = VC5_EZ_LT_LE;
      break;
   case VK_COMPARE_OP_GREATER:
   case VK_COMPARE_OP_GREATER_OR_EQUAL:
      pipeline->ez_state = VC5_EZ_GT_GE;
      break;
   case VK_COMPARE_OP_NEVER:
   case VK_COMPARE_OP_EQUAL:
      pipeline->ez_state = VC5_EZ_UNDECIDED;
      break;
   default:
      pipeline->ez_state = VC5_EZ_DISABLED;
      break;
   }

   /* If stencil is enabled and is not a no-op, we need to disable EZ */
   if (ds_info->stencilTestEnable &&
       (!stencil_op_is_no_op(&ds_info->front) ||
        !stencil_op_is_no_op(&ds_info->back))) {
         pipeline->ez_state = VC5_EZ_DISABLED;
   }
}

static void
pack_shader_state_record(struct v3dv_pipeline *pipeline)
{
   assert(sizeof(pipeline->shader_state_record) ==
          cl_packet_length(GL_SHADER_STATE_RECORD));

   /* Note: we are not packing addresses, as we need the job (see
    * cl_pack_emit_reloc). Additionally uniforms can't be filled up at this
    * point as they depend on dynamic info that can be set after create the
    * pipeline (like viewport), . Would need to be filled later, so we are
    * doing a partial prepacking.
    */
   v3dv_pack(pipeline->shader_state_record, GL_SHADER_STATE_RECORD, shader) {
      shader.enable_clipping = true;

      shader.point_size_in_shaded_vertex_data =
         pipeline->vs->key.vs.per_vertex_point_size;

      /* Must be set if the shader modifies Z, discards, or modifies
       * the sample mask.  For any of these cases, the fragment
       * shader needs to write the Z value (even just discards).
       */
      shader.fragment_shader_does_z_writes =
         pipeline->fs->prog_data.fs->writes_z;
      /* Set if the EZ test must be disabled (due to shader side
       * effects and the early_z flag not being present in the
       * shader).
       */
      shader.turn_off_early_z_test =
         pipeline->fs->prog_data.fs->disable_ez;

      shader.fragment_shader_uses_real_pixel_centre_w_in_addition_to_centroid_w2 =
         pipeline->fs->prog_data.fs->uses_center_w;

      shader.any_shader_reads_hardware_written_primitive_id = false;

      shader.do_scoreboard_wait_on_first_thread_switch =
         pipeline->fs->prog_data.fs->lock_scoreboard_on_first_thrsw;
      shader.disable_implicit_point_line_varyings =
         !pipeline->fs->prog_data.fs->uses_implicit_point_line_varyings;

      shader.number_of_varyings_in_fragment_shader =
         pipeline->fs->prog_data.fs->num_inputs;

      shader.coordinate_shader_propagate_nans = true;
      shader.vertex_shader_propagate_nans = true;
      shader.fragment_shader_propagate_nans = true;

      /* Note: see previous note about adresses */
      /* shader.coordinate_shader_code_address */
      /* shader.vertex_shader_code_address */
      /* shader.fragment_shader_code_address */

      /* FIXME: Use combined input/output size flag in the common case (also
       * on v3d, see v3dx_draw).
       */
      shader.coordinate_shader_has_separate_input_and_output_vpm_blocks =
         pipeline->vs_bin->prog_data.vs->separate_segments;
      shader.vertex_shader_has_separate_input_and_output_vpm_blocks =
         pipeline->vs->prog_data.vs->separate_segments;

      shader.coordinate_shader_input_vpm_segment_size =
         pipeline->vs_bin->prog_data.vs->separate_segments ?
         pipeline->vs_bin->prog_data.vs->vpm_input_size : 1;
      shader.vertex_shader_input_vpm_segment_size =
         pipeline->vs->prog_data.vs->separate_segments ?
         pipeline->vs->prog_data.vs->vpm_input_size : 1;

      shader.coordinate_shader_output_vpm_segment_size =
         pipeline->vs_bin->prog_data.vs->vpm_output_size;
      shader.vertex_shader_output_vpm_segment_size =
         pipeline->vs->prog_data.vs->vpm_output_size;

      /* Note: see previous note about adresses */
      /* shader.coordinate_shader_uniforms_address */
      /* shader.vertex_shader_uniforms_address */
      /* shader.fragment_shader_uniforms_address */

      shader.min_coord_shader_input_segments_required_in_play =
         pipeline->vpm_cfg_bin.As;
      shader.min_vertex_shader_input_segments_required_in_play =
         pipeline->vpm_cfg.As;

      shader.min_coord_shader_output_segments_required_in_play_in_addition_to_vcm_cache_size =
         pipeline->vpm_cfg_bin.Ve;
      shader.min_vertex_shader_output_segments_required_in_play_in_addition_to_vcm_cache_size =
         pipeline->vpm_cfg.Ve;

      shader.coordinate_shader_4_way_threadable =
         pipeline->vs_bin->prog_data.vs->base.threads == 4;
      shader.vertex_shader_4_way_threadable =
         pipeline->vs->prog_data.vs->base.threads == 4;
      shader.fragment_shader_4_way_threadable =
         pipeline->fs->prog_data.fs->base.threads == 4;

      shader.coordinate_shader_start_in_final_thread_section =
         pipeline->vs_bin->prog_data.vs->base.single_seg;
      shader.vertex_shader_start_in_final_thread_section =
         pipeline->vs->prog_data.vs->base.single_seg;
      shader.fragment_shader_start_in_final_thread_section =
         pipeline->fs->prog_data.fs->base.single_seg;

      shader.vertex_id_read_by_coordinate_shader =
         pipeline->vs_bin->prog_data.vs->uses_vid;
      shader.instance_id_read_by_coordinate_shader =
         pipeline->vs_bin->prog_data.vs->uses_iid;
      shader.vertex_id_read_by_vertex_shader =
         pipeline->vs->prog_data.vs->uses_vid;
      shader.instance_id_read_by_vertex_shader =
         pipeline->vs->prog_data.vs->uses_iid;

      /* Note: see previous note about adresses */
      /* shader.address_of_default_attribute_values */
   }
}

static void
pack_vcm_cache_size(struct v3dv_pipeline *pipeline)
{
   assert(sizeof(pipeline->vcm_cache_size) ==
          cl_packet_length(VCM_CACHE_SIZE));

   v3dv_pack(pipeline->vcm_cache_size, VCM_CACHE_SIZE, vcm) {
      vcm.number_of_16_vertex_batches_for_binning = pipeline->vpm_cfg_bin.Vc;
      vcm.number_of_16_vertex_batches_for_rendering = pipeline->vpm_cfg.Vc;
   }
}

/* As defined on the GL_SHADER_STATE_ATTRIBUTE_RECORD */
static uint8_t
get_attr_type(const struct util_format_description *desc)
{
   uint32_t r_size = desc->channel[0].size;
   uint8_t attr_type = ATTRIBUTE_FLOAT;

   switch (desc->channel[0].type) {
   case UTIL_FORMAT_TYPE_FLOAT:
      if (r_size == 32) {
         attr_type = ATTRIBUTE_FLOAT;
      } else {
         assert(r_size == 16);
         attr_type = ATTRIBUTE_HALF_FLOAT;
      }
      break;

   case UTIL_FORMAT_TYPE_SIGNED:
   case UTIL_FORMAT_TYPE_UNSIGNED:
      switch (r_size) {
      case 32:
         attr_type = ATTRIBUTE_INT;
         break;
      case 16:
         attr_type = ATTRIBUTE_SHORT;
         break;
      case 10:
         attr_type = ATTRIBUTE_INT2_10_10_10;
         break;
      case 8:
         attr_type = ATTRIBUTE_BYTE;
         break;
      default:
         fprintf(stderr,
                 "format %s unsupported\n",
                 desc->name);
         attr_type = ATTRIBUTE_BYTE;
         abort();
      }
      break;

   default:
      fprintf(stderr,
              "format %s unsupported\n",
              desc->name);
      abort();
   }

   return attr_type;
}

static void
create_default_attribute_values(struct v3dv_pipeline *pipeline,
                                const VkPipelineVertexInputStateCreateInfo *vi_info)
{
   uint32_t size = MAX_VERTEX_ATTRIBS * sizeof(float) * 4;

   if (pipeline->default_attribute_values == NULL) {
      pipeline->default_attribute_values = v3dv_bo_alloc(pipeline->device, size,
                                                         "default_vi_attributes");

      if (!pipeline->default_attribute_values) {
         fprintf(stderr, "failed to allocate memory for the default "
                 "attribute values\n");
      }
   }

   bool ok = v3dv_bo_map(pipeline->device,
                         pipeline->default_attribute_values, size);
   if (!ok) {
      fprintf(stderr, "failed to map default attribute values buffer\n");
      abort();
   }

   uint32_t *attrs = pipeline->default_attribute_values->map;

   for (int i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
      attrs[i * 4 + 0] = 0;
      attrs[i * 4 + 1] = 0;
      attrs[i * 4 + 2] = 0;
      if (i < pipeline->va_count && vk_format_is_int(pipeline->va[i].vk_format)) {
         attrs[i * 4 + 3] = 1;
      } else {
         attrs[i * 4 + 3] = fui(1.0);
      }
   }

   v3dv_bo_unmap(pipeline->device, pipeline->default_attribute_values);
}

static void
pack_shader_state_attribute_record(struct v3dv_pipeline *pipeline,
                                   uint32_t index,
                                   const VkVertexInputAttributeDescription *vi_desc)
{
   const uint32_t packet_length =
      cl_packet_length(GL_SHADER_STATE_ATTRIBUTE_RECORD);

   const struct util_format_description *desc =
      vk_format_description(vi_desc->format);

   uint32_t binding = vi_desc->binding;

   v3dv_pack(&pipeline->vertex_attrs[index * packet_length],
             GL_SHADER_STATE_ATTRIBUTE_RECORD, attr) {

      /* vec_size == 0 means 4 */
      attr.vec_size = desc->nr_channels & 3;
      attr.signed_int_type = (desc->channel[0].type ==
                              UTIL_FORMAT_TYPE_SIGNED);
      attr.normalized_int_type = desc->channel[0].normalized;
      attr.read_as_int_uint = desc->channel[0].pure_integer;

      attr.instance_divisor = MIN2(pipeline->vb[binding].instance_divisor,
                                   0xffff);
      attr.stride = pipeline->vb[binding].stride;
      attr.type = get_attr_type(desc);
   }
}

static VkResult
pipeline_init(struct v3dv_pipeline *pipeline,
              struct v3dv_device *device,
              const VkGraphicsPipelineCreateInfo *pCreateInfo,
              const VkAllocationCallbacks *pAllocator)
{
   VkResult result = VK_SUCCESS;

   pipeline->device = device;

   V3DV_FROM_HANDLE(v3dv_pipeline_layout, layout, pCreateInfo->layout);
   pipeline->layout = layout;

   V3DV_FROM_HANDLE(v3dv_render_pass, render_pass, pCreateInfo->renderPass);
   assert(pCreateInfo->subpass < render_pass->subpass_count);
   pipeline->pass = render_pass;
   pipeline->subpass = &render_pass->subpasses[pCreateInfo->subpass];

   pipeline_init_dynamic_state(pipeline, pCreateInfo);

   /* If rasterization is not enabled, various CreateInfo structs must be
    * ignored.
    */
   const bool raster_enabled =
      !pCreateInfo->pRasterizationState->rasterizerDiscardEnable;

   const VkPipelineDepthStencilStateCreateInfo *ds_info =
      raster_enabled ? pCreateInfo->pDepthStencilState : NULL;

   const VkPipelineRasterizationStateCreateInfo *rs_info =
      raster_enabled ? pCreateInfo->pRasterizationState : NULL;

   const VkPipelineColorBlendStateCreateInfo *cb_info =
      raster_enabled ? pCreateInfo->pColorBlendState : NULL;

   pack_blend(pipeline, cb_info);
   pack_cfg_bits(pipeline, ds_info, rs_info);
   pack_stencil_cfg(pipeline, ds_info);
   pipeline_set_ez_state(pipeline, ds_info);

   pipeline->primitive_restart =
      pCreateInfo->pInputAssemblyState->primitiveRestartEnable;

   result = pipeline_compile_graphics(pipeline, pCreateInfo, pAllocator);

   if (result != VK_SUCCESS) {
      /* Caller would already destroy the pipeline, and we didn't allocate any
       * extra info. We don't need to do anything else.
       */
      return result;
   }

   pack_shader_state_record(pipeline);
   pack_vcm_cache_size(pipeline);

   const VkPipelineVertexInputStateCreateInfo *vi_info =
      pCreateInfo->pVertexInputState;

   pipeline->vb_count = vi_info->vertexBindingDescriptionCount;
   for (uint32_t i = 0; i < vi_info->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi_info->pVertexBindingDescriptions[i];

      pipeline->vb[desc->binding].stride = desc->stride;
      pipeline->vb[desc->binding].instance_divisor = desc->inputRate;
   }

   pipeline->va_count = 0;
   nir_shader *shader = pipeline->vs->nir;

   for (uint32_t i = 0; i < vi_info->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *desc =
         &vi_info->pVertexAttributeDescriptions[i];
      uint32_t location = desc->location + VERT_ATTRIB_GENERIC0;

      nir_variable *var = nir_find_variable_with_location(shader, nir_var_shader_in, location);

      if (var != NULL) {
         unsigned driver_location = var->data.driver_location;

         pipeline->va[pipeline->va_count].offset = desc->offset;
         pipeline->va[pipeline->va_count].binding = desc->binding;
         pipeline->va[pipeline->va_count].driver_location = driver_location;
         pipeline->va[pipeline->va_count].vk_format = desc->format;

         pack_shader_state_attribute_record(pipeline, pipeline->va_count, desc);

         pipeline->va_count++;
      }
   }
   create_default_attribute_values(pipeline, vi_info);

   return result;
}

static VkResult
graphics_pipeline_create(VkDevice _device,
                         VkPipelineCache _cache,
                         const VkGraphicsPipelineCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkPipeline *pPipeline)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   struct v3dv_pipeline *pipeline;
   VkResult result;

   pipeline = vk_zalloc2(&device->alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = pipeline_init(pipeline, device,
                          pCreateInfo,
                          pAllocator);

   if (result != VK_SUCCESS) {
      vk_free2(&device->alloc, pAllocator, pipeline);
      return result;
   }

   *pPipeline = v3dv_pipeline_to_handle(pipeline);

   return VK_SUCCESS;
}

VkResult
v3dv_CreateGraphicsPipelines(VkDevice _device,
                             VkPipelineCache pipelineCache,
                             uint32_t count,
                             const VkGraphicsPipelineCreateInfo *pCreateInfos,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipelines)
{
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < count; i++) {
      VkResult local_result;

      local_result = graphics_pipeline_create(_device,
                                              pipelineCache,
                                              &pCreateInfos[i],
                                              pAllocator,
                                              &pPipelines[i]);

      if (local_result != VK_SUCCESS) {
         result = local_result;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   return result;
}

VkResult
v3dv_CreateComputePipelines(VkDevice device,
                            VkPipelineCache pipelineCache,
                            uint32_t createInfoCount,
                            const VkComputePipelineCreateInfo *pCreateInfos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   unreachable("vkCreateComputePipelines not implemented");
}

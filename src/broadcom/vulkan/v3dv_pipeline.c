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

   /* FIXME: needed? */
   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      NIR_PASS_V(nir, nir_lower_io_to_vector, nir_var_shader_out);
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS_V(nir, nir_lower_input_attachments,
                 &(nir_input_attachment_options) {
                    .use_fragcoord_sysval = false,
                       });                                                                                                                                                                                            }

   NIR_PASS_V(nir, nir_remove_dead_variables,
              nir_var_shader_in | nir_var_shader_out |
              nir_var_system_value | nir_var_mem_shared,
              NULL);

   NIR_PASS_V(nir, nir_propagate_invariant);

   NIR_PASS_V(nir, nir_lower_system_values);
   NIR_PASS_V(nir, nir_lower_clip_cull_distance_arrays);

   /* Vulkan uses the separate-shader linking model */
   nir->info.separate_shader = true;

   return nir;
}

static void
v3dv_nir_lower_fs_inputs(nir_shader *nir)
{
   /* FIXME: stub */
}

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void
v3dv_nir_lower_fs_outputs(nir_shader *nir)
{
   NIR_PASS_V(nir, nir_lower_io, nir_var_shader_out, type_size_vec4, 0);
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

   key->logicop_func = (cb_info->logicOpEnable == VK_FALSE ?
                        PIPE_LOGICOP_COPY :
                        vk_to_pipe_logicop[cb_info->logicOp]);

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

   const struct v3dv_subpass *subpass = p_stage->pipeline->subpass;
   for (uint32_t i = 0; i < subpass->color_count; i++) {
      if (subpass->color_attachments[i].attachment == VK_ATTACHMENT_UNUSED)
         continue;

      key->cbufs |= 1 << i;

      /* FIXME: in order to know this we need to access to the color
       * framebuffer. Still not in place. Using default hardcode value.
       */
      VkFormat fb_format = VK_FORMAT_R8G8B8A8_UNORM;
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
                             const VkAllocationCallbacks *alloc)
{
   struct v3dv_device *device = src->pipeline->device;

   struct v3dv_pipeline_stage *p_stage =
      vk_zalloc2(&device->alloc, alloc, sizeof(*p_stage), 8,
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
   /* We are uploading the assembly just once, so at this point we shouldn't
    * have any bo
    */
   assert(p_stage->assembly_bo == NULL);
   struct v3dv_device *device = p_stage->pipeline->device;

   struct v3dv_bo *bo = v3dv_bo_alloc(device, size);
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

static VkResult
pipeline_compile_graphics(struct v3dv_pipeline *pipeline,
                          const VkGraphicsPipelineCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *alloc)
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
         vk_zalloc2(&device->alloc, alloc, sizeof(*p_stage), 8,
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



   for (int stage = MESA_SHADER_STAGES - 1; stage >= 0; stage--) {
      if (stages[stage] == NULL || stages[stage]->entrypoint == NULL)
         continue;

      struct v3dv_pipeline_stage *p_stage = stages[stage];

      switch(stage) {
      case MESA_SHADER_VERTEX:
         /* Right now we only support pipelines with both vertex and fragment
          * shader.
          */
         assert(pipeline->fs);

         pipeline->vs = p_stage;

         pipeline->vs_bin = pipeline_stage_create_vs_bin(pipeline->vs, alloc);

         /* FIXME: likely this to be moved to a gather info method to a full
          * struct inside pipeline_stage
          */
         const VkPipelineInputAssemblyStateCreateInfo *ia_info =
            pCreateInfo->pInputAssemblyState;
         pipeline->vs->topology = vk_to_pipe_prim_type[ia_info->topology];

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

         /* FIXME: create a per-build method with all the lowering
          * needed. perhaps move to shader_compile_module_to_nir? */
         v3dv_nir_lower_fs_inputs(p_stage->nir);
         v3dv_nir_lower_fs_outputs(p_stage->nir);

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
   default:
      unreachable("Unhandled dynamic state");
   }
}

static uint32_t
pipeline_needed_dynamic_state(const VkGraphicsPipelineCreateInfo *pCreateInfo)
{
   uint32_t states = V3DV_DYNAMIC_ALL;

   /* FIXME: stub. Based on other values at pCreateInfo, we would need to
    * remove flags from states
    */

   return states;
}

static void
pipeline_init_dynamic_state(struct v3dv_pipeline *pipeline,
                            const VkGraphicsPipelineCreateInfo *pCreateInfo)
{
   uint32_t needed_states = pipeline_needed_dynamic_state(pCreateInfo);
   uint32_t states = needed_states;

   pipeline->dynamic_state = default_dynamic_state;

   if (pCreateInfo->pDynamicState) {
      uint32_t count = pCreateInfo->pDynamicState->dynamicStateCount;

      for (uint32_t s = 0; s < count; s++) {
         /* Remove all of the states that are marked as dynamic */
         states &= ~v3dv_dynamic_state_mask(pCreateInfo->pDynamicState->pDynamicStates[s]);
      }
   }

   struct v3dv_dynamic_state *dynamic = &pipeline->dynamic_state;
   /* Note, as this can be counter-intuitive: although we are checking against
    * _DYNAMIC flags, here we are copying the data from the pipeline that was
    * not defined as dynamic.
    */
   if (needed_states & V3DV_DYNAMIC_VIEWPORT) {
      assert(pCreateInfo->pViewportState);

      dynamic->viewport.count = pCreateInfo->pViewportState->viewportCount;
      if (states & V3DV_DYNAMIC_VIEWPORT) {
         typed_memcpy(dynamic->viewport.viewports,
                      pCreateInfo->pViewportState->pViewports,
                      pCreateInfo->pViewportState->viewportCount);
      }
   }

   if (needed_states & V3DV_DYNAMIC_SCISSOR) {
      dynamic->scissor.count = pCreateInfo->pViewportState->scissorCount;
      if (states & V3DV_DYNAMIC_SCISSOR) {
         typed_memcpy(dynamic->scissor.scissors,
                      pCreateInfo->pViewportState->pScissors,
                      pCreateInfo->pViewportState->scissorCount);
      }
   }

   pipeline->dynamic_state.mask = states;
}

static void
pack_cfg_bits(struct v3dv_pipeline *pipeline,
              const VkPipelineDepthStencilStateCreateInfo *ds_info,
              const VkPipelineRasterizationStateCreateInfo *rs_info,
              const VkPipelineColorBlendStateCreateInfo *cb_info)
{
   assert(sizeof(pipeline->cfg_bits) == cl_packet_length(CFG_BITS));

   /* CFG_BITS allow to set a overall blend_enable that it is anded with the
    * per-target blend enable. v3d so far creates a mask with each target, so
    * we just set to true if any attachment has blending enabled
    */
   bool overall_blend_enable = false;
   if (cb_info) {
      for (uint32_t i = 0; i < cb_info->attachmentCount; i++) {
         const VkPipelineColorBlendAttachmentState *b_state =
            &cb_info->pAttachments[i];

         overall_blend_enable |= b_state->blendEnable;
      }
   }

   v3dv_pack(pipeline->cfg_bits, CFG_BITS, config) {
      config.enable_forward_facing_primitive =
         rs_info ? !(rs_info->cullMode & VK_CULL_MODE_FRONT_BIT) : false;

      config.enable_reverse_facing_primitive =
         rs_info ? !(rs_info->cullMode & VK_CULL_MODE_BACK_BIT) : false;

      config.clockwise_primitives =
         rs_info ? rs_info->frontFace == VK_FRONT_FACE_CLOCKWISE : false;

      config.enable_depth_offset = rs_info ? rs_info->depthBiasEnable: false;

      /* FIXME: oversample_mode postponed until msaa gets supported */
      config.rasterizer_oversample_mode = false;

      config.direct3d_provoking_vertex = false; /* FIXME */

      config.blend_enable = overall_blend_enable;

      /* Note: ez state may update based on the compiled FS, along with zsa
       * (FIXME: not done)
       */
      config.early_z_updates_enable = true;
      if (ds_info && ds_info->depthTestEnable) {
         config.z_updates_enable = false;
         config.early_z_enable = config.early_z_enable;
         config.depth_test_function = ds_info->depthCompareOp;
      } else {
         config.depth_test_function = VK_COMPARE_OP_ALWAYS;
      }

      config.stencil_enable = ds_info ? ds_info->stencilTestEnable : false;
   };
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

static VkResult
pipeline_init(struct v3dv_pipeline *pipeline,
              struct v3dv_device *device,
              const VkGraphicsPipelineCreateInfo *pCreateInfo,
              const VkAllocationCallbacks *alloc)
{
   VkResult result = VK_SUCCESS;

   pipeline->device = device;

   V3DV_FROM_HANDLE(v3dv_render_pass, render_pass, pCreateInfo->renderPass);
   assert(pCreateInfo->subpass < render_pass->subpass_count);
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

   pack_cfg_bits(pipeline, ds_info, rs_info, cb_info);

   result = pipeline_compile_graphics(pipeline, pCreateInfo, alloc);

   if (result != VK_SUCCESS) {
      /* Caller would already destroy the pipeline, and we didn't allocate any
       * extra info. We don't need to do anything else.
       */
      return result;
   }

   pack_shader_state_record(pipeline);

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

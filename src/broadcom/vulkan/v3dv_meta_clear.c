/*
 * Copyright © 2020 Raspberry Pi
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

#include "v3dv_private.h"

#include "broadcom/cle/v3dx_pack.h"
#include "compiler/nir/nir_builder.h"
#include "vk_format_info.h"
#include "util/u_pack_color.h"

static nir_ssa_def *
gen_rect_vertices(nir_builder *b)
{
   nir_intrinsic_instr *vertex_id =
      nir_intrinsic_instr_create(b->shader,
                                 nir_intrinsic_load_vertex_id);
   nir_ssa_dest_init(&vertex_id->instr, &vertex_id->dest, 1, 32, "vertexid");
   nir_builder_instr_insert(b, &vertex_id->instr);


   /* vertex 0: -1.0, -1.0
    * vertex 1: -1.0,  1.0
    * vertex 2:  1.0, -1.0
    * vertex 3:  1.0,  1.0
    *
    * so:
    *
    * channel 0 is vertex_id < 2 ? -1.0 :  1.0
    * channel 1 is vertex id & 1 ?  1.0 : -1.0
    */

   nir_ssa_def *one = nir_imm_int(b, 1);
   nir_ssa_def *c0cmp = nir_ilt(b, &vertex_id->dest.ssa, nir_imm_int(b, 2));
   nir_ssa_def *c1cmp = nir_ieq(b, nir_iand(b, &vertex_id->dest.ssa, one), one);

   nir_ssa_def *comp[4];
   comp[0] = nir_bcsel(b, c0cmp,
                       nir_imm_float(b, -1.0f),
                       nir_imm_float(b, 1.0f));

   comp[1] = nir_bcsel(b, c1cmp,
                       nir_imm_float(b, 1.0f),
                       nir_imm_float(b, -1.0f));
   comp[2] = nir_imm_float(b, 0.0f);
   comp[3] = nir_imm_float(b, 1.0f);
   return nir_vec(b, comp, 4);
}

static nir_shader *
get_color_clear_rect_vs()
{
   nir_builder b;
   const nir_shader_compiler_options *options = v3dv_pipeline_get_nir_options();
   nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_VERTEX, options);
   b.shader->info.name = ralloc_strdup(b.shader, "meta clear vs");

   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *vs_out_pos =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   vs_out_pos->data.location = VARYING_SLOT_POS;

   nir_ssa_def *pos = gen_rect_vertices(&b);
   nir_store_var(&b, vs_out_pos, pos, 0xf);

   return b.shader;
}

static nir_shader *
get_color_clear_rect_fs(struct v3dv_render_pass *pass, uint32_t rt_idx)
{
   nir_builder b;
   const nir_shader_compiler_options *options = v3dv_pipeline_get_nir_options();
   nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_FRAGMENT, options);
   b.shader->info.name = ralloc_strdup(b.shader, "meta clear fs");

   /* Since our implementation can only clear one RT at a time we know there
    * is a single subpass with a single attachment.
    */
   assert(pass->attachment_count == 1);
   enum pipe_format pformat =
      vk_format_to_pipe_format(pass->attachments[0].desc.format);
   const struct glsl_type *fs_out_type =
      util_format_is_float(pformat) ? glsl_vec4_type() : glsl_uvec4_type();

   nir_variable *fs_out_color =
      nir_variable_create(b.shader, nir_var_shader_out, fs_out_type, "out_color");
   fs_out_color->data.location = FRAG_RESULT_DATA0 + rt_idx;

   nir_intrinsic_instr *color_load =
      nir_intrinsic_instr_create(b.shader, nir_intrinsic_load_push_constant);
   nir_intrinsic_set_base(color_load, 0);
   nir_intrinsic_set_range(color_load, 16);
   color_load->src[0] = nir_src_for_ssa(nir_imm_int(&b, 0));
   color_load->num_components = 4;
   nir_ssa_dest_init(&color_load->instr, &color_load->dest, 4, 32, "clear color");
   nir_builder_instr_insert(&b, &color_load->instr);

   nir_store_var(&b, fs_out_color, &color_load->dest.ssa, 0xf);

   return b.shader;
}

static VkResult
create_color_clear_pipeline_layout(struct v3dv_device *device,
                                   VkPipelineLayout *pipeline_layout)
{
   VkPipelineLayoutCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 0,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges =
         &(VkPushConstantRange) { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 },
   };

   return v3dv_CreatePipelineLayout(v3dv_device_to_handle(device),
                                    &info, &device->alloc, pipeline_layout);
}

static VkResult
create_pipeline(struct v3dv_device *device,
                struct v3dv_render_pass *pass,
                uint32_t samples,
                struct nir_shader *vs_nir,
                struct nir_shader *fs_nir,
                const VkPipelineVertexInputStateCreateInfo *vi_state,
                const VkPipelineDepthStencilStateCreateInfo *ds_state,
                const VkPipelineColorBlendStateCreateInfo *cb_state,
                const VkPipelineLayout layout,
                VkPipeline *pipeline)
{
   struct v3dv_shader_module vs_m = { .nir = vs_nir };
   struct v3dv_shader_module fs_m = { .nir = fs_nir };

   VkPipelineShaderStageCreateInfo stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = v3dv_shader_module_to_handle(&vs_m),
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = v3dv_shader_module_to_handle(&fs_m),
         .pName = "main",
      },
   };

   VkGraphicsPipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,

      .stageCount = 2,
      .pStages = stages,

      .pVertexInputState = vi_state,

      .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
         .primitiveRestartEnable = false,
      },

      .pViewportState = &(VkPipelineViewportStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
         .viewportCount = 1,
         .scissorCount = 1,
      },

      .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .rasterizerDiscardEnable = false,
         .polygonMode = VK_POLYGON_MODE_FILL,
         .cullMode = VK_CULL_MODE_NONE,
         .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
         .depthBiasEnable = false,
      },

      .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = samples,
         .sampleShadingEnable = false,
         .pSampleMask = NULL,
         .alphaToCoverageEnable = false,
         .alphaToOneEnable = false,
      },

      .pDepthStencilState = ds_state,

      .pColorBlendState = cb_state,

      /* The meta clear pipeline declares all state as dynamic.
       * As a consequence, vkCmdBindPipeline writes no dynamic state
       * to the cmd buffer. Therefore, at the end of the meta clear,
       * we need only restore dynamic state that was vkCmdSet.
       *
       * FIXME: Update this when we support more dynamic states (adding
       * them now will assert because they are not supported).
       */
      .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
         .dynamicStateCount = 6,
         .pDynamicStates = (VkDynamicState[]) {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS,
#if 0
            VK_DYNAMIC_STATE_LINE_WIDTH,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS,
#endif
         },
      },

      .flags = 0,
      .layout = layout,
      .renderPass = v3dv_render_pass_to_handle(pass),
      .subpass = 0,
   };

   VkResult result =
      v3dv_CreateGraphicsPipelines(v3dv_device_to_handle(device),
                                   VK_NULL_HANDLE,
                                   1, &info,
                                   &device->alloc,
                                   pipeline);

   ralloc_free(vs_nir);
   ralloc_free(fs_nir);

   return result;
}

static VkResult
create_color_clear_pipeline(struct v3dv_device *device,
                            uint32_t rt_idx,
                            uint32_t samples,
                            VkRenderPass _pass,
                            VkPipelineLayout pipeline_layout,
                            VkPipeline *pipeline)
{
   /* For now we only support clearing a framebuffer with a single attachment */
   assert(rt_idx == 0);

   struct v3dv_render_pass *pass = v3dv_render_pass_from_handle(_pass);

   nir_shader *vs_nir = get_color_clear_rect_vs();
   nir_shader *fs_nir = get_color_clear_rect_fs(pass, rt_idx);

   const VkPipelineVertexInputStateCreateInfo vi_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 0,
      .vertexAttributeDescriptionCount = 0,
   };

   const VkPipelineDepthStencilStateCreateInfo ds_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = false,
      .depthWriteEnable = false,
      .depthBoundsTestEnable = false,
      .stencilTestEnable = false,
   };

   /* FIXME: for now our color clear pipeline can only clear a single RT,
    * but in the future we might want to be able to support multiple render
    * targets. If we do that, then we might also be able to implement partial
    * color clearing for vkCmdClearAttachments without having to split the
    * subpass job at all.
    */
   VkPipelineColorBlendAttachmentState blend_att_state[1] = { 0 };
   blend_att_state[rt_idx] = (VkPipelineColorBlendAttachmentState) {
      .blendEnable = false,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                        VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT,
   };

   const VkPipelineColorBlendStateCreateInfo cb_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOpEnable = false,
      .attachmentCount = 1,
      .pAttachments = blend_att_state
   };

   return create_pipeline(device,
                          pass,
                          samples,
                          vs_nir, fs_nir,
                          &vi_state,
                          &ds_state,
                          &cb_state,
                          pipeline_layout,
                          pipeline);
}

static VkResult
create_color_clear_render_pass(struct v3dv_device *device,
                               VkFormat format,
                               uint32_t samples,
                               VkRenderPass *pass)
{
   VkAttachmentDescription att = {
      .format = format,
      .samples = samples,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
   };

   VkAttachmentReference att_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_GENERAL,
   };

   VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .inputAttachmentCount = 0,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att_ref,
      .pResolveAttachments = NULL,
      .pDepthStencilAttachment = NULL,
      .preserveAttachmentCount = 0,
      .pPreserveAttachments = NULL,
   };

   VkRenderPassCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &att,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 0,
      .pDependencies = NULL,
   };

   return v3dv_CreateRenderPass(v3dv_device_to_handle(device),
                                &info, &device->alloc, pass);
}

static inline uint64_t
get_color_clear_pipeline_cache_key(VkFormat format, uint32_t samples)
{
   return ((uint64_t) samples) << 32 | format;
}

static VkResult
get_color_clear_pipeline(struct v3dv_device *device,
                         VkFormat format,
                         uint32_t samples,
                         struct v3dv_meta_color_clear_pipeline **pipeline)
{
   VkResult result = VK_SUCCESS;

   mtx_lock(&device->meta.mtx);
   if (!device->meta.color_clear.playout) {
      result =
         create_color_clear_pipeline_layout(device,
                                            &device->meta.color_clear.playout);
   }
   mtx_unlock(&device->meta.mtx);
   if (result != VK_SUCCESS)
      return result;

   uint64_t key = get_color_clear_pipeline_cache_key(format, samples);
   mtx_lock(&device->meta.mtx);
   struct hash_entry *entry =
      _mesa_hash_table_search(device->meta.color_clear.cache, &key);
   if (entry) {
      mtx_unlock(&device->meta.mtx);
      *pipeline = entry->data;
      return VK_SUCCESS;
   }

   *pipeline = vk_zalloc2(&device->alloc, NULL, sizeof(**pipeline), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (*pipeline == NULL)
      goto fail;

   result = create_color_clear_render_pass(device,
                                           format,
                                           samples,
                                           &(*pipeline)->pass);
   if (result != VK_SUCCESS)
      goto fail;

   result = create_color_clear_pipeline(device, 0 /* rt_idx*/, samples,
                                        (*pipeline)->pass,
                                        device->meta.color_clear.playout,
                                        &(*pipeline)->pipeline);
   if (result != VK_SUCCESS)
      goto fail;

   _mesa_hash_table_insert(device->meta.color_clear.cache, &key, *pipeline);

   mtx_unlock(&device->meta.mtx);
   return VK_SUCCESS;

fail:
   mtx_unlock(&device->meta.mtx);

   VkDevice _device = v3dv_device_to_handle(device);
   if (*pipeline) {
      if ((*pipeline)->pass)
         v3dv_DestroyRenderPass(_device, (*pipeline)->pass, &device->alloc);
      if ((*pipeline)->pipeline)
         v3dv_DestroyPipeline(_device, (*pipeline)->pipeline, &device->alloc);
      vk_free(&device->alloc, *pipeline);
      *pipeline = NULL;
   }

   return result;
}

static VkFormat
get_color_format_for_depth_stencil_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_D16_UNORM:
      return VK_FORMAT_R16_UINT;
   case VK_FORMAT_D32_SFLOAT:
      return VK_FORMAT_R32_SFLOAT;
   case VK_FORMAT_X8_D24_UNORM_PACK32:
   case VK_FORMAT_D24_UNORM_S8_UINT:
      return VK_FORMAT_R32_UINT;
   default:
      unreachable("Unsupported depth/stencil format");
   };
}

static void
emit_color_clear_rect(struct v3dv_cmd_buffer *cmd_buffer,
                      uint32_t attachment_idx,
                      VkClearColorValue clear_color,
                      const VkClearRect *rect)
{
   assert(cmd_buffer->state.pass);
   struct v3dv_device *device = cmd_buffer->device;
   struct v3dv_render_pass *pass = cmd_buffer->state.pass;

   assert(attachment_idx != VK_ATTACHMENT_UNUSED &&
          attachment_idx < pass->attachment_count);

   const uint32_t rt_samples = pass->attachments[attachment_idx].desc.samples;
   VkFormat rt_format = pass->attachments[attachment_idx].desc.format;
   if (vk_format_is_depth_or_stencil(rt_format))
      rt_format = get_color_format_for_depth_stencil_format(rt_format);

   struct v3dv_meta_color_clear_pipeline *pipeline = NULL;
   VkResult result =
      get_color_clear_pipeline(device, rt_format, rt_samples, &pipeline);
   if (result != VK_SUCCESS)
      return;
   assert(pipeline && pipeline->pipeline && pipeline->pass);

   /* Store command buffer state for the current subpass before we interrupt
    * it to emit the color clear pass and then finish the job for the
    * interrupted subpass.
    */
   v3dv_cmd_buffer_meta_state_push(cmd_buffer);
   v3dv_cmd_buffer_finish_job(cmd_buffer);

   struct v3dv_framebuffer *subpass_fb =
      v3dv_framebuffer_from_handle(cmd_buffer->state.meta.framebuffer);
   VkCommandBuffer cmd_buffer_handle = v3dv_cmd_buffer_to_handle(cmd_buffer);
   VkDevice device_handle = v3dv_device_to_handle(cmd_buffer->device);

   /* If we are clearing a depth/stencil attachment as a color attachment
    * then we need to configure the framebuffer to the compatible color
    * format.
    */
   struct v3dv_image_view attachment_layer_view;
   memcpy(&attachment_layer_view, subpass_fb->attachments[attachment_idx],
          sizeof(struct v3dv_image_view));
   if (vk_format_is_depth_or_stencil(attachment_layer_view.vk_format)) {
      attachment_layer_view.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
      attachment_layer_view.vk_format = rt_format;
      attachment_layer_view.format = v3dv_get_format(rt_format);
      assert(attachment_layer_view.format &&
             attachment_layer_view.format->supported &&
             attachment_layer_view.format->rt_type !=
                V3D_OUTPUT_IMAGE_FORMAT_NO);
      v3dv_get_internal_type_bpp_for_output_format(
         attachment_layer_view.format->rt_type,
         &attachment_layer_view.internal_type,
         &attachment_layer_view.internal_bpp);
   }

   /* Emit the pass for each attachment layer, which creates a framebuffer
    * for each selected layer of the attachment and then renders a scissored
    * quad in the clear color.
    */
   uint32_t dirty_dynamic_state = 0;
   for (uint32_t i = 0; i < rect->layerCount; i++) {
      attachment_layer_view.first_layer =
         subpass_fb->attachments[attachment_idx]->first_layer +
         rect->baseArrayLayer + i;
      attachment_layer_view.last_layer = attachment_layer_view.first_layer;

      VkImageView fb_attachment =
         v3dv_image_view_to_handle(&attachment_layer_view);

      VkFramebufferCreateInfo fb_info = {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = v3dv_render_pass_to_handle(pass),
         .attachmentCount = 1,
         .pAttachments = &fb_attachment,
         .width = subpass_fb->width,
         .height = subpass_fb->height,
         .layers = 1,
      };

      VkFramebuffer fb;
      v3dv_CreateFramebuffer(device_handle, &fb_info,
                             &cmd_buffer->device->alloc, &fb);

      VkRenderPassBeginInfo rp_info = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = pipeline->pass,
         .framebuffer = fb,
         .renderArea = {
            .offset = { rect->rect.offset.x, rect->rect.offset.y },
            .extent = { rect->rect.extent.width, rect->rect.extent.height } },
         .clearValueCount = 0,
      };

      v3dv_CmdBeginRenderPass(cmd_buffer_handle, &rp_info,
                              VK_SUBPASS_CONTENTS_INLINE);

      struct v3dv_job *job = cmd_buffer->state.job;
      if (!job) {
         v3dv_DestroyFramebuffer(device_handle, fb, NULL);
         goto fail_job_start;
      }
      job->is_subpass_continue = true;

      v3dv_CmdPushConstants(cmd_buffer_handle,
                            device->meta.color_clear.playout,
                            VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16,
                            &clear_color);

      v3dv_CmdBindPipeline(cmd_buffer_handle,
                           VK_PIPELINE_BIND_POINT_GRAPHICS,
                           pipeline->pipeline);

      const VkViewport viewport = {
         .x = rect->rect.offset.x,
         .y = rect->rect.offset.y,
         .width = rect->rect.extent.width,
         .height = rect->rect.extent.height,
         .minDepth = 0.0f,
         .maxDepth = 1.0f
      };
      v3dv_CmdSetViewport(cmd_buffer_handle, 0, 1, &viewport);
      v3dv_CmdSetScissor(cmd_buffer_handle, 0, 1, &rect->rect);

      v3dv_CmdDraw(cmd_buffer_handle, 4, 1, 0, 0);

      v3dv_CmdEndRenderPass(cmd_buffer_handle);

      /* The Vulkan spec doesn't allow to destroy the framebuffer until all
       * command buffers that use it have completed execution, however, in
       * our particular case this is fine, since copy into all framebuffer
       * info we need to submit and execute the job into the command buffer,
       * so we don't need to keep the framebuffer object around.
       */
      v3dv_DestroyFramebuffer(device_handle, fb, &cmd_buffer->device->alloc);
   }

   /* The clear pipeline sets viewport and scissor state, so we need
    * to restore it
    */
   dirty_dynamic_state = V3DV_CMD_DIRTY_VIEWPORT | V3DV_CMD_DIRTY_SCISSOR;

fail_job_start:
   v3dv_cmd_buffer_meta_state_pop(cmd_buffer, dirty_dynamic_state);
}

static void
emit_ds_clear_rect(struct v3dv_cmd_buffer *cmd_buffer,
                   uint32_t attachment_idx,
                   VkClearDepthStencilValue clear_ds,
                   const VkClearRect *rect)
{
   assert(cmd_buffer->state.pass);
   assert(attachment_idx != VK_ATTACHMENT_UNUSED);
   assert(attachment_idx < cmd_buffer->state.pass->attachment_count);

   VkFormat format =
      cmd_buffer->state.pass->attachments[attachment_idx].desc.format;
   enum pipe_format pformat = vk_format_to_pipe_format(format);

   VkClearColorValue clear_color;
   uint32_t clear_zs =
      util_pack_z_stencil(pformat, clear_ds.depth, clear_ds.stencil);
   if (format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
       format == VK_FORMAT_D24_UNORM_S8_UINT) {
      clear_zs = clear_zs << 8 | clear_zs >> 24;
   }

   /* We implement depth/stencil clears by turning them into color clears
    * with a compatible color format. Passing -1 as the render target index
    * will inform the color clear code that we are attempting to clear a
    * depth/stencil attachment.
    */
   clear_color.uint32[0] = clear_zs;
   emit_color_clear_rect(cmd_buffer, attachment_idx, clear_color, rect);
}

static void
emit_tlb_clear_store(struct v3dv_cmd_buffer *cmd_buffer,
                     struct v3dv_cl *cl,
                     uint32_t attachment_idx,
                     uint32_t layer,
                     uint32_t buffer)
{
   const struct v3dv_image_view *iview =
      cmd_buffer->state.framebuffer->attachments[attachment_idx];
   const struct v3dv_image *image = iview->image;
   const struct v3d_resource_slice *slice = &image->slices[iview->base_level];
   uint32_t layer_offset = v3dv_layer_offset(image,
                                             iview->base_level,
                                             iview->first_layer + layer);

   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = buffer;
      store.address = v3dv_cl_address(image->mem->bo, layer_offset);
      store.clear_buffer_being_stored = false;

      store.output_image_format = iview->format->rt_type;
      store.r_b_swap = iview->swap_rb;
      store.memory_format = slice->tiling;

      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         store.height_in_ub_or_stride =
            slice->padded_height_of_output_image_in_uif_blocks;
      } else if (slice->tiling == VC5_TILING_RASTER) {
         store.height_in_ub_or_stride = slice->stride;
      }

      if (image->samples > VK_SAMPLE_COUNT_1_BIT)
         store.decimate_mode = V3D_DECIMATE_MODE_ALL_SAMPLES;
      else
         store.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }
}

static void
emit_tlb_clear_stores(struct v3dv_cmd_buffer *cmd_buffer,
                      struct v3dv_cl *cl,
                      uint32_t attachment_count,
                      const VkClearAttachment *attachments,
                      uint32_t layer)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   bool has_stores = false;
   for (uint32_t i = 0; i < attachment_count; i++) {
      uint32_t attachment_idx;
      uint32_t buffer;
      if (attachments[i].aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                       VK_IMAGE_ASPECT_STENCIL_BIT)) {
         attachment_idx = subpass->ds_attachment.attachment;
         buffer = v3dv_zs_buffer_from_aspect_bits(attachments[i].aspectMask);
      } else {
         uint32_t rt_idx = attachments[i].colorAttachment;
         attachment_idx = subpass->color_attachments[rt_idx].attachment;
         buffer = RENDER_TARGET_0 + rt_idx;
      }

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      has_stores = true;
      emit_tlb_clear_store(cmd_buffer, cl, attachment_idx, layer, buffer);
   }

   if (!has_stores) {
      cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
   }
}

static void
emit_tlb_clear_per_tile_rcl(struct v3dv_cmd_buffer *cmd_buffer,
                            uint32_t attachment_count,
                            const VkClearAttachment *attachments,
                            uint32_t layer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   cl_emit(cl, END_OF_LOADS, end); /* Nothing to load */

   cl_emit(cl, PRIM_LIST_FORMAT, fmt) {
      fmt.primitive_type = LIST_TRIANGLES;
   }

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   emit_tlb_clear_stores(cmd_buffer, cl, attachment_count, attachments, layer);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_tlb_clear_layer_rcl(struct v3dv_cmd_buffer *cmd_buffer,
                         uint32_t attachment_count,
                         const VkClearAttachment *attachments,
                         uint32_t layer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;

   struct v3dv_job *job = cmd_buffer->state.job;
   struct v3dv_cl *rcl = &job->rcl;

   const struct v3dv_frame_tiling *tiling = &job->frame_tiling;

   const uint32_t tile_alloc_offset =
      64 * layer * tiling->draw_tiles_x * tiling->draw_tiles_y;
   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(job->tile_alloc, tile_alloc_offset);
   }

   cl_emit(rcl, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
      config.number_of_bin_tile_lists = 1;
      config.total_frame_width_in_tiles = tiling->draw_tiles_x;
      config.total_frame_height_in_tiles = tiling->draw_tiles_y;

      config.supertile_width_in_tiles = tiling->supertile_width;
      config.supertile_height_in_tiles = tiling->supertile_height;

      config.total_frame_width_in_supertiles =
         tiling->frame_width_in_supertiles;
      config.total_frame_height_in_supertiles =
         tiling->frame_height_in_supertiles;
   }

   /* Emit the clear and also the workaround for GFXH-1742 */
   for (int i = 0; i < 2; i++) {
      cl_emit(rcl, TILE_COORDINATES, coords);
      cl_emit(rcl, END_OF_LOADS, end);
      cl_emit(rcl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
      if (i == 0) {
         cl_emit(rcl, CLEAR_TILE_BUFFERS, clear) {
            clear.clear_z_stencil_buffer = true;
            clear.clear_all_render_targets = true;
         }
      }
      cl_emit(rcl, END_OF_TILE_MARKER, end);
   }

   cl_emit(rcl, FLUSH_VCD_CACHE, flush);

   emit_tlb_clear_per_tile_rcl(cmd_buffer, attachment_count, attachments, layer);

   uint32_t supertile_w_in_pixels =
      tiling->tile_width * tiling->supertile_width;
   uint32_t supertile_h_in_pixels =
      tiling->tile_height * tiling->supertile_height;

   const uint32_t max_render_x = framebuffer->width - 1;
   const uint32_t max_render_y = framebuffer->height - 1;
   const uint32_t max_x_supertile = max_render_x / supertile_w_in_pixels;
   const uint32_t max_y_supertile = max_render_y / supertile_h_in_pixels;

   for (int y = 0; y <= max_y_supertile; y++) {
      for (int x = 0; x <= max_x_supertile; x++) {
         cl_emit(rcl, SUPERTILE_COORDINATES, coords) {
            coords.column_number_in_supertiles = x;
            coords.row_number_in_supertiles = y;
         }
      }
   }
}

static void
emit_tlb_clear_job(struct v3dv_cmd_buffer *cmd_buffer,
                   uint32_t attachment_count,
                   const VkClearAttachment *attachments,
                   uint32_t base_layer,
                   uint32_t layer_count)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* Check how many color attachments we have and also if we have a
    * depth/stencil attachment.
    */
   uint32_t color_attachment_count = 0;
   VkClearAttachment color_attachments[4];
   const VkClearDepthStencilValue *ds_clear_value = NULL;
   for (uint32_t i = 0; i < attachment_count; i++) {
      if (attachments[i].aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                       VK_IMAGE_ASPECT_STENCIL_BIT)) {
         ds_clear_value = &attachments[i].clearValue.depthStencil;
      } else if (attachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         color_attachments[color_attachment_count++] = attachments[i];
      }
   }

   const uint8_t internal_bpp =
      v3dv_framebuffer_compute_internal_bpp(framebuffer, subpass);

   v3dv_job_start_frame(job,
                        framebuffer->width,
                        framebuffer->height,
                        framebuffer->layers,
                        color_attachment_count,
                        internal_bpp);

   struct v3dv_cl *rcl = &job->rcl;
   v3dv_cl_ensure_space_with_branch(rcl, 200 +
                                    layer_count * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   const struct v3dv_frame_tiling *tiling = &job->frame_tiling;
   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.early_z_disable = true;
      config.image_width_pixels = framebuffer->width;
      config.image_height_pixels = framebuffer->height;
      config.number_of_render_targets = MAX2(color_attachment_count, 1);
      config.multisample_mode_4x = false; /* FIXME */
      config.maximum_bpp_of_all_render_targets = tiling->internal_bpp;
   }

   for (uint32_t i = 0; i < color_attachment_count; i++) {
      uint32_t rt_idx = color_attachments[i].colorAttachment;
      uint32_t attachment_idx = subpass->color_attachments[rt_idx].attachment;
      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      const struct v3dv_render_pass_attachment *attachment =
         &state->pass->attachments[attachment_idx];

      uint32_t internal_type, internal_bpp, internal_size;
      const struct v3dv_format *format =
         v3dv_get_format(attachment->desc.format);
      v3dv_get_internal_type_bpp_for_output_format(format->rt_type,
                                                   &internal_type,
                                                   &internal_bpp);
      internal_size = 4 << internal_bpp;

      uint32_t clear_color[4] = { 0 };
      v3dv_get_hw_clear_color(&color_attachments[i].clearValue.color,
                              internal_type,
                              internal_size,
                              clear_color);

      struct v3dv_image_view *iview = framebuffer->attachments[attachment_idx];
      const struct v3dv_image *image = iview->image;
      const struct v3d_resource_slice *slice = &image->slices[iview->base_level];

      uint32_t clear_pad = 0;
      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         int uif_block_height = v3d_utile_height(image->cpp) * 2;

         uint32_t implicit_padded_height =
            align(framebuffer->height, uif_block_height) / uif_block_height;

         if (slice->padded_height_of_output_image_in_uif_blocks -
             implicit_padded_height >= 15) {
            clear_pad = slice->padded_height_of_output_image_in_uif_blocks;
         }
      }

      cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1, clear) {
         clear.clear_color_low_32_bits = clear_color[0];
         clear.clear_color_next_24_bits = clear_color[1] & 0xffffff;
         clear.render_target_number = i;
      };

      if (iview->internal_bpp >= V3D_INTERNAL_BPP_64) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART2, clear) {
            clear.clear_color_mid_low_32_bits =
              ((clear_color[1] >> 24) | (clear_color[2] << 8));
            clear.clear_color_mid_high_24_bits =
              ((clear_color[2] >> 24) | ((clear_color[3] & 0xffff) << 8));
            clear.render_target_number = i;
         };
      }

      if (iview->internal_bpp >= V3D_INTERNAL_BPP_128 || clear_pad) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART3, clear) {
            clear.uif_padded_height_in_uif_blocks = clear_pad;
            clear.clear_color_high_16_bits = clear_color[3] >> 16;
            clear.render_target_number = i;
         };
      }
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
      v3dv_render_pass_setup_render_target(cmd_buffer, 0,
                                           &rt.render_target_0_internal_bpp,
                                           &rt.render_target_0_internal_type,
                                           &rt.render_target_0_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 1,
                                           &rt.render_target_1_internal_bpp,
                                           &rt.render_target_1_internal_type,
                                           &rt.render_target_1_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 2,
                                           &rt.render_target_2_internal_bpp,
                                           &rt.render_target_2_internal_type,
                                           &rt.render_target_2_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 3,
                                           &rt.render_target_3_internal_bpp,
                                           &rt.render_target_3_internal_type,
                                           &rt.render_target_3_clamp);
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
      clear.z_clear_value = ds_clear_value ? ds_clear_value->depth : 1.0f;
      clear.stencil_clear_value = ds_clear_value ? ds_clear_value->stencil : 0;
   };

   cl_emit(rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
      init.use_auto_chained_tile_lists = true;
      init.size_of_first_block_in_chained_tile_lists =
         TILE_ALLOCATION_BLOCK_SIZE_64B;
   }

   for (int layer = base_layer; layer < base_layer + layer_count; layer++) {
      emit_tlb_clear_layer_rcl(cmd_buffer,
                               attachment_count,
                               attachments,
                               layer);
   }

   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
emit_tlb_clear(struct v3dv_cmd_buffer *cmd_buffer,
               uint32_t attachment_count,
               const VkClearAttachment *attachments,
               uint32_t base_layer,
               uint32_t layer_count)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* Save a copy of the current subpass tiling spec, since we are about to
    * split the job for the clear and we will need to then resume it with the
    * same specs.
    */
   struct v3dv_frame_tiling subpass_tiling;
   memcpy(&subpass_tiling, &job->frame_tiling, sizeof(subpass_tiling));

   job = v3dv_cmd_buffer_start_job(cmd_buffer, cmd_buffer->state.subpass_idx);

   /* vkCmdClearAttachments runs inside a render pass */
   job->is_subpass_continue = true;

   emit_tlb_clear_job(cmd_buffer,
                      attachment_count,
                      attachments,
                      base_layer, layer_count);

   /* Since vkCmdClearAttachments executes inside a render pass command, this
    * will emit the binner FLUSH packet. Notice that this won't emit the
    * subpass RCL for this job though because it will see that the job has
    * recorded its own RCL.
    */
   v3dv_cmd_buffer_finish_job(cmd_buffer);

   /* Make sure we have an active job to continue the render pass recording
    * after the clear.
    */
   job = v3dv_cmd_buffer_start_job(cmd_buffer, cmd_buffer->state.subpass_idx);

   v3dv_job_start_frame(job,
                        subpass_tiling.width,
                        subpass_tiling.height,
                        subpass_tiling.layers,
                        subpass_tiling.render_target_count,
                        subpass_tiling.internal_bpp);

   job->is_subpass_continue = true;
}

static bool
is_subrect(const VkRect2D *r0, const VkRect2D *r1)
{
   return r0->offset.x <= r1->offset.x &&
          r0->offset.y <= r1->offset.y &&
          r0->offset.x + r0->extent.width >= r1->offset.x + r1->extent.width &&
          r0->offset.y + r0->extent.height >= r1->offset.y + r1->extent.height;
}

static bool
can_use_tlb_clear(struct v3dv_cmd_buffer *cmd_buffer,
                  uint32_t rect_count,
                  const VkClearRect* rects)
{
   const struct v3dv_framebuffer *framebuffer = cmd_buffer->state.framebuffer;

   const VkRect2D *render_area = &cmd_buffer->state.render_area;

   /* Check if we are clearing a single region covering the entire framebuffer
    * and that we are not constrained by the current render area.
    *
    * From the Vulkan 1.0 spec:
    *
    *   "The vkCmdClearAttachments command is not affected by the bound
    *    pipeline state."
    *
    * So we can ignore scissor and viewport state for this check.
    */
   const VkRect2D fb_rect = {
      { 0, 0 },
      { framebuffer->width, framebuffer->height }
   };

   return rect_count == 1 &&
          is_subrect(&rects[0].rect, &fb_rect) &&
          is_subrect(render_area, &fb_rect);
}

void
v3dv_CmdClearAttachments(VkCommandBuffer commandBuffer,
                         uint32_t attachmentCount,
                         const VkClearAttachment *pAttachments,
                         uint32_t rectCount,
                         const VkClearRect *pRects)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   /* We can only clear attachments in the current subpass */
   assert(attachmentCount <= 5); /* 4 color + D/S */

   /* Check if we can use the fast path via the TLB */
   if (can_use_tlb_clear(cmd_buffer, rectCount, pRects)) {
      emit_tlb_clear(cmd_buffer, attachmentCount, pAttachments,
                     pRects[0].baseArrayLayer, pRects[0].layerCount);
      return;
   }

   /* Otherwise, fall back to drawing rects with the clear value */
   const struct v3dv_subpass *subpass =
      &cmd_buffer->state.pass->subpasses[cmd_buffer->state.subpass_idx];

   for (uint32_t i = 0; i < attachmentCount; i++) {
      uint32_t attachment_idx = VK_ATTACHMENT_UNUSED;

      if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         uint32_t rt_idx = pAttachments[i].colorAttachment;
         attachment_idx = subpass->color_attachments[rt_idx].attachment;
      } else if (pAttachments[i].aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                               VK_IMAGE_ASPECT_STENCIL_BIT)) {
         attachment_idx = subpass->ds_attachment.attachment;
      }

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         for (uint32_t j = 0; j < rectCount; j++) {
            emit_color_clear_rect(cmd_buffer,
                                  attachment_idx,
                                  pAttachments[i].clearValue.color,
                                  &pRects[j]);
         }
      } else {
         for (uint32_t j = 0; j < rectCount; j++) {
            emit_ds_clear_rect(cmd_buffer,
                               attachment_idx,
                               pAttachments[i].clearValue.depthStencil,
                               &pRects[j]);
         }
      }
   }
}

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
#include "pipe/p_context.h"

static VkResult val_create_cmd_buffer(
   struct val_device *                         device,
   struct val_cmd_pool *                       pool,
   VkCommandBufferLevel                        level,
   VkCommandBuffer*                            pCommandBuffer)
{
   struct val_cmd_buffer *cmd_buffer;

   cmd_buffer = vk_alloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &cmd_buffer->base,
                       VK_OBJECT_TYPE_COMMAND_BUFFER);
   cmd_buffer->device = device;
   cmd_buffer->pool = pool;
   list_inithead(&cmd_buffer->cmds);
   cmd_buffer->status = VAL_CMD_BUFFER_STATUS_INITIAL;
   if (pool) {
      list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);
   } else {
      /* Init the pool_link so we can safefly call list_del when we destroy
       * the command buffer
       */
      list_inithead(&cmd_buffer->pool_link);
   }
   *pCommandBuffer = val_cmd_buffer_to_handle(cmd_buffer);

   return VK_SUCCESS;
}

static void
val_cmd_buffer_free_all_cmds(struct val_cmd_buffer *cmd_buffer)
{
   struct val_cmd_buffer_entry *tmp, *cmd;
   LIST_FOR_EACH_ENTRY_SAFE(cmd, tmp, &cmd_buffer->cmds, cmd_link) {
      list_del(&cmd->cmd_link);
      vk_free(&cmd_buffer->pool->alloc, cmd);
   }
}

static VkResult val_reset_cmd_buffer(struct val_cmd_buffer *cmd_buffer)
{
   val_cmd_buffer_free_all_cmds(cmd_buffer);
   list_inithead(&cmd_buffer->cmds);
   cmd_buffer->status = VAL_CMD_BUFFER_STATUS_INITIAL;
   return VK_SUCCESS;
}

VkResult val_AllocateCommandBuffers(
   VkDevice                                    _device,
   const VkCommandBufferAllocateInfo*          pAllocateInfo,
   VkCommandBuffer*                            pCommandBuffers)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_cmd_pool, pool, pAllocateInfo->commandPool);

   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {

      if (!list_is_empty(&pool->free_cmd_buffers)) {
         struct val_cmd_buffer *cmd_buffer = list_first_entry(&pool->free_cmd_buffers, struct val_cmd_buffer, pool_link);

         list_del(&cmd_buffer->pool_link);
         list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

         result = val_reset_cmd_buffer(cmd_buffer);
         cmd_buffer->level = pAllocateInfo->level;

         pCommandBuffers[i] = val_cmd_buffer_to_handle(cmd_buffer);
      } else {
         result = val_create_cmd_buffer(device, pool, pAllocateInfo->level,
                                        &pCommandBuffers[i]);
         if (result != VK_SUCCESS)
            break;
      }
   }

   if (result != VK_SUCCESS) {
      val_FreeCommandBuffers(_device, pAllocateInfo->commandPool,
                             i, pCommandBuffers);
      memset(pCommandBuffers, 0,
             sizeof(*pCommandBuffers) * pAllocateInfo->commandBufferCount);
   }

   return result;
}

static void
val_cmd_buffer_destroy(struct val_cmd_buffer *cmd_buffer)
{
   val_cmd_buffer_free_all_cmds(cmd_buffer);
   list_del(&cmd_buffer->pool_link);
   vk_object_base_finish(&cmd_buffer->base);
   vk_free(&cmd_buffer->pool->alloc, cmd_buffer);
}

void val_FreeCommandBuffers(
   VkDevice                                    device,
   VkCommandPool                               commandPool,
   uint32_t                                    commandBufferCount,
   const VkCommandBuffer*                      pCommandBuffers)
{
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

      if (cmd_buffer) {
         if (cmd_buffer->pool) {
            list_del(&cmd_buffer->pool_link);
            list_addtail(&cmd_buffer->pool_link, &cmd_buffer->pool->free_cmd_buffers);
         } else
            val_cmd_buffer_destroy(cmd_buffer);
      }
   }
}

VkResult val_ResetCommandBuffer(
   VkCommandBuffer                             commandBuffer,
   VkCommandBufferResetFlags                   flags)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);

   return val_reset_cmd_buffer(cmd_buffer);
}

VkResult val_BeginCommandBuffer(
   VkCommandBuffer                             commandBuffer,
   const VkCommandBufferBeginInfo*             pBeginInfo)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VkResult result;
   if (cmd_buffer->status != VAL_CMD_BUFFER_STATUS_INITIAL) {
      result = val_reset_cmd_buffer(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;
   }
   cmd_buffer->status = VAL_CMD_BUFFER_STATUS_RECORDING;
   return VK_SUCCESS;
}

VkResult val_EndCommandBuffer(
   VkCommandBuffer                             commandBuffer)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   cmd_buffer->status = VAL_CMD_BUFFER_STATUS_EXECUTABLE;
   return VK_SUCCESS;
}

VkResult val_CreateCommandPool(
   VkDevice                                    _device,
   const VkCommandPoolCreateInfo*              pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkCommandPool*                              pCmdPool)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_cmd_pool *pool;

   pool = vk_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pool->base,
                       VK_OBJECT_TYPE_COMMAND_POOL);
   if (pAllocator)
      pool->alloc = *pAllocator;
   else
      pool->alloc = device->alloc;

   list_inithead(&pool->cmd_buffers);
   list_inithead(&pool->free_cmd_buffers);

   *pCmdPool = val_cmd_pool_to_handle(pool);

   return VK_SUCCESS;
}

void val_DestroyCommandPool(
   VkDevice                                    _device,
   VkCommandPool                               commandPool,
   const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct val_cmd_buffer, cmd_buffer,
                            &pool->cmd_buffers, pool_link) {
      val_cmd_buffer_destroy(cmd_buffer);
   }

   list_for_each_entry_safe(struct val_cmd_buffer, cmd_buffer,
                            &pool->free_cmd_buffers, pool_link) {
      val_cmd_buffer_destroy(cmd_buffer);
   }

   vk_object_base_finish(&pool->base);
   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult val_ResetCommandPool(
   VkDevice                                    device,
   VkCommandPool                               commandPool,
   VkCommandPoolResetFlags                     flags)
{
   VAL_FROM_HANDLE(val_cmd_pool, pool, commandPool);
   VkResult result;

   list_for_each_entry(struct val_cmd_buffer, cmd_buffer,
                       &pool->cmd_buffers, pool_link) {
      result = val_reset_cmd_buffer(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

void val_TrimCommandPool(
   VkDevice                                    device,
   VkCommandPool                               commandPool,
   VkCommandPoolTrimFlags                      flags)
{
   VAL_FROM_HANDLE(val_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct val_cmd_buffer, cmd_buffer,
                            &pool->free_cmd_buffers, pool_link) {
      val_cmd_buffer_destroy(cmd_buffer);
   }
}

static struct val_cmd_buffer_entry *cmd_buf_entry_alloc_size(struct val_cmd_buffer *cmd_buffer,
                                                             uint32_t extra_size,
                                                             enum val_cmds type)
{
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = sizeof(*cmd) + extra_size;
   cmd = vk_alloc(&cmd_buffer->pool->alloc,
                  cmd_size,
                  8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return NULL;

   cmd->cmd_type = type;
   return cmd;
}

static struct val_cmd_buffer_entry *cmd_buf_entry_alloc(struct val_cmd_buffer *cmd_buffer,
                                                        enum val_cmds type)
{
   return cmd_buf_entry_alloc_size(cmd_buffer, 0, type);
}

static void cmd_buf_queue(struct val_cmd_buffer *cmd_buffer,
                          struct val_cmd_buffer_entry *cmd)
{
   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);
}

static void
state_setup_attachments(struct val_attachment_state *attachments,
                        struct val_render_pass *pass,
                        const VkClearValue *clear_values)
{
   for (uint32_t i = 0; i < pass->attachment_count; ++i) {
      struct val_render_pass_attachment *att = &pass->attachments[i];
      VkImageAspectFlags att_aspects = vk_format_aspects(att->format);
      VkImageAspectFlags clear_aspects = 0;
      if (att_aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
         /* color attachment */
         if (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            clear_aspects |= VK_IMAGE_ASPECT_COLOR_BIT;
         }
      } else {
         /* depthstencil attachment */
         if ((att_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
             att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            clear_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if ((att_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
                att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE)
               clear_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
         }
         if ((att_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
             att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            clear_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
         }
      }
      attachments[i].pending_clear_aspects = clear_aspects;
      if (clear_values)
         attachments[i].clear_value = clear_values[i];
   }
}

void val_CmdBeginRenderPass(
   VkCommandBuffer                             commandBuffer,
   const VkRenderPassBeginInfo*                pRenderPassBegin,
   VkSubpassContents                           contents)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_render_pass, pass, pRenderPassBegin->renderPass);
   VAL_FROM_HANDLE(val_framebuffer, framebuffer, pRenderPassBegin->framebuffer);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = pass->attachment_count * sizeof(struct val_attachment_state);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_BEGIN_RENDER_PASS);
   if (!cmd)
      return;

   cmd->u.begin_render_pass.render_pass = pass;
   cmd->u.begin_render_pass.framebuffer = framebuffer;
   cmd->u.begin_render_pass.render_area = pRenderPassBegin->renderArea;

   cmd->u.begin_render_pass.attachments = (struct val_attachment_state *)(cmd + 1);
   state_setup_attachments(cmd->u.begin_render_pass.attachments, pass, pRenderPassBegin->pClearValues);

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdNextSubpass(
   VkCommandBuffer                             commandBuffer,
   VkSubpassContents                           contents)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_NEXT_SUBPASS);
   if (!cmd)
      return;

   cmd->u.next_subpass.contents = contents;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdBindVertexBuffers(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    firstBinding,
   uint32_t                                    bindingCount,
   const VkBuffer*                             pBuffers,
   const VkDeviceSize*                         pOffsets)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;
   struct val_buffer **buffers;
   VkDeviceSize *offsets;
   int i;
   uint32_t cmd_size = bindingCount * sizeof(struct val_buffer *) + bindingCount * sizeof(VkDeviceSize);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_BIND_VERTEX_BUFFERS);
   if (!cmd)
      return;

   cmd->u.vertex_buffers.first = firstBinding;
   cmd->u.vertex_buffers.binding_count = bindingCount;

   buffers = (struct val_buffer **)(cmd + 1);
   offsets = (VkDeviceSize *)(buffers + bindingCount);
   for (i = 0; i < bindingCount; i++) {
      buffers[i] = val_buffer_from_handle(pBuffers[i]);
      offsets[i] = pOffsets[i];
   }
   cmd->u.vertex_buffers.buffers = buffers;
   cmd->u.vertex_buffers.offsets = offsets;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdBindPipeline(
   VkCommandBuffer                             commandBuffer,
   VkPipelineBindPoint                         pipelineBindPoint,
   VkPipeline                                  _pipeline)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_pipeline, pipeline, _pipeline);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_BIND_PIPELINE);
   if (!cmd)
      return;

   cmd->u.pipeline.bind_point = pipelineBindPoint;
   cmd->u.pipeline.pipeline = pipeline;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdBindDescriptorSets(
   VkCommandBuffer                             commandBuffer,
   VkPipelineBindPoint                         pipelineBindPoint,
   VkPipelineLayout                            _layout,
   uint32_t                                    firstSet,
   uint32_t                                    descriptorSetCount,
   const VkDescriptorSet*                      pDescriptorSets,
   uint32_t                                    dynamicOffsetCount,
   const uint32_t*                             pDynamicOffsets)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_pipeline_layout, layout, _layout);
   struct val_cmd_buffer_entry *cmd;
   struct val_descriptor_set **sets;
   uint32_t *offsets;
   int i;
   uint32_t cmd_size = descriptorSetCount * sizeof(struct val_descriptor_set *) + dynamicOffsetCount * sizeof(uint32_t);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_BIND_DESCRIPTOR_SETS);
   if (!cmd)
      return;

   cmd->u.descriptor_sets.bind_point = pipelineBindPoint;
   cmd->u.descriptor_sets.layout = layout;
   cmd->u.descriptor_sets.first = firstSet;
   cmd->u.descriptor_sets.count = descriptorSetCount;

   sets = (struct val_descriptor_set **)(cmd + 1);
   for (i = 0; i < descriptorSetCount; i++) {
      sets[i] = val_descriptor_set_from_handle(pDescriptorSets[i]);
   }
   cmd->u.descriptor_sets.sets = sets;

   cmd->u.descriptor_sets.dynamic_offset_count = dynamicOffsetCount;
   offsets = (uint32_t *)(sets + descriptorSetCount);
   for (i = 0; i < dynamicOffsetCount; i++)
      offsets[i] = pDynamicOffsets[i];
   cmd->u.descriptor_sets.dynamic_offsets = offsets;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdDraw(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    vertexCount,
   uint32_t                                    instanceCount,
   uint32_t                                    firstVertex,
   uint32_t                                    firstInstance)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_DRAW);
   if (!cmd)
      return;

   cmd->u.draw.vertex_count = vertexCount;
   cmd->u.draw.instance_count = instanceCount;
   cmd->u.draw.first_vertex = firstVertex;
   cmd->u.draw.first_instance = firstInstance;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdEndRenderPass(
   VkCommandBuffer                             commandBuffer)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_END_RENDER_PASS);
   if (!cmd)
      return;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetViewport(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    firstViewport,
   uint32_t                                    viewportCount,
   const VkViewport*                           pViewports)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;
   int i;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_VIEWPORT);
   if (!cmd)
      return;

   cmd->u.set_viewport.first_viewport = firstViewport;
   cmd->u.set_viewport.viewport_count = viewportCount;
   for (i = 0; i < viewportCount; i++)
      cmd->u.set_viewport.viewports[i] = pViewports[i];

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetScissor(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    firstScissor,
   uint32_t                                    scissorCount,
   const VkRect2D*                             pScissors)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;
   int i;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_SCISSOR);
   if (!cmd)
      return;

   cmd->u.set_scissor.first_scissor = firstScissor;
   cmd->u.set_scissor.scissor_count = scissorCount;
   for (i = 0; i < scissorCount; i++)
      cmd->u.set_scissor.scissors[i] = pScissors[i];

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetLineWidth(
   VkCommandBuffer                             commandBuffer,
   float                                       lineWidth)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_LINE_WIDTH);
   if (!cmd)
      return;

   cmd->u.set_line_width.line_width = lineWidth;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetDepthBias(
   VkCommandBuffer                             commandBuffer,
   float                                       depthBiasConstantFactor,
   float                                       depthBiasClamp,
   float                                       depthBiasSlopeFactor)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_DEPTH_BIAS);
   if (!cmd)
      return;

   cmd->u.set_depth_bias.constant_factor = depthBiasConstantFactor;
   cmd->u.set_depth_bias.clamp = depthBiasClamp;
   cmd->u.set_depth_bias.slope_factor = depthBiasSlopeFactor;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetBlendConstants(
   VkCommandBuffer                             commandBuffer,
   const float                                 blendConstants[4])
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_BLEND_CONSTANTS);
   if (!cmd)
      return;

   memcpy(cmd->u.set_blend_constants.blend_constants, blendConstants, 4 * sizeof(float));

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetDepthBounds(
   VkCommandBuffer                             commandBuffer,
   float                                       minDepthBounds,
   float                                       maxDepthBounds)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_DEPTH_BOUNDS);
   if (!cmd)
      return;

   cmd->u.set_depth_bounds.min_depth = minDepthBounds;
   cmd->u.set_depth_bounds.max_depth = maxDepthBounds;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetStencilCompareMask(
   VkCommandBuffer                             commandBuffer,
   VkStencilFaceFlags                          faceMask,
   uint32_t                                    compareMask)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_STENCIL_COMPARE_MASK);
   if (!cmd)
      return;

   cmd->u.stencil_vals.face_mask = faceMask;
   cmd->u.stencil_vals.value = compareMask;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetStencilWriteMask(
   VkCommandBuffer                             commandBuffer,
   VkStencilFaceFlags                          faceMask,
   uint32_t                                    writeMask)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_STENCIL_WRITE_MASK);
   if (!cmd)
      return;

   cmd->u.stencil_vals.face_mask = faceMask;
   cmd->u.stencil_vals.value = writeMask;

   cmd_buf_queue(cmd_buffer, cmd);
}


void val_CmdSetStencilReference(
   VkCommandBuffer                             commandBuffer,
   VkStencilFaceFlags                          faceMask,
   uint32_t                                    reference)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_STENCIL_REFERENCE);
   if (!cmd)
      return;

   cmd->u.stencil_vals.face_mask = faceMask;
   cmd->u.stencil_vals.value = reference;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdPushConstants(
   VkCommandBuffer                             commandBuffer,
   VkPipelineLayout                            layout,
   VkShaderStageFlags                          stageFlags,
   uint32_t                                    offset,
   uint32_t                                    size,
   const void*                                 pValues)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, (size - 4), VAL_CMD_PUSH_CONSTANTS);
   if (!cmd)
      return;

   cmd->u.push_constants.stage = stageFlags;
   cmd->u.push_constants.offset = offset;
   cmd->u.push_constants.size = size;
   memcpy(cmd->u.push_constants.val, pValues, size);

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdBindIndexBuffer(
   VkCommandBuffer                             commandBuffer,
   VkBuffer                                    _buffer,
   VkDeviceSize                                offset,
   VkIndexType                                 indexType)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_buffer, buffer, _buffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_BIND_INDEX_BUFFER);
   if (!cmd)
      return;

   cmd->u.index_buffer.buffer = buffer;
   cmd->u.index_buffer.offset = offset;
   cmd->u.index_buffer.index_type = indexType;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdDrawIndexed(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    indexCount,
   uint32_t                                    instanceCount,
   uint32_t                                    firstIndex,
   int32_t                                     vertexOffset,
   uint32_t                                    firstInstance)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_DRAW_INDEXED);
   if (!cmd)
      return;

   cmd->u.draw_indexed.index_count = indexCount;
   cmd->u.draw_indexed.instance_count = instanceCount;
   cmd->u.draw_indexed.first_index = firstIndex;
   cmd->u.draw_indexed.vertex_offset = vertexOffset;
   cmd->u.draw_indexed.first_instance = firstInstance;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdDrawIndirect(
   VkCommandBuffer                             commandBuffer,
   VkBuffer                                    _buffer,
   VkDeviceSize                                offset,
   uint32_t                                    drawCount,
   uint32_t                                    stride)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_buffer, buf, _buffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_DRAW_INDIRECT);
   if (!cmd)
      return;

   cmd->u.draw_indirect.offset = offset;
   cmd->u.draw_indirect.buffer = buf;
   cmd->u.draw_indirect.draw_count = drawCount;
   cmd->u.draw_indirect.stride = stride;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdDrawIndexedIndirect(
   VkCommandBuffer                             commandBuffer,
   VkBuffer                                    _buffer,
   VkDeviceSize                                offset,
   uint32_t                                    drawCount,
   uint32_t                                    stride)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_buffer, buf, _buffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_DRAW_INDEXED_INDIRECT);
   if (!cmd)
      return;

   cmd->u.draw_indirect.offset = offset;
   cmd->u.draw_indirect.buffer = buf;
   cmd->u.draw_indirect.draw_count = drawCount;
   cmd->u.draw_indirect.stride = stride;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdDispatch(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    x,
   uint32_t                                    y,
   uint32_t                                    z)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_DISPATCH);
   if (!cmd)
      return;

   cmd->u.dispatch.x = x;
   cmd->u.dispatch.y = y;
   cmd->u.dispatch.z = z;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdDispatchIndirect(
   VkCommandBuffer                             commandBuffer,
   VkBuffer                                    _buffer,
   VkDeviceSize                                offset)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_DISPATCH_INDIRECT);
   if (!cmd)
      return;

   cmd->u.dispatch_indirect.buffer = val_buffer_from_handle(_buffer);
   cmd->u.dispatch_indirect.offset = offset;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdExecuteCommands(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    commandBufferCount,
   const VkCommandBuffer*                      pCmdBuffers)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = commandBufferCount * sizeof(struct val_cmd_buffer *);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_EXECUTE_COMMANDS);
   if (!cmd)
      return;

   cmd->u.execute_commands.command_buffer_count = commandBufferCount;
   for (unsigned i = 0; i < commandBufferCount; i++)
      cmd->u.execute_commands.cmd_buffers[i] = val_cmd_buffer_from_handle(pCmdBuffers[i]);

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdSetEvent(VkCommandBuffer commandBuffer,
                     VkEvent _event,
                     VkPipelineStageFlags stageMask)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_event, event, _event);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_EVENT);
   if (!cmd)
      return;

   cmd->u.event_set.event = event;
   cmd->u.event_set.value = true;
   cmd->u.event_set.flush = !!(stageMask == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdResetEvent(VkCommandBuffer commandBuffer,
                       VkEvent _event,
                       VkPipelineStageFlags stageMask)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_event, event, _event);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_SET_EVENT);
   if (!cmd)
      return;

   cmd->u.event_set.event = event;
   cmd->u.event_set.value = false;
   cmd->u.event_set.flush = !!(stageMask == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

   cmd_buf_queue(cmd_buffer, cmd);

}

void val_CmdWaitEvents(VkCommandBuffer commandBuffer,
                       uint32_t eventCount,
                       const VkEvent* pEvents,
                       VkPipelineStageFlags srcStageMask,
                       VkPipelineStageFlags dstStageMask,
                       uint32_t memoryBarrierCount,
                       const VkMemoryBarrier* pMemoryBarriers,
                       uint32_t bufferMemoryBarrierCount,
                       const VkBufferMemoryBarrier* pBufferMemoryBarriers,
                       uint32_t imageMemoryBarrierCount,
                       const VkImageMemoryBarrier* pImageMemoryBarriers)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = 0;

   cmd_size += eventCount * sizeof(struct val_event *);
   cmd_size += memoryBarrierCount * sizeof(VkMemoryBarrier);
   cmd_size += bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier);
   cmd_size += imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_WAIT_EVENTS);
   if (!cmd)
      return;

   cmd->u.wait_events.src_stage_mask = srcStageMask;
   cmd->u.wait_events.dst_stage_mask = dstStageMask;
   cmd->u.wait_events.event_count = eventCount;
   cmd->u.wait_events.events = (struct val_event **)(cmd + 1);
   for (unsigned i = 0; i < eventCount; i++)
      cmd->u.wait_events.events[i] = val_event_from_handle(pEvents[i]);
   cmd->u.wait_events.memory_barrier_count = memoryBarrierCount;
   cmd->u.wait_events.buffer_memory_barrier_count = bufferMemoryBarrierCount;
   cmd->u.wait_events.image_memory_barrier_count = imageMemoryBarrierCount;

   /* TODO finish off this */
   cmd_buf_queue(cmd_buffer, cmd);
}


void val_CmdCopyBufferToImage(
   VkCommandBuffer                             commandBuffer,
   VkBuffer                                    srcBuffer,
   VkImage                                     destImage,
   VkImageLayout                               destImageLayout,
   uint32_t                                    regionCount,
   const VkBufferImageCopy*                    pRegions)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_buffer, src_buffer, srcBuffer);
   VAL_FROM_HANDLE(val_image, dst_image, destImage);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = regionCount * sizeof(VkBufferImageCopy);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_COPY_BUFFER_TO_IMAGE);
   if (!cmd)
      return;

   cmd->u.buffer_to_img.src = src_buffer;
   cmd->u.buffer_to_img.dst = dst_image;
   cmd->u.buffer_to_img.dst_layout = destImageLayout;
   cmd->u.buffer_to_img.region_count = regionCount;

   {
      VkBufferImageCopy *regions;

      regions = (VkBufferImageCopy *)(cmd + 1);
      memcpy(regions, pRegions, regionCount * sizeof(VkBufferImageCopy));
      cmd->u.buffer_to_img.regions = regions;
   }

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdCopyImageToBuffer(
   VkCommandBuffer                             commandBuffer,
   VkImage                                     srcImage,
   VkImageLayout                               srcImageLayout,
   VkBuffer                                    destBuffer,
   uint32_t                                    regionCount,
   const VkBufferImageCopy*                    pRegions)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_image, src_image, srcImage);
   VAL_FROM_HANDLE(val_buffer, dst_buffer, destBuffer);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = regionCount * sizeof(VkBufferImageCopy);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_COPY_IMAGE_TO_BUFFER);
   if (!cmd)
      return;

   cmd->u.img_to_buffer.src = src_image;
   cmd->u.img_to_buffer.dst = dst_buffer;
   cmd->u.img_to_buffer.src_layout = srcImageLayout;
   cmd->u.img_to_buffer.region_count = regionCount;

   {
      VkBufferImageCopy *regions;

      regions = (VkBufferImageCopy *)(cmd + 1);
      memcpy(regions, pRegions, regionCount * sizeof(VkBufferImageCopy));
      cmd->u.img_to_buffer.regions = regions;
   }

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdCopyImage(
   VkCommandBuffer                             commandBuffer,
   VkImage                                     srcImage,
   VkImageLayout                               srcImageLayout,
   VkImage                                     destImage,
   VkImageLayout                               destImageLayout,
   uint32_t                                    regionCount,
   const VkImageCopy*                          pRegions)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_image, src_image, srcImage);
   VAL_FROM_HANDLE(val_image, dest_image, destImage);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = regionCount * sizeof(VkImageCopy);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_COPY_IMAGE);
   if (!cmd)
      return;

   cmd->u.copy_image.src = src_image;
   cmd->u.copy_image.dst = dest_image;
   cmd->u.copy_image.src_layout = srcImageLayout;
   cmd->u.copy_image.dst_layout = destImageLayout;
   cmd->u.copy_image.region_count = regionCount;

   {
      VkImageCopy *regions;

      regions = (VkImageCopy *)(cmd + 1);
      memcpy(regions, pRegions, regionCount * sizeof(VkImageCopy));
      cmd->u.copy_image.regions = regions;
   }

   cmd_buf_queue(cmd_buffer, cmd);
}


void val_CmdCopyBuffer(
   VkCommandBuffer                             commandBuffer,
   VkBuffer                                    srcBuffer,
   VkBuffer                                    destBuffer,
   uint32_t                                    regionCount,
   const VkBufferCopy*                         pRegions)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_buffer, src_buffer, srcBuffer);
   VAL_FROM_HANDLE(val_buffer, dest_buffer, destBuffer);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = regionCount * sizeof(VkBufferCopy);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_COPY_BUFFER);
   if (!cmd)
      return;

   cmd->u.copy_buffer.src = src_buffer;
   cmd->u.copy_buffer.dst = dest_buffer;
   cmd->u.copy_buffer.region_count = regionCount;

   {
      VkBufferCopy *regions;

      regions = (VkBufferCopy *)(cmd + 1);
      memcpy(regions, pRegions, regionCount * sizeof(VkBufferCopy));
      cmd->u.copy_buffer.regions = regions;
   }

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdBlitImage(
   VkCommandBuffer                             commandBuffer,
   VkImage                                     srcImage,
   VkImageLayout                               srcImageLayout,
   VkImage                                     destImage,
   VkImageLayout                               destImageLayout,
   uint32_t                                    regionCount,
   const VkImageBlit*                          pRegions,
   VkFilter                                    filter)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_image, src_image, srcImage);
   VAL_FROM_HANDLE(val_image, dest_image, destImage);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = regionCount * sizeof(VkImageBlit);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_BLIT_IMAGE);
   if (!cmd)
      return;

   cmd->u.blit_image.src = src_image;
   cmd->u.blit_image.dst = dest_image;
   cmd->u.blit_image.src_layout = srcImageLayout;
   cmd->u.blit_image.dst_layout = destImageLayout;
   cmd->u.blit_image.filter = filter;
   cmd->u.blit_image.region_count = regionCount;

   {
      VkImageBlit *regions;

      regions = (VkImageBlit *)(cmd + 1);
      memcpy(regions, pRegions, regionCount * sizeof(VkImageBlit));
      cmd->u.blit_image.regions = regions;
   }

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdClearAttachments(
   VkCommandBuffer                             commandBuffer,
   uint32_t                                    attachmentCount,
   const VkClearAttachment*                    pAttachments,
   uint32_t                                    rectCount,
   const VkClearRect*                          pRects)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = attachmentCount * sizeof(VkClearAttachment) + rectCount * sizeof(VkClearRect);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_CLEAR_ATTACHMENTS);
   if (!cmd)
      return;

   cmd->u.clear_attachments.attachment_count = attachmentCount;
   cmd->u.clear_attachments.attachments = (VkClearAttachment *)(cmd + 1);
   for (unsigned i = 0; i < attachmentCount; i++)
      cmd->u.clear_attachments.attachments[i] = pAttachments[i];
   cmd->u.clear_attachments.rect_count = rectCount;
   cmd->u.clear_attachments.rects = (VkClearRect *)(cmd->u.clear_attachments.attachments + attachmentCount);
   for (unsigned i = 0; i < rectCount; i++)
      cmd->u.clear_attachments.rects[i] = pRects[i];

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdFillBuffer(
   VkCommandBuffer                             commandBuffer,
   VkBuffer                                    dstBuffer,
   VkDeviceSize                                dstOffset,
   VkDeviceSize                                fillSize,
   uint32_t                                    data)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_buffer, dst_buffer, dstBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_FILL_BUFFER);
   if (!cmd)
      return;

   cmd->u.fill_buffer.buffer = dst_buffer;
   cmd->u.fill_buffer.offset = dstOffset;
   cmd->u.fill_buffer.fill_size = fillSize;
   cmd->u.fill_buffer.data = data;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdUpdateBuffer(
   VkCommandBuffer                             commandBuffer,
   VkBuffer                                    dstBuffer,
   VkDeviceSize                                dstOffset,
   VkDeviceSize                                dataSize,
   const void*                                 pData)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_buffer, dst_buffer, dstBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, dataSize, VAL_CMD_UPDATE_BUFFER);
   if (!cmd)
      return;

   cmd->u.update_buffer.buffer = dst_buffer;
   cmd->u.update_buffer.offset = dstOffset;
   cmd->u.update_buffer.data_size = dataSize;
   memcpy(cmd->u.update_buffer.data, pData, dataSize);

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdClearColorImage(
   VkCommandBuffer                             commandBuffer,
   VkImage                                     image_h,
   VkImageLayout                               imageLayout,
   const VkClearColorValue*                    pColor,
   uint32_t                                    rangeCount,
   const VkImageSubresourceRange*              pRanges)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_image, image, image_h);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = rangeCount * sizeof(VkImageSubresourceRange);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_CLEAR_COLOR_IMAGE);
   if (!cmd)
      return;

   cmd->u.clear_color_image.image = image;
   cmd->u.clear_color_image.layout = imageLayout;
   cmd->u.clear_color_image.clear_val = *pColor;
   cmd->u.clear_color_image.range_count = rangeCount;
   cmd->u.clear_color_image.ranges = (VkImageSubresourceRange *)(cmd + 1);
   for (unsigned i = 0; i < rangeCount; i++)
      cmd->u.clear_color_image.ranges[i] = pRanges[i];

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdClearDepthStencilImage(
   VkCommandBuffer                             commandBuffer,
   VkImage                                     image_h,
   VkImageLayout                               imageLayout,
   const VkClearDepthStencilValue*             pDepthStencil,
   uint32_t                                    rangeCount,
   const VkImageSubresourceRange*              pRanges)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_image, image, image_h);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = rangeCount * sizeof(VkImageSubresourceRange);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_CLEAR_DEPTH_STENCIL_IMAGE);
   if (!cmd)
      return;

   cmd->u.clear_ds_image.image = image;
   cmd->u.clear_ds_image.layout = imageLayout;
   cmd->u.clear_ds_image.clear_val = *pDepthStencil;
   cmd->u.clear_ds_image.range_count = rangeCount;
   cmd->u.clear_ds_image.ranges = (VkImageSubresourceRange *)(cmd + 1);
   for (unsigned i = 0; i < rangeCount; i++)
      cmd->u.clear_ds_image.ranges[i] = pRanges[i];

   cmd_buf_queue(cmd_buffer, cmd);
}


void val_CmdResolveImage(
   VkCommandBuffer                             commandBuffer,
   VkImage                                     srcImage,
   VkImageLayout                               srcImageLayout,
   VkImage                                     destImage,
   VkImageLayout                               destImageLayout,
   uint32_t                                    regionCount,
   const VkImageResolve*                       regions)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_image, src_image, srcImage);
   VAL_FROM_HANDLE(val_image, dst_image, destImage);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = regionCount * sizeof(VkImageResolve);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_RESOLVE_IMAGE);
   if (!cmd)
      return;

   cmd->u.resolve_image.src = src_image;
   cmd->u.resolve_image.dst = dst_image;
   cmd->u.resolve_image.src_layout = srcImageLayout;
   cmd->u.resolve_image.dst_layout = destImageLayout;
   cmd->u.resolve_image.region_count = regionCount;
   cmd->u.resolve_image.regions = (VkImageResolve *)(cmd + 1);
   for (unsigned i = 0; i < regionCount; i++)
      cmd->u.resolve_image.regions[i] = regions[i];

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdResetQueryPool(
   VkCommandBuffer                             commandBuffer,
   VkQueryPool                                 queryPool,
   uint32_t                                    firstQuery,
   uint32_t                                    queryCount)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_query_pool, query_pool, queryPool);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_RESET_QUERY_POOL);
   if (!cmd)
      return;

   cmd->u.query.pool = query_pool;
   cmd->u.query.query = firstQuery;
   cmd->u.query.index = queryCount;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdBeginQueryIndexedEXT(
   VkCommandBuffer                             commandBuffer,
   VkQueryPool                                 queryPool,
   uint32_t                                    query,
   VkQueryControlFlags                         flags,
   uint32_t                                    index)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_query_pool, query_pool, queryPool);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_BEGIN_QUERY);
   if (!cmd)
      return;

   cmd->u.query.pool = query_pool;
   cmd->u.query.query = query;
   cmd->u.query.index = index;
   cmd->u.query.precise = true;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdBeginQuery(
   VkCommandBuffer                             commandBuffer,
   VkQueryPool                                 queryPool,
   uint32_t                                    query,
   VkQueryControlFlags                         flags)
{
   val_CmdBeginQueryIndexedEXT(commandBuffer, queryPool, query, flags, 0);
}

void val_CmdEndQueryIndexedEXT(
   VkCommandBuffer                             commandBuffer,
   VkQueryPool                                 queryPool,
   uint32_t                                    query,
   uint32_t                                    index)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_query_pool, query_pool, queryPool);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_END_QUERY);
   if (!cmd)
      return;

   cmd->u.query.pool = query_pool;
   cmd->u.query.query = query;
   cmd->u.query.index = index;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdEndQuery(
   VkCommandBuffer                             commandBuffer,
   VkQueryPool                                 queryPool,
   uint32_t                                    query)
{
   val_CmdEndQueryIndexedEXT(commandBuffer, queryPool, query, 0);
}

void val_CmdWriteTimestamp(
   VkCommandBuffer                             commandBuffer,
   VkPipelineStageFlagBits                     pipelineStage,
   VkQueryPool                                 queryPool,
   uint32_t                                    query)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_query_pool, query_pool, queryPool);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_WRITE_TIMESTAMP);
   if (!cmd)
      return;

   cmd->u.query.pool = query_pool;
   cmd->u.query.query = query;
   cmd->u.query.flush = !(pipelineStage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdCopyQueryPoolResults(
   VkCommandBuffer                             commandBuffer,
   VkQueryPool                                 queryPool,
   uint32_t                                    firstQuery,
   uint32_t                                    queryCount,
   VkBuffer                                    dstBuffer,
   VkDeviceSize                                dstOffset,
   VkDeviceSize                                stride,
   VkQueryResultFlags                          flags)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_query_pool, query_pool, queryPool);
   VAL_FROM_HANDLE(val_buffer, buffer, dstBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = cmd_buf_entry_alloc(cmd_buffer, VAL_CMD_COPY_QUERY_POOL_RESULTS);
   if (!cmd)
      return;

   cmd->u.copy_query_pool_results.pool = query_pool;
   cmd->u.copy_query_pool_results.first_query = firstQuery;
   cmd->u.copy_query_pool_results.query_count = queryCount;
   cmd->u.copy_query_pool_results.dst = buffer;
   cmd->u.copy_query_pool_results.dst_offset = dstOffset;
   cmd->u.copy_query_pool_results.stride = stride;
   cmd->u.copy_query_pool_results.flags = flags;

   cmd_buf_queue(cmd_buffer, cmd);
}

void val_CmdPipelineBarrier(
   VkCommandBuffer                             commandBuffer,
   VkPipelineStageFlags                        srcStageMask,
   VkPipelineStageFlags                        destStageMask,
   VkBool32                                    byRegion,
   uint32_t                                    memoryBarrierCount,
   const VkMemoryBarrier*                      pMemoryBarriers,
   uint32_t                                    bufferMemoryBarrierCount,
   const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
   uint32_t                                    imageMemoryBarrierCount,
   const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;
   uint32_t cmd_size = 0;

   cmd_size += memoryBarrierCount * sizeof(VkMemoryBarrier);
   cmd_size += bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier);
   cmd_size += imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier);

   cmd = cmd_buf_entry_alloc_size(cmd_buffer, cmd_size, VAL_CMD_PIPELINE_BARRIER);
   if (!cmd)
      return;

   cmd->u.pipeline_barrier.src_stage_mask = srcStageMask;
   cmd->u.pipeline_barrier.dst_stage_mask = destStageMask;
   cmd->u.pipeline_barrier.by_region = byRegion;
   cmd->u.pipeline_barrier.memory_barrier_count = memoryBarrierCount;
   cmd->u.pipeline_barrier.buffer_memory_barrier_count = bufferMemoryBarrierCount;
   cmd->u.pipeline_barrier.image_memory_barrier_count = imageMemoryBarrierCount;

   /* TODO finish off this */
   cmd_buf_queue(cmd_buffer, cmd);
}

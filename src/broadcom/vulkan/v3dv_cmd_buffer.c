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

#include "v3dv_private.h"
#include "broadcom/cle/v3dx_pack.h"
#include "util/u_pack_color.h"

const struct v3dv_dynamic_state default_dynamic_state = {
   .viewport = {
      .count = 0,
   },
   .scissor = {
      .count = 0,
   },
};

void
v3dv_job_add_bo(struct v3dv_job *job, struct v3dv_bo *bo)
{
   if (!bo)
      return;

   if (_mesa_set_search(job->bos, bo))
      return;

   _mesa_set_add(job->bos, bo);
   job->bo_count++;
}

static void
subpass_start(struct v3dv_cmd_buffer *cmd_buffer);

static void
subpass_finish(struct v3dv_cmd_buffer *cmd_buffer);

VkResult
v3dv_CreateCommandPool(VkDevice _device,
                       const VkCommandPoolCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkCommandPool *pCmdPool)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_cmd_pool *pool;

   /* We only support one queue */
   assert(pCreateInfo->queueFamilyIndex == 0);

   pool = vk_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (pAllocator)
      pool->alloc = *pAllocator;
   else
      pool->alloc = device->alloc;

   list_inithead(&pool->cmd_buffers);

   *pCmdPool = v3dv_cmd_pool_to_handle(pool);

   return VK_SUCCESS;
}

static VkResult
cmd_buffer_create(struct v3dv_device *device,
                  struct v3dv_cmd_pool *pool,
                  VkCommandBufferLevel level,
                  VkCommandBuffer *pCommandBuffer)
{
   struct v3dv_cmd_buffer *cmd_buffer;
   cmd_buffer = vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   cmd_buffer->device = device;
   cmd_buffer->pool = pool;
   cmd_buffer->level = level;
   cmd_buffer->usage_flags = 0;

   list_inithead(&cmd_buffer->submit_jobs);

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_NEW;

   assert(pool);
   list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

   *pCommandBuffer = v3dv_cmd_buffer_to_handle(cmd_buffer);

   return VK_SUCCESS;
}

static void
job_destroy(struct v3dv_job *job)
{
   assert(job);

   list_del(&job->list_link);

   v3dv_cl_destroy(&job->bcl);
   v3dv_cl_destroy(&job->rcl);
   v3dv_cl_destroy(&job->indirect);

   /* Since we don't ref BOs, when we add them to the command buffer, don't
    * unref them here either.
    */
#if 0
   set_foreach(job->bos, entry) {
      struct v3dv_bo *bo = (struct v3dv_bo *)entry->key;
      v3dv_bo_free(cmd_buffer->device, bo);
   }
#endif
   _mesa_set_destroy(job->bos, NULL);

   v3dv_bo_free(job->cmd_buffer->device, job->tile_alloc);
   v3dv_bo_free(job->cmd_buffer->device, job->tile_state);
}

static void
cmd_buffer_destroy(struct v3dv_cmd_buffer *cmd_buffer)
{
   list_del(&cmd_buffer->pool_link);

   list_for_each_entry_safe(struct v3dv_job, job,
                            &cmd_buffer->submit_jobs, list_link) {
      job_destroy(job);
   }

   if (cmd_buffer->state.job)
      job_destroy(cmd_buffer->state.job);

   if (cmd_buffer->state.attachments) {
      assert(cmd_buffer->state.attachment_count > 0);
      vk_free(&cmd_buffer->pool->alloc, cmd_buffer->state.attachments);
   }

   vk_free(&cmd_buffer->pool->alloc, cmd_buffer);
}

static void
emit_binning_flush(struct v3dv_job *job)
{
   assert(job);
   v3dv_cl_ensure_space_with_branch(&job->bcl, cl_packet_length(FLUSH));
   cl_emit(&job->bcl, FLUSH, flush);
}

void
v3dv_cmd_buffer_finish_job(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);
   assert(v3dv_cl_offset(&job->bcl) != 0);

   list_addtail(&job->list_link, &cmd_buffer->submit_jobs);
   cmd_buffer->state.job = NULL;
}

struct v3dv_job *
v3dv_cmd_buffer_start_job(struct v3dv_cmd_buffer *cmd_buffer)
{
   /* Ensure we are not starting a new job without finishing a previous one */
   if (cmd_buffer->state.job != NULL)
      v3dv_cmd_buffer_finish_job(cmd_buffer);

   assert(cmd_buffer->state.job == NULL);
   struct v3dv_job *job = vk_zalloc(&cmd_buffer->device->alloc,
                                    sizeof(struct v3dv_job), 8,
                                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   assert(job);

   job->cmd_buffer = cmd_buffer;

   job->bos =
      _mesa_set_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);
   job->bo_count = 0;

   v3dv_cl_init(job, &job->bcl);
   v3dv_cl_begin(&job->bcl);

   v3dv_cl_init(job, &job->rcl);
   v3dv_cl_begin(&job->rcl);

   v3dv_cl_init(job, &job->indirect);
   v3dv_cl_begin(&job->indirect);

   cmd_buffer->state.job = job;
   return job;
}

static VkResult
cmd_buffer_reset(struct v3dv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->status != V3DV_CMD_BUFFER_STATUS_INITIALIZED) {
      /* FIXME */
      assert(cmd_buffer->status == V3DV_CMD_BUFFER_STATUS_NEW);

      cmd_buffer->usage_flags = 0;

      struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
      state->pass = NULL;
      state->framebuffer = NULL;
      state->subpass_idx = 0;
      state->job = NULL;

      cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_INITIALIZED;
   }
   return VK_SUCCESS;
}

VkResult
v3dv_AllocateCommandBuffers(VkDevice _device,
                            const VkCommandBufferAllocateInfo *pAllocateInfo,
                            VkCommandBuffer *pCommandBuffers)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_cmd_pool, pool, pAllocateInfo->commandPool);

   /* FIXME: implement secondary command buffers */
   assert(pAllocateInfo->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      result = cmd_buffer_create(device, pool, pAllocateInfo->level,
                                 &pCommandBuffers[i]);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      v3dv_FreeCommandBuffers(_device, pAllocateInfo->commandPool,
                              i, pCommandBuffers);
      for (i = 0; i < pAllocateInfo->commandBufferCount; i++)
         pCommandBuffers[i] = VK_NULL_HANDLE;
   }

   return result;
}

void
v3dv_FreeCommandBuffers(VkDevice device,
                        VkCommandPool commandPool,
                        uint32_t commandBufferCount,
                        const VkCommandBuffer *pCommandBuffers)
{
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

      if (!cmd_buffer)
         continue;

      cmd_buffer_destroy(cmd_buffer);
   }
}

void
v3dv_DestroyCommandPool(VkDevice _device,
                        VkCommandPool commandPool,
                        const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct v3dv_cmd_buffer, cmd_buffer,
                            &pool->cmd_buffers, pool_link) {
      cmd_buffer_destroy(cmd_buffer);
   }

   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult
v3dv_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                        const VkCommandBufferBeginInfo *pBeginInfo)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
          !(cmd_buffer->usage_flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT));

   /* If this is the first vkBeginCommandBuffer, we must initialize the
    * command buffer's state. Otherwise, we must reset its state. In both
    * cases we reset it.
    */
   VkResult result = cmd_buffer_reset(cmd_buffer);
   if (result != VK_SUCCESS)
      return result;

   assert(cmd_buffer->status == V3DV_CMD_BUFFER_STATUS_INITIALIZED);

   cmd_buffer->usage_flags = pBeginInfo->flags;

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_RECORDING;

   return VK_SUCCESS;
}

static void
emit_clip_window(struct v3dv_job *job, const VkRect2D *rect)
{
   assert(job);
   cl_emit(&job->bcl, CLIP_WINDOW, clip) {
      clip.clip_window_left_pixel_coordinate = rect->offset.x;
      clip.clip_window_bottom_pixel_coordinate = rect->offset.y;
      clip.clip_window_width_in_pixels = rect->extent.width;
      clip.clip_window_height_in_pixels = rect->extent.height;
   }
}

static void
cmd_buffer_state_set_attachment_clear_color(struct v3dv_cmd_buffer *cmd_buffer,
                                            uint32_t attachment_idx,
                                            const VkClearColorValue *color)
{
   assert(attachment_idx < cmd_buffer->state.pass->attachment_count);

   const struct v3dv_render_pass_attachment *attachment =
      &cmd_buffer->state.pass->attachments[attachment_idx];

   uint32_t internal_type, internal_bpp;
   const struct v3dv_format *format = v3dv_get_format(attachment->desc.format);
   v3dv_get_internal_type_bpp_for_output_format(format->rt_type,
                                                &internal_type,
                                                &internal_bpp);

   uint32_t internal_size = 4 << internal_bpp;

   struct v3dv_cmd_buffer_attachment_state *attachment_state =
      &cmd_buffer->state.attachments[attachment_idx];

   uint32_t *hw_color =  &attachment_state->clear_value.color[0];

   union util_color uc;
   switch (internal_type) {
   case V3D_INTERNAL_TYPE_8:
      util_pack_color(color->float32, PIPE_FORMAT_R8G8B8A8_UNORM, &uc);
      memcpy(hw_color, uc.ui, internal_size);
   break;
   case V3D_INTERNAL_TYPE_8I:
   case V3D_INTERNAL_TYPE_8UI:
      hw_color[0] = ((color->uint32[0] & 0xff) |
                     (color->uint32[1] & 0xff) << 8 |
                     (color->uint32[2] & 0xff) << 16 |
                     (color->uint32[3] & 0xff) << 24);
   break;
   case V3D_INTERNAL_TYPE_16F:
      util_pack_color(color->float32, PIPE_FORMAT_R16G16B16A16_FLOAT, &uc);
      memcpy(hw_color, uc.ui, internal_size);
   break;
   case V3D_INTERNAL_TYPE_16I:
   case V3D_INTERNAL_TYPE_16UI:
      hw_color[0] = ((color->uint32[0] & 0xffff) | color->uint32[1] << 16);
      hw_color[1] = ((color->uint32[2] & 0xffff) | color->uint32[3] << 16);
   break;
   case V3D_INTERNAL_TYPE_32F:
   case V3D_INTERNAL_TYPE_32I:
   case V3D_INTERNAL_TYPE_32UI:
      memcpy(hw_color, color->uint32, internal_size);
      break;
   }
}

static void
cmd_buffer_state_set_clear_values(struct v3dv_cmd_buffer *cmd_buffer,
                                  uint32_t count, const VkClearValue *values)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_render_pass *pass = state->pass;
   assert(count <= pass->attachment_count);

   for (uint32_t i = 0; i < count; i++) {
      const struct v3dv_render_pass_attachment *attachment =
         &pass->attachments[i];

      if (attachment->desc.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      /* FIXME: support depth/stencil */
      cmd_buffer_state_set_attachment_clear_color(cmd_buffer, i,
                                                  &values[i].color);
   }
}

/* Identifies the first subpass that uses each attachment in the render pass */
static void
cmd_buffer_find_first_subpass_for_attachments(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_render_pass *pass = state->pass;

   for (uint32_t i = 0; i < pass->attachment_count; i++)
      state->attachments[i].first_subpass = pass->subpass_count - 1;

   for (uint32_t i = 0; i < pass->subpass_count - 1; i++) {
      const struct v3dv_subpass *subpass = &pass->subpasses[i];
      for (uint32_t j = 0; j < subpass->color_count; j++) {
         uint32_t attachment_idx = subpass->color_attachments[j].attachment;
         if (j < state->attachments[attachment_idx].first_subpass)
            state->attachments[attachment_idx].first_subpass = j;
      }
   }
}

static void
cmd_buffer_init_render_pass_attachment_state(struct v3dv_cmd_buffer *cmd_buffer,
                                             const VkRenderPassBeginInfo *pRenderPassBegin)
{
   cmd_buffer_find_first_subpass_for_attachments(cmd_buffer);

   cmd_buffer_state_set_clear_values(cmd_buffer,
                                     pRenderPassBegin->clearValueCount,
                                     pRenderPassBegin->pClearValues);
}

static void
cmd_buffer_ensure_render_pass_attachment_state(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_render_pass *pass = state->pass;

   if (state->attachment_count < pass->attachment_count) {
      if (state->attachment_count > 0)
         vk_free(&cmd_buffer->device->alloc, state->attachments);

      uint32_t size = sizeof(struct v3dv_cmd_buffer_attachment_state) *
                      pass->attachment_count;
      state->attachments = vk_zalloc(&cmd_buffer->device->alloc, size, 8,
                                     VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      state->attachment_count = pass->attachment_count;
   }

   assert(state->attachment_count >= pass->attachment_count);
}

void
v3dv_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                        const VkRenderPassBeginInfo *pRenderPassBegin,
                        VkSubpassContents contents)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_render_pass, pass, pRenderPassBegin->renderPass);
   V3DV_FROM_HANDLE(v3dv_framebuffer, framebuffer, pRenderPassBegin->framebuffer);

   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   state->pass = pass;
   state->framebuffer = framebuffer;

   cmd_buffer_ensure_render_pass_attachment_state(cmd_buffer);
   cmd_buffer_init_render_pass_attachment_state(cmd_buffer, pRenderPassBegin);

   /* FIXME: probably need to align the render area to tile boundaries since
    *        the tile clears will render full tiles anyway.
    *        See vkGetRenderAreaGranularity().
    */
   state->render_area = pRenderPassBegin->renderArea;

   /* Setup for first subpass */
   state->subpass_idx = 0;
   subpass_start(cmd_buffer);
}

void
v3dv_CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   assert(state->subpass_idx < state->pass->subpass_count - 1);

   /* Finish the previous subpass */
   subpass_finish(cmd_buffer);

   /* Start the next subpass */
   state->subpass_idx++;
   subpass_start(cmd_buffer);
}

static void
setup_render_target(struct v3dv_cmd_buffer *cmd_buffer, int rt,
                    uint32_t *rt_bpp, uint32_t *rt_type, uint32_t *rt_clamp)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;

   assert(state->subpass_idx < state->pass->subpass_count);
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   if (rt >= subpass->color_count)
      return;

   struct v3dv_subpass_attachment *attachment = &subpass->color_attachments[rt];
   const uint32_t attachment_idx = attachment->attachment;
   if (attachment_idx == VK_ATTACHMENT_UNUSED)
      return;

   const struct v3dv_framebuffer *framebuffer = state->framebuffer;
   assert(attachment_idx < framebuffer->attachment_count);
   struct v3dv_image_view *iview = framebuffer->attachments[attachment_idx];

   *rt_bpp = iview->internal_bpp;
   *rt_type = iview->internal_type;
   *rt_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
}

static void
load_general(struct v3dv_cmd_buffer *cmd_buffer,
             struct v3dv_cl *cl,
             struct v3dv_image_view *iview,
             uint32_t layer,
             uint32_t buffer)
{
   const struct v3dv_image *image = iview->image;
   uint32_t layer_offset = v3dv_layer_offset(image,
                                             iview->base_level,
                                             iview->first_layer + layer);

   cl_emit(cl, LOAD_TILE_BUFFER_GENERAL, load) {
      load.buffer_to_load = buffer;
      load.address = v3dv_cl_address(image->mem->bo, layer_offset);

      load.input_image_format = iview->format->rt_type;
      load.r_b_swap = iview->swap_rb;
      load.memory_format = iview->tiling;

      const struct v3d_resource_slice *slice = &image->slices[iview->base_level];
      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         load.height_in_ub_or_stride =
            slice->padded_height_of_output_image_in_uif_blocks;
      } else if (slice->tiling == VC5_TILING_RASTER) {
         load.height_in_ub_or_stride = slice->stride;
      }

      if (image->samples > VK_SAMPLE_COUNT_1_BIT)
         load.decimate_mode = V3D_DECIMATE_MODE_ALL_SAMPLES;
      else
         load.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }
}

static void
emit_loads(struct v3dv_cmd_buffer *cmd_buffer,
           struct v3dv_cl *cl,
           uint32_t layer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t attachment_idx = subpass->color_attachments[i].attachment;

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      const struct v3dv_render_pass_attachment *attachment =
         &state->pass->attachments[attachment_idx];

      const struct v3dv_cmd_buffer_attachment_state *attachment_state =
         &state->attachments[attachment_idx];

      /* According to the Vulkan spec:
       *
       *    "The load operation for each sample in an attachment happens before
       *     any recorded command which accesses the sample in the first subpass
       *     where the attachment is used."
       *
       * If the load operation is CLEAR, we must only clear once on the first
       * subpass that uses the attachment (and in that case we don't LOAD).
       * After that, we always want to load so we don't lose any rendering done
       * by a previous subpass to the same attachment.
       */
      bool needs_load =
         attachment->desc.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ||
         (attachment->desc.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
          state->subpass_idx > attachment_state->first_subpass);

      if (needs_load) {
         struct v3dv_image_view *iview = framebuffer->attachments[attachment_idx];
         load_general(cmd_buffer, cl, iview, layer, RENDER_TARGET_0 + i);
      }
   }

   cl_emit(cl, END_OF_LOADS, end);
}

static void
store_general(struct v3dv_cmd_buffer *cmd_buffer,
              struct v3dv_cl *cl,
              uint32_t attachment_idx,
              uint32_t layer,
              uint32_t buffer,
              bool clear)
{
   const struct v3dv_image_view *iview =
      cmd_buffer->state.framebuffer->attachments[attachment_idx];
   const struct v3dv_image *image = iview->image;
   uint32_t layer_offset = v3dv_layer_offset(image,
                                             iview->base_level,
                                             iview->first_layer + layer);

   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = buffer;
      store.address = v3dv_cl_address(image->mem->bo, layer_offset);
      store.clear_buffer_being_stored = clear;

      store.output_image_format = iview->format->rt_type;
      store.r_b_swap = iview->swap_rb;
      store.memory_format = iview->tiling;

      const struct v3d_resource_slice *slice = &image->slices[iview->base_level];
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
emit_stores(struct v3dv_cmd_buffer *cmd_buffer,
            struct v3dv_cl *cl,
            uint32_t layer)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   bool has_stores = false;
   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t attachment_idx = subpass->color_attachments[i].attachment;

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      const struct v3dv_render_pass_attachment *attachment =
         &state->pass->attachments[attachment_idx];

      if (attachment->desc.storeOp != VK_ATTACHMENT_STORE_OP_STORE)
         continue;

      /* Only clear once on the first subpass that uses the attachment */
      bool needs_clear =
         attachment->desc.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
         state->attachments[attachment_idx].first_subpass == state->subpass_idx;
      store_general(cmd_buffer, cl,
                    attachment_idx, layer, RENDER_TARGET_0 + i, needs_clear);
      has_stores = true;
   }

   /* FIXME: depth/stencil store
    *
    * GFXH-1461/GFXH-1689: The per-buffer store command's clear
    * buffer bit is broken for depth/stencil.  In addition, the
    * clear packet's Z/S bit is broken, but the RTs bit ends up
    * clearing Z/S.
    *
    * This means that when we implement depth/stencil clearing we
    * need to emit a separate clear before we start the render pass,
    * since the RTs bit is for clearing all render targets, and we might
    * not want to do that. We might want to consider emitting clears for
    * all RTs needing clearing just once ahead of the first subpass.
    */

   /* We always need to emit at least one dummy store */
   if (!has_stores) {
      cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
   }
}

static void
emit_generic_per_tile_list(struct v3dv_cmd_buffer *cmd_buffer, uint32_t layer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* Emit the generic list in our indirect state -- the rcl will just
    * have pointers into it.
    */
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   emit_loads(cmd_buffer, cl, layer);

   /* The binner starts out writing tiles assuming that the initial mode
    * is triangles, so make sure that's the case.
    */
   cl_emit(cl, PRIM_LIST_FORMAT, fmt) {
      fmt.primitive_type = LIST_TRIANGLES;
   }

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   emit_stores(cmd_buffer, cl, layer);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_render_layer(struct v3dv_cmd_buffer *cmd_buffer, uint32_t layer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;

   struct v3dv_job *job = cmd_buffer->state.job;
   struct v3dv_cl *rcl = &job->rcl;

   /* If doing multicore binning, we would need to initialize each
    * core's tile list here.
    */
   const uint32_t tile_alloc_offset =
      64 * layer * framebuffer->draw_tiles_x * framebuffer->draw_tiles_y;
   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(job->tile_alloc, tile_alloc_offset);
   }

   cl_emit(rcl, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
      config.number_of_bin_tile_lists = 1;
      config.total_frame_width_in_tiles = framebuffer->draw_tiles_x;
      config.total_frame_height_in_tiles = framebuffer->draw_tiles_y;

      config.supertile_width_in_tiles = framebuffer->supertile_width;
      config.supertile_height_in_tiles = framebuffer->supertile_height;

      config.total_frame_width_in_supertiles =
         framebuffer->frame_width_in_supertiles;
      config.total_frame_height_in_supertiles =
         framebuffer->frame_height_in_supertiles;
   }

   /* Start by clearing the tile buffer. */
   cl_emit(rcl, TILE_COORDINATES, coords) {
      coords.tile_column_number = 0;
      coords.tile_row_number = 0;
   }

   /* Emit an initial clear of the tile buffers. This is necessary
    * for any buffers that should be cleared (since clearing
    * normally happens at the *end* of the generic tile list), but
    * it's also nice to clear everything so the first tile doesn't
    * inherit any contents from some previous frame.
    *
    * Also, implement the GFXH-1742 workaround. There's a race in
    * the HW between the RCL updating the TLB's internal type/size
    * and the spawning of the QPU instances using the TLB's current
    * internal type/size. To make sure the QPUs get the right
    * state, we need 1 dummy store in between internal type/size
    * changes on V3D 3.x, and 2 dummy stores on 4.x.
    */
   for (int i = 0; i < 2; i++) {
      if (i > 0)
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

   emit_generic_per_tile_list(cmd_buffer, layer);

   uint32_t supertile_w_in_pixels =
      framebuffer->tile_width * framebuffer->supertile_width;
   uint32_t supertile_h_in_pixels =
      framebuffer->tile_height * framebuffer->supertile_height;
   const uint32_t min_x_supertile =
      state->render_area.offset.x / supertile_w_in_pixels;
   const uint32_t min_y_supertile =
      state->render_area.offset.y / supertile_h_in_pixels;

   const uint32_t max_render_x =
      state->render_area.offset.x + state->render_area.extent.width - 1;
   const uint32_t max_render_y =
      state->render_area.offset.y + state->render_area.extent.height - 1;
   const uint32_t max_x_supertile = max_render_x / supertile_w_in_pixels;
   const uint32_t max_y_supertile = max_render_y / supertile_h_in_pixels;

   for (int y = min_y_supertile; y <= max_y_supertile; y++) {
      for (int x = min_x_supertile; x <= max_x_supertile; x++) {
         cl_emit(rcl, SUPERTILE_COORDINATES, coords) {
            coords.column_number_in_supertiles = x;
            coords.row_number_in_supertiles = y;
         }
      }
   }
}

static void
emit_rcl(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* FIXME */
   const uint32_t fb_layers = 1;

   v3dv_cl_ensure_space_with_branch(&job->rcl, 200 +
                                    MAX2(fb_layers, 1) * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;

   assert(state->subpass_idx < state->pass->subpass_count);
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   struct v3dv_cl *rcl = &job->rcl;

   /* Comon config must be the first TILE_RENDERING_MODE_CFG and
    * Z_STENCIL_CLEAR_VALUES must be last. The ones in between are optional
    * updates to the previous HW state.
    */
   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.early_z_disable = true; /* FIXME */
      config.image_width_pixels = framebuffer->width;
      config.image_height_pixels = framebuffer->height;
      config.number_of_render_targets = MAX2(subpass->color_count, 1);
      config.multisample_mode_4x = false; /* FIXME */
      config.maximum_bpp_of_all_render_targets = framebuffer->internal_bpp;
   }

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t attachment_idx = subpass->color_attachments[i].attachment;
      struct v3dv_image_view *iview =
         state->framebuffer->attachments[attachment_idx];

      const uint32_t *clear_color =
         &state->attachments[attachment_idx].clear_value.color[0];

      uint32_t clear_pad = 0;
      if (iview->tiling == VC5_TILING_UIF_NO_XOR ||
          iview->tiling == VC5_TILING_UIF_XOR) {
         const struct v3dv_image *image = iview->image;
         const struct v3d_resource_slice *slice =
            &image->slices[iview->base_level];

         int uif_block_height = v3d_utile_height(image->cpp) * 2;

         uint32_t implicit_padded_height =
            align(framebuffer->height, uif_block_height) / uif_block_height;

         if (slice->padded_height_of_output_image_in_uif_blocks -
             implicit_padded_height >= 15) {
            clear_pad = slice->padded_height_of_output_image_in_uif_blocks;
         }
      }

      /* FIXME: the tile buffer clears don't seem to honor the scissor rect
       * so if the current combination of scissor + renderArea doesn't cover
       * the full extent of the render target we won't get correct behavior.
       * We probably need to detect these cases, implement the clearing by
       * drawing a rect and skip clearing here.
       */
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
      setup_render_target(cmd_buffer, 0,
                          &rt.render_target_0_internal_bpp,
                          &rt.render_target_0_internal_type,
                          &rt.render_target_0_clamp);
      setup_render_target(cmd_buffer, 1,
                          &rt.render_target_1_internal_bpp,
                          &rt.render_target_1_internal_type,
                          &rt.render_target_1_clamp);
      setup_render_target(cmd_buffer, 2,
                          &rt.render_target_2_internal_bpp,
                          &rt.render_target_2_internal_type,
                          &rt.render_target_2_clamp);
      setup_render_target(cmd_buffer, 3,
                          &rt.render_target_3_internal_bpp,
                          &rt.render_target_3_internal_type,
                          &rt.render_target_3_clamp);
   }

   /* Ends rendering mode config. */
   cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
      clear.z_clear_value = 0; /* FIXME */
      clear.stencil_clear_value = 0; /* FIXME */
   };

   /* Always set initial block size before the first branch, which needs
    * to match the value from binning mode config.
    */
   cl_emit(rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
      init.use_auto_chained_tile_lists = true;
      init.size_of_first_block_in_chained_tile_lists =
         TILE_ALLOCATION_BLOCK_SIZE_64B;
   }

   for (int layer = 0; layer < MAX2(1, fb_layers); layer++)
      emit_render_layer(cmd_buffer, layer);

   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
subpass_start(struct v3dv_cmd_buffer *cmd_buffer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;

   assert(state->subpass_idx < state->pass->subpass_count);

   /* FIXME: for now, each subpass goes into a separate job. In the future we
    * might be able to merge subpasses that render to the same render targets
    * so long as they don't render to more than 4 color attachments and there
    * aren't other subpass dependencies preveting this.
    */
   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer);

   const struct v3dv_framebuffer *framebuffer = cmd_buffer->state.framebuffer;

   /* Setup binning for this subpass.
    *
    * FIXME: For now we do this at the start each subpass but if we implement
    * subpass merges in the future we would only want to emit this once per job.
    */
   v3dv_cl_ensure_space_with_branch(&job->bcl, 256);

   /* The PTB will request the tile alloc initial size per tile at start
    * of tile binning.
    */
   const uint32_t fb_layers = 1; /* FIXME */
   uint32_t tile_alloc_size = 64 * MAX2(fb_layers, 1) *
                              framebuffer->draw_tiles_x *
                              framebuffer->draw_tiles_y;

   /* The PTB allocates in aligned 4k chunks after the initial setup. */
   tile_alloc_size = align(tile_alloc_size, 4096);

   /* Include the first two chunk allocations that the PTB does so that
    * we definitely clear the OOM condition before triggering one (the HW
    * won't trigger OOM during the first allocations).
    */
   tile_alloc_size += 8192;

   /* For performance, allocate some extra initial memory after the PTB's
    * minimal allocations, so that we hopefully don't have to block the
    * GPU on the kernel handling an OOM signal.
    */
   tile_alloc_size += 512 * 1024;

   job->tile_alloc = v3dv_bo_alloc(cmd_buffer->device, tile_alloc_size);
   v3dv_job_add_bo(job, job->tile_alloc);

   const uint32_t tsda_per_tile_size = 256;
   const uint32_t tile_state_size = MAX2(fb_layers, 1) *
                                    framebuffer->draw_tiles_x *
                                    framebuffer->draw_tiles_y *
                                    tsda_per_tile_size;
   job->tile_state = v3dv_bo_alloc(cmd_buffer->device, tile_state_size);
   v3dv_job_add_bo(job, job->tile_state);

   /* This must go before the binning mode configuration. It is
    * required for layered framebuffers to work.
    */
   if (fb_layers > 0) {
      cl_emit(&job->bcl, NUMBER_OF_LAYERS, config) {
         config.number_of_layers = fb_layers;
      }
   }

   cl_emit(&job->bcl, TILE_BINNING_MODE_CFG, config) {
      config.width_in_pixels = framebuffer->width;
      config.height_in_pixels = framebuffer->height;
      config.number_of_render_targets = MAX2(framebuffer->attachment_count, 1);
      config.multisample_mode_4x = false; /* FIXME */
      config.maximum_bpp_of_all_render_targets = framebuffer->internal_bpp;
   }

   /* There's definitely nothing in the VCD cache we want. */
   cl_emit(&job->bcl, FLUSH_VCD_CACHE, bin);

   /* Disable any leftover OQ state from another job. */
   cl_emit(&job->bcl, OCCLUSION_QUERY_COUNTER, counter);

   /* "Binning mode lists must have a Start Tile Binning item (6) after
    *  any prefix state data before the binning list proper starts."
    */
   cl_emit(&job->bcl, START_TILE_BINNING, bin);

   /* If we don't have a scissor or viewport defined let's just use the render
    * area as clip_window, as that would be required for a clear in any
    * case. If we have that, it would be emitted as part of the pipeline
    * dynamic state flush
    *
    * FIXME: this is mostly just needed for clear. radv has dedicated paths
    * for them, so we could get that idea. In any case, need to revisit if
    * this is the place to emit the clip window.
    */
   if (cmd_buffer->state.dynamic.scissor.count == 0 &&
       cmd_buffer->state.dynamic.viewport.count == 0) {
      emit_clip_window(job, &state->render_area);
   }

   /* FIXME: is here the best moment to do that? or when drawing? */
   if (cmd_buffer->state.pipeline) {
      struct v3dv_pipeline *pipeline = cmd_buffer->state.pipeline;

      if (pipeline->vs->assembly_bo)
         v3dv_job_add_bo(cmd_buffer->state.job, pipeline->vs->assembly_bo);
      if (pipeline->vs_bin->assembly_bo)
         v3dv_job_add_bo(cmd_buffer->state.job, pipeline->vs_bin->assembly_bo);
      if (pipeline->fs->assembly_bo)
         v3dv_job_add_bo(cmd_buffer->state.job, pipeline->fs->assembly_bo);
   }

}

static void
subpass_finish(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   emit_rcl(cmd_buffer);

   /* This finishes the a binning job.
    *
    * FIXME: if the next subpass draws to the same RTs, we could skip this
    * and the binning setup for the next subpass.
    */
   emit_binning_flush(job);
   v3dv_cmd_buffer_finish_job(cmd_buffer);
}

void
v3dv_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   /* Emit last subpass */
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   assert(state->subpass_idx == state->pass->subpass_count - 1);
   subpass_finish(cmd_buffer);

   /* We are no longer inside a render pass */
   state->pass = NULL;
   state->framebuffer = NULL;
}

VkResult
v3dv_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_EXECUTABLE;

   struct v3dv_job *job = cmd_buffer->state.job;
   if (!job)
      return VK_SUCCESS;

   /* We get here if we recorded commands after the last render pass in the
    * command buffer. Make sure we finish this last job. */
   assert(v3dv_cl_offset(&job->bcl) != 0);
   v3dv_cmd_buffer_finish_job(cmd_buffer);

   return VK_SUCCESS;
}

static void
bind_dynamic_state(struct v3dv_cmd_buffer *cmd_buffer,
                   const struct v3dv_dynamic_state *src)
{
   struct v3dv_dynamic_state *dest = &cmd_buffer->state.dynamic;
   uint32_t copy_mask = src->mask;
   uint32_t dest_mask = 0;

   /* See note on SetViewport. We follow radv approach to only allow to set
    * the number of viewports/scissors at pipeline creation time.
    */
   dest->viewport.count = src->viewport.count;
   dest->scissor.count = src->scissor.count;

   if (copy_mask & V3DV_DYNAMIC_VIEWPORT) {
      if (memcmp(&dest->viewport.viewports, &src->viewport.viewports,
                 src->viewport.count * sizeof(VkViewport))) {
         typed_memcpy(dest->viewport.viewports,
                      src->viewport.viewports,
                      src->viewport.count);
         dest_mask |= V3DV_DYNAMIC_VIEWPORT;
      }
   }

   if (copy_mask & V3DV_DYNAMIC_SCISSOR) {
      if (memcmp(&dest->scissor.scissors, &src->scissor.scissors,
                 src->scissor.count * sizeof(VkRect2D))) {
         typed_memcpy(dest->scissor.scissors,
                      src->scissor.scissors, src->scissor.count);
         dest_mask |= V3DV_DYNAMIC_SCISSOR;
      }
   }

   cmd_buffer->state.dirty |= dest_mask;
}


void
v3dv_CmdBindPipeline(VkCommandBuffer commandBuffer,
                     VkPipelineBindPoint pipelineBindPoint,
                     VkPipeline _pipeline)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_pipeline, pipeline, _pipeline);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      assert(!"VK_PIPELINE_BIND_POINT_COMPUTE not supported yet");
      break;

   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      if (cmd_buffer->state.pipeline == pipeline)
         return;

      cmd_buffer->state.pipeline = pipeline;
      bind_dynamic_state(cmd_buffer, &pipeline->dynamic_state);

      cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_PIPELINE;
      break;

   default:
      assert(!"invalid bind point");
      break;
   }
}

/* FIXME: C&P from radv. tu has similar code. Perhaps common place? */
static void
get_viewport_xform(const VkViewport *viewport,
                   float scale[3],
                   float translate[3])
{
   float x = viewport->x;
   float y = viewport->y;
   float half_width = 0.5f * viewport->width;
   float half_height = 0.5f * viewport->height;
   double n = viewport->minDepth;
   double f = viewport->maxDepth;

   scale[0] = half_width;
   translate[0] = half_width + x;
   scale[1] = half_height;
   translate[1] = half_height + y;

   scale[2] = (f - n);
   translate[2] = n;
}

void
v3dv_CmdSetViewport(VkCommandBuffer commandBuffer,
                    uint32_t firstViewport,
                    uint32_t viewportCount,
                    const VkViewport *pViewports)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const uint32_t total_count = firstViewport + viewportCount;

   assert(firstViewport < MAX_VIEWPORTS);
   assert(total_count >= 1 && total_count <= MAX_VIEWPORTS);

   /* anv allows CmdSetViewPort to change how many viewports are being used,
    * while radv not, using the value set on the pipeline creation. spec
    * doesn't specify, but radv approach makes more sense, as CmdSetViewport
    * is intended to set dynamically a specific viewport, increasing the
    * number of viewport used seems like a non-defined collateral
    * effect. Would make sense to open a spec issue to clarify. For now, as we
    * only support one, it is not really important, but we follow radv
    * approach.
    */
   if (!memcmp(state->dynamic.viewport.viewports + firstViewport,
               pViewports, viewportCount * sizeof(*pViewports))) {
      return;
   }

   memcpy(state->dynamic.viewport.viewports + firstViewport, pViewports,
          viewportCount * sizeof(*pViewports));

   for (uint32_t i = firstViewport; i < firstViewport + viewportCount; i++) {
      get_viewport_xform(&state->dynamic.viewport.viewports[i],
                         state->dynamic.viewport.scale[i],
                         state->dynamic.viewport.translate[i]);
   }

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_DYNAMIC_VIEWPORT;
}

void
v3dv_CmdSetScissor(VkCommandBuffer commandBuffer,
                   uint32_t firstScissor,
                   uint32_t scissorCount,
                   const VkRect2D *pScissors)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const uint32_t total_count = firstScissor + scissorCount;

   assert(firstScissor < MAX_SCISSORS);
   assert(total_count >= 1 && total_count <= MAX_SCISSORS);

   /* See note on CmdSetViewport related to anv/radv differences about setting
    * total viewports used. Also applies to scissor.
    */
   if (!memcmp(state->dynamic.scissor.scissors + firstScissor,
               pScissors, scissorCount * sizeof(*pScissors))) {
      return;
   }

   memcpy(state->dynamic.scissor.scissors + firstScissor, pScissors,
          scissorCount * sizeof(*pScissors));

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_DYNAMIC_SCISSOR;
}

static void
emit_scissor(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_dynamic_state *dynamic = &cmd_buffer->state.dynamic;

   /* FIXME: right now we only support one viewport. viewporst[0] would work
    * now, but would need to change if we allow multiple viewports.
    */
   float *vptranslate = dynamic->viewport.translate[0];
   float *vpscale = dynamic->viewport.scale[0];

   float vp_minx = -fabsf(vpscale[0]) + vptranslate[0];
   float vp_maxx = fabsf(vpscale[0]) + vptranslate[0];
   float vp_miny = -fabsf(vpscale[1]) + vptranslate[1];
   float vp_maxy = fabsf(vpscale[1]) + vptranslate[1];

   /* Quoting from v3dx_emit:
    * "Clip to the scissor if it's enabled, but still clip to the
    * drawable regardless since that controls where the binner
    * tries to put things.
    *
    * Additionally, always clip the rendering to the viewport,
    * since the hardware does guardband clipping, meaning
    * primitives would rasterize outside of the view volume."
    */

   VkRect2D clip_window;
   uint32_t minx, miny, maxx, maxy;

   /* From the Vulkan spec:
    *
    * "The application must ensure (using scissor if necessary) that all
    *  rendering is contained within the render area. The render area must be
    *  contained within the framebuffer dimensions."
    *
    * So it is the application's responsibility to ensure this. Still, we can
    * help by automatically restricting the scissor rect to the render area.
    */
   minx = MAX2(vp_minx, cmd_buffer->state.render_area.offset.x);
   miny = MAX2(vp_miny, cmd_buffer->state.render_area.offset.y);
   maxx = MIN2(vp_maxx, cmd_buffer->state.render_area.offset.x +
                        cmd_buffer->state.render_area.extent.width);
   maxy = MIN2(vp_maxy, cmd_buffer->state.render_area.offset.y +
                        cmd_buffer->state.render_area.extent.height);

   /* Clip against user provided scissor if needed.
    *
    * FIXME: right now we only allow one scissor. Below would need to be
    * updated if we support more
    */
   if (dynamic->scissor.count > 0) {
      VkRect2D *scissor = &dynamic->scissor.scissors[0];
      minx = MAX2(minx, scissor->offset.x);
      miny = MAX2(miny, scissor->offset.y);
      maxx = MIN2(maxx, scissor->offset.x + scissor->extent.width);
      maxy = MIN2(maxy, scissor->offset.y + scissor->extent.height);
   }

   clip_window.offset.x = minx;
   clip_window.offset.y = miny;
   clip_window.extent.width = maxx - minx;
   clip_window.extent.height = maxy - miny;

   emit_clip_window(cmd_buffer->state.job, &clip_window);
}

static void
emit_viewport(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_dynamic_state *dynamic = &cmd_buffer->state.dynamic;
   /* FIXME: right now we only support one viewport. viewporst[0] would work
    * now, would need to change if we allow multiple viewports
    */
   float *vptranslate = dynamic->viewport.translate[0];
   float *vpscale = dynamic->viewport.scale[0];

   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   cl_emit(&job->bcl, CLIPPER_XY_SCALING, clip) {
      clip.viewport_half_width_in_1_256th_of_pixel = vpscale[0] * 256.0f;
      clip.viewport_half_height_in_1_256th_of_pixel = vpscale[1] * 256.0f;
   }

   cl_emit(&job->bcl, CLIPPER_Z_SCALE_AND_OFFSET, clip) {
      clip.viewport_z_offset_zc_to_zs = vptranslate[2];
      clip.viewport_z_scale_zc_to_zs = vpscale[2];
   }
   cl_emit(&job->bcl, CLIPPER_Z_MIN_MAX_CLIPPING_PLANES, clip) {
      float z1 = (vptranslate[2] - vpscale[2]);
      float z2 = (vptranslate[2] + vpscale[2]);
      clip.minimum_zw = MIN2(z1, z2);
      clip.maximum_zw = MAX2(z1, z2);
   }

   cl_emit(&job->bcl, VIEWPORT_OFFSET, vp) {
      vp.viewport_centre_x_coordinate = vptranslate[0];
      vp.viewport_centre_y_coordinate = vptranslate[1];
   }
}

/* FIXME: in fact this is not really required at this point, as we don't plan
 * to initially support GS, but it is more readable and serves as a
 * placeholder, to have the struct and fill it with default values.
 */
struct vpm_config {
   uint32_t As;
   uint32_t Vc;
   uint32_t Gs;
   uint32_t Gd;
   uint32_t Gv;
   uint32_t Ve;
   uint32_t gs_width;
};

static void
cmd_buffer_emit_graphics_pipeline(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   struct v3dv_pipeline *pipeline = state->pipeline;
   assert(pipeline);

   /* Upload the uniforms to the indirect CL first */
   struct v3dv_cl_reloc fs_uniforms =
      v3dv_write_uniforms(cmd_buffer, pipeline->fs);

   struct v3dv_cl_reloc vs_uniforms =
      v3dv_write_uniforms(cmd_buffer, pipeline->vs);

   struct v3dv_cl_reloc vs_bin_uniforms =
      v3dv_write_uniforms(cmd_buffer, pipeline->vs_bin);

   /* Update the cache dirty flag based on the shader progs data */
   job->tmu_dirty_rcl |= pipeline->vs_bin->prog_data.vs->base.tmu_dirty_rcl;
   job->tmu_dirty_rcl |= pipeline->vs->prog_data.vs->base.tmu_dirty_rcl;
   job->tmu_dirty_rcl |= pipeline->fs->prog_data.fs->base.tmu_dirty_rcl;

   /* FIXME: fake vtx->num_elements, that is the vertex state that includes
    * data from the buffers used on the vertex. Such info is still not
    * supported or filled in any place. On Gallium that is filled by
    * st_update_array, that eventually calls v3d_vertex_state_create
    *
    * We area handling it mostly to the GFXH-930 workaround mentioned below,
    * as it would provide more context of why it is needed and to the code.
    */
   uint32_t vtx_num_elements = 0;

   /* See GFXH-930 workaround below */
   uint32_t num_elements_to_emit = MAX2(vtx_num_elements, 1);

   uint32_t shader_rec_offset =
      v3dv_cl_ensure_space(&job->indirect,
                           cl_packet_length(GL_SHADER_STATE_RECORD) +
                           num_elements_to_emit *
                           cl_packet_length(GL_SHADER_STATE_ATTRIBUTE_RECORD),
                           32);

   struct vpm_config vpm_cfg_bin, vpm_cfg;

   /* FIXME: values below are default when non-GS is available. Would need to
    * provide real values if GS gets supported
    */
   vpm_cfg_bin.As = 1;
   vpm_cfg_bin.Ve = 0;
   vpm_cfg_bin.Vc = pipeline->vs_bin->prog_data.vs->vcm_cache_size;

   vpm_cfg.As = 1;
   vpm_cfg.Ve = 0;
   vpm_cfg.Vc = pipeline->vs->prog_data.vs->vcm_cache_size;

   cl_emit(&job->indirect, GL_SHADER_STATE_RECORD, shader) {
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

      shader.coordinate_shader_code_address =
         v3dv_cl_address(pipeline->vs_bin->assembly_bo, 0);
      shader.vertex_shader_code_address =
         v3dv_cl_address(pipeline->vs->assembly_bo, 0);
      shader.fragment_shader_code_address =
         v3dv_cl_address(pipeline->fs->assembly_bo, 0);

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

      shader.coordinate_shader_uniforms_address = vs_bin_uniforms;
      shader.vertex_shader_uniforms_address = vs_uniforms;
      shader.fragment_shader_uniforms_address = fs_uniforms;

      shader.min_coord_shader_input_segments_required_in_play =
         vpm_cfg_bin.As;
      shader.min_vertex_shader_input_segments_required_in_play =
         vpm_cfg.As;

      shader.min_coord_shader_output_segments_required_in_play_in_addition_to_vcm_cache_size =
         vpm_cfg_bin.Ve;
      shader.min_vertex_shader_output_segments_required_in_play_in_addition_to_vcm_cache_size =
         vpm_cfg.Ve;

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

      /* FIXME: I understand that the following is needed only if
       * vtx_num_elements > 0
       */
/*       shader.address_of_default_attribute_values = */
   }

   /* Upload vertex element attributes (SHADER_STATE_ATTRIBUTE_RECORD) */

   /* FIXME: vertex elements not supported yet (vtx_num_elements == 0) */
   if (vtx_num_elements == 0) {
      /* GFXH-930: At least one attribute must be enabled and read
       * by CS and VS.  If we have no attributes being consumed by
       * the shader, set up a dummy to be loaded into the VPM.
       */
      cl_emit(&job->indirect, GL_SHADER_STATE_ATTRIBUTE_RECORD, attr) {
         /* Valid address of data whose value will be unused. */
         attr.address = v3dv_cl_address(job->indirect.bo, 0);

         attr.type = ATTRIBUTE_FLOAT;
         attr.stride = 0;
         attr.vec_size = 1;

         attr.number_of_values_read_by_coordinate_shader = 1;
         attr.number_of_values_read_by_vertex_shader = 1;
      }
   }

   cl_emit(&job->bcl, VCM_CACHE_SIZE, vcm) {
      vcm.number_of_16_vertex_batches_for_binning = vpm_cfg_bin.Vc;
      vcm.number_of_16_vertex_batches_for_rendering = vpm_cfg.Vc;
   }

   cl_emit(&job->bcl, GL_SHADER_STATE, state) {
      state.address = v3dv_cl_address(job->indirect.bo,
                                      shader_rec_offset);
      state.number_of_attribute_arrays = num_elements_to_emit;
   }

}

/* FIXME: C&P from v3dx_draw. Refactor to common place? */
static uint32_t
v3d_hw_prim_type(enum pipe_prim_type prim_type)
{
   switch (prim_type) {
   case PIPE_PRIM_POINTS:
   case PIPE_PRIM_LINES:
   case PIPE_PRIM_LINE_LOOP:
   case PIPE_PRIM_LINE_STRIP:
   case PIPE_PRIM_TRIANGLES:
   case PIPE_PRIM_TRIANGLE_STRIP:
   case PIPE_PRIM_TRIANGLE_FAN:
      return prim_type;

   case PIPE_PRIM_LINES_ADJACENCY:
   case PIPE_PRIM_LINE_STRIP_ADJACENCY:
   case PIPE_PRIM_TRIANGLES_ADJACENCY:
   case PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY:
      return 8 + (prim_type - PIPE_PRIM_LINES_ADJACENCY);

   default:
      unreachable("Unsupported primitive type");
   }
}

struct v3dv_draw_info {
   uint32_t vertex_count;
   uint32_t instance_count;
   uint32_t first_vertex;
   uint32_t first_instance;
};

static void
cmd_buffer_emit_draw_packets(struct v3dv_cmd_buffer *cmd_buffer,
                             struct v3dv_draw_info *info)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   struct v3dv_pipeline *pipeline = state->pipeline;

   assert(pipeline);

   uint32_t prim_tf_enable = 0;
   uint32_t hw_prim_type = v3d_hw_prim_type(pipeline->vs->topology);

   /* FIXME: using VERTEX_ARRAY_PRIMS always as it fits our test caselist
    * right now. Need to be choosen based on the current case.
    */
   cl_emit(&job->bcl, VERTEX_ARRAY_PRIMS, prim) {
      prim.mode = hw_prim_type | prim_tf_enable;
      prim.length = info->vertex_count;
      prim.index_of_first_vertex = info->first_vertex;
   }
}

static void
cmd_buffer_draw(struct v3dv_cmd_buffer *cmd_buffer,
                struct v3dv_draw_info *info)
{
   /* FIXME: likely to be filtered by really needed states */
   uint32_t states = cmd_buffer->state.dirty;
   struct v3dv_dynamic_state *dynamic = &cmd_buffer->state.dynamic;

   if (states & (V3DV_CMD_DIRTY_PIPELINE)) {
      cmd_buffer_emit_graphics_pipeline(cmd_buffer);
   }
   /* Emit(flush) dynamic state */
   if (states & (V3DV_CMD_DIRTY_DYNAMIC_VIEWPORT | V3DV_CMD_DIRTY_DYNAMIC_SCISSOR)) {
      assert(dynamic->scissor.count > 0 || dynamic->viewport.count > 0);

      emit_scissor(cmd_buffer);
   }

   if (states & (V3DV_CMD_DIRTY_DYNAMIC_VIEWPORT)) {
      emit_viewport(cmd_buffer);
   }

   /* FIXME: any dirty flag to filter ? */
   cmd_buffer_emit_draw_packets(cmd_buffer, info);

   cmd_buffer->state.dirty &= ~states;
}

void
v3dv_CmdDraw(VkCommandBuffer commandBuffer,
             uint32_t vertexCount,
             uint32_t instanceCount,
             uint32_t firstVertex,
             uint32_t firstInstance)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   struct v3dv_draw_info info = {};

   info.vertex_count = vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.first_vertex = firstVertex;

   cmd_buffer_draw(cmd_buffer, &info);
}

void
v3dv_CmdPipelineBarrier(VkCommandBuffer commandBuffer,
                        VkPipelineStageFlags srcStageMask,
                        VkPipelineStageFlags dstStageMask,
                        VkDependencyFlags dependencyFlags,
                        uint32_t memoryBarrierCount,
                        const VkMemoryBarrier *pMemoryBarriers,
                        uint32_t bufferMemoryBarrierCount,
                        const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                        uint32_t imageMemoryBarrierCount,
                        const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   struct v3dv_job *job = cmd_buffer->state.job;
   if (!job)
      return;

   v3dv_cmd_buffer_finish_job(cmd_buffer);
}

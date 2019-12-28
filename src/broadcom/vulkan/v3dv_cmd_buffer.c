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
v3dv_cmd_buffer_add_bo(struct v3dv_cmd_buffer *cmd_buffer, struct v3dv_bo *bo)
{
   if (!bo)
      return;

   if (_mesa_set_search(cmd_buffer->bos, bo))
      return;

   _mesa_set_add(cmd_buffer->bos, bo);
   cmd_buffer->bo_count++;
}

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

   cmd_buffer->bos =
      _mesa_set_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);
   cmd_buffer->bo_count = 0;

   v3dv_cl_init(cmd_buffer, &cmd_buffer->bcl);
   v3dv_cl_init(cmd_buffer, &cmd_buffer->rcl);
   v3dv_cl_init(cmd_buffer, &cmd_buffer->indirect);

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_NEW;

   assert(pool);
   list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

   *pCommandBuffer = v3dv_cmd_buffer_to_handle(cmd_buffer);

   return VK_SUCCESS;
}

static void
cmd_buffer_destroy(struct v3dv_cmd_buffer *cmd_buffer)
{
   list_del(&cmd_buffer->pool_link);

   v3dv_cl_destroy(&cmd_buffer->bcl);
   v3dv_cl_destroy(&cmd_buffer->rcl);
   v3dv_cl_destroy(&cmd_buffer->indirect);

   /* Since we don't ref BOs, when we add them to the command buffer, don't
    * unref them here either.
    */
#if 0
   set_foreach(cmd_buffer->bos, entry) {
      struct v3dv_bo *bo = (struct v3dv_bo *)entry->key;
      v3dv_bo_free(cmd_buffer->device, bo);
   }
#endif
   _mesa_set_destroy(cmd_buffer->bos, NULL);

   v3dv_bo_free(cmd_buffer->device, cmd_buffer->tile_alloc);
   v3dv_bo_free(cmd_buffer->device, cmd_buffer->tile_state);

   vk_free(&cmd_buffer->pool->alloc, cmd_buffer);
}

static VkResult
cmd_buffer_reset(struct v3dv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->status != V3DV_CMD_BUFFER_STATUS_INITIALIZED) {
      cmd_buffer->usage_flags = 0;

      _mesa_set_clear(cmd_buffer->bos, NULL);
      cmd_buffer->bo_count = 0;

      v3dv_cl_reset(&cmd_buffer->bcl);
      v3dv_cl_reset(&cmd_buffer->rcl);
      v3dv_cl_reset(&cmd_buffer->indirect);

      struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
      state->pass = NULL;
      state->framebuffer = NULL;
      state->subpass_idx = 0;

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

   v3dv_cl_begin(&cmd_buffer->bcl);
   v3dv_cl_begin(&cmd_buffer->rcl);
   v3dv_cl_begin(&cmd_buffer->indirect);

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_RECORDING;

   return VK_SUCCESS;
}

static void
emit_clip_window(struct v3dv_cmd_buffer *cmd_buffer, VkRect2D *rect)
{
   cl_emit(&cmd_buffer->bcl, CLIP_WINDOW, clip) {
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
   assert(attachment_idx < cmd_buffer->state.framebuffer->attachment_count);

   struct v3dv_image_view *iview =
      cmd_buffer->state.framebuffer->attachments[attachment_idx];
   uint32_t internal_size = 4 << iview->internal_bpp;

   struct v3dv_cmd_buffer_attachment_state *attachment =
      &cmd_buffer->state.attachments[attachment_idx];
   uint32_t *hw_color =  &attachment->clear_value.color[0];

   union util_color uc;
   switch (iview->internal_type) {
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

   const uint32_t bytes = sizeof(VkClearValue) * count;
   if (state->clear_value_count < count) {
      vk_free(&cmd_buffer->device->alloc, state->clear_values);
      state->clear_value_count = count;
      state->clear_values = vk_alloc(&cmd_buffer->device->alloc, bytes, 8,
                                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   }

   memcpy(state->clear_values, values, bytes);
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

   /* Store clear values in the command buffer state for later reference */
   assert(pRenderPassBegin->clearValueCount <= pass->attachment_count);
   cmd_buffer_state_set_clear_values(cmd_buffer,
                                     pRenderPassBegin->clearValueCount,
                                     pRenderPassBegin->pClearValues);

   v3dv_cl_ensure_space_with_branch(&cmd_buffer->bcl, 256);

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

   cmd_buffer->tile_alloc = v3dv_bo_alloc(cmd_buffer->device, tile_alloc_size);
   v3dv_cmd_buffer_add_bo(cmd_buffer, cmd_buffer->tile_alloc);

   const uint32_t tsda_per_tile_size = 256;
   const uint32_t tile_state_size = MAX2(fb_layers, 1) *
                                    framebuffer->draw_tiles_x *
                                    framebuffer->draw_tiles_y *
                                    tsda_per_tile_size;
   cmd_buffer->tile_state = v3dv_bo_alloc(cmd_buffer->device, tile_state_size);
   v3dv_cmd_buffer_add_bo(cmd_buffer, cmd_buffer->tile_state);

   /* This must go before the binning mode configuration. It is
    * required for layered framebuffers to work.
    */
   if (fb_layers > 0) {
      cl_emit(&cmd_buffer->bcl, NUMBER_OF_LAYERS, config) {
         config.number_of_layers = fb_layers;
      }
   }

   cl_emit(&cmd_buffer->bcl, TILE_BINNING_MODE_CFG, config) {
      config.width_in_pixels = framebuffer->width;
      config.height_in_pixels = framebuffer->height;
      config.number_of_render_targets = MAX2(framebuffer->attachment_count, 1);
      config.multisample_mode_4x = false; /* FIXME */
      config.maximum_bpp_of_all_render_targets = framebuffer->internal_bpp;
   }

   /* There's definitely nothing in the VCD cache we want. */
   cl_emit(&cmd_buffer->bcl, FLUSH_VCD_CACHE, bin);

   /* Disable any leftover OQ state from another job. */
   cl_emit(&cmd_buffer->bcl, OCCLUSION_QUERY_COUNTER, counter);

   /* "Binning mode lists must have a Start Tile Binning item (6) after
    *  any prefix state data before the binning list proper starts."
    */
   cl_emit(&cmd_buffer->bcl, START_TILE_BINNING, bin);

   /* FIXME: might want to merge actual scissor rect here if possible */
   /* FIXME: probably need to align the render area to tile boundaries since
    *        the tile clears will render full tiles anyway.
    *        See vkGetRenderAreaGranularity().
    */
   state->render_area = pRenderPassBegin->renderArea;
   emit_clip_window(cmd_buffer, &state->render_area);

   /* Setup for first subpass */
   state->subpass_idx = 0;
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
emit_loads(struct v3dv_cmd_buffer *cmd_buffer,
           struct v3dv_cl *cl,
           uint32_t layer)
{
   /* FIXME: implement tile loads */
   cl_emit(cl, END_OF_LOADS, end);
}

static void
store_general(struct v3dv_cmd_buffer *cmd_buffer,
              struct v3dv_cl *cl,
              struct v3dv_image_view *iview,
              uint32_t layer,
              uint32_t buffer)
{
   const struct v3dv_image *image = iview->image;
   uint32_t layer_offset = v3dv_layer_offset(image,
                                             iview->base_level,
                                             iview->first_layer + layer);

   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = buffer;
      store.address = v3dv_cl_address(image->mem->bo, layer_offset);
      store.clear_buffer_being_stored = false;

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
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   bool has_stores = false;
   bool has_clears = false;
   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t attachment_idx = subpass->color_attachments[i].attachment;

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      const struct v3dv_render_pass_attachment *attachment =
         &state->pass->attachments[attachment_idx];

      /* FIXME: we should probbably precompute this somehwere in the state */
      if (attachment->desc.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
         has_clears = true;

      if (attachment->desc.storeOp != VK_ATTACHMENT_STORE_OP_STORE)
         continue;

      struct v3dv_image_view *iview = framebuffer->attachments[attachment_idx];

      store_general(cmd_buffer, cl, iview, layer, RENDER_TARGET_0 + i);

      has_stores = true;
   }

   /* FIXME: depth/stencil store */

   /* We always need to emit at least one dummy store */
   if (!has_stores) {
      cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
   }

   /* GFXH-1461/GFXH-1689: The per-buffer store command's clear
    * buffer bit is broken for depth/stencil.  In addition, the
    * clear packet's Z/S bit is broken, but the RTs bit ends up
    * clearing Z/S.
    */
   if (has_clears) {
      cl_emit(cl, CLEAR_TILE_BUFFERS, clear) {
         clear.clear_z_stencil_buffer = true;
         clear.clear_all_render_targets = true;
      }
   }
}

static void
emit_generic_per_tile_list(struct v3dv_cmd_buffer *cmd_buffer, uint32_t layer)
{
   /* Emit the generic list in our indirect state -- the rcl will just
    * have pointers into it.
    */
   struct v3dv_cl *cl = &cmd_buffer->indirect;
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

   cl_emit(&cmd_buffer->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_render_layer(struct v3dv_cmd_buffer *cmd_buffer, uint32_t layer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;

   struct v3dv_cl *rcl = &cmd_buffer->rcl;

   /* If doing multicore binning, we would need to initialize each
    * core's tile list here.
    */
   const uint32_t tile_alloc_offset =
      64 * layer * framebuffer->draw_tiles_x * framebuffer->draw_tiles_y;
   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(cmd_buffer->tile_alloc, tile_alloc_offset);
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
   /* FIXME */
   const uint32_t fb_layers = 1;

   v3dv_cl_ensure_space_with_branch(&cmd_buffer->rcl, 200 +
                                    MAX2(fb_layers, 1) * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;

   assert(state->subpass_idx < state->pass->subpass_count);
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   struct v3dv_cl *rcl = &cmd_buffer->rcl;

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
setup_subpass(struct v3dv_cmd_buffer *cmd_buffer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;

   assert(state->subpass_idx < state->pass->subpass_count);
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   /* Compute hardware color clear values for each subpass attachment */
   /* FIXME: support depth/stencil */
   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t rp_attachment_idx = subpass->color_attachments[i].attachment;
      const struct v3dv_render_pass_attachment *attachment =
         &cmd_buffer->state.pass->attachments[rp_attachment_idx];

      /* FIXME: if a previous subpass has alredy computed the hw clear color
       *        for this attachment we could skip this. We can just flag this
       *        in the command buffer state.
       */

      if (attachment->desc.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      const uint32_t sp_attachment_idx = i;
      const struct v3dv_image_view *iview =
         cmd_buffer->state.framebuffer->attachments[sp_attachment_idx];

      assert((iview->aspects &
              (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) == 0);

      if (iview->aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
         const VkClearColorValue *clear_color =
            &state->clear_values[rp_attachment_idx].color;
         cmd_buffer_state_set_attachment_clear_color(cmd_buffer,
                                                     sp_attachment_idx,
                                                     clear_color);
      }
   }
}

static void
execute_subpass(struct v3dv_cmd_buffer *cmd_buffer)
{
   setup_subpass(cmd_buffer);
   emit_rcl(cmd_buffer);
}

void
v3dv_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   /* Emit last subpass */
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   assert(state->subpass_idx == state->pass->subpass_count - 1);
   execute_subpass(cmd_buffer);

   /* We are no longer inside a render pass */
   state->pass = NULL;
   state->framebuffer = NULL;
}

VkResult
v3dv_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   if (v3dv_cl_offset(&cmd_buffer->bcl) == 0)
      return VK_SUCCESS; /* FIXME? */

   v3dv_cl_ensure_space_with_branch(&cmd_buffer->bcl, cl_packet_length(FLUSH));

   /* We just FLUSH here to tell the HW to cap the bin CLs with a
    * return. Any remaining state changes won't be flushed to
    * the bins first -- you would need FLUSH_ALL for that, but
    * the HW for it hasn't been validated.
    */
   cl_emit(&cmd_buffer->bcl, FLUSH, flush);

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_EXECUTABLE;

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

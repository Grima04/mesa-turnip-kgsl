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

void
v3dv_cmd_buffer_add_bo(struct v3dv_cmd_buffer *cmd_buffer, struct v3dv_bo *bo)
{
   if (!bo)
      return;

   if (_mesa_set_search(cmd_buffer->bos, bo))
      return;

   _mesa_set_add(cmd_buffer->bos, bo);
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

   v3dv_cl_init(cmd_buffer, &cmd_buffer->bcl);
   v3dv_cl_init(cmd_buffer, &cmd_buffer->rcl);
   v3dv_cl_init(cmd_buffer, &cmd_buffer->indirect);

   cmd_buffer->bos =
      _mesa_set_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);

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

   set_foreach(cmd_buffer->bos, entry) {
      struct v3dv_bo *bo = (struct v3dv_bo *)entry->key;
      v3dv_bo_free(cmd_buffer->device, bo);
   }
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
      v3dv_cl_reset(&cmd_buffer->bcl);
      v3dv_cl_reset(&cmd_buffer->rcl);
      v3dv_cl_reset(&cmd_buffer->indirect);
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

void
v3dv_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                        const VkRenderPassBeginInfo *pRenderPassBegin,
                        VkSubpassContents contents)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_render_pass, pass, pRenderPassBegin->renderPass);
   V3DV_FROM_HANDLE(v3dv_framebuffer, framebuffer, pRenderPassBegin->framebuffer);

   cmd_buffer->state.pass = pass;
   cmd_buffer->state.framebuffer = framebuffer;

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

   const uint32_t tsda_per_tile_size = 256;
   const uint32_t tile_state_size = MAX2(fb_layers, 1) *
                                    framebuffer->draw_tiles_x *
                                    framebuffer->draw_tiles_y *
                                    tsda_per_tile_size;
   cmd_buffer->tile_state = v3dv_bo_alloc(cmd_buffer->device, tile_state_size);


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
}

void
v3dv_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
}

VkResult
v3dv_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   return VK_SUCCESS;
}

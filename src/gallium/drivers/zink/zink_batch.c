#include "zink_batch.h"

#include "zink_context.h"
#include "zink_fence.h"
#include "zink_framebuffer.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"

#include "util/u_debug.h"
#include "util/set.h"

static void
reset_batch(struct zink_screen *screen, struct zink_batch *batch)
{
   batch->descs_left = ZINK_BATCH_DESC_SIZE;

   // cmdbuf hasn't been submitted before
   if (!batch->fence)
      return;

   zink_fence_finish(screen, batch->fence, PIPE_TIMEOUT_INFINITE);
   zink_fence_reference(screen, &batch->fence, NULL);

   zink_render_pass_reference(screen, &batch->rp, NULL);
   zink_framebuffer_reference(screen, &batch->fb, NULL);

   /* unref all used resources */
   set_foreach(batch->resources, entry) {
      struct pipe_resource *pres = (struct pipe_resource *)entry->key;
      pipe_resource_reference(&pres, NULL);
   }
   _mesa_set_clear(batch->resources, NULL);

   /* unref all used sampler-views */
   set_foreach(batch->sampler_views, entry) {
      struct pipe_sampler_view *pres = (struct pipe_sampler_view *)entry->key;
      pipe_sampler_view_reference(&pres, NULL);
   }
   _mesa_set_clear(batch->sampler_views, NULL);

   util_dynarray_foreach(&batch->zombie_samplers, VkSampler, samp) {
      vkDestroySampler(screen->dev, *samp, NULL);
   }
   util_dynarray_clear(&batch->zombie_samplers);

   if (vkResetDescriptorPool(screen->dev, batch->descpool, 0) != VK_SUCCESS)
      fprintf(stderr, "vkResetDescriptorPool failed\n");
}

void
zink_start_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   reset_batch(zink_screen(ctx->base.screen), batch);

   VkCommandBufferBeginInfo cbbi = {};
   cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   if (vkBeginCommandBuffer(batch->cmdbuf, &cbbi) != VK_SUCCESS)
      debug_printf("vkBeginCommandBuffer failed\n");
}

static bool
submit_cmdbuf(struct zink_context *ctx, VkCommandBuffer cmdbuf, VkFence fence)
{
   VkPipelineStageFlags wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   VkSubmitInfo si = {};
   si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si.waitSemaphoreCount = 0;
   si.pWaitSemaphores = NULL;
   si.signalSemaphoreCount = 0;
   si.pSignalSemaphores = NULL;
   si.pWaitDstStageMask = &wait;
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cmdbuf;

   if (vkQueueSubmit(ctx->queue, 1, &si, fence) != VK_SUCCESS) {
      debug_printf("vkQueueSubmit failed\n");
      return false;
   }

   return true;
}

void
zink_end_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   if (vkEndCommandBuffer(batch->cmdbuf) != VK_SUCCESS) {
      debug_printf("vkEndCommandBuffer failed\n");
      return;
   }

   assert(batch->fence == NULL);
   batch->fence = zink_create_fence(ctx->base.screen);
   if (!batch->fence ||
       !submit_cmdbuf(ctx, batch->cmdbuf, batch->fence->fence))
      return;
}

void
zink_batch_reference_resoure(struct zink_batch *batch,
                             struct zink_resource *res)
{
   struct set_entry *entry = _mesa_set_search(batch->resources, res);
   if (!entry) {
      struct pipe_resource *tmp = NULL;
      entry = _mesa_set_add(batch->resources, res);
      pipe_resource_reference(&tmp, &res->base);
   }
}

void
zink_batch_reference_sampler_view(struct zink_batch *batch,
                                  struct zink_sampler_view *sv)
{
   struct set_entry *entry = _mesa_set_search(batch->sampler_views, sv);
   if (!entry) {
      struct pipe_sampler_view *tmp = NULL;
      entry = _mesa_set_add(batch->sampler_views, sv);
      pipe_sampler_view_reference(&tmp, &sv->base);
   }
}

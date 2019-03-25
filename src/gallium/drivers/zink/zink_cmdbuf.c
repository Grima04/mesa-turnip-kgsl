#include "zink_cmdbuf.h"

#include "zink_context.h"
#include "zink_fence.h"
#include "zink_screen.h"

#include "util/u_debug.h"

static void
reset_cmdbuf(struct zink_screen *screen, struct zink_cmdbuf *cmdbuf)
{
   // cmdbuf hasn't been submitted before
   if (!cmdbuf->fence)
      return;

   zink_fence_finish(screen, cmdbuf->fence, PIPE_TIMEOUT_INFINITE);
   zink_fence_reference(screen, &cmdbuf->fence, NULL);
}

struct zink_cmdbuf *
zink_start_cmdbuf(struct zink_context *ctx)
{
   struct zink_cmdbuf *cmdbuf = &ctx->cmdbuf;
   reset_cmdbuf(zink_screen(ctx->base.screen), cmdbuf);

   VkCommandBufferBeginInfo cbbi = {};
   cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   if (vkBeginCommandBuffer(cmdbuf->cmdbuf, &cbbi) != VK_SUCCESS) {
      debug_printf("vkBeginCommandBuffer failed\n");
      return NULL;
   }

   return cmdbuf;
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
zink_end_cmdbuf(struct zink_context *ctx, struct zink_cmdbuf *cmdbuf)
{
   if (vkEndCommandBuffer(cmdbuf->cmdbuf) != VK_SUCCESS) {
      debug_printf("vkEndCommandBuffer failed\n");
      return;
   }

   assert(cmdbuf->fence == NULL);
   cmdbuf->fence = zink_create_fence(ctx->base.screen);
   if (!cmdbuf->fence ||
       !submit_cmdbuf(ctx, cmdbuf->cmdbuf, cmdbuf->fence->fence))
      return;

   if (vkQueueWaitIdle(ctx->queue) != VK_SUCCESS)
      debug_printf("vkQueueWaitIdle failed\n");
}

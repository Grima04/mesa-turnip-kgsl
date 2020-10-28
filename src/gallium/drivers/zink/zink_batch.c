#include "zink_batch.h"

#include "zink_context.h"
#include "zink_fence.h"
#include "zink_framebuffer.h"
#include "zink_query.h"
#include "zink_program.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_surface.h"

#include "util/hash_table.h"
#include "util/u_debug.h"
#include "util/set.h"

#include "wsi_common.h"

void
zink_reset_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   batch->descs_used = 0;

   // cmdbuf hasn't been submitted before
   if (batch->submitted)
      zink_fence_finish(screen, &ctx->base, batch->fence, PIPE_TIMEOUT_INFINITE);

   /* unref all used resources */
   set_foreach(batch->resources, entry) {
      struct zink_resource_object *obj = (struct zink_resource_object *)entry->key;
      zink_resource_object_reference(screen, &obj, NULL);
      _mesa_set_remove(batch->resources, entry);
   }

   set_foreach(batch->surfaces, entry) {
      struct zink_surface *surf = (struct zink_surface *)entry->key;
      surf->batch_uses &= ~BITFIELD64_BIT(batch->batch_id);
      pipe_surface_reference((struct pipe_surface**)&surf, NULL);
      _mesa_set_remove(batch->surfaces, entry);
   }
   set_foreach(batch->bufferviews, entry) {
      struct zink_buffer_view *buffer_view = (struct zink_buffer_view *)entry->key;
      buffer_view->batch_uses &= ~BITFIELD64_BIT(batch->batch_id);
      zink_buffer_view_reference(screen, &buffer_view, NULL);
      _mesa_set_remove(batch->bufferviews, entry);
   }

   util_dynarray_foreach(&batch->zombie_samplers, VkSampler, samp) {
      vkDestroySampler(screen->dev, *samp, NULL);
   }
   util_dynarray_clear(&batch->zombie_samplers);
   util_dynarray_clear(&batch->persistent_resources);

   set_foreach(batch->desc_sets, entry) {
      struct zink_descriptor_set *zds = (void*)entry->key;
      zds->batch_uses &= ~BITFIELD_BIT(batch->batch_id);
      /* reset descriptor pools when no batch is using this program to avoid
       * having some inactive program hogging a billion descriptors
       */
      pipe_reference(&zds->reference, NULL);
      zink_descriptor_set_recycle(zds);
      _mesa_set_remove(batch->desc_sets, entry);
   }

   set_foreach(batch->programs, entry) {
      if (batch->batch_id == ZINK_COMPUTE_BATCH_ID) {
         struct zink_compute_program *comp = (struct zink_compute_program*)entry->key;
         bool in_use = comp == ctx->curr_compute;
         if (zink_compute_program_reference(screen, &comp, NULL) && in_use)
            ctx->curr_compute = NULL;
      } else {
         struct zink_gfx_program *prog = (struct zink_gfx_program*)entry->key;
         bool in_use = prog == ctx->curr_program;
         if (zink_gfx_program_reference(screen, &prog, NULL) && in_use)
            ctx->curr_program = NULL;
      }
      _mesa_set_remove(batch->programs, entry);
   }

   set_foreach(batch->fbs, entry) {
      struct zink_framebuffer *fb = (void*)entry->key;
      zink_framebuffer_reference(screen, &fb, NULL);
      _mesa_set_remove(batch->fbs, entry);
   }

   if (vkResetCommandPool(screen->dev, batch->cmdpool, 0) != VK_SUCCESS)
      debug_printf("vkResetCommandPool failed\n");
   batch->submitted = batch->has_work = false;
   batch->resource_size = 0;
}

void
zink_start_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   zink_reset_batch(ctx, batch);

   VkCommandBufferBeginInfo cbbi = {};
   cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   if (vkBeginCommandBuffer(batch->cmdbuf, &cbbi) != VK_SUCCESS)
      debug_printf("vkBeginCommandBuffer failed\n");

   if (!ctx->queries_disabled)
      zink_resume_queries(ctx, batch);
}

void
zink_end_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   if (!ctx->queries_disabled)
      zink_suspend_queries(ctx, batch);

   if (vkEndCommandBuffer(batch->cmdbuf) != VK_SUCCESS) {
      debug_printf("vkEndCommandBuffer failed\n");
      return;
   }

   vkResetFences(zink_screen(ctx->base.screen)->dev, 1, &batch->fence->fence);
   zink_fence_init(batch->fence, batch);

   util_dynarray_foreach(&batch->persistent_resources, struct zink_resource*, res) {
       struct zink_screen *screen = zink_screen(ctx->base.screen);
       assert(!(*res)->obj->offset);
       VkMappedMemoryRange range = {
          VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
          NULL,
          (*res)->obj->mem,
          (*res)->obj->offset,
          VK_WHOLE_SIZE,
       };
       vkFlushMappedMemoryRanges(screen->dev, 1, &range);
   }

   VkSubmitInfo si = {};
   si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si.waitSemaphoreCount = 0;
   si.pWaitSemaphores = NULL;
   si.signalSemaphoreCount = 0;
   si.pSignalSemaphores = NULL;
   si.pWaitDstStageMask = NULL;
   si.commandBufferCount = 1;
   si.pCommandBuffers = &batch->cmdbuf;

   struct wsi_memory_signal_submit_info mem_signal = {
      .sType = VK_STRUCTURE_TYPE_WSI_MEMORY_SIGNAL_SUBMIT_INFO_MESA,
      .pNext = si.pNext,
   };

   if (batch->flush_res) {
      mem_signal.memory = batch->flush_res->obj->mem;
      si.pNext = &mem_signal;
   }

   if (vkQueueSubmit(ctx->queue, 1, &si, batch->fence->fence) != VK_SUCCESS) {
      debug_printf("ZINK: vkQueueSubmit() failed\n");
      ctx->is_device_lost = true;

      if (ctx->reset.reset) {
         ctx->reset.reset(ctx->reset.data, PIPE_GUILTY_CONTEXT_RESET);
      }
   }
   batch->submitted = true;
   batch->flush_res = NULL;
}

/* returns either the compute batch id or 0 (gfx batch id) based on whether a resource
   has usage on a different queue than 'batch' belongs to
 */
int
zink_batch_reference_resource_rw(struct zink_batch *batch, struct zink_resource *res, bool write)
{
   unsigned mask = write ? ZINK_RESOURCE_ACCESS_WRITE : ZINK_RESOURCE_ACCESS_READ;
   int batch_to_flush = -1;

   /* u_transfer_helper unrefs the stencil buffer when the depth buffer is unrefed,
    * so we add an extra ref here to the stencil buffer to compensate
    */
   struct zink_resource *stencil;

   zink_get_depth_stencil_resources((struct pipe_resource*)res, NULL, &stencil);

   if (batch->batch_id == ZINK_COMPUTE_BATCH_ID) {
      if ((write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_RW, ZINK_QUEUE_GFX)) ||
          (!write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_WRITE, ZINK_QUEUE_GFX)))
         batch_to_flush = 0;
   } else {
      if ((write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_READ, ZINK_QUEUE_COMPUTE)) ||
          zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_WRITE, ZINK_QUEUE_COMPUTE))
         batch_to_flush = ZINK_COMPUTE_BATCH_ID;
   }

   /* if the resource already has usage of any sort set for this batch, we can skip hashing */
   if (!zink_resource_has_usage_for_id(res, batch->batch_id)) {
      bool found = false;
      _mesa_set_search_and_add(batch->resources, res->obj, &found);
      if (!found) {
         pipe_reference(NULL, &res->obj->reference);
         batch->resource_size += res->obj->size;
         if (stencil) {
            pipe_reference(NULL, &stencil->obj->reference);
            batch->resource_size += stencil->obj->size;
         }
      }
   }
   /* multiple array entries are fine */
   if (res->obj->persistent_maps)
      util_dynarray_append(&batch->persistent_resources, struct zink_resource*, res);
   /* the batch_uses value for this batch is guaranteed to not be in use now because
    * zink_reset_batch() waits on the fence and removes access before resetting
    */
   res->obj->batch_uses[batch->batch_id] |= mask;

   if (stencil)
      stencil->obj->batch_uses[batch->batch_id] |= mask;

   batch->has_work = true;
   return batch_to_flush;
}

static bool
ptr_add_usage(struct zink_batch *batch, struct set *s, void *ptr, uint32_t *u)
{
   bool found = false;
   uint32_t bit = BITFIELD_BIT(batch->batch_id);
   if ((*u) & bit)
      return false;
   _mesa_set_search_and_add(s, ptr, &found);
   assert(!found);
   *u |= bit;
   return true;
}

void
zink_batch_reference_sampler_view(struct zink_batch *batch,
                                  struct zink_sampler_view *sv)
{
   if (sv->base.target == PIPE_BUFFER) {
      if (!ptr_add_usage(batch, batch->bufferviews, sv->buffer_view, &sv->buffer_view->batch_uses))
         return;
      pipe_reference(NULL, &sv->buffer_view->reference);
   } else {
      if (!ptr_add_usage(batch, batch->surfaces, sv->image_view, &sv->image_view->batch_uses))
         return;
      pipe_reference(NULL, &sv->image_view->base.reference);
   }
   batch->has_work = true;
}

void
zink_batch_reference_framebuffer(struct zink_batch *batch,
                                 struct zink_framebuffer *fb)
{
   bool found;
   _mesa_set_search_or_add(batch->fbs, fb, &found);
   if (!found)
      pipe_reference(NULL, &fb->reference);
}

void
zink_batch_reference_program(struct zink_batch *batch,
                             struct zink_program *pg)
{
   bool found = false;
   _mesa_set_search_and_add(batch->programs, pg, &found);
   if (!found)
      pipe_reference(NULL, &pg->reference);
   batch->has_work = true;
}

bool
zink_batch_add_desc_set(struct zink_batch *batch, struct zink_descriptor_set *zds)
{
   bool found = false;
   uint32_t bit = BITFIELD_BIT(batch->batch_id);
   if (zds->batch_uses & bit)
      return false;
   _mesa_set_search_and_add(batch->desc_sets, zds, &found);
   assert(!found);
   zds->batch_uses |= bit;
   pipe_reference(NULL, &zds->reference);
   return !found;
}

void
zink_batch_reference_image_view(struct zink_batch *batch,
                                struct zink_image_view *image_view)
{
   bool found = false;
   uint32_t bit = BITFIELD64_BIT(batch->batch_id);
   if (image_view->base.resource->target == PIPE_BUFFER) {
      if (image_view->buffer_view->batch_uses & bit)
         return;
      _mesa_set_search_and_add(batch->bufferviews, image_view->buffer_view, &found);
      assert(!found);
      image_view->buffer_view->batch_uses |= bit;
      pipe_reference(NULL, &image_view->buffer_view->reference);
   } else {
      if (image_view->surface->batch_uses & bit)
         return;
      _mesa_set_search_and_add(batch->surfaces, image_view->surface, &found);
      assert(!found);
      image_view->surface->batch_uses |= bit;
      pipe_reference(NULL, &image_view->surface->base.reference);
   }
   batch->has_work = true;
}

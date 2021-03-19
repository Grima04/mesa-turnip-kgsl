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
zink_batch_state_clear_resources(struct zink_screen *screen, struct zink_batch_state *bs)
{
   /* unref all used resources */
   set_foreach(bs->resources, entry) {
      struct zink_resource_object *obj = (struct zink_resource_object *)entry->key;
      zink_resource_object_reference(screen, &obj, NULL);
      _mesa_set_remove(bs->resources, entry);
   }
}

void
zink_reset_batch_state(struct zink_context *ctx, struct zink_batch_state *bs)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   if (bs->fence->submitted)
      zink_fence_finish(screen, &ctx->base, bs->fence, PIPE_TIMEOUT_INFINITE);

   zink_batch_state_clear_resources(screen, bs);

   set_foreach(bs->active_queries, entry) {
      struct zink_query *query = (void*)entry->key;
      zink_prune_query(screen, query);
      _mesa_set_remove(bs->active_queries, entry);
   }

   set_foreach(bs->surfaces, entry) {
      struct zink_surface *surf = (struct zink_surface *)entry->key;
      surf->batch_uses &= ~BITFIELD64_BIT(bs->batch_id);
      pipe_surface_reference((struct pipe_surface**)&surf, NULL);
      _mesa_set_remove(bs->surfaces, entry);
   }
   set_foreach(bs->bufferviews, entry) {
      struct zink_buffer_view *buffer_view = (struct zink_buffer_view *)entry->key;
      buffer_view->batch_uses &= ~BITFIELD64_BIT(bs->batch_id);
      zink_buffer_view_reference(screen, &buffer_view, NULL);
      _mesa_set_remove(bs->bufferviews, entry);
   }

   util_dynarray_foreach(&bs->zombie_samplers, VkSampler, samp) {
      vkDestroySampler(screen->dev, *samp, NULL);
   }
   util_dynarray_clear(&bs->zombie_samplers);
   util_dynarray_clear(&bs->persistent_resources);

   set_foreach(bs->desc_sets, entry) {
      struct zink_descriptor_set *zds = (void*)entry->key;
      zds->batch_uses &= ~BITFIELD_BIT(bs->batch_id);
      /* reset descriptor pools when no bs is using this program to avoid
       * having some inactive program hogging a billion descriptors
       */
      pipe_reference(&zds->reference, NULL);
      zink_descriptor_set_recycle(zds);
      _mesa_set_remove(bs->desc_sets, entry);
   }

   set_foreach(bs->programs, entry) {
      if (bs->is_compute) {
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
      _mesa_set_remove(bs->programs, entry);
   }

   set_foreach(bs->fbs, entry) {
      struct zink_framebuffer *fb = (void*)entry->key;
      zink_framebuffer_reference(screen, &fb, NULL);
      _mesa_set_remove(bs->fbs, entry);
   }

   bs->flush_res = NULL;

   bs->descs_used = 0;
   bs->fence->submitted = false;
   bs->resource_size = 0;
}

void
zink_batch_state_destroy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!bs)
      return;

   if (bs->cmdbuf)
      vkFreeCommandBuffers(screen->dev, bs->cmdpool, 1, &bs->cmdbuf);
   if (bs->cmdpool)
      vkDestroyCommandPool(screen->dev, bs->cmdpool, NULL);

   _mesa_set_destroy(bs->fbs, NULL);
   _mesa_set_destroy(bs->resources, NULL);
   util_dynarray_fini(&bs->zombie_samplers);
   _mesa_set_destroy(bs->surfaces, NULL);
   _mesa_set_destroy(bs->bufferviews, NULL);
   _mesa_set_destroy(bs->programs, NULL);
   _mesa_set_destroy(bs->desc_sets, NULL);
   _mesa_set_destroy(bs->active_queries, NULL);
   ralloc_free(bs);
}

static struct zink_batch_state *
create_batch_state(struct zink_context *ctx, unsigned idx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_batch_state *bs = rzalloc(NULL, struct zink_batch_state);
   VkCommandPoolCreateInfo cpci = {};
   cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cpci.queueFamilyIndex = screen->gfx_queue;
   cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   if (vkCreateCommandPool(screen->dev, &cpci, NULL, &bs->cmdpool) != VK_SUCCESS)
      goto fail;

   VkCommandBufferAllocateInfo cbai = {};
   cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cbai.commandPool = bs->cmdpool;
   cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbai.commandBufferCount = 1;

   if (vkAllocateCommandBuffers(screen->dev, &cbai, &bs->cmdbuf) != VK_SUCCESS)
      goto fail;

#define SET_CREATE_OR_FAIL(ptr) \
   ptr = _mesa_pointer_set_create(bs); \
   if (!ptr) \
      goto fail

   SET_CREATE_OR_FAIL(bs->fbs);
   SET_CREATE_OR_FAIL(bs->resources);
   SET_CREATE_OR_FAIL(bs->surfaces);
   SET_CREATE_OR_FAIL(bs->bufferviews);
   SET_CREATE_OR_FAIL(bs->programs);
   SET_CREATE_OR_FAIL(bs->desc_sets);
   SET_CREATE_OR_FAIL(bs->active_queries);
   util_dynarray_init(&bs->zombie_samplers, NULL);
   util_dynarray_init(&bs->persistent_resources, NULL);
   bs->batch_id = idx;

   if (!zink_create_fence(screen, bs))
      /* this destroys the batch state on failure */
      return NULL;

   bs->is_compute = idx == ZINK_COMPUTE_BATCH_ID;

   return bs;
fail:
   zink_batch_state_destroy(screen, bs);
   return NULL;
}

static void
init_batch_state(struct zink_context *ctx, struct zink_batch *batch)
{
   struct zink_batch_state *bs = create_batch_state(ctx, batch->batch_id);
   batch->state = bs;
}

void
zink_reset_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   bool fresh = !batch->state;

   init_batch_state(ctx, batch);
   assert(batch->state);

   if (!fresh) {
      if (vkResetCommandPool(screen->dev, batch->state->cmdpool, 0) != VK_SUCCESS)
         debug_printf("vkResetCommandPool failed\n");
   }
   batch->has_work = false;
}

void
zink_start_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   zink_reset_batch(ctx, batch);

   VkCommandBufferBeginInfo cbbi = {};
   cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   if (vkBeginCommandBuffer(batch->state->cmdbuf, &cbbi) != VK_SUCCESS)
      debug_printf("vkBeginCommandBuffer failed\n");

   if (!ctx->queries_disabled)
      zink_resume_queries(ctx, batch);
}

void
zink_end_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   if (!ctx->queries_disabled)
      zink_suspend_queries(ctx, batch);

   if (vkEndCommandBuffer(batch->state->cmdbuf) != VK_SUCCESS) {
      debug_printf("vkEndCommandBuffer failed\n");
      return;
   }

   zink_fence_init(ctx, batch);

   util_dynarray_foreach(&batch->state->persistent_resources, struct zink_resource*, res) {
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
   si.pCommandBuffers = &batch->state->cmdbuf;

   struct wsi_memory_signal_submit_info mem_signal = {
      .sType = VK_STRUCTURE_TYPE_WSI_MEMORY_SIGNAL_SUBMIT_INFO_MESA,
      .pNext = si.pNext,
   };

   if (batch->state->flush_res) {
      mem_signal.memory = batch->state->flush_res->obj->mem;
      si.pNext = &mem_signal;
   }

   if (vkQueueSubmit(ctx->queue, 1, &si, batch->state->fence->fence) != VK_SUCCESS) {
      debug_printf("ZINK: vkQueueSubmit() failed\n");
      ctx->is_device_lost = true;

      if (ctx->reset.reset) {
         ctx->reset.reset(ctx->reset.data, PIPE_GUILTY_CONTEXT_RESET);
      }
   }
   batch->state->fence->submitted = true;
}

/* returns a queue based on whether a resource
   has usage on a different queue than 'batch' belongs to
 */
enum zink_queue
zink_batch_reference_resource_rw(struct zink_batch *batch, struct zink_resource *res, bool write)
{
   unsigned mask = write ? ZINK_RESOURCE_ACCESS_WRITE : ZINK_RESOURCE_ACCESS_READ;
   enum zink_queue batch_to_flush = 0;

   /* u_transfer_helper unrefs the stencil buffer when the depth buffer is unrefed,
    * so we add an extra ref here to the stencil buffer to compensate
    */
   struct zink_resource *stencil;

   zink_get_depth_stencil_resources((struct pipe_resource*)res, NULL, &stencil);

   if (batch->batch_id == ZINK_COMPUTE_BATCH_ID) {
      if ((write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_RW, ZINK_QUEUE_GFX)) ||
          (!write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_WRITE, ZINK_QUEUE_GFX)))
         batch_to_flush = ZINK_QUEUE_GFX;
   } else {
      if ((write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_READ, ZINK_QUEUE_COMPUTE)) ||
          zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_WRITE, ZINK_QUEUE_COMPUTE))
         batch_to_flush = ZINK_QUEUE_COMPUTE;
   }

   /* if the resource already has usage of any sort set for this batch, we can skip hashing */
   if (!zink_resource_has_usage_for_id(res, batch->state->batch_id)) {
      bool found = false;
      _mesa_set_search_and_add(batch->state->resources, res->obj, &found);
      if (!found) {
         pipe_reference(NULL, &res->obj->reference);
         batch->state->resource_size += res->obj->size;
         if (stencil) {
            pipe_reference(NULL, &stencil->obj->reference);
            batch->state->resource_size += stencil->obj->size;
         }
      }
   }
   /* multiple array entries are fine */
   if (res->obj->persistent_maps)
      util_dynarray_append(&batch->state->persistent_resources, struct zink_resource*, res);
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
   uint32_t bit = BITFIELD_BIT(batch->state->batch_id);
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
      if (!ptr_add_usage(batch, batch->state->bufferviews, sv->buffer_view, &sv->buffer_view->batch_uses))
         return;
      pipe_reference(NULL, &sv->buffer_view->reference);
   } else {
      if (!ptr_add_usage(batch, batch->state->surfaces, sv->image_view, &sv->image_view->batch_uses))
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
   _mesa_set_search_or_add(batch->state->fbs, fb, &found);
   if (!found)
      pipe_reference(NULL, &fb->reference);
}

void
zink_batch_reference_program(struct zink_batch *batch,
                             struct zink_program *pg)
{
   bool found = false;
   _mesa_set_search_and_add(batch->state->programs, pg, &found);
   if (!found)
      pipe_reference(NULL, &pg->reference);
   batch->has_work = true;
}

bool
zink_batch_add_desc_set(struct zink_batch *batch, struct zink_descriptor_set *zds)
{
   if (!ptr_add_usage(batch, batch->state->desc_sets, zds, &zds->batch_uses))
      return false;
   pipe_reference(NULL, &zds->reference);
   return true;
}

void
zink_batch_reference_image_view(struct zink_batch *batch,
                                struct zink_image_view *image_view)
{
   if (image_view->base.resource->target == PIPE_BUFFER) {
      if (!ptr_add_usage(batch, batch->state->bufferviews, image_view->buffer_view, &image_view->buffer_view->batch_uses))
         return;
      pipe_reference(NULL, &image_view->buffer_view->reference);
   } else {
      if (!ptr_add_usage(batch, batch->state->surfaces, image_view->surface, &image_view->surface->batch_uses))
         return;
      pipe_reference(NULL, &image_view->surface->base.reference);
   }
   batch->has_work = true;
}

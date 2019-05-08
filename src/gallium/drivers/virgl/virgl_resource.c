/*
 * Copyright 2014, 2015 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "virgl_context.h"
#include "virgl_resource.h"
#include "virgl_screen.h"

/* We need to flush to properly sync the transfer with the current cmdbuf.
 * But there are cases where the flushing can be skipped:
 *
 *  - synchronization is disabled
 *  - the resource is not referenced by the current cmdbuf
 */
static bool virgl_res_needs_flush(struct virgl_context *vctx,
                                  struct virgl_transfer *trans)
{
   struct virgl_winsys *vws = virgl_screen(vctx->base.screen)->vws;
   struct virgl_resource *res = virgl_resource(trans->base.resource);

   if (trans->base.usage & PIPE_TRANSFER_UNSYNCHRONIZED)
      return false;

   if (!vws->res_is_referenced(vws, vctx->cbuf, res->hw_res))
      return false;

   return true;
}

/* We need to read back from the host storage to make sure the guest storage
 * is up-to-date.  But there are cases where the readback can be skipped:
 *
 *  - the content can be discarded
 *  - the host storage is read-only
 *
 * Note that PIPE_TRANSFER_WRITE without discard bits requires readback.
 * PIPE_TRANSFER_READ becomes irrelevant.  PIPE_TRANSFER_UNSYNCHRONIZED and
 * PIPE_TRANSFER_FLUSH_EXPLICIT are also irrelevant.
 */
static bool virgl_res_needs_readback(struct virgl_context *vctx,
                                     struct virgl_resource *res,
                                     unsigned usage, unsigned level)
{
   if (usage & (PIPE_TRANSFER_DISCARD_RANGE |
                PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE))
      return false;

   if (res->clean_mask & (1 << level))
      return false;

   return true;
}

enum virgl_transfer_map_type
virgl_resource_transfer_prepare(struct virgl_context *vctx,
                                struct virgl_transfer *xfer)
{
   struct virgl_winsys *vws = virgl_screen(vctx->base.screen)->vws;
   struct virgl_resource *res = virgl_resource(xfer->base.resource);
   enum virgl_transfer_map_type map_type = VIRGL_TRANSFER_MAP_HW_RES;
   bool flush;
   bool readback;
   bool wait;

   /* there is no way to map the host storage currently */
   if (xfer->base.usage & PIPE_TRANSFER_MAP_DIRECTLY)
      return VIRGL_TRANSFER_MAP_ERROR;

   /* We break the logic down into four steps
    *
    * step 1: determine the required operations independently
    * step 2: look for chances to skip the operations
    * step 3: resolve dependencies between the operations
    * step 4: execute the operations
    */

   flush = virgl_res_needs_flush(vctx, xfer);
   readback = virgl_res_needs_readback(vctx, res, xfer->base.usage,
                                       xfer->base.level);
   /* We need to wait for all cmdbufs, current or previous, that access the
    * resource to finish unless synchronization is disabled.
    */
   wait = !(xfer->base.usage & PIPE_TRANSFER_UNSYNCHRONIZED);

   /* When the transfer range consists of only uninitialized data, we can
    * assume the GPU is not accessing the range and readback is unnecessary.
    * We can proceed as if PIPE_TRANSFER_UNSYNCHRONIZED and
    * PIPE_TRANSFER_DISCARD_RANGE are set.
    */
   if (res->u.b.target == PIPE_BUFFER &&
         !util_ranges_intersect(&res->valid_buffer_range, xfer->base.box.x,
            xfer->base.box.x + xfer->base.box.width)) {
      flush = false;
      readback = false;
      wait = false;
   }

   /* readback has some implications */
   if (readback) {
      /* Readback is yet another command and is transparent to the state
       * trackers.  It should be waited for in all cases, including when
       * PIPE_TRANSFER_UNSYNCHRONIZED is set.
       */
      wait = true;

      /* When the transfer queue has pending writes to this transfer's region,
       * we have to flush before readback.
       */
      if (!flush && virgl_transfer_queue_is_queued(&vctx->queue, xfer))
         flush = true;
   }

   /* XXX This is incorrect and will be removed.  Consider
    *
    *   glTexImage2D(..., data1);
    *   glDrawArrays();
    *   glFlush();
    *   glTexImage2D(..., data2);
    *
    * readback and flush are both false in the second glTexImage2D call.  The
    * draw call might end up seeing data2.  Same applies to buffers with
    * glBufferSubData.
    */
   wait = flush || readback;

   if (flush)
      vctx->base.flush(&vctx->base, NULL, 0);

   /* If we are not allowed to block, and we know that we will have to wait,
    * either because the resource is busy, or because it will become busy due
    * to a readback, return early to avoid performing an incomplete
    * transfer_get. Such an incomplete transfer_get may finish at any time,
    * during which another unsynchronized map could write to the resource
    * contents, leaving the contents in an undefined state.
    */
   if ((xfer->base.usage & PIPE_TRANSFER_DONTBLOCK) &&
       (readback || (wait && vws->resource_is_busy(vws, res->hw_res))))
      return VIRGL_TRANSFER_MAP_ERROR;

   if (readback) {
      vws->transfer_get(vws, res->hw_res, &xfer->base.box, xfer->base.stride,
                        xfer->l_stride, xfer->offset, xfer->base.level);
   }

   if (wait)
      vws->resource_wait(vws, res->hw_res);

   return map_type;
}

static struct pipe_resource *virgl_resource_create(struct pipe_screen *screen,
                                                   const struct pipe_resource *templ)
{
   unsigned vbind;
   struct virgl_screen *vs = virgl_screen(screen);
   struct virgl_resource *res = CALLOC_STRUCT(virgl_resource);

   res->u.b = *templ;
   res->u.b.screen = &vs->base;
   pipe_reference_init(&res->u.b.reference, 1);
   vbind = pipe_to_virgl_bind(vs, templ->bind, templ->flags);
   virgl_resource_layout(&res->u.b, &res->metadata);
   res->hw_res = vs->vws->resource_create(vs->vws, templ->target,
                                          templ->format, vbind,
                                          templ->width0,
                                          templ->height0,
                                          templ->depth0,
                                          templ->array_size,
                                          templ->last_level,
                                          templ->nr_samples,
                                          res->metadata.total_size);
   if (!res->hw_res) {
      FREE(res);
      return NULL;
   }

   res->clean_mask = (1 << VR_MAX_TEXTURE_2D_LEVELS) - 1;

   if (templ->target == PIPE_BUFFER) {
      util_range_init(&res->valid_buffer_range);
      virgl_buffer_init(res);
   } else {
      virgl_texture_init(res);
   }

   return &res->u.b;

}

static struct pipe_resource *virgl_resource_from_handle(struct pipe_screen *screen,
                                                        const struct pipe_resource *templ,
                                                        struct winsys_handle *whandle,
                                                        unsigned usage)
{
   struct virgl_screen *vs = virgl_screen(screen);
   if (templ->target == PIPE_BUFFER)
      return NULL;

   struct virgl_resource *res = CALLOC_STRUCT(virgl_resource);
   res->u.b = *templ;
   res->u.b.screen = &vs->base;
   pipe_reference_init(&res->u.b.reference, 1);

   res->hw_res = vs->vws->resource_create_from_handle(vs->vws, whandle);
   if (!res->hw_res) {
      FREE(res);
      return NULL;
   }

   virgl_texture_init(res);

   return &res->u.b;
}

void virgl_init_screen_resource_functions(struct pipe_screen *screen)
{
    screen->resource_create = virgl_resource_create;
    screen->resource_from_handle = virgl_resource_from_handle;
    screen->resource_get_handle = u_resource_get_handle_vtbl;
    screen->resource_destroy = u_resource_destroy_vtbl;
}

static bool virgl_buffer_transfer_extend(struct pipe_context *ctx,
                                         struct pipe_resource *resource,
                                         unsigned usage,
                                         const struct pipe_box *box,
                                         const void *data)
{
   struct virgl_context *vctx = virgl_context(ctx);
   struct virgl_resource *vbuf = virgl_resource(resource);
   struct virgl_transfer dummy_trans = { 0 };
   bool flush;
   struct virgl_transfer *queued;

   /*
    * Attempts to short circuit the entire process of mapping and unmapping
    * a resource if there is an existing transfer that can be extended.
    * Pessimestically falls back if a flush is required.
    */
   dummy_trans.base.resource = resource;
   dummy_trans.base.usage = usage;
   dummy_trans.base.box = *box;
   dummy_trans.base.stride = vbuf->metadata.stride[0];
   dummy_trans.base.layer_stride = vbuf->metadata.layer_stride[0];
   dummy_trans.offset = box->x;

   flush = virgl_res_needs_flush(vctx, &dummy_trans);
   if (flush && util_ranges_intersect(&vbuf->valid_buffer_range,
                                      box->x, box->x + box->width))
      return false;

   queued = virgl_transfer_queue_extend(&vctx->queue, &dummy_trans);
   if (!queued || !queued->hw_res_map)
      return false;

   memcpy(queued->hw_res_map + dummy_trans.offset, data, box->width);
   util_range_add(&vbuf->valid_buffer_range, box->x, box->x + box->width);

   return true;
}

static void virgl_buffer_subdata(struct pipe_context *pipe,
                                 struct pipe_resource *resource,
                                 unsigned usage, unsigned offset,
                                 unsigned size, const void *data)
{
   struct pipe_transfer *transfer;
   uint8_t *map;
   struct pipe_box box;

   assert(!(usage & PIPE_TRANSFER_READ));

   /* the write flag is implicit by the nature of buffer_subdata */
   usage |= PIPE_TRANSFER_WRITE;

   if (offset == 0 && size == resource->width0)
      usage |= PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE;
   else
      usage |= PIPE_TRANSFER_DISCARD_RANGE;

   u_box_1d(offset, size, &box);

   if (usage & PIPE_TRANSFER_DISCARD_RANGE &&
       virgl_buffer_transfer_extend(pipe, resource, usage, &box, data))
      return;

   map = pipe->transfer_map(pipe, resource, 0, usage, &box, &transfer);
   if (map) {
      memcpy(map, data, size);
      pipe_transfer_unmap(pipe, transfer);
   }
}

void virgl_init_context_resource_functions(struct pipe_context *ctx)
{
    ctx->transfer_map = u_transfer_map_vtbl;
    ctx->transfer_flush_region = u_transfer_flush_region_vtbl;
    ctx->transfer_unmap = u_transfer_unmap_vtbl;
    ctx->buffer_subdata = virgl_buffer_subdata;
    ctx->texture_subdata = u_default_texture_subdata;
}

void virgl_resource_layout(struct pipe_resource *pt,
                           struct virgl_resource_metadata *metadata)
{
   unsigned level, nblocksy;
   unsigned width = pt->width0;
   unsigned height = pt->height0;
   unsigned depth = pt->depth0;
   unsigned buffer_size = 0;

   for (level = 0; level <= pt->last_level; level++) {
      unsigned slices;

      if (pt->target == PIPE_TEXTURE_CUBE)
         slices = 6;
      else if (pt->target == PIPE_TEXTURE_3D)
         slices = depth;
      else
         slices = pt->array_size;

      nblocksy = util_format_get_nblocksy(pt->format, height);
      metadata->stride[level] = util_format_get_stride(pt->format, width);
      metadata->layer_stride[level] = nblocksy * metadata->stride[level];
      metadata->level_offset[level] = buffer_size;

      buffer_size += slices * metadata->layer_stride[level];

      width = u_minify(width, 1);
      height = u_minify(height, 1);
      depth = u_minify(depth, 1);
   }

   if (pt->nr_samples <= 1)
      metadata->total_size = buffer_size;
   else /* don't create guest backing store for MSAA */
      metadata->total_size = 0;
}

struct virgl_transfer *
virgl_resource_create_transfer(struct slab_child_pool *pool,
                               struct pipe_resource *pres,
                               const struct virgl_resource_metadata *metadata,
                               unsigned level, unsigned usage,
                               const struct pipe_box *box)
{
   struct virgl_transfer *trans;
   enum pipe_format format = pres->format;
   const unsigned blocksy = box->y / util_format_get_blockheight(format);
   const unsigned blocksx = box->x / util_format_get_blockwidth(format);

   unsigned offset = metadata->level_offset[level];
   if (pres->target == PIPE_TEXTURE_CUBE ||
       pres->target == PIPE_TEXTURE_CUBE_ARRAY ||
       pres->target == PIPE_TEXTURE_3D ||
       pres->target == PIPE_TEXTURE_2D_ARRAY) {
      offset += box->z * metadata->layer_stride[level];
   }
   else if (pres->target == PIPE_TEXTURE_1D_ARRAY) {
      offset += box->z * metadata->stride[level];
      assert(box->y == 0);
   } else if (pres->target == PIPE_BUFFER) {
      assert(box->y == 0 && box->z == 0);
   } else {
      assert(box->z == 0);
   }

   offset += blocksy * metadata->stride[level];
   offset += blocksx * util_format_get_blocksize(format);

   trans = slab_alloc(pool);
   if (!trans)
      return NULL;

   trans->base.resource = pres;
   trans->base.level = level;
   trans->base.usage = usage;
   trans->base.box = *box;
   trans->base.stride = metadata->stride[level];
   trans->base.layer_stride = metadata->layer_stride[level];
   trans->offset = offset;
   util_range_init(&trans->range);
   trans->copy_src_res = NULL;
   trans->copy_src_offset = 0;

   if (trans->base.resource->target != PIPE_TEXTURE_3D &&
       trans->base.resource->target != PIPE_TEXTURE_CUBE &&
       trans->base.resource->target != PIPE_TEXTURE_1D_ARRAY &&
       trans->base.resource->target != PIPE_TEXTURE_2D_ARRAY &&
       trans->base.resource->target != PIPE_TEXTURE_CUBE_ARRAY)
      trans->l_stride = 0;
   else
      trans->l_stride = trans->base.layer_stride;

   return trans;
}

void virgl_resource_destroy_transfer(struct slab_child_pool *pool,
                                     struct virgl_transfer *trans)
{
   pipe_resource_reference(&trans->copy_src_res, NULL);
   util_range_destroy(&trans->range);
   slab_free(pool, trans);
}

void virgl_resource_destroy(struct pipe_screen *screen,
                            struct pipe_resource *resource)
{
   struct virgl_screen *vs = virgl_screen(screen);
   struct virgl_resource *res = virgl_resource(resource);

   if (res->u.b.target == PIPE_BUFFER)
      util_range_destroy(&res->valid_buffer_range);

   vs->vws->resource_unref(vs->vws, res->hw_res);
   FREE(res);
}

boolean virgl_resource_get_handle(struct pipe_screen *screen,
                                  struct pipe_resource *resource,
                                  struct winsys_handle *whandle)
{
   struct virgl_screen *vs = virgl_screen(screen);
   struct virgl_resource *res = virgl_resource(resource);

   if (res->u.b.target == PIPE_BUFFER)
      return FALSE;

   return vs->vws->resource_get_handle(vs->vws, res->hw_res,
                                       res->metadata.stride[0],
                                       whandle);
}

void virgl_resource_dirty(struct virgl_resource *res, uint32_t level)
{
   if (res) {
      if (res->u.b.target == PIPE_BUFFER)
         res->clean_mask &= ~1;
      else
         res->clean_mask &= ~(1 << level);
   }
}

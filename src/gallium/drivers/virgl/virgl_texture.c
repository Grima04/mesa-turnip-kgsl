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

static void virgl_copy_region_with_blit(struct pipe_context *pipe,
                                        struct pipe_resource *dst,
                                        unsigned dst_level,
                                        const struct pipe_box *dst_box,
                                        struct pipe_resource *src,
                                        unsigned src_level,
                                        const struct pipe_box *src_box)
{
   struct pipe_blit_info blit;

   assert(src_box->width == dst_box->width);
   assert(src_box->height == dst_box->height);
   assert(src_box->depth == dst_box->depth);

   memset(&blit, 0, sizeof(blit));
   blit.src.resource = src;
   blit.src.format = src->format;
   blit.src.level = src_level;
   blit.src.box = *src_box;
   blit.dst.resource = dst;
   blit.dst.format = dst->format;
   blit.dst.level = dst_level;
   blit.dst.box.x = dst_box->x;
   blit.dst.box.y = dst_box->y;
   blit.dst.box.z = dst_box->z;
   blit.dst.box.width = src_box->width;
   blit.dst.box.height = src_box->height;
   blit.dst.box.depth = src_box->depth;
   blit.mask = util_format_get_mask(src->format) &
      util_format_get_mask(dst->format);
   blit.filter = PIPE_TEX_FILTER_NEAREST;

   if (blit.mask) {
      pipe->blit(pipe, &blit);
   }
}

static unsigned temp_bind(unsigned orig)
{
   unsigned warn = ~(PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL |
                     PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_DISPLAY_TARGET);
   if (orig & warn)
      debug_printf("VIRGL: Warning, possibly unhandled bind: %x\n",
                   orig & warn);

   return orig & (PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_RENDER_TARGET);
}

static void virgl_init_temp_resource_from_box(struct pipe_resource *res,
                                              struct pipe_resource *orig,
                                              const struct pipe_box *box,
                                              unsigned level, unsigned flags)
{
   memset(res, 0, sizeof(*res));
   res->bind = temp_bind(orig->bind);
   res->format = orig->format;
   res->width0 = box->width;
   res->height0 = box->height;
   res->depth0 = 1;
   res->array_size = 1;
   res->usage = PIPE_USAGE_STAGING;
   res->flags = flags;

   /* We must set the correct texture target and dimensions for a 3D box. */
   if (box->depth > 1 && util_max_layer(orig, level) > 0)
      res->target = orig->target;
   else
      res->target = PIPE_TEXTURE_2D;

   switch (res->target) {
   case PIPE_TEXTURE_1D_ARRAY:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_CUBE_ARRAY:
      res->array_size = box->depth;
      break;
   case PIPE_TEXTURE_3D:
      res->depth0 = box->depth;
      break;
   default:
      break;
   }
}

static void *texture_transfer_map_plain(struct pipe_context *ctx,
                                        struct pipe_resource *resource,
                                        unsigned level,
                                        unsigned usage,
                                        const struct pipe_box *box,
                                        struct pipe_transfer **transfer)
{
   struct virgl_context *vctx = virgl_context(ctx);
   struct virgl_winsys *vws = virgl_screen(ctx->screen)->vws;
   struct virgl_resource *vtex = virgl_resource(resource);
   struct virgl_transfer *trans;

   trans = virgl_resource_create_transfer(&vctx->transfer_pool, resource,
                                          &vtex->metadata, level, usage, box);
   trans->resolve_transfer = NULL;

   assert(resource->nr_samples <= 1);

   if (virgl_res_needs_flush(vctx, trans))
      ctx->flush(ctx, NULL, 0);

   if (virgl_res_needs_readback(vctx, vtex, usage, level)) {
      vws->transfer_get(vws, vtex->hw_res, box, trans->base.stride,
                        trans->l_stride, trans->offset, level);

      vws->resource_wait(vws, vtex->hw_res);
   }

   void *ptr = vws->resource_map(vws, vtex->hw_res);
   if (!ptr) {
      virgl_resource_destroy_transfer(&vctx->transfer_pool, trans);
      return NULL;
   }

   *transfer = &trans->base;
   return ptr + trans->offset;
}

static void *texture_transfer_map_resolve(struct pipe_context *ctx,
                                          struct pipe_resource *resource,
                                          unsigned level,
                                          unsigned usage,
                                          const struct pipe_box *box,
                                          struct pipe_transfer **transfer)
{
   struct virgl_context *vctx = virgl_context(ctx);
   struct pipe_resource templ, *resolve_tmp;
   struct virgl_transfer *trans;

   trans = virgl_resource_create_transfer(&vctx->transfer_pool, resource,
                                          &virgl_resource(resource)->metadata,
                                          level, usage, box);
   if (!trans)
      return NULL;

   virgl_init_temp_resource_from_box(&templ, resource, box, level, 0);

   resolve_tmp = ctx->screen->resource_create(ctx->screen, &templ);
   if (!resolve_tmp)
      return NULL;

   struct pipe_box dst_box = *box;
   dst_box.x = dst_box.y = dst_box.z = 0;

   virgl_copy_region_with_blit(ctx, resolve_tmp, 0, &dst_box, resource, level, box);
   ctx->flush(ctx, NULL, 0);

   void *ptr = texture_transfer_map_plain(ctx, resolve_tmp, 0, usage, &dst_box,
                                          &trans->resolve_transfer);
   if (!ptr)
      goto fail;

   *transfer = &trans->base;
   trans->base.stride = trans->resolve_transfer->stride;
   trans->base.layer_stride = trans->resolve_transfer->layer_stride;
   return ptr;

fail:
   pipe_resource_reference(&resolve_tmp, NULL);
   virgl_resource_destroy_transfer(&vctx->transfer_pool, trans);
   return NULL;
}

static void *virgl_texture_transfer_map(struct pipe_context *ctx,
                                        struct pipe_resource *resource,
                                        unsigned level,
                                        unsigned usage,
                                        const struct pipe_box *box,
                                        struct pipe_transfer **transfer)
{
   if (resource->nr_samples > 1)
      return texture_transfer_map_resolve(ctx, resource, level, usage, box,
                                          transfer);

   return texture_transfer_map_plain(ctx, resource, level, usage, box, transfer);
}

static void flush_data(struct pipe_context *ctx,
                       struct virgl_transfer *trans,
                       const struct pipe_box *box)
{
   struct virgl_winsys *vws = virgl_screen(ctx->screen)->vws;
   vws->transfer_put(vws, virgl_resource(trans->base.resource)->hw_res, box,
                     trans->base.stride, trans->l_stride, trans->offset,
                     trans->base.level);
}

static void virgl_texture_transfer_unmap(struct pipe_context *ctx,
                                         struct pipe_transfer *transfer)
{
   struct virgl_context *vctx = virgl_context(ctx);
   struct virgl_transfer *trans = virgl_transfer(transfer);
   bool queue_unmap = false;

   if (transfer->usage & PIPE_TRANSFER_WRITE &&
       (transfer->usage & PIPE_TRANSFER_FLUSH_EXPLICIT) == 0) {

      if (trans->resolve_transfer) {
         flush_data(ctx, virgl_transfer(trans->resolve_transfer),
                    &trans->resolve_transfer->box);

         virgl_copy_region_with_blit(ctx,
                                     trans->base.resource, trans->base.level,
                                     &transfer->box,
                                     trans->resolve_transfer->resource, 0,
                                     &trans->resolve_transfer->box);
         ctx->flush(ctx, NULL, 0);
      } else
         queue_unmap = true;
   }

   if (trans->resolve_transfer) {
      pipe_resource_reference(&trans->resolve_transfer->resource, NULL);
      virgl_resource_destroy_transfer(&vctx->transfer_pool,
                                      virgl_transfer(trans->resolve_transfer));
   }

   if (queue_unmap)
      virgl_transfer_queue_unmap(&vctx->queue, trans);
   else
      virgl_resource_destroy_transfer(&vctx->transfer_pool, trans);
}

static const struct u_resource_vtbl virgl_texture_vtbl =
{
   virgl_resource_get_handle,           /* get_handle */
   virgl_resource_destroy,              /* resource_destroy */
   virgl_texture_transfer_map,          /* transfer_map */
   NULL,                                /* transfer_flush_region */
   virgl_texture_transfer_unmap,        /* transfer_unmap */
};

void virgl_texture_init(struct virgl_resource *res)
{
   res->u.vtbl = &virgl_texture_vtbl;
}

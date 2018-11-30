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
#include "virgl_context.h"
#include "virgl_resource.h"
#include "virgl_screen.h"

bool virgl_res_needs_flush_wait(struct virgl_context *vctx,
                                struct virgl_resource *res,
                                unsigned usage)
{
   struct virgl_screen *vs = virgl_screen(vctx->base.screen);

   if ((!(usage & PIPE_TRANSFER_UNSYNCHRONIZED)) && vs->vws->res_is_referenced(vs->vws, vctx->cbuf, res->hw_res)) {
      return true;
   }
   return false;
}

bool virgl_res_needs_readback(struct virgl_context *vctx,
                              struct virgl_resource *res,
                              unsigned usage)
{
   bool readback = true;
   if (res->clean)
      readback = false;
   else if (usage & PIPE_TRANSFER_DISCARD_RANGE)
      readback = false;
   else if ((usage & (PIPE_TRANSFER_WRITE | PIPE_TRANSFER_FLUSH_EXPLICIT)) ==
            (PIPE_TRANSFER_WRITE | PIPE_TRANSFER_FLUSH_EXPLICIT))
      readback = false;
   return readback;
}

static struct pipe_resource *virgl_resource_create(struct pipe_screen *screen,
                                                   const struct pipe_resource *templ)
{
    struct virgl_screen *vs = virgl_screen(screen);
    if (templ->target == PIPE_BUFFER)
        return virgl_buffer_create(vs, templ);
    else
        return virgl_texture_create(vs, templ);
}

static struct pipe_resource *virgl_resource_from_handle(struct pipe_screen *screen,
                                                        const struct pipe_resource *templ,
                                                        struct winsys_handle *whandle,
                                                        unsigned usage)
{
    struct virgl_screen *vs = virgl_screen(screen);
    if (templ->target == PIPE_BUFFER)
        return NULL;
    else
        return virgl_texture_from_handle(vs, templ, whandle);
}

void virgl_init_screen_resource_functions(struct pipe_screen *screen)
{
    screen->resource_create = virgl_resource_create;
    screen->resource_from_handle = virgl_resource_from_handle;
    screen->resource_get_handle = u_resource_get_handle_vtbl;
    screen->resource_destroy = u_resource_destroy_vtbl;
}

static void virgl_buffer_subdata(struct pipe_context *pipe,
                                 struct pipe_resource *resource,
                                 unsigned usage, unsigned offset,
                                 unsigned size, const void *data)
{
   struct pipe_box box;

   if (offset == 0 && size == resource->width0)
      usage |= PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE;
   else
      usage |= PIPE_TRANSFER_DISCARD_RANGE;

   u_box_1d(offset, size, &box);

   if (size >= (VIRGL_MAX_CMDBUF_DWORDS * 4))
      u_default_buffer_subdata(pipe, resource, usage, offset, size, data);
   else
      virgl_transfer_inline_write(pipe, resource, 0, usage, &box, data, 0, 0);
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
virgl_resource_create_transfer(struct pipe_context *ctx,
                               struct pipe_resource *pres,
                               const struct virgl_resource_metadata *metadata,
                               unsigned level, unsigned usage,
                               const struct pipe_box *box)
{
   struct virgl_transfer *trans;
   enum pipe_format format = pres->format;
   struct virgl_context *vctx = virgl_context(ctx);
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
   }
   else {
      assert(box->z == 0);
   }

   offset += blocksy * metadata->stride[level];
   offset += blocksx * util_format_get_blocksize(format);

   trans = slab_alloc(&vctx->transfer_pool);
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

void virgl_resource_destroy_transfer(struct virgl_context *vctx,
                                     struct virgl_transfer *trans)
{
   util_range_destroy(&trans->range);
   slab_free(&vctx->transfer_pool, trans);
}

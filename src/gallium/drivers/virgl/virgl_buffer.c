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

#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "virgl_context.h"
#include "virgl_encode.h"
#include "virgl_resource.h"
#include "virgl_screen.h"

static void *virgl_buffer_transfer_map(struct pipe_context *ctx,
                                       struct pipe_resource *resource,
                                       unsigned level,
                                       unsigned usage,
                                       const struct pipe_box *box,
                                       struct pipe_transfer **transfer)
{
   struct virgl_context *vctx = virgl_context(ctx);
   struct virgl_screen *vs = virgl_screen(ctx->screen);
   struct virgl_resource *vbuf = virgl_resource(resource);
   struct virgl_transfer *trans;
   enum virgl_transfer_map_type map_type;
   void *map_addr;

   trans = virgl_resource_create_transfer(vctx, resource,
                                          &vbuf->metadata, level, usage, box);

   map_type = virgl_resource_transfer_prepare(vctx, trans);
   switch (map_type) {
   case VIRGL_TRANSFER_MAP_REALLOC:
      if (!virgl_resource_realloc(vctx, vbuf)) {
         map_addr = NULL;
         break;
      }
      vs->vws->resource_reference(vs->vws, &trans->hw_res, vbuf->hw_res);
      /* fall through */
   case VIRGL_TRANSFER_MAP_HW_RES:
      trans->hw_res_map = vs->vws->resource_map(vs->vws, vbuf->hw_res);
      if (trans->hw_res_map)
         map_addr = trans->hw_res_map + trans->offset;
      else
         map_addr = NULL;
      break;
   case VIRGL_TRANSFER_MAP_STAGING:
      map_addr = virgl_staging_map(vctx, trans);
      /* Copy transfers don't make use of hw_res_map at the moment. */
      trans->hw_res_map = NULL;
      break;
   case VIRGL_TRANSFER_MAP_ERROR:
   default:
      trans->hw_res_map = NULL;
      map_addr = NULL;
      break;
   }

   if (!map_addr) {
      virgl_resource_destroy_transfer(vctx, trans);
      return NULL;
   }

   /* For the checks below to be able to use 'usage', we assume that
    * transfer preparation doesn't affect the usage.
    */
   assert(usage == trans->base.usage);

   /* If we are doing a whole resource discard with a hw_res map, the buffer
    * storage can now be considered unused and we don't care about previous
    * contents.  We can thus mark the storage as uninitialized, but only if the
    * buffer is not host writable (in which case we can't clear the valid
    * range, since that would result in missed readbacks in future transfers).
    * We only do this for VIRGL_TRANSFER_MAP_HW_RES, since for
    * VIRGL_TRANSFER_MAP_REALLOC we already take care of the buffer range when
    * reallocating and rebinding, and VIRGL_TRANSFER_MAP_STAGING is not
    * currently used for whole resource discards.
    */
   if (map_type == VIRGL_TRANSFER_MAP_HW_RES &&
       (usage & PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE) &&
       (vbuf->clean_mask & 1)) {
      util_range_set_empty(&vbuf->valid_buffer_range);
   }

   if (usage & PIPE_TRANSFER_WRITE)
       util_range_add(&vbuf->valid_buffer_range, box->x, box->x + box->width);

   *transfer = &trans->base;
   return map_addr;
}

static void virgl_buffer_transfer_unmap(struct pipe_context *ctx,
                                        struct pipe_transfer *transfer)
{
   struct virgl_context *vctx = virgl_context(ctx);
   struct virgl_transfer *trans = virgl_transfer(transfer);

   if (trans->base.usage & PIPE_TRANSFER_WRITE) {
      if (transfer->usage & PIPE_TRANSFER_FLUSH_EXPLICIT) {
         if (trans->range.end <= trans->range.start) {
            virgl_resource_destroy_transfer(vctx, trans);
            return;
         }

         transfer->box.x += trans->range.start;
         transfer->box.width = trans->range.end - trans->range.start;
         trans->offset = transfer->box.x;
      }

      if (trans->copy_src_hw_res) {
         virgl_encode_copy_transfer(vctx, trans);
         virgl_resource_destroy_transfer(vctx, trans);
      } else {
         virgl_transfer_queue_unmap(&vctx->queue, trans);
      }
   } else
      virgl_resource_destroy_transfer(vctx, trans);
}

static void virgl_buffer_transfer_flush_region(struct pipe_context *ctx,
                                               struct pipe_transfer *transfer,
                                               const struct pipe_box *box)
{
   struct virgl_transfer *trans = virgl_transfer(transfer);

   /*
    * FIXME: This is not optimal.  For example,
    *
    * glMapBufferRange(.., 0, 100, GL_MAP_FLUSH_EXPLICIT_BIT)
    * glFlushMappedBufferRange(.., 25, 30)
    * glFlushMappedBufferRange(.., 65, 70)
    *
    * We'll end up flushing 25 --> 70.
    */
   util_range_add(&trans->range, box->x, box->x + box->width);
}

static const struct u_resource_vtbl virgl_buffer_vtbl =
{
   u_default_resource_get_handle,            /* get_handle */
   virgl_resource_destroy,                   /* resource_destroy */
   virgl_buffer_transfer_map,                /* transfer_map */
   virgl_buffer_transfer_flush_region,       /* transfer_flush_region */
   virgl_buffer_transfer_unmap,              /* transfer_unmap */
};

void virgl_buffer_init(struct virgl_resource *res)
{
   res->u.vtbl = &virgl_buffer_vtbl;
}

/*
 * Copyright © 2017 Intel Corporation
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
#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_upload_mgr.h"
#include "util/ralloc.h"
#include "iris_context.h"
#include "iris_resource.h"
#include "iris_screen.h"
#include "intel/compiler/brw_compiler.h"

/**
 * For debugging purposes, this returns a time in seconds.
 */
double
get_time(void)
{
   struct timespec tp;

   clock_gettime(CLOCK_MONOTONIC, &tp);

   return tp.tv_sec + tp.tv_nsec / 1000000000.0;
}

/*
 * query
 */
struct iris_query {
   unsigned query;
};
static struct pipe_query *
iris_create_query(struct pipe_context *ctx, unsigned query_type, unsigned index)
{
   struct iris_query *query = calloc(1, sizeof(struct iris_query));

   return (struct pipe_query *)query;
}

static void
iris_destroy_query(struct pipe_context *ctx, struct pipe_query *query)
{
   free(query);
}

static boolean
iris_begin_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static bool
iris_end_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static boolean
iris_get_query_result(struct pipe_context *ctx,
                      struct pipe_query *query,
                      boolean wait,
                      union pipe_query_result *vresult)
{
   uint64_t *result = (uint64_t*)vresult;

   *result = 0;
   return TRUE;
}

static void
iris_set_active_query_state(struct pipe_context *pipe, boolean enable)
{
}


/*
 * transfer
 */
static void *
iris_transfer_map(struct pipe_context *pipe,
                  struct pipe_resource *resource,
                  unsigned level,
                  enum pipe_transfer_usage usage,
                  const struct pipe_box *box,
                  struct pipe_transfer **ptransfer)
{
   struct pipe_transfer *transfer;
   struct iris_resource *res = (struct iris_resource *)resource;

   transfer = calloc(1, sizeof(struct pipe_transfer));
   if (!transfer)
      return NULL;
   pipe_resource_reference(&transfer->resource, resource);
   transfer->level = level;
   transfer->usage = usage;
   transfer->box = *box;
   transfer->stride = 1;
   transfer->layer_stride = 1;
   *ptransfer = transfer;

   return NULL;
}

static void
iris_transfer_flush_region(struct pipe_context *pipe,
                           struct pipe_transfer *transfer,
                           const struct pipe_box *box)
{
}

static void
iris_transfer_unmap(struct pipe_context *pipe,
                    struct pipe_transfer *transfer)
{
   pipe_resource_reference(&transfer->resource, NULL);
   free(transfer);
}

static void
iris_buffer_subdata(struct pipe_context *pipe,
                    struct pipe_resource *resource,
                    unsigned usage, unsigned offset,
                    unsigned size, const void *data)
{
}

static void
iris_texture_subdata(struct pipe_context *pipe,
                     struct pipe_resource *resource,
                     unsigned level,
                     unsigned usage,
                     const struct pipe_box *box,
                     const void *data,
                     unsigned stride,
                     unsigned layer_stride)
{
}


/*
  *clear/copy
 */
static void
iris_clear(struct pipe_context *ctx, unsigned buffers,
           const union pipe_color_union *color, double depth, unsigned stencil)
{
}

static void
iris_clear_render_target(struct pipe_context *ctx,
                         struct pipe_surface *dst,
                         const union pipe_color_union *color,
                         unsigned dstx, unsigned dsty,
                         unsigned width, unsigned height,
                         bool render_condition_enabled)
{
}

static void
iris_clear_depth_stencil(struct pipe_context *ctx,
                         struct pipe_surface *dst,
                         unsigned clear_flags,
                         double depth,
                         unsigned stencil,
                         unsigned dstx, unsigned dsty,
                         unsigned width, unsigned height,
                         bool render_condition_enabled)
{
}

static void
iris_resource_copy_region(struct pipe_context *ctx,
                          struct pipe_resource *dst,
                          unsigned dst_level,
                          unsigned dstx, unsigned dsty, unsigned dstz,
                          struct pipe_resource *src,
                          unsigned src_level,
                          const struct pipe_box *src_box)
{
}

static void
iris_blit(struct pipe_context *ctx, const struct pipe_blit_info *info)
{
}


static void
iris_flush_resource(struct pipe_context *ctx, struct pipe_resource *resource)
{
}


/*
 * context
 */
static void
iris_flush(struct pipe_context *ctx,
           struct pipe_fence_handle **fence,
           unsigned flags)
{
   if (fence)
      *fence = NULL;
}

static void
iris_destroy_context(struct pipe_context *ctx)
{
   if (ctx->stream_uploader)
      u_upload_destroy(ctx->stream_uploader);

   free(ctx);
}

static boolean
iris_generate_mipmap(struct pipe_context *ctx,
                     struct pipe_resource *resource,
                     enum pipe_format format,
                     unsigned base_level,
                     unsigned last_level,
                     unsigned first_layer,
                     unsigned last_layer)
{
   return true;
}

static void
iris_set_debug_callback(struct pipe_context *ctx,
                        const struct pipe_debug_callback *cb)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (cb)
      ice->dbg = *cb;
   else
      memset(&ice->dbg, 0, sizeof(ice->dbg));
}

struct pipe_context *
iris_create_context(struct pipe_screen *screen, void *priv, unsigned flags)
{
   struct iris_context *ice = calloc(1, sizeof(struct iris_context));

   if (!ice)
      return NULL;

   struct pipe_context *ctx = &ice->ctx;

   ctx->screen = screen;
   ctx->priv = priv;

   ctx->stream_uploader = u_upload_create_default(ctx);
   if (!ctx->stream_uploader) {
      free(ctx);
      return NULL;
   }
   ctx->const_uploader = ctx->stream_uploader;

   ctx->destroy = iris_destroy_context;
   ctx->flush = iris_flush;
   ctx->clear = iris_clear;
   ctx->clear_render_target = iris_clear_render_target;
   ctx->clear_depth_stencil = iris_clear_depth_stencil;
   ctx->resource_copy_region = iris_resource_copy_region;
   ctx->generate_mipmap = iris_generate_mipmap;
   ctx->blit = iris_blit;
   ctx->flush_resource = iris_flush_resource;
   ctx->create_query = iris_create_query;
   ctx->destroy_query = iris_destroy_query;
   ctx->begin_query = iris_begin_query;
   ctx->end_query = iris_end_query;
   ctx->get_query_result = iris_get_query_result;
   ctx->set_active_query_state = iris_set_active_query_state;
   ctx->transfer_map = iris_transfer_map;
   ctx->transfer_flush_region = iris_transfer_flush_region;
   ctx->transfer_unmap = iris_transfer_unmap;
   ctx->buffer_subdata = iris_buffer_subdata;
   ctx->texture_subdata = iris_texture_subdata;
   ctx->set_debug_callback = iris_set_debug_callback;
   iris_init_program_functions(ctx);
   iris_init_state_functions(ctx);

   return ctx;
}

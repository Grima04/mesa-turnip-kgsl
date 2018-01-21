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
#include <time.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/ralloc.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_upload_mgr.h"
#include "i915_drm.h"
#include "iris_context.h"
#include "iris_resource.h"
#include "iris_screen.h"

static void
iris_flush(struct pipe_context *ctx,
           struct pipe_fence_handle **fence,
           unsigned flags)
{
   if (fence)
      *fence = NULL;
}

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

static void
iris_destroy_context(struct pipe_context *ctx)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (ctx->stream_uploader)
      u_upload_destroy(ctx->stream_uploader);

   ralloc_free(ice);
}

struct pipe_context *
iris_create_context(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct iris_screen *screen = (struct iris_screen*)pscreen;
   struct iris_context *ice = rzalloc(NULL, struct iris_context);

   if (!ice)
      return NULL;

   struct pipe_context *ctx = &ice->ctx;

   ctx->screen = pscreen;
   ctx->priv = priv;

   ctx->stream_uploader = u_upload_create_default(ctx);
   if (!ctx->stream_uploader) {
      free(ctx);
      return NULL;
   }
   ctx->const_uploader = ctx->stream_uploader;

   ctx->destroy = iris_destroy_context;
   ctx->flush = iris_flush;
   ctx->set_debug_callback = iris_set_debug_callback;

   iris_init_blit_functions(ctx);
   iris_init_clear_functions(ctx);
   iris_init_program_functions(ctx);
   iris_init_resource_functions(ctx);
   iris_init_state_functions(ctx);
   iris_init_query_functions(ctx);

   iris_init_program_cache(ice);

   iris_init_batch(&ice->render_batch, screen, &ice->dbg, I915_EXEC_RENDER);

   return ctx;
}

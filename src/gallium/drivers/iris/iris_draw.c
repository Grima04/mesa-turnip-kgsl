/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iris_draw.c
 *
 * The main driver hooks for drawing and launching compute shaders.
 */

#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_transfer.h"
#include "intel/compiler/brw_compiler.h"
#include "iris_context.h"

/**
 * The pipe->draw_vbo() driver hook.  Performs a draw on the GPU.
 */
void
iris_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *info)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_batch *batch = &ice->render_batch;

   if (unlikely(INTEL_DEBUG & DEBUG_REEMIT))
      ice->state.dirty |= ~0ull;

   iris_batch_maybe_flush(batch, 1500);

   // XXX: actually do brw_cache_flush_for_*
   // XXX: CS stall is really expensive
   iris_emit_pipe_control_flush(batch,
                                PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                                PIPE_CONTROL_RENDER_TARGET_FLUSH |
                                PIPE_CONTROL_CS_STALL);

   iris_emit_pipe_control_flush(batch,
                                PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
                                PIPE_CONTROL_CONST_CACHE_INVALIDATE);

   iris_cache_sets_clear(batch);
   // XXX: ^^^

   iris_update_compiled_shaders(ice);

   if (iris_binder_is_empty(&batch->binder)) {
      ice->state.dirty |= IRIS_DIRTY_BINDINGS_VS |
                          IRIS_DIRTY_BINDINGS_TCS |
                          IRIS_DIRTY_BINDINGS_TES |
                          IRIS_DIRTY_BINDINGS_GS |
                          IRIS_DIRTY_BINDINGS_FS;
   }

   // XXX: don't do this unless things are dirty...
   iris_binder_reserve_3d(batch, ice);
   ice->vtbl.upload_render_state(ice, batch, info);

   ice->state.dirty = 0ull;

   // XXX: don't flush always
   //iris_batch_flush(batch);
}

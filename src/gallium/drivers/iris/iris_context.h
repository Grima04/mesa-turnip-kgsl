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
#ifndef IRIS_CONTEXT_H
#define IRIS_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "intel/common/gen_debug.h"
#include "iris_screen.h"

struct iris_bo;
struct iris_batch;

#define IRIS_MAX_TEXTURE_SAMPLERS 32
#define IRIS_MAX_VIEWPORTS 16

enum iris_dirty {
   IRIS_DIRTY_COLOR_CALC_STATE         = (1ull <<  0),
   IRIS_DIRTY_POLYGON_STIPPLE          = (1ull <<  1),
   IRIS_DIRTY_SCISSOR_RECT             = (1ull <<  2),
   IRIS_DIRTY_WM_DEPTH_STENCIL         = (1ull <<  3),
   IRIS_DIRTY_CC_VIEWPORT              = (1ull <<  4),
   IRIS_DIRTY_SF_CL_VIEWPORT           = (1ull <<  5),
   IRIS_DIRTY_PS_BLEND                 = (1ull <<  6),
   IRIS_DIRTY_BLEND_STATE              = (1ull <<  7),
   IRIS_DIRTY_RASTER                   = (1ull <<  8),
   IRIS_DIRTY_CLIP                     = (1ull <<  9),
   IRIS_DIRTY_SCISSOR                  = (1ull << 10),
   IRIS_DIRTY_LINE_STIPPLE             = (1ull << 11),
   IRIS_DIRTY_VERTEX_ELEMENTS          = (1ull << 12),
   IRIS_DIRTY_MULTISAMPLE              = (1ull << 13),
};

struct iris_depth_stencil_alpha_state;

struct iris_context {
   struct pipe_context ctx;

   struct pipe_debug_callback dbg;

   struct {
      uint64_t dirty;
      unsigned num_viewports; // XXX: can viewports + scissors be different?
      unsigned num_scissors;
      struct iris_blend_state *cso_blend;
      struct iris_rasterizer_state *cso_rast;
      struct iris_depth_stencil_alpha_state *cso_zsa;
      struct iris_vertex_element_state *cso_vertex_elements;
      struct iris_viewport_state *cso_vp;
      struct iris_depth_state *cso_depth;
      struct pipe_blend_color blend_color;
      struct pipe_poly_stipple poly_stipple;
      struct pipe_scissor_state scissors[IRIS_MAX_VIEWPORTS];
      struct pipe_stencil_ref stencil_ref;
      struct pipe_framebuffer_state framebuffer;
   } state;
};

#define perf_debug(dbg, ...) do {                      \
   if (INTEL_DEBUG & DEBUG_PERF)                       \
      dbg_printf(__VA_ARGS__);                         \
   if (unlikely(dbg))                                  \
      pipe_debug_message(dbg, PERF_INFO, __VA_ARGS__); \
} while(0)

double get_time(void);

struct pipe_context *
iris_create_context(struct pipe_screen *screen, void *priv, unsigned flags);

void iris_init_program_functions(struct pipe_context *ctx);
void iris_init_state_functions(struct pipe_context *ctx);

void iris_upload_render_state(struct iris_context *ice, struct iris_batch *batch);
void iris_destroy_state(struct iris_context *ice);

#endif

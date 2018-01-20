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

static void
iris_clear(struct pipe_context *ctx,
           unsigned buffers,
           const union pipe_color_union *color,
           double depth,
           unsigned stencil)
{
}

static void
iris_clear_render_target(struct pipe_context *ctx,
                         struct pipe_surface *dst,
                         const union pipe_color_union *color,
                         unsigned dst_x, unsigned dst_y,
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
                         unsigned dst_x, unsigned dst_y,
                         unsigned width, unsigned height,
                         bool render_condition_enabled)
{
}

void
iris_init_clear_functions(struct pipe_context *ctx)
{
   ctx->clear = iris_clear;
   ctx->clear_render_target = iris_clear_render_target;
   ctx->clear_depth_stencil = iris_clear_depth_stencil;
}

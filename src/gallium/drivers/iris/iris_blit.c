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
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/ralloc.h"
#include "intel/blorp/blorp.h"
#include "iris_context.h"
#include "iris_resource.h"
#include "iris_screen.h"

void
iris_blorp_surf_for_resource(struct blorp_surf *surf,
                             struct pipe_resource *p_res,
                             enum isl_aux_usage aux_usage,
                             bool is_render_target)
{
   struct iris_resource *res = (void *) p_res;

   *surf = (struct blorp_surf) {
      .surf = &res->surf,
      .addr = (struct blorp_address) {
         .buffer = res->bo,
         .offset = 0, // XXX: ???
         .reloc_flags = is_render_target ? EXEC_OBJECT_WRITE : 0,
         .mocs = I915_MOCS_CACHED, // XXX: BDW MOCS, PTE MOCS
      },
      .aux_usage = aux_usage,
   };

   assert(surf->aux_usage == ISL_AUX_USAGE_NONE);
}

static enum isl_format
iris_get_blorp_format(enum pipe_format pf)
{
   switch (pf) {
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      return ISL_FORMAT_R24_UNORM_X8_TYPELESS;
   case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
      return ISL_FORMAT_R32_FLOAT;
   default:
      return iris_isl_format_for_pipe_format(pf);
   }
}

static void
iris_blit(struct pipe_context *ctx, const struct pipe_blit_info *info)
{
   struct iris_context *ice = (void *) ctx;
   struct blorp_surf src_surf, dst_surf;
   iris_blorp_surf_for_resource(&src_surf, info->src.resource,
                                ISL_AUX_USAGE_NONE, false);
   iris_blorp_surf_for_resource(&dst_surf, info->dst.resource,
                                ISL_AUX_USAGE_NONE, true);

   enum isl_format src_isl_format = iris_get_blorp_format(info->src.format);
   enum isl_format dst_isl_format = iris_get_blorp_format(info->dst.format);

   // XXX: ???
   unsigned dst_layer = 0;
   unsigned src_layer = 0;

   struct isl_swizzle src_isl_swizzle = ISL_SWIZZLE_IDENTITY;

   int src_x0 = info->src.box.x;
   int src_x1 = info->src.box.x + info->src.box.width;
   int src_y0 = info->src.box.y;
   int src_y1 = info->src.box.y + info->src.box.height;
   int dst_x0 = info->dst.box.x;
   int dst_x1 = info->dst.box.x + info->dst.box.width;
   int dst_y0 = info->dst.box.y;
   int dst_y1 = info->dst.box.y + info->dst.box.height;
   bool mirror_x = false;
   bool mirror_y = false;
   GLenum filter =
      info->filter == PIPE_TEX_FILTER_LINEAR ? GL_LINEAR : GL_NEAREST;

   struct blorp_batch blorp_batch;
   blorp_batch_init(&ice->blorp, &blorp_batch, &ice->render_batch, 0);
   blorp_blit(&blorp_batch, &src_surf, info->src.level, src_layer,
              src_isl_format, src_isl_swizzle,
              &dst_surf, info->dst.level, dst_layer,
              dst_isl_format, ISL_SWIZZLE_IDENTITY,
              src_x0, src_y0, src_x1, src_y1,
              dst_x0, dst_y0, dst_x1, dst_y1,
              filter, mirror_x, mirror_y);

   blorp_batch_finish(&blorp_batch);
}

void
iris_init_blit_functions(struct pipe_context *ctx)
{
   ctx->blit = iris_blit;
}

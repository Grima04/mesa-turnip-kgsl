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
#include "util/u_format.h"
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

/**
 * The pipe->blit() driver hook.
 *
 * This performs a blit between two surfaces, which copies data but may
 * also perform format conversion, scaling, flipping, and so on.
 */
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
   enum blorp_filter filter;

   if (abs(info->dst.box.width) == abs(info->src.box.width) &&
       abs(info->dst.box.height) == abs(info->src.box.height)) {
      if (src_surf.surf->samples > 1 && dst_surf.surf->samples <= 1) {
         /* The OpenGL ES 3.2 specification, section 16.2.1, says:
          *
          *    "If the read framebuffer is multisampled (its effective
          *     value of SAMPLE_BUFFERS is one) and the draw framebuffer
          *     is not (its value of SAMPLE_BUFFERS is zero), the samples
          *     corresponding to each pixel location in the source are
          *     converted to a single sample before being written to the
          *     destination.  The filter parameter is ignored.  If the
          *     source formats are integer types or stencil values, a
          *     single sample’s value is selected for each pixel.  If the
          *     source formats are floating-point or normalized types,
          *     the sample values for each pixel are resolved in an
          *     implementation-dependent manner.  If the source formats
          *     are depth values, sample values are resolved in an
          *     implementation-dependent manner where the result will be
          *     between the minimum and maximum depth values in the pixel."
          *
          * When selecting a single sample, we always choose sample 0.
          */
         if (util_format_is_depth_or_stencil(info->src.format) ||
             util_format_is_pure_integer(info->src.format)) {
            filter = BLORP_FILTER_SAMPLE_0;
         } else {
            filter = BLORP_FILTER_AVERAGE;
         }
      } else {
         /* The OpenGL 4.6 specification, section 18.3.1, says:
          *
          *    "If the source and destination dimensions are identical,
          *     no filtering is applied."
          *
          * Using BLORP_FILTER_NONE will also handle the upsample case by
          * replicating the one value in the source to all values in the
          * destination.
          */
         filter = BLORP_FILTER_NONE;
      }
   } else if (info->filter == PIPE_TEX_FILTER_LINEAR) {
      filter = BLORP_FILTER_BILINEAR;
   } else {
      filter = BLORP_FILTER_NEAREST;
   }

   struct iris_batch *batch = &ice->render_batch;

   iris_batch_maybe_flush(batch, 1500);

   struct blorp_batch blorp_batch;
   blorp_batch_init(&ice->blorp, &blorp_batch, batch, 0);

   for (int slice = 0; slice < info->dst.box.depth; slice++) {
      blorp_blit(&blorp_batch,
                 &src_surf, info->src.level, info->src.box.z + slice,
                 src_isl_format, src_isl_swizzle,
                 &dst_surf, info->dst.level, info->dst.box.z + slice,
                 dst_isl_format, ISL_SWIZZLE_IDENTITY,
                 src_x0, src_y0, src_x1, src_y1,
                 dst_x0, dst_y0, dst_x1, dst_y1,
                 filter, mirror_x, mirror_y);
   }

   if (util_format_is_depth_and_stencil(info->dst.format) &&
       util_format_has_stencil(util_format_description(info->src.format))) {
      struct iris_resource *src_res, *dst_res, *junk;
      iris_get_depth_stencil_resources(info->src.resource, &junk, &src_res);
      iris_get_depth_stencil_resources(info->dst.resource, &junk, &dst_res);
      iris_blorp_surf_for_resource(&src_surf, &src_res->base,
                                   ISL_AUX_USAGE_NONE, false);
      iris_blorp_surf_for_resource(&dst_surf, &dst_res->base,
                                   ISL_AUX_USAGE_NONE, true);

      for (int slice = 0; slice < info->dst.box.depth; slice++) {
         blorp_blit(&blorp_batch,
                    &src_surf, info->src.level, info->src.box.z + slice,
                    ISL_FORMAT_R8_UINT, src_isl_swizzle,
                    &dst_surf, info->dst.level, info->dst.box.z + slice,
                    ISL_FORMAT_R8_UINT, ISL_SWIZZLE_IDENTITY,
                    src_x0, src_y0, src_x1, src_y1,
                    dst_x0, dst_y0, dst_x1, dst_y1,
                    filter, mirror_x, mirror_y);
      }
   }

   blorp_batch_finish(&blorp_batch);
}

/**
 * The pipe->resource_copy_region() driver hook.
 *
 * This implements ARB_copy_image semantics - a raw memory copy between
 * compatible view classes.
 */
static void
iris_resource_copy_region(struct pipe_context *ctx,
                          struct pipe_resource *dst,
                          unsigned dst_level,
                          unsigned dstx, unsigned dsty, unsigned dstz,
                          struct pipe_resource *src,
                          unsigned src_level,
                          const struct pipe_box *src_box)
{
   struct iris_context *ice = (void *) ctx;
   struct blorp_surf src_surf, dst_surf;
   iris_blorp_surf_for_resource(&src_surf, src, ISL_AUX_USAGE_NONE, false);
   iris_blorp_surf_for_resource(&dst_surf, dst, ISL_AUX_USAGE_NONE, true);

   // XXX: ???
   unsigned dst_layer = dstz;
   unsigned src_layer = src_box->z;

   struct iris_batch *batch = &ice->render_batch;

   iris_batch_maybe_flush(batch, 1500);

   struct blorp_batch blorp_batch;
   blorp_batch_init(&ice->blorp, &blorp_batch, batch, 0);
   blorp_copy(&blorp_batch, &src_surf, src_level, src_layer,
              &dst_surf, dst_level, dst_layer,
              src_box->x, src_box->y, dstx, dsty,
              src_box->width, src_box->height);
   blorp_batch_finish(&blorp_batch);
}

void
iris_init_blit_functions(struct pipe_context *ctx)
{
   ctx->blit = iris_blit;
   ctx->resource_copy_region = iris_resource_copy_region;
}

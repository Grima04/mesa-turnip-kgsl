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
 * The pipe->clear() driver hook.
 *
 * This clears buffers attached to the current draw framebuffer.
 */
static void
iris_clear(struct pipe_context *ctx,
           unsigned buffers,
           const union pipe_color_union *p_color,
           double depth,
           unsigned stencil)
{
   struct iris_context *ice = (void *) ctx;
   struct pipe_framebuffer_state *cso_fb = &ice->state.framebuffer;
   assert(buffers != 0);

   struct iris_batch *batch = &ice->render_batch;

   iris_batch_maybe_flush(batch, 1500);

   struct blorp_batch blorp_batch;
   blorp_batch_init(&ice->blorp, &blorp_batch, batch, 0);

   if (buffers & PIPE_CLEAR_DEPTHSTENCIL) {
      struct pipe_surface *psurf = cso_fb->zsbuf;
      struct iris_resource *z_res;
      struct iris_resource *stencil_res;
      struct blorp_surf z_surf;
      struct blorp_surf stencil_surf;
      const unsigned num_layers =
         psurf->u.tex.last_layer - psurf->u.tex.first_layer + 1;

      iris_get_depth_stencil_resources(psurf->texture, &z_res, &stencil_res);

      if (z_res) {
         iris_blorp_surf_for_resource(&z_surf, &z_res->base,
                                      ISL_AUX_USAGE_NONE, true);
      }

      if (stencil_res) {
         iris_blorp_surf_for_resource(&stencil_surf, &stencil_res->base,
                                      ISL_AUX_USAGE_NONE, true);
      }

      blorp_clear_depth_stencil(&blorp_batch, &z_surf, &stencil_surf,
                                psurf->u.tex.level, psurf->u.tex.first_layer,
                                num_layers, 0, 0, psurf->width, psurf->height,
                                (buffers & PIPE_CLEAR_DEPTH) != 0, depth,
                                (buffers & PIPE_CLEAR_STENCIL) ? 0xff : 0,
                                stencil);
   }

   if (buffers & PIPE_CLEAR_COLOR) {
      /* pipe_color_union and isl_color_value are interchangeable */
      union isl_color_value *clear_color = (void *) p_color;
      bool color_write_disable[4] = { false, false, false, false };

      for (unsigned i = 0; i < cso_fb->nr_cbufs; i++) {
         if (buffers & (PIPE_CLEAR_COLOR0 << i)) {
            struct pipe_surface *psurf = cso_fb->cbufs[i];
            struct iris_surface *isurf = (void *) psurf;
            struct blorp_surf surf;

            iris_blorp_surf_for_resource(&surf, psurf->texture,
                                         ISL_AUX_USAGE_NONE, true);

            blorp_clear(&blorp_batch, &surf, isurf->view.format,
                        ISL_SWIZZLE_IDENTITY,
                        psurf->u.tex.level, psurf->u.tex.first_layer,
                        psurf->u.tex.last_layer - psurf->u.tex.first_layer + 1,
                        0, 0, psurf->width, psurf->height,
                        *clear_color, color_write_disable);
         }
      }
   }

   blorp_batch_finish(&blorp_batch);
}

static void
iris_clear_texture(struct pipe_context *ctx,
                   struct pipe_resource *p_res,
                   unsigned level,
                   const struct pipe_box *box,
                   const void *data)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_resource *res = (void *) p_res;

   struct iris_batch *batch = &ice->render_batch;
   const struct gen_device_info *devinfo = &batch->screen->devinfo;

   iris_batch_maybe_flush(batch, 1500);

   struct blorp_batch blorp_batch;
   blorp_batch_init(&ice->blorp, &blorp_batch, batch, 0);

   if (util_format_is_depth_or_stencil(p_res->format)) {
      const struct util_format_description *fmt_desc =
         util_format_description(p_res->format);

      struct iris_resource *z_res;
      struct iris_resource *stencil_res;
      struct blorp_surf z_surf;
      struct blorp_surf stencil_surf;

      float depth = 0.0;
      uint8_t stencil = 0;

      iris_get_depth_stencil_resources(p_res, &z_res, &stencil_res);

      if (z_res) {
         iris_blorp_surf_for_resource(&z_surf, &z_res->base,
                                      ISL_AUX_USAGE_NONE, true);
         fmt_desc->unpack_z_float(&depth, 0, data, 0, 1, 1);
      }

      if (stencil_res) {
         iris_blorp_surf_for_resource(&stencil_surf, &stencil_res->base,
                                      ISL_AUX_USAGE_NONE, true);
         fmt_desc->unpack_s_8uint(&stencil, 0, data, 0, 1, 1);
      }

      blorp_clear_depth_stencil(&blorp_batch, &z_surf, &stencil_surf,
                                level, box->z, box->depth,
                                box->x, box->y,
                                box->x + box->width,
                                box->y + box->height,
                                z_res != NULL, depth,
                                stencil_res ? 0xff : 0, stencil);
   } else {
      union isl_color_value color;
      bool color_write_disable[4] = { false, false, false, false };
      struct blorp_surf surf;
      iris_blorp_surf_for_resource(&surf, p_res, ISL_AUX_USAGE_NONE, true);

      enum isl_format format = res->surf.format;

      if (!isl_format_supports_rendering(devinfo, format) &&
          isl_format_is_rgbx(format))
         format = isl_format_rgbx_to_rgba(format);

      if (!isl_format_supports_rendering(devinfo, format)) {
         const struct isl_format_layout *fmtl = isl_format_get_layout(format);
         // XXX: actually just get_copy_format_for_bpb from BLORP
         // XXX: don't cut and paste this
         switch (fmtl->bpb) {
         case 8:   format = ISL_FORMAT_R8_UINT;           break;
         case 16:  format = ISL_FORMAT_R8G8_UINT;         break;
         case 24:  format = ISL_FORMAT_R8G8B8_UINT;       break;
         case 32:  format = ISL_FORMAT_R8G8B8A8_UINT;     break;
         case 48:  format = ISL_FORMAT_R16G16B16_UINT;    break;
         case 64:  format = ISL_FORMAT_R16G16B16A16_UINT; break;
         case 96:  format = ISL_FORMAT_R32G32B32_UINT;    break;
         case 128: format = ISL_FORMAT_R32G32B32A32_UINT; break;
         default:
            unreachable("Unknown format bpb");
         }
      }

      isl_color_value_unpack(&color, format, data);

      blorp_clear(&blorp_batch, &surf, format, ISL_SWIZZLE_IDENTITY,
                  level, box->z, box->depth, box->x, box->y,
                  box->x + box->width, box->y + box->height,
                  color, color_write_disable);
   }

   blorp_batch_finish(&blorp_batch);
}


static void
iris_clear_render_target(struct pipe_context *ctx,
                         struct pipe_surface *dst,
                         const union pipe_color_union *color,
                         unsigned dst_x, unsigned dst_y,
                         unsigned width, unsigned height,
                         bool render_condition_enabled)
{
   fprintf(stderr, "XXX: iris_clear_render_target\n");
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
   fprintf(stderr, "XXX: iris_clear_depth_stencil\n");
}

void
iris_init_clear_functions(struct pipe_context *ctx)
{
   ctx->clear = iris_clear;
   ctx->clear_texture = iris_clear_texture;
   ctx->clear_render_target = iris_clear_render_target;
   ctx->clear_depth_stencil = iris_clear_depth_stencil;
}

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

static void
clear_color(struct iris_context *ice,
            struct pipe_resource *p_res,
            unsigned level,
            const struct pipe_box *box,
            bool render_condition_enabled,
            enum isl_format format,
            struct isl_swizzle swizzle,
            union isl_color_value color)
{
   struct iris_resource *res = (void *) p_res;

   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];
   const struct gen_device_info *devinfo = &batch->screen->devinfo;
   enum blorp_batch_flags blorp_flags = 0;

   if (render_condition_enabled) {
      if (ice->state.predicate == IRIS_PREDICATE_STATE_DONT_RENDER)
         return;

      if (ice->state.predicate == IRIS_PREDICATE_STATE_USE_BIT)
         blorp_flags |= BLORP_BATCH_PREDICATE_ENABLE;
   }

   iris_batch_maybe_flush(batch, 1500);

   struct blorp_batch blorp_batch;
   blorp_batch_init(&ice->blorp, &blorp_batch, batch, blorp_flags);

   bool color_write_disable[4] = { false, false, false, false };
   enum isl_aux_usage aux_usage =
      iris_resource_render_aux_usage(ice, res, format,
                                     false, false);

   iris_resource_prepare_render(ice, batch, res, level,
                                box->z, box->depth, aux_usage);

   struct blorp_surf surf;
   iris_blorp_surf_for_resource(&ice->vtbl, &surf, p_res, aux_usage, level,
                                true);

   if (!isl_format_supports_rendering(devinfo, format) &&
       isl_format_is_rgbx(format))
      format = isl_format_rgbx_to_rgba(format);

   blorp_clear(&blorp_batch, &surf, format, swizzle,
               level, box->z, box->depth, box->x, box->y,
               box->x + box->width, box->y + box->height,
               color, color_write_disable);

   blorp_batch_finish(&blorp_batch);
   iris_flush_and_dirty_for_history(ice, batch, res);

   iris_resource_finish_render(ice, res, level,
                               box->z, box->depth, aux_usage);
}

static bool
can_fast_clear_depth(struct iris_context *ice,
                     struct iris_resource *res,
                     unsigned level,
                     const struct pipe_box *box,
                     float depth)
{
   struct pipe_resource *p_res = (void *) res;

   /* Check for partial clears */
   if (box->x > 0 || box->y > 0 ||
       box->width < u_minify(p_res->width0, level) ||
       box->height < u_minify(p_res->height0, level)) {
      return false;
   }

   if (!(res->aux.has_hiz & (1 << level)))
      return false;

   return true;
}

static void
fast_clear_depth(struct iris_context *ice,
                 struct iris_resource *res,
                 unsigned level,
                 const struct pipe_box *box,
                 float depth)
{
   struct pipe_resource *p_res = (void *) res;
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];

   /* Quantize the clear value to what can be stored in the actual depth
    * buffer.  This makes the following check more accurate because it now
    * checks if the actual depth bits will match.  It also prevents us from
    * getting a too-accurate depth value during depth testing or when sampling
    * with HiZ enabled.
    */
   const unsigned nbits = p_res->format == PIPE_FORMAT_Z16_UNORM ? 16 : 24;
   const uint32_t depth_max = (1 << nbits) - 1;
   depth = p_res->format == PIPE_FORMAT_Z32_FLOAT ? depth :
      (unsigned)(depth * depth_max) / (float)depth_max;

   /* If we're clearing to a new clear value, then we need to resolve any clear
    * flags out of the HiZ buffer into the real depth buffer.
    */
   if (res->aux.clear_color.f32[0] != depth) {
      for (unsigned res_level = 0; res_level < res->surf.levels; res_level++) {
         if (!(res->aux.has_hiz & (1 << res_level)))
            continue;

         const unsigned level_layers =
            iris_get_num_logical_layers(res, res_level);
         for (unsigned layer = 0; layer < level_layers; layer++) {
            if (res_level == level &&
                layer >= box->z &&
                layer < box->z + box->depth) {
               /* We're going to clear this layer anyway.  Leave it alone. */
               continue;
            }

            enum isl_aux_state aux_state =
               iris_resource_get_aux_state(res, res_level, layer);

            if (aux_state != ISL_AUX_STATE_CLEAR &&
                aux_state != ISL_AUX_STATE_COMPRESSED_CLEAR) {
               /* This slice doesn't have any fast-cleared bits. */
               continue;
            }

            /* If we got here, then the level may have fast-clear bits that
             * use the old clear value.  We need to do a depth resolve to get
             * rid of their use of the clear value before we can change it.
             * Fortunately, few applications ever change their depth clear
             * value so this shouldn't happen often.
             */
            iris_hiz_exec(ice, batch, res, res_level, layer, 1,
                          ISL_AUX_OP_FULL_RESOLVE);
            iris_resource_set_aux_state(ice, res, res_level, layer, 1,
                                        ISL_AUX_STATE_RESOLVED);
         }
      }
      const union isl_color_value clear_color = { .f32 = {depth, } };
      iris_resource_set_clear_color(ice, res, clear_color);
   }

   for (unsigned l = 0; l < box->depth; l++) {
      enum isl_aux_state aux_state =
         iris_resource_get_aux_state(res, level, box->z + l);
      if (aux_state != ISL_AUX_STATE_CLEAR) {
         iris_hiz_exec(ice, batch, res, level,
                       box->z + l, 1, ISL_AUX_OP_FAST_CLEAR);
      }
   }

   iris_resource_set_aux_state(ice, res, level, box->z, box->depth,
                               ISL_AUX_STATE_CLEAR);
   ice->state.dirty |= IRIS_DIRTY_DEPTH_BUFFER;
}

static void
clear_depth_stencil(struct iris_context *ice,
                    struct pipe_resource *p_res,
                    unsigned level,
                    const struct pipe_box *box,
                    bool render_condition_enabled,
                    bool clear_depth,
                    bool clear_stencil,
                    float depth,
                    uint8_t stencil)
{
   struct iris_resource *res = (void *) p_res;

   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];
   enum blorp_batch_flags blorp_flags = 0;

   if (render_condition_enabled) {
      if (ice->state.predicate == IRIS_PREDICATE_STATE_DONT_RENDER)
         return;

      if (ice->state.predicate == IRIS_PREDICATE_STATE_USE_BIT)
         blorp_flags |= BLORP_BATCH_PREDICATE_ENABLE;
   }

   iris_batch_maybe_flush(batch, 1500);

   struct iris_resource *z_res;
   struct iris_resource *stencil_res;
   struct blorp_surf z_surf;
   struct blorp_surf stencil_surf;

   iris_get_depth_stencil_resources(p_res, &z_res, &stencil_res);
   if (z_res && clear_depth &&
       can_fast_clear_depth(ice, z_res, level, box, depth)) {
      fast_clear_depth(ice, z_res, level, box, depth);
      iris_flush_and_dirty_for_history(ice, batch, res);
      clear_depth = false;
      z_res = false;
   }

   /* At this point, we might have fast cleared the depth buffer. So if there's
    * no stencil clear pending, return early.
    */
   if (!(clear_depth || clear_stencil)) {
      return;
   }

   if (z_res) {
      iris_resource_prepare_depth(ice, batch, z_res, level, box->z, box->depth);
      iris_blorp_surf_for_resource(&ice->vtbl, &z_surf, &z_res->base,
                                   z_res->aux.usage, level, true);
   }

   struct blorp_batch blorp_batch;
   blorp_batch_init(&ice->blorp, &blorp_batch, batch, blorp_flags);

   if (stencil_res) {
      iris_blorp_surf_for_resource(&ice->vtbl, &stencil_surf,
                                   &stencil_res->base, stencil_res->aux.usage,
                                   level, true);
   }

   blorp_clear_depth_stencil(&blorp_batch, &z_surf, &stencil_surf,
                             level, box->z, box->depth,
                             box->x, box->y,
                             box->x + box->width,
                             box->y + box->height,
                             clear_depth && z_res, depth,
                             clear_stencil && stencil_res ? 0xff : 0, stencil);

   blorp_batch_finish(&blorp_batch);
   iris_flush_and_dirty_for_history(ice, batch, res);

   if (z_res) {
      iris_resource_finish_depth(ice, z_res, level,
                                 box->z, box->depth, true);
   }
}

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

   if (buffers & PIPE_CLEAR_DEPTHSTENCIL) {
      struct pipe_surface *psurf = cso_fb->zsbuf;
      struct pipe_box box = {
         .width = cso_fb->width,
         .height = cso_fb->height,
         .depth = psurf->u.tex.last_layer - psurf->u.tex.first_layer + 1,
         .z = psurf->u.tex.first_layer,
      };

      clear_depth_stencil(ice, psurf->texture, psurf->u.tex.level, &box, true,
                          buffers & PIPE_CLEAR_DEPTH,
                          buffers & PIPE_CLEAR_STENCIL,
                          depth, stencil);
   }

   if (buffers & PIPE_CLEAR_COLOR) {
      /* pipe_color_union and isl_color_value are interchangeable */
      union isl_color_value *color = (void *) p_color;

      for (unsigned i = 0; i < cso_fb->nr_cbufs; i++) {
         if (buffers & (PIPE_CLEAR_COLOR0 << i)) {
            struct pipe_surface *psurf = cso_fb->cbufs[i];
            struct iris_surface *isurf = (void *) psurf;
            struct pipe_box box = {
               .width = cso_fb->width,
               .height = cso_fb->height,
               .depth = psurf->u.tex.last_layer - psurf->u.tex.first_layer + 1,
               .z = psurf->u.tex.first_layer,
            };

            clear_color(ice, psurf->texture, psurf->u.tex.level, &box,
                        true, isurf->view.format, isurf->view.swizzle,
                        *color);
         }
      }
   }
}

/**
 * The pipe->clear_texture() driver hook.
 *
 * This clears the given texture resource.
 */
static void
iris_clear_texture(struct pipe_context *ctx,
                   struct pipe_resource *p_res,
                   unsigned level,
                   const struct pipe_box *box,
                   const void *data)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_screen *screen = (void *) ctx->screen;
   const struct gen_device_info *devinfo = &screen->devinfo;

   if (util_format_is_depth_or_stencil(p_res->format)) {
      const struct util_format_description *fmt_desc =
         util_format_description(p_res->format);

      float depth = 0.0;
      uint8_t stencil = 0;

      if (fmt_desc->unpack_z_float)
         fmt_desc->unpack_z_float(&depth, 0, data, 0, 1, 1);

      if (fmt_desc->unpack_s_8uint)
         fmt_desc->unpack_s_8uint(&stencil, 0, data, 0, 1, 1);

      clear_depth_stencil(ice, p_res, level, box, true, true, true,
                          depth, stencil);
   } else {
      union isl_color_value color;
      struct iris_resource *res = (void *) p_res;
      enum isl_format format = res->surf.format;

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

         /* No aux surfaces for non-renderable surfaces */
         assert(res->aux.usage == ISL_AUX_USAGE_NONE);
      }

      isl_color_value_unpack(&color, format, data);

      clear_color(ice, p_res, level, box, true, format,
                  ISL_SWIZZLE_IDENTITY, color);
   }
}

/**
 * The pipe->clear_render_target() driver hook.
 *
 * This clears the given render target surface.
 */
static void
iris_clear_render_target(struct pipe_context *ctx,
                         struct pipe_surface *psurf,
                         const union pipe_color_union *p_color,
                         unsigned dst_x, unsigned dst_y,
                         unsigned width, unsigned height,
                         bool render_condition_enabled)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_surface *isurf = (void *) psurf;
   struct pipe_box box = {
      .x = dst_x,
      .y = dst_y,
      .z = psurf->u.tex.first_layer,
      .width = width,
      .height = height,
      .depth = psurf->u.tex.last_layer - psurf->u.tex.first_layer + 1
   };

   /* pipe_color_union and isl_color_value are interchangeable */
   union isl_color_value *color = (void *) p_color;

   clear_color(ice, psurf->texture, psurf->u.tex.level, &box,
               render_condition_enabled,
               isurf->view.format, isurf->view.swizzle, *color);
}

/**
 * The pipe->clear_depth_stencil() driver hook.
 *
 * This clears the given depth/stencil surface.
 */
static void
iris_clear_depth_stencil(struct pipe_context *ctx,
                         struct pipe_surface *psurf,
                         unsigned flags,
                         double depth,
                         unsigned stencil,
                         unsigned dst_x, unsigned dst_y,
                         unsigned width, unsigned height,
                         bool render_condition_enabled)
{
   struct iris_context *ice = (void *) ctx;
   struct pipe_box box = {
      .x = dst_x,
      .y = dst_y,
      .z = psurf->u.tex.first_layer,
      .width = width,
      .height = height,
      .depth = psurf->u.tex.last_layer - psurf->u.tex.first_layer + 1
   };

   assert(util_format_is_depth_or_stencil(psurf->texture->format));

   clear_depth_stencil(ice, psurf->texture, psurf->u.tex.level, &box,
                       render_condition_enabled,
                       flags & PIPE_CLEAR_DEPTH, flags & PIPE_CLEAR_STENCIL,
                       depth, stencil);
}

void
iris_init_clear_functions(struct pipe_context *ctx)
{
   ctx->clear = iris_clear;
   ctx->clear_texture = iris_clear_texture;
   ctx->clear_render_target = iris_clear_render_target;
   ctx->clear_depth_stencil = iris_clear_depth_stencil;
}

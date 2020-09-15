/*
 * Copyright 2018 Collabora Ltd.
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

#include "zink_context.h"
#include "zink_resource.h"
#include "zink_screen.h"

#include "util/u_blitter.h"
#include "util/format/u_format.h"
#include "util/format_srgb.h"
#include "util/u_framebuffer.h"
#include "util/u_inlines.h"
#include "util/u_rect.h"
#include "util/u_surface.h"

static inline bool
check_3d_layers(struct pipe_surface *psurf)
{
   /* SPEC PROBLEM:
    * though the vk spec doesn't seem to explicitly address this, currently drivers
    * are claiming that all 3D images have a single "3D" layer regardless of layercount,
    * so we can never clear them if we aren't trying to clear only layer 0
    */
   if (psurf->u.tex.first_layer)
      return false;
      
   if (psurf->u.tex.last_layer - psurf->u.tex.first_layer > 0)
      return false;
   return true;
}

static void
clear_in_rp(struct pipe_context *pctx,
           unsigned buffers,
           const struct pipe_scissor_state *scissor_state,
           const union pipe_color_union *pcolor,
           double depth, unsigned stencil)
{
   struct zink_context *ctx = zink_context(pctx);
   struct pipe_framebuffer_state *fb = &ctx->fb_state;
   struct zink_resource *resources[PIPE_MAX_COLOR_BUFS + 1] = {};
   int res_count = 0;

   VkClearAttachment attachments[1 + PIPE_MAX_COLOR_BUFS];
   int num_attachments = 0;

   if (buffers & PIPE_CLEAR_COLOR) {
      VkClearColorValue color;
      color.float32[0] = pcolor->f[0];
      color.float32[1] = pcolor->f[1];
      color.float32[2] = pcolor->f[2];
      color.float32[3] = pcolor->f[3];

      for (unsigned i = 0; i < fb->nr_cbufs; i++) {
         if (!(buffers & (PIPE_CLEAR_COLOR0 << i)) || !fb->cbufs[i])
            continue;

         attachments[num_attachments].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         attachments[num_attachments].colorAttachment = i;
         attachments[num_attachments].clearValue.color = color;
         ++num_attachments;
         struct zink_resource *res = (struct zink_resource*)fb->cbufs[i]->texture;
         zink_resource_image_barrier(ctx, NULL, res, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0);
         resources[res_count++] = res;
      }
   }

   if (buffers & PIPE_CLEAR_DEPTHSTENCIL && fb->zsbuf) {
      VkImageAspectFlags aspect = 0;
      if (buffers & PIPE_CLEAR_DEPTH)
         aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (buffers & PIPE_CLEAR_STENCIL)
         aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

      attachments[num_attachments].aspectMask = aspect;
      attachments[num_attachments].clearValue.depthStencil.depth = depth;
      attachments[num_attachments].clearValue.depthStencil.stencil = stencil;
      ++num_attachments;
      struct zink_resource *res = (struct zink_resource*)fb->zsbuf->texture;
      zink_resource_image_barrier(ctx, NULL, res, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 0);
      resources[res_count++] = res;
   }

   VkClearRect cr = {};
   if (scissor_state) {
      cr.rect.offset.x = scissor_state->minx;
      cr.rect.offset.y = scissor_state->miny;
      cr.rect.extent.width = MIN2(fb->width, scissor_state->maxx - scissor_state->minx);
      cr.rect.extent.height = MIN2(fb->height, scissor_state->maxy - scissor_state->miny);
   } else {
      cr.rect.extent.width = fb->width;
      cr.rect.extent.height = fb->height;
   }
   cr.baseArrayLayer = 0;
   cr.layerCount = util_framebuffer_get_num_layers(fb);
   struct zink_batch *batch = zink_batch_rp(ctx);
   for (int i = 0; i < res_count; i++)
      zink_batch_reference_resource_rw(batch, resources[i], true);
   vkCmdClearAttachments(batch->cmdbuf, num_attachments, attachments, 1, &cr);
}

static void
clear_color_no_rp(struct zink_context *ctx, struct zink_resource *res, const union pipe_color_union *pcolor, unsigned level, unsigned layer, unsigned layerCount)
{
   struct zink_batch *batch = zink_batch_no_rp(ctx);
   VkImageSubresourceRange range = {};
   range.baseMipLevel = level;
   range.levelCount = 1;
   range.baseArrayLayer = layer;
   range.layerCount = layerCount;
   range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

   VkClearColorValue color;
   color.float32[0] = pcolor->f[0];
   color.float32[1] = pcolor->f[1];
   color.float32[2] = pcolor->f[2];
   color.float32[3] = pcolor->f[3];

   if (zink_resource_image_needs_barrier(res, VK_IMAGE_LAYOUT_GENERAL, 0, 0) &&
       zink_resource_image_needs_barrier(res, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0))
      zink_resource_image_barrier(ctx, NULL, res, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0);
   zink_batch_reference_resource_rw(batch, res, true);
   vkCmdClearColorImage(batch->cmdbuf, res->image, res->layout, &color, 1, &range);
}

static void
clear_zs_no_rp(struct zink_context *ctx, struct zink_resource *res, VkImageAspectFlags aspects, double depth, unsigned stencil, unsigned level, unsigned layer, unsigned layerCount)
{
   struct zink_batch *batch = zink_batch_no_rp(ctx);
   VkImageSubresourceRange range = {};
   range.baseMipLevel = level;
   range.levelCount = 1;
   range.baseArrayLayer = layer;
   range.layerCount = layerCount;
   range.aspectMask = aspects;

   VkClearDepthStencilValue zs_value = {depth, stencil};

   if (zink_resource_image_needs_barrier(res, VK_IMAGE_LAYOUT_GENERAL, 0, 0) &&
       zink_resource_image_needs_barrier(res, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0))
      zink_resource_image_barrier(ctx, NULL, res, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0);
   zink_batch_reference_resource_rw(batch, res, true);
   vkCmdClearDepthStencilImage(batch->cmdbuf, res->image, res->layout, &zs_value, 1, &range);
}

static bool
clear_needs_rp(unsigned width, unsigned height, struct u_rect *region)
{
   struct u_rect intersect = {0, width, 0, height};

   /* FIXME: this is very inefficient; if no renderpass has been started yet,
    * we should record the clear if it's full-screen, and apply it as we
    * start the render-pass. Otherwise we can do a partial out-of-renderpass
    * clear.
    */
   if (!u_rect_test_intersection(region, &intersect))
      /* is this even a thing? */
      return true;

    u_rect_find_intersection(region, &intersect);
    if (intersect.x0 != 0 || intersect.y0 != 0 ||
        intersect.x1 != width || intersect.y1 != height)
       return true;

   return false;
}

void
zink_clear(struct pipe_context *pctx,
           unsigned buffers,
           const struct pipe_scissor_state *scissor_state,
           const union pipe_color_union *pcolor,
           double depth, unsigned stencil)
{
   struct zink_context *ctx = zink_context(pctx);
   struct pipe_framebuffer_state *fb = &ctx->fb_state;
   struct zink_batch *batch = zink_curr_batch(ctx);
   bool needs_rp = false;

   if (scissor_state) {
      struct u_rect scissor = {scissor_state->minx, scissor_state->maxx, scissor_state->miny, scissor_state->maxy};
      needs_rp = clear_needs_rp(fb->width, fb->height, &scissor);
   }


   if (needs_rp || batch->in_rp || ctx->render_condition_active) {
      clear_in_rp(pctx, buffers, scissor_state, pcolor, depth, stencil);
      return;
   }

   if (buffers & PIPE_CLEAR_COLOR) {
      for (unsigned i = 0; i < fb->nr_cbufs; i++) {
         if ((buffers & (PIPE_CLEAR_COLOR0 << i)) && fb->cbufs[i]) {
            struct pipe_surface *psurf = fb->cbufs[i];

            if (psurf->texture->target == PIPE_TEXTURE_3D && !check_3d_layers(psurf)) {
               clear_in_rp(pctx, buffers, scissor_state, pcolor, depth, stencil);
               return;
            }
            struct zink_resource *res = zink_resource(psurf->texture);
            union pipe_color_union color = *pcolor;
            if (psurf->format != res->base.format &&
                !util_format_is_srgb(psurf->format) && util_format_is_srgb(res->base.format)) {
               /* if SRGB mode is disabled for the fb with a backing srgb image then we have to
                * convert this to srgb color
                */
               color.f[0] = util_format_srgb_to_linear_float(pcolor->f[0]);
               color.f[1] = util_format_srgb_to_linear_float(pcolor->f[1]);
               color.f[2] = util_format_srgb_to_linear_float(pcolor->f[2]);
            }
            clear_color_no_rp(ctx, zink_resource(fb->cbufs[i]->texture), &color,
                              psurf->u.tex.level, psurf->u.tex.first_layer,
                              psurf->u.tex.last_layer - psurf->u.tex.first_layer + 1);
         }
      }
   }

   if (buffers & PIPE_CLEAR_DEPTHSTENCIL && fb->zsbuf) {
      if (fb->zsbuf->texture->target == PIPE_TEXTURE_3D && !check_3d_layers(fb->zsbuf)) {
         clear_in_rp(pctx, buffers, scissor_state, pcolor, depth, stencil);
         return;
      }
      VkImageAspectFlags aspects = 0;
      if (buffers & PIPE_CLEAR_DEPTH)
         aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (buffers & PIPE_CLEAR_STENCIL)
         aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
      clear_zs_no_rp(ctx, zink_resource(fb->zsbuf->texture), aspects,
                     depth, stencil, fb->zsbuf->u.tex.level, fb->zsbuf->u.tex.first_layer,
                     fb->zsbuf->u.tex.last_layer - fb->zsbuf->u.tex.first_layer + 1);
   }
}

static struct pipe_surface *
create_clear_surface(struct pipe_context *pctx, struct pipe_resource *pres, unsigned level, const struct pipe_box *box)
{
   struct pipe_surface tmpl = {{0}};

   tmpl.format = pres->format;
   tmpl.u.tex.first_layer = box->z;
   tmpl.u.tex.last_layer = box->z + box->depth - 1;
   tmpl.u.tex.level = level;
   return pctx->create_surface(pctx, pres, &tmpl);
}

void
zink_clear_texture(struct pipe_context *pctx,
                   struct pipe_resource *pres,
                   unsigned level,
                   const struct pipe_box *box,
                   const void *data)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_resource *res = zink_resource(pres);
   struct pipe_screen *pscreen = pctx->screen;
   struct u_rect region = {box->x, box->x + box->width, box->y, box->y + box->height};
   bool needs_rp = clear_needs_rp(pres->width0, pres->height0, &region) || ctx->render_condition_active;
   struct zink_batch *batch = zink_curr_batch(ctx);
   struct pipe_surface *surf = NULL;

   if (res->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      union pipe_color_union color;

      util_format_unpack_rgba(pres->format, color.ui, data, 1);

      if (pscreen->is_format_supported(pscreen, pres->format, pres->target, 0, 0,
                                      PIPE_BIND_RENDER_TARGET) && !needs_rp && !batch->in_rp) {
         clear_color_no_rp(ctx, res, &color, level, box->z, box->depth);
      } else {
         surf = create_clear_surface(pctx, pres, level, box);
         zink_blit_begin(ctx, ZINK_BLIT_SAVE_FB | ZINK_BLIT_SAVE_FS);
         util_clear_render_target(pctx, surf, &color, box->x, box->y, box->width, box->height);
      }
      if (res->base.target == PIPE_BUFFER)
         util_range_add(&res->base, &res->valid_buffer_range, box->x, box->x + box->width);
   } else {
      float depth = 0.0;
      uint8_t stencil = 0;

      if (res->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
         util_format_unpack_z_float(pres->format, &depth, data, 1);

      if (res->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
         util_format_unpack_s_8uint(pres->format, &stencil, data, 1);

      if (!needs_rp && !batch->in_rp)
         clear_zs_no_rp(ctx, res, res->aspect, depth, stencil, level, box->z, box->depth);
      else {
         unsigned flags = 0;
         if (res->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
            flags |= PIPE_CLEAR_DEPTH;
         if (res->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
            flags |= PIPE_CLEAR_STENCIL;
         surf = create_clear_surface(pctx, pres, level, box);
         zink_blit_begin(ctx, ZINK_BLIT_SAVE_FB | ZINK_BLIT_SAVE_FS);
         util_blitter_clear_depth_stencil(ctx->blitter, surf, flags, depth, stencil, box->x, box->y, box->width, box->height);
      }
   }
   pipe_surface_reference(&surf, NULL);
}

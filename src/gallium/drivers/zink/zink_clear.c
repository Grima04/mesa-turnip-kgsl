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
#include "util/u_framebuffer.h"


void
zink_clear(struct pipe_context *pctx,
           unsigned buffers,
           const struct pipe_scissor_state *scissor_state,
           const union pipe_color_union *pcolor,
           double depth, unsigned stencil)
{
   struct zink_context *ctx = zink_context(pctx);
   struct pipe_framebuffer_state *fb = &ctx->fb_state;

   /* FIXME: this is very inefficient; if no renderpass has been started yet,
    * we should record the clear if it's full-screen, and apply it as we
    * start the render-pass. Otherwise we can do a partial out-of-renderpass
    * clear.
    */
   struct zink_batch *batch = zink_batch_rp(ctx);

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
   }

   VkClearRect cr;
   cr.rect.offset.x = 0;
   cr.rect.offset.y = 0;
   cr.rect.extent.width = fb->width;
   cr.rect.extent.height = fb->height;
   cr.baseArrayLayer = 0;
   cr.layerCount = util_framebuffer_get_num_layers(fb);
   vkCmdClearAttachments(batch->cmdbuf, num_attachments, attachments, 1, &cr);
}

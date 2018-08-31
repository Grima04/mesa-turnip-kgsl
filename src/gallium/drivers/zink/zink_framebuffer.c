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

#include "zink_framebuffer.h"

#include "zink_render_pass.h"
#include "zink_screen.h"
#include "zink_surface.h"

#include "util/u_memory.h"
#include "util/u_string.h"

void
zink_destroy_framebuffer(struct zink_screen *screen,
                         struct zink_framebuffer *fbuf)
{
   vkDestroyFramebuffer(screen->dev, fbuf->fb, NULL);
   for (int i = 0; i < ARRAY_SIZE(fbuf->surfaces); ++i)
      pipe_surface_reference(fbuf->surfaces + i, NULL);

   zink_render_pass_reference(screen, &fbuf->rp, NULL);

   FREE(fbuf);
}

struct zink_framebuffer *
zink_create_framebuffer(struct zink_screen *screen,
                        const struct pipe_framebuffer_state *fb,
                        struct zink_render_pass *rp)
{
   struct zink_framebuffer *fbuf = CALLOC_STRUCT(zink_framebuffer);
   if (!fbuf)
      return NULL;

   pipe_reference_init(&fbuf->reference, 1);

   VkImageView attachments[PIPE_MAX_COLOR_BUFS + 1];
   for (int i = 0; i < fb->nr_cbufs; i++) {
      struct pipe_surface *psurf = fb->cbufs[i];
      pipe_surface_reference(fbuf->surfaces + i, psurf);
      attachments[i] = zink_surface(psurf)->image_view;
   }

   int num_attachments = fb->nr_cbufs;
   if (fb->zsbuf) {
      struct pipe_surface *psurf = fb->zsbuf;
      pipe_surface_reference(fbuf->surfaces + num_attachments, psurf);
      attachments[num_attachments++] = zink_surface(psurf)->image_view;
   }

   assert(rp);
   zink_render_pass_reference(screen, &fbuf->rp, rp);

   VkFramebufferCreateInfo fci = {};
   fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
   fci.renderPass = rp->render_pass;
   fci.attachmentCount = num_attachments;
   fci.pAttachments = attachments;
   fci.width = (uint32_t)fb->width;
   fci.height = (uint32_t)fb->height;
   fci.layers = (uint32_t)MAX2(fb->layers, 1);

   if (vkCreateFramebuffer(screen->dev, &fci, NULL, &fbuf->fb) != VK_SUCCESS) {
      zink_destroy_framebuffer(screen, fbuf);
      return NULL;
   }

   return fbuf;
}

void
debug_describe_zink_framebuffer(char* buf, const struct zink_framebuffer *ptr)
{
   sprintf(buf, "zink_framebuffer");
}

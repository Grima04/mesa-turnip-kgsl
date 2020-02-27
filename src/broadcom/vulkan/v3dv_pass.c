/*
 * Copyright © 2019 Raspberry Pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "v3dv_private.h"

static uint32_t
num_subpass_attachments(const VkSubpassDescription *desc)
{
   return desc->inputAttachmentCount +
          desc->colorAttachmentCount +
          (desc->pResolveAttachments ? desc->colorAttachmentCount : 0) +
          (desc->pDepthStencilAttachment != NULL);
}

static void
pass_find_subpass_range_for_attachments(struct v3dv_render_pass *pass)
{
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      pass->attachments[i].first_subpass = pass->subpass_count - 1;
      pass->attachments[i].last_subpass = 0;
   }

   for (uint32_t i = 0; i < pass->subpass_count; i++) {
      const struct v3dv_subpass *subpass = &pass->subpasses[i];

      for (uint32_t j = 0; j < subpass->color_count; j++) {
         uint32_t attachment_idx = subpass->color_attachments[j].attachment;
         if (attachment_idx == VK_ATTACHMENT_UNUSED)
            continue;

         if (i < pass->attachments[attachment_idx].first_subpass)
            pass->attachments[attachment_idx].first_subpass = i;
         if (i > pass->attachments[attachment_idx].last_subpass)
            pass->attachments[attachment_idx].last_subpass = i;
      }

      uint32_t ds_attachment_idx = subpass->ds_attachment.attachment;
      if (ds_attachment_idx != VK_ATTACHMENT_UNUSED) {
         if (i < pass->attachments[ds_attachment_idx].first_subpass)
            pass->attachments[ds_attachment_idx].first_subpass = i;
         if (i > pass->attachments[ds_attachment_idx].last_subpass)
            pass->attachments[ds_attachment_idx].last_subpass = i;
      }

      /* FIXME: input/resolve attachments */
   }
}


VkResult
v3dv_CreateRenderPass(VkDevice _device,
                      const VkRenderPassCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkRenderPass *pRenderPass)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_render_pass *pass;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);

   size_t size = sizeof(*pass);
   size_t subpasses_offset = size;
   size += pCreateInfo->subpassCount * sizeof(pass->subpasses[0]);
   size_t attachments_offset = size;
   size += pCreateInfo->attachmentCount * sizeof(pass->attachments[0]);

   pass = vk_alloc2(&device->alloc, pAllocator, size, 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pass == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(pass, 0, size);
   pass->attachment_count = pCreateInfo->attachmentCount;
   pass->attachments = (void *) pass + attachments_offset;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->subpasses = (void *) pass + subpasses_offset;

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++)
      pass->attachments[i].desc = pCreateInfo->pAttachments[i];

   uint32_t subpass_attachment_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription *desc = &pCreateInfo->pSubpasses[i];
      subpass_attachment_count += num_subpass_attachments(desc);
   }

   if (subpass_attachment_count) {
      const size_t subpass_attachment_bytes =
         subpass_attachment_count * sizeof(struct v3dv_subpass_attachment);
      pass->subpass_attachments =
         vk_alloc2(&device->alloc, pAllocator, subpass_attachment_bytes, 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (pass->subpass_attachments == NULL) {
         vk_free2(&device->alloc, pAllocator, pass);
         return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   } else {
      pass->subpass_attachments = NULL;
   }

   struct v3dv_subpass_attachment *p = pass->subpass_attachments;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription *desc = &pCreateInfo->pSubpasses[i];
      struct v3dv_subpass *subpass = &pass->subpasses[i];

      subpass->input_count = desc->inputAttachmentCount;
      subpass->color_count = desc->colorAttachmentCount;

      if (desc->inputAttachmentCount > 0) {
         subpass->input_attachments = p;
         p += desc->inputAttachmentCount;

         for (uint32_t j = 0; j < desc->inputAttachmentCount; j++) {
            subpass->input_attachments[j] = (struct v3dv_subpass_attachment) {
               .attachment = desc->pInputAttachments[j].attachment,
               .layout = desc->pInputAttachments[j].layout,
            };
         }
      }

      if (desc->colorAttachmentCount > 0) {
         subpass->color_attachments = p;
         p += desc->colorAttachmentCount;

         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            subpass->color_attachments[j] = (struct v3dv_subpass_attachment) {
               .attachment = desc->pColorAttachments[j].attachment,
               .layout = desc->pColorAttachments[j].layout,
            };
         }
      }

      if (desc->pResolveAttachments) {
         subpass->resolve_attachments = p;
         p += desc->colorAttachmentCount;

         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            subpass->resolve_attachments[j] = (struct v3dv_subpass_attachment) {
               .attachment = desc->pResolveAttachments[j].attachment,
               .layout = desc->pResolveAttachments[j].layout,
            };
         }
      }

      if (desc->pDepthStencilAttachment) {
         subpass->ds_attachment = (struct v3dv_subpass_attachment) {
            .attachment = desc->pDepthStencilAttachment->attachment,
            .layout = desc->pDepthStencilAttachment->layout,
         };
      } else {
         subpass->ds_attachment.attachment = VK_ATTACHMENT_UNUSED;
      }
   }

   pass_find_subpass_range_for_attachments(pass);

   /* FIXME: handle subpass dependencies */

   *pRenderPass = v3dv_render_pass_to_handle(pass);

   return VK_SUCCESS;
}

void
v3dv_DestroyRenderPass(VkDevice _device,
                       VkRenderPass _pass,
                       const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_render_pass, pass, _pass);

   if (!_pass)
      return;

   vk_free2(&device->alloc, pAllocator, pass->subpass_attachments);
   vk_free2(&device->alloc, pAllocator, pass);
}

void
v3dv_GetRenderAreaGranularity(VkDevice device,
                              VkRenderPass renderPass,
                              VkExtent2D *pGranularity)
{
   V3DV_FROM_HANDLE(v3dv_render_pass, pass, renderPass);

   /* Our tile size depends on the max number of color attachments we can
    * have in any subpass and their bpp. Here we only know the number of
    * attachments, so we only use that. This means we might report a
    * granularity that is slightly larger, but that should be fine.
    */
   static const uint8_t tile_sizes[] = {
      64, 64,
      64, 32,
      32, 32,
      32, 16,
      16, 16,
   };

   uint32_t max_color_attachment_count = 0;
   for (unsigned i = 0; i < pass->subpass_count; i++) {
      max_color_attachment_count = MAX2(max_color_attachment_count,
                                        pass->subpasses[i].color_count);
   }

   uint32_t idx = 0;
   if (max_color_attachment_count > 2)
      idx += 2;
   else if (max_color_attachment_count > 1)
      idx += 1;

   *pGranularity = (VkExtent2D) { .width = tile_sizes[idx * 2],
                                  .height = tile_sizes[idx * 2 + 1] };
}

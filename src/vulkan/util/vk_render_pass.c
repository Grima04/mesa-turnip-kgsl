/*
 * Copyright © 2020 Valve Corporation
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

#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_util.h"

static void
translate_references(VkAttachmentReference2 **reference_ptr,
                     const VkAttachmentReference *reference,
                     uint32_t count)
{
   VkAttachmentReference2 *reference2 = *reference_ptr;
   *reference_ptr += count;
   for (uint32_t i = 0; i < count; i++) {
      reference2[i] = (VkAttachmentReference2) {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
         .pNext = NULL,
         .attachment = reference[i].attachment,
         .layout = reference[i].layout,
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
      };
   }
}

VkResult
vk_common_CreateRenderPass(VkDevice _device,
                           const VkRenderPassCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   /* note: these counts shouldn't be excessively high, so allocating it all
    * on the stack should be OK..
    * also note preserve attachments aren't translated, currently unused
    */
   VkAttachmentDescription2 attachments[pCreateInfo->attachmentCount];
   VkSubpassDescription2 subpasses[pCreateInfo->subpassCount];
   VkSubpassDependency2 dependencies[pCreateInfo->dependencyCount];
   uint32_t reference_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      reference_count += pCreateInfo->pSubpasses[i].inputAttachmentCount;
      reference_count += pCreateInfo->pSubpasses[i].colorAttachmentCount;
      if (pCreateInfo->pSubpasses[i].pResolveAttachments)
         reference_count += pCreateInfo->pSubpasses[i].colorAttachmentCount;
      if (pCreateInfo->pSubpasses[i].pDepthStencilAttachment)
         reference_count += 1;
   }
   VkAttachmentReference2 reference[reference_count];
   VkAttachmentReference2 *reference_ptr = reference;

   VkRenderPassMultiviewCreateInfo *multiview_info = NULL;
   vk_foreach_struct(ext, pCreateInfo->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
         multiview_info = (VkRenderPassMultiviewCreateInfo*) ext;
         break;
      }
   }

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      attachments[i] = (VkAttachmentDescription2) {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
         .pNext = NULL,
         .flags = pCreateInfo->pAttachments[i].flags,
         .format = pCreateInfo->pAttachments[i].format,
         .samples = pCreateInfo->pAttachments[i].samples,
         .loadOp = pCreateInfo->pAttachments[i].loadOp,
         .storeOp = pCreateInfo->pAttachments[i].storeOp,
         .stencilLoadOp = pCreateInfo->pAttachments[i].stencilLoadOp,
         .stencilStoreOp = pCreateInfo->pAttachments[i].stencilStoreOp,
         .initialLayout = pCreateInfo->pAttachments[i].initialLayout,
         .finalLayout = pCreateInfo->pAttachments[i].finalLayout,
      };
   }

   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      subpasses[i] = (VkSubpassDescription2) {
         .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
         .pNext = NULL,
         .flags = pCreateInfo->pSubpasses[i].flags,
         .pipelineBindPoint = pCreateInfo->pSubpasses[i].pipelineBindPoint,
         .viewMask = 0,
         .inputAttachmentCount = pCreateInfo->pSubpasses[i].inputAttachmentCount,
         .colorAttachmentCount = pCreateInfo->pSubpasses[i].colorAttachmentCount,
      };

      if (multiview_info && multiview_info->subpassCount)
         subpasses[i].viewMask = multiview_info->pViewMasks[i];

      subpasses[i].pInputAttachments = reference_ptr;
      translate_references(&reference_ptr,
                           pCreateInfo->pSubpasses[i].pInputAttachments,
                           subpasses[i].inputAttachmentCount);
      subpasses[i].pColorAttachments = reference_ptr;
      translate_references(&reference_ptr,
                           pCreateInfo->pSubpasses[i].pColorAttachments,
                           subpasses[i].colorAttachmentCount);
      subpasses[i].pResolveAttachments = NULL;
      if (pCreateInfo->pSubpasses[i].pResolveAttachments) {
         subpasses[i].pResolveAttachments = reference_ptr;
         translate_references(&reference_ptr,
                              pCreateInfo->pSubpasses[i].pResolveAttachments,
                              subpasses[i].colorAttachmentCount);
      }
      subpasses[i].pDepthStencilAttachment = NULL;
      if (pCreateInfo->pSubpasses[i].pDepthStencilAttachment) {
         subpasses[i].pDepthStencilAttachment = reference_ptr;
         translate_references(&reference_ptr,
                              pCreateInfo->pSubpasses[i].pDepthStencilAttachment,
                              1);
      }
   }

   assert(reference_ptr == reference + reference_count);

   for (uint32_t i = 0; i < pCreateInfo->dependencyCount; i++) {
      dependencies[i] = (VkSubpassDependency2) {
         .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
         .pNext = NULL,
         .srcSubpass = pCreateInfo->pDependencies[i].srcSubpass,
         .dstSubpass = pCreateInfo->pDependencies[i].dstSubpass,
         .srcStageMask = pCreateInfo->pDependencies[i].srcStageMask,
         .dstStageMask = pCreateInfo->pDependencies[i].dstStageMask,
         .srcAccessMask = pCreateInfo->pDependencies[i].srcAccessMask,
         .dstAccessMask = pCreateInfo->pDependencies[i].dstAccessMask,
         .dependencyFlags = pCreateInfo->pDependencies[i].dependencyFlags,
         .viewOffset = 0,
      };

      if (multiview_info && multiview_info->dependencyCount)
         dependencies[i].viewOffset = multiview_info->pViewOffsets[i];
   }

   VkRenderPassCreateInfo2 create_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
      .pNext = pCreateInfo->pNext,
      .flags = pCreateInfo->flags,
      .attachmentCount = pCreateInfo->attachmentCount,
      .pAttachments = attachments,
      .subpassCount = pCreateInfo->subpassCount,
      .pSubpasses = subpasses,
      .dependencyCount = pCreateInfo->dependencyCount,
      .pDependencies = dependencies,
   };

   if (multiview_info) {
      create_info.correlatedViewMaskCount = multiview_info->correlationMaskCount;
      create_info.pCorrelatedViewMasks = multiview_info->pCorrelationMasks;
   }

   return device->dispatch_table.CreateRenderPass2(_device, &create_info,
                                                   pAllocator, pRenderPass);
}

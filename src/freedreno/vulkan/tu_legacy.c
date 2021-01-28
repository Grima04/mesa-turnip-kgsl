/*
 * Copyright 2020 Valve Corporation
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Jonathan Marek <jonathan@marek.ca>
 */

#include <vulkan/vulkan.h>
#include <vulkan/vk_android_native_buffer.h> /* android tu_entrypoints.h depends on this */
#include <assert.h>

#include "tu_entrypoints.h"
#include "vk_util.h"

void
tu_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pdev,
                                          uint32_t *count,
                                          VkQueueFamilyProperties *props)
{
   if (!props)
      return tu_GetPhysicalDeviceQueueFamilyProperties2(pdev, count, NULL);

   VkQueueFamilyProperties2 props2[*count];
   for (uint32_t i = 0; i < *count; i++) {
      props2[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
      props2[i].pNext = NULL;
   }
   tu_GetPhysicalDeviceQueueFamilyProperties2(pdev, count, props2);
   for (uint32_t i = 0; i < *count; i++)
      props[i] = props2[i].queueFamilyProperties;
}

void
tu_GetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice pdev,
                                                VkFormat format,
                                                VkImageType type,
                                                VkSampleCountFlagBits samples,
                                                VkImageUsageFlags usage,
                                                VkImageTiling tiling,
                                                uint32_t *count,
                                                VkSparseImageFormatProperties *props)
{
   const VkPhysicalDeviceSparseImageFormatInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = format,
      .type = type,
      .samples = samples,
      .usage = usage,
      .tiling = tiling,
   };

   if (!props)
      return tu_GetPhysicalDeviceSparseImageFormatProperties2(pdev, &info, count, NULL);

   VkSparseImageFormatProperties2 props2[*count];
   for (uint32_t i = 0; i < *count; i++) {
      props2[i].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2;
      props2[i].pNext = NULL;
   }
   tu_GetPhysicalDeviceSparseImageFormatProperties2(pdev, &info, count, props2);
   for (uint32_t i = 0; i < *count; i++)
      props[i] = props2[i].properties;
}

void
tu_GetImageSparseMemoryRequirements(VkDevice device,
                                    VkImage image,
                                    uint32_t *count,
                                    VkSparseImageMemoryRequirements *reqs)
{
   const VkImageSparseMemoryRequirementsInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2,
      .image = image
   };

   if (!reqs)
      return tu_GetImageSparseMemoryRequirements2(device, &info, count, NULL);

   VkSparseImageMemoryRequirements2 reqs2[*count];
   for (uint32_t i = 0; i < *count; i++) {
      reqs2[i].sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2;
      reqs2[i].pNext = NULL;
   }
   tu_GetImageSparseMemoryRequirements2(device, &info, count, reqs2);
   for (uint32_t i = 0; i < *count; i++)
      reqs[i] = reqs2[i].memoryRequirements;
}

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
tu_CreateRenderPass(VkDevice device,
                    const VkRenderPassCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkRenderPass *pRenderPass)
{
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

   return tu_CreateRenderPass2(device, &create_info, pAllocator, pRenderPass);
}

void
tu_CmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo *info, VkSubpassContents contents)
{
   return tu_CmdBeginRenderPass2(cmd, info, &(VkSubpassBeginInfo) {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,
      .contents = contents
   });
}

void
tu_CmdNextSubpass(VkCommandBuffer cmd, VkSubpassContents contents)
{
   return tu_CmdNextSubpass2(cmd, &(VkSubpassBeginInfo) {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,
      .contents = contents
   }, &(VkSubpassEndInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO,
   });
}

void
tu_CmdEndRenderPass(VkCommandBuffer cmd)
{
   return tu_CmdEndRenderPass2(cmd, &(VkSubpassEndInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO,
   });
}

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


/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include "tu_private.h"

#include "vk_format.h"

#include "vk_util.h"

#include "util/format_r11g11b10f.h"
#include "util/format_srgb.h"
#include "util/u_half.h"

static void
tu_physical_device_get_format_properties(
   struct tu_physical_device *physical_device,
   VkFormat format,
   VkFormatProperties *out_properties)
{
   VkFormatFeatureFlags linear = 0, tiled = 0, buffer = 0;
   const struct vk_format_description *desc = vk_format_description(format);
   if (!desc) {
      out_properties->linearTilingFeatures = linear;
      out_properties->optimalTilingFeatures = tiled;
      out_properties->bufferFeatures = buffer;
      return;
   }

   out_properties->linearTilingFeatures = linear;
   out_properties->optimalTilingFeatures = tiled;
   out_properties->bufferFeatures = buffer;
}

void
tu_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                     VkFormat format,
                                     VkFormatProperties *pFormatProperties)
{
   TU_FROM_HANDLE(tu_physical_device, physical_device, physicalDevice);

   tu_physical_device_get_format_properties(
     physical_device, format, pFormatProperties);
}

void
tu_GetPhysicalDeviceFormatProperties2(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkFormatProperties2KHR *pFormatProperties)
{
   TU_FROM_HANDLE(tu_physical_device, physical_device, physicalDevice);

   tu_physical_device_get_format_properties(
     physical_device, format, &pFormatProperties->formatProperties);
}

static VkResult
tu_get_image_format_properties(struct tu_physical_device *physical_device,
                               const VkPhysicalDeviceImageFormatInfo2KHR *info,
                               VkImageFormatProperties *pImageFormatProperties)

{
   VkFormatProperties format_props;
   VkFormatFeatureFlags format_feature_flags;
   VkExtent3D maxExtent;
   uint32_t maxMipLevels;
   uint32_t maxArraySize;
   VkSampleCountFlags sampleCounts = VK_SAMPLE_COUNT_1_BIT;

   tu_physical_device_get_format_properties(
     physical_device, info->format, &format_props);
   if (info->tiling == VK_IMAGE_TILING_LINEAR) {
      format_feature_flags = format_props.linearTilingFeatures;
   } else if (info->tiling == VK_IMAGE_TILING_OPTIMAL) {
      format_feature_flags = format_props.optimalTilingFeatures;
   } else {
      unreachable("bad VkImageTiling");
   }

   if (format_feature_flags == 0)
      goto unsupported;

   if (info->type != VK_IMAGE_TYPE_2D &&
       vk_format_is_depth_or_stencil(info->format))
      goto unsupported;

   switch (info->type) {
      default:
         unreachable("bad vkimage type\n");
      case VK_IMAGE_TYPE_1D:
         maxExtent.width = 16384;
         maxExtent.height = 1;
         maxExtent.depth = 1;
         maxMipLevels = 15; /* log2(maxWidth) + 1 */
         maxArraySize = 2048;
         break;
      case VK_IMAGE_TYPE_2D:
         maxExtent.width = 16384;
         maxExtent.height = 16384;
         maxExtent.depth = 1;
         maxMipLevels = 15; /* log2(maxWidth) + 1 */
         maxArraySize = 2048;
         break;
      case VK_IMAGE_TYPE_3D:
         maxExtent.width = 2048;
         maxExtent.height = 2048;
         maxExtent.depth = 2048;
         maxMipLevels = 12; /* log2(maxWidth) + 1 */
         maxArraySize = 1;
         break;
   }

   if (info->tiling == VK_IMAGE_TILING_OPTIMAL &&
       info->type == VK_IMAGE_TYPE_2D &&
       (format_feature_flags &
        (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
         VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
       !(info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
       !(info->usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
      sampleCounts |=
        VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
   }

   if (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
         goto unsupported;
      }
   }

   if (info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      if (!(format_feature_flags &
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
         goto unsupported;
      }
   }

   *pImageFormatProperties = (VkImageFormatProperties){
      .maxExtent = maxExtent,
      .maxMipLevels = maxMipLevels,
      .maxArrayLayers = maxArraySize,
      .sampleCounts = sampleCounts,

      /* FINISHME: Accurately calculate
       * VkImageFormatProperties::maxResourceSize.
       */
      .maxResourceSize = UINT32_MAX,
   };

   return VK_SUCCESS;
unsupported:
   *pImageFormatProperties = (VkImageFormatProperties){
      .maxExtent = { 0, 0, 0 },
      .maxMipLevels = 0,
      .maxArrayLayers = 0,
      .sampleCounts = 0,
      .maxResourceSize = 0,
   };

   return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

VkResult
tu_GetPhysicalDeviceImageFormatProperties(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkImageType type,
   VkImageTiling tiling,
   VkImageUsageFlags usage,
   VkImageCreateFlags createFlags,
   VkImageFormatProperties *pImageFormatProperties)
{
   TU_FROM_HANDLE(tu_physical_device, physical_device, physicalDevice);

   const VkPhysicalDeviceImageFormatInfo2KHR info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR,
      .pNext = NULL,
      .format = format,
      .type = type,
      .tiling = tiling,
      .usage = usage,
      .flags = createFlags,
   };

   return tu_get_image_format_properties(
     physical_device, &info, pImageFormatProperties);
}

static void
get_external_image_format_properties(
  const VkPhysicalDeviceImageFormatInfo2KHR *pImageFormatInfo,
  VkExternalMemoryHandleTypeFlagBitsKHR handleType,
  VkExternalMemoryPropertiesKHR *external_properties)
{
   VkExternalMemoryFeatureFlagBitsKHR flags = 0;
   VkExternalMemoryHandleTypeFlagsKHR export_flags = 0;
   VkExternalMemoryHandleTypeFlagsKHR compat_flags = 0;
   switch (handleType) {
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR:
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
         switch (pImageFormatInfo->type) {
            case VK_IMAGE_TYPE_2D:
               flags = VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT_KHR |
                       VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT_KHR |
                       VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR;
               compat_flags = export_flags =
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR |
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
               break;
            default:
               break;
         }
         break;
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT:
         flags = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR;
         compat_flags = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
         break;
      default:
         break;
   }

   *external_properties = (VkExternalMemoryPropertiesKHR){
      .externalMemoryFeatures = flags,
      .exportFromImportedHandleTypes = export_flags,
      .compatibleHandleTypes = compat_flags,
   };
}

VkResult
tu_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2KHR *base_info,
   VkImageFormatProperties2KHR *base_props)
{
   TU_FROM_HANDLE(tu_physical_device, physical_device, physicalDevice);
   const VkPhysicalDeviceExternalImageFormatInfoKHR *external_info = NULL;
   VkExternalImageFormatPropertiesKHR *external_props = NULL;
   VkResult result;

   result = tu_get_image_format_properties(
     physical_device, base_info, &base_props->imageFormatProperties);
   if (result != VK_SUCCESS)
      return result;

   /* Extract input structs */
   vk_foreach_struct_const(s, base_info->pNext)
   {
      switch (s->sType) {
         case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR:
            external_info = (const void *)s;
            break;
         default:
            break;
      }
   }

   /* Extract output structs */
   vk_foreach_struct(s, base_props->pNext)
   {
      switch (s->sType) {
         case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR:
            external_props = (void *)s;
            break;
         default:
            break;
      }
   }

   /* From the Vulkan 1.0.42 spec:
    *
    *    If handleType is 0, vkGetPhysicalDeviceImageFormatProperties2KHR will
    *    behave as if VkPhysicalDeviceExternalImageFormatInfoKHR was not
    *    present and VkExternalImageFormatPropertiesKHR will be ignored.
    */
   if (external_info && external_info->handleType != 0) {
      switch (external_info->handleType) {
         case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR:
         case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
         case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT:
            get_external_image_format_properties(
              base_info,
              external_info->handleType,
              &external_props->externalMemoryProperties);
            break;
         default:
            /* From the Vulkan 1.0.42 spec:
             *
             *    If handleType is not compatible with the [parameters]
             * specified
             *    in VkPhysicalDeviceImageFormatInfo2KHR, then
             *    vkGetPhysicalDeviceImageFormatProperties2KHR returns
             *    VK_ERROR_FORMAT_NOT_SUPPORTED.
             */
            result =
              vk_errorf(physical_device->instance,
                        VK_ERROR_FORMAT_NOT_SUPPORTED,
                        "unsupported VkExternalMemoryTypeFlagBitsKHR 0x%x",
                        external_info->handleType);
            goto fail;
      }
   }

   return VK_SUCCESS;

fail:
   if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
      /* From the Vulkan 1.0.42 spec:
       *
       *    If the combination of parameters to
       *    vkGetPhysicalDeviceImageFormatProperties2KHR is not supported by
       *    the implementation for use in vkCreateImage, then all members of
       *    imageFormatProperties will be filled with zero.
       */
      base_props->imageFormatProperties = (VkImageFormatProperties){ 0 };
   }

   return result;
}

void
tu_GetPhysicalDeviceSparseImageFormatProperties(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkImageType type,
   uint32_t samples,
   VkImageUsageFlags usage,
   VkImageTiling tiling,
   uint32_t *pNumProperties,
   VkSparseImageFormatProperties *pProperties)
{
   /* Sparse images are not yet supported. */
   *pNumProperties = 0;
}

void
tu_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSparseImageFormatInfo2KHR *pFormatInfo,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties2KHR *pProperties)
{
   /* Sparse images are not yet supported. */
   *pPropertyCount = 0;
}

void
tu_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfoKHR *pExternalBufferInfo,
   VkExternalBufferPropertiesKHR *pExternalBufferProperties)
{
   VkExternalMemoryFeatureFlagBitsKHR flags = 0;
   VkExternalMemoryHandleTypeFlagsKHR export_flags = 0;
   VkExternalMemoryHandleTypeFlagsKHR compat_flags = 0;
   switch (pExternalBufferInfo->handleType) {
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR:
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
         flags = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT_KHR |
                 VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR;
         compat_flags = export_flags =
           VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR |
           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
         break;
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT:
         flags = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR;
         compat_flags = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
         break;
      default:
         break;
   }
   pExternalBufferProperties->externalMemoryProperties =
     (VkExternalMemoryPropertiesKHR){
        .externalMemoryFeatures = flags,
        .exportFromImportedHandleTypes = export_flags,
        .compatibleHandleTypes = compat_flags,
     };
}

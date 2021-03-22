/*
 * Copyright © 2015 Intel Corporation
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

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "drm-uapi/drm_fourcc.h"

#include "anv_private.h"
#include "vk_util.h"

VkResult anv_CreateDmaBufImageINTEL(
    VkDevice                                    _device,
    const VkDmaBufImageCreateInfo*              pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDeviceMemory*                             pMem,
    VkImage*                                    pImage)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_device_memory *mem;
   struct anv_image *image;
   VkResult result;
   VkImage image_h;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DMA_BUF_IMAGE_CREATE_INFO_INTEL);

   mem = vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*mem), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (mem == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   VkImageCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = pCreateInfo->format,
      .extent = pCreateInfo->extent,
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = 1,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .flags = 0,
   };

   VkImageDrmFormatModifierListCreateInfoEXT mod_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .drmFormatModifierCount = 1,
      .pDrmFormatModifiers = (uint64_t[]) { I915_FORMAT_MOD_X_TILED },
   };
   __vk_append_struct(&create_info, &mod_info);

   result = anv_image_create(_device,
      &(struct anv_image_create_info) {
         .stride = pCreateInfo->strideInBytes,
         .vk_info = &create_info,
      }, pAllocator, &image_h);
   if (result != VK_SUCCESS)
      goto fail;

   image = anv_image_from_handle(image_h);

   result = anv_device_import_bo(device, pCreateInfo->fd,
                                 ANV_BO_ALLOC_IMPLICIT_SYNC,
                                 0 /* address */,
                                 &mem->bo);
   if (result != VK_SUCCESS)
      goto fail_import;

   VkImageMemoryRequirementsInfo2 mem_reqs_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = image_h,
   };

   VkMemoryRequirements2 mem_reqs = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };

   anv_GetImageMemoryRequirements2(_device, &mem_reqs_info, &mem_reqs);

   VkDeviceSize aligned_image_size =
      align_u64(mem_reqs.memoryRequirements.size,
                mem_reqs.memoryRequirements.alignment);

   if (mem->bo->size < aligned_image_size) {
      result = vk_errorf(device, NULL, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                         "dma-buf too small for image in "
                         "vkCreateDmaBufImageINTEL: %"PRIu64"B < %"PRIu64"B",
                         mem->bo->size, aligned_image_size);
      anv_device_release_bo(device, mem->bo);
      goto fail_import;
   }

   image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN].address = (struct anv_address) {
      .bo = mem->bo,
      .offset = 0,
   };

   assert(image->extent.width > 0);
   assert(image->extent.height > 0);
   assert(image->extent.depth == 1);

   *pMem = anv_device_memory_to_handle(mem);
   *pImage = anv_image_to_handle(image);

   close(pCreateInfo->fd);

   return VK_SUCCESS;

 fail_import:
   vk_free2(&device->vk.alloc, pAllocator, image);

 fail:
   vk_free2(&device->vk.alloc, pAllocator, mem);

   return result;
}

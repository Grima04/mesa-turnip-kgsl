/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
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

#include "tu_private.h"
#include "util/debug.h"
#include "util/u_atomic.h"
#include "vk_format.h"
#include "vk_util.h"

VkResult
tu_image_create(VkDevice _device,
                const struct tu_image_create_info *create_info,
                const VkAllocationCallbacks *alloc,
                VkImage *pImage)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   const VkImageCreateInfo *pCreateInfo = create_info->vk_info;
   struct tu_image *image = NULL;
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   tu_assert(pCreateInfo->mipLevels > 0);
   tu_assert(pCreateInfo->arrayLayers > 0);
   tu_assert(pCreateInfo->samples > 0);
   tu_assert(pCreateInfo->extent.width > 0);
   tu_assert(pCreateInfo->extent.height > 0);
   tu_assert(pCreateInfo->extent.depth > 0);

   image = vk_zalloc2(&device->alloc,
                      alloc,
                      sizeof(*image),
                      8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!image)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   image->type = pCreateInfo->imageType;

   image->vk_format = pCreateInfo->format;
   image->tiling = pCreateInfo->tiling;
   image->usage = pCreateInfo->usage;
   image->flags = pCreateInfo->flags;

   image->exclusive = pCreateInfo->sharingMode == VK_SHARING_MODE_EXCLUSIVE;
   if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT) {
      for (uint32_t i = 0; i < pCreateInfo->queueFamilyIndexCount; ++i)
         if (pCreateInfo->pQueueFamilyIndices[i] ==
             VK_QUEUE_FAMILY_EXTERNAL_KHR)
            image->queue_family_mask |= (1u << TU_MAX_QUEUE_FAMILIES) - 1u;
         else
            image->queue_family_mask |= 1u
                                        << pCreateInfo->pQueueFamilyIndices[i];
   }

   image->shareable =
     vk_find_struct_const(pCreateInfo->pNext,
                          EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR) != NULL;

   *pImage = tu_image_to_handle(image);

   return VK_SUCCESS;
}

void
tu_image_view_init(struct tu_image_view *iview,
                   struct tu_device *device,
                   const VkImageViewCreateInfo *pCreateInfo)
{
}

unsigned
tu_image_queue_family_mask(const struct tu_image *image,
                           uint32_t family,
                           uint32_t queue_family)
{
   if (!image->exclusive)
      return image->queue_family_mask;
   if (family == VK_QUEUE_FAMILY_EXTERNAL_KHR)
      return (1u << TU_MAX_QUEUE_FAMILIES) - 1u;
   if (family == VK_QUEUE_FAMILY_IGNORED)
      return 1u << queue_family;
   return 1u << family;
}

VkResult
tu_CreateImage(VkDevice device,
               const VkImageCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkImage *pImage)
{
#ifdef ANDROID
   const VkNativeBufferANDROID *gralloc_info =
     vk_find_struct_const(pCreateInfo->pNext, NATIVE_BUFFER_ANDROID);

   if (gralloc_info)
      return tu_image_from_gralloc(
        device, pCreateInfo, gralloc_info, pAllocator, pImage);
#endif

   return tu_image_create(device,
                           &(struct tu_image_create_info) {
                             .vk_info = pCreateInfo,
                             .scanout = false,
                           },
                           pAllocator,
                           pImage);
}

void
tu_DestroyImage(VkDevice _device,
                VkImage _image,
                const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_image, image, _image);

   if (!image)
      return;

   if (image->owned_memory != VK_NULL_HANDLE)
      tu_FreeMemory(_device, image->owned_memory, pAllocator);

   vk_free2(&device->alloc, pAllocator, image);
}

void
tu_GetImageSubresourceLayout(VkDevice _device,
                             VkImage _image,
                             const VkImageSubresource *pSubresource,
                             VkSubresourceLayout *pLayout)
{
}

VkResult
tu_CreateImageView(VkDevice _device,
                   const VkImageViewCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkImageView *pView)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_image_view *view;

   view = vk_alloc2(&device->alloc,
                    pAllocator,
                    sizeof(*view),
                    8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (view == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   tu_image_view_init(view, device, pCreateInfo);

   *pView = tu_image_view_to_handle(view);

   return VK_SUCCESS;
}

void
tu_DestroyImageView(VkDevice _device,
                    VkImageView _iview,
                    const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_image_view, iview, _iview);

   if (!iview)
      return;
   vk_free2(&device->alloc, pAllocator, iview);
}

void
tu_buffer_view_init(struct tu_buffer_view *view,
                    struct tu_device *device,
                    const VkBufferViewCreateInfo *pCreateInfo)
{
   TU_FROM_HANDLE(tu_buffer, buffer, pCreateInfo->buffer);

   view->range = pCreateInfo->range == VK_WHOLE_SIZE
                   ? buffer->size - pCreateInfo->offset
                   : pCreateInfo->range;
   view->vk_format = pCreateInfo->format;
}

VkResult
tu_CreateBufferView(VkDevice _device,
                    const VkBufferViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkBufferView *pView)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_buffer_view *view;

   view = vk_alloc2(&device->alloc,
                    pAllocator,
                    sizeof(*view),
                    8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!view)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   tu_buffer_view_init(view, device, pCreateInfo);

   *pView = tu_buffer_view_to_handle(view);

   return VK_SUCCESS;
}

void
tu_DestroyBufferView(VkDevice _device,
                     VkBufferView bufferView,
                     const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_buffer_view, view, bufferView);

   if (!view)
      return;

   vk_free2(&device->alloc, pAllocator, view);
}

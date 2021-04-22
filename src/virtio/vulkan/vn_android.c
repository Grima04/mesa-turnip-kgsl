/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#include "vn_android.h"
#include "vn_common.h"

#include <drm/drm_fourcc.h>
#include <hardware/hwvulkan.h>
#include <vndk/hardware_buffer.h>
#include <vulkan/vk_icd.h>

#include "util/libsync.h"
#include "util/os_file.h"

#include "vn_device.h"
#include "vn_image.h"
#include "vn_queue.h"

static int
vn_hal_open(const struct hw_module_t *mod,
            const char *id,
            struct hw_device_t **dev);

static void UNUSED
static_asserts(void)
{
   STATIC_ASSERT(HWVULKAN_DISPATCH_MAGIC == ICD_LOADER_MAGIC);
}

PUBLIC struct hwvulkan_module_t HAL_MODULE_INFO_SYM = {
   .common = {
      .tag = HARDWARE_MODULE_TAG,
      .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
      .hal_api_version = HARDWARE_HAL_API_VERSION,
      .id = HWVULKAN_HARDWARE_MODULE_ID,
      .name = "Venus Vulkan HAL",
      .author = "Google LLC",
      .methods = &(hw_module_methods_t) {
         .open = vn_hal_open,
      },
   },
};

static int
vn_hal_close(UNUSED struct hw_device_t *dev)
{
   return 0;
}

static hwvulkan_device_t vn_hal_dev = {
  .common = {
     .tag = HARDWARE_DEVICE_TAG,
     .version = HWVULKAN_DEVICE_API_VERSION_0_1,
     .module = &HAL_MODULE_INFO_SYM.common,
     .close = vn_hal_close,
  },
 .EnumerateInstanceExtensionProperties = vn_EnumerateInstanceExtensionProperties,
 .CreateInstance = vn_CreateInstance,
 .GetInstanceProcAddr = vn_GetInstanceProcAddr,
};

static int
vn_hal_open(const struct hw_module_t *mod,
            const char *id,
            struct hw_device_t **dev)
{
   assert(mod == &HAL_MODULE_INFO_SYM.common);
   assert(strcmp(id, HWVULKAN_DEVICE_0) == 0);

   *dev = &vn_hal_dev.common;
   return 0;
}

VkResult
vn_GetSwapchainGrallocUsage2ANDROID(
   VkDevice device,
   VkFormat format,
   VkImageUsageFlags imageUsage,
   VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
   uint64_t *grallocConsumerUsage,
   uint64_t *grallocProducerUsage)
{
   struct vn_device *dev = vn_device_from_handle(device);
   *grallocConsumerUsage = 0;
   *grallocProducerUsage = 0;

   if (swapchainImageUsage & VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_ANDROID)
      return vn_error(dev->instance, VK_ERROR_INITIALIZATION_FAILED);

   if (VN_DEBUG(WSI))
      vn_log(dev->instance, "format=%d, imageUsage=0x%x", format, imageUsage);

   if (imageUsage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      *grallocProducerUsage |= AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;

   if (imageUsage &
       (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      *grallocConsumerUsage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   return VK_SUCCESS;
}

VkResult
vn_image_from_anb(struct vn_device *dev,
                  const VkImageCreateInfo *image_info,
                  const VkNativeBufferANDROID *anb_info,
                  const VkAllocationCallbacks *alloc,
                  struct vn_image **out_img)
{
   /* If anb_info->handle points to a classic resouce created from
    * virtio_gpu_cmd_resource_create_3d, anb_info->stride is the stride of the
    * guest shadow storage other than the host gpu storage.
    *
    * We also need to pass the correct stride to vn_CreateImage, which will be
    * done via VkImageDrmFormatModifierExplicitCreateInfoEXT and will require
    * VK_EXT_image_drm_format_modifier support in the host driver. The struct
    * also needs a modifier, which can only be encoded in anb_info->handle.
    *
    * Given above, until gralloc gets fixed to set stride correctly and to
    * encode modifier in the native handle, we will have to make assumptions.
    * (e.g. In CrOS, there's a VIRTGPU_RESOURCE_INFO_TYPE_EXTENDED kernel hack
    * for that)
    */
   VkResult result = VK_SUCCESS;
   VkDevice device = vn_device_to_handle(dev);
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkImage image = VK_NULL_HANDLE;
   struct vn_image *img = NULL;
   uint32_t mem_type_bits = 0;
   int dma_buf_fd = -1;
   int dup_fd = -1;

   if (anb_info->handle->numFds != 1) {
      if (VN_DEBUG(WSI))
         vn_log(dev->instance, "handle->numFds is %d, expected 1",
                anb_info->handle->numFds);
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   dma_buf_fd = anb_info->handle->data[0];
   if (dma_buf_fd < 0) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   /* XXX fix this!!!!! */
   uint32_t offset = 0;
   uint32_t bpp = 0;
   switch (image_info->format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
      bpp = 4;
      break;
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
      bpp = 2;
      break;
   default:
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   };
   uint32_t stride = align(image_info->extent.width * bpp, 512);
   uint64_t modifier = I915_FORMAT_MOD_X_TILED;

   const VkSubresourceLayout layout = {
      .offset = offset,
      .size = 0,
      .rowPitch = stride,
      .arrayPitch = 0,
      .depthPitch = 0,
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT drm_mod_info = {
      .sType =
         VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .pNext = image_info->pNext,
      .drmFormatModifier = modifier,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &layout,
   };
   VkImageCreateInfo local_image_info = *image_info;
   local_image_info.pNext = &drm_mod_info;
   local_image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   /* encoder will strip the Android specific pNext structs */
   result = vn_image_create(dev, &local_image_info, alloc, &img);
   if (result != VK_SUCCESS)
      goto fail;

   image = vn_image_to_handle(img);
   VkMemoryRequirements mem_req;
   vn_GetImageMemoryRequirements(device, image, &mem_req);
   if (!mem_req.memoryTypeBits) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   VkMemoryFdPropertiesKHR fd_prop = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .pNext = NULL,
      .memoryTypeBits = 0,
   };
   result = vn_GetMemoryFdPropertiesKHR(
      device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma_buf_fd,
      &fd_prop);
   if (result != VK_SUCCESS)
      goto fail;

   if (!fd_prop.memoryTypeBits) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   if (VN_DEBUG(WSI))
      vn_log(dev->instance, "memoryTypeBits = img(0x%X) & fd(0x%X)",
             mem_req.memoryTypeBits, fd_prop.memoryTypeBits);

   mem_type_bits = mem_req.memoryTypeBits & fd_prop.memoryTypeBits;
   if (!mem_type_bits) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   dup_fd = os_dupfd_cloexec(dma_buf_fd);
   if (dup_fd < 0) {
      result = (errno == EMFILE) ? VK_ERROR_TOO_MANY_OBJECTS
                                 : VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   const VkImportMemoryFdInfoKHR import_fd_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = NULL,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dup_fd,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_fd_info,
      .allocationSize = mem_req.size,
      .memoryTypeIndex = ffs(mem_type_bits) - 1,
   };
   result = vn_AllocateMemory(device, &memory_info, alloc, &memory);
   if (result != VK_SUCCESS) {
      /* only need to close the dup_fd on import failure */
      close(dup_fd);
      goto fail;
   }

   result = vn_BindImageMemory(device, image, memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   /* Android WSI image owns the memory */
   img->private_memory = memory;
   *out_img = img;

   return VK_SUCCESS;

fail:
   if (image != VK_NULL_HANDLE)
      vn_DestroyImage(device, image, alloc);
   if (memory != VK_NULL_HANDLE)
      vn_FreeMemory(device, memory, alloc);
   return vn_error(dev->instance, result);
}

VkResult
vn_AcquireImageANDROID(VkDevice device,
                       UNUSED VkImage image,
                       int nativeFenceFd,
                       VkSemaphore semaphore,
                       VkFence fence)
{
   /* At this moment, out semaphore and fence are filled with already signaled
    * payloads, and the native fence fd is waited inside until signaled.
    */
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem = vn_semaphore_from_handle(semaphore);
   struct vn_fence *fen = vn_fence_from_handle(fence);

   if (nativeFenceFd >= 0) {
      int ret = sync_wait(nativeFenceFd, INT32_MAX);
      /* Android loader expects the ICD to always close the fd */
      close(nativeFenceFd);
      if (ret)
         return vn_error(dev->instance, VK_ERROR_SURFACE_LOST_KHR);
   }

   if (sem)
      vn_semaphore_signal_wsi(dev, sem);

   if (fen)
      vn_fence_signal_wsi(dev, fen);

   return VK_SUCCESS;
}

VkResult
vn_QueueSignalReleaseImageANDROID(VkQueue queue,
                                  uint32_t waitSemaphoreCount,
                                  const VkSemaphore *pWaitSemaphores,
                                  VkImage image,
                                  int *pNativeFenceFd)
{
   /* At this moment, the wait semaphores are converted to a VkFence via an
    * empty submit. The VkFence is then waited inside until signaled, and the
    * out native fence fd is set to -1.
    */
   VkResult result = VK_SUCCESS;
   struct vn_queue *que = vn_queue_from_handle(queue);
   const VkAllocationCallbacks *alloc = &que->device->base.base.alloc;
   VkDevice device = vn_device_to_handle(que->device);
   VkPipelineStageFlags local_stage_masks[8];
   VkPipelineStageFlags *stage_masks = local_stage_masks;

   if (waitSemaphoreCount == 0)
      goto out;

   if (waitSemaphoreCount > ARRAY_SIZE(local_stage_masks)) {
      stage_masks =
         vk_alloc(alloc, sizeof(VkPipelineStageFlags) * waitSemaphoreCount,
                  VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!stage_masks) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto out;
      }
   }

   for (uint32_t i = 0; i < waitSemaphoreCount; i++)
      stage_masks[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = NULL,
      .waitSemaphoreCount = waitSemaphoreCount,
      .pWaitSemaphores = pWaitSemaphores,
      .pWaitDstStageMask = stage_masks,
      .commandBufferCount = 0,
      .pCommandBuffers = NULL,
      .signalSemaphoreCount = 0,
      .pSignalSemaphores = NULL,
   };
   result = vn_QueueSubmit(queue, 1, &submit_info, que->wait_fence);
   if (result != VK_SUCCESS)
      goto out;

   result =
      vn_WaitForFences(device, 1, &que->wait_fence, VK_TRUE, UINT64_MAX);
   vn_ResetFences(device, 1, &que->wait_fence);

out:
   *pNativeFenceFd = -1;
   return result;
}

/*
 * Copyright © 2020 Google, Inc.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_private.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "msm_kgsl.h"

static int
safe_ioctl(int fd, unsigned long request, void *arg)
{
   int ret;

   do {
      ret = ioctl(fd, request, arg);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

int
tu_drm_submitqueue_new(const struct tu_device *dev,
                       int priority,
                       uint32_t *queue_id)
{
   struct kgsl_drawctxt_create req = {
      .flags = KGSL_CONTEXT_SAVE_GMEM |
              KGSL_CONTEXT_NO_GMEM_ALLOC |
              KGSL_CONTEXT_PREAMBLE,
   };

   int ret = safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req);
   if (ret)
      return ret;

   *queue_id = req.drawctxt_id;

   return 0;
}

void
tu_drm_submitqueue_close(const struct tu_device *dev, uint32_t queue_id)
{
   struct kgsl_drawctxt_destroy req = {
      .drawctxt_id = queue_id,
   };

   safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_DRAWCTXT_DESTROY, &req);
}

VkResult
tu_bo_init_new(struct tu_device *dev, struct tu_bo *bo, uint64_t size, bool dump)
{
   struct kgsl_gpumem_alloc_id req = {
      .size = size,
   };
   int ret;

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUMEM_ALLOC_ID, &req);
   if (ret)
      return vk_error(dev->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *bo = (struct tu_bo) {
      .gem_handle = req.id,
      .size = req.mmapsize,
      .iova = req.gpuaddr,
   };

   return VK_SUCCESS;
}

VkResult
tu_bo_init_dmabuf(struct tu_device *dev,
                  struct tu_bo *bo,
                  uint64_t size,
                  int fd)
{
   tu_stub();

   return VK_SUCCESS;
}

int
tu_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   tu_stub();

   return -1;
}

VkResult
tu_bo_map(struct tu_device *dev, struct tu_bo *bo)
{
   if (bo->map)
      return VK_SUCCESS;

   uint64_t offset = bo->gem_handle << 12;
   void *map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    dev->physical_device->local_fd, offset);
   if (map == MAP_FAILED)
      return vk_error(dev->instance, VK_ERROR_MEMORY_MAP_FAILED);

   bo->map = map;

   return VK_SUCCESS;
}

void
tu_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);

   if (bo->map)
      munmap(bo->map, bo->size);

   struct kgsl_gpumem_free_id req = {
      .id = bo->gem_handle
   };

   safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_GPUMEM_FREE_ID, &req);
}

static VkResult
get_kgsl_prop(int fd, unsigned int type, void *value, size_t size)
{
   struct kgsl_device_getproperty getprop = {
      .type = type,
      .value = value,
      .sizebytes = size,
   };

   return safe_ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &getprop);
}

VkResult
tu_enumerate_devices(struct tu_instance *instance)
{
   static const char path[] = "/dev/kgsl-3d0";
   int fd;

   struct tu_physical_device *device = &instance->physical_devices[0];

   if (instance->enabled_extensions.KHR_display)
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "I can't KHR_display");

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      instance->physical_device_count = 0;
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "failed to open device %s", path);
   }

   struct kgsl_devinfo info;
   if (get_kgsl_prop(fd, KGSL_PROP_DEVICE_INFO, &info, sizeof(info)))
      goto fail;

   uint64_t gmem_iova;
   if (get_kgsl_prop(fd, KGSL_PROP_UCHE_GMEM_VADDR, &gmem_iova, sizeof(gmem_iova)))
      goto fail;

   /* kgsl version check? */

   if (instance->debug_flags & TU_DEBUG_STARTUP)
      mesa_logi("Found compatible device '%s'.", path);

   vk_object_base_init(NULL, &device->base, VK_OBJECT_TYPE_PHYSICAL_DEVICE);
   device->instance = instance;
   device->master_fd = -1;
   device->local_fd = fd;

   device->gpu_id =
      ((info.chip_id >> 24) & 0xff) * 100 +
      ((info.chip_id >> 16) & 0xff) * 10 +
      ((info.chip_id >>  8) & 0xff);
   device->gmem_size = info.gmem_sizebytes;
   device->gmem_base = gmem_iova;

   if (tu_physical_device_init(device, instance) != VK_SUCCESS)
      goto fail;

   instance->physical_device_count = 1;

   return VK_SUCCESS;

fail:
   close(fd);
   return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult
tu_QueueSubmit(VkQueue _queue,
               uint32_t submitCount,
               const VkSubmitInfo *pSubmits,
               VkFence _fence)
{
   TU_FROM_HANDLE(tu_queue, queue, _queue);
   VkResult result = VK_SUCCESS;

   uint32_t max_entry_count = 0;
   for (uint32_t i = 0; i < submitCount; ++i) {
      const VkSubmitInfo *submit = pSubmits + i;

      uint32_t entry_count = 0;
      for (uint32_t j = 0; j < submit->commandBufferCount; ++j) {
         TU_FROM_HANDLE(tu_cmd_buffer, cmdbuf, submit->pCommandBuffers[j]);
         entry_count += cmdbuf->cs.entry_count;
      }

      max_entry_count = MAX2(max_entry_count, entry_count);
   }

   struct kgsl_command_object *cmds =
      vk_alloc(&queue->device->vk.alloc,
               sizeof(cmds[0]) * max_entry_count, 8,
               VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (cmds == NULL)
      return vk_error(queue->device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < submitCount; ++i) {
      const VkSubmitInfo *submit = pSubmits + i;
      uint32_t entry_idx = 0;

      for (uint32_t j = 0; j < submit->commandBufferCount; j++) {
         TU_FROM_HANDLE(tu_cmd_buffer, cmdbuf, submit->pCommandBuffers[j]);
         struct tu_cs *cs = &cmdbuf->cs;
         for (unsigned k = 0; k < cs->entry_count; k++) {
            cmds[entry_idx++] = (struct kgsl_command_object) {
               .offset = cs->entries[k].offset,
               .gpuaddr = cs->entries[k].bo->iova,
               .size = cs->entries[k].size,
               .flags = KGSL_CMDLIST_IB,
               .id = cs->entries[k].bo->gem_handle,
            };
         }
      }

      struct kgsl_gpu_command req = {
         .flags = KGSL_CMDBATCH_SUBMIT_IB_LIST,
         .context_id = queue->msm_queue_id,
         .cmdlist = (uint64_t) (uintptr_t) cmds,
         .numcmds = entry_idx,
         .cmdsize = sizeof(struct kgsl_command_object),
      };

      int ret = safe_ioctl(queue->device->physical_device->local_fd,
                           IOCTL_KGSL_GPU_COMMAND, &req);
      if (ret) {
         result = tu_device_set_lost(queue->device,
                                     "submit failed: %s\n", strerror(errno));
         goto fail;
      }

      /* no need to merge fences as queue execution is serialized */
      if (i == submitCount - 1) {
         int fd;
         struct kgsl_timestamp_event event = {
            .type = KGSL_TIMESTAMP_EVENT_FENCE,
            .context_id = queue->msm_queue_id,
            .timestamp = req.timestamp,
            .priv = &fd,
            .len = sizeof(fd),
         };

         int ret = safe_ioctl(queue->device->physical_device->local_fd,
                              IOCTL_KGSL_TIMESTAMP_EVENT, &event);
         if (ret != 0) {
            result = tu_device_set_lost(queue->device,
                                        "Failed to create sync file for timestamp: %s\n",
                                        strerror(errno));
            goto fail;
         }

         if (queue->fence >= 0)
            close(queue->fence);
         queue->fence = fd;
      }
   }
fail:
   vk_free(&queue->device->vk.alloc, cmds);

   return result;
}

VkResult
tu_ImportSemaphoreFdKHR(VkDevice _device,
                        const VkImportSemaphoreFdInfoKHR *pImportSemaphoreFdInfo)
{
   tu_finishme("ImportSemaphoreFdKHR");
   return VK_SUCCESS;
}

VkResult
tu_GetSemaphoreFdKHR(VkDevice _device,
                     const VkSemaphoreGetFdInfoKHR *pGetFdInfo,
                     int *pFd)
{
   tu_finishme("GetSemaphoreFdKHR");
   return VK_SUCCESS;
}

VkResult
tu_CreateSemaphore(VkDevice _device,
                   const VkSemaphoreCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkSemaphore *pSemaphore)
{
   tu_finishme("CreateSemaphore");
   return VK_SUCCESS;
}

void
tu_DestroySemaphore(VkDevice _device,
                    VkSemaphore _semaphore,
                    const VkAllocationCallbacks *pAllocator)
{
   tu_finishme("DestroySemaphore");
}

VkResult
tu_ImportFenceFdKHR(VkDevice _device,
                    const VkImportFenceFdInfoKHR *pImportFenceFdInfo)
{
   tu_stub();

   return VK_SUCCESS;
}

VkResult
tu_GetFenceFdKHR(VkDevice _device,
                 const VkFenceGetFdInfoKHR *pGetFdInfo,
                 int *pFd)
{
   tu_stub();

   return VK_SUCCESS;
}

VkResult
tu_CreateFence(VkDevice _device,
               const VkFenceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkFence *pFence)
{
   tu_finishme("CreateFence");
   return VK_SUCCESS;
}

void
tu_DestroyFence(VkDevice _device, VkFence _fence, const VkAllocationCallbacks *pAllocator)
{
   tu_finishme("DestroyFence");
}

VkResult
tu_WaitForFences(VkDevice _device,
                 uint32_t fenceCount,
                 const VkFence *pFences,
                 VkBool32 waitAll,
                 uint64_t timeout)
{
   tu_finishme("WaitForFences");
   return VK_SUCCESS;
}

VkResult
tu_ResetFences(VkDevice _device, uint32_t fenceCount, const VkFence *pFences)
{
   tu_finishme("ResetFences");
   return VK_SUCCESS;
}

VkResult
tu_GetFenceStatus(VkDevice _device, VkFence _fence)
{
   tu_finishme("GetFenceStatus");
   return VK_SUCCESS;
}

int
tu_signal_fences(struct tu_device *device, struct tu_syncobj *fence1, struct tu_syncobj *fence2)
{
   tu_finishme("tu_signal_fences");
   return 0;
}

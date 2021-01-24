/*
 * Copyright © 2020 Intel Corporation
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

#include "vk_device.h"

#include "vk_common_entrypoints.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "util/hash_table.h"
#include "util/ralloc.h"

VkResult
vk_device_init(struct vk_device *device,
               struct vk_physical_device *physical_device,
               const struct vk_device_dispatch_table *dispatch_table,
               const VkDeviceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *instance_alloc,
               const VkAllocationCallbacks *device_alloc)
{
   memset(device, 0, sizeof(*device));
   vk_object_base_init(device, &device->base, VK_OBJECT_TYPE_DEVICE);
   if (device_alloc)
      device->alloc = *device_alloc;
   else
      device->alloc = *instance_alloc;

   device->physical = physical_device;

   if (dispatch_table != NULL) {
      device->dispatch_table = *dispatch_table;

      /* Add common entrypoints without overwriting driver-provided ones. */
      vk_device_dispatch_table_from_entrypoints(
         &device->dispatch_table, &vk_common_device_entrypoints, false);
   }

   if (physical_device != NULL) {
      for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
         int idx;
         for (idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
            if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                       vk_device_extensions[idx].extensionName) == 0)
               break;
         }

         if (idx >= VK_DEVICE_EXTENSION_COUNT)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

         if (!physical_device->supported_extensions.extensions[idx])
            return VK_ERROR_EXTENSION_NOT_PRESENT;

         device->enabled_extensions.extensions[idx] = true;
      }
   }

   p_atomic_set(&device->private_data_next_index, 0);

#ifdef ANDROID
   mtx_init(&device->swapchain_private_mtx, mtx_plain);
   device->swapchain_private = NULL;
#endif /* ANDROID */

   return VK_SUCCESS;
}

void
vk_device_finish(UNUSED struct vk_device *device)
{
#ifdef ANDROID
   if (device->swapchain_private) {
      hash_table_foreach(device->swapchain_private, entry)
         util_sparse_array_finish(entry->data);
      ralloc_free(device->swapchain_private);
   }
#endif /* ANDROID */

   vk_object_base_finish(&device->base);
}

PFN_vkVoidFunction
vk_device_get_proc_addr(const struct vk_device *device,
                        const char *name)
{
   if (device == NULL || name == NULL)
      return NULL;

   struct vk_instance *instance = device->physical->instance;
   return vk_device_dispatch_table_get_if_supported(&device->dispatch_table,
                                                    name,
                                                    instance->app_info.api_version,
                                                    &instance->enabled_extensions,
                                                    &device->enabled_extensions);
}

PFN_vkVoidFunction
vk_common_GetDeviceProcAddr(VkDevice _device,
                            const char *pName)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   return vk_device_get_proc_addr(device, pName);
}

/*
 * Copyright © 2021 Intel Corporation
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

#include "vk_physical_device.h"

#include "vk_common_entrypoints.h"
#include "vk_util.h"

VkResult
vk_physical_device_init(struct vk_physical_device *pdevice,
                        struct vk_instance *instance,
                        const struct vk_device_extension_table *supported_extensions,
                        const struct vk_physical_device_dispatch_table *dispatch_table)
{
   memset(pdevice, 0, sizeof(*pdevice));
   vk_object_base_init(NULL, &pdevice->base, VK_OBJECT_TYPE_PHYSICAL_DEVICE);
   pdevice->instance = instance;

   if (supported_extensions != NULL)
      pdevice->supported_extensions = *supported_extensions;

   if (dispatch_table != NULL) {
      pdevice->dispatch_table = *dispatch_table;

      /* Add common entrypoints without overwriting driver-provided ones. */
      vk_physical_device_dispatch_table_from_entrypoints(
         &pdevice->dispatch_table, &vk_common_physical_device_entrypoints, false);
   }

   return VK_SUCCESS;
}

void
vk_physical_device_finish(struct vk_physical_device *physical_device)
{
   vk_object_base_finish(&physical_device->base);
}

VkResult
vk_common_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                         uint32_t *pPropertyCount,
                                         VkLayerProperties *pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   /* None supported at this time */
   return VK_ERROR_LAYER_NOT_PRESENT;
}

VkResult
vk_common_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                             const char *pLayerName,
                                             uint32_t *pPropertyCount,
                                             VkExtensionProperties *pProperties)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

   for (int i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
      if (pdevice->supported_extensions.extensions[i]) {
         vk_outarray_append(&out, prop) {
            *prop = vk_device_extensions[i];
         }
      }
   }

   return vk_outarray_status(&out);
}

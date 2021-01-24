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

#include "util/hash_table.h"
#include "util/ralloc.h"

VkResult
vk_device_init(struct vk_device *device,
               UNUSED const VkDeviceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *instance_alloc,
               const VkAllocationCallbacks *device_alloc)
{
   vk_object_base_init(device, &device->base, VK_OBJECT_TYPE_DEVICE);
   if (device_alloc)
      device->alloc = *device_alloc;
   else
      device->alloc = *instance_alloc;

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

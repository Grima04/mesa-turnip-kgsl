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
#ifndef VK_DEVICE_H
#define VK_DEVICE_H

#include "vk_object.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_device {
   struct vk_object_base base;
   VkAllocationCallbacks alloc;

   /* For VK_EXT_private_data */
   uint32_t private_data_next_index;

#ifdef ANDROID
   mtx_t swapchain_private_mtx;
   struct hash_table *swapchain_private;
#endif
};

void vk_device_init(struct vk_device *device,
                    const VkDeviceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *instance_alloc,
                    const VkAllocationCallbacks *device_alloc);
void vk_device_finish(struct vk_device *device);

#ifdef __cplusplus
}
#endif

#endif /* VK_DEVICE_H */

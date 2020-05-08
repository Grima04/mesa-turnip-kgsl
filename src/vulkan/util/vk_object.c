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

#include "vk_object.h"

void
vk_object_base_init(UNUSED struct vk_device *device,
                    struct vk_object_base *base,
                    UNUSED VkObjectType obj_type)
{
   base->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   base->type = obj_type;
}

void
vk_object_base_finish(UNUSED struct vk_object_base *base)
{
}

void
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
}

void
vk_device_finish(UNUSED struct vk_device *device)
{
   vk_object_base_finish(&device->base);
}

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

#include "vk_alloc.h"

void
vk_object_base_init(UNUSED struct vk_device *device,
                    struct vk_object_base *base,
                    UNUSED VkObjectType obj_type)
{
   base->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   base->type = obj_type;
   util_sparse_array_init(&base->private_data, sizeof(uint64_t), 8);
}

void
vk_object_base_finish(struct vk_object_base *base)
{
   util_sparse_array_finish(&base->private_data);
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

   p_atomic_set(&device->private_data_next_index, 0);
}

void
vk_device_finish(UNUSED struct vk_device *device)
{
   vk_object_base_finish(&device->base);
}

VkResult
vk_private_data_slot_create(struct vk_device *device,
                            const VkPrivateDataSlotCreateInfoEXT* pCreateInfo,
                            const VkAllocationCallbacks* pAllocator,
                            VkPrivateDataSlotEXT* pPrivateDataSlot)
{
   struct vk_private_data_slot *slot =
      vk_alloc2(&device->alloc, pAllocator, sizeof(*slot), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (slot == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   vk_object_base_init(device, &slot->base,
                       VK_OBJECT_TYPE_PRIVATE_DATA_SLOT_EXT);
   slot->index = p_atomic_inc_return(&device->private_data_next_index);

   *pPrivateDataSlot = vk_private_data_slot_to_handle(slot);

   return VK_SUCCESS;
}

void
vk_private_data_slot_destroy(struct vk_device *device,
                             VkPrivateDataSlotEXT privateDataSlot,
                             const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_private_data_slot, slot, privateDataSlot);
   if (slot == NULL)
      return;

   vk_object_base_finish(&slot->base);
   vk_free2(&device->alloc, pAllocator, slot);
}

static uint64_t *
vk_object_base_private_data(VkObjectType objectType,
                            uint64_t objectHandle,
                            VkPrivateDataSlotEXT privateDataSlot)
{
   VK_FROM_HANDLE(vk_private_data_slot, slot, privateDataSlot);
   struct vk_object_base *obj =
      vk_object_base_from_u64_handle(objectHandle, objectType);
   return util_sparse_array_get(&obj->private_data, slot->index);
}

VkResult
vk_object_base_set_private_data(struct vk_device *device,
                                VkObjectType objectType,
                                uint64_t objectHandle,
                                VkPrivateDataSlotEXT privateDataSlot,
                                uint64_t data)
{
   uint64_t *private_data =
      vk_object_base_private_data(objectType, objectHandle, privateDataSlot);
   *private_data = data;
   return VK_SUCCESS;
}

void
vk_object_base_get_private_data(struct vk_device *device,
                                VkObjectType objectType,
                                uint64_t objectHandle,
                                VkPrivateDataSlotEXT privateDataSlot,
                                uint64_t *pData)
{
   uint64_t *private_data =
      vk_object_base_private_data(objectType, objectHandle, privateDataSlot);
   *pData = *private_data;
}

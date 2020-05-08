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
#ifndef VK_OBJECT_H
#define VK_OBJECT_H

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#include "util/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_device;

struct vk_object_base {
   VK_LOADER_DATA _loader_data;
   VkObjectType type;
};

void vk_object_base_init(UNUSED struct vk_device *device,
                         struct vk_object_base *base,
                         UNUSED VkObjectType obj_type);
void vk_object_base_finish(UNUSED struct vk_object_base *base);

static inline void
vk_object_base_assert_valid(ASSERTED struct vk_object_base *base,
                            ASSERTED VkObjectType obj_type)
{
   assert(base == NULL || base->type == obj_type);
}


struct vk_device {
   struct vk_object_base base;
   VkAllocationCallbacks alloc;
};

void vk_device_init(struct vk_device *device,
                    const VkDeviceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *instance_alloc,
                    const VkAllocationCallbacks *device_alloc);
void vk_device_finish(struct vk_device *device);

#define VK_DEFINE_HANDLE_CASTS(__driver_type, __base, __VkType, __VK_TYPE) \
   static inline struct __driver_type *                                    \
   __driver_type ## _from_handle(__VkType _handle)                         \
   {                                                                       \
      struct vk_object_base *base = (struct vk_object_base *)_handle;      \
      vk_object_base_assert_valid(base, __VK_TYPE);                        \
      STATIC_ASSERT(offsetof(struct __driver_type, __base) == 0);          \
      return (struct __driver_type *) base;                                \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __driver_type ## _to_handle(struct __driver_type *_obj)                 \
   {                                                                       \
      vk_object_base_assert_valid(&_obj->__base, __VK_TYPE);               \
      return (__VkType) _obj;                                              \
   }

#define VK_DEFINE_NONDISP_HANDLE_CASTS(__driver_type, __base, __VkType, __VK_TYPE) \
   static inline struct __driver_type *                                    \
   __driver_type ## _from_handle(__VkType _handle)                         \
   {                                                                       \
      struct vk_object_base *base =                                        \
         (struct vk_object_base *)(uintptr_t)_handle;                      \
      vk_object_base_assert_valid(base, __VK_TYPE);                        \
      STATIC_ASSERT(offsetof(struct __driver_type, __base) == 0);          \
      return (struct __driver_type *)base;                                 \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __driver_type ## _to_handle(struct __driver_type *_obj)                 \
   {                                                                       \
      vk_object_base_assert_valid(&_obj->__base, __VK_TYPE);               \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

#define VK_FROM_HANDLE(__driver_type, __name, __handle) \
   struct __driver_type *__name = __driver_type ## _from_handle(__handle)

#ifdef __cplusplus
}
#endif

#endif /* VK_OBJECT_H */

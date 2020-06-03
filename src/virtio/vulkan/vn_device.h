/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#ifndef VN_DEVICE_H
#define VN_DEVICE_H

#include "vn_common.h"

struct vn_instance {
   struct vn_instance_base base;
};
VK_DEFINE_HANDLE_CASTS(vn_instance,
                       base.base.base,
                       VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)

struct vn_physical_device {
   struct vn_physical_device_base base;

   struct vn_instance *instance;
};
VK_DEFINE_HANDLE_CASTS(vn_physical_device,
                       base.base.base,
                       VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

struct vn_device {
   struct vn_device_base base;
};
VK_DEFINE_HANDLE_CASTS(vn_device,
                       base.base.base,
                       VkDevice,
                       VK_OBJECT_TYPE_DEVICE)

struct vn_queue {
   struct vn_object_base base;

   struct vn_device *device;
};
VK_DEFINE_HANDLE_CASTS(vn_queue, base.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

struct vn_command_buffer {
   struct vn_object_base base;

   struct vn_device *device;
};
VK_DEFINE_HANDLE_CASTS(vn_command_buffer,
                       base.base,
                       VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

#endif /* VN_DEVICE_H */

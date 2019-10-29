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

#include "vn_cs.h"
#include "vn_renderer.h"
#include "vn_ring.h"

struct vn_instance {
   struct vn_instance_base base;

   struct driOptionCache dri_options;
   struct driOptionCache available_dri_options;

   struct vn_renderer *renderer;
   struct vn_renderer_info renderer_info;
   uint32_t renderer_version;

   /* to synchronize renderer/ring */
   mtx_t roundtrip_mutex;
   uint32_t roundtrip_next;

   struct {
      mtx_t mutex;
      struct vn_renderer_bo *bo;
      struct vn_ring ring;
      uint64_t id;

      struct vn_cs_encoder upload;
      uint32_t command_dropped;
   } ring;

   struct {
      struct vn_renderer_bo *bo;
      size_t size;
      size_t used;
      void *ptr;
   } reply;
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

VkResult
vn_instance_submit_roundtrip(struct vn_instance *instance,
                             uint32_t *roundtrip_seqno);

struct vn_instance_submit_command {
   /* empty command implies errors */
   struct vn_cs_encoder command;
   /* non-zero implies waiting */
   size_t reply_size;

   /* when reply_size is non-zero, NULL can be returned on errors */
   struct vn_renderer_bo *reply_bo;
   struct vn_cs_decoder reply;
};

void
vn_instance_submit_command(struct vn_instance *instance,
                           struct vn_instance_submit_command *submit);

#endif /* VN_DEVICE_H */

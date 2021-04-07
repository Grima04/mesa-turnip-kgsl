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
#include "vn_wsi.h"

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

   mtx_t physical_device_mutex;
   struct vn_physical_device *physical_devices;
   uint32_t physical_device_count;
};
VK_DEFINE_HANDLE_CASTS(vn_instance,
                       base.base.base,
                       VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)

struct vn_physical_device {
   struct vn_physical_device_base base;

   struct vn_instance *instance;

   uint32_t renderer_version;
   struct vk_device_extension_table renderer_extensions;

   uint32_t *extension_spec_versions;

   VkPhysicalDeviceFeatures2 features;
   VkPhysicalDeviceVulkan11Features vulkan_1_1_features;
   VkPhysicalDeviceVulkan12Features vulkan_1_2_features;
   VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback_features;

   VkPhysicalDeviceProperties2 properties;
   VkPhysicalDeviceVulkan11Properties vulkan_1_1_properties;
   VkPhysicalDeviceVulkan12Properties vulkan_1_2_properties;
   VkPhysicalDeviceTransformFeedbackPropertiesEXT
      transform_feedback_properties;

   VkQueueFamilyProperties2 *queue_family_properties;
   uint32_t *queue_family_sync_queue_bases;
   uint32_t queue_family_count;

   VkPhysicalDeviceMemoryProperties2 memory_properties;

   VkExternalMemoryHandleTypeFlags external_memory_handles;
   VkExternalFenceHandleTypeFlags external_fence_handles;
   VkExternalSemaphoreHandleTypeFlags external_binary_semaphore_handles;
   VkExternalSemaphoreHandleTypeFlags external_timeline_semaphore_handles;

   struct wsi_device wsi_device;
};
VK_DEFINE_HANDLE_CASTS(vn_physical_device,
                       base.base.base,
                       VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

struct vn_device_memory_pool {
   mtx_t mutex;
   struct vn_device_memory *memory;
   VkDeviceSize used;
};

struct vn_device {
   struct vn_device_base base;

   struct vn_instance *instance;
   struct vn_physical_device *physical_device;

   struct vn_queue *queues;
   uint32_t queue_count;

   struct vn_device_memory_pool memory_pools[VK_MAX_MEMORY_TYPES];
};
VK_DEFINE_HANDLE_CASTS(vn_device,
                       base.base.base,
                       VkDevice,
                       VK_OBJECT_TYPE_DEVICE)

struct vn_queue {
   struct vn_object_base base;

   struct vn_device *device;
   uint32_t family;
   uint32_t index;
   uint32_t flags;

   uint32_t sync_queue_index;

   struct vn_renderer_sync *idle_sync;
   uint64_t idle_sync_value;
};
VK_DEFINE_HANDLE_CASTS(vn_queue, base.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

enum vn_sync_type {
   /* no payload */
   VN_SYNC_TYPE_INVALID,

   /* When we signal or reset, we update both the device object and the
    * renderer sync.  When we wait or query, we use the renderer sync only.
    *
    * TODO VkFence does not need the device object
    */
   VN_SYNC_TYPE_SYNC,

   /* device object only; no renderer sync */
   VN_SYNC_TYPE_DEVICE_ONLY,

   /* already signaled by WSI */
   VN_SYNC_TYPE_WSI_SIGNALED,
};

struct vn_sync_payload {
   enum vn_sync_type type;
   struct vn_renderer_sync *sync;
};

struct vn_fence {
   struct vn_object_base base;

   struct vn_sync_payload *payload;

   struct vn_sync_payload permanent;
   struct vn_sync_payload temporary;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_fence,
                               base.base,
                               VkFence,
                               VK_OBJECT_TYPE_FENCE)

struct vn_semaphore {
   struct vn_object_base base;

   VkSemaphoreType type;

   struct vn_sync_payload *payload;

   struct vn_sync_payload permanent;
   struct vn_sync_payload temporary;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_semaphore,
                               base.base,
                               VkSemaphore,
                               VK_OBJECT_TYPE_SEMAPHORE)

struct vn_device_memory {
   struct vn_object_base base;

   VkDeviceSize size;

   /* non-NULL when suballocated */
   struct vn_device_memory *base_memory;
   /* non-NULL when mappable or external */
   struct vn_renderer_bo *base_bo;
   VkDeviceSize base_offset;

   VkDeviceSize map_end;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_device_memory,
                               base.base,
                               VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)

struct vn_buffer {
   struct vn_object_base base;

   VkMemoryRequirements2 memory_requirements;
   VkMemoryDedicatedRequirements dedicated_requirements;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_buffer,
                               base.base,
                               VkBuffer,
                               VK_OBJECT_TYPE_BUFFER)

struct vn_buffer_view {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_buffer_view,
                               base.base,
                               VkBufferView,
                               VK_OBJECT_TYPE_BUFFER_VIEW)

struct vn_image {
   struct vn_object_base base;

   VkMemoryRequirements2 memory_requirements[4];
   VkMemoryDedicatedRequirements dedicated_requirements[4];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_image,
                               base.base,
                               VkImage,
                               VK_OBJECT_TYPE_IMAGE)

struct vn_image_view {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_image_view,
                               base.base,
                               VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW)

struct vn_sampler {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_sampler,
                               base.base,
                               VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)

struct vn_sampler_ycbcr_conversion {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_sampler_ycbcr_conversion,
                               base.base,
                               VkSamplerYcbcrConversion,
                               VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION)

struct vn_descriptor_set_layout_binding {
   bool has_immutable_samplers;
};

struct vn_descriptor_set_layout {
   struct vn_object_base base;
   struct vn_descriptor_set_layout_binding bindings[];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_descriptor_set_layout,
                               base.base,
                               VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)

struct vn_descriptor_pool {
   struct vn_object_base base;

   VkAllocationCallbacks allocator;
   struct list_head descriptor_sets;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_descriptor_pool,
                               base.base,
                               VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

struct vn_update_descriptor_sets {
   uint32_t write_count;
   VkWriteDescriptorSet *writes;
   VkDescriptorImageInfo *images;
   VkDescriptorBufferInfo *buffers;
   VkBufferView *views;
};

struct vn_descriptor_set {
   struct vn_object_base base;

   const struct vn_descriptor_set_layout *layout;
   struct list_head head;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_descriptor_set,
                               base.base,
                               VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)

struct vn_descriptor_update_template_entry {
   size_t offset;
   size_t stride;
};

struct vn_descriptor_update_template {
   struct vn_object_base base;

   mtx_t mutex;
   struct vn_update_descriptor_sets *update;

   struct vn_descriptor_update_template_entry entries[];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_descriptor_update_template,
                               base.base,
                               VkDescriptorUpdateTemplate,
                               VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE)

struct vn_render_pass {
   struct vn_object_base base;

   VkExtent2D granularity;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_render_pass,
                               base.base,
                               VkRenderPass,
                               VK_OBJECT_TYPE_RENDER_PASS)

struct vn_framebuffer {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_framebuffer,
                               base.base,
                               VkFramebuffer,
                               VK_OBJECT_TYPE_FRAMEBUFFER)

struct vn_event {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_event,
                               base.base,
                               VkEvent,
                               VK_OBJECT_TYPE_EVENT)

struct vn_query_pool {
   struct vn_object_base base;

   VkAllocationCallbacks allocator;
   uint32_t result_array_size;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_query_pool,
                               base.base,
                               VkQueryPool,
                               VK_OBJECT_TYPE_QUERY_POOL)

struct vn_shader_module {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_shader_module,
                               base.base,
                               VkShaderModule,
                               VK_OBJECT_TYPE_SHADER_MODULE)

struct vn_pipeline_layout {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_pipeline_layout,
                               base.base,
                               VkPipelineLayout,
                               VK_OBJECT_TYPE_PIPELINE_LAYOUT)

struct vn_pipeline_cache {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_pipeline_cache,
                               base.base,
                               VkPipelineCache,
                               VK_OBJECT_TYPE_PIPELINE_CACHE)

struct vn_pipeline {
   struct vn_object_base base;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_pipeline,
                               base.base,
                               VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)

VkResult
vn_instance_submit_roundtrip(struct vn_instance *instance,
                             uint32_t *roundtrip_seqno);

void
vn_instance_wait_roundtrip(struct vn_instance *instance,
                           uint32_t roundtrip_seqno);

static inline void
vn_instance_roundtrip(struct vn_instance *instance)
{
   uint32_t roundtrip_seqno;
   if (vn_instance_submit_roundtrip(instance, &roundtrip_seqno) == VK_SUCCESS)
      vn_instance_wait_roundtrip(instance, roundtrip_seqno);
}

VkResult
vn_instance_ring_submit(struct vn_instance *instance,
                        const struct vn_cs_encoder *cs);

static inline void
vn_instance_ring_wait(struct vn_instance *instance)
{
   struct vn_ring *ring = &instance->ring.ring;
   vn_ring_wait_all(ring);
}

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

void
vn_fence_signal_wsi(struct vn_device *dev, struct vn_fence *fence);

void
vn_semaphore_signal_wsi(struct vn_device *dev, struct vn_semaphore *sem);

#endif /* VN_DEVICE_H */

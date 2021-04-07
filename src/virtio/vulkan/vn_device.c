/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#include "vn_device.h"

#include <stdio.h>

#include "git_sha1.h"
#include "util/driconf.h"
#include "util/mesa-sha1.h"
#include "venus-protocol/vn_protocol_driver.h"

#include "vn_icd.h"
#include "vn_renderer.h"

/* require and request at least Vulkan 1.1 at both instance and device levels
 */
#define VN_MIN_RENDERER_VERSION VK_API_VERSION_1_1

/*
 * Instance extensions add instance-level or physical-device-level
 * functionalities.  It seems renderer support is either unnecessary or
 * optional.  We should be able to advertise them or lie about them locally.
 */
static const struct vk_instance_extension_table
   vn_instance_supported_extensions = {
      /* promoted to VK_VERSION_1_1 */
      .KHR_device_group_creation = true,
      .KHR_external_fence_capabilities = true,
      .KHR_external_memory_capabilities = true,
      .KHR_external_semaphore_capabilities = true,
      .KHR_get_physical_device_properties2 = true,

      /* WSI */
#ifdef VN_USE_WSI_PLATFORM
      .KHR_get_surface_capabilities2 = true,
      .KHR_surface = true,
      .KHR_surface_protected_capabilities = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
      .KHR_wayland_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
      .KHR_xcb_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
      .KHR_xlib_surface = true,
#endif
   };

static const driOptionDescription vn_dri_options[] = {
   /* clang-format off */
   DRI_CONF_SECTION_PERFORMANCE
      DRI_CONF_VK_X11_ENSURE_MIN_IMAGE_COUNT(false)
      DRI_CONF_VK_X11_OVERRIDE_MIN_IMAGE_COUNT(0)
      DRI_CONF_VK_X11_STRICT_IMAGE_COUNT(false)
   DRI_CONF_SECTION_END
   DRI_CONF_SECTION_DEBUG
      DRI_CONF_VK_WSI_FORCE_BGRA8_UNORM_FIRST(false)
   DRI_CONF_SECTION_END
   /* clang-format on */
};

static VkResult
vn_instance_init_version(struct vn_instance *instance)
{
   uint32_t renderer_version = 0;
   VkResult result =
      vn_call_vkEnumerateInstanceVersion(instance, &renderer_version);
   if (result != VK_SUCCESS) {
      if (VN_DEBUG(INIT))
         vn_log(instance, "failed to enumerate renderer instance version");
      return result;
   }

   if (renderer_version < VN_MIN_RENDERER_VERSION) {
      if (VN_DEBUG(INIT)) {
         vn_log(instance, "unsupported renderer instance version %d.%d",
                VK_VERSION_MAJOR(instance->renderer_version),
                VK_VERSION_MINOR(instance->renderer_version));
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   instance->renderer_version =
      instance->base.base.app_info.api_version > VN_MIN_RENDERER_VERSION
         ? instance->base.base.app_info.api_version
         : VN_MIN_RENDERER_VERSION;

   if (VN_DEBUG(INIT)) {
      vn_log(instance, "vk instance version %d.%d.%d",
             VK_VERSION_MAJOR(instance->renderer_version),
             VK_VERSION_MINOR(instance->renderer_version),
             VK_VERSION_PATCH(instance->renderer_version));
   }

   return VK_SUCCESS;
}

static VkResult
vn_instance_init_ring(struct vn_instance *instance)
{
   /* 32-bit seqno for renderer roundtrips */
   const size_t extra_size = sizeof(uint32_t);
   struct vn_ring_layout layout;
   vn_ring_get_layout(extra_size, &layout);

   void *ring_ptr;
   VkResult result = vn_renderer_bo_create_cpu(
      instance->renderer, layout.bo_size, &instance->ring.bo);
   if (result == VK_SUCCESS) {
      ring_ptr = vn_renderer_bo_map(instance->ring.bo);
      if (!ring_ptr)
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   if (result != VK_SUCCESS) {
      if (VN_DEBUG(INIT))
         vn_log(instance, "failed to allocate/map ring bo");
      return result;
   }

   mtx_init(&instance->ring.mutex, mtx_plain);

   struct vn_ring *ring = &instance->ring.ring;
   vn_ring_init(ring, &layout, ring_ptr);

   instance->ring.id = (uintptr_t)ring;

   const struct VkRingCreateInfoMESA info = {
      .sType = VK_STRUCTURE_TYPE_RING_CREATE_INFO_MESA,
      .resourceId = instance->ring.bo->res_id,
      .size = layout.bo_size,
      .idleTimeout = 50ull * 1000 * 1000,
      .headOffset = layout.head_offset,
      .tailOffset = layout.tail_offset,
      .statusOffset = layout.status_offset,
      .bufferOffset = layout.buffer_offset,
      .bufferSize = layout.buffer_size,
      .extraOffset = layout.extra_offset,
      .extraSize = layout.extra_size,
   };

   uint32_t create_ring_data[64];
   struct vn_cs_encoder local_enc =
      VN_CS_ENCODER_INITIALIZER(create_ring_data, sizeof(create_ring_data));
   vn_encode_vkCreateRingMESA(&local_enc, 0, instance->ring.id, &info);
   vn_renderer_submit_simple(instance->renderer, create_ring_data,
                             vn_cs_encoder_get_len(&local_enc));

   vn_cs_encoder_init_indirect(&instance->ring.upload, instance,
                               1 * 1024 * 1024);

   return VK_SUCCESS;
}

static VkResult
vn_instance_init_renderer(struct vn_instance *instance)
{
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   VkResult result = vn_renderer_create(instance, alloc, &instance->renderer);
   if (result != VK_SUCCESS)
      return result;

   mtx_init(&instance->roundtrip_mutex, mtx_plain);
   instance->roundtrip_next = 1;

   vn_renderer_get_info(instance->renderer, &instance->renderer_info);

   uint32_t version = vn_info_wire_format_version();
   if (instance->renderer_info.wire_format_version != version) {
      if (VN_DEBUG(INIT)) {
         vn_log(instance, "wire format version %d != %d",
                instance->renderer_info.wire_format_version, version);
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   version = vn_info_vk_xml_version();
   if (instance->renderer_info.vk_xml_version > version)
      instance->renderer_info.vk_xml_version = version;

   version = vn_info_extension_spec_version("VK_EXT_command_serialization");
   if (instance->renderer_info.vk_ext_command_serialization_spec_version >
       version) {
      instance->renderer_info.vk_ext_command_serialization_spec_version =
         version;
   }

   version = vn_info_extension_spec_version("VK_MESA_venus_protocol");
   if (instance->renderer_info.vk_mesa_venus_protocol_spec_version >
       version) {
      instance->renderer_info.vk_mesa_venus_protocol_spec_version = version;
   }

   if (VN_DEBUG(INIT)) {
      vn_log(instance, "connected to renderer");
      vn_log(instance, "wire format version %d",
             instance->renderer_info.wire_format_version);
      vn_log(instance, "vk xml version %d.%d.%d",
             VK_VERSION_MAJOR(instance->renderer_info.vk_xml_version),
             VK_VERSION_MINOR(instance->renderer_info.vk_xml_version),
             VK_VERSION_PATCH(instance->renderer_info.vk_xml_version));
      vn_log(
         instance, "VK_EXT_command_serialization spec version %d",
         instance->renderer_info.vk_ext_command_serialization_spec_version);
      vn_log(instance, "VK_MESA_venus_protocol spec version %d",
             instance->renderer_info.vk_mesa_venus_protocol_spec_version);
   }

   return VK_SUCCESS;
}

VkResult
vn_instance_submit_roundtrip(struct vn_instance *instance,
                             uint32_t *roundtrip_seqno)
{
   uint32_t write_ring_extra_data[8];
   struct vn_cs_encoder local_enc = VN_CS_ENCODER_INITIALIZER(
      write_ring_extra_data, sizeof(write_ring_extra_data));

   /* submit a vkWriteRingExtraMESA through the renderer */
   mtx_lock(&instance->roundtrip_mutex);
   const uint32_t seqno = instance->roundtrip_next++;
   vn_encode_vkWriteRingExtraMESA(&local_enc, 0, instance->ring.id, 0, seqno);
   VkResult result =
      vn_renderer_submit_simple(instance->renderer, write_ring_extra_data,
                                vn_cs_encoder_get_len(&local_enc));
   mtx_unlock(&instance->roundtrip_mutex);

   *roundtrip_seqno = seqno;
   return result;
}

void
vn_instance_wait_roundtrip(struct vn_instance *instance,
                           uint32_t roundtrip_seqno)
{
   const struct vn_ring *ring = &instance->ring.ring;
   const volatile atomic_uint *ptr = ring->shared.extra;
   uint32_t iter = 0;
   do {
      const uint32_t cur = atomic_load_explicit(ptr, memory_order_acquire);
      if (cur >= roundtrip_seqno || roundtrip_seqno - cur >= INT32_MAX)
         break;
      vn_relax(&iter);
   } while (true);
}

struct vn_instance_submission {
   uint32_t local_cs_data[64];

   void *cs_data;
   size_t cs_size;
   struct vn_ring_submit *submit;
};

static void *
vn_instance_submission_indirect_cs(struct vn_instance_submission *submit,
                                   const struct vn_cs_encoder *cs,
                                   size_t *cs_size)
{
   VkCommandStreamDescriptionMESA local_descs[8];
   VkCommandStreamDescriptionMESA *descs = local_descs;
   if (cs->buffer_count > ARRAY_SIZE(local_descs)) {
      descs =
         malloc(sizeof(VkCommandStreamDescriptionMESA) * cs->buffer_count);
      if (!descs)
         return NULL;
   }

   uint32_t desc_count = 0;
   for (uint32_t i = 0; i < cs->buffer_count; i++) {
      const struct vn_cs_encoder_buffer *buf = &cs->buffers[i];
      if (buf->committed_size) {
         descs[desc_count++] = (VkCommandStreamDescriptionMESA){
            .resourceId = buf->bo->res_id,
            .offset = buf->offset,
            .size = buf->committed_size,
         };
      }
   }

   const size_t exec_size = vn_sizeof_vkExecuteCommandStreamsMESA(
      desc_count, descs, NULL, 0, NULL, 0);
   void *exec_data = submit->local_cs_data;
   if (exec_size > sizeof(submit->local_cs_data)) {
      exec_data = malloc(exec_size);
      if (!exec_data)
         goto out;
   }

   struct vn_cs_encoder local_enc =
      VN_CS_ENCODER_INITIALIZER(exec_data, exec_size);
   vn_encode_vkExecuteCommandStreamsMESA(&local_enc, 0, desc_count, descs,
                                         NULL, 0, NULL, 0);

   *cs_size = vn_cs_encoder_get_len(&local_enc);

out:
   if (descs != local_descs)
      free(descs);

   return exec_data;
}

static void *
vn_instance_submission_direct_cs(struct vn_instance_submission *submit,
                                 const struct vn_cs_encoder *cs,
                                 size_t *cs_size)
{
   if (cs->buffer_count == 1) {
      *cs_size = cs->buffers[0].committed_size;
      return cs->buffers[0].base;
   }

   assert(vn_cs_encoder_get_len(cs) <= sizeof(submit->local_cs_data));
   void *dst = submit->local_cs_data;
   for (uint32_t i = 0; i < cs->buffer_count; i++) {
      const struct vn_cs_encoder_buffer *buf = &cs->buffers[i];
      memcpy(dst, buf->base, buf->committed_size);
      dst += buf->committed_size;
   }

   *cs_size = dst - (void *)submit->local_cs_data;
   return submit->local_cs_data;
}

static struct vn_ring_submit *
vn_instance_submission_get_ring_submit(struct vn_ring *ring,
                                       const struct vn_cs_encoder *cs,
                                       struct vn_renderer_bo *extra_bo,
                                       bool direct)
{
   const uint32_t bo_count =
      (direct ? 0 : cs->buffer_count) + (extra_bo ? 1 : 0);
   struct vn_ring_submit *submit = vn_ring_get_submit(ring, bo_count);
   if (!submit)
      return NULL;

   submit->bo_count = bo_count;
   if (!direct) {
      for (uint32_t i = 0; i < cs->buffer_count; i++)
         submit->bos[i] = vn_renderer_bo_ref(cs->buffers[i].bo);
   }
   if (extra_bo)
      submit->bos[bo_count - 1] = vn_renderer_bo_ref(extra_bo);

   return submit;
}

static void
vn_instance_submission_cleanup(struct vn_instance_submission *submit,
                               const struct vn_cs_encoder *cs)
{
   if (submit->cs_data != submit->local_cs_data &&
       submit->cs_data != cs->buffers[0].base)
      free(submit->cs_data);
}

static VkResult
vn_instance_submission_prepare(struct vn_instance_submission *submit,
                               const struct vn_cs_encoder *cs,
                               struct vn_ring *ring,
                               struct vn_renderer_bo *extra_bo,
                               bool direct)
{
   if (direct) {
      submit->cs_data =
         vn_instance_submission_direct_cs(submit, cs, &submit->cs_size);
   } else {
      submit->cs_data =
         vn_instance_submission_indirect_cs(submit, cs, &submit->cs_size);
   }
   if (!submit->cs_data)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   submit->submit =
      vn_instance_submission_get_ring_submit(ring, cs, extra_bo, direct);
   if (!submit->submit) {
      vn_instance_submission_cleanup(submit, cs);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   return VK_SUCCESS;
}

static bool
vn_instance_submission_can_direct(const struct vn_cs_encoder *cs)
{
   struct vn_instance_submission submit;
   return vn_cs_encoder_get_len(cs) <= sizeof(submit.local_cs_data);
}

static struct vn_cs_encoder *
vn_instance_ring_cs_upload_locked(struct vn_instance *instance,
                                  const struct vn_cs_encoder *cs)
{
   assert(!cs->indirect && cs->buffer_count == 1);
   const void *cs_data = cs->buffers[0].base;
   const size_t cs_size = cs->total_committed_size;
   assert(cs_size == vn_cs_encoder_get_len(cs));

   struct vn_cs_encoder *upload = &instance->ring.upload;
   vn_cs_encoder_reset(upload);

   if (!vn_cs_encoder_reserve(upload, cs_size))
      return NULL;

   vn_cs_encoder_write(upload, cs_size, cs_data, cs_size);
   vn_cs_encoder_commit(upload);
   vn_instance_wait_roundtrip(instance, upload->current_buffer_roundtrip);

   return upload;
}

static VkResult
vn_instance_ring_submit_locked(struct vn_instance *instance,
                               const struct vn_cs_encoder *cs,
                               struct vn_renderer_bo *extra_bo,
                               uint32_t *ring_seqno)
{
   struct vn_ring *ring = &instance->ring.ring;

   const bool direct = vn_instance_submission_can_direct(cs);
   if (!direct && !cs->indirect) {
      cs = vn_instance_ring_cs_upload_locked(instance, cs);
      if (!cs)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      assert(cs->indirect);
   }

   struct vn_instance_submission submit;
   VkResult result =
      vn_instance_submission_prepare(&submit, cs, ring, extra_bo, direct);
   if (result != VK_SUCCESS)
      return result;

   uint32_t seqno;
   const bool notify = vn_ring_submit(ring, submit.submit, submit.cs_data,
                                      submit.cs_size, &seqno);
   if (notify) {
      uint32_t notify_ring_data[8];
      struct vn_cs_encoder local_enc = VN_CS_ENCODER_INITIALIZER(
         notify_ring_data, sizeof(notify_ring_data));
      vn_encode_vkNotifyRingMESA(&local_enc, 0, instance->ring.id, seqno, 0);
      vn_renderer_submit_simple(instance->renderer, notify_ring_data,
                                vn_cs_encoder_get_len(&local_enc));
   }

   vn_instance_submission_cleanup(&submit, cs);

   if (ring_seqno)
      *ring_seqno = seqno;

   return VK_SUCCESS;
}

VkResult
vn_instance_ring_submit(struct vn_instance *instance,
                        const struct vn_cs_encoder *cs)
{
   mtx_lock(&instance->ring.mutex);
   VkResult result = vn_instance_ring_submit_locked(instance, cs, NULL, NULL);
   mtx_unlock(&instance->ring.mutex);

   return result;
}

static bool
vn_instance_grow_reply_bo_locked(struct vn_instance *instance, size_t size)
{
   const size_t min_bo_size = 1 << 20;

   size_t bo_size = instance->reply.size ? instance->reply.size : min_bo_size;
   while (bo_size < size) {
      bo_size <<= 1;
      if (!bo_size)
         return false;
   }

   struct vn_renderer_bo *bo;
   VkResult result =
      vn_renderer_bo_create_cpu(instance->renderer, bo_size, &bo);
   if (result != VK_SUCCESS)
      return false;

   void *ptr = vn_renderer_bo_map(bo);
   if (!ptr) {
      vn_renderer_bo_unref(bo);
      return false;
   }

   if (instance->reply.bo)
      vn_renderer_bo_unref(instance->reply.bo);
   instance->reply.bo = bo;
   instance->reply.size = bo_size;
   instance->reply.used = 0;
   instance->reply.ptr = ptr;

   return true;
}

static struct vn_renderer_bo *
vn_instance_get_reply_bo_locked(struct vn_instance *instance,
                                size_t size,
                                void **ptr)
{
   if (unlikely(instance->reply.used + size > instance->reply.size)) {
      if (!vn_instance_grow_reply_bo_locked(instance, size))
         return NULL;

      uint32_t set_reply_command_stream_data[16];
      struct vn_cs_encoder local_enc =
         VN_CS_ENCODER_INITIALIZER(set_reply_command_stream_data,
                                   sizeof(set_reply_command_stream_data));
      const struct VkCommandStreamDescriptionMESA stream = {
         .resourceId = instance->reply.bo->res_id,
         .size = instance->reply.size,
      };
      vn_encode_vkSetReplyCommandStreamMESA(&local_enc, 0, &stream);
      vn_cs_encoder_commit(&local_enc);

      vn_instance_roundtrip(instance);
      vn_instance_ring_submit_locked(instance, &local_enc, NULL, NULL);
   }

   /* TODO avoid this seek command and go lock-free? */
   uint32_t seek_reply_command_stream_data[8];
   struct vn_cs_encoder local_enc = VN_CS_ENCODER_INITIALIZER(
      seek_reply_command_stream_data, sizeof(seek_reply_command_stream_data));
   const size_t offset = instance->reply.used;
   vn_encode_vkSeekReplyCommandStreamMESA(&local_enc, 0, offset);
   vn_cs_encoder_commit(&local_enc);
   vn_instance_ring_submit_locked(instance, &local_enc, NULL, NULL);

   *ptr = instance->reply.ptr + offset;
   instance->reply.used += size;

   return vn_renderer_bo_ref(instance->reply.bo);
}

void
vn_instance_submit_command(struct vn_instance *instance,
                           struct vn_instance_submit_command *submit)
{
   void *reply_ptr;
   submit->reply_bo = NULL;

   mtx_lock(&instance->ring.mutex);

   if (vn_cs_encoder_is_empty(&submit->command))
      goto fail;
   vn_cs_encoder_commit(&submit->command);

   if (submit->reply_size) {
      submit->reply_bo = vn_instance_get_reply_bo_locked(
         instance, submit->reply_size, &reply_ptr);
      if (!submit->reply_bo)
         goto fail;
   }

   uint32_t ring_seqno;
   VkResult result = vn_instance_ring_submit_locked(
      instance, &submit->command, submit->reply_bo, &ring_seqno);

   mtx_unlock(&instance->ring.mutex);

   submit->reply = VN_CS_DECODER_INITIALIZER(reply_ptr, submit->reply_size);

   if (submit->reply_size && result == VK_SUCCESS)
      vn_ring_wait(&instance->ring.ring, ring_seqno);

   return;

fail:
   instance->ring.command_dropped++;
   mtx_unlock(&instance->ring.mutex);
}

static struct vn_physical_device *
vn_instance_find_physical_device(struct vn_instance *instance,
                                 vn_object_id id)
{
   for (uint32_t i = 0; i < instance->physical_device_count; i++) {
      if (instance->physical_devices[i].base.id == id)
         return &instance->physical_devices[i];
   }
   return NULL;
}

static void
vn_physical_device_init_features(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   struct {
      /* Vulkan 1.1 */
      VkPhysicalDevice16BitStorageFeatures sixteen_bit_storage;
      VkPhysicalDeviceMultiviewFeatures multiview;
      VkPhysicalDeviceVariablePointersFeatures variable_pointers;
      VkPhysicalDeviceProtectedMemoryFeatures protected_memory;
      VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr_conversion;
      VkPhysicalDeviceShaderDrawParametersFeatures shader_draw_parameters;

      /* Vulkan 1.2 */
      VkPhysicalDevice8BitStorageFeatures eight_bit_storage;
      VkPhysicalDeviceShaderAtomicInt64Features shader_atomic_int64;
      VkPhysicalDeviceShaderFloat16Int8Features shader_float16_int8;
      VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing;
      VkPhysicalDeviceScalarBlockLayoutFeatures scalar_block_layout;
      VkPhysicalDeviceImagelessFramebufferFeatures imageless_framebuffer;
      VkPhysicalDeviceUniformBufferStandardLayoutFeatures
         uniform_buffer_standard_layout;
      VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures
         shader_subgroup_extended_types;
      VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures
         separate_depth_stencil_layouts;
      VkPhysicalDeviceHostQueryResetFeatures host_query_reset;
      VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore;
      VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address;
      VkPhysicalDeviceVulkanMemoryModelFeatures vulkan_memory_model;
   } local_feats;

   physical_dev->features.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
   if (physical_dev->renderer_version >= VK_API_VERSION_1_2) {
      physical_dev->features.pNext = &physical_dev->vulkan_1_1_features;

      physical_dev->vulkan_1_1_features.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
      physical_dev->vulkan_1_1_features.pNext =
         &physical_dev->vulkan_1_2_features;
      physical_dev->vulkan_1_2_features.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
      physical_dev->vulkan_1_2_features.pNext = NULL;
   } else {
      physical_dev->features.pNext = &local_feats.sixteen_bit_storage;

      local_feats.sixteen_bit_storage.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
      local_feats.sixteen_bit_storage.pNext = &local_feats.multiview;
      local_feats.multiview.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
      local_feats.multiview.pNext = &local_feats.variable_pointers;
      local_feats.variable_pointers.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES;
      local_feats.variable_pointers.pNext = &local_feats.protected_memory;
      local_feats.protected_memory.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES;
      local_feats.protected_memory.pNext =
         &local_feats.sampler_ycbcr_conversion;
      local_feats.sampler_ycbcr_conversion.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
      local_feats.sampler_ycbcr_conversion.pNext =
         &local_feats.shader_draw_parameters;
      local_feats.shader_draw_parameters.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
      local_feats.shader_draw_parameters.pNext =
         &local_feats.eight_bit_storage;

      local_feats.eight_bit_storage.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
      local_feats.eight_bit_storage.pNext = &local_feats.shader_atomic_int64;
      local_feats.shader_atomic_int64.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
      local_feats.shader_atomic_int64.pNext =
         &local_feats.shader_float16_int8;
      local_feats.shader_float16_int8.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
      local_feats.shader_float16_int8.pNext =
         &local_feats.descriptor_indexing;
      local_feats.descriptor_indexing.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
      local_feats.descriptor_indexing.pNext =
         &local_feats.scalar_block_layout;
      local_feats.scalar_block_layout.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
      local_feats.scalar_block_layout.pNext =
         &local_feats.imageless_framebuffer;
      local_feats.imageless_framebuffer.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES;
      local_feats.imageless_framebuffer.pNext =
         &local_feats.uniform_buffer_standard_layout;
      local_feats.uniform_buffer_standard_layout.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES;
      local_feats.uniform_buffer_standard_layout.pNext =
         &local_feats.shader_subgroup_extended_types;
      local_feats.shader_subgroup_extended_types.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES;
      local_feats.shader_subgroup_extended_types.pNext =
         &local_feats.separate_depth_stencil_layouts;
      local_feats.separate_depth_stencil_layouts.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES;
      local_feats.separate_depth_stencil_layouts.pNext =
         &local_feats.host_query_reset;
      local_feats.host_query_reset.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
      local_feats.host_query_reset.pNext = &local_feats.timeline_semaphore;
      local_feats.timeline_semaphore.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
      local_feats.timeline_semaphore.pNext =
         &local_feats.buffer_device_address;
      local_feats.buffer_device_address.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
      local_feats.buffer_device_address.pNext =
         &local_feats.vulkan_memory_model;
      local_feats.vulkan_memory_model.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
      local_feats.vulkan_memory_model.pNext = NULL;
   }

   if (physical_dev->renderer_extensions.EXT_transform_feedback) {
      physical_dev->transform_feedback_features.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
      physical_dev->transform_feedback_features.pNext =
         physical_dev->features.pNext;
      physical_dev->features.pNext =
         &physical_dev->transform_feedback_features;
   }

   vn_call_vkGetPhysicalDeviceFeatures2(
      instance, vn_physical_device_to_handle(physical_dev),
      &physical_dev->features);

   const struct vk_device_extension_table *exts =
      &physical_dev->renderer_extensions;
   struct VkPhysicalDeviceVulkan11Features *vk11_feats =
      &physical_dev->vulkan_1_1_features;
   struct VkPhysicalDeviceVulkan12Features *vk12_feats =
      &physical_dev->vulkan_1_2_features;

   if (physical_dev->renderer_version < VK_API_VERSION_1_2) {
      vk11_feats->storageBuffer16BitAccess =
         local_feats.sixteen_bit_storage.storageBuffer16BitAccess;
      vk11_feats->uniformAndStorageBuffer16BitAccess =
         local_feats.sixteen_bit_storage.uniformAndStorageBuffer16BitAccess;
      vk11_feats->storagePushConstant16 =
         local_feats.sixteen_bit_storage.storagePushConstant16;
      vk11_feats->storageInputOutput16 =
         local_feats.sixteen_bit_storage.storageInputOutput16;

      vk11_feats->multiview = local_feats.multiview.multiview;
      vk11_feats->multiviewGeometryShader =
         local_feats.multiview.multiviewGeometryShader;
      vk11_feats->multiviewTessellationShader =
         local_feats.multiview.multiviewTessellationShader;

      vk11_feats->variablePointersStorageBuffer =
         local_feats.variable_pointers.variablePointersStorageBuffer;
      vk11_feats->variablePointers =
         local_feats.variable_pointers.variablePointers;

      vk11_feats->protectedMemory =
         local_feats.protected_memory.protectedMemory;

      vk11_feats->samplerYcbcrConversion =
         local_feats.sampler_ycbcr_conversion.samplerYcbcrConversion;

      vk11_feats->shaderDrawParameters =
         local_feats.shader_draw_parameters.shaderDrawParameters;

      vk12_feats->samplerMirrorClampToEdge =
         exts->KHR_sampler_mirror_clamp_to_edge;
      vk12_feats->drawIndirectCount = exts->KHR_draw_indirect_count;

      if (exts->KHR_8bit_storage) {
         vk12_feats->storageBuffer8BitAccess =
            local_feats.eight_bit_storage.storageBuffer8BitAccess;
         vk12_feats->uniformAndStorageBuffer8BitAccess =
            local_feats.eight_bit_storage.uniformAndStorageBuffer8BitAccess;
         vk12_feats->storagePushConstant8 =
            local_feats.eight_bit_storage.storagePushConstant8;
      }
      if (exts->KHR_shader_atomic_int64) {
         vk12_feats->shaderBufferInt64Atomics =
            local_feats.shader_atomic_int64.shaderBufferInt64Atomics;
         vk12_feats->shaderSharedInt64Atomics =
            local_feats.shader_atomic_int64.shaderSharedInt64Atomics;
      }
      if (exts->KHR_shader_float16_int8) {
         vk12_feats->shaderFloat16 =
            local_feats.shader_float16_int8.shaderFloat16;
         vk12_feats->shaderInt8 = local_feats.shader_float16_int8.shaderInt8;
      }
      if (exts->EXT_descriptor_indexing) {
         vk12_feats->descriptorIndexing = true;
         vk12_feats->shaderInputAttachmentArrayDynamicIndexing =
            local_feats.descriptor_indexing
               .shaderInputAttachmentArrayDynamicIndexing;
         vk12_feats->shaderUniformTexelBufferArrayDynamicIndexing =
            local_feats.descriptor_indexing
               .shaderUniformTexelBufferArrayDynamicIndexing;
         vk12_feats->shaderStorageTexelBufferArrayDynamicIndexing =
            local_feats.descriptor_indexing
               .shaderStorageTexelBufferArrayDynamicIndexing;
         vk12_feats->shaderUniformBufferArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderUniformBufferArrayNonUniformIndexing;
         vk12_feats->shaderSampledImageArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderSampledImageArrayNonUniformIndexing;
         vk12_feats->shaderStorageBufferArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderStorageBufferArrayNonUniformIndexing;
         vk12_feats->shaderStorageImageArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderStorageImageArrayNonUniformIndexing;
         vk12_feats->shaderInputAttachmentArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderInputAttachmentArrayNonUniformIndexing;
         vk12_feats->shaderUniformTexelBufferArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderUniformTexelBufferArrayNonUniformIndexing;
         vk12_feats->shaderStorageTexelBufferArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderStorageTexelBufferArrayNonUniformIndexing;
         vk12_feats->descriptorBindingUniformBufferUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingUniformBufferUpdateAfterBind;
         vk12_feats->descriptorBindingSampledImageUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingSampledImageUpdateAfterBind;
         vk12_feats->descriptorBindingStorageImageUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingStorageImageUpdateAfterBind;
         vk12_feats->descriptorBindingStorageBufferUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingStorageBufferUpdateAfterBind;
         vk12_feats->descriptorBindingUniformTexelBufferUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingUniformTexelBufferUpdateAfterBind;
         vk12_feats->descriptorBindingStorageTexelBufferUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingStorageTexelBufferUpdateAfterBind;
         vk12_feats->descriptorBindingUpdateUnusedWhilePending =
            local_feats.descriptor_indexing
               .descriptorBindingUpdateUnusedWhilePending;
         vk12_feats->descriptorBindingPartiallyBound =
            local_feats.descriptor_indexing.descriptorBindingPartiallyBound;
         vk12_feats->descriptorBindingVariableDescriptorCount =
            local_feats.descriptor_indexing
               .descriptorBindingVariableDescriptorCount;
         vk12_feats->runtimeDescriptorArray =
            local_feats.descriptor_indexing.runtimeDescriptorArray;
      }

      vk12_feats->samplerFilterMinmax = exts->EXT_sampler_filter_minmax;

      if (exts->EXT_scalar_block_layout) {
         vk12_feats->scalarBlockLayout =
            local_feats.scalar_block_layout.scalarBlockLayout;
      }
      if (exts->KHR_imageless_framebuffer) {
         vk12_feats->imagelessFramebuffer =
            local_feats.imageless_framebuffer.imagelessFramebuffer;
      }
      if (exts->KHR_uniform_buffer_standard_layout) {
         vk12_feats->uniformBufferStandardLayout =
            local_feats.uniform_buffer_standard_layout
               .uniformBufferStandardLayout;
      }
      if (exts->KHR_shader_subgroup_extended_types) {
         vk12_feats->shaderSubgroupExtendedTypes =
            local_feats.shader_subgroup_extended_types
               .shaderSubgroupExtendedTypes;
      }
      if (exts->KHR_separate_depth_stencil_layouts) {
         vk12_feats->separateDepthStencilLayouts =
            local_feats.separate_depth_stencil_layouts
               .separateDepthStencilLayouts;
      }
      if (exts->EXT_host_query_reset) {
         vk12_feats->hostQueryReset =
            local_feats.host_query_reset.hostQueryReset;
      }
      if (exts->KHR_timeline_semaphore) {
         vk12_feats->timelineSemaphore =
            local_feats.timeline_semaphore.timelineSemaphore;
      }
      if (exts->KHR_buffer_device_address) {
         vk12_feats->bufferDeviceAddress =
            local_feats.buffer_device_address.bufferDeviceAddress;
         vk12_feats->bufferDeviceAddressCaptureReplay =
            local_feats.buffer_device_address.bufferDeviceAddressCaptureReplay;
         vk12_feats->bufferDeviceAddressMultiDevice =
            local_feats.buffer_device_address.bufferDeviceAddressMultiDevice;
      }
      if (exts->KHR_vulkan_memory_model) {
         vk12_feats->vulkanMemoryModel =
            local_feats.vulkan_memory_model.vulkanMemoryModel;
         vk12_feats->vulkanMemoryModelDeviceScope =
            local_feats.vulkan_memory_model.vulkanMemoryModelDeviceScope;
         vk12_feats->vulkanMemoryModelAvailabilityVisibilityChains =
            local_feats.vulkan_memory_model
               .vulkanMemoryModelAvailabilityVisibilityChains;
      }

      vk12_feats->shaderOutputViewportIndex =
         exts->EXT_shader_viewport_index_layer;
      vk12_feats->shaderOutputLayer = exts->EXT_shader_viewport_index_layer;
      vk12_feats->subgroupBroadcastDynamicId = false;
   }
}

static void
vn_physical_device_init_uuids(struct vn_physical_device *physical_dev)
{
   struct VkPhysicalDeviceProperties *props =
      &physical_dev->properties.properties;
   struct VkPhysicalDeviceVulkan11Properties *vk11_props =
      &physical_dev->vulkan_1_1_properties;
   struct VkPhysicalDeviceVulkan12Properties *vk12_props =
      &physical_dev->vulkan_1_2_properties;
   struct mesa_sha1 sha1_ctx;
   uint8_t sha1[SHA1_DIGEST_LENGTH];

   static_assert(VK_UUID_SIZE <= SHA1_DIGEST_LENGTH, "");

   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, &props->pipelineCacheUUID,
                     sizeof(props->pipelineCacheUUID));
   _mesa_sha1_final(&sha1_ctx, sha1);

   memcpy(props->pipelineCacheUUID, sha1, VK_UUID_SIZE);

   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, &props->vendorID, sizeof(props->vendorID));
   _mesa_sha1_update(&sha1_ctx, &props->deviceID, sizeof(props->deviceID));
   _mesa_sha1_final(&sha1_ctx, sha1);

   memcpy(vk11_props->deviceUUID, sha1, VK_UUID_SIZE);

   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, vk12_props->driverName,
                     strlen(vk12_props->driverName));
   _mesa_sha1_update(&sha1_ctx, vk12_props->driverInfo,
                     strlen(vk12_props->driverInfo));
   _mesa_sha1_final(&sha1_ctx, sha1);

   memcpy(vk11_props->driverUUID, sha1, VK_UUID_SIZE);

   memset(vk11_props->deviceLUID, 0, VK_LUID_SIZE);
   vk11_props->deviceNodeMask = 0;
   vk11_props->deviceLUIDValid = false;
}

static void
vn_physical_device_init_properties(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   struct {
      /* Vulkan 1.1 */
      VkPhysicalDeviceIDProperties id;
      VkPhysicalDeviceSubgroupProperties subgroup;
      VkPhysicalDevicePointClippingProperties point_clipping;
      VkPhysicalDeviceMultiviewProperties multiview;
      VkPhysicalDeviceProtectedMemoryProperties protected_memory;
      VkPhysicalDeviceMaintenance3Properties maintenance_3;

      /* Vulkan 1.2 */
      VkPhysicalDeviceDriverProperties driver;
      VkPhysicalDeviceFloatControlsProperties float_controls;
      VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing;
      VkPhysicalDeviceDepthStencilResolveProperties depth_stencil_resolve;
      VkPhysicalDeviceSamplerFilterMinmaxProperties sampler_filter_minmax;
      VkPhysicalDeviceTimelineSemaphoreProperties timeline_semaphore;
   } local_props;

   physical_dev->properties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
   if (physical_dev->renderer_version >= VK_API_VERSION_1_2) {
      physical_dev->properties.pNext = &physical_dev->vulkan_1_1_properties;

      physical_dev->vulkan_1_1_properties.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
      physical_dev->vulkan_1_1_properties.pNext =
         &physical_dev->vulkan_1_2_properties;
      physical_dev->vulkan_1_2_properties.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
      physical_dev->vulkan_1_2_properties.pNext = NULL;
   } else {
      physical_dev->properties.pNext = &local_props.id;

      local_props.id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
      local_props.id.pNext = &local_props.subgroup;
      local_props.subgroup.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
      local_props.subgroup.pNext = &local_props.point_clipping;
      local_props.point_clipping.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES;
      local_props.point_clipping.pNext = &local_props.multiview;
      local_props.multiview.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES;
      local_props.multiview.pNext = &local_props.protected_memory;
      local_props.protected_memory.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES;
      local_props.protected_memory.pNext = &local_props.maintenance_3;
      local_props.maintenance_3.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
      local_props.maintenance_3.pNext = &local_props.driver;

      local_props.driver.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      local_props.driver.pNext = &local_props.float_controls;
      local_props.float_controls.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;
      local_props.float_controls.pNext = &local_props.descriptor_indexing;
      local_props.descriptor_indexing.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
      local_props.descriptor_indexing.pNext =
         &local_props.depth_stencil_resolve;
      local_props.depth_stencil_resolve.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
      local_props.depth_stencil_resolve.pNext =
         &local_props.sampler_filter_minmax;
      local_props.sampler_filter_minmax.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES;
      local_props.sampler_filter_minmax.pNext =
         &local_props.timeline_semaphore;
      local_props.timeline_semaphore.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES;
      local_props.timeline_semaphore.pNext = NULL;
   }

   if (physical_dev->renderer_extensions.EXT_transform_feedback) {
      physical_dev->transform_feedback_properties.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT;
      physical_dev->transform_feedback_properties.pNext =
         physical_dev->properties.pNext;
      physical_dev->properties.pNext =
         &physical_dev->transform_feedback_properties;
   }

   vn_call_vkGetPhysicalDeviceProperties2(
      instance, vn_physical_device_to_handle(physical_dev),
      &physical_dev->properties);

   const struct vk_device_extension_table *exts =
      &physical_dev->renderer_extensions;
   struct VkPhysicalDeviceProperties *props =
      &physical_dev->properties.properties;
   struct VkPhysicalDeviceVulkan11Properties *vk11_props =
      &physical_dev->vulkan_1_1_properties;
   struct VkPhysicalDeviceVulkan12Properties *vk12_props =
      &physical_dev->vulkan_1_2_properties;

   if (physical_dev->renderer_version < VK_API_VERSION_1_2) {
      memcpy(vk11_props->deviceUUID, local_props.id.deviceUUID,
             sizeof(vk11_props->deviceUUID));
      memcpy(vk11_props->driverUUID, local_props.id.driverUUID,
             sizeof(vk11_props->driverUUID));
      memcpy(vk11_props->deviceLUID, local_props.id.deviceLUID,
             sizeof(vk11_props->deviceLUID));
      vk11_props->deviceNodeMask = local_props.id.deviceNodeMask;
      vk11_props->deviceLUIDValid = local_props.id.deviceLUIDValid;

      vk11_props->subgroupSize = local_props.subgroup.subgroupSize;
      vk11_props->subgroupSupportedStages =
         local_props.subgroup.supportedStages;
      vk11_props->subgroupSupportedOperations =
         local_props.subgroup.supportedOperations;
      vk11_props->subgroupQuadOperationsInAllStages =
         local_props.subgroup.quadOperationsInAllStages;

      vk11_props->pointClippingBehavior =
         local_props.point_clipping.pointClippingBehavior;

      vk11_props->maxMultiviewViewCount =
         local_props.multiview.maxMultiviewViewCount;
      vk11_props->maxMultiviewInstanceIndex =
         local_props.multiview.maxMultiviewInstanceIndex;

      vk11_props->protectedNoFault =
         local_props.protected_memory.protectedNoFault;

      vk11_props->maxPerSetDescriptors =
         local_props.maintenance_3.maxPerSetDescriptors;
      vk11_props->maxMemoryAllocationSize =
         local_props.maintenance_3.maxMemoryAllocationSize;

      if (exts->KHR_driver_properties) {
         vk12_props->driverID = local_props.driver.driverID;
         memcpy(vk12_props->driverName, local_props.driver.driverName,
                VK_MAX_DRIVER_NAME_SIZE);
         memcpy(vk12_props->driverInfo, local_props.driver.driverInfo,
                VK_MAX_DRIVER_INFO_SIZE);
         vk12_props->conformanceVersion =
            local_props.driver.conformanceVersion;
      }
      if (exts->KHR_shader_float_controls) {
         vk12_props->denormBehaviorIndependence =
            local_props.float_controls.denormBehaviorIndependence;
         vk12_props->roundingModeIndependence =
            local_props.float_controls.roundingModeIndependence;
         vk12_props->shaderSignedZeroInfNanPreserveFloat16 =
            local_props.float_controls.shaderSignedZeroInfNanPreserveFloat16;
         vk12_props->shaderSignedZeroInfNanPreserveFloat32 =
            local_props.float_controls.shaderSignedZeroInfNanPreserveFloat32;
         vk12_props->shaderSignedZeroInfNanPreserveFloat64 =
            local_props.float_controls.shaderSignedZeroInfNanPreserveFloat64;
         vk12_props->shaderDenormPreserveFloat16 =
            local_props.float_controls.shaderDenormPreserveFloat16;
         vk12_props->shaderDenormPreserveFloat32 =
            local_props.float_controls.shaderDenormPreserveFloat32;
         vk12_props->shaderDenormPreserveFloat64 =
            local_props.float_controls.shaderDenormPreserveFloat64;
         vk12_props->shaderDenormFlushToZeroFloat16 =
            local_props.float_controls.shaderDenormFlushToZeroFloat16;
         vk12_props->shaderDenormFlushToZeroFloat32 =
            local_props.float_controls.shaderDenormFlushToZeroFloat32;
         vk12_props->shaderDenormFlushToZeroFloat64 =
            local_props.float_controls.shaderDenormFlushToZeroFloat64;
         vk12_props->shaderRoundingModeRTEFloat16 =
            local_props.float_controls.shaderRoundingModeRTEFloat16;
         vk12_props->shaderRoundingModeRTEFloat32 =
            local_props.float_controls.shaderRoundingModeRTEFloat32;
         vk12_props->shaderRoundingModeRTEFloat64 =
            local_props.float_controls.shaderRoundingModeRTEFloat64;
         vk12_props->shaderRoundingModeRTZFloat16 =
            local_props.float_controls.shaderRoundingModeRTZFloat16;
         vk12_props->shaderRoundingModeRTZFloat32 =
            local_props.float_controls.shaderRoundingModeRTZFloat32;
         vk12_props->shaderRoundingModeRTZFloat64 =
            local_props.float_controls.shaderRoundingModeRTZFloat64;
      }
      if (exts->EXT_descriptor_indexing) {
         vk12_props->maxUpdateAfterBindDescriptorsInAllPools =
            local_props.descriptor_indexing
               .maxUpdateAfterBindDescriptorsInAllPools;
         vk12_props->shaderUniformBufferArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderUniformBufferArrayNonUniformIndexingNative;
         vk12_props->shaderSampledImageArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderSampledImageArrayNonUniformIndexingNative;
         vk12_props->shaderStorageBufferArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderStorageBufferArrayNonUniformIndexingNative;
         vk12_props->shaderStorageImageArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderStorageImageArrayNonUniformIndexingNative;
         vk12_props->shaderInputAttachmentArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderInputAttachmentArrayNonUniformIndexingNative;
         vk12_props->robustBufferAccessUpdateAfterBind =
            local_props.descriptor_indexing.robustBufferAccessUpdateAfterBind;
         vk12_props->quadDivergentImplicitLod =
            local_props.descriptor_indexing.quadDivergentImplicitLod;
         vk12_props->maxPerStageDescriptorUpdateAfterBindSamplers =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindSamplers;
         vk12_props->maxPerStageDescriptorUpdateAfterBindUniformBuffers =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindUniformBuffers;
         vk12_props->maxPerStageDescriptorUpdateAfterBindStorageBuffers =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindStorageBuffers;
         vk12_props->maxPerStageDescriptorUpdateAfterBindSampledImages =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindSampledImages;
         vk12_props->maxPerStageDescriptorUpdateAfterBindStorageImages =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindStorageImages;
         vk12_props->maxPerStageDescriptorUpdateAfterBindInputAttachments =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindInputAttachments;
         vk12_props->maxPerStageUpdateAfterBindResources =
            local_props.descriptor_indexing
               .maxPerStageUpdateAfterBindResources;
         vk12_props->maxDescriptorSetUpdateAfterBindSamplers =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindSamplers;
         vk12_props->maxDescriptorSetUpdateAfterBindUniformBuffers =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindUniformBuffers;
         vk12_props->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
         vk12_props->maxDescriptorSetUpdateAfterBindStorageBuffers =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindStorageBuffers;
         vk12_props->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindStorageBuffersDynamic;
         vk12_props->maxDescriptorSetUpdateAfterBindSampledImages =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindSampledImages;
         vk12_props->maxDescriptorSetUpdateAfterBindStorageImages =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindStorageImages;
         vk12_props->maxDescriptorSetUpdateAfterBindInputAttachments =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindInputAttachments;
      }
      if (exts->KHR_depth_stencil_resolve) {
         vk12_props->supportedDepthResolveModes =
            local_props.depth_stencil_resolve.supportedDepthResolveModes;
         vk12_props->supportedStencilResolveModes =
            local_props.depth_stencil_resolve.supportedStencilResolveModes;
         vk12_props->independentResolveNone =
            local_props.depth_stencil_resolve.independentResolveNone;
         vk12_props->independentResolve =
            local_props.depth_stencil_resolve.independentResolve;
      }
      if (exts->EXT_sampler_filter_minmax) {
         vk12_props->filterMinmaxSingleComponentFormats =
            local_props.sampler_filter_minmax
               .filterMinmaxSingleComponentFormats;
         vk12_props->filterMinmaxImageComponentMapping =
            local_props.sampler_filter_minmax
               .filterMinmaxImageComponentMapping;
      }
      if (exts->KHR_timeline_semaphore) {
         vk12_props->maxTimelineSemaphoreValueDifference =
            local_props.timeline_semaphore.maxTimelineSemaphoreValueDifference;
      }

      vk12_props->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   }

   const uint32_t version_override = vk_get_version_override();
   if (version_override) {
      props->apiVersion = version_override;
   } else {
      if (props->apiVersion > VK_HEADER_VERSION_COMPLETE)
         props->apiVersion = VK_HEADER_VERSION_COMPLETE;
      if (props->apiVersion > vn_info_vk_xml_version())
         props->apiVersion = vn_info_vk_xml_version();
      if (!instance->renderer_info.has_timeline_sync &&
          props->apiVersion >= VK_API_VERSION_1_2)
         props->apiVersion = VK_MAKE_VERSION(1, 1, 130);
   }

   props->driverVersion = vk_get_driver_version();
   props->vendorID = instance->renderer_info.pci.vendor_id;
   props->deviceID = instance->renderer_info.pci.device_id;
   /* some apps don't like VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU */
   props->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
   snprintf(props->deviceName, sizeof(props->deviceName), "Virtio GPU");

   vk12_props->driverID = 0;
   snprintf(vk12_props->driverName, sizeof(vk12_props->driverName), "venus");
   snprintf(vk12_props->driverInfo, sizeof(vk12_props->driverInfo),
            "Mesa " PACKAGE_VERSION MESA_GIT_SHA1);
   vk12_props->conformanceVersion = (VkConformanceVersionKHR){
      .major = 0,
      .minor = 0,
      .subminor = 0,
      .patch = 0,
   };

   vn_physical_device_init_uuids(physical_dev);
}

static VkResult
vn_physical_device_init_queue_family_properties(
   struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;
   uint32_t count;

   vn_call_vkGetPhysicalDeviceQueueFamilyProperties2(
      instance, vn_physical_device_to_handle(physical_dev), &count, NULL);

   uint32_t *sync_queue_bases;
   VkQueueFamilyProperties2 *props =
      vk_alloc(alloc, (sizeof(*props) + sizeof(*sync_queue_bases)) * count,
               VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!props)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   sync_queue_bases = (uint32_t *)&props[count];

   for (uint32_t i = 0; i < count; i++) {
      props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
      /* define an extension to query sync queue base? */
      props[i].pNext = NULL;
   }
   vn_call_vkGetPhysicalDeviceQueueFamilyProperties2(
      instance, vn_physical_device_to_handle(physical_dev), &count, props);

   physical_dev->queue_family_properties = props;
   /* sync_queue_bases will be initialized later */
   physical_dev->queue_family_sync_queue_bases = sync_queue_bases;
   physical_dev->queue_family_count = count;

   return VK_SUCCESS;
}

static void
vn_physical_device_init_memory_properties(
   struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;

   physical_dev->memory_properties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;

   vn_call_vkGetPhysicalDeviceMemoryProperties2(
      instance, vn_physical_device_to_handle(physical_dev),
      &physical_dev->memory_properties);

   if (!instance->renderer_info.has_cache_management) {
      VkPhysicalDeviceMemoryProperties *props =
         &physical_dev->memory_properties.memoryProperties;
      const uint32_t host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

      for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
         const bool coherent = props->memoryTypes[i].propertyFlags &
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
         if (!coherent)
            props->memoryTypes[i].propertyFlags &= ~host_flags;
      }
   }
}

static void
vn_physical_device_init_external_memory_handles(
   struct vn_physical_device *physical_dev)
{
   if (!physical_dev->instance->renderer_info.has_dmabuf_import)
      return;

   /* We have export support but we don't advertise it.  It is for WSI only at
    * the moment.  For import support, we need to be able to serialize
    * vkGetMemoryFdPropertiesKHR and VkImportMemoryFdInfoKHR.  We can
    * serialize fd to bo->res_id, but we probably want to add new
    * commands/structs first (using VK_MESA_venus_protocol).
    *
    * We also create a BO when a vn_device_memory is mappable.  We don't know
    * which handle type the renderer uses.  That seems fine though.
    */
}

static void
vn_physical_device_init_external_fence_handles(
   struct vn_physical_device *physical_dev)
{
   if (!physical_dev->instance->renderer_info.has_external_sync)
      return;

   /* In the current model, a vn_fence can be implemented entirely on top of
    * vn_renderer_sync.  All operations can go through the renderer sync.
    *
    * The current code still creates a host-side VkFence, which can be
    * eliminated.  The renderer also lacks proper external sync (i.e.,
    * drm_syncobj) support and we can only support handle types with copy
    * transference (i.e., sync fds).
    *
    * We are considering creating a vn_renderer_sync from a host-side VkFence
    * instead, similar to how a vn_renderer_bo is created from a host-side
    * VkDeviceMemory.  That will require tons of works on the host side, but
    * should allow us to get rid of ring<->renderer syncs in vkQueueSubmit.
    */
   physical_dev->external_fence_handles =
      VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
}

static void
vn_physical_device_init_external_semaphore_handles(
   struct vn_physical_device *physical_dev)
{
   /* In the current model, it is not possible to support external semaphores.
    * At least an external semaphore cannot be waited on GPU in the host but
    * can only be waited on CPU in the guest.
    *
    * A binary vn_semaphore is implemented solely on top of a host-side binary
    * VkSemaphore.  There is no CPU operation against binary semaphroes and
    * there is no need for vn_renderer_sync.
    *
    * A timeline vn_semaphore is implemented on top of both a host-side
    * timeline VkSemaphore and a vn_renderer_sync.  Whenever a timeline
    * vn_semaphore is updated, we make sure both the host-side timeline
    * VkSemaphore and the vn_renderer_sync are updated.  This allows us to use
    * whichever is more convenient depending on the operations: the host-side
    * timeline VkSemaphore for GPU waits and the vn_renderer_sync for CPU
    * waits/gets.
    *
    * To support external semaphores, we should create a vn_renderer_sync from
    * a host-side VkSemaphore instead, similar to how a vn_renderer_bo is
    * created from a host-side VkDeviceMemory.  The reasons to make a similar
    * move for fences apply to timeline semaphores as well.  Besides, the
    * external handle (drm_syncobj or sync file) needs to carry the necessary
    * information to identify the host-side semaphore.
    */
}

static void
vn_physical_device_get_supported_extensions(
   const struct vn_physical_device *device,
   struct vk_device_extension_table *supported,
   struct vk_device_extension_table *recognized)
{
   *supported = (struct vk_device_extension_table){
      /* WSI */
#ifdef VN_USE_WSI_PLATFORM
      .KHR_incremental_present = true,
      .KHR_swapchain = true,
      .KHR_swapchain_mutable_format = true,
#endif
   };

   *recognized = (struct vk_device_extension_table){
      /* promoted to VK_VERSION_1_1 */
      .KHR_16bit_storage = true,
      .KHR_bind_memory2 = true,
      .KHR_dedicated_allocation = true,
      .KHR_descriptor_update_template = true,
      .KHR_device_group = true,
      .KHR_external_fence = true,
      .KHR_external_memory = true,
      .KHR_external_semaphore = true,
      .KHR_get_memory_requirements2 = true,
      .KHR_maintenance1 = true,
      .KHR_maintenance2 = true,
      .KHR_maintenance3 = true,
      .KHR_multiview = true,
      .KHR_relaxed_block_layout = true,
      .KHR_sampler_ycbcr_conversion = true,
      .KHR_shader_draw_parameters = true,
      .KHR_storage_buffer_storage_class = true,
      .KHR_variable_pointers = true,

      /* promoted to VK_VERSION_1_2 */
      .KHR_8bit_storage = true,
      .KHR_buffer_device_address = true,
      .KHR_create_renderpass2 = true,
      .KHR_depth_stencil_resolve = true,
      .KHR_draw_indirect_count = true,
      .KHR_driver_properties = true,
      .KHR_image_format_list = true,
      .KHR_imageless_framebuffer = true,
      .KHR_sampler_mirror_clamp_to_edge = true,
      .KHR_separate_depth_stencil_layouts = true,
      .KHR_shader_atomic_int64 = true,
      .KHR_shader_float16_int8 = true,
      .KHR_shader_float_controls = true,
      .KHR_shader_subgroup_extended_types = true,
      .KHR_spirv_1_4 = true,
      .KHR_timeline_semaphore = true,
      .KHR_uniform_buffer_standard_layout = true,
      .KHR_vulkan_memory_model = true,
      .EXT_descriptor_indexing = true,
      .EXT_host_query_reset = true,
      .EXT_sampler_filter_minmax = true,
      .EXT_scalar_block_layout = true,
      .EXT_separate_stencil_usage = true,
      .EXT_shader_viewport_index_layer = true,

      /* EXT */
      .EXT_image_drm_format_modifier = true,
      .EXT_transform_feedback = true,
   };
}

static VkResult
vn_physical_device_init_extensions(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   /* get renderer extensions */
   uint32_t count;
   VkResult result = vn_call_vkEnumerateDeviceExtensionProperties(
      instance, vn_physical_device_to_handle(physical_dev), NULL, &count,
      NULL);
   if (result != VK_SUCCESS)
      return result;

   VkExtensionProperties *exts = NULL;
   if (count) {
      exts = vk_alloc(alloc, sizeof(*exts) * count, VN_DEFAULT_ALIGN,
                      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!exts)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      result = vn_call_vkEnumerateDeviceExtensionProperties(
         instance, vn_physical_device_to_handle(physical_dev), NULL, &count,
         exts);
      if (result < VK_SUCCESS) {
         vk_free(alloc, exts);
         return result;
      }
   }

   struct vk_device_extension_table supported;
   struct vk_device_extension_table recognized;
   vn_physical_device_get_supported_extensions(physical_dev, &supported,
                                               &recognized);
   if (!instance->renderer_info.has_timeline_sync)
      recognized.KHR_timeline_semaphore = false;

   physical_dev->extension_spec_versions =
      vk_zalloc(alloc,
                sizeof(*physical_dev->extension_spec_versions) *
                   VK_DEVICE_EXTENSION_COUNT,
                VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!physical_dev->extension_spec_versions) {
      vk_free(alloc, exts);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   for (uint32_t i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
      const VkExtensionProperties *props = &vk_device_extensions[i];
      const VkExtensionProperties *renderer_props = NULL;

      for (uint32_t j = 0; j < count; j++) {
         if (!strcmp(props->extensionName, exts[j].extensionName)) {
            physical_dev->renderer_extensions.extensions[i] = true;
            renderer_props = &exts[j];
            break;
         }
      }

#ifdef ANDROID
      if (!vk_android_allowed_device_extensions.extensions[i])
         continue;
#endif

      /* does not depend on renderer (e.g., WSI) */
      if (supported.extensions[i]) {
         physical_dev->base.base.supported_extensions.extensions[i] = true;
         physical_dev->extension_spec_versions[i] = props->specVersion;
         continue;
      }

      /* no driver support */
      if (!recognized.extensions[i])
         continue;

      /* check renderer support */
      if (!renderer_props)
         continue;

      /* check encoder support */
      const uint32_t spec_version =
         vn_info_extension_spec_version(props->extensionName);
      if (!spec_version)
         continue;

      physical_dev->base.base.supported_extensions.extensions[i] = true;
      physical_dev->extension_spec_versions[i] =
         MIN2(renderer_props->specVersion, spec_version);
   }

   vk_free(alloc, exts);

   return VK_SUCCESS;
}

static VkResult
vn_physical_device_init_version(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;

   /*
    * We either check and enable VK_KHR_get_physical_device_properties2, or we
    * must use vkGetPhysicalDeviceProperties to get the device-level version.
    */
   VkPhysicalDeviceProperties props;
   vn_call_vkGetPhysicalDeviceProperties(
      instance, vn_physical_device_to_handle(physical_dev), &props);
   if (props.apiVersion < VN_MIN_RENDERER_VERSION) {
      if (VN_DEBUG(INIT)) {
         vn_log(instance, "unsupported renderer device version %d.%d",
                VK_VERSION_MAJOR(props.apiVersion),
                VK_VERSION_MINOR(props.apiVersion));
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   physical_dev->renderer_version = props.apiVersion;
   if (physical_dev->renderer_version > instance->renderer_version)
      physical_dev->renderer_version = instance->renderer_version;

   return VK_SUCCESS;
}

static VkResult
vn_physical_device_init(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   VkResult result = vn_physical_device_init_version(physical_dev);
   if (result != VK_SUCCESS)
      return result;

   result = vn_physical_device_init_extensions(physical_dev);
   if (result != VK_SUCCESS)
      return result;

   /* TODO query all caps with minimal round trips */
   vn_physical_device_init_features(physical_dev);
   vn_physical_device_init_properties(physical_dev);

   result = vn_physical_device_init_queue_family_properties(physical_dev);
   if (result != VK_SUCCESS)
      goto fail;

   vn_physical_device_init_memory_properties(physical_dev);

   vn_physical_device_init_external_memory_handles(physical_dev);
   vn_physical_device_init_external_fence_handles(physical_dev);
   vn_physical_device_init_external_semaphore_handles(physical_dev);

   result = vn_wsi_init(physical_dev);
   if (result != VK_SUCCESS)
      goto fail;

   return VK_SUCCESS;

fail:
   vk_free(alloc, physical_dev->extension_spec_versions);
   vk_free(alloc, physical_dev->queue_family_properties);
   return result;
}

static void
vn_physical_device_fini(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   vn_wsi_fini(physical_dev);
   vk_free(alloc, physical_dev->extension_spec_versions);
   vk_free(alloc, physical_dev->queue_family_properties);

   vn_physical_device_base_fini(&physical_dev->base);
}

static VkResult
vn_instance_enumerate_physical_devices(struct vn_instance *instance)
{
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;
   struct vn_physical_device *physical_devs = NULL;
   VkResult result;

   mtx_lock(&instance->physical_device_mutex);

   if (instance->physical_devices) {
      result = VK_SUCCESS;
      goto out;
   }

   uint32_t count;
   result = vn_call_vkEnumeratePhysicalDevices(
      instance, vn_instance_to_handle(instance), &count, NULL);
   if (result != VK_SUCCESS || !count)
      goto out;

   physical_devs =
      vk_zalloc(alloc, sizeof(*physical_devs) * count, VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!physical_devs) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }

   VkPhysicalDevice *handles =
      vk_alloc(alloc, sizeof(*handles) * count, VN_DEFAULT_ALIGN,
               VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!handles) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }

   for (uint32_t i = 0; i < count; i++) {
      struct vn_physical_device *physical_dev = &physical_devs[i];

      struct vk_physical_device_dispatch_table dispatch_table;
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &vn_physical_device_entrypoints, true);
      result = vn_physical_device_base_init(
         &physical_dev->base, &instance->base, NULL, &dispatch_table);
      if (result != VK_SUCCESS) {
         count = i;
         goto out;
      }

      physical_dev->instance = instance;

      handles[i] = vn_physical_device_to_handle(physical_dev);
   }

   result = vn_call_vkEnumeratePhysicalDevices(
      instance, vn_instance_to_handle(instance), &count, handles);
   vk_free(alloc, handles);

   if (result != VK_SUCCESS)
      goto out;

   uint32_t sync_queue_base = 0;
   uint32_t i = 0;
   while (i < count) {
      struct vn_physical_device *physical_dev = &physical_devs[i];

      result = vn_physical_device_init(physical_dev);
      if (result == VK_SUCCESS) {
         /* TODO assign sync queues more fairly */
         for (uint32_t j = 0; j < physical_dev->queue_family_count; j++) {
            const VkQueueFamilyProperties *props =
               &physical_dev->queue_family_properties[j].queueFamilyProperties;

            if (sync_queue_base + props->queueCount >
                instance->renderer_info.max_sync_queue_count) {
               if (VN_DEBUG(INIT)) {
                  vn_log(instance, "not enough sync queues (max %d)",
                         instance->renderer_info.max_sync_queue_count);
               }
               result = VK_ERROR_INITIALIZATION_FAILED;
               break;
            }

            physical_dev->queue_family_sync_queue_bases[j] = sync_queue_base;
            sync_queue_base += props->queueCount;
         }
      }

      if (result != VK_SUCCESS) {
         vn_physical_device_base_fini(&physical_devs[i].base);
         memmove(&physical_devs[i], &physical_devs[i + 1],
                 sizeof(*physical_devs) * (count - i - 1));
         count--;
         continue;
      }

      i++;
   }

   if (count) {
      instance->physical_devices = physical_devs;
      instance->physical_device_count = count;
      result = VK_SUCCESS;
   }

out:
   if (result != VK_SUCCESS && physical_devs) {
      for (uint32_t i = 0; i < count; i++)
         vn_physical_device_base_fini(&physical_devs[i].base);
      vk_free(alloc, physical_devs);
   }

   mtx_unlock(&instance->physical_device_mutex);
   return result;
}

/* instance commands */

VkResult
vn_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   *pApiVersion = VK_HEADER_VERSION_COMPLETE;
   return VK_SUCCESS;
}

VkResult
vn_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                        uint32_t *pPropertyCount,
                                        VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vn_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &vn_instance_supported_extensions, pPropertyCount, pProperties);
}

VkResult
vn_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                    VkLayerProperties *pProperties)
{
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

VkResult
vn_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkInstance *pInstance)
{
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : vn_default_allocator();
   struct vn_instance *instance;
   VkResult result;

   vn_debug_init();

   instance = vk_zalloc(alloc, sizeof(*instance), VN_DEFAULT_ALIGN,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vn_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &vn_instance_entrypoints, true);
   result = vn_instance_base_init(&instance->base,
                                  &vn_instance_supported_extensions,
                                  &dispatch_table, pCreateInfo, alloc);
   if (result != VK_SUCCESS) {
      vk_free(alloc, instance);
      return vn_error(NULL, result);
   }

   mtx_init(&instance->physical_device_mutex, mtx_plain);

   if (!vn_icd_supports_api_version(
          instance->base.base.app_info.api_version)) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail;
   }

   if (pCreateInfo->enabledLayerCount) {
      result = VK_ERROR_LAYER_NOT_PRESENT;
      goto fail;
   }

   result = vn_instance_init_renderer(instance);
   if (result != VK_SUCCESS)
      goto fail;

   result = vn_instance_init_ring(instance);
   if (result != VK_SUCCESS)
      goto fail;

   result = vn_instance_init_version(instance);
   if (result != VK_SUCCESS)
      goto fail;

   VkInstanceCreateInfo local_create_info;
   local_create_info = *pCreateInfo;
   local_create_info.ppEnabledExtensionNames = NULL;
   local_create_info.enabledExtensionCount = 0;
   pCreateInfo = &local_create_info;

   /* request at least instance->renderer_version */
   VkApplicationInfo local_app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = instance->renderer_version,
   };
   if (instance->base.base.app_info.api_version <
       instance->renderer_version) {
      if (pCreateInfo->pApplicationInfo) {
         local_app_info = *pCreateInfo->pApplicationInfo;
         local_app_info.apiVersion = instance->renderer_version;
      }
      local_create_info.pApplicationInfo = &local_app_info;
   }

   VkInstance instance_handle = vn_instance_to_handle(instance);
   result =
      vn_call_vkCreateInstance(instance, pCreateInfo, NULL, &instance_handle);
   if (result != VK_SUCCESS)
      goto fail;

   driParseOptionInfo(&instance->available_dri_options, vn_dri_options,
                      ARRAY_SIZE(vn_dri_options));
   driParseConfigFiles(&instance->dri_options,
                       &instance->available_dri_options, 0, "venus", NULL,
                       instance->base.base.app_info.app_name,
                       instance->base.base.app_info.app_version,
                       instance->base.base.app_info.engine_name,
                       instance->base.base.app_info.engine_version);

   *pInstance = instance_handle;

   return VK_SUCCESS;

fail:
   if (instance->reply.bo)
      vn_renderer_bo_unref(instance->reply.bo);

   if (instance->ring.bo) {
      uint32_t destroy_ring_data[4];
      struct vn_cs_encoder local_enc = VN_CS_ENCODER_INITIALIZER(
         destroy_ring_data, sizeof(destroy_ring_data));
      vn_encode_vkDestroyRingMESA(&local_enc, 0, instance->ring.id);
      vn_renderer_submit_simple(instance->renderer, destroy_ring_data,
                                vn_cs_encoder_get_len(&local_enc));

      vn_cs_encoder_fini(&instance->ring.upload);
      vn_renderer_bo_unref(instance->ring.bo);
      vn_ring_fini(&instance->ring.ring);
      mtx_destroy(&instance->ring.mutex);
   }

   if (instance->renderer) {
      mtx_destroy(&instance->roundtrip_mutex);
      vn_renderer_destroy(instance->renderer, alloc);
   }

   mtx_destroy(&instance->physical_device_mutex);

   vn_instance_base_fini(&instance->base);
   vk_free(alloc, instance);

   return vn_error(NULL, result);
}

void
vn_DestroyInstance(VkInstance _instance,
                   const VkAllocationCallbacks *pAllocator)
{
   struct vn_instance *instance = vn_instance_from_handle(_instance);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &instance->base.base.alloc;

   if (!instance)
      return;

   if (instance->physical_devices) {
      for (uint32_t i = 0; i < instance->physical_device_count; i++)
         vn_physical_device_fini(&instance->physical_devices[i]);
      vk_free(alloc, instance->physical_devices);
   }

   vn_call_vkDestroyInstance(instance, _instance, NULL);

   vn_renderer_bo_unref(instance->reply.bo);

   uint32_t destroy_ring_data[4];
   struct vn_cs_encoder local_enc =
      VN_CS_ENCODER_INITIALIZER(destroy_ring_data, sizeof(destroy_ring_data));
   vn_encode_vkDestroyRingMESA(&local_enc, 0, instance->ring.id);
   vn_renderer_submit_simple(instance->renderer, destroy_ring_data,
                             vn_cs_encoder_get_len(&local_enc));

   vn_cs_encoder_fini(&instance->ring.upload);
   vn_ring_fini(&instance->ring.ring);
   mtx_destroy(&instance->ring.mutex);
   vn_renderer_bo_unref(instance->ring.bo);

   mtx_destroy(&instance->roundtrip_mutex);
   vn_renderer_destroy(instance->renderer, alloc);

   mtx_destroy(&instance->physical_device_mutex);

   driDestroyOptionCache(&instance->dri_options);
   driDestroyOptionInfo(&instance->available_dri_options);

   vn_instance_base_fini(&instance->base);
   vk_free(alloc, instance);
}

PFN_vkVoidFunction
vn_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   struct vn_instance *instance = vn_instance_from_handle(_instance);
   return vk_instance_get_proc_addr(&instance->base.base,
                                    &vn_instance_entrypoints, pName);
}

/* physical device commands */

VkResult
vn_EnumeratePhysicalDevices(VkInstance _instance,
                            uint32_t *pPhysicalDeviceCount,
                            VkPhysicalDevice *pPhysicalDevices)
{
   struct vn_instance *instance = vn_instance_from_handle(_instance);

   VkResult result = vn_instance_enumerate_physical_devices(instance);
   if (result != VK_SUCCESS)
      return vn_error(instance, result);

   VK_OUTARRAY_MAKE(out, pPhysicalDevices, pPhysicalDeviceCount);
   for (uint32_t i = 0; i < instance->physical_device_count; i++) {
      vk_outarray_append (&out, physical_dev) {
         *physical_dev =
            vn_physical_device_to_handle(&instance->physical_devices[i]);
      }
   }

   return vk_outarray_status(&out);
}

VkResult
vn_EnumeratePhysicalDeviceGroups(
   VkInstance _instance,
   uint32_t *pPhysicalDeviceGroupCount,
   VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties)
{
   struct vn_instance *instance = vn_instance_from_handle(_instance);
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;
   struct vn_physical_device_base *dummy = NULL;
   VkResult result;

   result = vn_instance_enumerate_physical_devices(instance);
   if (result != VK_SUCCESS)
      return vn_error(instance, result);

   /* make sure VkPhysicalDevice point to objects, as they are considered
    * inputs by the encoder
    */
   if (pPhysicalDeviceGroupProperties) {
      const uint32_t count = *pPhysicalDeviceGroupCount;
      const size_t size = sizeof(*dummy) * VK_MAX_DEVICE_GROUP_SIZE * count;

      dummy = vk_zalloc(alloc, size, VN_DEFAULT_ALIGN,
                        VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!dummy)
         return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

      for (uint32_t i = 0; i < count; i++) {
         VkPhysicalDeviceGroupProperties *props =
            &pPhysicalDeviceGroupProperties[i];

         for (uint32_t j = 0; j < VK_MAX_DEVICE_GROUP_SIZE; j++) {
            struct vn_physical_device_base *obj =
               &dummy[VK_MAX_DEVICE_GROUP_SIZE * i + j];
            obj->base.base.type = VK_OBJECT_TYPE_PHYSICAL_DEVICE;
            props->physicalDevices[j] = (VkPhysicalDevice)obj;
         }
      }
   }

   result = vn_call_vkEnumeratePhysicalDeviceGroups(
      instance, vn_instance_to_handle(instance), pPhysicalDeviceGroupCount,
      pPhysicalDeviceGroupProperties);
   if (result != VK_SUCCESS) {
      if (dummy)
         vk_free(alloc, dummy);
      return vn_error(instance, result);
   }

   if (pPhysicalDeviceGroupProperties) {
      for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; i++) {
         VkPhysicalDeviceGroupProperties *props =
            &pPhysicalDeviceGroupProperties[i];
         for (uint32_t j = 0; j < props->physicalDeviceCount; j++) {
            const vn_object_id id =
               dummy[VK_MAX_DEVICE_GROUP_SIZE * i + j].id;
            struct vn_physical_device *physical_dev =
               vn_instance_find_physical_device(instance, id);
            props->physicalDevices[j] =
               vn_physical_device_to_handle(physical_dev);
         }
      }
   }

   if (dummy)
      vk_free(alloc, dummy);

   return VK_SUCCESS;
}

void
vn_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                             VkPhysicalDeviceFeatures *pFeatures)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   *pFeatures = physical_dev->features.features;
}

void
vn_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceProperties *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   *pProperties = physical_dev->properties.properties;
}

void
vn_GetPhysicalDeviceQueueFamilyProperties(
   VkPhysicalDevice physicalDevice,
   uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties *pQueueFamilyProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);
   for (uint32_t i = 0; i < physical_dev->queue_family_count; i++) {
      vk_outarray_append (&out, props) {
         *props =
            physical_dev->queue_family_properties[i].queueFamilyProperties;
      }
   }
}

void
vn_GetPhysicalDeviceMemoryProperties(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   *pMemoryProperties = physical_dev->memory_properties.memoryProperties;
}

void
vn_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                     VkFormat format,
                                     VkFormatProperties *pFormatProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO query all formats during init */
   vn_call_vkGetPhysicalDeviceFormatProperties(
      physical_dev->instance, physicalDevice, format, pFormatProperties);
}

VkResult
vn_GetPhysicalDeviceImageFormatProperties(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkImageType type,
   VkImageTiling tiling,
   VkImageUsageFlags usage,
   VkImageCreateFlags flags,
   VkImageFormatProperties *pImageFormatProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO per-device cache */
   VkResult result = vn_call_vkGetPhysicalDeviceImageFormatProperties(
      physical_dev->instance, physicalDevice, format, type, tiling, usage,
      flags, pImageFormatProperties);

   return vn_result(physical_dev->instance, result);
}

void
vn_GetPhysicalDeviceSparseImageFormatProperties(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkImageType type,
   uint32_t samples,
   VkImageUsageFlags usage,
   VkImageTiling tiling,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO per-device cache */
   vn_call_vkGetPhysicalDeviceSparseImageFormatProperties(
      physical_dev->instance, physicalDevice, format, type, samples, usage,
      tiling, pPropertyCount, pProperties);
}

void
vn_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceFeatures2 *pFeatures)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   const struct VkPhysicalDeviceVulkan11Features *vk11_feats =
      &physical_dev->vulkan_1_1_features;
   const struct VkPhysicalDeviceVulkan12Features *vk12_feats =
      &physical_dev->vulkan_1_2_features;
   union {
      VkBaseOutStructure *pnext;

      /* Vulkan 1.1 */
      VkPhysicalDevice16BitStorageFeatures *sixteen_bit_storage;
      VkPhysicalDeviceMultiviewFeatures *multiview;
      VkPhysicalDeviceVariablePointersFeatures *variable_pointers;
      VkPhysicalDeviceProtectedMemoryFeatures *protected_memory;
      VkPhysicalDeviceSamplerYcbcrConversionFeatures *sampler_ycbcr_conversion;
      VkPhysicalDeviceShaderDrawParametersFeatures *shader_draw_parameters;

      /* Vulkan 1.2 */
      VkPhysicalDevice8BitStorageFeatures *eight_bit_storage;
      VkPhysicalDeviceShaderAtomicInt64Features *shader_atomic_int64;
      VkPhysicalDeviceShaderFloat16Int8Features *shader_float16_int8;
      VkPhysicalDeviceDescriptorIndexingFeatures *descriptor_indexing;
      VkPhysicalDeviceScalarBlockLayoutFeatures *scalar_block_layout;
      VkPhysicalDeviceImagelessFramebufferFeatures *imageless_framebuffer;
      VkPhysicalDeviceUniformBufferStandardLayoutFeatures
         *uniform_buffer_standard_layout;
      VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures
         *shader_subgroup_extended_types;
      VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures
         *separate_depth_stencil_layouts;
      VkPhysicalDeviceHostQueryResetFeatures *host_query_reset;
      VkPhysicalDeviceTimelineSemaphoreFeatures *timeline_semaphore;
      VkPhysicalDeviceBufferDeviceAddressFeatures *buffer_device_address;
      VkPhysicalDeviceVulkanMemoryModelFeatures *vulkan_memory_model;

      VkPhysicalDeviceTransformFeedbackFeaturesEXT *transform_feedback;
   } u;

   u.pnext = (VkBaseOutStructure *)pFeatures;
   while (u.pnext) {
      void *saved = u.pnext->pNext;
      switch (u.pnext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
         memcpy(u.pnext, &physical_dev->features,
                sizeof(physical_dev->features));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
         memcpy(u.pnext, vk11_feats, sizeof(*vk11_feats));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
         memcpy(u.pnext, vk12_feats, sizeof(*vk12_feats));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
         u.sixteen_bit_storage->storageBuffer16BitAccess =
            vk11_feats->storageBuffer16BitAccess;
         u.sixteen_bit_storage->uniformAndStorageBuffer16BitAccess =
            vk11_feats->uniformAndStorageBuffer16BitAccess;
         u.sixteen_bit_storage->storagePushConstant16 =
            vk11_feats->storagePushConstant16;
         u.sixteen_bit_storage->storageInputOutput16 =
            vk11_feats->storageInputOutput16;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
         u.multiview->multiview = vk11_feats->multiview;
         u.multiview->multiviewGeometryShader =
            vk11_feats->multiviewGeometryShader;
         u.multiview->multiviewTessellationShader =
            vk11_feats->multiviewTessellationShader;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
         u.variable_pointers->variablePointersStorageBuffer =
            vk11_feats->variablePointersStorageBuffer;
         u.variable_pointers->variablePointers = vk11_feats->variablePointers;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
         u.protected_memory->protectedMemory = vk11_feats->protectedMemory;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
         u.sampler_ycbcr_conversion->samplerYcbcrConversion =
            vk11_feats->samplerYcbcrConversion;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
         u.shader_draw_parameters->shaderDrawParameters =
            vk11_feats->shaderDrawParameters;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
         u.eight_bit_storage->storageBuffer8BitAccess =
            vk12_feats->storageBuffer8BitAccess;
         u.eight_bit_storage->uniformAndStorageBuffer8BitAccess =
            vk12_feats->uniformAndStorageBuffer8BitAccess;
         u.eight_bit_storage->storagePushConstant8 =
            vk12_feats->storagePushConstant8;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
         u.shader_atomic_int64->shaderBufferInt64Atomics =
            vk12_feats->shaderBufferInt64Atomics;
         u.shader_atomic_int64->shaderSharedInt64Atomics =
            vk12_feats->shaderSharedInt64Atomics;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
         u.shader_float16_int8->shaderFloat16 = vk12_feats->shaderFloat16;
         u.shader_float16_int8->shaderInt8 = vk12_feats->shaderInt8;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
         u.descriptor_indexing->shaderInputAttachmentArrayDynamicIndexing =
            vk12_feats->shaderInputAttachmentArrayDynamicIndexing;
         u.descriptor_indexing->shaderUniformTexelBufferArrayDynamicIndexing =
            vk12_feats->shaderUniformTexelBufferArrayDynamicIndexing;
         u.descriptor_indexing->shaderStorageTexelBufferArrayDynamicIndexing =
            vk12_feats->shaderStorageTexelBufferArrayDynamicIndexing;
         u.descriptor_indexing->shaderUniformBufferArrayNonUniformIndexing =
            vk12_feats->shaderUniformBufferArrayNonUniformIndexing;
         u.descriptor_indexing->shaderSampledImageArrayNonUniformIndexing =
            vk12_feats->shaderSampledImageArrayNonUniformIndexing;
         u.descriptor_indexing->shaderStorageBufferArrayNonUniformIndexing =
            vk12_feats->shaderStorageBufferArrayNonUniformIndexing;
         u.descriptor_indexing->shaderStorageImageArrayNonUniformIndexing =
            vk12_feats->shaderStorageImageArrayNonUniformIndexing;
         u.descriptor_indexing->shaderInputAttachmentArrayNonUniformIndexing =
            vk12_feats->shaderInputAttachmentArrayNonUniformIndexing;
         u.descriptor_indexing
            ->shaderUniformTexelBufferArrayNonUniformIndexing =
            vk12_feats->shaderUniformTexelBufferArrayNonUniformIndexing;
         u.descriptor_indexing
            ->shaderStorageTexelBufferArrayNonUniformIndexing =
            vk12_feats->shaderStorageTexelBufferArrayNonUniformIndexing;
         u.descriptor_indexing->descriptorBindingUniformBufferUpdateAfterBind =
            vk12_feats->descriptorBindingUniformBufferUpdateAfterBind;
         u.descriptor_indexing->descriptorBindingSampledImageUpdateAfterBind =
            vk12_feats->descriptorBindingSampledImageUpdateAfterBind;
         u.descriptor_indexing->descriptorBindingStorageImageUpdateAfterBind =
            vk12_feats->descriptorBindingStorageImageUpdateAfterBind;
         u.descriptor_indexing->descriptorBindingStorageBufferUpdateAfterBind =
            vk12_feats->descriptorBindingStorageBufferUpdateAfterBind;
         u.descriptor_indexing
            ->descriptorBindingUniformTexelBufferUpdateAfterBind =
            vk12_feats->descriptorBindingUniformTexelBufferUpdateAfterBind;
         u.descriptor_indexing
            ->descriptorBindingStorageTexelBufferUpdateAfterBind =
            vk12_feats->descriptorBindingStorageTexelBufferUpdateAfterBind;
         u.descriptor_indexing->descriptorBindingUpdateUnusedWhilePending =
            vk12_feats->descriptorBindingUpdateUnusedWhilePending;
         u.descriptor_indexing->descriptorBindingPartiallyBound =
            vk12_feats->descriptorBindingPartiallyBound;
         u.descriptor_indexing->descriptorBindingVariableDescriptorCount =
            vk12_feats->descriptorBindingVariableDescriptorCount;
         u.descriptor_indexing->runtimeDescriptorArray =
            vk12_feats->runtimeDescriptorArray;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
         u.scalar_block_layout->scalarBlockLayout =
            vk12_feats->scalarBlockLayout;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
         u.imageless_framebuffer->imagelessFramebuffer =
            vk12_feats->imagelessFramebuffer;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
         u.uniform_buffer_standard_layout->uniformBufferStandardLayout =
            vk12_feats->uniformBufferStandardLayout;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
         u.shader_subgroup_extended_types->shaderSubgroupExtendedTypes =
            vk12_feats->shaderSubgroupExtendedTypes;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
         u.separate_depth_stencil_layouts->separateDepthStencilLayouts =
            vk12_feats->separateDepthStencilLayouts;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
         u.host_query_reset->hostQueryReset = vk12_feats->hostQueryReset;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
         u.timeline_semaphore->timelineSemaphore =
            vk12_feats->timelineSemaphore;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
         u.buffer_device_address->bufferDeviceAddress =
            vk12_feats->bufferDeviceAddress;
         u.buffer_device_address->bufferDeviceAddressCaptureReplay =
            vk12_feats->bufferDeviceAddressCaptureReplay;
         u.buffer_device_address->bufferDeviceAddressMultiDevice =
            vk12_feats->bufferDeviceAddressMultiDevice;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
         u.vulkan_memory_model->vulkanMemoryModel =
            vk12_feats->vulkanMemoryModel;
         u.vulkan_memory_model->vulkanMemoryModelDeviceScope =
            vk12_feats->vulkanMemoryModelDeviceScope;
         u.vulkan_memory_model->vulkanMemoryModelAvailabilityVisibilityChains =
            vk12_feats->vulkanMemoryModelAvailabilityVisibilityChains;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
         memcpy(u.transform_feedback,
                &physical_dev->transform_feedback_features,
                sizeof(physical_dev->transform_feedback_features));
         break;
      default:
         break;
      }
      u.pnext->pNext = saved;

      u.pnext = u.pnext->pNext;
   }
}

void
vn_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceProperties2 *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   const struct VkPhysicalDeviceVulkan11Properties *vk11_props =
      &physical_dev->vulkan_1_1_properties;
   const struct VkPhysicalDeviceVulkan12Properties *vk12_props =
      &physical_dev->vulkan_1_2_properties;
   union {
      VkBaseOutStructure *pnext;

      /* Vulkan 1.1 */
      VkPhysicalDeviceIDProperties *id;
      VkPhysicalDeviceSubgroupProperties *subgroup;
      VkPhysicalDevicePointClippingProperties *point_clipping;
      VkPhysicalDeviceMultiviewProperties *multiview;
      VkPhysicalDeviceProtectedMemoryProperties *protected_memory;
      VkPhysicalDeviceMaintenance3Properties *maintenance_3;

      /* Vulkan 1.2 */
      VkPhysicalDeviceDriverProperties *driver;
      VkPhysicalDeviceFloatControlsProperties *float_controls;
      VkPhysicalDeviceDescriptorIndexingProperties *descriptor_indexing;
      VkPhysicalDeviceDepthStencilResolveProperties *depth_stencil_resolve;
      VkPhysicalDeviceSamplerFilterMinmaxProperties *sampler_filter_minmax;
      VkPhysicalDeviceTimelineSemaphoreProperties *timeline_semaphore;

      VkPhysicalDevicePCIBusInfoPropertiesEXT *pci_bus_info;
      VkPhysicalDeviceTransformFeedbackPropertiesEXT *transform_feedback;
   } u;

   u.pnext = (VkBaseOutStructure *)pProperties;
   while (u.pnext) {
      void *saved = u.pnext->pNext;
      switch (u.pnext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2:
         memcpy(u.pnext, &physical_dev->properties,
                sizeof(physical_dev->properties));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
         memcpy(u.pnext, vk11_props, sizeof(*vk11_props));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
         memcpy(u.pnext, vk12_props, sizeof(*vk12_props));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES:
         memcpy(u.id->deviceUUID, vk11_props->deviceUUID,
                sizeof(vk11_props->deviceUUID));
         memcpy(u.id->driverUUID, vk11_props->driverUUID,
                sizeof(vk11_props->driverUUID));
         memcpy(u.id->deviceLUID, vk11_props->deviceLUID,
                sizeof(vk11_props->deviceLUID));
         u.id->deviceNodeMask = vk11_props->deviceNodeMask;
         u.id->deviceLUIDValid = vk11_props->deviceLUIDValid;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
         u.subgroup->subgroupSize = vk11_props->subgroupSize;
         u.subgroup->supportedStages = vk11_props->subgroupSupportedStages;
         u.subgroup->supportedOperations =
            vk11_props->subgroupSupportedOperations;
         u.subgroup->quadOperationsInAllStages =
            vk11_props->subgroupQuadOperationsInAllStages;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
         u.point_clipping->pointClippingBehavior =
            vk11_props->pointClippingBehavior;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES:
         u.multiview->maxMultiviewViewCount =
            vk11_props->maxMultiviewViewCount;
         u.multiview->maxMultiviewInstanceIndex =
            vk11_props->maxMultiviewInstanceIndex;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:
         u.protected_memory->protectedNoFault = vk11_props->protectedNoFault;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:
         u.maintenance_3->maxPerSetDescriptors =
            vk11_props->maxPerSetDescriptors;
         u.maintenance_3->maxMemoryAllocationSize =
            vk11_props->maxMemoryAllocationSize;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES:
         u.driver->driverID = vk12_props->driverID;
         memcpy(u.driver->driverName, vk12_props->driverName,
                sizeof(vk12_props->driverName));
         memcpy(u.driver->driverInfo, vk12_props->driverInfo,
                sizeof(vk12_props->driverInfo));
         u.driver->conformanceVersion = vk12_props->conformanceVersion;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES:
         u.float_controls->denormBehaviorIndependence =
            vk12_props->denormBehaviorIndependence;
         u.float_controls->roundingModeIndependence =
            vk12_props->roundingModeIndependence;
         u.float_controls->shaderSignedZeroInfNanPreserveFloat16 =
            vk12_props->shaderSignedZeroInfNanPreserveFloat16;
         u.float_controls->shaderSignedZeroInfNanPreserveFloat32 =
            vk12_props->shaderSignedZeroInfNanPreserveFloat32;
         u.float_controls->shaderSignedZeroInfNanPreserveFloat64 =
            vk12_props->shaderSignedZeroInfNanPreserveFloat64;
         u.float_controls->shaderDenormPreserveFloat16 =
            vk12_props->shaderDenormPreserveFloat16;
         u.float_controls->shaderDenormPreserveFloat32 =
            vk12_props->shaderDenormPreserveFloat32;
         u.float_controls->shaderDenormPreserveFloat64 =
            vk12_props->shaderDenormPreserveFloat64;
         u.float_controls->shaderDenormFlushToZeroFloat16 =
            vk12_props->shaderDenormFlushToZeroFloat16;
         u.float_controls->shaderDenormFlushToZeroFloat32 =
            vk12_props->shaderDenormFlushToZeroFloat32;
         u.float_controls->shaderDenormFlushToZeroFloat64 =
            vk12_props->shaderDenormFlushToZeroFloat64;
         u.float_controls->shaderRoundingModeRTEFloat16 =
            vk12_props->shaderRoundingModeRTEFloat16;
         u.float_controls->shaderRoundingModeRTEFloat32 =
            vk12_props->shaderRoundingModeRTEFloat32;
         u.float_controls->shaderRoundingModeRTEFloat64 =
            vk12_props->shaderRoundingModeRTEFloat64;
         u.float_controls->shaderRoundingModeRTZFloat16 =
            vk12_props->shaderRoundingModeRTZFloat16;
         u.float_controls->shaderRoundingModeRTZFloat32 =
            vk12_props->shaderRoundingModeRTZFloat32;
         u.float_controls->shaderRoundingModeRTZFloat64 =
            vk12_props->shaderRoundingModeRTZFloat64;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
         u.descriptor_indexing->maxUpdateAfterBindDescriptorsInAllPools =
            vk12_props->maxUpdateAfterBindDescriptorsInAllPools;
         u.descriptor_indexing
            ->shaderUniformBufferArrayNonUniformIndexingNative =
            vk12_props->shaderUniformBufferArrayNonUniformIndexingNative;
         u.descriptor_indexing
            ->shaderSampledImageArrayNonUniformIndexingNative =
            vk12_props->shaderSampledImageArrayNonUniformIndexingNative;
         u.descriptor_indexing
            ->shaderStorageBufferArrayNonUniformIndexingNative =
            vk12_props->shaderStorageBufferArrayNonUniformIndexingNative;
         u.descriptor_indexing
            ->shaderStorageImageArrayNonUniformIndexingNative =
            vk12_props->shaderStorageImageArrayNonUniformIndexingNative;
         u.descriptor_indexing
            ->shaderInputAttachmentArrayNonUniformIndexingNative =
            vk12_props->shaderInputAttachmentArrayNonUniformIndexingNative;
         u.descriptor_indexing->robustBufferAccessUpdateAfterBind =
            vk12_props->robustBufferAccessUpdateAfterBind;
         u.descriptor_indexing->quadDivergentImplicitLod =
            vk12_props->quadDivergentImplicitLod;
         u.descriptor_indexing->maxPerStageDescriptorUpdateAfterBindSamplers =
            vk12_props->maxPerStageDescriptorUpdateAfterBindSamplers;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindUniformBuffers =
            vk12_props->maxPerStageDescriptorUpdateAfterBindUniformBuffers;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindStorageBuffers =
            vk12_props->maxPerStageDescriptorUpdateAfterBindStorageBuffers;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindSampledImages =
            vk12_props->maxPerStageDescriptorUpdateAfterBindSampledImages;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindStorageImages =
            vk12_props->maxPerStageDescriptorUpdateAfterBindStorageImages;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindInputAttachments =
            vk12_props->maxPerStageDescriptorUpdateAfterBindInputAttachments;
         u.descriptor_indexing->maxPerStageUpdateAfterBindResources =
            vk12_props->maxPerStageUpdateAfterBindResources;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindSamplers =
            vk12_props->maxDescriptorSetUpdateAfterBindSamplers;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindUniformBuffers =
            vk12_props->maxDescriptorSetUpdateAfterBindUniformBuffers;
         u.descriptor_indexing
            ->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic =
            vk12_props->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindStorageBuffers =
            vk12_props->maxDescriptorSetUpdateAfterBindStorageBuffers;
         u.descriptor_indexing
            ->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic =
            vk12_props->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindSampledImages =
            vk12_props->maxDescriptorSetUpdateAfterBindSampledImages;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindStorageImages =
            vk12_props->maxDescriptorSetUpdateAfterBindStorageImages;
         u.descriptor_indexing
            ->maxDescriptorSetUpdateAfterBindInputAttachments =
            vk12_props->maxDescriptorSetUpdateAfterBindInputAttachments;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:
         u.depth_stencil_resolve->supportedDepthResolveModes =
            vk12_props->supportedDepthResolveModes;
         u.depth_stencil_resolve->supportedStencilResolveModes =
            vk12_props->supportedStencilResolveModes;
         u.depth_stencil_resolve->independentResolveNone =
            vk12_props->independentResolveNone;
         u.depth_stencil_resolve->independentResolve =
            vk12_props->independentResolve;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES:
         u.sampler_filter_minmax->filterMinmaxSingleComponentFormats =
            vk12_props->filterMinmaxSingleComponentFormats;
         u.sampler_filter_minmax->filterMinmaxImageComponentMapping =
            vk12_props->filterMinmaxImageComponentMapping;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
         u.timeline_semaphore->maxTimelineSemaphoreValueDifference =
            vk12_props->maxTimelineSemaphoreValueDifference;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT:
         /* this is used by WSI */
         if (physical_dev->instance->renderer_info.pci.has_bus_info) {
            u.pci_bus_info->pciDomain =
               physical_dev->instance->renderer_info.pci.domain;
            u.pci_bus_info->pciBus =
               physical_dev->instance->renderer_info.pci.bus;
            u.pci_bus_info->pciDevice =
               physical_dev->instance->renderer_info.pci.device;
            u.pci_bus_info->pciFunction =
               physical_dev->instance->renderer_info.pci.function;
         }
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT:
         memcpy(u.transform_feedback,
                &physical_dev->transform_feedback_properties,
                sizeof(physical_dev->transform_feedback_properties));
         break;
      default:
         break;
      }
      u.pnext->pNext = saved;

      u.pnext = u.pnext->pNext;
   }
}

void
vn_GetPhysicalDeviceQueueFamilyProperties2(
   VkPhysicalDevice physicalDevice,
   uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);
   for (uint32_t i = 0; i < physical_dev->queue_family_count; i++) {
      vk_outarray_append (&out, props) {
         *props = physical_dev->queue_family_properties[i];
      }
   }
}

void
vn_GetPhysicalDeviceMemoryProperties2(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   pMemoryProperties->memoryProperties =
      physical_dev->memory_properties.memoryProperties;
}

void
vn_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                      VkFormat format,
                                      VkFormatProperties2 *pFormatProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO query all formats during init */
   vn_call_vkGetPhysicalDeviceFormatProperties2(
      physical_dev->instance, physicalDevice, format, pFormatProperties);
}

VkResult
vn_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
   VkImageFormatProperties2 *pImageFormatProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   const VkPhysicalDeviceExternalImageFormatInfo *external_info =
      vk_find_struct_const(pImageFormatInfo->pNext,
                           PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   if (external_info && !external_info->handleType)
      external_info = NULL;

   if (external_info &&
       !(external_info->handleType & physical_dev->external_memory_handles))
      return vn_error(physical_dev->instance, VK_ERROR_FORMAT_NOT_SUPPORTED);

   VkResult result;
   /* TODO per-device cache */
   result = vn_call_vkGetPhysicalDeviceImageFormatProperties2(
      physical_dev->instance, physicalDevice, pImageFormatInfo,
      pImageFormatProperties);

   if (result == VK_SUCCESS && external_info) {
      VkExternalImageFormatProperties *img_props = vk_find_struct(
         pImageFormatProperties->pNext, EXTERNAL_IMAGE_FORMAT_PROPERTIES);
      VkExternalMemoryProperties *mem_props =
         &img_props->externalMemoryProperties;

      mem_props->compatibleHandleTypes &=
         physical_dev->external_memory_handles;
      mem_props->exportFromImportedHandleTypes &=
         physical_dev->external_memory_handles;
   }

   return vn_result(physical_dev->instance, result);
}

void
vn_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties2 *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO per-device cache */
   vn_call_vkGetPhysicalDeviceSparseImageFormatProperties2(
      physical_dev->instance, physicalDevice, pFormatInfo, pPropertyCount,
      pProperties);
}

void
vn_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   VkExternalMemoryProperties *props =
      &pExternalBufferProperties->externalMemoryProperties;

   if (!(pExternalBufferInfo->handleType &
         physical_dev->external_memory_handles)) {
      props->compatibleHandleTypes = pExternalBufferInfo->handleType;
      props->exportFromImportedHandleTypes = 0;
      props->externalMemoryFeatures = 0;
      return;
   }

   /* TODO per-device cache */
   vn_call_vkGetPhysicalDeviceExternalBufferProperties(
      physical_dev->instance, physicalDevice, pExternalBufferInfo,
      pExternalBufferProperties);

   props->compatibleHandleTypes &= physical_dev->external_memory_handles;
   props->exportFromImportedHandleTypes &=
      physical_dev->external_memory_handles;
}

void
vn_GetPhysicalDeviceExternalFenceProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
   VkExternalFenceProperties *pExternalFenceProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   if (pExternalFenceInfo->handleType &
       physical_dev->external_fence_handles) {
      pExternalFenceProperties->compatibleHandleTypes =
         physical_dev->external_fence_handles;
      pExternalFenceProperties->exportFromImportedHandleTypes =
         physical_dev->external_fence_handles;
      pExternalFenceProperties->externalFenceFeatures =
         VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
   } else {
      pExternalFenceProperties->compatibleHandleTypes =
         pExternalFenceInfo->handleType;
      pExternalFenceProperties->exportFromImportedHandleTypes = 0;
      pExternalFenceProperties->externalFenceFeatures = 0;
   }
}

void
vn_GetPhysicalDeviceExternalSemaphoreProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
   VkExternalSemaphoreProperties *pExternalSemaphoreProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   const VkSemaphoreTypeCreateInfoKHR *type_info = vk_find_struct_const(
      pExternalSemaphoreInfo->pNext, SEMAPHORE_TYPE_CREATE_INFO_KHR);
   const VkSemaphoreType sem_type =
      type_info ? type_info->semaphoreType : VK_SEMAPHORE_TYPE_BINARY;
   const VkExternalSemaphoreHandleTypeFlags valid_handles =
      sem_type == VK_SEMAPHORE_TYPE_BINARY
         ? physical_dev->external_binary_semaphore_handles
         : physical_dev->external_timeline_semaphore_handles;
   if (pExternalSemaphoreInfo->handleType & valid_handles) {
      pExternalSemaphoreProperties->compatibleHandleTypes = valid_handles;
      pExternalSemaphoreProperties->exportFromImportedHandleTypes =
         valid_handles;
      pExternalSemaphoreProperties->externalSemaphoreFeatures =
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
   } else {
      pExternalSemaphoreProperties->compatibleHandleTypes =
         pExternalSemaphoreInfo->handleType;
      pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
      pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
   }
}

/* device commands */

VkResult
vn_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                      const char *pLayerName,
                                      uint32_t *pPropertyCount,
                                      VkExtensionProperties *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   if (pLayerName)
      return vn_error(physical_dev->instance, VK_ERROR_LAYER_NOT_PRESENT);

   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);
   for (uint32_t i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
      if (physical_dev->base.base.supported_extensions.extensions[i]) {
         vk_outarray_append (&out, prop) {
            *prop = vk_device_extensions[i];
            prop->specVersion = physical_dev->extension_spec_versions[i];
         }
      }
   }

   return vk_outarray_status(&out);
}

VkResult
vn_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                  uint32_t *pPropertyCount,
                                  VkLayerProperties *pProperties)
{
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

static VkResult
vn_queue_init(struct vn_device *dev,
              struct vn_queue *queue,
              const VkDeviceQueueCreateInfo *queue_info,
              uint32_t queue_index,
              uint32_t sync_queue_index)
{
   vn_object_base_init(&queue->base, VK_OBJECT_TYPE_QUEUE, &dev->base);

   VkQueue queue_handle = vn_queue_to_handle(queue);
   vn_async_vkGetDeviceQueue2(
      dev->instance, vn_device_to_handle(dev),
      &(VkDeviceQueueInfo2){
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
         .flags = queue_info->flags,
         .queueFamilyIndex = queue_info->queueFamilyIndex,
         .queueIndex = queue_index,
      },
      &queue_handle);

   queue->device = dev;
   queue->family = queue_info->queueFamilyIndex;
   queue->index = queue_index;
   queue->flags = queue_info->flags;

   queue->sync_queue_index = sync_queue_index;

   VkResult result =
      vn_renderer_sync_create_cpu(dev->instance->renderer, &queue->idle_sync);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

static VkResult
vn_device_init_queues(struct vn_device *dev,
                      const VkDeviceCreateInfo *create_info)
{
   struct vn_physical_device *physical_dev = dev->physical_device;
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   uint32_t count = 0;
   for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++)
      count += create_info->pQueueCreateInfos[i].queueCount;

   struct vn_queue *queues =
      vk_zalloc(alloc, sizeof(*queues) * count, VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!queues)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = VK_SUCCESS;
   count = 0;
   for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queue_info =
         &create_info->pQueueCreateInfos[i];
      const uint32_t sync_queue_base =
         physical_dev
            ->queue_family_sync_queue_bases[queue_info->queueFamilyIndex];

      for (uint32_t j = 0; j < queue_info->queueCount; j++) {
         result = vn_queue_init(dev, &queues[count], queue_info, j,
                                sync_queue_base + j);
         if (result != VK_SUCCESS)
            break;

         count++;
      }
   }

   if (result != VK_SUCCESS) {
      for (uint32_t i = 0; i < count; i++)
         vn_renderer_sync_destroy(queues[i].idle_sync);
      vk_free(alloc, queues);

      return result;
   }

   dev->queues = queues;
   dev->queue_count = count;

   return VK_SUCCESS;
}

static bool
find_extension_names(const char *const *exts,
                     uint32_t ext_count,
                     const char *name)
{
   for (uint32_t i = 0; i < ext_count; i++) {
      if (!strcmp(exts[i], name))
         return true;
   }
   return false;
}

static const char **
merge_extension_names(const char *const *exts,
                      uint32_t ext_count,
                      const char *const *extra_exts,
                      uint32_t extra_count,
                      const VkAllocationCallbacks *alloc,
                      uint32_t *merged_count)
{
   const char **merged =
      vk_alloc(alloc, sizeof(*merged) * (ext_count + extra_count),
               VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!merged)
      return NULL;

   memcpy(merged, exts, sizeof(*exts) * ext_count);

   uint32_t count = ext_count;
   for (uint32_t i = 0; i < extra_count; i++) {
      if (!find_extension_names(exts, ext_count, extra_exts[i]))
         merged[count++] = extra_exts[i];
   }

   *merged_count = count;
   return merged;
}

static const VkDeviceCreateInfo *
vn_device_fix_create_info(const struct vn_physical_device *physical_dev,
                          const VkDeviceCreateInfo *dev_info,
                          const VkAllocationCallbacks *alloc,
                          VkDeviceCreateInfo *local_info)
{
   const char *extra_exts[8];
   uint32_t extra_count = 0;

   if (physical_dev->wsi_device.supports_modifiers)
      extra_exts[extra_count++] = "VK_EXT_image_drm_format_modifier";

   if (!extra_count)
      return dev_info;

   *local_info = *dev_info;
   local_info->ppEnabledExtensionNames = merge_extension_names(
      dev_info->ppEnabledExtensionNames, dev_info->enabledExtensionCount,
      extra_exts, extra_count, alloc, &local_info->enabledExtensionCount);
   if (!local_info->ppEnabledExtensionNames)
      return NULL;

   return local_info;
}

VkResult
vn_CreateDevice(VkPhysicalDevice physicalDevice,
                const VkDeviceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkDevice *pDevice)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &instance->base.base.alloc;
   struct vn_device *dev;
   VkResult result;

   dev = vk_zalloc(alloc, sizeof(*dev), VN_DEFAULT_ALIGN,
                   VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!dev)
      return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &vn_device_entrypoints, true);
   result = vn_device_base_init(&dev->base, &physical_dev->base,
                                &dispatch_table, pCreateInfo, alloc);
   if (result != VK_SUCCESS) {
      vk_free(alloc, dev);
      return vn_error(instance, result);
   }

   dev->instance = instance;
   dev->physical_device = physical_dev;

   VkDeviceCreateInfo local_create_info;
   pCreateInfo = vn_device_fix_create_info(physical_dev, pCreateInfo, alloc,
                                           &local_create_info);
   if (!pCreateInfo) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   VkDevice dev_handle = vn_device_to_handle(dev);
   result = vn_call_vkCreateDevice(instance, physicalDevice, pCreateInfo,
                                   NULL, &dev_handle);
   if (result != VK_SUCCESS)
      goto fail;

   result = vn_device_init_queues(dev, pCreateInfo);
   if (result != VK_SUCCESS) {
      vn_call_vkDestroyDevice(instance, dev_handle, NULL);
      goto fail;
   }

   for (uint32_t i = 0; i < ARRAY_SIZE(dev->memory_pools); i++) {
      struct vn_device_memory_pool *pool = &dev->memory_pools[i];
      mtx_init(&pool->mutex, mtx_plain);
   }

   *pDevice = dev_handle;

   if (pCreateInfo == &local_create_info)
      vk_free(alloc, (void *)pCreateInfo->ppEnabledExtensionNames);

   return VK_SUCCESS;

fail:
   if (pCreateInfo == &local_create_info)
      vk_free(alloc, (void *)pCreateInfo->ppEnabledExtensionNames);
   vn_device_base_fini(&dev->base);
   vk_free(alloc, dev);
   return vn_error(instance, result);
}

static void
vn_device_memory_pool_fini(struct vn_device *dev, uint32_t mem_type_index);

void
vn_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!dev)
      return;

   for (uint32_t i = 0; i < ARRAY_SIZE(dev->memory_pools); i++)
      vn_device_memory_pool_fini(dev, i);

   vn_async_vkDestroyDevice(dev->instance, device, NULL);

   for (uint32_t i = 0; i < dev->queue_count; i++) {
      struct vn_queue *queue = &dev->queues[i];
      vn_renderer_sync_destroy(queue->idle_sync);
      vn_object_base_fini(&queue->base);
   }
   vk_free(alloc, dev->queues);

   vn_device_base_fini(&dev->base);
   vk_free(alloc, dev);
}

PFN_vkVoidFunction
vn_GetDeviceProcAddr(VkDevice device, const char *pName)
{
   struct vn_device *dev = vn_device_from_handle(device);
   return vk_device_get_proc_addr(&dev->base.base, pName);
}

void
vn_GetDeviceGroupPeerMemoryFeatures(
   VkDevice device,
   uint32_t heapIndex,
   uint32_t localDeviceIndex,
   uint32_t remoteDeviceIndex,
   VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO get and cache the values in vkCreateDevice */
   vn_call_vkGetDeviceGroupPeerMemoryFeatures(
      dev->instance, device, heapIndex, localDeviceIndex, remoteDeviceIndex,
      pPeerMemoryFeatures);
}

VkResult
vn_DeviceWaitIdle(VkDevice device)
{
   struct vn_device *dev = vn_device_from_handle(device);

   for (uint32_t i = 0; i < dev->queue_count; i++) {
      struct vn_queue *queue = &dev->queues[i];
      VkResult result = vn_QueueWaitIdle(vn_queue_to_handle(queue));
      if (result != VK_SUCCESS)
         return vn_error(dev->instance, result);
   }

   return VK_SUCCESS;
}

/* queue commands */

void
vn_GetDeviceQueue(VkDevice device,
                  uint32_t queueFamilyIndex,
                  uint32_t queueIndex,
                  VkQueue *pQueue)
{
   struct vn_device *dev = vn_device_from_handle(device);

   for (uint32_t i = 0; i < dev->queue_count; i++) {
      struct vn_queue *queue = &dev->queues[i];
      if (queue->family == queueFamilyIndex && queue->index == queueIndex) {
         assert(!queue->flags);
         *pQueue = vn_queue_to_handle(queue);
         return;
      }
   }
   unreachable("bad queue family/index");
}

void
vn_GetDeviceQueue2(VkDevice device,
                   const VkDeviceQueueInfo2 *pQueueInfo,
                   VkQueue *pQueue)
{
   struct vn_device *dev = vn_device_from_handle(device);

   for (uint32_t i = 0; i < dev->queue_count; i++) {
      struct vn_queue *queue = &dev->queues[i];
      if (queue->family == pQueueInfo->queueFamilyIndex &&
          queue->index == pQueueInfo->queueIndex &&
          queue->flags == pQueueInfo->flags) {
         *pQueue = vn_queue_to_handle(queue);
         return;
      }
   }
   unreachable("bad queue family/index");
}

static void
vn_semaphore_reset_wsi(struct vn_device *dev, struct vn_semaphore *sem);

struct vn_queue_submission {
   VkStructureType batch_type;
   VkQueue queue;
   uint32_t batch_count;
   union {
      const void *batches;
      const VkSubmitInfo *submit_batches;
      const VkBindSparseInfo *bind_sparse_batches;
   };
   VkFence fence;

   uint32_t wait_semaphore_count;
   uint32_t wait_wsi_count;
   uint32_t signal_semaphore_count;
   uint32_t signal_device_only_count;
   uint32_t signal_timeline_count;

   uint32_t sync_count;

   struct {
      void *storage;

      union {
         void *batches;
         VkSubmitInfo *submit_batches;
         VkBindSparseInfo *bind_sparse_batches;
      };
      VkSemaphore *semaphores;

      struct vn_renderer_sync **syncs;
      uint64_t *sync_values;

      uint32_t *batch_sync_counts;
   } temp;
};

static void
vn_queue_submission_count_semaphores(struct vn_queue_submission *submit)
{
   submit->wait_semaphore_count = 0;
   submit->wait_wsi_count = 0;
   submit->signal_semaphore_count = 0;
   submit->signal_device_only_count = 0;
   submit->signal_timeline_count = 0;
   switch (submit->batch_type) {
   case VK_STRUCTURE_TYPE_SUBMIT_INFO:
      for (uint32_t i = 0; i < submit->batch_count; i++) {
         const VkSubmitInfo *batch = &submit->submit_batches[i];

         submit->wait_semaphore_count += batch->waitSemaphoreCount;
         submit->signal_semaphore_count += batch->signalSemaphoreCount;

         for (uint32_t j = 0; j < batch->waitSemaphoreCount; j++) {
            struct vn_semaphore *sem =
               vn_semaphore_from_handle(batch->pWaitSemaphores[j]);
            const struct vn_sync_payload *payload = sem->payload;

            if (payload->type == VN_SYNC_TYPE_WSI_SIGNALED)
               submit->wait_wsi_count++;
         }

         for (uint32_t j = 0; j < batch->signalSemaphoreCount; j++) {
            struct vn_semaphore *sem =
               vn_semaphore_from_handle(batch->pSignalSemaphores[j]);
            const struct vn_sync_payload *payload = sem->payload;

            /* it must be one of the waited semaphores and will be reset */
            if (payload->type == VN_SYNC_TYPE_WSI_SIGNALED)
               payload = &sem->permanent;

            if (payload->type == VN_SYNC_TYPE_DEVICE_ONLY)
               submit->signal_device_only_count++;
            else if (sem->type == VK_SEMAPHORE_TYPE_TIMELINE)
               submit->signal_timeline_count++;
         }
      }
      break;
   case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
      for (uint32_t i = 0; i < submit->batch_count; i++) {
         const VkBindSparseInfo *batch = &submit->bind_sparse_batches[i];

         submit->wait_semaphore_count += batch->waitSemaphoreCount;
         submit->signal_semaphore_count += batch->signalSemaphoreCount;

         for (uint32_t j = 0; j < batch->waitSemaphoreCount; j++) {
            struct vn_semaphore *sem =
               vn_semaphore_from_handle(batch->pWaitSemaphores[j]);
            const struct vn_sync_payload *payload = sem->payload;

            if (payload->type == VN_SYNC_TYPE_WSI_SIGNALED)
               submit->wait_wsi_count++;
         }

         for (uint32_t j = 0; j < batch->signalSemaphoreCount; j++) {
            struct vn_semaphore *sem =
               vn_semaphore_from_handle(batch->pSignalSemaphores[j]);
            const struct vn_sync_payload *payload = sem->payload;

            if (payload->type == VN_SYNC_TYPE_DEVICE_ONLY)
               submit->signal_device_only_count++;
            else if (sem->type == VK_SEMAPHORE_TYPE_TIMELINE)
               submit->signal_timeline_count++;
         }
      }
      break;
   default:
      unreachable("unexpected batch type");
      break;
   }

   submit->sync_count =
      submit->signal_semaphore_count - submit->signal_device_only_count;
   if (submit->fence != VK_NULL_HANDLE)
      submit->sync_count++;
}

static VkResult
vn_queue_submission_alloc_storage(struct vn_queue_submission *submit)
{
   struct vn_queue *queue = vn_queue_from_handle(submit->queue);
   const VkAllocationCallbacks *alloc = &queue->device->base.base.alloc;
   size_t alloc_size = 0;
   size_t semaphores_offset = 0;
   size_t syncs_offset = 0;
   size_t sync_values_offset = 0;
   size_t batch_sync_counts_offset = 0;

   /* we want to filter out VN_SYNC_TYPE_WSI_SIGNALED wait semaphores */
   if (submit->wait_wsi_count) {
      switch (submit->batch_type) {
      case VK_STRUCTURE_TYPE_SUBMIT_INFO:
         alloc_size += sizeof(VkSubmitInfo) * submit->batch_count;
         break;
      case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
         alloc_size += sizeof(VkBindSparseInfo) * submit->batch_count;
         break;
      default:
         unreachable("unexpected batch type");
         break;
      }

      semaphores_offset = alloc_size;
      alloc_size += sizeof(*submit->temp.semaphores) *
                    (submit->wait_semaphore_count - submit->wait_wsi_count);
   }

   if (submit->sync_count) {
      syncs_offset = alloc_size;
      alloc_size += sizeof(*submit->temp.syncs) * submit->sync_count;

      alloc_size = (alloc_size + 7) & ~7;
      sync_values_offset = alloc_size;
      alloc_size += sizeof(*submit->temp.sync_values) * submit->sync_count;

      batch_sync_counts_offset = alloc_size;
      alloc_size +=
         sizeof(*submit->temp.batch_sync_counts) * submit->batch_count;
   }

   if (!alloc_size) {
      submit->temp.storage = NULL;
      return VK_SUCCESS;
   }

   submit->temp.storage = vk_alloc(alloc, alloc_size, VN_DEFAULT_ALIGN,
                                   VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!submit->temp.storage)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   submit->temp.batches = submit->temp.storage;
   submit->temp.semaphores = submit->temp.storage + semaphores_offset;

   submit->temp.syncs = submit->temp.storage + syncs_offset;
   submit->temp.sync_values = submit->temp.storage + sync_values_offset;
   submit->temp.batch_sync_counts =
      submit->temp.storage + batch_sync_counts_offset;

   return VK_SUCCESS;
}

static uint32_t
vn_queue_submission_filter_batch_wsi_semaphores(
   struct vn_queue_submission *submit,
   uint32_t batch_index,
   uint32_t sem_base)
{
   struct vn_queue *queue = vn_queue_from_handle(submit->queue);

   union {
      VkSubmitInfo *submit_batch;
      VkBindSparseInfo *bind_sparse_batch;
   } u;
   const VkSemaphore *src_sems;
   uint32_t src_count;
   switch (submit->batch_type) {
   case VK_STRUCTURE_TYPE_SUBMIT_INFO:
      u.submit_batch = &submit->temp.submit_batches[batch_index];
      src_sems = u.submit_batch->pWaitSemaphores;
      src_count = u.submit_batch->waitSemaphoreCount;
      break;
   case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
      u.bind_sparse_batch = &submit->temp.bind_sparse_batches[batch_index];
      src_sems = u.bind_sparse_batch->pWaitSemaphores;
      src_count = u.bind_sparse_batch->waitSemaphoreCount;
      break;
   default:
      unreachable("unexpected batch type");
      break;
   }

   VkSemaphore *dst_sems = &submit->temp.semaphores[sem_base];
   uint32_t dst_count = 0;

   /* filter out VN_SYNC_TYPE_WSI_SIGNALED wait semaphores */
   for (uint32_t i = 0; i < src_count; i++) {
      struct vn_semaphore *sem = vn_semaphore_from_handle(src_sems[i]);
      const struct vn_sync_payload *payload = sem->payload;

      if (payload->type == VN_SYNC_TYPE_WSI_SIGNALED)
         vn_semaphore_reset_wsi(queue->device, sem);
      else
         dst_sems[dst_count++] = src_sems[i];
   }

   switch (submit->batch_type) {
   case VK_STRUCTURE_TYPE_SUBMIT_INFO:
      u.submit_batch->pWaitSemaphores = dst_sems;
      u.submit_batch->waitSemaphoreCount = dst_count;
      break;
   case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
      u.bind_sparse_batch->pWaitSemaphores = dst_sems;
      u.bind_sparse_batch->waitSemaphoreCount = dst_count;
      break;
   default:
      break;
   }

   return dst_count;
}

static uint32_t
vn_queue_submission_setup_batch_syncs(struct vn_queue_submission *submit,
                                      uint32_t batch_index,
                                      uint32_t sync_base)
{
   union {
      const VkSubmitInfo *submit_batch;
      const VkBindSparseInfo *bind_sparse_batch;
   } u;
   const VkTimelineSemaphoreSubmitInfo *timeline;
   const VkSemaphore *sems;
   uint32_t sem_count;
   switch (submit->batch_type) {
   case VK_STRUCTURE_TYPE_SUBMIT_INFO:
      u.submit_batch = &submit->submit_batches[batch_index];
      timeline = vk_find_struct_const(u.submit_batch->pNext,
                                      TIMELINE_SEMAPHORE_SUBMIT_INFO);
      sems = u.submit_batch->pSignalSemaphores;
      sem_count = u.submit_batch->signalSemaphoreCount;
      break;
   case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
      u.bind_sparse_batch = &submit->bind_sparse_batches[batch_index];
      timeline = vk_find_struct_const(u.bind_sparse_batch->pNext,
                                      TIMELINE_SEMAPHORE_SUBMIT_INFO);
      sems = u.bind_sparse_batch->pSignalSemaphores;
      sem_count = u.bind_sparse_batch->signalSemaphoreCount;
      break;
   default:
      unreachable("unexpected batch type");
      break;
   }

   struct vn_renderer_sync **syncs = &submit->temp.syncs[sync_base];
   uint64_t *sync_values = &submit->temp.sync_values[sync_base];
   uint32_t sync_count = 0;

   for (uint32_t i = 0; i < sem_count; i++) {
      struct vn_semaphore *sem = vn_semaphore_from_handle(sems[i]);
      const struct vn_sync_payload *payload = sem->payload;

      if (payload->type == VN_SYNC_TYPE_DEVICE_ONLY)
         continue;

      assert(payload->type == VN_SYNC_TYPE_SYNC);
      syncs[sync_count] = payload->sync;
      sync_values[sync_count] = sem->type == VK_SEMAPHORE_TYPE_TIMELINE
                                   ? timeline->pSignalSemaphoreValues[i]
                                   : 1;
      sync_count++;
   }

   submit->temp.batch_sync_counts[batch_index] = sync_count;

   return sync_count;
}

static uint32_t
vn_queue_submission_setup_fence_sync(struct vn_queue_submission *submit,
                                     uint32_t sync_base)
{
   if (submit->fence == VK_NULL_HANDLE)
      return 0;

   struct vn_fence *fence = vn_fence_from_handle(submit->fence);
   struct vn_sync_payload *payload = fence->payload;

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   submit->temp.syncs[sync_base] = payload->sync;
   submit->temp.sync_values[sync_base] = 1;

   return 1;
}

static void
vn_queue_submission_setup_batches(struct vn_queue_submission *submit)
{
   if (!submit->temp.storage)
      return;

   /* make a copy because we need to filter out WSI semaphores */
   if (submit->wait_wsi_count) {
      switch (submit->batch_type) {
      case VK_STRUCTURE_TYPE_SUBMIT_INFO:
         memcpy(submit->temp.submit_batches, submit->submit_batches,
                sizeof(submit->submit_batches[0]) * submit->batch_count);
         submit->submit_batches = submit->temp.submit_batches;
         break;
      case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
         memcpy(submit->temp.bind_sparse_batches, submit->bind_sparse_batches,
                sizeof(submit->bind_sparse_batches[0]) * submit->batch_count);
         submit->bind_sparse_batches = submit->temp.bind_sparse_batches;
         break;
      default:
         unreachable("unexpected batch type");
         break;
      }
   }

   uint32_t wait_sem_base = 0;
   uint32_t sync_base = 0;
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      if (submit->wait_wsi_count) {
         wait_sem_base += vn_queue_submission_filter_batch_wsi_semaphores(
            submit, i, wait_sem_base);
      }

      if (submit->signal_semaphore_count > submit->signal_device_only_count) {
         sync_base +=
            vn_queue_submission_setup_batch_syncs(submit, i, sync_base);
      } else if (submit->sync_count) {
         submit->temp.batch_sync_counts[i] = 0;
      }
   }

   sync_base += vn_queue_submission_setup_fence_sync(submit, sync_base);

   assert(sync_base == submit->sync_count);
}

static VkResult
vn_queue_submission_prepare_submit(struct vn_queue_submission *submit,
                                   VkQueue queue,
                                   uint32_t batch_count,
                                   const VkSubmitInfo *submit_batches,
                                   VkFence fence)
{
   submit->batch_type = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit->queue = queue;
   submit->batch_count = batch_count;
   submit->submit_batches = submit_batches;
   submit->fence = fence;

   vn_queue_submission_count_semaphores(submit);

   VkResult result = vn_queue_submission_alloc_storage(submit);
   if (result != VK_SUCCESS)
      return result;

   vn_queue_submission_setup_batches(submit);

   return VK_SUCCESS;
}

static VkResult
vn_queue_submission_prepare_bind_sparse(
   struct vn_queue_submission *submit,
   VkQueue queue,
   uint32_t batch_count,
   const VkBindSparseInfo *bind_sparse_batches,
   VkFence fence)
{
   submit->batch_type = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
   submit->queue = queue;
   submit->batch_count = batch_count;
   submit->bind_sparse_batches = bind_sparse_batches;
   submit->fence = fence;

   vn_queue_submission_count_semaphores(submit);

   VkResult result = vn_queue_submission_alloc_storage(submit);
   if (result != VK_SUCCESS)
      return result;

   vn_queue_submission_setup_batches(submit);

   return VK_SUCCESS;
}

static void
vn_queue_submission_cleanup(struct vn_queue_submission *submit)
{
   struct vn_queue *queue = vn_queue_from_handle(submit->queue);
   const VkAllocationCallbacks *alloc = &queue->device->base.base.alloc;

   vk_free(alloc, submit->temp.storage);
}

static void
vn_queue_submit_syncs(struct vn_queue *queue,
                      struct vn_renderer_sync *const *syncs,
                      const uint64_t *sync_values,
                      uint32_t sync_count,
                      struct vn_renderer_bo *wsi_bo)
{
   struct vn_instance *instance = queue->device->instance;
   const struct vn_renderer_submit_batch batch = {
      .sync_queue_index = queue->sync_queue_index,
      .vk_queue_id = queue->base.id,
      .syncs = syncs,
      .sync_values = sync_values,
      .sync_count = sync_count,
   };
   const struct vn_renderer_submit submit = {
      .bos = &wsi_bo,
      .bo_count = wsi_bo ? 1 : 0,
      .batches = &batch,
      .batch_count = 1,
   };

   vn_renderer_submit(instance->renderer, &submit);
   vn_instance_roundtrip(instance);
}

VkResult
vn_QueueSubmit(VkQueue _queue,
               uint32_t submitCount,
               const VkSubmitInfo *pSubmits,
               VkFence fence)
{
   struct vn_queue *queue = vn_queue_from_handle(_queue);
   struct vn_device *dev = queue->device;

   struct vn_queue_submission submit;
   VkResult result = vn_queue_submission_prepare_submit(
      &submit, _queue, submitCount, pSubmits, fence);
   if (result != VK_SUCCESS)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   const struct vn_device_memory *wsi_mem = NULL;
   if (submit.batch_count == 1) {
      const struct wsi_memory_signal_submit_info *info = vk_find_struct_const(
         submit.submit_batches[0].pNext, WSI_MEMORY_SIGNAL_SUBMIT_INFO_MESA);
      if (info) {
         wsi_mem = vn_device_memory_from_handle(info->memory);
         assert(!wsi_mem->base_memory && wsi_mem->base_bo);
      }
   }

   /* TODO this should be one trip to the renderer */
   if (submit.signal_timeline_count) {
      uint32_t sync_base = 0;
      for (uint32_t i = 0; i < submit.batch_count - 1; i++) {
         vn_async_vkQueueSubmit(dev->instance, submit.queue, 1,
                                &submit.submit_batches[i], VK_NULL_HANDLE);
         vn_instance_ring_wait(dev->instance);

         vn_queue_submit_syncs(queue, &submit.temp.syncs[sync_base],
                               &submit.temp.sync_values[sync_base],
                               submit.temp.batch_sync_counts[i], NULL);
         sync_base += submit.temp.batch_sync_counts[i];
      }

      result = vn_call_vkQueueSubmit(
         dev->instance, submit.queue, 1,
         &submit.submit_batches[submit.batch_count - 1], submit.fence);
      if (result != VK_SUCCESS) {
         vn_queue_submission_cleanup(&submit);
         return vn_error(dev->instance, result);
      }

      if (sync_base < submit.sync_count || wsi_mem) {
         vn_queue_submit_syncs(queue, &submit.temp.syncs[sync_base],
                               &submit.temp.sync_values[sync_base],
                               submit.sync_count - sync_base,
                               wsi_mem ? wsi_mem->base_bo : NULL);
      }
   } else {
      result = vn_call_vkQueueSubmit(dev->instance, submit.queue,
                                     submit.batch_count,
                                     submit.submit_batches, submit.fence);
      if (result != VK_SUCCESS) {
         vn_queue_submission_cleanup(&submit);
         return vn_error(dev->instance, result);
      }

      if (submit.sync_count || wsi_mem) {
         vn_queue_submit_syncs(queue, submit.temp.syncs,
                               submit.temp.sync_values, submit.sync_count,
                               wsi_mem ? wsi_mem->base_bo : NULL);
      }
   }

   /* XXX The implicit fence won't work because the host is not aware of it.
    * It is guest-only and the guest kernel does not wait.  We need kernel
    * support, or better yet, an explicit fence that the host is aware of.
    *
    * vn_AcquireNextImage2KHR is also broken.
    */
   if (wsi_mem && VN_DEBUG(WSI)) {
      static uint32_t ratelimit;
      if (ratelimit < 10) {
         vn_log(dev->instance, "forcing vkQueueWaitIdle before presenting");
         ratelimit++;
      }
      vn_QueueWaitIdle(submit.queue);
   }

   vn_queue_submission_cleanup(&submit);

   return VK_SUCCESS;
}

VkResult
vn_QueueBindSparse(VkQueue _queue,
                   uint32_t bindInfoCount,
                   const VkBindSparseInfo *pBindInfo,
                   VkFence fence)
{
   struct vn_queue *queue = vn_queue_from_handle(_queue);
   struct vn_device *dev = queue->device;

   struct vn_queue_submission submit;
   VkResult result = vn_queue_submission_prepare_bind_sparse(
      &submit, _queue, bindInfoCount, pBindInfo, fence);
   if (result != VK_SUCCESS)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* TODO this should be one trip to the renderer */
   if (submit.signal_timeline_count) {
      uint32_t sync_base = 0;
      for (uint32_t i = 0; i < submit.batch_count - 1; i++) {
         vn_async_vkQueueBindSparse(dev->instance, submit.queue, 1,
                                    &submit.bind_sparse_batches[i],
                                    VK_NULL_HANDLE);
         vn_instance_ring_wait(dev->instance);

         vn_queue_submit_syncs(queue, &submit.temp.syncs[sync_base],
                               &submit.temp.sync_values[sync_base],
                               submit.temp.batch_sync_counts[i], NULL);
         sync_base += submit.temp.batch_sync_counts[i];
      }

      result = vn_call_vkQueueBindSparse(
         dev->instance, submit.queue, 1,
         &submit.bind_sparse_batches[submit.batch_count - 1], submit.fence);
      if (result != VK_SUCCESS) {
         vn_queue_submission_cleanup(&submit);
         return vn_error(dev->instance, result);
      }

      if (sync_base < submit.sync_count) {
         vn_queue_submit_syncs(queue, &submit.temp.syncs[sync_base],
                               &submit.temp.sync_values[sync_base],
                               submit.sync_count - sync_base, NULL);
      }
   } else {
      result = vn_call_vkQueueBindSparse(
         dev->instance, submit.queue, submit.batch_count,
         submit.bind_sparse_batches, submit.fence);
      if (result != VK_SUCCESS) {
         vn_queue_submission_cleanup(&submit);
         return vn_error(dev->instance, result);
      }

      if (submit.sync_count) {
         vn_queue_submit_syncs(queue, submit.temp.syncs,
                               submit.temp.sync_values, submit.sync_count,
                               NULL);
      }
   }

   vn_queue_submission_cleanup(&submit);

   return VK_SUCCESS;
}

VkResult
vn_QueueWaitIdle(VkQueue _queue)
{
   struct vn_queue *queue = vn_queue_from_handle(_queue);
   struct vn_device *dev = queue->device;
   struct vn_renderer *renderer = dev->instance->renderer;

   vn_instance_ring_wait(dev->instance);

   const uint64_t val = ++queue->idle_sync_value;
   const struct vn_renderer_submit submit = {
      .batches =
         &(const struct vn_renderer_submit_batch){
            .sync_queue_index = queue->sync_queue_index,
            .vk_queue_id = queue->base.id,
            .syncs = &queue->idle_sync,
            .sync_values = &val,
            .sync_count = 1,
         },
      .batch_count = 1,
   };
   vn_renderer_submit(renderer, &submit);

   const struct vn_renderer_wait wait = {
      .timeout = UINT64_MAX,
      .syncs = &queue->idle_sync,
      .sync_values = &val,
      .sync_count = 1,
   };
   VkResult result = vn_renderer_wait(renderer, &wait);

   return vn_result(dev->instance, result);
}

/* fence commands */

static void
vn_sync_payload_release(struct vn_device *dev,
                        struct vn_sync_payload *payload)
{
   if (payload->type == VN_SYNC_TYPE_SYNC)
      vn_renderer_sync_release(payload->sync);

   payload->type = VN_SYNC_TYPE_INVALID;
}

static VkResult
vn_fence_init_payloads(struct vn_device *dev,
                       struct vn_fence *fence,
                       bool signaled,
                       const VkAllocationCallbacks *alloc)
{
   struct vn_renderer_sync *perm_sync;
   VkResult result = vn_renderer_sync_create_fence(dev->instance->renderer,
                                                   signaled, 0, &perm_sync);
   if (result != VK_SUCCESS)
      return result;

   struct vn_renderer_sync *temp_sync;
   result =
      vn_renderer_sync_create_empty(dev->instance->renderer, &temp_sync);
   if (result != VK_SUCCESS) {
      vn_renderer_sync_destroy(perm_sync);
      return result;
   }

   fence->permanent.type = VN_SYNC_TYPE_SYNC;
   fence->permanent.sync = perm_sync;

   /* temp_sync is uninitialized */
   fence->temporary.type = VN_SYNC_TYPE_INVALID;
   fence->temporary.sync = temp_sync;

   fence->payload = &fence->permanent;

   return VK_SUCCESS;
}

void
vn_fence_signal_wsi(struct vn_device *dev, struct vn_fence *fence)
{
   struct vn_sync_payload *temp = &fence->temporary;

   vn_sync_payload_release(dev, temp);
   temp->type = VN_SYNC_TYPE_WSI_SIGNALED;
   fence->payload = temp;
}

VkResult
vn_CreateFence(VkDevice device,
               const VkFenceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkFence *pFence)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   VkFenceCreateInfo local_create_info;
   if (vk_find_struct_const(pCreateInfo->pNext, EXPORT_FENCE_CREATE_INFO)) {
      local_create_info = *pCreateInfo;
      local_create_info.pNext = NULL;
      pCreateInfo = &local_create_info;
   }

   struct vn_fence *fence = vk_zalloc(alloc, sizeof(*fence), VN_DEFAULT_ALIGN,
                                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!fence)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&fence->base, VK_OBJECT_TYPE_FENCE, &dev->base);

   VkResult result = vn_fence_init_payloads(
      dev, fence, pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT, alloc);
   if (result != VK_SUCCESS) {
      vk_free(alloc, fence);
      return vn_error(dev->instance, result);
   }

   VkFence fence_handle = vn_fence_to_handle(fence);
   vn_async_vkCreateFence(dev->instance, device, pCreateInfo, NULL,
                          &fence_handle);

   *pFence = fence_handle;

   return VK_SUCCESS;
}

void
vn_DestroyFence(VkDevice device,
                VkFence _fence,
                const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_fence *fence = vn_fence_from_handle(_fence);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!fence)
      return;

   vn_async_vkDestroyFence(dev->instance, device, _fence, NULL);

   vn_sync_payload_release(dev, &fence->permanent);
   vn_sync_payload_release(dev, &fence->temporary);
   vn_renderer_sync_destroy(fence->permanent.sync);
   vn_renderer_sync_destroy(fence->temporary.sync);

   vn_object_base_fini(&fence->base);
   vk_free(alloc, fence);
}

VkResult
vn_ResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO if the fence is shared-by-ref, this needs to be synchronous */
   if (false)
      vn_call_vkResetFences(dev->instance, device, fenceCount, pFences);
   else
      vn_async_vkResetFences(dev->instance, device, fenceCount, pFences);

   for (uint32_t i = 0; i < fenceCount; i++) {
      struct vn_fence *fence = vn_fence_from_handle(pFences[i]);
      struct vn_sync_payload *perm = &fence->permanent;

      vn_sync_payload_release(dev, &fence->temporary);

      assert(perm->type == VN_SYNC_TYPE_SYNC);
      vn_renderer_sync_reset(perm->sync, 0);
      fence->payload = perm;
   }

   return VK_SUCCESS;
}

VkResult
vn_GetFenceStatus(VkDevice device, VkFence _fence)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_fence *fence = vn_fence_from_handle(_fence);
   struct vn_sync_payload *payload = fence->payload;

   VkResult result;
   uint64_t val;
   switch (payload->type) {
   case VN_SYNC_TYPE_SYNC:
      result = vn_renderer_sync_read(payload->sync, &val);
      if (result == VK_SUCCESS && !val)
         result = VK_NOT_READY;
      break;
   case VN_SYNC_TYPE_WSI_SIGNALED:
      result = VK_SUCCESS;
      break;
   default:
      unreachable("unexpected fence payload type");
      break;
   }

   return vn_result(dev->instance, result);
}

VkResult
vn_WaitForFences(VkDevice device,
                 uint32_t fenceCount,
                 const VkFence *pFences,
                 VkBool32 waitAll,
                 uint64_t timeout)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   struct vn_renderer_sync *local_syncs[8];
   uint64_t local_sync_vals[8];
   struct vn_renderer_sync **syncs = local_syncs;
   uint64_t *sync_vals = local_sync_vals;
   if (fenceCount > ARRAY_SIZE(local_syncs)) {
      syncs = vk_alloc(alloc, sizeof(*syncs) * fenceCount, VN_DEFAULT_ALIGN,
                       VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      sync_vals =
         vk_alloc(alloc, sizeof(*sync_vals) * fenceCount, VN_DEFAULT_ALIGN,
                  VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!syncs || !sync_vals) {
         vk_free(alloc, syncs);
         vk_free(alloc, sync_vals);
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   uint32_t wait_count = 0;
   uint32_t signaled_count = 0;
   for (uint32_t i = 0; i < fenceCount; i++) {
      struct vn_fence *fence = vn_fence_from_handle(pFences[i]);
      const struct vn_sync_payload *payload = fence->payload;

      switch (payload->type) {
      case VN_SYNC_TYPE_SYNC:
         syncs[wait_count] = payload->sync;
         sync_vals[wait_count] = 1;
         wait_count++;
         break;
      case VN_SYNC_TYPE_WSI_SIGNALED:
         signaled_count++;
         break;
      default:
         unreachable("unexpected fence payload type");
         break;
      }
   }

   VkResult result = VK_SUCCESS;
   if (wait_count && (waitAll || !signaled_count)) {
      const struct vn_renderer_wait wait = {
         .wait_any = !waitAll,
         .timeout = timeout,
         .syncs = syncs,
         .sync_values = sync_vals,
         .sync_count = wait_count,
      };
      result = vn_renderer_wait(dev->instance->renderer, &wait);
   }

   if (syncs != local_syncs) {
      vk_free(alloc, syncs);
      vk_free(alloc, sync_vals);
   }

   return vn_result(dev->instance, result);
}

VkResult
vn_ImportFenceFdKHR(VkDevice device,
                    const VkImportFenceFdInfoKHR *pImportFenceFdInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_fence *fence = vn_fence_from_handle(pImportFenceFdInfo->fence);
   const bool sync_file = pImportFenceFdInfo->handleType ==
                          VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
   const int fd = pImportFenceFdInfo->fd;
   struct vn_sync_payload *payload =
      pImportFenceFdInfo->flags & VK_FENCE_IMPORT_TEMPORARY_BIT
         ? &fence->temporary
         : &fence->permanent;

   if (payload->type == VN_SYNC_TYPE_SYNC)
      vn_renderer_sync_release(payload->sync);

   VkResult result;
   if (sync_file && fd < 0)
      result = vn_renderer_sync_init_signaled(payload->sync);
   else
      result = vn_renderer_sync_init_syncobj(payload->sync, fd, sync_file);

   if (result != VK_SUCCESS)
      return vn_error(dev->instance, result);

   payload->type = VN_SYNC_TYPE_SYNC;
   fence->payload = payload;

   if (fd >= 0)
      close(fd);

   return VK_SUCCESS;
}

VkResult
vn_GetFenceFdKHR(VkDevice device,
                 const VkFenceGetFdInfoKHR *pGetFdInfo,
                 int *pFd)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_fence *fence = vn_fence_from_handle(pGetFdInfo->fence);
   const bool sync_file =
      pGetFdInfo->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
   struct vn_sync_payload *payload = fence->payload;

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   int fd = vn_renderer_sync_export_syncobj(payload->sync, sync_file);
   if (fd < 0)
      return vn_error(dev->instance, VK_ERROR_TOO_MANY_OBJECTS);

   if (sync_file)
      vn_ResetFences(device, 1, &pGetFdInfo->fence);

   *pFd = fd;
   return VK_SUCCESS;
}

/* semaphore commands */

static VkResult
vn_semaphore_init_payloads(struct vn_device *dev,
                           struct vn_semaphore *sem,
                           uint64_t initial_val,
                           const VkAllocationCallbacks *alloc)
{
   struct vn_renderer_sync *perm_sync;
   VkResult result;
   if (sem->type == VK_SEMAPHORE_TYPE_TIMELINE) {
      result = vn_renderer_sync_create_semaphore(dev->instance->renderer,
                                                 VK_SEMAPHORE_TYPE_TIMELINE,
                                                 initial_val, 0, &perm_sync);
   } else {
      result =
         vn_renderer_sync_create_empty(dev->instance->renderer, &perm_sync);
   }
   if (result != VK_SUCCESS)
      return result;

   struct vn_renderer_sync *temp_sync;
   result =
      vn_renderer_sync_create_empty(dev->instance->renderer, &temp_sync);
   if (result != VK_SUCCESS) {
      vn_renderer_sync_destroy(perm_sync);
      return result;
   }

   sem->permanent.type = sem->type == VK_SEMAPHORE_TYPE_TIMELINE
                            ? VN_SYNC_TYPE_SYNC
                            : VN_SYNC_TYPE_DEVICE_ONLY;
   sem->permanent.sync = perm_sync;

   /* temp_sync is uninitialized */
   sem->temporary.type = VN_SYNC_TYPE_INVALID;
   sem->temporary.sync = temp_sync;

   sem->payload = &sem->permanent;

   return VK_SUCCESS;
}

static void
vn_semaphore_reset_wsi(struct vn_device *dev, struct vn_semaphore *sem)
{
   struct vn_sync_payload *perm = &sem->permanent;

   vn_sync_payload_release(dev, &sem->temporary);

   if (perm->type == VN_SYNC_TYPE_SYNC)
      vn_renderer_sync_reset(perm->sync, 0);
   sem->payload = perm;
}

void
vn_semaphore_signal_wsi(struct vn_device *dev, struct vn_semaphore *sem)
{
   struct vn_sync_payload *temp = &sem->temporary;

   vn_sync_payload_release(dev, temp);
   temp->type = VN_SYNC_TYPE_WSI_SIGNALED;
   sem->payload = temp;
}

VkResult
vn_CreateSemaphore(VkDevice device,
                   const VkSemaphoreCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkSemaphore *pSemaphore)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_semaphore *sem = vk_zalloc(alloc, sizeof(*sem), VN_DEFAULT_ALIGN,
                                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!sem)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&sem->base, VK_OBJECT_TYPE_SEMAPHORE, &dev->base);

   const VkSemaphoreTypeCreateInfo *type_info =
      vk_find_struct_const(pCreateInfo->pNext, SEMAPHORE_TYPE_CREATE_INFO);
   uint64_t initial_val = 0;
   if (type_info && type_info->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
      sem->type = VK_SEMAPHORE_TYPE_TIMELINE;
      initial_val = type_info->initialValue;
   } else {
      sem->type = VK_SEMAPHORE_TYPE_BINARY;
   }

   VkResult result = vn_semaphore_init_payloads(dev, sem, initial_val, alloc);
   if (result != VK_SUCCESS) {
      vk_free(alloc, sem);
      return vn_error(dev->instance, result);
   }

   VkSemaphore sem_handle = vn_semaphore_to_handle(sem);
   vn_async_vkCreateSemaphore(dev->instance, device, pCreateInfo, NULL,
                              &sem_handle);

   *pSemaphore = sem_handle;

   return VK_SUCCESS;
}

void
vn_DestroySemaphore(VkDevice device,
                    VkSemaphore semaphore,
                    const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem = vn_semaphore_from_handle(semaphore);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!sem)
      return;

   vn_async_vkDestroySemaphore(dev->instance, device, semaphore, NULL);

   vn_sync_payload_release(dev, &sem->permanent);
   vn_sync_payload_release(dev, &sem->temporary);
   vn_renderer_sync_destroy(sem->permanent.sync);
   vn_renderer_sync_destroy(sem->temporary.sync);

   vn_object_base_fini(&sem->base);
   vk_free(alloc, sem);
}

VkResult
vn_GetSemaphoreCounterValue(VkDevice device,
                            VkSemaphore semaphore,
                            uint64_t *pValue)
{
   struct vn_semaphore *sem = vn_semaphore_from_handle(semaphore);
   struct vn_sync_payload *payload = sem->payload;

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   return vn_renderer_sync_read(payload->sync, pValue);
}

VkResult
vn_SignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo *pSignalInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem =
      vn_semaphore_from_handle(pSignalInfo->semaphore);
   struct vn_sync_payload *payload = sem->payload;

   /* TODO if the semaphore is shared-by-ref, this needs to be synchronous */
   if (false)
      vn_call_vkSignalSemaphore(dev->instance, device, pSignalInfo);
   else
      vn_async_vkSignalSemaphore(dev->instance, device, pSignalInfo);

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   vn_renderer_sync_write(payload->sync, pSignalInfo->value);

   return VK_SUCCESS;
}

VkResult
vn_WaitSemaphores(VkDevice device,
                  const VkSemaphoreWaitInfo *pWaitInfo,
                  uint64_t timeout)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   struct vn_renderer_sync *local_syncs[8];
   struct vn_renderer_sync **syncs = local_syncs;
   if (pWaitInfo->semaphoreCount > ARRAY_SIZE(local_syncs)) {
      syncs = vk_alloc(alloc, sizeof(*syncs) * pWaitInfo->semaphoreCount,
                       VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!syncs)
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; i++) {
      struct vn_semaphore *sem =
         vn_semaphore_from_handle(pWaitInfo->pSemaphores[i]);
      const struct vn_sync_payload *payload = sem->payload;

      assert(payload->type == VN_SYNC_TYPE_SYNC);
      syncs[i] = payload->sync;
   }

   const struct vn_renderer_wait wait = {
      .wait_any = pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT,
      .timeout = timeout,
      .syncs = syncs,
      .sync_values = pWaitInfo->pValues,
      .sync_count = pWaitInfo->semaphoreCount,
   };
   VkResult result = vn_renderer_wait(dev->instance->renderer, &wait);

   if (syncs != local_syncs)
      vk_free(alloc, syncs);

   return vn_result(dev->instance, result);
}

VkResult
vn_ImportSemaphoreFdKHR(
   VkDevice device, const VkImportSemaphoreFdInfoKHR *pImportSemaphoreFdInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem =
      vn_semaphore_from_handle(pImportSemaphoreFdInfo->semaphore);
   const bool sync_file = pImportSemaphoreFdInfo->handleType ==
                          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
   const int fd = pImportSemaphoreFdInfo->fd;
   struct vn_sync_payload *payload =
      pImportSemaphoreFdInfo->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT
         ? &sem->temporary
         : &sem->permanent;

   if (payload->type == VN_SYNC_TYPE_SYNC)
      vn_renderer_sync_release(payload->sync);

   VkResult result;
   if (sync_file && fd < 0)
      result = vn_renderer_sync_init_signaled(payload->sync);
   else
      result = vn_renderer_sync_init_syncobj(payload->sync, fd, sync_file);

   if (result != VK_SUCCESS)
      return vn_error(dev->instance, result);

   /* TODO import into the host-side semaphore */

   payload->type = VN_SYNC_TYPE_SYNC;
   sem->payload = payload;

   if (fd >= 0)
      close(fd);

   return VK_SUCCESS;
}

VkResult
vn_GetSemaphoreFdKHR(VkDevice device,
                     const VkSemaphoreGetFdInfoKHR *pGetFdInfo,
                     int *pFd)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem = vn_semaphore_from_handle(pGetFdInfo->semaphore);
   const bool sync_file =
      pGetFdInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
   struct vn_sync_payload *payload = sem->payload;

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   int fd = vn_renderer_sync_export_syncobj(payload->sync, sync_file);
   if (fd < 0)
      return vn_error(dev->instance, VK_ERROR_TOO_MANY_OBJECTS);

   if (sync_file) {
      vn_sync_payload_release(dev, &sem->temporary);
      vn_renderer_sync_reset(sem->permanent.sync, 0);
      sem->payload = &sem->permanent;
      /* TODO reset the host-side semaphore */
   }

   *pFd = fd;
   return VK_SUCCESS;
}

/* device memory commands */

static VkResult
vn_device_memory_simple_alloc(struct vn_device *dev,
                              uint32_t mem_type_index,
                              VkDeviceSize size,
                              struct vn_device_memory **out_mem)
{
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   struct vn_device_memory *mem =
      vk_zalloc(alloc, sizeof(*mem), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!mem)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   vn_object_base_init(&mem->base, VK_OBJECT_TYPE_DEVICE_MEMORY, &dev->base);
   mem->size = size;

   VkDeviceMemory mem_handle = vn_device_memory_to_handle(mem);
   VkResult result = vn_call_vkAllocateMemory(
      dev->instance, vn_device_to_handle(dev),
      &(const VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = size,
         .memoryTypeIndex = mem_type_index,
      },
      NULL, &mem_handle);
   if (result != VK_SUCCESS) {
      vk_free(alloc, mem);
      return result;
   }

   const VkPhysicalDeviceMemoryProperties *mem_props =
      &dev->physical_device->memory_properties.memoryProperties;
   const VkMemoryType *mem_type = &mem_props->memoryTypes[mem_type_index];
   result = vn_renderer_bo_create_gpu(dev->instance->renderer, mem->size,
                                      mem->base.id, mem_type->propertyFlags,
                                      0, &mem->base_bo);
   if (result != VK_SUCCESS) {
      vn_async_vkFreeMemory(dev->instance, vn_device_to_handle(dev),
                            mem_handle, NULL);
      vk_free(alloc, mem);
      return result;
   }
   vn_instance_roundtrip(dev->instance);

   *out_mem = mem;

   return VK_SUCCESS;
}

static void
vn_device_memory_simple_free(struct vn_device *dev,
                             struct vn_device_memory *mem)
{
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   if (mem->base_bo)
      vn_renderer_bo_unref(mem->base_bo);

   vn_async_vkFreeMemory(dev->instance, vn_device_to_handle(dev),
                         vn_device_memory_to_handle(mem), NULL);
   vn_object_base_fini(&mem->base);
   vk_free(alloc, mem);
}

static void
vn_device_memory_pool_fini(struct vn_device *dev, uint32_t mem_type_index)
{
   struct vn_device_memory_pool *pool = &dev->memory_pools[mem_type_index];
   if (pool->memory)
      vn_device_memory_simple_free(dev, pool->memory);
   mtx_destroy(&pool->mutex);
}

static VkResult
vn_device_memory_pool_grow_locked(struct vn_device *dev,
                                  uint32_t mem_type_index,
                                  VkDeviceSize size)
{
   struct vn_device_memory *mem;
   VkResult result =
      vn_device_memory_simple_alloc(dev, mem_type_index, size, &mem);
   if (result != VK_SUCCESS)
      return result;

   struct vn_device_memory_pool *pool = &dev->memory_pools[mem_type_index];
   if (pool->memory) {
      const bool bo_destroyed = vn_renderer_bo_unref(pool->memory->base_bo);
      pool->memory->base_bo = NULL;

      /* we use pool->memory's base_bo to keep it alive */
      if (bo_destroyed)
         vn_device_memory_simple_free(dev, pool->memory);
   }

   pool->memory = mem;
   pool->used = 0;

   return VK_SUCCESS;
}

static VkResult
vn_device_memory_pool_alloc(struct vn_device *dev,
                            uint32_t mem_type_index,
                            VkDeviceSize size,
                            struct vn_device_memory **base_mem,
                            struct vn_renderer_bo **base_bo,
                            VkDeviceSize *base_offset)
{
   /* We should not support suballocations because apps can do better and we
    * also don't know the alignment requirements.  But each BO takes up a
    * precious KVM memslot currently and some CTS tests exhausts them...
    */
   const VkDeviceSize pool_size = 16 * 1024 * 1024;
   const VkDeviceSize pool_align = 4096; /* XXX */
   struct vn_device_memory_pool *pool = &dev->memory_pools[mem_type_index];

   assert(size <= pool_size);

   mtx_lock(&pool->mutex);

   if (!pool->memory || pool->used + size > pool_size) {
      VkResult result =
         vn_device_memory_pool_grow_locked(dev, mem_type_index, pool_size);
      if (result != VK_SUCCESS) {
         mtx_unlock(&pool->mutex);
         return result;
      }
   }

   /* we use base_bo to keep base_mem alive */
   *base_mem = pool->memory;
   *base_bo = vn_renderer_bo_ref(pool->memory->base_bo);

   *base_offset = pool->used;
   pool->used += align64(size, pool_align);

   mtx_unlock(&pool->mutex);

   return VK_SUCCESS;
}

static void
vn_device_memory_pool_free(struct vn_device *dev,
                           struct vn_device_memory *base_mem,
                           struct vn_renderer_bo *base_bo)
{
   /* we use base_bo to keep base_mem alive */
   if (vn_renderer_bo_unref(base_bo))
      vn_device_memory_simple_free(dev, base_mem);
}

VkResult
vn_AllocateMemory(VkDevice device,
                  const VkMemoryAllocateInfo *pAllocateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkDeviceMemory *pMemory)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   const VkPhysicalDeviceMemoryProperties *mem_props =
      &dev->physical_device->memory_properties.memoryProperties;
   const VkMemoryType *mem_type =
      &mem_props->memoryTypes[pAllocateInfo->memoryTypeIndex];
   const VkImportMemoryFdInfoKHR *import_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
   const VkExportMemoryAllocateInfo *export_info =
      vk_find_struct_const(pAllocateInfo->pNext, EXPORT_MEMORY_ALLOCATE_INFO);
   if (export_info && !export_info->handleTypes)
      export_info = NULL;

   const bool need_bo =
      (mem_type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
      import_info || export_info;
   const bool suballocate =
      need_bo && !pAllocateInfo->pNext &&
      !(mem_type->propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) &&
      pAllocateInfo->allocationSize <= 64 * 1024;

   struct vn_device_memory *mem =
      vk_zalloc(alloc, sizeof(*mem), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!mem)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&mem->base, VK_OBJECT_TYPE_DEVICE_MEMORY, &dev->base);
   mem->size = pAllocateInfo->allocationSize;

   VkDeviceMemory mem_handle = vn_device_memory_to_handle(mem);
   VkResult result;
   if (import_info) {
      struct vn_renderer_bo *bo;
      result = vn_renderer_bo_create_dmabuf(
         dev->instance->renderer, pAllocateInfo->allocationSize,
         import_info->fd, mem_type->propertyFlags,
         export_info ? export_info->handleTypes : 0, &bo);
      if (result != VK_SUCCESS) {
         vk_free(alloc, mem);
         return vn_error(dev->instance, result);
      }

      /* TODO create host-side memory from bo->res_id */
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      if (result != VK_SUCCESS) {
         vn_renderer_bo_unref(bo);
         vk_free(alloc, mem);
         return vn_error(dev->instance, result);
      }

      mem->base_bo = bo;
   } else if (suballocate) {
      result = vn_device_memory_pool_alloc(
         dev, pAllocateInfo->memoryTypeIndex, mem->size, &mem->base_memory,
         &mem->base_bo, &mem->base_offset);
      if (result != VK_SUCCESS) {
         vk_free(alloc, mem);
         return vn_error(dev->instance, result);
      }
   } else {
      result = vn_call_vkAllocateMemory(dev->instance, device, pAllocateInfo,
                                        NULL, &mem_handle);
      if (result != VK_SUCCESS) {
         vk_free(alloc, mem);
         return vn_error(dev->instance, result);
      }
   }

   if (need_bo && !mem->base_bo) {
      result = vn_renderer_bo_create_gpu(
         dev->instance->renderer, mem->size, mem->base.id,
         mem_type->propertyFlags, export_info ? export_info->handleTypes : 0,
         &mem->base_bo);
      if (result != VK_SUCCESS) {
         vn_async_vkFreeMemory(dev->instance, device, mem_handle, NULL);
         vk_free(alloc, mem);
         return vn_error(dev->instance, result);
      }
      vn_instance_roundtrip(dev->instance);
   }

   *pMemory = mem_handle;

   return VK_SUCCESS;
}

void
vn_FreeMemory(VkDevice device,
              VkDeviceMemory memory,
              const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_device_memory *mem = vn_device_memory_from_handle(memory);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!mem)
      return;

   if (mem->base_memory) {
      vn_device_memory_pool_free(dev, mem->base_memory, mem->base_bo);
   } else {
      if (mem->base_bo)
         vn_renderer_bo_unref(mem->base_bo);
      vn_async_vkFreeMemory(dev->instance, device, memory, NULL);
   }

   vn_object_base_fini(&mem->base);
   vk_free(alloc, mem);
}

uint64_t
vn_GetDeviceMemoryOpaqueCaptureAddress(
   VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_device_memory *mem = vn_device_memory_from_handle(pInfo->memory);

   assert(!mem->base_memory);
   return vn_call_vkGetDeviceMemoryOpaqueCaptureAddress(dev->instance, device,
                                                        pInfo);
}

VkResult
vn_MapMemory(VkDevice device,
             VkDeviceMemory memory,
             VkDeviceSize offset,
             VkDeviceSize size,
             VkMemoryMapFlags flags,
             void **ppData)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_device_memory *mem = vn_device_memory_from_handle(memory);

   void *ptr = vn_renderer_bo_map(mem->base_bo);
   if (!ptr)
      return vn_error(dev->instance, VK_ERROR_MEMORY_MAP_FAILED);

   mem->map_end = size == VK_WHOLE_SIZE ? mem->size : offset + size;

   *ppData = ptr + mem->base_offset + offset;

   return VK_SUCCESS;
}

void
vn_UnmapMemory(VkDevice device, VkDeviceMemory memory)
{
}

VkResult
vn_FlushMappedMemoryRanges(VkDevice device,
                           uint32_t memoryRangeCount,
                           const VkMappedMemoryRange *pMemoryRanges)
{
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      const VkMappedMemoryRange *range = &pMemoryRanges[i];
      struct vn_device_memory *mem =
         vn_device_memory_from_handle(range->memory);

      const VkDeviceSize size = range->size == VK_WHOLE_SIZE
                                   ? mem->map_end - range->offset
                                   : range->size;
      vn_renderer_bo_flush(mem->base_bo, mem->base_offset + range->offset,
                           size);
   }

   return VK_SUCCESS;
}

VkResult
vn_InvalidateMappedMemoryRanges(VkDevice device,
                                uint32_t memoryRangeCount,
                                const VkMappedMemoryRange *pMemoryRanges)
{
   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      const VkMappedMemoryRange *range = &pMemoryRanges[i];
      struct vn_device_memory *mem =
         vn_device_memory_from_handle(range->memory);

      const VkDeviceSize size = range->size == VK_WHOLE_SIZE
                                   ? mem->map_end - range->offset
                                   : range->size;
      vn_renderer_bo_invalidate(mem->base_bo,
                                mem->base_offset + range->offset, size);
   }

   return VK_SUCCESS;
}

void
vn_GetDeviceMemoryCommitment(VkDevice device,
                             VkDeviceMemory memory,
                             VkDeviceSize *pCommittedMemoryInBytes)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_device_memory *mem = vn_device_memory_from_handle(memory);

   assert(!mem->base_memory);
   vn_call_vkGetDeviceMemoryCommitment(dev->instance, device, memory,
                                       pCommittedMemoryInBytes);
}

VkResult
vn_GetMemoryFdKHR(VkDevice device,
                  const VkMemoryGetFdInfoKHR *pGetFdInfo,
                  int *pFd)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_device_memory *mem =
      vn_device_memory_from_handle(pGetFdInfo->memory);

   assert(!mem->base_memory && mem->base_bo);
   *pFd = vn_renderer_bo_export_dmabuf(mem->base_bo);
   if (*pFd < 0)
      return vn_error(dev->instance, VK_ERROR_TOO_MANY_OBJECTS);

   return VK_SUCCESS;
}

VkResult
vn_GetMemoryFdPropertiesKHR(VkDevice device,
                            VkExternalMemoryHandleTypeFlagBits handleType,
                            int fd,
                            VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   struct vn_device *dev = vn_device_from_handle(device);

   struct vn_renderer_bo *bo;
   VkResult result = vn_renderer_bo_create_dmabuf(dev->instance->renderer, 0,
                                                  fd, 0, handleType, &bo);
   if (result != VK_SUCCESS)
      return vn_error(dev->instance, result);

   /* TODO call into the host with bo->res_id */
   result = VK_ERROR_INVALID_EXTERNAL_HANDLE;

   vn_renderer_bo_unref(bo);

   return result;
}

/* buffer commands */

VkResult
vn_CreateBuffer(VkDevice device,
                const VkBufferCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkBuffer *pBuffer)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_buffer *buf = vk_zalloc(alloc, sizeof(*buf), VN_DEFAULT_ALIGN,
                                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!buf)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&buf->base, VK_OBJECT_TYPE_BUFFER, &dev->base);

   VkBuffer buf_handle = vn_buffer_to_handle(buf);
   /* TODO async */
   VkResult result = vn_call_vkCreateBuffer(dev->instance, device,
                                            pCreateInfo, NULL, &buf_handle);
   if (result != VK_SUCCESS) {
      vk_free(alloc, buf);
      return vn_error(dev->instance, result);
   }

   /* TODO add a per-device cache for the requirements */
   buf->memory_requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
   buf->memory_requirements.pNext = &buf->dedicated_requirements;
   buf->dedicated_requirements.sType =
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
   buf->dedicated_requirements.pNext = NULL;

   vn_call_vkGetBufferMemoryRequirements2(
      dev->instance, device,
      &(VkBufferMemoryRequirementsInfo2){
         .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
         .buffer = vn_buffer_to_handle(buf),
      },
      &buf->memory_requirements);

   *pBuffer = buf_handle;

   return VK_SUCCESS;
}

void
vn_DestroyBuffer(VkDevice device,
                 VkBuffer buffer,
                 const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_buffer *buf = vn_buffer_from_handle(buffer);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!buf)
      return;

   vn_async_vkDestroyBuffer(dev->instance, device, buffer, NULL);

   vn_object_base_fini(&buf->base);
   vk_free(alloc, buf);
}

VkDeviceAddress
vn_GetBufferDeviceAddress(VkDevice device,
                          const VkBufferDeviceAddressInfo *pInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);

   return vn_call_vkGetBufferDeviceAddress(dev->instance, device, pInfo);
}

uint64_t
vn_GetBufferOpaqueCaptureAddress(VkDevice device,
                                 const VkBufferDeviceAddressInfo *pInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);

   return vn_call_vkGetBufferOpaqueCaptureAddress(dev->instance, device,
                                                  pInfo);
}

void
vn_GetBufferMemoryRequirements(VkDevice device,
                               VkBuffer buffer,
                               VkMemoryRequirements *pMemoryRequirements)
{
   const struct vn_buffer *buf = vn_buffer_from_handle(buffer);

   *pMemoryRequirements = buf->memory_requirements.memoryRequirements;
}

void
vn_GetBufferMemoryRequirements2(VkDevice device,
                                const VkBufferMemoryRequirementsInfo2 *pInfo,
                                VkMemoryRequirements2 *pMemoryRequirements)
{
   const struct vn_buffer *buf = vn_buffer_from_handle(pInfo->buffer);
   union {
      VkBaseOutStructure *pnext;
      VkMemoryRequirements2 *two;
      VkMemoryDedicatedRequirements *dedicated;
   } u = { .two = pMemoryRequirements };

   while (u.pnext) {
      switch (u.pnext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2:
         u.two->memoryRequirements =
            buf->memory_requirements.memoryRequirements;
         break;
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS:
         u.dedicated->prefersDedicatedAllocation =
            buf->dedicated_requirements.prefersDedicatedAllocation;
         u.dedicated->requiresDedicatedAllocation =
            buf->dedicated_requirements.requiresDedicatedAllocation;
         break;
      default:
         break;
      }
      u.pnext = u.pnext->pNext;
   }
}

VkResult
vn_BindBufferMemory(VkDevice device,
                    VkBuffer buffer,
                    VkDeviceMemory memory,
                    VkDeviceSize memoryOffset)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_device_memory *mem = vn_device_memory_from_handle(memory);

   if (mem->base_memory) {
      memory = vn_device_memory_to_handle(mem->base_memory);
      memoryOffset += mem->base_offset;
   }

   vn_async_vkBindBufferMemory(dev->instance, device, buffer, memory,
                               memoryOffset);

   return VK_SUCCESS;
}

VkResult
vn_BindBufferMemory2(VkDevice device,
                     uint32_t bindInfoCount,
                     const VkBindBufferMemoryInfo *pBindInfos)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   VkBindBufferMemoryInfo *local_infos = NULL;
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      const VkBindBufferMemoryInfo *info = &pBindInfos[i];
      struct vn_device_memory *mem =
         vn_device_memory_from_handle(info->memory);
      if (!mem->base_memory)
         continue;

      if (!local_infos) {
         const size_t size = sizeof(*local_infos) * bindInfoCount;
         local_infos = vk_alloc(alloc, size, VN_DEFAULT_ALIGN,
                                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
         if (!local_infos)
            return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

         memcpy(local_infos, pBindInfos, size);
      }

      local_infos[i].memory = vn_device_memory_to_handle(mem->base_memory);
      local_infos[i].memoryOffset += mem->base_offset;
   }
   if (local_infos)
      pBindInfos = local_infos;

   vn_async_vkBindBufferMemory2(dev->instance, device, bindInfoCount,
                                pBindInfos);

   vk_free(alloc, local_infos);

   return VK_SUCCESS;
}

/* buffer view commands */

VkResult
vn_CreateBufferView(VkDevice device,
                    const VkBufferViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkBufferView *pView)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_buffer_view *view =
      vk_zalloc(alloc, sizeof(*view), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!view)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&view->base, VK_OBJECT_TYPE_BUFFER_VIEW, &dev->base);

   VkBufferView view_handle = vn_buffer_view_to_handle(view);
   vn_async_vkCreateBufferView(dev->instance, device, pCreateInfo, NULL,
                               &view_handle);

   *pView = view_handle;

   return VK_SUCCESS;
}

void
vn_DestroyBufferView(VkDevice device,
                     VkBufferView bufferView,
                     const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_buffer_view *view = vn_buffer_view_from_handle(bufferView);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!view)
      return;

   vn_async_vkDestroyBufferView(dev->instance, device, bufferView, NULL);

   vn_object_base_fini(&view->base);
   vk_free(alloc, view);
}

/* image commands */

VkResult
vn_CreateImage(VkDevice device,
               const VkImageCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkImage *pImage)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   /* TODO wsi_create_native_image uses modifiers or set wsi_info->scanout to
    * true.  Instead of forcing VK_IMAGE_TILING_LINEAR, we should ask wsi to
    * use wsi_create_prime_image instead.
    */
   const struct wsi_image_create_info *wsi_info =
      vk_find_struct_const(pCreateInfo->pNext, WSI_IMAGE_CREATE_INFO_MESA);
   VkImageCreateInfo local_create_info;
   if (wsi_info && wsi_info->scanout) {
      if (VN_DEBUG(WSI))
         vn_log(dev->instance, "forcing scanout image linear");
      local_create_info = *pCreateInfo;
      local_create_info.tiling = VK_IMAGE_TILING_LINEAR;
      pCreateInfo = &local_create_info;
   }

   struct vn_image *img = vk_zalloc(alloc, sizeof(*img), VN_DEFAULT_ALIGN,
                                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!img)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&img->base, VK_OBJECT_TYPE_IMAGE, &dev->base);

   VkImage img_handle = vn_image_to_handle(img);
   /* TODO async */
   VkResult result = vn_call_vkCreateImage(dev->instance, device, pCreateInfo,
                                           NULL, &img_handle);
   if (result != VK_SUCCESS) {
      vk_free(alloc, img);
      return vn_error(dev->instance, result);
   }

   uint32_t plane_count = 1;
   if (pCreateInfo->flags & VK_IMAGE_CREATE_DISJOINT_BIT) {
      /* TODO VkDrmFormatModifierPropertiesEXT::drmFormatModifierPlaneCount */
      assert(pCreateInfo->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);

      switch (pCreateInfo->format) {
      case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
      case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
      case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
      case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
      case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
      case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
      case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
      case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
         plane_count = 2;
         break;
      case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
      case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
      case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
      case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
      case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
      case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
      case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
      case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
      case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
      case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
      case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
      case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
         plane_count = 3;
         break;
      default:
         plane_count = 1;
         break;
      }
   }
   assert(plane_count <= ARRAY_SIZE(img->memory_requirements));

   /* TODO add a per-device cache for the requirements */
   for (uint32_t i = 0; i < plane_count; i++) {
      img->memory_requirements[i].sType =
         VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
      img->memory_requirements[i].pNext = &img->dedicated_requirements[i];
      img->dedicated_requirements[i].sType =
         VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
      img->dedicated_requirements[i].pNext = NULL;
   }

   if (plane_count == 1) {
      vn_call_vkGetImageMemoryRequirements2(
         dev->instance, device,
         &(VkImageMemoryRequirementsInfo2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = img_handle,
         },
         &img->memory_requirements[0]);
   } else {
      for (uint32_t i = 0; i < plane_count; i++) {
         vn_call_vkGetImageMemoryRequirements2(
            dev->instance, device,
            &(VkImageMemoryRequirementsInfo2){
               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
               .pNext =
                  &(VkImagePlaneMemoryRequirementsInfo){
                     .sType =
                        VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
                     .planeAspect = VK_IMAGE_ASPECT_PLANE_0_BIT << i,
                  },
               .image = img_handle,
            },
            &img->memory_requirements[i]);
      }
   }

   *pImage = img_handle;

   return VK_SUCCESS;
}

void
vn_DestroyImage(VkDevice device,
                VkImage image,
                const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_image *img = vn_image_from_handle(image);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!img)
      return;

   vn_async_vkDestroyImage(dev->instance, device, image, NULL);

   vn_object_base_fini(&img->base);
   vk_free(alloc, img);
}

void
vn_GetImageMemoryRequirements(VkDevice device,
                              VkImage image,
                              VkMemoryRequirements *pMemoryRequirements)
{
   const struct vn_image *img = vn_image_from_handle(image);

   *pMemoryRequirements = img->memory_requirements[0].memoryRequirements;
}

void
vn_GetImageSparseMemoryRequirements(
   VkDevice device,
   VkImage image,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements *pSparseMemoryRequirements)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO per-device cache */
   vn_call_vkGetImageSparseMemoryRequirements(dev->instance, device, image,
                                              pSparseMemoryRequirementCount,
                                              pSparseMemoryRequirements);
}

void
vn_GetImageMemoryRequirements2(VkDevice device,
                               const VkImageMemoryRequirementsInfo2 *pInfo,
                               VkMemoryRequirements2 *pMemoryRequirements)
{
   const struct vn_image *img = vn_image_from_handle(pInfo->image);
   union {
      VkBaseOutStructure *pnext;
      VkMemoryRequirements2 *two;
      VkMemoryDedicatedRequirements *dedicated;
   } u = { .two = pMemoryRequirements };

   uint32_t plane = 0;
   const VkImagePlaneMemoryRequirementsInfo *plane_info =
      vk_find_struct_const(pInfo->pNext,
                           IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO);
   if (plane_info) {
      switch (plane_info->planeAspect) {
      case VK_IMAGE_ASPECT_PLANE_1_BIT:
         plane = 1;
         break;
      case VK_IMAGE_ASPECT_PLANE_2_BIT:
         plane = 2;
         break;
      default:
         plane = 0;
         break;
      }
   }

   while (u.pnext) {
      switch (u.pnext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2:
         u.two->memoryRequirements =
            img->memory_requirements[plane].memoryRequirements;
         break;
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS:
         u.dedicated->prefersDedicatedAllocation =
            img->dedicated_requirements[plane].prefersDedicatedAllocation;
         u.dedicated->requiresDedicatedAllocation =
            img->dedicated_requirements[plane].requiresDedicatedAllocation;
         break;
      default:
         break;
      }
      u.pnext = u.pnext->pNext;
   }
}

void
vn_GetImageSparseMemoryRequirements2(
   VkDevice device,
   const VkImageSparseMemoryRequirementsInfo2 *pInfo,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO per-device cache */
   vn_call_vkGetImageSparseMemoryRequirements2(dev->instance, device, pInfo,
                                               pSparseMemoryRequirementCount,
                                               pSparseMemoryRequirements);
}

VkResult
vn_BindImageMemory(VkDevice device,
                   VkImage image,
                   VkDeviceMemory memory,
                   VkDeviceSize memoryOffset)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_device_memory *mem = vn_device_memory_from_handle(memory);

   if (mem->base_memory) {
      memory = vn_device_memory_to_handle(mem->base_memory);
      memoryOffset += mem->base_offset;
   }

   vn_async_vkBindImageMemory(dev->instance, device, image, memory,
                              memoryOffset);

   return VK_SUCCESS;
}

VkResult
vn_BindImageMemory2(VkDevice device,
                    uint32_t bindInfoCount,
                    const VkBindImageMemoryInfo *pBindInfos)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   VkBindImageMemoryInfo *local_infos = NULL;
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      const VkBindImageMemoryInfo *info = &pBindInfos[i];
      struct vn_device_memory *mem =
         vn_device_memory_from_handle(info->memory);
      /* TODO handle VkBindImageMemorySwapchainInfoKHR */
      if (!mem || !mem->base_memory)
         continue;

      if (!local_infos) {
         const size_t size = sizeof(*local_infos) * bindInfoCount;
         local_infos = vk_alloc(alloc, size, VN_DEFAULT_ALIGN,
                                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
         if (!local_infos)
            return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

         memcpy(local_infos, pBindInfos, size);
      }

      local_infos[i].memory = vn_device_memory_to_handle(mem->base_memory);
      local_infos[i].memoryOffset += mem->base_offset;
   }
   if (local_infos)
      pBindInfos = local_infos;

   vn_async_vkBindImageMemory2(dev->instance, device, bindInfoCount,
                               pBindInfos);

   vk_free(alloc, local_infos);

   return VK_SUCCESS;
}

VkResult
vn_GetImageDrmFormatModifierPropertiesEXT(
   VkDevice device,
   VkImage image,
   VkImageDrmFormatModifierPropertiesEXT *pProperties)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO local cache */
   return vn_call_vkGetImageDrmFormatModifierPropertiesEXT(
      dev->instance, device, image, pProperties);
}

void
vn_GetImageSubresourceLayout(VkDevice device,
                             VkImage image,
                             const VkImageSubresource *pSubresource,
                             VkSubresourceLayout *pLayout)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO local cache */
   vn_call_vkGetImageSubresourceLayout(dev->instance, device, image,
                                       pSubresource, pLayout);
}

/* image view commands */

VkResult
vn_CreateImageView(VkDevice device,
                   const VkImageViewCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkImageView *pView)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_image_view *view =
      vk_zalloc(alloc, sizeof(*view), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!view)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&view->base, VK_OBJECT_TYPE_IMAGE_VIEW, &dev->base);

   VkImageView view_handle = vn_image_view_to_handle(view);
   vn_async_vkCreateImageView(dev->instance, device, pCreateInfo, NULL,
                              &view_handle);

   *pView = view_handle;

   return VK_SUCCESS;
}

void
vn_DestroyImageView(VkDevice device,
                    VkImageView imageView,
                    const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_image_view *view = vn_image_view_from_handle(imageView);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!view)
      return;

   vn_async_vkDestroyImageView(dev->instance, device, imageView, NULL);

   vn_object_base_fini(&view->base);
   vk_free(alloc, view);
}

/* sampler commands */

VkResult
vn_CreateSampler(VkDevice device,
                 const VkSamplerCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkSampler *pSampler)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_sampler *sampler =
      vk_zalloc(alloc, sizeof(*sampler), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!sampler)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&sampler->base, VK_OBJECT_TYPE_SAMPLER, &dev->base);

   VkSampler sampler_handle = vn_sampler_to_handle(sampler);
   vn_async_vkCreateSampler(dev->instance, device, pCreateInfo, NULL,
                            &sampler_handle);

   *pSampler = sampler_handle;

   return VK_SUCCESS;
}

void
vn_DestroySampler(VkDevice device,
                  VkSampler _sampler,
                  const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_sampler *sampler = vn_sampler_from_handle(_sampler);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!sampler)
      return;

   vn_async_vkDestroySampler(dev->instance, device, _sampler, NULL);

   vn_object_base_fini(&sampler->base);
   vk_free(alloc, sampler);
}

/* sampler YCbCr conversion commands */

VkResult
vn_CreateSamplerYcbcrConversion(
   VkDevice device,
   const VkSamplerYcbcrConversionCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkSamplerYcbcrConversion *pYcbcrConversion)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_sampler_ycbcr_conversion *conv =
      vk_zalloc(alloc, sizeof(*conv), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!conv)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&conv->base, VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION,
                       &dev->base);

   VkSamplerYcbcrConversion conv_handle =
      vn_sampler_ycbcr_conversion_to_handle(conv);
   vn_async_vkCreateSamplerYcbcrConversion(dev->instance, device, pCreateInfo,
                                           NULL, &conv_handle);

   *pYcbcrConversion = conv_handle;

   return VK_SUCCESS;
}

void
vn_DestroySamplerYcbcrConversion(VkDevice device,
                                 VkSamplerYcbcrConversion ycbcrConversion,
                                 const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_sampler_ycbcr_conversion *conv =
      vn_sampler_ycbcr_conversion_from_handle(ycbcrConversion);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!conv)
      return;

   vn_async_vkDestroySamplerYcbcrConversion(dev->instance, device,
                                            ycbcrConversion, NULL);

   vn_object_base_fini(&conv->base);
   vk_free(alloc, conv);
}

/* descriptor set layout commands */

void
vn_GetDescriptorSetLayoutSupport(
   VkDevice device,
   const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
   VkDescriptorSetLayoutSupport *pSupport)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO per-device cache */
   vn_call_vkGetDescriptorSetLayoutSupport(dev->instance, device, pCreateInfo,
                                           pSupport);
}

VkResult
vn_CreateDescriptorSetLayout(
   VkDevice device,
   const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkDescriptorSetLayout *pSetLayout)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   uint32_t max_binding = 0;
   VkDescriptorSetLayoutBinding *local_bindings = NULL;
   VkDescriptorSetLayoutCreateInfo local_create_info;
   if (pCreateInfo->bindingCount) {
      /* the encoder does not ignore
       * VkDescriptorSetLayoutBinding::pImmutableSamplers when it should
       */
      const size_t binding_size =
         sizeof(*pCreateInfo->pBindings) * pCreateInfo->bindingCount;
      local_bindings = vk_alloc(alloc, binding_size, VN_DEFAULT_ALIGN,
                                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!local_bindings)
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

      memcpy(local_bindings, pCreateInfo->pBindings, binding_size);
      for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
         VkDescriptorSetLayoutBinding *binding = &local_bindings[i];

         if (max_binding < binding->binding)
            max_binding = binding->binding;

         switch (binding->descriptorType) {
         case VK_DESCRIPTOR_TYPE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            break;
         default:
            binding->pImmutableSamplers = NULL;
            break;
         }
      }

      local_create_info = *pCreateInfo;
      local_create_info.pBindings = local_bindings;
      pCreateInfo = &local_create_info;
   }

   const size_t layout_size =
      offsetof(struct vn_descriptor_set_layout, bindings[max_binding + 1]);
   struct vn_descriptor_set_layout *layout =
      vk_zalloc(alloc, layout_size, VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!layout) {
      vk_free(alloc, local_bindings);
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vn_object_base_init(&layout->base, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                       &dev->base);

   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *binding =
         &pCreateInfo->pBindings[i];
      struct vn_descriptor_set_layout_binding *dst =
         &layout->bindings[binding->binding];

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         dst->has_immutable_samplers = binding->pImmutableSamplers;
         break;
      default:
         break;
      }
   }

   VkDescriptorSetLayout layout_handle =
      vn_descriptor_set_layout_to_handle(layout);
   vn_async_vkCreateDescriptorSetLayout(dev->instance, device, pCreateInfo,
                                        NULL, &layout_handle);

   vk_free(alloc, local_bindings);

   *pSetLayout = layout_handle;

   return VK_SUCCESS;
}

void
vn_DestroyDescriptorSetLayout(VkDevice device,
                              VkDescriptorSetLayout descriptorSetLayout,
                              const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_descriptor_set_layout *layout =
      vn_descriptor_set_layout_from_handle(descriptorSetLayout);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!layout)
      return;

   vn_async_vkDestroyDescriptorSetLayout(dev->instance, device,
                                         descriptorSetLayout, NULL);

   vn_object_base_fini(&layout->base);
   vk_free(alloc, layout);
}

/* descriptor pool commands */

VkResult
vn_CreateDescriptorPool(VkDevice device,
                        const VkDescriptorPoolCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkDescriptorPool *pDescriptorPool)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_descriptor_pool *pool =
      vk_zalloc(alloc, sizeof(*pool), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&pool->base, VK_OBJECT_TYPE_DESCRIPTOR_POOL,
                       &dev->base);

   pool->allocator = *alloc;
   list_inithead(&pool->descriptor_sets);

   VkDescriptorPool pool_handle = vn_descriptor_pool_to_handle(pool);
   vn_async_vkCreateDescriptorPool(dev->instance, device, pCreateInfo, NULL,
                                   &pool_handle);

   *pDescriptorPool = pool_handle;

   return VK_SUCCESS;
}

void
vn_DestroyDescriptorPool(VkDevice device,
                         VkDescriptorPool descriptorPool,
                         const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_descriptor_pool *pool =
      vn_descriptor_pool_from_handle(descriptorPool);
   const VkAllocationCallbacks *alloc;

   if (!pool)
      return;

   alloc = pAllocator ? pAllocator : &pool->allocator;

   vn_async_vkDestroyDescriptorPool(dev->instance, device, descriptorPool,
                                    NULL);

   list_for_each_entry_safe (struct vn_descriptor_set, set,
                             &pool->descriptor_sets, head) {
      list_del(&set->head);

      vn_object_base_fini(&set->base);
      vk_free(alloc, set);
   }

   vn_object_base_fini(&pool->base);
   vk_free(alloc, pool);
}

VkResult
vn_ResetDescriptorPool(VkDevice device,
                       VkDescriptorPool descriptorPool,
                       VkDescriptorPoolResetFlags flags)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_descriptor_pool *pool =
      vn_descriptor_pool_from_handle(descriptorPool);
   const VkAllocationCallbacks *alloc = &pool->allocator;

   vn_async_vkResetDescriptorPool(dev->instance, device, descriptorPool,
                                  flags);

   list_for_each_entry_safe (struct vn_descriptor_set, set,
                             &pool->descriptor_sets, head) {
      list_del(&set->head);

      vn_object_base_fini(&set->base);
      vk_free(alloc, set);
   }

   return VK_SUCCESS;
}

/* descriptor set commands */

VkResult
vn_AllocateDescriptorSets(VkDevice device,
                          const VkDescriptorSetAllocateInfo *pAllocateInfo,
                          VkDescriptorSet *pDescriptorSets)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_descriptor_pool *pool =
      vn_descriptor_pool_from_handle(pAllocateInfo->descriptorPool);
   const VkAllocationCallbacks *alloc = &pool->allocator;

   for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      struct vn_descriptor_set *set =
         vk_zalloc(alloc, sizeof(*set), VN_DEFAULT_ALIGN,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!set) {
         for (uint32_t j = 0; j < i; j++) {
            set = vn_descriptor_set_from_handle(pDescriptorSets[j]);
            list_del(&set->head);
            vk_free(alloc, set);
         }
         memset(pDescriptorSets, 0,
                sizeof(*pDescriptorSets) * pAllocateInfo->descriptorSetCount);
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      vn_object_base_init(&set->base, VK_OBJECT_TYPE_DESCRIPTOR_SET,
                          &dev->base);
      set->layout =
         vn_descriptor_set_layout_from_handle(pAllocateInfo->pSetLayouts[i]);
      list_addtail(&set->head, &pool->descriptor_sets);

      VkDescriptorSet set_handle = vn_descriptor_set_to_handle(set);
      pDescriptorSets[i] = set_handle;
   }

   VkResult result = vn_call_vkAllocateDescriptorSets(
      dev->instance, device, pAllocateInfo, pDescriptorSets);
   if (result != VK_SUCCESS) {
      for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
         struct vn_descriptor_set *set =
            vn_descriptor_set_from_handle(pDescriptorSets[i]);
         list_del(&set->head);
         vk_free(alloc, set);
      }
      memset(pDescriptorSets, 0,
             sizeof(*pDescriptorSets) * pAllocateInfo->descriptorSetCount);
      return vn_error(dev->instance, result);
   }

   return VK_SUCCESS;
}

VkResult
vn_FreeDescriptorSets(VkDevice device,
                      VkDescriptorPool descriptorPool,
                      uint32_t descriptorSetCount,
                      const VkDescriptorSet *pDescriptorSets)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_descriptor_pool *pool =
      vn_descriptor_pool_from_handle(descriptorPool);
   const VkAllocationCallbacks *alloc = &pool->allocator;

   vn_async_vkFreeDescriptorSets(dev->instance, device, descriptorPool,
                                 descriptorSetCount, pDescriptorSets);

   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      struct vn_descriptor_set *set =
         vn_descriptor_set_from_handle(pDescriptorSets[i]);

      if (!set)
         continue;

      list_del(&set->head);

      vn_object_base_fini(&set->base);
      vk_free(alloc, set);
   }

   return VK_SUCCESS;
}

static struct vn_update_descriptor_sets *
vn_update_descriptor_sets_alloc(uint32_t write_count,
                                uint32_t image_count,
                                uint32_t buffer_count,
                                uint32_t view_count,
                                const VkAllocationCallbacks *alloc,
                                VkSystemAllocationScope scope)
{
   const size_t writes_offset = sizeof(struct vn_update_descriptor_sets);
   const size_t images_offset =
      writes_offset + sizeof(VkWriteDescriptorSet) * write_count;
   const size_t buffers_offset =
      images_offset + sizeof(VkDescriptorImageInfo) * image_count;
   const size_t views_offset =
      buffers_offset + sizeof(VkDescriptorBufferInfo) * buffer_count;
   const size_t alloc_size = views_offset + sizeof(VkBufferView) * view_count;

   void *storage = vk_alloc(alloc, alloc_size, VN_DEFAULT_ALIGN, scope);
   if (!storage)
      return NULL;

   struct vn_update_descriptor_sets *update = storage;
   update->write_count = write_count;
   update->writes = storage + writes_offset;
   update->images = storage + images_offset;
   update->buffers = storage + buffers_offset;
   update->views = storage + views_offset;

   return update;
}

static struct vn_update_descriptor_sets *
vn_update_descriptor_sets_parse_writes(uint32_t write_count,
                                       const VkWriteDescriptorSet *writes,
                                       const VkAllocationCallbacks *alloc)
{
   uint32_t img_count = 0;
   for (uint32_t i = 0; i < write_count; i++) {
      const VkWriteDescriptorSet *write = &writes[i];
      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         img_count += write->descriptorCount;
         break;
      default:
         break;
      }
   }

   struct vn_update_descriptor_sets *update =
      vn_update_descriptor_sets_alloc(write_count, img_count, 0, 0, alloc,
                                      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!update)
      return NULL;

   /* the encoder does not ignore
    * VkWriteDescriptorSet::{pImageInfo,pBufferInfo,pTexelBufferView} when it
    * should
    *
    * TODO make the encoder smarter
    */
   memcpy(update->writes, writes, sizeof(*writes) * write_count);
   img_count = 0;
   for (uint32_t i = 0; i < write_count; i++) {
      const struct vn_descriptor_set *set =
         vn_descriptor_set_from_handle(writes[i].dstSet);
      const struct vn_descriptor_set_layout_binding *binding =
         &set->layout->bindings[writes[i].dstBinding];
      VkWriteDescriptorSet *write = &update->writes[i];
      VkDescriptorImageInfo *imgs = &update->images[img_count];

      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         memcpy(imgs, write->pImageInfo,
                sizeof(*imgs) * write->descriptorCount);
         img_count += write->descriptorCount;

         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            switch (write->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
               imgs[j].imageView = VK_NULL_HANDLE;
               break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
               if (binding->has_immutable_samplers)
                  imgs[j].sampler = VK_NULL_HANDLE;
               break;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
               imgs[j].sampler = VK_NULL_HANDLE;
               break;
            default:
               break;
            }
         }

         write->pImageInfo = imgs;
         write->pBufferInfo = NULL;
         write->pTexelBufferView = NULL;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         write->pImageInfo = NULL;
         write->pBufferInfo = NULL;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         write->pImageInfo = NULL;
         write->pTexelBufferView = NULL;
         break;
      default:
         write->pImageInfo = NULL;
         write->pBufferInfo = NULL;
         write->pTexelBufferView = NULL;
         break;
      }
   }

   return update;
}

void
vn_UpdateDescriptorSets(VkDevice device,
                        uint32_t descriptorWriteCount,
                        const VkWriteDescriptorSet *pDescriptorWrites,
                        uint32_t descriptorCopyCount,
                        const VkCopyDescriptorSet *pDescriptorCopies)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   struct vn_update_descriptor_sets *update =
      vn_update_descriptor_sets_parse_writes(descriptorWriteCount,
                                             pDescriptorWrites, alloc);
   if (!update) {
      /* TODO update one-by-one? */
      vn_log(dev->instance, "TODO descriptor set update ignored due to OOM");
      return;
   }

   vn_async_vkUpdateDescriptorSets(dev->instance, device, update->write_count,
                                   update->writes, descriptorCopyCount,
                                   pDescriptorCopies);

   vk_free(alloc, update);
}

/* descriptor update template commands */

static struct vn_update_descriptor_sets *
vn_update_descriptor_sets_parse_template(
   const VkDescriptorUpdateTemplateCreateInfo *create_info,
   const VkAllocationCallbacks *alloc,
   struct vn_descriptor_update_template_entry *entries)
{
   uint32_t img_count = 0;
   uint32_t buf_count = 0;
   uint32_t view_count = 0;
   for (uint32_t i = 0; i < create_info->descriptorUpdateEntryCount; i++) {
      const VkDescriptorUpdateTemplateEntry *entry =
         &create_info->pDescriptorUpdateEntries[i];

      switch (entry->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         img_count += entry->descriptorCount;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         view_count += entry->descriptorCount;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         buf_count += entry->descriptorCount;
         break;
      default:
         unreachable("unhandled descriptor type");
         break;
      }
   }

   struct vn_update_descriptor_sets *update = vn_update_descriptor_sets_alloc(
      create_info->descriptorUpdateEntryCount, img_count, buf_count,
      view_count, alloc, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!update)
      return NULL;

   img_count = 0;
   buf_count = 0;
   view_count = 0;
   for (uint32_t i = 0; i < create_info->descriptorUpdateEntryCount; i++) {
      const VkDescriptorUpdateTemplateEntry *entry =
         &create_info->pDescriptorUpdateEntries[i];
      VkWriteDescriptorSet *write = &update->writes[i];

      write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write->pNext = NULL;
      write->dstBinding = entry->dstBinding;
      write->dstArrayElement = entry->dstArrayElement;
      write->descriptorCount = entry->descriptorCount;
      write->descriptorType = entry->descriptorType;

      entries[i].offset = entry->offset;
      entries[i].stride = entry->stride;

      switch (entry->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         write->pImageInfo = &update->images[img_count];
         write->pBufferInfo = NULL;
         write->pTexelBufferView = NULL;
         img_count += entry->descriptorCount;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         write->pImageInfo = NULL;
         write->pBufferInfo = NULL;
         write->pTexelBufferView = &update->views[view_count];
         view_count += entry->descriptorCount;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         write->pImageInfo = NULL;
         write->pBufferInfo = &update->buffers[buf_count];
         write->pTexelBufferView = NULL;
         buf_count += entry->descriptorCount;
         break;
      default:
         break;
      }
   }

   return update;
}

VkResult
vn_CreateDescriptorUpdateTemplate(
   VkDevice device,
   const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   const size_t templ_size =
      offsetof(struct vn_descriptor_update_template,
               entries[pCreateInfo->descriptorUpdateEntryCount + 1]);
   struct vn_descriptor_update_template *templ = vk_zalloc(
      alloc, templ_size, VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!templ)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&templ->base,
                       VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, &dev->base);

   templ->update = vn_update_descriptor_sets_parse_template(
      pCreateInfo, alloc, templ->entries);
   if (!templ->update) {
      vk_free(alloc, templ);
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   mtx_init(&templ->mutex, mtx_plain);

   /* no host object */
   VkDescriptorUpdateTemplate templ_handle =
      vn_descriptor_update_template_to_handle(templ);
   *pDescriptorUpdateTemplate = templ_handle;

   return VK_SUCCESS;
}

void
vn_DestroyDescriptorUpdateTemplate(
   VkDevice device,
   VkDescriptorUpdateTemplate descriptorUpdateTemplate,
   const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_descriptor_update_template *templ =
      vn_descriptor_update_template_from_handle(descriptorUpdateTemplate);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!templ)
      return;

   /* no host object */
   vk_free(alloc, templ->update);
   mtx_destroy(&templ->mutex);

   vn_object_base_fini(&templ->base);
   vk_free(alloc, templ);
}

void
vn_UpdateDescriptorSetWithTemplate(
   VkDevice device,
   VkDescriptorSet descriptorSet,
   VkDescriptorUpdateTemplate descriptorUpdateTemplate,
   const void *pData)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_descriptor_set *set =
      vn_descriptor_set_from_handle(descriptorSet);
   struct vn_descriptor_update_template *templ =
      vn_descriptor_update_template_from_handle(descriptorUpdateTemplate);
   struct vn_update_descriptor_sets *update = templ->update;

   /* duplicate update instead to avoid locking? */
   mtx_lock(&templ->mutex);

   for (uint32_t i = 0; i < update->write_count; i++) {
      const struct vn_descriptor_update_template_entry *entry =
         &templ->entries[i];
      const struct vn_descriptor_set_layout_binding *binding =
         &set->layout->bindings[update->writes[i].dstBinding];
      VkWriteDescriptorSet *write = &update->writes[i];

      write->dstSet = vn_descriptor_set_to_handle(set);

      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            const bool need_sampler =
               (write->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                write->descriptorType ==
                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
               !binding->has_immutable_samplers;
            const bool need_view =
               write->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER;
            const VkDescriptorImageInfo *src =
               pData + entry->offset + entry->stride * j;
            VkDescriptorImageInfo *dst =
               (VkDescriptorImageInfo *)&write->pImageInfo[j];

            dst->sampler = need_sampler ? src->sampler : VK_NULL_HANDLE;
            dst->imageView = need_view ? src->imageView : VK_NULL_HANDLE;
            dst->imageLayout = src->imageLayout;
         }
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            const VkBufferView *src =
               pData + entry->offset + entry->stride * j;
            VkBufferView *dst = (VkBufferView *)&write->pTexelBufferView[j];
            *dst = *src;
         }
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            const VkDescriptorBufferInfo *src =
               pData + entry->offset + entry->stride * j;
            VkDescriptorBufferInfo *dst =
               (VkDescriptorBufferInfo *)&write->pBufferInfo[j];
            *dst = *src;
         }
         break;
      default:
         unreachable("unhandled descriptor type");
         break;
      }
   }

   vn_async_vkUpdateDescriptorSets(dev->instance, device, update->write_count,
                                   update->writes, 0, NULL);

   mtx_unlock(&templ->mutex);
}

/* render pass commands */

VkResult
vn_CreateRenderPass(VkDevice device,
                    const VkRenderPassCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkRenderPass *pRenderPass)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_render_pass *pass =
      vk_zalloc(alloc, sizeof(*pass), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pass)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&pass->base, VK_OBJECT_TYPE_RENDER_PASS, &dev->base);

   /* XXX VK_IMAGE_LAYOUT_PRESENT_SRC_KHR */

   VkRenderPass pass_handle = vn_render_pass_to_handle(pass);
   vn_async_vkCreateRenderPass(dev->instance, device, pCreateInfo, NULL,
                               &pass_handle);

   *pRenderPass = pass_handle;

   return VK_SUCCESS;
}

VkResult
vn_CreateRenderPass2(VkDevice device,
                     const VkRenderPassCreateInfo2 *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkRenderPass *pRenderPass)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_render_pass *pass =
      vk_zalloc(alloc, sizeof(*pass), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pass)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&pass->base, VK_OBJECT_TYPE_RENDER_PASS, &dev->base);

   /* XXX VK_IMAGE_LAYOUT_PRESENT_SRC_KHR */

   VkRenderPass pass_handle = vn_render_pass_to_handle(pass);
   vn_async_vkCreateRenderPass2(dev->instance, device, pCreateInfo, NULL,
                                &pass_handle);

   *pRenderPass = pass_handle;

   return VK_SUCCESS;
}

void
vn_DestroyRenderPass(VkDevice device,
                     VkRenderPass renderPass,
                     const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_render_pass *pass = vn_render_pass_from_handle(renderPass);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!pass)
      return;

   vn_async_vkDestroyRenderPass(dev->instance, device, renderPass, NULL);

   vn_object_base_fini(&pass->base);
   vk_free(alloc, pass);
}

void
vn_GetRenderAreaGranularity(VkDevice device,
                            VkRenderPass renderPass,
                            VkExtent2D *pGranularity)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_render_pass *pass = vn_render_pass_from_handle(renderPass);

   if (!pass->granularity.width) {
      vn_call_vkGetRenderAreaGranularity(dev->instance, device, renderPass,
                                         &pass->granularity);
   }

   *pGranularity = pass->granularity;
}

/* framebuffer commands */

VkResult
vn_CreateFramebuffer(VkDevice device,
                     const VkFramebufferCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkFramebuffer *pFramebuffer)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_framebuffer *fb = vk_zalloc(alloc, sizeof(*fb), VN_DEFAULT_ALIGN,
                                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!fb)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&fb->base, VK_OBJECT_TYPE_FRAMEBUFFER, &dev->base);

   VkFramebuffer fb_handle = vn_framebuffer_to_handle(fb);
   vn_async_vkCreateFramebuffer(dev->instance, device, pCreateInfo, NULL,
                                &fb_handle);

   *pFramebuffer = fb_handle;

   return VK_SUCCESS;
}

void
vn_DestroyFramebuffer(VkDevice device,
                      VkFramebuffer framebuffer,
                      const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_framebuffer *fb = vn_framebuffer_from_handle(framebuffer);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!fb)
      return;

   vn_async_vkDestroyFramebuffer(dev->instance, device, framebuffer, NULL);

   vn_object_base_fini(&fb->base);
   vk_free(alloc, fb);
}

/* event commands */

VkResult
vn_CreateEvent(VkDevice device,
               const VkEventCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkEvent *pEvent)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_event *ev = vk_zalloc(alloc, sizeof(*ev), VN_DEFAULT_ALIGN,
                                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!ev)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&ev->base, VK_OBJECT_TYPE_EVENT, &dev->base);

   VkEvent ev_handle = vn_event_to_handle(ev);
   vn_async_vkCreateEvent(dev->instance, device, pCreateInfo, NULL,
                          &ev_handle);

   *pEvent = ev_handle;

   return VK_SUCCESS;
}

void
vn_DestroyEvent(VkDevice device,
                VkEvent event,
                const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_event *ev = vn_event_from_handle(event);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!ev)
      return;

   vn_async_vkDestroyEvent(dev->instance, device, event, NULL);

   vn_object_base_fini(&ev->base);
   vk_free(alloc, ev);
}

VkResult
vn_GetEventStatus(VkDevice device, VkEvent event)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO When the renderer supports it (requires a new vk extension), there
    * should be a coherent memory backing the event.
    */
   VkResult result = vn_call_vkGetEventStatus(dev->instance, device, event);

   return vn_result(dev->instance, result);
}

VkResult
vn_SetEvent(VkDevice device, VkEvent event)
{
   struct vn_device *dev = vn_device_from_handle(device);

   VkResult result = vn_call_vkSetEvent(dev->instance, device, event);

   return vn_result(dev->instance, result);
}

VkResult
vn_ResetEvent(VkDevice device, VkEvent event)
{
   struct vn_device *dev = vn_device_from_handle(device);

   VkResult result = vn_call_vkResetEvent(dev->instance, device, event);

   return vn_result(dev->instance, result);
}

/* query pool commands */

VkResult
vn_CreateQueryPool(VkDevice device,
                   const VkQueryPoolCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkQueryPool *pQueryPool)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_query_pool *pool =
      vk_zalloc(alloc, sizeof(*pool), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&pool->base, VK_OBJECT_TYPE_QUERY_POOL, &dev->base);

   pool->allocator = *alloc;

   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
      pool->result_array_size = 1;
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      pool->result_array_size =
         util_bitcount(pCreateInfo->pipelineStatistics);
      break;
   case VK_QUERY_TYPE_TIMESTAMP:
      pool->result_array_size = 1;
      break;
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
      pool->result_array_size = 2;
      break;
   default:
      unreachable("bad query type");
      break;
   }

   VkQueryPool pool_handle = vn_query_pool_to_handle(pool);
   vn_async_vkCreateQueryPool(dev->instance, device, pCreateInfo, NULL,
                              &pool_handle);

   *pQueryPool = pool_handle;

   return VK_SUCCESS;
}

void
vn_DestroyQueryPool(VkDevice device,
                    VkQueryPool queryPool,
                    const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_query_pool *pool = vn_query_pool_from_handle(queryPool);
   const VkAllocationCallbacks *alloc;

   if (!pool)
      return;

   alloc = pAllocator ? pAllocator : &pool->allocator;

   vn_async_vkDestroyQueryPool(dev->instance, device, queryPool, NULL);

   vn_object_base_fini(&pool->base);
   vk_free(alloc, pool);
}

void
vn_ResetQueryPool(VkDevice device,
                  VkQueryPool queryPool,
                  uint32_t firstQuery,
                  uint32_t queryCount)
{
   struct vn_device *dev = vn_device_from_handle(device);

   vn_async_vkResetQueryPool(dev->instance, device, queryPool, firstQuery,
                             queryCount);
}

VkResult
vn_GetQueryPoolResults(VkDevice device,
                       VkQueryPool queryPool,
                       uint32_t firstQuery,
                       uint32_t queryCount,
                       size_t dataSize,
                       void *pData,
                       VkDeviceSize stride,
                       VkQueryResultFlags flags)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_query_pool *pool = vn_query_pool_from_handle(queryPool);
   const VkAllocationCallbacks *alloc = &pool->allocator;

   const size_t result_width = flags & VK_QUERY_RESULT_64_BIT ? 8 : 4;
   const size_t result_size = pool->result_array_size * result_width;
   const bool result_always_written =
      flags & (VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_PARTIAL_BIT);

   VkQueryResultFlags packed_flags = flags;
   size_t packed_stride = result_size;
   if (!result_always_written)
      packed_flags |= VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
   if (packed_flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
      packed_stride += result_width;

   const size_t packed_size = packed_stride * queryCount;
   void *packed_data;
   if (result_always_written && packed_stride == stride) {
      packed_data = pData;
   } else {
      packed_data = vk_alloc(alloc, packed_size, VN_DEFAULT_ALIGN,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!packed_data)
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* TODO the renderer should transparently vkCmdCopyQueryPoolResults to a
    * coherent memory such that we can memcpy from the coherent memory to
    * avoid this serialized round trip.
    */
   VkResult result = vn_call_vkGetQueryPoolResults(
      dev->instance, device, queryPool, firstQuery, queryCount, packed_size,
      packed_data, packed_stride, packed_flags);

   if (packed_data == pData)
      return vn_result(dev->instance, result);

   const size_t copy_size =
      result_size +
      (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ? result_width : 0);
   const void *src = packed_data;
   void *dst = pData;
   if (result == VK_SUCCESS) {
      for (uint32_t i = 0; i < queryCount; i++) {
         memcpy(dst, src, copy_size);
         src += packed_stride;
         dst += stride;
      }
   } else if (result == VK_NOT_READY) {
      assert(!result_always_written &&
             (packed_flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
      if (flags & VK_QUERY_RESULT_64_BIT) {
         for (uint32_t i = 0; i < queryCount; i++) {
            const bool avail = *(const uint64_t *)(src + result_size);
            if (avail)
               memcpy(dst, src, copy_size);
            else if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
               *(uint64_t *)(dst + result_size) = 0;

            src += packed_stride;
            dst += stride;
         }
      } else {
         for (uint32_t i = 0; i < queryCount; i++) {
            const bool avail = *(const uint32_t *)(src + result_size);
            if (avail)
               memcpy(dst, src, copy_size);
            else if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
               *(uint32_t *)(dst + result_size) = 0;

            src += packed_stride;
            dst += stride;
         }
      }
   }

   vk_free(alloc, packed_data);
   return vn_result(dev->instance, result);
}

/* shader module commands */

VkResult
vn_CreateShaderModule(VkDevice device,
                      const VkShaderModuleCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkShaderModule *pShaderModule)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_shader_module *mod =
      vk_zalloc(alloc, sizeof(*mod), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!mod)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&mod->base, VK_OBJECT_TYPE_SHADER_MODULE, &dev->base);

   VkShaderModule mod_handle = vn_shader_module_to_handle(mod);
   vn_async_vkCreateShaderModule(dev->instance, device, pCreateInfo, NULL,
                                 &mod_handle);

   *pShaderModule = mod_handle;

   return VK_SUCCESS;
}

void
vn_DestroyShaderModule(VkDevice device,
                       VkShaderModule shaderModule,
                       const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_shader_module *mod = vn_shader_module_from_handle(shaderModule);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!mod)
      return;

   vn_async_vkDestroyShaderModule(dev->instance, device, shaderModule, NULL);

   vn_object_base_fini(&mod->base);
   vk_free(alloc, mod);
}

/* pipeline layout commands */

VkResult
vn_CreatePipelineLayout(VkDevice device,
                        const VkPipelineLayoutCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkPipelineLayout *pPipelineLayout)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_pipeline_layout *layout =
      vk_zalloc(alloc, sizeof(*layout), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!layout)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&layout->base, VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                       &dev->base);

   VkPipelineLayout layout_handle = vn_pipeline_layout_to_handle(layout);
   vn_async_vkCreatePipelineLayout(dev->instance, device, pCreateInfo, NULL,
                                   &layout_handle);

   *pPipelineLayout = layout_handle;

   return VK_SUCCESS;
}

void
vn_DestroyPipelineLayout(VkDevice device,
                         VkPipelineLayout pipelineLayout,
                         const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_pipeline_layout *layout =
      vn_pipeline_layout_from_handle(pipelineLayout);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!layout)
      return;

   vn_async_vkDestroyPipelineLayout(dev->instance, device, pipelineLayout,
                                    NULL);

   vn_object_base_fini(&layout->base);
   vk_free(alloc, layout);
}

/* pipeline cache commands */

VkResult
vn_CreatePipelineCache(VkDevice device,
                       const VkPipelineCacheCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkPipelineCache *pPipelineCache)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_pipeline_cache *cache =
      vk_zalloc(alloc, sizeof(*cache), VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cache)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&cache->base, VK_OBJECT_TYPE_PIPELINE_CACHE,
                       &dev->base);

   VkPipelineCacheCreateInfo local_create_info;
   if (pCreateInfo->initialDataSize) {
      local_create_info = *pCreateInfo;
      local_create_info.pInitialData +=
         sizeof(struct vk_pipeline_cache_header);
      pCreateInfo = &local_create_info;
   }

   VkPipelineCache cache_handle = vn_pipeline_cache_to_handle(cache);
   vn_async_vkCreatePipelineCache(dev->instance, device, pCreateInfo, NULL,
                                  &cache_handle);

   *pPipelineCache = cache_handle;

   return VK_SUCCESS;
}

void
vn_DestroyPipelineCache(VkDevice device,
                        VkPipelineCache pipelineCache,
                        const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_pipeline_cache *cache =
      vn_pipeline_cache_from_handle(pipelineCache);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!cache)
      return;

   vn_async_vkDestroyPipelineCache(dev->instance, device, pipelineCache,
                                   NULL);

   vn_object_base_fini(&cache->base);
   vk_free(alloc, cache);
}

VkResult
vn_GetPipelineCacheData(VkDevice device,
                        VkPipelineCache pipelineCache,
                        size_t *pDataSize,
                        void *pData)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_physical_device *physical_dev = dev->physical_device;

   struct vk_pipeline_cache_header *header = pData;
   VkResult result;
   if (!pData) {
      result = vn_call_vkGetPipelineCacheData(dev->instance, device,
                                              pipelineCache, pDataSize, NULL);
      if (result != VK_SUCCESS)
         return vn_error(dev->instance, result);

      *pDataSize += sizeof(*header);
      return VK_SUCCESS;
   }

   if (*pDataSize <= sizeof(*header)) {
      *pDataSize = 0;
      return VK_INCOMPLETE;
   }

   const VkPhysicalDeviceProperties *props =
      &physical_dev->properties.properties;
   header->header_size = sizeof(*header);
   header->header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
   header->vendor_id = props->vendorID;
   header->device_id = props->deviceID;
   memcpy(header->uuid, props->pipelineCacheUUID, VK_UUID_SIZE);

   *pDataSize -= header->header_size;
   result =
      vn_call_vkGetPipelineCacheData(dev->instance, device, pipelineCache,
                                     pDataSize, pData + header->header_size);
   if (result < VK_SUCCESS)
      return vn_error(dev->instance, result);

   *pDataSize += header->header_size;

   return result;
}

VkResult
vn_MergePipelineCaches(VkDevice device,
                       VkPipelineCache dstCache,
                       uint32_t srcCacheCount,
                       const VkPipelineCache *pSrcCaches)
{
   struct vn_device *dev = vn_device_from_handle(device);

   vn_async_vkMergePipelineCaches(dev->instance, device, dstCache,
                                  srcCacheCount, pSrcCaches);

   return VK_SUCCESS;
}

/* pipeline commands */

VkResult
vn_CreateGraphicsPipelines(VkDevice device,
                           VkPipelineCache pipelineCache,
                           uint32_t createInfoCount,
                           const VkGraphicsPipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct vn_pipeline *pipeline =
         vk_zalloc(alloc, sizeof(*pipeline), VN_DEFAULT_ALIGN,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!pipeline) {
         for (uint32_t j = 0; j < i; j++)
            vk_free(alloc, vn_pipeline_from_handle(pPipelines[j]));
         memset(pPipelines, 0, sizeof(*pPipelines) * createInfoCount);
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      vn_object_base_init(&pipeline->base, VK_OBJECT_TYPE_PIPELINE,
                          &dev->base);

      VkPipeline pipeline_handle = vn_pipeline_to_handle(pipeline);
      pPipelines[i] = pipeline_handle;
   }

   vn_async_vkCreateGraphicsPipelines(dev->instance, device, pipelineCache,
                                      createInfoCount, pCreateInfos, NULL,
                                      pPipelines);

   return VK_SUCCESS;
}

VkResult
vn_CreateComputePipelines(VkDevice device,
                          VkPipelineCache pipelineCache,
                          uint32_t createInfoCount,
                          const VkComputePipelineCreateInfo *pCreateInfos,
                          const VkAllocationCallbacks *pAllocator,
                          VkPipeline *pPipelines)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct vn_pipeline *pipeline =
         vk_zalloc(alloc, sizeof(*pipeline), VN_DEFAULT_ALIGN,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!pipeline) {
         for (uint32_t j = 0; j < i; j++)
            vk_free(alloc, vn_pipeline_from_handle(pPipelines[j]));
         memset(pPipelines, 0, sizeof(*pPipelines) * createInfoCount);
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      vn_object_base_init(&pipeline->base, VK_OBJECT_TYPE_PIPELINE,
                          &dev->base);

      VkPipeline pipeline_handle = vn_pipeline_to_handle(pipeline);
      pPipelines[i] = pipeline_handle;
   }

   vn_async_vkCreateComputePipelines(dev->instance, device, pipelineCache,
                                     createInfoCount, pCreateInfos, NULL,
                                     pPipelines);

   return VK_SUCCESS;
}

void
vn_DestroyPipeline(VkDevice device,
                   VkPipeline _pipeline,
                   const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_pipeline *pipeline = vn_pipeline_from_handle(_pipeline);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!pipeline)
      return;

   vn_async_vkDestroyPipeline(dev->instance, device, _pipeline, NULL);

   vn_object_base_fini(&pipeline->base);
   vk_free(alloc, pipeline);
}

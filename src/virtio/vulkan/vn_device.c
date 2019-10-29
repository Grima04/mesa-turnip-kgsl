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

#include "util/driconf.h"
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
   vn_instance_supported_extensions;

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

static void
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

static void
vn_instance_roundtrip(struct vn_instance *instance)
{
   uint32_t roundtrip_seqno;
   if (vn_instance_submit_roundtrip(instance, &roundtrip_seqno) == VK_SUCCESS)
      vn_instance_wait_roundtrip(instance, roundtrip_seqno);
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

   return vn_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);
}

void
vn_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                             VkPhysicalDeviceFeatures *pFeatures)
{
}

void
vn_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceProperties *pProperties)
{
}

void
vn_GetPhysicalDeviceQueueFamilyProperties(
   VkPhysicalDevice physicalDevice,
   uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties *pQueueFamilyProperties)
{
}

void
vn_GetPhysicalDeviceMemoryProperties(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
}

void
vn_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                     VkFormat format,
                                     VkFormatProperties *pFormatProperties)
{
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
   return vn_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
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
}

/* device commands */

VkResult
vn_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                      const char *pLayerName,
                                      uint32_t *pPropertyCount,
                                      VkExtensionProperties *pProperties)
{
   return vn_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
}

VkResult
vn_CreateDevice(VkPhysicalDevice physicalDevice,
                const VkDeviceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkDevice *pDevice)
{
   return vn_error(NULL, VK_ERROR_INCOMPATIBLE_DRIVER);
}

PFN_vkVoidFunction
vn_GetDeviceProcAddr(VkDevice device, const char *pName)
{
   return NULL;
}

/*
 * Copyright © 2019 Red Hat.
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

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>

#include "util/macros.h"
#include "util/list.h"

#include "compiler/shader_enums.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "nir.h"

/* Pre-declarations needed for WSI entrypoints */
struct wl_surface;
struct wl_display;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_window_t;

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#include "val_extensions.h"
#include "val_entrypoints.h"
#include "vk_object.h"

#include "wsi_common.h"

#include <assert.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SETS         8
#define MAX_PUSH_CONSTANTS_SIZE 128

#define val_printflike(a, b) __attribute__((__format__(__printf__, a, b)))

#define typed_memcpy(dest, src, count) ({ \
   memcpy((dest), (src), (count) * sizeof(*(src))); \
})

int val_get_instance_entrypoint_index(const char *name);
int val_get_device_entrypoint_index(const char *name);
int val_get_physical_device_entrypoint_index(const char *name);

const char *val_get_instance_entry_name(int index);
const char *val_get_physical_device_entry_name(int index);
const char *val_get_device_entry_name(int index);

bool val_instance_entrypoint_is_enabled(int index, uint32_t core_version,
                                         const struct val_instance_extension_table *instance);
bool val_physical_device_entrypoint_is_enabled(int index, uint32_t core_version,
                                                const struct val_instance_extension_table *instance);
bool val_device_entrypoint_is_enabled(int index, uint32_t core_version,
                                       const struct val_instance_extension_table *instance,
                                       const struct val_device_extension_table *device);

void *val_lookup_entrypoint(const char *name);

#define VAL_DEFINE_HANDLE_CASTS(__val_type, __VkType)                      \
                                                                           \
   static inline struct __val_type *                                       \
   __val_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __val_type *) _handle;                                \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __val_type ## _to_handle(struct __val_type *_obj)                       \
   {                                                                       \
      return (__VkType) _obj;                                              \
   }

#define VAL_DEFINE_NONDISP_HANDLE_CASTS(__val_type, __VkType)              \
                                                                           \
   static inline struct __val_type *                                       \
   __val_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __val_type *)(uintptr_t) _handle;                     \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __val_type ## _to_handle(struct __val_type *_obj)                       \
   {                                                                       \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

#define VAL_FROM_HANDLE(__val_type, __name, __handle) \
   struct __val_type *__name = __val_type ## _from_handle(__handle)

VAL_DEFINE_HANDLE_CASTS(val_cmd_buffer, VkCommandBuffer)
VAL_DEFINE_HANDLE_CASTS(val_device, VkDevice)
VAL_DEFINE_HANDLE_CASTS(val_instance, VkInstance)
VAL_DEFINE_HANDLE_CASTS(val_physical_device, VkPhysicalDevice)
VAL_DEFINE_HANDLE_CASTS(val_queue, VkQueue)

VAL_DEFINE_NONDISP_HANDLE_CASTS(val_cmd_pool, VkCommandPool)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_buffer, VkBuffer)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_buffer_view, VkBufferView)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_descriptor_pool, VkDescriptorPool)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_descriptor_set, VkDescriptorSet)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_descriptor_set_layout, VkDescriptorSetLayout)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_device_memory, VkDeviceMemory)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_event, VkEvent)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_framebuffer, VkFramebuffer)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_image, VkImage)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_image_view, VkImageView);
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_pipeline_cache, VkPipelineCache)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_pipeline, VkPipeline)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_pipeline_layout, VkPipelineLayout)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_query_pool, VkQueryPool)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_render_pass, VkRenderPass)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_sampler, VkSampler)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_shader_module, VkShaderModule)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_fence, VkFence);
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_semaphore, VkSemaphore);

/* Whenever we generate an error, pass it through this function. Useful for
 * debugging, where we can break on it. Only call at error site, not when
 * propagating errors. Might be useful to plug in a stack trace here.
 */

VkResult __vk_errorf(struct val_instance *instance, VkResult error, const char *file, int line, const char *format, ...);

#define VAL_DEBUG_ALL_ENTRYPOINTS (1 << 0)

#define vk_error(instance, error) __vk_errorf(instance, error, __FILE__, __LINE__, NULL);
#define vk_errorf(instance, error, format, ...) __vk_errorf(instance, error, __FILE__, __LINE__, format, ## __VA_ARGS__);

void __val_finishme(const char *file, int line, const char *format, ...)
   val_printflike(3, 4);

#define val_finishme(format, ...) \
   __val_finishme(__FILE__, __LINE__, format, ##__VA_ARGS__);

#define stub_return(v) \
   do { \
      val_finishme("stub %s", __func__); \
      return (v); \
   } while (0)

#define stub() \
   do { \
      val_finishme("stub %s", __func__); \
      return; \
   } while (0)

struct val_shader_module {
   struct vk_object_base base;
   uint32_t                                     size;
   char                                         data[0];
};

static inline gl_shader_stage
vk_to_mesa_shader_stage(VkShaderStageFlagBits vk_stage)
{
   assert(__builtin_popcount(vk_stage) == 1);
   return ffs(vk_stage) - 1;
}

static inline VkShaderStageFlagBits
mesa_to_vk_shader_stage(gl_shader_stage mesa_stage)
{
   return (1 << mesa_stage);
}

#define VAL_STAGE_MASK ((1 << MESA_SHADER_STAGES) - 1)

#define val_foreach_stage(stage, stage_bits)                         \
   for (gl_shader_stage stage,                                       \
        __tmp = (gl_shader_stage)((stage_bits) & VAL_STAGE_MASK);    \
        stage = __builtin_ffs(__tmp) - 1, __tmp;                     \
        __tmp &= ~(1 << (stage)))

struct val_physical_device {
   VK_LOADER_DATA                              _loader_data;
   struct val_instance *                       instance;

   struct pipe_loader_device *pld;
   struct pipe_screen *pscreen;
   uint32_t max_images;

   struct wsi_device                       wsi_device;
   struct val_device_extension_table supported_extensions;
};

struct val_instance {
   struct vk_object_base base;

   VkAllocationCallbacks alloc;

   uint32_t apiVersion;
   int physicalDeviceCount;
   struct val_physical_device physicalDevice;

   uint64_t debug_flags;

   struct pipe_loader_device *devs;
   int num_devices;

   struct val_instance_extension_table enabled_extensions;
   struct val_instance_dispatch_table dispatch;
   struct val_physical_device_dispatch_table physical_device_dispatch;
   struct val_device_dispatch_table device_dispatch;
};

VkResult val_init_wsi(struct val_physical_device *physical_device);
void val_finish_wsi(struct val_physical_device *physical_device);

bool val_instance_extension_supported(const char *name);
uint32_t val_physical_device_api_version(struct val_physical_device *dev);
bool val_physical_device_extension_supported(struct val_physical_device *dev,
                                              const char *name);

struct val_queue {
   VK_LOADER_DATA                              _loader_data;
   VkDeviceQueueCreateFlags flags;
   struct val_device *                         device;
   struct pipe_context *ctx;
   bool shutdown;
   thrd_t exec_thread;
   mtx_t m;
   cnd_t new_work;
   struct list_head workqueue;
   uint32_t count;
};

struct val_queue_work {
   struct list_head list;
   uint32_t cmd_buffer_count;
   struct val_cmd_buffer **cmd_buffers;
   struct val_fence *fence;
};

struct val_pipeline_cache {
   struct vk_object_base                        base;
   struct val_device *                          device;
   VkAllocationCallbacks                        alloc;
};

struct val_device {
   struct vk_device vk;

   VkAllocationCallbacks                       alloc;

   struct val_queue queue;
   struct val_instance *                       instance;
   struct val_physical_device *physical_device;
   struct pipe_screen *pscreen;

   mtx_t fence_lock;
   struct val_device_extension_table enabled_extensions;
   struct val_device_dispatch_table dispatch;
};

void val_device_get_cache_uuid(void *uuid);

struct val_device_memory {
   struct vk_object_base base;
   struct pipe_memory_allocation *pmem;
   uint32_t                                     type_index;
   VkDeviceSize                                 map_size;
   void *                                       map;
};

struct val_image {
   struct vk_object_base base;
   VkImageType type;
   VkFormat vk_format;
   VkDeviceSize size;
   uint32_t alignment;
   struct pipe_resource *bo;
};

static inline uint32_t
val_get_layerCount(const struct val_image *image,
                   const VkImageSubresourceRange *range)
{
   return range->layerCount == VK_REMAINING_ARRAY_LAYERS ?
      image->bo->array_size - range->baseArrayLayer : range->layerCount;
}

static inline uint32_t
val_get_levelCount(const struct val_image *image,
                   const VkImageSubresourceRange *range)
{
   return range->levelCount == VK_REMAINING_MIP_LEVELS ?
      (image->bo->last_level + 1) - range->baseMipLevel : range->levelCount;
}

struct val_image_create_info {
   const VkImageCreateInfo *vk_info;
   uint32_t bind_flags;
   uint32_t stride;
};

VkResult
val_image_create(VkDevice _device,
                 const struct val_image_create_info *create_info,
                 const VkAllocationCallbacks* alloc,
                 VkImage *pImage);

struct val_image_view {
   struct vk_object_base base;
   const struct val_image *image; /**< VkImageViewCreateInfo::image */

   VkImageViewType view_type;
   VkFormat format;
   enum pipe_format pformat;
   VkComponentMapping components;
   VkImageSubresourceRange subresourceRange;

   struct pipe_surface *surface; /* have we created a pipe surface for this? */
};

struct val_subpass_attachment {
   uint32_t         attachment;
   VkImageLayout    layout;
   bool             in_render_loop;
};

struct val_subpass {
   uint32_t                                     attachment_count;
   struct val_subpass_attachment *             attachments;

   uint32_t                                     input_count;
   uint32_t                                     color_count;
   struct val_subpass_attachment *              input_attachments;
   struct val_subpass_attachment *              color_attachments;
   struct val_subpass_attachment *              resolve_attachments;
   struct val_subpass_attachment *              depth_stencil_attachment;
   struct val_subpass_attachment *              ds_resolve_attachment;

   /** Subpass has at least one color resolve attachment */
   bool                                         has_color_resolve;

   /** Subpass has at least one color attachment */
   bool                                         has_color_att;

   VkSampleCountFlagBits                        max_sample_count;
};

struct val_render_pass_attachment {
   VkFormat                                     format;
   uint32_t                                     samples;
   VkAttachmentLoadOp                           load_op;
   VkAttachmentLoadOp                           stencil_load_op;
   VkImageLayout                                initial_layout;
   VkImageLayout                                final_layout;

   /* The subpass id in which the attachment will be used first/last. */
   uint32_t                                     first_subpass_idx;
   uint32_t                                     last_subpass_idx;
};

struct val_render_pass {
   struct vk_object_base                        base;
   uint32_t                                     attachment_count;
   uint32_t                                     subpass_count;
   struct val_subpass_attachment *              subpass_attachments;
   struct val_render_pass_attachment *          attachments;
   struct val_subpass                           subpasses[0];
};

struct val_sampler {
   struct vk_object_base base;
   VkSamplerCreateInfo create_info;
   uint32_t state[4];
};

struct val_framebuffer {
   struct vk_object_base                        base;
   uint32_t                                     width;
   uint32_t                                     height;
   uint32_t                                     layers;

   uint32_t                                     attachment_count;
   struct val_image_view *                      attachments[0];
};

struct val_descriptor_set_binding_layout {
   uint16_t descriptor_index;
   /* Number of array elements in this binding */
   VkDescriptorType type;
   uint16_t array_size;
   bool valid;

   int16_t dynamic_index;
   struct {
      int16_t const_buffer_index;
      int16_t shader_buffer_index;
      int16_t sampler_index;
      int16_t sampler_view_index;
      int16_t image_index;
   } stage[MESA_SHADER_STAGES];

   /* Immutable samplers (or NULL if no immutable samplers) */
   struct val_sampler **immutable_samplers;
};

struct val_descriptor_set_layout {
   struct vk_object_base base;
   /* Number of bindings in this descriptor set */
   uint16_t binding_count;

   /* Total size of the descriptor set with room for all array entries */
   uint16_t size;

   /* Shader stages affected by this descriptor set */
   uint16_t shader_stages;

   struct {
      uint16_t const_buffer_count;
      uint16_t shader_buffer_count;
      uint16_t sampler_count;
      uint16_t sampler_view_count;
      uint16_t image_count;
   } stage[MESA_SHADER_STAGES];

   /* Number of dynamic offsets used by this descriptor set */
   uint16_t dynamic_offset_count;

   /* Bindings in this descriptor set */
   struct val_descriptor_set_binding_layout binding[0];
};

struct val_descriptor {
   VkDescriptorType type;

   union {
      struct {
         struct val_image_view *image_view;
         struct val_sampler *sampler;
      };
      struct {
         uint64_t offset;
         uint64_t range;
         struct val_buffer *buffer;
      } buf;
      struct val_buffer_view *buffer_view;
   };
};

struct val_descriptor_set {
   struct vk_object_base base;
   const struct val_descriptor_set_layout *layout;
   struct list_head link;
   struct val_descriptor descriptors[0];
};

struct val_descriptor_pool {
   struct vk_object_base base;
   VkDescriptorPoolCreateFlags flags;
   uint32_t max_sets;

   struct list_head sets;
};

VkResult
val_descriptor_set_create(struct val_device *device,
                          const struct val_descriptor_set_layout *layout,
                          struct val_descriptor_set **out_set);

void
val_descriptor_set_destroy(struct val_device *device,
                           struct val_descriptor_set *set);

struct val_pipeline_layout {
   struct vk_object_base base;
   struct {
      struct val_descriptor_set_layout *layout;
      uint32_t dynamic_offset_start;
   } set[MAX_SETS];

   uint32_t num_sets;
   uint32_t push_constant_size;
   struct {
      bool has_dynamic_offsets;
   } stage[MESA_SHADER_STAGES];
};

struct val_pipeline {
   struct vk_object_base base;
   struct val_device *                          device;
   struct val_pipeline_layout *                 layout;

   bool is_compute_pipeline;
   bool force_min_sample;
   nir_shader *pipeline_nir[MESA_SHADER_STAGES];
   void *shader_cso[PIPE_SHADER_TYPES];
   VkGraphicsPipelineCreateInfo graphics_create_info;
   VkComputePipelineCreateInfo compute_create_info;
};

struct val_event {
   struct vk_object_base base;
   uint64_t event_storage;
};

struct val_fence {
   struct vk_object_base base;
   bool signaled;
   struct pipe_fence_handle *handle;
};

struct val_semaphore {
   struct vk_object_base base;
   bool dummy;
};

struct val_buffer {
   struct vk_object_base base;
   struct val_device *                          device;
   VkDeviceSize                                 size;

   VkBufferUsageFlags                           usage;
   VkDeviceSize                                 offset;

   struct pipe_resource *bo;
   uint64_t total_size;
};

struct val_buffer_view {
   struct vk_object_base base;
   VkFormat format;
   enum pipe_format pformat;
   struct val_buffer *buffer;
   uint32_t offset;
   uint64_t range;
};

struct val_query_pool {
   struct vk_object_base base;
   VkQueryType type;
   uint32_t count;
   enum pipe_query_type base_type;
   struct pipe_query *queries[0];
};

struct val_cmd_pool {
   struct vk_object_base                        base;
   VkAllocationCallbacks                        alloc;
   struct list_head                             cmd_buffers;
   struct list_head                             free_cmd_buffers;
};


enum val_cmd_buffer_status {
   VAL_CMD_BUFFER_STATUS_INVALID,
   VAL_CMD_BUFFER_STATUS_INITIAL,
   VAL_CMD_BUFFER_STATUS_RECORDING,
   VAL_CMD_BUFFER_STATUS_EXECUTABLE,
   VAL_CMD_BUFFER_STATUS_PENDING,
};

struct val_cmd_buffer {
   struct vk_object_base base;

   struct val_device *                          device;

   VkCommandBufferLevel                         level;
   enum val_cmd_buffer_status status;
   struct val_cmd_pool *                        pool;
   struct list_head                             pool_link;

   struct list_head                             cmds;

   uint8_t push_constants[MAX_PUSH_CONSTANTS_SIZE];
};

/* in same order and buffer building commands in spec. */
enum val_cmds {
   VAL_CMD_BIND_PIPELINE,
   VAL_CMD_SET_VIEWPORT,
   VAL_CMD_SET_SCISSOR,
   VAL_CMD_SET_LINE_WIDTH,
   VAL_CMD_SET_DEPTH_BIAS,
   VAL_CMD_SET_BLEND_CONSTANTS,
   VAL_CMD_SET_DEPTH_BOUNDS,
   VAL_CMD_SET_STENCIL_COMPARE_MASK,
   VAL_CMD_SET_STENCIL_WRITE_MASK,
   VAL_CMD_SET_STENCIL_REFERENCE,
   VAL_CMD_BIND_DESCRIPTOR_SETS,
   VAL_CMD_BIND_INDEX_BUFFER,
   VAL_CMD_BIND_VERTEX_BUFFERS,
   VAL_CMD_DRAW,
   VAL_CMD_DRAW_INDEXED,
   VAL_CMD_DRAW_INDIRECT,
   VAL_CMD_DRAW_INDEXED_INDIRECT,
   VAL_CMD_DISPATCH,
   VAL_CMD_DISPATCH_INDIRECT,
   VAL_CMD_COPY_BUFFER,
   VAL_CMD_COPY_IMAGE,
   VAL_CMD_BLIT_IMAGE,
   VAL_CMD_COPY_BUFFER_TO_IMAGE,
   VAL_CMD_COPY_IMAGE_TO_BUFFER,
   VAL_CMD_UPDATE_BUFFER,
   VAL_CMD_FILL_BUFFER,
   VAL_CMD_CLEAR_COLOR_IMAGE,
   VAL_CMD_CLEAR_DEPTH_STENCIL_IMAGE,
   VAL_CMD_CLEAR_ATTACHMENTS,
   VAL_CMD_RESOLVE_IMAGE,
   VAL_CMD_SET_EVENT,
   VAL_CMD_RESET_EVENT,
   VAL_CMD_WAIT_EVENTS,
   VAL_CMD_PIPELINE_BARRIER,
   VAL_CMD_BEGIN_QUERY,
   VAL_CMD_END_QUERY,
   VAL_CMD_RESET_QUERY_POOL,
   VAL_CMD_WRITE_TIMESTAMP,
   VAL_CMD_COPY_QUERY_POOL_RESULTS,
   VAL_CMD_PUSH_CONSTANTS,
   VAL_CMD_BEGIN_RENDER_PASS,
   VAL_CMD_NEXT_SUBPASS,
   VAL_CMD_END_RENDER_PASS,
   VAL_CMD_EXECUTE_COMMANDS,
};

struct val_cmd_bind_pipeline {
   VkPipelineBindPoint bind_point;
   struct val_pipeline *pipeline;
};

struct val_cmd_set_viewport {
   uint32_t first_viewport;
   uint32_t viewport_count;
   VkViewport viewports[16];
};

struct val_cmd_set_scissor {
   uint32_t first_scissor;
   uint32_t scissor_count;
   VkRect2D scissors[16];
};

struct val_cmd_set_line_width {
   float line_width;
};

struct val_cmd_set_depth_bias {
   float constant_factor;
   float clamp;
   float slope_factor;
};

struct val_cmd_set_blend_constants {
   float blend_constants[4];
};

struct val_cmd_set_depth_bounds {
   float min_depth;
   float max_depth;
};

struct val_cmd_set_stencil_vals {
   VkStencilFaceFlags face_mask;
   uint32_t value;
};

struct val_cmd_bind_descriptor_sets {
   VkPipelineBindPoint bind_point;
   struct val_pipeline_layout *layout;
   uint32_t first;
   uint32_t count;
   struct val_descriptor_set **sets;
   uint32_t dynamic_offset_count;
   const uint32_t *dynamic_offsets;
};

struct val_cmd_bind_index_buffer {
   const struct val_buffer *buffer;
   VkDeviceSize offset;
   VkIndexType index_type;
};

struct val_cmd_bind_vertex_buffers {
   uint32_t first;
   uint32_t binding_count;
   struct val_buffer **buffers;
   const VkDeviceSize *offsets;
};

struct val_cmd_draw {
   uint32_t vertex_count;
   uint32_t instance_count;
   uint32_t first_vertex;
   uint32_t first_instance;
};

struct val_cmd_draw_indexed {
   uint32_t index_count;
   uint32_t instance_count;
   uint32_t first_index;
   uint32_t vertex_offset;
   uint32_t first_instance;
};

struct val_cmd_draw_indirect {
   VkDeviceSize offset;
   struct val_buffer *buffer;
   uint32_t draw_count;
   uint32_t stride;
};

struct val_cmd_dispatch {
   uint32_t x;
   uint32_t y;
   uint32_t z;
};

struct val_cmd_dispatch_indirect {
   const struct val_buffer *buffer;
   VkDeviceSize offset;
};

struct val_cmd_copy_buffer {
   struct val_buffer *src;
   struct val_buffer *dst;
   uint32_t region_count;
   const VkBufferCopy *regions;
};

struct val_cmd_copy_image {
   struct val_image *src;
   struct val_image *dst;
   VkImageLayout src_layout;
   VkImageLayout dst_layout;
   uint32_t region_count;
   const VkImageCopy *regions;
};

struct val_cmd_blit_image {
  struct val_image *src;
  struct val_image *dst;
  VkImageLayout src_layout;
  VkImageLayout dst_layout;
  uint32_t region_count;
  const VkImageBlit *regions;
  VkFilter filter;
};

struct val_cmd_copy_buffer_to_image {
   struct val_buffer *src;
   struct val_image *dst;
   VkImageLayout dst_layout;
   uint32_t region_count;
   const VkBufferImageCopy *regions;
};

struct val_cmd_copy_image_to_buffer {
   struct val_image *src;
   struct val_buffer *dst;
   VkImageLayout src_layout;
   uint32_t region_count;
   const VkBufferImageCopy *regions;
};

struct val_cmd_update_buffer {
   struct val_buffer *buffer;
   VkDeviceSize offset;
   VkDeviceSize data_size;
   char data[0];
};

struct val_cmd_fill_buffer {
   struct val_buffer *buffer;
   VkDeviceSize offset;
   VkDeviceSize fill_size;
   uint32_t data;
};

struct val_cmd_clear_color_image {
   struct val_image *image;
   VkImageLayout layout;
   VkClearColorValue clear_val;
   uint32_t range_count;
   VkImageSubresourceRange *ranges;
};

struct val_cmd_clear_ds_image {
   struct val_image *image;
   VkImageLayout layout;
   VkClearDepthStencilValue clear_val;
   uint32_t range_count;
   VkImageSubresourceRange *ranges;
};

struct val_cmd_clear_attachments {
   uint32_t attachment_count;
   VkClearAttachment *attachments;
   uint32_t rect_count;
   VkClearRect *rects;
};

struct val_cmd_resolve_image {
   struct val_image *src;
   struct val_image *dst;
   VkImageLayout src_layout;
   VkImageLayout dst_layout;
   uint32_t region_count;
   VkImageResolve *regions;
};

struct val_cmd_event_set {
   struct val_event *event;
   bool value;
   bool flush;
};

struct val_cmd_wait_events {
   uint32_t event_count;
   struct val_event **events;
   VkPipelineStageFlags src_stage_mask;
   VkPipelineStageFlags dst_stage_mask;
   uint32_t memory_barrier_count;
   VkMemoryBarrier *memory_barriers;
   uint32_t buffer_memory_barrier_count;
   VkBufferMemoryBarrier *buffer_memory_barriers;
   uint32_t image_memory_barrier_count;
   VkImageMemoryBarrier *image_memory_barriers;
};

struct val_cmd_pipeline_barrier {
   VkPipelineStageFlags src_stage_mask;
   VkPipelineStageFlags dst_stage_mask;
   bool by_region;
   uint32_t memory_barrier_count;
   VkMemoryBarrier *memory_barriers;
   uint32_t buffer_memory_barrier_count;
   VkBufferMemoryBarrier *buffer_memory_barriers;
   uint32_t image_memory_barrier_count;
   VkImageMemoryBarrier *image_memory_barriers;
};

struct val_cmd_query_cmd {
   struct val_query_pool *pool;
   uint32_t query;
   uint32_t index;
   bool precise;
   bool flush;
};

struct val_cmd_copy_query_pool_results {
   struct val_query_pool *pool;
   uint32_t first_query;
   uint32_t query_count;
   struct val_buffer *dst;
   VkDeviceSize dst_offset;
   VkDeviceSize stride;
   VkQueryResultFlags flags;
};

struct val_cmd_push_constants {
   VkShaderStageFlags stage;
   uint32_t offset;
   uint32_t size;
   uint32_t val[1];
};

struct val_attachment_state {
   VkImageAspectFlags pending_clear_aspects;
   VkClearValue clear_value;
};

struct val_cmd_begin_render_pass {
   struct val_framebuffer *framebuffer;
   struct val_render_pass *render_pass;
   VkRect2D render_area;
   struct val_attachment_state *attachments;
};

struct val_cmd_next_subpass {
   VkSubpassContents contents;
};

struct val_cmd_execute_commands {
   uint32_t command_buffer_count;
   struct val_cmd_buffer *cmd_buffers[0];
};

struct val_cmd_buffer_entry {
   struct list_head cmd_link;
   uint32_t cmd_type;
   union {
      struct val_cmd_bind_pipeline pipeline;
      struct val_cmd_set_viewport set_viewport;
      struct val_cmd_set_scissor set_scissor;
      struct val_cmd_set_line_width set_line_width;
      struct val_cmd_set_depth_bias set_depth_bias;
      struct val_cmd_set_blend_constants set_blend_constants;
      struct val_cmd_set_depth_bounds set_depth_bounds;
      struct val_cmd_set_stencil_vals stencil_vals;
      struct val_cmd_bind_descriptor_sets descriptor_sets;
      struct val_cmd_bind_vertex_buffers vertex_buffers;
      struct val_cmd_bind_index_buffer index_buffer;
      struct val_cmd_draw draw;
      struct val_cmd_draw_indexed draw_indexed;
      struct val_cmd_draw_indirect draw_indirect;
      struct val_cmd_dispatch dispatch;
      struct val_cmd_dispatch_indirect dispatch_indirect;
      struct val_cmd_copy_buffer copy_buffer;
      struct val_cmd_copy_image copy_image;
      struct val_cmd_blit_image blit_image;
      struct val_cmd_copy_buffer_to_image buffer_to_img;
      struct val_cmd_copy_image_to_buffer img_to_buffer;
      struct val_cmd_update_buffer update_buffer;
      struct val_cmd_fill_buffer fill_buffer;
      struct val_cmd_clear_color_image clear_color_image;
      struct val_cmd_clear_ds_image clear_ds_image;
      struct val_cmd_clear_attachments clear_attachments;
      struct val_cmd_resolve_image resolve_image;
      struct val_cmd_event_set event_set;
      struct val_cmd_wait_events wait_events;
      struct val_cmd_pipeline_barrier pipeline_barrier;
      struct val_cmd_query_cmd query;
      struct val_cmd_copy_query_pool_results copy_query_pool_results;
      struct val_cmd_push_constants push_constants;
      struct val_cmd_begin_render_pass begin_render_pass;
      struct val_cmd_next_subpass next_subpass;
      struct val_cmd_execute_commands execute_commands;
   } u;
};

VkResult val_execute_cmds(struct val_device *device,
                          struct val_queue *queue,
                          struct val_fence *fence,
                          struct val_cmd_buffer *cmd_buffer);

enum pipe_format vk_format_to_pipe(VkFormat format);

static inline VkImageAspectFlags
vk_format_aspects(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_UNDEFINED:
      return 0;

   case VK_FORMAT_S8_UINT:
      return VK_IMAGE_ASPECT_STENCIL_BIT;

   case VK_FORMAT_D16_UNORM_S8_UINT:
   case VK_FORMAT_D24_UNORM_S8_UINT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_X8_D24_UNORM_PACK32:
   case VK_FORMAT_D32_SFLOAT:
      return VK_IMAGE_ASPECT_DEPTH_BIT;

   default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
   }
}

#ifdef __cplusplus
}
#endif

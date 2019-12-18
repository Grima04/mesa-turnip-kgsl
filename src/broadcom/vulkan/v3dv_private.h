/*
 * Copyright © 2019 Raspberry Pi
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * based in part on radv driver which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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
#ifndef V3DV_PRIVATE_H
#define V3DV_PRIVATE_H

#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#include <xf86drm.h>

#ifdef HAVE_VALGRIND
#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>
#define VG(x) x
#else
#define VG(x) ((void)0)
#endif

#include "common/v3d_device_info.h"
#include "common/v3d_limits.h"

#include "vk_debug_report.h"
#include "util/set.h"
#include "util/hash_table.h"
#include "util/xmlconfig.h"

#include "v3dv_entrypoints.h"
#include "v3dv_extensions.h"
#include "v3dv_bo.h"

/* FIXME: hooks for the packet definition functions. */
static inline void
pack_emit_reloc(void *cl, const void *reloc) {}

#define __gen_user_data struct v3dv_cl
#define __gen_address_type struct v3dv_cl_reloc
#define __gen_address_offset(reloc) (((reloc)->bo ? (reloc)->bo->offset : 0) + \
                                     (reloc)->offset)
#define __gen_emit_reloc cl_pack_emit_reloc
#define __gen_unpack_address(cl, s, e) __unpack_address(cl, s, e)
#include "v3dv_cl.h"

#include "vk_alloc.h"
#include "simulator/v3d_simulator.h"

/* FIXME: pipe_box from Gallium. Needed for some v3d_tiling.c functions.
 * In the future we might want to drop that depedency, but for now it is
 * good enough.
 */
#include "util/u_box.h"

/* A non-fatal assert.  Useful for debugging. */
#ifdef DEBUG
#define v3dv_assert(x) ({ \
   if (unlikely(!(x))) \
      fprintf(stderr, "%s:%d ASSERT: %s", __FILE__, __LINE__, #x); \
})
#else
#define v3dv_assert(x)
#endif

struct v3dv_instance;

#ifdef USE_V3D_SIMULATOR
#define using_v3d_simulator true
#else
#define using_v3d_simulator false
#endif

struct v3d_simulator_file;

struct v3dv_physical_device {
   VK_LOADER_DATA _loader_data;

   struct v3dv_instance *instance;

   struct v3dv_device_extension_table supported_extensions;
   struct v3dv_physical_device_dispatch_table dispatch;

   char path[20];
   char *name;
   int32_t local_fd;
   int32_t master_fd;
   uint8_t pipeline_cache_uuid[VK_UUID_SIZE];

   VkPhysicalDeviceMemoryProperties memory;

   struct v3d_device_info devinfo;

   struct v3d_simulator_file *sim_file;
};

struct v3dv_app_info {
   const char *app_name;
   uint32_t app_version;
   const char *engine_name;
   uint32_t engine_version;
   uint32_t api_version;
};

struct v3dv_instance {
   VK_LOADER_DATA _loader_data;

   VkAllocationCallbacks alloc;

   struct v3dv_app_info app_info;

   struct v3dv_instance_extension_table enabled_extensions;
   struct v3dv_instance_dispatch_table dispatch;
   struct v3dv_device_dispatch_table device_dispatch;

   int physicalDeviceCount;
   struct v3dv_physical_device physicalDevice;

   struct vk_debug_report_instance debug_report_callbacks;
};

struct v3dv_queue {
   VK_LOADER_DATA _loader_data;

   struct v3dv_device *device;

   VkDeviceQueueCreateFlags flags;

   /* FIXME: stub */
};

struct v3dv_device {
   VK_LOADER_DATA _loader_data;

   VkAllocationCallbacks alloc;

   struct v3dv_instance *instance;

   struct v3dv_device_extension_table enabled_extensions;
   struct v3dv_device_dispatch_table dispatch;

   int32_t fd;
   struct v3d_device_info devinfo;
   struct v3dv_queue queue;

   /* FIXME: stub */
};

struct v3dv_device_memory {
   struct v3dv_bo *bo;
   const VkMemoryType *type;
};

#define V3D_OUTPUT_IMAGE_FORMAT_NO 255

struct v3dv_format {
   bool supported;

   /* One of V3D33_OUTPUT_IMAGE_FORMAT_*, or OUTPUT_IMAGE_FORMAT_NO */
   uint8_t rt_type;

   /* One of V3D33_TEXTURE_DATA_FORMAT_*. */
   uint8_t tex_type;

   /* Swizzle to apply to the RGBA shader output for storing to the tile
    * buffer, to the RGBA tile buffer to produce shader input (for
    * blending), and for turning the rgba8888 texture sampler return
    * value into shader rgba values.
    */
   uint8_t swizzle[4];

   /* Whether the return value is 16F/I/UI or 32F/I/UI. */
   uint8_t return_size;
};

/**
 * Tiling mode enum used for v3d_resource.c, which maps directly to the Memory
 * Format field of render target and Z/Stencil config.
 */
enum v3d_tiling_mode {
   /* Untiled resources.  Not valid as texture inputs. */
   VC5_TILING_RASTER,

   /* Single line of u-tiles. */
   VC5_TILING_LINEARTILE,

   /* Departure from standard 4-UIF block column format. */
   VC5_TILING_UBLINEAR_1_COLUMN,

   /* Departure from standard 4-UIF block column format. */
   VC5_TILING_UBLINEAR_2_COLUMN,

   /* Normal tiling format: grouped in 4x4 UIFblocks, each of which is
    * split 2x2 into utiles.
    */
   VC5_TILING_UIF_NO_XOR,

   /* Normal tiling format: grouped in 4x4 UIFblocks, each of which is
    * split 2x2 into utiles.
    */
   VC5_TILING_UIF_XOR,
};

struct v3d_resource_slice {
   uint32_t offset;
   uint32_t stride;
   uint32_t padded_height;
   /* Size of a single pane of the slice.  For 3D textures, there will be
    * a number of panes equal to the minified, power-of-two-aligned
    * depth.
    */
   uint32_t size;
   uint8_t ub_pad;
   enum v3d_tiling_mode tiling;
   uint32_t padded_height_of_output_image_in_uif_blocks;
};

struct v3dv_image {
   VkImageType type;
   VkImageAspectFlags aspects;

   VkExtent3D extent;
   uint32_t levels;
   uint32_t array_size;
   uint32_t samples;
   VkImageUsageFlags usage;
   VkImageCreateFlags create_flags;
   VkImageTiling tiling;

   VkFormat vk_format;
   const struct v3dv_format *format;

   uint32_t cpp;

   uint64_t drm_format_mod;
   bool tiled;

   struct v3d_resource_slice slices[V3D_MAX_MIP_LEVELS];
   uint32_t size; /* Total size in bytes */
   uint32_t cube_map_stride;
   uint32_t alignment;

   struct v3dv_device_memory *mem;
   VkDeviceSize mem_offset;
};

struct v3dv_image_view {
   const struct v3dv_image *image;
   VkImageAspectFlags aspects;
   VkExtent3D extent;

   VkFormat vk_format;
   const struct v3dv_format *format;
   bool swap_rb;
   enum v3d_tiling_mode tiling;
   uint32_t internal_bpp;
   uint32_t internal_type;

   uint32_t base_level;
   uint32_t first_layer;
   uint32_t last_layer;
   uint32_t offset;
};

uint32_t v3dv_layer_offset(const struct v3dv_image *image, uint32_t level, uint32_t layer);

struct v3dv_buffer {
   VkDeviceSize size;
   VkBufferUsageFlags usage;
   uint32_t alignment;

   struct v3dv_device_memory *mem;
   VkDeviceSize mem_offset;
};

struct v3dv_subpass_attachment {
   uint32_t attachment;
   VkImageLayout layout;
};

struct v3dv_subpass {
   uint32_t input_count;
   struct v3dv_subpass_attachment *input_attachments;

   uint32_t color_count;
   struct v3dv_subpass_attachment *color_attachments;
   struct v3dv_subpass_attachment *resolve_attachments;

   struct v3dv_subpass_attachment ds_attachment;
};

struct v3dv_render_pass_attachment {
   VkAttachmentDescription desc;
};

struct v3dv_render_pass {
   uint32_t attachment_count;
   struct v3dv_render_pass_attachment *attachments;

   uint32_t subpass_count;
   struct v3dv_subpass *subpasses;

   struct v3dv_subpass_attachment *subpass_attachments;
};

struct v3dv_framebuffer {
   uint32_t width;
   uint32_t height;
   uint32_t layers;

   uint32_t internal_bpp;
   uint32_t tile_width;
   uint32_t tile_height;
   uint32_t draw_tiles_x;
   uint32_t draw_tiles_y;
   uint32_t supertile_width;
   uint32_t supertile_height;
   uint32_t frame_width_in_supertiles;
   uint32_t frame_height_in_supertiles;

   uint32_t attachment_count;
   struct v3dv_image_view *attachments[0];
};

struct v3dv_cmd_pool {
   VkAllocationCallbacks alloc;
   struct list_head cmd_buffers;
};

enum v3dv_cmd_buffer_status {
   V3DV_CMD_BUFFER_STATUS_NEW           = 0,
   V3DV_CMD_BUFFER_STATUS_INITIALIZED   = 1,
   V3DV_CMD_BUFFER_STATUS_RECORDING     = 2,
   V3DV_CMD_BUFFER_STATUS_EXECUTABLE    = 3
};

union v3dv_clear_value {
   uint32_t color[4];
   float z;
   uint8_t s;
};

struct v3dv_cmd_buffer_attachment_state {
   union v3dv_clear_value clear_value;
};

struct v3dv_cmd_buffer_state {
   const struct v3dv_render_pass *pass;
   const struct v3dv_framebuffer *framebuffer;
   VkRect2D render_area;

   uint32_t clear_value_count;
   VkClearValue *clear_values;

   /* Subpass state */
   uint32_t subpass_idx;
   struct v3dv_cmd_buffer_attachment_state attachments[6]; /* 4 color + D + S */
};

struct v3dv_cmd_buffer {
   VK_LOADER_DATA _loader_data;

   struct v3dv_device *device;

   struct v3dv_cmd_pool *pool;
   struct list_head pool_link;

   VkCommandBufferUsageFlags usage_flags;
   VkCommandBufferLevel level;

   struct v3dv_cl bcl;
   struct v3dv_cl rcl;
   struct v3dv_cl indirect;

   enum v3dv_cmd_buffer_status status;

   struct v3dv_cmd_buffer_state state;

   /* Set of all BOs referenced by the job. This will be used for making
    * the list of BOs that the kernel will need to have paged in to
    * execute our job.
    */
   struct set *bos;
   uint32_t bo_count;

   struct v3dv_bo *tile_alloc;
   struct v3dv_bo *tile_state;
};

void v3dv_cmd_buffer_add_bo(struct v3dv_cmd_buffer *cmd_buffer, struct v3dv_bo *bo);

struct v3dv_shader_module {
   unsigned char sha1[20];
   uint32_t size;
   char data[0];
};

uint32_t v3dv_physical_device_api_version(struct v3dv_physical_device *dev);

int v3dv_get_instance_entrypoint_index(const char *name);
int v3dv_get_device_entrypoint_index(const char *name);
int v3dv_get_physical_device_entrypoint_index(const char *name);

const char *v3dv_get_instance_entry_name(int index);
const char *v3dv_get_physical_device_entry_name(int index);
const char *v3dv_get_device_entry_name(int index);

bool
v3dv_instance_entrypoint_is_enabled(int index, uint32_t core_version,
                                    const struct v3dv_instance_extension_table *instance);
bool
v3dv_physical_device_entrypoint_is_enabled(int index, uint32_t core_version,
                                           const struct v3dv_instance_extension_table *instance);
bool
v3dv_device_entrypoint_is_enabled(int index, uint32_t core_version,
                                  const struct v3dv_instance_extension_table *instance,
                                  const struct v3dv_device_extension_table *device);

void *v3dv_lookup_entrypoint(const struct v3d_device_info *devinfo,
                             const char *name);

#define v3dv_printflike(a, b) __attribute__((__format__(__printf__, a, b)))

VkResult __vk_errorf(struct v3dv_instance *instance, VkResult error,
                     const char *file, int line,
                     const char *format, ...);

#define vk_error(instance, error) __vk_errorf(instance, error, __FILE__, __LINE__, NULL);
#define vk_errorf(instance, error, format, ...) __vk_errorf(instance, error, __FILE__, __LINE__, format, ## __VA_ARGS__);

void v3dv_loge(const char *format, ...) v3dv_printflike(1, 2);
void v3dv_loge_v(const char *format, va_list va);

const struct v3dv_format *v3dv_get_format(VkFormat);
void v3dv_get_internal_type_bpp_for_output_format(uint32_t format, uint32_t *type, uint32_t *bpp);

uint32_t v3d_utile_width(int cpp);
uint32_t v3d_utile_height(int cpp);

void v3d_load_tiled_image(void *dst, uint32_t dst_stride,
                          void *src, uint32_t src_stride,
                          enum v3d_tiling_mode tiling_format,
                          int cpp, uint32_t image_h,
                          const struct pipe_box *box);

void v3d_store_tiled_image(void *dst, uint32_t dst_stride,
                           void *src, uint32_t src_stride,
                           enum v3d_tiling_mode tiling_format,
                           int cpp, uint32_t image_h,
                           const struct pipe_box *box);

#define V3DV_DEFINE_HANDLE_CASTS(__v3dv_type, __VkType)   \
                                                        \
   static inline struct __v3dv_type *                    \
   __v3dv_type ## _from_handle(__VkType _handle)         \
   {                                                    \
      return (struct __v3dv_type *) _handle;             \
   }                                                    \
                                                        \
   static inline __VkType                               \
   __v3dv_type ## _to_handle(struct __v3dv_type *_obj)    \
   {                                                    \
      return (__VkType) _obj;                           \
   }

#define V3DV_DEFINE_NONDISP_HANDLE_CASTS(__v3dv_type, __VkType)              \
                                                                           \
   static inline struct __v3dv_type *                                       \
   __v3dv_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __v3dv_type *)(uintptr_t) _handle;                     \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __v3dv_type ## _to_handle(struct __v3dv_type *_obj)                       \
   {                                                                       \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

#define V3DV_FROM_HANDLE(__v3dv_type, __name, __handle)			\
   struct __v3dv_type *__name = __v3dv_type ## _from_handle(__handle)

V3DV_DEFINE_HANDLE_CASTS(v3dv_cmd_buffer, VkCommandBuffer)
V3DV_DEFINE_HANDLE_CASTS(v3dv_device, VkDevice)
V3DV_DEFINE_HANDLE_CASTS(v3dv_instance, VkInstance)
V3DV_DEFINE_HANDLE_CASTS(v3dv_physical_device, VkPhysicalDevice)
V3DV_DEFINE_HANDLE_CASTS(v3dv_queue, VkQueue)

V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_cmd_pool, VkCommandPool)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_buffer, VkBuffer)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_device_memory, VkDeviceMemory)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_framebuffer, VkFramebuffer)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_image, VkImage)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_image_view, VkImageView)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_render_pass, VkRenderPass)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_shader_module, VkShaderModule)

/* This is defined as a macro so that it works for both
 * VkImageSubresourceRange and VkImageSubresourceLayers
 */
#define v3dv_layer_count(_image, _range) \
   ((_range)->layerCount == VK_REMAINING_ARRAY_LAYERS ? \
    (_image)->array_size - (_range)->baseArrayLayer : (_range)->layerCount)

static inline int
v3dv_ioctl(int fd, unsigned long request, void *arg)
{
   if (using_v3d_simulator)
      return v3d_simulator_ioctl(fd, request, arg);
   else
      return drmIoctl(fd, request, arg);
}

#endif /* V3DV_PRIVATE_H */

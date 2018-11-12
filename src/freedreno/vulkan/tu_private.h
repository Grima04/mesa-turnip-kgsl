/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
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

#ifndef TU_PRIVATE_H
#define TU_PRIVATE_H

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_VALGRIND
#include <memcheck.h>
#include <valgrind.h>
#define VG(x) x
#else
#define VG(x)
#endif

#include "c11/threads.h"
#include "compiler/shader_enums.h"
#include "main/macros.h"
#include "util/list.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_debug_report.h"

#include "tu_descriptor_set.h"
#include "tu_extensions.h"

/* Pre-declarations needed for WSI entrypoints */
struct wl_surface;
struct wl_display;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_window_t;

#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_intel.h>

#include "drm/freedreno_ringbuffer.h"

#include "tu_entrypoints.h"

#define MAX_VBS 32
#define MAX_VERTEX_ATTRIBS 32
#define MAX_RTS 8
#define MAX_VIEWPORTS 16
#define MAX_SCISSORS 16
#define MAX_DISCARD_RECTANGLES 4
#define MAX_PUSH_CONSTANTS_SIZE 128
#define MAX_PUSH_DESCRIPTORS 32
#define MAX_DYNAMIC_UNIFORM_BUFFERS 16
#define MAX_DYNAMIC_STORAGE_BUFFERS 8
#define MAX_DYNAMIC_BUFFERS                                                    \
   (MAX_DYNAMIC_UNIFORM_BUFFERS + MAX_DYNAMIC_STORAGE_BUFFERS)
#define MAX_SAMPLES_LOG2 4
#define NUM_META_FS_KEYS 13
#define TU_MAX_DRM_DEVICES 8
#define MAX_VIEWS 8

#define NUM_DEPTH_CLEAR_PIPELINES 3

/*
 * This is the point we switch from using CP to compute shader
 * for certain buffer operations.
 */
#define TU_BUFFER_OPS_CS_THRESHOLD 4096

enum tu_mem_heap
{
   TU_MEM_HEAP_VRAM,
   TU_MEM_HEAP_VRAM_CPU_ACCESS,
   TU_MEM_HEAP_GTT,
   TU_MEM_HEAP_COUNT
};

enum tu_mem_type
{
   TU_MEM_TYPE_VRAM,
   TU_MEM_TYPE_GTT_WRITE_COMBINE,
   TU_MEM_TYPE_VRAM_CPU_ACCESS,
   TU_MEM_TYPE_GTT_CACHED,
   TU_MEM_TYPE_COUNT
};

#define tu_printflike(a, b) __attribute__((__format__(__printf__, a, b)))

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

static inline uint32_t
align_u32_npot(uint32_t v, uint32_t a)
{
   return (v + a - 1) / a * a;
}

static inline uint64_t
align_u64(uint64_t v, uint64_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

static inline int32_t
align_i32(int32_t v, int32_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

/** Alignment must be a power of 2. */
static inline bool
tu_is_aligned(uintmax_t n, uintmax_t a)
{
   assert(a == (a & -a));
   return (n & (a - 1)) == 0;
}

static inline uint32_t
round_up_u32(uint32_t v, uint32_t a)
{
   return (v + a - 1) / a;
}

static inline uint64_t
round_up_u64(uint64_t v, uint64_t a)
{
   return (v + a - 1) / a;
}

static inline uint32_t
tu_minify(uint32_t n, uint32_t levels)
{
   if (unlikely(n == 0))
      return 0;
   else
      return MAX2(n >> levels, 1);
}
static inline float
tu_clamp_f(float f, float min, float max)
{
   assert(min < max);

   if (f > max)
      return max;
   else if (f < min)
      return min;
   else
      return f;
}

static inline bool
tu_clear_mask(uint32_t *inout_mask, uint32_t clear_mask)
{
   if (*inout_mask & clear_mask) {
      *inout_mask &= ~clear_mask;
      return true;
   } else {
      return false;
   }
}

#define for_each_bit(b, dword)                                                 \
   for (uint32_t __dword = (dword); (b) = __builtin_ffs(__dword) - 1, __dword; \
        __dword &= ~(1 << (b)))

#define typed_memcpy(dest, src, count)                                         \
   ({                                                                          \
      STATIC_ASSERT(sizeof(*src) == sizeof(*dest));                            \
      memcpy((dest), (src), (count) * sizeof(*(src)));                         \
   })

/* Whenever we generate an error, pass it through this function. Useful for
 * debugging, where we can break on it. Only call at error site, not when
 * propagating errors. Might be useful to plug in a stack trace here.
 */

struct tu_instance;

VkResult
__vk_errorf(struct tu_instance *instance,
            VkResult error,
            const char *file,
            int line,
            const char *format,
            ...);

#define vk_error(instance, error)                                              \
   __vk_errorf(instance, error, __FILE__, __LINE__, NULL);
#define vk_errorf(instance, error, format, ...)                                \
   __vk_errorf(instance, error, __FILE__, __LINE__, format, ##__VA_ARGS__);

void
__tu_finishme(const char *file, int line, const char *format, ...)
  tu_printflike(3, 4);
void
tu_loge(const char *format, ...) tu_printflike(1, 2);
void
tu_loge_v(const char *format, va_list va);
void
tu_logi(const char *format, ...) tu_printflike(1, 2);
void
tu_logi_v(const char *format, va_list va);

/**
 * Print a FINISHME message, including its source location.
 */
#define tu_finishme(format, ...)                                              \
   do {                                                                        \
      static bool reported = false;                                            \
      if (!reported) {                                                         \
         __tu_finishme(__FILE__, __LINE__, format, ##__VA_ARGS__);            \
         reported = true;                                                      \
      }                                                                        \
   } while (0)

/* A non-fatal assert.  Useful for debugging. */
#ifdef DEBUG
#define tu_assert(x)                                                          \
   ({                                                                          \
      if (unlikely(!(x)))                                                      \
         fprintf(stderr, "%s:%d ASSERT: %s\n", __FILE__, __LINE__, #x);        \
   })
#else
#define tu_assert(x)
#endif

/* Suppress -Wunused in stub functions */
#define tu_use_args(...) __tu_use_args(0, ##__VA_ARGS__)
static inline void __tu_use_args(int ignore, ...) {}

#define tu_stub()                                                                 \
   do {                                                                        \
      tu_finishme("stub %s", __func__);                                       \
   } while (0)

void *
tu_lookup_entrypoint_unchecked(const char *name);
void *
tu_lookup_entrypoint_checked(
   const char *name,
   uint32_t core_version,
   const struct tu_instance_extension_table *instance,
   const struct tu_device_extension_table *device);

struct tu_physical_device
{
   VK_LOADER_DATA _loader_data;

   struct tu_instance *instance;

   char path[20];
   char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
   uint8_t driver_uuid[VK_UUID_SIZE];
   uint8_t device_uuid[VK_UUID_SIZE];
   uint8_t cache_uuid[VK_UUID_SIZE];

   int local_fd;
   int master_fd;

   struct fd_device *drm_device;
   unsigned gpu_id;
   uint32_t gmem_size;

   /* This is the drivers on-disk cache used as a fallback as opposed to
    * the pipeline cache defined by apps.
    */
   struct disk_cache *disk_cache;

   struct tu_device_extension_table supported_extensions;
};

enum tu_debug_flags
{
   TU_DEBUG_STARTUP = 1 << 0,
};

struct tu_instance
{
   VK_LOADER_DATA _loader_data;

   VkAllocationCallbacks alloc;

   uint32_t api_version;
   int physical_device_count;
   struct tu_physical_device physical_devices[TU_MAX_DRM_DEVICES];

   enum tu_debug_flags debug_flags;

   struct vk_debug_report_instance debug_report_callbacks;

   struct tu_instance_extension_table enabled_extensions;
};

bool
tu_instance_extension_supported(const char *name);
uint32_t
tu_physical_device_api_version(struct tu_physical_device *dev);
bool
tu_physical_device_extension_supported(struct tu_physical_device *dev,
                                       const char *name);

struct cache_entry;

struct tu_pipeline_cache
{
   struct tu_device *device;
   pthread_mutex_t mutex;

   uint32_t total_size;
   uint32_t table_size;
   uint32_t kernel_count;
   struct cache_entry **hash_table;
   bool modified;

   VkAllocationCallbacks alloc;
};

struct tu_pipeline_key
{
};

void
tu_pipeline_cache_init(struct tu_pipeline_cache *cache,
                       struct tu_device *device);
void
tu_pipeline_cache_finish(struct tu_pipeline_cache *cache);
void
tu_pipeline_cache_load(struct tu_pipeline_cache *cache,
                       const void *data,
                       size_t size);

struct tu_shader_variant;

bool
tu_create_shader_variants_from_pipeline_cache(
   struct tu_device *device,
   struct tu_pipeline_cache *cache,
   const unsigned char *sha1,
   struct tu_shader_variant **variants);

void
tu_pipeline_cache_insert_shaders(struct tu_device *device,
                                 struct tu_pipeline_cache *cache,
                                 const unsigned char *sha1,
                                 struct tu_shader_variant **variants,
                                 const void *const *codes,
                                 const unsigned *code_sizes);

struct tu_meta_state
{
   VkAllocationCallbacks alloc;

   struct tu_pipeline_cache cache;
};

/* queue types */
#define TU_QUEUE_GENERAL 0

#define TU_MAX_QUEUE_FAMILIES 1

struct tu_queue
{
   VK_LOADER_DATA _loader_data;
   struct tu_device *device;
   uint32_t queue_family_index;
   int queue_idx;
   VkDeviceQueueCreateFlags flags;
};

struct tu_bo_list
{
   unsigned capacity;
   pthread_mutex_t mutex;
};

struct tu_device
{
   VK_LOADER_DATA _loader_data;

   VkAllocationCallbacks alloc;

   struct tu_instance *instance;
   struct radeon_winsys *ws;

   struct tu_meta_state meta_state;

   struct tu_queue *queues[TU_MAX_QUEUE_FAMILIES];
   int queue_count[TU_MAX_QUEUE_FAMILIES];

   struct tu_physical_device *physical_device;

   /* Backup in-memory cache to be used if the app doesn't provide one */
   struct tu_pipeline_cache *mem_cache;

   struct list_head shader_slabs;
   mtx_t shader_slab_mutex;

   struct tu_device_extension_table enabled_extensions;

   /* Whether the driver uses a global BO list. */
   bool use_global_bo_list;

   struct tu_bo_list bo_list;
};

struct tu_bo
{
   uint32_t gem_handle;
   uint64_t size;
   uint64_t offset;
   uint64_t iova;
   void *map;
};

VkResult
tu_bo_init_new(struct tu_device *dev, struct tu_bo *bo, uint64_t size);
void
tu_bo_finish(struct tu_device *dev, struct tu_bo *bo);
VkResult
tu_bo_map(struct tu_device *dev, struct tu_bo *bo);

struct tu_device_memory
{
   struct tu_bo bo;
   VkDeviceSize size;

   /* for dedicated allocations */
   struct tu_image *image;
   struct tu_buffer *buffer;

   uint32_t type_index;
   void *map;
   void *user_ptr;
};

struct tu_descriptor_range
{
   uint64_t va;
   uint32_t size;
};

struct tu_descriptor_set
{
   const struct tu_descriptor_set_layout *layout;
   uint32_t size;

   struct radeon_winsys_bo *bo;
   uint64_t va;
   uint32_t *mapped_ptr;
   struct tu_descriptor_range *dynamic_descriptors;
};

struct tu_push_descriptor_set
{
   struct tu_descriptor_set set;
   uint32_t capacity;
};

struct tu_descriptor_pool_entry
{
   uint32_t offset;
   uint32_t size;
   struct tu_descriptor_set *set;
};

struct tu_descriptor_pool
{
   struct radeon_winsys_bo *bo;
   uint8_t *mapped_ptr;
   uint64_t current_offset;
   uint64_t size;

   uint8_t *host_memory_base;
   uint8_t *host_memory_ptr;
   uint8_t *host_memory_end;

   uint32_t entry_count;
   uint32_t max_entry_count;
   struct tu_descriptor_pool_entry entries[0];
};

struct tu_descriptor_update_template_entry
{
   VkDescriptorType descriptor_type;

   /* The number of descriptors to update */
   uint32_t descriptor_count;

   /* Into mapped_ptr or dynamic_descriptors, in units of the respective array
    */
   uint32_t dst_offset;

   /* In dwords. Not valid/used for dynamic descriptors */
   uint32_t dst_stride;

   uint32_t buffer_offset;

   /* Only valid for combined image samplers and samplers */
   uint16_t has_sampler;

   /* In bytes */
   size_t src_offset;
   size_t src_stride;

   /* For push descriptors */
   const uint32_t *immutable_samplers;
};

struct tu_descriptor_update_template
{
   uint32_t entry_count;
   VkPipelineBindPoint bind_point;
   struct tu_descriptor_update_template_entry entry[0];
};

struct tu_buffer
{
   VkDeviceSize size;

   VkBufferUsageFlags usage;
   VkBufferCreateFlags flags;
};

enum tu_dynamic_state_bits
{
   TU_DYNAMIC_VIEWPORT = 1 << 0,
   TU_DYNAMIC_SCISSOR = 1 << 1,
   TU_DYNAMIC_LINE_WIDTH = 1 << 2,
   TU_DYNAMIC_DEPTH_BIAS = 1 << 3,
   TU_DYNAMIC_BLEND_CONSTANTS = 1 << 4,
   TU_DYNAMIC_DEPTH_BOUNDS = 1 << 5,
   TU_DYNAMIC_STENCIL_COMPARE_MASK = 1 << 6,
   TU_DYNAMIC_STENCIL_WRITE_MASK = 1 << 7,
   TU_DYNAMIC_STENCIL_REFERENCE = 1 << 8,
   TU_DYNAMIC_DISCARD_RECTANGLE = 1 << 9,
   TU_DYNAMIC_ALL = (1 << 10) - 1,
};

struct tu_vertex_binding
{
   struct tu_buffer *buffer;
   VkDeviceSize offset;
};

struct tu_viewport_state
{
   uint32_t count;
   VkViewport viewports[MAX_VIEWPORTS];
};

struct tu_scissor_state
{
   uint32_t count;
   VkRect2D scissors[MAX_SCISSORS];
};

struct tu_discard_rectangle_state
{
   uint32_t count;
   VkRect2D rectangles[MAX_DISCARD_RECTANGLES];
};

struct tu_dynamic_state
{
   /**
    * Bitmask of (1 << VK_DYNAMIC_STATE_*).
    * Defines the set of saved dynamic state.
    */
   uint32_t mask;

   struct tu_viewport_state viewport;

   struct tu_scissor_state scissor;

   float line_width;

   struct
   {
      float bias;
      float clamp;
      float slope;
   } depth_bias;

   float blend_constants[4];

   struct
   {
      float min;
      float max;
   } depth_bounds;

   struct
   {
      uint32_t front;
      uint32_t back;
   } stencil_compare_mask;

   struct
   {
      uint32_t front;
      uint32_t back;
   } stencil_write_mask;

   struct
   {
      uint32_t front;
      uint32_t back;
   } stencil_reference;

   struct tu_discard_rectangle_state discard_rectangle;
};

extern const struct tu_dynamic_state default_dynamic_state;

const char *
tu_get_debug_option_name(int id);

const char *
tu_get_perftest_option_name(int id);

/**
 * Attachment state when recording a renderpass instance.
 *
 * The clear value is valid only if there exists a pending clear.
 */
struct tu_attachment_state
{
   VkImageAspectFlags pending_clear_aspects;
   uint32_t cleared_views;
   VkClearValue clear_value;
   VkImageLayout current_layout;
};

struct tu_descriptor_state
{
   struct tu_descriptor_set *sets[MAX_SETS];
   uint32_t dirty;
   uint32_t valid;
   struct tu_push_descriptor_set push_set;
   bool push_dirty;
   uint32_t dynamic_buffers[4 * MAX_DYNAMIC_BUFFERS];
};

struct tu_cmd_state
{
   /* Vertex descriptors */
   uint64_t vb_va;
   unsigned vb_size;

   struct tu_dynamic_state dynamic;

   /* Index buffer */
   struct tu_buffer *index_buffer;
   uint64_t index_offset;
   uint32_t index_type;
   uint32_t max_index_count;
   uint64_t index_va;
};

struct tu_cmd_pool
{
   VkAllocationCallbacks alloc;
   struct list_head cmd_buffers;
   struct list_head free_cmd_buffers;
   uint32_t queue_family_index;
};

struct tu_cmd_buffer_upload
{
   uint8_t *map;
   unsigned offset;
   uint64_t size;
   struct radeon_winsys_bo *upload_bo;
   struct list_head list;
};

enum tu_cmd_buffer_status
{
   TU_CMD_BUFFER_STATUS_INVALID,
   TU_CMD_BUFFER_STATUS_INITIAL,
   TU_CMD_BUFFER_STATUS_RECORDING,
   TU_CMD_BUFFER_STATUS_EXECUTABLE,
   TU_CMD_BUFFER_STATUS_PENDING,
};

struct tu_cmd_buffer
{
   VK_LOADER_DATA _loader_data;

   struct tu_device *device;

   struct tu_cmd_pool *pool;
   struct list_head pool_link;

   VkCommandBufferUsageFlags usage_flags;
   VkCommandBufferLevel level;
   enum tu_cmd_buffer_status status;
   struct radeon_cmdbuf *cs;
   struct tu_cmd_state state;
   struct tu_vertex_binding vertex_bindings[MAX_VBS];
   uint32_t queue_family_index;

   uint8_t push_constants[MAX_PUSH_CONSTANTS_SIZE];
   VkShaderStageFlags push_constant_stages;
   struct tu_descriptor_set meta_push_descriptors;

   struct tu_descriptor_state descriptors[VK_PIPELINE_BIND_POINT_RANGE_SIZE];

   struct tu_cmd_buffer_upload upload;

   uint32_t scratch_size_needed;
   uint32_t compute_scratch_size_needed;
   uint32_t esgs_ring_size_needed;
   uint32_t gsvs_ring_size_needed;
   bool tess_rings_needed;
   bool sample_positions_needed;

   VkResult record_result;

   uint32_t gfx9_fence_offset;
   struct radeon_winsys_bo *gfx9_fence_bo;
   uint32_t gfx9_fence_idx;
   uint64_t gfx9_eop_bug_va;

   /**
    * Whether a query pool has been resetted and we have to flush caches.
    */
   bool pending_reset_query;
};

bool
tu_get_memory_fd(struct tu_device *device,
                 struct tu_device_memory *memory,
                 int *pFD);

/*
 * Takes x,y,z as exact numbers of invocations, instead of blocks.
 *
 * Limitations: Can't call normal dispatch functions without binding or
 * rebinding
 *              the compute pipeline.
 */
void
tu_unaligned_dispatch(struct tu_cmd_buffer *cmd_buffer,
                      uint32_t x,
                      uint32_t y,
                      uint32_t z);

struct tu_event
{
   struct radeon_winsys_bo *bo;
   uint64_t *map;
};

struct tu_shader_module;

#define TU_HASH_SHADER_IS_GEOM_COPY_SHADER (1 << 0)
#define TU_HASH_SHADER_SISCHED (1 << 1)
#define TU_HASH_SHADER_UNSAFE_MATH (1 << 2)
void
tu_hash_shaders(unsigned char *hash,
                const VkPipelineShaderStageCreateInfo **stages,
                const struct tu_pipeline_layout *layout,
                const struct tu_pipeline_key *key,
                uint32_t flags);

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

#define TU_STAGE_MASK ((1 << MESA_SHADER_STAGES) - 1)

#define tu_foreach_stage(stage, stage_bits)                                   \
   for (gl_shader_stage stage,                                                 \
        __tmp = (gl_shader_stage)((stage_bits)&TU_STAGE_MASK);                \
        stage = __builtin_ffs(__tmp) - 1, __tmp;                               \
        __tmp &= ~(1 << (stage)))

struct tu_shader_module
{
   struct nir_shader *nir;
   unsigned char sha1[20];
   uint32_t size;
   char data[0];
};

struct tu_pipeline
{
   struct tu_device *device;
   struct tu_dynamic_state dynamic_state;

   struct tu_pipeline_layout *layout;

   bool need_indirect_descriptor_sets;
   VkShaderStageFlags active_stages;
};

struct tu_userdata_info *
tu_lookup_user_sgpr(struct tu_pipeline *pipeline,
                    gl_shader_stage stage,
                    int idx);

struct tu_shader_variant *
tu_get_shader(struct tu_pipeline *pipeline, gl_shader_stage stage);

struct tu_graphics_pipeline_create_info
{
   bool use_rectlist;
   bool db_depth_clear;
   bool db_stencil_clear;
   bool db_depth_disable_expclear;
   bool db_stencil_disable_expclear;
   bool db_flush_depth_inplace;
   bool db_flush_stencil_inplace;
   bool db_resummarize;
   uint32_t custom_blend_mode;
};

VkResult
tu_graphics_pipeline_create(
   VkDevice device,
   VkPipelineCache cache,
   const VkGraphicsPipelineCreateInfo *pCreateInfo,
   const struct tu_graphics_pipeline_create_info *extra,
   const VkAllocationCallbacks *alloc,
   VkPipeline *pPipeline);

struct vk_format_description;
uint32_t
tu_translate_buffer_dataformat(const struct vk_format_description *desc,
                               int first_non_void);
uint32_t
tu_translate_buffer_numformat(const struct vk_format_description *desc,
                              int first_non_void);
uint32_t
tu_translate_colorformat(VkFormat format);
uint32_t
tu_translate_color_numformat(VkFormat format,
                             const struct vk_format_description *desc,
                             int first_non_void);
uint32_t
tu_colorformat_endian_swap(uint32_t colorformat);
unsigned
tu_translate_colorswap(VkFormat format, bool do_endian_swap);
uint32_t
tu_translate_dbformat(VkFormat format);
uint32_t
tu_translate_tex_dataformat(VkFormat format,
                            const struct vk_format_description *desc,
                            int first_non_void);
uint32_t
tu_translate_tex_numformat(VkFormat format,
                           const struct vk_format_description *desc,
                           int first_non_void);
bool
tu_format_pack_clear_color(VkFormat format,
                           uint32_t clear_vals[2],
                           VkClearColorValue *value);
bool
tu_is_colorbuffer_format_supported(VkFormat format, bool *blendable);
bool
tu_dcc_formats_compatible(VkFormat format1, VkFormat format2);

struct tu_image
{
   VkImageType type;
   /* The original VkFormat provided by the client.  This may not match any
    * of the actual surface formats.
    */
   VkFormat vk_format;
   VkImageAspectFlags aspects;
   VkImageUsageFlags usage;  /**< Superset of VkImageCreateInfo::usage. */
   VkImageTiling tiling;     /** VkImageCreateInfo::tiling */
   VkImageCreateFlags flags; /** VkImageCreateInfo::flags */

   VkDeviceSize size;
   uint32_t alignment;

   unsigned queue_family_mask;
   bool exclusive;
   bool shareable;

   /* For VK_ANDROID_native_buffer, the WSI image owns the memory, */
   VkDeviceMemory owned_memory;
};

unsigned
tu_image_queue_family_mask(const struct tu_image *image,
                           uint32_t family,
                           uint32_t queue_family);

static inline uint32_t
tu_get_layerCount(const struct tu_image *image,
                  const VkImageSubresourceRange *range)
{
   abort();
}

static inline uint32_t
tu_get_levelCount(const struct tu_image *image,
                  const VkImageSubresourceRange *range)
{
   abort();
}

struct tu_image_view
{
   struct tu_image *image; /**< VkImageViewCreateInfo::image */

   VkImageViewType type;
   VkImageAspectFlags aspect_mask;
   VkFormat vk_format;
   uint32_t base_layer;
   uint32_t layer_count;
   uint32_t base_mip;
   uint32_t level_count;
   VkExtent3D extent; /**< Extent of VkImageViewCreateInfo::baseMipLevel. */

   uint32_t descriptor[16];

   /* Descriptor for use as a storage image as opposed to a sampled image.
    * This has a few differences for cube maps (e.g. type).
    */
   uint32_t storage_descriptor[16];
};

struct tu_sampler
{
};

struct tu_image_create_info
{
   const VkImageCreateInfo *vk_info;
   bool scanout;
   bool no_metadata_planes;
};

VkResult
tu_image_create(VkDevice _device,
                const struct tu_image_create_info *info,
                const VkAllocationCallbacks *alloc,
                VkImage *pImage);

VkResult
tu_image_from_gralloc(VkDevice device_h,
                      const VkImageCreateInfo *base_info,
                      const VkNativeBufferANDROID *gralloc_info,
                      const VkAllocationCallbacks *alloc,
                      VkImage *out_image_h);

void
tu_image_view_init(struct tu_image_view *view,
                   struct tu_device *device,
                   const VkImageViewCreateInfo *pCreateInfo);

struct tu_buffer_view
{
   struct radeon_winsys_bo *bo;
   VkFormat vk_format;
   uint64_t range; /**< VkBufferViewCreateInfo::range */
   uint32_t state[4];
};
void
tu_buffer_view_init(struct tu_buffer_view *view,
                    struct tu_device *device,
                    const VkBufferViewCreateInfo *pCreateInfo);

static inline struct VkExtent3D
tu_sanitize_image_extent(const VkImageType imageType,
                         const struct VkExtent3D imageExtent)
{
   switch (imageType) {
      case VK_IMAGE_TYPE_1D:
         return (VkExtent3D){ imageExtent.width, 1, 1 };
      case VK_IMAGE_TYPE_2D:
         return (VkExtent3D){ imageExtent.width, imageExtent.height, 1 };
      case VK_IMAGE_TYPE_3D:
         return imageExtent;
      default:
         unreachable("invalid image type");
   }
}

static inline struct VkOffset3D
tu_sanitize_image_offset(const VkImageType imageType,
                         const struct VkOffset3D imageOffset)
{
   switch (imageType) {
      case VK_IMAGE_TYPE_1D:
         return (VkOffset3D){ imageOffset.x, 0, 0 };
      case VK_IMAGE_TYPE_2D:
         return (VkOffset3D){ imageOffset.x, imageOffset.y, 0 };
      case VK_IMAGE_TYPE_3D:
         return imageOffset;
      default:
         unreachable("invalid image type");
   }
}

struct tu_attachment_info
{
   struct tu_image_view *attachment;
};

struct tu_framebuffer
{
   uint32_t width;
   uint32_t height;
   uint32_t layers;

   uint32_t attachment_count;
   struct tu_attachment_info attachments[0];
};

struct tu_subpass_barrier
{
   VkPipelineStageFlags src_stage_mask;
   VkAccessFlags src_access_mask;
   VkAccessFlags dst_access_mask;
};

void
tu_subpass_barrier(struct tu_cmd_buffer *cmd_buffer,
                   const struct tu_subpass_barrier *barrier);

struct tu_subpass_attachment
{
   uint32_t attachment;
   VkImageLayout layout;
};

struct tu_subpass
{
   uint32_t input_count;
   uint32_t color_count;
   struct tu_subpass_attachment *input_attachments;
   struct tu_subpass_attachment *color_attachments;
   struct tu_subpass_attachment *resolve_attachments;
   struct tu_subpass_attachment depth_stencil_attachment;

   /** Subpass has at least one resolve attachment */
   bool has_resolve;

   struct tu_subpass_barrier start_barrier;

   uint32_t view_mask;
   VkSampleCountFlagBits max_sample_count;
};

struct tu_render_pass_attachment
{
   VkFormat format;
   uint32_t samples;
   VkAttachmentLoadOp load_op;
   VkAttachmentLoadOp stencil_load_op;
   VkImageLayout initial_layout;
   VkImageLayout final_layout;
   uint32_t view_mask;
};

struct tu_render_pass
{
   uint32_t attachment_count;
   uint32_t subpass_count;
   struct tu_subpass_attachment *subpass_attachments;
   struct tu_render_pass_attachment *attachments;
   struct tu_subpass_barrier end_barrier;
   struct tu_subpass subpasses[0];
};

VkResult
tu_device_init_meta(struct tu_device *device);
void
tu_device_finish_meta(struct tu_device *device);

struct tu_query_pool
{
   struct radeon_winsys_bo *bo;
   uint32_t stride;
   uint32_t availability_offset;
   uint64_t size;
   char *ptr;
   VkQueryType type;
   uint32_t pipeline_stats_mask;
};

struct tu_semaphore
{
   /* use a winsys sem for non-exportable */
   struct radeon_winsys_sem *sem;
   uint32_t syncobj;
   uint32_t temp_syncobj;
};

void
tu_set_descriptor_set(struct tu_cmd_buffer *cmd_buffer,
                      VkPipelineBindPoint bind_point,
                      struct tu_descriptor_set *set,
                      unsigned idx);

void
tu_update_descriptor_sets(struct tu_device *device,
                          struct tu_cmd_buffer *cmd_buffer,
                          VkDescriptorSet overrideSet,
                          uint32_t descriptorWriteCount,
                          const VkWriteDescriptorSet *pDescriptorWrites,
                          uint32_t descriptorCopyCount,
                          const VkCopyDescriptorSet *pDescriptorCopies);

void
tu_update_descriptor_set_with_template(
   struct tu_device *device,
   struct tu_cmd_buffer *cmd_buffer,
   struct tu_descriptor_set *set,
   VkDescriptorUpdateTemplateKHR descriptorUpdateTemplate,
   const void *pData);

void
tu_meta_push_descriptor_set(struct tu_cmd_buffer *cmd_buffer,
                            VkPipelineBindPoint pipelineBindPoint,
                            VkPipelineLayout _layout,
                            uint32_t set,
                            uint32_t descriptorWriteCount,
                            const VkWriteDescriptorSet *pDescriptorWrites);

struct tu_fence
{
   struct radeon_winsys_fence *fence;
   bool submitted;
   bool signalled;

   uint32_t syncobj;
   uint32_t temp_syncobj;
};

/* tu_nir_to_llvm.c */
struct tu_shader_variant_info;
struct tu_nir_compiler_options;

struct radeon_winsys_sem;

uint32_t
tu_gem_new(struct tu_device *dev, uint64_t size, uint32_t flags);
void
tu_gem_close(struct tu_device *dev, uint32_t gem_handle);
uint64_t
tu_gem_info_offset(struct tu_device *dev, uint32_t gem_handle);
uint64_t
tu_gem_info_iova(struct tu_device *dev, uint32_t gem_handle);

#define TU_DEFINE_HANDLE_CASTS(__tu_type, __VkType)                          \
                                                                               \
   static inline struct __tu_type *__tu_type##_from_handle(__VkType _handle) \
   {                                                                           \
      return (struct __tu_type *)_handle;                                     \
   }                                                                           \
                                                                               \
   static inline __VkType __tu_type##_to_handle(struct __tu_type *_obj)      \
   {                                                                           \
      return (__VkType)_obj;                                                   \
   }

#define TU_DEFINE_NONDISP_HANDLE_CASTS(__tu_type, __VkType)                  \
                                                                               \
   static inline struct __tu_type *__tu_type##_from_handle(__VkType _handle) \
   {                                                                           \
      return (struct __tu_type *)(uintptr_t)_handle;                          \
   }                                                                           \
                                                                               \
   static inline __VkType __tu_type##_to_handle(struct __tu_type *_obj)      \
   {                                                                           \
      return (__VkType)(uintptr_t)_obj;                                        \
   }

#define TU_FROM_HANDLE(__tu_type, __name, __handle)                          \
   struct __tu_type *__name = __tu_type##_from_handle(__handle)

TU_DEFINE_HANDLE_CASTS(tu_cmd_buffer, VkCommandBuffer)
TU_DEFINE_HANDLE_CASTS(tu_device, VkDevice)
TU_DEFINE_HANDLE_CASTS(tu_instance, VkInstance)
TU_DEFINE_HANDLE_CASTS(tu_physical_device, VkPhysicalDevice)
TU_DEFINE_HANDLE_CASTS(tu_queue, VkQueue)

TU_DEFINE_NONDISP_HANDLE_CASTS(tu_cmd_pool, VkCommandPool)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_buffer, VkBuffer)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_buffer_view, VkBufferView)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_descriptor_pool, VkDescriptorPool)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_descriptor_set, VkDescriptorSet)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_descriptor_set_layout,
                                VkDescriptorSetLayout)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_descriptor_update_template,
                                VkDescriptorUpdateTemplateKHR)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_device_memory, VkDeviceMemory)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_fence, VkFence)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_event, VkEvent)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_framebuffer, VkFramebuffer)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_image, VkImage)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_image_view, VkImageView);
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_pipeline_cache, VkPipelineCache)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_pipeline, VkPipeline)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_pipeline_layout, VkPipelineLayout)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_query_pool, VkQueryPool)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_render_pass, VkRenderPass)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_sampler, VkSampler)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_shader_module, VkShaderModule)
TU_DEFINE_NONDISP_HANDLE_CASTS(tu_semaphore, VkSemaphore)

#endif /* TU_PRIVATE_H */

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
#include <vk_enum_to_str.h>

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

#include "compiler/shader_enums.h"
#include "compiler/spirv/nir_spirv.h"

#include "compiler/v3d_compiler.h"

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
#include "wsi_common.h"

#include "broadcom/cle/v3dx_pack.h"

/* A non-fatal assert.  Useful for debugging. */
#ifdef DEBUG
#define v3dv_assert(x) ({ \
   if (unlikely(!(x))) \
      fprintf(stderr, "%s:%d ASSERT: %s", __FILE__, __LINE__, #x); \
})
#else
#define v3dv_assert(x)
#endif

#define perf_debug(...) do {                       \
   if (unlikely(V3D_DEBUG & V3D_DEBUG_PERF))       \
      fprintf(stderr, __VA_ARGS__);                \
} while (0)

#define for_each_bit(b, dword)                                               \
   for (uint32_t __dword = (dword);                                          \
        (b) = __builtin_ffs(__dword) - 1, __dword; __dword &= ~(1 << (b)))

#define typed_memcpy(dest, src, count) ({				\
			STATIC_ASSERT(sizeof(*src) == sizeof(*dest)); \
			memcpy((dest), (src), (count) * sizeof(*(src))); \
		})

#define NSEC_PER_SEC 1000000000ull

/* From vulkan spec "If the multiple viewports feature is not enabled,
 * scissorCount must be 1", ditto for viewportCount. For now we don't support
 * that feature.
 */
#define MAX_VIEWPORTS 1
#define MAX_SCISSORS  1

#define MAX_VBS 16
#define MAX_VERTEX_ATTRIBS 16

#define MAX_SETS 16

#define MAX_PUSH_CONSTANTS_SIZE 128

#define MAX_DYNAMIC_UNIFORM_BUFFERS 16
#define MAX_DYNAMIC_STORAGE_BUFFERS 8
#define MAX_DYNAMIC_BUFFERS                                                  \
   (MAX_DYNAMIC_UNIFORM_BUFFERS + MAX_DYNAMIC_STORAGE_BUFFERS)

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

   char *name;
   int32_t render_fd;
   int32_t display_fd;
   uint8_t pipeline_cache_uuid[VK_UUID_SIZE];

   struct wsi_device wsi_device;

   VkPhysicalDeviceMemoryProperties memory;

   struct v3d_device_info devinfo;

   struct v3d_simulator_file *sim_file;

   const struct v3d_compiler *compiler;
   uint32_t next_program_id;

   struct {
      bool merge_jobs;
   } options;
};

VkResult v3dv_wsi_init(struct v3dv_physical_device *physical_device);
void v3dv_wsi_finish(struct v3dv_physical_device *physical_device);

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

   /* When the client submits to the queue without a command buffer the queue
    * needs to create and submit a no-op job and it is then responsible from
    * destroying it once it has completed execution. This list keeps references
    * to all no-op jobs in flight so we can do that.
    */
   struct list_head noop_jobs;
};

void v3dv_queue_destroy_completed_noop_jobs(struct v3dv_queue *queue);

struct v3dv_device {
   VK_LOADER_DATA _loader_data;

   VkAllocationCallbacks alloc;

   struct v3dv_instance *instance;

   struct v3dv_device_extension_table enabled_extensions;
   struct v3dv_device_dispatch_table dispatch;

   int32_t render_fd;
   int32_t display_fd;
   struct v3d_device_info devinfo;
   struct v3dv_queue queue;

   /* Last command buffer submitted on this device. We use this to check if
    * the GPU is idle.
    */
   uint32_t last_job_sync;
};

struct v3dv_device_memory {
   struct v3dv_bo *bo;
   const VkMemoryType *type;
};

#define V3D_OUTPUT_IMAGE_FORMAT_NO 255
#define TEXTURE_DATA_FORMAT_NO     255

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
   uint32_t internal_bpp;
   uint32_t internal_type;

   uint32_t base_level;
   uint32_t max_level;
   uint32_t first_layer;
   uint32_t last_layer;
   uint32_t offset;

   /* Precomputed (composed from createinfo->components and formar swizzle)
    * swizzles to pass in to the shader key.
    *
    * FIXME: this is also a candidate to be included on the descriptor info.
    */
   uint8_t swizzle[4];

   /* FIXME: here we store the packet TEXTURE_SHADER_STATE, that is referenced
    * as part of the tmu configuration, and the content is set per sampler. A
    * possible perf improvement, to avoid bo fragmentation, would be to save
    * the state as static, have the bo as part of the descriptor (booked from
    * the descriptor pools), and then copy this content to the descriptor bo
    * on UpdateDescriptor. This also makes sense because not all the images
    * are used as textures.
    */
   struct v3dv_bo *texture_shader_state;
};

uint32_t v3dv_layer_offset(const struct v3dv_image *image, uint32_t level, uint32_t layer);

struct v3dv_buffer {
   VkDeviceSize size;
   VkBufferUsageFlags usage;
   uint32_t alignment;

   struct v3dv_device_memory *mem;
   VkDeviceSize mem_offset;
};

struct v3dv_buffer_view {
   const struct v3dv_buffer *buffer;

   VkFormat vk_format;
   const struct v3dv_format *format;
   uint32_t internal_bpp;
   uint32_t internal_type;

   uint32_t offset;
   uint32_t size;
   uint32_t num_elements;
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

   bool has_srgb_rt;
};

struct v3dv_render_pass_attachment {
   VkAttachmentDescription desc;
   uint32_t first_subpass;
   uint32_t last_subpass;
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

   uint32_t attachment_count;
   uint32_t color_attachment_count;
   struct v3dv_image_view *attachments[0];
};

struct v3dv_frame_tiling {
   uint32_t width;
   uint32_t height;
   uint32_t render_target_count;
   uint32_t internal_bpp;
   uint32_t layers;
   uint32_t tile_width;
   uint32_t tile_height;
   uint32_t draw_tiles_x;
   uint32_t draw_tiles_y;
   uint32_t supertile_width;
   uint32_t supertile_height;
   uint32_t frame_width_in_supertiles;
   uint32_t frame_height_in_supertiles;
};

uint8_t v3dv_framebuffer_compute_internal_bpp(const struct v3dv_framebuffer *framebuffer,
                                              const struct v3dv_subpass *subpass);

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
   struct {
      float z;
      uint8_t s;
   };
};

struct v3dv_cmd_buffer_attachment_state {
   union v3dv_clear_value clear_value;
};

void v3dv_get_hw_clear_color(const VkClearColorValue *color,
                             uint32_t internal_type,
                             uint32_t internal_size,
                             uint32_t *hw_color);

struct v3dv_viewport_state {
   uint32_t count;
   VkViewport viewports[MAX_VIEWPORTS];
   float translate[MAX_VIEWPORTS][3];
   float scale[MAX_VIEWPORTS][3];
};

struct v3dv_scissor_state {
   uint32_t count;
   VkRect2D scissors[MAX_SCISSORS];
};

/* Mostly a v3dv mapping of VkDynamicState, used to track which data as
 * defined as dynamic
 */
enum v3dv_dynamic_state_bits {
   V3DV_DYNAMIC_VIEWPORT                  = 1 << 0,
   V3DV_DYNAMIC_SCISSOR                   = 1 << 1,
   V3DV_DYNAMIC_STENCIL_COMPARE_MASK      = 1 << 2,
   V3DV_DYNAMIC_STENCIL_WRITE_MASK        = 1 << 3,
   V3DV_DYNAMIC_STENCIL_REFERENCE         = 1 << 4,
   V3DV_DYNAMIC_BLEND_CONSTANTS           = 1 << 5,
   V3DV_DYNAMIC_ALL                       = (1 << 6) - 1,
};

/* Flags for dirty pipeline state.
 */
enum v3dv_cmd_dirty_bits {
   V3DV_CMD_DIRTY_VIEWPORT                  = 1 << 0,
   V3DV_CMD_DIRTY_SCISSOR                   = 1 << 1,
   V3DV_CMD_DIRTY_STENCIL_COMPARE_MASK      = 1 << 2,
   V3DV_CMD_DIRTY_STENCIL_WRITE_MASK        = 1 << 3,
   V3DV_CMD_DIRTY_STENCIL_REFERENCE         = 1 << 4,
   V3DV_CMD_DIRTY_PIPELINE                  = 1 << 5,
   V3DV_CMD_DIRTY_VERTEX_BUFFER             = 1 << 6,
   V3DV_CMD_DIRTY_DESCRIPTOR_SETS           = 1 << 7,
   V3DV_CMD_DIRTY_PUSH_CONSTANTS            = 1 << 8,
   V3DV_CMD_DIRTY_BLEND_CONSTANTS           = 1 << 9,
};


struct v3dv_dynamic_state {
   /**
    * Bitmask of (1 << VK_DYNAMIC_STATE_*).
    * Defines the set of saved dynamic state.
    */
   uint32_t mask;

   struct v3dv_viewport_state viewport;

   struct v3dv_scissor_state scissor;

   struct {
      uint32_t front;
      uint32_t back;
   } stencil_compare_mask;

   struct {
      uint32_t front;
      uint32_t back;
   } stencil_write_mask;

   struct {
      uint32_t front;
      uint32_t back;
   } stencil_reference;

   float blend_constants[4];
};

extern const struct v3dv_dynamic_state default_dynamic_state;

void v3dv_viewport_compute_xform(const VkViewport *viewport,
                                 float scale[3],
                                 float translate[3]);

enum v3dv_ez_state {
   VC5_EZ_UNDECIDED = 0,
   VC5_EZ_GT_GE,
   VC5_EZ_LT_LE,
   VC5_EZ_DISABLED,
};

struct v3dv_job {
   struct list_head list_link;

   struct v3dv_device *device;

   struct v3dv_cmd_buffer *cmd_buffer;

   struct v3dv_cl bcl;
   struct v3dv_cl rcl;
   struct v3dv_cl indirect;

   /* Set of all BOs referenced by the job. This will be used for making
    * the list of BOs that the kernel will need to have paged in to
    * execute our job.
    */
   struct set *bos;
   uint32_t bo_count;

   /* A subset of the BOs set above that are allocated internally by
    * the job and that should be explicitly freed with it.
    */
   struct set *extra_bos;

   struct v3dv_bo *tile_alloc;
   struct v3dv_bo *tile_state;

   bool tmu_dirty_rcl;

   uint32_t first_subpass;

   /* When the current subpass is split into multiple jobs, this flag is set
    * to true for any jobs after the first in the same subpass.
    */
   bool is_subpass_continue;

   /* If this job is the last job emitted for a subpass. */
   bool is_subpass_finish;

   struct v3dv_frame_tiling frame_tiling;

   enum v3dv_ez_state ez_state;
   enum v3dv_ez_state first_ez_state;

   /* Typically, the client is responsible for handling the life-time of
    * command buffers by using fences to tell when they are no longer in
    * use by the GPU, however, when the jobs that are submitted to the GPU
    * are created internally by the driver (for example when we need to
    * submit no-op jobs), then it is our responsibility to do that.
    */
   struct v3dv_fence *fence;

   /* Number of draw calls recorded into the job */
   uint32_t draw_count;

   /* A flag indicating whether we want to flush every draw separately. This
    * can be used for debugging, or for cases where special circumstances
    * require this behavior.
    */
   bool always_flush;
};

void v3dv_job_init(struct v3dv_job *job,
                   struct v3dv_device *device,
                   struct v3dv_cmd_buffer *cmd_buffer,
                   int32_t subpass_idx);
void v3dv_job_destroy(struct v3dv_job *job);
void v3dv_job_add_bo(struct v3dv_job *job, struct v3dv_bo *bo);
void v3dv_job_add_extra_bo(struct v3dv_job *job, struct v3dv_bo *bo);
void v3dv_job_emit_binning_flush(struct v3dv_job *job);
void v3dv_job_start_frame(struct v3dv_job *job,
                          uint32_t width,
                          uint32_t height,
                          uint32_t layers,
                          uint32_t render_target_count,
                          uint8_t max_internal_bpp);

struct v3dv_vertex_binding {
   struct v3dv_buffer *buffer;
   VkDeviceSize offset;
};

struct v3dv_descriptor_state {
   struct v3dv_descriptor_set *descriptor_sets[MAX_SETS];
   uint32_t valid;
   uint32_t dynamic_offsets[MAX_DYNAMIC_BUFFERS];
};

struct v3dv_cmd_buffer_state {
   const struct v3dv_render_pass *pass;
   const struct v3dv_framebuffer *framebuffer;
   VkRect2D render_area;

   /* Current job being recorded */
   struct v3dv_job *job;

   uint32_t subpass_idx;

   struct v3dv_pipeline *pipeline;
   struct v3dv_descriptor_state descriptor_state;

   struct v3dv_dynamic_state dynamic;
   uint32_t dirty;

   uint32_t attachment_count;
   struct v3dv_cmd_buffer_attachment_state *attachments;

   struct v3dv_vertex_binding vertex_bindings[MAX_VBS];

   uint8_t index_size;

   /* Used to flag OOM conditions during command buffer recording */
   bool oom;
};

struct v3dv_descriptor {
   VkDescriptorType type;

   union {
      struct {
         struct v3dv_image_view *image_view;
         struct v3dv_sampler *sampler;
      };

      struct {
         struct v3dv_buffer *buffer;
         uint32_t offset;
      };
   };
};

/* Aux struct as it is really common to have a pair bo/address. Called
 * resource because it is really likely that we would need something like that
 * if we work on reuse the same bo at different points (like the shader
 * assembly).
 */
struct v3dv_resource {
   struct v3dv_bo *bo;
   uint32_t offset;
};

struct v3dv_cmd_buffer {
   VK_LOADER_DATA _loader_data;

   struct v3dv_device *device;

   struct v3dv_cmd_pool *pool;
   struct list_head pool_link;

   VkCommandBufferUsageFlags usage_flags;
   VkCommandBufferLevel level;

   enum v3dv_cmd_buffer_status status;

   struct v3dv_cmd_buffer_state state;

   uint32_t push_constants_data[MAX_PUSH_CONSTANTS_SIZE / 4];
   struct v3dv_resource push_constants_resource;

   /* List of jobs to submit to the kernel */
   struct list_head submit_jobs;
};

struct v3dv_job *v3dv_cmd_buffer_start_job(struct v3dv_cmd_buffer *cmd_buffer,
                                           int32_t subpass_idx);
void v3dv_cmd_buffer_finish_job(struct v3dv_cmd_buffer *cmd_buffer);

void v3dv_render_pass_setup_render_target(struct v3dv_cmd_buffer *cmd_buffer,
                                          int rt,
                                          uint32_t *rt_bpp,
                                          uint32_t *rt_type,
                                          uint32_t *rt_clamp);

struct v3dv_semaphore {
   /* A syncobject handle associated with this semaphore */
   uint32_t sync;

   /* The file handle of a fence that we imported into our syncobject */
   int32_t fd;
};

struct v3dv_fence {
   /* A syncobject handle associated with this fence */
   uint32_t sync;

   /* The file handle of a fence that we imported into our syncobject */
   int32_t fd;
};

struct v3dv_event {
   struct v3dv_bo *bo;
};

struct v3dv_shader_module {
   unsigned char sha1[20];
   uint32_t size;
   char data[0];
};

/* FIXME: the same function at anv, radv and tu, perhaps create common
 * place?
 */
static inline gl_shader_stage
vk_to_mesa_shader_stage(VkShaderStageFlagBits vk_stage)
{
   assert(__builtin_popcount(vk_stage) == 1);
   return ffs(vk_stage) - 1;
}

struct v3dv_shader_variant {
   union {
      struct v3d_prog_data *base;
      struct v3d_vs_prog_data *vs;
      struct v3d_fs_prog_data *fs;
   } prog_data;

   /* FIXME: using one bo per shader. Eventually we would be interested on
    * reusing the same bo for all the shaders, like a bo per v3dv_pipeline for
    * shaders.
    */
   struct v3dv_bo *assembly_bo;
};

/*
 * Per-stage info for each stage, useful so shader_module_compile_to_nir and
 * other methods doesn't have so many parameters.
 *
 * FIXME: for the case of the coordinate shader and the vertex shader, module,
 * entrypoint, spec_info and nir are the same. There are also info only
 * relevant to some stages. But seemed too much a hassle to create a new
 * struct only to handle that. Revisit if such kind of info starts to grow.
 */
struct v3dv_pipeline_stage {
   struct v3dv_pipeline *pipeline;

   gl_shader_stage stage;
   /* FIXME: is_coord only make sense if stage == MESA_SHADER_VERTEX. Perhaps
    * a stage base/vs/fs as keys and prog_data?
    */
   bool is_coord;

   const struct v3dv_shader_module *module;
   const char *entrypoint;
   const VkSpecializationInfo *spec_info;

   nir_shader *nir;

   /** A name for this program, so you can track it in shader-db output. */
   uint32_t program_id;
   /** How many variants of this program were compiled, for shader-db. */
   uint32_t compiled_variant_count;

   /* The following are the default v3d_key populated using
    * VkCreateGraphicsPipelineCreateInfo. Variants will be created tweaking
    * them, so we don't need to maintain a copy of that create info struct
    * around
    */
   union {
      struct v3d_key base;
      struct v3d_vs_key vs;
      struct v3d_fs_key fs;
   } key;

   /* Cache with all the shader variant.
    */
   struct hash_table *cache;

   struct v3dv_shader_variant *current_variant;

   /* FIXME: only make sense on vs, so perhaps a v3dv key like radv? or a kind
    * of pipe_draw_info
    */
   enum pipe_prim_type topology;
};

/* FIXME: although the full vpm_config is not required at this point, as we
 * don't plan to initially support GS, it is more readable and serves as a
 * placeholder, to have the struct and fill it with default values.
 */
struct vpm_config {
   uint32_t As;
   uint32_t Vc;
   uint32_t Gs;
   uint32_t Gd;
   uint32_t Gv;
   uint32_t Ve;
   uint32_t gs_width;
};

struct v3dv_descriptor_pool_entry
{
   struct v3dv_descriptor_set *set;
};

struct v3dv_descriptor_pool {
   uint8_t *host_memory_base;
   uint8_t *host_memory_ptr;
   uint8_t *host_memory_end;

   uint32_t entry_count;
   uint32_t max_entry_count;
   struct v3dv_descriptor_pool_entry entries[0];
};

struct v3dv_descriptor_set {
   struct v3dv_descriptor_pool *pool;

   const struct v3dv_descriptor_set_layout *layout;

   /* The descriptors below can be indexed (set/binding) using the set_layout
    */
   struct v3dv_descriptor descriptors[0];
};

struct v3dv_descriptor_set_binding_layout {
   VkDescriptorType type;

   /* Number of array elements in this binding */
   uint32_t array_size;

   uint32_t descriptor_index;

   uint32_t dynamic_offset_count;
   uint32_t dynamic_offset_index;
};

struct v3dv_descriptor_set_layout {
   VkDescriptorSetLayoutCreateFlags flags;

   /* Number of bindings in this descriptor set */
   uint32_t binding_count;

   /* Total size of the descriptor set with room for all array entries */
   uint32_t size;

   /* Shader stages affected by this descriptor set */
   uint16_t shader_stages;

   /* Number of descriptors in this descriptor set */
   uint32_t descriptor_count;

   /* Number of dynamic offsets used by this descriptor set */
   uint16_t dynamic_offset_count;

   bool has_immutable_samplers;

   /* Bindings in this descriptor set */
   struct v3dv_descriptor_set_binding_layout binding[0];
};

struct v3dv_pipeline_layout {
   struct {
      struct v3dv_descriptor_set_layout *layout;
      uint32_t dynamic_offset_start;
   } set[MAX_SETS];

   uint32_t num_sets;
   uint32_t dynamic_offset_count;

   uint32_t push_constant_size;
};

struct v3dv_descriptor_map {
   /* TODO: avoid fixed size array/justify the size */
   unsigned num_desc; /* Number of descriptors  */
   int set[64];
   int binding[64];
   int array_index[64];
   int array_size[64];
};

struct v3dv_sampler {
   /* FIXME: here we store the packet SAMPLER_STATE, that is referenced as part
    * of the tmu configuration, and the content is set per sampler. A possible
    * perf improvement, to avoid bo fragmentation, would be to save the state
    * as static, have the bo as part of the descriptor (booked from the
    * descriptor pools), and then copy this content to the descriptor bo on
    * UpdateDescriptor
    */
   struct v3dv_bo *state;
};

struct v3dv_pipeline {
   struct v3dv_device *device;

   VkShaderStageFlags active_stages;

   struct v3dv_render_pass *pass;
   struct v3dv_subpass *subpass;

   /* Note: We can't use just a MESA_SHADER_STAGES array as we need to track
    * too the coordinate shader
    */
   struct v3dv_pipeline_stage *vs;
   struct v3dv_pipeline_stage *vs_bin;
   struct v3dv_pipeline_stage *fs;

   struct v3dv_dynamic_state dynamic_state;

   struct v3dv_pipeline_layout *layout;

   enum v3dv_ez_state ez_state;

   bool primitive_restart;

   /* Accessed by binding. So vb[binding]->stride is the stride of the vertex
    * array with such binding
    */
   struct v3dv_pipeline_vertex_binding {
      uint32_t stride;
      uint32_t instance_divisor;
   } vb[MAX_VBS];
   uint32_t vb_count;

   /* Note that a lot of info from VkVertexInputAttributeDescription is
    * already prepacked, so storing here only those that need recheck later
    *
    * Note that they are not indexed by the location or nir driver location,
    * as we are defining here only the inputs that the shader are really
    * using.
    */
   struct v3dv_pipeline_vertex_attrib {
      uint32_t binding;
      uint32_t offset;
      /* We store driver_location instead of location because most v3d structs
       * are indexed by driver_location
       */
      uint32_t driver_location;
      VkFormat vk_format;
   } va[MAX_VERTEX_ATTRIBS];
   uint32_t va_count;

   struct v3dv_descriptor_map ubo_map;
   struct v3dv_descriptor_map ssbo_map;

   struct v3dv_descriptor_map sampler_map;
   struct v3dv_descriptor_map texture_map;

   /* FIXME: this bo is another candidate to data to be uploaded using a
    * resource manager, instead of a individual bo
    */
   struct v3dv_bo *default_attribute_values;

   struct vpm_config vpm_cfg;
   struct vpm_config vpm_cfg_bin;

   /* If the pipeline should emit any of the stencil configuration packets */
   bool emit_stencil_cfg[2];

   /* If the pipeline is using push constants */
   bool use_push_constants;

   /* Blend state */
   struct {
      /* Per-RT bit mask with blend enables */
      uint8_t enables;
      /* Per-RT prepacked blend config packets */
      uint8_t cfg[V3D_MAX_DRAW_BUFFERS][cl_packet_length(BLEND_CFG)];
      /* Flag indicating whether the blend factors in use require
       * color constants.
       */
      bool needs_color_constants;
      /* Blend constants packet */
      uint8_t constant_color[cl_packet_length(BLEND_CONSTANT_COLOR)];
      /* Mask with enabled color channels for each RT (4 bits per RT) */
      uint32_t color_write_masks;
   } blend;

   /* Packets prepacked during pipeline creation
    */
   uint8_t cfg_bits[cl_packet_length(CFG_BITS)];
   uint8_t shader_state_record[cl_packet_length(GL_SHADER_STATE_RECORD)];
   uint8_t vcm_cache_size[cl_packet_length(VCM_CACHE_SIZE)];
   uint8_t vertex_attrs[cl_packet_length(GL_SHADER_STATE_ATTRIBUTE_RECORD) *
                        MAX_VERTEX_ATTRIBS];
   uint8_t stencil_cfg[2][cl_packet_length(STENCIL_CFG)];
};

static inline uint32_t
v3dv_zs_buffer_from_aspect_bits(VkImageAspectFlags aspects)
{
   const VkImageAspectFlags zs_aspects =
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
   const VkImageAspectFlags filtered_aspects = aspects & zs_aspects;

   if (filtered_aspects == zs_aspects)
      return ZSTENCIL;
   else if (filtered_aspects == VK_IMAGE_ASPECT_DEPTH_BIT)
      return Z;
   else if (filtered_aspects == VK_IMAGE_ASPECT_STENCIL_BIT)
      return STENCIL;
   else
      return NONE;
}

static inline uint32_t
v3dv_zs_buffer_from_vk_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_D16_UNORM_S8_UINT:
   case VK_FORMAT_D24_UNORM_S8_UINT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return ZSTENCIL;
   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_X8_D24_UNORM_PACK32:
      return Z;
   case VK_FORMAT_S8_UINT:
      return STENCIL;
   default:
      return NONE;
   }
}

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

#define v3dv_debug_ignored_stype(sType) \
   v3dv_loge("%s: ignored VkStructureType %u:%s\n", __func__, (sType), vk_StructureType_to_str(sType))

const struct v3dv_format *v3dv_get_format(VkFormat);
const uint8_t *v3dv_get_format_swizzle(VkFormat f);
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

struct v3dv_cl_reloc v3dv_write_uniforms(struct v3dv_cmd_buffer *cmd_buffer,
                                         struct v3dv_pipeline_stage *p_stage);

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
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_buffer_view, VkBufferView)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_device_memory, VkDeviceMemory)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_descriptor_pool, VkDescriptorPool)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_descriptor_set, VkDescriptorSet)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_descriptor_set_layout, VkDescriptorSetLayout)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_event, VkEvent)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_fence, VkFence)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_framebuffer, VkFramebuffer)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_image, VkImage)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_image_view, VkImageView)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_pipeline, VkPipeline)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_pipeline_layout, VkPipelineLayout)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_render_pass, VkRenderPass)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_sampler, VkSampler)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_semaphore, VkSemaphore)
V3DV_DEFINE_NONDISP_HANDLE_CASTS(v3dv_shader_module, VkShaderModule)

/* This is defined as a macro so that it works for both
 * VkImageSubresourceRange and VkImageSubresourceLayers
 */
#define v3dv_layer_count(_image, _range) \
   ((_range)->layerCount == VK_REMAINING_ARRAY_LAYERS ? \
    (_image)->array_size - (_range)->baseArrayLayer : (_range)->layerCount)

#define v3dv_level_count(_image, _range) \
   ((_range)->levelCount == VK_REMAINING_MIP_LEVELS ? \
    (_image)->levels - (_range)->baseMipLevel : (_range)->levelCount)

static inline int
v3dv_ioctl(int fd, unsigned long request, void *arg)
{
   if (using_v3d_simulator)
      return v3d_simulator_ioctl(fd, request, arg);
   else
      return drmIoctl(fd, request, arg);
}

#endif /* V3DV_PRIVATE_H */

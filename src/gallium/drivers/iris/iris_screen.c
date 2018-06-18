/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_upload_mgr.h"
#include "util/ralloc.h"
#include "drm-uapi/i915_drm.h"
#include "iris_context.h"
#include "iris_pipe.h"
#include "iris_resource.h"
#include "iris_screen.h"
#include "intel/compiler/brw_compiler.h"

static void
iris_flush_frontbuffer(struct pipe_screen *_screen,
                       struct pipe_resource *resource,
                       unsigned level, unsigned layer,
                       void *context_private, struct pipe_box *box)
{
}

static const char *
iris_get_vendor(struct pipe_screen *pscreen)
{
   return "Mesa Project";
}

static const char *
iris_get_device_vendor(struct pipe_screen *pscreen)
{
   return "Intel";
}

static const char *
iris_get_name(struct pipe_screen *pscreen)
{
   struct iris_screen *screen = (struct iris_screen *)pscreen;
   const char *chipset;

   switch (screen->pci_id) {
#undef CHIPSET
#define CHIPSET(id, symbol, str) case id: chipset = str; break;
#include "pci_ids/i965_pci_ids.h"
   default:
      chipset = "Unknown Intel Chipset";
      break;
   }
   return &chipset[9];
}

static int
iris_get_param(struct pipe_screen *pscreen, enum pipe_cap param)
{
   struct iris_screen *screen = (struct iris_screen *)pscreen;

   switch (param) {
   case PIPE_CAP_NPOT_TEXTURES:
   case PIPE_CAP_ANISOTROPIC_FILTER:
   case PIPE_CAP_POINT_SPRITE:
   case PIPE_CAP_OCCLUSION_QUERY:
   case PIPE_CAP_QUERY_TIME_ELAPSED:
   case PIPE_CAP_TEXTURE_SWIZZLE:
   case PIPE_CAP_TEXTURE_MIRROR_CLAMP:
   case PIPE_CAP_BLEND_EQUATION_SEPARATE:
   case PIPE_CAP_SM3:
   case PIPE_CAP_PRIMITIVE_RESTART:
   case PIPE_CAP_INDEP_BLEND_ENABLE:
   case PIPE_CAP_INDEP_BLEND_FUNC:
   case PIPE_CAP_RGB_OVERRIDE_DST_ALPHA_BLEND:
   case PIPE_CAP_TGSI_FS_COORD_ORIGIN_UPPER_LEFT:
   case PIPE_CAP_TGSI_FS_COORD_ORIGIN_LOWER_LEFT:
   case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_HALF_INTEGER:
   case PIPE_CAP_TGSI_FS_COORD_PIXEL_CENTER_INTEGER:
   case PIPE_CAP_DEPTH_CLIP_DISABLE:
   case PIPE_CAP_SHADER_STENCIL_EXPORT:
   case PIPE_CAP_TGSI_INSTANCEID:
   case PIPE_CAP_VERTEX_ELEMENT_INSTANCE_DIVISOR:
   case PIPE_CAP_MIXED_COLORBUFFER_FORMATS:
   case PIPE_CAP_SEAMLESS_CUBE_MAP:
   case PIPE_CAP_SEAMLESS_CUBE_MAP_PER_TEXTURE:
   case PIPE_CAP_CONDITIONAL_RENDER:
   case PIPE_CAP_TEXTURE_BARRIER:
   case PIPE_CAP_STREAM_OUTPUT_PAUSE_RESUME:
   case PIPE_CAP_VERTEX_COLOR_UNCLAMPED:
   case PIPE_CAP_COMPUTE:
   case PIPE_CAP_START_INSTANCE:
   case PIPE_CAP_QUERY_TIMESTAMP:
   case PIPE_CAP_TEXTURE_MULTISAMPLE:
   case PIPE_CAP_CUBE_MAP_ARRAY:
   case PIPE_CAP_TEXTURE_BUFFER_OBJECTS:
   case PIPE_CAP_QUERY_PIPELINE_STATISTICS:
   case PIPE_CAP_BUFFER_MAP_PERSISTENT_COHERENT:
   case PIPE_CAP_TEXTURE_QUERY_LOD:
   case PIPE_CAP_SAMPLE_SHADING:
   case PIPE_CAP_TEXTURE_GATHER_OFFSETS:
   case PIPE_CAP_DRAW_INDIRECT:
   case PIPE_CAP_MIXED_FRAMEBUFFER_SIZES:
   case PIPE_CAP_TGSI_VS_LAYER_VIEWPORT:
   case PIPE_CAP_TGSI_FS_FINE_DERIVATIVE:
   case PIPE_CAP_ACCELERATED:
   case PIPE_CAP_UMA:
   case PIPE_CAP_CONDITIONAL_RENDER_INVERTED:
   case PIPE_CAP_CLIP_HALFZ:
   case PIPE_CAP_TGSI_TEXCOORD:
   case PIPE_CAP_STREAM_OUTPUT_INTERLEAVE_BUFFERS:
   case PIPE_CAP_DOUBLES:
   case PIPE_CAP_INT64:
   case PIPE_CAP_INT64_DIVMOD:
   case PIPE_CAP_BUFFER_SAMPLER_VIEW_RGBA_ONLY:
   case PIPE_CAP_SAMPLER_VIEW_TARGET:
   case PIPE_CAP_ROBUST_BUFFER_ACCESS_BEHAVIOR:
   case PIPE_CAP_COPY_BETWEEN_COMPRESSED_AND_PLAIN_FORMATS:
   case PIPE_CAP_FRAMEBUFFER_NO_ATTACHMENT:
   case PIPE_CAP_CULL_DISTANCE:
   case PIPE_CAP_PACKED_UNIFORMS:
   case PIPE_CAP_ALLOW_MAPPED_BUFFERS_DURING_EXECUTION:
   case PIPE_CAP_SIGNED_VERTEX_BUFFER_OFFSET:
      return true;

   case PIPE_CAP_FRAGMENT_COLOR_CLAMPED:
   case PIPE_CAP_TGSI_CAN_COMPACT_CONSTANTS:
   case PIPE_CAP_VERTEX_COLOR_CLAMPED:
   case PIPE_CAP_QUADS_FOLLOW_PROVOKING_VERTEX_CONVENTION:
   case PIPE_CAP_USER_VERTEX_BUFFERS:
   case PIPE_CAP_VERTEX_BUFFER_OFFSET_4BYTE_ALIGNED_ONLY:
   case PIPE_CAP_VERTEX_BUFFER_STRIDE_4BYTE_ALIGNED_ONLY:
   case PIPE_CAP_VERTEX_ELEMENT_SRC_OFFSET_4BYTE_ALIGNED_ONLY:
   case PIPE_CAP_TEXTURE_BORDER_COLOR_QUIRK:
   case PIPE_CAP_FAKE_SW_MSAA:
   case PIPE_CAP_VERTEXID_NOBASE:
   case PIPE_CAP_FENCE_SIGNAL:
   case PIPE_CAP_CONSTBUF0_FLAGS:
   case PIPE_CAP_CONSERVATIVE_RASTER_POST_SNAP_TRIANGLES:
   case PIPE_CAP_CONSERVATIVE_RASTER_POST_SNAP_POINTS_LINES:
   case PIPE_CAP_CONSERVATIVE_RASTER_PRE_SNAP_TRIANGLES:
   case PIPE_CAP_CONSERVATIVE_RASTER_PRE_SNAP_POINTS_LINES:
   case PIPE_CAP_MAX_CONSERVATIVE_RASTER_SUBPIXEL_PRECISION_BIAS:
   case PIPE_CAP_CONSERVATIVE_RASTER_POST_DEPTH_COVERAGE:
      return false;

   case PIPE_CAP_MAX_DUAL_SOURCE_RENDER_TARGETS:
      return 1;
   case PIPE_CAP_MAX_RENDER_TARGETS:
      return BRW_MAX_DRAW_BUFFERS;
   case PIPE_CAP_MAX_TEXTURE_2D_LEVELS:
   case PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS:
      return 15; /* 16384x16384 */
   case PIPE_CAP_MAX_TEXTURE_3D_LEVELS:
      return 12; /* 2048x2048 */
   case PIPE_CAP_MAX_STREAM_OUTPUT_BUFFERS:
      return 4;
   case PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS:
      return 2048;
   case PIPE_CAP_MIN_TEXEL_OFFSET:
      return -8;
   case PIPE_CAP_MAX_TEXEL_OFFSET:
      return 7;
   case PIPE_CAP_MAX_STREAM_OUTPUT_SEPARATE_COMPONENTS:
      return BRW_MAX_SOL_BINDINGS / IRIS_MAX_SOL_BUFFERS;
   case PIPE_CAP_MAX_STREAM_OUTPUT_INTERLEAVED_COMPONENTS:
      return BRW_MAX_SOL_BINDINGS;
   case PIPE_CAP_GLSL_FEATURE_LEVEL:
      return 460;
   case PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT:
      /* 3DSTATE_CONSTANT_XS requires the start of UBOs to be 32B aligned */
      return 32;
   case PIPE_CAP_MIN_MAP_BUFFER_ALIGNMENT:
      return 64; // XXX: ?
   case PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT:
      return 1;
   case PIPE_CAP_PREFER_BLIT_BASED_TEXTURE_TRANSFER:
      return true; // XXX: ?????
   case PIPE_CAP_MAX_TEXTURE_BUFFER_SIZE:
      return 1 << 27; /* 128MB */
   case PIPE_CAP_MAX_VIEWPORTS:
      return 16;
   case PIPE_CAP_ENDIANNESS:
      return PIPE_ENDIAN_LITTLE;
   case PIPE_CAP_MAX_GEOMETRY_OUTPUT_VERTICES:
      return 256;
   case PIPE_CAP_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS:
      return 128;
   case PIPE_CAP_MAX_TEXTURE_GATHER_COMPONENTS:
   case PIPE_CAP_TEXTURE_GATHER_SM5:
      return 0; // XXX:
   case PIPE_CAP_MIN_TEXTURE_GATHER_OFFSET:
      return -32;
   case PIPE_CAP_MAX_TEXTURE_GATHER_OFFSET:
      return 31;
   case PIPE_CAP_TGSI_VS_WINDOW_SPACE_POSITION:
   case PIPE_CAP_MAX_VERTEX_STREAMS:
      return 4;
   case PIPE_CAP_VENDOR_ID:
      return 0x8086;
   case PIPE_CAP_DEVICE_ID:
      return screen->pci_id;
   case PIPE_CAP_VIDEO_MEMORY:
      return 0xffffffff; // XXX: bogus
   case PIPE_CAP_MAX_VERTEX_ATTRIB_STRIDE:
      return 2048;
   case PIPE_CAP_POLYGON_OFFSET_CLAMP:
   case PIPE_CAP_MULTISAMPLE_Z_RESOLVE:
   case PIPE_CAP_RESOURCE_FROM_USER_MEMORY:
   case PIPE_CAP_DEVICE_RESET_STATUS_QUERY:
   case PIPE_CAP_MAX_SHADER_PATCH_VARYINGS:
   case PIPE_CAP_TEXTURE_FLOAT_LINEAR:
   case PIPE_CAP_TEXTURE_HALF_FLOAT_LINEAR:
   case PIPE_CAP_DEPTH_BOUNDS_TEST:
   case PIPE_CAP_TGSI_TXQS:
   case PIPE_CAP_FORCE_PERSAMPLE_INTERP:
   case PIPE_CAP_SHAREABLE_SHADERS:
   case PIPE_CAP_CLEAR_TEXTURE:
   case PIPE_CAP_DRAW_PARAMETERS:
   case PIPE_CAP_TGSI_PACK_HALF_FLOAT:
   case PIPE_CAP_MULTI_DRAW_INDIRECT:
   case PIPE_CAP_MULTI_DRAW_INDIRECT_PARAMS:
   case PIPE_CAP_TGSI_FS_POSITION_IS_SYSVAL:
   case PIPE_CAP_TGSI_FS_FACE_IS_INTEGER_SYSVAL:
   case PIPE_CAP_SHADER_BUFFER_OFFSET_ALIGNMENT:
   case PIPE_CAP_INVALIDATE_BUFFER:
   case PIPE_CAP_GENERATE_MIPMAP:
   case PIPE_CAP_STRING_MARKER:
   case PIPE_CAP_SURFACE_REINTERPRET_BLOCKS:
   case PIPE_CAP_QUERY_BUFFER_OBJECT:
   case PIPE_CAP_QUERY_MEMORY_INFO:
   case PIPE_CAP_PCI_GROUP:
   case PIPE_CAP_PCI_BUS:
   case PIPE_CAP_PCI_DEVICE:
   case PIPE_CAP_PCI_FUNCTION:
   case PIPE_CAP_PRIMITIVE_RESTART_FOR_PATCHES:
   case PIPE_CAP_TGSI_VOTE:
   case PIPE_CAP_MAX_WINDOW_RECTANGLES:
   case PIPE_CAP_POLYGON_OFFSET_UNITS_UNSCALED:
   case PIPE_CAP_VIEWPORT_SUBPIXEL_BITS:
   case PIPE_CAP_MIXED_COLOR_DEPTH_BITS:
   case PIPE_CAP_TGSI_ARRAY_COMPONENTS:
   case PIPE_CAP_TGSI_CAN_READ_OUTPUTS:
   case PIPE_CAP_NATIVE_FENCE_FD:
   case PIPE_CAP_GLSL_OPTIMIZE_CONSERVATIVELY:
   case PIPE_CAP_TGSI_FS_FBFETCH:
   case PIPE_CAP_TGSI_MUL_ZERO_WINS:
   case PIPE_CAP_TGSI_TEX_TXF_LZ:
   case PIPE_CAP_TGSI_CLOCK:
   case PIPE_CAP_POLYGON_MODE_FILL_RECTANGLE:
   case PIPE_CAP_SPARSE_BUFFER_PAGE_SIZE:
   case PIPE_CAP_TGSI_BALLOT:
   case PIPE_CAP_TGSI_TES_LAYER_VIEWPORT:
   case PIPE_CAP_CAN_BIND_CONST_BUFFER_AS_VERTEX:
   case PIPE_CAP_POST_DEPTH_COVERAGE:
   case PIPE_CAP_BINDLESS_TEXTURE:
   case PIPE_CAP_NIR_SAMPLERS_AS_DEREF:
   case PIPE_CAP_QUERY_SO_OVERFLOW:
   case PIPE_CAP_MEMOBJ:
   case PIPE_CAP_LOAD_CONSTBUF:
   case PIPE_CAP_TGSI_ANY_REG_AS_ADDRESS:
   case PIPE_CAP_TILE_RASTER_ORDER:
   case PIPE_CAP_MAX_COMBINED_SHADER_OUTPUT_RESOURCES:
   case PIPE_CAP_CONTEXT_PRIORITY_MASK:
      // XXX: TODO: fill these out
      break;
   }
   return 0;
}

static float
iris_get_paramf(struct pipe_screen *pscreen, enum pipe_capf param)
{
   switch (param) {
   case PIPE_CAPF_MAX_LINE_WIDTH:
   case PIPE_CAPF_MAX_LINE_WIDTH_AA:
      return 7.375f;

   case PIPE_CAPF_MAX_POINT_WIDTH:
   case PIPE_CAPF_MAX_POINT_WIDTH_AA:
      return 255.0f;

   case PIPE_CAPF_MAX_TEXTURE_ANISOTROPY:
      return 16.0f;
   case PIPE_CAPF_MAX_TEXTURE_LOD_BIAS:
      return 15.0f;
   case PIPE_CAPF_MIN_CONSERVATIVE_RASTER_DILATE:
   case PIPE_CAPF_MAX_CONSERVATIVE_RASTER_DILATE:
   case PIPE_CAPF_CONSERVATIVE_RASTER_DILATE_GRANULARITY:
      return 0.0f;
   default:
      unreachable("unknown param");
   }
}

static int
iris_get_shader_param(struct pipe_screen *pscreen,
                      enum pipe_shader_type shader,
                      enum pipe_shader_cap param)
{
   struct iris_screen *screen = (struct iris_screen *)pscreen;
   struct brw_compiler *compiler = screen->compiler;

   /* this is probably not totally correct.. but it's a start: */
   switch (param) {
   case PIPE_SHADER_CAP_MAX_INSTRUCTIONS:
      return shader == PIPE_SHADER_FRAGMENT ? 1024 : 16384;
   case PIPE_SHADER_CAP_MAX_ALU_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INDIRECTIONS:
      return shader == PIPE_SHADER_FRAGMENT ? 1024 : 0;

   case PIPE_SHADER_CAP_MAX_CONTROL_FLOW_DEPTH:
      return UINT_MAX;

   case PIPE_SHADER_CAP_MAX_INPUTS:
      return shader == PIPE_SHADER_VERTEX ? 16 : 32;
   case PIPE_SHADER_CAP_MAX_OUTPUTS:
      return 32;
   case PIPE_SHADER_CAP_MAX_CONST_BUFFER_SIZE:
      return 16 * 1024 * sizeof(float);
   case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
      return 16;
   case PIPE_SHADER_CAP_MAX_TEMPS:
      return 256; /* GL_MAX_PROGRAM_TEMPORARIES_ARB */
   case PIPE_SHADER_CAP_TGSI_CONT_SUPPORTED:
      return 0;
   case PIPE_SHADER_CAP_INDIRECT_INPUT_ADDR:
      return !compiler->glsl_compiler_options[shader].EmitNoIndirectInput;
   case PIPE_SHADER_CAP_INDIRECT_OUTPUT_ADDR:
      return !compiler->glsl_compiler_options[shader].EmitNoIndirectOutput;
   case PIPE_SHADER_CAP_INDIRECT_TEMP_ADDR:
      return !compiler->glsl_compiler_options[shader].EmitNoIndirectTemp;
   case PIPE_SHADER_CAP_INDIRECT_CONST_ADDR:
      return 1;
   case PIPE_SHADER_CAP_SUBROUTINES:
      return 0;
   case PIPE_SHADER_CAP_INTEGERS:
   case PIPE_SHADER_CAP_SCALAR_ISA:
      return 1;
   case PIPE_SHADER_CAP_INT64_ATOMICS:
   case PIPE_SHADER_CAP_FP16:
      return 0;
   case PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS:
   case PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS:
   case PIPE_SHADER_CAP_MAX_SHADER_IMAGES:
   case PIPE_SHADER_CAP_MAX_SHADER_BUFFERS:
      return IRIS_MAX_TEXTURE_SAMPLERS;
   case PIPE_SHADER_CAP_MAX_HW_ATOMIC_COUNTERS:
   case PIPE_SHADER_CAP_MAX_HW_ATOMIC_COUNTER_BUFFERS:
      return 0;
   case PIPE_SHADER_CAP_PREFERRED_IR:
      return PIPE_SHADER_IR_NIR;
   case PIPE_SHADER_CAP_SUPPORTED_IRS:
      return 0;
   case PIPE_SHADER_CAP_MAX_UNROLL_ITERATIONS_HINT:
      return 32;
   case PIPE_SHADER_CAP_LOWER_IF_THRESHOLD:
   case PIPE_SHADER_CAP_TGSI_SKIP_MERGE_REGISTERS:
   case PIPE_SHADER_CAP_TGSI_DROUND_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_DFRACEXP_DLDEXP_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_LDEXP_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_FMA_SUPPORTED:
   case PIPE_SHADER_CAP_TGSI_ANY_INOUT_DECL_RANGE:
   case PIPE_SHADER_CAP_TGSI_SQRT_SUPPORTED:
      return 0;
   default:
      unreachable("unknown shader param");
   }
}

static int
iris_get_compute_param(struct pipe_screen *pscreen,
                       enum pipe_shader_ir ir_type,
                       enum pipe_compute_cap param,
                       void *ret)
{
   /* TODO: compute shaders */
   return 0;
}

static uint64_t
iris_get_timestamp(struct pipe_screen *pscreen)
{
   return 0;
}

static void
iris_destroy_screen(struct pipe_screen *pscreen)
{
   struct iris_screen *screen = (struct iris_screen *) pscreen;
   iris_bo_unreference(screen->workaround_bo);
   ralloc_free(screen);
}

static void
iris_fence_reference(struct pipe_screen *screen,
                     struct pipe_fence_handle **ptr,
                     struct pipe_fence_handle *fence)
{
}

static boolean
iris_fence_finish(struct pipe_screen *screen,
                  struct pipe_context *ctx,
                  struct pipe_fence_handle *fence,
                  uint64_t timeout)
{
   return true;
}

static void
iris_query_memory_info(struct pipe_screen *pscreen,
                       struct pipe_memory_info *info)
{
}

static const void *
iris_get_compiler_options(struct pipe_screen *pscreen,
                          enum pipe_shader_ir ir,
                          enum pipe_shader_type pstage)
{
   struct iris_screen *screen = (struct iris_screen *) pscreen;
   gl_shader_stage stage = stage_from_pipe(pstage);
   assert(ir == PIPE_SHADER_IR_NIR);

   return screen->compiler->glsl_compiler_options[stage].NirOptions;
}

static int
iris_getparam(struct iris_screen *screen, int param, int *value)
{
   struct drm_i915_getparam gp = { .param = param, .value = value };

   if (ioctl(screen->fd, DRM_IOCTL_I915_GETPARAM, &gp) == -1)
      return -errno;

   return 0;
}

static bool
iris_getparam_boolean(struct iris_screen *screen, int param)
{
   int value = 0;
   return (iris_getparam(screen, param, &value) == 0) && value;
}

static int
iris_getparam_integer(struct iris_screen *screen, int param)
{
   int value = -1;

   if (iris_getparam(screen, param, &value) == 0)
      return value;

   return -1;
}

static void
iris_shader_debug_log(void *data, const char *fmt, ...)
{
   struct pipe_debug_callback *dbg = data;
   unsigned id = 0;
   va_list args;

   if (!dbg->debug_message)
      return;

   va_start(args, fmt);

   dbg->debug_message(dbg->data, &id, PIPE_DEBUG_TYPE_SHADER_INFO, fmt, args);
   va_end(args);
}

struct pipe_screen *
iris_screen_create(int fd)
{
   struct iris_screen *screen = rzalloc(NULL, struct iris_screen);
   if (!screen)
      return NULL;

   screen->fd = fd;
   screen->pci_id = iris_getparam_integer(screen, I915_PARAM_CHIPSET_ID);

   if (!gen_get_device_info(screen->pci_id, &screen->devinfo))
      return NULL;

   screen->bufmgr = iris_bufmgr_init(&screen->devinfo, fd);
   if (!screen->bufmgr)
      return NULL;

   screen->workaround_bo =
      iris_bo_alloc(screen->bufmgr, "workaround", 4096, IRIS_MEMZONE_OTHER);
   if (!screen->workaround_bo)
      return NULL;

   brw_process_intel_debug_variable();

   bool hw_has_swizzling = false; // XXX: detect?
   isl_device_init(&screen->isl_dev, &screen->devinfo, hw_has_swizzling);

   screen->compiler = brw_compiler_create(screen, &screen->devinfo);
   screen->compiler->shader_debug_log = iris_shader_debug_log;

   struct pipe_screen *pscreen = &screen->base;

   iris_init_screen_resource_functions(pscreen);

   pscreen->destroy = iris_destroy_screen;
   pscreen->get_name = iris_get_name;
   pscreen->get_vendor = iris_get_vendor;
   pscreen->get_device_vendor = iris_get_device_vendor;
   pscreen->get_param = iris_get_param;
   pscreen->get_shader_param = iris_get_shader_param;
   pscreen->get_compute_param = iris_get_compute_param;
   pscreen->get_paramf = iris_get_paramf;
   pscreen->get_compiler_options = iris_get_compiler_options;
   pscreen->is_format_supported = iris_is_format_supported;
   pscreen->context_create = iris_create_context;
   pscreen->flush_frontbuffer = iris_flush_frontbuffer;
   pscreen->get_timestamp = iris_get_timestamp;
   pscreen->fence_reference = iris_fence_reference;
   pscreen->fence_finish = iris_fence_finish;
   pscreen->query_memory_info = iris_query_memory_info;

   return pscreen;
}

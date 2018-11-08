/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iris_resource.c
 *
 * Resources are images, buffers, and other objects used by the GPU.
 *
 * XXX: explain resources
 */

#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/os_memory.h"
#include "util/u_cpu_detect.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_transfer.h"
#include "util/u_transfer_helper.h"
#include "util/u_upload_mgr.h"
#include "util/ralloc.h"
#include "iris_batch.h"
#include "iris_context.h"
#include "iris_resource.h"
#include "iris_screen.h"
#include "intel/common/gen_debug.h"
#include "isl/isl.h"
#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/i915_drm.h"

// XXX: u_transfer_helper...for separate stencil...

enum modifier_priority {
   MODIFIER_PRIORITY_INVALID = 0,
   MODIFIER_PRIORITY_LINEAR,
   MODIFIER_PRIORITY_X,
   MODIFIER_PRIORITY_Y,
   MODIFIER_PRIORITY_Y_CCS,
};

static const uint64_t priority_to_modifier[] = {
   [MODIFIER_PRIORITY_INVALID] = DRM_FORMAT_MOD_INVALID,
   [MODIFIER_PRIORITY_LINEAR] = DRM_FORMAT_MOD_LINEAR,
   [MODIFIER_PRIORITY_X] = I915_FORMAT_MOD_X_TILED,
   [MODIFIER_PRIORITY_Y] = I915_FORMAT_MOD_Y_TILED,
   [MODIFIER_PRIORITY_Y_CCS] = I915_FORMAT_MOD_Y_TILED_CCS,
};

static bool
modifier_is_supported(const struct gen_device_info *devinfo,
                      uint64_t modifier)
{
   /* XXX: do something real */
   switch (modifier) {
   case I915_FORMAT_MOD_Y_TILED:
   case I915_FORMAT_MOD_X_TILED:
   case DRM_FORMAT_MOD_LINEAR:
      return true;
   case I915_FORMAT_MOD_Y_TILED_CCS:
   case DRM_FORMAT_MOD_INVALID:
   default:
      return false;
   }
}

static uint64_t
select_best_modifier(struct gen_device_info *devinfo,
                     const uint64_t *modifiers,
                     int count)
{
   enum modifier_priority prio = MODIFIER_PRIORITY_INVALID;

   for (int i = 0; i < count; i++) {
      if (!modifier_is_supported(devinfo, modifiers[i]))
         continue;

      switch (modifiers[i]) {
      case I915_FORMAT_MOD_Y_TILED_CCS:
         prio = MAX2(prio, MODIFIER_PRIORITY_Y_CCS);
         break;
      case I915_FORMAT_MOD_Y_TILED:
         prio = MAX2(prio, MODIFIER_PRIORITY_Y);
         break;
      case I915_FORMAT_MOD_X_TILED:
         prio = MAX2(prio, MODIFIER_PRIORITY_X);
         break;
      case DRM_FORMAT_MOD_LINEAR:
         prio = MAX2(prio, MODIFIER_PRIORITY_LINEAR);
         break;
      case DRM_FORMAT_MOD_INVALID:
      default:
         break;
      }
   }

   return priority_to_modifier[prio];
}

static enum isl_surf_dim
target_to_isl_surf_dim(enum pipe_texture_target target)
{
   switch (target) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
      return ISL_SURF_DIM_1D;
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_CUBE_ARRAY:
      return ISL_SURF_DIM_2D;
   case PIPE_TEXTURE_3D:
      return ISL_SURF_DIM_3D;
   case PIPE_MAX_TEXTURE_TYPES:
      break;
   }
   unreachable("invalid texture type");
}

static isl_surf_usage_flags_t
pipe_bind_to_isl_usage(unsigned bindings)
{
   isl_surf_usage_flags_t usage = 0;

   if (bindings & PIPE_BIND_RENDER_TARGET)
      usage |= ISL_SURF_USAGE_RENDER_TARGET_BIT;

   if (bindings & PIPE_BIND_SAMPLER_VIEW)
      usage |= ISL_SURF_USAGE_TEXTURE_BIT;

   if (bindings & (PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SHADER_BUFFER))
      usage |= ISL_SURF_USAGE_STORAGE_BIT;

   if (bindings & PIPE_BIND_DISPLAY_TARGET)
      usage |= ISL_SURF_USAGE_DISPLAY_BIT;

   return usage;
}

struct pipe_resource *
iris_resource_get_separate_stencil(struct pipe_resource *p_res)
{
   /* For packed depth-stencil, we treat depth as the primary resource
    * and store S8 as the "second plane" resource.
    */
   return p_res->next;
}

static void
iris_resource_set_separate_stencil(struct pipe_resource *p_res,
                                   struct pipe_resource *stencil)
{
   assert(util_format_has_depth(util_format_description(p_res->format)));
   pipe_resource_reference(&p_res->next, stencil);
}

void
iris_get_depth_stencil_resources(struct pipe_resource *res,
                                 struct iris_resource **out_z,
                                 struct iris_resource **out_s)
{
   if (!res) {
      *out_z = NULL;
      *out_s = NULL;
      return;
   }

   const struct util_format_description *desc =
      util_format_description(res->format);

   if (util_format_has_depth(desc)) {
      *out_z = (void *) res;
      *out_s = (void *) iris_resource_get_separate_stencil(res);
   } else {
      assert(util_format_has_stencil(desc));
      *out_z = NULL;
      *out_s = (void *) res;
   }
}

static void
iris_resource_destroy(struct pipe_screen *screen,
                      struct pipe_resource *resource)
{
   struct iris_resource *res = (struct iris_resource *)resource;

   iris_bo_unreference(res->bo);
   free(res);
}

static struct iris_resource *
iris_alloc_resource(struct pipe_screen *pscreen,
                    const struct pipe_resource *templ)
{
   struct iris_resource *res = calloc(1, sizeof(struct iris_resource));
   if (!res)
      return NULL;

   res->base = *templ;
   res->base.screen = pscreen;
   pipe_reference_init(&res->base.reference, 1);

   return res;
}

static struct pipe_resource *
iris_resource_create_with_modifiers(struct pipe_screen *pscreen,
                                    const struct pipe_resource *templ,
                                    const uint64_t *modifiers,
                                    int modifiers_count)
{
   struct iris_screen *screen = (struct iris_screen *)pscreen;
   struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_resource *res = iris_alloc_resource(pscreen, templ);
   const struct util_format_description *format_desc =
      util_format_description(templ->format);

   if (!res)
      return NULL;

   const bool has_depth = util_format_has_depth(format_desc);
   uint64_t modifier = DRM_FORMAT_MOD_INVALID;

   if (modifiers_count == 0 || !modifiers) {
      if (has_depth) {
         modifier = I915_FORMAT_MOD_Y_TILED;
      } else if (templ->target == PIPE_TEXTURE_1D ||
                 templ->target == PIPE_TEXTURE_1D_ARRAY) {
         modifier = DRM_FORMAT_MOD_LINEAR;
      } else if (templ->bind & PIPE_BIND_DISPLAY_TARGET) {
         /* Display is X-tiled for historical reasons. */
         modifier = I915_FORMAT_MOD_X_TILED;
      } else {
         modifier = I915_FORMAT_MOD_Y_TILED;
      }
      /* XXX: make sure this doesn't do stupid things for internal textures */
   }

   if (templ->target == PIPE_BUFFER || templ->usage == PIPE_USAGE_STAGING)
      modifier = DRM_FORMAT_MOD_LINEAR;

   if (templ->bind & (PIPE_BIND_LINEAR | PIPE_BIND_CURSOR))
      modifier = DRM_FORMAT_MOD_LINEAR;

   if (modifier == DRM_FORMAT_MOD_INVALID) {
      /* User requested specific modifiers */
      modifier = select_best_modifier(devinfo, modifiers, modifiers_count);
      if (modifier == DRM_FORMAT_MOD_INVALID)
         return NULL;
   }

   const struct isl_drm_modifier_info *mod_info =
      isl_drm_modifier_get_info(modifier);

   enum isl_tiling tiling = templ->format == PIPE_FORMAT_S8_UINT ?
      ISL_TILING_W : mod_info->tiling;

   isl_surf_usage_flags_t usage = pipe_bind_to_isl_usage(templ->bind);

   if (templ->target == PIPE_TEXTURE_CUBE ||
       templ->target == PIPE_TEXTURE_CUBE_ARRAY)
      usage |= ISL_SURF_USAGE_CUBE_BIT;

   if (templ->usage != PIPE_USAGE_STAGING) {
      if (templ->format == PIPE_FORMAT_S8_UINT)
         usage |= ISL_SURF_USAGE_STENCIL_BIT;
      else if (has_depth)
         usage |= ISL_SURF_USAGE_DEPTH_BIT;
   }

   enum pipe_format pfmt = templ->format;
   res->internal_format = pfmt;

   /* Should be handled by u_transfer_helper */
   assert(!util_format_is_depth_and_stencil(pfmt));

   struct iris_format_info fmt = iris_format_for_usage(devinfo, pfmt, usage);
   assert(fmt.fmt != ISL_FORMAT_UNSUPPORTED);

   UNUSED const bool isl_surf_created_successfully =
      isl_surf_init(&screen->isl_dev, &res->surf,
                    .dim = target_to_isl_surf_dim(templ->target),
                    .format = fmt.fmt,
                    .width = templ->width0,
                    .height = templ->height0,
                    .depth = templ->depth0,
                    .levels = templ->last_level + 1,
                    .array_len = templ->array_size,
                    .samples = MAX2(templ->nr_samples, 1),
                    .min_alignment_B = 0,
                    .row_pitch_B = 0,
                    .usage = usage,
                    .tiling_flags = 1 << tiling);
   assert(isl_surf_created_successfully);

   enum iris_memory_zone memzone = IRIS_MEMZONE_OTHER;
   const char *name = templ->target == PIPE_BUFFER ? "buffer" : "miptree";
   if (templ->flags & IRIS_RESOURCE_FLAG_SHADER_MEMZONE) {
      memzone = IRIS_MEMZONE_SHADER;
      name = "shader kernels";
   } else if (templ->flags & IRIS_RESOURCE_FLAG_SURFACE_MEMZONE) {
      memzone = IRIS_MEMZONE_SURFACE;
      name = "surface state";
   } else if (templ->flags & IRIS_RESOURCE_FLAG_DYNAMIC_MEMZONE) {
      memzone = IRIS_MEMZONE_DYNAMIC;
      name = "dynamic state";
   }

   res->bo = iris_bo_alloc_tiled(screen->bufmgr, name, res->surf.size_B,
                                 memzone,
                                 isl_tiling_to_i915_tiling(res->surf.tiling),
                                 res->surf.row_pitch_B, 0);
   if (!res->bo)
      goto fail;

   return &res->base;

fail:
   iris_resource_destroy(pscreen, &res->base);
   return NULL;
}

static struct pipe_resource *
iris_resource_create(struct pipe_screen *pscreen,
                     const struct pipe_resource *templ)
{
   return iris_resource_create_with_modifiers(pscreen, templ, NULL, 0);
}

static uint64_t
tiling_to_modifier(uint32_t tiling)
{
   static const uint64_t map[] = {
      [I915_TILING_NONE]   = DRM_FORMAT_MOD_LINEAR,
      [I915_TILING_X]      = I915_FORMAT_MOD_X_TILED,
      [I915_TILING_Y]      = I915_FORMAT_MOD_Y_TILED,
   };

   assert(tiling < ARRAY_SIZE(map));

   return map[tiling];
}

static struct pipe_resource *
iris_resource_from_user_memory(struct pipe_screen *pscreen,
                               const struct pipe_resource *templ,
                               void *user_memory)
{
   struct iris_screen *screen = (struct iris_screen *)pscreen;
   struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_bufmgr *bufmgr = screen->bufmgr;
   struct iris_resource *res = iris_alloc_resource(pscreen, templ);
   if (!res)
      return NULL;

   res->bo = iris_bo_create_userptr(bufmgr, "user",
                                    user_memory, templ->width0,
                                    IRIS_MEMZONE_OTHER);
   if (!res->bo) {
      free(res);
      return NULL;
   }

   res->internal_format = templ->format;

   // XXX: usage...
   isl_surf_usage_flags_t isl_usage = 0;

   const struct iris_format_info fmt =
      iris_format_for_usage(devinfo, templ->format, isl_usage);

   isl_surf_init(&screen->isl_dev, &res->surf,
                 .dim = target_to_isl_surf_dim(templ->target),
                 .format = fmt.fmt,
                 .width = templ->width0,
                 .height = templ->height0,
                 .depth = templ->depth0,
                 .levels = templ->last_level + 1,
                 .array_len = templ->array_size,
                 .samples = MAX2(templ->nr_samples, 1),
                 .min_alignment_B = 0,
                 .row_pitch_B = 0,
                 .usage = isl_usage,
                 .tiling_flags = 1 << ISL_TILING_LINEAR);

   assert(res->bo->tiling_mode == isl_tiling_to_i915_tiling(res->surf.tiling));

   return &res->base;
}

static struct pipe_resource *
iris_resource_from_handle(struct pipe_screen *pscreen,
                          const struct pipe_resource *templ,
                          struct winsys_handle *whandle,
                          unsigned usage)
{
   struct iris_screen *screen = (struct iris_screen *)pscreen;
   struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_bufmgr *bufmgr = screen->bufmgr;
   struct iris_resource *res = iris_alloc_resource(pscreen, templ);
   if (!res)
      return NULL;

   if (whandle->offset != 0) {
      dbg_printf("Attempt to import unsupported winsys offset %u\n",
                 whandle->offset);
      goto fail;
   }

   switch (whandle->type) {
   case WINSYS_HANDLE_TYPE_FD:
      res->bo = iris_bo_import_dmabuf(bufmgr, whandle->handle);
      break;
   case WINSYS_HANDLE_TYPE_SHARED:
      res->bo = iris_bo_gem_create_from_name(bufmgr, "winsys image",
                                             whandle->handle);
      break;
   default:
      unreachable("invalid winsys handle type");
   }
   if (!res->bo)
      return NULL;

   uint64_t modifier = whandle->modifier;
   if (modifier == DRM_FORMAT_MOD_INVALID) {
	  modifier = tiling_to_modifier(res->bo->tiling_mode);
   }
   const struct isl_drm_modifier_info *mod_info =
      isl_drm_modifier_get_info(modifier);
   assert(mod_info);

   // XXX: usage...
   isl_surf_usage_flags_t isl_usage = ISL_SURF_USAGE_DISPLAY_BIT;

   const struct iris_format_info fmt =
      iris_format_for_usage(devinfo, templ->format, isl_usage);

   isl_surf_init(&screen->isl_dev, &res->surf,
                 .dim = target_to_isl_surf_dim(templ->target),
                 .format = fmt.fmt,
                 .width = templ->width0,
                 .height = templ->height0,
                 .depth = templ->depth0,
                 .levels = templ->last_level + 1,
                 .array_len = templ->array_size,
                 .samples = MAX2(templ->nr_samples, 1),
                 .min_alignment_B = 0,
                 .row_pitch_B = 0,
                 .usage = isl_usage,
                 .tiling_flags = 1 << mod_info->tiling);

   assert(res->bo->tiling_mode == isl_tiling_to_i915_tiling(res->surf.tiling));

   return &res->base;

fail:
   iris_resource_destroy(pscreen, &res->base);
   return NULL;
}

static boolean
iris_resource_get_handle(struct pipe_screen *pscreen,
                         struct pipe_context *ctx,
                         struct pipe_resource *resource,
                         struct winsys_handle *whandle,
                         unsigned usage)
{
   struct iris_resource *res = (struct iris_resource *)resource;

   whandle->stride = res->surf.row_pitch_B;
   whandle->modifier = tiling_to_modifier(res->bo->tiling_mode);

   switch (whandle->type) {
   case WINSYS_HANDLE_TYPE_SHARED:
      return iris_bo_flink(res->bo, &whandle->handle) == 0;
   case WINSYS_HANDLE_TYPE_KMS:
      return iris_bo_export_gem_handle(res->bo) != 0;
   case WINSYS_HANDLE_TYPE_FD:
      return iris_bo_export_dmabuf(res->bo, (int *) &whandle->handle) == 0;
   }

   return false;
}

static void
get_image_offset_el(struct isl_surf *surf, unsigned level, unsigned z,
                    unsigned *out_x0_el, unsigned *out_y0_el)
{
   if (surf->dim == ISL_SURF_DIM_3D) {
      isl_surf_get_image_offset_el(surf, level, 0, z, out_x0_el, out_y0_el);
   } else {
      isl_surf_get_image_offset_el(surf, level, z, 0, out_x0_el, out_y0_el);
   }
}

/**
 * Get pointer offset into stencil buffer.
 *
 * The stencil buffer is W tiled. Since the GTT is incapable of W fencing, we
 * must decode the tile's layout in software.
 *
 * See
 *   - PRM, 2011 Sandy Bridge, Volume 1, Part 2, Section 4.5.2.1 W-Major Tile
 *     Format.
 *   - PRM, 2011 Sandy Bridge, Volume 1, Part 2, Section 4.5.3 Tiling Algorithm
 *
 * Even though the returned offset is always positive, the return type is
 * signed due to
 *    commit e8b1c6d6f55f5be3bef25084fdd8b6127517e137
 *    mesa: Fix return type of  _mesa_get_format_bytes() (#37351)
 */
static intptr_t
s8_offset(uint32_t stride, uint32_t x, uint32_t y, bool swizzled)
{
   uint32_t tile_size = 4096;
   uint32_t tile_width = 64;
   uint32_t tile_height = 64;
   uint32_t row_size = 64 * stride / 2; /* Two rows are interleaved. */

   uint32_t tile_x = x / tile_width;
   uint32_t tile_y = y / tile_height;

   /* The byte's address relative to the tile's base addres. */
   uint32_t byte_x = x % tile_width;
   uint32_t byte_y = y % tile_height;

   uintptr_t u = tile_y * row_size
               + tile_x * tile_size
               + 512 * (byte_x / 8)
               +  64 * (byte_y / 8)
               +  32 * ((byte_y / 4) % 2)
               +  16 * ((byte_x / 4) % 2)
               +   8 * ((byte_y / 2) % 2)
               +   4 * ((byte_x / 2) % 2)
               +   2 * (byte_y % 2)
               +   1 * (byte_x % 2);

   if (swizzled) {
      /* adjust for bit6 swizzling */
      if (((byte_x / 8) % 2) == 1) {
	 if (((byte_y / 8) % 2) == 0) {
	    u += 64;
	 } else {
	    u -= 64;
	 }
      }
   }

   return u;
}

static void
iris_unmap_s8(struct iris_transfer *map)
{
   struct pipe_transfer *xfer = &map->base;
   struct iris_resource *res = (struct iris_resource *) xfer->resource;
   struct isl_surf *surf = &res->surf;
   const bool has_swizzling = false;

   if (xfer->usage & PIPE_TRANSFER_WRITE) {
      uint8_t *untiled_s8_map = map->ptr;
      uint8_t *tiled_s8_map =
         iris_bo_map(map->dbg, res->bo, xfer->usage | MAP_RAW);

      struct pipe_box box = xfer->box;

      for (int s = 0; s < box.depth; s++) {
         unsigned x0_el, y0_el;
         get_image_offset_el(surf, xfer->level, box.z, &x0_el, &y0_el);

         for (uint32_t y = 0; y < box.height; y++) {
            for (uint32_t x = 0; x < box.width; x++) {
               ptrdiff_t offset = s8_offset(surf->row_pitch_B,
                                            x0_el + box.x + x,
                                            y0_el + box.y + y,
                                            has_swizzling);
               tiled_s8_map[offset] =
                  untiled_s8_map[s * xfer->layer_stride + y * xfer->stride + x];
            }
         }

         box.z++;
      }
   }

   free(map->buffer);
}

static void
iris_map_s8(struct iris_transfer *map)
{
   struct pipe_transfer *xfer = &map->base;
   struct iris_resource *res = (struct iris_resource *) xfer->resource;
   struct isl_surf *surf = &res->surf;

   xfer->stride = surf->row_pitch_B;
   xfer->layer_stride = xfer->stride * xfer->box.height;

   /* The tiling and detiling functions require that the linear buffer has
    * a 16-byte alignment (that is, its `x0` is 16-byte aligned).  Here we
    * over-allocate the linear buffer to get the proper alignment.
    */
   map->buffer = map->ptr = malloc(xfer->layer_stride * xfer->box.depth);
   assert(map->buffer);

   const bool has_swizzling = false;

   /* One of either READ_BIT or WRITE_BIT or both is set.  READ_BIT implies no
    * INVALIDATE_RANGE_BIT.  WRITE_BIT needs the original values read in unless
    * invalidate is set, since we'll be writing the whole rectangle from our
    * temporary buffer back out.
    */
   if (!(xfer->usage & PIPE_TRANSFER_DISCARD_RANGE)) {
      uint8_t *untiled_s8_map = map->ptr;
      uint8_t *tiled_s8_map =
         iris_bo_map(map->dbg, res->bo, xfer->usage | MAP_RAW);

      struct pipe_box box = xfer->box;

      for (int s = 0; s < box.depth; s++) {
         unsigned x0_el, y0_el;
         get_image_offset_el(surf, xfer->level, box.z, &x0_el, &y0_el);

         for (uint32_t y = 0; y < box.height; y++) {
            for (uint32_t x = 0; x < box.width; x++) {
               ptrdiff_t offset = s8_offset(surf->row_pitch_B,
                                            x0_el + box.x + x,
                                            y0_el + box.y + y,
                                            has_swizzling);
               untiled_s8_map[s * xfer->layer_stride + y * xfer->stride + x] =
                  tiled_s8_map[offset];
            }
         }

         box.z++;
      }
   }

   map->unmap = iris_unmap_s8;
}

/* Compute extent parameters for use with tiled_memcpy functions.
 * xs are in units of bytes and ys are in units of strides.
 */
static inline void
tile_extents(struct isl_surf *surf,
             const struct pipe_box *box,
             unsigned level,
             unsigned *x1_B, unsigned *x2_B,
             unsigned *y1_el, unsigned *y2_el)
{
   const struct isl_format_layout *fmtl = isl_format_get_layout(surf->format);
   const unsigned cpp = fmtl->bpb / 8;

   assert(box->x % fmtl->bw == 0);
   assert(box->y % fmtl->bh == 0);

   unsigned x0_el, y0_el;
   get_image_offset_el(surf, level, box->z, &x0_el, &y0_el);

   *x1_B = (box->x / fmtl->bw + x0_el) * cpp;
   *y1_el = box->y / fmtl->bh + y0_el;
   *x2_B = (DIV_ROUND_UP(box->x + box->width, fmtl->bw) + x0_el) * cpp;
   *y2_el = DIV_ROUND_UP(box->y + box->height, fmtl->bh) + y0_el;
}

static void
iris_unmap_tiled_memcpy(struct iris_transfer *map)
{
   struct pipe_transfer *xfer = &map->base;
   struct pipe_box box = xfer->box;
   struct iris_resource *res = (struct iris_resource *) xfer->resource;
   struct isl_surf *surf = &res->surf;

   const bool has_swizzling = false;

   if (xfer->usage & PIPE_TRANSFER_WRITE) {
      char *dst = iris_bo_map(map->dbg, res->bo, xfer->usage | MAP_RAW);

      for (int s = 0; s < box.depth; s++) {
         unsigned x1, x2, y1, y2;
         tile_extents(surf, &box, xfer->level, &x1, &x2, &y1, &y2);

         void *ptr = map->ptr + s * xfer->layer_stride;

         isl_memcpy_linear_to_tiled(x1, x2, y1, y2, dst, ptr,
                                    surf->row_pitch_B, xfer->stride,
                                    has_swizzling, surf->tiling, ISL_MEMCPY);
         box.z++;
      }
   }
   os_free_aligned(map->buffer);
   map->buffer = map->ptr = NULL;
}

static void
iris_map_tiled_memcpy(struct iris_transfer *map)
{
   struct pipe_transfer *xfer = &map->base;
   struct iris_resource *res = (struct iris_resource *) xfer->resource;
   struct isl_surf *surf = &res->surf;

   xfer->stride = ALIGN(surf->row_pitch_B, 16);
   xfer->layer_stride = xfer->stride * xfer->box.height;

   unsigned x1, x2, y1, y2;
   tile_extents(surf, &xfer->box, xfer->level, &x1, &x2, &y1, &y2);

   /* The tiling and detiling functions require that the linear buffer has
    * a 16-byte alignment (that is, its `x0` is 16-byte aligned).  Here we
    * over-allocate the linear buffer to get the proper alignment.
    */
   map->buffer =
      os_malloc_aligned(xfer->layer_stride * xfer->box.depth, 16);
   assert(map->buffer);
   map->ptr = (char *)map->buffer + (x1 & 0xf);

   const bool has_swizzling = false;

   // XXX: PIPE_TRANSFER_READ?
   if (!(xfer->usage & PIPE_TRANSFER_DISCARD_RANGE)) {
      char *src = iris_bo_map(map->dbg, res->bo, xfer->usage | MAP_RAW);

      struct pipe_box box = xfer->box;

      for (int s = 0; s < box.depth; s++) {
         unsigned x1, x2, y1, y2;
         tile_extents(surf, &box, xfer->level, &x1, &x2, &y1, &y2);

         /* When transferring cubes, box.depth is counted in cubes, but
          * box.z is counted in faces.  We want to transfer only the
          * specified face, but for all array elements.  So, use 's'
          * (the zero-based slice count) rather than box.z.
          */
         void *ptr = map->ptr + s * xfer->layer_stride;

         isl_memcpy_tiled_to_linear(x1, x2, y1, y2, ptr, src, xfer->stride,
                                    surf->row_pitch_B, has_swizzling,
                                    surf->tiling, ISL_MEMCPY);
         box.z++;
      }
   }

   map->unmap = iris_unmap_tiled_memcpy;
}

static void
iris_map_direct(struct iris_transfer *map)
{
   struct pipe_transfer *xfer = &map->base;
   struct pipe_box *box = &xfer->box;
   struct iris_resource *res = (struct iris_resource *) xfer->resource;
   struct isl_surf *surf = &res->surf;
   const struct isl_format_layout *fmtl = isl_format_get_layout(surf->format);
   const unsigned cpp = fmtl->bpb / 8;
   unsigned x0_el, y0_el;

   get_image_offset_el(surf, xfer->level, box->z, &x0_el, &y0_el);

   xfer->stride = isl_surf_get_row_pitch_B(surf);
   xfer->layer_stride = isl_surf_get_array_pitch(surf);

   void *ptr = iris_bo_map(map->dbg, res->bo, xfer->usage);

   map->ptr = ptr + (y0_el + box->y) * xfer->stride + (x0_el + box->x) * cpp;
}

static void *
iris_transfer_map(struct pipe_context *ctx,
                  struct pipe_resource *resource,
                  unsigned level,
                  enum pipe_transfer_usage usage,
                  const struct pipe_box *box,
                  struct pipe_transfer **ptransfer)
{
   struct iris_context *ice = (struct iris_context *)ctx;
   struct iris_resource *res = (struct iris_resource *)resource;
   struct isl_surf *surf = &res->surf;

   if (surf->tiling != ISL_TILING_LINEAR &&
       (usage & PIPE_TRANSFER_MAP_DIRECTLY))
      return NULL;

   if (!(usage & PIPE_TRANSFER_UNSYNCHRONIZED) &&
       iris_batch_references(&ice->render_batch, res->bo)) {
      iris_batch_flush(&ice->render_batch);
   }

   if (!(usage & PIPE_TRANSFER_UNSYNCHRONIZED) &&
       iris_batch_references(&ice->compute_batch, res->bo)) {
      iris_batch_flush(&ice->compute_batch);
   }

   if ((usage & PIPE_TRANSFER_DONTBLOCK) && iris_bo_busy(res->bo))
      return NULL;

   struct iris_transfer *map = slab_alloc(&ice->transfer_pool);
   struct pipe_transfer *xfer = &map->base;

   // PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE
   // PIPE_TRANSFER_DISCARD_RANGE

   if (!map)
      return NULL;

   memset(map, 0, sizeof(*map));
   map->dbg = &ice->dbg;

   pipe_resource_reference(&xfer->resource, resource);
   xfer->level = level;
   xfer->usage = usage;
   xfer->box = *box;
   *ptransfer = xfer;

   xfer->usage &= (PIPE_TRANSFER_READ |
                   PIPE_TRANSFER_WRITE |
                   PIPE_TRANSFER_UNSYNCHRONIZED |
                   PIPE_TRANSFER_PERSISTENT |
                   PIPE_TRANSFER_COHERENT |
                   PIPE_TRANSFER_DISCARD_RANGE);

   if (surf->tiling == ISL_TILING_W) {
      // XXX: just teach iris_map_tiled_memcpy about W tiling...
      iris_map_s8(map);
   } else if (surf->tiling != ISL_TILING_LINEAR) {
      iris_map_tiled_memcpy(map);
   } else {
      iris_map_direct(map);
   }

   return map->ptr;
}

static void
iris_transfer_unmap(struct pipe_context *ctx, struct pipe_transfer *xfer)
{
   struct iris_context *ice = (struct iris_context *)ctx;
   struct iris_transfer *map = (void *) xfer;
   struct iris_resource *res = (struct iris_resource *) xfer->resource;
   struct isl_surf *surf = &res->surf;

   if (map->unmap)
      map->unmap(map);

   /* XXX: big ol' hack!  need to re-emit UBOs.  want bind_history? */
   if (surf->tiling == ISL_TILING_LINEAR) {
      ice->state.dirty |= IRIS_DIRTY_CONSTANTS_VS  | IRIS_DIRTY_BINDINGS_VS
                       |  IRIS_DIRTY_CONSTANTS_TCS | IRIS_DIRTY_BINDINGS_TCS
                       |  IRIS_DIRTY_CONSTANTS_TES | IRIS_DIRTY_BINDINGS_TES
                       |  IRIS_DIRTY_CONSTANTS_GS  | IRIS_DIRTY_BINDINGS_GS
                       |  IRIS_DIRTY_CONSTANTS_FS  | IRIS_DIRTY_BINDINGS_FS;
   }

   pipe_resource_reference(&xfer->resource, NULL);
   slab_free(&ice->transfer_pool, map);
}

static void
iris_flush_resource(struct pipe_context *ctx, struct pipe_resource *resource)
{
}

static enum pipe_format
iris_resource_get_internal_format(struct pipe_resource *p_res)
{
   struct iris_resource *res = (void *) p_res;
   return res->internal_format;
}

static const struct u_transfer_vtbl transfer_vtbl = {
   .resource_create       = iris_resource_create,
   .resource_destroy      = iris_resource_destroy,
   .transfer_map          = iris_transfer_map,
   .transfer_unmap        = iris_transfer_unmap,
   .transfer_flush_region = u_default_transfer_flush_region,
   .get_internal_format   = iris_resource_get_internal_format,
   .set_stencil           = iris_resource_set_separate_stencil,
   .get_stencil           = iris_resource_get_separate_stencil,
};

void
iris_init_screen_resource_functions(struct pipe_screen *pscreen)
{
   pscreen->resource_create_with_modifiers =
      iris_resource_create_with_modifiers;
   pscreen->resource_create = u_transfer_helper_resource_create;
   pscreen->resource_from_user_memory = iris_resource_from_user_memory;
   pscreen->resource_from_handle = iris_resource_from_handle;
   pscreen->resource_get_handle = iris_resource_get_handle;
   pscreen->resource_destroy = u_transfer_helper_resource_destroy;
   pscreen->transfer_helper =
      u_transfer_helper_create(&transfer_vtbl, true, true, false, true);
}

void
iris_init_resource_functions(struct pipe_context *ctx)
{
   ctx->flush_resource = iris_flush_resource;
   ctx->transfer_map = u_transfer_helper_transfer_map;
   ctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;
   ctx->transfer_unmap = u_transfer_helper_transfer_unmap;
   ctx->buffer_subdata = u_default_buffer_subdata;
   ctx->texture_subdata = u_default_texture_subdata;
}

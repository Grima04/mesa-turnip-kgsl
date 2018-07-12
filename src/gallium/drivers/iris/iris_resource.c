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
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/os_memory.h"
#include "util/u_cpu_detect.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_transfer.h"
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
   if (!res)
      return NULL;

   bool depth = util_format_is_depth_or_stencil(templ->format);

   uint64_t modifier = DRM_FORMAT_MOD_INVALID;

   if (modifiers_count == 0 || !modifiers) {
      if (depth) {
         modifier = I915_FORMAT_MOD_Y_TILED;
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

   isl_surf_usage_flags_t usage = pipe_bind_to_isl_usage(templ->bind);

   if (templ->target == PIPE_TEXTURE_CUBE)
      usage |= ISL_SURF_USAGE_CUBE_BIT;

   // XXX: separate stencil...
   enum pipe_format pfmt = templ->format;

   if (util_format_is_depth_or_stencil(pfmt) &&
       templ->usage != PIPE_USAGE_STAGING)
      usage |= ISL_SURF_USAGE_DEPTH_BIT;

   if (util_format_is_depth_and_stencil(pfmt)) {
      // XXX: Z32S8
      pfmt = PIPE_FORMAT_X8Z24_UNORM;
   }

   enum isl_format isl_format = iris_isl_format_for_pipe_format(pfmt);
   assert(isl_format != ISL_FORMAT_UNSUPPORTED);

   UNUSED const bool isl_surf_created_successfully =
      isl_surf_init(&screen->isl_dev, &res->surf,
                    .dim = target_to_isl_surf_dim(templ->target),
                    .format = isl_format,
                    .width = templ->width0,
                    .height = templ->height0,
                    .depth = templ->depth0,
                    .levels = templ->last_level + 1,
                    .array_len = templ->array_size,
                    .samples = MAX2(templ->nr_samples, 1),
                    .min_alignment_B = 0,
                    .row_pitch_B = 0,
                    .usage = usage,
                    .tiling_flags = 1 << mod_info->tiling);
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
iris_resource_from_handle(struct pipe_screen *pscreen,
                          const struct pipe_resource *templ,
                          struct winsys_handle *whandle,
                          unsigned usage)
{
   struct iris_screen *screen = (struct iris_screen *)pscreen;
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

   isl_surf_init(&screen->isl_dev, &res->surf,
                 .dim = target_to_isl_surf_dim(templ->target),
                 .format = iris_isl_format_for_pipe_format(templ->format),
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

/* Compute extent parameters for use with tiled_memcpy functions.
 * xs are in units of bytes and ys are in units of strides.
 */
static inline void
tile_extents(struct isl_surf *surf,
             const struct pipe_box *box,
             unsigned int level,
             unsigned int *x1_B, unsigned int *x2_B,
             unsigned int *y1_el, unsigned int *y2_el)
{
   const struct isl_format_layout *fmtl = isl_format_get_layout(surf->format);
   const unsigned cpp = fmtl->bpb / 8;

   assert(box->x % fmtl->bw == 0);
   assert(box->y % fmtl->bh == 0);

   unsigned x0_el, y0_el;
   if (surf->dim == ISL_SURF_DIM_3D) {
      isl_surf_get_image_offset_el(surf, level, 0, box->z, &x0_el, &y0_el);
   } else {
      isl_surf_get_image_offset_el(surf, level, box->z, 0, &x0_el, &y0_el);
   }

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

   const bool has_swizzling = false; // XXX: swizzling?

   if (xfer->usage & PIPE_TRANSFER_WRITE) {
      char *dst = iris_bo_map(map->dbg, res->bo, xfer->usage | MAP_RAW);

      for (int s = 0; s < box.depth; s++) {
         unsigned int x1, x2, y1, y2;
         tile_extents(surf, &box, xfer->level, &x1, &x2, &y1, &y2);

         void *ptr = map->ptr + box.z * xfer->layer_stride;

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

   const bool has_swizzling = false; // XXX: swizzling?

   // XXX: PIPE_TRANSFER_READ?
   if (!(xfer->usage & PIPE_TRANSFER_DISCARD_RANGE)) {
      char *src = iris_bo_map(map->dbg, res->bo, xfer->usage | MAP_RAW);

      struct pipe_box box = xfer->box;

      for (int s = 0; s < box.depth; s++) {
         unsigned int x1, x2, y1, y2;
         tile_extents(surf, &box, xfer->level, &x1, &x2, &y1, &y2);

         isl_memcpy_tiled_to_linear(x1, x2, y1, y2, map->ptr, src,
                                    xfer->stride, surf->row_pitch_B,
                                    has_swizzling, surf->tiling, ISL_MEMCPY);
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

   xfer->stride = isl_surf_get_row_pitch_B(surf);
   xfer->layer_stride = isl_surf_get_array_pitch(surf);

   void *ptr = iris_bo_map(map->dbg, res->bo, xfer->usage);

   // XXX: level, layer, etc
   assert(xfer->level == 0);
   assert(box->z == 0);

   map->ptr = ptr + box->y * xfer->stride + box->x * cpp;
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

   if (surf->tiling != ISL_TILING_LINEAR) {
      iris_map_tiled_memcpy(map);
   } else {
      iris_map_direct(map);
   }

   return map->ptr;
}

static void
iris_transfer_flush_region(struct pipe_context *pipe,
                           struct pipe_transfer *transfer,
                           const struct pipe_box *box)
{
}

static void
iris_transfer_unmap(struct pipe_context *ctx, struct pipe_transfer *xfer)
{
   struct iris_context *ice = (struct iris_context *)ctx;
   struct iris_transfer *map = (void *) xfer;

   if (map->unmap)
      map->unmap(map);

   pipe_resource_reference(&xfer->resource, NULL);
   slab_free(&ice->transfer_pool, map);
}

static void
iris_flush_resource(struct pipe_context *ctx, struct pipe_resource *resource)
{
}

void
iris_init_screen_resource_functions(struct pipe_screen *pscreen)
{
   pscreen->resource_create_with_modifiers =
      iris_resource_create_with_modifiers;
   pscreen->resource_create = iris_resource_create;
   pscreen->resource_from_handle = iris_resource_from_handle;
   pscreen->resource_get_handle = iris_resource_get_handle;
   pscreen->resource_destroy = iris_resource_destroy;
}

void
iris_init_resource_functions(struct pipe_context *ctx)
{
   ctx->flush_resource = iris_flush_resource;
   ctx->transfer_map = iris_transfer_map;
   ctx->transfer_flush_region = iris_transfer_flush_region;
   ctx->transfer_unmap = iris_transfer_unmap;
   ctx->buffer_subdata = u_default_buffer_subdata;
   ctx->texture_subdata = u_default_texture_subdata;
}

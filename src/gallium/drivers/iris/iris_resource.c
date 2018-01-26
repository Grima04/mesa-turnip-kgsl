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
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_upload_mgr.h"
#include "util/ralloc.h"
#include "iris_batch.h"
#include "iris_context.h"
#include "iris_resource.h"
#include "iris_screen.h"
#include "intel/common/gen_debug.h"
#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/i915_drm.h"

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

   if (bindings & PIPE_BIND_DEPTH_STENCIL)
      usage |= ISL_SURF_USAGE_DEPTH_BIT | ISL_SURF_USAGE_STENCIL_BIT;

   if (bindings & PIPE_BIND_RENDER_TARGET)
      usage |= ISL_SURF_USAGE_RENDER_TARGET_BIT;

   if (bindings & PIPE_BIND_SHADER_IMAGE)
      usage |= ISL_SURF_USAGE_STORAGE_BIT;

   if (bindings & PIPE_BIND_DISPLAY_TARGET)
      usage |= ISL_SURF_USAGE_DISPLAY_BIT;

   /* XXX: what to do with these? */
   if (bindings & PIPE_BIND_BLENDABLE)
      ;
   if (bindings & PIPE_BIND_SAMPLER_VIEW)
      ;
   if (bindings & PIPE_BIND_VERTEX_BUFFER)
      ;
   if (bindings & PIPE_BIND_INDEX_BUFFER)
      ;
   if (bindings & PIPE_BIND_CONSTANT_BUFFER)
      ;

   if (bindings & PIPE_BIND_STREAM_OUTPUT)
      ;
   if (bindings & PIPE_BIND_CURSOR)
      ;
   if (bindings & PIPE_BIND_CUSTOM)
      ;

   if (bindings & PIPE_BIND_GLOBAL)
      ;
   if (bindings & PIPE_BIND_SHADER_BUFFER)
      ;
   if (bindings & PIPE_BIND_COMPUTE_RESOURCE)
      ;
   if (bindings & PIPE_BIND_COMMAND_ARGS_BUFFER)
      ;
   if (bindings & PIPE_BIND_QUERY_BUFFER)
      ;

   return usage;
}

static void
iris_resource_destroy(struct pipe_screen *screen,
                      struct pipe_resource *resource)
{
   struct iris_resource *res = (struct iris_resource *)resource;

   iris_bo_unreference(res->bo);
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

   uint64_t modifier = DRM_FORMAT_MOD_INVALID;

   if (modifiers_count == 0) {
      /* Display is X-tiled for historical reasons. */
      modifier = (templ->bind & PIPE_BIND_DISPLAY_TARGET) ?
                 I915_FORMAT_MOD_X_TILED : I915_FORMAT_MOD_Y_TILED;
      /* XXX: make sure this doesn't do stupid things for internal textures */
   }

   if (templ->target == PIPE_BUFFER)
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
                 .usage = usage,
                 .tiling_flags = 1 << mod_info->tiling);

   res->bo = iris_bo_alloc_tiled(screen->bufmgr, "resource", res->surf.size_B,
                                 isl_tiling_to_i915_tiling(res->surf.tiling),
                                 res->surf.row_pitch_B, 0);
   if (!res->bo)
      goto fail;

   if (templ->flags & IRIS_RESOURCE_FLAG_INSTRUCTION_CACHE) {
      // XXX: p_atomic_add is backwards :(
      res->bo->gtt_offset = __atomic_fetch_add(&screen->next_instruction_address, res->bo->size, __ATOMIC_ACQ_REL);
   }

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
   case WINSYS_HANDLE_TYPE_SHARED:
      res->bo = iris_bo_import_dmabuf(bufmgr, whandle->handle);
      break;
   case WINSYS_HANDLE_TYPE_FD:
      res->bo = iris_bo_gem_create_from_name(bufmgr, "winsys image",
                                             whandle->handle);
      break;
   default:
      unreachable("invalid winsys handle type");
   }

   const struct isl_drm_modifier_info *mod_info =
      isl_drm_modifier_get_info(whandle->modifier);

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
      return iris_bo_flink(res->bo, &whandle->handle) > 0;
   case WINSYS_HANDLE_TYPE_KMS:
      return iris_bo_export_gem_handle(res->bo);
   case WINSYS_HANDLE_TYPE_FD:
      return iris_bo_export_dmabuf(res->bo, (int *) &whandle->handle) > 0;
   }

   return false;
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
   struct pipe_transfer *transfer;

   // PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE
   // PIPE_TRANSFER_DISCARD_RANGE
   // PIPE_TRANSFER_MAP_DIRECTLY

   transfer = calloc(1, sizeof(struct pipe_transfer));
   if (!transfer)
      return NULL;

   pipe_resource_reference(&transfer->resource, resource);
   transfer->level = level;
   transfer->usage = usage;
   transfer->box = *box;
   transfer->stride = 1;
   transfer->layer_stride = 1;
   *ptransfer = transfer;

   if (!(usage & PIPE_TRANSFER_UNSYNCHRONIZED) &&
       iris_batch_references(&ice->render_batch, res->bo)) {
      iris_batch_flush(&ice->render_batch);
   }

   if ((usage & PIPE_TRANSFER_DONTBLOCK) && iris_bo_busy(res->bo))
      return NULL;

   usage &= (PIPE_TRANSFER_READ |
             PIPE_TRANSFER_WRITE |
             PIPE_TRANSFER_UNSYNCHRONIZED |
             PIPE_TRANSFER_PERSISTENT |
             PIPE_TRANSFER_COHERENT);

   return iris_bo_map(&ice->dbg, res->bo, usage);
}

static void
iris_transfer_flush_region(struct pipe_context *pipe,
                           struct pipe_transfer *transfer,
                           const struct pipe_box *box)
{
}

static void
iris_transfer_unmap(struct pipe_context *pipe,
                    struct pipe_transfer *transfer)
{
   pipe_resource_reference(&transfer->resource, NULL);
   free(transfer);
}

static void
iris_buffer_subdata(struct pipe_context *pipe,
                    struct pipe_resource *resource,
                    unsigned usage, unsigned offset,
                    unsigned size, const void *data)
{
}

static void
iris_texture_subdata(struct pipe_context *pipe,
                     struct pipe_resource *resource,
                     unsigned level,
                     unsigned usage,
                     const struct pipe_box *box,
                     const void *data,
                     unsigned stride,
                     unsigned layer_stride)
{
}


static void
iris_resource_copy_region(struct pipe_context *ctx,
                          struct pipe_resource *dst,
                          unsigned dst_level,
                          unsigned dstx, unsigned dsty, unsigned dstz,
                          struct pipe_resource *src,
                          unsigned src_level,
                          const struct pipe_box *src_box)
{
}

static void
iris_flush_resource(struct pipe_context *ctx, struct pipe_resource *resource)
{
}

static boolean
iris_generate_mipmap(struct pipe_context *ctx,
                     struct pipe_resource *resource,
                     enum pipe_format format,
                     unsigned base_level,
                     unsigned last_level,
                     unsigned first_layer,
                     unsigned last_layer)
{
   return true;
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
   ctx->buffer_subdata = iris_buffer_subdata;
   ctx->texture_subdata = iris_texture_subdata;
   ctx->resource_copy_region = iris_resource_copy_region;
}

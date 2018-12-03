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
 * @file iris_resolve.c
 *
 * This file handles resolve tracking for main and auxiliary surfaces.
 *
 * It also handles our cache tracking.  We have sets for the render cache,
 * depth cache, and so on.  If a BO is in a cache's set, then it may have
 * data in that cache.  The helpers take care of emitting flushes for
 * render-to-texture, format reinterpretation issues, and other situations.
 */

#include "util/hash_table.h"
#include "util/set.h"
#include "iris_context.h"

static void
resolve_sampler_views(struct iris_batch *batch,
                      struct iris_shader_state *shs)
{
   uint32_t views = shs->bound_sampler_views;

   while (views) {
      const int i = u_bit_scan(&views);
      struct iris_sampler_view *isv = shs->textures[i];
      struct iris_resource *res = (void *) isv->base.texture;

      // XXX: aux tracking
      iris_cache_flush_for_read(batch, res->bo);
   }
}

static void
resolve_image_views(struct iris_batch *batch,
                    struct iris_shader_state *shs)
{
   uint32_t views = shs->bound_image_views;

   while (views) {
      const int i = u_bit_scan(&views);
      struct pipe_resource *res = shs->image[i].res;

      // XXX: aux tracking
      iris_cache_flush_for_read(batch, iris_resource_bo(res));
   }
}


/**
 * \brief Resolve buffers before drawing.
 *
 * Resolve the depth buffer's HiZ buffer, resolve the depth buffer of each
 * enabled depth texture, and flush the render cache for any dirty textures.
 */
void
iris_predraw_resolve_inputs(struct iris_context *ice,
                            struct iris_batch *batch)
{
   for (gl_shader_stage stage = 0; stage < MESA_SHADER_STAGES; stage++) {
      struct iris_shader_state *shs = &ice->state.shaders[stage];
      resolve_sampler_views(batch, shs);
      resolve_image_views(batch, shs);
   }

   // XXX: storage images
}

void
iris_predraw_resolve_framebuffer(struct iris_context *ice,
                                 struct iris_batch *batch)
{
   struct pipe_framebuffer_state *cso_fb = &ice->state.framebuffer;
   struct pipe_surface *zs_surf = cso_fb->zsbuf;

   if (zs_surf) {
      // XXX: HiZ resolves
   }

   for (unsigned i = 0; i < cso_fb->nr_cbufs; i++) {
      struct iris_surface *surf = (void *) cso_fb->cbufs[i];
      if (!surf)
         continue;

      struct iris_resource *res = (void *) surf->base.texture;

      // XXX: aux tracking

      iris_cache_flush_for_render(batch, res->bo, surf->view.format,
                                  ISL_AUX_USAGE_NONE);
   }
}

/**
 * \brief Call this after drawing to mark which buffers need resolving
 *
 * If the depth buffer was written to and if it has an accompanying HiZ
 * buffer, then mark that it needs a depth resolve.
 *
 * If the color buffer is a multisample window system buffer, then
 * mark that it needs a downsample.
 *
 * Also mark any render targets which will be textured as needing a render
 * cache flush.
 */
void
iris_postdraw_update_resolve_tracking(struct iris_context *ice,
                                      struct iris_batch *batch)
{
   struct pipe_framebuffer_state *cso_fb = &ice->state.framebuffer;
   struct pipe_surface *zs_surf = cso_fb->zsbuf;

   // XXX: front buffer drawing?

   if (zs_surf) {
      struct iris_resource *z_res, *s_res;
      iris_get_depth_stencil_resources(zs_surf->texture, &z_res, &s_res);

      if (z_res) {
         // XXX: aux tracking

         if (ice->state.depth_writes_enabled)
            iris_depth_cache_add_bo(batch, z_res->bo);
      }

      if (s_res) {
         // XXX: aux tracking

         if (ice->state.stencil_writes_enabled)
            iris_depth_cache_add_bo(batch, s_res->bo);
      }
   }

   for (unsigned i = 0; i < cso_fb->nr_cbufs; i++) {
      struct iris_surface *surf = (void *) cso_fb->cbufs[i];
      if (!surf)
         continue;

      struct iris_resource *res = (void *) surf->base.texture;

      // XXX: aux tracking
      iris_render_cache_add_bo(batch, res->bo, surf->view.format,
                               ISL_AUX_USAGE_NONE);
   }
}

/**
 * Clear the cache-tracking sets.
 */
void
iris_cache_sets_clear(struct iris_batch *batch)
{
   hash_table_foreach(batch->cache.render, render_entry)
      _mesa_hash_table_remove(batch->cache.render, render_entry);

   set_foreach(batch->cache.depth, depth_entry)
      _mesa_set_remove(batch->cache.depth, depth_entry);
}

/**
 * Emits an appropriate flush for a BO if it has been rendered to within the
 * same batchbuffer as a read that's about to be emitted.
 *
 * The GPU has separate, incoherent caches for the render cache and the
 * sampler cache, along with other caches.  Usually data in the different
 * caches don't interact (e.g. we don't render to our driver-generated
 * immediate constant data), but for render-to-texture in FBOs we definitely
 * do.  When a batchbuffer is flushed, the kernel will ensure that everything
 * necessary is flushed before another use of that BO, but for reuse from
 * different caches within a batchbuffer, it's all our responsibility.
 */
void
iris_flush_depth_and_render_caches(struct iris_batch *batch)
{
   iris_emit_pipe_control_flush(batch,
                                PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                                PIPE_CONTROL_RENDER_TARGET_FLUSH |
                                PIPE_CONTROL_CS_STALL);

   iris_emit_pipe_control_flush(batch,
                                PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
                                PIPE_CONTROL_CONST_CACHE_INVALIDATE);

   iris_cache_sets_clear(batch);
}

void
iris_cache_flush_for_read(struct iris_batch *batch,
                          struct iris_bo *bo)
{
   if (_mesa_hash_table_search_pre_hashed(batch->cache.render, bo->hash, bo) ||
       _mesa_set_search_pre_hashed(batch->cache.depth, bo->hash, bo))
      iris_flush_depth_and_render_caches(batch);
}

static void *
format_aux_tuple(enum isl_format format, enum isl_aux_usage aux_usage)
{
   return (void *)(uintptr_t)((uint32_t)format << 8 | aux_usage);
}

void
iris_cache_flush_for_render(struct iris_batch *batch,
                            struct iris_bo *bo,
                            enum isl_format format,
                            enum isl_aux_usage aux_usage)
{
   if (_mesa_set_search_pre_hashed(batch->cache.depth, bo->hash, bo))
      iris_flush_depth_and_render_caches(batch);

   /* Check to see if this bo has been used by a previous rendering operation
    * but with a different format or aux usage.  If it has, flush the render
    * cache so we ensure that it's only in there with one format or aux usage
    * at a time.
    *
    * Even though it's not obvious, this can easily happen in practice.
    * Suppose a client is blending on a surface with sRGB encode enabled on
    * gen9.  This implies that you get AUX_USAGE_CCS_D at best.  If the client
    * then disables sRGB decode and continues blending we will flip on
    * AUX_USAGE_CCS_E without doing any sort of resolve in-between (this is
    * perfectly valid since CCS_E is a subset of CCS_D).  However, this means
    * that we have fragments in-flight which are rendering with UNORM+CCS_E
    * and other fragments in-flight with SRGB+CCS_D on the same surface at the
    * same time and the pixel scoreboard and color blender are trying to sort
    * it all out.  This ends badly (i.e. GPU hangs).
    *
    * To date, we have never observed GPU hangs or even corruption to be
    * associated with switching the format, only the aux usage.  However,
    * there are comments in various docs which indicate that the render cache
    * isn't 100% resilient to format changes.  We may as well be conservative
    * and flush on format changes too.  We can always relax this later if we
    * find it to be a performance problem.
    */
   struct hash_entry *entry =
      _mesa_hash_table_search_pre_hashed(batch->cache.render, bo->hash, bo);
   if (entry && entry->data != format_aux_tuple(format, aux_usage))
      iris_flush_depth_and_render_caches(batch);
}

void
iris_render_cache_add_bo(struct iris_batch *batch,
                         struct iris_bo *bo,
                         enum isl_format format,
                         enum isl_aux_usage aux_usage)
{
#ifndef NDEBUG
   struct hash_entry *entry =
      _mesa_hash_table_search_pre_hashed(batch->cache.render, bo->hash, bo);
   if (entry) {
      /* Otherwise, someone didn't do a flush_for_render and that would be
       * very bad indeed.
       */
      assert(entry->data == format_aux_tuple(format, aux_usage));
   }
#endif

   _mesa_hash_table_insert_pre_hashed(batch->cache.render, bo->hash, bo,
                                      format_aux_tuple(format, aux_usage));
}

void
iris_cache_flush_for_depth(struct iris_batch *batch,
                           struct iris_bo *bo)
{
   if (_mesa_hash_table_search_pre_hashed(batch->cache.render, bo->hash, bo))
      iris_flush_depth_and_render_caches(batch);
}

void
iris_depth_cache_add_bo(struct iris_batch *batch, struct iris_bo *bo)
{
   _mesa_set_add_pre_hashed(batch->cache.depth, bo->hash, bo);
}

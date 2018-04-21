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

#include "iris_context.h"
#include "util/hash_table.h"
#include "util/set.h"

/**
 * Emit a PIPE_CONTROL with various flushing flags.
 *
 * The caller is responsible for deciding what flags are appropriate for the
 * given generation.
 */
void
iris_emit_pipe_control_flush(struct iris_batch *batch, uint32_t flags)
{
   if ((flags & PIPE_CONTROL_CACHE_FLUSH_BITS) &&
       (flags & PIPE_CONTROL_CACHE_INVALIDATE_BITS)) {
      /* A pipe control command with flush and invalidate bits set
       * simultaneously is an inherently racy operation on Gen6+ if the
       * contents of the flushed caches were intended to become visible from
       * any of the invalidated caches.  Split it in two PIPE_CONTROLs, the
       * first one should stall the pipeline to make sure that the flushed R/W
       * caches are coherent with memory once the specified R/O caches are
       * invalidated.  On pre-Gen6 hardware the (implicit) R/O cache
       * invalidation seems to happen at the bottom of the pipeline together
       * with any write cache flush, so this shouldn't be a concern.  In order
       * to ensure a full stall, we do an end-of-pipe sync.
       */
      iris_emit_end_of_pipe_sync(batch, flags & PIPE_CONTROL_CACHE_FLUSH_BITS);
      flags &= ~(PIPE_CONTROL_CACHE_FLUSH_BITS | PIPE_CONTROL_CS_STALL);
   }

   batch->vtbl->emit_raw_pipe_control(batch, flags, NULL, 0, 0);
}

/**
 * Emit a PIPE_CONTROL that writes to a buffer object.
 *
 * \p flags should contain one of the following items:
 *  - PIPE_CONTROL_WRITE_IMMEDIATE
 *  - PIPE_CONTROL_WRITE_TIMESTAMP
 *  - PIPE_CONTROL_WRITE_DEPTH_COUNT
 */
void
iris_emit_pipe_control_write(struct iris_batch *batch, uint32_t flags,
                             struct iris_bo *bo, uint32_t offset,
                             uint64_t imm)
{
   batch->vtbl->emit_raw_pipe_control(batch, flags, bo, offset, imm);
}

/*
 * From Sandybridge PRM, volume 2, "1.7.2 End-of-Pipe Synchronization":
 *
 *  Write synchronization is a special case of end-of-pipe
 *  synchronization that requires that the render cache and/or depth
 *  related caches are flushed to memory, where the data will become
 *  globally visible. This type of synchronization is required prior to
 *  SW (CPU) actually reading the result data from memory, or initiating
 *  an operation that will use as a read surface (such as a texture
 *  surface) a previous render target and/or depth/stencil buffer
 *
 * From Haswell PRM, volume 2, part 1, "End-of-Pipe Synchronization":
 *
 *  Exercising the write cache flush bits (Render Target Cache Flush
 *  Enable, Depth Cache Flush Enable, DC Flush) in PIPE_CONTROL only
 *  ensures the write caches are flushed and doesn't guarantee the data
 *  is globally visible.
 *
 *  SW can track the completion of the end-of-pipe-synchronization by
 *  using "Notify Enable" and "PostSync Operation - Write Immediate
 *  Data" in the PIPE_CONTROL command.
 */
void
iris_emit_end_of_pipe_sync(struct iris_batch *batch, uint32_t flags)
{
   /* From Sandybridge PRM, volume 2, "1.7.3.1 Writing a Value to Memory":
    *
    *    "The most common action to perform upon reaching a synchronization
    *    point is to write a value out to memory. An immediate value
    *    (included with the synchronization command) may be written."
    *
    * From Broadwell PRM, volume 7, "End-of-Pipe Synchronization":
    *
    *    "In case the data flushed out by the render engine is to be read
    *    back in to the render engine in coherent manner, then the render
    *    engine has to wait for the fence completion before accessing the
    *    flushed data. This can be achieved by following means on various
    *    products: PIPE_CONTROL command with CS Stall and the required
    *    write caches flushed with Post-Sync-Operation as Write Immediate
    *    Data.
    *
    *    Example:
    *       - Workload-1 (3D/GPGPU/MEDIA)
    *       - PIPE_CONTROL (CS Stall, Post-Sync-Operation Write Immediate
    *         Data, Required Write Cache Flush bits set)
    *       - Workload-2 (Can use the data produce or output by Workload-1)
    */
   iris_emit_pipe_control_write(batch, flags | PIPE_CONTROL_CS_STALL |
                                PIPE_CONTROL_WRITE_IMMEDIATE,
                                batch->screen->workaround_bo, 0, 0);
}

void
iris_cache_sets_clear(struct iris_batch *batch)
{
   struct hash_entry *render_entry;
   hash_table_foreach(batch->cache.render, render_entry)
      _mesa_hash_table_remove(batch->cache.render, render_entry);

   struct set_entry *depth_entry;
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
static void
flush_depth_and_render_caches(struct iris_batch *batch, struct iris_bo *bo)
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
   if (_mesa_hash_table_search(batch->cache.render, bo) ||
       _mesa_set_search(batch->cache.depth, bo))
      flush_depth_and_render_caches(batch, bo);
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
   if (_mesa_set_search(batch->cache.depth, bo))
      flush_depth_and_render_caches(batch, bo);

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
   struct hash_entry *entry = _mesa_hash_table_search(batch->cache.render, bo);
   if (entry && entry->data != format_aux_tuple(format, aux_usage))
      flush_depth_and_render_caches(batch, bo);
}

void
iris_render_cache_add_bo(struct iris_batch *batch,
                         struct iris_bo *bo,
                         enum isl_format format,
                         enum isl_aux_usage aux_usage)
{
#ifndef NDEBUG
   struct hash_entry *entry = _mesa_hash_table_search(batch->cache.render, bo);
   if (entry) {
      /* Otherwise, someone didn't do a flush_for_render and that would be
       * very bad indeed.
       */
      assert(entry->data == format_aux_tuple(format, aux_usage));
   }
#endif

   _mesa_hash_table_insert(batch->cache.render, bo,
                           format_aux_tuple(format, aux_usage));
}

void
iris_cache_flush_for_depth(struct iris_batch *batch,
                           struct iris_bo *bo)
{
   if (_mesa_hash_table_search(batch->cache.render, bo))
      flush_depth_and_render_caches(batch, bo);
}

void
iris_depth_cache_add_bo(struct iris_batch *batch, struct iris_bo *bo)
{
   _mesa_set_add(batch->cache.depth, bo);
}

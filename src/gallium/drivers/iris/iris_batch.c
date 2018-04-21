/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "iris_batch.h"
#include "iris_bufmgr.h"
#include "iris_context.h"

#include "drm-uapi/i915_drm.h"

#include "util/hash_table.h"
#include "util/set.h"
#include "main/macros.h"

#include <errno.h>
#include <xf86drm.h>

#define FILE_DEBUG_FLAG DEBUG_BUFMGR

/**
 * Target sizes of the batch and state buffers.  We create the initial
 * buffers at these sizes, and flush when they're nearly full.  If we
 * underestimate how close we are to the end, and suddenly need more space
 * in the middle of a draw, we can grow the buffers, and finish the draw.
 * At that point, we'll be over our target size, so the next operation
 * should flush.  Each time we flush the batch, we recreate both buffers
 * at the original target size, so it doesn't grow without bound.
 */
#define BATCH_SZ (20 * 1024)
#define STATE_SZ (18 * 1024)

static void decode_batch(struct iris_batch *batch);

static void
iris_batch_reset(struct iris_batch *batch);

UNUSED static void
dump_validation_list(struct iris_batch *batch)
{
   fprintf(stderr, "Validation list (length %d):\n", batch->exec_count);

   for (int i = 0; i < batch->exec_count; i++) {
      uint64_t flags = batch->validation_list[i].flags;
      assert(batch->validation_list[i].handle ==
             batch->exec_bos[i]->gem_handle);
      fprintf(stderr, "[%2d]: %2d %-14s %p %s%-7s @ 0x%016llx (%"PRIu64"B)\n",
              i,
              batch->validation_list[i].handle,
              batch->exec_bos[i]->name,
              batch->exec_bos[i],
              (flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS) ? "(48b" : "(32b",
              (flags & EXEC_OBJECT_WRITE) ? " write)" : ")",
              batch->validation_list[i].offset,
              batch->exec_bos[i]->size);
   }
}

static struct gen_batch_decode_bo
decode_get_bo(void *v_batch, uint64_t address)
{
   struct iris_batch *batch = v_batch;

   for (int i = 0; i < batch->exec_count; i++) {
      struct iris_bo *bo = batch->exec_bos[i];
      /* The decoder zeroes out the top 16 bits, so we need to as well */
      uint64_t bo_address = bo->gtt_offset & (~0ull >> 16);

      if (address >= bo_address && address < bo_address + bo->size) {
         return (struct gen_batch_decode_bo) {
            .addr = address,
            .size = bo->size,
            .map = iris_bo_map(batch->dbg, bo, MAP_READ) +
                   (address - bo_address),
         };
      }
   }

   return (struct gen_batch_decode_bo) { };
}

static bool
uint_key_compare(const void *a, const void *b)
{
   return a == b;
}

static uint32_t
uint_key_hash(const void *key)
{
   return (uintptr_t) key;
}

static void
create_batch_buffer(struct iris_bufmgr *bufmgr,
                    struct iris_batch_buffer *buf,
                    const char *name, unsigned size)
{
   buf->bo = iris_bo_alloc(bufmgr, name, size, IRIS_MEMZONE_OTHER);
   buf->bo->kflags |= EXEC_OBJECT_CAPTURE;
   buf->map = iris_bo_map(NULL, buf->bo, MAP_READ | MAP_WRITE);
   buf->map_next = buf->map;
}

void
iris_init_batch(struct iris_batch *batch,
                struct iris_screen *screen,
                struct iris_vtable *vtbl,
                struct pipe_debug_callback *dbg,
                uint8_t ring)
{
   batch->screen = screen;
   batch->vtbl = vtbl;
   batch->dbg = dbg;

   /* ring should be one of I915_EXEC_RENDER, I915_EXEC_BLT, etc. */
   assert((ring & ~I915_EXEC_RING_MASK) == 0);
   assert(util_bitcount(ring) == 1);
   batch->ring = ring;

   batch->exec_count = 0;
   batch->exec_array_size = 100;
   batch->exec_bos =
      malloc(batch->exec_array_size * sizeof(batch->exec_bos[0]));
   batch->validation_list =
      malloc(batch->exec_array_size * sizeof(batch->validation_list[0]));

   batch->cache.render = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                                 _mesa_key_pointer_equal);
   batch->cache.depth = _mesa_set_create(NULL, _mesa_hash_pointer,
                                         _mesa_key_pointer_equal);
   if (unlikely(INTEL_DEBUG)) {
      batch->state_sizes =
         _mesa_hash_table_create(NULL, uint_key_hash, uint_key_compare);

      const unsigned decode_flags =
         GEN_BATCH_DECODE_FULL |
         ((INTEL_DEBUG & DEBUG_COLOR) ? GEN_BATCH_DECODE_IN_COLOR : 0) |
         GEN_BATCH_DECODE_OFFSETS |
         GEN_BATCH_DECODE_FLOATS;

      gen_batch_decode_ctx_init(&batch->decoder, &screen->devinfo,
                                stderr, decode_flags, NULL,
                                decode_get_bo, NULL, batch);
   }

   iris_batch_reset(batch);
}

#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))

static unsigned
add_exec_bo(struct iris_batch *batch, struct iris_bo *bo)
{
   unsigned index = READ_ONCE(bo->index);

   if (index < batch->exec_count && batch->exec_bos[index] == bo)
      return index;

   /* May have been shared between multiple active batches */
   for (index = 0; index < batch->exec_count; index++) {
      if (batch->exec_bos[index] == bo)
         return index;
   }

   iris_bo_reference(bo);

   if (batch->exec_count == batch->exec_array_size) {
      batch->exec_array_size *= 2;
      batch->exec_bos =
         realloc(batch->exec_bos,
                 batch->exec_array_size * sizeof(batch->exec_bos[0]));
      batch->validation_list =
         realloc(batch->validation_list,
                 batch->exec_array_size * sizeof(batch->validation_list[0]));
   }

   batch->validation_list[batch->exec_count] =
      (struct drm_i915_gem_exec_object2) {
         .handle = bo->gem_handle,
         .offset = bo->gtt_offset,
         .flags = bo->kflags,
      };

   bo->index = batch->exec_count;
   batch->exec_bos[batch->exec_count] = bo;
   batch->aperture_space += bo->size;

   return batch->exec_count++;
}

static void
iris_batch_reset(struct iris_batch *batch)
{
   struct iris_screen *screen = batch->screen;
   struct iris_bufmgr *bufmgr = screen->bufmgr;

   if (batch->last_cmd_bo != NULL) {
      iris_bo_unreference(batch->last_cmd_bo);
      batch->last_cmd_bo = NULL;
   }
   batch->last_cmd_bo = batch->cmdbuf.bo;

   create_batch_buffer(bufmgr, &batch->cmdbuf, "command buffer", BATCH_SZ);

   add_exec_bo(batch, batch->cmdbuf.bo);
   assert(batch->cmdbuf.bo->index == 0);

   if (batch->state_sizes)
      _mesa_hash_table_clear(batch->state_sizes, NULL);
}

static void
iris_batch_reset_and_clear_caches(struct iris_batch *batch)
{
   iris_batch_reset(batch);
   iris_cache_sets_clear(batch);
}

static void
free_batch_buffer(struct iris_batch_buffer *buf)
{
   iris_bo_unreference(buf->bo);
   buf->bo = NULL;
   buf->map = NULL;
   buf->map_next = NULL;
}

void
iris_batch_free(struct iris_batch *batch)
{
   for (int i = 0; i < batch->exec_count; i++) {
      iris_bo_unreference(batch->exec_bos[i]);
   }
   free(batch->exec_bos);
   free(batch->validation_list);
   free_batch_buffer(&batch->cmdbuf);

   iris_bo_unreference(batch->last_cmd_bo);

   _mesa_hash_table_destroy(batch->cache.render, NULL);
   _mesa_set_destroy(batch->cache.depth, NULL);

   if (batch->state_sizes) {
      _mesa_hash_table_destroy(batch->state_sizes, NULL);
      gen_batch_decode_ctx_finish(&batch->decoder);
   }
}

/**
 * Finish copying the old batch/state buffer's contents to the new one
 * after we tried to "grow" the buffer in an earlier operation.
 */
static void
finish_growing_bos(struct iris_batch_buffer *buf)
{
   struct iris_bo *old_bo = buf->partial_bo;
   if (!old_bo)
      return;

   void *old_map = old_bo->map_cpu ? old_bo->map_cpu : old_bo->map_wc;
   memcpy(buf->map, old_map, buf->partial_bytes);

   buf->partial_bo = NULL;
   buf->partial_bytes = 0;

   iris_bo_unreference(old_bo);
}

static unsigned
buffer_bytes_used(struct iris_batch_buffer *buf)
{
   return buf->map_next - buf->map;
}

/**
 * Grow either the batch or state buffer to a new larger size.
 *
 * We can't actually grow buffers, so we allocate a new one, copy over
 * the existing contents, and update our lists to refer to the new one.
 *
 * Note that this is only temporary - each new batch recreates the buffers
 * at their original target size (BATCH_SZ or STATE_SZ).
 */
static void
grow_buffer(struct iris_batch *batch,
            struct iris_batch_buffer *buf,
            unsigned new_size)
{
   struct iris_bufmgr *bufmgr = batch->screen->bufmgr;
   struct iris_bo *bo = buf->bo;

   perf_debug(batch->dbg, "Growing %s - ran out of space\n", bo->name);

   if (buf->partial_bo) {
      /* We've already grown once, and now we need to do it again.
       * Finish our last grow operation so we can start a new one.
       * This should basically never happen.
       */
      perf_debug(batch->dbg, "Had to grow multiple times");
      finish_growing_bos(buf);
   }

   const unsigned existing_bytes = buffer_bytes_used(buf);

   struct iris_bo *new_bo =
      iris_bo_alloc(bufmgr, bo->name, new_size, IRIS_MEMZONE_OTHER);

   buf->map = iris_bo_map(NULL, new_bo, MAP_READ | MAP_WRITE);
   buf->map_next = buf->map + existing_bytes;

   /* Try to put the new BO at the same GTT offset as the old BO (which
    * we're throwing away, so it doesn't need to be there).
    *
    * This guarantees that our relocations continue to work: values we've
    * already written into the buffer, values we're going to write into the
    * buffer, and the validation/relocation lists all will match.
    *
    * Also preserve kflags for EXEC_OBJECT_CAPTURE.
    */
   new_bo->gtt_offset = bo->gtt_offset;
   new_bo->index = bo->index;
   new_bo->kflags = bo->kflags;

   /* Batch/state buffers are per-context, and if we've run out of space,
    * we must have actually used them before, so...they will be in the list.
    */
   assert(bo->index < batch->exec_count);
   assert(batch->exec_bos[bo->index] == bo);

   /* Update the validation list to use the new BO. */
   batch->exec_bos[bo->index] = new_bo;
   batch->validation_list[bo->index].handle = new_bo->gem_handle;

   /* Exchange the two BOs...without breaking pointers to the old BO.
    *
    * Consider this scenario:
    *
    * 1. Somebody calls iris_state_batch() to get a region of memory, and
    *    and then creates a iris_address pointing to iris->batch.state.bo.
    * 2. They then call iris_state_batch() a second time, which happens to
    *    grow and replace the state buffer.  They then try to emit a
    *    relocation to their first section of memory.
    *
    * If we replace the iris->batch.state.bo pointer at step 2, we would
    * break the address created in step 1.  They'd have a pointer to the
    * old destroyed BO.  Emitting a relocation would add this dead BO to
    * the validation list...causing /both/ statebuffers to be in the list,
    * and all kinds of disasters.
    *
    * This is not a contrived case - BLORP vertex data upload hits this.
    *
    * There are worse scenarios too.  Fences for GL sync objects reference
    * iris->batch.batch.bo.  If we replaced the batch pointer when growing,
    * we'd need to chase down every fence and update it to point to the
    * new BO.  Otherwise, it would refer to a "batch" that never actually
    * gets submitted, and would fail to trigger.
    *
    * To work around both of these issues, we transmutate the buffers in
    * place, making the existing struct iris_bo represent the new buffer,
    * and "new_bo" represent the old BO.  This is highly unusual, but it
    * seems like a necessary evil.
    *
    * We also defer the memcpy of the existing batch's contents.  Callers
    * may make multiple iris_state_batch calls, and retain pointers to the
    * old BO's map.  We'll perform the memcpy in finish_growing_bo() when
    * we finally submit the batch, at which point we've finished uploading
    * state, and nobody should have any old references anymore.
    *
    * To do that, we keep a reference to the old BO in grow->partial_bo,
    * and store the number of bytes to copy in grow->partial_bytes.  We
    * can monkey with the refcounts directly without atomics because these
    * are per-context BOs and they can only be touched by this thread.
    */
   assert(new_bo->refcount == 1);
   new_bo->refcount = bo->refcount;
   bo->refcount = 1;

   struct iris_bo tmp;
   memcpy(&tmp, bo, sizeof(struct iris_bo));
   memcpy(bo, new_bo, sizeof(struct iris_bo));
   memcpy(new_bo, &tmp, sizeof(struct iris_bo));

   buf->partial_bo = new_bo; /* the one reference of the OLD bo */
   buf->partial_bytes = existing_bytes;
}

static void
require_buffer_space(struct iris_batch *batch,
                     struct iris_batch_buffer *buf,
                     unsigned size,
                     unsigned flush_threshold,
                     unsigned max_buffer_size)
{
   const unsigned required_bytes = buffer_bytes_used(buf) + size;

   if (!batch->no_wrap && required_bytes >= flush_threshold) {
      iris_batch_flush(batch);
   } else if (required_bytes >= buf->bo->size) {
      grow_buffer(batch, buf,
                  MIN2(buf->bo->size + buf->bo->size / 2, max_buffer_size));
      assert(required_bytes < buf->bo->size);
   }
}


void
iris_require_command_space(struct iris_batch *batch, unsigned size)
{
   require_buffer_space(batch, &batch->cmdbuf, size, BATCH_SZ, MAX_BATCH_SIZE);
}

void *
iris_get_command_space(struct iris_batch *batch, unsigned bytes)
{
   iris_require_command_space(batch, bytes);
   void *map = batch->cmdbuf.map_next;
   batch->cmdbuf.map_next += bytes;
   return map;
}

void
iris_batch_emit(struct iris_batch *batch, const void *data, unsigned size)
{
   void *map = iris_get_command_space(batch, size);
   memcpy(map, data, size);
}

/**
 * Called from iris_batch_flush before emitting MI_BATCHBUFFER_END and
 * sending it off.
 *
 * This function can emit state (say, to preserve registers that aren't saved
 * between batches).
 */
static void
iris_finish_batch(struct iris_batch *batch)
{
   batch->no_wrap = true;

   // XXX: ISP DIS

   /* Emit MI_BATCH_BUFFER_END to finish our batch.  Note that execbuf2
    * requires our batch size to be QWord aligned, so we pad it out if
    * necessary by emitting an extra MI_NOOP after the end.
    */
   const uint32_t MI_BATCH_BUFFER_END_AND_NOOP[2]  = { (0xA << 23), 0 };
   const bool qword_aligned = (buffer_bytes_used(&batch->cmdbuf) % 8) == 0;
   iris_batch_emit(batch, MI_BATCH_BUFFER_END_AND_NOOP, qword_aligned ? 8 : 4);

   batch->no_wrap = false;
}

static int
submit_batch(struct iris_batch *batch, int in_fence_fd, int *out_fence_fd)
{
   iris_bo_unmap(batch->cmdbuf.bo);

   /* The requirement for using I915_EXEC_NO_RELOC are:
    *
    *   The addresses written in the objects must match the corresponding
    *   reloc.gtt_offset which in turn must match the corresponding
    *   execobject.offset.
    *
    *   Any render targets written to in the batch must be flagged with
    *   EXEC_OBJECT_WRITE.
    *
    *   To avoid stalling, execobject.offset should match the current
    *   address of that object within the active context.
    */
   struct drm_i915_gem_execbuffer2 execbuf = {
      .buffers_ptr = (uintptr_t) batch->validation_list,
      .buffer_count = batch->exec_count,
      .batch_start_offset = 0,
      .batch_len = buffer_bytes_used(&batch->cmdbuf),
      .flags = batch->ring |
               I915_EXEC_NO_RELOC |
               I915_EXEC_BATCH_FIRST |
               I915_EXEC_HANDLE_LUT,
      .rsvd1 = batch->hw_ctx_id, /* rsvd1 is actually the context ID */
   };

   unsigned long cmd = DRM_IOCTL_I915_GEM_EXECBUFFER2;

   if (in_fence_fd != -1) {
      execbuf.rsvd2 = in_fence_fd;
      execbuf.flags |= I915_EXEC_FENCE_IN;
   }

   if (out_fence_fd != NULL) {
      cmd = DRM_IOCTL_I915_GEM_EXECBUFFER2_WR;
      *out_fence_fd = -1;
      execbuf.flags |= I915_EXEC_FENCE_OUT;
   }

   int ret = drm_ioctl(batch->screen->fd, cmd, &execbuf);
   if (ret != 0) {
      ret = -errno;
      DBG("execbuf FAILED: errno = %d\n", -ret);
   } else {
      DBG("execbuf succeeded\n");
   }

   for (int i = 0; i < batch->exec_count; i++) {
      struct iris_bo *bo = batch->exec_bos[i];

      bo->idle = false;
      bo->index = -1;
   }

   if (ret == 0 && out_fence_fd != NULL)
      *out_fence_fd = execbuf.rsvd2 >> 32;

   return ret;
}

/**
 * The in_fence_fd is ignored if -1.  Otherwise this function takes ownership
 * of the fd.
 *
 * The out_fence_fd is ignored if NULL. Otherwise, the caller takes ownership
 * of the returned fd.
 */
int
_iris_batch_flush_fence(struct iris_batch *batch,
                        int in_fence_fd, int *out_fence_fd,
                        const char *file, int line)
{
   if (buffer_bytes_used(&batch->cmdbuf) == 0)
      return 0;

   /* Check that we didn't just wrap our batchbuffer at a bad time. */
   assert(!batch->no_wrap);

   iris_finish_batch(batch);

   if (unlikely(INTEL_DEBUG & (DEBUG_BATCH | DEBUG_SUBMIT))) {
      int bytes_for_commands = buffer_bytes_used(&batch->cmdbuf);
      fprintf(stderr, "%19s:%-3d: Batchbuffer flush with %5db (%0.1f%%), "
              "%4d BOs (%0.1fMb aperture)\n",
              file, line,
              bytes_for_commands, 100.0f * bytes_for_commands / BATCH_SZ,
              batch->exec_count,
              (float) batch->aperture_space / (1024 * 1024));
      dump_validation_list(batch);
   }

   if (unlikely(INTEL_DEBUG & DEBUG_BATCH))
      decode_batch(batch);

   int ret = submit_batch(batch, in_fence_fd, out_fence_fd);

   //throttle(iris);

   if (ret < 0)
      return ret;

   //if (iris->ctx.Const.ResetStrategy == GL_LOSE_CONTEXT_ON_RESET_ARB)
      //iris_check_for_reset(ice);

   if (unlikely(INTEL_DEBUG & DEBUG_SYNC)) {
      dbg_printf("waiting for idle\n");
      iris_bo_wait_rendering(batch->cmdbuf.bo);
   }

   /* Clean up after the batch we submitted and prepare for a new one. */
   for (int i = 0; i < batch->exec_count; i++) {
      iris_bo_unreference(batch->exec_bos[i]);
      batch->exec_bos[i] = NULL;
   }
   batch->exec_count = 0;
   batch->aperture_space = 0;

   /* Start a new batch buffer. */
   iris_batch_reset_and_clear_caches(batch);

   return 0;
}

bool
iris_batch_references(struct iris_batch *batch, struct iris_bo *bo)
{
   unsigned index = READ_ONCE(bo->index);
   if (index < batch->exec_count && batch->exec_bos[index] == bo)
      return true;

   for (int i = 0; i < batch->exec_count; i++) {
      if (batch->exec_bos[i] == bo)
         return true;
   }
   return false;
}

/* This is the only way buffers get added to the validate list.
 */
void
iris_use_pinned_bo(struct iris_batch *batch,
                   struct iris_bo *bo,
                   bool writable)
{
   assert(bo->kflags & EXEC_OBJECT_PINNED);
   unsigned index = add_exec_bo(batch, bo);
   if (writable)
      batch->validation_list[index].flags |= EXEC_OBJECT_WRITE;
}

static void
decode_batch(struct iris_batch *batch)
{
   gen_print_batch(&batch->decoder, batch->cmdbuf.map,
                   buffer_bytes_used(&batch->cmdbuf),
                   batch->cmdbuf.bo->gtt_offset);
}

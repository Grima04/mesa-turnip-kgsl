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
#include "common/gen_decoder.h"

#include "drm-uapi/i915_drm.h"

#include "util/hash_table.h"
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
      assert(batch->validation_list[i].handle ==
             batch->exec_bos[i]->gem_handle);
      fprintf(stderr, "[%d] = %d %s %p\n", i,
              batch->validation_list[i].handle,
              batch->exec_bos[i]->name,
              batch->exec_bos[i]);
   }
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
init_reloc_list(struct iris_reloc_list *rlist, int count)
{
   rlist->reloc_count = 0;
   rlist->reloc_array_size = count;
   rlist->relocs = malloc(rlist->reloc_array_size *
                          sizeof(struct drm_i915_gem_relocation_entry));
}

static void
create_batch_buffer(struct iris_bufmgr *bufmgr,
                    struct iris_batch_buffer *buf,
                    const char *name, unsigned size)
{
   buf->bo = iris_bo_alloc(bufmgr, name, size, 4096);
   buf->bo->kflags |= EXEC_OBJECT_CAPTURE;
   buf->map = iris_bo_map(NULL, buf->bo, MAP_READ | MAP_WRITE);
   buf->map_next = buf->map;
}

void
iris_init_batch(struct iris_batch *batch,
                struct iris_screen *screen,
                struct pipe_debug_callback *dbg,
                uint8_t ring)
{
   batch->screen = screen;
   batch->dbg = dbg;

   /* ring should be one of I915_EXEC_RENDER, I915_EXEC_BLT, etc. */
   assert((ring & ~I915_EXEC_RING_MASK) == 0);
   assert(util_bitcount(ring) == 1);
   batch->ring = ring;

   init_reloc_list(&batch->cmdbuf.relocs, 256);
   init_reloc_list(&batch->statebuf.relocs, 256);

   batch->exec_count = 0;
   batch->exec_array_size = 100;
   batch->exec_bos =
      malloc(batch->exec_array_size * sizeof(batch->exec_bos[0]));
   batch->validation_list =
      malloc(batch->exec_array_size * sizeof(batch->validation_list[0]));

   if (unlikely(INTEL_DEBUG)) {
      batch->state_sizes =
         _mesa_hash_table_create(NULL, uint_key_hash, uint_key_compare);
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
         .alignment = bo->align,
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
   create_batch_buffer(bufmgr, &batch->statebuf, "state buffer", STATE_SZ);

   /* Avoid making 0 a valid state offset - otherwise the decoder will try
    * and decode data when we use offset 0 as a null pointer.
    */
   batch->statebuf.map_next += 1;

   add_exec_bo(batch, batch->cmdbuf.bo);
   assert(batch->cmdbuf.bo->index == 0);

   if (batch->state_sizes)
      _mesa_hash_table_clear(batch->state_sizes, NULL);

   if (batch->ring == I915_EXEC_RENDER)
      batch->emit_state_base_address(batch);
}

static void
iris_batch_reset_and_clear_render_cache(struct iris_batch *batch)
{
   iris_batch_reset(batch);
   // XXX: iris_render_cache_set_clear(batch);
}

static void
free_batch_buffer(struct iris_batch_buffer *buf)
{
   iris_bo_unreference(buf->bo);
   buf->bo = NULL;
   buf->map = NULL;
   buf->map_next = NULL;

   free(buf->relocs.relocs);
   buf->relocs.relocs = NULL;
   buf->relocs.reloc_array_size = 0;
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
   free_batch_buffer(&batch->statebuf);

   iris_bo_unreference(batch->last_cmd_bo);

   if (batch->state_sizes)
      _mesa_hash_table_destroy(batch->state_sizes, NULL);
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
      iris_bo_alloc(bufmgr, bo->name, new_size, bo->align);

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

/**
 * Reserve some space in the statebuffer, or flush.
 *
 * This is used to estimate when we're near the end of the batch,
 * so we can flush early.
 */
void
iris_require_state_space(struct iris_batch *batch, unsigned size)
{
   require_buffer_space(batch, &batch->statebuf, size, STATE_SZ,
                        MAX_STATE_SIZE);
}

void
iris_batch_emit(struct iris_batch *batch, const void *data, unsigned size)
{
   iris_require_command_space(batch, size);
   memcpy(batch->cmdbuf.map_next, data, size);
   batch->cmdbuf.map_next += size;
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

   /* Mark the end of the buffer. */
   const uint32_t MI_BATCH_BUFFER_END = (0xA << 23);
   iris_batch_emit(batch, &MI_BATCH_BUFFER_END, sizeof(uint32_t));

   batch->no_wrap = false;
}

static int
submit_batch(struct iris_batch *batch, int in_fence_fd, int *out_fence_fd)
{
   iris_bo_unmap(batch->cmdbuf.bo);
   iris_bo_unmap(batch->statebuf.bo);

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
   /* Set statebuffer relocations */
   const unsigned state_index = batch->statebuf.bo->index;
   if (state_index < batch->exec_count &&
       batch->exec_bos[state_index] == batch->statebuf.bo) {
      struct drm_i915_gem_exec_object2 *entry =
         &batch->validation_list[state_index];
      assert(entry->handle == batch->statebuf.bo->gem_handle);
      entry->relocation_count = batch->statebuf.relocs.reloc_count;
      entry->relocs_ptr = (uintptr_t) batch->statebuf.relocs.relocs;
   }

   /* Set batchbuffer relocations */
   struct drm_i915_gem_exec_object2 *entry = &batch->validation_list[0];
   assert(entry->handle == batch->cmdbuf.bo->gem_handle);
   entry->relocation_count = batch->cmdbuf.relocs.reloc_count;
   entry->relocs_ptr = (uintptr_t) batch->cmdbuf.relocs.relocs;

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

#if 0
   int ret = drm_ioctl(batch->screen->fd, cmd, &execbuf);
   if (ret != 0)
      ret = -errno;
#else
   int ret = 0;
   fprintf(stderr, "execbuf disabled for now\n");
#endif

   for (int i = 0; i < batch->exec_count; i++) {
      struct iris_bo *bo = batch->exec_bos[i];

      bo->idle = false;
      bo->index = -1;

      /* Update iris_bo::gtt_offset */
      if (batch->validation_list[i].offset != bo->gtt_offset) {
         DBG("BO %d migrated: 0x%" PRIx64 " -> 0x%llx\n",
             bo->gem_handle, bo->gtt_offset,
             batch->validation_list[i].offset);
         bo->gtt_offset = batch->validation_list[i].offset;
      }
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
      int bytes_for_state = buffer_bytes_used(&batch->statebuf);
      fprintf(stderr, "%19s:%-3d: Batchbuffer flush with %5db (%0.1f%%) (pkt),"
              " %5db (%0.1f%%) (state), %4d BOs (%0.1fMb aperture),"
              " %4d batch relocs, %4d state relocs\n", file, line,
              bytes_for_commands, 100.0f * bytes_for_commands / BATCH_SZ,
              bytes_for_state, 100.0f * bytes_for_state / STATE_SZ,
              batch->exec_count,
              (float) batch->aperture_space / (1024 * 1024),
              batch->cmdbuf.relocs.reloc_count,
              batch->statebuf.relocs.reloc_count);
   }

   int ret = submit_batch(batch, in_fence_fd, out_fence_fd);
   if (ret < 0)
      return ret;

   //throttle(iris);

   if (unlikely(INTEL_DEBUG & DEBUG_BATCH))
      decode_batch(batch);

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
   batch->cmdbuf.relocs.reloc_count = 0;
   batch->statebuf.relocs.reloc_count = 0;
   batch->exec_count = 0;
   batch->aperture_space = 0;

   iris_bo_unreference(batch->statebuf.bo);

   /* Start a new batch buffer. */
   iris_batch_reset_and_clear_render_cache(batch);

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

/*  This is the only way buffers get added to the validate list.
 */
static uint64_t
emit_reloc(struct iris_batch *batch,
           struct iris_reloc_list *rlist, uint32_t offset,
           struct iris_bo *target, uint32_t target_offset,
           unsigned int reloc_flags)
{
   assert(target != NULL);

   unsigned int index = add_exec_bo(batch, target);
   struct drm_i915_gem_exec_object2 *entry = &batch->validation_list[index];

   if (target->kflags & EXEC_OBJECT_PINNED) {
      assert(entry->offset == target->gtt_offset);
      return entry->offset + target_offset;
   }

   if (rlist->reloc_count == rlist->reloc_array_size) {
      rlist->reloc_array_size *= 2;
      rlist->relocs = realloc(rlist->relocs,
                              rlist->reloc_array_size *
                              sizeof(struct drm_i915_gem_relocation_entry));
   }

   rlist->relocs[rlist->reloc_count++] =
      (struct drm_i915_gem_relocation_entry) {
         .offset = offset,
         .delta = target_offset,
         .target_handle = index,
         .presumed_offset = entry->offset,
      };

   /* Using the old buffer offset, write in what the right data would be, in
    * case the buffer doesn't move and we can short-circuit the relocation
    * processing in the kernel
    */
   return entry->offset + target_offset;
}

uint64_t
iris_batch_reloc(struct iris_batch *batch, uint32_t batch_offset,
                 struct iris_bo *target, uint32_t target_offset,
                 unsigned int reloc_flags)
{
   assert(batch_offset <= batch->cmdbuf.bo->size - sizeof(uint32_t));

   return emit_reloc(batch, &batch->cmdbuf.relocs, batch_offset,
                     target, target_offset, reloc_flags);
}

uint64_t
iris_state_reloc(struct iris_batch *batch, uint32_t state_offset,
                 struct iris_bo *target, uint32_t target_offset,
                 unsigned int reloc_flags)
{
   assert(state_offset <= batch->statebuf.bo->size - sizeof(uint32_t));

   return emit_reloc(batch, &batch->statebuf.relocs, state_offset,
                     target, target_offset, reloc_flags);
}


static uint32_t
iris_state_entry_size(struct iris_batch *batch, uint32_t offset)
{
   struct hash_entry *entry =
      _mesa_hash_table_search(batch->state_sizes, (void *)(uintptr_t) offset);
   return entry ? (uintptr_t) entry->data : 0;
}

/**
 * Allocates a block of space in the batchbuffer for indirect state.
 */
void *
iris_alloc_state(struct iris_batch *batch,
                 int size, int alignment,
                 uint32_t *out_offset)
{
   assert(size < batch->statebuf.bo->size);

   const unsigned existing_bytes = buffer_bytes_used(&batch->statebuf);
   unsigned aligned_size =
      ALIGN(existing_bytes, alignment) - existing_bytes + size;

   require_buffer_space(batch, &batch->statebuf, aligned_size,
                        STATE_SZ, MAX_STATE_SIZE);

   unsigned offset = ALIGN(buffer_bytes_used(&batch->statebuf), alignment);

   if (unlikely(batch->state_sizes)) {
      _mesa_hash_table_insert(batch->state_sizes,
                              (void *) (uintptr_t) offset,
                              (void *) (uintptr_t) size);
   }

   batch->statebuf.map_next += aligned_size;

   *out_offset = offset;
   return batch->statebuf.map_next;
}

uint32_t
iris_emit_state(struct iris_batch *batch,
                const void *data,
                int size, int alignment)
{
   uint32_t out_offset;
   void *dest = iris_alloc_state(batch, size, alignment, &out_offset);
   memcpy(dest, data, size);
   return out_offset;
}

static void
decode_batch(struct iris_batch *batch)
{
   // XXX: decode the batch
}

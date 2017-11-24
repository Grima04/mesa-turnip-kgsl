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
#define STATE_SZ (16 * 1024)

/* The kernel assumes batchbuffers are smaller than 256kB. */
#define MAX_BATCH_SIZE (256 * 1024)

/* 3DSTATE_BINDING_TABLE_POINTERS has a U16 offset from Surface State Base
 * Address, which means that we can't put binding tables beyond 64kB.  This
 * effectively limits the maximum statebuffer size to 64kB.
 */
#define MAX_STATE_SIZE (64 * 1024)

static unsigned
iris_batch_used(struct iris_batch *batch)
{
   return batch->cmd_map_next - batch->cmd_map;
}

static unsigned
iris_state_used(struct iris_batch *batch)
{
   return batch->state_map_next - batch->state_map;
}

static void
iris_batch_reset(struct iris_batch *batch);

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

void
iris_batch_init(struct iris_batch *batch,
                struct iris_screen *screen,
                struct pipe_debug_callback *dbg)
{
   batch->screen = screen;
   batch->dbg = dbg;

   init_reloc_list(&batch->batch_relocs, 256);
   init_reloc_list(&batch->state_relocs, 256);

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
   batch->last_cmd_bo = batch->cmd_bo;

   batch->cmd_bo = iris_bo_alloc(bufmgr, "batchbuffer", BATCH_SZ, 4096);
   batch->cmd_map = iris_bo_map(NULL, batch->cmd_bo, MAP_READ | MAP_WRITE);
   batch->cmd_map_next = batch->cmd_map;

   batch->state_bo = iris_bo_alloc(bufmgr, "statebuffer", STATE_SZ, 4096);
   batch->state_bo->kflags = EXEC_OBJECT_CAPTURE;
   batch->state_map =
      iris_bo_map(NULL, batch->state_bo, MAP_READ | MAP_WRITE);

   /* Avoid making 0 a valid state offset - otherwise the decoder will try
    * and decode data when we use offset 0 as a null pointer.
    */
   batch->state_map_next = batch->state_map + 1;

   add_exec_bo(batch, batch->cmd_bo);
   assert(batch->cmd_bo->index == 0);

   if (batch->state_sizes)
      _mesa_hash_table_clear(batch->state_sizes, NULL);
}

static void
iris_batch_reset_and_clear_render_cache(struct iris_batch *batch)
{
   iris_batch_reset(batch);
   // XXX: iris_render_cache_set_clear(batch);
}

void
iris_batch_free(struct iris_batch *batch)
{
   for (int i = 0; i < batch->exec_count; i++) {
      iris_bo_unreference(batch->exec_bos[i]);
   }
   free(batch->batch_relocs.relocs);
   free(batch->state_relocs.relocs);
   free(batch->exec_bos);
   free(batch->validation_list);

   iris_bo_unreference(batch->cmd_bo);
   iris_bo_unreference(batch->state_bo);
   iris_bo_unreference(batch->last_cmd_bo);
   if (batch->state_sizes)
      _mesa_hash_table_destroy(batch->state_sizes, NULL);
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
            struct iris_bo **bo_ptr,
            void **map_ptr,
            void **map_next_ptr,
            unsigned new_size)
{
   struct iris_bufmgr *bufmgr = batch->screen->bufmgr;

   const unsigned existing_bytes = *map_next_ptr - *map_ptr;

   void *old_map = *map_ptr;
   struct iris_bo *old_bo = *bo_ptr;

   struct iris_bo *new_bo = iris_bo_alloc(bufmgr, old_bo->name, new_size, 4096);

   perf_debug(batch->dbg, "Growing %s - ran out of space\n", old_bo->name);

   void *new_map = iris_bo_map(NULL, new_bo, MAP_READ | MAP_WRITE);
   memcpy(new_map, old_map, existing_bytes);

   /* Try to put the new BO at the same GTT offset as the old BO (which
    * we're throwing away, so it doesn't need to be there).
    *
    * This guarantees that our relocations continue to work: values we've
    * already written into the buffer, values we're going to write into the
    * buffer, and the validation/relocation lists all will match.
    */
   new_bo->gtt_offset = old_bo->gtt_offset;
   new_bo->index = old_bo->index;

   /* Batch/state buffers are per-context, and if we've run out of space,
    * we must have actually used them before, so...they will be in the list.
    */
   assert(old_bo->index < batch->exec_count);
   assert(batch->exec_bos[old_bo->index] == old_bo);

   /* Update the validation list to use the new BO. */
   batch->exec_bos[old_bo->index] = new_bo;
   batch->validation_list[old_bo->index].handle = new_bo->gem_handle;
   iris_bo_reference(new_bo);
   iris_bo_unreference(old_bo);

   /* Drop the *bo_ptr reference.  This should free the old BO. */
   iris_bo_unreference(old_bo);

   *bo_ptr = new_bo;
   *map_ptr = new_map;
   *map_next_ptr = new_map + existing_bytes;
}

void
iris_require_command_space(struct iris_batch *batch, unsigned size)
{
   if (iris_batch_used(batch) + size >= BATCH_SZ) {
      if (!batch->no_wrap) {
         iris_batch_flush(batch);
      } else {
         const unsigned new_size =
            MIN2(batch->cmd_bo->size + batch->cmd_bo->size / 2,
                 MAX_BATCH_SIZE);
         grow_buffer(batch, &batch->cmd_bo, &batch->cmd_map,
                     &batch->cmd_map_next, new_size);
         assert(iris_batch_used(batch) + size < batch->cmd_bo->size);
      }
   }
}

void
iris_batch_emit(struct iris_batch *batch, const void *data, unsigned size)
{
   iris_require_command_space(batch, size);
   memcpy(batch->cmd_map_next, data, size);
}

/**
 * Called when starting a new batch buffer.
 */
static void
iris_new_batch(struct iris_batch *batch)
{
   /* Unreference any BOs held by the previous batch, and reset counts. */
   for (int i = 0; i < batch->exec_count; i++) {
      iris_bo_unreference(batch->exec_bos[i]);
      batch->exec_bos[i] = NULL;
   }
   batch->batch_relocs.reloc_count = 0;
   batch->state_relocs.reloc_count = 0;
   batch->exec_count = 0;
   batch->aperture_space = 0;

   iris_bo_unreference(batch->state_bo);

   /* Create a new batchbuffer and reset the associated state: */
   iris_batch_reset_and_clear_render_cache(batch);
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
   iris_bo_unmap(batch->cmd_bo);
   iris_bo_unmap(batch->state_bo);

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
   int flags = I915_EXEC_NO_RELOC |
               I915_EXEC_BATCH_FIRST |
               I915_EXEC_HANDLE_LUT |
               I915_EXEC_RENDER;

   /* Set statebuffer relocations */
   const unsigned state_index = batch->state_bo->index;
   if (state_index < batch->exec_count &&
       batch->exec_bos[state_index] == batch->state_bo) {
      struct drm_i915_gem_exec_object2 *entry =
         &batch->validation_list[state_index];
      assert(entry->handle == batch->state_bo->gem_handle);
      entry->relocation_count = batch->state_relocs.reloc_count;
      entry->relocs_ptr = (uintptr_t) batch->state_relocs.relocs;
   }

   /* Set batchbuffer relocations */
   struct drm_i915_gem_exec_object2 *entry = &batch->validation_list[0];
   assert(entry->handle == batch->cmd_bo->gem_handle);
   entry->relocation_count = batch->batch_relocs.reloc_count;
   entry->relocs_ptr = (uintptr_t) batch->batch_relocs.relocs;

   struct drm_i915_gem_execbuffer2 execbuf = {
      .buffers_ptr = (uintptr_t) batch->validation_list,
      .buffer_count = batch->exec_count,
      .batch_start_offset = 0,
      .batch_len = iris_batch_used(batch),
      .flags = flags,
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
   if (ret != 0)
      ret = -errno;

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
   if (iris_batch_used(batch) == 0)
      return 0;

   /* Check that we didn't just wrap our batchbuffer at a bad time. */
   assert(!batch->no_wrap);

   iris_finish_batch(batch);

   if (unlikely(INTEL_DEBUG & (DEBUG_BATCH | DEBUG_SUBMIT))) {
      int bytes_for_commands = iris_batch_used(batch);
      int bytes_for_state = iris_state_used(batch);
      fprintf(stderr, "%19s:%-3d: Batchbuffer flush with %5db (%0.1f%%) (pkt),"
              " %5db (%0.1f%%) (state), %4d BOs (%0.1fMb aperture),"
              " %4d batch relocs, %4d state relocs\n", file, line,
              bytes_for_commands, 100.0f * bytes_for_commands / BATCH_SZ,
              bytes_for_state, 100.0f * bytes_for_state / STATE_SZ,
              batch->exec_count,
              (float) batch->aperture_space / (1024 * 1024),
              batch->batch_relocs.reloc_count,
              batch->state_relocs.reloc_count);
   }

   int ret = submit_batch(batch, in_fence_fd, out_fence_fd);
   if (ret < 0)
      return ret;

   //throttle(brw);

   //if (unlikely(INTEL_DEBUG & DEBUG_BATCH))
      //do_batch_dump(brw);

   //if (brw->ctx.Const.ResetStrategy == GL_LOSE_CONTEXT_ON_RESET_ARB)
      //iris_check_for_reset(ice);

   if (unlikely(INTEL_DEBUG & DEBUG_SYNC)) {
      dbg_printf("waiting for idle\n");
      iris_bo_wait_rendering(batch->cmd_bo);
   }

   /* Start a new batch buffer. */
   iris_new_batch(batch);

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

   if (rlist->reloc_count == rlist->reloc_array_size) {
      rlist->reloc_array_size *= 2;
      rlist->relocs = realloc(rlist->relocs,
                              rlist->reloc_array_size *
                              sizeof(struct drm_i915_gem_relocation_entry));
   }

   unsigned int index = add_exec_bo(batch, target);
   struct drm_i915_gem_exec_object2 *entry = &batch->validation_list[index];

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
   assert(batch_offset <= batch->cmd_bo->size - sizeof(uint32_t));

   return emit_reloc(batch, &batch->batch_relocs, batch_offset,
                     target, target_offset, reloc_flags);
}

uint64_t
iris_state_reloc(struct iris_batch *batch, uint32_t state_offset,
                 struct iris_bo *target, uint32_t target_offset,
                 unsigned int reloc_flags)
{
   assert(state_offset <= batch->state_bo->size - sizeof(uint32_t));

   return emit_reloc(batch, &batch->state_relocs, state_offset,
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
 * Reserve some space in the statebuffer, or flush.
 *
 * This is used to estimate when we're near the end of the batch,
 * so we can flush early.
 */
void
iris_require_state_space(struct iris_batch *batch, unsigned size)
{
   if (iris_state_used(batch) + size >= STATE_SZ)
      iris_batch_flush(batch);
}

/**
 * Allocates a block of space in the batchbuffer for indirect state.
 */
void *
iris_alloc_state(struct iris_batch *batch,
                 int size, int alignment,
                 uint32_t *out_offset)
{
   assert(size < batch->cmd_bo->size);

   if (ALIGN(iris_state_used(batch), alignment) + size >= STATE_SZ) {
      if (!batch->no_wrap) {
         iris_batch_flush(batch);
      } else {
         const unsigned new_size =
            MIN2(batch->state_bo->size + batch->state_bo->size / 2,
                 MAX_STATE_SIZE);
         grow_buffer(batch, &batch->state_bo, &batch->state_map,
                     &batch->state_map_next, new_size);
      }
   }

   unsigned offset = ALIGN(iris_state_used(batch), alignment);
   assert(offset + size < batch->state_bo->size);

   if (unlikely(batch->state_sizes)) {
      _mesa_hash_table_insert(batch->state_sizes,
                              (void *) (uintptr_t) offset,
                              (void *) (uintptr_t) size);
   }

   batch->state_map_next = batch->state_map + offset + size;

   *out_offset = offset;
   return batch->state_map + offset;
}

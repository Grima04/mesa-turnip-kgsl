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
#include "iris_binder.h"
#include "iris_bufmgr.h"
#include "iris_context.h"

#include "drm-uapi/i915_drm.h"

#include "util/hash_table.h"
#include "util/set.h"
#include "main/macros.h"

#include <errno.h>
#include <xf86drm.h>

#define FILE_DEBUG_FLAG DEBUG_BUFMGR

#define BATCH_SZ (20 * 1024)

/* Terminating the batch takes either 4 bytes for MI_BATCH_BUFFER_END
 * or 12 bytes for MI_BATCH_BUFFER_START (when chaining).  Plus, we may
 * need an extra 4 bytes to pad out to the nearest QWord.  So reserve 16.
 */
#define BATCH_RESERVED 16

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
      fprintf(stderr, "[%2d]: %2d %-14s %p %-7s @ 0x%016llx (%"PRIu64"B)\n",
              i,
              batch->validation_list[i].handle,
              batch->exec_bos[i]->name,
              batch->exec_bos[i],
              (flags & EXEC_OBJECT_WRITE) ? "(write)" : "",
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

   batch->binder.bo = NULL;

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
      batch->decoder.max_vbo_decoded_lines = 32;
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
create_batch(struct iris_batch *batch)
{
   struct iris_screen *screen = batch->screen;
   struct iris_bufmgr *bufmgr = screen->bufmgr;

   batch->bo = iris_bo_alloc(bufmgr, "command buffer",
                             BATCH_SZ + BATCH_RESERVED, IRIS_MEMZONE_OTHER);
   batch->bo->kflags |= EXEC_OBJECT_CAPTURE;
   batch->map = iris_bo_map(NULL, batch->bo, MAP_READ | MAP_WRITE);
   batch->map_next = batch->map;
   batch->contains_draw = false;

   add_exec_bo(batch, batch->bo);
}

static void
iris_batch_reset(struct iris_batch *batch)
{
   if (batch->last_bo != NULL) {
      iris_bo_unreference(batch->last_bo);
      batch->last_bo = NULL;
   }
   batch->last_bo = batch->bo;
   batch->primary_batch_size = 0;

   create_batch(batch);
   assert(batch->bo->index == 0);

   iris_destroy_binder(&batch->binder);
   iris_init_binder(&batch->binder, batch->bo->bufmgr);

   if (batch->state_sizes)
      _mesa_hash_table_clear(batch->state_sizes, NULL);

   iris_cache_sets_clear(batch);
}

void
iris_batch_free(struct iris_batch *batch)
{
   for (int i = 0; i < batch->exec_count; i++) {
      iris_bo_unreference(batch->exec_bos[i]);
   }
   free(batch->exec_bos);
   free(batch->validation_list);
   iris_bo_unreference(batch->bo);
   batch->bo = NULL;
   batch->map = NULL;
   batch->map_next = NULL;

   iris_bo_unreference(batch->last_bo);

   _mesa_hash_table_destroy(batch->cache.render, NULL);
   _mesa_set_destroy(batch->cache.depth, NULL);

   iris_destroy_binder(&batch->binder);

   if (batch->state_sizes) {
      _mesa_hash_table_destroy(batch->state_sizes, NULL);
      gen_batch_decode_ctx_finish(&batch->decoder);
   }
}

static unsigned
batch_bytes_used(struct iris_batch *batch)
{
   return batch->map_next - batch->map;
}

/**
 * If we've chained to a secondary batch, or are getting near to the end,
 * then flush.  This should only be called between draws.
 */
void
iris_batch_maybe_flush(struct iris_batch *batch, unsigned estimate)
{
   if (batch->bo != batch->exec_bos[0] ||
       batch_bytes_used(batch) + estimate >= BATCH_SZ) {
      iris_batch_flush(batch);
   }
}

void
iris_require_command_space(struct iris_batch *batch, unsigned size)
{
   const unsigned required_bytes = batch_bytes_used(batch) + size;

   if (required_bytes >= BATCH_SZ) {
      /* We only support chaining a single time. */
      assert(batch->bo == batch->exec_bos[0]);

      uint32_t *cmd = batch->map_next;
      uint64_t *addr = batch->map_next + 4;
      uint32_t *noop = batch->map_next + 12;
      batch->map_next += 12;

      /* No longer held by batch->bo, still held by validation list */
      iris_bo_unreference(batch->bo);
      batch->primary_batch_size = ALIGN(batch_bytes_used(batch), 8);
      create_batch(batch);

      /* Emit MI_BATCH_BUFFER_START to chain to another batch. */
      *cmd = (0x31 << 23) | (1 << 8) | (3 - 2);
      *addr = batch->bo->gtt_offset;
      *noop = 0;
   }
}

void *
iris_get_command_space(struct iris_batch *batch, unsigned bytes)
{
   iris_require_command_space(batch, bytes);
   void *map = batch->map_next;
   batch->map_next += bytes;
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
   if (batch->bo == batch->exec_bos[0])
      batch->primary_batch_size = batch_bytes_used(batch);

   // XXX: ISP DIS

   /* Emit MI_BATCH_BUFFER_END to finish our batch.  Note that execbuf2
    * requires our batch size to be QWord aligned, so we pad it out if
    * necessary by emitting an extra MI_NOOP after the end.
    */
   const bool qword_aligned = (batch_bytes_used(batch) % 8) == 0;
   uint32_t *map = batch->map_next;

   map[0] = (0xA << 23);
   map[1] = 0;

   batch->map_next += qword_aligned ? 8 : 4;
}

static int
submit_batch(struct iris_batch *batch, int in_fence_fd, int *out_fence_fd)
{
   iris_bo_unmap(batch->bo);

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
      .batch_len = batch->primary_batch_size,
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
   if (batch_bytes_used(batch) == 0)
      return 0;

   iris_finish_batch(batch);

   if (unlikely(INTEL_DEBUG & (DEBUG_BATCH | DEBUG_SUBMIT))) {
      int bytes_for_commands = batch_bytes_used(batch);
      if (batch->bo != batch->exec_bos[0])
         bytes_for_commands += batch->primary_batch_size;
      fprintf(stderr, "%19s:%-3d: Batchbuffer flush with %5db (%0.1f%%), "
              "%4d BOs (%0.1fMb aperture)\n",
              file, line,
              bytes_for_commands, 100.0f * bytes_for_commands / BATCH_SZ,
              batch->exec_count,
              (float) batch->aperture_space / (1024 * 1024));
      dump_validation_list(batch);
   }

   if (unlikely(INTEL_DEBUG & DEBUG_BATCH)) {
      decode_batch(batch);
   }

   int ret = submit_batch(batch, in_fence_fd, out_fence_fd);

   //throttle(iris);

   if (ret < 0)
      return ret;

   //if (iris->ctx.Const.ResetStrategy == GL_LOSE_CONTEXT_ON_RESET_ARB)
      //iris_check_for_reset(ice);

   if (unlikely(INTEL_DEBUG & DEBUG_SYNC)) {
      dbg_printf("waiting for idle\n");
      iris_bo_wait_rendering(batch->bo);
   }

   /* Clean up after the batch we submitted and prepare for a new one. */
   for (int i = 0; i < batch->exec_count; i++) {
      iris_bo_unreference(batch->exec_bos[i]);
      batch->exec_bos[i] = NULL;
   }
   batch->exec_count = 0;
   batch->aperture_space = 0;

   /* Start a new batch buffer. */
   iris_batch_reset(batch);

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
   //if (batch->bo != batch->exec_bos[0]) {
   void *map = iris_bo_map(batch->dbg, batch->exec_bos[0], MAP_READ);
   gen_print_batch(&batch->decoder, map, batch->primary_batch_size,
                   batch->exec_bos[0]->gtt_offset);

      //fprintf(stderr, "Secondary batch...\n");
   //}

   //gen_print_batch(&batch->decoder, batch->map, batch_bytes_used(batch),
                   //batch->bo->gtt_offset);
}

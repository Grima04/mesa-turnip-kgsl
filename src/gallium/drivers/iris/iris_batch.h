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

#ifndef IRIS_BATCH_DOT_H
#define IRIS_BATCH_DOT_H

#include <stdint.h>
#include <stdbool.h>

struct iris_address {
   struct iris_bo *bo;
   unsigned reloc_flags;
   uint32_t offset;
};

struct iris_reloc_list {
   struct drm_i915_gem_relocation_entry *relocs;
   int reloc_count;
   int reloc_array_size;
};

struct iris_batch {
   struct iris_screen *screen;
   struct pipe_debug_callback *dbg;

   /** Current batchbuffer being queued up. */
   struct iris_bo *cmd_bo;
   /** Current statebuffer being queued up. */
   struct iris_bo *state_bo;

   /** Last BO submitted to the hardware.  Used for glFinish(). */
   struct iris_bo *last_cmd_bo;

   uint32_t hw_ctx_id;

   void *cmd_map_next;
   void *cmd_map;
   void *state_map;
   void *state_map_next;

   bool no_wrap;

   struct iris_reloc_list batch_relocs;
   struct iris_reloc_list state_relocs;

   /** The validation list */
   struct drm_i915_gem_exec_object2 *validation_list;
   struct iris_bo **exec_bos;
   int exec_count;
   int exec_array_size;

   /** The amount of aperture space (in bytes) used by all exec_bos */
   int aperture_space;

   /** Map from batch offset to iris_alloc_state data (with DEBUG_BATCH) */
   struct hash_table *state_sizes;
};

void iris_batch_init(struct iris_batch *batch,
                     struct iris_screen *screen,
                     struct pipe_debug_callback *dbg);
void iris_batch_free(struct iris_batch *batch);
void iris_require_command_space(struct iris_batch *batch, unsigned size);
void iris_require_state_space(struct iris_batch *batch, unsigned size);
void iris_batch_emit(struct iris_batch *batch, const void *data, unsigned size);
void *iris_alloc_state(struct iris_batch *batch, int size, int alignment,
                       uint32_t *out_offset);

int _iris_batch_flush_fence(struct iris_batch *batch,
                            int in_fence_fd, int *out_fence_fd,
                            const char *file, int line);


#define iris_batch_flush_fence(batch, in_fence_fd, out_fence_fd) \
   _iris_batch_flush_fence((batch), (in_fence_fd), (out_fence_fd), \
                           __FILE__, __LINE__)

#define iris_batch_flush(batch) iris_batch_flush_fence((batch), -1, NULL)

bool iris_batch_references(struct iris_batch *batch, struct iris_bo *bo);

#define RELOC_WRITE EXEC_OBJECT_WRITE

uint64_t iris_batch_reloc(struct iris_batch *batch,
                          uint32_t batch_offset,
                          struct iris_bo *target,
                          uint32_t target_offset,
                          unsigned flags);

uint64_t iris_state_reloc(struct iris_batch *batch,
                          uint32_t batch_offset,
                          struct iris_bo *target,
                          uint32_t target_offset,
                          unsigned flags);
#endif

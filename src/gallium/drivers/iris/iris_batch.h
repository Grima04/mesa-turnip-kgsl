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

/* The kernel assumes batchbuffers are smaller than 256kB. */
#define MAX_BATCH_SIZE (256 * 1024)

/* 3DSTATE_BINDING_TABLE_POINTERS has a U16 offset from Surface State Base
 * Address, which means that we can't put binding tables beyond 64kB.  This
 * effectively limits the maximum statebuffer size to 64kB.
 */
#define MAX_STATE_SIZE (64 * 1024)

struct iris_address {
   struct iris_bo *bo;
   uint64_t offset;
   bool write;
};

struct iris_batch_buffer {
   struct iris_bo *bo;
   void *map;
   void *map_next;

   struct iris_bo *partial_bo;
   unsigned partial_bytes;
};

struct iris_batch {
   struct iris_screen *screen;
   struct pipe_debug_callback *dbg;

   /** Current batchbuffer being queued up. */
   struct iris_batch_buffer cmdbuf;

   /** Last BO submitted to the hardware.  Used for glFinish(). */
   struct iris_bo *last_cmd_bo;

   uint32_t hw_ctx_id;

   /** Which ring this batch targets - a I915_EXEC_RING_MASK value */
   uint8_t ring;

   bool no_wrap;

   /** The validation list */
   struct drm_i915_gem_exec_object2 *validation_list;
   struct iris_bo **exec_bos;
   int exec_count;
   int exec_array_size;

   /** The amount of aperture space (in bytes) used by all exec_bos */
   int aperture_space;

   /** Map from batch offset to iris_alloc_state data (with DEBUG_BATCH) */
   struct hash_table *state_sizes;

   void (*emit_state_base_address)(struct iris_batch *batch);
};

void iris_init_batch(struct iris_batch *batch,
                     struct iris_screen *screen,
                     struct pipe_debug_callback *dbg,
                     uint8_t ring);
void iris_batch_free(struct iris_batch *batch);
void iris_require_command_space(struct iris_batch *batch, unsigned size);
void iris_batch_emit(struct iris_batch *batch, const void *data, unsigned size);

int _iris_batch_flush_fence(struct iris_batch *batch,
                            int in_fence_fd, int *out_fence_fd,
                            const char *file, int line);


#define iris_batch_flush_fence(batch, in_fence_fd, out_fence_fd) \
   _iris_batch_flush_fence((batch), (in_fence_fd), (out_fence_fd), \
                           __FILE__, __LINE__)

#define iris_batch_flush(batch) iris_batch_flush_fence((batch), -1, NULL)

bool iris_batch_references(struct iris_batch *batch, struct iris_bo *bo);

#define RELOC_WRITE EXEC_OBJECT_WRITE

void iris_use_pinned_bo(struct iris_batch *batch, struct iris_bo *bo,
                        bool writable);

#endif

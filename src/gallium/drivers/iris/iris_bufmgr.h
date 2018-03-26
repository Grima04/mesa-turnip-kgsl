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

#ifndef IRIS_BUFMGR_H
#define IRIS_BUFMGR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "util/macros.h"
#include "util/u_atomic.h"
#include "util/list.h"

struct gen_device_info;
struct pipe_debug_callback;

struct iris_bo {
   /**
    * Size in bytes of the buffer object.
    *
    * The size may be larger than the size originally requested for the
    * allocation, such as being aligned to page size.
    */
   uint64_t size;

   /** Buffer manager context associated with this buffer object */
   struct iris_bufmgr *bufmgr;

   /** The GEM handle for this buffer object. */
   uint32_t gem_handle;

   /**
    * Offset of the buffer inside the Graphics Translation Table.
    *
    * This is effectively our GPU address for the buffer and we use it
    * as our base for all state pointers into the buffer. However, since the
    * kernel may be forced to move it around during the course of the
    * buffer's lifetime, we can only know where the buffer was on the last
    * execbuf. We presume, and are usually right, that the buffer will not
    * move and so we use that last offset for the next batch and by doing
    * so we can avoid having the kernel perform a relocation fixup pass as
    * our pointers inside the batch will be using the correct base offset.
    *
    * Since we do use it as a base address for the next batch of pointers,
    * the kernel treats our offset as a request, and if possible will
    * arrange the buffer to placed at that address (trying to balance
    * the cost of buffer migration versus the cost of performing
    * relocations). Furthermore, we can force the kernel to place the buffer,
    * or report a failure if we specified a conflicting offset, at our chosen
    * offset by specifying EXEC_OBJECT_PINNED.
    *
    * Note the GTT may be either per context, or shared globally across the
    * system. On a shared system, our buffers have to contend for address
    * space with both aperture mappings and framebuffers and so are more
    * likely to be moved. On a full ppGTT system, each batch exists in its
    * own GTT, and so each buffer may have their own offset within each
    * context.
    */
   uint64_t gtt_offset;

   /**
    * The validation list index for this buffer, or -1 when not in a batch.
    * Note that a single buffer may be in multiple batches (contexts), and
    * this is a global field, which refers to the last batch using the BO.
    * It should not be considered authoritative, but can be used to avoid a
    * linear walk of the validation list in the common case by guessing that
    * exec_bos[bo->index] == bo and confirming whether that's the case.
    */
   unsigned index;

   /**
    * Boolean of whether the GPU is definitely not accessing the buffer.
    *
    * This is only valid when reusable, since non-reusable
    * buffers are those that have been shared with other
    * processes, so we don't know their state.
    */
   bool idle;

   int refcount;
   const char *name;

   uint64_t kflags;

   /**
    * Kenel-assigned global name for this object
    *
    * List contains both flink named and prime fd'd objects
    */
   unsigned global_name;

   /**
    * Current tiling mode
    */
   uint32_t tiling_mode;
   uint32_t swizzle_mode;
   uint32_t stride;

   time_t free_time;

   /** Mapped address for the buffer, saved across map/unmap cycles */
   void *map_cpu;
   /** GTT virtual address for the buffer, saved across map/unmap cycles */
   void *map_gtt;
   /** WC CPU address for the buffer, saved across map/unmap cycles */
   void *map_wc;

   /** BO cache list */
   struct list_head head;

   /**
    * Boolean of whether this buffer can be re-used
    */
   bool reusable;

   /**
    * Boolean of whether this buffer has been shared with an external client.
    */
   bool external;

   /**
    * Boolean of whether this buffer is cache coherent
    */
   bool cache_coherent;
};

#define BO_ALLOC_ZEROED     (1<<0)

/**
 * Allocate a buffer object.
 *
 * Buffer objects are not necessarily initially mapped into CPU virtual
 * address space or graphics device aperture.  They must be mapped
 * using iris_bo_map() to be used by the CPU.
 */
struct iris_bo *iris_bo_alloc(struct iris_bufmgr *bufmgr,
                              const char *name,
                              uint64_t size);

/**
 * Allocate a tiled buffer object.
 *
 * Alignment for tiled objects is set automatically; the 'flags'
 * argument provides a hint about how the object will be used initially.
 *
 * Valid tiling formats are:
 *  I915_TILING_NONE
 *  I915_TILING_X
 *  I915_TILING_Y
 */
struct iris_bo *iris_bo_alloc_tiled(struct iris_bufmgr *bufmgr,
                                    const char *name,
                                    uint64_t size,
                                    uint32_t tiling_mode,
                                    uint32_t pitch,
                                    unsigned flags);

/** Takes a reference on a buffer object */
static inline void
iris_bo_reference(struct iris_bo *bo)
{
   p_atomic_inc(&bo->refcount);
}

/**
 * Releases a reference on a buffer object, freeing the data if
 * no references remain.
 */
void iris_bo_unreference(struct iris_bo *bo);

#define MAP_READ          PIPE_TRANSFER_READ
#define MAP_WRITE         PIPE_TRANSFER_WRITE
#define MAP_ASYNC         PIPE_TRANSFER_UNSYNCHRONIZED
#define MAP_PERSISTENT    PIPE_TRANSFER_PERSISTENT
#define MAP_COHERENT      PIPE_TRANSFER_COHERENT
/* internal */
#define MAP_INTERNAL_MASK (0xff << 24)
#define MAP_RAW           (0x01 << 24)

/**
 * Maps the buffer into userspace.
 *
 * This function will block waiting for any existing execution on the
 * buffer to complete, first.  The resulting mapping is returned.
 */
MUST_CHECK void *iris_bo_map(struct pipe_debug_callback *dbg,
                             struct iris_bo *bo, unsigned flags);

/**
 * Reduces the refcount on the userspace mapping of the buffer
 * object.
 */
static inline int iris_bo_unmap(struct iris_bo *bo) { return 0; }

/** Write data into an object. */
int iris_bo_subdata(struct iris_bo *bo, uint64_t offset,
                   uint64_t size, const void *data);
/**
 * Waits for rendering to an object by the GPU to have completed.
 *
 * This is not required for any access to the BO by bo_map,
 * bo_subdata, etc.  It is merely a way for the driver to implement
 * glFinish.
 */
void iris_bo_wait_rendering(struct iris_bo *bo);

/**
 * Tears down the buffer manager instance.
 */
void iris_bufmgr_destroy(struct iris_bufmgr *bufmgr);

/**
 * Get the current tiling (and resulting swizzling) mode for the bo.
 *
 * \param buf Buffer to get tiling mode for
 * \param tiling_mode returned tiling mode
 * \param swizzle_mode returned swizzling mode
 */
int iris_bo_get_tiling(struct iris_bo *bo, uint32_t *tiling_mode,
                      uint32_t *swizzle_mode);

/**
 * Create a visible name for a buffer which can be used by other apps
 *
 * \param buf Buffer to create a name for
 * \param name Returned name
 */
int iris_bo_flink(struct iris_bo *bo, uint32_t *name);

/**
 * Returns 1 if mapping the buffer for write could cause the process
 * to block, due to the object being active in the GPU.
 */
int iris_bo_busy(struct iris_bo *bo);

/**
 * Specify the volatility of the buffer.
 * \param bo Buffer to create a name for
 * \param madv The purgeable status
 *
 * Use I915_MADV_DONTNEED to mark the buffer as purgeable, and it will be
 * reclaimed under memory pressure. If you subsequently require the buffer,
 * then you must pass I915_MADV_WILLNEED to mark the buffer as required.
 *
 * Returns 1 if the buffer was retained, or 0 if it was discarded whilst
 * marked as I915_MADV_DONTNEED.
 */
int iris_bo_madvise(struct iris_bo *bo, int madv);

/* drm_bacon_bufmgr_gem.c */
struct iris_bufmgr *iris_bufmgr_init(struct gen_device_info *devinfo, int fd);
struct iris_bo *iris_bo_gem_create_from_name(struct iris_bufmgr *bufmgr,
                                             const char *name,
                                             unsigned handle);
void iris_bufmgr_enable_reuse(struct iris_bufmgr *bufmgr);

int iris_bo_wait(struct iris_bo *bo, int64_t timeout_ns);

uint32_t iris_create_hw_context(struct iris_bufmgr *bufmgr);

#define IRIS_CONTEXT_LOW_PRIORITY    ((I915_CONTEXT_MIN_USER_PRIORITY-1)/2)
#define IRIS_CONTEXT_MEDIUM_PRIORITY (I915_CONTEXT_DEFAULT_PRIORITY)
#define IRIS_CONTEXT_HIGH_PRIORITY   ((I915_CONTEXT_MAX_USER_PRIORITY+1)/2)

int iris_hw_context_set_priority(struct iris_bufmgr *bufmgr,
                                 uint32_t ctx_id, int priority);

void iris_destroy_hw_context(struct iris_bufmgr *bufmgr, uint32_t ctx_id);

int iris_bo_export_dmabuf(struct iris_bo *bo, int *prime_fd);
struct iris_bo *iris_bo_import_dmabuf(struct iris_bufmgr *bufmgr, int prime_fd);

uint32_t iris_bo_export_gem_handle(struct iris_bo *bo);

int iris_reg_read(struct iris_bufmgr *bufmgr, uint32_t offset, uint64_t *out);

int drm_ioctl(int fd, unsigned long request, void *arg);


#endif /* IRIS_BUFMGR_H */

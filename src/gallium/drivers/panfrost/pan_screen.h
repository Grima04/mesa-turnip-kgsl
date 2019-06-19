/**************************************************************************
 *
 * Copyright 2018-2019 Alyssa Rosenzweig
 * Copyright 2018-2019 Collabora
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifndef PAN_SCREEN_H
#define PAN_SCREEN_H

#include "pipe/p_screen.h"
#include "pipe/p_defines.h"
#include "renderonly/renderonly.h"

#include <panfrost-misc.h>
#include "pan_allocate.h"

struct panfrost_context;
struct panfrost_resource;
struct panfrost_screen;

/* Flags for allocated memory */
#define PAN_ALLOCATE_EXECUTE (1 << 0)
#define PAN_ALLOCATE_GROWABLE (1 << 1)
#define PAN_ALLOCATE_INVISIBLE (1 << 2)
#define PAN_ALLOCATE_COHERENT_LOCAL (1 << 3)

struct panfrost_screen {
        struct pipe_screen base;
        int fd;

        struct renderonly *ro;

        /* Memory management is based on subdividing slabs with AMD's allocator */
        struct pb_slabs slabs;
        
        /* TODO: Where? */
        struct panfrost_resource *display_target;

        /* While we're busy building up the job for frame N, the GPU is
         * still busy executing frame N-1. So hold a reference to
         * yesterjob */
	int last_fragment_flushed;
        struct panfrost_job *last_job;
};

static inline struct panfrost_screen *
pan_screen(struct pipe_screen *p)
{
   return (struct panfrost_screen *)p;
}

void
panfrost_drm_allocate_slab(struct panfrost_screen *screen,
                           struct panfrost_memory *mem,
                           size_t pages,
                           bool same_va,
                           int extra_flags,
                           int commit_count,
                           int extent);
void
panfrost_drm_free_slab(struct panfrost_screen *screen,
                       struct panfrost_memory *mem);
struct panfrost_bo *
panfrost_drm_import_bo(struct panfrost_screen *screen,
                       struct winsys_handle *whandle);
int
panfrost_drm_export_bo(struct panfrost_screen *screen, int gem_handle,
                       unsigned int stride, struct winsys_handle *whandle);
void
panfrost_drm_free_imported_bo(struct panfrost_screen *screen,
                              struct panfrost_bo *bo);
int
panfrost_drm_submit_job(struct panfrost_context *ctx, u64 job_desc, int reqs,
                        struct pipe_surface *surf);
int
panfrost_drm_submit_vs_fs_job(struct panfrost_context *ctx, bool has_draws,
                              bool is_scanout);
struct panfrost_fence *
panfrost_fence_create(struct panfrost_context *ctx);
void
panfrost_drm_force_flush_fragment(struct panfrost_context *ctx,
                                  struct pipe_fence_handle **fence);
unsigned
panfrost_drm_query_gpu_version(struct panfrost_screen *screen);
int
panfrost_drm_init_context(struct panfrost_context *ctx);
void
panfrost_drm_fence_reference(struct pipe_screen *screen,
                             struct pipe_fence_handle **ptr,
                             struct pipe_fence_handle *fence);
boolean
panfrost_drm_fence_finish(struct pipe_screen *pscreen,
                          struct pipe_context *ctx,
                          struct pipe_fence_handle *fence,
                          uint64_t timeout);

#endif /* PAN_SCREEN_H */

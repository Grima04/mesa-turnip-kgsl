/*
 * Copyright (C) 2019 Alyssa Rosenzweig
 * Copyright (C) 2014-2017 Broadcom
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef __PAN_JOB_H__
#define __PAN_JOB_H__

#include "util/u_dynarray.h"
#include "pipe/p_state.h"
#include "pan_allocate.h"
#include "pan_resource.h"

#define PAN_REQ_MSAA            (1 << 0)
#define PAN_REQ_DEPTH_WRITE     (1 << 1)

/* A panfrost_batch corresponds to a bound FBO we're rendering to,
 * collecting over multiple draws. */

struct panfrost_batch {
        struct panfrost_context *ctx;
        struct pipe_framebuffer_state key;

        /* Buffers cleared (PIPE_CLEAR_* bitmask) */
        unsigned clear;

        /* Packed clear values, indexed by both render target as well as word.
         * Essentially, a single pixel is packed, with some padding to bring it
         * up to a 32-bit interval; that pixel is then duplicated over to fill
         * all 16-bytes */

        uint32_t clear_color[PIPE_MAX_COLOR_BUFS][4];
        float clear_depth;
        unsigned clear_stencil;

        /* Whether this job uses the corresponding requirement (PAN_REQ_*
         * bitmask) */
        unsigned requirements;

        /* The bounding box covered by this job, taking scissors into account.
         * Basically, the bounding box we have to run fragment shaders for */

        unsigned minx, miny;
        unsigned maxx, maxy;

        /* CPU pointers to the job descriptor headers. next_job is only
         * set at submit time (since only then are all the dependencies
         * known). The upshot is that this is append-only.
         *
         * These arrays contain the headers for the "primary batch", our jargon
         * referring to the part of the panfrost_job that actually contains
         * meaningful work. In an OpenGL ES setting, that means the
         * SET_VALUE/VERTEX/TILER jobs. Excluded is specifically the FRAGMENT
         * job, which is sent on as a secondary batch containing only a single
         * hardware job. Since there's one and only one FRAGMENT job issued per
         * panfrost_job, there is no need to do any scoreboarding / management;
         * it's easy enough to open-code it and it's not like we can get any
         * better anyway. */
        struct util_dynarray headers;

        /* (And the GPU versions; TODO maybe combine) */
        struct util_dynarray gpu_headers;

        /* The last job in the primary batch */
        struct panfrost_transfer last_job;

        /* The first/last tiler job */
        struct panfrost_transfer first_tiler;
        struct panfrost_transfer last_tiler;

        /* The first vertex job used as the input to a tiler job */
        struct panfrost_transfer first_vertex_for_tiler;

        /* The first job. Notice we've created a linked list */
        struct panfrost_transfer first_job;

        /* The number of jobs in the primary batch, essentially */
        unsigned job_index;

        /* BOs referenced -- will be used for flushing logic */
        struct set *bos;

        /* Current transient BO */
	struct panfrost_bo *transient_bo;

        /* Within the topmost transient BO, how much has been used? */
        unsigned transient_offset;

        /* Polygon list bound to the batch, or NULL if none bound yet */
        struct panfrost_bo *polygon_list;

        /* Scratchpath BO bound to the batch, or NULL if none bound yet */
        struct panfrost_bo *scratchpad;

        /* Tiler heap BO bound to the batch, or NULL if none bound yet */
        struct panfrost_bo *tiler_heap;

        /* Dummy tiler BO bound to the batch, or NULL if none bound yet */
        struct panfrost_bo *tiler_dummy;

        /* Framebuffer descriptor. */
        mali_ptr framebuffer;
};

/* Functions for managing the above */

struct panfrost_batch *
panfrost_get_batch_for_fbo(struct panfrost_context *ctx);

void
panfrost_batch_init(struct panfrost_context *ctx);

void
panfrost_batch_add_bo(struct panfrost_batch *batch, struct panfrost_bo *bo);

void panfrost_batch_add_fbo_bos(struct panfrost_batch *batch);

struct panfrost_bo *
panfrost_batch_create_bo(struct panfrost_batch *batch, size_t size,
                         uint32_t create_flags);

void
panfrost_batch_submit(struct panfrost_batch *batch);

void
panfrost_batch_set_requirements(struct panfrost_batch *batch);

mali_ptr
panfrost_batch_get_polygon_list(struct panfrost_batch *batch, unsigned size);

struct panfrost_bo *
panfrost_batch_get_scratchpad(struct panfrost_batch *batch);

struct panfrost_bo *
panfrost_batch_get_tiler_heap(struct panfrost_batch *batch);

struct panfrost_bo *
panfrost_batch_get_tiler_dummy(struct panfrost_batch *batch);

void
panfrost_batch_clear(struct panfrost_batch *batch,
                     unsigned buffers,
                     const union pipe_color_union *color,
                     double depth, unsigned stencil);

void
panfrost_batch_union_scissor(struct panfrost_batch *batch,
                             unsigned minx, unsigned miny,
                             unsigned maxx, unsigned maxy);

void
panfrost_batch_intersection_scissor(struct panfrost_batch *batch,
                                    unsigned minx, unsigned miny,
                                    unsigned maxx, unsigned maxy);

/* Scoreboarding */

void
panfrost_scoreboard_queue_compute_job(
        struct panfrost_batch *batch,
        struct panfrost_transfer job);

void
panfrost_scoreboard_queue_vertex_job(
        struct panfrost_batch *batch,
        struct panfrost_transfer vertex,
        bool requires_tiling);

void
panfrost_scoreboard_queue_tiler_job(
        struct panfrost_batch *batch,
        struct panfrost_transfer tiler);

void
panfrost_scoreboard_queue_fused_job(
        struct panfrost_batch *batch,
        struct panfrost_transfer vertex,
        struct panfrost_transfer tiler);
void
panfrost_scoreboard_queue_fused_job_prepend(
        struct panfrost_batch *batch,
        struct panfrost_transfer vertex,
        struct panfrost_transfer tiler);

void
panfrost_scoreboard_link_batch(struct panfrost_batch *batch);

bool
panfrost_batch_is_scanout(struct panfrost_batch *batch);

#endif

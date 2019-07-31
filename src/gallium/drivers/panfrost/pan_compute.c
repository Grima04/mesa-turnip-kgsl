/*
 * Copyright (C) 2019 Collabora, Ltd.
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
 * Authors (Collabora):
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 *
 */

#include "pan_context.h"
#include "util/u_memory.h"

static void *
panfrost_create_compute_state(
        struct pipe_context *pctx,
        const struct pipe_compute_state *cso)
{
        return mem_dup(cso, sizeof(*cso));
}

static void
panfrost_bind_compute_state(struct pipe_context *pipe, void *cso)
{
        struct pipe_compute_state *state = (struct pipe_compute_state *) cso;

        printf("Binding compute %p\n", state);
        /* Stub */
}

static void
panfrost_delete_compute_state(struct pipe_context *pipe, void *cso)
{
        free(cso);
}

/* Launch grid is the compute equivalent of draw_vbo, so in this routine, we
 * construct the COMPUTE job and some of its payload.
 */

static void
panfrost_launch_grid(struct pipe_context *pipe,
                const struct pipe_grid_info *info)
{
        struct panfrost_context *ctx = pan_context(pipe);

        struct mali_job_descriptor_header job = {
                .job_type = JOB_TYPE_COMPUTE,
                .job_descriptor_size = 1,
                .job_barrier = 1
        };

        /* TODO: Stub */
        struct midgard_payload_vertex_tiler *payload = &ctx->payloads[PIPE_SHADER_COMPUTE];

        /* Compute jobs have a "compute FBD". It's not a real framebuffer
         * descriptor - there is no framebuffer - but it takes the place of
         * one. As far as I can tell, it's actually the beginning of a
         * single-render-target framebuffer descriptor with almost everything
         * zeroed out.
         */
        struct mali_compute_fbd compute_fbd = {
                .unknown1 = {
                        0, 0x1F, 0, 0, 0, 0, 0, 0
                }
        };

        payload->postfix.framebuffer =
                panfrost_upload_transient(ctx, &compute_fbd, sizeof(compute_fbd));

        /* Invoke according to the grid info */

        panfrost_pack_work_groups_compute(&payload->prefix,
                        info->grid[0], info->grid[1], info->grid[2],
                        info->block[0], info->block[1], info->block[2]);

        /* Upload the payload */

        struct panfrost_transfer transfer = panfrost_allocate_transient(ctx, sizeof(job) + sizeof(*payload));
        memcpy(transfer.cpu, &job, sizeof(job));
        memcpy(transfer.cpu + sizeof(job), payload, sizeof(*payload));

        /* TODO: Do we want a special compute-only batch? */
        struct panfrost_job *batch = panfrost_get_job_for_fbo(ctx);

        /* Queue the job */
        panfrost_scoreboard_queue_compute_job(batch, transfer);
}

void
panfrost_compute_context_init(struct pipe_context *pctx)
{
        pctx->create_compute_state = panfrost_create_compute_state;
        pctx->bind_compute_state = panfrost_bind_compute_state;
        pctx->delete_compute_state = panfrost_delete_compute_state;

        pctx->launch_grid = panfrost_launch_grid;
}



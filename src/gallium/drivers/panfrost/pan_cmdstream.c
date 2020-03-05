/*
 * Copyright (C) 2018 Alyssa Rosenzweig
 * Copyright (C) 2020 Collabora Ltd.
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
 */

#include "pan_allocate.h"
#include "pan_cmdstream.h"
#include "pan_context.h"
#include "pan_job.h"

static void
panfrost_mali_viewport_init(struct panfrost_context *ctx,
                            struct mali_viewport *mvp)
{
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        /* Clip bounds are encoded as floats. The viewport itself is encoded as
         * (somewhat) asymmetric ints. */

        const struct pipe_scissor_state *ss = &ctx->scissor;

        memset(mvp, 0, sizeof(*mvp));

        /* By default, do no viewport clipping, i.e. clip to (-inf, inf) in
         * each direction. Clipping to the viewport in theory should work, but
         * in practice causes issues when we're not explicitly trying to
         * scissor */

        *mvp = (struct mali_viewport) {
                .clip_minx = -INFINITY,
                .clip_miny = -INFINITY,
                .clip_maxx = INFINITY,
                .clip_maxy = INFINITY,
        };

        /* Always scissor to the viewport by default. */
        float vp_minx = (int) (vp->translate[0] - fabsf(vp->scale[0]));
        float vp_maxx = (int) (vp->translate[0] + fabsf(vp->scale[0]));

        float vp_miny = (int) (vp->translate[1] - fabsf(vp->scale[1]));
        float vp_maxy = (int) (vp->translate[1] + fabsf(vp->scale[1]));

        float minz = (vp->translate[2] - fabsf(vp->scale[2]));
        float maxz = (vp->translate[2] + fabsf(vp->scale[2]));

        /* Apply the scissor test */

        unsigned minx, miny, maxx, maxy;

        if (ss && ctx->rasterizer && ctx->rasterizer->base.scissor) {
                minx = MAX2(ss->minx, vp_minx);
                miny = MAX2(ss->miny, vp_miny);
                maxx = MIN2(ss->maxx, vp_maxx);
                maxy = MIN2(ss->maxy, vp_maxy);
        } else {
                minx = vp_minx;
                miny = vp_miny;
                maxx = vp_maxx;
                maxy = vp_maxy;
        }

        /* Hardware needs the min/max to be strictly ordered, so flip if we
         * need to. The viewport transformation in the vertex shader will
         * handle the negatives if we don't */

        if (miny > maxy) {
                unsigned temp = miny;
                miny = maxy;
                maxy = temp;
        }

        if (minx > maxx) {
                unsigned temp = minx;
                minx = maxx;
                maxx = temp;
        }

        if (minz > maxz) {
                float temp = minz;
                minz = maxz;
                maxz = temp;
        }

        /* Clamp to the framebuffer size as a last check */

        minx = MIN2(ctx->pipe_framebuffer.width, minx);
        maxx = MIN2(ctx->pipe_framebuffer.width, maxx);

        miny = MIN2(ctx->pipe_framebuffer.height, miny);
        maxy = MIN2(ctx->pipe_framebuffer.height, maxy);

        /* Upload */

        mvp->viewport0[0] = minx;
        mvp->viewport1[0] = MALI_POSITIVE(maxx);

        mvp->viewport0[1] = miny;
        mvp->viewport1[1] = MALI_POSITIVE(maxy);

        mvp->clip_minz = minz;
        mvp->clip_maxz = maxz;
}

void
panfrost_emit_viewport(struct panfrost_batch *batch,
                       struct midgard_payload_vertex_tiler *tp)
{
        struct panfrost_context *ctx = batch->ctx;
        struct mali_viewport mvp;

        panfrost_mali_viewport_init(batch->ctx,  &mvp);

        /* Update the job, unless we're doing wallpapering (whose lack of
         * scissor we can ignore, since if we "miss" a tile of wallpaper, it'll
         * just... be faster :) */

        if (!ctx->wallpaper_batch)
                panfrost_batch_union_scissor(batch, mvp.viewport0[0],
                                             mvp.viewport0[1],
                                             mvp.viewport1[0] + 1,
                                             mvp.viewport1[1] + 1);

        tp->postfix.viewport = panfrost_upload_transient(batch, &mvp,
                                                         sizeof(mvp));
}

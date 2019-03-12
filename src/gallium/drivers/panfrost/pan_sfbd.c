/*
 * Copyright 2018-2019 Alyssa Rosenzweig
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

#include "pan_context.h"
#include "pan_util.h"
#include "pan_format.h"

#include "util/u_format.h"

static unsigned
panfrost_sfbd_format(struct pipe_surface *surf)
{
        /* TODO */
        return 0xb84e0281; /* RGB32, no MSAA */
}

static void
panfrost_sfbd_enable_msaa(struct panfrost_context *ctx)
{
        ctx->fragment_sfbd.format |= MALI_FRAMEBUFFER_MSAA_A | MALI_FRAMEBUFFER_MSAA_B;
}

static void
panfrost_sfbd_clear(struct panfrost_job *job)
{
        struct panfrost_context *ctx = job->ctx;
        struct mali_single_framebuffer *sfbd = &ctx->fragment_sfbd;

        if (job->clear & PIPE_CLEAR_COLOR) {
                sfbd->clear_color_1 = job->clear_color;
                sfbd->clear_color_2 = job->clear_color;
                sfbd->clear_color_3 = job->clear_color;
                sfbd->clear_color_4 = job->clear_color;
        }

        if (job->clear & PIPE_CLEAR_DEPTH) {
                sfbd->clear_depth_1 = job->clear_depth;
                sfbd->clear_depth_2 = job->clear_depth;
                sfbd->clear_depth_3 = job->clear_depth;
                sfbd->clear_depth_4 = job->clear_depth;

                sfbd->depth_buffer = ctx->depth_stencil_buffer.gpu;
                sfbd->depth_buffer_enable = MALI_DEPTH_STENCIL_ENABLE;
        }

        if (job->clear & PIPE_CLEAR_STENCIL) {
                sfbd->clear_stencil = job->clear_stencil;

                sfbd->stencil_buffer = ctx->depth_stencil_buffer.gpu;
                sfbd->stencil_buffer_enable = MALI_DEPTH_STENCIL_ENABLE;
        }

        /* Set flags based on what has been cleared, for the SFBD case */
        /* XXX: What do these flags mean? */
        int clear_flags = 0x101100;

        if (!(job->clear & ~(PIPE_CLEAR_COLOR | PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL))) {
                /* On a tiler like this, it's fastest to clear all three buffers at once */

                clear_flags |= MALI_CLEAR_FAST;
        } else {
                clear_flags |= MALI_CLEAR_SLOW;

                if (job->clear & PIPE_CLEAR_STENCIL)
                        clear_flags |= MALI_CLEAR_SLOW_STENCIL;
        }

        sfbd->clear_flags = clear_flags;
}

static void
panfrost_sfbd_set_cbuf(
                struct panfrost_context *ctx,
                struct pipe_surface *surf)
{
        struct panfrost_resource *rsrc = pan_resource(surf->texture);

        signed stride =
                util_format_get_stride(surf->format, surf->texture->width0);

        ctx->fragment_sfbd.format = panfrost_sfbd_format(surf);

        if (rsrc->bo->layout == PAN_LINEAR) {
                mali_ptr framebuffer = rsrc->bo->gpu[0];

                /* The default is upside down from OpenGL's perspective. */
                if (panfrost_is_scanout(ctx)) {
                        framebuffer += stride * (surf->texture->height0 - 1);
                        stride = -stride;
                }

                ctx->fragment_sfbd.framebuffer = framebuffer;
                ctx->fragment_sfbd.stride = stride;
        } else {
                fprintf(stderr, "Invalid render layout\n");
                assert(0);
        }
}

static void
panfrost_sfbd_set_targets(struct panfrost_context *ctx)
{
        assert(ctx->pipe_framebuffer.nr_cbufs == 1);
        panfrost_sfbd_set_cbuf(ctx, ctx->pipe_framebuffer.cbufs[0]);

        if (ctx->pipe_framebuffer.zsbuf) {
                /* TODO */
        }
}

/* Creates an SFBD for the FRAGMENT section of the bound framebuffer */

mali_ptr
panfrost_sfbd_fragment(struct panfrost_context *ctx)
{
        struct panfrost_job *job = panfrost_get_job_for_fbo(ctx);

        struct mali_single_framebuffer fb = panfrost_emit_sfbd(ctx);
        memcpy(&ctx->fragment_sfbd, &fb, sizeof(fb));

        panfrost_sfbd_clear(job);
        panfrost_sfbd_set_targets(ctx);

        if (job->msaa)
                panfrost_sfbd_enable_msaa(ctx);

        return MALI_SFBD |
                panfrost_upload_transient(ctx, &ctx->fragment_sfbd, sizeof(fb));
}

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

static struct mali_rt_format
panfrost_mfbd_format(struct pipe_surface *surf)
{
        /* Explode details on the format */

        const struct util_format_description *desc =
                util_format_description(surf->texture->format);

        /* Fill in accordingly */

        struct mali_rt_format fmt = {
                .unk1 = 0x4000000,
                .unk2 = 0x1,
                .nr_channels = MALI_POSITIVE(desc->nr_channels),
                .flags = 0x444,
                .swizzle = panfrost_translate_swizzle_4(desc->swizzle),
                .unk4 = 0x8
        };

        return fmt;
}

static void
panfrost_mfbd_enable_msaa(struct panfrost_context *ctx)
{
        ctx->fragment_rts[0].format.flags |= MALI_MFBD_FORMAT_MSAA;

        /* XXX */
        ctx->fragment_mfbd.unk1 |= (1 << 4) | (1 << 1);
        ctx->fragment_mfbd.rt_count_2 = 4;
}

static void
panfrost_mfbd_clear(struct panfrost_job *job)
{
        struct panfrost_context *ctx = job->ctx;
        struct bifrost_render_target *buffer_color = &ctx->fragment_rts[0];
        struct bifrost_framebuffer *buffer_ds = &ctx->fragment_mfbd;

        if (job->clear & PIPE_CLEAR_COLOR) {
                buffer_color->clear_color_1 = job->clear_color;
                buffer_color->clear_color_2 = job->clear_color;
                buffer_color->clear_color_3 = job->clear_color;
                buffer_color->clear_color_4 = job->clear_color;
        }

        if (job->clear & PIPE_CLEAR_DEPTH) {
                buffer_ds->clear_depth = job->clear_depth;
        }

        if (job->clear & PIPE_CLEAR_STENCIL) {
                buffer_ds->clear_stencil = job->clear_stencil;
        }

        if (job->clear & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) {
                /* Setup combined 24/8 depth/stencil */
                ctx->fragment_mfbd.unk3 |= MALI_MFBD_EXTRA;
                ctx->fragment_extra.flags = 0x405;
                ctx->fragment_extra.ds_linear.depth = ctx->depth_stencil_buffer.gpu;
                ctx->fragment_extra.ds_linear.depth_stride = ctx->pipe_framebuffer.width * 4;
        }
}

static void
panfrost_mfbd_set_cbuf(
                struct panfrost_context *ctx,
                struct pipe_surface *surf,
                unsigned cb)
{
        struct panfrost_resource *rsrc = pan_resource(surf->texture);

        signed stride =
                util_format_get_stride(surf->format, surf->texture->width0);

        ctx->fragment_rts[cb].format = panfrost_mfbd_format(surf);

        /* Now, we set the layout specific pieces */

        if (rsrc->bo->layout == PAN_LINEAR) {
                mali_ptr framebuffer = rsrc->bo->gpu[0];

                /* The default is upside down from OpenGL's perspective. */
                if (panfrost_is_scanout(ctx)) {
                        framebuffer += stride * (surf->texture->height0 - 1);
                        stride = -stride;
                }

                /* MFBD specifies stride in tiles */
                ctx->fragment_rts[cb].framebuffer = framebuffer;
                ctx->fragment_rts[cb].framebuffer_stride = stride / 16;
        } else if (rsrc->bo->layout == PAN_AFBC) {
                ctx->fragment_rts[cb].afbc.metadata = rsrc->bo->afbc_slab.gpu;
                ctx->fragment_rts[cb].afbc.stride = 0;
                ctx->fragment_rts[cb].afbc.unk = 0x30009;

                ctx->fragment_rts[cb].format.flags |= MALI_MFBD_FORMAT_AFBC;

                mali_ptr afbc_main = rsrc->bo->afbc_slab.gpu + rsrc->bo->afbc_metadata_size;
                ctx->fragment_rts[cb].framebuffer = afbc_main;

                /* TODO: Investigate shift */
                ctx->fragment_rts[cb].framebuffer_stride = stride << 1;
        } else {
                fprintf(stderr, "Invalid render layout (cbuf %d)", cb);
                assert(0);
        }
}

static void
panfrost_mfbd_set_targets(struct panfrost_context *ctx)
{
        for (int cb = 0; cb < ctx->pipe_framebuffer.nr_cbufs; ++cb) {
                struct pipe_surface *surf = ctx->pipe_framebuffer.cbufs[cb];
                panfrost_mfbd_set_cbuf(ctx, surf, cb);
        }

        /* Enable depth/stencil AFBC for the framebuffer (not the render target) */
        if (ctx->pipe_framebuffer.zsbuf) {
                struct panfrost_resource *rsrc = (struct panfrost_resource *) ctx->pipe_framebuffer.zsbuf->texture;

                if (rsrc->bo->layout == PAN_AFBC) {
                        ctx->fragment_mfbd.unk3 |= MALI_MFBD_EXTRA;

                        ctx->fragment_extra.flags =
                                MALI_EXTRA_PRESENT |
                                MALI_EXTRA_AFBC |
                                MALI_EXTRA_AFBC_ZS |
                                MALI_EXTRA_ZS |
                                0x1; /* unknown */

                        ctx->fragment_extra.ds_afbc.depth_stencil_afbc_metadata = rsrc->bo->afbc_slab.gpu;
                        ctx->fragment_extra.ds_afbc.depth_stencil_afbc_stride = 0;

                        ctx->fragment_extra.ds_afbc.depth_stencil = rsrc->bo->afbc_slab.gpu + rsrc->bo->afbc_metadata_size;

                        ctx->fragment_extra.ds_afbc.zero1 = 0x10009;
                        ctx->fragment_extra.ds_afbc.padding = 0x1000;

                        ctx->fragment_mfbd.unk3 |= MALI_MFBD_DEPTH_WRITE;
                }
        }

        /* For the special case of a depth-only FBO, we need to attach a dummy render target */

        if (ctx->pipe_framebuffer.nr_cbufs == 0) {
                struct mali_rt_format null_rt = {
                        .unk1 = 0x4000000,
                        .unk4 = 0x8
                };

                ctx->fragment_rts[0].format = null_rt;
                ctx->fragment_rts[0].framebuffer = 0;
                ctx->fragment_rts[0].framebuffer_stride = 0;
        }
}

/* Helper for sequential uploads used for MFBD */

#define UPLOAD(dest, offset, src, max) { \
        size_t sz = sizeof(src); \
        memcpy(dest.cpu + offset, &src, sz); \
        assert((offset + sz) <= max); \
        offset += sz; \
}

static mali_ptr
panfrost_mfbd_upload(struct panfrost_context *ctx)
{
        off_t offset = 0;

        /* We always upload at least one (dummy) cbuf */
        unsigned cbufs = MAX2(ctx->pipe_framebuffer.nr_cbufs, 1);

        /* There may be extra data stuck in the middle */
        bool has_extra = ctx->fragment_mfbd.unk3 & MALI_MFBD_EXTRA;

        /* Compute total size for transfer */

        size_t total_sz =
                sizeof(struct bifrost_framebuffer) +
                (has_extra ? sizeof(struct bifrost_fb_extra) : 0) +
                sizeof(struct bifrost_render_target) * cbufs;

        struct panfrost_transfer m_f_trans =
                panfrost_allocate_transient(ctx, total_sz);

        /* Do the transfer */

        UPLOAD(m_f_trans, offset, ctx->fragment_mfbd, total_sz);

        if (has_extra)
                UPLOAD(m_f_trans, offset, ctx->fragment_extra, total_sz);

        for (unsigned c = 0; c < cbufs; ++c) {
                UPLOAD(m_f_trans, offset, ctx->fragment_rts[c], total_sz);
        }

        /* Return pointer suitable for the fragment seciton */
        return m_f_trans.gpu | MALI_MFBD | (has_extra ? 2 : 0);
}

#undef UPLOAD

/* Creates an MFBD for the FRAGMENT section of the bound framebuffer */

mali_ptr
panfrost_mfbd_fragment(struct panfrost_context *ctx)
{
        struct panfrost_job *job = panfrost_get_job_for_fbo(ctx);

        struct bifrost_framebuffer fb = panfrost_emit_mfbd(ctx);

        /* XXX: MRT case */
        fb.rt_count_2 = 1;
        fb.unk3 = 0x100;

        struct bifrost_render_target rt = {};

        memcpy(&ctx->fragment_rts[0], &rt, sizeof(rt));
        memset(&ctx->fragment_extra, 0, sizeof(ctx->fragment_extra));
        memcpy(&ctx->fragment_mfbd, &fb, sizeof(fb));

        panfrost_mfbd_clear(job);
        panfrost_mfbd_set_targets(ctx);

        if (job->msaa)
                panfrost_mfbd_enable_msaa(ctx);

        if (ctx->pipe_framebuffer.nr_cbufs == 1) {
                struct panfrost_resource *rsrc = (struct panfrost_resource *) ctx->pipe_framebuffer.cbufs[0]->texture;

                if (rsrc->bo->has_checksum) {
                        int stride = util_format_get_stride(rsrc->base.format, rsrc->base.width0);

                        ctx->fragment_mfbd.unk3 |= MALI_MFBD_EXTRA;
                        ctx->fragment_extra.flags |= MALI_EXTRA_PRESENT;
                        ctx->fragment_extra.checksum_stride = rsrc->bo->checksum_stride;
                        ctx->fragment_extra.checksum = rsrc->bo->gpu[0] + stride * rsrc->base.height0;
                }
        }

        return panfrost_mfbd_upload(ctx);
}

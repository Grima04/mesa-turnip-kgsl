/*
 * Copyright (C) 2014 Broadcom
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
 *   Tomeu Vizoso <tomeu.vizoso@collabora.com>
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 *
 */

#include "pan_blitter.h"
#include "pan_context.h"
#include "pan_util.h"
#include "util/format/u_format.h"

static void
panfrost_blitter_save(
        struct panfrost_context *ctx,
        struct blitter_context *blitter,
        bool render_cond)
{

        util_blitter_save_vertex_buffer_slot(blitter, ctx->vertex_buffers);
        util_blitter_save_vertex_elements(blitter, ctx->vertex);
        util_blitter_save_vertex_shader(blitter, ctx->shader[PIPE_SHADER_VERTEX]);
        util_blitter_save_rasterizer(blitter, ctx->rasterizer);
        util_blitter_save_viewport(blitter, &ctx->pipe_viewport);
        util_blitter_save_scissor(blitter, &ctx->scissor);
        util_blitter_save_fragment_shader(blitter, ctx->shader[PIPE_SHADER_FRAGMENT]);
        util_blitter_save_blend(blitter, ctx->blend);
        util_blitter_save_depth_stencil_alpha(blitter, ctx->depth_stencil);
        util_blitter_save_stencil_ref(blitter, &ctx->stencil_ref);
        util_blitter_save_so_targets(blitter, 0, NULL);
        util_blitter_save_sample_mask(blitter, ctx->sample_mask);

        util_blitter_save_framebuffer(blitter, &ctx->pipe_framebuffer);
        util_blitter_save_fragment_sampler_states(blitter,
                        ctx->sampler_count[PIPE_SHADER_FRAGMENT],
                        (void **)(&ctx->samplers[PIPE_SHADER_FRAGMENT]));
        util_blitter_save_fragment_sampler_views(blitter,
                        ctx->sampler_view_count[PIPE_SHADER_FRAGMENT],
                        (struct pipe_sampler_view **)&ctx->sampler_views[PIPE_SHADER_FRAGMENT]);
        util_blitter_save_fragment_constant_buffer_slot(blitter,
                        ctx->constant_buffer[PIPE_SHADER_FRAGMENT].cb);

        if (!render_cond) {
                util_blitter_save_render_condition(blitter,
                                (struct pipe_query *) ctx->cond_query,
                                ctx->cond_cond, ctx->cond_mode);
        }

}

static bool
panfrost_u_blitter_blit(struct pipe_context *pipe,
                        const struct pipe_blit_info *info)
{
        struct panfrost_context *ctx = pan_context(pipe);

        if (!util_blitter_is_blit_supported(ctx->blitter, info))
                unreachable("Unsupported blit\n");

        /* TODO: Scissor */

        panfrost_blitter_save(ctx, ctx->blitter, info->render_condition_enable);
        util_blitter_blit(ctx->blitter, info);

        return true;
}

static void
panfrost_blit_add_ctx_bos(struct panfrost_batch *batch,
                          struct pan_blit_context *ctx)
{
        if (ctx->pool.transient_bo) {
                panfrost_batch_add_bo(batch, ctx->pool.transient_bo,
                                      PAN_BO_ACCESS_SHARED |
                                      PAN_BO_ACCESS_READ |
                                      PAN_BO_ACCESS_VERTEX_TILER |
                                      PAN_BO_ACCESS_FRAGMENT);
        }

        util_dynarray_foreach(&ctx->pool.bos, struct panfrost_bo *, bo) {
                panfrost_batch_add_bo(batch, *bo,
                                      PAN_BO_ACCESS_SHARED |
                                      PAN_BO_ACCESS_READ |
                                      PAN_BO_ACCESS_VERTEX_TILER |
                                      PAN_BO_ACCESS_FRAGMENT);
        }
}

void
panfrost_blit(struct pipe_context *pipe,
              const struct pipe_blit_info *info)
{
        if (info->render_condition_enable &&
            !panfrost_render_condition_check(pan_context(pipe)))
                return;

        struct panfrost_device *dev = pan_device(pipe->screen);

        if (!(dev->debug & PAN_DBG_PANBLIT)) {
                panfrost_u_blitter_blit(pipe, info);
                return;
        }

        assert(!info->num_window_rectangles);
        assert(!info->alpha_blend);

        struct panfrost_resource *psrc = pan_resource(info->src.resource);
        struct panfrost_resource *pdst = pan_resource(info->dst.resource);
        struct panfrost_context *ctx = pan_context(pipe);

        struct pipe_surface tmpl = {
                .format = info->dst.format,
                .u.tex.level = info->dst.level,
        };

        struct pan_blit_info pinfo = {
                .src = {
                        .planes[0].format = info->src.format,
                        .planes[0].image = &psrc->image,
                        .level = info->src.level,
                        .start = { info->src.box.x, info->src.box.y },
                        .end = {
                                info->src.box.x + info->src.box.width - 1,
                                info->src.box.y + info->src.box.height - 1,
                        },
                },
                .dst = {
                        .planes[0].format = info->dst.format,
                        .planes[0].image = &pdst->image,
                        .level = info->dst.level,
                        .start = { info->dst.box.x, info->dst.box.y },
                        .end = {
                                info->dst.box.x + info->dst.box.width - 1,
                                info->dst.box.y + info->dst.box.height - 1,
                        },
                },
                .scissor = {
                        .enable = info->scissor_enable,
                        .minx = info->scissor.minx,
                        .miny = info->scissor.miny,
                        .maxx = info->scissor.maxx - 1,
                        .maxy = info->scissor.maxy - 1,
                },
                .nearest = info->filter == PIPE_TEX_FILTER_NEAREST,
        };

        if (info->dst.resource->target == PIPE_TEXTURE_2D_ARRAY ||
            info->dst.resource->target == PIPE_TEXTURE_1D_ARRAY ||
            info->dst.resource->target == PIPE_TEXTURE_CUBE ||
            info->dst.resource->target == PIPE_TEXTURE_CUBE_ARRAY) {
                pinfo.dst.start.layer = info->dst.box.z;
                pinfo.dst.end.layer = info->dst.box.z + info->dst.box.depth - 1;
        } else if (info->dst.resource->target == PIPE_TEXTURE_3D) {
                pinfo.dst.start.z = info->dst.box.z;
                pinfo.dst.end.z = info->dst.box.z + info->dst.box.depth - 1;
        }

        if (info->src.resource->target == PIPE_TEXTURE_2D_ARRAY ||
            info->src.resource->target == PIPE_TEXTURE_1D_ARRAY ||
            info->src.resource->target == PIPE_TEXTURE_CUBE ||
            info->src.resource->target == PIPE_TEXTURE_CUBE_ARRAY) {
                pinfo.src.start.layer = info->src.box.z;
                pinfo.src.end.layer = info->src.box.z + info->src.box.depth - 1;
        } else if (info->src.resource->target == PIPE_TEXTURE_3D) {
                pinfo.src.start.z = info->src.box.z;
                pinfo.src.end.z = info->src.box.z + info->src.box.depth - 1;
        }

        unsigned draw_flags = 0;

        /* For ZS buffers, only blit the component defined in the mask, the
         * preload logic will take care of preloading the other component.
         */
        if (util_format_is_depth_and_stencil(pinfo.dst.planes[0].format) &&
            util_format_is_depth_and_stencil(pinfo.src.planes[0].format) &&
            (info->mask & PIPE_MASK_ZS) != PIPE_MASK_ZS) {
                pinfo.src.planes[0].format =
                        (info->mask & PIPE_MASK_Z) ?
                        util_format_get_depth_only(info->src.format) :
                        util_format_stencil_only(info->src.format);
                pinfo.dst.planes[0].format =
                        (info->mask & PIPE_MASK_Z) ?
                        util_format_get_depth_only(info->dst.format) :
                        util_format_stencil_only(info->dst.format);
        }

        /* With our Z32_FLOAT_S8X24_UINT mapped to Z32_FLOAT + S8_UINT we
         * can't easily handle ZS <-> color blits, so let's forbid it for
         * now.
         */
        assert((!psrc->separate_stencil && !pdst->separate_stencil) ||
               !(info->mask & ~PIPE_MASK_ZS));

        if (psrc->separate_stencil) {
                if (pinfo.src.planes[0].format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT)
                        pinfo.src.planes[0].format = PIPE_FORMAT_Z32_FLOAT;

                if (info->mask & PIPE_MASK_S) {
                        unsigned s_idx = info->mask & PIPE_MASK_Z ? 1 : 0;

                        pinfo.src.planes[s_idx].format = PIPE_FORMAT_S8_UINT;
                        pinfo.src.planes[s_idx].image = &psrc->separate_stencil->image;
                }
        }

        if (info->mask & PIPE_MASK_Z)
                draw_flags |= PIPE_CLEAR_DEPTH;

        if (info->mask & PIPE_MASK_S)
                draw_flags |= PIPE_CLEAR_STENCIL;

        if (info->mask & PIPE_MASK_RGBA)
                draw_flags |= PIPE_CLEAR_COLOR0;

        unsigned dst_w = u_minify(info->dst.resource->width0, info->dst.level);
        unsigned dst_h = u_minify(info->dst.resource->height0, info->dst.level);
        unsigned minx = MAX2(0, pinfo.dst.start.x) & ~31;
        unsigned miny = MAX2(0, pinfo.dst.start.y) & ~31;
        unsigned maxx = MIN2(dst_w, ALIGN_POT(pinfo.dst.end.x + 1, 32));
        unsigned maxy = MIN2(dst_h, ALIGN_POT(pinfo.dst.end.y + 1, 32));

        if (info->scissor_enable) {
                minx = MAX2(minx, info->scissor.minx & ~31);
                miny = MAX2(miny, info->scissor.miny & ~31);
                maxx = MIN2(maxx, ALIGN_POT(info->scissor.maxx + 1, 32));
                maxy = MIN2(maxy, ALIGN_POT(info->scissor.maxy + 1, 32));
        }

        struct pan_blit_context bctx;

        pan_blit_ctx_init(dev, &pinfo, &bctx);
        do {
                if (bctx.dst.cur_layer < 0)
                        continue;

                tmpl.u.tex.first_layer = tmpl.u.tex.last_layer = bctx.dst.cur_layer;
                struct pipe_surface *dst_surf =
                        pipe->create_surface(pipe, info->dst.resource, &tmpl);
                struct pipe_framebuffer_state key = {
                        .width = dst_w,
                        .height = dst_h,
                };

                if (util_format_is_depth_or_stencil(info->dst.format)) {
                        key.zsbuf = dst_surf;
                } else {
                        key.cbufs[0] = dst_surf;
                        key.nr_cbufs = 1;
                }

                struct panfrost_batch *batch = panfrost_get_fresh_batch(ctx, &key);

                pipe_surface_reference(&dst_surf, NULL);

                panfrost_batch_add_bo(batch, pinfo.src.planes[0].image->data.bo,
                                      PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_READ |
                                      PAN_BO_ACCESS_FRAGMENT);

                if (pinfo.src.planes[1].image) {
                        panfrost_batch_add_bo(batch,
                                              pinfo.src.planes[1].image->data.bo,
                                              PAN_BO_ACCESS_SHARED |
                                              PAN_BO_ACCESS_READ |
                                              PAN_BO_ACCESS_FRAGMENT);
                }

                panfrost_batch_add_fbo_bos(batch);
                panfrost_blit_add_ctx_bos(batch, &bctx);
                batch->draws = draw_flags;
                batch->minx = minx;
                batch->miny = miny;
                batch->maxx = maxx;
                batch->maxy = maxy;

                mali_ptr tiler = pan_is_bifrost(dev) ?
                                 panfrost_batch_get_bifrost_tiler(batch, ~0) : 0;
                pan_blit(&bctx, &batch->pool, &batch->scoreboard,
                         panfrost_batch_reserve_tls(batch, false), tiler);
                panfrost_freeze_batch(batch);
        } while (pan_blit_next_surface(&bctx));

        pan_blit_ctx_cleanup(&bctx);
}

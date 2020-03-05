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

#include "util/macros.h"

#include "panfrost-quirks.h"

#include "pan_allocate.h"
#include "pan_bo.h"
#include "pan_cmdstream.h"
#include "pan_context.h"
#include "pan_job.h"

/* TODO: Bifrost requires just a mali_shared_memory, without the rest of the
 * framebuffer */

void
panfrost_vt_attach_framebuffer(struct panfrost_context *ctx,
                               struct midgard_payload_vertex_tiler *vt)
{
        struct panfrost_screen *screen = pan_screen(ctx->base.screen);
        struct panfrost_batch *batch = panfrost_get_batch_for_fbo(ctx);

        /* If we haven't, reserve space for the framebuffer */

        if (!batch->framebuffer.gpu) {
                unsigned size = (screen->quirks & MIDGARD_SFBD) ?
                        sizeof(struct mali_single_framebuffer) :
                        sizeof(struct mali_framebuffer);

                batch->framebuffer = panfrost_allocate_transient(batch, size);

                /* Tag the pointer */
                if (!(screen->quirks & MIDGARD_SFBD))
                        batch->framebuffer.gpu |= MALI_MFBD;
        }

        vt->postfix.shared_memory = batch->framebuffer.gpu;
}

void
panfrost_vt_update_rasterizer(struct panfrost_context *ctx,
                              struct midgard_payload_vertex_tiler *tp)
{
        struct panfrost_rasterizer *rasterizer = ctx->rasterizer;

        tp->gl_enables |= 0x7;
        SET_BIT(tp->gl_enables, MALI_FRONT_CCW_TOP,
                rasterizer && rasterizer->base.front_ccw);
        SET_BIT(tp->gl_enables, MALI_CULL_FACE_FRONT,
                rasterizer && (rasterizer->base.cull_face & PIPE_FACE_FRONT));
        SET_BIT(tp->gl_enables, MALI_CULL_FACE_BACK,
                rasterizer && (rasterizer->base.cull_face & PIPE_FACE_BACK));
        SET_BIT(tp->prefix.unknown_draw, MALI_DRAW_FLATSHADE_FIRST,
                rasterizer && rasterizer->base.flatshade_first);

        if (!panfrost_writes_point_size(ctx)) {
                bool points = tp->prefix.draw_mode == MALI_POINTS;
                float val = 0.0f;

                if (rasterizer)
                        val = points ?
                              rasterizer->base.point_size :
                              rasterizer->base.line_width;

                tp->primitive_size.constant = val;
        }
}

void
panfrost_vt_update_occlusion_query(struct panfrost_context *ctx,
                                   struct midgard_payload_vertex_tiler *tp)
{
        SET_BIT(tp->gl_enables, MALI_OCCLUSION_QUERY, ctx->occlusion_query);
        if (ctx->occlusion_query)
                tp->postfix.occlusion_counter = ctx->occlusion_query->bo->gpu;
        else
                tp->postfix.occlusion_counter = 0;
}

static void
panfrost_shader_meta_init(struct panfrost_context *ctx,
                          enum pipe_shader_type st,
                          struct mali_shader_meta *meta)
{
        struct panfrost_shader_state *ss = panfrost_get_shader_state(ctx, st);

        memset(meta, 0, sizeof(*meta));
        meta->shader = (ss->bo ? ss->bo->gpu : 0) | ss->first_tag;
        meta->midgard1.uniform_count = MIN2(ss->uniform_count,
                                            ss->uniform_cutoff);
        meta->midgard1.work_count = ss->work_reg_count;
        meta->attribute_count = ss->attribute_count;
        meta->varying_count = ss->varying_count;
        meta->midgard1.flags_hi = 0x8; /* XXX */
        meta->midgard1.flags_lo = 0x220;
        meta->texture_count = ctx->sampler_view_count[st];
        meta->sampler_count = ctx->sampler_count[st];
        meta->midgard1.uniform_buffer_count = panfrost_ubo_count(ctx, st);
}

static unsigned
panfrost_translate_compare_func(enum pipe_compare_func in)
{
        switch (in) {
        case PIPE_FUNC_NEVER:
                return MALI_FUNC_NEVER;

        case PIPE_FUNC_LESS:
                return MALI_FUNC_LESS;

        case PIPE_FUNC_EQUAL:
                return MALI_FUNC_EQUAL;

        case PIPE_FUNC_LEQUAL:
                return MALI_FUNC_LEQUAL;

        case PIPE_FUNC_GREATER:
                return MALI_FUNC_GREATER;

        case PIPE_FUNC_NOTEQUAL:
                return MALI_FUNC_NOTEQUAL;

        case PIPE_FUNC_GEQUAL:
                return MALI_FUNC_GEQUAL;

        case PIPE_FUNC_ALWAYS:
                return MALI_FUNC_ALWAYS;

        default:
                unreachable("Invalid func");
        }
}

static unsigned
panfrost_translate_stencil_op(enum pipe_stencil_op in)
{
        switch (in) {
        case PIPE_STENCIL_OP_KEEP:
                return MALI_STENCIL_KEEP;

        case PIPE_STENCIL_OP_ZERO:
                return MALI_STENCIL_ZERO;

        case PIPE_STENCIL_OP_REPLACE:
               return MALI_STENCIL_REPLACE;

        case PIPE_STENCIL_OP_INCR:
                return MALI_STENCIL_INCR;

        case PIPE_STENCIL_OP_DECR:
                return MALI_STENCIL_DECR;

        case PIPE_STENCIL_OP_INCR_WRAP:
                return MALI_STENCIL_INCR_WRAP;

        case PIPE_STENCIL_OP_DECR_WRAP:
                return MALI_STENCIL_DECR_WRAP;

        case PIPE_STENCIL_OP_INVERT:
                return MALI_STENCIL_INVERT;

        default:
                unreachable("Invalid stencil op");
        }
}

static unsigned
translate_tex_wrap(enum pipe_tex_wrap w)
{
        switch (w) {
        case PIPE_TEX_WRAP_REPEAT:
                return MALI_WRAP_REPEAT;

        case PIPE_TEX_WRAP_CLAMP:
                return MALI_WRAP_CLAMP;

        case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
                return MALI_WRAP_CLAMP_TO_EDGE;

        case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
                return MALI_WRAP_CLAMP_TO_BORDER;

        case PIPE_TEX_WRAP_MIRROR_REPEAT:
                return MALI_WRAP_MIRRORED_REPEAT;

        case PIPE_TEX_WRAP_MIRROR_CLAMP:
                return MALI_WRAP_MIRRORED_CLAMP;

        case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
                return MALI_WRAP_MIRRORED_CLAMP_TO_EDGE;

        case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
                return MALI_WRAP_MIRRORED_CLAMP_TO_BORDER;

        default:
                unreachable("Invalid wrap");
        }
}

void panfrost_sampler_desc_init(const struct pipe_sampler_state *cso,
                                struct mali_sampler_descriptor *hw)
{
        unsigned func = panfrost_translate_compare_func(cso->compare_func);
        bool min_nearest = cso->min_img_filter == PIPE_TEX_FILTER_NEAREST;
        bool mag_nearest = cso->mag_img_filter == PIPE_TEX_FILTER_NEAREST;
        bool mip_linear  = cso->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR;
        unsigned min_filter = min_nearest ? MALI_SAMP_MIN_NEAREST : 0;
        unsigned mag_filter = mag_nearest ? MALI_SAMP_MAG_NEAREST : 0;
        unsigned mip_filter = mip_linear  ?
                              (MALI_SAMP_MIP_LINEAR_1 | MALI_SAMP_MIP_LINEAR_2) : 0;
        unsigned normalized = cso->normalized_coords ? MALI_SAMP_NORM_COORDS : 0;

        *hw = (struct mali_sampler_descriptor) {
                .filter_mode = min_filter | mag_filter | mip_filter |
                               normalized,
                .wrap_s = translate_tex_wrap(cso->wrap_s),
                .wrap_t = translate_tex_wrap(cso->wrap_t),
                .wrap_r = translate_tex_wrap(cso->wrap_r),
                .compare_func = panfrost_flip_compare_func(func),
                .border_color = {
                        cso->border_color.f[0],
                        cso->border_color.f[1],
                        cso->border_color.f[2],
                        cso->border_color.f[3]
                },
                .min_lod = FIXED_16(cso->min_lod, false), /* clamp at 0 */
                .max_lod = FIXED_16(cso->max_lod, false),
                .lod_bias = FIXED_16(cso->lod_bias, true), /* can be negative */
                .seamless_cube_map = cso->seamless_cube_map,
        };

        /* If necessary, we disable mipmapping in the sampler descriptor by
         * clamping the LOD as tight as possible (from 0 to epsilon,
         * essentially -- remember these are fixed point numbers, so
         * epsilon=1/256) */

        if (cso->min_mip_filter == PIPE_TEX_MIPFILTER_NONE)
                hw->max_lod = hw->min_lod + 1;
}

static void
panfrost_make_stencil_state(const struct pipe_stencil_state *in,
                            struct mali_stencil_test *out)
{
        out->ref = 0; /* Gallium gets it from elsewhere */

        out->mask = in->valuemask;
        out->func = panfrost_translate_compare_func(in->func);
        out->sfail = panfrost_translate_stencil_op(in->fail_op);
        out->dpfail = panfrost_translate_stencil_op(in->zfail_op);
        out->dppass = panfrost_translate_stencil_op(in->zpass_op);
}

static void
panfrost_frag_meta_rasterizer_update(struct panfrost_context *ctx,
                                     struct mali_shader_meta *fragmeta)
{
        if (!ctx->rasterizer) {
                SET_BIT(fragmeta->unknown2_4, MALI_NO_MSAA, true);
                SET_BIT(fragmeta->unknown2_3, MALI_HAS_MSAA, false);
                fragmeta->depth_units = 0.0f;
                fragmeta->depth_factor = 0.0f;
                SET_BIT(fragmeta->unknown2_4, MALI_DEPTH_RANGE_A, false);
                SET_BIT(fragmeta->unknown2_4, MALI_DEPTH_RANGE_B, false);
                return;
        }

        bool msaa = ctx->rasterizer->base.multisample;

        /* TODO: Sample size */
        SET_BIT(fragmeta->unknown2_3, MALI_HAS_MSAA, msaa);
        SET_BIT(fragmeta->unknown2_4, MALI_NO_MSAA, !msaa);
        fragmeta->depth_units = ctx->rasterizer->base.offset_units * 2.0f;
        fragmeta->depth_factor = ctx->rasterizer->base.offset_scale;

        /* XXX: Which bit is which? Does this maybe allow offseting not-tri? */

        SET_BIT(fragmeta->unknown2_4, MALI_DEPTH_RANGE_A,
                ctx->rasterizer->base.offset_tri);
        SET_BIT(fragmeta->unknown2_4, MALI_DEPTH_RANGE_B,
                ctx->rasterizer->base.offset_tri);
}

static void
panfrost_frag_meta_zsa_update(struct panfrost_context *ctx,
                              struct mali_shader_meta *fragmeta)
{
        const struct pipe_depth_stencil_alpha_state *zsa = ctx->depth_stencil;
        int zfunc = PIPE_FUNC_ALWAYS;

        if (!zsa) {
                struct pipe_stencil_state default_stencil = {
                        .enabled = 0,
                        .func = PIPE_FUNC_ALWAYS,
                        .fail_op = MALI_STENCIL_KEEP,
                        .zfail_op = MALI_STENCIL_KEEP,
                        .zpass_op = MALI_STENCIL_KEEP,
                        .writemask = 0xFF,
                        .valuemask = 0xFF
                };

                panfrost_make_stencil_state(&default_stencil,
                                            &fragmeta->stencil_front);
                fragmeta->stencil_mask_front = default_stencil.writemask;
                fragmeta->stencil_back = fragmeta->stencil_front;
                fragmeta->stencil_mask_back = default_stencil.writemask;
                SET_BIT(fragmeta->unknown2_4, MALI_STENCIL_TEST, false);
                SET_BIT(fragmeta->unknown2_3, MALI_DEPTH_WRITEMASK, false);
        } else {
                SET_BIT(fragmeta->unknown2_4, MALI_STENCIL_TEST,
                        zsa->stencil[0].enabled);
                panfrost_make_stencil_state(&zsa->stencil[0],
                                            &fragmeta->stencil_front);
                fragmeta->stencil_mask_front = zsa->stencil[0].writemask;
                fragmeta->stencil_front.ref = ctx->stencil_ref.ref_value[0];

                /* If back-stencil is not enabled, use the front values */

                if (zsa->stencil[1].enabled) {
                        panfrost_make_stencil_state(&zsa->stencil[1],
                                                    &fragmeta->stencil_back);
                        fragmeta->stencil_mask_back = zsa->stencil[1].writemask;
                        fragmeta->stencil_back.ref = ctx->stencil_ref.ref_value[1];
                } else {
                        fragmeta->stencil_back = fragmeta->stencil_front;
                        fragmeta->stencil_mask_back = fragmeta->stencil_mask_front;
                        fragmeta->stencil_back.ref = fragmeta->stencil_front.ref;
                }

                if (zsa->depth.enabled)
                        zfunc = zsa->depth.func;

                /* Depth state (TODO: Refactor) */

                SET_BIT(fragmeta->unknown2_3, MALI_DEPTH_WRITEMASK,
                        zsa->depth.writemask);
        }

        fragmeta->unknown2_3 &= ~MALI_DEPTH_FUNC_MASK;
        fragmeta->unknown2_3 |= MALI_DEPTH_FUNC(panfrost_translate_compare_func(zfunc));
}

static void
panfrost_frag_meta_blend_update(struct panfrost_context *ctx,
                                struct mali_shader_meta *fragmeta,
                                struct midgard_blend_rt *rts)
{
        const struct panfrost_screen *screen = pan_screen(ctx->base.screen);

        SET_BIT(fragmeta->unknown2_4, MALI_NO_DITHER,
                (screen->quirks & MIDGARD_SFBD) && ctx->blend &&
                !ctx->blend->base.dither);

        /* Get blending setup */
        unsigned rt_count = MAX2(ctx->pipe_framebuffer.nr_cbufs, 1);

        struct panfrost_blend_final blend[PIPE_MAX_COLOR_BUFS];
        unsigned shader_offset = 0;
        struct panfrost_bo *shader_bo = NULL;

        for (unsigned c = 0; c < rt_count; ++c)
                blend[c] = panfrost_get_blend_for_context(ctx, c, &shader_bo,
                                                          &shader_offset);

         /* If there is a blend shader, work registers are shared. XXX: opt */

        for (unsigned c = 0; c < rt_count; ++c) {
                if (blend[c].is_shader)
                        fragmeta->midgard1.work_count = 16;
        }

        /* Even on MFBD, the shader descriptor gets blend shaders. It's *also*
         * copied to the blend_meta appended (by convention), but this is the
         * field actually read by the hardware. (Or maybe both are read...?).
         * Specify the last RTi with a blend shader. */

        fragmeta->blend.shader = 0;

        for (signed rt = (rt_count - 1); rt >= 0; --rt) {
                if (!blend[rt].is_shader)
                        continue;

                fragmeta->blend.shader = blend[rt].shader.gpu |
                                         blend[rt].shader.first_tag;
                break;
        }

        if (screen->quirks & MIDGARD_SFBD) {
                /* When only a single render target platform is used, the blend
                 * information is inside the shader meta itself. We additionally
                 * need to signal CAN_DISCARD for nontrivial blend modes (so
                 * we're able to read back the destination buffer) */

                SET_BIT(fragmeta->unknown2_3, MALI_HAS_BLEND_SHADER,
                        blend[0].is_shader);

                if (!blend[0].is_shader) {
                        fragmeta->blend.equation = *blend[0].equation.equation;
                        fragmeta->blend.constant = blend[0].equation.constant;
                }

                SET_BIT(fragmeta->unknown2_3, MALI_CAN_DISCARD,
                        !blend[0].no_blending);
                return;
        }

        /* Additional blend descriptor tacked on for jobs using MFBD */

        for (unsigned i = 0; i < rt_count; ++i) {
                rts[i].flags = 0x200;

                bool is_srgb = (ctx->pipe_framebuffer.nr_cbufs > i) &&
                               (ctx->pipe_framebuffer.cbufs[i]) &&
                               util_format_is_srgb(ctx->pipe_framebuffer.cbufs[i]->format);

                SET_BIT(rts[i].flags, MALI_BLEND_MRT_SHADER, blend[i].is_shader);
                SET_BIT(rts[i].flags, MALI_BLEND_LOAD_TIB, !blend[i].no_blending);
                SET_BIT(rts[i].flags, MALI_BLEND_SRGB, is_srgb);
                SET_BIT(rts[i].flags, MALI_BLEND_NO_DITHER, !ctx->blend->base.dither);

                if (blend[i].is_shader) {
                        rts[i].blend.shader = blend[i].shader.gpu | blend[i].shader.first_tag;
                } else {
                        rts[i].blend.equation = *blend[i].equation.equation;
                        rts[i].blend.constant = blend[i].equation.constant;
                }
        }
}

static void
panfrost_frag_shader_meta_init(struct panfrost_context *ctx,
                               struct mali_shader_meta *fragmeta,
                               struct midgard_blend_rt *rts)
{
        const struct panfrost_screen *screen = pan_screen(ctx->base.screen);
        struct panfrost_shader_state *fs;

        fs = panfrost_get_shader_state(ctx, PIPE_SHADER_FRAGMENT);

        fragmeta->alpha_coverage = ~MALI_ALPHA_COVERAGE(0.000000);
        fragmeta->unknown2_3 = MALI_DEPTH_FUNC(MALI_FUNC_ALWAYS) | 0x3010;
        fragmeta->unknown2_4 = 0x4e0;

        /* unknown2_4 has 0x10 bit set on T6XX and T720. We don't know why this
         * is required (independent of 32-bit/64-bit descriptors), or why it's
         * not used on later GPU revisions. Otherwise, all shader jobs fault on
         * these earlier chips (perhaps this is a chicken bit of some kind).
         * More investigation is needed. */

        SET_BIT(fragmeta->unknown2_4, 0x10, screen->quirks & MIDGARD_SFBD);

        /* Depending on whether it's legal to in the given shader, we try to
         * enable early-z testing (or forward-pixel kill?) */

        SET_BIT(fragmeta->midgard1.flags_lo, MALI_EARLY_Z,
                !fs->can_discard && !fs->writes_depth);

        /* Add the writes Z/S flags if needed. */
        SET_BIT(fragmeta->midgard1.flags_lo, MALI_WRITES_Z, fs->writes_depth);
        SET_BIT(fragmeta->midgard1.flags_hi, MALI_WRITES_S, fs->writes_stencil);

        /* Any time texturing is used, derivatives are implicitly calculated,
         * so we need to enable helper invocations */

        SET_BIT(fragmeta->midgard1.flags_lo, MALI_HELPER_INVOCATIONS,
                fs->helper_invocations);

        /* CAN_DISCARD should be set if the fragment shader possibly contains a
         * 'discard' instruction. It is likely this is related to optimizations
         * related to forward-pixel kill, as per "Mali Performance 3: Is
         * EGL_BUFFER_PRESERVED a good thing?" by Peter Harris */

        SET_BIT(fragmeta->unknown2_3, MALI_CAN_DISCARD, fs->can_discard);
        SET_BIT(fragmeta->midgard1.flags_lo, 0x400, fs->can_discard);

        panfrost_frag_meta_rasterizer_update(ctx, fragmeta);
        panfrost_frag_meta_zsa_update(ctx, fragmeta);
        panfrost_frag_meta_blend_update(ctx, fragmeta, rts);
}

void
panfrost_emit_shader_meta(struct panfrost_batch *batch,
                          enum pipe_shader_type st,
                          struct midgard_payload_vertex_tiler *vtp)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_shader_state *ss = panfrost_get_shader_state(ctx, st);

        if (!ss) {
                vtp->postfix.shader = 0;
                return;
        }

        struct mali_shader_meta meta;

        panfrost_shader_meta_init(ctx, st, &meta);

        /* Add the shader BO to the batch. */
        panfrost_batch_add_bo(batch, ss->bo,
                              PAN_BO_ACCESS_PRIVATE |
                              PAN_BO_ACCESS_READ |
                              panfrost_bo_access_for_stage(st));

        mali_ptr shader_ptr;

        if (st == PIPE_SHADER_FRAGMENT) {
                struct panfrost_screen *screen = pan_screen(ctx->base.screen);
                unsigned rt_count = MAX2(ctx->pipe_framebuffer.nr_cbufs, 1);
                size_t desc_size = sizeof(meta);
                struct midgard_blend_rt rts[4];
                struct panfrost_transfer xfer;

                assert(rt_count <= ARRAY_SIZE(rts));

                panfrost_frag_shader_meta_init(ctx, &meta, rts);

                if (!(screen->quirks & MIDGARD_SFBD))
                        desc_size += sizeof(*rts) * rt_count;

                xfer = panfrost_allocate_transient(batch, desc_size);

                memcpy(xfer.cpu, &meta, sizeof(meta));
                memcpy(xfer.cpu + sizeof(meta), rts, sizeof(*rts) * rt_count);

                shader_ptr = xfer.gpu;
        } else {
                shader_ptr = panfrost_upload_transient(batch, &meta,
                                                       sizeof(meta));
        }

        vtp->postfix.shader = shader_ptr;
}

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

static mali_ptr
panfrost_map_constant_buffer_gpu(struct panfrost_batch *batch,
                                 enum pipe_shader_type st,
                                 struct panfrost_constant_buffer *buf,
                                 unsigned index)
{
        struct pipe_constant_buffer *cb = &buf->cb[index];
        struct panfrost_resource *rsrc = pan_resource(cb->buffer);

        if (rsrc) {
                panfrost_batch_add_bo(batch, rsrc->bo,
                                      PAN_BO_ACCESS_SHARED |
                                      PAN_BO_ACCESS_READ |
                                      panfrost_bo_access_for_stage(st));

                /* Alignment gauranteed by
                 * PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT */
                return rsrc->bo->gpu + cb->buffer_offset;
        } else if (cb->user_buffer) {
                return panfrost_upload_transient(batch,
                                                 cb->user_buffer +
                                                 cb->buffer_offset,
                                                 cb->buffer_size);
        } else {
                unreachable("No constant buffer");
        }
}

struct sysval_uniform {
        union {
                float f[4];
                int32_t i[4];
                uint32_t u[4];
                uint64_t du[2];
        };
};

static void
panfrost_upload_viewport_scale_sysval(struct panfrost_batch *batch,
                                      struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        uniform->f[0] = vp->scale[0];
        uniform->f[1] = vp->scale[1];
        uniform->f[2] = vp->scale[2];
}

static void
panfrost_upload_viewport_offset_sysval(struct panfrost_batch *batch,
                                       struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        uniform->f[0] = vp->translate[0];
        uniform->f[1] = vp->translate[1];
        uniform->f[2] = vp->translate[2];
}

static void panfrost_upload_txs_sysval(struct panfrost_batch *batch,
                                       enum pipe_shader_type st,
                                       unsigned int sysvalid,
                                       struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        unsigned texidx = PAN_SYSVAL_ID_TO_TXS_TEX_IDX(sysvalid);
        unsigned dim = PAN_SYSVAL_ID_TO_TXS_DIM(sysvalid);
        bool is_array = PAN_SYSVAL_ID_TO_TXS_IS_ARRAY(sysvalid);
        struct pipe_sampler_view *tex = &ctx->sampler_views[st][texidx]->base;

        assert(dim);
        uniform->i[0] = u_minify(tex->texture->width0, tex->u.tex.first_level);

        if (dim > 1)
                uniform->i[1] = u_minify(tex->texture->height0,
                                         tex->u.tex.first_level);

        if (dim > 2)
                uniform->i[2] = u_minify(tex->texture->depth0,
                                         tex->u.tex.first_level);

        if (is_array)
                uniform->i[dim] = tex->texture->array_size;
}

static void
panfrost_upload_ssbo_sysval(struct panfrost_batch *batch,
                            enum pipe_shader_type st,
                            unsigned ssbo_id,
                            struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;

        assert(ctx->ssbo_mask[st] & (1 << ssbo_id));
        struct pipe_shader_buffer sb = ctx->ssbo[st][ssbo_id];

        /* Compute address */
        struct panfrost_bo *bo = pan_resource(sb.buffer)->bo;

        panfrost_batch_add_bo(batch, bo,
                              PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_RW |
                              panfrost_bo_access_for_stage(st));

        /* Upload address and size as sysval */
        uniform->du[0] = bo->gpu + sb.buffer_offset;
        uniform->u[2] = sb.buffer_size;
}

static void
panfrost_upload_sampler_sysval(struct panfrost_batch *batch,
                               enum pipe_shader_type st,
                               unsigned samp_idx,
                               struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;
        struct pipe_sampler_state *sampl = &ctx->samplers[st][samp_idx]->base;

        uniform->f[0] = sampl->min_lod;
        uniform->f[1] = sampl->max_lod;
        uniform->f[2] = sampl->lod_bias;

        /* Even without any errata, Midgard represents "no mipmapping" as
         * fixing the LOD with the clamps; keep behaviour consistent. c.f.
         * panfrost_create_sampler_state which also explains our choice of
         * epsilon value (again to keep behaviour consistent) */

        if (sampl->min_mip_filter == PIPE_TEX_MIPFILTER_NONE)
                uniform->f[1] = uniform->f[0] + (1.0/256.0);
}

static void
panfrost_upload_num_work_groups_sysval(struct panfrost_batch *batch,
                                       struct sysval_uniform *uniform)
{
        struct panfrost_context *ctx = batch->ctx;

        uniform->u[0] = ctx->compute_grid->grid[0];
        uniform->u[1] = ctx->compute_grid->grid[1];
        uniform->u[2] = ctx->compute_grid->grid[2];
}

static void
panfrost_upload_sysvals(struct panfrost_batch *batch, void *buf,
                        struct panfrost_shader_state *ss,
                        enum pipe_shader_type st)
{
        struct sysval_uniform *uniforms = (void *)buf;

        for (unsigned i = 0; i < ss->sysval_count; ++i) {
                int sysval = ss->sysval[i];

                switch (PAN_SYSVAL_TYPE(sysval)) {
                case PAN_SYSVAL_VIEWPORT_SCALE:
                        panfrost_upload_viewport_scale_sysval(batch,
                                                              &uniforms[i]);
                        break;
                case PAN_SYSVAL_VIEWPORT_OFFSET:
                        panfrost_upload_viewport_offset_sysval(batch,
                                                               &uniforms[i]);
                        break;
                case PAN_SYSVAL_TEXTURE_SIZE:
                        panfrost_upload_txs_sysval(batch, st,
                                                   PAN_SYSVAL_ID(sysval),
                                                   &uniforms[i]);
                        break;
                case PAN_SYSVAL_SSBO:
                        panfrost_upload_ssbo_sysval(batch, st,
                                                    PAN_SYSVAL_ID(sysval),
                                                    &uniforms[i]);
                        break;
                case PAN_SYSVAL_NUM_WORK_GROUPS:
                        panfrost_upload_num_work_groups_sysval(batch,
                                                               &uniforms[i]);
                        break;
                case PAN_SYSVAL_SAMPLER:
                        panfrost_upload_sampler_sysval(batch, st,
                                                       PAN_SYSVAL_ID(sysval),
                                                       &uniforms[i]);
                        break;
                default:
                        assert(0);
                }
        }
}

static const void *
panfrost_map_constant_buffer_cpu(struct panfrost_constant_buffer *buf,
                                 unsigned index)
{
        struct pipe_constant_buffer *cb = &buf->cb[index];
        struct panfrost_resource *rsrc = pan_resource(cb->buffer);

        if (rsrc)
                return rsrc->bo->cpu;
        else if (cb->user_buffer)
                return cb->user_buffer;
        else
                unreachable("No constant buffer");
}

void
panfrost_emit_const_buf(struct panfrost_batch *batch,
                        enum pipe_shader_type stage,
                        struct midgard_payload_vertex_tiler *vtp)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_shader_variants *all = ctx->shader[stage];

        if (!all)
                return;

        struct panfrost_constant_buffer *buf = &ctx->constant_buffer[stage];

        struct panfrost_shader_state *ss = &all->variants[all->active_variant];

        /* Uniforms are implicitly UBO #0 */
        bool has_uniforms = buf->enabled_mask & (1 << 0);

        /* Allocate room for the sysval and the uniforms */
        size_t sys_size = sizeof(float) * 4 * ss->sysval_count;
        size_t uniform_size = has_uniforms ? (buf->cb[0].buffer_size) : 0;
        size_t size = sys_size + uniform_size;
        struct panfrost_transfer transfer = panfrost_allocate_transient(batch,
                                                                        size);

        /* Upload sysvals requested by the shader */
        panfrost_upload_sysvals(batch, transfer.cpu, ss, stage);

        /* Upload uniforms */
        if (has_uniforms && uniform_size) {
                const void *cpu = panfrost_map_constant_buffer_cpu(buf, 0);
                memcpy(transfer.cpu + sys_size, cpu, uniform_size);
        }

        struct mali_vertex_tiler_postfix *postfix = &vtp->postfix;

        /* Next up, attach UBOs. UBO #0 is the uniforms we just
         * uploaded */

        unsigned ubo_count = panfrost_ubo_count(ctx, stage);
        assert(ubo_count >= 1);

        size_t sz = sizeof(uint64_t) * ubo_count;
        uint64_t ubos[PAN_MAX_CONST_BUFFERS];
        int uniform_count = ss->uniform_count;

        /* Upload uniforms as a UBO */
        ubos[0] = MALI_MAKE_UBO(2 + uniform_count, transfer.gpu);

        /* The rest are honest-to-goodness UBOs */

        for (unsigned ubo = 1; ubo < ubo_count; ++ubo) {
                size_t usz = buf->cb[ubo].buffer_size;
                bool enabled = buf->enabled_mask & (1 << ubo);
                bool empty = usz == 0;

                if (!enabled || empty) {
                        /* Stub out disabled UBOs to catch accesses */
                        ubos[ubo] = MALI_MAKE_UBO(0, 0xDEAD0000);
                        continue;
                }

                mali_ptr gpu = panfrost_map_constant_buffer_gpu(batch, stage,
                                                                buf, ubo);

                unsigned bytes_per_field = 16;
                unsigned aligned = ALIGN_POT(usz, bytes_per_field);
                ubos[ubo] = MALI_MAKE_UBO(aligned / bytes_per_field, gpu);
        }

        mali_ptr ubufs = panfrost_upload_transient(batch, ubos, sz);
        postfix->uniforms = transfer.gpu;
        postfix->uniform_buffers = ubufs;

        buf->dirty_mask = 0;
}

void
panfrost_emit_shared_memory(struct panfrost_batch *batch,
                            const struct pipe_grid_info *info,
                            struct midgard_payload_vertex_tiler *vtp)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_shader_variants *all = ctx->shader[PIPE_SHADER_COMPUTE];
        struct panfrost_shader_state *ss = &all->variants[all->active_variant];
        unsigned single_size = util_next_power_of_two(MAX2(ss->shared_size,
                                                           128));
        unsigned shared_size = single_size * info->grid[0] * info->grid[1] *
                               info->grid[2] * 4;
        struct panfrost_bo *bo = panfrost_batch_get_shared_memory(batch,
                                                                  shared_size,
                                                                  1);

        struct mali_shared_memory shared = {
                .shared_memory = bo->gpu,
                .shared_workgroup_count =
                        util_logbase2_ceil(info->grid[0]) +
                        util_logbase2_ceil(info->grid[1]) +
                        util_logbase2_ceil(info->grid[2]),
                .shared_unk1 = 0x2,
                .shared_shift = util_logbase2(single_size) - 1
        };

        vtp->postfix.shared_memory = panfrost_upload_transient(batch, &shared,
                                                               sizeof(shared));
}

static mali_ptr
panfrost_get_tex_desc(struct panfrost_batch *batch,
                      enum pipe_shader_type st,
                      struct panfrost_sampler_view *view)
{
        if (!view)
                return (mali_ptr) 0;

        struct pipe_sampler_view *pview = &view->base;
        struct panfrost_resource *rsrc = pan_resource(pview->texture);

        /* Add the BO to the job so it's retained until the job is done. */

        panfrost_batch_add_bo(batch, rsrc->bo,
                              PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_READ |
                              panfrost_bo_access_for_stage(st));

        panfrost_batch_add_bo(batch, view->bo,
                              PAN_BO_ACCESS_SHARED | PAN_BO_ACCESS_READ |
                              panfrost_bo_access_for_stage(st));

        return view->bo->gpu;
}

void
panfrost_emit_texture_descriptors(struct panfrost_batch *batch,
                                  enum pipe_shader_type stage,
                                  struct midgard_payload_vertex_tiler *vtp)
{
        struct panfrost_context *ctx = batch->ctx;

        if (!ctx->sampler_view_count[stage])
                return;

        uint64_t trampolines[PIPE_MAX_SHADER_SAMPLER_VIEWS];

         for (int i = 0; i < ctx->sampler_view_count[stage]; ++i)
                trampolines[i] = panfrost_get_tex_desc(batch, stage,
                                                       ctx->sampler_views[stage][i]);

         vtp->postfix.texture_trampoline = panfrost_upload_transient(batch,
                                                                     trampolines,
                                                                     sizeof(uint64_t) *
                                                                     ctx->sampler_view_count[stage]);
}

void
panfrost_emit_sampler_descriptors(struct panfrost_batch *batch,
                                  enum pipe_shader_type stage,
                                  struct midgard_payload_vertex_tiler *vtp)
{
        struct panfrost_context *ctx = batch->ctx;

        if (!ctx->sampler_count[stage])
                return;

        size_t desc_size = sizeof(struct mali_sampler_descriptor);
        size_t transfer_size = desc_size * ctx->sampler_count[stage];
        struct panfrost_transfer transfer = panfrost_allocate_transient(batch,
                                                                        transfer_size);
        struct mali_sampler_descriptor *desc = (struct mali_sampler_descriptor *)transfer.cpu;

        for (int i = 0; i < ctx->sampler_count[stage]; ++i)
                desc[i] = ctx->samplers[stage][i]->hw;

        vtp->postfix.sampler_descriptor = transfer.gpu;
}

void
panfrost_emit_vertex_tiler_jobs(struct panfrost_batch *batch,
                                struct midgard_payload_vertex_tiler *vp,
                                struct midgard_payload_vertex_tiler *tp)
{
        struct panfrost_context *ctx = batch->ctx;
        bool wallpapering = ctx->wallpaper_batch && batch->tiler_dep;

        if (wallpapering) {
                /* Inject in reverse order, with "predicted" job indices.
                 * THIS IS A HACK XXX */
                panfrost_new_job(batch, JOB_TYPE_TILER, false,
                                 batch->job_index + 2, tp, sizeof(*tp), true);
                panfrost_new_job(batch, JOB_TYPE_VERTEX, false, 0,
                                 vp, sizeof(*vp), true);
                return;
        }

        /* If rasterizer discard is enable, only submit the vertex */

        bool rasterizer_discard = ctx->rasterizer &&
                                  ctx->rasterizer->base.rasterizer_discard;

        unsigned vertex = panfrost_new_job(batch, JOB_TYPE_VERTEX, false, 0,
                                           vp, sizeof(*vp), false);

        if (rasterizer_discard)
                return;

        panfrost_new_job(batch, JOB_TYPE_TILER, false, vertex, tp, sizeof(*tp),
                         false);
}

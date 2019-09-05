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

#include <assert.h>

#include "pan_context.h"
#include "util/hash_table.h"
#include "util/ralloc.h"
#include "util/u_format.h"
#include "util/u_pack_color.h"

struct panfrost_batch *
panfrost_create_batch(struct panfrost_context *ctx)
{
        struct panfrost_batch *batch = rzalloc(ctx, struct panfrost_batch);

        batch->ctx = ctx;

        batch->bos = _mesa_set_create(batch,
                                      _mesa_hash_pointer,
                                      _mesa_key_pointer_equal);

        batch->minx = batch->miny = ~0;
        batch->maxx = batch->maxy = 0;
        batch->transient_offset = 0;

        util_dynarray_init(&batch->headers, batch);
        util_dynarray_init(&batch->gpu_headers, batch);
        util_dynarray_init(&batch->transient_indices, batch);

        return batch;
}

void
panfrost_free_batch(struct panfrost_context *ctx, struct panfrost_batch *batch)
{
        if (!batch)
                return;

        set_foreach(batch->bos, entry) {
                struct panfrost_bo *bo = (struct panfrost_bo *)entry->key;
                panfrost_bo_unreference(ctx->base.screen, bo);
        }

        /* Free up the transient BOs we're sitting on */
        struct panfrost_screen *screen = pan_screen(ctx->base.screen);

        pthread_mutex_lock(&screen->transient_lock);
        util_dynarray_foreach(&batch->transient_indices, unsigned, index) {
                /* Mark it free */
                BITSET_SET(screen->free_transient, *index);
        }
        pthread_mutex_unlock(&screen->transient_lock);

        /* Unreference the polygon list */
        panfrost_bo_unreference(ctx->base.screen, batch->polygon_list);

        _mesa_hash_table_remove_key(ctx->batches, &batch->key);

        if (ctx->batch == batch)
                ctx->batch = NULL;

        ralloc_free(batch);
}

struct panfrost_batch *
panfrost_get_batch(struct panfrost_context *ctx,
                 struct pipe_surface **cbufs, struct pipe_surface *zsbuf)
{
        /* Lookup the job first */

        struct panfrost_batch_key key = {
                .cbufs = {
                        cbufs[0],
                        cbufs[1],
                        cbufs[2],
                        cbufs[3],
                },
                .zsbuf = zsbuf
        };

        struct hash_entry *entry = _mesa_hash_table_search(ctx->batches, &key);

        if (entry)
                return entry->data;

        /* Otherwise, let's create a job */

        struct panfrost_batch *batch = panfrost_create_batch(ctx);

        /* Save the created job */

        memcpy(&batch->key, &key, sizeof(key));
        _mesa_hash_table_insert(ctx->batches, &batch->key, batch);

        return batch;
}

/* Get the job corresponding to the FBO we're currently rendering into */

struct panfrost_batch *
panfrost_get_batch_for_fbo(struct panfrost_context *ctx)
{
        /* If we're wallpapering, we special case to workaround
         * u_blitter abuse */

        if (ctx->wallpaper_batch)
                return ctx->wallpaper_batch;

        /* If we already began rendering, use that */

        if (ctx->batch) {
                assert(ctx->batch->key.zsbuf == ctx->pipe_framebuffer.zsbuf &&
                       !memcmp(ctx->batch->key.cbufs,
                               ctx->pipe_framebuffer.cbufs,
                               sizeof(ctx->batch->key.cbufs)));
                return ctx->batch;
        }

        /* If not, look up the job */

        struct pipe_surface **cbufs = ctx->pipe_framebuffer.cbufs;
        struct pipe_surface *zsbuf = ctx->pipe_framebuffer.zsbuf;
        struct panfrost_batch *batch = panfrost_get_batch(ctx, cbufs, zsbuf);

        /* Set this job as the current FBO job. Will be reset when updating the
         * FB state and when submitting or releasing a job.
         */
        ctx->batch = batch;
        return batch;
}

void
panfrost_batch_add_bo(struct panfrost_batch *batch, struct panfrost_bo *bo)
{
        if (!bo)
                return;

        if (_mesa_set_search(batch->bos, bo))
                return;

        panfrost_bo_reference(bo);
        _mesa_set_add(batch->bos, bo);
}

/* Returns the polygon list's GPU address if available, or otherwise allocates
 * the polygon list.  It's perfectly fast to use allocate/free BO directly,
 * since we'll hit the BO cache and this is one-per-batch anyway. */

mali_ptr
panfrost_batch_get_polygon_list(struct panfrost_batch *batch, unsigned size)
{
        if (batch->polygon_list) {
                assert(batch->polygon_list->size >= size);
        } else {
                struct panfrost_screen *screen = pan_screen(batch->ctx->base.screen);

                /* Create the BO as invisible, as there's no reason to map */

                batch->polygon_list = panfrost_drm_create_bo(screen,
                                size, PAN_ALLOCATE_INVISIBLE);
        }

        return batch->polygon_list->gpu;
}

void
panfrost_flush_jobs_writing_resource(struct panfrost_context *panfrost,
                                     struct pipe_resource *prsc)
{
#if 0
        struct hash_entry *entry = _mesa_hash_table_search(panfrost->write_jobs,
                                   prsc);
        if (entry) {
                struct panfrost_batch *batch = entry->data;
                panfrost_batch_submit(panfrost, job);
        }
#endif
        /* TODO stub */
}

void
panfrost_batch_submit(struct panfrost_context *ctx, struct panfrost_batch *batch)
{
        int ret;

        assert(batch);
        panfrost_scoreboard_link_batch(batch);

        bool has_draws = batch->last_job.gpu;

        ret = panfrost_drm_submit_vs_fs_batch(ctx, has_draws);

        if (ret)
                fprintf(stderr, "panfrost_batch_submit failed: %d\n", ret);

        /* The job has been submitted, let's invalidate the current FBO job
         * cache.
	 */
        assert(!ctx->batch || batch == ctx->batch);
        ctx->batch = NULL;

        /* Remove the job from the ctx->batches set so that future
         * panfrost_get_batch() calls don't see it.
         * We must reset the job key to avoid removing another valid entry when
         * the job is freed.
         */
        _mesa_hash_table_remove_key(ctx->batches, &batch->key);
        memset(&batch->key, 0, sizeof(batch->key));
}

void
panfrost_batch_set_requirements(struct panfrost_context *ctx,
                                struct panfrost_batch *batch)
{
        if (ctx->rasterizer && ctx->rasterizer->base.multisample)
                batch->requirements |= PAN_REQ_MSAA;

        if (ctx->depth_stencil && ctx->depth_stencil->depth.writemask)
                batch->requirements |= PAN_REQ_DEPTH_WRITE;
}

/* Helper to smear a 32-bit color across 128-bit components */

static void
pan_pack_color_32(uint32_t *packed, uint32_t v)
{
        for (unsigned i = 0; i < 4; ++i)
                packed[i] = v;
}

static void
pan_pack_color_64(uint32_t *packed, uint32_t lo, uint32_t hi)
{
        for (unsigned i = 0; i < 4; i += 2) {
                packed[i + 0] = lo;
                packed[i + 1] = hi;
        }
}

static void
pan_pack_color(uint32_t *packed, const union pipe_color_union *color, enum pipe_format format)
{
        /* Alpha magicked to 1.0 if there is no alpha */

        bool has_alpha = util_format_has_alpha(format);
        float clear_alpha = has_alpha ? color->f[3] : 1.0f;

        /* Packed color depends on the framebuffer format */

        const struct util_format_description *desc =
                util_format_description(format);

        if (util_format_is_rgba8_variant(desc)) {
                pan_pack_color_32(packed,
                                  (float_to_ubyte(clear_alpha) << 24) |
                                  (float_to_ubyte(color->f[2]) << 16) |
                                  (float_to_ubyte(color->f[1]) <<  8) |
                                  (float_to_ubyte(color->f[0]) <<  0));
        } else if (format == PIPE_FORMAT_B5G6R5_UNORM) {
                /* First, we convert the components to R5, G6, B5 separately */
                unsigned r5 = CLAMP(color->f[0], 0.0, 1.0) * 31.0;
                unsigned g6 = CLAMP(color->f[1], 0.0, 1.0) * 63.0;
                unsigned b5 = CLAMP(color->f[2], 0.0, 1.0) * 31.0;

                /* Then we pack into a sparse u32. TODO: Why these shifts? */
                pan_pack_color_32(packed, (b5 << 25) | (g6 << 14) | (r5 << 5));
        } else if (format == PIPE_FORMAT_B4G4R4A4_UNORM) {
                /* We scale the components against 0xF0 (=240.0), rather than 0xFF */
                unsigned r4 = CLAMP(color->f[0], 0.0, 1.0) * 240.0;
                unsigned g4 = CLAMP(color->f[1], 0.0, 1.0) * 240.0;
                unsigned b4 = CLAMP(color->f[2], 0.0, 1.0) * 240.0;
                unsigned a4 = CLAMP(clear_alpha, 0.0, 1.0) * 240.0;

                /* Pack on *byte* intervals */
                pan_pack_color_32(packed, (a4 << 24) | (b4 << 16) | (g4 << 8) | r4);
        } else if (format == PIPE_FORMAT_B5G5R5A1_UNORM) {
                /* Scale as expected but shift oddly */
                unsigned r5 = round(CLAMP(color->f[0], 0.0, 1.0)) * 31.0;
                unsigned g5 = round(CLAMP(color->f[1], 0.0, 1.0)) * 31.0;
                unsigned b5 = round(CLAMP(color->f[2], 0.0, 1.0)) * 31.0;
                unsigned a1 = round(CLAMP(clear_alpha, 0.0, 1.0)) * 1.0;

                pan_pack_color_32(packed, (a1 << 31) | (b5 << 25) | (g5 << 15) | (r5 << 5));
        } else {
                /* Try Gallium's generic default path. Doesn't work for all
                 * formats but it's a good guess. */

                union util_color out;

                if (util_format_is_pure_integer(format)) {
                        memcpy(out.ui, color->ui, 16);
                } else {
                        util_pack_color(color->f, format, &out);
                }

                unsigned size = util_format_get_blocksize(format);

                if (size == 1) {
                        unsigned b = out.ui[0];
                        unsigned s = b | (b << 8);
                        pan_pack_color_32(packed, s | (s << 16));
                } else if (size == 2)
                        pan_pack_color_32(packed, out.ui[0] | (out.ui[0] << 16));
                else if (size == 4)
                        pan_pack_color_32(packed, out.ui[0]);
                else if (size == 8)
                        pan_pack_color_64(packed, out.ui[0], out.ui[1]);
                else if (size == 16)
                        memcpy(packed, out.ui, 16);
                else
                        unreachable("Unknown generic format size packing clear colour");
        }
}

void
panfrost_batch_clear(struct panfrost_context *ctx,
                     struct panfrost_batch *batch,
                     unsigned buffers,
                     const union pipe_color_union *color,
                     double depth, unsigned stencil)

{
        if (buffers & PIPE_CLEAR_COLOR) {
                for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; ++i) {
                        if (!(buffers & (PIPE_CLEAR_COLOR0 << i)))
                                continue;

                        enum pipe_format format = ctx->pipe_framebuffer.cbufs[i]->format;
                        pan_pack_color(batch->clear_color[i], color, format);
                }
        }

        if (buffers & PIPE_CLEAR_DEPTH) {
                batch->clear_depth = depth;
        }

        if (buffers & PIPE_CLEAR_STENCIL) {
                batch->clear_stencil = stencil;
        }

        batch->clear |= buffers;

        /* Clearing affects the entire framebuffer (by definition -- this is
         * the Gallium clear callback, which clears the whole framebuffer. If
         * the scissor test were enabled from the GL side, the state tracker
         * would emit a quad instead and we wouldn't go down this code path) */

        panfrost_batch_union_scissor(batch, 0, 0,
                                     ctx->pipe_framebuffer.width,
                                     ctx->pipe_framebuffer.height);
}

void
panfrost_flush_jobs_reading_resource(struct panfrost_context *panfrost,
                                     struct pipe_resource *prsc)
{
        struct panfrost_resource *rsc = pan_resource(prsc);

        panfrost_flush_jobs_writing_resource(panfrost, prsc);

        hash_table_foreach(panfrost->batches, entry) {
                struct panfrost_batch *batch = entry->data;

                if (_mesa_set_search(batch->bos, rsc->bo)) {
                        printf("TODO: submit job for flush\n");
                        //panfrost_batch_submit(panfrost, job);
                        continue;
                }
        }
}

static bool
panfrost_batch_compare(const void *a, const void *b)
{
        return memcmp(a, b, sizeof(struct panfrost_batch_key)) == 0;
}

static uint32_t
panfrost_batch_hash(const void *key)
{
        return _mesa_hash_data(key, sizeof(struct panfrost_batch_key));
}

/* Given a new bounding rectangle (scissor), let the job cover the union of the
 * new and old bounding rectangles */

void
panfrost_batch_union_scissor(struct panfrost_batch *batch,
                             unsigned minx, unsigned miny,
                             unsigned maxx, unsigned maxy)
{
        batch->minx = MIN2(batch->minx, minx);
        batch->miny = MIN2(batch->miny, miny);
        batch->maxx = MAX2(batch->maxx, maxx);
        batch->maxy = MAX2(batch->maxy, maxy);
}

void
panfrost_batch_intersection_scissor(struct panfrost_batch *batch,
                                  unsigned minx, unsigned miny,
                                  unsigned maxx, unsigned maxy)
{
        batch->minx = MAX2(batch->minx, minx);
        batch->miny = MAX2(batch->miny, miny);
        batch->maxx = MIN2(batch->maxx, maxx);
        batch->maxy = MIN2(batch->maxy, maxy);
}

void
panfrost_batch_init(struct panfrost_context *ctx)
{
        ctx->batches = _mesa_hash_table_create(ctx,
                                               panfrost_batch_hash,
                                               panfrost_batch_compare);

        ctx->write_jobs = _mesa_hash_table_create(ctx,
                          _mesa_hash_pointer,
                          _mesa_key_pointer_equal);
}

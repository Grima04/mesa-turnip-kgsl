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

#include "drm-uapi/panfrost_drm.h"

#include "pan_bo.h"
#include "pan_context.h"
#include "util/hash_table.h"
#include "util/ralloc.h"
#include "util/u_format.h"
#include "util/u_pack_color.h"
#include "pan_util.h"
#include "pandecode/decode.h"

static struct panfrost_batch *
panfrost_create_batch(struct panfrost_context *ctx,
                      const struct pipe_framebuffer_state *key)
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
        util_copy_framebuffer_state(&batch->key, key);

        return batch;
}

static void
panfrost_free_batch(struct panfrost_batch *batch)
{
        if (!batch)
                return;

        struct panfrost_context *ctx = batch->ctx;

        set_foreach(batch->bos, entry) {
                struct panfrost_bo *bo = (struct panfrost_bo *)entry->key;
                panfrost_bo_unreference(bo);
        }

        _mesa_hash_table_remove_key(ctx->batches, &batch->key);

        if (ctx->batch == batch)
                ctx->batch = NULL;

        util_unreference_framebuffer_state(&batch->key);
        ralloc_free(batch);
}

static struct panfrost_batch *
panfrost_get_batch(struct panfrost_context *ctx,
                   const struct pipe_framebuffer_state *key)
{
        /* Lookup the job first */
        struct hash_entry *entry = _mesa_hash_table_search(ctx->batches, key);

        if (entry)
                return entry->data;

        /* Otherwise, let's create a job */

        struct panfrost_batch *batch = panfrost_create_batch(ctx, key);

        /* Save the created job */
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
                assert(util_framebuffer_state_equal(&ctx->batch->key,
                                                    &ctx->pipe_framebuffer));
                return ctx->batch;
        }

        /* If not, look up the job */
        struct panfrost_batch *batch = panfrost_get_batch(ctx,
                                                          &ctx->pipe_framebuffer);

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

void panfrost_batch_add_fbo_bos(struct panfrost_batch *batch)
{
        for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
                struct panfrost_resource *rsrc = pan_resource(batch->key.cbufs[i]->texture);
                panfrost_batch_add_bo(batch, rsrc->bo);
        }

        if (batch->key.zsbuf) {
                struct panfrost_resource *rsrc = pan_resource(batch->key.zsbuf->texture);
                panfrost_batch_add_bo(batch, rsrc->bo);
        }
}

struct panfrost_bo *
panfrost_batch_create_bo(struct panfrost_batch *batch, size_t size,
                         uint32_t create_flags)
{
        struct panfrost_bo *bo;

        bo = panfrost_bo_create(pan_screen(batch->ctx->base.screen), size,
                                create_flags);
        panfrost_batch_add_bo(batch, bo);

        /* panfrost_batch_add_bo() has retained a reference and
         * panfrost_bo_create() initialize the refcnt to 1, so let's
         * unreference the BO here so it gets released when the batch is
         * destroyed (unless it's retained by someone else in the meantime).
         */
        panfrost_bo_unreference(bo);
        return bo;
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
                /* Create the BO as invisible, as there's no reason to map */

                batch->polygon_list = panfrost_batch_create_bo(batch, size,
				                               PAN_BO_INVISIBLE);
        }

        return batch->polygon_list->gpu;
}

struct panfrost_bo *
panfrost_batch_get_scratchpad(struct panfrost_batch *batch)
{
        if (batch->scratchpad)
                return batch->scratchpad;

        batch->scratchpad = panfrost_batch_create_bo(batch, 64 * 4 * 4096,
                                                     PAN_BO_INVISIBLE);
        assert(batch->scratchpad);
        return batch->scratchpad;
}

struct panfrost_bo *
panfrost_batch_get_tiler_heap(struct panfrost_batch *batch)
{
        if (batch->tiler_heap)
                return batch->tiler_heap;

        batch->tiler_heap = panfrost_batch_create_bo(batch, 4096 * 4096,
                                                     PAN_BO_INVISIBLE |
                                                     PAN_BO_GROWABLE);
        assert(batch->tiler_heap);
        return batch->tiler_heap;
}

struct panfrost_bo *
panfrost_batch_get_tiler_dummy(struct panfrost_batch *batch)
{
        if (batch->tiler_dummy)
                return batch->tiler_dummy;

        batch->tiler_dummy = panfrost_batch_create_bo(batch, 4096,
                                                      PAN_BO_INVISIBLE);
        assert(batch->tiler_dummy);
        return batch->tiler_dummy;
}

static void
panfrost_batch_draw_wallpaper(struct panfrost_batch *batch)
{
        /* Nothing to reload? TODO: MRT wallpapers */
        if (batch->key.cbufs[0] == NULL)
                return;

        /* Check if the buffer has any content on it worth preserving */

        struct pipe_surface *surf = batch->key.cbufs[0];
        struct panfrost_resource *rsrc = pan_resource(surf->texture);
        unsigned level = surf->u.tex.level;

        if (!rsrc->slices[level].initialized)
                return;

        batch->ctx->wallpaper_batch = batch;

        /* Clamp the rendering area to the damage extent. The
         * KHR_partial_update() spec states that trying to render outside of
         * the damage region is "undefined behavior", so we should be safe.
         */
        unsigned damage_width = (rsrc->damage.extent.maxx - rsrc->damage.extent.minx);
        unsigned damage_height = (rsrc->damage.extent.maxy - rsrc->damage.extent.miny);

        if (damage_width && damage_height) {
                panfrost_batch_intersection_scissor(batch,
                                                    rsrc->damage.extent.minx,
                                                    rsrc->damage.extent.miny,
                                                    rsrc->damage.extent.maxx,
                                                    rsrc->damage.extent.maxy);
        }

        /* FIXME: Looks like aligning on a tile is not enough, but
         * aligning on twice the tile size seems to works. We don't
         * know exactly what happens here but this deserves extra
         * investigation to figure it out.
         */
        batch->minx = batch->minx & ~((MALI_TILE_LENGTH * 2) - 1);
        batch->miny = batch->miny & ~((MALI_TILE_LENGTH * 2) - 1);
        batch->maxx = MIN2(ALIGN_POT(batch->maxx, MALI_TILE_LENGTH * 2),
                           rsrc->base.width0);
        batch->maxy = MIN2(ALIGN_POT(batch->maxy, MALI_TILE_LENGTH * 2),
                           rsrc->base.height0);

        struct pipe_scissor_state damage;
        struct pipe_box rects[4];

        /* Clamp the damage box to the rendering area. */
        damage.minx = MAX2(batch->minx, rsrc->damage.biggest_rect.x);
        damage.miny = MAX2(batch->miny, rsrc->damage.biggest_rect.y);
        damage.maxx = MIN2(batch->maxx,
                           rsrc->damage.biggest_rect.x +
                           rsrc->damage.biggest_rect.width);
        damage.maxy = MIN2(batch->maxy,
                           rsrc->damage.biggest_rect.y +
                           rsrc->damage.biggest_rect.height);

        /* One damage rectangle means we can end up with at most 4 reload
         * regions:
         * 1: left region, only exists if damage.x > 0
         * 2: right region, only exists if damage.x + damage.width < fb->width
         * 3: top region, only exists if damage.y > 0. The intersection with
         *    the left and right regions are dropped
         * 4: bottom region, only exists if damage.y + damage.height < fb->height.
         *    The intersection with the left and right regions are dropped
         *
         *                    ____________________________
         *                    |       |     3     |      |
         *                    |       |___________|      |
         *                    |       |   damage  |      |
         *                    |   1   |    rect   |   2  |
         *                    |       |___________|      |
         *                    |       |     4     |      |
         *                    |_______|___________|______|
         */
        u_box_2d(batch->minx, batch->miny, damage.minx - batch->minx,
                 batch->maxy - batch->miny, &rects[0]);
        u_box_2d(damage.maxx, batch->miny, batch->maxx - damage.maxx,
                 batch->maxy - batch->miny, &rects[1]);
        u_box_2d(damage.minx, batch->miny, damage.maxx - damage.minx,
                 damage.miny - batch->miny, &rects[2]);
        u_box_2d(damage.minx, damage.maxy, damage.maxx - damage.minx,
                 batch->maxy - damage.maxy, &rects[3]);

        for (unsigned i = 0; i < 4; i++) {
                /* Width and height are always >= 0 even if width is declared as a
                 * signed integer: u_box_2d() helper takes unsigned args and
                 * panfrost_set_damage_region() is taking care of clamping
                 * negative values.
                 */
                if (!rects[i].width || !rects[i].height)
                        continue;

                /* Blit the wallpaper in */
                panfrost_blit_wallpaper(batch->ctx, &rects[i]);
        }
        batch->ctx->wallpaper_batch = NULL;
}

static int
panfrost_batch_submit_ioctl(struct panfrost_batch *batch,
                            mali_ptr first_job_desc,
                            uint32_t reqs)
{
        struct panfrost_context *ctx = batch->ctx;
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_screen *screen = pan_screen(gallium->screen);
        struct drm_panfrost_submit submit = {0,};
        uint32_t *bo_handles;
        int ret;

        submit.in_syncs = (u64) (uintptr_t) &ctx->out_sync;
        submit.in_sync_count = 1;

        submit.out_sync = ctx->out_sync;

        submit.jc = first_job_desc;
        submit.requirements = reqs;

        bo_handles = calloc(batch->bos->entries, sizeof(*bo_handles));
        assert(bo_handles);

        set_foreach(batch->bos, entry) {
                struct panfrost_bo *bo = (struct panfrost_bo *)entry->key;
                assert(bo->gem_handle > 0);
                bo_handles[submit.bo_handle_count++] = bo->gem_handle;
        }

        submit.bo_handles = (u64) (uintptr_t) bo_handles;
        ret = drmIoctl(screen->fd, DRM_IOCTL_PANFROST_SUBMIT, &submit);
        free(bo_handles);
        if (ret) {
                fprintf(stderr, "Error submitting: %m\n");
                return errno;
        }

        /* Trace the job if we're doing that */
        if (pan_debug & PAN_DBG_TRACE) {
                /* Wait so we can get errors reported back */
                drmSyncobjWait(screen->fd, &ctx->out_sync, 1, INT64_MAX, 0, NULL);
                pandecode_jc(submit.jc, FALSE);
        }

        return 0;
}

static int
panfrost_batch_submit_jobs(struct panfrost_batch *batch)
{
        bool has_draws = batch->first_job.gpu;
        int ret = 0;

        if (has_draws) {
                ret = panfrost_batch_submit_ioctl(batch, batch->first_job.gpu, 0);
                assert(!ret);
        }

        if (batch->first_tiler.gpu || batch->clear) {
                mali_ptr fragjob = panfrost_fragment_job(batch, has_draws);

                ret = panfrost_batch_submit_ioctl(batch, fragjob, PANFROST_JD_REQ_FS);
                assert(!ret);
        }

        return ret;
}

void
panfrost_batch_submit(struct panfrost_batch *batch)
{
        assert(batch);

        struct panfrost_context *ctx = batch->ctx;
        int ret;

        /* Nothing to do! */
        if (!batch->last_job.gpu && !batch->clear)
                goto out;

        if (!batch->clear && batch->last_tiler.gpu)
                panfrost_batch_draw_wallpaper(batch);

        panfrost_scoreboard_link_batch(batch);

        ret = panfrost_batch_submit_jobs(batch);

        if (ret)
                fprintf(stderr, "panfrost_batch_submit failed: %d\n", ret);

out:
        /* If this is the bound batch, the panfrost_context parameters are
         * relevant so submitting it invalidates those paramaters, but if it's
         * not bound, the context parameters are for some other batch so we
         * can't invalidate them.
         */
        if (ctx->batch == batch)
                panfrost_invalidate_frame(ctx);

        /* The job has been submitted, let's invalidate the current FBO job
         * cache.
	 */
        assert(!ctx->batch || batch == ctx->batch);
        ctx->batch = NULL;

        /* We always stall the pipeline for correct results since pipelined
         * rendering is quite broken right now (to be fixed by the panfrost_job
         * refactor, just take the perf hit for correctness)
         */
        drmSyncobjWait(pan_screen(ctx->base.screen)->fd, &ctx->out_sync, 1,
                       INT64_MAX, 0, NULL);
        panfrost_free_batch(batch);
}

void
panfrost_batch_set_requirements(struct panfrost_batch *batch)
{
        struct panfrost_context *ctx = batch->ctx;

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
panfrost_batch_clear(struct panfrost_batch *batch,
                     unsigned buffers,
                     const union pipe_color_union *color,
                     double depth, unsigned stencil)
{
        struct panfrost_context *ctx = batch->ctx;

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

static bool
panfrost_batch_compare(const void *a, const void *b)
{
        return util_framebuffer_state_equal(a, b);
}

static uint32_t
panfrost_batch_hash(const void *key)
{
        return _mesa_hash_data(key, sizeof(struct pipe_framebuffer_state));
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

/* Are we currently rendering to the screen (rather than an FBO)? */

bool
panfrost_batch_is_scanout(struct panfrost_batch *batch)
{
        /* If there is no color buffer, it's an FBO */
        if (batch->key.nr_cbufs != 1)
                return false;

        /* If we're too early that no framebuffer was sent, it's scanout */
        if (!batch->key.cbufs[0])
                return true;

        return batch->key.cbufs[0]->texture->bind & PIPE_BIND_DISPLAY_TARGET ||
               batch->key.cbufs[0]->texture->bind & PIPE_BIND_SCANOUT ||
               batch->key.cbufs[0]->texture->bind & PIPE_BIND_SHARED;
}

void
panfrost_batch_init(struct panfrost_context *ctx)
{
        ctx->batches = _mesa_hash_table_create(ctx,
                                               panfrost_batch_hash,
                                               panfrost_batch_compare);
}

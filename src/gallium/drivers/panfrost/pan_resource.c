/**************************************************************************
 *
 * Copyright 2008 VMware, Inc.
 * Copyright 2014 Broadcom
 * Copyright 2018 Alyssa Rosenzweig
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

#include <xf86drm.h>
#include <fcntl.h>
#include "drm-uapi/drm_fourcc.h"

#include "state_tracker/winsys_handle.h"
#include "util/u_format.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "util/u_transfer_helper.h"

#include "pan_context.h"
#include "pan_screen.h"
#include "pan_resource.h"
#include "pan_swizzle.h"
#include "pan_util.h"

static struct pipe_resource *
panfrost_resource_from_handle(struct pipe_screen *pscreen,
                              const struct pipe_resource *templat,
                              struct winsys_handle *whandle,
                              unsigned usage)
{
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_resource *rsc;
        struct pipe_resource *prsc;

        assert(whandle->type == WINSYS_HANDLE_TYPE_FD);

        rsc = CALLOC_STRUCT(panfrost_resource);
        if (!rsc)
                return NULL;

        prsc = &rsc->base;

        *prsc = *templat;

        pipe_reference_init(&prsc->reference, 1);
        prsc->screen = pscreen;

	rsc->bo = screen->driver->import_bo(screen, whandle);
	rsc->bo->slices[0].stride = whandle->stride;

	if (screen->ro) {
		rsc->scanout =
			renderonly_create_gpu_import_for_resource(prsc, screen->ro, NULL);
		/* failure is expected in some cases.. */
	}

        return prsc;
}

static boolean
panfrost_resource_get_handle(struct pipe_screen *pscreen,
                             struct pipe_context *ctx,
                             struct pipe_resource *pt,
                             struct winsys_handle *handle,
                             unsigned usage)
{
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_resource *rsrc = (struct panfrost_resource *) pt;
        struct renderonly_scanout *scanout = rsrc->scanout;

        handle->modifier = DRM_FORMAT_MOD_INVALID;

	if (handle->type == WINSYS_HANDLE_TYPE_SHARED) {
		return FALSE;
	} else if (handle->type == WINSYS_HANDLE_TYPE_KMS) {
		if (renderonly_get_handle(scanout, handle))
			return TRUE;

		handle->handle = rsrc->bo->gem_handle;
		handle->stride = rsrc->bo->slices[0].stride;
		return TRUE;
	} else if (handle->type == WINSYS_HANDLE_TYPE_FD) {
                if (scanout) {
                        struct drm_prime_handle args = {
                                .handle = scanout->handle,
                                .flags = DRM_CLOEXEC,
                        };

                        int ret = drmIoctl(screen->ro->kms_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
                        if (ret == -1)
                                return FALSE;

                        handle->stride = scanout->stride;
                        handle->handle = args.fd;

                        return TRUE;
                } else
			return screen->driver->export_bo(screen, rsrc->bo->gem_handle, rsrc->bo->slices[0].stride, handle);
	}

	return FALSE;
}

static void
panfrost_flush_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
        //DBG("TODO %s\n", __func__);
}

static void
panfrost_blit(struct pipe_context *pipe,
              const struct pipe_blit_info *info)
{
        if (util_try_blit_via_copy_region(pipe, info))
                return;

        /* TODO */
        DBG("Unhandled blit.\n");

        return;
}

static struct pipe_surface *
panfrost_create_surface(struct pipe_context *pipe,
                        struct pipe_resource *pt,
                        const struct pipe_surface *surf_tmpl)
{
        struct pipe_surface *ps = NULL;

        ps = CALLOC_STRUCT(pipe_surface);

        if (ps) {
                pipe_reference_init(&ps->reference, 1);
                pipe_resource_reference(&ps->texture, pt);
                ps->context = pipe;
                ps->format = surf_tmpl->format;

                if (pt->target != PIPE_BUFFER) {
                        assert(surf_tmpl->u.tex.level <= pt->last_level);
                        ps->width = u_minify(pt->width0, surf_tmpl->u.tex.level);
                        ps->height = u_minify(pt->height0, surf_tmpl->u.tex.level);
                        ps->u.tex.level = surf_tmpl->u.tex.level;
                        ps->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
                        ps->u.tex.last_layer = surf_tmpl->u.tex.last_layer;
                } else {
                        /* setting width as number of elements should get us correct renderbuffer width */
                        ps->width = surf_tmpl->u.buf.last_element - surf_tmpl->u.buf.first_element + 1;
                        ps->height = pt->height0;
                        ps->u.buf.first_element = surf_tmpl->u.buf.first_element;
                        ps->u.buf.last_element = surf_tmpl->u.buf.last_element;
                        assert(ps->u.buf.first_element <= ps->u.buf.last_element);
                        assert(ps->u.buf.last_element < ps->width);
                }
        }

        return ps;
}

static void
panfrost_surface_destroy(struct pipe_context *pipe,
                         struct pipe_surface *surf)
{
        assert(surf->texture);
        pipe_resource_reference(&surf->texture, NULL);
        free(surf);
}

static void
panfrost_setup_slices(const struct pipe_resource *tmpl, struct panfrost_bo *bo)
{
        unsigned width = tmpl->width0;
        unsigned height = tmpl->height0;
        unsigned bytes_per_pixel = util_format_get_blocksize(tmpl->format);

        unsigned offset = 0;

        for (unsigned l = 0; l <= tmpl->last_level; ++l) {
                struct panfrost_slice *slice = &bo->slices[l];

                unsigned effective_width = width;
                unsigned effective_height = height;

                /* Tiled operates blockwise; linear is packed */

                if (bo->layout == PAN_TILED) {
                        effective_width = ALIGN(effective_width, 16);
                        effective_height = ALIGN(effective_height, 16);
                }

                slice->offset = offset;
                slice->stride = bytes_per_pixel * effective_width;

                offset += slice->stride * effective_height;

                width = u_minify(width, 1);
                height = u_minify(height, 1);
        }

        bo->size = ALIGN(offset, 4096);
}

static struct panfrost_bo *
panfrost_create_bo(struct panfrost_screen *screen, const struct pipe_resource *template)
{
	struct panfrost_bo *bo = CALLOC_STRUCT(panfrost_bo);

        /* Based on the usage, figure out what storing will be used. There are
         * various tradeoffs:
         *
         * Linear: the basic format, bad for memory bandwidth, bad for cache
         * use. Zero-copy, though. Renderable.
         *
         * Tiled: Not compressed, but cache-optimized. Expensive to write into
         * (due to software tiling), but cheap to sample from. Ideal for most
         * textures. 
         *
         * AFBC: Compressed and renderable (so always desirable for non-scanout
         * rendertargets). Cheap to sample from. The format is black box, so we
         * can't read/write from software.
         */

        /* Tiling textures is almost always faster, unless we only use it once */
        bool should_tile = (template->usage != PIPE_USAGE_STREAM) && (template->bind & PIPE_BIND_SAMPLER_VIEW);

        /* For unclear reasons, depth/stencil is faster linear than AFBC, so
         * make sure it's linear */

        if (template->bind & PIPE_BIND_DEPTH_STENCIL)
                should_tile = false;

        /* Set the layout appropriately */
        bo->layout = should_tile ? PAN_TILED : PAN_LINEAR;

        panfrost_setup_slices(template, bo);

        if (bo->layout == PAN_TILED || bo->layout == PAN_LINEAR) {
                struct panfrost_memory mem;

                screen->driver->allocate_slab(screen, &mem, bo->size / 4096, true, 0, 0, 0);

                bo->cpu = mem.cpu;
                bo->gpu = mem.gpu;
                bo->gem_handle = mem.gem_handle;
        }

        return bo;
}

static struct pipe_resource *
panfrost_resource_create(struct pipe_screen *screen,
                         const struct pipe_resource *template)
{
        struct panfrost_resource *so = CALLOC_STRUCT(panfrost_resource);
        struct panfrost_screen *pscreen = (struct panfrost_screen *) screen;

        so->base = *template;
        so->base.screen = screen;

        pipe_reference_init(&so->base.reference, 1);

        /* Make sure we're familiar */
        switch (template->target) {
                case PIPE_BUFFER:
                case PIPE_TEXTURE_1D:
                case PIPE_TEXTURE_2D:
                case PIPE_TEXTURE_3D:
                case PIPE_TEXTURE_RECT:
                        break;
                default:
                        DBG("Unknown texture target %d\n", template->target);
                        assert(0);
        }

        if (template->bind & PIPE_BIND_DISPLAY_TARGET ||
            template->bind & PIPE_BIND_SCANOUT ||
            template->bind & PIPE_BIND_SHARED) {
                struct pipe_resource scanout_templat = *template;
                struct renderonly_scanout *scanout;
                struct winsys_handle handle;

                scanout = renderonly_scanout_for_resource(&scanout_templat,
                                                          pscreen->ro, &handle);
                if (!scanout)
                        return NULL;

                assert(handle.type == WINSYS_HANDLE_TYPE_FD);
                /* TODO: handle modifiers? */
                so = pan_resource(screen->resource_from_handle(screen, template,
                                                                 &handle,
                                                                 PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE));
                close(handle.handle);
                if (!so)
                        return NULL;

                so->scanout = scanout;
                pscreen->display_target = so;
        } else {
                so->bo = panfrost_create_bo(pscreen, template);
        }

        return (struct pipe_resource *)so;
}

static void
panfrost_destroy_bo(struct panfrost_screen *screen, struct panfrost_bo *pbo)
{
	struct panfrost_bo *bo = (struct panfrost_bo *)pbo;

        if ((bo->layout == PAN_LINEAR || bo->layout == PAN_TILED) &&
                        !bo->imported) {
                struct panfrost_memory mem = {
                        .cpu = bo->cpu,
                        .gpu = bo->gpu,
                        .size = bo->size,
                        .gem_handle = bo->gem_handle,
                };

                screen->driver->free_slab(screen, &mem);
        }

        if (bo->layout == PAN_AFBC) {
                /* TODO */
                DBG("--leaking afbc (%d bytes)--\n", bo->afbc_metadata_size);
        }

        if (bo->has_checksum) {
                /* TODO */
                DBG("--leaking checksum (%zd bytes)--\n", bo->checksum_slab.size);
        }

        if (bo->imported) {
                screen->driver->free_imported_bo(screen, bo);
        }
}

static void
panfrost_resource_destroy(struct pipe_screen *screen,
                          struct pipe_resource *pt)
{
        struct panfrost_screen *pscreen = pan_screen(screen);
        struct panfrost_resource *rsrc = (struct panfrost_resource *) pt;

	if (rsrc->scanout)
		renderonly_scanout_destroy(rsrc->scanout, pscreen->ro);

	if (rsrc->bo)
		panfrost_destroy_bo(pscreen, rsrc->bo);

	FREE(rsrc);
}

static void *
panfrost_transfer_map(struct pipe_context *pctx,
                      struct pipe_resource *resource,
                      unsigned level,
                      unsigned usage,  /* a combination of PIPE_TRANSFER_x */
                      const struct pipe_box *box,
                      struct pipe_transfer **out_transfer)
{
        int bytes_per_pixel = util_format_get_blocksize(resource->format);
        struct panfrost_bo *bo = pan_resource(resource)->bo;

        struct panfrost_gtransfer *transfer = CALLOC_STRUCT(panfrost_gtransfer);
        transfer->base.level = level;
        transfer->base.usage = usage;
        transfer->base.box = *box;
        transfer->base.stride = bo->slices[level].stride;
        transfer->base.layer_stride = bytes_per_pixel * resource->width0; /* TODO: Cubemaps */
        assert(!transfer->base.box.z);

        pipe_resource_reference(&transfer->base.resource, resource);

        *out_transfer = &transfer->base;

        if (resource->bind & PIPE_BIND_DISPLAY_TARGET ||
            resource->bind & PIPE_BIND_SCANOUT ||
            resource->bind & PIPE_BIND_SHARED) {
                /* Mipmapped readpixels?! */
                assert(level == 0);

                /* Force a flush -- kill the pipeline */
                panfrost_flush(pctx, NULL, PIPE_FLUSH_END_OF_FRAME);
        }

        if (bo->layout != PAN_LINEAR) {
                /* Non-linear resources need to be indirectly mapped */

                if (usage & PIPE_TRANSFER_MAP_DIRECTLY)
                        return NULL;

                /* TODO: Reads */
                transfer->map = malloc(ALIGN(box->width, 16) * ALIGN(box->height, 16) * bytes_per_pixel);

                return transfer->map;
        } else {
                return bo->cpu
                        + bo->slices[level].offset
                        + transfer->base.box.y * bo->slices[level].stride
                        + transfer->base.box.x * bytes_per_pixel;
        }
}

static void
panfrost_tile_texture(struct panfrost_screen *screen, struct panfrost_resource *rsrc, struct panfrost_gtransfer *trans)
{
	struct panfrost_bo *bo = (struct panfrost_bo *)rsrc->bo;

        unsigned level = trans->base.level;

        assert(!trans->base.box.z);

        panfrost_texture_swizzle(
                        trans->base.box.x,
                        trans->base.box.y,
                        trans->base.box.width,
                        trans->base.box.height,
                        util_format_get_blocksize(rsrc->base.format),
                        bo->slices[level].stride,
                        trans->map,
                        bo->cpu + bo->slices[level].offset);
}

static void
panfrost_unmap_bo(struct panfrost_context *ctx,
                         struct pipe_transfer *transfer)
{
        struct panfrost_gtransfer *trans = pan_transfer(transfer);
	struct panfrost_bo *bo = (struct panfrost_bo *)pan_resource(transfer->resource)->bo;

        if (transfer->usage & PIPE_TRANSFER_WRITE) {
                if (transfer->resource->target == PIPE_TEXTURE_2D) {
                        struct panfrost_resource *prsrc = (struct panfrost_resource *) transfer->resource;

                        /* Gallium thinks writeback happens here; instead, this is our cue to tile */
                        if (bo->layout == PAN_AFBC) {
                                DBG("Warning: writes to afbc surface can't possibly work out well for you...\n");
                        } else if (bo->layout == PAN_TILED) {
                                struct pipe_context *gallium = (struct pipe_context *) ctx;
                                struct panfrost_screen *screen = pan_screen(gallium->screen);
                                panfrost_tile_texture(screen, prsrc, trans);
                        }
                }
        }

        free(trans->map);
}

static void
panfrost_transfer_unmap(struct pipe_context *pctx,
                        struct pipe_transfer *transfer)
{
        struct panfrost_context *ctx = pan_context(pctx);

	panfrost_unmap_bo(ctx, transfer);

        /* Derefence the resource */
        pipe_resource_reference(&transfer->resource, NULL);

        /* Transfer itself is CALLOCed at the moment */
        free(transfer);
}

static struct pb_slab *
panfrost_slab_alloc(void *priv, unsigned heap, unsigned entry_size, unsigned group_index)
{
        struct panfrost_screen *screen = (struct panfrost_screen *) priv;
        struct panfrost_memory *mem = CALLOC_STRUCT(panfrost_memory);

        size_t slab_size = (1 << (MAX_SLAB_ENTRY_SIZE + 1));

        mem->slab.num_entries = slab_size / entry_size;
        mem->slab.num_free = mem->slab.num_entries;

        LIST_INITHEAD(&mem->slab.free);
        for (unsigned i = 0; i < mem->slab.num_entries; ++i) {
                /* Create a slab entry */
                struct panfrost_memory_entry *entry = CALLOC_STRUCT(panfrost_memory_entry);
                entry->offset = entry_size * i;

                entry->base.slab = &mem->slab;
                entry->base.group_index = group_index;

                LIST_ADDTAIL(&entry->base.head, &mem->slab.free);
        }

        /* Actually allocate the memory from kernel-space. Mapped, same_va, no
         * special flags */

        screen->driver->allocate_slab(screen, mem, slab_size / 4096, true, 0, 0, 0);

        return &mem->slab;
}

static bool
panfrost_slab_can_reclaim(void *priv, struct pb_slab_entry *entry)
{
        struct panfrost_memory_entry *p_entry = (struct panfrost_memory_entry *) entry;
        return p_entry->freed;
}

static void
panfrost_slab_free(void *priv, struct pb_slab *slab)
{
        struct panfrost_memory *mem = (struct panfrost_memory *) slab;
        struct panfrost_screen *screen = (struct panfrost_screen *) priv;

        screen->driver->free_slab(screen, mem);
}

static void
panfrost_invalidate_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
        //DBG("TODO %s\n", __func__);
}

static enum pipe_format
panfrost_resource_get_internal_format(struct pipe_resource *prsrc)
{
        return prsrc->format;
}

static void
panfrost_resource_set_stencil(struct pipe_resource *prsrc,
                              struct pipe_resource *stencil)
{
        pan_resource(prsrc)->separate_stencil = pan_resource(stencil);
}

static struct pipe_resource *
panfrost_resource_get_stencil(struct pipe_resource *prsrc)
{
        return &pan_resource(prsrc)->separate_stencil->base;
}

static const struct u_transfer_vtbl transfer_vtbl = {
        .resource_create          = panfrost_resource_create,
        .resource_destroy         = panfrost_resource_destroy,
        .transfer_map             = panfrost_transfer_map,
        .transfer_unmap           = panfrost_transfer_unmap,
        .transfer_flush_region    = u_default_transfer_flush_region,
        .get_internal_format      = panfrost_resource_get_internal_format,
        .set_stencil              = panfrost_resource_set_stencil,
        .get_stencil              = panfrost_resource_get_stencil,
};

void
panfrost_resource_screen_init(struct panfrost_screen *pscreen)
{
        //pscreen->base.resource_create_with_modifiers =
        //        panfrost_resource_create_with_modifiers;
        pscreen->base.resource_create = u_transfer_helper_resource_create;
        pscreen->base.resource_destroy = u_transfer_helper_resource_destroy;
        pscreen->base.resource_from_handle = panfrost_resource_from_handle;
        pscreen->base.resource_get_handle = panfrost_resource_get_handle;
        pscreen->base.transfer_helper = u_transfer_helper_create(&transfer_vtbl,
                                                            true, false,
                                                            true, true);

        pb_slabs_init(&pscreen->slabs,
                        MIN_SLAB_ENTRY_SIZE,
                        MAX_SLAB_ENTRY_SIZE,

                        3, /* Number of heaps */

                        pscreen,

                        panfrost_slab_can_reclaim,
                        panfrost_slab_alloc,
                        panfrost_slab_free);
}

void
panfrost_resource_context_init(struct pipe_context *pctx)
{
        pctx->transfer_map = u_transfer_helper_transfer_map;
        pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;
        pctx->transfer_unmap = u_transfer_helper_transfer_unmap;
        pctx->buffer_subdata = u_default_buffer_subdata;
        pctx->create_surface = panfrost_create_surface;
        pctx->surface_destroy = panfrost_surface_destroy;
        pctx->resource_copy_region = util_resource_copy_region;
        pctx->blit = panfrost_blit;
        pctx->flush_resource = panfrost_flush_resource;
        pctx->invalidate_resource = panfrost_invalidate_resource;
        pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;
        pctx->buffer_subdata = u_default_buffer_subdata;
        pctx->texture_subdata = u_default_texture_subdata;
}

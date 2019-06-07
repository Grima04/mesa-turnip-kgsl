/*
 * Copyright (C) 2019 Collabora
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
 * Authors:
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#include "pan_resource.h"
#include "util/u_format.h"

/* Arm FrameBuffer Compression (AFBC) is a lossless compression scheme natively
 * implemented in Mali GPUs (as well as many display controllers paired with
 * Mali GPUs, etc). Where possible, Panfrost prefers to use AFBC for both
 * rendering and texturing. In most cases, this is a performance-win due to a
 * dramatic reduction in memory bandwidth and cache locality compared to a
 * linear resources.
 *
 * AFBC divides the framebuffer into 16x16 tiles (other sizes possible, TODO:
 * do we need to support this?). So, the width and height each must be aligned
 * up to 16 pixels. This is inherently good for performance; note that for a 4
 * byte-per-pixel format like RGBA8888, that means that rows are 16*4=64 byte
 * aligned, which is the cache-line size.
 *
 * For each AFBC-compressed resource, there is a single contiguous
 * (CPU/GPU-shared) buffer. This buffer itself is divided into two parts:
 * header and body, placed immediately after each other.
 *
 * The AFBC header contains 16 bytes of metadata per tile.
 *
 * The AFBC body is the same size as the original linear resource (padded to
 * the nearest tile). Although the body comes immediately after the header, it
 * must also be cache-line aligned, so there can sometimes be a bit of padding
 * between the header and body.
 *
 * As an example, a 64x64 RGBA framebuffer contains 64/16 = 4 tiles horizontally and
 * 4 tiles vertically. There are 4*4=16 tiles in total, each containing 16
 * bytes of metadata, so there is a 16*16=256 byte header. 64x64 is already
 * tile aligned, so the body is 64*64 * 4 bytes per pixel = 16384 bytes of
 * body.
 *
 * From userspace, Panfrost needs to be able to calculate these sizes. It
 * explicitly does not and can not know the format of the data contained within
 * this header and body. The GPU has native support for AFBC encode/decode. For
 * an internal FBO or a framebuffer used for scanout with an AFBC-compatible
 * winsys/display-controller, the buffer is maintained AFBC throughout flight,
 * and the driver never needs to know the internal data. For edge cases where
 * the driver really does need to read/write from the AFBC resource, we
 * generate a linear staging buffer and use the GPU to blit AFBC<--->linear.
 * TODO: Implement me. */

#define AFBC_TILE_WIDTH 16
#define AFBC_TILE_HEIGHT 16
#define AFBC_HEADER_BYTES_PER_TILE 16
#define AFBC_CACHE_ALIGN 64

/* Is it possible to AFBC compress a particular format? Common formats (and
 * YUV) are compressible. Some obscure formats are not and fallback on linear,
 * at a performance hit. Also, if you need to disable AFBC entirely in the
 * driver for debug/profiling, just always return false here. */

bool
panfrost_format_supports_afbc(enum pipe_format format)
{
        const struct util_format_description *desc =
                util_format_description(format);

        if (util_format_is_rgba8_variant(desc))
                return true;

        /* TODO: AFBC of other formats */
        /* TODO: AFBC of ZS */

        return false;
}

/* AFBC is enabled on a per-resource basis (AFBC enabling is theoretically
 * indepdent between color buffers and depth/stencil). To enable, we allocate
 * the AFBC metadata buffer and mark that it is enabled. We do -not- actually
 * edit the fragment job here. This routine should be called ONCE per
 * AFBC-compressed buffer, rather than on every frame. */

void
panfrost_enable_afbc(struct panfrost_context *ctx, struct panfrost_resource *rsrc, bool ds)
{
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_screen *screen = pan_screen(gallium->screen);

        unsigned width  = rsrc->base.width0;
        unsigned height = rsrc->base.height0;
        unsigned bytes_per_pixel = util_format_get_blocksize(rsrc->base.format);

        /* Align to tile */
        unsigned aligned_width  = ALIGN(width,  AFBC_TILE_WIDTH);
        unsigned aligned_height = ALIGN(height, AFBC_TILE_HEIGHT);

        /* Compute size in tiles, rather than pixels */
        unsigned tile_count_x = aligned_width  / AFBC_TILE_WIDTH;
        unsigned tile_count_y = aligned_height / AFBC_TILE_HEIGHT;
        unsigned tile_count = tile_count_x * tile_count_y;

        unsigned header_bytes = tile_count * AFBC_HEADER_BYTES_PER_TILE;
        unsigned header_size = ALIGN(header_bytes, AFBC_CACHE_ALIGN);

        /* The stride is a normal stride, but aligned */
        unsigned unaligned_stride = aligned_width * bytes_per_pixel;
        unsigned stride = ALIGN(unaligned_stride, AFBC_CACHE_ALIGN);

        /* Compute the entire buffer size */
        unsigned body_size = stride * aligned_height;
        unsigned buffer_size = header_size + body_size;

        /* Allocate the AFBC slab itself, large enough to hold the above */
        screen->driver->allocate_slab(screen, &rsrc->bo->afbc_slab,
                               ALIGN(buffer_size, 4096) / 4096,
                               true, 0, 0, 0);

        /* Compressed textured reads use a tagged pointer to the metadata */
        rsrc->bo->layout = PAN_AFBC;
        rsrc->bo->gpu = rsrc->bo->afbc_slab.gpu | (ds ? 0 : 1);
        rsrc->bo->cpu = rsrc->bo->afbc_slab.cpu;
        rsrc->bo->gem_handle = rsrc->bo->afbc_slab.gem_handle;
        rsrc->bo->afbc_metadata_size = header_size;
}

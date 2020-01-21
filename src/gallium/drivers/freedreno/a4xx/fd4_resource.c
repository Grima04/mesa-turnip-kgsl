/*
 * Copyright (C) 2012 Rob Clark <robclark@freedesktop.org>
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
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "fd4_resource.h"

static uint32_t
setup_slices(struct fd_resource *rsc, uint32_t alignment, enum pipe_format format)
{
	struct pipe_resource *prsc = &rsc->base;
	struct fd_screen *screen = fd_screen(prsc->screen);
	enum util_format_layout layout = util_format_description(format)->layout;
	uint32_t pitchalign = screen->gmem_alignw;
	uint32_t level, size = 0;
	uint32_t width = prsc->width0;
	uint32_t height = prsc->height0;
	uint32_t depth = prsc->depth0;
	/* in layer_first layout, the level (slice) contains just one
	 * layer (since in fact the layer contains the slices)
	 */
	uint32_t layers_in_level = rsc->layout.layer_first ? 1 : prsc->array_size;

	for (level = 0; level <= prsc->last_level; level++) {
		struct fdl_slice *slice = fd_resource_slice(rsc, level);
		uint32_t blocks;

		if (layout == UTIL_FORMAT_LAYOUT_ASTC)
			slice->pitch = width =
				util_align_npot(width, pitchalign * util_format_get_blockwidth(format));
		else
			slice->pitch = width = align(width, pitchalign);
		slice->offset = size;
		blocks = util_format_get_nblocks(format, width, height);
		/* 1d array and 2d array textures must all have the same layer size
		 * for each miplevel on a3xx. 3d textures can have different layer
		 * sizes for high levels, but the hw auto-sizer is buggy (or at least
		 * different than what this code does), so as soon as the layer size
		 * range gets into range, we stop reducing it.
		 */
		if (prsc->target == PIPE_TEXTURE_3D && (
					level == 1 ||
					(level > 1 && fd_resource_slice(rsc, level - 1)->size0 > 0xf000)))
			slice->size0 = align(blocks * rsc->layout.cpp, alignment);
		else if (level == 0 || rsc->layout.layer_first || alignment == 1)
			slice->size0 = align(blocks * rsc->layout.cpp, alignment);
		else
			slice->size0 = fd_resource_slice(rsc, level - 1)->size0;

		size += slice->size0 * depth * layers_in_level;

		width = u_minify(width, 1);
		height = u_minify(height, 1);
		depth = u_minify(depth, 1);
	}

	return size;
}

static uint32_t
slice_alignment(enum pipe_texture_target target)
{
	/* on a3xx, 2d array and 3d textures seem to want their
	 * layers aligned to page boundaries:
	 */
	switch (target) {
	case PIPE_TEXTURE_3D:
	case PIPE_TEXTURE_1D_ARRAY:
	case PIPE_TEXTURE_2D_ARRAY:
		return 4096;
	default:
		return 1;
	}
}

/* cross generation texture layout to plug in to screen->setup_slices()..
 * replace with generation specific one as-needed.
 *
 * TODO for a4xx probably can extract out the a4xx specific logic int
 * a small fd4_setup_slices() wrapper that sets up layer_first, and then
 * calls this.
 */
uint32_t
fd4_setup_slices(struct fd_resource *rsc)
{
	uint32_t alignment;

	alignment = slice_alignment(rsc->base.target);

	struct fd_screen *screen = fd_screen(rsc->base.screen);
	if (is_a4xx(screen)) {
		switch (rsc->base.target) {
		case PIPE_TEXTURE_3D:
			rsc->layout.layer_first = false;
			break;
		default:
			rsc->layout.layer_first = true;
			alignment = 1;
			break;
		}
	}

	return setup_slices(rsc, alignment, rsc->base.format);
}

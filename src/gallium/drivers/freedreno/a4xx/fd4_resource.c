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
 *    Jonathan Marek <jonathan@marek.ca>
 */

#include "fd4_resource.h"

uint32_t
fd4_setup_slices(struct fd_resource *rsc)
{
	struct pipe_resource *prsc = &rsc->base;
	enum pipe_format format = prsc->format;
	enum util_format_layout layout = util_format_description(format)->layout;
	uint32_t level, size = 0;
	uint32_t width = prsc->width0;
	uint32_t height = prsc->height0;
	uint32_t depth = prsc->depth0;
	/* in layer_first layout, the level (slice) contains just one
	 * layer (since in fact the layer contains the slices)
	 */
	uint32_t layers_in_level, alignment;

	if (prsc->target == PIPE_TEXTURE_3D) {
		rsc->layout.layer_first = false;
		layers_in_level = prsc->array_size;
		alignment = 4096;
	} else {
		rsc->layout.layer_first = true;
		layers_in_level = 1;
		alignment = 1;
	}

	for (level = 0; level <= prsc->last_level; level++) {
		struct fdl_slice *slice = fd_resource_slice(rsc, level);
		uint32_t blocks;

		if (layout == UTIL_FORMAT_LAYOUT_ASTC)
			width = util_align_npot(width, 32 * util_format_get_blockwidth(format));
		else
			width = align(width, 32);
		slice->pitch = util_format_get_nblocksx(format, width) * rsc->layout.cpp;
		slice->offset = size;
		blocks = util_format_get_nblocks(format, width, height);
		/* 3d textures can have different layer sizes for high levels, but the
		 * hw auto-sizer is buggy (or at least different than what this code
		 * does), so as soon as the layer size range gets into range, we stop
		 * reducing it.
		 */
		if (prsc->target == PIPE_TEXTURE_3D &&
			(level > 1 && fd_resource_slice(rsc, level - 1)->size0 <= 0xf000))
			slice->size0 = fd_resource_slice(rsc, level - 1)->size0;
		else
			slice->size0 = align(blocks * rsc->layout.cpp, alignment);

		size += slice->size0 * depth * layers_in_level;

		width = u_minify(width, 1);
		height = u_minify(height, 1);
		depth = u_minify(depth, 1);
	}

	return size;
}

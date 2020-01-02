/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 * Copyright 2018 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "sid.h"
#include "si_pipe.h"

#include "util/format/u_format.h"

static void si_dma_copy_buffer(struct si_context *ctx,
				struct pipe_resource *dst,
				struct pipe_resource *src,
				uint64_t dst_offset,
				uint64_t src_offset,
				uint64_t size)
{
	struct radeon_cmdbuf *cs = ctx->sdma_cs;
	unsigned i, ncopy, count, max_size, sub_cmd, shift;
	struct si_resource *sdst = si_resource(dst);
	struct si_resource *ssrc = si_resource(src);

	/* Mark the buffer range of destination as valid (initialized),
	 * so that transfer_map knows it should wait for the GPU when mapping
	 * that range. */
	util_range_add(dst, &sdst->valid_buffer_range, dst_offset,
		       dst_offset + size);

	dst_offset += sdst->gpu_address;
	src_offset += ssrc->gpu_address;

	/* see whether we should use the dword-aligned or byte-aligned copy */
	if (!(dst_offset % 4) && !(src_offset % 4) && !(size % 4)) {
		sub_cmd = SI_DMA_COPY_DWORD_ALIGNED;
		shift = 2;
		max_size = SI_DMA_COPY_MAX_DWORD_ALIGNED_SIZE;
	} else {
		sub_cmd = SI_DMA_COPY_BYTE_ALIGNED;
		shift = 0;
		max_size = SI_DMA_COPY_MAX_BYTE_ALIGNED_SIZE;
	}

	ncopy = DIV_ROUND_UP(size, max_size);
	si_need_dma_space(ctx, ncopy * 5, sdst, ssrc);

	for (i = 0; i < ncopy; i++) {
		count = MIN2(size, max_size);
		radeon_emit(cs, SI_DMA_PACKET(SI_DMA_PACKET_COPY, sub_cmd,
					      count >> shift));
		radeon_emit(cs, dst_offset);
		radeon_emit(cs, src_offset);
		radeon_emit(cs, (dst_offset >> 32UL) & 0xff);
		radeon_emit(cs, (src_offset >> 32UL) & 0xff);
		dst_offset += count;
		src_offset += count;
		size -= count;
	}
}

static void si_dma_copy(struct pipe_context *ctx,
			struct pipe_resource *dst,
			unsigned dst_level,
			unsigned dstx, unsigned dsty, unsigned dstz,
			struct pipe_resource *src,
			unsigned src_level,
			const struct pipe_box *src_box)
{
	struct si_context *sctx = (struct si_context *)ctx;

	if (sctx->sdma_cs == NULL ||
	    src->flags & PIPE_RESOURCE_FLAG_SPARSE ||
	    dst->flags & PIPE_RESOURCE_FLAG_SPARSE) {
		goto fallback;
	}

	if (dst->target == PIPE_BUFFER && src->target == PIPE_BUFFER) {
		si_dma_copy_buffer(sctx, dst, src, dstx, src_box->x, src_box->width);
		return;
	}

	/* SI SDMA image copies are unimplemented. */
fallback:
	si_resource_copy_region(ctx, dst, dst_level, dstx, dsty, dstz,
				src, src_level, src_box);
}

void si_init_dma_functions(struct si_context *sctx)
{
	sctx->dma_copy = si_dma_copy;
}

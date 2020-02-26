/*
 * Copyright © 2020 Google LLC
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "freedreno_layout.h"
#include "adreno_common.xml.h"
#include "a6xx.xml.h"

#include <stdio.h>

struct testcase {
	enum pipe_format format;

	int array_size; /* Size for array textures, or 0 otherwise. */
	bool is_3d;

    /* Partially filled layout of input parameters and expected results. */
	struct fdl_layout layout;
};

static const struct testcase testcases[] = {
	/* A straightforward first testcase, linear, with an obvious format. */
	{
		.format = PIPE_FORMAT_R8G8B8A8_UNORM,
		.layout = {
			.width0 = 32, .height0 = 32,
			.slices = {
				{ .offset = 0, .pitch = 256 },
				{ .offset = 8192, .pitch = 256 },
				{ .offset = 12288, .pitch = 256 },
				{ .offset = 14336, .pitch = 256 },
				{ .offset = 15360, .pitch = 256 },
				{ .offset = 15872, .pitch = 256 },
			},
		},
	},

	/* A tiled/ubwc layout from the blob driver, at a size where the a630 blob
	 * driver does something interesting for linear.
	 */
	{
		.format = PIPE_FORMAT_R8G8B8A8_UNORM,
		.layout = {
			.tile_mode = TILE6_3,
			.ubwc = true,
			.width0 = 1024, .height0 = 1,
			.slices = {
				{ .offset = 0, .pitch = 4096 },
				{ .offset = 65536, .pitch = 2048 },
				{ .offset = 98304, .pitch = 1024 },
				{ .offset = 114688, .pitch = 512 },
				{ .offset = 122880, .pitch = 256 },
				{ .offset = 126976, .pitch = 256 },
				{ .offset = 131072, .pitch = 256 },
				{ .offset = 135168, .pitch = 256 },
				{ .offset = 139264, .pitch = 256 },
				{ .offset = 143360, .pitch = 256 },
				{ .offset = 147456, .pitch = 256 },
			},
			.ubwc_slices = {
				{ .offset = 0, .pitch = 64 },
				{ .offset = 4096, .pitch = 64 },
				{ .offset = 8192, .pitch = 64 },
				{ .offset = 12288, .pitch = 64 },
				{ .offset = 16384, .pitch = 64 },
				{ .offset = 20480, .pitch = 64 },
				{ .offset = 24576, .pitch = 64 },
				{ .offset = 28672, .pitch = 64 },
				{ .offset = 32768, .pitch = 64 },
				{ .offset = 36864, .pitch = 64 },
				{ .offset = 40960, .pitch = 64 },
			},
		},
	},

	/* An interesting layout from the blob driver on a630, showing that
	 * per-level pitch must be derived from level 0's pitch, not width0.  We
	 * don't do this level 0 pitch disalignment (we pick 4096), so disabled
	 * this test for now.
	 */
#if 0
	{
		.format = PIPE_FORMAT_R8G8B8A8_UNORM,
		.layout = {
			.width0 = 1024, .height0 = 1,
			.slices = {
				{ .offset = 0, .pitch = 5120 },
				{ .offset = 5120, .pitch = 2560 },
				{ .offset = 7680, .pitch = 1280 },
				{ .offset = 8960, .pitch = 768 },
				{ .offset = 9728, .pitch = 512 },
				{ .offset = 10240, .pitch = 256 },
				{ .offset = 10496, .pitch = 256 },
				{ .offset = 10752, .pitch = 256 },
				{ .offset = 11008, .pitch = 256 },
				{ .offset = 11264, .pitch = 256 },
				{ .offset = 11520, .pitch = 256 },
			},
		},
	},
#endif
};

static bool test_layout(const struct testcase *testcase)
{
	struct fdl_layout layout = {
		.ubwc = testcase->layout.ubwc,
		.tile_mode = testcase->layout.tile_mode,
	};
	bool ok = true;

	int max_size = MAX2(testcase->layout.width0, testcase->layout.height0);
	int mip_levels = 1;
	while (max_size > 1) {
		mip_levels++;
		max_size = u_minify(max_size, 1);
	}

	fdl6_layout(&layout,
			testcase->format,
			MAX2(testcase->layout.nr_samples, 1),
			testcase->layout.width0,
			MAX2(testcase->layout.height0, 1),
			MAX2(testcase->layout.depth0, 1),
			mip_levels,
			MAX2(testcase->array_size, 1),
			testcase->is_3d);

	/* Our pitch values in the testcases[] layouts are in bytes straight out
	 * of the traces, while fdl is in pixels.  Rescale now.
	 */
	for (int l = 0; l < mip_levels; l++)
		layout.slices[l].pitch *= layout.cpp;

	/* fdl lays out UBWC data before the color data, while all we have
	 * recorded in this testcase are the color offsets.  Shift the fdl layout
	 * down so we can compare color offsets.
	 */
	if (layout.ubwc) {
		for (int l = 1; l < mip_levels; l++)
			layout.slices[l].offset -= layout.slices[0].offset;
		layout.slices[0].offset = 0;
	}

	for (int l = 0; l < mip_levels; l++) {
		if (layout.slices[l].offset != testcase->layout.slices[l].offset) {
			fprintf(stderr, "%s %dx%dx%d@%dx lvl%d: offset 0x%x != 0x%x\n",
					util_format_short_name(testcase->format),
					layout.width0, layout.height0, layout.depth0,
					layout.nr_samples, l,
					layout.slices[l].offset,
					testcase->layout.slices[l].offset);
			ok = false;
		}
		if (layout.slices[l].pitch != testcase->layout.slices[l].pitch) {
			fprintf(stderr, "%s %dx%dx%d@%dx lvl%d: pitch %d != %d\n",
					util_format_short_name(testcase->format),
					layout.width0, layout.height0, layout.depth0,
					layout.nr_samples, l,
					layout.slices[l].pitch,
					testcase->layout.slices[l].pitch);
			ok = false;
		}

		if (layout.ubwc_slices[l].offset != testcase->layout.ubwc_slices[l].offset) {
			fprintf(stderr, "%s %dx%dx%d@%dx lvl%d: offset 0x%x != 0x%x\n",
					util_format_short_name(testcase->format),
					layout.width0, layout.height0, layout.depth0,
					layout.nr_samples, l,
					layout.ubwc_slices[l].offset,
					testcase->layout.ubwc_slices[l].offset);
			ok = false;
		}
		if (layout.ubwc_slices[l].pitch != testcase->layout.ubwc_slices[l].pitch) {
			fprintf(stderr, "%s %dx%dx%d@%dx lvl%d: pitch %d != %d\n",
					util_format_short_name(testcase->format),
					layout.width0, layout.height0, layout.depth0,
					layout.nr_samples, l,
					layout.ubwc_slices[l].pitch,
					testcase->layout.ubwc_slices[l].pitch);
			ok = false;
		}
	}

	if (!ok)
		fprintf(stderr, "\n");

	return ok;
}

int
main(int argc, char **argv)
{
	int ret = 0;

	for (int i = 0; i < ARRAY_SIZE(testcases); i++) {
		if (!test_layout(&testcases[i]))
			ret = 1;
	}

	return ret;
}

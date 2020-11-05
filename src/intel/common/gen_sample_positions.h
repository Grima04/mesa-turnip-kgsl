/*
 * Copyright © 2016 Intel Corporation
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
#ifndef GEN_SAMPLE_POSITIONS_H
#define GEN_SAMPLE_POSITIONS_H

#include <util/macros.h>

/*
 * This file defines the standard multisample positions used by both GL and
 * Vulkan.  These correspond to the Vulkan "standard sample locations".
 */

struct gen_sample_position {
   float x;
   float y;
};

extern const struct gen_sample_position gen_sample_positions_1x[];
extern const struct gen_sample_position gen_sample_positions_2x[];
extern const struct gen_sample_position gen_sample_positions_4x[];
extern const struct gen_sample_position gen_sample_positions_8x[];
extern const struct gen_sample_position gen_sample_positions_16x[];

static inline const struct gen_sample_position *
gen_get_sample_positions(int samples)
{
   switch (samples) {
   case 1: return gen_sample_positions_1x;
   case 2: return gen_sample_positions_2x;
   case 4: return gen_sample_positions_4x;
   case 8: return gen_sample_positions_8x;
   case 16: return gen_sample_positions_16x;
   default: unreachable("Invalid sample count");
   }
}

/* Examples:
 * in case of GEN_GEN < 8:
 * GEN_SAMPLE_POS_ELEM(ms.Sample, info->pSampleLocations, 0); expands to:
 *    ms.Sample0XOffset = info->pSampleLocations[0].x;
 *    ms.Sample0YOffset = info->pSampleLocations[0].y;
 *
 * in case of GEN_GEN >= 8:
 * GEN_SAMPLE_POS_ELEM(sp._16xSample, info->pSampleLocations, 0); expands to:
 *    sp._16xSample0XOffset = info->pSampleLocations[0].x;
 *    sp._16xSample0YOffset = info->pSampleLocations[0].y;
 */

#define GEN_SAMPLE_POS_ELEM(prefix, arr, sample_idx) \
prefix##sample_idx##XOffset = arr[sample_idx].x; \
prefix##sample_idx##YOffset = arr[sample_idx].y;

#define GEN_SAMPLE_POS_1X_ARRAY(prefix, arr)\
   GEN_SAMPLE_POS_ELEM(prefix, arr, 0);

#define GEN_SAMPLE_POS_2X_ARRAY(prefix, arr) \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 0); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 1);

#define GEN_SAMPLE_POS_4X_ARRAY(prefix, arr) \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 0); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 1); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 2); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 3);

#define GEN_SAMPLE_POS_8X_ARRAY(prefix, arr) \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 0); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 1); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 2); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 3); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 4); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 5); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 6); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 7);

#define GEN_SAMPLE_POS_16X_ARRAY(prefix, arr) \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 0); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 1); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 2); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 3); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 4); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 5); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 6); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 7); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 8); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 9); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 10); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 11); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 12); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 13); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 14); \
   GEN_SAMPLE_POS_ELEM(prefix, arr, 15);

#define GEN_SAMPLE_POS_1X(prefix) \
   GEN_SAMPLE_POS_1X_ARRAY(prefix, gen_sample_positions_1x)

#define GEN_SAMPLE_POS_2X(prefix) \
   GEN_SAMPLE_POS_2X_ARRAY(prefix, gen_sample_positions_2x)

#define GEN_SAMPLE_POS_4X(prefix) \
   GEN_SAMPLE_POS_4X_ARRAY(prefix, gen_sample_positions_4x)

#define GEN_SAMPLE_POS_8X(prefix) \
   GEN_SAMPLE_POS_8X_ARRAY(prefix, gen_sample_positions_8x)

#define GEN_SAMPLE_POS_16X(prefix) \
   GEN_SAMPLE_POS_16X_ARRAY(prefix, gen_sample_positions_16x)

#endif /* GEN_SAMPLE_POSITIONS_H */

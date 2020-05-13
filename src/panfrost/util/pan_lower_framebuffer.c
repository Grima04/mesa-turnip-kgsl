/*
 * Copyright (C) 2020 Collabora, Ltd.
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
 *
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

/**
 * Implements framebuffer format conversions in software for Midgard/Bifrost
 * blend shaders. This pass is designed for a single render target; Midgard
 * duplicates blend shaders for MRT to simplify everything. A particular
 * framebuffer format may be categorized as 1) typed load available, 2) typed
 * unpack available, or 3) software unpack only, and likewise for stores. The
 * first two types are handled in the compiler backend directly, so this module
 * is responsible for identifying type 3 formats (hardware dependent) and
 * inserting appropriate ALU code to perform the conversion from the packed
 * type to a designated unpacked type, and vice versa.
 *
 * The unpacked type depends on the format:
 *
 *      - For 32-bit float formats, 32-bit floats.
 *      - For other floats, 16-bit floats.
 *      - For 32-bit ints, 32-bit ints.
 *      - For 8-bit ints, 8-bit ints.
 *      - For other ints, 16-bit ints.
 *
 * The rationale is to optimize blending and logic op instructions by using the
 * smallest precision necessary to store the pixel losslessly.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_format_convert.h"
#include "util/format/u_format.h"

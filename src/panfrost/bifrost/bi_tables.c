/*
 * Copyright (C) 2020 Collabora Ltd.
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
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#include "compiler.h"

unsigned bi_class_props[BI_NUM_CLASSES] = {
        [BI_ADD] 		= BI_GENERIC | BI_MODS,
        [BI_ATEST] 		= 0,
        [BI_BRANCH] 		= 0,
        [BI_CMP] 		= BI_GENERIC | BI_MODS,
        [BI_BLEND] 		= 0,
        [BI_BITWISE] 		= BI_GENERIC,
        [BI_CONVERT] 		= 0,
        [BI_CSEL] 		= 0,
        [BI_DISCARD] 		= 0,
        [BI_FMA] 		= BI_ROUNDMODE,
        [BI_FREXP] 		= 0,
        [BI_LOAD] 		= 0,
        [BI_LOAD_ATTR] 		= 0,
        [BI_LOAD_VAR] 		= 0,
        [BI_LOAD_VAR_ADDRESS] 	= 0,
        [BI_MINMAX] 		= BI_GENERIC,
        [BI_MOV] 		= BI_MODS,
        [BI_SHIFT] 		= 0,
        [BI_STORE] 		= 0,
        [BI_STORE_VAR] 		= 0,
        [BI_SPECIAL] 		= 0,
        [BI_TEX] 		= 0,
        [BI_ROUND] 		= BI_GENERIC | BI_ROUNDMODE,
};

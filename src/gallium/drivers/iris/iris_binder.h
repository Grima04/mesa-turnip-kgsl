/*
 * Copyright © 2018 Intel Corporation
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

#ifndef IRIS_BINDER_DOT_H
#define IRIS_BINDER_DOT_H

#include <stdint.h>
#include <stdbool.h>

struct iris_bo;
struct iris_bufmgr;

struct iris_binder
{
   struct iris_bo *bo;
   void *map;

   /* Insert new entries at this offset (in bytes) */
   unsigned insert_point;
};

void iris_init_binder(struct iris_binder *binder, struct iris_bufmgr *bufmgr);
void iris_destroy_binder(struct iris_binder *binder);
void *iris_binder_reserve(struct iris_binder *binder, unsigned size,
                          uint32_t *out_offset);

#endif

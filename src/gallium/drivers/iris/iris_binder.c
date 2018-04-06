/*
 * Copyright © 2018 Intel Corporation
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

#include <stdlib.h>
#include "util/u_math.h"
#include "iris_binder.h"
#include "iris_bufmgr.h"

/* 64kb */
#define BINDER_SIZE (64 * 1024)

void *
iris_binder_reserve(struct iris_binder *binder, unsigned size,
                    uint32_t *out_offset)
{
   /* XXX: Implement a real ringbuffer, for now just croak if run out */
   assert(size > 0);
   assert(binder->insert_point + size <= BINDER_SIZE);

   assert((binder->insert_point % 64) == 0);
   *out_offset = binder->insert_point;

   binder->insert_point = align(binder->insert_point + size, 64);

   return binder->map + *out_offset;
}

void
iris_init_binder(struct iris_binder *binder, struct iris_bufmgr *bufmgr)
{
   binder->bo =
      iris_bo_alloc(bufmgr, "binder", BINDER_SIZE, IRIS_MEMZONE_BINDER);
   binder->map = iris_bo_map(NULL, binder->bo, MAP_WRITE);
   binder->insert_point = 64; // XXX: avoid null pointer, it confuses tools
}

void
iris_destroy_binder(struct iris_binder *binder)
{
   iris_bo_unreference(binder->bo);
   free(binder);
}

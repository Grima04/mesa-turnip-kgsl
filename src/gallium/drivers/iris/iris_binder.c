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
#include "iris_context.h"

/* 64kb */
#define BINDER_SIZE (64 * 1024)

/**
 * Reserve a block of space in the binder.
 */
uint32_t
iris_binder_reserve(struct iris_batch *batch, unsigned size)
{
   struct iris_binder *binder = &batch->binder;

   assert(size > 0);
   assert((binder->insert_point % 64) == 0);

   /* If we can't fit all stages in the binder, flush the batch which
    * will cause us to gain a new empty binder.
    */
   if (binder->insert_point + size > BINDER_SIZE)
      iris_batch_flush(batch);

   uint32_t offset = binder->insert_point;

   /* It had better fit now. */
   assert(offset + size <= BINDER_SIZE);

   binder->insert_point = align(binder->insert_point + size, 64);

   iris_use_pinned_bo(batch, binder->bo, false);

   return offset;
}

/**
 * Reserve and record binder space for 3D pipeline shader stages.
 */
void
iris_binder_reserve_3d(struct iris_batch *batch,
                       struct iris_compiled_shader **shaders)
{
   struct iris_binder *binder = &batch->binder;
   unsigned total_size = 0;
   unsigned sizes[MESA_SHADER_STAGES] = {};

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (!shaders[stage])
         continue;

      const struct brw_stage_prog_data *prog_data =
         (const void *) shaders[stage]->prog_data;

      sizes[stage] = align(prog_data->binding_table.size_bytes, 64);
      total_size += sizes[stage];
   }

   uint32_t offset = iris_binder_reserve(batch, total_size);

   /* Assign space and record the current binding table. */
   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      binder->bt_offset[stage] = sizes[stage] > 0 ? offset : 0;
      offset += sizes[stage];
   }
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
}

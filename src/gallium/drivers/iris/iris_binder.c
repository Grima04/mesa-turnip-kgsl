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
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iris_binder.c
 *
 * Shader programs refer to most resources via integer handles.  These are
 * indexes (BTIs) into a "Binding Table", which is simply a list of pointers
 * to SURFACE_STATE entries.  Each shader stage has its own binding table,
 * set by the 3DSTATE_BINDING_TABLE_POINTERS_* commands.  Both the binding
 * table itself and the SURFACE_STATEs are relative to Surface State Base
 * Address, so they all live in IRIS_MEMZONE_SURFACE.
 *
 * Unfortunately, the hardware designers made 3DSTATE_BINDING_TABLE_POINTERS
 * only accept a 16-bit pointer.  This means that all binding tables have to
 * live within the 64kB range starting at Surface State Base Address.  (The
 * actual SURFACE_STATE entries can live anywhere in the 4GB zone, as the
 * binding table entries are full 32-bit pointers.)
 *
 * We stream out binding tables dynamically, storing them in a single 64kB
 * "binder" buffer, located at IRIS_BINDER_ADDRESS.  Before emitting a draw
 * call, we reserve space for any new binding tables needed by bound shaders.
 * If there is no space, we flush the batch and swap out the binder for a
 * new empty BO.
 *
 * XXX: This should be fancier.  We currently replace the binder with a
 * fresh BO on every batch, which causes the kernel to stall, trying to
 * pin the new buffer at the same memory address as the old one.  We ought
 * to avoid this by using a ringbuffer, tracking the busy section of the BO,
 * and cycling back around where possible to avoid replacing it at all costs.
 *
 * XXX: if we do have to flush, we should emit a performance warning.
 */

#include <stdlib.h>
#include "util/u_math.h"
#include "iris_binder.h"
#include "iris_bufmgr.h"
#include "iris_context.h"

#define BTP_ALIGNMENT 32

/**
 * Reserve a block of space in the binder, given the raw size in bytes.
 */
uint32_t
iris_binder_reserve(struct iris_batch *batch, unsigned size)
{
   struct iris_binder *binder = &batch->binder;

   assert(size > 0);
   assert((binder->insert_point % BTP_ALIGNMENT) == 0);

   /* If we can't fit all stages in the binder, flush the batch which
    * will cause us to gain a new empty binder.
    */
   if (binder->insert_point + size > IRIS_BINDER_SIZE)
      iris_batch_flush(batch);

   uint32_t offset = binder->insert_point;

   /* It had better fit now. */
   assert(offset + size <= IRIS_BINDER_SIZE);

   binder->insert_point = align(binder->insert_point + size, BTP_ALIGNMENT);

   iris_use_pinned_bo(batch, binder->bo, false);

   return offset;
}

/**
 * Reserve and record binder space for 3D pipeline shader stages.
 *
 * Note that you must actually populate the new binding tables after
 * calling this command - the new area is uninitialized.
 */
void
iris_binder_reserve_3d(struct iris_batch *batch,
                       struct iris_context *ice)
{
   struct iris_compiled_shader **shaders = ice->shaders.prog;
   struct iris_binder *binder = &batch->binder;
   unsigned total_size = 0;
   unsigned sizes[MESA_SHADER_STAGES] = {};

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (!(ice->state.dirty & (IRIS_DIRTY_BINDINGS_VS << stage)))
         continue;

      if (!shaders[stage])
         continue;

      const struct brw_stage_prog_data *prog_data =
         (const void *) shaders[stage]->prog_data;

      sizes[stage] = align(prog_data->binding_table.size_bytes, BTP_ALIGNMENT);
      total_size += sizes[stage];
   }

   if (total_size == 0)
      return;

   uint32_t offset = iris_binder_reserve(batch, total_size);

   /* Assign space and record the current binding table. */
   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (!(ice->state.dirty & (IRIS_DIRTY_BINDINGS_VS << stage)))
         continue;

      binder->bt_offset[stage] = sizes[stage] > 0 ? offset : 0;
      offset += sizes[stage];
   }
}

/* Avoid using offset 0, tools consider it NULL */
#define INIT_INSERT_POINT BTP_ALIGNMENT

void
iris_init_binder(struct iris_binder *binder, struct iris_bufmgr *bufmgr)
{
   binder->bo =
      iris_bo_alloc(bufmgr, "binder", IRIS_BINDER_SIZE, IRIS_MEMZONE_BINDER);
   binder->map = iris_bo_map(NULL, binder->bo, MAP_WRITE);
   binder->insert_point = INIT_INSERT_POINT;
}

/**
 * Is the binder empty?  (If so, old binding table pointers are stale.)
 */
bool
iris_binder_is_empty(struct iris_binder *binder)
{
   return binder->insert_point <= INIT_INSERT_POINT;
}

void
iris_destroy_binder(struct iris_binder *binder)
{
   iris_bo_unreference(binder->bo);
}

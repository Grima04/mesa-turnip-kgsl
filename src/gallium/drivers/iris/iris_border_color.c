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

#define BC_ALIGNMENT 64

static bool
color_equals(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(union pipe_color_union)) == 0;
}

static uint32_t
color_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(union pipe_color_union));
}

static void
iris_reset_border_color_pool(struct iris_border_color_pool *pool,
                             struct iris_bufmgr *bufmgr)
{
   _mesa_hash_table_clear(pool->ht, NULL);

   iris_bo_unreference(pool->bo);

   pool->bo = iris_bo_alloc(bufmgr, "border colors",
                            IRIS_BORDER_COLOR_POOL_SIZE,
                            IRIS_MEMZONE_BORDER_COLOR_POOL);
   pool->map = iris_bo_map(NULL, pool->bo, MAP_WRITE);

   /* Don't make 0 a valid offset - tools treat that as a NULL pointer. */
   pool->insert_point = BC_ALIGNMENT;
}

void
iris_init_border_color_pool(struct iris_context *ice)
{
   struct iris_screen *screen = (void *) ice->ctx.screen;
   struct iris_bufmgr *bufmgr = screen->bufmgr;

   struct iris_border_color_pool *pool = &ice->state.border_color_pool;

   pool->bo = NULL;
   pool->ht = _mesa_hash_table_create(ice, color_hash, color_equals);

   iris_reset_border_color_pool(pool, bufmgr);
}

/**
 * Reserve space for a number of border colors.  If no space, flushes any
 * batches that are referring to the old BO and makes a new one.
 */
void
iris_border_color_pool_reserve(struct iris_context *ice, unsigned count)
{
   struct iris_border_color_pool *pool = &ice->state.border_color_pool;
   const unsigned remaining_entries =
      (IRIS_BORDER_COLOR_POOL_SIZE - pool->insert_point) / BC_ALIGNMENT;

   if (remaining_entries < count) {
      if (iris_batch_references(&ice->render_batch, pool->bo))
         iris_batch_flush(&ice->render_batch);

      iris_reset_border_color_pool(pool, pool->bo->bufmgr);
   }
}

/**
 * Upload a border color (or use a cached version).
 *
 * Returns the offset into the border color pool BO.
 */
uint32_t
iris_upload_border_color(struct iris_context *ice,
                         union pipe_color_union *color)
{
   struct iris_border_color_pool *pool = &ice->state.border_color_pool;

   uint32_t hash = color_hash(color);
   struct hash_entry *entry =
      _mesa_hash_table_search_pre_hashed(pool->ht, hash, color);
   if (entry)
      return (uintptr_t) entry->data;

   assert(pool->insert_point + BC_ALIGNMENT < IRIS_BORDER_COLOR_POOL_SIZE);

   uint32_t offset = pool->insert_point;
   memcpy(pool->map + offset, color, sizeof(*color));
   pool->insert_point += BC_ALIGNMENT;

   _mesa_hash_table_insert_pre_hashed(pool->ht, hash, color,
                                      (void *) (uintptr_t) offset);
   return offset;
}


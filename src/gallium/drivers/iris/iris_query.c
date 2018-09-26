/*
 * Copyright © 2017 Intel Corporation
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
 * @file iris_query.c
 *
 * XXX: this file is EMPTY.  it will eventually implement query objects!
 */

#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "iris_context.h"
#include "iris_defines.h"
#include "iris_resource.h"
#include "iris_screen.h"

#define CS_GPR(n) (0x2600 + (n) * 8)

#define MI_MATH (0x1a << 23)

#define MI_ALU_LOAD      0x080
#define MI_ALU_LOADINV   0x480
#define MI_ALU_LOAD0     0x081
#define MI_ALU_LOAD1     0x481
#define MI_ALU_ADD       0x100
#define MI_ALU_SUB       0x101
#define MI_ALU_AND       0x102
#define MI_ALU_OR        0x103
#define MI_ALU_XOR       0x104
#define MI_ALU_STORE     0x180
#define MI_ALU_STOREINV  0x580

#define MI_ALU_R0        0x00
#define MI_ALU_R1        0x01
#define MI_ALU_R2        0x02
#define MI_ALU_R3        0x03
#define MI_ALU_R4        0x04
#define MI_ALU_SRCA      0x20
#define MI_ALU_SRCB      0x21
#define MI_ALU_ACCU      0x31
#define MI_ALU_ZF        0x32
#define MI_ALU_CF        0x33

#define MI_ALU0(op)       ((MI_ALU_##op << 20))
#define MI_ALU1(op, x)    ((MI_ALU_##op << 20) | (MI_ALU_##x << 10)
#define MI_ALU2(op, x, y) \
   ((MI_ALU_##op << 20) | (MI_ALU_##x << 10) | (MI_ALU_##y))

struct iris_query {
   enum pipe_query_type type;

   bool ready;

   uint64_t result;

   struct iris_bo *bo;
   struct iris_query_snapshots *map;
};

struct iris_query_snapshots {
   uint64_t start;
   uint64_t end;
   uint64_t snapshots_landed;
};

/**
 * Is this type of query written by PIPE_CONTROL?
 */
static bool
iris_is_query_pipelined(struct iris_query *q)
{
   switch (q->type) {
   case PIPE_QUERY_OCCLUSION_COUNTER:
   case PIPE_QUERY_OCCLUSION_PREDICATE:
   case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
   case PIPE_QUERY_TIMESTAMP:
   case PIPE_QUERY_TIMESTAMP_DISJOINT:
   case PIPE_QUERY_TIME_ELAPSED:
      return true;

   default:
      return false;
   }
}

static void
write_availability(struct iris_context *ice,
                   struct iris_query *q,
                   bool available)
{
   struct iris_batch *batch = &ice->render_batch;
   unsigned flags = PIPE_CONTROL_WRITE_IMMEDIATE;
   unsigned offset = offsetof(struct iris_query_snapshots, snapshots_landed);

   if (!iris_is_query_pipelined(q)) {
      ice->vtbl.store_data_imm64(batch, q->bo, offset, available);
   } else {
      if (available) {
         /* Order available *after* the query results. */
         flags |= PIPE_CONTROL_FLUSH_ENABLE;
      } else {
         /* Make it unavailable *before* any pipelined reads. */
         flags |= PIPE_CONTROL_CS_STALL;
      }
      iris_emit_pipe_control_write(batch, flags, q->bo, offset, available);
   }
}

/**
 * Write PS_DEPTH_COUNT to q->(dest) via a PIPE_CONTROL.
 */
static void
iris_pipelined_write(struct iris_batch *batch,
                     struct iris_query *q,
                     enum pipe_control_flags flags,
                     unsigned offset)
{
   const struct gen_device_info *devinfo = &batch->screen->devinfo;
   const unsigned optional_cs_stall =
      devinfo->gen == 9 && devinfo->gt == 4 ?  PIPE_CONTROL_CS_STALL : 0;

   iris_emit_pipe_control_write(batch, flags | optional_cs_stall,
                                q->bo, offset, 0ull);
}

static void
write_value(struct iris_context *ice, struct iris_query *q, unsigned offset)
{
   struct iris_batch *batch = &ice->render_batch;
   const struct gen_device_info *devinfo = &batch->screen->devinfo;

   iris_use_pinned_bo(batch, q->bo, true);

   switch (q->type) {
   case PIPE_QUERY_OCCLUSION_COUNTER:
   case PIPE_QUERY_OCCLUSION_PREDICATE:
   case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
      if (devinfo->gen >= 10) {
         /* "Driver must program PIPE_CONTROL with only Depth Stall Enable
          *  bit set prior to programming a PIPE_CONTROL with Write PS Depth
          *  Count sync operation."
          */
         iris_emit_pipe_control_flush(batch, PIPE_CONTROL_DEPTH_STALL);
      }
      iris_pipelined_write(&ice->render_batch, q,
                           PIPE_CONTROL_WRITE_DEPTH_COUNT |
                           PIPE_CONTROL_DEPTH_STALL,
                           offset);
      break;
   case PIPE_QUERY_TIME_ELAPSED:
      iris_pipelined_write(&ice->render_batch, q,
                           PIPE_CONTROL_WRITE_TIMESTAMP,
                           offset);
      break;
   default:
      assert(false);
   }
}

static void
calculate_result_on_cpu(struct iris_query *q)
{
   switch (q->type) {
   case PIPE_QUERY_OCCLUSION_PREDICATE:
   case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
      q->result = q->map->end != q->map->start;
      break;
   case PIPE_QUERY_OCCLUSION_COUNTER:
   case PIPE_QUERY_TIME_ELAPSED:
   case PIPE_QUERY_PRIMITIVES_GENERATED:
   case PIPE_QUERY_PRIMITIVES_EMITTED:
   default:
      q->result = q->map->end - q->map->start;
      break;
   }

   q->ready = true;
}

/**
 * Calculate the result and store it to CS_GPR0.
 */
static void
calculate_result_on_gpu(struct iris_context *ice, struct iris_query *q)
{
   struct iris_batch *batch = &ice->render_batch;

   ice->vtbl.load_register_mem64(batch, CS_GPR(1), q->bo,
                                 offsetof(struct iris_query_snapshots, start));
   ice->vtbl.load_register_mem64(batch, CS_GPR(2), q->bo,
                                 offsetof(struct iris_query_snapshots, end));

   static const uint32_t math[] = {
      MI_MATH | (5 - 2),
      MI_ALU2(LOAD, SRCA, R2),
      MI_ALU2(LOAD, SRCB, R1),
      MI_ALU0(SUB),
      MI_ALU2(STORE, R0, ACCU),
   };
   iris_batch_emit(batch, math, sizeof(math));
}

static struct pipe_query *
iris_create_query(struct pipe_context *ctx,
                  unsigned query_type,
                  unsigned index)
{
   struct iris_query *q = calloc(1, sizeof(struct iris_query));

   q->type = query_type;

   return (struct pipe_query *) q;
}

static void
iris_destroy_query(struct pipe_context *ctx, struct pipe_query *p_query)
{
   struct iris_query *query = (void *) p_query;
   iris_bo_unreference(query->bo);
   free(query);
}


static boolean
iris_begin_query(struct pipe_context *ctx, struct pipe_query *query)
{
   struct iris_screen *screen = (void *) ctx->screen;
   struct iris_context *ice = (void *) ctx;
   struct iris_query *q = (void *) query;

   iris_bo_unreference(q->bo);
   q->bo = iris_bo_alloc(screen->bufmgr, "query object", 4096,
                         IRIS_MEMZONE_OTHER);
   if (!q->bo)
      return false;

   q->map = iris_bo_map(&ice->dbg, q->bo, MAP_READ | MAP_ASYNC);
   if (!q->map)
      return false;

   q->result = 0ull;
   q->ready = false;

   write_availability(ice, q, false);
   write_value(ice, q, offsetof(struct iris_query_snapshots, start));

   return true;
}

static bool
iris_end_query(struct pipe_context *ctx, struct pipe_query *query)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_query *q = (void *) query;

   write_value(ice, q, offsetof(struct iris_query_snapshots, end));
   write_availability(ice, q, true);

   return true;
}

static boolean
iris_get_query_result(struct pipe_context *ctx,
                      struct pipe_query *query,
                      boolean wait,
                      union pipe_query_result *result)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_query *q = (void *) query;

   if (!q->ready) {
      if (iris_batch_references(&ice->render_batch, q->bo))
         iris_batch_flush(&ice->render_batch);

      if (!q->map->snapshots_landed) {
         if (wait)
            iris_bo_wait_rendering(q->bo);
         else
            return false;
      }

      assert(q->map->snapshots_landed);
      calculate_result_on_cpu(q);
   }

   assert(q->ready);
   result->u64 = q->result;

   return true;
}

static void
iris_get_query_result_resource(struct pipe_context *ctx,
                               struct pipe_query *query,
                               boolean wait,
                               enum pipe_query_value_type result_type,
                               int index,
                               struct pipe_resource *p_res,
                               unsigned offset)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_query *q = (void *) query;
   struct iris_batch *batch = &ice->render_batch;

   if (!q->ready && q->map->snapshots_landed) {
      /* The final snapshots happen to have landed, so let's just compute
       * the result on the CPU now...
       */
      calculate_result_on_cpu(q);
   }

   if (q->ready) {
      /* We happen to have the result on the CPU, so just copy it. */
      if (result_type <= PIPE_QUERY_TYPE_U32) {
         ice->vtbl.store_data_imm32(batch, iris_resource_bo(p_res), offset,
                                    q->result);
      } else {
         ice->vtbl.store_data_imm64(batch, iris_resource_bo(p_res), offset,
                                    q->result);
      }
      return;
   }

   /* Calculate the result to CS_GPR0 */
   calculate_result_on_gpu(ice, q);

   bool predicated = !wait && iris_is_query_pipelined(q);

   if (predicated) {
      ice->vtbl.load_register_imm64(batch, MI_PREDICATE_SRC1, 0ull);
      ice->vtbl.load_register_mem64(batch, MI_PREDICATE_SRC0, q->bo,
                                    offsetof(struct iris_query_snapshots,
                                             snapshots_landed));
      uint32_t predicate = MI_PREDICATE |
                           MI_PREDICATE_LOADOP_LOADINV |
                           MI_PREDICATE_COMBINEOP_SET |
                           MI_PREDICATE_COMPAREOP_SRCS_EQUAL;
      iris_batch_emit(batch, &predicate, sizeof(uint32_t));
   }

   if (result_type <= PIPE_QUERY_TYPE_U32) {
      ice->vtbl.store_register_mem32(batch, CS_GPR(0),
                                     iris_resource_bo(p_res),
                                     offset, predicated);
   } else {
      ice->vtbl.store_register_mem64(batch, CS_GPR(0),
                                     iris_resource_bo(p_res),
                                     offset, predicated);
   }
}

static void
iris_set_active_query_state(struct pipe_context *pipe, boolean enable)
{
   /* Do nothing, intentionally - only u_blitter uses this. */
}

void
iris_init_query_functions(struct pipe_context *ctx)
{
   ctx->create_query = iris_create_query;
   ctx->destroy_query = iris_destroy_query;
   ctx->begin_query = iris_begin_query;
   ctx->end_query = iris_end_query;
   ctx->get_query_result = iris_get_query_result;
   ctx->get_query_result_resource = iris_get_query_result_resource;
   ctx->set_active_query_state = iris_set_active_query_state;
}

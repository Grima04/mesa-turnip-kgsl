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
 * Query object support.  This allows measuring various simple statistics
 * via counters on the GPU.
 */

#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/fast_idiv_by_const.h"
#include "util/u_inlines.h"
#include "iris_context.h"
#include "iris_defines.h"
#include "iris_resource.h"
#include "iris_screen.h"
#include "vulkan/util/vk_util.h"

#define IA_VERTICES_COUNT          0x2310
#define IA_PRIMITIVES_COUNT        0x2318
#define VS_INVOCATION_COUNT        0x2320
#define HS_INVOCATION_COUNT        0x2300
#define DS_INVOCATION_COUNT        0x2308
#define GS_INVOCATION_COUNT        0x2328
#define GS_PRIMITIVES_COUNT        0x2330
#define CL_INVOCATION_COUNT        0x2338
#define CL_PRIMITIVES_COUNT        0x2340
#define PS_INVOCATION_COUNT        0x2348
#define CS_INVOCATION_COUNT        0x2290
#define PS_DEPTH_COUNT             0x2350

#define SO_PRIM_STORAGE_NEEDED(n)  (0x5240 + (n) * 8)

#define SO_NUM_PRIMS_WRITTEN(n)    (0x5200 + (n) * 8)

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

#define _MI_ALU(op, x, y)  (((op) << 20) | ((x) << 10) | (y))

#define _MI_ALU0(op)       _MI_ALU(MI_ALU_##op, 0, 0)
#define _MI_ALU1(op, x)    _MI_ALU(MI_ALU_##op, x, 0)
#define _MI_ALU2(op, x, y) _MI_ALU(MI_ALU_##op, x, y)

#define MI_ALU0(op)        _MI_ALU0(op)
#define MI_ALU1(op, x)     _MI_ALU1(op, MI_ALU_##x)
#define MI_ALU2(op, x, y)  _MI_ALU2(op, MI_ALU_##x, MI_ALU_##y)

#define emit_lri32 ice->vtbl.load_register_imm32
#define emit_lri64 ice->vtbl.load_register_imm64
#define emit_lrr32 ice->vtbl.load_register_reg32

struct iris_query {
   enum pipe_query_type type;
   int index;

   bool ready;

   bool stalled;

   uint64_t result;

   struct iris_bo *bo;
   struct iris_query_snapshots *map;

   int batch_idx;
};

struct iris_query_snapshots {
   /** iris_render_condition's saved MI_PREDICATE_DATA value. */
   uint64_t predicate_data;

   /** Have the start/end snapshots landed? */
   uint64_t snapshots_landed;

   /** Starting and ending counter snapshots */
   uint64_t start;
   uint64_t end;
};

struct iris_query_so_overflow {
   uint64_t predicate_data;
   uint64_t snapshots_landed;

   struct {
      uint64_t prim_storage_needed[2];
      uint64_t num_prims[2];
   } stream[4];
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
mark_available(struct iris_context *ice, struct iris_query *q)
{
   struct iris_batch *batch = &ice->batches[q->batch_idx];
   unsigned flags = PIPE_CONTROL_WRITE_IMMEDIATE;
   unsigned offset = offsetof(struct iris_query_snapshots, snapshots_landed);

   if (!iris_is_query_pipelined(q)) {
      ice->vtbl.store_data_imm64(batch, q->bo, offset, true);
   } else {
      /* Order available *after* the query results. */
      flags |= PIPE_CONTROL_FLUSH_ENABLE;
      iris_emit_pipe_control_write(batch, flags, q->bo, offset, true);
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
   struct iris_batch *batch = &ice->batches[q->batch_idx];
   const struct gen_device_info *devinfo = &batch->screen->devinfo;

   if (!iris_is_query_pipelined(q)) {
      iris_emit_pipe_control_flush(batch,
                                   PIPE_CONTROL_CS_STALL |
                                   PIPE_CONTROL_STALL_AT_SCOREBOARD);
      q->stalled = true;
   }

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
      iris_pipelined_write(&ice->batches[IRIS_BATCH_RENDER], q,
                           PIPE_CONTROL_WRITE_DEPTH_COUNT |
                           PIPE_CONTROL_DEPTH_STALL,
                           offset);
      break;
   case PIPE_QUERY_TIME_ELAPSED:
   case PIPE_QUERY_TIMESTAMP:
   case PIPE_QUERY_TIMESTAMP_DISJOINT:
      iris_pipelined_write(&ice->batches[IRIS_BATCH_RENDER], q,
                           PIPE_CONTROL_WRITE_TIMESTAMP,
                           offset);
      break;
   case PIPE_QUERY_PRIMITIVES_GENERATED:
      ice->vtbl.store_register_mem64(batch,
                                     q->index == 0 ? CL_INVOCATION_COUNT :
                                     SO_PRIM_STORAGE_NEEDED(q->index),
                                     q->bo, offset, false);
      break;
   case PIPE_QUERY_PRIMITIVES_EMITTED:
      ice->vtbl.store_register_mem64(batch,
                                     SO_NUM_PRIMS_WRITTEN(q->index),
                                     q->bo, offset, false);
      break;
   case PIPE_QUERY_PIPELINE_STATISTICS: {
      static const uint32_t index_to_reg[] = {
         IA_VERTICES_COUNT,
         IA_PRIMITIVES_COUNT,
         VS_INVOCATION_COUNT,
         GS_INVOCATION_COUNT,
         GS_PRIMITIVES_COUNT,
         CL_INVOCATION_COUNT,
         CL_PRIMITIVES_COUNT,
         PS_INVOCATION_COUNT,
         HS_INVOCATION_COUNT,
         DS_INVOCATION_COUNT,
         CS_INVOCATION_COUNT,
      };
      const uint32_t reg = index_to_reg[q->index];

      ice->vtbl.store_register_mem64(batch, reg, q->bo, offset, false);
      break;
   }
   default:
      assert(false);
   }
}

static void
write_overflow_values(struct iris_context *ice, struct iris_query *q, bool end)
{
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];
   uint32_t count = q->type == PIPE_QUERY_SO_OVERFLOW_PREDICATE ? 1 : 4;

   iris_emit_pipe_control_flush(batch,
                                PIPE_CONTROL_CS_STALL |
                                PIPE_CONTROL_STALL_AT_SCOREBOARD);
   for (uint32_t i = 0; i < count; i++) {
      int s = q->index + i;
      int g_idx = offsetof(struct iris_query_so_overflow,
                           stream[s].num_prims[end]);
      int w_idx = offsetof(struct iris_query_so_overflow,
                           stream[s].prim_storage_needed[end]);
      ice->vtbl.store_register_mem64(batch, SO_NUM_PRIMS_WRITTEN(s),
                                     q->bo, g_idx, false);
      ice->vtbl.store_register_mem64(batch, SO_PRIM_STORAGE_NEEDED(s),
                                     q->bo, w_idx, false);
   }
}

uint64_t
iris_timebase_scale(const struct gen_device_info *devinfo,
                    uint64_t gpu_timestamp)
{
   return (1000000000ull * gpu_timestamp) / devinfo->timestamp_frequency;
}

static uint64_t
iris_raw_timestamp_delta(uint64_t time0, uint64_t time1)
{
   if (time0 > time1) {
      return (1ULL << TIMESTAMP_BITS) + time1 - time0;
   } else {
      return time1 - time0;
   }
}

static bool
stream_overflowed(struct iris_query_so_overflow *so, int s)
{
   return (so->stream[s].prim_storage_needed[1] -
           so->stream[s].prim_storage_needed[0]) !=
          (so->stream[s].num_prims[1] - so->stream[s].num_prims[0]);
}

static void
calculate_result_on_cpu(const struct gen_device_info *devinfo,
                        struct iris_query *q)
{
   switch (q->type) {
   case PIPE_QUERY_OCCLUSION_PREDICATE:
   case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE:
      q->result = q->map->end != q->map->start;
      break;
   case PIPE_QUERY_TIMESTAMP:
   case PIPE_QUERY_TIMESTAMP_DISJOINT:
      /* The timestamp is the single starting snapshot. */
      q->result = iris_timebase_scale(devinfo, q->map->start);
      q->result &= (1ull << TIMESTAMP_BITS) - 1;
      break;
   case PIPE_QUERY_TIME_ELAPSED:
      q->result = iris_raw_timestamp_delta(q->map->start, q->map->end);
      q->result = iris_timebase_scale(devinfo, q->result);
      q->result &= (1ull << TIMESTAMP_BITS) - 1;
      break;
   case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
      q->result = stream_overflowed((void *) q->map, q->index);
      break;
   case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
      q->result = false;
      for (int i = 0; i < MAX_VERTEX_STREAMS; i++)
         q->result |= stream_overflowed((void *) q->map, i);
      break;
   case PIPE_QUERY_OCCLUSION_COUNTER:
   case PIPE_QUERY_PRIMITIVES_GENERATED:
   case PIPE_QUERY_PRIMITIVES_EMITTED:
   case PIPE_QUERY_PIPELINE_STATISTICS:
   default:
      q->result = q->map->end - q->map->start;
      break;
   }

   q->ready = true;
}

static void
emit_alu_add(struct iris_batch *batch, unsigned dst_reg,
             unsigned reg_a, unsigned reg_b)
{
   uint32_t *math = iris_get_command_space(batch, 5 * sizeof(uint32_t));

   math[0] = MI_MATH | (5 - 2);
   math[1] = _MI_ALU2(LOAD, MI_ALU_SRCA, reg_a);
   math[2] = _MI_ALU2(LOAD, MI_ALU_SRCB, reg_b);
   math[3] = _MI_ALU0(ADD);
   math[4] = _MI_ALU2(STORE, dst_reg, MI_ALU_ACCU);
}

static void
emit_alu_shl(struct iris_batch *batch, unsigned dst_reg,
             unsigned src_reg, unsigned shift)
{
   assert(shift > 0);

   int dwords = 1 + 4 * shift;

   uint32_t *math = iris_get_command_space(batch, sizeof(uint32_t) * dwords);

   math[0] = MI_MATH | ((1 + 4 * shift) - 2);

   for (unsigned i = 0; i < shift; i++) {
      unsigned add_src = (i == 0) ? src_reg : dst_reg;
      math[1 + (i * 4) + 0] = _MI_ALU2(LOAD, MI_ALU_SRCA, add_src);
      math[1 + (i * 4) + 1] = _MI_ALU2(LOAD, MI_ALU_SRCB, add_src);
      math[1 + (i * 4) + 2] = _MI_ALU0(ADD);
      math[1 + (i * 4) + 3] = _MI_ALU2(STORE, dst_reg, MI_ALU_ACCU);
   }
}

/* Emit dwords to multiply GPR0 by N */
static void
build_alu_multiply_gpr0(uint32_t *dw, unsigned *dw_count, uint32_t N)
{
   VK_OUTARRAY_MAKE(out, dw, dw_count);

#define APPEND_ALU(op, x, y) \
   vk_outarray_append(&out, alu_dw) *alu_dw = _MI_ALU(MI_ALU_##op, x, y)

   assert(N > 0);
   unsigned top_bit = 31 - __builtin_clz(N);
   for (int i = top_bit - 1; i >= 0; i--) {
      /* We get our initial data in GPR0 and we write the final data out to
       * GPR0 but we use GPR1 as our scratch register.
       */
      unsigned src_reg = i == top_bit - 1 ? MI_ALU_R0 : MI_ALU_R1;
      unsigned dst_reg = i == 0 ? MI_ALU_R0 : MI_ALU_R1;

      /* Shift the current value left by 1 */
      APPEND_ALU(LOAD, MI_ALU_SRCA, src_reg);
      APPEND_ALU(LOAD, MI_ALU_SRCB, src_reg);
      APPEND_ALU(ADD, 0, 0);

      if (N & (1 << i)) {
         /* Store ACCU to R1 and add R0 to R1 */
         APPEND_ALU(STORE, MI_ALU_R1, MI_ALU_ACCU);
         APPEND_ALU(LOAD, MI_ALU_SRCA, MI_ALU_R0);
         APPEND_ALU(LOAD, MI_ALU_SRCB, MI_ALU_R1);
         APPEND_ALU(ADD, 0, 0);
      }

      APPEND_ALU(STORE, dst_reg, MI_ALU_ACCU);
   }

#undef APPEND_ALU
}

static void
emit_mul_gpr0(struct iris_batch *batch, uint32_t N)
{
   uint32_t num_dwords;
   build_alu_multiply_gpr0(NULL, &num_dwords, N);

   uint32_t *math = iris_get_command_space(batch, 4 * num_dwords);
   math[0] = MI_MATH | (num_dwords - 2);
   build_alu_multiply_gpr0(&math[1], &num_dwords, N);
}

void
iris_math_div32_gpr0(struct iris_context *ice,
                     struct iris_batch *batch,
                     uint32_t D)
{
   /* Zero out the top of GPR0 */
   emit_lri32(batch, CS_GPR(0) + 4, 0);

   if (D == 0) {
      /* This invalid, but we should do something so we set GPR0 to 0. */
      emit_lri32(batch, CS_GPR(0), 0);
   } else if (util_is_power_of_two_or_zero(D)) {
      unsigned log2_D = util_logbase2(D);
      assert(log2_D < 32);
      /* We right-shift by log2(D) by left-shifting by 32 - log2(D) and taking
       * the top 32 bits of the result.
       */
      emit_alu_shl(batch, MI_ALU_R0, MI_ALU_R0, 32 - log2_D);
      emit_lrr32(batch, CS_GPR(0) + 0, CS_GPR(0) + 4);
      emit_lri32(batch, CS_GPR(0) + 4, 0);
   } else {
      struct util_fast_udiv_info m = util_compute_fast_udiv_info(D, 32, 32);
      assert(m.multiplier <= UINT32_MAX);

      if (m.pre_shift) {
         /* We right-shift by L by left-shifting by 32 - l and taking the top
          * 32 bits of the result.
          */
         if (m.pre_shift < 32)
            emit_alu_shl(batch, MI_ALU_R0, MI_ALU_R0, 32 - m.pre_shift);
         emit_lrr32(batch, CS_GPR(0) + 0, CS_GPR(0) + 4);
         emit_lri32(batch, CS_GPR(0) + 4, 0);
      }

      /* Do the 32x32 multiply into gpr0 */
      emit_mul_gpr0(batch, m.multiplier);

      if (m.increment) {
         /* If we need to increment, save off a copy of GPR0 */
         emit_lri32(batch, CS_GPR(1) + 0, m.multiplier);
         emit_lri32(batch, CS_GPR(1) + 4, 0);
         emit_alu_add(batch, MI_ALU_R0, MI_ALU_R0, MI_ALU_R1);
      }

      /* Shift by 32 */
      emit_lrr32(batch, CS_GPR(0) + 0, CS_GPR(0) + 4);
      emit_lri32(batch, CS_GPR(0) + 4, 0);

      if (m.post_shift) {
         /* We right-shift by L by left-shifting by 32 - l and taking the top
          * 32 bits of the result.
          */
         if (m.post_shift < 32)
            emit_alu_shl(batch, MI_ALU_R0, MI_ALU_R0, 32 - m.post_shift);
         emit_lrr32(batch, CS_GPR(0) + 0, CS_GPR(0) + 4);
         emit_lri32(batch, CS_GPR(0) + 4, 0);
      }
   }
}

/*
 * GPR0 = (GPR0 == 0) ? 0 : 1;
 */
static void
gpr0_to_bool(struct iris_context *ice)
{
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];

   ice->vtbl.load_register_imm64(batch, CS_GPR(1), 1ull);

   static const uint32_t math[] = {
      MI_MATH | (9 - 2),
      MI_ALU2(LOAD, SRCA, R0),
      MI_ALU1(LOAD0, SRCB),
      MI_ALU0(ADD),
      MI_ALU2(STOREINV, R0, ZF),
      MI_ALU2(LOAD, SRCA, R0),
      MI_ALU2(LOAD, SRCB, R1),
      MI_ALU0(AND),
      MI_ALU2(STORE, R0, ACCU),
   };
   iris_batch_emit(batch, math, sizeof(math));
}

static void
load_overflow_data_to_cs_gprs(struct iris_context *ice,
                              struct iris_query *q,
                              int idx)
{
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];

   ice->vtbl.load_register_mem64(batch, CS_GPR(1), q->bo,
                                 offsetof(struct iris_query_so_overflow,
                                          stream[idx].prim_storage_needed[0]));
   ice->vtbl.load_register_mem64(batch, CS_GPR(2), q->bo,
                                 offsetof(struct iris_query_so_overflow,
                                          stream[idx].prim_storage_needed[1]));

   ice->vtbl.load_register_mem64(batch, CS_GPR(3), q->bo,
                                 offsetof(struct iris_query_so_overflow,
                                          stream[idx].num_prims[0]));
   ice->vtbl.load_register_mem64(batch, CS_GPR(4), q->bo,
                                 offsetof(struct iris_query_so_overflow,
                                          stream[idx].num_prims[1]));
}

/*
 * R3 = R4 - R3;
 * R1 = R2 - R1;
 * R1 = R3 - R1;
 * R0 = R0 | R1;
 */
static void
calc_overflow_for_stream(struct iris_context *ice)
{
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];
   static const uint32_t maths[] = {
      MI_MATH | (17 - 2),
      MI_ALU2(LOAD, SRCA, R4),
      MI_ALU2(LOAD, SRCB, R3),
      MI_ALU0(SUB),
      MI_ALU2(STORE, R3, ACCU),
      MI_ALU2(LOAD, SRCA, R2),
      MI_ALU2(LOAD, SRCB, R1),
      MI_ALU0(SUB),
      MI_ALU2(STORE, R1, ACCU),
      MI_ALU2(LOAD, SRCA, R3),
      MI_ALU2(LOAD, SRCB, R1),
      MI_ALU0(SUB),
      MI_ALU2(STORE, R1, ACCU),
      MI_ALU2(LOAD, SRCA, R1),
      MI_ALU2(LOAD, SRCB, R0),
      MI_ALU0(OR),
      MI_ALU2(STORE, R0, ACCU),
   };

   iris_batch_emit(batch, maths, sizeof(maths));
}

static void
overflow_result_to_gpr0(struct iris_context *ice, struct iris_query *q)
{
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];

   ice->vtbl.load_register_imm64(batch, CS_GPR(0), 0ull);

   if (q->type == PIPE_QUERY_SO_OVERFLOW_PREDICATE) {
      load_overflow_data_to_cs_gprs(ice, q, q->index);
      calc_overflow_for_stream(ice);
   } else {
      for (int i = 0; i < MAX_VERTEX_STREAMS; i++) {
         load_overflow_data_to_cs_gprs(ice, q, i);
         calc_overflow_for_stream(ice);
      }
   }

   gpr0_to_bool(ice);
}

/**
 * Calculate the result and store it to CS_GPR0.
 */
static void
calculate_result_on_gpu(struct iris_context *ice, struct iris_query *q)
{
   struct iris_batch *batch = &ice->batches[q->batch_idx];

   if (q->type == PIPE_QUERY_SO_OVERFLOW_PREDICATE ||
       q->type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE) {
      overflow_result_to_gpr0(ice, q);
      return;
   }

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

   if (q->type == PIPE_QUERY_OCCLUSION_PREDICATE ||
       q->type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE)
      gpr0_to_bool(ice);
}

static struct pipe_query *
iris_create_query(struct pipe_context *ctx,
                  unsigned query_type,
                  unsigned index)
{
   struct iris_query *q = calloc(1, sizeof(struct iris_query));

   q->type = query_type;
   q->index = index;

   if (q->type == PIPE_QUERY_PIPELINE_STATISTICS && q->index == 10)
      q->batch_idx = IRIS_BATCH_COMPUTE;
   else
      q->batch_idx = IRIS_BATCH_RENDER;
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

   q->map = iris_bo_map(&ice->dbg, q->bo, MAP_READ | MAP_WRITE | MAP_ASYNC);
   if (!q->map)
      return false;

   q->result = 0ull;
   q->ready = false;
   q->map->snapshots_landed = false;

   if (q->type == PIPE_QUERY_PRIMITIVES_GENERATED && q->index == 0) {
      ice->state.prims_generated_query_active = true;
      ice->state.dirty |= IRIS_DIRTY_STREAMOUT | IRIS_DIRTY_CLIP;
   }

   if (q->type == PIPE_QUERY_SO_OVERFLOW_PREDICATE ||
       q->type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE)
      write_overflow_values(ice, q, false);
   else
      write_value(ice, q, offsetof(struct iris_query_snapshots, start));

   return true;
}

static bool
iris_end_query(struct pipe_context *ctx, struct pipe_query *query)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_query *q = (void *) query;

   if (q->type == PIPE_QUERY_TIMESTAMP) {
      iris_begin_query(ctx, query);
      mark_available(ice, q);
      return true;
   }

   if (q->type == PIPE_QUERY_PRIMITIVES_GENERATED && q->index == 0) {
      ice->state.prims_generated_query_active = false;
      ice->state.dirty |= IRIS_DIRTY_STREAMOUT | IRIS_DIRTY_CLIP;
   }

   if (q->type == PIPE_QUERY_SO_OVERFLOW_PREDICATE ||
       q->type == PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE)
      write_overflow_values(ice, q, true);
   else
      write_value(ice, q, offsetof(struct iris_query_snapshots, end));
   mark_available(ice, q);

   return true;
}

/**
 * See if the snapshots have landed for a query, and if so, compute the
 * result and mark it ready.  Does not flush (unlike iris_get_query_result).
 */
static void
iris_check_query_no_flush(struct iris_context *ice, struct iris_query *q)
{
   struct iris_screen *screen = (void *) ice->ctx.screen;
   const struct gen_device_info *devinfo = &screen->devinfo;

   if (!q->ready && q->map->snapshots_landed) {
      calculate_result_on_cpu(devinfo, q);
   }
}

static boolean
iris_get_query_result(struct pipe_context *ctx,
                      struct pipe_query *query,
                      boolean wait,
                      union pipe_query_result *result)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_query *q = (void *) query;
   struct iris_screen *screen = (void *) ctx->screen;
   const struct gen_device_info *devinfo = &screen->devinfo;

   if (!q->ready) {
      if (iris_batch_references(&ice->batches[q->batch_idx], q->bo))
         iris_batch_flush(&ice->batches[q->batch_idx]);

      if (!q->map->snapshots_landed) {
         if (wait)
            iris_bo_wait_rendering(q->bo);
         else
            return false;
      }

      assert(q->map->snapshots_landed);
      calculate_result_on_cpu(devinfo, q);
   }

   assert(q->ready);

   if (q->type == PIPE_QUERY_PIPELINE_STATISTICS) {
      switch (q->index) {
      case 0:
         result->pipeline_statistics.ia_vertices = q->result;
         break;
      case 1:
         result->pipeline_statistics.ia_primitives = q->result;
         break;
      case 2:
         result->pipeline_statistics.vs_invocations = q->result;
         break;
      case 3:
         result->pipeline_statistics.gs_invocations = q->result;
         break;
      case 4:
         result->pipeline_statistics.gs_primitives = q->result;
         break;
      case 5:
         result->pipeline_statistics.c_invocations = q->result;
         break;
      case 6:
         result->pipeline_statistics.c_primitives = q->result;
         break;
      case 7:
         result->pipeline_statistics.ps_invocations = q->result;
         break;
      case 8:
         result->pipeline_statistics.hs_invocations = q->result;
         break;
      case 9:
         result->pipeline_statistics.ds_invocations = q->result;
         break;
      case 10:
         result->pipeline_statistics.cs_invocations = q->result;
         break;
      }
   } else {
      result->u64 = q->result;
   }

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
   struct iris_batch *batch = &ice->batches[q->batch_idx];
   const struct gen_device_info *devinfo = &batch->screen->devinfo;
   struct iris_resource *res = (void *) p_res;
   unsigned snapshots_landed_offset =
      offsetof(struct iris_query_snapshots, snapshots_landed);

   res->bind_history |= PIPE_BIND_QUERY_BUFFER;

   if (index == -1) {
      /* They're asking for the availability of the result.  If we still
       * have commands queued up which produce the result, submit them
       * now so that progress happens.  Either way, copy the snapshots
       * landed field to the destination resource.
       */
      if (iris_batch_references(batch, q->bo))
         iris_batch_flush(batch);

      ice->vtbl.copy_mem_mem(batch, iris_resource_bo(p_res), offset,
                             q->bo, snapshots_landed_offset,
                             result_type <= PIPE_QUERY_TYPE_U32 ? 4 : 8);
      return;
   }

   if (!q->ready && q->map->snapshots_landed) {
      /* The final snapshots happen to have landed, so let's just compute
       * the result on the CPU now...
       */
      calculate_result_on_cpu(devinfo, q);
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

      /* Make sure the result lands before they use bind the QBO elsewhere
       * and use the result.
       */
      // XXX: Why?  i965 doesn't do this.
      iris_emit_pipe_control_flush(batch, PIPE_CONTROL_CS_STALL);
      return;
   }

   /* Calculate the result to CS_GPR0 */
   calculate_result_on_gpu(ice, q);

   bool predicated = !wait && !q->stalled;

   if (predicated) {
      ice->vtbl.load_register_imm64(batch, MI_PREDICATE_SRC1, 0ull);
      ice->vtbl.load_register_mem64(batch, MI_PREDICATE_SRC0, q->bo,
                                    snapshots_landed_offset);
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
iris_set_active_query_state(struct pipe_context *ctx, boolean enable)
{
   struct iris_context *ice = (void *) ctx;

   if (ice->state.statistics_counters_enabled == enable)
      return;

   // XXX: most packets aren't paying attention to this yet, because it'd
   // have to be done dynamically at draw time, which is a pain
   ice->state.statistics_counters_enabled = enable;
   ice->state.dirty |= IRIS_DIRTY_CLIP |
                       IRIS_DIRTY_GS |
                       IRIS_DIRTY_RASTER |
                       IRIS_DIRTY_STREAMOUT |
                       IRIS_DIRTY_TCS |
                       IRIS_DIRTY_TES |
                       IRIS_DIRTY_VS |
                       IRIS_DIRTY_WM;
}

static void
set_predicate_enable(struct iris_context *ice, bool value)
{
   if (value)
      ice->state.predicate = IRIS_PREDICATE_STATE_RENDER;
   else
      ice->state.predicate = IRIS_PREDICATE_STATE_DONT_RENDER;
}

static void
set_predicate_for_result(struct iris_context *ice,
                         struct iris_query *q,
                         bool inverted)
{
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];

   /* The CPU doesn't have the query result yet; use hardware predication */
   ice->state.predicate = IRIS_PREDICATE_STATE_USE_BIT;

   /* Ensure the memory is coherent for MI_LOAD_REGISTER_* commands. */
   iris_emit_pipe_control_flush(batch, PIPE_CONTROL_FLUSH_ENABLE);
   q->stalled = true;

   switch (q->type) {
   case PIPE_QUERY_SO_OVERFLOW_PREDICATE:
   case PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE:
      overflow_result_to_gpr0(ice, q);

      ice->vtbl.load_register_reg64(batch, MI_PREDICATE_SRC0, CS_GPR(0));
      ice->vtbl.load_register_imm64(batch, MI_PREDICATE_SRC1, 0ull);
      break;
   default:
      /* PIPE_QUERY_OCCLUSION_* */
      ice->vtbl.load_register_mem64(batch, MI_PREDICATE_SRC0, q->bo,
         offsetof(struct iris_query_snapshots, start));
      ice->vtbl.load_register_mem64(batch, MI_PREDICATE_SRC1, q->bo,
         offsetof(struct iris_query_snapshots, end));
      break;
   }

   uint32_t mi_predicate = MI_PREDICATE |
                           MI_PREDICATE_COMBINEOP_SET |
                           MI_PREDICATE_COMPAREOP_SRCS_EQUAL |
                           (inverted ? MI_PREDICATE_LOADOP_LOAD
                                     : MI_PREDICATE_LOADOP_LOADINV);
   iris_batch_emit(batch, &mi_predicate, sizeof(uint32_t));

   /* We immediately set the predicate on the render batch, as all the
    * counters come from 3D operations.  However, we may need to predicate
    * a compute dispatch, which executes in a different GEM context and has
    * a different MI_PREDICATE_DATA register.  So, we save the result to
    * memory and reload it in iris_launch_grid.
    */
   unsigned offset = offsetof(struct iris_query_snapshots, predicate_data);
   ice->vtbl.store_register_mem64(batch, MI_PREDICATE_DATA,
                                  q->bo, offset, false);
   ice->state.compute_predicate = q->bo;
}

static void
iris_render_condition(struct pipe_context *ctx,
                      struct pipe_query *query,
                      boolean condition,
                      enum pipe_render_cond_flag mode)
{
   struct iris_context *ice = (void *) ctx;
   struct iris_query *q = (void *) query;

   if (!q) {
      ice->state.predicate = IRIS_PREDICATE_STATE_RENDER;
      return;
   }

   iris_check_query_no_flush(ice, q);

   if (q->result || q->ready) {
      set_predicate_enable(ice, (q->result != 0) ^ condition);
   } else {
      if (mode == PIPE_RENDER_COND_NO_WAIT ||
          mode == PIPE_RENDER_COND_BY_REGION_NO_WAIT) {
         perf_debug(&ice->dbg, "Conditional rendering demoted from "
                    "\"no wait\" to \"wait\".");
      }
      set_predicate_for_result(ice, q, condition);
   }
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
   ctx->render_condition = iris_render_condition;
}

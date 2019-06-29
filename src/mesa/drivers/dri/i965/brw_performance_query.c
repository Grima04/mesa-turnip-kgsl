/*
 * Copyright © 2013 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file brw_performance_query.c
 *
 * Implementation of the GL_INTEL_performance_query extension.
 *
 * Currently there are two possible counter sources exposed here:
 *
 * On Gen6+ hardware we have numerous 64bit Pipeline Statistics Registers
 * that we can snapshot at the beginning and end of a query.
 *
 * On Gen7.5+ we have Observability Architecture counters which are
 * covered in separate document from the rest of the PRMs.  It is available at:
 * https://01.org/linuxgraphics/documentation/driver-documentation-prms
 * => 2013 Intel Core Processor Family => Observability Performance Counters
 * (This one volume covers Sandybridge, Ivybridge, Baytrail, and Haswell,
 * though notably we currently only support OA counters for Haswell+)
 */

#include <limits.h>

/* put before sys/types.h to silence glibc warnings */
#ifdef MAJOR_IN_MKDEV
#include <sys/mkdev.h>
#endif
#ifdef MAJOR_IN_SYSMACROS
#include <sys/sysmacros.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include "drm-uapi/i915_drm.h"

#include "main/hash.h"
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/performance_query.h"

#include "util/bitset.h"
#include "util/ralloc.h"
#include "util/hash_table.h"
#include "util/list.h"
#include "util/u_math.h"

#include "brw_context.h"
#include "brw_defines.h"
#include "intel_batchbuffer.h"

#include "perf/gen_perf.h"
#include "perf/gen_perf_mdapi.h"

#define FILE_DEBUG_FLAG DEBUG_PERFMON

#define OAREPORT_REASON_MASK           0x3f
#define OAREPORT_REASON_SHIFT          19
#define OAREPORT_REASON_TIMER          (1<<0)
#define OAREPORT_REASON_TRIGGER1       (1<<1)
#define OAREPORT_REASON_TRIGGER2       (1<<2)
#define OAREPORT_REASON_CTX_SWITCH     (1<<3)
#define OAREPORT_REASON_GO_TRANSITION  (1<<4)

struct brw_perf_query_object {
   struct gl_perf_query_object base;
   struct gen_perf_query_object *query;
};

/** Downcasting convenience macro. */
static inline struct brw_perf_query_object *
brw_perf_query(struct gl_perf_query_object *o)
{
   return (struct brw_perf_query_object *) o;
}

#define MI_RPC_BO_SIZE              4096
#define MI_RPC_BO_END_OFFSET_BYTES  (MI_RPC_BO_SIZE / 2)
#define MI_FREQ_START_OFFSET_BYTES  (3072)
#define MI_FREQ_END_OFFSET_BYTES    (3076)

/******************************************************************************/

static bool
brw_is_perf_query_ready(struct gl_context *ctx,
                        struct gl_perf_query_object *o);

static void
dump_perf_query_callback(GLuint id, void *query_void, void *brw_void)
{
   struct gl_context *ctx = brw_void;
   struct gl_perf_query_object *o = query_void;
   struct brw_perf_query_object * brw_query = brw_perf_query(o);
   struct gen_perf_query_object *obj = brw_query->query;

   switch (obj->queryinfo->kind) {
   case GEN_PERF_QUERY_TYPE_OA:
   case GEN_PERF_QUERY_TYPE_RAW:
      DBG("%4d: %-6s %-8s BO: %-4s OA data: %-10s %-15s\n",
          id,
          o->Used ? "Dirty," : "New,",
          o->Active ? "Active," : (o->Ready ? "Ready," : "Pending,"),
          obj->oa.bo ? "yes," : "no,",
          brw_is_perf_query_ready(ctx, o) ? "ready," : "not ready,",
          obj->oa.results_accumulated ? "accumulated" : "not accumulated");
      break;
   case GEN_PERF_QUERY_TYPE_PIPELINE:
      DBG("%4d: %-6s %-8s BO: %-4s\n",
          id,
          o->Used ? "Dirty," : "New,",
          o->Active ? "Active," : (o->Ready ? "Ready," : "Pending,"),
          obj->pipeline_stats.bo ? "yes" : "no");
      break;
   default:
      unreachable("Unknown query type");
      break;
   }
}

static void
dump_perf_queries(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   DBG("Queries: (Open queries = %d, OA users = %d)\n",
       brw->perf_ctx.n_active_oa_queries, brw->perf_ctx.n_oa_users);
   _mesa_HashWalk(ctx->PerfQuery.Objects, dump_perf_query_callback, brw);
}

/**
 * Driver hook for glGetPerfQueryInfoINTEL().
 */
static void
brw_get_perf_query_info(struct gl_context *ctx,
                        unsigned query_index,
                        const char **name,
                        GLuint *data_size,
                        GLuint *n_counters,
                        GLuint *n_active)
{
   struct brw_context *brw = brw_context(ctx);
   struct gen_perf_context *perf_ctx = &brw->perf_ctx;
   const struct gen_perf_query_info *query =
      &perf_ctx->perf->queries[query_index];

   *name = query->name;
   *data_size = query->data_size;
   *n_counters = query->n_counters;

   switch (query->kind) {
   case GEN_PERF_QUERY_TYPE_OA:
   case GEN_PERF_QUERY_TYPE_RAW:
      *n_active = perf_ctx->n_active_oa_queries;
      break;

   case GEN_PERF_QUERY_TYPE_PIPELINE:
      *n_active = perf_ctx->n_active_pipeline_stats_queries;
      break;

   default:
      unreachable("Unknown query type");
      break;
   }
}

static GLuint
gen_counter_type_enum_to_gl_type(enum gen_perf_counter_type type)
{
   switch (type) {
   case GEN_PERF_COUNTER_TYPE_EVENT: return GL_PERFQUERY_COUNTER_EVENT_INTEL;
   case GEN_PERF_COUNTER_TYPE_DURATION_NORM: return GL_PERFQUERY_COUNTER_DURATION_NORM_INTEL;
   case GEN_PERF_COUNTER_TYPE_DURATION_RAW: return GL_PERFQUERY_COUNTER_DURATION_RAW_INTEL;
   case GEN_PERF_COUNTER_TYPE_THROUGHPUT: return GL_PERFQUERY_COUNTER_THROUGHPUT_INTEL;
   case GEN_PERF_COUNTER_TYPE_RAW: return GL_PERFQUERY_COUNTER_RAW_INTEL;
   case GEN_PERF_COUNTER_TYPE_TIMESTAMP: return GL_PERFQUERY_COUNTER_TIMESTAMP_INTEL;
   default:
      unreachable("Unknown counter type");
   }
}

static GLuint
gen_counter_data_type_to_gl_type(enum gen_perf_counter_data_type type)
{
   switch (type) {
   case GEN_PERF_COUNTER_DATA_TYPE_BOOL32: return GL_PERFQUERY_COUNTER_DATA_BOOL32_INTEL;
   case GEN_PERF_COUNTER_DATA_TYPE_UINT32: return GL_PERFQUERY_COUNTER_DATA_UINT32_INTEL;
   case GEN_PERF_COUNTER_DATA_TYPE_UINT64: return GL_PERFQUERY_COUNTER_DATA_UINT64_INTEL;
   case GEN_PERF_COUNTER_DATA_TYPE_FLOAT: return GL_PERFQUERY_COUNTER_DATA_FLOAT_INTEL;
   case GEN_PERF_COUNTER_DATA_TYPE_DOUBLE: return GL_PERFQUERY_COUNTER_DATA_DOUBLE_INTEL;
   default:
      unreachable("Unknown counter data type");
   }
}

/**
 * Driver hook for glGetPerfCounterInfoINTEL().
 */
static void
brw_get_perf_counter_info(struct gl_context *ctx,
                          unsigned query_index,
                          unsigned counter_index,
                          const char **name,
                          const char **desc,
                          GLuint *offset,
                          GLuint *data_size,
                          GLuint *type_enum,
                          GLuint *data_type_enum,
                          GLuint64 *raw_max)
{
   struct brw_context *brw = brw_context(ctx);
   const struct gen_perf_query_info *query =
      &brw->perf_ctx.perf->queries[query_index];
   const struct gen_perf_query_counter *counter =
      &query->counters[counter_index];

   *name = counter->name;
   *desc = counter->desc;
   *offset = counter->offset;
   *data_size = gen_perf_query_counter_get_size(counter);
   *type_enum = gen_counter_type_enum_to_gl_type(counter->type);
   *data_type_enum = gen_counter_data_type_to_gl_type(counter->data_type);
   *raw_max = counter->raw_max;
}

enum OaReadStatus {
   OA_READ_STATUS_ERROR,
   OA_READ_STATUS_UNFINISHED,
   OA_READ_STATUS_FINISHED,
};

/******************************************************************************/

static void
capture_frequency_stat_register(struct brw_context *brw,
                                struct brw_bo *bo,
                                uint32_t bo_offset)
{
   const struct gen_device_info *devinfo = &brw->screen->devinfo;

   if (devinfo->gen >= 7 && devinfo->gen <= 8 &&
       !devinfo->is_baytrail && !devinfo->is_cherryview) {
      brw_store_register_mem32(brw, bo, GEN7_RPSTAT1, bo_offset);
   } else if (devinfo->gen >= 9) {
      brw_store_register_mem32(brw, bo, GEN9_RPSTAT0, bo_offset);
   }
}

/**
 * Driver hook for glBeginPerfQueryINTEL().
 */
static bool
brw_begin_perf_query(struct gl_context *ctx,
                     struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *brw_query = brw_perf_query(o);
   struct gen_perf_query_object *obj = brw_query->query;
   struct gen_perf_context *perf_ctx = &brw->perf_ctx;

   /* We can assume the frontend hides mistaken attempts to Begin a
    * query object multiple times before its End. Similarly if an
    * application reuses a query object before results have arrived
    * the frontend will wait for prior results so we don't need
    * to support abandoning in-flight results.
    */
   assert(!o->Active);
   assert(!o->Used || o->Ready); /* no in-flight query to worry about */

   DBG("Begin(%d)\n", o->Id);

   gen_perf_begin_query(perf_ctx, obj);

   if (INTEL_DEBUG & DEBUG_PERFMON)
      dump_perf_queries(brw);

   return true;
}

/**
 * Driver hook for glEndPerfQueryINTEL().
 */
static void
brw_end_perf_query(struct gl_context *ctx,
                     struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *brw_query = brw_perf_query(o);
   struct gen_perf_query_object *obj = brw_query->query;
   struct gen_perf_context *perf_ctx = &brw->perf_ctx;

   DBG("End(%d)\n", o->Id);
   gen_perf_end_query(perf_ctx, obj);
}

static void
brw_wait_perf_query(struct gl_context *ctx, struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *brw_query = brw_perf_query(o);
   struct gen_perf_query_object *obj = brw_query->query;

   assert(!o->Ready);

   gen_perf_wait_query(&brw->perf_ctx, obj, &brw->batch);
}

static bool
brw_is_perf_query_ready(struct gl_context *ctx,
                        struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *brw_query = brw_perf_query(o);
   struct gen_perf_query_object *obj = brw_query->query;

   if (o->Ready)
      return true;

   return gen_perf_is_query_ready(&brw->perf_ctx, obj, &brw->batch);
}

/**
 * Driver hook for glGetPerfQueryDataINTEL().
 */
static void
brw_get_perf_query_data(struct gl_context *ctx,
                        struct gl_perf_query_object *o,
                        GLsizei data_size,
                        GLuint *data,
                        GLuint *bytes_written)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *brw_query = brw_perf_query(o);
   struct gen_perf_query_object *obj = brw_query->query;

   assert(brw_is_perf_query_ready(ctx, o));

   DBG("GetData(%d)\n", o->Id);

   if (INTEL_DEBUG & DEBUG_PERFMON)
      dump_perf_queries(brw);

   /* We expect that the frontend only calls this hook when it knows
    * that results are available.
    */
   assert(o->Ready);

   gen_perf_get_query_data(&brw->perf_ctx, obj,
                           data_size, data, bytes_written);
}

static struct gl_perf_query_object *
brw_new_perf_query_object(struct gl_context *ctx, unsigned query_index)
{
   struct brw_context *brw = brw_context(ctx);
   struct gen_perf_context *perf_ctx = &brw->perf_ctx;
   const struct gen_perf_query_info *queryinfo =
      &perf_ctx->perf->queries[query_index];
   struct gen_perf_query_object *obj =
      calloc(1, sizeof(struct gen_perf_query_object));

   if (!obj)
      return NULL;

   obj->queryinfo = queryinfo;

   perf_ctx->n_query_instances++;

   struct brw_perf_query_object *brw_query = calloc(1, sizeof(struct brw_perf_query_object));
   if (unlikely(!brw_query))
      return NULL;
   brw_query->query = obj;
   return &brw_query->base;
}

/**
 * Driver hook for glDeletePerfQueryINTEL().
 */
static void
brw_delete_perf_query(struct gl_context *ctx,
                      struct gl_perf_query_object *o)
{
   struct brw_context *brw = brw_context(ctx);
   struct brw_perf_query_object *brw_query = brw_perf_query(o);
   struct gen_perf_query_object *obj = brw_query->query;
   struct gen_perf_context *perf_ctx = &brw->perf_ctx;

   /* We can assume that the frontend waits for a query to complete
    * before ever calling into here, so we don't have to worry about
    * deleting an in-flight query object.
    */
   assert(!o->Active);
   assert(!o->Used || o->Ready);

   DBG("Delete(%d)\n", o->Id);

   gen_perf_delete_query(perf_ctx, obj);
   free(brw_query);
}

/******************************************************************************/

static void
init_pipeline_statistic_query_registers(struct brw_context *brw)
{
   const struct gen_device_info *devinfo = &brw->screen->devinfo;
   struct gen_perf_config *perf = brw->perf_ctx.perf;
   struct gen_perf_query_info *query =
      gen_perf_query_append_query_info(perf, MAX_STAT_COUNTERS);

   query->kind = GEN_PERF_QUERY_TYPE_PIPELINE;
   query->name = "Pipeline Statistics Registers";

   gen_perf_query_info_add_basic_stat_reg(query, IA_VERTICES_COUNT,
                                            "N vertices submitted");
   gen_perf_query_info_add_basic_stat_reg(query, IA_PRIMITIVES_COUNT,
                                            "N primitives submitted");
   gen_perf_query_info_add_basic_stat_reg(query, VS_INVOCATION_COUNT,
                                            "N vertex shader invocations");

   if (devinfo->gen == 6) {
      gen_perf_query_info_add_stat_reg(query, GEN6_SO_PRIM_STORAGE_NEEDED, 1, 1,
                                       "SO_PRIM_STORAGE_NEEDED",
                                       "N geometry shader stream-out primitives (total)");
      gen_perf_query_info_add_stat_reg(query, GEN6_SO_NUM_PRIMS_WRITTEN, 1, 1,
                                       "SO_NUM_PRIMS_WRITTEN",
                                       "N geometry shader stream-out primitives (written)");
   } else {
      gen_perf_query_info_add_stat_reg(query, GEN7_SO_PRIM_STORAGE_NEEDED(0), 1, 1,
                                       "SO_PRIM_STORAGE_NEEDED (Stream 0)",
                                       "N stream-out (stream 0) primitives (total)");
      gen_perf_query_info_add_stat_reg(query, GEN7_SO_PRIM_STORAGE_NEEDED(1), 1, 1,
                                       "SO_PRIM_STORAGE_NEEDED (Stream 1)",
                                       "N stream-out (stream 1) primitives (total)");
      gen_perf_query_info_add_stat_reg(query, GEN7_SO_PRIM_STORAGE_NEEDED(2), 1, 1,
                                       "SO_PRIM_STORAGE_NEEDED (Stream 2)",
                                       "N stream-out (stream 2) primitives (total)");
      gen_perf_query_info_add_stat_reg(query, GEN7_SO_PRIM_STORAGE_NEEDED(3), 1, 1,
                                       "SO_PRIM_STORAGE_NEEDED (Stream 3)",
                                       "N stream-out (stream 3) primitives (total)");
      gen_perf_query_info_add_stat_reg(query, GEN7_SO_NUM_PRIMS_WRITTEN(0), 1, 1,
                                       "SO_NUM_PRIMS_WRITTEN (Stream 0)",
                                       "N stream-out (stream 0) primitives (written)");
      gen_perf_query_info_add_stat_reg(query, GEN7_SO_NUM_PRIMS_WRITTEN(1), 1, 1,
                                       "SO_NUM_PRIMS_WRITTEN (Stream 1)",
                                       "N stream-out (stream 1) primitives (written)");
      gen_perf_query_info_add_stat_reg(query, GEN7_SO_NUM_PRIMS_WRITTEN(2), 1, 1,
                                       "SO_NUM_PRIMS_WRITTEN (Stream 2)",
                                       "N stream-out (stream 2) primitives (written)");
      gen_perf_query_info_add_stat_reg(query, GEN7_SO_NUM_PRIMS_WRITTEN(3), 1, 1,
                                       "SO_NUM_PRIMS_WRITTEN (Stream 3)",
                                       "N stream-out (stream 3) primitives (written)");
   }

   gen_perf_query_info_add_basic_stat_reg(query, HS_INVOCATION_COUNT,
                                          "N TCS shader invocations");
   gen_perf_query_info_add_basic_stat_reg(query, DS_INVOCATION_COUNT,
                                          "N TES shader invocations");

   gen_perf_query_info_add_basic_stat_reg(query, GS_INVOCATION_COUNT,
                                          "N geometry shader invocations");
   gen_perf_query_info_add_basic_stat_reg(query, GS_PRIMITIVES_COUNT,
                                          "N geometry shader primitives emitted");

   gen_perf_query_info_add_basic_stat_reg(query, CL_INVOCATION_COUNT,
                                          "N primitives entering clipping");
   gen_perf_query_info_add_basic_stat_reg(query, CL_PRIMITIVES_COUNT,
                                          "N primitives leaving clipping");

   if (devinfo->is_haswell || devinfo->gen == 8) {
      gen_perf_query_info_add_stat_reg(query, PS_INVOCATION_COUNT, 1, 4,
                                       "N fragment shader invocations",
                                       "N fragment shader invocations");
   } else {
      gen_perf_query_info_add_basic_stat_reg(query, PS_INVOCATION_COUNT,
                                             "N fragment shader invocations");
   }

   gen_perf_query_info_add_basic_stat_reg(query, PS_DEPTH_COUNT,
                                          "N z-pass fragments");

   if (devinfo->gen >= 7) {
      gen_perf_query_info_add_basic_stat_reg(query, CS_INVOCATION_COUNT,
                                             "N compute shader invocations");
   }

   query->data_size = sizeof(uint64_t) * query->n_counters;
}

/* gen_device_info will have incorrect default topology values for unsupported kernels.
 * verify kernel support to ensure OA metrics are accurate.
 */
static bool
oa_metrics_kernel_support(int fd, const struct gen_device_info *devinfo)
{
   if (devinfo->gen >= 10) {
      /* topology uAPI required for CNL+ (kernel 4.17+) make a call to the api
       * to verify support
       */
      struct drm_i915_query_item item = {
         .query_id = DRM_I915_QUERY_TOPOLOGY_INFO,
      };
      struct drm_i915_query query = {
         .num_items = 1,
         .items_ptr = (uintptr_t) &item,
      };

      /* kernel 4.17+ supports the query */
      return drmIoctl(fd, DRM_IOCTL_I915_QUERY, &query) == 0;
   }

   if (devinfo->gen >= 8) {
      /* 4.13+ api required for gen8 - gen9 */
      int mask;
      struct drm_i915_getparam gp = {
         .param = I915_PARAM_SLICE_MASK,
         .value = &mask,
      };
      /* kernel 4.13+ supports this parameter */
      return drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) == 0;
   }

   if (devinfo->gen == 7)
      /* default topology values are correct for HSW */
      return true;

   /* oa not supported before gen 7*/
   return false;
}

static void *
brw_oa_bo_alloc(void *bufmgr, const char *name, uint64_t size)
{
   return brw_bo_alloc(bufmgr, name, size, BRW_MEMZONE_OTHER);
}

static void
brw_oa_emit_mi_report_perf_count(void *c,
                                 void *bo,
                                 uint32_t offset_in_bytes,
                                 uint32_t report_id)
{
   struct brw_context *ctx = c;
   ctx->vtbl.emit_mi_report_perf_count(ctx,
                                       bo,
                                       offset_in_bytes,
                                       report_id);
}

typedef void (*bo_unreference_t)(void *);
typedef void *(*bo_map_t)(void *, void *, unsigned flags);
typedef void (*bo_unmap_t)(void *);
typedef void (* emit_mi_report_t)(void *, void *, uint32_t, uint32_t);
typedef void (*emit_mi_flush_t)(void *);

static void
brw_oa_batchbuffer_flush(void *c, const char *file, int line)
{
   struct brw_context *ctx = c;
   _intel_batchbuffer_flush_fence(ctx, -1, NULL, file,  line);
}

typedef void (*capture_frequency_stat_register_t)(void *, void *, uint32_t );
typedef void (*store_register_mem64_t)(void *ctx, void *bo,
                                       uint32_t reg, uint32_t offset);
typedef bool (*batch_references_t)(void *batch, void *bo);
typedef void (*bo_wait_rendering_t)(void *bo);
typedef int (*bo_busy_t)(void *bo);

static unsigned
brw_init_perf_query_info(struct gl_context *ctx)
{
   struct brw_context *brw = brw_context(ctx);
   const struct gen_device_info *devinfo = &brw->screen->devinfo;

   struct gen_perf_context *perf_ctx = &brw->perf_ctx;
   if (perf_ctx->perf)
      return perf_ctx->perf->n_queries;

   perf_ctx->perf = gen_perf_new(brw);
   struct gen_perf_config *perf_cfg = perf_ctx->perf;

   perf_cfg->vtbl.bo_alloc = brw_oa_bo_alloc;
   perf_cfg->vtbl.bo_unreference = (bo_unreference_t)brw_bo_unreference;
   perf_cfg->vtbl.bo_map = (bo_map_t)brw_bo_map;
   perf_cfg->vtbl.bo_unmap = (bo_unmap_t)brw_bo_unmap;
   perf_cfg->vtbl.emit_mi_flush = (emit_mi_flush_t)brw_emit_mi_flush;
   perf_cfg->vtbl.emit_mi_report_perf_count =
      (emit_mi_report_t)brw_oa_emit_mi_report_perf_count;
   perf_cfg->vtbl.batchbuffer_flush = brw_oa_batchbuffer_flush;
   perf_cfg->vtbl.capture_frequency_stat_register =
      (capture_frequency_stat_register_t) capture_frequency_stat_register;
   perf_cfg->vtbl.store_register_mem64 =
      (store_register_mem64_t) brw_store_register_mem64;
   perf_cfg->vtbl.batch_references = (batch_references_t)brw_batch_references;
   perf_cfg->vtbl.bo_wait_rendering = (bo_wait_rendering_t)brw_bo_wait_rendering;
   perf_cfg->vtbl.bo_busy = (bo_busy_t)brw_bo_busy;

   gen_perf_init_context(perf_ctx, perf_cfg, brw, brw->bufmgr, devinfo,
                         brw->hw_ctx, brw->screen->driScrnPriv->fd);

   init_pipeline_statistic_query_registers(brw);
   gen_perf_query_register_mdapi_statistic_query(devinfo, perf_cfg);

   if ((oa_metrics_kernel_support(perf_ctx->drm_fd, devinfo)) &&
       (gen_perf_load_oa_metrics(perf_cfg, perf_ctx->drm_fd, devinfo)))
      gen_perf_query_register_mdapi_oa_query(devinfo, perf_cfg);

   return perf_cfg->n_queries;
}

void
brw_init_performance_queries(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;

   ctx->Driver.InitPerfQueryInfo = brw_init_perf_query_info;
   ctx->Driver.GetPerfQueryInfo = brw_get_perf_query_info;
   ctx->Driver.GetPerfCounterInfo = brw_get_perf_counter_info;
   ctx->Driver.NewPerfQueryObject = brw_new_perf_query_object;
   ctx->Driver.DeletePerfQuery = brw_delete_perf_query;
   ctx->Driver.BeginPerfQuery = brw_begin_perf_query;
   ctx->Driver.EndPerfQuery = brw_end_perf_query;
   ctx->Driver.WaitPerfQuery = brw_wait_perf_query;
   ctx->Driver.IsPerfQueryReady = brw_is_perf_query_ready;
   ctx->Driver.GetPerfQueryData = brw_get_perf_query_data;
}

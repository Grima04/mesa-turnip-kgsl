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

#ifndef GEN_PERF_H
#define GEN_PERF_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <sys/sysmacros.h>

#include "util/hash_table.h"
#include "compiler/glsl/list.h"
#include "util/ralloc.h"

struct gen_device_info;

struct gen_perf_config;
struct gen_perf_query_info;

enum gen_perf_counter_type {
   GEN_PERF_COUNTER_TYPE_EVENT,
   GEN_PERF_COUNTER_TYPE_DURATION_NORM,
   GEN_PERF_COUNTER_TYPE_DURATION_RAW,
   GEN_PERF_COUNTER_TYPE_THROUGHPUT,
   GEN_PERF_COUNTER_TYPE_RAW,
   GEN_PERF_COUNTER_TYPE_TIMESTAMP,
};

enum gen_perf_counter_data_type {
   GEN_PERF_COUNTER_DATA_TYPE_BOOL32,
   GEN_PERF_COUNTER_DATA_TYPE_UINT32,
   GEN_PERF_COUNTER_DATA_TYPE_UINT64,
   GEN_PERF_COUNTER_DATA_TYPE_FLOAT,
   GEN_PERF_COUNTER_DATA_TYPE_DOUBLE,
};

struct gen_pipeline_stat {
   uint32_t reg;
   uint32_t numerator;
   uint32_t denominator;
};

/*
 * The largest OA formats we can use include:
 * For Haswell:
 *   1 timestamp, 45 A counters, 8 B counters and 8 C counters.
 * For Gen8+
 *   1 timestamp, 1 clock, 36 A counters, 8 B counters and 8 C counters
 */
#define MAX_OA_REPORT_COUNTERS 62

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

/*
 * When currently allocate only one page for pipeline statistics queries. Here
 * we derived the maximum number of counters for that amount.
 */
#define STATS_BO_SIZE               4096
#define STATS_BO_END_OFFSET_BYTES   (STATS_BO_SIZE / 2)
#define MAX_STAT_COUNTERS           (STATS_BO_END_OFFSET_BYTES / 8)

#define I915_PERF_OA_SAMPLE_SIZE (8 +   /* drm_i915_perf_record_header */ \
                                  256)  /* OA counter report */

struct gen_perf_query_result {
   /**
    * Storage for the final accumulated OA counters.
    */
   uint64_t accumulator[MAX_OA_REPORT_COUNTERS];

   /**
    * Hw ID used by the context on which the query was running.
    */
   uint32_t hw_id;

   /**
    * Number of reports accumulated to produce the results.
    */
   uint32_t reports_accumulated;

   /**
    * Frequency in the slices of the GT at the begin and end of the
    * query.
    */
   uint64_t slice_frequency[2];

   /**
    * Frequency in the unslice of the GT at the begin and end of the
    * query.
    */
   uint64_t unslice_frequency[2];
};

struct gen_perf_query_counter {
   const char *name;
   const char *desc;
   enum gen_perf_counter_type type;
   enum gen_perf_counter_data_type data_type;
   uint64_t raw_max;
   size_t offset;

   union {
      uint64_t (*oa_counter_read_uint64)(struct gen_perf_config *perf,
                                         const struct gen_perf_query_info *query,
                                         const uint64_t *accumulator);
      float (*oa_counter_read_float)(struct gen_perf_config *perf,
                                     const struct gen_perf_query_info *query,
                                     const uint64_t *accumulator);
      struct gen_pipeline_stat pipeline_stat;
   };
};

struct gen_perf_query_register_prog {
   uint32_t reg;
   uint32_t val;
};

struct gen_perf_query_info {
   enum gen_perf_query_type {
      GEN_PERF_QUERY_TYPE_OA,
      GEN_PERF_QUERY_TYPE_RAW,
      GEN_PERF_QUERY_TYPE_PIPELINE,
   } kind;
   const char *name;
   const char *guid;
   struct gen_perf_query_counter *counters;
   int n_counters;
   int max_counters;
   size_t data_size;

   /* OA specific */
   uint64_t oa_metrics_set_id;
   int oa_format;

   /* For indexing into the accumulator[] ... */
   int gpu_time_offset;
   int gpu_clock_offset;
   int a_offset;
   int b_offset;
   int c_offset;

   /* Register programming for a given query */
   struct gen_perf_query_register_prog *flex_regs;
   uint32_t n_flex_regs;

   struct gen_perf_query_register_prog *mux_regs;
   uint32_t n_mux_regs;

   struct gen_perf_query_register_prog *b_counter_regs;
   uint32_t n_b_counter_regs;
};

struct gen_perf_config {
   struct gen_perf_query_info *queries;
   int n_queries;

   /* Variables referenced in the XML meta data for OA performance
    * counters, e.g in the normalization equations.
    *
    * All uint64_t for consistent operand types in generated code
    */
   struct {
      uint64_t timestamp_frequency; /** $GpuTimestampFrequency */
      uint64_t n_eus;               /** $EuCoresTotalCount */
      uint64_t n_eu_slices;         /** $EuSlicesTotalCount */
      uint64_t n_eu_sub_slices;     /** $EuSubslicesTotalCount */
      uint64_t eu_threads_count;    /** $EuThreadsCount */
      uint64_t slice_mask;          /** $SliceMask */
      uint64_t subslice_mask;       /** $SubsliceMask */
      uint64_t gt_min_freq;         /** $GpuMinFrequency */
      uint64_t gt_max_freq;         /** $GpuMaxFrequency */
      uint64_t revision;            /** $SkuRevisionId */
   } sys_vars;

   /* OA metric sets, indexed by GUID, as know by Mesa at build time, to
    * cross-reference with the GUIDs of configs advertised by the kernel at
    * runtime
    */
   struct hash_table *oa_metrics_table;

   /* Location of the device's sysfs entry. */
   char sysfs_dev_dir[256];

   struct {
      void *(*bo_alloc)(void *bufmgr, const char *name, uint64_t size);
      void (*bo_unreference)(void *bo);
      void *(*bo_map)(void *ctx, void *bo, unsigned flags);
      void (*bo_unmap)(void *bo);
      bool (*batch_references)(void *batch, void *bo);
      void (*bo_wait_rendering)(void *bo);
      int (*bo_busy)(void *bo);
      void (*emit_mi_flush)(void *ctx);
      void (*emit_mi_report_perf_count)(void *ctx,
                                        void *bo,
                                        uint32_t offset_in_bytes,
                                        uint32_t report_id);
      void (*batchbuffer_flush)(void *ctx,
                                const char *file, int line);
      void (*capture_frequency_stat_register)(void *ctx, void *bo,
                                              uint32_t bo_offset);
      void (*store_register_mem64)(void *ctx, void *bo, uint32_t reg, uint32_t offset);

   } vtbl;
};

/**
 * Periodic OA samples are read() into these buffer structures via the
 * i915 perf kernel interface and appended to the
 * brw->perfquery.sample_buffers linked list. When we process the
 * results of an OA metrics query we need to consider all the periodic
 * samples between the Begin and End MI_REPORT_PERF_COUNT command
 * markers.
 *
 * 'Periodic' is a simplification as there are other automatic reports
 * written by the hardware also buffered here.
 *
 * Considering three queries, A, B and C:
 *
 *  Time ---->
 *                ________________A_________________
 *                |                                |
 *                | ________B_________ _____C___________
 *                | |                | |           |   |
 *
 * And an illustration of sample buffers read over this time frame:
 * [HEAD ][     ][     ][     ][     ][     ][     ][     ][TAIL ]
 *
 * These nodes may hold samples for query A:
 * [     ][     ][  A  ][  A  ][  A  ][  A  ][  A  ][     ][     ]
 *
 * These nodes may hold samples for query B:
 * [     ][     ][  B  ][  B  ][  B  ][     ][     ][     ][     ]
 *
 * These nodes may hold samples for query C:
 * [     ][     ][     ][     ][     ][  C  ][  C  ][  C  ][     ]
 *
 * The illustration assumes we have an even distribution of periodic
 * samples so all nodes have the same size plotted against time:
 *
 * Note, to simplify code, the list is never empty.
 *
 * With overlapping queries we can see that periodic OA reports may
 * relate to multiple queries and care needs to be take to keep
 * track of sample buffers until there are no queries that might
 * depend on their contents.
 *
 * We use a node ref counting system where a reference ensures that a
 * node and all following nodes can't be freed/recycled until the
 * reference drops to zero.
 *
 * E.g. with a ref of one here:
 * [  0  ][  0  ][  1  ][  0  ][  0  ][  0  ][  0  ][  0  ][  0  ]
 *
 * These nodes could be freed or recycled ("reaped"):
 * [  0  ][  0  ]
 *
 * These must be preserved until the leading ref drops to zero:
 *               [  1  ][  0  ][  0  ][  0  ][  0  ][  0  ][  0  ]
 *
 * When a query starts we take a reference on the current tail of
 * the list, knowing that no already-buffered samples can possibly
 * relate to the newly-started query. A pointer to this node is
 * also saved in the query object's ->oa.samples_head.
 *
 * E.g. starting query A while there are two nodes in .sample_buffers:
 *                ________________A________
 *                |
 *
 * [  0  ][  1  ]
 *           ^_______ Add a reference and store pointer to node in
 *                    A->oa.samples_head
 *
 * Moving forward to when the B query starts with no new buffer nodes:
 * (for reference, i915 perf reads() are only done when queries finish)
 *                ________________A_______
 *                | ________B___
 *                | |
 *
 * [  0  ][  2  ]
 *           ^_______ Add a reference and store pointer to
 *                    node in B->oa.samples_head
 *
 * Once a query is finished, after an OA query has become 'Ready',
 * once the End OA report has landed and after we we have processed
 * all the intermediate periodic samples then we drop the
 * ->oa.samples_head reference we took at the start.
 *
 * So when the B query has finished we have:
 *                ________________A________
 *                | ______B___________
 *                | |                |
 * [  0  ][  1  ][  0  ][  0  ][  0  ]
 *           ^_______ Drop B->oa.samples_head reference
 *
 * We still can't free these due to the A->oa.samples_head ref:
 *        [  1  ][  0  ][  0  ][  0  ]
 *
 * When the A query finishes: (note there's a new ref for C's samples_head)
 *                ________________A_________________
 *                |                                |
 *                |                    _____C_________
 *                |                    |           |
 * [  0  ][  0  ][  0  ][  0  ][  1  ][  0  ][  0  ]
 *           ^_______ Drop A->oa.samples_head reference
 *
 * And we can now reap these nodes up to the C->oa.samples_head:
 * [  X  ][  X  ][  X  ][  X  ]
 *                  keeping -> [  1  ][  0  ][  0  ]
 *
 * We reap old sample buffers each time we finish processing an OA
 * query by iterating the sample_buffers list from the head until we
 * find a referenced node and stop.
 *
 * Reaped buffers move to a perfquery.free_sample_buffers list and
 * when we come to read() we first look to recycle a buffer from the
 * free_sample_buffers list before allocating a new buffer.
 */
struct oa_sample_buf {
   struct exec_node link;
   int refcount;
   int len;
   uint8_t buf[I915_PERF_OA_SAMPLE_SIZE * 10];
   uint32_t last_timestamp;
};

/**
 * gen representation of a performance query object.
 *
 * NB: We want to keep this structure relatively lean considering that
 * applications may expect to allocate enough objects to be able to
 * query around all draw calls in a frame.
 */
struct gen_perf_query_object
{
   const struct gen_perf_query_info *queryinfo;

   /* See query->kind to know which state below is in use... */
   union {
      struct {

         /**
          * BO containing OA counter snapshots at query Begin/End time.
          */
         void *bo;

         /**
          * Address of mapped of @bo
          */
         void *map;

         /**
          * The MI_REPORT_PERF_COUNT command lets us specify a unique
          * ID that will be reflected in the resulting OA report
          * that's written by the GPU. This is the ID we're expecting
          * in the begin report and the the end report should be
          * @begin_report_id + 1.
          */
         int begin_report_id;

         /**
          * Reference the head of the brw->perfquery.sample_buffers
          * list at the time that the query started (so we only need
          * to look at nodes after this point when looking for samples
          * related to this query)
          *
          * (See struct brw_oa_sample_buf description for more details)
          */
         struct exec_node *samples_head;

         /**
          * false while in the unaccumulated_elements list, and set to
          * true when the final, end MI_RPC snapshot has been
          * accumulated.
          */
         bool results_accumulated;

         /**
          * Frequency of the GT at begin and end of the query.
          */
         uint64_t gt_frequency[2];

         /**
          * Accumulated OA results between begin and end of the query.
          */
         struct gen_perf_query_result result;
      } oa;

      struct {
         /**
          * BO containing starting and ending snapshots for the
          * statistics counters.
          */
         void *bo;
      } pipeline_stats;
   };
};

struct gen_perf_context {
   struct gen_perf_config *perf;

   void * ctx;  /* driver context (eg, brw_context) */
   void * bufmgr;
   const struct gen_device_info *devinfo;

   uint32_t hw_ctx;
   int drm_fd;

   /* The i915 perf stream we open to setup + enable the OA counters */
   int oa_stream_fd;

   /* An i915 perf stream fd gives exclusive access to the OA unit that will
    * report counter snapshots for a specific counter set/profile in a
    * specific layout/format so we can only start OA queries that are
    * compatible with the currently open fd...
    */
   int current_oa_metrics_set_id;
   int current_oa_format;

   /* List of buffers containing OA reports */
   struct exec_list sample_buffers;

   /* Cached list of empty sample buffers */
   struct exec_list free_sample_buffers;

   int n_active_oa_queries;
   int n_active_pipeline_stats_queries;

   /* The number of queries depending on running OA counters which
    * extends beyond brw_end_perf_query() since we need to wait until
    * the last MI_RPC command has parsed by the GPU.
    *
    * Accurate accounting is important here as emitting an
    * MI_REPORT_PERF_COUNT command while the OA unit is disabled will
    * effectively hang the gpu.
    */
   int n_oa_users;

   /* To help catch an spurious problem with the hardware or perf
    * forwarding samples, we emit each MI_REPORT_PERF_COUNT command
    * with a unique ID that we can explicitly check for...
    */
   int next_query_start_report_id;

   /**
    * An array of queries whose results haven't yet been assembled
    * based on the data in buffer objects.
    *
    * These may be active, or have already ended.  However, the
    * results have not been requested.
    */
   struct gen_perf_query_object **unaccumulated;
   int unaccumulated_elements;
   int unaccumulated_array_size;

   /* The total number of query objects so we can relinquish
    * our exclusive access to perf if the application deletes
    * all of its objects. (NB: We only disable perf while
    * there are no active queries)
    */
   int n_query_instances;
};

void gen_perf_init_context(struct gen_perf_context *perf_ctx,
                           struct gen_perf_config *perf_cfg,
                           void * ctx,  /* driver context (eg, brw_context) */
                           void * bufmgr,  /* eg brw_bufmgr */
                           const struct gen_device_info *devinfo,
                           uint32_t hw_ctx,
                           int drm_fd);

static inline size_t
gen_perf_query_counter_get_size(const struct gen_perf_query_counter *counter)
{
   switch (counter->data_type) {
   case GEN_PERF_COUNTER_DATA_TYPE_BOOL32:
      return sizeof(uint32_t);
   case GEN_PERF_COUNTER_DATA_TYPE_UINT32:
      return sizeof(uint32_t);
   case GEN_PERF_COUNTER_DATA_TYPE_UINT64:
      return sizeof(uint64_t);
   case GEN_PERF_COUNTER_DATA_TYPE_FLOAT:
      return sizeof(float);
   case GEN_PERF_COUNTER_DATA_TYPE_DOUBLE:
      return sizeof(double);
   default:
      unreachable("invalid counter data type");
   }
}

static inline struct gen_perf_query_info *
gen_perf_query_append_query_info(struct gen_perf_config *perf, int max_counters)
{
   struct gen_perf_query_info *query;

   perf->queries = reralloc(perf, perf->queries,
                            struct gen_perf_query_info,
                            ++perf->n_queries);
   query = &perf->queries[perf->n_queries - 1];
   memset(query, 0, sizeof(*query));

   if (max_counters > 0) {
      query->max_counters = max_counters;
      query->counters =
         rzalloc_array(perf, struct gen_perf_query_counter, max_counters);
   }

   return query;
}

static inline void
gen_perf_query_info_add_stat_reg(struct gen_perf_query_info *query,
                                 uint32_t reg,
                                 uint32_t numerator,
                                 uint32_t denominator,
                                 const char *name,
                                 const char *description)
{
   struct gen_perf_query_counter *counter;

   assert(query->n_counters < query->max_counters);

   counter = &query->counters[query->n_counters];
   counter->name = name;
   counter->desc = description;
   counter->type = GEN_PERF_COUNTER_TYPE_RAW;
   counter->data_type = GEN_PERF_COUNTER_DATA_TYPE_UINT64;
   counter->offset = sizeof(uint64_t) * query->n_counters;
   counter->pipeline_stat.reg = reg;
   counter->pipeline_stat.numerator = numerator;
   counter->pipeline_stat.denominator = denominator;

   query->n_counters++;
}

static inline void
gen_perf_query_info_add_basic_stat_reg(struct gen_perf_query_info *query,
                                       uint32_t reg, const char *name)
{
   gen_perf_query_info_add_stat_reg(query, reg, 1, 1, name, name);
}

static inline struct gen_perf_config *
gen_perf_new(void *ctx)
{
   struct gen_perf_config *perf = rzalloc(ctx, struct gen_perf_config);
   return perf;
}

bool gen_perf_load_oa_metrics(struct gen_perf_config *perf, int fd,
                              const struct gen_device_info *devinfo);
bool gen_perf_load_metric_id(struct gen_perf_config *perf, const char *guid,
                             uint64_t *metric_id);

void gen_perf_query_result_read_frequencies(struct gen_perf_query_result *result,
                                            const struct gen_device_info *devinfo,
                                            const uint32_t *start,
                                            const uint32_t *end);
void gen_perf_query_result_accumulate(struct gen_perf_query_result *result,
                                      const struct gen_perf_query_info *query,
                                      const uint32_t *start,
                                      const uint32_t *end);
void gen_perf_query_result_clear(struct gen_perf_query_result *result);
void gen_perf_query_register_mdapi_statistic_query(const struct gen_device_info *devinfo,
                                                   struct gen_perf_config *perf);
void gen_perf_query_register_mdapi_oa_query(const struct gen_device_info *devinfo,
                                            struct gen_perf_config *perf);
uint64_t gen_perf_query_get_metric_id(struct gen_perf_config *perf,
                                      const struct gen_perf_query_info *query);
struct oa_sample_buf * gen_perf_get_free_sample_buf(struct gen_perf_context *perf);
void gen_perf_reap_old_sample_buffers(struct gen_perf_context *perf_ctx);
void gen_perf_free_sample_bufs(struct gen_perf_context *perf_ctx);

void gen_perf_snapshot_statistics_registers(void *context,
                                            struct gen_perf_config *perf,
                                            struct gen_perf_query_object *obj,
                                            uint32_t offset_in_bytes);

void gen_perf_close(struct gen_perf_context *perfquery,
                    const struct gen_perf_query_info *query);
bool gen_perf_open(struct gen_perf_context *perfquery,
                   int metrics_set_id,
                   int report_format,
                   int period_exponent,
                   int drm_fd,
                   uint32_t ctx_id);

bool gen_perf_inc_n_users(struct gen_perf_context *perfquery);
void gen_perf_dec_n_users(struct gen_perf_context *perfquery);

bool gen_perf_begin_query(struct gen_perf_context *perf_ctx,
                          struct gen_perf_query_object *query);
void gen_perf_end_query(struct gen_perf_context *perf_ctx,
                        struct gen_perf_query_object *query);
void gen_perf_wait_query(struct gen_perf_context *perf_ctx,
                         struct gen_perf_query_object *query,
                         void *current_batch);
bool gen_perf_is_query_ready(struct gen_perf_context *perf_ctx,
                             struct gen_perf_query_object *query,
                             void *current_batch);
void gen_perf_delete_query(struct gen_perf_context *perf_ctx,
                           struct gen_perf_query_object *query);

#endif /* GEN_PERF_H */

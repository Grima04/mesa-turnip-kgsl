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

#define GEN7_RPSTAT1                       0xA01C
#define  GEN7_RPSTAT1_CURR_GT_FREQ_SHIFT   7
#define  GEN7_RPSTAT1_CURR_GT_FREQ_MASK    INTEL_MASK(13, 7)
#define  GEN7_RPSTAT1_PREV_GT_FREQ_SHIFT   0
#define  GEN7_RPSTAT1_PREV_GT_FREQ_MASK    INTEL_MASK(6, 0)

#define GEN9_RPSTAT0                       0xA01C
#define  GEN9_RPSTAT0_CURR_GT_FREQ_SHIFT   23
#define  GEN9_RPSTAT0_CURR_GT_FREQ_MASK    INTEL_MASK(31, 23)
#define  GEN9_RPSTAT0_PREV_GT_FREQ_SHIFT   0
#define  GEN9_RPSTAT0_PREV_GT_FREQ_MASK    INTEL_MASK(8, 0)

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

void gen_perf_init_metrics(struct gen_perf_config *perf_cfg,
                           const struct gen_device_info *devinfo,
                           int drm_fd);
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

static inline struct gen_perf_config *
gen_perf_new(void *ctx)
{
   struct gen_perf_config *perf = rzalloc(ctx, struct gen_perf_config);
   return perf;
}

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
uint64_t gen_perf_query_get_metric_id(struct gen_perf_config *perf,
                                      const struct gen_perf_query_info *query);
struct oa_sample_buf * gen_perf_get_free_sample_buf(struct gen_perf_context *perf);
void gen_perf_reap_old_sample_buffers(struct gen_perf_context *perf_ctx);
void gen_perf_free_sample_bufs(struct gen_perf_context *perf_ctx);

void gen_perf_snapshot_statistics_registers(void *context,
                                            struct gen_perf_config *perf,
                                            struct gen_perf_query_object *obj,
                                            uint32_t offset_in_bytes);
struct gen_perf_query_object *
gen_perf_new_query(struct gen_perf_context *, unsigned query_index);

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
void gen_perf_get_query_data(struct gen_perf_context *perf_ctx,
                             struct gen_perf_query_object *query,
                             int data_size,
                             unsigned *data,
                             unsigned *bytes_written);

#endif /* GEN_PERF_H */

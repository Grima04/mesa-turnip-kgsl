/*
 * Copyright © 2019 Intel Corporation
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

#include "iris_perf.h"
#include "iris_context.h"
#include "perf/gen_perf_regs.h"

static void *
iris_oa_bo_alloc(void *bufmgr, const char *name, uint64_t size)
{
   return iris_bo_alloc(bufmgr, name, size, IRIS_MEMZONE_OTHER);
}

static void
iris_perf_emit_mi_flush(struct iris_context *ice)
{
   const int flags = PIPE_CONTROL_RENDER_TARGET_FLUSH |
                     PIPE_CONTROL_INSTRUCTION_INVALIDATE |
                     PIPE_CONTROL_CONST_CACHE_INVALIDATE |
                     PIPE_CONTROL_DATA_CACHE_FLUSH |
                     PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                     PIPE_CONTROL_VF_CACHE_INVALIDATE |
                     PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
                     PIPE_CONTROL_CS_STALL;
   iris_emit_pipe_control_flush(&ice->batches[IRIS_BATCH_RENDER],
                                "OA metrics", flags);
}

static void
iris_perf_emit_mi_report_perf_count(void *c,
                                       void *bo,
                                       uint32_t offset_in_bytes,
                                       uint32_t report_id)
{
   struct iris_context *ice = c;
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];
   ice->vtbl.emit_mi_report_perf_count(batch, bo, offset_in_bytes, report_id);
}

static void
iris_perf_batchbuffer_flush(void *c, const char *file, int line)
{
   struct iris_context *ice = c;
   _iris_batch_flush(&ice->batches[IRIS_BATCH_RENDER], __FILE__, __LINE__);
}

static void
iris_perf_capture_frequency_stat_register(void *ctx,
                                                void *bo,
                                                uint32_t bo_offset)
{
   struct iris_context *ice = ctx;
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];
   struct gen_device_info *devinfo = &batch->screen->devinfo;

   if (devinfo->gen == 8 && !devinfo->is_cherryview)
      ice->vtbl.store_register_mem32(batch, GEN7_RPSTAT1, bo, bo_offset, false);
   else if (devinfo->gen >= 9)
      ice->vtbl.store_register_mem32(batch, GEN9_RPSTAT0, bo, bo_offset, false);
}

static void
iris_perf_store_register_mem64(void *ctx, void *bo,
                                     uint32_t reg, uint32_t offset)
{
   struct iris_context *ice = ctx;
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];
   ice->vtbl.store_register_mem64(batch, reg, bo, offset, false);
}

typedef void (*bo_unreference_t)(void *);
typedef void *(*bo_map_t)(void *, void *, unsigned flags);
typedef void (*bo_unmap_t)(void *);
typedef void (*emit_mi_report_t)(void *, void *, uint32_t, uint32_t);
typedef void (*emit_mi_flush_t)(void *);
typedef void (*capture_frequency_stat_register_t)(void *, void *, uint32_t );
typedef void (*store_register_mem64_t)(void *ctx, void *bo,
                                       uint32_t reg, uint32_t offset);
typedef bool (*batch_references_t)(void *batch, void *bo);
typedef void (*bo_wait_rendering_t)(void *bo);
typedef int (*bo_busy_t)(void *bo);

void
iris_perf_init_vtbl(struct gen_perf_config *perf_cfg)
{
   perf_cfg->vtbl.bo_alloc = iris_oa_bo_alloc;
   perf_cfg->vtbl.bo_unreference = (bo_unreference_t)iris_bo_unreference;
   perf_cfg->vtbl.bo_map = (bo_map_t)iris_bo_map;
   perf_cfg->vtbl.bo_unmap = (bo_unmap_t)iris_bo_unmap;
   perf_cfg->vtbl.emit_mi_flush = (emit_mi_flush_t)iris_perf_emit_mi_flush;

   perf_cfg->vtbl.emit_mi_report_perf_count =
      (emit_mi_report_t)iris_perf_emit_mi_report_perf_count;
   perf_cfg->vtbl.batchbuffer_flush = iris_perf_batchbuffer_flush;
   perf_cfg->vtbl.capture_frequency_stat_register =
      (capture_frequency_stat_register_t) iris_perf_capture_frequency_stat_register;
   perf_cfg->vtbl.store_register_mem64 =
      (store_register_mem64_t) iris_perf_store_register_mem64;
   perf_cfg->vtbl.batch_references = (batch_references_t)iris_batch_references;
   perf_cfg->vtbl.bo_wait_rendering =
      (bo_wait_rendering_t)iris_bo_wait_rendering;
   perf_cfg->vtbl.bo_busy = (bo_busy_t)iris_bo_busy;
}

/*
 * Copyright © 2017 Intel Corporation
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
#ifndef IRIS_CONTEXT_H
#define IRIS_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "intel/common/gen_debug.h"
#include "intel/compiler/brw_compiler.h"
#include "iris_batch.h"
#include "iris_screen.h"

struct iris_bo;
struct iris_batch;

#define IRIS_RESOURCE_FLAG_INSTRUCTION_CACHE (PIPE_RESOURCE_FLAG_DRV_PRIV << 0)

#define IRIS_MAX_TEXTURE_SAMPLERS 32
#define IRIS_MAX_VIEWPORTS 16

#define IRIS_DIRTY_COLOR_CALC_STATE         (1ull <<  0)
#define IRIS_DIRTY_POLYGON_STIPPLE          (1ull <<  1)
#define IRIS_DIRTY_SCISSOR_RECT             (1ull <<  2)
#define IRIS_DIRTY_WM_DEPTH_STENCIL         (1ull <<  3)
#define IRIS_DIRTY_CC_VIEWPORT              (1ull <<  4)
#define IRIS_DIRTY_SF_CL_VIEWPORT           (1ull <<  5)
#define IRIS_DIRTY_PS_BLEND                 (1ull <<  6)
#define IRIS_DIRTY_BLEND_STATE              (1ull <<  7)
#define IRIS_DIRTY_RASTER                   (1ull <<  8)
#define IRIS_DIRTY_CLIP                     (1ull <<  9)
#define IRIS_DIRTY_SCISSOR                  (1ull << 10)
#define IRIS_DIRTY_LINE_STIPPLE             (1ull << 11)
#define IRIS_DIRTY_VERTEX_ELEMENTS          (1ull << 12)
#define IRIS_DIRTY_MULTISAMPLE              (1ull << 13)
#define IRIS_DIRTY_VERTEX_BUFFERS           (1ull << 14)
#define IRIS_DIRTY_SAMPLE_MASK              (1ull << 15)
#define IRIS_DIRTY_SAMPLER_STATES_VS        (1ull << 16)
#define IRIS_DIRTY_SAMPLER_STATES_TCS       (1ull << 17)
#define IRIS_DIRTY_SAMPLER_STATES_TES       (1ull << 18)
#define IRIS_DIRTY_SAMPLER_STATES_GS        (1ull << 19)
#define IRIS_DIRTY_SAMPLER_STATES_PS        (1ull << 20)
#define IRIS_DIRTY_SAMPLER_STATES_CS        (1ull << 21)
#define IRIS_DIRTY_UNCOMPILED_VS            (1ull << 22)
#define IRIS_DIRTY_UNCOMPILED_TCS           (1ull << 23)
#define IRIS_DIRTY_UNCOMPILED_TES           (1ull << 24)
#define IRIS_DIRTY_UNCOMPILED_GS            (1ull << 25)
#define IRIS_DIRTY_UNCOMPILED_FS            (1ull << 26)
#define IRIS_DIRTY_UNCOMPILED_CS            (1ull << 27)
#define IRIS_DIRTY_VS                       (1ull << 28)
#define IRIS_DIRTY_TCS                      (1ull << 29)
#define IRIS_DIRTY_TES                      (1ull << 30)
#define IRIS_DIRTY_GS                       (1ull << 31)
#define IRIS_DIRTY_FS                       (1ull << 32)
#define IRIS_DIRTY_CS                       (1ull << 33)
#define IRIS_DIRTY_URB                      (1ull << 34)

struct iris_depth_stencil_alpha_state;

enum iris_program_cache_id {
   IRIS_CACHE_VS  = MESA_SHADER_VERTEX,
   IRIS_CACHE_TCS = MESA_SHADER_TESS_CTRL,
   IRIS_CACHE_TES = MESA_SHADER_TESS_EVAL,
   IRIS_CACHE_GS  = MESA_SHADER_GEOMETRY,
   IRIS_CACHE_FS  = MESA_SHADER_FRAGMENT,
   IRIS_CACHE_CS  = MESA_SHADER_COMPUTE,
   IRIS_CACHE_BLORP_BLIT,
};

struct iris_compiled_shader {
   /** Buffer containing the uploaded assembly. */
   struct pipe_resource *buffer;

   /** Offset where the assembly lives in the BO. */
   unsigned offset;

   /** Pointer to the assembly in the BO's map. */
   void *map;

   /** The program data (owned by the program cache hash table) */
   struct brw_stage_prog_data *prog_data;

   /**
    * Shader packets and other data derived from prog_data.  These must be
    * completely determined from prog_data.
    */
   uint8_t derived_data[0];
};

struct iris_context {
   struct pipe_context ctx;

   struct pipe_debug_callback dbg;

   struct {
      struct iris_uncompiled_shader *uncompiled[MESA_SHADER_STAGES];
      struct iris_compiled_shader *prog[MESA_SHADER_STAGES];
      struct brw_vue_map *last_vue_map;

      struct u_upload_mgr *uploader;
      struct hash_table *cache;
   } shaders;

   /** The main batch for rendering */
   struct iris_batch render_batch;

   struct {
      uint64_t dirty;
      unsigned num_viewports; // XXX: can viewports + scissors be different?
      unsigned num_scissors;
      unsigned sample_mask;
      struct iris_blend_state *cso_blend;
      struct iris_rasterizer_state *cso_rast;
      struct iris_depth_stencil_alpha_state *cso_zsa;
      struct iris_vertex_element_state *cso_vertex_elements;
      struct iris_vertex_buffer_state *cso_vertex_buffers;
      struct iris_viewport_state *cso_vp;
      struct iris_depth_state *cso_depth;
      struct pipe_blend_color blend_color;
      struct pipe_poly_stipple poly_stipple;
      struct pipe_scissor_state scissors[IRIS_MAX_VIEWPORTS];
      struct pipe_stencil_ref stencil_ref;
      struct pipe_framebuffer_state framebuffer;

      struct iris_sampler_state *samplers[MESA_SHADER_STAGES][IRIS_MAX_TEXTURE_SAMPLERS];

      void (*destroy_state)(struct iris_context *ice);
      void (*init_render_context)(struct iris_screen *screen,
                                  struct iris_batch *batch,
                                  struct pipe_debug_callback *dbg);
      void (*upload_render_state)(struct iris_context *ice,
                                  struct iris_batch *batch,
                                  const struct pipe_draw_info *draw);
      unsigned (*derived_program_state_size)(enum iris_program_cache_id id);
      void (*set_derived_program_state)(const struct gen_device_info *devinfo,
                                        enum iris_program_cache_id cache_id,
                                        struct iris_compiled_shader *shader);
      void (*populate_vs_key)(const struct iris_context *ice,
                              struct brw_vs_prog_key *key);
      void (*populate_tcs_key)(const struct iris_context *ice,
                               struct brw_tcs_prog_key *key);
      void (*populate_tes_key)(const struct iris_context *ice,
                               struct brw_tes_prog_key *key);
      void (*populate_gs_key)(const struct iris_context *ice,
                              struct brw_gs_prog_key *key);
      void (*populate_fs_key)(const struct iris_context *ice,
                              struct brw_wm_prog_key *key);
   } state;
};

#define perf_debug(dbg, ...) do {                      \
   if (INTEL_DEBUG & DEBUG_PERF)                       \
      dbg_printf(__VA_ARGS__);                         \
   if (unlikely(dbg))                                  \
      pipe_debug_message(dbg, PERF_INFO, __VA_ARGS__); \
} while(0)

double get_time(void);

struct pipe_context *
iris_create_context(struct pipe_screen *screen, void *priv, unsigned flags);

void iris_init_blit_functions(struct pipe_context *ctx);
void iris_init_clear_functions(struct pipe_context *ctx);
void iris_init_program_functions(struct pipe_context *ctx);
void iris_init_resource_functions(struct pipe_context *ctx);
void iris_init_query_functions(struct pipe_context *ctx);
void iris_update_compiled_shaders(struct iris_context *ice);

/* iris_draw.c */

void iris_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *info);

/* iris_state.c */

void gen9_init_state(struct iris_context *ice);
void gen10_init_state(struct iris_context *ice);

/* iris_program_cache.c */

void iris_init_program_cache(struct iris_context *ice);
void iris_destroy_program_cache(struct iris_context *ice);
void iris_print_program_cache(struct iris_context *ice);
bool iris_bind_cached_shader(struct iris_context *ice,
                             enum iris_program_cache_id cache_id,
                             const void *key);
void iris_upload_and_bind_shader(struct iris_context *ice,
                                 enum iris_program_cache_id cache_id,
                                 const void *key,
                                 const void *assembly,
                                 struct brw_stage_prog_data *prog_data);
const void *iris_find_previous_compile(const struct iris_context *ice,
                                       enum iris_program_cache_id cache_id,
                                       unsigned program_string_id);
#endif

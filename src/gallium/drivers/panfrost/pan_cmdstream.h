/*
 * Copyright (C) 2018 Alyssa Rosenzweig
 * Copyright (C) 2020 Collabora Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __PAN_CMDSTREAM_H__
#define __PAN_CMDSTREAM_H__

#include "pipe/p_defines.h"
#include "pipe/p_state.h"

#include "panfrost-job.h"

#include "pan_job.h"

void panfrost_sampler_desc_init(const struct pipe_sampler_state *cso,
                                struct mali_sampler_descriptor *hw);

void
panfrost_vt_init(struct panfrost_context *ctx,
                 enum pipe_shader_type stage,
                 struct midgard_payload_vertex_tiler *vtp);

void
panfrost_vt_attach_framebuffer(struct panfrost_context *ctx,
                               struct midgard_payload_vertex_tiler *vt);

void
panfrost_vt_update_rasterizer(struct panfrost_context *ctx,
                              struct midgard_payload_vertex_tiler *tp);

void
panfrost_vt_update_occlusion_query(struct panfrost_context *ctx,
                                   struct midgard_payload_vertex_tiler *tp);

void
panfrost_vt_set_draw_info(struct panfrost_context *ctx,
                          const struct pipe_draw_info *info,
                          enum mali_draw_mode draw_mode,
                          struct midgard_payload_vertex_tiler *vp,
                          struct midgard_payload_vertex_tiler *tp,
                          unsigned *vertex_count,
                          unsigned *padded_count);

void
panfrost_emit_shader_meta(struct panfrost_batch *batch,
                          enum pipe_shader_type st,
                          struct midgard_payload_vertex_tiler *vtp);

void
panfrost_emit_viewport(struct panfrost_batch *batch,
                       struct midgard_payload_vertex_tiler *tp);

void
panfrost_emit_const_buf(struct panfrost_batch *batch,
                        enum pipe_shader_type stage,
                        struct midgard_payload_vertex_tiler *vtp);

void
panfrost_emit_shared_memory(struct panfrost_batch *batch,
                            const struct pipe_grid_info *info,
                            struct midgard_payload_vertex_tiler *vtp);

void
panfrost_emit_texture_descriptors(struct panfrost_batch *batch,
                                  enum pipe_shader_type stage,
                                  struct midgard_payload_vertex_tiler *vtp);

void
panfrost_emit_sampler_descriptors(struct panfrost_batch *batch,
                                  enum pipe_shader_type stage,
                                  struct midgard_payload_vertex_tiler *vtp);

void
panfrost_emit_vertex_attr_meta(struct panfrost_batch *batch,
                               struct midgard_payload_vertex_tiler *vp);

void
panfrost_emit_vertex_data(struct panfrost_batch *batch,
                          struct midgard_payload_vertex_tiler *vp);

void
panfrost_emit_varying_descriptor(struct panfrost_batch *batch,
                                 unsigned vertex_count,
                                 struct midgard_payload_vertex_tiler *vp,
                                 struct midgard_payload_vertex_tiler *tp);

void
panfrost_emit_vertex_tiler_jobs(struct panfrost_batch *batch,
                                struct midgard_payload_vertex_tiler *vp,
                                struct midgard_payload_vertex_tiler *tp);

#endif /* __PAN_CMDSTREAM_H__ */

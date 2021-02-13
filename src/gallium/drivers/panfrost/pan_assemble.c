/*
 * © Copyright 2018 Alyssa Rosenzweig
 * Copyright (C) 2019-2020 Collabora, Ltd.
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
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pan_bo.h"
#include "pan_context.h"
#include "pan_shader.h"
#include "pan_util.h"
#include "panfrost-quirks.h"

#include "compiler/nir/nir.h"
#include "nir/tgsi_to_nir.h"
#include "util/u_dynarray.h"
#include "util/u_upload_mgr.h"

#include "tgsi/tgsi_dump.h"

static void
pan_prepare_midgard_props(struct panfrost_shader_state *state)
{
        pan_prepare(&state->properties, RENDERER_PROPERTIES);
        state->properties.uniform_buffer_count = state->info.ubo_count;
        state->properties.midgard.uniform_count = state->info.midgard.uniform_cutoff;
        state->properties.midgard.shader_has_side_effects = state->info.writes_global;
        state->properties.midgard.fp_mode = MALI_FP_MODE_GL_INF_NAN_ALLOWED;

        /* For fragment shaders, work register count, early-z, reads at draw-time */

        if (state->info.stage != MESA_SHADER_FRAGMENT)
                state->properties.midgard.work_register_count = state->info.work_reg_count;
}

static void
pan_prepare_bifrost_props(struct panfrost_shader_state *state)
{
        unsigned fau_count = DIV_ROUND_UP(state->info.push.count, 2);

        switch (state->info.stage) {
        case MESA_SHADER_VERTEX:
                pan_prepare(&state->properties, RENDERER_PROPERTIES);
                state->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
                state->properties.uniform_buffer_count = state->info.ubo_count;

                pan_prepare(&state->preload, PRELOAD);
                state->preload.uniform_count = fau_count;
                state->preload.vertex.vertex_id = true;
                state->preload.vertex.instance_id = true;
                break;
        case MESA_SHADER_FRAGMENT:
                pan_prepare(&state->properties, RENDERER_PROPERTIES);
                /* Early-Z set at draw-time */
                if (state->info.fs.writes_depth || state->info.fs.writes_stencil) {
                        state->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_FORCE_LATE;
                        state->properties.bifrost.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_LATE;
                } else if (state->info.fs.can_discard) {
                        state->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_FORCE_LATE;
                        state->properties.bifrost.pixel_kill_operation = MALI_PIXEL_KILL_WEAK_EARLY;
                } else {
                        state->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
                        state->properties.bifrost.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
                }
                state->properties.uniform_buffer_count = state->info.ubo_count;
                state->properties.bifrost.shader_modifies_coverage = state->info.fs.can_discard;
                state->properties.bifrost.shader_wait_dependency_6 = state->info.bifrost.wait_6;
                state->properties.bifrost.shader_wait_dependency_7 = state->info.bifrost.wait_7;

                pan_prepare(&state->preload, PRELOAD);
                state->preload.uniform_count = fau_count;
                state->preload.fragment.fragment_position = state->info.fs.reads_frag_coord;
                state->preload.fragment.coverage = true;
                state->preload.fragment.primitive_flags = state->info.fs.reads_face;

                /* Contains sample ID and sample mask. Sample position and
                 * helper invocation are expressed in terms of the above, so
                 * preload for those too */
                state->preload.fragment.sample_mask_id =
                        state->info.fs.reads_sample_id |
                        state->info.fs.reads_sample_pos |
                        state->info.fs.reads_sample_mask_in |
                        state->info.fs.reads_helper_invocation;
                break;
        case MESA_SHADER_COMPUTE:
                pan_prepare(&state->properties, RENDERER_PROPERTIES);
                state->properties.uniform_buffer_count = state->info.ubo_count;

                pan_prepare(&state->preload, PRELOAD);
                state->preload.uniform_count = fau_count;

                state->preload.compute.local_invocation_xy = true;
                state->preload.compute.local_invocation_z = true;

                state->preload.compute.work_group_x = true;
                state->preload.compute.work_group_y = true;
                state->preload.compute.work_group_z = true;

                state->preload.compute.global_invocation_x = true;
                state->preload.compute.global_invocation_y = true;
                state->preload.compute.global_invocation_z = true;
                break;
        default:
                unreachable("TODO");
        }
}

static void
pan_upload_shader_descriptor(struct panfrost_context *ctx,
                        struct panfrost_shader_state *state)
{
        const struct panfrost_device *dev = pan_device(ctx->base.screen);
        struct mali_state_packed *out;

        u_upload_alloc(ctx->state_uploader, 0, MALI_RENDERER_STATE_LENGTH, MALI_RENDERER_STATE_LENGTH,
                        &state->upload.offset, &state->upload.rsrc, (void **) &out);

        pan_pack(out, RENDERER_STATE, cfg) {
                cfg.shader = state->shader;
                cfg.properties = state->properties;

                if (pan_is_bifrost(dev))
                        cfg.preload = state->preload;
        }

        u_upload_unmap(ctx->state_uploader);
}

void
panfrost_shader_compile(struct panfrost_context *ctx,
                        enum pipe_shader_ir ir_type,
                        const void *ir,
                        gl_shader_stage stage,
                        struct panfrost_shader_state *state)
{
        struct panfrost_device *dev = pan_device(ctx->base.screen);

        nir_shader *s;

        if (ir_type == PIPE_SHADER_IR_NIR) {
                s = nir_shader_clone(NULL, ir);
        } else {
                assert (ir_type == PIPE_SHADER_IR_TGSI);
                s = tgsi_to_nir(ir, ctx->base.screen, false);
        }

        s->info.stage = stage;

        /* Call out to Midgard compiler given the above NIR */
        struct panfrost_compile_inputs inputs = {
                .gpu_id = dev->gpu_id,
                .shaderdb = !!(dev->debug & PAN_DBG_PRECOMPILE),
        };

        memcpy(inputs.rt_formats, state->rt_formats, sizeof(inputs.rt_formats));

        struct util_dynarray binary;

        util_dynarray_init(&binary, NULL);
        pan_shader_compile(dev, s, &inputs, &binary, &state->info);

        /* Prepare the compiled binary for upload */
        mali_ptr shader = 0;
        int size = binary.size;

        if (size) {
                state->bo = panfrost_bo_create(dev, size, PAN_BO_EXECUTE);
                memcpy(state->bo->ptr.cpu, binary.data, size);
                shader = state->bo->ptr.gpu;
        }

        /* Midgard needs the first tag on the bottom nibble */

        if (!pan_is_bifrost(dev))
                shader |= state->info.midgard.first_tag;

        /* Prepare the descriptors at compile-time */
        state->shader.shader = shader;
        state->shader.attribute_count = state->info.attribute_count;
        state->shader.varying_count = state->info.varyings.input_count +
                                      state->info.varyings.output_count;
        state->shader.texture_count = state->info.texture_count;
        state->shader.sampler_count = state->info.texture_count;

        if (pan_is_bifrost(dev))
                pan_prepare_bifrost_props(state);
        else
                pan_prepare_midgard_props(state);

        state->properties.shader_contains_barrier =
                state->info.contains_barrier;

        /* Ordering gaurantees are the same */
        if (stage == MESA_SHADER_FRAGMENT) {
                state->properties.shader_contains_barrier |=
                        state->info.fs.helper_invocations;
                state->properties.stencil_from_shader =
                        state->info.fs.writes_stencil;
                state->properties.depth_source =
                        state->info.fs.writes_depth ?
                        MALI_DEPTH_SOURCE_SHADER :
                        MALI_DEPTH_SOURCE_FIXED_FUNCTION;
        } else {
                state->properties.depth_source =
                        MALI_DEPTH_SOURCE_FIXED_FUNCTION;
        }


        if (stage != MESA_SHADER_FRAGMENT)
                pan_upload_shader_descriptor(ctx, state);

        util_dynarray_fini(&binary);

        /* In both clone and tgsi_to_nir paths, the shader is ralloc'd against
         * a NULL context */
        ralloc_free(s);
}

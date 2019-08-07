/*
 * Copyright (C) 2018-2019 Alyssa Rosenzweig
 * Copyright (C) 2019 Collabora, Ltd.
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

#include "pan_context.h"

static mali_ptr
panfrost_emit_varyings(
        struct panfrost_context *ctx,
        union mali_attr *slot,
        unsigned stride,
        unsigned count)
{
        /* Fill out the descriptor */
        slot->stride = stride;
        slot->size = stride * count;
        slot->shift = slot->extra_flags = 0;

        struct panfrost_transfer transfer =
                panfrost_allocate_transient(ctx, slot->size);

        slot->elements = transfer.gpu | MALI_ATTR_LINEAR;

        return transfer.gpu;
}

static void
panfrost_emit_point_coord(union mali_attr *slot)
{
        slot->elements = MALI_VARYING_POINT_COORD | MALI_ATTR_LINEAR;
        slot->stride = slot->size = slot->shift = slot->extra_flags = 0;
}

static void
panfrost_emit_front_face(union mali_attr *slot)
{
        slot->elements = MALI_VARYING_FRONT_FACING | MALI_ATTR_INTERNAL;
}

void
panfrost_emit_varying_descriptor(
        struct panfrost_context *ctx,
        unsigned vertex_count)
{
        /* Load the shaders */

        struct panfrost_shader_state *vs = &ctx->shader[PIPE_SHADER_VERTEX]->variants[ctx->shader[PIPE_SHADER_VERTEX]->active_variant];
        struct panfrost_shader_state *fs = &ctx->shader[PIPE_SHADER_FRAGMENT]->variants[ctx->shader[PIPE_SHADER_FRAGMENT]->active_variant];
        unsigned int num_gen_varyings = 0;

        /* Allocate the varying descriptor */

        size_t vs_size = sizeof(struct mali_attr_meta) * vs->tripipe->varying_count;
        size_t fs_size = sizeof(struct mali_attr_meta) * fs->tripipe->varying_count;

        struct panfrost_transfer trans = panfrost_allocate_transient(ctx,
                                         vs_size + fs_size);

        /*
         * Assign ->src_offset now that we know about all the general purpose
         * varyings that will be used by the fragment and vertex shaders.
         */
        for (unsigned i = 0; i < vs->tripipe->varying_count; i++) {
                /*
                 * General purpose varyings have ->index set to 0, skip other
                 * entries.
                 */
                if (vs->varyings[i].index)
                        continue;

                vs->varyings[i].src_offset = 16 * (num_gen_varyings++);
        }

        for (unsigned i = 0; i < fs->tripipe->varying_count; i++) {
                unsigned j;

                /* If we have a point sprite replacement, handle that here. We
                 * have to translate location first.  TODO: Flip y in shader.
                 * We're already keying ... just time crunch .. */

                unsigned loc = fs->varyings_loc[i];
                unsigned pnt_loc =
                        (loc >= VARYING_SLOT_VAR0) ? (loc - VARYING_SLOT_VAR0) :
                        (loc == VARYING_SLOT_PNTC) ? 8 :
                        ~0;

                if (~pnt_loc && fs->point_sprite_mask & (1 << pnt_loc)) {
                        /* gl_PointCoord index by convention */
                        fs->varyings[i].index = 3;
                        fs->reads_point_coord = true;

                        /* Swizzle out the z/w to 0/1 */
                        fs->varyings[i].format = MALI_RG16F;
                        fs->varyings[i].swizzle =
                                panfrost_get_default_swizzle(2);

                        continue;
                }

                if (fs->varyings[i].index)
                        continue;

                /*
                 * Re-use the VS general purpose varying pos if it exists,
                 * create a new one otherwise.
                 */
                for (j = 0; j < vs->tripipe->varying_count; j++) {
                        if (fs->varyings_loc[i] == vs->varyings_loc[j])
                                break;
                }

                if (j < vs->tripipe->varying_count)
                        fs->varyings[i].src_offset = vs->varyings[j].src_offset;
                else
                        fs->varyings[i].src_offset = 16 * (num_gen_varyings++);
        }

        memcpy(trans.cpu, vs->varyings, vs_size);
        memcpy(trans.cpu + vs_size, fs->varyings, fs_size);

        ctx->payloads[PIPE_SHADER_VERTEX].postfix.varying_meta = trans.gpu;
        ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.varying_meta = trans.gpu + vs_size;

        /* Buffer indices must be in this order per our convention */
        union mali_attr varyings[PIPE_MAX_ATTRIBS];
        unsigned idx = 0;

        panfrost_emit_varyings(ctx, &varyings[idx++], num_gen_varyings * 16,
                               vertex_count);

        /* fp32 vec4 gl_Position */
        ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.position_varying =
                panfrost_emit_varyings(ctx, &varyings[idx++],
                                       sizeof(float) * 4, vertex_count);


        if (vs->writes_point_size || fs->reads_point_coord) {
                /* fp16 vec1 gl_PointSize */
                ctx->payloads[PIPE_SHADER_FRAGMENT].primitive_size.pointer =
                        panfrost_emit_varyings(ctx, &varyings[idx++],
                                               2, vertex_count);
        } else if (fs->reads_face) {
                /* Dummy to advance index */
                ++idx;
        }

        if (fs->reads_point_coord) {
                /* Special descriptor */
                panfrost_emit_point_coord(&varyings[idx++]);
        } else if (fs->reads_face) {
                ++idx;
        }

        if (fs->reads_face) {
                panfrost_emit_front_face(&varyings[idx++]);
        }

        mali_ptr varyings_p = panfrost_upload_transient(ctx, &varyings, idx * sizeof(union mali_attr));
        ctx->payloads[PIPE_SHADER_VERTEX].postfix.varyings = varyings_p;
        ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.varyings = varyings_p;
}

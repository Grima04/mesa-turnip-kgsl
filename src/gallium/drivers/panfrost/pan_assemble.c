/*
 * © Copyright 2018 Alyssa Rosenzweig
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
#include "pan_context.h"

#include "compiler/nir/nir.h"
#include "nir/tgsi_to_nir.h"
#include "midgard/midgard_compile.h"
#include "util/u_dynarray.h"

#include "tgsi/tgsi_dump.h"

void
panfrost_shader_compile(struct panfrost_context *ctx, struct mali_shader_meta *meta, const char *src, int type, struct panfrost_shader_state *state)
{
        uint8_t *dst;

        nir_shader *s;

        struct pipe_shader_state *cso = state->base;

        if (cso->type == PIPE_SHADER_IR_NIR) {
                s = nir_shader_clone(NULL, cso->ir.nir);
        } else {
                assert (cso->type == PIPE_SHADER_IR_TGSI);
                //tgsi_dump(cso->tokens, 0);
                s = tgsi_to_nir(cso->tokens, &midgard_nir_options);
        }

        s->info.stage = type == JOB_TYPE_VERTEX ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT;

        if (s->info.stage == MESA_SHADER_FRAGMENT) {
                /* Inject the alpha test now if we need to */

                if (state->alpha_state.enabled) {
                        NIR_PASS_V(s, nir_lower_alpha_test, state->alpha_state.func, false);
                }
        }

        /* Call out to Midgard compiler given the above NIR */

        midgard_program program = {
                .alpha_ref = state->alpha_state.ref_value
        };

        midgard_compile_shader_nir(s, &program, false);

        /* Prepare the compiled binary for upload */
        int size = program.compiled.size;
        dst = program.compiled.data;

        /* Inject an external shader */
#if 0
        char buf[4096];

        if (type != JOB_TYPE_VERTEX) {
                FILE *fp = fopen("/home/alyssa/panfrost/midgard/good.bin", "rb");
                fread(buf, 1, 2816, fp);
                fclose(fp);
                dst = buf;
                size = 2816;
        }

#endif

        /* Upload the shader. The lookahead tag is ORed on as a tagged pointer.
         * I bet someone just thought that would be a cute pun. At least,
         * that's how I'd do it. */

        meta->shader = panfrost_upload(&ctx->shaders, dst, size, true) | program.first_tag;

        util_dynarray_fini(&program.compiled);

        meta->midgard1.uniform_count = MIN2(program.uniform_count, program.uniform_cutoff);
        meta->attribute_count = program.attribute_count;
        meta->varying_count = program.varying_count;
        meta->midgard1.work_count = program.work_register_count;

        state->can_discard = program.can_discard;
        state->writes_point_size = program.writes_point_size;

        /* Separate as primary uniform count is truncated */
        state->uniform_count = program.uniform_count;

        /* gl_Position eats up an extra spot */
        if (type == JOB_TYPE_VERTEX)
                meta->varying_count += 1;

        /* gl_FragCoord does -not- eat an extra spot; it will be included in our count if we need it */


        meta->midgard1.unknown2 = 8; /* XXX */

        /* Varyings are known only through the shader. We choose to upload this
         * information with the vertex shader, though the choice is perhaps
         * arbitrary */

        if (type == JOB_TYPE_VERTEX) {
                struct panfrost_varyings *varyings = &state->varyings;

                /* Measured in vec4 words. Don't include gl_Position */
                int varying_count = program.varying_count;

                /* Setup two buffers, one for position, the other for normal
                 * varyings, as seen in traces. TODO: Are there other
                 * configurations we might use? */

                varyings->varying_buffer_count = 2;

                /* mediump vec4s sequentially */
                varyings->varyings_stride[0] = (2 * sizeof(float)) * varying_count;

                /* highp gl_Position */
                varyings->varyings_stride[1] = 4 * sizeof(float);

                /* mediump gl_PointSize */
                if (program.writes_point_size) {
                        ++varyings->varying_buffer_count;
                        varyings->varyings_stride[2] = 2; /* sizeof(fp16) */
                }

                /* Setup gl_Position, its weirdo analogue, and gl_PointSize (optionally) */
                unsigned default_vec1_swizzle = panfrost_get_default_swizzle(1);
                unsigned default_vec4_swizzle = panfrost_get_default_swizzle(4);

                struct mali_attr_meta vertex_special_varyings[] = {
                        {
                                .index = 1,
                                .format = MALI_VARYING_POS,

                                .swizzle = default_vec4_swizzle,
                                .unknown1 = 0x2,
                        },
                        {
                                .index = 1,
                                .format = MALI_RGBA16F,

                                /* TODO: Wat? yyyy swizzle? */
                                .swizzle = 0x249,
                                .unknown1 = 0x0,
                        },
                        {
                                .index = 2,
                                .format = MALI_R16F,
                                .swizzle =  default_vec1_swizzle,
                                .unknown1 = 0x2
                        }
                };

                /* How many special vertex varyings are actually required? */
                int vertex_special_count = 2 + (program.writes_point_size ? 1 : 0);

                /* Setup actual varyings. XXX: Don't assume vec4 */

                struct mali_attr_meta mali_varyings[PIPE_MAX_ATTRIBS];

                for (int i = 0; i < varying_count; ++i) {
                        struct mali_attr_meta vec4_varying_meta = {
                                .index = 0,
                                .format = MALI_RGBA16F,
                                .swizzle = default_vec4_swizzle,
                                .unknown1 = 0x2,

                                /* Set offset to keep everything back-to-back in
                                 * the same buffer */
                                .src_offset = 8 * i,
                        };

                        mali_varyings[i] = vec4_varying_meta;
                }

                /* We don't count the weirdo gl_Position in our varying count */
                varyings->varying_count = varying_count - 1;

                /* In this context, position_meta represents the implicit
                 * gl_FragCoord varying. So, upload all the varyings */

                unsigned varyings_size = sizeof(struct mali_attr_meta) * varyings->varying_count;
                unsigned vertex_special_size = sizeof(struct mali_attr_meta) * vertex_special_count;
                unsigned vertex_size = vertex_special_size + varyings_size;
                unsigned fragment_size = varyings_size + sizeof(struct mali_attr_meta);

                struct panfrost_transfer transfer = panfrost_allocate_chunk(ctx, vertex_size + fragment_size, HEAP_DESCRIPTOR);

                /* Copy varyings in the follow order:
                 *  - Position 1, 2
                 *  - Varyings 1, 2, ..., n
                 *  - Varyings 1, 2, ..., n (duplicate)
                 *  - Position 1
                 */

                memcpy(transfer.cpu, vertex_special_varyings, vertex_special_size);
                memcpy(transfer.cpu + vertex_special_size, mali_varyings, varyings_size);
                memcpy(transfer.cpu + vertex_size, mali_varyings, varyings_size);
                memcpy(transfer.cpu + vertex_size + varyings_size, &vertex_special_varyings[0], sizeof(struct mali_attr_meta));

                /* Point to the descriptor */
                varyings->varyings_buffer_cpu = transfer.cpu;
                varyings->varyings_descriptor = transfer.gpu;
                varyings->varyings_descriptor_fragment = transfer.gpu + vertex_size;
        }
}

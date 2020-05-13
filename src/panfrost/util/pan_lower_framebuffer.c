/*
 * Copyright (C) 2020 Collabora, Ltd.
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
 *
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

/**
 * Implements framebuffer format conversions in software for Midgard/Bifrost
 * blend shaders. This pass is designed for a single render target; Midgard
 * duplicates blend shaders for MRT to simplify everything. A particular
 * framebuffer format may be categorized as 1) typed load available, 2) typed
 * unpack available, or 3) software unpack only, and likewise for stores. The
 * first two types are handled in the compiler backend directly, so this module
 * is responsible for identifying type 3 formats (hardware dependent) and
 * inserting appropriate ALU code to perform the conversion from the packed
 * type to a designated unpacked type, and vice versa.
 *
 * The unpacked type depends on the format:
 *
 *      - For 32-bit float formats, 32-bit floats.
 *      - For other floats, 16-bit floats.
 *      - For 32-bit ints, 32-bit ints.
 *      - For 8-bit ints, 8-bit ints.
 *      - For other ints, 16-bit ints.
 *
 * The rationale is to optimize blending and logic op instructions by using the
 * smallest precision necessary to store the pixel losslessly.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_format_convert.h"
#include "util/format/u_format.h"
#include "pan_lower_framebuffer.h"
#include "panfrost-quirks.h"

/* Determines the unpacked type best suiting a given format, so the rest of the
 * pipeline may be adjusted accordingly */

nir_alu_type
pan_unpacked_type_for_format(const struct util_format_description *desc)
{
        int c = util_format_get_first_non_void_channel(desc->format);

        if (c == -1)
                unreachable("Void format not renderable");

        bool large = (desc->channel[c].size > 16);
        bool bit8 = (desc->channel[c].size == 8);
        assert(desc->channel[c].size <= 32);

        if (desc->channel[c].normalized)
                return large ? nir_type_float32 : nir_type_float16;

        switch (desc->channel[c].type) {
        case UTIL_FORMAT_TYPE_UNSIGNED:
                return bit8 ? nir_type_uint8 :
                        large ? nir_type_uint32 : nir_type_uint16;
        case UTIL_FORMAT_TYPE_SIGNED:
                return bit8 ? nir_type_int8 :
                        large ? nir_type_int32 : nir_type_int16;
        case UTIL_FORMAT_TYPE_FLOAT:
                return large ? nir_type_float32 : nir_type_float16;
        default:
                unreachable("Format not renderable");
        }
}

enum pan_format_class
pan_format_class_load(const struct util_format_description *desc, unsigned quirks)
{
        /* Check if we can do anything better than software architecturally */
        if (quirks & MIDGARD_NO_TYPED_BLEND_LOADS) {
                return (quirks & NO_BLEND_PACKS)
                        ? PAN_FORMAT_SOFTWARE : PAN_FORMAT_PACK;
        }

        /* Some formats are missing as typed on some GPUs but have unpacks */
        if (quirks & MIDGARD_MISSING_LOADS) {
                switch (desc->format) {
                case PIPE_FORMAT_R11G11B10_FLOAT:
                case PIPE_FORMAT_R10G10B10A2_UNORM:
                case PIPE_FORMAT_B10G10R10A2_UNORM:
                case PIPE_FORMAT_R10G10B10X2_UNORM:
                case PIPE_FORMAT_B10G10R10X2_UNORM:
                case PIPE_FORMAT_R10G10B10A2_UINT:
                        return PAN_FORMAT_PACK;
                default:
                        return PAN_FORMAT_NATIVE;
                }
        }

        /* Otherwise, we can do native */
        return PAN_FORMAT_NATIVE;
}

enum pan_format_class
pan_format_class_store(const struct util_format_description *desc, unsigned quirks)
{
        /* Check if we can do anything better than software architecturally */
        if (quirks & MIDGARD_NO_TYPED_BLEND_STORES) {
                return (quirks & NO_BLEND_PACKS)
                        ? PAN_FORMAT_SOFTWARE : PAN_FORMAT_PACK;
        }

        return PAN_FORMAT_NATIVE;
}

/* Software packs/unpacks, by format class. Packs take in the pixel value typed
 * as `pan_unpacked_type_for_format` of the format and return an i32vec4
 * suitable for storing (with components replicated to fill). Unpacks do the
 * reverse but cannot rely on replication.
 *
 * Pure 32 formats (R32F ... RGBA32F) are 32 unpacked, so just need to
 * replicate to fill */

static nir_ssa_def *
pan_pack_pure_32(nir_builder *b, nir_ssa_def *v)
{
        nir_ssa_def *replicated[4];

        for (unsigned i = 0; i < 4; ++i)
                replicated[i] = nir_channel(b, v, i % v->num_components);

        return nir_vec(b, replicated, 4);
}

static nir_ssa_def *
pan_unpack_pure_32(nir_builder *b, nir_ssa_def *pack, unsigned num_components)
{
        return nir_channels(b, pack, (1 << num_components) - 1);
}

/* Pure x16 formats are x16 unpacked, so it's similar, but we need to pack
 * upper/lower halves of course */

static nir_ssa_def *
pan_pack_pure_16(nir_builder *b, nir_ssa_def *v)
{
        nir_ssa_def *replicated[4];

        for (unsigned i = 0; i < 4; ++i) {
                unsigned c = 2 * i;

                nir_ssa_def *parts[2] = {
                        nir_channel(b, v, (c + 0) % v->num_components),
                        nir_channel(b, v, (c + 1) % v->num_components)
                };

                replicated[i] = nir_pack_32_2x16(b, nir_vec(b, parts, 2));
        }

        return nir_vec(b, replicated, 4);
}

static nir_ssa_def *
pan_unpack_pure_16(nir_builder *b, nir_ssa_def *pack, unsigned num_components)
{
        nir_ssa_def *unpacked[4];

        assert(num_components <= 4);

        for (unsigned i = 0; i < num_components; i += 2) {
                nir_ssa_def *halves = 
                        nir_unpack_32_2x16(b, nir_channel(b, pack, i >> 1));

                unpacked[i + 0] = nir_channel(b, halves, 0);
                unpacked[i + 1] = nir_channel(b, halves, 1);
        }

        for (unsigned i = num_components; i < 4; ++i)
                unpacked[i] = nir_imm_intN_t(b, 0, 16);

        return nir_vec(b, unpacked, 4);
}

/* Generic dispatches for un/pack regardless of format */

static nir_ssa_def *
pan_unpack(nir_builder *b,
                const struct util_format_description *desc,
                nir_ssa_def *packed)
{
        /* Stub */
        return packed;
}

static nir_ssa_def *
pan_pack(nir_builder *b,
                const struct util_format_description *desc,
                nir_ssa_def *unpacked)
{
        /* Stub */
        return unpacked;
}

static void
pan_lower_fb_store(nir_shader *shader,
                nir_builder *b,
                nir_intrinsic_instr *intr,
                const struct util_format_description *desc,
                unsigned quirks)
{
        /* For stores, add conversion before */
        nir_ssa_def *unpacked = nir_ssa_for_src(b, intr->src[1], 4);
        nir_ssa_def *packed = pan_pack(b, desc, unpacked);

        nir_intrinsic_instr *new =
                nir_intrinsic_instr_create(shader, nir_intrinsic_store_raw_output_pan);
        new->src[0] = nir_src_for_ssa(packed);
        new->num_components = 4;
        nir_builder_instr_insert(b, &new->instr);
}

static void
pan_lower_fb_load(nir_shader *shader,
                nir_builder *b,
                nir_intrinsic_instr *intr,
                const struct util_format_description *desc,
                unsigned quirks)
{
        nir_intrinsic_instr *new = nir_intrinsic_instr_create(shader,
                       nir_intrinsic_load_raw_output_pan);
        new->num_components = 4;

        nir_ssa_dest_init(&new->instr, &new->dest, 4, 32, NULL);
        nir_builder_instr_insert(b, &new->instr);

        /* Convert the raw value */
        nir_ssa_def *packed = &new->dest.ssa;
        nir_ssa_def *unpacked = pan_unpack(b, desc, packed);

        nir_src rewritten = nir_src_for_ssa(unpacked);
        nir_ssa_def_rewrite_uses_after(&intr->dest.ssa, rewritten, &intr->instr);
}

void
pan_lower_framebuffer(nir_shader *shader,
                const struct util_format_description *desc,
                unsigned quirks)
{
        /* Blend shaders are represented as special fragment shaders */
        assert(shader->info.stage == MESA_SHADER_FRAGMENT);

        nir_foreach_function(func, shader) {
                nir_foreach_block(block, func->impl) {
                        nir_foreach_instr_safe(instr, block) {
                                if (instr->type != nir_instr_type_intrinsic)
                                        continue;

                                nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

                                bool is_load = intr->intrinsic == nir_intrinsic_load_deref;
                                bool is_store = intr->intrinsic == nir_intrinsic_store_deref;

                                if (!(is_load || is_store))
                                        continue;

                                /* Don't worry about MRT */
                                nir_variable *var = nir_intrinsic_get_var(intr, 0);

                                if (var->data.location != FRAG_RESULT_COLOR)
                                        continue;

                                nir_builder b;
                                nir_builder_init(&b, func->impl);

                                if (is_store) {
                                        b.cursor = nir_before_instr(instr);
                                        pan_lower_fb_store(shader, &b, intr, desc, quirks);
                                } else {
                                        b.cursor = nir_after_instr(instr);
                                        pan_lower_fb_load(shader, &b, intr, desc, quirks);
                                }

                                nir_instr_remove(instr);
                        }
                }

                nir_metadata_preserve(func->impl, nir_metadata_block_index |
                                nir_metadata_dominance);
        }
}

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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "compiler.h"

/* NIR creates vectors as vecN ops, which we represent by a synthetic
 * BI_COMBINE instruction, e.g.:
 *
 *      v = combine x, y, z, w
 *
 * These combines need to be lowered by the pass in this file.
 */

static void
bi_insert_combine_mov(bi_context *ctx, bi_instruction *parent, unsigned comp)
{
        unsigned bits = nir_alu_type_get_type_size(parent->dest_type);
        unsigned bytes = bits / 8;

        bi_instruction move = {
                .type = BI_MOV,
                .dest = parent->dest,
                .dest_type = parent->dest_type,
                .writemask = ((1 << bytes) - 1) << (bytes * comp),
                .src = { parent->src[comp] },
                .src_types = { parent->dest_type },
                .swizzle = { { parent->swizzle[comp][0] } }
        };

        bi_emit_before(ctx, parent, move);
}

void
bi_lower_combine(bi_context *ctx, bi_block *block)
{
        bi_foreach_instr_in_block_safe(block, ins) {
                if (ins->type != BI_COMBINE) continue;

                bi_foreach_src(ins, s) {
                        if (!ins->src[s])
                                break;

                        bi_insert_combine_mov(ctx, ins, s);
                }

                bi_remove_instruction(ins);
        }
}

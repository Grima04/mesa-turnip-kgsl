/*
 * Copyright (C) 2018 Alyssa Rosenzweig
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
 */

#include "compiler.h"

/* A simple scalar-only SSA-based copy-propagation pass. O(N^2) due to the lack
 * of use tracking. TODO: better data structures for O(N), TODO: vectors */

static bool
bi_rewrite_scalar_uses(bi_context *ctx, bi_index old, bi_index new)
{
        bool progress = false;

        bi_foreach_instr_global(ctx, use) {
                bi_foreach_src(use, s) {
                        bi_index src = use->src[s];
                        bool scalar = (bi_count_read_registers(use, s) == 1);

                        if (bi_is_word_equiv(src, old) && scalar) {
                                use->src[s] = bi_replace_index(src, new);
                                progress = true;
                        }
                }
        }

        return progress;
}

bool
bi_opt_copy_prop(bi_context *ctx)
{
        bool progress = false;

        bi_foreach_instr_global_safe(ctx, ins) {
                if (ins->op != BI_OPCODE_MOV_I32) continue;
                if (!bi_is_ssa(ins->dest[0])) continue;
                if (!(bi_is_ssa(ins->src[0]) || ins->src[0].type == BI_INDEX_FAU)) continue;

                progress |= bi_rewrite_scalar_uses(ctx, ins->dest[0], ins->src[0]);
        }

        return progress;
}

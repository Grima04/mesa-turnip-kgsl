/*
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
 *
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#include "compiler.h"
#include "panfrost/util/lcra.h"
#include "util/u_memory.h"

static void
bi_compute_interference(bi_context *ctx, struct lcra_state *l)
{
        bi_compute_liveness(ctx);

        bi_foreach_block(ctx, _blk) {
                bi_block *blk = (bi_block *) _blk;
                uint16_t *live = mem_dup(_blk->live_out, l->node_count * sizeof(uint16_t));

                bi_foreach_instr_in_block_rev(blk, ins) {
                        /* Mark all registers live after the instruction as
                         * interfering with the destination */

                        if (ins->dest && (ins->dest < l->node_count)) {
                                for (unsigned i = 1; i < l->node_count; ++i) {
                                        if (live[i])
                                                lcra_add_node_interference(l, ins->dest, ins->writemask, i, live[i]);
                                }
                        }

                        /* Update live_in */
                        bi_liveness_ins_update(live, ins, l->node_count);
                }

                free(live);
        }
}

enum {
        BI_REG_CLASS_WORK = 0,
} bi_reg_class;

static struct lcra_state *
bi_allocate_registers(bi_context *ctx, bool *success)
{
        unsigned node_count = bi_max_temp(ctx);

        struct lcra_state *l =
                lcra_alloc_equations(node_count, 1, 8, 16, 1);

        l->class_start[BI_REG_CLASS_WORK] = 0;
        l->class_size[BI_REG_CLASS_WORK] = 64 * 4; /* R0 - R63, all 32-bit */

        bi_foreach_instr_global(ctx, ins) {
                unsigned dest = ins->dest;

                if (!dest || (dest >= node_count))
                        continue;

                l->class[dest] = BI_REG_CLASS_WORK;
                lcra_set_alignment(l, dest, 2); /* 2^2 = 4 */
                lcra_restrict_range(l, dest, 4);
        }

        bi_compute_interference(ctx, l);

        *success = lcra_solve(l);

        return l;
}

void
bi_register_allocate(bi_context *ctx)
{
        struct lcra_state *l = NULL;
        bool success = false;

        do {
                if (l) {
                        lcra_free(l);
                        l = NULL;
                }

                bi_invalidate_liveness(ctx);
                l = bi_allocate_registers(ctx, &success);

                /* TODO: Spilling */
                assert(success);
        } while(!success);

        lcra_free(l);
}

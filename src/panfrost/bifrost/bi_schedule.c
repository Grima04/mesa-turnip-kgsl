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

/* Finds the clause type required or return none */

static enum bifrost_clause_type
bi_clause_type_for_ins(bi_instruction *ins)
{
        unsigned T = ins->type;

        /* Only high latency ops impose clause types */
        if (!(bi_class_props[T] & BI_SCHED_HI_LATENCY))
                return BIFROST_CLAUSE_NONE;

        switch (T) {
        case BI_BRANCH:
        case BI_DISCARD:
                return BIFROST_CLAUSE_NONE;

        case BI_LOAD_VAR:
                return BIFROST_CLAUSE_LOAD_VARY;

        case BI_LOAD_UNIFORM:
        case BI_LOAD_ATTR:
        case BI_LOAD_VAR_ADDRESS:
                return BIFROST_CLAUSE_UBO;

        case BI_TEX:
                return BIFROST_CLAUSE_TEX;

        case BI_LOAD:
                return BIFROST_CLAUSE_SSBO_LOAD;

        case BI_STORE:
        case BI_STORE_VAR:
                return BIFROST_CLAUSE_SSBO_STORE;

        case BI_BLEND:
                return BIFROST_CLAUSE_BLEND;

        case BI_ATEST:
                return BIFROST_CLAUSE_ATEST;

        default:
                unreachable("Invalid high-latency class");
        }
}

/* Eventually, we'll need a proper scheduling, grouping instructions
 * into clauses and ordering/assigning grouped instructions to the
 * appropriate FMA/ADD slots. Right now we do the dumbest possible
 * thing just to have the scheduler stubbed out so we can focus on
 * codegen */

void
bi_schedule(bi_context *ctx)
{
        unsigned ids = 0;
        unsigned last_id = 0;
        bool is_first = true;

        bi_foreach_block(ctx, block) {
                bi_block *bblock = (bi_block *) block;

                list_inithead(&bblock->clauses);

                bi_foreach_instr_in_block(bblock, ins) {
                        unsigned props = bi_class_props[ins->type];

                        bi_clause *u = rzalloc(ctx, bi_clause);
                        u->bundle_count = 1;

                        if (props & BI_SCHED_FMA)
                                u->bundles[0].fma = ins;
                        else
                                u->bundles[0].add = ins;

                        u->scoreboard_id = ids++;

                        if (is_first)
                                is_first = false;
                        else
                                u->dependencies |= (1 << last_id);

                        ids = ids & 1;
                        last_id = u->scoreboard_id;
                        u->back_to_back = true;
                        u->data_register_write_barrier = true;

                        u->constant_count = 1;
                        u->constants[0] = ins->constant.u64;

                        u->clause_type = bi_clause_type_for_ins(ins);

                        list_addtail(&u->link, &bblock->clauses);
                }

                bblock->scheduled = true;
        }
}

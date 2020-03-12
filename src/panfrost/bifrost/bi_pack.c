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

/* This file contains the final passes of the compiler. Running after
 * scheduling and RA, the IR is now finalized, so we need to emit it to actual
 * bits on the wire (as well as fixup branches) */

static uint64_t
bi_pack_header(bi_clause *clause, bi_clause *next)
{
        struct bifrost_header header = {
                /* stub */
                .no_end_of_shader = (next != NULL),
        };

        uint64_t u = 0;
        memcpy(&u, &header, sizeof(header));
        return u;
}

static void
bi_pack_clause(bi_context *ctx, bi_clause *clause, bi_clause *next,
                struct util_dynarray *emission)
{
        
        struct bifrost_fmt1 quad_1 = {
                .tag = BIFROST_FMT1_FINAL,
                .header = bi_pack_header(clause, next) 
        };

        util_dynarray_append(emission, struct bifrost_fmt1, quad_1);
}

static bi_clause *
bi_next_clause(bi_context *ctx, pan_block *block, bi_clause *clause)
{
        /* Try the next clause in this block */
        if (clause->link.next != &((bi_block *) block)->clauses)
                return list_first_entry(&(clause->link), bi_clause, link);

        /* Try the next block, or the one after that if it's empty, etc .*/
        pan_block *next_block = pan_next_block(block);

        bi_foreach_block_from(ctx, next_block, block) {
                bi_block *blk = (bi_block *) block;

                if (!list_is_empty(&blk->clauses))
                        return list_first_entry(&(blk->clauses), bi_clause, link);
        }

        return NULL;
}

void
bi_pack(bi_context *ctx, struct util_dynarray *emission)
{
        util_dynarray_init(emission, NULL);

        bi_foreach_block(ctx, _block) {
                bi_block *block = (bi_block *) _block;

                bi_foreach_clause_in_block(block, clause) {
                        bi_clause *next = bi_next_clause(ctx, _block, clause);
                        bi_pack_clause(ctx, clause, next, emission);
                }
        }
}

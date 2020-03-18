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

/* Represents the assignment of ports for a given bundle */

struct bi_registers {
        /* Register to assign to each port */
        unsigned port[4];

        /* Read ports can be disabled */
        bool enabled[2];

        /* Should we write FMA? what about ADD? If only a single port is
         * enabled it is in port 2, else ADD/FMA is 2/3 respectively */
        bool write_fma, write_add;

        /* Should we read with port 3? */
        bool read_port3;

        /* Packed uniform/constant */
        unsigned uniform_constant;

        /* Whether writes are actually for the last instruction */
        bool first_instruction;
};

/* Determines the register control field, ignoring the first? flag */

static enum bifrost_reg_control
bi_pack_register_ctrl_lo(struct bi_registers r)
{
        if (r.write_fma) {
                if (r.write_add) {
                        assert(!r.read_port3);
                        return BIFROST_WRITE_ADD_P2_FMA_P3;
                } else {
                        if (r.read_port3)
                                return BIFROST_WRITE_FMA_P2_READ_P3;
                        else
                                return BIFROST_WRITE_FMA_P2;
                }
        } else if (r.write_add) {
                if (r.read_port3)
                        return BIFROST_WRITE_ADD_P2_READ_P3;
                else
                        return BIFROST_WRITE_ADD_P2;
        } else if (r.read_port3)
                return BIFROST_READ_P3;
        else
                return BIFROST_REG_NONE;
}

/* Ditto but account for the first? flag this time */

static enum bifrost_reg_control
bi_pack_register_ctrl(struct bi_registers r)
{
        enum bifrost_reg_control ctrl = bi_pack_register_ctrl_lo(r);

        if (r.first_instruction) {
                if (ctrl == BIFROST_REG_NONE)
                        ctrl = BIFROST_FIRST_NONE;
                else
                        ctrl |= BIFROST_FIRST_NONE;
        }

        return ctrl;
}

static unsigned
bi_pack_registers(struct bi_registers regs)
{
        /* TODO */
        return 0;
}

static unsigned
bi_pack_fma(bi_clause *clause, bi_bundle bundle)
{
        /* TODO */
        return BIFROST_FMA_NOP;
}

static unsigned
bi_pack_add(bi_clause *clause, bi_bundle bundle)
{
        /* TODO */
        return BIFROST_ADD_NOP;
}

struct bi_packed_bundle {
        uint64_t lo;
        uint64_t hi;
};

static struct bi_packed_bundle
bi_pack_bundle(bi_clause *clause, bi_bundle bundle)
{
        unsigned reg = /*bi_pack_registers(clause, bundle)*/0;
        uint64_t fma = bi_pack_fma(clause, bundle);
        uint64_t add = bi_pack_add(clause, bundle);

        struct bi_packed_bundle packed = {
                .lo = reg | (fma << 35) | ((add & 0b111111) << 58),
                .hi = add >> 6
        };

        return packed;
}

static void
bi_pack_clause(bi_context *ctx, bi_clause *clause, bi_clause *next,
                struct util_dynarray *emission)
{
        struct bi_packed_bundle ins_1 = bi_pack_bundle(clause, clause->bundles[0]);
        assert(clause->bundle_count == 1);

        struct bifrost_fmt1 quad_1 = {
                .tag = BIFROST_FMT1_FINAL,
                .header = bi_pack_header(clause, next),
                .ins_1 = ins_1.lo,
                .ins_2 = ins_1.hi & ((1 << 11) - 1),
                .ins_0 = (ins_1.hi >> 11) & 0b111,
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

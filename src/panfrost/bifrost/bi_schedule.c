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
#include "bi_builder.h"

/* Arguments common to worklist, passed by value for convenience */

struct bi_worklist {
        /* # of instructions in the block */
        unsigned count;

        /* Instructions in the block */
        bi_instr **instructions;

        /* Bitset of instructions in the block ready for scheduling */
        BITSET_WORD *worklist;
};

/* State of a single tuple and clause under construction */

struct bi_reg_state {
        /* Number of register writes */
        unsigned nr_writes;

        /* Register reads, expressed as (equivalence classes of)
         * sources. Only 3 reads are allowed, but up to 2 may spill as
         * "forced" for the next scheduled tuple, provided such a tuple
         * can be constructed */
        bi_index reads[5];
        unsigned nr_reads;

        /* The previous tuple scheduled (= the next tuple executed in the
         * program) may require certain writes, in order to bypass the register
         * file and use a temporary passthrough for the value. Up to 2 such
         * constraints are architecturally satisfiable */
        unsigned forced_count;
        bi_index forceds[2];
};

struct bi_tuple_state {
        /* Is this the last tuple in the clause */
        bool last;

        /* Scheduled ADD instruction, or null if none */
        bi_instr *add;

        /* Reads for previous (succeeding) tuple */
        bi_index prev_reads[5];
        unsigned nr_prev_reads;
        bi_tuple *prev;

        /* Register slot state for current tuple */
        struct bi_reg_state reg;

        /* Constants are shared in the tuple. If constant_count is nonzero, it
         * is a size for constant count. Otherwise, fau is the slot read from
         * FAU, or zero if none is assigned. Ordinarily FAU slot 0 reads zero,
         * but within a tuple, that should be encoded as constant_count != 0
         * and constants[0] = constants[1] = 0 */
        unsigned constant_count;

        union {
                uint32_t constants[2];
                enum bir_fau fau;
        };

        unsigned pcrel_idx;
};

struct bi_const_state {
        unsigned constant_count;
        bool pcrel; /* applies to first const */
        uint32_t constants[2];

        /* Index of the constant into the clause */
        unsigned word_idx;
};

struct bi_clause_state {
        /* Has a message-passing instruction already been assigned? */
        bool message;

        /* Indices already read, this needs to be tracked to avoid hazards
         * around message-passing instructions */
        unsigned read_count;
        bi_index reads[BI_MAX_SRCS * 16];

        unsigned tuple_count;
        struct bi_const_state consts[8];
};

/* Determines messsage type by checking the table and a few special cases. Only
 * case missing is tilebuffer instructions that access depth/stencil, which
 * require a Z_STENCIL message (to implement
 * ARM_shader_framebuffer_fetch_depth_stencil) */

static enum bifrost_message_type
bi_message_type_for_instr(bi_instr *ins)
{
        enum bifrost_message_type msg = bi_opcode_props[ins->op].message;
        bool ld_var_special = (ins->op == BI_OPCODE_LD_VAR_SPECIAL);

        if (ld_var_special && ins->varying_name == BI_VARYING_NAME_FRAG_Z)
                return BIFROST_MESSAGE_Z_STENCIL;

        if (msg == BIFROST_MESSAGE_LOAD && ins->seg == BI_SEG_UBO)
                return BIFROST_MESSAGE_ATTRIBUTE;

        return msg;
}

/* To work out the back-to-back flag, we need to detect branches and
 * "fallthrough" branches, implied in the last clause of a block that falls
 * through to another block with *multiple predecessors*. */

static bool
bi_back_to_back(bi_block *block)
{
        /* Last block of a program */
        if (!block->base.successors[0]) {
                assert(!block->base.successors[1]);
                return false;
        }

        /* Multiple successors? We're branching */
        if (block->base.successors[1])
                return false;

        struct pan_block *succ = block->base.successors[0];
        assert(succ->predecessors);
        unsigned count = succ->predecessors->entries;

        /* Back to back only if the successor has only a single predecessor */
        return (count == 1);
}

/* Insert a clause wrapping a single instruction */

bi_clause *
bi_singleton(void *memctx, bi_instr *ins,
                bi_block *block,
                unsigned scoreboard_id,
                unsigned dependencies,
                bool osrb)
{
        bi_clause *u = rzalloc(memctx, bi_clause);
        u->tuple_count = 1;

        ASSERTED bool can_fma = bi_opcode_props[ins->op].fma;
        bool can_add = bi_opcode_props[ins->op].add;
        assert(can_fma || can_add);

        if (can_add)
                u->tuples[0].add = ins;
        else
                u->tuples[0].fma = ins;

        u->scoreboard_id = scoreboard_id;
        u->staging_barrier = osrb;
        u->dependencies = dependencies;

        if (ins->op == BI_OPCODE_ATEST)
                u->dependencies |= (1 << 6);

        if (ins->op == BI_OPCODE_BLEND)
                u->dependencies |= (1 << 6) | (1 << 7);

        /* Let's be optimistic, we'll fix up later */
        u->flow_control = BIFROST_FLOW_NBTB;

        /* Build up a combined constant, count in 32-bit words */
        uint64_t combined_constant = 0;
        unsigned constant_count = 0;

        bi_foreach_src(ins, s) {
                if (ins->src[s].type != BI_INDEX_CONSTANT) continue;
                unsigned value = ins->src[s].value;

                /* Allow fast zero */
                if (value == 0 && u->tuples[0].fma) continue;

                if (constant_count == 0) {
                        combined_constant = ins->src[s].value;
                } else if (constant_count == 1) {
                        /* Allow reuse */
                        if (combined_constant == value)
                                continue;

                        combined_constant |= ((uint64_t) value) << 32ull;
                } else {
                        /* No more room! */
                        assert((combined_constant & 0xffffffff) == value ||
                                        (combined_constant >> 32ull) == value);
                }

                constant_count++;
        }

        if (ins->branch_target)
                u->branch_constant = true;

        /* XXX: Investigate errors when constants are not used */
        if (constant_count || u->branch_constant || true) {
                /* Clause in 64-bit, above in 32-bit */
                u->constant_count = 1;
                u->constants[0] = combined_constant;
        }

        u->next_clause_prefetch = (ins->op != BI_OPCODE_JUMP);
        u->message_type = bi_message_type_for_instr(ins);
        u->block = block;

        return u;
}

/* Scheduler predicates */

ASSERTED static bool
bi_can_fma(bi_instr *ins)
{
        /* TODO: some additional fp16 constraints */
        return bi_opcode_props[ins->op].fma;
}

ASSERTED static bool
bi_can_add(bi_instr *ins)
{
        /* TODO: some additional fp16 constraints */
        return bi_opcode_props[ins->op].add;
}

ASSERTED static bool
bi_must_last(bi_instr *ins)
{
        return bi_opcode_props[ins->op].last;
}

ASSERTED static bool
bi_must_message(bi_instr *ins)
{
        return bi_opcode_props[ins->op].message != BIFROST_MESSAGE_NONE;
}

static bool
bi_fma_atomic(enum bi_opcode op)
{
        switch (op) {
        case BI_OPCODE_ATOM_C_I32:
        case BI_OPCODE_ATOM_C_I64:
        case BI_OPCODE_ATOM_C1_I32:
        case BI_OPCODE_ATOM_C1_I64:
        case BI_OPCODE_ATOM_C1_RETURN_I32:
        case BI_OPCODE_ATOM_C1_RETURN_I64:
        case BI_OPCODE_ATOM_C_RETURN_I32:
        case BI_OPCODE_ATOM_C_RETURN_I64:
        case BI_OPCODE_ATOM_POST_I32:
        case BI_OPCODE_ATOM_POST_I64:
        case BI_OPCODE_ATOM_PRE_I64:
                return true;
        default:
                return false;
        }
}

ASSERTED static bool
bi_reads_zero(bi_instr *ins)
{
        return !(bi_fma_atomic(ins->op) || ins->op == BI_OPCODE_IMULD);
}

static bool
bi_reads_temps(bi_instr *ins, unsigned src)
{
        switch (ins->op) {
        /* Cannot permute a temporary */
        case BI_OPCODE_CLPER_V6_I32:
        case BI_OPCODE_CLPER_V7_I32:
                return src != 0;
        case BI_OPCODE_IMULD:
                return false;
        default:
                return true;
        }
}

ASSERTED static bool
bi_reads_t(bi_instr *ins, unsigned src)
{
        /* Branch offset cannot come from passthrough */
        if (bi_opcode_props[ins->op].branch)
                return src != 2;

        /* Table can never read passthrough */
        if (bi_opcode_props[ins->op].table)
                return false;

        /* Staging register reads may happen before the succeeding register
         * block encodes a write, so effectively there is no passthrough */
        if (src == 0 && bi_opcode_props[ins->op].sr_read)
                return false;

        /* Descriptor must not come from a passthrough */
        switch (ins->op) {
        case BI_OPCODE_LD_CVT:
        case BI_OPCODE_LD_TILE:
        case BI_OPCODE_ST_CVT:
        case BI_OPCODE_ST_TILE:
        case BI_OPCODE_TEXC:
                return src != 2;
        case BI_OPCODE_BLEND:
                return src != 2 && src != 3;

        /* Else, just check if we can read any temps */
        default:
                return bi_reads_temps(ins, src);
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
        bool is_first = true;

        bi_foreach_block(ctx, block) {
                bi_block *bblock = (bi_block *) block;

                list_inithead(&bblock->clauses);

                bi_foreach_instr_in_block(bblock, ins) {
                        bi_clause *u = bi_singleton(ctx, ins,
                                        bblock, 0, (1 << 0),
                                        !is_first);

                        is_first = false;
                        list_addtail(&u->link, &bblock->clauses);
                }

                /* Back-to-back bit affects only the last clause of a block,
                 * the rest are implicitly true */

                if (!list_is_empty(&bblock->clauses)) {
                        bi_clause *last_clause = list_last_entry(&bblock->clauses, bi_clause, link);
                        if (!bi_back_to_back(bblock))
                                last_clause->flow_control = BIFROST_FLOW_NBTB_UNCONDITIONAL;
                }

                bblock->scheduled = true;
        }
}

#ifndef NDEBUG

static bi_builder *
bit_builder(void *memctx)
{
        bi_context *ctx = rzalloc(memctx, bi_context);
        list_inithead(&ctx->blocks);

        bi_block *blk = rzalloc(ctx, bi_block);

        blk->base.predecessors = _mesa_set_create(blk,
                        _mesa_hash_pointer,
                        _mesa_key_pointer_equal);

        list_addtail(&blk->base.link, &ctx->blocks);
        list_inithead(&blk->base.instructions);

        bi_builder *b = rzalloc(memctx, bi_builder);
        b->shader = ctx;
        b->cursor = bi_after_block(blk);
        return b;
}

#define TMP() bi_temp(b->shader)

static void
bi_test_units(bi_builder *b)
{
        bi_instr *mov = bi_mov_i32_to(b, TMP(), TMP());
        assert(bi_can_fma(mov));
        assert(bi_can_add(mov));
        assert(!bi_must_last(mov));
        assert(!bi_must_message(mov));
        assert(bi_reads_zero(mov));
        assert(bi_reads_temps(mov, 0));
        assert(bi_reads_t(mov, 0));

        bi_instr *fma = bi_fma_f32_to(b, TMP(), TMP(), TMP(), bi_zero(), BI_ROUND_NONE);
        assert(bi_can_fma(fma));
        assert(!bi_can_add(fma));
        assert(!bi_must_last(fma));
        assert(!bi_must_message(fma));
        assert(bi_reads_zero(fma));
        for (unsigned i = 0; i < 3; ++i) {
                assert(bi_reads_temps(fma, i));
                assert(bi_reads_t(fma, i));
        }

        bi_instr *load = bi_load_i128_to(b, TMP(), TMP(), TMP(), BI_SEG_UBO);
        assert(!bi_can_fma(load));
        assert(bi_can_add(load));
        assert(!bi_must_last(load));
        assert(bi_must_message(load));
        for (unsigned i = 0; i < 2; ++i) {
                assert(bi_reads_temps(load, i));
                assert(bi_reads_t(load, i));
        }

        bi_instr *blend = bi_blend_to(b, TMP(), TMP(), TMP(), TMP(), TMP());
        assert(!bi_can_fma(load));
        assert(bi_can_add(load));
        assert(bi_must_last(blend));
        assert(bi_must_message(blend));
        for (unsigned i = 0; i < 4; ++i)
                assert(bi_reads_temps(blend, i));
        assert(!bi_reads_t(blend, 0));
        assert(bi_reads_t(blend, 1));
        assert(!bi_reads_t(blend, 2));
        assert(!bi_reads_t(blend, 3));
}

int bi_test_scheduler(void)
{
        void *memctx = NULL;

        bi_test_units(bit_builder(memctx));

        return 0;
}
#endif

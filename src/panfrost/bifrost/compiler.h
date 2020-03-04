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

#ifndef __BIFROST_COMPILER_H
#define __BIFROST_COMPILER_H

#include "bifrost.h"
#include "compiler/nir/nir.h"

/* Bifrost opcodes are tricky -- the same op may exist on both FMA and
 * ADD with two completely different opcodes, and opcodes can be varying
 * length in some cases. Then we have different opcodes for int vs float
 * and then sometimes even for different typesizes. Further, virtually
 * every op has a number of flags which depend on the op. In constrast
 * to Midgard where you have a strict ALU/LDST/TEX division and within
 * ALU you have strict int/float and that's it... here it's a *lot* more
 * involved. As such, we use something much higher level for our IR,
 * encoding "classes" of operations, letting the opcode details get
 * sorted out at emit time.
 *
 * Please keep this list alphabetized. Please use a dictionary if you
 * don't know how to do that.
 */

enum bi_class {
        BI_ADD,
        BI_ATEST,
        BI_BRANCH,
        BI_CMP,
        BI_BLEND,
        BI_BITWISE,
        BI_CONVERT,
        BI_CSEL,
        BI_DISCARD,
        BI_EXTRACT,
        BI_FMA,
        BI_FREXP,
        BI_LOAD,
        BI_LOAD_ATTR,
        BI_LOAD_VAR,
        BI_LOAD_VAR_ADDRESS,
        BI_MAKE_VEC,
        BI_MINMAX,
        BI_MOV,
        BI_SHIFT,
        BI_STORE,
        BI_STORE_VAR,
        BI_SPECIAL, /* _FAST, _TABLE on supported GPUs */
        BI_SWIZZLE,
        BI_TEX,
        BI_ROUND,
        BI_NUM_CLASSES
};

/* Properties of a class... */
extern unsigned bi_class_props[BI_NUM_CLASSES];

/* abs/neg/outmod valid for a float op */
#define BI_MODS (1 << 0)

/* Generic enough that little class-specific information is required. In other
 * words, it acts as a "normal" ALU op, even if the encoding ends up being
 * irregular enough to warrant a separate class */
#define BI_GENERIC (1 << 1)

/* Accepts a bifrost_roundmode */
#define BI_ROUNDMODE (1 << 2)

/* Can be scheduled to FMA */
#define BI_SCHED_FMA (1 << 3)

/* Can be scheduled to ADD */
#define BI_SCHED_ADD (1 << 4)

/* Most ALU ops can do either, actually */
#define BI_SCHED_ALL (BI_SCHED_FMA | BI_SCHED_ADD)

/* Along with setting BI_SCHED_ADD, eats up the entire cycle, so FMA must be
 * nopped out. Used for _FAST operations. */
#define BI_SCHED_SLOW (1 << 5)

/* Swizzling allowed for the 8/16-bit source */
#define BI_SWIZZLABLE (1 << 6)

/* For scheduling purposes this is a high latency instruction and must be at
 * the end of a clause. Implies ADD */
#define BI_SCHED_HI_LATENCY ((1 << 7) | BI_SCHED_ADD)

/* It can't get any worse than csel4... can it? */
#define BIR_SRC_COUNT 4

/* Class-specific data for BI_LD_ATTR, BI_LD_VAR_ADDR */
struct bi_load {
        /* Note: no indirects here */
        unsigned location;

        /* Only for BI_LD_ATTR. But number of vector channels */
        unsigned channels;
};

/* BI_LD_VARY */
struct bi_load_vary {
        /* All parameters used here. Indirect location specified in
         * src1 and ignoring location, if present. */
        struct bi_load load;

        enum bifrost_interp_mode interp_mode;
        bool reuse;
        bool flat;
};

/* BI_BRANCH encoding the details of the branch itself as well as a pointer to
 * the target. We forward declare bi_block since this is mildly circular (not
 * strictly, but this order of the file makes more sense I think)
 *
 * We define our own enum of conditions since the conditions in the hardware
 * packed in crazy ways that would make manipulation unweildly (meaning changes
 * based on port swapping, etc), so we defer dealing with that until emit time.
 * Likewise, we expose NIR types instead of the crazy branch types, although
 * the restrictions do eventually apply of course. */

struct bi_block;

enum bi_cond {
        BI_COND_ALWAYS,
        BI_COND_LT,
        BI_COND_LE,
        BI_COND_GE,
        BI_COND_GT,
        BI_COND_EQ,
        BI_COND_NE,
};

struct bi_branch {
        /* Types are specified in src_types and must be compatible (either both
         * int, or both float, 16/32, and same size or 32/16 if float. Types
         * ignored if BI_COND_ALWAYS is set for an unconditional branch. */

        enum bi_cond cond;
        struct bi_block *target;
};

/* Opcodes within a class */
enum bi_minmax_op {
        BI_MINMAX_MIN,
        BI_MINMAX_MAX
};

enum bi_bitwise_op {
        BI_BITWISE_AND,
        BI_BITWISE_OR,
        BI_BITWISE_XOR
};

enum bi_round_op {
        BI_ROUND_MODE, /* use round mode */
        BI_ROUND_ROUND /* i.e.: fround() */
};

typedef struct {
        struct list_head link; /* Must be first */
        enum bi_class type;

        /* Indices, see bir_ssa_index etc. Note zero is special cased
         * to "no argument" */
        unsigned dest;
        unsigned src[BIR_SRC_COUNT];

        /* If one of the sources has BIR_INDEX_CONSTANT... Also, for
         * BI_EXTRACT, the component index is stored here. */
        union {
                uint64_t u64;
                uint32_t u32;
                uint16_t u16[2];
                uint8_t u8[4];
        } constant;

        /* Floating-point modifiers, type/class permitting. If not
         * allowed for the type/class, these are ignored. */
        enum bifrost_outmod outmod;
        bool src_abs[BIR_SRC_COUNT];
        bool src_neg[BIR_SRC_COUNT];

        /* Round mode (requires BI_ROUNDMODE) */
        enum bifrost_roundmode roundmode;

        /* Destination type. Usually the type of the instruction
         * itself, but if sources and destination have different
         * types, the type of the destination wins (so f2i would be
         * int). Zero if there is no destination. Bitsize included */
        nir_alu_type dest_type;

        /* Source types if required by the class */
        nir_alu_type src_types[BIR_SRC_COUNT];

        /* If the source type is 8-bit or 16-bit such that SIMD is possible, and
         * the class has BI_SWIZZLABLE, this is a swizzle for the input. Swizzles
         * in practice only occur with one-source arguments (conversions,
         * dedicated swizzle ops) and as component selection on two-sources
         * where it is unambiguous which is which. Bounds are 32/type_size. */
        unsigned swizzle[4];

        /* A class-specific op from which the actual opcode can be derived
         * (along with the above information) */

        union {
                enum bi_minmax_op minmax;
                enum bi_bitwise_op bitwise;
                enum bi_round_op round;
        } op;

        /* Union for class-specific information */
        union {
                enum bifrost_minmax_mode minmax;
                struct bi_load load;
                struct bi_load_vary load_vary;
                struct bi_branch branch;

                /* For CSEL, the comparison op. BI_COND_ALWAYS doesn't make
                 * sense here but you can always just use a move for that */
                enum bi_cond csel_cond;
        };
} bi_instruction;

/* Scheduling takes place in two steps. Step 1 groups instructions within a
 * block into distinct clauses (bi_clause). Step 2 schedules instructions
 * within a clause into FMA/ADD pairs (bi_bundle).
 *
 * A bi_bundle contains two paired instruction pointers. If a slot is unfilled,
 * leave it NULL; the emitter will fill in a nop.
 */

typedef struct {
        bi_instruction *fma;
        bi_instruction *add;
} bi_bundle;

typedef struct {
        struct list_head link;

        /* A clause can have 8 instructions in bundled FMA/ADD sense, so there
         * can be 8 bundles. But each bundle can have both an FMA and an ADD,
         * so a clause can have up to 16 bi_instructions. Whether bundles or
         * instructions are used depends on where in scheduling we are. */

        unsigned instruction_count;
        unsigned bundle_count;

        union {
                bi_instruction *instructions[16];
                bi_bundle bundles[8];
        };

        /* For scoreboarding -- the clause ID (this is not globally unique!)
         * and its dependencies in terms of other clauses, computed during
         * scheduling and used when emitting code. Dependencies expressed as a
         * bitfield matching the hardware, except shifted by a clause (the
         * shift back to the ISA's off-by-one encoding is worked out when
         * emitting clauses) */
        unsigned scoreboard_id;
        uint8_t dependencies;

        /* Back-to-back corresponds directly to the back-to-back bit. Branch
         * conditional corresponds to the branch conditional bit except that in
         * the emitted code it's always set if back-to-bit is, whereas we use
         * the actual value (without back-to-back so to speak) internally */
        bool back_to_back;
        bool branch_conditional;

        /* Corresponds to the usual bit but shifted by a clause */
        bool data_register_write_barrier;

        /* Constants read by this clause. ISA limit. */
        uint64_t constants[8];
        unsigned constant_count;
} bi_clause;

typedef struct bi_block {
        struct list_head link; /* must be first */
        unsigned name; /* Just for pretty-printing */

        /* If true, uses clauses; if false, uses instructions */
        bool scheduled;

        union {
                struct list_head instructions; /* pre-schedule, list of bi_instructions */
                struct list_head clauses; /* list of bi_clause */
        };

        /* Control flow graph */
        struct set *predecessors;
        struct bi_block *successors[2];
} bi_block;

typedef struct {
       nir_shader *nir;
       struct list_head blocks; /* list of bi_block */
       uint32_t quirks;
} bi_context; 

/* So we can distinguish between SSA/reg/sentinel quickly */
#define BIR_NO_ARG (0)
#define BIR_IS_REG (1)

/* If high bits are set, instead of SSA/registers, we have specials indexed by
 * the low bits if necessary.
 *
 *  Fixed register: do not allocate register, do not collect $200.
 *  Uniform: access a uniform register given by low bits.
 *  Constant: access the specified constant 
 *  Zero: special cased to avoid wasting a constant
 */

#define BIR_INDEX_REGISTER (1 << 31)
#define BIR_INDEX_UNIFORM  (1 << 30)
#define BIR_INDEX_CONSTANT (1 << 29)
#define BIR_INDEX_ZERO     (1 << 28)

/* Keep me synced please so we can check src & BIR_SPECIAL */

#define BIR_SPECIAL        ((BIR_INDEX_REGISTER | BIR_INDEX_UNIFORM) | \
        (BIR_INDEX_CONSTANT | BIR_INDEX_ZERO)

static inline unsigned
bir_ssa_index(nir_ssa_def *ssa)
{
        /* Off-by-one ensures BIR_NO_ARG is skipped */
        return ((ssa->index + 1) << 1) | 0;
}

static inline unsigned
bir_src_index(nir_src *src)
{
        if (src->is_ssa)
                return bir_ssa_index(src->ssa);
        else {
                assert(!src->reg.indirect);
                return (src->reg.reg->index << 1) | BIR_IS_REG;
        }
}

static inline unsigned
bir_dest_index(nir_dest *dst)
{
        if (dst->is_ssa)
                return bir_ssa_index(&dst->ssa);
        else {
                assert(!dst->reg.indirect);
                return (dst->reg.reg->index << 1) | BIR_IS_REG;
        }
}

/* Iterators for Bifrost IR */

#define bi_foreach_block(ctx, v) \
        list_for_each_entry(bi_block, v, &ctx->blocks, link)

#define bi_foreach_block_from(ctx, from, v) \
        list_for_each_entry_from(bi_block, v, from, &ctx->blocks, link)

#define bi_foreach_instr_in_block(block, v) \
        list_for_each_entry(bi_instruction, v, &block->instructions, link)

#define bi_foreach_instr_in_block_rev(block, v) \
        list_for_each_entry_rev(bi_instruction, v, &block->instructions, link)

#define bi_foreach_instr_in_block_safe(block, v) \
        list_for_each_entry_safe(bi_instruction, v, &block->instructions, link)

#define bi_foreach_instr_in_block_safe_rev(block, v) \
        list_for_each_entry_safe_rev(bi_instruction, v, &block->instructions, link)

#define bi_foreach_instr_in_block_from(block, v, from) \
        list_for_each_entry_from(bi_instruction, v, from, &block->instructions, link)

#define bi_foreach_instr_in_block_from_rev(block, v, from) \
        list_for_each_entry_from_rev(bi_instruction, v, from, &block->instructions, link)

#define bi_foreach_clause_in_block(block, v) \
        list_for_each_entry(bi_clause, v, &block->clauses, link)

#define bi_foreach_instr_global(ctx, v) \
        bi_foreach_block(ctx, v_block) \
                bi_foreach_instr_in_block(v_block, v)

#define bi_foreach_instr_global_safe(ctx, v) \
        bi_foreach_block(ctx, v_block) \
                bi_foreach_instr_in_block_safe(v_block, v)

#define bi_foreach_successor(blk, v) \
        bi_block *v; \
        bi_block **_v; \
        for (_v = &blk->successors[0], \
                v = *_v; \
                v != NULL && _v < &blk->successors[2]; \
                _v++, v = *_v) \

/* Based on set_foreach, expanded with automatic type casts */

#define bi_foreach_predecessor(blk, v) \
        struct set_entry *_entry_##v; \
        bi_block *v; \
        for (_entry_##v = _mesa_set_next_entry(blk->predecessors, NULL), \
                v = (bi_block *) (_entry_##v ? _entry_##v->key : NULL);  \
                _entry_##v != NULL; \
                _entry_##v = _mesa_set_next_entry(blk->predecessors, _entry_##v), \
                v = (bi_block *) (_entry_##v ? _entry_##v->key : NULL))

#define bi_foreach_src(ins, v) \
        for (unsigned v = 0; v < ARRAY_SIZE(ins->src); ++v)

/* BIR manipulation */

bool bi_has_outmod(bi_instruction *ins);
bool bi_has_source_mods(bi_instruction *ins);
bool bi_is_src_swizzled(bi_instruction *ins, unsigned s);

#endif

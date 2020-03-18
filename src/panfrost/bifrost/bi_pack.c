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

#define RETURN_PACKED(str) { \
        uint64_t temp = 0; \
        memcpy(&temp, &str, sizeof(str)); \
        return temp; \
}

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

/* Assigns a port for reading, before anything is written */

static void
bi_assign_port_read(struct bi_registers *regs, unsigned src)
{
        /* We only assign for registers */
        if (!(src & BIR_INDEX_REGISTER))
                return;

        unsigned reg = src & ~BIR_INDEX_REGISTER;

        /* Check if we already assigned the port */
        for (unsigned i = 0; i <= 1; ++i) {
                if (regs->port[i] == reg && regs->enabled[i])
                        return;
        }

        if (regs->port[3] == reg && regs->read_port3)
                return;

        /* Assign it now */

        for (unsigned i = 0; i <= 1; ++i) {
                if (!regs->enabled[i]) {
                        regs->port[i] = reg;
                        regs->enabled[i] = true;
                        return;
                }
        }

        if (!regs->read_port3) {
                regs->port[3] = reg;
                regs->read_port3 = true;
        }
}

static struct bi_registers
bi_assign_ports(bi_bundle now, bi_bundle prev)
{
        struct bi_registers regs = { 0 };

        /* We assign ports for the main register mechanism. Special ops
         * use the data registers, which has its own mechanism entirely
         * and thus gets skipped over here. */

        unsigned read_dreg = now.add &&
                bi_class_props[now.add->type] & BI_DATA_REG_SRC;

        unsigned write_dreg = prev.add &&
                bi_class_props[prev.add->type] & BI_DATA_REG_DEST;

        /* First, assign reads */

        if (now.fma)
                bi_foreach_src(now.fma, src)
                        bi_assign_port_read(&regs, now.fma->src[src]);

        if (now.add) {
                bi_foreach_src(now.add, src) {
                        if (!(src == 0 && read_dreg))
                                bi_assign_port_read(&regs, now.add->src[src]);
                }
        }

        /* Next, assign writes */

        if (prev.fma && prev.fma->dest & BIR_INDEX_REGISTER) {
                regs.port[2] = prev.fma->dest & ~BIR_INDEX_REGISTER;
                regs.write_fma = true;
        }

        if (prev.add && prev.add->dest & BIR_INDEX_REGISTER && !write_dreg) {
                unsigned r = prev.add->dest & ~BIR_INDEX_REGISTER;

                if (regs.write_fma) {
                        /* Scheduler constraint: cannot read 3 and write 2 */
                        assert(!regs.read_port3);
                        regs.port[3] = r;
                } else {
                        regs.port[2] = r;
                }

                regs.write_add = true;
        }

        /* Finally, ensure port 1 > port 0 for the 63-x trick to function */

        if (regs.enabled[0] && regs.enabled[1] && regs.port[1] < regs.port[0]) {
                unsigned temp = regs.port[0];
                regs.port[0] = regs.port[1];
                regs.port[1] = temp;
        }

        return regs;
}

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

static uint64_t
bi_pack_registers(struct bi_registers regs)
{
        enum bifrost_reg_control ctrl = bi_pack_register_ctrl(regs);
        struct bifrost_regs s;
        uint64_t packed = 0;

        if (regs.enabled[1]) {
                /* Gotta save that bit!~ Required by the 63-x trick */
                assert(regs.port[1] > regs.port[0]);
                assert(regs.enabled[0]);

                /* Do the 63-x trick, see docs/disasm */
                if (regs.port[0] > 31) {
                        regs.port[0] = 63 - regs.port[0];
                        regs.port[1] = 63 - regs.port[1];
                }

                assert(regs.port[0] <= 31);
                assert(regs.port[1] <= 63);

                s.ctrl = ctrl;
                s.reg1 = regs.port[1];
                s.reg0 = regs.port[0];
        } else {
                /* Port 1 disabled, so set to zero and use port 1 for ctrl */
                s.reg1 = ctrl << 2;

                if (regs.enabled[0]) {
                        /* Bit 0 upper bit of port 0 */
                        s.reg1 |= (regs.port[0] >> 5);

                        /* Rest of port 0 in usual spot */
                        s.reg0 = (regs.port[0] & 0b11111);
                } else {
                        /* Bit 1 set if port 0 also disabled */
                        s.reg1 |= (1 << 1);
                }
        }

        s.reg3 = regs.port[3];
        s.reg2 = regs.port[2];
        s.uniform_const = regs.uniform_constant;

        memcpy(&packed, &s, sizeof(s));
        return packed;
}

static enum bifrost_packed_src
bi_get_src_reg_port(struct bi_registers *regs, unsigned src)
{
        unsigned reg = src & ~BIR_INDEX_REGISTER;

        if (regs->port[0] == reg && regs->enabled[0])
                return BIFROST_SRC_PORT0;
        else if (regs->port[1] == reg && regs->enabled[1])
                return BIFROST_SRC_PORT1;
        else if (regs->port[3] == reg && regs->read_port3)
                return BIFROST_SRC_PORT3;
        else
                unreachable("Tried to access register with no port");
}

static enum bifrost_packed_src
bi_get_src_const(struct bi_registers *regs, unsigned constant)
{
        if (regs->uniform_constant & (1 << 7))
                unreachable("Tried to get constant but loading uniforms");

        unsigned loc = (regs->uniform_constant >> 4) & 0x7;

        if (loc != 0)
                unreachable("TODO: constants in clauses");

        unsigned lo = regs->uniform_constant & 0xF;

        if (lo == 0) {
                if (constant != 0)
                        unreachable("Tried to load !0 in 0 slot");

                return BIFROST_SRC_CONST_LO;
        } else {
                unreachable("Special slot is not a fixed immediate");
        }
}

static enum bifrost_packed_src
bi_get_src(bi_instruction *ins, struct bi_registers *regs, unsigned s, bool is_fma)
{
        unsigned src = ins->src[s];

        if (src & BIR_INDEX_REGISTER)
                return bi_get_src_reg_port(regs, src);
        else if (src & BIR_INDEX_ZERO && is_fma)
                return BIFROST_SRC_STAGE;
        else if (src & BIR_INDEX_ZERO)
                return bi_get_src_const(regs, 0);
        else if (src & BIR_INDEX_PASS)
                return src & ~BIR_INDEX_PASS;
        else
                unreachable("Unknown src");
}

static unsigned
bi_pack_fma_fma(bi_instruction *ins, struct bi_registers *regs)
{
        /* (-a)(-b) = ab, so we only need one negate bit */
        bool negate_mul = ins->src_neg[0] ^ ins->src_neg[1];

        struct bifrost_fma_fma pack = {
                .src0 = bi_get_src(ins, regs, 0, true),
                .src1 = bi_get_src(ins, regs, 1, true),
                .src2 = bi_get_src(ins, regs, 2, true),
                .src0_abs = ins->src_abs[0],
                .src1_abs = ins->src_abs[1],
                .src2_abs = ins->src_abs[2],
                .src0_neg = negate_mul,
                .src2_neg = ins->src_neg[2],
                .op = BIFROST_FMA_OP_FMA
        };

        RETURN_PACKED(pack);
}

static unsigned
bi_pack_fma_add(bi_instruction *ins, struct bi_registers *regs)
{
        /* TODO: fadd16 packing is a bit different */
        assert(ins->dest_type == nir_type_float32);

        struct bifrost_fma_add pack = {
                .src0 = bi_get_src(ins, regs, 0, true),
                .src1 = bi_get_src(ins, regs, 1, true),
                .src0_abs = ins->src_abs[0],
                .src1_abs = ins->src_abs[1],
                .src0_neg = ins->src_neg[0],
                .src1_neg = ins->src_neg[1],
                .unk = 0x0,
                .outmod = ins->outmod,
                .roundmode = ins->roundmode,
                .op = BIFROST_FMA_OP_FADD32
        };

        RETURN_PACKED(pack);
}

static unsigned
bi_pack_fma(bi_clause *clause, bi_bundle bundle, struct bi_registers *regs)
{
        if (!bundle.fma)
                return BIFROST_FMA_NOP;

        switch (bundle.fma->type) {
        case BI_ADD:
                return bi_pack_fma_add(bundle.fma, regs);
        case BI_CMP:
        case BI_BITWISE:
        case BI_CONVERT:
        case BI_CSEL:
		return BIFROST_FMA_NOP;
        case BI_FMA:
                return bi_pack_fma_fma(bundle.fma, regs);
        case BI_FREXP:
        case BI_ISUB:
        case BI_MINMAX:
        case BI_MOV:
        case BI_SHIFT:
        case BI_SWIZZLE:
        case BI_ROUND:
		return BIFROST_FMA_NOP;
        default:
                unreachable("Cannot encode class as FMA");
        }
}

static unsigned
bi_pack_add_ld_vary(bi_instruction *ins, struct bi_registers *regs)
{
        unsigned size = nir_alu_type_get_type_size(ins->dest_type);
        assert(size == 32 || size == 16);

        unsigned op = (size == 32) ?
                BIFROST_ADD_OP_LD_VAR_32 :
                BIFROST_ADD_OP_LD_VAR_16;

        unsigned cmask = bi_from_bytemask(ins->writemask, size / 8);
        unsigned channels = util_bitcount(cmask);
        assert(cmask == ((1 << channels) - 1));

        unsigned packed_addr = 0;

        if (ins->src[0] & BIR_INDEX_CONSTANT) {
                /* Direct uses address field directly */
                packed_addr = ins->src[0] & ~BIR_INDEX_CONSTANT;
                assert(packed_addr < 0b1000);
        } else {
                /* Indirect gets an extra source */
                packed_addr = bi_get_src(ins, regs, 0, false) | 0b11000;
        }

        assert(channels >= 1 && channels <= 4);

        struct bifrost_ld_var pack = {
                .src0 = bi_get_src(ins, regs, 1, false),
                .addr = packed_addr,
                .channels = MALI_POSITIVE(channels),
                .interp_mode = ins->load_vary.interp_mode,
                .reuse = ins->load_vary.reuse,
                .flat = ins->load_vary.flat,
                .op = op
        };

        RETURN_PACKED(pack);
}

static unsigned
bi_pack_add(bi_clause *clause, bi_bundle bundle, struct bi_registers *regs)
{
        if (!bundle.add)
                return BIFROST_ADD_NOP;

        switch (bundle.add->type) {
        case BI_ADD:
        case BI_ATEST:
        case BI_BRANCH:
        case BI_CMP:
        case BI_BLEND:
        case BI_BITWISE:
        case BI_CONVERT:
        case BI_DISCARD:
        case BI_FREXP:
        case BI_ISUB:
        case BI_LOAD:
        case BI_LOAD_UNIFORM:
        case BI_LOAD_ATTR:
                return BIFROST_ADD_NOP;
        case BI_LOAD_VAR:
                return bi_pack_add_ld_vary(bundle.add, regs);
        case BI_LOAD_VAR_ADDRESS:
        case BI_MINMAX:
        case BI_MOV:
        case BI_SHIFT:
        case BI_STORE:
        case BI_STORE_VAR:
        case BI_SPECIAL:
        case BI_SWIZZLE:
        case BI_TEX:
        case BI_ROUND:
                return BIFROST_ADD_NOP;
        default:
                unreachable("Cannot encode class as ADD");
        }
}

struct bi_packed_bundle {
        uint64_t lo;
        uint64_t hi;
};

static struct bi_packed_bundle
bi_pack_bundle(bi_clause *clause, bi_bundle bundle, bi_bundle prev, bool first_bundle)
{
        struct bi_registers regs = bi_assign_ports(bundle, prev);
        regs.first_instruction = first_bundle;

        uint64_t reg = bi_pack_registers(regs);
        uint64_t fma = bi_pack_fma(clause, bundle, &regs);
        uint64_t add = bi_pack_add(clause, bundle, &regs);

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
        struct bi_packed_bundle ins_1 = bi_pack_bundle(clause, clause->bundles[0], clause->bundles[0], true);
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

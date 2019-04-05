/* Author(s):
 *  Alyssa Rosenzweig
 *
 * Copyright (c) 2018 Alyssa Rosenzweig (alyssa@rosenzweig.io)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Some constants and macros not found in the disassembler */

#define OP_IS_STORE_VARY(op) (\
		op == midgard_op_store_vary_16 || \
		op == midgard_op_store_vary_32 \
	)

#define OP_IS_STORE(op) (\
                OP_IS_STORE_VARY(op) || \
                op == midgard_op_store_cubemap_coords \
	)

/* ALU control words are single bit fields with a lot of space */

#define ALU_ENAB_VEC_MUL  (1 << 17)
#define ALU_ENAB_SCAL_ADD  (1 << 19)
#define ALU_ENAB_VEC_ADD  (1 << 21)
#define ALU_ENAB_SCAL_MUL  (1 << 23)
#define ALU_ENAB_VEC_LUT  (1 << 25)
#define ALU_ENAB_BR_COMPACT (1 << 26)
#define ALU_ENAB_BRANCH   (1 << 27)

/* Other opcode properties that don't conflict with the ALU_ENABs, non-ISA */

/* Denotes an opcode that takes a vector input with a fixed-number of
 * channels, but outputs to only a single output channel, like dot products.
 * For these, to determine the effective mask, this quirk can be set. We have
 * an intentional off-by-one (a la MALI_POSITIVE), since 0-channel makes no
 * sense but we need to fit 4 channels in 2-bits. Similarly, 1-channel doesn't
 * make sense (since then why are we quirked?), so that corresponds to "no
 * count set" */

#define OP_CHANNEL_COUNT(c) ((c - 1) << 0)
#define GET_CHANNEL_COUNT(c) ((c & (0x3 << 0)) ? ((c & (0x3 << 0)) + 1) : 0)

/* For instructions that take a single argument, normally the first argument
 * slot is used for the argument and the second slot is a dummy #0 constant.
 * However, there are exceptions: instructions like fmov store their argument
 * in the _second_ slot and store a dummy r24 in the first slot, designated by
 * QUIRK_FLIPPED_R24 */

#define QUIRK_FLIPPED_R24 (1 << 2)

/* Vector-independant shorthands for the above; these numbers are arbitrary and
 * not from the ISA. Convert to the above with unit_enum_to_midgard */

#define UNIT_MUL 0
#define UNIT_ADD 1
#define UNIT_LUT 2

/* 4-bit type tags */

#define TAG_TEXTURE_4 0x3
#define TAG_LOAD_STORE_4 0x5
#define TAG_ALU_4 0x8
#define TAG_ALU_8 0x9
#define TAG_ALU_12 0xA
#define TAG_ALU_16 0xB

/* Special register aliases */

#define MAX_WORK_REGISTERS 16

/* Uniforms are begin at (REGISTER_UNIFORMS - uniform_count) */
#define REGISTER_UNIFORMS 24

#define REGISTER_UNUSED 24
#define REGISTER_CONSTANT 26
#define REGISTER_VARYING_BASE 26
#define REGISTER_OFFSET 27
#define REGISTER_TEXTURE_BASE 28
#define REGISTER_SELECT 31

/* SSA helper aliases to mimic the registers. UNUSED_0 encoded as an inline
 * constant. UNUSED_1 encoded as REGISTER_UNUSED */

#define SSA_UNUSED_0 0
#define SSA_UNUSED_1 -2

#define SSA_FIXED_SHIFT 24
#define SSA_FIXED_REGISTER(reg) ((1 + reg) << SSA_FIXED_SHIFT)
#define SSA_REG_FROM_FIXED(reg) ((reg >> SSA_FIXED_SHIFT) - 1)
#define SSA_FIXED_MINIMUM SSA_FIXED_REGISTER(0)

/* Swizzle support */

#define SWIZZLE(A, B, C, D) ((D << 6) | (C << 4) | (B << 2) | (A << 0))
#define SWIZZLE_FROM_ARRAY(r) SWIZZLE(r[0], r[1], r[2], r[3])
#define COMPONENT_X 0x0
#define COMPONENT_Y 0x1
#define COMPONENT_Z 0x2
#define COMPONENT_W 0x3

/* See ISA notes */

#define LDST_NOP (3)

/* Is this opcode that of an integer? */
static bool
midgard_is_integer_op(int op)
{
        switch (op) {
        case midgard_alu_op_iadd:
        case midgard_alu_op_ishladd:
        case midgard_alu_op_isub:
        case midgard_alu_op_imul:
        case midgard_alu_op_imin:
        case midgard_alu_op_imax:
        case midgard_alu_op_iasr:
        case midgard_alu_op_ilsr:
        case midgard_alu_op_ishl:
        case midgard_alu_op_iand:
        case midgard_alu_op_ior:
        case midgard_alu_op_inot:
        case midgard_alu_op_iandnot:
        case midgard_alu_op_ixor:
        case midgard_alu_op_imov:

        //case midgard_alu_op_f2i:
        //case midgard_alu_op_f2u:
        case midgard_alu_op_ieq:
        case midgard_alu_op_iabs:
        case midgard_alu_op_ine:
        case midgard_alu_op_ilt:
        case midgard_alu_op_ile:
        case midgard_alu_op_iball_eq:
        case midgard_alu_op_ibany_neq:

        //case midgard_alu_op_i2f:
        //case midgard_alu_op_u2f:
        case midgard_alu_op_icsel:
                return true;

        default:
                return false;
        }
}

/* Is this unit a branch? */
static bool
midgard_is_branch_unit(unsigned unit)
{
        return (unit == ALU_ENAB_BRANCH) || (unit == ALU_ENAB_BR_COMPACT);
}

/* There are five ALU units: VMUL, VADD, SMUL, SADD, LUT. A given opcode is
 * implemented on some subset of these units (or occassionally all of them).
 * This table encodes a bit mask of valid units for each opcode, so the
 * scheduler can figure where to plonk the instruction. */

/* Shorthands for each unit */
#define UNIT_VMUL ALU_ENAB_VEC_MUL
#define UNIT_SADD ALU_ENAB_SCAL_ADD
#define UNIT_VADD ALU_ENAB_VEC_ADD
#define UNIT_SMUL ALU_ENAB_SCAL_MUL
#define UNIT_VLUT ALU_ENAB_VEC_LUT

/* Shorthands for usual combinations of units */

#define UNITS_MUL (UNIT_VMUL | UNIT_SMUL)
#define UNITS_ADD (UNIT_VADD | UNIT_SADD)
#define UNITS_MOST (UNITS_MUL | UNITS_ADD)
#define UNITS_ALL (UNITS_MOST | UNIT_VLUT)
#define UNITS_SCALAR (UNIT_SADD | UNIT_SMUL)
#define UNITS_VECTOR (UNIT_VMUL | UNIT_VADD)
#define UNITS_ANY_VECTOR (UNITS_VECTOR | UNIT_VLUT)

static unsigned alu_opcode_props[256] = {
        [midgard_alu_op_fadd]		 = UNITS_ADD,
        [midgard_alu_op_fmul]		 = UNITS_MUL | UNIT_VLUT,
        [midgard_alu_op_fmin]		 = UNITS_MUL | UNITS_ADD,
        [midgard_alu_op_fmax]		 = UNITS_MUL | UNITS_ADD,
        [midgard_alu_op_imin]		 = UNITS_MOST,
        [midgard_alu_op_imax]		 = UNITS_MOST,
        [midgard_alu_op_umin]		 = UNITS_MOST,
        [midgard_alu_op_umax]		 = UNITS_MOST,
        [midgard_alu_op_fmov]		 = UNITS_ALL | QUIRK_FLIPPED_R24,
        [midgard_alu_op_fround]          = UNITS_ADD,
        [midgard_alu_op_froundeven]      = UNITS_ADD,
        [midgard_alu_op_ftrunc]          = UNITS_ADD,
        [midgard_alu_op_ffloor]		 = UNITS_ADD,
        [midgard_alu_op_fceil]		 = UNITS_ADD,
        [midgard_alu_op_ffma]		 = UNIT_VLUT,

        /* Though they output a scalar, they need to run on a vector unit
         * since they process vectors */
        [midgard_alu_op_fdot3]		 = UNIT_VMUL | OP_CHANNEL_COUNT(3),
        [midgard_alu_op_fdot4]		 = UNIT_VMUL | OP_CHANNEL_COUNT(4),

        /* Incredibly, iadd can run on vmul, etc */
        [midgard_alu_op_iadd]		 = UNITS_MOST,
        [midgard_alu_op_iabs]		 = UNITS_MOST,
        [midgard_alu_op_isub]		 = UNITS_MOST,
        [midgard_alu_op_imul]		 = UNITS_MOST,
        [midgard_alu_op_imov]		 = UNITS_MOST | QUIRK_FLIPPED_R24,

        /* For vector comparisons, use ball etc */
        [midgard_alu_op_feq]		 = UNITS_MOST,
        [midgard_alu_op_fne]		 = UNITS_MOST,
        [midgard_alu_op_fle]		 = UNITS_MOST,
        [midgard_alu_op_flt]		 = UNITS_MOST,
        [midgard_alu_op_ieq]		 = UNITS_MOST,
        [midgard_alu_op_ine]		 = UNITS_MOST,
        [midgard_alu_op_ilt]		 = UNITS_MOST,
        [midgard_alu_op_ile]		 = UNITS_MOST,
        [midgard_alu_op_ule]		 = UNITS_MOST,
        [midgard_alu_op_ult]		 = UNITS_MOST,

        [midgard_alu_op_icsel]		 = UNITS_ADD,
        [midgard_alu_op_fcsel_i]	 = UNITS_ADD,
        [midgard_alu_op_fcsel]		 = UNITS_ADD | UNIT_SMUL,

        [midgard_alu_op_frcp]		 = UNIT_VLUT,
        [midgard_alu_op_frsqrt]		 = UNIT_VLUT,
        [midgard_alu_op_fsqrt]		 = UNIT_VLUT,
        [midgard_alu_op_fpow_pt1]	 = UNIT_VLUT,
        [midgard_alu_op_fexp2]		 = UNIT_VLUT,
        [midgard_alu_op_flog2]		 = UNIT_VLUT,

        [midgard_alu_op_f2i]		 = UNITS_ADD,
        [midgard_alu_op_f2u]		 = UNITS_ADD,
        [midgard_alu_op_f2u8]		 = UNITS_ADD,
        [midgard_alu_op_i2f]		 = UNITS_ADD,
        [midgard_alu_op_u2f]		 = UNITS_ADD,

        [midgard_alu_op_fsin]		 = UNIT_VLUT,
        [midgard_alu_op_fcos]		 = UNIT_VLUT,

        [midgard_alu_op_iand]		 = UNITS_ADD, /* XXX: Test case where it's right on smul but not sadd */
        [midgard_alu_op_ior]		 = UNITS_ADD,
        [midgard_alu_op_ixor]		 = UNITS_ADD,
        [midgard_alu_op_ilzcnt]		 = UNITS_ADD,
        [midgard_alu_op_ibitcount8]	 = UNITS_ADD,
        [midgard_alu_op_inot]		 = UNITS_MOST,
        [midgard_alu_op_ishl]		 = UNITS_ADD,
        [midgard_alu_op_iasr]		 = UNITS_ADD,
        [midgard_alu_op_ilsr]		 = UNITS_ADD,
        [midgard_alu_op_ilsr]		 = UNITS_ADD,

        [midgard_alu_op_fball_eq]	 = UNITS_VECTOR,
        [midgard_alu_op_fbany_neq]	 = UNITS_VECTOR,
        [midgard_alu_op_iball_eq]	 = UNITS_VECTOR,
        [midgard_alu_op_ibany_neq]	 = UNITS_VECTOR
};

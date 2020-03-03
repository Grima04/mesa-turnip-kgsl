/*
 * Copyright (C) 2019 Connor Abbott <cwabbott0@gmail.com>
 * Copyright (C) 2019 Lyude Paul <thatslyude@gmail.com>
 * Copyright (C) 2019 Ryan Houdek <Sonicadvance1@gmail.com>
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

#ifndef __bifrost_h__
#define __bifrost_h__

#include <stdint.h>
#include <stdbool.h>

struct bifrost_header {
        unsigned unk0 : 7;
        // If true, convert any infinite result of any floating-point operation to
        // the biggest representable number.
        unsigned suppress_inf: 1;
        // Convert any NaN results to 0.
        unsigned suppress_nan : 1;
        unsigned unk1 : 2;
        // true if the execution mask of the next clause is the same as the mask of
        // the current clause.
        unsigned back_to_back : 1;
        unsigned no_end_of_shader: 1;
        unsigned unk2 : 2;
        // Set to true for fragment shaders, to implement this bit of spec text
        // from section 7.1.5 of the GLSL ES spec:
        //
        // "Stores to image and buffer variables performed by helper invocations
        // have no effect on the underlying image or buffer memory."
        //
        // Helper invocations are threads (invocations) corresponding to pixels in
        // a quad that aren't actually part of the triangle, but are included to
        // make derivatives work correctly. They're usually turned on, but they
        // need to be masked off for GLSL-level stores. This bit seems to be the
        // only bit that's actually different between fragment shaders and other
        // shaders, so this is probably what it's doing.
        unsigned elide_writes : 1;
        // If backToBack is off:
        // - true for conditional branches and fallthrough
        // - false for unconditional branches
        // The blob seems to always set it to true if back-to-back is on.
        unsigned branch_cond : 1;
        // This bit is set when the next clause writes to the data register of some
        // previous clause.
        unsigned datareg_writebarrier: 1;
        unsigned datareg : 6;
        unsigned scoreboard_deps: 8;
        unsigned scoreboard_index: 3;
        unsigned clause_type: 4;
        unsigned unk3 : 1; // part of clauseType?
        unsigned next_clause_type: 4;
        unsigned unk4 : 1; // part of nextClauseType?
};

struct bifrost_fma_inst {
        unsigned src0 : 3;
        unsigned op   : 20;
};

struct bifrost_add_inst {
        unsigned src0 : 3;
        unsigned op   : 17;
};

enum bifrost_outmod {
        BIFROST_NONE = 0x0,
        BIFROST_POS = 0x1,
        BIFROST_SAT_SIGNED = 0x2,
        BIFROST_SAT = 0x3,
};

enum bifrost_roundmode {
        BIFROST_RTE = 0x0,
        BIFROST_RTP = 0x1,
        BIFROST_RTN = 0x2,
        BIFROST_RTZ = 0x3
};

enum bifrost_minmax_mode {
        BIFROST_MINMAX_NONE = 0x0,
        BIFROST_NAN_WINS    = 0x1,
        BIFROST_SRC1_WINS   = 0x2,
        BIFROST_SRC0_WINS   = 0x3,
};

struct bifrost_fma_add {
        unsigned src0 : 3;
        unsigned src1 : 3;
        unsigned src1_abs : 1;
        unsigned src0_neg : 1;
        unsigned src1_neg : 1;
        unsigned unk : 3;
        unsigned src0_abs : 1;
        enum bifrost_outmod outmod : 2;
        enum bifrost_roundmode roundmode : 2;
        unsigned op : 6;
};

enum bifrost_csel_cond {
        BIFROST_FEQ_F = 0x0,
        BIFROST_FGT_F = 0x1,
        BIFROST_FGE_F = 0x2,
        BIFROST_IEQ_F = 0x3,
        BIFROST_IGT_I = 0x4,
        BIFROST_IGE_I = 0x5,
        BIFROST_UGT_I = 0x6,
        BIFROST_UGE_I = 0x7
};

struct bifrost_csel4 {
        unsigned src0 : 3;
        unsigned src1 : 3;
        unsigned src2 : 3;
        unsigned src3 : 3;
        enum bifrost_csel_cond cond : 3;
        unsigned op   : 8;
};

struct bifrost_shift_fma {
        unsigned src0 : 3;
        unsigned src1 : 3;
        unsigned src2 : 3;
        unsigned half : 3; /* 000 for i32, 100 for i8, 111 for v2i16 */
        unsigned unk  : 1; /* always set? */
        unsigned invert_1 : 1; /* Inverts sources to combining op */
        /* For XOR, switches RSHIFT to LSHIFT since only one invert needed */
        unsigned invert_2 : 1;
        unsigned op : 8;
};

struct bifrost_shift_add {
        unsigned src0 : 3;
        unsigned src1 : 3;
        unsigned src2 : 3;
        unsigned zero : 2;

        unsigned invert_1 : 1;
        unsigned invert_2 : 1;

        unsigned op : 7;
};

enum bifrost_ldst_type {
        BIFROST_LDST_F16 = 0,
        BIFROST_LDST_F32 = 1,
        BIFROST_LDST_I32 = 2,
        BIFROST_LDST_U32 = 3
};

struct bifrost_ld_var_addr {
        unsigned src0 : 3;
        unsigned src1 : 3;
        unsigned location : 5;
        enum bifrost_ldst_type type : 2;
        unsigned op : 7;
};

struct bifrost_ld_attr {
        unsigned src0 : 3;
        unsigned src1 : 3;
        unsigned location : 5;
        unsigned channels : 2; /* MALI_POSITIVE */
        enum bifrost_ldst_type type : 2;
        unsigned op : 5;
};

enum bifrost_interp_mode {
        BIFROST_INTERP_PER_FRAG = 0x0,
        BIFROST_INTERP_CENTROID = 0x1,
        BIFROST_INTERP_DEFAULT  = 0x2,
        BIFROST_INTERP_EXPLICIT = 0x3
};

struct bifrost_ld_var {
        unsigned src0 : 3;

        /* If top two bits set, indirect with src in bottom three */
        unsigned addr : 5;

        unsigned channels : 2; /* MALI_POSITIVE */
        enum bifrost_interp_mode interp_mode : 2;
        unsigned reuse : 1;
        unsigned flat : 1;
        unsigned op : 6;
};

struct bifrost_tex_ctrl {
        unsigned sampler_index : 4; // also used to signal indirects
        unsigned tex_index : 7;
        bool no_merge_index : 1; // whether to merge (direct) sampler & texture indices
        bool filter : 1; // use the usual filtering pipeline (0 for texelFetch & textureGather)
        unsigned unk0 : 2;
        bool texel_offset : 1; // *Offset()
        bool is_shadow : 1;
        bool is_array : 1;
        unsigned tex_type : 2; // 2D, 3D, Cube, Buffer
        bool compute_lod : 1; // 0 for *Lod()
        bool not_supply_lod : 1; // 0 for *Lod() or when a bias is applied
        bool calc_gradients : 1; // 0 for *Grad()
        unsigned unk1 : 1;
        unsigned result_type : 4; // integer, unsigned, float TODO: why is this 4 bits?
        unsigned unk2 : 4;
};

struct bifrost_dual_tex_ctrl {
        unsigned sampler_index0 : 2;
        unsigned unk0 : 2;
        unsigned tex_index0 : 2;
        unsigned sampler_index1 : 2;
        unsigned tex_index1 : 2;
        unsigned unk1 : 22;
};

enum branch_bit_size {
        BR_SIZE_32 = 0,
        BR_SIZE_16XX = 1,
        BR_SIZE_16YY = 2,
        // For the above combinations of bitsize and location, an extra bit is
        // encoded via comparing the sources. The only possible source of ambiguity
        // would be if the sources were the same, but then the branch condition
        // would be always true or always false anyways, so we can ignore it. But
        // this no longer works when comparing the y component to the x component,
        // since it's valid to compare the y component of a source against its own
        // x component. Instead, the extra bit is encoded via an extra bitsize.
        BR_SIZE_16YX0 = 3,
        BR_SIZE_16YX1 = 4,
        BR_SIZE_32_AND_16X = 5,
        BR_SIZE_32_AND_16Y = 6,
        // Used for comparisons with zero and always-true, see below. I think this
        // only works for integer comparisons.
        BR_SIZE_ZERO = 7,
};

enum bifrost_reg_write_unit {
        REG_WRITE_NONE = 0, // don't write
        REG_WRITE_TWO, // write using reg2
        REG_WRITE_THREE, // write using reg3
};

struct bifrost_regs {
        unsigned uniform_const : 8;
        unsigned reg2 : 6;
        unsigned reg3 : 6;
        unsigned reg0 : 5;
        unsigned reg1 : 6;
        unsigned ctrl : 4;
};

enum bifrost_branch_cond {
        BR_COND_LT = 0,
        BR_COND_LE = 1,
        BR_COND_GE = 2,
        BR_COND_GT = 3,
        // Equal vs. not-equal determined by src0/src1 comparison
        BR_COND_EQ = 4,
        // floating-point comparisons
        // Becomes UNE when you flip the arguments
        BR_COND_OEQ = 5,
        // TODO what happens when you flip the arguments?
        BR_COND_OGT = 6,
        BR_COND_OLT = 7,
};

enum bifrost_branch_code {
        BR_ALWAYS = 63,
};

struct bifrost_branch {
        unsigned src0 : 3;

        /* For BR_SIZE_ZERO, upper two bits become ctrl */
        unsigned src1 : 3;

        /* Offset source -- always uniform/const but
         * theoretically could support indirect jumps? */
        unsigned src2 : 3;

        enum bifrost_branch_cond cond : 3;
        enum branch_bit_size size : 3;

        unsigned op : 5;
};

#endif

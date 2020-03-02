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

#endif

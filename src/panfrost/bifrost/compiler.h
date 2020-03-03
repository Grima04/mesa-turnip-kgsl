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
        BI_FMA,
        BI_FREXP,
        BI_LOAD,
        BI_LOAD_ATTR,
        BI_LOAD_VAR,
        BI_LOAD_VAR_ADDRESS,
        BI_MINMAX,
        BI_MOV,
        BI_SHIFT,
        BI_STORE,
        BI_STORE_VAR,
        BI_SPECIAL, /* _FAST, _TABLE on supported GPUs */
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

/* It can't get any worse than csel4... can it? */
#define BIR_SRC_COUNT 4

typedef struct {
        struct list_head link; /* Must be first */
        enum bi_class type;

        /* Indices, see bir_ssa_index etc. Note zero is special cased
         * to "no argument" */
        unsigned dest;
        unsigned src[BIR_SRC_COUNT];

        /* Floating-point modifiers, type/class permitting. If not
         * allowed for the type/class, these are ignored. */
        enum bifrost_outmod outmod;
        bool src_abs[BIR_SRC_COUNT];
        bool src_neg[BIR_SRC_COUNT];
} bi_instruction;

typedef struct {
        struct list_head link; /* must be first */
        struct list_head instructions; /* list of bi_instructions */
} bi_block;

typedef struct {
       nir_shader *nir;
       struct list_head blocks; /* list of bi_block */
} bi_context; 

/* So we can distinguish between SSA/reg/sentinel quickly */
#define BIR_NO_ARG (0)
#define BIR_IS_REG (1)

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

#endif

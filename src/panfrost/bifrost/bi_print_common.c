/*
 * Copyright (C) 2019 Connor Abbott <cwabbott0@gmail.com>
 * Copyright (C) 2019 Lyude Paul <thatslyude@gmail.com>
 * Copyright (C) 2019 Ryan Houdek <Sonicadvance1@gmail.com>
 * Copyright (C) 2019-2020 Collabora, Ltd.
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

/* Prints shared with the disassembler */

#include "bi_print_common.h"

const char *
bi_clause_type_name(enum bifrost_clause_type T)
{
        switch (T) {
        case BIFROST_CLAUSE_NONE: return "";
        case BIFROST_CLAUSE_LOAD_VARY: return "load_vary";
        case BIFROST_CLAUSE_UBO: return "ubo";
        case BIFROST_CLAUSE_TEX: return "tex";
        case BIFROST_CLAUSE_SSBO_LOAD: return "load";
        case BIFROST_CLAUSE_SSBO_STORE: return "store";
        case BIFROST_CLAUSE_BLEND: return "blend";
        case BIFROST_CLAUSE_FRAGZ: return "fragz";
        case BIFROST_CLAUSE_ATEST: return "atest";
        case BIFROST_CLAUSE_64BIT: return "64";
        default: return "??";
        }
}

const char *
bi_output_mod_name(enum bifrost_outmod mod)
{
        switch (mod) {
        case BIFROST_NONE: return "";
        case BIFROST_POS: return ".pos";
        case BIFROST_SAT_SIGNED: return ".sat_signed";
        case BIFROST_SAT: return ".sat";
        default: return "invalid";
        }
}

const char *
bi_minmax_mode_name(enum bifrost_minmax_mode mod)
{
        switch (mod) {
        case BIFROST_MINMAX_NONE: return "";
        case BIFROST_NAN_WINS: return ".nan_wins";
        case BIFROST_SRC1_WINS: return ".src1_wins";
        case BIFROST_SRC0_WINS: return ".src0_wins";
        default: return "invalid";
        }
}

const char *
bi_round_mode_name(enum bifrost_roundmode mod)
{
        switch (mod) {
        case BIFROST_RTE: return "";
        case BIFROST_RTP: return ".rtp";
        case BIFROST_RTN: return ".rtn";
        case BIFROST_RTZ: return ".rtz";
        default: return "invalid";
        }
}

const char *
bi_csel_cond_name(enum bifrost_csel_cond cond)
{
        switch (cond) {
        case BIFROST_FEQ_F: return "feq.f";
        case BIFROST_FGT_F: return "fgt.f";
        case BIFROST_FGE_F: return "fge.f";
        case BIFROST_IEQ_F: return "ieq.f";
        case BIFROST_IGT_I: return "igt.i";
        case BIFROST_IGE_I: return "uge.i";
        case BIFROST_UGT_I: return "ugt.i";
        case BIFROST_UGE_I: return "uge.i";
        default: return "invalid";
        }
}

const char *
bi_interp_mode_name(enum bifrost_interp_mode mode)
{
        switch (mode) {
        case BIFROST_INTERP_PER_FRAG: return ".per_frag";
        case BIFROST_INTERP_CENTROID: return ".centroid";
        case BIFROST_INTERP_DEFAULT: return "";
        case BIFROST_INTERP_EXPLICIT: return ".explicit";
        default: return ".unknown";
        }
}

const char *
bi_ldst_type_name(enum bifrost_ldst_type type)
{
        switch (type) {
        case BIFROST_LDST_F16: return "f16";
        case BIFROST_LDST_F32: return "f32";
        case BIFROST_LDST_I32: return "i32";
        case BIFROST_LDST_U32: return "u32";
        default: return "invalid";
        }
}

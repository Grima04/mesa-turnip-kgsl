/*
 * Copyright © 2014 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * @file brw_inst.h
 *
 * A representation of i965 EU assembly instructions, with helper methods to
 * get and set various fields.  This is the actual hardware format.
 */

#ifndef BRW_INST_H
#define BRW_INST_H

#include <assert.h>
#include <stdint.h>

#include "brw_eu_defines.h"
#include "brw_reg_type.h"
#include "dev/gen_device_info.h"

#ifdef __cplusplus
extern "C" {
#endif

/* brw_context.h has a forward declaration of brw_inst, so name the struct. */
typedef struct brw_inst {
   uint64_t data[2];
} brw_inst;

static inline uint64_t brw_inst_bits(const brw_inst *inst,
                                     unsigned high, unsigned low);
static inline void brw_inst_set_bits(brw_inst *inst,
                                     unsigned high, unsigned low,
                                     uint64_t value);

#define FC(name, hi4, lo4, hi12, lo12, assertions)            \
static inline void                                            \
brw_inst_set_##name(const struct gen_device_info *devinfo,    \
                    brw_inst *inst, uint64_t v)               \
{                                                             \
   assert(assertions);                                        \
   if (devinfo->gen >= 12)                                    \
      brw_inst_set_bits(inst, hi12, lo12, v);                 \
   else                                                       \
      brw_inst_set_bits(inst, hi4, lo4, v);                   \
}                                                             \
static inline uint64_t                                        \
brw_inst_##name(const struct gen_device_info *devinfo,        \
                const brw_inst *inst)                         \
{                                                             \
   assert(assertions);                                        \
   if (devinfo->gen >= 12)                                    \
      return brw_inst_bits(inst, hi12, lo12);                 \
   else                                                       \
      return brw_inst_bits(inst, hi4, lo4);                   \
}

/* A simple macro for fields which stay in the same place on all generations,
 * except for Gen12!
 */
#define F(name, hi4, lo4, hi12, lo12) FC(name, hi4, lo4, hi12, lo12, true)

#define BOUNDS(hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,                     \
               hi7, lo7, hi8, lo8, hi12, lo12)                               \
   unsigned high, low;                                                       \
   if (devinfo->gen >= 12) {                                                 \
      high = hi12; low = lo12;                                               \
   } else if (devinfo->gen >= 8) {                                           \
      high = hi8;  low = lo8;                                                \
   } else if (devinfo->gen >= 7) {                                           \
      high = hi7;  low = lo7;                                                \
   } else if (devinfo->gen >= 6) {                                           \
      high = hi6;  low = lo6;                                                \
   } else if (devinfo->gen >= 5) {                                           \
      high = hi5;  low = lo5;                                                \
   } else if (devinfo->is_g4x) {                                             \
      high = hi45; low = lo45;                                               \
   } else {                                                                  \
      high = hi4;  low = lo4;                                                \
   }                                                                         \
   assert(((int) high) != -1 && ((int) low) != -1);

/* A general macro for cases where the field has moved to several different
 * bit locations across generations.  GCC appears to combine cases where the
 * bits are identical, removing some of the inefficiency.
 */
#define FF(name, hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,                    \
           hi7, lo7, hi8, lo8, hi12, lo12)                                    \
static inline void                                                            \
brw_inst_set_##name(const struct gen_device_info *devinfo,                    \
                    brw_inst *inst, uint64_t value)                           \
{                                                                             \
   BOUNDS(hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,                           \
          hi7, lo7, hi8, lo8, hi12, lo12)                                     \
   brw_inst_set_bits(inst, high, low, value);                                 \
}                                                                             \
static inline uint64_t                                                        \
brw_inst_##name(const struct gen_device_info *devinfo, const brw_inst *inst)  \
{                                                                             \
   BOUNDS(hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,                           \
          hi7, lo7, hi8, lo8, hi12, lo12)                                     \
   return brw_inst_bits(inst, high, low);                                     \
}

/* A macro for fields which moved as of Gen8+. */
#define F8(name, gen4_high, gen4_low, gen8_high, gen8_low, \
           gen12_high, gen12_low)                          \
FF(name,                                                   \
   /* 4:   */ gen4_high, gen4_low,                         \
   /* 4.5: */ gen4_high, gen4_low,                         \
   /* 5:   */ gen4_high, gen4_low,                         \
   /* 6:   */ gen4_high, gen4_low,                         \
   /* 7:   */ gen4_high, gen4_low,                         \
   /* 8:   */ gen8_high, gen8_low,                         \
   /* 12:  */ gen12_high, gen12_low);

/* Macro for fields that gained extra discontiguous MSBs in Gen12 (specified
 * by hi12ex-lo12ex).
 */
#define FFDC(name, hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,                  \
             hi7, lo7, hi8, lo8, hi12ex, lo12ex, hi12, lo12, assertions)      \
static inline void                                                            \
brw_inst_set_##name(const struct gen_device_info *devinfo,                    \
                    brw_inst *inst, uint64_t value)                           \
{                                                                             \
   assert(assertions);                                                        \
   if (devinfo->gen >= 12) {                                                  \
      const unsigned k = hi12 - lo12 + 1;                                     \
      if (hi12ex != -1 && lo12ex != -1)                                       \
         brw_inst_set_bits(inst, hi12ex, lo12ex, value >> k);                 \
      brw_inst_set_bits(inst, hi12, lo12, value & ((1ull << k) - 1));         \
   } else {                                                                   \
      BOUNDS(hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,                        \
             hi7, lo7, hi8, lo8, -1, -1);                                     \
      brw_inst_set_bits(inst, high, low, value);                              \
   }                                                                          \
}                                                                             \
static inline uint64_t                                                        \
brw_inst_##name(const struct gen_device_info *devinfo, const brw_inst *inst)  \
{                                                                             \
   assert(assertions);                                                        \
   if (devinfo->gen >= 12) {                                                  \
      const unsigned k = hi12 - lo12 + 1;                                     \
      return (hi12ex == -1 || lo12ex == -1 ? 0 :                              \
              brw_inst_bits(inst, hi12ex, lo12ex) << k) |                     \
             brw_inst_bits(inst, hi12, lo12);                                 \
   } else {                                                                   \
      BOUNDS(hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,                        \
             hi7, lo7, hi8, lo8, -1, -1);                                     \
      return brw_inst_bits(inst, high, low);                                  \
   }                                                                          \
}

#define FD(name, hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,        \
           hi7, lo7, hi8, lo8, hi12ex, lo12ex, hi12, lo12)        \
   FFDC(name, hi4, lo4, hi45, lo45, hi5, lo5, hi6, lo6,           \
        hi7, lo7, hi8, lo8, hi12ex, lo12ex, hi12, lo12, true)

/* Macro for fields that didn't move across generations until Gen12, and then
 * gained extra discontiguous bits.
 */
#define FDC(name, hi4, lo4, hi12ex, lo12ex, hi12, lo12, assertions)     \
   FFDC(name, hi4, lo4, hi4, lo4, hi4, lo4, hi4, lo4,                   \
        hi4, lo4, hi4, lo4, hi12ex, lo12ex, hi12, lo12, assertions)


/* Macro for the 2-bit register file field, which on Gen12+ is stored as the
 * variable length combination of an IsImm (hi12) bit and an additional file
 * (lo12) bit.
 */
#define FI(name, hi4, lo4, hi8, lo8, hi12, lo12)                              \
static inline void                                                            \
brw_inst_set_##name(const struct gen_device_info *devinfo,                    \
                    brw_inst *inst, uint64_t value)                           \
{                                                                             \
   if (devinfo->gen >= 12) {                                                  \
      brw_inst_set_bits(inst, hi12, hi12, value >> 1);                        \
      if ((value >> 1) == 0)                                                  \
         brw_inst_set_bits(inst, lo12, lo12, value & 1);                      \
   } else {                                                                   \
      BOUNDS(hi4, lo4, hi4, lo4, hi4, lo4, hi4, lo4,                          \
             hi4, lo4, hi8, lo8, -1, -1);                                     \
      brw_inst_set_bits(inst, high, low, value);                              \
   }                                                                          \
}                                                                             \
static inline uint64_t                                                        \
brw_inst_##name(const struct gen_device_info *devinfo, const brw_inst *inst)  \
{                                                                             \
   if (devinfo->gen >= 12) {                                                  \
      return (brw_inst_bits(inst, hi12, hi12) << 1) |                         \
             (brw_inst_bits(inst, hi12, hi12) == 0 ?                          \
              brw_inst_bits(inst, lo12, lo12) : 1);                           \
   } else {                                                                   \
      BOUNDS(hi4, lo4, hi4, lo4, hi4, lo4, hi4, lo4,                          \
             hi4, lo4, hi8, lo8, -1, -1);                                     \
      return brw_inst_bits(inst, high, low);                                  \
   }                                                                          \
}

/* Macro for fields that become a constant in Gen12+ not actually represented
 * in the instruction.
 */
#define FK(name, hi4, lo4, const12)                           \
static inline void                                            \
brw_inst_set_##name(const struct gen_device_info *devinfo,    \
                    brw_inst *inst, uint64_t v)               \
{                                                             \
   if (devinfo->gen >= 12)                                    \
      assert(v == (const12));                                 \
   else                                                       \
      brw_inst_set_bits(inst, hi4, lo4, v);                   \
}                                                             \
static inline uint64_t                                        \
brw_inst_##name(const struct gen_device_info *devinfo,        \
                const brw_inst *inst)                         \
{                                                             \
   if (devinfo->gen >= 12)                                    \
      return (const12);                                       \
   else                                                       \
      return brw_inst_bits(inst, hi4, lo4);                   \
}

F(src1_vstride,        /* 4+ */ 120, 117, /* 12+ */ 119, 116)
F(src1_width,          /* 4+ */ 116, 114, /* 12+ */ 115, 113)
F(src1_da16_swiz_w,    /* 4+ */ 115, 114, /* 12+ */ -1, -1)
F(src1_da16_swiz_z,    /* 4+ */ 113, 112, /* 12+ */ -1, -1)
F(src1_hstride,        /* 4+ */ 113, 112, /* 12+ */ 97, 96)
F(src1_address_mode,   /* 4+ */ 111, 111, /* 12+ */ 112, 112)
/** Src1.SrcMod @{ */
F(src1_negate,         /* 4+ */ 110, 110, /* 12+ */ 121, 121)
F(src1_abs,            /* 4+ */ 109, 109, /* 12+ */ 120, 120)
/** @} */
F8(src1_ia_subreg_nr,  /* 4+ */ 108, 106, /* 8+ */  108, 105, /* 12+ */ 111, 108)
F(src1_da_reg_nr,      /* 4+ */ 108, 101, /* 12+ */ 111, 104)
F(src1_da16_subreg_nr, /* 4+ */ 100, 100, /* 12+ */ -1, -1)
F(src1_da1_subreg_nr,  /* 4+ */ 100,  96, /* 12+ */ 103, 99)
F(src1_da16_swiz_y,    /* 4+ */ 99,  98,  /* 12+ */ -1, -1)
F(src1_da16_swiz_x,    /* 4+ */ 97,  96,  /* 12+ */ -1, -1)
F8(src1_reg_hw_type,   /* 4+ */ 46,  44,  /* 8+ */  94,  91, /* 12+ */ 91, 88)
FI(src1_reg_file,      /* 4+ */ 43,  42,  /* 8+ */  90,  89, /* 12+ */ 47, 98)
F(src1_is_imm,         /* 4+ */ -1,  -1,  /* 12+ */ 47, 47)
F(src0_vstride,        /* 4+ */ 88,  85,  /* 12+ */ 87, 84)
F(src0_width,          /* 4+ */ 84,  82,  /* 12+ */ 83, 81)
F(src0_da16_swiz_w,    /* 4+ */ 83,  82,  /* 12+ */ -1, -1)
F(src0_da16_swiz_z,    /* 4+ */ 81,  80,  /* 12+ */ -1, -1)
F(src0_hstride,        /* 4+ */ 81,  80,  /* 12+ */ 65, 64)
F(src0_address_mode,   /* 4+ */ 79,  79,  /* 12+ */ 80, 80)
/** Src0.SrcMod @{ */
F(src0_negate,         /* 4+ */ 78,  78,  /* 12+ */ 45, 45)
F(src0_abs,            /* 4+ */ 77,  77,  /* 12+ */ 44, 44)
/** @} */
F8(src0_ia_subreg_nr,  /* 4+ */ 76,  74,  /* 8+ */  76,  73, /* 12+ */ 79, 76)
F(src0_da_reg_nr,      /* 4+ */ 76,  69,  /* 12+ */ 79, 72)
F(src0_da16_subreg_nr, /* 4+ */ 68,  68,  /* 12+ */ -1, -1)
F(src0_da1_subreg_nr,  /* 4+ */ 68,  64,  /* 12+ */ 71, 67)
F(src0_da16_swiz_y,    /* 4+ */ 67,  66,  /* 12+ */ -1, -1)
F(src0_da16_swiz_x,    /* 4+ */ 65,  64,  /* 12+ */ -1, -1)
F(dst_address_mode,    /* 4+ */ 63,  63,  /* 12+ */ 35, 35)
F(dst_hstride,         /* 4+ */ 62,  61,  /* 12+ */ 49, 48)
F8(dst_ia_subreg_nr,   /* 4+ */ 60,  58,  /* 8+ */  60,  57, /* 12+ */ 63, 60)
F(dst_da_reg_nr,       /* 4+ */ 60,  53,  /* 12+ */ 63, 56)
F(dst_da16_subreg_nr,  /* 4+ */ 52,  52,  /* 12+ */ -1, -1)
F(dst_da1_subreg_nr,   /* 4+ */ 52,  48,  /* 12+ */ 55, 51)
F(da16_writemask,      /* 4+ */ 51,  48,  /* 12+ */ -1, -1) /* Dst.ChanEn */
F8(src0_reg_hw_type,   /* 4+ */ 41,  39,  /* 8+ */  46,  43, /* 12+ */ 43, 40)
FI(src0_reg_file,      /* 4+ */ 38,  37,  /* 8+ */  42,  41, /* 12+ */ 46, 66)
F(src0_is_imm,         /* 4+ */ -1,  -1,  /* 12+ */ 46, 46)
F8(dst_reg_hw_type,    /* 4+ */ 36,  34,  /* 8+ */  40,  37, /* 12+ */ 39, 36)
F8(dst_reg_file,       /* 4+ */ 33,  32,  /* 8+ */  36,  35, /* 12+ */ 50, 50)
F8(mask_control,       /* 4+ */  9,   9,  /* 8+ */  34,  34, /* 12+ */ 31, 31)
FF(flag_reg_nr,
   /* 4-6: doesn't exist */ -1, -1, -1, -1, -1, -1, -1, -1,
   /* 7: */ 90, 90,
   /* 8: */ 33, 33,
   /* 12: */ 23, 23)
F8(flag_subreg_nr,     /* 4+ */ 89,  89,  /* 8+ */ 32, 32,   /* 12+ */ 22, 22)
F(saturate,            /* 4+ */ 31,  31,  /* 12+ */ 34, 34)
F(debug_control,       /* 4+ */ 30,  30,  /* 12+ */ 30, 30)
F(cmpt_control,        /* 4+ */ 29,  29,  /* 12+ */ 29, 29)
FC(branch_control,     /* 4+ */ 28,  28,  /* 12+ */ 33, 33, devinfo->gen >= 8)
FC(acc_wr_control,     /* 4+ */ 28,  28,  /* 12+ */ 33, 33, devinfo->gen >= 6)
FC(mask_control_ex,    /* 4+ */ 28,  28,  /* 12+ */ -1, -1, devinfo->is_g4x || devinfo->gen == 5)
F(cond_modifier,       /* 4+ */ 27,  24,  /* 12+ */ 95, 92)
FC(math_function,      /* 4+ */ 27,  24,  /* 12+ */ 95, 92, devinfo->gen >= 6)
F(exec_size,           /* 4+ */ 23,  21,  /* 12+ */ 18, 16)
F(pred_inv,            /* 4+ */ 20,  20,  /* 12+ */ 28, 28)
F(pred_control,        /* 4+ */ 19,  16,  /* 12+ */ 27, 24)
F(thread_control,      /* 4+ */ 15,  14,  /* 12+ */ -1, -1)
F(atomic_control,      /* 4+ */ -1,  -1,  /* 12+ */ 32, 32)
F(qtr_control,         /* 4+ */ 13,  12,  /* 12+ */ 21, 20)
FF(nib_control,
   /* 4-6: doesn't exist */ -1, -1, -1, -1, -1, -1, -1, -1,
   /* 7: */ 47, 47,
   /* 8: */ 11, 11,
   /* 12: */ 19, 19)
F8(no_dd_check,        /* 4+ */  11, 11,  /* 8+ */  10,  10, /* 12+ */ -1, -1)
F8(no_dd_clear,        /* 4+ */  10, 10,  /* 8+ */   9,   9, /* 12+ */ -1, -1)
F(swsb,                /* 4+ */  -1, -1,  /* 12+ */ 15,  8)
FK(access_mode,        /* 4+ */   8,  8,  /* 12+ */ BRW_ALIGN_1)
/* Bit 7 is Reserved (for future Opcode expansion) */
F(hw_opcode,           /* 4+ */   6,  0,  /* 12+ */ 6,  0)

/**
 * Three-source instructions:
 *  @{
 */
F(3src_src2_reg_nr,        /* 4+ */ 125, 118, /* 12+ */ -1, -1) /* same in align1 */
F(3src_a16_src2_subreg_nr, /* 4+ */ 117, 115, /* 12+ */ -1, -1) /* Extra discontiguous bit on CHV? */
F(3src_a16_src2_swizzle,   /* 4+ */ 114, 107, /* 12+ */ -1, -1)
F(3src_a16_src2_rep_ctrl,  /* 4+ */ 106, 106, /* 12+ */ -1, -1)
F(3src_src1_reg_nr,        /* 4+ */ 104,  97, /* 12+ */ -1, -1) /* same in align1 */
F(3src_a16_src1_subreg_nr, /* 4+ */ 96,  94,  /* 12+ */ -1, -1) /* Extra discontiguous bit on CHV? */
F(3src_a16_src1_swizzle,   /* 4+ */ 93,  86,  /* 12+ */ -1, -1)
F(3src_a16_src1_rep_ctrl,  /* 4+ */ 85,  85,  /* 12+ */ -1, -1)
F(3src_src0_reg_nr,        /* 4+ */ 83,  76,  /* 12+ */ -1, -1) /* same in align1 */
F(3src_a16_src0_subreg_nr, /* 4+ */ 75,  73,  /* 12+ */ -1, -1) /* Extra discontiguous bit on CHV? */
F(3src_a16_src0_swizzle,   /* 4+ */ 72,  65,  /* 12+ */ -1, -1)
F(3src_a16_src0_rep_ctrl,  /* 4+ */ 64,  64,  /* 12+ */ -1, -1)
F(3src_dst_reg_nr,         /* 4+ */ 63,  56,  /* 12+ */ -1, -1) /* same in align1 */
F(3src_a16_dst_subreg_nr,  /* 4+ */ 55,  53,  /* 12+ */ -1, -1)
F(3src_a16_dst_writemask,  /* 4+ */ 52,  49,  /* 12+ */ -1, -1)
F8(3src_a16_nib_ctrl,      /* 4+ */ 47, 47,   /* 8+ */ 11, 11, /* 12+ */ -1, -1) /* only exists on IVB+ */
F8(3src_a16_dst_hw_type,   /* 4+ */ 45, 44,   /* 8+ */ 48, 46, /* 12+ */ -1, -1) /* only exists on IVB+ */
F8(3src_a16_src_hw_type,   /* 4+ */ 43, 42,   /* 8+ */ 45, 43, /* 12+ */ -1, -1)
F8(3src_src2_negate,       /* 4+ */ 41, 41,   /* 8+ */ 42, 42, /* 12+ */ -1, -1)
F8(3src_src2_abs,          /* 4+ */ 40, 40,   /* 8+ */ 41, 41, /* 12+ */ -1, -1)
F8(3src_src1_negate,       /* 4+ */ 39, 39,   /* 8+ */ 40, 40, /* 12+ */ -1, -1)
F8(3src_src1_abs,          /* 4+ */ 38, 38,   /* 8+ */ 39, 39, /* 12+ */ -1, -1)
F8(3src_src0_negate,       /* 4+ */ 37, 37,   /* 8+ */ 38, 38, /* 12+ */ -1, -1)
F8(3src_src0_abs,          /* 4+ */ 36, 36,   /* 8+ */ 37, 37, /* 12+ */ -1, -1)
F8(3src_a16_src1_type,     /* 4+ */ -1, -1,   /* 8+ */ 36, 36, /* 12+ */ -1, -1)
F8(3src_a16_src2_type,     /* 4+ */ -1, -1,   /* 8+ */ 35, 35, /* 12+ */ -1, -1)
F8(3src_a16_flag_reg_nr,   /* 4+ */ 34, 34,   /* 8+ */ 33, 33, /* 12+ */ -1, -1)
F8(3src_a16_flag_subreg_nr,/* 4+ */ 33, 33,   /* 8+ */ 32, 32, /* 12+ */ -1, -1)
FF(3src_a16_dst_reg_file,
   /* 4-5: doesn't exist - no 3-source instructions */ -1, -1, -1, -1, -1, -1,
   /* 6: */ 32, 32,
   /* 7-8: doesn't exist - no MRFs */ -1, -1, -1, -1,
   /* 12: */ -1, -1)
F(3src_saturate,           /* 4+ */ 31, 31,      /* 12+ */ -1, -1)
F(3src_debug_control,      /* 4+ */ 30, 30,      /* 12+ */ -1, -1)
F(3src_cmpt_control,       /* 4+ */ 29, 29,      /* 12+ */ -1, -1)
F(3src_acc_wr_control,     /* 4+ */ 28, 28,      /* 12+ */ -1, -1)
F(3src_cond_modifier,      /* 4+ */ 27, 24,      /* 12+ */ -1, -1)
F(3src_exec_size,          /* 4+ */ 23, 21,      /* 12+ */ -1, -1)
F(3src_pred_inv,           /* 4+ */ 20, 20,      /* 12+ */ -1, -1)
F(3src_pred_control,       /* 4+ */ 19, 16,      /* 12+ */ -1, -1)
F(3src_thread_control,     /* 4+ */ 15, 14,      /* 12+ */ -1, -1)
F(3src_qtr_control,        /* 4+ */ 13, 12,      /* 12+ */ -1, -1)
F8(3src_no_dd_check,       /* 4+ */ 11, 11,      /* 8+ */ 10, 10, /* 12+ */ -1, -1)
F8(3src_no_dd_clear,       /* 4+ */ 10, 10,      /* 8+ */  9,  9, /* 12+ */ -1, -1)
F8(3src_mask_control,      /* 4+ */ 9,  9,       /* 8+ */ 34, 34, /* 12+ */ -1, -1)
F(3src_access_mode,        /* 4+ */ 8,  8,       /* 12+ */ -1, -1)
/* Bit 7 is Reserved (for future Opcode expansion) */
F(3src_hw_opcode,          /* 4+ */ 6,  0,       /* 12+ */ -1, -1)
/** @} */

#define REG_TYPE(reg)                                                         \
static inline void                                                            \
brw_inst_set_3src_a16_##reg##_type(const struct gen_device_info *devinfo,     \
                                   brw_inst *inst, enum brw_reg_type type)    \
{                                                                             \
   unsigned hw_type = brw_reg_type_to_a16_hw_3src_type(devinfo, type);        \
   brw_inst_set_3src_a16_##reg##_hw_type(devinfo, inst, hw_type);             \
}                                                                             \
                                                                              \
static inline enum brw_reg_type                                               \
brw_inst_3src_a16_##reg##_type(const struct gen_device_info *devinfo,         \
                               const brw_inst *inst)                          \
{                                                                             \
   unsigned hw_type = brw_inst_3src_a16_##reg##_hw_type(devinfo, inst);       \
   return brw_a16_hw_3src_type_to_reg_type(devinfo, hw_type);                 \
}

REG_TYPE(dst)
REG_TYPE(src)
#undef REG_TYPE

/**
 * Three-source align1 instructions:
 *  @{
 */
/* Reserved 127:126 */
/* src2_reg_nr same in align16 */
FC(3src_a1_src2_subreg_nr, /* 4+ */ 117, 113, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src2_hstride,   /* 4+ */ 112, 111, /* 12+ */ -1, -1, devinfo->gen >= 10)
/* Reserved 110:109. src2 vstride is an implied parameter */
FC(3src_a1_src2_hw_type,   /* 4+ */ 108, 106, /* 12+ */ -1, -1, devinfo->gen >= 10)
/* Reserved 105 */
/* src1_reg_nr same in align16 */
FC(3src_a1_src1_subreg_nr,  /* 4+ */ 96,  92, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src1_hstride,    /* 4+ */ 91,  90, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src1_vstride,    /* 4+ */ 89,  88, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src1_hw_type,    /* 4+ */ 87,  85, /* 12+ */ -1, -1, devinfo->gen >= 10)
/* Reserved 84 */
/* src0_reg_nr same in align16 */
FC(3src_a1_src0_subreg_nr,  /* 4+ */ 75,  71, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src0_hstride,    /* 4+ */ 70,  69, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src0_vstride,    /* 4+ */ 68,  67, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src0_hw_type,    /* 4+ */ 66,  64, /* 12+ */ -1, -1, devinfo->gen >= 10)
/* dst_reg_nr same in align16 */
FC(3src_a1_dst_subreg_nr,   /* 4+ */ 55,  54, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_special_acc,     /* 4+ */ 55,  52, /* 12+ */ -1, -1, devinfo->gen >= 10) /* aliases dst_subreg_nr */
/* Reserved 51:50 */
FC(3src_a1_dst_hstride,     /* 4+ */ 49,  49, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_dst_hw_type,     /* 4+ */ 48,  46, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src2_reg_file,   /* 4+ */ 45,  45, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src1_reg_file,   /* 4+ */ 44,  44, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_src0_reg_file,   /* 4+ */ 43,  43, /* 12+ */ -1, -1, devinfo->gen >= 10)
/* Source Modifier fields same in align16 */
FC(3src_a1_dst_reg_file,    /* 4+ */ 36,  36, /* 12+ */ -1, -1, devinfo->gen >= 10)
FC(3src_a1_exec_type,       /* 4+ */ 35,  35, /* 12+ */ -1, -1, devinfo->gen >= 10)
/* Fields below this same in align16 */
/** @} */

#define REG_TYPE(reg)                                                         \
static inline void                                                            \
brw_inst_set_3src_a1_##reg##_type(const struct gen_device_info *devinfo,      \
                                  brw_inst *inst, enum brw_reg_type type)     \
{                                                                             \
   UNUSED enum gen10_align1_3src_exec_type exec_type =                        \
      (enum gen10_align1_3src_exec_type) brw_inst_3src_a1_exec_type(devinfo,  \
                                                                    inst);    \
   if (brw_reg_type_is_floating_point(type)) {                                \
      assert(exec_type == BRW_ALIGN1_3SRC_EXEC_TYPE_FLOAT);                   \
   } else {                                                                   \
      assert(exec_type == BRW_ALIGN1_3SRC_EXEC_TYPE_INT);                     \
   }                                                                          \
   unsigned hw_type = brw_reg_type_to_a1_hw_3src_type(devinfo, type);         \
   brw_inst_set_3src_a1_##reg##_hw_type(devinfo, inst, hw_type);              \
}                                                                             \
                                                                              \
static inline enum brw_reg_type                                               \
brw_inst_3src_a1_##reg##_type(const struct gen_device_info *devinfo,          \
                              const brw_inst *inst)                           \
{                                                                             \
   enum gen10_align1_3src_exec_type exec_type =                               \
      (enum gen10_align1_3src_exec_type) brw_inst_3src_a1_exec_type(devinfo,  \
                                                                    inst);    \
   unsigned hw_type = brw_inst_3src_a1_##reg##_hw_type(devinfo, inst);        \
   return brw_a1_hw_3src_type_to_reg_type(devinfo, hw_type, exec_type);       \
}

REG_TYPE(dst)
REG_TYPE(src0)
REG_TYPE(src1)
REG_TYPE(src2)
#undef REG_TYPE

/**
 * Three-source align1 instruction immediates:
 *  @{
 */
static inline uint16_t
brw_inst_3src_a1_src0_imm(ASSERTED const struct gen_device_info *devinfo,
                          const brw_inst *insn)
{
   assert(devinfo->gen >= 10);
   return brw_inst_bits(insn, 82, 67);
}

static inline uint16_t
brw_inst_3src_a1_src2_imm(ASSERTED const struct gen_device_info *devinfo,
                          const brw_inst *insn)
{
   assert(devinfo->gen >= 10);
   return brw_inst_bits(insn, 124, 109);
}

static inline void
brw_inst_set_3src_a1_src0_imm(ASSERTED const struct gen_device_info *devinfo,
                              brw_inst *insn, uint16_t value)
{
   assert(devinfo->gen >= 10);
   brw_inst_set_bits(insn, 82, 67, value);
}

static inline void
brw_inst_set_3src_a1_src2_imm(ASSERTED const struct gen_device_info *devinfo,
                              brw_inst *insn, uint16_t value)
{
   assert(devinfo->gen >= 10);
   brw_inst_set_bits(insn, 124, 109, value);
}
/** @} */

/**
 * Flow control instruction bits:
 *  @{
 */
static inline void
brw_inst_set_uip(const struct gen_device_info *devinfo,
                 brw_inst *inst, int32_t value)
{
   assert(devinfo->gen >= 6);

   if (devinfo->gen >= 8) {
      brw_inst_set_bits(inst, 95, 64, (uint32_t)value);
   } else {
      assert(value <= (1 << 16) - 1);
      assert(value > -(1 << 16));
      brw_inst_set_bits(inst, 127, 112, (uint16_t)value);
   }
}

static inline int32_t
brw_inst_uip(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   assert(devinfo->gen >= 6);

   if (devinfo->gen >= 8) {
      return brw_inst_bits(inst, 95, 64);
   } else {
      return (int16_t)brw_inst_bits(inst, 127, 112);
   }
}

static inline void
brw_inst_set_jip(const struct gen_device_info *devinfo,
                 brw_inst *inst, int32_t value)
{
   assert(devinfo->gen >= 6);

   if (devinfo->gen >= 8) {
      brw_inst_set_bits(inst, 127, 96, (uint32_t)value);
   } else {
      assert(value <= (1 << 15) - 1);
      assert(value >= -(1 << 15));
      brw_inst_set_bits(inst, 111, 96, (uint16_t)value);
   }
}

static inline int32_t
brw_inst_jip(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   assert(devinfo->gen >= 6);

   if (devinfo->gen >= 8) {
      return brw_inst_bits(inst, 127, 96);
   } else {
      return (int16_t)brw_inst_bits(inst, 111, 96);
   }
}

/** Like FC, but using int16_t to handle negative jump targets. */
#define FJ(name, high, low, assertions)                                       \
static inline void                                                            \
brw_inst_set_##name(const struct gen_device_info *devinfo, brw_inst *inst, int16_t v) \
{                                                                             \
   assert(assertions);                                                        \
   (void) devinfo;                                                            \
   brw_inst_set_bits(inst, high, low, (uint16_t) v);                          \
}                                                                             \
static inline int16_t                                                         \
brw_inst_##name(const struct gen_device_info *devinfo, const brw_inst *inst)  \
{                                                                             \
   assert(assertions);                                                        \
   (void) devinfo;                                                            \
   return brw_inst_bits(inst, high, low);                                     \
}

FJ(gen6_jump_count,  63,  48, devinfo->gen == 6)
FJ(gen4_jump_count, 111,  96, devinfo->gen < 6)
FC(gen4_pop_count,  /* 4+ */ 115, 112, /* 12+ */ -1, -1, devinfo->gen < 6)
/** @} */

/**
 * SEND instructions:
 *  @{
 */
FC(send_ex_desc_ia_subreg_nr, /* 4+ */ 82, 80, /* 12+ */ -1, -1, devinfo->gen >= 9)
FC(send_src0_address_mode,    /* 4+ */ 79, 79, /* 12+ */ -1, -1, devinfo->gen >= 9)
FC(send_sel_reg32_desc,       /* 4+ */ 77, 77, /* 12+ */ -1, -1, devinfo->gen >= 9)
FC(send_sel_reg32_ex_desc,    /* 4+ */ 61, 61, /* 12+ */ -1, -1, devinfo->gen >= 9)
FC(send_src1_reg_nr,          /* 4+ */ 51, 44, /* 12+ */ -1, -1, devinfo->gen >= 9)
FC(send_src1_reg_file,        /* 4+ */ 36, 36, /* 12+ */ -1, -1, devinfo->gen >= 9)
FC(send_dst_reg_file,         /* 4+ */ 35, 35, /* 12+ */ -1, -1, devinfo->gen >= 9)
/** @} */

/* Message descriptor bits */
#define MD(x) ((x) + 96)

/**
 * Set the SEND(C) message descriptor immediate.
 *
 * This doesn't include the SFID nor the EOT field that were considered to be
 * part of the message descriptor by ancient versions of the BSpec, because
 * they are present in the instruction even if the message descriptor is
 * provided indirectly in the address register, so we want to specify them
 * separately.
 */
static inline void
brw_inst_set_send_desc(const struct gen_device_info *devinfo,
                       brw_inst *inst, uint32_t value)
{
   if (devinfo->gen >= 9) {
      brw_inst_set_bits(inst, 126, 96, value);
      assert(value >> 31 == 0);
   } else if (devinfo->gen >= 5) {
      brw_inst_set_bits(inst, 124, 96, value);
      assert(value >> 29 == 0);
   } else {
      brw_inst_set_bits(inst, 119, 96, value);
      assert(value >> 24 == 0);
   }
}

/**
 * Get the SEND(C) message descriptor immediate.
 *
 * \sa brw_inst_set_send_desc().
 */
static inline uint32_t
brw_inst_send_desc(const struct gen_device_info *devinfo, const brw_inst *inst)
{
   if (devinfo->gen >= 9)
      return brw_inst_bits(inst, 126, 96);
   else if (devinfo->gen >= 5)
      return brw_inst_bits(inst, 124, 96);
   else
      return brw_inst_bits(inst, 119, 96);
}

/**
 * Set the SEND(C) message extended descriptor immediate.
 *
 * This doesn't include the SFID nor the EOT field that were considered to be
 * part of the extended message descriptor by some versions of the BSpec,
 * because they are present in the instruction even if the extended message
 * descriptor is provided indirectly in a register, so we want to specify them
 * separately.
 */
static inline void
brw_inst_set_send_ex_desc(const struct gen_device_info *devinfo,
                          brw_inst *inst, uint32_t value)
{
   assert(devinfo->gen >= 9);
   brw_inst_set_bits(inst, 94, 91, GET_BITS(value, 31, 28));
   brw_inst_set_bits(inst, 88, 85, GET_BITS(value, 27, 24));
   brw_inst_set_bits(inst, 83, 80, GET_BITS(value, 23, 20));
   brw_inst_set_bits(inst, 67, 64, GET_BITS(value, 19, 16));
   assert(GET_BITS(value, 15, 0) == 0);
}

/**
 * Set the SENDS(C) message extended descriptor immediate.
 *
 * This doesn't include the SFID nor the EOT field that were considered to be
 * part of the extended message descriptor by some versions of the BSpec,
 * because they are present in the instruction even if the extended message
 * descriptor is provided indirectly in a register, so we want to specify them
 * separately.
 */
static inline void
brw_inst_set_sends_ex_desc(const struct gen_device_info *devinfo,
                           brw_inst *inst, uint32_t value)
{
   brw_inst_set_bits(inst, 95, 80, GET_BITS(value, 31, 16));
   assert(GET_BITS(value, 15, 10) == 0);
   brw_inst_set_bits(inst, 67, 64, GET_BITS(value, 9, 6));
   assert(GET_BITS(value, 5, 0) == 0);
}

/**
 * Get the SEND(C) message extended descriptor immediate.
 *
 * \sa brw_inst_set_send_ex_desc().
 */
static inline uint32_t
brw_inst_send_ex_desc(const struct gen_device_info *devinfo,
                      const brw_inst *inst)
{
   assert(devinfo->gen >= 9);
   return (brw_inst_bits(inst, 94, 91) << 28 |
           brw_inst_bits(inst, 88, 85) << 24 |
           brw_inst_bits(inst, 83, 80) << 20 |
           brw_inst_bits(inst, 67, 64) << 16);
}

/**
 * Get the SENDS(C) message extended descriptor immediate.
 *
 * \sa brw_inst_set_send_ex_desc().
 */
static inline uint32_t
brw_inst_sends_ex_desc(const struct gen_device_info *devinfo,
                       const brw_inst *inst)
{
   return (brw_inst_bits(inst, 95, 80) << 16 |
           brw_inst_bits(inst, 67, 64) << 6);
}

/**
 * Fields for SEND messages:
 *  @{
 */
F(eot,                 /* 4+ */ 127, 127, /* 12+ */ -1, -1)
FF(mlen,
   /* 4:   */ 119, 116,
   /* 4.5: */ 119, 116,
   /* 5:   */ 124, 121,
   /* 6:   */ 124, 121,
   /* 7:   */ 124, 121,
   /* 8:   */ 124, 121,
   /* 12:  */ -1, -1);
FF(rlen,
   /* 4:   */ 115, 112,
   /* 4.5: */ 115, 112,
   /* 5:   */ 120, 116,
   /* 6:   */ 120, 116,
   /* 7:   */ 120, 116,
   /* 8:   */ 120, 116,
   /* 12:  */ -1, -1);
FF(header_present,
   /* 4: doesn't exist */ -1, -1, -1, -1,
   /* 5:   */ 115, 115,
   /* 6:   */ 115, 115,
   /* 7:   */ 115, 115,
   /* 8:   */ 115, 115,
   /* 12:  */ -1, -1)
F(gateway_notify, /* 4+ */ MD(16), MD(15), /* 12+ */ -1, -1)
FF(function_control,
   /* 4:   */ 111,  96,
   /* 4.5: */ 111,  96,
   /* 5:   */ 114,  96,
   /* 6:   */ 114,  96,
   /* 7:   */ 114,  96,
   /* 8:   */ 114,  96,
   /* 12:  */ -1, -1)
FF(gateway_subfuncid,
   /* 4:   */ MD(1), MD(0),
   /* 4.5: */ MD(1), MD(0),
   /* 5:   */ MD(1), MD(0), /* 2:0, but bit 2 is reserved MBZ */
   /* 6:   */ MD(2), MD(0),
   /* 7:   */ MD(2), MD(0),
   /* 8:   */ MD(2), MD(0),
   /* 12:  */ -1, -1)
FF(sfid,
   /* 4:   */ 123, 120, /* called msg_target */
   /* 4.5  */ 123, 120,
   /* 5:   */  95,  92,
   /* 6:   */  27,  24,
   /* 7:   */  27,  24,
   /* 8:   */  27,  24,
   /* 12:  */ -1, -1)
FF(null_rt,
   /* 4-7: */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
   /* 8:   */ 80, 80,
   /* 12: */ -1, -1) /* actually only Gen11+ */
FC(base_mrf,   /* 4+ */ 27,  24, /* 12+ */ -1, -1, devinfo->gen < 6);
/** @} */

/**
 * URB message function control bits:
 *  @{
 */
FF(urb_per_slot_offset,
   /* 4-6: */ -1, -1, -1, -1, -1, -1, -1, -1,
   /* 7:   */ MD(16), MD(16),
   /* 8:   */ MD(17), MD(17),
   /* 12:  */ -1, -1)
FC(urb_channel_mask_present, /* 4+ */ MD(15), MD(15), /* 12+ */ -1, -1, devinfo->gen >= 8)
FC(urb_complete, /* 4+ */ MD(15), MD(15), /* 12+ */ -1, -1, devinfo->gen < 8)
FC(urb_used,     /* 4+ */ MD(14), MD(14), /* 12+ */ -1, -1, devinfo->gen < 7)
FC(urb_allocate, /* 4+ */ MD(13), MD(13), /* 12+ */ -1, -1, devinfo->gen < 7)
FF(urb_swizzle_control,
   /* 4:   */ MD(11), MD(10),
   /* 4.5: */ MD(11), MD(10),
   /* 5:   */ MD(11), MD(10),
   /* 6:   */ MD(11), MD(10),
   /* 7:   */ MD(14), MD(14),
   /* 8:   */ MD(15), MD(15),
   /* 12:  */ -1, -1)
FF(urb_global_offset,
   /* 4:   */ MD( 9), MD(4),
   /* 4.5: */ MD( 9), MD(4),
   /* 5:   */ MD( 9), MD(4),
   /* 6:   */ MD( 9), MD(4),
   /* 7:   */ MD(13), MD(3),
   /* 8:   */ MD(14), MD(4),
   /* 12:  */ -1, -1)
FF(urb_opcode,
   /* 4:   */ MD( 3), MD(0),
   /* 4.5: */ MD( 3), MD(0),
   /* 5:   */ MD( 3), MD(0),
   /* 6:   */ MD( 3), MD(0),
   /* 7:   */ MD( 2), MD(0),
   /* 8:   */ MD( 3), MD(0),
   /* 12:  */ -1, -1)
/** @} */

/**
 * Gen4-5 math messages:
 *  @{
 */
FC(math_msg_data_type,  /* 4+ */ MD(7), MD(7), /* 12+ */ -1, -1, devinfo->gen < 6)
FC(math_msg_saturate,   /* 4+ */ MD(6), MD(6), /* 12+ */ -1, -1, devinfo->gen < 6)
FC(math_msg_precision,  /* 4+ */ MD(5), MD(5), /* 12+ */ -1, -1, devinfo->gen < 6)
FC(math_msg_signed_int, /* 4+ */ MD(4), MD(4), /* 12+ */ -1, -1, devinfo->gen < 6)
FC(math_msg_function,   /* 4+ */ MD(3), MD(0), /* 12+ */ -1, -1, devinfo->gen < 6)
/** @} */

/**
 * Sampler message function control bits:
 *  @{
 */
FF(sampler_simd_mode,
   /* 4: doesn't exist */ -1, -1, -1, -1,
   /* 5:   */ MD(17), MD(16),
   /* 6:   */ MD(17), MD(16),
   /* 7:   */ MD(18), MD(17),
   /* 8:   */ MD(18), MD(17),
   /* 12:  */ -1, -1)
FF(sampler_msg_type,
   /* 4:   */ MD(15), MD(14),
   /* 4.5: */ MD(15), MD(12),
   /* 5:   */ MD(15), MD(12),
   /* 6:   */ MD(15), MD(12),
   /* 7:   */ MD(16), MD(12),
   /* 8:   */ MD(16), MD(12),
   /* 12:  */ -1, -1)
FC(sampler_return_format, /* 4+ */ MD(13), MD(12), /* 12+ */ -1, -1, devinfo->gen == 4 && !devinfo->is_g4x)
F(sampler,                /* 4+ */ MD(11), MD(8),  /* 12+ */ -1, -1)
F(binding_table_index,    /* 4+ */ MD( 7), MD(0),  /* 12+ */ -1, -1) /* also used by other messages */
/** @} */

/**
 * Data port message function control bits:
 *  @{
 */
FC(dp_category,           /* 4+ */ MD(18), MD(18), /* 12+ */ -1, -1, devinfo->gen >= 7)

/* Gen4-5 store fields in different bits for read/write messages. */
FF(dp_read_msg_type,
   /* 4:   */ MD(13), MD(12),
   /* 4.5: */ MD(13), MD(11),
   /* 5:   */ MD(13), MD(11),
   /* 6:   */ MD(16), MD(13),
   /* 7:   */ MD(17), MD(14),
   /* 8:   */ MD(17), MD(14),
   /* 12:  */ -1, -1)
FF(dp_write_msg_type,
   /* 4:   */ MD(14), MD(12),
   /* 4.5: */ MD(14), MD(12),
   /* 5:   */ MD(14), MD(12),
   /* 6:   */ MD(16), MD(13),
   /* 7:   */ MD(17), MD(14),
   /* 8:   */ MD(17), MD(14),
   /* 12:  */ -1, -1)
FF(dp_read_msg_control,
   /* 4:   */ MD(11), MD( 8),
   /* 4.5: */ MD(10), MD( 8),
   /* 5:   */ MD(10), MD( 8),
   /* 6:   */ MD(12), MD( 8),
   /* 7:   */ MD(13), MD( 8),
   /* 8:   */ MD(13), MD( 8),
   /* 12:  */ -1, -1)
FF(dp_write_msg_control,
   /* 4:   */ MD(11), MD( 8),
   /* 4.5: */ MD(11), MD( 8),
   /* 5:   */ MD(11), MD( 8),
   /* 6:   */ MD(12), MD( 8),
   /* 7:   */ MD(13), MD( 8),
   /* 8:   */ MD(13), MD( 8),
   /* 12:  */ -1, -1)
FC(dp_read_target_cache, /* 4+ */ MD(15), MD(14), /* 12+ */ -1, -1, devinfo->gen < 6);

FF(dp_write_commit,
   /* 4:   */ MD(15),  MD(15),
   /* 4.5: */ MD(15),  MD(15),
   /* 5:   */ MD(15),  MD(15),
   /* 6:   */ MD(17),  MD(17),
   /* 7+: does not exist */ -1, -1, -1, -1,
   /* 12:  */ -1, -1)

/* Gen6+ use the same bit locations for everything. */
FF(dp_msg_type,
   /* 4-5: use dp_read_msg_type or dp_write_msg_type instead */
   -1, -1, -1, -1, -1, -1,
   /* 6:   */ MD(16), MD(13),
   /* 7:   */ MD(17), MD(14),
   /* 8:   */ MD(18), MD(14),
   /* 12:  */ -1, -1)
FF(dp_msg_control,
   /* 4:   */ MD(11), MD( 8),
   /* 4.5-5: use dp_read_msg_control or dp_write_msg_control */ -1, -1, -1, -1,
   /* 6:   */ MD(12), MD( 8),
   /* 7:   */ MD(13), MD( 8),
   /* 8:   */ MD(13), MD( 8),
   /* 12:  */ -1, -1)
/** @} */

/**
 * Scratch message bits (Gen7+):
 *  @{
 */
FC(scratch_read_write, /* 4+ */ MD(17), MD(17), /* 12+ */ -1, -1, devinfo->gen >= 7) /* 0 = read,  1 = write */
FC(scratch_type,       /* 4+ */ MD(16), MD(16), /* 12+ */ -1, -1, devinfo->gen >= 7) /* 0 = OWord, 1 = DWord */
FC(scratch_invalidate_after_read, /* 4+ */ MD(15), MD(15), /* 12+ */ -1, -1, devinfo->gen >= 7)
FC(scratch_block_size,  /* 4+ */ MD(13),  MD(12), /* 12+ */ -1, -1, devinfo->gen >= 7)
FC(scratch_addr_offset, /* 4+ */ MD(11),  MD( 0), /* 12+ */ -1, -1, devinfo->gen >= 7)
/** @} */

/**
 * Render Target message function control bits:
 *  @{
 */
FF(rt_last,
   /* 4:   */ MD(11), MD(11),
   /* 4.5: */ MD(11), MD(11),
   /* 5:   */ MD(11), MD(11),
   /* 6:   */ MD(12), MD(12),
   /* 7:   */ MD(12), MD(12),
   /* 8:   */ MD(12), MD(12),
   /* 12:  */ -1, -1)
FC(rt_slot_group,      /* 4+ */ MD(11),  MD(11), /* 12+ */ -1, -1, devinfo->gen >= 6)
F(rt_message_type,     /* 4+ */ MD(10),  MD( 8), /* 12+ */ -1, -1)
/** @} */

/**
 * Thread Spawn message function control bits:
 *  @{
 */
F(ts_resource_select,  /* 4+ */ MD( 4),  MD( 4), /* 12+ */ -1, -1)
F(ts_request_type,     /* 4+ */ MD( 1),  MD( 1), /* 12+ */ -1, -1)
F(ts_opcode,           /* 4+ */ MD( 0),  MD( 0), /* 12+ */ -1, -1)
/** @} */

/**
 * Pixel Interpolator message function control bits:
 *  @{
 */
F(pi_simd_mode,        /* 4+ */ MD(16),  MD(16), /* 12+ */ -1, -1)
F(pi_nopersp,          /* 4+ */ MD(14),  MD(14), /* 12+ */ -1, -1)
F(pi_message_type,     /* 4+ */ MD(13),  MD(12), /* 12+ */ -1, -1)
F(pi_slot_group,       /* 4+ */ MD(11),  MD(11), /* 12+ */ -1, -1)
F(pi_message_data,     /* 4+ */ MD(7),   MD(0),  /* 12+ */ -1, -1)
/** @} */

/**
 * Immediates:
 *  @{
 */
static inline int
brw_inst_imm_d(const struct gen_device_info *devinfo, const brw_inst *insn)
{
   (void) devinfo;
   return brw_inst_bits(insn, 127, 96);
}

static inline unsigned
brw_inst_imm_ud(const struct gen_device_info *devinfo, const brw_inst *insn)
{
   (void) devinfo;
   return brw_inst_bits(insn, 127, 96);
}

static inline uint64_t
brw_inst_imm_uq(ASSERTED const struct gen_device_info *devinfo,
                const brw_inst *insn)
{
   assert(devinfo->gen >= 8);
   return brw_inst_bits(insn, 127, 64);
}

static inline float
brw_inst_imm_f(const struct gen_device_info *devinfo, const brw_inst *insn)
{
   union {
      float f;
      uint32_t u;
   } ft;
   (void) devinfo;
   ft.u = brw_inst_bits(insn, 127, 96);
   return ft.f;
}

static inline double
brw_inst_imm_df(const struct gen_device_info *devinfo, const brw_inst *insn)
{
   union {
      double d;
      uint64_t u;
   } dt;
   (void) devinfo;
   dt.u = brw_inst_bits(insn, 127, 64);
   return dt.d;
}

static inline void
brw_inst_set_imm_d(const struct gen_device_info *devinfo,
                   brw_inst *insn, int value)
{
   (void) devinfo;
   return brw_inst_set_bits(insn, 127, 96, value);
}

static inline void
brw_inst_set_imm_ud(const struct gen_device_info *devinfo,
                    brw_inst *insn, unsigned value)
{
   (void) devinfo;
   return brw_inst_set_bits(insn, 127, 96, value);
}

static inline void
brw_inst_set_imm_f(const struct gen_device_info *devinfo,
                   brw_inst *insn, float value)
{
   union {
      float f;
      uint32_t u;
   } ft;
   (void) devinfo;
   ft.f = value;
   brw_inst_set_bits(insn, 127, 96, ft.u);
}

static inline void
brw_inst_set_imm_df(const struct gen_device_info *devinfo,
                    brw_inst *insn, double value)
{
   union {
      double d;
      uint64_t u;
   } dt;
   (void) devinfo;
   dt.d = value;
   brw_inst_set_bits(insn, 127, 64, dt.u);
}

static inline void
brw_inst_set_imm_uq(const struct gen_device_info *devinfo,
                    brw_inst *insn, uint64_t value)
{
   (void) devinfo;
   brw_inst_set_bits(insn, 127, 64, value);
}

/** @} */

#define REG_TYPE(reg)                                                         \
static inline void                                                            \
brw_inst_set_##reg##_file_type(const struct gen_device_info *devinfo,         \
                               brw_inst *inst, enum brw_reg_file file,        \
                               enum brw_reg_type type)                        \
{                                                                             \
   assert(file <= BRW_IMMEDIATE_VALUE);                                       \
   unsigned hw_type = brw_reg_type_to_hw_type(devinfo, file, type);           \
   brw_inst_set_##reg##_reg_file(devinfo, inst, file);                        \
   brw_inst_set_##reg##_reg_hw_type(devinfo, inst, hw_type);                  \
}                                                                             \
                                                                              \
static inline enum brw_reg_type                                               \
brw_inst_##reg##_type(const struct gen_device_info *devinfo,                  \
                      const brw_inst *inst)                                   \
{                                                                             \
   unsigned file = __builtin_strcmp("dst", #reg) == 0 ?                       \
                   (unsigned) BRW_GENERAL_REGISTER_FILE :                     \
                   brw_inst_##reg##_reg_file(devinfo, inst);                  \
   unsigned hw_type = brw_inst_##reg##_reg_hw_type(devinfo, inst);            \
   return brw_hw_type_to_reg_type(devinfo, (enum brw_reg_file)file, hw_type); \
}

REG_TYPE(dst)
REG_TYPE(src0)
REG_TYPE(src1)
#undef REG_TYPE


/* The AddrImm fields are split into two discontiguous sections on Gen8+ */
#define BRW_IA1_ADDR_IMM(reg, g4_high, g4_low, g8_nine, g8_high, g8_low) \
static inline void                                                       \
brw_inst_set_##reg##_ia1_addr_imm(const struct gen_device_info *devinfo, \
                                  brw_inst *inst,                        \
                                  unsigned value)                        \
{                                                                        \
   assert((value & ~0x3ff) == 0);                                        \
   if (devinfo->gen >= 8) {                                              \
      brw_inst_set_bits(inst, g8_high, g8_low, value & 0x1ff);           \
      brw_inst_set_bits(inst, g8_nine, g8_nine, value >> 9);             \
   } else {                                                              \
      brw_inst_set_bits(inst, g4_high, g4_low, value);                   \
   }                                                                     \
}                                                                        \
static inline unsigned                                                   \
brw_inst_##reg##_ia1_addr_imm(const struct gen_device_info *devinfo,     \
                              const brw_inst *inst)                      \
{                                                                        \
   if (devinfo->gen >= 8) {                                              \
      return brw_inst_bits(inst, g8_high, g8_low) |                      \
             (brw_inst_bits(inst, g8_nine, g8_nine) << 9);               \
   } else {                                                              \
      return brw_inst_bits(inst, g4_high, g4_low);                       \
   }                                                                     \
}

/* AddrImm[9:0] for Align1 Indirect Addressing */
/*                     -Gen 4-  ----Gen8---- */
BRW_IA1_ADDR_IMM(src1, 105, 96, 121, 104, 96)
BRW_IA1_ADDR_IMM(src0,  73, 64,  95,  72, 64)
BRW_IA1_ADDR_IMM(dst,   57, 48,  47,  56, 48)

#define BRW_IA16_ADDR_IMM(reg, g4_high, g4_low, g8_nine, g8_high, g8_low) \
static inline void                                                        \
brw_inst_set_##reg##_ia16_addr_imm(const struct gen_device_info *devinfo, \
                                   brw_inst *inst, unsigned value)        \
{                                                                         \
   assert((value & ~0x3ff) == 0);                                         \
   if (devinfo->gen >= 8) {                                               \
      assert(GET_BITS(value, 3, 0) == 0);                                 \
      brw_inst_set_bits(inst, g8_high, g8_low, GET_BITS(value, 8, 4));    \
      brw_inst_set_bits(inst, g8_nine, g8_nine, GET_BITS(value, 9, 9));   \
   } else {                                                               \
      brw_inst_set_bits(inst, g4_high, g4_low, value);                    \
   }                                                                      \
}                                                                         \
static inline unsigned                                                    \
brw_inst_##reg##_ia16_addr_imm(const struct gen_device_info *devinfo,     \
                               const brw_inst *inst)                      \
{                                                                         \
   if (devinfo->gen >= 8) {                                               \
      return (brw_inst_bits(inst, g8_high, g8_low) << 4) |                \
             (brw_inst_bits(inst, g8_nine, g8_nine) << 9);                \
   } else {                                                               \
      return brw_inst_bits(inst, g4_high, g4_low);                        \
   }                                                                      \
}

/* AddrImm[9:0] for Align16 Indirect Addressing:
 * Compared to Align1, these are missing the low 4 bits.
 *                     -Gen 4-  ----Gen8----
 */
BRW_IA16_ADDR_IMM(src1,       105, 96, 121, 104, 100)
BRW_IA16_ADDR_IMM(src0,        73, 64,  95,  72,  68)
BRW_IA16_ADDR_IMM(dst,         57, 52,  47,  56,  52)
BRW_IA16_ADDR_IMM(send_src0,   -1, -1,  78,  72,  68)
BRW_IA16_ADDR_IMM(send_dst,    -1, -1,  62,  56,  52)

/**
 * Fetch a set of contiguous bits from the instruction.
 *
 * Bits indices range from 0..127; fields may not cross 64-bit boundaries.
 */
static inline uint64_t
brw_inst_bits(const brw_inst *inst, unsigned high, unsigned low)
{
   assert(high >= low);
   /* We assume the field doesn't cross 64-bit boundaries. */
   const unsigned word = high / 64;
   assert(word == low / 64);

   high %= 64;
   low %= 64;

   const uint64_t mask = (~0ull >> (64 - (high - low + 1)));

   return (inst->data[word] >> low) & mask;
}

/**
 * Set bits in the instruction, with proper shifting and masking.
 *
 * Bits indices range from 0..127; fields may not cross 64-bit boundaries.
 */
static inline void
brw_inst_set_bits(brw_inst *inst, unsigned high, unsigned low, uint64_t value)
{
   assert(high >= low);
   const unsigned word = high / 64;
   assert(word == low / 64);

   high %= 64;
   low %= 64;

   const uint64_t mask = (~0ull >> (64 - (high - low + 1))) << low;

   /* Make sure the supplied value actually fits in the given bitfield. */
   assert((value & (mask >> low)) == value);

   inst->data[word] = (inst->data[word] & ~mask) | (value << low);
}

#undef BRW_IA16_ADDR_IMM
#undef BRW_IA1_ADDR_IMM
#undef MD
#undef F8
#undef FF
#undef BOUNDS
#undef F
#undef FC

typedef struct {
   uint64_t data;
} brw_compact_inst;

/**
 * Fetch a set of contiguous bits from the compacted instruction.
 *
 * Bits indices range from 0..63.
 */
static inline unsigned
brw_compact_inst_bits(const brw_compact_inst *inst, unsigned high, unsigned low)
{
   const uint64_t mask = (1ull << (high - low + 1)) - 1;

   return (inst->data >> low) & mask;
}

/**
 * Set bits in the compacted instruction.
 *
 * Bits indices range from 0..63.
 */
static inline void
brw_compact_inst_set_bits(brw_compact_inst *inst, unsigned high, unsigned low,
                          uint64_t value)
{
   const uint64_t mask = ((1ull << (high - low + 1)) - 1) << low;

   /* Make sure the supplied value actually fits in the given bitfield. */
   assert((value & (mask >> low)) == value);

   inst->data = (inst->data & ~mask) | (value << low);
}

#define FC(name, high, low, assertions)                            \
static inline void                                                 \
brw_compact_inst_set_##name(const struct gen_device_info *devinfo, \
                            brw_compact_inst *inst, unsigned v)    \
{                                                                  \
   assert(assertions);                                             \
   (void) devinfo;                                                 \
   brw_compact_inst_set_bits(inst, high, low, v);                  \
}                                                                  \
static inline unsigned                                             \
brw_compact_inst_##name(const struct gen_device_info *devinfo,     \
                        const brw_compact_inst *inst)              \
{                                                                  \
   assert(assertions);                                             \
   (void) devinfo;                                                 \
   return brw_compact_inst_bits(inst, high, low);                  \
}

/* A simple macro for fields which stay in the same place on all generations. */
#define F(name, high, low) FC(name, high, low, true)

F(src1_reg_nr,      63, 56)
F(src0_reg_nr,      55, 48)
F(dst_reg_nr,       47, 40)
F(src1_index,       39, 35)
F(src0_index,       34, 30)
F(cmpt_control,     29, 29) /* Same location as brw_inst */
FC(flag_subreg_nr,  28, 28, devinfo->gen <= 6)
F(cond_modifier,    27, 24) /* Same location as brw_inst */
FC(acc_wr_control,  23, 23, devinfo->gen >= 6)
FC(mask_control_ex, 23, 23, devinfo->is_g4x || devinfo->gen == 5)
F(subreg_index,     22, 18)
F(datatype_index,   17, 13)
F(control_index,    12,  8)
F(debug_control,     7,  7)
F(hw_opcode,         6,  0) /* Same location as brw_inst */

/**
 * (Gen8+) Compacted three-source instructions:
 *  @{
 */
FC(3src_src2_reg_nr,    63, 57, devinfo->gen >= 8)
FC(3src_src1_reg_nr,    56, 50, devinfo->gen >= 8)
FC(3src_src0_reg_nr,    49, 43, devinfo->gen >= 8)
FC(3src_src2_subreg_nr, 42, 40, devinfo->gen >= 8)
FC(3src_src1_subreg_nr, 39, 37, devinfo->gen >= 8)
FC(3src_src0_subreg_nr, 36, 34, devinfo->gen >= 8)
FC(3src_src2_rep_ctrl,  33, 33, devinfo->gen >= 8)
FC(3src_src1_rep_ctrl,  32, 32, devinfo->gen >= 8)
FC(3src_saturate,       31, 31, devinfo->gen >= 8)
FC(3src_debug_control,  30, 30, devinfo->gen >= 8)
FC(3src_cmpt_control,   29, 29, devinfo->gen >= 8)
FC(3src_src0_rep_ctrl,  28, 28, devinfo->gen >= 8)
/* Reserved */
FC(3src_dst_reg_nr,     18, 12, devinfo->gen >= 8)
FC(3src_source_index,   11, 10, devinfo->gen >= 8)
FC(3src_control_index,   9,  8, devinfo->gen >= 8)
/* Bit 7 is Reserved (for future Opcode expansion) */
FC(3src_hw_opcode,       6,  0, devinfo->gen >= 8)
/** @} */

#undef F

#ifdef __cplusplus
}
#endif

#endif

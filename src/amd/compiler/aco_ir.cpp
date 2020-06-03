/*
 * Copyright © 2020 Valve Corporation
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
 *
 */
#include "aco_ir.h"

namespace aco {

bool can_use_opsel(chip_class chip, aco_opcode op, int idx, bool high)
{
   /* opsel is only GFX9+ */
   if ((high || idx == -1) && chip < GFX9)
      return false;

   switch (op) {
   case aco_opcode::v_div_fixup_f16:
   case aco_opcode::v_fma_f16:
   case aco_opcode::v_mad_f16:
   case aco_opcode::v_mad_u16:
   case aco_opcode::v_mad_i16:
   case aco_opcode::v_med3_f16:
   case aco_opcode::v_med3_i16:
   case aco_opcode::v_med3_u16:
   case aco_opcode::v_min3_f16:
   case aco_opcode::v_min3_i16:
   case aco_opcode::v_min3_u16:
   case aco_opcode::v_max3_f16:
   case aco_opcode::v_max3_i16:
   case aco_opcode::v_max3_u16:
   case aco_opcode::v_max_u16_e64:
   case aco_opcode::v_max_i16_e64:
   case aco_opcode::v_min_u16_e64:
   case aco_opcode::v_min_i16_e64:
   case aco_opcode::v_add_i16:
   case aco_opcode::v_sub_i16:
   case aco_opcode::v_add_u16_e64:
   case aco_opcode::v_sub_u16_e64:
   case aco_opcode::v_cvt_pknorm_i16_f16:
   case aco_opcode::v_cvt_pknorm_u16_f16:
   case aco_opcode::v_lshlrev_b16_e64:
   case aco_opcode::v_lshrrev_b16_e64:
   case aco_opcode::v_ashrrev_i16_e64:
   case aco_opcode::v_mul_lo_u16_e64:
      return true;
   case aco_opcode::v_pack_b32_f16:
      return idx != -1;
   case aco_opcode::v_mad_u32_u16:
   case aco_opcode::v_mad_i32_i16:
      return idx >= 0 && idx < 2;
   default:
      return false;
   }
}

}

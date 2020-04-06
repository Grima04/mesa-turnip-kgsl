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
#include "helpers.h"

using namespace aco;

BEGIN_TEST(to_hw_instr.swap_subdword)
   for (unsigned i = GFX8; i <= GFX9; i++) {
      if (!setup_cs(NULL, (chip_class)i))
         continue;

      PhysReg v0_lo{256};
      PhysReg v0_hi{256};
      PhysReg v0_b1{256};
      PhysReg v0_b3{256};
      PhysReg v1_lo{257};
      PhysReg v1_hi{257};
      PhysReg v1_b1{257};
      PhysReg v1_b3{257};
      v0_hi.reg_b += 2;
      v1_hi.reg_b += 2;
      v0_b1.reg_b += 1;
      v1_b1.reg_b += 1;
      v0_b3.reg_b += 3;
      v1_b3.reg_b += 3;

      //>> p_unit_test 0
      //~gfx8! v2b: %0:v[0][16:32] = v_xor_b32 %0:v[0][16:32], %0:v[0][0:16] dst_preserve
      //~gfx8! v2b: %0:v[0][0:16] = v_xor_b32 %0:v[0][16:32], %0:v[0][0:16] dst_preserve
      //~gfx8! v2b: %0:v[0][16:32] = v_xor_b32 %0:v[0][16:32], %0:v[0][0:16] dst_preserve
      //~gfx9! v1: %0:v[0] = v_pk_add_u16 %0:v[0].yx, 0
      bld.pseudo(aco_opcode::p_unit_test, Operand(0u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand(v0_hi, v2b), Operand(v0_lo, v2b));

      //! p_unit_test 1
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx9! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //! v2b: %0:v[1][16:32] = v_mov_b32 %0:v[0][16:32] dst_preserve
      bld.pseudo(aco_opcode::p_unit_test, Operand(1u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v1), Definition(v1_lo, v2b),
                 Operand(v1_lo, v1), Operand(v0_lo, v2b));

      //! p_unit_test 2
      //! v2b: %0:v[0][16:32] = v_mov_b32 %0:v[1][16:32] dst_preserve
      //! v2b: %0:v[1][16:32] = v_mov_b32 %0:v[0][0:16] dst_preserve
      //! v2b: %0:v[1][0:16] = v_xor_b32 %0:v[1][0:16], %0:v[0][0:16] dst_preserve
      //! v2b: %0:v[0][0:16] = v_xor_b32 %0:v[1][0:16], %0:v[0][0:16] dst_preserve
      //! v2b: %0:v[1][0:16] = v_xor_b32 %0:v[1][0:16], %0:v[0][0:16] dst_preserve
      bld.pseudo(aco_opcode::p_unit_test, Operand(2u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v1), Definition(v1_lo, v2b), Definition(v1_hi, v2b),
                 Operand(v1_lo, v1), Operand(v0_lo, v2b), Operand(v0_lo, v2b));

      //! p_unit_test 3
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx9! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //! v2b: %0:v[1][0:16] = v_mov_b32 %0:v[0][0:16] dst_preserve
      //! v1b: %0:v[1][16:24] = v_mov_b32 %0:v[0][16:24] dst_preserve
      bld.pseudo(aco_opcode::p_unit_test, Operand(3u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v1), Definition(v1_b3, v1b),
                 Operand(v1_lo, v1), Operand(v0_b3, v1b));

      //! p_unit_test 4
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx9! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //! v1b: %0:v[1][8:16] = v_mov_b32 %0:v[0][8:16] dst_preserve
      //! v2b: %0:v[1][16:32] = v_mov_b32 %0:v[0][16:32] dst_preserve
      bld.pseudo(aco_opcode::p_unit_test, Operand(4u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v1), Definition(v1_lo, v1b),
                 Operand(v1_lo, v1), Operand(v0_lo, v1b));

      //! p_unit_test 5
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx9! v1: %0:v[1],  v1: %0:v[0] = v_swap_b32 %0:v[0], %0:v[1]
      //! v1b: %0:v[0][8:16] = v_mov_b32 %0:v[1][8:16] dst_preserve
      //! v1b: %0:v[0][24:32] = v_mov_b32 %0:v[1][24:32] dst_preserve
      bld.pseudo(aco_opcode::p_unit_test, Operand(5u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v1b), Definition(v0_hi, v1b), Definition(v1_lo, v1),
                 Operand(v1_lo, v1b), Operand(v1_hi, v1b), Operand(v0_lo, v1));

      //! p_unit_test 6
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx9! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      bld.pseudo(aco_opcode::p_unit_test, Operand(6u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v2b), Definition(v0_hi, v2b), Definition(v1_lo, v1),
                 Operand(v1_lo, v2b), Operand(v1_hi, v2b), Operand(v0_lo, v1));

      //! p_unit_test 7
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v2b: %0:v[0][16:32] = v_xor_b32 %0:v[0][16:32], %0:v[0][0:16] dst_preserve
      //~gfx8! v2b: %0:v[0][0:16] = v_xor_b32 %0:v[0][16:32], %0:v[0][0:16] dst_preserve
      //~gfx8! v2b: %0:v[0][16:32] = v_xor_b32 %0:v[0][16:32], %0:v[0][0:16] dst_preserve
      //~gfx9! v1: %0:v[1],  v1: %0:v[0] = v_swap_b32 %0:v[0], %0:v[1]
      //~gfx9! v1: %0:v[0] = v_pk_add_u16 %0:v[0].yx, 0
      bld.pseudo(aco_opcode::p_unit_test, Operand(7u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v2b), Definition(v0_hi, v2b), Definition(v1_lo, v1),
                 Operand(v1_hi, v2b), Operand(v1_lo, v2b), Operand(v0_lo, v1));

      //! p_unit_test 8
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx9! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //! v1b: %0:v[1][24:32] = v_xor_b32 %0:v[1][24:32], %0:v[0][24:32] dst_preserve
      //! v1b: %0:v[0][24:32] = v_xor_b32 %0:v[1][24:32], %0:v[0][24:32] dst_preserve
      //! v1b: %0:v[1][24:32] = v_xor_b32 %0:v[1][24:32], %0:v[0][24:32] dst_preserve
      bld.pseudo(aco_opcode::p_unit_test, Operand(8u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v3b), Definition(v1_lo, v3b),
                 Operand(v1_lo, v3b), Operand(v0_lo, v3b));

      //! p_unit_test 9
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx9! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //! v1b: %0:v[1][24:32] = v_mov_b32 %0:v[0][24:32] dst_preserve
      bld.pseudo(aco_opcode::p_unit_test, Operand(9u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v3b), Definition(v1_lo, v3b), Definition(v0_b3, v1b),
                 Operand(v1_lo, v3b), Operand(v0_lo, v3b), Operand(v1_b3, v1b));

      //! p_unit_test 10
      //! v1b: %0:v[1][8:16] = v_xor_b32 %0:v[1][8:16], %0:v[0][8:16] dst_preserve
      //! v1b: %0:v[0][8:16] = v_xor_b32 %0:v[1][8:16], %0:v[0][8:16] dst_preserve
      //! v1b: %0:v[1][8:16] = v_xor_b32 %0:v[1][8:16], %0:v[0][8:16] dst_preserve
      //! v1b: %0:v[1][16:24] = v_xor_b32 %0:v[1][16:24], %0:v[0][16:24] dst_preserve
      //! v1b: %0:v[0][16:24] = v_xor_b32 %0:v[1][16:24], %0:v[0][16:24] dst_preserve
      //! v1b: %0:v[1][16:24] = v_xor_b32 %0:v[1][16:24], %0:v[0][16:24] dst_preserve
      bld.pseudo(aco_opcode::p_unit_test, Operand(10u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_b1, v2b), Definition(v1_b1, v2b),
                 Operand(v1_b1, v2b), Operand(v0_b1, v2b));

      //! p_unit_test 11
      //! v2b: %0:v[1][0:16] = v_mov_b32 %0:v[0][16:32] dst_preserve
      //! v1: %0:v[0] = v_mov_b32 42
      bld.pseudo(aco_opcode::p_unit_test, Operand(11u));
      bld.pseudo(aco_opcode::p_parallelcopy,
                 Definition(v0_lo, v1), Definition(v1_lo, v2b),
                 Operand(42u), Operand(v0_hi, v2b));

      //! s_endpgm

      finish_to_hw_instr_test();
   }
END_TEST

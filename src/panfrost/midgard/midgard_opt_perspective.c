/*
 * Copyright (C) 2019 Collabora, Ltd.
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

/* Midgard has some accelerated support for perspective projection on the
 * load/store pipes. So the first perspective projection pass looks for
 * lowered/open-coded perspective projection of the form "fmul (A.xyz,
 * frcp(A.w))" or "fmul (A.xy, frcp(A.z))" and rewrite with a native
 * perspective division opcode (on the load/store pipe).
 *
 * Caveats apply: the frcp should be used only once to make this optimization
 * worthwhile.
 */

#include "compiler.h"

bool
midgard_opt_combine_projection(compiler_context *ctx, midgard_block *block)
{
        bool progress = false;

        mir_foreach_instr_in_block_safe(block, ins) {
                /* First search for fmul */
                if (ins->type != TAG_ALU_4) continue;
                if (ins->alu.op != midgard_alu_op_fmul) continue;

                /* TODO: Flip */

                /* Check the swizzles */
                
                midgard_vector_alu_src src1 =
                        vector_alu_from_unsigned(ins->alu.src1);

                midgard_vector_alu_src src2 =
                        vector_alu_from_unsigned(ins->alu.src2);

                if (!mir_is_simple_swizzle(src1.swizzle, ins->mask)) continue;
                if (src2.swizzle != SWIZZLE_XXXX) continue;

                /* Awesome, we're the right form. Now check where src2 is from */
                unsigned frcp = ins->ssa_args.src1;
                unsigned to = ins->ssa_args.dest;

                if (frcp >= ctx->func->impl->ssa_alloc) continue;
                if (to >= ctx->func->impl->ssa_alloc) continue;

                bool frcp_found = false;
                unsigned frcp_component = 0;
                unsigned frcp_from = 0;

                mir_foreach_instr_in_block_safe(block, sub) {
                        if (sub->ssa_args.dest != frcp) continue;

                        midgard_vector_alu_src s =
                                vector_alu_from_unsigned(sub->alu.src1);

                        frcp_component = s.swizzle & 3;
                        frcp_from = sub->ssa_args.src0;

                        frcp_found =
                                (sub->type == TAG_ALU_4) &&
                                (sub->alu.op == midgard_alu_op_frcp);
                        break;
                }

                if (!frcp_found) continue;
                if (frcp_component != COMPONENT_W && frcp_component != COMPONENT_Z) continue;
                if (!mir_single_use(ctx, frcp)) continue;

                /* Nice, we got the form spot on. Let's convert! */

                midgard_instruction accel = {
                        .type = TAG_LOAD_STORE_4,
                        .mask = ins->mask,
                        .ssa_args = {
                                .dest = to,
                                .src0 = frcp_from,
                                .src1 = -1
                        },
                        .load_store = {
                                .op = frcp_component == COMPONENT_W ?
                                        midgard_op_ldst_perspective_division_w : 
                                        midgard_op_ldst_perspective_division_z,
                                .swizzle = SWIZZLE_XYZW,
                                .unknown = 0x24,
                        }
                };

                mir_insert_instruction_before(ins, accel);
                mir_remove_instruction(ins);

                progress |= true;
        }

        return progress;
}

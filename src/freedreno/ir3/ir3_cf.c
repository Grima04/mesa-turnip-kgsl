/*
 * Copyright (C) 2019 Google.
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

#include "ir3.h"

static bool
is_fp16_conv(struct ir3_instruction *instr)
{
	if (instr->opc == OPC_MOV &&
			instr->cat1.src_type == TYPE_F32 &&
			instr->cat1.dst_type == TYPE_F16)
		return true;

	return false;
}

static bool
all_uses_fp16_conv(struct ir3 *ir, struct ir3_instruction *conv_src)
{
	foreach_block (block, &ir->block_list) {
		foreach_instr (instr, &block->instr_list) {
			struct ir3_instruction *src;
			foreach_ssa_src (src, instr) {
				if (src == conv_src && !is_fp16_conv(instr))
					return false;
			}
		}
	}

	return true;
}

static void
rewrite_uses(struct ir3 *ir, struct ir3_instruction *conv,
			 struct ir3_instruction *replace)
{
	foreach_block (block, &ir->block_list) {
		foreach_instr (instr, &block->instr_list) {
			struct ir3_instruction *src;
			foreach_ssa_src_n (src, n, instr) {
				if (src == conv)
					instr->regs[n]->instr = replace;
			}
		}
	}
}

static void
try_conversion_folding(struct ir3 *ir, struct ir3_instruction *conv)
{
	struct ir3_instruction *src;

	if (!is_fp16_conv(conv))
		return;

	src = ssa(conv->regs[1]);
	if (!is_alu(src))
		return;

	switch (src->opc) {
	case OPC_SEL_B32:
	case OPC_MAX_F:
	case OPC_MIN_F:
	case OPC_SIGN_F:
	case OPC_ABSNEG_F:
		return;
	default:
		break;
	}

	if (!all_uses_fp16_conv(ir, src))
		return;

	if (src->opc == OPC_MOV) {
		if (src->cat1.dst_type == src->cat1.src_type) {
			/* If we're folding a conversion into a bitwise move, we need to
			 * change the dst type to F32 to get the right behavior, since we
			 * could be moving a float with a u32.u32 move.
			 */
			src->cat1.dst_type = TYPE_F16;
			src->cat1.src_type = TYPE_F32;
		} else {
			/* Otherwise, for typechanging movs, we can just change the dst
			 * type to F16 to collaps the two conversions.  For example
			 * cov.s32f32 follwed by cov.f32f16 becomes cov.s32f16.
			 */
			src->cat1.dst_type = TYPE_F16;
		}
	}

	src->regs[0]->flags |= IR3_REG_HALF;

	rewrite_uses(ir, conv, src);
}

void
ir3_cf(struct ir3 *ir)
{
	foreach_block_safe (block, &ir->block_list) {
		foreach_instr_safe (instr, &block->instr_list) {
			try_conversion_folding(ir, instr);
		}
	}
}

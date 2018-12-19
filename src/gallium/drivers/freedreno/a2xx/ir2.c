/*
 * Copyright (C) 2018 Jonathan Marek <jonathan@marek.ca>
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
 * Authors:
 *    Jonathan Marek <jonathan@marek.ca>
 */

#include "ir2_private.h"

static bool scalar_possible(struct ir2_instr *instr)
{
	if (instr->alu.scalar_opc == SCALAR_NONE)
		return false;

	return src_ncomp(instr) == 1;
}

static bool is_alu_compatible(struct ir2_instr *a, struct ir2_instr *b)
{
	if (!a)
		return true;

	/* dont use same instruction twice */
	if (a == b)
		return false;

	/* PRED_SET must be alone */
	if (b->alu.scalar_opc >= PRED_SETEs &&
		b->alu.scalar_opc <= PRED_SET_RESTOREs)
		return false;

	/* must write to same export (issues otherwise?) */
	return a->alu.export == b->alu.export;
}

/* priority of vector instruction for scheduling (lower=higher prio) */
static unsigned alu_vector_prio(struct ir2_instr *instr)
{
	if (instr->alu.vector_opc == VECTOR_NONE)
		return ~0u;

	if (is_export(instr))
		return 4;

	/* TODO check src type and ncomps */
	if (instr->src_count == 3)
		return 0;

	if (!scalar_possible(instr))
		return 1;

	return instr->src_count == 2 ? 2 : 3;
}

/* priority of scalar instruction for scheduling (lower=higher prio) */
static unsigned alu_scalar_prio(struct ir2_instr *instr)
{
	if (!scalar_possible(instr))
		return ~0u;

	/* this case is dealt with later */
	if (instr->src_count > 1)
		return ~0u;

	if (is_export(instr))
		return 4;

	/* PRED to end of block */
	if (instr->alu.scalar_opc >= PRED_SETEs &&
		instr->alu.scalar_opc <= PRED_SET_RESTOREs)
		return 5;

	/* scalar only have highest priority */
	return instr->alu.vector_opc == VECTOR_NONE ? 0 : 3;
}

/* fill sched with next fetch or (vector and/or scalar) alu instruction */
static int sched_next(struct ir2_context *ctx, struct ir2_sched_instr *sched)
{
	struct ir2_instr *avail[0x100], *instr_v = NULL, *instr_s = NULL;
	unsigned avail_count = 0;

	instr_alloc_type_t export = ~0u;
	int block_idx = -1;

	/* XXX merge this loop with the other one somehow? */
	ir2_foreach_instr(instr, ctx) {
		if (!instr->need_emit)
			continue;
		if (is_export(instr))
			export = MIN2(export, export_buf(instr->alu.export));
	}

	ir2_foreach_instr(instr, ctx) {
		if (!instr->need_emit)
			continue;

		/* dont mix exports */
		if (is_export(instr) && export_buf(instr->alu.export) != export)
			continue;

		if (block_idx < 0)
			block_idx = instr->block_idx;
		else if (block_idx != instr->block_idx || /* must be same block */
			instr->type == IR2_CF || /* CF/MEM must be alone */
			(is_export(instr) && export == SQ_MEMORY))
			break;
		/* it works because IR2_CF is always at end of block
		 * and somewhat same idea with MEM exports, which might not be alone
		 * but will end up in-order at least
		 */

		/* check if dependencies are satisfied */
		bool is_ok = true;
		ir2_foreach_src(src, instr) {
			if (src->type == IR2_SRC_REG) {
				/* need to check if all previous instructions in the block
				 * which write the reg have been emitted
				 * slow..
				 * XXX: check components instead of whole register
				 */
				struct ir2_reg *reg = get_reg_src(ctx, src);
				ir2_foreach_instr(p, ctx) {
					if (!p->is_ssa && p->reg == reg && p->idx < instr->idx)
						is_ok &= !p->need_emit;
				}
			} else if (src->type == IR2_SRC_SSA) {
				/* in this case its easy, just check need_emit */
				is_ok &= !ctx->instr[src->num].need_emit;
			}
		}
		if (!is_ok)
			continue;

		avail[avail_count++] = instr;
	}

	if (!avail_count) {
		assert(block_idx == -1);
		return -1;
	}

	/* priority to FETCH instructions */
	ir2_foreach_avail(instr) {
		if (instr->type == IR2_ALU)
			continue;

		ra_src_free(ctx, instr);
		ra_reg(ctx, get_reg(instr), -1, false, 0);

		instr->need_emit = false;
		sched->instr = instr;
		sched->instr_s = NULL;
		return block_idx;
	}

	/* TODO precompute priorities */

	unsigned prio_v = ~0u, prio_s = ~0u, prio;
	ir2_foreach_avail(instr) {
		prio = alu_vector_prio(instr);
		if (prio < prio_v) {
			instr_v = instr;
			prio_v = prio;
		}
	}

	/* TODO can still insert scalar if src_count=3, if smart about it */
	if (!instr_v || instr_v->src_count < 3) {
		ir2_foreach_avail(instr) {
			bool compat = is_alu_compatible(instr_v, instr);

			prio = alu_scalar_prio(instr);
			if (prio >= prio_v && !compat)
				continue;

			if (prio < prio_s) {
				instr_s = instr;
				prio_s = prio;
				if (!compat)
					instr_v = NULL;
			}
		}
	}

	assert(instr_v || instr_s);

	/* free src registers */
	if (instr_v) {
		instr_v->need_emit = false;
		ra_src_free(ctx, instr_v);
	}

	if (instr_s) {
		instr_s->need_emit = false;
		ra_src_free(ctx, instr_s);
	}

	/* allocate dst registers */
	if (instr_v)
		ra_reg(ctx, get_reg(instr_v), -1, is_export(instr_v), instr_v->alu.write_mask);

	if (instr_s)
		ra_reg(ctx, get_reg(instr_s), -1, is_export(instr_s), instr_s->alu.write_mask);

	sched->instr = instr_v;
	sched->instr_s = instr_s;
	return block_idx;
}

/* scheduling: determine order of instructions */
static void schedule_instrs(struct ir2_context *ctx)
{
	struct ir2_sched_instr *sched;
	int block_idx;

	/* allocate input registers */
	for (unsigned idx = 0; idx < ARRAY_SIZE(ctx->input); idx++)
		if (ctx->input[idx].initialized)
			ra_reg(ctx, &ctx->input[idx], idx, false, 0);

	for (;;) {
		sched = &ctx->instr_sched[ctx->instr_sched_count++];
		block_idx = sched_next(ctx, sched);
		if (block_idx < 0)
			break;
		memcpy(sched->reg_state, ctx->reg_state, sizeof(ctx->reg_state));

		/* catch texture fetch after scheduling and insert the
		 * SET_TEX_LOD right before it if necessary
		 * TODO clean this up
		 */
		struct ir2_instr *instr = sched->instr, *tex_lod;
		if (instr && instr->type == IR2_FETCH &&
			instr->fetch.opc == TEX_FETCH && instr->src_count == 2) {
			/* generate the SET_LOD instruction */
			tex_lod = &ctx->instr[ctx->instr_count++];
			tex_lod->type = IR2_FETCH;
			tex_lod->block_idx = instr->block_idx;
			tex_lod->pred = instr->pred;
			tex_lod->fetch.opc = TEX_SET_TEX_LOD;
			tex_lod->src[0] = instr->src[1];
			tex_lod->src_count = 1;

			sched[1] = sched[0];
			sched->instr = tex_lod;
			ctx->instr_sched_count++;
		}

		bool free_block = true;
		ir2_foreach_instr(instr, ctx)
			free_block &= instr->block_idx != block_idx;
		if (free_block)
			ra_block_free(ctx, block_idx);
	};
	ctx->instr_sched_count--;
}

void
ir2_compile(struct fd2_shader_stateobj *so, unsigned variant,
		struct fd2_shader_stateobj *fp)
{
	struct ir2_context ctx = { };
	bool binning = !fp && so->type == MESA_SHADER_VERTEX;

	if (fp)
		so->variant[variant].f = fp->variant[0].f;

	ctx.so = so;
	ctx.info = &so->variant[variant].info;
	ctx.f = &so->variant[variant].f;
	ctx.info->max_reg = -1;

	/* convert nir to internal representation */
	ir2_nir_compile(&ctx, binning);

	/* get ref_counts and kill non-needed instructions */
	ra_count_refs(&ctx);

	/* instruction order.. and vector->scalar conversions */
	schedule_instrs(&ctx);

	/* finally, assemble to bitcode */
	assemble(&ctx, binning);
}

/*
 * Copyright (C) 2014 Rob Clark <robclark@freedesktop.org>
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
 *    Rob Clark <robclark@freedesktop.org>
 */

#ifndef IR3_RA_H_
#define IR3_RA_H_

//#include "util/u_math.h"
//#include "util/register_allocate.h"
//#include "util/ralloc.h"
#include "util/bitset.h"

//#include "ir3.h"
//#include "ir3_compiler.h"


static const unsigned class_sizes[] = {
	1, 2, 3, 4,
	4 + 4, /* txd + 1d/2d */
	4 + 6, /* txd + 3d */
};
#define class_count ARRAY_SIZE(class_sizes)

static const unsigned half_class_sizes[] = {
	1, 2, 3, 4,
};
#define half_class_count  ARRAY_SIZE(half_class_sizes)

/* seems to just be used for compute shaders?  Seems like vec1 and vec3
 * are sufficient (for now?)
 */
static const unsigned high_class_sizes[] = {
	1, 3,
};
#define high_class_count ARRAY_SIZE(high_class_sizes)

#define total_class_count (class_count + half_class_count + high_class_count)

/* Below a0.x are normal regs.  RA doesn't need to assign a0.x/p0.x. */
#define NUM_REGS             (4 * 48)  /* r0 to r47 */
#define NUM_HIGH_REGS        (4 * 8)   /* r48 to r55 */
#define FIRST_HIGH_REG       (4 * 48)
/* Number of virtual regs in a given class: */
#define CLASS_REGS(i)        (NUM_REGS - (class_sizes[i] - 1))
#define HALF_CLASS_REGS(i)   (NUM_REGS - (half_class_sizes[i] - 1))
#define HIGH_CLASS_REGS(i)   (NUM_HIGH_REGS - (high_class_sizes[i] - 1))

#define HALF_OFFSET          (class_count)
#define HIGH_OFFSET          (class_count + half_class_count)

/* register-set, created one time, used for all shaders: */
struct ir3_ra_reg_set {
	struct ra_regs *regs;
	unsigned int classes[class_count];
	unsigned int half_classes[half_class_count];
	unsigned int high_classes[high_class_count];
	/* maps flat virtual register space to base gpr: */
	uint16_t *ra_reg_to_gpr;
	/* maps cls,gpr to flat virtual register space: */
	uint16_t **gpr_to_ra_reg;
};

/* additional block-data (per-block) */
struct ir3_ra_block_data {
	BITSET_WORD *def;        /* variables defined before used in block */
	BITSET_WORD *use;        /* variables used before defined in block */
	BITSET_WORD *livein;     /* which defs reach entry point of block */
	BITSET_WORD *liveout;    /* which defs reach exit point of block */
};

/* additional instruction-data (per-instruction) */
struct ir3_ra_instr_data {
	/* cached instruction 'definer' info: */
	struct ir3_instruction *defn;
	int off, sz, cls;
};

/* register-assign context, per-shader */
struct ir3_ra_ctx {
	struct ir3_shader_variant *v;
	struct ir3 *ir;

	struct ir3_ra_reg_set *set;
	struct ra_graph *g;

	/* Are we in the scalar assignment pass?  In this pass, all larger-
	 * than-vec1 vales have already been assigned and pre-colored, so
	 * we only consider scalar values.
	 */
	bool scalar_pass;

	unsigned alloc_count;
	/* one per class, plus one slot for arrays: */
	unsigned class_alloc_count[total_class_count + 1];
	unsigned class_base[total_class_count + 1];
	unsigned instr_cnt;
	unsigned *def, *use;     /* def/use table */
	struct ir3_ra_instr_data *instrd;

	/* Mapping vreg name back to instruction, used select reg callback: */
	struct hash_table *name_to_instr;

	/* Tracking for max half/full register assigned.  We don't need to
	 * track high registers.
	 *
	 * The feedback about registers used in first pass is used to choose
	 * a target register usage to round-robin between in the 2nd pass.
	 */
	unsigned max_assigned;
	unsigned max_half_assigned;

	/* Tracking for select_reg callback */
	unsigned start_search_reg;
	unsigned max_target;
};

static inline int
ra_name(struct ir3_ra_ctx *ctx, struct ir3_ra_instr_data *id)
{
	unsigned name;
	debug_assert(id->cls >= 0);
	debug_assert(id->cls < total_class_count);  /* we shouldn't get arrays here.. */
	name = ctx->class_base[id->cls] + id->defn->name;
	debug_assert(name < ctx->alloc_count);
	return name;
}

/* Get the scalar name of the n'th component of an instruction dst: */
static inline int
scalar_name(struct ir3_ra_ctx *ctx, struct ir3_instruction *instr, unsigned n)
{
	if (ctx->scalar_pass) {
		if (instr->opc == OPC_META_SPLIT) {
			debug_assert(n == 0);     /* split results in a scalar */
			struct ir3_instruction *src = instr->regs[1]->instr;
			return scalar_name(ctx, src, instr->split.off);
		} else if (instr->opc == OPC_META_COLLECT) {
			debug_assert(n < (instr->regs_count + 1));
			struct ir3_instruction *src = instr->regs[n + 1]->instr;
			return scalar_name(ctx, src, 0);
		}
	} else {
		debug_assert(n == 0);
	}

	return ra_name(ctx, &ctx->instrd[instr->ip]) + n;
}

static inline bool
writes_gpr(struct ir3_instruction *instr)
{
	if (dest_regs(instr) == 0)
		return false;
	/* is dest a normal temp register: */
	struct ir3_register *reg = instr->regs[0];
	debug_assert(!(reg->flags & (IR3_REG_CONST | IR3_REG_IMMED)));
	if ((reg->num == regid(REG_A0, 0)) ||
			(reg->num == regid(REG_P0, 0)))
		return false;
	return true;
}

int ra_size_to_class(unsigned sz, bool half, bool high);

#endif  /* IR3_RA_H_ */

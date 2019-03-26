/*
 * Copyright © 2019 Google, Inc.
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

#include "ir3_nir.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_dynarray.h"
#include "mesa/main/macros.h"

struct ir3_ubo_analysis_state {
	unsigned lower_count;
};

static void
lower_ubo_load_to_uniform(nir_intrinsic_instr *instr, nir_builder *b,
						  struct ir3_ubo_analysis_state *state)
{
	/* We don't lower dynamic block index UBO loads to load_uniform, but we
	 * could probably with some effort determine a block stride in number of
	 * registers.
	 */
	if (!nir_src_is_const(instr->src[0]))
		return;

	const uint32_t block = nir_src_as_uint(instr->src[0]);
	if (block > 0)
		return;

	b->cursor = nir_before_instr(&instr->instr);

	nir_ssa_def *ubo_offset = nir_ssa_for_src(b, instr->src[1], 1);
	nir_ssa_def *uniform_offset = ir3_nir_try_propagate_bit_shift(b, ubo_offset, -2);
	if (uniform_offset == NULL)
		uniform_offset = nir_ushr(b, ubo_offset, nir_imm_int(b, 2));

	nir_intrinsic_instr *uniform =
		nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_uniform);
	uniform->num_components = instr->num_components;
	uniform->src[0] = nir_src_for_ssa(uniform_offset);
	nir_ssa_dest_init(&uniform->instr, &uniform->dest,
					  uniform->num_components, instr->dest.ssa.bit_size,
					  instr->dest.ssa.name);
	nir_builder_instr_insert(b, &uniform->instr);
	nir_ssa_def_rewrite_uses(&instr->dest.ssa,
							 nir_src_for_ssa(&uniform->dest.ssa));

	nir_instr_remove(&instr->instr);

	state->lower_count++;
}

bool
ir3_nir_analyze_ubo_ranges(nir_shader *nir, struct ir3_shader *shader)
{
	struct ir3_ubo_analysis_state state = { 0 };

	nir_foreach_function(function, nir) {
		if (function->impl) {
			nir_builder builder;
			nir_builder_init(&builder, function->impl);
			nir_foreach_block(block, function->impl) {
				nir_foreach_instr_safe(instr, block) {
					if (instr->type == nir_instr_type_intrinsic &&
						nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_ubo)
						lower_ubo_load_to_uniform(nir_instr_as_intrinsic(instr), &builder, &state);
				}
			}

			nir_metadata_preserve(function->impl, nir_metadata_block_index |
								  nir_metadata_dominance);
		}
	}

	return state.lower_count > 0;
}

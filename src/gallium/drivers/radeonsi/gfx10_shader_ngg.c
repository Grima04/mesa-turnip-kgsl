/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "si_pipe.h"
#include "si_shader_internal.h"

#include "sid.h"

#include "util/u_memory.h"

static LLVMValueRef get_wave_id_in_tg(struct si_shader_context *ctx)
{
	return si_unpack_param(ctx, ctx->param_merged_wave_info, 24, 4);
}

static LLVMValueRef ngg_get_vtx_cnt(struct si_shader_context *ctx)
{
	return ac_build_bfe(&ctx->ac, ctx->gs_tg_info,
			    LLVMConstInt(ctx->ac.i32, 12, false),
			    LLVMConstInt(ctx->ac.i32, 9, false),
			    false);
}

static LLVMValueRef ngg_get_prim_cnt(struct si_shader_context *ctx)
{
	return ac_build_bfe(&ctx->ac, ctx->gs_tg_info,
			    LLVMConstInt(ctx->ac.i32, 22, false),
			    LLVMConstInt(ctx->ac.i32, 9, false),
			    false);
}

/* Send GS Alloc Req message from the first wave of the group to SPI.
 * Message payload is:
 * - bits 0..10: vertices in group
 * - bits 12..22: primitives in group
 */
static void build_sendmsg_gs_alloc_req(struct si_shader_context *ctx,
				       LLVMValueRef vtx_cnt,
				       LLVMValueRef prim_cnt)
{
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef tmp;

	tmp = LLVMBuildICmp(builder, LLVMIntEQ, get_wave_id_in_tg(ctx), ctx->ac.i32_0, "");
	ac_build_ifcc(&ctx->ac, tmp, 5020);

	tmp = LLVMBuildShl(builder, prim_cnt, LLVMConstInt(ctx->ac.i32, 12, false),"");
	tmp = LLVMBuildOr(builder, tmp, vtx_cnt, "");
	ac_build_sendmsg(&ctx->ac, AC_SENDMSG_GS_ALLOC_REQ, tmp);

	ac_build_endif(&ctx->ac, 5020);
}

struct ngg_prim {
	unsigned num_vertices;
	LLVMValueRef isnull;
	LLVMValueRef index[3];
	LLVMValueRef edgeflag[3];
};

static void build_export_prim(struct si_shader_context *ctx,
			      const struct ngg_prim *prim)
{
	LLVMBuilderRef builder = ctx->ac.builder;
	struct ac_export_args args;
	LLVMValueRef tmp;

	tmp = LLVMBuildZExt(builder, prim->isnull, ctx->ac.i32, "");
	args.out[0] = LLVMBuildShl(builder, tmp, LLVMConstInt(ctx->ac.i32, 31, false), "");

	for (unsigned i = 0; i < prim->num_vertices; ++i) {
		tmp = LLVMBuildShl(builder, prim->index[i],
				   LLVMConstInt(ctx->ac.i32, 10 * i, false), "");
		args.out[0] = LLVMBuildOr(builder, args.out[0], tmp, "");
		tmp = LLVMBuildZExt(builder, prim->edgeflag[i], ctx->ac.i32, "");
		tmp = LLVMBuildShl(builder, tmp,
				   LLVMConstInt(ctx->ac.i32, 10 * i + 9, false), "");
		args.out[0] = LLVMBuildOr(builder, args.out[0], tmp, "");
	}

	args.out[0] = LLVMBuildBitCast(builder, args.out[0], ctx->ac.f32, "");
	args.out[1] = LLVMGetUndef(ctx->ac.f32);
	args.out[2] = LLVMGetUndef(ctx->ac.f32);
	args.out[3] = LLVMGetUndef(ctx->ac.f32);

	args.target = V_008DFC_SQ_EXP_PRIM;
	args.enabled_channels = 1;
	args.done = true;
	args.valid_mask = false;
	args.compr = false;

	ac_build_export(&ctx->ac, &args);
}

/**
 * Emit the epilogue of an API VS or TES shader compiled as ESGS shader.
 */
void gfx10_emit_ngg_epilogue(struct ac_shader_abi *abi,
			     unsigned max_outputs,
			     LLVMValueRef *addrs)
{
	struct si_shader_context *ctx = si_shader_context_from_abi(abi);
	struct tgsi_shader_info *info = &ctx->shader->selector->info;
	struct si_shader_output_values *outputs = NULL;
	LLVMBuilderRef builder = ctx->ac.builder;
	struct lp_build_if_state if_state;
	LLVMValueRef tmp;

	assert(!ctx->shader->is_gs_copy_shader);
	assert(info->num_outputs <= max_outputs);

	outputs = MALLOC((info->num_outputs + 1) * sizeof(outputs[0]));

	for (unsigned i = 0; i < info->num_outputs; i++) {
		outputs[i].semantic_name = info->output_semantic_name[i];
		outputs[i].semantic_index = info->output_semantic_index[i];

		/* This is used only by streamout. */
		for (unsigned j = 0; j < 4; j++) {
			outputs[i].values[j] =
				LLVMBuildLoad(builder,
					      addrs[4 * i + j],
					      "");
			outputs[i].vertex_stream[j] =
				(info->output_streams[i] >> (2 * j)) & 3;
		}
	}

	lp_build_endif(&ctx->merged_wrap_if_state);

	LLVMValueRef prims_in_wave = si_unpack_param(ctx, ctx->param_merged_wave_info, 8, 8);
	LLVMValueRef vtx_in_wave = si_unpack_param(ctx, ctx->param_merged_wave_info, 0, 8);
	LLVMValueRef is_gs_thread = LLVMBuildICmp(builder, LLVMIntULT,
						  ac_get_thread_id(&ctx->ac), prims_in_wave, "");
	LLVMValueRef is_es_thread = LLVMBuildICmp(builder, LLVMIntULT,
						  ac_get_thread_id(&ctx->ac), vtx_in_wave, "");
	LLVMValueRef vtxindex[] = {
		si_unpack_param(ctx, ctx->param_gs_vtx01_offset, 0, 16),
		si_unpack_param(ctx, ctx->param_gs_vtx01_offset, 16, 16),
		si_unpack_param(ctx, ctx->param_gs_vtx23_offset, 0, 16),
	};

	/* Determine the number of vertices per primitive. */
	unsigned num_vertices;
	LLVMValueRef num_vertices_val;

	if (ctx->type == PIPE_SHADER_VERTEX) {
		if (info->properties[TGSI_PROPERTY_VS_BLIT_SGPRS]) {
			/* Blits always use axis-aligned rectangles with 3 vertices. */
			num_vertices = 3;
			num_vertices_val = LLVMConstInt(ctx->i32, 3, 0);
		} else {
			/* Extract OUTPRIM field. */
			tmp = si_unpack_param(ctx, ctx->param_vs_state_bits, 2, 2);
			num_vertices_val = LLVMBuildAdd(builder, tmp, ctx->i32_1, "");
			num_vertices = 3; /* TODO: optimize for points & lines */
		}
	} else {
		assert(ctx->type == PIPE_SHADER_TESS_EVAL);

		if (info->properties[TGSI_PROPERTY_TES_POINT_MODE])
			num_vertices = 1;
		else if (info->properties[TGSI_PROPERTY_TES_PRIM_MODE] == PIPE_PRIM_LINES)
			num_vertices = 2;
		else
			num_vertices = 3;

		num_vertices_val = LLVMConstInt(ctx->i32, num_vertices, false);
	}

	/* TODO: streamout */

	/* TODO: primitive culling */

	build_sendmsg_gs_alloc_req(ctx, ngg_get_vtx_cnt(ctx), ngg_get_prim_cnt(ctx));

	/* Export primitive data to the index buffer. Format is:
	 *  - bits 0..8: index 0
	 *  - bit 9: edge flag 0
	 *  - bits 10..18: index 1
	 *  - bit 19: edge flag 1
	 *  - bits 20..28: index 2
	 *  - bit 29: edge flag 2
	 *  - bit 31: null primitive (skip)
	 *
	 * For the first version, we will always build up all three indices
	 * independent of the primitive type. The additional garbage data
	 * shouldn't hurt.
	 *
	 * TODO: culling depends on the primitive type, so can have some
	 * interaction here.
	 */
	lp_build_if(&if_state, &ctx->gallivm, is_gs_thread);
	{
		struct ngg_prim prim = {};

		prim.num_vertices = num_vertices;
		prim.isnull = ctx->ac.i1false;
		memcpy(prim.index, vtxindex, sizeof(vtxindex[0]) * 3);

		for (unsigned i = 0; i < num_vertices; ++i) {
			tmp = LLVMBuildLShr(builder, ctx->abi.gs_invocation_id,
					    LLVMConstInt(ctx->ac.i32, 8 + i, false), "");
			prim.edgeflag[i] = LLVMBuildTrunc(builder, tmp, ctx->ac.i1, "");
		}

		build_export_prim(ctx, &prim);
	}
	lp_build_endif(&if_state);

	/* Export per-vertex data (positions and parameters). */
	lp_build_if(&if_state, &ctx->gallivm, is_es_thread);
	{
		unsigned i;

		/* Unconditionally (re-)load the values for proper SSA form. */
		for (i = 0; i < info->num_outputs; i++) {
			for (unsigned j = 0; j < 4; j++) {
				outputs[i].values[j] =
					LLVMBuildLoad(builder,
						addrs[4 * i + j],
						"");
			}
		}

		/* TODO: Vertex shaders have to get PrimitiveID from GS VGPRs. */
		if (ctx->type == PIPE_SHADER_TESS_EVAL &&
		    ctx->shader->key.mono.u.vs_export_prim_id) {
			outputs[i].semantic_name = TGSI_SEMANTIC_PRIMID;
			outputs[i].semantic_index = 0;
			outputs[i].values[0] = ac_to_float(&ctx->ac, si_get_primitive_id(ctx, 0));
			for (unsigned j = 1; j < 4; j++)
				outputs[i].values[j] = LLVMGetUndef(ctx->f32);

			memset(outputs[i].vertex_stream, 0,
			       sizeof(outputs[i].vertex_stream));
			i++;
		}

		si_llvm_export_vs(ctx, outputs, i);
	}
	lp_build_endif(&if_state);

	FREE(outputs);
}

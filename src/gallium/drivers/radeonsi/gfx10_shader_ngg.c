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
#include "util/u_prim.h"

static LLVMValueRef get_wave_id_in_tg(struct si_shader_context *ctx)
{
	return si_unpack_param(ctx, ctx->param_merged_wave_info, 24, 4);
}

static LLVMValueRef get_tgsize(struct si_shader_context *ctx)
{
	return si_unpack_param(ctx, ctx->param_merged_wave_info, 28, 4);
}

static LLVMValueRef get_thread_id_in_tg(struct si_shader_context *ctx)
{
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef tmp;
	tmp = LLVMBuildMul(builder, get_wave_id_in_tg(ctx),
			   LLVMConstInt(ctx->ac.i32, 64, false), "");
	return LLVMBuildAdd(builder, tmp, ac_get_thread_id(&ctx->ac), "");
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

static LLVMValueRef
ngg_gs_get_vertex_storage(struct si_shader_context *ctx)
{
	const struct si_shader_selector *sel = ctx->shader->selector;
	const struct tgsi_shader_info *info = &sel->info;

	LLVMTypeRef elements[2] = {
		LLVMArrayType(ctx->ac.i32, 4 * info->num_outputs),
		LLVMArrayType(ctx->ac.i8, 4),
	};
	LLVMTypeRef type = LLVMStructTypeInContext(ctx->ac.context, elements, 2, false);
	type = LLVMPointerType(LLVMArrayType(type, 0), AC_ADDR_SPACE_LDS);
	return LLVMBuildBitCast(ctx->ac.builder, ctx->gs_ngg_emit, type, "");
}

/**
 * Return a pointer to the LDS storage reserved for the N'th vertex, where N
 * is in emit order; that is:
 * - during the epilogue, N is the threadidx (relative to the entire threadgroup)
 * - during vertex emit, i.e. while the API GS shader invocation is running,
 *   N = threadidx * gs_max_out_vertices + emitidx
 *
 * Goals of the LDS memory layout:
 * 1. Eliminate bank conflicts on write for geometry shaders that have all emits
 *    in uniform control flow
 * 2. Eliminate bank conflicts on read for export if, additionally, there is no
 *    culling
 * 3. Agnostic to the number of waves (since we don't know it before compiling)
 * 4. Allow coalescing of LDS instructions (ds_write_b128 etc.)
 * 5. Avoid wasting memory.
 *
 * We use an AoS layout due to point 4 (this also helps point 3). In an AoS
 * layout, elimination of bank conflicts requires that each vertex occupy an
 * odd number of dwords. We use the additional dword to store the output stream
 * index as well as a flag to indicate whether this vertex ends a primitive
 * for rasterization.
 *
 * Swizzling is required to satisfy points 1 and 2 simultaneously.
 *
 * Vertices are stored in export order (gsthread * gs_max_out_vertices + emitidx).
 * Indices are swizzled in groups of 32, which ensures point 1 without
 * disturbing point 2.
 *
 * \return an LDS pointer to type {[N x i32], [4 x i8]}
 */
static LLVMValueRef
ngg_gs_vertex_ptr(struct si_shader_context *ctx, LLVMValueRef vertexidx)
{
	struct si_shader_selector *sel = ctx->shader->selector;
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef storage = ngg_gs_get_vertex_storage(ctx);

	/* gs_max_out_vertices = 2^(write_stride_2exp) * some odd number */
	unsigned write_stride_2exp = ffs(sel->gs_max_out_vertices) - 1;
	if (write_stride_2exp) {
		LLVMValueRef row =
			LLVMBuildLShr(builder, vertexidx,
				      LLVMConstInt(ctx->ac.i32, 5, false), "");
		LLVMValueRef swizzle =
			LLVMBuildAnd(builder, row,
				     LLVMConstInt(ctx->ac.i32, (1u << write_stride_2exp) - 1,
						  false), "");
		vertexidx = LLVMBuildXor(builder, vertexidx, swizzle, "");
	}

	return ac_build_gep0(&ctx->ac, storage, vertexidx);
}

static LLVMValueRef
ngg_gs_emit_vertex_ptr(struct si_shader_context *ctx, LLVMValueRef gsthread,
		       LLVMValueRef emitidx)
{
	struct si_shader_selector *sel = ctx->shader->selector;
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef tmp;

	tmp = LLVMConstInt(ctx->ac.i32, sel->gs_max_out_vertices, false);
	tmp = LLVMBuildMul(builder, tmp, gsthread, "");
	const LLVMValueRef vertexidx = LLVMBuildAdd(builder, tmp, emitidx, "");
	return ngg_gs_vertex_ptr(ctx, vertexidx);
}

void gfx10_ngg_gs_emit_vertex(struct si_shader_context *ctx,
			      unsigned stream,
			      LLVMValueRef *addrs)
{
	const struct si_shader_selector *sel = ctx->shader->selector;
	const struct tgsi_shader_info *info = &sel->info;
	LLVMBuilderRef builder = ctx->ac.builder;
	struct lp_build_if_state if_state;
	LLVMValueRef tmp;
	const LLVMValueRef vertexidx =
		LLVMBuildLoad(builder, ctx->gs_next_vertex[stream], "");

	/* If this thread has already emitted the declared maximum number of
	 * vertices, skip the write: excessive vertex emissions are not
	 * supposed to have any effect.
	 */
	const LLVMValueRef can_emit =
		LLVMBuildICmp(builder, LLVMIntULT, vertexidx,
			      LLVMConstInt(ctx->i32, sel->gs_max_out_vertices, false), "");

	tmp = LLVMBuildAdd(builder, vertexidx, ctx->ac.i32_1, "");
	tmp = LLVMBuildSelect(builder, can_emit, tmp, vertexidx, "");
	LLVMBuildStore(builder, tmp, ctx->gs_next_vertex[stream]);

	lp_build_if(&if_state, &ctx->gallivm, can_emit);

	const LLVMValueRef vertexptr =
		ngg_gs_emit_vertex_ptr(ctx, get_thread_id_in_tg(ctx), vertexidx);
	unsigned out_idx = 0;
	for (unsigned i = 0; i < info->num_outputs; i++) {
		for (unsigned chan = 0; chan < 4; chan++, out_idx++) {
			if (!(info->output_usagemask[i] & (1 << chan)) ||
			    ((info->output_streams[i] >> (2 * chan)) & 3) != stream)
				continue;

			LLVMValueRef out_val = LLVMBuildLoad(builder, addrs[4 * i + chan], "");
			LLVMValueRef gep_idx[3] = {
				ctx->ac.i32_0, /* implied C-style array */
				ctx->ac.i32_0, /* first entry of struct */
				LLVMConstInt(ctx->ac.i32, out_idx, false),
			};
			LLVMValueRef ptr = LLVMBuildGEP(builder, vertexptr, gep_idx, 3, "");

			out_val = ac_to_integer(&ctx->ac, out_val);
			LLVMBuildStore(builder, out_val, ptr);
		}
	}
	assert(out_idx * 4 == sel->gsvs_vertex_size);

	/* Determine and store whether this vertex completed a primitive. */
	const LLVMValueRef curverts = LLVMBuildLoad(builder, ctx->gs_curprim_verts[stream], "");

	tmp = LLVMConstInt(ctx->ac.i32, u_vertices_per_prim(sel->gs_output_prim) - 1, false);
	const LLVMValueRef iscompleteprim =
		LLVMBuildICmp(builder, LLVMIntUGE, curverts, tmp, "");

	tmp = LLVMBuildAdd(builder, curverts, ctx->ac.i32_1, "");
	LLVMBuildStore(builder, tmp, ctx->gs_curprim_verts[stream]);

	LLVMValueRef gep_idx[3] = {
		ctx->ac.i32_0, /* implied C-style array */
		ctx->ac.i32_1, /* second struct entry */
		LLVMConstInt(ctx->ac.i32, stream, false),
	};
	const LLVMValueRef primflagptr =
		LLVMBuildGEP(builder, vertexptr, gep_idx, 3, "");

	tmp = LLVMBuildZExt(builder, iscompleteprim, ctx->ac.i8, "");
	LLVMBuildStore(builder, tmp, primflagptr);

	lp_build_endif(&if_state);
}

void gfx10_ngg_gs_emit_epilogue(struct si_shader_context *ctx)
{
	const struct si_shader_selector *sel = ctx->shader->selector;
	const struct tgsi_shader_info *info = &sel->info;
	const unsigned verts_per_prim = u_vertices_per_prim(sel->gs_output_prim);
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef i8_0 = LLVMConstInt(ctx->ac.i8, 0, false);
	LLVMValueRef tmp, tmp2;

	/* Zero out remaining (non-emitted) primitive flags.
	 *
	 * Note: Alternatively, we could pass the relevant gs_next_vertex to
	 *       the emit threads via LDS. This is likely worse in the expected
	 *       typical case where each GS thread emits the full set of
	 *       vertices.
	 */
	for (unsigned stream = 0; stream < 4; ++stream) {
		if (!info->num_stream_output_components[stream])
			continue;

		const LLVMValueRef gsthread = get_thread_id_in_tg(ctx);

		ac_build_bgnloop(&ctx->ac, 5100);

		const LLVMValueRef vertexidx =
			LLVMBuildLoad(builder, ctx->gs_next_vertex[stream], "");
		tmp = LLVMBuildICmp(builder, LLVMIntUGE, vertexidx,
			LLVMConstInt(ctx->ac.i32, sel->gs_max_out_vertices, false), "");
		ac_build_ifcc(&ctx->ac, tmp, 5101);
		ac_build_break(&ctx->ac);
		ac_build_endif(&ctx->ac, 5101);

		tmp = LLVMBuildAdd(builder, vertexidx, ctx->ac.i32_1, "");
		LLVMBuildStore(builder, tmp, ctx->gs_next_vertex[stream]);

		tmp = ngg_gs_emit_vertex_ptr(ctx, gsthread, vertexidx);
		LLVMValueRef gep_idx[3] = {
			ctx->ac.i32_0, /* implied C-style array */
			ctx->ac.i32_1, /* second entry of struct */
			LLVMConstInt(ctx->ac.i32, stream, false),
		};
		tmp = LLVMBuildGEP(builder, tmp, gep_idx, 3, "");
		LLVMBuildStore(builder, i8_0, tmp);

		ac_build_endloop(&ctx->ac, 5100);
	}

	lp_build_endif(&ctx->merged_wrap_if_state);

	ac_build_s_barrier(&ctx->ac);

	const LLVMValueRef tid = get_thread_id_in_tg(ctx);
	LLVMValueRef num_emit_threads = ngg_get_prim_cnt(ctx);

	/* TODO: streamout */

	/* TODO: culling */

	/* Determine vertex liveness. */
	LLVMValueRef vertliveptr = lp_build_alloca(&ctx->gallivm, ctx->ac.i1, "vertexlive");

	tmp = LLVMBuildICmp(builder, LLVMIntULT, tid, num_emit_threads, "");
	ac_build_ifcc(&ctx->ac, tmp, 5120);
	{
		for (unsigned i = 0; i < verts_per_prim; ++i) {
			const LLVMValueRef primidx =
				LLVMBuildAdd(builder, tid,
					     LLVMConstInt(ctx->ac.i32, i, false), "");

			if (i > 0) {
				tmp = LLVMBuildICmp(builder, LLVMIntULT, primidx, num_emit_threads, "");
				ac_build_ifcc(&ctx->ac, tmp, 5121 + i);
			}

			/* Load primitive liveness */
			tmp = ngg_gs_vertex_ptr(ctx, primidx);
			LLVMValueRef gep_idx[3] = {
				ctx->ac.i32_0, /* implicit C-style array */
				ctx->ac.i32_1, /* second value of struct */
				ctx->ac.i32_0, /* stream 0 */
			};
			tmp = LLVMBuildGEP(builder, tmp, gep_idx, 3, "");
			tmp = LLVMBuildLoad(builder, tmp, "");
			const LLVMValueRef primlive =
				LLVMBuildTrunc(builder, tmp, ctx->ac.i1, "");

			tmp = LLVMBuildLoad(builder, vertliveptr, "");
			tmp = LLVMBuildOr(builder, tmp, primlive, ""),
			LLVMBuildStore(builder, tmp, vertliveptr);

			if (i > 0)
				ac_build_endif(&ctx->ac, 5121 + i);
		}
	}
	ac_build_endif(&ctx->ac, 5120);

	/* Inclusive scan addition across the current wave. */
	LLVMValueRef vertlive = LLVMBuildLoad(builder, vertliveptr, "");
	struct ac_wg_scan vertlive_scan = {};
	vertlive_scan.op = nir_op_iadd;
	vertlive_scan.enable_reduce = true;
	vertlive_scan.enable_exclusive = true;
	vertlive_scan.src = vertlive;
	vertlive_scan.scratch = ac_build_gep0(&ctx->ac, ctx->gs_ngg_scratch, ctx->i32_0);
	vertlive_scan.waveidx = get_wave_id_in_tg(ctx);
	vertlive_scan.numwaves = get_tgsize(ctx);
	vertlive_scan.maxwaves = 8;

	ac_build_wg_scan(&ctx->ac, &vertlive_scan);

	/* Skip all exports (including index exports) when possible. At least on
	 * early gfx10 revisions this is also to avoid hangs.
	 */
	LLVMValueRef have_exports =
		LLVMBuildICmp(builder, LLVMIntNE, vertlive_scan.result_reduce, ctx->ac.i32_0, "");
	num_emit_threads =
		LLVMBuildSelect(builder, have_exports, num_emit_threads, ctx->ac.i32_0, "");

	/* Allocate export space. Send this message as early as possible, to
	 * hide the latency of the SQ <-> SPI roundtrip.
	 *
	 * Note: We could consider compacting primitives for export as well.
	 *       PA processes 1 non-null prim / clock, but it fetches 4 DW of
	 *       prim data per clock and skips null primitives at no additional
	 *       cost. So compacting primitives can only be beneficial when
	 *       there are 4 or more contiguous null primitives in the export
	 *       (in the common case of single-dword prim exports).
	 */
	build_sendmsg_gs_alloc_req(ctx, vertlive_scan.result_reduce, num_emit_threads);

	/* Setup the reverse vertex compaction permutation. We re-use stream 1
	 * of the primitive liveness flags, relying on the fact that each
	 * threadgroup can have at most 256 threads. */
	ac_build_ifcc(&ctx->ac, vertlive, 5130);
	{
		tmp = ngg_gs_vertex_ptr(ctx, vertlive_scan.result_exclusive);
		LLVMValueRef gep_idx[3] = {
			ctx->ac.i32_0, /* implicit C-style array */
			ctx->ac.i32_1, /* second value of struct */
			ctx->ac.i32_1, /* stream 1 */
		};
		tmp = LLVMBuildGEP(builder, tmp, gep_idx, 3, "");
		tmp2 = LLVMBuildTrunc(builder, tid, ctx->ac.i8, "");
		LLVMBuildStore(builder, tmp2, tmp);
	}
	ac_build_endif(&ctx->ac, 5130);

	ac_build_s_barrier(&ctx->ac);

	/* Export primitive data */
	tmp = LLVMBuildICmp(builder, LLVMIntULT, tid, num_emit_threads, "");
	ac_build_ifcc(&ctx->ac, tmp, 5140);
	{
		struct ngg_prim prim = {};
		prim.num_vertices = verts_per_prim;

		tmp = ngg_gs_vertex_ptr(ctx, tid);
		LLVMValueRef gep_idx[3] = {
			ctx->ac.i32_0, /* implicit C-style array */
			ctx->ac.i32_1, /* second value of struct */
			ctx->ac.i32_0, /* primflag */
		};
		tmp = LLVMBuildGEP(builder, tmp, gep_idx, 3, "");
		tmp = LLVMBuildLoad(builder, tmp, "");
		prim.isnull = LLVMBuildICmp(builder, LLVMIntEQ, tmp,
					    LLVMConstInt(ctx->ac.i8, 0, false), "");

		for (unsigned i = 0; i < verts_per_prim; ++i) {
			prim.index[i] = LLVMBuildSub(builder, vertlive_scan.result_exclusive,
				LLVMConstInt(ctx->ac.i32, verts_per_prim - i - 1, false), "");
			prim.edgeflag[i] = ctx->ac.i1false;
		}

		build_export_prim(ctx, &prim);
	}
	ac_build_endif(&ctx->ac, 5140);

	/* Export position and parameter data */
	tmp = LLVMBuildICmp(builder, LLVMIntULT, tid, vertlive_scan.result_reduce, "");
	ac_build_ifcc(&ctx->ac, tmp, 5145);
	{
		struct si_shader_output_values *outputs = NULL;
		outputs = MALLOC(info->num_outputs * sizeof(outputs[0]));

		tmp = ngg_gs_vertex_ptr(ctx, tid);
		LLVMValueRef gep_idx[3] = {
			ctx->ac.i32_0, /* implicit C-style array */
			ctx->ac.i32_1, /* second value of struct */
			ctx->ac.i32_1, /* stream 1: source data index */
		};
		tmp = LLVMBuildGEP(builder, tmp, gep_idx, 3, "");
		tmp = LLVMBuildLoad(builder, tmp, "");
		tmp = LLVMBuildZExt(builder, tmp, ctx->ac.i32, "");
		const LLVMValueRef vertexptr = ngg_gs_vertex_ptr(ctx, tmp);

		unsigned out_idx = 0;
		gep_idx[1] = ctx->ac.i32_0;
		for (unsigned i = 0; i < info->num_outputs; i++) {
			outputs[i].semantic_name = info->output_semantic_name[i];
			outputs[i].semantic_index = info->output_semantic_index[i];

			for (unsigned j = 0; j < 4; j++, out_idx++) {
				gep_idx[2] = LLVMConstInt(ctx->ac.i32, out_idx, false);
				tmp = LLVMBuildGEP(builder, vertexptr, gep_idx, 3, "");
				tmp = LLVMBuildLoad(builder, tmp, "");
				outputs[i].values[j] = ac_to_float(&ctx->ac, tmp);
				outputs[i].vertex_stream[j] =
					(info->output_streams[i] >> (2 * j)) & 3;
			}
		}

		si_llvm_export_vs(ctx, outputs, info->num_outputs);

		FREE(outputs);
	}
	ac_build_endif(&ctx->ac, 5145);
}

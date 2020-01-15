/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 * All Rights Reserved.
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

#include <llvm/Config/llvm-config.h>

#include "si_shader_internal.h"
#include "si_pipe.h"
#include "sid.h"
#include "ac_llvm_util.h"

/**
 * Return a value that is equal to the given i32 \p index if it lies in [0,num)
 * or an undefined value in the same interval otherwise.
 */
LLVMValueRef si_llvm_bound_index(struct si_shader_context *ctx,
				 LLVMValueRef index,
				 unsigned num)
{
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef c_max = LLVMConstInt(ctx->i32, num - 1, 0);
	LLVMValueRef cc;

	if (util_is_power_of_two_or_zero(num)) {
		index = LLVMBuildAnd(builder, index, c_max, "");
	} else {
		/* In theory, this MAX pattern should result in code that is
		 * as good as the bit-wise AND above.
		 *
		 * In practice, LLVM generates worse code (at the time of
		 * writing), because its value tracking is not strong enough.
		 */
		cc = LLVMBuildICmp(builder, LLVMIntULE, index, c_max, "");
		index = LLVMBuildSelect(builder, cc, index, c_max, "");
	}

	return index;
}

/**
 * Given a 256-bit resource descriptor, force the DCC enable bit to off.
 *
 * At least on Tonga, executing image stores on images with DCC enabled and
 * non-trivial can eventually lead to lockups. This can occur when an
 * application binds an image as read-only but then uses a shader that writes
 * to it. The OpenGL spec allows almost arbitrarily bad behavior (including
 * program termination) in this case, but it doesn't cost much to be a bit
 * nicer: disabling DCC in the shader still leads to undefined results but
 * avoids the lockup.
 */
static LLVMValueRef force_dcc_off(struct si_shader_context *ctx,
				  LLVMValueRef rsrc)
{
	if (ctx->screen->info.chip_class <= GFX7) {
		return rsrc;
	} else {
		LLVMValueRef i32_6 = LLVMConstInt(ctx->i32, 6, 0);
		LLVMValueRef i32_C = LLVMConstInt(ctx->i32, C_008F28_COMPRESSION_EN, 0);
		LLVMValueRef tmp;

		tmp = LLVMBuildExtractElement(ctx->ac.builder, rsrc, i32_6, "");
		tmp = LLVMBuildAnd(ctx->ac.builder, tmp, i32_C, "");
		return LLVMBuildInsertElement(ctx->ac.builder, rsrc, tmp, i32_6, "");
	}
}

/* AC_DESC_FMASK is handled exactly like AC_DESC_IMAGE. The caller should
 * adjust "index" to point to FMASK. */
LLVMValueRef si_load_image_desc(struct si_shader_context *ctx,
				LLVMValueRef list, LLVMValueRef index,
				enum ac_descriptor_type desc_type,
				bool uses_store, bool bindless)
{
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef rsrc;

	if (desc_type == AC_DESC_BUFFER) {
		index = ac_build_imad(&ctx->ac, index, LLVMConstInt(ctx->i32, 2, 0),
				      ctx->i32_1);
		list = LLVMBuildPointerCast(builder, list,
					    ac_array_in_const32_addr_space(ctx->v4i32), "");
	} else {
		assert(desc_type == AC_DESC_IMAGE ||
		       desc_type == AC_DESC_FMASK);
	}

	if (bindless)
		rsrc = ac_build_load_to_sgpr_uint_wraparound(&ctx->ac, list, index);
	else
		rsrc = ac_build_load_to_sgpr(&ctx->ac, list, index);

	if (desc_type == AC_DESC_IMAGE && uses_store)
		rsrc = force_dcc_off(ctx, rsrc);
	return rsrc;
}

/**
 * Load an image view, fmask view. or sampler state descriptor.
 */
LLVMValueRef si_load_sampler_desc(struct si_shader_context *ctx,
				  LLVMValueRef list, LLVMValueRef index,
				  enum ac_descriptor_type type)
{
	LLVMBuilderRef builder = ctx->ac.builder;

	switch (type) {
	case AC_DESC_IMAGE:
		/* The image is at [0:7]. */
		index = LLVMBuildMul(builder, index, LLVMConstInt(ctx->i32, 2, 0), "");
		break;
	case AC_DESC_BUFFER:
		/* The buffer is in [4:7]. */
		index = ac_build_imad(&ctx->ac, index, LLVMConstInt(ctx->i32, 4, 0),
				      ctx->i32_1);
		list = LLVMBuildPointerCast(builder, list,
					    ac_array_in_const32_addr_space(ctx->v4i32), "");
		break;
	case AC_DESC_FMASK:
		/* The FMASK is at [8:15]. */
		index = ac_build_imad(&ctx->ac, index, LLVMConstInt(ctx->i32, 2, 0),
				      ctx->i32_1);
		break;
	case AC_DESC_SAMPLER:
		/* The sampler state is at [12:15]. */
		index = ac_build_imad(&ctx->ac, index, LLVMConstInt(ctx->i32, 4, 0),
				      LLVMConstInt(ctx->i32, 3, 0));
		list = LLVMBuildPointerCast(builder, list,
					    ac_array_in_const32_addr_space(ctx->v4i32), "");
		break;
	case AC_DESC_PLANE_0:
	case AC_DESC_PLANE_1:
	case AC_DESC_PLANE_2:
		/* Only used for the multiplane image support for Vulkan. Should
		 * never be reached in radeonsi.
		 */
		unreachable("Plane descriptor requested in radeonsi.");
	}

	return ac_build_load_to_sgpr(&ctx->ac, list, index);
}

/**
 * Load a dword from a constant buffer.
 */
LLVMValueRef si_buffer_load_const(struct si_shader_context *ctx,
				  LLVMValueRef resource, LLVMValueRef offset)
{
	return ac_build_buffer_load(&ctx->ac, resource, 1, NULL, offset, NULL,
				    0, 0, true, true);
}

void si_llvm_build_ret(struct si_shader_context *ctx, LLVMValueRef ret)
{
	if (LLVMGetTypeKind(LLVMTypeOf(ret)) == LLVMVoidTypeKind)
		LLVMBuildRetVoid(ctx->ac.builder);
	else
		LLVMBuildRet(ctx->ac.builder, ret);
}

LLVMValueRef si_insert_input_ret(struct si_shader_context *ctx, LLVMValueRef ret,
				 struct ac_arg param, unsigned return_index)
{
	return LLVMBuildInsertValue(ctx->ac.builder, ret,
				    ac_get_arg(&ctx->ac, param),
				    return_index, "");
}

LLVMValueRef si_insert_input_ret_float(struct si_shader_context *ctx, LLVMValueRef ret,
				       struct ac_arg param, unsigned return_index)
{
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef p = ac_get_arg(&ctx->ac, param);

	return LLVMBuildInsertValue(builder, ret,
				    ac_to_float(&ctx->ac, p),
				    return_index, "");
}

LLVMValueRef si_insert_input_ptr(struct si_shader_context *ctx, LLVMValueRef ret,
				 struct ac_arg param, unsigned return_index)
{
	LLVMBuilderRef builder = ctx->ac.builder;
	LLVMValueRef ptr = ac_get_arg(&ctx->ac, param);
	ptr = LLVMBuildPtrToInt(builder, ptr, ctx->i32, "");
	return LLVMBuildInsertValue(builder, ret, ptr, return_index, "");
}

LLVMValueRef si_prolog_get_rw_buffers(struct si_shader_context *ctx)
{
	LLVMValueRef ptr[2], list;
	bool merged_shader = si_is_merged_shader(ctx);

	ptr[0] = LLVMGetParam(ctx->main_fn, (merged_shader ? 8 : 0) + SI_SGPR_RW_BUFFERS);
	list = LLVMBuildIntToPtr(ctx->ac.builder, ptr[0],
				 ac_array_in_const32_addr_space(ctx->v4i32), "");
	return list;
}

LLVMValueRef si_build_gather_64bit(struct si_shader_context *ctx,
				   LLVMTypeRef type, LLVMValueRef val1,
				   LLVMValueRef val2)
{
	LLVMValueRef values[2] = {
		ac_to_integer(&ctx->ac, val1),
		ac_to_integer(&ctx->ac, val2),
	};
	LLVMValueRef result = ac_build_gather_values(&ctx->ac, values, 2);
	return LLVMBuildBitCast(ctx->ac.builder, result, type, "");
}

void si_llvm_emit_barrier(struct si_shader_context *ctx)
{
	/* GFX6 only (thanks to a hw bug workaround):
	 * The real barrier instruction isn’t needed, because an entire patch
	 * always fits into a single wave.
	 */
	if (ctx->screen->info.chip_class == GFX6 &&
	    ctx->type == PIPE_SHADER_TESS_CTRL) {
		ac_build_waitcnt(&ctx->ac, AC_WAIT_LGKM | AC_WAIT_VLOAD | AC_WAIT_VSTORE);
		return;
	}

	ac_build_s_barrier(&ctx->ac);
}

/* Ensure that the esgs ring is declared.
 *
 * We declare it with 64KB alignment as a hint that the
 * pointer value will always be 0.
 */
void si_llvm_declare_esgs_ring(struct si_shader_context *ctx)
{
	if (ctx->esgs_ring)
		return;

	assert(!LLVMGetNamedGlobal(ctx->ac.module, "esgs_ring"));

	ctx->esgs_ring = LLVMAddGlobalInAddressSpace(
		ctx->ac.module, LLVMArrayType(ctx->i32, 0),
		"esgs_ring",
		AC_ADDR_SPACE_LDS);
	LLVMSetLinkage(ctx->esgs_ring, LLVMExternalLinkage);
	LLVMSetAlignment(ctx->esgs_ring, 64 * 1024);
}

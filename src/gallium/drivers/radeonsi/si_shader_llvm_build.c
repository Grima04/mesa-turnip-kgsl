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

#include "si_shader_internal.h"
#include "si_pipe.h"
#include "sid.h"

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

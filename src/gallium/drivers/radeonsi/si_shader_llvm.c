/*
 * Copyright 2016 Advanced Micro Devices, Inc.
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
#include "util/u_memory.h"

struct si_llvm_diagnostics {
	struct pipe_debug_callback *debug;
	unsigned retval;
};

static void si_diagnostic_handler(LLVMDiagnosticInfoRef di, void *context)
{
	struct si_llvm_diagnostics *diag = (struct si_llvm_diagnostics *)context;
	LLVMDiagnosticSeverity severity = LLVMGetDiagInfoSeverity(di);
	const char *severity_str = NULL;

	switch (severity) {
	case LLVMDSError:
		severity_str = "error";
		break;
	case LLVMDSWarning:
		severity_str = "warning";
		break;
	case LLVMDSRemark:
	case LLVMDSNote:
	default:
		return;
	}

	char *description = LLVMGetDiagInfoDescription(di);

	pipe_debug_message(diag->debug, SHADER_INFO,
			   "LLVM diagnostic (%s): %s", severity_str, description);

	if (severity == LLVMDSError) {
		diag->retval = 1;
		fprintf(stderr,"LLVM triggered Diagnostic Handler: %s\n", description);
	}

	LLVMDisposeMessage(description);
}

/**
 * Compile an LLVM module to machine code.
 *
 * @returns 0 for success, 1 for failure
 */
unsigned si_llvm_compile(LLVMModuleRef M, struct si_shader_binary *binary,
			 struct ac_llvm_compiler *compiler,
			 struct pipe_debug_callback *debug,
			 bool less_optimized, unsigned wave_size)
{
	struct ac_compiler_passes *passes = compiler->passes;

	if (wave_size == 32)
		passes = compiler->passes_wave32;
	else if (less_optimized && compiler->low_opt_passes)
		passes = compiler->low_opt_passes;

	struct si_llvm_diagnostics diag;
	LLVMContextRef llvm_ctx;

	diag.debug = debug;
	diag.retval = 0;

	/* Setup Diagnostic Handler*/
	llvm_ctx = LLVMGetModuleContext(M);

	LLVMContextSetDiagnosticHandler(llvm_ctx, si_diagnostic_handler, &diag);

	/* Compile IR. */
	if (!ac_compile_module_to_elf(passes, M, (char **)&binary->elf_buffer,
				      &binary->elf_size))
		diag.retval = 1;

	if (diag.retval != 0)
		pipe_debug_message(debug, SHADER_INFO, "LLVM compile failed");
	return diag.retval;
}

void si_shader_binary_clean(struct si_shader_binary *binary)
{
	free((void *)binary->elf_buffer);
	binary->elf_buffer = NULL;

	free(binary->llvm_ir_string);
	binary->llvm_ir_string = NULL;
}

void si_llvm_context_init(struct si_shader_context *ctx,
			  struct si_screen *sscreen,
			  struct ac_llvm_compiler *compiler,
			  unsigned wave_size)
{
	/* Initialize the gallivm object:
	 * We are only using the module, context, and builder fields of this struct.
	 * This should be enough for us to be able to pass our gallivm struct to the
	 * helper functions in the gallivm module.
	 */
	memset(ctx, 0, sizeof(*ctx));
	ctx->screen = sscreen;
	ctx->compiler = compiler;

	ac_llvm_context_init(&ctx->ac, compiler, sscreen->info.chip_class,
			     sscreen->info.family,
			     AC_FLOAT_MODE_NO_SIGNED_ZEROS_FP_MATH,
			     wave_size, 64);

	ctx->voidt = LLVMVoidTypeInContext(ctx->ac.context);
	ctx->i1 = LLVMInt1TypeInContext(ctx->ac.context);
	ctx->i8 = LLVMInt8TypeInContext(ctx->ac.context);
	ctx->i32 = LLVMInt32TypeInContext(ctx->ac.context);
	ctx->i64 = LLVMInt64TypeInContext(ctx->ac.context);
	ctx->i128 = LLVMIntTypeInContext(ctx->ac.context, 128);
	ctx->f32 = LLVMFloatTypeInContext(ctx->ac.context);
	ctx->v2i32 = LLVMVectorType(ctx->i32, 2);
	ctx->v4i32 = LLVMVectorType(ctx->i32, 4);
	ctx->v4f32 = LLVMVectorType(ctx->f32, 4);
	ctx->v8i32 = LLVMVectorType(ctx->i32, 8);

	ctx->i32_0 = LLVMConstInt(ctx->i32, 0, 0);
	ctx->i32_1 = LLVMConstInt(ctx->i32, 1, 0);
	ctx->i1false = LLVMConstInt(ctx->i1, 0, 0);
	ctx->i1true = LLVMConstInt(ctx->i1, 1, 0);
}

/* Set the context to a certain shader. Can be called repeatedly
 * to change the shader. */
void si_llvm_context_set_ir(struct si_shader_context *ctx,
			    struct si_shader *shader)
{
	struct si_shader_selector *sel = shader->selector;
	const struct si_shader_info *info = &sel->info;

	ctx->shader = shader;
	ctx->type = sel->type;

	ctx->num_const_buffers = util_last_bit(info->const_buffers_declared);
	ctx->num_shader_buffers = util_last_bit(info->shader_buffers_declared);

	ctx->num_samplers = util_last_bit(info->samplers_declared);
	ctx->num_images = util_last_bit(info->images_declared);
}

void si_llvm_create_func(struct si_shader_context *ctx, const char *name,
			 LLVMTypeRef *return_types, unsigned num_return_elems,
			 unsigned max_workgroup_size)
{
	LLVMTypeRef ret_type;
	enum ac_llvm_calling_convention call_conv;
	enum pipe_shader_type real_shader_type;

	if (num_return_elems)
		ret_type = LLVMStructTypeInContext(ctx->ac.context,
						   return_types,
						   num_return_elems, true);
	else
		ret_type = ctx->voidt;

	real_shader_type = ctx->type;

	/* LS is merged into HS (TCS), and ES is merged into GS. */
	if (ctx->screen->info.chip_class >= GFX9) {
		if (ctx->shader->key.as_ls)
			real_shader_type = PIPE_SHADER_TESS_CTRL;
		else if (ctx->shader->key.as_es || ctx->shader->key.as_ngg)
			real_shader_type = PIPE_SHADER_GEOMETRY;
	}

	switch (real_shader_type) {
	case PIPE_SHADER_VERTEX:
	case PIPE_SHADER_TESS_EVAL:
		call_conv = AC_LLVM_AMDGPU_VS;
		break;
	case PIPE_SHADER_TESS_CTRL:
		call_conv = AC_LLVM_AMDGPU_HS;
		break;
	case PIPE_SHADER_GEOMETRY:
		call_conv = AC_LLVM_AMDGPU_GS;
		break;
	case PIPE_SHADER_FRAGMENT:
		call_conv = AC_LLVM_AMDGPU_PS;
		break;
	case PIPE_SHADER_COMPUTE:
		call_conv = AC_LLVM_AMDGPU_CS;
		break;
	default:
		unreachable("Unhandle shader type");
	}

	/* Setup the function */
	ctx->return_type = ret_type;
	ctx->main_fn = ac_build_main(&ctx->args, &ctx->ac, call_conv, name,
				     ret_type, ctx->ac.module);
	ctx->return_value = LLVMGetUndef(ctx->return_type);

	if (ctx->screen->info.address32_hi) {
		ac_llvm_add_target_dep_function_attr(ctx->main_fn,
						     "amdgpu-32bit-address-high-bits",
						     ctx->screen->info.address32_hi);
	}

	LLVMAddTargetDependentFunctionAttr(ctx->main_fn,
					   "no-signed-zeros-fp-math",
					   "true");

	ac_llvm_set_workgroup_size(ctx->main_fn, max_workgroup_size);
}

void si_llvm_optimize_module(struct si_shader_context *ctx)
{
	/* Dump LLVM IR before any optimization passes */
	if (ctx->screen->debug_flags & DBG(PREOPT_IR) &&
	    si_can_dump_shader(ctx->screen, ctx->type))
		LLVMDumpModule(ctx->ac.module);

	/* Run the pass */
	LLVMRunPassManager(ctx->compiler->passmgr, ctx->ac.module);
	LLVMDisposeBuilder(ctx->ac.builder);
}

void si_llvm_dispose(struct si_shader_context *ctx)
{
	LLVMDisposeModule(ctx->ac.module);
	LLVMContextDispose(ctx->ac.context);
	ac_llvm_context_dispose(&ctx->ac);
}

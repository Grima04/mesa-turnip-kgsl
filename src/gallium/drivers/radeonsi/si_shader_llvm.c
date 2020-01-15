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
#include "ac_rtld.h"
#include "ac_nir_to_llvm.h"
#include "sid.h"

#include "tgsi/tgsi_from_mesa.h"
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

int si_compile_llvm(struct si_screen *sscreen,
		    struct si_shader_binary *binary,
		    struct ac_shader_config *conf,
		    struct ac_llvm_compiler *compiler,
		    struct ac_llvm_context *ac,
		    struct pipe_debug_callback *debug,
		    enum pipe_shader_type shader_type,
		    const char *name,
		    bool less_optimized)
{
	unsigned count = p_atomic_inc_return(&sscreen->num_compilations);

	if (si_can_dump_shader(sscreen, shader_type)) {
		fprintf(stderr, "radeonsi: Compiling shader %d\n", count);

		if (!(sscreen->debug_flags & (DBG(NO_IR) | DBG(PREOPT_IR)))) {
			fprintf(stderr, "%s LLVM IR:\n\n", name);
			ac_dump_module(ac->module);
			fprintf(stderr, "\n");
		}
	}

	if (sscreen->record_llvm_ir) {
		char *ir = LLVMPrintModuleToString(ac->module);
		binary->llvm_ir_string = strdup(ir);
		LLVMDisposeMessage(ir);
	}

	if (!si_replace_shader(count, binary)) {
		struct ac_compiler_passes *passes = compiler->passes;

		if (ac->wave_size == 32)
			passes = compiler->passes_wave32;
		else if (less_optimized && compiler->low_opt_passes)
			passes = compiler->low_opt_passes;

		struct si_llvm_diagnostics diag = {debug};
		LLVMContextSetDiagnosticHandler(ac->context, si_diagnostic_handler, &diag);

		if (!ac_compile_module_to_elf(passes, ac->module,
					      (char **)&binary->elf_buffer,
					      &binary->elf_size))
			diag.retval = 1;

		if (diag.retval != 0) {
			pipe_debug_message(debug, SHADER_INFO, "LLVM compilation failed");
			return diag.retval;
		}
	}

	struct ac_rtld_binary rtld;
	if (!ac_rtld_open(&rtld, (struct ac_rtld_open_info){
			.info = &sscreen->info,
			.shader_type = tgsi_processor_to_shader_stage(shader_type),
			.wave_size = ac->wave_size,
			.num_parts = 1,
			.elf_ptrs = &binary->elf_buffer,
			.elf_sizes = &binary->elf_size }))
		return -1;

	bool ok = ac_rtld_read_config(&rtld, conf);
	ac_rtld_close(&rtld);
	if (!ok)
		return -1;

	/* Enable 64-bit and 16-bit denormals, because there is no performance
	 * cost.
	 *
	 * If denormals are enabled, all floating-point output modifiers are
	 * ignored.
	 *
	 * Don't enable denormals for 32-bit floats, because:
	 * - Floating-point output modifiers would be ignored by the hw.
	 * - Some opcodes don't support denormals, such as v_mad_f32. We would
	 *   have to stop using those.
	 * - GFX6 & GFX7 would be very slow.
	 */
	conf->float_mode |= V_00B028_FP_64_DENORMS;

	return 0;
}

void si_llvm_context_init(struct si_shader_context *ctx,
			  struct si_screen *sscreen,
			  struct ac_llvm_compiler *compiler,
			  unsigned wave_size)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->screen = sscreen;
	ctx->compiler = compiler;

	ac_llvm_context_init(&ctx->ac, compiler, sscreen->info.chip_class,
			     sscreen->info.family,
			     AC_FLOAT_MODE_NO_SIGNED_ZEROS_FP_MATH,
			     wave_size, 64);
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
		ret_type = ctx->ac.voidt;

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
	ptr = LLVMBuildPtrToInt(builder, ptr, ctx->ac.i32, "");
	return LLVMBuildInsertValue(builder, ret, ptr, return_index, "");
}

LLVMValueRef si_prolog_get_rw_buffers(struct si_shader_context *ctx)
{
	LLVMValueRef ptr[2], list;
	bool merged_shader = si_is_merged_shader(ctx);

	ptr[0] = LLVMGetParam(ctx->main_fn, (merged_shader ? 8 : 0) + SI_SGPR_RW_BUFFERS);
	list = LLVMBuildIntToPtr(ctx->ac.builder, ptr[0],
				 ac_array_in_const32_addr_space(ctx->ac.v4i32), "");
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
		ctx->ac.module, LLVMArrayType(ctx->ac.i32, 0),
		"esgs_ring",
		AC_ADDR_SPACE_LDS);
	LLVMSetLinkage(ctx->esgs_ring, LLVMExternalLinkage);
	LLVMSetAlignment(ctx->esgs_ring, 64 * 1024);
}

void si_init_exec_from_input(struct si_shader_context *ctx, struct ac_arg param,
			     unsigned bitoffset)
{
	LLVMValueRef args[] = {
		ac_get_arg(&ctx->ac, param),
		LLVMConstInt(ctx->ac.i32, bitoffset, 0),
	};
	ac_build_intrinsic(&ctx->ac,
			   "llvm.amdgcn.init.exec.from.input",
			   ctx->ac.voidt, args, 2, AC_FUNC_ATTR_CONVERGENT);
}

bool si_nir_build_llvm(struct si_shader_context *ctx, struct nir_shader *nir)
{
	if (nir->info.stage == MESA_SHADER_VERTEX) {
		si_llvm_load_vs_inputs(ctx, nir);
	} else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
                unsigned colors_read =
                        ctx->shader->selector->info.colors_read;
                LLVMValueRef main_fn = ctx->main_fn;

                LLVMValueRef undef = LLVMGetUndef(ctx->ac.f32);

                unsigned offset = SI_PARAM_POS_FIXED_PT + 1;

                if (colors_read & 0x0f) {
                        unsigned mask = colors_read & 0x0f;
                        LLVMValueRef values[4];
                        values[0] = mask & 0x1 ? LLVMGetParam(main_fn, offset++) : undef;
                        values[1] = mask & 0x2 ? LLVMGetParam(main_fn, offset++) : undef;
                        values[2] = mask & 0x4 ? LLVMGetParam(main_fn, offset++) : undef;
                        values[3] = mask & 0x8 ? LLVMGetParam(main_fn, offset++) : undef;
                        ctx->abi.color0 =
                                ac_to_integer(&ctx->ac,
                                              ac_build_gather_values(&ctx->ac, values, 4));
                }
                if (colors_read & 0xf0) {
                        unsigned mask = (colors_read & 0xf0) >> 4;
                        LLVMValueRef values[4];
                        values[0] = mask & 0x1 ? LLVMGetParam(main_fn, offset++) : undef;
                        values[1] = mask & 0x2 ? LLVMGetParam(main_fn, offset++) : undef;
                        values[2] = mask & 0x4 ? LLVMGetParam(main_fn, offset++) : undef;
                        values[3] = mask & 0x8 ? LLVMGetParam(main_fn, offset++) : undef;
                        ctx->abi.color1 =
                                ac_to_integer(&ctx->ac,
                                              ac_build_gather_values(&ctx->ac, values, 4));
                }

		ctx->abi.interp_at_sample_force_center =
			ctx->shader->key.mono.u.ps.interpolate_at_sample_force_center;
	} else if (nir->info.stage == MESA_SHADER_COMPUTE) {
		if (nir->info.cs.user_data_components_amd) {
			ctx->abi.user_data = ac_get_arg(&ctx->ac, ctx->cs_user_data);
			ctx->abi.user_data = ac_build_expand_to_vec4(&ctx->ac, ctx->abi.user_data,
								     nir->info.cs.user_data_components_amd);
		}
	}

	ctx->abi.inputs = &ctx->inputs[0];
	ctx->abi.clamp_shadow_reference = true;
	ctx->abi.robust_buffer_access = true;

	if (ctx->shader->selector->info.properties[TGSI_PROPERTY_CS_LOCAL_SIZE]) {
		assert(gl_shader_stage_is_compute(nir->info.stage));
		si_declare_compute_memory(ctx);
	}
	ac_nir_translate(&ctx->ac, &ctx->abi, &ctx->args, nir);

	return true;
}

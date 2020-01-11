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

#ifndef SI_SHADER_PRIVATE_H
#define SI_SHADER_PRIVATE_H

#include "si_shader.h"
#include "ac_shader_abi.h"

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>

struct pipe_debug_callback;

#define RADEON_LLVM_MAX_INPUTS 32 * 4

struct si_shader_output_values {
	LLVMValueRef values[4];
	unsigned semantic_name;
	unsigned semantic_index;
	ubyte vertex_stream[4];
};

struct si_shader_context {
	struct ac_llvm_context ac;
	struct si_shader *shader;
	struct si_screen *screen;

	unsigned type; /* PIPE_SHADER_* specifies the type of shader. */

	/* For clamping the non-constant index in resource indexing: */
	unsigned num_const_buffers;
	unsigned num_shader_buffers;
	unsigned num_images;
	unsigned num_samplers;

	struct ac_shader_args args;
	struct ac_shader_abi abi;

	LLVMValueRef inputs[RADEON_LLVM_MAX_INPUTS];

	LLVMBasicBlockRef merged_wrap_if_entry_block;
	int merged_wrap_if_label;

	LLVMValueRef main_fn;
	LLVMTypeRef return_type;

	struct ac_arg const_and_shader_buffers;
	struct ac_arg samplers_and_images;

	/* For merged shaders, the per-stage descriptors for the stage other
	 * than the one we're processing, used to pass them through from the
	 * first stage to the second.
	 */
	struct ac_arg other_const_and_shader_buffers;
	struct ac_arg other_samplers_and_images;

	struct ac_arg rw_buffers;
	struct ac_arg bindless_samplers_and_images;
	/* Common inputs for merged shaders. */
	struct ac_arg merged_wave_info;
	struct ac_arg merged_scratch_offset;
	/* API VS */
	struct ac_arg vertex_buffers;
	struct ac_arg vb_descriptors[5];
	struct ac_arg rel_auto_id;
	struct ac_arg vs_prim_id;
	struct ac_arg vertex_index0;
	/* VS states and layout of LS outputs / TCS inputs at the end
	 *   [0] = clamp vertex color
	 *   [1] = indexed
	 *   [8:20] = stride between patches in DW = num_inputs * num_vertices * 4
	 *            max = 32*32*4 + 32*4
	 *   [24:31] = stride between vertices in DW = num_inputs * 4
	 *             max = 32*4
	 */
	struct ac_arg vs_state_bits;
	struct ac_arg vs_blit_inputs;
	/* HW VS */
	struct ac_arg streamout_config;
	struct ac_arg streamout_write_index;
	struct ac_arg streamout_offset[4];

	/* API TCS & TES */
	/* Layout of TCS outputs in the offchip buffer
	 * # 6 bits
	 *   [0:5] = the number of patches per threadgroup, max = NUM_PATCHES (40)
	 * # 6 bits
	 *   [6:11] = the number of output vertices per patch, max = 32
	 * # 20 bits
	 *   [12:31] = the offset of per patch attributes in the buffer in bytes.
	 *             max = NUM_PATCHES*32*32*16
	 */
	struct ac_arg tcs_offchip_layout;

	/* API TCS */
	/* Offsets where TCS outputs and TCS patch outputs live in LDS:
	 *   [0:15] = TCS output patch0 offset / 16, max = NUM_PATCHES * 32 * 32
	 *   [16:31] = TCS output patch0 offset for per-patch / 16
	 *             max = (NUM_PATCHES + 1) * 32*32
	 */
	struct ac_arg tcs_out_lds_offsets;
	/* Layout of TCS outputs / TES inputs:
	 *   [0:12] = stride between output patches in DW, num_outputs * num_vertices * 4
	 *            max = 32*32*4 + 32*4
	 *   [13:18] = gl_PatchVerticesIn, max = 32
	 *   [19:31] = high 13 bits of the 32-bit address of tessellation ring buffers
	 */
	struct ac_arg tcs_out_lds_layout;
	struct ac_arg tcs_offchip_offset;
	struct ac_arg tcs_factor_offset;

	/* API TES */
	struct ac_arg tes_offchip_addr;
	struct ac_arg tes_u;
	struct ac_arg tes_v;
	struct ac_arg tes_rel_patch_id;
	/* HW ES */
	struct ac_arg es2gs_offset;
	/* HW GS */
	/* On gfx10:
	 *  - bits 0..11: ordered_wave_id
	 *  - bits 12..20: number of vertices in group
	 *  - bits 22..30: number of primitives in group
	 */
	struct ac_arg gs_tg_info;
	/* API GS */
	struct ac_arg gs2vs_offset;
	struct ac_arg gs_wave_id; /* GFX6 */
	struct ac_arg gs_vtx_offset[6]; /* in dwords (GFX6) */
	struct ac_arg gs_vtx01_offset; /* in dwords (GFX9) */
	struct ac_arg gs_vtx23_offset; /* in dwords (GFX9) */
	struct ac_arg gs_vtx45_offset; /* in dwords (GFX9) */
	/* PS */
	struct ac_arg pos_fixed_pt;
	/* CS */
	struct ac_arg block_size;
	struct ac_arg cs_user_data;

	struct ac_llvm_compiler *compiler;

	/* Preloaded descriptors. */
	LLVMValueRef esgs_ring;
	LLVMValueRef gsvs_ring[4];
	LLVMValueRef tess_offchip_ring;

	LLVMValueRef invoc0_tess_factors[6]; /* outer[4], inner[2] */
	LLVMValueRef gs_next_vertex[4];
	LLVMValueRef gs_curprim_verts[4];
	LLVMValueRef gs_generated_prims[4];
	LLVMValueRef gs_ngg_emit;
	LLVMValueRef gs_ngg_scratch;
	LLVMValueRef postponed_kill;
	LLVMValueRef return_value;

	LLVMTypeRef voidt;
	LLVMTypeRef i1;
	LLVMTypeRef i8;
	LLVMTypeRef i32;
	LLVMTypeRef i64;
	LLVMTypeRef i128;
	LLVMTypeRef f32;
	LLVMTypeRef v2i32;
	LLVMTypeRef v4i32;
	LLVMTypeRef v4f32;
	LLVMTypeRef v8i32;

	LLVMValueRef i32_0;
	LLVMValueRef i32_1;
	LLVMValueRef i1false;
	LLVMValueRef i1true;
};

static inline struct si_shader_context *
si_shader_context_from_abi(struct ac_shader_abi *abi)
{
	struct si_shader_context *ctx = NULL;
	return container_of(abi, ctx, abi);
}

unsigned si_llvm_compile(LLVMModuleRef M, struct si_shader_binary *binary,
			 struct ac_llvm_compiler *compiler,
			 struct pipe_debug_callback *debug,
			 bool less_optimized, unsigned wave_size);

LLVMValueRef si_llvm_bound_index(struct si_shader_context *ctx,
				 LLVMValueRef index,
				 unsigned num);

void si_llvm_context_init(struct si_shader_context *ctx,
			  struct si_screen *sscreen,
			  struct ac_llvm_compiler *compiler,
			  unsigned wave_size);
void si_llvm_context_set_ir(struct si_shader_context *ctx,
			    struct si_shader *shader);

void si_llvm_create_func(struct si_shader_context *ctx, const char *name,
			 LLVMTypeRef *return_types, unsigned num_return_elems,
			 unsigned max_workgroup_size);

void si_llvm_dispose(struct si_shader_context *ctx);

void si_llvm_optimize_module(struct si_shader_context *ctx);

LLVMValueRef si_nir_load_input_tes(struct ac_shader_abi *abi,
				   LLVMTypeRef type,
				   LLVMValueRef vertex_index,
				   LLVMValueRef param_index,
				   unsigned const_index,
				   unsigned location,
				   unsigned driver_location,
				   unsigned component,
				   unsigned num_components,
				   bool is_patch,
				   bool is_compact,
				   bool load_input);
LLVMValueRef si_nir_lookup_interp_param(struct ac_shader_abi *abi,
					enum glsl_interp_mode interp,
					unsigned location);
LLVMValueRef si_get_sample_id(struct si_shader_context *ctx);
LLVMValueRef si_load_sampler_desc(struct si_shader_context *ctx,
				  LLVMValueRef list, LLVMValueRef index,
				  enum ac_descriptor_type type);
LLVMValueRef si_load_image_desc(struct si_shader_context *ctx,
				LLVMValueRef list, LLVMValueRef index,
				enum ac_descriptor_type desc_type,
				bool uses_store, bool bindless);
LLVMValueRef si_nir_emit_fbfetch(struct ac_shader_abi *abi);
void si_declare_compute_memory(struct si_shader_context *ctx);
LLVMValueRef si_get_primitive_id(struct si_shader_context *ctx,
				 unsigned swizzle);
void si_llvm_export_vs(struct si_shader_context *ctx,
		       struct si_shader_output_values *outputs,
		       unsigned noutput);
void si_emit_streamout_output(struct si_shader_context *ctx,
			      LLVMValueRef const *so_buffers,
			      LLVMValueRef const *so_write_offsets,
			      struct pipe_stream_output *stream_out,
			      struct si_shader_output_values *shader_out);

void si_llvm_load_input_vs(
	struct si_shader_context *ctx,
	unsigned input_index,
	LLVMValueRef out[4]);

bool si_nir_build_llvm(struct si_shader_context *ctx, struct nir_shader *nir);

LLVMValueRef si_unpack_param(struct si_shader_context *ctx,
			     struct ac_arg param, unsigned rshift,
			     unsigned bitwidth);
LLVMValueRef si_is_es_thread(struct si_shader_context *ctx);
LLVMValueRef si_is_gs_thread(struct si_shader_context *ctx);

void gfx10_emit_ngg_epilogue(struct ac_shader_abi *abi,
			     unsigned max_outputs,
			     LLVMValueRef *addrs);
void gfx10_ngg_gs_emit_vertex(struct si_shader_context *ctx,
			      unsigned stream,
			      LLVMValueRef *addrs);
void gfx10_ngg_gs_emit_prologue(struct si_shader_context *ctx);
void gfx10_ngg_gs_emit_epilogue(struct si_shader_context *ctx);
void gfx10_ngg_calculate_subgroup_info(struct si_shader *shader);

#endif

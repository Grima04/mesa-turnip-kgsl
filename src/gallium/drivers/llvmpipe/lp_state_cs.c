/**************************************************************************
 *
 * Copyright 2019 Red Hat.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **************************************************************************/
#include "util/u_memory.h"
#include "util/simple_list.h"
#include "util/os_time.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_parse.h"
#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_debug.h"
#include "gallivm/lp_bld_intr.h"
#include "gallivm/lp_bld_flow.h"
#include "gallivm/lp_bld_gather.h"
#include "gallivm/lp_bld_coro.h"
#include "lp_state_cs.h"
#include "lp_context.h"
#include "lp_debug.h"
#include "lp_state.h"
#include "lp_perf.h"
#include "lp_screen.h"
#include "lp_cs_tpool.h"

struct lp_cs_job_info {
   unsigned grid_size[3];
   unsigned block_size[3];
   struct lp_cs_exec *current;
};

static void
generate_compute(struct llvmpipe_context *lp,
                 struct lp_compute_shader *shader,
                 struct lp_compute_shader_variant *variant)
{
   struct gallivm_state *gallivm = variant->gallivm;
   char func_name[64], func_name_coro[64];
   LLVMTypeRef arg_types[13];
   LLVMTypeRef func_type, coro_func_type;
   LLVMTypeRef int32_type = LLVMInt32TypeInContext(gallivm->context);
   LLVMValueRef context_ptr;
   LLVMValueRef x_size_arg, y_size_arg, z_size_arg;
   LLVMValueRef grid_x_arg, grid_y_arg, grid_z_arg;
   LLVMValueRef grid_size_x_arg, grid_size_y_arg, grid_size_z_arg;
   LLVMValueRef thread_data_ptr;
   LLVMBasicBlockRef block;
   LLVMBuilderRef builder;
   LLVMValueRef function, coro;
   struct lp_type cs_type;
   unsigned i;

   /*
    * This function has two parts
    * a) setup the coroutine execution environment loop.
    * b) build the compute shader llvm for use inside the coroutine.
    */
   assert(lp_native_vector_width / 32 >= 4);

   memset(&cs_type, 0, sizeof cs_type);
   cs_type.floating = TRUE;      /* floating point values */
   cs_type.sign = TRUE;          /* values are signed */
   cs_type.norm = FALSE;         /* values are not limited to [0,1] or [-1,1] */
   cs_type.width = 32;           /* 32-bit float */
   cs_type.length = MIN2(lp_native_vector_width / 32, 16); /* n*4 elements per vector */
   snprintf(func_name, sizeof(func_name), "cs%u_variant%u",
            shader->no, variant->no);

   snprintf(func_name_coro, sizeof(func_name), "cs_co_%u_variant%u",
            shader->no, variant->no);

   arg_types[0] = variant->jit_cs_context_ptr_type;       /* context */
   arg_types[1] = int32_type;                          /* block_x_size */
   arg_types[2] = int32_type;                          /* block_y_size */
   arg_types[3] = int32_type;                          /* block_z_size */
   arg_types[4] = int32_type;                          /* grid_x */
   arg_types[5] = int32_type;                          /* grid_y */
   arg_types[6] = int32_type;                          /* grid_z */
   arg_types[7] = int32_type;                          /* grid_size_x */
   arg_types[8] = int32_type;                          /* grid_size_y */
   arg_types[9] = int32_type;                          /* grid_size_z */
   arg_types[10] = variant->jit_cs_thread_data_ptr_type;  /* per thread data */
   arg_types[11] = int32_type;
   arg_types[12] = int32_type;
   func_type = LLVMFunctionType(LLVMVoidTypeInContext(gallivm->context),
                                arg_types, ARRAY_SIZE(arg_types) - 2, 0);

   coro_func_type = LLVMFunctionType(LLVMPointerType(LLVMInt8TypeInContext(gallivm->context), 0),
                                     arg_types, ARRAY_SIZE(arg_types), 0);

   function = LLVMAddFunction(gallivm->module, func_name, func_type);
   LLVMSetFunctionCallConv(function, LLVMCCallConv);

   coro = LLVMAddFunction(gallivm->module, func_name_coro, coro_func_type);
   LLVMSetFunctionCallConv(coro, LLVMCCallConv);

   variant->function = function;

   for(i = 0; i < ARRAY_SIZE(arg_types); ++i) {
      if(LLVMGetTypeKind(arg_types[i]) == LLVMPointerTypeKind) {
         lp_add_function_attr(coro, i + 1, LP_FUNC_ATTR_NOALIAS);
         lp_add_function_attr(function, i + 1, LP_FUNC_ATTR_NOALIAS);
      }
   }

   context_ptr  = LLVMGetParam(function, 0);
   x_size_arg = LLVMGetParam(function, 1);
   y_size_arg = LLVMGetParam(function, 2);
   z_size_arg = LLVMGetParam(function, 3);
   grid_x_arg = LLVMGetParam(function, 4);
   grid_y_arg = LLVMGetParam(function, 5);
   grid_z_arg = LLVMGetParam(function, 6);
   grid_size_x_arg = LLVMGetParam(function, 7);
   grid_size_y_arg = LLVMGetParam(function, 8);
   grid_size_z_arg = LLVMGetParam(function, 9);
   thread_data_ptr  = LLVMGetParam(function, 10);

   lp_build_name(context_ptr, "context");
   lp_build_name(x_size_arg, "x_size");
   lp_build_name(y_size_arg, "y_size");
   lp_build_name(z_size_arg, "z_size");
   lp_build_name(grid_x_arg, "grid_x");
   lp_build_name(grid_y_arg, "grid_y");
   lp_build_name(grid_z_arg, "grid_z");
   lp_build_name(grid_size_x_arg, "grid_size_x");
   lp_build_name(grid_size_y_arg, "grid_size_y");
   lp_build_name(grid_size_z_arg, "grid_size_z");
   lp_build_name(thread_data_ptr, "thread_data");

   block = LLVMAppendBasicBlockInContext(gallivm->context, function, "entry");
   builder = gallivm->builder;
   assert(builder);
   LLVMPositionBuilderAtEnd(builder, block);

   struct lp_build_loop_state loop_state[4];
   LLVMValueRef num_x_loop;
   LLVMValueRef vec_length = lp_build_const_int32(gallivm, cs_type.length);
   num_x_loop = LLVMBuildAdd(gallivm->builder, x_size_arg, vec_length, "");
   num_x_loop = LLVMBuildSub(gallivm->builder, num_x_loop, lp_build_const_int32(gallivm, 1), "");
   num_x_loop = LLVMBuildUDiv(gallivm->builder, num_x_loop, vec_length, "");
   LLVMValueRef partials = LLVMBuildURem(gallivm->builder, x_size_arg, vec_length, "");

   LLVMValueRef coro_num_hdls = LLVMBuildMul(gallivm->builder, num_x_loop, y_size_arg, "");
   coro_num_hdls = LLVMBuildMul(gallivm->builder, coro_num_hdls, z_size_arg, "");

   LLVMTypeRef hdl_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(gallivm->context), 0);
   LLVMValueRef coro_hdls = LLVMBuildArrayAlloca(gallivm->builder, hdl_ptr_type, coro_num_hdls, "coro_hdls");

   unsigned end_coroutine = INT_MAX;

   /*
    * This is the main coroutine execution loop. It iterates over the dimensions
    * and calls the coroutine main entrypoint on the first pass, but in subsequent
    * passes it checks if the coroutine has completed and resumes it if not.
    */
   /* take x_width - round up to type.length width */
   lp_build_loop_begin(&loop_state[3], gallivm,
                       lp_build_const_int32(gallivm, 0)); /* coroutine reentry loop */
   lp_build_loop_begin(&loop_state[2], gallivm,
                       lp_build_const_int32(gallivm, 0)); /* z loop */
   lp_build_loop_begin(&loop_state[1], gallivm,
                       lp_build_const_int32(gallivm, 0)); /* y loop */
   lp_build_loop_begin(&loop_state[0], gallivm,
                       lp_build_const_int32(gallivm, 0)); /* x loop */
   {
      LLVMValueRef args[13];
      args[0] = context_ptr;
      args[1] = loop_state[0].counter;
      args[2] = loop_state[1].counter;
      args[3] = loop_state[2].counter;
      args[4] = grid_x_arg;
      args[5] = grid_y_arg;
      args[6] = grid_z_arg;
      args[7] = grid_size_x_arg;
      args[8] = grid_size_y_arg;
      args[9] = grid_size_z_arg;
      args[10] = thread_data_ptr;
      args[11] = num_x_loop;
      args[12] = partials;

      /* idx = (z * (size_x * size_y) + y * size_x + x */
      LLVMValueRef coro_hdl_idx = LLVMBuildMul(gallivm->builder, loop_state[2].counter,
                                               LLVMBuildMul(gallivm->builder, num_x_loop, y_size_arg, ""), "");
      coro_hdl_idx = LLVMBuildAdd(gallivm->builder, coro_hdl_idx,
                                  LLVMBuildMul(gallivm->builder, loop_state[1].counter,
                                               num_x_loop, ""), "");
      coro_hdl_idx = LLVMBuildAdd(gallivm->builder, coro_hdl_idx,
                                  loop_state[0].counter, "");

      LLVMValueRef coro_entry = LLVMBuildGEP(gallivm->builder, coro_hdls, &coro_hdl_idx, 1, "");

      LLVMValueRef coro_hdl = LLVMBuildLoad(gallivm->builder, coro_entry, "coro_hdl");

      struct lp_build_if_state ifstate;
      LLVMValueRef cmp = LLVMBuildICmp(gallivm->builder, LLVMIntEQ, loop_state[3].counter,
                                       lp_build_const_int32(gallivm, 0), "");
      /* first time here - call the coroutine function entry point */
      lp_build_if(&ifstate, gallivm, cmp);
      LLVMValueRef coro_ret = LLVMBuildCall(gallivm->builder, coro, args, 13, "");
      LLVMBuildStore(gallivm->builder, coro_ret, coro_entry);
      lp_build_else(&ifstate);
      /* subsequent calls for this invocation - check if done. */
      LLVMValueRef coro_done = lp_build_coro_done(gallivm, coro_hdl);
      struct lp_build_if_state ifstate2;
      lp_build_if(&ifstate2, gallivm, coro_done);
      /* if done destroy and force loop exit */
      lp_build_coro_destroy(gallivm, coro_hdl);
      lp_build_loop_force_set_counter(&loop_state[3], lp_build_const_int32(gallivm, end_coroutine - 1));
      lp_build_else(&ifstate2);
      /* otherwise resume the coroutine */
      lp_build_coro_resume(gallivm, coro_hdl);
      lp_build_endif(&ifstate2);
      lp_build_endif(&ifstate);
      lp_build_loop_force_reload_counter(&loop_state[3]);
   }
   lp_build_loop_end_cond(&loop_state[0],
                          num_x_loop,
                          NULL,  LLVMIntUGE);
   lp_build_loop_end_cond(&loop_state[1],
                          y_size_arg,
                          NULL,  LLVMIntUGE);
   lp_build_loop_end_cond(&loop_state[2],
                          z_size_arg,
                          NULL,  LLVMIntUGE);
   lp_build_loop_end_cond(&loop_state[3],
                          lp_build_const_int32(gallivm, end_coroutine),
                          NULL, LLVMIntEQ);
   LLVMBuildRetVoid(builder);

   /* This is stage (b) - generate the compute shader code inside the coroutine. */
   context_ptr  = LLVMGetParam(coro, 0);
   x_size_arg = LLVMGetParam(coro, 1);
   y_size_arg = LLVMGetParam(coro, 2);
   z_size_arg = LLVMGetParam(coro, 3);
   grid_x_arg = LLVMGetParam(coro, 4);
   grid_y_arg = LLVMGetParam(coro, 5);
   grid_z_arg = LLVMGetParam(coro, 6);
   grid_size_x_arg = LLVMGetParam(coro, 7);
   grid_size_y_arg = LLVMGetParam(coro, 8);
   grid_size_z_arg = LLVMGetParam(coro, 9);
   thread_data_ptr  = LLVMGetParam(coro, 10);
   num_x_loop = LLVMGetParam(coro, 11);
   partials = LLVMGetParam(coro, 12);
   block = LLVMAppendBasicBlockInContext(gallivm->context, coro, "entry");
   LLVMPositionBuilderAtEnd(builder, block);
   {
      const struct tgsi_token *tokens = shader->base.tokens;
      LLVMValueRef consts_ptr, num_consts_ptr;
      LLVMValueRef ssbo_ptr, num_ssbo_ptr;
      LLVMValueRef shared_ptr;
      struct lp_build_mask_context mask;
      struct lp_bld_tgsi_system_values system_values;

      memset(&system_values, 0, sizeof(system_values));
      consts_ptr = lp_jit_cs_context_constants(gallivm, context_ptr);
      num_consts_ptr = lp_jit_cs_context_num_constants(gallivm, context_ptr);
      ssbo_ptr = lp_jit_cs_context_ssbos(gallivm, context_ptr);
      num_ssbo_ptr = lp_jit_cs_context_num_ssbos(gallivm, context_ptr);
      shared_ptr = lp_jit_cs_thread_data_shared(gallivm, thread_data_ptr);

      /* these are coroutine entrypoint necessities */
      LLVMValueRef coro_id = lp_build_coro_id(gallivm);
      LLVMValueRef coro_hdl = lp_build_coro_begin_alloc_mem(gallivm, coro_id);

      LLVMValueRef has_partials = LLVMBuildICmp(gallivm->builder, LLVMIntNE, partials, lp_build_const_int32(gallivm, 0), "");
      LLVMValueRef tid_vals[3];
      LLVMValueRef tids_x[LP_MAX_VECTOR_LENGTH], tids_y[LP_MAX_VECTOR_LENGTH], tids_z[LP_MAX_VECTOR_LENGTH];
      LLVMValueRef base_val = LLVMBuildMul(gallivm->builder, x_size_arg, vec_length, "");
      for (i = 0; i < cs_type.length; i++) {
         tids_x[i] = LLVMBuildAdd(gallivm->builder, base_val, lp_build_const_int32(gallivm, i), "");
         tids_y[i] = y_size_arg;
         tids_z[i] = z_size_arg;
      }
      tid_vals[0] = lp_build_gather_values(gallivm, tids_x, cs_type.length);
      tid_vals[1] = lp_build_gather_values(gallivm, tids_y, cs_type.length);
      tid_vals[2] = lp_build_gather_values(gallivm, tids_z, cs_type.length);
      system_values.thread_id = LLVMGetUndef(LLVMArrayType(LLVMVectorType(int32_type, cs_type.length), 3));
      for (i = 0; i < 3; i++)
         system_values.thread_id = LLVMBuildInsertValue(builder, system_values.thread_id, tid_vals[i], i, "");

      LLVMValueRef gtids[3] = { grid_x_arg, grid_y_arg, grid_z_arg };
      system_values.block_id = LLVMGetUndef(LLVMVectorType(int32_type, 3));
      for (i = 0; i < 3; i++)
         system_values.block_id = LLVMBuildInsertElement(builder, system_values.block_id, gtids[i], lp_build_const_int32(gallivm, i), "");

      LLVMValueRef gstids[3] = { grid_size_x_arg, grid_size_y_arg, grid_size_z_arg };
      system_values.grid_size = LLVMGetUndef(LLVMVectorType(int32_type, 3));
      for (i = 0; i < 3; i++)
         system_values.grid_size = LLVMBuildInsertElement(builder, system_values.grid_size, gstids[i], lp_build_const_int32(gallivm, i), "");

      LLVMValueRef last_x_loop = LLVMBuildICmp(gallivm->builder, LLVMIntEQ, x_size_arg, LLVMBuildSub(gallivm->builder, num_x_loop, lp_build_const_int32(gallivm, 1), ""), "");
      LLVMValueRef use_partial_mask = LLVMBuildAnd(gallivm->builder, last_x_loop, has_partials, "");
      struct lp_build_if_state if_state;
      LLVMValueRef mask_val = lp_build_alloca(gallivm, LLVMVectorType(int32_type, cs_type.length), "mask");
      LLVMValueRef full_mask_val = lp_build_const_int_vec(gallivm, cs_type, ~0);
      LLVMBuildStore(gallivm->builder, full_mask_val, mask_val);

      lp_build_if(&if_state, gallivm, use_partial_mask);
      struct lp_build_loop_state mask_loop_state;
      lp_build_loop_begin(&mask_loop_state, gallivm, partials);
      LLVMValueRef tmask_val = LLVMBuildLoad(gallivm->builder, mask_val, "");
      tmask_val = LLVMBuildInsertElement(gallivm->builder, tmask_val, lp_build_const_int32(gallivm, 0), mask_loop_state.counter, "");
      LLVMBuildStore(gallivm->builder, tmask_val, mask_val);
      lp_build_loop_end_cond(&mask_loop_state, vec_length, NULL, LLVMIntUGE);
      lp_build_endif(&if_state);

      mask_val = LLVMBuildLoad(gallivm->builder, mask_val, "");
      lp_build_mask_begin(&mask, gallivm, cs_type, mask_val);

      struct lp_build_coro_suspend_info coro_info;

      LLVMBasicBlockRef sus_block = LLVMAppendBasicBlockInContext(gallivm->context, coro, "suspend");
      LLVMBasicBlockRef clean_block = LLVMAppendBasicBlockInContext(gallivm->context, coro, "cleanup");

      coro_info.suspend = sus_block;
      coro_info.cleanup = clean_block;

      struct lp_build_tgsi_params params;
      memset(&params, 0, sizeof(params));

      params.type = cs_type;
      params.mask = &mask;
      params.consts_ptr = consts_ptr;
      params.const_sizes_ptr = num_consts_ptr;
      params.system_values = &system_values;
      params.context_ptr = context_ptr;
      params.info = &shader->info.base;
      params.ssbo_ptr = ssbo_ptr;
      params.ssbo_sizes_ptr = num_ssbo_ptr;
      params.shared_ptr = shared_ptr;
      params.coro = &coro_info;

      lp_build_tgsi_soa(gallivm, tokens, &params, NULL);

      mask_val = lp_build_mask_end(&mask);

      lp_build_coro_suspend_switch(gallivm, &coro_info, NULL, true);
      LLVMPositionBuilderAtEnd(builder, clean_block);

      lp_build_coro_free_mem(gallivm, coro_id, coro_hdl);

      LLVMBuildBr(builder, sus_block);
      LLVMPositionBuilderAtEnd(builder, sus_block);

      lp_build_coro_end(gallivm, coro_hdl);
      LLVMBuildRet(builder, coro_hdl);
   }

   gallivm_verify_function(gallivm, coro);
   gallivm_verify_function(gallivm, function);
}

static void *
llvmpipe_create_compute_state(struct pipe_context *pipe,
                                     const struct pipe_compute_state *templ)
{
   struct lp_compute_shader *shader;

   shader = CALLOC_STRUCT(lp_compute_shader);
   if (!shader)
      return NULL;

   assert(templ->ir_type == PIPE_SHADER_IR_TGSI);
   shader->base.tokens = tgsi_dup_tokens(templ->prog);

   lp_build_tgsi_info(shader->base.tokens, &shader->info);
   make_empty_list(&shader->variants);

   return shader;
}

static void
llvmpipe_bind_compute_state(struct pipe_context *pipe,
                            void *cs)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);

   if (llvmpipe->cs == cs)
      return;

   llvmpipe->cs = (struct lp_compute_shader *)cs;
   llvmpipe->cs_dirty |= LP_CSNEW_CS;
}

/**
 * Remove shader variant from two lists: the shader's variant list
 * and the context's variant list.
 */
static void
llvmpipe_remove_cs_shader_variant(struct llvmpipe_context *lp,
                                  struct lp_compute_shader_variant *variant)
{
   if ((LP_DEBUG & DEBUG_CS) || (gallivm_debug & GALLIVM_DEBUG_IR)) {
      debug_printf("llvmpipe: del cs #%u var %u v created %u v cached %u "
                   "v total cached %u inst %u total inst %u\n",
                   variant->shader->no, variant->no,
                   variant->shader->variants_created,
                   variant->shader->variants_cached,
                   lp->nr_cs_variants, variant->nr_instrs, lp->nr_cs_instrs);
   }

   gallivm_destroy(variant->gallivm);

   /* remove from shader's list */
   remove_from_list(&variant->list_item_local);
   variant->shader->variants_cached--;

   /* remove from context's list */
   remove_from_list(&variant->list_item_global);
   lp->nr_fs_variants--;
   lp->nr_fs_instrs -= variant->nr_instrs;

   FREE(variant);
}

static void
llvmpipe_delete_compute_state(struct pipe_context *pipe,
                              void *cs)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_compute_shader *shader = cs;
   struct lp_cs_variant_list_item *li;

   /* Delete all the variants */
   li = first_elem(&shader->variants);
   while(!at_end(&shader->variants, li)) {
      struct lp_cs_variant_list_item *next = next_elem(li);
      llvmpipe_remove_cs_shader_variant(llvmpipe, li->base);
      li = next;
   }
   tgsi_free_tokens(shader->base.tokens);
   FREE(shader);
}

static void
make_variant_key(struct llvmpipe_context *lp,
                 struct lp_compute_shader *shader,
                 struct lp_compute_shader_variant_key *key)
{
   memset(key, 0, shader->variant_key_size);
}

static void
dump_cs_variant_key(const struct lp_compute_shader_variant_key *key)
{
   debug_printf("cs variant %p:\n", (void *) key);
}

static void
lp_debug_cs_variant(const struct lp_compute_shader_variant *variant)
{
   debug_printf("llvmpipe: Compute shader #%u variant #%u:\n",
                variant->shader->no, variant->no);
   tgsi_dump(variant->shader->base.tokens, 0);
   dump_cs_variant_key(&variant->key);
   debug_printf("\n");
}

static struct lp_compute_shader_variant *
generate_variant(struct llvmpipe_context *lp,
                 struct lp_compute_shader *shader,
                 const struct lp_compute_shader_variant_key *key)
{
   struct lp_compute_shader_variant *variant;
   char module_name[64];

   variant = CALLOC_STRUCT(lp_compute_shader_variant);
   if (!variant)
      return NULL;

   snprintf(module_name, sizeof(module_name), "cs%u_variant%u",
            shader->no, shader->variants_created);

   variant->gallivm = gallivm_create(module_name, lp->context);
   if (!variant->gallivm) {
      FREE(variant);
      return NULL;
   }

   variant->shader = shader;
   variant->list_item_global.base = variant;
   variant->list_item_local.base = variant;
   variant->no = shader->variants_created++;

   memcpy(&variant->key, key, shader->variant_key_size);

   if ((LP_DEBUG & DEBUG_CS) || (gallivm_debug & GALLIVM_DEBUG_IR)) {
      lp_debug_cs_variant(variant);
   }

   lp_jit_init_cs_types(variant);

   generate_compute(lp, shader, variant);

   gallivm_compile_module(variant->gallivm);

   variant->nr_instrs += lp_build_count_ir_module(variant->gallivm->module);

   variant->jit_function = (lp_jit_cs_func)gallivm_jit_function(variant->gallivm, variant->function);

   gallivm_free_ir(variant->gallivm);
   return variant;
}

static void
lp_cs_ctx_set_cs_variant( struct lp_cs_context *csctx,
                          struct lp_compute_shader_variant *variant)
{
   csctx->cs.current.variant = variant;
}

static void
llvmpipe_update_cs(struct llvmpipe_context *lp)
{
   struct lp_compute_shader *shader = lp->cs;

   struct lp_compute_shader_variant_key key;
   struct lp_compute_shader_variant *variant = NULL;
   struct lp_cs_variant_list_item *li;

   make_variant_key(lp, shader, &key);

   /* Search the variants for one which matches the key */
   li = first_elem(&shader->variants);
   while(!at_end(&shader->variants, li)) {
      if(memcmp(&li->base->key, &key, shader->variant_key_size) == 0) {
         variant = li->base;
         break;
      }
      li = next_elem(li);
   }

   if (variant) {
      /* Move this variant to the head of the list to implement LRU
       * deletion of shader's when we have too many.
       */
      move_to_head(&lp->cs_variants_list, &variant->list_item_global);
   }
   else {
      /* variant not found, create it now */
      int64_t t0, t1, dt;
      unsigned i;
      unsigned variants_to_cull;

      if (LP_DEBUG & DEBUG_CS) {
         debug_printf("%u variants,\t%u instrs,\t%u instrs/variant\n",
                      lp->nr_cs_variants,
                      lp->nr_cs_instrs,
                      lp->nr_cs_variants ? lp->nr_cs_instrs / lp->nr_cs_variants : 0);
      }

      /* First, check if we've exceeded the max number of shader variants.
       * If so, free 6.25% of them (the least recently used ones).
       */
      variants_to_cull = lp->nr_cs_variants >= LP_MAX_SHADER_VARIANTS ? LP_MAX_SHADER_VARIANTS / 16 : 0;

      if (variants_to_cull ||
          lp->nr_cs_instrs >= LP_MAX_SHADER_INSTRUCTIONS) {
         if (gallivm_debug & GALLIVM_DEBUG_PERF) {
            debug_printf("Evicting CS: %u cs variants,\t%u total variants,"
                         "\t%u instrs,\t%u instrs/variant\n",
                         shader->variants_cached,
                         lp->nr_cs_variants, lp->nr_cs_instrs,
                         lp->nr_cs_instrs / lp->nr_cs_variants);
         }

         /*
          * We need to re-check lp->nr_cs_variants because an arbitrarliy large
          * number of shader variants (potentially all of them) could be
          * pending for destruction on flush.
          */

         for (i = 0; i < variants_to_cull || lp->nr_cs_instrs >= LP_MAX_SHADER_INSTRUCTIONS; i++) {
            struct lp_cs_variant_list_item *item;
            if (is_empty_list(&lp->cs_variants_list)) {
               break;
            }
            item = last_elem(&lp->cs_variants_list);
            assert(item);
            assert(item->base);
            llvmpipe_remove_cs_shader_variant(lp, item->base);
         }
      }
      /*
       * Generate the new variant.
       */
      t0 = os_time_get();
      variant = generate_variant(lp, shader, &key);
      t1 = os_time_get();
      dt = t1 - t0;
      LP_COUNT_ADD(llvm_compile_time, dt);
      LP_COUNT_ADD(nr_llvm_compiles, 2);  /* emit vs. omit in/out test */

      /* Put the new variant into the list */
      if (variant) {
         insert_at_head(&shader->variants, &variant->list_item_local);
         insert_at_head(&lp->cs_variants_list, &variant->list_item_global);
         lp->nr_cs_variants++;
         lp->nr_cs_instrs += variant->nr_instrs;
         shader->variants_cached++;
      }
   }
   /* Bind this variant */
   lp_cs_ctx_set_cs_variant(lp->csctx, variant);
}

static void
lp_csctx_set_cs_constants(struct lp_cs_context *csctx,
                          unsigned num,
                          struct pipe_constant_buffer *buffers)
{
   unsigned i;

   LP_DBG(DEBUG_SETUP, "%s %p\n", __FUNCTION__, (void *) buffers);

   assert(num <= ARRAY_SIZE(csctx->constants));

   for (i = 0; i < num; ++i) {
      util_copy_constant_buffer(&csctx->constants[i].current, &buffers[i]);
   }
   for (; i < ARRAY_SIZE(csctx->constants); i++) {
      util_copy_constant_buffer(&csctx->constants[i].current, NULL);
   }
}

static void
update_csctx_consts(struct llvmpipe_context *llvmpipe)
{
   struct lp_cs_context *csctx = llvmpipe->csctx;
   int i;

   for (i = 0; i < ARRAY_SIZE(csctx->constants); ++i) {
      struct pipe_resource *buffer = csctx->constants[i].current.buffer;
      const ubyte *current_data = NULL;

      if (buffer) {
         /* resource buffer */
         current_data = (ubyte *) llvmpipe_resource_data(buffer);
      }
      else if (csctx->constants[i].current.user_buffer) {
         /* user-space buffer */
         current_data = (ubyte *) csctx->constants[i].current.user_buffer;
      }

      if (current_data) {
         current_data += csctx->constants[i].current.buffer_offset;

         csctx->cs.current.jit_context.constants[i] = (const float *)current_data;
         csctx->cs.current.jit_context.num_constants[i] = csctx->constants[i].current.buffer_size;
      } else {
         csctx->cs.current.jit_context.constants[i] = NULL;
         csctx->cs.current.jit_context.num_constants[i] = 0;
      }
   }
}

static void
llvmpipe_cs_update_derived(struct llvmpipe_context *llvmpipe)
{
   if (llvmpipe->cs_dirty & (LP_CSNEW_CS))
      llvmpipe_update_cs(llvmpipe);

   if (llvmpipe->cs_dirty & LP_CSNEW_CONSTANTS) {
      lp_csctx_set_cs_constants(llvmpipe->csctx,
                                ARRAY_SIZE(llvmpipe->constants[PIPE_SHADER_COMPUTE]),
                                llvmpipe->constants[PIPE_SHADER_COMPUTE]);
      update_csctx_consts(llvmpipe);
   }

   llvmpipe->cs_dirty = 0;
}

static void
cs_exec_fn(void *init_data, int iter_idx, struct lp_cs_local_mem *lmem)
{
   struct lp_cs_job_info *job_info = init_data;
   struct lp_jit_cs_thread_data thread_data;

   memset(&thread_data, 0, sizeof(thread_data));

   unsigned grid_z = iter_idx / (job_info->grid_size[0] * job_info->grid_size[1]);
   unsigned grid_y = (iter_idx - (grid_z * (job_info->grid_size[0] * job_info->grid_size[1]))) / job_info->grid_size[0];
   unsigned grid_x = (iter_idx - (grid_z * (job_info->grid_size[0] * job_info->grid_size[1])) - (grid_y * job_info->grid_size[0]));
   struct lp_compute_shader_variant *variant = job_info->current->variant;
   variant->jit_function(&job_info->current->jit_context,
                         job_info->block_size[0], job_info->block_size[1], job_info->block_size[2],
                         grid_x, grid_y, grid_z,
                         job_info->grid_size[0], job_info->grid_size[1], job_info->grid_size[2],
                         &thread_data);
}

static void
fill_grid_size(struct pipe_context *pipe,
               const struct pipe_grid_info *info,
               uint32_t grid_size[3])
{
   struct pipe_transfer *transfer;
   uint32_t *params;
   if (!info->indirect) {
      grid_size[0] = info->grid[0];
      grid_size[1] = info->grid[1];
      grid_size[2] = info->grid[2];
      return;
   }
   params = pipe_buffer_map_range(pipe, info->indirect,
                                  info->indirect_offset,
                                  3 * sizeof(uint32_t),
                                  PIPE_TRANSFER_READ,
                                  &transfer);

   if (!transfer)
      return;

   grid_size[0] = params[0];
   grid_size[1] = params[1];
   grid_size[2] = params[2];
   pipe_buffer_unmap(pipe, transfer);
}

static void llvmpipe_launch_grid(struct pipe_context *pipe,
                                 const struct pipe_grid_info *info)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct llvmpipe_screen *screen = llvmpipe_screen(pipe->screen);
   struct lp_cs_job_info job_info;

   memset(&job_info, 0, sizeof(job_info));

   llvmpipe_cs_update_derived(llvmpipe);

   fill_grid_size(pipe, info, job_info.grid_size);

   job_info.block_size[0] = info->block[0];
   job_info.block_size[1] = info->block[1];
   job_info.block_size[2] = info->block[2];
   job_info.current = &llvmpipe->csctx->cs.current;

   int num_tasks = job_info.grid_size[2] * job_info.grid_size[1] * job_info.grid_size[0];
   if (num_tasks) {
      struct lp_cs_tpool_task *task;
      mtx_lock(&screen->cs_mutex);
      task = lp_cs_tpool_queue_task(screen->cs_tpool, cs_exec_fn, &job_info, num_tasks);

      lp_cs_tpool_wait_for_task(screen->cs_tpool, &task);
      mtx_unlock(&screen->cs_mutex);
   }
   llvmpipe->pipeline_statistics.cs_invocations += num_tasks * info->block[0] * info->block[1] * info->block[2];
}

void
llvmpipe_init_compute_funcs(struct llvmpipe_context *llvmpipe)
{
   llvmpipe->pipe.create_compute_state = llvmpipe_create_compute_state;
   llvmpipe->pipe.bind_compute_state = llvmpipe_bind_compute_state;
   llvmpipe->pipe.delete_compute_state = llvmpipe_delete_compute_state;
   llvmpipe->pipe.launch_grid = llvmpipe_launch_grid;
}

void
lp_csctx_destroy(struct lp_cs_context *csctx)
{
   unsigned i;
   for (i = 0; i < ARRAY_SIZE(csctx->constants); i++) {
      pipe_resource_reference(&csctx->constants[i].current.buffer, NULL);
   }
   FREE(csctx);
}

struct lp_cs_context *lp_csctx_create(struct pipe_context *pipe)
{
   struct lp_cs_context *csctx;

   csctx = CALLOC_STRUCT(lp_cs_context);
   if (!csctx)
      return NULL;

   csctx->pipe = pipe;
   return csctx;
}

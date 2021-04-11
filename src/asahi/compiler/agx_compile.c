/*
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2020 Collabora Ltd.
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

#include "main/mtypes.h"
#include "compiler/nir_types.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_debug.h"
#include "agx_compile.h"
#include "agx_compiler.h"
#include "agx_builder.h"

static const struct debug_named_value agx_debug_options[] = {
   {"msgs",      AGX_DBG_MSGS,		"Print debug messages"},
   {"shaders",   AGX_DBG_SHADERS,	"Dump shaders in NIR and AIR"},
   {"shaderdb",  AGX_DBG_SHADERDB,	"Print statistics"},
   {"verbose",   AGX_DBG_VERBOSE,	"Disassemble verbosely"},
   {"internal",  AGX_DBG_INTERNAL,	"Dump even internal shaders"},
   DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(agx_debug, "AGX_MESA_DEBUG", agx_debug_options, 0)

int agx_debug = 0;

#define DBG(fmt, ...) \
   do { if (agx_debug & AGX_DBG_MSGS) \
      fprintf(stderr, "%s:%d: "fmt, \
            __FUNCTION__, __LINE__, ##__VA_ARGS__); } while (0)

static void
agx_emit_load_const(agx_builder *b, nir_load_const_instr *instr)
{
   /* Ensure we've been scalarized and bit size lowered */
   unsigned bit_size = instr->def.bit_size;
   assert(instr->def.num_components == 1);
   assert(bit_size == 16 || bit_size == 32);

   /* Emit move, later passes can inline/push if useful */
   agx_mov_imm_to(b,
                  agx_get_index(instr->def.index, agx_size_for_bits(bit_size)),
                  nir_const_value_as_uint(instr->value[0], bit_size));
}

static void
agx_emit_load_attr(agx_builder *b, nir_intrinsic_instr *instr)
{
   unreachable("stub");
}

static void
agx_emit_load_vary(agx_builder *b, nir_intrinsic_instr *instr)
{
   unreachable("stub");
}

static void
agx_emit_store_vary(agx_builder *b, nir_intrinsic_instr *instr)
{
   unreachable("stub");
}

static void
agx_emit_fragment_out(agx_builder *b, nir_intrinsic_instr *instr)
{
   unreachable("stub");
}

static void
agx_emit_intrinsic(agx_builder *b, nir_intrinsic_instr *instr)
{
  agx_index dst = nir_intrinsic_infos[instr->intrinsic].has_dest ?
     agx_dest_index(&instr->dest) : agx_null();
  gl_shader_stage stage = b->shader->stage;

  switch (instr->intrinsic) {
  case nir_intrinsic_load_barycentric_pixel:
  case nir_intrinsic_load_barycentric_centroid:
  case nir_intrinsic_load_barycentric_sample:
  case nir_intrinsic_load_barycentric_at_sample:
  case nir_intrinsic_load_barycentric_at_offset:
     /* handled later via load_vary */
     break;
  case nir_intrinsic_load_interpolated_input:
  case nir_intrinsic_load_input:
     if (stage == MESA_SHADER_FRAGMENT)
        agx_emit_load_vary(b, instr);
     else if (stage == MESA_SHADER_VERTEX)
        agx_emit_load_attr(b, instr);
     else
        unreachable("Unsupported shader stage");
     break;

  case nir_intrinsic_store_output:
     if (stage == MESA_SHADER_FRAGMENT)
        agx_emit_fragment_out(b, instr);
     else if (stage == MESA_SHADER_VERTEX)
        agx_emit_store_vary(b, instr);
     else
        unreachable("Unsupported shader stage");
     break;

  default:
       fprintf(stderr, "Unhandled intrinsic %s\n", nir_intrinsic_infos[instr->intrinsic].name);
       assert(0);
  }
}

static agx_instr *
agx_emit_alu(agx_builder *b, nir_alu_instr *instr)
{
   unreachable("stub");
}

static void
agx_emit_tex(agx_builder *b, nir_tex_instr *instr)
{
   unreachable("stub");
}

static void
agx_emit_jump(agx_builder *b, nir_jump_instr *instr)
{
   unreachable("stub");
}

static void
agx_emit_instr(agx_builder *b, struct nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_load_const:
      agx_emit_load_const(b, nir_instr_as_load_const(instr));
      break;

   case nir_instr_type_intrinsic:
      agx_emit_intrinsic(b, nir_instr_as_intrinsic(instr));
      break;

   case nir_instr_type_alu:
      agx_emit_alu(b, nir_instr_as_alu(instr));
      break;

   case nir_instr_type_tex:
      agx_emit_tex(b, nir_instr_as_tex(instr));
      break;

   case nir_instr_type_jump:
      agx_emit_jump(b, nir_instr_as_jump(instr));
      break;

   default:
      unreachable("should've been lowered");
   }
}

static agx_block *
agx_create_block(agx_context *ctx)
{
   agx_block *blk = rzalloc(ctx, agx_block);

   blk->predecessors = _mesa_set_create(blk,
         _mesa_hash_pointer, _mesa_key_pointer_equal);

   return blk;
}

static agx_block *
emit_block(agx_context *ctx, nir_block *block)
{
   agx_block *blk = agx_create_block(ctx);
   list_addtail(&blk->link, &ctx->blocks);
   list_inithead(&blk->instructions);

   agx_builder _b = agx_init_builder(ctx, agx_after_block(blk));

   nir_foreach_instr(instr, block) {
      agx_emit_instr(&_b, instr);
   }

   return blk;
}

static void
emit_if(agx_context *ctx, nir_if *nif)
{
   unreachable("if-statements todo");
}

static void
emit_loop(agx_context *ctx, nir_loop *nloop)
{
   unreachable("loops todo");
}

static agx_block *
emit_cf_list(agx_context *ctx, struct exec_list *list)
{
   agx_block *start_block = NULL;

   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block: {
         agx_block *block = emit_block(ctx, nir_cf_node_as_block(node));

         if (!start_block)
            start_block = block;

         break;
      }

      case nir_cf_node_if:
         emit_if(ctx, nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         emit_loop(ctx, nir_cf_node_as_loop(node));
         break;

      default:
         unreachable("Unknown control flow");
      }
   }

   return start_block;
}

static void
agx_print_stats(agx_context *ctx, unsigned size, FILE *fp)
{
   unsigned nr_ins = 0, nr_bytes = 0, nr_threads = 1;

   /* TODO */
   fprintf(stderr, "%s shader: %u inst, %u bytes, %u threads, %u loops,"
           "%u:%u spills:fills\n",
           ctx->nir->info.label ?: "",
           nr_ins, nr_bytes, nr_threads, ctx->loop_count,
           ctx->spills, ctx->fills);
}

static int
glsl_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void
agx_optimize_nir(nir_shader *nir)
{
   bool progress;

   nir_lower_idiv_options idiv_options = {
      .imprecise_32bit_lowering = true,
      .allow_fp16 = true,
   };

   NIR_PASS_V(nir, nir_lower_regs_to_ssa);
   NIR_PASS_V(nir, nir_lower_int64);
   NIR_PASS_V(nir, nir_lower_idiv, &idiv_options);
   NIR_PASS_V(nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS_V(nir, nir_lower_load_const_to_scalar);
   NIR_PASS_V(nir, nir_lower_flrp, 16 | 32 | 64, false);

   do {
      progress = false;

      NIR_PASS(progress, nir, nir_lower_var_copies);
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);

      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_peephole_select, 64, false, true);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);

      NIR_PASS(progress, nir, nir_opt_undef);
      NIR_PASS(progress, nir, nir_lower_undef_to_zero);

      NIR_PASS(progress, nir, nir_opt_loop_unroll,
               nir_var_shader_in |
               nir_var_shader_out |
               nir_var_function_temp);
   } while (progress);

   NIR_PASS_V(nir, nir_opt_algebraic_late);
   NIR_PASS_V(nir, nir_opt_constant_folding);
   NIR_PASS_V(nir, nir_copy_prop);
   NIR_PASS_V(nir, nir_opt_dce);
   NIR_PASS_V(nir, nir_opt_cse);
   NIR_PASS_V(nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS_V(nir, nir_lower_load_const_to_scalar);

   /* Cleanup optimizations */
   nir_move_options move_all =
      nir_move_const_undef | nir_move_load_ubo | nir_move_load_input |
      nir_move_comparisons | nir_move_copies | nir_move_load_ssbo;

   NIR_PASS_V(nir, nir_opt_sink, move_all);
   NIR_PASS_V(nir, nir_opt_move, move_all);
}

/* ABI: position first, then user, then psiz */
static void
agx_remap_varyings(nir_shader *nir)
{
   unsigned base = 0;

   nir_variable *pos = nir_find_variable_with_location(nir, nir_var_shader_out, VARYING_SLOT_POS);
   if (pos) {
      pos->data.driver_location = base;
      base += 4;
   }

   nir_foreach_shader_out_variable(var, nir) {
      unsigned loc = var->data.location;

      if(loc == VARYING_SLOT_POS || loc == VARYING_SLOT_PSIZ) {
         continue;
      }

      var->data.driver_location = base;
      base += 4;
   }

   nir_variable *psiz = nir_find_variable_with_location(nir, nir_var_shader_out, VARYING_SLOT_PSIZ);
   if (psiz) {
      psiz->data.driver_location = base;
      base += 1;
   }
}

void
agx_compile_shader_nir(nir_shader *nir,
      struct agx_shader_key *key,
      struct util_dynarray *binary,
      struct agx_shader_info *out)
{
   agx_debug = debug_get_option_agx_debug();

   agx_context *ctx = rzalloc(NULL, agx_context);
   ctx->nir = nir;
   ctx->out = out;
   ctx->key = key;
   ctx->stage = nir->info.stage;
   list_inithead(&ctx->blocks);

   NIR_PASS_V(nir, nir_lower_vars_to_ssa);

   /* Lower large arrays to scratch and small arrays to csel */
   NIR_PASS_V(nir, nir_lower_vars_to_scratch, nir_var_function_temp, 16,
         glsl_get_natural_size_align_bytes);
   NIR_PASS_V(nir, nir_lower_indirect_derefs, nir_var_function_temp, ~0);

   if (ctx->stage == MESA_SHADER_VERTEX)
      agx_remap_varyings(nir);

   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_lower_global_vars_to_local);
   NIR_PASS_V(nir, nir_lower_var_copies);
   NIR_PASS_V(nir, nir_lower_vars_to_ssa);
   NIR_PASS_V(nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
         glsl_type_size, 0);
   if (ctx->stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS_V(nir, nir_lower_mediump_io,
            nir_var_shader_in | nir_var_shader_out, ~0, false);
   }
   NIR_PASS_V(nir, nir_lower_ssbo);

   /* Varying output is scalar, other I/O is vector */
   if (ctx->stage == MESA_SHADER_VERTEX) {
      NIR_PASS_V(nir, nir_lower_io_to_scalar, nir_var_shader_out);
   }

   nir_lower_tex_options lower_tex_options = {
      .lower_txs_lod = true,
      .lower_txp = ~0,
   };

   NIR_PASS_V(nir, nir_lower_tex, &lower_tex_options);

   agx_optimize_nir(nir);

   bool skip_internal = nir->info.internal;
   skip_internal &= !(agx_debug & AGX_DBG_INTERNAL);

   if (agx_debug & AGX_DBG_SHADERS && !skip_internal) {
      nir_print_shader(nir, stdout);
   }

   nir_foreach_function(func, nir) {
      if (!func->impl)
         continue;

      ctx->alloc += func->impl->ssa_alloc;
      emit_cf_list(ctx, &func->impl->body);
      break; /* TODO: Multi-function shaders */
   }

   unsigned block_source_count = 0;

   /* Name blocks now that we're done emitting so the order is consistent */
   agx_foreach_block(ctx, block)
      block->name = block_source_count++;

   if (agx_debug & AGX_DBG_SHADERS && !skip_internal)
      agx_print_shader(ctx, stdout);

   if ((agx_debug & AGX_DBG_SHADERDB) && !skip_internal)
      agx_print_stats(ctx, binary->size, stderr);

   ralloc_free(ctx);
}

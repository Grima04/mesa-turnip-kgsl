/*
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
 *
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#include "main/mtypes.h"
#include "compiler/glsl/glsl_to_nir.h"
#include "compiler/nir_types.h"
#include "main/imports.h"
#include "compiler/nir/nir_builder.h"

#include "disassemble.h"
#include "bifrost_compile.h"
#include "compiler.h"
#include "bi_quirks.h"
#include "bi_print.h"

static bi_block *emit_cf_list(bi_context *ctx, struct exec_list *list);
static bi_instruction *bi_emit_branch(bi_context *ctx);
static void bi_block_add_successor(bi_block *block, bi_block *successor);

static void
emit_jump(bi_context *ctx, nir_jump_instr *instr)
{
        bi_instruction *branch = bi_emit_branch(ctx);

        switch (instr->type) {
        case nir_jump_break:
                branch->branch.target = ctx->break_block;
                break;
        case nir_jump_continue:
                branch->branch.target = ctx->continue_block;
                break;
        default:
                unreachable("Unhandled jump type");
        }

        bi_block_add_successor(ctx->current_block, branch->branch.target);
}

static void
bi_emit_ld_vary(bi_context *ctx, nir_intrinsic_instr *instr)
{
        bi_instruction ins = {
                .type = BI_LOAD_VAR,
                .load_vary = {
                        .load = {
                                .location = nir_intrinsic_base(instr),
                                .channels = instr->num_components,
                        },
                        .interp_mode = BIFROST_INTERP_DEFAULT, /* TODO */
                        .reuse = false, /* TODO */
                        .flat = instr->intrinsic != nir_intrinsic_load_interpolated_input
                },
                .dest = bir_dest_index(&instr->dest),
                .dest_type = nir_type_float | nir_dest_bit_size(instr->dest),
        };

        nir_src *offset = nir_get_io_offset_src(instr);

        if (nir_src_is_const(*offset))
                ins.load_vary.load.location += nir_src_as_uint(*offset);
        else
                ins.src[0] = bir_src_index(offset);

        bi_emit(ctx, ins);
}

static void
emit_intrinsic(bi_context *ctx, nir_intrinsic_instr *instr)
{

        switch (instr->intrinsic) {
        case nir_intrinsic_load_barycentric_pixel:
                /* stub */
                break;
        case nir_intrinsic_load_interpolated_input:
                bi_emit_ld_vary(ctx, instr);
                break;
        default:
                /* todo */
                break;
        }
}

static void
emit_instr(bi_context *ctx, struct nir_instr *instr)
{
        switch (instr->type) {
#if 0
        case nir_instr_type_load_const:
                emit_load_const(ctx, nir_instr_as_load_const(instr));
                break;
#endif

        case nir_instr_type_intrinsic:
                emit_intrinsic(ctx, nir_instr_as_intrinsic(instr));
                break;

#if 0
        case nir_instr_type_alu:
                emit_alu(ctx, nir_instr_as_alu(instr));
                break;

        case nir_instr_type_tex:
                emit_tex(ctx, nir_instr_as_tex(instr));
                break;
#endif

        case nir_instr_type_jump:
                emit_jump(ctx, nir_instr_as_jump(instr));
                break;

        case nir_instr_type_ssa_undef:
                /* Spurious */
                break;

        default:
                //unreachable("Unhandled instruction type");
                break;
        }
}



static bi_block *
create_empty_block(bi_context *ctx)
{
        bi_block *blk = rzalloc(ctx, bi_block);

        blk->predecessors = _mesa_set_create(blk,
                        _mesa_hash_pointer,
                        _mesa_key_pointer_equal);

        blk->name = ctx->block_name_count++;

        return blk;
}

static void
bi_block_add_successor(bi_block *block, bi_block *successor)
{
        assert(block);
        assert(successor);

        for (unsigned i = 0; i < ARRAY_SIZE(block->successors); ++i) {
                if (block->successors[i]) {
                       if (block->successors[i] == successor)
                               return;
                       else
                               continue;
                }

                block->successors[i] = successor;
                _mesa_set_add(successor->predecessors, block);
                return;
        }

        unreachable("Too many successors");
}

static bi_block *
emit_block(bi_context *ctx, nir_block *block)
{
        if (ctx->after_block) {
                ctx->current_block = ctx->after_block;
                ctx->after_block = NULL;
        } else {
                ctx->current_block = create_empty_block(ctx);
        }

        list_addtail(&ctx->current_block->link, &ctx->blocks);
        list_inithead(&ctx->current_block->instructions);

        nir_foreach_instr(instr, block) {
                emit_instr(ctx, instr);
                ++ctx->instruction_count;
        }

        return ctx->current_block;
}

/* Emits an unconditional branch to the end of the current block, returning a
 * pointer so the user can fill in details */

static bi_instruction *
bi_emit_branch(bi_context *ctx)
{
        bi_instruction branch = {
                .type = BI_BRANCH,
                .branch = {
                        .cond = BI_COND_ALWAYS
                }
        };

        return bi_emit(ctx, branch);
}

/* Sets a condition for a branch by examing the NIR condition. If we're
 * familiar with the condition, we unwrap it to fold it into the branch
 * instruction. Otherwise, we consume the condition directly. We
 * generally use 1-bit booleans which allows us to use small types for
 * the conditions.
 */

static void
bi_set_branch_cond(bi_instruction *branch, nir_src *cond, bool invert)
{
        /* TODO: Try to unwrap instead of always bailing */
        branch->src[0] = bir_src_index(cond);
        branch->src[1] = BIR_INDEX_ZERO;
        branch->src_types[0] = branch->src_types[1] = nir_type_uint16;
        branch->branch.cond = invert ? BI_COND_EQ : BI_COND_NE;
}

static void
emit_if(bi_context *ctx, nir_if *nif)
{
        bi_block *before_block = ctx->current_block;

        /* Speculatively emit the branch, but we can't fill it in until later */
        bi_instruction *then_branch = bi_emit_branch(ctx);
        bi_set_branch_cond(then_branch, &nif->condition, true);

        /* Emit the two subblocks. */
        bi_block *then_block = emit_cf_list(ctx, &nif->then_list);
        bi_block *end_then_block = ctx->current_block;

        /* Emit a jump from the end of the then block to the end of the else */
        bi_instruction *then_exit = bi_emit_branch(ctx);

        /* Emit second block, and check if it's empty */

        int count_in = ctx->instruction_count;
        bi_block *else_block = emit_cf_list(ctx, &nif->else_list);
        bi_block *end_else_block = ctx->current_block;
        ctx->after_block = create_empty_block(ctx);

        /* Now that we have the subblocks emitted, fix up the branches */

        assert(then_block);
        assert(else_block);

        if (ctx->instruction_count == count_in) {
                /* The else block is empty, so don't emit an exit jump */
                bi_remove_instruction(then_exit);
                then_branch->branch.target = ctx->after_block;
        } else {
                then_branch->branch.target = else_block;
                then_exit->branch.target = ctx->after_block;
                bi_block_add_successor(end_then_block, then_exit->branch.target);
        }

        /* Wire up the successors */

        bi_block_add_successor(before_block, then_branch->branch.target); /* then_branch */

        bi_block_add_successor(before_block, then_block); /* fallthrough */
        bi_block_add_successor(end_else_block, ctx->after_block); /* fallthrough */
}

static void
emit_loop(bi_context *ctx, nir_loop *nloop)
{
        /* Remember where we are */
        bi_block *start_block = ctx->current_block;

        bi_block *saved_break = ctx->break_block;
        bi_block *saved_continue = ctx->continue_block;

        ctx->continue_block = create_empty_block(ctx);
        ctx->break_block = create_empty_block(ctx);
        ctx->after_block = ctx->continue_block;

        /* Emit the body itself */
        emit_cf_list(ctx, &nloop->body);

        /* Branch back to loop back */
        bi_instruction *br_back = bi_emit_branch(ctx);
        br_back->branch.target = ctx->continue_block;
        bi_block_add_successor(start_block, ctx->continue_block);
        bi_block_add_successor(ctx->current_block, ctx->continue_block);

        ctx->after_block = ctx->break_block;

        /* Pop off */
        ctx->break_block = saved_break;
        ctx->continue_block = saved_continue;
        ++ctx->loop_count;
}

static bi_block *
emit_cf_list(bi_context *ctx, struct exec_list *list)
{
        bi_block *start_block = NULL;

        foreach_list_typed(nir_cf_node, node, node, list) {
                switch (node->type) {
                case nir_cf_node_block: {
                        bi_block *block = emit_block(ctx, nir_cf_node_as_block(node));

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

static int
glsl_type_size(const struct glsl_type *type, bool bindless)
{
        return glsl_count_attribute_slots(type, false);
}

static void
bi_optimize_nir(nir_shader *nir)
{
        bool progress;
        unsigned lower_flrp = 16 | 32 | 64;

        NIR_PASS(progress, nir, nir_lower_regs_to_ssa);
        NIR_PASS(progress, nir, nir_lower_idiv, nir_lower_idiv_fast);

        nir_lower_tex_options lower_tex_options = {
                .lower_txs_lod = true,
                .lower_txp = ~0,
                .lower_tex_without_implicit_lod = true,
                .lower_txd = true,
        };

        NIR_PASS(progress, nir, nir_lower_tex, &lower_tex_options);

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

                if (lower_flrp != 0) {
                        bool lower_flrp_progress = false;
                        NIR_PASS(lower_flrp_progress,
                                 nir,
                                 nir_lower_flrp,
                                 lower_flrp,
                                 false /* always_precise */,
                                 nir->options->lower_ffma);
                        if (lower_flrp_progress) {
                                NIR_PASS(progress, nir,
                                         nir_opt_constant_folding);
                                progress = true;
                        }

                        /* Nothing should rematerialize any flrps, so we only
                         * need to do this lowering once.
                         */
                        lower_flrp = 0;
                }

                NIR_PASS(progress, nir, nir_opt_undef);
                NIR_PASS(progress, nir, nir_opt_loop_unroll,
                         nir_var_shader_in |
                         nir_var_shader_out |
                         nir_var_function_temp);
        } while (progress);

        NIR_PASS(progress, nir, nir_opt_algebraic_late);

        /* Take us out of SSA */
        NIR_PASS(progress, nir, nir_lower_locals_to_regs);
        NIR_PASS(progress, nir, nir_convert_from_ssa, true);
}

void
bifrost_compile_shader_nir(nir_shader *nir, bifrost_program *program, unsigned product_id)
{
        bi_context *ctx = rzalloc(NULL, bi_context);
        ctx->nir = nir;
        ctx->stage = nir->info.stage;
        ctx->quirks = bifrost_get_quirks(product_id);
        list_inithead(&ctx->blocks);

        /* Lower gl_Position pre-optimisation, but after lowering vars to ssa
         * (so we don't accidentally duplicate the epilogue since mesa/st has
         * messed with our I/O quite a bit already) */

        NIR_PASS_V(nir, nir_lower_vars_to_ssa);

        if (ctx->stage == MESA_SHADER_VERTEX) {
                NIR_PASS_V(nir, nir_lower_viewport_transform);
                NIR_PASS_V(nir, nir_lower_point_size, 1.0, 1024.0);
        }

        NIR_PASS_V(nir, nir_split_var_copies);
        NIR_PASS_V(nir, nir_lower_global_vars_to_local);
        NIR_PASS_V(nir, nir_lower_var_copies);
        NIR_PASS_V(nir, nir_lower_vars_to_ssa);
        NIR_PASS_V(nir, nir_lower_io, nir_var_all, glsl_type_size, 0);
        NIR_PASS_V(nir, nir_lower_ssbo);

        /* We have to lower ALU to scalar ourselves since viewport
         * transformations produce vector ops */
        NIR_PASS_V(nir, nir_lower_alu_to_scalar, NULL, NULL);

        bi_optimize_nir(nir);
        nir_print_shader(nir, stdout);

        nir_foreach_function(func, nir) {
                if (!func->impl)
                        continue;

                emit_cf_list(ctx, &func->impl->body);
                break; /* TODO: Multi-function shaders */
        }

        bi_print_shader(ctx, stdout);

        ralloc_free(ctx);
}

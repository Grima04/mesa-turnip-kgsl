/*
 * Copyright (C) 2018-2019 Alyssa Rosenzweig <alyssa@rosenzweig.io>
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

#include "compiler.h"
#include "util/register_allocate.h"

/* When we're 'squeezing down' the values in the IR, we maintain a hash
 * as such */

static unsigned
find_or_allocate_temp(compiler_context *ctx, unsigned hash)
{
        if ((hash < 0) || (hash >= SSA_FIXED_MINIMUM))
                return hash;

        unsigned temp = (uintptr_t) _mesa_hash_table_u64_search(ctx->hash_to_temp, hash + 1);

        if (temp)
                return temp - 1;

        /* If no temp is find, allocate one */
        temp = ctx->temp_count++;
        ctx->max_hash = MAX2(ctx->max_hash, hash);

        _mesa_hash_table_u64_insert(ctx->hash_to_temp, hash + 1, (void *) ((uintptr_t) temp + 1));

        return temp;
}

/* Callback for register allocation selection, trivial default for now */

static unsigned int
midgard_ra_select_callback(struct ra_graph *g, BITSET_WORD *regs, void *data)
{
        /* Choose the first available register to minimise reported register pressure */

        for (int i = 0; i < 16; ++i) {
                if (BITSET_TEST(regs, i)) {
                        return i;
                }
        }

        assert(0);
        return 0;
}

/* Determine the actual hardware from the index based on the RA results or special values */

static int
dealias_register(compiler_context *ctx, struct ra_graph *g, int reg, int maxreg)
{
        if (reg >= SSA_FIXED_MINIMUM)
                return SSA_REG_FROM_FIXED(reg);

        if (reg >= 0) {
                assert(reg < maxreg);
                assert(g);
                int r = ra_get_node_reg(g, reg);
                ctx->work_registers = MAX2(ctx->work_registers, r);
                return r;
        }

        switch (reg) {
        case SSA_UNUSED_0:
        case SSA_UNUSED_1:
                return REGISTER_UNUSED;

        default:
                unreachable("Unknown SSA register alias");
        }
}

/* This routine performs the actual register allocation. It should be succeeded
 * by install_registers */

struct ra_graph *
allocate_registers(compiler_context *ctx)
{
        /* First, initialize the RA */
        struct ra_regs *regs = ra_alloc_reg_set(NULL, 32, true);

        /* Create a primary (general purpose) class, as well as special purpose
         * pipeline register classes */

        int primary_class = ra_alloc_reg_class(regs);
        int varying_class  = ra_alloc_reg_class(regs);

        /* Add the full set of work registers */
        int work_count = 16 - MAX2((ctx->uniform_cutoff - 8), 0);
        for (int i = 0; i < work_count; ++i)
                ra_class_add_reg(regs, primary_class, i);

        /* Add special registers */
        ra_class_add_reg(regs, varying_class, REGISTER_VARYING_BASE);
        ra_class_add_reg(regs, varying_class, REGISTER_VARYING_BASE + 1);

        /* We're done setting up */
        ra_set_finalize(regs, NULL);

        /* Transform the MIR into squeezed index form */
        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->compact_branch) continue;

                        ins->ssa_args.src0 = find_or_allocate_temp(ctx, ins->ssa_args.src0);
                        ins->ssa_args.src1 = find_or_allocate_temp(ctx, ins->ssa_args.src1);
                        ins->ssa_args.dest = find_or_allocate_temp(ctx, ins->ssa_args.dest);
                }
        }

        /* No register allocation to do with no SSA */

        if (!ctx->temp_count)
                return NULL;

        /* Let's actually do register allocation */
        int nodes = ctx->temp_count;
        struct ra_graph *g = ra_alloc_interference_graph(regs, nodes);

        /* Set everything to the work register class, unless it has somewhere
         * special to go */

        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->compact_branch) continue;

                        if (ins->ssa_args.dest < 0) continue;

                        if (ins->ssa_args.dest >= SSA_FIXED_MINIMUM) continue;

                        int class = primary_class;

                        ra_set_node_class(g, ins->ssa_args.dest, class);
                }
        }

        for (int index = 0; index <= ctx->max_hash; ++index) {
                unsigned temp = (uintptr_t) _mesa_hash_table_u64_search(ctx->ssa_to_register, index + 1);

                if (temp) {
                        unsigned reg = temp - 1;
                        int t = find_or_allocate_temp(ctx, index);
                        ra_set_node_reg(g, t, reg);
                }
        }

        /* Determine liveness */

        int *live_start = malloc(nodes * sizeof(int));
        int *live_end = malloc(nodes * sizeof(int));

        /* Initialize as non-existent */

        for (int i = 0; i < nodes; ++i) {
                live_start[i] = live_end[i] = -1;
        }

        int d = 0;

        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->compact_branch) continue;

                        /* Dest is < 0 for st_vary instructions, which break
                         * the usual SSA conventions. Liveness analysis doesn't
                         * make sense on these instructions, so skip them to
                         * avoid memory corruption */

                        if (ins->ssa_args.dest < 0) continue;

                        if (ins->ssa_args.dest < SSA_FIXED_MINIMUM) {
                                /* If this destination is not yet live, it is now since we just wrote it */

                                int dest = ins->ssa_args.dest;

                                if (live_start[dest] == -1)
                                        live_start[dest] = d;
                        }

                        /* Since we just used a source, the source might be
                         * dead now. Scan the rest of the block for
                         * invocations, and if there are none, the source dies
                         * */

                        int sources[2] = { ins->ssa_args.src0, ins->ssa_args.src1 };

                        for (int src = 0; src < 2; ++src) {
                                int s = sources[src];

                                if (s < 0) continue;

                                if (s >= SSA_FIXED_MINIMUM) continue;

                                if (!mir_is_live_after(ctx, block, ins, s)) {
                                        live_end[s] = d;
                                }
                        }

                        ++d;
                }
        }

        /* If a node still hasn't been killed, kill it now */

        for (int i = 0; i < nodes; ++i) {
                /* live_start == -1 most likely indicates a pinned output */

                if (live_end[i] == -1)
                        live_end[i] = d;
        }

        /* Setup interference between nodes that are live at the same time */

        for (int i = 0; i < nodes; ++i) {
                for (int j = i + 1; j < nodes; ++j) {
                        if (!(live_start[i] >= live_end[j] || live_start[j] >= live_end[i]))
                                ra_add_node_interference(g, i, j);
                }
        }

        ra_set_select_reg_callback(g, midgard_ra_select_callback, NULL);

        if (!ra_allocate(g)) {
                unreachable("Error allocating registers\n");
        }

        /* Cleanup */
        free(live_start);
        free(live_end);

        return g;
}

/* Once registers have been decided via register allocation
 * (allocate_registers), we need to rewrite the MIR to use registers instead of
 * SSA */

void
install_registers(compiler_context *ctx, struct ra_graph *g)
{
        mir_foreach_block(ctx, block) {
                mir_foreach_instr_in_block(block, ins) {
                        if (ins->compact_branch) continue;

                        ssa_args args = ins->ssa_args;

                        switch (ins->type) {
                        case TAG_ALU_4:
                                ins->registers.src1_reg = dealias_register(ctx, g, args.src0, ctx->temp_count);

                                ins->registers.src2_imm = args.inline_constant;

                                if (args.inline_constant) {
                                        /* Encode inline 16-bit constant as a vector by default */

                                        ins->registers.src2_reg = ins->inline_constant >> 11;

                                        int lower_11 = ins->inline_constant & ((1 << 12) - 1);

                                        uint16_t imm = ((lower_11 >> 8) & 0x7) | ((lower_11 & 0xFF) << 3);
                                        ins->alu.src2 = imm << 2;
                                } else {
                                        ins->registers.src2_reg = dealias_register(ctx, g, args.src1, ctx->temp_count);
                                }

                                ins->registers.out_reg = dealias_register(ctx, g, args.dest, ctx->temp_count);

                                break;

                        case TAG_LOAD_STORE_4: {
                                if (OP_IS_STORE_VARY(ins->load_store.op)) {
                                        /* TODO: use ssa_args for st_vary */
                                        ins->load_store.reg = 0;
                                } else {
                                        bool has_dest = args.dest >= 0;
                                        int ssa_arg = has_dest ? args.dest : args.src0;

                                        ins->load_store.reg = dealias_register(ctx, g, ssa_arg, ctx->temp_count);
                                }

                                break;
                        }

                        default:
                                break;
                        }
                }
        }

}

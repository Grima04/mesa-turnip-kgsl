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

/* mir_is_live_after performs liveness analysis on the MIR, used primarily
 * as part of register allocation. TODO: Algorithmic improvements for
 * compiler performance (this is the worst algorithm possible -- see
 * backlog with Connor on IRC) */

#include "compiler.h"
#include "util/u_memory.h"

/* Routines for liveness analysis */

static void
liveness_gen(uint8_t *live, unsigned node, unsigned max, unsigned mask)
{
        if (node >= max)
                return;

        live[node] |= mask;
}

static void
liveness_kill(uint8_t *live, unsigned node, unsigned max, unsigned mask)
{
        if (node >= max)
                return;

        live[node] &= ~mask;
}

/* Updates live_in for a single instruction */

void
mir_liveness_ins_update(uint8_t *live, midgard_instruction *ins, unsigned max)
{
        /* live_in[s] = GEN[s] + (live_out[s] - KILL[s]) */

        liveness_kill(live, ins->dest, max, ins->mask);

        mir_foreach_src(ins, src) {
                unsigned node = ins->src[src];
                unsigned mask = mir_mask_of_read_components(ins, node);

                liveness_gen(live, node, max, mask);
        }
}

/* live_out[s] = sum { p in succ[s] } ( live_in[p] ) */

static void
liveness_block_live_out(compiler_context *ctx, midgard_block *blk)
{
        mir_foreach_successor(blk, succ) {
                for (unsigned i = 0; i < ctx->temp_count; ++i)
                        blk->live_out[i] |= succ->live_in[i];
        }
}

/* Liveness analysis is a backwards-may dataflow analysis pass. Within a block,
 * we compute live_out from live_in. The intrablock pass is linear-time. It
 * returns whether progress was made. */

static bool
liveness_block_update(compiler_context *ctx, midgard_block *blk)
{
        bool progress = false;

        liveness_block_live_out(ctx, blk);

        uint8_t *live = mem_dup(blk->live_out, ctx->temp_count);

        mir_foreach_instr_in_block_rev(blk, ins)
                mir_liveness_ins_update(live, ins, ctx->temp_count);

        /* To figure out progress, diff live_in */

        for (unsigned i = 0; (i < ctx->temp_count) && !progress; ++i)
                progress |= (blk->live_in[i] != live[i]);

        free(blk->live_in);
        blk->live_in = live;

        return progress;
}

/* Globally, liveness analysis uses a fixed-point algorithm based on a
 * worklist. We initialize a work list with the exit block. We iterate the work
 * list to compute live_in from live_out for each block on the work list,
 * adding the predecessors of the block to the work list if we made progress.
 */

void
mir_compute_liveness(compiler_context *ctx)
{
        /* If we already have fresh liveness, nothing to do */
        if (ctx->metadata & MIDGARD_METADATA_LIVENESS)
                return;

        /* List of midgard_block */
        struct set *work_list = _mesa_set_create(ctx,
                        _mesa_hash_pointer,
                        _mesa_key_pointer_equal);

        /* Allocate */

        mir_foreach_block(ctx, block) {
                block->live_in = calloc(ctx->temp_count, 1);
                block->live_out = calloc(ctx->temp_count, 1);
        }

        /* Initialize the work list with the exit block */
        struct set_entry *cur;

        midgard_block *exit = mir_exit_block(ctx);
        cur = _mesa_set_add(work_list, exit);

        /* Iterate the work list */

        do {
                /* Pop off a block */
                midgard_block *blk = (struct midgard_block *) cur->key;
                _mesa_set_remove(work_list, cur);

                /* Update its liveness information */
                bool progress = liveness_block_update(ctx, blk);

                /* If we made progress, we need to process the predecessors */

                if (progress || (blk == exit)) {
                        mir_foreach_predecessor(blk, pred)
                                _mesa_set_add(work_list, pred);
                }
        } while((cur = _mesa_set_next_entry(work_list, NULL)) != NULL);

        /* Liveness is now valid */
        ctx->metadata |= MIDGARD_METADATA_LIVENESS;
}

/* Once liveness data is no longer valid, call this */

void
mir_invalidate_liveness(compiler_context *ctx)
{
        /* If we didn't already compute liveness, there's nothing to do */
        if (!(ctx->metadata & MIDGARD_METADATA_LIVENESS))
                return;

        /* It's now invalid regardless */
        ctx->metadata &= ~MIDGARD_METADATA_LIVENESS;

        mir_foreach_block(ctx, block) {
                if (block->live_in)
                        free(block->live_in);

                if (block->live_out)
                        free(block->live_out);

                block->live_in = NULL;
                block->live_out = NULL;
        }
}

/* Determine if a variable is live in the successors of a block */
static bool
is_live_after_successors(compiler_context *ctx, midgard_block *bl, int src)
{
        for (unsigned i = 0; i < bl->nr_successors; ++i) {
                midgard_block *succ = bl->successors[i];

                /* If we already visited, the value we're seeking
                 * isn't down this path (or we would have short
                 * circuited */

                if (succ->visited) continue;

                /* Otherwise (it's visited *now*), check the block */

                succ->visited = true;

                /* Within this block, check if it's overwritten first */
                unsigned overwritten_mask = 0;

                mir_foreach_instr_in_block(succ, ins) {
                        /* Did we read any components that we haven't overwritten yet? */
                        if (mir_mask_of_read_components(ins, src) & ~overwritten_mask)
                                return true;

                        /* If written-before-use, we're gone */

                        if (ins->dest == src)
                                overwritten_mask |= ins->mask;
                }

                /* ...and also, check *its* successors */
                if (is_live_after_successors(ctx, succ, src))
                        return true;

        }

        /* Welp. We're really not live. */

        return false;
}

bool
mir_is_live_after(compiler_context *ctx, midgard_block *block, midgard_instruction *start, int src)
{
        /* Check the rest of the block for liveness */

        mir_foreach_instr_in_block_from(block, ins, mir_next_op(start)) {
                if (mir_has_arg(ins, src))
                        return true;
        }

        /* Check the rest of the blocks for liveness recursively */

        bool succ = is_live_after_successors(ctx, block, src);

        mir_foreach_block(ctx, block) {
                block->visited = false;
        }

        return succ;
}

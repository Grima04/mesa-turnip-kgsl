/*
 * Copyright (c) 2019 Connor Abbott
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "gpir.h"

/* Here we perform a few optimizations that can't currently be done in NIR:
 *
 * - Optimize the result of a conditional break/continue. In NIR something
 *   like:
 *
 * loop {
 *    ...
 *    if (cond)
 *       continue;
 *
 * would get lowered to:
 *
 * block_0:
 * ...
 * block_1:
 * branch_cond !cond block_3
 * block_2:
 * branch_uncond block_0
 * block_3:
 * ...
 *
 *   We recognize the conditional branch skipping over the unconditional
 *   branch, and turn it into:
 *
 * block_0:
 * ...
 * block_1:
 * branch_cond cond block_0
 * block_2:
 * block_3:
 * ...
 */

static void
optimize_branches(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      /* Look for a block with a single unconditional branch. */
      if (!list_is_singular(&block->node_list))
         continue;

      gpir_node *node = list_first_entry(&block->node_list, gpir_node, list);
      if (node->op != gpir_op_branch_uncond)
         continue;

      gpir_block *target = gpir_node_to_branch(node)->dest;

      /* Find the previous block */
      if (block->list.prev == &comp->block_list)
         continue;

      gpir_block *prev_block = LIST_ENTRY(gpir_block, block->list.prev, list);
      if (list_is_empty(&prev_block->node_list))
         continue;

      /* Previous block must end with a conditional branch */
      gpir_node *prev_block_last =
         list_last_entry(&prev_block->node_list, gpir_node, list);
      if (prev_block_last->op != gpir_op_branch_cond)
         continue;

      /* That branch must branch to the block after this */
      gpir_branch_node *prev_branch = gpir_node_to_branch(prev_block_last);
      gpir_block *prev_target = prev_branch->dest;

      if (&prev_target->list != block->list.next)
         continue;

      /* Hooray! Invert the conditional branch and change the target */
      gpir_alu_node *cond = gpir_node_create(prev_block, gpir_op_not);
      cond->children[0] = prev_branch->cond;
      cond->num_child = 1;
      gpir_node_add_dep(&cond->node, cond->children[0], GPIR_DEP_INPUT);
      list_addtail(&cond->node.list, &prev_block_last->list);
      gpir_node_insert_child(prev_block_last, prev_branch->cond, &cond->node);
      prev_branch->dest = target;
      prev_block->successors[1] = target;

      /* Delete the branch */
      list_del(&node->list);
      block->successors[0] = LIST_ENTRY(gpir_block, block->list.next, list);
   }
}

bool
gpir_optimize(gpir_compiler *comp)
{
   optimize_branches(comp);

   gpir_debug("after optimization\n");
   gpir_node_print_prog_seq(comp);

   return true;
}


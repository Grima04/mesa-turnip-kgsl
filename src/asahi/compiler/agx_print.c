/*
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2019-2020 Collabora, Ltd.
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

#include "agx_compiler.h"

void
agx_print_instr(agx_instr *I, FILE *fp)
{
   /* Stub */
}

void
agx_print_block(agx_block *block, FILE *fp)
{
   fprintf(fp, "block%u {\n", block->name);

   agx_foreach_instr_in_block(block, ins)
      agx_print_instr(ins, fp);

   fprintf(fp, "}");

   if (block->successors[0]) {
      fprintf(fp, " -> ");

      agx_foreach_successor(block, succ)
         fprintf(fp, "block%u ", succ->name);
   }

   if (block->predecessors->entries) {
      fprintf(fp, " from");

      agx_foreach_predecessor(block, pred)
         fprintf(fp, " block%u", pred->name);
   }

   fprintf(fp, "\n\n");
}

void
agx_print_shader(agx_context *ctx, FILE *fp)
{
   agx_foreach_block(ctx, block)
      agx_print_block(block, fp);
}

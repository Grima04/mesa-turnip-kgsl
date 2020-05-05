/*
 * Copyright (C) 2020 Collabora, Ltd.
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

/* The scheduler packs multiple instructions into a clause (grouped as bundle),
 * and the packing code takes in a clause and emits it to the wire. During
 * scheduling, we need to lay out the instructions (bundles) and constants
 * within the clause so constraints can be resolved during scheduling instead
 * of failing packing. These routines will help building clauses from
 * instructions so the scheduler can focus on the high-level algorithm, and
 * manipulating clause layouts.
 */

/* Helper to see if a bundle can be inserted. We must satisfy the invariant:
 *
 *      constant_count + bundle_count <= 13
 *
 * ...which is equivalent to the clause ending up with 8 or fewer quardwords.
 * Inserting a bundle increases bundle_count by one, and if it reads a unique
 * constant, it increases constant_count by one.
 */

bool
bi_can_insert_bundle(bi_clause *clause, bool constant)
{
        unsigned constant_count = clause->constant_count + (constant ? 1 : 0);
        unsigned bundle_count = clause->bundle_count + 1;

        return (constant_count + bundle_count) <= 13;
}

/* Copyright (c) 2018-2019 Alyssa Rosenzweig (alyssa@rosenzweig.io)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "helpers.h"

/* Forward declare */

extern struct mir_op_props alu_opcode_props[256];
extern const char *load_store_opcode_names[256];

/* Is this opcode that of an integer (regardless of signedness)? Instruction
 * names authoritatively determine types */

static inline bool
midgard_is_integer_op(int op)
{
        const char *name = alu_opcode_props[op].name;

        if (!name)
                return false;

        return (name[0] == 'i') || (name[0] == 'u');
}

/* Does this opcode *write* an integer? Same as is_integer_op, unless it's a
 * conversion between int<->float in which case we do the opposite */

static inline bool
midgard_is_integer_out_op(int op)
{
        bool is_int = midgard_is_integer_op(op);
        bool is_conversion = alu_opcode_props[op].props & OP_TYPE_CONVERT;

        return is_int ^ is_conversion;
}

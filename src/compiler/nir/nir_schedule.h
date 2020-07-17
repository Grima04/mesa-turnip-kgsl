/*
 * Copyright © 2014 Connor Abbott
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef NIR_SCHEDULE_H
#define NIR_SCHEDULE_H

#include "nir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nir_schedule_options {
   /* On some hardware with some stages the inputs and outputs to the shader
    * share the same memory. In that case the scheduler needs to ensure that
    * all output writes are scheduled after all of the input writes to avoid
    * overwriting them. This is a bitmask of stages that need that.
    */
   unsigned stages_with_shared_io_memory;
   /* The approximate amount of register pressure at which point the scheduler
    * will try to reduce register usage.
    */
   int threshold;
} nir_schedule_options;

void nir_schedule(nir_shader *shader, const nir_schedule_options *options);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NIR_SCHEDULE_H */

/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef IRIS_PIPE_H
#define IRIS_PIPE_H

#include "pipe/p_defines.h"
#include "compiler/shader_enums.h"

static gl_shader_stage
stage_from_pipe(enum pipe_shader_type pstage)
{
   static const gl_shader_stage stages[PIPE_SHADER_TYPES] = {
      [PIPE_SHADER_VERTEX] = MESA_SHADER_VERTEX,
      [PIPE_SHADER_TESS_CTRL] = MESA_SHADER_TESS_CTRL,
      [PIPE_SHADER_TESS_EVAL] = MESA_SHADER_TESS_EVAL,
      [PIPE_SHADER_GEOMETRY] = MESA_SHADER_GEOMETRY,
      [PIPE_SHADER_FRAGMENT] = MESA_SHADER_FRAGMENT,
      [PIPE_SHADER_COMPUTE] = MESA_SHADER_COMPUTE,
   };
   return stages[pstage];
}

#endif

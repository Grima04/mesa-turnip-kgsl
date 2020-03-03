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

void
bifrost_compile_shader_nir(nir_shader *nir, bifrost_program *program, unsigned product_id)
{
        bi_context *ctx = rzalloc(NULL, bi_context);
        ctx->nir = nir;
        ctx->quirks = bifrost_get_quirks(product_id);

        nir_print_shader(nir, stdout);

        ralloc_free(ctx);
}

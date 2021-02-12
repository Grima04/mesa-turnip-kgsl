/*
 * Copyright (C) 2018 Alyssa Rosenzweig
 * Copyright (C) 2019-2021 Collabora, Ltd.
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

#include "pan_device.h"
#include "pan_shader.h"

#include "panfrost/midgard/midgard_compile.h"
#include "panfrost/bifrost/bifrost_compile.h"

const nir_shader_compiler_options *
panfrost_get_shader_options(const struct panfrost_device *dev)
{
        if (pan_is_bifrost(dev))
                return &bifrost_nir_options;

        return &midgard_nir_options;
}

panfrost_program *
panfrost_compile_shader(const struct panfrost_device *dev,
                        void *mem_ctx, nir_shader *nir,
                        const struct panfrost_compile_inputs *inputs)
{
        if (pan_is_bifrost(dev))
                return bifrost_compile_shader_nir(mem_ctx, nir, inputs);

        return midgard_compile_shader_nir(mem_ctx, nir, inputs);
}

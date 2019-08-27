/**************************************************************************
 *
 * Copyright 2019 Red Hat.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **************************************************************************/

#ifndef LP_STATE_CS_H
#define LP_STATE_CS_H

#include "os/os_thread.h"
#include "util/u_thread.h"
#include "pipe/p_state.h"

#include "gallivm/lp_bld.h"
#include "lp_jit.h"
struct lp_compute_shader_variant;

struct lp_compute_shader_variant_key
{
};

struct lp_cs_variant_list_item
{
   struct lp_compute_shader_variant *base;
   struct lp_cs_variant_list_item *next, *prev;
};

struct lp_compute_shader_variant
{
   struct lp_compute_shader_variant_key key;

   struct gallivm_state *gallivm;

   LLVMTypeRef jit_cs_context_ptr_type;
   LLVMTypeRef jit_cs_thread_data_ptr_type;
};

struct lp_compute_shader {
   struct pipe_shader_state base;

   struct lp_cs_variant_list_item variants;
};

struct lp_cs_exec {
   struct lp_jit_cs_context jit_context;
   struct lp_compute_shader_variant *variant;
};

struct lp_cs_context {
   struct pipe_context *pipe;

   struct {
      struct lp_cs_exec current;
   } cs;
};

struct lp_cs_context *lp_csctx_create(struct pipe_context *pipe);
void lp_csctx_destroy(struct lp_cs_context *csctx);

#endif

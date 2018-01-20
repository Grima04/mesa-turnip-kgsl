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
#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_upload_mgr.h"
#include "util/ralloc.h"
#include "iris_context.h"
#include "iris_resource.h"
#include "iris_screen.h"
#include "intel/compiler/brw_compiler.h"

struct iris_query {
   unsigned query;
};

static struct pipe_query *
iris_create_query(struct pipe_context *ctx,
                  unsigned query_type,
                  unsigned index)
{
   struct iris_query *query = calloc(1, sizeof(struct iris_query));

   return (struct pipe_query *)query;
}

static void
iris_destroy_query(struct pipe_context *ctx, struct pipe_query *query)
{
   free(query);
}

static boolean
iris_begin_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static bool
iris_end_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static boolean
iris_get_query_result(struct pipe_context *ctx,
                      struct pipe_query *query,
                      boolean wait,
                      union pipe_query_result *vresult)
{
   uint64_t *result = (uint64_t*)vresult;

   *result = 0;
   return TRUE;
}

static void
iris_set_active_query_state(struct pipe_context *pipe, boolean enable)
{
}

void
iris_init_query_functions(struct pipe_context *ctx)
{
   ctx->create_query = iris_create_query;
   ctx->destroy_query = iris_destroy_query;
   ctx->begin_query = iris_begin_query;
   ctx->end_query = iris_end_query;
   ctx->get_query_result = iris_get_query_result;
   ctx->set_active_query_state = iris_set_active_query_state;
}

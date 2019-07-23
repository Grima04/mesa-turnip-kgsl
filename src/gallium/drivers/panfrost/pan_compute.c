/*
 * Copyright (C) 2019 Collabora, Ltd.
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
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 *
 */

#include "pan_context.h"
#include "util/u_memory.h"

static void *
panfrost_create_compute_state(
        struct pipe_context *pctx,
        const struct pipe_compute_state *cso)
{
        return mem_dup(cso, sizeof(*cso));
}

static void
panfrost_bind_compute_state(struct pipe_context *pipe, void *cso)
{
        struct pipe_compute_state *state = (struct pipe_compute_state *) cso;

        printf("Binding compute %p\n", state);
        /* Stub */
}

static void
panfrost_delete_compute_state(struct pipe_context *pipe, void *cso)
{
        free(cso);
}

void
panfrost_compute_context_init(struct pipe_context *pctx)
{
        pctx->create_compute_state = panfrost_create_compute_state;
        pctx->bind_compute_state = panfrost_bind_compute_state;
        pctx->delete_compute_state = panfrost_delete_compute_state;
}



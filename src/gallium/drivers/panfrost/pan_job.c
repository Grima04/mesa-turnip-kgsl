/*
 * Copyright (C) 2019 Alyssa Rosenzweig
 * Copyright (C) 2014-2017 Broadcom
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
 */

#include "pan_context.h"
#include "util/hash_table.h"
#include "util/ralloc.h"

struct panfrost_job *
panfrost_create_job(struct panfrost_context *ctx)
{
        /* TODO: Don't leak */
        struct panfrost_job *job = rzalloc(NULL, struct panfrost_job);

        job->ctx = ctx;

        return job;
}

struct panfrost_job *
panfrost_get_job(struct panfrost_context *ctx,
                struct pipe_surface **cbufs, struct pipe_surface *zsbuf)
{
        /* Lookup the job first */

        struct panfrost_job_key key = {
                .cbufs = {
                        cbufs[0],
                        cbufs[1],
                        cbufs[2],
                        cbufs[3],
                },
                .zsbuf = zsbuf
        };
        
        struct hash_entry *entry = _mesa_hash_table_search(ctx->jobs, &key);

        if (entry)
                return entry->data;

        /* Otherwise, let's create a job */

        struct panfrost_job *job = panfrost_create_job(ctx);

        /* Save the created job */

        memcpy(&job->key, &key, sizeof(key));
        _mesa_hash_table_insert(ctx->jobs, &job->key, job);

        return job;
}

/* Get the job corresponding to the FBO we're currently rendering into */

struct panfrost_job *
panfrost_get_job_for_fbo(struct panfrost_context *ctx)
{
        /* If we already began rendering, use that */

        if (ctx->job)
                return ctx->job;

        /* If not, look up the job */

        struct pipe_surface **cbufs = ctx->pipe_framebuffer.cbufs;
        struct pipe_surface *zsbuf = ctx->pipe_framebuffer.zsbuf;
        struct panfrost_job *job = panfrost_get_job(ctx, cbufs, zsbuf);

        return job;
}

static bool
panfrost_job_compare(const void *a, const void *b)
{
        return memcmp(a, b, sizeof(struct panfrost_job_key)) == 0;
}

static uint32_t
panfrost_job_hash(const void *key)
{
        return _mesa_hash_data(key, sizeof(struct panfrost_job_key));
}

void
panfrost_job_init(struct panfrost_context *ctx)
{
        /* TODO: Don't leak */
        ctx->jobs = _mesa_hash_table_create(NULL,
                                            panfrost_job_hash,
                                            panfrost_job_compare);
}

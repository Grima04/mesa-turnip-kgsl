/*
 * Copyright (C) 2018-2019 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef H_LIMA_SUBMIT
#define H_LIMA_SUBMIT

#include <stdbool.h>
#include <stdint.h>

#include <util/u_dynarray.h>

struct lima_context;
struct lima_bo;
struct pipe_surface;

struct lima_submit_key {
   struct pipe_surface *cbuf;
   struct pipe_surface *zsbuf;
};

struct lima_submit {
   int fd;
   struct lima_context *ctx;

   struct util_dynarray gem_bos[2];
   struct util_dynarray bos[2];

   struct lima_submit_key key;

   struct util_dynarray vs_cmd_array;
   struct util_dynarray plbu_cmd_array;
   struct util_dynarray plbu_cmd_head;

   unsigned resolve;
};

struct lima_submit *lima_submit_get(struct lima_context *ctx);

bool lima_submit_add_bo(struct lima_submit *submit, int pipe,
                        struct lima_bo *bo, uint32_t flags);
void *lima_submit_create_stream_bo(struct lima_submit *submit, int pipe,
                                   unsigned size, uint32_t *va);

bool lima_submit_init(struct lima_context *ctx);
void lima_submit_fini(struct lima_context *ctx);

#endif

/*
 * Copyright 2017 Intel Corporation
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
#ifndef IRIS_RESOURCE_H
#define IRIS_RESOURCE_H

#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "intel/isl/isl.h"

struct iris_resource {
   struct pipe_resource	base;
   struct isl_surf surf;
   struct iris_bo *bo;
};

/**
 * A simple <resource, offset> tuple for storing a reference to a
 * piece of state stored in a GPU buffer object.
 */
struct iris_state_ref {
   struct pipe_resource *res;
   uint32_t offset;
};

enum isl_format iris_isl_format_for_pipe_format(enum pipe_format pf);

void iris_init_screen_resource_functions(struct pipe_screen *pscreen);

static inline struct iris_bo *
iris_resource_bo(struct pipe_resource *p_res)
{
   struct iris_resource *res = (void *) p_res;
   return res->bo;
}

struct iris_surface {
   struct pipe_surface pipe;
   struct isl_view view;

   /** The resource (BO) holding our SURFACE_STATE. */
   struct iris_state_ref surface_state;
};

struct iris_transfer {
   struct pipe_transfer base;
   struct pipe_debug_callback *dbg;
   void *buffer;
   void *ptr;

   /** Stride of the temporary image (not the actual surface) */
   int temp_stride;

   void (*unmap)(struct iris_transfer *);
};

#endif

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

#define IRIS_RESOURCE_FLAG_SHADER_MEMZONE  (PIPE_RESOURCE_FLAG_DRV_PRIV << 0)
#define IRIS_RESOURCE_FLAG_SURFACE_MEMZONE (PIPE_RESOURCE_FLAG_DRV_PRIV << 1)
#define IRIS_RESOURCE_FLAG_DYNAMIC_MEMZONE (PIPE_RESOURCE_FLAG_DRV_PRIV << 2)

/**
 * Resources represent a GPU buffer object or image (mipmap tree).
 *
 * They contain the storage (BO) and layout information (ISL surface).
 */
struct iris_resource {
   struct pipe_resource	base;
   enum pipe_format internal_format;
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

/**
 * A view of a surface that can be bound to a color render target or
 * depth/stencil attachment.
 */
struct iris_surface {
   struct pipe_surface pipe;
   struct isl_view view;

   /** The resource (BO) holding our SURFACE_STATE. */
   struct iris_state_ref surface_state;
};

/**
 * Transfer object - information about a buffer mapping.
 */
struct iris_transfer {
   struct pipe_transfer base;
   struct pipe_debug_callback *dbg;
   void *buffer;
   void *ptr;

   void (*unmap)(struct iris_transfer *);
};

/**
 * Unwrap a pipe_resource to get the underlying iris_bo (for convenience).
 */
static inline struct iris_bo *
iris_resource_bo(struct pipe_resource *p_res)
{
   struct iris_resource *res = (void *) p_res;
   return res->bo;
}

enum isl_format iris_isl_format_for_pipe_format(enum pipe_format pf);

struct pipe_resource *iris_resource_get_separate_stencil(struct pipe_resource *);

void iris_get_depth_stencil_resources(struct pipe_resource *res,
                                      struct iris_resource **out_z,
                                      struct iris_resource **out_s);

void iris_init_screen_resource_functions(struct pipe_screen *pscreen);

#endif

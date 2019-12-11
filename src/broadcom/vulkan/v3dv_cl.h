/*
 * Copyright © 2019 Raspberry Pi
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef V3DV_CL_H
#define V3DV_CL_H

struct v3dv_bo;
struct v3dv_cmd_buffer;
struct v3dv_cl;

/**
 * Undefined structure, used for typechecking that you're passing the pointers
 * to these functions correctly.
 */
struct v3dv_cl_out;

struct v3dv_cl {
   void *base;
   struct v3dv_cmd_buffer *cmd_buffer;
   struct v3dv_cl_out *next;
   struct v3dv_bo *bo;
   uint32_t size;
};

static inline uint32_t
v3dv_cl_offset(struct v3dv_cl *cl)
{
   return (char *)cl->next - (char *)cl->base;
}

void v3dv_cl_init(struct v3dv_cmd_buffer *cmd_buffer, struct v3dv_cl *cl);
void v3dv_cl_begin(struct v3dv_cl *cl);
void v3dv_cl_reset(struct v3dv_cl *cl);
void v3dv_cl_destroy(struct v3dv_cl *cl);

#endif /* V3DV_CL_H */

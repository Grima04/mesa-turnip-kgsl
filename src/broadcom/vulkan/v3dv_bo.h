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

#ifndef V3DV_BO_H
#define V3DV_BO_H

struct v3dv_device;

struct v3dv_bo {
   uint32_t handle;
   uint32_t size;
   uint32_t offset;

   uint32_t map_size;
   void *map;
};

bool v3dv_bo_alloc(struct v3dv_device *device, uint32_t size, struct v3dv_bo *bo);

bool v3dv_bo_free(struct v3dv_device *device, struct v3dv_bo *bo);

bool v3dv_bo_wait(struct v3dv_device *device, struct v3dv_bo *bo, uint64_t timeout_ns);

bool v3dv_bo_map_unsynchronized(struct v3dv_device *device, struct v3dv_bo *bo, uint32_t size);

bool v3dv_bo_map(struct v3dv_device *device, struct v3dv_bo *bo, uint32_t size);

void v3dv_bo_unmap(struct v3dv_device *device, struct v3dv_bo *bo);

#endif /* V3DV_BO_H */

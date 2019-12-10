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

#include "v3dv_private.h"

#include <errno.h>
#include <sys/mman.h>

#include "drm-uapi/v3d_drm.h"

bool
v3dv_bo_alloc(struct v3dv_device *device, uint32_t size, struct v3dv_bo *bo)
{
   const uint32_t page_align = 4096; /* Always allocate full pages */
   size = align(size, page_align);
   struct drm_v3d_create_bo create = {
      .size = size
   };

   int ret = v3dv_ioctl(device->fd, DRM_IOCTL_V3D_CREATE_BO, &create);
   if (ret != 0)
      return false;

   assert(create.offset % page_align == 0);
   assert((create.offset & 0xffffffff) == create.offset);

   bo->handle = create.handle;
   bo->size = size;
   bo->offset = create.offset;
   bo->map = NULL;
   bo->map_size = 0;

   return true;
}

bool
v3dv_bo_free(struct v3dv_device *device, struct v3dv_bo *bo)
{
   assert(bo);

   if (bo->map)
      v3dv_bo_unmap(device, bo);

   struct drm_gem_close c;
   memset(&c, 0, sizeof(c));
   c.handle = bo->handle;
   int ret = v3dv_ioctl(device->fd, DRM_IOCTL_GEM_CLOSE, &c);
   if (ret != 0)
      fprintf(stderr, "close object %d: %s\n", bo->handle, strerror(errno));

   return ret == 0;
}

bool
v3dv_bo_map_unsynchronized(struct v3dv_device *device,
                           struct v3dv_bo *bo,
                           uint32_t size)
{
   assert(bo != NULL && size <= bo->size);

   struct drm_v3d_mmap_bo map;
   memset(&map, 0, sizeof(map));
   map.handle = bo->handle;
   int ret = v3dv_ioctl(device->fd, DRM_IOCTL_V3D_MMAP_BO, &map);
   if (ret != 0) {
      fprintf(stderr, "map ioctl failure\n");
      return false;
   }

   bo->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  device->fd, map.offset);
   if (bo->map == MAP_FAILED) {
      fprintf(stderr, "mmap of bo %d (offset 0x%016llx, size %d) failed\n",
              bo->handle, (long long)map.offset, (uint32_t)bo->size);
      return false;
   }
   VG(VALGRIND_MALLOCLIKE_BLOCK(bo->map, bo->size, 0, false));

   bo->map_size = size;

   return true;
}

bool
v3dv_bo_wait(struct v3dv_device *device,
             struct v3dv_bo *bo,
             uint64_t timeout_ns)
{
   struct drm_v3d_wait_bo wait = {
      .handle = bo->handle,
      .timeout_ns = timeout_ns,
   };
   return v3dv_ioctl(device->fd, DRM_IOCTL_V3D_WAIT_BO, &wait) == 0;
}

bool
v3dv_bo_map(struct v3dv_device *device, struct v3dv_bo *bo, uint32_t size)
{
   assert(bo && size <= bo->size);

   bool ok = v3dv_bo_map_unsynchronized(device, bo, size);
   if (!ok)
      return false;

   const uint64_t infinite = 0xffffffffffffffffull;
   ok = v3dv_bo_wait(device, bo, infinite);
   if (!ok) {
      fprintf(stderr, "memory wait for map failed\n");
      return false;
   }

   return true;
}

void
v3dv_bo_unmap(struct v3dv_device *device, struct v3dv_bo *bo)
{
   assert(bo && bo->map && bo->map_size > 0);

   munmap(bo->map, bo->map_size);
   VG(VALGRIND_FREELIKE_BLOCK(bo->map, 0));
   bo->map = NULL;
   bo->map_size = 0;
}


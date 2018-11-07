/*
 * Copyright © 2018 Google, Inc.
 * Copyright © 2015 Intel Corporation
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

#include <stdint.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <msm_drm.h>

#include "tu_private.h"

static int
tu_ioctl(int fd, unsigned long request, void *arg)
{
   int ret;

   do {
      ret = ioctl(fd, request, arg);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

/**
 * Return gem handle on success. Return 0 on failure.
 */
uint32_t
tu_gem_new(struct tu_device *dev, uint64_t size, uint32_t flags)
{
   struct drm_msm_gem_new req = {
      .size = size,
      .flags = flags,
   };


   int ret = tu_ioctl(dev->physical_device->local_fd, DRM_MSM_GEM_NEW, &req);
   if (ret)
      return 0;

   return req.handle;
}

void
tu_gem_close(struct tu_device *dev, uint32_t gem_handle)
{
   struct drm_gem_close req = {
      .handle = gem_handle,
   };

   tu_ioctl(dev->physical_device->local_fd, DRM_IOCTL_GEM_CLOSE, &req);
}

/** Return UINT64_MAX on error. */
static uint64_t
tu_gem_info(struct tu_device *dev, uint32_t gem_handle, uint32_t flags)
{
   struct drm_msm_gem_info req = {
      .handle = gem_handle,
      .flags = flags,
   };

   int ret = tu_ioctl(dev->physical_device->local_fd, DRM_MSM_GEM_INFO, &req);
   if (ret == -1)
      return UINT64_MAX;

   return req.offset;
}

/** Return UINT64_MAX on error. */
uint64_t
tu_gem_info_offset(struct tu_device *dev, uint32_t gem_handle)
{
   return tu_gem_info(dev, gem_handle, 0);
}

/** Return UINT64_MAX on error. */
uint64_t
tu_gem_info_iova(struct tu_device *dev, uint32_t gem_handle)
{
   return tu_gem_info(dev, gem_handle, MSM_INFO_IOVA);
}

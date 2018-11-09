/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iris_fence.c
 *
 * Fences for driver and IPC serialisation, scheduling and synchronisation.
 */

#include "util/u_inlines.h"

#include "iris_batch.h"
#include "iris_bufmgr.h"
#include "iris_fence.h"
#include "iris_screen.h"

static uint32_t
gem_syncobj_create(int fd, uint32_t flags)
{
   struct drm_syncobj_create args = {
      .flags = flags,
   };

   drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args);

   return args.handle;
}

static void
gem_syncobj_destroy(int fd, uint32_t handle)
{
   struct drm_syncobj_destroy args = {
      .handle = handle,
   };

   drm_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

/**
 * Make a new sync-point.
 */
struct iris_syncpt *
iris_create_syncpt(struct iris_screen *screen)
{
   struct iris_syncpt *syncpt = malloc(sizeof(*syncpt));

   if (!syncpt)
      return NULL;

   syncpt->handle = gem_syncobj_create(screen->fd, 0);
   assert(syncpt->handle);

   pipe_reference_init(&syncpt->ref, 1);

   return syncpt;
}

void
iris_syncpt_destroy(struct iris_screen *screen, struct iris_syncpt *syncpt)
{
   gem_syncobj_destroy(screen->fd, syncpt->handle);
   free(syncpt);
}

/**
 * Add a sync-point to the batch, with the given flags.
 *
 * \p flags   One of I915_EXEC_FENCE_WAIT or I915_EXEC_FENCE_SIGNAL.
 */
void
iris_batch_add_syncpt(struct iris_batch *batch,
                      struct iris_syncpt *syncpt,
                      unsigned flags)
{
   struct drm_i915_gem_exec_fence *fence =
      util_dynarray_grow(&batch->exec_fences, sizeof(*fence));

   *fence = (struct drm_i915_gem_exec_fence) {
      .handle = syncpt->handle,
      .flags = flags,
   };

   struct iris_syncpt **store =
      util_dynarray_grow(&batch->syncpts, sizeof(*store));

   *store = NULL;
   iris_syncpt_reference(batch->screen, store, syncpt);
}

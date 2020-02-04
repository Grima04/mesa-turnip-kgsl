/*
 * Copyright (C) 2017-2019 Lima Project
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

#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "drm-uapi/lima_drm.h"

#include "util/ralloc.h"
#include "util/u_dynarray.h"
#include "util/os_time.h"

#include "lima_screen.h"
#include "lima_context.h"
#include "lima_submit.h"
#include "lima_bo.h"
#include "lima_util.h"

struct lima_submit {
   int fd;
   struct lima_context *ctx;

   struct util_dynarray gem_bos[2];
   struct util_dynarray bos[2];
};


#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

struct lima_submit *lima_submit_create(struct lima_context *ctx)
{
   struct lima_submit *s;

   s = rzalloc(ctx, struct lima_submit);
   if (!s)
      return NULL;

   s->fd = lima_screen(ctx->base.screen)->fd;
   s->ctx = ctx;

   for (int i = 0; i < 2; i++) {
      util_dynarray_init(s->gem_bos + i, s);
      util_dynarray_init(s->bos + i, s);
   }

   return s;
}

void lima_submit_free(struct lima_submit *submit)
{

}

bool lima_submit_add_bo(struct lima_submit *submit, int pipe,
                        struct lima_bo *bo, uint32_t flags)
{
   util_dynarray_foreach(submit->gem_bos + pipe, struct drm_lima_gem_submit_bo, gem_bo) {
      if (bo->handle == gem_bo->handle) {
         gem_bo->flags |= flags;
         return true;
      }
   }

   struct drm_lima_gem_submit_bo *submit_bo =
      util_dynarray_grow(submit->gem_bos + pipe, struct drm_lima_gem_submit_bo, 1);
   submit_bo->handle = bo->handle;
   submit_bo->flags = flags;

   struct lima_bo **jbo = util_dynarray_grow(submit->bos + pipe, struct lima_bo *, 1);
   *jbo = bo;

   /* prevent bo from being freed when submit start */
   lima_bo_reference(bo);

   return true;
}

bool lima_submit_start(struct lima_submit *submit, int pipe, void *frame, uint32_t size)
{
   struct lima_context *ctx = submit->ctx;
   struct drm_lima_gem_submit req = {
      .ctx = ctx->id,
      .pipe = pipe,
      .nr_bos = submit->gem_bos[pipe].size / sizeof(struct drm_lima_gem_submit_bo),
      .bos = VOID2U64(util_dynarray_begin(submit->gem_bos + pipe)),
      .frame = VOID2U64(frame),
      .frame_size = size,
      .out_sync = ctx->out_sync[pipe],
   };

   if (ctx->in_sync_fd >= 0) {
      int err = drmSyncobjImportSyncFile(submit->fd, ctx->in_sync[pipe],
                                         ctx->in_sync_fd);
      if (err)
         return false;

      req.in_sync[0] = ctx->in_sync[pipe];
      close(ctx->in_sync_fd);
      ctx->in_sync_fd = -1;
   }

   bool ret = drmIoctl(submit->fd, DRM_IOCTL_LIMA_GEM_SUBMIT, &req) == 0;

   util_dynarray_foreach(submit->bos + pipe, struct lima_bo *, bo) {
      lima_bo_unreference(*bo);
   }

   util_dynarray_clear(submit->gem_bos + pipe);
   util_dynarray_clear(submit->bos + pipe);
   return ret;
}

bool lima_submit_wait(struct lima_submit *submit, int pipe, uint64_t timeout_ns)
{
   int64_t abs_timeout = os_time_get_absolute_timeout(timeout_ns);
   if (abs_timeout == OS_TIMEOUT_INFINITE)
      abs_timeout = INT64_MAX;

   struct lima_context *ctx = submit->ctx;
   return !drmSyncobjWait(submit->fd, ctx->out_sync + pipe, 1, abs_timeout, 0, NULL);
}

bool lima_submit_has_bo(struct lima_submit *submit, struct lima_bo *bo, bool all)
{
   for (int i = 0; i < 2; i++) {
      util_dynarray_foreach(submit->gem_bos + i, struct drm_lima_gem_submit_bo, gem_bo) {
         if (bo->handle == gem_bo->handle) {
            if (all || gem_bo->flags & LIMA_SUBMIT_BO_WRITE)
               return true;
            else
               break;
         }
      }
   }

   return false;
}

bool lima_submit_init(struct lima_context *ctx)
{
   int fd = lima_screen(ctx->base.screen)->fd;

   ctx->in_sync_fd = -1;

   for (int i = 0; i < 2; i++) {
      if (drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, ctx->in_sync + i) ||
          drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, ctx->out_sync + i))
         return false;
   }

   return true;
}

void lima_submit_fini(struct lima_context *ctx)
{
   int fd = lima_screen(ctx->base.screen)->fd;

   for (int i = 0; i < 2; i++) {
      if (ctx->in_sync[i])
         drmSyncobjDestroy(fd, ctx->in_sync[i]);
      if (ctx->out_sync[i])
         drmSyncobjDestroy(fd, ctx->out_sync[i]);
   }

   if (ctx->in_sync_fd >= 0)
      close(ctx->in_sync_fd);
}

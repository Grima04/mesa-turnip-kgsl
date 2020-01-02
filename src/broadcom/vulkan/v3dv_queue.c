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
#include "drm-uapi/v3d_drm.h"

#include "broadcom/clif/clif_dump.h"

#include <errno.h>

static void
v3dv_clif_dump(struct v3dv_queue *queue,
               struct v3dv_cmd_buffer *cmd_buffer,
               struct drm_v3d_submit_cl *submit)
{
   if (!(V3D_DEBUG & (V3D_DEBUG_CL | V3D_DEBUG_CLIF)))
      return;

   struct clif_dump *clif = clif_dump_init(&queue->device->devinfo,
                                           stderr,
                                           V3D_DEBUG & V3D_DEBUG_CL);

   set_foreach(cmd_buffer->bos, entry) {
      struct v3dv_bo *bo = (void *)entry->key;
      char *name = ralloc_asprintf(NULL, "%s_0x%x",
                                   "" /* bo->name */ , bo->offset);

      v3dv_bo_map(queue->device, bo, bo->size);
      clif_dump_add_bo(clif, name, bo->offset, bo->size, bo->map);

      ralloc_free(name);
   }

   clif_dump(clif, submit);

   clif_dump_destroy(clif);
}

static VkResult
queue_submit(struct v3dv_queue *queue,
             const VkSubmitInfo *pSubmit,
             VkFence fence)
{
   /* FIXME */
   assert(fence == 0);
   assert(pSubmit->waitSemaphoreCount == 0);
   assert(pSubmit->signalSemaphoreCount == 0);
   assert(pSubmit->commandBufferCount == 1);

   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, pSubmit->pCommandBuffers[0]);

   struct drm_v3d_submit_cl submit;

   /* While the RCL will implicitly depend on the last RCL to have finished, we
    * also need to block on any previous TFU job we may have dispatched.
    */
   submit.in_sync_rcl = 0; /* FIXME */

   /* Update the sync object for the last rendering by our context. */
   submit.out_sync = 0; /* FIXME */

   submit.bcl_start = cmd_buffer->bcl.bo->offset;
   submit.bcl_end = cmd_buffer->bcl.bo->offset + v3dv_cl_offset(&cmd_buffer->bcl);
   submit.rcl_start = cmd_buffer->rcl.bo->offset;
   submit.rcl_end = cmd_buffer->rcl.bo->offset + v3dv_cl_offset(&cmd_buffer->rcl);

   submit.flags = 0;
   /* FIXME: we already know that we support cache flush, as we only support
    * hw that supports that, but would be better to just DRM-ask it
    */
   if (cmd_buffer->state.tmu_dirty_rcl)
      submit.flags |= DRM_V3D_SUBMIT_CL_FLUSH_CACHE;

   submit.qma = cmd_buffer->tile_alloc->offset;
   submit.qms = cmd_buffer->tile_alloc->size;
   submit.qts = cmd_buffer->tile_state->offset;

   submit.bo_handle_count = cmd_buffer->bo_count;
   uint32_t *bo_handles =
      (uint32_t *) malloc(sizeof(uint32_t) * MAX2(4, submit.bo_handle_count * 2));
   uint32_t bo_idx = 0;
   set_foreach(cmd_buffer->bos, entry) {
      struct v3dv_bo *bo = (struct v3dv_bo *)entry->key;
      bo_handles[bo_idx++] = bo->handle;
   }
   assert(bo_idx == submit.bo_handle_count);
   submit.bo_handles = (uintptr_t)(void *)bo_handles;

   v3dv_clif_dump(queue, cmd_buffer, &submit);

   int ret = v3dv_ioctl(queue->device->fd, DRM_IOCTL_V3D_SUBMIT_CL, &submit);
   static bool warned = false;
   if (ret && !warned) {
      fprintf(stderr, "Draw call returned %s. Expect corruption.\n",
              strerror(errno));
      warned = true;
   }

   free(bo_handles);

   if (ret)
      return VK_ERROR_DEVICE_LOST;

   return VK_SUCCESS;
}

VkResult
v3dv_QueueSubmit(VkQueue _queue,
                 uint32_t submitCount,
                 const VkSubmitInfo* pSubmits,
                 VkFence fence)
{
   V3DV_FROM_HANDLE(v3dv_queue, queue, _queue);

   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < submitCount; i++) {
      result = queue_submit(queue, &pSubmits[i], fence);
      if (result != VK_SUCCESS)
         break;
   }

   return result;
}

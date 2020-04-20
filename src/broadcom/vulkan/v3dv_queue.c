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
#include <time.h>

static void
v3dv_clif_dump(struct v3dv_device *device,
               struct v3dv_job *job,
               struct drm_v3d_submit_cl *submit)
{
   if (!(V3D_DEBUG & (V3D_DEBUG_CL | V3D_DEBUG_CLIF)))
      return;

   struct clif_dump *clif = clif_dump_init(&device->devinfo,
                                           stderr,
                                           V3D_DEBUG & V3D_DEBUG_CL);

   set_foreach(job->bos, entry) {
      struct v3dv_bo *bo = (void *)entry->key;
      char *name = ralloc_asprintf(NULL, "%s_0x%x",
                                   bo->name, bo->offset);

      v3dv_bo_map(device, bo, bo->size);
      clif_dump_add_bo(clif, name, bo->offset, bo->size, bo->map);

      ralloc_free(name);
   }

   clif_dump(clif, submit);

   clif_dump_destroy(clif);
}

static uint64_t
gettime_ns()
{
   struct timespec current;
   clock_gettime(CLOCK_MONOTONIC, &current);
   return (uint64_t)current.tv_sec * NSEC_PER_SEC + current.tv_nsec;
}

static uint64_t
get_absolute_timeout(uint64_t timeout)
{
   uint64_t current_time = gettime_ns();
   uint64_t max_timeout = (uint64_t) INT64_MAX - current_time;

   timeout = MIN2(max_timeout, timeout);

   return (current_time + timeout);
}

static VkResult
handle_reset_query_cpu_job(struct v3dv_job *job)
{
   /* We are about to reset query counters so we need to make sure that
    * The GPU is not using them.
    *
    * FIXME: we could avoid blocking the main thread for this if we use
    *        submission thread.
    */
   v3dv_DeviceWaitIdle(v3dv_device_to_handle(job->device));

   struct v3dv_reset_query_cpu_job_info *info = &job->cpu.query_reset;
   for (uint32_t i = info->first; i < info->first + info->count; i++) {
      assert(i < info->pool->query_count);
      struct v3dv_query *query = &info->pool->queries[i];
      query->maybe_available = false;
      uint32_t *counter = (uint32_t *) query->bo->map;
      *counter = 0;
   }

   return VK_SUCCESS;
}

static VkResult
handle_end_query_cpu_job(struct v3dv_job *job)
{
   struct v3dv_end_query_cpu_job_info *info = &job->cpu.query_end;
   assert(info->query < info->pool->query_count);
   struct v3dv_query *query = &info->pool->queries[info->query];
   query->maybe_available = true;

   return VK_SUCCESS;
}

static VkResult
handle_copy_query_results_cpu_job(struct v3dv_job *job)
{
   struct v3dv_copy_query_results_cpu_job_info *info =
      &job->cpu.query_copy_results;

   assert(info->dst && info->dst->mem && info->dst->mem->bo);
   struct v3dv_bo *bo = info->dst->mem->bo;
   const uint32_t bo_size = info->dst->size;

   /* Map the entire dst buffer for the CPU copy */
   bool dst_was_mapped = bo->map != NULL;
   uint32_t map_size = bo->map_size;
   bool needs_map = false;
   if (!dst_was_mapped) {
      needs_map = true;
   } else if (map_size < bo_size) {
      v3dv_bo_unmap(job->device, bo);
      needs_map = true;
   }
   if (needs_map && !v3dv_bo_map(job->device, bo, bo_size))
      return vk_error(job->device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* FIXME: if flags includes VK_QUERY_RESULT_WAIT_BIT this could trigger a
    * sync wait on the CPU for the corresponding GPU jobs to finish. We might
    * want to use a submission thread to avoid blocking on the main thread.
    */
   v3dv_get_query_pool_results_cpu(job->device,
                                   info->pool,
                                   info->first,
                                   info->count,
                                   bo->map + info->dst->mem_offset,
                                   info->stride,
                                   info->flags);

   if (needs_map) {
      v3dv_bo_unmap(job->device, bo);
      if (dst_was_mapped) {
         if (!v3dv_bo_map(job->device, bo, map_size))
            return vk_error(job->device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   return VK_SUCCESS;
}

static VkResult
process_semaphores_to_signal(struct v3dv_device *device,
                             uint32_t count, const VkSemaphore *sems)
{
   if (count == 0)
      return VK_SUCCESS;

   int fd;
   drmSyncobjExportSyncFile(device->render_fd, device->last_job_sync, &fd);
   if (fd == -1)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < count; i++) {
      struct v3dv_semaphore *sem = v3dv_semaphore_from_handle(sems[i]);

      if (sem->fd >= 0)
         close(sem->fd);
      sem->fd = -1;

      int ret = drmSyncobjImportSyncFile(device->render_fd, sem->sync, fd);
      if (ret)
         return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

      sem->fd = fd;
   }

   return VK_SUCCESS;
}

static VkResult
process_fence_to_signal(struct v3dv_device *device, VkFence _fence)
{
   if (_fence == VK_NULL_HANDLE)
      return VK_SUCCESS;

   struct v3dv_fence *fence = v3dv_fence_from_handle(_fence);

   if (fence->fd >= 0)
      close(fence->fd);
   fence->fd = -1;

   int fd;
   drmSyncobjExportSyncFile(device->render_fd, device->last_job_sync, &fd);
   if (fd == -1)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   int ret = drmSyncobjImportSyncFile(device->render_fd, fence->sync, fd);
   if (ret)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   fence->fd = fd;

   return VK_SUCCESS;
}

static VkResult
handle_cl_job(struct v3dv_queue *queue,
              struct v3dv_job *job,
              bool do_wait)
{
   struct v3dv_device *device = queue->device;

   struct drm_v3d_submit_cl submit;

   /* RCL jobs don't start until the previous RCL job has finished so we don't
    * really need to add a fence for those, however, we might need to wait on a
    * CSD or TFU job, which are not serialized.
    *
    * FIXME: for now, if we are asked to wait on any semaphores, we just wait
    * on the last job we submitted. In the future we might want to pass the
    * actual syncobj of the wait semaphores so we don't block on the last RCL
    * if we only need to wait for a previous CSD or TFU, for example, but
    * we would have to extend our kernel interface to support the case where
    * we have more than one semaphore to wait on.
    */
   submit.in_sync_bcl = 0;
   submit.in_sync_rcl = do_wait ? device->last_job_sync : 0;

   /* Update the sync object for the last rendering by this device. */
   submit.out_sync = device->last_job_sync;

   submit.bcl_start = job->bcl.bo->offset;
   submit.bcl_end = job->bcl.bo->offset + v3dv_cl_offset(&job->bcl);
   submit.rcl_start = job->rcl.bo->offset;
   submit.rcl_end = job->rcl.bo->offset + v3dv_cl_offset(&job->rcl);

   submit.flags = 0;
   /* FIXME: we already know that we support cache flush, as we only support
    * hw that supports that, but would be better to just DRM-ask it
    */
   if (job->tmu_dirty_rcl)
      submit.flags |= DRM_V3D_SUBMIT_CL_FLUSH_CACHE;

   submit.qma = job->tile_alloc->offset;
   submit.qms = job->tile_alloc->size;
   submit.qts = job->tile_state->offset;

   submit.bo_handle_count = job->bo_count;
   uint32_t *bo_handles =
      (uint32_t *) malloc(sizeof(uint32_t) * MAX2(4, submit.bo_handle_count * 2));
   uint32_t bo_idx = 0;
   set_foreach(job->bos, entry) {
      struct v3dv_bo *bo = (struct v3dv_bo *)entry->key;
      bo_handles[bo_idx++] = bo->handle;
   }
   assert(bo_idx == submit.bo_handle_count);
   submit.bo_handles = (uintptr_t)(void *)bo_handles;

   v3dv_clif_dump(device, job, &submit);

   int ret = v3dv_ioctl(device->render_fd, DRM_IOCTL_V3D_SUBMIT_CL, &submit);
   static bool warned = false;
   if (ret && !warned) {
      fprintf(stderr, "Draw call returned %s. Expect corruption.\n",
              strerror(errno));
      warned = true;
   }

   free(bo_handles);

   if (ret)
      return vk_error(device->instance, VK_ERROR_DEVICE_LOST);

   return VK_SUCCESS;
}

static VkResult
handle_tfu_job(struct v3dv_queue *queue,
               struct v3dv_job *job,
               bool do_wait)
{
   const struct v3dv_device *device = queue->device;

   job->tfu.in_sync = do_wait ? device->last_job_sync : 0;
   job->tfu.out_sync = device->last_job_sync;

   int ret = v3dv_ioctl(device->render_fd, DRM_IOCTL_V3D_SUBMIT_TFU, &job->tfu);
   if (ret != 0) {
      fprintf(stderr, "Failed to submit TFU job: %d\n", ret);
      return vk_error(device->instance, VK_ERROR_DEVICE_LOST);
   }

   return VK_SUCCESS;
}

static VkResult
queue_submit_job(struct v3dv_queue *queue,
                 struct v3dv_job *job,
                 bool do_wait)
{
   assert(job);

   switch (job->type) {
   case V3DV_JOB_TYPE_GPU_CL:
      return handle_cl_job(queue, job, do_wait);
   case V3DV_JOB_TYPE_GPU_TFU:
      return handle_tfu_job(queue, job, do_wait);
   case V3DV_JOB_TYPE_CPU_RESET_QUERIES:
      return handle_reset_query_cpu_job(job);
   case V3DV_JOB_TYPE_CPU_END_QUERY:
      return handle_end_query_cpu_job(job);
   case V3DV_JOB_TYPE_CPU_COPY_QUERY_RESULTS:
      return handle_copy_query_results_cpu_job(job);
   default:
      unreachable("Unhandled job type");
   }
}

static void
emit_noop_bin(struct v3dv_job *job)
{
   v3dv_job_start_frame(job, 1, 1, 1, 1, V3D_INTERNAL_BPP_32);
   v3dv_job_emit_binning_flush(job);
}

static void
emit_noop_render(struct v3dv_job *job)
{
   struct v3dv_cl *rcl = &job->rcl;
   v3dv_cl_ensure_space_with_branch(rcl, 200 + 1 * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.early_z_disable = true;
      config.image_width_pixels = 1;
      config.image_height_pixels = 1;
      config.number_of_render_targets = 1;
      config.multisample_mode_4x = false;
      config.maximum_bpp_of_all_render_targets = V3D_INTERNAL_BPP_32;
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
      rt.render_target_0_internal_bpp = V3D_INTERNAL_BPP_32;
      rt.render_target_0_internal_type = V3D_INTERNAL_TYPE_8;
      rt.render_target_0_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
      clear.z_clear_value = 1.0f;
      clear.stencil_clear_value = 0;
   };

   cl_emit(rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
      init.use_auto_chained_tile_lists = true;
      init.size_of_first_block_in_chained_tile_lists =
         TILE_ALLOCATION_BLOCK_SIZE_64B;
   }

   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(job->tile_alloc, 0);
   }

   cl_emit(rcl, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
      config.number_of_bin_tile_lists = 1;
      config.total_frame_width_in_tiles = 1;
      config.total_frame_height_in_tiles = 1;
      config.supertile_width_in_tiles = 1;
      config.supertile_height_in_tiles = 1;
      config.total_frame_width_in_supertiles = 1;
      config.total_frame_height_in_supertiles = 1;
   }

   struct v3dv_cl *icl = &job->indirect;
   v3dv_cl_ensure_space(icl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(icl);

   cl_emit(icl, TILE_COORDINATES_IMPLICIT, coords);

   cl_emit(icl, END_OF_LOADS, end);

   cl_emit(icl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   cl_emit(icl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = NONE;
   }

   cl_emit(icl, END_OF_TILE_MARKER, end);

   cl_emit(icl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(icl);
   }

   cl_emit(rcl, SUPERTILE_COORDINATES, coords) {
      coords.column_number_in_supertiles = 0;
      coords.row_number_in_supertiles = 0;
   }

   cl_emit(rcl, END_OF_RENDERING, end);
}

static VkResult
queue_create_noop_job(struct v3dv_queue *queue, struct v3dv_job **job)
{
   struct v3dv_device *device = queue->device;
   *job = vk_zalloc(&device->alloc, sizeof(struct v3dv_job), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!*job)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   v3dv_job_init(*job, V3DV_JOB_TYPE_GPU_CL, device, NULL, -1);

   emit_noop_bin(*job);
   emit_noop_render(*job);

   return VK_SUCCESS;
}

static VkResult
queue_submit_noop_job(struct v3dv_queue *queue, const VkSubmitInfo *pSubmit)
{
   /* VkQueue host access is externally synchronized so we don't need to lock
    * here for the static variable.
    */
   static struct v3dv_job *noop_job = NULL;

   if (!noop_job) {
      VkResult result = queue_create_noop_job(queue, &noop_job);
      if (result != VK_SUCCESS)
         return result;
   }

   return queue_submit_job(queue, noop_job, pSubmit->waitSemaphoreCount > 0);
}

static VkResult
queue_submit_cmd_buffer(struct v3dv_queue *queue,
                        struct v3dv_cmd_buffer *cmd_buffer,
                        const VkSubmitInfo *pSubmit)
{
   assert(cmd_buffer);

   if (list_is_empty(&cmd_buffer->submit_jobs))
      return queue_submit_noop_job(queue, pSubmit);

   list_for_each_entry_safe(struct v3dv_job, job,
                            &cmd_buffer->submit_jobs, list_link) {
      VkResult result = queue_submit_job(queue, job,
                                         pSubmit->waitSemaphoreCount > 0);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
queue_submit_cmd_buffer_batch(struct v3dv_queue *queue,
                              const VkSubmitInfo *pSubmit,
                              VkFence fence)
{
   VkResult result = VK_SUCCESS;

   /* Even if we don't have any actual work to submit we still need to wait
    * on the wait semaphores and signal the signal semaphores and fence, so
    * in this scenario we just submit a trivial no-op job so we don't have
    * to do anything special, it should not be a common case anyway.
    */
   if (pSubmit->commandBufferCount == 0) {
      result = queue_submit_noop_job(queue, pSubmit);
   } else {
      for (uint32_t i = 0; i < pSubmit->commandBufferCount; i++) {
         struct v3dv_cmd_buffer *cmd_buffer =
            v3dv_cmd_buffer_from_handle(pSubmit->pCommandBuffers[i]);
         result = queue_submit_cmd_buffer(queue, cmd_buffer, pSubmit);
         if (result != VK_SUCCESS)
            break;
      }
   }

   if (result != VK_SUCCESS)
      return result;

   result = process_semaphores_to_signal(queue->device,
                                         pSubmit->signalSemaphoreCount,
                                         pSubmit->pSignalSemaphores);
   if (result != VK_SUCCESS)
      return result;

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
      result = queue_submit_cmd_buffer_batch(queue, &pSubmits[i], fence);
      if (result != VK_SUCCESS)
         return result;
   }

   result = process_fence_to_signal(queue->device, fence);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

VkResult
v3dv_CreateSemaphore(VkDevice _device,
                     const VkSemaphoreCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkSemaphore *pSemaphore)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);

   struct v3dv_semaphore *sem =
      vk_alloc2(&device->alloc, pAllocator, sizeof(struct v3dv_semaphore), 8,
               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (sem == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   sem->fd = -1;

   int ret = drmSyncobjCreate(device->render_fd, 0, &sem->sync);
   if (ret) {
      vk_free2(&device->alloc, pAllocator, sem);
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   *pSemaphore = v3dv_semaphore_to_handle(sem);

   return VK_SUCCESS;
}

void
v3dv_DestroySemaphore(VkDevice _device,
                      VkSemaphore semaphore,
                      const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_semaphore, sem, semaphore);

   if (sem == NULL)
      return;

   drmSyncobjDestroy(device->render_fd, sem->sync);

   if (sem->fd != -1)
      close(sem->fd);

   vk_free2(&device->alloc, pAllocator, sem);
}

VkResult
v3dv_CreateFence(VkDevice _device,
                 const VkFenceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkFence *pFence)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);

   struct v3dv_fence *fence =
      vk_alloc2(&device->alloc, pAllocator, sizeof(struct v3dv_fence), 8,
               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (fence == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   unsigned flags = 0;
   if (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT)
      flags |= DRM_SYNCOBJ_CREATE_SIGNALED;
   int ret = drmSyncobjCreate(device->render_fd, flags, &fence->sync);
   if (ret) {
      vk_free2(&device->alloc, pAllocator, fence);
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   fence->fd = -1;

   *pFence = v3dv_fence_to_handle(fence);

   return VK_SUCCESS;
}

void
v3dv_DestroyFence(VkDevice _device,
                  VkFence _fence,
                  const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_fence, fence, _fence);

   if (fence == NULL)
      return;

   drmSyncobjDestroy(device->render_fd, fence->sync);

   if (fence->fd != -1)
      close(fence->fd);

   vk_free2(&device->alloc, pAllocator, fence);
}

VkResult
v3dv_GetFenceStatus(VkDevice _device, VkFence _fence)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_fence, fence, _fence);

   int ret = drmSyncobjWait(device->render_fd, &fence->sync, 1,
                            0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
   if (ret == -ETIME)
      return VK_NOT_READY;
   else if (ret)
      return vk_error(device->instance, VK_ERROR_DEVICE_LOST);
   return VK_SUCCESS;
}

VkResult
v3dv_ResetFences(VkDevice _device, uint32_t fenceCount, const VkFence *pFences)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   uint32_t *syncobjs = vk_alloc(&device->alloc,
                                 sizeof(*syncobjs) * fenceCount, 8,
                                 VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!syncobjs)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < fenceCount; i++) {
      struct v3dv_fence *fence = v3dv_fence_from_handle(pFences[i]);
      syncobjs[i] = fence->sync;
   }

   int ret = drmSyncobjReset(device->render_fd, syncobjs, fenceCount);

   vk_free(&device->alloc, syncobjs);

   if (ret)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   return VK_SUCCESS;
}

VkResult
v3dv_WaitForFences(VkDevice _device,
                   uint32_t fenceCount,
                   const VkFence *pFences,
                   VkBool32 waitAll,
                   uint64_t timeout)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   const uint64_t abs_timeout = get_absolute_timeout(timeout);

   uint32_t *syncobjs = vk_alloc(&device->alloc,
                                 sizeof(*syncobjs) * fenceCount, 8,
                                 VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!syncobjs)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < fenceCount; i++) {
      struct v3dv_fence *fence = v3dv_fence_from_handle(pFences[i]);
      syncobjs[i] = fence->sync;
   }

   unsigned flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
   if (waitAll)
      flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;

   int ret;
   do {
      ret = drmSyncobjWait(device->render_fd, syncobjs, fenceCount,
                           timeout, flags, NULL);
   } while (ret == -ETIME && gettime_ns() < abs_timeout);

   vk_free(&device->alloc, syncobjs);

   if (ret == -ETIME)
      return VK_TIMEOUT;
   else if (ret)
      return vk_error(device->instance, VK_ERROR_DEVICE_LOST);
   return VK_SUCCESS;
}

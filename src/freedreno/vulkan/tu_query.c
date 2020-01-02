/*
 * Copyrigh 2016 Red Hat Inc.
 * Based on anv:
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_private.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "registers/adreno_pm4.xml.h"
#include "registers/adreno_common.xml.h"
#include "registers/a6xx.xml.h"

#include "nir/nir_builder.h"

#include "tu_cs.h"

/* It seems like sample counts need to be copied over to 16-byte aligned
 * memory. */
struct PACKED slot_value {
   uint64_t value;
   uint64_t __padding;
};

struct PACKED occlusion_query_slot {
   struct slot_value available; /* 0 when unavailable, 1 when available */
   struct slot_value begin;
   struct slot_value end;
   struct slot_value result;
};

/* Returns the IOVA of a given uint64_t field in a given slot of a query
 * pool. */
#define query_iova(type, pool, query, field)                         \
   pool->bo.iova + pool->stride * query + offsetof(type, field) +    \
         offsetof(struct slot_value, value)

#define occlusion_query_iova(pool, query, field)                     \
   query_iova(struct occlusion_query_slot, pool, query, field)

VkResult
tu_CreateQueryPool(VkDevice _device,
                   const VkQueryPoolCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkQueryPool *pQueryPool)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
   assert(pCreateInfo->queryCount > 0);

   uint32_t slot_size;
   switch (pCreateInfo->queryType) {
   case VK_QUERY_TYPE_OCCLUSION:
      slot_size = sizeof(struct occlusion_query_slot);
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   case VK_QUERY_TYPE_TIMESTAMP:
      unreachable("Unimplemented query type");
   default:
      assert(!"Invalid query type");
   }

   struct tu_query_pool *pool =
      vk_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (!pool)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = tu_bo_init_new(device, &pool->bo,
         pCreateInfo->queryCount * slot_size);
   if (result != VK_SUCCESS) {
      vk_free2(&device->alloc, pAllocator, pool);
      return result;
   }

   result = tu_bo_map(device, &pool->bo);
   if (result != VK_SUCCESS) {
      tu_bo_finish(device, &pool->bo);
      vk_free2(&device->alloc, pAllocator, pool);
      return result;
   }

   /* Initialize all query statuses to unavailable */
   memset(pool->bo.map, 0, pool->bo.size);

   pool->type = pCreateInfo->queryType;
   pool->stride = slot_size;
   pool->size = pCreateInfo->queryCount;
   pool->pipeline_statistics = pCreateInfo->pipelineStatistics;
   *pQueryPool = tu_query_pool_to_handle(pool);

   return VK_SUCCESS;
}

void
tu_DestroyQueryPool(VkDevice _device,
                    VkQueryPool _pool,
                    const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_query_pool, pool, _pool);

   if (!pool)
      return;

   tu_bo_finish(device, &pool->bo);
   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult
tu_GetQueryPoolResults(VkDevice _device,
                       VkQueryPool queryPool,
                       uint32_t firstQuery,
                       uint32_t queryCount,
                       size_t dataSize,
                       void *pData,
                       VkDeviceSize stride,
                       VkQueryResultFlags flags)
{
   return VK_SUCCESS;
}

void
tu_CmdCopyQueryPoolResults(VkCommandBuffer commandBuffer,
                           VkQueryPool queryPool,
                           uint32_t firstQuery,
                           uint32_t queryCount,
                           VkBuffer dstBuffer,
                           VkDeviceSize dstOffset,
                           VkDeviceSize stride,
                           VkQueryResultFlags flags)
{
}

void
tu_CmdResetQueryPool(VkCommandBuffer commandBuffer,
                     VkQueryPool queryPool,
                     uint32_t firstQuery,
                     uint32_t queryCount)
{
}

static void
emit_begin_occlusion_query(struct tu_cmd_buffer *cmdbuf,
                           struct tu_query_pool *pool,
                           uint32_t query)
{
   /* From the Vulkan 1.1.130 spec:
    *
    *    A query must begin and end inside the same subpass of a render pass
    *    instance, or must both begin and end outside of a render pass
    *    instance.
    *
    * Unlike on an immediate-mode renderer, Turnip renders all tiles on
    * vkCmdEndRenderPass, not individually on each vkCmdDraw*. As such, if a
    * query begins/ends inside the same subpass of a render pass, we need to
    * record the packets on the secondary draw command stream. cmdbuf->draw_cs
    * is then run on every tile during render, so we just need to accumulate
    * sample counts in slot->result to compute the query result.
    */
   struct tu_cs *cs = cmdbuf->state.pass ? &cmdbuf->draw_cs : &cmdbuf->cs;

   uint64_t begin_iova = occlusion_query_iova(pool, query, begin);

   tu_cs_reserve_space(cmdbuf->device, cs, 7);
   tu_cs_emit_regs(cs,
                   A6XX_RB_SAMPLE_COUNT_CONTROL(.copy = true));

   tu_cs_emit_regs(cs,
                   A6XX_RB_SAMPLE_COUNT_ADDR_LO(begin_iova));

   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
   tu_cs_emit(cs, ZPASS_DONE);
}

void
tu_CmdBeginQuery(VkCommandBuffer commandBuffer,
                 VkQueryPool queryPool,
                 uint32_t query,
                 VkQueryControlFlags flags)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmdbuf, commandBuffer);
   TU_FROM_HANDLE(tu_query_pool, pool, queryPool);
   assert(query < pool->size);

   switch (pool->type) {
   case VK_QUERY_TYPE_OCCLUSION:
      /* In freedreno, there is no implementation difference between
       * GL_SAMPLES_PASSED and GL_ANY_SAMPLES_PASSED, so we can similarly
       * ignore the VK_QUERY_CONTROL_PRECISE_BIT flag here.
       */
      emit_begin_occlusion_query(cmdbuf, pool, query);
      break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
   case VK_QUERY_TYPE_TIMESTAMP:
      unreachable("Unimplemented query type");
   default:
      assert(!"Invalid query type");
   }

   tu_bo_list_add(&cmdbuf->bo_list, &pool->bo, MSM_SUBMIT_BO_WRITE);
}

void
tu_CmdEndQuery(VkCommandBuffer commandBuffer,
               VkQueryPool queryPool,
               uint32_t query)
{
}

void
tu_CmdWriteTimestamp(VkCommandBuffer commandBuffer,
                     VkPipelineStageFlagBits pipelineStage,
                     VkQueryPool queryPool,
                     uint32_t query)
{
}

/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
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

#include "main/menums.h"
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "spirv/nir_spirv.h"
#include "util/debug.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "vk_format.h"
#include "vk_util.h"

#include "tu_cs.h"

struct tu_pipeline_builder
{
   struct tu_device *device;
   struct tu_pipeline_cache *cache;
   const VkAllocationCallbacks *alloc;
   const VkGraphicsPipelineCreateInfo *create_info;
};

static VkResult
tu_pipeline_builder_create_pipeline(struct tu_pipeline_builder *builder,
                                    struct tu_pipeline **out_pipeline)
{
   struct tu_device *dev = builder->device;

   struct tu_pipeline *pipeline =
      vk_zalloc2(&dev->alloc, builder->alloc, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   tu_cs_init(&pipeline->cs, TU_CS_MODE_SUB_STREAM, 2048);

   /* reserve the space now such that tu_cs_begin_sub_stream never fails */
   VkResult result = tu_cs_reserve_space(dev, &pipeline->cs, 2048);
   if (result != VK_SUCCESS) {
      vk_free2(&dev->alloc, builder->alloc, pipeline);
      return result;
   }

   *out_pipeline = pipeline;

   return VK_SUCCESS;
}

static void
tu_pipeline_finish(struct tu_pipeline *pipeline,
                   struct tu_device *dev,
                   const VkAllocationCallbacks *alloc)
{
   tu_cs_finish(dev, &pipeline->cs);
}

static VkResult
tu_pipeline_builder_build(struct tu_pipeline_builder *builder,
                          struct tu_pipeline **pipeline)
{
   VkResult result = tu_pipeline_builder_create_pipeline(builder, pipeline);
   if (result != VK_SUCCESS)
      return result;

   /* we should have reserved enough space upfront such that the CS never
    * grows
    */
   assert((*pipeline)->cs.bo_count == 1);

   return VK_SUCCESS;
}

static void
tu_pipeline_builder_init_graphics(
   struct tu_pipeline_builder *builder,
   struct tu_device *dev,
   struct tu_pipeline_cache *cache,
   const VkGraphicsPipelineCreateInfo *create_info,
   const VkAllocationCallbacks *alloc)
{
   *builder = (struct tu_pipeline_builder) {
      .device = dev,
      .cache = cache,
      .create_info = create_info,
      .alloc = alloc,
   };
}

VkResult
tu_CreateGraphicsPipelines(VkDevice device,
                           VkPipelineCache pipelineCache,
                           uint32_t count,
                           const VkGraphicsPipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   TU_FROM_HANDLE(tu_device, dev, device);
   TU_FROM_HANDLE(tu_pipeline_cache, cache, pipelineCache);

   for (uint32_t i = 0; i < count; i++) {
      struct tu_pipeline_builder builder;
      tu_pipeline_builder_init_graphics(&builder, dev, cache,
                                        &pCreateInfos[i], pAllocator);

      struct tu_pipeline *pipeline;
      VkResult result = tu_pipeline_builder_build(&builder, &pipeline);

      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            tu_DestroyPipeline(device, pPipelines[j], pAllocator);
            pPipelines[j] = VK_NULL_HANDLE;
         }

         return result;
      }

      pPipelines[i] = tu_pipeline_to_handle(pipeline);
   }

   return VK_SUCCESS;
}

static VkResult
tu_compute_pipeline_create(VkDevice _device,
                           VkPipelineCache _cache,
                           const VkComputePipelineCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipeline)
{
   return VK_SUCCESS;
}

VkResult
tu_CreateComputePipelines(VkDevice _device,
                          VkPipelineCache pipelineCache,
                          uint32_t count,
                          const VkComputePipelineCreateInfo *pCreateInfos,
                          const VkAllocationCallbacks *pAllocator,
                          VkPipeline *pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      VkResult r;
      r = tu_compute_pipeline_create(_device, pipelineCache, &pCreateInfos[i],
                                     pAllocator, &pPipelines[i]);
      if (r != VK_SUCCESS) {
         result = r;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   return result;
}

void
tu_DestroyPipeline(VkDevice _device,
                   VkPipeline _pipeline,
                   const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, dev, _device);
   TU_FROM_HANDLE(tu_pipeline, pipeline, _pipeline);

   if (!_pipeline)
      return;

   tu_pipeline_finish(pipeline, dev, pAllocator);
   vk_free2(&dev->alloc, pAllocator, pipeline);
}

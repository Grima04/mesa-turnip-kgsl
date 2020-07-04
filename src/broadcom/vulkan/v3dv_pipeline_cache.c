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
#include "vulkan/util/vk_util.h"


static void
pipeline_cache_init(struct v3dv_pipeline_cache *cache,
                    struct v3dv_device *device)
{
   cache->_loader_data.loaderMagic = ICD_LOADER_MAGIC;

   cache->device = device;
   pthread_mutex_init(&cache->mutex, NULL);
}

static void
pipeline_cache_load(struct v3dv_pipeline_cache *cache,
                    size_t size,
                    const void *data)
{
   struct v3dv_device *device = cache->device;
   struct v3dv_physical_device *pdevice = &device->instance->physicalDevice;
   struct vk_pipeline_cache_header header;

   if (size < sizeof(header))
      return;
   memcpy(&header, data, sizeof(header));
   if (header.header_size < sizeof(header))
      return;
   if (header.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
      return;
   if (header.vendor_id != v3dv_physical_device_vendor_id(pdevice))
      return;
   if (header.device_id != v3dv_physical_device_device_id(pdevice))
      return;
   if (memcmp(header.uuid, pdevice->pipeline_cache_uuid, VK_UUID_SIZE) != 0)
      return;

   /* FIXME: at this point we only verify the header but we dont really load
    * any data. pending to implement serialize/deserialize among other things.
    */
}

VkResult
v3dv_CreatePipelineCache(VkDevice _device,
                         const VkPipelineCacheCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkPipelineCache *pPipelineCache)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_pipeline_cache *cache;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   cache = vk_alloc2(&device->alloc, pAllocator,
                     sizeof(*cache), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (cache == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline_cache_init(cache, device);

   if (pCreateInfo->initialDataSize > 0) {
      pipeline_cache_load(cache,
                          pCreateInfo->initialDataSize,
                          pCreateInfo->pInitialData);
   }

   *pPipelineCache = v3dv_pipeline_cache_to_handle(cache);

   return VK_SUCCESS;
}

void
v3dv_DestroyPipelineCache(VkDevice _device,
                          VkPipelineCache _cache,
                          const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_pipeline_cache, cache, _cache);

   if (!cache)
      return;

   pthread_mutex_destroy(&cache->mutex);

   vk_free2(&device->alloc, pAllocator, cache);
}

VkResult
v3dv_MergePipelineCaches(VkDevice device,
                         VkPipelineCache dstCache,
                         uint32_t srcCacheCount,
                         const VkPipelineCache *pSrcCaches)
{
   /* FIXME: at this point there are not other content that the header cache,
    * so merging pipeline caches would be always successful
    */
   return VK_SUCCESS;
}

VkResult
v3dv_GetPipelineCacheData(VkDevice _device,
                          VkPipelineCache _cache,
                          size_t *pDataSize,
                          void *pData)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_pipeline_cache, cache, _cache);
   struct v3dv_physical_device *pdevice = &device->instance->physicalDevice;
   struct vk_pipeline_cache_header *header;
   VkResult result = VK_SUCCESS;

   pthread_mutex_lock(&cache->mutex);

   /* FIXME: at this point the cache data is just the header */
   const size_t size = sizeof(*header);
   if (pData == NULL) {
      pthread_mutex_unlock(&cache->mutex);
      *pDataSize = size;
      return VK_SUCCESS;
   }
   if (*pDataSize < sizeof(*header)) {
      pthread_mutex_unlock(&cache->mutex);
      *pDataSize = 0;
      return VK_INCOMPLETE;
   }

   header = pData;
   header->header_size = sizeof(*header);
   header->header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
   header->vendor_id = v3dv_physical_device_vendor_id(pdevice);
   header->device_id = v3dv_physical_device_device_id(pdevice);
   memcpy(header->uuid, pdevice->pipeline_cache_uuid, VK_UUID_SIZE);

   pthread_mutex_unlock(&cache->mutex);
   return result;
}

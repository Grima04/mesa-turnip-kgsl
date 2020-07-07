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
#include "util/blob.h"
#include "nir/nir_serialize.h"

static const bool dump_stats = false;
static const bool dump_stats_verbose = false;

static uint32_t
sha1_hash_func(const void *sha1)
{
   return _mesa_hash_data(sha1, 20);
}

static bool
sha1_compare_func(const void *sha1_a, const void *sha1_b)
{
   return memcmp(sha1_a, sha1_b, 20) == 0;
}

struct serialized_nir {
   unsigned char sha1_key[20];
   size_t size;
   char data[0];
};

static void
cache_dump_stats(struct v3dv_pipeline_cache *cache)
{
   if (!dump_stats_verbose)
      return;

   fprintf(stderr, "  NIR cache entries:      %d\n", cache->nir_stats.count);
   fprintf(stderr, "  NIR cache miss count:   %d\n", cache->nir_stats.miss);
   fprintf(stderr, "  NIR cache hit  count:   %d\n", cache->nir_stats.hit);
}

void
v3dv_pipeline_cache_upload_nir(struct v3dv_pipeline *pipeline,
                               struct v3dv_pipeline_cache *cache,
                               nir_shader *nir,
                               unsigned char sha1_key[20])
{
   if (!cache || !cache->nir_cache)
      return;

   pthread_mutex_lock(&cache->mutex);
   struct hash_entry *entry =
      _mesa_hash_table_search(cache->nir_cache, sha1_key);
   pthread_mutex_unlock(&cache->mutex);
   if (entry)
      return;

   struct blob blob;
   blob_init(&blob);

   nir_serialize(&blob, nir, false);
   if (blob.out_of_memory) {
      blob_finish(&blob);
      return;
   }

   pthread_mutex_lock(&cache->mutex);
   /* Because ralloc isn't thread-safe, we have to do all this inside the
    * lock.  We could unlock for the big memcpy but it's probably not worth
    * the hassle.
    */
   entry = _mesa_hash_table_search(cache->nir_cache, sha1_key);
   if (entry) {
      blob_finish(&blob);
      pthread_mutex_unlock(&cache->mutex);
      return;
   }

   struct serialized_nir *snir =
      ralloc_size(cache->nir_cache, sizeof(*snir) + blob.size);
   memcpy(snir->sha1_key, sha1_key, 20);
   snir->size = blob.size;
   memcpy(snir->data, blob.data, blob.size);

   blob_finish(&blob);

   if (unlikely(dump_stats)) {
      char sha1buf[41];
      _mesa_sha1_format(sha1buf, snir->sha1_key);
      fprintf(stderr, "pipeline cache %p, new nir entry %s\n", cache, sha1buf);

      cache->nir_stats.count++;
      cache_dump_stats(cache);
   }

   _mesa_hash_table_insert(cache->nir_cache, snir->sha1_key, snir);

   pthread_mutex_unlock(&cache->mutex);
}

nir_shader*
v3dv_pipeline_cache_search_for_nir(struct v3dv_pipeline *pipeline,
                                   struct v3dv_pipeline_cache *cache,
                                   const nir_shader_compiler_options *nir_options,
                                   unsigned char sha1_key[20])
{
   if (!cache || !cache->nir_cache)
      return NULL;

   if (unlikely(dump_stats)) {
      char sha1buf[41];
      _mesa_sha1_format(sha1buf, sha1_key);

      fprintf(stderr, "pipeline cache %p, search for nir %s\n", cache, sha1buf);
   }

   const struct serialized_nir *snir = NULL;

   pthread_mutex_lock(&cache->mutex);
   struct hash_entry *entry =
      _mesa_hash_table_search(cache->nir_cache, sha1_key);
   if (entry)
      snir = entry->data;
   pthread_mutex_unlock(&cache->mutex);

   if (snir) {
      struct blob_reader blob;
      blob_reader_init(&blob, snir->data, snir->size);

      /* We use context NULL as we want the p_stage to keep the reference to
       * nir, as we keep open the possibility of provide a shader variant
       * after cache creation
       */
      nir_shader *nir = nir_deserialize(NULL, nir_options, &blob);
      if (blob.overrun) {
         ralloc_free(nir);
      } else {
         if (unlikely(dump_stats)) {
            cache->nir_stats.hit++;
            cache_dump_stats(cache);
         }
         return nir;
      }
   }

   if (unlikely(dump_stats)) {
      cache->nir_stats.miss++;
      cache_dump_stats(cache);
   }

   return NULL;
}

static void
pipeline_cache_init(struct v3dv_pipeline_cache *cache,
                    struct v3dv_device *device,
                    bool cache_enabled)
{
   cache->_loader_data.loaderMagic = ICD_LOADER_MAGIC;

   cache->device = device;
   pthread_mutex_init(&cache->mutex, NULL);

   if (cache_enabled) {
      cache->nir_cache = _mesa_hash_table_create(NULL, sha1_hash_func,
                                                 sha1_compare_func);
      cache->nir_stats.miss = 0;
      cache->nir_stats.hit = 0;
      cache->nir_stats.count = 0;
   } else {
      cache->nir_cache = NULL;
   }

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

   pipeline_cache_init(cache, device,
                       device->instance->pipeline_cache_enabled);

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

   if (cache->nir_cache) {
      hash_table_foreach(cache->nir_cache, entry)
         ralloc_free(entry->data);

      _mesa_hash_table_destroy(cache->nir_cache, NULL);
   }

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

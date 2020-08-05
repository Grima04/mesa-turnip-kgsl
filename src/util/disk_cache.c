/*
 * Copyright © 2014 Intel Corporation
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

#ifdef ENABLE_SHADER_CACHE

#include <ctype.h>
#include <ftw.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include "zlib.h"

#ifdef HAVE_ZSTD
#include "zstd.h"
#endif

#include "util/crc32.h"
#include "util/debug.h"
#include "util/rand_xor.h"
#include "util/u_atomic.h"
#include "util/mesa-sha1.h"
#include "util/ralloc.h"
#include "util/compiler.h"

#include "disk_cache.h"
#include "disk_cache_os.h"

/* The cache version should be bumped whenever a change is made to the
 * structure of cache entries or the index. This will give any 3rd party
 * applications reading the cache entries a chance to adjust to the changes.
 *
 * - The cache version is checked internally when reading a cache entry. If we
 *   ever have a mismatch we are in big trouble as this means we had a cache
 *   collision. In case of such an event please check the skys for giant
 *   asteroids and that the entire Mesa team hasn't been eaten by wolves.
 *
 * - There is no strict requirement that cache versions be backwards
 *   compatible but effort should be taken to limit disruption where possible.
 */
#define CACHE_VERSION 1

#define DRV_KEY_CPY(_dst, _src, _src_size) \
do {                                       \
   memcpy(_dst, _src, _src_size);          \
   _dst += _src_size;                      \
} while (0);

struct disk_cache *
disk_cache_create(const char *gpu_name, const char *driver_id,
                  uint64_t driver_flags)
{
   void *local;
   struct disk_cache *cache = NULL;
   char *max_size_str;
   uint64_t max_size;

   uint8_t cache_version = CACHE_VERSION;
   size_t cv_size = sizeof(cache_version);

   if (!disk_cache_enabled())
      return NULL;

   /* A ralloc context for transient data during this invocation. */
   local = ralloc_context(NULL);
   if (local == NULL)
      goto fail;

   cache = rzalloc(NULL, struct disk_cache);
   if (cache == NULL)
      goto fail;

   /* Assume failure. */
   cache->path_init_failed = true;

   char *path = disk_cache_generate_cache_dir(local);
   if (!path)
      goto path_fail;

   if (!disk_cache_mmap_cache_index(local, cache, path))
      goto path_fail;

   max_size = 0;

   max_size_str = getenv("MESA_GLSL_CACHE_MAX_SIZE");
   if (max_size_str) {
      char *end;
      max_size = strtoul(max_size_str, &end, 10);
      if (end == max_size_str) {
         max_size = 0;
      } else {
         switch (*end) {
         case 'K':
         case 'k':
            max_size *= 1024;
            break;
         case 'M':
         case 'm':
            max_size *= 1024*1024;
            break;
         case '\0':
         case 'G':
         case 'g':
         default:
            max_size *= 1024*1024*1024;
            break;
         }
      }
   }

   /* Default to 1GB for maximum cache size. */
   if (max_size == 0) {
      max_size = 1024*1024*1024;
   }

   cache->max_size = max_size;

   /* 4 threads were chosen below because just about all modern CPUs currently
    * available that run Mesa have *at least* 4 cores. For these CPUs allowing
    * more threads can result in the queue being processed faster, thus
    * avoiding excessive memory use due to a backlog of cache entrys building
    * up in the queue. Since we set the UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY
    * flag this should have little negative impact on low core systems.
    *
    * The queue will resize automatically when it's full, so adding new jobs
    * doesn't stall.
    */
   util_queue_init(&cache->cache_queue, "disk$", 32, 4,
                   UTIL_QUEUE_INIT_RESIZE_IF_FULL |
                   UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY |
                   UTIL_QUEUE_INIT_SET_FULL_THREAD_AFFINITY);

   cache->path_init_failed = false;

 path_fail:

   cache->driver_keys_blob_size = cv_size;

   /* Create driver id keys */
   size_t id_size = strlen(driver_id) + 1;
   size_t gpu_name_size = strlen(gpu_name) + 1;
   cache->driver_keys_blob_size += id_size;
   cache->driver_keys_blob_size += gpu_name_size;

   /* We sometimes store entire structs that contains a pointers in the cache,
    * use pointer size as a key to avoid hard to debug issues.
    */
   uint8_t ptr_size = sizeof(void *);
   size_t ptr_size_size = sizeof(ptr_size);
   cache->driver_keys_blob_size += ptr_size_size;

   size_t driver_flags_size = sizeof(driver_flags);
   cache->driver_keys_blob_size += driver_flags_size;

   cache->driver_keys_blob =
      ralloc_size(cache, cache->driver_keys_blob_size);
   if (!cache->driver_keys_blob)
      goto fail;

   uint8_t *drv_key_blob = cache->driver_keys_blob;
   DRV_KEY_CPY(drv_key_blob, &cache_version, cv_size)
   DRV_KEY_CPY(drv_key_blob, driver_id, id_size)
   DRV_KEY_CPY(drv_key_blob, gpu_name, gpu_name_size)
   DRV_KEY_CPY(drv_key_blob, &ptr_size, ptr_size_size)
   DRV_KEY_CPY(drv_key_blob, &driver_flags, driver_flags_size)

   /* Seed our rand function */
   s_rand_xorshift128plus(cache->seed_xorshift128plus, true);

   ralloc_free(local);

   return cache;

 fail:
   if (cache)
      ralloc_free(cache);
   ralloc_free(local);

   return NULL;
}

void
disk_cache_destroy(struct disk_cache *cache)
{
   if (cache && !cache->path_init_failed) {
      util_queue_finish(&cache->cache_queue);
      util_queue_destroy(&cache->cache_queue);
      disk_cache_destroy_mmap(cache);
   }

   ralloc_free(cache);
}

void
disk_cache_wait_for_idle(struct disk_cache *cache)
{
   util_queue_finish(&cache->cache_queue);
}

void
disk_cache_remove(struct disk_cache *cache, const cache_key key)
{
   char *filename = disk_cache_get_cache_filename(cache, key);
   if (filename == NULL) {
      return;
   }

   disk_cache_evict_item(cache, filename);
}

static ssize_t
read_all(int fd, void *buf, size_t count)
{
   char *in = buf;
   ssize_t read_ret;
   size_t done;

   for (done = 0; done < count; done += read_ret) {
      read_ret = read(fd, in + done, count - done);
      if (read_ret == -1 || read_ret == 0)
         return -1;
   }
   return done;
}

static struct disk_cache_put_job *
create_put_job(struct disk_cache *cache, const cache_key key,
               const void *data, size_t size,
               struct cache_item_metadata *cache_item_metadata)
{
   struct disk_cache_put_job *dc_job = (struct disk_cache_put_job *)
      malloc(sizeof(struct disk_cache_put_job) + size);

   if (dc_job) {
      dc_job->cache = cache;
      memcpy(dc_job->key, key, sizeof(cache_key));
      dc_job->data = dc_job + 1;
      memcpy(dc_job->data, data, size);
      dc_job->size = size;

      /* Copy the cache item metadata */
      if (cache_item_metadata) {
         dc_job->cache_item_metadata.type = cache_item_metadata->type;
         if (cache_item_metadata->type == CACHE_ITEM_TYPE_GLSL) {
            dc_job->cache_item_metadata.num_keys =
               cache_item_metadata->num_keys;
            dc_job->cache_item_metadata.keys = (cache_key *)
               malloc(cache_item_metadata->num_keys * sizeof(cache_key));

            if (!dc_job->cache_item_metadata.keys)
               goto fail;

            memcpy(dc_job->cache_item_metadata.keys,
                   cache_item_metadata->keys,
                   sizeof(cache_key) * cache_item_metadata->num_keys);
         }
      } else {
         dc_job->cache_item_metadata.type = CACHE_ITEM_TYPE_UNKNOWN;
         dc_job->cache_item_metadata.keys = NULL;
      }
   }

   return dc_job;

fail:
   free(dc_job);

   return NULL;
}

static void
destroy_put_job(void *job, int thread_index)
{
   if (job) {
      struct disk_cache_put_job *dc_job = (struct disk_cache_put_job *) job;
      free(dc_job->cache_item_metadata.keys);

      free(job);
   }
}

static void
cache_put(void *job, int thread_index)
{
   assert(job);

   unsigned i = 0;
   char *filename = NULL;
   struct disk_cache_put_job *dc_job = (struct disk_cache_put_job *) job;

   filename = disk_cache_get_cache_filename(dc_job->cache, dc_job->key);
   if (filename == NULL)
      goto done;

   /* If the cache is too large, evict something else first. */
   while (*dc_job->cache->size + dc_job->size > dc_job->cache->max_size &&
          i < 8) {
      disk_cache_evict_lru_item(dc_job->cache);
      i++;
   }

   /* Create CRC of the data. We will read this when restoring the cache and
    * use it to check for corruption.
    */
   struct cache_entry_file_data cf_data;
   cf_data.crc32 = util_hash_crc32(dc_job->data, dc_job->size);
   cf_data.uncompressed_size = dc_job->size;

   disk_cache_write_item_to_disk(dc_job, &cf_data, filename);

done:
   free(filename);
}

void
disk_cache_put(struct disk_cache *cache, const cache_key key,
               const void *data, size_t size,
               struct cache_item_metadata *cache_item_metadata)
{
   if (cache->blob_put_cb) {
      cache->blob_put_cb(key, CACHE_KEY_SIZE, data, size);
      return;
   }

   if (cache->path_init_failed)
      return;

   struct disk_cache_put_job *dc_job =
      create_put_job(cache, key, data, size, cache_item_metadata);

   if (dc_job) {
      util_queue_fence_init(&dc_job->fence);
      util_queue_add_job(&cache->cache_queue, dc_job, &dc_job->fence,
                         cache_put, destroy_put_job, dc_job->size);
   }
}

/**
 * Decompresses cache entry, returns true if successful.
 */
static bool
inflate_cache_data(uint8_t *in_data, size_t in_data_size,
                   uint8_t *out_data, size_t out_data_size)
{
#ifdef HAVE_ZSTD
   size_t ret = ZSTD_decompress(out_data, out_data_size, in_data, in_data_size);
   return !ZSTD_isError(ret);
#else
   z_stream strm;

   /* allocate inflate state */
   strm.zalloc = Z_NULL;
   strm.zfree = Z_NULL;
   strm.opaque = Z_NULL;
   strm.next_in = in_data;
   strm.avail_in = in_data_size;
   strm.next_out = out_data;
   strm.avail_out = out_data_size;

   int ret = inflateInit(&strm);
   if (ret != Z_OK)
      return false;

   ret = inflate(&strm, Z_NO_FLUSH);
   assert(ret != Z_STREAM_ERROR);  /* state not clobbered */

   /* Unless there was an error we should have decompressed everything in one
    * go as we know the uncompressed file size.
    */
   if (ret != Z_STREAM_END) {
      (void)inflateEnd(&strm);
      return false;
   }
   assert(strm.avail_out == 0);

   /* clean up and return */
   (void)inflateEnd(&strm);
   return true;
#endif
}

void *
disk_cache_get(struct disk_cache *cache, const cache_key key, size_t *size)
{
   int fd = -1, ret;
   struct stat sb;
   char *filename = NULL;
   uint8_t *data = NULL;
   uint8_t *uncompressed_data = NULL;
   uint8_t *file_header = NULL;

   if (size)
      *size = 0;

   if (cache->blob_get_cb) {
      /* This is what Android EGL defines as the maxValueSize in egl_cache_t
       * class implementation.
       */
      const signed long max_blob_size = 64 * 1024;
      void *blob = malloc(max_blob_size);
      if (!blob)
         return NULL;

      signed long bytes =
         cache->blob_get_cb(key, CACHE_KEY_SIZE, blob, max_blob_size);

      if (!bytes) {
         free(blob);
         return NULL;
      }

      if (size)
         *size = bytes;
      return blob;
   }

   filename = disk_cache_get_cache_filename(cache, key);
   if (filename == NULL)
      goto fail;

   fd = open(filename, O_RDONLY | O_CLOEXEC);
   if (fd == -1)
      goto fail;

   if (fstat(fd, &sb) == -1)
      goto fail;

   data = malloc(sb.st_size);
   if (data == NULL)
      goto fail;

   size_t ck_size = cache->driver_keys_blob_size;
   file_header = malloc(ck_size);
   if (!file_header)
      goto fail;

   if (sb.st_size < ck_size)
      goto fail;

   ret = read_all(fd, file_header, ck_size);
   if (ret == -1)
      goto fail;

   /* Check for extremely unlikely hash collisions */
   if (memcmp(cache->driver_keys_blob, file_header, ck_size) != 0) {
      assert(!"Mesa cache keys mismatch!");
      goto fail;
   }

   size_t cache_item_md_size = sizeof(uint32_t);
   uint32_t md_type;
   ret = read_all(fd, &md_type, cache_item_md_size);
   if (ret == -1)
      goto fail;

   if (md_type == CACHE_ITEM_TYPE_GLSL) {
      uint32_t num_keys;
      cache_item_md_size += sizeof(uint32_t);
      ret = read_all(fd, &num_keys, sizeof(uint32_t));
      if (ret == -1)
         goto fail;

      /* The cache item metadata is currently just used for distributing
       * precompiled shaders, they are not used by Mesa so just skip them for
       * now.
       * TODO: pass the metadata back to the caller and do some basic
       * validation.
       */
      cache_item_md_size += num_keys * sizeof(cache_key);
      ret = lseek(fd, num_keys * sizeof(cache_key), SEEK_CUR);
      if (ret == -1)
         goto fail;
   }

   /* Load the CRC that was created when the file was written. */
   struct cache_entry_file_data cf_data;
   size_t cf_data_size = sizeof(cf_data);
   ret = read_all(fd, &cf_data, cf_data_size);
   if (ret == -1)
      goto fail;

   /* Load the actual cache data. */
   size_t cache_data_size =
      sb.st_size - cf_data_size - ck_size - cache_item_md_size;
   ret = read_all(fd, data, cache_data_size);
   if (ret == -1)
      goto fail;

   /* Uncompress the cache data */
   uncompressed_data = malloc(cf_data.uncompressed_size);
   if (!inflate_cache_data(data, cache_data_size, uncompressed_data,
                           cf_data.uncompressed_size))
      goto fail;

   /* Check the data for corruption */
   if (cf_data.crc32 != util_hash_crc32(uncompressed_data,
                                        cf_data.uncompressed_size))
      goto fail;

   free(data);
   free(filename);
   free(file_header);
   close(fd);

   if (size)
      *size = cf_data.uncompressed_size;

   return uncompressed_data;

 fail:
   if (data)
      free(data);
   if (uncompressed_data)
      free(uncompressed_data);
   if (filename)
      free(filename);
   if (file_header)
      free(file_header);
   if (fd != -1)
      close(fd);

   return NULL;
}

void
disk_cache_put_key(struct disk_cache *cache, const cache_key key)
{
   const uint32_t *key_chunk = (const uint32_t *) key;
   int i = CPU_TO_LE32(*key_chunk) & CACHE_INDEX_KEY_MASK;
   unsigned char *entry;

   if (cache->blob_put_cb) {
      cache->blob_put_cb(key, CACHE_KEY_SIZE, key_chunk, sizeof(uint32_t));
      return;
   }

   if (cache->path_init_failed)
      return;

   entry = &cache->stored_keys[i * CACHE_KEY_SIZE];

   memcpy(entry, key, CACHE_KEY_SIZE);
}

/* This function lets us test whether a given key was previously
 * stored in the cache with disk_cache_put_key(). The implement is
 * efficient by not using syscalls or hitting the disk. It's not
 * race-free, but the races are benign. If we race with someone else
 * calling disk_cache_put_key, then that's just an extra cache miss and an
 * extra recompile.
 */
bool
disk_cache_has_key(struct disk_cache *cache, const cache_key key)
{
   const uint32_t *key_chunk = (const uint32_t *) key;
   int i = CPU_TO_LE32(*key_chunk) & CACHE_INDEX_KEY_MASK;
   unsigned char *entry;

   if (cache->blob_get_cb) {
      uint32_t blob;
      return cache->blob_get_cb(key, CACHE_KEY_SIZE, &blob, sizeof(uint32_t));
   }

   if (cache->path_init_failed)
      return false;

   entry = &cache->stored_keys[i * CACHE_KEY_SIZE];

   return memcmp(entry, key, CACHE_KEY_SIZE) == 0;
}

void
disk_cache_compute_key(struct disk_cache *cache, const void *data, size_t size,
                       cache_key key)
{
   struct mesa_sha1 ctx;

   _mesa_sha1_init(&ctx);
   _mesa_sha1_update(&ctx, cache->driver_keys_blob,
                     cache->driver_keys_blob_size);
   _mesa_sha1_update(&ctx, data, size);
   _mesa_sha1_final(&ctx, key);
}

void
disk_cache_set_callbacks(struct disk_cache *cache, disk_cache_put_cb put,
                         disk_cache_get_cb get)
{
   cache->blob_put_cb = put;
   cache->blob_get_cb = get;
}

#endif /* ENABLE_SHADER_CACHE */

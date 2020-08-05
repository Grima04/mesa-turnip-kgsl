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

#if DETECT_OS_WINDOWS
/* TODO: implement disk cache support on windows */

#else

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/debug.h"
#include "util/disk_cache.h"
#include "util/disk_cache_os.h"
#include "util/ralloc.h"
#include "util/rand_xor.h"

/* Create a directory named 'path' if it does not already exist.
 *
 * Returns: 0 if path already exists as a directory or if created.
 *         -1 in all other cases.
 */
static int
mkdir_if_needed(const char *path)
{
   struct stat sb;

   /* If the path exists already, then our work is done if it's a
    * directory, but it's an error if it is not.
    */
   if (stat(path, &sb) == 0) {
      if (S_ISDIR(sb.st_mode)) {
         return 0;
      } else {
         fprintf(stderr, "Cannot use %s for shader cache (not a directory)"
                         "---disabling.\n", path);
         return -1;
      }
   }

   int ret = mkdir(path, 0755);
   if (ret == 0 || (ret == -1 && errno == EEXIST))
     return 0;

   fprintf(stderr, "Failed to create %s for shader cache (%s)---disabling.\n",
           path, strerror(errno));

   return -1;
}

/* Concatenate an existing path and a new name to form a new path.  If the new
 * path does not exist as a directory, create it then return the resulting
 * name of the new path (ralloc'ed off of 'ctx').
 *
 * Returns NULL on any error, such as:
 *
 *      <path> does not exist or is not a directory
 *      <path>/<name> exists but is not a directory
 *      <path>/<name> cannot be created as a directory
 */
static char *
concatenate_and_mkdir(void *ctx, const char *path, const char *name)
{
   char *new_path;
   struct stat sb;

   if (stat(path, &sb) != 0 || ! S_ISDIR(sb.st_mode))
      return NULL;

   new_path = ralloc_asprintf(ctx, "%s/%s", path, name);

   if (mkdir_if_needed(new_path) == 0)
      return new_path;
   else
      return NULL;
}

/* Given a directory path and predicate function, find the entry with
 * the oldest access time in that directory for which the predicate
 * returns true.
 *
 * Returns: A malloc'ed string for the path to the chosen file, (or
 * NULL on any error). The caller should free the string when
 * finished.
 */
static char *
choose_lru_file_matching(const char *dir_path,
                         bool (*predicate)(const char *dir_path,
                                           const struct stat *,
                                           const char *, const size_t))
{
   DIR *dir;
   struct dirent *entry;
   char *filename;
   char *lru_name = NULL;
   time_t lru_atime = 0;

   dir = opendir(dir_path);
   if (dir == NULL)
      return NULL;

   while (1) {
      entry = readdir(dir);
      if (entry == NULL)
         break;

      struct stat sb;
      if (fstatat(dirfd(dir), entry->d_name, &sb, 0) == 0) {
         if (!lru_atime || (sb.st_atime < lru_atime)) {
            size_t len = strlen(entry->d_name);

            if (!predicate(dir_path, &sb, entry->d_name, len))
               continue;

            char *tmp = realloc(lru_name, len + 1);
            if (tmp) {
               lru_name = tmp;
               memcpy(lru_name, entry->d_name, len + 1);
               lru_atime = sb.st_atime;
            }
         }
      }
   }

   if (lru_name == NULL) {
      closedir(dir);
      return NULL;
   }

   if (asprintf(&filename, "%s/%s", dir_path, lru_name) < 0)
      filename = NULL;

   free(lru_name);
   closedir(dir);

   return filename;
}

/* Is entry a regular file, and not having a name with a trailing
 * ".tmp"
 */
static bool
is_regular_non_tmp_file(const char *path, const struct stat *sb,
                        const char *d_name, const size_t len)
{
   if (!S_ISREG(sb->st_mode))
      return false;

   if (len >= 4 && strcmp(&d_name[len-4], ".tmp") == 0)
      return false;

   return true;
}

/* Returns the size of the deleted file, (or 0 on any error). */
static size_t
unlink_lru_file_from_directory(const char *path)
{
   struct stat sb;
   char *filename;

   filename = choose_lru_file_matching(path, is_regular_non_tmp_file);
   if (filename == NULL)
      return 0;

   if (stat(filename, &sb) == -1) {
      free (filename);
      return 0;
   }

   unlink(filename);
   free (filename);

   return sb.st_blocks * 512;
}

/* Is entry a directory with a two-character name, (and not the
 * special name of ".."). We also return false if the dir is empty.
 */
static bool
is_two_character_sub_directory(const char *path, const struct stat *sb,
                               const char *d_name, const size_t len)
{
   if (!S_ISDIR(sb->st_mode))
      return false;

   if (len != 2)
      return false;

   if (strcmp(d_name, "..") == 0)
      return false;

   char *subdir;
   if (asprintf(&subdir, "%s/%s", path, d_name) == -1)
      return false;
   DIR *dir = opendir(subdir);
   free(subdir);

   if (dir == NULL)
     return false;

   unsigned subdir_entries = 0;
   struct dirent *d;
   while ((d = readdir(dir)) != NULL) {
      if(++subdir_entries > 2)
         break;
   }
   closedir(dir);

   /* If dir only contains '.' and '..' it must be empty */
   if (subdir_entries <= 2)
      return false;

   return true;
}

/* Evict least recently used cache item */
void
disk_cache_evict_lru_item(struct disk_cache *cache)
{
   char *dir_path;

   /* With a reasonably-sized, full cache, (and with keys generated
    * from a cryptographic hash), we can choose two random hex digits
    * and reasonably expect the directory to exist with a file in it.
    * Provides pseudo-LRU eviction to reduce checking all cache files.
    */
   uint64_t rand64 = rand_xorshift128plus(cache->seed_xorshift128plus);
   if (asprintf(&dir_path, "%s/%02" PRIx64 , cache->path, rand64 & 0xff) < 0)
      return;

   size_t size = unlink_lru_file_from_directory(dir_path);

   free(dir_path);

   if (size) {
      p_atomic_add(cache->size, - (uint64_t)size);
      return;
   }

   /* In the case where the random choice of directory didn't find
    * something, we choose the least recently accessed from the
    * existing directories.
    *
    * Really, the only reason this code exists is to allow the unit
    * tests to work, (which use an artificially-small cache to be able
    * to force a single cached item to be evicted).
    */
   dir_path = choose_lru_file_matching(cache->path,
                                       is_two_character_sub_directory);
   if (dir_path == NULL)
      return;

   size = unlink_lru_file_from_directory(dir_path);

   free(dir_path);

   if (size)
      p_atomic_add(cache->size, - (uint64_t)size);
}

/* Determine path for cache based on the first defined name as follows:
 *
 *   $MESA_GLSL_CACHE_DIR
 *   $XDG_CACHE_HOME/mesa_shader_cache
 *   <pwd.pw_dir>/.cache/mesa_shader_cache
 */
char *
disk_cache_generate_cache_dir(void *mem_ctx)
{
   char *path = getenv("MESA_GLSL_CACHE_DIR");
   if (path) {
      if (mkdir_if_needed(path) == -1)
         return NULL;

      path = concatenate_and_mkdir(mem_ctx, path, CACHE_DIR_NAME);
      if (!path)
         return NULL;
   }

   if (path == NULL) {
      char *xdg_cache_home = getenv("XDG_CACHE_HOME");

      if (xdg_cache_home) {
         if (mkdir_if_needed(xdg_cache_home) == -1)
            return NULL;

         path = concatenate_and_mkdir(mem_ctx, xdg_cache_home, CACHE_DIR_NAME);
         if (!path)
            return NULL;
      }
   }

   if (!path) {
      char *buf;
      size_t buf_size;
      struct passwd pwd, *result;

      buf_size = sysconf(_SC_GETPW_R_SIZE_MAX);
      if (buf_size == -1)
         buf_size = 512;

      /* Loop until buf_size is large enough to query the directory */
      while (1) {
         buf = ralloc_size(mem_ctx, buf_size);

         getpwuid_r(getuid(), &pwd, buf, buf_size, &result);
         if (result)
            break;

         if (errno == ERANGE) {
            ralloc_free(buf);
            buf = NULL;
            buf_size *= 2;
         } else {
            return NULL;
         }
      }

      path = concatenate_and_mkdir(mem_ctx, pwd.pw_dir, ".cache");
      if (!path)
         return NULL;

      path = concatenate_and_mkdir(mem_ctx, path, CACHE_DIR_NAME);
      if (!path)
         return NULL;
   }

   return path;
}

bool
disk_cache_enabled()
{
   /* If running as a users other than the real user disable cache */
   if (geteuid() != getuid())
      return false;

   /* At user request, disable shader cache entirely. */
   if (env_var_as_boolean("MESA_GLSL_CACHE_DISABLE", false))
      return false;

   return true;
}

bool
disk_cache_mmap_cache_index(void *mem_ctx, struct disk_cache *cache,
                            char *path)
{
   int fd = -1;
   bool mapped = false;

   cache->path = ralloc_strdup(cache, path);
   if (cache->path == NULL)
      goto path_fail;

   path = ralloc_asprintf(mem_ctx, "%s/index", cache->path);
   if (path == NULL)
      goto path_fail;

   fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
   if (fd == -1)
      goto path_fail;

   struct stat sb;
   if (fstat(fd, &sb) == -1)
      goto path_fail;

   /* Force the index file to be the expected size. */
   size_t size = sizeof(*cache->size) + CACHE_INDEX_MAX_KEYS * CACHE_KEY_SIZE;
   if (sb.st_size != size) {
      if (ftruncate(fd, size) == -1)
         goto path_fail;
   }

   /* We map this shared so that other processes see updates that we
    * make.
    *
    * Note: We do use atomic addition to ensure that multiple
    * processes don't scramble the cache size recorded in the
    * index. But we don't use any locking to prevent multiple
    * processes from updating the same entry simultaneously. The idea
    * is that if either result lands entirely in the index, then
    * that's equivalent to a well-ordered write followed by an
    * eviction and a write. On the other hand, if the simultaneous
    * writes result in a corrupt entry, that's not really any
    * different than both entries being evicted, (since within the
    * guarantees of the cryptographic hash, a corrupt entry is
    * unlikely to ever match a real cache key).
    */
   cache->index_mmap = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
   if (cache->index_mmap == MAP_FAILED)
      goto path_fail;
   cache->index_mmap_size = size;

   cache->size = (uint64_t *) cache->index_mmap;
   cache->stored_keys = cache->index_mmap + sizeof(uint64_t);
   mapped = true;

path_fail:
   if (fd != -1)
      close(fd);

   return mapped;
}

void
disk_cache_destroy_mmap(struct disk_cache *cache)
{
   munmap(cache->index_mmap, cache->index_mmap_size);
}
#endif

#endif /* ENABLE_SHADER_CACHE */

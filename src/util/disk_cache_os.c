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

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/debug.h"
#include "util/disk_cache.h"
#include "util/disk_cache_os.h"
#include "util/ralloc.h"

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
#endif

#endif /* ENABLE_SHADER_CACHE */

/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iris_disk_cache.c
 *
 * Functions for interacting with the on-disk shader cache.
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "compiler/blob.h"
#include "compiler/nir/nir.h"
#include "util/build_id.h"
#include "util/disk_cache.h"
#include "util/mesa-sha1.h"

#include "iris_context.h"

/**
 * Initialize the on-disk shader cache.
 */
void
iris_disk_cache_init(struct iris_screen *screen)
{
#ifdef ENABLE_SHADER_CACHE
   if (INTEL_DEBUG & DEBUG_DISK_CACHE_DISABLE_MASK)
      return;

   /* array length = print length + nul char + 1 extra to verify it's unused */
   char renderer[11];
   UNUSED int len =
      snprintf(renderer, sizeof(renderer), "iris_%04x", screen->pci_id);
   assert(len == sizeof(renderer) - 2);

   const struct build_id_note *note =
      build_id_find_nhdr_for_addr(iris_disk_cache_init);
   assert(note && build_id_length(note) == 20); /* sha1 */

   const uint8_t *id_sha1 = build_id_data(note);
   assert(id_sha1);

   char timestamp[41];
   _mesa_sha1_format(timestamp, id_sha1);

   const uint64_t driver_flags =
      brw_get_compiler_config_value(screen->compiler);
   screen->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);
#endif
}

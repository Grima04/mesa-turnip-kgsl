/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include <i915_drm.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_atomic.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "intel/compiler/brw_compiler.h"
#include "intel/compiler/brw_eu.h"
#include "intel/compiler/brw_nir.h"
#include "iris_context.h"

struct keybox {
   uint8_t size;
   enum iris_program_cache_id cache_id;
   uint8_t data[0];
};

// XXX: put packets here, expose this somehow and simplify the interfaces
struct iris_program_cache_item {
   uint32_t assembly_offset;
   uint32_t assembly_size;
   void *prog_data;
};

static struct keybox *
make_keybox(struct iris_program_cache *cache,
            enum iris_program_cache_id cache_id,
            const void *key, unsigned key_size)
{
   struct keybox *keybox =
      ralloc_size(cache->table, sizeof(struct keybox) + key_size);

   keybox->cache_id = cache_id;
   keybox->size = key_size;
   memcpy(keybox->data, key, key_size);

   return keybox;
}

static uint32_t
keybox_hash(const void *void_key)
{
   const struct keybox *key = void_key;
   return _mesa_hash_data(&key->cache_id, key->size + sizeof(key->cache_id));
}

static bool
keybox_equals(const void *void_a, const void *void_b)
{
   const struct keybox *a = void_a, *b = void_b;
   if (a->size != b->size)
      return false;

   return memcmp(a->data, b->data, a->size) == 0;
}

static unsigned
get_program_string_id(enum iris_program_cache_id cache_id, const void *key)
{
   switch (cache_id) {
   case IRIS_CACHE_VS:
      return ((struct brw_vs_prog_key *) key)->program_string_id;
   case IRIS_CACHE_TCS:
      return ((struct brw_tcs_prog_key *) key)->program_string_id;
   case IRIS_CACHE_TES:
      return ((struct brw_tes_prog_key *) key)->program_string_id;
   case IRIS_CACHE_GS:
      return ((struct brw_gs_prog_key *) key)->program_string_id;
   case IRIS_CACHE_CS:
      return ((struct brw_cs_prog_key *) key)->program_string_id;
   case IRIS_CACHE_FS:
      return ((struct brw_wm_prog_key *) key)->program_string_id;
   default:
      unreachable("no program string id for this kind of program");
   }
}

/**
 * Returns the buffer object matching cache_id and key, or NULL.
 */
// XXX: rename to iris_bind_cached_shader?
bool
iris_search_cache(struct iris_context *ice,
                  enum iris_program_cache_id cache_id,
                  const void *key,
                  unsigned key_size,
                  uint64_t dirty_flag,
                  uint32_t *inout_assembly_offset,
                  void *inout_prog_data)
{
   struct iris_program_cache *cache = &ice->shaders.cache;

   struct keybox *keybox = make_keybox(cache, cache_id, key, key_size);

   struct hash_entry *entry = _mesa_hash_table_search(cache->table, keybox);

   if (entry == NULL)
      return false;

   struct iris_program_cache_item *item = entry->data;

   if (item->assembly_offset != *inout_assembly_offset ||
       item->prog_data != *((void **) inout_prog_data)) {
      *inout_assembly_offset = item->assembly_offset;
      *((void **) inout_prog_data) = item->prog_data;
      ice->state.dirty |= dirty_flag;
   }

   return true;
}

static void
recreate_cache_bo(struct iris_context *ice, uint32_t size)
{
   struct iris_program_cache *cache = &ice->shaders.cache;
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   struct iris_bo *old_bo = cache->bo;
   void *old_map = cache->map;

   cache->bo = iris_bo_alloc(screen->bufmgr, "program cache", size, 64);
   cache->bo->kflags = EXEC_OBJECT_CAPTURE;
   cache->map = iris_bo_map(&ice->dbg, cache->bo,
                            MAP_READ | MAP_WRITE | MAP_ASYNC | MAP_PERSISTENT);

   /* Copy any existing data that needs to be saved. */
   if (old_bo) {
      perf_debug(&ice->dbg,
                 "Copying to larger program cache: %u kB -> %u kB\n",
                 (unsigned) old_bo->size / 1024,
                 (unsigned) cache->bo->size / 1024);

      memcpy(cache->map, old_map, cache->next_offset);

      iris_bo_unreference(old_bo);
      iris_bo_unmap(old_bo);
   }

   ice->state.dirty |= IRIS_DIRTY_STATE_BASE_ADDRESS;
}

const void *
iris_find_previous_compile(struct iris_program_cache *cache,
                           enum iris_program_cache_id cache_id,
                           unsigned program_string_id)
{
   hash_table_foreach(cache->table, entry) {
      const struct keybox *keybox = entry->key;
      if (keybox->cache_id == cache_id &&
          get_program_string_id(cache_id, keybox->data) == program_string_id) {
         return keybox->data;
      }
   }

   return NULL;
}

/**
 * Look for an existing entry in the cache that has identical assembly code.
 *
 * This is useful for programs generating shaders at runtime, where multiple
 * distinct shaders (from an API perspective) may compile to the same assembly
 * in our backend.
 */
static const struct iris_program_cache_item *
find_existing_assembly(const struct iris_program_cache *cache,
                       const void *assembly,
                       unsigned assembly_size)
{
   hash_table_foreach(cache->table, entry) {
      const struct iris_program_cache_item *item = entry->data;
      if (item->assembly_size == assembly_size &&
          memcmp(cache->map + item->assembly_offset,
                 assembly, assembly_size) == 0)
         return item;
   }
   return NULL;
}

static uint32_t
upload_new_assembly(struct iris_context *ice,
                    const void *assembly,
                    unsigned size)
{
   struct iris_program_cache *cache = &ice->shaders.cache;

   /* Allocate space in the cache BO for our new program. */
   if (cache->next_offset + size > cache->bo->size) {
      uint32_t new_size = cache->bo->size * 2;

      while (cache->next_offset + size > new_size)
         new_size *= 2;

      recreate_cache_bo(ice, new_size);
   }

   uint32_t offset = cache->next_offset;

   /* Programs are always 64-byte aligned, so set up the next one now */
   cache->next_offset = ALIGN(offset + size, 64);

   /* Copy data to the buffer */
   memcpy(cache->map + offset, assembly, size);

   return offset;
}

void
iris_upload_cache(struct iris_context *ice,
                  enum iris_program_cache_id cache_id,
                  const void *key,
                  unsigned key_size,
                  const void *assembly,
                  unsigned assembly_size,
                  const void *prog_data,
                  unsigned prog_data_size,
                  uint32_t *out_assembly_offset,
                  void *out_prog_data)
{
   struct iris_program_cache *cache = &ice->shaders.cache;
   struct iris_program_cache_item *item =
      ralloc(cache->table, struct iris_program_cache_item);
   const struct iris_program_cache_item *matching_data =
      find_existing_assembly(cache, assembly, assembly_size);

   /* If we can find a matching prog in the cache already, then reuse the
    * existing stuff without creating new copy into the underlying buffer
    * object.  This is notably useful for programs generating shaders at
    * runtime, where multiple shaders may compile to the same thing in our
    * backend.
    */
   if (matching_data) {
      item->assembly_offset = matching_data->assembly_offset;
   } else {
      item->assembly_offset =
         upload_new_assembly(ice, assembly, assembly_size);
   }

   item->assembly_size = assembly_size;
   item->prog_data = ralloc_size(item, prog_data_size);
   memcpy(item->prog_data, prog_data, prog_data_size);

   if (cache_id != IRIS_CACHE_BLORP) {
      struct brw_stage_prog_data *stage_prog_data = prog_data;
      ralloc_steal(item->prog_data, stage_prog_data->param);
      ralloc_steal(item->prog_data, stage_prog_data->pull_param);
   }

   struct keybox *keybox = make_keybox(cache, cache_id, key, key_size);
   _mesa_hash_table_insert(cache->table, keybox, item);

   *out_assembly_offset = item->assembly_offset;
   *(void **)out_prog_data = item->prog_data;
}

void
iris_init_program_cache(struct iris_context *ice)
{
   struct iris_program_cache *cache = &ice->shaders.cache;

   cache->table = _mesa_hash_table_create(ice, keybox_hash, keybox_equals);

   recreate_cache_bo(ice, 16384);
}

void
iris_destroy_program_cache(struct iris_context *ice)
{
   struct iris_program_cache *cache = &ice->shaders.cache;

   /* This can be NULL if context creation failed early on. */
   if (cache->bo) {
      iris_bo_unmap(cache->bo);
      iris_bo_unreference(cache->bo);
      cache->bo = NULL;
      cache->map = NULL;
   }

   cache->next_offset = 0;

   /* Also, NULL out any stale program pointers. */
   for (int i = 0; i < MESA_SHADER_STAGES; i++) {
      ice->shaders.prog_data[i] = NULL;
   }
}

static const char *
cache_name(enum iris_program_cache_id cache_id)
{
   if (cache_id == IRIS_CACHE_BLORP)
      return "BLORP";

   return _mesa_shader_stage_to_string(cache_id);
}

void
iris_print_program_cache(struct iris_context *ice)
{
   struct iris_program_cache *cache = &ice->shaders.cache;
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct gen_device_info *devinfo = &screen->devinfo;

   hash_table_foreach(cache->table, entry) {
      const struct keybox *keybox = entry->key;
      struct iris_program_cache_item *item = entry->data;
      fprintf(stderr, "%s:\n", cache_name(keybox->cache_id));
      brw_disassemble(devinfo, cache->map,
                      item->assembly_offset, item->assembly_size, stderr);
   }
}

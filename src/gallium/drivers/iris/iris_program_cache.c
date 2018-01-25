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

static struct keybox *
make_keybox(struct iris_program_cache *cache,
            enum iris_program_cache_id cache_id,
            const void *key)
{
   static const unsigned key_sizes[] = {
      [IRIS_CACHE_VS]         = sizeof(struct brw_vs_prog_key),
      [IRIS_CACHE_TCS]        = sizeof(struct brw_tcs_prog_key),
      [IRIS_CACHE_TES]        = sizeof(struct brw_tes_prog_key),
      [IRIS_CACHE_GS]         = sizeof(struct brw_gs_prog_key),
      [IRIS_CACHE_FS]         = sizeof(struct brw_wm_prog_key),
      [IRIS_CACHE_CS]         = sizeof(struct brw_cs_prog_key),
      //[IRIS_CACHE_BLORP_BLIT] = sizeof(struct brw_blorp_blit_prog_key),
   };

   struct keybox *keybox =
      ralloc_size(cache->table, sizeof(struct keybox) + key_sizes[cache_id]);

   keybox->cache_id = cache_id;
   keybox->size = key_sizes[cache_id];
   memcpy(keybox->data, key, key_sizes[cache_id]);

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

static uint64_t
dirty_flag_for_cache(enum iris_program_cache_id cache_id)
{
   assert(cache_id <= MESA_SHADER_STAGES);
   return IRIS_DIRTY_VS << cache_id;
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
 * Looks for a program in the cache and binds it.
 *
 * If no program was found, returns false and leaves the binding alone.
 */
bool
iris_bind_cached_shader(struct iris_context *ice,
                        enum iris_program_cache_id cache_id,
                        const void *key)
{
   struct iris_program_cache *cache = &ice->shaders.cache;

   struct keybox *keybox = make_keybox(cache, cache_id, key);

   struct hash_entry *entry = _mesa_hash_table_search(cache->table, keybox);

   if (!entry)
      return false;

   struct iris_compiled_shader *shader = entry->data;

   if (cache_id <= MESA_SHADER_STAGES &&
       memcmp(shader, ice->shaders.prog[cache_id], sizeof(*shader)) != 0) {
      ice->shaders.prog[cache_id] = shader;
      ice->state.dirty |= dirty_flag_for_cache(cache_id);
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
   cache->bo->kflags = EXEC_OBJECT_CAPTURE | EXEC_OBJECT_PINNED;
   cache->map = iris_bo_map(&ice->dbg, cache->bo,
                            MAP_READ | MAP_WRITE | MAP_ASYNC | MAP_PERSISTENT);

   /* Copy any existing data that needs to be saved. */
   if (old_bo) {
      perf_debug(&ice->dbg,
                 "Copying to larger program cache: %u kB -> %u kB\n",
                 (unsigned) old_bo->size / 1024,
                 (unsigned) cache->bo->size / 1024);

      /* Put the new BO just past the old one */
      cache->bo->gtt_offset = ALIGN(old_bo->gtt_offset + old_bo->size, 4096);

      memcpy(cache->map, old_map, cache->next_offset);

      iris_bo_unreference(old_bo);
      iris_bo_unmap(old_bo);
   } else {
      /* Put the initial cache BO...somewhere. */
      cache->bo->gtt_offset = 4096 * 10;
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
 * in our backend.  This saves space in the program cache buffer.
 */
static const struct iris_compiled_shader *
find_existing_assembly(const struct iris_program_cache *cache,
                       const void *assembly,
                       unsigned assembly_size)
{
   hash_table_foreach(cache->table, entry) {
      const struct iris_compiled_shader *existing = entry->data;
      if (existing->prog_data->program_size == assembly_size &&
          memcmp(cache->map + existing->prog_offset,
                 assembly, assembly_size) == 0)
         return existing;
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

/**
 * Upload a new shader to the program cache, and bind it for use.
 *
 * \param prog_data must be ralloc'd and will be stolen.
 */
void
iris_upload_and_bind_shader(struct iris_context *ice,
                            enum iris_program_cache_id cache_id,
                            const void *key,
                            const void *assembly,
                            struct brw_stage_prog_data *prog_data)
{
   struct iris_screen *screen = (void *) ice->ctx.screen;
   struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_program_cache *cache = &ice->shaders.cache;
   struct iris_compiled_shader *shader =
      ralloc_size(cache->table, sizeof(struct iris_compiled_shader) +
                  ice->state.derived_program_state_size(cache_id));
   const struct iris_compiled_shader *existing =
      find_existing_assembly(cache, assembly, prog_data->program_size);

   /* If we can find a matching prog in the cache already, then reuse the
    * existing stuff without creating new copy into the underlying buffer
    * object.  This is notably useful for programs generating shaders at
    * runtime, where multiple shaders may compile to the same thing in our
    * backend.
    */
   if (existing) {
      shader->prog_offset = existing->prog_offset;
   } else {
      shader->prog_offset =
         upload_new_assembly(ice, assembly, prog_data->program_size);
   }

   shader->prog_data = prog_data;

   ralloc_steal(shader, shader->prog_data);
   ralloc_steal(shader->prog_data, prog_data->param);
   ralloc_steal(shader->prog_data, prog_data->pull_param);

   /* Store the 3DSTATE shader packets and other derived state. */
   ice->state.set_derived_program_state(devinfo, cache_id, shader);

   struct keybox *keybox = make_keybox(cache, cache_id, key);
   _mesa_hash_table_insert(cache->table, keybox, shader);

   if (cache_id <= MESA_SHADER_STAGES) {
      ice->shaders.prog[cache_id] = shader;
      ice->state.dirty |= dirty_flag_for_cache(cache_id);
   }
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

   for (int i = 0; i < MESA_SHADER_STAGES; i++) {
      ice->shaders.prog[i] = NULL;
   }

   ralloc_free(cache->table);
}

static const char *
cache_name(enum iris_program_cache_id cache_id)
{
   if (cache_id == IRIS_CACHE_BLORP_BLIT)
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
      struct iris_compiled_shader *shader = entry->data;
      fprintf(stderr, "%s:\n", cache_name(keybox->cache_id));
      brw_disassemble(devinfo, cache->map, shader->prog_offset,
                      shader->prog_data->program_size, stderr);
   }
}

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
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_atomic.h"
#include "util/u_upload_mgr.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "intel/compiler/brw_compiler.h"
#include "intel/compiler/brw_eu.h"
#include "intel/compiler/brw_nir.h"
#include "iris_context.h"
#include "iris_resource.h"

struct keybox {
   uint8_t size;
   enum iris_program_cache_id cache_id;
   uint8_t data[0];
};

static uint32_t
key_size_for_cache(enum iris_program_cache_id cache_id)
{
   static const unsigned key_sizes[] = {
      [IRIS_CACHE_VS]         = sizeof(struct brw_vs_prog_key),
      [IRIS_CACHE_TCS]        = sizeof(struct brw_tcs_prog_key),
      [IRIS_CACHE_TES]        = sizeof(struct brw_tes_prog_key),
      [IRIS_CACHE_GS]         = sizeof(struct brw_gs_prog_key),
      [IRIS_CACHE_FS]         = sizeof(struct brw_wm_prog_key),
      [IRIS_CACHE_CS]         = sizeof(struct brw_cs_prog_key),
   };

   /* BLORP keys aren't all the same size. */
   assert(cache_id != IRIS_CACHE_BLORP);

   return key_sizes[cache_id];
}

static struct keybox *
make_keybox(void *mem_ctx,
            enum iris_program_cache_id cache_id,
            const void *key,
            uint32_t key_size)
{
   struct keybox *keybox =
      ralloc_size(mem_ctx, sizeof(struct keybox) + key_size);

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

static uint64_t
dirty_flag_for_cache(enum iris_program_cache_id cache_id)
{
   assert(cache_id <= MESA_SHADER_STAGES);

   // XXX: ugly
   if (cache_id == IRIS_CACHE_FS)
      return IRIS_DIRTY_WM | IRIS_DIRTY_FS;

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

static struct iris_compiled_shader *
iris_find_cached_shader(struct iris_context *ice,
                        enum iris_program_cache_id cache_id,
                        const void *key,
                        uint32_t key_size)
{
   struct keybox *keybox =
      make_keybox(ice->shaders.cache, cache_id, key, key_size);
   struct hash_entry *entry =
      _mesa_hash_table_search(ice->shaders.cache, keybox);

   return entry ? entry->data : NULL;
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
   unsigned key_size = key_size_for_cache(cache_id);
   struct iris_compiled_shader *shader =
      iris_find_cached_shader(ice, cache_id, key, key_size);

   if (!shader)
      return false;

   if (memcmp(shader, ice->shaders.prog[cache_id], sizeof(*shader)) != 0) {
      ice->shaders.prog[cache_id] = shader;
      ice->state.dirty |= dirty_flag_for_cache(cache_id);
   }

   return true;
}

const void *
iris_find_previous_compile(const struct iris_context *ice,
                           enum iris_program_cache_id cache_id,
                           unsigned program_string_id)
{
   hash_table_foreach(ice->shaders.cache, entry) {
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
find_existing_assembly(struct hash_table *cache,
                       const void *assembly,
                       unsigned assembly_size)
{
   hash_table_foreach(cache, entry) {
      const struct iris_compiled_shader *existing = entry->data;
      if (existing->prog_data->program_size == assembly_size &&
          memcmp(existing->map, assembly, assembly_size) == 0)
         return existing;
   }
   return NULL;
}

static struct iris_compiled_shader *
iris_upload_shader(struct iris_context *ice,
                   enum iris_program_cache_id cache_id,
                   uint32_t key_size,
                   const void *key,
                   const void *assembly,
                   struct brw_stage_prog_data *prog_data)
{
   struct iris_screen *screen = (void *) ice->ctx.screen;
   struct gen_device_info *devinfo = &screen->devinfo;
   struct hash_table *cache = ice->shaders.cache;
   struct iris_compiled_shader *shader =
      ralloc_size(cache, sizeof(struct iris_compiled_shader) +
                  ice->vtbl.derived_program_state_size(cache_id));
   const struct iris_compiled_shader *existing =
      find_existing_assembly(cache, assembly, prog_data->program_size);

   /* If we can find a matching prog in the cache already, then reuse the
    * existing stuff without creating new copy into the underlying buffer
    * object.  This is notably useful for programs generating shaders at
    * runtime, where multiple shaders may compile to the same thing in our
    * backend.
    */
   if (existing) {
      shader->offset = existing->offset;
   } else {
      shader->buffer = NULL;
      u_upload_alloc(ice->shaders.uploader, 0, prog_data->program_size,
                     64, &shader->offset, &shader->buffer, &shader->map);
      memcpy(shader->map, assembly, prog_data->program_size);
   }

   shader->prog_data = prog_data;

   ralloc_steal(shader, shader->prog_data);
   ralloc_steal(shader->prog_data, prog_data->param);
   ralloc_steal(shader->prog_data, prog_data->pull_param);

   /* Store the 3DSTATE shader packets and other derived state. */
   ice->vtbl.store_derived_program_state(devinfo, cache_id, shader);

   struct keybox *keybox = make_keybox(cache, cache_id, key, key_size);
   _mesa_hash_table_insert(ice->shaders.cache, keybox, shader);

   return shader;
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
   assert(cache_id != IRIS_CACHE_BLORP);

   struct iris_compiled_shader *shader =
      iris_upload_shader(ice, cache_id, key_size_for_cache(cache_id), key,
                         assembly, prog_data);

   ice->shaders.prog[cache_id] = shader;
   ice->state.dirty |= dirty_flag_for_cache(cache_id);
}

bool
iris_blorp_lookup_shader(struct blorp_batch *blorp_batch,
                         const void *key, uint32_t key_size,
                         uint32_t *kernel_out, void *prog_data_out)
{
   struct blorp_context *blorp = blorp_batch->blorp;
   struct iris_context *ice = blorp->driver_ctx;
   struct iris_batch *batch = blorp_batch->driver_batch;
   struct iris_compiled_shader *shader =
      iris_find_cached_shader(ice, IRIS_CACHE_BLORP, key, key_size);

   if (!shader)
      return false;

   struct iris_bo *bo = iris_resource_bo(shader->buffer);
   *kernel_out = iris_bo_offset_from_base_address(bo) + shader->offset;
   *((void **) prog_data_out) = shader->prog_data;

   iris_use_pinned_bo(batch, bo, false);

   return true;
}

bool
iris_blorp_upload_shader(struct blorp_batch *blorp_batch,
                         const void *key, uint32_t key_size,
                         const void *kernel, UNUSED uint32_t kernel_size,
                         const struct brw_stage_prog_data *prog_data_templ,
                         UNUSED uint32_t prog_data_size,
                         uint32_t *kernel_out, void *prog_data_out)
{
   struct blorp_context *blorp = blorp_batch->blorp;
   struct iris_context *ice = blorp->driver_ctx;
   struct iris_batch *batch = blorp_batch->driver_batch;

   void *prog_data = ralloc_size(NULL, prog_data_size);
   memcpy(prog_data, prog_data_templ, prog_data_size);

   struct iris_compiled_shader *shader =
      iris_upload_shader(ice, IRIS_CACHE_BLORP, key_size, key, kernel,
                         prog_data);

   struct iris_bo *bo = iris_resource_bo(shader->buffer);
   *kernel_out = iris_bo_offset_from_base_address(bo) + shader->offset;
   *((void **) prog_data_out) = shader->prog_data;

   iris_use_pinned_bo(batch, bo, false);

   return true;
}

void
iris_init_program_cache(struct iris_context *ice)
{
   ice->shaders.cache =
      _mesa_hash_table_create(ice, keybox_hash, keybox_equals);

   ice->shaders.uploader =
      u_upload_create(&ice->ctx, 16384, PIPE_BIND_CUSTOM, PIPE_USAGE_IMMUTABLE,
                      IRIS_RESOURCE_FLAG_SHADER_MEMZONE);
}

void
iris_destroy_program_cache(struct iris_context *ice)
{
   for (int i = 0; i < MESA_SHADER_STAGES; i++) {
      ice->shaders.prog[i] = NULL;
   }

   u_upload_destroy(ice->shaders.uploader);

   ralloc_free(ice->shaders.cache);
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
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;
   const struct gen_device_info *devinfo = &screen->devinfo;

   hash_table_foreach(ice->shaders.cache, entry) {
      const struct keybox *keybox = entry->key;
      struct iris_compiled_shader *shader = entry->data;
      fprintf(stderr, "%s:\n", cache_name(keybox->cache_id));
      brw_disassemble(devinfo, shader->map, 0,
                      shader->prog_data->program_size, stderr);
   }
}

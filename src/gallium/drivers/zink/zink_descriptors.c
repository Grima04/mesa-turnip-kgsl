/*
 * Copyright © 2020 Mike Blumenkrantz
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
 * 
 * Authors:
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */

#include "tgsi/tgsi_from_mesa.h"


#include "zink_context.h"
#include "zink_descriptors.h"
#include "zink_program.h"
#include "zink_resource.h"
#include "zink_screen.h"

static bool
get_invalidated_desc_set(struct zink_descriptor_set *zds)
{
   if (!zds->invalid)
      return false;
   return p_atomic_read(&zds->reference.count) == 1;
}

static struct zink_descriptor_set *
allocate_desc_set(struct zink_screen *screen, struct zink_program *pg, enum zink_descriptor_type type, unsigned descs_used, bool is_compute)
{
   VkDescriptorSetAllocateInfo dsai;
#define DESC_BUCKET_FACTOR 10
   unsigned bucket_size = pg->num_descriptors[type] ? DESC_BUCKET_FACTOR : 1;
   if (pg->num_descriptors[type]) {
      for (unsigned desc_factor = DESC_BUCKET_FACTOR; desc_factor < descs_used; desc_factor *= DESC_BUCKET_FACTOR)
         bucket_size = desc_factor;
   }
   VkDescriptorSetLayout layouts[bucket_size];
   memset((void *)&dsai, 0, sizeof(dsai));
   dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   dsai.pNext = NULL;
   dsai.descriptorPool = pg->descpool[type];
   dsai.descriptorSetCount = bucket_size;
   for (unsigned i = 0; i < bucket_size; i ++)
      layouts[i] = pg->dsl[type];
   dsai.pSetLayouts = layouts;

   VkDescriptorSet desc_set[bucket_size];
   if (vkAllocateDescriptorSets(screen->dev, &dsai, desc_set) != VK_SUCCESS) {
      debug_printf("ZINK: %p failed to allocate descriptor set :/\n", pg);
      return VK_NULL_HANDLE;
   }

   struct zink_descriptor_set *alloc = ralloc_array(pg, struct zink_descriptor_set, bucket_size);
   assert(alloc);
   unsigned num_resources = zink_program_num_bindings_typed(pg, type, is_compute);
   struct zink_resource **resources = rzalloc_array(pg, struct zink_resource*, num_resources * bucket_size);
   assert(resources);
   void **samplers = NULL;
   if (type == ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW) {
      samplers = rzalloc_array(pg, void*, num_resources * bucket_size);
      assert(samplers);
   }
   for (unsigned i = 0; i < bucket_size; i ++) {
      struct zink_descriptor_set *zds = &alloc[i];
      pipe_reference_init(&zds->reference, 1);
      zds->pg = pg;
      zds->hash = 0;
      zds->invalid = true;
      zds->recycled = false;
      zds->type = type;
#ifndef NDEBUG
      zds->num_resources = num_resources;
#endif
      if (type == ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW) {
         zds->sampler_views = (struct zink_sampler_view**)&resources[i * pg->num_descriptors[type]];
         zds->sampler_states = (struct zink_sampler_state**)&samplers[i * pg->num_descriptors[type]];
      } else
         zds->resources = (struct zink_resource**)&resources[i * pg->num_descriptors[type]];
      zds->desc_set = desc_set[i];
      if (i > 0)
         util_dynarray_append(&pg->alloc_desc_sets[type], struct zink_descriptor_set *, zds);
   }
   return alloc;
}

static void
populate_zds_key(struct zink_context *ctx, enum zink_descriptor_type type, bool is_compute,
                 struct zink_descriptor_state_key *key) {
   if (is_compute) {
      for (unsigned i = 1; i < ZINK_SHADER_COUNT; i++)
         key->exists[i] = false;
      key->exists[0] = true;
      key->state[0] = ctx->descriptor_states[is_compute].state[type];
   } else {
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         key->exists[i] = ctx->gfx_descriptor_states[i].valid[type];
         key->state[i] = ctx->gfx_descriptor_states[i].state[type];
      }
   }
}

struct zink_descriptor_set *
zink_descriptor_set_get(struct zink_context *ctx,
                               struct zink_batch *batch,
                               struct zink_program *pg,
                               enum zink_descriptor_type type,
                               bool is_compute,
                               bool *cache_hit)
{
   *cache_hit = false;
   struct zink_descriptor_set *zds;
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   unsigned descs_used = 1;
   assert(type < ZINK_DESCRIPTOR_TYPES);
   uint32_t hash = pg->num_descriptors[type] ? ctx->descriptor_states[is_compute].state[type] : 0;
   struct zink_descriptor_state_key key;
   populate_zds_key(ctx, type, is_compute, &key);
   if (pg->last_set[type] && pg->last_set[type]->hash == hash &&
       zink_desc_state_equal(&pg->last_set[type]->key, &key)) {
      zds = pg->last_set[type];
      *cache_hit = !zds->invalid;
      if (pg->num_descriptors[type] && zds->recycled) {
         struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pg->free_desc_sets[type], hash, &key);
         if (he)
            _mesa_hash_table_remove(pg->free_desc_sets[type], he);
      }
      goto out;
   }

   if (pg->num_descriptors[type]) {
      struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pg->desc_sets[type], hash, &key);
      bool recycled = false;
      if (he) {
          zds = (void*)he->data;
          /* this shouldn't happen, but if we somehow get a cache hit on an invalidated, active desc set then
           * we probably should just crash here rather than later
           */
          assert(!zds->invalid);
      }
      if (!he) {
         he = _mesa_hash_table_search_pre_hashed(pg->free_desc_sets[type], hash, &key);
         recycled = true;
      }
      if (he) {
         zds = (void*)he->data;
         *cache_hit = !zds->invalid;
         if (recycled) {
            /* need to migrate this entry back to the in-use hash */
            _mesa_hash_table_remove(pg->free_desc_sets[type], he);
            goto out;
         }
         goto quick_out;
      }

      if (util_dynarray_num_elements(&pg->alloc_desc_sets[type], struct zink_descriptor_set *)) {
         /* grab one off the allocated array */
         zds = util_dynarray_pop(&pg->alloc_desc_sets[type], struct zink_descriptor_set *);
         goto out;
      }

      if (_mesa_hash_table_num_entries(pg->free_desc_sets[type])) {
         /* try for an invalidated set first */
         unsigned count = 0;
         hash_table_foreach(pg->free_desc_sets[type], he) {
            struct zink_descriptor_set *tmp = he->data;
            if ((count++ >= 100 && tmp->reference.count == 1) || get_invalidated_desc_set(he->data)) {
               zds = tmp;
               assert(p_atomic_read(&zds->reference.count) == 1);
               zink_descriptor_set_invalidate(zds);
               _mesa_hash_table_remove(pg->free_desc_sets[type], he);
               goto out;
            }
         }
      }

      descs_used = _mesa_hash_table_num_entries(pg->desc_sets[type]) + _mesa_hash_table_num_entries(pg->free_desc_sets[type]);
      if (descs_used + pg->num_descriptors[type] > ZINK_DEFAULT_MAX_DESCS) {
         batch = zink_flush_batch(ctx, batch);
         zink_batch_reference_program(batch, pg);
         return zink_descriptor_set_get(ctx, batch, pg, type, is_compute, cache_hit);
      }
   } else {
      if (pg->last_set[type] && !pg->last_set[type]->hash) {
         zds = pg->last_set[type];
         *cache_hit = true;
         goto quick_out;
      }
   }

   zds = allocate_desc_set(screen, pg, type, descs_used, is_compute);
out:
   zds->hash = hash;
   populate_zds_key(ctx, type, is_compute, &zds->key);
   zds->recycled = false;
   if (pg->num_descriptors[type])
      _mesa_hash_table_insert_pre_hashed(pg->desc_sets[type], hash, &zds->key, zds);
   else {
      /* we can safely apply the null set to all the slots which will need it here */
      for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
         if (!pg->num_descriptors[i])
            pg->last_set[i] = zds;
      }
   }
quick_out:
   zds->invalid = false;
   if (zink_batch_add_desc_set(batch, zds))
      batch->descs_used += pg->num_descriptors[type];
   pg->last_set[type] = zds;
   return zds;
}

void
zink_descriptor_set_recycle(struct zink_descriptor_set *zds)
{
   struct zink_program *pg = zds->pg;
   /* if desc set is still in use by a batch, don't recache */
   uint32_t refcount = p_atomic_read(&zds->reference.count);
   if (refcount != 1)
      return;
   /* this is a null set */
   if (!zds->hash && !pg->num_descriptors[zds->type])
      return;

   struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pg->desc_sets[zds->type], zds->hash, &zds->key);
   if (!he)
      /* desc sets can be used multiple times in the same batch */
      return;

   _mesa_hash_table_remove(pg->desc_sets[zds->type], he);
   if (zds->invalid) {
      if (pg->last_set[zds->type] == zds)
         pg->last_set[zds->type] = NULL;
      zink_descriptor_set_invalidate(zds);
      util_dynarray_append(&pg->alloc_desc_sets[zds->type], struct zink_descriptor_set *, zds);
   } else {
      zds->recycled = true;
      _mesa_hash_table_insert_pre_hashed(pg->free_desc_sets[zds->type], zds->hash, &zds->key, zds);
   }
}


static void
desc_set_ref_add(struct zink_descriptor_set *zds, struct zink_descriptor_refs *refs, void **ref_ptr, void *ptr)
{
   struct zink_descriptor_reference ref = {ref_ptr, &zds->invalid};
   *ref_ptr = ptr;
   if (ptr)
      util_dynarray_append(&refs->refs, struct zink_descriptor_reference, ref);
}

void
zink_image_view_desc_set_add(struct zink_image_view *image_view, struct zink_descriptor_set *zds, unsigned idx)
{
   desc_set_ref_add(zds, &image_view->desc_set_refs, (void**)&zds->image_views[idx], image_view);
}

void
zink_sampler_state_desc_set_add(struct zink_sampler_state *sampler_state, struct zink_descriptor_set *zds, unsigned idx)
{
   desc_set_ref_add(zds, &sampler_state->desc_set_refs, (void**)&zds->sampler_states[idx], sampler_state);
}

void
zink_sampler_view_desc_set_add(struct zink_sampler_view *sampler_view, struct zink_descriptor_set *zds, unsigned idx)
{
   desc_set_ref_add(zds, &sampler_view->desc_set_refs, (void**)&zds->sampler_views[idx], sampler_view);
}

void
zink_resource_desc_set_add(struct zink_resource *res, struct zink_descriptor_set *zds, unsigned idx)
{
   desc_set_ref_add(zds, &res->desc_set_refs, (void**)&zds->resources[idx], res);
}

void
zink_descriptor_set_refs_clear(struct zink_descriptor_refs *refs, void *ptr)
{
   util_dynarray_foreach(&refs->refs, struct zink_descriptor_reference, ref) {
      if (*ref->ref == ptr) {
         *ref->invalid = true;
         *ref->ref = NULL;
      }
   }
   util_dynarray_fini(&refs->refs);
}

bool
zink_descriptor_program_init(VkDevice dev,
                       struct zink_shader *stages[ZINK_SHADER_COUNT],
                       struct zink_program *pg)
{
   VkDescriptorSetLayoutBinding bindings[ZINK_DESCRIPTOR_TYPES][PIPE_SHADER_TYPES * 32];
   int num_bindings[ZINK_DESCRIPTOR_TYPES] = {};

   VkDescriptorPoolSize sizes[6] = {};
   int type_map[12];
   unsigned num_types = 0;
   memset(type_map, -1, sizeof(type_map));

   for (int i = 0; i < ZINK_SHADER_COUNT; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;

      VkShaderStageFlagBits stage_flags = zink_shader_stage(pipe_shader_type_from_mesa(shader->nir->info.stage));
      for (int j = 0; j < ZINK_DESCRIPTOR_TYPES; j++) {
         for (int k = 0; k < shader->num_bindings[j]; k++) {
            assert(num_bindings[j] < ARRAY_SIZE(bindings[j]));
            bindings[j][num_bindings[j]].binding = shader->bindings[j][k].binding;
            bindings[j][num_bindings[j]].descriptorType = shader->bindings[j][k].type;
            bindings[j][num_bindings[j]].descriptorCount = shader->bindings[j][k].size;
            bindings[j][num_bindings[j]].stageFlags = stage_flags;
            bindings[j][num_bindings[j]].pImmutableSamplers = NULL;
            if (type_map[shader->bindings[j][k].type] == -1) {
               type_map[shader->bindings[j][k].type] = num_types++;
               sizes[type_map[shader->bindings[j][k].type]].type = shader->bindings[j][k].type;
            }
            sizes[type_map[shader->bindings[j][k].type]].descriptorCount += shader->bindings[j][k].size;
            ++num_bindings[j];
         }
      }
   }

   unsigned total_descs = 0;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      pg->num_descriptors[i] = num_bindings[i];
      total_descs += num_bindings[i];;
   }
   if (!total_descs)
      return true;

   for (int i = 0; i < num_types; i++)
      sizes[i].descriptorCount *= ZINK_DEFAULT_MAX_DESCS;

   VkDescriptorSetLayout null_set = VK_NULL_HANDLE;
   VkDescriptorPool null_pool = VK_NULL_HANDLE;
   bool found_descriptors = false;
   for (unsigned i = ZINK_DESCRIPTOR_TYPES - 1; i < ZINK_DESCRIPTOR_TYPES; i--) {
      VkDescriptorSetLayoutCreateInfo dcslci = {};
      dcslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      dcslci.pNext = NULL;
      dcslci.flags = 0;

      if (!num_bindings[i]) {
         if (!found_descriptors)
            continue;
         if (!null_set) {
            dcslci.bindingCount = 1;
            VkDescriptorSetLayoutBinding null_binding;
            null_binding.binding = 1;
            null_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            null_binding.descriptorCount = 1;
            null_binding.pImmutableSamplers = NULL;
            null_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                      VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                      VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
            dcslci.pBindings = &null_binding;
            if (vkCreateDescriptorSetLayout(dev, &dcslci, 0, &null_set) != VK_SUCCESS) {
               debug_printf("vkCreateDescriptorSetLayout failed\n");
               return false;
            }
            VkDescriptorPoolCreateInfo dpci = {};
            VkDescriptorPoolSize null_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ZINK_DESCRIPTOR_TYPES};
            dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            dpci.pPoolSizes = &null_size;
            dpci.poolSizeCount = 1;
            dpci.flags = 0;
            dpci.maxSets = 1;
            if (vkCreateDescriptorPool(dev, &dpci, 0, &null_pool) != VK_SUCCESS)
               return false;
         }
         pg->dsl[i] = null_set;
         pg->descpool[i] = null_pool;
         continue;
      }
      dcslci.bindingCount = num_bindings[i];
      dcslci.pBindings = bindings[i];
      found_descriptors = true;

      if (vkCreateDescriptorSetLayout(dev, &dcslci, 0, &pg->dsl[i]) != VK_SUCCESS) {
         debug_printf("vkCreateDescriptorSetLayout failed\n");
         return false;
      }
      VkDescriptorPoolSize type_sizes[2] = {};
      int num_type_sizes = 0;
      switch (i) {
      case ZINK_DESCRIPTOR_TYPE_UBO:
         if (type_map[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER]];
            num_type_sizes++;
         }
         if (type_map[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC]];
            num_type_sizes++;
         }
         break;
      case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
         if (type_map[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER]];
            num_type_sizes++;
         }
         if (type_map[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER]];
            num_type_sizes++;
         }
         break;
      case ZINK_DESCRIPTOR_TYPE_SSBO:
         if (type_map[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] != -1) {
            num_type_sizes = 1;
            type_sizes[0] = sizes[type_map[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER]];
         }
         break;
      case ZINK_DESCRIPTOR_TYPE_IMAGE:
         if (type_map[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER]];
            num_type_sizes++;
         }
         if (type_map[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] != -1) {
            type_sizes[num_type_sizes] = sizes[type_map[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE]];
            num_type_sizes++;
         }
         break;
      }
      VkDescriptorPoolCreateInfo dpci = {};
      dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      dpci.pPoolSizes = type_sizes;
      dpci.poolSizeCount = num_type_sizes;
      dpci.flags = 0;
      dpci.maxSets = ZINK_DEFAULT_MAX_DESCS;
      if (vkCreateDescriptorPool(dev, &dpci, 0, &pg->descpool[i]) != VK_SUCCESS) {
         return false;
      }
   }
   return true;
}

void
zink_descriptor_set_invalidate(struct zink_descriptor_set *zds)
{
   zds->invalid = true;
}

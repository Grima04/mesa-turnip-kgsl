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

#ifndef ZINK_DESCRIPTOR_H
# define  ZINK_DESCRIPTOR_H
#include <vulkan/vulkan.h>
#include "util/u_dynarray.h"

enum zink_descriptor_type {
   ZINK_DESCRIPTOR_TYPE_UBO,
   ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW,
   ZINK_DESCRIPTOR_TYPE_SSBO,
   ZINK_DESCRIPTOR_TYPE_IMAGE,
   ZINK_DESCRIPTOR_TYPES,
};


struct zink_descriptor_refs {
   struct util_dynarray refs;
};


#include "zink_context.h"

struct hash_table;

struct zink_program;
struct zink_resource;
struct zink_shader;


struct zink_descriptor_state_key {
   bool exists[ZINK_SHADER_COUNT];
   uint32_t state[ZINK_SHADER_COUNT];
};

struct zink_descriptor_pool {
   struct hash_table *desc_sets;
   struct hash_table *free_desc_sets;
   struct util_dynarray alloc_desc_sets;
   VkDescriptorPool descpool;
   VkDescriptorSetLayout dsl;
   unsigned num_descriptors;
};

struct zink_descriptor_set {
   struct zink_descriptor_pool *pool;
   enum zink_descriptor_type type;
   struct pipe_reference reference; //incremented for batch usage
   VkDescriptorSet desc_set;
   uint32_t hash;
   bool invalid;
   bool recycled;
   struct zink_descriptor_state_key key;
#ifndef NDEBUG
   /* for extra debug asserts */
   unsigned num_resources;
#endif
   union {
      struct zink_resource **resources;
      struct zink_image_view **image_views;
      struct {
         struct zink_sampler_view **sampler_views;
         struct zink_sampler_state **sampler_states;
      };
   };
};


struct zink_descriptor_reference {
   void **ref;
   bool *invalid;
};
void
zink_descriptor_set_refs_clear(struct zink_descriptor_refs *refs, void *ptr);

void
zink_image_view_desc_set_add(struct zink_image_view *image_view, struct zink_descriptor_set *zds, unsigned idx);
void
zink_sampler_state_desc_set_add(struct zink_sampler_state *sampler_state, struct zink_descriptor_set *zds, unsigned idx);
void
zink_sampler_view_desc_set_add(struct zink_sampler_view *sv, struct zink_descriptor_set *zds, unsigned idx);
void
zink_resource_desc_set_add(struct zink_resource *res, struct zink_descriptor_set *zds, unsigned idx);

struct zink_descriptor_set *
zink_descriptor_set_get(struct zink_context *ctx,
                               struct zink_batch *batch,
                               struct zink_program *pg,
                               enum zink_descriptor_type type,
                               bool is_compute,
                               bool *cache_hit);
void
zink_descriptor_set_recycle(struct zink_descriptor_set *zds);

bool
zink_descriptor_program_init(struct zink_screen *screen,
                       struct zink_shader *stages[ZINK_SHADER_COUNT],
                       struct zink_program *pg);

void
zink_descriptor_set_invalidate(struct zink_descriptor_set *zds);

void
zink_descriptor_pool_free(struct zink_screen *screen, struct zink_descriptor_pool *pool);

#endif

/*
 * Copyright 2018 Collabora Ltd.
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

#include "zink_program.h"

#include "zink_compiler.h"
#include "zink_context.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_state.h"

#include "util/hash_table.h"
#include "util/set.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "tgsi/tgsi_from_mesa.h"

struct gfx_pipeline_cache_entry {
   struct zink_gfx_pipeline_state state;
   VkPipeline pipeline;
};

struct compute_pipeline_cache_entry {
   struct zink_compute_pipeline_state state;
   VkPipeline pipeline;
};

void
debug_describe_zink_gfx_program(char *buf, const struct zink_gfx_program *ptr)
{
   sprintf(buf, "zink_gfx_program");
}

void
debug_describe_zink_compute_program(char *buf, const struct zink_compute_program *ptr)
{
   sprintf(buf, "zink_compute_program");
}

static void
debug_describe_zink_shader_module(char *buf, const struct zink_shader_module *ptr)
{
   sprintf(buf, "zink_shader_module");
}

static void
debug_describe_zink_shader_cache(char* buf, const struct zink_shader_cache *ptr)
{
   sprintf(buf, "zink_shader_cache");
}

/* copied from iris */
struct keybox {
   uint16_t size;
   gl_shader_stage stage;
   uint8_t data[0];
};

static struct keybox *
make_keybox(void *mem_ctx,
            gl_shader_stage stage,
            const void *key,
            uint32_t key_size)
{
   struct keybox *keybox =
      ralloc_size(mem_ctx, sizeof(struct keybox) + key_size);

   keybox->stage = stage;
   keybox->size = key_size;
   memcpy(keybox->data, key, key_size);

   return keybox;
}

static uint32_t
keybox_hash(const void *void_key)
{
   const struct keybox *key = void_key;
   return _mesa_hash_data(&key->stage, key->size + sizeof(key->stage));
}

static bool
keybox_equals(const void *void_a, const void *void_b)
{
   const struct keybox *a = void_a, *b = void_b;
   if (a->size != b->size)
      return false;

   return memcmp(a->data, b->data, a->size) == 0;
}

static VkDescriptorSetLayout
create_desc_set_layout(VkDevice dev,
                       struct zink_shader *stages[ZINK_SHADER_COUNT],
                       unsigned *num_descriptors,
                       VkDescriptorPool *descpool)
{
   VkDescriptorSetLayoutBinding bindings[(PIPE_SHADER_TYPES * (PIPE_MAX_CONSTANT_BUFFERS + PIPE_MAX_SAMPLERS + PIPE_MAX_SHADER_BUFFERS + PIPE_MAX_SHADER_IMAGES))];
   int num_bindings = 0;

   for (int i = 0; i < ZINK_SHADER_COUNT; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;

      VkShaderStageFlagBits stage_flags = zink_shader_stage(pipe_shader_type_from_mesa(shader->nir->info.stage));
      for (int j = 0; j < shader->num_bindings; j++) {
         assert(num_bindings < ARRAY_SIZE(bindings));
         bindings[num_bindings].binding = shader->bindings[j].binding;
         bindings[num_bindings].descriptorType = shader->bindings[j].type;
         bindings[num_bindings].descriptorCount = shader->bindings[j].size;
         bindings[num_bindings].stageFlags = stage_flags;
         bindings[num_bindings].pImmutableSamplers = NULL;
         ++num_bindings;
      }
   }

   *num_descriptors = num_bindings;
   if (!num_bindings)
      return VK_NULL_HANDLE;

   VkDescriptorSetLayoutCreateInfo dcslci = {};
   dcslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   dcslci.pNext = NULL;
   dcslci.flags = 0;
   dcslci.bindingCount = num_bindings;
   dcslci.pBindings = bindings;

   VkDescriptorSetLayout dsl;
   if (vkCreateDescriptorSetLayout(dev, &dcslci, 0, &dsl) != VK_SUCCESS) {
      debug_printf("vkCreateDescriptorSetLayout failed\n");
      return VK_NULL_HANDLE;
   }
   VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         ZINK_BATCH_DESC_SIZE},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,   ZINK_BATCH_DESC_SIZE},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ZINK_BATCH_DESC_SIZE},
      {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,   ZINK_BATCH_DESC_SIZE},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          ZINK_BATCH_DESC_SIZE},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         ZINK_BATCH_DESC_SIZE},
   };

   VkDescriptorPoolCreateInfo dpci = {};
   dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpci.pPoolSizes = sizes;
   dpci.poolSizeCount = ARRAY_SIZE(sizes);
   dpci.flags = 0;
   dpci.maxSets = ZINK_BATCH_DESC_SIZE;
   if (vkCreateDescriptorPool(dev, &dpci, 0, descpool) != VK_SUCCESS) {
      vkDestroyDescriptorSetLayout(dev, dsl, NULL);
      return VK_NULL_HANDLE;
   }

   return dsl;
}

static VkPipelineLayout
create_gfx_pipeline_layout(VkDevice dev, VkDescriptorSetLayout dsl)
{
   VkPipelineLayoutCreateInfo plci = {};
   plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

   plci.pSetLayouts = &dsl;
   plci.setLayoutCount = !!dsl;


   VkPushConstantRange pcr[2] = {};
   pcr[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
   pcr[0].offset = offsetof(struct zink_push_constant, draw_mode_is_indexed);
   pcr[0].size = 2 * sizeof(unsigned);
   pcr[1].stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
   pcr[1].offset = offsetof(struct zink_push_constant, default_inner_level);
   pcr[1].size = sizeof(float) * 6;
   plci.pushConstantRangeCount = 2;
   plci.pPushConstantRanges = &pcr[0];

   VkPipelineLayout layout;
   if (vkCreatePipelineLayout(dev, &plci, NULL, &layout) != VK_SUCCESS) {
      debug_printf("vkCreatePipelineLayout failed!\n");
      return VK_NULL_HANDLE;
   }

   return layout;
}

static VkPipelineLayout
create_compute_pipeline_layout(VkDevice dev, VkDescriptorSetLayout dsl)
{
   VkPipelineLayoutCreateInfo plci = {};
   plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

   plci.pSetLayouts = &dsl;
   plci.setLayoutCount = !!dsl;

   VkPipelineLayout layout;
   if (vkCreatePipelineLayout(dev, &plci, NULL, &layout) != VK_SUCCESS) {
      debug_printf("vkCreatePipelineLayout failed!\n");
      return VK_NULL_HANDLE;
   }

   return layout;
}

static void
shader_key_vs_gen(struct zink_context *ctx, struct zink_shader *zs,
                  struct zink_shader *shaders[ZINK_SHADER_COUNT], struct zink_shader_key *key)
{
   struct zink_vs_key *vs_key = &key->key.vs;
   key->size = sizeof(struct zink_vs_key);

   vs_key->shader_id = zs->shader_id;
   vs_key->clip_halfz = ctx->rast_state->base.clip_halfz;
   switch (zs->nir->info.stage) {
   case MESA_SHADER_VERTEX:
      vs_key->last_vertex_stage = !shaders[PIPE_SHADER_TESS_EVAL] && !shaders[PIPE_SHADER_GEOMETRY];
      vs_key->push_drawid = ctx->drawid_broken;
      break;
   case MESA_SHADER_TESS_EVAL:
      vs_key->last_vertex_stage = !shaders[PIPE_SHADER_GEOMETRY];
      break;
   case MESA_SHADER_GEOMETRY:
      vs_key->last_vertex_stage = true;
      break;
   default:
      unreachable("impossible case");
   }
}

static void
shader_key_fs_gen(struct zink_context *ctx, struct zink_shader *zs,
                  struct zink_shader *shaders[ZINK_SHADER_COUNT], struct zink_shader_key *key)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_fs_key *fs_key = &key->key.fs;
   key->size = sizeof(struct zink_fs_key);

   fs_key->shader_id = zs->shader_id;
   //fs_key->flat_shade = ctx->rast_state->base.flatshade;

   /* if gl_SampleMask[] is written to, we have to ensure that we get a shader with the same sample count:
    * in GL, rast_samples==1 means ignore gl_SampleMask[]
    * in VK, gl_SampleMask[] is never ignored
    */
   if (zs->nir->info.outputs_written & (1 << FRAG_RESULT_SAMPLE_MASK))
      fs_key->samples = !!ctx->fb_state.samples;
   fs_key->force_dual_color_blend = screen->driconf.dual_color_blend_by_location &&
                                    ctx->gfx_pipeline_state.blend_state->dual_src_blend &&
                                    ctx->gfx_pipeline_state.blend_state->attachments[1].blendEnable;
   if (((shaders[PIPE_SHADER_GEOMETRY] && shaders[PIPE_SHADER_GEOMETRY]->nir->info.gs.output_primitive == GL_POINTS) ||
       ctx->gfx_prim_mode == PIPE_PRIM_POINTS) && ctx->rast_state->base.point_quad_rasterization && ctx->rast_state->base.sprite_coord_enable) {
      fs_key->coord_replace_bits = ctx->rast_state->base.sprite_coord_enable;
      fs_key->coord_replace_yinvert = !!ctx->rast_state->base.sprite_coord_mode;
   }
}

static void
shader_key_tcs_gen(struct zink_context *ctx, struct zink_shader *zs,
                   struct zink_shader *shaders[ZINK_SHADER_COUNT], struct zink_shader_key *key)
{
   struct zink_tcs_key *tcs_key = &key->key.tcs;
   key->size = sizeof(struct zink_tcs_key);

   tcs_key->shader_id = zs->shader_id;
   tcs_key->vertices_per_patch = ctx->gfx_pipeline_state.vertices_per_patch;
   tcs_key->vs_outputs_written = shaders[PIPE_SHADER_VERTEX]->nir->info.outputs_written;
}

typedef void (*zink_shader_key_gen)(struct zink_context *ctx, struct zink_shader *zs,
                                    struct zink_shader *shaders[ZINK_SHADER_COUNT],
                                    struct zink_shader_key *key);
static zink_shader_key_gen shader_key_vtbl[] =
{
   [MESA_SHADER_VERTEX] = shader_key_vs_gen,
   [MESA_SHADER_TESS_CTRL] = shader_key_tcs_gen,
   /* reusing vs key for now since we're only using clip_halfz */
   [MESA_SHADER_TESS_EVAL] = shader_key_vs_gen,
   [MESA_SHADER_GEOMETRY] = shader_key_vs_gen,
   [MESA_SHADER_FRAGMENT] = shader_key_fs_gen,
};

static struct zink_shader_module *
get_shader_module_for_stage(struct zink_context *ctx, struct zink_shader *zs, struct zink_gfx_program *prog)
{
   gl_shader_stage stage = zs->nir->info.stage;
   struct zink_shader_key key = {};
   VkShaderModule mod;
   struct zink_shader_module *zm;
   struct keybox *keybox;
   uint32_t hash;

   shader_key_vtbl[stage](ctx, zs, ctx->gfx_stages, &key);
   keybox = make_keybox(NULL, stage, &key, key.size);
   hash = keybox_hash(keybox);
   struct hash_entry *entry = _mesa_hash_table_search_pre_hashed(prog->shader_cache->shader_cache,
                                                                 hash, keybox);

   if (entry) {
      ralloc_free(keybox);
      zm = entry->data;
   } else {
      zm = CALLOC_STRUCT(zink_shader_module);
      if (!zm) {
         ralloc_free(keybox);
         return NULL;
      }
      pipe_reference_init(&zm->reference, 1);
      mod = zink_shader_compile(zink_screen(ctx->base.screen), zs, &key,
                                prog->shader_slot_map, &prog->shader_slots_reserved);
      if (!mod) {
         ralloc_free(keybox);
         FREE(zm);
         return NULL;
      }
      zm->shader = mod;

      _mesa_hash_table_insert_pre_hashed(prog->shader_cache->shader_cache, hash, keybox, zm);
   }
   return zm;
}

static void
zink_destroy_shader_module(struct zink_screen *screen, struct zink_shader_module *zm)
{
   vkDestroyShaderModule(screen->dev, zm->shader, NULL);
   free(zm);
}

static inline void
zink_shader_module_reference(struct zink_screen *screen,
                           struct zink_shader_module **dst,
                           struct zink_shader_module *src)
{
   struct zink_shader_module *old_dst = dst ? *dst : NULL;

   if (pipe_reference_described(old_dst ? &old_dst->reference : NULL, &src->reference,
                                (debug_reference_descriptor)debug_describe_zink_shader_module))
      zink_destroy_shader_module(screen, old_dst);
   if (dst) *dst = src;
}

static void
zink_destroy_shader_cache(struct zink_screen *screen, struct zink_shader_cache *sc)
{
   hash_table_foreach(sc->shader_cache, entry) {
      struct zink_shader_module *zm = entry->data;
      zink_shader_module_reference(screen, &zm, NULL);
   }
   _mesa_hash_table_destroy(sc->shader_cache, NULL);
   free(sc);
}

static inline void
zink_shader_cache_reference(struct zink_screen *screen,
                           struct zink_shader_cache **dst,
                           struct zink_shader_cache *src)
{
   struct zink_shader_cache *old_dst = dst ? *dst : NULL;

   if (pipe_reference_described(old_dst ? &old_dst->reference : NULL, &src->reference,
                                (debug_reference_descriptor)debug_describe_zink_shader_cache))
      zink_destroy_shader_cache(screen, old_dst);
   if (dst) *dst = src;
}

static void
update_shader_modules(struct zink_context *ctx, struct zink_shader *stages[ZINK_SHADER_COUNT], struct zink_gfx_program *prog, bool disallow_reuse)
{
   struct zink_shader *dirty[ZINK_SHADER_COUNT] = {NULL};

   /* we need to map pipe_shader_type -> gl_shader_stage so we can ensure that we're compiling
    * the shaders in pipeline order and have builtin input/output locations match up after being compacted
    */
   unsigned dirty_shader_stages = ctx->dirty_shader_stages;
   while (dirty_shader_stages) {
      unsigned type = u_bit_scan(&dirty_shader_stages);
      dirty[tgsi_processor_to_shader_stage(type)] = stages[type];
   }
   if (ctx->dirty_shader_stages & (1 << PIPE_SHADER_TESS_EVAL)) {
      if (dirty[MESA_SHADER_TESS_EVAL] && !dirty[MESA_SHADER_TESS_CTRL] &&
          !stages[PIPE_SHADER_TESS_CTRL]) {
         dirty[MESA_SHADER_TESS_CTRL] = stages[PIPE_SHADER_TESS_CTRL] = zink_shader_tcs_create(ctx, stages[PIPE_SHADER_VERTEX]);
         dirty[MESA_SHADER_TESS_EVAL]->generated = stages[PIPE_SHADER_TESS_CTRL];
      }
   }

   for (int i = 0; i < ZINK_SHADER_COUNT; ++i) {
      enum pipe_shader_type type = pipe_shader_type_from_mesa(i);
      if (dirty[i]) {
         struct zink_shader_module *zm;
         zm = get_shader_module_for_stage(ctx, dirty[i], prog);
         zink_shader_module_reference(zink_screen(ctx->base.screen), &prog->modules[type], zm);
         /* we probably need a new pipeline when we switch shader modules */
         ctx->gfx_pipeline_state.dirty = true;
      } else if (stages[type] && !disallow_reuse) /* reuse existing shader module */
         zink_shader_module_reference(zink_screen(ctx->base.screen), &prog->modules[type], ctx->curr_program->modules[type]);
      prog->shaders[type] = stages[type];
   }
   unsigned clean = u_bit_consecutive(PIPE_SHADER_VERTEX, 5);;
   ctx->dirty_shader_stages &= ~clean;
}

static uint32_t
hash_gfx_pipeline_state(const void *key)
{
   return _mesa_hash_data(key, offsetof(struct zink_gfx_pipeline_state, hash));
}

static bool
equals_gfx_pipeline_state(const void *a, const void *b)
{
   return !memcmp(a, b, offsetof(struct zink_gfx_pipeline_state, hash));
}

static void
init_slot_map(struct zink_context *ctx, struct zink_gfx_program *prog)
{
   unsigned existing_shaders = 0;
   bool needs_new_map = false;

   /* if there's a case where we'll be reusing any shaders, we need to (maybe) reuse the slot map too */
   if (ctx->curr_program) {
      for (int i = 0; i < ZINK_SHADER_COUNT; ++i) {
          if (ctx->curr_program->shaders[i])
             existing_shaders |= 1 << i;
      }
      /* if there's reserved slots, check whether we have enough remaining slots */
      if (ctx->curr_program->shader_slots_reserved) {
         uint64_t max_outputs = 0;
         uint32_t num_xfb_outputs = 0;
         for (int i = 0; i < ZINK_SHADER_COUNT; ++i) {
            if (i != PIPE_SHADER_TESS_CTRL &&
                i != PIPE_SHADER_FRAGMENT &&
                ctx->gfx_stages[i]) {
               uint32_t user_outputs = ctx->gfx_stages[i]->nir->info.outputs_written >> 32;
               uint32_t builtin_outputs = ctx->gfx_stages[i]->nir->info.outputs_written;
               num_xfb_outputs = MAX2(num_xfb_outputs, ctx->gfx_stages[i]->streamout.so_info.num_outputs);
               unsigned user_outputs_count = 0;
               /* check builtins first */
               u_foreach_bit(slot, builtin_outputs) {
                  switch (slot) {
                  /* none of these require slot map entries */
                  case VARYING_SLOT_POS:
                  case VARYING_SLOT_PSIZ:
                  case VARYING_SLOT_LAYER:
                  case VARYING_SLOT_PRIMITIVE_ID:
                  case VARYING_SLOT_CULL_DIST0:
                  case VARYING_SLOT_CLIP_DIST0:
                  case VARYING_SLOT_VIEWPORT:
                  case VARYING_SLOT_TESS_LEVEL_INNER:
                  case VARYING_SLOT_TESS_LEVEL_OUTER:
                     break;
                  default:
                     /* remaining legacy builtins only require 1 slot each */
                     if (ctx->curr_program->shader_slot_map[slot] == -1)
                        user_outputs_count++;
                     break;
                  }
               }
               u_foreach_bit(slot, user_outputs) {
                  if (ctx->curr_program->shader_slot_map[slot] == -1) {
                     /* user variables can span multiple slots */
                     nir_variable *var = nir_find_variable_with_location(ctx->gfx_stages[i]->nir,
                                                                         nir_var_shader_out, slot);
                     assert(var);
                     user_outputs_count += glsl_count_vec4_slots(var->type, false, false);
                  }
               }
               max_outputs = MAX2(max_outputs, user_outputs_count);
            }
         }
         /* slot map can only hold 32 entries, so dump this one if we'll exceed that */
         if (ctx->curr_program->shader_slots_reserved + max_outputs + num_xfb_outputs > 32)
            needs_new_map = true;
      }
   }

   if (needs_new_map || ctx->dirty_shader_stages == existing_shaders || !existing_shaders) {
      /* all shaders are being recompiled: new slot map */
      memset(prog->shader_slot_map, -1, sizeof(prog->shader_slot_map));
      /* we need the slot map to match up, so we can't reuse the previous cache if we can't guarantee
       * the slots match up
       * TOOD: if we compact the slot map table, we can store it on the shader keys and reuse the cache
       */
      prog->shader_cache = CALLOC_STRUCT(zink_shader_cache);
      pipe_reference_init(&prog->shader_cache->reference, 1);
      prog->shader_cache->shader_cache =  _mesa_hash_table_create(NULL, keybox_hash, keybox_equals);
   } else {
      /* at least some shaders are being reused: use existing slot map so locations match up */
      memcpy(prog->shader_slot_map, ctx->curr_program->shader_slot_map, sizeof(prog->shader_slot_map));
      prog->shader_slots_reserved = ctx->curr_program->shader_slots_reserved;
      /* and then we can also reuse the shader cache since we know the slots are the same */
      zink_shader_cache_reference(zink_screen(ctx->base.screen), &prog->shader_cache, ctx->curr_program->shader_cache);
   }
}

void
zink_update_gfx_program(struct zink_context *ctx, struct zink_gfx_program *prog)
{
   update_shader_modules(ctx, ctx->gfx_stages, prog, true);
}

struct zink_gfx_program *
zink_create_gfx_program(struct zink_context *ctx,
                        struct zink_shader *stages[ZINK_SHADER_COUNT])
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_gfx_program *prog = rzalloc(NULL, struct zink_gfx_program);
   if (!prog)
      goto fail;

   pipe_reference_init(&prog->base.reference, 1);

   init_slot_map(ctx, prog);

   update_shader_modules(ctx, stages, prog, false);

   for (int i = 0; i < ARRAY_SIZE(prog->pipelines); ++i) {
      prog->pipelines[i] = _mesa_hash_table_create(NULL,
                                                   NULL,
                                                   equals_gfx_pipeline_state);
      if (!prog->pipelines[i])
         goto fail;
   }

   for (int i = 0; i < ZINK_SHADER_COUNT; ++i) {
      if (prog->modules[i]) {
         _mesa_set_add(stages[i]->programs, prog);
         zink_gfx_program_reference(screen, NULL, prog);
      }
   }

   prog->base.dsl = create_desc_set_layout(screen->dev, stages,
                                      &prog->base.num_descriptors, &prog->base.descpool);
   if (prog->base.num_descriptors && (!prog->base.dsl || !prog->base.descpool))
      goto fail;

   prog->layout = create_gfx_pipeline_layout(screen->dev, prog->base.dsl);
   if (!prog->layout)
      goto fail;

   util_dynarray_init(&prog->base.alloc_desc_sets, NULL);
   return prog;

fail:
   if (prog)
      zink_destroy_gfx_program(screen, prog);
   return NULL;
}

static uint32_t
hash_compute_pipeline_state(const void *key)
{
   const struct zink_compute_pipeline_state *state = key;
   uint32_t hash = _mesa_hash_data(state, offsetof(struct zink_compute_pipeline_state, hash));
   if (state->use_local_size)
      hash = XXH32(&state->local_size[0], sizeof(state->local_size), hash);
   return hash;
}

void
zink_program_update_compute_pipeline_state(struct zink_context *ctx, struct zink_compute_program *comp, const uint block[3])
{
   struct zink_shader *zs = comp->shader;
   bool use_local_size = BITSET_TEST(zs->nir->info.system_values_read, SYSTEM_VALUE_LOCAL_GROUP_SIZE);
   if (ctx->compute_pipeline_state.use_local_size != use_local_size)
      ctx->compute_pipeline_state.dirty = true;
   ctx->compute_pipeline_state.use_local_size = use_local_size;

   if (ctx->compute_pipeline_state.use_local_size) {
      for (int i = 0; i < ARRAY_SIZE(ctx->compute_pipeline_state.local_size); i++) {
         if (ctx->compute_pipeline_state.local_size[i] != block[i])
            ctx->compute_pipeline_state.dirty = true;
         ctx->compute_pipeline_state.local_size[i] = block[i];
      }
   } else
      ctx->compute_pipeline_state.local_size[0] =
      ctx->compute_pipeline_state.local_size[1] =
      ctx->compute_pipeline_state.local_size[2] = 0;
}

static bool
equals_compute_pipeline_state(const void *a, const void *b)
{
   return memcmp(a, b, offsetof(struct zink_compute_pipeline_state, hash)) == 0;
}

struct zink_compute_program *
zink_create_compute_program(struct zink_context *ctx, struct zink_shader *shader)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_compute_program *comp = rzalloc(NULL, struct zink_compute_program);
   if (!comp)
      goto fail;

   pipe_reference_init(&comp->base.reference, 1);

   if (!ctx->curr_compute || !ctx->curr_compute->shader_cache) {
      /* TODO: cs shader keys placeholder for now */
      comp->shader_cache = CALLOC_STRUCT(zink_shader_cache);
      pipe_reference_init(&comp->shader_cache->reference, 1);
      comp->shader_cache->shader_cache = _mesa_hash_table_create(NULL, _mesa_hash_u32, _mesa_key_u32_equal);
   } else
      zink_shader_cache_reference(zink_screen(ctx->base.screen), &comp->shader_cache, ctx->curr_compute->shader_cache);

   if (ctx->dirty_shader_stages & (1 << PIPE_SHADER_COMPUTE)) {
      struct hash_entry *he = _mesa_hash_table_search(comp->shader_cache->shader_cache, &shader->shader_id);
      if (he)
         comp->module = he->data;
      else {
         comp->module = CALLOC_STRUCT(zink_shader_module);
         assert(comp->module);
         pipe_reference_init(&comp->module->reference, 1);
         comp->module->shader = zink_shader_compile(screen, shader, NULL, NULL, NULL);
         assert(comp->module->shader);
         _mesa_hash_table_insert(comp->shader_cache->shader_cache, &shader->shader_id, comp->module);
      }
   } else
     comp->module = ctx->curr_compute->module;

   struct zink_shader_module *zm = NULL;
   zink_shader_module_reference(zink_screen(ctx->base.screen), &zm, comp->module);
   ctx->dirty_shader_stages &= ~(1 << PIPE_SHADER_COMPUTE);

   comp->pipelines = _mesa_hash_table_create(NULL, hash_compute_pipeline_state,
                                             equals_compute_pipeline_state);

   _mesa_set_add(shader->programs, comp);
   zink_compute_program_reference(screen, NULL, comp);
   comp->shader = shader;

   struct zink_shader *stages[ZINK_SHADER_COUNT] = {};
   stages[0] = shader;
   comp->base.dsl = create_desc_set_layout(screen->dev, stages,
                                      &comp->base.num_descriptors, &comp->base.descpool);
   if (comp->base.num_descriptors && (!comp->base.dsl || !comp->base.descpool))
      goto fail;

   comp->layout = create_compute_pipeline_layout(screen->dev, comp->base.dsl);
   if (!comp->layout)
      goto fail;

   util_dynarray_init(&comp->base.alloc_desc_sets, NULL);

   return comp;

fail:
   if (comp)
      zink_destroy_compute_program(screen, comp);
   return NULL;
}

struct zink_descriptor_set *
zink_program_allocate_desc_set(struct zink_screen *screen,
                               struct zink_batch *batch,
                               struct zink_program *pg)
{
   struct zink_descriptor_set *zds;

   if (util_dynarray_num_elements(&pg->alloc_desc_sets, struct zink_descriptor_set *)) {
      /* grab one off the allocated array */
      zds = util_dynarray_pop(&pg->alloc_desc_sets, struct zink_descriptor_set *);
      goto out;
   }

   VkDescriptorSetAllocateInfo dsai;
   memset((void *)&dsai, 0, sizeof(dsai));
   dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   dsai.pNext = NULL;
   dsai.descriptorPool = pg->descpool;
   dsai.descriptorSetCount = 1;
   dsai.pSetLayouts = &pg->dsl;

   VkDescriptorSet desc_set;
   if (vkAllocateDescriptorSets(screen->dev, &dsai, &desc_set) != VK_SUCCESS) {
      debug_printf("ZINK: %p failed to allocate descriptor set :/\n", pg);
      return VK_NULL_HANDLE;
   }
   zds = ralloc_size(NULL, sizeof(struct zink_descriptor_set));
   assert(zds);
   pipe_reference_init(&zds->reference, 1);
   zds->desc_set = desc_set;
out:
   if (zink_batch_add_desc_set(batch, pg, zds))
      batch->descs_used += pg->num_descriptors;

   return zds;
}

void
zink_program_invalidate_desc_set(struct zink_program *pg, struct zink_descriptor_set *zds)
{
   uint32_t refcount = p_atomic_read(&zds->reference.count);
   /* refcount > 1 means this is currently in use, so we can't recycle it yet */
   if (refcount == 1)
      util_dynarray_append(&pg->alloc_desc_sets, struct zink_descriptor_set *, zds);
}

static void
gfx_program_remove_shader(struct zink_gfx_program *prog, struct zink_shader *shader)
{
   enum pipe_shader_type p_stage = pipe_shader_type_from_mesa(shader->nir->info.stage);

   assert(prog->shaders[p_stage] == shader);
   prog->shaders[p_stage] = NULL;
   _mesa_set_remove_key(shader->programs, prog);
}

void
zink_destroy_gfx_program(struct zink_screen *screen,
                         struct zink_gfx_program *prog)
{
   if (prog->layout)
      vkDestroyPipelineLayout(screen->dev, prog->layout, NULL);

   if (prog->base.dsl)
      vkDestroyDescriptorSetLayout(screen->dev, prog->base.dsl, NULL);

   for (int i = 0; i < ZINK_SHADER_COUNT; ++i) {
      if (prog->shaders[i])
         gfx_program_remove_shader(prog, prog->shaders[i]);
      if (prog->modules[i])
         zink_shader_module_reference(screen, &prog->modules[i], NULL);
   }

   for (int i = 0; i < ARRAY_SIZE(prog->pipelines); ++i) {
      hash_table_foreach(prog->pipelines[i], entry) {
         struct gfx_pipeline_cache_entry *pc_entry = entry->data;

         vkDestroyPipeline(screen->dev, pc_entry->pipeline, NULL);
         free(pc_entry);
      }
      _mesa_hash_table_destroy(prog->pipelines[i], NULL);
   }
   zink_shader_cache_reference(screen, &prog->shader_cache, NULL);

   if (prog->base.descpool)
      vkDestroyDescriptorPool(screen->dev, prog->base.descpool, NULL);

   util_dynarray_fini(&prog->base.alloc_desc_sets);

   ralloc_free(prog);
}

void
zink_destroy_compute_program(struct zink_screen *screen,
                         struct zink_compute_program *comp)
{
   if (comp->layout)
      vkDestroyPipelineLayout(screen->dev, comp->layout, NULL);

   if (comp->base.dsl)
      vkDestroyDescriptorSetLayout(screen->dev, comp->base.dsl, NULL);

   if (comp->shader)
      _mesa_set_remove_key(comp->shader->programs, comp);
   if (comp->module)
      zink_shader_module_reference(screen, &comp->module, NULL);

   hash_table_foreach(comp->pipelines, entry) {
      struct compute_pipeline_cache_entry *pc_entry = entry->data;

      vkDestroyPipeline(screen->dev, pc_entry->pipeline, NULL);
      free(pc_entry);
   }
   _mesa_hash_table_destroy(comp->pipelines, NULL);
   zink_shader_cache_reference(screen, &comp->shader_cache, NULL);

   if (comp->base.descpool)
      vkDestroyDescriptorPool(screen->dev, comp->base.descpool, NULL);

   util_dynarray_fini(&comp->base.alloc_desc_sets);

   ralloc_free(comp);
}

static VkPrimitiveTopology
primitive_topology(enum pipe_prim_type mode)
{
   switch (mode) {
   case PIPE_PRIM_POINTS:
      return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

   case PIPE_PRIM_LINES:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

   case PIPE_PRIM_LINE_STRIP:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;

   case PIPE_PRIM_TRIANGLES:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

   case PIPE_PRIM_TRIANGLE_STRIP:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

   case PIPE_PRIM_TRIANGLE_FAN:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;

   case PIPE_PRIM_LINE_STRIP_ADJACENCY:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;

   case PIPE_PRIM_LINES_ADJACENCY:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;

   case PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;

   case PIPE_PRIM_TRIANGLES_ADJACENCY:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;

   case PIPE_PRIM_PATCHES:
      return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;

   default:
      unreachable("unexpected enum pipe_prim_type");
   }
}

VkPipeline
zink_get_gfx_pipeline(struct zink_screen *screen,
                      struct zink_gfx_program *prog,
                      struct zink_gfx_pipeline_state *state,
                      enum pipe_prim_type mode)
{
   VkPrimitiveTopology vkmode = primitive_topology(mode);
   assert(vkmode <= ARRAY_SIZE(prog->pipelines));

   struct hash_entry *entry = NULL;
   
   if (state->dirty) {
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++)
         state->modules[i] = prog->modules[i] ? prog->modules[i]->shader : VK_NULL_HANDLE;

      state->hash = hash_gfx_pipeline_state(state);
      state->dirty = false;
   }
   entry = _mesa_hash_table_search_pre_hashed(prog->pipelines[vkmode], state->hash, state);

   if (!entry) {
      VkPipeline pipeline = zink_create_gfx_pipeline(screen, prog,
                                                     state, vkmode);
      if (pipeline == VK_NULL_HANDLE)
         return VK_NULL_HANDLE;

      struct gfx_pipeline_cache_entry *pc_entry = CALLOC_STRUCT(gfx_pipeline_cache_entry);
      if (!pc_entry)
         return VK_NULL_HANDLE;

      memcpy(&pc_entry->state, state, sizeof(*state));
      pc_entry->pipeline = pipeline;

      entry = _mesa_hash_table_insert_pre_hashed(prog->pipelines[vkmode], state->hash, state, pc_entry);
      assert(entry);
   }

   return ((struct gfx_pipeline_cache_entry *)(entry->data))->pipeline;
}

VkPipeline
zink_get_compute_pipeline(struct zink_screen *screen,
                      struct zink_compute_program *comp,
                      struct zink_compute_pipeline_state *state)
{
   struct hash_entry *entry = NULL;

   if (state->dirty) {
      state->hash = hash_compute_pipeline_state(state);
      state->dirty = false;
   }
   entry = _mesa_hash_table_search_pre_hashed(comp->pipelines, state->hash, state);

   if (!entry) {
      VkPipeline pipeline = zink_create_compute_pipeline(screen, comp, state);

      if (pipeline == VK_NULL_HANDLE)
         return VK_NULL_HANDLE;

      struct compute_pipeline_cache_entry *pc_entry = CALLOC_STRUCT(compute_pipeline_cache_entry);
      if (!pc_entry)
         return VK_NULL_HANDLE;

      memcpy(&pc_entry->state, state, sizeof(*state));
      pc_entry->pipeline = pipeline;

      entry = _mesa_hash_table_insert_pre_hashed(comp->pipelines, state->hash, state, pc_entry);
      assert(entry);
   }

   return ((struct compute_pipeline_cache_entry *)(entry->data))->pipeline;
}


static void *
zink_create_vs_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *shader)
{
   struct nir_shader *nir;
   if (shader->type != PIPE_SHADER_IR_NIR)
      nir = zink_tgsi_to_nir(pctx->screen, shader->tokens);
   else
      nir = (struct nir_shader *)shader->ir.nir;

   return zink_shader_create(zink_screen(pctx->screen), nir, &shader->stream_output);
}

static void
bind_stage(struct zink_context *ctx, enum pipe_shader_type stage,
           struct zink_shader *shader)
{
   if (stage == PIPE_SHADER_COMPUTE)
      ctx->compute_stage = shader;
   else
      ctx->gfx_stages[stage] = shader;
   ctx->dirty_shader_stages |= 1 << stage;
}

static void
zink_bind_vs_state(struct pipe_context *pctx,
                   void *cso)
{
   bind_stage(zink_context(pctx), PIPE_SHADER_VERTEX, cso);
}

static void *
zink_create_fs_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *shader)
{
   struct nir_shader *nir;
   if (shader->type != PIPE_SHADER_IR_NIR)
      nir = zink_tgsi_to_nir(pctx->screen, shader->tokens);
   else
      nir = (struct nir_shader *)shader->ir.nir;

   return zink_shader_create(zink_screen(pctx->screen), nir, NULL);
}

static void
zink_bind_fs_state(struct pipe_context *pctx,
                   void *cso)
{
   bind_stage(zink_context(pctx), PIPE_SHADER_FRAGMENT, cso);
}

static void *
zink_create_gs_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *shader)
{
   struct nir_shader *nir;
   if (shader->type != PIPE_SHADER_IR_NIR)
      nir = zink_tgsi_to_nir(pctx->screen, shader->tokens);
   else
      nir = (struct nir_shader *)shader->ir.nir;

   return zink_shader_create(zink_screen(pctx->screen), nir, &shader->stream_output);
}

static void
zink_bind_gs_state(struct pipe_context *pctx,
                   void *cso)
{
   struct zink_context *ctx = zink_context(pctx);
   if (!!ctx->gfx_stages[PIPE_SHADER_GEOMETRY] != !!cso)
      ctx->dirty_shader_stages |= BITFIELD_BIT(PIPE_SHADER_VERTEX) |
                                  BITFIELD_BIT(PIPE_SHADER_TESS_EVAL);
   bind_stage(ctx, PIPE_SHADER_GEOMETRY, cso);
}

static void *
zink_create_tcs_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *shader)
{
   struct nir_shader *nir;
   if (shader->type != PIPE_SHADER_IR_NIR)
      nir = zink_tgsi_to_nir(pctx->screen, shader->tokens);
   else
      nir = (struct nir_shader *)shader->ir.nir;

   return zink_shader_create(zink_screen(pctx->screen), nir, &shader->stream_output);
}

static void
zink_bind_tcs_state(struct pipe_context *pctx,
                   void *cso)
{
   bind_stage(zink_context(pctx), PIPE_SHADER_TESS_CTRL, cso);
}

static void *
zink_create_tes_state(struct pipe_context *pctx,
                     const struct pipe_shader_state *shader)
{
   struct nir_shader *nir;
   if (shader->type != PIPE_SHADER_IR_NIR)
      nir = zink_tgsi_to_nir(pctx->screen, shader->tokens);
   else
      nir = (struct nir_shader *)shader->ir.nir;

   return zink_shader_create(zink_screen(pctx->screen), nir, &shader->stream_output);
}

static void
zink_bind_tes_state(struct pipe_context *pctx,
                   void *cso)
{
   struct zink_context *ctx = zink_context(pctx);
   if (!!ctx->gfx_stages[PIPE_SHADER_TESS_EVAL] != !!cso) {
      if (!cso) {
         /* if unsetting a TESS that uses a generated TCS, ensure the TCS is unset */
         if (ctx->gfx_stages[PIPE_SHADER_TESS_EVAL]->generated)
            ctx->gfx_stages[PIPE_SHADER_TESS_CTRL] = NULL;
      }
      ctx->dirty_shader_stages |= BITFIELD_BIT(PIPE_SHADER_VERTEX);
   }
   bind_stage(ctx, PIPE_SHADER_TESS_EVAL, cso);
}

static void
zink_delete_shader_state(struct pipe_context *pctx, void *cso)
{
   zink_shader_free(zink_context(pctx), cso);
}

static void *
zink_create_cs_state(struct pipe_context *pctx,
                     const struct pipe_compute_state *shader)
{
   struct nir_shader *nir;
   if (shader->ir_type != PIPE_SHADER_IR_NIR)
      nir = zink_tgsi_to_nir(pctx->screen, shader->prog);
   else
      nir = (struct nir_shader *)shader->prog;

   return zink_shader_create(zink_screen(pctx->screen), nir, NULL);
}

static void
zink_bind_cs_state(struct pipe_context *pctx,
                   void *cso)
{
   bind_stage(zink_context(pctx), PIPE_SHADER_COMPUTE, cso);
}

void
zink_program_init(struct zink_context *ctx)
{
   ctx->base.create_vs_state = zink_create_vs_state;
   ctx->base.bind_vs_state = zink_bind_vs_state;
   ctx->base.delete_vs_state = zink_delete_shader_state;

   ctx->base.create_fs_state = zink_create_fs_state;
   ctx->base.bind_fs_state = zink_bind_fs_state;
   ctx->base.delete_fs_state = zink_delete_shader_state;

   ctx->base.create_gs_state = zink_create_gs_state;
   ctx->base.bind_gs_state = zink_bind_gs_state;
   ctx->base.delete_gs_state = zink_delete_shader_state;

   ctx->base.create_tcs_state = zink_create_tcs_state;
   ctx->base.bind_tcs_state = zink_bind_tcs_state;
   ctx->base.delete_tcs_state = zink_delete_shader_state;

   ctx->base.create_tes_state = zink_create_tes_state;
   ctx->base.bind_tes_state = zink_bind_tes_state;
   ctx->base.delete_tes_state = zink_delete_shader_state;

   ctx->base.create_compute_state = zink_create_cs_state;
   ctx->base.bind_compute_state = zink_bind_cs_state;
   ctx->base.delete_compute_state = zink_delete_shader_state;
}

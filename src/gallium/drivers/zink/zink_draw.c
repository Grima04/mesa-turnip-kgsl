#include "zink_compiler.h"
#include "zink_context.h"
#include "zink_program.h"
#include "zink_query.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_state.h"
#include "zink_surface.h"

#include "indices/u_primconvert.h"
#include "tgsi/tgsi_from_mesa.h"
#include "util/hash_table.h"
#include "util/u_debug.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_prim.h"
#include "util/u_prim_restart.h"

static VkDescriptorSet
allocate_descriptor_set(struct zink_screen *screen,
                        struct zink_batch *batch,
                        VkDescriptorSetLayout dsl,
                        unsigned num_descriptors)
{
   assert(batch->descs_left >= num_descriptors);
   VkDescriptorSetAllocateInfo dsai;
   memset((void *)&dsai, 0, sizeof(dsai));
   dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   dsai.pNext = NULL;
   dsai.descriptorPool = batch->descpool;
   dsai.descriptorSetCount = 1;
   dsai.pSetLayouts = &dsl;

   VkDescriptorSet desc_set;
   if (vkAllocateDescriptorSets(screen->dev, &dsai, &desc_set) != VK_SUCCESS) {
      debug_printf("ZINK: failed to allocate descriptor set :/");
      return VK_NULL_HANDLE;
   }

   batch->descs_left -= num_descriptors;
   return desc_set;
}

static void
zink_emit_xfb_counter_barrier(struct zink_context *ctx)
{
   /* Between the pause and resume there needs to be a memory barrier for the counter buffers
    * with a source access of VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT
    * at pipeline stage VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT
    * to a destination access of VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT
    * at pipeline stage VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT.
    *
    * - from VK_EXT_transform_feedback spec
    */
   struct zink_batch *batch = zink_batch_no_rp(ctx);
   for (unsigned i = 0; i < ctx->num_so_targets; i++) {
      struct zink_so_target *t = zink_so_target(ctx->so_targets[i]);
      if (!t)
         continue;
      struct zink_resource *res = zink_resource(t->counter_buffer);
      if (t->counter_buffer_valid)
          zink_resource_buffer_barrier(batch, res, VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT,
                                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
      else
          zink_resource_buffer_barrier(batch, res, VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
                                       VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT);
   }
   ctx->xfb_barrier = false;
}

static void
zink_emit_xfb_vertex_input_barrier(struct zink_context *ctx, struct zink_resource *res)
{
   /* A pipeline barrier is required between using the buffers as
    * transform feedback buffers and vertex buffers to
    * ensure all writes to the transform feedback buffers are visible
    * when the data is read as vertex attributes.
    * The source access is VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT
    * and the destination access is VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
    * for the pipeline stages VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT
    * and VK_PIPELINE_STAGE_VERTEX_INPUT_BIT respectively.
    *
    * - 20.3.1. Drawing Transform Feedback
    */
   struct zink_batch *batch = zink_batch_no_rp(ctx);
   zink_resource_buffer_barrier(batch, res, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
}

static void
zink_emit_stream_output_targets(struct pipe_context *pctx)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_batch *batch = zink_curr_batch(ctx);
   VkBuffer buffers[PIPE_MAX_SO_OUTPUTS] = {};
   VkDeviceSize buffer_offsets[PIPE_MAX_SO_OUTPUTS] = {};
   VkDeviceSize buffer_sizes[PIPE_MAX_SO_OUTPUTS] = {};

   for (unsigned i = 0; i < ctx->num_so_targets; i++) {
      struct zink_so_target *t = (struct zink_so_target *)ctx->so_targets[i];
      if (!t) {
         /* no need to reference this or anything */
         buffers[i] = zink_resource(ctx->dummy_xfb_buffer)->buffer;
         buffer_offsets[i] = 0;
         buffer_sizes[i] = sizeof(uint8_t);
         continue;
      }
      buffers[i] = zink_resource(t->base.buffer)->buffer;
      if (zink_resource_buffer_needs_barrier(zink_resource(t->base.buffer),
                                             VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                                             VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT)) {
         batch = zink_batch_no_rp(ctx);
         zink_resource_buffer_barrier(batch, zink_resource(t->base.buffer),
                                      VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT);
      }
      zink_batch_reference_resource_rw(batch, zink_resource(t->base.buffer), true);
      buffer_offsets[i] = t->base.buffer_offset;
      buffer_sizes[i] = t->base.buffer_size;
   }

   screen->vk_CmdBindTransformFeedbackBuffersEXT(batch->cmdbuf, 0, ctx->num_so_targets,
                                                 buffers, buffer_offsets,
                                                 buffer_sizes);
   ctx->dirty_so_targets = false;
}

static void
barrier_vertex_buffers(struct zink_context *ctx)
{
   const struct zink_vertex_elements_state *elems = ctx->element_state;
   for (unsigned i = 0; i < elems->hw_state.num_bindings; i++) {
      struct pipe_vertex_buffer *vb = ctx->buffers + ctx->element_state->binding_map[i];
      assert(vb);
      if (vb->buffer.resource) {
         struct zink_resource *res = zink_resource(vb->buffer.resource);
         if (zink_resource_buffer_needs_barrier(res, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                                                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT)) {
            struct zink_batch *batch = zink_batch_no_rp(ctx);
            zink_resource_buffer_barrier(batch, res, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
         }
      }
   }
}

static void
check_buffer_barrier(struct zink_context *ctx, struct pipe_resource *pres, VkAccessFlags flags, VkPipelineStageFlags pipeline)
{
   struct zink_resource *res = zink_resource(pres);
   if (zink_resource_buffer_needs_barrier(res, flags, pipeline)) {
       struct zink_batch *batch = zink_batch_no_rp(ctx);
       zink_resource_buffer_barrier(batch, res, flags, pipeline);
    }
}

static void
barrier_draw_buffers(struct zink_context *ctx, const struct pipe_draw_info *dinfo,
                     const struct pipe_draw_indirect_info *dindirect, struct pipe_resource *index_buffer)
{
   if (index_buffer)
      check_buffer_barrier(ctx, index_buffer, VK_ACCESS_INDEX_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
   if (dindirect && dindirect->buffer) {
      check_buffer_barrier(ctx, dindirect->buffer,
                           VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
      if (dindirect->indirect_draw_count)
         check_buffer_barrier(ctx, dindirect->indirect_draw_count,
                              VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
   }
}

static void
zink_bind_vertex_buffers(struct zink_batch *batch, struct zink_context *ctx)
{
   VkBuffer buffers[PIPE_MAX_ATTRIBS];
   VkDeviceSize buffer_offsets[PIPE_MAX_ATTRIBS];
   const struct zink_vertex_elements_state *elems = ctx->element_state;
   for (unsigned i = 0; i < elems->hw_state.num_bindings; i++) {
      struct pipe_vertex_buffer *vb = ctx->buffers + ctx->element_state->binding_map[i];
      assert(vb);
      if (vb->buffer.resource) {
         struct zink_resource *res = zink_resource(vb->buffer.resource);
         buffers[i] = res->buffer;
         buffer_offsets[i] = vb->buffer_offset;
         zink_batch_reference_resource_rw(batch, res, false);
      } else {
         buffers[i] = zink_resource(ctx->dummy_vertex_buffer)->buffer;
         buffer_offsets[i] = 0;
      }
   }

   if (elems->hw_state.num_bindings > 0)
      vkCmdBindVertexBuffers(batch->cmdbuf, 0,
                             elems->hw_state.num_bindings,
                             buffers, buffer_offsets);
}

static struct zink_compute_program *
get_compute_program(struct zink_context *ctx)
{
   if (ctx->dirty_shader_stages) {
      struct hash_entry *entry = _mesa_hash_table_search(ctx->compute_program_cache,
                                                         &ctx->compute_stage->shader_id);
      if (!entry) {
         struct zink_compute_program *comp;
         comp = zink_create_compute_program(ctx, ctx->compute_stage);
         entry = _mesa_hash_table_insert(ctx->compute_program_cache, &comp->shader->shader_id, comp);
         if (!entry)
            return NULL;
      }
      if (entry->data != ctx->curr_compute)
         ctx->compute_pipeline_state.dirty = true;
      ctx->curr_compute = entry->data;
      ctx->dirty_shader_stages &= (1 << PIPE_SHADER_COMPUTE);
   }

   assert(ctx->curr_compute);
   return ctx->curr_compute;
}

static struct zink_gfx_program *
get_gfx_program(struct zink_context *ctx)
{
   if (ctx->last_vertex_stage_dirty) {
      if (ctx->gfx_stages[PIPE_SHADER_GEOMETRY])
         ctx->dirty_shader_stages |= BITFIELD_BIT(PIPE_SHADER_GEOMETRY);
      else if (ctx->gfx_stages[PIPE_SHADER_TESS_EVAL])
         ctx->dirty_shader_stages |= BITFIELD_BIT(PIPE_SHADER_TESS_EVAL);
      else
         ctx->dirty_shader_stages |= BITFIELD_BIT(PIPE_SHADER_VERTEX);
   }
   if (ctx->dirty_shader_stages) {
      struct hash_entry *entry = _mesa_hash_table_search(ctx->program_cache,
                                                         ctx->gfx_stages);
      if (entry)
         zink_update_gfx_program(ctx, entry->data);
      else {
         struct zink_gfx_program *prog;
         prog = zink_create_gfx_program(ctx, ctx->gfx_stages);
         entry = _mesa_hash_table_insert(ctx->program_cache, prog->shaders, prog);
         if (!entry)
            return NULL;
      }
      ctx->curr_program = entry->data;
      unsigned bits = u_bit_consecutive(PIPE_SHADER_VERTEX, 5);
      ctx->dirty_shader_stages &= ~bits;
   }

   assert(ctx->curr_program);
   return ctx->curr_program;
}

struct zink_transition {
   struct zink_resource *res;
   VkImageLayout layout;
   VkAccessFlags access;
   VkPipelineStageFlagBits stage;
};

#define MAX_DESCRIPTORS (PIPE_SHADER_TYPES * (PIPE_MAX_CONSTANT_BUFFERS + PIPE_MAX_SAMPLERS + PIPE_MAX_SHADER_BUFFERS + PIPE_MAX_SHADER_IMAGES))

static bool
transition_equals(const void *a, const void *b)
{
   const struct zink_transition *t1 = a, *t2 = b;
   if (t1->res != t2->res)
      return false;
   if ((t1->access & t2->access) != t2->access)
      return false;
   if (t1->layout != t2->layout)
      return false;
   return true;
}

static uint32_t
transition_hash(const void *key)
{
   return _mesa_hash_data(key, offsetof(struct zink_transition, stage));
}

static inline void
add_transition(struct zink_resource *res, VkImageLayout layout, VkAccessFlags flags, enum pipe_shader_type stage, struct zink_transition *t, int *num_transitions, struct set *ht)
{
   VkPipelineStageFlags pipeline = zink_pipeline_flags_from_stage(zink_shader_stage(stage));
   struct zink_transition key = {res, layout, flags, 0};

   uint32_t hash = transition_hash(&key);
   struct set_entry *entry = _mesa_set_search_pre_hashed(ht, hash, &key);
   if (entry)
      t = (struct zink_transition*)entry->key;
   else {
      (*num_transitions)++;
      t->stage = 0;
      t->layout = layout;
      t->res = res;
      t->access = flags;
      _mesa_set_add_pre_hashed(ht, hash, t);
   }
   t->stage |= pipeline;
}

static void
update_descriptors(struct zink_context *ctx, struct zink_screen *screen, bool is_compute)
{
   VkWriteDescriptorSet wds[MAX_DESCRIPTORS];
   struct zink_resource *read_desc_resources[MAX_DESCRIPTORS] = {};
   struct zink_resource *write_desc_resources[MAX_DESCRIPTORS] = {};
   struct zink_surface *surface_refs[PIPE_SHADER_TYPES * PIPE_MAX_SHADER_IMAGES] = {};
   VkDescriptorBufferInfo buffer_infos[PIPE_SHADER_TYPES * (PIPE_MAX_CONSTANT_BUFFERS + PIPE_MAX_SHADER_BUFFERS + PIPE_MAX_SHADER_IMAGES)];
   VkDescriptorImageInfo image_infos[PIPE_SHADER_TYPES * (PIPE_MAX_SAMPLERS + PIPE_MAX_SHADER_IMAGES)];
   VkBufferView buffer_view[] = {VK_NULL_HANDLE};
   unsigned num_wds = 0, num_buffer_info = 0, num_image_info = 0, num_surface_refs = 0;
   struct zink_shader **stages;

   unsigned num_stages = is_compute ? 1 : ZINK_SHADER_COUNT;
   if (is_compute)
      stages = &ctx->curr_compute->shader;
   else
      stages = &ctx->gfx_stages[0];

   struct zink_transition transitions[MAX_DESCRIPTORS];
   int num_transitions = 0;
   struct set *ht = _mesa_set_create(NULL, transition_hash, transition_equals);

   for (int i = 0; i < num_stages; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;
      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);
      if (ctx->num_so_targets &&
          (stage == PIPE_SHADER_GEOMETRY ||
          (stage == PIPE_SHADER_TESS_EVAL && !ctx->gfx_stages[PIPE_SHADER_GEOMETRY]) ||
          (stage == PIPE_SHADER_VERTEX && !ctx->gfx_stages[PIPE_SHADER_GEOMETRY] && !ctx->gfx_stages[PIPE_SHADER_TESS_EVAL]))) {
         for (unsigned j = 0; j < ctx->num_so_targets; j++) {
            struct zink_so_target *t = zink_so_target(ctx->so_targets[j]);
            if (t)
               t->stride = shader->streamout.so_info.stride[j] * sizeof(uint32_t);
         }
      }

      for (int j = 0; j < shader->num_bindings; j++) {
         int index = shader->bindings[j].index;
         if (shader->bindings[j].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            assert(ctx->ubos[stage][index].buffer_size <= screen->info.props.limits.maxUniformBufferRange);
            struct zink_resource *res = zink_resource(ctx->ubos[stage][index].buffer);
            assert(!res || ctx->ubos[stage][index].buffer_size > 0);
            assert(!res || ctx->ubos[stage][index].buffer);
            read_desc_resources[num_wds] = res;
            buffer_infos[num_buffer_info].buffer = res ? res->buffer :
                                                   (screen->info.rb2_feats.nullDescriptor ?
                                                    VK_NULL_HANDLE :
                                                    zink_resource(ctx->dummy_vertex_buffer)->buffer);
            buffer_infos[num_buffer_info].offset = res ? ctx->ubos[stage][index].buffer_offset : 0;
            buffer_infos[num_buffer_info].range  = res ? ctx->ubos[stage][index].buffer_size : VK_WHOLE_SIZE;
            if (res)
               add_transition(res, 0, VK_ACCESS_UNIFORM_READ_BIT, stage, &transitions[num_transitions], &num_transitions, ht);
            wds[num_wds].pBufferInfo = buffer_infos + num_buffer_info;
            ++num_buffer_info;
         } else if (shader->bindings[j].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            struct zink_resource *res = zink_resource(ctx->ssbos[stage][index].buffer);
            if (res) {
               assert(ctx->ssbos[stage][index].buffer_size > 0);
               assert(ctx->ssbos[stage][index].buffer_size <= screen->info.props.limits.maxStorageBufferRange);
               unsigned flag = VK_ACCESS_SHADER_READ_BIT;
               if (ctx->writable_ssbos[stage] & (1 << index)) {
                  write_desc_resources[num_wds] = res;
                  flag |= VK_ACCESS_SHADER_WRITE_BIT;
               } else {
                  read_desc_resources[num_wds] = res;
               }
               add_transition(res, 0, flag, stage, &transitions[num_transitions], &num_transitions, ht);
               buffer_infos[num_buffer_info].buffer = res->buffer;
               buffer_infos[num_buffer_info].offset = ctx->ssbos[stage][index].buffer_offset;
               buffer_infos[num_buffer_info].range  = ctx->ssbos[stage][index].buffer_size;
            } else {
               assert(screen->info.rb2_feats.nullDescriptor);
               buffer_infos[num_buffer_info].buffer = VK_NULL_HANDLE;
               buffer_infos[num_buffer_info].offset = 0;
               buffer_infos[num_buffer_info].range  = VK_WHOLE_SIZE;
            }
            wds[num_wds].pBufferInfo = buffer_infos + num_buffer_info;
            ++num_buffer_info;
         } else {
            for (unsigned k = 0; k < shader->bindings[j].size; k++) {
               VkImageView imageview = VK_NULL_HANDLE;
               struct zink_resource *res = NULL;
               VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
               VkSampler sampler = VK_NULL_HANDLE;

               switch (shader->bindings[j].type) {
               case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
               /* fallthrough */
               case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
                  struct pipe_sampler_view *psampler_view = ctx->sampler_views[stage][index + k];
                  struct zink_sampler_view *sampler_view = zink_sampler_view(psampler_view);
                  res = psampler_view ? zink_resource(psampler_view->texture) : NULL;
                  if (!res)
                     break;
                  if (res->base.target == PIPE_BUFFER)
                     wds[num_wds].pTexelBufferView = &sampler_view->buffer_view;
                  else {
                     imageview = sampler_view->image_view;
                     layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                     sampler = ctx->samplers[stage][index + k];
                  }
                  add_transition(res, layout, VK_ACCESS_SHADER_READ_BIT, stage, &transitions[num_transitions], &num_transitions, ht);
                  read_desc_resources[num_wds] = res;
               }
               break;
               case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
               /* fallthrough */
               case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
                  struct zink_image_view *image_view = &ctx->image_views[stage][index + k];
                  assert(image_view);
                  surface_refs[num_surface_refs++] = image_view->surface;
                  res = zink_resource(image_view->base.resource);
                  if (!res)
                     break;
                  if (image_view->base.resource->target == PIPE_BUFFER) {
                     wds[num_wds].pTexelBufferView = &image_view->buffer_view;
                  } else {
                     imageview = image_view->surface->image_view;
                     layout = VK_IMAGE_LAYOUT_GENERAL;
                  }
                  VkAccessFlags flags = 0;
                  if (image_view->base.access & PIPE_IMAGE_ACCESS_READ)
                     flags |= VK_ACCESS_SHADER_READ_BIT;
                  if (image_view->base.access & PIPE_IMAGE_ACCESS_WRITE)
                     flags |= VK_ACCESS_SHADER_WRITE_BIT;
                  add_transition(res, layout, flags, stage, &transitions[num_transitions], &num_transitions, ht);
                  if (image_view->base.access & PIPE_IMAGE_ACCESS_WRITE)
                     write_desc_resources[num_wds] = res;
                  else
                     read_desc_resources[num_wds] = res;
               }
               break;
               default:
                  unreachable("unknown descriptor type");
               }

               if (!res) {
                  /* if we're hitting this assert often, we can probably just throw a junk buffer in since
                   * the results of this codepath are undefined in ARB_texture_buffer_object spec
                   */
                  assert(screen->info.rb2_feats.nullDescriptor);
                  read_desc_resources[num_wds] = res;
                  switch (shader->bindings[j].type) {
                  case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                  case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                     wds[num_wds].pTexelBufferView = &buffer_view[0];
                     break;
                  case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                  case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                     image_infos[num_image_info].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                     image_infos[num_image_info].imageView = VK_NULL_HANDLE;
                     image_infos[num_image_info].sampler = sampler;
                     if (!k)
                        wds[num_wds].pImageInfo = image_infos + num_image_info;
                     ++num_image_info;
                     break;
                  default:
                     unreachable("unknown descriptor type");
                  }
               } else if (res->base.target != PIPE_BUFFER) {
                  assert(layout != VK_IMAGE_LAYOUT_UNDEFINED);
                  image_infos[num_image_info].imageLayout = layout;
                  image_infos[num_image_info].imageView = imageview;
                  image_infos[num_image_info].sampler = ctx->samplers[stage][index + k];
                  if (!k)
                     wds[num_wds].pImageInfo = image_infos + num_image_info;
                  ++num_image_info;
               }
            }
         }

         wds[num_wds].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
         wds[num_wds].pNext = NULL;
         wds[num_wds].dstBinding = shader->bindings[j].binding;
         wds[num_wds].dstArrayElement = 0;
         wds[num_wds].descriptorCount = shader->bindings[j].size;
         wds[num_wds].descriptorType = shader->bindings[j].type;
         ++num_wds;
      }
   }
   _mesa_set_destroy(ht, NULL);

   struct zink_batch *batch = NULL;
   if (num_transitions > 0) {
      for (int i = 0; i < num_transitions; ++i) {
         if (!zink_resource_needs_barrier(transitions[i].res,
                                                   transitions[i].layout,
                                                   transitions[i].access,
                                                   transitions[i].stage))
            continue;
         if (is_compute)
            batch = &ctx->compute_batch;
         else
            batch = zink_batch_no_rp(ctx);

         zink_resource_barrier(batch, transitions[i].res,
                               transitions[i].layout, transitions[i].access, transitions[i].stage);
      }
   }

   unsigned num_descriptors;
   VkDescriptorSetLayout dsl;
   if (is_compute) {
      num_descriptors = ctx->curr_compute->num_descriptors;
      dsl = ctx->curr_compute->dsl;
      batch = &ctx->compute_batch;
   } else {
      batch = zink_batch_rp(ctx);
      num_descriptors = ctx->curr_program->num_descriptors;
      dsl = ctx->curr_program->dsl;
   }

   if (batch->descs_left < num_descriptors) {
      if (is_compute)
         zink_wait_on_batch(ctx, ZINK_COMPUTE_BATCH_ID);
      else {
         ctx->base.flush(&ctx->base, NULL, 0);
         batch = zink_batch_rp(ctx);
      }
      assert(batch->descs_left >= num_descriptors);
   }
   if (is_compute)
      zink_batch_reference_program(batch, &ctx->curr_compute->reference);
   else
      zink_batch_reference_program(batch, &ctx->curr_program->reference);

   VkDescriptorSet desc_set = allocate_descriptor_set(screen, batch,
                                                      dsl, num_descriptors);
   assert(desc_set != VK_NULL_HANDLE);

   for (int i = 0; i < num_stages; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;
      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);

      for (int j = 0; j < shader->num_bindings; j++) {
         int index = shader->bindings[j].index;
         if (shader->bindings[j].type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            struct zink_sampler_view *sampler_view = zink_sampler_view(ctx->sampler_views[stage][index]);
            if (sampler_view)
               zink_batch_reference_sampler_view(batch, sampler_view);
         }
      }
   }

   unsigned check_flush_id = is_compute ? 0 : ZINK_COMPUTE_BATCH_ID;
   bool need_flush = false;
   if (num_wds > 0) {
      for (int i = 0; i < num_wds; ++i) {
         wds[i].dstSet = desc_set;
         struct zink_resource *res = read_desc_resources[i] ? read_desc_resources[i] : write_desc_resources[i];
         if (res) {
            need_flush |= zink_batch_reference_resource_rw(batch, res, res == write_desc_resources[i]) == check_flush_id;
         }
      }
      vkUpdateDescriptorSets(screen->dev, num_wds, wds, 0, NULL);
      for (int i = 0; i < num_surface_refs; i++) {
         if (surface_refs[i])
            zink_batch_reference_surface(batch, surface_refs[i]);
      }
   }

   if (is_compute)
      vkCmdBindDescriptorSets(batch->cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                              ctx->curr_compute->layout, 0, 1, &desc_set, 0, NULL);
   else
      vkCmdBindDescriptorSets(batch->cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              ctx->curr_program->layout, 0, 1, &desc_set, 0, NULL);
   if (!need_flush)
      return;

   if (is_compute)
      /* flush gfx batch */
      ctx->base.flush(&ctx->base, NULL, PIPE_FLUSH_HINT_FINISH);
   else {
      /* flush compute batch */
      zink_flush_compute(ctx);
   }
}

static bool
line_width_needed(enum pipe_prim_type reduced_prim,
                  VkPolygonMode polygon_mode)
{
   switch (reduced_prim) {
   case PIPE_PRIM_POINTS:
      return false;

   case PIPE_PRIM_LINES:
      return true;

   case PIPE_PRIM_TRIANGLES:
      return polygon_mode == VK_POLYGON_MODE_LINE;

   default:
      unreachable("unexpected reduced prim");
   }
}

static inline bool
restart_supported(enum pipe_prim_type mode)
{
    return mode == PIPE_PRIM_LINE_STRIP || mode == PIPE_PRIM_TRIANGLE_STRIP || mode == PIPE_PRIM_TRIANGLE_FAN;
}


void
zink_draw_vbo(struct pipe_context *pctx,
              const struct pipe_draw_info *dinfo,
              const struct pipe_draw_indirect_info *dindirect,
              const struct pipe_draw_start_count *draws,
              unsigned num_draws)
{
   if (num_draws > 1) {
      struct pipe_draw_info tmp_info = *dinfo;

      for (unsigned i = 0; i < num_draws; i++) {
         zink_draw_vbo(pctx, &tmp_info, dindirect, &draws[i], 1);
         if (tmp_info.increment_draw_id)
            tmp_info.drawid++;
      }
      return;
   }

   if (!dindirect && (!draws[0].count || !dinfo->instance_count))
      return;

   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_rasterizer_state *rast_state = ctx->rast_state;
   struct zink_depth_stencil_alpha_state *dsa_state = ctx->dsa_state;
   struct zink_so_target *so_target =
      dindirect && dindirect->count_from_stream_output ?
         zink_so_target(dindirect->count_from_stream_output) : NULL;
   VkBuffer counter_buffers[PIPE_MAX_SO_OUTPUTS];
   VkDeviceSize counter_buffer_offsets[PIPE_MAX_SO_OUTPUTS] = {};
   bool need_index_buffer_unref = false;


   if (dinfo->primitive_restart && !restart_supported(dinfo->mode)) {
       util_draw_vbo_without_prim_restart(pctx, dinfo, dindirect, &draws[0]);
       return;
   }
   if (dinfo->mode == PIPE_PRIM_QUADS ||
       dinfo->mode == PIPE_PRIM_QUAD_STRIP ||
       dinfo->mode == PIPE_PRIM_POLYGON ||
       (dinfo->mode == PIPE_PRIM_TRIANGLE_FAN && !screen->have_triangle_fans) ||
       dinfo->mode == PIPE_PRIM_LINE_LOOP) {
      if (!u_trim_pipe_prim(dinfo->mode, (unsigned *)&draws[0].count))
         return;

      util_primconvert_save_rasterizer_state(ctx->primconvert, &rast_state->base);
      util_primconvert_draw_vbo(ctx->primconvert, dinfo, &draws[0]);
      return;
   }
   if (ctx->gfx_pipeline_state.vertices_per_patch != dinfo->vertices_per_patch)
      ctx->gfx_pipeline_state.dirty = true;
   bool drawid_broken = ctx->drawid_broken;
   ctx->drawid_broken = BITSET_TEST(ctx->gfx_stages[PIPE_SHADER_VERTEX]->nir->info.system_values_read, SYSTEM_VALUE_DRAW_ID) &&
                        (!dindirect || !dindirect->buffer);
   if (drawid_broken != ctx->drawid_broken)
      ctx->dirty_shader_stages |= BITFIELD_BIT(PIPE_SHADER_VERTEX);
   ctx->gfx_pipeline_state.vertices_per_patch = dinfo->vertices_per_patch;
   struct zink_gfx_program *gfx_program = get_gfx_program(ctx);
   if (!gfx_program)
      return;

   if (ctx->gfx_pipeline_state.primitive_restart != !!dinfo->primitive_restart)
      ctx->gfx_pipeline_state.dirty = true;
   ctx->gfx_pipeline_state.primitive_restart = !!dinfo->primitive_restart;

   for (unsigned i = 0; i < ctx->element_state->hw_state.num_bindings; i++) {
      unsigned binding = ctx->element_state->binding_map[i];
      const struct pipe_vertex_buffer *vb = ctx->buffers + binding;
      if (ctx->gfx_pipeline_state.bindings[i].stride != vb->stride) {
         ctx->gfx_pipeline_state.bindings[i].stride = vb->stride;
         ctx->gfx_pipeline_state.dirty = true;
      }
   }

   VkPipeline pipeline = zink_get_gfx_pipeline(screen, gfx_program,
                                               &ctx->gfx_pipeline_state,
                                               dinfo->mode);

   enum pipe_prim_type reduced_prim = u_reduced_prim(dinfo->mode);

   bool depth_bias = false;
   switch (reduced_prim) {
   case PIPE_PRIM_POINTS:
      depth_bias = rast_state->offset_point;
      break;

   case PIPE_PRIM_LINES:
      depth_bias = rast_state->offset_line;
      break;

   case PIPE_PRIM_TRIANGLES:
      depth_bias = rast_state->offset_tri;
      break;

   default:
      unreachable("unexpected reduced prim");
   }

   unsigned index_offset = 0;
   struct pipe_resource *index_buffer = NULL;
   if (dinfo->index_size > 0) {
       uint32_t restart_index = util_prim_restart_index_from_size(dinfo->index_size);
       if ((dinfo->primitive_restart && (dinfo->restart_index != restart_index)) ||
           (!screen->info.have_EXT_index_type_uint8 && dinfo->index_size == 1)) {
          util_translate_prim_restart_ib(pctx, dinfo, dindirect, &draws[0], &index_buffer);
          need_index_buffer_unref = true;
       } else {
          if (dinfo->has_user_indices) {
             if (!util_upload_index_buffer(pctx, dinfo, &draws[0], &index_buffer, &index_offset, 4)) {
                debug_printf("util_upload_index_buffer() failed\n");
                return;
             }
          } else
             index_buffer = dinfo->index.resource;
       }
   }
   if (ctx->xfb_barrier)
      zink_emit_xfb_counter_barrier(ctx);

   if (ctx->dirty_so_targets && ctx->num_so_targets)
      zink_emit_stream_output_targets(pctx);

   if (so_target && zink_resource_buffer_needs_barrier(zink_resource(so_target->base.buffer),
                                                       VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT))
      zink_emit_xfb_vertex_input_barrier(ctx, zink_resource(so_target->base.buffer));

   barrier_vertex_buffers(ctx);
   barrier_draw_buffers(ctx, dinfo, dindirect, index_buffer);

   update_descriptors(ctx, screen, false);

   struct zink_batch *batch = zink_batch_rp(ctx);
   VkViewport viewports[PIPE_MAX_VIEWPORTS] = {};
   for (unsigned i = 0; i < ctx->vp_state.num_viewports; i++) {
      VkViewport viewport = {
         ctx->vp_state.viewport_states[i].translate[0] - ctx->vp_state.viewport_states[i].scale[0],
         ctx->vp_state.viewport_states[i].translate[1] - ctx->vp_state.viewport_states[i].scale[1],
         ctx->vp_state.viewport_states[i].scale[0] * 2,
         ctx->vp_state.viewport_states[i].scale[1] * 2,
         ctx->rast_state->base.clip_halfz ?
            ctx->vp_state.viewport_states[i].translate[2] :
            ctx->vp_state.viewport_states[i].translate[2] - ctx->vp_state.viewport_states[i].scale[2],
         ctx->vp_state.viewport_states[i].translate[2] + ctx->vp_state.viewport_states[i].scale[2]
      };
      viewports[i] = viewport;
   }
   if (screen->info.have_EXT_extended_dynamic_state)
      screen->vk_CmdSetViewportWithCountEXT(batch->cmdbuf, ctx->vp_state.num_viewports, viewports);
   else
      vkCmdSetViewport(batch->cmdbuf, 0, ctx->vp_state.num_viewports, viewports);
   VkRect2D scissors[PIPE_MAX_VIEWPORTS] = {};
   if (ctx->rast_state->base.scissor) {
      for (unsigned i = 0; i < ctx->vp_state.num_viewports; i++) {
         scissors[i].offset.x = ctx->vp_state.scissor_states[i].minx;
         scissors[i].offset.y = ctx->vp_state.scissor_states[i].miny;
         scissors[i].extent.width = ctx->vp_state.scissor_states[i].maxx - ctx->vp_state.scissor_states[i].minx;
         scissors[i].extent.height = ctx->vp_state.scissor_states[i].maxy - ctx->vp_state.scissor_states[i].miny;
      }
   } else if (ctx->fb_state.width && ctx->fb_state.height) {
      for (unsigned i = 0; i < ctx->vp_state.num_viewports; i++) {
         scissors[i].extent.width = ctx->fb_state.width;
         scissors[i].extent.height = ctx->fb_state.height;
      }
   }
   if (screen->info.have_EXT_extended_dynamic_state)
      screen->vk_CmdSetScissorWithCountEXT(batch->cmdbuf, ctx->vp_state.num_viewports, scissors);
   else
      vkCmdSetScissor(batch->cmdbuf, 0, ctx->vp_state.num_viewports, scissors);

   if (line_width_needed(reduced_prim, rast_state->hw_state.polygon_mode)) {
      if (screen->info.feats.features.wideLines || ctx->line_width == 1.0f)
         vkCmdSetLineWidth(batch->cmdbuf, ctx->line_width);
      else
         debug_printf("BUG: wide lines not supported, needs fallback!");
   }

   if (dsa_state->base.stencil[0].enabled) {
      if (dsa_state->base.stencil[1].enabled) {
         vkCmdSetStencilReference(batch->cmdbuf, VK_STENCIL_FACE_FRONT_BIT,
                                  ctx->stencil_ref.ref_value[0]);
         vkCmdSetStencilReference(batch->cmdbuf, VK_STENCIL_FACE_BACK_BIT,
                                  ctx->stencil_ref.ref_value[1]);
      } else
         vkCmdSetStencilReference(batch->cmdbuf,
                                  VK_STENCIL_FACE_FRONT_AND_BACK,
                                  ctx->stencil_ref.ref_value[0]);
   }

   if (depth_bias)
      vkCmdSetDepthBias(batch->cmdbuf, rast_state->offset_units, rast_state->offset_clamp, rast_state->offset_scale);
   else
      vkCmdSetDepthBias(batch->cmdbuf, 0.0f, 0.0f, 0.0f);

   if (ctx->gfx_pipeline_state.blend_state->need_blend_constants)
      vkCmdSetBlendConstants(batch->cmdbuf, ctx->blend_constants);

   vkCmdBindPipeline(batch->cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

   zink_bind_vertex_buffers(batch, ctx);

   if (BITSET_TEST(ctx->gfx_stages[PIPE_SHADER_VERTEX]->nir->info.system_values_read, SYSTEM_VALUE_BASE_VERTEX)) {
      unsigned draw_mode_is_indexed = dinfo->index_size > 0;
      vkCmdPushConstants(batch->cmdbuf, gfx_program->layout, VK_SHADER_STAGE_VERTEX_BIT,
                         offsetof(struct zink_push_constant, draw_mode_is_indexed), sizeof(unsigned),
                         &draw_mode_is_indexed);
   }
   if (ctx->drawid_broken) {
      unsigned draw_id = dinfo->drawid;
      vkCmdPushConstants(batch->cmdbuf, gfx_program->layout, VK_SHADER_STAGE_VERTEX_BIT,
                         offsetof(struct zink_push_constant, draw_id), sizeof(unsigned),
                         &draw_id);
   }
   if (gfx_program->shaders[PIPE_SHADER_TESS_CTRL] && gfx_program->shaders[PIPE_SHADER_TESS_CTRL]->is_generated)
      vkCmdPushConstants(batch->cmdbuf, gfx_program->layout, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                         offsetof(struct zink_push_constant, default_inner_level), sizeof(float) * 6,
                         &ctx->tess_levels[0]);

   zink_query_update_gs_states(ctx);

   if (ctx->num_so_targets) {
      for (unsigned i = 0; i < ctx->num_so_targets; i++) {
         struct zink_so_target *t = zink_so_target(ctx->so_targets[i]);
         counter_buffers[i] = VK_NULL_HANDLE;
         if (t) {
            struct zink_resource *res = zink_resource(t->counter_buffer);
            zink_batch_reference_resource_rw(batch, res, true);
            if (t->counter_buffer_valid) {
               counter_buffers[i] = res->buffer;
               counter_buffer_offsets[i] = t->counter_buffer_offset;
            }
         }
      }
      screen->vk_CmdBeginTransformFeedbackEXT(batch->cmdbuf, 0, ctx->num_so_targets, counter_buffers, counter_buffer_offsets);
   }

   if (dinfo->index_size > 0) {
      VkIndexType index_type;
      unsigned index_size = dinfo->index_size;
      if (need_index_buffer_unref)
         /* index buffer will have been promoted from uint8 to uint16 in this case */
         index_size = MAX2(index_size, 2);
      switch (index_size) {
      case 1:
         assert(screen->info.have_EXT_index_type_uint8);
         index_type = VK_INDEX_TYPE_UINT8_EXT;
         break;
      case 2:
         index_type = VK_INDEX_TYPE_UINT16;
         break;
      case 4:
         index_type = VK_INDEX_TYPE_UINT32;
         break;
      default:
         unreachable("unknown index size!");
      }
      struct zink_resource *res = zink_resource(index_buffer);
      vkCmdBindIndexBuffer(batch->cmdbuf, res->buffer, index_offset, index_type);
      zink_batch_reference_resource_rw(batch, res, false);
      if (dindirect && dindirect->buffer) {
         struct zink_resource *indirect = zink_resource(dindirect->buffer);
         zink_batch_reference_resource_rw(batch, indirect, false);
         if (dindirect->indirect_draw_count) {
             struct zink_resource *indirect_draw_count = zink_resource(dindirect->indirect_draw_count);
             zink_batch_reference_resource_rw(batch, indirect_draw_count, false);
             screen->vk_CmdDrawIndexedIndirectCount(batch->cmdbuf, indirect->buffer, dindirect->offset,
                                           indirect_draw_count->buffer, dindirect->indirect_draw_count_offset,
                                           dindirect->draw_count, dindirect->stride);
         } else
            vkCmdDrawIndexedIndirect(batch->cmdbuf, indirect->buffer, dindirect->offset, dindirect->draw_count, dindirect->stride);
      } else
         vkCmdDrawIndexed(batch->cmdbuf,
            draws[0].count, dinfo->instance_count,
            need_index_buffer_unref ? 0 : draws[0].start, dinfo->index_bias, dinfo->start_instance);
   } else {
      if (so_target && screen->info.tf_props.transformFeedbackDraw) {
         zink_batch_reference_resource_rw(batch, zink_resource(so_target->base.buffer), false);
         zink_batch_reference_resource_rw(batch, zink_resource(so_target->counter_buffer), true);
         screen->vk_CmdDrawIndirectByteCountEXT(batch->cmdbuf, dinfo->instance_count, dinfo->start_instance,
                                       zink_resource(so_target->counter_buffer)->buffer, so_target->counter_buffer_offset, 0,
                                       MIN2(so_target->stride, screen->info.tf_props.maxTransformFeedbackBufferDataStride));
      } else if (dindirect && dindirect->buffer) {
         struct zink_resource *indirect = zink_resource(dindirect->buffer);
         zink_batch_reference_resource_rw(batch, indirect, false);
         if (dindirect->indirect_draw_count) {
             struct zink_resource *indirect_draw_count = zink_resource(dindirect->indirect_draw_count);
             zink_batch_reference_resource_rw(batch, indirect_draw_count, false);
             screen->vk_CmdDrawIndirectCount(batch->cmdbuf, indirect->buffer, dindirect->offset,
                                           indirect_draw_count->buffer, dindirect->indirect_draw_count_offset,
                                           dindirect->draw_count, dindirect->stride);
         } else
            vkCmdDrawIndirect(batch->cmdbuf, indirect->buffer, dindirect->offset, dindirect->draw_count, dindirect->stride);
      } else
         vkCmdDraw(batch->cmdbuf, draws[0].count, dinfo->instance_count, draws[0].start, dinfo->start_instance);
   }

   if (dinfo->index_size > 0 && (dinfo->has_user_indices || need_index_buffer_unref))
      pipe_resource_reference(&index_buffer, NULL);

   if (ctx->num_so_targets) {
      for (unsigned i = 0; i < ctx->num_so_targets; i++) {
         struct zink_so_target *t = zink_so_target(ctx->so_targets[i]);
         if (t) {
            counter_buffers[i] = zink_resource(t->counter_buffer)->buffer;
            counter_buffer_offsets[i] = t->counter_buffer_offset;
            t->counter_buffer_valid = true;
         }
      }
      screen->vk_CmdEndTransformFeedbackEXT(batch->cmdbuf, 0, ctx->num_so_targets, counter_buffers, counter_buffer_offsets);
   }
   batch->has_draw = true;
}

void
zink_launch_grid(struct pipe_context *pctx, const struct pipe_grid_info *info)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_batch *batch = &ctx->compute_batch;
   struct zink_compute_program *comp_program = get_compute_program(ctx);
   if (!comp_program)
      return;

   VkPipeline pipeline = zink_get_compute_pipeline(screen, comp_program,
                                               &ctx->compute_pipeline_state);

   update_descriptors(ctx, screen, true);


   vkCmdBindPipeline(batch->cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   if (info->indirect) {
      vkCmdDispatchIndirect(batch->cmdbuf, zink_resource(info->indirect)->buffer, info->indirect_offset);
      zink_batch_reference_resource_rw(batch, zink_resource(info->indirect), false);
   } else
      vkCmdDispatch(batch->cmdbuf, info->grid[0], info->grid[1], info->grid[2]);
   batch->has_draw = true;
}

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

#include "zink_context.h"

#include "zink_batch.h"
#include "zink_compiler.h"
#include "zink_fence.h"
#include "zink_framebuffer.h"
#include "zink_helpers.h"
#include "zink_program.h"
#include "zink_pipeline.h"
#include "zink_query.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_state.h"
#include "zink_surface.h"

#include "indices/u_primconvert.h"
#include "util/u_blitter.h"
#include "util/u_debug.h"
#include "util/format/u_format.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"

#include "nir.h"

#include "util/u_memory.h"
#include "util/u_upload_mgr.h"

static void
zink_context_destroy(struct pipe_context *pctx)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);

   if (vkQueueWaitIdle(ctx->queue) != VK_SUCCESS)
      debug_printf("vkQueueWaitIdle failed\n");

   util_blitter_destroy(ctx->blitter);

   pipe_resource_reference(&ctx->dummy_vertex_buffer, NULL);
   pipe_resource_reference(&ctx->dummy_xfb_buffer, NULL);
   for (unsigned i = 0; i < ARRAY_SIZE(ctx->null_buffers); i++)
      pipe_resource_reference(&ctx->null_buffers[i], NULL);

   for (int i = 0; i < ARRAY_SIZE(ctx->batches); ++i) {
      zink_batch_release(screen, &ctx->batches[i]);
      util_dynarray_fini(&ctx->batches[i].zombie_samplers);
      vkDestroyDescriptorPool(screen->dev, ctx->batches[i].descpool, NULL);

      _mesa_set_destroy(ctx->batches[i].resources, NULL);
      _mesa_set_destroy(ctx->batches[i].sampler_views, NULL);
      _mesa_set_destroy(ctx->batches[i].programs, NULL);
      vkFreeCommandBuffers(screen->dev, ctx->batches[i].cmdpool, 1, &ctx->batches[i].cmdbuf);
      vkDestroyCommandPool(screen->dev, ctx->batches[i].cmdpool, NULL);
   }
   if (ctx->compute_batch.cmdpool) {
      zink_batch_release(screen, &ctx->compute_batch);
      util_dynarray_fini(&ctx->compute_batch.zombie_samplers);
      vkDestroyDescriptorPool(screen->dev, ctx->compute_batch.descpool, NULL);
      vkFreeCommandBuffers(screen->dev, ctx->compute_batch.cmdpool, 1, &ctx->compute_batch.cmdbuf);

      _mesa_set_destroy(ctx->compute_batch.resources, NULL);
      _mesa_set_destroy(ctx->compute_batch.sampler_views, NULL);
      _mesa_set_destroy(ctx->compute_batch.programs, NULL);
      vkDestroyCommandPool(screen->dev, ctx->compute_batch.cmdpool, NULL);
   }

   util_primconvert_destroy(ctx->primconvert);
   u_upload_destroy(pctx->stream_uploader);
   slab_destroy_child(&ctx->transfer_pool);
   _mesa_hash_table_destroy(ctx->program_cache, NULL);
   _mesa_hash_table_destroy(ctx->compute_program_cache, NULL);
   _mesa_hash_table_destroy(ctx->render_pass_cache, NULL);
   FREE(ctx);
}

static enum pipe_reset_status
zink_get_device_reset_status(struct pipe_context *pctx)
{
   struct zink_context *ctx = zink_context(pctx);

   enum pipe_reset_status status = PIPE_NO_RESET;

   if (ctx->is_device_lost) {
      // Since we don't know what really happened to the hardware, just
      // assume that we are in the wrong
      status = PIPE_GUILTY_CONTEXT_RESET;

      debug_printf("ZINK: device lost detected!\n");

      if (ctx->reset.reset)
         ctx->reset.reset(ctx->reset.data, status);
   }

   return status;
}

static void
zink_set_device_reset_callback(struct pipe_context *pctx,
                               const struct pipe_device_reset_callback *cb)
{
   struct zink_context *ctx = zink_context(pctx);

   if (cb)
      ctx->reset = *cb;
   else
      memset(&ctx->reset, 0, sizeof(ctx->reset));
}

static VkSamplerMipmapMode
sampler_mipmap_mode(enum pipe_tex_mipfilter filter)
{
   switch (filter) {
   case PIPE_TEX_MIPFILTER_NEAREST: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
   case PIPE_TEX_MIPFILTER_LINEAR: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
   case PIPE_TEX_MIPFILTER_NONE:
      unreachable("PIPE_TEX_MIPFILTER_NONE should be dealt with earlier");
   }
   unreachable("unexpected filter");
}

static VkSamplerAddressMode
sampler_address_mode(enum pipe_tex_wrap filter)
{
   switch (filter) {
   case PIPE_TEX_WRAP_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
   case PIPE_TEX_WRAP_MIRROR_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; /* not technically correct, but kinda works */
   default: break;
   }
   unreachable("unexpected wrap");
}

static VkCompareOp
compare_op(enum pipe_compare_func op)
{
   switch (op) {
      case PIPE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
      case PIPE_FUNC_LESS: return VK_COMPARE_OP_LESS;
      case PIPE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
      case PIPE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
      case PIPE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
      case PIPE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
      case PIPE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
      case PIPE_FUNC_ALWAYS: return VK_COMPARE_OP_ALWAYS;
   }
   unreachable("unexpected compare");
}

static inline bool
wrap_needs_border_color(unsigned wrap)
{
   return wrap == PIPE_TEX_WRAP_CLAMP || wrap == PIPE_TEX_WRAP_CLAMP_TO_BORDER ||
          wrap == PIPE_TEX_WRAP_MIRROR_CLAMP || wrap == PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER;
}

struct zink_sampler_state {
   VkSampler sampler;
   bool custom_border_color;
};

static void *
zink_create_sampler_state(struct pipe_context *pctx,
                          const struct pipe_sampler_state *state)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   bool need_custom = false;

   VkSamplerCreateInfo sci = {};
   VkSamplerCustomBorderColorCreateInfoEXT cbci = {};
   sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
   sci.magFilter = zink_filter(state->mag_img_filter);
   sci.minFilter = zink_filter(state->min_img_filter);

   if (state->min_mip_filter != PIPE_TEX_MIPFILTER_NONE) {
      sci.mipmapMode = sampler_mipmap_mode(state->min_mip_filter);
      sci.minLod = state->min_lod;
      sci.maxLod = state->max_lod;
   } else {
      sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      sci.minLod = 0;
      sci.maxLod = 0;
   }

   sci.addressModeU = sampler_address_mode(state->wrap_s);
   sci.addressModeV = sampler_address_mode(state->wrap_t);
   sci.addressModeW = sampler_address_mode(state->wrap_r);
   sci.mipLodBias = state->lod_bias;

   need_custom |= wrap_needs_border_color(state->wrap_s);
   need_custom |= wrap_needs_border_color(state->wrap_t);
   need_custom |= wrap_needs_border_color(state->wrap_r);

   if (state->compare_mode == PIPE_TEX_COMPARE_NONE)
      sci.compareOp = VK_COMPARE_OP_NEVER;
   else {
      sci.compareOp = compare_op(state->compare_func);
      sci.compareEnable = VK_TRUE;
   }

   if (screen->info.have_EXT_custom_border_color &&
       screen->info.border_color_feats.customBorderColorWithoutFormat && need_custom) {
      cbci.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
      cbci.format = VK_FORMAT_UNDEFINED;
      /* these are identical unions */
      memcpy(&cbci.customBorderColor, &state->border_color, sizeof(union pipe_color_union));
      sci.pNext = &cbci;
      sci.borderColor = VK_BORDER_COLOR_INT_CUSTOM_EXT;
      UNUSED uint32_t check = p_atomic_inc_return(&screen->cur_custom_border_color_samplers);
      assert(check <= screen->info.border_color_props.maxCustomBorderColorSamplers);
   } else
      sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; // TODO with custom shader if we're super interested?
   sci.unnormalizedCoordinates = !state->normalized_coords;

   if (state->max_anisotropy > 1) {
      sci.maxAnisotropy = state->max_anisotropy;
      sci.anisotropyEnable = VK_TRUE;
   }

   struct zink_sampler_state *sampler = CALLOC_STRUCT(zink_sampler_state);
   if (!sampler)
      return NULL;

   if (vkCreateSampler(screen->dev, &sci, NULL, &sampler->sampler) != VK_SUCCESS) {
      FREE(sampler);
      return NULL;
   }

   return sampler;
}

static void
zink_bind_sampler_states(struct pipe_context *pctx,
                         enum pipe_shader_type shader,
                         unsigned start_slot,
                         unsigned num_samplers,
                         void **samplers)
{
   struct zink_context *ctx = zink_context(pctx);
   for (unsigned i = 0; i < num_samplers; ++i) {
      VkSampler *sampler = samplers[i];
      ctx->sampler_states[shader][start_slot + i] = sampler;
      ctx->samplers[shader][start_slot + i] = sampler ? *sampler : VK_NULL_HANDLE;
   }
   ctx->num_samplers[shader] = start_slot + num_samplers;
}

static void
zink_delete_sampler_state(struct pipe_context *pctx,
                          void *sampler_state)
{
   struct zink_sampler_state *sampler = sampler_state;
   struct zink_batch *batch = zink_curr_batch(zink_context(pctx));
   util_dynarray_append(&batch->zombie_samplers, VkSampler,
                        sampler->sampler);
   if (sampler->custom_border_color)
      p_atomic_dec(&zink_screen(pctx->screen)->cur_custom_border_color_samplers);
   FREE(sampler);
}


static VkImageViewType
image_view_type(enum pipe_texture_target target)
{
   switch (target) {
   case PIPE_TEXTURE_1D: return VK_IMAGE_VIEW_TYPE_1D;
   case PIPE_TEXTURE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
   case PIPE_TEXTURE_2D: return VK_IMAGE_VIEW_TYPE_2D;
   case PIPE_TEXTURE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   case PIPE_TEXTURE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
   case PIPE_TEXTURE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
   case PIPE_TEXTURE_3D: return VK_IMAGE_VIEW_TYPE_3D;
   case PIPE_TEXTURE_RECT: return VK_IMAGE_VIEW_TYPE_2D;
   default:
      unreachable("unexpected target");
   }
}

static VkComponentSwizzle
component_mapping(enum pipe_swizzle swizzle)
{
   switch (swizzle) {
   case PIPE_SWIZZLE_X: return VK_COMPONENT_SWIZZLE_R;
   case PIPE_SWIZZLE_Y: return VK_COMPONENT_SWIZZLE_G;
   case PIPE_SWIZZLE_Z: return VK_COMPONENT_SWIZZLE_B;
   case PIPE_SWIZZLE_W: return VK_COMPONENT_SWIZZLE_A;
   case PIPE_SWIZZLE_0: return VK_COMPONENT_SWIZZLE_ZERO;
   case PIPE_SWIZZLE_1: return VK_COMPONENT_SWIZZLE_ONE;
   case PIPE_SWIZZLE_NONE: return VK_COMPONENT_SWIZZLE_IDENTITY; // ???
   default:
      unreachable("unexpected swizzle");
   }
}

static VkImageAspectFlags
sampler_aspect_from_format(enum pipe_format fmt)
{
   if (util_format_is_depth_or_stencil(fmt)) {
      const struct util_format_description *desc = util_format_description(fmt);
      if (util_format_has_depth(desc))
         return VK_IMAGE_ASPECT_DEPTH_BIT;
      assert(util_format_has_stencil(desc));
      return VK_IMAGE_ASPECT_STENCIL_BIT;
   } else
     return VK_IMAGE_ASPECT_COLOR_BIT;
}

static VkBufferView
create_buffer_view(struct zink_screen *screen, struct zink_resource *res, enum pipe_format format, uint32_t offset, uint32_t range)
{
   VkBufferView view = VK_NULL_HANDLE;
   VkBufferViewCreateInfo bvci = {};
   bvci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
   bvci.buffer = res->buffer;
   bvci.format = zink_get_format(screen, format);
   assert(bvci.format);
   bvci.offset = offset;
   bvci.range = range;

   if (vkCreateBufferView(screen->dev, &bvci, NULL, &view) == VK_SUCCESS)
      return view;
   return VK_NULL_HANDLE;
}

static struct pipe_sampler_view *
zink_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *pres,
                         const struct pipe_sampler_view *state)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_resource *res = zink_resource(pres);
   struct zink_sampler_view *sampler_view = CALLOC_STRUCT(zink_sampler_view);
   VkResult err;

   sampler_view->base = *state;
   sampler_view->base.texture = NULL;
   pipe_resource_reference(&sampler_view->base.texture, pres);
   sampler_view->base.reference.count = 1;
   sampler_view->base.context = pctx;

   if (state->target != PIPE_BUFFER) {
      VkImageViewCreateInfo ivci = {};
      ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      ivci.image = res->image;
      ivci.viewType = image_view_type(state->target);

      ivci.components.r = component_mapping(state->swizzle_r);
      ivci.components.g = component_mapping(state->swizzle_g);
      ivci.components.b = component_mapping(state->swizzle_b);
      ivci.components.a = component_mapping(state->swizzle_a);
      ivci.subresourceRange.aspectMask = sampler_aspect_from_format(state->format);
      /* samplers for stencil aspects of packed formats need to always use stencil type */
      if (ivci.subresourceRange.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
         ivci.format = VK_FORMAT_S8_UINT;
         ivci.components.g = VK_COMPONENT_SWIZZLE_R;
      } else
         ivci.format = zink_get_format(screen, state->format);
      assert(ivci.format);

      ivci.subresourceRange.baseMipLevel = state->u.tex.first_level;
      ivci.subresourceRange.baseArrayLayer = state->u.tex.first_layer;
      ivci.subresourceRange.levelCount = state->u.tex.last_level - state->u.tex.first_level + 1;
      ivci.subresourceRange.layerCount = state->u.tex.last_layer - state->u.tex.first_layer + 1;

      err = vkCreateImageView(screen->dev, &ivci, NULL, &sampler_view->image_view);
   } else {
      sampler_view->buffer_view = create_buffer_view(screen, res, state->format, state->u.buf.offset, state->u.buf.size);
      err = !sampler_view->buffer_view;
   }
   if (err != VK_SUCCESS) {
      FREE(sampler_view);
      return NULL;
   }
   return &sampler_view->base;
}

static void
zink_sampler_view_destroy(struct pipe_context *pctx,
                          struct pipe_sampler_view *pview)
{
   struct zink_sampler_view *view = zink_sampler_view(pview);
   if (pview->texture->target == PIPE_BUFFER)
      vkDestroyBufferView(zink_screen(pctx->screen)->dev, view->buffer_view, NULL);
   else
      vkDestroyImageView(zink_screen(pctx->screen)->dev, view->image_view, NULL);
   pipe_resource_reference(&pview->texture, NULL);
   FREE(view);
}

static void
zink_get_sample_position(struct pipe_context *ctx,
                         unsigned sample_count,
                         unsigned sample_index,
                         float *out_value)
{
   /* TODO: handle this I guess */
   assert(zink_screen(ctx->screen)->info.props.limits.standardSampleLocations);
   /* from 26.4. Multisampling */
   switch (sample_count) {
   case 0:
   case 1: {
      float pos[][2] = { {0.5,0.5}, };
      out_value[0] = pos[sample_index][0];
      out_value[1] = pos[sample_index][1];
      break;
   }
   case 2: {
      float pos[][2] = { {0.75,0.75},
                        {0.25,0.25}, };
      out_value[0] = pos[sample_index][0];
      out_value[1] = pos[sample_index][1];
      break;
   }
   case 4: {
      float pos[][2] = { {0.375, 0.125},
                        {0.875, 0.375},
                        {0.125, 0.625},
                        {0.625, 0.875}, };
      out_value[0] = pos[sample_index][0];
      out_value[1] = pos[sample_index][1];
      break;
   }
   case 8: {
      float pos[][2] = { {0.5625, 0.3125},
                        {0.4375, 0.6875},
                        {0.8125, 0.5625},
                        {0.3125, 0.1875},
                        {0.1875, 0.8125},
                        {0.0625, 0.4375},
                        {0.6875, 0.9375},
                        {0.9375, 0.0625}, };
      out_value[0] = pos[sample_index][0];
      out_value[1] = pos[sample_index][1];
      break;
   }
   case 16: {
      float pos[][2] = { {0.5625, 0.5625},
                        {0.4375, 0.3125},
                        {0.3125, 0.625},
                        {0.75, 0.4375},
                        {0.1875, 0.375},
                        {0.625, 0.8125},
                        {0.8125, 0.6875},
                        {0.6875, 0.1875},
                        {0.375, 0.875},
                        {0.5, 0.0625},
                        {0.25, 0.125},
                        {0.125, 0.75},
                        {0.0, 0.5},
                        {0.9375, 0.25},
                        {0.875, 0.9375},
                        {0.0625, 0.0}, };
      out_value[0] = pos[sample_index][0];
      out_value[1] = pos[sample_index][1];
      break;
   }
   default:
      unreachable("unhandled sample count!");
   }
}

static void
zink_set_polygon_stipple(struct pipe_context *pctx,
                         const struct pipe_poly_stipple *ps)
{
}

static void
zink_set_vertex_buffers(struct pipe_context *pctx,
                        unsigned start_slot,
                        unsigned num_buffers,
                        unsigned unbind_num_trailing_slots,
                        bool take_ownership,
                        const struct pipe_vertex_buffer *buffers)
{
   struct zink_context *ctx = zink_context(pctx);

   util_set_vertex_buffers_mask(ctx->buffers, &ctx->buffers_enabled_mask,
                                buffers, start_slot, num_buffers,
                                unbind_num_trailing_slots, take_ownership);
}

static void
zink_set_viewport_states(struct pipe_context *pctx,
                         unsigned start_slot,
                         unsigned num_viewports,
                         const struct pipe_viewport_state *state)
{
   struct zink_context *ctx = zink_context(pctx);

   for (unsigned i = 0; i < num_viewports; ++i)
      ctx->vp_state.viewport_states[start_slot + i] = state[i];
   ctx->vp_state.num_viewports = start_slot + num_viewports;

   if (!zink_screen(pctx->screen)->info.have_EXT_extended_dynamic_state) {
      if (ctx->gfx_pipeline_state.num_viewports != ctx->vp_state.num_viewports)
         ctx->gfx_pipeline_state.dirty = true;
      ctx->gfx_pipeline_state.num_viewports = ctx->vp_state.num_viewports;
   }
}

static void
zink_set_scissor_states(struct pipe_context *pctx,
                        unsigned start_slot, unsigned num_scissors,
                        const struct pipe_scissor_state *states)
{
   struct zink_context *ctx = zink_context(pctx);

   for (unsigned i = 0; i < num_scissors; i++) {
      VkRect2D scissor;

      scissor.offset.x = states[i].minx;
      scissor.offset.y = states[i].miny;
      scissor.extent.width = states[i].maxx - states[i].minx;
      scissor.extent.height = states[i].maxy - states[i].miny;
      ctx->vp_state.scissor_states[start_slot + i] = states[i];
      ctx->vp_state.scissors[start_slot + i] = scissor;
   }
}

static void
zink_set_constant_buffer(struct pipe_context *pctx,
                         enum pipe_shader_type shader, uint index,
                         bool take_ownership,
                         const struct pipe_constant_buffer *cb)
{
   struct zink_context *ctx = zink_context(pctx);

   if (cb) {
      struct pipe_resource *buffer = cb->buffer;
      unsigned offset = cb->buffer_offset;
      if (cb->user_buffer) {
         struct zink_screen *screen = zink_screen(pctx->screen);
         u_upload_data(ctx->base.const_uploader, 0, cb->buffer_size,
                       screen->info.props.limits.minUniformBufferOffsetAlignment,
                       cb->user_buffer, &offset, &buffer);
      }

      if (take_ownership) {
         pipe_resource_reference(&ctx->ubos[shader][index].buffer, NULL);
         ctx->ubos[shader][index].buffer = buffer;
      } else {
         pipe_resource_reference(&ctx->ubos[shader][index].buffer, buffer);
      }
      ctx->ubos[shader][index].buffer_offset = offset;
      ctx->ubos[shader][index].buffer_size = cb->buffer_size;
      ctx->ubos[shader][index].user_buffer = NULL;

      if (cb->user_buffer)
         pipe_resource_reference(&buffer, NULL);
   } else {
      pipe_resource_reference(&ctx->ubos[shader][index].buffer, NULL);
      ctx->ubos[shader][index].buffer_offset = 0;
      ctx->ubos[shader][index].buffer_size = 0;
      ctx->ubos[shader][index].user_buffer = NULL;
   }
}

static void
zink_set_shader_buffers(struct pipe_context *pctx,
                        enum pipe_shader_type p_stage,
                        unsigned start_slot, unsigned count,
                        const struct pipe_shader_buffer *buffers,
                        unsigned writable_bitmask)
{
   struct zink_context *ctx = zink_context(pctx);

   unsigned modified_bits = u_bit_consecutive(start_slot, count);
   ctx->writable_ssbos[p_stage] &= ~modified_bits;
   ctx->writable_ssbos[p_stage] |= writable_bitmask << start_slot;

   for (unsigned i = 0; i < count; i++) {
      struct pipe_shader_buffer *ssbo = &ctx->ssbos[p_stage][start_slot + i];
      if (buffers && buffers[i].buffer) {
         struct zink_resource *res = zink_resource(buffers[i].buffer);
         pipe_resource_reference(&ssbo->buffer, &res->base);
         ssbo->buffer_offset = buffers[i].buffer_offset;
         ssbo->buffer_size = MIN2(buffers[i].buffer_size, res->size - ssbo->buffer_offset);
         util_range_add(&res->base, &res->valid_buffer_range, ssbo->buffer_offset,
                        ssbo->buffer_offset + ssbo->buffer_size);
      } else {
         pipe_resource_reference(&ssbo->buffer, NULL);
         ssbo->buffer_offset = 0;
         ssbo->buffer_size = 0;
      }
   }
}

static void
unbind_shader_image(struct zink_context *ctx, enum pipe_shader_type stage, unsigned slot)
{
   struct zink_image_view *image_view = &ctx->image_views[stage][slot];
   if (!image_view->base.resource)
      return;

   if (image_view->base.resource->target == PIPE_BUFFER)
      vkDestroyBufferView(zink_screen(ctx->base.screen)->dev, image_view->buffer_view, NULL);
   else
      pipe_surface_reference((struct pipe_surface**)&image_view->surface, NULL);
   pipe_resource_reference(&image_view->base.resource, NULL);
   image_view->base.resource = NULL;
   image_view->surface = NULL;
}

static void
zink_set_shader_images(struct pipe_context *pctx,
                       enum pipe_shader_type p_stage,
                       unsigned start_slot, unsigned count,
                       unsigned unbind_num_trailing_slots,
                       const struct pipe_image_view *images)
{
   struct zink_context *ctx = zink_context(pctx);

   for (unsigned i = 0; i < count; i++) {
      struct zink_image_view *image_view = &ctx->image_views[p_stage][start_slot + i];
      if (images && images[i].resource) {
         struct zink_resource *res = zink_resource(images[i].resource);
         util_copy_image_view(&image_view->base, images + i);
         if (images[i].resource->target == PIPE_BUFFER) {
            image_view->buffer_view = create_buffer_view(zink_screen(pctx->screen), res, images[i].format, images[i].u.buf.offset, images[i].u.buf.size);
            assert(image_view->buffer_view);
            util_range_add(&res->base, &res->valid_buffer_range, images[i].u.buf.offset,
                           images[i].u.buf.offset + images[i].u.buf.size);
         } else {
            struct pipe_surface tmpl = {};
            tmpl.format = images[i].format;
            tmpl.nr_samples = 1;
            tmpl.u.tex.level = images[i].u.tex.level;
            tmpl.u.tex.first_layer = images[i].u.tex.first_layer;
            tmpl.u.tex.last_layer = images[i].u.tex.last_layer;
            image_view->surface = zink_surface(pctx->create_surface(pctx, &res->base, &tmpl));
            assert(image_view->surface);
         }
      } else if (image_view->base.resource)
         unbind_shader_image(ctx, p_stage, start_slot + i);
   }

   for (unsigned i = 0; i < unbind_num_trailing_slots; i++)
      unbind_shader_image(ctx, p_stage, start_slot + count + i);
}

static void
zink_set_sampler_views(struct pipe_context *pctx,
                       enum pipe_shader_type shader_type,
                       unsigned start_slot,
                       unsigned num_views,
                       unsigned unbind_num_trailing_slots,
                       struct pipe_sampler_view **views)
{
   struct zink_context *ctx = zink_context(pctx);
   unsigned i;

   for (i = 0; i < num_views; ++i) {
      struct pipe_sampler_view *pview = views ? views[i] : NULL;
      pipe_sampler_view_reference(
         &ctx->sampler_views[shader_type][start_slot + i],
         pview);
   }
   for (; i < num_views + unbind_num_trailing_slots; ++i) {
      pipe_sampler_view_reference(
         &ctx->sampler_views[shader_type][start_slot + i],
         NULL);
   }
   ctx->num_sampler_views[shader_type] = start_slot + num_views;
}

static void
zink_set_stencil_ref(struct pipe_context *pctx,
                     const struct pipe_stencil_ref ref)
{
   struct zink_context *ctx = zink_context(pctx);
   ctx->stencil_ref = ref;
}

static void
zink_set_clip_state(struct pipe_context *pctx,
                    const struct pipe_clip_state *pcs)
{
}

static void
zink_set_tess_state(struct pipe_context *pctx,
                    const float default_outer_level[4],
                    const float default_inner_level[2])
{
   struct zink_context *ctx = zink_context(pctx);
   memcpy(&ctx->default_inner_level, default_inner_level, sizeof(ctx->default_inner_level));
   memcpy(&ctx->default_outer_level, default_outer_level, sizeof(ctx->default_outer_level));
}

static uint32_t
hash_render_pass_state(const void *key)
{
   struct zink_render_pass_state* s = (struct zink_render_pass_state*)key;
   return _mesa_hash_data(key, offsetof(struct zink_render_pass_state, rts) + sizeof(s->rts[0]) * s->num_rts);
}

static bool
equals_render_pass_state(const void *a, const void *b)
{
   const struct zink_render_pass_state *s_a = a, *s_b = b;
   if (s_a->num_rts != s_b->num_rts)
      return false;
   return memcmp(a, b, offsetof(struct zink_render_pass_state, rts) + sizeof(s_a->rts[0]) * s_a->num_rts) == 0;
}

static struct zink_render_pass *
get_render_pass(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   const struct pipe_framebuffer_state *fb = &ctx->fb_state;
   struct zink_render_pass_state state = { 0 };

   for (int i = 0; i < fb->nr_cbufs; i++) {
      struct pipe_surface *surf = fb->cbufs[i];
      if (surf) {
         state.rts[i].format = zink_get_format(screen, surf->format);
         state.rts[i].samples = surf->texture->nr_samples > 0 ? surf->texture->nr_samples :
                                                       VK_SAMPLE_COUNT_1_BIT;
      } else {
         state.rts[i].format = VK_FORMAT_R8_UINT;
         state.rts[i].samples = MAX2(fb->samples, 1);
      }
      state.num_rts++;
   }
   state.num_cbufs = fb->nr_cbufs;

   if (fb->zsbuf) {
      struct zink_resource *zsbuf = zink_resource(fb->zsbuf->texture);
      state.rts[fb->nr_cbufs].format = zsbuf->format;
      state.rts[fb->nr_cbufs].samples = zsbuf->base.nr_samples > 0 ? zsbuf->base.nr_samples : VK_SAMPLE_COUNT_1_BIT;
      state.num_rts++;
   }
   state.have_zsbuf = fb->zsbuf != NULL;

   uint32_t hash = hash_render_pass_state(&state);
   struct hash_entry *entry = _mesa_hash_table_search_pre_hashed(ctx->render_pass_cache, hash,
                                                                 &state);
   if (!entry) {
      struct zink_render_pass *rp;
      rp = zink_create_render_pass(screen, &state);
      entry = _mesa_hash_table_insert_pre_hashed(ctx->render_pass_cache, hash, &rp->state, rp);
      if (!entry)
         return NULL;
   }

   return entry->data;
}

static struct zink_framebuffer *
create_framebuffer(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct pipe_surface *attachments[PIPE_MAX_COLOR_BUFS + 1] = {};

   struct zink_framebuffer_state state = {};
   for (int i = 0; i < ctx->fb_state.nr_cbufs; i++) {
      struct pipe_surface *psurf = ctx->fb_state.cbufs[i];
      state.attachments[i] = psurf ? zink_surface(psurf)->image_view : VK_NULL_HANDLE;
      attachments[i] = psurf;
   }

   state.num_attachments = ctx->fb_state.nr_cbufs;
   if (ctx->fb_state.zsbuf) {
      struct pipe_surface *psurf = ctx->fb_state.zsbuf;
      state.attachments[state.num_attachments] = psurf ? zink_surface(psurf)->image_view : VK_NULL_HANDLE;;
      attachments[state.num_attachments++] = psurf;
   }

   state.width = MAX2(ctx->fb_state.width, 1);
   state.height = MAX2(ctx->fb_state.height, 1);
   state.layers = MAX2(util_framebuffer_get_num_layers(&ctx->fb_state), 1);
   state.samples = ctx->fb_state.samples;

   struct zink_framebuffer *fb = zink_create_framebuffer(ctx, &state, attachments);
   zink_init_framebuffer(screen, fb, get_render_pass(ctx));
   return fb;
}

static void
framebuffer_state_buffer_barriers_setup(struct zink_context *ctx,
                                        const struct pipe_framebuffer_state *state, struct zink_batch *batch)
{
   for (int i = 0; i < state->nr_cbufs; i++) {
      struct pipe_surface *surf = state->cbufs[i];
      if (!surf)
         surf = ctx->framebuffer->null_surface;
      struct zink_resource *res = zink_resource(surf->texture);
      if (res->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
         zink_resource_image_barrier(batch, res,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0);
   }

   if (state->zsbuf) {
      struct zink_resource *res = zink_resource(state->zsbuf->texture);
      if (res->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
         zink_resource_image_barrier(batch, res,
                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 0);
   }
}

void
zink_begin_render_pass(struct zink_context *ctx, struct zink_batch *batch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   assert(batch == zink_curr_batch(ctx));
   assert(ctx->gfx_pipeline_state.render_pass);

   struct pipe_framebuffer_state *fb_state = &ctx->fb_state;

   VkRenderPassBeginInfo rpbi = {};
   rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   rpbi.renderPass = ctx->gfx_pipeline_state.render_pass->render_pass;
   rpbi.renderArea.offset.x = 0;
   rpbi.renderArea.offset.y = 0;
   rpbi.renderArea.extent.width = fb_state->width;
   rpbi.renderArea.extent.height = fb_state->height;
   rpbi.clearValueCount = 0;
   rpbi.pClearValues = NULL;
   rpbi.framebuffer = ctx->framebuffer->fb;

   assert(ctx->gfx_pipeline_state.render_pass && ctx->framebuffer);
   assert(!batch->fb || batch->fb == ctx->framebuffer);

   framebuffer_state_buffer_barriers_setup(ctx, fb_state, batch);

   zink_framebuffer_reference(screen, &batch->fb, ctx->framebuffer);
   for (struct zink_surface **surf = (struct zink_surface **)batch->fb->surfaces; *surf; surf++)
      zink_batch_reference_resource_rw(batch, zink_resource((*surf)->base.texture), true);

   vkCmdBeginRenderPass(batch->cmdbuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   batch->in_rp = true;
}

static void
zink_end_render_pass(struct zink_context *ctx, struct zink_batch *batch)
{
   if (batch->in_rp)
      vkCmdEndRenderPass(batch->cmdbuf);
   batch->in_rp = false;
}

static void
flush_batch(struct zink_context *ctx)
{
   struct zink_batch *batch = zink_curr_batch(ctx);
   zink_end_render_pass(ctx, batch);
   zink_end_batch(ctx, batch);

   ctx->curr_batch++;
   if (ctx->curr_batch == ARRAY_SIZE(ctx->batches))
      ctx->curr_batch = 0;

   zink_start_batch(ctx, zink_curr_batch(ctx));
}

struct zink_batch *
zink_batch_rp(struct zink_context *ctx)
{
   struct zink_batch *batch = zink_curr_batch(ctx);
   if (!batch->in_rp) {
      zink_begin_render_pass(ctx, batch);
      assert(batch->fb && batch->fb->rp);
   }
   return batch;
}

struct zink_batch *
zink_batch_no_rp(struct zink_context *ctx)
{
   struct zink_batch *batch = zink_curr_batch(ctx);
   zink_end_render_pass(ctx, batch);
   assert(!batch->in_rp);
   return batch;
}

void
zink_flush_compute(struct zink_context *ctx)
{
   zink_end_batch(ctx, &ctx->compute_batch);
   zink_start_batch(ctx, &ctx->compute_batch);
}

static void
zink_set_framebuffer_state(struct pipe_context *pctx,
                           const struct pipe_framebuffer_state *state)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);

   util_copy_framebuffer_state(&ctx->fb_state, state);

   struct zink_framebuffer *fb = ctx->framebuffer;
   /* explicitly unref previous fb to ensure it gets destroyed */
   if (fb)
      zink_framebuffer_reference(screen, &fb, NULL);
   fb = create_framebuffer(ctx);
   zink_framebuffer_reference(screen, &ctx->framebuffer, fb);
   ctx->gfx_pipeline_state.render_pass = fb->rp;

   uint8_t rast_samples = util_framebuffer_get_num_samples(state);
   /* in vulkan, gl_SampleMask needs to be explicitly ignored for sampleCount == 1 */
   if ((ctx->gfx_pipeline_state.rast_samples > 1) != (rast_samples > 1))
      ctx->dirty_shader_stages |= 1 << PIPE_SHADER_FRAGMENT;
   ctx->gfx_pipeline_state.rast_samples = rast_samples;
   ctx->gfx_pipeline_state.num_attachments = state->nr_cbufs;
   ctx->gfx_pipeline_state.dirty = true;

   /* need to start a new renderpass */
   if (zink_curr_batch(ctx)->in_rp)
      flush_batch(ctx);

   framebuffer_state_buffer_barriers_setup(ctx, &ctx->fb_state, zink_curr_batch(ctx));
}

static void
zink_set_blend_color(struct pipe_context *pctx,
                     const struct pipe_blend_color *color)
{
   struct zink_context *ctx = zink_context(pctx);
   memcpy(ctx->blend_constants, color->color, sizeof(float) * 4);
}

static void
zink_set_sample_mask(struct pipe_context *pctx, unsigned sample_mask)
{
   struct zink_context *ctx = zink_context(pctx);
   ctx->gfx_pipeline_state.sample_mask = sample_mask;
   ctx->gfx_pipeline_state.dirty = true;
}

static VkAccessFlags
access_src_flags(VkImageLayout layout)
{
   switch (layout) {
   case VK_IMAGE_LAYOUT_UNDEFINED:
      return 0;

   case VK_IMAGE_LAYOUT_GENERAL:
      return VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

   case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
   case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return VK_ACCESS_SHADER_READ_BIT;

   case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return VK_ACCESS_TRANSFER_READ_BIT;

   case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return VK_ACCESS_TRANSFER_WRITE_BIT;

   case VK_IMAGE_LAYOUT_PREINITIALIZED:
      return VK_ACCESS_HOST_WRITE_BIT;

   default:
      unreachable("unexpected layout");
   }
}

static VkAccessFlags
access_dst_flags(VkImageLayout layout)
{
   switch (layout) {
   case VK_IMAGE_LAYOUT_UNDEFINED:
      return 0;

   case VK_IMAGE_LAYOUT_GENERAL:
      return VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

   case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

   case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return VK_ACCESS_SHADER_READ_BIT;

   case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return VK_ACCESS_TRANSFER_READ_BIT;

   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
      return VK_ACCESS_SHADER_READ_BIT;

   case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return VK_ACCESS_TRANSFER_WRITE_BIT;

   default:
      unreachable("unexpected layout");
   }
}

static VkPipelineStageFlags
pipeline_dst_stage(VkImageLayout layout)
{
   switch (layout) {
   case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

   case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return VK_PIPELINE_STAGE_TRANSFER_BIT;
   case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return VK_PIPELINE_STAGE_TRANSFER_BIT;

   case VK_IMAGE_LAYOUT_GENERAL:
      return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
      return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

   default:
      return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
   }
}

#define ALL_READ_ACCESS_FLAGS \
    (VK_ACCESS_INDIRECT_COMMAND_READ_BIT | \
    VK_ACCESS_INDEX_READ_BIT | \
    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | \
    VK_ACCESS_UNIFORM_READ_BIT | \
    VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | \
    VK_ACCESS_SHADER_READ_BIT | \
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | \
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | \
    VK_ACCESS_TRANSFER_READ_BIT |\
    VK_ACCESS_HOST_READ_BIT |\
    VK_ACCESS_MEMORY_READ_BIT |\
    VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |\
    VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT |\
    VK_ACCESS_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT |\
    VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |\
    VK_ACCESS_SHADING_RATE_IMAGE_READ_BIT_NV |\
    VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT |\
    VK_ACCESS_COMMAND_PREPROCESS_READ_BIT_NV |\
    VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV |\
    VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV)


bool
zink_resource_access_is_write(VkAccessFlags flags)
{
   return (flags & ALL_READ_ACCESS_FLAGS) != flags;
}

bool
zink_resource_image_needs_barrier(struct zink_resource *res, VkImageLayout new_layout, VkAccessFlags flags, VkPipelineStageFlags pipeline)
{
   if (!pipeline)
      pipeline = pipeline_dst_stage(new_layout);
   if (!flags)
      flags = access_dst_flags(new_layout);
   return res->layout != new_layout || (res->access_stage & pipeline) != pipeline ||
          (res->access & flags) != flags ||
          zink_resource_access_is_write(res->access) ||
          zink_resource_access_is_write(flags);
}

void
zink_resource_image_barrier(struct zink_batch *batch, struct zink_resource *res,
                      VkImageLayout new_layout, VkAccessFlags flags, VkPipelineStageFlags pipeline)
{
   assert(!batch->in_rp);
   if (!pipeline)
      pipeline = pipeline_dst_stage(new_layout);
   if (!flags)
      flags = access_dst_flags(new_layout);
   if (!zink_resource_image_needs_barrier(res, new_layout, flags, pipeline))
      return;
   VkImageSubresourceRange isr = {
      res->aspect,
      0, VK_REMAINING_MIP_LEVELS,
      0, VK_REMAINING_ARRAY_LAYERS
   };

   VkImageMemoryBarrier imb = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      NULL,
      res->access ?: access_src_flags(res->layout),
      flags,
      res->layout,
      new_layout,
      VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED,
      res->image,
      isr
   };
   vkCmdPipelineBarrier(
      batch->cmdbuf,
      res->access_stage ?: VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      pipeline,
      0,
      0, NULL,
      0, NULL,
      1, &imb
   );

   res->layout = new_layout;
   res->access_stage = pipeline;
   res->access = flags;
}


VkPipelineStageFlags
zink_pipeline_flags_from_stage(VkShaderStageFlagBits stage)
{
   switch (stage) {
   case VK_SHADER_STAGE_VERTEX_BIT:
      return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
   case VK_SHADER_STAGE_FRAGMENT_BIT:
      return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
   case VK_SHADER_STAGE_GEOMETRY_BIT:
      return VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
   case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      return VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT;
   case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      return VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
   case VK_SHADER_STAGE_COMPUTE_BIT:
      return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
   default:
      unreachable("unknown shader stage bit");
   }
}

static VkPipelineStageFlags
pipeline_access_stage(VkAccessFlags flags)
{
   if (flags & (VK_ACCESS_UNIFORM_READ_BIT |
                VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_SHADER_WRITE_BIT))
      return VK_PIPELINE_STAGE_TASK_SHADER_BIT_NV |
             VK_PIPELINE_STAGE_MESH_SHADER_BIT_NV |
             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
             VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
             VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
             VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
   return VK_PIPELINE_STAGE_TRANSFER_BIT;
}

bool
zink_resource_buffer_needs_barrier(struct zink_resource *res, VkAccessFlags flags, VkPipelineStageFlags pipeline)
{
   if (!pipeline)
      pipeline = pipeline_access_stage(flags);
   return (res->access_stage & pipeline) != pipeline || (res->access & flags) != flags ||
          zink_resource_access_is_write(res->access) ||
          zink_resource_access_is_write(flags);
}

void
zink_resource_buffer_barrier(struct zink_batch *batch, struct zink_resource *res, VkAccessFlags flags, VkPipelineStageFlags pipeline)
{
   assert(!batch->in_rp);
   if (!pipeline)
      pipeline = pipeline_access_stage(flags);
   if (!zink_resource_buffer_needs_barrier(res, flags, pipeline))
      return;
   VkBufferMemoryBarrier bmb = {
      VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      NULL,
      res->access,
      flags,
      VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED,
      res->buffer,
      res->offset,
      res->base.width0
   };

   vkCmdPipelineBarrier(
      batch->cmdbuf,
      res->access_stage ? res->access_stage : pipeline_access_stage(res->access),
      pipeline,
      0,
      0, NULL,
      1, &bmb,
      0, NULL
   );
   res->access = flags;
   res->access_stage = pipeline;
}

bool
zink_resource_needs_barrier(struct zink_resource *res, VkImageLayout layout, VkAccessFlags flags, VkPipelineStageFlags pipeline)
{
   if (res->base.target == PIPE_BUFFER)
      return zink_resource_buffer_needs_barrier(res, flags, pipeline);
   return zink_resource_image_needs_barrier(res, layout, flags, pipeline);
}

void
zink_resource_barrier(struct zink_batch *batch, struct zink_resource *res, VkImageLayout layout, VkAccessFlags flags, VkPipelineStageFlags pipeline)
{
   if (res->base.target == PIPE_BUFFER)
      zink_resource_buffer_barrier(batch, res, flags, pipeline);
   else
      zink_resource_image_barrier(batch, res, layout, flags, pipeline);
}

VkShaderStageFlagBits
zink_shader_stage(enum pipe_shader_type type)
{
   VkShaderStageFlagBits stages[] = {
      [PIPE_SHADER_VERTEX] = VK_SHADER_STAGE_VERTEX_BIT,
      [PIPE_SHADER_FRAGMENT] = VK_SHADER_STAGE_FRAGMENT_BIT,
      [PIPE_SHADER_GEOMETRY] = VK_SHADER_STAGE_GEOMETRY_BIT,
      [PIPE_SHADER_TESS_CTRL] = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
      [PIPE_SHADER_TESS_EVAL] = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
      [PIPE_SHADER_COMPUTE] = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   return stages[type];
}

static uint32_t
hash_gfx_program(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct zink_shader *) * (ZINK_SHADER_COUNT));
}

static bool
equals_gfx_program(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(struct zink_shader *) * (ZINK_SHADER_COUNT)) == 0;
}

static void
zink_flush(struct pipe_context *pctx,
           struct pipe_fence_handle **pfence,
           enum pipe_flush_flags flags)
{
   struct zink_context *ctx = zink_context(pctx);

   struct zink_batch *batch = zink_curr_batch(ctx);
   flush_batch(ctx);

   if (zink_screen(pctx->screen)->info.have_EXT_transform_feedback && ctx->num_so_targets)
      ctx->dirty_so_targets = true;

   if (pfence)
      zink_fence_reference(zink_screen(pctx->screen),
                           (struct zink_fence **)pfence,
                           batch->fence);

   /* HACK:
    * For some strange reason, we need to finish before presenting, or else
    * we start rendering on top of the back-buffer for the next frame. This
    * seems like a bug in the DRI-driver to me, because we really should
    * be properly protected by fences here, and the back-buffer should
    * either be swapped with the front-buffer, or blitted from. But for
    * some strange reason, neither of these things happen.
    */
   if (flags & PIPE_FLUSH_END_OF_FRAME)
      pctx->screen->fence_finish(pctx->screen, pctx,
                                 (struct pipe_fence_handle *)batch->fence,
                                 PIPE_TIMEOUT_INFINITE);
}

void
zink_fence_wait(struct pipe_context *pctx)
{
   struct pipe_fence_handle *fence = NULL;
   pctx->flush(pctx, &fence, PIPE_FLUSH_HINT_FINISH);
   if (fence) {
      pctx->screen->fence_finish(pctx->screen, NULL, fence,
                                 PIPE_TIMEOUT_INFINITE);
      pctx->screen->fence_reference(pctx->screen, &fence, NULL);
   }
}

void
zink_wait_on_batch(struct zink_context *ctx, int batch_id)
{
   if (batch_id >= 0) {
      struct zink_batch *batch = batch_id == ZINK_COMPUTE_BATCH_ID ? &ctx->compute_batch : &ctx->batches[batch_id];
      if (batch != zink_curr_batch(ctx)) {
         if (!batch->fence) { // this is the compute batch
            zink_end_batch(ctx, batch);
            zink_start_batch(ctx, batch);
         } else
            ctx->base.screen->fence_finish(ctx->base.screen, NULL, (struct pipe_fence_handle*)batch->fence,
                                       PIPE_TIMEOUT_INFINITE);
         return;
      }
   }
   zink_fence_wait(&ctx->base);
}

static void
zink_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
   struct zink_context *ctx = zink_context(pctx);
   if (zink_curr_batch(ctx)->has_draw)
      pctx->flush(pctx, NULL, 0);
   zink_flush_compute(ctx);
}

static void
zink_memory_barrier(struct pipe_context *pctx, unsigned flags)
{
   struct zink_context *ctx = zink_context(pctx);
   VkAccessFlags sflags = 0;
   VkAccessFlags dflags = 0;
   VkPipelineStageFlags src = 0;
   VkPipelineStageFlags dst = 0;

   VkPipelineStageFlags all_flags = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                    VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                    VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                                    VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

   if (flags == PIPE_BARRIER_ALL) {
      sflags = dflags = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      src = dst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   } else {
      while (flags) {
         unsigned flag = u_bit_scan(&flags);
         
         switch (1 << flag) {
         case PIPE_BARRIER_MAPPED_BUFFER:
            sflags |= VK_ACCESS_SHADER_WRITE_BIT;
            dflags |= VK_ACCESS_TRANSFER_READ_BIT;
            break;
         case PIPE_BARRIER_SHADER_BUFFER:
            sflags |= VK_ACCESS_SHADER_WRITE_BIT;
            dflags |= VK_ACCESS_SHADER_READ_BIT;
            src |= all_flags;
            dst |= all_flags;
            break;
         case PIPE_BARRIER_QUERY_BUFFER:
            sflags |= VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            dflags |= VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            break;
         case PIPE_BARRIER_VERTEX_BUFFER:
            sflags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            dflags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            src |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            dst |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            break;
         case PIPE_BARRIER_INDEX_BUFFER:
            sflags |= VK_ACCESS_INDEX_READ_BIT;
            dflags |= VK_ACCESS_INDEX_READ_BIT;
            src |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            dst |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            break;
         case PIPE_BARRIER_CONSTANT_BUFFER:
            sflags |= VK_ACCESS_UNIFORM_READ_BIT;
            dflags |= VK_ACCESS_UNIFORM_READ_BIT;
            src |= all_flags;
            dst |= all_flags;
            break;
         case PIPE_BARRIER_INDIRECT_BUFFER:
            sflags |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            dflags |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            src |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
            dst |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
            break;
         case PIPE_BARRIER_TEXTURE:
            sflags |= VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            dflags |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            src |= all_flags;
            dst |= all_flags;
            break;
         case PIPE_BARRIER_IMAGE:
            sflags |= VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            dflags |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
            src |= all_flags;
            dst |= all_flags;
            break;
         case PIPE_BARRIER_FRAMEBUFFER:
            sflags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dflags |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            src |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dst |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
         case PIPE_BARRIER_STREAMOUT_BUFFER:
            sflags |= VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
            dflags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            src |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
            dst |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            break;
         case PIPE_BARRIER_GLOBAL_BUFFER:
            debug_printf("zink: unhandled barrier flag %u\n", flag);
            break;
         case PIPE_BARRIER_UPDATE_BUFFER:
            sflags |= VK_ACCESS_TRANSFER_WRITE_BIT;
            dflags |= VK_ACCESS_TRANSFER_READ_BIT;
            src |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            dst |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
         case PIPE_BARRIER_UPDATE_TEXTURE:
            sflags |= VK_ACCESS_TRANSFER_WRITE_BIT;
            dflags |= VK_ACCESS_TRANSFER_READ_BIT;
            src |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            dst |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
         }
      }
   }
   VkMemoryBarrier b = {};
   b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
   /* TODO: these are all probably wrong */
   b.srcAccessMask = sflags;
   b.dstAccessMask = dflags;

   struct zink_batch *batch = zink_curr_batch(ctx);
   if (batch->has_draw) {
      zink_end_render_pass(ctx, batch);

      /* this should be the only call needed */
      vkCmdPipelineBarrier(batch->cmdbuf, src, dst, 0, 0, &b, 0, NULL, 0, NULL);
      flush_batch(ctx);
   }
   batch = &ctx->compute_batch;
   if (batch->has_draw) {
      /* this should be the only call needed */
      vkCmdPipelineBarrier(batch->cmdbuf, src, dst, 0, 0, &b, 0, NULL, 0, NULL);
      zink_end_batch(ctx, batch);
      zink_start_batch(ctx, batch);
   }
}

static void
zink_flush_resource(struct pipe_context *pipe,
                    struct pipe_resource *resource)
{
}

void
zink_copy_buffer(struct zink_context *ctx, struct zink_batch *batch, struct zink_resource *dst, struct zink_resource *src,
                 unsigned dst_offset, unsigned src_offset, unsigned size)
{
   VkBufferCopy region;
   region.srcOffset = src_offset;
   region.dstOffset = dst_offset;
   region.size = size;

   if (!batch)
      batch = zink_batch_no_rp(ctx);
   assert(!batch->in_rp);
   zink_batch_reference_resource_rw(batch, src, false);
   zink_batch_reference_resource_rw(batch, dst, true);
   util_range_add(&dst->base, &dst->valid_buffer_range, dst_offset, dst_offset + size);
   zink_resource_buffer_barrier(batch, src, VK_ACCESS_TRANSFER_READ_BIT, 0);
   zink_resource_buffer_barrier(batch, dst, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
   vkCmdCopyBuffer(batch->cmdbuf, src->buffer, dst->buffer, 1, &region);
}

static void
zink_resource_copy_region(struct pipe_context *pctx,
                          struct pipe_resource *pdst,
                          unsigned dst_level, unsigned dstx, unsigned dsty, unsigned dstz,
                          struct pipe_resource *psrc,
                          unsigned src_level, const struct pipe_box *src_box)
{
   struct zink_resource *dst = zink_resource(pdst);
   struct zink_resource *src = zink_resource(psrc);
   struct zink_context *ctx = zink_context(pctx);
   if (dst->base.target != PIPE_BUFFER && src->base.target != PIPE_BUFFER) {
      VkImageCopy region = {};
      if (util_format_get_num_planes(src->base.format) == 1 &&
          util_format_get_num_planes(dst->base.format) == 1) {
      /* If neither the calling command’s srcImage nor the calling command’s dstImage
       * has a multi-planar image format then the aspectMask member of srcSubresource
       * and dstSubresource must match
       *
       * -VkImageCopy spec
       */
         assert(src->aspect == dst->aspect);
      } else
         unreachable("planar formats not yet handled");

      region.srcSubresource.aspectMask = src->aspect;
      region.srcSubresource.mipLevel = src_level;
      region.srcSubresource.layerCount = 1;
      if (src->base.array_size > 1) {
         region.srcSubresource.baseArrayLayer = src_box->z;
         region.srcSubresource.layerCount = src_box->depth;
         region.extent.depth = 1;
      } else {
         region.srcOffset.z = src_box->z;
         region.srcSubresource.layerCount = 1;
         region.extent.depth = src_box->depth;
      }

      region.srcOffset.x = src_box->x;
      region.srcOffset.y = src_box->y;

      region.dstSubresource.aspectMask = dst->aspect;
      region.dstSubresource.mipLevel = dst_level;
      if (dst->base.array_size > 1) {
         region.dstSubresource.baseArrayLayer = dstz;
         region.dstSubresource.layerCount = src_box->depth;
      } else {
         region.dstOffset.z = dstz;
         region.dstSubresource.layerCount = 1;
      }

      region.dstOffset.x = dstx;
      region.dstOffset.y = dsty;
      region.extent.width = src_box->width;
      region.extent.height = src_box->height;

      struct zink_batch *batch = zink_batch_no_rp(ctx);
      zink_batch_reference_resource_rw(batch, src, false);
      zink_batch_reference_resource_rw(batch, dst, true);

      zink_resource_setup_transfer_layouts(batch, src, dst);
      vkCmdCopyImage(batch->cmdbuf, src->image, src->layout,
                     dst->image, dst->layout,
                     1, &region);
   } else if (dst->base.target == PIPE_BUFFER &&
              src->base.target == PIPE_BUFFER) {
      zink_copy_buffer(ctx, NULL, dst, src, dstx, src_box->x, src_box->width);
   } else
      debug_printf("zink: TODO resource copy\n");
      //util_range_add(dst, &dst->valid_buffer_range, dstx, dstx + src_box->width);
}

static struct pipe_stream_output_target *
zink_create_stream_output_target(struct pipe_context *pctx,
                                 struct pipe_resource *pres,
                                 unsigned buffer_offset,
                                 unsigned buffer_size)
{
   struct zink_so_target *t;
   t = CALLOC_STRUCT(zink_so_target);
   if (!t)
      return NULL;

   /* using PIPE_BIND_CUSTOM here lets us create a custom pipe buffer resource,
    * which allows us to differentiate and use VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT
    * as we must for this case
    */
   t->counter_buffer = pipe_buffer_create(pctx->screen, PIPE_BIND_STREAM_OUTPUT | PIPE_BIND_CUSTOM, PIPE_USAGE_DEFAULT, 4);
   if (!t->counter_buffer) {
      FREE(t);
      return NULL;
   }

   t->base.reference.count = 1;
   t->base.context = pctx;
   pipe_resource_reference(&t->base.buffer, pres);
   t->base.buffer_offset = buffer_offset;
   t->base.buffer_size = buffer_size;

   struct zink_resource *res = zink_resource(pres);
   util_range_add(pres, &res->valid_buffer_range, buffer_offset,
                  buffer_offset + buffer_size);
   return &t->base;
}

static void
zink_stream_output_target_destroy(struct pipe_context *pctx,
                                  struct pipe_stream_output_target *psot)
{
   struct zink_so_target *t = (struct zink_so_target *)psot;
   pipe_resource_reference(&t->counter_buffer, NULL);
   pipe_resource_reference(&t->base.buffer, NULL);
   FREE(t);
}

static void
zink_set_stream_output_targets(struct pipe_context *pctx,
                               unsigned num_targets,
                               struct pipe_stream_output_target **targets,
                               const unsigned *offsets)
{
   struct zink_context *ctx = zink_context(pctx);

   if (num_targets == 0) {
      for (unsigned i = 0; i < ctx->num_so_targets; i++)
         pipe_so_target_reference(&ctx->so_targets[i], NULL);
      ctx->num_so_targets = 0;
   } else {
      for (unsigned i = 0; i < num_targets; i++) {
         struct zink_so_target *t = zink_so_target(targets[i]);
         pipe_so_target_reference(&ctx->so_targets[i], targets[i]);
         if (!t)
            continue;
         struct zink_resource *res = zink_resource(t->counter_buffer);
         if (offsets[0] == (unsigned)-1)
            ctx->xfb_barrier |= zink_resource_buffer_needs_barrier(res,
                                                                   VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT,
                                                                   VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
         else
            ctx->xfb_barrier |= zink_resource_buffer_needs_barrier(res,
                                                                   VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
                                                                   VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT);
      }
      for (unsigned i = num_targets; i < ctx->num_so_targets; i++)
         pipe_so_target_reference(&ctx->so_targets[i], NULL);
      ctx->num_so_targets = num_targets;

      /* TODO: possibly avoid rebinding on resume if resuming from same buffers? */
      ctx->dirty_so_targets = true;
   }
}

static bool
init_batch(struct zink_context *ctx, struct zink_batch *batch, unsigned idx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkCommandPoolCreateInfo cpci = {};
   cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cpci.queueFamilyIndex = screen->gfx_queue;
   cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   if (vkCreateCommandPool(screen->dev, &cpci, NULL, &batch->cmdpool) != VK_SUCCESS)
      return false;

   VkCommandBufferAllocateInfo cbai = {};
   cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cbai.commandPool = batch->cmdpool;
   cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbai.commandBufferCount = 1;

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
   dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
   dpci.maxSets = ZINK_BATCH_DESC_SIZE;

   if (vkAllocateCommandBuffers(screen->dev, &cbai, &batch->cmdbuf) != VK_SUCCESS)
      return false;

   batch->resources = _mesa_pointer_set_create(NULL);
   batch->sampler_views = _mesa_pointer_set_create(NULL);
   batch->programs = _mesa_pointer_set_create(NULL);
   batch->surfaces = _mesa_pointer_set_create(NULL);

   if (!batch->resources || !batch->sampler_views ||
       !batch->programs || !batch->surfaces)
      return false;

   util_dynarray_init(&batch->zombie_samplers, NULL);
   util_dynarray_init(&batch->persistent_resources, NULL);

   if (vkCreateDescriptorPool(screen->dev, &dpci, 0,
                              &batch->descpool) != VK_SUCCESS)
      return false;

   batch->batch_id = idx;
   return true;
}

struct pipe_context *
zink_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct zink_screen *screen = zink_screen(pscreen);
   struct zink_context *ctx = CALLOC_STRUCT(zink_context);
   if (!ctx)
      goto fail;

   ctx->gfx_pipeline_state.dirty = true;
   ctx->compute_pipeline_state.dirty = true;

   ctx->base.screen = pscreen;
   ctx->base.priv = priv;

   ctx->base.destroy = zink_context_destroy;
   ctx->base.get_device_reset_status = zink_get_device_reset_status;
   ctx->base.set_device_reset_callback = zink_set_device_reset_callback;

   zink_context_state_init(&ctx->base);

   ctx->base.create_sampler_state = zink_create_sampler_state;
   ctx->base.bind_sampler_states = zink_bind_sampler_states;
   ctx->base.delete_sampler_state = zink_delete_sampler_state;

   ctx->base.create_sampler_view = zink_create_sampler_view;
   ctx->base.set_sampler_views = zink_set_sampler_views;
   ctx->base.sampler_view_destroy = zink_sampler_view_destroy;
   ctx->base.get_sample_position = zink_get_sample_position;

   zink_program_init(ctx);

   ctx->base.set_polygon_stipple = zink_set_polygon_stipple;
   ctx->base.set_vertex_buffers = zink_set_vertex_buffers;
   ctx->base.set_viewport_states = zink_set_viewport_states;
   ctx->base.set_scissor_states = zink_set_scissor_states;
   ctx->base.set_constant_buffer = zink_set_constant_buffer;
   ctx->base.set_shader_buffers = zink_set_shader_buffers;
   ctx->base.set_shader_images = zink_set_shader_images;
   ctx->base.set_framebuffer_state = zink_set_framebuffer_state;
   ctx->base.set_stencil_ref = zink_set_stencil_ref;
   ctx->base.set_clip_state = zink_set_clip_state;
   ctx->base.set_blend_color = zink_set_blend_color;
   ctx->base.set_tess_state = zink_set_tess_state;

   ctx->base.set_sample_mask = zink_set_sample_mask;

   ctx->base.clear = zink_clear;
   ctx->base.clear_texture = zink_clear_texture;

   ctx->base.draw_vbo = zink_draw_vbo;
   ctx->base.launch_grid = zink_launch_grid;
   ctx->base.flush = zink_flush;
   ctx->base.memory_barrier = zink_memory_barrier;
   ctx->base.texture_barrier = zink_texture_barrier;

   ctx->base.resource_copy_region = zink_resource_copy_region;
   ctx->base.blit = zink_blit;
   ctx->base.create_stream_output_target = zink_create_stream_output_target;
   ctx->base.stream_output_target_destroy = zink_stream_output_target_destroy;

   ctx->base.set_stream_output_targets = zink_set_stream_output_targets;
   ctx->base.flush_resource = zink_flush_resource;
   zink_context_surface_init(&ctx->base);
   zink_context_resource_init(&ctx->base);
   zink_context_query_init(&ctx->base);

   slab_create_child(&ctx->transfer_pool, &screen->transfer_pool);

   ctx->base.stream_uploader = u_upload_create_default(&ctx->base);
   ctx->base.const_uploader = ctx->base.stream_uploader;

   int prim_hwsupport = 1 << PIPE_PRIM_POINTS |
                        1 << PIPE_PRIM_LINES |
                        1 << PIPE_PRIM_LINE_STRIP |
                        1 << PIPE_PRIM_TRIANGLES |
                        1 << PIPE_PRIM_TRIANGLE_STRIP;
   if (screen->have_triangle_fans)
      prim_hwsupport |= 1 << PIPE_PRIM_TRIANGLE_FAN;

   ctx->primconvert = util_primconvert_create(&ctx->base, prim_hwsupport);
   if (!ctx->primconvert)
      goto fail;

   ctx->blitter = util_blitter_create(&ctx->base);
   if (!ctx->blitter)
      goto fail;

   for (int i = 0; i < ARRAY_SIZE(ctx->batches); ++i) {
      if (!init_batch(ctx, &ctx->batches[i], i))
         goto fail;
   }

   if (!init_batch(ctx, &ctx->compute_batch, ZINK_COMPUTE_BATCH_ID))
      goto fail;
   zink_start_batch(ctx, &ctx->compute_batch);

   vkGetDeviceQueue(screen->dev, screen->gfx_queue, 0, &ctx->queue);

   ctx->program_cache = _mesa_hash_table_create(NULL,
                                                hash_gfx_program,
                                                equals_gfx_program);
   ctx->compute_program_cache = _mesa_hash_table_create(NULL,
                                                _mesa_hash_uint,
                                                _mesa_key_uint_equal);
   ctx->render_pass_cache = _mesa_hash_table_create(NULL,
                                                    hash_render_pass_state,
                                                    equals_render_pass_state);
   if (!ctx->program_cache || !ctx->compute_program_cache || !ctx->render_pass_cache)
      goto fail;

   const uint8_t data[] = { 0 };
   ctx->dummy_vertex_buffer = pipe_buffer_create_with_data(&ctx->base,
      PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_IMMUTABLE, sizeof(data), data);
   if (!ctx->dummy_vertex_buffer)
      goto fail;
   ctx->dummy_xfb_buffer = pipe_buffer_create_with_data(&ctx->base,
      PIPE_BIND_STREAM_OUTPUT, PIPE_USAGE_DEFAULT, sizeof(data), data);
   if (!ctx->dummy_xfb_buffer)
      goto fail;

   /* start the first batch */
   zink_start_batch(ctx, zink_curr_batch(ctx));

   return &ctx->base;

fail:
   if (ctx)
      zink_context_destroy(&ctx->base);
   return NULL;
}

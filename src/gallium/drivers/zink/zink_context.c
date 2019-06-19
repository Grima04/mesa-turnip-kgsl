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
#include "zink_pipeline.h"
#include "zink_program.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_state.h"
#include "zink_surface.h"

#include "indices/u_primconvert.h"
#include "util/u_blitter.h"
#include "util/u_debug.h"
#include "util/u_format.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"

#include "nir.h"

#include "util/u_memory.h"
#include "util/u_prim.h"
#include "util/u_upload_mgr.h"

static void
zink_context_destroy(struct pipe_context *pctx)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);

   if (vkQueueWaitIdle(ctx->queue) != VK_SUCCESS)
      debug_printf("vkQueueWaitIdle failed\n");

   for (int i = 0; i < ARRAY_SIZE(ctx->batches); ++i)
      vkFreeCommandBuffers(screen->dev, ctx->cmdpool, 1, &ctx->batches[i].cmdbuf);
   vkDestroyCommandPool(screen->dev, ctx->cmdpool, NULL);

   util_primconvert_destroy(ctx->primconvert);
   u_upload_destroy(pctx->stream_uploader);
   slab_destroy_child(&ctx->transfer_pool);
   util_blitter_destroy(ctx->blitter);
   FREE(ctx);
}

static VkFilter
filter(enum pipe_tex_filter filter)
{
   switch (filter) {
   case PIPE_TEX_FILTER_NEAREST: return VK_FILTER_NEAREST;
   case PIPE_TEX_FILTER_LINEAR: return VK_FILTER_LINEAR;
   }
   unreachable("unexpected filter");
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
   case PIPE_TEX_WRAP_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; /* not technically correct, but kinda works */
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
   case PIPE_TEX_WRAP_MIRROR_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
   case PIPE_TEX_WRAP_MIRROR_CLAMP: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; /* not technically correct, but kinda works */
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; /* not technically correct, but kinda works */
   }
   unreachable("unexpected wrap");
}

static void *
zink_create_sampler_state(struct pipe_context *pctx,
                          const struct pipe_sampler_state *state)
{
   struct zink_screen *screen = zink_screen(pctx->screen);

   VkSamplerCreateInfo sci = {};
   sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
   sci.magFilter = filter(state->mag_img_filter);
   sci.minFilter = filter(state->min_img_filter);

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
   sci.compareOp = VK_COMPARE_OP_NEVER; // TODO
   sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; // TODO
   sci.unnormalizedCoordinates = !state->normalized_coords;

   if (state->max_anisotropy > 1) {
      sci.maxAnisotropy = state->max_anisotropy;
      sci.anisotropyEnable = VK_TRUE;
   }

   VkSampler sampler;
   VkResult err = vkCreateSampler(screen->dev, &sci, NULL, &sampler);
   if (err != VK_SUCCESS)
      return NULL;

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
   for (unsigned i = 0; i < num_samplers; ++i)
      ctx->samplers[shader][start_slot + i] = (VkSampler)samplers[i];
}

static void
zink_delete_sampler_state(struct pipe_context *pctx,
                          void *sampler_state)
{
   struct zink_batch *batch = zink_context_curr_batch(zink_context(pctx));
   util_dynarray_append(&batch->zombie_samplers,
                        VkSampler, sampler_state);
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
   case PIPE_TEXTURE_RECT: return VK_IMAGE_VIEW_TYPE_2D; /* not sure */
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

static struct pipe_sampler_view *
zink_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *pres,
                         const struct pipe_sampler_view *state)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_resource *res = zink_resource(pres);
   struct zink_sampler_view *sampler_view = CALLOC_STRUCT(zink_sampler_view);

   sampler_view->base = *state;
   sampler_view->base.texture = NULL;
   pipe_resource_reference(&sampler_view->base.texture, pres);
   sampler_view->base.reference.count = 1;
   sampler_view->base.context = pctx;

   VkImageViewCreateInfo ivci = {};
   ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   ivci.image = res->image;
   ivci.viewType = image_view_type(state->target);
   ivci.format = zink_get_format(state->format);
   ivci.components.r = component_mapping(state->swizzle_r);
   ivci.components.g = component_mapping(state->swizzle_g);
   ivci.components.b = component_mapping(state->swizzle_b);
   ivci.components.a = component_mapping(state->swizzle_a);
   ivci.subresourceRange.aspectMask = zink_aspect_from_format(state->format);
   ivci.subresourceRange.baseMipLevel = state->u.tex.first_level;
   ivci.subresourceRange.baseArrayLayer = state->u.tex.first_layer;
   ivci.subresourceRange.levelCount = state->u.tex.last_level - state->u.tex.first_level + 1;
   ivci.subresourceRange.layerCount = state->u.tex.last_layer - state->u.tex.first_layer + 1;

   VkResult err = vkCreateImageView(screen->dev, &ivci, NULL, &sampler_view->image_view);
   if (err != VK_SUCCESS) {
      FREE(sampler_view);
      return NULL;
   }

   return &sampler_view->base;
}

static void
zink_destroy_sampler_view(struct pipe_context *pctx,
                          struct pipe_sampler_view *pview)
{
   struct zink_sampler_view *view = zink_sampler_view(pview);
   vkDestroyImageView(zink_screen(pctx->screen)->dev, view->image_view, NULL);
   FREE(view);
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

   return zink_compile_nir(zink_screen(pctx->screen), nir);
}

static void
bind_stage(struct zink_context *ctx, enum pipe_shader_type stage,
           struct zink_shader *shader)
{
   assert(stage < PIPE_SHADER_COMPUTE);
   ctx->gfx_stages[stage] = shader;
   ctx->dirty |= ZINK_DIRTY_PROGRAM;
}

static void
zink_bind_vs_state(struct pipe_context *pctx,
                   void *cso)
{
   bind_stage(zink_context(pctx), PIPE_SHADER_VERTEX, cso);
}

static void
zink_delete_vs_state(struct pipe_context *pctx,
                     void *cso)
{
   zink_shader_free(zink_screen(pctx->screen), cso);
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

   return zink_compile_nir(zink_screen(pctx->screen), nir);
}

static void
zink_bind_fs_state(struct pipe_context *pctx,
                   void *cso)
{
   bind_stage(zink_context(pctx), PIPE_SHADER_FRAGMENT, cso);
}

static void
zink_delete_fs_state(struct pipe_context *pctx,
                     void *cso)
{
   zink_shader_free(zink_screen(pctx->screen), cso);
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
                        const struct pipe_vertex_buffer *buffers)
{
   struct zink_context *ctx = zink_context(pctx);

   if (buffers) {
      for (int i = 0; i < num_buffers; ++i) {
         const struct pipe_vertex_buffer *vb = buffers + i;
         ctx->gfx_pipeline_state.bindings[start_slot + i].stride = vb->stride;
      }
   }

   util_set_vertex_buffers_mask(ctx->buffers, &ctx->buffers_enabled_mask,
                                buffers, start_slot, num_buffers);
}

static void
zink_set_viewport_states(struct pipe_context *pctx,
                         unsigned start_slot,
                         unsigned num_viewports,
                         const struct pipe_viewport_state *state)
{
   struct zink_context *ctx = zink_context(pctx);

   for (unsigned i = 0; i < num_viewports; ++i) {
      VkViewport viewport = {
         state[i].translate[0] - state[i].scale[0],
         state[i].translate[1] - state[i].scale[1],
         state[i].scale[0] * 2,
         state[i].scale[1] * 2,
         state[i].translate[2] - state[i].scale[2],
         state[i].translate[2] + state[i].scale[2]
      };
      ctx->viewports[start_slot + i] = viewport;
   }
   ctx->num_viewports = start_slot + num_viewports;
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
      ctx->scissors[start_slot + i] = scissor;
   }
   ctx->num_scissors = start_slot + num_scissors;
}

static void
zink_set_constant_buffer(struct pipe_context *pctx,
                         enum pipe_shader_type shader, uint index,
                         const struct pipe_constant_buffer *cb)
{
   struct zink_context *ctx = zink_context(pctx);

   if (cb) {
      struct pipe_resource *buffer = cb->buffer;
      unsigned offset = cb->buffer_offset;
      if (cb->user_buffer)
         u_upload_data(ctx->base.const_uploader, 0, cb->buffer_size, 64,
                       cb->user_buffer, &offset, &buffer);

      pipe_resource_reference(&ctx->ubos[shader][index].buffer, buffer);
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
zink_set_sampler_views(struct pipe_context *pctx,
                       enum pipe_shader_type shader_type,
                       unsigned start_slot,
                       unsigned num_views,
                       struct pipe_sampler_view **views)
{
   struct zink_context *ctx = zink_context(pctx);
   assert(views);
   for (unsigned i = 0; i < num_views; ++i) {
      pipe_sampler_view_reference(
         &ctx->image_views[shader_type][start_slot + i],
         views[i]);
   }
}

static void
zink_set_stencil_ref(struct pipe_context *pctx,
                     const struct pipe_stencil_ref *ref)
{
   struct zink_context *ctx = zink_context(pctx);
   ctx->stencil_ref[0] = ref->ref_value[0];
   ctx->stencil_ref[1] = ref->ref_value[1];
}

static void
zink_set_clip_state(struct pipe_context *pctx,
                    const struct pipe_clip_state *pcs)
{
}

static struct zink_render_pass *
get_render_pass(struct zink_context *ctx)
{
   const struct pipe_framebuffer_state *fb = &ctx->fb_state;
   struct zink_render_pass_state state;

   for (int i = 0; i < fb->nr_cbufs; i++) {
      struct zink_resource *cbuf = zink_resource(fb->cbufs[i]->texture);
      state.rts[i].format = cbuf->format;
   }
   state.num_cbufs = fb->nr_cbufs;

   if (fb->zsbuf) {
      struct zink_resource *zsbuf = zink_resource(fb->zsbuf->texture);
      state.rts[fb->nr_cbufs].format = zsbuf->format;
   }
   state.have_zsbuf = fb->zsbuf != NULL;

   // TODO: cache instead!
   return zink_create_render_pass(zink_screen(ctx->base.screen), &state);
}

static struct zink_framebuffer *
get_framebuffer(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_render_pass *rp = get_render_pass(ctx);
   // TODO: cache!
   struct zink_framebuffer *ret = zink_create_framebuffer(screen,
                                                          &ctx->fb_state,
                                                          rp);
   zink_render_pass_reference(screen, &rp, NULL);
   return ret;
}

static void
end_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   if (batch->rp)
      vkCmdEndRenderPass(batch->cmdbuf);

   zink_end_cmdbuf(ctx, batch);
}

void
zink_begin_render_pass(struct zink_context *ctx, struct zink_batch *batch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   assert(batch == zink_context_curr_batch(ctx));

   VkRenderPassBeginInfo rpbi = {};
   rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   rpbi.renderPass = ctx->gfx_pipeline_state.render_pass->render_pass;
   rpbi.renderArea.offset.x = 0;
   rpbi.renderArea.offset.y = 0;
   rpbi.renderArea.extent.width = ctx->fb_state.width;
   rpbi.renderArea.extent.height = ctx->fb_state.height;
   rpbi.clearValueCount = 0;
   rpbi.pClearValues = NULL;
   rpbi.framebuffer = ctx->framebuffer->fb;

   assert(ctx->gfx_pipeline_state.render_pass && ctx->framebuffer);
   assert(!batch->rp || batch->rp == ctx->gfx_pipeline_state.render_pass);
   assert(!batch->fb || batch->fb == ctx->framebuffer);

   zink_render_pass_reference(screen, &batch->rp, ctx->gfx_pipeline_state.render_pass);
   zink_framebuffer_reference(screen, &batch->fb, ctx->framebuffer);

   vkCmdBeginRenderPass(batch->cmdbuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
}

static void
flush_batch(struct zink_context *ctx)
{
   end_batch(ctx, zink_context_curr_batch(ctx));

   ctx->curr_batch++;
   if (ctx->curr_batch == ARRAY_SIZE(ctx->batches))
      ctx->curr_batch = 0;

   struct zink_batch *batch = zink_context_curr_batch(ctx);
   zink_start_cmdbuf(ctx, batch);
}

static void
zink_set_framebuffer_state(struct pipe_context *pctx,
                           const struct pipe_framebuffer_state *state)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);

   util_copy_framebuffer_state(&ctx->fb_state, state);

   struct zink_framebuffer *fb = get_framebuffer(ctx);
   zink_framebuffer_reference(screen, &ctx->framebuffer, fb);
   zink_render_pass_reference(screen, &ctx->gfx_pipeline_state.render_pass, fb->rp);
   zink_framebuffer_reference(screen, &fb, NULL);

   ctx->gfx_pipeline_state.num_attachments = state->nr_cbufs;

   flush_batch(ctx);
   struct zink_batch *batch = zink_context_curr_batch(ctx);

   for (int i = 0; i < state->nr_cbufs; i++) {
      struct zink_resource *res = zink_resource(state->cbufs[i]->texture);
      if (res->layout != VK_IMAGE_LAYOUT_GENERAL &&
          res->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
         zink_resource_barrier(batch->cmdbuf, res, res->aspect,
                               VK_IMAGE_LAYOUT_GENERAL);
   }

   if (state->zsbuf) {
      struct zink_resource *res = zink_resource(state->zsbuf->texture);
      if (res->layout != VK_IMAGE_LAYOUT_GENERAL &&
          res->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
         zink_resource_barrier(batch->cmdbuf, res, res->aspect,
                               VK_IMAGE_LAYOUT_GENERAL);
   }
}

static void
zink_set_active_query_state(struct pipe_context *pctx, bool enable)
{
}

static void
zink_set_blend_color(struct pipe_context *pctx,
                     const struct pipe_blend_color *color)
{
   struct zink_context *ctx = zink_context(pctx);
   memcpy(ctx->blend_constants, color->color, sizeof(float) * 4);
}

static VkAccessFlags
access_flags(VkImageLayout layout)
{
   switch (layout) {
   case VK_IMAGE_LAYOUT_UNDEFINED:
   case VK_IMAGE_LAYOUT_GENERAL:
      return 0;

   case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

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

void
zink_resource_barrier(VkCommandBuffer cmdbuf, struct zink_resource *res,
                      VkImageAspectFlags aspect, VkImageLayout new_layout)
{
   VkImageSubresourceRange isr = {
      aspect,
      0, VK_REMAINING_MIP_LEVELS,
      0, VK_REMAINING_ARRAY_LAYERS
   };

   VkImageMemoryBarrier imb = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      NULL,
      access_flags(res->layout),
      access_flags(new_layout),
      res->layout,
      new_layout,
      VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED,
      res->image,
      isr
   };
   vkCmdPipelineBarrier(
      cmdbuf,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
      0,
      0, NULL,
      0, NULL,
      1, &imb
   );

   res->layout = new_layout;
}

static void
zink_clear(struct pipe_context *pctx,
           unsigned buffers,
           const union pipe_color_union *pcolor,
           double depth, unsigned stencil)
{
   struct zink_context *ctx = zink_context(pctx);
   struct pipe_framebuffer_state *fb = &ctx->fb_state;

   struct zink_batch *batch = zink_context_curr_batch(ctx);

   VkClearAttachment attachments[1 + PIPE_MAX_COLOR_BUFS];
   int num_attachments = 0;

   if (buffers & PIPE_CLEAR_COLOR) {
      VkClearColorValue color;
      color.float32[0] = pcolor->f[0];
      color.float32[1] = pcolor->f[1];
      color.float32[2] = pcolor->f[2];
      color.float32[3] = pcolor->f[3];

      for (unsigned i = 0; i < fb->nr_cbufs; i++) {
         if (!(buffers & (PIPE_CLEAR_COLOR0 << i)) || !fb->cbufs[i])
            continue;

         attachments[num_attachments].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         attachments[num_attachments].colorAttachment = i;
         attachments[num_attachments].clearValue.color = color;
         ++num_attachments;
      }
   }

   if (buffers & PIPE_CLEAR_DEPTHSTENCIL && fb->zsbuf) {
      VkImageAspectFlags aspect = 0;
      if (buffers & PIPE_CLEAR_DEPTH)
         aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (buffers & PIPE_CLEAR_STENCIL)
         aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

      attachments[num_attachments].aspectMask = aspect;
      attachments[num_attachments].clearValue.depthStencil.depth = depth;
      attachments[num_attachments].clearValue.depthStencil.stencil = stencil;
      ++num_attachments;
   }

   unsigned num_layers = util_framebuffer_get_num_layers(fb);
   VkClearRect rects[PIPE_MAX_VIEWPORTS];
   uint32_t num_rects;
   if (ctx->num_scissors) {
      for (unsigned i = 0 ; i < ctx->num_scissors; ++i) {
         rects[i].rect = ctx->scissors[i];
         rects[i].baseArrayLayer = 0;
         rects[i].layerCount = num_layers;
      }
      num_rects = ctx->num_scissors;
   } else {
      rects[0].rect.offset.x = 0;
      rects[0].rect.offset.y = 0;
      rects[0].rect.extent.width = fb->width;
      rects[0].rect.extent.height = fb->height;
      rects[0].baseArrayLayer = 0;
      rects[0].layerCount = num_layers;
      num_rects = 1;
   }

   if (!batch->rp)
      zink_begin_render_pass(ctx, batch);

   vkCmdClearAttachments(batch->cmdbuf,
                         num_attachments, attachments,
                         num_rects, rects);
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

static VkDescriptorSet
allocate_descriptor_set(struct zink_context *ctx, VkDescriptorSetLayout dsl)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetAllocateInfo dsai;
   memset((void *)&dsai, 0, sizeof(dsai));
   dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   dsai.pNext = NULL;
   dsai.descriptorPool = ctx->descpool;
   dsai.descriptorSetCount = 1;
   dsai.pSetLayouts = &dsl;

   VkDescriptorSet desc_set;
   if (vkAllocateDescriptorSets(screen->dev, &dsai, &desc_set) != VK_SUCCESS) {

      /* if we run out of descriptor sets we either need to create a bunch
       * more... or flush and wait. For simplicity, let's flush for now.
       */
      struct pipe_fence_handle *fence = NULL;
      ctx->base.flush(&ctx->base, &fence, 0);
      ctx->base.screen->fence_finish(ctx->base.screen, &ctx->base, fence,
                                     PIPE_TIMEOUT_INFINITE);

      if (vkResetDescriptorPool(screen->dev, ctx->descpool, 0) != VK_SUCCESS) {
         fprintf(stderr, "vkResetDescriptorPool failed\n");
         return VK_NULL_HANDLE;
      }
      if (vkAllocateDescriptorSets(screen->dev, &dsai, &desc_set) != VK_SUCCESS) {
         fprintf(stderr, "vkAllocateDescriptorSets failed\n");
         return VK_NULL_HANDLE;
      }
   }

   return desc_set;
}

static void
zink_bind_vertex_buffers(struct zink_batch *batch, struct zink_context *ctx)
{
   VkBuffer buffers[PIPE_MAX_ATTRIBS];
   VkDeviceSize buffer_offsets[PIPE_MAX_ATTRIBS];
   const struct zink_vertex_elements_state *elems = ctx->element_state;
   for (unsigned i = 0; i < elems->hw_state.num_bindings; i++) {
      struct pipe_vertex_buffer *vb = ctx->buffers + ctx->element_state->binding_map[i];
      assert(vb && vb->buffer.resource);
      struct zink_resource *res = zink_resource(vb->buffer.resource);
      buffers[i] = res->buffer;
      buffer_offsets[i] = vb->buffer_offset;
      zink_batch_reference_resoure(batch, res);
   }

   if (elems->hw_state.num_bindings > 0)
      vkCmdBindVertexBuffers(batch->cmdbuf, 0,
                             elems->hw_state.num_bindings,
                             buffers, buffer_offsets);
}

static uint32_t
hash_gfx_program(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct zink_shader *) * (PIPE_SHADER_TYPES - 1));
}

static bool
equals_gfx_program(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(struct zink_shader *) * (PIPE_SHADER_TYPES - 1)) == 0;
}

static struct zink_gfx_program *
get_gfx_program(struct zink_context *ctx)
{
   if (ctx->dirty & ZINK_DIRTY_PROGRAM) {
      struct hash_entry *entry = _mesa_hash_table_search(ctx->program_cache,
                                                         ctx->gfx_stages);
      if (!entry) {
         struct zink_gfx_program *prog;
         prog = zink_create_gfx_program(zink_screen(ctx->base.screen)->dev,
                                                     ctx->gfx_stages);
         entry = _mesa_hash_table_insert(ctx->program_cache, prog->stages, prog);
         if (!entry)
            return NULL;
      }
      ctx->curr_program = entry->data;
      ctx->dirty &= ~ZINK_DIRTY_PROGRAM;
   }

   assert(ctx->curr_program);
   return ctx->curr_program;
}

static void
zink_draw_vbo(struct pipe_context *pctx,
              const struct pipe_draw_info *dinfo)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_rasterizer_state *rast_state = ctx->rast_state;

   if (dinfo->mode >= PIPE_PRIM_QUADS ||
       dinfo->mode == PIPE_PRIM_LINE_LOOP) {
      if (!u_trim_pipe_prim(dinfo->mode, (unsigned *)&dinfo->count))
         return;

      util_primconvert_save_rasterizer_state(ctx->primconvert, &rast_state->base);
      util_primconvert_draw_vbo(ctx->primconvert, dinfo);
      return;
   }

   struct zink_gfx_program *gfx_program = get_gfx_program(ctx);
   if (!gfx_program)
      return;

   VkPipeline pipeline = zink_get_gfx_pipeline(screen->dev, gfx_program,
                                               &ctx->gfx_pipeline_state,
                                               dinfo->mode);

   bool depth_bias = false;
   switch (u_reduced_prim(dinfo->mode)) {
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
      if (dinfo->has_user_indices) {
         if (!util_upload_index_buffer(pctx, dinfo, &index_buffer, &index_offset)) {
            debug_printf("util_upload_index_buffer() failed\n");
            return;
         }
      } else
         index_buffer = dinfo->index.resource;
   }

   VkDescriptorSet desc_set = allocate_descriptor_set(ctx, gfx_program->dsl);

   struct zink_batch *batch = zink_context_curr_batch(ctx);

   VkWriteDescriptorSet wds[PIPE_SHADER_TYPES * PIPE_MAX_CONSTANT_BUFFERS + PIPE_SHADER_TYPES * PIPE_MAX_SHADER_SAMPLER_VIEWS];
   VkDescriptorBufferInfo buffer_infos[PIPE_SHADER_TYPES * PIPE_MAX_CONSTANT_BUFFERS];
   VkDescriptorImageInfo image_infos[PIPE_SHADER_TYPES * PIPE_MAX_SHADER_SAMPLER_VIEWS];
   int num_wds = 0, num_buffer_info = 0, num_image_info = 0;

   struct zink_resource *transitions[PIPE_SHADER_TYPES * PIPE_MAX_SHADER_SAMPLER_VIEWS];
   int num_transitions = 0;

   for (int i = 0; i < ARRAY_SIZE(ctx->gfx_stages); i++) {
      struct zink_shader *shader = ctx->gfx_stages[i];
      if (!shader)
         continue;

      for (int j = 0; j < shader->num_bindings; j++) {
         int index = shader->bindings[j].index;
         if (shader->bindings[j].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            assert(ctx->ubos[i][index].buffer_size > 0);
            assert(ctx->ubos[i][index].buffer);
            struct zink_resource *res = zink_resource(ctx->ubos[i][index].buffer);
            buffer_infos[num_buffer_info].buffer = res->buffer;
            buffer_infos[num_buffer_info].offset = ctx->ubos[i][index].buffer_offset;
            buffer_infos[num_buffer_info].range  = VK_WHOLE_SIZE;
            wds[num_wds].pBufferInfo = buffer_infos + num_buffer_info;
            ++num_buffer_info;
            zink_batch_reference_resoure(batch, res);
         } else {
            struct pipe_sampler_view *psampler_view = ctx->image_views[i][index];
            assert(psampler_view);
            struct zink_sampler_view *sampler_view = (struct zink_sampler_view *)psampler_view;
            struct zink_resource *res = zink_resource(psampler_view->texture);
            VkImageLayout layout = res->layout;
            if (layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
                layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                layout != VK_IMAGE_LAYOUT_GENERAL) {
               transitions[num_transitions++] = res;
               layout = VK_IMAGE_LAYOUT_GENERAL;
            }
            image_infos[num_image_info].imageLayout = layout;
            image_infos[num_image_info].imageView = sampler_view->image_view;
            image_infos[num_image_info].sampler = ctx->samplers[i][index];
            wds[num_wds].pImageInfo = image_infos + num_image_info;
            ++num_image_info;
            zink_batch_reference_resoure(batch, res);
         }

         wds[num_wds].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
         wds[num_wds].pNext = NULL;
         wds[num_wds].dstBinding = shader->bindings[j].binding;
         wds[num_wds].dstArrayElement = 0;
         wds[num_wds].descriptorCount = 1;
         wds[num_wds].descriptorType = shader->bindings[j].type;
         ++num_wds;
      }
   }

   if (num_transitions > 0) {
      if (batch->rp)
         vkCmdEndRenderPass(batch->cmdbuf);

      for (int i = 0; i < num_transitions; ++i)
         zink_resource_barrier(batch->cmdbuf, transitions[i],
                               transitions[i]->aspect,
                               VK_IMAGE_LAYOUT_GENERAL);

      zink_begin_render_pass(ctx, batch);
   } else if (!batch->rp)
      zink_begin_render_pass(ctx, batch);


   vkCmdSetViewport(batch->cmdbuf, 0, ctx->num_viewports, ctx->viewports);

   if (ctx->num_scissors)
      vkCmdSetScissor(batch->cmdbuf, 0, ctx->num_scissors, ctx->scissors);
   else if (ctx->fb_state.width && ctx->fb_state.height) {
      VkRect2D fb_scissor = {};
      fb_scissor.extent.width = ctx->fb_state.width;
      fb_scissor.extent.height = ctx->fb_state.height;
      vkCmdSetScissor(batch->cmdbuf, 0, 1, &fb_scissor);
   }

   vkCmdSetStencilReference(batch->cmdbuf, VK_STENCIL_FACE_FRONT_BIT, ctx->stencil_ref[0]);
   vkCmdSetStencilReference(batch->cmdbuf, VK_STENCIL_FACE_BACK_BIT, ctx->stencil_ref[1]);

   if (depth_bias)
      vkCmdSetDepthBias(batch->cmdbuf, rast_state->offset_units, rast_state->offset_clamp, rast_state->offset_scale);
   else
      vkCmdSetDepthBias(batch->cmdbuf, 0.0f, 0.0f, 0.0f);

   if (ctx->gfx_pipeline_state.blend_state->need_blend_constants)
      vkCmdSetBlendConstants(batch->cmdbuf, ctx->blend_constants);

   for (int i = 0; i < num_wds; ++i)
      wds[i].dstSet = desc_set;

   vkUpdateDescriptorSets(screen->dev, num_wds, wds, 0, NULL);

   vkCmdBindPipeline(batch->cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(batch->cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           gfx_program->layout, 0, 1, &desc_set, 0, NULL);
   zink_bind_vertex_buffers(batch, ctx);

   if (dinfo->index_size > 0) {
      assert(dinfo->index_size != 1);
      VkIndexType index_type = dinfo->index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      struct zink_resource *res = zink_resource(index_buffer);
      vkCmdBindIndexBuffer(batch->cmdbuf, res->buffer, index_offset, index_type);
      zink_batch_reference_resoure(batch, res);
      vkCmdDrawIndexed(batch->cmdbuf,
         dinfo->count, dinfo->instance_count,
         dinfo->start, dinfo->index_bias, dinfo->start_instance);
   } else
      vkCmdDraw(batch->cmdbuf, dinfo->count, dinfo->instance_count, dinfo->start, dinfo->start_instance);

   if (dinfo->index_size > 0 && dinfo->has_user_indices)
      pipe_resource_reference(&index_buffer, NULL);
}

static void
zink_flush(struct pipe_context *pctx,
           struct pipe_fence_handle **pfence,
           enum pipe_flush_flags flags)
{
   struct zink_context *ctx = zink_context(pctx);

   struct zink_batch *batch = zink_context_curr_batch(ctx);
   flush_batch(ctx);

   if (pfence)
      zink_fence_reference(zink_screen(pctx->screen),
                           (struct zink_fence **)pfence,
                           batch->fence);

   if (flags & PIPE_FLUSH_END_OF_FRAME)
      pctx->screen->fence_finish(pctx->screen, pctx,
                                 (struct pipe_fence_handle *)batch->fence,
                                 PIPE_TIMEOUT_INFINITE);
}

static void
zink_blit(struct pipe_context *pctx,
          const struct pipe_blit_info *info)
{
   struct zink_context *ctx = zink_context(pctx);
   bool is_resolve = false;
   if (info->mask != PIPE_MASK_RGBA ||
       info->scissor_enable ||
       info->alpha_blend) {
      if (!util_blitter_is_blit_supported(ctx->blitter, info)) {
         debug_printf("blit unsupported %s -> %s\n",
                 util_format_short_name(info->src.resource->format),
                 util_format_short_name(info->dst.resource->format));
         return;
      }

      util_blitter_save_fragment_constant_buffer_slot(ctx->blitter, ctx->ubos[PIPE_SHADER_FRAGMENT]);
      util_blitter_save_vertex_buffer_slot(ctx->blitter, ctx->buffers);
      util_blitter_save_vertex_shader(ctx->blitter, ctx->gfx_stages[PIPE_SHADER_VERTEX]);
      util_blitter_save_fragment_shader(ctx->blitter, ctx->gfx_stages[PIPE_SHADER_FRAGMENT]);
      util_blitter_save_rasterizer(ctx->blitter, ctx->gfx_pipeline_state.rast_state);

      util_blitter_blit(ctx->blitter, info);
      return;
   }

   struct zink_resource *src = zink_resource(info->src.resource);
   struct zink_resource *dst = zink_resource(info->dst.resource);

   if (src->base.nr_samples > 1 && dst->base.nr_samples <= 1)
      is_resolve = true;

   struct zink_batch *batch = zink_context_curr_batch(ctx);
   if (batch->rp)
      vkCmdEndRenderPass(batch->cmdbuf);

   zink_batch_reference_resoure(batch, src);
   zink_batch_reference_resoure(batch, dst);

   if (is_resolve) {
      VkImageResolve region = {};

      region.srcSubresource.aspectMask = src->aspect;
      region.srcSubresource.mipLevel = info->src.level;
      region.srcSubresource.baseArrayLayer = 0; // no clue
      region.srcSubresource.layerCount = 1; // no clue
      region.srcOffset.x = info->src.box.x;
      region.srcOffset.y = info->src.box.y;
      region.srcOffset.z = info->src.box.z;

      region.dstSubresource.aspectMask = dst->aspect;
      region.dstSubresource.mipLevel = info->dst.level;
      region.dstSubresource.baseArrayLayer = 0; // no clue
      region.dstSubresource.layerCount = 1; // no clue
      region.dstOffset.x = info->dst.box.x;
      region.dstOffset.y = info->dst.box.y;
      region.dstOffset.z = info->dst.box.z;

      region.extent.width = info->dst.box.width;
      region.extent.height = info->dst.box.height;
      region.extent.depth = info->dst.box.depth;
      vkCmdResolveImage(batch->cmdbuf, src->image, src->layout,
                        dst->image, dst->layout,
                        1, &region);

   } else {
      if (dst->layout != VK_IMAGE_LAYOUT_GENERAL &&
          dst->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
         zink_resource_barrier(batch->cmdbuf, dst, dst->aspect,
                               VK_IMAGE_LAYOUT_GENERAL);

      VkImageBlit region = {};
      region.srcSubresource.aspectMask = src->aspect;
      region.srcSubresource.mipLevel = info->src.level;
      region.srcOffsets[0].x = info->src.box.x;
      region.srcOffsets[0].y = info->src.box.y;
      region.srcOffsets[1].x = info->src.box.x + info->src.box.width;
      region.srcOffsets[1].y = info->src.box.y + info->src.box.height;

      if (src->base.array_size > 1) {
         region.srcOffsets[0].z = 0;
         region.srcOffsets[1].z = 1;
         region.srcSubresource.baseArrayLayer = info->src.box.z;
         region.srcSubresource.layerCount = info->src.box.depth;
      } else {
         region.srcOffsets[0].z = info->src.box.z;
         region.srcOffsets[1].z = info->src.box.z + info->src.box.depth;
         region.srcSubresource.baseArrayLayer = 0;
         region.srcSubresource.layerCount = 1;
      }

      region.dstSubresource.aspectMask = dst->aspect;
      region.dstSubresource.mipLevel = info->dst.level;
      region.dstOffsets[0].x = info->dst.box.x;
      region.dstOffsets[0].y = info->dst.box.y;
      region.dstOffsets[1].x = info->dst.box.x + info->dst.box.width;
      region.dstOffsets[1].y = info->dst.box.y + info->dst.box.height;

      if (dst->base.array_size > 1) {
         region.dstOffsets[0].z = 0;
         region.dstOffsets[1].z = 1;
         region.dstSubresource.baseArrayLayer = info->dst.box.z;
         region.dstSubresource.layerCount = info->dst.box.depth;
      } else {
         region.dstOffsets[0].z = info->dst.box.z;
         region.dstOffsets[1].z = info->dst.box.z + info->dst.box.depth;
         region.dstSubresource.baseArrayLayer = 0;
         region.dstSubresource.layerCount = 1;
      }

      vkCmdBlitImage(batch->cmdbuf, src->image, src->layout,
                     dst->image, dst->layout,
                     1, &region,
                     filter(info->filter));
   }

   if (batch->rp)
      zink_begin_render_pass(ctx, batch);

   /* HACK: I have no idea why this is needed, but without it ioquake3
    * randomly keeps fading to black.
    */
   flush_batch(ctx);
}

static void
zink_flush_resource(struct pipe_context *pipe,
                    struct pipe_resource *resource)
{
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

      struct zink_batch *batch = zink_context_curr_batch(ctx);
      zink_batch_reference_resoure(batch, src);
      zink_batch_reference_resoure(batch, dst);

      vkCmdCopyImage(batch->cmdbuf, src->image, src->layout,
                     dst->image, dst->layout,
                     1, &region);
   } else
      debug_printf("zink: TODO resource copy\n");
}

struct pipe_context *
zink_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct zink_screen *screen = zink_screen(pscreen);
   struct zink_context *ctx = CALLOC_STRUCT(zink_context);

   ctx->base.screen = pscreen;
   ctx->base.priv = priv;

   ctx->base.destroy = zink_context_destroy;

   zink_context_state_init(&ctx->base);

   ctx->base.create_sampler_state = zink_create_sampler_state;
   ctx->base.bind_sampler_states = zink_bind_sampler_states;
   ctx->base.delete_sampler_state = zink_delete_sampler_state;

   ctx->base.create_sampler_view = zink_create_sampler_view;
   ctx->base.set_sampler_views = zink_set_sampler_views;
   ctx->base.sampler_view_destroy = zink_destroy_sampler_view;

   ctx->base.create_vs_state = zink_create_vs_state;
   ctx->base.bind_vs_state = zink_bind_vs_state;
   ctx->base.delete_vs_state = zink_delete_vs_state;

   ctx->base.create_fs_state = zink_create_fs_state;
   ctx->base.bind_fs_state = zink_bind_fs_state;
   ctx->base.delete_fs_state = zink_delete_fs_state;

   ctx->base.set_polygon_stipple = zink_set_polygon_stipple;
   ctx->base.set_vertex_buffers = zink_set_vertex_buffers;
   ctx->base.set_viewport_states = zink_set_viewport_states;
   ctx->base.set_scissor_states = zink_set_scissor_states;
   ctx->base.set_constant_buffer = zink_set_constant_buffer;
   ctx->base.set_framebuffer_state = zink_set_framebuffer_state;
   ctx->base.set_stencil_ref = zink_set_stencil_ref;
   ctx->base.set_clip_state = zink_set_clip_state;
   ctx->base.set_active_query_state = zink_set_active_query_state;
   ctx->base.set_blend_color = zink_set_blend_color;

   ctx->base.clear = zink_clear;
   ctx->base.draw_vbo = zink_draw_vbo;
   ctx->base.flush = zink_flush;

   ctx->base.resource_copy_region = zink_resource_copy_region;
   ctx->base.blit = zink_blit;

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
                        1 << PIPE_PRIM_TRIANGLE_STRIP |
                        1 << PIPE_PRIM_TRIANGLE_FAN;

   ctx->primconvert = util_primconvert_create(&ctx->base, prim_hwsupport);
   if (!ctx->primconvert)
      goto fail;

   ctx->blitter = util_blitter_create(&ctx->base);
   if (!ctx->blitter)
      goto fail;

   VkCommandPoolCreateInfo cpci = {};
   cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cpci.queueFamilyIndex = screen->gfx_queue;
   cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   if (vkCreateCommandPool(screen->dev, &cpci, NULL, &ctx->cmdpool) != VK_SUCCESS)
      goto fail;

   VkCommandBufferAllocateInfo cbai = {};
   cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cbai.commandPool = ctx->cmdpool;
   cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbai.commandBufferCount = 1;
   for (int i = 0; i < ARRAY_SIZE(ctx->batches); ++i) {
      if (vkAllocateCommandBuffers(screen->dev, &cbai, &ctx->batches[i].cmdbuf) != VK_SUCCESS)
         goto fail;

      ctx->batches[i].resources = _mesa_set_create(NULL, _mesa_hash_pointer,
                                                   _mesa_key_pointer_equal);
      if (!ctx->batches[i].resources)
         goto fail;

      util_dynarray_init(&ctx->batches[i].zombie_samplers, NULL);
   }

   VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000}
   };
   VkDescriptorPoolCreateInfo dpci = {};
   dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpci.pPoolSizes = sizes;
   dpci.poolSizeCount = ARRAY_SIZE(sizes);
   dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
   dpci.maxSets = 1000;

   if(vkCreateDescriptorPool(screen->dev, &dpci, 0, &ctx->descpool) != VK_SUCCESS)
      goto fail;

   vkGetDeviceQueue(screen->dev, screen->gfx_queue, 0, &ctx->queue);

   ctx->program_cache = _mesa_hash_table_create(NULL, hash_gfx_program, equals_gfx_program);
   if (!ctx->program_cache)
      goto fail;

   ctx->dirty = ZINK_DIRTY_PROGRAM;

   /* start the first batch */
   zink_start_cmdbuf(ctx, zink_context_curr_batch(ctx));

   return &ctx->base;

fail:
   if (ctx) {
      vkDestroyCommandPool(screen->dev, ctx->cmdpool, NULL);
      FREE(ctx);
   }
   return NULL;
}

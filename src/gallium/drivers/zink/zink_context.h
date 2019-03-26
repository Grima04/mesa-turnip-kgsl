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

#ifndef ZINK_CONTEXT_H
#define ZINK_CONTEXT_H

#include "zink_pipeline.h"
#include "zink_cmdbuf.h"

#include "pipe/p_context.h"
#include "pipe/p_state.h"

#include "util/slab.h"

#include <vulkan/vulkan.h>

struct blitter_context;
struct primconvert_context;

struct zink_blend_state;
struct zink_depth_stencil_alpha_state;
struct zink_gfx_program;
struct zink_rasterizer_state;
struct zink_resource;
struct zink_vertex_elements_state;

struct zink_sampler_view {
   struct pipe_sampler_view base;
   VkImageView image_view;
};

static inline struct zink_sampler_view *
zink_sampler_view(struct pipe_sampler_view *pview)
{
   return (struct zink_sampler_view *)pview;
}

#define ZINK_DIRTY_PROGRAM (1 << 0)

struct zink_context {
   struct pipe_context base;
   struct slab_child_pool transfer_pool;
   struct blitter_context *blitter;

   VkCommandPool cmdpool;
   struct zink_cmdbuf cmdbufs[1];

   VkQueue queue;

   VkDescriptorPool descpool;

   struct pipe_constant_buffer ubos[PIPE_SHADER_TYPES][PIPE_MAX_CONSTANT_BUFFERS];
   struct pipe_framebuffer_state fb_state;

   struct zink_shader *gfx_stages[PIPE_SHADER_TYPES - 1];
   struct zink_gfx_pipeline_state gfx_pipeline_state;
   struct hash_table *program_cache;
   struct zink_gfx_program *curr_program;
   unsigned dirty;

   struct primconvert_context *primconvert;

   struct zink_render_pass *render_pass;
   struct zink_framebuffer *framebuffer;

   VkViewport viewports[PIPE_MAX_VIEWPORTS];
   unsigned num_viewports;

   VkRect2D scissors[PIPE_MAX_VIEWPORTS];
   unsigned num_scissors;

   struct pipe_vertex_buffer buffers[PIPE_MAX_ATTRIBS];
   uint32_t buffers_enabled_mask;

   VkSampler samplers[PIPE_SHADER_TYPES][PIPE_MAX_SAMPLERS];
   struct pipe_sampler_view *image_views[PIPE_SHADER_TYPES][PIPE_MAX_SHADER_SAMPLER_VIEWS];

   float blend_constants[4];

   uint32_t stencil_ref[2];
};

static inline struct zink_context *
zink_context(struct pipe_context *context)
{
   return (struct zink_context *)context;
}

void
zink_resource_barrier(VkCommandBuffer cmdbuf, struct zink_resource *res,
                      VkImageAspectFlags aspect, VkImageLayout new_layout);

VkShaderStageFlagBits
zink_shader_stage(enum pipe_shader_type type);

struct pipe_context *
zink_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags);

void
zink_context_query_init(struct pipe_context *ctx);

#endif

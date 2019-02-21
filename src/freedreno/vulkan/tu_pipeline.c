/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_private.h"

#include "main/menums.h"
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "spirv/nir_spirv.h"
#include "util/debug.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "vk_format.h"
#include "vk_util.h"

#include "tu_cs.h"

struct tu_pipeline_builder
{
   struct tu_device *device;
   struct tu_pipeline_cache *cache;
   const VkAllocationCallbacks *alloc;
   const VkGraphicsPipelineCreateInfo *create_info;

   bool rasterizer_discard;
   /* these states are affectd by rasterizer_discard */
   VkSampleCountFlagBits samples;
   bool use_depth_stencil_attachment;
};

static enum tu_dynamic_state_bits
tu_dynamic_state_bit(VkDynamicState state)
{
   switch (state) {
   case VK_DYNAMIC_STATE_VIEWPORT:
      return TU_DYNAMIC_VIEWPORT;
   case VK_DYNAMIC_STATE_SCISSOR:
      return TU_DYNAMIC_SCISSOR;
   case VK_DYNAMIC_STATE_LINE_WIDTH:
      return TU_DYNAMIC_LINE_WIDTH;
   case VK_DYNAMIC_STATE_DEPTH_BIAS:
      return TU_DYNAMIC_DEPTH_BIAS;
   case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
      return TU_DYNAMIC_BLEND_CONSTANTS;
   case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
      return TU_DYNAMIC_DEPTH_BOUNDS;
   case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
      return TU_DYNAMIC_STENCIL_COMPARE_MASK;
   case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
      return TU_DYNAMIC_STENCIL_WRITE_MASK;
   case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
      return TU_DYNAMIC_STENCIL_REFERENCE;
   default:
      unreachable("invalid dynamic state");
      return 0;
   }
}

static enum pc_di_primtype
tu6_primtype(VkPrimitiveTopology topology)
{
   switch (topology) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return DI_PT_POINTLIST;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return DI_PT_LINELIST;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return DI_PT_LINESTRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return DI_PT_TRILIST;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return DI_PT_TRILIST;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return DI_PT_TRIFAN;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      return DI_PT_LINE_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
      return DI_PT_LINESTRIP_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
      return DI_PT_TRI_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
      return DI_PT_TRISTRIP_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
   default:
      unreachable("invalid primitive topology");
      return DI_PT_NONE;
   }
}

static enum adreno_compare_func
tu6_compare_func(VkCompareOp op)
{
   switch (op) {
   case VK_COMPARE_OP_NEVER:
      return FUNC_NEVER;
   case VK_COMPARE_OP_LESS:
      return FUNC_LESS;
   case VK_COMPARE_OP_EQUAL:
      return FUNC_EQUAL;
   case VK_COMPARE_OP_LESS_OR_EQUAL:
      return FUNC_LEQUAL;
   case VK_COMPARE_OP_GREATER:
      return FUNC_GREATER;
   case VK_COMPARE_OP_NOT_EQUAL:
      return FUNC_NOTEQUAL;
   case VK_COMPARE_OP_GREATER_OR_EQUAL:
      return FUNC_GEQUAL;
   case VK_COMPARE_OP_ALWAYS:
      return FUNC_ALWAYS;
   default:
      unreachable("invalid VkCompareOp");
      return FUNC_NEVER;
   }
}

static enum adreno_stencil_op
tu6_stencil_op(VkStencilOp op)
{
   switch (op) {
   case VK_STENCIL_OP_KEEP:
      return STENCIL_KEEP;
   case VK_STENCIL_OP_ZERO:
      return STENCIL_ZERO;
   case VK_STENCIL_OP_REPLACE:
      return STENCIL_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
      return STENCIL_INCR_CLAMP;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
      return STENCIL_DECR_CLAMP;
   case VK_STENCIL_OP_INVERT:
      return STENCIL_INVERT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:
      return STENCIL_INCR_WRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:
      return STENCIL_DECR_WRAP;
   default:
      unreachable("invalid VkStencilOp");
      return STENCIL_KEEP;
   }
}

static uint32_t
tu6_guardband_adj(uint32_t v)
{
   if (v > 256)
      return (uint32_t)(511.0 - 65.0 * (log2(v) - 8.0));
   else
      return 511;
}

void
tu6_emit_viewport(struct tu_cs *cs, const VkViewport *viewport)
{
   float offsets[3];
   float scales[3];
   scales[0] = viewport->width / 2.0f;
   scales[1] = viewport->height / 2.0f;
   scales[2] = viewport->maxDepth - viewport->minDepth;
   offsets[0] = viewport->x + scales[0];
   offsets[1] = viewport->y + scales[1];
   offsets[2] = viewport->minDepth;

   VkOffset2D min;
   VkOffset2D max;
   min.x = (int32_t) viewport->x;
   max.x = (int32_t) ceilf(viewport->x + viewport->width);
   if (viewport->height >= 0.0f) {
      min.y = (int32_t) viewport->y;
      max.y = (int32_t) ceilf(viewport->y + viewport->height);
   } else {
      min.y = (int32_t)(viewport->y + viewport->height);
      max.y = (int32_t) ceilf(viewport->y);
   }
   /* the spec allows viewport->height to be 0.0f */
   if (min.y == max.y)
      max.y++;
   assert(min.x >= 0 && min.x < max.x);
   assert(min.y >= 0 && min.y < max.y);

   VkExtent2D guardband_adj;
   guardband_adj.width = tu6_guardband_adj(max.x - min.x);
   guardband_adj.height = tu6_guardband_adj(max.y - min.y);

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_CL_VPORT_XOFFSET_0, 6);
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_XOFFSET_0(offsets[0]));
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_XSCALE_0(scales[0]));
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_YOFFSET_0(offsets[1]));
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_YSCALE_0(scales[1]));
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_ZOFFSET_0(offsets[2]));
   tu_cs_emit(cs, A6XX_GRAS_CL_VPORT_ZSCALE_0(scales[2]));

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(min.x) |
                     A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(min.y));
   tu_cs_emit(cs, A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(max.x - 1) |
                     A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(max.y - 1));

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ, 1);
   tu_cs_emit(cs,
              A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ(guardband_adj.width) |
                 A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT(guardband_adj.height));
}

void
tu6_emit_scissor(struct tu_cs *cs, const VkRect2D *scissor)
{
   const VkOffset2D min = scissor->offset;
   const VkOffset2D max = {
      scissor->offset.x + scissor->extent.width,
      scissor->offset.y + scissor->extent.height,
   };

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(min.x) |
                     A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(min.y));
   tu_cs_emit(cs, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(max.x - 1) |
                     A6XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(max.y - 1));
}

static void
tu6_emit_gras_unknowns(struct tu_cs *cs)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_UNKNOWN_8000, 1);
   tu_cs_emit(cs, 0x80);
   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_UNKNOWN_8001, 1);
   tu_cs_emit(cs, 0x0);
   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_UNKNOWN_8004, 1);
   tu_cs_emit(cs, 0x0);
}

static void
tu6_emit_point_size(struct tu_cs *cs)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SU_POINT_MINMAX, 2);
   tu_cs_emit(cs, A6XX_GRAS_SU_POINT_MINMAX_MIN(1.0f / 16.0f) |
                     A6XX_GRAS_SU_POINT_MINMAX_MAX(4092.0f));
   tu_cs_emit(cs, A6XX_GRAS_SU_POINT_SIZE(1.0f));
}

static uint32_t
tu6_gras_su_cntl(const VkPipelineRasterizationStateCreateInfo *rast_info,
                 VkSampleCountFlagBits samples)
{
   uint32_t gras_su_cntl = 0;

   if (rast_info->cullMode & VK_CULL_MODE_FRONT_BIT)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_CULL_FRONT;
   if (rast_info->cullMode & VK_CULL_MODE_BACK_BIT)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_CULL_BACK;

   if (rast_info->frontFace == VK_FRONT_FACE_CLOCKWISE)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_FRONT_CW;

   /* don't set A6XX_GRAS_SU_CNTL_LINEHALFWIDTH */

   if (rast_info->depthBiasEnable)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_POLY_OFFSET;

   if (samples > VK_SAMPLE_COUNT_1_BIT)
      gras_su_cntl |= A6XX_GRAS_SU_CNTL_MSAA_ENABLE;

   return gras_su_cntl;
}

void
tu6_emit_gras_su_cntl(struct tu_cs *cs,
                      uint32_t gras_su_cntl,
                      float line_width)
{
   assert((gras_su_cntl & A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__MASK) == 0);
   gras_su_cntl |= A6XX_GRAS_SU_CNTL_LINEHALFWIDTH(line_width / 2.0f);

   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SU_CNTL, 1);
   tu_cs_emit(cs, gras_su_cntl);
}

void
tu6_emit_depth_bias(struct tu_cs *cs,
                    float constant_factor,
                    float clamp,
                    float slope_factor)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_GRAS_SU_POLY_OFFSET_SCALE, 3);
   tu_cs_emit(cs, A6XX_GRAS_SU_POLY_OFFSET_SCALE(slope_factor));
   tu_cs_emit(cs, A6XX_GRAS_SU_POLY_OFFSET_OFFSET(constant_factor));
   tu_cs_emit(cs, A6XX_GRAS_SU_POLY_OFFSET_OFFSET_CLAMP(clamp));
}

static void
tu6_emit_alpha_control_disable(struct tu_cs *cs)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_RB_ALPHA_CONTROL, 1);
   tu_cs_emit(cs, 0);
}

static void
tu6_emit_depth_control(struct tu_cs *cs,
                       const VkPipelineDepthStencilStateCreateInfo *ds_info)
{
   assert(!ds_info->depthBoundsTestEnable);

   uint32_t rb_depth_cntl = 0;
   if (ds_info->depthTestEnable) {
      rb_depth_cntl |=
         A6XX_RB_DEPTH_CNTL_Z_ENABLE |
         A6XX_RB_DEPTH_CNTL_ZFUNC(tu6_compare_func(ds_info->depthCompareOp)) |
         A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE;

      if (ds_info->depthWriteEnable)
         rb_depth_cntl |= A6XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE;
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_DEPTH_CNTL, 1);
   tu_cs_emit(cs, rb_depth_cntl);
}

static void
tu6_emit_stencil_control(struct tu_cs *cs,
                         const VkPipelineDepthStencilStateCreateInfo *ds_info)
{
   uint32_t rb_stencil_control = 0;
   if (ds_info->stencilTestEnable) {
      const VkStencilOpState *front = &ds_info->front;
      const VkStencilOpState *back = &ds_info->back;
      rb_stencil_control |=
         A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE |
         A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF |
         A6XX_RB_STENCIL_CONTROL_STENCIL_READ |
         A6XX_RB_STENCIL_CONTROL_FUNC(tu6_compare_func(front->compareOp)) |
         A6XX_RB_STENCIL_CONTROL_FAIL(tu6_stencil_op(front->failOp)) |
         A6XX_RB_STENCIL_CONTROL_ZPASS(tu6_stencil_op(front->passOp)) |
         A6XX_RB_STENCIL_CONTROL_ZFAIL(tu6_stencil_op(front->depthFailOp)) |
         A6XX_RB_STENCIL_CONTROL_FUNC_BF(tu6_compare_func(back->compareOp)) |
         A6XX_RB_STENCIL_CONTROL_FAIL_BF(tu6_stencil_op(back->failOp)) |
         A6XX_RB_STENCIL_CONTROL_ZPASS_BF(tu6_stencil_op(back->passOp)) |
         A6XX_RB_STENCIL_CONTROL_ZFAIL_BF(tu6_stencil_op(back->depthFailOp));
   }

   tu_cs_emit_pkt4(cs, REG_A6XX_RB_STENCIL_CONTROL, 1);
   tu_cs_emit(cs, rb_stencil_control);
}

void
tu6_emit_stencil_compare_mask(struct tu_cs *cs, uint32_t front, uint32_t back)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_RB_STENCILMASK, 1);
   tu_cs_emit(
      cs, A6XX_RB_STENCILMASK_MASK(front) | A6XX_RB_STENCILMASK_BFMASK(back));
}

void
tu6_emit_stencil_write_mask(struct tu_cs *cs, uint32_t front, uint32_t back)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_RB_STENCILWRMASK, 1);
   tu_cs_emit(cs, A6XX_RB_STENCILWRMASK_WRMASK(front) |
                     A6XX_RB_STENCILWRMASK_BFWRMASK(back));
}

void
tu6_emit_stencil_reference(struct tu_cs *cs, uint32_t front, uint32_t back)
{
   tu_cs_emit_pkt4(cs, REG_A6XX_RB_STENCILREF, 1);
   tu_cs_emit(cs,
              A6XX_RB_STENCILREF_REF(front) | A6XX_RB_STENCILREF_BFREF(back));
}

static VkResult
tu_pipeline_builder_create_pipeline(struct tu_pipeline_builder *builder,
                                    struct tu_pipeline **out_pipeline)
{
   struct tu_device *dev = builder->device;

   struct tu_pipeline *pipeline =
      vk_zalloc2(&dev->alloc, builder->alloc, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   tu_cs_init(&pipeline->cs, TU_CS_MODE_SUB_STREAM, 2048);

   /* reserve the space now such that tu_cs_begin_sub_stream never fails */
   VkResult result = tu_cs_reserve_space(dev, &pipeline->cs, 2048);
   if (result != VK_SUCCESS) {
      vk_free2(&dev->alloc, builder->alloc, pipeline);
      return result;
   }

   *out_pipeline = pipeline;

   return VK_SUCCESS;
}

static void
tu_pipeline_builder_parse_dynamic(struct tu_pipeline_builder *builder,
                                  struct tu_pipeline *pipeline)
{
   const VkPipelineDynamicStateCreateInfo *dynamic_info =
      builder->create_info->pDynamicState;

   if (!dynamic_info)
      return;

   for (uint32_t i = 0; i < dynamic_info->dynamicStateCount; i++) {
      pipeline->dynamic_state.mask |=
         tu_dynamic_state_bit(dynamic_info->pDynamicStates[i]);
   }
}

static void
tu_pipeline_builder_parse_input_assembly(struct tu_pipeline_builder *builder,
                                         struct tu_pipeline *pipeline)
{
   const VkPipelineInputAssemblyStateCreateInfo *ia_info =
      builder->create_info->pInputAssemblyState;

   pipeline->ia.primtype = tu6_primtype(ia_info->topology);
   pipeline->ia.primitive_restart = ia_info->primitiveRestartEnable;
}

static void
tu_pipeline_builder_parse_viewport(struct tu_pipeline_builder *builder,
                                   struct tu_pipeline *pipeline)
{
   /* The spec says:
    *
    *    pViewportState is a pointer to an instance of the
    *    VkPipelineViewportStateCreateInfo structure, and is ignored if the
    *    pipeline has rasterization disabled."
    *
    * We leave the relevant registers stale in that case.
    */
   if (builder->rasterizer_discard)
      return;

   const VkPipelineViewportStateCreateInfo *vp_info =
      builder->create_info->pViewportState;

   struct tu_cs vp_cs;
   tu_cs_begin_sub_stream(builder->device, &pipeline->cs, 15, &vp_cs);

   if (!(pipeline->dynamic_state.mask & TU_DYNAMIC_VIEWPORT)) {
      assert(vp_info->viewportCount == 1);
      tu6_emit_viewport(&vp_cs, vp_info->pViewports);
   }

   if (!(pipeline->dynamic_state.mask & TU_DYNAMIC_SCISSOR)) {
      assert(vp_info->scissorCount == 1);
      tu6_emit_scissor(&vp_cs, vp_info->pScissors);
   }

   pipeline->vp.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &vp_cs);
}

static void
tu_pipeline_builder_parse_rasterization(struct tu_pipeline_builder *builder,
                                        struct tu_pipeline *pipeline)
{
   const VkPipelineRasterizationStateCreateInfo *rast_info =
      builder->create_info->pRasterizationState;

   assert(!rast_info->depthClampEnable);
   assert(rast_info->polygonMode == VK_POLYGON_MODE_FILL);

   struct tu_cs rast_cs;
   tu_cs_begin_sub_stream(builder->device, &pipeline->cs, 20, &rast_cs);

   /* move to hw ctx init? */
   tu6_emit_gras_unknowns(&rast_cs);
   tu6_emit_point_size(&rast_cs);

   const uint32_t gras_su_cntl =
      tu6_gras_su_cntl(rast_info, builder->samples);

   if (!(pipeline->dynamic_state.mask & TU_DYNAMIC_LINE_WIDTH))
      tu6_emit_gras_su_cntl(&rast_cs, gras_su_cntl, rast_info->lineWidth);

   if (!(pipeline->dynamic_state.mask & TU_DYNAMIC_DEPTH_BIAS)) {
      tu6_emit_depth_bias(&rast_cs, rast_info->depthBiasConstantFactor,
                          rast_info->depthBiasClamp,
                          rast_info->depthBiasSlopeFactor);
   }

   pipeline->rast.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &rast_cs);

   pipeline->rast.gras_su_cntl = gras_su_cntl;
}

static void
tu_pipeline_builder_parse_depth_stencil(struct tu_pipeline_builder *builder,
                                        struct tu_pipeline *pipeline)
{
   /* The spec says:
    *
    *    pDepthStencilState is a pointer to an instance of the
    *    VkPipelineDepthStencilStateCreateInfo structure, and is ignored if
    *    the pipeline has rasterization disabled or if the subpass of the
    *    render pass the pipeline is created against does not use a
    *    depth/stencil attachment.
    *
    * We disable both depth and stenil tests in those cases.
    */
   static const VkPipelineDepthStencilStateCreateInfo dummy_ds_info;
   const VkPipelineDepthStencilStateCreateInfo *ds_info =
      builder->use_depth_stencil_attachment
         ? builder->create_info->pDepthStencilState
         : &dummy_ds_info;

   struct tu_cs ds_cs;
   tu_cs_begin_sub_stream(builder->device, &pipeline->cs, 12, &ds_cs);

   /* move to hw ctx init? */
   tu6_emit_alpha_control_disable(&ds_cs);

   tu6_emit_depth_control(&ds_cs, ds_info);
   tu6_emit_stencil_control(&ds_cs, ds_info);

   if (!(pipeline->dynamic_state.mask & TU_DYNAMIC_STENCIL_COMPARE_MASK)) {
      tu6_emit_stencil_compare_mask(&ds_cs, ds_info->front.compareMask,
                                    ds_info->back.compareMask);
   }
   if (!(pipeline->dynamic_state.mask & TU_DYNAMIC_STENCIL_WRITE_MASK)) {
      tu6_emit_stencil_write_mask(&ds_cs, ds_info->front.writeMask,
                                  ds_info->back.writeMask);
   }
   if (!(pipeline->dynamic_state.mask & TU_DYNAMIC_STENCIL_REFERENCE)) {
      tu6_emit_stencil_reference(&ds_cs, ds_info->front.reference,
                                 ds_info->back.reference);
   }

   pipeline->ds.state_ib = tu_cs_end_sub_stream(&pipeline->cs, &ds_cs);
}

static void
tu_pipeline_finish(struct tu_pipeline *pipeline,
                   struct tu_device *dev,
                   const VkAllocationCallbacks *alloc)
{
   tu_cs_finish(dev, &pipeline->cs);
}

static VkResult
tu_pipeline_builder_build(struct tu_pipeline_builder *builder,
                          struct tu_pipeline **pipeline)
{
   VkResult result = tu_pipeline_builder_create_pipeline(builder, pipeline);
   if (result != VK_SUCCESS)
      return result;

   tu_pipeline_builder_parse_dynamic(builder, *pipeline);
   tu_pipeline_builder_parse_input_assembly(builder, *pipeline);
   tu_pipeline_builder_parse_viewport(builder, *pipeline);
   tu_pipeline_builder_parse_rasterization(builder, *pipeline);
   tu_pipeline_builder_parse_depth_stencil(builder, *pipeline);

   /* we should have reserved enough space upfront such that the CS never
    * grows
    */
   assert((*pipeline)->cs.bo_count == 1);

   return VK_SUCCESS;
}

static void
tu_pipeline_builder_init_graphics(
   struct tu_pipeline_builder *builder,
   struct tu_device *dev,
   struct tu_pipeline_cache *cache,
   const VkGraphicsPipelineCreateInfo *create_info,
   const VkAllocationCallbacks *alloc)
{
   *builder = (struct tu_pipeline_builder) {
      .device = dev,
      .cache = cache,
      .create_info = create_info,
      .alloc = alloc,
   };

   builder->rasterizer_discard =
      create_info->pRasterizationState->rasterizerDiscardEnable;

   if (builder->rasterizer_discard) {
      builder->samples = VK_SAMPLE_COUNT_1_BIT;
   } else {
      builder->samples = create_info->pMultisampleState->rasterizationSamples;

      const struct tu_render_pass *pass =
         tu_render_pass_from_handle(create_info->renderPass);
      const struct tu_subpass *subpass =
         &pass->subpasses[create_info->subpass];

      builder->use_depth_stencil_attachment =
         subpass->depth_stencil_attachment.attachment != VK_ATTACHMENT_UNUSED;
   }
}

VkResult
tu_CreateGraphicsPipelines(VkDevice device,
                           VkPipelineCache pipelineCache,
                           uint32_t count,
                           const VkGraphicsPipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   TU_FROM_HANDLE(tu_device, dev, device);
   TU_FROM_HANDLE(tu_pipeline_cache, cache, pipelineCache);

   for (uint32_t i = 0; i < count; i++) {
      struct tu_pipeline_builder builder;
      tu_pipeline_builder_init_graphics(&builder, dev, cache,
                                        &pCreateInfos[i], pAllocator);

      struct tu_pipeline *pipeline;
      VkResult result = tu_pipeline_builder_build(&builder, &pipeline);

      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            tu_DestroyPipeline(device, pPipelines[j], pAllocator);
            pPipelines[j] = VK_NULL_HANDLE;
         }

         return result;
      }

      pPipelines[i] = tu_pipeline_to_handle(pipeline);
   }

   return VK_SUCCESS;
}

static VkResult
tu_compute_pipeline_create(VkDevice _device,
                           VkPipelineCache _cache,
                           const VkComputePipelineCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipeline)
{
   return VK_SUCCESS;
}

VkResult
tu_CreateComputePipelines(VkDevice _device,
                          VkPipelineCache pipelineCache,
                          uint32_t count,
                          const VkComputePipelineCreateInfo *pCreateInfos,
                          const VkAllocationCallbacks *pAllocator,
                          VkPipeline *pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      VkResult r;
      r = tu_compute_pipeline_create(_device, pipelineCache, &pCreateInfos[i],
                                     pAllocator, &pPipelines[i]);
      if (r != VK_SUCCESS) {
         result = r;
         pPipelines[i] = VK_NULL_HANDLE;
      }
   }

   return result;
}

void
tu_DestroyPipeline(VkDevice _device,
                   VkPipeline _pipeline,
                   const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, dev, _device);
   TU_FROM_HANDLE(tu_pipeline, pipeline, _pipeline);

   if (!_pipeline)
      return;

   tu_pipeline_finish(pipeline, dev, pAllocator);
   vk_free2(&dev->alloc, pAllocator, pipeline);
}

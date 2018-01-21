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

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#include <memcheck.h>
#define VG(x) x
#define __gen_validate_value(x) VALGRIND_CHECK_MEM_IS_DEFINED(&(x), sizeof(x))
#else
#define VG(x)
#endif

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_transfer.h"
#include "intel/compiler/brw_compiler.h"
#include "intel/common/gen_sample_positions.h"
#include "iris_batch.h"
#include "iris_context.h"
#include "iris_pipe.h"
#include "iris_resource.h"

#define __gen_address_type struct iris_address
#define __gen_user_data struct iris_batch

static uint64_t
__gen_combine_address(struct iris_batch *batch, void *location,
                      struct iris_address addr, uint32_t delta)
{
   if (addr.bo == NULL)
      return addr.offset + delta;

   return iris_batch_reloc(batch, location - batch->cmdbuf.map, addr.bo,
                           addr.offset + delta, addr.reloc_flags);
}

#define __genxml_cmd_length(cmd) cmd ## _length
#define __genxml_cmd_length_bias(cmd) cmd ## _length_bias
#define __genxml_cmd_header(cmd) cmd ## _header
#define __genxml_cmd_pack(cmd) cmd ## _pack

#define iris_pack_command(cmd, dst, name)                         \
   for (struct cmd name = { __genxml_cmd_header(cmd) },           \
        *_dst = (void *)(dst); __builtin_expect(_dst != NULL, 1); \
        ({ __genxml_cmd_pack(cmd)(NULL, (void *)_dst, &name);     \
           VG(VALGRIND_CHECK_MEM_IS_DEFINED(_dst, __genxml_cmd_length(cmd) * 4)); \
           _dst = NULL;                                           \
           }))

#define iris_pack_state(cmd, dst, name)                           \
   for (struct cmd name = {},                                     \
        *_dst = (void *)(dst); __builtin_expect(_dst != NULL, 1); \
        __genxml_cmd_pack(cmd)(NULL, (void *)_dst, &name),        \
        _dst = NULL)

#define iris_emit_cmd(batch, cmd, name) \
   iris_require_command_space(batch, 4 * __genxml_cmd_length(cmd)); \
   iris_pack_command(cmd, batch->cmdbuf.map_next, name)

#define iris_emit_merge(batch, dwords0, dwords1) \
   do {                                                                 \
      STATIC_ASSERT(ARRAY_SIZE(dwords0) == ARRAY_SIZE(dwords1));        \
                                                                        \
      iris_require_command_space(batch, ARRAY_SIZE(dwords0));           \
      uint32_t *dw = batch->cmdbuf.map_next;                            \
      for (uint32_t i = 0; i < ARRAY_SIZE(dwords0); i++)                \
         dw[i] = (dwords0)[i] | (dwords1)[i];                           \
      VG(VALGRIND_CHECK_MEM_IS_DEFINED(dw, ARRAY_SIZE(dwords0) * 4));   \
   } while (0)

#include "genxml/genX_pack.h"
#include "genxml/gen_macros.h"

#define MOCS_WB (2 << 1)

UNUSED static void pipe_asserts()
{
#define PIPE_ASSERT(x) STATIC_ASSERT((int)x)

   /* pipe_logicop happens to match the hardware. */
   PIPE_ASSERT(PIPE_LOGICOP_CLEAR == LOGICOP_CLEAR);
   PIPE_ASSERT(PIPE_LOGICOP_NOR == LOGICOP_NOR);
   PIPE_ASSERT(PIPE_LOGICOP_AND_INVERTED == LOGICOP_AND_INVERTED);
   PIPE_ASSERT(PIPE_LOGICOP_COPY_INVERTED == LOGICOP_COPY_INVERTED);
   PIPE_ASSERT(PIPE_LOGICOP_AND_REVERSE == LOGICOP_AND_REVERSE);
   PIPE_ASSERT(PIPE_LOGICOP_INVERT == LOGICOP_INVERT);
   PIPE_ASSERT(PIPE_LOGICOP_XOR == LOGICOP_XOR);
   PIPE_ASSERT(PIPE_LOGICOP_NAND == LOGICOP_NAND);
   PIPE_ASSERT(PIPE_LOGICOP_AND == LOGICOP_AND);
   PIPE_ASSERT(PIPE_LOGICOP_EQUIV == LOGICOP_EQUIV);
   PIPE_ASSERT(PIPE_LOGICOP_NOOP == LOGICOP_NOOP);
   PIPE_ASSERT(PIPE_LOGICOP_OR_INVERTED == LOGICOP_OR_INVERTED);
   PIPE_ASSERT(PIPE_LOGICOP_COPY == LOGICOP_COPY);
   PIPE_ASSERT(PIPE_LOGICOP_OR_REVERSE == LOGICOP_OR_REVERSE);
   PIPE_ASSERT(PIPE_LOGICOP_OR == LOGICOP_OR);
   PIPE_ASSERT(PIPE_LOGICOP_SET == LOGICOP_SET);

   /* pipe_blend_func happens to match the hardware. */
   PIPE_ASSERT(PIPE_BLENDFACTOR_ONE == BLENDFACTOR_ONE);
   PIPE_ASSERT(PIPE_BLENDFACTOR_SRC_COLOR == BLENDFACTOR_SRC_COLOR);
   PIPE_ASSERT(PIPE_BLENDFACTOR_SRC_ALPHA == BLENDFACTOR_SRC_ALPHA);
   PIPE_ASSERT(PIPE_BLENDFACTOR_DST_ALPHA == BLENDFACTOR_DST_ALPHA);
   PIPE_ASSERT(PIPE_BLENDFACTOR_DST_COLOR == BLENDFACTOR_DST_COLOR);
   PIPE_ASSERT(PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE == BLENDFACTOR_SRC_ALPHA_SATURATE);
   PIPE_ASSERT(PIPE_BLENDFACTOR_CONST_COLOR == BLENDFACTOR_CONST_COLOR);
   PIPE_ASSERT(PIPE_BLENDFACTOR_CONST_ALPHA == BLENDFACTOR_CONST_ALPHA);
   PIPE_ASSERT(PIPE_BLENDFACTOR_SRC1_COLOR == BLENDFACTOR_SRC1_COLOR);
   PIPE_ASSERT(PIPE_BLENDFACTOR_SRC1_ALPHA == BLENDFACTOR_SRC1_ALPHA);
   PIPE_ASSERT(PIPE_BLENDFACTOR_ZERO == BLENDFACTOR_ZERO);
   PIPE_ASSERT(PIPE_BLENDFACTOR_INV_SRC_COLOR == BLENDFACTOR_INV_SRC_COLOR);
   PIPE_ASSERT(PIPE_BLENDFACTOR_INV_SRC_ALPHA == BLENDFACTOR_INV_SRC_ALPHA);
   PIPE_ASSERT(PIPE_BLENDFACTOR_INV_DST_ALPHA == BLENDFACTOR_INV_DST_ALPHA);
   PIPE_ASSERT(PIPE_BLENDFACTOR_INV_DST_COLOR == BLENDFACTOR_INV_DST_COLOR);
   PIPE_ASSERT(PIPE_BLENDFACTOR_INV_CONST_COLOR == BLENDFACTOR_INV_CONST_COLOR);
   PIPE_ASSERT(PIPE_BLENDFACTOR_INV_CONST_ALPHA == BLENDFACTOR_INV_CONST_ALPHA);
   PIPE_ASSERT(PIPE_BLENDFACTOR_INV_SRC1_COLOR == BLENDFACTOR_INV_SRC1_COLOR);
   PIPE_ASSERT(PIPE_BLENDFACTOR_INV_SRC1_ALPHA == BLENDFACTOR_INV_SRC1_ALPHA);

   /* pipe_blend_func happens to match the hardware. */
   PIPE_ASSERT(PIPE_BLEND_ADD == BLENDFUNCTION_ADD);
   PIPE_ASSERT(PIPE_BLEND_SUBTRACT == BLENDFUNCTION_SUBTRACT);
   PIPE_ASSERT(PIPE_BLEND_REVERSE_SUBTRACT == BLENDFUNCTION_REVERSE_SUBTRACT);
   PIPE_ASSERT(PIPE_BLEND_MIN == BLENDFUNCTION_MIN);
   PIPE_ASSERT(PIPE_BLEND_MAX == BLENDFUNCTION_MAX);

   /* pipe_stencil_op happens to match the hardware. */
   PIPE_ASSERT(PIPE_STENCIL_OP_KEEP == STENCILOP_KEEP);
   PIPE_ASSERT(PIPE_STENCIL_OP_ZERO == STENCILOP_ZERO);
   PIPE_ASSERT(PIPE_STENCIL_OP_REPLACE == STENCILOP_REPLACE);
   PIPE_ASSERT(PIPE_STENCIL_OP_INCR == STENCILOP_INCRSAT);
   PIPE_ASSERT(PIPE_STENCIL_OP_DECR == STENCILOP_DECRSAT);
   PIPE_ASSERT(PIPE_STENCIL_OP_INCR_WRAP == STENCILOP_INCR);
   PIPE_ASSERT(PIPE_STENCIL_OP_DECR_WRAP == STENCILOP_DECR);
   PIPE_ASSERT(PIPE_STENCIL_OP_INVERT == STENCILOP_INVERT);
#undef PIPE_ASSERT
}

static unsigned
translate_prim_type(enum pipe_prim_type prim, uint8_t verts_per_patch)
{
   static const unsigned map[] = {
      [PIPE_PRIM_POINTS]                   = _3DPRIM_POINTLIST,
      [PIPE_PRIM_LINES]                    = _3DPRIM_LINELIST,
      [PIPE_PRIM_LINE_LOOP]                = _3DPRIM_LINELOOP,
      [PIPE_PRIM_LINE_STRIP]               = _3DPRIM_LINESTRIP,
      [PIPE_PRIM_TRIANGLES]                = _3DPRIM_TRILIST,
      [PIPE_PRIM_TRIANGLE_STRIP]           = _3DPRIM_TRISTRIP,
      [PIPE_PRIM_TRIANGLE_FAN]             = _3DPRIM_TRIFAN,
      [PIPE_PRIM_QUADS]                    = _3DPRIM_QUADLIST,
      [PIPE_PRIM_QUAD_STRIP]               = _3DPRIM_QUADSTRIP,
      [PIPE_PRIM_POLYGON]                  = _3DPRIM_POLYGON,
      [PIPE_PRIM_LINES_ADJACENCY]          = _3DPRIM_LINELIST_ADJ,
      [PIPE_PRIM_LINE_STRIP_ADJACENCY]     = _3DPRIM_LINESTRIP_ADJ,
      [PIPE_PRIM_TRIANGLES_ADJACENCY]      = _3DPRIM_TRILIST_ADJ,
      [PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY] = _3DPRIM_TRISTRIP_ADJ,
      [PIPE_PRIM_PATCHES]                  = _3DPRIM_PATCHLIST_1 - 1,
   };

   return map[prim] + (prim == PIPE_PRIM_PATCHES ? verts_per_patch : 0);
}

static unsigned
translate_compare_func(enum pipe_compare_func pipe_func)
{
   static const unsigned map[] = {
      [PIPE_FUNC_NEVER]    = COMPAREFUNCTION_NEVER,
      [PIPE_FUNC_LESS]     = COMPAREFUNCTION_LESS,
      [PIPE_FUNC_EQUAL]    = COMPAREFUNCTION_EQUAL,
      [PIPE_FUNC_LEQUAL]   = COMPAREFUNCTION_LEQUAL,
      [PIPE_FUNC_GREATER]  = COMPAREFUNCTION_GREATER,
      [PIPE_FUNC_NOTEQUAL] = COMPAREFUNCTION_NOTEQUAL,
      [PIPE_FUNC_GEQUAL]   = COMPAREFUNCTION_GEQUAL,
      [PIPE_FUNC_ALWAYS]   = COMPAREFUNCTION_ALWAYS,
   };
   return map[pipe_func];
}

static unsigned
translate_shadow_func(enum pipe_compare_func pipe_func)
{
   /* Gallium specifies the result of shadow comparisons as:
    *
    *    1 if ref <op> texel,
    *    0 otherwise.
    *
    * The hardware does:
    *
    *    0 if texel <op> ref,
    *    1 otherwise.
    *
    * So we need to flip the operator and also negate.
    */
   static const unsigned map[] = {
      [PIPE_FUNC_NEVER]    = PREFILTEROPALWAYS,
      [PIPE_FUNC_LESS]     = PREFILTEROPLEQUAL,
      [PIPE_FUNC_EQUAL]    = PREFILTEROPNOTEQUAL,
      [PIPE_FUNC_LEQUAL]   = PREFILTEROPLESS,
      [PIPE_FUNC_GREATER]  = PREFILTEROPGEQUAL,
      [PIPE_FUNC_NOTEQUAL] = PREFILTEROPEQUAL,
      [PIPE_FUNC_GEQUAL]   = PREFILTEROPGREATER,
      [PIPE_FUNC_ALWAYS]   = PREFILTEROPNEVER,
   };
   return map[pipe_func];
}

static unsigned
translate_cull_mode(unsigned pipe_face)
{
   static const unsigned map[4] = {
      [PIPE_FACE_NONE]           = CULLMODE_NONE,
      [PIPE_FACE_FRONT]          = CULLMODE_FRONT,
      [PIPE_FACE_BACK]           = CULLMODE_BACK,
      [PIPE_FACE_FRONT_AND_BACK] = CULLMODE_BOTH,
   };
   return map[pipe_face];
}

static unsigned
translate_fill_mode(unsigned pipe_polymode)
{
   static const unsigned map[4] = {
      [PIPE_POLYGON_MODE_FILL]           = FILL_MODE_SOLID,
      [PIPE_POLYGON_MODE_LINE]           = FILL_MODE_WIREFRAME,
      [PIPE_POLYGON_MODE_POINT]          = FILL_MODE_POINT,
      [PIPE_POLYGON_MODE_FILL_RECTANGLE] = FILL_MODE_SOLID,
   };
   return map[pipe_polymode];
}

static struct iris_address
ro_bo(struct iris_bo *bo, uint32_t offset)
{
   return (struct iris_address) { .bo = bo, .offset = offset };
}

static void
iris_upload_initial_gpu_state(struct iris_context *ice,
                              struct iris_batch *batch)
{
   iris_emit_cmd(batch, GENX(3DSTATE_DRAWING_RECTANGLE), rect) {
      rect.ClippedDrawingRectangleXMax = UINT16_MAX;
      rect.ClippedDrawingRectangleYMax = UINT16_MAX;
   }
   iris_emit_cmd(batch, GENX(3DSTATE_SAMPLE_PATTERN), pat) {
      GEN_SAMPLE_POS_1X(pat._1xSample);
      GEN_SAMPLE_POS_2X(pat._2xSample);
      GEN_SAMPLE_POS_4X(pat._4xSample);
      GEN_SAMPLE_POS_8X(pat._8xSample);
      GEN_SAMPLE_POS_16X(pat._16xSample);
   }
   iris_emit_cmd(batch, GENX(3DSTATE_AA_LINE_PARAMETERS), foo);
   iris_emit_cmd(batch, GENX(3DSTATE_WM_CHROMAKEY), foo);
   iris_emit_cmd(batch, GENX(3DSTATE_WM_HZ_OP), foo);
   /* XXX: may need to set an offset for origin-UL framebuffers */
   iris_emit_cmd(batch, GENX(3DSTATE_POLY_STIPPLE_OFFSET), foo);

   /* Just assign a static partitioning. */
   for (int i = 0; i <= MESA_SHADER_FRAGMENT; i++) {
      iris_emit_cmd(batch, GENX(3DSTATE_PUSH_CONSTANT_ALLOC_VS), alloc) {
         alloc._3DCommandSubOpcode = 18 + i;
         alloc.ConstantBufferOffset = 6 * i;
         alloc.ConstantBufferSize = i == MESA_SHADER_FRAGMENT ? 8 : 6;
      }
   }
}

static void
iris_launch_grid(struct pipe_context *ctx, const struct pipe_grid_info *info)
{
}

static void
iris_set_blend_color(struct pipe_context *ctx,
                     const struct pipe_blend_color *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;

   memcpy(&ice->state.blend_color, state, sizeof(struct pipe_blend_color));
   ice->state.dirty |= IRIS_DIRTY_COLOR_CALC_STATE;
}

struct iris_blend_state {
   uint32_t ps_blend[GENX(3DSTATE_PS_BLEND_length)];
   uint32_t blend_state[GENX(BLEND_STATE_length)];
   uint32_t blend_entries[BRW_MAX_DRAW_BUFFERS *
                          GENX(BLEND_STATE_ENTRY_length)];
};

static void *
iris_create_blend_state(struct pipe_context *ctx,
                        const struct pipe_blend_state *state)
{
   struct iris_blend_state *cso = malloc(sizeof(struct iris_blend_state));

   iris_pack_state(GENX(BLEND_STATE), cso->blend_state, bs) {
      bs.AlphaToCoverageEnable = state->alpha_to_coverage;
      bs.IndependentAlphaBlendEnable = state->independent_blend_enable;
      bs.AlphaToOneEnable = state->alpha_to_one;
      bs.AlphaToCoverageDitherEnable = state->alpha_to_coverage;
      bs.ColorDitherEnable = state->dither;
      //bs.AlphaTestEnable = <comes from alpha state> :(
      //bs.AlphaTestFunction = <comes from alpha state> :(
   }

   iris_pack_command(GENX(3DSTATE_PS_BLEND), cso->ps_blend, pb) {
      //pb.HasWriteableRT = <comes from somewhere> :(
      //pb.AlphaTestEnable = <comes from alpha state> :(
      pb.AlphaToCoverageEnable = state->alpha_to_coverage;
      pb.IndependentAlphaBlendEnable = state->independent_blend_enable;

      pb.ColorBufferBlendEnable = state->rt[0].blend_enable;

      pb.SourceBlendFactor           = state->rt[0].rgb_src_factor;
      pb.SourceAlphaBlendFactor      = state->rt[0].alpha_func;
      pb.DestinationBlendFactor      = state->rt[0].rgb_dst_factor;
      pb.DestinationAlphaBlendFactor = state->rt[0].alpha_dst_factor;
   }

   for (int i = 0; i < BRW_MAX_DRAW_BUFFERS; i++) {
      iris_pack_state(GENX(BLEND_STATE_ENTRY), &cso->blend_entries[i], be) {
         be.LogicOpEnable = state->logicop_enable;
         be.LogicOpFunction = state->logicop_func;

         be.PreBlendSourceOnlyClampEnable = false;
         be.ColorClampRange = COLORCLAMP_RTFORMAT;
         be.PreBlendColorClampEnable = true;
         be.PostBlendColorClampEnable = true;

         be.ColorBufferBlendEnable = state->rt[i].blend_enable;

         be.ColorBlendFunction          = state->rt[i].rgb_func;
         be.AlphaBlendFunction          = state->rt[i].alpha_func;
         be.SourceBlendFactor           = state->rt[i].rgb_src_factor;
         be.SourceAlphaBlendFactor      = state->rt[i].alpha_func;
         be.DestinationBlendFactor      = state->rt[i].rgb_dst_factor;
         be.DestinationAlphaBlendFactor = state->rt[i].alpha_dst_factor;

         be.WriteDisableRed   = state->rt[i].colormask & PIPE_MASK_R;
         be.WriteDisableGreen = state->rt[i].colormask & PIPE_MASK_G;
         be.WriteDisableBlue  = state->rt[i].colormask & PIPE_MASK_B;
         be.WriteDisableAlpha = state->rt[i].colormask & PIPE_MASK_A;
      }
   }

   return cso;
}

static void
iris_bind_blend_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   ice->state.cso_blend = state;
   ice->state.dirty |= IRIS_DIRTY_CC_VIEWPORT;
   ice->state.dirty |= IRIS_DIRTY_WM_DEPTH_STENCIL;
}

struct iris_depth_stencil_alpha_state {
   uint32_t wmds[GENX(3DSTATE_WM_DEPTH_STENCIL_length)];
   uint32_t cc_vp[GENX(CC_VIEWPORT_length)];

   struct pipe_alpha_state alpha; /* to BLEND_STATE, 3DSTATE_PS_BLEND */
};

static void *
iris_create_zsa_state(struct pipe_context *ctx,
                      const struct pipe_depth_stencil_alpha_state *state)
{
   struct iris_depth_stencil_alpha_state *cso =
      malloc(sizeof(struct iris_depth_stencil_alpha_state));

   cso->alpha = state->alpha;

   bool two_sided_stencil = state->stencil[1].enabled;

   /* The state tracker needs to optimize away EQUAL writes for us. */
   assert(!(state->depth.func == PIPE_FUNC_EQUAL && state->depth.writemask));

   iris_pack_command(GENX(3DSTATE_WM_DEPTH_STENCIL), cso->wmds, wmds) {
      wmds.StencilFailOp = state->stencil[0].fail_op;
      wmds.StencilPassDepthFailOp = state->stencil[0].zfail_op;
      wmds.StencilPassDepthPassOp = state->stencil[0].zpass_op;
      wmds.StencilTestFunction =
         translate_compare_func(state->stencil[0].func);
      wmds.BackfaceStencilFailOp = state->stencil[1].fail_op;
      wmds.BackfaceStencilPassDepthFailOp = state->stencil[1].zfail_op;
      wmds.BackfaceStencilPassDepthPassOp = state->stencil[1].zpass_op;
      wmds.BackfaceStencilTestFunction =
         translate_compare_func(state->stencil[1].func);
      wmds.DepthTestFunction = translate_compare_func(state->depth.func);
      wmds.DoubleSidedStencilEnable = two_sided_stencil;
      wmds.StencilTestEnable = state->stencil[0].enabled;
      wmds.StencilBufferWriteEnable =
         state->stencil[0].writemask != 0 ||
         (two_sided_stencil && state->stencil[1].writemask != 0);
      wmds.DepthTestEnable = state->depth.enabled;
      wmds.DepthBufferWriteEnable = state->depth.writemask;
      wmds.StencilTestMask = state->stencil[0].valuemask;
      wmds.StencilWriteMask = state->stencil[0].writemask;
      wmds.BackfaceStencilTestMask = state->stencil[1].valuemask;
      wmds.BackfaceStencilWriteMask = state->stencil[1].writemask;
      /* wmds.[Backface]StencilReferenceValue are merged later */
   }

   iris_pack_state(GENX(CC_VIEWPORT), cso->cc_vp, ccvp) {
      ccvp.MinimumDepth = state->depth.bounds_min;
      ccvp.MaximumDepth = state->depth.bounds_max;
   }

   return cso;
}

static void
iris_bind_zsa_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_depth_stencil_alpha_state *old_cso = ice->state.cso_zsa;
   struct iris_depth_stencil_alpha_state *new_cso = state;

   if (new_cso) {
      if (!old_cso || old_cso->alpha.ref_value != new_cso->alpha.ref_value) {
         ice->state.dirty |= IRIS_DIRTY_COLOR_CALC_STATE;
      }
   }

   ice->state.cso_zsa = new_cso;
   ice->state.dirty |= IRIS_DIRTY_CC_VIEWPORT;
   ice->state.dirty |= IRIS_DIRTY_WM_DEPTH_STENCIL;
}

struct iris_rasterizer_state {
   uint32_t sf[GENX(3DSTATE_SF_length)];
   uint32_t clip[GENX(3DSTATE_CLIP_length)];
   uint32_t raster[GENX(3DSTATE_RASTER_length)];
   uint32_t wm[GENX(3DSTATE_WM_length)];
   uint32_t line_stipple[GENX(3DSTATE_LINE_STIPPLE_length)];

   bool flatshade; /* for shader state */
   bool light_twoside; /* for shader state */
   bool rasterizer_discard; /* for 3DSTATE_STREAMOUT */
   bool half_pixel_center; /* for 3DSTATE_MULTISAMPLE */
   enum pipe_sprite_coord_mode sprite_coord_mode; /* PIPE_SPRITE_* */
};

static void *
iris_create_rasterizer_state(struct pipe_context *ctx,
                             const struct pipe_rasterizer_state *state)
{
   struct iris_rasterizer_state *cso =
      malloc(sizeof(struct iris_rasterizer_state));

#if 0
   sprite_coord_mode -> SBE PointSpriteTextureCoordinateOrigin
   sprite_coord_enable -> SBE PointSpriteTextureCoordinateEnable
   point_quad_rasterization -> SBE?

   not necessary?
   {
      poly_smooth
      force_persample_interp - ?
      bottom_edge_rule

      offset_units_unscaled - cap not exposed
   }
   #endif

   cso->flatshade = state->flatshade;
   cso->light_twoside = state->light_twoside;
   cso->rasterizer_discard = state->rasterizer_discard;
   cso->half_pixel_center = state->half_pixel_center;

   iris_pack_command(GENX(3DSTATE_SF), cso->sf, sf) {
      sf.StatisticsEnable = true;
      sf.ViewportTransformEnable = true;
      sf.AALineDistanceMode = AALINEDISTANCE_TRUE;
      sf.LineEndCapAntialiasingRegionWidth =
         state->line_smooth ? _10pixels : _05pixels;
      sf.LastPixelEnable = state->line_last_pixel;
      sf.LineWidth = state->line_width;
      sf.SmoothPointEnable = state->point_smooth;
      sf.PointWidthSource = state->point_size_per_vertex ? Vertex : State;
      sf.PointWidth = state->point_size;

      if (state->flatshade_first) {
         sf.TriangleStripListProvokingVertexSelect = 2;
         sf.TriangleFanProvokingVertexSelect = 2;
         sf.LineStripListProvokingVertexSelect = 1;
      } else {
         sf.TriangleFanProvokingVertexSelect = 1;
      }
   }

   /* COMPLETE! */
   iris_pack_command(GENX(3DSTATE_RASTER), cso->raster, rr) {
      rr.FrontWinding = state->front_ccw ? CounterClockwise : Clockwise;
      rr.CullMode = translate_cull_mode(state->cull_face);
      rr.FrontFaceFillMode = translate_fill_mode(state->fill_front);
      rr.BackFaceFillMode = translate_fill_mode(state->fill_back);
      rr.DXMultisampleRasterizationEnable = state->multisample;
      rr.GlobalDepthOffsetEnableSolid = state->offset_tri;
      rr.GlobalDepthOffsetEnableWireframe = state->offset_line;
      rr.GlobalDepthOffsetEnablePoint = state->offset_point;
      rr.GlobalDepthOffsetConstant = state->offset_units;
      rr.GlobalDepthOffsetScale = state->offset_scale;
      rr.GlobalDepthOffsetClamp = state->offset_clamp;
      rr.SmoothPointEnable = state->point_smooth;
      rr.AntialiasingEnable = state->line_smooth;
      rr.ScissorRectangleEnable = state->scissor;
      rr.ViewportZNearClipTestEnable = state->depth_clip_near;
      rr.ViewportZFarClipTestEnable = state->depth_clip_far;
      //rr.ConservativeRasterizationEnable = not yet supported by Gallium...
   }

   iris_pack_command(GENX(3DSTATE_CLIP), cso->clip, cl) {
      cl.StatisticsEnable = true;
      cl.EarlyCullEnable = true;
      cl.UserClipDistanceClipTestEnableBitmask = state->clip_plane_enable;
      cl.ForceUserClipDistanceClipTestEnableBitmask = true;
      cl.APIMode = state->clip_halfz ? APIMODE_D3D : APIMODE_OGL;
      cl.GuardbandClipTestEnable = true;
      cl.ClipMode = CLIPMODE_NORMAL;
      cl.ClipEnable = true;
      cl.ViewportXYClipTestEnable = state->point_tri_clip;
      cl.MinimumPointWidth = 0.125;
      cl.MaximumPointWidth = 255.875;
      //.NonPerspectiveBarycentricEnable = <comes from FS prog> :(
      //.ForceZeroRTAIndexEnable = <comes from FB layers being 0>

      if (state->flatshade_first) {
         cl.TriangleStripListProvokingVertexSelect = 2;
         cl.TriangleFanProvokingVertexSelect = 2;
         cl.LineStripListProvokingVertexSelect = 1;
      } else {
         cl.TriangleFanProvokingVertexSelect = 1;
      }
   }

   iris_pack_command(GENX(3DSTATE_WM), cso->wm, wm) {
      wm.LineAntialiasingRegionWidth = _10pixels;
      wm.LineEndCapAntialiasingRegionWidth = _05pixels;
      wm.PointRasterizationRule = RASTRULE_UPPER_RIGHT;
      wm.StatisticsEnable = true;
      wm.LineStippleEnable = state->line_stipple_enable;
      wm.PolygonStippleEnable = state->poly_stipple_enable;
      // wm.BarycentricInterpolationMode = <comes from FS program> :(
      // wm.EarlyDepthStencilControl = <comes from FS program> :(
   }

   /* Remap from 0..255 back to 1..256 */
   const unsigned line_stipple_factor = state->line_stipple_factor + 1;

   iris_pack_command(GENX(3DSTATE_LINE_STIPPLE), cso->line_stipple, line) {
      line.LineStipplePattern = state->line_stipple_pattern;
      line.LineStippleInverseRepeatCount = 1.0f / line_stipple_factor;
      line.LineStippleRepeatCount = line_stipple_factor;
   }

   return cso;
}

static void
iris_bind_rasterizer_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_rasterizer_state *old_cso = ice->state.cso_rast;
   struct iris_rasterizer_state *new_cso = state;

   if (new_cso) {
      /* Try to avoid re-emitting 3DSTATE_LINE_STIPPLE, it's non-pipelined */
      if (!old_cso || memcmp(old_cso->line_stipple, new_cso->line_stipple,
                             sizeof(old_cso->line_stipple)) != 0) {
         ice->state.dirty |= IRIS_DIRTY_LINE_STIPPLE;
      }

      if (!old_cso ||
          old_cso->half_pixel_center != new_cso->half_pixel_center) {
         ice->state.dirty |= IRIS_DIRTY_MULTISAMPLE;
      }
   }

   ice->state.cso_rast = new_cso;
   ice->state.dirty |= IRIS_DIRTY_RASTER;
}

static uint32_t
translate_wrap(unsigned pipe_wrap)
{
   static const unsigned map[] = {
      [PIPE_TEX_WRAP_REPEAT]                 = TCM_WRAP,
      [PIPE_TEX_WRAP_CLAMP]                  = TCM_HALF_BORDER,
      [PIPE_TEX_WRAP_CLAMP_TO_EDGE]          = TCM_CLAMP,
      [PIPE_TEX_WRAP_CLAMP_TO_BORDER]        = TCM_CLAMP_BORDER,
      [PIPE_TEX_WRAP_MIRROR_REPEAT]          = TCM_MIRROR,
      [PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE]   = TCM_MIRROR_ONCE,
      [PIPE_TEX_WRAP_MIRROR_CLAMP]           = -1, // XXX: ???
      [PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER] = -1, // XXX: ???
   };
   return map[pipe_wrap];
}

/**
 * Return true if the given wrap mode requires the border color to exist.
 */
static bool
wrap_mode_needs_border_color(unsigned wrap_mode)
{
   return wrap_mode == TCM_CLAMP_BORDER || wrap_mode == TCM_HALF_BORDER;
}

static unsigned
translate_mip_filter(enum pipe_tex_mipfilter pipe_mip)
{
   static const unsigned map[] = {
      [PIPE_TEX_MIPFILTER_NEAREST] = MIPFILTER_NEAREST,
      [PIPE_TEX_MIPFILTER_LINEAR]  = MIPFILTER_LINEAR,
      [PIPE_TEX_MIPFILTER_NONE]    = MIPFILTER_NONE,
   };
   return map[pipe_mip];
}

struct iris_sampler_state {
   struct pipe_sampler_state base;

   bool needs_border_color;

   uint32_t sampler_state[GENX(SAMPLER_STATE_length)];
};

static void *
iris_create_sampler_state(struct pipe_context *pctx,
                          const struct pipe_sampler_state *state)
{
   struct iris_sampler_state *cso = CALLOC_STRUCT(iris_sampler_state);

   if (!cso)
      return NULL;

   STATIC_ASSERT(PIPE_TEX_FILTER_NEAREST == MAPFILTER_NEAREST);
   STATIC_ASSERT(PIPE_TEX_FILTER_LINEAR == MAPFILTER_LINEAR);

   unsigned wrap_s = translate_wrap(state->wrap_s);
   unsigned wrap_t = translate_wrap(state->wrap_t);
   unsigned wrap_r = translate_wrap(state->wrap_r);

   cso->needs_border_color = wrap_mode_needs_border_color(wrap_s) ||
                             wrap_mode_needs_border_color(wrap_t) ||
                             wrap_mode_needs_border_color(wrap_r);

   iris_pack_state(GENX(SAMPLER_STATE), cso->sampler_state, samp) {
      samp.TCXAddressControlMode = wrap_s;
      samp.TCYAddressControlMode = wrap_t;
      samp.TCZAddressControlMode = wrap_r;
      samp.CubeSurfaceControlMode = state->seamless_cube_map;
      samp.NonnormalizedCoordinateEnable = !state->normalized_coords;
      samp.MinModeFilter = state->min_img_filter;
      samp.MagModeFilter = state->mag_img_filter;
      samp.MipModeFilter = translate_mip_filter(state->min_mip_filter);
      samp.MaximumAnisotropy = RATIO21;

      if (state->max_anisotropy >= 2) {
         if (state->min_img_filter == PIPE_TEX_FILTER_LINEAR) {
            samp.MinModeFilter = MAPFILTER_ANISOTROPIC;
            samp.AnisotropicAlgorithm = EWAApproximation;
         }

         if (state->mag_img_filter == PIPE_TEX_FILTER_LINEAR)
            samp.MagModeFilter = MAPFILTER_ANISOTROPIC;

         samp.MaximumAnisotropy =
            MIN2((state->max_anisotropy - 2) / 2, RATIO161);
      }

      /* Set address rounding bits if not using nearest filtering. */
      if (state->min_img_filter != PIPE_TEX_FILTER_NEAREST) {
         samp.UAddressMinFilterRoundingEnable = true;
         samp.VAddressMinFilterRoundingEnable = true;
         samp.RAddressMinFilterRoundingEnable = true;
      }

      if (state->mag_img_filter != PIPE_TEX_FILTER_NEAREST) {
         samp.UAddressMagFilterRoundingEnable = true;
         samp.VAddressMagFilterRoundingEnable = true;
         samp.RAddressMagFilterRoundingEnable = true;
      }

      if (state->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE)
         samp.ShadowFunction = translate_shadow_func(state->compare_func);

      const float hw_max_lod = GEN_GEN >= 7 ? 14 : 13;

      samp.LODPreClampMode = CLAMP_MODE_OGL;
      samp.MinLOD = CLAMP(state->min_lod, 0, hw_max_lod);
      samp.MaxLOD = CLAMP(state->max_lod, 0, hw_max_lod);
      samp.TextureLODBias = CLAMP(state->lod_bias, -16, 15);

      //samp.BorderColorPointer = <<comes from elsewhere>>
   }

   return cso;
}

static void
iris_bind_sampler_states(struct pipe_context *ctx,
                         enum pipe_shader_type p_stage,
                         unsigned start, unsigned count,
                         void **states)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   gl_shader_stage stage = stage_from_pipe(p_stage);

   assert(start + count <= IRIS_MAX_TEXTURE_SAMPLERS);

   for (int i = 0; i < count; i++) {
      ice->state.samplers[stage][start + i] = states[i];
   }

   ice->state.dirty |= IRIS_DIRTY_SAMPLER_STATES_VS << stage;
}

struct iris_sampler_view {
   struct pipe_sampler_view pipe;
   struct isl_view view;
   uint32_t surface_state[GENX(RENDER_SURFACE_STATE_length)];
};

/**
 * Convert an swizzle enumeration (i.e. SWIZZLE_X) to one of the Gen7.5+
 * "Shader Channel Select" enumerations (i.e. HSW_SCS_RED).  The mappings are
 *
 * SWIZZLE_X, SWIZZLE_Y, SWIZZLE_Z, SWIZZLE_W, SWIZZLE_ZERO, SWIZZLE_ONE
 *         0          1          2          3             4            5
 *         4          5          6          7             0            1
 *   SCS_RED, SCS_GREEN,  SCS_BLUE, SCS_ALPHA,     SCS_ZERO,     SCS_ONE
 *
 * which is simply adding 4 then modding by 8 (or anding with 7).
 *
 * We then may need to apply workarounds for textureGather hardware bugs.
 */
static enum isl_channel_select
pipe_swizzle_to_isl_channel(enum pipe_swizzle swizzle)
{
   return (swizzle + 4) & 7;
}

static struct pipe_sampler_view *
iris_create_sampler_view(struct pipe_context *ctx,
                         struct pipe_resource *tex,
                         const struct pipe_sampler_view *tmpl)
{
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   struct iris_resource *itex = (struct iris_resource *) tex;
   struct iris_sampler_view *isv = calloc(1, sizeof(struct iris_sampler_view));

   if (!isv)
      return NULL;

   /* initialize base object */
   isv->pipe = *tmpl;
   isv->pipe.context = ctx;
   isv->pipe.texture = NULL;
   pipe_reference_init(&isv->pipe.reference, 1);
   pipe_resource_reference(&isv->pipe.texture, tex);

   /* XXX: do we need brw_get_texture_swizzle hacks here? */

   isv->view = (struct isl_view) {
      .format = iris_isl_format_for_pipe_format(tmpl->format),
      .base_level = tmpl->u.tex.first_level,
      .levels = tmpl->u.tex.last_level - tmpl->u.tex.first_level + 1,
      .base_array_layer = tmpl->u.tex.first_layer,
      .array_len = tmpl->u.tex.last_layer - tmpl->u.tex.first_layer + 1,
      .swizzle = (struct isl_swizzle) {
         .r = pipe_swizzle_to_isl_channel(tmpl->swizzle_r),
         .g = pipe_swizzle_to_isl_channel(tmpl->swizzle_g),
         .b = pipe_swizzle_to_isl_channel(tmpl->swizzle_b),
         .a = pipe_swizzle_to_isl_channel(tmpl->swizzle_a),
      },
      .usage = ISL_SURF_USAGE_TEXTURE_BIT,
   };

   isl_surf_fill_state(&screen->isl_dev, isv->surface_state,
                       .surf = &itex->surf, .view = &isv->view,
                       .mocs = MOCS_WB);
                       // .address = ...
                       // .aux_surf =
                       // .clear_color = clear_color,

   return &isv->pipe;
}

struct iris_surface {
   struct pipe_surface pipe;
   struct isl_view view;
   uint32_t surface_state[GENX(RENDER_SURFACE_STATE_length)];
};

static struct pipe_surface *
iris_create_surface(struct pipe_context *ctx,
                    struct pipe_resource *tex,
                    const struct pipe_surface *tmpl)
{
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   struct iris_surface *surf = calloc(1, sizeof(struct iris_surface));
   struct pipe_surface *psurf = &surf->pipe;
   struct iris_resource *itex = (struct iris_resource *) tex;

   if (!surf)
      return NULL;

   pipe_reference_init(&psurf->reference, 1);
   pipe_resource_reference(&psurf->texture, tex);
   psurf->context = ctx;
   psurf->format = tmpl->format;
   psurf->width = tex->width0;
   psurf->height = tex->height0;
   psurf->texture = tex;
   psurf->u.tex.first_layer = tmpl->u.tex.first_layer;
   psurf->u.tex.last_layer = tmpl->u.tex.last_layer;
   psurf->u.tex.level = tmpl->u.tex.level;

   surf->view = (struct isl_view) {
      .format = iris_isl_format_for_pipe_format(tmpl->format),
      .base_level = tmpl->u.tex.level,
      .levels = 1,
      .base_array_layer = tmpl->u.tex.first_layer,
      .array_len = tmpl->u.tex.last_layer - tmpl->u.tex.first_layer + 1,
      .swizzle = ISL_SWIZZLE_IDENTITY,
      // XXX: DEPTH_BIt, STENCIL_BIT...CUBE_BIT?  Other bits?!
      .usage = ISL_SURF_USAGE_RENDER_TARGET_BIT,
   };

   isl_surf_fill_state(&screen->isl_dev, surf->surface_state,
                       .surf = &itex->surf, .view = &surf->view,
                       .mocs = MOCS_WB);
                       // .address = ...
                       // .aux_surf =
                       // .clear_color = clear_color,

   return psurf;
}

static void
iris_set_sampler_views(struct pipe_context *ctx,
                       enum pipe_shader_type shader,
                       unsigned start, unsigned count,
                       struct pipe_sampler_view **views)
{
}

static void
iris_set_clip_state(struct pipe_context *ctx,
                    const struct pipe_clip_state *state)
{
}

static void
iris_set_polygon_stipple(struct pipe_context *ctx,
                         const struct pipe_poly_stipple *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   memcpy(&ice->state.poly_stipple, state, sizeof(*state));
   ice->state.dirty |= IRIS_DIRTY_POLYGON_STIPPLE;
}

static void
iris_set_sample_mask(struct pipe_context *ctx, unsigned sample_mask)
{
   struct iris_context *ice = (struct iris_context *) ctx;

   ice->state.sample_mask = sample_mask;
   ice->state.dirty |= IRIS_DIRTY_SAMPLE_MASK;
}

static void
iris_set_scissor_states(struct pipe_context *ctx,
                        unsigned start_slot,
                        unsigned num_scissors,
                        const struct pipe_scissor_state *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;

   // XXX: start_slot
   ice->state.num_scissors = num_scissors;

   for (unsigned i = start_slot; i < start_slot + num_scissors; i++) {
      ice->state.scissors[i] = *state;
   }

   ice->state.dirty |= IRIS_DIRTY_SCISSOR_RECT;
}

static void
iris_set_stencil_ref(struct pipe_context *ctx,
                     const struct pipe_stencil_ref *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   memcpy(&ice->state.stencil_ref, state, sizeof(*state));
   ice->state.dirty |= IRIS_DIRTY_WM_DEPTH_STENCIL;
}


struct iris_viewport_state {
   uint32_t sf_cl_vp[GENX(SF_CLIP_VIEWPORT_length)];
};

static float
extent_from_matrix(const struct pipe_viewport_state *state, int axis)
{
   return fabsf(state->scale[axis]) * state->translate[axis];
}

#if 0
static void
calculate_guardband_size(uint32_t fb_width, uint32_t fb_height,
                         float m00, float m11, float m30, float m31,
                         float *xmin, float *xmax,
                         float *ymin, float *ymax)
{
   /* According to the "Vertex X,Y Clamping and Quantization" section of the
    * Strips and Fans documentation:
    *
    * "The vertex X and Y screen-space coordinates are also /clamped/ to the
    *  fixed-point "guardband" range supported by the rasterization hardware"
    *
    * and
    *
    * "In almost all circumstances, if an object’s vertices are actually
    *  modified by this clamping (i.e., had X or Y coordinates outside of
    *  the guardband extent the rendered object will not match the intended
    *  result.  Therefore software should take steps to ensure that this does
    *  not happen - e.g., by clipping objects such that they do not exceed
    *  these limits after the Drawing Rectangle is applied."
    *
    * I believe the fundamental restriction is that the rasterizer (in
    * the SF/WM stages) have a limit on the number of pixels that can be
    * rasterized.  We need to ensure any coordinates beyond the rasterizer
    * limit are handled by the clipper.  So effectively that limit becomes
    * the clipper's guardband size.
    *
    * It goes on to say:
    *
    * "In addition, in order to be correctly rendered, objects must have a
    *  screenspace bounding box not exceeding 8K in the X or Y direction.
    *  This additional restriction must also be comprehended by software,
    *  i.e., enforced by use of clipping."
    *
    * This makes no sense.  Gen7+ hardware supports 16K render targets,
    * and you definitely need to be able to draw polygons that fill the
    * surface.  Our assumption is that the rasterizer was limited to 8K
    * on Sandybridge, which only supports 8K surfaces, and it was actually
    * increased to 16K on Ivybridge and later.
    *
    * So, limit the guardband to 16K on Gen7+ and 8K on Sandybridge.
    */
   const float gb_size = GEN_GEN >= 7 ? 16384.0f : 8192.0f;

   if (m00 != 0 && m11 != 0) {
      /* First, we compute the screen-space render area */
      const float ss_ra_xmin = MIN3(        0, m30 + m00, m30 - m00);
      const float ss_ra_xmax = MAX3( fb_width, m30 + m00, m30 - m00);
      const float ss_ra_ymin = MIN3(        0, m31 + m11, m31 - m11);
      const float ss_ra_ymax = MAX3(fb_height, m31 + m11, m31 - m11);

      /* We want the guardband to be centered on that */
      const float ss_gb_xmin = (ss_ra_xmin + ss_ra_xmax) / 2 - gb_size;
      const float ss_gb_xmax = (ss_ra_xmin + ss_ra_xmax) / 2 + gb_size;
      const float ss_gb_ymin = (ss_ra_ymin + ss_ra_ymax) / 2 - gb_size;
      const float ss_gb_ymax = (ss_ra_ymin + ss_ra_ymax) / 2 + gb_size;

      /* Now we need it in native device coordinates */
      const float ndc_gb_xmin = (ss_gb_xmin - m30) / m00;
      const float ndc_gb_xmax = (ss_gb_xmax - m30) / m00;
      const float ndc_gb_ymin = (ss_gb_ymin - m31) / m11;
      const float ndc_gb_ymax = (ss_gb_ymax - m31) / m11;

      /* Thanks to Y-flipping and ORIGIN_UPPER_LEFT, the Y coordinates may be
       * flipped upside-down.  X should be fine though.
       */
      assert(ndc_gb_xmin <= ndc_gb_xmax);
      *xmin = ndc_gb_xmin;
      *xmax = ndc_gb_xmax;
      *ymin = MIN2(ndc_gb_ymin, ndc_gb_ymax);
      *ymax = MAX2(ndc_gb_ymin, ndc_gb_ymax);
   } else {
      /* The viewport scales to 0, so nothing will be rendered. */
      *xmin = 0.0f;
      *xmax = 0.0f;
      *ymin = 0.0f;
      *ymax = 0.0f;
   }
}
#endif

static void
iris_set_viewport_states(struct pipe_context *ctx,
                         unsigned start_slot,
                         unsigned num_viewports,
                         const struct pipe_viewport_state *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_viewport_state *cso =
      malloc(sizeof(struct iris_viewport_state));

   for (unsigned i = start_slot; i < start_slot + num_viewports; i++) {
      float x_extent = extent_from_matrix(&state[i], 0);
      float y_extent = extent_from_matrix(&state[i], 1);

      iris_pack_state(GENX(SF_CLIP_VIEWPORT), cso->sf_cl_vp, vp) {
         vp.ViewportMatrixElementm00 = state[i].scale[0];
         vp.ViewportMatrixElementm11 = state[i].scale[1];
         vp.ViewportMatrixElementm22 = state[i].scale[2];
         vp.ViewportMatrixElementm30 = state[i].translate[0];
         vp.ViewportMatrixElementm31 = state[i].translate[1];
         vp.ViewportMatrixElementm32 = state[i].translate[2];
         /* XXX: in i965 this is computed based on the drawbuffer size,
          * but we don't have that here...
          */
         vp.XMinClipGuardband = -1.0;
         vp.XMaxClipGuardband = 1.0;
         vp.YMinClipGuardband = -1.0;
         vp.YMaxClipGuardband = 1.0;
         vp.XMinViewPort = -x_extent;
         vp.XMaxViewPort =  x_extent;
         vp.YMinViewPort = -y_extent;
         vp.YMaxViewPort =  y_extent;
      }
   }

   ice->state.cso_vp = cso;
   // XXX: start_slot
   ice->state.num_viewports = num_viewports;
   ice->state.dirty |= IRIS_DIRTY_SF_CL_VIEWPORT;
}

struct iris_depth_state
{
   uint32_t depth_buffer[GENX(3DSTATE_DEPTH_BUFFER_length)];
   uint32_t hier_depth_buffer[GENX(3DSTATE_HIER_DEPTH_BUFFER_length)];
   uint32_t stencil_buffer[GENX(3DSTATE_STENCIL_BUFFER_length)];
};

static void
iris_set_framebuffer_state(struct pipe_context *ctx,
                           const struct pipe_framebuffer_state *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct pipe_framebuffer_state *cso = &ice->state.framebuffer;

   if (cso->samples != state->samples) {
      ice->state.dirty |= IRIS_DIRTY_MULTISAMPLE;
   }

   cso->width = state->width;
   cso->height = state->height;
   cso->layers = state->layers;
   cso->samples = state->samples;

   unsigned i;
   for (i = 0; i < state->nr_cbufs; i++)
      pipe_surface_reference(&cso->cbufs[i], state->cbufs[i]);
   for (; i < cso->nr_cbufs; i++)
      pipe_surface_reference(&cso->cbufs[i], NULL);

   cso->nr_cbufs = state->nr_cbufs;

   pipe_surface_reference(&cso->zsbuf, state->zsbuf);

   struct isl_depth_stencil_hiz_emit_info info = {
      .mocs = MOCS_WB,
   };

   // XXX: depth buffers
}

static void
iris_set_constant_buffer(struct pipe_context *ctx,
                         enum pipe_shader_type shader, uint index,
                         const struct pipe_constant_buffer *cb)
{
}


static void
iris_sampler_view_destroy(struct pipe_context *ctx,
                          struct pipe_sampler_view *state)
{
   pipe_resource_reference(&state->texture, NULL);
   free(state);
}


static void
iris_surface_destroy(struct pipe_context *ctx, struct pipe_surface *surface)
{
   pipe_resource_reference(&surface->texture, NULL);
   free(surface);
}

static void
iris_delete_state(struct pipe_context *ctx, void *state)
{
   free(state);
}

struct iris_vertex_buffer_state {
   uint32_t vertex_buffers[1 + 33 * GENX(VERTEX_BUFFER_STATE_length)];
   unsigned length; /* length of 3DSTATE_VERTEX_BUFFERS in DWords */
};

static void
iris_set_vertex_buffers(struct pipe_context *ctx,
                        unsigned start_slot, unsigned count,
                        const struct pipe_vertex_buffer *buffers)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_vertex_buffer_state *cso =
      malloc(sizeof(struct iris_vertex_buffer_state));

   cso->length = 4 * count - 1;

   iris_pack_state(GENX(3DSTATE_VERTEX_BUFFERS), cso->vertex_buffers, vb) {
      vb.DWordLength = cso->length;
   }

   /* If there are no buffers, do nothing.  We can leave the stale
    * 3DSTATE_VERTEX_BUFFERS in place - as long as there are no vertex
    * elements that point to them, it should be fine.
    */
   if (!buffers)
      return;

   uint32_t *vb_pack_dest = &cso->vertex_buffers[1];

   for (unsigned i = 0; i < count; i++) {
      assert(!buffers[i].is_user_buffer);

      iris_pack_state(GENX(VERTEX_BUFFER_STATE), vb_pack_dest, vb) {
         vb.VertexBufferIndex = start_slot + i;
         vb.MOCS = MOCS_WB;
         vb.AddressModifyEnable = true;
         vb.BufferPitch = buffers[i].stride;
         //vb.BufferStartingAddress = ro_bo(bo, buffers[i].buffer_offset);
         //vb.BufferSize = bo->size;
      }

      vb_pack_dest += GENX(VERTEX_BUFFER_STATE_length);
   }

   ice->state.cso_vertex_buffers = cso;
   ice->state.dirty |= IRIS_DIRTY_VERTEX_BUFFERS;
}

struct iris_vertex_element_state {
   uint32_t vertex_elements[1 + 33 * GENX(VERTEX_ELEMENT_STATE_length)];
   uint32_t vf_instancing[GENX(3DSTATE_VF_INSTANCING_length)][33];
   unsigned count;
};

static void *
iris_create_vertex_elements(struct pipe_context *ctx,
                            unsigned count,
                            const struct pipe_vertex_element *state)
{
   struct iris_vertex_element_state *cso =
      malloc(sizeof(struct iris_vertex_element_state));

   cso->count = count;

   /* TODO:
    *  - create edge flag one
    *  - create SGV ones
    *  - if those are necessary, use count + 1/2/3... OR in the length
    */
   iris_pack_state(GENX(3DSTATE_VERTEX_ELEMENTS), cso->vertex_elements, ve);

   uint32_t *ve_pack_dest = &cso->vertex_elements[1];

   for (int i = 0; i < count; i++) {
      iris_pack_state(GENX(VERTEX_ELEMENT_STATE), ve_pack_dest, ve) {
         ve.VertexBufferIndex = state[i].vertex_buffer_index;
         ve.Valid = true;
         ve.SourceElementOffset = state[i].src_offset;
         ve.SourceElementFormat =
            iris_isl_format_for_pipe_format(state[i].src_format);
      }

      iris_pack_state(GENX(3DSTATE_VF_INSTANCING), cso->vf_instancing[i], vi) {
         vi.VertexElementIndex = i;
         vi.InstancingEnable = state[i].instance_divisor > 0;
         vi.InstanceDataStepRate = state[i].instance_divisor;
      }

      ve_pack_dest += GENX(VERTEX_ELEMENT_STATE_length);
   }

   return cso;
}

static void
iris_bind_vertex_elements_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;

   ice->state.cso_vertex_elements = state;
   ice->state.dirty |= IRIS_DIRTY_VERTEX_ELEMENTS;
}

static void *
iris_create_compute_state(struct pipe_context *ctx,
                          const struct pipe_compute_state *state)
{
   return malloc(1);
}

static struct pipe_stream_output_target *
iris_create_stream_output_target(struct pipe_context *ctx,
                                 struct pipe_resource *res,
                                 unsigned buffer_offset,
                                 unsigned buffer_size)
{
   struct pipe_stream_output_target *t =
      CALLOC_STRUCT(pipe_stream_output_target);
   if (!t)
      return NULL;

   pipe_reference_init(&t->reference, 1);
   pipe_resource_reference(&t->buffer, res);
   t->buffer_offset = buffer_offset;
   t->buffer_size = buffer_size;
   return t;
}

static void
iris_stream_output_target_destroy(struct pipe_context *ctx,
                                  struct pipe_stream_output_target *t)
{
   pipe_resource_reference(&t->buffer, NULL);
   free(t);
}

static void
iris_set_stream_output_targets(struct pipe_context *ctx,
                               unsigned num_targets,
                               struct pipe_stream_output_target **targets,
                               const unsigned *offsets)
{
}

void
iris_setup_state_base_address(struct iris_context *ice,
                              struct iris_batch *batch,
                              struct iris_bo *instruction_bo)
{
   if (!(ice->state.dirty & IRIS_DIRTY_STATE_BASE_ADDRESS))
      return;

   //iris_batchbuffer_flush(...)

   ice->state.dirty &= ~IRIS_DIRTY_STATE_BASE_ADDRESS;
      
   /* XXX: PIPE_CONTROLs */

   iris_emit_cmd(batch, GENX(STATE_BASE_ADDRESS), sba) {
   #if 0
   // XXX: MOCS is stupid for this.
      sba.GeneralStateMemoryObjectControlState            = MOCS_WB;
      sba.StatelessDataPortAccessMemoryObjectControlState = MOCS_WB;
      sba.SurfaceStateMemoryObjectControlState            = MOCS_WB;
      sba.DynamicStateMemoryObjectControlState            = MOCS_WB;
      sba.IndirectObjectMemoryObjectControlState          = MOCS_WB;
      sba.InstructionMemoryObjectControlState             = MOCS_WB;
      sba.BindlessSurfaceStateMemoryObjectControlState    = MOCS_WB;
   #endif

      sba.GeneralStateBaseAddressModifyEnable   = true;
      sba.SurfaceStateBaseAddressModifyEnable   = true;
      sba.DynamicStateBaseAddressModifyEnable   = true;
      sba.IndirectObjectBaseAddressModifyEnable = true;
      sba.InstructionBaseAddressModifyEnable    = true;
      sba.GeneralStateBufferSizeModifyEnable    = true;
      sba.DynamicStateBufferSizeModifyEnable    = true;
      sba.BindlessSurfaceStateBaseAddressModifyEnable = true;
      sba.IndirectObjectBufferSizeModifyEnable  = true;
      sba.InstructionBuffersizeModifyEnable     = true;

      sba.SurfaceStateBaseAddress = ro_bo(batch->statebuf.bo, 0);
      sba.DynamicStateBaseAddress = ro_bo(batch->statebuf.bo, 0);
      sba.InstructionBaseAddress =  ro_bo(instruction_bo, 0);

      sba.GeneralStateBufferSize = 0xfffff000;
      sba.DynamicStateBufferSize = ALIGN(MAX_STATE_SIZE, 4096);
      sba.IndirectObjectBufferSize = 0xfffff000;
      sba.InstructionBufferSize = ALIGN(ice->shaders.cache.bo->size, 4096);
      sba.BindlessSurfaceStateSize = 0;
   }
}

void
iris_upload_render_state(struct iris_context *ice,
                         struct iris_batch *batch,
                         const struct pipe_draw_info *draw)
{
   const uint64_t dirty = ice->state.dirty;

   if (dirty & IRIS_DIRTY_WM_DEPTH_STENCIL) {
      struct iris_depth_stencil_alpha_state *cso = ice->state.cso_zsa;
      struct pipe_stencil_ref *p_stencil_refs = &ice->state.stencil_ref;

      uint32_t stencil_refs[GENX(3DSTATE_WM_DEPTH_STENCIL_length)];
      iris_pack_command(GENX(3DSTATE_WM_DEPTH_STENCIL), &stencil_refs, wmds) {
         wmds.StencilReferenceValue = p_stencil_refs->ref_value[0];
         wmds.BackfaceStencilReferenceValue = p_stencil_refs->ref_value[1];
      }
      iris_emit_merge(batch, cso->wmds, stencil_refs);
   }

   if (dirty & IRIS_DIRTY_CC_VIEWPORT) {
      struct iris_depth_stencil_alpha_state *cso = ice->state.cso_zsa;
      iris_emit_cmd(batch, GENX(3DSTATE_VIEWPORT_STATE_POINTERS_CC), ptr) {
         ptr.CCViewportPointer =
            iris_emit_state(batch, cso->cc_vp, sizeof(cso->cc_vp), 32);
      }
   }

   if (dirty & IRIS_DIRTY_PS_BLEND) {
      struct iris_blend_state *cso = ice->state.cso_blend;
      iris_batch_emit(batch, cso->ps_blend, sizeof(cso->ps_blend));
   }

   if (dirty & IRIS_DIRTY_BLEND_STATE) {
      //struct iris_blend_state *cso = ice->state.cso_blend;
      // XXX: 3DSTATE_BLEND_STATE_POINTERS - BLEND_STATE
      // -> from iris_blend_state (most) + iris_depth_stencil_alpha_state
      //    (alpha test function/enable) + has writeable RT from ???????
   }

   if (dirty & IRIS_DIRTY_SF_CL_VIEWPORT) {
      struct iris_viewport_state *cso = ice->state.cso_vp;
      iris_emit_cmd(batch, GENX(3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP), ptr) {
         ptr.SFClipViewportPointer =
            iris_emit_state(batch, cso->sf_cl_vp, sizeof(cso->sf_cl_vp), 64);
      }
   }

   if (dirty & IRIS_DIRTY_CLIP) {
      struct iris_rasterizer_state *cso = ice->state.cso_rast;

      uint32_t dynamic_clip[GENX(3DSTATE_CLIP_length)];
      iris_pack_command(GENX(3DSTATE_CLIP), &dynamic_clip, cl) {
         //.NonPerspectiveBarycentricEnable = <comes from FS prog> :(
         //.ForceZeroRTAIndexEnable = <comes from FB layers being 0>
         // also userclip stuffs...
      }
      iris_emit_merge(batch, cso->clip, dynamic_clip);
   }

   if (dirty & IRIS_DIRTY_RASTER) {
      struct iris_rasterizer_state *cso = ice->state.cso_rast;
      iris_batch_emit(batch, cso->raster, sizeof(cso->raster));
      iris_batch_emit(batch, cso->sf, sizeof(cso->sf));
   }

   if (dirty & IRIS_DIRTY_LINE_STIPPLE) {
      struct iris_rasterizer_state *cso = ice->state.cso_rast;
      iris_batch_emit(batch, cso->line_stipple, sizeof(cso->line_stipple));
   }

   if (dirty & IRIS_DIRTY_SCISSOR) {
      uint32_t scissor_offset =
         iris_emit_state(batch, ice->state.scissors,
                         sizeof(struct pipe_scissor_state) *
                         ice->state.num_scissors, 32);

      iris_emit_cmd(batch, GENX(3DSTATE_SCISSOR_STATE_POINTERS), ptr) {
         ptr.ScissorRectPointer = scissor_offset;
      }
   }

   if (dirty & IRIS_DIRTY_POLYGON_STIPPLE) {
      iris_emit_cmd(batch, GENX(3DSTATE_POLY_STIPPLE_PATTERN), poly) {
         for (int i = 0; i < 32; i++) {
            poly.PatternRow[i] = ice->state.poly_stipple.stipple[i];
         }
      }
   }

   if (dirty & IRIS_DIRTY_COLOR_CALC_STATE) {
      struct iris_depth_stencil_alpha_state *cso = ice->state.cso_zsa;
      uint32_t cc_offset;
      void *cc_map =
         iris_alloc_state(batch,
                          sizeof(uint32_t) * GENX(COLOR_CALC_STATE_length),
                          64, &cc_offset);
      iris_pack_state(GENX(COLOR_CALC_STATE), cc_map, cc) {
         cc.AlphaTestFormat = ALPHATEST_FLOAT32;
         cc.AlphaReferenceValueAsFLOAT32 = cso->alpha.ref_value;
         cc.BlendConstantColorRed   = ice->state.blend_color.color[0];
         cc.BlendConstantColorGreen = ice->state.blend_color.color[1];
         cc.BlendConstantColorBlue  = ice->state.blend_color.color[2];
         cc.BlendConstantColorAlpha = ice->state.blend_color.color[3];
      }
      iris_emit_cmd(batch, GENX(3DSTATE_CC_STATE_POINTERS), ptr) {
         ptr.ColorCalcStatePointer = cc_offset;
         ptr.ColorCalcStatePointerValid = true;
      }
   }

   if (dirty & IRIS_DIRTY_VERTEX_BUFFERS) {
      struct iris_vertex_buffer_state *cso = ice->state.cso_vertex_buffers;
      // XXX: address!!!
      iris_batch_emit(batch, cso->vertex_buffers,
                      sizeof(uint32_t) * cso->length);
   }

   if (dirty & IRIS_DIRTY_VERTEX_ELEMENTS) {
      struct iris_vertex_element_state *cso = ice->state.cso_vertex_elements;
      iris_batch_emit(batch, cso->vertex_elements, sizeof(uint32_t) *
                      (1 + cso->count * GENX(VERTEX_ELEMENT_STATE_length)));
      for (int i = 0; i < cso->count; i++) {
         iris_batch_emit(batch, cso->vf_instancing[i],
                         sizeof(cso->vf_instancing[0]));
      }
      for (int i = 0; i < cso->count; i++) {
         /* TODO: vertexid, instanceid support */
         iris_emit_cmd(batch, GENX(3DSTATE_VF_SGVS), sgvs);
      }
   }

   if (dirty & IRIS_DIRTY_MULTISAMPLE) {
      iris_emit_cmd(batch, GENX(3DSTATE_MULTISAMPLE), ms) {
         ms.PixelLocation =
            ice->state.cso_rast->half_pixel_center ? CENTER : UL_CORNER;
         if (ice->state.framebuffer.samples > 0)
            ms.NumberofMultisamples = ffs(ice->state.framebuffer.samples) - 1;
      }
   }

   if (dirty & IRIS_DIRTY_SAMPLE_MASK) {
      iris_emit_cmd(batch, GENX(3DSTATE_SAMPLE_MASK), ms) {
         ms.SampleMask = ice->state.sample_mask;
      }
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (!(dirty & (IRIS_DIRTY_SAMPLER_STATES_VS << stage)))
         continue;

      // XXX: get sampler count from shader; don't emit them all...
      const int count = IRIS_MAX_TEXTURE_SAMPLERS;

      uint32_t offset;
      uint32_t *map = iris_alloc_state(batch,
                                       count * 4 * GENX(SAMPLER_STATE_length),
                                       32, &offset);

      for (int i = 0; i < count; i++) {
         memcpy(map, ice->state.samplers[stage][i]->sampler_state,
                4 * GENX(SAMPLER_STATE_length));
         map += GENX(SAMPLER_STATE_length);
      }

      iris_emit_cmd(batch, GENX(3DSTATE_SAMPLER_STATE_POINTERS_VS), ptr) {
         ptr._3DCommandSubOpcode = 43 + stage;
         ptr.PointertoVSSamplerState = offset;
      }
   }

   if (1) {
      iris_emit_cmd(batch, GENX(3DSTATE_VF_TOPOLOGY), topo) {
         topo.PrimitiveTopologyType =
            translate_prim_type(draw->mode, draw->vertices_per_patch);
      }
   }

   if (1) {
      iris_emit_cmd(batch, GENX(3DSTATE_VF), vf) {
         if (draw->primitive_restart) {
            vf.IndexedDrawCutIndexEnable = true;
            vf.CutIndex = draw->restart_index;
         }
      }
   }

   // draw->index_size > 0
   if (draw->index_size > 0) {
      struct iris_resource *res = (struct iris_resource *)draw->index.resource;

      assert(!draw->has_user_indices);

      iris_emit_cmd(batch, GENX(3DSTATE_INDEX_BUFFER), ib) {
         ib.IndexFormat = draw->index_size;
         ib.MOCS = MOCS_WB;
         ib.BufferSize = res->bo->size;
         // XXX: gah, addresses :(  need two different combine address funcs
         // ib.BufferStartingAddress = res->bo;
      }
   }

   assert(!draw->indirect); // XXX: indirect support

   iris_emit_cmd(batch, GENX(3DPRIMITIVE), prim) {
      prim.StartInstanceLocation = draw->start_instance;
      prim.InstanceCount = draw->instance_count;

      // XXX: this is probably bonkers.
      prim.StartVertexLocation = draw->start;

      if (draw->index_size) {
         prim.BaseVertexLocation += draw->index_bias;
      } else {
         prim.StartVertexLocation += draw->index_bias;
      }

      //prim.BaseVertexLocation = ...;
   }
#if 0
   l3 configuration

   3DSTATE_URB_*
     -> TODO

   3DSTATE_CONSTANT_* - push constants
     -> TODO

   Surfaces:
   - pull constants
   - ubos/ssbos/abos
   - images
   - textures
   - render targets - write and read
   3DSTATE_BINDING_TABLE_POINTERS_*
     -> TODO

   3DSTATE_VS
   3DSTATE_HS
   3DSTATE_TE
   3DSTATE_DS
   3DSTATE_GS
   3DSTATE_PS_EXTRA
   3DSTATE_PS
   3DSTATE_STREAMOUT
   3DSTATE_SO_BUFFER
   3DSTATE_SO_DECL_LIST

   3DSTATE_WM
     -> iris_raster_state + FS state (barycentric, EDSC)
   3DSTATE_SBE
     -> iris_raster_state (point sprite texture coordinate origin)
     -> bunch of shader state...
   3DSTATE_SBE_SWIZ
     -> FS state

   3DSTATE_DEPTH_BUFFER
   3DSTATE_HIER_DEPTH_BUFFER
   3DSTATE_STENCIL_BUFFER
   3DSTATE_CLEAR_PARAMS
     -> iris_framebuffer_state?
#endif
}

static void
iris_bind_state(struct pipe_context *ctx, void *state)
{
}

void
iris_destroy_state(struct iris_context *ice)
{
   // XXX: unreference resources/surfaces.
   for (unsigned i = 0; i < ice->state.framebuffer.nr_cbufs; i++) {
      pipe_surface_reference(&ice->state.framebuffer.cbufs[i], NULL);
   }
   pipe_surface_reference(&ice->state.framebuffer.zsbuf, NULL);
}

void
iris_init_state_functions(struct pipe_context *ctx)
{
   ctx->create_blend_state = iris_create_blend_state;
   ctx->create_depth_stencil_alpha_state = iris_create_zsa_state;
   ctx->create_rasterizer_state = iris_create_rasterizer_state;
   ctx->create_sampler_state = iris_create_sampler_state;
   ctx->create_sampler_view = iris_create_sampler_view;
   ctx->create_surface = iris_create_surface;
   ctx->create_vertex_elements_state = iris_create_vertex_elements;
   ctx->create_compute_state = iris_create_compute_state;
   ctx->bind_blend_state = iris_bind_blend_state;
   ctx->bind_depth_stencil_alpha_state = iris_bind_zsa_state;
   ctx->bind_sampler_states = iris_bind_sampler_states;
   ctx->bind_rasterizer_state = iris_bind_rasterizer_state;
   ctx->bind_vertex_elements_state = iris_bind_vertex_elements_state;
   ctx->bind_compute_state = iris_bind_state;
   ctx->delete_blend_state = iris_delete_state;
   ctx->delete_depth_stencil_alpha_state = iris_delete_state;
   ctx->delete_fs_state = iris_delete_state;
   ctx->delete_rasterizer_state = iris_delete_state;
   ctx->delete_sampler_state = iris_delete_state;
   ctx->delete_vertex_elements_state = iris_delete_state;
   ctx->delete_compute_state = iris_delete_state;
   ctx->delete_tcs_state = iris_delete_state;
   ctx->delete_tes_state = iris_delete_state;
   ctx->delete_gs_state = iris_delete_state;
   ctx->delete_vs_state = iris_delete_state;
   ctx->set_blend_color = iris_set_blend_color;
   ctx->set_clip_state = iris_set_clip_state;
   ctx->set_constant_buffer = iris_set_constant_buffer;
   ctx->set_sampler_views = iris_set_sampler_views;
   ctx->set_framebuffer_state = iris_set_framebuffer_state;
   ctx->set_polygon_stipple = iris_set_polygon_stipple;
   ctx->set_sample_mask = iris_set_sample_mask;
   ctx->set_scissor_states = iris_set_scissor_states;
   ctx->set_stencil_ref = iris_set_stencil_ref;
   ctx->set_vertex_buffers = iris_set_vertex_buffers;
   ctx->set_viewport_states = iris_set_viewport_states;
   ctx->sampler_view_destroy = iris_sampler_view_destroy;
   ctx->surface_destroy = iris_surface_destroy;
   ctx->draw_vbo = iris_draw_vbo;
   ctx->launch_grid = iris_launch_grid;
   ctx->create_stream_output_target = iris_create_stream_output_target;
   ctx->stream_output_target_destroy = iris_stream_output_target_destroy;
   ctx->set_stream_output_targets = iris_set_stream_output_targets;
}

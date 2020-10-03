/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 * Copyright (C) 2009  VMware, Inc.   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "glheader.h"

#include "accum.h"
#include "arrayobj.h"
#include "attrib.h"
#include "blend.h"
#include "buffers.h"
#include "bufferobj.h"
#include "clear.h"
#include "context.h"
#include "depth.h"
#include "enable.h"
#include "enums.h"
#include "fog.h"
#include "hint.h"
#include "light.h"
#include "lines.h"
#include "macros.h"
#include "matrix.h"
#include "multisample.h"
#include "pixelstore.h"
#include "points.h"
#include "polygon.h"
#include "shared.h"
#include "scissor.h"
#include "stencil.h"
#include "texenv.h"
#include "texgen.h"
#include "texobj.h"
#include "texparam.h"
#include "texstate.h"
#include "varray.h"
#include "viewport.h"
#include "mtypes.h"
#include "state.h"
#include "hash.h"
#include <stdbool.h>
#include "util/u_memory.h"


void GLAPIENTRY
_mesa_PushAttrib(GLbitfield mask)
{
   struct gl_attrib_node *head;

   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glPushAttrib %x\n", (int) mask);

   if (ctx->AttribStackDepth >= MAX_ATTRIB_STACK_DEPTH) {
      _mesa_error(ctx, GL_STACK_OVERFLOW, "glPushAttrib");
      return;
   }

   head = &ctx->AttribStack[ctx->AttribStackDepth];
   head->Mask = mask;

   if (mask & GL_ACCUM_BUFFER_BIT)
      memcpy(&head->Accum, &ctx->Accum, sizeof(head->Accum));

   if (mask & GL_COLOR_BUFFER_BIT) {
      memcpy(&head->Color, &ctx->Color, sizeof(struct gl_colorbuffer_attrib));
      /* push the Draw FBO's DrawBuffer[] state, not ctx->Color.DrawBuffer[] */
      for (unsigned i = 0; i < ctx->Const.MaxDrawBuffers; i ++)
         head->Color.DrawBuffer[i] = ctx->DrawBuffer->ColorDrawBuffer[i];
   }

   if (mask & GL_CURRENT_BIT) {
      FLUSH_CURRENT(ctx, 0);
      memcpy(&head->Current, &ctx->Current, sizeof(head->Current));
   }

   if (mask & GL_DEPTH_BUFFER_BIT)
      memcpy(&head->Depth, &ctx->Depth, sizeof(head->Depth));

   if (mask & GL_ENABLE_BIT) {
      struct gl_enable_attrib_node *attr = &head->Enable;
      GLuint i;

      /* Copy enable flags from all other attributes into the enable struct. */
      attr->AlphaTest = ctx->Color.AlphaEnabled;
      attr->AutoNormal = ctx->Eval.AutoNormal;
      attr->Blend = ctx->Color.BlendEnabled;
      attr->ClipPlanes = ctx->Transform.ClipPlanesEnabled;
      attr->ColorMaterial = ctx->Light.ColorMaterialEnabled;
      attr->CullFace = ctx->Polygon.CullFlag;
      attr->DepthClampNear = ctx->Transform.DepthClampNear;
      attr->DepthClampFar = ctx->Transform.DepthClampFar;
      attr->DepthTest = ctx->Depth.Test;
      attr->Dither = ctx->Color.DitherFlag;
      attr->Fog = ctx->Fog.Enabled;
      for (i = 0; i < ctx->Const.MaxLights; i++) {
         attr->Light[i] = ctx->Light.Light[i].Enabled;
      }
      attr->Lighting = ctx->Light.Enabled;
      attr->LineSmooth = ctx->Line.SmoothFlag;
      attr->LineStipple = ctx->Line.StippleFlag;
      attr->IndexLogicOp = ctx->Color.IndexLogicOpEnabled;
      attr->ColorLogicOp = ctx->Color.ColorLogicOpEnabled;
      attr->Map1Color4 = ctx->Eval.Map1Color4;
      attr->Map1Index = ctx->Eval.Map1Index;
      attr->Map1Normal = ctx->Eval.Map1Normal;
      attr->Map1TextureCoord1 = ctx->Eval.Map1TextureCoord1;
      attr->Map1TextureCoord2 = ctx->Eval.Map1TextureCoord2;
      attr->Map1TextureCoord3 = ctx->Eval.Map1TextureCoord3;
      attr->Map1TextureCoord4 = ctx->Eval.Map1TextureCoord4;
      attr->Map1Vertex3 = ctx->Eval.Map1Vertex3;
      attr->Map1Vertex4 = ctx->Eval.Map1Vertex4;
      attr->Map2Color4 = ctx->Eval.Map2Color4;
      attr->Map2Index = ctx->Eval.Map2Index;
      attr->Map2Normal = ctx->Eval.Map2Normal;
      attr->Map2TextureCoord1 = ctx->Eval.Map2TextureCoord1;
      attr->Map2TextureCoord2 = ctx->Eval.Map2TextureCoord2;
      attr->Map2TextureCoord3 = ctx->Eval.Map2TextureCoord3;
      attr->Map2TextureCoord4 = ctx->Eval.Map2TextureCoord4;
      attr->Map2Vertex3 = ctx->Eval.Map2Vertex3;
      attr->Map2Vertex4 = ctx->Eval.Map2Vertex4;
      attr->Normalize = ctx->Transform.Normalize;
      attr->RasterPositionUnclipped = ctx->Transform.RasterPositionUnclipped;
      attr->PointSmooth = ctx->Point.SmoothFlag;
      attr->PointSprite = ctx->Point.PointSprite;
      attr->PolygonOffsetPoint = ctx->Polygon.OffsetPoint;
      attr->PolygonOffsetLine = ctx->Polygon.OffsetLine;
      attr->PolygonOffsetFill = ctx->Polygon.OffsetFill;
      attr->PolygonSmooth = ctx->Polygon.SmoothFlag;
      attr->PolygonStipple = ctx->Polygon.StippleFlag;
      attr->RescaleNormals = ctx->Transform.RescaleNormals;
      attr->Scissor = ctx->Scissor.EnableFlags;
      attr->Stencil = ctx->Stencil.Enabled;
      attr->StencilTwoSide = ctx->Stencil.TestTwoSide;
      attr->MultisampleEnabled = ctx->Multisample.Enabled;
      attr->SampleAlphaToCoverage = ctx->Multisample.SampleAlphaToCoverage;
      attr->SampleAlphaToOne = ctx->Multisample.SampleAlphaToOne;
      attr->SampleCoverage = ctx->Multisample.SampleCoverage;
      for (i = 0; i < ctx->Const.MaxTextureUnits; i++) {
         attr->Texture[i] = ctx->Texture.FixedFuncUnit[i].Enabled;
         attr->TexGen[i] = ctx->Texture.FixedFuncUnit[i].TexGenEnabled;
      }
      /* GL_ARB_vertex_program */
      attr->VertexProgram = ctx->VertexProgram.Enabled;
      attr->VertexProgramPointSize = ctx->VertexProgram.PointSizeEnabled;
      attr->VertexProgramTwoSide = ctx->VertexProgram.TwoSideEnabled;

      /* GL_ARB_fragment_program */
      attr->FragmentProgram = ctx->FragmentProgram.Enabled;

      /* GL_ARB_framebuffer_sRGB / GL_EXT_framebuffer_sRGB */
      attr->sRGBEnabled = ctx->Color.sRGBEnabled;

      /* GL_NV_conservative_raster */
      attr->ConservativeRasterization = ctx->ConservativeRasterization;
   }

   if (mask & GL_EVAL_BIT)
      memcpy(&head->Eval, &ctx->Eval, sizeof(head->Eval));

   if (mask & GL_FOG_BIT)
      memcpy(&head->Fog, &ctx->Fog, sizeof(head->Fog));

   if (mask & GL_HINT_BIT)
      memcpy(&head->Hint, &ctx->Hint, sizeof(head->Hint));

   if (mask & GL_LIGHTING_BIT) {
      FLUSH_CURRENT(ctx, 0);   /* flush material changes */
      memcpy(&head->Light, &ctx->Light, sizeof(head->Light));
   }

   if (mask & GL_LINE_BIT)
      memcpy(&head->Line, &ctx->Line, sizeof(head->Line));

   if (mask & GL_LIST_BIT)
      memcpy(&head->List, &ctx->List, sizeof(head->List));

   if (mask & GL_PIXEL_MODE_BIT) {
      memcpy(&head->Pixel, &ctx->Pixel, sizeof(struct gl_pixel_attrib));
      /* push the Read FBO's ReadBuffer state, not ctx->Pixel.ReadBuffer */
      head->Pixel.ReadBuffer = ctx->ReadBuffer->ColorReadBuffer;
   }

   if (mask & GL_POINT_BIT)
      memcpy(&head->Point, &ctx->Point, sizeof(head->Point));

   if (mask & GL_POLYGON_BIT)
      memcpy(&head->Polygon, &ctx->Polygon, sizeof(head->Polygon));

   if (mask & GL_POLYGON_STIPPLE_BIT) {
      memcpy(&head->PolygonStipple, &ctx->PolygonStipple,
             sizeof(head->PolygonStipple));
   }

   if (mask & GL_SCISSOR_BIT)
      memcpy(&head->Scissor, &ctx->Scissor, sizeof(head->Scissor));

   if (mask & GL_STENCIL_BUFFER_BIT)
      memcpy(&head->Stencil, &ctx->Stencil, sizeof(head->Stencil));

   if (mask & GL_TEXTURE_BIT) {
      GLuint u, tex;

      _mesa_lock_context_textures(ctx);

      /* copy/save the bulk of texture state here */
      memcpy(&head->Texture.Texture, &ctx->Texture, sizeof(ctx->Texture));

      /* Save references to the currently bound texture objects so they don't
       * accidentally get deleted while referenced in the attribute stack.
       */
      for (u = 0; u < ctx->Const.MaxTextureUnits; u++) {
         for (tex = 0; tex < NUM_TEXTURE_TARGETS; tex++) {
            _mesa_reference_texobj(&head->Texture.SavedTexRef[u][tex],
                                   ctx->Texture.Unit[u].CurrentTex[tex]);
         }
      }

      /* copy state/contents of the currently bound texture objects */
      for (u = 0; u < ctx->Const.MaxTextureUnits; u++) {
         for (tex = 0; tex < NUM_TEXTURE_TARGETS; tex++) {
            _mesa_copy_texture_object(&head->Texture.SavedObj[u][tex],
                                      ctx->Texture.Unit[u].CurrentTex[tex]);
         }
      }

      head->Texture.SharedRef = NULL;
      _mesa_reference_shared_state(ctx, &head->Texture.SharedRef, ctx->Shared);

      _mesa_unlock_context_textures(ctx);
   }

   if (mask & GL_TRANSFORM_BIT)
      memcpy(&head->Transform, &ctx->Transform, sizeof(head->Transform));

   if (mask & GL_VIEWPORT_BIT) {
      memcpy(&head->Viewport.ViewportArray, &ctx->ViewportArray,
             sizeof(struct gl_viewport_attrib)*ctx->Const.MaxViewports);

      head->Viewport.SubpixelPrecisionBias[0] = ctx->SubpixelPrecisionBias[0];
      head->Viewport.SubpixelPrecisionBias[1] = ctx->SubpixelPrecisionBias[1];
   }

   /* GL_ARB_multisample */
   if (mask & GL_MULTISAMPLE_BIT_ARB)
      memcpy(&head->Multisample, &ctx->Multisample, sizeof(head->Multisample));

   ctx->AttribStackDepth++;
}



static void
pop_enable_group(struct gl_context *ctx, const struct gl_enable_attrib_node *enable)
{
   const GLuint curTexUnitSave = ctx->Texture.CurrentUnit;
   GLuint i;

#define TEST_AND_UPDATE(VALUE, NEWVALUE, ENUM)                \
        if ((VALUE) != (NEWVALUE)) {                        \
           _mesa_set_enable(ctx, ENUM, (NEWVALUE));        \
        }

   TEST_AND_UPDATE(ctx->Color.AlphaEnabled, enable->AlphaTest, GL_ALPHA_TEST);
   if (ctx->Color.BlendEnabled != enable->Blend) {
      if (ctx->Extensions.EXT_draw_buffers2) {
         GLuint i;
         for (i = 0; i < ctx->Const.MaxDrawBuffers; i++) {
            _mesa_set_enablei(ctx, GL_BLEND, i, (enable->Blend >> i) & 1);
         }
      }
      else {
         _mesa_set_enable(ctx, GL_BLEND, (enable->Blend & 1));
      }
   }

   for (i=0;i<ctx->Const.MaxClipPlanes;i++) {
      const GLuint mask = 1 << i;
      if ((ctx->Transform.ClipPlanesEnabled & mask) != (enable->ClipPlanes & mask))
          _mesa_set_enable(ctx, (GLenum) (GL_CLIP_PLANE0 + i),
                           !!(enable->ClipPlanes & mask));
   }

   TEST_AND_UPDATE(ctx->Light.ColorMaterialEnabled, enable->ColorMaterial,
                   GL_COLOR_MATERIAL);
   TEST_AND_UPDATE(ctx->Polygon.CullFlag, enable->CullFace, GL_CULL_FACE);

   if (!ctx->Extensions.AMD_depth_clamp_separate) {
      TEST_AND_UPDATE(ctx->Transform.DepthClampNear && ctx->Transform.DepthClampFar,
                      enable->DepthClampNear && enable->DepthClampFar,
                      GL_DEPTH_CLAMP);
   } else {
      TEST_AND_UPDATE(ctx->Transform.DepthClampNear, enable->DepthClampNear,
                      GL_DEPTH_CLAMP_NEAR_AMD);
      TEST_AND_UPDATE(ctx->Transform.DepthClampFar, enable->DepthClampFar,
                      GL_DEPTH_CLAMP_FAR_AMD);
   }

   TEST_AND_UPDATE(ctx->Depth.Test, enable->DepthTest, GL_DEPTH_TEST);
   TEST_AND_UPDATE(ctx->Color.DitherFlag, enable->Dither, GL_DITHER);
   TEST_AND_UPDATE(ctx->Fog.Enabled, enable->Fog, GL_FOG);
   TEST_AND_UPDATE(ctx->Light.Enabled, enable->Lighting, GL_LIGHTING);
   TEST_AND_UPDATE(ctx->Line.SmoothFlag, enable->LineSmooth, GL_LINE_SMOOTH);
   TEST_AND_UPDATE(ctx->Line.StippleFlag, enable->LineStipple,
                   GL_LINE_STIPPLE);
   TEST_AND_UPDATE(ctx->Color.IndexLogicOpEnabled, enable->IndexLogicOp,
                   GL_INDEX_LOGIC_OP);
   TEST_AND_UPDATE(ctx->Color.ColorLogicOpEnabled, enable->ColorLogicOp,
                   GL_COLOR_LOGIC_OP);

   TEST_AND_UPDATE(ctx->Eval.Map1Color4, enable->Map1Color4, GL_MAP1_COLOR_4);
   TEST_AND_UPDATE(ctx->Eval.Map1Index, enable->Map1Index, GL_MAP1_INDEX);
   TEST_AND_UPDATE(ctx->Eval.Map1Normal, enable->Map1Normal, GL_MAP1_NORMAL);
   TEST_AND_UPDATE(ctx->Eval.Map1TextureCoord1, enable->Map1TextureCoord1,
                   GL_MAP1_TEXTURE_COORD_1);
   TEST_AND_UPDATE(ctx->Eval.Map1TextureCoord2, enable->Map1TextureCoord2,
                   GL_MAP1_TEXTURE_COORD_2);
   TEST_AND_UPDATE(ctx->Eval.Map1TextureCoord3, enable->Map1TextureCoord3,
                   GL_MAP1_TEXTURE_COORD_3);
   TEST_AND_UPDATE(ctx->Eval.Map1TextureCoord4, enable->Map1TextureCoord4,
                   GL_MAP1_TEXTURE_COORD_4);
   TEST_AND_UPDATE(ctx->Eval.Map1Vertex3, enable->Map1Vertex3,
                   GL_MAP1_VERTEX_3);
   TEST_AND_UPDATE(ctx->Eval.Map1Vertex4, enable->Map1Vertex4,
                   GL_MAP1_VERTEX_4);

   TEST_AND_UPDATE(ctx->Eval.Map2Color4, enable->Map2Color4, GL_MAP2_COLOR_4);
   TEST_AND_UPDATE(ctx->Eval.Map2Index, enable->Map2Index, GL_MAP2_INDEX);
   TEST_AND_UPDATE(ctx->Eval.Map2Normal, enable->Map2Normal, GL_MAP2_NORMAL);
   TEST_AND_UPDATE(ctx->Eval.Map2TextureCoord1, enable->Map2TextureCoord1,
                   GL_MAP2_TEXTURE_COORD_1);
   TEST_AND_UPDATE(ctx->Eval.Map2TextureCoord2, enable->Map2TextureCoord2,
                   GL_MAP2_TEXTURE_COORD_2);
   TEST_AND_UPDATE(ctx->Eval.Map2TextureCoord3, enable->Map2TextureCoord3,
                   GL_MAP2_TEXTURE_COORD_3);
   TEST_AND_UPDATE(ctx->Eval.Map2TextureCoord4, enable->Map2TextureCoord4,
                   GL_MAP2_TEXTURE_COORD_4);
   TEST_AND_UPDATE(ctx->Eval.Map2Vertex3, enable->Map2Vertex3,
                   GL_MAP2_VERTEX_3);
   TEST_AND_UPDATE(ctx->Eval.Map2Vertex4, enable->Map2Vertex4,
                   GL_MAP2_VERTEX_4);

   TEST_AND_UPDATE(ctx->Eval.AutoNormal, enable->AutoNormal, GL_AUTO_NORMAL);
   TEST_AND_UPDATE(ctx->Transform.Normalize, enable->Normalize, GL_NORMALIZE);
   TEST_AND_UPDATE(ctx->Transform.RescaleNormals, enable->RescaleNormals,
                   GL_RESCALE_NORMAL_EXT);
   TEST_AND_UPDATE(ctx->Transform.RasterPositionUnclipped,
                   enable->RasterPositionUnclipped,
                   GL_RASTER_POSITION_UNCLIPPED_IBM);
   TEST_AND_UPDATE(ctx->Point.SmoothFlag, enable->PointSmooth,
                   GL_POINT_SMOOTH);
   if (ctx->Extensions.NV_point_sprite || ctx->Extensions.ARB_point_sprite) {
      TEST_AND_UPDATE(ctx->Point.PointSprite, enable->PointSprite,
                      GL_POINT_SPRITE_NV);
   }
   TEST_AND_UPDATE(ctx->Polygon.OffsetPoint, enable->PolygonOffsetPoint,
                   GL_POLYGON_OFFSET_POINT);
   TEST_AND_UPDATE(ctx->Polygon.OffsetLine, enable->PolygonOffsetLine,
                   GL_POLYGON_OFFSET_LINE);
   TEST_AND_UPDATE(ctx->Polygon.OffsetFill, enable->PolygonOffsetFill,
                   GL_POLYGON_OFFSET_FILL);
   TEST_AND_UPDATE(ctx->Polygon.SmoothFlag, enable->PolygonSmooth,
                   GL_POLYGON_SMOOTH);
   TEST_AND_UPDATE(ctx->Polygon.StippleFlag, enable->PolygonStipple,
                   GL_POLYGON_STIPPLE);
   if (ctx->Scissor.EnableFlags != enable->Scissor) {
      unsigned i;

      for (i = 0; i < ctx->Const.MaxViewports; i++) {
         _mesa_set_enablei(ctx, GL_SCISSOR_TEST, i, (enable->Scissor >> i) & 1);
      }
   }
   TEST_AND_UPDATE(ctx->Stencil.Enabled, enable->Stencil, GL_STENCIL_TEST);
   if (ctx->Extensions.EXT_stencil_two_side) {
      TEST_AND_UPDATE(ctx->Stencil.TestTwoSide, enable->StencilTwoSide, GL_STENCIL_TEST_TWO_SIDE_EXT);
   }
   TEST_AND_UPDATE(ctx->Multisample.Enabled, enable->MultisampleEnabled,
                   GL_MULTISAMPLE_ARB);
   TEST_AND_UPDATE(ctx->Multisample.SampleAlphaToCoverage,
                   enable->SampleAlphaToCoverage,
                   GL_SAMPLE_ALPHA_TO_COVERAGE_ARB);
   TEST_AND_UPDATE(ctx->Multisample.SampleAlphaToOne,
                   enable->SampleAlphaToOne,
                   GL_SAMPLE_ALPHA_TO_ONE_ARB);
   TEST_AND_UPDATE(ctx->Multisample.SampleCoverage,
                   enable->SampleCoverage,
                   GL_SAMPLE_COVERAGE_ARB);
   /* GL_ARB_vertex_program */
   TEST_AND_UPDATE(ctx->VertexProgram.Enabled,
                   enable->VertexProgram,
                   GL_VERTEX_PROGRAM_ARB);
   TEST_AND_UPDATE(ctx->VertexProgram.PointSizeEnabled,
                   enable->VertexProgramPointSize,
                   GL_VERTEX_PROGRAM_POINT_SIZE_ARB);
   TEST_AND_UPDATE(ctx->VertexProgram.TwoSideEnabled,
                   enable->VertexProgramTwoSide,
                   GL_VERTEX_PROGRAM_TWO_SIDE_ARB);

   /* GL_ARB_fragment_program */
   TEST_AND_UPDATE(ctx->FragmentProgram.Enabled,
                   enable->FragmentProgram,
                   GL_FRAGMENT_PROGRAM_ARB);

   /* GL_ARB_framebuffer_sRGB / GL_EXT_framebuffer_sRGB */
   TEST_AND_UPDATE(ctx->Color.sRGBEnabled, enable->sRGBEnabled,
                   GL_FRAMEBUFFER_SRGB);

   /* GL_NV_conservative_raster */
   if (ctx->Extensions.NV_conservative_raster) {
      TEST_AND_UPDATE(ctx->ConservativeRasterization,
                      enable->ConservativeRasterization,
                      GL_CONSERVATIVE_RASTERIZATION_NV);
   }

   /* texture unit enables */
   for (i = 0; i < ctx->Const.MaxTextureUnits; i++) {
      const GLbitfield enabled = enable->Texture[i];
      const GLbitfield genEnabled = enable->TexGen[i];

      if (ctx->Texture.FixedFuncUnit[i].Enabled != enabled) {
         _mesa_ActiveTexture(GL_TEXTURE0 + i);

         _mesa_set_enable(ctx, GL_TEXTURE_1D, !!(enabled & TEXTURE_1D_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_2D, !!(enabled & TEXTURE_2D_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_3D, !!(enabled & TEXTURE_3D_BIT));
         if (ctx->Extensions.NV_texture_rectangle) {
            _mesa_set_enable(ctx, GL_TEXTURE_RECTANGLE_ARB,
                             !!(enabled & TEXTURE_RECT_BIT));
         }
         if (ctx->Extensions.ARB_texture_cube_map) {
            _mesa_set_enable(ctx, GL_TEXTURE_CUBE_MAP,
                             !!(enabled & TEXTURE_CUBE_BIT));
         }
      }

      if (ctx->Texture.FixedFuncUnit[i].TexGenEnabled != genEnabled) {
         _mesa_ActiveTexture(GL_TEXTURE0 + i);
         _mesa_set_enable(ctx, GL_TEXTURE_GEN_S, !!(genEnabled & S_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_GEN_T, !!(genEnabled & T_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_GEN_R, !!(genEnabled & R_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_GEN_Q, !!(genEnabled & Q_BIT));
      }
   }

   _mesa_ActiveTexture(GL_TEXTURE0 + curTexUnitSave);
}


/**
 * Pop/restore texture attribute/group state.
 */
static void
pop_texture_group(struct gl_context *ctx, struct gl_texture_attrib_node *texstate)
{
   GLuint u;

   _mesa_lock_context_textures(ctx);

   for (u = 0; u < ctx->Const.MaxTextureUnits; u++) {
      const struct gl_fixedfunc_texture_unit *unit =
         &texstate->Texture.FixedFuncUnit[u];
      struct gl_fixedfunc_texture_unit *destUnit =
         &ctx->Texture.FixedFuncUnit[u];
      GLuint tgt;

      _mesa_ActiveTexture(GL_TEXTURE0_ARB + u);

      if (ctx->Driver.TexEnv || ctx->Driver.TexGen) {
         /* Slow path for legacy classic drivers. */
         _mesa_set_enable(ctx, GL_TEXTURE_1D, !!(unit->Enabled & TEXTURE_1D_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_2D, !!(unit->Enabled & TEXTURE_2D_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_3D, !!(unit->Enabled & TEXTURE_3D_BIT));
         if (ctx->Extensions.ARB_texture_cube_map) {
            _mesa_set_enable(ctx, GL_TEXTURE_CUBE_MAP,
                             !!(unit->Enabled & TEXTURE_CUBE_BIT));
         }
         if (ctx->Extensions.NV_texture_rectangle) {
            _mesa_set_enable(ctx, GL_TEXTURE_RECTANGLE_NV,
                             !!(unit->Enabled & TEXTURE_RECT_BIT));
         }

         _mesa_TexGeni(GL_S, GL_TEXTURE_GEN_MODE, unit->GenS.Mode);
         _mesa_TexGeni(GL_T, GL_TEXTURE_GEN_MODE, unit->GenT.Mode);
         _mesa_TexGeni(GL_R, GL_TEXTURE_GEN_MODE, unit->GenR.Mode);
         _mesa_TexGeni(GL_Q, GL_TEXTURE_GEN_MODE, unit->GenQ.Mode);
         _mesa_TexGenfv(GL_S, GL_OBJECT_PLANE, unit->GenS.ObjectPlane);
         _mesa_TexGenfv(GL_T, GL_OBJECT_PLANE, unit->GenT.ObjectPlane);
         _mesa_TexGenfv(GL_R, GL_OBJECT_PLANE, unit->GenR.ObjectPlane);
         _mesa_TexGenfv(GL_Q, GL_OBJECT_PLANE, unit->GenQ.ObjectPlane);
         /* Eye plane done differently to avoid re-transformation */
         {

            COPY_4FV(destUnit->GenS.EyePlane, unit->GenS.EyePlane);
            COPY_4FV(destUnit->GenT.EyePlane, unit->GenT.EyePlane);
            COPY_4FV(destUnit->GenR.EyePlane, unit->GenR.EyePlane);
            COPY_4FV(destUnit->GenQ.EyePlane, unit->GenQ.EyePlane);
            if (ctx->Driver.TexGen) {
               ctx->Driver.TexGen(ctx, GL_S, GL_EYE_PLANE, unit->GenS.EyePlane);
               ctx->Driver.TexGen(ctx, GL_T, GL_EYE_PLANE, unit->GenT.EyePlane);
               ctx->Driver.TexGen(ctx, GL_R, GL_EYE_PLANE, unit->GenR.EyePlane);
               ctx->Driver.TexGen(ctx, GL_Q, GL_EYE_PLANE, unit->GenQ.EyePlane);
            }
         }
         _mesa_set_enable(ctx, GL_TEXTURE_GEN_S, !!(unit->TexGenEnabled & S_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_GEN_T, !!(unit->TexGenEnabled & T_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_GEN_R, !!(unit->TexGenEnabled & R_BIT));
         _mesa_set_enable(ctx, GL_TEXTURE_GEN_Q, !!(unit->TexGenEnabled & Q_BIT));

         _mesa_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, unit->EnvMode);
         _mesa_TexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, unit->EnvColor);
         _mesa_TexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS,
                       texstate->Texture.Unit[u].LodBias);
         _mesa_TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,
                       unit->Combine.ModeRGB);
         _mesa_TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,
                       unit->Combine.ModeA);
         {
            const GLuint n = ctx->Extensions.NV_texture_env_combine4 ? 4 : 3;
            GLuint i;
            for (i = 0; i < n; i++) {
               _mesa_TexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + i,
                             unit->Combine.SourceRGB[i]);
               _mesa_TexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + i,
                             unit->Combine.SourceA[i]);
               _mesa_TexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + i,
                             unit->Combine.OperandRGB[i]);
               _mesa_TexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + i,
                             unit->Combine.OperandA[i]);
            }
         }
         _mesa_TexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE,
                       1 << unit->Combine.ScaleShiftRGB);
         _mesa_TexEnvi(GL_TEXTURE_ENV, GL_ALPHA_SCALE,
                       1 << unit->Combine.ScaleShiftA);
      } else {
         /* Fast path for other drivers. */
         memcpy(destUnit, unit, sizeof(*unit));
         destUnit->_CurrentCombine = NULL;
         ctx->Texture.Unit[u].LodBias = texstate->Texture.Unit[u].LodBias;
      }

      /* Restore texture object state for each target */
      for (tgt = 0; tgt < NUM_TEXTURE_TARGETS; tgt++) {
         const struct gl_texture_object *obj = NULL;
         const struct gl_sampler_object *samp;
         GLenum target;

         obj = &texstate->SavedObj[u][tgt];

         /* don't restore state for unsupported targets to prevent
          * raising GL errors.
          */
         if (obj->Target == GL_TEXTURE_CUBE_MAP &&
             !ctx->Extensions.ARB_texture_cube_map) {
            continue;
         }
         else if (obj->Target == GL_TEXTURE_RECTANGLE_NV &&
                  !ctx->Extensions.NV_texture_rectangle) {
            continue;
         }
         else if ((obj->Target == GL_TEXTURE_1D_ARRAY_EXT ||
                   obj->Target == GL_TEXTURE_2D_ARRAY_EXT) &&
                  !ctx->Extensions.EXT_texture_array) {
            continue;
         }
         else if (obj->Target == GL_TEXTURE_CUBE_MAP_ARRAY &&
             !ctx->Extensions.ARB_texture_cube_map_array) {
            continue;
         } else if (obj->Target == GL_TEXTURE_BUFFER)
            continue;
         else if (obj->Target == GL_TEXTURE_EXTERNAL_OES)
            continue;
         else if (obj->Target == GL_TEXTURE_2D_MULTISAMPLE ||
                  obj->Target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
            continue;

         target = obj->Target;

         _mesa_BindTexture(target, obj->Name);

         samp = &obj->Sampler;

         _mesa_TexParameterfv(target, GL_TEXTURE_BORDER_COLOR, samp->BorderColor.f);
         _mesa_TexParameteri(target, GL_TEXTURE_WRAP_S, samp->WrapS);
         _mesa_TexParameteri(target, GL_TEXTURE_WRAP_T, samp->WrapT);
         _mesa_TexParameteri(target, GL_TEXTURE_WRAP_R, samp->WrapR);
         _mesa_TexParameteri(target, GL_TEXTURE_MIN_FILTER, samp->MinFilter);
         _mesa_TexParameteri(target, GL_TEXTURE_MAG_FILTER, samp->MagFilter);
         _mesa_TexParameterf(target, GL_TEXTURE_MIN_LOD, samp->MinLod);
         _mesa_TexParameterf(target, GL_TEXTURE_MAX_LOD, samp->MaxLod);
         _mesa_TexParameterf(target, GL_TEXTURE_LOD_BIAS, samp->LodBias);
         _mesa_TexParameterf(target, GL_TEXTURE_PRIORITY, obj->Priority);
         _mesa_TexParameteri(target, GL_TEXTURE_BASE_LEVEL, obj->BaseLevel);
         if (target != GL_TEXTURE_RECTANGLE_ARB)
            _mesa_TexParameteri(target, GL_TEXTURE_MAX_LEVEL, obj->MaxLevel);
         if (ctx->Extensions.EXT_texture_filter_anisotropic) {
            _mesa_TexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                samp->MaxAnisotropy);
         }
         if (ctx->Extensions.ARB_shadow) {
            _mesa_TexParameteri(target, GL_TEXTURE_COMPARE_MODE,
                                samp->CompareMode);
            _mesa_TexParameteri(target, GL_TEXTURE_COMPARE_FUNC,
                                samp->CompareFunc);
         }
         if (ctx->Extensions.ARB_depth_texture)
            _mesa_TexParameteri(target, GL_DEPTH_TEXTURE_MODE, obj->DepthMode);
      }

      /* remove saved references to the texture objects */
      for (tgt = 0; tgt < NUM_TEXTURE_TARGETS; tgt++) {
         _mesa_reference_texobj(&texstate->SavedTexRef[u][tgt], NULL);
      }
   }

   if (!ctx->Driver.TexEnv && !ctx->Driver.TexGen) {
      ctx->Texture._TexGenEnabled = texstate->Texture._TexGenEnabled;
      ctx->Texture._GenFlags = texstate->Texture._GenFlags;
   }

   _mesa_ActiveTexture(GL_TEXTURE0_ARB + texstate->Texture.CurrentUnit);

   _mesa_reference_shared_state(ctx, &texstate->SharedRef, NULL);

   _mesa_unlock_context_textures(ctx);
}


/*
 * This function is kind of long just because we have to call a lot
 * of device driver functions to update device driver state.
 *
 * XXX As it is now, most of the pop-code calls immediate-mode Mesa functions
 * in order to restore GL state.  This isn't terribly efficient but it
 * ensures that dirty flags and any derived state gets updated correctly.
 * We could at least check if the value to restore equals the current value
 * and then skip the Mesa call.
 */
void GLAPIENTRY
_mesa_PopAttrib(void)
{
   struct gl_attrib_node *attr;
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_VERTICES(ctx, 0);

   if (ctx->AttribStackDepth == 0) {
      _mesa_error(ctx, GL_STACK_UNDERFLOW, "glPopAttrib");
      return;
   }

   ctx->AttribStackDepth--;
   attr = &ctx->AttribStack[ctx->AttribStackDepth];

   unsigned mask = attr->Mask;

   if (mask & GL_ACCUM_BUFFER_BIT) {
      _mesa_ClearAccum(attr->Accum.ClearColor[0],
                       attr->Accum.ClearColor[1],
                       attr->Accum.ClearColor[2],
                       attr->Accum.ClearColor[3]);
   }

   if (mask & GL_COLOR_BUFFER_BIT) {
      _mesa_ClearIndex((GLfloat) attr->Color.ClearIndex);
      _mesa_ClearColor(attr->Color.ClearColor.f[0],
                       attr->Color.ClearColor.f[1],
                       attr->Color.ClearColor.f[2],
                       attr->Color.ClearColor.f[3]);
      _mesa_IndexMask(attr->Color.IndexMask);
      if (!ctx->Extensions.EXT_draw_buffers2) {
         _mesa_ColorMask(GET_COLORMASK_BIT(attr->Color.ColorMask, 0, 0),
                         GET_COLORMASK_BIT(attr->Color.ColorMask, 0, 1),
                         GET_COLORMASK_BIT(attr->Color.ColorMask, 0, 2),
                         GET_COLORMASK_BIT(attr->Color.ColorMask, 0, 3));
      }
      else {
         GLuint i;
         for (i = 0; i < ctx->Const.MaxDrawBuffers; i++) {
            _mesa_ColorMaski(i,
                             GET_COLORMASK_BIT(attr->Color.ColorMask, i, 0),
                             GET_COLORMASK_BIT(attr->Color.ColorMask, i, 1),
                             GET_COLORMASK_BIT(attr->Color.ColorMask, i, 2),
                             GET_COLORMASK_BIT(attr->Color.ColorMask, i, 3));
         }
      }
      {
         /* Need to determine if more than one color output is
          * specified.  If so, call glDrawBuffersARB, else call
          * glDrawBuffer().  This is a subtle, but essential point
          * since GL_FRONT (for example) is illegal for the former
          * function, but legal for the later.
          */
         GLboolean multipleBuffers = GL_FALSE;
         GLuint i;

         for (i = 1; i < ctx->Const.MaxDrawBuffers; i++) {
            if (attr->Color.DrawBuffer[i] != GL_NONE) {
               multipleBuffers = GL_TRUE;
               break;
            }
         }
         /* Call the API_level functions, not _mesa_drawbuffers()
          * since we need to do error checking on the pop'd
          * GL_DRAW_BUFFER.
          * Ex: if GL_FRONT were pushed, but we're popping with a
          * user FBO bound, GL_FRONT will be illegal and we'll need
          * to record that error.  Per OpenGL ARB decision.
          */
         if (multipleBuffers) {
            GLenum buffers[MAX_DRAW_BUFFERS];

            for (unsigned i = 0; i < ctx->Const.MaxDrawBuffers; i++)
               buffers[i] = attr->Color.DrawBuffer[i];

            _mesa_DrawBuffers(ctx->Const.MaxDrawBuffers, buffers);
         } else {
            _mesa_DrawBuffer(attr->Color.DrawBuffer[0]);
         }
      }
      _mesa_set_enable(ctx, GL_ALPHA_TEST, attr->Color.AlphaEnabled);
      _mesa_AlphaFunc(attr->Color.AlphaFunc, attr->Color.AlphaRefUnclamped);
      if (ctx->Color.BlendEnabled != attr->Color.BlendEnabled) {
         if (ctx->Extensions.EXT_draw_buffers2) {
            GLuint i;
            for (i = 0; i < ctx->Const.MaxDrawBuffers; i++) {
               _mesa_set_enablei(ctx, GL_BLEND, i,
                                 (attr->Color.BlendEnabled >> i) & 1);
            }
         }
         else {
            _mesa_set_enable(ctx, GL_BLEND, (attr->Color.BlendEnabled & 1));
         }
      }
      if (ctx->Color._BlendFuncPerBuffer ||
          ctx->Color._BlendEquationPerBuffer) {
         /* set blend per buffer */
         GLuint buf;
         for (buf = 0; buf < ctx->Const.MaxDrawBuffers; buf++) {
            _mesa_BlendFuncSeparateiARB(buf, attr->Color.Blend[buf].SrcRGB,
                                     attr->Color.Blend[buf].DstRGB,
                                     attr->Color.Blend[buf].SrcA,
                                     attr->Color.Blend[buf].DstA);
            _mesa_BlendEquationSeparateiARB(buf,
                                         attr->Color.Blend[buf].EquationRGB,
                                         attr->Color.Blend[buf].EquationA);
         }
      }
      else {
         /* set same blend modes for all buffers */
         _mesa_BlendFuncSeparate(attr->Color.Blend[0].SrcRGB,
                                    attr->Color.Blend[0].DstRGB,
                                    attr->Color.Blend[0].SrcA,
                                    attr->Color.Blend[0].DstA);
         /* This special case is because glBlendEquationSeparateEXT
          * cannot take GL_LOGIC_OP as a parameter.
          */
         if (attr->Color.Blend[0].EquationRGB ==
             attr->Color.Blend[0].EquationA) {
            _mesa_BlendEquation(attr->Color.Blend[0].EquationRGB);
         }
         else {
            _mesa_BlendEquationSeparate(
                                        attr->Color.Blend[0].EquationRGB,
                                        attr->Color.Blend[0].EquationA);
         }
      }
      _mesa_BlendColor(attr->Color.BlendColorUnclamped[0],
                       attr->Color.BlendColorUnclamped[1],
                       attr->Color.BlendColorUnclamped[2],
                       attr->Color.BlendColorUnclamped[3]);
      _mesa_LogicOp(attr->Color.LogicOp);
      _mesa_set_enable(ctx, GL_COLOR_LOGIC_OP,
                       attr->Color.ColorLogicOpEnabled);
      _mesa_set_enable(ctx, GL_INDEX_LOGIC_OP,
                       attr->Color.IndexLogicOpEnabled);
      _mesa_set_enable(ctx, GL_DITHER, attr->Color.DitherFlag);
      if (ctx->Extensions.ARB_color_buffer_float)
         _mesa_ClampColor(GL_CLAMP_FRAGMENT_COLOR_ARB,
                          attr->Color.ClampFragmentColor);
      if (ctx->Extensions.ARB_color_buffer_float || ctx->Version >= 30)
         _mesa_ClampColor(GL_CLAMP_READ_COLOR_ARB, attr->Color.ClampReadColor);

      /* GL_ARB_framebuffer_sRGB / GL_EXT_framebuffer_sRGB */
      if (ctx->Extensions.EXT_framebuffer_sRGB)
         _mesa_set_enable(ctx, GL_FRAMEBUFFER_SRGB, attr->Color.sRGBEnabled);
   }

   if (mask & GL_CURRENT_BIT) {
      FLUSH_CURRENT(ctx, 0);
      memcpy(&ctx->Current, &attr->Current,
             sizeof(struct gl_current_attrib));
   }

   if (mask & GL_DEPTH_BUFFER_BIT) {
      _mesa_DepthFunc(attr->Depth.Func);
      _mesa_ClearDepth(attr->Depth.Clear);
      _mesa_set_enable(ctx, GL_DEPTH_TEST, attr->Depth.Test);
      _mesa_DepthMask(attr->Depth.Mask);
      if (ctx->Extensions.EXT_depth_bounds_test) {
         _mesa_set_enable(ctx, GL_DEPTH_BOUNDS_TEST_EXT,
                          attr->Depth.BoundsTest);
         _mesa_DepthBoundsEXT(attr->Depth.BoundsMin, attr->Depth.BoundsMax);
      }
   }

   if (mask & GL_ENABLE_BIT) {
      pop_enable_group(ctx, &attr->Enable);
      ctx->NewState |= _NEW_ALL;
      ctx->NewDriverState |= ctx->DriverFlags.NewAlphaTest |
                             ctx->DriverFlags.NewBlend |
                             ctx->DriverFlags.NewClipPlaneEnable |
                             ctx->DriverFlags.NewDepth |
                             ctx->DriverFlags.NewDepthClamp |
                             ctx->DriverFlags.NewFramebufferSRGB |
                             ctx->DriverFlags.NewLineState |
                             ctx->DriverFlags.NewLogicOp |
                             ctx->DriverFlags.NewMultisampleEnable |
                             ctx->DriverFlags.NewPolygonState |
                             ctx->DriverFlags.NewSampleAlphaToXEnable |
                             ctx->DriverFlags.NewSampleMask |
                             ctx->DriverFlags.NewScissorTest |
                             ctx->DriverFlags.NewStencil |
                             ctx->DriverFlags.NewNvConservativeRasterization;
   }

   if (mask & GL_EVAL_BIT) {
      memcpy(&ctx->Eval, &attr->Eval, sizeof(struct gl_eval_attrib));
      vbo_exec_update_eval_maps(ctx);
   }

   if (mask & GL_FOG_BIT) {
      _mesa_set_enable(ctx, GL_FOG, attr->Fog.Enabled);
      _mesa_Fogfv(GL_FOG_COLOR, attr->Fog.Color);
      _mesa_Fogf(GL_FOG_DENSITY, attr->Fog.Density);
      _mesa_Fogf(GL_FOG_START, attr->Fog.Start);
      _mesa_Fogf(GL_FOG_END, attr->Fog.End);
      _mesa_Fogf(GL_FOG_INDEX, attr->Fog.Index);
      _mesa_Fogi(GL_FOG_MODE, attr->Fog.Mode);
   }

   if (mask & GL_HINT_BIT) {
      _mesa_Hint(GL_PERSPECTIVE_CORRECTION_HINT,
                 attr->Hint.PerspectiveCorrection);
      _mesa_Hint(GL_POINT_SMOOTH_HINT, attr->Hint.PointSmooth);
      _mesa_Hint(GL_LINE_SMOOTH_HINT, attr->Hint.LineSmooth);
      _mesa_Hint(GL_POLYGON_SMOOTH_HINT, attr->Hint.PolygonSmooth);
      _mesa_Hint(GL_FOG_HINT, attr->Hint.Fog);
      _mesa_Hint(GL_TEXTURE_COMPRESSION_HINT_ARB,
                 attr->Hint.TextureCompression);
   }

   if (mask & GL_LIGHTING_BIT) {
      GLuint i;
      /* lighting enable */
      _mesa_set_enable(ctx, GL_LIGHTING, attr->Light.Enabled);
      /* per-light state */
      if (_math_matrix_is_dirty(ctx->ModelviewMatrixStack.Top))
         _math_matrix_analyse(ctx->ModelviewMatrixStack.Top);

      for (i = 0; i < ctx->Const.MaxLights; i++) {
         const struct gl_light_uniforms *lu = &attr->Light.LightSource[i];
         const struct gl_light *l = &attr->Light.Light[i];
         _mesa_set_enable(ctx, GL_LIGHT0 + i, l->Enabled);
         _mesa_light(ctx, i, GL_AMBIENT, lu->Ambient);
         _mesa_light(ctx, i, GL_DIFFUSE, lu->Diffuse);
         _mesa_light(ctx, i, GL_SPECULAR, lu->Specular);
         _mesa_light(ctx, i, GL_POSITION, lu->EyePosition);
         _mesa_light(ctx, i, GL_SPOT_DIRECTION, lu->SpotDirection);
         {
            GLfloat p[4] = { 0 };
            p[0] = lu->SpotExponent;
            _mesa_light(ctx, i, GL_SPOT_EXPONENT, p);
         }
         {
            GLfloat p[4] = { 0 };
            p[0] = lu->SpotCutoff;
            _mesa_light(ctx, i, GL_SPOT_CUTOFF, p);
         }
         {
            GLfloat p[4] = { 0 };
            p[0] = lu->ConstantAttenuation;
            _mesa_light(ctx, i, GL_CONSTANT_ATTENUATION, p);
         }
         {
            GLfloat p[4] = { 0 };
            p[0] = lu->LinearAttenuation;
            _mesa_light(ctx, i, GL_LINEAR_ATTENUATION, p);
         }
         {
            GLfloat p[4] = { 0 };
            p[0] = lu->QuadraticAttenuation;
            _mesa_light(ctx, i, GL_QUADRATIC_ATTENUATION, p);
         }
      }
      /* light model */
      _mesa_LightModelfv(GL_LIGHT_MODEL_AMBIENT,
                         attr->Light.Model.Ambient);
      _mesa_LightModelf(GL_LIGHT_MODEL_LOCAL_VIEWER,
                        (GLfloat) attr->Light.Model.LocalViewer);
      _mesa_LightModelf(GL_LIGHT_MODEL_TWO_SIDE,
                        (GLfloat) attr->Light.Model.TwoSide);
      _mesa_LightModelf(GL_LIGHT_MODEL_COLOR_CONTROL,
                        (GLfloat) attr->Light.Model.ColorControl);
      /* shade model */
      _mesa_ShadeModel(attr->Light.ShadeModel);
      /* color material */
      _mesa_ColorMaterial(attr->Light.ColorMaterialFace,
                          attr->Light.ColorMaterialMode);
      _mesa_set_enable(ctx, GL_COLOR_MATERIAL,
                       attr->Light.ColorMaterialEnabled);
      /* materials */
      memcpy(&ctx->Light.Material, &attr->Light.Material,
             sizeof(struct gl_material));
      if (ctx->Extensions.ARB_color_buffer_float) {
         _mesa_ClampColor(GL_CLAMP_VERTEX_COLOR_ARB,
                          attr->Light.ClampVertexColor);
      }
   }

   if (mask & GL_LINE_BIT) {
      _mesa_set_enable(ctx, GL_LINE_SMOOTH, attr->Line.SmoothFlag);
      _mesa_set_enable(ctx, GL_LINE_STIPPLE, attr->Line.StippleFlag);
      _mesa_LineStipple(attr->Line.StippleFactor, attr->Line.StipplePattern);
      _mesa_LineWidth(attr->Line.Width);
   }

   if (mask & GL_LIST_BIT)
      memcpy(&ctx->List, &attr->List, sizeof(struct gl_list_attrib));

   if (mask & GL_PIXEL_MODE_BIT) {
      memcpy(&ctx->Pixel, &attr->Pixel, sizeof(struct gl_pixel_attrib));
      /* XXX what other pixel state needs to be set by function calls? */
      _mesa_ReadBuffer(ctx->Pixel.ReadBuffer);
      ctx->NewState |= _NEW_PIXEL;
   }

   if (mask & GL_POINT_BIT) {
      _mesa_PointSize(attr->Point.Size);
      _mesa_set_enable(ctx, GL_POINT_SMOOTH, attr->Point.SmoothFlag);
      if (ctx->Extensions.EXT_point_parameters) {
         _mesa_PointParameterfv(GL_DISTANCE_ATTENUATION_EXT,
                                attr->Point.Params);
         _mesa_PointParameterf(GL_POINT_SIZE_MIN_EXT,
                               attr->Point.MinSize);
         _mesa_PointParameterf(GL_POINT_SIZE_MAX_EXT,
                               attr->Point.MaxSize);
         _mesa_PointParameterf(GL_POINT_FADE_THRESHOLD_SIZE_EXT,
                               attr->Point.Threshold);
      }
      if (ctx->Extensions.NV_point_sprite
          || ctx->Extensions.ARB_point_sprite) {
         GLuint u;
         for (u = 0; u < ctx->Const.MaxTextureUnits; u++) {
            _mesa_TexEnvi(GL_POINT_SPRITE_NV, GL_COORD_REPLACE_NV,
                          !!(attr->Point.CoordReplace & (1u << u)));
         }
         _mesa_set_enable(ctx, GL_POINT_SPRITE_NV,attr->Point.PointSprite);
         if (ctx->Extensions.NV_point_sprite)
            _mesa_PointParameteri(GL_POINT_SPRITE_R_MODE_NV,
                                  ctx->Point.SpriteRMode);

         if ((ctx->API == API_OPENGL_COMPAT && ctx->Version >= 20)
             || ctx->API == API_OPENGL_CORE)
            _mesa_PointParameterf(GL_POINT_SPRITE_COORD_ORIGIN,
                                  (GLfloat)ctx->Point.SpriteOrigin);
      }
   }

   if (mask & GL_POLYGON_BIT) {
      _mesa_CullFace(attr->Polygon.CullFaceMode);
      _mesa_FrontFace(attr->Polygon.FrontFace);
      _mesa_PolygonMode(GL_FRONT, attr->Polygon.FrontMode);
      _mesa_PolygonMode(GL_BACK, attr->Polygon.BackMode);
      _mesa_polygon_offset_clamp(ctx,
                                 attr->Polygon.OffsetFactor,
                                 attr->Polygon.OffsetUnits,
                                 attr->Polygon.OffsetClamp);
      _mesa_set_enable(ctx, GL_POLYGON_SMOOTH, attr->Polygon.SmoothFlag);
      _mesa_set_enable(ctx, GL_POLYGON_STIPPLE, attr->Polygon.StippleFlag);
      _mesa_set_enable(ctx, GL_CULL_FACE, attr->Polygon.CullFlag);
      _mesa_set_enable(ctx, GL_POLYGON_OFFSET_POINT,
                       attr->Polygon.OffsetPoint);
      _mesa_set_enable(ctx, GL_POLYGON_OFFSET_LINE,
                       attr->Polygon.OffsetLine);
      _mesa_set_enable(ctx, GL_POLYGON_OFFSET_FILL,
                       attr->Polygon.OffsetFill);
   }

   if (mask & GL_POLYGON_STIPPLE_BIT) {
      memcpy(ctx->PolygonStipple, attr->PolygonStipple, 32*sizeof(GLuint));

      if (ctx->DriverFlags.NewPolygonStipple)
         ctx->NewDriverState |= ctx->DriverFlags.NewPolygonStipple;
      else
         ctx->NewState |= _NEW_POLYGONSTIPPLE;

      if (ctx->Driver.PolygonStipple)
         ctx->Driver.PolygonStipple(ctx, (const GLubyte *) attr->PolygonStipple);
   }

   if (mask & GL_SCISSOR_BIT) {
      unsigned i;

      for (i = 0; i < ctx->Const.MaxViewports; i++) {
         _mesa_set_scissor(ctx, i,
                           attr->Scissor.ScissorArray[i].X,
                           attr->Scissor.ScissorArray[i].Y,
                           attr->Scissor.ScissorArray[i].Width,
                           attr->Scissor.ScissorArray[i].Height);
         _mesa_set_enablei(ctx, GL_SCISSOR_TEST, i,
                           (attr->Scissor.EnableFlags >> i) & 1);
      }
      if (ctx->Extensions.EXT_window_rectangles) {
         STATIC_ASSERT(sizeof(struct gl_scissor_rect) ==
                       4 * sizeof(GLint));
         _mesa_WindowRectanglesEXT(
               attr->Scissor.WindowRectMode, attr->Scissor.NumWindowRects,
               (const GLint *)attr->Scissor.WindowRects);
      }
   }

   if (mask & GL_STENCIL_BUFFER_BIT) {
      _mesa_set_enable(ctx, GL_STENCIL_TEST, attr->Stencil.Enabled);
      _mesa_ClearStencil(attr->Stencil.Clear);
      if (ctx->Extensions.EXT_stencil_two_side) {
         _mesa_set_enable(ctx, GL_STENCIL_TEST_TWO_SIDE_EXT,
                          attr->Stencil.TestTwoSide);
         _mesa_ActiveStencilFaceEXT(attr->Stencil.ActiveFace
                                    ? GL_BACK : GL_FRONT);
      }
      /* front state */
      _mesa_StencilFuncSeparate(GL_FRONT,
                                attr->Stencil.Function[0],
                                attr->Stencil.Ref[0],
                                attr->Stencil.ValueMask[0]);
      _mesa_StencilMaskSeparate(GL_FRONT, attr->Stencil.WriteMask[0]);
      _mesa_StencilOpSeparate(GL_FRONT, attr->Stencil.FailFunc[0],
                              attr->Stencil.ZFailFunc[0],
                              attr->Stencil.ZPassFunc[0]);
      /* back state */
      _mesa_StencilFuncSeparate(GL_BACK,
                                attr->Stencil.Function[1],
                                attr->Stencil.Ref[1],
                                attr->Stencil.ValueMask[1]);
      _mesa_StencilMaskSeparate(GL_BACK, attr->Stencil.WriteMask[1]);
      _mesa_StencilOpSeparate(GL_BACK, attr->Stencil.FailFunc[1],
                              attr->Stencil.ZFailFunc[1],
                              attr->Stencil.ZPassFunc[1]);
   }

   if (mask & GL_TRANSFORM_BIT) {
      GLuint i;
      _mesa_MatrixMode(attr->Transform.MatrixMode);
      if (_math_matrix_is_dirty(ctx->ProjectionMatrixStack.Top))
         _math_matrix_analyse(ctx->ProjectionMatrixStack.Top);

      /* restore clip planes */
      for (i = 0; i < ctx->Const.MaxClipPlanes; i++) {
         const GLuint mask = 1 << i;
         const GLfloat *eyePlane = attr->Transform.EyeUserPlane[i];
         COPY_4V(ctx->Transform.EyeUserPlane[i], eyePlane);
         _mesa_set_enable(ctx, GL_CLIP_PLANE0 + i,
                          !!(attr->Transform.ClipPlanesEnabled & mask));
         if (ctx->Driver.ClipPlane)
            ctx->Driver.ClipPlane(ctx, GL_CLIP_PLANE0 + i, eyePlane);
      }

      /* normalize/rescale */
      if (attr->Transform.Normalize != ctx->Transform.Normalize)
         _mesa_set_enable(ctx, GL_NORMALIZE,ctx->Transform.Normalize);
      if (attr->Transform.RescaleNormals != ctx->Transform.RescaleNormals)
         _mesa_set_enable(ctx, GL_RESCALE_NORMAL_EXT,
                          ctx->Transform.RescaleNormals);

      if (!ctx->Extensions.AMD_depth_clamp_separate) {
         if (attr->Transform.DepthClampNear != ctx->Transform.DepthClampNear &&
             attr->Transform.DepthClampFar != ctx->Transform.DepthClampFar) {
            _mesa_set_enable(ctx, GL_DEPTH_CLAMP,
                             ctx->Transform.DepthClampNear &&
                             ctx->Transform.DepthClampFar);
         }
      } else {
         if (attr->Transform.DepthClampNear != ctx->Transform.DepthClampNear)
            _mesa_set_enable(ctx, GL_DEPTH_CLAMP_NEAR_AMD,
                             ctx->Transform.DepthClampNear);
         if (attr->Transform.DepthClampFar != ctx->Transform.DepthClampFar)
            _mesa_set_enable(ctx, GL_DEPTH_CLAMP_FAR_AMD,
                             ctx->Transform.DepthClampFar);
      }

      if (ctx->Extensions.ARB_clip_control)
         _mesa_ClipControl(attr->Transform.ClipOrigin, attr->Transform.ClipDepthMode);
   }

   if (mask & GL_TEXTURE_BIT) {
      pop_texture_group(ctx, &attr->Texture);
      ctx->NewState |= _NEW_TEXTURE_OBJECT | _NEW_TEXTURE_STATE;
   }

   if (mask & GL_VIEWPORT_BIT) {
      unsigned i;

      for (i = 0; i < ctx->Const.MaxViewports; i++) {
         const struct gl_viewport_attrib *vp = &attr->Viewport.ViewportArray[i];
         _mesa_set_viewport(ctx, i, vp->X, vp->Y, vp->Width,
                            vp->Height);
         _mesa_set_depth_range(ctx, i, vp->Near, vp->Far);
      }

      if (ctx->Extensions.NV_conservative_raster) {
         GLuint biasx = attr->Viewport.SubpixelPrecisionBias[0];
         GLuint biasy = attr->Viewport.SubpixelPrecisionBias[1];
         _mesa_SubpixelPrecisionBiasNV(biasx, biasy);
      }
   }

   if (mask & GL_MULTISAMPLE_BIT_ARB) {
      TEST_AND_UPDATE(ctx->Multisample.Enabled,
                      attr->Multisample.Enabled,
                      GL_MULTISAMPLE);

      TEST_AND_UPDATE(ctx->Multisample.SampleCoverage,
                      attr->Multisample.SampleCoverage,
                      GL_SAMPLE_COVERAGE);

      TEST_AND_UPDATE(ctx->Multisample.SampleAlphaToCoverage,
                      attr->Multisample.SampleAlphaToCoverage,
                      GL_SAMPLE_ALPHA_TO_COVERAGE);

      TEST_AND_UPDATE(ctx->Multisample.SampleAlphaToOne,
                      attr->Multisample.SampleAlphaToOne,
                      GL_SAMPLE_ALPHA_TO_ONE);

      _mesa_SampleCoverage(attr->Multisample.SampleCoverageValue,
                              attr->Multisample.SampleCoverageInvert);

      _mesa_AlphaToCoverageDitherControlNV(attr->Multisample.SampleAlphaToCoverageDitherControl);
   }
}


/**
 * Copy gl_pixelstore_attrib from src to dst, updating buffer
 * object refcounts.
 */
static void
copy_pixelstore(struct gl_context *ctx,
                struct gl_pixelstore_attrib *dst,
                const struct gl_pixelstore_attrib *src)
{
   dst->Alignment = src->Alignment;
   dst->RowLength = src->RowLength;
   dst->SkipPixels = src->SkipPixels;
   dst->SkipRows = src->SkipRows;
   dst->ImageHeight = src->ImageHeight;
   dst->SkipImages = src->SkipImages;
   dst->SwapBytes = src->SwapBytes;
   dst->LsbFirst = src->LsbFirst;
   dst->Invert = src->Invert;
   _mesa_reference_buffer_object(ctx, &dst->BufferObj, src->BufferObj);
}


#define GL_CLIENT_PACK_BIT (1<<20)
#define GL_CLIENT_UNPACK_BIT (1<<21)

/**
 * Copy gl_vertex_array_object from src to dest.
 * 'dest' must be in an initialized state.
 */
static void
copy_array_object(struct gl_context *ctx,
                  struct gl_vertex_array_object *dest,
                  struct gl_vertex_array_object *src)
{
   GLuint i;

   /* skip Name */
   /* skip RefCount */

   for (i = 0; i < ARRAY_SIZE(src->VertexAttrib); i++) {
      _mesa_copy_vertex_attrib_array(ctx, &dest->VertexAttrib[i], &src->VertexAttrib[i]);
      _mesa_copy_vertex_buffer_binding(ctx, &dest->BufferBinding[i], &src->BufferBinding[i]);
   }

   /* Enabled must be the same than on push */
   dest->Enabled = src->Enabled;
   dest->_EffEnabledVBO = src->_EffEnabledVBO;
   dest->_EffEnabledNonZeroDivisor = src->_EffEnabledNonZeroDivisor;
   /* The bitmask of bound VBOs needs to match the VertexBinding array */
   dest->VertexAttribBufferMask = src->VertexAttribBufferMask;
   dest->NonZeroDivisorMask = src->NonZeroDivisorMask;
   dest->_AttributeMapMode = src->_AttributeMapMode;
   dest->NewArrays = src->NewArrays;
   dest->NumUpdates = src->NumUpdates;
   dest->IsDynamic = src->IsDynamic;
}

/**
 * Copy gl_array_attrib from src to dest.
 * 'dest' must be in an initialized state.
 */
static void
copy_array_attrib(struct gl_context *ctx,
                  struct gl_array_attrib *dest,
                  struct gl_array_attrib *src,
                  bool vbo_deleted)
{
   /* skip ArrayObj */
   /* skip DefaultArrayObj, Objects */
   dest->ActiveTexture = src->ActiveTexture;
   dest->LockFirst = src->LockFirst;
   dest->LockCount = src->LockCount;
   dest->PrimitiveRestart = src->PrimitiveRestart;
   dest->PrimitiveRestartFixedIndex = src->PrimitiveRestartFixedIndex;
   dest->RestartIndex = src->RestartIndex;
   memcpy(dest->_PrimitiveRestart, src->_PrimitiveRestart,
          sizeof(src->_PrimitiveRestart));
   memcpy(dest->_RestartIndex, src->_RestartIndex, sizeof(src->_RestartIndex));
   /* skip NewState */
   /* skip RebindArrays */

   if (!vbo_deleted)
      copy_array_object(ctx, dest->VAO, src->VAO);

   /* skip ArrayBufferObj */
   /* skip IndexBufferObj */

   /* Invalidate array state. It will be updated during the next draw. */
   _mesa_set_draw_vao(ctx, ctx->Array._EmptyVAO, 0);
}

/**
 * Save the content of src to dest.
 */
static void
save_array_attrib(struct gl_context *ctx,
                  struct gl_array_attrib *dest,
                  struct gl_array_attrib *src)
{
   /* Set the Name, needed for restore, but do never overwrite.
    * Needs to match value in the object hash. */
   dest->VAO->Name = src->VAO->Name;
   /* And copy all of the rest. */
   copy_array_attrib(ctx, dest, src, false);

   /* Just reference them here */
   _mesa_reference_buffer_object(ctx, &dest->ArrayBufferObj,
                                 src->ArrayBufferObj);
   _mesa_reference_buffer_object(ctx, &dest->VAO->IndexBufferObj,
                                 src->VAO->IndexBufferObj);
}

/**
 * Restore the content of src to dest.
 */
static void
restore_array_attrib(struct gl_context *ctx,
                     struct gl_array_attrib *dest,
                     struct gl_array_attrib *src)
{
   bool is_vao_name_zero = src->VAO->Name == 0;

   /* The ARB_vertex_array_object spec says:
    *
    *     "BindVertexArray fails and an INVALID_OPERATION error is generated
    *     if array is not a name returned from a previous call to
    *     GenVertexArrays, or if such a name has since been deleted with
    *     DeleteVertexArrays."
    *
    * Therefore popping a deleted VAO cannot magically recreate it.
    */
   if (!is_vao_name_zero && !_mesa_IsVertexArray(src->VAO->Name))
      return;

   _mesa_BindVertexArray(src->VAO->Name);

   /* Restore or recreate the buffer objects by the names ... */
   if (is_vao_name_zero || !src->ArrayBufferObj ||
       _mesa_IsBuffer(src->ArrayBufferObj->Name)) {
      /* ... and restore its content */
      copy_array_attrib(ctx, dest, src, false);

      _mesa_BindBuffer(GL_ARRAY_BUFFER_ARB,
                       src->ArrayBufferObj ?
                          src->ArrayBufferObj->Name : 0);
   } else {
      copy_array_attrib(ctx, dest, src, true);
   }

   if (is_vao_name_zero || !src->VAO->IndexBufferObj ||
       _mesa_IsBuffer(src->VAO->IndexBufferObj->Name)) {
      _mesa_BindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB,
                       src->VAO->IndexBufferObj ?
                          src->VAO->IndexBufferObj->Name : 0);
   }
}


void GLAPIENTRY
_mesa_PushClientAttrib(GLbitfield mask)
{
   struct gl_client_attrib_node *head;

   GET_CURRENT_CONTEXT(ctx);

   if (ctx->ClientAttribStackDepth >= MAX_CLIENT_ATTRIB_STACK_DEPTH) {
      _mesa_error(ctx, GL_STACK_OVERFLOW, "glPushClientAttrib");
      return;
   }

   head = &ctx->ClientAttribStack[ctx->ClientAttribStackDepth];
   head->Mask = mask;

   if (mask & GL_CLIENT_PIXEL_STORE_BIT) {
      copy_pixelstore(ctx, &head->Pack, &ctx->Pack);
      copy_pixelstore(ctx, &head->Unpack, &ctx->Unpack);
   }

   if (mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
      _mesa_initialize_vao(ctx, &head->VAO, 0);
      /* Use the VAO declared within the node instead of allocating it. */
      head->Array.VAO = &head->VAO;
      save_array_attrib(ctx, &head->Array, &ctx->Array);
   }

   ctx->ClientAttribStackDepth++;
}


void GLAPIENTRY
_mesa_PopClientAttrib(void)
{
   struct gl_client_attrib_node *head;

   GET_CURRENT_CONTEXT(ctx);
   FLUSH_VERTICES(ctx, 0);

   if (ctx->ClientAttribStackDepth == 0) {
      _mesa_error(ctx, GL_STACK_UNDERFLOW, "glPopClientAttrib");
      return;
   }

   ctx->ClientAttribStackDepth--;
   head = &ctx->ClientAttribStack[ctx->ClientAttribStackDepth];

   if (head->Mask & GL_CLIENT_PIXEL_STORE_BIT) {
      copy_pixelstore(ctx, &ctx->Pack, &head->Pack);
      _mesa_reference_buffer_object(ctx, &head->Pack.BufferObj, NULL);

      copy_pixelstore(ctx, &ctx->Unpack, &head->Unpack);
      _mesa_reference_buffer_object(ctx, &head->Unpack.BufferObj, NULL);
   }

   if (head->Mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
      restore_array_attrib(ctx, &ctx->Array, &head->Array);
      _mesa_unbind_array_object_vbos(ctx, &head->VAO);
      _mesa_reference_buffer_object(ctx, &head->VAO.IndexBufferObj, NULL);
      _mesa_reference_buffer_object(ctx, &head->Array.ArrayBufferObj, NULL);
   }
}

void GLAPIENTRY
_mesa_ClientAttribDefaultEXT( GLbitfield mask )
{
   if (mask & GL_CLIENT_PIXEL_STORE_BIT) {
      _mesa_PixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
      _mesa_PixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
      _mesa_PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
      _mesa_PixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
      _mesa_PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      _mesa_PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
      _mesa_PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
      _mesa_PixelStorei(GL_UNPACK_ALIGNMENT, 4);
      _mesa_PixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
      _mesa_PixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
      _mesa_PixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
      _mesa_PixelStorei(GL_PACK_SKIP_IMAGES, 0);
      _mesa_PixelStorei(GL_PACK_ROW_LENGTH, 0);
      _mesa_PixelStorei(GL_PACK_SKIP_ROWS, 0);
      _mesa_PixelStorei(GL_PACK_SKIP_PIXELS, 0);
      _mesa_PixelStorei(GL_PACK_ALIGNMENT, 4);

      _mesa_BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      _mesa_BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
   }
   if (mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
      GET_CURRENT_CONTEXT(ctx);
      int i;

      _mesa_BindBuffer(GL_ARRAY_BUFFER, 0);
      _mesa_BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

      _mesa_DisableClientState(GL_EDGE_FLAG_ARRAY);
      _mesa_EdgeFlagPointer(0, 0);

      _mesa_DisableClientState(GL_INDEX_ARRAY);
      _mesa_IndexPointer(GL_FLOAT, 0, 0);

      _mesa_DisableClientState(GL_SECONDARY_COLOR_ARRAY);
      _mesa_SecondaryColorPointer(4, GL_FLOAT, 0, 0);

      _mesa_DisableClientState(GL_FOG_COORD_ARRAY);
      _mesa_FogCoordPointer(GL_FLOAT, 0, 0);

      for (i = 0; i < ctx->Const.MaxTextureCoordUnits; i++) {
         _mesa_ClientActiveTexture(GL_TEXTURE0 + i);
         _mesa_DisableClientState(GL_TEXTURE_COORD_ARRAY);
         _mesa_TexCoordPointer(4, GL_FLOAT, 0, 0);
      }

      _mesa_DisableClientState(GL_COLOR_ARRAY);
      _mesa_ColorPointer(4, GL_FLOAT, 0, 0);

      _mesa_DisableClientState(GL_NORMAL_ARRAY);
      _mesa_NormalPointer(GL_FLOAT, 0, 0);

      _mesa_DisableClientState(GL_VERTEX_ARRAY);
      _mesa_VertexPointer(4, GL_FLOAT, 0, 0);

      for (i = 0; i < ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs; i++) {
         _mesa_DisableVertexAttribArray(i);
         _mesa_VertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, 0, 0);
      }

      _mesa_ClientActiveTexture(GL_TEXTURE0);

      _mesa_PrimitiveRestartIndex_no_error(0);
      if (ctx->Version >= 31)
         _mesa_Disable(GL_PRIMITIVE_RESTART);
      else if (_mesa_has_NV_primitive_restart(ctx))
         _mesa_DisableClientState(GL_PRIMITIVE_RESTART_NV);

      if (_mesa_has_ARB_ES3_compatibility(ctx))
         _mesa_Disable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
   }
}

void GLAPIENTRY
_mesa_PushClientAttribDefaultEXT( GLbitfield mask )
{
   _mesa_PushClientAttrib(mask);
   _mesa_ClientAttribDefaultEXT(mask);
}


/**
 * Free any attribute state data that might be attached to the context.
 */
void
_mesa_free_attrib_data(struct gl_context *ctx)
{
   while (ctx->AttribStackDepth > 0) {
      struct gl_attrib_node *attr;

      ctx->AttribStackDepth--;
      attr = &ctx->AttribStack[ctx->AttribStackDepth];

      if (attr->Mask & GL_TEXTURE_BIT) {
         /* clear references to the saved texture objects */
         for (unsigned u = 0; u < ctx->Const.MaxTextureUnits; u++) {
            for (unsigned tgt = 0; tgt < NUM_TEXTURE_TARGETS; tgt++) {
               _mesa_reference_texobj(&attr->Texture.SavedTexRef[u][tgt], NULL);
            }
         }
         _mesa_reference_shared_state(ctx, &attr->Texture.SharedRef, NULL);
      }
   }
}


void
_mesa_init_attrib(struct gl_context *ctx)
{
   /* Renderer and client attribute stacks */
   ctx->AttribStackDepth = 0;
   ctx->ClientAttribStackDepth = 0;
}

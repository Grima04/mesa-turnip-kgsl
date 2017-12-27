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
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_transfer.h"
#include "intel/compiler/brw_compiler.h"
#include "iris_context.h"

#define __gen_address_type unsigned
#define __gen_user_data void

static uint64_t
__gen_combine_address(void *user_data, void *location,
                      unsigned address, uint32_t delta)
{
   return delta;
}

#define __genxml_cmd_length(cmd) cmd ## _length
#define __genxml_cmd_length_bias(cmd) cmd ## _length_bias
#define __genxml_cmd_header(cmd) cmd ## _header
#define __genxml_cmd_pack(cmd) cmd ## _pack

#define iris_pack_command(cmd, dst, name)                         \
   for (struct cmd name = { __genxml_cmd_header(cmd) },           \
        *_dst = (void *)(dst); __builtin_expect(_dst != NULL, 1); \
        __genxml_cmd_pack(cmd)(NULL, (void *)dst, &name),         \
        _dst = NULL)

#define iris_pack_state(cmd, dst, name)                           \
   for (struct cmd name = {},                                     \
        *_dst = (void *)(dst); __builtin_expect(_dst != NULL, 1); \
        __genxml_cmd_pack(cmd)(NULL, (void *)_dst, &name),        \
        _dst = NULL)

#include "genxml/genX_pack.h"
#include "genxml/gen_macros.h"

static void
iris_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *info)
{
   struct iris_context *ice = (struct iris_context *) ctx;

   iris_upload_render_state(ice);

#if 0
   l3 configuration

   3DSTATE_VIEWPORT_STATE_POINTERS_CC - CC_VIEWPORT
     -> from iris_depth_stencil_alpha_state

   3DSTATE_VIEWPORT_STATE_POINTERS_SF_CL - SF_CLIP_VIEWPORT
     -> pipe_viewport_state for matrix elements, guardband is calculated
        from those.  can calculate screen space from matrix apparently...

   3DSTATE_SCISSOR_STATE_POINTERS - SCISSOR_RECT
     -> from ice->state.scissors

   3DSTATE_PUSH_CONSTANT_ALLOC_*
   3DSTATE_URB_*
     -> TODO

   3DSTATE_PS_BLEND
   3DSTATE_BLEND_STATE_POINTERS - BLEND_STATE
     -> from iris_blend_state (most) + iris_depth_stencil_alpha_state
        (alpha test function/enable) + has writeable RT from ???????

   3DSTATE_CC_STATE_POINTERS - COLOR_CALC_STATE
     -> from ice->state.blend_color + iris_depth_stencil_alpha_state
        (ref_value)

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

   3DSTATE_SAMPLER_STATE_POINTERS_*
     -> TODO

   3DSTATE_MULTISAMPLE
   3DSTATE_SAMPLE_MASK

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

   3DSTATE_CLIP
     -> iris_raster_state + ??? (Non-perspective Bary, ForceZeroRTAIndex)

   3DSTATE_RASTER
   3DSTATE_SF
     -> iris_raster_state

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

   3DSTATE_VF_TOPOLOGY
     -> pipe_draw_info (prim_mode)
   3DSTATE_VF
     -> pipe_draw_info (restart_index, primitive_restart)

   3DSTATE_INDEX_BUFFER
     -> pipe_draw_info (index)
   3DSTATE_VERTEX_BUFFERS
     -> pipe_vertex_buffer (set_vertex_buffer hook)
   3DSTATE_VERTEX_ELEMENTS
     -> iris_vertex_element
   3DSTATE_VF_INSTANCING
     -> iris_vertex_element
   3DSTATE_VF_SGVS
     -> TODO ???
   3DSTATE_VF_COMPONENT_PACKING
     -> TODO ???

   3DPRIMITIVE
     -> pipe_draw_info

   rare:
   3DSTATE_POLY_STIPPLE_OFFSET
   3DSTATE_POLY_STIPPLE_PATTERN
     -> ice->state.poly_stipple
   3DSTATE_LINE_STIPPLE
     -> iris_raster_state

   once:
   3DSTATE_AA_LINE_PARAMETERS
   3DSTATE_WM_CHROMAKEY
   3DSTATE_SAMPLE_PATTERN
   3DSTATE_DRAWING_RECTANGLE
   3DSTATE_WM_HZ_OP
#endif
}

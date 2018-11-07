/*
 * Copyright © 2017 Intel Corporation
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
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iris_state.c
 *
 * ============================= GENXML CODE =============================
 *              [This file is compiled once per generation.]
 * =======================================================================
 *
 * This is the main state upload code.
 *
 * Gallium uses Constant State Objects, or CSOs, for most state.  Large,
 * complex, or highly reusable state can be created once, and bound and
 * rebound multiple times.  This is modeled with the pipe->create_*_state()
 * and pipe->bind_*_state() hooks.  Highly dynamic or inexpensive state is
 * streamed out on the fly, via pipe->set_*_state() hooks.
 *
 * OpenGL involves frequently mutating context state, which is mirrored in
 * core Mesa by highly mutable data structures.  However, most applications
 * typically draw the same things over and over - from frame to frame, most
 * of the same objects are still visible and need to be redrawn.  So, rather
 * than inventing new state all the time, applications usually mutate to swap
 * between known states that we've seen before.
 *
 * Gallium isolates us from this mutation by tracking API state, and
 * distilling it into a set of Constant State Objects, or CSOs.  Large,
 * complex, or typically reusable state can be created once, then reused
 * multiple times.  Drivers can create and store their own associated data.
 * This create/bind model corresponds to the pipe->create_*_state() and
 * pipe->bind_*_state() driver hooks.
 *
 * Some state is cheap to create, or expected to be highly dynamic.  Rather
 * than creating and caching piles of CSOs for these, Gallium simply streams
 * them out, via the pipe->set_*_state() driver hooks.
 *
 * To reduce draw time overhead, we try to compute as much state at create
 * time as possible.  Wherever possible, we translate the Gallium pipe state
 * to 3DSTATE commands, and store those commands in the CSO.  At draw time,
 * we can simply memcpy them into a batch buffer.
 *
 * No hardware matches the abstraction perfectly, so some commands require
 * information from multiple CSOs.  In this case, we can store two copies
 * of the packet (one in each CSO), and simply | together their DWords at
 * draw time.  Sometimes the second set is trivial (one or two fields), so
 * we simply pack it at draw time.
 *
 * There are two main components in the file below.  First, the CSO hooks
 * create/bind/track state.  The second are the draw-time upload functions,
 * iris_upload_render_state() and iris_upload_compute_state(), which read
 * the context state and emit the commands into the actual batch.
 */

#include <stdio.h>
#include <errno.h>

#if HAVE_VALGRIND
#include <valgrind.h>
#include <memcheck.h>
#define VG(x) x
#ifndef NDEBUG
#define __gen_validate_value(x) VALGRIND_CHECK_MEM_IS_DEFINED(&(x), sizeof(x))
#endif
#else
#define VG(x)
#endif

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_framebuffer.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"
#include "util/u_viewport.h"
#include "i915_drm.h"
#include "nir.h"
#include "intel/compiler/brw_compiler.h"
#include "intel/common/gen_l3_config.h"
#include "intel/common/gen_sample_positions.h"
#include "iris_batch.h"
#include "iris_context.h"
#include "iris_pipe.h"
#include "iris_resource.h"

#define __gen_address_type struct iris_address
#define __gen_user_data struct iris_batch

#define ARRAY_BYTES(x) (sizeof(uint32_t) * ARRAY_SIZE(x))

static uint64_t
__gen_combine_address(struct iris_batch *batch, void *location,
                      struct iris_address addr, uint32_t delta)
{
   uint64_t result = addr.offset + delta;

   if (addr.bo) {
      iris_use_pinned_bo(batch, addr.bo, addr.write);
      /* Assume this is a general address, not relative to a base. */
      result += addr.bo->gtt_offset;
   }

   return result;
}

#define __genxml_cmd_length(cmd) cmd ## _length
#define __genxml_cmd_length_bias(cmd) cmd ## _length_bias
#define __genxml_cmd_header(cmd) cmd ## _header
#define __genxml_cmd_pack(cmd) cmd ## _pack

#define _iris_pack_command(batch, cmd, dst, name)                 \
   for (struct cmd name = { __genxml_cmd_header(cmd) },           \
        *_dst = (void *)(dst); __builtin_expect(_dst != NULL, 1); \
        ({ __genxml_cmd_pack(cmd)(batch, (void *)_dst, &name);    \
           _dst = NULL;                                           \
           }))

#define iris_pack_command(cmd, dst, name) \
   _iris_pack_command(NULL, cmd, dst, name)

#define iris_pack_state(cmd, dst, name)                           \
   for (struct cmd name = {},                                     \
        *_dst = (void *)(dst); __builtin_expect(_dst != NULL, 1); \
        __genxml_cmd_pack(cmd)(NULL, (void *)_dst, &name),        \
        _dst = NULL)

#define iris_emit_cmd(batch, cmd, name) \
   _iris_pack_command(batch, cmd, iris_get_command_space(batch, 4 * __genxml_cmd_length(cmd)), name)

#define iris_emit_merge(batch, dwords0, dwords1, num_dwords)   \
   do {                                                        \
      uint32_t *dw = iris_get_command_space(batch, 4 * num_dwords); \
      for (uint32_t i = 0; i < num_dwords; i++)                \
         dw[i] = (dwords0)[i] | (dwords1)[i];                  \
      VG(VALGRIND_CHECK_MEM_IS_DEFINED(dw, num_dwords));       \
   } while (0)

#include "genxml/genX_pack.h"
#include "genxml/gen_macros.h"
#include "genxml/genX_bits.h"

#define MOCS_WB (2 << 1)

/**
 * Statically assert that PIPE_* enums match the hardware packets.
 * (As long as they match, we don't need to translate them.)
 */
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

   /* pipe_sprite_coord_mode happens to match 3DSTATE_SBE */
   PIPE_ASSERT(PIPE_SPRITE_COORD_UPPER_LEFT == UPPERLEFT);
   PIPE_ASSERT(PIPE_SPRITE_COORD_LOWER_LEFT == LOWERLEFT);
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

      /* These are unsupported. */
      [PIPE_TEX_WRAP_MIRROR_CLAMP]           = -1,
      [PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER] = -1,
   };
   return map[pipe_wrap];
}

static struct iris_address
ro_bo(struct iris_bo *bo, uint64_t offset)
{
   /* CSOs must pass NULL for bo!  Otherwise it will add the BO to the
    * validation list at CSO creation time, instead of draw time.
    */
   return (struct iris_address) { .bo = bo, .offset = offset };
}

static struct iris_address
rw_bo(struct iris_bo *bo, uint64_t offset)
{
   /* CSOs must pass NULL for bo!  Otherwise it will add the BO to the
    * validation list at CSO creation time, instead of draw time.
    */
   return (struct iris_address) { .bo = bo, .offset = offset, .write = true };
}

/**
 * Allocate space for some indirect state.
 *
 * Return a pointer to the map (to fill it out) and a state ref (for
 * referring to the state in GPU commands).
 */
static void *
upload_state(struct u_upload_mgr *uploader,
             struct iris_state_ref *ref,
             unsigned size,
             unsigned alignment)
{
   void *p = NULL;
   u_upload_alloc(uploader, 0, size, alignment, &ref->offset, &ref->res, &p);
   return p;
}

/**
 * Stream out temporary/short-lived state.
 *
 * This allocates space, pins the BO, and includes the BO address in the
 * returned offset (which works because all state lives in 32-bit memory
 * zones).
 */
static uint32_t *
stream_state(struct iris_batch *batch,
             struct u_upload_mgr *uploader,
             struct pipe_resource **out_res,
             unsigned size,
             unsigned alignment,
             uint32_t *out_offset)
{
   void *ptr = NULL;

   u_upload_alloc(uploader, 0, size, alignment, out_offset, out_res, &ptr);

   struct iris_bo *bo = iris_resource_bo(*out_res);
   iris_use_pinned_bo(batch, bo, false);

   *out_offset += iris_bo_offset_from_base_address(bo);

   return ptr;
}

/**
 * stream_state() + memcpy.
 */
static uint32_t
emit_state(struct iris_batch *batch,
           struct u_upload_mgr *uploader,
           struct pipe_resource **out_res,
           const void *data,
           unsigned size,
           unsigned alignment)
{
   unsigned offset = 0;
   uint32_t *map =
      stream_state(batch, uploader, out_res, size, alignment, &offset);

   if (map)
      memcpy(map, data, size);

   return offset;
}

/**
 * Did field 'x' change between 'old_cso' and 'new_cso'?
 *
 * (If so, we may want to set some dirty flags.)
 */
#define cso_changed(x) (!old_cso || (old_cso->x != new_cso->x))
#define cso_changed_memcmp(x) \
   (!old_cso || memcmp(old_cso->x, new_cso->x, sizeof(old_cso->x)) != 0)

static void
flush_for_state_base_change(struct iris_batch *batch)
{
   /* Flush before emitting STATE_BASE_ADDRESS.
    *
    * This isn't documented anywhere in the PRM.  However, it seems to be
    * necessary prior to changing the surface state base adress.  We've
    * seen issues in Vulkan where we get GPU hangs when using multi-level
    * command buffers which clear depth, reset state base address, and then
    * go render stuff.
    *
    * Normally, in GL, we would trust the kernel to do sufficient stalls
    * and flushes prior to executing our batch.  However, it doesn't seem
    * as if the kernel's flushing is always sufficient and we don't want to
    * rely on it.
    *
    * We make this an end-of-pipe sync instead of a normal flush because we
    * do not know the current status of the GPU.  On Haswell at least,
    * having a fast-clear operation in flight at the same time as a normal
    * rendering operation can cause hangs.  Since the kernel's flushing is
    * insufficient, we need to ensure that any rendering operations from
    * other processes are definitely complete before we try to do our own
    * rendering.  It's a bit of a big hammer but it appears to work.
    */
   iris_emit_end_of_pipe_sync(batch,
                              PIPE_CONTROL_RENDER_TARGET_FLUSH |
                              PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                              PIPE_CONTROL_DATA_CACHE_FLUSH);
}

static void
_iris_emit_lri(struct iris_batch *batch, uint32_t reg, uint32_t val)
{
   iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
      lri.RegisterOffset = reg;
      lri.DataDWord      = val;
   }
}
#define iris_emit_lri(b, r, v) _iris_emit_lri(b, GENX(r##_num), v)

static void
emit_pipeline_select(struct iris_batch *batch, uint32_t pipeline)
{
#if GEN_GEN >= 8 && GEN_GEN < 10
   /* From the Broadwell PRM, Volume 2a: Instructions, PIPELINE_SELECT:
    *
    *   Software must clear the COLOR_CALC_STATE Valid field in
    *   3DSTATE_CC_STATE_POINTERS command prior to send a PIPELINE_SELECT
    *   with Pipeline Select set to GPGPU.
    *
    * The internal hardware docs recommend the same workaround for Gen9
    * hardware too.
    */
   if (pipeline == GPGPU)
      iris_emit_cmd(batch, GENX(3DSTATE_CC_STATE_POINTERS), t);
#endif


   /* From "BXML » GT » MI » vol1a GPU Overview » [Instruction]
    * PIPELINE_SELECT [DevBWR+]":
    *
    *    "Project: DEVSNB+
    *
    *     Software must ensure all the write caches are flushed through a
    *     stalling PIPE_CONTROL command followed by another PIPE_CONTROL
    *     command to invalidate read only caches prior to programming
    *     MI_PIPELINE_SELECT command to change the Pipeline Select Mode."
    */
    iris_emit_pipe_control_flush(batch,
                                 PIPE_CONTROL_RENDER_TARGET_FLUSH |
                                 PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                                 PIPE_CONTROL_DATA_CACHE_FLUSH |
                                 PIPE_CONTROL_CS_STALL);

    iris_emit_pipe_control_flush(batch,
                                 PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
                                 PIPE_CONTROL_CONST_CACHE_INVALIDATE |
                                 PIPE_CONTROL_STATE_CACHE_INVALIDATE |
                                 PIPE_CONTROL_INSTRUCTION_INVALIDATE);

   iris_emit_cmd(batch, GENX(PIPELINE_SELECT), sel) {
#if GEN_GEN >= 9
      sel.MaskBits = 3;
#endif
      sel.PipelineSelection = pipeline;
   }
}

UNUSED static void
init_glk_barrier_mode(struct iris_batch *batch, uint32_t value)
{
#if GEN_GEN == 9
   /* Project: DevGLK
    *
    *    "This chicken bit works around a hardware issue with barrier
    *     logic encountered when switching between GPGPU and 3D pipelines.
    *     To workaround the issue, this mode bit should be set after a
    *     pipeline is selected."
    */
   uint32_t reg_val;
   iris_pack_state(GENX(SLICE_COMMON_ECO_CHICKEN1), &reg_val, reg) {
      reg.GLKBarrierMode = value;
      reg.GLKBarrierModeMask = 1;
   }
   iris_emit_lri(batch, SLICE_COMMON_ECO_CHICKEN1, reg_val);
#endif
}

static void
init_state_base_address(struct iris_batch *batch)
{
   flush_for_state_base_change(batch);

   /* We program most base addresses once at context initialization time.
    * Each base address points at a 4GB memory zone, and never needs to
    * change.  See iris_bufmgr.h for a description of the memory zones.
    *
    * The one exception is Surface State Base Address, which needs to be
    * updated occasionally.  See iris_binder.c for the details there.
    */
   iris_emit_cmd(batch, GENX(STATE_BASE_ADDRESS), sba) {
   #if 0
   // XXX: MOCS is stupid for this.
      sba.GeneralStateMemoryObjectControlState            = MOCS_WB;
      sba.StatelessDataPortAccessMemoryObjectControlState = MOCS_WB;
      sba.DynamicStateMemoryObjectControlState            = MOCS_WB;
      sba.IndirectObjectMemoryObjectControlState          = MOCS_WB;
      sba.InstructionMemoryObjectControlState             = MOCS_WB;
      sba.BindlessSurfaceStateMemoryObjectControlState    = MOCS_WB;
   #endif

      sba.GeneralStateBaseAddressModifyEnable   = true;
      sba.DynamicStateBaseAddressModifyEnable   = true;
      sba.IndirectObjectBaseAddressModifyEnable = true;
      sba.InstructionBaseAddressModifyEnable    = true;
      sba.GeneralStateBufferSizeModifyEnable    = true;
      sba.DynamicStateBufferSizeModifyEnable    = true;
      sba.BindlessSurfaceStateBaseAddressModifyEnable = true;
      sba.IndirectObjectBufferSizeModifyEnable  = true;
      sba.InstructionBuffersizeModifyEnable     = true;

      sba.InstructionBaseAddress  = ro_bo(NULL, IRIS_MEMZONE_SHADER_START);
      sba.DynamicStateBaseAddress = ro_bo(NULL, IRIS_MEMZONE_DYNAMIC_START);

      sba.GeneralStateBufferSize   = 0xfffff;
      sba.IndirectObjectBufferSize = 0xfffff;
      sba.InstructionBufferSize    = 0xfffff;
      sba.DynamicStateBufferSize   = 0xfffff;
   }
}

/**
 * Upload the initial GPU state for a render context.
 *
 * This sets some invariant state that needs to be programmed a particular
 * way, but we never actually change.
 */
static void
iris_init_render_context(struct iris_screen *screen,
                         struct iris_batch *batch,
                         struct iris_vtable *vtbl,
                         struct pipe_debug_callback *dbg)
{
   UNUSED const struct gen_device_info *devinfo = &screen->devinfo;
   uint32_t reg_val;

   emit_pipeline_select(batch, _3D);

   init_state_base_address(batch);

   // XXX: INSTPM on Gen8
   iris_pack_state(GENX(CS_DEBUG_MODE2), &reg_val, reg) {
      reg.CONSTANT_BUFFERAddressOffsetDisable = true;
      reg.CONSTANT_BUFFERAddressOffsetDisableMask = true;
   }
   iris_emit_lri(batch, CS_DEBUG_MODE2, reg_val);

#if GEN_GEN == 9
   iris_pack_state(GENX(CACHE_MODE_1), &reg_val, reg) {
      reg.FloatBlendOptimizationEnable = true;
      reg.FloatBlendOptimizationEnableMask = true;
      reg.PartialResolveDisableInVC = true;
      reg.PartialResolveDisableInVCMask = true;
   }
   iris_emit_lri(batch, CACHE_MODE_1, reg_val);

   if (devinfo->is_geminilake)
      init_glk_barrier_mode(batch, GLK_BARRIER_MODE_3D_HULL);
#endif

#if GEN_GEN == 11
      iris_pack_state(GENX(SAMPLER_MODE), &reg_val, reg) {
         reg.HeaderlessMessageforPreemptableContexts = 1;
         reg.HeaderlessMessageforPreemptableContextsMask = 1;
      }
      iris_emit_lri(batch, SAMPLER_MODE, reg_val);

      // XXX: 3D_MODE?
#endif

   /* 3DSTATE_DRAWING_RECTANGLE is non-pipelined, so we want to avoid
    * changing it dynamically.  We set it to the maximum size here, and
    * instead include the render target dimensions in the viewport, so
    * viewport extents clipping takes care of pruning stray geometry.
    */
   iris_emit_cmd(batch, GENX(3DSTATE_DRAWING_RECTANGLE), rect) {
      rect.ClippedDrawingRectangleXMax = UINT16_MAX;
      rect.ClippedDrawingRectangleYMax = UINT16_MAX;
   }

   /* Set the initial MSAA sample positions. */
   iris_emit_cmd(batch, GENX(3DSTATE_SAMPLE_PATTERN), pat) {
      GEN_SAMPLE_POS_1X(pat._1xSample);
      GEN_SAMPLE_POS_2X(pat._2xSample);
      GEN_SAMPLE_POS_4X(pat._4xSample);
      GEN_SAMPLE_POS_8X(pat._8xSample);
      GEN_SAMPLE_POS_16X(pat._16xSample);
   }

   /* Use the legacy AA line coverage computation. */
   iris_emit_cmd(batch, GENX(3DSTATE_AA_LINE_PARAMETERS), foo);

   /* Disable chromakeying (it's for media) */
   iris_emit_cmd(batch, GENX(3DSTATE_WM_CHROMAKEY), foo);

   /* We want regular rendering, not special HiZ operations. */
   iris_emit_cmd(batch, GENX(3DSTATE_WM_HZ_OP), foo);

   /* No polygon stippling offsets are necessary. */
   // XXX: may need to set an offset for origin-UL framebuffers
   iris_emit_cmd(batch, GENX(3DSTATE_POLY_STIPPLE_OFFSET), foo);

   /* Set a static partitioning of the push constant area. */
   // XXX: this may be a bad idea...could starve the push ringbuffers...
   for (int i = 0; i <= MESA_SHADER_FRAGMENT; i++) {
      iris_emit_cmd(batch, GENX(3DSTATE_PUSH_CONSTANT_ALLOC_VS), alloc) {
         alloc._3DCommandSubOpcode = 18 + i;
         alloc.ConstantBufferOffset = 6 * i;
         alloc.ConstantBufferSize = i == MESA_SHADER_FRAGMENT ? 8 : 6;
      }
   }
}

static void
iris_init_compute_context(struct iris_screen *screen,
                          struct iris_batch *batch,
                          struct iris_vtable *vtbl,
                          struct pipe_debug_callback *dbg)
{
   UNUSED const struct gen_device_info *devinfo = &screen->devinfo;

   emit_pipeline_select(batch, GPGPU);

   init_state_base_address(batch);

#if GEN_GEN == 9
   if (devinfo->is_geminilake)
      init_glk_barrier_mode(batch, GLK_BARRIER_MODE_GPGPU);
#endif
}

struct iris_vertex_buffer_state {
   /** The 3DSTATE_VERTEX_BUFFERS hardware packet. */
   uint32_t vertex_buffers[1 + 33 * GENX(VERTEX_BUFFER_STATE_length)];

   /** The resource to source vertex data from. */
   struct pipe_resource *resources[33];

   /** The number of bound vertex buffers. */
   unsigned num_buffers;
};

struct iris_depth_buffer_state {
   /* Depth/HiZ/Stencil related hardware packets. */
   uint32_t packets[GENX(3DSTATE_DEPTH_BUFFER_length) +
                    GENX(3DSTATE_STENCIL_BUFFER_length) +
                    GENX(3DSTATE_HIER_DEPTH_BUFFER_length) +
                    GENX(3DSTATE_CLEAR_PARAMS_length)];
};

/**
 * Generation-specific context state (ice->state.genx->...).
 *
 * Most state can go in iris_context directly, but these encode hardware
 * packets which vary by generation.
 */
struct iris_genx_state {
   /** SF_CLIP_VIEWPORT */
   uint32_t sf_cl_vp[GENX(SF_CLIP_VIEWPORT_length) * IRIS_MAX_VIEWPORTS];

   struct iris_vertex_buffer_state vertex_buffers;
   struct iris_depth_buffer_state depth_buffer;

   uint32_t so_buffers[4 * GENX(3DSTATE_SO_BUFFER_length)];
   uint32_t streamout[4 * GENX(3DSTATE_STREAMOUT_length)];
};

/**
 * The pipe->set_blend_color() driver hook.
 *
 * This corresponds to our COLOR_CALC_STATE.
 */
static void
iris_set_blend_color(struct pipe_context *ctx,
                     const struct pipe_blend_color *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;

   /* Our COLOR_CALC_STATE is exactly pipe_blend_color, so just memcpy */
   memcpy(&ice->state.blend_color, state, sizeof(struct pipe_blend_color));
   ice->state.dirty |= IRIS_DIRTY_COLOR_CALC_STATE;
}

/**
 * Gallium CSO for blend state (see pipe_blend_state).
 */
struct iris_blend_state {
   /** Partial 3DSTATE_PS_BLEND */
   uint32_t ps_blend[GENX(3DSTATE_PS_BLEND_length)];

   /** Partial BLEND_STATE */
   uint32_t blend_state[GENX(BLEND_STATE_length) +
                        BRW_MAX_DRAW_BUFFERS * GENX(BLEND_STATE_ENTRY_length)];

   bool alpha_to_coverage; /* for shader key */
};

/**
 * The pipe->create_blend_state() driver hook.
 *
 * Translates a pipe_blend_state into iris_blend_state.
 */
static void *
iris_create_blend_state(struct pipe_context *ctx,
                        const struct pipe_blend_state *state)
{
   struct iris_blend_state *cso = malloc(sizeof(struct iris_blend_state));
   uint32_t *blend_state = cso->blend_state;

   cso->alpha_to_coverage = state->alpha_to_coverage;

   iris_pack_command(GENX(3DSTATE_PS_BLEND), cso->ps_blend, pb) {
      /* pb.HasWriteableRT is filled in at draw time. */
      /* pb.AlphaTestEnable is filled in at draw time. */
      pb.AlphaToCoverageEnable = state->alpha_to_coverage;
      pb.IndependentAlphaBlendEnable = state->independent_blend_enable;

      pb.ColorBufferBlendEnable = state->rt[0].blend_enable;

      pb.SourceBlendFactor           = state->rt[0].rgb_src_factor;
      pb.SourceAlphaBlendFactor      = state->rt[0].alpha_func;
      pb.DestinationBlendFactor      = state->rt[0].rgb_dst_factor;
      pb.DestinationAlphaBlendFactor = state->rt[0].alpha_dst_factor;
   }

   iris_pack_state(GENX(BLEND_STATE), blend_state, bs) {
      bs.AlphaToCoverageEnable = state->alpha_to_coverage;
      bs.IndependentAlphaBlendEnable = state->independent_blend_enable;
      bs.AlphaToOneEnable = state->alpha_to_one;
      bs.AlphaToCoverageDitherEnable = state->alpha_to_coverage;
      bs.ColorDitherEnable = state->dither;
      /* bl.AlphaTestEnable and bs.AlphaTestFunction are filled in later. */
   }

   blend_state += GENX(BLEND_STATE_length);

   for (int i = 0; i < BRW_MAX_DRAW_BUFFERS; i++) {
      iris_pack_state(GENX(BLEND_STATE_ENTRY), blend_state, be) {
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

         be.WriteDisableRed   = !(state->rt[i].colormask & PIPE_MASK_R);
         be.WriteDisableGreen = !(state->rt[i].colormask & PIPE_MASK_G);
         be.WriteDisableBlue  = !(state->rt[i].colormask & PIPE_MASK_B);
         be.WriteDisableAlpha = !(state->rt[i].colormask & PIPE_MASK_A);
      }
      blend_state += GENX(BLEND_STATE_ENTRY_length);
   }

   return cso;
}

/**
 * The pipe->bind_blend_state() driver hook.
 *
 * Bind a blending CSO and flag related dirty bits.
 */
static void
iris_bind_blend_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   ice->state.cso_blend = state;
   ice->state.dirty |= IRIS_DIRTY_PS_BLEND;
   ice->state.dirty |= IRIS_DIRTY_BLEND_STATE;
   ice->state.dirty |= ice->state.dirty_for_nos[IRIS_NOS_BLEND];
}

/**
 * Gallium CSO for depth, stencil, and alpha testing state.
 */
struct iris_depth_stencil_alpha_state {
   /** Partial 3DSTATE_WM_DEPTH_STENCIL. */
   uint32_t wmds[GENX(3DSTATE_WM_DEPTH_STENCIL_length)];

   /** Outbound to BLEND_STATE, 3DSTATE_PS_BLEND, COLOR_CALC_STATE. */
   struct pipe_alpha_state alpha;

   /** Outbound to resolve and cache set tracking. */
   bool depth_writes_enabled;
   bool stencil_writes_enabled;
};

/**
 * The pipe->create_depth_stencil_alpha_state() driver hook.
 *
 * We encode most of 3DSTATE_WM_DEPTH_STENCIL, and just save off the alpha
 * testing state since we need pieces of it in a variety of places.
 */
static void *
iris_create_zsa_state(struct pipe_context *ctx,
                      const struct pipe_depth_stencil_alpha_state *state)
{
   struct iris_depth_stencil_alpha_state *cso =
      malloc(sizeof(struct iris_depth_stencil_alpha_state));

   bool two_sided_stencil = state->stencil[1].enabled;

   cso->alpha = state->alpha;
   cso->depth_writes_enabled = state->depth.writemask;
   cso->stencil_writes_enabled =
      state->stencil[0].writemask != 0 ||
      (two_sided_stencil && state->stencil[1].writemask != 1);

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

   return cso;
}

/**
 * The pipe->bind_depth_stencil_alpha_state() driver hook.
 *
 * Bind a depth/stencil/alpha CSO and flag related dirty bits.
 */
static void
iris_bind_zsa_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_depth_stencil_alpha_state *old_cso = ice->state.cso_zsa;
   struct iris_depth_stencil_alpha_state *new_cso = state;

   if (new_cso) {
      if (cso_changed(alpha.ref_value))
         ice->state.dirty |= IRIS_DIRTY_COLOR_CALC_STATE;

      if (cso_changed(alpha.enabled))
         ice->state.dirty |= IRIS_DIRTY_PS_BLEND | IRIS_DIRTY_BLEND_STATE;

      if (cso_changed(alpha.func))
         ice->state.dirty |= IRIS_DIRTY_BLEND_STATE;

      ice->state.depth_writes_enabled = new_cso->depth_writes_enabled;
      ice->state.stencil_writes_enabled = new_cso->stencil_writes_enabled;
   }

   ice->state.cso_zsa = new_cso;
   ice->state.dirty |= IRIS_DIRTY_CC_VIEWPORT;
   ice->state.dirty |= IRIS_DIRTY_WM_DEPTH_STENCIL;
   ice->state.dirty |= ice->state.dirty_for_nos[IRIS_NOS_DEPTH_STENCIL_ALPHA];
}

/**
 * Gallium CSO for rasterizer state.
 */
struct iris_rasterizer_state {
   uint32_t sf[GENX(3DSTATE_SF_length)];
   uint32_t clip[GENX(3DSTATE_CLIP_length)];
   uint32_t raster[GENX(3DSTATE_RASTER_length)];
   uint32_t wm[GENX(3DSTATE_WM_length)];
   uint32_t line_stipple[GENX(3DSTATE_LINE_STIPPLE_length)];

   bool clip_halfz; /* for CC_VIEWPORT */
   bool depth_clip_near; /* for CC_VIEWPORT */
   bool depth_clip_far; /* for CC_VIEWPORT */
   bool flatshade; /* for shader state */
   bool flatshade_first; /* for stream output */
   bool clamp_fragment_color; /* for shader state */
   bool light_twoside; /* for shader state */
   bool rasterizer_discard; /* for 3DSTATE_STREAMOUT */
   bool half_pixel_center; /* for 3DSTATE_MULTISAMPLE */
   bool line_stipple_enable;
   bool poly_stipple_enable;
   bool multisample;
   bool force_persample_interp;
   enum pipe_sprite_coord_mode sprite_coord_mode; /* PIPE_SPRITE_* */
   uint16_t sprite_coord_enable;
};

static float
get_line_width(const struct pipe_rasterizer_state *state)
{
   float line_width = state->line_width;

   /* From the OpenGL 4.4 spec:
    *
    * "The actual width of non-antialiased lines is determined by rounding
    *  the supplied width to the nearest integer, then clamping it to the
    *  implementation-dependent maximum non-antialiased line width."
    */
   if (!state->multisample && !state->line_smooth)
      line_width = roundf(state->line_width);

   if (!state->multisample && state->line_smooth && line_width < 1.5f) {
      /* For 1 pixel line thickness or less, the general anti-aliasing
       * algorithm gives up, and a garbage line is generated.  Setting a
       * Line Width of 0.0 specifies the rasterization of the "thinnest"
       * (one-pixel-wide), non-antialiased lines.
       *
       * Lines rendered with zero Line Width are rasterized using the
       * "Grid Intersection Quantization" rules as specified by the
       * "Zero-Width (Cosmetic) Line Rasterization" section of the docs.
       */
      line_width = 0.0f;
   }

   return line_width;
}

/**
 * The pipe->create_rasterizer_state() driver hook.
 */
static void *
iris_create_rasterizer_state(struct pipe_context *ctx,
                             const struct pipe_rasterizer_state *state)
{
   struct iris_rasterizer_state *cso =
      malloc(sizeof(struct iris_rasterizer_state));

#if 0
   point_quad_rasterization -> SBE?

   not necessary?
   {
      poly_smooth
      force_persample_interp - ?
      bottom_edge_rule

      offset_units_unscaled - cap not exposed
   }
   #endif

   // XXX: it may make more sense just to store the pipe_rasterizer_state,
   // we're copying a lot of booleans here.  But we don't need all of them...

   cso->multisample = state->multisample;
   cso->force_persample_interp = state->force_persample_interp;
   cso->clip_halfz = state->clip_halfz;
   cso->depth_clip_near = state->depth_clip_near;
   cso->depth_clip_far = state->depth_clip_far;
   cso->flatshade = state->flatshade;
   cso->flatshade_first = state->flatshade_first;
   cso->clamp_fragment_color = state->clamp_fragment_color;
   cso->light_twoside = state->light_twoside;
   cso->rasterizer_discard = state->rasterizer_discard;
   cso->half_pixel_center = state->half_pixel_center;
   cso->sprite_coord_mode = state->sprite_coord_mode;
   cso->sprite_coord_enable = state->sprite_coord_enable;
   cso->line_stipple_enable = state->line_stipple_enable;
   cso->poly_stipple_enable = state->poly_stipple_enable;

   float line_width = get_line_width(state);

   iris_pack_command(GENX(3DSTATE_SF), cso->sf, sf) {
      sf.StatisticsEnable = true;
      sf.ViewportTransformEnable = true;
      sf.AALineDistanceMode = AALINEDISTANCE_TRUE;
      sf.LineEndCapAntialiasingRegionWidth =
         state->line_smooth ? _10pixels : _05pixels;
      sf.LastPixelEnable = state->line_last_pixel;
      sf.LineWidth = line_width;
      sf.SmoothPointEnable = state->point_smooth;
      sf.PointWidthSource = state->point_size_per_vertex ? Vertex : State;
      sf.PointWidth = state->point_size;

      if (state->flatshade_first) {
         sf.TriangleFanProvokingVertexSelect = 1;
      } else {
         sf.TriangleStripListProvokingVertexSelect = 2;
         sf.TriangleFanProvokingVertexSelect = 2;
         sf.LineStripListProvokingVertexSelect = 1;
      }
   }

   iris_pack_command(GENX(3DSTATE_RASTER), cso->raster, rr) {
      rr.FrontWinding = state->front_ccw ? CounterClockwise : Clockwise;
      rr.CullMode = translate_cull_mode(state->cull_face);
      rr.FrontFaceFillMode = translate_fill_mode(state->fill_front);
      rr.BackFaceFillMode = translate_fill_mode(state->fill_back);
      rr.DXMultisampleRasterizationEnable = state->multisample;
      rr.GlobalDepthOffsetEnableSolid = state->offset_tri;
      rr.GlobalDepthOffsetEnableWireframe = state->offset_line;
      rr.GlobalDepthOffsetEnablePoint = state->offset_point;
      rr.GlobalDepthOffsetConstant = state->offset_units * 2;
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
      /* cl.NonPerspectiveBarycentricEnable is filled in at draw time from
       * the FS program; cl.ForceZeroRTAIndexEnable is filled in from the FB.
       */
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

      if (state->flatshade_first) {
         cl.TriangleFanProvokingVertexSelect = 1;
      } else {
         cl.TriangleStripListProvokingVertexSelect = 2;
         cl.TriangleFanProvokingVertexSelect = 2;
         cl.LineStripListProvokingVertexSelect = 1;
      }
   }

   iris_pack_command(GENX(3DSTATE_WM), cso->wm, wm) {
      /* wm.BarycentricInterpolationMode and wm.EarlyDepthStencilControl are
       * filled in at draw time from the FS program.
       */
      wm.LineAntialiasingRegionWidth = _10pixels;
      wm.LineEndCapAntialiasingRegionWidth = _05pixels;
      wm.PointRasterizationRule = RASTRULE_UPPER_RIGHT;
      wm.StatisticsEnable = true;
      wm.LineStippleEnable = state->line_stipple_enable;
      wm.PolygonStippleEnable = state->poly_stipple_enable;
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

/**
 * The pipe->bind_rasterizer_state() driver hook.
 *
 * Bind a rasterizer CSO and flag related dirty bits.
 */
static void
iris_bind_rasterizer_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_rasterizer_state *old_cso = ice->state.cso_rast;
   struct iris_rasterizer_state *new_cso = state;

   if (new_cso) {
      /* Try to avoid re-emitting 3DSTATE_LINE_STIPPLE, it's non-pipelined */
      if (cso_changed_memcmp(line_stipple))
         ice->state.dirty |= IRIS_DIRTY_LINE_STIPPLE;

      if (cso_changed(half_pixel_center))
         ice->state.dirty |= IRIS_DIRTY_MULTISAMPLE;

      if (cso_changed(line_stipple_enable) || cso_changed(poly_stipple_enable))
         ice->state.dirty |= IRIS_DIRTY_WM;

      if (cso_changed(rasterizer_discard) || cso_changed(flatshade_first))
         ice->state.dirty |= IRIS_DIRTY_STREAMOUT;

      if (cso_changed(depth_clip_near) || cso_changed(depth_clip_far) ||
          cso_changed(clip_halfz))
         ice->state.dirty |= IRIS_DIRTY_CC_VIEWPORT;

      if (cso_changed(sprite_coord_enable) || cso_changed(light_twoside))
         ice->state.dirty |= IRIS_DIRTY_SBE;
   }

   ice->state.cso_rast = new_cso;
   ice->state.dirty |= IRIS_DIRTY_RASTER;
   ice->state.dirty |= IRIS_DIRTY_CLIP;
   ice->state.dirty |= ice->state.dirty_for_nos[IRIS_NOS_RASTERIZER];
}

/**
 * Return true if the given wrap mode requires the border color to exist.
 *
 * (We can skip uploading it if the sampler isn't going to use it.)
 */
static bool
wrap_mode_needs_border_color(unsigned wrap_mode)
{
   return wrap_mode == TCM_CLAMP_BORDER || wrap_mode == TCM_HALF_BORDER;
}

/**
 * Gallium CSO for sampler state.
 */
struct iris_sampler_state {
   union pipe_color_union border_color;
   bool needs_border_color;

   uint32_t sampler_state[GENX(SAMPLER_STATE_length)];
};

/**
 * The pipe->create_sampler_state() driver hook.
 *
 * We fill out SAMPLER_STATE (except for the border color pointer), and
 * store that on the CPU.  It doesn't make sense to upload it to a GPU
 * buffer object yet, because 3DSTATE_SAMPLER_STATE_POINTERS requires
 * all bound sampler states to be in contiguous memor.
 */
static void *
iris_create_sampler_state(struct pipe_context *ctx,
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

   memcpy(&cso->border_color, &state->border_color, sizeof(cso->border_color));

   cso->needs_border_color = wrap_mode_needs_border_color(wrap_s) ||
                             wrap_mode_needs_border_color(wrap_t) ||
                             wrap_mode_needs_border_color(wrap_r);

   float min_lod = state->min_lod;
   unsigned mag_img_filter = state->mag_img_filter;

   // XXX: explain this code ported from ilo...I don't get it at all...
   if (state->min_mip_filter == PIPE_TEX_MIPFILTER_NONE &&
       state->min_lod > 0.0f) {
      min_lod = 0.0f;
      mag_img_filter = state->min_img_filter;
   }

   iris_pack_state(GENX(SAMPLER_STATE), cso->sampler_state, samp) {
      samp.TCXAddressControlMode = wrap_s;
      samp.TCYAddressControlMode = wrap_t;
      samp.TCZAddressControlMode = wrap_r;
      samp.CubeSurfaceControlMode = state->seamless_cube_map;
      samp.NonnormalizedCoordinateEnable = !state->normalized_coords;
      samp.MinModeFilter = state->min_img_filter;
      samp.MagModeFilter = mag_img_filter;
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
      samp.MinLOD = CLAMP(min_lod, 0, hw_max_lod);
      samp.MaxLOD = CLAMP(state->max_lod, 0, hw_max_lod);
      samp.TextureLODBias = CLAMP(state->lod_bias, -16, 15);

      /* .BorderColorPointer is filled in by iris_bind_sampler_states. */
   }

   return cso;
}

/**
 * The pipe->bind_sampler_states() driver hook.
 *
 * Now that we know all the sampler states, we upload them all into a
 * contiguous area of GPU memory, for 3DSTATE_SAMPLER_STATE_POINTERS_*.
 * We also fill out the border color state pointers at this point.
 *
 * We could defer this work to draw time, but we assume that binding
 * will be less frequent than drawing.
 */
// XXX: this may be a bad idea, need to make sure that st/mesa calls us
// XXX: with the complete set of shaders.  If it makes multiple calls to
// XXX: things one at a time, we could waste a lot of time assembling things.
// XXX: it doesn't even BUY us anything to do it here, because we only flag
// XXX: IRIS_DIRTY_SAMPLER_STATE when this is called...
static void
iris_bind_sampler_states(struct pipe_context *ctx,
                         enum pipe_shader_type p_stage,
                         unsigned start, unsigned count,
                         void **states)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   gl_shader_stage stage = stage_from_pipe(p_stage);
   struct iris_shader_state *shs = &ice->state.shaders[stage];

   assert(start + count <= IRIS_MAX_TEXTURE_SAMPLERS);
   shs->num_samplers = MAX2(shs->num_samplers, start + count);

   for (int i = 0; i < count; i++) {
      shs->samplers[start + i] = states[i];
   }

   /* Assemble the SAMPLER_STATEs into a contiguous table that lives
    * in the dynamic state memory zone, so we can point to it via the
    * 3DSTATE_SAMPLER_STATE_POINTERS_* commands.
    */
   uint32_t *map =
      upload_state(ice->state.dynamic_uploader, &shs->sampler_table,
                   count * 4 * GENX(SAMPLER_STATE_length), 32);
   if (unlikely(!map))
      return;

   struct pipe_resource *res = shs->sampler_table.res;
   shs->sampler_table.offset +=
      iris_bo_offset_from_base_address(iris_resource_bo(res));

   /* Make sure all land in the same BO */
   iris_border_color_pool_reserve(ice, IRIS_MAX_TEXTURE_SAMPLERS);

   for (int i = 0; i < count; i++) {
      struct iris_sampler_state *state = shs->samplers[i];

      if (!state) {
         memset(map, 0, 4 * GENX(SAMPLER_STATE_length));
      } else if (!state->needs_border_color) {
         memcpy(map, state->sampler_state, 4 * GENX(SAMPLER_STATE_length));
      } else {
         ice->state.need_border_colors = true;

         /* Stream out the border color and merge the pointer. */
         uint32_t offset =
            iris_upload_border_color(ice, &state->border_color);

         uint32_t dynamic[GENX(SAMPLER_STATE_length)];
         iris_pack_state(GENX(SAMPLER_STATE), dynamic, dyns) {
            dyns.BorderColorPointer = offset;
         }

         for (uint32_t j = 0; j < GENX(SAMPLER_STATE_length); j++)
            map[j] = state->sampler_state[j] | dynamic[j];
      }

      map += GENX(SAMPLER_STATE_length);
   }

   ice->state.dirty |= IRIS_DIRTY_SAMPLER_STATES_VS << stage;
}

static enum isl_channel_select
fmt_swizzle(const struct iris_format_info *fmt, enum pipe_swizzle swz)
{
   switch (swz) {
   case PIPE_SWIZZLE_X: return fmt->swizzle.r;
   case PIPE_SWIZZLE_Y: return fmt->swizzle.g;
   case PIPE_SWIZZLE_Z: return fmt->swizzle.b;
   case PIPE_SWIZZLE_W: return fmt->swizzle.a;
   case PIPE_SWIZZLE_1: return SCS_ONE;
   case PIPE_SWIZZLE_0: return SCS_ZERO;
   default: unreachable("invalid swizzle");
   }
}

static void
fill_buffer_surface_state(struct isl_device *isl_dev,
                          struct iris_bo *bo,
                          void *map,
                          enum isl_format format,
                          unsigned offset,
                          unsigned size)
{
   const struct isl_format_layout *fmtl = isl_format_get_layout(format);
   const unsigned cpp = fmtl->bpb / 8;

   /* The ARB_texture_buffer_specification says:
    *
    *    "The number of texels in the buffer texture's texel array is given by
    *
    *       floor(<buffer_size> / (<components> * sizeof(<base_type>)),
    *
    *     where <buffer_size> is the size of the buffer object, in basic
    *     machine units and <components> and <base_type> are the element count
    *     and base data type for elements, as specified in Table X.1.  The
    *     number of texels in the texel array is then clamped to the
    *     implementation-dependent limit MAX_TEXTURE_BUFFER_SIZE_ARB."
    *
    * We need to clamp the size in bytes to MAX_TEXTURE_BUFFER_SIZE * stride,
    * so that when ISL divides by stride to obtain the number of texels, that
    * texel count is clamped to MAX_TEXTURE_BUFFER_SIZE.
    */
   unsigned final_size =
      MIN3(size, bo->size - offset, IRIS_MAX_TEXTURE_BUFFER_SIZE * cpp);

   isl_buffer_fill_state(isl_dev, map,
                         .address = bo->gtt_offset + offset,
                         .size_B = final_size,
                         .format = format,
                         .stride_B = cpp,
                         .mocs = MOCS_WB);
}

/**
 * The pipe->create_sampler_view() driver hook.
 */
static struct pipe_sampler_view *
iris_create_sampler_view(struct pipe_context *ctx,
                         struct pipe_resource *tex,
                         const struct pipe_sampler_view *tmpl)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   const struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_sampler_view *isv = calloc(1, sizeof(struct iris_sampler_view));

   if (!isv)
      return NULL;

   /* initialize base object */
   isv->base = *tmpl;
   isv->base.context = ctx;
   isv->base.texture = NULL;
   pipe_reference_init(&isv->base.reference, 1);
   pipe_resource_reference(&isv->base.texture, tex);

   void *map = upload_state(ice->state.surface_uploader, &isv->surface_state,
                            4 * GENX(RENDER_SURFACE_STATE_length), 64);
   if (!unlikely(map))
      return NULL;

   struct iris_bo *state_bo = iris_resource_bo(isv->surface_state.res);
   isv->surface_state.offset += iris_bo_offset_from_base_address(state_bo);

   if (util_format_is_depth_or_stencil(tmpl->format)) {
      struct iris_resource *zres, *sres;
      const struct util_format_description *desc =
         util_format_description(tmpl->format);

      iris_get_depth_stencil_resources(tex, &zres, &sres);

      tex = util_format_has_depth(desc) ? &zres->base : &sres->base;
   }

   isv->res = (struct iris_resource *) tex;

   isl_surf_usage_flags_t usage =
      ISL_SURF_USAGE_TEXTURE_BIT |
      (isv->res->surf.usage & ISL_SURF_USAGE_CUBE_BIT);

   const struct iris_format_info fmt =
      iris_format_for_usage(devinfo, tmpl->format, usage);

   isv->view = (struct isl_view) {
      .format = fmt.fmt,
      .swizzle = (struct isl_swizzle) {
         .r = fmt_swizzle(&fmt, tmpl->swizzle_r),
         .g = fmt_swizzle(&fmt, tmpl->swizzle_g),
         .b = fmt_swizzle(&fmt, tmpl->swizzle_b),
         .a = fmt_swizzle(&fmt, tmpl->swizzle_a),
      },
      .usage = usage,
   };

   /* Fill out SURFACE_STATE for this view. */
   if (tmpl->target != PIPE_BUFFER) {
      isv->view.base_level = tmpl->u.tex.first_level;
      isv->view.levels = tmpl->u.tex.last_level - tmpl->u.tex.first_level + 1;
      isv->view.base_array_layer = tmpl->u.tex.first_layer;
      isv->view.array_len =
         tmpl->u.tex.last_layer - tmpl->u.tex.first_layer + 1;

      isl_surf_fill_state(&screen->isl_dev, map,
                          .surf = &isv->res->surf, .view = &isv->view,
                          .mocs = MOCS_WB,
                          .address = isv->res->bo->gtt_offset);
                          // .aux_surf =
                          // .clear_color = clear_color,
   } else {
      fill_buffer_surface_state(&screen->isl_dev, isv->res->bo, map,
                                isv->view.format, tmpl->u.buf.offset,
                                tmpl->u.buf.size);
   }

   return &isv->base;
}

static void
iris_sampler_view_destroy(struct pipe_context *ctx,
                          struct pipe_sampler_view *state)
{
   struct iris_sampler_view *isv = (void *) state;
   pipe_resource_reference(&state->texture, NULL);
   pipe_resource_reference(&isv->surface_state.res, NULL);
   free(isv);
}

/**
 * The pipe->create_surface() driver hook.
 *
 * In Gallium nomenclature, "surfaces" are a view of a resource that
 * can be bound as a render target or depth/stencil buffer.
 */
static struct pipe_surface *
iris_create_surface(struct pipe_context *ctx,
                    struct pipe_resource *tex,
                    const struct pipe_surface *tmpl)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   const struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_surface *surf = calloc(1, sizeof(struct iris_surface));
   struct pipe_surface *psurf = &surf->base;
   struct iris_resource *res = (struct iris_resource *) tex;

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

   isl_surf_usage_flags_t usage = 0;
   if (tmpl->writable)
      usage = ISL_SURF_USAGE_STORAGE_BIT;
   else if (util_format_is_depth_or_stencil(tmpl->format))
      usage = ISL_SURF_USAGE_DEPTH_BIT;
   else
      usage = ISL_SURF_USAGE_RENDER_TARGET_BIT;

   const struct iris_format_info fmt =
      iris_format_for_usage(devinfo, psurf->format, usage);

   if ((usage & ISL_SURF_USAGE_RENDER_TARGET_BIT) &&
       !isl_format_supports_rendering(devinfo, fmt.fmt)) {
      /* Framebuffer validation will reject this invalid case, but it
       * hasn't had the opportunity yet.  In the meantime, we need to
       * avoid hitting ISL asserts about unsupported formats below.
       */
      free(surf);
      return NULL;
   }

   surf->view = (struct isl_view) {
      .format = fmt.fmt,
      .base_level = tmpl->u.tex.level,
      .levels = 1,
      .base_array_layer = tmpl->u.tex.first_layer,
      .array_len = tmpl->u.tex.last_layer - tmpl->u.tex.first_layer + 1,
      .swizzle = ISL_SWIZZLE_IDENTITY,
      .usage = usage,
   };

   /* Bail early for depth/stencil - we don't want SURFACE_STATE for them. */
   if (res->surf.usage & (ISL_SURF_USAGE_DEPTH_BIT |
                          ISL_SURF_USAGE_STENCIL_BIT))
      return psurf;


   void *map = upload_state(ice->state.surface_uploader, &surf->surface_state,
                            4 * GENX(RENDER_SURFACE_STATE_length), 64);
   if (!unlikely(map))
      return NULL;

   struct iris_bo *state_bo = iris_resource_bo(surf->surface_state.res);
   surf->surface_state.offset += iris_bo_offset_from_base_address(state_bo);

   isl_surf_fill_state(&screen->isl_dev, map,
                       .surf = &res->surf, .view = &surf->view,
                       .mocs = MOCS_WB,
                       .address = res->bo->gtt_offset);
                       // .aux_surf =
                       // .clear_color = clear_color,

   return psurf;
}

/**
 * The pipe->set_shader_images() driver hook.
 */
static void
iris_set_shader_images(struct pipe_context *ctx,
                       enum pipe_shader_type p_stage,
                       unsigned start_slot, unsigned count,
                       const struct pipe_image_view *p_images)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   const struct gen_device_info *devinfo = &screen->devinfo;
   gl_shader_stage stage = stage_from_pipe(p_stage);
   struct iris_shader_state *shs = &ice->state.shaders[stage];

   for (unsigned i = 0; i < count; i++) {
      if (p_images && p_images[i].resource) {
         const struct pipe_image_view *img = &p_images[i];
         struct iris_resource *res = (void *) img->resource;
         pipe_resource_reference(&shs->image[start_slot + i].res, &res->base);

         // XXX: these are not retained forever, use a separate uploader?
         void *map =
            upload_state(ice->state.surface_uploader,
                         &shs->image[start_slot + i].surface_state,
                         4 * GENX(RENDER_SURFACE_STATE_length), 64);
         if (!unlikely(map)) {
            pipe_resource_reference(&shs->image[start_slot + i].res, NULL);
            return;
         }

         struct iris_bo *surf_state_bo =
            iris_resource_bo(shs->image[start_slot + i].surface_state.res);
         shs->image[start_slot + i].surface_state.offset +=
            iris_bo_offset_from_base_address(surf_state_bo);

         isl_surf_usage_flags_t usage = ISL_SURF_USAGE_STORAGE_BIT;
         enum isl_format isl_format =
            iris_format_for_usage(devinfo, img->format, usage).fmt;

         if (img->shader_access & PIPE_IMAGE_ACCESS_READ)
            isl_format = isl_lower_storage_image_format(devinfo, isl_format);

         shs->image[start_slot + i].access = img->shader_access;

         if (res->base.target != PIPE_BUFFER) {
            struct isl_view view = {
               .format = isl_format,
               .base_level = img->u.tex.level,
               .levels = 1,
               .base_array_layer = img->u.tex.first_layer,
               .array_len = img->u.tex.last_layer - img->u.tex.first_layer + 1,
               .swizzle = ISL_SWIZZLE_IDENTITY,
               .usage = usage,
            };

            isl_surf_fill_state(&screen->isl_dev, map,
                                .surf = &res->surf, .view = &view,
                                .mocs = MOCS_WB,
                                .address = res->bo->gtt_offset);
                                // .aux_surf =
                                // .clear_color = clear_color,
         } else {
            fill_buffer_surface_state(&screen->isl_dev, res->bo, map,
                                      isl_format, img->u.buf.offset,
                                      img->u.buf.size);
         }
      } else {
         pipe_resource_reference(&shs->image[start_slot + i].res, NULL);
         pipe_resource_reference(&shs->image[start_slot + i].surface_state.res,
                                 NULL);
      }
   }

   ice->state.dirty |= IRIS_DIRTY_BINDINGS_VS << stage;
}


/**
 * The pipe->set_sampler_views() driver hook.
 */
static void
iris_set_sampler_views(struct pipe_context *ctx,
                       enum pipe_shader_type p_stage,
                       unsigned start, unsigned count,
                       struct pipe_sampler_view **views)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   gl_shader_stage stage = stage_from_pipe(p_stage);
   struct iris_shader_state *shs = &ice->state.shaders[stage];

   unsigned i;
   for (i = 0; i < count; i++) {
      pipe_sampler_view_reference((struct pipe_sampler_view **)
                                  &shs->textures[i], views[i]);
   }
   for (; i < shs->num_textures; i++) {
      pipe_sampler_view_reference((struct pipe_sampler_view **)
                                  &shs->textures[i], NULL);
   }

   shs->num_textures = count;

   ice->state.dirty |= (IRIS_DIRTY_BINDINGS_VS << stage);
}

/**
 * The pipe->set_tess_state() driver hook.
 */
static void
iris_set_tess_state(struct pipe_context *ctx,
                    const float default_outer_level[4],
                    const float default_inner_level[2])
{
   struct iris_context *ice = (struct iris_context *) ctx;

   memcpy(&ice->state.default_outer_level[0], &default_outer_level[0], 4 * sizeof(float));
   memcpy(&ice->state.default_inner_level[0], &default_inner_level[0], 2 * sizeof(float));

   ice->state.dirty |= IRIS_DIRTY_CONSTANTS_TCS;
}

static void
iris_surface_destroy(struct pipe_context *ctx, struct pipe_surface *p_surf)
{
   struct iris_surface *surf = (void *) p_surf;
   pipe_resource_reference(&p_surf->texture, NULL);
   pipe_resource_reference(&surf->surface_state.res, NULL);
   free(surf);
}

// XXX: actually implement user clip planes
static void
iris_set_clip_state(struct pipe_context *ctx,
                    const struct pipe_clip_state *state)
{
}

/**
 * The pipe->set_polygon_stipple() driver hook.
 */
static void
iris_set_polygon_stipple(struct pipe_context *ctx,
                         const struct pipe_poly_stipple *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   memcpy(&ice->state.poly_stipple, state, sizeof(*state));
   ice->state.dirty |= IRIS_DIRTY_POLYGON_STIPPLE;
}

/**
 * The pipe->set_sample_mask() driver hook.
 */
static void
iris_set_sample_mask(struct pipe_context *ctx, unsigned sample_mask)
{
   struct iris_context *ice = (struct iris_context *) ctx;

   /* We only support 16x MSAA, so we have 16 bits of sample maks.
    * st/mesa may pass us 0xffffffff though, meaning "enable all samples".
    */
   ice->state.sample_mask = sample_mask & 0xffff;
   ice->state.dirty |= IRIS_DIRTY_SAMPLE_MASK;
}

/**
 * The pipe->set_scissor_states() driver hook.
 *
 * This corresponds to our SCISSOR_RECT state structures.  It's an
 * exact match, so we just store them, and memcpy them out later.
 */
static void
iris_set_scissor_states(struct pipe_context *ctx,
                        unsigned start_slot,
                        unsigned num_scissors,
                        const struct pipe_scissor_state *rects)
{
   struct iris_context *ice = (struct iris_context *) ctx;

   for (unsigned i = 0; i < num_scissors; i++) {
      if (rects[i].minx == rects[i].maxx || rects[i].miny == rects[i].maxy) {
         /* If the scissor was out of bounds and got clamped to 0 width/height
          * at the bounds, the subtraction of 1 from maximums could produce a
          * negative number and thus not clip anything.  Instead, just provide
          * a min > max scissor inside the bounds, which produces the expected
          * no rendering.
          */
         ice->state.scissors[start_slot + i] = (struct pipe_scissor_state) {
            .minx = 1, .maxx = 0, .miny = 1, .maxy = 0,
         };
      } else {
         ice->state.scissors[start_slot + i] = (struct pipe_scissor_state) {
            .minx = rects[i].minx,     .miny = rects[i].miny,
            .maxx = rects[i].maxx - 1, .maxy = rects[i].maxy - 1,
         };
      }
   }

   ice->state.dirty |= IRIS_DIRTY_SCISSOR_RECT;
}

/**
 * The pipe->set_stencil_ref() driver hook.
 *
 * This is added to 3DSTATE_WM_DEPTH_STENCIL dynamically at draw time.
 */
static void
iris_set_stencil_ref(struct pipe_context *ctx,
                     const struct pipe_stencil_ref *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   memcpy(&ice->state.stencil_ref, state, sizeof(*state));
   ice->state.dirty |= IRIS_DIRTY_WM_DEPTH_STENCIL;
}

static float
viewport_extent(const struct pipe_viewport_state *state, int axis, float sign)
{
   return copysignf(state->scale[axis], sign) + state->translate[axis];
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

/**
 * The pipe->set_viewport_states() driver hook.
 *
 * This corresponds to our SF_CLIP_VIEWPORT states.  We can't calculate
 * the guardband yet, as we need the framebuffer dimensions, but we can
 * at least fill out the rest.
 */
static void
iris_set_viewport_states(struct pipe_context *ctx,
                         unsigned start_slot,
                         unsigned count,
                         const struct pipe_viewport_state *states)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_genx_state *genx = ice->state.genx;
   uint32_t *vp_map =
      &genx->sf_cl_vp[start_slot * GENX(SF_CLIP_VIEWPORT_length)];

   for (unsigned i = 0; i < count; i++) {
      const struct pipe_viewport_state *state = &states[i];

      memcpy(&ice->state.viewports[start_slot + i], state, sizeof(*state));

      iris_pack_state(GENX(SF_CLIP_VIEWPORT), vp_map, vp) {
         vp.ViewportMatrixElementm00 = state->scale[0];
         vp.ViewportMatrixElementm11 = state->scale[1];
         vp.ViewportMatrixElementm22 = state->scale[2];
         vp.ViewportMatrixElementm30 = state->translate[0];
         vp.ViewportMatrixElementm31 = state->translate[1];
         vp.ViewportMatrixElementm32 = state->translate[2];
         /* XXX: in i965 this is computed based on the drawbuffer size,
          * but we don't have that here...
          */
         vp.XMinClipGuardband = -1.0;
         vp.XMaxClipGuardband = 1.0;
         vp.YMinClipGuardband = -1.0;
         vp.YMaxClipGuardband = 1.0;
         vp.XMinViewPort = viewport_extent(state, 0, -1.0f);
         vp.XMaxViewPort = viewport_extent(state, 0,  1.0f) - 1;
         vp.YMinViewPort = viewport_extent(state, 1, -1.0f);
         vp.YMaxViewPort = viewport_extent(state, 1,  1.0f) - 1;
      }

      vp_map += GENX(SF_CLIP_VIEWPORT_length);
   }

   ice->state.dirty |= IRIS_DIRTY_SF_CL_VIEWPORT;

   if (ice->state.cso_rast && (!ice->state.cso_rast->depth_clip_near ||
                               !ice->state.cso_rast->depth_clip_far))
      ice->state.dirty |= IRIS_DIRTY_CC_VIEWPORT;
}

/**
 * The pipe->set_framebuffer_state() driver hook.
 *
 * Sets the current draw FBO, including color render targets, depth,
 * and stencil buffers.
 */
static void
iris_set_framebuffer_state(struct pipe_context *ctx,
                           const struct pipe_framebuffer_state *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   struct isl_device *isl_dev = &screen->isl_dev;
   struct pipe_framebuffer_state *cso = &ice->state.framebuffer;
   struct iris_resource *zres;
   struct iris_resource *stencil_res;

   unsigned samples = util_framebuffer_get_num_samples(state);

   if (cso->samples != samples) {
      ice->state.dirty |= IRIS_DIRTY_MULTISAMPLE;
   }

   if (cso->nr_cbufs != state->nr_cbufs) {
      ice->state.dirty |= IRIS_DIRTY_BLEND_STATE;
   }

   if ((cso->layers == 0) != (state->layers == 0)) {
      ice->state.dirty |= IRIS_DIRTY_CLIP;
   }

   util_copy_framebuffer_state(cso, state);
   cso->samples = samples;

   struct iris_depth_buffer_state *cso_z = &ice->state.genx->depth_buffer;

   struct isl_view view = {
      .base_level = 0,
      .levels = 1,
      .base_array_layer = 0,
      .array_len = 1,
      .swizzle = ISL_SWIZZLE_IDENTITY,
   };

   struct isl_depth_stencil_hiz_emit_info info = {
      .view = &view,
      .mocs = MOCS_WB,
   };

   if (cso->zsbuf) {
      iris_get_depth_stencil_resources(cso->zsbuf->texture, &zres,
                                       &stencil_res);

      view.base_level = cso->zsbuf->u.tex.level;
      view.base_array_layer = cso->zsbuf->u.tex.first_layer;
      view.array_len =
         cso->zsbuf->u.tex.last_layer - cso->zsbuf->u.tex.first_layer + 1;

      if (zres) {
         view.usage |= ISL_SURF_USAGE_DEPTH_BIT;

         info.depth_surf = &zres->surf;
         info.depth_address = zres->bo->gtt_offset;
         info.hiz_usage = ISL_AUX_USAGE_NONE;

         view.format = zres->surf.format;
      }

      if (stencil_res) {
         view.usage |= ISL_SURF_USAGE_STENCIL_BIT;
         info.stencil_surf = &stencil_res->surf;
         info.stencil_address = stencil_res->bo->gtt_offset;
         if (!zres)
            view.format = stencil_res->surf.format;
      }
   }

   isl_emit_depth_stencil_hiz_s(isl_dev, cso_z->packets, &info);

   /* Make a null surface for unbound buffers */
   void *null_surf_map =
      upload_state(ice->state.surface_uploader, &ice->state.null_fb,
                   4 * GENX(RENDER_SURFACE_STATE_length), 64);
   isl_null_fill_state(&screen->isl_dev, null_surf_map,
                       isl_extent3d(MAX2(cso->width, 1),
                                    MAX2(cso->height, 1),
                                    cso->layers ? cso->layers : 1));
   ice->state.null_fb.offset +=
      iris_bo_offset_from_base_address(iris_resource_bo(ice->state.null_fb.res));

   ice->state.dirty |= IRIS_DIRTY_DEPTH_BUFFER;

   /* Render target change */
   ice->state.dirty |= IRIS_DIRTY_BINDINGS_FS;

   ice->state.dirty |= ice->state.dirty_for_nos[IRIS_NOS_FRAMEBUFFER];

#if GEN_GEN == 11
   // XXX: we may want to flag IRIS_DIRTY_MULTISAMPLE (or SAMPLE_MASK?)
   // XXX: see commit 979fc1bc9bcc64027ff2cfafd285676f31b930a6

   /* The PIPE_CONTROL command description says:
    *
    *   "Whenever a Binding Table Index (BTI) used by a Render Target Message
    *    points to a different RENDER_SURFACE_STATE, SW must issue a Render
    *    Target Cache Flush by enabling this bit. When render target flush
    *    is set due to new association of BTI, PS Scoreboard Stall bit must
    *    be set in this packet."
    */
   // XXX: does this need to happen at 3DSTATE_BTP_PS time?
   iris_emit_pipe_control_flush(&ice->render_batch,
                                PIPE_CONTROL_RENDER_TARGET_FLUSH |
                                PIPE_CONTROL_STALL_AT_SCOREBOARD);
#endif
}

/**
 * The pipe->set_constant_buffer() driver hook.
 *
 * This uploads any constant data in user buffers, and references
 * any UBO resources containing constant data.
 */
static void
iris_set_constant_buffer(struct pipe_context *ctx,
                         enum pipe_shader_type p_stage, unsigned index,
                         const struct pipe_constant_buffer *input)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   gl_shader_stage stage = stage_from_pipe(p_stage);
   struct iris_shader_state *shs = &ice->state.shaders[stage];
   struct iris_const_buffer *cbuf = &shs->constbuf[index];

   if (input && (input->buffer || input->user_buffer)) {
      if (input->user_buffer) {
         u_upload_data(ctx->const_uploader, 0, input->buffer_size, 32,
                       input->user_buffer, &cbuf->data.offset,
                       &cbuf->data.res);
      } else {
         pipe_resource_reference(&cbuf->data.res, input->buffer);
         cbuf->data.offset = input->buffer_offset;
      }

      // XXX: these are not retained forever, use a separate uploader?
      void *map =
         upload_state(ice->state.surface_uploader, &cbuf->surface_state,
                      4 * GENX(RENDER_SURFACE_STATE_length), 64);
      if (!unlikely(map)) {
         pipe_resource_reference(&cbuf->data.res, NULL);
         return;
      }

      struct iris_resource *res = (void *) cbuf->data.res;
      struct iris_bo *surf_bo = iris_resource_bo(cbuf->surface_state.res);
      cbuf->surface_state.offset += iris_bo_offset_from_base_address(surf_bo);

      isl_buffer_fill_state(&screen->isl_dev, map,
                            .address = res->bo->gtt_offset + cbuf->data.offset,
                            .size_B = MIN2(input->buffer_size,
                                           res->bo->size - cbuf->data.offset),
                            .format = ISL_FORMAT_R32G32B32A32_FLOAT,
                            .stride_B = 1,
                            .mocs = MOCS_WB)
   } else {
      pipe_resource_reference(&cbuf->data.res, NULL);
      pipe_resource_reference(&cbuf->surface_state.res, NULL);
   }

   ice->state.dirty |= IRIS_DIRTY_CONSTANTS_VS << stage;
   // XXX: maybe not necessary all the time...?
   // XXX: we need 3DS_BTP to commit these changes, and if we fell back to
   // XXX: pull model we may need actual new bindings...
   ice->state.dirty |= IRIS_DIRTY_BINDINGS_VS << stage;
}

/**
 * The pipe->set_shader_buffers() driver hook.
 *
 * This binds SSBOs and ABOs.  Unfortunately, we need to stream out
 * SURFACE_STATE here, as the buffer offset may change each time.
 */
static void
iris_set_shader_buffers(struct pipe_context *ctx,
                        enum pipe_shader_type p_stage,
                        unsigned start_slot, unsigned count,
                        const struct pipe_shader_buffer *buffers)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   gl_shader_stage stage = stage_from_pipe(p_stage);
   struct iris_shader_state *shs = &ice->state.shaders[stage];

   for (unsigned i = 0; i < count; i++) {
      if (buffers && buffers[i].buffer) {
         const struct pipe_shader_buffer *buffer = &buffers[i];
         struct iris_resource *res = (void *) buffer->buffer;
         pipe_resource_reference(&shs->ssbo[start_slot + i], &res->base);

         // XXX: these are not retained forever, use a separate uploader?
         void *map =
            upload_state(ice->state.surface_uploader,
                         &shs->ssbo_surface_state[start_slot + i],
                         4 * GENX(RENDER_SURFACE_STATE_length), 64);
         if (!unlikely(map)) {
            pipe_resource_reference(&shs->ssbo[start_slot + i], NULL);
            return;
         }

         struct iris_bo *surf_state_bo =
            iris_resource_bo(shs->ssbo_surface_state[start_slot + i].res);
         shs->ssbo_surface_state[start_slot + i].offset +=
            iris_bo_offset_from_base_address(surf_state_bo);

         isl_buffer_fill_state(&screen->isl_dev, map,
                               .address =
                                  res->bo->gtt_offset + buffer->buffer_offset,
                               .size_B =
                                  MIN2(buffer->buffer_size,
                                       res->bo->size - buffer->buffer_offset),
                               .format = ISL_FORMAT_RAW,
                               .stride_B = 1,
                               .mocs = MOCS_WB);
      } else {
         pipe_resource_reference(&shs->ssbo[start_slot + i], NULL);
         pipe_resource_reference(&shs->ssbo_surface_state[start_slot + i].res,
                                 NULL);
      }
   }

   ice->state.dirty |= IRIS_DIRTY_BINDINGS_VS << stage;
}

static void
iris_delete_state(struct pipe_context *ctx, void *state)
{
   free(state);
}

static void
iris_free_vertex_buffers(struct iris_vertex_buffer_state *cso)
{
   for (unsigned i = 0; i < cso->num_buffers; i++)
      pipe_resource_reference(&cso->resources[i], NULL);
}

/**
 * The pipe->set_vertex_buffers() driver hook.
 *
 * This translates pipe_vertex_buffer to our 3DSTATE_VERTEX_BUFFERS packet.
 */
static void
iris_set_vertex_buffers(struct pipe_context *ctx,
                        unsigned start_slot, unsigned count,
                        const struct pipe_vertex_buffer *buffers)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_vertex_buffer_state *cso = &ice->state.genx->vertex_buffers;

   iris_free_vertex_buffers(&ice->state.genx->vertex_buffers);

   if (!buffers)
      count = 0;

   cso->num_buffers = count;

   iris_pack_command(GENX(3DSTATE_VERTEX_BUFFERS), cso->vertex_buffers, vb) {
      vb.DWordLength = 4 * MAX2(cso->num_buffers, 1) - 1;
   }

   uint32_t *vb_pack_dest = &cso->vertex_buffers[1];

   if (count == 0) {
      iris_pack_state(GENX(VERTEX_BUFFER_STATE), vb_pack_dest, vb) {
         vb.VertexBufferIndex = start_slot;
         vb.NullVertexBuffer = true;
         vb.AddressModifyEnable = true;
      }
   }

   for (unsigned i = 0; i < count; i++) {
      assert(!buffers[i].is_user_buffer);

      pipe_resource_reference(&cso->resources[i], buffers[i].buffer.resource);
      struct iris_resource *res = (void *) cso->resources[i];

      iris_pack_state(GENX(VERTEX_BUFFER_STATE), vb_pack_dest, vb) {
         vb.VertexBufferIndex = start_slot + i;
         vb.MOCS = MOCS_WB;
         vb.AddressModifyEnable = true;
         vb.BufferPitch = buffers[i].stride;
         if (res) {
            vb.BufferSize = res->bo->size;
            vb.BufferStartingAddress =
               ro_bo(NULL, res->bo->gtt_offset + buffers[i].buffer_offset);
         } else {
            vb.NullVertexBuffer = true;
         }
      }

      vb_pack_dest += GENX(VERTEX_BUFFER_STATE_length);
   }

   ice->state.dirty |= IRIS_DIRTY_VERTEX_BUFFERS;
}

/**
 * Gallium CSO for vertex elements.
 */
struct iris_vertex_element_state {
   uint32_t vertex_elements[1 + 33 * GENX(VERTEX_ELEMENT_STATE_length)];
   uint32_t vf_instancing[33 * GENX(3DSTATE_VF_INSTANCING_length)];
   unsigned count;
};

/**
 * The pipe->create_vertex_elements() driver hook.
 *
 * This translates pipe_vertex_element to our 3DSTATE_VERTEX_ELEMENTS
 * and 3DSTATE_VF_INSTANCING commands.  SGVs are handled at draw time.
 */
static void *
iris_create_vertex_elements(struct pipe_context *ctx,
                            unsigned count,
                            const struct pipe_vertex_element *state)
{
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;
   const struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_vertex_element_state *cso =
      malloc(sizeof(struct iris_vertex_element_state));

   cso->count = count;

   /* TODO:
    *  - create edge flag one
    *  - create SGV ones
    *  - if those are necessary, use count + 1/2/3... OR in the length
    */
   iris_pack_command(GENX(3DSTATE_VERTEX_ELEMENTS), cso->vertex_elements, ve) {
      ve.DWordLength =
         1 + GENX(VERTEX_ELEMENT_STATE_length) * MAX2(count, 1) - 2;
   }

   uint32_t *ve_pack_dest = &cso->vertex_elements[1];
   uint32_t *vfi_pack_dest = cso->vf_instancing;

   if (count == 0) {
      iris_pack_state(GENX(VERTEX_ELEMENT_STATE), ve_pack_dest, ve) {
         ve.Valid = true;
         ve.SourceElementFormat = ISL_FORMAT_R32G32B32A32_FLOAT;
         ve.Component0Control = VFCOMP_STORE_0;
         ve.Component1Control = VFCOMP_STORE_0;
         ve.Component2Control = VFCOMP_STORE_0;
         ve.Component3Control = VFCOMP_STORE_1_FP;
      }

      iris_pack_command(GENX(3DSTATE_VF_INSTANCING), vfi_pack_dest, vi) {
      }
   }

   for (int i = 0; i < count; i++) {
      const struct iris_format_info fmt =
         iris_format_for_usage(devinfo, state[i].src_format, 0);
      unsigned comp[4] = { VFCOMP_STORE_SRC, VFCOMP_STORE_SRC,
                           VFCOMP_STORE_SRC, VFCOMP_STORE_SRC };

      switch (isl_format_get_num_channels(fmt.fmt)) {
      case 0: comp[0] = VFCOMP_STORE_0;
      case 1: comp[1] = VFCOMP_STORE_0;
      case 2: comp[2] = VFCOMP_STORE_0;
      case 3:
         comp[3] = isl_format_has_int_channel(fmt.fmt) ? VFCOMP_STORE_1_INT
                                                       : VFCOMP_STORE_1_FP;
         break;
      }
      iris_pack_state(GENX(VERTEX_ELEMENT_STATE), ve_pack_dest, ve) {
         ve.VertexBufferIndex = state[i].vertex_buffer_index;
         ve.Valid = true;
         ve.SourceElementOffset = state[i].src_offset;
         ve.SourceElementFormat = fmt.fmt;
         ve.Component0Control = comp[0];
         ve.Component1Control = comp[1];
         ve.Component2Control = comp[2];
         ve.Component3Control = comp[3];
      }

      iris_pack_command(GENX(3DSTATE_VF_INSTANCING), vfi_pack_dest, vi) {
         vi.VertexElementIndex = i;
         vi.InstancingEnable = state[i].instance_divisor > 0;
         vi.InstanceDataStepRate = state[i].instance_divisor;
      }

      ve_pack_dest += GENX(VERTEX_ELEMENT_STATE_length);
      vfi_pack_dest += GENX(3DSTATE_VF_INSTANCING_length);
   }

   return cso;
}

/**
 * The pipe->bind_vertex_elements_state() driver hook.
 */
static void
iris_bind_vertex_elements_state(struct pipe_context *ctx, void *state)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_vertex_element_state *old_cso = ice->state.cso_vertex_elements;
   struct iris_vertex_element_state *new_cso = state;

   /* 3DSTATE_VF_SGVs overrides the last VE, so if the count is changing,
    * we need to re-emit it to ensure we're overriding the right one.
    */
   if (new_cso && cso_changed(count))
      ice->state.dirty |= IRIS_DIRTY_VF_SGVS;

   ice->state.cso_vertex_elements = state;
   ice->state.dirty |= IRIS_DIRTY_VERTEX_ELEMENTS;
}

/**
 * Gallium CSO for stream output (transform feedback) targets.
 */
struct iris_stream_output_target {
   struct pipe_stream_output_target base;

   uint32_t so_buffer[GENX(3DSTATE_SO_BUFFER_length)];

   /** Storage holding the offset where we're writing in the buffer */
   struct iris_state_ref offset;
};

/**
 * The pipe->create_stream_output_target() driver hook.
 *
 * "Target" here refers to a destination buffer.  We translate this into
 * a 3DSTATE_SO_BUFFER packet.  We can handle most fields, but don't yet
 * know which buffer this represents, or whether we ought to zero the
 * write-offsets, or append.  Those are handled in the set() hook.
 */
static struct pipe_stream_output_target *
iris_create_stream_output_target(struct pipe_context *ctx,
                                 struct pipe_resource *res,
                                 unsigned buffer_offset,
                                 unsigned buffer_size)
{
   struct iris_stream_output_target *cso = calloc(1, sizeof(*cso));
   if (!cso)
      return NULL;

   pipe_reference_init(&cso->base.reference, 1);
   pipe_resource_reference(&cso->base.buffer, res);
   cso->base.buffer_offset = buffer_offset;
   cso->base.buffer_size = buffer_size;
   cso->base.context = ctx;

   upload_state(ctx->stream_uploader, &cso->offset, 4 * sizeof(uint32_t), 4);

   iris_pack_command(GENX(3DSTATE_SO_BUFFER), cso->so_buffer, sob) {
      sob.SurfaceBaseAddress =
         rw_bo(NULL, iris_resource_bo(res)->gtt_offset + buffer_offset);
      sob.SOBufferEnable = true;
      sob.StreamOffsetWriteEnable = true;
      sob.StreamOutputBufferOffsetAddressEnable = true;
      sob.MOCS = MOCS_WB; // XXX: MOCS

      sob.SurfaceSize = MAX2(buffer_size / 4, 1) - 1;

      /* .SOBufferIndex, .StreamOffset, and .StreamOutputBufferOffsetAddress
       * are filled in later when we have stream IDs.
       */
   }

   return &cso->base;
}

static void
iris_stream_output_target_destroy(struct pipe_context *ctx,
                                  struct pipe_stream_output_target *state)
{
   struct iris_stream_output_target *cso = (void *) state;

   pipe_resource_reference(&cso->base.buffer, NULL);
   pipe_resource_reference(&cso->offset.res, NULL);

   free(cso);
}

/**
 * The pipe->set_stream_output_targets() driver hook.
 *
 * At this point, we know which targets are bound to a particular index,
 * and also whether we want to append or start over.  We can finish the
 * 3DSTATE_SO_BUFFER packets we started earlier.
 */
static void
iris_set_stream_output_targets(struct pipe_context *ctx,
                               unsigned num_targets,
                               struct pipe_stream_output_target **targets,
                               const unsigned *offsets)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_genx_state *genx = ice->state.genx;
   uint32_t *so_buffers = genx->so_buffers;

   const bool active = num_targets > 0;
   if (ice->state.streamout_active != active) {
      ice->state.streamout_active = active;
      ice->state.dirty |= IRIS_DIRTY_STREAMOUT;

      /* We only emit 3DSTATE_SO_DECL_LIST when streamout is active, because
       * it's a non-pipelined command.  If we're switching streamout on, we
       * may have missed emitting it earlier, so do so now.  (We're already
       * taking a stall to update 3DSTATE_SO_BUFFERS anyway...)
       */
      if (active)
         ice->state.dirty |= IRIS_DIRTY_SO_DECL_LIST;
   }

   for (int i = 0; i < 4; i++) {
      pipe_so_target_reference(&ice->state.so_target[i],
                               i < num_targets ? targets[i] : NULL);
   }

   /* No need to update 3DSTATE_SO_BUFFER unless SOL is active. */
   if (!active)
      return;

   for (unsigned i = 0; i < 4; i++,
        so_buffers += GENX(3DSTATE_SO_BUFFER_length)) {

      if (i >= num_targets || !targets[i]) {
         iris_pack_command(GENX(3DSTATE_SO_BUFFER), so_buffers, sob)
            sob.SOBufferIndex = i;
         continue;
      }

      struct iris_stream_output_target *tgt = (void *) targets[i];

      /* Note that offsets[i] will either be 0, causing us to zero
       * the value in the buffer, or 0xFFFFFFFF, which happens to mean
       * "continue appending at the existing offset."
       */
      assert(offsets[i] == 0 || offsets[i] == 0xFFFFFFFF);

      uint32_t dynamic[GENX(3DSTATE_SO_BUFFER_length)];
      iris_pack_state(GENX(3DSTATE_SO_BUFFER), dynamic, dyns) {
         dyns.SOBufferIndex = i;
         dyns.StreamOffset = offsets[i];
         dyns.StreamOutputBufferOffsetAddress =
            rw_bo(NULL, iris_resource_bo(tgt->offset.res)->gtt_offset + tgt->offset.offset + i * sizeof(uint32_t));
      }

      for (uint32_t j = 0; j < GENX(3DSTATE_SO_BUFFER_length); j++) {
         so_buffers[j] = tgt->so_buffer[j] | dynamic[j];
      }
   }

   ice->state.dirty |= IRIS_DIRTY_SO_BUFFERS;
}

/**
 * An iris-vtable helper for encoding the 3DSTATE_SO_DECL_LIST and
 * 3DSTATE_STREAMOUT packets.
 *
 * 3DSTATE_SO_DECL_LIST is a list of shader outputs we want the streamout
 * hardware to record.  We can create it entirely based on the shader, with
 * no dynamic state dependencies.
 *
 * 3DSTATE_STREAMOUT is an annoying mix of shader-based information and
 * state-based settings.  We capture the shader-related ones here, and merge
 * the rest in at draw time.
 */
static uint32_t *
iris_create_so_decl_list(const struct pipe_stream_output_info *info,
                         const struct brw_vue_map *vue_map)
{
   struct GENX(SO_DECL) so_decl[MAX_VERTEX_STREAMS][128];
   int buffer_mask[MAX_VERTEX_STREAMS] = {0, 0, 0, 0};
   int next_offset[MAX_VERTEX_STREAMS] = {0, 0, 0, 0};
   int decls[MAX_VERTEX_STREAMS] = {0, 0, 0, 0};
   int max_decls = 0;
   STATIC_ASSERT(ARRAY_SIZE(so_decl[0]) >= MAX_PROGRAM_OUTPUTS);

   memset(so_decl, 0, sizeof(so_decl));

   /* Construct the list of SO_DECLs to be emitted.  The formatting of the
    * command feels strange -- each dword pair contains a SO_DECL per stream.
    */
   for (unsigned i = 0; i < info->num_outputs; i++) {
      const struct pipe_stream_output *output = &info->output[i];
      const int buffer = output->output_buffer;
      const int varying = output->register_index;
      const unsigned stream_id = output->stream;
      assert(stream_id < MAX_VERTEX_STREAMS);

      buffer_mask[stream_id] |= 1 << buffer;

      assert(vue_map->varying_to_slot[varying] >= 0);

      /* Mesa doesn't store entries for gl_SkipComponents in the Outputs[]
       * array.  Instead, it simply increments DstOffset for the following
       * input by the number of components that should be skipped.
       *
       * Our hardware is unusual in that it requires us to program SO_DECLs
       * for fake "hole" components, rather than simply taking the offset
       * for each real varying.  Each hole can have size 1, 2, 3, or 4; we
       * program as many size = 4 holes as we can, then a final hole to
       * accommodate the final 1, 2, or 3 remaining.
       */
      int skip_components = output->dst_offset - next_offset[buffer];

      while (skip_components > 0) {
         so_decl[stream_id][decls[stream_id]++] = (struct GENX(SO_DECL)) {
            .HoleFlag = 1,
            .OutputBufferSlot = output->output_buffer,
            .ComponentMask = (1 << MIN2(skip_components, 4)) - 1,
         };
         skip_components -= 4;
      }

      next_offset[buffer] = output->dst_offset + output->num_components;

      so_decl[stream_id][decls[stream_id]++] = (struct GENX(SO_DECL)) {
         .OutputBufferSlot = output->output_buffer,
         .RegisterIndex = vue_map->varying_to_slot[varying],
         .ComponentMask =
            ((1 << output->num_components) - 1) << output->start_component,
      };

      if (decls[stream_id] > max_decls)
         max_decls = decls[stream_id];
   }

   unsigned dwords = GENX(3DSTATE_STREAMOUT_length) + (3 + 2 * max_decls);
   uint32_t *map = ralloc_size(NULL, sizeof(uint32_t) * dwords);
   uint32_t *so_decl_map = map + GENX(3DSTATE_STREAMOUT_length);

   iris_pack_command(GENX(3DSTATE_STREAMOUT), map, sol) {
      int urb_entry_read_offset = 0;
      int urb_entry_read_length = (vue_map->num_slots + 1) / 2 -
         urb_entry_read_offset;

      /* We always read the whole vertex.  This could be reduced at some
       * point by reading less and offsetting the register index in the
       * SO_DECLs.
       */
      sol.Stream0VertexReadOffset = urb_entry_read_offset;
      sol.Stream0VertexReadLength = urb_entry_read_length - 1;
      sol.Stream1VertexReadOffset = urb_entry_read_offset;
      sol.Stream1VertexReadLength = urb_entry_read_length - 1;
      sol.Stream2VertexReadOffset = urb_entry_read_offset;
      sol.Stream2VertexReadLength = urb_entry_read_length - 1;
      sol.Stream3VertexReadOffset = urb_entry_read_offset;
      sol.Stream3VertexReadLength = urb_entry_read_length - 1;

      /* Set buffer pitches; 0 means unbound. */
      sol.Buffer0SurfacePitch = 4 * info->stride[0];
      sol.Buffer1SurfacePitch = 4 * info->stride[1];
      sol.Buffer2SurfacePitch = 4 * info->stride[2];
      sol.Buffer3SurfacePitch = 4 * info->stride[3];
   }

   iris_pack_command(GENX(3DSTATE_SO_DECL_LIST), so_decl_map, list) {
      list.DWordLength = 3 + 2 * max_decls - 2;
      list.StreamtoBufferSelects0 = buffer_mask[0];
      list.StreamtoBufferSelects1 = buffer_mask[1];
      list.StreamtoBufferSelects2 = buffer_mask[2];
      list.StreamtoBufferSelects3 = buffer_mask[3];
      list.NumEntries0 = decls[0];
      list.NumEntries1 = decls[1];
      list.NumEntries2 = decls[2];
      list.NumEntries3 = decls[3];
   }

   for (int i = 0; i < max_decls; i++) {
      iris_pack_state(GENX(SO_DECL_ENTRY), so_decl_map + 3 + i * 2, entry) {
         entry.Stream0Decl = so_decl[0][i];
         entry.Stream1Decl = so_decl[1][i];
         entry.Stream2Decl = so_decl[2][i];
         entry.Stream3Decl = so_decl[3][i];
      }
   }

   return map;
}

static void
iris_compute_sbe_urb_read_interval(uint64_t fs_input_slots,
                                   const struct brw_vue_map *last_vue_map,
                                   bool two_sided_color,
                                   unsigned *out_offset,
                                   unsigned *out_length)
{
   /* The compiler computes the first URB slot without considering COL/BFC
    * swizzling (because it doesn't know whether it's enabled), so we need
    * to do that here too.  This may result in a smaller offset, which
    * should be safe.
    */
   const unsigned first_slot =
      brw_compute_first_urb_slot_required(fs_input_slots, last_vue_map);

   /* This becomes the URB read offset (counted in pairs of slots). */
   assert(first_slot % 2 == 0);
   *out_offset = first_slot / 2;

   /* We need to adjust the inputs read to account for front/back color
    * swizzling, as it can make the URB length longer.
    */
   for (int c = 0; c <= 1; c++) {
      if (fs_input_slots & (VARYING_BIT_COL0 << c)) {
         /* If two sided color is enabled, the fragment shader's gl_Color
          * (COL0) input comes from either the gl_FrontColor (COL0) or
          * gl_BackColor (BFC0) input varyings.  Mark BFC as used, too.
          */
         if (two_sided_color)
            fs_input_slots |= (VARYING_BIT_BFC0 << c);

         /* If front color isn't written, we opt to give them back color
          * instead of an undefined value.  Switch from COL to BFC.
          */
         if (last_vue_map->varying_to_slot[VARYING_SLOT_COL0 + c] == -1) {
            fs_input_slots &= ~(VARYING_BIT_COL0 << c);
            fs_input_slots |= (VARYING_BIT_BFC0 << c);
         }
      }
   }

   /* Compute the minimum URB Read Length necessary for the FS inputs.
    *
    * From the Sandy Bridge PRM, Volume 2, Part 1, documentation for
    * 3DSTATE_SF DWord 1 bits 15:11, "Vertex URB Entry Read Length":
    *
    * "This field should be set to the minimum length required to read the
    *  maximum source attribute.  The maximum source attribute is indicated
    *  by the maximum value of the enabled Attribute # Source Attribute if
    *  Attribute Swizzle Enable is set, Number of Output Attributes-1 if
    *  enable is not set.
    *  read_length = ceiling((max_source_attr + 1) / 2)
    *
    *  [errata] Corruption/Hang possible if length programmed larger than
    *  recommended"
    *
    * Similar text exists for Ivy Bridge.
    *
    * We find the last URB slot that's actually read by the FS.
    */
   unsigned last_read_slot = last_vue_map->num_slots - 1;
   while (last_read_slot > first_slot && !(fs_input_slots &
          (1ull << last_vue_map->slot_to_varying[last_read_slot])))
      --last_read_slot;

   /* The URB read length is the difference of the two, counted in pairs. */
   *out_length = DIV_ROUND_UP(last_read_slot - first_slot + 1, 2);
}

static void
iris_emit_sbe_swiz(struct iris_batch *batch,
                   const struct iris_context *ice,
                   unsigned urb_read_offset,
                   unsigned sprite_coord_enables)
{
   struct GENX(SF_OUTPUT_ATTRIBUTE_DETAIL) attr_overrides[16] = {};
   const struct brw_wm_prog_data *wm_prog_data = (void *)
      ice->shaders.prog[MESA_SHADER_FRAGMENT]->prog_data;
   const struct brw_vue_map *vue_map = ice->shaders.last_vue_map;
   const struct iris_rasterizer_state *cso_rast = ice->state.cso_rast;

   /* XXX: this should be generated when putting programs in place */

   // XXX: raster->sprite_coord_enable

   for (int fs_attr = 0; fs_attr < VARYING_SLOT_MAX; fs_attr++) {
      const int input_index = wm_prog_data->urb_setup[fs_attr];
      if (input_index < 0 || input_index >= 16)
         continue;

      struct GENX(SF_OUTPUT_ATTRIBUTE_DETAIL) *attr =
         &attr_overrides[input_index];
      int slot = vue_map->varying_to_slot[fs_attr];

      /* Viewport and Layer are stored in the VUE header.  We need to override
       * them to zero if earlier stages didn't write them, as GL requires that
       * they read back as zero when not explicitly set.
       */
      switch (fs_attr) {
      case VARYING_SLOT_VIEWPORT:
      case VARYING_SLOT_LAYER:
         attr->ComponentOverrideX = true;
         attr->ComponentOverrideW = true;
         attr->ConstantSource = CONST_0000;

         if (!(vue_map->slots_valid & VARYING_BIT_LAYER))
            attr->ComponentOverrideY = true;
         if (!(vue_map->slots_valid & VARYING_BIT_VIEWPORT))
            attr->ComponentOverrideZ = true;
         continue;

      case VARYING_SLOT_PRIMITIVE_ID:
         /* Override if the previous shader stage didn't write gl_PrimitiveID. */
         if (slot == -1) {
            attr->ComponentOverrideX = true;
            attr->ComponentOverrideY = true;
            attr->ComponentOverrideZ = true;
            attr->ComponentOverrideW = true;
            attr->ConstantSource = PRIM_ID;
            continue;
         }

      default:
         break;
      }

      if (sprite_coord_enables & (1 << input_index))
         continue;

      /* If there was only a back color written but not front, use back
       * as the color instead of undefined.
       */
      if (slot == -1 && fs_attr == VARYING_SLOT_COL0)
         slot = vue_map->varying_to_slot[VARYING_SLOT_BFC0];
      if (slot == -1 && fs_attr == VARYING_SLOT_COL1)
         slot = vue_map->varying_to_slot[VARYING_SLOT_BFC1];

      /* Not written by the previous stage - undefined. */
      if (slot == -1) {
         attr->ComponentOverrideX = true;
         attr->ComponentOverrideY = true;
         attr->ComponentOverrideZ = true;
         attr->ComponentOverrideW = true;
         attr->ConstantSource = CONST_0001_FLOAT;
         continue;
      }

      /* Compute the location of the attribute relative to the read offset,
       * which is counted in 256-bit increments (two 128-bit VUE slots).
       */
      const int source_attr = slot - 2 * urb_read_offset;
      assert(source_attr >= 0 && source_attr <= 32);
      attr->SourceAttribute = source_attr;

      /* If we are doing two-sided color, and the VUE slot following this one
       * represents a back-facing color, then we need to instruct the SF unit
       * to do back-facing swizzling.
       */
      if (cso_rast->light_twoside &&
          ((vue_map->slot_to_varying[slot] == VARYING_SLOT_COL0 &&
            vue_map->slot_to_varying[slot+1] == VARYING_SLOT_BFC0) ||
           (vue_map->slot_to_varying[slot] == VARYING_SLOT_COL1 &&
            vue_map->slot_to_varying[slot+1] == VARYING_SLOT_BFC1)))
         attr->SwizzleSelect = INPUTATTR_FACING;
   }

   iris_emit_cmd(batch, GENX(3DSTATE_SBE_SWIZ), sbes) {
      for (int i = 0; i < 16; i++)
         sbes.Attribute[i] = attr_overrides[i];
   }
}

static unsigned
iris_calculate_point_sprite_overrides(const struct brw_wm_prog_data *prog_data,
                                      const struct iris_rasterizer_state *cso)
{
   unsigned overrides = 0;

   if (prog_data->urb_setup[VARYING_SLOT_PNTC] != -1)
      overrides |= 1 << prog_data->urb_setup[VARYING_SLOT_PNTC];

   for (int i = 0; i < 8; i++) {
      if ((cso->sprite_coord_enable & (1 << i)) &&
          prog_data->urb_setup[VARYING_SLOT_TEX0 + i] != -1)
         overrides |= 1 << prog_data->urb_setup[VARYING_SLOT_TEX0 + i];
   }

   return overrides;
}

static void
iris_emit_sbe(struct iris_batch *batch, const struct iris_context *ice)
{
   const struct iris_rasterizer_state *cso_rast = ice->state.cso_rast;
   const struct brw_wm_prog_data *wm_prog_data = (void *)
      ice->shaders.prog[MESA_SHADER_FRAGMENT]->prog_data;
   const struct shader_info *fs_info =
      iris_get_shader_info(ice, MESA_SHADER_FRAGMENT);

   unsigned urb_read_offset, urb_read_length;
   iris_compute_sbe_urb_read_interval(fs_info->inputs_read,
                                      ice->shaders.last_vue_map,
                                      cso_rast->light_twoside,
                                      &urb_read_offset, &urb_read_length);

   unsigned sprite_coord_overrides =
      iris_calculate_point_sprite_overrides(wm_prog_data, cso_rast);

   iris_emit_cmd(batch, GENX(3DSTATE_SBE), sbe) {
      sbe.AttributeSwizzleEnable = true;
      sbe.NumberofSFOutputAttributes = wm_prog_data->num_varying_inputs;
      sbe.PointSpriteTextureCoordinateOrigin = cso_rast->sprite_coord_mode;
      sbe.VertexURBEntryReadOffset = urb_read_offset;
      sbe.VertexURBEntryReadLength = urb_read_length;
      sbe.ForceVertexURBEntryReadOffset = true;
      sbe.ForceVertexURBEntryReadLength = true;
      sbe.ConstantInterpolationEnable = wm_prog_data->flat_inputs;
      sbe.PointSpriteTextureCoordinateEnable = sprite_coord_overrides;

      for (int i = 0; i < 32; i++) {
         sbe.AttributeActiveComponentFormat[i] = ACTIVE_COMPONENT_XYZW;
      }
   }

   iris_emit_sbe_swiz(batch, ice, urb_read_offset, sprite_coord_overrides);
}

/* ------------------------------------------------------------------- */

/**
 * Set sampler-related program key fields based on the current state.
 */
static void
iris_populate_sampler_key(const struct iris_context *ice,
                          struct brw_sampler_prog_key_data *key)
{
   for (int i = 0; i < MAX_SAMPLERS; i++) {
      key->swizzles[i] = 0x688; /* XYZW */
   }
}

/**
 * Populate VS program key fields based on the current state.
 */
static void
iris_populate_vs_key(const struct iris_context *ice,
                     struct brw_vs_prog_key *key)
{
   iris_populate_sampler_key(ice, &key->tex);
}

/**
 * Populate TCS program key fields based on the current state.
 */
static void
iris_populate_tcs_key(const struct iris_context *ice,
                      struct brw_tcs_prog_key *key)
{
   iris_populate_sampler_key(ice, &key->tex);
}

/**
 * Populate TES program key fields based on the current state.
 */
static void
iris_populate_tes_key(const struct iris_context *ice,
                      struct brw_tes_prog_key *key)
{
   iris_populate_sampler_key(ice, &key->tex);
}

/**
 * Populate GS program key fields based on the current state.
 */
static void
iris_populate_gs_key(const struct iris_context *ice,
                     struct brw_gs_prog_key *key)
{
   iris_populate_sampler_key(ice, &key->tex);
}

/**
 * Populate FS program key fields based on the current state.
 */
static void
iris_populate_fs_key(const struct iris_context *ice,
                     struct brw_wm_prog_key *key)
{
   iris_populate_sampler_key(ice, &key->tex);

   /* XXX: dirty flags? */
   const struct pipe_framebuffer_state *fb = &ice->state.framebuffer;
   const struct iris_depth_stencil_alpha_state *zsa = ice->state.cso_zsa;
   const struct iris_rasterizer_state *rast = ice->state.cso_rast;
   const struct iris_blend_state *blend = ice->state.cso_blend;

   key->nr_color_regions = fb->nr_cbufs;

   key->clamp_fragment_color = rast->clamp_fragment_color;

   key->replicate_alpha = fb->nr_cbufs > 1 &&
      (zsa->alpha.enabled || blend->alpha_to_coverage);

   /* XXX: only bother if COL0/1 are read */
   key->flat_shade = rast->flatshade;

   key->persample_interp = rast->force_persample_interp;
   key->multisample_fbo = rast->multisample && fb->samples > 1;

   key->coherent_fb_fetch = true;

   // XXX: uint64_t input_slots_valid; - for >16 inputs

   // XXX: key->force_dual_color_blend for unigine
   // XXX: respect hint for high_quality_derivatives:1;
}

static void
iris_populate_cs_key(const struct iris_context *ice,
                     struct brw_cs_prog_key *key)
{
   iris_populate_sampler_key(ice, &key->tex);
}

#if 0
   // XXX: these need to go in INIT_THREAD_DISPATCH_FIELDS
   pkt.SamplerCount =                                                     \
      DIV_ROUND_UP(CLAMP(stage_state->sampler_count, 0, 16), 4);          \
   pkt.PerThreadScratchSpace = prog_data->total_scratch == 0 ? 0 :        \
      ffs(stage_state->per_thread_scratch) - 11;                          \

#endif

static uint64_t
KSP(const struct iris_compiled_shader *shader)
{
   struct iris_resource *res = (void *) shader->assembly.res;
   return iris_bo_offset_from_base_address(res->bo) + shader->assembly.offset;
}

// Gen11 workaround table #2056 WABTPPrefetchDisable suggests to disable
// prefetching of binding tables in A0 and B0 steppings.  XXX: Revisit
// this WA on C0 stepping.

#define INIT_THREAD_DISPATCH_FIELDS(pkt, prefix)                          \
   pkt.KernelStartPointer = KSP(shader);                                  \
   pkt.BindingTableEntryCount = GEN_GEN == 11 ? 0 :                       \
      prog_data->binding_table.size_bytes / 4;                            \
   pkt.FloatingPointMode = prog_data->use_alt_mode;                       \
                                                                          \
   pkt.DispatchGRFStartRegisterForURBData =                               \
      prog_data->dispatch_grf_start_reg;                                  \
   pkt.prefix##URBEntryReadLength = vue_prog_data->urb_read_length;       \
   pkt.prefix##URBEntryReadOffset = 0;                                    \
                                                                          \
   pkt.StatisticsEnable = true;                                           \
   pkt.Enable           = true;

/**
 * Encode most of 3DSTATE_VS based on the compiled shader.
 */
static void
iris_store_vs_state(const struct gen_device_info *devinfo,
                    struct iris_compiled_shader *shader)
{
   struct brw_stage_prog_data *prog_data = shader->prog_data;
   struct brw_vue_prog_data *vue_prog_data = (void *) prog_data;

   iris_pack_command(GENX(3DSTATE_VS), shader->derived_data, vs) {
      INIT_THREAD_DISPATCH_FIELDS(vs, Vertex);
      vs.MaximumNumberofThreads = devinfo->max_vs_threads - 1;
      vs.SIMD8DispatchEnable = true;
      vs.UserClipDistanceCullTestEnableBitmask =
         vue_prog_data->cull_distance_mask;
   }
}

/**
 * Encode most of 3DSTATE_HS based on the compiled shader.
 */
static void
iris_store_tcs_state(const struct gen_device_info *devinfo,
                     struct iris_compiled_shader *shader)
{
   struct brw_stage_prog_data *prog_data = shader->prog_data;
   struct brw_vue_prog_data *vue_prog_data = (void *) prog_data;
   struct brw_tcs_prog_data *tcs_prog_data = (void *) prog_data;

   iris_pack_command(GENX(3DSTATE_HS), shader->derived_data, hs) {
      INIT_THREAD_DISPATCH_FIELDS(hs, Vertex);

      hs.InstanceCount = tcs_prog_data->instances - 1;
      hs.MaximumNumberofThreads = devinfo->max_tcs_threads - 1;
      hs.IncludeVertexHandles = true;
   }
}

/**
 * Encode 3DSTATE_TE and most of 3DSTATE_DS based on the compiled shader.
 */
static void
iris_store_tes_state(const struct gen_device_info *devinfo,
                     struct iris_compiled_shader *shader)
{
   struct brw_stage_prog_data *prog_data = shader->prog_data;
   struct brw_vue_prog_data *vue_prog_data = (void *) prog_data;
   struct brw_tes_prog_data *tes_prog_data = (void *) prog_data;

   uint32_t *te_state = (void *) shader->derived_data;
   uint32_t *ds_state = te_state + GENX(3DSTATE_TE_length);

   iris_pack_command(GENX(3DSTATE_TE), te_state, te) {
      te.Partitioning = tes_prog_data->partitioning;
      te.OutputTopology = tes_prog_data->output_topology;
      te.TEDomain = tes_prog_data->domain;
      te.TEEnable = true;
      te.MaximumTessellationFactorOdd = 63.0;
      te.MaximumTessellationFactorNotOdd = 64.0;
   }

   iris_pack_command(GENX(3DSTATE_DS), ds_state, ds) {
      INIT_THREAD_DISPATCH_FIELDS(ds, Patch);

      ds.DispatchMode = DISPATCH_MODE_SIMD8_SINGLE_PATCH;
      ds.MaximumNumberofThreads = devinfo->max_tes_threads - 1;
      ds.ComputeWCoordinateEnable =
         tes_prog_data->domain == BRW_TESS_DOMAIN_TRI;

      ds.UserClipDistanceCullTestEnableBitmask =
         vue_prog_data->cull_distance_mask;
   }

}

/**
 * Encode most of 3DSTATE_GS based on the compiled shader.
 */
static void
iris_store_gs_state(const struct gen_device_info *devinfo,
                    struct iris_compiled_shader *shader)
{
   struct brw_stage_prog_data *prog_data = shader->prog_data;
   struct brw_vue_prog_data *vue_prog_data = (void *) prog_data;
   struct brw_gs_prog_data *gs_prog_data = (void *) prog_data;

   iris_pack_command(GENX(3DSTATE_GS), shader->derived_data, gs) {
      INIT_THREAD_DISPATCH_FIELDS(gs, Vertex);

      gs.OutputVertexSize = gs_prog_data->output_vertex_size_hwords * 2 - 1;
      gs.OutputTopology = gs_prog_data->output_topology;
      gs.ControlDataHeaderSize =
         gs_prog_data->control_data_header_size_hwords;
      gs.InstanceControl = gs_prog_data->invocations - 1;
      gs.DispatchMode = DISPATCH_MODE_SIMD8;
      gs.IncludePrimitiveID = gs_prog_data->include_primitive_id;
      gs.ControlDataFormat = gs_prog_data->control_data_format;
      gs.ReorderMode = TRAILING;
      gs.ExpectedVertexCount = gs_prog_data->vertices_in;
      gs.MaximumNumberofThreads =
         GEN_GEN == 8 ? (devinfo->max_gs_threads / 2 - 1)
                      : (devinfo->max_gs_threads - 1);

      if (gs_prog_data->static_vertex_count != -1) {
         gs.StaticOutput = true;
         gs.StaticOutputVertexCount = gs_prog_data->static_vertex_count;
      }
      gs.IncludeVertexHandles = vue_prog_data->include_vue_handles;

      gs.UserClipDistanceCullTestEnableBitmask =
         vue_prog_data->cull_distance_mask;

      const int urb_entry_write_offset = 1;
      const uint32_t urb_entry_output_length =
         DIV_ROUND_UP(vue_prog_data->vue_map.num_slots, 2) -
         urb_entry_write_offset;

      gs.VertexURBEntryOutputReadOffset = urb_entry_write_offset;
      gs.VertexURBEntryOutputLength = MAX2(urb_entry_output_length, 1);
   }
}

/**
 * Encode most of 3DSTATE_PS and 3DSTATE_PS_EXTRA based on the shader.
 */
static void
iris_store_fs_state(const struct gen_device_info *devinfo,
                    struct iris_compiled_shader *shader)
{
   struct brw_stage_prog_data *prog_data = shader->prog_data;
   struct brw_wm_prog_data *wm_prog_data = (void *) shader->prog_data;

   uint32_t *ps_state = (void *) shader->derived_data;
   uint32_t *psx_state = ps_state + GENX(3DSTATE_PS_length);

   iris_pack_command(GENX(3DSTATE_PS), ps_state, ps) {
      ps.VectorMaskEnable = true;
      //ps.SamplerCount = ...
      // XXX: WABTPPrefetchDisable, see above, drop at C0
      ps.BindingTableEntryCount = GEN_GEN == 11 ? 0 :
         prog_data->binding_table.size_bytes / 4;
      ps.FloatingPointMode = prog_data->use_alt_mode;
      ps.MaximumNumberofThreadsPerPSD = 64 - (GEN_GEN == 8 ? 2 : 1);

      ps.PushConstantEnable = prog_data->nr_params > 0 ||
                              prog_data->ubo_ranges[0].length > 0;

      /* From the documentation for this packet:
       * "If the PS kernel does not need the Position XY Offsets to
       *  compute a Position Value, then this field should be programmed
       *  to POSOFFSET_NONE."
       *
       * "SW Recommendation: If the PS kernel needs the Position Offsets
       *  to compute a Position XY value, this field should match Position
       *  ZW Interpolation Mode to ensure a consistent position.xyzw
       *  computation."
       *
       * We only require XY sample offsets. So, this recommendation doesn't
       * look useful at the moment.  We might need this in future.
       */
      ps.PositionXYOffsetSelect =
         wm_prog_data->uses_pos_offset ? POSOFFSET_SAMPLE : POSOFFSET_NONE;
      ps._8PixelDispatchEnable = wm_prog_data->dispatch_8;
      ps._16PixelDispatchEnable = wm_prog_data->dispatch_16;
      ps._32PixelDispatchEnable = wm_prog_data->dispatch_32;

      // XXX: Disable SIMD32 with 16x MSAA

      ps.DispatchGRFStartRegisterForConstantSetupData0 =
         brw_wm_prog_data_dispatch_grf_start_reg(wm_prog_data, ps, 0);
      ps.DispatchGRFStartRegisterForConstantSetupData1 =
         brw_wm_prog_data_dispatch_grf_start_reg(wm_prog_data, ps, 1);
      ps.DispatchGRFStartRegisterForConstantSetupData2 =
         brw_wm_prog_data_dispatch_grf_start_reg(wm_prog_data, ps, 2);

      ps.KernelStartPointer0 =
         KSP(shader) + brw_wm_prog_data_prog_offset(wm_prog_data, ps, 0);
      ps.KernelStartPointer1 =
         KSP(shader) + brw_wm_prog_data_prog_offset(wm_prog_data, ps, 1);
      ps.KernelStartPointer2 =
         KSP(shader) + brw_wm_prog_data_prog_offset(wm_prog_data, ps, 2);
   }

   iris_pack_command(GENX(3DSTATE_PS_EXTRA), psx_state, psx) {
      psx.PixelShaderValid = true;
      psx.PixelShaderComputedDepthMode = wm_prog_data->computed_depth_mode;
      psx.PixelShaderKillsPixel = wm_prog_data->uses_kill;
      psx.AttributeEnable = wm_prog_data->num_varying_inputs != 0;
      psx.PixelShaderUsesSourceDepth = wm_prog_data->uses_src_depth;
      psx.PixelShaderUsesSourceW = wm_prog_data->uses_src_w;
      psx.PixelShaderIsPerSample = wm_prog_data->persample_dispatch;

      if (wm_prog_data->uses_sample_mask) {
         /* TODO: conservative rasterization */
         if (wm_prog_data->post_depth_coverage)
            psx.InputCoverageMaskState = ICMS_DEPTH_COVERAGE;
         else
            psx.InputCoverageMaskState = ICMS_NORMAL;
      }

      psx.oMaskPresenttoRenderTarget = wm_prog_data->uses_omask;
      psx.PixelShaderPullsBary = wm_prog_data->pulls_bary;
      psx.PixelShaderComputesStencil = wm_prog_data->computed_stencil;

      // XXX: UAV bit
   }
}

/**
 * Compute the size of the derived data (shader command packets).
 *
 * This must match the data written by the iris_store_xs_state() functions.
 */
static void
iris_store_cs_state(const struct gen_device_info *devinfo,
                    struct iris_compiled_shader *shader)
{
   struct brw_stage_prog_data *prog_data = shader->prog_data;
   struct brw_cs_prog_data *cs_prog_data = (void *) shader->prog_data;
   void *map = shader->derived_data;

   iris_pack_state(GENX(INTERFACE_DESCRIPTOR_DATA), map, desc) {
      desc.KernelStartPointer = KSP(shader);
      desc.ConstantURBEntryReadLength = cs_prog_data->push.per_thread.regs;
      desc.NumberofThreadsinGPGPUThreadGroup = cs_prog_data->threads;
      desc.SharedLocalMemorySize =
         encode_slm_size(GEN_GEN, prog_data->total_shared);
      desc.BarrierEnable = cs_prog_data->uses_barrier;
      desc.CrossThreadConstantDataReadLength =
         cs_prog_data->push.cross_thread.regs;
   }
}

static unsigned
iris_derived_program_state_size(enum iris_program_cache_id cache_id)
{
   assert(cache_id <= IRIS_CACHE_BLORP);

   static const unsigned dwords[] = {
      [IRIS_CACHE_VS] = GENX(3DSTATE_VS_length),
      [IRIS_CACHE_TCS] = GENX(3DSTATE_HS_length),
      [IRIS_CACHE_TES] = GENX(3DSTATE_TE_length) + GENX(3DSTATE_DS_length),
      [IRIS_CACHE_GS] = GENX(3DSTATE_GS_length),
      [IRIS_CACHE_FS] =
         GENX(3DSTATE_PS_length) + GENX(3DSTATE_PS_EXTRA_length),
      [IRIS_CACHE_CS] = GENX(INTERFACE_DESCRIPTOR_DATA_length),
      [IRIS_CACHE_BLORP] = 0,
   };

   return sizeof(uint32_t) * dwords[cache_id];
}

/**
 * Create any state packets corresponding to the given shader stage
 * (i.e. 3DSTATE_VS) and save them as "derived data" in the shader variant.
 * This means that we can look up a program in the in-memory cache and
 * get most of the state packet without having to reconstruct it.
 */
static void
iris_store_derived_program_state(const struct gen_device_info *devinfo,
                                 enum iris_program_cache_id cache_id,
                                 struct iris_compiled_shader *shader)
{
   switch (cache_id) {
   case IRIS_CACHE_VS:
      iris_store_vs_state(devinfo, shader);
      break;
   case IRIS_CACHE_TCS:
      iris_store_tcs_state(devinfo, shader);
      break;
   case IRIS_CACHE_TES:
      iris_store_tes_state(devinfo, shader);
      break;
   case IRIS_CACHE_GS:
      iris_store_gs_state(devinfo, shader);
      break;
   case IRIS_CACHE_FS:
      iris_store_fs_state(devinfo, shader);
      break;
   case IRIS_CACHE_CS:
      iris_store_cs_state(devinfo, shader);
   case IRIS_CACHE_BLORP:
      break;
   default:
      break;
   }
}

/* ------------------------------------------------------------------- */

/**
 * Configure the URB.
 *
 * XXX: write a real comment.
 */
static void
iris_upload_urb_config(struct iris_context *ice, struct iris_batch *batch)
{
   const struct gen_device_info *devinfo = &batch->screen->devinfo;
   const unsigned push_size_kB = 32;
   unsigned entries[4];
   unsigned start[4];
   unsigned size[4];

   for (int i = MESA_SHADER_VERTEX; i <= MESA_SHADER_GEOMETRY; i++) {
      if (!ice->shaders.prog[i]) {
         size[i] = 1;
      } else {
         struct brw_vue_prog_data *vue_prog_data =
            (void *) ice->shaders.prog[i]->prog_data;
         size[i] = vue_prog_data->urb_entry_size;
      }
      assert(size[i] != 0);
   }

   gen_get_urb_config(devinfo, 1024 * push_size_kB,
                      1024 * ice->shaders.urb_size,
                      ice->shaders.prog[MESA_SHADER_TESS_EVAL] != NULL,
                      ice->shaders.prog[MESA_SHADER_GEOMETRY] != NULL,
                      size, entries, start);

   for (int i = MESA_SHADER_VERTEX; i <= MESA_SHADER_GEOMETRY; i++) {
      iris_emit_cmd(batch, GENX(3DSTATE_URB_VS), urb) {
         urb._3DCommandSubOpcode += i;
         urb.VSURBStartingAddress     = start[i];
         urb.VSURBEntryAllocationSize = size[i] - 1;
         urb.VSNumberofURBEntries     = entries[i];
      }
   }
}

static const uint32_t push_constant_opcodes[] = {
   [MESA_SHADER_VERTEX]    = 21,
   [MESA_SHADER_TESS_CTRL] = 25, /* HS */
   [MESA_SHADER_TESS_EVAL] = 26, /* DS */
   [MESA_SHADER_GEOMETRY]  = 22,
   [MESA_SHADER_FRAGMENT]  = 23,
   [MESA_SHADER_COMPUTE]   = 0,
};

static uint32_t
use_null_surface(struct iris_batch *batch, struct iris_context *ice)
{
   struct iris_bo *state_bo = iris_resource_bo(ice->state.unbound_tex.res);

   iris_use_pinned_bo(batch, state_bo, false);

   return ice->state.unbound_tex.offset;
}

static uint32_t
use_null_fb_surface(struct iris_batch *batch, struct iris_context *ice)
{
   /* If set_framebuffer_state() was never called, fall back to 1x1x1 */
   if (!ice->state.null_fb.res)
      return use_null_surface(batch, ice);

   struct iris_bo *state_bo = iris_resource_bo(ice->state.null_fb.res);

   iris_use_pinned_bo(batch, state_bo, false);

   return ice->state.null_fb.offset;
}

/**
 * Add a surface to the validation list, as well as the buffer containing
 * the corresponding SURFACE_STATE.
 *
 * Returns the binding table entry (offset to SURFACE_STATE).
 */
static uint32_t
use_surface(struct iris_batch *batch,
            struct pipe_surface *p_surf,
            bool writeable)
{
   struct iris_surface *surf = (void *) p_surf;

   iris_use_pinned_bo(batch, iris_resource_bo(p_surf->texture), writeable);
   iris_use_pinned_bo(batch, iris_resource_bo(surf->surface_state.res), false);

   return surf->surface_state.offset;
}

static uint32_t
use_sampler_view(struct iris_batch *batch, struct iris_sampler_view *isv)
{
   iris_use_pinned_bo(batch, isv->res->bo, false);
   iris_use_pinned_bo(batch, iris_resource_bo(isv->surface_state.res), false);

   return isv->surface_state.offset;
}

static uint32_t
use_const_buffer(struct iris_batch *batch,
                 struct iris_context *ice,
                 struct iris_const_buffer *cbuf)
{
   if (!cbuf->surface_state.res)
      return use_null_surface(batch, ice);

   iris_use_pinned_bo(batch, iris_resource_bo(cbuf->data.res), false);
   iris_use_pinned_bo(batch, iris_resource_bo(cbuf->surface_state.res), false);

   return cbuf->surface_state.offset;
}

static uint32_t
use_ssbo(struct iris_batch *batch, struct iris_context *ice,
         struct iris_shader_state *shs, int i)
{
   if (!shs->ssbo[i])
      return use_null_surface(batch, ice);

   struct iris_state_ref *surf_state = &shs->ssbo_surface_state[i];

   iris_use_pinned_bo(batch, iris_resource_bo(shs->ssbo[i]), true);
   iris_use_pinned_bo(batch, iris_resource_bo(surf_state->res), false);

   return surf_state->offset;
}

static uint32_t
use_image(struct iris_batch *batch, struct iris_context *ice,
          struct iris_shader_state *shs, int i)
{
   if (!shs->image[i].res)
      return use_null_surface(batch, ice);

   struct iris_state_ref *surf_state = &shs->image[i].surface_state;

   iris_use_pinned_bo(batch, iris_resource_bo(shs->image[i].res),
                      shs->image[i].access & PIPE_IMAGE_ACCESS_WRITE);
   iris_use_pinned_bo(batch, iris_resource_bo(surf_state->res), false);

   return surf_state->offset;
}

#define push_bt_entry(addr) \
   assert(addr >= binder_addr); \
   if (!pin_only) bt_map[s++] = (addr) - binder_addr;

/**
 * Populate the binding table for a given shader stage.
 *
 * This fills out the table of pointers to surfaces required by the shader,
 * and also adds those buffers to the validation list so the kernel can make
 * resident before running our batch.
 */
static void
iris_populate_binding_table(struct iris_context *ice,
                            struct iris_batch *batch,
                            gl_shader_stage stage,
                            bool pin_only)
{
   const struct iris_binder *binder = &ice->state.binder;
   struct iris_compiled_shader *shader = ice->shaders.prog[stage];
   if (!shader)
      return;

   struct iris_shader_state *shs = &ice->state.shaders[stage];
   uint32_t binder_addr = binder->bo->gtt_offset;

   //struct brw_stage_prog_data *prog_data = (void *) shader->prog_data;
   uint32_t *bt_map = binder->map + binder->bt_offset[stage];
   int s = 0;

   const struct shader_info *info = iris_get_shader_info(ice, stage);
   if (!info) {
      /* TCS passthrough doesn't need a binding table. */
      assert(stage == MESA_SHADER_TESS_CTRL);
      return;
   }

   if (stage == MESA_SHADER_COMPUTE) {
      /* surface for gl_NumWorkGroups */
      struct iris_state_ref *grid_data = &ice->state.grid_size;
      struct iris_state_ref *grid_state = &ice->state.grid_surf_state;
      iris_use_pinned_bo(batch, iris_resource_bo(grid_data->res),  false);
      iris_use_pinned_bo(batch, iris_resource_bo(grid_state->res), false);
      push_bt_entry(grid_state->offset);
   }

   if (stage == MESA_SHADER_FRAGMENT) {
      struct pipe_framebuffer_state *cso_fb = &ice->state.framebuffer;
      /* Note that cso_fb->nr_cbufs == fs_key->nr_color_regions. */
      if (cso_fb->nr_cbufs) {
         for (unsigned i = 0; i < cso_fb->nr_cbufs; i++) {
            uint32_t addr =
               cso_fb->cbufs[i] ? use_surface(batch, cso_fb->cbufs[i], true)
                                : use_null_fb_surface(batch, ice);
            push_bt_entry(addr);
         }
      } else {
         uint32_t addr = use_null_fb_surface(batch, ice);
         push_bt_entry(addr);
      }
   }

   //assert(prog_data->binding_table.texture_start ==
          //(ice->state.num_textures[stage] ? s : 0xd0d0d0d0));

   for (int i = 0; i < shs->num_textures; i++) {
      struct iris_sampler_view *view = shs->textures[i];
      uint32_t addr = view ? use_sampler_view(batch, view)
                           : use_null_surface(batch, ice);
      push_bt_entry(addr);
   }

   for (int i = 0; i < info->num_images; i++) {
      uint32_t addr = use_image(batch, ice, shs, i);
      push_bt_entry(addr);
   }

   const int num_ubos = iris_get_shader_num_ubos(ice, stage);

   for (int i = 0; i < num_ubos; i++) {
      uint32_t addr = use_const_buffer(batch, ice, &shs->constbuf[i]);
      push_bt_entry(addr);
   }

   /* XXX: st is wasting 16 binding table slots for ABOs.  Should add a cap
    * for changing nir_lower_atomics_to_ssbos setting and buffer_base offset
    * in st_atom_storagebuf.c so it'll compact them into one range, with
    * SSBOs starting at info->num_abos.  Ideally it'd reset num_abos to 0 too
    */
   if (info->num_abos + info->num_ssbos > 0) {
      for (int i = 0; i < IRIS_MAX_ABOS + info->num_ssbos; i++) {
         uint32_t addr = use_ssbo(batch, ice, shs, i);
         push_bt_entry(addr);
      }
   }

#if 0
      // XXX: not implemented yet
      assert(prog_data->binding_table.plane_start[1] == 0xd0d0d0d0);
      assert(prog_data->binding_table.plane_start[2] == 0xd0d0d0d0);
#endif
}

static void
iris_use_optional_res(struct iris_batch *batch,
                      struct pipe_resource *res,
                      bool writeable)
{
   if (res) {
      struct iris_bo *bo = iris_resource_bo(res);
      iris_use_pinned_bo(batch, bo, writeable);
   }
}

/* ------------------------------------------------------------------- */

/**
 * Pin any BOs which were installed by a previous batch, and restored
 * via the hardware logical context mechanism.
 *
 * We don't need to re-emit all state every batch - the hardware context
 * mechanism will save and restore it for us.  This includes pointers to
 * various BOs...which won't exist unless we ask the kernel to pin them
 * by adding them to the validation list.
 *
 * We can skip buffers if we've re-emitted those packets, as we're
 * overwriting those stale pointers with new ones, and don't actually
 * refer to the old BOs.
 */
static void
iris_restore_render_saved_bos(struct iris_context *ice,
                              struct iris_batch *batch,
                              const struct pipe_draw_info *draw)
{
   // XXX: whack IRIS_SHADER_DIRTY_BINDING_TABLE on new batch

   const uint64_t clean = ~ice->state.dirty;

   if (clean & IRIS_DIRTY_CC_VIEWPORT) {
      iris_use_optional_res(batch, ice->state.last_res.cc_vp, false);
   }

   if (clean & IRIS_DIRTY_SF_CL_VIEWPORT) {
      iris_use_optional_res(batch, ice->state.last_res.sf_cl_vp, false);
   }

   if (clean & IRIS_DIRTY_BLEND_STATE) {
      iris_use_optional_res(batch, ice->state.last_res.blend, false);
   }

   if (clean & IRIS_DIRTY_COLOR_CALC_STATE) {
      iris_use_optional_res(batch, ice->state.last_res.color_calc, false);
   }

   if (clean & IRIS_DIRTY_SCISSOR_RECT) {
      iris_use_optional_res(batch, ice->state.last_res.scissor, false);
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (!(clean & (IRIS_DIRTY_CONSTANTS_VS << stage)))
         continue;

      struct iris_shader_state *shs = &ice->state.shaders[stage];
      struct iris_compiled_shader *shader = ice->shaders.prog[stage];

      if (!shader)
         continue;

      struct brw_stage_prog_data *prog_data = (void *) shader->prog_data;

      for (int i = 0; i < 4; i++) {
         const struct brw_ubo_range *range = &prog_data->ubo_ranges[i];

         if (range->length == 0)
            continue;

         struct iris_const_buffer *cbuf = &shs->constbuf[range->block];
         struct iris_resource *res = (void *) cbuf->data.res;

         if (res)
            iris_use_pinned_bo(batch, res->bo, false);
         else
            iris_use_pinned_bo(batch, batch->screen->workaround_bo, false);
      }
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (clean & (IRIS_DIRTY_BINDINGS_VS << stage)) {
         /* Re-pin any buffers referred to by the binding table. */
         iris_populate_binding_table(ice, batch, stage, true);
      }
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      struct iris_shader_state *shs = &ice->state.shaders[stage];
      struct pipe_resource *res = shs->sampler_table.res;
      if (res)
         iris_use_pinned_bo(batch, iris_resource_bo(res), false);
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (clean & (IRIS_DIRTY_VS << stage)) {
         struct iris_compiled_shader *shader = ice->shaders.prog[stage];
         if (shader) {
            struct iris_bo *bo = iris_resource_bo(shader->assembly.res);
            iris_use_pinned_bo(batch, bo, false);
         }

         // XXX: scratch buffer
      }
   }

   if (clean & IRIS_DIRTY_DEPTH_BUFFER) {
      struct pipe_framebuffer_state *cso_fb = &ice->state.framebuffer;

      if (cso_fb->zsbuf) {
         struct iris_resource *zres, *sres;
         iris_get_depth_stencil_resources(cso_fb->zsbuf->texture,
                                          &zres, &sres);
         // XXX: might not be writable...
         if (zres)
            iris_use_pinned_bo(batch, zres->bo, true);
         if (sres)
            iris_use_pinned_bo(batch, sres->bo, true);
      }
   }

   if (draw->index_size == 0 && ice->state.last_res.index_buffer) {
      /* This draw didn't emit a new index buffer, so we are inheriting the
       * older index buffer.  This draw didn't need it, but future ones may.
       */
      struct iris_bo *bo = iris_resource_bo(ice->state.last_res.index_buffer);
      iris_use_pinned_bo(batch, bo, false);
   }

   if (clean & IRIS_DIRTY_VERTEX_BUFFERS) {
      struct iris_vertex_buffer_state *cso = &ice->state.genx->vertex_buffers;
      for (unsigned i = 0; i < cso->num_buffers; i++) {
         struct iris_resource *res = (void *) cso->resources[i];
         iris_use_pinned_bo(batch, res->bo, false);
      }
   }
}

static void
iris_restore_compute_saved_bos(struct iris_context *ice,
                               struct iris_batch *batch,
                               const struct pipe_grid_info *grid)
{
   const uint64_t clean = ~ice->state.dirty;

   const int stage = MESA_SHADER_COMPUTE;
   struct iris_shader_state *shs = &ice->state.shaders[stage];

   if (clean & IRIS_DIRTY_CONSTANTS_CS) {
      struct iris_compiled_shader *shader = ice->shaders.prog[stage];

      if (shader) {
         struct brw_stage_prog_data *prog_data = (void *) shader->prog_data;
         const struct brw_ubo_range *range = &prog_data->ubo_ranges[0];

         if (range->length > 0) {
            struct iris_const_buffer *cbuf = &shs->constbuf[range->block];
            struct iris_resource *res = (void *) cbuf->data.res;

            if (res)
               iris_use_pinned_bo(batch, res->bo, false);
            else
               iris_use_pinned_bo(batch, batch->screen->workaround_bo, false);
           }
      }
   }

   if (clean & IRIS_DIRTY_BINDINGS_CS) {
      /* Re-pin any buffers referred to by the binding table. */
      iris_populate_binding_table(ice, batch, stage, true);
   }

   struct pipe_resource *sampler_res = shs->sampler_table.res;
   if (sampler_res)
      iris_use_pinned_bo(batch, iris_resource_bo(sampler_res), false);

   if (clean & IRIS_DIRTY_CS) {
      struct iris_compiled_shader *shader = ice->shaders.prog[stage];
      if (shader) {
         struct iris_bo *bo = iris_resource_bo(shader->assembly.res);
         iris_use_pinned_bo(batch, bo, false);
      }

      // XXX: scratch buffer
   }
}

/**
 * Possibly emit STATE_BASE_ADDRESS to update Surface State Base Address.
 */
static void
iris_update_surface_base_address(struct iris_batch *batch,
                                 struct iris_binder *binder)
{
   if (batch->last_surface_base_address == binder->bo->gtt_offset)
      return;

   flush_for_state_base_change(batch);

   iris_emit_cmd(batch, GENX(STATE_BASE_ADDRESS), sba) {
      // XXX: sba.SurfaceStateMemoryObjectControlState = MOCS_WB;
      sba.SurfaceStateBaseAddressModifyEnable = true;
      sba.SurfaceStateBaseAddress = ro_bo(binder->bo, 0);
   }

   batch->last_surface_base_address = binder->bo->gtt_offset;
}

static void
iris_upload_dirty_render_state(struct iris_context *ice,
                               struct iris_batch *batch,
                               const struct pipe_draw_info *draw)
{
   const uint64_t dirty = ice->state.dirty;

   if (!(dirty & IRIS_ALL_DIRTY_FOR_RENDER))
      return;

   struct iris_genx_state *genx = ice->state.genx;
   struct iris_binder *binder = &ice->state.binder;
   struct brw_wm_prog_data *wm_prog_data = (void *)
      ice->shaders.prog[MESA_SHADER_FRAGMENT]->prog_data;

   if (dirty & IRIS_DIRTY_CC_VIEWPORT) {
      const struct iris_rasterizer_state *cso_rast = ice->state.cso_rast;
      uint32_t cc_vp_address;

      /* XXX: could avoid streaming for depth_clip [0,1] case. */
      uint32_t *cc_vp_map =
         stream_state(batch, ice->state.dynamic_uploader,
                      &ice->state.last_res.cc_vp,
                      4 * ice->state.num_viewports *
                      GENX(CC_VIEWPORT_length), 32, &cc_vp_address);
      for (int i = 0; i < ice->state.num_viewports; i++) {
         float zmin, zmax;
         util_viewport_zmin_zmax(&ice->state.viewports[i],
                                 cso_rast->clip_halfz, &zmin, &zmax);
         if (cso_rast->depth_clip_near)
            zmin = 0.0;
         if (cso_rast->depth_clip_far)
            zmax = 1.0;

         iris_pack_state(GENX(CC_VIEWPORT), cc_vp_map, ccv) {
            ccv.MinimumDepth = zmin;
            ccv.MaximumDepth = zmax;
         }

         cc_vp_map += GENX(CC_VIEWPORT_length);
      }

      iris_emit_cmd(batch, GENX(3DSTATE_VIEWPORT_STATE_POINTERS_CC), ptr) {
         ptr.CCViewportPointer = cc_vp_address;
      }
   }

   if (dirty & IRIS_DIRTY_SF_CL_VIEWPORT) {
      iris_emit_cmd(batch, GENX(3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP), ptr) {
         ptr.SFClipViewportPointer =
            emit_state(batch, ice->state.dynamic_uploader,
                       &ice->state.last_res.sf_cl_vp,
                       genx->sf_cl_vp, 4 * GENX(SF_CLIP_VIEWPORT_length) *
                       ice->state.num_viewports, 64);
      }
   }

   /* XXX: L3 State */

   // XXX: this is only flagged at setup, we assume a static configuration
   if (dirty & IRIS_DIRTY_URB) {
      iris_upload_urb_config(ice, batch);
   }

   if (dirty & IRIS_DIRTY_BLEND_STATE) {
      struct iris_blend_state *cso_blend = ice->state.cso_blend;
      struct pipe_framebuffer_state *cso_fb = &ice->state.framebuffer;
      struct iris_depth_stencil_alpha_state *cso_zsa = ice->state.cso_zsa;
      const int header_dwords = GENX(BLEND_STATE_length);
      const int rt_dwords = cso_fb->nr_cbufs * GENX(BLEND_STATE_ENTRY_length);
      uint32_t blend_offset;
      uint32_t *blend_map =
         stream_state(batch, ice->state.dynamic_uploader,
                      &ice->state.last_res.blend,
                      4 * (header_dwords + rt_dwords), 64, &blend_offset);

      uint32_t blend_state_header;
      iris_pack_state(GENX(BLEND_STATE), &blend_state_header, bs) {
         bs.AlphaTestEnable = cso_zsa->alpha.enabled;
         bs.AlphaTestFunction = translate_compare_func(cso_zsa->alpha.func);
      }

      blend_map[0] = blend_state_header | cso_blend->blend_state[0];
      memcpy(&blend_map[1], &cso_blend->blend_state[1], 4 * rt_dwords);

      iris_emit_cmd(batch, GENX(3DSTATE_BLEND_STATE_POINTERS), ptr) {
         ptr.BlendStatePointer = blend_offset;
         ptr.BlendStatePointerValid = true;
      }
   }

   if (dirty & IRIS_DIRTY_COLOR_CALC_STATE) {
      struct iris_depth_stencil_alpha_state *cso = ice->state.cso_zsa;
      uint32_t cc_offset;
      void *cc_map =
         stream_state(batch, ice->state.dynamic_uploader,
                      &ice->state.last_res.color_calc,
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

   /* Upload constants for TCS passthrough. */
   if ((dirty & IRIS_DIRTY_CONSTANTS_TCS) &&
       ice->shaders.prog[MESA_SHADER_TESS_CTRL] &&
       !ice->shaders.uncompiled[MESA_SHADER_TESS_CTRL]) {
      struct iris_compiled_shader *tes_shader = ice->shaders.prog[MESA_SHADER_TESS_EVAL];
      assert(tes_shader);

      /* Passthrough always copies 2 vec4s, so when uploading data we ensure
       * it is in the right layout for TES.
       */
      float hdr[8] = {};
      struct brw_tes_prog_data *tes_prog_data = (void *) tes_shader->prog_data;
      switch (tes_prog_data->domain) {
      case BRW_TESS_DOMAIN_QUAD:
         for (int i = 0; i < 4; i++)
            hdr[7 - i] = ice->state.default_outer_level[i];
         hdr[3] = ice->state.default_inner_level[0];
         hdr[2] = ice->state.default_inner_level[1];
         break;
      case BRW_TESS_DOMAIN_TRI:
         for (int i = 0; i < 3; i++)
            hdr[7 - i] = ice->state.default_outer_level[i];
         hdr[4] = ice->state.default_inner_level[0];
         break;
      case BRW_TESS_DOMAIN_ISOLINE:
         hdr[7] = ice->state.default_outer_level[1];
         hdr[6] = ice->state.default_outer_level[0];
         break;
      }

      struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_TESS_CTRL];
      struct iris_const_buffer *cbuf = &shs->constbuf[0];
      u_upload_data(ice->ctx.const_uploader, 0, sizeof(hdr), 32,
                    &hdr[0], &cbuf->data.offset,
                    &cbuf->data.res);
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (!(dirty & (IRIS_DIRTY_CONSTANTS_VS << stage)))
         continue;

      struct iris_shader_state *shs = &ice->state.shaders[stage];
      struct iris_compiled_shader *shader = ice->shaders.prog[stage];

      if (!shader)
         continue;

      struct brw_stage_prog_data *prog_data = (void *) shader->prog_data;

      iris_emit_cmd(batch, GENX(3DSTATE_CONSTANT_VS), pkt) {
         pkt._3DCommandSubOpcode = push_constant_opcodes[stage];
         if (prog_data) {
            /* The Skylake PRM contains the following restriction:
             *
             *    "The driver must ensure The following case does not occur
             *     without a flush to the 3D engine: 3DSTATE_CONSTANT_* with
             *     buffer 3 read length equal to zero committed followed by a
             *     3DSTATE_CONSTANT_* with buffer 0 read length not equal to
             *     zero committed."
             *
             * To avoid this, we program the buffers in the highest slots.
             * This way, slot 0 is only used if slot 3 is also used.
             */
            int n = 3;

            for (int i = 3; i >= 0; i--) {
               const struct brw_ubo_range *range = &prog_data->ubo_ranges[i];

               if (range->length == 0)
                  continue;

               struct iris_const_buffer *cbuf = &shs->constbuf[range->block];
               struct iris_resource *res = (void *) cbuf->data.res;

               assert(cbuf->data.offset % 32 == 0);

               pkt.ConstantBody.ReadLength[n] = range->length;
               pkt.ConstantBody.Buffer[n] =
                  res ? ro_bo(res->bo, range->start * 32 + cbuf->data.offset)
                      : ro_bo(batch->screen->workaround_bo, 0);
               n--;
            }
         }
      }
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (dirty & (IRIS_DIRTY_BINDINGS_VS << stage)) {
         iris_emit_cmd(batch, GENX(3DSTATE_BINDING_TABLE_POINTERS_VS), ptr) {
            ptr._3DCommandSubOpcode = 38 + stage;
            ptr.PointertoVSBindingTable = binder->bt_offset[stage];
         }
      }
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (dirty & (IRIS_DIRTY_BINDINGS_VS << stage)) {
         iris_populate_binding_table(ice, batch, stage, false);
      }
   }

   if (ice->state.need_border_colors)
      iris_use_pinned_bo(batch, ice->state.border_color_pool.bo, false);

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (!(dirty & (IRIS_DIRTY_SAMPLER_STATES_VS << stage)) ||
          !ice->shaders.prog[stage])
         continue;

      struct iris_shader_state *shs = &ice->state.shaders[stage];
      struct pipe_resource *res = shs->sampler_table.res;
      if (res)
         iris_use_pinned_bo(batch, iris_resource_bo(res), false);

      iris_emit_cmd(batch, GENX(3DSTATE_SAMPLER_STATE_POINTERS_VS), ptr) {
         ptr._3DCommandSubOpcode = 43 + stage;
         ptr.PointertoVSSamplerState = shs->sampler_table.offset;
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
         ms.SampleMask = MAX2(ice->state.sample_mask, 1);
      }
   }

   for (int stage = 0; stage <= MESA_SHADER_FRAGMENT; stage++) {
      if (!(dirty & (IRIS_DIRTY_VS << stage)))
         continue;

      struct iris_compiled_shader *shader = ice->shaders.prog[stage];

      if (shader) {
         struct iris_resource *cache = (void *) shader->assembly.res;
         iris_use_pinned_bo(batch, cache->bo, false);
         iris_batch_emit(batch, shader->derived_data,
                         iris_derived_program_state_size(stage));
      } else {
         if (stage == MESA_SHADER_TESS_EVAL) {
            iris_emit_cmd(batch, GENX(3DSTATE_HS), hs);
            iris_emit_cmd(batch, GENX(3DSTATE_TE), te);
            iris_emit_cmd(batch, GENX(3DSTATE_DS), ds);
         } else if (stage == MESA_SHADER_GEOMETRY) {
            iris_emit_cmd(batch, GENX(3DSTATE_GS), gs);
         }
      }
   }

   if (ice->state.streamout_active) {
      if (dirty & IRIS_DIRTY_SO_BUFFERS) {
         iris_batch_emit(batch, genx->so_buffers,
                         4 * 4 * GENX(3DSTATE_SO_BUFFER_length));
         for (int i = 0; i < 4; i++) {
            struct iris_stream_output_target *tgt =
               (void *) ice->state.so_target[i];
            if (tgt) {
               iris_use_pinned_bo(batch, iris_resource_bo(tgt->base.buffer),
                                  true);
               iris_use_pinned_bo(batch, iris_resource_bo(tgt->offset.res),
                                  true);
            }
         }
      }

      if ((dirty & IRIS_DIRTY_SO_DECL_LIST) && ice->state.streamout) {
         uint32_t *decl_list =
            ice->state.streamout + GENX(3DSTATE_STREAMOUT_length);
         iris_batch_emit(batch, decl_list, 4 * ((decl_list[0] & 0xff) + 2));
      }

      if (dirty & IRIS_DIRTY_STREAMOUT) {
         const struct iris_rasterizer_state *cso_rast = ice->state.cso_rast;

         uint32_t dynamic_sol[GENX(3DSTATE_STREAMOUT_length)];
         iris_pack_command(GENX(3DSTATE_STREAMOUT), dynamic_sol, sol) {
            sol.SOFunctionEnable = true;
            sol.SOStatisticsEnable = true;

            sol.RenderingDisable = cso_rast->rasterizer_discard &&
                                   !ice->state.prims_generated_query_active;
            sol.ReorderMode = cso_rast->flatshade_first ? LEADING : TRAILING;
         }

         assert(ice->state.streamout);

         iris_emit_merge(batch, ice->state.streamout, dynamic_sol,
                         GENX(3DSTATE_STREAMOUT_length));
      }
   } else {
      if (dirty & IRIS_DIRTY_STREAMOUT) {
         iris_emit_cmd(batch, GENX(3DSTATE_STREAMOUT), sol);
      }
   }

   if (dirty & IRIS_DIRTY_CLIP) {
      struct iris_rasterizer_state *cso_rast = ice->state.cso_rast;
      struct pipe_framebuffer_state *cso_fb = &ice->state.framebuffer;

      uint32_t dynamic_clip[GENX(3DSTATE_CLIP_length)];
      iris_pack_command(GENX(3DSTATE_CLIP), &dynamic_clip, cl) {
         if (wm_prog_data->barycentric_interp_modes &
             BRW_BARYCENTRIC_NONPERSPECTIVE_BITS)
            cl.NonPerspectiveBarycentricEnable = true;

         cl.ForceZeroRTAIndexEnable = cso_fb->layers == 0;
         cl.MaximumVPIndex = ice->state.num_viewports - 1;
      }
      iris_emit_merge(batch, cso_rast->clip, dynamic_clip,
                      ARRAY_SIZE(cso_rast->clip));
   }

   if (dirty & IRIS_DIRTY_RASTER) {
      struct iris_rasterizer_state *cso = ice->state.cso_rast;
      iris_batch_emit(batch, cso->raster, sizeof(cso->raster));
      iris_batch_emit(batch, cso->sf, sizeof(cso->sf));

   }

   /* XXX: FS program updates needs to flag IRIS_DIRTY_WM */
   if (dirty & IRIS_DIRTY_WM) {
      struct iris_rasterizer_state *cso = ice->state.cso_rast;
      uint32_t dynamic_wm[GENX(3DSTATE_WM_length)];

      iris_pack_command(GENX(3DSTATE_WM), &dynamic_wm, wm) {
         wm.BarycentricInterpolationMode =
            wm_prog_data->barycentric_interp_modes;

         if (wm_prog_data->early_fragment_tests)
            wm.EarlyDepthStencilControl = EDSC_PREPS;
         else if (wm_prog_data->has_side_effects)
            wm.EarlyDepthStencilControl = EDSC_PSEXEC;
      }
      iris_emit_merge(batch, cso->wm, dynamic_wm, ARRAY_SIZE(cso->wm));
   }

   if (dirty & IRIS_DIRTY_SBE) {
      iris_emit_sbe(batch, ice);
   }

   if (dirty & IRIS_DIRTY_PS_BLEND) {
      struct iris_blend_state *cso_blend = ice->state.cso_blend;
      struct iris_depth_stencil_alpha_state *cso_zsa = ice->state.cso_zsa;
      uint32_t dynamic_pb[GENX(3DSTATE_PS_BLEND_length)];
      iris_pack_command(GENX(3DSTATE_PS_BLEND), &dynamic_pb, pb) {
         pb.HasWriteableRT = true; // XXX: comes from somewhere :(
         pb.AlphaTestEnable = cso_zsa->alpha.enabled;
      }

      iris_emit_merge(batch, cso_blend->ps_blend, dynamic_pb,
                      ARRAY_SIZE(cso_blend->ps_blend));
   }

   if (dirty & IRIS_DIRTY_WM_DEPTH_STENCIL) {
      struct iris_depth_stencil_alpha_state *cso = ice->state.cso_zsa;
      struct pipe_stencil_ref *p_stencil_refs = &ice->state.stencil_ref;

      uint32_t stencil_refs[GENX(3DSTATE_WM_DEPTH_STENCIL_length)];
      iris_pack_command(GENX(3DSTATE_WM_DEPTH_STENCIL), &stencil_refs, wmds) {
         wmds.StencilReferenceValue = p_stencil_refs->ref_value[0];
         wmds.BackfaceStencilReferenceValue = p_stencil_refs->ref_value[1];
      }
      iris_emit_merge(batch, cso->wmds, stencil_refs, ARRAY_SIZE(cso->wmds));
   }

   if (dirty & IRIS_DIRTY_SCISSOR_RECT) {
      uint32_t scissor_offset =
         emit_state(batch, ice->state.dynamic_uploader,
                    &ice->state.last_res.scissor,
                    ice->state.scissors,
                    sizeof(struct pipe_scissor_state) *
                    ice->state.num_viewports, 32);

      iris_emit_cmd(batch, GENX(3DSTATE_SCISSOR_STATE_POINTERS), ptr) {
         ptr.ScissorRectPointer = scissor_offset;
      }
   }

   if (dirty & IRIS_DIRTY_DEPTH_BUFFER) {
      struct pipe_framebuffer_state *cso_fb = &ice->state.framebuffer;
      struct iris_depth_buffer_state *cso_z = &ice->state.genx->depth_buffer;

      iris_batch_emit(batch, cso_z->packets, sizeof(cso_z->packets));

      if (cso_fb->zsbuf) {
         struct iris_resource *zres = (void *) cso_fb->zsbuf->texture;
         // XXX: depth might not be writable...
         iris_use_pinned_bo(batch, zres->bo, true);
      }
   }

   if (dirty & IRIS_DIRTY_POLYGON_STIPPLE) {
      iris_emit_cmd(batch, GENX(3DSTATE_POLY_STIPPLE_PATTERN), poly) {
         for (int i = 0; i < 32; i++) {
            poly.PatternRow[i] = ice->state.poly_stipple.stipple[i];
         }
      }
   }

   if (dirty & IRIS_DIRTY_LINE_STIPPLE) {
      struct iris_rasterizer_state *cso = ice->state.cso_rast;
      iris_batch_emit(batch, cso->line_stipple, sizeof(cso->line_stipple));
   }

   if (dirty & IRIS_DIRTY_VF_TOPOLOGY) {
      iris_emit_cmd(batch, GENX(3DSTATE_VF_TOPOLOGY), topo) {
         topo.PrimitiveTopologyType =
            translate_prim_type(draw->mode, draw->vertices_per_patch);
      }
   }

   if (dirty & IRIS_DIRTY_VERTEX_BUFFERS) {
      struct iris_vertex_buffer_state *cso = &ice->state.genx->vertex_buffers;
      const unsigned vb_dwords = GENX(VERTEX_BUFFER_STATE_length);

      if (cso->num_buffers > 0) {
         iris_batch_emit(batch, cso->vertex_buffers, sizeof(uint32_t) *
                         (1 + vb_dwords * cso->num_buffers));

         for (unsigned i = 0; i < cso->num_buffers; i++) {
            struct iris_resource *res = (void *) cso->resources[i];
            if (res)
               iris_use_pinned_bo(batch, res->bo, false);
         }
      }
   }

   if (dirty & IRIS_DIRTY_VERTEX_ELEMENTS) {
      struct iris_vertex_element_state *cso = ice->state.cso_vertex_elements;
      const unsigned entries = MAX2(cso->count, 1);
      iris_batch_emit(batch, cso->vertex_elements, sizeof(uint32_t) *
                      (1 + entries * GENX(VERTEX_ELEMENT_STATE_length)));
      iris_batch_emit(batch, cso->vf_instancing, sizeof(uint32_t) *
                      entries * GENX(3DSTATE_VF_INSTANCING_length));
   }

   if (dirty & IRIS_DIRTY_VF_SGVS) {
      const struct brw_vs_prog_data *vs_prog_data = (void *)
         ice->shaders.prog[MESA_SHADER_VERTEX]->prog_data;
      struct iris_vertex_element_state *cso = ice->state.cso_vertex_elements;

      iris_emit_cmd(batch, GENX(3DSTATE_VF_SGVS), sgv) {
         if (vs_prog_data->uses_vertexid) {
            sgv.VertexIDEnable = true;
            sgv.VertexIDComponentNumber = 2;
            sgv.VertexIDElementOffset = cso->count;
         }

         if (vs_prog_data->uses_instanceid) {
            sgv.InstanceIDEnable = true;
            sgv.InstanceIDComponentNumber = 3;
            sgv.InstanceIDElementOffset = cso->count;
         }
      }
   }

   if (dirty & IRIS_DIRTY_VF) {
      iris_emit_cmd(batch, GENX(3DSTATE_VF), vf) {
         if (draw->primitive_restart) {
            vf.IndexedDrawCutIndexEnable = true;
            vf.CutIndex = draw->restart_index;
         }
      }
   }

   // XXX: Gen8 - PMA fix
}

static void
iris_upload_render_state(struct iris_context *ice,
                         struct iris_batch *batch,
                         const struct pipe_draw_info *draw)
{
   /* Always pin the binder.  If we're emitting new binding table pointers,
    * we need it.  If not, we're probably inheriting old tables via the
    * context, and need it anyway.  Since true zero-bindings cases are
    * practically non-existent, just pin it and avoid last_res tracking.
    */
   iris_use_pinned_bo(batch, ice->state.binder.bo, false);

   iris_upload_dirty_render_state(ice, batch, draw);

   if (draw->index_size > 0) {
      unsigned offset;

      if (draw->has_user_indices) {
         u_upload_data(ice->ctx.stream_uploader, 0,
                       draw->count * draw->index_size, 4, draw->index.user,
                       &offset, &ice->state.last_res.index_buffer);
      } else {
         pipe_resource_reference(&ice->state.last_res.index_buffer,
                                 draw->index.resource);
         offset = 0;
      }

      struct iris_bo *bo = iris_resource_bo(ice->state.last_res.index_buffer);

      iris_emit_cmd(batch, GENX(3DSTATE_INDEX_BUFFER), ib) {
         ib.IndexFormat = draw->index_size >> 1;
         ib.MOCS = MOCS_WB;
         ib.BufferSize = bo->size;
         ib.BufferStartingAddress = ro_bo(bo, offset);
      }
   }

#define _3DPRIM_END_OFFSET          0x2420
#define _3DPRIM_START_VERTEX        0x2430
#define _3DPRIM_VERTEX_COUNT        0x2434
#define _3DPRIM_INSTANCE_COUNT      0x2438
#define _3DPRIM_START_INSTANCE      0x243C
#define _3DPRIM_BASE_VERTEX         0x2440

   if (draw->indirect) {
      /* We don't support this MultidrawIndirect. */
      assert(!draw->indirect->indirect_draw_count);

      struct iris_bo *bo = iris_resource_bo(draw->indirect->buffer);
      assert(bo);

      iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
         lrm.RegisterAddress = _3DPRIM_VERTEX_COUNT;
         lrm.MemoryAddress = ro_bo(bo, draw->indirect->offset + 0);
      }
      iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
         lrm.RegisterAddress = _3DPRIM_INSTANCE_COUNT;
         lrm.MemoryAddress = ro_bo(bo, draw->indirect->offset + 4);
      }
      iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
         lrm.RegisterAddress = _3DPRIM_START_VERTEX;
         lrm.MemoryAddress = ro_bo(bo, draw->indirect->offset + 8);
      }
      if (draw->index_size) {
         iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
            lrm.RegisterAddress = _3DPRIM_BASE_VERTEX;
            lrm.MemoryAddress = ro_bo(bo, draw->indirect->offset + 12);
         }
         iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
            lrm.RegisterAddress = _3DPRIM_START_INSTANCE;
            lrm.MemoryAddress = ro_bo(bo, draw->indirect->offset + 16);
         }
      } else {
         iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
            lrm.RegisterAddress = _3DPRIM_START_INSTANCE;
            lrm.MemoryAddress = ro_bo(bo, draw->indirect->offset + 12);
         }
         iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
            lri.RegisterOffset = _3DPRIM_BASE_VERTEX;
            lri.DataDWord = 0;
         }
      }
   }

   iris_emit_cmd(batch, GENX(3DPRIMITIVE), prim) {
      prim.StartInstanceLocation = draw->start_instance;
      prim.InstanceCount = draw->instance_count;
      prim.VertexCountPerInstance = draw->count;
      prim.VertexAccessType = draw->index_size > 0 ? RANDOM : SEQUENTIAL;

      // XXX: this is probably bonkers.
      prim.StartVertexLocation = draw->start;

      prim.IndirectParameterEnable = draw->indirect != NULL;

      if (draw->index_size) {
         prim.BaseVertexLocation += draw->index_bias;
      } else {
         prim.StartVertexLocation += draw->index_bias;
      }

      //prim.BaseVertexLocation = ...;
   }

   if (!batch->contains_draw) {
      iris_restore_render_saved_bos(ice, batch, draw);
      batch->contains_draw = true;
   }
}

static void
iris_upload_compute_state(struct iris_context *ice,
                          struct iris_batch *batch,
                          const struct pipe_grid_info *grid)
{
   const uint64_t dirty = ice->state.dirty;
   struct iris_screen *screen = batch->screen;
   const struct gen_device_info *devinfo = &screen->devinfo;
   struct iris_binder *binder = &ice->state.binder;
   struct iris_shader_state *shs = &ice->state.shaders[MESA_SHADER_COMPUTE];
   struct iris_compiled_shader *shader =
      ice->shaders.prog[MESA_SHADER_COMPUTE];
   struct brw_stage_prog_data *prog_data = shader->prog_data;
   struct brw_cs_prog_data *cs_prog_data = (void *) prog_data;

   // XXX: L3 configuration not set up for SLM
   assert(prog_data->total_shared == 0);

   if (dirty & IRIS_DIRTY_BINDINGS_CS)
      iris_populate_binding_table(ice, batch, MESA_SHADER_COMPUTE, false);

   iris_use_optional_res(batch, shs->sampler_table.res, false);
   iris_use_pinned_bo(batch, iris_resource_bo(shader->assembly.res), false);

   if (ice->state.need_border_colors)
      iris_use_pinned_bo(batch, ice->state.border_color_pool.bo, false);

   if (dirty & IRIS_DIRTY_CS) {
      /* The MEDIA_VFE_STATE documentation for Gen8+ says:
       *
       *   "A stalling PIPE_CONTROL is required before MEDIA_VFE_STATE unless
       *    the only bits that are changed are scoreboard related: Scoreboard
       *    Enable, Scoreboard Type, Scoreboard Mask, Scoreboard Delta.  For
       *    these scoreboard related states, a MEDIA_STATE_FLUSH is
       *    sufficient."
       */
      iris_emit_pipe_control_flush(batch, PIPE_CONTROL_CS_STALL);

      iris_emit_cmd(batch, GENX(MEDIA_VFE_STATE), vfe) {
         if (prog_data->total_scratch) {
            /* Per Thread Scratch Space is in the range [0, 11] where
             * 0 = 1k, 1 = 2k, 2 = 4k, ..., 11 = 2M.
             */
            // XXX: vfe.ScratchSpaceBasePointer
            //vfe.PerThreadScratchSpace =
               //ffs(stage_state->per_thread_scratch) - 11;
         }

         vfe.MaximumNumberofThreads =
            devinfo->max_cs_threads * screen->subslice_total - 1;
#if GEN_GEN < 11
         vfe.ResetGatewayTimer =
            Resettingrelativetimerandlatchingtheglobaltimestamp;
#endif

         vfe.NumberofURBEntries = 2;
         vfe.URBEntryAllocationSize = 2;

         // XXX: Use Indirect Payload Storage?
         vfe.CURBEAllocationSize =
            ALIGN(cs_prog_data->push.per_thread.regs * cs_prog_data->threads +
                  cs_prog_data->push.cross_thread.regs, 2);
      }
   }

   // XXX: hack iris_set_constant_buffers to upload these thread counts
   // XXX: along with regular uniforms for compute shaders, somehow.

   uint32_t curbe_data_offset = 0;
   // TODO: Move subgroup-id into uniforms ubo so we can push uniforms
   assert(cs_prog_data->push.cross_thread.dwords == 0 &&
          cs_prog_data->push.per_thread.dwords == 1 &&
          cs_prog_data->base.param[0] == BRW_PARAM_BUILTIN_SUBGROUP_ID);
   struct pipe_resource *curbe_data_res = NULL;
   uint32_t *curbe_data_map =
      stream_state(batch, ice->state.dynamic_uploader, &curbe_data_res,
                   ALIGN(cs_prog_data->push.total.size, 64), 64,
                   &curbe_data_offset);
   assert(curbe_data_map);
   memset(curbe_data_map, 0x5a, ALIGN(cs_prog_data->push.total.size, 64));
   iris_fill_cs_push_const_buffer(cs_prog_data, curbe_data_map);

   if (dirty & IRIS_DIRTY_CONSTANTS_CS) {
      iris_emit_cmd(batch, GENX(MEDIA_CURBE_LOAD), curbe) {
         curbe.CURBETotalDataLength =
            ALIGN(cs_prog_data->push.total.size, 64);
         curbe.CURBEDataStartAddress = curbe_data_offset;
      }
   }

   if (dirty & (IRIS_DIRTY_SAMPLER_STATES_CS |
                IRIS_DIRTY_BINDINGS_CS |
                IRIS_DIRTY_CONSTANTS_CS |
                IRIS_DIRTY_CS)) {
      struct pipe_resource *desc_res = NULL;
      uint32_t desc[GENX(INTERFACE_DESCRIPTOR_DATA_length)];

      iris_pack_state(GENX(INTERFACE_DESCRIPTOR_DATA), desc, idd) {
         idd.SamplerStatePointer = shs->sampler_table.offset;
         idd.BindingTablePointer = binder->bt_offset[MESA_SHADER_COMPUTE];
         idd.ConstantURBEntryReadLength = cs_prog_data->push.per_thread.regs;
         idd.CrossThreadConstantDataReadLength =
            cs_prog_data->push.cross_thread.regs;
      }

      for (int i = 0; i < GENX(INTERFACE_DESCRIPTOR_DATA_length); i++)
         desc[i] |= ((uint32_t *) shader->derived_data)[i];

      iris_emit_cmd(batch, GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD), load) {
         load.InterfaceDescriptorTotalLength =
            GENX(INTERFACE_DESCRIPTOR_DATA_length) * sizeof(uint32_t);
         load.InterfaceDescriptorDataStartAddress =
            emit_state(batch, ice->state.dynamic_uploader,
                       &desc_res, desc, sizeof(desc), 32);
      }

      pipe_resource_reference(&desc_res, NULL);
   }

   uint32_t group_size = grid->block[0] * grid->block[1] * grid->block[2];
   uint32_t remainder = group_size & (cs_prog_data->simd_size - 1);
   uint32_t right_mask;

   if (remainder > 0)
      right_mask = ~0u >> (32 - remainder);
   else
      right_mask = ~0u >> (32 - cs_prog_data->simd_size);

#define GPGPU_DISPATCHDIMX 0x2500
#define GPGPU_DISPATCHDIMY 0x2504
#define GPGPU_DISPATCHDIMZ 0x2508

   if (grid->indirect) {
      struct iris_state_ref *grid_size = &ice->state.grid_size;
      struct iris_bo *bo = iris_resource_bo(grid_size->res);
      iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
         lrm.RegisterAddress = GPGPU_DISPATCHDIMX;
         lrm.MemoryAddress = ro_bo(bo, grid_size->offset + 0);
      }
      iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
         lrm.RegisterAddress = GPGPU_DISPATCHDIMY;
         lrm.MemoryAddress = ro_bo(bo, grid_size->offset + 4);
      }
      iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
         lrm.RegisterAddress = GPGPU_DISPATCHDIMZ;
         lrm.MemoryAddress = ro_bo(bo, grid_size->offset + 8);
      }
   }

   iris_emit_cmd(batch, GENX(GPGPU_WALKER), ggw) {
      ggw.IndirectParameterEnable    = grid->indirect != NULL;
      ggw.SIMDSize                   = cs_prog_data->simd_size / 16;
      ggw.ThreadDepthCounterMaximum  = 0;
      ggw.ThreadHeightCounterMaximum = 0;
      ggw.ThreadWidthCounterMaximum  = cs_prog_data->threads - 1;
      ggw.ThreadGroupIDXDimension    = grid->grid[0];
      ggw.ThreadGroupIDYDimension    = grid->grid[1];
      ggw.ThreadGroupIDZDimension    = grid->grid[2];
      ggw.RightExecutionMask         = right_mask;
      ggw.BottomExecutionMask        = 0xffffffff;
   }

   iris_emit_cmd(batch, GENX(MEDIA_STATE_FLUSH), msf);

   if (!batch->contains_draw) {
      iris_restore_compute_saved_bos(ice, batch, grid);
      batch->contains_draw = true;
   }
}

/**
 * State module teardown.
 */
static void
iris_destroy_state(struct iris_context *ice)
{
   iris_free_vertex_buffers(&ice->state.genx->vertex_buffers);

   // XXX: unreference resources/surfaces.
   for (unsigned i = 0; i < ice->state.framebuffer.nr_cbufs; i++) {
      pipe_surface_reference(&ice->state.framebuffer.cbufs[i], NULL);
   }
   pipe_surface_reference(&ice->state.framebuffer.zsbuf, NULL);

   for (int stage = 0; stage < MESA_SHADER_STAGES; stage++) {
      struct iris_shader_state *shs = &ice->state.shaders[stage];
      pipe_resource_reference(&shs->sampler_table.res, NULL);
   }
   free(ice->state.genx);

   pipe_resource_reference(&ice->state.last_res.cc_vp, NULL);
   pipe_resource_reference(&ice->state.last_res.sf_cl_vp, NULL);
   pipe_resource_reference(&ice->state.last_res.color_calc, NULL);
   pipe_resource_reference(&ice->state.last_res.scissor, NULL);
   pipe_resource_reference(&ice->state.last_res.blend, NULL);
   pipe_resource_reference(&ice->state.last_res.index_buffer, NULL);
}

/* ------------------------------------------------------------------- */

static void
iris_load_register_imm32(struct iris_batch *batch, uint32_t reg,
                         uint32_t val)
{
   _iris_emit_lri(batch, reg, val);
}

static void
iris_load_register_imm64(struct iris_batch *batch, uint32_t reg,
                         uint64_t val)
{
   _iris_emit_lri(batch, reg + 0, val & 0xffffffff);
   _iris_emit_lri(batch, reg + 4, val >> 32);
}

/**
 * Emit MI_LOAD_REGISTER_MEM to load a 32-bit MMIO register from a buffer.
 */
static void
iris_load_register_mem32(struct iris_batch *batch, uint32_t reg,
                         struct iris_bo *bo, uint32_t offset)
{
   iris_emit_cmd(batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
      lrm.RegisterAddress = reg;
      lrm.MemoryAddress = ro_bo(bo, offset);
   }
}

/**
 * Load a 64-bit value from a buffer into a MMIO register via
 * two MI_LOAD_REGISTER_MEM commands.
 */
static void
iris_load_register_mem64(struct iris_batch *batch, uint32_t reg,
                         struct iris_bo *bo, uint32_t offset)
{
   iris_load_register_mem32(batch, reg + 0, bo, offset + 0);
   iris_load_register_mem32(batch, reg + 4, bo, offset + 4);
}

static void
iris_store_register_mem32(struct iris_batch *batch, uint32_t reg,
                          struct iris_bo *bo, uint32_t offset,
                          bool predicated)
{
   iris_emit_cmd(batch, GENX(MI_STORE_REGISTER_MEM), srm) {
      srm.RegisterAddress = reg;
      srm.MemoryAddress = rw_bo(bo, offset);
      srm.PredicateEnable = predicated;
   }
}

static void
iris_store_register_mem64(struct iris_batch *batch, uint32_t reg,
                          struct iris_bo *bo, uint32_t offset,
                          bool predicated)
{
   iris_store_register_mem32(batch, reg + 0, bo, offset + 0, predicated);
   iris_store_register_mem32(batch, reg + 4, bo, offset + 4, predicated);
}

static void
iris_store_data_imm32(struct iris_batch *batch,
                      struct iris_bo *bo, uint32_t offset,
                      uint32_t imm)
{
   iris_emit_cmd(batch, GENX(MI_STORE_DATA_IMM), sdi) {
      sdi.Address = rw_bo(bo, offset);
      sdi.ImmediateData = imm;
   }
}

static void
iris_store_data_imm64(struct iris_batch *batch,
                      struct iris_bo *bo, uint32_t offset,
                      uint64_t imm)
{
   /* Can't use iris_emit_cmd because MI_STORE_DATA_IMM has a length of
    * 2 in genxml but it's actually variable length and we need 5 DWords.
    */
   void *map = iris_get_command_space(batch, 4 * 5);
   _iris_pack_command(batch, GENX(MI_STORE_DATA_IMM), map, sdi) {
      sdi.DWordLength = 5 - 2;
      sdi.Address = rw_bo(bo, offset);
      sdi.ImmediateData = imm;
   }
}

static void
iris_copy_mem_mem(struct iris_batch *batch,
                  struct iris_bo *dst_bo, uint32_t dst_offset,
                  struct iris_bo *src_bo, uint32_t src_offset,
                  unsigned bytes)
{
   /* MI_COPY_MEM_MEM operates on DWords. */
   assert(bytes % 4 == 0);
   assert(dst_offset % 4 == 0);
   assert(src_offset % 4 == 0);

   for (unsigned i = 0; i < bytes; i += 4) {
      iris_emit_cmd(batch, GENX(MI_COPY_MEM_MEM), cp) {
         cp.DestinationMemoryAddress = rw_bo(dst_bo, dst_offset + i);
         cp.SourceMemoryAddress = ro_bo(src_bo, src_offset + i);
      }
   }
}

/* ------------------------------------------------------------------- */

static unsigned
flags_to_post_sync_op(uint32_t flags)
{
   if (flags & PIPE_CONTROL_WRITE_IMMEDIATE)
      return WriteImmediateData;

   if (flags & PIPE_CONTROL_WRITE_DEPTH_COUNT)
      return WritePSDepthCount;

   if (flags & PIPE_CONTROL_WRITE_TIMESTAMP)
      return WriteTimestamp;

   return 0;
}

/**
 * Do the given flags have a Post Sync or LRI Post Sync operation?
 */
static enum pipe_control_flags
get_post_sync_flags(enum pipe_control_flags flags)
{
   flags &= PIPE_CONTROL_WRITE_IMMEDIATE |
            PIPE_CONTROL_WRITE_DEPTH_COUNT |
            PIPE_CONTROL_WRITE_TIMESTAMP |
            PIPE_CONTROL_LRI_POST_SYNC_OP;

   /* Only one "Post Sync Op" is allowed, and it's mutually exclusive with
    * "LRI Post Sync Operation".  So more than one bit set would be illegal.
    */
   assert(util_bitcount(flags) <= 1);

   return flags;
}

// XXX: compute support
#define IS_COMPUTE_PIPELINE(batch) (batch->engine != I915_EXEC_RENDER)

/**
 * Emit a series of PIPE_CONTROL commands, taking into account any
 * workarounds necessary to actually accomplish the caller's request.
 *
 * Unless otherwise noted, spec quotations in this function come from:
 *
 * Synchronization of the 3D Pipeline > PIPE_CONTROL Command > Programming
 * Restrictions for PIPE_CONTROL.
 *
 * You should not use this function directly.  Use the helpers in
 * iris_pipe_control.c instead, which may split the pipe control further.
 */
static void
iris_emit_raw_pipe_control(struct iris_batch *batch, uint32_t flags,
                           struct iris_bo *bo, uint32_t offset, uint64_t imm)
{
   UNUSED const struct gen_device_info *devinfo = &batch->screen->devinfo;
   enum pipe_control_flags post_sync_flags = get_post_sync_flags(flags);
   enum pipe_control_flags non_lri_post_sync_flags =
      post_sync_flags & ~PIPE_CONTROL_LRI_POST_SYNC_OP;

   /* Recursive PIPE_CONTROL workarounds --------------------------------
    * (http://knowyourmeme.com/memes/xzibit-yo-dawg)
    *
    * We do these first because we want to look at the original operation,
    * rather than any workarounds we set.
    */
   if (GEN_GEN == 9 && (flags & PIPE_CONTROL_VF_CACHE_INVALIDATE)) {
      /* The PIPE_CONTROL "VF Cache Invalidation Enable" bit description
       * lists several workarounds:
       *
       *    "Project: SKL, KBL, BXT
       *
       *     If the VF Cache Invalidation Enable is set to a 1 in a
       *     PIPE_CONTROL, a separate Null PIPE_CONTROL, all bitfields
       *     sets to 0, with the VF Cache Invalidation Enable set to 0
       *     needs to be sent prior to the PIPE_CONTROL with VF Cache
       *     Invalidation Enable set to a 1."
       */
      iris_emit_raw_pipe_control(batch, 0, NULL, 0, 0);
   }

   if (GEN_GEN == 9 && IS_COMPUTE_PIPELINE(batch) && post_sync_flags) {
      /* Project: SKL / Argument: LRI Post Sync Operation [23]
       *
       * "PIPECONTROL command with “Command Streamer Stall Enable” must be
       *  programmed prior to programming a PIPECONTROL command with "LRI
       *  Post Sync Operation" in GPGPU mode of operation (i.e when
       *  PIPELINE_SELECT command is set to GPGPU mode of operation)."
       *
       * The same text exists a few rows below for Post Sync Op.
       */
      iris_emit_raw_pipe_control(batch, PIPE_CONTROL_CS_STALL, bo, offset, imm);
   }

   if (GEN_GEN == 10 && (flags & PIPE_CONTROL_RENDER_TARGET_FLUSH)) {
      /* Cannonlake:
       * "Before sending a PIPE_CONTROL command with bit 12 set, SW must issue
       *  another PIPE_CONTROL with Render Target Cache Flush Enable (bit 12)
       *  = 0 and Pipe Control Flush Enable (bit 7) = 1"
       */
      iris_emit_raw_pipe_control(batch, PIPE_CONTROL_FLUSH_ENABLE, bo,
                                 offset, imm);
   }

   /* "Flush Types" workarounds ---------------------------------------------
    * We do these now because they may add post-sync operations or CS stalls.
    */

   if (GEN_GEN < 11 && flags & PIPE_CONTROL_VF_CACHE_INVALIDATE) {
      /* Project: BDW, SKL+ (stopping at CNL) / Argument: VF Invalidate
       *
       * "'Post Sync Operation' must be enabled to 'Write Immediate Data' or
       *  'Write PS Depth Count' or 'Write Timestamp'."
       */
      if (!bo) {
         flags |= PIPE_CONTROL_WRITE_IMMEDIATE;
         post_sync_flags |= PIPE_CONTROL_WRITE_IMMEDIATE;
         non_lri_post_sync_flags |= PIPE_CONTROL_WRITE_IMMEDIATE;
         bo = batch->screen->workaround_bo;
      }
   }

   /* #1130 from Gen10 workarounds page:
    *
    *    "Enable Depth Stall on every Post Sync Op if Render target Cache
    *     Flush is not enabled in same PIPE CONTROL and Enable Pixel score
    *     board stall if Render target cache flush is enabled."
    *
    * Applicable to CNL B0 and C0 steppings only.
    *
    * The wording here is unclear, and this workaround doesn't look anything
    * like the internal bug report recommendations, but leave it be for now...
    */
   if (GEN_GEN == 10) {
      if (flags & PIPE_CONTROL_RENDER_TARGET_FLUSH) {
         flags |= PIPE_CONTROL_STALL_AT_SCOREBOARD;
      } else if (flags & non_lri_post_sync_flags) {
         flags |= PIPE_CONTROL_DEPTH_STALL;
      }
   }

   if (flags & PIPE_CONTROL_DEPTH_STALL) {
      /* From the PIPE_CONTROL instruction table, bit 13 (Depth Stall Enable):
       *
       *    "This bit must be DISABLED for operations other than writing
       *     PS_DEPTH_COUNT."
       *
       * This seems like nonsense.  An Ivybridge workaround requires us to
       * emit a PIPE_CONTROL with a depth stall and write immediate post-sync
       * operation.  Gen8+ requires us to emit depth stalls and depth cache
       * flushes together.  So, it's hard to imagine this means anything other
       * than "we originally intended this to be used for PS_DEPTH_COUNT".
       *
       * We ignore the supposed restriction and do nothing.
       */
   }

   if (flags & (PIPE_CONTROL_RENDER_TARGET_FLUSH |
                PIPE_CONTROL_STALL_AT_SCOREBOARD)) {
      /* From the PIPE_CONTROL instruction table, bit 12 and bit 1:
       *
       *    "This bit must be DISABLED for End-of-pipe (Read) fences,
       *     PS_DEPTH_COUNT or TIMESTAMP queries."
       *
       * TODO: Implement end-of-pipe checking.
       */
      assert(!(post_sync_flags & (PIPE_CONTROL_WRITE_DEPTH_COUNT |
                                  PIPE_CONTROL_WRITE_TIMESTAMP)));
   }

   if (GEN_GEN < 11 && (flags & PIPE_CONTROL_STALL_AT_SCOREBOARD)) {
      /* From the PIPE_CONTROL instruction table, bit 1:
       *
       *    "This bit is ignored if Depth Stall Enable is set.
       *     Further, the render cache is not flushed even if Write Cache
       *     Flush Enable bit is set."
       *
       * We assert that the caller doesn't do this combination, to try and
       * prevent mistakes.  It shouldn't hurt the GPU, though.
       *
       * We skip this check on Gen11+ as the "Stall at Pixel Scoreboard"
       * and "Render Target Flush" combo is explicitly required for BTI
       * update workarounds.
       */
      assert(!(flags & (PIPE_CONTROL_DEPTH_STALL |
                        PIPE_CONTROL_RENDER_TARGET_FLUSH)));
   }

   /* PIPE_CONTROL page workarounds ------------------------------------- */

   if (GEN_GEN <= 8 && (flags & PIPE_CONTROL_STATE_CACHE_INVALIDATE)) {
      /* From the PIPE_CONTROL page itself:
       *
       *    "IVB, HSW, BDW
       *     Restriction: Pipe_control with CS-stall bit set must be issued
       *     before a pipe-control command that has the State Cache
       *     Invalidate bit set."
       */
      flags |= PIPE_CONTROL_CS_STALL;
   }

   if (flags & PIPE_CONTROL_FLUSH_LLC) {
      /* From the PIPE_CONTROL instruction table, bit 26 (Flush LLC):
       *
       *    "Project: ALL
       *     SW must always program Post-Sync Operation to "Write Immediate
       *     Data" when Flush LLC is set."
       *
       * For now, we just require the caller to do it.
       */
      assert(flags & PIPE_CONTROL_WRITE_IMMEDIATE);
   }

   /* "Post-Sync Operation" workarounds -------------------------------- */

   /* Project: All / Argument: Global Snapshot Count Reset [19]
    *
    * "This bit must not be exercised on any product.
    *  Requires stall bit ([20] of DW1) set."
    *
    * We don't use this, so we just assert that it isn't used.  The
    * PIPE_CONTROL instruction page indicates that they intended this
    * as a debug feature and don't think it is useful in production,
    * but it may actually be usable, should we ever want to.
    */
   assert((flags & PIPE_CONTROL_GLOBAL_SNAPSHOT_COUNT_RESET) == 0);

   if (flags & (PIPE_CONTROL_MEDIA_STATE_CLEAR |
                PIPE_CONTROL_INDIRECT_STATE_POINTERS_DISABLE)) {
      /* Project: All / Arguments:
       *
       * - Generic Media State Clear [16]
       * - Indirect State Pointers Disable [16]
       *
       *    "Requires stall bit ([20] of DW1) set."
       *
       * Also, the PIPE_CONTROL instruction table, bit 16 (Generic Media
       * State Clear) says:
       *
       *    "PIPECONTROL command with “Command Streamer Stall Enable” must be
       *     programmed prior to programming a PIPECONTROL command with "Media
       *     State Clear" set in GPGPU mode of operation"
       *
       * This is a subset of the earlier rule, so there's nothing to do.
       */
      flags |= PIPE_CONTROL_CS_STALL;
   }

   if (flags & PIPE_CONTROL_STORE_DATA_INDEX) {
      /* Project: All / Argument: Store Data Index
       *
       * "Post-Sync Operation ([15:14] of DW1) must be set to something other
       *  than '0'."
       *
       * For now, we just assert that the caller does this.  We might want to
       * automatically add a write to the workaround BO...
       */
      assert(non_lri_post_sync_flags != 0);
   }

   if (flags & PIPE_CONTROL_SYNC_GFDT) {
      /* Project: All / Argument: Sync GFDT
       *
       * "Post-Sync Operation ([15:14] of DW1) must be set to something other
       *  than '0' or 0x2520[13] must be set."
       *
       * For now, we just assert that the caller does this.
       */
      assert(non_lri_post_sync_flags != 0);
   }

   if (flags & PIPE_CONTROL_TLB_INVALIDATE) {
      /* Project: IVB+ / Argument: TLB inv
       *
       *    "Requires stall bit ([20] of DW1) set."
       *
       * Also, from the PIPE_CONTROL instruction table:
       *
       *    "Project: SKL+
       *     Post Sync Operation or CS stall must be set to ensure a TLB
       *     invalidation occurs.  Otherwise no cycle will occur to the TLB
       *     cache to invalidate."
       *
       * This is not a subset of the earlier rule, so there's nothing to do.
       */
      flags |= PIPE_CONTROL_CS_STALL;
   }

   if (GEN_GEN == 9 && devinfo->gt == 4) {
      /* TODO: The big Skylake GT4 post sync op workaround */
   }

   /* "GPGPU specific workarounds" (both post-sync and flush) ------------ */

   if (IS_COMPUTE_PIPELINE(batch)) {
      if (GEN_GEN >= 9 && (flags & PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE)) {
         /* Project: SKL+ / Argument: Tex Invalidate
          * "Requires stall bit ([20] of DW) set for all GPGPU Workloads."
          */
         flags |= PIPE_CONTROL_CS_STALL;
      }

      if (GEN_GEN == 8 && (post_sync_flags ||
                           (flags & (PIPE_CONTROL_NOTIFY_ENABLE |
                                     PIPE_CONTROL_DEPTH_STALL |
                                     PIPE_CONTROL_RENDER_TARGET_FLUSH |
                                     PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                                     PIPE_CONTROL_DATA_CACHE_FLUSH)))) {
         /* Project: BDW / Arguments:
          *
          * - LRI Post Sync Operation   [23]
          * - Post Sync Op              [15:14]
          * - Notify En                 [8]
          * - Depth Stall               [13]
          * - Render Target Cache Flush [12]
          * - Depth Cache Flush         [0]
          * - DC Flush Enable           [5]
          *
          *    "Requires stall bit ([20] of DW) set for all GPGPU and Media
          *     Workloads."
          */
         flags |= PIPE_CONTROL_CS_STALL;

         /* Also, from the PIPE_CONTROL instruction table, bit 20:
          *
          *    "Project: BDW
          *     This bit must be always set when PIPE_CONTROL command is
          *     programmed by GPGPU and MEDIA workloads, except for the cases
          *     when only Read Only Cache Invalidation bits are set (State
          *     Cache Invalidation Enable, Instruction cache Invalidation
          *     Enable, Texture Cache Invalidation Enable, Constant Cache
          *     Invalidation Enable). This is to WA FFDOP CG issue, this WA
          *     need not implemented when FF_DOP_CG is disable via "Fixed
          *     Function DOP Clock Gate Disable" bit in RC_PSMI_CTRL register."
          *
          * It sounds like we could avoid CS stalls in some cases, but we
          * don't currently bother.  This list isn't exactly the list above,
          * either...
          */
      }
   }

   /* "Stall" workarounds ----------------------------------------------
    * These have to come after the earlier ones because we may have added
    * some additional CS stalls above.
    */

   if (GEN_GEN < 9 && (flags & PIPE_CONTROL_CS_STALL)) {
      /* Project: PRE-SKL, VLV, CHV
       *
       * "[All Stepping][All SKUs]:
       *
       *  One of the following must also be set:
       *
       *  - Render Target Cache Flush Enable ([12] of DW1)
       *  - Depth Cache Flush Enable ([0] of DW1)
       *  - Stall at Pixel Scoreboard ([1] of DW1)
       *  - Depth Stall ([13] of DW1)
       *  - Post-Sync Operation ([13] of DW1)
       *  - DC Flush Enable ([5] of DW1)"
       *
       * If we don't already have one of those bits set, we choose to add
       * "Stall at Pixel Scoreboard".  Some of the other bits require a
       * CS stall as a workaround (see above), which would send us into
       * an infinite recursion of PIPE_CONTROLs.  "Stall at Pixel Scoreboard"
       * appears to be safe, so we choose that.
       */
      const uint32_t wa_bits = PIPE_CONTROL_RENDER_TARGET_FLUSH |
                               PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                               PIPE_CONTROL_WRITE_IMMEDIATE |
                               PIPE_CONTROL_WRITE_DEPTH_COUNT |
                               PIPE_CONTROL_WRITE_TIMESTAMP |
                               PIPE_CONTROL_STALL_AT_SCOREBOARD |
                               PIPE_CONTROL_DEPTH_STALL |
                               PIPE_CONTROL_DATA_CACHE_FLUSH;
      if (!(flags & wa_bits))
         flags |= PIPE_CONTROL_STALL_AT_SCOREBOARD;
   }

   /* Emit --------------------------------------------------------------- */

   iris_emit_cmd(batch, GENX(PIPE_CONTROL), pc) {
      pc.LRIPostSyncOperation = NoLRIOperation;
      pc.PipeControlFlushEnable = flags & PIPE_CONTROL_FLUSH_ENABLE;
      pc.DCFlushEnable = flags & PIPE_CONTROL_DATA_CACHE_FLUSH;
      pc.StoreDataIndex = 0;
      pc.CommandStreamerStallEnable = flags & PIPE_CONTROL_CS_STALL;
      pc.GlobalSnapshotCountReset =
         flags & PIPE_CONTROL_GLOBAL_SNAPSHOT_COUNT_RESET;
      pc.TLBInvalidate = flags & PIPE_CONTROL_TLB_INVALIDATE;
      pc.GenericMediaStateClear = flags & PIPE_CONTROL_MEDIA_STATE_CLEAR;
      pc.StallAtPixelScoreboard = flags & PIPE_CONTROL_STALL_AT_SCOREBOARD;
      pc.RenderTargetCacheFlushEnable =
         flags & PIPE_CONTROL_RENDER_TARGET_FLUSH;
      pc.DepthCacheFlushEnable = flags & PIPE_CONTROL_DEPTH_CACHE_FLUSH;
      pc.StateCacheInvalidationEnable =
         flags & PIPE_CONTROL_STATE_CACHE_INVALIDATE;
      pc.VFCacheInvalidationEnable = flags & PIPE_CONTROL_VF_CACHE_INVALIDATE;
      pc.ConstantCacheInvalidationEnable =
         flags & PIPE_CONTROL_CONST_CACHE_INVALIDATE;
      pc.PostSyncOperation = flags_to_post_sync_op(flags);
      pc.DepthStallEnable = flags & PIPE_CONTROL_DEPTH_STALL;
      pc.InstructionCacheInvalidateEnable =
         flags & PIPE_CONTROL_INSTRUCTION_INVALIDATE;
      pc.NotifyEnable = flags & PIPE_CONTROL_NOTIFY_ENABLE;
      pc.IndirectStatePointersDisable =
         flags & PIPE_CONTROL_INDIRECT_STATE_POINTERS_DISABLE;
      pc.TextureCacheInvalidationEnable =
         flags & PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
      pc.Address = rw_bo(bo, offset);
      pc.ImmediateData = imm;
   }
}

void
genX(init_state)(struct iris_context *ice)
{
   struct pipe_context *ctx = &ice->ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;

   ctx->create_blend_state = iris_create_blend_state;
   ctx->create_depth_stencil_alpha_state = iris_create_zsa_state;
   ctx->create_rasterizer_state = iris_create_rasterizer_state;
   ctx->create_sampler_state = iris_create_sampler_state;
   ctx->create_sampler_view = iris_create_sampler_view;
   ctx->create_surface = iris_create_surface;
   ctx->create_vertex_elements_state = iris_create_vertex_elements;
   ctx->bind_blend_state = iris_bind_blend_state;
   ctx->bind_depth_stencil_alpha_state = iris_bind_zsa_state;
   ctx->bind_sampler_states = iris_bind_sampler_states;
   ctx->bind_rasterizer_state = iris_bind_rasterizer_state;
   ctx->bind_vertex_elements_state = iris_bind_vertex_elements_state;
   ctx->delete_blend_state = iris_delete_state;
   ctx->delete_depth_stencil_alpha_state = iris_delete_state;
   ctx->delete_fs_state = iris_delete_state;
   ctx->delete_rasterizer_state = iris_delete_state;
   ctx->delete_sampler_state = iris_delete_state;
   ctx->delete_vertex_elements_state = iris_delete_state;
   ctx->delete_tcs_state = iris_delete_state;
   ctx->delete_tes_state = iris_delete_state;
   ctx->delete_gs_state = iris_delete_state;
   ctx->delete_vs_state = iris_delete_state;
   ctx->set_blend_color = iris_set_blend_color;
   ctx->set_clip_state = iris_set_clip_state;
   ctx->set_constant_buffer = iris_set_constant_buffer;
   ctx->set_shader_buffers = iris_set_shader_buffers;
   ctx->set_shader_images = iris_set_shader_images;
   ctx->set_sampler_views = iris_set_sampler_views;
   ctx->set_tess_state = iris_set_tess_state;
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

   ice->vtbl.destroy_state = iris_destroy_state;
   ice->vtbl.init_render_context = iris_init_render_context;
   ice->vtbl.init_compute_context = iris_init_compute_context;
   ice->vtbl.upload_render_state = iris_upload_render_state;
   ice->vtbl.update_surface_base_address = iris_update_surface_base_address;
   ice->vtbl.upload_compute_state = iris_upload_compute_state;
   ice->vtbl.emit_raw_pipe_control = iris_emit_raw_pipe_control;
   ice->vtbl.load_register_imm32 = iris_load_register_imm32;
   ice->vtbl.load_register_imm64 = iris_load_register_imm64;
   ice->vtbl.load_register_mem32 = iris_load_register_mem32;
   ice->vtbl.load_register_mem64 = iris_load_register_mem64;
   ice->vtbl.store_register_mem32 = iris_store_register_mem32;
   ice->vtbl.store_register_mem64 = iris_store_register_mem64;
   ice->vtbl.store_data_imm32 = iris_store_data_imm32;
   ice->vtbl.store_data_imm64 = iris_store_data_imm64;
   ice->vtbl.copy_mem_mem = iris_copy_mem_mem;
   ice->vtbl.derived_program_state_size = iris_derived_program_state_size;
   ice->vtbl.store_derived_program_state = iris_store_derived_program_state;
   ice->vtbl.create_so_decl_list = iris_create_so_decl_list;
   ice->vtbl.populate_vs_key = iris_populate_vs_key;
   ice->vtbl.populate_tcs_key = iris_populate_tcs_key;
   ice->vtbl.populate_tes_key = iris_populate_tes_key;
   ice->vtbl.populate_gs_key = iris_populate_gs_key;
   ice->vtbl.populate_fs_key = iris_populate_fs_key;
   ice->vtbl.populate_cs_key = iris_populate_cs_key;

   ice->state.dirty = ~0ull;

   ice->state.sample_mask = 0xffff;
   ice->state.num_viewports = 1;
   ice->state.genx = calloc(1, sizeof(struct iris_genx_state));

   /* Make a 1x1x1 null surface for unbound textures */
   void *null_surf_map =
      upload_state(ice->state.surface_uploader, &ice->state.unbound_tex,
                   4 * GENX(RENDER_SURFACE_STATE_length), 64);
   isl_null_fill_state(&screen->isl_dev, null_surf_map, isl_extent3d(1, 1, 1));
   ice->state.unbound_tex.offset +=
      iris_bo_offset_from_base_address(iris_resource_bo(ice->state.unbound_tex.res));

   /* Default all scissor rectangles to be empty regions. */
   for (int i = 0; i < IRIS_MAX_VIEWPORTS; i++) {
      ice->state.scissors[i] = (struct pipe_scissor_state) {
         .minx = 1, .maxx = 0, .miny = 1, .maxy = 0,
      };
   }
}

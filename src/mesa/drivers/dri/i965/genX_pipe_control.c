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
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "genX_boilerplate.h"
#include "brw_defines.h"
#include "brw_state.h"

/**
 * According to the latest documentation, any PIPE_CONTROL with the
 * "Command Streamer Stall" bit set must also have another bit set,
 * with five different options:
 *
 *  - Render Target Cache Flush
 *  - Depth Cache Flush
 *  - Stall at Pixel Scoreboard
 *  - Post-Sync Operation
 *  - Depth Stall
 *  - DC Flush Enable
 *
 * I chose "Stall at Pixel Scoreboard" since we've used it effectively
 * in the past, but the choice is fairly arbitrary.
 */
static void
gen8_add_cs_stall_workaround_bits(uint32_t *flags)
{
   uint32_t wa_bits = PIPE_CONTROL_RENDER_TARGET_FLUSH |
                      PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                      PIPE_CONTROL_WRITE_IMMEDIATE |
                      PIPE_CONTROL_WRITE_DEPTH_COUNT |
                      PIPE_CONTROL_WRITE_TIMESTAMP |
                      PIPE_CONTROL_STALL_AT_SCOREBOARD |
                      PIPE_CONTROL_DEPTH_STALL |
                      PIPE_CONTROL_DATA_CACHE_FLUSH;

   /* If we're doing a CS stall, and don't already have one of the
    * workaround bits set, add "Stall at Pixel Scoreboard."
    */
   if ((*flags & PIPE_CONTROL_CS_STALL) != 0 && (*flags & wa_bits) == 0)
      *flags |= PIPE_CONTROL_STALL_AT_SCOREBOARD;
}

/* Implement the WaCsStallAtEveryFourthPipecontrol workaround on IVB, BYT:
 *
 * "Every 4th PIPE_CONTROL command, not counting the PIPE_CONTROL with
 *  only read-cache-invalidate bit(s) set, must have a CS_STALL bit set."
 *
 * Note that the kernel does CS stalls between batches, so we only need
 * to count them within a batch.
 */
static uint32_t
gen7_cs_stall_every_four_pipe_controls(struct brw_context *brw, uint32_t flags)
{
   if (GEN_GEN == 7 && !GEN_IS_HASWELL) {
      if (flags & PIPE_CONTROL_CS_STALL) {
         /* If we're doing a CS stall, reset the counter and carry on. */
         brw->pipe_controls_since_last_cs_stall = 0;
         return 0;
      }

      /* If this is the fourth pipe control without a CS stall, do one now. */
      if (++brw->pipe_controls_since_last_cs_stall == 4) {
         brw->pipe_controls_since_last_cs_stall = 0;
         return PIPE_CONTROL_CS_STALL;
      }
   }
   return 0;
}

/* #1130 from gen10 workarounds page in h/w specs:
 * "Enable Depth Stall on every Post Sync Op if Render target Cache Flush is
 *  not enabled in same PIPE CONTROL and Enable Pixel score board stall if
 *  Render target cache flush is enabled."
 *
 * Applicable to CNL B0 and C0 steppings only.
 */
static void
gen10_add_rcpfe_workaround_bits(uint32_t *flags)
{
   if (*flags & PIPE_CONTROL_RENDER_TARGET_FLUSH) {
      *flags = *flags | PIPE_CONTROL_STALL_AT_SCOREBOARD;
   } else if (*flags &
             (PIPE_CONTROL_WRITE_IMMEDIATE |
              PIPE_CONTROL_WRITE_DEPTH_COUNT |
              PIPE_CONTROL_WRITE_TIMESTAMP)) {
      *flags = *flags | PIPE_CONTROL_DEPTH_STALL;
   }
}

static unsigned
flags_to_post_sync_op(uint32_t flags)
{
   flags &= PIPE_CONTROL_WRITE_IMMEDIATE |
            PIPE_CONTROL_WRITE_DEPTH_COUNT |
            PIPE_CONTROL_WRITE_TIMESTAMP;

   assert(util_bitcount(flags) <= 1);

   if (flags & PIPE_CONTROL_WRITE_IMMEDIATE)
      return WriteImmediateData;

   if (flags & PIPE_CONTROL_WRITE_DEPTH_COUNT)
      return WritePSDepthCount;

   if (flags & PIPE_CONTROL_WRITE_TIMESTAMP)
      return WriteTimestamp;

   return 0;
}

void
genX(emit_raw_pipe_control)(struct brw_context *brw, uint32_t flags,
                            struct brw_bo *bo, uint32_t offset, uint64_t imm)
{
   if (GEN_GEN >= 8) {
      if (GEN_GEN == 8)
         gen8_add_cs_stall_workaround_bits(&flags);

      if (flags & PIPE_CONTROL_VF_CACHE_INVALIDATE) {
         if (GEN_GEN == 9) {
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
            brw_emit_pipe_control_flush(brw, 0);
         }

         if (GEN_GEN >= 9) {
            /* THE PIPE_CONTROL "VF Cache Invalidation Enable" docs continue:
             *
             *    "Project: BDW+
             *
             *     When VF Cache Invalidate is set “Post Sync Operation” must
             *     be enabled to “Write Immediate Data” or “Write PS Depth
             *     Count” or “Write Timestamp”."
             *
             * If there's a BO, we're already doing some kind of write.
             * If not, add a write to the workaround BO.
             *
             * XXX: This causes GPU hangs on Broadwell, so restrict it to
             *      Gen9+ for now...see this bug for more information:
             *      https://bugs.freedesktop.org/show_bug.cgi?id=103787
             */
            if (!bo) {
               flags |= PIPE_CONTROL_WRITE_IMMEDIATE;
               bo = brw->workaround_bo;
            }
         }
      }

      if (GEN_GEN == 10)
         gen10_add_rcpfe_workaround_bits(&flags);
   } else if (GEN_GEN >= 6) {
      if (GEN_GEN == 6 &&
          (flags & PIPE_CONTROL_RENDER_TARGET_FLUSH)) {
         /* Hardware workaround: SNB B-Spec says:
          *
          *   [Dev-SNB{W/A}]: Before a PIPE_CONTROL with Write Cache Flush
          *   Enable = 1, a PIPE_CONTROL with any non-zero post-sync-op is
          *   required.
          */
         brw_emit_post_sync_nonzero_flush(brw);
      }

      flags |= gen7_cs_stall_every_four_pipe_controls(brw, flags);
   }

   brw_batch_emit(brw, GENX(PIPE_CONTROL), pc) {
   #if GEN_GEN >= 9
      pc.FlushLLC = 0;
   #endif
   #if GEN_GEN >= 7
      pc.LRIPostSyncOperation = NoLRIOperation;
      pc.PipeControlFlushEnable = flags & PIPE_CONTROL_FLUSH_ENABLE;
      pc.DCFlushEnable = flags & PIPE_CONTROL_DATA_CACHE_FLUSH;
   #endif
   #if GEN_GEN >= 6
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
   #else
      pc.WriteCacheFlush = flags & PIPE_CONTROL_RENDER_TARGET_FLUSH;
   #endif
      pc.PostSyncOperation = flags_to_post_sync_op(flags);
      pc.DepthStallEnable = flags & PIPE_CONTROL_DEPTH_STALL;
      pc.InstructionCacheInvalidateEnable =
         flags & PIPE_CONTROL_INSTRUCTION_INVALIDATE;
      pc.NotifyEnable = flags & PIPE_CONTROL_NOTIFY_ENABLE;
   #if GEN_GEN >= 5 || GEN_IS_G4X
      pc.IndirectStatePointersDisable =
         flags & PIPE_CONTROL_INDIRECT_STATE_POINTERS_DISABLE;
   #endif
   #if GEN_GEN >= 6
      pc.TextureCacheInvalidationEnable =
         flags & PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
   #elif GEN_GEN == 5 || GEN_IS_G4X
      pc.TextureCacheFlushEnable =
         flags & PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
   #endif
      pc.Address = ggtt_bo(bo, offset);
      if (GEN_GEN < 7 && bo)
         pc.DestinationAddressType = DAT_GGTT;
      pc.ImmediateData = imm;
   }
}

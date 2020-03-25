/*
 * Copyright © 2020 Raspberry Pi
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

#include "v3dv_private.h"

#include "broadcom/cle/v3dx_pack.h"
#include "vk_format_info.h"

static void
emit_color_clear_rect(struct v3dv_cmd_buffer *cmd_buffer,
                      uint32_t rt,
                      VkClearColorValue clear_color,
                      VkClearRect rect)
{
   assert(!"Not implemented.");
}

static void
emit_ds_clear_rect(struct v3dv_cmd_buffer *cmd_buffer,
                   VkClearDepthStencilValue clear_ds,
                   VkClearRect rect)
{
   assert(!"Not implemented.");
}

static void
emit_tlb_clear_store(struct v3dv_cmd_buffer *cmd_buffer,
                     struct v3dv_cl *cl,
                     uint32_t attachment_idx,
                     uint32_t layer,
                     uint32_t buffer)
{
   const struct v3dv_image_view *iview =
      cmd_buffer->state.framebuffer->attachments[attachment_idx];
   const struct v3dv_image *image = iview->image;
   const struct v3d_resource_slice *slice = &image->slices[iview->base_level];
   uint32_t layer_offset = v3dv_layer_offset(image,
                                             iview->base_level,
                                             iview->first_layer + layer);

   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = buffer;
      store.address = v3dv_cl_address(image->mem->bo, layer_offset);
      store.clear_buffer_being_stored = false;

      store.output_image_format = iview->format->rt_type;
      store.r_b_swap = iview->swap_rb;
      store.memory_format = slice->tiling;

      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         store.height_in_ub_or_stride =
            slice->padded_height_of_output_image_in_uif_blocks;
      } else if (slice->tiling == VC5_TILING_RASTER) {
         store.height_in_ub_or_stride = slice->stride;
      }

      if (image->samples > VK_SAMPLE_COUNT_1_BIT)
         store.decimate_mode = V3D_DECIMATE_MODE_ALL_SAMPLES;
      else
         store.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }
}

static void
emit_tlb_clear_stores(struct v3dv_cmd_buffer *cmd_buffer,
                      struct v3dv_cl *cl,
                      uint32_t attachment_count,
                      const VkClearAttachment *attachments,
                      uint32_t layer)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   bool has_stores = false;
   for (uint32_t i = 0; i < attachment_count; i++) {
      uint32_t attachment_idx;
      uint32_t buffer;
      if (attachments[i].aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                       VK_IMAGE_ASPECT_STENCIL_BIT)) {
         attachment_idx = subpass->ds_attachment.attachment;
         buffer = v3dv_zs_buffer_from_aspect_bits(attachments[i].aspectMask);
      } else {
         uint32_t rt_idx = attachments[i].colorAttachment;
         attachment_idx = subpass->color_attachments[rt_idx].attachment;
         buffer = RENDER_TARGET_0 + rt_idx;
      }

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      has_stores = true;
      emit_tlb_clear_store(cmd_buffer, cl, attachment_idx, layer, buffer);
   }

   if (!has_stores) {
      cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
   }
}

static void
emit_tlb_clear_per_tile_rcl(struct v3dv_cmd_buffer *cmd_buffer,
                            uint32_t attachment_count,
                            const VkClearAttachment *attachments,
                            uint32_t layer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   cl_emit(cl, END_OF_LOADS, end); /* Nothing to load */

   cl_emit(cl, PRIM_LIST_FORMAT, fmt) {
      fmt.primitive_type = LIST_TRIANGLES;
   }

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   emit_tlb_clear_stores(cmd_buffer, cl, attachment_count, attachments, layer);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_tlb_clear_layer_rcl(struct v3dv_cmd_buffer *cmd_buffer,
                         uint32_t attachment_count,
                         const VkClearAttachment *attachments,
                         uint32_t layer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;

   struct v3dv_job *job = cmd_buffer->state.job;
   struct v3dv_cl *rcl = &job->rcl;

   const struct v3dv_frame_tiling *tiling = &job->frame_tiling;

   const uint32_t tile_alloc_offset =
      64 * layer * tiling->draw_tiles_x * tiling->draw_tiles_y;
   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(job->tile_alloc, tile_alloc_offset);
   }

   cl_emit(rcl, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
      config.number_of_bin_tile_lists = 1;
      config.total_frame_width_in_tiles = tiling->draw_tiles_x;
      config.total_frame_height_in_tiles = tiling->draw_tiles_y;

      config.supertile_width_in_tiles = tiling->supertile_width;
      config.supertile_height_in_tiles = tiling->supertile_height;

      config.total_frame_width_in_supertiles =
         tiling->frame_width_in_supertiles;
      config.total_frame_height_in_supertiles =
         tiling->frame_height_in_supertiles;
   }

   /* Emit the clear and also the workaround for GFXH-1742 */
   for (int i = 0; i < 2; i++) {
      cl_emit(rcl, TILE_COORDINATES, coords);
      cl_emit(rcl, END_OF_LOADS, end);
      cl_emit(rcl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
      if (i == 0) {
         cl_emit(rcl, CLEAR_TILE_BUFFERS, clear) {
            clear.clear_z_stencil_buffer = true;
            clear.clear_all_render_targets = true;
         }
      }
      cl_emit(rcl, END_OF_TILE_MARKER, end);
   }

   cl_emit(rcl, FLUSH_VCD_CACHE, flush);

   emit_tlb_clear_per_tile_rcl(cmd_buffer, attachment_count, attachments, layer);

   uint32_t supertile_w_in_pixels =
      tiling->tile_width * tiling->supertile_width;
   uint32_t supertile_h_in_pixels =
      tiling->tile_height * tiling->supertile_height;

   const uint32_t max_render_x = framebuffer->width - 1;
   const uint32_t max_render_y = framebuffer->height - 1;
   const uint32_t max_x_supertile = max_render_x / supertile_w_in_pixels;
   const uint32_t max_y_supertile = max_render_y / supertile_h_in_pixels;

   for (int y = 0; y <= max_y_supertile; y++) {
      for (int x = 0; x <= max_x_supertile; x++) {
         cl_emit(rcl, SUPERTILE_COORDINATES, coords) {
            coords.column_number_in_supertiles = x;
            coords.row_number_in_supertiles = y;
         }
      }
   }
}

static void
emit_tlb_clear_job(struct v3dv_cmd_buffer *cmd_buffer,
                   uint32_t attachment_count,
                   const VkClearAttachment *attachments,
                   uint32_t base_layer,
                   uint32_t layer_count)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* Check how many color attachments we have and also if we have a
    * depth/stencil attachment.
    */
   uint32_t color_attachment_count = 0;
   VkClearAttachment color_attachments[4];
   const VkClearDepthStencilValue *ds_clear_value = NULL;
   for (uint32_t i = 0; i < attachment_count; i++) {
      if (attachments[i].aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                       VK_IMAGE_ASPECT_STENCIL_BIT)) {
         ds_clear_value = &attachments[i].clearValue.depthStencil;
      } else if (attachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         color_attachments[color_attachment_count++] = attachments[i];
      }
   }

   const uint8_t internal_bpp =
      v3dv_framebuffer_compute_internal_bpp(framebuffer, subpass);

   v3dv_job_start_frame(job,
                        framebuffer->width,
                        framebuffer->height,
                        framebuffer->layers,
                        color_attachment_count,
                        internal_bpp);

   struct v3dv_cl *rcl = &job->rcl;
   v3dv_cl_ensure_space_with_branch(rcl, 200 +
                                    layer_count * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   const struct v3dv_frame_tiling *tiling = &job->frame_tiling;
   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.early_z_disable = true;
      config.image_width_pixels = framebuffer->width;
      config.image_height_pixels = framebuffer->height;
      config.number_of_render_targets = MAX2(color_attachment_count, 1);
      config.multisample_mode_4x = false; /* FIXME */
      config.maximum_bpp_of_all_render_targets = tiling->internal_bpp;
   }

   for (uint32_t i = 0; i < color_attachment_count; i++) {
      uint32_t rt_idx = color_attachments[i].colorAttachment;
      uint32_t attachment_idx = subpass->color_attachments[rt_idx].attachment;
      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      const struct v3dv_render_pass_attachment *attachment =
         &state->pass->attachments[attachment_idx];

      uint32_t internal_type, internal_bpp, internal_size;
      const struct v3dv_format *format =
         v3dv_get_format(attachment->desc.format);
      v3dv_get_internal_type_bpp_for_output_format(format->rt_type,
                                                   &internal_type,
                                                   &internal_bpp);
      internal_size = 4 << internal_bpp;

      uint32_t clear_color[4] = { 0 };
      v3dv_get_hw_clear_color(&color_attachments[i].clearValue.color,
                              internal_type,
                              internal_size,
                              clear_color);

      struct v3dv_image_view *iview = framebuffer->attachments[attachment_idx];
      const struct v3dv_image *image = iview->image;
      const struct v3d_resource_slice *slice = &image->slices[iview->base_level];

      uint32_t clear_pad = 0;
      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         int uif_block_height = v3d_utile_height(image->cpp) * 2;

         uint32_t implicit_padded_height =
            align(framebuffer->height, uif_block_height) / uif_block_height;

         if (slice->padded_height_of_output_image_in_uif_blocks -
             implicit_padded_height >= 15) {
            clear_pad = slice->padded_height_of_output_image_in_uif_blocks;
         }
      }

      cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1, clear) {
         clear.clear_color_low_32_bits = clear_color[0];
         clear.clear_color_next_24_bits = clear_color[1] & 0xffffff;
         clear.render_target_number = i;
      };

      if (iview->internal_bpp >= V3D_INTERNAL_BPP_64) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART2, clear) {
            clear.clear_color_mid_low_32_bits =
              ((clear_color[1] >> 24) | (clear_color[2] << 8));
            clear.clear_color_mid_high_24_bits =
              ((clear_color[2] >> 24) | ((clear_color[3] & 0xffff) << 8));
            clear.render_target_number = i;
         };
      }

      if (iview->internal_bpp >= V3D_INTERNAL_BPP_128 || clear_pad) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART3, clear) {
            clear.uif_padded_height_in_uif_blocks = clear_pad;
            clear.clear_color_high_16_bits = clear_color[3] >> 16;
            clear.render_target_number = i;
         };
      }
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
      v3dv_render_pass_setup_render_target(cmd_buffer, 0,
                                           &rt.render_target_0_internal_bpp,
                                           &rt.render_target_0_internal_type,
                                           &rt.render_target_0_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 1,
                                           &rt.render_target_1_internal_bpp,
                                           &rt.render_target_1_internal_type,
                                           &rt.render_target_1_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 2,
                                           &rt.render_target_2_internal_bpp,
                                           &rt.render_target_2_internal_type,
                                           &rt.render_target_2_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 3,
                                           &rt.render_target_3_internal_bpp,
                                           &rt.render_target_3_internal_type,
                                           &rt.render_target_3_clamp);
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
      clear.z_clear_value = ds_clear_value ? ds_clear_value->depth : 1.0f;
      clear.stencil_clear_value = ds_clear_value ? ds_clear_value->stencil : 0;
   };

   cl_emit(rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
      init.use_auto_chained_tile_lists = true;
      init.size_of_first_block_in_chained_tile_lists =
         TILE_ALLOCATION_BLOCK_SIZE_64B;
   }

   for (int layer = base_layer; layer < base_layer + layer_count; layer++) {
      emit_tlb_clear_layer_rcl(cmd_buffer,
                               attachment_count,
                               attachments,
                               layer);
   }

   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
emit_tlb_clear(struct v3dv_cmd_buffer *cmd_buffer,
               uint32_t attachment_count,
               const VkClearAttachment *attachments,
               uint32_t base_layer,
               uint32_t layer_count)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* Save a copy of the current subpass tiling spec, since we are about to
    * split the job for the clear and we will need to then resume it with the
    * same specs.
    */
   struct v3dv_frame_tiling subpass_tiling;
   memcpy(&subpass_tiling, &job->frame_tiling, sizeof(subpass_tiling));

   job = v3dv_cmd_buffer_start_job(cmd_buffer, cmd_buffer->state.subpass_idx);

   /* vkCmdClearAttachments runs inside a render pass */
   job->is_subpass_continue = true;

   emit_tlb_clear_job(cmd_buffer,
                      attachment_count,
                      attachments,
                      base_layer, layer_count);

   /* Since vkCmdClearAttachments executes inside a render pass command, this
    * will emit the binner FLUSH packet.
    */
   v3dv_cmd_buffer_finish_job(cmd_buffer);

   /* Make sure we have an active job to continue the render pass recording
    * after the clear.
    */
   job = v3dv_cmd_buffer_start_job(cmd_buffer, cmd_buffer->state.subpass_idx);

   v3dv_job_start_frame(job,
                        subpass_tiling.width,
                        subpass_tiling.height,
                        subpass_tiling.layers,
                        subpass_tiling.render_target_count,
                        subpass_tiling.internal_bpp);

   job->is_subpass_continue = true;
}

static bool
is_subrect(const VkRect2D *r0, const VkRect2D *r1)
{
   return r0->offset.x <= r1->offset.x &&
          r0->offset.y <= r1->offset.y &&
          r0->offset.x + r0->extent.width >= r1->offset.x + r1->extent.width &&
          r0->offset.y + r0->extent.height >= r1->offset.y + r1->extent.height;
}

static bool
can_use_tlb_clear(struct v3dv_cmd_buffer *cmd_buffer,
                  uint32_t rect_count,
                  const VkClearRect* rects)
{
   const struct v3dv_framebuffer *framebuffer = cmd_buffer->state.framebuffer;

   const VkRect2D *render_area = &cmd_buffer->state.render_area;

   /* Check if we are clearing a single region covering the entire framebuffer
    * and that we are not constrained by the current render area.
    */
   const VkRect2D fb_rect = {
      {0, 0},
      { framebuffer->width, framebuffer->height}
   };

   bool ok =
      rect_count == 1 &&
      is_subrect(&rects[0].rect, &fb_rect) &&
      is_subrect(render_area, &fb_rect);

   if (!ok)
      return false;

   /* If we have enabled scissors, make sure they don't reduce the clear area.
    * For simplicity, we only check the case for a single scissor.
    */
   const struct v3dv_scissor_state *scissor_state =
      &cmd_buffer->state.dynamic.scissor;

   if (scissor_state->count == 0)
      return true;

   if (scissor_state->count == 1)
      return is_subrect(&scissor_state->scissors[0], &fb_rect);

   return false;
}

void
v3dv_CmdClearAttachments(VkCommandBuffer commandBuffer,
                         uint32_t attachmentCount,
                         const VkClearAttachment *pAttachments,
                         uint32_t rectCount,
                         const VkClearRect *pRects)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   /* We can only clear attachments in the current subpass */
   assert(attachmentCount <= 5); /* 4 color + D/S */

   /* Check if we can use the fast path via the TLB */
   if (can_use_tlb_clear(cmd_buffer, rectCount, pRects)) {
      emit_tlb_clear(cmd_buffer, attachmentCount, pAttachments,
                     pRects[0].baseArrayLayer, pRects[0].layerCount);
      return;
   }

   /* Otherwise, fall back to drawing rects with the clear value */
   const struct v3dv_subpass *subpass =
      &cmd_buffer->state.pass->subpasses[cmd_buffer->state.subpass_idx];

   for (uint32_t i = 0; i < attachmentCount; i++) {
      uint32_t attachment_idx = VK_ATTACHMENT_UNUSED;
      int32_t rt_idx = -1;

      if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         rt_idx = pAttachments[i].colorAttachment;
         attachment_idx = subpass->color_attachments[rt_idx].attachment;
      } else if (pAttachments[i].aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                               VK_IMAGE_ASPECT_STENCIL_BIT)) {
         attachment_idx = subpass->ds_attachment.attachment;
      }

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      if (rt_idx != -1) {
         for (uint32_t j = 0; j < rectCount; j++) {
            emit_color_clear_rect(cmd_buffer,
                                  rt_idx,
                                  pAttachments[i].clearValue.color,
                                  pRects[j]);
         }
      } else {
         for (uint32_t j = 0; j < rectCount; j++) {
            emit_ds_clear_rect(cmd_buffer,
                               pAttachments[i].clearValue.depthStencil,
                               pRects[j]);
         }
      }
   }
}

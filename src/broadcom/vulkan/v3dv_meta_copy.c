/*
 * Copyright © 2019 Raspberry Pi
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
emit_image_loads(struct v3dv_cl *cl,
                 struct v3dv_image *image,
                 uint32_t layer,
                 uint32_t mip_level)
{
   uint32_t layer_offset = v3dv_layer_offset(image, mip_level, layer);

   const struct v3d_resource_slice *slice = &image->slices[mip_level];
   cl_emit(cl, LOAD_TILE_BUFFER_GENERAL, load) {
      load.buffer_to_load = RENDER_TARGET_0;
      load.address = v3dv_cl_address(image->mem->bo, layer_offset);

      load.input_image_format = image->format->rt_type;
      load.r_b_swap = false;
      load.memory_format = slice->tiling;

      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         load.height_in_ub_or_stride =
            slice->padded_height_of_output_image_in_uif_blocks;
      } else if (slice->tiling == VC5_TILING_RASTER) {
         load.height_in_ub_or_stride = slice->stride;
      }

      if (image->samples > VK_SAMPLE_COUNT_1_BIT)
         load.decimate_mode = V3D_DECIMATE_MODE_ALL_SAMPLES;
      else
         load.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }

   cl_emit(cl, END_OF_LOADS, end);
}

static void
emit_buffer_stores(struct v3dv_cl *cl,
                   struct v3dv_buffer *buffer,
                   struct v3dv_image *image,
                   uint32_t buffer_offset,
                   uint32_t buffer_stride)
{
   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = RENDER_TARGET_0;
      store.address = v3dv_cl_address(buffer->mem->bo, buffer_offset);
      store.clear_buffer_being_stored = false;

      store.output_image_format = image->format->rt_type;
      store.r_b_swap = false;
      store.memory_format = VC5_TILING_RASTER;
      store.height_in_ub_or_stride = buffer_stride;

      if (image->samples > VK_SAMPLE_COUNT_1_BIT)
         store.decimate_mode = V3D_DECIMATE_MODE_ALL_SAMPLES;
      else
         store.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }
}

static void
emit_copy_layer_to_buffer_per_tile_list(struct v3dv_job *job,
                                        struct v3dv_buffer *buffer,
                                        struct v3dv_image *image,
                                        uint32_t layer,
                                        const VkBufferImageCopy *region)
{
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   const VkImageSubresourceLayers *imgrsc = &region->imageSubresource;
   assert(layer < imgrsc->layerCount);

   /* Load image to TLB */
   emit_image_loads(cl, image, imgrsc->baseArrayLayer + layer, imgrsc->mipLevel);

   cl_emit(cl, PRIM_LIST_FORMAT, fmt) {
      fmt.primitive_type = LIST_TRIANGLES;
   }

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   /* Store TLB to buffer */
   uint32_t width, height;
   if (region->bufferRowLength == 0)
      width = image->extent.width;
   else
      width = region->bufferRowLength;

   if (region->bufferImageHeight == 0)
      height = image->extent.height;
   else
      height = region->bufferImageHeight;

   uint32_t buffer_stride = width * image->cpp;
   uint32_t buffer_offset =
      region->bufferOffset + height * buffer_stride * layer;
   emit_buffer_stores(cl, buffer, image, buffer_offset, buffer_stride);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_copy_layer_to_buffer(struct v3dv_job *job,
                          uint32_t min_x_supertile,
                          uint32_t min_y_supertile,
                          uint32_t max_x_supertile,
                          uint32_t max_y_supertile,
                          struct v3dv_buffer *buffer,
                          struct v3dv_image *image,
                          struct v3dv_framebuffer *framebuffer,
                          uint32_t layer,
                          const VkBufferImageCopy *region)
{
   struct v3dv_cl *rcl = &job->rcl;

   const uint32_t tile_alloc_offset =
      64 * layer * framebuffer->draw_tiles_x * framebuffer->draw_tiles_y;
   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(job->tile_alloc, tile_alloc_offset);
   }

   cl_emit(rcl, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
      config.number_of_bin_tile_lists = 1;
      config.total_frame_width_in_tiles = framebuffer->draw_tiles_x;
      config.total_frame_height_in_tiles = framebuffer->draw_tiles_y;

      config.supertile_width_in_tiles = framebuffer->supertile_width;
      config.supertile_height_in_tiles = framebuffer->supertile_height;

      config.total_frame_width_in_supertiles =
         framebuffer->frame_width_in_supertiles;
      config.total_frame_height_in_supertiles =
         framebuffer->frame_height_in_supertiles;
   }

   /* GFXH-1742 workaround */
   for (int i = 0; i < 2; i++) {
      cl_emit(rcl, TILE_COORDINATES, coords);
      cl_emit(rcl, END_OF_LOADS, end);
      cl_emit(rcl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
      cl_emit(rcl, END_OF_TILE_MARKER, end);
   }

   cl_emit(rcl, FLUSH_VCD_CACHE, flush);

   emit_copy_layer_to_buffer_per_tile_list(job, buffer, image, layer, region);

   for (int y = min_y_supertile; y <= max_y_supertile; y++) {
      for (int x = min_x_supertile; x <= max_x_supertile; x++) {
         cl_emit(rcl, SUPERTILE_COORDINATES, coords) {
            coords.column_number_in_supertiles = x;
            coords.row_number_in_supertiles = y;
         }
      }
   }
}

static void
emit_copy_image_to_buffer_rcl(struct v3dv_job *job,
                              struct v3dv_buffer *buffer,
                              struct v3dv_image *image,
                              struct v3dv_framebuffer *framebuffer,
                              uint32_t internal_type,
                              const VkBufferImageCopy *region)
{
   const VkImageSubresourceLayers *imgrsc = &region->imageSubresource;

   struct v3dv_cl *rcl = &job->rcl;
   v3dv_cl_ensure_space_with_branch(rcl, 200 +
                                    imgrsc->layerCount * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   uint32_t level_width = u_minify(image->extent.width, imgrsc->mipLevel);
   uint32_t level_height = u_minify(image->extent.height, imgrsc->mipLevel);
   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.early_z_disable = true;
      config.image_width_pixels = level_width;
      config.image_height_pixels = level_height;
      config.number_of_render_targets = 1;
      config.multisample_mode_4x = false; /* FIXME */
      config.maximum_bpp_of_all_render_targets = framebuffer->internal_bpp;
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
      rt.render_target_0_internal_bpp = framebuffer->internal_bpp;
      rt.render_target_0_internal_type = internal_type;
      rt.render_target_0_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
   }

   /* We always need to emit this, since it signals the end of the RCL config */
   cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
      clear.z_clear_value = 0;
      clear.stencil_clear_value = 0;
   };

   cl_emit(rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
      init.use_auto_chained_tile_lists = true;
      init.size_of_first_block_in_chained_tile_lists =
         TILE_ALLOCATION_BLOCK_SIZE_64B;
   }

   uint32_t supertile_w_in_pixels =
      framebuffer->tile_width * framebuffer->supertile_width;
   uint32_t supertile_h_in_pixels =
      framebuffer->tile_height * framebuffer->supertile_height;
   const uint32_t min_x_supertile =
      region->imageOffset.x / supertile_w_in_pixels;
   const uint32_t min_y_supertile =
      region->imageOffset.y / supertile_h_in_pixels;

   const uint32_t max_render_x =
      region->imageOffset.x + region->imageExtent.width - 1;
   const uint32_t max_render_y =
      region->imageOffset.y + region->imageExtent.height - 1;
   const uint32_t max_x_supertile = max_render_x / supertile_w_in_pixels;
   const uint32_t max_y_supertile = max_render_y / supertile_h_in_pixels;

   for (int layer = 0; layer < imgrsc->layerCount; layer++) {
      emit_copy_layer_to_buffer(job,
                                min_x_supertile, min_y_supertile,
                                max_x_supertile, max_y_supertile,
                                buffer, image, framebuffer,
                                layer,
                                region);
   }

   cl_emit(rcl, END_OF_RENDERING, end);
}

/* Sets framebuffer dimensions and computes tile size parameters based on the
 * maximum internal bpp provided.
 */
static void
setup_framebuffer_params(struct v3dv_framebuffer *fb,
                         struct v3dv_image *image,
                         uint32_t layer_count,
                         uint32_t internal_bpp)
{
   fb->width  = image->extent.width;
   fb->height = image->extent.height;
   fb->layers = layer_count;
   fb->internal_bpp = MAX2(RENDER_TARGET_MAXIMUM_32BPP, internal_bpp);

   /* We are only interested in the framebufer description required to compute
    * the tiling setup parameters below, so we don't need real attachments,
    * only the framebuffer size and the internal bpp.
    */
   fb->attachment_count = 0;
   fb->color_attachment_count = 0;

   v3dv_framebuffer_compute_tiling_params(fb);
}

static inline bool
can_use_tlb_copy_for_image_region(const VkBufferImageCopy *region)
{
   return region->imageOffset.x == 0 && region->imageOffset.y == 0;
}

/* Implements a copy using the TLB.
 *
 * This only works if we are copying from offset (0,0), since a TLB store for
 * tile (x,y) will be written at the same tile offset into the destination.
 * When this requirement is not met, we need to use a blit instead.
 */
static void
copy_image_to_buffer_tlb(struct v3dv_cmd_buffer *cmd_buffer,
                         struct v3dv_buffer *buffer,
                         struct v3dv_image *image,
                         const VkBufferImageCopy *region)
{
      assert(can_use_tlb_copy_for_image_region(region));

      /* FIXME: pre-compute this at image creation time? */
      uint32_t internal_type;
      uint32_t internal_bpp;
      v3dv_get_internal_type_bpp_for_output_format(image->format->rt_type,
                                                   &internal_type,
                                                   &internal_bpp);

      uint32_t num_layers = region->imageSubresource.layerCount;
      assert(num_layers > 0);

      struct v3dv_framebuffer framebuffer;
      setup_framebuffer_params(&framebuffer, image, num_layers, internal_bpp);

      struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer);
      v3dv_cmd_buffer_start_frame(cmd_buffer, &framebuffer);

      v3dv_job_emit_binning_flush(job);
      emit_copy_image_to_buffer_rcl(job, buffer, image,
                                    &framebuffer, internal_type, region);

      v3dv_cmd_buffer_finish_job(cmd_buffer);
}

void
v3dv_CmdCopyImageToBuffer(VkCommandBuffer commandBuffer,
                          VkImage srcImage,
                          VkImageLayout srcImageLayout,
                          VkBuffer destBuffer,
                          uint32_t regionCount,
                          const VkBufferImageCopy *pRegions)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_image, image, srcImage);
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, destBuffer);

   for (uint32_t i = 0; i < regionCount; i++) {
      if (can_use_tlb_copy_for_image_region(&pRegions[i]))
         copy_image_to_buffer_tlb(cmd_buffer, buffer, image, &pRegions[i]);
   }
}


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

/* This chooses a tile buffer format that is appropriate for the copy operation.
 * Typically, this is the image render target type, however, for depth/stencil
 * formats that can't be stored to raster, we need to use a compatible color
 * format instead.
 */
static uint32_t
choose_tlb_format(struct v3dv_image *image,
                  VkImageAspectFlags aspect,
                  bool for_store)
{
   switch (image->vk_format) {
   case VK_FORMAT_D16_UNORM:
      return V3D_OUTPUT_IMAGE_FORMAT_R16UI;
   case VK_FORMAT_D32_SFLOAT:
      return V3D_OUTPUT_IMAGE_FORMAT_R32F;
   case VK_FORMAT_X8_D24_UNORM_PACK32:
      return V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI;
   case VK_FORMAT_D24_UNORM_S8_UINT:
      /* When storing the stencil aspect of a combined depth/stencil image,
       * the Vulkan spec states that the output buffer must have packed stencil
       * values, so we choose an R8UI format for our store outputs. For the
       * load input we still want RGBA8UI since the source image contains 4
       * channels (including the 3 channels containing the 24-bit depth value).
       */
      if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
         return V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI;
      } else {
         assert(aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
         return for_store ? V3D_OUTPUT_IMAGE_FORMAT_R8UI :
                            V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI;
      }
   default:
      return image->format->rt_type;
      break;
   }
}

static void
emit_image_loads(struct v3dv_cl *cl,
                 struct v3dv_image *image,
                 VkImageAspectFlags aspect,
                 uint32_t layer,
                 uint32_t mip_level)
{
   uint32_t layer_offset = v3dv_layer_offset(image, mip_level, layer);

   const struct v3d_resource_slice *slice = &image->slices[mip_level];
   cl_emit(cl, LOAD_TILE_BUFFER_GENERAL, load) {
      load.buffer_to_load = RENDER_TARGET_0;
      load.address = v3dv_cl_address(image->mem->bo, layer_offset);

      load.input_image_format = choose_tlb_format(image, aspect, false);
      load.memory_format = slice->tiling;

      /* For D24 formats Vulkan expects the depth value in the LSB bits of each
       * 32-bit pixel. Unfortunately, the hardware seems to put the S8/X8 bits
       * there and the depth bits on the MSB. To work around that we can reverse
       * the channel order and then swap the R/B channels to get what we want.
       *
       * NOTE: reversing and swapping only gets us the behavior we want if the
       * operations happen in that exact order, which seems to be the case when
       * done on the tile buffer load operations. On the store, it seems the
       * order is not the same. The order on the store is probably reversed so
       * that reversing and swapping on both the load and the store preserves
       * the original order of the channels in memory.
       */
      if (image->vk_format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
          (image->vk_format == VK_FORMAT_D24_UNORM_S8_UINT &&
           (aspect & VK_IMAGE_ASPECT_DEPTH_BIT))) {
         load.r_b_swap = true;
         load.channel_reverse = true;
      }

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
                   VkImageAspectFlags aspect,
                   uint32_t buffer_offset,
                   uint32_t buffer_stride)
{
   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = RENDER_TARGET_0;
      store.address = v3dv_cl_address(buffer->mem->bo, buffer_offset);
      store.clear_buffer_being_stored = false;

      store.output_image_format = choose_tlb_format(image, aspect, true);
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
   emit_image_loads(cl, image, imgrsc->aspectMask,
                    imgrsc->baseArrayLayer + layer, imgrsc->mipLevel);

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

   /* If we are storing stencil from a combined depth/stencil format the
    * Vulkan spec states that the output buffer must have packed stencil
    * values, where each stencil value is 1 byte.
    */
   uint32_t cpp = imgrsc->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT ?
                  1 : image->cpp;
   uint32_t buffer_stride = width * cpp;
   uint32_t buffer_offset =
      region->bufferOffset + height * buffer_stride * layer;
   emit_buffer_stores(cl, buffer, image, imgrsc->aspectMask,
                      buffer_offset, buffer_stride);

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
                         uint32_t width,
                         uint32_t height,
                         uint32_t layer_count,
                         uint32_t internal_bpp)
{
   fb->width  = width;
   fb->height = height;
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

   const VkImageAspectFlags ds_aspects = VK_IMAGE_ASPECT_DEPTH_BIT |
                                         VK_IMAGE_ASPECT_STENCIL_BIT;

   /* We can't store depth/stencil pixel formats to a raster format, so
    * so instead we load our depth/stencil aspects to a compatible color
    * format.
    */
   /* FIXME: pre-compute this at image creation time? */
   uint32_t internal_type;
   uint32_t internal_bpp;
   if (region->imageSubresource.aspectMask & ds_aspects) {
      switch (image->vk_format) {
      case VK_FORMAT_D16_UNORM:
         internal_type = V3D_INTERNAL_TYPE_16UI;
         internal_bpp = V3D_INTERNAL_BPP_64;
         break;
      case VK_FORMAT_D32_SFLOAT:
         internal_type = V3D_INTERNAL_TYPE_32F;
         internal_bpp = V3D_INTERNAL_BPP_128;
         break;
      case VK_FORMAT_X8_D24_UNORM_PACK32:
      case VK_FORMAT_D24_UNORM_S8_UINT:
         /* Use RGBA8 format so we can relocate the X/S bits in the appropriate
          * place to match Vulkan expectations. See the comment on the tile
          * load command for more details.
          */
         internal_type = V3D_INTERNAL_TYPE_8UI;
         internal_bpp = V3D_INTERNAL_BPP_32;
         break;
      default:
         assert(!"unsupported format");
         break;
      }
   } else {
      v3dv_get_internal_type_bpp_for_output_format(image->format->rt_type,
                                                   &internal_type,
                                                   &internal_bpp);
   }

   uint32_t num_layers = region->imageSubresource.layerCount;
   assert(num_layers > 0);

   struct v3dv_framebuffer framebuffer;
   setup_framebuffer_params(&framebuffer,
                            image->extent.width, image->extent.height,
                            num_layers, internal_bpp);

   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, false);
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

static void
emit_copy_buffer_per_tile_list(struct v3dv_job *job,
                               struct v3dv_buffer *dst,
                               struct v3dv_buffer *src,
                               uint32_t dst_offset,
                               uint32_t src_offset,
                               uint32_t stride,
                               uint32_t format)
{
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   cl_emit(cl, LOAD_TILE_BUFFER_GENERAL, load) {
      load.buffer_to_load = RENDER_TARGET_0;
      load.address = v3dv_cl_address(src->mem->bo, src_offset);
      load.input_image_format = format;
      load.memory_format = VC5_TILING_RASTER;
      load.height_in_ub_or_stride = stride;
      load.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, PRIM_LIST_FORMAT, fmt) {
      fmt.primitive_type = LIST_TRIANGLES;
   }

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = RENDER_TARGET_0;
      store.address = v3dv_cl_address(dst->mem->bo, dst_offset);
      store.clear_buffer_being_stored = false;
      store.output_image_format = format;
      store.memory_format = VC5_TILING_RASTER;
      store.height_in_ub_or_stride = stride;
      store.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_copy_buffer(struct v3dv_job *job,
                 uint32_t min_x_supertile,
                 uint32_t min_y_supertile,
                 uint32_t max_x_supertile,
                 uint32_t max_y_supertile,
                 struct v3dv_buffer *dst,
                 struct v3dv_buffer *src,
                 uint32_t dst_offset,
                 uint32_t src_offset,
                 struct v3dv_framebuffer *framebuffer,
                 uint32_t format)
{
   struct v3dv_cl *rcl = &job->rcl;

   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(job->tile_alloc, 0);
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

   const uint32_t stride = framebuffer->width * 4;
   emit_copy_buffer_per_tile_list(job, dst, src,
                                  dst_offset, src_offset,
                                  stride, format);

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
emit_copy_buffer_rcl(struct v3dv_job *job,
                     struct v3dv_buffer *dst,
                     struct v3dv_buffer *src,
                     uint32_t dst_offset,
                     uint32_t src_offset,
                     struct v3dv_framebuffer *framebuffer,
                     uint32_t internal_type,
                     uint32_t format)
{
   struct v3dv_cl *rcl = &job->rcl;
   v3dv_cl_ensure_space_with_branch(rcl, 200 +
                                    1 * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.early_z_disable = true;
      config.image_width_pixels = framebuffer->width;
      config.image_height_pixels = framebuffer->height;
      config.number_of_render_targets = 1;
      config.multisample_mode_4x = false;
      config.maximum_bpp_of_all_render_targets = framebuffer->internal_bpp;
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
      rt.render_target_0_internal_bpp = framebuffer->internal_bpp;
      rt.render_target_0_internal_type = internal_type;
      rt.render_target_0_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
   }

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

   const uint32_t min_x_supertile = 0;
   const uint32_t min_y_supertile = 0;

   const uint32_t max_x_supertile =
      (framebuffer->width - 1) / supertile_w_in_pixels;
   const uint32_t max_y_supertile =
      (framebuffer->height - 1) / supertile_h_in_pixels;

   emit_copy_buffer(job,
                    min_x_supertile, min_y_supertile,
                    max_x_supertile, max_y_supertile,
                    dst, src, dst_offset, src_offset,
                    framebuffer, format);

   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
copy_buffer(struct v3dv_cmd_buffer *cmd_buffer,
            struct v3dv_buffer *dst,
            struct v3dv_buffer *src,
            const VkBufferCopy *region)
{
   const uint32_t internal_bpp = V3D_INTERNAL_BPP_32;
   const uint32_t internal_type = V3D_INTERNAL_TYPE_8UI;

   /* Select appropriate pixel format for the copy operation based on the
    * alignment of the size to copy.
    */
   uint32_t item_size;
   uint32_t format;
   switch (region->size % 4) {
   case 0:
      item_size = 4;
      format = V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI;
      break;
   case 2:
      item_size = 2;
      format = V3D_OUTPUT_IMAGE_FORMAT_RG8UI;
      break;
   case 1:
   case 3:
      item_size = 1;
      format = V3D_OUTPUT_IMAGE_FORMAT_R8UI;
      break;

   }
   assert(region->size % item_size == 0);
   uint32_t num_items = region->size / item_size;

   uint32_t src_offset = region->srcOffset;
   uint32_t dst_offset = region->dstOffset;
   while (num_items > 0) {
      /* Figure out a TLB size configuration for the number of items to copy.
       * We can't "render" more than 4096x4096 pixels in a single job, so make
       * sure we don't exceed that by splitting the job into multiple jobs if
       * needed.
       */
      const uint32_t max_dim_items = 4096;
      const uint32_t max_items = max_dim_items * max_dim_items;
      uint32_t width, height;
      if (num_items > max_items) {
         width = max_dim_items;
         height = max_dim_items;
      } else {
         width = num_items;
         height = 1;
         while (width > max_dim_items ||
                ((width % 2) == 0 && width > 2 * height)) {
            width >>= 1;
            height <<= 1;
         }
      }
      assert(width <= max_dim_items && height <= max_dim_items);
      assert(width * height <= num_items);

      struct v3dv_framebuffer framebuffer;
      setup_framebuffer_params(&framebuffer, width, height, 1, internal_bpp);

      struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, false);
      v3dv_cmd_buffer_start_frame(cmd_buffer, &framebuffer);

      v3dv_job_emit_binning_flush(job);

      emit_copy_buffer_rcl(job, dst, src, dst_offset, src_offset,
                           &framebuffer, internal_type, format);

      v3dv_cmd_buffer_finish_job(cmd_buffer);

      const uint32_t items_copied = width * height;
      const uint32_t bytes_copied = items_copied * item_size;
      num_items -= items_copied;
      src_offset += bytes_copied;
      dst_offset += bytes_copied;
   }
}

void
v3dv_CmdCopyBuffer(VkCommandBuffer commandBuffer,
                   VkBuffer srcBuffer,
                   VkBuffer dstBuffer,
                   uint32_t regionCount,
                   const VkBufferCopy *pRegions)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_buffer, src_buffer, srcBuffer);
   V3DV_FROM_HANDLE(v3dv_buffer, dst_buffer, dstBuffer);

   for (uint32_t i = 0; i < regionCount; i++)
     copy_buffer(cmd_buffer, dst_buffer, src_buffer, &pRegions[i]);
}

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

/* Most "copy" operations in this file are implemented using the tile buffer
 * to fill and/or copy buffers and images. To do that, we need to have some
 * representation of a framebuffer that describes the layout of the render
 * target and the tiling information. That information is typically represented
 * in a framebuffer object but for most operations in this file we don't have
 * one provided by the user, so instead we need to create one that matches
 * the semantics of the copy operation we want to implement. A "real"
 * framebuffer description includes references to image views (v3dv_image_view)
 * and their underlying images (v3dv_image) for each attachment though,
 * but here, we usually work with buffers instead of images, or we have images
 * but we don't have image views, so instead of trying to use a real
 * framebuffer we use a "fake" one, where we don't include attachment info
 * and we simply store the internal type of the single render target we are
 * copying to or filling with data.
 */
struct fake_framebuffer {
   struct v3dv_framebuffer fb;
   uint32_t internal_type;
   uint32_t min_x_supertile;
   uint32_t min_y_supertile;
   uint32_t max_x_supertile;
   uint32_t max_y_supertile;
};

/* Sets framebuffer dimensions and computes tile size parameters based on the
 * maximum internal bpp provided.
 */
static void
setup_framebuffer_params(struct fake_framebuffer *fb,
                         uint32_t width,
                         uint32_t height,
                         uint32_t layer_count,
                         uint32_t internal_bpp,
                         uint32_t internal_type)
{
   fb->fb.width  = width;
   fb->fb.height = height;
   fb->fb.layers = layer_count;
   fb->fb.internal_bpp = MAX2(RENDER_TARGET_MAXIMUM_32BPP, internal_bpp);

   /* We are only interested in the framebufer description required to compute
    * the tiling setup parameters below, so we don't need real attachments,
    * only the framebuffer size and the internal bpp.
    */
   fb->fb.attachment_count = 0;
   fb->fb.color_attachment_count = 0;

   /* For simplicity, we store the internal type of the single render target
    * that functions in this file need in the fake framebuffer objects so
    * we don't have to pass it around everywhere.
    */
   fb->internal_type = internal_type;

   v3dv_framebuffer_compute_tiling_params(&fb->fb);

   uint32_t supertile_w_in_pixels = fb->fb.tile_width * fb->fb.supertile_width;
   uint32_t supertile_h_in_pixels = fb->fb.tile_height * fb->fb.supertile_height;
   fb->min_x_supertile = 0;
   fb->min_y_supertile = 0;
   fb->max_x_supertile = (fb->fb.width - 1) / supertile_w_in_pixels;
   fb->max_y_supertile = (fb->fb.height - 1) / supertile_h_in_pixels;
}

/* This chooses a tile buffer format that is appropriate for the copy operation.
 * Typically, this is the image render target type, however, if we are copying
 * depth/stencil to/from a buffer the hardware can't do raster loads/stores, so
 * we need to load and store to/from a tile color buffer using a compatible
 * color format.
 */
static uint32_t
choose_tlb_format(struct v3dv_image *image,
                  VkImageAspectFlags aspect,
                  bool for_store,
                  bool is_copy_to_buffer,
                  bool is_copy_from_buffer)
{
   if (is_copy_to_buffer || is_copy_from_buffer) {
      switch (image->vk_format) {
      case VK_FORMAT_D16_UNORM:
         return V3D_OUTPUT_IMAGE_FORMAT_R16UI;
      case VK_FORMAT_D32_SFLOAT:
         return V3D_OUTPUT_IMAGE_FORMAT_R32F;
      case VK_FORMAT_X8_D24_UNORM_PACK32:
         return V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI;
      case VK_FORMAT_D24_UNORM_S8_UINT:
         /* When storing the stencil aspect of a combined depth/stencil image
          * to a buffer, the Vulkan spec states that the output buffer must
          * have packed stencil values, so we choose an R8UI format for our
          * store outputs. For the load input we still want RGBA8UI since the
          * source image contains 4 channels (including the 3 channels
          * containing the 24-bit depth value).
          *
          * When loading the stencil aspect of a combined depth/stencil image
          * from a buffer, we read packed 8-bit stencil values from the buffer
          * that we need to put into the LSB of the 32-bit format (the R
          * channel), so we use R8UI. For the store, if we used R8UI then we
          * would write 8-bit stencil values consecutively over depth channels,
          * so we need to use RGBA8UI. This will write each stencil value in
          * its correct position, but will overwrite depth values (channels G
          * B,A) with undefined values. To fix this,  we will have to restore
          * the depth aspect from the Z tile buffer, which we should pre-load
          * from the image before the store).
          */
         if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
            return V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI;
         } else {
            assert(aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
            if (is_copy_to_buffer) {
               return for_store ? V3D_OUTPUT_IMAGE_FORMAT_R8UI :
                                  V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI;
            } else {
               assert(is_copy_from_buffer);
               return for_store ? V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI :
                                  V3D_OUTPUT_IMAGE_FORMAT_R8UI;
            }
         }
      default: /* Color formats */
         return image->format->rt_type;
         break;
      }
   } else {
      return image->format->rt_type;
   }
}

static void
get_internal_type_bpp_for_image_aspects(struct v3dv_image *image,
                                        VkImageAspectFlags aspect_mask,
                                        uint32_t *internal_type,
                                        uint32_t *internal_bpp)
{
   const VkImageAspectFlags ds_aspects = VK_IMAGE_ASPECT_DEPTH_BIT |
                                         VK_IMAGE_ASPECT_STENCIL_BIT;

   /* We can't store depth/stencil pixel formats to a raster format, so
    * so instead we load our depth/stencil aspects to a compatible color
    * format.
    */
   /* FIXME: pre-compute this at image creation time? */
   if (aspect_mask & ds_aspects) {
      switch (image->vk_format) {
      case VK_FORMAT_D16_UNORM:
         *internal_type = V3D_INTERNAL_TYPE_16UI;
         *internal_bpp = V3D_INTERNAL_BPP_64;
         break;
      case VK_FORMAT_D32_SFLOAT:
         *internal_type = V3D_INTERNAL_TYPE_32F;
         *internal_bpp = V3D_INTERNAL_BPP_128;
         break;
      case VK_FORMAT_X8_D24_UNORM_PACK32:
      case VK_FORMAT_D24_UNORM_S8_UINT:
         /* Use RGBA8 format so we can relocate the X/S bits in the appropriate
          * place to match Vulkan expectations. See the comment on the tile
          * load command for more details.
          */
         *internal_type = V3D_INTERNAL_TYPE_8UI;
         *internal_bpp = V3D_INTERNAL_BPP_32;
         break;
      default:
         assert(!"unsupported format");
         break;
      }
   } else {
      v3dv_get_internal_type_bpp_for_output_format(image->format->rt_type,
                                                   internal_type,
                                                   internal_bpp);
   }
}

static struct v3dv_cl *
emit_rcl_prologue(struct v3dv_job *job,
                  struct fake_framebuffer *framebuffer,
                  const union v3dv_clear_value *clear_value,
                  struct v3dv_image *image,
                  VkImageAspectFlags aspects,
                  uint32_t layer,
                  uint32_t level)
{
   struct v3dv_cl *rcl = &job->rcl;
   v3dv_cl_ensure_space_with_branch(rcl, 200 +
                                    framebuffer->fb.layers * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.early_z_disable = true;
      config.image_width_pixels = framebuffer->fb.width;
      config.image_height_pixels = framebuffer->fb.height;
      config.number_of_render_targets = 1;
      config.multisample_mode_4x = false;
      config.maximum_bpp_of_all_render_targets = framebuffer->fb.internal_bpp;
   }

   if (clear_value && (aspects & VK_IMAGE_ASPECT_COLOR_BIT)) {
      uint32_t clear_pad = 0;
      if (image) {
         if (image->slices[layer].tiling == VC5_TILING_UIF_NO_XOR ||
             image->slices[layer].tiling == VC5_TILING_UIF_XOR) {
            const struct v3d_resource_slice *slice = &image->slices[level];

            int uif_block_height = v3d_utile_height(image->cpp) * 2;

            uint32_t implicit_padded_height =
               align(framebuffer->fb.height, uif_block_height) / uif_block_height;

            if (slice->padded_height_of_output_image_in_uif_blocks -
                implicit_padded_height >= 15) {
               clear_pad = slice->padded_height_of_output_image_in_uif_blocks;
            }
         }
      }

      const uint32_t *color = &clear_value->color[0];
      cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1, clear) {
         clear.clear_color_low_32_bits = color[0];
         clear.clear_color_next_24_bits = color[1] & 0x00ffffff;
         clear.render_target_number = 0;
      };

      if (framebuffer->fb.internal_bpp >= V3D_INTERNAL_BPP_64) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART2, clear) {
            clear.clear_color_mid_low_32_bits =
              ((color[1] >> 24) | (color[2] << 8));
            clear.clear_color_mid_high_24_bits =
              ((color[2] >> 24) | ((color[3] & 0xffff) << 8));
            clear.render_target_number = 0;
         };
      }

      if (framebuffer->fb.internal_bpp >= V3D_INTERNAL_BPP_128 || clear_pad) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART3, clear) {
            clear.uif_padded_height_in_uif_blocks = clear_pad;
            clear.clear_color_high_16_bits = color[3] >> 16;
            clear.render_target_number = 0;
         };
      }
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
      rt.render_target_0_internal_bpp = framebuffer->fb.internal_bpp;
      rt.render_target_0_internal_type = framebuffer->internal_type;
      rt.render_target_0_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
      clear.z_clear_value = clear_value ? clear_value->z : 1.0f;
      clear.stencil_clear_value = clear_value ? clear_value->s : 0;
   };

   cl_emit(rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
      init.use_auto_chained_tile_lists = true;
      init.size_of_first_block_in_chained_tile_lists =
         TILE_ALLOCATION_BLOCK_SIZE_64B;
   }

   return rcl;
}

static void
emit_frame_setup(struct v3dv_job *job,
                 struct fake_framebuffer *framebuffer,
                 uint32_t layer,
                 const union v3dv_clear_value *clear_value)
{
   struct v3dv_cl *rcl = &job->rcl;

   const uint32_t tile_alloc_offset =
      64 * layer * framebuffer->fb.draw_tiles_x * framebuffer->fb.draw_tiles_y;
   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(job->tile_alloc, tile_alloc_offset);
   }

   cl_emit(rcl, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
      config.number_of_bin_tile_lists = 1;
      config.total_frame_width_in_tiles = framebuffer->fb.draw_tiles_x;
      config.total_frame_height_in_tiles = framebuffer->fb.draw_tiles_y;

      config.supertile_width_in_tiles = framebuffer->fb.supertile_width;
      config.supertile_height_in_tiles = framebuffer->fb.supertile_height;

      config.total_frame_width_in_supertiles =
         framebuffer->fb.frame_width_in_supertiles;
      config.total_frame_height_in_supertiles =
         framebuffer->fb.frame_height_in_supertiles;
   }

   /* Implement GFXH-1742 workaround. Also, if we are clearing we have to do
    * it here.
    */
   for (int i = 0; i < 2; i++) {
      cl_emit(rcl, TILE_COORDINATES, coords);
      cl_emit(rcl, END_OF_LOADS, end);
      cl_emit(rcl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
      if (clear_value && i == 0) {
         cl_emit(rcl, CLEAR_TILE_BUFFERS, clear) {
            clear.clear_z_stencil_buffer = true;
            clear.clear_all_render_targets = true;
         }
      }
      cl_emit(rcl, END_OF_TILE_MARKER, end);
   }

   cl_emit(rcl, FLUSH_VCD_CACHE, flush);
}

static void
emit_supertile_coordinates(struct v3dv_job *job,
                           struct fake_framebuffer *framebuffer)
{
   struct v3dv_cl *rcl = &job->rcl;

   const uint32_t min_y = framebuffer->min_y_supertile;
   const uint32_t max_y = framebuffer->max_y_supertile;
   const uint32_t min_x = framebuffer->min_x_supertile;
   const uint32_t max_x = framebuffer->max_x_supertile;

   for (int y = min_y; y <= max_y; y++) {
      for (int x = min_x; x <= max_x; x++) {
         cl_emit(rcl, SUPERTILE_COORDINATES, coords) {
            coords.column_number_in_supertiles = x;
            coords.row_number_in_supertiles = y;
         }
      }
   }
}

static void
emit_linear_load(struct v3dv_cl *cl,
                 uint32_t buffer,
                 struct v3dv_bo *bo,
                 uint32_t offset,
                 uint32_t stride,
                 uint32_t format)
{
   cl_emit(cl, LOAD_TILE_BUFFER_GENERAL, load) {
      load.buffer_to_load = buffer;
      load.address = v3dv_cl_address(bo, offset);
      load.input_image_format = format;
      load.memory_format = VC5_TILING_RASTER;
      load.height_in_ub_or_stride = stride;
      load.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }
}

static void
emit_linear_store(struct v3dv_cl *cl,
                  uint32_t buffer,
                  struct v3dv_bo *bo,
                  uint32_t offset,
                  uint32_t stride,
                  bool msaa,
                  uint32_t format)
{
   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = RENDER_TARGET_0;
      store.address = v3dv_cl_address(bo, offset);
      store.clear_buffer_being_stored = false;
      store.output_image_format = format;
      store.memory_format = VC5_TILING_RASTER;
      store.height_in_ub_or_stride = stride;
      store.decimate_mode = msaa ? V3D_DECIMATE_MODE_ALL_SAMPLES :
                                   V3D_DECIMATE_MODE_SAMPLE_0;
   }
}

static void
emit_image_load(struct v3dv_cl *cl,
                struct v3dv_image *image,
                VkImageAspectFlags aspect,
                uint32_t layer,
                uint32_t mip_level,
                bool is_copy_to_buffer,
                bool is_copy_from_buffer)
{
   uint32_t layer_offset = v3dv_layer_offset(image, mip_level, layer);

   /* For image to/from buffer copies we always load to and store from RT0,
    * even for depth/stencil aspects, because the hardware can't do raster
    * stores or loads from/to the depth/stencil tile buffers.
    */
   bool load_to_color_tlb = is_copy_to_buffer || is_copy_from_buffer ||
                            aspect == VK_IMAGE_ASPECT_COLOR_BIT;

   const struct v3d_resource_slice *slice = &image->slices[mip_level];
   cl_emit(cl, LOAD_TILE_BUFFER_GENERAL, load) {
      load.buffer_to_load = load_to_color_tlb ?
         RENDER_TARGET_0 : v3dv_zs_buffer_from_aspect_bits(aspect);

      load.address = v3dv_cl_address(image->mem->bo, layer_offset);

      load.input_image_format = choose_tlb_format(image, aspect, false,
                                                  is_copy_to_buffer,
                                                  is_copy_from_buffer);
      load.memory_format = slice->tiling;

      /* When copying depth/stencil images to a buffer, for D24 formats Vulkan
       * expects the depth value in the LSB bits of each 32-bit pixel.
       * Unfortunately, the hardware seems to put the S8/X8 bits there and the
       * depth bits on the MSB. To work around that we can reverse the channel
       * order and then swap the R/B channels to get what we want.
       *
       * NOTE: reversing and swapping only gets us the behavior we want if the
       * operations happen in that exact order, which seems to be the case when
       * done on the tile buffer load operations. On the store, it seems the
       * order is not the same. The order on the store is probably reversed so
       * that reversing and swapping on both the load and the store preserves
       * the original order of the channels in memory.
       *
       * Notice that we only need to do this when copying to a buffer, where
       * depth and stencil aspects are copied as separate regions and
       * the spec expects them to be tightly packed.
       */
      bool needs_rb_swap = false;
      bool needs_chan_reverse = false;
      if (is_copy_to_buffer &&
         (image->vk_format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
          (image->vk_format == VK_FORMAT_D24_UNORM_S8_UINT &&
           (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)))) {
         needs_rb_swap = true;
         needs_chan_reverse = true;
      } else if (!is_copy_from_buffer && !is_copy_to_buffer &&
                 (aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
         /* This is not a raw data copy (i.e. we are clearing the image),
          * so we need to make sure we respect the format swizzle.
          */
         const struct util_format_description *desc =
            vk_format_description(image->vk_format);
         needs_rb_swap = desc->swizzle[0] == PIPE_SWIZZLE_Z &&
                         image->vk_format != VK_FORMAT_B5G6R5_UNORM_PACK16;
      }

      load.r_b_swap = needs_rb_swap;
      load.channel_reverse = needs_chan_reverse;

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
}

static void
emit_image_store(struct v3dv_cl *cl,
                 struct v3dv_image *image,
                 VkImageAspectFlags aspect,
                 uint32_t layer,
                 uint32_t mip_level,
                 bool is_copy_to_buffer,
                 bool is_copy_from_buffer)
{
   uint32_t layer_offset = v3dv_layer_offset(image, mip_level, layer);

   bool store_from_color_tlb = is_copy_to_buffer || is_copy_from_buffer ||
                               aspect == VK_IMAGE_ASPECT_COLOR_BIT;

   const struct v3d_resource_slice *slice = &image->slices[mip_level];
   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = store_from_color_tlb ?
         RENDER_TARGET_0 : v3dv_zs_buffer_from_aspect_bits(aspect);

      store.address = v3dv_cl_address(image->mem->bo, layer_offset);
      store.clear_buffer_being_stored = false;

      /* See rationale in emit_image_load() */
      bool needs_rb_swap = false;
      bool needs_chan_reverse = false;
      if (is_copy_from_buffer &&
         (image->vk_format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
          (image->vk_format == VK_FORMAT_D24_UNORM_S8_UINT &&
           (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)))) {
         needs_rb_swap = true;
         needs_chan_reverse = true;
      } else if (!is_copy_from_buffer && !is_copy_to_buffer &&
                 (aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
         const struct util_format_description *desc =
            vk_format_description(image->vk_format);
         needs_rb_swap = desc->swizzle[0] == PIPE_SWIZZLE_Z &&
                         image->vk_format != VK_FORMAT_B5G6R5_UNORM_PACK16;
      }

      store.r_b_swap = needs_rb_swap;
      store.channel_reverse = needs_chan_reverse;

      store.output_image_format = choose_tlb_format(image, aspect, true,
                                                    is_copy_to_buffer,
                                                    is_copy_from_buffer);
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
   emit_image_load(cl, image, imgrsc->aspectMask,
                   imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                   true, false);

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   /* Store TLB to buffer */
   uint32_t width, height;
   if (region->bufferRowLength == 0)
      width = region->imageExtent.width;
   else
      width = region->bufferRowLength;

   if (region->bufferImageHeight == 0)
      height = region->imageExtent.height;
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

   uint32_t format = choose_tlb_format(image, imgrsc->aspectMask,
                                       true, true, false);
   bool msaa = image->samples > VK_SAMPLE_COUNT_1_BIT;

   emit_linear_store(cl, RENDER_TARGET_0, buffer->mem->bo,
                     buffer_offset, buffer_stride, msaa, format);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_copy_layer_to_buffer(struct v3dv_job *job,
                          struct v3dv_buffer *buffer,
                          struct v3dv_image *image,
                          struct fake_framebuffer *framebuffer,
                          uint32_t layer,
                          const VkBufferImageCopy *region)
{
   emit_frame_setup(job, framebuffer, layer, NULL);
   emit_copy_layer_to_buffer_per_tile_list(job, buffer, image, layer, region);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_copy_image_to_buffer_rcl(struct v3dv_job *job,
                              struct v3dv_buffer *buffer,
                              struct v3dv_image *image,
                              struct fake_framebuffer *framebuffer,
                              const VkBufferImageCopy *region)
{
   struct v3dv_cl *rcl = emit_rcl_prologue(job, framebuffer, NULL, NULL,
                                           region->imageSubresource.aspectMask,
                                           0, 0);
   for (int layer = 0; layer < framebuffer->fb.layers; layer++)
      emit_copy_layer_to_buffer(job, buffer, image, framebuffer, layer, region);
   cl_emit(rcl, END_OF_RENDERING, end);
}

static inline bool
can_use_tlb_copy_for_image_offset(const VkOffset3D *offset)
{
   return offset->x == 0 && offset->y == 0;
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
   assert(can_use_tlb_copy_for_image_offset(&region->imageOffset));

   uint32_t internal_type, internal_bpp;
   get_internal_type_bpp_for_image_aspects(image,
                                           region->imageSubresource.aspectMask,
                                           &internal_type, &internal_bpp);

   uint32_t num_layers = region->imageSubresource.layerCount;
   assert(num_layers > 0);

   struct fake_framebuffer framebuffer;
   setup_framebuffer_params(&framebuffer,
                            region->imageExtent.width,
                            region->imageExtent.height,
                            num_layers, internal_bpp, internal_type);

   /* Limit supertile coverage to the requested region  */
   uint32_t supertile_w_in_pixels =
      framebuffer.fb.tile_width * framebuffer.fb.supertile_width;
   uint32_t supertile_h_in_pixels =
      framebuffer.fb.tile_height * framebuffer.fb.supertile_height;
   const uint32_t max_render_x =
      region->imageOffset.x + region->imageExtent.width - 1;
   const uint32_t max_render_y =
      region->imageOffset.y + region->imageExtent.height - 1;

   assert(region->imageOffset.x == 0 && region->imageOffset.y == 0);
   framebuffer.min_x_supertile = 0;
   framebuffer.min_y_supertile = 0;
   framebuffer.max_x_supertile = max_render_x / supertile_w_in_pixels;
   framebuffer.max_y_supertile = max_render_y / supertile_h_in_pixels;

   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, false);
   v3dv_cmd_buffer_start_frame(cmd_buffer, &framebuffer.fb);

   v3dv_job_emit_binning_flush(job);
   emit_copy_image_to_buffer_rcl(job, buffer, image, &framebuffer, region);

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
      if (can_use_tlb_copy_for_image_offset(&pRegions[i].imageOffset))
         copy_image_to_buffer_tlb(cmd_buffer, buffer, image, &pRegions[i]);
      else
         assert(!"Fallback path for vkCopyImageToBuffer not implemented");
   }
}

static void
emit_copy_image_layer_per_tile_list(struct v3dv_job *job,
                                    struct v3dv_image *dst,
                                    struct v3dv_image *src,
                                    uint32_t layer,
                                    const VkImageCopy *region)
{
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   const VkImageSubresourceLayers *srcrsc = &region->srcSubresource;
   assert(layer < srcrsc->layerCount);

   emit_image_load(cl, src, srcrsc->aspectMask,
                   srcrsc->baseArrayLayer + layer, srcrsc->mipLevel,
                   false, false);

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   const VkImageSubresourceLayers *dstrsc = &region->dstSubresource;
   assert(layer < dstrsc->layerCount);

   emit_image_store(cl, dst, dstrsc->aspectMask,
                    dstrsc->baseArrayLayer + layer, dstrsc->mipLevel,
                    false, false);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_copy_image_layer(struct v3dv_job *job,
                      struct v3dv_image *dst,
                      struct v3dv_image *src,
                      struct fake_framebuffer *framebuffer,
                      uint32_t layer,
                      const VkImageCopy *region)
{
   emit_frame_setup(job, framebuffer, layer, NULL);
   emit_copy_image_layer_per_tile_list(job, dst, src, layer, region);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_copy_image_rcl(struct v3dv_job *job,
                    struct v3dv_image *dst,
                    struct v3dv_image *src,
                    struct fake_framebuffer *framebuffer,
                    const VkImageCopy *region)
{
   struct v3dv_cl *rcl = emit_rcl_prologue(job, framebuffer, NULL, NULL,
                                           region->dstSubresource.aspectMask,
                                           0, 0);
   for (int layer = 0; layer < framebuffer->fb.layers; layer++)
      emit_copy_image_layer(job, dst, src, framebuffer, layer, region);
   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
copy_image_tlb(struct v3dv_cmd_buffer *cmd_buffer,
               struct v3dv_image *dst,
               struct v3dv_image *src,
               const VkImageCopy *region)
{
   assert(can_use_tlb_copy_for_image_offset(&region->dstOffset));

   /* From the Vulkan spec, VkImageCopy valid usage:
    *
    *    "If neither the calling command’s srcImage nor the calling command’s
    *     dstImage has a multi-planar image format then the aspectMask member
    *     of srcSubresource and dstSubresource must match."
    */
   assert(region->dstSubresource.aspectMask ==
          region->srcSubresource.aspectMask);
   uint32_t internal_type, internal_bpp;
   get_internal_type_bpp_for_image_aspects(dst,
                                           region->dstSubresource.aspectMask,
                                           &internal_type, &internal_bpp);

   /* From the Vulkan spec, VkImageCopy valid usage:
    *
    *   "The number of slices of the extent (for 3D) or layers of the
    *    srcSubresource (for non-3D) must match the number of slices of
    *    the extent (for 3D) or layers of the dstSubresource (for non-3D)."
    */
   assert(region->srcSubresource.layerCount ==
          region->dstSubresource.layerCount);
   uint32_t num_layers = region->dstSubresource.layerCount;
   assert(num_layers > 0);

   struct fake_framebuffer framebuffer;
   setup_framebuffer_params(&framebuffer,
                            region->extent.width, region->extent.height,
                            num_layers, internal_bpp, internal_type);

   /* Limit supertile coverage to the requested region  */
   uint32_t supertile_w_in_pixels =
      framebuffer.fb.tile_width * framebuffer.fb.supertile_width;
   uint32_t supertile_h_in_pixels =
      framebuffer.fb.tile_height * framebuffer.fb.supertile_height;
   const uint32_t max_render_x = region->extent.width - 1;
   const uint32_t max_render_y = region->extent.height - 1;

   assert(region->dstOffset.x == 0 && region->dstOffset.y == 0);
   framebuffer.min_x_supertile = 0;
   framebuffer.min_y_supertile = 0;
   framebuffer.max_x_supertile = max_render_x / supertile_w_in_pixels;
   framebuffer.max_y_supertile = max_render_y / supertile_h_in_pixels;

   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, false);
   v3dv_cmd_buffer_start_frame(cmd_buffer, &framebuffer.fb);

   v3dv_job_emit_binning_flush(job);
   emit_copy_image_rcl(job, dst, src, &framebuffer, region);

   v3dv_cmd_buffer_finish_job(cmd_buffer);
}

void
v3dv_CmdCopyImage(VkCommandBuffer commandBuffer,
                  VkImage srcImage,
                  VkImageLayout srcImageLayout,
                  VkImage dstImage,
                  VkImageLayout dstImageLayout,
                  uint32_t regionCount,
                  const VkImageCopy *pRegions)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_image, src, srcImage);
   V3DV_FROM_HANDLE(v3dv_image, dst, dstImage);

   for (uint32_t i = 0; i < regionCount; i++) {
      if (can_use_tlb_copy_for_image_offset(&pRegions[i].dstOffset) &&
          can_use_tlb_copy_for_image_offset(&pRegions[i].srcOffset)) {
         copy_image_tlb(cmd_buffer, dst, src, &pRegions[i]);
      } else {
         assert(!"Fallback path for vkCopyImageToImage not implemented");
      }
   }
}

static void
emit_clear_image_per_tile_list(struct v3dv_job *job,
                               struct v3dv_image *image,
                               VkImageAspectFlags aspects,
                               uint32_t layer,
                               uint32_t level)
{
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   emit_image_store(cl, image, aspects, layer, level, false, false);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_clear_image(struct v3dv_job *job,
                 struct v3dv_image *image,
                 struct fake_framebuffer *framebuffer,
                 VkImageAspectFlags aspects,
                 uint32_t layer,
                 uint32_t level)
{
   emit_clear_image_per_tile_list(job, image, aspects, layer, level);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_clear_image_rcl(struct v3dv_job *job,
                     struct v3dv_image *image,
                     struct fake_framebuffer *framebuffer,
                     const union v3dv_clear_value *clear_value,
                     VkImageAspectFlags aspects,
                     uint32_t layer,
                     uint32_t level)
{
   struct v3dv_cl *rcl = emit_rcl_prologue(job, framebuffer, clear_value,
                                           image, aspects, layer, level);
   emit_frame_setup(job, framebuffer, 0, clear_value);
   emit_clear_image(job, image, framebuffer, aspects, layer, level);
   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
clear_image_tlb(struct v3dv_cmd_buffer *cmd_buffer,
                struct v3dv_image *image,
                const VkClearValue *clear_value,
                const VkImageSubresourceRange *range)
{
   uint32_t internal_type, internal_bpp;
   get_internal_type_bpp_for_image_aspects(image, range->aspectMask,
                                           &internal_type, &internal_bpp);

   union v3dv_clear_value hw_clear_value = { 0 };
   if (range->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
      uint32_t internal_size = 4 << internal_bpp;
      v3dv_get_hw_clear_color(&clear_value->color, internal_type, internal_size,
                              hw_clear_value.color);
   } else {
      assert((range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
             (range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT));
      hw_clear_value.z = clear_value->depthStencil.depth;
      hw_clear_value.s = clear_value->depthStencil.stencil;
   }

   uint32_t layer_count = range->layerCount == VK_REMAINING_ARRAY_LAYERS ?
                          image->array_size : range->layerCount;

   uint32_t level_count = range->levelCount == VK_REMAINING_MIP_LEVELS ?
                          image->levels : range->levelCount;
   uint32_t min_layer = range->baseArrayLayer;
   uint32_t max_layer = range->baseArrayLayer + layer_count;
   uint32_t min_level = range->baseMipLevel;
   uint32_t max_level = range->baseMipLevel + level_count;

   for (uint32_t layer = min_layer; layer < max_layer; layer++) {
      for (uint32_t level = min_level; level < max_level; level++) {
         uint32_t width = u_minify(image->extent.width, level);
         uint32_t height = u_minify(image->extent.height, level);

         struct fake_framebuffer framebuffer;
         setup_framebuffer_params(&framebuffer, width, height, 1,
                                  internal_bpp, internal_type);

         struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, false);
         v3dv_cmd_buffer_start_frame(cmd_buffer, &framebuffer.fb);
         v3dv_job_emit_binning_flush(job);

         /* If this triggers it is an application bug: the spec requires
          * that any aspects to clear are present in the image.
          */
         assert(range->aspectMask & image->aspects);

         emit_clear_image_rcl(job, image, &framebuffer, &hw_clear_value,
                             range->aspectMask, layer, level);

         v3dv_cmd_buffer_finish_job(cmd_buffer);
      }
   }
}

void
v3dv_CmdClearColorImage(VkCommandBuffer commandBuffer,
                        VkImage _image,
                        VkImageLayout imageLayout,
                        const VkClearColorValue *pColor,
                        uint32_t rangeCount,
                        const VkImageSubresourceRange *pRanges)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_image, image, _image);

   const VkClearValue clear_value = {
      .color = *pColor,
   };

   for (uint32_t i = 0; i < rangeCount; i++)
      clear_image_tlb(cmd_buffer, image, &clear_value, &pRanges[i]);
}

void
v3dv_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer,
                               VkImage _image,
                               VkImageLayout imageLayout,
                               const VkClearDepthStencilValue *pDepthStencil,
                               uint32_t rangeCount,
                               const VkImageSubresourceRange *pRanges)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_image, image, _image);

   const VkClearValue clear_value = {
      .depthStencil = *pDepthStencil,
   };

   for (uint32_t i = 0; i < rangeCount; i++)
      clear_image_tlb(cmd_buffer, image, &clear_value, &pRanges[i]);
}

static void
emit_copy_buffer_per_tile_list(struct v3dv_job *job,
                               struct v3dv_bo *dst,
                               struct v3dv_bo *src,
                               uint32_t dst_offset,
                               uint32_t src_offset,
                               uint32_t stride,
                               uint32_t format)
{
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   emit_linear_load(cl, RENDER_TARGET_0, src, src_offset, stride, format);

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   emit_linear_store(cl, RENDER_TARGET_0,
                     dst, dst_offset, stride, false, format);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_copy_buffer(struct v3dv_job *job,
                 struct v3dv_bo *dst,
                 struct v3dv_bo *src,
                 uint32_t dst_offset,
                 uint32_t src_offset,
                 struct fake_framebuffer *framebuffer,
                 uint32_t format)
{
   const uint32_t stride = framebuffer->fb.width * 4;
   emit_copy_buffer_per_tile_list(job, dst, src,
                                  dst_offset, src_offset,
                                  stride, format);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_copy_buffer_rcl(struct v3dv_job *job,
                     struct v3dv_bo *dst,
                     struct v3dv_bo *src,
                     uint32_t dst_offset,
                     uint32_t src_offset,
                     struct fake_framebuffer *framebuffer,
                     uint32_t format)
{
   struct v3dv_cl *rcl = emit_rcl_prologue(job, framebuffer, NULL, NULL,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
   emit_frame_setup(job, framebuffer, 0, NULL);
   emit_copy_buffer(job, dst, src, dst_offset, src_offset, framebuffer, format);
   cl_emit(rcl, END_OF_RENDERING, end);
}

/* Figure out a TLB size configuration for a number of pixels to process.
 * Beware that we can't "render" more than 4096x4096 pixels in a single job,
 * if the pixel count is larger than this, the caller might need to split
 * the job and call this function multiple times.
 */
static void
setup_framebuffer_for_pixel_count(struct fake_framebuffer *framebuffer,
                                  uint32_t num_pixels,
                                  uint32_t internal_bpp,
                                  uint32_t internal_type)
{
   const uint32_t max_dim_pixels = 4096;
   const uint32_t max_pixels = max_dim_pixels * max_dim_pixels;

   uint32_t w, h;
   if (num_pixels > max_pixels) {
      w = max_dim_pixels;
      h = max_dim_pixels;
   } else {
      w = num_pixels;
      h = 1;
      while (w > max_dim_pixels || ((w % 2) == 0 && w > 2 * h)) {
         w >>= 1;
         h <<= 1;
      }
   }
   assert(w <= max_dim_pixels && h <= max_dim_pixels);
   assert(w * h <= num_pixels);

   /* Skip tiling calculations if the framebuffer setup has not changed */
   if (w != framebuffer->fb.width ||
       h != framebuffer->fb.height ||
       internal_bpp != framebuffer->fb.internal_bpp ||
       internal_type != framebuffer->internal_type) {
      setup_framebuffer_params(framebuffer, w, h, 1,
                               internal_bpp, internal_type);
   }
}

static struct v3dv_job *
copy_buffer(struct v3dv_cmd_buffer *cmd_buffer,
            struct v3dv_bo *dst,
            struct v3dv_bo *src,
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
   assert(num_items > 0);

   struct v3dv_job *job;
   uint32_t src_offset = region->srcOffset;
   uint32_t dst_offset = region->dstOffset;
   struct fake_framebuffer framebuffer = { .fb.width = 0 };
   while (num_items > 0) {
      setup_framebuffer_for_pixel_count(&framebuffer, num_items,
                                        internal_bpp, internal_type);

      job = v3dv_cmd_buffer_start_job(cmd_buffer, false);
      v3dv_cmd_buffer_start_frame(cmd_buffer, &framebuffer.fb);

      v3dv_job_emit_binning_flush(job);

      emit_copy_buffer_rcl(job, dst, src, dst_offset, src_offset,
                           &framebuffer, format);

      v3dv_cmd_buffer_finish_job(cmd_buffer);

      const uint32_t items_copied = framebuffer.fb.width * framebuffer.fb.height;
      const uint32_t bytes_copied = items_copied * item_size;
      num_items -= items_copied;
      src_offset += bytes_copied;
      dst_offset += bytes_copied;
   }

   return job;
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

   for (uint32_t i = 0; i < regionCount; i++) {
     copy_buffer(cmd_buffer, dst_buffer->mem->bo, src_buffer->mem->bo,
                 &pRegions[i]);
   }
}

void
v3dv_CmdUpdateBuffer(VkCommandBuffer commandBuffer,
                     VkBuffer dstBuffer,
                     VkDeviceSize dstOffset,
                     VkDeviceSize dataSize,
                     const void *pData)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_buffer, dst_buffer, dstBuffer);

   struct v3dv_bo *src_bo =
      v3dv_bo_alloc(cmd_buffer->device, dataSize, "vkCmdUpdateBuffer");
   if (!src_bo) {
      fprintf(stderr, "Failed to allocate BO for vkCmdUpdateBuffer.\n");
      return;
   }

   bool ok = v3dv_bo_map(cmd_buffer->device, src_bo, src_bo->size);
   if (!ok) {
      fprintf(stderr, "Failed to map BO for vkCmdUpdateBuffer.\n");
      return;
   }

   memcpy(src_bo->map, pData, dataSize);

   v3dv_bo_unmap(cmd_buffer->device, src_bo);

   VkBufferCopy region = {
      .srcOffset = 0,
      .dstOffset = dstOffset,
      .size = dataSize,
   };
   struct v3dv_job *copy_job =
      copy_buffer(cmd_buffer, dst_buffer->mem->bo, src_bo, &region);

   /* Make sure we add the BO to the list of extra BOs so it is not leaked.
    * If the copy job was split into multiple jobs, we just bind it to the last
    * one.
    */
   v3dv_job_add_extra_bo(copy_job, src_bo);
}

static void
emit_fill_buffer_per_tile_list(struct v3dv_job *job,
                               struct v3dv_bo *bo,
                               uint32_t offset,
                               uint32_t stride)
{
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   emit_linear_store(cl, RENDER_TARGET_0, bo, offset, stride, false,
                     V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_fill_buffer(struct v3dv_job *job,
                 struct v3dv_bo *bo,
                 uint32_t offset,
                 struct fake_framebuffer *framebuffer)
{
   const uint32_t stride = framebuffer->fb.width * 4;
   emit_fill_buffer_per_tile_list(job, bo, offset, stride);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_fill_buffer_rcl(struct v3dv_job *job,
                     struct v3dv_bo *bo,
                     uint32_t offset,
                     struct fake_framebuffer *framebuffer,
                     uint32_t data)
{
   const union v3dv_clear_value clear_value = {
       .color = { data, 0, 0, 0 },
   };

   struct v3dv_cl *rcl = emit_rcl_prologue(job, framebuffer, &clear_value, NULL,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
   emit_frame_setup(job, framebuffer, 0, &clear_value);
   emit_fill_buffer(job, bo, offset, framebuffer);
   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
fill_buffer(struct v3dv_cmd_buffer *cmd_buffer,
            struct v3dv_bo *bo,
            uint32_t offset,
            uint32_t size,
            uint32_t data)
{
   assert(size > 0 && size % 4 == 0);
   assert(offset + size <= bo->size);

   const uint32_t internal_bpp = V3D_INTERNAL_BPP_32;
   const uint32_t internal_type = V3D_INTERNAL_TYPE_8UI;
   uint32_t num_items = size / 4;

   struct fake_framebuffer framebuffer = { .fb.width = 0 };
   while (num_items > 0) {
      setup_framebuffer_for_pixel_count(&framebuffer, num_items,
                                        internal_bpp, internal_type);

      struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, false);
      v3dv_cmd_buffer_start_frame(cmd_buffer, &framebuffer.fb);

      v3dv_job_emit_binning_flush(job);

      emit_fill_buffer_rcl(job, bo, offset, &framebuffer, data);

      v3dv_cmd_buffer_finish_job(cmd_buffer);

      const uint32_t items_copied = framebuffer.fb.width * framebuffer.fb.height;
      const uint32_t bytes_copied = items_copied * 4;
      num_items -= items_copied;
      offset += bytes_copied;
   }
}

void
v3dv_CmdFillBuffer(VkCommandBuffer commandBuffer,
                   VkBuffer dstBuffer,
                   VkDeviceSize dstOffset,
                   VkDeviceSize size,
                   uint32_t data)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_buffer, dst_buffer, dstBuffer);

   struct v3dv_bo *bo = dst_buffer->mem->bo;

   /* From the Vulkan spec:
    *
    *   "If VK_WHOLE_SIZE is used and the remaining size of the buffer is not
    *    a multiple of 4, then the nearest smaller multiple is used."
    */
   if (size == VK_WHOLE_SIZE) {
      size = dst_buffer->mem->bo->size;
      size -= size % 4;
   }

   fill_buffer(cmd_buffer, bo, dstOffset, size, data);
}

static void
emit_copy_buffer_to_layer_per_tile_list(struct v3dv_job *job,
                                        struct v3dv_image *image,
                                        struct v3dv_buffer *buffer,
                                        uint32_t layer,
                                        const VkBufferImageCopy *region)
{
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   const VkImageSubresourceLayers *imgrsc = &region->imageSubresource;
   assert(layer < imgrsc->layerCount);

   /* Load TLB from buffer */
   uint32_t width, height;
   if (region->bufferRowLength == 0)
      width = region->imageExtent.width;
   else
      width = region->bufferRowLength;

   if (region->bufferImageHeight == 0)
      height = region->imageExtent.height;
   else
      height = region->bufferImageHeight;

   uint32_t cpp = imgrsc->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT ?
                  1 : image->cpp;
   uint32_t buffer_stride = width * cpp;
   uint32_t buffer_offset =
      region->bufferOffset + height * buffer_stride * layer;

   uint32_t format = choose_tlb_format(image, imgrsc->aspectMask,
                                       false, false, true);

   emit_linear_load(cl, RENDER_TARGET_0, buffer->mem->bo,
                    buffer_offset, buffer_stride, format);

   /* Because we can't do raster loads/stores of Z/S formats we need to
    * use a color tile buffer with a compatible RGBA color format instead.
    * However, when we are uploading a single aspect to a combined
    * depth/stencil image we have the problem that our tile buffer stores don't
    * allow us to mask out the other aspect, so we always write all four RGBA
    * channels to the image and we end up overwriting that other aspect with
    * undefined values. To work around that, we first load the aspect we are
    * not copying from the image memory into a proper Z/S tile buffer. Then we
    * do our store from the color buffer for the aspect we are copying, and
    * after that, we do another store from the Z/S tile buffer to restore the
    * other aspect to its original value.
    */
   if (image->vk_format == VK_FORMAT_D24_UNORM_S8_UINT) {
      if (imgrsc->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         emit_image_load(cl, image, VK_IMAGE_ASPECT_STENCIL_BIT,
                         imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                         false, false);
      } else {
         assert(imgrsc->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
         emit_image_load(cl, image, VK_IMAGE_ASPECT_DEPTH_BIT,
                         imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                         false, false);
      }
   }

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   /* Store TLB to image */
   emit_image_store(cl, image, imgrsc->aspectMask,
                    imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                    false, true);

   if (image->vk_format == VK_FORMAT_D24_UNORM_S8_UINT) {
      if (imgrsc->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         emit_image_store(cl, image, VK_IMAGE_ASPECT_STENCIL_BIT,
                          imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                          false, false);
      } else {
         assert(imgrsc->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
         emit_image_store(cl, image, VK_IMAGE_ASPECT_DEPTH_BIT,
                          imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                          false, false);
      }
   }

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
emit_copy_buffer_to_layer(struct v3dv_job *job,
                          struct v3dv_image *image,
                          struct v3dv_buffer *buffer,
                          struct fake_framebuffer *framebuffer,
                          uint32_t layer,
                          const VkBufferImageCopy *region)
{
   emit_frame_setup(job, framebuffer, layer, NULL);
   emit_copy_buffer_to_layer_per_tile_list(job, image, buffer, layer, region);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_copy_buffer_to_image_rcl(struct v3dv_job *job,
                              struct v3dv_image *image,
                              struct v3dv_buffer *buffer,
                              struct fake_framebuffer *framebuffer,
                              const VkBufferImageCopy *region)
{
   struct v3dv_cl *rcl = emit_rcl_prologue(job, framebuffer, NULL, NULL,
                                           region->imageSubresource.aspectMask,
                                           0, 0);
   for (int layer = 0; layer < framebuffer->fb.layers; layer++)
      emit_copy_buffer_to_layer(job, image, buffer, framebuffer, layer, region);
   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
copy_buffer_to_image_tlb(struct v3dv_cmd_buffer *cmd_buffer,
                         struct v3dv_image *image,
                         struct v3dv_buffer *buffer,
                         const VkBufferImageCopy *region)
{
   assert(can_use_tlb_copy_for_image_offset(&region->imageOffset));

   uint32_t internal_type, internal_bpp;
   get_internal_type_bpp_for_image_aspects(image,
                                           region->imageSubresource.aspectMask,
                                           &internal_type, &internal_bpp);

   uint32_t num_layers = region->imageSubresource.layerCount;
   assert(num_layers > 0);

   struct fake_framebuffer framebuffer;
   setup_framebuffer_params(&framebuffer,
                            region->imageExtent.width,
                            region->imageExtent.height,
                            num_layers, internal_bpp, internal_type);

   /* Limit supertile coverage to the requested region  */
   uint32_t supertile_w_in_pixels =
      framebuffer.fb.tile_width * framebuffer.fb.supertile_width;
   uint32_t supertile_h_in_pixels =
      framebuffer.fb.tile_height * framebuffer.fb.supertile_height;
   const uint32_t max_render_x =
      region->imageOffset.x + region->imageExtent.width - 1;
   const uint32_t max_render_y =
      region->imageOffset.y + region->imageExtent.height - 1;

   assert(region->imageOffset.x == 0 && region->imageOffset.y == 0);
   framebuffer.min_x_supertile = 0;
   framebuffer.min_y_supertile = 0;
   framebuffer.max_x_supertile = max_render_x / supertile_w_in_pixels;
   framebuffer.max_y_supertile = max_render_y / supertile_h_in_pixels;

   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, false);
   v3dv_cmd_buffer_start_frame(cmd_buffer, &framebuffer.fb);

   v3dv_job_emit_binning_flush(job);
   emit_copy_buffer_to_image_rcl(job, image, buffer, &framebuffer, region);

   v3dv_cmd_buffer_finish_job(cmd_buffer);
}

void
v3dv_CmdCopyBufferToImage(VkCommandBuffer commandBuffer,
                          VkBuffer srcBuffer,
                          VkImage dstImage,
                          VkImageLayout dstImageLayout,
                          uint32_t regionCount,
                          const VkBufferImageCopy *pRegions)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, srcBuffer);
   V3DV_FROM_HANDLE(v3dv_image, image, dstImage);

   for (uint32_t i = 0; i < regionCount; i++) {
      if (can_use_tlb_copy_for_image_offset(&pRegions[i].imageOffset))
         copy_buffer_to_image_tlb(cmd_buffer, image, buffer, &pRegions[i]);
      else
         assert(!"Fallback path for vkCmdCopyBufferToImage not implemented");
   }
}

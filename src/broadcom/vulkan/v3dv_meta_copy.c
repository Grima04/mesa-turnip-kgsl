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
#include "util/u_pack_color.h"

/**
 * Copy operations implemented in this file don't operate on a framebuffer
 * object provided by the user, however, since most use the TLB for this,
 * we still need to have some representation of the framebuffer. For the most
 * part, the job's frame tiling information is enough for this, however we
 * still need additional information such us the internal type of our single
 * render target, so we use this auxiliary struct to pass that information
 * around.
 */
struct framebuffer_data {
   /* The internal type of the single render target */
   uint32_t internal_type;

   /* Supertile coverage */
   uint32_t min_x_supertile;
   uint32_t min_y_supertile;
   uint32_t max_x_supertile;
   uint32_t max_y_supertile;

   /* Format info */
   VkFormat vk_format;
   const struct v3dv_format *format;
};

static void
setup_framebuffer_data(struct framebuffer_data *fb,
                       VkFormat vk_format,
                       uint32_t internal_type,
                       const struct v3dv_frame_tiling *tiling)
{
   fb->internal_type = internal_type;

   /* Supertile coverage always starts at 0,0  */
   uint32_t supertile_w_in_pixels =
      tiling->tile_width * tiling->supertile_width;
   uint32_t supertile_h_in_pixels =
      tiling->tile_height * tiling->supertile_height;

   fb->min_x_supertile = 0;
   fb->min_y_supertile = 0;
   fb->max_x_supertile = (tiling->width - 1) / supertile_w_in_pixels;
   fb->max_y_supertile = (tiling->height - 1) / supertile_h_in_pixels;

   fb->vk_format = vk_format;
   fb->format = v3dv_get_format(vk_format);
}

/* This chooses a tile buffer format that is appropriate for the copy operation.
 * Typically, this is the image render target type, however, if we are copying
 * depth/stencil to/from a buffer the hardware can't do raster loads/stores, so
 * we need to load and store to/from a tile color buffer using a compatible
 * color format.
 */
static uint32_t
choose_tlb_format(struct framebuffer_data *framebuffer,
                  VkImageAspectFlags aspect,
                  bool for_store,
                  bool is_copy_to_buffer,
                  bool is_copy_from_buffer)
{
   if (is_copy_to_buffer || is_copy_from_buffer) {
      switch (framebuffer->vk_format) {
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
         return framebuffer->format->rt_type;
         break;
      }
   } else {
      return framebuffer->format->rt_type;
   }
}

static inline bool
format_needs_rb_swap(VkFormat format)
{
   const uint8_t *swizzle = v3dv_get_format_swizzle(format);
   return swizzle[0] == PIPE_SWIZZLE_Z;
}

static void
get_internal_type_bpp_for_image_aspects(VkFormat vk_format,
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
      switch (vk_format) {
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
      const struct v3dv_format *format = v3dv_get_format(vk_format);
      v3dv_get_internal_type_bpp_for_output_format(format->rt_type,
                                                   internal_type,
                                                   internal_bpp);
   }
}

struct rcl_clear_info {
   const union v3dv_clear_value *clear_value;
   struct v3dv_image *image;
   VkImageAspectFlags aspects;
   uint32_t layer;
   uint32_t level;
};

static struct v3dv_cl *
emit_rcl_prologue(struct v3dv_job *job,
                  uint32_t rt_internal_type,
                  const struct rcl_clear_info *clear_info)
{
   const struct v3dv_frame_tiling *tiling = &job->frame_tiling;

   struct v3dv_cl *rcl = &job->rcl;
   v3dv_cl_ensure_space_with_branch(rcl, 200 +
                                    tiling->layers * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.early_z_disable = true;
      config.image_width_pixels = tiling->width;
      config.image_height_pixels = tiling->height;
      config.number_of_render_targets = 1;
      config.multisample_mode_4x = false;
      config.maximum_bpp_of_all_render_targets = tiling->internal_bpp;
   }

   if (clear_info && (clear_info->aspects & VK_IMAGE_ASPECT_COLOR_BIT)) {
      uint32_t clear_pad = 0;
      if (clear_info->image) {
         const struct v3dv_image *image = clear_info->image;
         const struct v3d_resource_slice *slice =
            &image->slices[clear_info->level];
         if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
             slice->tiling == VC5_TILING_UIF_XOR) {
            int uif_block_height = v3d_utile_height(image->cpp) * 2;

            uint32_t implicit_padded_height =
               align(tiling->height, uif_block_height) / uif_block_height;

            if (slice->padded_height_of_output_image_in_uif_blocks -
                implicit_padded_height >= 15) {
               clear_pad = slice->padded_height_of_output_image_in_uif_blocks;
            }
         }
      }

      const uint32_t *color = &clear_info->clear_value->color[0];
      cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1, clear) {
         clear.clear_color_low_32_bits = color[0];
         clear.clear_color_next_24_bits = color[1] & 0x00ffffff;
         clear.render_target_number = 0;
      };

      if (tiling->internal_bpp >= V3D_INTERNAL_BPP_64) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART2, clear) {
            clear.clear_color_mid_low_32_bits =
              ((color[1] >> 24) | (color[2] << 8));
            clear.clear_color_mid_high_24_bits =
              ((color[2] >> 24) | ((color[3] & 0xffff) << 8));
            clear.render_target_number = 0;
         };
      }

      if (tiling->internal_bpp >= V3D_INTERNAL_BPP_128 || clear_pad) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART3, clear) {
            clear.uif_padded_height_in_uif_blocks = clear_pad;
            clear.clear_color_high_16_bits = color[3] >> 16;
            clear.render_target_number = 0;
         };
      }
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
      rt.render_target_0_internal_bpp = tiling->internal_bpp;
      rt.render_target_0_internal_type = rt_internal_type;
      rt.render_target_0_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
      clear.z_clear_value = clear_info ? clear_info->clear_value->z : 1.0f;
      clear.stencil_clear_value = clear_info ? clear_info->clear_value->s : 0;
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
                 uint32_t layer,
                 const union v3dv_clear_value *clear_value)
{
   const struct v3dv_frame_tiling *tiling = &job->frame_tiling;

   struct v3dv_cl *rcl = &job->rcl;

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
                           struct framebuffer_data *framebuffer)
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
                struct framebuffer_data *framebuffer,
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

      load.input_image_format = choose_tlb_format(framebuffer, aspect, false,
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
         (framebuffer->vk_format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
          (framebuffer->vk_format == VK_FORMAT_D24_UNORM_S8_UINT &&
           (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)))) {
         needs_rb_swap = true;
         needs_chan_reverse = true;
      } else if (!is_copy_from_buffer && !is_copy_to_buffer &&
                 (aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
         /* This is not a raw data copy (i.e. we are clearing the image),
          * so we need to make sure we respect the format swizzle.
          */
         needs_rb_swap = format_needs_rb_swap(framebuffer->vk_format);
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
                 struct framebuffer_data *framebuffer,
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
         (framebuffer->vk_format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
          (framebuffer->vk_format == VK_FORMAT_D24_UNORM_S8_UINT &&
           (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)))) {
         needs_rb_swap = true;
         needs_chan_reverse = true;
      } else if (!is_copy_from_buffer && !is_copy_to_buffer &&
                 (aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
         needs_rb_swap = format_needs_rb_swap(framebuffer->vk_format);
      }

      store.r_b_swap = needs_rb_swap;
      store.channel_reverse = needs_chan_reverse;

      store.output_image_format = choose_tlb_format(framebuffer, aspect, true,
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
                                        struct framebuffer_data *framebuffer,
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
   assert((image->type != VK_IMAGE_TYPE_3D && layer < imgrsc->layerCount) ||
          layer < image->extent.depth);

   /* Load image to TLB */
   emit_image_load(cl, framebuffer, image, imgrsc->aspectMask,
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

   uint32_t format = choose_tlb_format(framebuffer, imgrsc->aspectMask,
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
                          struct framebuffer_data *framebuffer,
                          uint32_t layer,
                          const VkBufferImageCopy *region)
{
   emit_frame_setup(job, layer, NULL);
   emit_copy_layer_to_buffer_per_tile_list(job, framebuffer, buffer,
                                           image, layer, region);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_copy_image_to_buffer_rcl(struct v3dv_job *job,
                              struct v3dv_buffer *buffer,
                              struct v3dv_image *image,
                              struct framebuffer_data *framebuffer,
                              const VkBufferImageCopy *region)
{
   struct v3dv_cl *rcl =
      emit_rcl_prologue(job, framebuffer->internal_type, NULL);
   for (int layer = 0; layer < job->frame_tiling.layers; layer++)
      emit_copy_layer_to_buffer(job, buffer, image, framebuffer, layer, region);
   cl_emit(rcl, END_OF_RENDERING, end);
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
                         VkFormat fb_format,
                         const VkBufferImageCopy *region)
{
   uint32_t internal_type, internal_bpp;
   get_internal_type_bpp_for_image_aspects(fb_format,
                                           region->imageSubresource.aspectMask,
                                           &internal_type, &internal_bpp);

   uint32_t num_layers;
   if (image->type != VK_IMAGE_TYPE_3D)
      num_layers = region->imageSubresource.layerCount;
   else
      num_layers = region->imageExtent.depth;
   assert(num_layers > 0);

   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, -1);
   if (!job)
      return;

   v3dv_job_start_frame(job,
                        region->imageExtent.width,
                        region->imageExtent.height,
                        num_layers, 1, internal_bpp);

   struct framebuffer_data framebuffer;
   setup_framebuffer_data(&framebuffer, fb_format, internal_type,
                          &job->frame_tiling);

   v3dv_job_emit_binning_flush(job);
   emit_copy_image_to_buffer_rcl(job, buffer, image, &framebuffer, region);

   v3dv_cmd_buffer_finish_job(cmd_buffer);
}

static VkFormat
get_compatible_tlb_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_SNORM:
      return VK_FORMAT_R8G8B8A8_UINT;

   case VK_FORMAT_R8G8_SNORM:
      return VK_FORMAT_R8G8_UINT;

   case VK_FORMAT_R8_SNORM:
      return VK_FORMAT_R8_UINT;

   case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
      return VK_FORMAT_A8B8G8R8_UINT_PACK32;

   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
      return VK_FORMAT_R16_UINT;

   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
      return VK_FORMAT_R16G16_UINT;

   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
      return VK_FORMAT_R16G16B16A16_UINT;

   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
      return VK_FORMAT_R32_SFLOAT;

   default:
      return VK_FORMAT_UNDEFINED;
   }
}

static inline bool
can_use_tlb(struct v3dv_image *image,
            const VkOffset3D *offset,
            VkFormat *compat_format)
{
   if (offset->x != 0 || offset->y != 0)
      return false;

   if (image->format->rt_type != V3D_OUTPUT_IMAGE_FORMAT_NO) {
      if (compat_format)
         *compat_format = image->vk_format;
      return true;
   }

   /* If the image format is not TLB-supported, then check if we can use
    * a compatible format instead.
    */
   if (compat_format) {
      *compat_format = get_compatible_tlb_format(image->vk_format);
      if (*compat_format != VK_FORMAT_UNDEFINED)
         return true;
   }

   return false;
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

   VkFormat compat_format;
   for (uint32_t i = 0; i < regionCount; i++) {
      if (can_use_tlb(image, &pRegions[i].imageOffset, &compat_format)) {
         copy_image_to_buffer_tlb(cmd_buffer, buffer, image, compat_format,
                                  &pRegions[i]);
      } else {
         assert(!"Fallback path for vkCopyImageToBuffer not implemented");
      }
   }
}

static void
emit_copy_image_layer_per_tile_list(struct v3dv_job *job,
                                    struct framebuffer_data *framebuffer,
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
   assert((src->type != VK_IMAGE_TYPE_3D && layer < srcrsc->layerCount) ||
          layer < src->extent.depth);

   emit_image_load(cl, framebuffer, src, srcrsc->aspectMask,
                   srcrsc->baseArrayLayer + layer, srcrsc->mipLevel,
                   false, false);

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   const VkImageSubresourceLayers *dstrsc = &region->dstSubresource;
   assert((dst->type != VK_IMAGE_TYPE_3D && layer < dstrsc->layerCount) ||
          layer < dst->extent.depth);

   emit_image_store(cl, framebuffer, dst, dstrsc->aspectMask,
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
                      struct framebuffer_data *framebuffer,
                      uint32_t layer,
                      const VkImageCopy *region)
{
   emit_frame_setup(job, layer, NULL);
   emit_copy_image_layer_per_tile_list(job, framebuffer, dst, src, layer, region);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_copy_image_rcl(struct v3dv_job *job,
                    struct v3dv_image *dst,
                    struct v3dv_image *src,
                    struct framebuffer_data *framebuffer,
                    const VkImageCopy *region)
{
   struct v3dv_cl *rcl =
      emit_rcl_prologue(job, framebuffer->internal_type, NULL);
   for (int layer = 0; layer < job->frame_tiling.layers; layer++)
      emit_copy_image_layer(job, dst, src, framebuffer, layer, region);
   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
copy_image_tlb(struct v3dv_cmd_buffer *cmd_buffer,
               struct v3dv_image *dst,
               struct v3dv_image *src,
               VkFormat fb_format,
               const VkImageCopy *region)
{
   /* From the Vulkan spec, VkImageCopy valid usage:
    *
    *    "If neither the calling command’s srcImage nor the calling command’s
    *     dstImage has a multi-planar image format then the aspectMask member
    *     of srcSubresource and dstSubresource must match."
    */
   assert(region->dstSubresource.aspectMask ==
          region->srcSubresource.aspectMask);
   uint32_t internal_type, internal_bpp;
   get_internal_type_bpp_for_image_aspects(fb_format,
                                           region->dstSubresource.aspectMask,
                                           &internal_type, &internal_bpp);

   /* From the Vulkan spec, VkImageCopy valid usage:
    *
    * "The layerCount member of srcSubresource and dstSubresource must match"
    */
   assert(region->srcSubresource.layerCount ==
          region->dstSubresource.layerCount);
   uint32_t num_layers;
   if (dst->type != VK_IMAGE_TYPE_3D)
      num_layers = region->dstSubresource.layerCount;
   else
      num_layers = region->extent.depth;
   assert(num_layers > 0);

   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, -1);
   if (!job)
      return;

   v3dv_job_start_frame(job,
                        region->extent.width,
                        region->extent.height,
                        num_layers, 1, internal_bpp);

   struct framebuffer_data framebuffer;
   setup_framebuffer_data(&framebuffer, fb_format, internal_type,
                          &job->frame_tiling);

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

   VkFormat compat_format;
   for (uint32_t i = 0; i < regionCount; i++) {
      if (can_use_tlb(src, &pRegions[i].srcOffset, &compat_format) &&
          can_use_tlb(dst, &pRegions[i].dstOffset, &compat_format)) {
         copy_image_tlb(cmd_buffer, dst, src, compat_format, &pRegions[i]);
      } else {
         assert(!"Fallback path for vkCopyImageToImage not implemented");
      }
   }
}

static void
emit_clear_image_per_tile_list(struct v3dv_job *job,
                               struct framebuffer_data *framebuffer,
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

   emit_image_store(cl, framebuffer, image, aspects, layer, level, false, false);

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
                 struct framebuffer_data *framebuffer,
                 VkImageAspectFlags aspects,
                 uint32_t layer,
                 uint32_t level)
{
   emit_clear_image_per_tile_list(job, framebuffer, image, aspects, layer, level);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_clear_image_rcl(struct v3dv_job *job,
                     struct v3dv_image *image,
                     struct framebuffer_data *framebuffer,
                     const union v3dv_clear_value *clear_value,
                     VkImageAspectFlags aspects,
                     uint32_t layer,
                     uint32_t level)
{
   const struct rcl_clear_info clear_info = {
      .clear_value = clear_value,
      .image = image,
      .aspects = aspects,
      .layer = layer,
      .level = level,
   };

   struct v3dv_cl *rcl =
      emit_rcl_prologue(job, framebuffer->internal_type, &clear_info);
   emit_frame_setup(job, 0, clear_value);
   emit_clear_image(job, image, framebuffer, aspects, layer, level);
   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
get_hw_clear_color(const VkClearColorValue *color,
                   VkFormat fb_format,
                   VkFormat image_format,
                   uint32_t internal_type,
                   uint32_t internal_bpp,
                   uint32_t *hw_color)
{
   const uint32_t internal_size = 4 << internal_bpp;

   /* If the image format doesn't match the framebuffer format, then we are
    * trying to clear an unsupported tlb format using a compatible
    * format for the framebuffer. In this case, we want to make sure that
    * we pack the clear value according to the original format semantics,
    * not the compatible format.
    */
   if (fb_format == image_format) {
      v3dv_get_hw_clear_color(color, internal_type, internal_size, hw_color);
   } else {
      union util_color uc;
      enum pipe_format pipe_image_format =
         vk_format_to_pipe_format(image_format);
      util_pack_color(color->float32, pipe_image_format, &uc);
      memcpy(hw_color, uc.ui, internal_size);
   }
}

static void
clear_image_tlb(struct v3dv_cmd_buffer *cmd_buffer,
                struct v3dv_image *image,
                VkFormat fb_format,
                const VkClearValue *clear_value,
                const VkImageSubresourceRange *range)
{
   uint32_t internal_type, internal_bpp;
   get_internal_type_bpp_for_image_aspects(fb_format, range->aspectMask,
                                           &internal_type, &internal_bpp);

   union v3dv_clear_value hw_clear_value = { 0 };
   if (range->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
      get_hw_clear_color(&clear_value->color, fb_format, image->vk_format,
                         internal_type, internal_bpp, &hw_clear_value.color[0]);
   } else {
      assert((range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
             (range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT));
      hw_clear_value.z = clear_value->depthStencil.depth;
      hw_clear_value.s = clear_value->depthStencil.stencil;
   }

   uint32_t level_count = range->levelCount == VK_REMAINING_MIP_LEVELS ?
                          image->levels - range->baseMipLevel :
                          range->levelCount;
   uint32_t min_level = range->baseMipLevel;
   uint32_t max_level = range->baseMipLevel + level_count;

   /* For 3D images baseArrayLayer and layerCount must be 0 and 1 respectively.
    * Instead, we need to consider the full depth dimension of the image, which
    * goes from 0 up to the level's depth extent.
    */
   uint32_t min_layer;
   uint32_t max_layer;
   if (image->type != VK_IMAGE_TYPE_3D) {
      uint32_t layer_count = range->layerCount == VK_REMAINING_ARRAY_LAYERS ?
                             image->array_size - range->baseArrayLayer :
                             range->layerCount;
      min_layer = range->baseArrayLayer;
      max_layer = range->baseArrayLayer + layer_count;
   } else {
      min_layer = 0;
   }

   for (uint32_t level = min_level; level < max_level; level++) {
      if (image->type == VK_IMAGE_TYPE_3D)
         max_layer = u_minify(image->extent.depth, level);
      for (uint32_t layer = min_layer; layer < max_layer; layer++) {
         uint32_t width = u_minify(image->extent.width, level);
         uint32_t height = u_minify(image->extent.height, level);

         struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, -1);
         if (!job)
            return;

         /* We start a a new job for each layer so the frame "depth" is 1 */
         v3dv_job_start_frame(job, width, height, 1, 1, internal_bpp);

         struct framebuffer_data framebuffer;
         setup_framebuffer_data(&framebuffer, fb_format, internal_type,
                                &job->frame_tiling);

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

   VkFormat compat_format;
   const VkOffset3D origin = { 0, 0, 0 };
   for (uint32_t i = 0; i < rangeCount; i++) {
      if (can_use_tlb(image, &origin, &compat_format)) {
         clear_image_tlb(cmd_buffer, image, compat_format,
                         &clear_value, &pRanges[i]);
      } else {
         assert(!"Fallback path for vkCmdClearColorImage not implemented");
      }
   }
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

   const VkOffset3D origin = { 0, 0, 0 };
   for (uint32_t i = 0; i < rangeCount; i++) {
      if (can_use_tlb(image, &origin, NULL)) {
         clear_image_tlb(cmd_buffer, image, image->vk_format,
                         &clear_value, &pRanges[i]);
      } else {
         assert(!"Fallback path for vkCmdClearDepthStencilImage not implemented");
      }
   }
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
                 struct framebuffer_data *framebuffer,
                 uint32_t format)
{
   const uint32_t stride = job->frame_tiling.width * 4;
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
                     struct framebuffer_data *framebuffer,
                     uint32_t format)
{
   struct v3dv_cl *rcl =
      emit_rcl_prologue(job, framebuffer->internal_type, NULL);
   emit_frame_setup(job, 0, NULL);
   emit_copy_buffer(job, dst, src, dst_offset, src_offset, framebuffer, format);
   cl_emit(rcl, END_OF_RENDERING, end);
}

/* Figure out a TLB size configuration for a number of pixels to process.
 * Beware that we can't "render" more than 4096x4096 pixels in a single job,
 * if the pixel count is larger than this, the caller might need to split
 * the job and call this function multiple times.
 */
static void
framebuffer_size_for_pixel_count(uint32_t num_pixels,
                                 uint32_t *width,
                                 uint32_t *height)
{
   assert(num_pixels > 0);

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
   assert(w > 0 && h > 0);

   *width = w;
   *height = h;
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
   VkFormat vk_format;
   switch (region->size % 4) {
   case 0:
      item_size = 4;
      format = V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI;
      vk_format = VK_FORMAT_R8G8B8A8_UINT;
      break;
   case 2:
      item_size = 2;
      format = V3D_OUTPUT_IMAGE_FORMAT_RG8UI;
      vk_format = VK_FORMAT_R8G8_UINT;
      break;
   case 1:
   case 3:
      item_size = 1;
      format = V3D_OUTPUT_IMAGE_FORMAT_R8UI;
      vk_format = VK_FORMAT_R8_UINT;
      break;

   }
   assert(region->size % item_size == 0);
   uint32_t num_items = region->size / item_size;
   assert(num_items > 0);

   struct v3dv_job *job;
   uint32_t src_offset = region->srcOffset;
   uint32_t dst_offset = region->dstOffset;
   while (num_items > 0) {
      job = v3dv_cmd_buffer_start_job(cmd_buffer, -1);
      if (!job)
         return NULL;

      uint32_t width, height;
      framebuffer_size_for_pixel_count(num_items, &width, &height);

      v3dv_job_start_frame(job, width, height, 1, 1, internal_bpp);

      struct framebuffer_data framebuffer;
      setup_framebuffer_data(&framebuffer, vk_format, internal_type,
                             &job->frame_tiling);

      v3dv_job_emit_binning_flush(job);

      emit_copy_buffer_rcl(job, dst, src, dst_offset, src_offset,
                           &framebuffer, format);

      v3dv_cmd_buffer_finish_job(cmd_buffer);

      const uint32_t items_copied = width * height;
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
   if (!copy_job)
      return;

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
                 struct framebuffer_data *framebuffer)
{
   const uint32_t stride = job->frame_tiling.width * 4;
   emit_fill_buffer_per_tile_list(job, bo, offset, stride);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_fill_buffer_rcl(struct v3dv_job *job,
                     struct v3dv_bo *bo,
                     uint32_t offset,
                     struct framebuffer_data *framebuffer,
                     uint32_t data)
{
   const union v3dv_clear_value clear_value = {
       .color = { data, 0, 0, 0 },
   };

   const struct rcl_clear_info clear_info = {
      .clear_value = &clear_value,
      .image = NULL,
      .aspects = VK_IMAGE_ASPECT_COLOR_BIT,
      .layer = 0,
      .level = 0,
   };

   struct v3dv_cl *rcl =
      emit_rcl_prologue(job, framebuffer->internal_type, &clear_info);
   emit_frame_setup(job, 0, &clear_value);
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

   while (num_items > 0) {
      struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, -1);
      if (!job)
         return;

      uint32_t width, height;
      framebuffer_size_for_pixel_count(num_items, &width, &height);

      v3dv_job_start_frame(job, width, height, 1, 1, internal_bpp);

      struct framebuffer_data framebuffer;
      setup_framebuffer_data(&framebuffer, VK_FORMAT_R8G8B8A8_UINT,
                             internal_type, &job->frame_tiling);

      v3dv_job_emit_binning_flush(job);

      emit_fill_buffer_rcl(job, bo, offset, &framebuffer, data);

      v3dv_cmd_buffer_finish_job(cmd_buffer);

      const uint32_t items_copied = width * height;
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
      size = dst_buffer->size - dstOffset;
      size -= size % 4;
   }

   fill_buffer(cmd_buffer, bo, dstOffset, size, data);
}

static void
emit_copy_buffer_to_layer_per_tile_list(struct v3dv_job *job,
                                        struct framebuffer_data *framebuffer,
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
   assert((image->type != VK_IMAGE_TYPE_3D && layer < imgrsc->layerCount) ||
          layer < image->extent.depth);

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

   uint32_t format = choose_tlb_format(framebuffer, imgrsc->aspectMask,
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
   if (framebuffer->vk_format == VK_FORMAT_D24_UNORM_S8_UINT) {
      if (imgrsc->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         emit_image_load(cl, framebuffer, image, VK_IMAGE_ASPECT_STENCIL_BIT,
                         imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                         false, false);
      } else {
         assert(imgrsc->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
         emit_image_load(cl, framebuffer, image, VK_IMAGE_ASPECT_DEPTH_BIT,
                         imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                         false, false);
      }
   }

   cl_emit(cl, END_OF_LOADS, end);

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   /* Store TLB to image */
   emit_image_store(cl, framebuffer, image, imgrsc->aspectMask,
                    imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                    false, true);

   if (framebuffer->vk_format == VK_FORMAT_D24_UNORM_S8_UINT) {
      if (imgrsc->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         emit_image_store(cl, framebuffer, image, VK_IMAGE_ASPECT_STENCIL_BIT,
                          imgrsc->baseArrayLayer + layer, imgrsc->mipLevel,
                          false, false);
      } else {
         assert(imgrsc->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
         emit_image_store(cl, framebuffer, image, VK_IMAGE_ASPECT_DEPTH_BIT,
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
                          struct framebuffer_data *framebuffer,
                          uint32_t layer,
                          const VkBufferImageCopy *region)
{
   emit_frame_setup(job, layer, NULL);
   emit_copy_buffer_to_layer_per_tile_list(job, framebuffer, image, buffer,
                                           layer, region);
   emit_supertile_coordinates(job, framebuffer);
}

static void
emit_copy_buffer_to_image_rcl(struct v3dv_job *job,
                              struct v3dv_image *image,
                              struct v3dv_buffer *buffer,
                              struct framebuffer_data *framebuffer,
                              const VkBufferImageCopy *region)
{
   struct v3dv_cl *rcl =
      emit_rcl_prologue(job, framebuffer->internal_type, NULL);
   for (int layer = 0; layer < job->frame_tiling.layers; layer++)
      emit_copy_buffer_to_layer(job, image, buffer, framebuffer, layer, region);
   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
copy_buffer_to_image_tlb(struct v3dv_cmd_buffer *cmd_buffer,
                         struct v3dv_image *image,
                         struct v3dv_buffer *buffer,
                         VkFormat fb_format,
                         const VkBufferImageCopy *region)
{
   uint32_t internal_type, internal_bpp;
   get_internal_type_bpp_for_image_aspects(fb_format,
                                           region->imageSubresource.aspectMask,
                                           &internal_type, &internal_bpp);

   uint32_t num_layers;
   if (image->type != VK_IMAGE_TYPE_3D)
      num_layers = region->imageSubresource.layerCount;
   else
      num_layers = region->imageExtent.depth;
   assert(num_layers > 0);

   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, -1);
   if (!job)
      return;

   v3dv_job_start_frame(job,
                        region->imageExtent.width,
                        region->imageExtent.height,
                        num_layers, 1, internal_bpp);

   struct framebuffer_data framebuffer;
   setup_framebuffer_data(&framebuffer, fb_format, internal_type,
                          &job->frame_tiling);

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

   VkFormat compat_format;
   for (uint32_t i = 0; i < regionCount; i++) {
      if (can_use_tlb(image, &pRegions[i].imageOffset, &compat_format)) {
         copy_buffer_to_image_tlb(cmd_buffer, image, buffer, compat_format,
                                  &pRegions[i]);
      } else {
         assert(!"Fallback path for vkCmdCopyBufferToImage not implemented");
      }
   }
}

/* Disable level 0 write, just write following mipmaps */
#define V3D_TFU_IOA_DIMTW (1 << 0)
#define V3D_TFU_IOA_FORMAT_SHIFT 3
#define V3D_TFU_IOA_FORMAT_LINEARTILE 3
#define V3D_TFU_IOA_FORMAT_UBLINEAR_1_COLUMN 4
#define V3D_TFU_IOA_FORMAT_UBLINEAR_2_COLUMN 5
#define V3D_TFU_IOA_FORMAT_UIF_NO_XOR 6
#define V3D_TFU_IOA_FORMAT_UIF_XOR 7

#define V3D_TFU_ICFG_NUMMM_SHIFT 5
#define V3D_TFU_ICFG_TTYPE_SHIFT 9

#define V3D_TFU_ICFG_OPAD_SHIFT 22

#define V3D_TFU_ICFG_FORMAT_SHIFT 18
#define V3D_TFU_ICFG_FORMAT_RASTER 0
#define V3D_TFU_ICFG_FORMAT_SAND_128 1
#define V3D_TFU_ICFG_FORMAT_SAND_256 2
#define V3D_TFU_ICFG_FORMAT_LINEARTILE 11
#define V3D_TFU_ICFG_FORMAT_UBLINEAR_1_COLUMN 12
#define V3D_TFU_ICFG_FORMAT_UBLINEAR_2_COLUMN 13
#define V3D_TFU_ICFG_FORMAT_UIF_NO_XOR 14
#define V3D_TFU_ICFG_FORMAT_UIF_XOR 15

static void
emit_tfu_job(struct v3dv_cmd_buffer *cmd_buffer,
             struct v3dv_image *dst,
             uint32_t dst_mip_level,
             uint32_t dst_layer,
             struct v3dv_image *src,
             uint32_t src_mip_level,
             uint32_t src_layer,
             uint32_t width,
             uint32_t height)
{
   /* Blit jobs can only happen outside a render pass */
   assert(cmd_buffer->state.pass == NULL);
   assert(cmd_buffer->state.job == NULL);

   const struct v3d_resource_slice *src_slice = &src->slices[src_mip_level];
   const struct v3d_resource_slice *dst_slice = &dst->slices[src_mip_level];

   assert(dst->mem && dst->mem->bo);
   const struct v3dv_bo *dst_bo = dst->mem->bo;

   assert(src->mem && src->mem->bo);
   const struct v3dv_bo *src_bo = src->mem->bo;

   struct drm_v3d_submit_tfu tfu = {
      .ios = (height << 16) | width,
      .bo_handles = {
         dst_bo->handle,
         src != dst ? src_bo->handle : 0
      },
   };

   const uint32_t src_offset =
      src_bo->offset + v3dv_layer_offset(src, src_mip_level, src_layer);
   tfu.iia |= src_offset;

   uint32_t icfg;
   if (src_slice->tiling == VC5_TILING_RASTER) {
      icfg = V3D_TFU_ICFG_FORMAT_RASTER;
   } else {
      icfg = V3D_TFU_ICFG_FORMAT_LINEARTILE +
             (src_slice->tiling - VC5_TILING_LINEARTILE);
   }
   tfu.icfg |= icfg << V3D_TFU_ICFG_FORMAT_SHIFT;

   const uint32_t dst_offset =
      dst_bo->offset + v3dv_layer_offset(dst, dst_mip_level, dst_layer);
   tfu.ioa |= dst_offset;

   tfu.ioa |= (V3D_TFU_IOA_FORMAT_LINEARTILE +
               (dst_slice->tiling - VC5_TILING_LINEARTILE)) <<
                V3D_TFU_IOA_FORMAT_SHIFT;
   tfu.icfg |= dst->format->tex_type << V3D_TFU_ICFG_TTYPE_SHIFT;

   switch (src_slice->tiling) {
   case VC5_TILING_UIF_NO_XOR:
   case VC5_TILING_UIF_XOR:
      tfu.iis |= src_slice->padded_height / (2 * v3d_utile_height(src->cpp));
      break;
   case VC5_TILING_RASTER:
      tfu.iis |= src_slice->stride / src->cpp;
      break;
   default:
      break;
   }

   /* If we're writing level 0 (!IOA_DIMTW), then we need to supply the
    * OPAD field for the destination (how many extra UIF blocks beyond
    * those necessary to cover the height).
    */
   if (dst_slice->tiling == VC5_TILING_UIF_NO_XOR ||
       dst_slice->tiling == VC5_TILING_UIF_XOR) {
      uint32_t uif_block_h = 2 * v3d_utile_height(dst->cpp);
      uint32_t implicit_padded_height = align(height, uif_block_h);
      uint32_t icfg =
         (dst_slice->padded_height - implicit_padded_height) / uif_block_h;
      tfu.icfg |= icfg << V3D_TFU_ICFG_OPAD_SHIFT;
   }

   v3dv_cmd_buffer_add_tfu_job(cmd_buffer, &tfu);
}

static bool
blit_tfu(struct v3dv_cmd_buffer *cmd_buffer,
         struct v3dv_image *dst,
         struct v3dv_image *src,
         const VkImageBlit *region,
         VkFilter filter)
{
   /* FIXME? The v3d driver seems to ignore filtering completely! */
   if (filter != VK_FILTER_NEAREST)
      return false;

   /* Format must match */
   if (src->vk_format != dst->vk_format)
      return false;

   VkFormat vk_format = dst->vk_format;
   const struct v3dv_format *format = dst->format;

   /* Format must be supported for texturing */
   if (!v3dv_tfu_supports_tex_format(&cmd_buffer->device->devinfo,
                                     format->tex_type)) {
      return false;
   }

   /* Only color formats */
   if (vk_format_is_depth_or_stencil(vk_format))
      return false;

#if 0
   /* FIXME: Only 2D images? */
   if (dst->type == VK_IMAGE_TYPE_2D || src->type == VK_IMAGE_TYPE_2D)
      return false;
#endif

   /* Destination can't be raster format */
   const uint32_t dst_mip_level = region->dstSubresource.mipLevel;
   if (dst->slices[dst_mip_level].tiling == VC5_TILING_RASTER)
      return false;

   /* Source region must start at (0,0) */
   if (region->srcOffsets[0].x != 0 || region->srcOffsets[0].y != 0)
      return false;

   /* Destination image must be complete */
   if (region->dstOffsets[0].x != 0 || region->dstOffsets[0].y != 0)
      return false;

   const uint32_t dst_width = u_minify(dst->extent.width, dst_mip_level);
   const uint32_t dst_height = u_minify(dst->extent.height, dst_mip_level);
   if (region->dstOffsets[1].x < dst_width - 1||
       region->dstOffsets[1].y < dst_height - 1) {
      return false;
   }

   /* No scaling */
   if (region->srcOffsets[1].x != region->dstOffsets[1].x ||
       region->srcOffsets[1].y != region->dstOffsets[1].y) {
      return false;
   }

   /* Emit a TFU job for each layer to blit */
   assert(region->dstSubresource.layerCount ==
          region->srcSubresource.layerCount);
   const uint32_t layer_count = region->dstSubresource.layerCount;
   const uint32_t src_mip_level = region->srcSubresource.mipLevel;
   for (uint32_t i = 0; i < layer_count; i++) {
      uint32_t src_layer, dst_layer;
      if (src->type == VK_IMAGE_TYPE_3D) {
         assert(layer_count == 1);
         src_layer = u_minify(src->extent.depth, src_mip_level);
      } else {
         src_layer = region->srcSubresource.baseArrayLayer + i;
      }

      if (dst->type == VK_IMAGE_TYPE_3D) {
         assert(layer_count == 1);
         dst_layer = u_minify(dst->extent.depth, dst_mip_level);
      } else {
         dst_layer = region->dstSubresource.baseArrayLayer + i;
      }

      emit_tfu_job(cmd_buffer,
                   dst, dst_mip_level, dst_layer,
                   src, src_mip_level, src_layer,
                   dst_width, dst_height);
   }

   return true;
}

void
v3dv_CmdBlitImage(VkCommandBuffer commandBuffer,
                  VkImage srcImage,
                  VkImageLayout srcImageLayout,
                  VkImage dstImage,
                  VkImageLayout dstImageLayout,
                  uint32_t regionCount,
                  const VkImageBlit* pRegions,
                  VkFilter filter)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_image, src, srcImage);
   V3DV_FROM_HANDLE(v3dv_image, dst, dstImage);

   /* From the Vulkan 1.0 spec, vkCmdBlitImage valid usage */
   assert (dst->samples == VK_SAMPLE_COUNT_1_BIT &&
           src->samples == VK_SAMPLE_COUNT_1_BIT);

   for (uint32_t i = 0; i < regionCount; i++) {
      if (!blit_tfu(cmd_buffer, dst, src, &pRegions[i], filter))
         assert(!"Fallback path for vkCmdBlitImage not implemented.");
   }
}

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
#include "vk_util.h"
#include "vk_format_info.h"

#include "broadcom/cle/v3dx_pack.h"
#include "util/format/u_format.h"

#define SWIZ(x,y,z,w) {   \
   PIPE_SWIZZLE_##x,      \
   PIPE_SWIZZLE_##y,      \
   PIPE_SWIZZLE_##z,      \
   PIPE_SWIZZLE_##w       \
}

#define FORMAT(vk, rt, tex, swiz, return_size)     \
   [VK_FORMAT_##vk] = {                            \
      true,                                        \
      V3D_OUTPUT_IMAGE_FORMAT_##rt,                \
      TEXTURE_DATA_FORMAT_##tex,                   \
      swiz,                                        \
      return_size,                                 \
   }

#define SWIZ_X001	SWIZ(X, 0, 0, 1)
#define SWIZ_XY01	SWIZ(X, Y, 0, 1)
#define SWIZ_XYZ1	SWIZ(X, Y, Z, 1)
#define SWIZ_XYZW	SWIZ(X, Y, Z, W)
#define SWIZ_YZWX	SWIZ(Y, Z, W, X)
#define SWIZ_YZW1	SWIZ(Y, Z, W, 1)
#define SWIZ_ZYXW	SWIZ(Z, Y, X, W)
#define SWIZ_ZYX1	SWIZ(Z, Y, X, 1)
#define SWIZ_XXXY	SWIZ(X, X, X, Y)
#define SWIZ_XXX1	SWIZ(X, X, X, 1)
#define SWIZ_XXXX	SWIZ(X, X, X, X)
#define SWIZ_000X	SWIZ(0, 0, 0, X)

static const struct v3dv_format format_table[] = {
   FORMAT(R8G8B8A8_UNORM,          RGBA8,       RGBA8,       SWIZ_XYZW, 16),
   FORMAT(B8G8R8A8_UNORM,          RGBA8,       RGBA8,       SWIZ_ZYXW, 16),
};

const struct v3dv_format *
v3dv_get_format(VkFormat format)
{
   if (format < ARRAY_SIZE(format_table) && format_table[format].supported)
      return &format_table[format];
   else
      return NULL;
}

void
v3dv_get_internal_type_bpp_for_output_format(uint32_t format,
                                             uint32_t *type,
                                             uint32_t *bpp)
{
   switch (format) {
   case V3D_OUTPUT_IMAGE_FORMAT_RGBA8:
   case V3D_OUTPUT_IMAGE_FORMAT_RGB8:
   case V3D_OUTPUT_IMAGE_FORMAT_RG8:
   case V3D_OUTPUT_IMAGE_FORMAT_R8:
   case V3D_OUTPUT_IMAGE_FORMAT_ABGR4444:
   case V3D_OUTPUT_IMAGE_FORMAT_BGR565:
   case V3D_OUTPUT_IMAGE_FORMAT_ABGR1555:
      *type = V3D_INTERNAL_TYPE_8;
      *bpp = V3D_INTERNAL_BPP_32;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RGBA8I:
   case V3D_OUTPUT_IMAGE_FORMAT_RG8I:
   case V3D_OUTPUT_IMAGE_FORMAT_R8I:
      *type = V3D_INTERNAL_TYPE_8I;
      *bpp = V3D_INTERNAL_BPP_32;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RGBA8UI:
   case V3D_OUTPUT_IMAGE_FORMAT_RG8UI:
   case V3D_OUTPUT_IMAGE_FORMAT_R8UI:
      *type = V3D_INTERNAL_TYPE_8UI;
      *bpp = V3D_INTERNAL_BPP_32;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_SRGB8_ALPHA8:
   case V3D_OUTPUT_IMAGE_FORMAT_SRGB:
   case V3D_OUTPUT_IMAGE_FORMAT_RGB10_A2:
   case V3D_OUTPUT_IMAGE_FORMAT_R11F_G11F_B10F:
   case V3D_OUTPUT_IMAGE_FORMAT_RGBA16F:
      /* Note that sRGB RTs are stored in the tile buffer at 16F,
       * and the conversion to sRGB happens at tilebuffer load/store.
       */
      *type = V3D_INTERNAL_TYPE_16F;
      *bpp = V3D_INTERNAL_BPP_64;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RG16F:
   case V3D_OUTPUT_IMAGE_FORMAT_R16F:
      *type = V3D_INTERNAL_TYPE_16F;
      /* Use 64bpp to make sure the TLB doesn't throw away the alpha
       * channel before alpha test happens.
       */
      *bpp = V3D_INTERNAL_BPP_64;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RGBA16I:
      *type = V3D_INTERNAL_TYPE_16I;
      *bpp = V3D_INTERNAL_BPP_64;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RG16I:
   case V3D_OUTPUT_IMAGE_FORMAT_R16I:
      *type = V3D_INTERNAL_TYPE_16I;
      *bpp = V3D_INTERNAL_BPP_32;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RGB10_A2UI:
   case V3D_OUTPUT_IMAGE_FORMAT_RGBA16UI:
      *type = V3D_INTERNAL_TYPE_16UI;
      *bpp = V3D_INTERNAL_BPP_64;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RG16UI:
   case V3D_OUTPUT_IMAGE_FORMAT_R16UI:
      *type = V3D_INTERNAL_TYPE_16UI;
      *bpp = V3D_INTERNAL_BPP_32;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RGBA32I:
      *type = V3D_INTERNAL_TYPE_32I;
      *bpp = V3D_INTERNAL_BPP_128;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RG32I:
      *type = V3D_INTERNAL_TYPE_32I;
      *bpp = V3D_INTERNAL_BPP_64;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_R32I:
      *type = V3D_INTERNAL_TYPE_32I;
      *bpp = V3D_INTERNAL_BPP_32;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RGBA32UI:
      *type = V3D_INTERNAL_TYPE_32UI;
      *bpp = V3D_INTERNAL_BPP_128;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RG32UI:
      *type = V3D_INTERNAL_TYPE_32UI;
      *bpp = V3D_INTERNAL_BPP_64;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_R32UI:
      *type = V3D_INTERNAL_TYPE_32UI;
      *bpp = V3D_INTERNAL_BPP_32;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RGBA32F:
      *type = V3D_INTERNAL_TYPE_32F;
      *bpp = V3D_INTERNAL_BPP_128;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_RG32F:
      *type = V3D_INTERNAL_TYPE_32F;
      *bpp = V3D_INTERNAL_BPP_64;
      break;

   case V3D_OUTPUT_IMAGE_FORMAT_R32F:
      *type = V3D_INTERNAL_TYPE_32F;
      *bpp = V3D_INTERNAL_BPP_32;
      break;

   default:
      /* Provide some default values, as we'll be called at RB
       * creation time, even if an RB with this format isn't supported.
       */
      *type = V3D_INTERNAL_TYPE_8;
      *bpp = V3D_INTERNAL_BPP_32;
      break;
   }
}

const uint8_t *
v3dv_get_format_swizzle(VkFormat f)
{
   const struct v3dv_format *vf = v3dv_get_format(f);
   static const uint8_t fallback[] = {0, 1, 2, 3};

   if (!vf)
      return fallback;

   return vf->swizzle;
}

static VkFormatFeatureFlags
image_format_features(VkFormat vk_format,
                      const struct v3dv_format *v3dv_format,
                      VkImageTiling tiling)
{
   if (!v3dv_format || !v3dv_format->supported)
      return 0;

   const VkImageAspectFlags aspects = vk_format_aspects(vk_format);

   if (aspects != VK_IMAGE_ASPECT_COLOR_BIT)
      return 0;

   VkFormatFeatureFlags flags =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_BLIT_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

   if (v3dv_format->rt_type != V3D_OUTPUT_IMAGE_FORMAT_NO) {
      flags |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
               VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
               VK_FORMAT_FEATURE_BLIT_DST_BIT;
   }

   return flags;
}

static VkFormatFeatureFlags
buffer_format_features(VkFormat vk_format, const struct v3dv_format *v3dv_format)
{
   if (!v3dv_format || !v3dv_format->supported)
      return 0;

   if (!v3dv_format->supported)
      return 0;

   /* FIXME */
   const VkImageAspectFlags aspects = vk_format_aspects(vk_format);
   if (aspects != VK_IMAGE_ASPECT_COLOR_BIT)
      return 0;

   VkFormatFeatureFlags flags = 0;

   flags |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;

   return flags;
}

void
v3dv_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                       VkFormat format,
                                       VkFormatProperties* pFormatProperties)
{
   const struct v3dv_format *v3dv_format = v3dv_get_format(format);

   *pFormatProperties = (VkFormatProperties) {
      .linearTilingFeatures =
         image_format_features(format, v3dv_format, VK_IMAGE_TILING_LINEAR),
      .optimalTilingFeatures =
         image_format_features(format, v3dv_format, VK_IMAGE_TILING_OPTIMAL),
      .bufferFeatures =
         buffer_format_features(format, v3dv_format),
   };
}

VkResult
v3dv_GetPhysicalDeviceImageFormatProperties(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkImageType type,
   VkImageTiling tiling,
   VkImageUsageFlags usage,
   VkImageCreateFlags createFlags,
   VkImageFormatProperties *pImageFormatProperties)
{
   const struct v3dv_format *v3dv_format = v3dv_get_format(format);
   VkFormatFeatureFlags format_feature_flags =
      image_format_features(format, v3dv_format, tiling);
   if (!format_feature_flags)
      goto unsupported;

   if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_STORAGE_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
         goto unsupported;
      }
   }

   if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      if (!(format_feature_flags &
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
         goto unsupported;
      }
   }

   /* FIXME: these are taken from VkPhysicalDeviceLimits, we should just put
    * these limits available in the physical device and read them from there
    * wherever we need them.
    */
   switch (type) {
   case VK_IMAGE_TYPE_1D:
      pImageFormatProperties->maxExtent.width = 4096;
      pImageFormatProperties->maxExtent.height = 1;
      pImageFormatProperties->maxExtent.depth = 1;
      pImageFormatProperties->maxArrayLayers = 2048;
      pImageFormatProperties->maxMipLevels = 13; /* log2(maxWidth) + 1 */
      break;
   case VK_IMAGE_TYPE_2D:
      pImageFormatProperties->maxExtent.width = 4096;
      pImageFormatProperties->maxExtent.height = 4096;
      pImageFormatProperties->maxExtent.depth = 1;
      pImageFormatProperties->maxArrayLayers = 2048;
      pImageFormatProperties->maxMipLevels = 13; /* log2(maxWidth) + 1 */
      break;
   case VK_IMAGE_TYPE_3D:
      pImageFormatProperties->maxExtent.width = 4096;
      pImageFormatProperties->maxExtent.height = 4096;
      pImageFormatProperties->maxExtent.depth = 4096;
      pImageFormatProperties->maxArrayLayers = 1;
      pImageFormatProperties->maxMipLevels = 13; /* log2(maxWidth) + 1 */
      break;
   default:
      unreachable("bad VkImageType");
   }

   pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;

   if (tiling == VK_IMAGE_TILING_LINEAR)
      pImageFormatProperties->maxMipLevels = 1;

   pImageFormatProperties->maxResourceSize = 0xffffffff; /* 32-bit allocation */

   return VK_SUCCESS;

unsupported:
   *pImageFormatProperties = (VkImageFormatProperties) {
      .maxExtent = { 0, 0, 0 },
      .maxMipLevels = 0,
      .maxArrayLayers = 0,
      .sampleCounts = 0,
      .maxResourceSize = 0,
   };

   return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

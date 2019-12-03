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


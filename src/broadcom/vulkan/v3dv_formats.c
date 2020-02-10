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
#include "drm-uapi/drm_fourcc.h"
#include "util/format/u_format.h"
#include "vulkan/wsi/wsi_common.h"

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

/* FIXME: expand format table to describe whether the format is supported
 * for buffer surfaces (texel buffers, vertex buffers, etc).
 */
static const struct v3dv_format format_table[] = {
   /* Color, 4 channels */
   FORMAT(B8G8R8A8_SRGB,           SRGB8_ALPHA8, RGBA8,         SWIZ_ZYXW, 16),
   FORMAT(B8G8R8A8_UNORM,          RGBA8,        RGBA8,         SWIZ_ZYXW, 16),

   FORMAT(R8G8B8A8_SRGB,           SRGB8_ALPHA8, RGBA8,         SWIZ_XYZW, 16),
   FORMAT(R8G8B8A8_UNORM,          RGBA8,        RGBA8,         SWIZ_XYZW, 16),
   FORMAT(R8G8B8A8_SNORM,          NO,           RGBA8_SNORM,   SWIZ_XYZW, 16),
   FORMAT(R8G8B8A8_SINT,           RGBA8I,       RGBA8I,        SWIZ_XYZW, 16),
   FORMAT(R8G8B8A8_UINT,           RGBA8UI,      RGBA8UI,       SWIZ_XYZW, 16),

   FORMAT(R16G16B16A16_SFLOAT,     RGBA16F,      RGBA16F,       SWIZ_XYZW, 16),
   FORMAT(R16G16B16A16_UNORM,      NO,           RGBA16,        SWIZ_XYZW, 32),
   FORMAT(R16G16B16A16_SNORM,      NO,           RGBA16_SNORM,  SWIZ_XYZW, 32),
   FORMAT(R16G16B16A16_SINT,       RGBA16I,      RGBA16I,       SWIZ_XYZW, 16),
   FORMAT(R16G16B16A16_UINT,       RGBA16UI,     RGBA16UI,      SWIZ_XYZW, 16),

   FORMAT(R32G32B32A32_SFLOAT,     RGBA32F,      RGBA32F,       SWIZ_XYZW, 32),
   FORMAT(R32G32B32A32_SINT,       RGBA32I,      RGBA32I,       SWIZ_XYZW, 32),
   FORMAT(R32G32B32A32_UINT,       RGBA32UI,     RGBA32UI,      SWIZ_XYZW, 32),

   /* Color, 3 channels */
   FORMAT(R32G32B32_SFLOAT,        NO,           NO,            SWIZ_XYZ1,  0),
   FORMAT(R32G32B32_UINT,          NO,           NO,            SWIZ_XYZ1,  0),
   FORMAT(R32G32B32_SINT,          NO,           NO,            SWIZ_XYZ1,  0),

   /* Color, 2 channels */
   FORMAT(R8G8_UNORM,              RG8,          RG8,           SWIZ_XY01, 16),
   FORMAT(R8G8_SNORM,              NO,           RG8_SNORM,     SWIZ_XY01, 16),
   FORMAT(R8G8_SINT,               RG8I,         RG8I,          SWIZ_XY01, 16),
   FORMAT(R8G8_UINT,               RG8UI,        RG8UI,         SWIZ_XY01, 16),

   FORMAT(R16G16_UNORM,            NO,           RG16,          SWIZ_XY01, 32),
   FORMAT(R16G16_SNORM,            NO,           RG16_SNORM,    SWIZ_XY01, 32),
   FORMAT(R16G16_SFLOAT,           RG16F,        RG16F,         SWIZ_XY01, 16),
   FORMAT(R16G16_SINT,             RG16I,        RG16I,         SWIZ_XY01, 16),
   FORMAT(R16G16_UINT,             RG16UI,       RG16UI,        SWIZ_XY01, 16),

   FORMAT(R32G32_SFLOAT,           RG32F,        RG32F,         SWIZ_XY01, 32),
   FORMAT(R32G32_SINT,             RG32I,        RG32I,         SWIZ_XY01, 32),
   FORMAT(R32G32_UINT,             RG32UI,       RG32UI,        SWIZ_XY01, 32),

   /* Color, 1 channel */
   FORMAT(R8_UNORM,                R8,           R8,            SWIZ_X001, 16),
   FORMAT(R8_SNORM,                NO,           R8_SNORM,      SWIZ_X001, 16),
   FORMAT(R8_SINT,                 R8I,          R8I,           SWIZ_X001, 16),
   FORMAT(R8_UINT,                 R8UI,         R8UI,          SWIZ_X001, 16),

   FORMAT(R16_UNORM,               NO,           R16,           SWIZ_X001, 32),
   FORMAT(R16_SNORM,               NO,           R16_SNORM,     SWIZ_X001, 32),
   FORMAT(R16_SFLOAT,              R16F,         R16F,          SWIZ_X001, 16),
   FORMAT(R16_SINT,                R16I,         R16I,          SWIZ_X001, 16),
   FORMAT(R16_UINT,                R16UI,        R16UI,         SWIZ_X001, 16),

   FORMAT(R32_SFLOAT,              R32F,         R32F,          SWIZ_X001, 32),
   FORMAT(R32_SINT,                R32I,         R32I,          SWIZ_X001, 32),
   FORMAT(R32_UINT,                R32UI,        R32UI,         SWIZ_X001, 32),

   /* Depth */
   FORMAT(D16_UNORM,               D16,          DEPTH_COMP16,  SWIZ_XXXX, 32),
   FORMAT(D32_SFLOAT,              D32F,         DEPTH_COMP32F, SWIZ_XXXX, 32),
   FORMAT(X8_D24_UNORM_PACK32,     D24S8,        DEPTH24_X8,    SWIZ_XXXX, 32),

   /* Depth + Stencil */
   FORMAT(D24_UNORM_S8_UINT,       D24S8,        DEPTH24_X8,    SWIZ_XXXX, 32),
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

   const VkImageAspectFlags zs_aspects = VK_IMAGE_ASPECT_DEPTH_BIT |
                                         VK_IMAGE_ASPECT_STENCIL_BIT;
   const VkImageAspectFlags supported_aspects = VK_IMAGE_ASPECT_COLOR_BIT |
                                                zs_aspects;
   if ((aspects & supported_aspects) != aspects)
      return 0;

   /* FIXME: We don't support separate stencil yet */
   if ((aspects & zs_aspects) == VK_IMAGE_ASPECT_STENCIL_BIT)
      return 0;

   VkFormatFeatureFlags flags =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_BLIT_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

   if (v3dv_format->rt_type != V3D_OUTPUT_IMAGE_FORMAT_NO) {
      flags |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
      if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
         flags |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
      } else if (aspects & zs_aspects) {
         flags |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
      }
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

   flags |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;

   /* FIXME: add texel uniform/storage for formats that are "image compatible"
    */

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

void
v3dv_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                        VkFormat format,
                                        VkFormatProperties2 *pFormatProperties)
{
   v3dv_GetPhysicalDeviceFormatProperties(physicalDevice, format,
                                          &pFormatProperties->formatProperties);

   vk_foreach_struct(ext, pFormatProperties->pNext) {
      switch ((unsigned)ext->sType) {
      case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT: {
         struct VkDrmFormatModifierPropertiesListEXT *list = (void *)ext;
         VK_OUTARRAY_MAKE(out, list->pDrmFormatModifierProperties,
                          &list->drmFormatModifierCount);
         /* Only expose LINEAR for winsys formats.
          * FIXME: is this correct?
          */
         if (format == VK_FORMAT_B8G8R8A8_SRGB ||
             format == VK_FORMAT_B8G8R8A8_UNORM) {
            vk_outarray_append(&out, mod_props) {
               mod_props->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
               mod_props->drmFormatModifierPlaneCount = 1;
            }
         } else {
            vk_outarray_append(&out, mod_props) {
               mod_props->drmFormatModifier = DRM_FORMAT_MOD_BROADCOM_UIF;
               mod_props->drmFormatModifierPlaneCount = 1;
            }
         }
         break;
      }
      default:
         v3dv_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

static VkResult
get_image_format_properties(
   struct v3dv_physical_device *physical_device,
   const VkPhysicalDeviceImageFormatInfo2 *info,
   VkImageFormatProperties *pImageFormatProperties,
   VkSamplerYcbcrConversionImageFormatProperties *pYcbcrImageFormatProperties)
{
   const struct v3dv_format *v3dv_format = v3dv_get_format(info->format);
   VkFormatFeatureFlags format_feature_flags =
      image_format_features(info->format, v3dv_format, info->tiling);
   if (!format_feature_flags)
      goto unsupported;

   if (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
         goto unsupported;
      }
   }

   if (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      if (!(format_feature_flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
         goto unsupported;
      }
   }

   if (info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      if (!(format_feature_flags &
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
         goto unsupported;
      }
   }

   /* FIXME: these are taken from VkPhysicalDeviceLimits, we should just put
    * these limits available in the physical device and read them from there
    * wherever we need them.
    */
   switch (info->type) {
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

   if (info->tiling == VK_IMAGE_TILING_LINEAR)
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

static const VkExternalMemoryProperties prime_fd_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   .compatibleHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
};

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
   V3DV_FROM_HANDLE(v3dv_physical_device, physical_device, physicalDevice);

   const VkPhysicalDeviceImageFormatInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = NULL,
      .format = format,
      .type = type,
      .tiling = tiling,
      .usage = usage,
      .flags = createFlags,
   };

   return get_image_format_properties(physical_device, &info,
                                      pImageFormatProperties, NULL);
}

VkResult
v3dv_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
                                             const VkPhysicalDeviceImageFormatInfo2 *base_info,
                                             VkImageFormatProperties2 *base_props)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, physical_device, physicalDevice);
   const VkPhysicalDeviceExternalImageFormatInfo *external_info = NULL;
   VkExternalImageFormatProperties *external_props = NULL;

   /* Extract input structs */
   vk_foreach_struct_const(s, base_info->pNext) {
      switch (s->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO:
         external_info = (const void *) s;
         break;
      default:
         v3dv_debug_ignored_stype(s->sType);
         break;
      }
   }

   /* Extract output structs */
   vk_foreach_struct(s, base_props->pNext) {
      switch (s->sType) {
      case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES:
         external_props = (void *) s;
         break;
      default:
         v3dv_debug_ignored_stype(s->sType);
         break;
      }
   }

   VkResult result =
      get_image_format_properties(physical_device, base_info,
                                  &base_props->imageFormatProperties, NULL);
   if (result != VK_SUCCESS)
      goto done;

   if (external_info && external_info->handleType != 0) {
      switch (external_info->handleType) {
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
         if (external_props)
            external_props->externalMemoryProperties = prime_fd_props;
         break;
      default:
         result = VK_ERROR_FORMAT_NOT_SUPPORTED;
         break;
      }
   }

done:
   return result;
}

void
v3dv_GetPhysicalDeviceSparseImageFormatProperties(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkImageType type,
   VkSampleCountFlagBits samples,
   VkImageUsageFlags usage,
   VkImageTiling tiling,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties *pProperties)
{
   *pPropertyCount = 0;
}

void
v3dv_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties2 *pProperties)
{
   *pPropertyCount = 0;
}

void
v3dv_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   switch (pExternalBufferInfo->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      pExternalBufferProperties->externalMemoryProperties = prime_fd_props;
      return;
   default: /* Unsupported */
      pExternalBufferProperties->externalMemoryProperties =
         (VkExternalMemoryProperties) {
            .compatibleHandleTypes = pExternalBufferInfo->handleType,
         };
      break;
   }
}

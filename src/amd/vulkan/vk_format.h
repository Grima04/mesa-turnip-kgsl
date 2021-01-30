/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * Based on u_format.h which is:
 * Copyright 2009-2010 VMware, Inc.
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

#ifndef VK_FORMAT_H
#define VK_FORMAT_H

#include <assert.h>
#include <vulkan/vulkan.h>
#include <util/macros.h>
#include <vulkan/util/vk_format.h>

enum vk_format_layout {
	/**
	 * Formats with vk_format_block::width == vk_format_block::height == 1
	 * that can be described as an ordinary data structure.
	 */
	VK_FORMAT_LAYOUT_PLAIN = 0,

	/**
	 * Formats with sub-sampled channels.
	 *
	 * This is for formats like YVYU where there is less than one sample per
	 * pixel.
	 */
	VK_FORMAT_LAYOUT_SUBSAMPLED = 3,

	/**
	 * S3 Texture Compression formats.
	 */
	VK_FORMAT_LAYOUT_S3TC = 4,

	/**
	 * Red-Green Texture Compression formats.
	 */
	VK_FORMAT_LAYOUT_RGTC = 5,

	/**
	 * Ericsson Texture Compression
	 */
	VK_FORMAT_LAYOUT_ETC = 6,

	/**
	 * BC6/7 Texture Compression
	 */
	VK_FORMAT_LAYOUT_BPTC = 7,

	/**
	 * ASTC
	 */
	VK_FORMAT_LAYOUT_ASTC = 8,

	/**
	 * Everything else that doesn't fit in any of the above layouts.
	 */
	VK_FORMAT_LAYOUT_OTHER = 9,

	/**
	 * Formats that contain multiple planes.
	 */
	VK_FORMAT_LAYOUT_MULTIPLANE = 10,
};

struct vk_format_block
{
	/** Block width in pixels */
	unsigned width;

	/** Block height in pixels */
	unsigned height;

	/** Block size in bits */
	unsigned bits;
};

enum vk_format_type {
	VK_FORMAT_TYPE_VOID = 0,
	VK_FORMAT_TYPE_UNSIGNED = 1,
	VK_FORMAT_TYPE_SIGNED = 2,
	VK_FORMAT_TYPE_FIXED = 3,
	VK_FORMAT_TYPE_FLOAT = 4
};


enum vk_format_colorspace {
	VK_FORMAT_COLORSPACE_RGB = 0,
	VK_FORMAT_COLORSPACE_SRGB = 1,
	VK_FORMAT_COLORSPACE_YUV = 2,
	VK_FORMAT_COLORSPACE_ZS = 3
};

struct vk_format_channel_description {
	unsigned type:5;
	unsigned normalized:1;
	unsigned pure_integer:1;
	unsigned scaled:1;
	unsigned size:8;
	unsigned shift:16;
};

struct vk_format_description
{
	VkFormat format;
	const char *name;
	const char *short_name;

	struct vk_format_block block;
	enum vk_format_layout layout;

	unsigned nr_channels:3;
	unsigned is_array:1;
	unsigned is_bitmask:1;
	unsigned is_mixed:1;

	struct vk_format_channel_description channel[4];

	unsigned char swizzle[4];

	enum vk_format_colorspace colorspace;

	unsigned plane_count:2;
	unsigned width_divisor:2;
	unsigned height_divisor:2;
	VkFormat plane_formats[3];
};

extern const struct vk_format_description vk_format_description_table[];

/* Silence warnings triggered by sharing function/struct names */
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
const struct vk_format_description *vk_format_description(VkFormat format);
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

/**
 * Return total bits needed for the pixel format per block.
 */
static inline unsigned
vk_format_get_blocksizebits(VkFormat format)
{
	return util_format_get_blocksizebits(vk_format_to_pipe_format(format));
}

/**
 * Return bytes per block (not pixel) for the given format.
 */
static inline unsigned
vk_format_get_blocksize(VkFormat format)
{
	return util_format_get_blocksize(vk_format_to_pipe_format(format));
}

static inline unsigned
vk_format_get_blockwidth(VkFormat format)
{
	return util_format_get_blockwidth(vk_format_to_pipe_format(format));
}

static inline unsigned
vk_format_get_blockheight(VkFormat format)
{
	return util_format_get_blockheight(vk_format_to_pipe_format(format));
}

/**
 * Return the index of the first non-void channel
 * -1 if no non-void channels
 */
static inline int
vk_format_get_first_non_void_channel(VkFormat format)
{
	return util_format_get_first_non_void_channel(vk_format_to_pipe_format(format));
}

static inline VkImageAspectFlags
vk_format_aspects(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_UNDEFINED:
		return 0;

	case VK_FORMAT_S8_UINT:
		return VK_IMAGE_ASPECT_STENCIL_BIT;

	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
		return VK_IMAGE_ASPECT_DEPTH_BIT;

	default:
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

static inline enum pipe_swizzle
radv_swizzle_conv(VkComponentSwizzle component, const unsigned char chan[4], VkComponentSwizzle vk_swiz)
{
	if (vk_swiz == VK_COMPONENT_SWIZZLE_IDENTITY)
		vk_swiz = component;
	switch (vk_swiz) {
	case VK_COMPONENT_SWIZZLE_ZERO:
		return PIPE_SWIZZLE_0;
	case VK_COMPONENT_SWIZZLE_ONE:
		return PIPE_SWIZZLE_1;
	case VK_COMPONENT_SWIZZLE_R:
	case VK_COMPONENT_SWIZZLE_G:
	case VK_COMPONENT_SWIZZLE_B:
	case VK_COMPONENT_SWIZZLE_A:
		return (enum pipe_swizzle)chan[vk_swiz - VK_COMPONENT_SWIZZLE_R];
	default:
		unreachable("Illegal swizzle");
	}
}

static inline void vk_format_compose_swizzles(const VkComponentMapping *mapping,
					      const unsigned char swz[4],
					      enum pipe_swizzle dst[4])
{
	dst[0] = radv_swizzle_conv(VK_COMPONENT_SWIZZLE_R, swz, mapping->r);
	dst[1] = radv_swizzle_conv(VK_COMPONENT_SWIZZLE_G, swz, mapping->g);
	dst[2] = radv_swizzle_conv(VK_COMPONENT_SWIZZLE_B, swz, mapping->b);
	dst[3] = radv_swizzle_conv(VK_COMPONENT_SWIZZLE_A, swz, mapping->a);
}

static inline bool
vk_format_is_compressed(VkFormat format)
{
	return util_format_is_compressed(vk_format_to_pipe_format(format));
}

static inline bool
vk_format_is_subsampled(VkFormat format)
{
	return util_format_is_subsampled_422(vk_format_to_pipe_format(format));
}

static inline bool
vk_format_has_depth(const struct vk_format_description *desc)
{
	return desc->colorspace == VK_FORMAT_COLORSPACE_ZS &&
		desc->swizzle[0] != PIPE_SWIZZLE_NONE;
}

static inline bool
vk_format_has_stencil(const struct vk_format_description *desc)
{
	return desc->colorspace == VK_FORMAT_COLORSPACE_ZS &&
		desc->swizzle[1] != PIPE_SWIZZLE_NONE;
}

static inline bool
vk_format_is_depth_or_stencil(VkFormat format)
{
	const struct util_format_description *desc = util_format_description(vk_format_to_pipe_format(format));

	assert(desc);
	if (!desc) {
		return false;
	}

	return util_format_has_depth(desc) ||
	       util_format_has_stencil(desc);
}

static inline bool
vk_format_is_depth(VkFormat format)
{
	const struct util_format_description *desc = util_format_description(vk_format_to_pipe_format(format));

	assert(desc);
	if (!desc) {
		return false;
	}

	return util_format_has_depth(desc);
}

static inline bool
vk_format_is_stencil(VkFormat format)
{
	const struct util_format_description *desc = util_format_description(vk_format_to_pipe_format(format));

	assert(desc);
	if (!desc) {
		return false;
	}

	return util_format_has_stencil(desc);
}

static inline bool
vk_format_is_color(VkFormat format)
{
	return !vk_format_is_depth_or_stencil(format);
}

static inline VkFormat
vk_format_depth_only(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_D16_UNORM_S8_UINT:
		return VK_FORMAT_D16_UNORM;
	case VK_FORMAT_D24_UNORM_S8_UINT:
		return VK_FORMAT_X8_D24_UNORM_PACK32;
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_FORMAT_D32_SFLOAT;
	default:
		return format;
	}
}

static inline bool
vk_format_is_int(VkFormat format)
{
	return util_format_is_pure_integer(vk_format_to_pipe_format(format));
}

static inline bool
vk_format_is_uint(VkFormat format)
{
	return util_format_is_pure_uint(vk_format_to_pipe_format(format));
}

static inline bool
vk_format_is_sint(VkFormat format)
{
	return util_format_is_pure_sint(vk_format_to_pipe_format(format));
}

static inline bool
vk_format_is_unorm(VkFormat format)
{
	return util_format_is_unorm(vk_format_to_pipe_format(format));
}

static inline bool
vk_format_is_srgb(VkFormat format)
{
	return util_format_is_srgb(vk_format_to_pipe_format(format));
}

static inline VkFormat
vk_format_no_srgb(VkFormat format)
{
	switch(format) {
	case VK_FORMAT_R8_SRGB:
		return VK_FORMAT_R8_UNORM;
	case VK_FORMAT_R8G8_SRGB:
		return VK_FORMAT_R8G8_UNORM;
	case VK_FORMAT_R8G8B8_SRGB:
		return VK_FORMAT_R8G8B8_UNORM;
	case VK_FORMAT_B8G8R8_SRGB:
		return VK_FORMAT_B8G8R8_UNORM;
	case VK_FORMAT_R8G8B8A8_SRGB:
		return VK_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_SRGB:
		return VK_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
		return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
	case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
		return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
	case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
		return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case VK_FORMAT_BC2_SRGB_BLOCK:
		return VK_FORMAT_BC2_UNORM_BLOCK;
	case VK_FORMAT_BC3_SRGB_BLOCK:
		return VK_FORMAT_BC3_UNORM_BLOCK;
	case VK_FORMAT_BC7_SRGB_BLOCK:
		return VK_FORMAT_BC7_UNORM_BLOCK;
	case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
		return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
	case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
		return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
	case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
		return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
	default:
		assert(!vk_format_is_srgb(format));
		return format;
	}
}

static inline VkFormat
vk_format_stencil_only(VkFormat format)
{
	return VK_FORMAT_S8_UINT;
}

static inline unsigned
vk_format_get_component_bits(VkFormat format,
			     enum vk_format_colorspace colorspace,
			     unsigned component)
{
	const struct vk_format_description *desc = vk_format_description(format);
	enum vk_format_colorspace desc_colorspace;

	assert(format);
	if (!format) {
		return 0;
	}

	assert(component < 4);

	/* Treat RGB and SRGB as equivalent. */
	if (colorspace == VK_FORMAT_COLORSPACE_SRGB) {
		colorspace = VK_FORMAT_COLORSPACE_RGB;
	}
	if (desc->colorspace == VK_FORMAT_COLORSPACE_SRGB) {
		desc_colorspace = VK_FORMAT_COLORSPACE_RGB;
	} else {
		desc_colorspace = desc->colorspace;
	}

	if (desc_colorspace != colorspace) {
		return 0;
	}

	switch (desc->swizzle[component]) {
	case PIPE_SWIZZLE_X:
		return desc->channel[0].size;
	case PIPE_SWIZZLE_Y:
		return desc->channel[1].size;
	case PIPE_SWIZZLE_Z:
		return desc->channel[2].size;
	case PIPE_SWIZZLE_W:
		return desc->channel[3].size;
	default:
		return 0;
	}
}

static inline VkFormat
vk_to_non_srgb_format(VkFormat format)
{
	switch(format) {
	case VK_FORMAT_R8_SRGB :
		return VK_FORMAT_R8_UNORM;
	case VK_FORMAT_R8G8_SRGB:
		return VK_FORMAT_R8G8_UNORM;
	case VK_FORMAT_R8G8B8_SRGB:
		return VK_FORMAT_R8G8B8_UNORM;
	case VK_FORMAT_B8G8R8_SRGB:
		return VK_FORMAT_B8G8R8_UNORM;
	case VK_FORMAT_R8G8B8A8_SRGB :
		return VK_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_SRGB:
		return VK_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
		return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
	default:
		return format;
	}
}

static inline unsigned
vk_format_get_nr_components(VkFormat format)
{
	return util_format_get_nr_components(vk_format_to_pipe_format(format));
}

static inline unsigned
vk_format_get_plane_count(VkFormat format)
{
	return util_format_get_num_planes(vk_format_to_pipe_format(format));
}

static inline unsigned
vk_format_get_plane_width(VkFormat format, unsigned plane,
                          unsigned width)
{
	return util_format_get_plane_width(vk_format_to_pipe_format(format), plane, width);
}

static inline unsigned
vk_format_get_plane_height(VkFormat format, unsigned plane,
                          unsigned height)
{
	return util_format_get_plane_height(vk_format_to_pipe_format(format), plane, height);
}

static inline VkFormat
vk_format_get_plane_format(VkFormat format, unsigned plane_id)
{
	const struct vk_format_description *desc = vk_format_description(format);

	if (desc->layout != VK_FORMAT_LAYOUT_MULTIPLANE) {
		assert(plane_id == 0);
		return format;
	}

	assert(plane_id < desc->plane_count);

	return desc->plane_formats[plane_id];
}


#endif /* VK_FORMAT_H */

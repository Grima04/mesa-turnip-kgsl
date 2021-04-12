/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#ifndef VN_ANDROID_H
#define VN_ANDROID_H

#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vulkan.h>

/* venus implements VK_ANDROID_native_buffer up to spec version 7 */
#define VN_ANDROID_NATIVE_BUFFER_SPEC_VERSION 7

struct vn_device;
struct vn_image;

VkResult
vn_image_from_anb(struct vn_device *dev,
                  const VkImageCreateInfo *image_info,
                  const VkNativeBufferANDROID *anb_info,
                  const VkAllocationCallbacks *alloc,
                  struct vn_image **out_img);

#endif /* VN_ANDROID_H */

/*
 * Copyright © 2016 Intel Corporation
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

#include "tu_private.h"

static void
meta_copy_buffer_to_image(struct tu_cmd_buffer *cmd_buffer,
                          struct tu_buffer *buffer,
                          struct tu_image *image,
                          VkImageLayout layout,
                          uint32_t regionCount,
                          const VkBufferImageCopy *pRegions)
{
}

void
tu_CmdCopyBufferToImage(VkCommandBuffer commandBuffer,
                         VkBuffer srcBuffer,
                         VkImage destImage,
                         VkImageLayout destImageLayout,
                         uint32_t regionCount,
                         const VkBufferImageCopy *pRegions)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_image, dest_image, destImage);
   TU_FROM_HANDLE(tu_buffer, src_buffer, srcBuffer);

   meta_copy_buffer_to_image(cmd_buffer,
                             src_buffer,
                             dest_image,
                             destImageLayout,
                             regionCount,
                             pRegions);
}

static void
meta_copy_image_to_buffer(struct tu_cmd_buffer *cmd_buffer,
                          struct tu_buffer *buffer,
                          struct tu_image *image,
                          VkImageLayout layout,
                          uint32_t regionCount,
                          const VkBufferImageCopy *pRegions)
{
}

void
tu_CmdCopyImageToBuffer(VkCommandBuffer commandBuffer,
                         VkImage srcImage,
                         VkImageLayout srcImageLayout,
                         VkBuffer destBuffer,
                         uint32_t regionCount,
                         const VkBufferImageCopy *pRegions)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_image, src_image, srcImage);
   TU_FROM_HANDLE(tu_buffer, dst_buffer, destBuffer);

   meta_copy_image_to_buffer(
     cmd_buffer, dst_buffer, src_image, srcImageLayout, regionCount, pRegions);
}

static void
meta_copy_image(struct tu_cmd_buffer *cmd_buffer,
                struct tu_image *src_image,
                VkImageLayout src_image_layout,
                struct tu_image *dest_image,
                VkImageLayout dest_image_layout,
                uint32_t regionCount,
                const VkImageCopy *pRegions)
{
}

void
tu_CmdCopyImage(VkCommandBuffer commandBuffer,
                 VkImage srcImage,
                 VkImageLayout srcImageLayout,
                 VkImage destImage,
                 VkImageLayout destImageLayout,
                 uint32_t regionCount,
                 const VkImageCopy *pRegions)
{
   TU_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);
   TU_FROM_HANDLE(tu_image, src_image, srcImage);
   TU_FROM_HANDLE(tu_image, dest_image, destImage);

   meta_copy_image(cmd_buffer,
                   src_image,
                   srcImageLayout,
                   dest_image,
                   destImageLayout,
                   regionCount,
                   pRegions);
}

#include "tu_private.h"

void
tu_CmdFillBuffer(VkCommandBuffer commandBuffer,
                  VkBuffer dstBuffer,
                  VkDeviceSize dstOffset,
                  VkDeviceSize fillSize,
                  uint32_t data)
{
}

void
tu_CmdCopyBuffer(VkCommandBuffer commandBuffer,
                  VkBuffer srcBuffer,
                  VkBuffer destBuffer,
                  uint32_t regionCount,
                  const VkBufferCopy *pRegions)
{
}

void
tu_CmdUpdateBuffer(VkCommandBuffer commandBuffer,
                    VkBuffer dstBuffer,
                    VkDeviceSize dstOffset,
                    VkDeviceSize dataSize,
                    const void *pData)
{
}

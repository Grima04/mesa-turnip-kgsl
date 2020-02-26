/*
 * Copyright © 2020 Valve Corporation
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

#include "radv_private.h"

/**
 * Identifiers for RGP SQ thread-tracing markers (Table 1)
 */
enum rgp_sqtt_marker_identifier {
	RGP_SQTT_MARKER_IDENTIFIER_EVENT		= 0x0,
	RGP_SQTT_MARKER_IDENTIFIER_CB_START             = 0x1,
	RGP_SQTT_MARKER_IDENTIFIER_CB_END               = 0x2,
	RGP_SQTT_MARKER_IDENTIFIER_BARRIER_START        = 0x3,
	RGP_SQTT_MARKER_IDENTIFIER_BARRIER_END          = 0x4,
	RGP_SQTT_MARKER_IDENTIFIER_USER_EVENT           = 0x5,
	RGP_SQTT_MARKER_IDENTIFIER_GENERAL_API          = 0x6,
	RGP_SQTT_MARKER_IDENTIFIER_SYNC			= 0x7,
	RGP_SQTT_MARKER_IDENTIFIER_PRESENT		= 0x8,
	RGP_SQTT_MARKER_IDENTIFIER_LAYOUT_TRANSITION    = 0x9,
	RGP_SQTT_MARKER_IDENTIFIER_RENDER_PASS          = 0xA,
	RGP_SQTT_MARKER_IDENTIFIER_RESERVED2		= 0xB,
	RGP_SQTT_MARKER_IDENTIFIER_BIND_PIPELINE        = 0xC,
	RGP_SQTT_MARKER_IDENTIFIER_RESERVED4		= 0xD,
	RGP_SQTT_MARKER_IDENTIFIER_RESERVED5		= 0xE,
	RGP_SQTT_MARKER_IDENTIFIER_RESERVED6		= 0xF
};

/**
 * API types used in RGP SQ thread-tracing markers for the "General API"
 * packet.
 */
enum rgp_sqtt_marker_general_api_type {
	ApiCmdBindPipeline                     = 0,
	ApiCmdBindDescriptorSets               = 1,
	ApiCmdBindIndexBuffer                  = 2,
	ApiCmdBindVertexBuffers                = 3,
	ApiCmdDraw                             = 4,
	ApiCmdDrawIndexed                      = 5,
	ApiCmdDrawIndirect                     = 6,
	ApiCmdDrawIndexedIndirect              = 7,
	ApiCmdDrawIndirectCountAMD             = 8,
	ApiCmdDrawIndexedIndirectCountAMD      = 9,
	ApiCmdDispatch                         = 10,
	ApiCmdDispatchIndirect                 = 11,
	ApiCmdCopyBuffer                       = 12,
	ApiCmdCopyImage                        = 13,
	ApiCmdBlitImage                        = 14,
	ApiCmdCopyBufferToImage                = 15,
	ApiCmdCopyImageToBuffer                = 16,
	ApiCmdUpdateBuffer                     = 17,
	ApiCmdFillBuffer                       = 18,
	ApiCmdClearColorImage                  = 19,
	ApiCmdClearDepthStencilImage           = 20,
	ApiCmdClearAttachments                 = 21,
	ApiCmdResolveImage                     = 22,
	ApiCmdWaitEvents                       = 23,
	ApiCmdPipelineBarrier                  = 24,
	ApiCmdBeginQuery                       = 25,
	ApiCmdEndQuery                         = 26,
	ApiCmdResetQueryPool                   = 27,
	ApiCmdWriteTimestamp                   = 28,
	ApiCmdCopyQueryPoolResults             = 29,
	ApiCmdPushConstants                    = 30,
	ApiCmdBeginRenderPass                  = 31,
	ApiCmdNextSubpass                      = 32,
	ApiCmdEndRenderPass                    = 33,
	ApiCmdExecuteCommands                  = 34,
	ApiCmdSetViewport                      = 35,
	ApiCmdSetScissor                       = 36,
	ApiCmdSetLineWidth                     = 37,
	ApiCmdSetDepthBias                     = 38,
	ApiCmdSetBlendConstants                = 39,
	ApiCmdSetDepthBounds                   = 40,
	ApiCmdSetStencilCompareMask            = 41,
	ApiCmdSetStencilWriteMask              = 42,
	ApiCmdSetStencilReference              = 43,
	ApiCmdDrawIndirectCountKHR             = 44,
	ApiCmdDrawIndexedIndirectCountKHR      = 45,
	ApiInvalid                             = 0xffffffff
};

/**
 * RGP SQ thread-tracing marker for a "General API" instrumentation packet.
 */
struct rgp_sqtt_marker_general_api {
	union {
		struct {
			uint32_t identifier : 4;
			uint32_t ext_dwords : 3;
			uint32_t api_type : 20;
			uint32_t is_end : 1;
			uint32_t reserved : 4;
		};
		uint32_t dword01;
	};
};

static_assert(sizeof(struct rgp_sqtt_marker_general_api) == 4,
	      "rgp_sqtt_marker_general_api doesn't match RGP spec");

static void
radv_write_begin_general_api_marker(struct radv_cmd_buffer *cmd_buffer,
				    enum rgp_sqtt_marker_general_api_type api_type)
{
	struct rgp_sqtt_marker_general_api marker = {};
	struct radeon_cmdbuf *cs = cmd_buffer->cs;

	marker.identifier = RGP_SQTT_MARKER_IDENTIFIER_GENERAL_API;
	marker.api_type = api_type;

	radv_emit_thread_trace_userdata(cs, &marker, sizeof(marker) / 4);
}

static void
radv_write_end_general_api_marker(struct radv_cmd_buffer *cmd_buffer,
				  enum rgp_sqtt_marker_general_api_type api_type)
{
	struct rgp_sqtt_marker_general_api marker = {};
	struct radeon_cmdbuf *cs = cmd_buffer->cs;

	marker.identifier = RGP_SQTT_MARKER_IDENTIFIER_GENERAL_API;
	marker.api_type = api_type;
	marker.is_end = 1;

	radv_emit_thread_trace_userdata(cs, &marker, sizeof(marker) / 4);
}

#define API_MARKER(cmd_name, args...) \
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer); \
	radv_write_begin_general_api_marker(cmd_buffer, ApiCmd##cmd_name); \
	radv_Cmd##cmd_name(args); \
	radv_write_end_general_api_marker(cmd_buffer, ApiCmd##cmd_name);

void sqtt_CmdBindPipeline(
	VkCommandBuffer                             commandBuffer,
	VkPipelineBindPoint                         pipelineBindPoint,
	VkPipeline                                  pipeline)
{
	API_MARKER(BindPipeline, commandBuffer, pipelineBindPoint, pipeline);
}

void sqtt_CmdBindDescriptorSets(
	VkCommandBuffer                             commandBuffer,
	VkPipelineBindPoint                         pipelineBindPoint,
	VkPipelineLayout                            layout,
	uint32_t                                    firstSet,
	uint32_t                                    descriptorSetCount,
	const VkDescriptorSet*                      pDescriptorSets,
	uint32_t                                    dynamicOffsetCount,
	const uint32_t*                             pDynamicOffsets)
{
	API_MARKER(BindDescriptorSets, commandBuffer, pipelineBindPoint,
		   layout, firstSet, descriptorSetCount,
		   pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

void sqtt_CmdBindIndexBuffer(
	VkCommandBuffer                             commandBuffer,
	VkBuffer				    buffer,
	VkDeviceSize				    offset,
	VkIndexType				    indexType)
{
	API_MARKER(BindIndexBuffer, commandBuffer, buffer, offset, indexType);
}

void sqtt_CmdBindVertexBuffers(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    firstBinding,
	uint32_t                                    bindingCount,
	const VkBuffer*                             pBuffers,
	const VkDeviceSize*                         pOffsets)
{
	API_MARKER(BindVertexBuffers, commandBuffer, firstBinding, bindingCount,
		   pBuffers, pOffsets);
}

void sqtt_CmdBeginQuery(
	VkCommandBuffer                             commandBuffer,
	VkQueryPool                                 queryPool,
	uint32_t                                    query,
	VkQueryControlFlags                         flags)
{
	API_MARKER(BeginQuery, commandBuffer, queryPool, query, flags);
}

void sqtt_CmdEndQuery(
	VkCommandBuffer                             commandBuffer,
	VkQueryPool                                 queryPool,
	uint32_t                                    query)
{
	API_MARKER(EndQuery, commandBuffer, queryPool, query);
}

void sqtt_CmdWriteTimestamp(
	VkCommandBuffer                             commandBuffer,
	VkPipelineStageFlagBits                     pipelineStage,
	VkQueryPool                                 queryPool,
	uint32_t				    flags)
{
	API_MARKER(WriteTimestamp, commandBuffer, pipelineStage, queryPool, flags);
}

void sqtt_CmdPushConstants(
	VkCommandBuffer				    commandBuffer,
	VkPipelineLayout			    layout,
	VkShaderStageFlags			    stageFlags,
	uint32_t				    offset,
	uint32_t				    size,
	const void*				    pValues)
{
	API_MARKER(PushConstants, commandBuffer, layout, stageFlags, offset,
		   size, pValues);
}

void sqtt_CmdBeginRenderPass(
	VkCommandBuffer                             commandBuffer,
	const VkRenderPassBeginInfo*                pRenderPassBegin,
	VkSubpassContents                           contents)
{
	API_MARKER(BeginRenderPass, commandBuffer, pRenderPassBegin, contents);
}

void sqtt_CmdNextSubpass(
	VkCommandBuffer                             commandBuffer,
	VkSubpassContents                           contents)
{
	API_MARKER(NextSubpass, commandBuffer, contents);
}

void sqtt_CmdEndRenderPass(
	VkCommandBuffer                             commandBuffer)
{
	API_MARKER(EndRenderPass, commandBuffer);
}

void sqtt_CmdExecuteCommands(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    commandBufferCount,
	const VkCommandBuffer*                      pCmdBuffers)
{
	API_MARKER(ExecuteCommands, commandBuffer, commandBufferCount,
		   pCmdBuffers);
}

void sqtt_CmdSetViewport(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    firstViewport,
	uint32_t                                    viewportCount,
	const VkViewport*                           pViewports)
{
	API_MARKER(SetViewport, commandBuffer, firstViewport, viewportCount,
		   pViewports);
}

void sqtt_CmdSetScissor(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    firstScissor,
	uint32_t                                    scissorCount,
	const VkRect2D*                             pScissors)
{
	API_MARKER(SetScissor, commandBuffer, firstScissor, scissorCount,
		   pScissors);
}

void sqtt_CmdSetLineWidth(
	VkCommandBuffer                             commandBuffer,
	float                                       lineWidth)
{
	API_MARKER(SetLineWidth, commandBuffer, lineWidth);
}

void sqtt_CmdSetDepthBias(
	VkCommandBuffer                             commandBuffer,
	float                                       depthBiasConstantFactor,
	float                                       depthBiasClamp,
	float                                       depthBiasSlopeFactor)
{
	API_MARKER(SetDepthBias, commandBuffer, depthBiasConstantFactor,
		   depthBiasClamp, depthBiasSlopeFactor);
}

void sqtt_CmdSetBlendConstants(
	VkCommandBuffer                             commandBuffer,
	const float                                 blendConstants[4])
{
	API_MARKER(SetBlendConstants, commandBuffer, blendConstants);
}

void sqtt_CmdSetDepthBounds(
	VkCommandBuffer                             commandBuffer,
	float                                       minDepthBounds,
	float                                       maxDepthBounds)
{
	API_MARKER(SetDepthBounds, commandBuffer, minDepthBounds,
		   maxDepthBounds);
}

void sqtt_CmdSetStencilCompareMask(
	VkCommandBuffer                             commandBuffer,
	VkStencilFaceFlags                          faceMask,
	uint32_t                                    compareMask)
{
	API_MARKER(SetStencilCompareMask, commandBuffer, faceMask, compareMask);
}

void sqtt_CmdSetStencilWriteMask(
	VkCommandBuffer                             commandBuffer,
	VkStencilFaceFlags                          faceMask,
	uint32_t                                    writeMask)
{
	API_MARKER(SetStencilWriteMask, commandBuffer, faceMask, writeMask);
}

void sqtt_CmdSetStencilReference(
	VkCommandBuffer                             commandBuffer,
	VkStencilFaceFlags                          faceMask,
	uint32_t                                    reference)
{
	API_MARKER(SetStencilReference, commandBuffer, faceMask, reference);
}

#undef API_MARKER

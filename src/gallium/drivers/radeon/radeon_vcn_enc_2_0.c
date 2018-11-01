/**************************************************************************
 *
 * Copyright 2017 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <stdio.h>

#include "pipe/p_video_codec.h"

#include "util/u_video.h"

#include "si_pipe.h"
#include "radeon_video.h"
#include "radeon_vcn_enc.h"

#define RENCODE_FW_INTERFACE_MAJOR_VERSION		0
#define RENCODE_FW_INTERFACE_MINOR_VERSION		0

#define RENCODE_IB_PARAM_SESSION_INFO				0x00000001
#define RENCODE_IB_PARAM_TASK_INFO  				0x00000002
#define RENCODE_IB_PARAM_SESSION_INIT				0x00000003
#define RENCODE_IB_PARAM_LAYER_CONTROL				0x00000004
#define RENCODE_IB_PARAM_LAYER_SELECT				0x00000005
#define RENCODE_IB_PARAM_RATE_CONTROL_SESSION_INIT 		0x00000006
#define RENCODE_IB_PARAM_RATE_CONTROL_LAYER_INIT	 	0x00000007
#define RENCODE_IB_PARAM_RATE_CONTROL_PER_PICTURE 		0x00000008
#define RENCODE_IB_PARAM_QUALITY_PARAMS 			0x00000009
#define RENCODE_IB_PARAM_DIRECT_OUTPUT_NALU 			0x0000000a
#define RENCODE_IB_PARAM_SLICE_HEADER				0x0000000b
#define RENCODE_IB_PARAM_INPUT_FORMAT				0x0000000c
#define RENCODE_IB_PARAM_OUTPUT_FORMAT				0x0000000d
#define RENCODE_IB_PARAM_ENCODE_PARAMS				0x0000000f
#define RENCODE_IB_PARAM_INTRA_REFRESH				0x00000010
#define RENCODE_IB_PARAM_ENCODE_CONTEXT_BUFFER  		0x00000011
#define RENCODE_IB_PARAM_VIDEO_BITSTREAM_BUFFER 		0x00000012
#define RENCODE_IB_PARAM_FEEDBACK_BUFFER			0x00000015

#define RENCODE_HEVC_IB_PARAM_SLICE_CONTROL			0x00100001
#define RENCODE_HEVC_IB_PARAM_SPEC_MISC 			0x00100002
#define RENCODE_HEVC_IB_PARAM_LOOP_FILTER			0x00100003

#define RENCODE_H264_IB_PARAM_SLICE_CONTROL			0x00200001
#define RENCODE_H264_IB_PARAM_SPEC_MISC 			0x00200002
#define RENCODE_H264_IB_PARAM_ENCODE_PARAMS			0x00200003
#define RENCODE_H264_IB_PARAM_DEBLOCKING_FILTER 		0x00200004

static void encode(struct radeon_encoder *enc)
{
	/* TODO */
}

void radeon_enc_2_0_init(struct radeon_encoder *enc)
{
	/* TODO */
}

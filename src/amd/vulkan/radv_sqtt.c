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

#include <inttypes.h>

#include "radv_private.h"
#include "radv_cs.h"
#include "sid.h"

#define SQTT_BUFFER_ALIGN_SHIFT 12

static uint64_t
radv_thread_trace_get_info_offset(unsigned se)
{
	return sizeof(struct radv_thread_trace_info) * se;
}

static uint64_t
radv_thread_trace_get_data_offset(struct radv_device *device, unsigned se)
{
	uint64_t data_offset;

	data_offset = align64(sizeof(struct radv_thread_trace_info) * 4,
			      1 << SQTT_BUFFER_ALIGN_SHIFT);
	data_offset += device->thread_trace_buffer_size * se;

	return data_offset;
}

static uint64_t
radv_thread_trace_get_info_va(struct radv_device *device, unsigned se)
{
	uint64_t va = radv_buffer_get_va(device->thread_trace_bo);
	return va + radv_thread_trace_get_info_offset(se);
}

static uint64_t
radv_thread_trace_get_data_va(struct radv_device *device, unsigned se)
{
	uint64_t va = radv_buffer_get_va(device->thread_trace_bo);
	return va + radv_thread_trace_get_data_offset(device, se);
}

static void
radv_emit_thread_trace_start(struct radv_device *device,
			     struct radeon_cmdbuf *cs,
			     uint32_t queue_family_index)
{
	uint32_t shifted_size = device->thread_trace_buffer_size >> SQTT_BUFFER_ALIGN_SHIFT;

	assert(device->physical_device->rad_info.chip_class == GFX9);

	for (unsigned se = 0; se < 4; se++) {
		uint64_t data_va = radv_thread_trace_get_data_va(device, se);
		uint64_t shifted_va = data_va >> SQTT_BUFFER_ALIGN_SHIFT;

		/* Target SEx and SH0. */
		radeon_set_uconfig_reg(cs, R_030800_GRBM_GFX_INDEX,
				       S_030800_SE_INDEX(se) |
				       S_030800_SH_INDEX(0) |
				       S_030800_INSTANCE_BROADCAST_WRITES(1));

		/* Order seems important for the following 4 registers. */
		radeon_set_uconfig_reg(cs, R_030CDC_SQ_THREAD_TRACE_BASE2,
				       S_030CDC_ADDR_HI(shifted_va >> 32));

		radeon_set_uconfig_reg(cs, R_030CC0_SQ_THREAD_TRACE_BASE,
				       S_030CC0_ADDR(shifted_va));

		radeon_set_uconfig_reg(cs, R_030CC4_SQ_THREAD_TRACE_SIZE,
				       S_030CC4_SIZE(shifted_size));

		radeon_set_uconfig_reg(cs, R_030CD4_SQ_THREAD_TRACE_CTRL,
				       S_030CD4_RESET_BUFFER(1));

		radeon_set_uconfig_reg(cs, R_030CC8_SQ_THREAD_TRACE_MASK,
				       S_030CC8_CU_SEL(2) |
				       S_030CC8_SH_SEL(0) |
				       S_030CC8_SIMD_EN(0xf) |
				       S_030CC8_VM_ID_MASK(0) |
				       S_030CC8_REG_STALL_EN(1) |
				       S_030CC8_SPI_STALL_EN(1) |
				       S_030CC8_SQ_STALL_EN(1));

		/* Trace all tokens and registers. */
		radeon_set_uconfig_reg(cs, R_030CCC_SQ_THREAD_TRACE_TOKEN_MASK,
				       S_030CCC_TOKEN_MASK(0xbfff) |
				       S_030CCC_REG_MASK(0xff) |
				       S_030CCC_REG_DROP_ON_STALL(0));

		/* Enable SQTT perf counters for all CUs. */
		radeon_set_uconfig_reg(cs, R_030CD0_SQ_THREAD_TRACE_PERF_MASK,
				       S_030CD0_SH0_MASK(0xffff) |
				       S_030CD0_SH1_MASK(0xffff));

		radeon_set_uconfig_reg(cs, R_030CE0_SQ_THREAD_TRACE_TOKEN_MASK2,
				       S_030CE0_INST_MASK(0xffffffff));

		radeon_set_uconfig_reg(cs, R_030CEC_SQ_THREAD_TRACE_HIWATER,
				       S_030CEC_HIWATER(4));

		/* Reset thread trace status errors. */
		radeon_set_uconfig_reg(cs, R_030CE8_SQ_THREAD_TRACE_STATUS,
				       S_030CE8_UTC_ERROR(0));

		/* Enable the thread trace mode. */
		radeon_set_uconfig_reg(cs, R_030CD8_SQ_THREAD_TRACE_MODE,
				       S_030CD8_MASK_PS(1) |
				       S_030CD8_MASK_VS(1) |
				       S_030CD8_MASK_GS(1) |
				       S_030CD8_MASK_ES(1) |
				       S_030CD8_MASK_HS(1) |
				       S_030CD8_MASK_LS(1) |
				       S_030CD8_MASK_CS(1) |
				       S_030CD8_AUTOFLUSH_EN(1) | /* periodically flush SQTT data to memory */
				       S_030CD8_TC_PERF_EN(1) | /* count SQTT traffic in TCC perf counters */
				       S_030CD8_MODE(1));
	}

	/* Restore global broadcasting. */
	radeon_set_uconfig_reg(cs, R_030800_GRBM_GFX_INDEX,
		               S_030800_SE_BROADCAST_WRITES(1) |
			       S_030800_SH_BROADCAST_WRITES(1) |
			       S_030800_INSTANCE_BROADCAST_WRITES(1));

	/* Start the thread trace with a different event based on the queue. */
	if (queue_family_index == RADV_QUEUE_COMPUTE &&
	    device->physical_device->rad_info.chip_class >= GFX7) {
		radeon_set_sh_reg(cs, R_00B878_COMPUTE_THREAD_TRACE_ENABLE,
				  S_00B878_THREAD_TRACE_ENABLE(1));
	} else {
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_THREAD_TRACE_START) | EVENT_INDEX(0));
	}
}

static const uint32_t thread_trace_info_regs[] =
{
	R_030CE4_SQ_THREAD_TRACE_WPTR,
	R_030CE8_SQ_THREAD_TRACE_STATUS,
	R_030CF0_SQ_THREAD_TRACE_CNTR,
};

static void
radv_emit_thread_trace_stop(struct radv_device *device,
			    struct radeon_cmdbuf *cs,
			    uint32_t queue_family_index)
{
	assert(device->physical_device->rad_info.chip_class == GFX9);

	/* Stop the thread trace with a different event based on the queue. */
	if (queue_family_index == RADV_QUEUE_COMPUTE &&
	    device->physical_device->rad_info.chip_class >= GFX7) {
		radeon_set_sh_reg(cs, R_00B878_COMPUTE_THREAD_TRACE_ENABLE,
				  S_00B878_THREAD_TRACE_ENABLE(0));
	} else {
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(V_028A90_THREAD_TRACE_STOP) | EVENT_INDEX(0));
	}

	radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
	radeon_emit(cs, EVENT_TYPE(V_028A90_THREAD_TRACE_FINISH) | EVENT_INDEX(0));

	for (unsigned se = 0; se < 4; se++) {
		/* Target SEi and SH0. */
		radeon_set_uconfig_reg(cs, R_030800_GRBM_GFX_INDEX,
				       S_030800_SE_INDEX(se) |
				       S_030800_SH_INDEX(0) |
				       S_030800_INSTANCE_BROADCAST_WRITES(1));

		/* Disable the thread trace mode. */
		radeon_set_uconfig_reg(cs, R_030CD8_SQ_THREAD_TRACE_MODE,
				       S_030CD8_MODE(0));

		/* Wait for thread trace completion. */
		radeon_emit(cs, PKT3(PKT3_WAIT_REG_MEM, 5, 0));
		radeon_emit(cs, WAIT_REG_MEM_EQUAL); /* wait until the register is equal to the reference value */
		radeon_emit(cs, R_030CE8_SQ_THREAD_TRACE_STATUS >> 2);  /* register */
		radeon_emit(cs, 0);
		radeon_emit(cs, 0); /* reference value */
		radeon_emit(cs, S_030CE8_BUSY(1)); /* mask */
		radeon_emit(cs, 4); /* poll interval */

		/* Get the VA where the info struct is stored for this SE. */
		uint64_t info_va = radv_thread_trace_get_info_va(device, se);

		/* Copy back the info struct one DWORD at a time. */
		for (unsigned i = 0; i < ARRAY_SIZE(thread_trace_info_regs); i++) {
			radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
			radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_PERF) |
					COPY_DATA_DST_SEL(COPY_DATA_TC_L2) |
					COPY_DATA_WR_CONFIRM);
			radeon_emit(cs, thread_trace_info_regs[i] >> 2);
			radeon_emit(cs, 0); /* unused */
			radeon_emit(cs, (info_va + i * 4));
			radeon_emit(cs, (info_va + i * 4) >> 32);
		}
	}

	/* Restore global broadcasting. */
	radeon_set_uconfig_reg(cs, R_030800_GRBM_GFX_INDEX,
		               S_030800_SE_BROADCAST_WRITES(1) |
			       S_030800_SH_BROADCAST_WRITES(1) |
			       S_030800_INSTANCE_BROADCAST_WRITES(1));
}

static void
radv_emit_spi_config_cntl(struct radeon_cmdbuf *cs, bool enable)
{
	radeon_set_uconfig_reg(cs, R_031100_SPI_CONFIG_CNTL,
			       S_031100_GPR_WRITE_PRIORITY(0x2c688) |
			       S_031100_EXP_PRIORITY_ORDER(6) |
			       S_031100_ENABLE_SQG_TOP_EVENTS(enable) |
			       S_031100_ENABLE_SQG_BOP_EVENTS(enable));
}

static void
radv_emit_wait_for_idle(struct radv_device *device,
			struct radeon_cmdbuf *cs, int family)
{
	si_cs_emit_cache_flush(cs, device->physical_device->rad_info.chip_class,
			       NULL, 0,
			       family == RING_COMPUTE &&
			       device->physical_device->rad_info.chip_class >= GFX7,
			       (family == RADV_QUEUE_COMPUTE ?
				RADV_CMD_FLAG_CS_PARTIAL_FLUSH :
				(RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH)) |
			       RADV_CMD_FLAG_INV_ICACHE |
			       RADV_CMD_FLAG_INV_SCACHE |
			       RADV_CMD_FLAG_INV_VCACHE |
			       RADV_CMD_FLAG_INV_L2, 0);
}

static void
radv_thread_trace_init_cs(struct radv_device *device)
{
	struct radeon_winsys *ws = device->ws;

	/* Thread trace start CS. */
	for (int family = 0; family < 2; ++family) {
		device->thread_trace_start_cs[family] = ws->cs_create(ws, family);
		switch (family) {
		case RADV_QUEUE_GENERAL:
			radeon_emit(device->thread_trace_start_cs[family], PKT3(PKT3_CONTEXT_CONTROL, 1, 0));
			radeon_emit(device->thread_trace_start_cs[family], CONTEXT_CONTROL_LOAD_ENABLE(1));
			radeon_emit(device->thread_trace_start_cs[family], CONTEXT_CONTROL_SHADOW_ENABLE(1));
			break;
		case RADV_QUEUE_COMPUTE:
			radeon_emit(device->thread_trace_start_cs[family], PKT3(PKT3_NOP, 0, 0));
			radeon_emit(device->thread_trace_start_cs[family], 0);
			break;
		}

		radv_cs_add_buffer(ws, device->thread_trace_start_cs[family],
				   device->thread_trace_bo);

		/* Make sure to wait-for-idle before starting SQTT. */
		radv_emit_wait_for_idle(device,
					device->thread_trace_start_cs[family],
					family);

		/* Enable SQG events that collects thread trace data. */
		radv_emit_spi_config_cntl(device->thread_trace_start_cs[family], true);

		radv_emit_thread_trace_start(device,
					     device->thread_trace_start_cs[family],
					     family);

		ws->cs_finalize(device->thread_trace_start_cs[family]);
	}

	/* Thread trace stop CS. */
	for (int family = 0; family < 2; ++family) {
		device->thread_trace_stop_cs[family] = ws->cs_create(ws, family);
		switch (family) {
		case RADV_QUEUE_GENERAL:
			radeon_emit(device->thread_trace_stop_cs[family], PKT3(PKT3_CONTEXT_CONTROL, 1, 0));
			radeon_emit(device->thread_trace_stop_cs[family], CONTEXT_CONTROL_LOAD_ENABLE(1));
			radeon_emit(device->thread_trace_stop_cs[family], CONTEXT_CONTROL_SHADOW_ENABLE(1));
			break;
		case RADV_QUEUE_COMPUTE:
			radeon_emit(device->thread_trace_stop_cs[family], PKT3(PKT3_NOP, 0, 0));
			radeon_emit(device->thread_trace_stop_cs[family], 0);
			break;
		}

		radv_cs_add_buffer(ws, device->thread_trace_stop_cs[family],
				   device->thread_trace_bo);

		/* Make sure to wait-for-idle before stopping SQTT. */
		radv_emit_wait_for_idle(device,
					device->thread_trace_stop_cs[family],
					family);

		radv_emit_thread_trace_stop(device,
					    device->thread_trace_stop_cs[family],
					    family);

		/* Restore previous state by disabling SQG events. */
		radv_emit_spi_config_cntl(device->thread_trace_stop_cs[family], false);

		ws->cs_finalize(device->thread_trace_stop_cs[family]);
	}
}

static bool
radv_thread_trace_init_bo(struct radv_device *device)
{
	struct radeon_winsys *ws = device->ws;
	uint64_t size;

	/* Compute total size of the thread trace BO for 4 SEs. */
	size = align64(sizeof(struct radv_thread_trace_info) * 4,
		       1 << SQTT_BUFFER_ALIGN_SHIFT);
	size += device->thread_trace_buffer_size * 4;

	device->thread_trace_bo = ws->buffer_create(ws, size, 4096,
						    RADEON_DOMAIN_VRAM,
						    RADEON_FLAG_CPU_ACCESS |
						    RADEON_FLAG_NO_INTERPROCESS_SHARING |
						    RADEON_FLAG_ZERO_VRAM,
						    RADV_BO_PRIORITY_SCRATCH);
	if (!device->thread_trace_bo)
		return false;

	device->thread_trace_ptr = ws->buffer_map(device->thread_trace_bo);
	if (!device->thread_trace_ptr)
		return false;

	return true;
}

bool
radv_thread_trace_init(struct radv_device *device)
{
	if (!radv_thread_trace_init_bo(device))
		return false;

	radv_thread_trace_init_cs(device);
	return true;
}

void
radv_thread_trace_finish(struct radv_device *device)
{
	struct radeon_winsys *ws = device->ws;

	if (unlikely(device->thread_trace_bo))
		ws->buffer_destroy(device->thread_trace_bo);

	for (unsigned i = 0; i < 2; i++) {
		if (device->thread_trace_start_cs[i])
			ws->cs_destroy(device->thread_trace_start_cs[i]);
		if (device->thread_trace_stop_cs[i])
			ws->cs_destroy(device->thread_trace_stop_cs[i]);
	}
}

bool
radv_begin_thread_trace(struct radv_queue *queue)
{
	int family = queue->queue_family_index;
	struct radeon_cmdbuf *cs = queue->device->thread_trace_start_cs[family];
	return radv_queue_internal_submit(queue, cs);
}

bool
radv_end_thread_trace(struct radv_queue *queue)
{
	int family = queue->queue_family_index;
	struct radeon_cmdbuf *cs = queue->device->thread_trace_stop_cs[family];
	return radv_queue_internal_submit(queue, cs);
}

bool
radv_get_thread_trace(struct radv_queue *queue,
		      struct radv_thread_trace *thread_trace)
{
	struct radv_device *device = queue->device;
	void *thread_trace_ptr = device->thread_trace_ptr;

	memset(thread_trace, 0, sizeof(*thread_trace));
	thread_trace->num_traces = 4;

	for (unsigned se = 0; se < 4; se++) {
		uint64_t info_offset = radv_thread_trace_get_info_offset(se);
		uint64_t data_offset = radv_thread_trace_get_data_offset(device, se);
		void *info_ptr = thread_trace_ptr + info_offset;
		void *data_ptr = thread_trace_ptr + data_offset;
		struct radv_thread_trace_info *info =
			(struct radv_thread_trace_info *)info_ptr;
		struct radv_thread_trace_se thread_trace_se = {};

		if (info->cur_offset < info->write_counter) {
			uint32_t expected_size =
				(info->write_counter * 32) / 1024;
			uint32_t available_size =
				(info->cur_offset * 32) / 1024;

			fprintf(stderr, "Failed to get the thread trace "
					"because the buffer is too small. The "
					"hardware needs %d KB but the "
					"buffer size is %d KB.\n",
					expected_size, available_size);
			fprintf(stderr, "Please update the buffer size with "
					"RADV_THREAD_TRACE_BUFER_SIZE=<size_in_bytes>\n");
			return false;
		}

		thread_trace_se.data_ptr = data_ptr;
		thread_trace_se.info = *info;
		thread_trace_se.shader_engine = se;
		thread_trace_se.compute_unit = 0;

		thread_trace->traces[se] = thread_trace_se;
	}

	return true;
}

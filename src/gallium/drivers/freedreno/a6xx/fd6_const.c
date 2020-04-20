/*
 * Copyright (C) 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2018 Google, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "fd6_const.h"
#include "fd6_pack.h"

#include "ir3_const.h"

/* regid:          base const register
 * prsc or dwords: buffer containing constant values
 * sizedwords:     size of const value buffer
 */
static void
fd6_emit_const(struct fd_ringbuffer *ring, gl_shader_stage type,
		uint32_t regid, uint32_t offset, uint32_t sizedwords,
		const uint32_t *dwords, struct pipe_resource *prsc)
{
	if (prsc) {
		struct fd_bo *bo = fd_resource(prsc)->bo;

		if (fd6_geom_stage(type)) {
			OUT_PKT(ring, CP_LOAD_STATE6_GEOM,
					CP_LOAD_STATE6_0(
							.dst_off     = regid/4,
							.state_type  = ST6_CONSTANTS,
							.state_src   = SS6_INDIRECT,
							.state_block = fd6_stage2shadersb(type),
							.num_unit    = DIV_ROUND_UP(sizedwords, 4)
						),
					CP_LOAD_STATE6_EXT_SRC_ADDR(
							.bo          = bo,
							.bo_offset   = offset
						)
				);
		} else {
			OUT_PKT(ring, CP_LOAD_STATE6_FRAG,
					CP_LOAD_STATE6_0(
							.dst_off     = regid/4,
							.state_type  = ST6_CONSTANTS,
							.state_src   = SS6_INDIRECT,
							.state_block = fd6_stage2shadersb(type),
							.num_unit    = DIV_ROUND_UP(sizedwords, 4)
						),
					CP_LOAD_STATE6_EXT_SRC_ADDR(
							.bo          = bo,
							.bo_offset   = offset
						)
				);
		}
	} else {
		/* NOTE we cheat a bit here, since we know mesa is aligning
		 * the size of the user buffer to 16 bytes.  And we want to
		 * cut cycles in a hot path.
		 */
		uint32_t align_sz = align(sizedwords, 4);
		dwords = (uint32_t *)&((uint8_t *)dwords)[offset];

		if (fd6_geom_stage(type)) {
			OUT_PKTBUF(ring, CP_LOAD_STATE6_GEOM, dwords, align_sz,
					CP_LOAD_STATE6_0(
							.dst_off     = regid/4,
							.state_type  = ST6_CONSTANTS,
							.state_src   = SS6_DIRECT,
							.state_block = fd6_stage2shadersb(type),
							.num_unit    = DIV_ROUND_UP(sizedwords, 4)
						),
					CP_LOAD_STATE6_1(),
					CP_LOAD_STATE6_2()
				);
		} else {
			OUT_PKTBUF(ring, CP_LOAD_STATE6_FRAG, dwords, align_sz,
					CP_LOAD_STATE6_0(
							.dst_off     = regid/4,
							.state_type  = ST6_CONSTANTS,
							.state_src   = SS6_DIRECT,
							.state_block = fd6_stage2shadersb(type),
							.num_unit    = DIV_ROUND_UP(sizedwords, 4)
						),
					CP_LOAD_STATE6_1(),
					CP_LOAD_STATE6_2()
				);
		}
	}
}

static bool
is_stateobj(struct fd_ringbuffer *ring)
{
	return true;
}

void
emit_const(struct fd_ringbuffer *ring,
		const struct ir3_shader_variant *v, uint32_t dst_offset,
		uint32_t offset, uint32_t size, const void *user_buffer,
		struct pipe_resource *buffer)
{
	/* TODO inline this */
	assert(dst_offset + size <= v->constlen * 4);
	fd6_emit_const(ring, v->type, dst_offset,
			offset, size, user_buffer, buffer);
}

static void
emit_const_bo(struct fd_ringbuffer *ring,
		const struct ir3_shader_variant *v, uint32_t dst_offset,
		uint32_t num, struct pipe_resource **prscs, uint32_t *offsets)
{
	unreachable("shouldn't be called on a6xx");
}

static void
emit_tess_bos(struct fd_ringbuffer *ring, struct fd6_emit *emit, struct ir3_shader_variant *s)
{
	struct fd_context *ctx = emit->ctx;
	const unsigned regid = s->shader->const_state.offsets.primitive_param * 4 + 4;
	uint32_t dwords = 16;

	OUT_PKT7(ring, fd6_stage2opcode(s->type), 3);
	OUT_RING(ring, CP_LOAD_STATE6_0_DST_OFF(regid / 4) |
			CP_LOAD_STATE6_0_STATE_TYPE(ST6_CONSTANTS)|
			CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
			CP_LOAD_STATE6_0_STATE_BLOCK(fd6_stage2shadersb(s->type)) |
			CP_LOAD_STATE6_0_NUM_UNIT(dwords / 4));
	OUT_RB(ring, ctx->batch->tess_addrs_constobj);
}

static void
emit_stage_tess_consts(struct fd_ringbuffer *ring, struct ir3_shader_variant *v,
		uint32_t *params, int num_params)
{
	const unsigned regid = v->shader->const_state.offsets.primitive_param;
	int size = MIN2(1 + regid, v->constlen) - regid;
	if (size > 0)
		fd6_emit_const(ring, v->type, regid * 4, 0, num_params, params, NULL);
}

static void
emit_tess_consts(struct fd6_emit *emit)
{
	struct fd_context *ctx = emit->ctx;

	struct fd_ringbuffer *constobj = fd_submit_new_ringbuffer(
		ctx->batch->submit, 0x1000, FD_RINGBUFFER_STREAMING);

	/* VS sizes are in bytes since that's what STLW/LDLW use, while the HS
	 * size is dwords, since that's what LDG/STG use.
	 */
	unsigned num_vertices =
		emit->hs ?
		emit->info->vertices_per_patch :
		emit->gs->shader->nir->info.gs.vertices_in;

	uint32_t vs_params[4] = {
		emit->vs->shader->output_size * num_vertices * 4,	/* vs primitive stride */
		emit->vs->shader->output_size * 4,					/* vs vertex stride */
		0,
		0
	};

	emit_stage_tess_consts(constobj, emit->vs, vs_params, ARRAY_SIZE(vs_params));

	if (emit->hs) {
		uint32_t hs_params[4] = {
			emit->vs->shader->output_size * num_vertices * 4,	/* vs primitive stride */
			emit->vs->shader->output_size * 4,					/* vs vertex stride */
			emit->hs->shader->output_size,
			emit->info->vertices_per_patch
		};

		emit_stage_tess_consts(constobj, emit->hs, hs_params, ARRAY_SIZE(hs_params));
		emit_tess_bos(constobj, emit, emit->hs);

		if (emit->gs)
			num_vertices = emit->gs->shader->nir->info.gs.vertices_in;

		uint32_t ds_params[4] = {
			emit->ds->shader->output_size * num_vertices * 4,	/* ds primitive stride */
			emit->ds->shader->output_size * 4,					/* ds vertex stride */
			emit->hs->shader->output_size,                      /* hs vertex stride (dwords) */
			emit->hs->shader->nir->info.tess.tcs_vertices_out
		};

		emit_stage_tess_consts(constobj, emit->ds, ds_params, ARRAY_SIZE(ds_params));
		emit_tess_bos(constobj, emit, emit->ds);
	}

	if (emit->gs) {
		struct ir3_shader_variant *prev;
		if (emit->ds)
			prev = emit->ds;
		else
			prev = emit->vs;

		uint32_t gs_params[4] = {
			prev->shader->output_size * num_vertices * 4,	/* ds primitive stride */
			prev->shader->output_size * 4,					/* ds vertex stride */
			0,
			0,
		};

		num_vertices = emit->gs->shader->nir->info.gs.vertices_in;
		emit_stage_tess_consts(constobj, emit->gs, gs_params, ARRAY_SIZE(gs_params));
	}

	fd6_emit_take_group(emit, constobj, FD6_GROUP_PRIMITIVE_PARAMS, ENABLE_ALL);
}

static void
fd6_emit_ubos(const struct ir3_shader_variant *v,
		struct fd_ringbuffer *ring, struct fd_constbuf_stateobj *constbuf)
{
	if (!v->shader->num_ubos)
		return;

	int num_ubos = v->shader->num_ubos;

	OUT_PKT7(ring, fd6_stage2opcode(v->type), 3 + (2 * num_ubos));
	OUT_RING(ring, CP_LOAD_STATE6_0_DST_OFF(0) |
			CP_LOAD_STATE6_0_STATE_TYPE(ST6_UBO)|
			CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
			CP_LOAD_STATE6_0_STATE_BLOCK(fd6_stage2shadersb(v->type)) |
			CP_LOAD_STATE6_0_NUM_UNIT(num_ubos));
	OUT_RING(ring, CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
	OUT_RING(ring, CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));

	for (int i = 0; i < num_ubos; i++) {
		/* Note: gallium constbuf 0 was always lowered to hardware constbuf,
		 * and UBO load indices decremented by one.
		 */
		struct pipe_constant_buffer *cb = &constbuf->cb[i + 1];
		if (cb->buffer) {
			int size_vec4s = DIV_ROUND_UP(cb->buffer_size, 16);
			OUT_RELOC(ring, fd_resource(cb->buffer)->bo,
					cb->buffer_offset,
					(uint64_t)A6XX_UBO_1_SIZE(size_vec4s) << 32,
					0);
		} else {
			OUT_RING(ring, 0xbad00000 | (i << 16));
			OUT_RING(ring, 0xbad00000 | (i << 16));
		}
	}
}

static void
emit_user_consts(struct fd6_emit *emit)
{
	static const enum pipe_shader_type types[] = {
			PIPE_SHADER_VERTEX, PIPE_SHADER_TESS_CTRL, PIPE_SHADER_TESS_EVAL,
			PIPE_SHADER_GEOMETRY, PIPE_SHADER_FRAGMENT,
	};
	const struct ir3_shader_variant *variants[] = {
			emit->vs, emit->hs, emit->ds, emit->gs, emit->fs,
	};
	struct fd_context *ctx = emit->ctx;
	unsigned sz = 0;

	for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
		if (!variants[i])
			continue;
		sz += variants[i]->shader->ubo_state.cmdstream_size;
	}

	struct fd_ringbuffer *constobj = fd_submit_new_ringbuffer(
			ctx->batch->submit, sz, FD_RINGBUFFER_STREAMING);

	for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
		if (!variants[i])
			continue;
		ir3_emit_user_consts(ctx->screen, variants[i], constobj, &ctx->constbuf[types[i]]);
		fd6_emit_ubos(variants[i], constobj, &ctx->constbuf[types[i]]);
	}

	fd6_emit_take_group(emit, constobj, FD6_GROUP_CONST, ENABLE_ALL);
}

void
fd6_emit_consts(struct fd6_emit *emit)
{
	struct fd_context *ctx = emit->ctx;
	struct fd6_context *fd6_ctx = fd6_context(ctx);

	if (emit->dirty & (FD_DIRTY_CONST | FD_DIRTY_PROG))
		emit_user_consts(emit);

	if (emit->key.key.has_gs || emit->key.key.tessellation)
		emit_tess_consts(emit);

	/* if driver-params are needed, emit each time: */
	const struct ir3_shader_variant *vs = emit->vs;
	if (ir3_needs_vs_driver_params(vs)) {
		struct fd_ringbuffer *dpconstobj = fd_submit_new_ringbuffer(
				ctx->batch->submit, IR3_DP_VS_COUNT * 4, FD_RINGBUFFER_STREAMING);
		ir3_emit_vs_driver_params(vs, dpconstobj, ctx, emit->info);
		fd6_emit_take_group(emit, dpconstobj, FD6_GROUP_VS_DRIVER_PARAMS, ENABLE_ALL);
		fd6_ctx->has_dp_state = true;
	} else if (fd6_ctx->has_dp_state) {
		fd6_emit_take_group(emit, NULL, FD6_GROUP_VS_DRIVER_PARAMS, ENABLE_ALL);
		fd6_ctx->has_dp_state = false;
	}
}

void
fd6_emit_ibo_consts(struct fd6_emit *emit, const struct ir3_shader_variant *v,
		enum pipe_shader_type stage, struct fd_ringbuffer *ring)
{
	struct fd_context *ctx = emit->ctx;

	ir3_emit_ssbo_sizes(ctx->screen, v, ring, &ctx->shaderbuf[stage]);
	ir3_emit_image_dims(ctx->screen, v, ring, &ctx->shaderimg[stage]);
}

void
fd6_emit_cs_consts(const struct ir3_shader_variant *v, struct fd_ringbuffer *ring,
		struct fd_context *ctx, const struct pipe_grid_info *info)
{
	ir3_emit_cs_consts(v, ring, ctx, info);
	fd6_emit_ubos(v, ring, &ctx->constbuf[PIPE_SHADER_COMPUTE]);
}

void
fd6_emit_immediates(struct fd_screen *screen, const struct ir3_shader_variant *v,
		struct fd_ringbuffer *ring)
{
	ir3_emit_immediates(screen, v, ring);
}

void
fd6_user_consts_size(struct ir3_ubo_analysis_state *state,
		unsigned *packets, unsigned *size)
{
	ir3_user_consts_size(state, packets, size);
}

void
fd6_emit_link_map(struct fd_screen *screen,
		const struct ir3_shader_variant *producer,
		const struct ir3_shader_variant *v, struct fd_ringbuffer *ring)
{
	ir3_emit_link_map(screen, producer, v, ring);
}

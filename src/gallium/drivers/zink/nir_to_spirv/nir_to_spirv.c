/*
 * Copyright 2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "nir_to_spirv.h"
#include "spirv_builder.h"

#include "nir.h"
#include "pipe/p_state.h"
#include "util/u_memory.h"

struct ntv_context {
   struct spirv_builder builder;

   SpvId GLSL_std_450;

   gl_shader_stage stage;
   SpvId inputs[PIPE_MAX_SHADER_INPUTS][4];
   SpvId input_types[PIPE_MAX_SHADER_INPUTS][4];
   SpvId outputs[PIPE_MAX_SHADER_OUTPUTS][4];
   SpvId output_types[PIPE_MAX_SHADER_OUTPUTS][4];

   SpvId ubos[128];
   size_t num_ubos;
   SpvId samplers[PIPE_MAX_SAMPLERS];
   size_t num_samplers;
   SpvId entry_ifaces[PIPE_MAX_SHADER_INPUTS * 4 + PIPE_MAX_SHADER_OUTPUTS * 4];
   size_t num_entry_ifaces;

   SpvId *defs;
   size_t num_defs;

   struct hash_table *vars;

   const SpvId *block_ids;
   size_t num_blocks;
   bool block_started;
   SpvId loop_break, loop_cont;
};

static SpvId
get_fvec_constant(struct ntv_context *ctx, int bit_size, int num_components,
                  const float values[]);

static SpvId
get_uvec_constant(struct ntv_context *ctx, int bit_size, int num_components,
                  const uint32_t values[]);

static SpvId
emit_unop(struct ntv_context *ctx, SpvOp op, SpvId type, SpvId src);

static SpvId
emit_binop(struct ntv_context *ctx, SpvOp op, SpvId type,
           SpvId src0, SpvId src1);

static SpvId
emit_triop(struct ntv_context *ctx, SpvOp op, SpvId type,
           SpvId src0, SpvId src1, SpvId src2);

static SpvId
get_bvec_type(struct ntv_context *ctx, int num_components)
{
   SpvId bool_type = spirv_builder_type_bool(&ctx->builder);
   if (num_components > 1)
      return spirv_builder_type_vector(&ctx->builder, bool_type,
                                       num_components);

   assert(num_components == 1);
   return bool_type;
}

static SpvId
block_label(struct ntv_context *ctx, nir_block *block)
{
   assert(block->index < ctx->num_blocks);
   return ctx->block_ids[block->index];
}

static SpvId
get_fvec_type(struct ntv_context *ctx, unsigned bit_size, unsigned num_components)
{
   assert(bit_size == 32); // only 32-bit floats supported so far

   SpvId float_type = spirv_builder_type_float(&ctx->builder, bit_size);
   if (num_components > 1)
      return spirv_builder_type_vector(&ctx->builder, float_type,
                                       num_components);

   assert(num_components == 1);
   return float_type;
}

static SpvId
get_ivec_type(struct ntv_context *ctx, unsigned bit_size, unsigned num_components)
{
   assert(bit_size == 32); // only 32-bit ints supported so far

   SpvId int_type = spirv_builder_type_int(&ctx->builder, bit_size);
   if (num_components > 1)
      return spirv_builder_type_vector(&ctx->builder, int_type,
                                       num_components);

   assert(num_components == 1);
   return int_type;
}

static SpvId
get_uvec_type(struct ntv_context *ctx, unsigned bit_size, unsigned num_components)
{
   assert(bit_size == 32); // only 32-bit uints supported so far

   SpvId uint_type = spirv_builder_type_uint(&ctx->builder, bit_size);
   if (num_components > 1)
      return spirv_builder_type_vector(&ctx->builder, uint_type,
                                       num_components);

   assert(num_components == 1);
   return uint_type;
}

static SpvId
get_dest_uvec_type(struct ntv_context *ctx, nir_dest *dest)
{
   return get_uvec_type(ctx, nir_dest_bit_size(*dest),
                             nir_dest_num_components(*dest));
}

static SpvId
get_glsl_basetype(struct ntv_context *ctx, enum glsl_base_type type)
{
   switch (type) {
   case GLSL_TYPE_FLOAT:
      return spirv_builder_type_float(&ctx->builder, 32);

   case GLSL_TYPE_INT:
      return spirv_builder_type_int(&ctx->builder, 32);

   case GLSL_TYPE_UINT:
      return spirv_builder_type_uint(&ctx->builder, 32);
   /* TODO: handle more types */

   default:
      unreachable("unknown GLSL type");
   }
}

static SpvId
get_glsl_type(struct ntv_context *ctx, const struct glsl_type *type)
{
   assert(type);
   if (glsl_type_is_scalar(type))
      return get_glsl_basetype(ctx, glsl_get_base_type(type));

   if (glsl_type_is_vector(type))
      return spirv_builder_type_vector(&ctx->builder,
         get_glsl_basetype(ctx, glsl_get_base_type(type)),
         glsl_get_vector_elements(type));

   unreachable("we shouldn't get here, I think...");
}

static void
emit_input(struct ntv_context *ctx, struct nir_variable *var)
{
   SpvId vec_type = get_glsl_type(ctx, var->type);
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassInput,
                                                   vec_type);
   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         SpvStorageClassInput);

   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);

   if (ctx->stage == MESA_SHADER_FRAGMENT) {
      switch (var->data.location) {
      case VARYING_SLOT_POS:
         spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInFragCoord);
         break;

      case VARYING_SLOT_PNTC:
         spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInPointCoord);
         break;

      default:
         spirv_builder_emit_location(&ctx->builder, var_id,
                                     var->data.driver_location);
         break;
      }
   } else {
      spirv_builder_emit_location(&ctx->builder, var_id,
                                  var->data.driver_location);
   }

   if (var->data.location_frac)
      spirv_builder_emit_component(&ctx->builder, var_id,
                                   var->data.location_frac);

   if (var->data.interpolation == INTERP_MODE_FLAT)
      spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationFlat);

   assert(var->data.driver_location < PIPE_MAX_SHADER_INPUTS);
   assert(var->data.location_frac < 4);
   assert(ctx->inputs[var->data.driver_location][var->data.location_frac] == 0);
   ctx->inputs[var->data.driver_location][var->data.location_frac] = var_id;
   ctx->input_types[var->data.driver_location][var->data.location_frac] = vec_type;

   assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
   ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
}

static void
emit_output(struct ntv_context *ctx, struct nir_variable *var)
{
   SpvId vec_type = get_glsl_type(ctx, var->type);
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassOutput,
                                                   vec_type);
   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         SpvStorageClassOutput);
   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);


   if (ctx->stage == MESA_SHADER_VERTEX) {
      switch (var->data.location) {
      case VARYING_SLOT_POS:
         spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInPosition);
         break;

      case VARYING_SLOT_PSIZ:
         spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInPointSize);
         break;

      default:
         spirv_builder_emit_location(&ctx->builder, var_id,
                                     var->data.driver_location - 1);
      }
   } else if (ctx->stage == MESA_SHADER_FRAGMENT) {
      switch (var->data.location) {
      case FRAG_RESULT_DEPTH:
         spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInFragDepth);
         break;

      default:
         spirv_builder_emit_location(&ctx->builder, var_id,
                                     var->data.driver_location);
      }
   }

   if (var->data.location_frac)
      spirv_builder_emit_component(&ctx->builder, var_id,
                                   var->data.location_frac);

   assert(var->data.driver_location < PIPE_MAX_SHADER_INPUTS);
   assert(var->data.location_frac < 4);
   assert(ctx->outputs[var->data.driver_location][var->data.location_frac] == 0);
   ctx->outputs[var->data.driver_location][var->data.location_frac] = var_id;
   ctx->output_types[var->data.driver_location][var->data.location_frac] = vec_type;

   assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
   ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
}

static SpvDim
type_to_dim(enum glsl_sampler_dim gdim, bool *is_ms)
{
   *is_ms = false;
   switch (gdim) {
   case GLSL_SAMPLER_DIM_1D:
      return SpvDim1D;
   case GLSL_SAMPLER_DIM_2D:
      return SpvDim2D;
   case GLSL_SAMPLER_DIM_RECT:
      return SpvDimRect;
   case GLSL_SAMPLER_DIM_CUBE:
      return SpvDimCube;
   case GLSL_SAMPLER_DIM_3D:
      return SpvDim3D;
   case GLSL_SAMPLER_DIM_MS:
      *is_ms = true;
      return SpvDim2D;
   default:
      fprintf(stderr, "unknown sampler type %d\n", gdim);
      break;
   }
   return SpvDim2D;
}

static void
emit_sampler(struct ntv_context *ctx, struct nir_variable *var)
{
   bool is_ms;
   SpvDim dimension = type_to_dim(glsl_get_sampler_dim(var->type), &is_ms);
   SpvId float_type = spirv_builder_type_float(&ctx->builder, 32);
   SpvId image_type = spirv_builder_type_image(&ctx->builder, float_type,
                            dimension, false, glsl_sampler_type_is_array(var->type), is_ms, 1,
                            SpvImageFormatUnknown);

   SpvId sampled_type = spirv_builder_type_sampled_image(&ctx->builder,
                                                         image_type);
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassUniformConstant,
                                                   sampled_type);
   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         SpvStorageClassUniformConstant);

   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);

   assert(ctx->num_samplers < ARRAY_SIZE(ctx->samplers));
   ctx->samplers[ctx->num_samplers++] = var_id;

   spirv_builder_emit_descriptor_set(&ctx->builder, var_id,
                                     var->data.descriptor_set);
   spirv_builder_emit_binding(&ctx->builder, var_id, var->data.binding);
}

static void
emit_ubo(struct ntv_context *ctx, struct nir_variable *var)
{
   uint32_t size = glsl_count_attribute_slots(var->type, false);
   SpvId vec4_type = get_uvec_type(ctx, 32, 4);
   SpvId array_length = spirv_builder_const_uint(&ctx->builder, 32, size);
   SpvId array_type = spirv_builder_type_array(&ctx->builder, vec4_type,
                                               array_length);
   spirv_builder_emit_array_stride(&ctx->builder, array_type, 16);

   // wrap UBO-array in a struct
   SpvId struct_type = spirv_builder_type_struct(&ctx->builder, &array_type, 1);
   if (var->name) {
      char struct_name[100];
      snprintf(struct_name, sizeof(struct_name), "struct_%s", var->name);
      spirv_builder_emit_name(&ctx->builder, struct_type, struct_name);
   }

   spirv_builder_emit_decoration(&ctx->builder, struct_type,
                                 SpvDecorationBlock);
   spirv_builder_emit_member_offset(&ctx->builder, struct_type, 0, 0);


   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassUniform,
                                                   struct_type);

   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         SpvStorageClassUniform);
   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);

   assert(ctx->num_ubos < ARRAY_SIZE(ctx->ubos));
   ctx->ubos[ctx->num_ubos++] = var_id;

   spirv_builder_emit_descriptor_set(&ctx->builder, var_id,
                                     var->data.descriptor_set);
   spirv_builder_emit_binding(&ctx->builder, var_id, var->data.binding);
}

static void
emit_uniform(struct ntv_context *ctx, struct nir_variable *var)
{
   if (glsl_type_is_sampler(var->type))
      emit_sampler(ctx, var);
   else if (var->interface_type)
      emit_ubo(ctx, var);
}

static SpvId
get_src_uint_ssa(struct ntv_context *ctx, const nir_ssa_def *ssa)
{
   assert(ssa->index < ctx->num_defs);
   assert(ctx->defs[ssa->index] != 0);
   return ctx->defs[ssa->index];
}

static SpvId
get_var_from_reg(struct ntv_context *ctx, nir_register *reg)
{
   struct hash_entry *he = _mesa_hash_table_search(ctx->vars, reg);
   assert(he);
   return (SpvId)(intptr_t)he->data;
}

static SpvId
get_src_uint_reg(struct ntv_context *ctx, const nir_reg_src *reg)
{
   assert(reg->reg);
   assert(!reg->indirect);
   assert(!reg->base_offset);

   SpvId var = get_var_from_reg(ctx, reg->reg);
   SpvId type = get_uvec_type(ctx, reg->reg->bit_size, reg->reg->num_components);
   return spirv_builder_emit_load(&ctx->builder, type, var);
}

static SpvId
get_src_uint(struct ntv_context *ctx, nir_src *src)
{
   if (src->is_ssa)
      return get_src_uint_ssa(ctx, src->ssa);
   else
      return get_src_uint_reg(ctx, &src->reg);
}

static SpvId
get_alu_src_uint(struct ntv_context *ctx, nir_alu_instr *alu, unsigned src)
{
   assert(!alu->src[src].negate);
   assert(!alu->src[src].abs);

   SpvId def = get_src_uint(ctx, &alu->src[src].src);

   unsigned used_channels = 0;
   bool need_swizzle = false;
   for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; i++) {
      if (!nir_alu_instr_channel_used(alu, src, i))
         continue;

      used_channels++;

      if (alu->src[src].swizzle[i] != i)
         need_swizzle = true;
   }
   assert(used_channels != 0);

   unsigned live_channels = nir_src_num_components(alu->src[src].src);
   if (used_channels != live_channels)
      need_swizzle = true;

   if (!need_swizzle)
      return def;

   int bit_size = nir_src_bit_size(alu->src[src].src);

   SpvId uint_type = spirv_builder_type_uint(&ctx->builder, bit_size);
   if (used_channels == 1) {
      uint32_t indices[] =  { alu->src[src].swizzle[0] };
      return spirv_builder_emit_composite_extract(&ctx->builder, uint_type,
                                                  def, indices,
                                                  ARRAY_SIZE(indices));
   } else if (live_channels == 1) {
      SpvId uvec_type = spirv_builder_type_vector(&ctx->builder, uint_type,
                                                  used_channels);

      SpvId constituents[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < used_channels; ++i)
        constituents[i] = def;

      return spirv_builder_emit_composite_construct(&ctx->builder, uvec_type,
                                                    constituents,
                                                    used_channels);
   } else {
      SpvId uvec_type = spirv_builder_type_vector(&ctx->builder, uint_type,
                                                  used_channels);

      uint32_t components[NIR_MAX_VEC_COMPONENTS];
      size_t num_components = 0;
      for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; i++) {
         if (!nir_alu_instr_channel_used(alu, src, i))
            continue;

         components[num_components++] = alu->src[src].swizzle[i];
      }

      return spirv_builder_emit_vector_shuffle(&ctx->builder, uvec_type,
                                        def, def, components, num_components);
   }
}

static void
store_ssa_def_uint(struct ntv_context *ctx, nir_ssa_def *ssa, SpvId result)
{
   assert(result != 0);
   assert(ssa->index < ctx->num_defs);
   ctx->defs[ssa->index] = result;
}

static SpvId
bvec_to_uvec(struct ntv_context *ctx, SpvId value, unsigned num_components)
{
   SpvId otype = get_uvec_type(ctx, 32, num_components);
   uint32_t zeros[4] = { 0, 0, 0, 0 };
   uint32_t ones[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
   SpvId zero = get_uvec_constant(ctx, 32, num_components, zeros);
   SpvId one = get_uvec_constant(ctx, 32, num_components, ones);
   return emit_triop(ctx, SpvOpSelect, otype, value, one, zero);
}

static SpvId
uvec_to_bvec(struct ntv_context *ctx, SpvId value, unsigned num_components)
{
   SpvId type = get_bvec_type(ctx, num_components);

   uint32_t zeros[NIR_MAX_VEC_COMPONENTS] = { 0 };
   SpvId zero = get_uvec_constant(ctx, 32, num_components, zeros);

   return emit_binop(ctx, SpvOpINotEqual, type, value, zero);
}

static SpvId
bitcast_to_uvec(struct ntv_context *ctx, SpvId value, unsigned bit_size,
                unsigned num_components)
{
   SpvId type = get_uvec_type(ctx, bit_size, num_components);
   return emit_unop(ctx, SpvOpBitcast, type, value);
}

static SpvId
bitcast_to_ivec(struct ntv_context *ctx, SpvId value, unsigned bit_size,
                unsigned num_components)
{
   SpvId type = get_ivec_type(ctx, bit_size, num_components);
   return emit_unop(ctx, SpvOpBitcast, type, value);
}

static SpvId
bitcast_to_fvec(struct ntv_context *ctx, SpvId value, unsigned bit_size,
               unsigned num_components)
{
   SpvId type = get_fvec_type(ctx, bit_size, num_components);
   return emit_unop(ctx, SpvOpBitcast, type, value);
}

static void
store_reg_def(struct ntv_context *ctx, nir_reg_dest *reg, SpvId result)
{
   SpvId var = get_var_from_reg(ctx, reg->reg);
   assert(var);
   spirv_builder_emit_store(&ctx->builder, var, result);
}

static void
store_dest_uint(struct ntv_context *ctx, nir_dest *dest, SpvId result)
{
   if (dest->is_ssa)
      store_ssa_def_uint(ctx, &dest->ssa, result);
   else
      store_reg_def(ctx, &dest->reg, result);
}

static void
store_dest(struct ntv_context *ctx, nir_dest *dest, SpvId result, nir_alu_type type)
{
   unsigned num_components = nir_dest_num_components(*dest);
   unsigned bit_size = nir_dest_bit_size(*dest);

   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_bool:
      assert(bit_size == 1);
      result = bvec_to_uvec(ctx, result, num_components);
      break;

   case nir_type_uint:
      break; /* nothing to do! */

   case nir_type_int:
   case nir_type_float:
      result = bitcast_to_uvec(ctx, result, bit_size, num_components);
      break;

   default:
      unreachable("unsupported nir_alu_type");
   }

   store_dest_uint(ctx, dest, result);
}

static SpvId
emit_unop(struct ntv_context *ctx, SpvOp op, SpvId type, SpvId src)
{
   return spirv_builder_emit_unop(&ctx->builder, op, type, src);
}

static SpvId
emit_binop(struct ntv_context *ctx, SpvOp op, SpvId type,
           SpvId src0, SpvId src1)
{
   return spirv_builder_emit_binop(&ctx->builder, op, type, src0, src1);
}

static SpvId
emit_triop(struct ntv_context *ctx, SpvOp op, SpvId type,
           SpvId src0, SpvId src1, SpvId src2)
{
   return spirv_builder_emit_triop(&ctx->builder, op, type, src0, src1, src2);
}

static SpvId
emit_builtin_unop(struct ntv_context *ctx, enum GLSLstd450 op, SpvId type,
                  SpvId src)
{
   SpvId args[] = { src };
   return spirv_builder_emit_ext_inst(&ctx->builder, type, ctx->GLSL_std_450,
                                      op, args, ARRAY_SIZE(args));
}

static SpvId
emit_builtin_binop(struct ntv_context *ctx, enum GLSLstd450 op, SpvId type,
                   SpvId src0, SpvId src1)
{
   SpvId args[] = { src0, src1 };
   return spirv_builder_emit_ext_inst(&ctx->builder, type, ctx->GLSL_std_450,
                                      op, args, ARRAY_SIZE(args));
}

static SpvId
get_fvec_constant(struct ntv_context *ctx, int bit_size, int num_components,
                  const float values[])
{
   assert(bit_size == 32);

   if (num_components > 1) {
      SpvId components[num_components];
      for (int i = 0; i < num_components; i++)
         components[i] = spirv_builder_const_float(&ctx->builder, bit_size,
                                                   values[i]);

      SpvId type = get_fvec_type(ctx, bit_size, num_components);
      return spirv_builder_const_composite(&ctx->builder, type, components,
                                           num_components);
   }

   assert(num_components == 1);
   return spirv_builder_const_float(&ctx->builder, bit_size, values[0]);
}

static SpvId
get_uvec_constant(struct ntv_context *ctx, int bit_size, int num_components,
                  const uint32_t values[])
{
   assert(bit_size == 32);

   if (num_components > 1) {
      SpvId components[num_components];
      for (int i = 0; i < num_components; i++)
         components[i] = spirv_builder_const_uint(&ctx->builder, bit_size,
                                                  values[i]);

      SpvId type = get_uvec_type(ctx, bit_size, num_components);
      return spirv_builder_const_composite(&ctx->builder, type, components,
                                           num_components);
   }

   assert(num_components == 1);
   return spirv_builder_const_uint(&ctx->builder, bit_size, values[0]);
}

static inline unsigned
alu_instr_src_components(const nir_alu_instr *instr, unsigned src)
{
   if (nir_op_infos[instr->op].input_sizes[src] > 0)
      return nir_op_infos[instr->op].input_sizes[src];

   if (instr->dest.dest.is_ssa)
      return instr->dest.dest.ssa.num_components;
   else
      return instr->dest.dest.reg.reg->num_components;
}

static SpvId
get_alu_src(struct ntv_context *ctx, nir_alu_instr *alu, unsigned src)
{
   SpvId uint_value = get_alu_src_uint(ctx, alu, src);

   unsigned num_components = alu_instr_src_components(alu, src);
   unsigned bit_size = nir_src_bit_size(alu->src[src].src);
   nir_alu_type type = nir_op_infos[alu->op].input_types[src];

   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_bool:
      assert(bit_size == 1);
      return uvec_to_bvec(ctx, uint_value, num_components);

   case nir_type_int:
      return bitcast_to_ivec(ctx, uint_value, bit_size, num_components);

   case nir_type_uint:
      return uint_value;

   case nir_type_float:
      return bitcast_to_fvec(ctx, uint_value, bit_size, num_components);

   default:
      unreachable("unknown nir_alu_type");
   }
}

static void
store_alu_result(struct ntv_context *ctx, nir_alu_instr *alu, SpvId result)
{
   assert(!alu->dest.saturate);
   return store_dest(ctx, &alu->dest.dest, result, nir_op_infos[alu->op].output_type);
}

static SpvId
get_dest_type(struct ntv_context *ctx, nir_dest *dest, nir_alu_type type)
{
   unsigned num_components = nir_dest_num_components(*dest);
   unsigned bit_size = nir_dest_bit_size(*dest);

   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_bool:
      return get_bvec_type(ctx, num_components);

   case nir_type_int:
      return get_ivec_type(ctx, bit_size, num_components);

   case nir_type_uint:
      return get_uvec_type(ctx, bit_size, num_components);

   case nir_type_float:
      return get_fvec_type(ctx, bit_size, num_components);

   default:
      unreachable("unsupported nir_alu_type");
   }
}

static void
emit_alu(struct ntv_context *ctx, nir_alu_instr *alu)
{
   SpvId src[nir_op_infos[alu->op].num_inputs];
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
      src[i] = get_alu_src(ctx, alu, i);

   SpvId dest_type = get_dest_type(ctx, &alu->dest.dest,
                                   nir_op_infos[alu->op].output_type);
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);
   unsigned num_components = nir_dest_num_components(alu->dest.dest);

   SpvId result = 0;
   switch (alu->op) {
   case nir_op_mov:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = src[0];
      break;

#define UNOP(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 1); \
      result = emit_unop(ctx, spirv_op, dest_type, src[0]); \
      break;

#define BUILTIN_UNOP(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 1); \
      result = emit_builtin_unop(ctx, spirv_op, dest_type, src[0]); \
      break;

   UNOP(nir_op_fneg, SpvOpFNegate)
   UNOP(nir_op_fddx, SpvOpDPdx)
   UNOP(nir_op_fddy, SpvOpDPdy)

   BUILTIN_UNOP(nir_op_fabs, GLSLstd450FAbs)
   BUILTIN_UNOP(nir_op_fsqrt, GLSLstd450Sqrt)
   BUILTIN_UNOP(nir_op_frsq, GLSLstd450InverseSqrt)
   BUILTIN_UNOP(nir_op_flog2, GLSLstd450Log2)
   BUILTIN_UNOP(nir_op_fexp2, GLSLstd450Exp2)
   BUILTIN_UNOP(nir_op_ffract, GLSLstd450Fract)
   BUILTIN_UNOP(nir_op_ffloor, GLSLstd450Floor)
   BUILTIN_UNOP(nir_op_fceil, GLSLstd450Ceil)
   BUILTIN_UNOP(nir_op_ftrunc, GLSLstd450Trunc)
   BUILTIN_UNOP(nir_op_fround_even, GLSLstd450RoundEven)
   BUILTIN_UNOP(nir_op_fsign, GLSLstd450FSign)
   BUILTIN_UNOP(nir_op_fsin, GLSLstd450Sin)
   BUILTIN_UNOP(nir_op_fcos, GLSLstd450Cos)

   case nir_op_frcp: {
      assert(nir_op_infos[alu->op].num_inputs == 1);
      float one[4] = { 1, 1, 1, 1 };
      src[1] = src[0];
      src[0] = get_fvec_constant(ctx, bit_size, num_components, one);
      result = emit_binop(ctx, SpvOpFDiv, dest_type, src[0], src[1]);
      }
      break;

#undef UNOP
#undef BUILTIN_UNOP

#define BINOP(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 2); \
      result = emit_binop(ctx, spirv_op, dest_type, src[0], src[1]); \
      break;

#define BUILTIN_BINOP(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 2); \
      result = emit_builtin_binop(ctx, spirv_op, dest_type, src[0], src[1]); \
      break;

   BINOP(nir_op_iadd, SpvOpIAdd)
   BINOP(nir_op_isub, SpvOpISub)
   BINOP(nir_op_imul, SpvOpIMul)
   BINOP(nir_op_fadd, SpvOpFAdd)
   BINOP(nir_op_fsub, SpvOpFSub)
   BINOP(nir_op_fmul, SpvOpFMul)
   BINOP(nir_op_fmod, SpvOpFMod)
   BINOP(nir_op_flt, SpvOpFUnordLessThan)
   BINOP(nir_op_fge, SpvOpFUnordGreaterThanEqual)

   BUILTIN_BINOP(nir_op_fmin, GLSLstd450FMin)
   BUILTIN_BINOP(nir_op_fmax, GLSLstd450FMax)

#undef BINOP
#undef BUILTIN_BINOP

   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
      assert(nir_op_infos[alu->op].num_inputs == 2);
      result = emit_binop(ctx, SpvOpDot, dest_type, src[0], src[1]);
      break;

   case nir_op_seq:
   case nir_op_sne:
   case nir_op_slt:
   case nir_op_sge: {
      assert(nir_op_infos[alu->op].num_inputs == 2);
      int num_components = nir_dest_num_components(alu->dest.dest);
      SpvId bool_type = get_bvec_type(ctx, num_components);

      SpvId zero = spirv_builder_const_float(&ctx->builder, 32, 0.0f);
      SpvId one = spirv_builder_const_float(&ctx->builder, 32, 1.0f);
      if (num_components > 1) {
         SpvId zero_comps[num_components], one_comps[num_components];
         for (int i = 0; i < num_components; i++) {
            zero_comps[i] = zero;
            one_comps[i] = one;
         }

         zero = spirv_builder_const_composite(&ctx->builder, dest_type,
                                              zero_comps, num_components);
         one = spirv_builder_const_composite(&ctx->builder, dest_type,
                                             one_comps, num_components);
      }

      SpvOp op;
      switch (alu->op) {
      case nir_op_seq: op = SpvOpFOrdEqual; break;
      case nir_op_sne: op = SpvOpFOrdNotEqual; break;
      case nir_op_slt: op = SpvOpFOrdLessThan; break;
      case nir_op_sge: op = SpvOpFOrdGreaterThanEqual; break;
      default: unreachable("unexpected op");
      }

      result = emit_binop(ctx, op, bool_type, src[0], src[1]);
      result = emit_triop(ctx, SpvOpSelect, dest_type, result, one, zero);
      }
      break;

   case nir_op_fcsel: {
      assert(nir_op_infos[alu->op].num_inputs == 3);
      int num_components = nir_dest_num_components(alu->dest.dest);
      SpvId bool_type = get_bvec_type(ctx, num_components);

      float zero[4] = { 0, 0, 0, 0 };
      SpvId cmp = get_fvec_constant(ctx, nir_src_bit_size(alu->src[0].src),
                                         num_components, zero);

      result = emit_binop(ctx, SpvOpFOrdGreaterThan, bool_type, src[0], cmp);
      result = emit_triop(ctx, SpvOpSelect, dest_type, result, src[1], src[2]);
      }
      break;

   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4: {
      int num_inputs = nir_op_infos[alu->op].num_inputs;
      assert(2 <= num_inputs && num_inputs <= 4);
      result = spirv_builder_emit_composite_construct(&ctx->builder, dest_type,
                                                      src, num_inputs);
   }
   break;

   default:
      fprintf(stderr, "emit_alu: not implemented (%s)\n",
              nir_op_infos[alu->op].name);

      unreachable("unsupported opcode");
      return;
   }

   store_alu_result(ctx, alu, result);
}

static void
emit_load_const(struct ntv_context *ctx, nir_load_const_instr *load_const)
{
   uint32_t values[NIR_MAX_VEC_COMPONENTS];
   for (int i = 0; i < load_const->def.num_components; ++i)
      values[i] = load_const->value[i].u32;

   SpvId constant = get_uvec_constant(ctx, load_const->def.bit_size,
                                           load_const->def.num_components,
                                           values);
   store_ssa_def_uint(ctx, &load_const->def, constant);
}

static void
emit_load_input(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   nir_const_value *const_offset = nir_src_as_const_value(intr->src[0]);
   if (const_offset) {
      int driver_location = (int)nir_intrinsic_base(intr) + const_offset->u32;
      assert(driver_location < PIPE_MAX_SHADER_INPUTS);
      int location_frac = nir_intrinsic_component(intr);
      assert(location_frac < 4);

      SpvId ptr = ctx->inputs[driver_location][location_frac];
      SpvId type = ctx->input_types[driver_location][location_frac];
      assert(ptr && type);

      SpvId result = spirv_builder_emit_load(&ctx->builder, type, ptr);

      unsigned num_components = nir_dest_num_components(intr->dest);
      unsigned bit_size = nir_dest_bit_size(intr->dest);
      result = bitcast_to_uvec(ctx, result, bit_size, num_components);

      store_dest_uint(ctx, &intr->dest, result);
   } else
      unreachable("input-addressing not yet supported");
}

static void
emit_load_ubo(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   nir_const_value *const_block_index = nir_src_as_const_value(intr->src[0]);
   assert(const_block_index); // no dynamic indexing for now
   assert(const_block_index->u32 == 0); // we only support the default UBO for now

   nir_const_value *const_offset = nir_src_as_const_value(intr->src[1]);
   if (const_offset) {
      SpvId uvec4_type = get_uvec_type(ctx, 32, 4);
      SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                      SpvStorageClassUniform,
                                                      uvec4_type);

      unsigned idx = const_offset->u32;
      SpvId member = spirv_builder_const_uint(&ctx->builder, 32, 0);
      SpvId offset = spirv_builder_const_uint(&ctx->builder, 32, idx);
      SpvId offsets[] = { member, offset };
      SpvId ptr = spirv_builder_emit_access_chain(&ctx->builder, pointer_type,
                                                  ctx->ubos[0], offsets,
                                                  ARRAY_SIZE(offsets));
      SpvId result = spirv_builder_emit_load(&ctx->builder, uvec4_type, ptr);

      SpvId type = get_dest_uvec_type(ctx, &intr->dest);
      unsigned num_components = nir_dest_num_components(intr->dest);
      if (num_components == 1) {
         uint32_t components[] = { 0 };
         result = spirv_builder_emit_composite_extract(&ctx->builder,
                                                       type,
                                                       result, components,
                                                       1);
      } else if (num_components < 4) {
         SpvId constituents[num_components];
         SpvId uint_type = spirv_builder_type_uint(&ctx->builder, 32);
         for (uint32_t i = 0; i < num_components; ++i)
            constituents[i] = spirv_builder_emit_composite_extract(&ctx->builder,
                                                                   uint_type,
                                                                   result, &i,
                                                                   1);

         result = spirv_builder_emit_composite_construct(&ctx->builder,
                                                         type,
                                                         constituents,
                                                         num_components);
      }

      store_dest_uint(ctx, &intr->dest, result);
   } else
      unreachable("uniform-addressing not yet supported");
}

static void
emit_store_output(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   nir_const_value *const_offset = nir_src_as_const_value(intr->src[1]);
   if (const_offset) {
      int driver_location = (int)nir_intrinsic_base(intr) + const_offset->u32;
      assert(driver_location < PIPE_MAX_SHADER_OUTPUTS);
      int location_frac = nir_intrinsic_component(intr);
      assert(location_frac < 4);

      SpvId ptr = ctx->outputs[driver_location][location_frac];
      assert(ptr > 0);

      SpvId src = get_src_uint(ctx, &intr->src[0]);
      SpvId spirv_type = ctx->output_types[driver_location][location_frac];
      SpvId result = emit_unop(ctx, SpvOpBitcast, spirv_type, src);
      spirv_builder_emit_store(&ctx->builder, ptr, result);
   } else
      unreachable("output-addressing not yet supported");
}

static void
emit_discard(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   assert(ctx->block_started);
   spirv_builder_emit_kill(&ctx->builder);
   /* discard is weird in NIR, so let's just create an unreachable block after
      it and hope that the vulkan driver will DCE any instructinos in it. */
   spirv_builder_label(&ctx->builder, spirv_builder_new_id(&ctx->builder));
}

static void
emit_intrinsic(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      emit_load_input(ctx, intr);
      break;

   case nir_intrinsic_load_ubo:
      emit_load_ubo(ctx, intr);
      break;

   case nir_intrinsic_store_output:
      emit_store_output(ctx, intr);
      break;

   case nir_intrinsic_discard:
      emit_discard(ctx, intr);
      break;

   default:
      fprintf(stderr, "emit_intrinsic: not implemented (%s)\n",
              nir_intrinsic_infos[intr->intrinsic].name);
      unreachable("unsupported intrinsic");
   }
}

static void
emit_undef(struct ntv_context *ctx, nir_ssa_undef_instr *undef)
{
   SpvId type = get_uvec_type(ctx, undef->def.bit_size,
                              undef->def.num_components);

   store_ssa_def_uint(ctx, &undef->def,
                      spirv_builder_emit_undef(&ctx->builder, type));
}

static SpvId
get_src_float(struct ntv_context *ctx, nir_src *src)
{
   SpvId def = get_src_uint(ctx, src);
   unsigned num_components = nir_src_num_components(*src);
   unsigned bit_size = nir_src_bit_size(*src);
   return bitcast_to_fvec(ctx, def, bit_size, num_components);
}

static void
emit_tex(struct ntv_context *ctx, nir_tex_instr *tex)
{
   assert(tex->op == nir_texop_tex);
   assert(nir_alu_type_get_base_type(tex->dest_type) == nir_type_float);
   assert(tex->texture_index == tex->sampler_index);

   bool has_proj = false, has_lod = false;
   SpvId coord = 0, proj, lod;
   unsigned coord_components;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      switch (tex->src[i].src_type) {
      case nir_tex_src_coord:
         coord = get_src_float(ctx, &tex->src[i].src);
         coord_components = nir_src_num_components(tex->src[i].src);
         break;

      case nir_tex_src_projector:
         has_proj = true;
         proj = get_src_float(ctx, &tex->src[i].src);
         assert(nir_src_num_components(tex->src[i].src) == 1);
         break;

      case nir_tex_src_lod:
         has_lod = true;
         lod = get_src_float(ctx, &tex->src[i].src);
         assert(nir_src_num_components(tex->src[i].src) == 1);
         break;

      default:
         fprintf(stderr, "texture source: %d\n", tex->src[i].src_type);
         unreachable("unknown texture source");
      }
   }

   if (!has_lod && ctx->stage != MESA_SHADER_FRAGMENT) {
      has_lod = true;
      lod = spirv_builder_const_float(&ctx->builder, 32, 0);
   }

   bool is_ms;
   SpvDim dimension = type_to_dim(tex->sampler_dim, &is_ms);
   SpvId float_type = spirv_builder_type_float(&ctx->builder, 32);
   SpvId image_type = spirv_builder_type_image(&ctx->builder, float_type,
                            dimension, false, tex->is_array, is_ms, 1,
                            SpvImageFormatUnknown);
   SpvId sampled_type = spirv_builder_type_sampled_image(&ctx->builder,
                                                         image_type);

   assert(tex->texture_index < ctx->num_samplers);
   SpvId load = spirv_builder_emit_load(&ctx->builder, sampled_type,
                                        ctx->samplers[tex->texture_index]);

   SpvId dest_type = get_dest_type(ctx, &tex->dest, tex->dest_type);

   SpvId result;
   if (has_proj) {
      SpvId constituents[coord_components + 1];
      SpvId float_type = spirv_builder_type_float(&ctx->builder, 32);
      for (uint32_t i = 0; i < coord_components; ++i)
         constituents[i] = spirv_builder_emit_composite_extract(&ctx->builder,
                                              float_type,
                                              coord,
                                              &i, 1);

      constituents[coord_components++] = proj;

      SpvId vec_type = get_fvec_type(ctx, 32, coord_components);
      SpvId merged = spirv_builder_emit_composite_construct(&ctx->builder,
                                                            vec_type,
                                                            constituents,
                                                            coord_components);

      if (has_lod)
         result = spirv_builder_emit_image_sample_proj_explicit_lod(&ctx->builder,
                                                                    dest_type,
                                                                    load,
                                                                    merged,
                                                                    lod);
      else
         result = spirv_builder_emit_image_sample_proj_implicit_lod(&ctx->builder,
                                                                    dest_type,
                                                                    load,
                                                                    merged);
   } else {
      if (has_lod)
         result = spirv_builder_emit_image_sample_explicit_lod(&ctx->builder,
                                                               dest_type,
                                                               load,
                                                               coord, lod);
      else
         result = spirv_builder_emit_image_sample_implicit_lod(&ctx->builder,
                                                               dest_type,
                                                               load,
                                                               coord);
   }
   spirv_builder_emit_decoration(&ctx->builder, result,
                                 SpvDecorationRelaxedPrecision);

   store_dest(ctx, &tex->dest, result, tex->dest_type);
}

static void
start_block(struct ntv_context *ctx, SpvId label)
{
   /* terminate previous block if needed */
   if (ctx->block_started)
      spirv_builder_emit_branch(&ctx->builder, label);

   /* start new block */
   spirv_builder_label(&ctx->builder, label);
   ctx->block_started = true;
}

static void
branch(struct ntv_context *ctx, SpvId label)
{
   assert(ctx->block_started);
   spirv_builder_emit_branch(&ctx->builder, label);
   ctx->block_started = false;
}

static void
branch_conditional(struct ntv_context *ctx, SpvId condition, SpvId then_id,
                   SpvId else_id)
{
   assert(ctx->block_started);
   spirv_builder_emit_branch_conditional(&ctx->builder, condition,
                                         then_id, else_id);
   ctx->block_started = false;
}

static void
emit_jump(struct ntv_context *ctx, nir_jump_instr *jump)
{
   switch (jump->type) {
   case nir_jump_break:
      assert(ctx->loop_break);
      branch(ctx, ctx->loop_break);
      break;

   case nir_jump_continue:
      assert(ctx->loop_cont);
      branch(ctx, ctx->loop_cont);
      break;

   default:
      unreachable("Unsupported jump type\n");
   }
}

static void
emit_block(struct ntv_context *ctx, struct nir_block *block)
{
   start_block(ctx, block_label(ctx, block));
   nir_foreach_instr(instr, block) {
      switch (instr->type) {
      case nir_instr_type_alu:
         emit_alu(ctx, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_intrinsic:
         emit_intrinsic(ctx, nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_load_const:
         emit_load_const(ctx, nir_instr_as_load_const(instr));
         break;
      case nir_instr_type_ssa_undef:
         emit_undef(ctx, nir_instr_as_ssa_undef(instr));
         break;
      case nir_instr_type_tex:
         emit_tex(ctx, nir_instr_as_tex(instr));
         break;
      case nir_instr_type_phi:
         unreachable("nir_instr_type_phi not supported");
         break;
      case nir_instr_type_jump:
         emit_jump(ctx, nir_instr_as_jump(instr));
         break;
      case nir_instr_type_call:
         unreachable("nir_instr_type_call not supported");
         break;
      case nir_instr_type_parallel_copy:
         unreachable("nir_instr_type_parallel_copy not supported");
         break;
      case nir_instr_type_deref:
         unreachable("nir_instr_type_deref not supported");
         break;
      }
   }
}

static void
emit_cf_list(struct ntv_context *ctx, struct exec_list *list);

static SpvId
get_src_bool(struct ntv_context *ctx, nir_src *src)
{
   SpvId def = get_src_uint(ctx, src);
   assert(nir_src_bit_size(*src) == 32);
   unsigned num_components = nir_src_num_components(*src);
   return uvec_to_bvec(ctx, def, num_components);
}

static void
emit_if(struct ntv_context *ctx, nir_if *if_stmt)
{
   SpvId condition = get_src_bool(ctx, &if_stmt->condition);

   SpvId header_id = spirv_builder_new_id(&ctx->builder);
   SpvId then_id = block_label(ctx, nir_if_first_then_block(if_stmt));
   SpvId endif_id = spirv_builder_new_id(&ctx->builder);
   SpvId else_id = endif_id;

   bool has_else = !exec_list_is_empty(&if_stmt->else_list);
   if (has_else) {
      assert(nir_if_first_else_block(if_stmt)->index < ctx->num_blocks);
      else_id = block_label(ctx, nir_if_first_else_block(if_stmt));
   }

   /* create a header-block */
   start_block(ctx, header_id);
   spirv_builder_emit_selection_merge(&ctx->builder, endif_id,
                                      SpvSelectionControlMaskNone);
   branch_conditional(ctx, condition, then_id, else_id);

   emit_cf_list(ctx, &if_stmt->then_list);

   if (has_else) {
      if (ctx->block_started)
         branch(ctx, endif_id);

      emit_cf_list(ctx, &if_stmt->else_list);
   }

   start_block(ctx, endif_id);
}

static void
emit_loop(struct ntv_context *ctx, nir_loop *loop)
{
   SpvId header_id = spirv_builder_new_id(&ctx->builder);
   SpvId begin_id = block_label(ctx, nir_loop_first_block(loop));
   SpvId break_id = spirv_builder_new_id(&ctx->builder);
   SpvId cont_id = spirv_builder_new_id(&ctx->builder);

   /* create a header-block */
   start_block(ctx, header_id);
   spirv_builder_loop_merge(&ctx->builder, break_id, cont_id, SpvLoopControlMaskNone);
   branch(ctx, begin_id);

   SpvId save_break = ctx->loop_break;
   SpvId save_cont = ctx->loop_cont;
   ctx->loop_break = break_id;
   ctx->loop_cont = cont_id;

   emit_cf_list(ctx, &loop->body);

   ctx->loop_break = save_break;
   ctx->loop_cont = save_cont;

   branch(ctx, cont_id);
   start_block(ctx, cont_id);
   branch(ctx, header_id);

   start_block(ctx, break_id);
}

static void
emit_cf_list(struct ntv_context *ctx, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         emit_block(ctx, nir_cf_node_as_block(node));
         break;

      case nir_cf_node_if:
         emit_if(ctx, nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         emit_loop(ctx, nir_cf_node_as_loop(node));
         break;

      case nir_cf_node_function:
         unreachable("nir_cf_node_function not supported");
         break;
      }
   }
}

struct spirv_shader *
nir_to_spirv(struct nir_shader *s)
{
   struct spirv_shader *ret = NULL;

   struct ntv_context ctx = {};

   switch (s->info.stage) {
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_FRAGMENT:
   case MESA_SHADER_COMPUTE:
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilityShader);
      break;

   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilityTessellation);
      break;

   case MESA_SHADER_GEOMETRY:
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilityGeometry);
      break;

   default:
      unreachable("invalid stage");
   }

   ctx.stage = s->info.stage;
   ctx.GLSL_std_450 = spirv_builder_import(&ctx.builder, "GLSL.std.450");
   spirv_builder_emit_source(&ctx.builder, SpvSourceLanguageGLSL, 450);

   spirv_builder_emit_mem_model(&ctx.builder, SpvAddressingModelLogical,
                                SpvMemoryModelGLSL450);

   SpvExecutionModel exec_model;
   switch (s->info.stage) {
   case MESA_SHADER_VERTEX:
      exec_model = SpvExecutionModelVertex;
      break;
   case MESA_SHADER_TESS_CTRL:
      exec_model = SpvExecutionModelTessellationControl;
      break;
   case MESA_SHADER_TESS_EVAL:
      exec_model = SpvExecutionModelTessellationEvaluation;
      break;
   case MESA_SHADER_GEOMETRY:
      exec_model = SpvExecutionModelGeometry;
      break;
   case MESA_SHADER_FRAGMENT:
      exec_model = SpvExecutionModelFragment;
      break;
   case MESA_SHADER_COMPUTE:
      exec_model = SpvExecutionModelGLCompute;
      break;
   default:
      unreachable("invalid stage");
   }

   SpvId type_void = spirv_builder_type_void(&ctx.builder);
   SpvId type_main = spirv_builder_type_function(&ctx.builder, type_void,
                                                 NULL, 0);
   SpvId entry_point = spirv_builder_new_id(&ctx.builder);
   spirv_builder_emit_name(&ctx.builder, entry_point, "main");

   nir_foreach_variable(var, &s->inputs)
      emit_input(&ctx, var);

   nir_foreach_variable(var, &s->outputs)
      emit_output(&ctx, var);

   nir_foreach_variable(var, &s->uniforms)
      emit_uniform(&ctx, var);

   spirv_builder_emit_entry_point(&ctx.builder, exec_model, entry_point,
                                  "main", ctx.entry_ifaces,
                                  ctx.num_entry_ifaces);
   if (s->info.stage == MESA_SHADER_FRAGMENT)
      spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                   SpvExecutionModeOriginUpperLeft);


   spirv_builder_function(&ctx.builder, entry_point, type_void,
                                            SpvFunctionControlMaskNone,
                                            type_main);

   nir_function_impl *entry = nir_shader_get_entrypoint(s);
   nir_metadata_require(entry, nir_metadata_block_index);

   ctx.defs = (SpvId *)malloc(sizeof(SpvId) * entry->ssa_alloc);
   if (!ctx.defs)
      goto fail;
   ctx.num_defs = entry->ssa_alloc;

   ctx.vars = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                            _mesa_key_pointer_equal);
   if (!ctx.vars)
      goto fail;

   SpvId *block_ids = (SpvId *)malloc(sizeof(SpvId) * entry->num_blocks);
   if (!block_ids)
      goto fail;

   for (int i = 0; i < entry->num_blocks; ++i)
      block_ids[i] = spirv_builder_new_id(&ctx.builder);

   ctx.block_ids = block_ids;
   ctx.num_blocks = entry->num_blocks;

   /* emit a block only for the variable declarations */
   start_block(&ctx, spirv_builder_new_id(&ctx.builder));
   foreach_list_typed(nir_register, reg, node, &entry->registers) {
      SpvId type = get_uvec_type(&ctx, reg->bit_size, reg->num_components);
      SpvId pointer_type = spirv_builder_type_pointer(&ctx.builder,
                                                      SpvStorageClassFunction,
                                                      type);
      SpvId var = spirv_builder_emit_var(&ctx.builder, pointer_type,
                                         SpvStorageClassFunction);

      if (!_mesa_hash_table_insert(ctx.vars, reg, (void *)(intptr_t)var))
         goto fail;
   }

   emit_cf_list(&ctx, &entry->body);

   free(ctx.defs);

   spirv_builder_return(&ctx.builder); // doesn't belong here, but whatevz
   spirv_builder_function_end(&ctx.builder);

   size_t num_words = spirv_builder_get_num_words(&ctx.builder);

   ret = CALLOC_STRUCT(spirv_shader);
   if (!ret)
      goto fail;

   ret->words = MALLOC(sizeof(uint32_t) * num_words);
   if (!ret->words)
      goto fail;

   ret->num_words = spirv_builder_get_words(&ctx.builder, ret->words, num_words);
   assert(ret->num_words == num_words);

   return ret;

fail:

   if (ret)
      spirv_shader_delete(ret);

   return NULL;
}

void
spirv_shader_delete(struct spirv_shader *s)
{
   FREE(s->words);
   FREE(s);
}

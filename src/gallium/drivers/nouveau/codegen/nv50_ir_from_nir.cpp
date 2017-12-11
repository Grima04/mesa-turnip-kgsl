/*
 * Copyright 2017 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Karol Herbst <kherbst@redhat.com>
 */

#include "compiler/nir/nir.h"

#include "util/u_debug.h"

#include "codegen/nv50_ir.h"
#include "codegen/nv50_ir_from_common.h"
#include "codegen/nv50_ir_lowering_helper.h"
#include "codegen/nv50_ir_util.h"

#if __cplusplus >= 201103L
#include <unordered_map>
#else
#include <tr1/unordered_map>
#endif
#include <vector>

namespace {

#if __cplusplus >= 201103L
using std::hash;
using std::unordered_map;
#else
using std::tr1::hash;
using std::tr1::unordered_map;
#endif

using namespace nv50_ir;

int
type_size(const struct glsl_type *type)
{
   return glsl_count_attribute_slots(type, false);
}

class Converter : public ConverterCommon
{
public:
   Converter(Program *, nir_shader *, nv50_ir_prog_info *);

   bool run();
private:
   typedef std::vector<LValue *> LValues;
   typedef unordered_map<unsigned, LValues> NirDefMap;

   LValues& convert(nir_alu_dest *);
   LValues& convert(nir_dest *);
   LValues& convert(nir_register *);
   LValues& convert(nir_ssa_def *);

   Value* getSrc(nir_alu_src *, uint8_t component = 0);
   Value* getSrc(nir_register *, uint8_t);
   Value* getSrc(nir_src *, uint8_t, bool indirect = false);
   Value* getSrc(nir_ssa_def *, uint8_t);

   // returned value is the constant part of the given source (either the
   // nir_src or the selected source component of an intrinsic). Even though
   // this is mostly an optimization to be able to skip indirects in a few
   // cases, sometimes we require immediate values or set some fileds on
   // instructions (e.g. tex) in order for codegen to consume those.
   // If the found value has not a constant part, the Value gets returned
   // through the Value parameter.
   uint32_t getIndirect(nir_src *, uint8_t, Value *&);
   uint32_t getIndirect(nir_intrinsic_instr *, uint8_t s, uint8_t c, Value *&);

   uint32_t getSlotAddress(nir_intrinsic_instr *, uint8_t idx, uint8_t slot);

   void setInterpolate(nv50_ir_varying *,
                       uint8_t,
                       bool centroid,
                       unsigned semantics);

   bool isFloatType(nir_alu_type);
   bool isSignedType(nir_alu_type);
   bool isResultFloat(nir_op);
   bool isResultSigned(nir_op);

   DataType getDType(nir_alu_instr *);
   DataType getDType(nir_intrinsic_instr *);
   DataType getDType(nir_op, uint8_t);

   std::vector<DataType> getSTypes(nir_alu_instr *);
   DataType getSType(nir_src &, bool isFloat, bool isSigned);

   bool assignSlots();

   nir_shader *nir;

   NirDefMap ssaDefs;
   NirDefMap regDefs;
};

Converter::Converter(Program *prog, nir_shader *nir, nv50_ir_prog_info *info)
   : ConverterCommon(prog, info),
     nir(nir) {}

bool
Converter::isFloatType(nir_alu_type type)
{
   return nir_alu_type_get_base_type(type) == nir_type_float;
}

bool
Converter::isSignedType(nir_alu_type type)
{
   return nir_alu_type_get_base_type(type) == nir_type_int;
}

bool
Converter::isResultFloat(nir_op op)
{
   const nir_op_info &info = nir_op_infos[op];
   if (info.output_type != nir_type_invalid)
      return isFloatType(info.output_type);

   ERROR("isResultFloat not implemented for %s\n", nir_op_infos[op].name);
   assert(false);
   return true;
}

bool
Converter::isResultSigned(nir_op op)
{
   switch (op) {
   // there is no umul and we get wrong results if we treat all muls as signed
   case nir_op_imul:
   case nir_op_inot:
      return false;
   default:
      const nir_op_info &info = nir_op_infos[op];
      if (info.output_type != nir_type_invalid)
         return isSignedType(info.output_type);
      ERROR("isResultSigned not implemented for %s\n", nir_op_infos[op].name);
      assert(false);
      return true;
   }
}

DataType
Converter::getDType(nir_alu_instr *insn)
{
   if (insn->dest.dest.is_ssa)
      return getDType(insn->op, insn->dest.dest.ssa.bit_size);
   else
      return getDType(insn->op, insn->dest.dest.reg.reg->bit_size);
}

DataType
Converter::getDType(nir_intrinsic_instr *insn)
{
   if (insn->dest.is_ssa)
      return typeOfSize(insn->dest.ssa.bit_size / 8, false, false);
   else
      return typeOfSize(insn->dest.reg.reg->bit_size / 8, false, false);
}

DataType
Converter::getDType(nir_op op, uint8_t bitSize)
{
   DataType ty = typeOfSize(bitSize / 8, isResultFloat(op), isResultSigned(op));
   if (ty == TYPE_NONE) {
      ERROR("couldn't get Type for op %s with bitSize %u\n", nir_op_infos[op].name, bitSize);
      assert(false);
   }
   return ty;
}

std::vector<DataType>
Converter::getSTypes(nir_alu_instr *insn)
{
   const nir_op_info &info = nir_op_infos[insn->op];
   std::vector<DataType> res(info.num_inputs);

   for (uint8_t i = 0; i < info.num_inputs; ++i) {
      if (info.input_types[i] != nir_type_invalid) {
         res[i] = getSType(insn->src[i].src, isFloatType(info.input_types[i]), isSignedType(info.input_types[i]));
      } else {
         ERROR("getSType not implemented for %s idx %u\n", info.name, i);
         assert(false);
         res[i] = TYPE_NONE;
         break;
      }
   }

   return res;
}

DataType
Converter::getSType(nir_src &src, bool isFloat, bool isSigned)
{
   uint8_t bitSize;
   if (src.is_ssa)
      bitSize = src.ssa->bit_size;
   else
      bitSize = src.reg.reg->bit_size;

   DataType ty = typeOfSize(bitSize / 8, isFloat, isSigned);
   if (ty == TYPE_NONE) {
      const char *str;
      if (isFloat)
         str = "float";
      else if (isSigned)
         str = "int";
      else
         str = "uint";
      ERROR("couldn't get Type for %s with bitSize %u\n", str, bitSize);
      assert(false);
   }
   return ty;
}

Converter::LValues&
Converter::convert(nir_dest *dest)
{
   if (dest->is_ssa)
      return convert(&dest->ssa);
   if (dest->reg.indirect) {
      ERROR("no support for indirects.");
      assert(false);
   }
   return convert(dest->reg.reg);
}

Converter::LValues&
Converter::convert(nir_register *reg)
{
   NirDefMap::iterator it = regDefs.find(reg->index);
   if (it != regDefs.end())
      return it->second;

   LValues newDef(reg->num_components);
   for (uint8_t i = 0; i < reg->num_components; i++)
      newDef[i] = getScratch(std::max(4, reg->bit_size / 8));
   return regDefs[reg->index] = newDef;
}

Converter::LValues&
Converter::convert(nir_ssa_def *def)
{
   NirDefMap::iterator it = ssaDefs.find(def->index);
   if (it != ssaDefs.end())
      return it->second;

   LValues newDef(def->num_components);
   for (uint8_t i = 0; i < def->num_components; i++)
      newDef[i] = getSSA(std::max(4, def->bit_size / 8));
   return ssaDefs[def->index] = newDef;
}

Value*
Converter::getSrc(nir_alu_src *src, uint8_t component)
{
   if (src->abs || src->negate) {
      ERROR("modifiers currently not supported on nir_alu_src\n");
      assert(false);
   }
   return getSrc(&src->src, src->swizzle[component]);
}

Value*
Converter::getSrc(nir_register *reg, uint8_t idx)
{
   NirDefMap::iterator it = regDefs.find(reg->index);
   if (it == regDefs.end())
      return convert(reg)[idx];
   return it->second[idx];
}

Value*
Converter::getSrc(nir_src *src, uint8_t idx, bool indirect)
{
   if (src->is_ssa)
      return getSrc(src->ssa, idx);

   if (src->reg.indirect) {
      if (indirect)
         return getSrc(src->reg.indirect, idx);
      ERROR("no support for indirects.");
      assert(false);
      return NULL;
   }

   return getSrc(src->reg.reg, idx);
}

Value*
Converter::getSrc(nir_ssa_def *src, uint8_t idx)
{
   NirDefMap::iterator it = ssaDefs.find(src->index);
   if (it == ssaDefs.end()) {
      ERROR("SSA value %u not found\n", src->index);
      assert(false);
      return NULL;
   }
   return it->second[idx];
}

uint32_t
Converter::getIndirect(nir_src *src, uint8_t idx, Value *&indirect)
{
   nir_const_value *offset = nir_src_as_const_value(*src);

   if (offset) {
      indirect = NULL;
      return offset->u32[0];
   }

   indirect = getSrc(src, idx, true);
   return 0;
}

uint32_t
Converter::getIndirect(nir_intrinsic_instr *insn, uint8_t s, uint8_t c, Value *&indirect)
{
   int32_t idx = nir_intrinsic_base(insn) + getIndirect(&insn->src[s], c, indirect);
   if (indirect)
      indirect = mkOp2v(OP_SHL, TYPE_U32, getSSA(4, FILE_ADDRESS), indirect, loadImm(NULL, 4));
   return idx;
}

static void
vert_attrib_to_tgsi_semantic(gl_vert_attrib slot, unsigned *name, unsigned *index)
{
   assert(name && index);

   if (slot >= VERT_ATTRIB_MAX) {
      ERROR("invalid varying slot %u\n", slot);
      assert(false);
      return;
   }

   if (slot >= VERT_ATTRIB_GENERIC0 &&
       slot < VERT_ATTRIB_GENERIC0 + VERT_ATTRIB_GENERIC_MAX) {
      *name = TGSI_SEMANTIC_GENERIC;
      *index = slot - VERT_ATTRIB_GENERIC0;
      return;
   }

   if (slot >= VERT_ATTRIB_TEX0 &&
       slot < VERT_ATTRIB_TEX0 + VERT_ATTRIB_TEX_MAX) {
      *name = TGSI_SEMANTIC_TEXCOORD;
      *index = slot - VERT_ATTRIB_TEX0;
      return;
   }

   switch (slot) {
   case VERT_ATTRIB_COLOR0:
      *name = TGSI_SEMANTIC_COLOR;
      *index = 0;
      break;
   case VERT_ATTRIB_COLOR1:
      *name = TGSI_SEMANTIC_COLOR;
      *index = 1;
      break;
   case VERT_ATTRIB_EDGEFLAG:
      *name = TGSI_SEMANTIC_EDGEFLAG;
      *index = 0;
      break;
   case VERT_ATTRIB_FOG:
      *name = TGSI_SEMANTIC_FOG;
      *index = 0;
      break;
   case VERT_ATTRIB_NORMAL:
      *name = TGSI_SEMANTIC_NORMAL;
      *index = 0;
      break;
   case VERT_ATTRIB_POS:
      *name = TGSI_SEMANTIC_POSITION;
      *index = 0;
      break;
   case VERT_ATTRIB_POINT_SIZE:
      *name = TGSI_SEMANTIC_PSIZE;
      *index = 0;
      break;
   default:
      ERROR("unknown vert attrib slot %u\n", slot);
      assert(false);
      break;
   }
}

static void
varying_slot_to_tgsi_semantic(gl_varying_slot slot, unsigned *name, unsigned *index)
{
   assert(name && index);

   if (slot >= VARYING_SLOT_TESS_MAX) {
      ERROR("invalid varying slot %u\n", slot);
      assert(false);
      return;
   }

   if (slot >= VARYING_SLOT_PATCH0) {
      *name = TGSI_SEMANTIC_PATCH;
      *index = slot - VARYING_SLOT_PATCH0;
      return;
   }

   if (slot >= VARYING_SLOT_VAR0) {
      *name = TGSI_SEMANTIC_GENERIC;
      *index = slot - VARYING_SLOT_VAR0;
      return;
   }

   if (slot >= VARYING_SLOT_TEX0 && slot <= VARYING_SLOT_TEX7) {
      *name = TGSI_SEMANTIC_TEXCOORD;
      *index = slot - VARYING_SLOT_TEX0;
      return;
   }

   switch (slot) {
   case VARYING_SLOT_BFC0:
      *name = TGSI_SEMANTIC_BCOLOR;
      *index = 0;
      break;
   case VARYING_SLOT_BFC1:
      *name = TGSI_SEMANTIC_BCOLOR;
      *index = 1;
      break;
   case VARYING_SLOT_CLIP_DIST0:
      *name = TGSI_SEMANTIC_CLIPDIST;
      *index = 0;
      break;
   case VARYING_SLOT_CLIP_DIST1:
      *name = TGSI_SEMANTIC_CLIPDIST;
      *index = 1;
      break;
   case VARYING_SLOT_CLIP_VERTEX:
      *name = TGSI_SEMANTIC_CLIPVERTEX;
      *index = 0;
      break;
   case VARYING_SLOT_COL0:
      *name = TGSI_SEMANTIC_COLOR;
      *index = 0;
      break;
   case VARYING_SLOT_COL1:
      *name = TGSI_SEMANTIC_COLOR;
      *index = 1;
      break;
   case VARYING_SLOT_EDGE:
      *name = TGSI_SEMANTIC_EDGEFLAG;
      *index = 0;
      break;
   case VARYING_SLOT_FACE:
      *name = TGSI_SEMANTIC_FACE;
      *index = 0;
      break;
   case VARYING_SLOT_FOGC:
      *name = TGSI_SEMANTIC_FOG;
      *index = 0;
      break;
   case VARYING_SLOT_LAYER:
      *name = TGSI_SEMANTIC_LAYER;
      *index = 0;
      break;
   case VARYING_SLOT_PNTC:
      *name = TGSI_SEMANTIC_PCOORD;
      *index = 0;
      break;
   case VARYING_SLOT_POS:
      *name = TGSI_SEMANTIC_POSITION;
      *index = 0;
      break;
   case VARYING_SLOT_PRIMITIVE_ID:
      *name = TGSI_SEMANTIC_PRIMID;
      *index = 0;
      break;
   case VARYING_SLOT_PSIZ:
      *name = TGSI_SEMANTIC_PSIZE;
      *index = 0;
      break;
   case VARYING_SLOT_TESS_LEVEL_INNER:
      *name = TGSI_SEMANTIC_TESSINNER;
      *index = 0;
      break;
   case VARYING_SLOT_TESS_LEVEL_OUTER:
      *name = TGSI_SEMANTIC_TESSOUTER;
      *index = 0;
      break;
   case VARYING_SLOT_VIEWPORT:
      *name = TGSI_SEMANTIC_VIEWPORT_INDEX;
      *index = 0;
      break;
   default:
      ERROR("unknown varying slot %u\n", slot);
      assert(false);
      break;
   }
}

static void
frag_result_to_tgsi_semantic(unsigned slot, unsigned *name, unsigned *index)
{
   if (slot >= FRAG_RESULT_DATA0) {
      *name = TGSI_SEMANTIC_COLOR;
      *index = slot - FRAG_RESULT_COLOR - 2; // intentional
      return;
   }

   switch (slot) {
   case FRAG_RESULT_COLOR:
      *name = TGSI_SEMANTIC_COLOR;
      *index = 0;
      break;
   case FRAG_RESULT_DEPTH:
      *name = TGSI_SEMANTIC_POSITION;
      *index = 0;
      break;
   case FRAG_RESULT_SAMPLE_MASK:
      *name = TGSI_SEMANTIC_SAMPLEMASK;
      *index = 0;
      break;
   default:
      ERROR("unknown frag result slot %u\n", slot);
      assert(false);
      break;
   }
}

// copy of _mesa_sysval_to_semantic
static void
system_val_to_tgsi_semantic(unsigned val, unsigned *name, unsigned *index)
{
   *index = 0;
   switch (val) {
   // Vertex shader
   case SYSTEM_VALUE_VERTEX_ID:
      *name = TGSI_SEMANTIC_VERTEXID;
      break;
   case SYSTEM_VALUE_INSTANCE_ID:
      *name = TGSI_SEMANTIC_INSTANCEID;
      break;
   case SYSTEM_VALUE_VERTEX_ID_ZERO_BASE:
      *name = TGSI_SEMANTIC_VERTEXID_NOBASE;
      break;
   case SYSTEM_VALUE_BASE_VERTEX:
      *name = TGSI_SEMANTIC_BASEVERTEX;
      break;
   case SYSTEM_VALUE_BASE_INSTANCE:
      *name = TGSI_SEMANTIC_BASEINSTANCE;
      break;
   case SYSTEM_VALUE_DRAW_ID:
      *name = TGSI_SEMANTIC_DRAWID;
      break;

   // Geometry shader
   case SYSTEM_VALUE_INVOCATION_ID:
      *name = TGSI_SEMANTIC_INVOCATIONID;
      break;

   // Fragment shader
   case SYSTEM_VALUE_FRAG_COORD:
      *name = TGSI_SEMANTIC_POSITION;
      break;
   case SYSTEM_VALUE_FRONT_FACE:
      *name = TGSI_SEMANTIC_FACE;
      break;
   case SYSTEM_VALUE_SAMPLE_ID:
      *name = TGSI_SEMANTIC_SAMPLEID;
      break;
   case SYSTEM_VALUE_SAMPLE_POS:
      *name = TGSI_SEMANTIC_SAMPLEPOS;
      break;
   case SYSTEM_VALUE_SAMPLE_MASK_IN:
      *name = TGSI_SEMANTIC_SAMPLEMASK;
      break;
   case SYSTEM_VALUE_HELPER_INVOCATION:
      *name = TGSI_SEMANTIC_HELPER_INVOCATION;
      break;

   // Tessellation shader
   case SYSTEM_VALUE_TESS_COORD:
      *name = TGSI_SEMANTIC_TESSCOORD;
      break;
   case SYSTEM_VALUE_VERTICES_IN:
      *name = TGSI_SEMANTIC_VERTICESIN;
      break;
   case SYSTEM_VALUE_PRIMITIVE_ID:
      *name = TGSI_SEMANTIC_PRIMID;
      break;
   case SYSTEM_VALUE_TESS_LEVEL_OUTER:
      *name = TGSI_SEMANTIC_TESSOUTER;
      break;
   case SYSTEM_VALUE_TESS_LEVEL_INNER:
      *name = TGSI_SEMANTIC_TESSINNER;
      break;

   // Compute shader
   case SYSTEM_VALUE_LOCAL_INVOCATION_ID:
      *name = TGSI_SEMANTIC_THREAD_ID;
      break;
   case SYSTEM_VALUE_WORK_GROUP_ID:
      *name = TGSI_SEMANTIC_BLOCK_ID;
      break;
   case SYSTEM_VALUE_NUM_WORK_GROUPS:
      *name = TGSI_SEMANTIC_GRID_SIZE;
      break;
   case SYSTEM_VALUE_LOCAL_GROUP_SIZE:
      *name = TGSI_SEMANTIC_BLOCK_SIZE;
      break;

   // ARB_shader_ballot
   case SYSTEM_VALUE_SUBGROUP_SIZE:
      *name = TGSI_SEMANTIC_SUBGROUP_SIZE;
      break;
   case SYSTEM_VALUE_SUBGROUP_INVOCATION:
      *name = TGSI_SEMANTIC_SUBGROUP_INVOCATION;
      break;
   case SYSTEM_VALUE_SUBGROUP_EQ_MASK:
      *name = TGSI_SEMANTIC_SUBGROUP_EQ_MASK;
      break;
   case SYSTEM_VALUE_SUBGROUP_GE_MASK:
      *name = TGSI_SEMANTIC_SUBGROUP_GE_MASK;
      break;
   case SYSTEM_VALUE_SUBGROUP_GT_MASK:
      *name = TGSI_SEMANTIC_SUBGROUP_GT_MASK;
      break;
   case SYSTEM_VALUE_SUBGROUP_LE_MASK:
      *name = TGSI_SEMANTIC_SUBGROUP_LE_MASK;
      break;
   case SYSTEM_VALUE_SUBGROUP_LT_MASK:
      *name = TGSI_SEMANTIC_SUBGROUP_LT_MASK;
      break;

   default:
      ERROR("unknown system value %u\n", val);
      assert(false);
      break;
   }
}

void
Converter::setInterpolate(nv50_ir_varying *var,
                          uint8_t mode,
                          bool centroid,
                          unsigned semantic)
{
   switch (mode) {
   case INTERP_MODE_FLAT:
      var->flat = 1;
      break;
   case INTERP_MODE_NONE:
      if (semantic == TGSI_SEMANTIC_COLOR)
         var->sc = 1;
      else if (semantic == TGSI_SEMANTIC_POSITION)
         var->linear = 1;
      break;
   case INTERP_MODE_NOPERSPECTIVE:
      var->linear = 1;
      break;
   case INTERP_MODE_SMOOTH:
      break;
   }
   var->centroid = centroid;
}

static uint16_t
calcSlots(const glsl_type *type, Program::Type stage, const shader_info &info,
          bool input, const nir_variable *var)
{
   if (!type->is_array())
      return type->count_attribute_slots(false);

   uint16_t slots;
   switch (stage) {
   case Program::TYPE_GEOMETRY:
      slots = type->uniform_locations();
      if (input)
         slots /= info.gs.vertices_in;
      break;
   case Program::TYPE_TESSELLATION_CONTROL:
   case Program::TYPE_TESSELLATION_EVAL:
      // remove first dimension
      if (var->data.patch || (!input && stage == Program::TYPE_TESSELLATION_EVAL))
         slots = type->uniform_locations();
      else
         slots = type->fields.array->uniform_locations();
      break;
   default:
      slots = type->count_attribute_slots(false);
      break;
   }

   return slots;
}

bool Converter::assignSlots() {
   unsigned name;
   unsigned index;

   info->io.viewportId = -1;
   info->numInputs = 0;

   // we have to fixup the uniform locations for arrays
   unsigned numImages = 0;
   nir_foreach_variable(var, &nir->uniforms) {
      const glsl_type *type = var->type;
      if (!type->without_array()->is_image())
         continue;
      var->data.driver_location = numImages;
      numImages += type->is_array() ? type->arrays_of_arrays_size() : 1;
   }

   nir_foreach_variable(var, &nir->inputs) {
      const glsl_type *type = var->type;
      int slot = var->data.location;
      uint16_t slots = calcSlots(type, prog->getType(), nir->info, true, var);
      uint32_t comp = type->is_array() ? type->without_array()->component_slots()
                                       : type->component_slots();
      uint32_t frac = var->data.location_frac;
      uint32_t vary = var->data.driver_location;

      if (glsl_base_type_is_64bit(type->without_array()->base_type)) {
         if (comp > 2)
            slots *= 2;
      }

      assert(vary + slots <= PIPE_MAX_SHADER_INPUTS);

      switch(prog->getType()) {
      case Program::TYPE_FRAGMENT:
         varying_slot_to_tgsi_semantic((gl_varying_slot)slot, &name, &index);
         for (uint16_t i = 0; i < slots; ++i) {
            setInterpolate(&info->in[vary + i], var->data.interpolation,
                           var->data.centroid | var->data.sample, name);
         }
         break;
      case Program::TYPE_GEOMETRY:
         varying_slot_to_tgsi_semantic((gl_varying_slot)slot, &name, &index);
         break;
      case Program::TYPE_TESSELLATION_CONTROL:
      case Program::TYPE_TESSELLATION_EVAL:
         varying_slot_to_tgsi_semantic((gl_varying_slot)slot, &name, &index);
         if (var->data.patch && name == TGSI_SEMANTIC_PATCH)
            info->numPatchConstants = MAX2(info->numPatchConstants, index + slots);
         break;
      case Program::TYPE_VERTEX:
         vert_attrib_to_tgsi_semantic((gl_vert_attrib)slot, &name, &index);
         switch (name) {
         case TGSI_SEMANTIC_EDGEFLAG:
            info->io.edgeFlagIn = vary;
            break;
         default:
            break;
         }
         break;
      default:
         ERROR("unknown shader type %u in assignSlots\n", prog->getType());
         return false;
      }

      for (uint16_t i = 0u; i < slots; ++i, ++vary) {
         info->in[vary].id = vary;
         info->in[vary].patch = var->data.patch;
         info->in[vary].sn = name;
         info->in[vary].si = index + i;
         if (glsl_base_type_is_64bit(type->without_array()->base_type))
            if (i & 0x1)
               info->in[vary].mask |= (((1 << (comp * 2)) - 1) << (frac * 2) >> 0x4);
            else
               info->in[vary].mask |= (((1 << (comp * 2)) - 1) << (frac * 2) & 0xf);
         else
            info->in[vary].mask |= ((1 << comp) - 1) << frac;
      }
      info->numInputs = std::max<uint8_t>(info->numInputs, vary);
   }

   info->numOutputs = 0;
   nir_foreach_variable(var, &nir->outputs) {
      const glsl_type *type = var->type;
      int slot = var->data.location;
      uint16_t slots = calcSlots(type, prog->getType(), nir->info, false, var);
      uint32_t comp = type->is_array() ? type->without_array()->component_slots()
                                       : type->component_slots();
      uint32_t frac = var->data.location_frac;
      uint32_t vary = var->data.driver_location;

      if (glsl_base_type_is_64bit(type->without_array()->base_type)) {
         if (comp > 2)
            slots *= 2;
      }

      assert(vary < PIPE_MAX_SHADER_OUTPUTS);

      switch(prog->getType()) {
      case Program::TYPE_FRAGMENT:
         frag_result_to_tgsi_semantic((gl_frag_result)slot, &name, &index);
         switch (name) {
         case TGSI_SEMANTIC_COLOR:
            if (!var->data.fb_fetch_output)
               info->prop.fp.numColourResults++;
            info->prop.fp.separateFragData = true;
            // sometimes we get FRAG_RESULT_DATAX with data.index 0
            // sometimes we get FRAG_RESULT_DATA0 with data.index X
            index = index == 0 ? var->data.index : index;
            break;
         case TGSI_SEMANTIC_POSITION:
            info->io.fragDepth = vary;
            info->prop.fp.writesDepth = true;
            break;
         case TGSI_SEMANTIC_SAMPLEMASK:
            info->io.sampleMask = vary;
            break;
         default:
            break;
         }
         break;
      case Program::TYPE_GEOMETRY:
      case Program::TYPE_TESSELLATION_CONTROL:
      case Program::TYPE_TESSELLATION_EVAL:
      case Program::TYPE_VERTEX:
         varying_slot_to_tgsi_semantic((gl_varying_slot)slot, &name, &index);

         if (var->data.patch && name != TGSI_SEMANTIC_TESSINNER &&
             name != TGSI_SEMANTIC_TESSOUTER)
            info->numPatchConstants = MAX2(info->numPatchConstants, index + slots);

         switch (name) {
         case TGSI_SEMANTIC_CLIPDIST:
            info->io.genUserClip = -1;
            break;
         case TGSI_SEMANTIC_EDGEFLAG:
            info->io.edgeFlagOut = vary;
            break;
         default:
            break;
         }
         break;
      default:
         ERROR("unknown shader type %u in assignSlots\n", prog->getType());
         return false;
      }

      for (uint16_t i = 0u; i < slots; ++i, ++vary) {
         info->out[vary].id = vary;
         info->out[vary].patch = var->data.patch;
         info->out[vary].sn = name;
         info->out[vary].si = index + i;
         if (glsl_base_type_is_64bit(type->without_array()->base_type))
            if (i & 0x1)
               info->out[vary].mask |= (((1 << (comp * 2)) - 1) << (frac * 2) >> 0x4);
            else
               info->out[vary].mask |= (((1 << (comp * 2)) - 1) << (frac * 2) & 0xf);
         else
            info->out[vary].mask |= ((1 << comp) - 1) << frac;

         if (nir->info.outputs_read & 1ll << slot)
            info->out[vary].oread = 1;
      }
      info->numOutputs = std::max<uint8_t>(info->numOutputs, vary);
   }

   info->numSysVals = 0;
   for (uint8_t i = 0; i < 64; ++i) {
      if (!(nir->info.system_values_read & 1ll << i))
         continue;

      system_val_to_tgsi_semantic(i, &name, &index);
      info->sv[info->numSysVals].sn = name;
      info->sv[info->numSysVals].si = index;
      info->sv[info->numSysVals].input = 0; // TODO inferSysValDirection(sn);

      switch (i) {
      case SYSTEM_VALUE_INSTANCE_ID:
         info->io.instanceId = info->numSysVals;
         break;
      case SYSTEM_VALUE_TESS_LEVEL_INNER:
      case SYSTEM_VALUE_TESS_LEVEL_OUTER:
         info->sv[info->numSysVals].patch = 1;
         break;
      case SYSTEM_VALUE_VERTEX_ID:
         info->io.vertexId = info->numSysVals;
         break;
      default:
         break;
      }

      info->numSysVals += 1;
   }

   if (info->io.genUserClip > 0) {
      info->io.clipDistances = info->io.genUserClip;

      const unsigned int nOut = (info->io.genUserClip + 3) / 4;

      for (unsigned int n = 0; n < nOut; ++n) {
         unsigned int i = info->numOutputs++;
         info->out[i].id = i;
         info->out[i].sn = TGSI_SEMANTIC_CLIPDIST;
         info->out[i].si = n;
         info->out[i].mask = ((1 << info->io.clipDistances) - 1) >> (n * 4);
      }
   }

   return info->assignSlots(info) == 0;
}

uint32_t
Converter::getSlotAddress(nir_intrinsic_instr *insn, uint8_t idx, uint8_t slot)
{
   DataType ty;
   int offset = nir_intrinsic_component(insn);
   bool input;

   if (nir_intrinsic_infos[insn->intrinsic].has_dest)
      ty = getDType(insn);
   else
      ty = getSType(insn->src[0], false, false);

   switch (insn->intrinsic) {
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_per_vertex_input:
      input = true;
      break;
   case nir_intrinsic_load_output:
   case nir_intrinsic_load_per_vertex_output:
   case nir_intrinsic_store_output:
   case nir_intrinsic_store_per_vertex_output:
      input = false;
      break;
   default:
      ERROR("unknown intrinsic in getSlotAddress %s",
            nir_intrinsic_infos[insn->intrinsic].name);
      input = false;
      assert(false);
      break;
   }

   if (typeSizeof(ty) == 8) {
      slot *= 2;
      slot += offset;
      if (slot >= 4) {
         idx += 1;
         slot -= 4;
      }
   } else {
      slot += offset;
   }

   assert(slot < 4);
   assert(!input || idx < PIPE_MAX_SHADER_INPUTS);
   assert(input || idx < PIPE_MAX_SHADER_OUTPUTS);

   const nv50_ir_varying *vary = input ? info->in : info->out;
   return vary[idx].slot[slot] * 4;
}

bool
Converter::run()
{
   bool progress;

   if (prog->dbgFlags & NV50_IR_DEBUG_VERBOSE)
      nir_print_shader(nir, stderr);

   NIR_PASS_V(nir, nir_lower_io, nir_var_all, type_size, (nir_lower_io_options)0);
   NIR_PASS_V(nir, nir_lower_regs_to_ssa);
   NIR_PASS_V(nir, nir_lower_load_const_to_scalar);
   NIR_PASS_V(nir, nir_lower_vars_to_ssa);
   NIR_PASS_V(nir, nir_lower_alu_to_scalar);
   NIR_PASS_V(nir, nir_lower_phis_to_scalar);

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_trivial_continues);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
   } while (progress);

   NIR_PASS_V(nir, nir_lower_bool_to_int32);
   NIR_PASS_V(nir, nir_lower_locals_to_regs);
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_function_temp);
   NIR_PASS_V(nir, nir_convert_from_ssa, true);

   // Garbage collect dead instructions
   nir_sweep(nir);

   if (!assignSlots()) {
      ERROR("Couldn't assign slots!\n");
      return false;
   }

   if (prog->dbgFlags & NV50_IR_DEBUG_BASIC)
      nir_print_shader(nir, stderr);

   return false;
}

} // unnamed namespace

namespace nv50_ir {

bool
Program::makeFromNIR(struct nv50_ir_prog_info *info)
{
   nir_shader *nir = (nir_shader*)info->bin.source;
   Converter converter(this, nir, info);
   bool result = converter.run();
   if (!result)
      return result;
   LoweringHelper lowering;
   lowering.run(this);
   tlsSize = info->bin.tlsSpace;
   return result;
}

} // namespace nv50_ir

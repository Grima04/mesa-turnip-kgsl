/*
 * Copyright © 2018 Red Hat
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
 *
 * Authors:
 *    Rob Clark (robdclark@gmail.com)
 */

#include "math.h"
#include "nir/nir_builtin_builder.h"

#include "vtn_private.h"
#include "OpenCL.std.h"

typedef nir_ssa_def *(*nir_handler)(struct vtn_builder *b,
                                    enum OpenCLstd_Entrypoints opcode,
                                    unsigned num_srcs, nir_ssa_def **srcs,
                                    struct vtn_type **src_types,
                                    const struct vtn_type *dest_type);

static int to_llvm_address_space(SpvStorageClass mode)
{
   switch (mode) {
   case SpvStorageClassPrivate:
   case SpvStorageClassFunction: return 0;
   case SpvStorageClassCrossWorkgroup: return 1;
   case SpvStorageClassUniform:
   case SpvStorageClassUniformConstant: return 2;
   case SpvStorageClassWorkgroup: return 3;
   default: return -1;
   }
}


static void
vtn_opencl_mangle(const char *in_name,
                  uint32_t const_mask,
                  int ntypes, struct vtn_type **src_types,
                  char **outstring)
{
   char local_name[256] = "";
   char *args_str = local_name + sprintf(local_name, "_Z%zu%s", strlen(in_name), in_name);

   for (unsigned i = 0; i < ntypes; ++i) {
      const struct glsl_type *type = src_types[i]->type;
      enum vtn_base_type base_type = src_types[i]->base_type;
      if (src_types[i]->base_type == vtn_base_type_pointer) {
         *(args_str++) = 'P';
         int address_space = to_llvm_address_space(src_types[i]->storage_class);
         if (address_space > 0)
            args_str += sprintf(args_str, "U3AS%d", address_space);

         type = src_types[i]->deref->type;
         base_type = src_types[i]->deref->base_type;
      }

      if (const_mask & (1 << i))
         *(args_str++) = 'K';

      unsigned num_elements = glsl_get_components(type);
      if (num_elements > 1) {
         /* Vectors are not treated as built-ins for mangling, so check for substitution.
          * In theory, we'd need to know which substitution value this is. In practice,
          * the functions we need from libclc only support 1
          */
         bool substitution = false;
         for (unsigned j = 0; j < i; ++j) {
            const struct glsl_type *other_type = src_types[j]->base_type == vtn_base_type_pointer ?
               src_types[j]->deref->type : src_types[j]->type;
            if (type == other_type) {
               substitution = true;
               break;
            }
         }

         if (substitution) {
            args_str += sprintf(args_str, "S_");
            continue;
         } else
            args_str += sprintf(args_str, "Dv%d_", num_elements);
      }

      const char *suffix = NULL;
      switch (base_type) {
      case vtn_base_type_sampler: suffix = "11ocl_sampler"; break;
      case vtn_base_type_event: suffix = "9ocl_event"; break;
      default: {
         const char *primitives[] = {
            [GLSL_TYPE_UINT] = "j",
            [GLSL_TYPE_INT] = "i",
            [GLSL_TYPE_FLOAT] = "f",
            [GLSL_TYPE_FLOAT16] = "Dh",
            [GLSL_TYPE_DOUBLE] = "d",
            [GLSL_TYPE_UINT8] = "h",
            [GLSL_TYPE_INT8] = "c",
            [GLSL_TYPE_UINT16] = "t",
            [GLSL_TYPE_INT16] = "s",
            [GLSL_TYPE_UINT64] = "m",
            [GLSL_TYPE_INT64] = "l",
            [GLSL_TYPE_BOOL] = "b",
            [GLSL_TYPE_ERROR] = NULL,
         };
         enum glsl_base_type glsl_base_type = glsl_get_base_type(type);
         assert(glsl_base_type < ARRAY_SIZE(primitives) && primitives[glsl_base_type]);
         suffix = primitives[glsl_base_type];
         break;
      }
      }
      args_str += sprintf(args_str, "%s", suffix);
   }

   *outstring = strdup(local_name);
}

static nir_function *mangle_and_find(struct vtn_builder *b,
                                     const char *name,
                                     uint32_t const_mask,
                                     uint32_t num_srcs,
                                     struct vtn_type **src_types)
{
   char *mname;
   nir_function *found = NULL;

   vtn_opencl_mangle(name, const_mask, num_srcs, src_types, &mname);
   /* try and find in current shader first. */
   nir_foreach_function(funcs, b->shader) {
      if (!strcmp(funcs->name, mname)) {
         found = funcs;
         break;
      }
   }
   /* if not found here find in clc shader and create a decl mirroring it */
   if (!found && b->options->clc_shader && b->options->clc_shader != b->shader) {
      nir_foreach_function(funcs, b->options->clc_shader) {
         if (!strcmp(funcs->name, mname)) {
            found = funcs;
            break;
         }
      }
      if (found) {
         nir_function *decl = nir_function_create(b->shader, mname);
         decl->num_params = found->num_params;
         decl->params = ralloc_array(b->shader, nir_parameter, decl->num_params);
         for (unsigned i = 0; i < decl->num_params; i++) {
            decl->params[i] = found->params[i];
         }
         found = decl;
      }
   }
   if (!found)
      vtn_fail("Can't find clc function %s\n", mname);
   free(mname);
   return found;
}

static bool call_mangled_function(struct vtn_builder *b,
                                  const char *name,
                                  uint32_t const_mask,
                                  uint32_t num_srcs,
                                  struct vtn_type **src_types,
                                  const struct vtn_type *dest_type,
                                  nir_ssa_def **srcs,
                                  nir_deref_instr **ret_deref_ptr)
{
   nir_function *found = mangle_and_find(b, name, const_mask, num_srcs, src_types);
   if (!found)
      return false;

   nir_call_instr *call = nir_call_instr_create(b->shader, found);

   nir_deref_instr *ret_deref = NULL;
   uint32_t param_idx = 0;
   if (dest_type) {
      nir_variable *ret_tmp = nir_local_variable_create(b->nb.impl,
                                                        glsl_get_bare_type(dest_type->type),
                                                        "return_tmp");
      ret_deref = nir_build_deref_var(&b->nb, ret_tmp);
      call->params[param_idx++] = nir_src_for_ssa(&ret_deref->dest.ssa);
   }

   for (unsigned i = 0; i < num_srcs; i++)
      call->params[param_idx++] = nir_src_for_ssa(srcs[i]);
   nir_builder_instr_insert(&b->nb, &call->instr);

   *ret_deref_ptr = ret_deref;
   return true;
}

static void
handle_instr(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode,
             const uint32_t *w, unsigned count, nir_handler handler)
{
   struct vtn_type *dest_type = vtn_get_type(b, w[1]);

   unsigned num_srcs = count - 5;
   nir_ssa_def *srcs[3] = { NULL };
   struct vtn_type *src_types[3] = { NULL };
   vtn_assert(num_srcs <= ARRAY_SIZE(srcs));
   for (unsigned i = 0; i < num_srcs; i++) {
      struct vtn_value *val = vtn_untyped_value(b, w[i + 5]);
      struct vtn_ssa_value *ssa = vtn_ssa_value(b, w[i + 5]);
      srcs[i] = ssa->def;
      src_types[i] = val->type;
   }

   nir_ssa_def *result = handler(b, opcode, num_srcs, srcs, src_types, dest_type);
   if (result) {
      vtn_push_nir_ssa(b, w[2], result);
   } else {
      vtn_assert(dest_type->type == glsl_void_type());
   }
}

static nir_op
nir_alu_op_for_opencl_opcode(struct vtn_builder *b,
                             enum OpenCLstd_Entrypoints opcode)
{
   switch (opcode) {
   case OpenCLstd_Fabs: return nir_op_fabs;
   case OpenCLstd_SAbs: return nir_op_iabs;
   case OpenCLstd_SAdd_sat: return nir_op_iadd_sat;
   case OpenCLstd_UAdd_sat: return nir_op_uadd_sat;
   case OpenCLstd_Ceil: return nir_op_fceil;
   case OpenCLstd_Cos: return nir_op_fcos;
   case OpenCLstd_Exp2: return nir_op_fexp2;
   case OpenCLstd_Log2: return nir_op_flog2;
   case OpenCLstd_Floor: return nir_op_ffloor;
   case OpenCLstd_SHadd: return nir_op_ihadd;
   case OpenCLstd_UHadd: return nir_op_uhadd;
   case OpenCLstd_Fma: return nir_op_ffma;
   case OpenCLstd_Fmax: return nir_op_fmax;
   case OpenCLstd_SMax: return nir_op_imax;
   case OpenCLstd_UMax: return nir_op_umax;
   case OpenCLstd_Fmin: return nir_op_fmin;
   case OpenCLstd_SMin: return nir_op_imin;
   case OpenCLstd_UMin: return nir_op_umin;
   case OpenCLstd_Fmod: return nir_op_fmod;
   case OpenCLstd_Mix: return nir_op_flrp;
   case OpenCLstd_Native_cos: return nir_op_fcos;
   case OpenCLstd_Native_divide: return nir_op_fdiv;
   case OpenCLstd_Native_exp2: return nir_op_fexp2;
   case OpenCLstd_Native_log2: return nir_op_flog2;
   case OpenCLstd_Native_powr: return nir_op_fpow;
   case OpenCLstd_Native_recip: return nir_op_frcp;
   case OpenCLstd_Native_rsqrt: return nir_op_frsq;
   case OpenCLstd_Native_sin: return nir_op_fsin;
   case OpenCLstd_Native_sqrt: return nir_op_fsqrt;
   case OpenCLstd_SMul_hi: return nir_op_imul_high;
   case OpenCLstd_UMul_hi: return nir_op_umul_high;
   case OpenCLstd_Popcount: return nir_op_bit_count;
   case OpenCLstd_Pow: return nir_op_fpow;
   case OpenCLstd_Remainder: return nir_op_frem;
   case OpenCLstd_SRhadd: return nir_op_irhadd;
   case OpenCLstd_URhadd: return nir_op_urhadd;
   case OpenCLstd_Rsqrt: return nir_op_frsq;
   case OpenCLstd_Sign: return nir_op_fsign;
   case OpenCLstd_Sin: return nir_op_fsin;
   case OpenCLstd_Sqrt: return nir_op_fsqrt;
   case OpenCLstd_SSub_sat: return nir_op_isub_sat;
   case OpenCLstd_USub_sat: return nir_op_usub_sat;
   case OpenCLstd_Trunc: return nir_op_ftrunc;
   case OpenCLstd_Rint: return nir_op_fround_even;
   /* uhm... */
   case OpenCLstd_UAbs: return nir_op_mov;
   default:
      vtn_fail("No NIR equivalent");
   }
}

static nir_ssa_def *
handle_alu(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode,
           unsigned num_srcs, nir_ssa_def **srcs, struct vtn_type **src_types,
           const struct vtn_type *dest_type)
{
   nir_ssa_def *ret = nir_build_alu(&b->nb, nir_alu_op_for_opencl_opcode(b, opcode),
                                    srcs[0], srcs[1], srcs[2], NULL);
   if (opcode == OpenCLstd_Popcount)
      ret = nir_u2u(&b->nb, ret, glsl_get_bit_size(dest_type->type));
   return ret;
}

#define REMAP(op, str) [OpenCLstd_##op] = { str }
static const struct {
   const char *fn;
} remap_table[] = {
   REMAP(Distance, "distance"),
   REMAP(Fast_distance, "fast_distance"),
   REMAP(Fast_length, "fast_length"),
   REMAP(Fast_normalize, "fast_normalize"),
   REMAP(Half_rsqrt, "half_rsqrt"),
   REMAP(Half_sqrt, "half_sqrt"),
   REMAP(Length, "length"),
   REMAP(Normalize, "normalize"),
   REMAP(Degrees, "degrees"),
   REMAP(Radians, "radians"),
   REMAP(Rotate, "rotate"),
   REMAP(Smoothstep, "smoothstep"),
   REMAP(Step, "step"),

   REMAP(Pow, "pow"),
   REMAP(Pown, "pown"),
   REMAP(Powr, "powr"),
   REMAP(Rootn, "rootn"),
   REMAP(Modf, "modf"),

   REMAP(Acos, "acos"),
   REMAP(Acosh, "acosh"),
   REMAP(Acospi, "acospi"),
   REMAP(Asin, "asin"),
   REMAP(Asinh, "asinh"),
   REMAP(Asinpi, "asinpi"),
   REMAP(Atan, "atan"),
   REMAP(Atan2, "atan2"),
   REMAP(Atanh, "atanh"),
   REMAP(Atanpi, "atanpi"),
   REMAP(Atan2pi, "atan2pi"),
   REMAP(Cos, "cos"),
   REMAP(Cosh, "cosh"),
   REMAP(Cospi, "cospi"),
   REMAP(Sin, "sin"),
   REMAP(Sinh, "sinh"),
   REMAP(Sinpi, "sinpi"),
   REMAP(Tan, "tan"),
   REMAP(Tanh, "tanh"),
   REMAP(Tanpi, "tanpi"),
   REMAP(Sincos, "sincos"),
   REMAP(Fract, "fract"),
   REMAP(Frexp, "frexp"),
   REMAP(Fma, "fma"),
   REMAP(Fmod, "fmod"),

   REMAP(Half_cos, "cos"),
   REMAP(Half_exp, "exp"),
   REMAP(Half_exp2, "exp2"),
   REMAP(Half_exp10, "exp10"),
   REMAP(Half_log, "log"),
   REMAP(Half_log2, "log2"),
   REMAP(Half_log10, "log10"),
   REMAP(Half_powr, "powr"),
   REMAP(Half_sin, "sin"),
   REMAP(Half_tan, "tan"),

   REMAP(Remainder, "remainder"),
   REMAP(Remquo, "remquo"),
   REMAP(Hypot, "hypot"),
   REMAP(Exp, "exp"),
   REMAP(Exp2, "exp2"),
   REMAP(Exp10, "exp10"),
   REMAP(Expm1, "expm1"),
   REMAP(Ldexp, "ldexp"),

   REMAP(Ilogb, "ilogb"),
   REMAP(Log, "log"),
   REMAP(Log2, "log2"),
   REMAP(Log10, "log10"),
   REMAP(Log1p, "log1p"),
   REMAP(Logb, "logb"),

   REMAP(Cbrt, "cbrt"),
   REMAP(Erfc, "erfc"),
   REMAP(Erf, "erf"),

   REMAP(Lgamma, "lgamma"),
   REMAP(Lgamma_r, "lgamma_r"),
   REMAP(Tgamma, "tgamma"),

   REMAP(UMad_sat, "mad_sat"),
   REMAP(SMad_sat, "mad_sat"),

   REMAP(Shuffle, "shuffle"),
   REMAP(Shuffle2, "shuffle2"),
};
#undef REMAP

static const char *remap_clc_opcode(enum OpenCLstd_Entrypoints opcode)
{
   if (opcode >= (sizeof(remap_table) / sizeof(const char *)))
      return NULL;
   return remap_table[opcode].fn;
}

static struct vtn_type *
get_vtn_type_for_glsl_type(struct vtn_builder *b, const struct glsl_type *type)
{
   struct vtn_type *ret = rzalloc(b, struct vtn_type);
   assert(glsl_type_is_vector_or_scalar(type));
   ret->type = type;
   ret->length = glsl_get_vector_elements(type);
   ret->base_type = glsl_type_is_vector(type) ? vtn_base_type_vector : vtn_base_type_scalar;
   return ret;
}

static struct vtn_type *
get_pointer_type(struct vtn_builder *b, struct vtn_type *t, SpvStorageClass storage_class)
{
   struct vtn_type *ret = rzalloc(b, struct vtn_type);
   ret->type = nir_address_format_to_glsl_type(
            vtn_mode_to_address_format(
               b, vtn_storage_class_to_mode(b, storage_class, NULL, NULL)));
   ret->base_type = vtn_base_type_pointer;
   ret->storage_class = storage_class;
   ret->deref = t;
   return ret;
}

static struct vtn_type *
get_signed_type(struct vtn_builder *b, struct vtn_type *t)
{
   if (t->base_type == vtn_base_type_pointer) {
      return get_pointer_type(b, get_signed_type(b, t->deref), t->storage_class);
   }
   return get_vtn_type_for_glsl_type(
      b, glsl_vector_type(glsl_signed_base_type_of(glsl_get_base_type(t->type)),
                          glsl_get_vector_elements(t->type)));
}

static nir_ssa_def *
handle_clc_fn(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode,
              int num_srcs,
              nir_ssa_def **srcs,
              struct vtn_type **src_types,
              const struct vtn_type *dest_type)
{
   const char *name = remap_clc_opcode(opcode);
   if (!name)
       return NULL;

   /* Some functions which take params end up with uint (or pointer-to-uint) being passed,
    * which doesn't mangle correctly when the function expects int or pointer-to-int.
    * See https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#_a_id_unsignedsigned_a_unsigned_versus_signed_integers
    */
   int signed_param = -1;
   switch (opcode) {
   case OpenCLstd_Frexp:
   case OpenCLstd_Lgamma_r:
   case OpenCLstd_Pown:
   case OpenCLstd_Rootn:
   case OpenCLstd_Ldexp:
      signed_param = 1;
      break;
   case OpenCLstd_Remquo:
      signed_param = 2;
      break;
   case OpenCLstd_SMad_sat: {
      /* All parameters need to be converted to signed */
      src_types[0] = src_types[1] = src_types[2] = get_signed_type(b, src_types[0]);
      break;
   }
   default: break;
   }

   if (signed_param >= 0) {
      src_types[signed_param] = get_signed_type(b, src_types[signed_param]);
   }

   nir_deref_instr *ret_deref = NULL;

   if (!call_mangled_function(b, name, 0, num_srcs, src_types,
                              dest_type, srcs, &ret_deref))
      return NULL;

   return ret_deref ? nir_load_deref(&b->nb, ret_deref) : NULL;
}

static nir_ssa_def *
handle_special(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode,
               unsigned num_srcs, nir_ssa_def **srcs, struct vtn_type **src_types,
               const struct vtn_type *dest_type)
{
   nir_builder *nb = &b->nb;

   switch (opcode) {
   case OpenCLstd_SAbs_diff:
     /* these works easier in direct NIR */
      return nir_iabs_diff(nb, srcs[0], srcs[1]);
   case OpenCLstd_UAbs_diff:
      return nir_uabs_diff(nb, srcs[0], srcs[1]);
   case OpenCLstd_Bitselect:
      return nir_bitselect(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_SMad_hi:
      return nir_imad_hi(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_UMad_hi:
      return nir_umad_hi(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_SMul24:
      return nir_imul24(nb, srcs[0], srcs[1]);
   case OpenCLstd_UMul24:
      return nir_umul24(nb, srcs[0], srcs[1]);
   case OpenCLstd_SMad24:
      return nir_imad24(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_UMad24:
      return nir_umad24(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_FClamp:
      return nir_fclamp(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_SClamp:
      return nir_iclamp(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_UClamp:
      return nir_uclamp(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_Copysign:
      return nir_copysign(nb, srcs[0], srcs[1]);
   case OpenCLstd_Cross:
      if (dest_type->length == 4)
         return nir_cross4(nb, srcs[0], srcs[1]);
      return nir_cross3(nb, srcs[0], srcs[1]);
   case OpenCLstd_Degrees:
      return nir_degrees(nb, srcs[0]);
   case OpenCLstd_Fdim:
      return nir_fdim(nb, srcs[0], srcs[1]);
   case OpenCLstd_Distance:
      return nir_distance(nb, srcs[0], srcs[1]);
   case OpenCLstd_Fast_distance:
      return nir_fast_distance(nb, srcs[0], srcs[1]);
   case OpenCLstd_Fast_length:
      return nir_fast_length(nb, srcs[0]);
   case OpenCLstd_Fast_normalize:
      return nir_fast_normalize(nb, srcs[0]);
   case OpenCLstd_Length:
      return nir_length(nb, srcs[0]);
   case OpenCLstd_Mad:
      return nir_fmad(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_Maxmag:
      return nir_maxmag(nb, srcs[0], srcs[1]);
   case OpenCLstd_Minmag:
      return nir_minmag(nb, srcs[0], srcs[1]);
   case OpenCLstd_Nan:
      return nir_nan(nb, srcs[0]);
   case OpenCLstd_Nextafter:
      return nir_nextafter(nb, srcs[0], srcs[1]);
   case OpenCLstd_Normalize:
      return nir_normalize(nb, srcs[0]);
   case OpenCLstd_Radians:
      return nir_radians(nb, srcs[0]);
   case OpenCLstd_Rotate:
      return nir_rotate(nb, srcs[0], srcs[1]);
   case OpenCLstd_Smoothstep:
      return nir_smoothstep(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_Clz:
      return nir_clz_u(nb, srcs[0]);
   case OpenCLstd_Select:
      return nir_select(nb, srcs[0], srcs[1], srcs[2]);
   case OpenCLstd_Step:
      return nir_sge(nb, srcs[1], srcs[0]);
   case OpenCLstd_S_Upsample:
   case OpenCLstd_U_Upsample:
      /* SPIR-V and CL have different defs for upsample, just implement in nir */
      return nir_upsample(nb, srcs[0], srcs[1]);
   case OpenCLstd_Native_exp:
      return nir_fexp(nb, srcs[0]);
   case OpenCLstd_Native_exp10:
      return nir_fexp2(nb, nir_fmul_imm(nb, srcs[0], log(10) / log(2)));
   case OpenCLstd_Native_log:
      return nir_flog(nb, srcs[0]);
   case OpenCLstd_Native_log10:
      return nir_fmul_imm(nb, nir_flog2(nb, srcs[0]), log(2) / log(10));
   case OpenCLstd_Native_tan:
      return nir_ftan(nb, srcs[0]);
   default:
      break;
   }

   nir_ssa_def *ret = handle_clc_fn(b, opcode, num_srcs, srcs, src_types, dest_type);
   if (!ret)
      vtn_fail("No NIR equivalent");

   return ret;
}

static void
_handle_v_load_store(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode,
                     const uint32_t *w, unsigned count, bool load)
{
   struct vtn_type *type;
   if (load)
      type = vtn_get_type(b, w[1]);
   else
      type = vtn_get_value_type(b, w[5]);
   unsigned a = load ? 0 : 1;

   const struct glsl_type *dest_type = type->type;
   unsigned components = glsl_get_vector_elements(dest_type);

   nir_ssa_def *offset = vtn_get_nir_ssa(b, w[5 + a]);
   struct vtn_value *p = vtn_value(b, w[6 + a], vtn_value_type_pointer);

   struct vtn_ssa_value *comps[NIR_MAX_VEC_COMPONENTS];
   nir_ssa_def *ncomps[NIR_MAX_VEC_COMPONENTS];

   nir_ssa_def *moffset = nir_imul_imm(&b->nb, offset, components);
   nir_deref_instr *deref = vtn_pointer_to_deref(b, p->pointer);

   for (int i = 0; i < components; i++) {
      nir_ssa_def *coffset = nir_iadd_imm(&b->nb, moffset, i);
      nir_deref_instr *arr_deref = nir_build_deref_ptr_as_array(&b->nb, deref, coffset);

      if (load) {
         comps[i] = vtn_local_load(b, arr_deref, p->type->access);
         ncomps[i] = comps[i]->def;
      } else {
         struct vtn_ssa_value *ssa = vtn_create_ssa_value(b, glsl_scalar_type(glsl_get_base_type(dest_type)));
         struct vtn_ssa_value *val = vtn_ssa_value(b, w[5]);
         ssa->def = nir_channel(&b->nb, val->def, i);
         vtn_local_store(b, ssa, arr_deref, p->type->access);
      }
   }
   if (load) {
      vtn_push_nir_ssa(b, w[2], nir_vec(&b->nb, ncomps, components));
   }
}

static void
vtn_handle_opencl_vload(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode,
                        const uint32_t *w, unsigned count)
{
   _handle_v_load_store(b, opcode, w, count, true);
}

static void
vtn_handle_opencl_vstore(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode,
                         const uint32_t *w, unsigned count)
{
   _handle_v_load_store(b, opcode, w, count, false);
}

static nir_ssa_def *
handle_printf(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode,
              unsigned num_srcs, nir_ssa_def **srcs, struct vtn_type **src_types,
              const struct vtn_type *dest_type)
{
   /* hahah, yeah, right.. */
   return nir_imm_int(&b->nb, -1);
}

static nir_ssa_def *
handle_round(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode, unsigned num_srcs,
             nir_ssa_def **srcs, struct vtn_type **src_types,
             const struct vtn_type *dest_type)
{
   nir_ssa_def *src = srcs[0];
   nir_builder *nb = &b->nb;
   nir_ssa_def *half = nir_imm_floatN_t(nb, 0.5, src->bit_size);
   nir_ssa_def *truncated = nir_ftrunc(nb, src);
   nir_ssa_def *remainder = nir_fsub(nb, src, truncated);

   return nir_bcsel(nb, nir_fge(nb, nir_fabs(nb, remainder), half),
                    nir_fadd(nb, truncated, nir_fsign(nb, src)), truncated);
}

static nir_ssa_def *
handle_shuffle(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode, unsigned num_srcs,
               nir_ssa_def **srcs, struct vtn_type **src_types,
               const struct vtn_type *dest_type)
{
   struct nir_ssa_def *input = srcs[0];
   struct nir_ssa_def *mask = srcs[1];

   unsigned out_elems = dest_type->length;
   nir_ssa_def *outres[NIR_MAX_VEC_COMPONENTS];
   unsigned in_elems = input->num_components;
   if (mask->bit_size != 32)
      mask = nir_u2u32(&b->nb, mask);
   mask = nir_iand(&b->nb, mask, nir_imm_intN_t(&b->nb, in_elems - 1, mask->bit_size));
   for (unsigned i = 0; i < out_elems; i++)
      outres[i] = nir_vector_extract(&b->nb, input, nir_channel(&b->nb, mask, i));

   return nir_vec(&b->nb, outres, out_elems);
}

static nir_ssa_def *
handle_shuffle2(struct vtn_builder *b, enum OpenCLstd_Entrypoints opcode, unsigned num_srcs,
                nir_ssa_def **srcs, struct vtn_type **src_types,
                const struct vtn_type *dest_type)
{
   struct nir_ssa_def *input0 = srcs[0];
   struct nir_ssa_def *input1 = srcs[1];
   struct nir_ssa_def *mask = srcs[2];

   unsigned out_elems = dest_type->length;
   nir_ssa_def *outres[NIR_MAX_VEC_COMPONENTS];
   unsigned in_elems = input0->num_components;
   unsigned total_mask = 2 * in_elems - 1;
   unsigned half_mask = in_elems - 1;
   if (mask->bit_size != 32)
      mask = nir_u2u32(&b->nb, mask);
   mask = nir_iand(&b->nb, mask, nir_imm_intN_t(&b->nb, total_mask, mask->bit_size));
   for (unsigned i = 0; i < out_elems; i++) {
      nir_ssa_def *this_mask = nir_channel(&b->nb, mask, i);
      nir_ssa_def *vmask = nir_iand(&b->nb, this_mask, nir_imm_intN_t(&b->nb, half_mask, mask->bit_size));
      nir_ssa_def *val0 = nir_vector_extract(&b->nb, input0, vmask);
      nir_ssa_def *val1 = nir_vector_extract(&b->nb, input1, vmask);
      nir_ssa_def *sel = nir_ilt(&b->nb, this_mask, nir_imm_intN_t(&b->nb, in_elems, mask->bit_size));
      outres[i] = nir_bcsel(&b->nb, sel, val0, val1);
   }
   return nir_vec(&b->nb, outres, out_elems);
}

bool
vtn_handle_opencl_instruction(struct vtn_builder *b, SpvOp ext_opcode,
                              const uint32_t *w, unsigned count)
{
   enum OpenCLstd_Entrypoints cl_opcode = (enum OpenCLstd_Entrypoints) ext_opcode;

   switch (cl_opcode) {
   case OpenCLstd_Fabs:
   case OpenCLstd_SAbs:
   case OpenCLstd_UAbs:
   case OpenCLstd_SAdd_sat:
   case OpenCLstd_UAdd_sat:
   case OpenCLstd_Ceil:
   case OpenCLstd_Cos:
   case OpenCLstd_Exp2:
   case OpenCLstd_Log2:
   case OpenCLstd_Floor:
   case OpenCLstd_Fma:
   case OpenCLstd_Fmax:
   case OpenCLstd_SHadd:
   case OpenCLstd_UHadd:
   case OpenCLstd_SMax:
   case OpenCLstd_UMax:
   case OpenCLstd_Fmin:
   case OpenCLstd_SMin:
   case OpenCLstd_UMin:
   case OpenCLstd_Mix:
   case OpenCLstd_Native_cos:
   case OpenCLstd_Native_divide:
   case OpenCLstd_Native_exp2:
   case OpenCLstd_Native_log2:
   case OpenCLstd_Native_powr:
   case OpenCLstd_Native_recip:
   case OpenCLstd_Native_rsqrt:
   case OpenCLstd_Native_sin:
   case OpenCLstd_Native_sqrt:
   case OpenCLstd_Fmod:
   case OpenCLstd_SMul_hi:
   case OpenCLstd_UMul_hi:
   case OpenCLstd_Popcount:
   case OpenCLstd_Pow:
   case OpenCLstd_Remainder:
   case OpenCLstd_SRhadd:
   case OpenCLstd_URhadd:
   case OpenCLstd_Rsqrt:
   case OpenCLstd_Sign:
   case OpenCLstd_Sin:
   case OpenCLstd_Sqrt:
   case OpenCLstd_SSub_sat:
   case OpenCLstd_USub_sat:
   case OpenCLstd_Trunc:
   case OpenCLstd_Rint:
      handle_instr(b, cl_opcode, w, count, handle_alu);
      return true;
   case OpenCLstd_SAbs_diff:
   case OpenCLstd_UAbs_diff:
   case OpenCLstd_SMad_hi:
   case OpenCLstd_UMad_hi:
   case OpenCLstd_SMad24:
   case OpenCLstd_UMad24:
   case OpenCLstd_SMul24:
   case OpenCLstd_UMul24:
   case OpenCLstd_Bitselect:
   case OpenCLstd_FClamp:
   case OpenCLstd_SClamp:
   case OpenCLstd_UClamp:
   case OpenCLstd_Copysign:
   case OpenCLstd_Cross:
   case OpenCLstd_Degrees:
   case OpenCLstd_Fdim:
   case OpenCLstd_Distance:
   case OpenCLstd_Fast_distance:
   case OpenCLstd_Fast_length:
   case OpenCLstd_Fast_normalize:
   case OpenCLstd_Length:
   case OpenCLstd_Mad:
   case OpenCLstd_Maxmag:
   case OpenCLstd_Minmag:
   case OpenCLstd_Nan:
   case OpenCLstd_Nextafter:
   case OpenCLstd_Normalize:
   case OpenCLstd_Radians:
   case OpenCLstd_Rotate:
   case OpenCLstd_Select:
   case OpenCLstd_Step:
   case OpenCLstd_Smoothstep:
   case OpenCLstd_S_Upsample:
   case OpenCLstd_U_Upsample:
   case OpenCLstd_Clz:
   case OpenCLstd_Native_exp:
   case OpenCLstd_Native_exp10:
   case OpenCLstd_Native_log:
   case OpenCLstd_Native_log10:
   case OpenCLstd_Native_tan:
      handle_instr(b, cl_opcode, w, count, handle_special);
      return true;
   case OpenCLstd_Vloadn:
      vtn_handle_opencl_vload(b, cl_opcode, w, count);
      return true;
   case OpenCLstd_Vstoren:
      vtn_handle_opencl_vstore(b, cl_opcode, w, count);
      return true;
   case OpenCLstd_Shuffle:
      handle_instr(b, cl_opcode, w, count, handle_shuffle);
      return true;
   case OpenCLstd_Shuffle2:
      handle_instr(b, cl_opcode, w, count, handle_shuffle2);
      return true;
   case OpenCLstd_Round:
      handle_instr(b, cl_opcode, w, count, handle_round);
      return true;
   case OpenCLstd_Printf:
      handle_instr(b, cl_opcode, w, count, handle_printf);
      return true;
   case OpenCLstd_Prefetch:
      /* TODO maybe add a nir instruction for this? */
      return true;
   default:
      vtn_fail("unhandled opencl opc: %u\n", ext_opcode);
      return false;
   }
}

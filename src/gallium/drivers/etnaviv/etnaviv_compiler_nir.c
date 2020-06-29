/*
 * Copyright (c) 2012-2019 Etnaviv Project
 * Copyright (c) 2019 Zodiac Inflight Innovations
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Jonathan Marek <jonathan@marek.ca>
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */

#include "etnaviv_compiler.h"
#include "etnaviv_compiler_nir.h"
#include "etnaviv_asm.h"
#include "etnaviv_context.h"
#include "etnaviv_debug.h"
#include "etnaviv_disasm.h"
#include "etnaviv_nir.h"
#include "etnaviv_uniforms.h"
#include "etnaviv_util.h"

#include <math.h>
#include "util/u_memory.h"
#include "util/register_allocate.h"
#include "compiler/nir/nir_builder.h"

#include "tgsi/tgsi_strings.h"
#include "util/u_half.h"

static bool
etna_alu_to_scalar_filter_cb(const nir_instr *instr, const void *data)
{
   const struct etna_specs *specs = data;

   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   switch (alu->op) {
   case nir_op_frsq:
   case nir_op_frcp:
   case nir_op_flog2:
   case nir_op_fexp2:
   case nir_op_fsqrt:
   case nir_op_fcos:
   case nir_op_fsin:
   case nir_op_fdiv:
   case nir_op_imul:
      return true;
   /* TODO: can do better than alu_to_scalar for vector compares */
   case nir_op_b32all_fequal2:
   case nir_op_b32all_fequal3:
   case nir_op_b32all_fequal4:
   case nir_op_b32any_fnequal2:
   case nir_op_b32any_fnequal3:
   case nir_op_b32any_fnequal4:
   case nir_op_b32all_iequal2:
   case nir_op_b32all_iequal3:
   case nir_op_b32all_iequal4:
   case nir_op_b32any_inequal2:
   case nir_op_b32any_inequal3:
   case nir_op_b32any_inequal4:
      return true;
   case nir_op_fdot2:
      if (!specs->has_halti2_instructions)
         return true;
      break;
   default:
      break;
   }

   return false;
}

static void
etna_emit_block_start(struct etna_compile *c, unsigned block)
{
   c->block_ptr[block] = c->inst_ptr;
}

static void
etna_emit_output(struct etna_compile *c, nir_variable *var, struct etna_inst_src src)
{
   struct etna_shader_io_file *sf = &c->variant->outfile;

   if (is_fs(c)) {
      switch (var->data.location) {
      case FRAG_RESULT_COLOR:
      case FRAG_RESULT_DATA0: /* DATA0 is used by gallium shaders for color */
         c->variant->ps_color_out_reg = src.reg;
         break;
      case FRAG_RESULT_DEPTH:
         c->variant->ps_depth_out_reg = src.reg;
         break;
      default:
         unreachable("Unsupported fs output");
      }
      return;
   }

   switch (var->data.location) {
   case VARYING_SLOT_POS:
      c->variant->vs_pos_out_reg = src.reg;
      break;
   case VARYING_SLOT_PSIZ:
      c->variant->vs_pointsize_out_reg = src.reg;
      break;
   default:
      sf->reg[sf->num_reg].reg = src.reg;
      sf->reg[sf->num_reg].slot = var->data.location;
      sf->reg[sf->num_reg].num_components = glsl_get_components(var->type);
      sf->num_reg++;
      break;
   }
}

#define OPT(nir, pass, ...) ({                             \
   bool this_progress = false;                             \
   NIR_PASS(this_progress, nir, pass, ##__VA_ARGS__);      \
   this_progress;                                          \
})

static void
etna_optimize_loop(nir_shader *s)
{
   bool progress;
   do {
      progress = false;

      NIR_PASS_V(s, nir_lower_vars_to_ssa);
      progress |= OPT(s, nir_opt_copy_prop_vars);
      progress |= OPT(s, nir_copy_prop);
      progress |= OPT(s, nir_opt_dce);
      progress |= OPT(s, nir_opt_cse);
      progress |= OPT(s, nir_opt_peephole_select, 16, true, true);
      progress |= OPT(s, nir_opt_intrinsics);
      progress |= OPT(s, nir_opt_algebraic);
      progress |= OPT(s, nir_opt_constant_folding);
      progress |= OPT(s, nir_opt_dead_cf);
      if (OPT(s, nir_opt_trivial_continues)) {
         progress = true;
         /* If nir_opt_trivial_continues makes progress, then we need to clean
          * things up if we want any hope of nir_opt_if or nir_opt_loop_unroll
          * to make progress.
          */
         OPT(s, nir_copy_prop);
         OPT(s, nir_opt_dce);
      }
      progress |= OPT(s, nir_opt_loop_unroll, nir_var_all);
      progress |= OPT(s, nir_opt_if, false);
      progress |= OPT(s, nir_opt_remove_phis);
      progress |= OPT(s, nir_opt_undef);
   }
   while (progress);
}

static int
etna_glsl_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void
copy_uniform_state_to_shader(struct etna_shader_variant *sobj, uint64_t *consts, unsigned count)
{
   struct etna_shader_uniform_info *uinfo = &sobj->uniforms;

   uinfo->imm_count = count * 4;
   uinfo->imm_data = MALLOC(uinfo->imm_count * sizeof(*uinfo->imm_data));
   uinfo->imm_contents = MALLOC(uinfo->imm_count * sizeof(*uinfo->imm_contents));

   for (unsigned i = 0; i < uinfo->imm_count; i++) {
      uinfo->imm_data[i] = consts[i];
      uinfo->imm_contents[i] = consts[i] >> 32;
   }

   etna_set_shader_uniforms_dirty_flags(sobj);
}

#include "etnaviv_compiler_nir_emit.h"

static bool
etna_compile_check_limits(struct etna_shader_variant *v)
{
   const struct etna_specs *specs = v->shader->specs;
   int max_uniforms = (v->stage == MESA_SHADER_VERTEX)
                         ? specs->max_vs_uniforms
                         : specs->max_ps_uniforms;

   if (!specs->has_icache && v->needs_icache) {
      DBG("Number of instructions (%d) exceeds maximum %d", v->code_size / 4,
          specs->max_instructions);
      return false;
   }

   if (v->num_temps > specs->max_registers) {
      DBG("Number of registers (%d) exceeds maximum %d", v->num_temps,
          specs->max_registers);
      return false;
   }

   if (v->uniforms.imm_count / 4 > max_uniforms) {
      DBG("Number of uniforms (%d) exceeds maximum %d",
          v->uniforms.imm_count / 4, max_uniforms);
      return false;
   }

   return true;
}

static void
fill_vs_mystery(struct etna_shader_variant *v)
{
   const struct etna_specs *specs = v->shader->specs;

   v->input_count_unk8 = DIV_ROUND_UP(v->infile.num_reg + 4, 16); /* XXX what is this */

   /* fill in "mystery meat" load balancing value. This value determines how
    * work is scheduled between VS and PS
    * in the unified shader architecture. More precisely, it is determined from
    * the number of VS outputs, as well as chip-specific
    * vertex output buffer size, vertex cache size, and the number of shader
    * cores.
    *
    * XXX this is a conservative estimate, the "optimal" value is only known for
    * sure at link time because some
    * outputs may be unused and thus unmapped. Then again, in the general use
    * case with GLSL the vertex and fragment
    * shaders are linked already before submitting to Gallium, thus all outputs
    * are used.
    *
    * note: TGSI compiler counts all outputs (including position and pointsize), here
    * v->outfile.num_reg only counts varyings, +1 to compensate for the position output
    * TODO: might have a problem that we don't count pointsize when it is used
    */

   int half_out = v->outfile.num_reg / 2 + 1;
   assert(half_out);

   uint32_t b = ((20480 / (specs->vertex_output_buffer_size -
                           2 * half_out * specs->vertex_cache_size)) +
                 9) /
                10;
   uint32_t a = (b + 256 / (specs->shader_core_count * half_out)) / 2;
   v->vs_load_balancing = VIVS_VS_LOAD_BALANCING_A(MIN2(a, 255)) |
                             VIVS_VS_LOAD_BALANCING_B(MIN2(b, 255)) |
                             VIVS_VS_LOAD_BALANCING_C(0x3f) |
                             VIVS_VS_LOAD_BALANCING_D(0x0f);
}

bool
etna_compile_shader_nir(struct etna_shader_variant *v)
{
   if (unlikely(!v))
      return false;

   struct etna_compile *c = CALLOC_STRUCT(etna_compile);
   if (!c)
      return false;

   c->variant = v;
   c->specs = v->shader->specs;
   c->nir = nir_shader_clone(NULL, v->shader->nir);

   nir_shader *s = c->nir;
   const struct etna_specs *specs = c->specs;

   v->stage = s->info.stage;
   v->num_loops = 0; /* TODO */
   v->vs_id_in_reg = -1;
   v->vs_pos_out_reg = -1;
   v->vs_pointsize_out_reg = -1;
   v->ps_color_out_reg = 0; /* 0 for shader that doesn't write fragcolor.. */
   v->ps_depth_out_reg = -1;

   /* setup input linking */
   struct etna_shader_io_file *sf = &v->infile;
   if (s->info.stage == MESA_SHADER_VERTEX) {
      nir_foreach_variable(var, &s->inputs) {
         unsigned idx = var->data.driver_location;
         sf->reg[idx].reg = idx;
         sf->reg[idx].slot = var->data.location;
         sf->reg[idx].num_components = glsl_get_components(var->type);
         sf->num_reg = MAX2(sf->num_reg, idx+1);
      }
   } else {
      unsigned count = 0;
      nir_foreach_variable(var, &s->inputs) {
         unsigned idx = var->data.driver_location;
         sf->reg[idx].reg = idx + 1;
         sf->reg[idx].slot = var->data.location;
         sf->reg[idx].num_components = glsl_get_components(var->type);
         sf->num_reg = MAX2(sf->num_reg, idx+1);
         count++;
      }
      assert(sf->num_reg == count);
   }

   NIR_PASS_V(s, nir_lower_io, ~nir_var_shader_out, etna_glsl_type_size,
            (nir_lower_io_options)0);

   NIR_PASS_V(s, nir_lower_regs_to_ssa);
   NIR_PASS_V(s, nir_lower_vars_to_ssa);
   NIR_PASS_V(s, nir_lower_indirect_derefs, nir_var_all);
   NIR_PASS_V(s, nir_lower_tex, &(struct nir_lower_tex_options) { .lower_txp = ~0u });
   NIR_PASS_V(s, nir_lower_alu_to_scalar, etna_alu_to_scalar_filter_cb, specs);

   etna_optimize_loop(s);

   NIR_PASS_V(s, etna_lower_io, v);

   if (v->shader->specs->vs_need_z_div)
      NIR_PASS_V(s, nir_lower_clip_halfz);

   /* lower pre-halti2 to float (halti0 has integers, but only scalar..) */
   if (c->specs->halti < 2) {
      /* use opt_algebraic between int_to_float and boot_to_float because
       * int_to_float emits ftrunc, and ftrunc lowering generates bool ops
       */
      NIR_PASS_V(s, nir_lower_int_to_float);
      NIR_PASS_V(s, nir_opt_algebraic);
      NIR_PASS_V(s, nir_lower_bool_to_float);
   } else {
      NIR_PASS_V(s, nir_lower_idiv, nir_lower_idiv_fast);
      NIR_PASS_V(s, nir_lower_bool_to_int32);
   }

   etna_optimize_loop(s);

   if (DBG_ENABLED(ETNA_DBG_DUMP_SHADERS))
      nir_print_shader(s, stdout);

   while( OPT(s, nir_opt_vectorize) );
   NIR_PASS_V(s, nir_lower_alu_to_scalar, etna_alu_to_scalar_filter_cb, specs);

   NIR_PASS_V(s, nir_remove_dead_variables, nir_var_function_temp, NULL);
   NIR_PASS_V(s, nir_opt_algebraic_late);

   NIR_PASS_V(s, nir_move_vec_src_uses_to_dest);
   NIR_PASS_V(s, nir_copy_prop);
   /* only HW supported integer source mod is ineg for iadd instruction (?) */
   NIR_PASS_V(s, nir_lower_to_source_mods, ~nir_lower_int_source_mods);
   /* need copy prop after uses_to_dest, and before src mods: see
    * dEQP-GLES2.functional.shaders.random.all_features.fragment.95
    */

   NIR_PASS_V(s, nir_opt_dce);

   NIR_PASS_V(s, etna_lower_alu, c->specs->has_new_transcendentals);

   if (DBG_ENABLED(ETNA_DBG_DUMP_SHADERS))
      nir_print_shader(s, stdout);

   unsigned block_ptr[nir_shader_get_entrypoint(s)->num_blocks];
   c->block_ptr = block_ptr;

   unsigned num_consts;
   ASSERTED bool ok = emit_shader(c, &v->num_temps, &num_consts);
   assert(ok);

   /* empty shader, emit NOP */
   if (!c->inst_ptr)
      emit_inst(c, &(struct etna_inst) { .opcode = INST_OPCODE_NOP });

   /* assemble instructions, fixing up labels */
   uint32_t *code = MALLOC(c->inst_ptr * 16);
   for (unsigned i = 0; i < c->inst_ptr; i++) {
      struct etna_inst *inst = &c->code[i];
      if (inst->opcode == INST_OPCODE_BRANCH)
         inst->imm = block_ptr[inst->imm];

      inst->halti5 = specs->halti >= 5;
      etna_assemble(&code[i * 4], inst);
   }

   v->code_size = c->inst_ptr * 4;
   v->code = code;
   v->needs_icache = c->inst_ptr > specs->max_instructions;

   copy_uniform_state_to_shader(v, c->consts, num_consts);

   if (s->info.stage == MESA_SHADER_FRAGMENT) {
      v->input_count_unk8 = 31; /* XXX what is this */
      assert(v->ps_depth_out_reg <= 0);
   } else {
      fill_vs_mystery(v);
   }

   bool result = etna_compile_check_limits(v);
   ralloc_free(c->nir);
   FREE(c);
   return result;
}

void
etna_destroy_shader_nir(struct etna_shader_variant *shader)
{
   assert(shader);

   FREE(shader->code);
   FREE(shader->uniforms.imm_data);
   FREE(shader->uniforms.imm_contents);
   FREE(shader);
}

extern const char *tgsi_swizzle_names[];
void
etna_dump_shader_nir(const struct etna_shader_variant *shader)
{
   if (shader->stage == MESA_SHADER_VERTEX)
      printf("VERT\n");
   else
      printf("FRAG\n");

   etna_disasm(shader->code, shader->code_size, PRINT_RAW);

   printf("num loops: %i\n", shader->num_loops);
   printf("num temps: %i\n", shader->num_temps);
   printf("immediates:\n");
   for (int idx = 0; idx < shader->uniforms.imm_count; ++idx) {
      printf(" [%i].%s = %f (0x%08x) (%d)\n",
             idx / 4,
             tgsi_swizzle_names[idx % 4],
             *((float *)&shader->uniforms.imm_data[idx]),
             shader->uniforms.imm_data[idx],
             shader->uniforms.imm_contents[idx]);
   }
   printf("inputs:\n");
   for (int idx = 0; idx < shader->infile.num_reg; ++idx) {
      printf(" [%i] name=%s comps=%i\n", shader->infile.reg[idx].reg,
               (shader->stage == MESA_SHADER_VERTEX) ?
               gl_vert_attrib_name(shader->infile.reg[idx].slot) :
               gl_varying_slot_name(shader->infile.reg[idx].slot),
               shader->infile.reg[idx].num_components);
   }
   printf("outputs:\n");
   for (int idx = 0; idx < shader->outfile.num_reg; ++idx) {
      printf(" [%i] name=%s comps=%i\n", shader->outfile.reg[idx].reg,
               (shader->stage == MESA_SHADER_VERTEX) ?
               gl_varying_slot_name(shader->outfile.reg[idx].slot) :
               gl_frag_result_name(shader->outfile.reg[idx].slot),
               shader->outfile.reg[idx].num_components);
   }
   printf("special:\n");
   if (shader->stage == MESA_SHADER_VERTEX) {
      printf("  vs_pos_out_reg=%i\n", shader->vs_pos_out_reg);
      printf("  vs_pointsize_out_reg=%i\n", shader->vs_pointsize_out_reg);
      printf("  vs_load_balancing=0x%08x\n", shader->vs_load_balancing);
   } else {
      printf("  ps_color_out_reg=%i\n", shader->ps_color_out_reg);
      printf("  ps_depth_out_reg=%i\n", shader->ps_depth_out_reg);
   }
   printf("  input_count_unk8=0x%08x\n", shader->input_count_unk8);
}

static const struct etna_shader_inout *
etna_shader_vs_lookup(const struct etna_shader_variant *sobj,
                      const struct etna_shader_inout *in)
{
   for (int i = 0; i < sobj->outfile.num_reg; i++)
      if (sobj->outfile.reg[i].slot == in->slot)
         return &sobj->outfile.reg[i];

   return NULL;
}

bool
etna_link_shader_nir(struct etna_shader_link_info *info,
                     const struct etna_shader_variant *vs,
                     const struct etna_shader_variant *fs)
{
   int comp_ofs = 0;
   /* For each fragment input we need to find the associated vertex shader
    * output, which can be found by matching on semantic name and index. A
    * binary search could be used because the vs outputs are sorted by their
    * semantic index and grouped by semantic type by fill_in_vs_outputs.
    */
   assert(fs->infile.num_reg < ETNA_NUM_INPUTS);
   info->pcoord_varying_comp_ofs = -1;

   for (int idx = 0; idx < fs->infile.num_reg; ++idx) {
      const struct etna_shader_inout *fsio = &fs->infile.reg[idx];
      const struct etna_shader_inout *vsio = etna_shader_vs_lookup(vs, fsio);
      struct etna_varying *varying;
      bool interpolate_always = true;

      assert(fsio->reg > 0 && fsio->reg <= ARRAY_SIZE(info->varyings));

      if (fsio->reg > info->num_varyings)
         info->num_varyings = fsio->reg;

      varying = &info->varyings[fsio->reg - 1];
      varying->num_components = fsio->num_components;

      if (!interpolate_always) /* colors affected by flat shading */
         varying->pa_attributes = 0x200;
      else /* texture coord or other bypasses flat shading */
         varying->pa_attributes = 0x2f1;

      varying->use[0] = VARYING_COMPONENT_USE_UNUSED;
      varying->use[1] = VARYING_COMPONENT_USE_UNUSED;
      varying->use[2] = VARYING_COMPONENT_USE_UNUSED;
      varying->use[3] = VARYING_COMPONENT_USE_UNUSED;

      /* point coord is an input to the PS without matching VS output,
       * so it gets a varying slot without being assigned a VS register.
       */
      if (fsio->slot == VARYING_SLOT_PNTC) {
         varying->use[0] = VARYING_COMPONENT_USE_POINTCOORD_X;
         varying->use[1] = VARYING_COMPONENT_USE_POINTCOORD_Y;

         info->pcoord_varying_comp_ofs = comp_ofs;
      } else {
         if (vsio == NULL) { /* not found -- link error */
            BUG("Semantic value not found in vertex shader outputs\n");
            return true;
         }
         varying->reg = vsio->reg;
      }

      comp_ofs += varying->num_components;
   }

   assert(info->num_varyings == fs->infile.num_reg);

   return false;
}

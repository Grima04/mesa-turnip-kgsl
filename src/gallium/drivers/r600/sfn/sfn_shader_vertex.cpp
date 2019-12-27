/* -*- mesa-c++  -*-
 *
 * Copyright (c) 2018 Collabora LTD
 *
 * Author: Gert Wollny <gert.wollny@collabora.com>
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


#include "pipe/p_defines.h"
#include "tgsi/tgsi_from_mesa.h"
#include "sfn_shader_vertex.h"

#include <queue>


namespace r600 {

using std::priority_queue;

VertexShaderFromNir::VertexShaderFromNir(r600_pipe_shader *sh,
                                         r600_pipe_shader_selector& sel,
                                         const r600_shader_key& key):
   ShaderFromNirProcessor (PIPE_SHADER_VERTEX, sel, sh->shader,
                           sh->scratch_space_needed),
   m_num_clip_dist(0),
   m_last_param_export(nullptr),
   m_last_pos_export(nullptr),
   m_pipe_shader(sh),
   m_enabled_stream_buffers_mask(0),
   m_so_info(&sel.so),
   m_cur_param(0),
   m_cur_clip_pos(1),
   m_vertex_id(),
   m_key(key)
{
   // reg 0 is used in the fetch shader
   increment_reserved_registers();

   sh_info().atomic_base = key.vs.first_atomic_counter;
}

bool VertexShaderFromNir::do_process_inputs(nir_variable *input)
{
   ++sh_info().ninput;

   if (input->data.location < VERT_ATTRIB_MAX) {
      increment_reserved_registers();
      return true;
   }
   fprintf(stderr, "r600-NIR-VS: Unimplemented process_inputs for %d\n", input->data.location);
   return false;
}

bool VertexShaderFromNir::allocate_reserved_registers()
{
   /* Since the vertex ID is nearly always used, we add it here as an input so
    * that the registers used for vertex attributes don't get clobbered by the
    * register merge step */
   auto R0x = new GPRValue(0,0);
   R0x->set_as_input();
   m_vertex_id.reset(R0x);
   inject_register(0, 0, m_vertex_id, false);

   if (m_sv_values.test(es_instanceid)) {
      auto R0w = new GPRValue(0,3);
      R0w->set_as_input();
      m_instance_id.reset(R0w);
      inject_register(0, 3, m_instance_id, false);
   }

   priority_queue<int, std::vector<int>, std::greater<int>>  q;
   for (auto a: m_param_map) {
      q.push(a.first);
   }

   int next_param = 0;
   while (!q.empty()) {
      int loc = q.top();
      q.pop();
      m_param_map[loc] = next_param++;
   }
   return true;
}

bool VertexShaderFromNir::scan_sysvalue_access(nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *ii =  nir_instr_as_intrinsic(instr);
      switch (ii->intrinsic) {
      case nir_intrinsic_load_vertex_id:
         m_sv_values.set(es_vertexid);
         break;
      case nir_intrinsic_load_instance_id:
         m_sv_values.set(es_instanceid);
         break;
      default:
         ;
      }
   }
   default:
      ;
   }
   return true;
}

bool VertexShaderFromNir::emit_intrinsic_instruction_override(nir_intrinsic_instr* instr)
{
   switch (instr->intrinsic) {
   case nir_intrinsic_load_vertex_id:
      return load_preloaded_value(instr->dest, 0, m_vertex_id);
   case nir_intrinsic_load_instance_id:
      return load_preloaded_value(instr->dest, 0, m_instance_id);
   default:
      return false;
   }
}

bool VertexShaderFromNir::do_process_outputs(nir_variable *output)
{
   if (output->data.location == VARYING_SLOT_COL0 ||
       output->data.location == VARYING_SLOT_COL1 ||
       (output->data.location >= VARYING_SLOT_VAR0 &&
       output->data.location <= VARYING_SLOT_VAR31) ||
       (output->data.location >= VARYING_SLOT_TEX0 &&
        output->data.location <= VARYING_SLOT_TEX7) ||
       output->data.location == VARYING_SLOT_BFC0 ||
       output->data.location == VARYING_SLOT_BFC1 ||
       output->data.location == VARYING_SLOT_CLIP_VERTEX ||
       output->data.location == VARYING_SLOT_CLIP_DIST0 ||
       output->data.location == VARYING_SLOT_CLIP_DIST1 ||
       output->data.location == VARYING_SLOT_POS ||
       output->data.location == VARYING_SLOT_PSIZ ||
       output->data.location == VARYING_SLOT_FOGC ||
       output->data.location == VARYING_SLOT_LAYER ||
       output->data.location == VARYING_SLOT_EDGE ||
       output->data.location == VARYING_SLOT_VIEWPORT
       ) {

      r600_shader_io& io = sh_info().output[output->data.driver_location];
      tgsi_get_gl_varying_semantic(static_cast<gl_varying_slot>( output->data.location),
                                   true, &io.name, &io.sid);
      if (! m_key.vs.as_es)
         evaluate_spi_sid(io);
      ++sh_info().noutput;

      if (output->data.location == VARYING_SLOT_PSIZ ||
          output->data.location == VARYING_SLOT_EDGE ||
          output->data.location == VARYING_SLOT_LAYER)
            m_cur_clip_pos = 2;

      if (output->data.location != VARYING_SLOT_POS &&
          output->data.location != VARYING_SLOT_EDGE &&
          output->data.location != VARYING_SLOT_PSIZ &&
          output->data.location != VARYING_SLOT_CLIP_VERTEX)
         m_param_map[output->data.location] = m_cur_param++;

      return true;
   }
   return false;
}

bool VertexShaderFromNir::do_emit_load_deref(const nir_variable *in_var, nir_intrinsic_instr* instr)
{
   if (in_var->data.location < VERT_ATTRIB_MAX) {
      for (int i = 0; i < instr->num_components ; ++i) {
         auto s = new GPRValue(in_var->data.driver_location + 1, i);
         s->set_as_input();
         auto src = PValue(s);
         inject_register(in_var->data.driver_location + 1, i, src, false);

         if (i == 0)
            set_input(in_var->data.driver_location, src);

         load_preloaded_value(instr->dest, i, src, i == instr->num_components - 1);
      }
      return true;
   }
   fprintf(stderr, "r600-NIR: Unimplemented load_deref for %d\n", in_var->data.location);
   return false;
}

bool VertexShaderFromNir::emit_clip_vertices(const nir_variable *out_var, nir_intrinsic_instr* instr)
{
   sh_info().cc_dist_mask = 0xff;
   sh_info().clip_dist_write = 0xff;

   std::unique_ptr<GPRVector> clip_vertex(vec_from_nir_with_fetch_constant(instr->src[1], 0xf, {0,1,2,3}));

   for (int i = 0; i < 4; ++i)
      sh_info().output[out_var->data.driver_location].write_mask |= 1 << i;

   GPRVector clip_dist[2] = { get_temp_vec4(), get_temp_vec4()};

   for (int i = 0; i < 8; i++) {
      int oreg = i >> 2;
      int ochan = i & 3;
      AluInstruction *ir = nullptr;
      for (int j = 0; j < 4; j++) {
         ir = new AluInstruction(op2_dot4_ieee, clip_dist[oreg].reg_i(j), clip_vertex->reg_i(j),
                                 PValue(new UniformValue(512 + i, j, R600_BUFFER_INFO_CONST_BUFFER)),
                                 (j == ochan) ? EmitInstruction::write : EmitInstruction::empty);
         emit_instruction(ir);
      }
      ir->set_flag(alu_last_instr);
   }

   m_last_pos_export = new ExportInstruction(m_cur_clip_pos++, clip_dist[0], ExportInstruction::et_pos);
   emit_export_instruction(m_last_pos_export);

   m_last_pos_export = new ExportInstruction(m_cur_clip_pos, clip_dist[1], ExportInstruction::et_pos);
   emit_export_instruction(m_last_pos_export);

   return true;
}

bool VertexShaderFromNir::emit_varying_pos(const nir_variable *out_var, nir_intrinsic_instr* instr,
                                           std::array<uint32_t, 4> *swizzle_override)
{
   std::array<uint32_t,4> swizzle;
   uint32_t write_mask = 0;

   if (swizzle_override) {
      swizzle = *swizzle_override;
      for (int i = 0; i < 4; ++i) {
         if (swizzle[i] < 6)
            write_mask |= 1 << i;
      }
   } else {
      write_mask = nir_intrinsic_write_mask(instr) << out_var->data.location_frac;
      for (int i = 0; i < 4; ++i)
         swizzle[i] = ((1 << i) & write_mask) ? i - out_var->data.location_frac : 7;
   }

   sh_info().output[out_var->data.driver_location].write_mask = write_mask;

   GPRVector *value = vec_from_nir_with_fetch_constant(instr->src[1], write_mask, swizzle);
   set_output(out_var->data.driver_location, PValue(value));

   int export_slot = 0;

   switch (out_var->data.location) {
   case VARYING_SLOT_EDGE: {
      sh_info().vs_out_misc_write = 1;
      sh_info().vs_out_edgeflag = 1;
      emit_instruction(op1_mov, value->reg_i(1), {value->reg_i(1)}, {alu_write, alu_dst_clamp, alu_last_instr});
      emit_instruction(op1_flt_to_int, value->reg_i(1), {value->reg_i(1)}, {alu_write, alu_last_instr});
      sh_info().output[out_var->data.driver_location].write_mask = 0xf;
   }
      /* fallthrough */
   case VARYING_SLOT_PSIZ:
   case VARYING_SLOT_LAYER:
      export_slot = 1;
      break;
   case VARYING_SLOT_POS:
      break;
   case VARYING_SLOT_CLIP_DIST0:
   case VARYING_SLOT_CLIP_DIST1:
      export_slot = m_cur_clip_pos++;
      break;
   default:
      sfn_log << SfnLog::err << __func__ << "Unsupported location "
              << out_var->data.location << "\n";
      return false;
   }

   m_last_pos_export = new ExportInstruction(export_slot, *value, ExportInstruction::et_pos);
   emit_export_instruction(m_last_pos_export);
   add_param_output_reg(out_var->data.driver_location, m_last_pos_export->gpr_ptr());
   return true;
}

bool VertexShaderFromNir::emit_varying_param(const nir_variable *out_var, nir_intrinsic_instr* instr)
{
   assert(out_var->data.driver_location < sh_info().noutput);
   sfn_log << SfnLog::io << __func__ << ": emit DDL: " << out_var->data.driver_location << "\n";

   int write_mask = nir_intrinsic_write_mask(instr) << out_var->data.location_frac;
   std::array<uint32_t,4> swizzle;
   for (int i = 0; i < 4; ++i)
      swizzle[i] = ((1 << i) & write_mask) ? i - out_var->data.location_frac : 7;

   sh_info().output[out_var->data.driver_location].write_mask = write_mask;

   GPRVector *value = vec_from_nir_with_fetch_constant(instr->src[1], write_mask, swizzle);
   sh_info().output[out_var->data.driver_location].gpr = value->sel();

   /* This should use the registers!! */
   set_output(out_var->data.driver_location, PValue(value));

   auto param_loc = m_param_map.find(out_var->data.location);
   assert(param_loc != m_param_map.end());

   m_last_param_export = new ExportInstruction(param_loc->second, *value, ExportInstruction::et_param);
   emit_export_instruction(m_last_param_export);
   add_param_output_reg(out_var->data.driver_location, m_last_param_export->gpr_ptr());
   return true;
}

bool VertexShaderFromNir::emit_stream(int stream)
{
   assert(m_so_info);
   if (m_so_info->num_outputs > PIPE_MAX_SO_OUTPUTS) {
           R600_ERR("Too many stream outputs: %d\n", m_so_info->num_outputs);
           return false;
   }
   for (unsigned i = 0; i < m_so_info->num_outputs; i++) {
           if (m_so_info->output[i].output_buffer >= 4) {
                   R600_ERR("Exceeded the max number of stream output buffers, got: %d\n",
                            m_so_info->output[i].output_buffer);
                   return false;
           }
   }
   const GPRVector *so_gpr[PIPE_MAX_SHADER_OUTPUTS];
   unsigned start_comp[PIPE_MAX_SHADER_OUTPUTS];
   std::vector<GPRVector> tmp(m_so_info->num_outputs);

   /* Initialize locations where the outputs are stored. */
   for (unsigned i = 0; i < m_so_info->num_outputs; i++) {
      if (stream != -1 && stream != m_so_info->output[i].stream)
         continue;

      sfn_log << SfnLog::instr << "Emit stream " << i
              << " with register index " << m_so_info->output[i].register_index << "  so_gpr:";


      so_gpr[i] = output_register(m_so_info->output[i].register_index);

      if (!so_gpr[i]) {
         sfn_log << SfnLog::err << "\nERR: register index "
                 << m_so_info->output[i].register_index
                 << " doesn't correspond to an output register\n";
         return false;
      }
      start_comp[i] = m_so_info->output[i].start_component;
      /* Lower outputs with dst_offset < start_component.
       *
       * We can only output 4D vectors with a write mask, e.g. we can
       * only output the W component at offset 3, etc. If we want
       * to store Y, Z, or W at buffer offset 0, we need to use MOV
       * to move it to X and output X. */
      if (m_so_info->output[i].dst_offset < m_so_info->output[i].start_component) {
         int tmp_index = allocate_temp_register();
         int sc = m_so_info->output[i].start_component;
         AluInstruction *alu = nullptr;
         for (int j = 0; j < m_so_info->output[i].num_components; j++) {
            PValue dst(new GPRValue(tmp_index, j));
            alu = new AluInstruction(op1_mov, dst, so_gpr[i]->reg_i(j + sc), {alu_write});
            tmp[i].set_reg_i(j, dst);
            emit_instruction(alu);
         }
         if (alu)
            alu->set_flag(alu_last_instr);

         /* Fill the vector with masked values */
         PValue dst_blank(new GPRValue(tmp_index, 7));
         for (int j = m_so_info->output[i].num_components; j < 4; j++)
            tmp[i].set_reg_i(j, dst_blank);

         start_comp[i] = 0;
         so_gpr[i] = &tmp[i];
      }
      sfn_log << SfnLog::instr <<  *so_gpr[i] << "\n";
   }

   /* Write outputs to buffers. */
   for (unsigned i = 0; i < m_so_info->num_outputs; i++) {
      sfn_log << SfnLog::instr << "Write output buffer " << i
              << " with register index " << m_so_info->output[i].register_index << "\n";

      StreamOutIntruction *out_stream =
            new StreamOutIntruction(*so_gpr[i],
                                    m_so_info->output[i].num_components,
                                    m_so_info->output[i].dst_offset - start_comp[i],
                                    ((1 << m_so_info->output[i].num_components) - 1) << start_comp[i],
                                    m_so_info->output[i].output_buffer,
                                    m_so_info->output[i].stream);
      emit_export_instruction(out_stream);
      m_enabled_stream_buffers_mask |= (1 << m_so_info->output[i].output_buffer) << m_so_info->output[i].stream * 4;
   }
   return true;
}

void VertexShaderFromNir::do_finalize()
{
   if (m_key.vs.as_gs_a) {
      PValue o(new GPRValue(0,PIPE_SWIZZLE_0));
      GPRVector primid({PValue(new GPRValue(0,2)), o,o,o});
      m_last_param_export = new ExportInstruction(m_cur_param, primid, ExportInstruction::et_param);
      emit_export_instruction(m_last_param_export);
      int i;
      i = sh_info().noutput++;
      auto& io = sh_info().output[i];
      io.name = TGSI_SEMANTIC_PRIMID;
      io.sid = 0;
      io.gpr = 0;
      io.interpolate = TGSI_INTERPOLATE_CONSTANT;
      io.write_mask = 0x4;
      io.spi_sid = m_key.vs.prim_id_out;
      sh_info().vs_as_gs_a = 1;
   }

   finalize_exports();
}


bool VertexShaderFromNirForFS::do_emit_store_deref(const nir_variable *out_var, nir_intrinsic_instr* instr)
{

   switch (out_var->data.location) {
   case VARYING_SLOT_PSIZ:
      sh_info().vs_out_point_size = 1;
      sh_info().vs_out_misc_write = 1;
      /* fallthrough */
   case VARYING_SLOT_POS:
      return emit_varying_pos(out_var, instr);
   case VARYING_SLOT_EDGE: {
      std::array<uint32_t, 4> swizzle_override = {7 ,0, 7, 7};
      return emit_varying_pos(out_var, instr, &swizzle_override);
   }
   case VARYING_SLOT_CLIP_VERTEX:
      return emit_clip_vertices(out_var, instr);
   case VARYING_SLOT_CLIP_DIST0:
   case VARYING_SLOT_CLIP_DIST1:
      m_num_clip_dist += 4;
      return emit_varying_param(out_var, instr) && emit_varying_pos(out_var, instr);
   case VARYING_SLOT_LAYER: {
      sh_info().vs_out_misc_write = 1;
      sh_info().vs_out_layer = 1;
      std::array<uint32_t, 4> swz = {7,7,0,7};
      return emit_varying_pos(out_var, instr, &swz) &&
            emit_varying_param(out_var, instr);
   }
   case VARYING_SLOT_VIEW_INDEX:
      return emit_varying_pos(out_var, instr) &&
            emit_varying_param(out_var, instr);

   default:
      if (out_var->data.location <= VARYING_SLOT_VAR31 ||
          (out_var->data.location >= VARYING_SLOT_TEX0 &&
           out_var->data.location <= VARYING_SLOT_TEX7))
         return emit_varying_param(out_var, instr);
   }

   fprintf(stderr, "r600-NIR: Unimplemented store_deref for %d\n",
           out_var->data.location);
   return false;
}

void VertexShaderFromNirForFS::finalize_exports()
{
   if (m_so_info && m_so_info->num_outputs)
      emit_stream(-1);

   m_pipe_shader->enabled_stream_buffers_mask = m_enabled_stream_buffers_mask;

   if (!m_last_param_export) {
      GPRVector value(0,{7,7,7,7});
      m_last_param_export = new ExportInstruction(0, value, ExportInstruction::et_param);
      emit_export_instruction(m_last_param_export);
   }
   m_last_param_export->set_last();

   if (!m_last_pos_export) {
      GPRVector value(0,{7,7,7,7});
      m_last_pos_export = new ExportInstruction(0, value, ExportInstruction::et_pos);
      emit_export_instruction(m_last_pos_export);
   }
   m_last_pos_export->set_last();

}

VertexShaderFromNirForGS::VertexShaderFromNirForGS(r600_pipe_shader *sh,
                                                   r600_pipe_shader_selector& sel,
                                                   const r600_shader_key &key,
                                                   const r600_shader *gs_shader):
   VertexShaderFromNir(sh, sel, key),
   m_gs_shader(gs_shader)
{
   sh->shader.vs_as_es = true;
}

bool VertexShaderFromNirForGS::do_emit_store_deref(const nir_variable *out_var, nir_intrinsic_instr* instr)
{

   assert(m_gs_shader);

   int ring_offset = -1;
   const r600_shader_io& out_io = sh_info().output[out_var->data.driver_location];

   sfn_log << SfnLog::io << "check output " << out_var->data.driver_location
           << " name=" << out_io.name<< " sid=" << out_io.sid << "\n";
   for (unsigned k = 0; k < m_gs_shader->ninput; ++k) {
      auto& in_io = m_gs_shader->input[k];
      sfn_log << SfnLog::io << "  against  " <<  k << " name=" << in_io.name<< " sid=" << in_io.sid << "\n";

      if (in_io.name == out_io.name &&
          in_io.sid == out_io.sid) {
         ring_offset = in_io.ring_offset;
         break;
      }
   }

   if (out_var->data.location == VARYING_SLOT_VIEWPORT)
      return true;

   if (ring_offset == -1) {
      sfn_log << SfnLog::err << "VS defines output at "
              << out_var->data.driver_location << "name=" << out_io.name
              << " sid=" << out_io.sid << " that is not consumed as GS input\n";
      return true;
   }

   uint32_t write_mask =  (1 << instr->num_components) - 1;

   std::unique_ptr<GPRVector> value(vec_from_nir_with_fetch_constant(instr->src[1], write_mask,
                                    swizzle_from_mask(instr->num_components)));

   auto ir = new MemRingOutIntruction(cf_mem_ring, mem_write, *value,
                                      ring_offset >> 2, 4, PValue());
   emit_export_instruction(ir);

   sh_info().output[out_var->data.driver_location].write_mask |= write_mask;
   if (out_var->data.location == VARYING_SLOT_CLIP_DIST0 ||
       out_var->data.location == VARYING_SLOT_CLIP_DIST1)
      m_num_clip_dist += 4;

   return true;
}

void VertexShaderFromNirForGS::finalize_exports()
{
}


VertexShaderFromNirForES::VertexShaderFromNirForES(r600_pipe_shader *sh,
                                                   UNUSED const pipe_stream_output_info *so_info,
                                                   r600_pipe_shader_selector&  sel,
                                                   const r600_shader_key &key):
   VertexShaderFromNir(sh, sel, key)
{
}

bool VertexShaderFromNirForES::do_emit_store_deref(UNUSED const nir_variable *out_var,
                                                   UNUSED nir_intrinsic_instr* instr)
{
   return false;
}

void VertexShaderFromNirForES::finalize_exports()
{
}

}

/* -*- mesa-c++  -*-
 *
 * Copyright (c) 2019 Collabora LTD
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


#include "sfn_instruction_export.h"
#include "sfn_valuepool.h"

namespace r600 {

WriteoutInstruction::WriteoutInstruction(instr_type t, const GPRVector& value):
   Instruction(t),
   m_value(value)
{
}

ExportInstruction::ExportInstruction(unsigned loc, const GPRVector &value, ExportType type):
   WriteoutInstruction(Instruction::exprt, value),
   m_type(type),
   m_loc(loc),
   m_is_last(false)
{
}


bool ExportInstruction::is_equal_to(const Instruction& lhs) const
{
   assert(lhs.type() == exprt);
   const auto& oth = static_cast<const ExportInstruction&>(lhs);

   return (gpr() == oth.gpr()) &&
         (m_type == oth.m_type) &&
         (m_loc == oth.m_loc) &&
         (m_is_last == oth.m_is_last);
}

void ExportInstruction::do_print(std::ostream& os) const
{
   os << (m_is_last ? "EXPORT_DONE ":"EXPORT ");
   switch (m_type) {
   case et_pixel: os << "PIXEL "; break;
   case et_pos: os << "POS "; break;
   case et_param: os << "PARAM "; break;
   }
   os << m_loc << " " << gpr();
}

void ExportInstruction::update_output_map(OutputRegisterMap& map) const
{
   map[m_loc] = gpr_ptr();
}

void ExportInstruction::set_last()
{
   m_is_last = true;
}

StreamOutIntruction::StreamOutIntruction(const GPRVector& value, int num_components,
                                         int array_base, int comp_mask, int out_buffer,
                                         int stream):
   WriteoutInstruction(Instruction::streamout, value),
   m_element_size(num_components == 3 ? 3 : num_components - 1),
   m_burst_count(1),
   m_array_base(array_base),
   m_array_size(0xfff),
   m_writemask(comp_mask),
   m_output_buffer(out_buffer),
   m_stream(stream)
{
}

unsigned StreamOutIntruction::op() const
{
   int op = 0;
   switch (m_output_buffer) {
   case 0: op = CF_OP_MEM_STREAM0_BUF0; break;
   case 1: op = CF_OP_MEM_STREAM0_BUF1; break;
   case 2: op = CF_OP_MEM_STREAM0_BUF2; break;
   case 3: op = CF_OP_MEM_STREAM0_BUF3; break;
   }
   return 4 * m_stream + op;
}

bool StreamOutIntruction::is_equal_to(const Instruction& lhs) const
{
   assert(lhs.type() == streamout);
   const auto& oth = static_cast<const StreamOutIntruction&>(lhs);

   return gpr() == oth.gpr() &&
         m_element_size == oth.m_element_size &&
         m_burst_count == oth.m_burst_count &&
         m_array_base == oth.m_array_base &&
         m_array_size == oth.m_array_size &&
         m_writemask == oth.m_writemask &&
         m_output_buffer == oth.m_output_buffer &&
         m_stream == oth.m_stream;
}

void StreamOutIntruction::do_print(std::ostream& os) const
{
   os << "WRITE STREAM(" << m_stream << ") "  << gpr()
      << " ES:" << m_element_size
      << " BC:" << m_burst_count
      << " BUF:" << m_output_buffer
      << " ARRAY:" <<  m_array_base;
   if (m_array_size != 0xfff)
      os << "+" << m_array_size;
}

}

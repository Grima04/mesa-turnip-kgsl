from __future__ import print_function

template = """\
/* Copyright (C) 2015 Broadcom
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

#ifndef _NIR_BUILDER_OPCODES_
#define _NIR_BUILDER_OPCODES_

<%
def src_decl_list(num_srcs):
   return ', '.join('nir_ssa_def *src' + str(i) for i in range(num_srcs))

def src_list(num_srcs):
   if num_srcs <= 4:
      return ', '.join('src' + str(i) if i < num_srcs else 'NULL' for i in range(4))
   else:
      return ', '.join('src' + str(i) for i in range(num_srcs))
%>

% for name, opcode in sorted(opcodes.items()):
static inline nir_ssa_def *
nir_${name}(nir_builder *build, ${src_decl_list(opcode.num_inputs)})
{
% if opcode.num_inputs <= 4:
   return nir_build_alu(build, nir_op_${name}, ${src_list(opcode.num_inputs)});
% else:
   nir_ssa_def *srcs[${opcode.num_inputs}] = {${src_list(opcode.num_inputs)}};
   return nir_build_alu_src_arr(build, nir_op_${name}, srcs);
% endif
}
% endfor

<%
def sysval_decl_list(opcode):
   res = ''
   if opcode.indices:
      res += ', unsigned ' + opcode.indices[0].name.lower()
   if opcode.dest_components == 0:
      res += ', unsigned num_components'
   if len(opcode.bit_sizes) != 1:
      res += ', unsigned bit_size'
   return res

def sysval_arg_list(opcode):
   args = []
   if opcode.indices:
      args.append(opcode.indices[0].name.lower())
   else:
      args.append('0')

   if opcode.dest_components == 0:
      args.append('num_components')
   else:
      args.append(str(opcode.dest_components))

   if len(opcode.bit_sizes) == 1:
      bit_size = opcode.bit_sizes[0]
      args.append(str(bit_size))
   else:
      args.append('bit_size')

   return ', '.join(args)
%>

% for name, opcode in filter(lambda v: v[1].sysval, sorted(INTR_OPCODES.items())):
<% assert len(opcode.bit_sizes) > 0 %>
static inline nir_ssa_def *
nir_${name}(nir_builder *build${sysval_decl_list(opcode)})
{
   return nir_load_system_value(build, nir_intrinsic_${name},
                                ${sysval_arg_list(opcode)});
}
% endfor

#endif /* _NIR_BUILDER_OPCODES_ */"""

from nir_opcodes import opcodes
from nir_intrinsics import INTR_OPCODES
from mako.template import Template

print(Template(template).render(opcodes=opcodes, INTR_OPCODES=INTR_OPCODES))

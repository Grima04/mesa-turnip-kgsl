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
 *
 */
#include "helpers.h"
#include <stdio.h>
#include <sstream>
#include <llvm-c/Target.h>

using namespace aco;

ac_shader_config config;
radv_shader_info info;
std::unique_ptr<Program> program;
Builder bld(NULL);
Temp inputs[16];
Temp exec_input;
const char *subvariant = "";

void create_program(enum chip_class chip_class, Stage stage, unsigned wave_size, enum radeon_family family)
{
   memset(&config, 0, sizeof(config));
   info.wave_size = wave_size;

   program.reset(new Program);
   aco::init_program(program.get(), stage, &info, chip_class, family, &config);

   Block *block = program->create_and_insert_block();
   block->kind = block_kind_top_level;

   bld = Builder(program.get(), &program->blocks[0]);

   config.float_mode = program->blocks[0].fp_mode.val;
}

bool setup_cs(const char *input_spec, enum chip_class chip_class,
              enum radeon_family family, unsigned wave_size)
{
   const char *old_subvariant = subvariant;
   subvariant = "";
   if (!set_variant(chip_class, old_subvariant))
      return false;

   memset(&info, 0, sizeof(info));
   info.cs.block_size[0] = 1;
   info.cs.block_size[1] = 1;
   info.cs.block_size[2] = 1;

   create_program(chip_class, compute_cs, wave_size, family);

   if (input_spec) {
      unsigned num_inputs = DIV_ROUND_UP(strlen(input_spec), 3u);
      aco_ptr<Instruction> startpgm{create_instruction<Pseudo_instruction>(aco_opcode::p_startpgm, Format::PSEUDO, 0, num_inputs + 1)};
      for (unsigned i = 0; i < num_inputs; i++) {
         RegClass cls(input_spec[i * 3] == 'v' ? RegType::vgpr : RegType::sgpr, input_spec[i * 3 + 1] - '0');
         inputs[i] = bld.tmp(cls);
         startpgm->definitions[i] = Definition(inputs[i]);
      }
      exec_input = bld.tmp(program->lane_mask);
      startpgm->definitions[num_inputs] = bld.exec(Definition(exec_input));
      bld.insert(std::move(startpgm));
   }

   return true;
}

void finish_program(Program *program)
{
   for (Block& BB : program->blocks) {
      for (unsigned idx : BB.linear_preds)
         program->blocks[idx].linear_succs.emplace_back(BB.index);
      for (unsigned idx : BB.logical_preds)
         program->blocks[idx].logical_succs.emplace_back(BB.index);
   }

   for (Block& block : program->blocks) {
      if (block.linear_succs.size() == 0) {
         block.kind |= block_kind_uniform;
         Builder bld(program, &block);
         if (program->wb_smem_l1_on_end)
            bld.smem(aco_opcode::s_dcache_wb, false);
         bld.sopp(aco_opcode::s_endpgm);
      }
   }
}

void finish_validator_test()
{
   finish_program(program.get());
   aco_print_program(program.get(), output);
   fprintf(output, "Validation results:\n");
   if (aco::validate(program.get(), output))
      fprintf(output, "Validation passed\n");
   else
      fprintf(output, "Validation failed\n");
}

void finish_opt_test()
{
   finish_program(program.get());
   if (!aco::validate(program.get(), output)) {
      fail_test("Validation before optimization failed");
      return;
   }
   aco::optimize(program.get());
   if (!aco::validate(program.get(), output)) {
      fail_test("Validation after optimization failed");
      return;
   }
   aco_print_program(program.get(), output);
}

void finish_to_hw_instr_test()
{
   finish_program(program.get());
   aco::lower_to_hw_instr(program.get());
   aco_print_program(program.get(), output);
}

void finish_assembler_test()
{
   finish_program(program.get());
   std::vector<uint32_t> binary;
   unsigned exec_size = emit_program(program.get(), binary);

   /* we could use CLRX for disassembly but that would require it to be
    * installed */
   if (program->chip_class == GFX10_3 && LLVM_VERSION_MAJOR < 9) {
      skip_test("LLVM 11 needed for GFX10_3 disassembly");
   } else if (program->chip_class == GFX10 && LLVM_VERSION_MAJOR < 9) {
      skip_test("LLVM 9 needed for GFX10 disassembly");
   } else if (program->chip_class >= GFX8) {
      std::ostringstream ss;
      print_asm(program.get(), binary, exec_size / 4u, ss);

      fputs(ss.str().c_str(), output);
   } else {
      //TODO: maybe we should use CLRX and skip this test if it's not available?
      for (uint32_t dword : binary)
         fprintf(output, "%.8x\n", dword);
   }
}

void writeout(unsigned i, Temp tmp)
{
   if (tmp.id())
      bld.pseudo(aco_opcode::p_unit_test, Operand(i), tmp);
   else
      bld.pseudo(aco_opcode::p_unit_test, Operand(i));
}


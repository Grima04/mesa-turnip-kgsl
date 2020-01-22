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
#ifndef ACO_TEST_HELPERS_H
#define ACO_TEST_HELPERS_H

#include "framework.h"

extern ac_shader_config config;
extern radv_shader_info info;
extern std::unique_ptr<aco::Program> program;
extern aco::Builder bld;
extern aco::Temp exec_input;
extern aco::Temp inputs[16];
extern const char *subvariant;

void create_program(enum chip_class chip_class, aco::Stage stage,
                    unsigned wave_size=64, enum radeon_family family=CHIP_UNKNOWN);
bool setup_cs(const char *input_spec, enum chip_class chip_class,
              enum radeon_family family=CHIP_UNKNOWN, unsigned wave_size=64);

void finish_program(aco::Program *program);
void finish_validator_test();
void finish_opt_test();
void finish_to_hw_instr_test();
void finish_assembler_test();

void writeout(unsigned i, aco::Temp tmp=aco::Temp(0, aco::s1));

#endif /* ACO_TEST_HELPERS_H */

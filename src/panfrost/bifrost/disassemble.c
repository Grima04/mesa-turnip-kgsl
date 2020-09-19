/*
 * Copyright (C) 2019 Connor Abbott <cwabbott0@gmail.com>
 * Copyright (C) 2019 Lyude Paul <thatslyude@gmail.com>
 * Copyright (C) 2019 Ryan Houdek <Sonicadvance1@gmail.com>
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

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "bifrost.h"
#include "disassemble.h"
#include "bi_print_common.h"
#include "util/macros.h"

// return bits (high, lo]
static uint64_t bits(uint32_t word, unsigned lo, unsigned high)
{
        if (high == 32)
                return word >> lo;
        return (word & ((1 << high) - 1)) >> lo;
}

// each of these structs represents an instruction that's dispatched in one
// cycle. Note that these instructions are packed in funny ways within the
// clause, hence the need for a separate struct.
struct bifrost_alu_inst {
        uint32_t fma_bits;
        uint32_t add_bits;
        uint64_t reg_bits;
};

static unsigned get_reg0(struct bifrost_regs regs)
{
        if (regs.ctrl == 0)
                return regs.reg0 | ((regs.reg1 & 0x1) << 5);

        return regs.reg0 <= regs.reg1 ? regs.reg0 : 63 - regs.reg0;
}

static unsigned get_reg1(struct bifrost_regs regs)
{
        return regs.reg0 <= regs.reg1 ? regs.reg1 : 63 - regs.reg1;
}

// this represents the decoded version of the ctrl register field.
struct bifrost_reg_ctrl {
        bool read_reg0;
        bool read_reg1;
        bool read_reg3;
        enum bifrost_reg_write_unit fma_write_unit;
        enum bifrost_reg_write_unit add_write_unit;
        bool clause_start;
};

static void dump_header(FILE *fp, struct bifrost_header header, bool verbose)
{
        fprintf(fp, "id(%du) ", header.scoreboard_index);

        if (header.clause_type != 0) {
                const char *name = bi_clause_type_name(header.clause_type);

                if (name[0] == '?')
                        fprintf(fp, "unk%u ", header.clause_type);
                else
                        fprintf(fp, "%s ", name);
        }

        if (header.scoreboard_deps != 0) {
                fprintf(fp, "next-wait(");
                bool first = true;
                for (unsigned i = 0; i < 8; i++) {
                        if (header.scoreboard_deps & (1 << i)) {
                                if (!first) {
                                        fprintf(fp, ", ");
                                }
                                fprintf(fp, "%d", i);
                                first = false;
                        }
                }
                fprintf(fp, ") ");
        }

        if (header.datareg_writebarrier)
                fprintf(fp, "data-reg-barrier ");

        if (!header.no_end_of_shader)
                fprintf(fp, "eos ");

        if (!header.back_to_back) {
                fprintf(fp, "nbb ");
                if (header.branch_cond)
                        fprintf(fp, "branch-cond ");
                else
                        fprintf(fp, "branch-uncond ");
        }

        if (header.elide_writes)
                fprintf(fp, "we ");

        if (header.suppress_inf)
                fprintf(fp, "suppress-inf ");
        if (header.suppress_nan)
                fprintf(fp, "suppress-nan ");

        if (header.unk0)
                fprintf(fp, "unk0 ");
        if (header.unk1)
                fprintf(fp, "unk1 ");
        if  (header.unk2)
                fprintf(fp, "unk2 ");
        if (header.unk3)
                fprintf(fp, "unk3 ");
        if (header.unk4)
                fprintf(fp, "unk4 ");

        fprintf(fp, "\n");

        if (verbose) {
                fprintf(fp, "# clause type %d, next clause type %d\n",
                       header.clause_type, header.next_clause_type);
        }
}

static struct bifrost_reg_ctrl DecodeRegCtrl(FILE *fp, struct bifrost_regs regs)
{
        struct bifrost_reg_ctrl decoded = {};
        unsigned ctrl;
        if (regs.ctrl == 0) {
                ctrl = regs.reg1 >> 2;
                decoded.read_reg0 = !(regs.reg1 & 0x2);
                decoded.read_reg1 = false;
        } else {
                ctrl = regs.ctrl;
                decoded.read_reg0 = decoded.read_reg1 = true;
        }
        switch (ctrl) {
        case 1:
                decoded.fma_write_unit = REG_WRITE_TWO;
                break;
        case 2:
        case 3:
                decoded.fma_write_unit = REG_WRITE_TWO;
                decoded.read_reg3 = true;
                break;
        case 4:
                decoded.read_reg3 = true;
                break;
        case 5:
                decoded.add_write_unit = REG_WRITE_TWO;
                break;
        case 6:
                decoded.add_write_unit = REG_WRITE_TWO;
                decoded.read_reg3 = true;
                break;
        case 8:
                decoded.clause_start = true;
                break;
        case 9:
                decoded.fma_write_unit = REG_WRITE_TWO;
                decoded.clause_start = true;
                break;
        case 11:
                break;
        case 12:
                decoded.read_reg3 = true;
                decoded.clause_start = true;
                break;
        case 13:
                decoded.add_write_unit = REG_WRITE_TWO;
                decoded.clause_start = true;
                break;

        case 7:
        case 15:
                decoded.fma_write_unit = REG_WRITE_THREE;
                decoded.add_write_unit = REG_WRITE_TWO;
                break;
        default:
                fprintf(fp, "# unknown reg ctrl %d\n", ctrl);
        }

        return decoded;
}

// Pass in the add_write_unit or fma_write_unit, and this returns which register
// the ADD/FMA units are writing to
static unsigned GetRegToWrite(enum bifrost_reg_write_unit unit, struct bifrost_regs regs)
{
        switch (unit) {
        case REG_WRITE_TWO:
                return regs.reg2;
        case REG_WRITE_THREE:
                return regs.reg3;
        default: /* REG_WRITE_NONE */
                assert(0);
                return 0;
        }
}

static void dump_regs(FILE *fp, struct bifrost_regs srcs)
{
        struct bifrost_reg_ctrl ctrl = DecodeRegCtrl(fp, srcs);
        fprintf(fp, "# ");
        if (ctrl.read_reg0)
                fprintf(fp, "port 0: r%d ", get_reg0(srcs));
        if (ctrl.read_reg1)
                fprintf(fp, "port 1: r%d ", get_reg1(srcs));

        if (ctrl.fma_write_unit == REG_WRITE_TWO)
                fprintf(fp, "port 2: r%d (write FMA) ", srcs.reg2);
        else if (ctrl.add_write_unit == REG_WRITE_TWO)
                fprintf(fp, "port 2: r%d (write ADD) ", srcs.reg2);

        if (ctrl.fma_write_unit == REG_WRITE_THREE)
                fprintf(fp, "port 3: r%d (write FMA) ", srcs.reg3);
        else if (ctrl.add_write_unit == REG_WRITE_THREE)
                fprintf(fp, "port 3: r%d (write ADD) ", srcs.reg3);
        else if (ctrl.read_reg3)
                fprintf(fp, "port 3: r%d (read) ", srcs.reg3);

        if (srcs.uniform_const) {
                if (srcs.uniform_const & 0x80) {
                        fprintf(fp, "uniform: u%d", (srcs.uniform_const & 0x7f) * 2);
                }
        }

        fprintf(fp, "\n");
}

void
bi_disasm_dest_fma(FILE *fp, struct bifrost_regs *next_regs)
{
    struct bifrost_reg_ctrl next_ctrl = DecodeRegCtrl(fp, *next_regs);
    if (next_ctrl.fma_write_unit != REG_WRITE_NONE)
        fprintf(fp, "r%u:t0", GetRegToWrite(next_ctrl.fma_write_unit, *next_regs));
    else
        fprintf(fp, "t0");
}

void
bi_disasm_dest_add(FILE *fp, struct bifrost_regs *next_regs)
{
    struct bifrost_reg_ctrl next_ctrl = DecodeRegCtrl(fp, *next_regs);
    if (next_ctrl.add_write_unit != REG_WRITE_NONE)
        fprintf(fp, "r%u:t1", GetRegToWrite(next_ctrl.add_write_unit, *next_regs));
    else
        fprintf(fp, "t1");
}

static void dump_const_imm(FILE *fp, uint32_t imm)
{
        union {
                float f;
                uint32_t i;
        } fi;
        fi.i = imm;
        fprintf(fp, "0x%08x /* %f */", imm, fi.f);
}

/* Convert an index to an embedded constant in FAU-RAM to the index of the
 * embedded constant. No, it's not in order. Yes, really. */

static unsigned
const_fau_to_idx(unsigned fau_value)
{
        unsigned map[8] = {
                ~0, ~0, 4, 5, 0, 1, 2, 3
        };

        assert(map[fau_value] < 6);
        return map[fau_value];
}

static void dump_uniform_const_src(FILE *fp, struct bifrost_regs srcs, struct bi_constants *consts, bool high32)
{
        if (srcs.uniform_const & 0x80) {
                unsigned uniform = (srcs.uniform_const & 0x7f);
                fprintf(fp, "u%d.w%d", uniform, high32);
        } else if (srcs.uniform_const >= 0x20) {
                uint64_t imm = consts->raw[const_fau_to_idx(srcs.uniform_const >> 4)];
                imm |= (srcs.uniform_const & 0xf);

                if (high32)
                        dump_const_imm(fp, imm >> 32);
                else
                        dump_const_imm(fp, imm);
        } else {
                switch (srcs.uniform_const) {
                case 0:
                        fprintf(fp, "#0");
                        break;
                case 1:
                        fprintf(fp, "lane_id");
                        break;
                case 2:
                        fprintf(fp, "warp_id");
                        break;
                case 3:
                        fprintf(fp, "core_id");
                        break;
                case 4:
                        fprintf(fp, "framebuffer_size");
                        break;
                case 5:
                        fprintf(fp, "atest_datum");
                        break;
                case 6:
                        fprintf(fp, "sample");
                        break;
                case 8:
                case 9:
                case 10:
                case 11:
                case 12:
                case 13:
                case 14:
                case 15:
                        fprintf(fp, "blend_descriptor_%u", (unsigned) srcs.uniform_const - 8);
                        break;
                default:
                        fprintf(fp, "XXX - reserved%u", (unsigned) srcs.uniform_const);
                        break;
                }

                if (high32)
                        fprintf(fp, ".y");
                else
                        fprintf(fp, ".x");
        }
}

void
dump_src(FILE *fp, unsigned src, struct bifrost_regs srcs, struct bi_constants *consts, bool isFMA)
{
        switch (src) {
        case 0:
                fprintf(fp, "r%d", get_reg0(srcs));
                break;
        case 1:
                fprintf(fp, "r%d", get_reg1(srcs));
                break;
        case 2:
                fprintf(fp, "r%d", srcs.reg3);
                break;
        case 3:
                if (isFMA)
                        fprintf(fp, "#0");
                else
                        fprintf(fp, "t"); // i.e. the output of FMA this cycle
                break;
        case 4:
                dump_uniform_const_src(fp, srcs, consts, false);
                break;
        case 5:
                dump_uniform_const_src(fp, srcs, consts, true);
                break;
        case 6:
                fprintf(fp, "t0");
                break;
        case 7:
                fprintf(fp, "t1");
                break;
        }
}

static bool dump_clause(FILE *fp, uint32_t *words, unsigned *size, unsigned offset, bool verbose)
{
        // State for a decoded clause
        struct bifrost_alu_inst instrs[8] = {};
        struct bi_constants consts = {};
        unsigned num_instrs = 0;
        unsigned num_consts = 0;
        uint64_t header_bits = 0;
        bool stopbit = false;

        unsigned i;
        for (i = 0; ; i++, words += 4) {
                if (verbose) {
                        fprintf(fp, "# ");
                        for (int j = 0; j < 4; j++)
                                fprintf(fp, "%08x ", words[3 - j]); // low bit on the right
                        fprintf(fp, "\n");
                }
                unsigned tag = bits(words[0], 0, 8);

                // speculatively decode some things that are common between many formats, so we can share some code
                struct bifrost_alu_inst main_instr = {};
                // 20 bits
                main_instr.add_bits = bits(words[2], 2, 32 - 13);
                // 23 bits
                main_instr.fma_bits = bits(words[1], 11, 32) | bits(words[2], 0, 2) << (32 - 11);
                // 35 bits
                main_instr.reg_bits = ((uint64_t) bits(words[1], 0, 11)) << 24 | (uint64_t) bits(words[0], 8, 32);

                uint64_t const0 = bits(words[0], 8, 32) << 4 | (uint64_t) words[1] << 28 | bits(words[2], 0, 4) << 60;
                uint64_t const1 = bits(words[2], 4, 32) << 4 | (uint64_t) words[3] << 32;

                /* Z-bit */
                bool stop = tag & 0x40;

                if (verbose) {
                        fprintf(fp, "# tag: 0x%02x\n", tag);
                }
                if (tag & 0x80) {
                        /* Format 5 or 10 */
                        unsigned idx = stop ? 5 : 2;
                        main_instr.add_bits |= ((tag >> 3) & 0x7) << 17;
                        instrs[idx + 1] = main_instr;
                        instrs[idx].add_bits = bits(words[3], 0, 17) | ((tag & 0x7) << 17);
                        instrs[idx].fma_bits |= bits(words[2], 19, 32) << 10;
                        consts.raw[0] = bits(words[3], 17, 32) << 4;
                } else {
                        bool done = false;
                        switch ((tag >> 3) & 0x7) {
                        case 0x0:
                                switch (tag & 0x7) {
                                case 0x3:
                                        /* Format 1 */
                                        main_instr.add_bits |= bits(words[3], 29, 32) << 17;
                                        instrs[1] = main_instr;
                                        num_instrs = 2;
                                        done = stop;
                                        break;
                                case 0x4:
                                        /* Format 3 */
                                        instrs[2].add_bits = bits(words[3], 0, 17) | bits(words[3], 29, 32) << 17;
                                        instrs[2].fma_bits |= bits(words[2], 19, 32) << 10;
                                        consts.raw[0] = const0;
                                        num_instrs = 3;
                                        num_consts = 1;
                                        done = stop;
                                        break;
                                case 0x1:
                                case 0x5:
                                        /* Format 4 */
                                        instrs[2].add_bits = bits(words[3], 0, 17) | bits(words[3], 29, 32) << 17;
                                        instrs[2].fma_bits |= bits(words[2], 19, 32) << 10;
                                        main_instr.add_bits |= bits(words[3], 26, 29) << 17;
                                        instrs[3] = main_instr;
                                        if ((tag & 0x7) == 0x5) {
                                                num_instrs = 4;
                                                done = stop;
                                        }
                                        break;
                                case 0x6:
                                        /* Format 8 */
                                        instrs[5].add_bits = bits(words[3], 0, 17) | bits(words[3], 29, 32) << 17;
                                        instrs[5].fma_bits |= bits(words[2], 19, 32) << 10;
                                        consts.raw[0] = const0;
                                        num_instrs = 6;
                                        num_consts = 1;
                                        done = stop;
                                        break;
                                case 0x7:
                                        /* Format 9 */
                                        instrs[5].add_bits = bits(words[3], 0, 17) | bits(words[3], 29, 32) << 17;
                                        instrs[5].fma_bits |= bits(words[2], 19, 32) << 10;
                                        main_instr.add_bits |= bits(words[3], 26, 29) << 17;
                                        instrs[6] = main_instr;
                                        num_instrs = 7;
                                        done = stop;
                                        break;
                                default:
                                        unreachable("[INSTR_INVALID_ENC] Invalid tag bits");
                                }
                                break;
                        case 0x2:
                        case 0x3: {
                                /* Format 6 or 11 */
                                unsigned idx = ((tag >> 3) & 0x7) == 2 ? 4 : 7;
                                main_instr.add_bits |= (tag & 0x7) << 17;
                                instrs[idx] = main_instr;
                                consts.raw[0] |= (bits(words[2], 19, 32) | ((uint64_t) words[3] << 13)) << 19;
                                num_consts = 1;
                                num_instrs = idx + 1;
                                done = stop;
                                break;
                        }
                        case 0x4: {
                                /* Format 2 */
                                unsigned idx = stop ? 4 : 1;
                                main_instr.add_bits |= (tag & 0x7) << 17;
                                instrs[idx] = main_instr;
                                instrs[idx + 1].fma_bits |= bits(words[3], 22, 32);
                                instrs[idx + 1].reg_bits = bits(words[2], 19, 32) | (bits(words[3], 0, 22) << (32 - 19));
                                break;
                        }
                        case 0x1:
                                /* Format 0 - followed by constants */
                                num_instrs = 1;
                                done = stop;
                                /* fallthrough */
                        case 0x5:
                                /* Format 0 - followed by instructions */
                                header_bits = bits(words[2], 19, 32) | ((uint64_t) words[3] << (32 - 19));
                                main_instr.add_bits |= (tag & 0x7) << 17;
                                instrs[0] = main_instr;
                                break;
                        case 0x6:
                        case 0x7: {
                                /* Format 12 */
                                unsigned pos = tag & 0xf;
                                // note that `pos' encodes both the total number of
                                // instructions and the position in the constant stream,
                                // presumably because decoded constants and instructions
                                // share a buffer in the decoder, but we only care about
                                // the position in the constant stream; the total number of
                                // instructions is redundant.
                                unsigned const_idx = 0;
                                switch (pos) {
                                case 0:
                                case 1:
                                case 2:
                                case 6:
                                        const_idx = 0;
                                        break;
                                case 3:
                                case 4:
                                case 7:
                                case 9:
                                        const_idx = 1;
                                        break;
                                case 5:
                                case 0xa:
                                        const_idx = 2;
                                        break;
                                case 8:
                                case 0xb:
                                case 0xc:
                                        const_idx = 3;
                                        break;
                                case 0xd:
                                        const_idx = 4;
                                        break;
                                case 0xe:
                                        const_idx = 5;
                                        break;
                                default:
                                        fprintf(fp, "# unknown pos 0x%x\n", pos);
                                        break;
                                }

                                if (num_consts < const_idx + 2)
                                        num_consts = const_idx + 2;

                                consts.raw[const_idx] = const0;
                                consts.raw[const_idx + 1] = const1;
                                done = stop;
                                break;
                        }
                        default:
                                break;
                        }

                        if (done)
                                break;
                }
        }

        *size = i + 1;

        if (verbose) {
                fprintf(fp, "# header: %012" PRIx64 "\n", header_bits);
        }

        struct bifrost_header header;
        memcpy((char *) &header, (char *) &header_bits, sizeof(struct bifrost_header));
        dump_header(fp, header, verbose);
        if (!header.no_end_of_shader)
                stopbit = true;

        fprintf(fp, "{\n");
        for (i = 0; i < num_instrs; i++) {
                struct bifrost_regs regs, next_regs;
                if (i + 1 == num_instrs) {
                        memcpy((char *) &next_regs, (char *) &instrs[0].reg_bits,
                               sizeof(next_regs));
                } else {
                        memcpy((char *) &next_regs, (char *) &instrs[i + 1].reg_bits,
                               sizeof(next_regs));
                }

                memcpy((char *) &regs, (char *) &instrs[i].reg_bits, sizeof(regs));

                if (verbose) {
                        fprintf(fp, "# regs: %016" PRIx64 "\n", instrs->reg_bits);
                        dump_regs(fp, regs);
                }

                bi_disasm_fma(fp, instrs[i].fma_bits, &regs, &next_regs, header.datareg, offset, &consts);
                bi_disasm_add(fp, instrs[i].add_bits, &regs, &next_regs, header.datareg, offset, &consts);
        }
        fprintf(fp, "}\n");

        if (verbose) {
                for (unsigned i = 0; i < num_consts; i++) {
                        fprintf(fp, "# const%d: %08" PRIx64 "\n", 2 * i, consts.raw[i] & 0xffffffff);
                        fprintf(fp, "# const%d: %08" PRIx64 "\n", 2 * i + 1, consts.raw[i] >> 32);
                }
        }
        return stopbit;
}

void disassemble_bifrost(FILE *fp, uint8_t *code, size_t size, bool verbose)
{
        uint32_t *words = (uint32_t *) code;
        uint32_t *words_end = words + (size / 4);
        // used for displaying branch targets
        unsigned offset = 0;
        while (words != words_end) {
                // we don't know what the program-end bit is quite yet, so for now just
                // assume that an all-0 quadword is padding
                uint32_t zero[4] = {};
                if (memcmp(words, zero, 4 * sizeof(uint32_t)) == 0)
                        break;
                fprintf(fp, "clause_%d:\n", offset);
                unsigned size;
                if (dump_clause(fp, words, &size, offset, verbose) == true) {
                        break;
                }
                words += size * 4;
                offset += size;
        }
}


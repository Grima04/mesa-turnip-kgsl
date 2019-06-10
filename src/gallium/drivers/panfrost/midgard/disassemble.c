/* Author(s):
 *   Connor Abbott
 *   Alyssa Rosenzweig
 *
 * Copyright (c) 2013 Connor Abbott (connor@abbott.cx)
 * Copyright (c) 2018 Alyssa Rosenzweig (alyssa@rosenzweig.io)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include "midgard.h"
#include "midgard-parse.h"
#include "midgard_ops.h"
#include "disassemble.h"
#include "helpers.h"
#include "util/half_float.h"

#define DEFINE_CASE(define, str) case define: { printf(str); break; }

static bool is_instruction_int = false;

/* Prints a short form of the tag for branching, the minimum needed to be
 * legible and unambiguous */

static void
print_tag_short(unsigned tag)
{
        switch (midgard_word_types[tag]) {
                case midgard_word_type_texture:
                        printf("tex/%X", tag);
                        break;

                case midgard_word_type_load_store:
                        printf("ldst");
                        break;

                case midgard_word_type_alu:
                        printf("alu%d/%X", midgard_word_size[tag], tag);
                        break;

                default:
                        printf("%s%X", (tag > 0) ? "" : "unk", tag);
                        break;
        }
}

static void
print_alu_opcode(midgard_alu_op op)
{
        bool int_op = false;

        if (alu_opcode_props[op].name) {
                printf("%s", alu_opcode_props[op].name);

                int_op = midgard_is_integer_op(op);
        } else
                printf("alu_op_%02X", op);

        /* For constant analysis */
        is_instruction_int = int_op;
}

static void
print_ld_st_opcode(midgard_load_store_op op)
{
        if (load_store_opcode_names[op])
                printf("%s", load_store_opcode_names[op]);
        else
                printf("ldst_op_%02X", op);
}

static bool is_embedded_constant_half = false;
static bool is_embedded_constant_int = false;

static char
prefix_for_bits(unsigned bits)
{
        switch (bits) {
                case 8:
                        return 'q';
                case 16:
                        return 'h';
                case 64:
                        return 'd';
                default:
                        return 0;
        }
}

static void
print_reg(unsigned reg, unsigned bits)
{
        /* Perform basic static analysis for expanding constants correctly */

        if (reg == 26) {
                is_embedded_constant_int = is_instruction_int;
                is_embedded_constant_half = (bits < 32);
        }

        char prefix = prefix_for_bits(bits);

        if (prefix)
                putchar(prefix);

        printf("r%u", reg);
}

static char *outmod_names_float[4] = {
        "",
        ".pos",
        ".unk2",
        ".sat"
};

static char *outmod_names_int[4] = {
        ".isat",
        ".usat",
        "",
        ".hi"
};

static char *srcmod_names_int[4] = {
        "sext(",
        "zext(",
        "",
        "("
};

static void
print_outmod(unsigned outmod, bool is_int)
{
        printf("%s", is_int ? outmod_names_int[outmod] :
                              outmod_names_float[outmod]);
}

static void
print_quad_word(uint32_t *words, unsigned tabs)
{
        unsigned i;

        for (i = 0; i < 4; i++)
                printf("0x%08X%s ", words[i], i == 3 ? "" : ",");

        printf("\n");
}

static const char components[16] = "xyzwefghijklmnop";

/* Helper to print 4 chars of a swizzle */
static void
print_swizzle_helper(unsigned swizzle, bool upper)
{
        for (unsigned i = 0; i < 4; ++i) {
                unsigned c = (swizzle >> (i * 2)) & 3;
                c += upper*4;
                printf("%c", components[c]);
        }
}

/* Helper to print 8 chars of a swizzle, duplicating over */
static void
print_swizzle_helper_8(unsigned swizzle, bool upper)
{
        for (unsigned i = 0; i < 4; ++i) {
                unsigned c = (swizzle >> (i * 2)) & 3;
                c *= 2;
                c += upper*8;
                printf("%c%c", components[c], components[c+1]);
        }
}

static void
print_swizzle_vec16(unsigned swizzle, bool rep_high, bool rep_low,
                midgard_dest_override override)
{
        printf(".");

        if (override == midgard_dest_override_upper) {
                if (rep_high)
                        printf(" /* rep_high */ ");
                if (rep_low)
                        printf(" /* rep_low */ ");

                if (!rep_high && rep_low)
                        print_swizzle_helper_8(swizzle, true);
                else
                        print_swizzle_helper_8(swizzle, false);
        } else {
                print_swizzle_helper_8(swizzle, rep_high & 1);
                print_swizzle_helper_8(swizzle, !rep_low & 1);
        }
}

static void
print_swizzle_vec8(unsigned swizzle, bool rep_high, bool rep_low)
{
        printf(".");

        print_swizzle_helper(swizzle, rep_high & 1);
        print_swizzle_helper(swizzle, !rep_low & 1);
}

static void
print_swizzle_vec4(unsigned swizzle, bool rep_high, bool rep_low)
{
        if (rep_high)
                printf(" /* rep_high */ ");
        if (rep_low)
                printf(" /* rep_low */ ");

        if (swizzle == 0xE4) return; /* xyzw */

        printf(".");
        print_swizzle_helper(swizzle, 0);
}
static void
print_swizzle_vec2(unsigned swizzle, bool rep_high, bool rep_low)
{
        if (rep_high)
                printf(" /* rep_high */ ");
        if (rep_low)
                printf(" /* rep_low */ ");

        if (swizzle == 0xE4) return; /* XY */

        printf(".");

        for (unsigned i = 0; i < 4; i += 2) {
                unsigned a = (swizzle >> (i * 2)) & 3;
                unsigned b = (swizzle >> ((i+1) * 2)) & 3;

                /* Normally we're adjacent, but if there's an issue, don't make
                 * it ambiguous */

                if (a & 0x1)
                        printf("[%c%c]", components[a], components[b]);
                else if (a == b)
                        printf("%c", components[a >> 1]);
                else if (b == (a + 1))
                        printf("%c", "XY"[a >> 1]);
                else
                        printf("[%c%c]", components[a], components[b]);
        }
}

static int
bits_for_mode(midgard_reg_mode mode)
{
        switch (mode) {
                case midgard_reg_mode_8:
                        return 8;
                case midgard_reg_mode_16:
                        return 16;
                case midgard_reg_mode_32:
                        return 32;
                case midgard_reg_mode_64:
                        return 64;
                default:
                        return 0;
        }
}

static int
bits_for_mode_halved(midgard_reg_mode mode, bool half)
{
        unsigned bits = bits_for_mode(mode);

        if (half)
                bits >>= 1;

        return bits;
}

static void
print_vector_src(unsigned src_binary,
                 midgard_reg_mode mode, unsigned reg,
                 midgard_dest_override override, bool is_int)
{
        midgard_vector_alu_src *src = (midgard_vector_alu_src *)&src_binary;

        /* Modifiers change meaning depending on the op's context */

        midgard_int_mod int_mod = src->mod;

        if (is_int) {
                printf("%s", srcmod_names_int[int_mod]);
        } else {
                if (src->mod & MIDGARD_FLOAT_MOD_NEG)
                        printf("-");

                if (src->mod & MIDGARD_FLOAT_MOD_ABS)
                        printf("abs(");
        }

        //register
        unsigned bits = bits_for_mode_halved(mode, src->half);
        print_reg(reg, bits);

        //swizzle
        if (bits == 16)
                print_swizzle_vec8(src->swizzle, src->rep_high, src->rep_low);
        else if (bits == 8)
                print_swizzle_vec16(src->swizzle, src->rep_high, src->rep_low, override);
        else if (bits == 32)
                print_swizzle_vec4(src->swizzle, src->rep_high, src->rep_low);
        else if (bits == 64)
                print_swizzle_vec2(src->swizzle, src->rep_high, src->rep_low);

        /* Since we wrapped with a function-looking thing */

        if (is_int && int_mod == midgard_int_shift)
                printf(") << %d", bits);
        else if ((is_int && (int_mod != midgard_int_normal))
                        || (!is_int && src->mod & MIDGARD_FLOAT_MOD_ABS))
                printf(")");
}

static uint16_t
decode_vector_imm(unsigned src2_reg, unsigned imm)
{
        uint16_t ret;
        ret = src2_reg << 11;
        ret |= (imm & 0x7) << 8;
        ret |= (imm >> 3) & 0xFF;
        return ret;
}

static void
print_immediate(uint16_t imm)
{
        if (is_instruction_int)
                printf("#%d", imm);
        else
                printf("#%g", _mesa_half_to_float(imm));
}

static unsigned
print_dest(unsigned reg, midgard_reg_mode mode, midgard_dest_override override)
{
        /* Depending on the mode and override, we determine the type of
         * destination addressed. Absent an override, we address just the
         * type of the operation itself */

        unsigned bits = bits_for_mode(mode);

        if (override != midgard_dest_override_none)
                bits /= 2;

        print_reg(reg, bits);

        return bits;
}

static void
print_mask_vec16(uint8_t mask, midgard_dest_override override)
{
        printf(".");

        if (override == midgard_dest_override_none) {
                for (unsigned i = 0; i < 8; i++) {
                        if (mask & (1 << i))
                                printf("%c%c",
                                        components[i*2 + 0],
                                        components[i*2 + 1]);
                }
        } else {
                bool upper = (override == midgard_dest_override_upper);

                for (unsigned i = 0; i < 8; i++) {
                        if (mask & (1 << i))
                                printf("%c", components[i + (upper ? 8 : 0)]);
                }
        }
}

/* For 16-bit+ masks, we read off from the 8-bit mask field. For 16-bit (vec8),
 * it's just one bit per channel, easy peasy. For 32-bit (vec4), it's one bit
 * per channel with one duplicate bit in the middle. For 64-bit (vec2), it's
 * one-bit per channel with _3_ duplicate bits in the middle. Basically, just
 * subdividing the 128-bit word in 16-bit increments. For 64-bit, we uppercase
 * the mask to make it obvious what happened */

static void
print_mask(uint8_t mask, unsigned bits, midgard_dest_override override)
{
        if (bits == 8) {
                print_mask_vec16(mask, override);
                return;
        }

        /* Skip 'complete' masks */

        if (bits >= 32 && mask == 0xFF) return;

        if (bits == 16) {
                if (mask == 0x0F)
                        return;
                else if (mask == 0xF0) {
                        printf("'");
                        return;
                }
        }

        printf(".");

        unsigned skip = (bits / 16);
        bool uppercase = bits > 32;
        bool tripped = false;

        for (unsigned i = 0; i < 8; i += skip) {
                bool a = (mask & (1 << i)) != 0;

                for (unsigned j = 1; j < skip; ++j) {
                        bool dupe = (mask & (1 << (i + j))) != 0;
                        tripped |= (dupe != a);
                }

                if (a) {
                        char c = components[i / skip];

                        if (uppercase)
                                c = toupper(c);

                        printf("%c", c);
                }
        }

        if (tripped)
                printf(" /* %X */", mask);
}

static void
print_mask_4(unsigned mask)
{
        printf(".");

        for (unsigned i = 0; i < 4; ++i) {
                bool a = (mask & (1 << i)) != 0;
                if (a)
                        printf("%c", components[i]);
        }
}

static void
print_vector_field(const char *name, uint16_t *words, uint16_t reg_word,
                   unsigned tabs)
{
        midgard_reg_info *reg_info = (midgard_reg_info *)&reg_word;
        midgard_vector_alu *alu_field = (midgard_vector_alu *) words;
        midgard_reg_mode mode = alu_field->reg_mode;
        unsigned override = alu_field->dest_override;

        /* For now, prefix instruction names with their unit, until we
         * understand how this works on a deeper level */
        printf("%s.", name);

        print_alu_opcode(alu_field->op);

        /* Postfix with the size to disambiguate if necessary */
        char postfix = prefix_for_bits(bits_for_mode(mode));
        bool size_ambiguous = override != midgard_dest_override_none;

        if (size_ambiguous)
                printf("%c", postfix ? postfix : 'r');

        /* Print the outmod, if there is one */
        print_outmod(alu_field->outmod,
                midgard_is_integer_out_op(alu_field->op));

        printf(" ");

        /* Mask denoting status of 8-lanes */
        uint8_t mask = alu_field->mask;

        /* First, print the destination */
        unsigned dest_size =
                print_dest(reg_info->out_reg, mode, alu_field->dest_override);

        /* Apply the destination override to the mask */

        if (mode == midgard_reg_mode_32 || mode == midgard_reg_mode_64) {
                if (override == midgard_dest_override_lower)
                        mask &= 0x0F;
                else if (override == midgard_dest_override_upper)
                        mask &= 0xF0;
        } else if (mode == midgard_reg_mode_16
                        && override == midgard_dest_override_lower) {
                /* stub */
        }

        if (override != midgard_dest_override_none) {
                bool modeable = (mode != midgard_reg_mode_8);
                bool known = override != 0x3; /* Unused value */

                if (!(modeable && known))
                        printf("/* do%d */ ", override);
        }

        print_mask(mask, dest_size, override);

        printf(", ");

        bool is_int = midgard_is_integer_op(alu_field->op);
        print_vector_src(alu_field->src1, mode, reg_info->src1_reg, override, is_int);

        printf(", ");

        if (reg_info->src2_imm) {
                uint16_t imm = decode_vector_imm(reg_info->src2_reg, alu_field->src2 >> 2);
                print_immediate(imm);
        } else {
                print_vector_src(alu_field->src2, mode,
                                 reg_info->src2_reg, override, is_int);
        }

        printf("\n");
}

static void
print_scalar_src(unsigned src_binary, unsigned reg)
{
        midgard_scalar_alu_src *src = (midgard_scalar_alu_src *)&src_binary;

        if (src->negate)
                printf("-");

        if (src->abs)
                printf("abs(");

        print_reg(reg, src->full ? 32 : 16);

        unsigned c = src->component;

        if (src->full) {
                assert((c & 1) == 0);
                c >>= 1;
        }

        printf(".%c", components[c]);

        if (src->abs)
                printf(")");

}

static uint16_t
decode_scalar_imm(unsigned src2_reg, unsigned imm)
{
        uint16_t ret;
        ret = src2_reg << 11;
        ret |= (imm & 3) << 9;
        ret |= (imm & 4) << 6;
        ret |= (imm & 0x38) << 2;
        ret |= imm >> 6;
        return ret;
}

static void
print_scalar_field(const char *name, uint16_t *words, uint16_t reg_word,
                   unsigned tabs)
{
        midgard_reg_info *reg_info = (midgard_reg_info *)&reg_word;
        midgard_scalar_alu *alu_field = (midgard_scalar_alu *) words;

        if (alu_field->unknown)
                printf("scalar ALU unknown bit set\n");

        printf("%s.", name);
        print_alu_opcode(alu_field->op);
        print_outmod(alu_field->outmod,
                        midgard_is_integer_out_op(alu_field->op));
        printf(" ");

        bool full = alu_field->output_full;
        print_reg(reg_info->out_reg, full ? 32 : 16);
        unsigned c = alu_field->output_component;

        if (full) {
                assert((c & 1) == 0);
                c >>= 1;
        }

        printf(".%c, ", components[c]);

        print_scalar_src(alu_field->src1, reg_info->src1_reg);

        printf(", ");

        if (reg_info->src2_imm) {
                uint16_t imm = decode_scalar_imm(reg_info->src2_reg,
                                                 alu_field->src2);
                print_immediate(imm);
        } else
                print_scalar_src(alu_field->src2, reg_info->src2_reg);

        printf("\n");
}

static void
print_branch_op(int op)
{
        switch (op) {
        case midgard_jmp_writeout_op_branch_uncond:
                printf("uncond.");
                break;

        case midgard_jmp_writeout_op_branch_cond:
                printf("cond.");
                break;

        case midgard_jmp_writeout_op_writeout:
                printf("write.");
                break;

        case midgard_jmp_writeout_op_tilebuffer_pending:
                printf("tilebuffer.");
                break;

        case midgard_jmp_writeout_op_discard:
                printf("discard.");
                break;

        default:
                printf("unk%d.", op);
                break;
        }
}

static void
print_branch_cond(int cond)
{
        switch (cond) {
        case midgard_condition_write0:
                printf("write0");
                break;

        case midgard_condition_false:
                printf("false");
                break;

        case midgard_condition_true:
                printf("true");
                break;

        case midgard_condition_always:
                printf("always");
                break;

        default:
                printf("unk%X", cond);
                break;
        }
}

static void
print_compact_branch_writeout_field(uint16_t word)
{
        midgard_jmp_writeout_op op = word & 0x7;

        switch (op) {
        case midgard_jmp_writeout_op_branch_uncond: {
                midgard_branch_uncond br_uncond;
                memcpy((char *) &br_uncond, (char *) &word, sizeof(br_uncond));
                printf("br.uncond ");

                if (br_uncond.unknown != 1)
                        printf("unknown:%d, ", br_uncond.unknown);

                if (br_uncond.offset >= 0)
                        printf("+");

                printf("%d -> ", br_uncond.offset);
                print_tag_short(br_uncond.dest_tag);
                printf("\n");

                break;
        }

        case midgard_jmp_writeout_op_branch_cond:
        case midgard_jmp_writeout_op_writeout:
        case midgard_jmp_writeout_op_discard:
        default: {
                midgard_branch_cond br_cond;
                memcpy((char *) &br_cond, (char *) &word, sizeof(br_cond));

                printf("br.");

                print_branch_op(br_cond.op);
                print_branch_cond(br_cond.cond);

                printf(" ");

                if (br_cond.offset >= 0)
                        printf("+");

                printf("%d -> ", br_cond.offset);
                print_tag_short(br_cond.dest_tag);
                printf("\n");

                break;
        }
        }
}

static void
print_extended_branch_writeout_field(uint8_t *words)
{
        midgard_branch_extended br;
        memcpy((char *) &br, (char *) words, sizeof(br));

        printf("brx.");

        print_branch_op(br.op);

        /* Condition repeated 8 times in all known cases. Check this. */

        unsigned cond = br.cond & 0x3;

        for (unsigned i = 0; i < 16; i += 2) {
                assert(((br.cond >> i) & 0x3) == cond);
        }

        print_branch_cond(cond);

        if (br.unknown)
                printf(".unknown%d", br.unknown);

        printf(" ");

        if (br.offset >= 0)
                printf("+");

        printf("%d -> ", br.offset);
        print_tag_short(br.dest_tag);
        printf("\n");
}

static unsigned
num_alu_fields_enabled(uint32_t control_word)
{
        unsigned ret = 0;

        if ((control_word >> 17) & 1)
                ret++;

        if ((control_word >> 19) & 1)
                ret++;

        if ((control_word >> 21) & 1)
                ret++;

        if ((control_word >> 23) & 1)
                ret++;

        if ((control_word >> 25) & 1)
                ret++;

        return ret;
}

static float
float_bitcast(uint32_t integer)
{
        union {
                uint32_t i;
                float f;
        } v;

        v.i = integer;
        return v.f;
}

static void
print_alu_word(uint32_t *words, unsigned num_quad_words,
               unsigned tabs)
{
        uint32_t control_word = words[0];
        uint16_t *beginning_ptr = (uint16_t *)(words + 1);
        unsigned num_fields = num_alu_fields_enabled(control_word);
        uint16_t *word_ptr = beginning_ptr + num_fields;
        unsigned num_words = 2 + num_fields;

        if ((control_word >> 16) & 1)
                printf("unknown bit 16 enabled\n");

        if ((control_word >> 17) & 1) {
                print_vector_field("vmul", word_ptr, *beginning_ptr, tabs);
                beginning_ptr += 1;
                word_ptr += 3;
                num_words += 3;
        }

        if ((control_word >> 18) & 1)
                printf("unknown bit 18 enabled\n");

        if ((control_word >> 19) & 1) {
                print_scalar_field("sadd", word_ptr, *beginning_ptr, tabs);
                beginning_ptr += 1;
                word_ptr += 2;
                num_words += 2;
        }

        if ((control_word >> 20) & 1)
                printf("unknown bit 20 enabled\n");

        if ((control_word >> 21) & 1) {
                print_vector_field("vadd", word_ptr, *beginning_ptr, tabs);
                beginning_ptr += 1;
                word_ptr += 3;
                num_words += 3;
        }

        if ((control_word >> 22) & 1)
                printf("unknown bit 22 enabled\n");

        if ((control_word >> 23) & 1) {
                print_scalar_field("smul", word_ptr, *beginning_ptr, tabs);
                beginning_ptr += 1;
                word_ptr += 2;
                num_words += 2;
        }

        if ((control_word >> 24) & 1)
                printf("unknown bit 24 enabled\n");

        if ((control_word >> 25) & 1) {
                print_vector_field("lut", word_ptr, *beginning_ptr, tabs);
                beginning_ptr += 1;
                word_ptr += 3;
                num_words += 3;
        }

        if ((control_word >> 26) & 1) {
                print_compact_branch_writeout_field(*word_ptr);
                word_ptr += 1;
                num_words += 1;
        }

        if ((control_word >> 27) & 1) {
                print_extended_branch_writeout_field((uint8_t *) word_ptr);
                word_ptr += 3;
                num_words += 3;
        }

        if (num_quad_words > (num_words + 7) / 8) {
                assert(num_quad_words == (num_words + 15) / 8);
                //Assume that the extra quadword is constants
                void *consts = words + (4 * num_quad_words - 4);

                if (is_embedded_constant_int) {
                        if (is_embedded_constant_half) {
                                int16_t *sconsts = (int16_t *) consts;
                                printf("sconstants %d, %d, %d, %d\n",
                                       sconsts[0],
                                       sconsts[1],
                                       sconsts[2],
                                       sconsts[3]);
                        } else {
                                int32_t *iconsts = (int32_t *) consts;
                                printf("iconstants %d, %d, %d, %d\n",
                                       iconsts[0],
                                       iconsts[1],
                                       iconsts[2],
                                       iconsts[3]);
                        }
                } else {
                        if (is_embedded_constant_half) {
                                uint16_t *hconsts = (uint16_t *) consts;
                                printf("hconstants %g, %g, %g, %g\n",
                                       _mesa_half_to_float(hconsts[0]),
                                       _mesa_half_to_float(hconsts[1]),
                                       _mesa_half_to_float(hconsts[2]),
                                       _mesa_half_to_float(hconsts[3]));
                        } else {
                                uint32_t *fconsts = (uint32_t *) consts;
                                printf("fconstants %g, %g, %g, %g\n",
                                       float_bitcast(fconsts[0]),
                                       float_bitcast(fconsts[1]),
                                       float_bitcast(fconsts[2]),
                                       float_bitcast(fconsts[3]));
                        }

                }
        }
}

static void
print_varying_parameters(midgard_load_store_word *word)
{
        midgard_varying_parameter param;
        unsigned v = word->varying_parameters;
        memcpy(&param, &v, sizeof(param));

        if (param.is_varying) {
                /* If a varying, there are qualifiers */
                if (param.flat)
                        printf(".flat");

                if (param.interpolation != midgard_interp_default) {
                        if (param.interpolation == midgard_interp_centroid)
                                printf(".centroid");
                        else
                                printf(".interp%d", param.interpolation);
                }
        } else if (param.flat || param.interpolation) {
                printf(" /* is_varying not set but varying metadata attached */");
        }

        if (param.zero1 || param.zero2)
                printf(" /* zero tripped, %d %d */ ", param.zero1, param.zero2);
}

static bool
is_op_varying(unsigned op)
{
        switch (op) {
        case midgard_op_st_vary_16:
        case midgard_op_st_vary_32:
        case midgard_op_ld_vary_16:
        case midgard_op_ld_vary_32:
                return true;
        }

        return false;
}

static void
print_load_store_instr(uint64_t data,
                       unsigned tabs)
{
        midgard_load_store_word *word = (midgard_load_store_word *) &data;

        print_ld_st_opcode(word->op);

        if (is_op_varying(word->op))
                print_varying_parameters(word);

        printf(" r%d", word->reg);
        print_mask_4(word->mask);

        int address = word->address;

        if (word->op == midgard_op_ld_uniform_32) {
                /* Uniforms use their own addressing scheme */

                int lo = word->varying_parameters >> 7;
                int hi = word->address;

                /* TODO: Combine fields logically */
                address = (hi << 3) | lo;
        }

        printf(", %d", address);

        print_swizzle_vec4(word->swizzle, false, false);

        printf(", 0x%X /* %X */\n", word->unknown, word->varying_parameters);
}

static void
print_load_store_word(uint32_t *word, unsigned tabs)
{
        midgard_load_store *load_store = (midgard_load_store *) word;

        if (load_store->word1 != 3) {
                print_load_store_instr(load_store->word1, tabs);
        }

        if (load_store->word2 != 3) {
                print_load_store_instr(load_store->word2, tabs);
        }
}

static void
print_texture_reg(bool full, bool select, bool upper)
{
        if (full)
                printf("r%d", REG_TEX_BASE + select);
        else
                printf("hr%d", (REG_TEX_BASE + select) * 2 + upper);

        if (full && upper)
                printf("// error: out full / upper mutually exclusive\n");

}

static void
print_texture_format(int format)
{
        /* Act like a modifier */
        printf(".");

        switch (format) {
                DEFINE_CASE(TEXTURE_2D, "2d");
                DEFINE_CASE(TEXTURE_3D, "3d");
                DEFINE_CASE(TEXTURE_CUBE, "cube");

        default:
                printf("fmt_%d", format);
                break;
        }
}

static void
print_texture_op(int format)
{
        /* Act like a modifier */
        printf(".");

        switch (format) {
                DEFINE_CASE(TEXTURE_OP_NORMAL, "normal");
                DEFINE_CASE(TEXTURE_OP_TEXEL_FETCH, "texelfetch");

        default:
                printf("op_%d", format);
                break;
        }
}

#undef DEFINE_CASE

static void
print_texture_word(uint32_t *word, unsigned tabs)
{
        midgard_texture_word *texture = (midgard_texture_word *) word;

        /* Instruction family, like ALU words have theirs */
        printf("texture");

        /* Broad category of texture operation in question */
        print_texture_op(texture->op);

        /* Specific format in question */
        print_texture_format(texture->format);

        /* Instruction "modifiers" parallel the ALU instructions. First group
         * are modifiers that act alone */

        if (!texture->filter)
                printf(".raw");

        if (texture->shadow)
                printf(".shadow");

        if (texture->cont)
                printf(".cont");

        if (texture->last)
                printf(".last");

        /* Second set are modifiers which take an extra argument each */

        if (texture->has_offset)
                printf(".offset");

        if (texture->bias)
                printf(".bias");

        printf(" ");

        print_texture_reg(texture->out_full, texture->out_reg_select, texture->out_upper);
        print_mask_4(texture->mask);
        printf(", ");

        printf("texture%d, ", texture->texture_handle);

        printf("sampler%d", texture->sampler_handle);
        print_swizzle_vec4(texture->swizzle, false, false);
        printf(", ");

        print_texture_reg(/*texture->in_reg_full*/true, texture->in_reg_select, texture->in_reg_upper);
        print_swizzle_vec4(texture->in_reg_swizzle, false, false);

        /* TODO: can offsets be full words? */
        if (texture->has_offset) {
                print_texture_reg(false, texture->offset_reg_select, texture->offset_reg_upper);
                printf(", ");
        }

        if (texture->bias)
                printf("%f, ", texture->bias / 256.0f);

        printf("\n");

        /* While not zero in general, for these simple instructions the
         * following unknowns are zero, so we don't include them */

        if (texture->unknown2 ||
                        texture->unknown3 ||
                        texture->unknown4 ||
                        texture->unknownA ||
                        texture->unknownB ||
                        texture->unknown8 ||
                        texture->unknown9) {
                printf("// unknown2 = 0x%x\n", texture->unknown2);
                printf("// unknown3 = 0x%x\n", texture->unknown3);
                printf("// unknown4 = 0x%x\n", texture->unknown4);
                printf("// unknownA = 0x%x\n", texture->unknownA);
                printf("// unknownB = 0x%x\n", texture->unknownB);
                printf("// unknown8 = 0x%x\n", texture->unknown8);
                printf("// unknown9 = 0x%x\n", texture->unknown9);
        }

        /* Similarly, if no offset is applied, these are zero. If an offset
         * -is- applied, or gradients are used, etc, these are nonzero but
         *  largely unknown still. */

        if (texture->offset_unknown1 ||
                        texture->offset_reg_select ||
                        texture->offset_reg_upper ||
                        texture->offset_unknown4 ||
                        texture->offset_unknown5 ||
                        texture->offset_unknown6 ||
                        texture->offset_unknown7 ||
                        texture->offset_unknown8 ||
                        texture->offset_unknown9) {
                printf("// offset_unknown1 = 0x%x\n", texture->offset_unknown1);
                printf("// offset_reg_select = 0x%x\n", texture->offset_reg_select);
                printf("// offset_reg_upper = 0x%x\n", texture->offset_reg_upper);
                printf("// offset_unknown4 = 0x%x\n", texture->offset_unknown4);
                printf("// offset_unknown5 = 0x%x\n", texture->offset_unknown5);
                printf("// offset_unknown6 = 0x%x\n", texture->offset_unknown6);
                printf("// offset_unknown7 = 0x%x\n", texture->offset_unknown7);
                printf("// offset_unknown8 = 0x%x\n", texture->offset_unknown8);
                printf("// offset_unknown9 = 0x%x\n", texture->offset_unknown9);
        }

        /* Don't blow up */
        if (texture->unknown7 != 0x1)
                printf("// (!) unknown7 = %d\n", texture->unknown7);
}

void
disassemble_midgard(uint8_t *code, size_t size)
{
        uint32_t *words = (uint32_t *) code;
        unsigned num_words = size / 4;
        int tabs = 0;

        bool prefetch_flag = false;

        unsigned i = 0;

        while (i < num_words) {
                unsigned tag = words[i] & 0xF;
                unsigned num_quad_words = midgard_word_size[tag];

                switch (midgard_word_types[tag]) {
                case midgard_word_type_texture:
                        print_texture_word(&words[i], tabs);
                        break;

                case midgard_word_type_load_store:
                        print_load_store_word(&words[i], tabs);
                        break;

                case midgard_word_type_alu:
                        print_alu_word(&words[i], num_quad_words, tabs);

                        if (prefetch_flag)
                                return;

                        /* Reset word static analysis state */
                        is_embedded_constant_half = false;
                        is_embedded_constant_int = false;

                        break;

                default:
                        printf("Unknown word type %u:\n", words[i] & 0xF);
                        num_quad_words = 1;
                        print_quad_word(&words[i], tabs);
                        printf("\n");
                        break;
                }

                printf("\n");

                unsigned next = (words[i] & 0xF0) >> 4;

                i += 4 * num_quad_words;

                /* Break based on instruction prefetch flag */

                if (i < num_words && next == 1) {
                        prefetch_flag = true;

                        if (midgard_word_types[words[i] & 0xF] != midgard_word_type_alu)
                                return;
                }
        }

        return;
}

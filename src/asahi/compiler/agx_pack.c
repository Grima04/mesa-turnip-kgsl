/*
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
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

#include "agx_compiler.h"

/* Load/stores have their own operands */

static unsigned
agx_pack_memory_reg(agx_index index, bool *flag)
{
   assert(index.size == AGX_SIZE_16 || index.size == AGX_SIZE_32);
   assert(index.size == AGX_SIZE_16 || (index.value & 1) == 0);
   assert(index.value < 0x100);

   *flag = (index.size == AGX_SIZE_32);
   return index.value;
}

static unsigned
agx_pack_memory_base(agx_index index, bool *flag)
{
   assert(index.size == AGX_SIZE_64);
   assert((index.value & 1) == 0);

   if (index.type == AGX_INDEX_UNIFORM) {
      assert(index.value < 0x200);
      *flag = 1;
      return index.value;
   } else {
      assert(index.value < 0x100);
      *flag = 0;
      return index.value;
   }
}

static unsigned
agx_pack_memory_index(agx_index index, bool *flag)
{
   if (index.type == AGX_INDEX_IMMEDIATE) {
      assert(index.value < 0x10000);
      *flag = 1;

      return index.value;
   } else {
      assert(index.type == AGX_INDEX_REGISTER);
      assert((index.value & 1) == 0);
      assert(index.value < 0x100);

      *flag = 0;
      return index.value;
   }
}

/* ALU goes through a common path */

static unsigned
agx_pack_alu_dst(agx_index dest)
{
   assert(dest.type == AGX_INDEX_REGISTER);
   unsigned reg = dest.value;
   enum agx_size size = dest.size;
   assert(reg < 0x100);

   /* RA invariant: alignment of half-reg */
   if (size >= AGX_SIZE_32)
      assert((reg & 1) == 0);

   return
      (dest.cache ? (1 << 0) : 0) |
      ((size >= AGX_SIZE_32) ? (1 << 1) : 0) |
      ((size == AGX_SIZE_64) ? (1 << 2) : 0) |
      ((reg << 2));
}

static unsigned
agx_pack_alu_src(agx_index src)
{
   unsigned value = src.value;
   enum agx_size size = src.size;

   if (src.type == AGX_INDEX_IMMEDIATE) {
      /* Flags 0 for an 8-bit immediate */
      assert(value < 0x100);

      return
         (value & BITFIELD_MASK(6)) |
         ((value >> 6) << 10);
   } else if (src.type == AGX_INDEX_UNIFORM) {
      assert(size == AGX_SIZE_16 || size == AGX_SIZE_32);
      assert(value < 0x200);

      return
         (value & BITFIELD_MASK(6)) |
         ((value >> 8) << 6) |
         ((size == AGX_SIZE_32) ? (1 << 7) : 0) |
         (0x1 << 8) |
         (((value >> 6) & BITFIELD_MASK(2)) << 10);
   } else {
      assert(src.type == AGX_INDEX_REGISTER);
      assert(!(src.cache && src.discard));

      unsigned hint = src.discard ? 0x3 : src.cache ? 0x2 : 0x1;
      unsigned size_flag =
         (size == AGX_SIZE_64) ? 0x3 :
         (size == AGX_SIZE_32) ? 0x2 :
         (size == AGX_SIZE_16) ? 0x0 : 0x0;

      return
         (value & BITFIELD_MASK(6)) |
         (hint << 6) |
         (size_flag << 8) |
         (((value >> 6) & BITFIELD_MASK(2)) << 10);
   }
}

static unsigned
agx_pack_float_mod(agx_index src)
{
   return (src.abs ? (1 << 0) : 0)
        | (src.neg ? (1 << 1) : 0);
}

static bool
agx_all_16(agx_instr *I)
{
   agx_foreach_dest(I, d) {
      if (!agx_is_null(I->dest[d]) && I->dest[d].size != AGX_SIZE_16)
         return false;
   }

   agx_foreach_src(I, s) {
      if (!agx_is_null(I->src[s]) && I->src[s].size != AGX_SIZE_16)
         return false;
   }

   return true;
}

/* Generic pack for ALU instructions, which are quite regular */

static void
agx_pack_alu(struct util_dynarray *emission, agx_instr *I)
{
   struct agx_opcode_info info = agx_opcodes_info[I->op];
   bool is_16 = agx_all_16(I) && info.encoding_16.exact;
   struct agx_encoding encoding = is_16 ?
                                     info.encoding_16 : info.encoding;

   assert(encoding.exact && "invalid encoding");

   uint64_t raw = encoding.exact;
   uint16_t extend = 0;

   // TODO: assert saturable
   if (I->saturate)
      raw |= (1 << 6);

   if (info.nr_dests) {
      assert(info.nr_dests == 1);
      unsigned D = agx_pack_alu_dst(I->dest[0]);
      unsigned extend_offset = (sizeof(extend)*8) - 4;

      raw |= (D & BITFIELD_MASK(8)) << 7;
      extend |= ((D >> 8) << extend_offset);
   }

   for (unsigned s = 0; s < info.nr_srcs; ++s) {
      unsigned src = agx_pack_alu_src(I->src[s]);
      unsigned src_short = (src & BITFIELD_MASK(10));
      unsigned src_extend = (src >> 10);

      /* Size bit always zero and so omitted for 16-bit */
      if (is_16)
         assert((src_short & (1 << 9)) == 0);

      if (info.is_float) {
         unsigned fmod = agx_pack_float_mod(I->src[s]);
         unsigned fmod_offset = is_16 ? 9 : 10;
         src_short |= (fmod << fmod_offset);
      } else if (I->op == AGX_OPCODE_IMAD || I->op == AGX_OPCODE_IADD) {
         bool zext = I->src[s].abs;
         bool extends = I->src[s].size < AGX_SIZE_64;

         unsigned sxt = (extends && !zext) ? (1 << 10) : 0;

         assert(!I->src[s].neg || s == 1);
         src_short |= sxt;
      }

      /* Sources come at predictable offsets */
      unsigned offset = 16 + (12 * s);
      raw |= (((uint64_t) src_short) << offset);

      /* Destination and each source get extended in reverse order */
      unsigned extend_offset = (sizeof(extend)*8) - ((s + 3) * 2);
      extend |= (src_extend << extend_offset);
   }

   if ((I->op == AGX_OPCODE_IMAD || I->op == AGX_OPCODE_IADD) && I->src[1].neg)
      raw |= (1 << 27);

   if (info.immediates & AGX_IMMEDIATE_TRUTH_TABLE) {
      raw |= (I->truth_table & 0x3) << 26;
      raw |= (uint64_t) (I->truth_table >> 2)  << 38;
   } else if (info.immediates & AGX_IMMEDIATE_SHIFT) {
      raw |= (uint64_t) (I->shift & 1) << 39;
      raw |= (uint64_t) (I->shift >> 2) << 52;
   } else if (info.immediates & AGX_IMMEDIATE_BFI_MASK) {
      raw |= (uint64_t) (I->mask & 0x3) << 38;
      raw |= (uint64_t) ((I->mask >> 2) & 0x3) << 50;
      raw |= (uint64_t) ((I->mask >> 4) & 0x1) << 63;
   } else if (info.immediates & AGX_IMMEDIATE_WRITEOUT)
      raw |= (uint64_t) (I->imm) << 8;
   else if (info.immediates & AGX_IMMEDIATE_IMM)
      raw |= (uint64_t) (I->imm) << 16;
   else if (info.immediates & AGX_IMMEDIATE_ROUND)
      raw |= (uint64_t) (I->imm) << 26;

   /* Determine length bit */
   unsigned length = encoding.length_short;
   unsigned short_mask = (1 << length) - 1;
   bool length_bit = (extend || (raw & ~short_mask));

   if (encoding.extensible && length_bit) {
      raw |= (1 << 15);
      length += (length > 8) ? 4 : 2;
   }

   /* Pack! */
   if (length <= sizeof(uint64_t)) {
      unsigned extend_offset = ((length - sizeof(extend)) * 8);

      /* XXX: This is a weird special case */
      if (I->op == AGX_OPCODE_IADD)
         extend_offset -= 16;

      raw |= (uint64_t) extend << extend_offset;
      memcpy(util_dynarray_grow_bytes(emission, 1, length), &raw, length);
   } else {
      /* So far, >8 byte ALU is only to store the extend bits */
      unsigned extend_offset = (((length - sizeof(extend)) * 8) - 64);
      unsigned hi = ((uint64_t) extend) << extend_offset;

      memcpy(util_dynarray_grow_bytes(emission, 1, 8), &raw, 8);
      memcpy(util_dynarray_grow_bytes(emission, 1, length - 8), &hi, length - 8);
   }
}

static void
agx_pack_instr(struct util_dynarray *emission, agx_instr *I)
{
   switch (I->op) {
   case AGX_OPCODE_BLEND:
   {
      unsigned D = agx_pack_alu_dst(I->src[0]);
      unsigned rt = 0; /* TODO */
      unsigned mask = I->mask ?: 0xF;
      assert(mask < 0x10);

      uint64_t raw =
         0x09 |
         ((uint64_t) (D & BITFIELD_MASK(8)) << 7) |
         ((uint64_t) (I->format) << 24) |
         ((uint64_t) (rt) << 32) |
         ((uint64_t) (mask) << 36) |
         ((uint64_t) 0x0380FC << 40) |
         (((uint64_t) (D >> 8)) << 60);

      unsigned size = 8;
      memcpy(util_dynarray_grow_bytes(emission, 1, size), &raw, size);
      break;
   }

   case AGX_OPCODE_LD_VARY:
   {
      unsigned D = agx_pack_alu_dst(I->dest[0]);
      bool perspective = 1; // TODO
      unsigned channels = (I->channels & 0x3);
      assert(I->mask < 0xF); /* 0 indicates full mask */
      agx_index index_src = I->src[0];
      assert(index_src.type == AGX_INDEX_IMMEDIATE);
      assert((D >> 8) == 0); /* TODO: Dx? */
      unsigned index = index_src.value;

      uint64_t raw =
            0x21 | (perspective ? (1 << 6) : 0) |
            ((D & 0xFF) << 7) |
            (1ull << 15) | /* XXX */
            (((uint64_t) index) << 16) |
            (((uint64_t) channels) << 30) |
            (1ull << 46) | /* XXX */
            (1ull << 52); /* XXX */

      unsigned size = 8;
      memcpy(util_dynarray_grow_bytes(emission, 1, size), &raw, size);
      break;
   }

   case AGX_OPCODE_DEVICE_LOAD:
   {
      assert(I->mask != 0);
      assert(I->format <= 0x10);

      bool Rt, At, Ot;
      unsigned R = agx_pack_memory_reg(I->dest[0], &Rt);
      unsigned A = agx_pack_memory_base(I->src[0], &At);
      unsigned O = agx_pack_memory_index(I->src[1], &Ot);
      unsigned u1 = 1; // XXX
      unsigned u3 = 0;
      unsigned u4 = 4; // XXX
      unsigned u5 = 0;
      bool L = true; /* TODO: when would you want short? */

      uint64_t raw =
            0x05 |
            ((I->format & BITFIELD_MASK(3)) << 7) |
            ((R & BITFIELD_MASK(6)) << 10) |
            ((A & BITFIELD_MASK(4)) << 16) |
            ((O & BITFIELD_MASK(4)) << 20) |
            (Ot ? (1 << 24) : 0) |
            (I->src[1].abs ? (1 << 25) : 0) |
            (u1 << 26) |
            (At << 27) |
            (u3 << 28) |
            (I->scoreboard << 30) |
            (((uint64_t) ((O >> 4) & BITFIELD_MASK(4))) << 32) |
            (((uint64_t) ((A >> 4) & BITFIELD_MASK(4))) << 36) |
            (((uint64_t) ((R >> 6) & BITFIELD_MASK(2))) << 40) |
            (((uint64_t) I->shift) << 42) |
            (((uint64_t) u4) << 44) |
            (L ? (1ull << 47) : 0) |
            (((uint64_t) (I->format >> 3)) << 48) |
            (((uint64_t) Rt) << 49) |
            (((uint64_t) u5) << 50) |
            (((uint64_t) I->mask) << 52) |
            (((uint64_t) (O >> 8)) << 56);

      unsigned size = L ? 8 : 6;
      memcpy(util_dynarray_grow_bytes(emission, 1, size), &raw, size);
      break;
   }

   default:
      agx_pack_alu(emission, I);
      return;
   }
}

void
agx_pack(agx_context *ctx, struct util_dynarray *emission)
{
   agx_foreach_instr_global(ctx, ins)
      agx_pack_instr(emission, ins);
}

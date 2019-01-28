/*
 * Copyright © 2019 Google LLC
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#ifndef TU_CS_H
#define TU_CS_H

#include "tu_private.h"

#include "registers/adreno_pm4.xml.h"

void
tu_cs_init(struct tu_cs *cs, uint32_t initial_size);

void
tu_cs_finish(struct tu_device *dev, struct tu_cs *cs);

void
tu_cs_begin(struct tu_cs *cs);

void
tu_cs_end(struct tu_cs *cs);

VkResult
tu_cs_reserve_space(struct tu_device *dev,
                    struct tu_cs *cs,
                    uint32_t reserved_size);

void
tu_cs_reset(struct tu_device *dev, struct tu_cs *cs);

/**
 * Assert that we did not exceed the reserved space.
 */
static inline void
tu_cs_reserve_space_assert(struct tu_cs *cs)
{
   assert(cs->cur <= cs->reserved_end);
}

/**
 * Emit a uint32_t value into a command stream, without boundary checking.
 */
static inline void
tu_cs_emit(struct tu_cs *cs, uint32_t value)
{
   assert(cs->cur < cs->end);
   *cs->cur = value;
   ++cs->cur;
}

static inline unsigned
tu_odd_parity_bit(unsigned val)
{
   /* See: http://graphics.stanford.edu/~seander/bithacks.html#ParityParallel
    * note that we want odd parity so 0x6996 is inverted.
    */
   val ^= val >> 16;
   val ^= val >> 8;
   val ^= val >> 4;
   val &= 0xf;
   return (~0x6996 >> val) & 1;
}

/**
 * Emit a type-4 command packet header into a command stream.
 */
static inline void
tu_cs_emit_pkt4(struct tu_cs *cs, uint16_t regindx, uint16_t cnt)
{
   tu_cs_emit(cs, CP_TYPE4_PKT | cnt | (tu_odd_parity_bit(cnt) << 7) |
                     ((regindx & 0x3ffff) << 8) |
                     ((tu_odd_parity_bit(regindx) << 27)));
}

/**
 * Emit a type-7 command packet header into a command stream.
 */
static inline void
tu_cs_emit_pkt7(struct tu_cs *cs, uint8_t opcode, uint16_t cnt)
{
   tu_cs_emit(cs, CP_TYPE7_PKT | cnt | (tu_odd_parity_bit(cnt) << 15) |
                     ((opcode & 0x7f) << 16) |
                     ((tu_odd_parity_bit(opcode) << 23)));
}

static inline void
tu_cs_emit_wfi(struct tu_cs *cs)
{
   tu_cs_emit_pkt7(cs, CP_WAIT_FOR_IDLE, 0);
}

static inline void
tu_cs_emit_qw(struct tu_cs *cs, uint64_t value)
{
   tu_cs_emit(cs, (uint32_t) value);
   tu_cs_emit(cs, (uint32_t) (value >> 32));
}

static inline void
tu_cs_emit_write_reg(struct tu_cs *cs, uint16_t reg, uint32_t value)
{
   tu_cs_emit_pkt4(cs, reg, 1);
   tu_cs_emit(cs, value);
}

static inline void
tu_cs_emit_ib(struct tu_cs *cs, const struct tu_cs *target)
{
   for (uint32_t i = 0; i < target->entry_count; i++) {
      const struct tu_cs_entry *entry = target->entries + i;

      tu_cs_emit_pkt7(cs, CP_INDIRECT_BUFFER, 3);
      tu_cs_emit_qw(cs, entry->bo->iova + entry->offset);
      tu_cs_emit(cs, entry->size / sizeof(uint32_t));
   }
}

#endif /* TU_CS_H */

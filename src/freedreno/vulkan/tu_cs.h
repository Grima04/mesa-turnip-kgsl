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

#include "adreno_pm4.xml.h"

void
tu_cmd_stream_init(struct tu_cmd_stream *stream);
void
tu_cmd_stream_finish(struct tu_device *dev, struct tu_cmd_stream *stream);
VkResult
tu_cmd_stream_begin(struct tu_device *dev,
                    struct tu_cmd_stream *stream,
                    uint32_t reserve_size);
VkResult
tu_cmd_stream_end(struct tu_cmd_stream *stream);
void
tu_cmd_stream_reset(struct tu_device *dev, struct tu_cmd_stream *stream);
VkResult
tu_cs_check_space(struct tu_device *dev,
                  struct tu_cmd_stream *stream,
                  size_t size);

static inline void
tu_cs_emit(struct tu_cmd_stream *stream, uint32_t value)
{
   assert(stream->cur < stream->end);
   *stream->cur = value;
   ++stream->cur;
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

static inline void
tu_cs_emit_pkt4(struct tu_cmd_stream *stream, uint16_t regindx, uint16_t cnt)
{
   tu_cs_emit(stream, CP_TYPE4_PKT | cnt | (tu_odd_parity_bit(cnt) << 7) |
                         ((regindx & 0x3ffff) << 8) |
                         ((tu_odd_parity_bit(regindx) << 27)));
}

static inline void
tu_cs_emit_pkt7(struct tu_cmd_stream *stream, uint8_t opcode, uint16_t cnt)
{
   tu_cs_emit(stream, CP_TYPE7_PKT | cnt | (tu_odd_parity_bit(cnt) << 15) |
                         ((opcode & 0x7f) << 16) |
                         ((tu_odd_parity_bit(opcode) << 23)));
}

static inline void
tu_cs_emit_wfi5(struct tu_cmd_stream *stream)
{
   tu_cs_emit_pkt7(stream, CP_WAIT_FOR_IDLE, 0);
}

#endif /* TU_CS_H */

/**************************************************************************
 *
 * Copyright 2020 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "vid_dec.h"

static unsigned av1_f(struct vl_vlc *vlc, unsigned n)
{
   unsigned valid = vl_vlc_valid_bits(vlc);

   if (n == 0)
      return 0;

   if (valid < 32)
      vl_vlc_fillbits(vlc);

   return vl_vlc_get_uimsbf(vlc, n);
}

static unsigned av1_uvlc(struct vl_vlc *vlc)
{
   unsigned value;
   unsigned leadingZeros = 0;

   while (1) {
      bool done = av1_f(vlc, 1);
      if (done)
         break;
      leadingZeros++;
   }

   if (leadingZeros >= 32)
      return 0xffffffff;

   value = av1_f(vlc, leadingZeros);

   return value + (1 << leadingZeros) - 1;
}

static int av1_le(struct vl_vlc *vlc, const unsigned n)
{
   unsigned byte, t = 0;
   unsigned i;

   for (i = 0; i < n; ++i) {
      byte = av1_f(vlc, 8);
      t += (byte << (i * 8));
   }

   return t;
}

static unsigned av1_uleb128(struct vl_vlc *vlc)
{
   unsigned value = 0;
   unsigned leb128Bytes = 0;
   unsigned i;

   for (i = 0; i < 8; ++i) {
      leb128Bytes = av1_f(vlc, 8);
      value |= ((leb128Bytes & 0x7f) << (i * 7));
      if (!(leb128Bytes & 0x80))
         break;
   }

   return value;
}

static int av1_su(struct vl_vlc *vlc, const unsigned n)
{
   unsigned value = av1_f(vlc, n);
   unsigned signMask = 1 << (n - 1);

   if (value && signMask)
      value = value - 2 * signMask;

   return value;
}

static unsigned FloorLog2(unsigned x)
{
   unsigned s = 0;
   unsigned x1 = x;

   while (x1 != 0) {
      x1 = x1 >> 1;
      s++;
   }

   return s - 1;
}

static unsigned av1_ns(struct vl_vlc *vlc, unsigned n)
{
   unsigned w = FloorLog2(n) + 1;
   unsigned m = (1 << w) - n;
   unsigned v = av1_f(vlc, w - 1);

   if (v < m)
      return v;

   bool extra_bit = av1_f(vlc, 1);

   return (v << 1) - m + extra_bit;
}

static void av1_byte_alignment(struct vl_vlc *vlc)
{
   vl_vlc_eatbits(vlc, vl_vlc_valid_bits(vlc) % 8);
}

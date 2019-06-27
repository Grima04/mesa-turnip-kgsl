/**************************************************************************
 *
 * Copyright 2010 Luca Barbieri
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


#ifndef U_HALF_H
#define U_HALF_H

#include "pipe/p_compiler.h"
#include "util/u_math.h"
#include "util/half_float.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * References for float <-> half conversions
 *
 *  http://fgiesen.wordpress.com/2012/03/28/half-to-float-done-quic/
 *  https://gist.github.com/2156668
 *  https://gist.github.com/2144712
 */

static inline uint16_t
util_float_to_half(float f)
{
   return _mesa_float_to_half(f);
}

static inline float
util_half_to_float(uint16_t f16)
{
   union fi infnan;
   union fi magic;
   union fi f32;

   infnan.ui = 0x8f << 23;
   infnan.f = 65536.0f;
   magic.ui  = 0xef << 23;

   /* Exponent / Mantissa */
   f32.ui = (f16 & 0x7fff) << 13;

   /* Adjust */
   f32.f *= magic.f;
   /* XXX: The magic mul relies on denorms being available */

   /* Inf / NaN */
   if (f32.f >= infnan.f)
      f32.ui |= 0xff << 23;

   /* Sign */
   f32.ui |= (uint32_t)(f16 & 0x8000) << 16;

   return f32.f;
}

#ifdef __cplusplus
}
#endif

#endif /* U_HALF_H */


/*
 * The implementations contained in this file are heavily based on the
 * implementations found in the Berkeley SoftFloat library. As such, they are
 * licensed under the same 3-clause BSD license:
 *
 * License for Berkeley SoftFloat Release 3e
 *
 * John R. Hauser
 * 2018 January 20
 *
 * The following applies to the whole of SoftFloat Release 3e as well as to
 * each source file individually.
 *
 * Copyright 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018 The Regents of the
 * University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions, and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions, and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the University nor the names of its contributors
 *     may be used to endorse or promote products derived from this software
 *     without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS "AS IS", AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#version 430
#extension GL_ARB_gpu_shader_int64 : enable
#extension GL_ARB_shader_bit_encoding : enable
#extension GL_EXT_shader_integer_mix : enable

#pragma warning(off)

/* Software IEEE floating-point rounding mode.
 * GLSL spec section "4.7.1 Range and Precision":
 * The rounding mode cannot be set and is undefined.
 * But here, we are able to define the rounding mode at the compilation time.
 */
#define FLOAT_ROUND_NEAREST_EVEN    0
#define FLOAT_ROUND_TO_ZERO         1
#define FLOAT_ROUND_DOWN            2
#define FLOAT_ROUND_UP              3
#define FLOAT_ROUNDING_MODE         FLOAT_ROUND_NEAREST_EVEN

/* Absolute value of a Float64 :
 * Clear the sign bit
 */
uint64_t
__fabs64(uint64_t __a)
{
   uvec2 a = unpackUint2x32(__a);
   a.y &= 0x7FFFFFFFu;
   return packUint2x32(a);
}

/* Returns 1 if the double-precision floating-point value `a' is a NaN;
 * otherwise returns 0.
 */
bool
__is_nan(uint64_t __a)
{
   uvec2 a = unpackUint2x32(__a);
   return (0xFFE00000u <= (a.y<<1)) &&
      ((a.x != 0u) || ((a.y & 0x000FFFFFu) != 0u));
}

/* Negate value of a Float64 :
 * Toggle the sign bit
 */
uint64_t
__fneg64(uint64_t __a)
{
   uvec2 a = unpackUint2x32(__a);
   uint t = a.y;

   t ^= (1u << 31);
   a.y = mix(t, a.y, __is_nan(__a));
   return packUint2x32(a);
}

uint64_t
__fsign64(uint64_t __a)
{
   uvec2 a = unpackUint2x32(__a);
   uvec2 retval;
   retval.x = 0u;
   retval.y = mix((a.y & 0x80000000u) | 0x3FF00000u, 0u, (a.y << 1 | a.x) == 0u);
   return packUint2x32(retval);
}

/* Returns the fraction bits of the double-precision floating-point value `a'.*/
uint
__extractFloat64FracLo(uint64_t a)
{
   return unpackUint2x32(a).x;
}

uint
__extractFloat64FracHi(uint64_t a)
{
   return unpackUint2x32(a).y & 0x000FFFFFu;
}

/* Returns the exponent bits of the double-precision floating-point value `a'.*/
int
__extractFloat64Exp(uint64_t __a)
{
   uvec2 a = unpackUint2x32(__a);
   return int((a.y>>20) & 0x7FFu);
}

bool
__feq64_nonnan(uint64_t __a, uint64_t __b)
{
   uvec2 a = unpackUint2x32(__a);
   uvec2 b = unpackUint2x32(__b);
   return (a.x == b.x) &&
          ((a.y == b.y) || ((a.x == 0u) && (((a.y | b.y)<<1) == 0u)));
}

/* Returns true if the double-precision floating-point value `a' is equal to the
 * corresponding value `b', and false otherwise.  The comparison is performed
 * according to the IEEE Standard for Floating-Point Arithmetic.
 */
bool
__feq64(uint64_t a, uint64_t b)
{
   if (__is_nan(a) || __is_nan(b))
      return false;

   return __feq64_nonnan(a, b);
}

/* Returns true if the double-precision floating-point value `a' is not equal
 * to the corresponding value `b', and false otherwise.  The comparison is
 * performed according to the IEEE Standard for Floating-Point Arithmetic.
 */
bool
__fne64(uint64_t a, uint64_t b)
{
   if (__is_nan(a) || __is_nan(b))
      return true;

   return !__feq64_nonnan(a, b);
}

/* Returns the sign bit of the double-precision floating-point value `a'.*/
uint
__extractFloat64Sign(uint64_t a)
{
   return unpackUint2x32(a).y >> 31;
}

/* Returns true if the 64-bit value formed by concatenating `a0' and `a1' is less
 * than the 64-bit value formed by concatenating `b0' and `b1'.  Otherwise,
 * returns false.
 */
bool
lt64(uint a0, uint a1, uint b0, uint b1)
{
   return (a0 < b0) || ((a0 == b0) && (a1 < b1));
}

bool
__flt64_nonnan(uint64_t __a, uint64_t __b)
{
   uvec2 a = unpackUint2x32(__a);
   uvec2 b = unpackUint2x32(__b);
   uint aSign = __extractFloat64Sign(__a);
   uint bSign = __extractFloat64Sign(__b);
   if (aSign != bSign)
      return (aSign != 0u) && ((((a.y | b.y)<<1) | a.x | b.x) != 0u);

   return mix(lt64(a.y, a.x, b.y, b.x), lt64(b.y, b.x, a.y, a.x), aSign != 0u);
}

/* Returns true if the double-precision floating-point value `a' is less than
 * the corresponding value `b', and false otherwise.  The comparison is performed
 * according to the IEEE Standard for Floating-Point Arithmetic.
 */
bool
__flt64(uint64_t a, uint64_t b)
{
   if (__is_nan(a) || __is_nan(b))
      return false;

   return __flt64_nonnan(a, b);
}

/* Returns true if the double-precision floating-point value `a' is greater
 * than or equal to * the corresponding value `b', and false otherwise.  The
 * comparison is performed * according to the IEEE Standard for Floating-Point
 * Arithmetic.
 */
bool
__fge64(uint64_t a, uint64_t b)
{
   if (__is_nan(a) || __is_nan(b))
      return false;

   return !__flt64_nonnan(a, b);
}

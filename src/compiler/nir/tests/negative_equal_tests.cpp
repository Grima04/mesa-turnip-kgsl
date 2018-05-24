/*
 * Copyright © 2018 Intel Corporation
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
#include <gtest/gtest.h>
#include "nir.h"
#include "util/half_float.h"

static nir_const_value count_sequence(nir_alu_type base_type, unsigned bits,
                                      int first);
static nir_const_value negate(const nir_const_value &src,
                              nir_alu_type base_type, unsigned bits,
                              unsigned components);

class const_value_negative_equal_test : public ::testing::Test {
protected:
   const_value_negative_equal_test()
   {
      memset(&c1, 0, sizeof(c1));
      memset(&c2, 0, sizeof(c2));
   }

   ~const_value_negative_equal_test()
   {
      /* empty */
   }

   nir_const_value c1;
   nir_const_value c2;
};


TEST_F(const_value_negative_equal_test, float32_zero)
{
   /* Verify that 0.0 negative-equals 0.0. */
   EXPECT_TRUE(nir_const_value_negative_equal(&c1, &c1,
                                              4, nir_type_float, 32));
}

TEST_F(const_value_negative_equal_test, float64_zero)
{
   /* Verify that 0.0 negative-equals 0.0. */
   EXPECT_TRUE(nir_const_value_negative_equal(&c1, &c1,
                                              4, nir_type_float, 64));
}

/* Compare an object with non-zero values to itself.  This should always be
 * false.
 */
#define compare_with_self(base_type, bits) \
TEST_F(const_value_negative_equal_test, base_type ## bits ## _self)     \
{                                                                       \
   c1 = count_sequence(base_type, bits, 1);                             \
   EXPECT_FALSE(nir_const_value_negative_equal(&c1, &c1, 4, base_type, bits)); \
}

compare_with_self(nir_type_float, 16)
compare_with_self(nir_type_float, 32)
compare_with_self(nir_type_float, 64)
compare_with_self(nir_type_int, 8)
compare_with_self(nir_type_uint, 8)
compare_with_self(nir_type_int, 16)
compare_with_self(nir_type_uint, 16)
compare_with_self(nir_type_int, 32)
compare_with_self(nir_type_uint, 32)
compare_with_self(nir_type_int, 64)
compare_with_self(nir_type_uint, 64)

/* Compare an object with the negation of itself.  This should always be true.
 */
#define compare_with_negation(base_type, bits) \
TEST_F(const_value_negative_equal_test, base_type ## bits ## _trivially_true) \
{                                                                       \
   c1 = count_sequence(base_type, bits, 1);                             \
   c2 = negate(c1, base_type, bits, 4);                                 \
   EXPECT_TRUE(nir_const_value_negative_equal(&c1, &c2, 4, base_type, bits)); \
}

compare_with_negation(nir_type_float, 16)
compare_with_negation(nir_type_float, 32)
compare_with_negation(nir_type_float, 64)
compare_with_negation(nir_type_int, 8)
compare_with_negation(nir_type_uint, 8)
compare_with_negation(nir_type_int, 16)
compare_with_negation(nir_type_uint, 16)
compare_with_negation(nir_type_int, 32)
compare_with_negation(nir_type_uint, 32)
compare_with_negation(nir_type_int, 64)
compare_with_negation(nir_type_uint, 64)

/* Compare fewer than the maximum possible components.  All of the components
 * that are compared a negative-equal, but the extra components are not.
 */
#define compare_fewer_components(base_type, bits) \
TEST_F(const_value_negative_equal_test, base_type ## bits ## _fewer_components) \
{                                                                       \
   c1 = count_sequence(base_type, bits, 1);                             \
   c2 = negate(c1, base_type, bits, 3);                                 \
   EXPECT_TRUE(nir_const_value_negative_equal(&c1, &c2, 3, base_type, bits)); \
   EXPECT_FALSE(nir_const_value_negative_equal(&c1, &c2, 4, base_type, bits)); \
}

compare_fewer_components(nir_type_float, 16)
compare_fewer_components(nir_type_float, 32)
compare_fewer_components(nir_type_float, 64)
compare_fewer_components(nir_type_int, 8)
compare_fewer_components(nir_type_uint, 8)
compare_fewer_components(nir_type_int, 16)
compare_fewer_components(nir_type_uint, 16)
compare_fewer_components(nir_type_int, 32)
compare_fewer_components(nir_type_uint, 32)
compare_fewer_components(nir_type_int, 64)
compare_fewer_components(nir_type_uint, 64)

static nir_const_value
count_sequence(nir_alu_type base_type, unsigned bits, int first)
{
   nir_const_value c;

   switch (base_type) {
   case nir_type_float:
      switch (bits) {
      case 16:
         for (unsigned i = 0; i < ARRAY_SIZE(c.u16); i++)
            c.u16[i] = _mesa_float_to_half(float(i + first));

         break;

      case 32:
         for (unsigned i = 0; i < ARRAY_SIZE(c.f32); i++)
            c.f32[i] = float(i + first);

         break;

      case 64:
         for (unsigned i = 0; i < ARRAY_SIZE(c.f64); i++)
            c.f64[i] = double(i + first);

         break;

      default:
         unreachable("unknown bit size");
      }

      break;

   case nir_type_int:
   case nir_type_uint:
      switch (bits) {
      case 8:
         for (unsigned i = 0; i < ARRAY_SIZE(c.i8); i++)
            c.i8[i] = i + first;

         break;

      case 16:
         for (unsigned i = 0; i < ARRAY_SIZE(c.i16); i++)
            c.i16[i] = i + first;

         break;

      case 32:
         for (unsigned i = 0; i < ARRAY_SIZE(c.i32); i++)
            c.i32[i] = i + first;

         break;

      case 64:
         for (unsigned i = 0; i < ARRAY_SIZE(c.i64); i++)
            c.i64[i] = i + first;

         break;

      default:
         unreachable("unknown bit size");
      }

      break;

   case nir_type_bool:
   default:
      unreachable("invalid base type");
   }

   return c;
}

static nir_const_value
negate(const nir_const_value &src, nir_alu_type base_type, unsigned bits,
       unsigned components)
{
   nir_const_value c = src;

   switch (base_type) {
   case nir_type_float:
      switch (bits) {
      case 16:
         for (unsigned i = 0; i < components; i++)
            c.u16[i] = _mesa_float_to_half(-_mesa_half_to_float(c.u16[i]));

         break;

      case 32:
         for (unsigned i = 0; i < components; i++)
            c.f32[i] = -c.f32[i];

         break;

      case 64:
         for (unsigned i = 0; i < components; i++)
            c.f64[i] = -c.f64[i];

         break;

      default:
         unreachable("unknown bit size");
      }

      break;

   case nir_type_int:
   case nir_type_uint:
      switch (bits) {
      case 8:
         for (unsigned i = 0; i < components; i++)
            c.i8[i] = -c.i8[i];

         break;

      case 16:
         for (unsigned i = 0; i < components; i++)
            c.i16[i] = -c.i16[i];

         break;

      case 32:
         for (unsigned i = 0; i < components; i++)
            c.i32[i] = -c.i32[i];

         break;

      case 64:
         for (unsigned i = 0; i < components; i++)
            c.i64[i] = -c.i64[i];

         break;

      default:
         unreachable("unknown bit size");
      }

      break;

   case nir_type_bool:
   default:
      unreachable("invalid base type");
   }

   return c;
}

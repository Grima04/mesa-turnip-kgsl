#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <memory>

#include <gtest/gtest.h>

#include "GL/osmesa.h"


typedef std::array<GLenum, 2> Params;

class OSMesaRenderTestFixture : public testing::TestWithParam<Params> {};

std::string
name_params(const testing::TestParamInfo<Params> params) {
   auto p = params.param;
   std::string first, second;
   switch (p[0]) {
   case OSMESA_RGBA:
      first = "rgba";
      break;
   case OSMESA_BGRA:
      first = "bgra";
      break;
   case OSMESA_RGB:
      first = "rgb";
      break;
   case OSMESA_RGB_565:
      first = "rgb_565";
      break;
   case OSMESA_ARGB:
      first = "argb";
      break;
   }

   switch (p[1]) {
   case GL_UNSIGNED_SHORT:
      second = "unsigned_short";
      break;
   case GL_UNSIGNED_BYTE:
      second = "unsigned_byte";
      break;
   case GL_FLOAT:
      second = "float";
      break;
   case GL_UNSIGNED_SHORT_5_6_5:
      second = "unisgned_short_565";
      break;
   }

   return first + "_" + second;
};

TEST_P(OSMesaRenderTestFixture, Render)
{
   auto params = GetParam();
   const int w = 2, h = 2;
   uint8_t pixels[w * h * 4] = { 0 };
   uint32_t expected;  // This should be green for the given color model

   std::unique_ptr<osmesa_context, decltype(&OSMesaDestroyContext)> ctx{
      OSMesaCreateContext(params[0], NULL), &OSMesaDestroyContext};
   ASSERT_TRUE(ctx);

   auto ret = OSMesaMakeCurrent(ctx.get(), &pixels, params[1], w, h);
   ASSERT_EQ(ret, GL_TRUE);

   int bpp = 4;
   switch (params[0]) {
   case OSMESA_RGB:
      bpp = 3;
      break;
   case OSMESA_RGB_565:
      bpp = 2;
      break;
   }

   switch (params[0]) {
   case OSMESA_RGBA:
   case OSMESA_BGRA:
   case OSMESA_RGB:
      expected = 0xff << 8;
      glClearColor(0, 1, 0, 0);
      break;
   case OSMESA_RGB_565:
      expected = 0x3f << 5;
      glClearColor(0, 1, 0, 0);
      break;
   case OSMESA_ARGB:
      expected = 0xff << 24;
      glClearColor(0, 0, 1, 0);
      break;
   }
   glClear(GL_COLOR_BUFFER_BIT);
   glFinish();

   for (unsigned i = 0; i < w * h; i++) {
      uint32_t color = 0;
      memcpy(&color, &pixels[i * bpp], bpp);

      ASSERT_EQ(expected, color);
   }
}

INSTANTIATE_TEST_CASE_P(
   OSMesaRenderTest,
   OSMesaRenderTestFixture,
   testing::Values(
      Params{ OSMESA_RGBA, GL_UNSIGNED_BYTE },
      Params{ OSMESA_BGRA, GL_UNSIGNED_BYTE },
      Params{ OSMESA_ARGB, GL_UNSIGNED_BYTE },
      Params{ OSMESA_RGB, GL_UNSIGNED_BYTE }
   ),
   name_params
);

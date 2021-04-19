from __future__ import print_function

from mako.template import Template
from sys import argv

string = """/*
 * Mesa 3-D graphics library
 *
 * Copyright (c) 2011 VMware, Inc.
 * Copyright (c) 2014 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


/**
 * Color, depth, stencil packing functions.
 * Used to pack basic color, depth and stencil formats to specific
 * hardware formats.
 *
 * There are both per-pixel and per-row packing functions:
 * - The former will be used by swrast to write values to the color, depth,
 *   stencil buffers when drawing points, lines and masked spans.
 * - The later will be used for image-oriented functions like glDrawPixels,
 *   glAccum, and glTexImage.
 */

#include <stdint.h>

#include "format_pack.h"
#include "format_utils.h"
#include "macros.h"
#include "util/format_rgb9e5.h"
#include "util/format_r11g11b10f.h"
#include "util/format_srgb.h"

#define UNPACK(SRC, OFFSET, BITS) (((SRC) >> (OFFSET)) & MAX_UINT(BITS))
#define PACK(SRC, OFFSET, BITS) (((SRC) & MAX_UINT(BITS)) << (OFFSET))

<%
import format_parser as parser

formats = parser.parse(argv[1])

rgb_formats = []
for f in formats:
   if f.name == 'MESA_FORMAT_NONE':
      continue
   if f.colorspace not in ('rgb', 'srgb'):
      continue

   rgb_formats.append(f)
%>

/* ubyte packing functions */

%for f in rgb_formats:
   %if f.name in ('MESA_FORMAT_R9G9B9E5_FLOAT', 'MESA_FORMAT_R11G11B10_FLOAT'):
      <% continue %>
   %elif f.is_compressed():
      <% continue %>
   %endif

static inline void
pack_ubyte_${f.short_name()}(const uint8_t src[4], void *dst)
{
   %for (i, c) in enumerate(f.channels):
      <% i = f.swizzle.inverse()[i] %>
      %if c.type == 'x':
         <% continue %>
      %endif

      ${c.datatype()} ${c.name} =
      %if not f.is_normalized() and f.is_int():
          %if c.type == parser.SIGNED:
              _mesa_unsigned_to_signed(src[${i}], ${c.size});
          %else:
              _mesa_unsigned_to_unsigned(src[${i}], ${c.size});
          %endif
      %elif c.type == parser.UNSIGNED:
         %if f.colorspace == 'srgb' and c.name in 'rgb':
            <% assert c.size == 8 %>
            util_format_linear_to_srgb_8unorm(src[${i}]);
         %else:
            _mesa_unorm_to_unorm(src[${i}], 8, ${c.size});
         %endif
      %elif c.type == parser.SIGNED:
         _mesa_unorm_to_snorm(src[${i}], 8, ${c.size});
      %elif c.type == parser.FLOAT:
         %if c.size == 32:
            _mesa_unorm_to_float(src[${i}], 8);
         %elif c.size == 16:
            _mesa_unorm_to_half(src[${i}], 8);
         %else:
            <% assert False %>
         %endif
      %else:
         <% assert False %>
      %endif
   %endfor

   %if f.layout == parser.ARRAY:
      ${f.datatype()} *d = (${f.datatype()} *)dst;
      %for (i, c) in enumerate(f.channels):
         %if c.type == 'x':
            <% continue %>
         %endif
         d[${i}] = ${c.name};
      %endfor
   %elif f.layout == parser.PACKED:
      ${f.datatype()} d = 0;
      %for (i, c) in enumerate(f.channels):
         %if c.type == 'x':
            <% continue %>
         %endif
         d |= PACK(${c.name}, ${c.shift}, ${c.size});
      %endfor
      (*(${f.datatype()} *)dst) = d;
   %else:
      <% assert False %>
   %endif
}
%endfor

static inline void
pack_ubyte_r9g9b9e5_float(const uint8_t src[4], void *dst)
{
   uint32_t *d = (uint32_t *) dst;
   float rgb[3];
   rgb[0] = _mesa_unorm_to_float(src[0], 8);
   rgb[1] = _mesa_unorm_to_float(src[1], 8);
   rgb[2] = _mesa_unorm_to_float(src[2], 8);
   *d = float3_to_rgb9e5(rgb);
}

static inline void
pack_ubyte_r11g11b10_float(const uint8_t src[4], void *dst)
{
   uint32_t *d = (uint32_t *) dst;
   float rgb[3];
   rgb[0] = _mesa_unorm_to_float(src[0], 8);
   rgb[1] = _mesa_unorm_to_float(src[1], 8);
   rgb[2] = _mesa_unorm_to_float(src[2], 8);
   *d = float3_to_r11g11b10f(rgb);
}


/**
 * Pack a row of uint8_t rgba[4] values to the destination.
 */
void
_mesa_pack_ubyte_rgba_row(mesa_format format, uint32_t n,
                          const uint8_t src[][4], void *dst)
{
   uint32_t i;
   uint8_t *d = dst;

   switch (format) {
%for f in rgb_formats:
   %if f.is_compressed():
      <% continue %>
   %endif

   case ${f.name}:
      for (i = 0; i < n; ++i) {
         pack_ubyte_${f.short_name()}(src[i], d);
         d += ${f.block_size() // 8};
      }
      break;
%endfor
   default:
      assert(!"Invalid format");
   }
}


/**
 * Pack a 2D image of ubyte RGBA pixels in the given format.
 * \param srcRowStride  source image row stride in bytes
 * \param dstRowStride  destination image row stride in bytes
 */
void
_mesa_pack_ubyte_rgba_rect(mesa_format format, uint32_t width, uint32_t height,
                           const uint8_t *src, int32_t srcRowStride,
                           void *dst, int32_t dstRowStride)
{
   uint8_t *dstUB = dst;
   uint32_t i;

   if (srcRowStride == width * 4 * sizeof(uint8_t) &&
       dstRowStride == _mesa_format_row_stride(format, width)) {
      /* do whole image at once */
      _mesa_pack_ubyte_rgba_row(format, width * height,
                                (const uint8_t (*)[4]) src, dst);
   }
   else {
      /* row by row */
      for (i = 0; i < height; i++) {
         _mesa_pack_ubyte_rgba_row(format, width,
                                   (const uint8_t (*)[4]) src, dstUB);
         src += srcRowStride;
         dstUB += dstRowStride;
      }
   }
}


/** Helper struct for MESA_FORMAT_Z32_FLOAT_S8X24_UINT */
struct z32f_x24s8
{
   float z;
   uint32_t x24s8;
};


/**
 ** Pack uint Z pixels.  The incoming src value is always in
 ** the range [0, 2^32-1].
 **/

static void
pack_uint_S8_UINT_Z24_UNORM(const uint32_t *src, void *dst)
{
   /* don't disturb the stencil values */
   uint32_t *d = ((uint32_t *) dst);
   uint32_t s = *d & 0xff;
   uint32_t z = *src & 0xffffff00;
   *d = z | s;
}

static void
pack_uint_Z24_UNORM_S8_UINT(const uint32_t *src, void *dst)
{
   /* don't disturb the stencil values */
   uint32_t *d = ((uint32_t *) dst);
   uint32_t s = *d & 0xff000000;
   uint32_t z = *src >> 8;
   *d = s | z;
}

static void
pack_uint_Z_UNORM16(const uint32_t *src, void *dst)
{
   uint16_t *d = ((uint16_t *) dst);
   *d = *src >> 16;
}

static void
pack_uint_Z_UNORM32(const uint32_t *src, void *dst)
{
   uint32_t *d = ((uint32_t *) dst);
   *d = *src;
}

/**
 ** Pack uint to Z_FLOAT32 or Z_FLOAT32_X24S8.
 **/

static void
pack_uint_Z_FLOAT32(const uint32_t *src, void *dst)
{
   float *d = ((float *) dst);
   const double scale = 1.0 / (double) 0xffffffff;
   *d = (float) (*src * scale);
   assert(*d >= 0.0f);
   assert(*d <= 1.0f);
}

mesa_pack_uint_z_func
_mesa_get_pack_uint_z_func(mesa_format format)
{
   switch (format) {
   case MESA_FORMAT_S8_UINT_Z24_UNORM:
   case MESA_FORMAT_X8_UINT_Z24_UNORM:
      return pack_uint_S8_UINT_Z24_UNORM;
   case MESA_FORMAT_Z24_UNORM_S8_UINT:
   case MESA_FORMAT_Z24_UNORM_X8_UINT:
      return pack_uint_Z24_UNORM_S8_UINT;
   case MESA_FORMAT_Z_UNORM16:
      return pack_uint_Z_UNORM16;
   case MESA_FORMAT_Z_UNORM32:
      return pack_uint_Z_UNORM32;
   case MESA_FORMAT_Z_FLOAT32:
   case MESA_FORMAT_Z32_FLOAT_S8X24_UINT:
      return pack_uint_Z_FLOAT32;
   default:
      unreachable("unexpected format in _mesa_get_pack_uint_z_func()");
   }
}


/**
 ** Pack ubyte stencil pixels
 **/

static void
pack_ubyte_stencil_Z24_S8(const uint8_t *src, void *dst)
{
   /* don't disturb the Z values */
   uint32_t *d = ((uint32_t *) dst);
   uint32_t s = *src;
   uint32_t z = *d & 0xffffff00;
   *d = z | s;
}

static void
pack_ubyte_stencil_S8_Z24(const uint8_t *src, void *dst)
{
   /* don't disturb the Z values */
   uint32_t *d = ((uint32_t *) dst);
   uint32_t s = *src << 24;
   uint32_t z = *d & 0xffffff;
   *d = s | z;
}

static void
pack_ubyte_stencil_S8(const uint8_t *src, void *dst)
{
   uint8_t *d = (uint8_t *) dst;
   *d = *src;
}

static void
pack_ubyte_stencil_Z32_FLOAT_X24S8(const uint8_t *src, void *dst)
{
   float *d = ((float *) dst);
   d[1] = *src;
}


mesa_pack_ubyte_stencil_func
_mesa_get_pack_ubyte_stencil_func(mesa_format format)
{
   switch (format) {
   case MESA_FORMAT_S8_UINT_Z24_UNORM:
      return pack_ubyte_stencil_Z24_S8;
   case MESA_FORMAT_Z24_UNORM_S8_UINT:
      return pack_ubyte_stencil_S8_Z24;
   case MESA_FORMAT_S_UINT8:
      return pack_ubyte_stencil_S8;
   case MESA_FORMAT_Z32_FLOAT_S8X24_UINT:
      return pack_ubyte_stencil_Z32_FLOAT_X24S8;
   default:
      unreachable("unexpected format in _mesa_pack_ubyte_stencil_func()");
   }
}


"""

template = Template(string, future_imports=['division'])

print(template.render(argv=argv[0:]))

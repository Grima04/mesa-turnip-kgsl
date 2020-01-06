/*
 * Copyright (C) 2005-2007  Brian Paul   All Rights Reserved.
 * Copyright (C) 2008  VMware, Inc.   All Rights Reserved.
 * Copyright © 2010 Intel Corporation
 * Copyright © 2011 Bryan Cain
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

#include "st_glsl_types.h"

int
st_glsl_type_dword_size(const struct glsl_type *type, bool bindless)
{
   unsigned int size, i;

   switch (type->base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_BOOL:
      return type->components();
   case GLSL_TYPE_UINT16:
   case GLSL_TYPE_INT16:
   case GLSL_TYPE_FLOAT16:
      return DIV_ROUND_UP(type->components(), 2);
   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_INT8:
      return DIV_ROUND_UP(type->components(), 4);
   case GLSL_TYPE_IMAGE:
   case GLSL_TYPE_SAMPLER:
      if (!bindless)
         return 0;
   case GLSL_TYPE_DOUBLE:
   case GLSL_TYPE_UINT64:
   case GLSL_TYPE_INT64:
      return type->components() * 2;
   case GLSL_TYPE_ARRAY:
      return st_glsl_type_dword_size(type->fields.array, bindless) *
             type->length;
   case GLSL_TYPE_STRUCT:
      size = 0;
      for (i = 0; i < type->length; i++) {
         size += st_glsl_type_dword_size(type->fields.structure[i].type,
                                         bindless);
      }
      return size;
   case GLSL_TYPE_ATOMIC_UINT:
      return 0;
   case GLSL_TYPE_SUBROUTINE:
      return 1;
   case GLSL_TYPE_VOID:
   case GLSL_TYPE_ERROR:
   case GLSL_TYPE_INTERFACE:
   case GLSL_TYPE_FUNCTION:
   default:
      unreachable("invalid type in st_glsl_type_dword_size()");
   }

   return 0;
}

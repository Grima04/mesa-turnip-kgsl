/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
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
 * \file imports.h
 * Standard C library function wrappers.
 *
 * This file provides wrappers for all the standard C library functions
 * like malloc(), free(), printf(), getenv(), etc.
 */


#ifndef IMPORTS_H
#define IMPORTS_H


#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "util/compiler.h"
#include "util/bitscan.h"
#include "util/u_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * For GL_ARB_vertex_buffer_object we need to treat vertex array pointers
 * as offsets into buffer stores.  Since the vertex array pointer and
 * buffer store pointer are both pointers and we need to add them, we use
 * this macro.
 * Both pointers/offsets are expressed in bytes.
 */
#define ADD_POINTERS(A, B)  ( (uint8_t *) (A) + (uintptr_t) (B) )


/**
 * Sometimes we treat floats as ints.  On x86 systems, moving a float
 * as an int (thereby using integer registers instead of FP registers) is
 * a performance win.  Typically, this can be done with ordinary casts.
 * But with gcc's -fstrict-aliasing flag (which defaults to on in gcc 3.0)
 * these casts generate warnings.
 * The following union typedef is used to solve that.
 */
typedef union { float f; int i; unsigned u; } fi_type;



/*@}*/


/**********************************************************************
 * Functions
 */

extern int
_mesa_snprintf( char *str, size_t size, const char *fmt, ... ) PRINTFLIKE(3, 4);

extern int
_mesa_vsnprintf(char *str, size_t size, const char *fmt, va_list arg);


#ifdef __cplusplus
}
#endif


#endif /* IMPORTS_H */

/**
 * \file imports.c
 * Standard C library function wrappers.
 *
 * Imports are services which the device driver or window system or
 * operating system provides to the core renderer.  The core renderer (Mesa)
 * will call these functions in order to do memory allocation, simple I/O,
 * etc.
 *
 * Some drivers will want to override/replace this file with something
 * specialized, but that'll be rare.
 *
 * Eventually, I want to move roll the glheader.h file into this.
 *
 * \todo Functions still needed:
 * - scanf
 * - qsort
 * - rand and RAND_MAX
 */

/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.
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

#include <stdio.h>
#include <stdarg.h>
#include "c99_math.h"
#include "imports.h"

#ifdef _GNU_SOURCE
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#endif


#ifdef _WIN32
#define vsnprintf _vsnprintf
#elif defined(__IBMC__) || defined(__IBMCPP__)
extern int vsnprintf(char *str, size_t count, const char *fmt, va_list arg);
#endif

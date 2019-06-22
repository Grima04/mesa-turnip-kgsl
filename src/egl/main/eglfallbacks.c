/**************************************************************************
 *
 * Copyright 2010 LunarG, Inc.
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


#include <string.h>
#include "egltypedefs.h"
#include "egldriver.h"
#include "eglconfig.h"
#include "eglcontext.h"
#include "eglsurface.h"
#include "eglsync.h"


/**
 * Plug all the available fallback routines into the given driver's
 * dispatch table.
 */
void
_eglInitDriverFallbacks(_EGLDriver *drv)
{
   drv->API.QueryContext = _eglQueryContext;

   drv->API.QuerySurface = _eglQuerySurface;
   drv->API.SurfaceAttrib = _eglSurfaceAttrib;

   drv->API.SwapInterval = _eglSwapInterval;

   drv->API.GetSyncAttrib = _eglGetSyncAttrib;
}

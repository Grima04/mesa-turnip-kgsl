/*
 * Copyright 2014 Intel Corporation
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef EGL_DRI2_FALLBACKS_INCLUDED
#define EGL_DRI2_FALLBACKS_INCLUDED

#include "eglcurrent.h"
#include "egltypedefs.h"

struct wl_buffer;

static inline EGLBoolean
dri2_fallback_swap_buffers_region(_EGLDriver *drv, _EGLDisplay *disp,
                                  _EGLSurface *surf,
                                  EGLint numRects, const EGLint *rects)
{
   return EGL_FALSE;
}

static inline EGLBoolean
dri2_fallback_post_sub_buffer(_EGLDriver *drv, _EGLDisplay *disp,
                              _EGLSurface *draw,
                              EGLint x, EGLint y, EGLint width, EGLint height)
{
   return EGL_FALSE;
}

static inline EGLBoolean
dri2_fallback_copy_buffers(_EGLDriver *drv, _EGLDisplay *disp,
                           _EGLSurface *surf,
                           void *native_pixmap_target)
{
   return _eglError(EGL_BAD_NATIVE_PIXMAP, "no support for native pixmaps");
}

static inline EGLint
dri2_fallback_query_buffer_age(_EGLDriver *drv, _EGLDisplay *disp,
                               _EGLSurface *surf)
{
   return 0;
}

static inline struct wl_buffer*
dri2_fallback_create_wayland_buffer_from_image(_EGLDriver *drv,
                                               _EGLDisplay *disp,
                                               _EGLImage *img)
{
   return NULL;
}

static inline EGLBoolean
dri2_fallback_get_sync_values(_EGLDisplay *disp, _EGLSurface *surf,
                              EGLuint64KHR *ust, EGLuint64KHR *msc,
                              EGLuint64KHR *sbc)
{
   return EGL_FALSE;
}

#endif /* EGL_DRI2_FALLBACKS_INCLUDED */

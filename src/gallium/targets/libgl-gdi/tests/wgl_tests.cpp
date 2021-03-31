/*
 * Copyright © Microsoft Corporation
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

#include <windows.h>
#include <GL/gl.h>

class window
{
public:
   window(UINT width = 64, UINT height = 64);
   ~window();

   HWND get_hwnd() const { return _window; };
   HDC get_hdc() const { return _hdc; };
   bool valid() const { return _window && _hdc && _hglrc; }
   void show() {
      ShowWindow(_window, SW_SHOW);
   }

private:
   HWND _window = nullptr;
   HDC _hdc = nullptr;
   HGLRC _hglrc = nullptr;
};

window::window(uint32_t width, uint32_t height)
{
   _window = CreateWindowW(
      L"STATIC",
      L"OpenGLTestWindow",
      WS_OVERLAPPEDWINDOW,
      0,
      0,
      width,
      height,
      NULL,
      NULL,
      NULL,
      NULL
   );

   if (_window == nullptr)
      return;

   _hdc = ::GetDC(_window);

   PIXELFORMATDESCRIPTOR pfd = {
       sizeof(PIXELFORMATDESCRIPTOR),  /* size */
       1,                              /* version */
       PFD_SUPPORT_OPENGL |
       PFD_DRAW_TO_WINDOW |
       PFD_DOUBLEBUFFER,               /* support double-buffering */
       PFD_TYPE_RGBA,                  /* color type */
       8,                              /* prefered color depth */
       0, 0, 0, 0, 0, 0,               /* color bits (ignored) */
       0,                              /* no alpha buffer */
       0,                              /* alpha bits (ignored) */
       0,                              /* no accumulation buffer */
       0, 0, 0, 0,                     /* accum bits (ignored) */
       32,                             /* depth buffer */
       0,                              /* no stencil buffer */
       0,                              /* no auxiliary buffers */
       PFD_MAIN_PLANE,                 /* main layer */
       0,                              /* reserved */
       0, 0, 0,                        /* no layer, visible, damage masks */
   };
   int pixel_format = ChoosePixelFormat(_hdc, &pfd);
   if (pixel_format == 0)
      return;
   if (!SetPixelFormat(_hdc, pixel_format, &pfd))
      return;

   _hglrc = wglCreateContext(_hdc);
   if (!_hglrc)
      return;

   wglMakeCurrent(_hdc, _hglrc);
}

window::~window()
{
   if (_hglrc) {
      wglMakeCurrent(NULL, NULL);
      wglDeleteContext(_hglrc);
   }
   if (_hdc)
      ReleaseDC(_window, _hdc);
   if (_window)
      DestroyWindow(_window);
}

TEST(wgl, basic_create)
{
   window wnd;
   ASSERT_TRUE(wnd.valid());

   const char *version = (const char *)glGetString(GL_VERSION);
   ASSERT_NE(strstr(version, "Mesa"), nullptr);
}

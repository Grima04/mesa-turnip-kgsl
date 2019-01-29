# Copyright © 2017 Intel Corporation
# Copyright © 2018 Alyssa Rosenzweig

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

files_panfrost = files(
  'pan_public.h',
  'pan_screen.c',
  'pan_screen.h',
)

inc_panfrost = [
  inc_common,
  inc_gallium,
  inc_gallium_aux,
  inc_drm_uapi,
  inc_include,
  inc_src,
  include_directories('include')
]

libpanfrost = static_library(
  'panfrost',
  [files_panfrost],
  dependencies: [
    dep_thread,
    idep_nir
  ],
  include_directories : inc_panfrost,
  c_args : [c_vis_args, c_msvc_compat_args],
)

driver_panfrost = declare_dependency(
  compile_args : ['-DGALLIUM_PANFROST', '-Wno-pointer-arith'],
  link_with : [libpanfrost, libpanfrostwinsys],
)

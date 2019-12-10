# Copyright © 2019 Raspberry Pi
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

v3dv_entrypoints = custom_target(
  'v3dv_entrypoints.[ch]',
  input : ['v3dv_entrypoints_gen.py', vk_api_xml],
  output : ['v3dv_entrypoints.h', 'v3dv_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--outdir',
    meson.current_build_dir()
  ],
  depend_files : files('v3dv_extensions.py'),
)

v3dv_extensions_c = custom_target(
  'v3dv_extensions.c',
  input : ['v3dv_extensions_gen.py', vk_api_xml],
  output : 'v3dv_extensions.c',
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@',
    '--out-c', '@OUTPUT@',
  ],
  depend_files : files('v3dv_extensions.py'),
)

v3dv_extensions_h = custom_target(
  'v3dv_extensions.h',
  input : ['v3dv_extensions_gen.py', vk_api_xml],
  output : 'v3dv_extensions.h',
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@',
    '--out-h', '@OUTPUT@',
  ],
  depend_files : files('v3dv_extensions.py'),
)

libv3dv_files = files(
  'v3dv_bo.c',
  'v3dv_cmd_buffer.c',
  'v3dv_debug.c',
  'v3dv_debug.h',
  'v3dv_descriptor_set.c',
  'v3dv_device.c',
  'v3dv_formats.c',
  'v3dv_image.c',
  'v3dv_pass.c',
  'v3dv_pipeline.c',
  'v3dv_private.h',
  'v3dv_util.c',
  'v3d_tiling.c',
)

# The vulkan driver only supports version >= 42, which is the version present in
# Rpi4. We need to explicitly set it as we are reusing pieces from the GL v3d
# driver.
v3dv_flags = ['-DV3D_VERSION=42']

dep_v3dv3 = dependency('v3dv3', required : false)
if dep_v3dv3.found()
  v3dv_flags += '-DUSE_V3D_SIMULATOR'
endif

v3dv_deps = [
  dep_libdrm,
  dep_valgrind,
  dep_v3dv3,
  idep_vulkan_util,
]

libvulkan_broadcom = shared_library(
  'vulkan_broadcom',
  [libv3dv_files, v3dv_entrypoints, v3dv_extensions_c, v3dv_extensions_h, sha1_h],
  include_directories : [
    inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux, inc_broadcom, inc_compiler, inc_util,
  ],
  link_with : [
    libbroadcom_v3d,
    # For glsl_type_singleton_init_or_ref:
    libglsl,
  ],
  dependencies : v3dv_deps,
  c_args : v3dv_flags,
  link_args : ['-Wl,--build-id=sha1', ld_args_bsymbolic, ld_args_gc_sections],
  gnu_symbol_visibility : 'hidden',
  install : true,
)

if with_symbols_check
  test(
    'v3dv symbols check',
    symbols_check,
    args : [
      '--lib', libvulkan_broadcom,
      '--symbols-file', vulkan_icd_symbols,
      symbols_check_args,
    ],
    suite : ['broadcom'],
  )
endif

broadcom_icd = custom_target(
  'broadcom_icd',
  input : 'v3dv_icd.py',
  output : 'broadcom_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir')),
    '--out', '@OUTPUT@',
  ],
  depend_files : files('v3dv_extensions.py'),
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

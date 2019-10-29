# Copyright 2019 Google LLC
# SPDX-License-Identifier: MIT
#
# based in part on anv and radv which are:
# Copyright © 2017 Intel Corporation

vn_entrypoints = custom_target(
  'vn_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['vn_entrypoints.h', 'vn_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'vn',
  ],
)

virtio_icd = custom_target(
  'virtio_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'virtio_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.2', '--xml', '@INPUT1@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir'),
                             'libvulkan_virtio.so'),
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

libvn_files = files(
  'vn_common.c',
  'vn_device.c',
  'vn_icd.c',
  'vn_renderer_virtgpu.c',
  'vn_renderer_vtest.c',
)

vn_deps = [
  dep_libdrm,
  dep_thread,
  idep_mesautil,
  idep_vulkan_util,
  idep_xmlconfig,
]

vn_flags = [
  no_override_init_args,
]

libvulkan_virtio = shared_library(
  'vulkan_virtio',
  [libvn_files, vn_entrypoints, sha1_h],
  include_directories : [
    inc_include, inc_src, inc_vulkan_wsi, inc_virtio,
  ],
  link_with : [libvulkan_wsi],
  dependencies : [vn_deps],
  c_args : [vn_flags],
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  gnu_symbol_visibility : 'hidden',
  install : true,
)

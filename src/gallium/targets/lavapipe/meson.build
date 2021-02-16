libvulkan_lvp = shared_library(
  'vulkan_lvp',
  [ 'target.c' ],
  include_directories : [ inc_src, inc_util, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_winsys, inc_gallium_drivers ],
  link_whole : [ liblavapipe_st ],
  link_with : [libpipe_loader_static, libgallium, libwsw, libswdri, libws_null, libswkmsdri ],
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  dependencies : driver_swrast,
  install : true,
)

if with_platform_windows
  module_dir = join_paths(get_option('prefix'), get_option('bindir'))
else
  module_dir = join_paths(get_option('prefix'), get_option('libdir'))
endif

lvp_icd = custom_target(
  'lvp_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'lvp_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.1', '--xml', '@INPUT1@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir'),
                             'libvulkan_lvp.so'),
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

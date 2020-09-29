libvulkan_lvp = shared_library(
  'vulkan_lvp',
  [ 'target.c' ],
  include_directories : [ inc_src, inc_util, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_winsys, inc_gallium_drivers ],
  link_whole : [ liblavapipe_st ],
  link_with : [libpipe_loader_static, libmegadriver_stub, libdri, libdricommon ,libgallium, libwsw, libswdri, libws_null, libswkmsdri ],
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  dependencies : driver_swrast,
  install : true,
  name_suffix : 'so',
)

lvp_icd = custom_target(
  'lvp_icd',
  input : 'lvp_icd.py',
  output : 'lvp_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir')),
    '--out', '@OUTPUT@',
  ],
  depend_files : files('../../frontends/lavapipe/lvp_extensions.py'),
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

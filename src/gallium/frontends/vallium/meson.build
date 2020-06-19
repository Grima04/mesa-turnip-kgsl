
val_entrypoints = custom_target(
  'val_entrypoints.[ch]',
  input : ['val_entrypoints_gen.py', vk_api_xml],
  output : ['val_entrypoints.h', 'val_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--outdir',
    meson.current_build_dir()
  ],
  depend_files : files('val_extensions.py'),
)

val_extensions_c = custom_target(
  'val_extensions.c',
  input : ['val_extensions.py', vk_api_xml],
  output : ['val_extensions.c', 'val_extensions.h'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--out-c', '@OUTPUT0@',
    '--out-h', '@OUTPUT1@'
  ],
)

libval_files = files(
    'val_device.c',
    'val_cmd_buffer.c',
    'val_descriptor_set.c',
    'val_execute.c',
    'val_util.c',
    'val_image.c',
    'val_formats.c',
    'val_lower_vulkan_resource.c',
    'val_lower_vulkan_resource.h',
    'val_lower_input_attachments.c',
    'val_pass.c',
    'val_pipeline.c',
    'val_pipeline_cache.c',
    'val_query.c',
    'val_wsi.c')

val_deps = []
val_flags = []

if with_platform_x11
  val_deps += dep_xcb_dri3
  val_flags += [
    '-DVK_USE_PLATFORM_XCB_KHR',
    '-DVK_USE_PLATFORM_XLIB_KHR',
  ]
  libval_files += files('val_wsi_x11.c')
endif

if with_platform_wayland
  val_deps += dep_wayland_client
  val_flags += '-DVK_USE_PLATFORM_WAYLAND_KHR'
  libval_files += files('val_wsi_wayland.c')
endif

libvallium_st = static_library(
  'vallium_st',
  [libval_files, val_entrypoints, val_extensions_c ],
  link_with : [ libvulkan_wsi ],
  c_args : [ val_flags ],
  gnu_symbol_visibility : 'hidden',
  include_directories : [ inc_include, inc_src, inc_util, inc_gallium, inc_compiler, inc_gallium_aux, inc_vulkan_wsi ],
  dependencies : [ idep_nir, idep_mesautil, idep_vulkan_util ]
)

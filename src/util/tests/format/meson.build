foreach t : ['srgb']
  test(t,
    executable(
      t,
      '@0@.c'.format(t),
      include_directories : inc_common,
      dependencies : idep_mesautil,
    ),
    suite : 'format',
    should_fail : meson.get_cross_property('xfail', '').contains(t),
  )
endforeach

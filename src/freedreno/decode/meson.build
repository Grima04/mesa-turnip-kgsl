# Copyright © 2020 Google, Inc

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

dep_lua = dependency('lua53', required: false)
if not dep_lua.found()
  dep_lua = dependency('lua52', required: false)
endif
if not dep_lua.found()
  dep_lua = dependency('lua', required: false)
endif

dep_libarchive = dependency('libarchive', required: false)

# Shared cmdstream decoding:
libfreedreno_cffdec = static_library(
  'freedreno_cffdec',
  [
    'buffers.c',
    'buffers.h',
    'cffdec.c',
    'cffdec.h',
    'disasm-a2xx.c',
    'disasm-a3xx.c',
    'disasm.h',
    'instr-a2xx.h',
    'instr-a3xx.h',
    'pager.c',
    'pager.h',
    'rnnutil.c',
    'rnnutil.h',
    'util.h',
  ],
  include_directories: [
    inc_freedreno_rnn,
    inc_include,
    inc_src,
  ],
  c_args : [ no_override_init_args ],
  gnu_symbol_visibility: 'hidden',
  dependencies: [],
  link_with: libfreedreno_rnn,
  build_by_default: false,
)

if dep_libarchive.found()
  libfreedreno_io = static_library(
    'libfreedreno_io',
    [
      'io.c',
      'io.h',
    ],
    include_directories: [],
    c_args : [no_override_init_args],
    gnu_symbol_visibility: 'hidden',
    dependencies: [
      dep_libarchive,
    ],
    build_by_default: false,
  )
endif

if dep_lua.found() and dep_libarchive.found()
  cffdump = executable(
    'cffdump',
    [
      'cffdump.c',
      'script.c',
      'script.h'
    ],
    include_directories: [
      inc_freedreno_rnn,
      inc_include,
      inc_src,
    ],
    c_args : [no_override_init_args],
    gnu_symbol_visibility: 'hidden',
    dependencies: [
      dep_lua,
    ],
    link_with: [
      libfreedreno_cffdec,
      libfreedreno_io,
    ],
    build_by_default: with_tools.contains('freedreno'),
    install : with_tools.contains('freedreno'),
  )
endif

crashdec = executable(
  'crashdec',
  'crashdec.c',
  include_directories: [
    inc_freedreno_rnn,
    inc_include,
    inc_src,
  ],
  gnu_symbol_visibility: 'hidden',
  dependencies: [],
  link_with: [
    libfreedreno_cffdec,
  ],
  build_by_default: with_tools.contains('freedreno'),
  install : with_tools.contains('freedreno'),
)

if dep_libarchive.found()
  pgmdump = executable(
    'pgmdump',
    'pgmdump.c',
    include_directories: [
      inc_include,
      inc_src,
    ],
    gnu_symbol_visibility: 'hidden',
    dependencies: [],
    link_with: [
      libfreedreno_cffdec,
      libfreedreno_io,
    ],
    build_by_default: with_tools.contains('freedreno'),
    install: false,
  )
  pgmdump2 = executable(
    'pgmdump2',
    'pgmdump2.c',
    include_directories: [
      inc_include,
      inc_src,
    ],
    gnu_symbol_visibility: 'hidden',
    dependencies: [],
    link_with: [
      libfreedreno_cffdec,
      libfreedreno_io,
    ],
    build_by_default: with_tools.contains('freedreno'),
    install: false,
  )
endif

# Copyright © 2020 Valve Corporation

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
aco_tests_files = files(
  'framework.h',
  'helpers.cpp',
  'helpers.h',
  'main.cpp',
  'test_assembler.cpp',
  'test_optimizer.cpp',
  'test_tests.cpp',
)

test(
  'aco_tests',
  executable(
    'aco_tests',
    aco_tests_files,
    cpp_args : ['-DACO_TEST_SOURCE_DIR="@0@"'.format(meson.current_source_dir()),
                '-DACO_TEST_BUILD_ROOT="@0@"'.format(meson.build_root()),
                '-DACO_TEST_PYTHON_BIN="@0@"'.format(prog_python.path())],
    include_directories : [
      inc_include, inc_src, inc_gallium, inc_compiler, inc_mesa, inc_mapi, inc_amd, inc_amd_common, inc_amd_common_llvm,
    ],
    link_with : [
      libamd_common, libamd_common_llvm
    ],
    dependencies : [
      dep_llvm, dep_thread, idep_aco, idep_nir, idep_mesautil
    ],
    gnu_symbol_visibility : 'hidden',
    build_by_default : true,
  ),
  suite : ['amd', 'compiler'],
)

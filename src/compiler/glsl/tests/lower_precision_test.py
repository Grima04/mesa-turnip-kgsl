# encoding=utf-8
# Copyright © 2019 Google

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

from __future__ import print_function
import sys
import subprocess
import tempfile
import re
from collections import namedtuple


Test = namedtuple("Test", "name source match_re")


TESTS = [
    Test("f32 simple division",
         """
         uniform mediump float a, b;

         void main()
         {
                 gl_FragColor.rgba = vec4(a / b);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 simple division",
         """
         #version 300 es
         precision mediump float;
         precision mediump int;
         uniform mediump int a, b;

         out vec4 color;

         void main()
         {
                 color = vec4(a / b);
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 simple division",
         """
         #version 300 es
         precision mediump float;
         precision mediump int;
         uniform mediump uint a, b;

         out vec4 color;

         void main()
         {
                 color = vec4(a / b);
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("dot",
         """
         uniform mediump vec2 a, b;

         void main()
         {
                 gl_FragColor.rgba = vec4(dot(a, b));
         }
         """,
         r'\(expression +float16_t +dot\b'),
    Test("f32 array with const index",
         """
         precision mediump float;

         uniform float in_simple[2];

         void main()
         {
                 gl_FragColor = vec4(in_simple[0] / in_simple[1]);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 array with const index",
         """
         #version 300 es
         precision mediump float;
         precision mediump int;

         uniform int in_simple[2];

         out vec4 color;

         void main()
         {
                 color = vec4(in_simple[0] / in_simple[1]);
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 array with const index",
         """
         #version 300 es
         precision mediump float;
         precision mediump int;

         uniform uint in_simple[2];

         out vec4 color;

         void main()
         {
                 color = vec4(in_simple[0] / in_simple[1]);
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 array with uniform index",
         """
         precision mediump float;

         uniform float in_simple[2];
         uniform int i0, i1;

         void main()
         {
                 gl_FragColor = vec4(in_simple[i0] / in_simple[i1]);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 array with uniform index",
         """
         #version 300 es
         precision mediump float;
         precision mediump int;

         uniform int in_simple[2];
         uniform int i0, i1;

         out vec4 color;

         void main()
         {
                 color = vec4(in_simple[i0] / in_simple[i1]);
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 array with uniform index",
         """
         #version 300 es
         precision mediump float;
         precision mediump int;

         uniform uint in_simple[2];
         uniform int i0, i1;

         out vec4 color;

         void main()
         {
                 color = vec4(in_simple[i0] / in_simple[i1]);
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 array-of-array with const index",
         """
         #version 310 es
         precision mediump float;

         uniform float in_aoa[2][2];

         layout(location = 0) out float out_color;

         void main()
         {
                 out_color = in_aoa[0][0] / in_aoa[1][1];
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 array-of-array with const index",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform int in_aoa[2][2];

         layout(location = 0) out highp int out_color;

         void main()
         {
                 out_color = in_aoa[0][0] / in_aoa[1][1];
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 array-of-array with const index",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform uint in_aoa[2][2];

         layout(location = 0) out highp uint out_color;

         void main()
         {
                 out_color = in_aoa[0][0] / in_aoa[1][1];
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 array-of-array with uniform index",
         """
         #version 310 es
         precision mediump float;

         uniform float in_aoa[2][2];
         uniform int i0, i1;

         layout(location = 0) out float out_color;

         void main()
         {
                 out_color = in_aoa[i0][i0] / in_aoa[i1][i1];
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 array-of-array with uniform index",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform int in_aoa[2][2];
         uniform int i0, i1;

         layout(location = 0) out highp int out_color;

         void main()
         {
                 out_color = in_aoa[i0][i0] / in_aoa[i1][i1];
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 array-of-array with uniform index",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform uint in_aoa[2][2];
         uniform int i0, i1;

         layout(location = 0) out highp uint out_color;

         void main()
         {
                 out_color = in_aoa[i0][i0] / in_aoa[i1][i1];
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 array index",
         """
         uniform mediump float a, b;
         uniform mediump float values[2];

         void main()
         {
                 gl_FragColor.rgba = vec4(values[int(a / b)]);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 array index",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform mediump int a, b;
         uniform mediump int values[2];

         out highp int color;

         void main()
         {
                 color = values[a / b];
         }
         """,
         r'\(expression +int16_t +/'),
    Test("f32 function",
         """
         precision mediump float;

         uniform float a, b;

         mediump float
         get_a()
         {
                 return a;
         }

         float
         get_b()
         {
                 return b;
         }

         void main()
         {
                 gl_FragColor = vec4(get_a() / get_b());
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 function",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform int a, b;

         mediump int
         get_a()
         {
                 return a;
         }

         int
         get_b()
         {
                 return b;
         }

         out highp int color;

         void main()
         {
                 color = get_a() / get_b();
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 function",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform uint a, b;

         mediump uint
         get_a()
         {
                 return a;
         }

         uint
         get_b()
         {
                 return b;
         }

         out highp uint color;

         void main()
         {
                 color = get_a() / get_b();
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 function mediump args",
         """
         precision mediump float;

         uniform float a, b;

         mediump float
         do_div(float x, float y)
         {
                 return x / y;
         }

         void main()
         {
                 gl_FragColor = vec4(do_div(a, b));
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 function mediump args",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform int a, b;

         mediump int
         do_div(int x, int y)
         {
                 return x / y;
         }

         out highp int color;

         void main()
         {
                 color = do_div(a, b);
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 function mediump args",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform uint a, b;

         mediump uint
         do_div(uint x, uint y)
         {
                 return x / y;
         }

         out highp uint color;

         void main()
         {
                 color = do_div(a, b);
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 function highp args",
         """
         precision mediump float;

         uniform float a, b;

         mediump float
         do_div(highp float x, highp float y)
         {
                 return x / y;
         }

         void main()
         {
                 gl_FragColor = vec4(do_div(a, b));
         }
         """,
         r'\(expression +float +/'),
    Test("i32 function highp args",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform int a, b;

         mediump int
         do_div(highp int x, highp int y)
         {
                 return x / y;
         }

         out highp int color;

         void main()
         {
                  color = do_div(a, b);
         }
         """,
         r'\(expression +int +/'),
    Test("u32 function highp args",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform uint a, b;

         mediump uint
         do_div(highp uint x, highp uint y)
         {
                 return x / y;
         }

         out highp uint color;

         void main()
         {
                  color = do_div(a, b);
         }
         """,
         r'\(expression +uint +/'),
    Test("f32 function inout different precision highp",
         """
         uniform mediump float a, b;

         void
         do_div(inout highp float x, highp float y)
         {
                 x = x / y;
         }

         void main()
         {
                 mediump float temp = a;
                 do_div(temp, b);
                 gl_FragColor = vec4(temp);
         }
         """,
         r'\(expression +float +/'),
    Test("i32 function inout different precision highp",
         """
         #version 310 es
         uniform mediump int a, b;

         void
         do_div(inout highp int x, highp int y)
         {
                 x = x / y;
         }

         out mediump int color;

         void main()
         {
                 mediump int temp = a;
                 do_div(temp, b);
                 color = temp;
         }
         """,
         r'\(expression +int +/'),
    Test("u32 function inout different precision highp",
         """
         #version 310 es
         uniform mediump uint a, b;

         void
         do_div(inout highp uint x, highp uint y)
         {
                 x = x / y;
         }

         out mediump uint color;

         void main()
         {
                 mediump uint temp = a;
                 do_div(temp, b);
                 color = temp;
         }
         """,
         r'\(expression +uint +/'),
    Test("f32 function inout different precision mediump",
         """
         uniform highp float a, b;

         void
         do_div(inout mediump float x, mediump float y)
         {
                 x = x / y;
         }

         void main()
         {
                 highp float temp = a;
                 do_div(temp, b);
                 gl_FragColor = vec4(temp);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 function inout different precision mediump",
         """
         #version 310 es
         uniform highp int a, b;

         out highp int color;

         void
         do_div(inout mediump int x, mediump int y)
         {
                 x = x / y;
         }

         void main()
         {
                 highp int temp = a;
                 do_div(temp, b);
                 color = temp;
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 function inout different precision mediump",
         """
         #version 310 es
         uniform highp uint a, b;

         out highp uint color;

         void
         do_div(inout mediump uint x, mediump uint y)
         {
                 x = x / y;
         }

         void main()
         {
                 highp uint temp = a;
                 do_div(temp, b);
                 color = temp;
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 if",
         """
         precision mediump float;

         uniform float a, b;

         void
         main()
         {
                 if (a / b < 0.31)
                         gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
                 else
                         gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 if",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform int a, b;

         out vec4 color;

         void
         main()
         {
                 if (a / b < 10)
                         color = vec4(0.0, 1.0, 0.0, 1.0);
                 else
                         color = vec4(1.0, 0.0, 0.0, 1.0);
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 if",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform uint a, b;

         out vec4 color;

         void
         main()
         {
                 if (a / b < 10u)
                         color = vec4(0.0, 1.0, 0.0, 1.0);
                 else
                         color = vec4(1.0, 0.0, 0.0, 1.0);
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("matrix",
         """
         precision mediump float;

         uniform vec2 a;
         uniform mat2 b;

         void main()
         {
             gl_FragColor = vec4(b * a, 0.0, 0.0);
         }
         """,
         r'\(expression +f16vec2 \*.*\bf16mat2\b'),
    Test("f32 simple struct deref",
         """
         precision mediump float;

         struct simple {
                 float a, b;
         };

         uniform simple in_simple;

         void main()
         {
                 gl_FragColor = vec4(in_simple.a / in_simple.b);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 simple struct deref",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         struct simple {
                 int a, b;
         };

         uniform simple in_simple;

         out highp int color;

         void main()
         {
                 color = in_simple.a / in_simple.b;
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 simple struct deref",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         struct simple {
                 uint a, b;
         };

         uniform simple in_simple;

         out highp uint color;

         void main()
         {
                 color = in_simple.a / in_simple.b;
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 embedded struct deref",
         """
         precision mediump float;

         struct simple {
                 float a, b;
         };

         struct embedded {
                 simple a, b;
         };

         uniform embedded in_embedded;

         void main()
         {
                 gl_FragColor = vec4(in_embedded.a.a / in_embedded.b.b);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 embedded struct deref",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         struct simple {
                 int a, b;
         };

         struct embedded {
                 simple a, b;
         };

         uniform embedded in_embedded;

         out highp int color;

         void main()
         {
                 color = in_embedded.a.a / in_embedded.b.b;
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 embedded struct deref",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         struct simple {
                 uint a, b;
         };

         struct embedded {
                 simple a, b;
         };

         uniform embedded in_embedded;

         out highp uint color;

         void main()
         {
                 color = in_embedded.a.a / in_embedded.b.b;
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 arrayed struct deref",
         """
         precision mediump float;

         struct simple {
                 float a, b;
         };

         struct arrayed {
                 simple a[2];
         };

         uniform arrayed in_arrayed;

         void main()
         {
                 gl_FragColor = vec4(in_arrayed.a[0].a / in_arrayed.a[1].b);
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 arrayed struct deref",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         struct simple {
                 int a, b;
         };

         struct arrayed {
                 simple a[2];
         };

         uniform arrayed in_arrayed;

         out highp int color;

         void main()
         {
                 color = in_arrayed.a[0].a / in_arrayed.a[1].b;
         }
         """,
         r'\(expression +int16_t +/'),
    Test("u32 arrayed struct deref",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         struct simple {
                 uint a, b;
         };

         struct arrayed {
                 simple a[2];
         };

         uniform arrayed in_arrayed;

         out highp uint color;

         void main()
         {
                 color = in_arrayed.a[0].a / in_arrayed.a[1].b;
         }
         """,
         r'\(expression +uint16_t +/'),
    Test("f32 mixed precision not lowered",
         """
         uniform mediump float a;
         uniform highp float b;

         void main()
         {
                 gl_FragColor = vec4(a / b);
         }
         """,
         r'\(expression +float +/'),
    Test("i32 mixed precision not lowered",
         """
         #version 310 es
         uniform mediump int a;
         uniform highp int b;

         out mediump int color;

         void main()
         {
                 color = a / b;
         }
         """,
         r'\(expression +int +/'),
    Test("u32 mixed precision not lowered",
         """
         #version 310 es
         uniform mediump uint a;
         uniform highp uint b;

         out mediump uint color;

         void main()
         {
                 color = a / b;
         }
         """,
         r'\(expression +uint +/'),
    Test("f32 texture sample",
         """
         precision mediump float;

         uniform sampler2D tex;
         uniform vec2 coord;
         uniform float divisor;

         void main()
         {
                 gl_FragColor = texture2D(tex, coord) / divisor;
         }
         """,
         r'\(expression +f16vec4 +/.*\(tex +f16vec4 +'),
    Test("i32 texture sample",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform mediump isampler2D tex;
         uniform vec2 coord;
         uniform int divisor;

         out highp ivec4 color;

         void main()
         {
                 color = texture(tex, coord) / divisor;
         }
         """,
         r'\(expression +i16vec4 +/.*\(tex +i16vec4 +'),
    Test("u32 texture sample",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform mediump usampler2D tex;
         uniform vec2 coord;
         uniform uint divisor;

         out highp uvec4 color;

         void main()
         {
                 color = texture(tex, coord) / divisor;
         }
         """,
         r'\(expression +u16vec4 +/.*\(tex +u16vec4 +'),
    Test("f32 image array",
         """
         #version 320 es
         precision mediump float;

         layout(rgba16f) readonly uniform mediump image2D img[2];
         // highp shouldn't affect the return value of imageLoad
         uniform highp ivec2 coord;
         uniform float divisor;

         out highp vec4 color;

         void main()
         {
                 color = imageLoad(img[1], coord) / divisor;
         }
         """,
         r'\(expression +f16vec4 +/'),
    Test("f32 image load",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         layout(rgba16f) readonly uniform mediump image2D img;
         uniform ivec2 coord;
         uniform float divisor;

         out highp vec4 color;

         void main()
         {
                 color = imageLoad(img, coord) / divisor;
         }
         """,
         r'\(expression +f16vec4 +/'),
    Test("i32 image load",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         layout(rgba16i) readonly uniform mediump iimage2D img;
         uniform ivec2 coord;
         uniform int divisor;

         out highp ivec4 color;

         void main()
         {
                 color = imageLoad(img, coord) / divisor;
         }
         """,
         r'\(expression +i16vec4 +/'),
    Test("u32 image load",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         layout(rgba16ui) readonly uniform mediump uimage2D img;
         uniform ivec2 coord;
         uniform uint divisor;

         out highp uvec4 color;

         void main()
         {
                 color = imageLoad(img, coord) / divisor;
         }
         """,
         r'\(expression +u16vec4 +/'),
    Test("f32 expression in lvalue",
         """
         uniform mediump float a, b;

         void main()
         {
                 gl_FragColor = vec4(1.0);
                 gl_FragColor[int(a / b)] = 0.5;
         }
         """,
         r'\(expression +float16_t +/'),
    Test("i32 expression in lvalue",
         """
         #version 310 es
         precision mediump float;
         precision mediump int;

         uniform mediump int a, b;

         out vec4 color;

         void main()
         {
                 color = vec4(1.0);
                 color[a / b] = 0.5;
         }
         """,
         r'\(expression +int16_t +/'),
    Test("f32 builtin with const arg",
         """
         uniform mediump float a;

         void main()
         {
                 gl_FragColor = vec4(min(a, 3.0));
         }
         """,
         r'\(expression +float16_t min'),
    Test("i32 builtin with const arg",
         """
         #version 310 es
         uniform mediump int a;

         out highp int color;

         void main()
         {
                 color = min(a, 3);
         }
         """,
         r'\(expression +int16_t min'),
    Test("u32 builtin with const arg",
         """
         #version 310 es
         uniform mediump uint a;

         out highp uint color;

         void main()
         {
                 color = min(a, 3u);
         }
         """,
         r'\(expression +uint16_t min'),
    Test("dFdx",
         """
         #version 300 es
         precision mediump float;

         in vec4 var;
         out vec4 color;

         void main()
         {
                 color = dFdx(var);
         }
         """,
         r'\(expression +f16vec4 +dFdx +\(expression +f16vec4'),
    Test("dFdy",
         """
         #version 300 es
         precision mediump float;

         in vec4 var;
         out vec4 color;

         void main()
         {
                 color = dFdy(var);
         }
         """,
         r'\(expression +f16vec4 +dFdy +\(expression +f16vec4'),
]


def compile_shader(standalone_compiler, source):
    with tempfile.NamedTemporaryFile(mode='wt', suffix='.frag') as source_file:
        print(source, file=source_file)
        source_file.flush()
        return subprocess.check_output([standalone_compiler,
                                        '--version', '300',
                                        '--lower-precision',
                                        '--dump-lir',
                                        source_file.name],
                                       universal_newlines=True)


def run_test(standalone_compiler, test):
    ir = compile_shader(standalone_compiler, test.source)

    if re.search(test.match_re, ir) is None:
        print(ir)
        return False

    return True


def main():
    standalone_compiler = sys.argv[1]
    passed = 0

    for test in TESTS:
        print('Testing {} ... '.format(test.name), end='')

        result = run_test(standalone_compiler, test)

        if result:
            print('PASS')
            passed += 1
        else:
            print('FAIL')

    print('{}/{} tests returned correct results'.format(passed, len(TESTS)))
    sys.exit(0 if passed == len(TESTS) else 1)


if __name__ == '__main__':
    main()

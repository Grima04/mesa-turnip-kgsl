/*
 * Copyright (C) 2019 Collabora
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

/* Mali GPUs are tiled-mode renderers, rather than immediate-mode.
 * Conceptually, the screen is divided into 16x16 tiles. Vertex shaders run.
 * Then, a fixed-function hardware block (the tiler) consumes the gl_Position
 * results. For each triangle specified, it marks each containing tile as
 * containing that triangle. This set of "triangles per tile" form the "polygon
 * list". Finally, the rasterization unit consumes the polygon list to invoke
 * the fragment shader.
 *
 * In practice, it's a bit more complicated than this. 16x16 is the logical
 * tile size, but Midgard features "hierarchical tiling", where power-of-two
 * multiples of the base tile size can be used: hierarchy level 0 (16x16),
 * level 1 (32x32), level 2 (64x64), per public information about Midgard's
 * tiling. In fact, tiling goes up to 2048x2048 (!), although in practice
 * 128x128 is the largest usually used (though higher modes are enabled).  The
 * idea behind hierarchical tiling is to use low tiling levels for small
 * triangles and high levels for large triangles, to minimize memory bandwidth
 * and repeated fragment shader invocations (the former issue inherent to
 * immediate-mode rendering and the latter common in traditional tilers).
 *
 * The tiler itself works by reading varyings in and writing a polygon list
 * out. Unfortunately (for us), both of these buffers are managed in main
 * memory; although they ideally will be cached, it is the drivers'
 * responsibility to allocate these buffers. Varying buffe allocation is
 * handled elsewhere, as it is not tiler specific; the real issue is allocating
 * the polygon list.
 *
 * This is hard, because from the driver's perspective, we have no information
 * about what geometry will actually look like on screen; that information is
 * only gained from running the vertex shader. (Theoretically, we could run the
 * vertex shaders in software as a prepass, or in hardware with transform
 * feedback as a prepass, but either idea is ludicrous on so many levels).
 *
 * Instead, Mali uses a bit of a hybrid approach, splitting the polygon list
 * into three distinct pieces. First, the driver statically determines which
 * tile hierarchy levels to use (more on that later). At this point, we know the
 * framebuffer dimensions and all the possible tilings of the framebuffer, so
 * we know exactly how many tiles exist across all hierarchy levels. The first
 * piece of the polygon list is the header, which is exactly 8 bytes per tile,
 * plus padding and a small 64-byte prologue. (If that doesn't remind you of
 * AFBC, it should. See pan_afbc.c for some fun parallels). The next part is
 * the polygon list body, which seems to contain 512 bytes per tile, again
 * across every level of the hierarchy. These two parts form the polygon list
 * buffer. This buffer has a statically determinable size, approximately equal
 * to the # of tiles across all hierarchy levels * (8 bytes + 512 bytes), plus
 * alignment / minimum restrictions / etc.
 *
 * The third piece is the easy one (for us): the tiler heap. In essence, the
 * tiler heap is a gigantic slab that's as big as could possibly be necessary
 * in the worst case imaginable. Just... a gigantic allocation that we give a
 * start and end pointer to. What's the catch? The tiler heap is lazily
 * allocated; that is, a huge amount of memory is _reserved_, but only a tiny
 * bit is actually allocated upfront. The GPU just keeps using the
 * unallocated-but-reserved portions as it goes along, generating page faults
 * if it goes beyond the allocation, and then the kernel is instructed to
 * expand the allocation on page fault (known in the vendor kernel as growable
 * memory). This is quite a bit of bookkeeping of its own, but that task is
 * pushed to kernel space and we can mostly ignore it here, just remembering to
 * set the GROWABLE flag so the kernel actually uses this path rather than
 * allocating a gigantic amount up front and burning a hole in RAM.
 */

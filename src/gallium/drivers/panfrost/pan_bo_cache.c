/*
 * Copyright 2019 Collabora, Ltd.
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
 * Authors (Collabora):
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#include "pan_screen.h"

struct panfrost_bo *
panfrost_bo_cache_fetch(
                struct panfrost_screen *screen,
                size_t size, uint32_t flags)
{
        /* Stub */
        return NULL;
}

/* Tries to add a BO to the cache. Returns if it was
 * successful */

bool
panfrost_bo_cache_put(
                struct panfrost_screen *screen,
                struct panfrost_bo *bo)
{
        /* Stub */
        return false;
}

/* Evicts all BOs from the cache. Called during context
 * destroy or during low-memory situations (to free up
 * memory that may be unused by us just sitting in our
 * cache, but still reserved from the perspective of the
 * OS) */

void
panfrost_bo_cache_evict_all(
                struct panfrost_screen *screen)
{
        /* Stub */
        return;
}


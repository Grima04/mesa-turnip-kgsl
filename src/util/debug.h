/*
 * Copyright © 2015 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef _UTIL_DEBUG_H
#define _UTIL_DEBUG_H

#include <stdint.h>
#include <stdbool.h>

#include "util/simple_mtx.h"

#ifdef __cplusplus
extern "C" {
#endif

struct debug_control {
    const char * string;
    uint64_t     flag;
};

uint64_t
parse_debug_string(const char *debug,
                   const struct debug_control *control);
bool
comma_separated_list_contains(const char *list, const char *s);
bool
env_var_as_boolean(const char *var_name, bool default_value);
unsigned
env_var_as_unsigned(const char *var_name, unsigned default_value);

#ifdef __cplusplus
} /* extern C */
#endif

/**
 * Helper for arbitrary one-time initialization, with additional locking
 * to ensure the initialization only happens once (and to make tools like
 * helgrind happy).
 */
#define do_once for( \
      struct __do_once_data *__d = ({                 \
         static struct __do_once_data __sd = {        \
               .lock = _SIMPLE_MTX_INITIALIZER_NP,    \
               .done = false,                         \
         };                                           \
         simple_mtx_lock(&__sd.lock);                 \
         &__sd;                                       \
      });                                             \
      ({                                              \
         if (__d->done)                               \
            simple_mtx_unlock(&__d->lock);            \
         !__d->done;                                  \
      });                                             \
      __d->done = true)

struct __do_once_data {
   simple_mtx_t lock;
   bool done;
};

/**
 * Helper for one-time debug value from env-var, and other similar cases,
 * where the expression is expected to return the same value each time.
 *
 * This has additional locking, compared to open-coding the initialization,
 * to make tools like helgrind happy.
 */
#define get_once(__expr) ({                           \
      static __typeof__(__expr) __val;                \
      do_once {                                       \
         __val = __expr;                              \
      }                                               \
      __val;                                          \
   })

/**
 * Alternative version of get_once() which has no locking in release builds,
 * suitable for hot-paths.
 */
#ifndef NDEBUG
#define get_once_nolock(__expr) ({                    \
      static bool __once;                             \
      static __typeof__(__expr) __val;                \
      if (!__once) {                                  \
         __val = __expr;                              \
         __once = true;                               \
      }                                               \
      __val;                                          \
   })
#else
#define get_once_nolock(__expr) get_once(__expr)
#endif

#endif /* _UTIL_DEBUG_H */

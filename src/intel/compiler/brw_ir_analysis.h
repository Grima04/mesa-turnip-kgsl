/* -*- c++ -*- */
/*
 * Copyright © 2016 Intel Corporation
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

#ifndef BRW_IR_ANALYSIS_H
#define BRW_IR_ANALYSIS_H

namespace brw {
   /**
    * Bitset of state categories that can influence the result of IR analysis
    * passes.
    */
   enum analysis_dependency_class {
      /**
       * The analysis doesn't depend on the IR, its result is effectively a
       * constant during the compilation.
       */
      DEPENDENCY_NOTHING = 0,
      /**
       * The analysis depends on the program being literally the same (good
       * luck...), any change in the input invalidates previous analysis
       * computations.
       */
      DEPENDENCY_EVERYTHING = ~0
   };

   inline analysis_dependency_class
   operator|(analysis_dependency_class x, analysis_dependency_class y)
   {
      return static_cast<analysis_dependency_class>(
         static_cast<unsigned>(x) | static_cast<unsigned>(y));
   }
}

/**
 * Instantiate a program analysis class \p L which can calculate an object of
 * type \p T as result.  \p C is a closure that encapsulates whatever
 * information is required as argument to run the analysis pass.  The purpose
 * of this class is to make sure that:
 *
 *  - The analysis pass is executed lazily whenever it's needed and multiple
 *    executions are optimized out as long as the cached result remains marked
 *    up-to-date.
 *
 *  - There is no way to access the cached analysis result without first
 *    calling L::require(), which makes sure that the analysis pass is rerun
 *    if necessary.
 *
 *  - The cached result doesn't become inconsistent with the program for as
 *    long as it remains marked up-to-date. (This is only enforced in debug
 *    builds for performance reasons)
 *
 * The requirements on \p T are the following:
 *
 *  - Constructible with a single argument, as in 'x = T(c)' for \p c of type
 *    \p C.
 *
 *  - 'x.dependency_class()' on const \p x returns a bitset of
 *    brw::analysis_dependency_class specifying the set of IR objects that are
 *    required to remain invariant for the cached analysis result to be
 *    considered valid.
 *
 *  - 'x.validate(c)' on const \p x returns a boolean result specifying
 *    whether the analysis result \p x is consistent with the input IR.  This
 *    is currently only used for validation in debug builds.
 */
#define BRW_ANALYSIS(L, T, C)                                           \
   class L {                                                            \
   public:                                                              \
      /**                                                               \
       * Construct a program analysis.  \p c is an arbitrary object     \
       * passed as argument to the constructor of the analysis result   \
       * object of type \p T.                                           \
       */                                                               \
      L(C const &c) : c(c), p(NULL) {}                                  \
                                                                        \
      /**                                                               \
       * Destroy a program analysis.                                    \
       */                                                               \
      ~L()                                                              \
      {                                                                 \
         delete p;                                                      \
      }                                                                 \
                                                                        \
      /**                                                               \
       * Obtain the result of a program analysis.  This gives a         \
       * guaranteed up-to-date result, the analysis pass will be        \
       * rerun implicitly if it has become stale.                       \
       */                                                               \
      T &                                                               \
      require()                                                         \
      {                                                                 \
         if (p)                                                         \
            assert(p->validate(c));                                     \
         else                                                           \
            p = new T(c);                                               \
                                                                        \
         return *p;                                                     \
      }                                                                 \
                                                                        \
      const T &                                                         \
      require() const                                                   \
      {                                                                 \
         return const_cast<L *>(this)->require();                       \
      }                                                                 \
                                                                        \
      /**                                                               \
       * Report that dependencies of the analysis pass may have changed \
       * since the last calculation and the cached analysis result may  \
       * have to be discarded.                                          \
       */                                                               \
      void                                                              \
      invalidate(brw::analysis_dependency_class c)                      \
      {                                                                 \
         if (p && c & p->dependency_class()) {                          \
            delete p;                                                   \
            p = NULL;                                                   \
         }                                                              \
      }                                                                 \
                                                                        \
   private:                                                             \
      C c;                                                              \
      T *p;                                                             \
   }

#endif

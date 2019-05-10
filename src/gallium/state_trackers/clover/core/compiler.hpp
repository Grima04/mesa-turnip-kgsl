//
// Copyright 2019 Red Hat, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//

#ifndef CLOVER_CORE_COMPILER_HPP
#define CLOVER_CORE_COMPILER_HPP

#include "core/device.hpp"
#include "core/module.hpp"
#include "llvm/invocation.hpp"

namespace clover {
   namespace compiler {
      static inline module
      compile_program(const std::string &source, const header_map &headers,
                      const device &dev, const std::string &opts,
                      std::string &log) {
         switch (dev.ir_format()) {
         case PIPE_SHADER_IR_NATIVE:
            return llvm::compile_program(source, headers, dev, opts, log);
         default:
            unreachable("device with unsupported IR");
            throw error(CL_INVALID_VALUE);
         }
      }

      static inline module
      link_program(const std::vector<module> &ms, const device &dev,
                   const std::string &opts, std::string &log) {
         switch (dev.ir_format()) {
         case PIPE_SHADER_IR_NATIVE:
            return llvm::link_program(ms, dev, opts, log);
         default:
            unreachable("device with unsupported IR");
            throw error(CL_INVALID_VALUE);
         }
      }
   }
}

#endif

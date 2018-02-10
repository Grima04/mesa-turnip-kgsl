//
// Copyright 2018 Pierre Moreau
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

#include "invocation.hpp"

#ifdef HAVE_CLOVER_SPIRV
#include <spirv-tools/libspirv.hpp>
#endif

#include "util/u_math.h"

#include "compiler/spirv/spirv.h"

using namespace clover;

namespace {

#ifdef HAVE_CLOVER_SPIRV
   std::string
   format_validator_msg(spv_message_level_t level, const char * /* source */,
                        const spv_position_t &position, const char *message) {
      std::string level_str;
      switch (level) {
      case SPV_MSG_FATAL:
         level_str = "Fatal";
         break;
      case SPV_MSG_INTERNAL_ERROR:
         level_str = "Internal error";
         break;
      case SPV_MSG_ERROR:
         level_str = "Error";
         break;
      case SPV_MSG_WARNING:
         level_str = "Warning";
         break;
      case SPV_MSG_INFO:
         level_str = "Info";
         break;
      case SPV_MSG_DEBUG:
         level_str = "Debug";
         break;
      }
      return "[" + level_str + "] At word No." +
             std::to_string(position.index) + ": \"" + message + "\"\n";
   }

   spv_target_env
   convert_opencl_str_to_target_env(const std::string &opencl_version) {
      if (opencl_version == "2.2") {
         return SPV_ENV_OPENCL_2_2;
      } else if (opencl_version == "2.1") {
         return SPV_ENV_OPENCL_2_1;
      } else if (opencl_version == "2.0") {
         return SPV_ENV_OPENCL_2_0;
      } else if (opencl_version == "1.2" ||
                 opencl_version == "1.1" ||
                 opencl_version == "1.0") {
         // SPIR-V is only defined for OpenCL >= 1.2, however some drivers
         // might use it with OpenCL 1.0 and 1.1.
         return SPV_ENV_OPENCL_1_2;
      } else {
         throw build_error("Invalid OpenCL version");
      }
   }
#endif

}

#ifdef HAVE_CLOVER_SPIRV
bool
clover::spirv::is_valid_spirv(const uint32_t *binary, size_t length,
                              const std::string &opencl_version,
                              std::string &r_log) {
   auto const validator_consumer =
      [&r_log](spv_message_level_t level, const char *source,
               const spv_position_t &position, const char *message) {
      r_log += format_validator_msg(level, source, position, message);
   };

   const spv_target_env target_env =
      convert_opencl_str_to_target_env(opencl_version);
   spvtools::SpirvTools spvTool(target_env);
   spvTool.SetMessageConsumer(validator_consumer);

   return spvTool.Validate(binary, length);
}
#else
bool
clover::spirv::is_valid_spirv(const uint32_t * /*binary*/, size_t /*length*/,
                              const std::string &/*opencl_version*/,
                              std::string &/*r_log*/) {
   return false;
}
#endif

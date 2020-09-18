# Copyright © 2020 Hoe Hao Cheng
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
# 
# Authors:
#    Hoe Hao Cheng <haochengho12907@gmail.com>
# 

from mako.template import Template
from os import path
import re
import sys

# constructor: Extensions(name, alias="", required=False, properties=False, feature=None)
# The attributes:
#  - required: the generated code debug_prints "ZINK: {name} required!" and
#              returns NULL if the extension is unavailable.
#
#  - properties: enable the detection of extension properties in a physical
#                device in the generated code using vkGetPhysicalDeviceProperties2(),
#                and store the returned properties struct inside
#                `zink_device_info.{alias}_props`.
#                Example: the properties for `VK_EXT_transform_feedback`, is stored in
#                `VkPhysicalDeviceTransformFeedbackPropertiesEXT tf_props`.
#
#  - feature: enable the fine-grained detection of extension features in a
#             device. Similar to `properties`, this stores the features
#             struct inside `zink_device_info.{alias}_feats`.
#             It sets `zink_device_info.have_{name} = true` only if
#             `{alias}_feats.{feature}` is true. 
#             If feature is None, `have_{extension_name}` is true when the extensions
#             given by vkEnumerateDeviceExtensionProperties() include the extension.
#             Furthermore, `zink_device_info.{extension_alias}_feats` is unavailable.
def EXTENSIONS():
    return [
        Extension("VK_KHR_maintenance1",             required=True),
        Extension("VK_KHR_external_memory",          required=True),
        Extension("VK_KHR_external_memory_fd"),
        Extension("VK_EXT_conditional_rendering",    alias="cond_render", feature="conditionalRendering"),
        Extension("VK_EXT_transform_feedback",       alias="tf", properties=True, feature="transformFeedback"),
        Extension("VK_EXT_index_type_uint8",         alias="index_uint8", feature="indexTypeUint8"),
        Extension("VK_EXT_robustness2",              alias="rb2", properties=True, feature="nullDescriptor"),
        Extension("VK_EXT_vertex_attribute_divisor", alias="vdiv", properties=True, feature="vertexAttributeInstanceRateDivisor"),
        Extension("VK_EXT_calibrated_timestamps"),
    ]

# There exists some inconsistencies regarding the enum constants, fix them.
# This is basically generated_code.replace(key, value).
def REPLACEMENTS():
    return {
        "ROBUSTNESS2": "ROBUSTNESS_2"
    }


header_code = """
#ifndef ZINK_DEVICE_INFO_H
#define ZINK_DEVICE_INFO_H

#include "util/u_memory.h"

#include <vulkan/vulkan.h>

struct zink_screen;

struct zink_device_info {
%for ext in extensions:
   bool have_${ext.name_with_vendor()};
%endfor

   VkPhysicalDeviceFeatures2 feats;
   VkPhysicalDeviceProperties props;
   VkPhysicalDeviceMemoryProperties mem_props;

%for ext in extensions:
%if ext.feature_field is not None:
   VkPhysicalDevice${ext.name_in_camel_case()}Features${ext.vendor()} ${ext.field("feats")};
%endif
%if ext.has_properties:
   VkPhysicalDevice${ext.name_in_camel_case()}Properties${ext.vendor()} ${ext.field("props")};
%endif
%endfor

    const char *extensions[${len(extensions)}];
    uint32_t num_extensions;
};

bool
zink_get_physical_device_info(struct zink_screen *screen);

#endif
"""


impl_code = """
#include "zink_device_info.h"
#include "zink_screen.h"

bool
zink_get_physical_device_info(struct zink_screen *screen) 
{
   struct zink_device_info *info = &screen->info;
%for ext in extensions:
   bool support_${ext.name_with_vendor()} = false;
%endfor
   uint32_t num_extensions = 0;

   vkGetPhysicalDeviceMemoryProperties(screen->pdev, &info->mem_props);

   // enumerate device supported extensions
   if (vkEnumerateDeviceExtensionProperties(screen->pdev, NULL, &num_extensions, NULL) == VK_SUCCESS) {
      if (num_extensions > 0) {
         VkExtensionProperties *extensions = MALLOC(sizeof(VkExtensionProperties) * num_extensions);
         if (!extensions) goto fail;
         vkEnumerateDeviceExtensionProperties(screen->pdev, NULL, &num_extensions, extensions);

         for (uint32_t i = 0; i < num_extensions; ++i) {
         %for ext in extensions:
            if (!strcmp(extensions[i].extensionName, "${ext.name}")) {
               support_${ext.name_with_vendor()} = true;
            }
         %endfor
         }

         FREE(extensions);
      }
   }

   // check for device extension features
   info->feats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

%for ext in extensions:
%if ext.feature_field is not None:
   if (support_${ext.name_with_vendor()}) {
      info->${ext.field("feats")}.sType = ${ext.stype("FEATURES")};
      info->${ext.field("feats")}.pNext = info->feats.pNext;
      info->feats.pNext = &info->${ext.field("feats")};
   }
%endif
%endfor

   vkGetPhysicalDeviceFeatures2(screen->pdev, &info->feats);

%for ext in extensions:
%if ext.feature_field is None:
   info->have_${ext.name_with_vendor()} = support_${ext.name_with_vendor()};
%else:
   if (support_${ext.name_with_vendor()} && info->${ext.field("feats")}.${ext.feature_field}) {
      info->have_${ext.name_with_vendor()} = true;
   }
%endif
%endfor

   // check for device properties
   VkPhysicalDeviceProperties2 props = {};
   props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

%for ext in extensions:
%if ext.has_properties:
   if (info->have_${ext.name_with_vendor()}) {
      info->${ext.field("props")}.sType = ${ext.stype("PROPERTIES")};
      info->${ext.field("props")}.pNext = props.pNext;
      props.pNext = &info->${ext.field("props")};
   }
%endif
%endfor

   vkGetPhysicalDeviceProperties2(screen->pdev, &props);
   memcpy(&info->props, &props.properties, sizeof(info->props));

   // generate extension list
   num_extensions = 0;

%for ext in extensions:
   if (info->have_${ext.name_with_vendor()}) {
       info->extensions[num_extensions++] = "${ext.name}";
%if ext.is_required:
   } else {
       debug_printf("ZINK: ${ext.name} required!\\n");
       goto fail;
%endif
   }
%endfor

   info->num_extensions = num_extensions;

   return true;

fail:
   return false;
}
"""

class Extension:
    name           : str  = None
    alias          : str  = None
    is_required    : bool = False
    has_properties : bool = False
    feature_field : str  = None

    def __init__(self, name, alias="", required=False, properties=False, feature=None):
        self.name = name
        self.alias = alias
        self.is_required = required
        self.has_properties = properties
        self.feature_field = feature

        if alias == "" and (properties == True or feature is not None):
            raise RuntimeError("alias must be available when properties/feature is used")

    # e.g.: "VK_EXT_robustness2" -> "robustness2"
    def pure_name(self):
        return '_'.join(self.name.split('_')[2:])
    
    # e.g.: "VK_EXT_robustness2" -> "EXT_robustness2"
    def name_with_vendor(self):
        return self.name[3:]
    
    # e.g.: "VK_EXT_robustness2" -> "Robustness2"
    def name_in_camel_case(self):
        return "".join([x.title() for x in self.name.split('_')[2:]])
    
    # e.g.: "VK_EXT_robustness2" -> "VK_EXT_ROBUSTNESS2_EXTENSION_NAME"
    # do note that inconsistencies exist, i.e. we have
    # VK_EXT_ROBUSTNESS_2_EXTENSION_NAME defined in the headers, but then
    # we also have VK_KHR_MAINTENANCE1_EXTENSION_NAME
    def extension_name(self):
        return self.name.upper() + "_EXTENSION_NAME"

    # generate a C string literal for the extension
    def extension_name_literal(self):
        return '"' + self.name + '"'

    # get the field in zink_device_info that refers to the extension's
    # feature/properties struct
    # e.g. rb2_<suffix> for VK_EXT_robustness2
    def field(self, suffix: str):
        return self.alias + '_' + suffix

    # the sType of the extension's struct
    # e.g. VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT
    # for VK_EXT_transform_feedback and struct="FEATURES"
    def stype(self, struct: str):
        return ("VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_" 
                + self.pure_name().upper()
                + '_' + struct + '_' 
                + self.vendor())

    # e.g. EXT in VK_EXT_robustness2
    def vendor(self):
        return self.name.split('_')[1]


def replace_code(code: str, replacement: dict):
    for (k, v) in replacement.items():
        code = code.replace(k, v)
    
    return code


if __name__ == "__main__":
    try:
        header_path = sys.argv[1]
        impl_path = sys.argv[2]

        header_path = path.abspath(header_path)
        impl_path = path.abspath(impl_path)
    except:
        print("usage: %s <path to .h> <path to .c>" % sys.argv[0])
        exit(1)

    extensions = EXTENSIONS()
    replacement = REPLACEMENTS()

    with open(header_path, "w") as header_file:
        header = Template(header_code).render(extensions=extensions).strip()
        header = replace_code(header, replacement)
        print(header, file=header_file)

    with open(impl_path, "w") as impl_file:
        impl = Template(impl_code).render(extensions=extensions).strip()
        impl = replace_code(impl, replacement)
        print(impl, file=impl_file)

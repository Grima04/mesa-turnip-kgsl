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
from xml.etree import ElementTree
from zink_extensions import Extension,Layer,Version
import sys

# constructor: Extension(name, core_since=None, functions=[])
# The attributes:
#  - core_since: the Vulkan version where this extension is promoted to core.
#                When instance_info->loader_version is greater than or equal to this
#                instance_info.have_{name} is set to true unconditionally. This
#                is done because loading extensions that are promoted to core is
#                considered to be an error.
#
#  - functions: functions which are added by the extension. The function names
#               should not include the "vk" prefix and the vendor suffix - these
#               will be added by the codegen accordingly.
EXTENSIONS = [
    Extension("VK_EXT_debug_utils"),
    Extension("VK_KHR_get_physical_device_properties2",
        functions=["GetPhysicalDeviceFeatures2", "GetPhysicalDeviceProperties2"]),
    Extension("VK_KHR_draw_indirect_count",
        functions=["CmdDrawIndexedIndirectCount", "CmdDrawIndirectCount"]),
    Extension("VK_KHR_external_memory_capabilities"),
    Extension("VK_MVK_moltenvk"),
]

# constructor: Layer(name, conditions=[])
LAYERS = [
    # if we have debug_util, allow a validation layer to be added.
    Layer("VK_LAYER_KHRONOS_validation",
      conditions=["have_EXT_debug_utils"]),
    Layer("VK_LAYER_LUNARG_standard_validation",
      conditions=["have_EXT_debug_utils", "!have_layer_KHRONOS_validation"]),
]

REPLACEMENTS = {
    "VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES2_EXTENSION_NAME" : "VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME"
}

header_code = """
#ifndef ZINK_INSTANCE_H
#define ZINK_INSTANCE_H

#include "os/os_process.h"

#include <vulkan/vulkan.h>

#if defined(__APPLE__)
// Source of MVK_VERSION
// Source of VK_EXTX_PORTABILITY_SUBSET_EXTENSION_NAME
#include "MoltenVK/vk_mvk_moltenvk.h"
#endif

struct zink_screen;

struct zink_instance_info {
   uint32_t loader_version;

%for ext in extensions:
   bool have_${ext.name_with_vendor()};
%endfor

%for layer in layers:
   bool have_layer_${layer.pure_name()};
%endfor
};

VkInstance
zink_create_instance(struct zink_instance_info *instance_info);

bool
zink_load_instance_extensions(struct zink_screen *screen);

#endif
"""

impl_code = """
#include "zink_instance.h"
#include "zink_screen.h"

VkInstance
zink_create_instance(struct zink_instance_info *instance_info)
{
   /* reserve one slot for MoltenVK */
   const char *layers[${len(extensions) + 1}] = { 0 };
   uint32_t num_layers = 0;
   
   const char *extensions[${len(layers) + 1}] = { 0 };
   uint32_t num_extensions = 0;

%for ext in extensions:
   bool have_${ext.name_with_vendor()} = false;
%endfor

%for layer in layers:
   bool have_layer_${layer.pure_name()} = false;
%endfor

#if defined(MVK_VERSION)
   bool have_moltenvk_layer = false;
#endif

   // Build up the extensions from the reported ones but only for the unnamed layer
   uint32_t extension_count = 0;
   if (vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL) == VK_SUCCESS) {
       VkExtensionProperties *extension_props = malloc(extension_count * sizeof(VkExtensionProperties));
       if (extension_props) {
           if (vkEnumerateInstanceExtensionProperties(NULL, &extension_count, extension_props) == VK_SUCCESS) {
              for (uint32_t i = 0; i < extension_count; i++) {
        %for ext in extensions:
            %if not ext.core_since:
                if (!strcmp(extension_props[i].extensionName, ${ext.extension_name_literal()})) {
                    have_${ext.name_with_vendor()} = true;
                    extensions[num_extensions++] = ${ext.extension_name_literal()};
                }
            %else:
                if (instance_info->loader_version < ${ext.core_since.version()}) {
                   if (!strcmp(extension_props[i].extensionName, ${ext.extension_name_literal()})) {
                        have_${ext.name_with_vendor()} = true;
                        extensions[num_extensions++] = ${ext.extension_name_literal()};
                   }
                 } else {
                    have_${ext.name_with_vendor()} = true;
                 }
            %endif
        %endfor
              }
           }
       free(extension_props);
       }
   }

   // Clear have_EXT_debug_utils if we do not want debug info
   if (!(zink_debug & ZINK_DEBUG_VALIDATION)) {
      have_EXT_debug_utils = false;
   }

    // Build up the layers from the reported ones
    uint32_t layer_count = 0;

    if (vkEnumerateInstanceLayerProperties(&layer_count, NULL) == VK_SUCCESS) {
        VkLayerProperties *layer_props = malloc(layer_count * sizeof(VkLayerProperties));
        if (layer_props) {
            if (vkEnumerateInstanceLayerProperties(&layer_count, layer_props) == VK_SUCCESS) {
               for (uint32_t i = 0; i < layer_count; i++) {
%for layer in layers:
                  if (!strcmp(layer_props[i].layerName, ${layer.extension_name_literal()})) {
                     have_layer_${layer.pure_name()} = true;
                  }
%endfor
#if defined(MVK_VERSION)
                  if (!strcmp(layer_props[i].layerName, "MoltenVK")) {
                     have_moltenvk_layer = true;
                     layers[num_layers++] = "MoltenVK";
                  }
#endif
               }
            }
        free(layer_props);
        }
    }

%for ext in extensions:
   instance_info->have_${ext.name_with_vendor()} = have_${ext.name_with_vendor()};
%endfor

%for layer in layers:
<%
    conditions = ""
    if layer.enable_conds:
        for cond in layer.enable_conds:
            conditions += "&& (" + cond + ") "
    conditions = conditions.strip()
%>\
   if (have_layer_${layer.pure_name()} ${conditions}) {
      layers[num_layers++] = ${layer.extension_name_literal()};
      instance_info->have_layer_${layer.pure_name()} = true;
   }
%endfor

   VkApplicationInfo ai = {};
   ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;

   char proc_name[128];
   if (os_get_process_name(proc_name, ARRAY_SIZE(proc_name)))
      ai.pApplicationName = proc_name;
   else
      ai.pApplicationName = "unknown";

   ai.pEngineName = "mesa zink";
   ai.apiVersion = instance_info->loader_version;

   VkInstanceCreateInfo ici = {};
   ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ici.pApplicationInfo = &ai;
   ici.ppEnabledExtensionNames = extensions;
   ici.enabledExtensionCount = num_extensions;
   ici.ppEnabledLayerNames = layers;
   ici.enabledLayerCount = num_layers;

   VkInstance instance = VK_NULL_HANDLE;
   VkResult err = vkCreateInstance(&ici, NULL, &instance);
   if (err != VK_SUCCESS)
      return VK_NULL_HANDLE;

   return instance;
}

bool
zink_load_instance_extensions(struct zink_screen *screen)
{
   if (zink_debug & ZINK_DEBUG_VALIDATION) {
      printf("zink: Loader %d.%d.%d \\n", VK_VERSION_MAJOR(screen->instance_info.loader_version), VK_VERSION_MINOR(screen->instance_info.loader_version), VK_VERSION_PATCH(screen->instance_info.loader_version));
   }

%for ext in extensions:
%if bool(ext.instance_funcs) and not ext.core_since:
   if (screen->instance_info.have_${ext.name_with_vendor()}) {
   %for func in ext.instance_funcs:
      GET_PROC_ADDR_INSTANCE_LOCAL(screen->instance, ${func}${ext.vendor()});
      screen->vk_${func} = vk_${func}${ext.vendor()};
   %endfor
   }
%elif bool(ext.instance_funcs):
   if (screen->instance_info.have_${ext.name_with_vendor()}) {
      if (screen->instance_info.loader_version < ${ext.core_since.version()}) {
      %for func in ext.instance_funcs:
         GET_PROC_ADDR_INSTANCE_LOCAL(screen->instance, ${func}${ext.vendor()});
         screen->vk_${func} = vk_${func}${ext.vendor()};
      %endfor
      } else {
      %for func in ext.instance_funcs:
         GET_PROC_ADDR_INSTANCE(${func});
      %endfor
      }
   }
%endif
%endfor

   return true;
}
"""


def replace_code(code: str, replacement: dict):
    for (k, v) in replacement.items():
        code = code.replace(k, v)
    
    return code


# Parses e.g. "VK_VERSION_x_y" to integer tuple (x, y)
# For any erroneous inputs, None is returned
def parse_promotedto(promotedto: str):
   result = None

   if promotedto and promotedto.startswith("VK_VERSION_"):
      (major, minor) = promotedto.split('_')[-2:]
      result = (int(major), int(minor))

   return result

def parse_vkxml(path: str):
    vkxml = ElementTree.parse(path)
    all_extensions = dict()

    for ext in vkxml.findall("extensions/extension"):
        name = ext.get("name")
        promotedto = parse_promotedto(ext.get("promotedto"))
        
        if not name:
            print("found malformed extension entry in vk.xml")
            exit(1)

        all_extensions[name] = promotedto

    return all_extensions

if __name__ == "__main__":
    try:
        header_path = sys.argv[1]
        impl_path = sys.argv[2]
        vkxml_path = sys.argv[3]

        header_path = path.abspath(header_path)
        impl_path = path.abspath(impl_path)
        vkxml_path = path.abspath(vkxml_path)
    except:
        print("usage: %s <path to .h> <path to .c> <path to vk.xml>" % sys.argv[0])
        exit(1)

    all_extensions = parse_vkxml(vkxml_path)

    extensions = EXTENSIONS
    layers = LAYERS
    replacement = REPLACEMENTS

    for ext in extensions:
        if ext.name not in all_extensions:
            print("the extension {} is not registered in vk.xml - a typo?".format(ext.name))
            exit(1)
        
        if all_extensions[ext.name] is not None:
            ext.core_since = Version((*all_extensions[ext.name], 0))

    with open(header_path, "w") as header_file:
        header = Template(header_code).render(extensions=extensions, layers=layers).strip()
        header = replace_code(header, replacement)
        print(header, file=header_file)

    with open(impl_path, "w") as impl_file:
        impl = Template(impl_code).render(extensions=extensions, layers=layers).strip()
        impl = replace_code(impl, replacement)
        print(impl, file=impl_file)

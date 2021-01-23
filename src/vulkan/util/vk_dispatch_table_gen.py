# coding=utf-8
COPYRIGHT = """\
/*
 * Copyright 2020 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
"""

import argparse
import math
import os
import xml.etree.ElementTree as et

from collections import OrderedDict, namedtuple
from mako.template import Template

from vk_extensions import *

# We generate a static hash table for entry point lookup
# (vkGetProcAddress). We use a linear congruential generator for our hash
# function and a power-of-two size table. The prime numbers are determined
# experimentally.

TEMPLATE_H = Template(COPYRIGHT + """\
/* This file generated from ${filename}, don't edit directly. */

#ifndef VK_DISPATCH_TABLE_H
#define VK_DISPATCH_TABLE_H

#include "vulkan/vulkan.h"
#include "vulkan/vulkan_intel.h"
#include "vulkan/vk_android_native_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

<%def name="dispatch_table(type, entrypoints)">
struct vk_${type}_dispatch_table {
% for e in entrypoints:
  % if e.alias:
    <% continue %>
  % endif
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
  % if e.aliases:
    union {
        PFN_vk${e.name} ${e.name};
      % for a in e.aliases:
        PFN_vk${a.name} ${a.name};
      % endfor
    };
  % else:
    PFN_vk${e.name} ${e.name};
  % endif
  % if e.guard is not None:
#else
    % if e.aliases:
    union {
        PFN_vkVoidFunction ${e.name};
      % for a in e.aliases:
        PFN_vkVoidFunction ${a.name};
      % endfor
    };
    % else:
    PFN_vkVoidFunction ${e.name};
    % endif
#endif
  % endif
% endfor
};
</%def>

${dispatch_table('instance', instance_entrypoints)}
${dispatch_table('physical_device', physical_device_entrypoints)}
${dispatch_table('device', device_entrypoints)}

void
vk_instance_dispatch_table_load(struct vk_instance_dispatch_table *table,
                                PFN_vkGetInstanceProcAddr gpa,
                                VkInstance instance);
void
vk_physical_device_dispatch_table_load(struct vk_physical_device_dispatch_table *table,
                                       PFN_vkGetInstanceProcAddr gpa,
                                       VkInstance instance);
void
vk_device_dispatch_table_load(struct vk_device_dispatch_table *table,
                              PFN_vkGetDeviceProcAddr gpa,
                              VkDevice device);

#ifdef __cplusplus
}
#endif

#endif /* VK_DISPATCH_TABLE_H */
""", output_encoding='utf-8')

TEMPLATE_C = Template(COPYRIGHT + """\
/* This file generated from ${filename}, don't edit directly. */

#include "vk_dispatch_table.h"

<%def name="load_dispatch_table(type, VkType, ProcAddr, entrypoints)">
void
vk_${type}_dispatch_table_load(struct vk_${type}_dispatch_table *table,
                               PFN_vk${ProcAddr} gpa,
                               ${VkType} obj)
{
% if type != 'physical_device':
    table->${ProcAddr} = gpa;
% endif
% for e in entrypoints:
  % if e.alias or e.name == '${ProcAddr}':
    <% continue %>
  % endif
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
    table->${e.name} = (PFN_vk${e.name}) gpa(obj, "vk${e.name}");
  % for a in e.aliases:
    if (table->${e.name} == NULL) {
        table->${e.name} = (PFN_vk${e.name}) gpa(obj, "vk${a.name}");
    }
  % endfor
  % if e.guard is not None:
#endif
  % endif
% endfor
}
</%def>

${load_dispatch_table('instance', 'VkInstance', 'GetInstanceProcAddr',
                      instance_entrypoints)}

${load_dispatch_table('physical_device', 'VkInstance', 'GetInstanceProcAddr',
                      physical_device_entrypoints)}

${load_dispatch_table('device', 'VkDevice', 'GetDeviceProcAddr',
                      device_entrypoints)}

""", output_encoding='utf-8')

EntrypointParam = namedtuple('EntrypointParam', 'type name decl')

class EntrypointBase(object):
    def __init__(self, name):
        assert name.startswith('vk')
        self.name = name[2:]
        self.alias = None
        self.guard = None
        self.num = None
        # Extensions which require this entrypoint
        self.core_version = None
        self.extensions = []

    def prefixed_name(self, prefix):
        return prefix + '_' + self.name

class Entrypoint(EntrypointBase):
    def __init__(self, name, return_type, params, guard=None):
        super(Entrypoint, self).__init__(name)
        self.return_type = return_type
        self.params = params
        self.guard = guard
        self.aliases = []

    def is_physical_device_entrypoint(self):
        return self.params[0].type in ('VkPhysicalDevice', )

    def is_device_entrypoint(self):
        return self.params[0].type in ('VkDevice', 'VkCommandBuffer', 'VkQueue')

    def decl_params(self):
        return ', '.join(p.decl for p in self.params)

    def call_params(self):
        return ', '.join(p.name for p in self.params)

class EntrypointAlias(EntrypointBase):
    def __init__(self, name, entrypoint):
        super(EntrypointAlias, self).__init__(name)
        self.alias = entrypoint
        entrypoint.aliases.append(self)

    def is_physical_device_entrypoint(self):
        return self.alias.is_physical_device_entrypoint()

    def is_device_entrypoint(self):
        return self.alias.is_device_entrypoint()

    def prefixed_name(self, prefix):
        return self.alias.prefixed_name(prefix)

    @property
    def params(self):
        return self.alias.params

    @property
    def return_type(self):
        return self.alias.return_type

    def decl_params(self):
        return self.alias.decl_params()

    def call_params(self):
        return self.alias.call_params()

def get_entrypoints(doc, entrypoints_to_defines):
    """Extract the entry points from the registry."""
    entrypoints = OrderedDict()

    for command in doc.findall('./commands/command'):
        if 'alias' in command.attrib:
            alias = command.attrib['name']
            target = command.attrib['alias']
            entrypoints[alias] = EntrypointAlias(alias, entrypoints[target])
        else:
            name = command.find('./proto/name').text
            ret_type = command.find('./proto/type').text
            params = [EntrypointParam(
                type=p.find('./type').text,
                name=p.find('./name').text,
                decl=''.join(p.itertext())
            ) for p in command.findall('./param')]
            guard = entrypoints_to_defines.get(name)
            # They really need to be unique
            assert name not in entrypoints
            entrypoints[name] = Entrypoint(name, ret_type, params, guard)

    for feature in doc.findall('./feature'):
        assert feature.attrib['api'] == 'vulkan'
        version = VkVersion(feature.attrib['number'])
        for command in feature.findall('./require/command'):
            e = entrypoints[command.attrib['name']]
            assert e.core_version is None
            e.core_version = version

    for extension in doc.findall('.extensions/extension'):
        if extension.attrib['supported'] != 'vulkan':
            continue

        ext_name = extension.attrib['name']

        ext = Extension(ext_name, 1, True)
        ext.type = extension.attrib['type']

        for command in extension.findall('./require/command'):
            e = entrypoints[command.attrib['name']]
            assert e.core_version is None
            e.extensions.append(ext)

    return entrypoints.values()


def get_entrypoints_defines(doc):
    """Maps entry points to extension defines."""
    entrypoints_to_defines = {}

    platform_define = {}
    for platform in doc.findall('./platforms/platform'):
        name = platform.attrib['name']
        define = platform.attrib['protect']
        platform_define[name] = define

    for extension in doc.findall('./extensions/extension[@platform]'):
        platform = extension.attrib['platform']
        define = platform_define[platform]

        for entrypoint in extension.findall('./require/command'):
            fullname = entrypoint.attrib['name']
            entrypoints_to_defines[fullname] = define

    return entrypoints_to_defines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-c', help='Output C file.')
    parser.add_argument('--out-h', help='Output H file.')
    parser.add_argument('--xml',
                        help='Vulkan API XML file.',
                        required=True,
                        action='append',
                        dest='xml_files')
    args = parser.parse_args()

    entrypoints = []

    for filename in args.xml_files:
        doc = et.parse(filename)
        entrypoints += get_entrypoints(doc, get_entrypoints_defines(doc))

    # Manually add CreateDmaBufImageINTEL for which we don't have an extension
    # defined.
    entrypoints.append(Entrypoint('vkCreateDmaBufImageINTEL', 'VkResult', [
        EntrypointParam('VkDevice', 'device', 'VkDevice device'),
        EntrypointParam('VkDmaBufImageCreateInfo', 'pCreateInfo',
                        'const VkDmaBufImageCreateInfo* pCreateInfo'),
        EntrypointParam('VkAllocationCallbacks', 'pAllocator',
                        'const VkAllocationCallbacks* pAllocator'),
        EntrypointParam('VkDeviceMemory', 'pMem', 'VkDeviceMemory* pMem'),
        EntrypointParam('VkImage', 'pImage', 'VkImage* pImage')
    ]))

    device_entrypoints = []
    physical_device_entrypoints = []
    instance_entrypoints = []
    for e in entrypoints:
        if e.is_device_entrypoint():
            device_entrypoints.append(e)
        elif e.is_physical_device_entrypoint():
            physical_device_entrypoints.append(e)
        else:
            instance_entrypoints.append(e)

    # For outputting entrypoints.h we generate a anv_EntryPoint() prototype
    # per entry point.
    try:
        if args.out_h:
            with open(args.out_h, 'wb') as f:
                f.write(TEMPLATE_H.render(instance_entrypoints=instance_entrypoints,
                                          physical_device_entrypoints=physical_device_entrypoints,
                                          device_entrypoints=device_entrypoints,
                                          filename=os.path.basename(__file__)))
            with open(args.out_c, 'wb') as f:
                f.write(TEMPLATE_C.render(instance_entrypoints=instance_entrypoints,
                                          physical_device_entrypoints=physical_device_entrypoints,
                                          device_entrypoints=device_entrypoints,
                                          filename=os.path.basename(__file__)))
    except Exception:
        # In the event there's an error, this imports some helpers from mako
        # to print a useful stack trace and prints it, then exits with
        # status 1, if python is run with debug; otherwise it just raises
        # the exception
        if __debug__:
            import sys
            from mako import exceptions
            sys.stderr.write(exceptions.text_error_template().render() + '\n')
            sys.exit(1)
        raise


if __name__ == '__main__':
    main()

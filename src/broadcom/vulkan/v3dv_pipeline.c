/*
 * Copyright © 2019 Raspberry Pi
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

#include "vk_util.h"

#include "v3dv_private.h"

VkResult
v3dv_CreateShaderModule(VkDevice _device,
                        const VkShaderModuleCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkShaderModule *pShaderModule)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_shader_module *module;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   module = vk_alloc2(&device->alloc, pAllocator,
                       sizeof(*module) + pCreateInfo->codeSize, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (module == NULL)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   module->size = pCreateInfo->codeSize;
   memcpy(module->data, pCreateInfo->pCode, module->size);

   _mesa_sha1_compute(module->data, module->size, module->sha1);

   *pShaderModule = v3dv_shader_module_to_handle(module);

   return VK_SUCCESS;
}

void
v3dv_DestroyShaderModule(VkDevice _device,
                         VkShaderModule _module,
                         const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_shader_module, module, _module);

   if (!module)
      return;

   vk_free2(&device->alloc, pAllocator, module);
}

void
v3dv_DestroyPipeline(VkDevice _device,
                     VkPipeline _pipeline,
                     const VkAllocationCallbacks *pAllocator)
{
   /* FIXME: stub */
}

VkResult
v3dv_CreateGraphicsPipelines(VkDevice _device,
                             VkPipelineCache pipelineCache,
                             uint32_t count,
                             const VkGraphicsPipelineCreateInfo *pCreateInfos,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipelines)
{
   /* FIXME: stub */

   return VK_SUCCESS;
}

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

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "v3dv_private.h"
#include "vk_util.h"

VkResult
v3dv_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                          uint32_t *pPropertyCount,
                                          VkExtensionProperties *pProperties)
{
   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

   for (int i = 0; i < V3DV_INSTANCE_EXTENSION_COUNT; i++) {
      if (v3dv_instance_extensions_supported.extensions[i]) {
         vk_outarray_append(&out, prop) {
            *prop = v3dv_instance_extensions[i];
         }
      }
   }

   return vk_outarray_status(&out);
}

VkResult
v3dv_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkInstance *pInstance)
{
   /* FIXME: stub */

   return VK_SUCCESS;
}

void
v3dv_DestroyInstance(VkInstance _instance,
                     const VkAllocationCallbacks *pAllocator)
{
   /* FIXME: stub */
}

VkResult
v3dv_EnumeratePhysicalDevices(VkInstance _instance,
                              uint32_t *pPhysicalDeviceCount,
                              VkPhysicalDevice *pPhysicalDevices)
{
   /* FIXME: stub */

   return VK_SUCCESS;
}

void
v3dv_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceFeatures *pFeatures)
{
   /* FIXME: stub */
}

void
v3dv_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties *pProperties)
{
   /* FIXME: stub */
}

void
v3dv_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                            uint32_t *pCount,
                                            VkQueueFamilyProperties *pQueueFamilyProperties)
{
   /* FIXME: stub */
}

void
v3dv_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                       VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   /* FIXME: stub */
}


PFN_vkVoidFunction
v3dv_GetInstanceProcAddr(VkInstance _instance,
                         const char *pName)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);

   /* The Vulkan 1.0 spec for vkGetInstanceProcAddr has a table of exactly
    * when we have to return valid function pointers, NULL, or it's left
    * undefined.  See the table for exact details.
    */
   if (pName == NULL)
      return NULL;

#define LOOKUP_V3DV_ENTRYPOINT(entrypoint)              \
   if (strcmp(pName, "vk" #entrypoint) == 0)            \
      return (PFN_vkVoidFunction)v3dv_##entrypoint

   LOOKUP_V3DV_ENTRYPOINT(EnumerateInstanceExtensionProperties);
   LOOKUP_V3DV_ENTRYPOINT(CreateInstance);

#undef LOOKUP_V3DV_ENTRYPOINT

   if (instance == NULL)
      return NULL;

   int idx = v3dv_get_instance_entrypoint_index(pName);
   if (idx >= 0)
      return instance->dispatch.entrypoints[idx];

   idx = v3dv_get_physical_device_entrypoint_index(pName);
   if (idx >= 0)
      return instance->physicalDevice.dispatch.entrypoints[idx];

   idx = v3dv_get_device_entrypoint_index(pName);
   if (idx >= 0)
      return instance->device_dispatch.entrypoints[idx];

   return NULL;
}

/* With version 1+ of the loader interface the ICD should expose
 * vk_icdGetInstanceProcAddr to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction
VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                     const char *pName);

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance,
                          const char*                                 pName)
{
   return v3dv_GetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction
v3dv_GetDeviceProcAddr(VkDevice _device,
                       const char *pName)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   if (!device || !pName)
      return NULL;

   int idx = v3dv_get_device_entrypoint_index(pName);
   if (idx < 0)
      return NULL;

   return device->dispatch.entrypoints[idx];
}

/* With version 4+ of the loader interface the ICD should expose
 * vk_icdGetPhysicalDeviceProcAddr()
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance  _instance,
                                const char* pName);

PFN_vkVoidFunction
vk_icdGetPhysicalDeviceProcAddr(VkInstance  _instance,
                                const char* pName)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);

   if (!pName || !instance)
      return NULL;

   int idx = v3dv_get_physical_device_entrypoint_index(pName);
   if (idx < 0)
      return NULL;

   return instance->physicalDevice.dispatch.entrypoints[idx];
}

VkResult
v3dv_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                        const char *pLayerName,
                                        uint32_t *pPropertyCount,
                                        VkExtensionProperties *pProperties)
{
   /* FIXME: stub */

   return VK_SUCCESS;
}

VkResult
v3dv_CreateDevice(VkPhysicalDevice physicalDevice,
                  const VkDeviceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkDevice *pDevice)
{
   /* FIXME: stub */

   return VK_SUCCESS;
}

void
v3dv_DestroyDevice(VkDevice _device,
                   const VkAllocationCallbacks *pAllocator)
{
   /* FIXME: stub */
}

void
v3dv_GetDeviceQueue(VkDevice _device,
                    uint32_t queueNodeIndex,
                    uint32_t queueIndex,
                    VkQueue *pQueue)
{
   /* FIXME: stub */
}

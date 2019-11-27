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
#include <xf86drm.h>

#include "v3dv_private.h"
#include "vk_util.h"

#include "compiler/glsl_types.h"

static void *
default_alloc_func(void *pUserData, size_t size, size_t align,
                   VkSystemAllocationScope allocationScope)
{
   return malloc(size);
}

static void *
default_realloc_func(void *pUserData, void *pOriginal, size_t size,
                     size_t align, VkSystemAllocationScope allocationScope)
{
   return realloc(pOriginal, size);
}

static void
default_free_func(void *pUserData, void *pMemory)
{
   free(pMemory);
}

static const VkAllocationCallbacks default_alloc = {
   .pUserData = NULL,
   .pfnAllocation = default_alloc_func,
   .pfnReallocation = default_realloc_func,
   .pfnFree = default_free_func,
};

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
   struct v3dv_instance *instance;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   struct v3dv_instance_extension_table enabled_extensions = {};
   for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      int idx;
      for (idx = 0; idx < V3DV_INSTANCE_EXTENSION_COUNT; idx++) {
         if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                    v3dv_instance_extensions[idx].extensionName) == 0)
            break;
      }

      if (idx >= V3DV_INSTANCE_EXTENSION_COUNT)
         return vk_error(NULL, VK_ERROR_EXTENSION_NOT_PRESENT);

      if (!v3dv_instance_extensions_supported.extensions[idx])
         return vk_error(NULL, VK_ERROR_EXTENSION_NOT_PRESENT);

      enabled_extensions.extensions[idx] = true;
   }

   instance = vk_alloc2(&default_alloc, pAllocator, sizeof(*instance), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   instance->_loader_data.loaderMagic = ICD_LOADER_MAGIC;

   if (pAllocator)
      instance->alloc = *pAllocator;
   else
      instance->alloc = default_alloc;

   instance->app_info = (struct v3dv_app_info) { .api_version = 0 };
   if (pCreateInfo->pApplicationInfo) {
      const VkApplicationInfo *app = pCreateInfo->pApplicationInfo;

      instance->app_info.app_name =
         vk_strdup(&instance->alloc, app->pApplicationName,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      instance->app_info.app_version = app->applicationVersion;

      instance->app_info.engine_name =
         vk_strdup(&instance->alloc, app->pEngineName,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      instance->app_info.engine_version = app->engineVersion;

      instance->app_info.api_version = app->apiVersion;
   }

   if (instance->app_info.api_version == 0)
      instance->app_info.api_version = VK_API_VERSION_1_0;

   instance->enabled_extensions = enabled_extensions;

   for (unsigned i = 0; i < ARRAY_SIZE(instance->dispatch.entrypoints); i++) {
      /* Vulkan requires that entrypoints for extensions which have not been
       * enabled must not be advertised.
       */
      if (!v3dv_instance_entrypoint_is_enabled(i,
                                              instance->app_info.api_version,
                                              &instance->enabled_extensions)) {
         instance->dispatch.entrypoints[i] = NULL;
      } else {
         instance->dispatch.entrypoints[i] =
            v3dv_instance_dispatch_table.entrypoints[i];
      }
   }

   struct v3dv_physical_device *pdevice = &instance->physicalDevice;
   for (unsigned i = 0; i < ARRAY_SIZE(pdevice->dispatch.entrypoints); i++) {
      /* Vulkan requires that entrypoints for extensions which have not been
       * enabled must not be advertised.
       */
      if (!v3dv_physical_device_entrypoint_is_enabled(i,
                                                     instance->app_info.api_version,
                                                     &instance->enabled_extensions)) {
         pdevice->dispatch.entrypoints[i] = NULL;
      } else {
         pdevice->dispatch.entrypoints[i] =
            v3dv_physical_device_dispatch_table.entrypoints[i];
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(instance->device_dispatch.entrypoints); i++) {
      /* Vulkan requires that entrypoints for extensions which have not been
       * enabled must not be advertised.
       */
      if (!v3dv_device_entrypoint_is_enabled(i,
                                            instance->app_info.api_version,
                                            &instance->enabled_extensions,
                                            NULL)) {
         instance->device_dispatch.entrypoints[i] = NULL;
      } else {
         instance->device_dispatch.entrypoints[i] =
            v3dv_device_dispatch_table.entrypoints[i];
      }
   }

   instance->physicalDeviceCount = -1;

   result = vk_debug_report_instance_init(&instance->debug_report_callbacks);
   if (result != VK_SUCCESS) {
      vk_free2(&default_alloc, pAllocator, instance);
      return vk_error(NULL, result);
   }

   glsl_type_singleton_init_or_ref();

   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   *pInstance = v3dv_instance_to_handle(instance);

   return VK_SUCCESS;
}

static void
physical_device_finish(struct v3dv_physical_device *device)
{
   /* FIXME: stub */
}

void
v3dv_DestroyInstance(VkInstance _instance,
                     const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);

   if (!instance)
      return;

   if (instance->physicalDeviceCount > 0) {
      /* We support at most one physical device. */
      assert(instance->physicalDeviceCount == 1);
      physical_device_finish(&instance->physicalDevice);
   }

   vk_free(&instance->alloc, (char *)instance->app_info.app_name);
   vk_free(&instance->alloc, (char *)instance->app_info.engine_name);

   VG(VALGRIND_DESTROY_MEMPOOL(instance));

   vk_debug_report_instance_destroy(&instance->debug_report_callbacks);

   glsl_type_singleton_decref();

   vk_free(&instance->alloc, instance);
}

static VkResult
physical_device_init(struct v3dv_physical_device *device,
                     struct v3dv_instance *instance,
                     drmDevicePtr drm_device)
{
   /* FIXME stub */
   return VK_SUCCESS;
}

static VkResult
enumerate_devices(struct v3dv_instance *instance)
{
   /* TODO: Check for more devices? */
   drmDevicePtr devices[8];
   VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
   int max_devices;

   instance->physicalDeviceCount = 0;

   max_devices = drmGetDevices2(0, devices, ARRAY_SIZE(devices));
   if (max_devices < 1)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   for (unsigned i = 0; i < (unsigned)max_devices; i++) {
      if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER &&
          devices[i]->bustype == DRM_BUS_PCI &&
          /* FIXME: so we can run past this point for now */
          (true || devices[i]->deviceinfo.pci->vendor_id == 0x14E4)) {
         result = physical_device_init(&instance->physicalDevice,
                                       instance, devices[i]);
         if (result != VK_ERROR_INCOMPATIBLE_DRIVER)
            break;
      }
   }
   drmFreeDevices(devices, max_devices);

   if (result == VK_SUCCESS)
      instance->physicalDeviceCount = 1;

   return result;
}

static VkResult
instance_ensure_physical_device(struct v3dv_instance *instance)
{
   if (instance->physicalDeviceCount < 0) {
      VkResult result = enumerate_devices(instance);
      if (result != VK_SUCCESS &&
          result != VK_ERROR_INCOMPATIBLE_DRIVER)
         return result;
   }

   return VK_SUCCESS;
}

VkResult
v3dv_EnumeratePhysicalDevices(VkInstance _instance,
                              uint32_t *pPhysicalDeviceCount,
                              VkPhysicalDevice *pPhysicalDevices)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);
   VK_OUTARRAY_MAKE(out, pPhysicalDevices, pPhysicalDeviceCount);
 
   VkResult result = instance_ensure_physical_device(instance);
   if (result != VK_SUCCESS)
      return result;

   if (instance->physicalDeviceCount == 0)
      return VK_SUCCESS;

   assert(instance->physicalDeviceCount == 1);
   vk_outarray_append(&out, i) {
      *i = v3dv_physical_device_to_handle(&instance->physicalDevice);
   }

   return vk_outarray_status(&out);
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

VkResult
v3dv_CreateDebugReportCallbackEXT(VkInstance _instance,
                                 const VkDebugReportCallbackCreateInfoEXT* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator,
                                 VkDebugReportCallbackEXT* pCallback)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);
   return vk_create_debug_report_callback(&instance->debug_report_callbacks,
                                          pCreateInfo, pAllocator, &instance->alloc,
                                          pCallback);
}

void
v3dv_DestroyDebugReportCallbackEXT(VkInstance _instance,
                                  VkDebugReportCallbackEXT _callback,
                                  const VkAllocationCallbacks* pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);
   vk_destroy_debug_report_callback(&instance->debug_report_callbacks,
                                    _callback, pAllocator, &instance->alloc);
}

VkResult
v3dv_AllocateMemory(VkDevice _device,
                    const VkMemoryAllocateInfo *pAllocateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkDeviceMemory *pMem)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_device_memory *mem;
   /* struct v3dv_physical_device *pdevice = &device->instance->physicalDevice; */

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   /* The Vulkan 1.0.33 spec says "allocationSize must be greater than 0". */
   assert(pAllocateInfo->allocationSize > 0);

   if (pAllocateInfo->allocationSize > MAX_MEMORY_ALLOCATION_SIZE)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   mem = vk_alloc2(&device->alloc, pAllocator, sizeof(*mem), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (mem == NULL)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* FIXME: assert(pAllocateInfo->memoryTypeIndex < pdevice->memory.type_count); */
   /* FIXME: mem->type = &pdevice->memory.types[pAllocateInfo->memoryTypeIndex]; */
   mem->map = NULL;
   mem->map_size = 0;

   /* FIXME: stub */

   return VK_SUCCESS;
}

void
v3dv_FreeMemory(VkDevice _device,
                VkDeviceMemory _mem,
                const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   if (mem->map)
      v3dv_UnmapMemory(_device, _mem);

   /* FIXME: stub */

   vk_free2(&device->alloc, pAllocator, mem);
}

VkResult
v3dv_MapMemory(VkDevice _device,
               VkDeviceMemory _memory,
               VkDeviceSize offset,
               VkDeviceSize size,
               VkMemoryMapFlags flags,
               void **ppData)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   /* FIXME: stub */

   return vk_error(device->instance, VK_ERROR_MEMORY_MAP_FAILED);
}

void
v3dv_UnmapMemory(VkDevice _device,
                 VkDeviceMemory _memory)
{
   /* FIXME: stub */
}

VkResult
v3dv_FlushMappedMemoryRanges(VkDevice _device,
                             uint32_t memoryRangeCount,
                             const VkMappedMemoryRange *pMemoryRanges)
{
   /* FIXME: stub (although note that both radv and tu just returns success
    * here. Pending further research)
    */
   return VK_SUCCESS;
}

VkResult
v3dv_InvalidateMappedMemoryRanges(VkDevice _device,
                                  uint32_t memoryRangeCount,
                                  const VkMappedMemoryRange *pMemoryRanges)
{
   /* FIXME: stub (although note that both radv and tu just returns success
    * here. Pending further research)
    */
   return VK_SUCCESS;
}

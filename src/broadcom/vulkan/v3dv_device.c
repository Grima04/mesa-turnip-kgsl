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
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <xf86drm.h>

#include "v3dv_private.h"

#include "common/v3d_debug.h"

#include "broadcom/cle/v3dx_pack.h"

#include "compiler/v3d_compiler.h"
#include "compiler/glsl_types.h"

#include "drm-uapi/v3d_drm.h"
#include "format/u_format.h"
#include "vk_util.h"

#ifdef VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#endif

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
   /* We don't support any layers  */
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

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

   v3d_process_debug_variable();

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
   close(device->render_fd);
   if (device->display_fd >= 0)
      close(device->display_fd);

   free(device->name);

#if using_v3d_simulator
   v3d_simulator_destroy(device->sim_file);
#endif
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

static uint64_t
compute_heap_size()
{
   /* Query the total ram from the system */
   struct sysinfo info;
   sysinfo(&info);

   uint64_t total_ram = (uint64_t)info.totalram * (uint64_t)info.mem_unit;

   /* We don't want to burn too much ram with the GPU.  If the user has 4GiB
    * or less, we use at most half.  If they have more than 4GiB, we use 3/4.
    */
   uint64_t available_ram;
   if (total_ram <= 4ull * 1024ull * 1024ull * 1024ull)
      available_ram = total_ram / 2;
   else
      available_ram = total_ram * 3 / 4;

   return available_ram;
}

/* When running on the simulator we do everything on a single render node so
 * we don't need to get an authenticated display fd from the display server.
 */
#if !using_v3d_simulator
#ifdef VK_USE_PLATFORM_XCB_KHR
static int
create_display_fd_xcb()
{
   xcb_connection_t *conn = xcb_connect(NULL, NULL);
   const xcb_setup_t *setup = xcb_get_setup(conn);
   xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
   xcb_screen_t *screen = iter.data;

   xcb_dri3_open_cookie_t cookie;
   xcb_dri3_open_reply_t *reply;
   cookie = xcb_dri3_open(conn, screen->root, None);
   reply = xcb_dri3_open_reply(conn, cookie, NULL);
   if (!reply)
      return -1;

   if (reply->nfd != 1) {
      free(reply);
      return -1;
   }

   int fd = xcb_dri3_open_reply_fds(conn, reply)[0];
   free(reply);
   fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

   return fd;
}
#endif
#endif

static VkResult
physical_device_init(struct v3dv_physical_device *device,
                     struct v3dv_instance *instance,
                     drmDevicePtr drm_device)
{
   VkResult result = VK_SUCCESS;
   int32_t display_fd = -1;

   device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   device->instance = instance;

   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   int32_t render_fd = open(path, O_RDWR | O_CLOEXEC);
   if (render_fd < 0)
      return vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);

   /* If we are running on real hardware we need to open the vc4 display
    * device so we can allocate winsys BOs for the v3d core to render into.
    */
#if !using_v3d_simulator
#ifdef VK_USE_PLATFORM_XCB_KHR
   display_fd = create_display_fd_xcb();
#endif

   if (display_fd == -1) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail;
   }
#endif

   device->render_fd = render_fd;       /* The v3d render node  */
   device->display_fd = display_fd;    /* The vc4 primary node */

   uint8_t zeroes[VK_UUID_SIZE] = { 0 };
   memcpy(device->pipeline_cache_uuid, zeroes, VK_UUID_SIZE);

#if using_v3d_simulator
   device->sim_file = v3d_simulator_init(device->render_fd);
#endif

   if (!v3d_get_device_info(device->render_fd, &device->devinfo, &v3dv_ioctl)) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail;
   }

   device->compiler = v3d_compiler_init(&device->devinfo);
   device->next_program_id = 0;

   asprintf(&device->name, "V3D %d.%d",
            device->devinfo.ver / 10, device->devinfo.ver % 10);

   /* Setup available memory heaps and types */
   VkPhysicalDeviceMemoryProperties *mem = &device->memory;
   mem->memoryHeapCount = 1;
   mem->memoryHeaps[0].size = compute_heap_size();
   mem->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   mem->memoryTypeCount = 2;

   /* This is the only combination required by the spec */
   mem->memoryTypes[0].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   mem->memoryTypes[0].heapIndex = 0;

   mem->memoryTypes[1].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
   mem->memoryTypes[1].heapIndex = 0;

   device->options.merge_jobs = getenv("V3DV_NO_MERGE_JOBS") == NULL;

   result = v3dv_wsi_init(device);
   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail;
   }

   v3dv_physical_device_get_supported_extensions(device,
                                                 &device->supported_extensions);
   return VK_SUCCESS;

fail:
   if (render_fd >= 0)
      close(render_fd);
   if (display_fd >= 0)
      close(display_fd);

   return result;
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

#if !using_v3d_simulator
   int32_t v3d_idx = -1;
   int32_t vc4_idx = -1;
#endif
   for (unsigned i = 0; i < (unsigned)max_devices; i++) {
#if using_v3d_simulator
      /* In the simulator, we look for an Intel render node */
      if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER &&
          devices[i]->bustype == DRM_BUS_PCI &&
          devices[i]->deviceinfo.pci->vendor_id == 0x8086) {
         result = physical_device_init(&instance->physicalDevice, instance,
                                       devices[i]);
         if (result != VK_ERROR_INCOMPATIBLE_DRIVER)
            break;
      }
#else
      /* On actual hardware, we should have a render node (v3d)
       * and a primary node (vc4). We will need to use the primary
       * to allocate WSI buffers and share them with the render node
       * via prime, but that is a privileged operation so we need the
       * primary node to be authenticated, and for that we need the
       * display server to provide the device fd (with DRI3), so we
       * here we only check that the device is present but we don't
       * try to open it.
       */
      if (devices[i]->bustype != DRM_BUS_PLATFORM)
         continue;

      if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER) {
         char **compat = devices[i]->deviceinfo.platform->compatible;
         while (*compat) {
            if (strncmp(*compat, "brcm,2711-v3d", 13) == 0) {
               v3d_idx = i;
               break;
            }
            compat++;
         }
      } else if (devices[i]->available_nodes & 1 << DRM_NODE_PRIMARY) {
         char **compat = devices[i]->deviceinfo.platform->compatible;
         while (*compat) {
            if (strncmp(*compat, "brcm,bcm2835-vc4", 16) == 0) {
               vc4_idx = i;
               break;
            }
            compat++;
         }
      }
#endif
   }

#if !using_v3d_simulator
   if (v3d_idx == -1 || vc4_idx == -1)
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
   else
      result = physical_device_init(&instance->physicalDevice, instance,
                                    devices[v3d_idx]);
#endif

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
   memset(pFeatures, 0, sizeof(*pFeatures));

   *pFeatures = (VkPhysicalDeviceFeatures) {
      .robustBufferAccess = false,
      .fullDrawIndexUint32 = false,
      .imageCubeArray = false,
      .independentBlend = false,
      .geometryShader = false,
      .tessellationShader = false,
      .sampleRateShading = false,
      .dualSrcBlend = false,
      .logicOp = false,
      .multiDrawIndirect = false,
      .drawIndirectFirstInstance = false,
      .depthClamp = false,
      .depthBiasClamp = false,
      .fillModeNonSolid = false,
      .depthBounds = false,
      .wideLines = false,
      .largePoints = false,
      .alphaToOne = false,
      .multiViewport = false,
      .samplerAnisotropy = false,
      .textureCompressionETC2 = false,
      .textureCompressionASTC_LDR = false,
      .textureCompressionBC = false,
      .occlusionQueryPrecise = false,
      .pipelineStatisticsQuery = false,
      .vertexPipelineStoresAndAtomics = true,
      .fragmentStoresAndAtomics = true,
      .shaderTessellationAndGeometryPointSize = false,
      .shaderImageGatherExtended = false,
      .shaderStorageImageExtendedFormats = false,
      .shaderStorageImageMultisample = false,
      .shaderStorageImageReadWithoutFormat = false,
      .shaderStorageImageWriteWithoutFormat = false,
      .shaderUniformBufferArrayDynamicIndexing = false,
      .shaderSampledImageArrayDynamicIndexing = false,
      .shaderStorageBufferArrayDynamicIndexing = false,
      .shaderStorageImageArrayDynamicIndexing = false,
      .shaderClipDistance = false,
      .shaderCullDistance = false,
      .shaderFloat64 = false,
      .shaderInt64 = false,
      .shaderInt16 = false,
      .shaderResourceResidency = false,
      .shaderResourceMinLod = false,
      .sparseBinding = false,
      .sparseResidencyBuffer = false,
      .sparseResidencyImage2D = false,
      .sparseResidencyImage3D = false,
      .sparseResidency2Samples = false,
      .sparseResidency4Samples = false,
      .sparseResidency8Samples = false,
      .sparseResidency16Samples = false,
      .sparseResidencyAliased = false,
      .variableMultisampleRate = false,
      .inheritedQueries = false,
   };
}

void
v3dv_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceFeatures2 *pFeatures)
{
   v3dv_GetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);

   vk_foreach_struct(ext, pFeatures->pNext) {
      switch (ext->sType) {
      default:
         v3dv_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

void
v3dv_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties *pProperties)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, pdevice, physicalDevice);

   const uint32_t page_size = 4096;
   const uint32_t mem_size = compute_heap_size();

   /* Per-stage limits */
   const uint32_t max_samplers = 16;
   const uint32_t max_uniform_buffers = 12;
   const uint32_t max_storage_buffers = 4;
   const uint32_t max_sampled_images = 16;
   const uint32_t max_storage_images = 4;

   const uint32_t max_varying_components = 16 * 4;
   const uint32_t max_render_targets = 4;

   const uint32_t v3d_coord_shift = 6;
   const uint32_t v3d_coord_scale = (1 << v3d_coord_shift);
   const float point_size_granularity = 2.0f / v3d_coord_scale;

   const uint32_t max_fb_size = 4096;

   const VkSampleCountFlags supported_sample_counts = VK_SAMPLE_COUNT_1_BIT;

   /* FIXME: this will probably require an in-depth review */
   VkPhysicalDeviceLimits limits = {
      .maxImageDimension1D                      = 4096,
      .maxImageDimension2D                      = 4096,
      .maxImageDimension3D                      = 4096,
      .maxImageDimensionCube                    = 4096,
      .maxImageArrayLayers                      = 2048,
      .maxTexelBufferElements                   = (1ul << 28),
      .maxUniformBufferRange                    = (1ul << 27) - 1,
      .maxStorageBufferRange                    = (1ul << 27) - 1,
      .maxPushConstantsSize                     = MAX_PUSH_CONSTANTS_SIZE,
      .maxMemoryAllocationCount                 = mem_size / page_size,
      .maxSamplerAllocationCount                = 64 * 1024,
      .bufferImageGranularity                   = 256, /* A cache line */
      .sparseAddressSpaceSize                   = 0,
      .maxBoundDescriptorSets                   = MAX_SETS,
      .maxPerStageDescriptorSamplers            = max_samplers,
      .maxPerStageDescriptorUniformBuffers      = max_uniform_buffers,
      .maxPerStageDescriptorStorageBuffers      = max_storage_buffers,
      .maxPerStageDescriptorSampledImages       = max_sampled_images,
      .maxPerStageDescriptorStorageImages       = max_storage_images,
      .maxPerStageDescriptorInputAttachments    = 4,
      .maxPerStageResources                     = 128,

      /* We multiply some limits by 6 to account for all shader stages */
      .maxDescriptorSetSamplers                 = 6 * max_samplers,
      .maxDescriptorSetUniformBuffers           = 6 * max_uniform_buffers,
      .maxDescriptorSetUniformBuffersDynamic    = 8,
      .maxDescriptorSetStorageBuffers           = 6 * max_storage_buffers,
      .maxDescriptorSetStorageBuffersDynamic    = 4,
      .maxDescriptorSetSampledImages            = 6 * max_sampled_images,
      .maxDescriptorSetStorageImages            = 6 * max_storage_images,
      .maxDescriptorSetInputAttachments         = 4,

      /* Vertex limits */
      .maxVertexInputAttributes                 = MAX_VERTEX_ATTRIBS,
      .maxVertexInputBindings                   = MAX_VBS,
      .maxVertexInputAttributeOffset            = 0xffffffff,
      .maxVertexInputBindingStride              = 0xffffffff,
      .maxVertexOutputComponents                = max_varying_components,

      /* Tessellation limits */
      .maxTessellationGenerationLevel           = 0,
      .maxTessellationPatchSize                 = 0,
      .maxTessellationControlPerVertexInputComponents = 0,
      .maxTessellationControlPerVertexOutputComponents = 0,
      .maxTessellationControlPerPatchOutputComponents = 0,
      .maxTessellationControlTotalOutputComponents = 0,
      .maxTessellationEvaluationInputComponents = 0,
      .maxTessellationEvaluationOutputComponents = 0,

      /* Geometry limits */
      .maxGeometryShaderInvocations             = 0,
      .maxGeometryInputComponents               = 0,
      .maxGeometryOutputComponents              = 0,
      .maxGeometryOutputVertices                = 0,
      .maxGeometryTotalOutputComponents         = 0,

      /* Fragment limits */
      .maxFragmentInputComponents               = max_varying_components,
      .maxFragmentOutputAttachments             = 4,
      .maxFragmentDualSrcAttachments            = 0,
      .maxFragmentCombinedOutputResources       = max_render_targets +
                                                  max_storage_buffers +
                                                  max_storage_images,

      /* Compute limits */
      .maxComputeSharedMemorySize               = 16384,
      .maxComputeWorkGroupCount                 = { 65535, 65535, 65535 },
      .maxComputeWorkGroupInvocations           = 256,
      .maxComputeWorkGroupSize                  = { 256, 256, 256 },

      .subPixelPrecisionBits                    = v3d_coord_shift,
      .subTexelPrecisionBits                    = 8,
      .mipmapPrecisionBits                      = 8,
      .maxDrawIndexedIndexValue                 = 0x00ffffff,
      .maxDrawIndirectCount                     = 0x7fffffff,
      .maxSamplerLodBias                        = 14.0f,
      .maxSamplerAnisotropy                     = 16.0f,
      .maxViewports                             = MAX_VIEWPORTS,
      .maxViewportDimensions                    = { max_fb_size, max_fb_size },
      .viewportBoundsRange                      = { -2.0 * max_fb_size,
                                                    2.0 * max_fb_size - 1 },
      .viewportSubPixelBits                     = 0,
      .minMemoryMapAlignment                    = page_size,
      .minTexelBufferOffsetAlignment            = 16,
      .minUniformBufferOffsetAlignment          = 32,
      .minStorageBufferOffsetAlignment          = 32,
      .minTexelOffset                           = -8,
      .maxTexelOffset                           = 7,
      .minTexelGatherOffset                     = -8,
      .maxTexelGatherOffset                     = 7,
      .minInterpolationOffset                   = -0.5,
      .maxInterpolationOffset                   = 0.5,
      .subPixelInterpolationOffsetBits          = v3d_coord_shift,
      .maxFramebufferWidth                      = max_fb_size,
      .maxFramebufferHeight                     = max_fb_size,
      .maxFramebufferLayers                     = 256,
      .framebufferColorSampleCounts             = supported_sample_counts,
      .framebufferDepthSampleCounts             = supported_sample_counts,
      .framebufferStencilSampleCounts           = supported_sample_counts,
      .framebufferNoAttachmentsSampleCounts     = supported_sample_counts,
      .maxColorAttachments                      = max_render_targets,
      .sampledImageColorSampleCounts            = supported_sample_counts,
      .sampledImageIntegerSampleCounts          = supported_sample_counts,
      .sampledImageDepthSampleCounts            = supported_sample_counts,
      .sampledImageStencilSampleCounts          = supported_sample_counts,
      .storageImageSampleCounts                 = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords                       = 1,
      .timestampComputeAndGraphics              = false,
      .timestampPeriod                          = 0.0f,
      .maxClipDistances                         = 0,
      .maxCullDistances                         = 0,
      .maxCombinedClipAndCullDistances          = 0,
      .discreteQueuePriorities                  = 2,
      .pointSizeRange                           = { point_size_granularity,
                                                    512.0f },
      .lineWidthRange                           = { 1.0f, 1.0f },
      .pointSizeGranularity                     = point_size_granularity,
      .lineWidthGranularity                     = 0.0f,
      .strictLines                              = true,
      .standardSampleLocations                  = false,
      .optimalBufferCopyOffsetAlignment         = 32,
      .optimalBufferCopyRowPitchAlignment       = 32,
      .nonCoherentAtomSize                      = 256,
   };

   /* FIXME:
    * Getting deviceID and UUID will probably require to use the kernel pci
    * interface. See this:
    * https://www.kernel.org/doc/html/latest/PCI/pci.html#how-to-find-pci-devices-manually
    * And check the getparam ioctl in the i915 kernel with CHIPSET_ID for
    * example.
    */
   *pProperties = (VkPhysicalDeviceProperties) {
      .apiVersion = v3dv_physical_device_api_version(pdevice),
      .driverVersion = vk_get_driver_version(),
      .vendorID = 0x14E4,
      .deviceID = 0, /* FIXME */
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      .limits = limits,
      .sparseProperties = { 0 },
   };

   snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
            "%s", pdevice->name);
   memcpy(pProperties->pipelineCacheUUID,
          pdevice->pipeline_cache_uuid, VK_UUID_SIZE);
}

void
v3dv_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceProperties2 *pProperties)
{
   v3dv_GetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

   vk_foreach_struct(ext, pProperties->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
         VkPhysicalDeviceIDProperties *id_props =
            (VkPhysicalDeviceIDProperties *)ext;
         /* FIXME */
         memset(id_props->deviceUUID, 0, VK_UUID_SIZE);
         memset(id_props->driverUUID, 0, VK_UUID_SIZE);
         /* The LUID is for Windows. */
         id_props->deviceLUIDValid = false;
         break;
      }
      default:
         v3dv_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

/* We support exactly one queue family. */
static const VkQueueFamilyProperties
v3dv_queue_family_properties = {
   .queueFlags = VK_QUEUE_GRAPHICS_BIT |
                 VK_QUEUE_COMPUTE_BIT |
                 VK_QUEUE_TRANSFER_BIT,
   .queueCount = 1,
   .timestampValidBits = 0, /* FIXME */
   .minImageTransferGranularity = { 1, 1, 1 },
};

void
v3dv_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                            uint32_t *pCount,
                                            VkQueueFamilyProperties *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pCount);

   vk_outarray_append(&out, p) {
      *p = v3dv_queue_family_properties;
   }
}

void
v3dv_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                             uint32_t *pQueueFamilyPropertyCount,
                                             VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);

   vk_outarray_append(&out, p) {
      p->queueFamilyProperties = v3dv_queue_family_properties;

      vk_foreach_struct(s, p->pNext) {
         v3dv_debug_ignored_stype(s->sType);
      }
   }
}

void
v3dv_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                       VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, device, physicalDevice);
   *pMemoryProperties = device->memory;
}

void
v3dv_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                        VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   v3dv_GetPhysicalDeviceMemoryProperties(physicalDevice,
                                          &pMemoryProperties->memoryProperties);

   vk_foreach_struct(ext, pMemoryProperties->pNext) {
      switch (ext->sType) {
      default:
         v3dv_debug_ignored_stype(ext->sType);
         break;
      }
   }
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
   /* We don't support any layers */
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   V3DV_FROM_HANDLE(v3dv_physical_device, device, physicalDevice);
   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

   for (int i = 0; i < V3DV_DEVICE_EXTENSION_COUNT; i++) {
      if (device->supported_extensions.extensions[i]) {
         vk_outarray_append(&out, prop) {
            *prop = v3dv_device_extensions[i];
         }
      }
   }

   return vk_outarray_status(&out);
}

VkResult
v3dv_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                      VkLayerProperties *pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
}

VkResult
v3dv_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                    uint32_t *pPropertyCount,
                                    VkLayerProperties *pProperties)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, physical_device, physicalDevice);

   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   return vk_error(physical_device->instance, VK_ERROR_LAYER_NOT_PRESENT);
}

static VkResult
queue_init(struct v3dv_device *device, struct v3dv_queue *queue)
{
   queue->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   queue->device = device;
   queue->flags = 0;
   return VK_SUCCESS;
}

static void
queue_finish(struct v3dv_queue *queue)
{
}

static void
init_device_dispatch(struct v3dv_device *device)
{
   for (unsigned i = 0; i < ARRAY_SIZE(device->dispatch.entrypoints); i++) {
      /* Vulkan requires that entrypoints for extensions which have not been
       * enabled must not be advertised.
       */
      if (!v3dv_device_entrypoint_is_enabled(i, device->instance->app_info.api_version,
                                             &device->instance->enabled_extensions,
                                             &device->enabled_extensions)) {
         device->dispatch.entrypoints[i] = NULL;
      } else {
         device->dispatch.entrypoints[i] =
            v3dv_device_dispatch_table.entrypoints[i];
      }
   }
}

VkResult
v3dv_CreateDevice(VkPhysicalDevice physicalDevice,
                  const VkDeviceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkDevice *pDevice)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, physical_device, physicalDevice);
   struct v3dv_instance *instance = physical_device->instance;
   VkResult result;
   struct v3dv_device *device;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

   /* Check enabled extensions */
   struct v3dv_device_extension_table enabled_extensions = { };
   for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      int idx;
      for (idx = 0; idx < V3DV_DEVICE_EXTENSION_COUNT; idx++) {
         if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                    v3dv_device_extensions[idx].extensionName) == 0)
            break;
      }

      if (idx >= V3DV_DEVICE_EXTENSION_COUNT)
         return vk_error(instance, VK_ERROR_EXTENSION_NOT_PRESENT);

      if (!physical_device->supported_extensions.extensions[idx])
         return vk_error(instance, VK_ERROR_EXTENSION_NOT_PRESENT);

      enabled_extensions.extensions[idx] = true;
   }

   /* Check enabled features */
   if (pCreateInfo->pEnabledFeatures) {
      VkPhysicalDeviceFeatures supported_features;
      v3dv_GetPhysicalDeviceFeatures(physicalDevice, &supported_features);
      VkBool32 *supported_feature = (VkBool32 *)&supported_features;
      VkBool32 *enabled_feature = (VkBool32 *)pCreateInfo->pEnabledFeatures;
      unsigned num_features = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
      for (uint32_t i = 0; i < num_features; i++) {
         if (enabled_feature[i] && !supported_feature[i])
            return vk_error(instance, VK_ERROR_FEATURE_NOT_PRESENT);
      }
   }

   /* Check requested queues (we only expose one queue ) */
   assert(pCreateInfo->queueCreateInfoCount == 1);
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      assert(pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex == 0);
      assert(pCreateInfo->pQueueCreateInfos[i].queueCount == 1);
      if (pCreateInfo->pQueueCreateInfos[i].flags != 0)
         return vk_error(instance, VK_ERROR_INITIALIZATION_FAILED);
   }

   device = vk_alloc2(&physical_device->instance->alloc, pAllocator,
                       sizeof(*device), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   device->instance = instance;

   if (pAllocator)
      device->alloc = *pAllocator;
   else
      device->alloc = physical_device->instance->alloc;

   device->render_fd = physical_device->render_fd;
   if (device->render_fd == -1) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto fail;
   }

   if (physical_device->display_fd != -1) {
      device->display_fd = physical_device->display_fd;
      if (device->display_fd == -1) {
         result = VK_ERROR_INITIALIZATION_FAILED;
         goto fail;
      }
   } else {
      device->display_fd = -1;
   }

   result = queue_init(device, &device->queue);
   if (result != VK_SUCCESS)
      goto fail;

   device->devinfo = physical_device->devinfo;
   device->enabled_extensions = enabled_extensions;

   int ret = drmSyncobjCreate(device->render_fd,
                              DRM_SYNCOBJ_CREATE_SIGNALED,
                              &device->last_job_sync);
   if (ret) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto fail;
   }

   init_device_dispatch(device);

   *pDevice = v3dv_device_to_handle(device);

   return VK_SUCCESS;

fail:
   vk_free(&device->alloc, device);

   return result;
}

void
v3dv_DestroyDevice(VkDevice _device,
                   const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   drmSyncobjDestroy(device->render_fd, device->last_job_sync);
   queue_finish(&device->queue);

   vk_free2(&default_alloc, pAllocator, device);
}

void
v3dv_GetDeviceQueue(VkDevice _device,
                    uint32_t queueFamilyIndex,
                    uint32_t queueIndex,
                    VkQueue *pQueue)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   assert(queueIndex == 0);
   assert(queueFamilyIndex == 0);

   *pQueue = v3dv_queue_to_handle(&device->queue);
}

VkResult
v3dv_DeviceWaitIdle(VkDevice _device)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   int ret = drmSyncobjWait(device->render_fd,
                            &device->last_job_sync, 1, INT64_MAX, 0, NULL);
   if (ret)
      return VK_ERROR_DEVICE_LOST;

   return VK_SUCCESS;
}

VkResult
v3dv_QueueWaitIdle(VkQueue _queue)
{
   V3DV_FROM_HANDLE(v3dv_queue, queue, _queue);
   return v3dv_DeviceWaitIdle(v3dv_device_to_handle(queue->device));
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

static VkResult
device_alloc(struct v3dv_device *device,
             struct v3dv_device_memory *mem,
             VkDeviceSize size)
{
   /* Our kernel interface is 32-bit */
   assert((size & 0xffffffff) == size);
   mem->bo = v3dv_bo_alloc(device, size, "device_alloc");
   if (!mem->bo)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   return VK_SUCCESS;
}

static void
device_free(struct v3dv_device *device, struct v3dv_device_memory *mem)
{
   v3dv_bo_free(device, mem->bo);
}

static VkResult
device_map(struct v3dv_device *device,
           struct v3dv_device_memory *mem,
           uint32_t size)
{
   /* From the spec:
    *
    *   "After a successful call to vkMapMemory the memory object memory is
    *   considered to be currently host mapped. It is an application error to
    *   call vkMapMemory on a memory object that is already host mapped."
    */
   assert(mem && mem->bo->map == NULL);

   bool ok = v3dv_bo_map(device, mem->bo, size);
   if (!ok)
      return VK_ERROR_MEMORY_MAP_FAILED;

   return VK_SUCCESS;
}

static void
device_unmap(struct v3dv_device *device, struct v3dv_device_memory *mem)
{
   assert(mem && mem->bo->map && mem->bo->map_size > 0);
   v3dv_bo_unmap(device, mem->bo);
}

static VkResult
device_import_bo(struct v3dv_device *device,
                 const VkAllocationCallbacks *pAllocator,
                 int fd, uint64_t size,
                 struct v3dv_bo **bo)
{
   VkResult result;

   *bo = vk_alloc2(&device->alloc, pAllocator, sizeof(struct v3dv_bo), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (*bo == NULL) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   off_t real_size = lseek(fd, 0, SEEK_END);
   lseek(fd, 0, SEEK_SET);
   if (real_size < 0 || (uint64_t) real_size < size) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   int ret;
   uint32_t handle;
   ret = drmPrimeFDToHandle(device->render_fd, fd, &handle);
   if (ret) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }

   struct drm_v3d_get_bo_offset get_offset = {
      .handle = handle,
   };
   ret = v3dv_ioctl(device->render_fd, DRM_IOCTL_V3D_GET_BO_OFFSET, &get_offset);
   if (ret) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto fail;
   }
   assert(get_offset.offset != 0);

   (*bo)->handle = handle;
   (*bo)->size = size;
   (*bo)->offset = get_offset.offset;
   (*bo)->map = NULL;
   (*bo)->map_size = 0;

   return VK_SUCCESS;

fail:
   if (*bo) {
      vk_free2(&device->alloc, pAllocator, *bo);
      *bo = NULL;
   }
   return result;
}

static VkResult
device_alloc_for_wsi(struct v3dv_device *device,
                     const VkAllocationCallbacks *pAllocator,
                     struct v3dv_device_memory *mem,
                     VkDeviceSize size)
{
   /* In the simulator we can get away with a regular allocation since both
    * allocation and rendering happen in the same DRM render node. On actual
    * hardware we need to allocate our winsys BOs on the vc4 display device
    * and import them into v3d.
    */
#if using_v3d_simulator
      return device_alloc(device, mem, size);
#else
   assert(device->display_fd != -1);
   int display_fd = device->instance->physicalDevice.display_fd;
   struct drm_mode_create_dumb create_dumb = {
      .width = 1024, /* one page */
      .height = align(size, 4096) / 4096,
      .bpp = util_format_get_blocksizebits(PIPE_FORMAT_RGBA8888_UNORM),
   };

   int err;
   err = v3dv_ioctl(display_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
   if (err < 0)
      goto fail_create;

   int fd;
   err =
      drmPrimeHandleToFD(display_fd, create_dumb.handle, O_CLOEXEC, &fd);
   if (err < 0)
      goto fail_export;

   VkResult result = device_import_bo(device, pAllocator, fd, size, &mem->bo);
   close(fd);
   if (result != VK_SUCCESS)
      goto fail_import;


   return VK_SUCCESS;

fail_import:
fail_export: {
      struct drm_mode_destroy_dumb destroy_dumb = {
         .handle = create_dumb.handle,
      };
      v3dv_ioctl(display_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
   }

fail_create:
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
#endif
}

VkResult
v3dv_AllocateMemory(VkDevice _device,
                    const VkMemoryAllocateInfo *pAllocateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkDeviceMemory *pMem)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_device_memory *mem;
   struct v3dv_physical_device *pdevice = &device->instance->physicalDevice;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   /* The Vulkan 1.0.33 spec says "allocationSize must be greater than 0". */
   assert(pAllocateInfo->allocationSize > 0);

   mem = vk_alloc2(&device->alloc, pAllocator, sizeof(*mem), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (mem == NULL)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   assert(pAllocateInfo->memoryTypeIndex < pdevice->memory.memoryTypeCount);
   mem->type = &pdevice->memory.memoryTypes[pAllocateInfo->memoryTypeIndex];

   const struct wsi_memory_allocate_info *wsi_info = NULL;
   const VkImportMemoryFdInfoKHR *fd_info = NULL;
   vk_foreach_struct_const(ext, pAllocateInfo->pNext) {
      switch ((unsigned)ext->sType) {
      case VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA:
         wsi_info = (void *)ext;
         break;
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
         fd_info = (void *)ext;
         break;
      default:
         v3dv_debug_ignored_stype(ext->sType);
         break;
      }
   }

   VkResult result = VK_SUCCESS;
   if (wsi_info) {
      result = device_alloc_for_wsi(device, pAllocator, mem,
                                    pAllocateInfo->allocationSize);
   } else if (fd_info && fd_info->handleType) {
      assert(fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
             fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      result = device_import_bo(device, pAllocator,
                                fd_info->fd, pAllocateInfo->allocationSize,
                                &mem->bo);
      if (result == VK_SUCCESS)
         close(fd_info->fd);
   } else {
      result = device_alloc(device, mem, pAllocateInfo->allocationSize);
   }

   if (result != VK_SUCCESS) {
      vk_free2(&device->alloc, pAllocator, mem);
      return vk_error(device->instance, result);
   }

   *pMem = v3dv_device_memory_to_handle(mem);
   return result;
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

   if (mem->bo->map)
      v3dv_UnmapMemory(_device, _mem);

   device_free(device, mem);

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

   assert(offset < mem->bo->size);

   /* We always map from the beginning of the region, so if our offset
    * is not 0 and we are not mapping the entire region, we need to
    * add the offset to the map size.
    */
   if (size == VK_WHOLE_SIZE)
      size = mem->bo->size;
   else if (offset > 0)
      size += offset;

   VkResult result = device_map(device, mem, size);
   if (result != VK_SUCCESS)
      return vk_error(device->instance, result);

   *ppData = ((uint8_t *) mem->bo->map) + offset;
   return VK_SUCCESS;
}

void
v3dv_UnmapMemory(VkDevice _device,
                 VkDeviceMemory _memory)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _memory);

   if (mem == NULL)
      return;

   device_unmap(device, mem);
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

void
v3dv_GetImageMemoryRequirements(VkDevice _device,
                                VkImage _image,
                                VkMemoryRequirements *pMemoryRequirements)
{
   V3DV_FROM_HANDLE(v3dv_image, image, _image);

   assert(image->size > 0);

   pMemoryRequirements->size = image->size;
   pMemoryRequirements->alignment = image->alignment;
   pMemoryRequirements->memoryTypeBits = 0x3; /* Both memory types */
}

VkResult
v3dv_BindImageMemory(VkDevice _device,
                     VkImage _image,
                     VkDeviceMemory _memory,
                     VkDeviceSize memoryOffset)
{
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _memory);
   V3DV_FROM_HANDLE(v3dv_image, image, _image);

   /* Valid usage:
    *
    *   "memoryOffset must be an integer multiple of the alignment member of
    *    the VkMemoryRequirements structure returned from a call to
    *    vkGetImageMemoryRequirements with image"
    */
   assert(memoryOffset % image->alignment == 0);
   assert(memoryOffset < mem->bo->size);

   image->mem = mem;
   image->mem_offset = memoryOffset;

   return VK_SUCCESS;
}

void
v3dv_GetBufferMemoryRequirements(VkDevice _device,
                                 VkBuffer _buffer,
                                 VkMemoryRequirements* pMemoryRequirements)
{
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, _buffer);

   pMemoryRequirements->memoryTypeBits = 0x3; /* Both memory types */
   pMemoryRequirements->alignment = buffer->alignment;
   pMemoryRequirements->size =
      align64(buffer->size, pMemoryRequirements->alignment);
}

VkResult
v3dv_BindBufferMemory(VkDevice _device,
                      VkBuffer _buffer,
                      VkDeviceMemory _memory,
                      VkDeviceSize memoryOffset)
{
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _memory);
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, _buffer);

   /* Valid usage:
    *
    *   "memoryOffset must be an integer multiple of the alignment member of
    *    the VkMemoryRequirements structure returned from a call to
    *    vkGetBufferMemoryRequirements with buffer"
    */
   assert(memoryOffset % buffer->alignment == 0);
   assert(memoryOffset < mem->bo->size);

   buffer->mem = mem;
   buffer->mem_offset = memoryOffset;

   return VK_SUCCESS;
}

VkResult
v3dv_CreateBuffer(VkDevice  _device,
                  const VkBufferCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkBuffer *pBuffer)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_buffer *buffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
   assert(pCreateInfo->usage != 0);

   /* We don't support any flags for now */
   assert(pCreateInfo->flags == 0);

   buffer = vk_alloc2(&device->alloc, pAllocator, sizeof(*buffer), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   buffer->size = pCreateInfo->size;
   buffer->usage = pCreateInfo->usage;
   buffer->alignment = 256; /* nonCoherentAtomSize */

   assert((buffer->size & 0xffffffff) == buffer->size);

   *pBuffer = v3dv_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

void
v3dv_DestroyBuffer(VkDevice _device,
                   VkBuffer _buffer,
                   const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, _buffer);

   if (!buffer)
      return;

   vk_free2(&device->alloc, pAllocator, buffer);
}

static void
compute_internal_bpp_from_attachments(struct v3dv_framebuffer *framebuffer)
{
   STATIC_ASSERT(RENDER_TARGET_MAXIMUM_32BPP == 0);
   uint8_t max_bpp = RENDER_TARGET_MAXIMUM_32BPP;
   for (uint32_t i = 0; i < framebuffer->attachment_count; i++) {
      const struct v3dv_image_view *att = framebuffer->attachments[i];
      assert(att);

      if (att->aspects & VK_IMAGE_ASPECT_COLOR_BIT)
         max_bpp = MAX2(max_bpp, att->internal_bpp);
   }
   framebuffer->internal_bpp = max_bpp;
}

void
v3dv_framebuffer_compute_tiling_params(struct v3dv_framebuffer *framebuffer)
{
   static const uint8_t tile_sizes[] = {
      64, 64,
      64, 32,
      32, 32,
      32, 16,
      16, 16,
   };

   uint32_t tile_size_index = 0;

   /* FIXME: MSAA */

   if (framebuffer->color_attachment_count > 2)
      tile_size_index += 2;
   else if (framebuffer->color_attachment_count > 1)
      tile_size_index += 1;

   tile_size_index += framebuffer->internal_bpp;
   assert(tile_size_index < ARRAY_SIZE(tile_sizes));

   framebuffer->tile_width = tile_sizes[tile_size_index * 2];
   framebuffer->tile_height = tile_sizes[tile_size_index * 2 + 1];

   framebuffer->draw_tiles_x =
      DIV_ROUND_UP(framebuffer->width, framebuffer->tile_width);
   framebuffer->draw_tiles_y =
      DIV_ROUND_UP(framebuffer->height, framebuffer->tile_height);

   /* Size up our supertiles until we get under the limit */
   const uint32_t max_supertiles = 256;
   framebuffer->supertile_width = 1;
   framebuffer->supertile_height = 1;
   for (;;) {
      framebuffer->frame_width_in_supertiles =
         DIV_ROUND_UP(framebuffer->draw_tiles_x, framebuffer->supertile_width);
      framebuffer->frame_height_in_supertiles =
         DIV_ROUND_UP(framebuffer->draw_tiles_y, framebuffer->supertile_height);
      const uint32_t num_supertiles = framebuffer->frame_width_in_supertiles *
                                      framebuffer->frame_height_in_supertiles;
      if (num_supertiles < max_supertiles)
         break;

      if (framebuffer->supertile_width < framebuffer->supertile_height)
         framebuffer->supertile_width++;
      else
         framebuffer->supertile_height++;
   }
}

VkResult
v3dv_CreateFramebuffer(VkDevice _device,
                       const VkFramebufferCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkFramebuffer *pFramebuffer)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_framebuffer *framebuffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);

   size_t size = sizeof(*framebuffer) +
                 sizeof(struct v3dv_image_view *) * pCreateInfo->attachmentCount;
   framebuffer = vk_alloc2(&device->alloc, pAllocator, size, 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (framebuffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   framebuffer->width = pCreateInfo->width;
   framebuffer->height = pCreateInfo->height;
   framebuffer->layers = pCreateInfo->layers;
   framebuffer->attachment_count = pCreateInfo->attachmentCount;
   framebuffer->color_attachment_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      framebuffer->attachments[i] =
         v3dv_image_view_from_handle(pCreateInfo->pAttachments[i]);
      if (framebuffer->attachments[i]->aspects & VK_IMAGE_ASPECT_COLOR_BIT)
         framebuffer->color_attachment_count++;
   }

   compute_internal_bpp_from_attachments(framebuffer);
   v3dv_framebuffer_compute_tiling_params(framebuffer);

   *pFramebuffer = v3dv_framebuffer_to_handle(framebuffer);

   return VK_SUCCESS;
}

void
v3dv_DestroyFramebuffer(VkDevice _device,
                        VkFramebuffer _fb,
                        const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_framebuffer, fb, _fb);

   if (!fb)
      return;

   vk_free2(&device->alloc, pAllocator, fb);
}

VkResult
v3dv_GetMemoryFdPropertiesKHR(VkDevice _device,
                              VkExternalMemoryHandleTypeFlagBits handleType,
                              int fd,
                              VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_physical_device *pdevice = &device->instance->physicalDevice;

   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      pMemoryFdProperties->memoryTypeBits =
         (1 << pdevice->memory.memoryTypeCount) - 1;
      return VK_SUCCESS;
   default:
      return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
}

VkResult
v3dv_GetMemoryFdKHR(VkDevice _device,
                    const VkMemoryGetFdInfoKHR *pGetFdInfo,
                    int *pFd)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);
   assert(pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
          pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   int fd, ret;
   ret =
      drmPrimeHandleToFD(device->render_fd, mem->bo->handle, DRM_CLOEXEC, &fd);
   if (ret)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pFd = fd;

   return VK_SUCCESS;
}

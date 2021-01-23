/*
 * Copyright © 2021 Intel Corporation
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

#include "vk_instance.h"

#include "vk_alloc.h"

VkResult
vk_instance_init(struct vk_instance *instance,
                 const struct vk_instance_extension_table *supported_extensions,
                 const struct vk_instance_dispatch_table *dispatch_table,
                 const VkInstanceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *alloc)
{
   memset(instance, 0, sizeof(*instance));
   vk_object_base_init(NULL, &instance->base, VK_OBJECT_TYPE_INSTANCE);
   instance->alloc = *alloc;

   instance->app_info = (struct vk_app_info) { .api_version = 0 };
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

   if (supported_extensions != NULL) {
      for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
         int idx;
         for (idx = 0; idx < VK_INSTANCE_EXTENSION_COUNT; idx++) {
            if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                       vk_instance_extensions[idx].extensionName) == 0)
               break;
         }

         if (idx >= VK_INSTANCE_EXTENSION_COUNT)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

         if (!supported_extensions->extensions[idx])
            return VK_ERROR_EXTENSION_NOT_PRESENT;

         instance->enabled_extensions.extensions[idx] = true;
      }
   }

   if (dispatch_table != NULL)
      instance->dispatch_table = *dispatch_table;

   return VK_SUCCESS;
}

void
vk_instance_finish(struct vk_instance *instance)
{
   vk_free(&instance->alloc, (char *)instance->app_info.app_name);
   vk_free(&instance->alloc, (char *)instance->app_info.engine_name);
   vk_object_base_finish(&instance->base);
}

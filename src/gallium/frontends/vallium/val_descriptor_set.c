/*
 * Copyright © 2019 Red Hat.
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

#include "val_private.h"
#include "vk_util.h"
#include "u_math.h"

VkResult val_CreateDescriptorSetLayout(
    VkDevice                                    _device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorSetLayout*                      pSetLayout)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_descriptor_set_layout *set_layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
   uint32_t max_binding = 0;
   uint32_t immutable_sampler_count = 0;
   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      max_binding = MAX2(max_binding, pCreateInfo->pBindings[j].binding);
      if (pCreateInfo->pBindings[j].pImmutableSamplers)
         immutable_sampler_count += pCreateInfo->pBindings[j].descriptorCount;
   }

   size_t size = sizeof(struct val_descriptor_set_layout) +
                 (max_binding + 1) * sizeof(set_layout->binding[0]) +
                 immutable_sampler_count * sizeof(struct val_sampler *);

   set_layout = vk_zalloc2(&device->alloc, pAllocator, size, 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set_layout)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &set_layout->base,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT);
   /* We just allocate all the samplers at the end of the struct */
   struct val_sampler **samplers =
      (struct val_sampler **)&set_layout->binding[max_binding + 1];

   set_layout->binding_count = max_binding + 1;
   set_layout->shader_stages = 0;
   set_layout->size = 0;

   uint32_t dynamic_offset_count = 0;

   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      const VkDescriptorSetLayoutBinding *binding = &pCreateInfo->pBindings[j];
      uint32_t b = binding->binding;

      set_layout->binding[b].array_size = binding->descriptorCount;
      set_layout->binding[b].descriptor_index = set_layout->size;
      set_layout->binding[b].type = binding->descriptorType;
      set_layout->binding[b].valid = true;
      set_layout->size += binding->descriptorCount;

      for (gl_shader_stage stage = MESA_SHADER_VERTEX; stage < MESA_SHADER_STAGES; stage++) {
         set_layout->binding[b].stage[stage].const_buffer_index = -1;
         set_layout->binding[b].stage[stage].shader_buffer_index = -1;
         set_layout->binding[b].stage[stage].sampler_index = -1;
         set_layout->binding[b].stage[stage].sampler_view_index = -1;
         set_layout->binding[b].stage[stage].image_index = -1;
      }

      if (binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
          binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
         set_layout->binding[b].dynamic_index = dynamic_offset_count;
         dynamic_offset_count += binding->descriptorCount;
      }
      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         val_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].sampler_index = set_layout->stage[s].sampler_count;
            set_layout->stage[s].sampler_count += binding->descriptorCount;
         }
         break;
      default:
         break;
      }

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         val_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].const_buffer_index = set_layout->stage[s].const_buffer_count;
            set_layout->stage[s].const_buffer_count += binding->descriptorCount;
         }
        break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         val_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].shader_buffer_index = set_layout->stage[s].shader_buffer_count;
            set_layout->stage[s].shader_buffer_count += binding->descriptorCount;
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         val_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].image_index = set_layout->stage[s].image_count;
            set_layout->stage[s].image_count += binding->descriptorCount;
         }
         break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         val_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].sampler_view_index = set_layout->stage[s].sampler_view_count;
            set_layout->stage[s].sampler_view_count += binding->descriptorCount;
         }
         break;
      default:
         break;
      }

      if (binding->pImmutableSamplers) {
         set_layout->binding[b].immutable_samplers = samplers;
         samplers += binding->descriptorCount;

         for (uint32_t i = 0; i < binding->descriptorCount; i++)
            set_layout->binding[b].immutable_samplers[i] =
               val_sampler_from_handle(binding->pImmutableSamplers[i]);
      } else {
         set_layout->binding[b].immutable_samplers = NULL;
      }

      set_layout->shader_stages |= binding->stageFlags;
   }

   set_layout->dynamic_offset_count = dynamic_offset_count;

   *pSetLayout = val_descriptor_set_layout_to_handle(set_layout);

   return VK_SUCCESS;
}

void val_DestroyDescriptorSetLayout(
    VkDevice                                    _device,
    VkDescriptorSetLayout                       _set_layout,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_descriptor_set_layout, set_layout, _set_layout);

   if (!_set_layout)
     return;
   vk_object_base_finish(&set_layout->base);
   vk_free2(&device->alloc, pAllocator, set_layout);
}

VkResult val_CreatePipelineLayout(
    VkDevice                                    _device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipelineLayout*                           pPipelineLayout)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_pipeline_layout *layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

   layout = vk_alloc2(&device->alloc, pAllocator, sizeof(*layout), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (layout == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &layout->base,
                       VK_OBJECT_TYPE_PIPELINE_LAYOUT);
   layout->num_sets = pCreateInfo->setLayoutCount;

   for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
      VAL_FROM_HANDLE(val_descriptor_set_layout, set_layout,
                      pCreateInfo->pSetLayouts[set]);
      layout->set[set].layout = set_layout;
   }

   layout->push_constant_size = 0;
   for (unsigned i = 0; i < pCreateInfo->pushConstantRangeCount; ++i) {
      const VkPushConstantRange *range = pCreateInfo->pPushConstantRanges + i;
      layout->push_constant_size = MAX2(layout->push_constant_size,
                                        range->offset + range->size);
   }
   layout->push_constant_size = align(layout->push_constant_size, 16);
   *pPipelineLayout = val_pipeline_layout_to_handle(layout);

   return VK_SUCCESS;
}

void val_DestroyPipelineLayout(
    VkDevice                                    _device,
    VkPipelineLayout                            _pipelineLayout,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_pipeline_layout, pipeline_layout, _pipelineLayout);

   if (!_pipelineLayout)
     return;
   vk_object_base_finish(&pipeline_layout->base);
   vk_free2(&device->alloc, pAllocator, pipeline_layout);
}

VkResult
val_descriptor_set_create(struct val_device *device,
                          const struct val_descriptor_set_layout *layout,
                          struct val_descriptor_set **out_set)
{
   struct val_descriptor_set *set;
   size_t size = sizeof(*set) + layout->size * sizeof(set->descriptors[0]);

   set = vk_alloc(&device->alloc /* XXX: Use the pool */, size, 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* A descriptor set may not be 100% filled. Clear the set so we can can
    * later detect holes in it.
    */
   memset(set, 0, size);

   vk_object_base_init(&device->vk, &set->base,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET);
   set->layout = layout;

   /* Go through and fill out immutable samplers if we have any */
   struct val_descriptor *desc = set->descriptors;
   for (uint32_t b = 0; b < layout->binding_count; b++) {
      if (layout->binding[b].immutable_samplers) {
         for (uint32_t i = 0; i < layout->binding[b].array_size; i++)
            desc[i].sampler = layout->binding[b].immutable_samplers[i];
      }
      desc += layout->binding[b].array_size;
   }

   *out_set = set;

   return VK_SUCCESS;
}

void
val_descriptor_set_destroy(struct val_device *device,
                           struct val_descriptor_set *set)
{
   vk_object_base_finish(&set->base);
   vk_free(&device->alloc, set);
}

VkResult val_AllocateDescriptorSets(
    VkDevice                                    _device,
    const VkDescriptorSetAllocateInfo*          pAllocateInfo,
    VkDescriptorSet*                            pDescriptorSets)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_descriptor_pool, pool, pAllocateInfo->descriptorPool);
   VkResult result = VK_SUCCESS;
   struct val_descriptor_set *set;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      VAL_FROM_HANDLE(val_descriptor_set_layout, layout,
                      pAllocateInfo->pSetLayouts[i]);

      result = val_descriptor_set_create(device, layout, &set);
      if (result != VK_SUCCESS)
         break;

      list_addtail(&set->link, &pool->sets);
      pDescriptorSets[i] = val_descriptor_set_to_handle(set);
   }

   if (result != VK_SUCCESS)
      val_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool,
                             i, pDescriptorSets);

   return result;
}

VkResult val_FreeDescriptorSets(
    VkDevice                                    _device,
    VkDescriptorPool                            descriptorPool,
    uint32_t                                    count,
    const VkDescriptorSet*                      pDescriptorSets)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   for (uint32_t i = 0; i < count; i++) {
      VAL_FROM_HANDLE(val_descriptor_set, set, pDescriptorSets[i]);

      if (!set)
         continue;
      list_del(&set->link);
      val_descriptor_set_destroy(device, set);
   }
   return VK_SUCCESS;
}

void val_UpdateDescriptorSets(
    VkDevice                                    _device,
    uint32_t                                    descriptorWriteCount,
    const VkWriteDescriptorSet*                 pDescriptorWrites,
    uint32_t                                    descriptorCopyCount,
    const VkCopyDescriptorSet*                  pDescriptorCopies)
{
   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[i];
      VAL_FROM_HANDLE(val_descriptor_set, set, write->dstSet);
      const struct val_descriptor_set_binding_layout *bind_layout =
         &set->layout->binding[write->dstBinding];
      struct val_descriptor *desc =
         &set->descriptors[bind_layout->descriptor_index];
      desc += write->dstArrayElement;

      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            VAL_FROM_HANDLE(val_sampler, sampler,
                            write->pImageInfo[j].sampler);

            desc[j] = (struct val_descriptor) {
               .type = VK_DESCRIPTOR_TYPE_SAMPLER,
               .sampler = sampler,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            VAL_FROM_HANDLE(val_image_view, iview,
                            write->pImageInfo[j].imageView);
            VAL_FROM_HANDLE(val_sampler, sampler,
                            write->pImageInfo[j].sampler);

            desc[j].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            desc[j].image_view = iview;

            /* If this descriptor has an immutable sampler, we don't want
             * to stomp on it.
             */
            if (sampler)
               desc[j].sampler = sampler;
         }
         break;

      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            VAL_FROM_HANDLE(val_image_view, iview,
                            write->pImageInfo[j].imageView);

            desc[j] = (struct val_descriptor) {
               .type = write->descriptorType,
               .image_view = iview,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            VAL_FROM_HANDLE(val_buffer_view, bview,
                            write->pTexelBufferView[j]);

            desc[j] = (struct val_descriptor) {
               .type = write->descriptorType,
               .buffer_view = bview,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            assert(write->pBufferInfo[j].buffer);
            VAL_FROM_HANDLE(val_buffer, buffer, write->pBufferInfo[j].buffer);
            assert(buffer);
            desc[j] = (struct val_descriptor) {
               .type = write->descriptorType,
               .buf.offset = write->pBufferInfo[j].offset,
               .buf.buffer = buffer,
               .buf.range =  write->pBufferInfo[j].range,
            };

         }

      default:
         break;
      }
   }

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      const VkCopyDescriptorSet *copy = &pDescriptorCopies[i];
      VAL_FROM_HANDLE(val_descriptor_set, src, copy->srcSet);
      VAL_FROM_HANDLE(val_descriptor_set, dst, copy->dstSet);

      const struct val_descriptor_set_binding_layout *src_layout =
         &src->layout->binding[copy->srcBinding];
      struct val_descriptor *src_desc =
         &src->descriptors[src_layout->descriptor_index];
      src_desc += copy->srcArrayElement;

      const struct val_descriptor_set_binding_layout *dst_layout =
         &dst->layout->binding[copy->dstBinding];
      struct val_descriptor *dst_desc =
         &dst->descriptors[dst_layout->descriptor_index];
      dst_desc += copy->dstArrayElement;

      for (uint32_t j = 0; j < copy->descriptorCount; j++)
         dst_desc[j] = src_desc[j];
   }
}

VkResult val_CreateDescriptorPool(
    VkDevice                                    _device,
    const VkDescriptorPoolCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorPool*                           pDescriptorPool)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_descriptor_pool *pool;
   size_t size = sizeof(struct val_descriptor_pool);
   pool = vk_zalloc2(&device->alloc, pAllocator, size, 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pool->base,
                       VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   pool->flags = pCreateInfo->flags;
   list_inithead(&pool->sets);
   *pDescriptorPool = val_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

static void val_reset_descriptor_pool(struct val_device *device,
                                      struct val_descriptor_pool *pool)
{
   struct val_descriptor_set *set, *tmp;
   LIST_FOR_EACH_ENTRY_SAFE(set, tmp, &pool->sets, link) {
      list_del(&set->link);
      vk_free(&device->alloc, set);
   }
}

void val_DestroyDescriptorPool(
    VkDevice                                    _device,
    VkDescriptorPool                            _pool,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_descriptor_pool, pool, _pool);

   if (!_pool)
      return;

   val_reset_descriptor_pool(device, pool);
   vk_object_base_finish(&pool->base);
   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult val_ResetDescriptorPool(
    VkDevice                                    _device,
    VkDescriptorPool                            _pool,
    VkDescriptorPoolResetFlags                  flags)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_descriptor_pool, pool, _pool);

   val_reset_descriptor_pool(device, pool);
   return VK_SUCCESS;
}

void val_GetDescriptorSetLayoutSupport(VkDevice device,
                                       const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                       VkDescriptorSetLayoutSupport* pSupport)
{

}

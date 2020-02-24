/*
 * Copyright © 2019 Raspberry Pi
 *
 * Based in part on v3d driver which is:
 *
 * Copyright © 2014-2017 Broadcom
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

#include "v3dv_private.h"

static bool
descriptor_type_is_dynamic(VkDescriptorType type)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      return false;
      break;
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      return true;
      break;
   default:
      assert(!"descriptor type not supported.\n");
      break;
   }
   return false;
}

static struct v3dv_descriptor *
get_descriptor(struct v3dv_descriptor_state *descriptor_state,
               struct v3dv_descriptor_map *map,
               struct v3dv_pipeline_layout *pipeline_layout,
               uint32_t index,
               uint32_t *dynamic_offset)
{
   assert(index >= 0 && index < map->num_desc);

   uint32_t set_number = map->set[index];
   assert(descriptor_state->valid & 1 << set_number);

   struct v3dv_descriptor_set *set =
      descriptor_state->descriptor_sets[set_number];
   assert(set);

   uint32_t binding_number = map->binding[index];
   assert(binding_number < set->layout->binding_count);

   const struct v3dv_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding_number];

   uint32_t array_index = map->array_index[index];
   assert(array_index < binding_layout->array_size);

   if (descriptor_type_is_dynamic(binding_layout->type)) {
      uint32_t dynamic_offset_index =
         pipeline_layout->set[set_number].dynamic_offset_start +
         binding_layout->dynamic_offset_index + array_index;

      *dynamic_offset = descriptor_state->dynamic_offsets[dynamic_offset_index];
   }

   return &set->descriptors[binding_layout->descriptor_index + array_index];
}

/*
 * This method checks if the ubo used for push constants is needed to be
 * updated or not.
 *
 * push contants ubo is only used for push constants accessed by a non-const
 * index.
 *
 * FIXME: right now for this cases we are uploading the full
 * push_constants_data. An improvement would be to upload only the data that
 * we need to rely on a UBO.
 */
static void
check_push_constants_ubo(struct v3dv_cmd_buffer *cmd_buffer)
{
   if (!(cmd_buffer->state.dirty & V3DV_CMD_DIRTY_PUSH_CONSTANTS))
      return;

   if (cmd_buffer->push_constants_descriptor.bo == NULL) {
      cmd_buffer->push_constants_descriptor.bo =
         v3dv_bo_alloc(cmd_buffer->device, MAX_PUSH_CONSTANTS_SIZE, "push constants");

      if (!cmd_buffer->push_constants_descriptor.bo) {
         fprintf(stderr, "Failed to allocate memory for push constants\n");
         abort();
      }

      bool ok = v3dv_bo_map(cmd_buffer->device,
                            cmd_buffer->push_constants_descriptor.bo,
                            MAX_PUSH_CONSTANTS_SIZE);
      if (!ok) {
         fprintf(stderr, "failed to map push constants buffer\n");
         abort();
      }
   } else {
      if (cmd_buffer->push_constants_descriptor.offset + MAX_PUSH_CONSTANTS_SIZE <=
          cmd_buffer->push_constants_descriptor.bo->size) {
         cmd_buffer->push_constants_descriptor.offset += MAX_PUSH_CONSTANTS_SIZE;
      } else {
         /* FIXME: we got out of space for push descriptors. Should we create
          * a new bo? This could be easier with a uploader
          */
      }
   }

   memcpy(cmd_buffer->push_constants_descriptor.bo->map +
          cmd_buffer->push_constants_descriptor.offset,
          cmd_buffer->push_constants_data,
          MAX_PUSH_CONSTANTS_SIZE);

   cmd_buffer->state.dirty &= ~V3DV_CMD_DIRTY_PUSH_CONSTANTS;
}

struct v3dv_cl_reloc
v3dv_write_uniforms(struct v3dv_cmd_buffer *cmd_buffer,
                    struct v3dv_pipeline_stage *p_stage)
{
   struct v3d_uniform_list *uinfo = &p_stage->prog_data.base->uniforms;
   struct v3dv_dynamic_state *dynamic = &cmd_buffer->state.dynamic;
   struct v3dv_descriptor_state *descriptor_state =
      &cmd_buffer->state.descriptor_state;
   struct v3dv_pipeline *pipeline = p_stage->pipeline;

   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* The hardware always pre-fetches the next uniform (also when there
    * aren't any), so we always allocate space for an extra slot. This
    * fixes MMU exceptions reported since Linux kernel 5.4 when the
    * uniforms fill up the tail bytes of a page in the indirect
    * BO. In that scenario, when the hardware pre-fetches after reading
    * the last uniform it will read beyond the end of the page and trigger
    * the MMU exception.
    */
   v3dv_cl_ensure_space(&job->indirect, (uinfo->count + 1) * 4, 4);

   struct v3dv_cl_reloc uniform_stream = v3dv_cl_get_address(&job->indirect);

   struct v3dv_cl_out *uniforms = cl_start(&job->indirect);

   for (int i = 0; i < uinfo->count; i++) {
      uint32_t data = uinfo->data[i];

      switch (uinfo->contents[i]) {
      case QUNIFORM_CONSTANT:
         cl_aligned_u32(&uniforms, data);
         break;

      case QUNIFORM_UNIFORM:
         assert(pipeline->use_push_constants);
         cl_aligned_u32(&uniforms, cmd_buffer->push_constants_data[data]);
         break;

      case QUNIFORM_VIEWPORT_X_SCALE:
         cl_aligned_f(&uniforms, dynamic->viewport.scale[0][0] * 256.0f);
         break;

      case QUNIFORM_VIEWPORT_Y_SCALE:
         cl_aligned_f(&uniforms, dynamic->viewport.scale[0][1] * 256.0f);
         break;

      case QUNIFORM_VIEWPORT_Z_OFFSET:
         cl_aligned_f(&uniforms, dynamic->viewport.translate[0][2]);
         break;

      case QUNIFORM_VIEWPORT_Z_SCALE:
         cl_aligned_f(&uniforms, dynamic->viewport.scale[0][2]);
         break;

      case QUNIFORM_SSBO_OFFSET:
      case QUNIFORM_UBO_ADDR: {
         struct v3dv_descriptor_map *map =
            uinfo->contents[i] == QUNIFORM_UBO_ADDR ?
            &pipeline->ubo_map : &pipeline->ssbo_map;

         uint32_t offset =
            uinfo->contents[i] == QUNIFORM_UBO_ADDR ?
            v3d_unit_data_get_offset(data) :
            0; /* FIXME */

         uint32_t dynamic_offset = 0;

         /* For ubos, index is shifted, as 0 is reserved for push constants.
          */
         struct v3dv_descriptor *descriptor = NULL;
         if (uinfo->contents[i] == QUNIFORM_UBO_ADDR &&
             v3d_unit_data_get_unit(data) == 0) {
            /* This calls is to ensure that the push_constant_ubo is
             * updated. It already take into account it is should do the
             * update or not
             */
            check_push_constants_ubo(cmd_buffer);

            descriptor = &cmd_buffer->push_constants_descriptor;
         } else {
            uint32_t index =
               uinfo->contents[i] == QUNIFORM_UBO_ADDR ?
               v3d_unit_data_get_unit(data) - 1 :
               data;

            descriptor =
               get_descriptor(descriptor_state, map,
                              pipeline->layout,
                              index, &dynamic_offset);
            assert(descriptor);
         }

         cl_aligned_reloc(&job->indirect, &uniforms,
                          descriptor->bo,
                          descriptor->offset + offset + dynamic_offset);
         break;
      }

      default:
         unreachable("unsupported quniform_contents uniform type\n");
      }
   }

   cl_end(&job->indirect, uniforms);

   return uniform_stream;
}

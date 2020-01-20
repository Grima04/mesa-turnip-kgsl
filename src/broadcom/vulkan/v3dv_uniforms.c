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

static struct v3dv_descriptor *
get_descriptor(struct v3dv_descriptor_state *descriptor_state,
               struct v3dv_descriptor_map *map,
               uint32_t index)
{
   assert(index >= 0 && index < map->num );

   uint32_t set_number = map->set[index];
   assert(descriptor_state->valid & 1 << set_number);

   struct v3dv_descriptor_set *set =
      descriptor_state->descriptors[set_number];
   assert(set);

   uint32_t binding_number = map->binding[index];
   assert(binding_number < set->layout->binding_count);

   const struct v3dv_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding_number];

   return &set->descriptors[binding_layout->descriptor_index];
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

         /* UBO index is shift up by 1, to follow gallium (0 is gallium's
          * constant buffer 0), that is what nir_to_vir expects. But for the
          * ubo_map below, we start from 0.
          */
         uint32_t index =
            uinfo->contents[i] == QUNIFORM_UBO_ADDR ?
            v3d_unit_data_get_unit(data) - 1 :
            data;

         struct v3dv_descriptor *descriptor =
            get_descriptor(descriptor_state, map, index);
         assert(descriptor);

         cl_aligned_reloc(&job->indirect, &uniforms,
                          descriptor->bo,
                          descriptor->offset + offset);
         break;
      }

      default:
         unreachable("unsupported quniform_contents uniform type\n");
      }
   }

   cl_end(&job->indirect, uniforms);

   return uniform_stream;
}

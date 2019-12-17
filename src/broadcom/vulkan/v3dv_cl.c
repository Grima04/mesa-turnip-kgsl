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

#include "v3dv_private.h"
#include "broadcom/cle/v3dx_pack.h"

void
v3dv_cl_init(struct v3dv_cmd_buffer *cmd_buffer, struct v3dv_cl *cl)
{
   cl->base = NULL;
   cl->next = cl->base;
   cl->bo = NULL;
   cl->size = 0;
   cl->cmd_buffer = cmd_buffer;
}

void
v3dv_cl_begin(struct v3dv_cl *cl)
{
   assert(!cl->cmd_buffer ||
          cl->cmd_buffer->status == V3DV_CMD_BUFFER_STATUS_INITIALIZED);
   assert(v3dv_cl_offset(cl) == 0);
}

void
v3dv_cl_reset(struct v3dv_cl *cl)
{
   /* FIXME: consider keeping the BO when the command buffer is reset with
    * flag VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT.
    */
   v3dv_cl_init(cl->cmd_buffer, cl);
}

void
v3dv_cl_destroy(struct v3dv_cl *cl)
{
   if (cl->bo) {
      assert(cl->cmd_buffer);
      v3dv_bo_free(cl->cmd_buffer->device, cl->bo);
   }

   /* Leave the CL in a reset state to catch use after destroy instances */
   v3dv_cl_init(NULL, cl);
}

void
v3dv_cl_ensure_space_with_branch(struct v3dv_cl *cl, uint32_t space)
{
   if (v3dv_cl_offset(cl) + space + cl_packet_length(BRANCH) <= cl->size)
      return;

   struct v3dv_bo *bo = v3dv_bo_alloc(cl->cmd_buffer->device, space);
   if (!bo) {
      fprintf(stderr, "failed to allocate memory for command list");
      abort();
   }

   /* Chain to the new BO from the old one if needed */
   if (cl->bo) {
      cl_emit(cl, BRANCH, branch) {
         branch.address = v3dv_cl_address(bo, 0);
      }
   }

   v3dv_cmd_buffer_add_bo(cl->cmd_buffer, bo);

   bool ok = v3dv_bo_map(cl->cmd_buffer->device, bo, bo->size);
   if (!ok) {
      fprintf(stderr, "failed to map command list buffer");
      abort();
   }

   cl->bo = bo;
   cl->base = cl->bo->map;
   cl->size = cl->bo->size;
   cl->next = cl->base;
}

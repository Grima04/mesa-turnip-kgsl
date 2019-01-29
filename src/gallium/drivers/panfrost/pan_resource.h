/*
 * © Copyright2018-2019 Alyssa Rosenzweig
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */


#ifndef PAN_RESOURCE_H
#define PAN_RESOURCE_H

#include <panfrost-job.h>
#include "pan_screen.h"
#include "pan_allocate.h"
#include <drm.h>

struct panfrost_bo {
        /* Address to the BO in question */

        uint8_t *cpu[MAX_MIP_LEVELS];

        /* Not necessarily a GPU mapping of cpu! In case of texture tiling, gpu
         * points to the GPU-side, tiled texture, while cpu points to the
         * CPU-side, untiled texture from mesa */

        mali_ptr gpu[MAX_MIP_LEVELS];

        /* Memory entry corresponding to gpu above */
        struct panfrost_memory_entry *entry[MAX_MIP_LEVELS];

        /* Set for tiled, clear for linear. */
        bool tiled;

        /* Is something other than level 0 ever written? */
        bool is_mipmap;

        /* If AFBC is enabled for this resource, we lug around an AFBC
         * metadata buffer as well. The actual AFBC resource is also in
         * afbc_slab (only defined for AFBC) at position afbc_main_offset */

        bool has_afbc;
        struct panfrost_memory afbc_slab;
        int afbc_metadata_size;

        /* Similarly for TE */
        bool has_checksum;
        struct panfrost_memory checksum_slab;
        int checksum_stride;
};

struct panfrost_resource {
        struct pipe_resource base;

        struct panfrost_bo *bo;
        struct renderonly_scanout *scanout;
};

static inline struct panfrost_resource *
pan_resource(struct pipe_resource *p)
{
   return (struct panfrost_resource *)p;
}

void panfrost_resource_screen_init(struct panfrost_screen *screen);

void panfrost_resource_context_init(struct pipe_context *pctx);

#endif /* PAN_RESOURCE_H */

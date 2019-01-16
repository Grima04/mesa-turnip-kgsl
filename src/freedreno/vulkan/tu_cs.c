/*
 * Copyright © 2019 Google LLC
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_cs.h"

/**
 * Initialize a command stream.
 */
void
tu_cs_init(struct tu_cs *cs)
{
   cs->start = cs->cur = cs->end = NULL;

   cs->entry_count = cs->entry_capacity = 0;
   cs->entries = NULL;

   cs->bo_count = cs->bo_capacity = 0;
   cs->bos = NULL;
}

/**
 * Finish and release all resources owned by a command stream.
 */
void
tu_cs_finish(struct tu_device *dev, struct tu_cs *cs)
{
   for (uint32_t i = 0; i < cs->bo_count; ++i) {
      tu_bo_finish(dev, cs->bos[i]);
      free(cs->bos[i]);
   }

   free(cs->entries);
   free(cs->bos);
}

/**
 * Begin (or continue) command packet emission.  This will reserve space from
 * the command stream for at least \a reserve_size uint32_t values.
 */
VkResult
tu_cs_begin(struct tu_device *dev, struct tu_cs *cs, uint32_t reserve_size)
{
   assert(reserve_size);

   if (cs->end - cs->cur < reserve_size) {
      if (cs->bo_count == cs->bo_capacity) {
         uint32_t new_capacity = MAX2(4, 2 * cs->bo_capacity);
         struct tu_bo **new_bos =
            realloc(cs->bos, new_capacity * sizeof(struct tu_bo *));
         if (!new_bos)
            abort();

         cs->bo_capacity = new_capacity;
         cs->bos = new_bos;
      }

      uint32_t new_size = MAX2(16384, reserve_size * sizeof(uint32_t));
      if (cs->bo_count)
         new_size = MAX2(new_size, cs->bos[cs->bo_count - 1]->size * 2);

      struct tu_bo *new_bo = malloc(sizeof(struct tu_bo));
      if (!new_bo)
         abort();

      VkResult result = tu_bo_init_new(dev, new_bo, new_size);
      if (result != VK_SUCCESS) {
         free(new_bo);
         return result;
      }

      result = tu_bo_map(dev, new_bo);
      if (result != VK_SUCCESS) {
         tu_bo_finish(dev, new_bo);
         free(new_bo);
         return result;
      }

      cs->bos[cs->bo_count] = new_bo;
      ++cs->bo_count;

      cs->start = cs->cur = (uint32_t *) new_bo->map;
      cs->end = cs->start + new_bo->size / sizeof(uint32_t);
   }
   cs->start = cs->cur;

   return VK_SUCCESS;
}

/**
 * End command packet emission by adding an IB entry for the command packets
 * emitted since the last call to tu_cs_begin.
 */
VkResult
tu_cs_end(struct tu_cs *cs)
{
   if (cs->start == cs->cur)
      return VK_SUCCESS;

   if (cs->entry_capacity == cs->entry_count) {
      uint32_t new_capacity = MAX2(cs->entry_capacity * 2, 4);
      struct tu_cs_entry *new_entries =
         realloc(cs->entries, new_capacity * sizeof(struct tu_cs_entry));
      if (!new_entries)
         abort(); /* TODO */

      cs->entries = new_entries;
      cs->entry_capacity = new_capacity;
   }

   assert(cs->bo_count);

   struct tu_cs_entry entry;
   entry.bo = cs->bos[cs->bo_count - 1];
   entry.size = (cs->cur - cs->start) * sizeof(uint32_t);
   entry.offset = (cs->start - (uint32_t *) entry.bo->map) * sizeof(uint32_t);

   cs->entries[cs->entry_count] = entry;
   ++cs->entry_count;

   return VK_SUCCESS;
}

/**
 * Reset a command stream to its initial state.  This discards all comand
 * packets in \a cs, but does not necessarily release all resources.
 */
void
tu_cs_reset(struct tu_device *dev, struct tu_cs *cs)
{
   for (uint32_t i = 0; i + 1 < cs->bo_count; ++i) {
      tu_bo_finish(dev, cs->bos[i]);
      free(cs->bos[i]);
   }

   if (cs->bo_count) {
      cs->bos[0] = cs->bos[cs->bo_count - 1];
      cs->bo_count = 1;

      cs->start = cs->cur = (uint32_t *) cs->bos[0]->map;
      cs->end = cs->start + cs->bos[0]->size / sizeof(uint32_t);
   }

   cs->entry_count = 0;
}

/**
 * Reserve space from a command stream for \a size uint32_t values.
 */
VkResult
tu_cs_check_space(struct tu_device *dev, struct tu_cs *cs, size_t size)
{
   if (cs->end - cs->cur >= size)
      return VK_SUCCESS;

   VkResult result = tu_cs_end(cs);
   if (result != VK_SUCCESS)
      return result;

   return tu_cs_begin(dev, cs, size);
}

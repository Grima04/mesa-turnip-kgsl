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
 * Get the offset of the command packets emitted since the last call to
 * tu_cs_end.
 */
static uint32_t
tu_cs_get_offset(const struct tu_cs *cs)
{
   assert(cs->bo_count);
   return cs->start - (uint32_t *) cs->bos[cs->bo_count - 1]->map;
}

/**
 * Get the size of the command packets emitted since the last call to
 * tu_cs_end.
 */
static uint32_t
tu_cs_get_size(const struct tu_cs *cs)
{
   return cs->cur - cs->start;
}

/**
 * Get the size of the remaining space in the current BO.
 */
static uint32_t
tu_cs_get_space(const struct tu_cs *cs)
{
   return cs->end - cs->cur;
}

/**
 * Return true if there is no command packet emitted since the last call to
 * tu_cs_end.
 */
static uint32_t
tu_cs_is_empty(const struct tu_cs *cs)
{
   return tu_cs_get_size(cs) == 0;
}

/*
 * Allocate and add a BO to a command stream.  Following command packets will
 * be emitted to the new BO.
 */
static VkResult
tu_cs_add_bo(struct tu_device *dev, struct tu_cs *cs, uint32_t byte_size)
{
   /* grow cs->bos if needed */
   if (cs->bo_count == cs->bo_capacity) {
      uint32_t new_capacity = MAX2(4, 2 * cs->bo_capacity);
      struct tu_bo **new_bos =
         realloc(cs->bos, new_capacity * sizeof(struct tu_bo *));
      if (!new_bos)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      cs->bo_capacity = new_capacity;
      cs->bos = new_bos;
   }

   struct tu_bo *new_bo = malloc(sizeof(struct tu_bo));
   if (!new_bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = tu_bo_init_new(dev, new_bo, byte_size);
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

   cs->bos[cs->bo_count++] = new_bo;

   cs->start = cs->cur = (uint32_t *) new_bo->map;
   cs->end = cs->start + new_bo->size / sizeof(uint32_t);

   return VK_SUCCESS;
}

/**
 * Begin (or continue) command packet emission.  This will reserve space from
 * the command stream for at least \a reserve_size uint32_t values.
 */
VkResult
tu_cs_begin(struct tu_device *dev, struct tu_cs *cs, uint32_t reserve_size)
{
   /* no dangling command packet */
   assert(tu_cs_is_empty(cs));

   if (tu_cs_get_space(cs) < reserve_size) {
      uint32_t new_size = MAX2(16384, reserve_size * sizeof(uint32_t));
      if (cs->bo_count)
         new_size = MAX2(new_size, cs->bos[cs->bo_count - 1]->size * 2);

      VkResult result = tu_cs_add_bo(dev, cs, new_size);
      if (result != VK_SUCCESS)
         return result;
   }

   assert(tu_cs_get_space(cs) >= reserve_size);

   return VK_SUCCESS;
}

/**
 * End command packet emission by adding an IB entry for the command packets
 * emitted since the last call to tu_cs_begin.
 */
VkResult
tu_cs_end(struct tu_cs *cs)
{
   /* no command packet at all */
   if (tu_cs_is_empty(cs))
      return VK_SUCCESS;

   /* grow cs->entries if needed */
   if (cs->entry_capacity == cs->entry_count) {
      uint32_t new_capacity = MAX2(cs->entry_capacity * 2, 4);
      struct tu_cs_entry *new_entries =
         realloc(cs->entries, new_capacity * sizeof(struct tu_cs_entry));
      if (!new_entries)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      cs->entries = new_entries;
      cs->entry_capacity = new_capacity;
   }

   assert(cs->bo_count);

   /* add an entry for [cs->start, cs->cur] */
   cs->entries[cs->entry_count++] = (struct tu_cs_entry) {
      .bo = cs->bos[cs->bo_count - 1],
      .size = tu_cs_get_size(cs) * sizeof(uint32_t),
      .offset = tu_cs_get_offset(cs) * sizeof(uint32_t),
   };

   cs->start = cs->cur;

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

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
tu_cs_init(struct tu_cs *cs, uint32_t initial_size)
{
   memset(cs, 0, sizeof(*cs));

   cs->next_bo_size = initial_size;
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
 * tu_cs_add_entry.
 */
static uint32_t
tu_cs_get_offset(const struct tu_cs *cs)
{
   assert(cs->bo_count);
   return cs->start - (uint32_t *) cs->bos[cs->bo_count - 1]->map;
}

/**
 * Get the size of the command packets emitted since the last call to
 * tu_cs_add_entry.
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
 * tu_cs_add_entry.
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
tu_cs_add_bo(struct tu_device *dev, struct tu_cs *cs, uint32_t size)
{
   /* no dangling command packet */
   assert(tu_cs_is_empty(cs));

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

   VkResult result = tu_bo_init_new(dev, new_bo, size * sizeof(uint32_t));
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
 * Reserve an IB entry.
 */
static VkResult
tu_cs_reserve_entry(struct tu_device *dev, struct tu_cs *cs)
{
   /* grow cs->entries if needed */
   if (cs->entry_count == cs->entry_capacity) {
      uint32_t new_capacity = MAX2(4, cs->entry_capacity * 2);
      struct tu_cs_entry *new_entries =
         realloc(cs->entries, new_capacity * sizeof(struct tu_cs_entry));
      if (!new_entries)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      cs->entry_capacity = new_capacity;
      cs->entries = new_entries;
   }

   return VK_SUCCESS;
}

/**
 * Add an IB entry for the command packets emitted since the last call to this
 * function.
 */
static void
tu_cs_add_entry(struct tu_cs *cs)
{
   /* disallow empty entry */
   assert(!tu_cs_is_empty(cs));

   /*
    * because we disallow empty entry, tu_cs_add_bo and tu_cs_reserve_entry
    * must both have been called
    */
   assert(cs->bo_count);
   assert(cs->entry_count < cs->entry_capacity);

   /* add an entry for [cs->start, cs->cur] */
   cs->entries[cs->entry_count++] = (struct tu_cs_entry) {
      .bo = cs->bos[cs->bo_count - 1],
      .size = tu_cs_get_size(cs) * sizeof(uint32_t),
      .offset = tu_cs_get_offset(cs) * sizeof(uint32_t),
   };

   cs->start = cs->cur;
}

/**
 * Begin (or continue) command packet emission.  This does nothing but sanity
 * checks currently.
 */
void
tu_cs_begin(struct tu_cs *cs)
{
   assert(tu_cs_is_empty(cs));
}

/**
 * End command packet emission and add an IB entry.
 */
void
tu_cs_end(struct tu_cs *cs)
{
   if (!tu_cs_is_empty(cs))
      tu_cs_add_entry(cs);
}

/**
 * Reserve space from a command stream for \a reserved_size uint32_t values.
 */
VkResult
tu_cs_reserve_space(struct tu_device *dev,
                    struct tu_cs *cs,
                    uint32_t reserved_size)
{
   if (tu_cs_get_space(cs) < reserved_size) {
      /* add an entry for the exiting command packets */
      if (!tu_cs_is_empty(cs))
         tu_cs_add_entry(cs);

      /* switch to a new BO */
      uint32_t new_size = MAX2(cs->next_bo_size, reserved_size);
      VkResult result = tu_cs_add_bo(dev, cs, new_size);
      if (result != VK_SUCCESS)
         return result;
      cs->next_bo_size = new_size * 2;
   }

   assert(tu_cs_get_space(cs) >= reserved_size);
   cs->reserved_end = cs->cur + reserved_size;

   /* reserve an entry for the next call to this function or tu_cs_end */
   return tu_cs_reserve_entry(dev, cs);
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

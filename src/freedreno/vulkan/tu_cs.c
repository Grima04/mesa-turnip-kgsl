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

void
tu_cmd_stream_init(struct tu_cmd_stream *stream)
{
   stream->start = stream->cur = stream->end = NULL;

   stream->entry_count = stream->entry_capacity = 0;
   stream->entries = NULL;

   stream->bo_count = stream->bo_capacity = 0;
   stream->bos = NULL;
}

void
tu_cmd_stream_finish(struct tu_device *dev, struct tu_cmd_stream *stream)
{
   for (uint32_t i = 0; i < stream->bo_count; ++i) {
      tu_bo_finish(dev, stream->bos[i]);
      free(stream->bos[i]);
   }

   free(stream->entries);
   free(stream->bos);
}

VkResult
tu_cmd_stream_begin(struct tu_device *dev,
                    struct tu_cmd_stream *stream,
                    uint32_t reserve_size)
{
   assert(reserve_size);

   if (stream->end - stream->cur < reserve_size) {
      if (stream->bo_count == stream->bo_capacity) {
         uint32_t new_capacity = MAX2(4, 2 * stream->bo_capacity);
         struct tu_bo **new_bos =
            realloc(stream->bos, new_capacity * sizeof(struct tu_bo *));
         if (!new_bos)
            abort();

         stream->bo_capacity = new_capacity;
         stream->bos = new_bos;
      }

      uint32_t new_size = MAX2(16384, reserve_size * sizeof(uint32_t));
      if (stream->bo_count)
         new_size =
            MAX2(new_size, stream->bos[stream->bo_count - 1]->size * 2);

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

      stream->bos[stream->bo_count] = new_bo;
      ++stream->bo_count;

      stream->start = stream->cur = (uint32_t *) new_bo->map;
      stream->end = stream->start + new_bo->size / sizeof(uint32_t);
   }
   stream->start = stream->cur;

   return VK_SUCCESS;
}

VkResult
tu_cmd_stream_end(struct tu_cmd_stream *stream)
{
   if (stream->start == stream->cur)
      return VK_SUCCESS;

   if (stream->entry_capacity == stream->entry_count) {
      uint32_t new_capacity = MAX2(stream->entry_capacity * 2, 4);
      struct tu_cmd_stream_entry *new_entries = realloc(
         stream->entries, new_capacity * sizeof(struct tu_cmd_stream_entry));
      if (!new_entries)
         abort(); /* TODO */

      stream->entries = new_entries;
      stream->entry_capacity = new_capacity;
   }

   assert(stream->bo_count);

   struct tu_cmd_stream_entry entry;
   entry.bo = stream->bos[stream->bo_count - 1];
   entry.size = (stream->cur - stream->start) * sizeof(uint32_t);
   entry.offset =
      (stream->start - (uint32_t *) entry.bo->map) * sizeof(uint32_t);

   stream->entries[stream->entry_count] = entry;
   ++stream->entry_count;

   return VK_SUCCESS;
}

void
tu_cmd_stream_reset(struct tu_device *dev, struct tu_cmd_stream *stream)
{
   for (uint32_t i = 0; i + 1 < stream->bo_count; ++i) {
      tu_bo_finish(dev, stream->bos[i]);
      free(stream->bos[i]);
   }

   if (stream->bo_count) {
      stream->bos[0] = stream->bos[stream->bo_count - 1];
      stream->bo_count = 1;

      stream->start = stream->cur = (uint32_t *) stream->bos[0]->map;
      stream->end = stream->start + stream->bos[0]->size / sizeof(uint32_t);
   }

   stream->entry_count = 0;
}

VkResult
tu_cs_check_space(struct tu_device *dev,
                  struct tu_cmd_stream *stream,
                  size_t size)
{
   if (stream->end - stream->cur >= size)
      return VK_SUCCESS;

   VkResult result = tu_cmd_stream_end(stream);
   if (result != VK_SUCCESS)
      return result;

   return tu_cmd_stream_begin(dev, stream, size);
}

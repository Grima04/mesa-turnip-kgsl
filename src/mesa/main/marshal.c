/*
 * Copyright © 2012 Intel Corporation
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

/** \file marshal.c
 *
 * Custom functions for marshalling GL calls from the main thread to a worker
 * thread when automatic code generation isn't appropriate.
 */

#include "main/enums.h"
#include "main/macros.h"
#include "marshal.h"
#include "dispatch.h"

static inline void
_mesa_post_marshal_hook(struct gl_context *ctx)
{
   /* This can be enabled for debugging whether a failure is a synchronization
    * problem between the main thread and the worker thread, or a failure in
    * how we actually marshal.
    */
   if (false)
      _mesa_glthread_finish(ctx);
}


struct marshal_cmd_ShaderSource
{
   struct marshal_cmd_base cmd_base;
   GLuint shader;
   GLsizei count;
   /* Followed by GLint length[count], then the contents of all strings,
    * concatenated.
    */
};


void
_mesa_unmarshal_ShaderSource(struct gl_context *ctx,
                             const struct marshal_cmd_ShaderSource *cmd)
{
   const GLint *cmd_length = (const GLint *) (cmd + 1);
   const GLchar *cmd_strings = (const GLchar *) (cmd_length + cmd->count);
   /* TODO: how to deal with malloc failure? */
   const GLchar * *string = malloc(cmd->count * sizeof(const GLchar *));
   int i;

   for (i = 0; i < cmd->count; ++i) {
      string[i] = cmd_strings;
      cmd_strings += cmd_length[i];
   }
   CALL_ShaderSource(ctx->CurrentServerDispatch,
                     (cmd->shader, cmd->count, string, cmd_length));
   free((void *)string);
}


static size_t
measure_ShaderSource_strings(GLsizei count, const GLchar * const *string,
                             const GLint *length_in, GLint *length_out)
{
   int i;
   size_t total_string_length = 0;

   for (i = 0; i < count; ++i) {
      if (length_in == NULL || length_in[i] < 0) {
         if (string[i])
            length_out[i] = strlen(string[i]);
      } else {
         length_out[i] = length_in[i];
      }
      total_string_length += length_out[i];
   }
   return total_string_length;
}


void GLAPIENTRY
_mesa_marshal_ShaderSource(GLuint shader, GLsizei count,
                           const GLchar * const *string, const GLint *length)
{
   /* TODO: how to report an error if count < 0? */

   GET_CURRENT_CONTEXT(ctx);
   /* TODO: how to deal with malloc failure? */
   const size_t fixed_cmd_size = sizeof(struct marshal_cmd_ShaderSource);
   STATIC_ASSERT(sizeof(struct marshal_cmd_ShaderSource) % sizeof(GLint) == 0);
   size_t length_size = count * sizeof(GLint);
   GLint *length_tmp = malloc(length_size);
   size_t total_string_length =
      measure_ShaderSource_strings(count, string, length, length_tmp);
   size_t total_cmd_size = fixed_cmd_size + length_size + total_string_length;

   if (total_cmd_size <= MARSHAL_MAX_CMD_SIZE) {
      struct marshal_cmd_ShaderSource *cmd =
         _mesa_glthread_allocate_command(ctx, DISPATCH_CMD_ShaderSource,
                                         total_cmd_size);
      GLint *cmd_length = (GLint *) (cmd + 1);
      GLchar *cmd_strings = (GLchar *) (cmd_length + count);
      int i;

      cmd->shader = shader;
      cmd->count = count;
      memcpy(cmd_length, length_tmp, length_size);
      for (i = 0; i < count; ++i) {
         memcpy(cmd_strings, string[i], cmd_length[i]);
         cmd_strings += cmd_length[i];
      }
      _mesa_post_marshal_hook(ctx);
   } else {
      _mesa_glthread_finish(ctx);
      CALL_ShaderSource(ctx->CurrentServerDispatch,
                        (shader, count, string, length_tmp));
   }
   free(length_tmp);
}


/** Tracks the current bindings for the vertex array and index array buffers.
 *
 * This is part of what we need to enable glthread on compat-GL contexts that
 * happen to use VBOs, without also supporting the full tracking of VBO vs
 * user vertex array bindings per attribute on each vertex array for
 * determining what to upload at draw call time.
 *
 * Note that GL core makes it so that a buffer binding with an invalid handle
 * in the "buffer" parameter will throw an error, and then a
 * glVertexAttribPointer() that followsmight not end up pointing at a VBO.
 * However, in GL core the draw call would throw an error as well, so we don't
 * really care if our tracking is wrong for this case -- we never need to
 * marshal user data for draw calls, and the unmarshal will just generate an
 * error or not as appropriate.
 *
 * For compatibility GL, we do need to accurately know whether the draw call
 * on the unmarshal side will dereference a user pointer or load data from a
 * VBO per vertex.  That would make it seem like we need to track whether a
 * "buffer" is valid, so that we can know when an error will be generated
 * instead of updating the binding.  However, compat GL has the ridiculous
 * feature that if you pass a bad name, it just gens a buffer object for you,
 * so we escape without having to know if things are valid or not.
 */
void
_mesa_glthread_BindBuffer(struct gl_context *ctx, GLenum target, GLuint buffer)
{
   struct glthread_state *glthread = ctx->GLThread;

   switch (target) {
   case GL_ARRAY_BUFFER:
      glthread->vertex_array_is_vbo = (buffer != 0);
      break;
   case GL_ELEMENT_ARRAY_BUFFER:
      /* The current element array buffer binding is actually tracked in the
       * vertex array object instead of the context, so this would need to
       * change on vertex array object updates.
       */
      glthread->CurrentVAO->IndexBufferIsUserPointer = buffer != 0;
      break;
   case GL_DRAW_INDIRECT_BUFFER:
      glthread->draw_indirect_buffer_is_vbo = buffer != 0;
      break;
   }
}


/* BufferData: marshalled asynchronously */
struct marshal_cmd_BufferData
{
   struct marshal_cmd_base cmd_base;
   GLuint target_or_name;
   GLsizeiptr size;
   GLenum usage;
   const GLvoid *data_external_mem;
   bool data_null; /* If set, no data follows for "data" */
   bool named;
   /* Next size bytes are GLubyte data[size] */
};

void
_mesa_unmarshal_BufferData(struct gl_context *ctx,
                           const struct marshal_cmd_BufferData *cmd)
{
   const GLuint target_or_name = cmd->target_or_name;
   const GLsizei size = cmd->size;
   const GLenum usage = cmd->usage;
   const void *data;

   if (cmd->data_null)
      data = NULL;
   else if (!cmd->named && target_or_name == GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD)
      data = cmd->data_external_mem;
   else
      data = (const void *) (cmd + 1);

   if (cmd->named) {
      CALL_NamedBufferData(ctx->CurrentServerDispatch,
                           (target_or_name, size, data, usage));
   } else {
      CALL_BufferData(ctx->CurrentServerDispatch,
                      (target_or_name, size, data, usage));
   }
}

void
_mesa_unmarshal_NamedBufferData(struct gl_context *ctx,
                                const struct marshal_cmd_BufferData *cmd)
{
   unreachable("never used - all BufferData variants use DISPATCH_CMD_BufferData");
}

static void
_mesa_marshal_BufferData_merged(GLuint target_or_name, GLsizeiptr size,
                                const GLvoid *data, GLenum usage, bool named,
                                const char *func)
{
   GET_CURRENT_CONTEXT(ctx);
   bool external_mem = !named &&
                       target_or_name == GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD;
   bool copy_data = data && !external_mem;
   int cmd_size = sizeof(struct marshal_cmd_BufferData) + (copy_data ? size : 0);
   debug_print_marshal("BufferData");

   if (unlikely(size < 0 || size > INT_MAX || cmd_size < 0 ||
                cmd_size > MARSHAL_MAX_CMD_SIZE ||
                (named && target_or_name == 0))) {
      _mesa_glthread_finish_before(ctx, func);
      if (named) {
         CALL_NamedBufferData(ctx->CurrentServerDispatch,
                              (target_or_name, size, data, usage));
      } else {
         CALL_BufferData(ctx->CurrentServerDispatch,
                         (target_or_name, size, data, usage));
      }
      return;
   }

   struct marshal_cmd_BufferData *cmd =
      _mesa_glthread_allocate_command(ctx, DISPATCH_CMD_BufferData,
                                      cmd_size);

   cmd->target_or_name = target_or_name;
   cmd->size = size;
   cmd->usage = usage;
   cmd->data_null = !data;
   cmd->named = named;
   cmd->data_external_mem = data;

   if (copy_data) {
      char *variable_data = (char *) (cmd + 1);
      memcpy(variable_data, data, size);
   }
   _mesa_post_marshal_hook(ctx);
}

void GLAPIENTRY
_mesa_marshal_BufferData(GLenum target, GLsizeiptr size, const GLvoid * data,
                         GLenum usage)
{
   _mesa_marshal_BufferData_merged(target, size, data, usage, false,
                                   "BufferData");
}

void GLAPIENTRY
_mesa_marshal_NamedBufferData(GLuint buffer, GLsizeiptr size,
                              const GLvoid * data, GLenum usage)
{
   _mesa_marshal_BufferData_merged(buffer, size, data, usage, true,
                                   "NamedBufferData");
}


/* BufferSubData: marshalled asynchronously */
struct marshal_cmd_BufferSubData
{
   struct marshal_cmd_base cmd_base;
   GLenum target;
   GLintptr offset;
   GLsizeiptr size;
   /* Next size bytes are GLubyte data[size] */
};

void
_mesa_unmarshal_BufferSubData(struct gl_context *ctx,
                              const struct marshal_cmd_BufferSubData *cmd)
{
   const GLenum target = cmd->target;
   const GLintptr offset = cmd->offset;
   const GLsizeiptr size = cmd->size;
   const void *data = (const void *) (cmd + 1);

   CALL_BufferSubData(ctx->CurrentServerDispatch,
                      (target, offset, size, data));
}

void GLAPIENTRY
_mesa_marshal_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                            const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   size_t cmd_size = sizeof(struct marshal_cmd_BufferSubData) + size;

   debug_print_marshal("BufferSubData");
   if (unlikely(size < 0)) {
      _mesa_glthread_finish(ctx);
      _mesa_error(ctx, GL_INVALID_VALUE, "BufferSubData(size < 0)");
      return;
   }

   if (target != GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD &&
       cmd_size <= MARSHAL_MAX_CMD_SIZE) {
      struct marshal_cmd_BufferSubData *cmd =
         _mesa_glthread_allocate_command(ctx, DISPATCH_CMD_BufferSubData,
                                         cmd_size);
      cmd->target = target;
      cmd->offset = offset;
      cmd->size = size;
      char *variable_data = (char *) (cmd + 1);
      memcpy(variable_data, data, size);
      _mesa_post_marshal_hook(ctx);
   } else {
      _mesa_glthread_finish(ctx);
      CALL_BufferSubData(ctx->CurrentServerDispatch,
                         (target, offset, size, data));
   }
}


/* NamedBufferSubData: marshalled asynchronously */
struct marshal_cmd_NamedBufferSubData
{
   struct marshal_cmd_base cmd_base;
   GLuint name;
   GLintptr offset;
   GLsizei size;
   /* Next size bytes are GLubyte data[size] */
};

void
_mesa_unmarshal_NamedBufferSubData(struct gl_context *ctx,
                                   const struct marshal_cmd_NamedBufferSubData *cmd)
{
   const GLuint name = cmd->name;
   const GLintptr offset = cmd->offset;
   const GLsizei size = cmd->size;
   const void *data = (const void *) (cmd + 1);

   CALL_NamedBufferSubData(ctx->CurrentServerDispatch,
                           (name, offset, size, data));
}

void GLAPIENTRY
_mesa_marshal_NamedBufferSubData(GLuint buffer, GLintptr offset,
                                 GLsizeiptr size, const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   size_t cmd_size = sizeof(struct marshal_cmd_NamedBufferSubData) + size;

   debug_print_marshal("NamedBufferSubData");
   if (unlikely(size < 0)) {
      _mesa_glthread_finish(ctx);
      _mesa_error(ctx, GL_INVALID_VALUE, "NamedBufferSubData(size < 0)");
      return;
   }

   if (buffer > 0 && cmd_size <= MARSHAL_MAX_CMD_SIZE) {
      struct marshal_cmd_NamedBufferSubData *cmd =
         _mesa_glthread_allocate_command(ctx, DISPATCH_CMD_NamedBufferSubData,
                                         cmd_size);
      cmd->name = buffer;
      cmd->offset = offset;
      cmd->size = size;
      char *variable_data = (char *) (cmd + 1);
      memcpy(variable_data, data, size);
      _mesa_post_marshal_hook(ctx);
   } else {
      _mesa_glthread_finish(ctx);
      CALL_NamedBufferSubData(ctx->CurrentServerDispatch,
                              (buffer, offset, size, data));
   }
}

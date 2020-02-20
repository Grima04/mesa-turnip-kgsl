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

/** \file marshal.h
 *
 * Declarations of functions related to marshalling GL calls from a client
 * thread to a server thread.
 */

#ifndef MARSHAL_H
#define MARSHAL_H

#include "main/glthread.h"
#include "main/context.h"
#include "main/macros.h"
#include "marshal_generated.h"

struct marshal_cmd_base
{
   /**
    * Type of command.  See enum marshal_dispatch_cmd_id.
    */
   uint16_t cmd_id;

   /**
    * Size of command, in multiples of 4 bytes, including cmd_base.
    */
   uint16_t cmd_size;
};

typedef void (*_mesa_unmarshal_func)(struct gl_context *ctx, const void *cmd);
extern const _mesa_unmarshal_func _mesa_unmarshal_dispatch[NUM_DISPATCH_CMD];

static inline void *
_mesa_glthread_allocate_command(struct gl_context *ctx,
                                uint16_t cmd_id,
                                int size)
{
   struct glthread_state *glthread = ctx->GLThread;
   struct glthread_batch *next = &glthread->batches[glthread->next];
   struct marshal_cmd_base *cmd_base;
   const int aligned_size = ALIGN(size, 8);

   if (unlikely(next->used + size > MARSHAL_MAX_CMD_SIZE)) {
      _mesa_glthread_flush_batch(ctx);
      next = &glthread->batches[glthread->next];
   }

   cmd_base = (struct marshal_cmd_base *)&next->buffer[next->used];
   next->used += aligned_size;
   cmd_base->cmd_id = cmd_id;
   cmd_base->cmd_size = aligned_size;
   return cmd_base;
}

/**
 * Instead of conditionally handling marshaling previously-bound user vertex
 * array data in draw calls (deprecated and removed in GL core), we just
 * disable threading at the point where the user sets a user vertex array.
 */
static inline bool
_mesa_glthread_is_non_vbo_vertex_attrib_pointer(const struct gl_context *ctx)
{
   struct glthread_state *glthread = ctx->GLThread;

   return ctx->API != API_OPENGL_CORE && !glthread->vertex_array_is_vbo;
}

/**
 * Instead of conditionally handling marshaling immediate index data in draw
 * calls (deprecated and removed in GL core), we just disable threading.
 */
static inline bool
_mesa_glthread_is_non_vbo_draw_elements(const struct gl_context *ctx)
{
   struct glthread_state *glthread = ctx->GLThread;

   return ctx->API != API_OPENGL_CORE && !glthread->element_array_is_vbo;
}

static inline bool
_mesa_glthread_is_non_vbo_draw_arrays_indirect(const struct gl_context *ctx)
{
   struct glthread_state *glthread = ctx->GLThread;

   return ctx->API != API_OPENGL_CORE &&
          !glthread->draw_indirect_buffer_is_vbo;
}

static inline bool
_mesa_glthread_is_non_vbo_draw_elements_indirect(const struct gl_context *ctx)
{
   struct glthread_state *glthread = ctx->GLThread;

   return ctx->API != API_OPENGL_CORE &&
          (!glthread->draw_indirect_buffer_is_vbo ||
           !glthread->element_array_is_vbo);
}

#define DEBUG_MARSHAL_PRINT_CALLS 0

/**
 * This is printed when we have fallen back to a sync. This can happen when
 * MARSHAL_MAX_CMD_SIZE is exceeded.
 */
static inline void
debug_print_sync_fallback(const char *func)
{
#if DEBUG_MARSHAL_PRINT_CALLS
   printf("fallback to sync: %s\n", func);
#endif
}


static inline void
debug_print_sync(const char *func)
{
#if DEBUG_MARSHAL_PRINT_CALLS
   printf("sync: %s\n", func);
#endif
}

static inline void
debug_print_marshal(const char *func)
{
#if DEBUG_MARSHAL_PRINT_CALLS
   printf("marshal: %s\n", func);
#endif
}

struct _glapi_table *
_mesa_create_marshal_table(const struct gl_context *ctx);


/**
 * Checks whether we're on a compat context for code-generated
 * glBindVertexArray().
 *
 * In order to decide whether a draw call uses only VBOs for vertex and index
 * buffers, we track the current vertex and index buffer bindings by
 * glBindBuffer().  However, the index buffer binding is stored in the vertex
 * array as opposed to the context.  If we were to accurately track whether
 * the index buffer was a user pointer ot not, we'd have to track it per
 * vertex array, which would mean synchronizing with the client thread and
 * looking into the hash table to find the actual vertex array object.  That's
 * more tracking than we'd like to do in the main thread, if possible.
 *
 * Instead, just punt for now and disable threading on apps using vertex
 * arrays and compat contexts.  Apps using vertex arrays can probably use a
 * core context.
 */
static inline bool
_mesa_glthread_is_compat_bind_vertex_array(const struct gl_context *ctx)
{
   return ctx->API != API_OPENGL_CORE;
}

struct marshal_cmd_Enable;
struct marshal_cmd_ShaderSource;
struct marshal_cmd_Flush;
struct marshal_cmd_BindBuffer;
struct marshal_cmd_BufferData;
struct marshal_cmd_BufferSubData;
struct marshal_cmd_NamedBufferData;
struct marshal_cmd_NamedBufferSubData;

void
_mesa_unmarshal_Enable(struct gl_context *ctx,
                       const struct marshal_cmd_Enable *cmd);

void GLAPIENTRY
_mesa_marshal_Enable(GLenum cap);

void GLAPIENTRY
_mesa_marshal_ShaderSource(GLuint shader, GLsizei count,
                           const GLchar * const *string, const GLint *length);

void
_mesa_unmarshal_ShaderSource(struct gl_context *ctx,
                             const struct marshal_cmd_ShaderSource *cmd);

void GLAPIENTRY
_mesa_marshal_Flush(void);

void
_mesa_unmarshal_Flush(struct gl_context *ctx,
                      const struct marshal_cmd_Flush *cmd);

void GLAPIENTRY
_mesa_marshal_BindBuffer(GLenum target, GLuint buffer);

void
_mesa_unmarshal_BindBuffer(struct gl_context *ctx,
                           const struct marshal_cmd_BindBuffer *cmd);

void
_mesa_unmarshal_BufferData(struct gl_context *ctx,
                           const struct marshal_cmd_BufferData *cmd);

void GLAPIENTRY
_mesa_marshal_BufferData(GLenum target, GLsizeiptr size, const GLvoid * data,
                         GLenum usage);

void
_mesa_unmarshal_BufferSubData(struct gl_context *ctx,
                              const struct marshal_cmd_BufferSubData *cmd);

void GLAPIENTRY
_mesa_marshal_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                            const GLvoid * data);

void
_mesa_unmarshal_NamedBufferData(struct gl_context *ctx,
                                const struct marshal_cmd_NamedBufferData *cmd);

void GLAPIENTRY
_mesa_marshal_NamedBufferData(GLuint buffer, GLsizeiptr size,
                              const GLvoid * data, GLenum usage);

void
_mesa_unmarshal_NamedBufferSubData(struct gl_context *ctx,
                                   const struct marshal_cmd_NamedBufferSubData *cmd);

void GLAPIENTRY
_mesa_marshal_NamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size,
                                 const GLvoid * data);

static inline unsigned
_mesa_buffer_enum_to_count(GLenum buffer)
{
   switch (buffer) {
   case GL_COLOR:
      return 4;
   case GL_DEPTH_STENCIL:
      return 2;
   case GL_STENCIL:
   case GL_DEPTH:
      return 1;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_tex_param_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_TEXTURE_MIN_FILTER:
   case GL_TEXTURE_MAG_FILTER:
   case GL_TEXTURE_WRAP_S:
   case GL_TEXTURE_WRAP_T:
   case GL_TEXTURE_WRAP_R:
   case GL_TEXTURE_BASE_LEVEL:
   case GL_TEXTURE_MAX_LEVEL:
   case GL_GENERATE_MIPMAP_SGIS:
   case GL_TEXTURE_COMPARE_MODE_ARB:
   case GL_TEXTURE_COMPARE_FUNC_ARB:
   case GL_DEPTH_TEXTURE_MODE_ARB:
   case GL_DEPTH_STENCIL_TEXTURE_MODE:
   case GL_TEXTURE_SRGB_DECODE_EXT:
   case GL_TEXTURE_CUBE_MAP_SEAMLESS:
   case GL_TEXTURE_SWIZZLE_R:
   case GL_TEXTURE_SWIZZLE_G:
   case GL_TEXTURE_SWIZZLE_B:
   case GL_TEXTURE_SWIZZLE_A:
   case GL_TEXTURE_MIN_LOD:
   case GL_TEXTURE_MAX_LOD:
   case GL_TEXTURE_PRIORITY:
   case GL_TEXTURE_MAX_ANISOTROPY_EXT:
   case GL_TEXTURE_LOD_BIAS:
   case GL_TEXTURE_TILING_EXT:
      return 1;
   case GL_TEXTURE_CROP_RECT_OES:
   case GL_TEXTURE_SWIZZLE_RGBA:
   case GL_TEXTURE_BORDER_COLOR:
      return 4;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_fog_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_FOG_MODE:
   case GL_FOG_DENSITY:
   case GL_FOG_START:
   case GL_FOG_END:
   case GL_FOG_INDEX:
   case GL_FOG_COORDINATE_SOURCE_EXT:
   case GL_FOG_DISTANCE_MODE_NV:
      return 1;
   case GL_FOG_COLOR:
      return 4;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_light_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_AMBIENT:
   case GL_DIFFUSE:
   case GL_SPECULAR:
   case GL_POSITION:
      return 4;
   case GL_SPOT_DIRECTION:
      return 3;
   case GL_SPOT_EXPONENT:
   case GL_SPOT_CUTOFF:
   case GL_CONSTANT_ATTENUATION:
   case GL_LINEAR_ATTENUATION:
   case GL_QUADRATIC_ATTENUATION:
      return 1;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_light_model_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_LIGHT_MODEL_AMBIENT:
      return 4;
   case GL_LIGHT_MODEL_LOCAL_VIEWER:
   case GL_LIGHT_MODEL_TWO_SIDE:
   case GL_LIGHT_MODEL_COLOR_CONTROL:
      return 1;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_texenv_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_TEXTURE_ENV_MODE:
   case GL_COMBINE_RGB:
   case GL_COMBINE_ALPHA:
   case GL_SOURCE0_RGB:
   case GL_SOURCE1_RGB:
   case GL_SOURCE2_RGB:
   case GL_SOURCE3_RGB_NV:
   case GL_SOURCE0_ALPHA:
   case GL_SOURCE1_ALPHA:
   case GL_SOURCE2_ALPHA:
   case GL_SOURCE3_ALPHA_NV:
   case GL_OPERAND0_RGB:
   case GL_OPERAND1_RGB:
   case GL_OPERAND2_RGB:
   case GL_OPERAND3_RGB_NV:
   case GL_OPERAND0_ALPHA:
   case GL_OPERAND1_ALPHA:
   case GL_OPERAND2_ALPHA:
   case GL_OPERAND3_ALPHA_NV:
   case GL_RGB_SCALE:
   case GL_ALPHA_SCALE:
   case GL_TEXTURE_LOD_BIAS_EXT:
   case GL_COORD_REPLACE_NV:
      return 1;
   case GL_TEXTURE_ENV_COLOR:
      return 4;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_texgen_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_TEXTURE_GEN_MODE:
      return 1;
   case GL_OBJECT_PLANE:
   case GL_EYE_PLANE:
      return 4;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_material_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_EMISSION:
   case GL_AMBIENT:
   case GL_DIFFUSE:
   case GL_SPECULAR:
   case GL_AMBIENT_AND_DIFFUSE:
      return 4;
   case GL_COLOR_INDEXES:
      return 3;
   case GL_SHININESS:
      return 1;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_point_param_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_DISTANCE_ATTENUATION_EXT:
      return 3;
   case GL_POINT_SIZE_MIN_EXT:
   case GL_POINT_SIZE_MAX_EXT:
   case GL_POINT_FADE_THRESHOLD_SIZE_EXT:
   case GL_POINT_SPRITE_R_MODE_NV:
   case GL_POINT_SPRITE_COORD_ORIGIN:
      return 1;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_calllists_enum_to_count(GLenum type)
{
   switch (type) {
   case GL_BYTE:
   case GL_UNSIGNED_BYTE:
      return 1;
   case GL_SHORT:
   case GL_UNSIGNED_SHORT:
   case GL_2_BYTES:
      return 2;
   case GL_3_BYTES:
      return 3;
   case GL_INT:
   case GL_UNSIGNED_INT:
   case GL_FLOAT:
   case GL_4_BYTES:
      return 4;
   default:
      return 0;
   }
}

static inline unsigned
_mesa_patch_param_enum_to_count(GLenum pname)
{
   switch (pname) {
   case GL_PATCH_DEFAULT_OUTER_LEVEL:
      return 4;
   case GL_PATCH_DEFAULT_INNER_LEVEL:
      return 2;
   default:
      return 0;
   }
}

#endif /* MARSHAL_H */

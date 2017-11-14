/*
 * SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
 * Copyright (C) 1991-2000 Silicon Graphics, Inc. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice including the dates of first publication and
 * either this permission notice or a reference to
 * http://oss.sgi.com/projects/FreeB/
 * shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * SILICON GRAPHICS, INC. BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Except as contained in this notice, the name of Silicon Graphics, Inc.
 * shall not be used in advertising or otherwise to promote the sale, use or
 * other dealings in this Software without prior written authorization from
 * Silicon Graphics, Inc.
 */

/**
 * \file glxcurrent.c
 * Client-side GLX interface for current context management.
 */

#include <pthread.h>

#include "glxclient.h"
#include "glapi.h"
#include "glx_error.h"

/*
** We setup some dummy structures here so that the API can be used
** even if no context is current.
*/

static GLubyte dummyBuffer[__GLX_BUFFER_LIMIT_SIZE];
static struct glx_context_vtable dummyVtable;
/*
** Dummy context used by small commands when there is no current context.
** All the
** gl and glx entry points are designed to operate as nop's when using
** the dummy context structure.
*/
struct glx_context dummyContext = {
   &dummyBuffer[0],
   &dummyBuffer[0],
   &dummyBuffer[0],
   &dummyBuffer[__GLX_BUFFER_LIMIT_SIZE],
   sizeof(dummyBuffer),
   &dummyVtable
};

/*
 * Current context management and locking
 */

_X_HIDDEN pthread_mutex_t __glXmutex = PTHREAD_MUTEX_INITIALIZER;

# if defined( USE_ELF_TLS )

/**
 * Per-thread GLX context pointer.
 *
 * \c __glXSetCurrentContext is written is such a way that this pointer can
 * \b never be \c NULL.  This is important!  Because of this
 * \c __glXGetCurrentContext can be implemented as trivial macro.
 */
__thread void *__glX_tls_Context __attribute__ ((tls_model("initial-exec")))
   = &dummyContext;

_X_HIDDEN void
__glXSetCurrentContext(struct glx_context * c)
{
   __glX_tls_Context = (c != NULL) ? c : &dummyContext;
}

# else

static pthread_once_t once_control = PTHREAD_ONCE_INIT;

/**
 * Per-thread data key.
 *
 * Once \c init_thread_data has been called, the per-thread data key will
 * take a value of \c NULL.  As each new thread is created the default
 * value, in that thread, will be \c NULL.
 */
static pthread_key_t ContextTSD;

/**
 * Initialize the per-thread data key.
 *
 * This function is called \b exactly once per-process (not per-thread!) to
 * initialize the per-thread data key.  This is ideally done using the
 * \c pthread_once mechanism.
 */
static void
init_thread_data(void)
{
   if (pthread_key_create(&ContextTSD, NULL) != 0) {
      perror("pthread_key_create");
      exit(-1);
   }
}

_X_HIDDEN void
__glXSetCurrentContext(struct glx_context * c)
{
   pthread_once(&once_control, init_thread_data);
   pthread_setspecific(ContextTSD, c);
}

_X_HIDDEN struct glx_context *
__glXGetCurrentContext(void)
{
   void *v;

   pthread_once(&once_control, init_thread_data);

   v = pthread_getspecific(ContextTSD);
   return (v == NULL) ? &dummyContext : (struct glx_context *) v;
}

# endif /* defined( USE_ELF_TLS ) */


_X_HIDDEN void
__glXSetCurrentContextNull(void)
{
   __glXSetCurrentContext(&dummyContext);
#if defined(GLX_DIRECT_RENDERING)
   _glapi_set_dispatch(NULL);   /* no-op functions */
   _glapi_set_context(NULL);
#endif
}

_GLX_PUBLIC GLXContext
glXGetCurrentContext(void)
{
   struct glx_context *cx = __glXGetCurrentContext();

   if (cx == &dummyContext) {
      return NULL;
   }
   else {
      return (GLXContext) cx;
   }
}

_GLX_PUBLIC GLXDrawable
glXGetCurrentDrawable(void)
{
   struct glx_context *gc = __glXGetCurrentContext();
   return gc->currentDrawable;
}

static Bool
SendMakeCurrentRequest(Display * dpy, GLXContextID gc_id,
                       GLXContextTag gc_tag, GLXDrawable draw,
                       GLXDrawable read, GLXContextTag *out_tag)
{
   xGLXMakeCurrentReply reply;
   Bool ret;
   int opcode = __glXSetupForCommand(dpy);

   LockDisplay(dpy);

   if (draw == read) {
      xGLXMakeCurrentReq *req;

      GetReq(GLXMakeCurrent, req);
      req->reqType = opcode;
      req->glxCode = X_GLXMakeCurrent;
      req->drawable = draw;
      req->context = gc_id;
      req->oldContextTag = gc_tag;
   }
   else {
      struct glx_display *priv = __glXInitialize(dpy);

      if ((priv->majorVersion > 1) || (priv->minorVersion >= 3)) {
         xGLXMakeContextCurrentReq *req;

         GetReq(GLXMakeContextCurrent, req);
         req->reqType = opcode;
         req->glxCode = X_GLXMakeContextCurrent;
         req->drawable = draw;
         req->readdrawable = read;
         req->context = gc_id;
         req->oldContextTag = gc_tag;
      }
      else {
         xGLXVendorPrivateWithReplyReq *vpreq;
         xGLXMakeCurrentReadSGIReq *req;

         GetReqExtra(GLXVendorPrivateWithReply,
                     sz_xGLXMakeCurrentReadSGIReq -
                     sz_xGLXVendorPrivateWithReplyReq, vpreq);
         req = (xGLXMakeCurrentReadSGIReq *) vpreq;
         req->reqType = opcode;
         req->glxCode = X_GLXVendorPrivateWithReply;
         req->vendorCode = X_GLXvop_MakeCurrentReadSGI;
         req->drawable = draw;
         req->readable = read;
         req->context = gc_id;
         req->oldContextTag = gc_tag;
      }
   }

   ret = _XReply(dpy, (xReply *) &reply, 0, False);


   if (ret == 1)
      *out_tag = reply.contextTag;

   UnlockDisplay(dpy);
   SyncHandle();

   return ret;
}

static void
SetGC(struct glx_context *gc, Display *dpy, GLXDrawable draw, GLXDrawable read)
{
   gc->currentDpy = dpy;
   gc->currentDrawable = draw;
   gc->currentReadable = read;
}

static Bool
should_send(Display *dpy, struct glx_context *gc)
{
   /* always send for indirect contexts */
   if (!gc->isDirect)
      return 1;

   /* don't send for broken servers. */
   if (VendorRelease(dpy) < 12006000 || VendorRelease(dpy) >= 40000000)
      return 0;

   return 1;
}

static Bool
MakeContextCurrent(Display * dpy, GLXDrawable draw,
                   GLXDrawable read, GLXContext gc_user)
{
   struct glx_context *gc = (struct glx_context *) gc_user;
   struct glx_context *oldGC = __glXGetCurrentContext();
   Bool ret = GL_FALSE;

   /* Make sure that the new context has a nonzero ID.  In the request,
    * a zero context ID is used only to mean that we bind to no current
    * context.
    */
   if ((gc != NULL) && (gc->xid == None)) {
      return GL_FALSE;
   }

   _glapi_check_multithread();
   __glXLock();

   if (oldGC == gc &&
       gc->currentDrawable == draw &&
       gc->currentReadable == read) {
      /* Same context and drawables: no op, just return */
      ret = GL_TRUE;
   }

   else if (oldGC == gc) {
      /* Same context and new drawables: update drawable bindings */
      if (should_send(dpy, gc)) {
         if (!SendMakeCurrentRequest(dpy, gc->xid, gc->currentContextTag,
                                     draw, read, &gc->currentContextTag)) {
            goto out;
         }
      }

      if (gc->vtable->bind(gc, gc, draw, read) != Success) {
         __glXSetCurrentContextNull();
         goto out;
      }
   }

   else {
      /* Different contexts: release the old, bind the new */
      GLXContextTag oldTag = oldGC->currentContextTag;

      if (oldGC != &dummyContext) {

         if (--oldGC->thread_refcount == 0) {
            if (oldGC->xid != None &&
                should_send(dpy, oldGC) &&
                !SendMakeCurrentRequest(dpy, None, oldTag, None, None,
					&oldGC->currentContextTag)) {
               goto out;
            }

            oldGC->vtable->unbind(oldGC, gc);

            if (oldGC->xid == None) {
               /* destroyed context, free it */
               oldGC->vtable->destroy(oldGC);
               oldTag = 0;
            } else {
               SetGC(oldGC, NULL, None, None);
               oldTag = oldGC->currentContextTag;
            }
         }
      }
      __glXSetCurrentContextNull();

      if (gc) {
         /*
          * MESA_multithread_makecurrent makes this complicated. We need to
          * send the request if the new context is
          *
          * a) indirect (may be current to another client), or
          * b) (direct and) newly being made current, or
          * c) (direct and) being bound to new drawables
          */
         Bool new_drawables = gc->currentReadable != read ||
                              gc->currentDrawable != draw;

         if (should_send(dpy, gc)) {
            if (!gc->isDirect || !gc->thread_refcount || new_drawables) {
               if (!SendMakeCurrentRequest(dpy, gc->xid, oldTag, draw, read,
                                           &gc->currentContextTag)) {
                  goto out;
               }
            }
         }

         if (gc->vtable->bind(gc, oldGC, draw, read) != Success) {
            __glXSendError(dpy, GLXBadContext, None, X_GLXMakeContextCurrent,
                           False);
            goto out;
         }

         if (gc->thread_refcount == 0) {
            SetGC(gc, dpy, draw, read);
         }
         gc->thread_refcount++;
         __glXSetCurrentContext(gc);
      }
   }
   ret = GL_TRUE;

out:
   __glXUnlock();
   return ret;
}


_GLX_PUBLIC Bool
glXMakeCurrent(Display * dpy, GLXDrawable draw, GLXContext gc)
{
   return MakeContextCurrent(dpy, draw, draw, gc);
}

_GLX_PUBLIC
GLX_ALIAS(Bool, glXMakeCurrentReadSGI,
          (Display * dpy, GLXDrawable d, GLXDrawable r, GLXContext ctx),
          (dpy, d, r, ctx), MakeContextCurrent)

_GLX_PUBLIC
GLX_ALIAS(Bool, glXMakeContextCurrent,
          (Display * dpy, GLXDrawable d, GLXDrawable r,
           GLXContext ctx), (dpy, d, r, ctx), MakeContextCurrent)

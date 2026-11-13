/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The EGL Kit: EGL with no display** (`docs/w6.md` W6b; Diego, 13
 * November 2026). Kosmos has no GPU API yet, and what asks for one -
 * WebCore, whose every port links GL ES, and Skia's GPU code under it -
 * asks through EGL first: is there a display? Here the answer is no, every
 * time, and a program that can live without a GPU then does - WebKit paints
 * with Skia on the processor, as WebKitGTK does on a machine with none.
 *
 * So every function below answers as the EGL 1.5 specification says a call
 * on a display that does not exist is answered: `EGL_NO_DISPLAY` from the
 * two that would find one, `EGL_FALSE` and `EGL_BAD_DISPLAY` from those
 * that take one, a null where an object would come back, no extensions and
 * no functions from `eglGetProcAddress`. The four that need no display -
 * waiting on a client, on GL, on the native system, releasing the thread -
 * succeed, having nothing to wait for. And `eglBindAPI` refuses every API
 * with `EGL_BAD_PARAMETER`: none is supported.
 *
 * The GL ES functions are `gles_unreached.c`, generated from the same
 * Khronos header, and each stops the process: with no display there is no
 * context, so a call to one is a caller that did not ask EGL first.
 *
 * When Kosmos has a GPU API - virgl under QEMU, the M700's own
 * (`docs/m700-gpu.md`) - this file is where a display comes from, and
 * every program linked against the kit gains it unchanged.
 *
 * The headers are the Khronos ones WebKit carries
 * (`Source/ThirdParty/ANGLE/include`), compiled with
 * `EGL_NO_PLATFORM_SPECIFIC_TYPES`, since Kosmos is not a platform they
 * know.
 */

#include <EGL/egl.h>
#include <stdatomic.h>

/* What the last call on any thread said, which `eglGetError` returns and
 * resets. The specification keeps one per thread; with every call but six
 * failing the same way, one for the process says the same thing. */
static atomic_int last_error = EGL_SUCCESS;

static EGLBoolean no_display(void)
{
    atomic_store(&last_error, EGL_BAD_DISPLAY);
    return EGL_FALSE;
}

static EGLBoolean nothing_to_do(void)
{
    atomic_store(&last_error, EGL_SUCCESS);
    return EGL_TRUE;
}

/* The two that would find a display. */
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
    (void)display_id;
    atomic_store(&last_error, EGL_SUCCESS);
    return EGL_NO_DISPLAY;
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display, const EGLAttrib *attrib_list)
{
    (void)native_display; (void)attrib_list;
    (void)platform;
    atomic_store(&last_error, EGL_BAD_PARAMETER);
    return EGL_NO_DISPLAY;
}

EGLint eglGetError(void)
{
    return atomic_exchange(&last_error, EGL_SUCCESS);
}

/* No extensions, for no display or for EGL itself: NULL, as the
 * specification answers for a display it does not know. */
const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    (void)dpy; (void)name;
    no_display();
    return 0;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
{
    (void)procname;
    return 0;
}

EGLBoolean eglBindAPI(EGLenum api)
{
    (void)api;
    atomic_store(&last_error, EGL_BAD_PARAMETER);
    return EGL_FALSE;
}

EGLenum eglQueryAPI(void)
{
    return EGL_NONE;
}

/* Nothing to wait for, and no thread state to release. */
EGLBoolean eglWaitClient(void) { return nothing_to_do(); }
EGLBoolean eglWaitGL(void) { return nothing_to_do(); }
EGLBoolean eglWaitNative(EGLint engine) { (void)engine; return nothing_to_do(); }
EGLBoolean eglReleaseThread(void) { return nothing_to_do(); }

/* Nothing is current, because nothing could be made. */
EGLDisplay eglGetCurrentDisplay(void) { return EGL_NO_DISPLAY; }
EGLContext eglGetCurrentContext(void) { return EGL_NO_CONTEXT; }
EGLSurface eglGetCurrentSurface(EGLint readdraw) { (void)readdraw; return EGL_NO_SURFACE; }

/* Everything else takes a display, and there is none. */
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{ (void)dpy; (void)major; (void)minor; return no_display(); }
EGLBoolean eglTerminate(EGLDisplay dpy)
{ (void)dpy; return no_display(); }
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                           EGLint config_size, EGLint *num_config)
{ (void)dpy; (void)attrib_list; (void)configs; (void)config_size; (void)num_config; return no_display(); }
EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config)
{ (void)dpy; (void)configs; (void)config_size; (void)num_config; return no_display(); }
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{ (void)dpy; (void)config; (void)attribute; (void)value; return no_display(); }

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                            const EGLint *attrib_list)
{ (void)dpy; (void)config; (void)share_context; (void)attrib_list; no_display(); return EGL_NO_CONTEXT; }
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{ (void)dpy; (void)ctx; return no_display(); }
EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value)
{ (void)dpy; (void)ctx; (void)attribute; (void)value; return no_display(); }
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{ (void)dpy; (void)draw; (void)read; (void)ctx; return no_display(); }

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                  const EGLint *attrib_list)
{ (void)dpy; (void)config; (void)win; (void)attrib_list; no_display(); return EGL_NO_SURFACE; }
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list)
{ (void)dpy; (void)config; (void)attrib_list; no_display(); return EGL_NO_SURFACE; }
EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                  const EGLint *attrib_list)
{ (void)dpy; (void)config; (void)pixmap; (void)attrib_list; no_display(); return EGL_NO_SURFACE; }
EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer,
                                            EGLConfig config, const EGLint *attrib_list)
{ (void)dpy; (void)buftype; (void)buffer; (void)config; (void)attrib_list; no_display(); return EGL_NO_SURFACE; }
EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void *native_window,
                                          const EGLAttrib *attrib_list)
{ (void)dpy; (void)config; (void)native_window; (void)attrib_list; no_display(); return EGL_NO_SURFACE; }
EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void *native_pixmap,
                                          const EGLAttrib *attrib_list)
{ (void)dpy; (void)config; (void)native_pixmap; (void)attrib_list; no_display(); return EGL_NO_SURFACE; }
EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{ (void)dpy; (void)surface; return no_display(); }
EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value)
{ (void)dpy; (void)surface; (void)attribute; (void)value; return no_display(); }
EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value)
{ (void)dpy; (void)surface; (void)attribute; (void)value; return no_display(); }
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{ (void)dpy; (void)surface; return no_display(); }
EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{ (void)dpy; (void)interval; return no_display(); }
EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target)
{ (void)dpy; (void)surface; (void)target; return no_display(); }
EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{ (void)dpy; (void)surface; (void)buffer; return no_display(); }
EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{ (void)dpy; (void)surface; (void)buffer; return no_display(); }

EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list)
{ (void)dpy; (void)type; (void)attrib_list; no_display(); return EGL_NO_SYNC; }
EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync)
{ (void)dpy; (void)sync; return no_display(); }
EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout)
{ (void)dpy; (void)sync; (void)flags; (void)timeout; no_display(); return EGL_FALSE; }
EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags)
{ (void)dpy; (void)sync; (void)flags; return no_display(); }
EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib *value)
{ (void)dpy; (void)sync; (void)attribute; (void)value; return no_display(); }

EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                        const EGLAttrib *attrib_list)
{ (void)dpy; (void)ctx; (void)target; (void)buffer; (void)attrib_list; no_display(); return EGL_NO_IMAGE; }
EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage image)
{ (void)dpy; (void)image; return no_display(); }

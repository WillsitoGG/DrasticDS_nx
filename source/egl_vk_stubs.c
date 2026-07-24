/* egl_vk_stubs.c -- the few EGL/GLES symbols NVK does NOT already stub, for the
 * VULKAN build only.
 *
 * The Vulkan (NVK) build does NOT link switch-mesa's libEGL/libGLESv2 (they and
 * NVK both bundle mesa util/nir/compiler object code and can't co-link). But
 * the shared import layer references EGL entry points even when the Vulkan host
 * is selected. NVK's own
 * rust_switch_stubs already provides most egl* symbols (eglGetDisplay,
 * eglSwapBuffers, ...); this file adds only the handful it does not, so the link
 * resolves. The Vulkan presenter uses DraStic's renderFrame only to select and
 * consume its completed screen buffer; glBindTexture/glTexSubImage2D are
 * redirected to the CPU capture bridge and no GLES context is created. These
 * remaining EGL queries therefore fail benignly. Compiled to nothing in the
 * OpenGL build (real switch-mesa symbols linked instead).
 */
#ifdef USE_VULKAN

#include <EGL/egl.h>
#include <GLES2/gl2.h>

EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v) {
  (void)d; (void)s; (void)a; if (v) *v = 0; return EGL_FALSE; }
EGLContext eglGetCurrentContext(void) { return EGL_NO_CONTEXT; }
EGLSurface eglGetCurrentSurface(EGLint rw) { (void)rw; return EGL_NO_SURFACE; }
const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)""; }

#endif // USE_VULKAN

/* Initialization-only EGL/GLES compatibility used by Android CRI plugins.
 *
 * These entry points intentionally own no GPU objects and must never be used as
 * Unity's renderer.  They only make CRI's Android OpenGL capability probe and
 * shutdown paths deterministic while the game itself renders through Vulkan.
 */
#ifndef GENSHIN_VULKAN_EGL_STUBS_H
#define GENSHIN_VULKAN_EGL_STUBS_H

#include <EGL/egl.h>
#include <GLES2/gl2.h>

/* EGL import set used by libcri_ware_unity.so. */
EGLDisplay nx_eglGetDisplay_stub(EGLNativeDisplayType display_id);
EGLBoolean nx_eglInitialize_stub(EGLDisplay display, EGLint *major, EGLint *minor);
EGLBoolean nx_eglTerminate_stub(EGLDisplay display);
EGLBoolean nx_eglChooseConfig_stub(EGLDisplay display, const EGLint *attributes,
                                   EGLConfig *configs, EGLint config_size,
                                   EGLint *config_count);
EGLBoolean nx_eglGetConfigAttrib_stub(EGLDisplay display, EGLConfig config,
                                      EGLint attribute, EGLint *value);
EGLContext nx_eglCreateContext_stub(EGLDisplay display, EGLConfig config,
                                    EGLContext shared_context,
                                    const EGLint *attributes);
EGLSurface nx_eglCreatePbufferSurface_stub(EGLDisplay display, EGLConfig config,
                                           const EGLint *attributes);
EGLSurface nx_eglCreateWindowSurface_stub(EGLDisplay display, EGLConfig config,
                                          EGLNativeWindowType window,
                                          const EGLint *attributes);
EGLBoolean nx_eglDestroyContext_stub(EGLDisplay display, EGLContext context);
EGLBoolean nx_eglDestroySurface_stub(EGLDisplay display, EGLSurface surface);
EGLBoolean nx_eglMakeCurrent_stub(EGLDisplay display, EGLSurface draw,
                                  EGLSurface read, EGLContext context);
EGLContext nx_eglGetCurrentContext_stub(void);
EGLSurface nx_eglGetCurrentSurface_stub(EGLint which);
EGLint nx_eglGetError_stub(void);
const char *nx_eglQueryString_stub(EGLDisplay display, EGLint name);
EGLBoolean nx_eglQuerySurface_stub(EGLDisplay display, EGLSurface surface,
                                   EGLint attribute, EGLint *value);
EGLBoolean nx_eglSurfaceAttrib_stub(EGLDisplay display, EGLSurface surface,
                                    EGLint attribute, EGLint value);
EGLBoolean nx_eglSwapBuffers_stub(EGLDisplay display, EGLSurface surface);
EGLBoolean nx_eglSwapInterval_stub(EGLDisplay display, EGLint interval);
__eglMustCastToProperFunctionPointerType
nx_eglGetProcAddress_stub(const char *name);

/* GLES import set shared by libcri_ware_unity.so and libcri_vip_unity.so. */
void nx_glActiveTexture_stub(GLenum texture);
void nx_glBindBuffer_stub(GLenum target, GLuint buffer);
void nx_glBindTexture_stub(GLenum target, GLuint texture);
void nx_glBufferData_stub(GLenum target, GLsizeiptr size, const void *data,
                          GLenum usage);
void nx_glDeleteBuffers_stub(GLsizei count, const GLuint *buffers);
void nx_glDeleteTextures_stub(GLsizei count, const GLuint *textures);
void nx_glGenBuffers_stub(GLsizei count, GLuint *buffers);
void nx_glGenTextures_stub(GLsizei count, GLuint *textures);
GLenum nx_glGetError_stub(void);
void nx_glGetIntegerv_stub(GLenum pname, GLint *data);
void nx_glGetShaderPrecisionFormat_stub(GLenum shader_type,
                                        GLenum precision_type,
                                        GLint *range, GLint *precision);
const GLubyte *nx_glGetString_stub(GLenum name);
void nx_glTexImage2D_stub(GLenum target, GLint level, GLint internal_format,
                          GLsizei width, GLsizei height, GLint border,
                          GLenum format, GLenum type, const void *pixels);
void nx_glCompressedTexImage2D_stub(GLenum target, GLint level,
                                    GLenum internal_format, GLsizei width,
                                    GLsizei height, GLint border,
                                    GLsizei image_size, const void *data);
void nx_glTexImage2DMultisample_stub(GLenum target, GLsizei samples,
                                     GLint internal_format, GLsizei width,
                                     GLsizei height,
                                     GLboolean fixed_sample_locations);
void nx_glTexStorage2DMultisample_stub(GLenum target, GLsizei samples,
                                       GLenum internal_format, GLsizei width,
                                       GLsizei height,
                                       GLboolean fixed_sample_locations);
void nx_glTexParameterf_stub(GLenum target, GLenum pname, GLfloat param);
void nx_glTexParameteri_stub(GLenum target, GLenum pname, GLint param);
void nx_glTexSubImage2D_stub(GLenum target, GLint level, GLint x_offset,
                             GLint y_offset, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, const void *pixels);

/* CRI obtains these optional map functions through eglGetProcAddress.  They
 * expose bounded CPU scratch memory only; no data reaches a Vulkan texture. */
void *nx_glMapBufferOES_stub(GLenum target, GLenum access);
GLboolean nx_glUnmapBufferOES_stub(GLenum target);
void *nx_glMapBufferRange_stub(GLenum target, GLintptr offset,
                               GLsizeiptr length, GLbitfield access);
GLboolean nx_glUnmapBuffer_stub(GLenum target);

/* Shared by eglGetProcAddress and any host dlsym compatibility fallback. */
void *nx_egl_gles_compat_lookup(const char *name);

#endif

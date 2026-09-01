/* The loaderless NVK SDK intentionally carries hard-failure EGL symbols for
 * Rust link compatibility.  Android CRI plugins nevertheless probe a small
 * EGL/GLES surface after Unity selected Vulkan.  The nx_* entry points below
 * provide deterministic initialization/shutdown behavior without creating GPU
 * state.  Texture uploads are discarded and bounded mapped buffers are CPU
 * scratch only, so this is not a CRI video renderer. */

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_log_sink.h"
#include "config.h"
#include "vulkan_egl_stubs.h"

#define CRI_MAP_LIMIT ((size_t)16 * 1024 * 1024)
#define CRI_MAP_DEFAULT ((size_t)4096)

typedef struct CriGlThreadState {
  uint8_t *map_storage;
  size_t map_capacity;
  size_t buffer_size;
  int mapped;
  GLenum error;
  EGLint egl_error;
  EGLContext current_context;
  EGLSurface current_draw;
  EGLSurface current_read;
} CriGlThreadState;

static __thread CriGlThreadState g_cri_gl;
static uint32_t g_next_gl_name = 1;
static uint32_t g_missing_proc_trace_count;

static unsigned char g_egl_display_token;
static unsigned char g_egl_config_token;
static unsigned char g_egl_context_token;
static unsigned char g_egl_surface_token;

static EGLDisplay fake_display(void) {
  return (EGLDisplay)(uintptr_t)&g_egl_display_token;
}

static EGLConfig fake_config(void) {
  return (EGLConfig)(uintptr_t)&g_egl_config_token;
}

static EGLContext fake_context(void) {
  return (EGLContext)(uintptr_t)&g_egl_context_token;
}

static EGLSurface fake_surface(void) {
  return (EGLSurface)(uintptr_t)&g_egl_surface_token;
}

static int valid_display(EGLDisplay display) {
  return display == fake_display();
}

static void set_egl_error(EGLint error) {
  g_cri_gl.egl_error = error;
}

static EGLBoolean egl_failure(EGLint error) {
  set_egl_error(error);
  return EGL_FALSE;
}

static void set_gl_error(GLenum error) {
  if (g_cri_gl.error == GL_NO_ERROR) g_cri_gl.error = error;
}

static GLuint allocate_gl_name(void) {
  GLuint result = (GLuint)__atomic_fetch_add(&g_next_gl_name, 1u,
                                              __ATOMIC_RELAXED);
  if (result != 0) return result;
  result = (GLuint)__atomic_fetch_add(&g_next_gl_name, 1u, __ATOMIC_RELAXED);
  return result != 0 ? result : 1u;
}

static void generate_gl_names(GLsizei count, GLuint *names) {
  if (count < 0) {
    set_gl_error(GL_INVALID_VALUE);
    return;
  }
  if (count && !names) {
    set_gl_error(GL_INVALID_VALUE);
    return;
  }
  for (GLsizei i = 0; i < count; ++i) names[i] = allocate_gl_name();
}

static void *map_scratch(size_t required) {
  if (!required) required = CRI_MAP_DEFAULT;
  if (required > CRI_MAP_LIMIT) {
    set_gl_error(GL_OUT_OF_MEMORY);
    return NULL;
  }
  if (g_cri_gl.map_capacity < required) {
    uint8_t *next = (uint8_t *)realloc(g_cri_gl.map_storage, required);
    if (!next) {
      set_gl_error(GL_OUT_OF_MEMORY);
      return NULL;
    }
    memset(next + g_cri_gl.map_capacity, 0, required - g_cri_gl.map_capacity);
    g_cri_gl.map_storage = next;
    g_cri_gl.map_capacity = required;
  }
  g_cri_gl.mapped = 1;
  return g_cri_gl.map_storage;
}

EGLDisplay nx_eglGetDisplay_stub(EGLNativeDisplayType display_id) {
  (void)display_id;
  return fake_display();
}

EGLBoolean nx_eglInitialize_stub(EGLDisplay display, EGLint *major,
                                 EGLint *minor) {
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (major) *major = 1;
  if (minor) *minor = 4;
  return EGL_TRUE;
}

EGLBoolean nx_eglTerminate_stub(EGLDisplay display) {
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  free(g_cri_gl.map_storage);
  memset(&g_cri_gl, 0, sizeof(g_cri_gl));
  return EGL_TRUE;
}

EGLBoolean nx_eglChooseConfig_stub(EGLDisplay display,
                                   const EGLint *attributes,
                                   EGLConfig *configs, EGLint config_size,
                                   EGLint *config_count) {
  (void)attributes;
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (!config_count || config_size < 0) return egl_failure(EGL_BAD_PARAMETER);
  *config_count = 1;
  if (configs && config_size > 0) configs[0] = fake_config();
  return EGL_TRUE;
}

EGLBoolean nx_eglGetConfigAttrib_stub(EGLDisplay display, EGLConfig config,
                                      EGLint attribute, EGLint *value) {
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (config != fake_config()) return egl_failure(EGL_BAD_CONFIG);
  if (!value) return egl_failure(EGL_BAD_PARAMETER);
  switch (attribute) {
    case EGL_BUFFER_SIZE: *value = 32; break;
    case EGL_RED_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_BLUE_SIZE:
    case EGL_ALPHA_SIZE: *value = 8; break;
    case EGL_DEPTH_SIZE: *value = 24; break;
    case EGL_STENCIL_SIZE: *value = 8; break;
    case EGL_CONFIG_CAVEAT:
    case EGL_NATIVE_VISUAL_TYPE:
    case EGL_TRANSPARENT_TYPE: *value = EGL_NONE; break;
    case EGL_CONFIG_ID:
    case EGL_NATIVE_VISUAL_ID: *value = 1; break;
    case EGL_LEVEL:
    case EGL_SAMPLES:
    case EGL_SAMPLE_BUFFERS:
    case EGL_TRANSPARENT_RED_VALUE:
    case EGL_TRANSPARENT_GREEN_VALUE:
    case EGL_TRANSPARENT_BLUE_VALUE:
    case EGL_LUMINANCE_SIZE:
    case EGL_ALPHA_MASK_SIZE:
    case EGL_MIN_SWAP_INTERVAL: *value = 0; break;
    case EGL_MAX_PBUFFER_WIDTH:
    case EGL_MAX_PBUFFER_HEIGHT: *value = 4096; break;
    case EGL_MAX_PBUFFER_PIXELS: *value = 4096 * 4096; break;
    case EGL_MAX_SWAP_INTERVAL: *value = 1; break;
    case EGL_NATIVE_RENDERABLE:
    case EGL_BIND_TO_TEXTURE_RGB:
    case EGL_BIND_TO_TEXTURE_RGBA: *value = EGL_FALSE; break;
    case EGL_SURFACE_TYPE: *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
    case EGL_RENDERABLE_TYPE:
    case EGL_CONFORMANT:
      *value = EGL_OPENGL_ES2_BIT | 0x0040; /* EGL_OPENGL_ES3_BIT_KHR */
      break;
    case EGL_COLOR_BUFFER_TYPE: *value = EGL_RGB_BUFFER; break;
    case 0x3142: /* EGL_RECORDABLE_ANDROID */
    case 0x3147: /* EGL_FRAMEBUFFER_TARGET_ANDROID */
      *value = EGL_TRUE;
      break;
    case 0x3339: /* EGL_COLOR_COMPONENT_TYPE_EXT */
      *value = 0x333a; /* EGL_COLOR_COMPONENT_TYPE_FIXED_EXT */
      break;
    case 0x32c0: /* EGL_PROTECTED_CONTENT_EXT */
      *value = EGL_FALSE;
      break;
    default: return egl_failure(EGL_BAD_ATTRIBUTE);
  }
  return EGL_TRUE;
}

EGLContext nx_eglCreateContext_stub(EGLDisplay display, EGLConfig config,
                                    EGLContext shared_context,
                                    const EGLint *attributes) {
  (void)shared_context;
  (void)attributes;
  if (!valid_display(display)) {
    set_egl_error(EGL_BAD_DISPLAY);
    return EGL_NO_CONTEXT;
  }
  if (config != fake_config()) {
    set_egl_error(EGL_BAD_CONFIG);
    return EGL_NO_CONTEXT;
  }
  return fake_context();
}

EGLSurface nx_eglCreatePbufferSurface_stub(EGLDisplay display,
                                           EGLConfig config,
                                           const EGLint *attributes) {
  (void)attributes;
  if (!valid_display(display)) {
    set_egl_error(EGL_BAD_DISPLAY);
    return EGL_NO_SURFACE;
  }
  if (config != fake_config()) {
    set_egl_error(EGL_BAD_CONFIG);
    return EGL_NO_SURFACE;
  }
  return fake_surface();
}

EGLSurface nx_eglCreateWindowSurface_stub(EGLDisplay display, EGLConfig config,
                                          EGLNativeWindowType window,
                                          const EGLint *attributes) {
  (void)window;
  return nx_eglCreatePbufferSurface_stub(display, config, attributes);
}

EGLBoolean nx_eglDestroyContext_stub(EGLDisplay display, EGLContext context) {
  if (!valid_display(display) ||
      (context != fake_context() && context != EGL_NO_CONTEXT))
    return egl_failure(valid_display(display) ? EGL_BAD_CONTEXT
                                              : EGL_BAD_DISPLAY);
  if (g_cri_gl.current_context == context) {
    g_cri_gl.current_context = EGL_NO_CONTEXT;
    g_cri_gl.current_draw = EGL_NO_SURFACE;
    g_cri_gl.current_read = EGL_NO_SURFACE;
  }
  return EGL_TRUE;
}

EGLBoolean nx_eglDestroySurface_stub(EGLDisplay display, EGLSurface surface) {
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (surface != fake_surface() && surface != EGL_NO_SURFACE)
    return egl_failure(EGL_BAD_SURFACE);
  return EGL_TRUE;
}

EGLBoolean nx_eglMakeCurrent_stub(EGLDisplay display, EGLSurface draw,
                                  EGLSurface read, EGLContext context) {
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (context == EGL_NO_CONTEXT) {
    if (draw != EGL_NO_SURFACE || read != EGL_NO_SURFACE)
      return egl_failure(EGL_BAD_MATCH);
  } else {
    const int window_pair = draw == fake_surface() && read == fake_surface();
    const int surfaceless_pair = draw == EGL_NO_SURFACE &&
                                 read == EGL_NO_SURFACE;
    if (context != fake_context() || (!window_pair && !surfaceless_pair))
      return egl_failure(EGL_BAD_MATCH);
  }
  g_cri_gl.current_context = context;
  g_cri_gl.current_draw = draw;
  g_cri_gl.current_read = read;
  return EGL_TRUE;
}

EGLContext nx_eglGetCurrentContext_stub(void) {
  return g_cri_gl.current_context;
}

EGLSurface nx_eglGetCurrentSurface_stub(EGLint which) {
  if (which == EGL_DRAW) return g_cri_gl.current_draw;
  if (which == EGL_READ) return g_cri_gl.current_read;
  set_egl_error(EGL_BAD_PARAMETER);
  return EGL_NO_SURFACE;
}

EGLint nx_eglGetError_stub(void) {
  const EGLint result = g_cri_gl.egl_error
    ? g_cri_gl.egl_error : EGL_SUCCESS;
  g_cri_gl.egl_error = EGL_SUCCESS;
  return result;
}

const char *nx_eglQueryString_stub(EGLDisplay display, EGLint name) {
  if (!valid_display(display)) {
    set_egl_error(EGL_BAD_DISPLAY);
    return NULL;
  }
  switch (name) {
    case EGL_VENDOR: return "Mesa/NVK Nintendo Switch compatibility";
    case EGL_VERSION: return "1.4 Genshin-NX";
    case EGL_EXTENSIONS:
      return "EGL_KHR_create_context EGL_KHR_surfaceless_context "
             "EGL_ANDROID_recordable";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    default:
      set_egl_error(EGL_BAD_PARAMETER);
      return NULL;
  }
}

EGLBoolean nx_eglQuerySurface_stub(EGLDisplay display, EGLSurface surface,
                                   EGLint attribute, EGLint *value) {
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (surface != fake_surface()) return egl_failure(EGL_BAD_SURFACE);
  if (!value) return egl_failure(EGL_BAD_PARAMETER);
  if (attribute == EGL_WIDTH) *value = screen_width > 0 ? screen_width : 1280;
  else if (attribute == EGL_HEIGHT)
    *value = screen_height > 0 ? screen_height : 720;
  else if (attribute == EGL_CONFIG_ID) *value = 1;
  else return egl_failure(EGL_BAD_ATTRIBUTE);
  return EGL_TRUE;
}

EGLBoolean nx_eglSurfaceAttrib_stub(EGLDisplay display, EGLSurface surface,
                                    EGLint attribute, EGLint value) {
  (void)attribute;
  (void)value;
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (surface != fake_surface()) return egl_failure(EGL_BAD_SURFACE);
  return EGL_TRUE;
}

EGLBoolean nx_eglSwapBuffers_stub(EGLDisplay display, EGLSurface surface) {
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (surface != fake_surface()) return egl_failure(EGL_BAD_SURFACE);
  return EGL_TRUE;
}

EGLBoolean nx_eglSwapInterval_stub(EGLDisplay display, EGLint interval) {
  if (!valid_display(display)) return egl_failure(EGL_BAD_DISPLAY);
  if (interval < 0 || interval > 1) return egl_failure(EGL_BAD_PARAMETER);
  return EGL_TRUE;
}

void nx_glActiveTexture_stub(GLenum texture) { (void)texture; }

void nx_glBindBuffer_stub(GLenum target, GLuint buffer) {
  (void)target;
  (void)buffer;
}

void nx_glBindTexture_stub(GLenum target, GLuint texture) {
  (void)target;
  (void)texture;
}

void nx_glBufferData_stub(GLenum target, GLsizeiptr size, const void *data,
                          GLenum usage) {
  (void)target;
  (void)data;
  (void)usage;
  if (size < 0) {
    set_gl_error(GL_INVALID_VALUE);
    g_cri_gl.buffer_size = 0;
    return;
  }
  g_cri_gl.buffer_size = (size_t)size;
}

void nx_glDeleteBuffers_stub(GLsizei count, const GLuint *buffers) {
  (void)buffers;
  if (count < 0) set_gl_error(GL_INVALID_VALUE);
}

void nx_glDeleteTextures_stub(GLsizei count, const GLuint *textures) {
  (void)textures;
  if (count < 0) set_gl_error(GL_INVALID_VALUE);
}

void nx_glGenBuffers_stub(GLsizei count, GLuint *buffers) {
  generate_gl_names(count, buffers);
}

void nx_glGenTextures_stub(GLsizei count, GLuint *textures) {
  generate_gl_names(count, textures);
}

GLenum nx_glGetError_stub(void) {
  const GLenum result = g_cri_gl.error;
  g_cri_gl.error = GL_NO_ERROR;
  return result;
}

/* Unity resolves every GLES2 core entry before selecting its actual renderer.
 * In VULKAN_ONLY builds these functions are an initialization contract, not a
 * second renderer.  Pure state/draw commands share an ABI-safe AArch64 no-op;
 * queries and object-producing calls below retain their exact signatures so
 * the capability loader receives coherent results. */
static uintptr_t nx_gl_noop_stub(void) {
  return 0;
}

static GLenum nx_glCheckFramebufferStatus_stub(GLenum target) {
  (void)target;
  return GL_FRAMEBUFFER_COMPLETE;
}

static GLuint nx_glCreateProgram_stub(void) {
  return allocate_gl_name();
}

static GLuint nx_glCreateShader_stub(GLenum shader_type) {
  if (shader_type != GL_VERTEX_SHADER && shader_type != GL_FRAGMENT_SHADER) {
    set_gl_error(GL_INVALID_ENUM);
    return 0;
  }
  return allocate_gl_name();
}

static void nx_glGenObjects_stub(GLsizei count, GLuint *objects) {
  generate_gl_names(count, objects);
}

static GLint nx_glGetAttribLocation_stub(GLuint program, const GLchar *name) {
  (void)program;
  return name ? 0 : -1;
}

static GLint nx_glGetUniformLocation_stub(GLuint program, const GLchar *name) {
  (void)program;
  return name ? 0 : -1;
}

static void nx_glGetActive_stub(GLuint program, GLuint index,
                                GLsizei buffer_size, GLsizei *length,
                                GLint *size, GLenum *type, GLchar *name) {
  (void)program;
  (void)index;
  if (length) *length = 0;
  if (size) *size = 0;
  if (type) *type = GL_FLOAT;
  if (name && buffer_size > 0) name[0] = 0;
}

static void nx_glGetAttachedShaders_stub(GLuint program, GLsizei max_count,
                                         GLsizei *count, GLuint *shaders) {
  (void)program;
  (void)max_count;
  (void)shaders;
  if (count) *count = 0;
}

static void nx_glGetBooleanv_stub(GLenum pname, GLboolean *data) {
  (void)pname;
  if (data) *data = GL_FALSE;
}

static void nx_glGetBufferParameteriv_stub(GLenum target, GLenum pname,
                                           GLint *params) {
  (void)target;
  if (!params) return;
  if (pname == GL_BUFFER_SIZE) *params = (GLint)g_cri_gl.buffer_size;
  else if (pname == GL_BUFFER_USAGE) *params = GL_STATIC_DRAW;
  else *params = 0;
}

static void nx_glGetFloatv_stub(GLenum pname, GLfloat *data) {
  if (!data) return;
  if (pname == GL_ALIASED_LINE_WIDTH_RANGE) {
    data[0] = 1.0f;
    data[1] = 1.0f;
  } else if (pname == GL_ALIASED_POINT_SIZE_RANGE) {
    data[0] = 1.0f;
    data[1] = 64.0f;
  } else {
    data[0] = 0.0f;
  }
}

static void nx_glGetFramebufferAttachmentParameteriv_stub(
  GLenum target, GLenum attachment, GLenum pname, GLint *params) {
  (void)target;
  (void)attachment;
  (void)pname;
  if (params) *params = GL_NONE;
}

static void nx_glGetProgramiv_stub(GLuint program, GLenum pname,
                                   GLint *params) {
  (void)program;
  if (!params) return;
  switch (pname) {
    case GL_LINK_STATUS:
    case GL_VALIDATE_STATUS: *params = GL_TRUE; break;
    case GL_DELETE_STATUS:
    case GL_INFO_LOG_LENGTH:
    case GL_ATTACHED_SHADERS:
    case GL_ACTIVE_ATTRIBUTES:
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
    case GL_ACTIVE_UNIFORMS:
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: *params = 0; break;
    default: *params = 0; break;
  }
}

static void nx_glGetShaderiv_stub(GLuint shader, GLenum pname, GLint *params) {
  (void)shader;
  if (!params) return;
  switch (pname) {
    case GL_COMPILE_STATUS: *params = GL_TRUE; break;
    case GL_DELETE_STATUS:
    case GL_INFO_LOG_LENGTH:
    case GL_SHADER_SOURCE_LENGTH: *params = 0; break;
    default: *params = 0; break;
  }
}

static void nx_glGetInfoLog_stub(GLuint object, GLsizei buffer_size,
                                 GLsizei *length, GLchar *info_log) {
  (void)object;
  if (length) *length = 0;
  if (info_log && buffer_size > 0) info_log[0] = 0;
}

static void nx_glGetRenderbufferParameteriv_stub(GLenum target, GLenum pname,
                                                 GLint *params) {
  (void)target;
  (void)pname;
  if (params) *params = 0;
}

static void nx_glGetShaderSource_stub(GLuint shader, GLsizei buffer_size,
                                      GLsizei *length, GLchar *source) {
  (void)shader;
  if (length) *length = 0;
  if (source && buffer_size > 0) source[0] = 0;
}

static void nx_glGetTexParameterfv_stub(GLenum target, GLenum pname,
                                        GLfloat *params) {
  (void)target;
  (void)pname;
  if (params) *params = 0.0f;
}

static void nx_glGetTexParameteriv_stub(GLenum target, GLenum pname,
                                        GLint *params) {
  (void)target;
  (void)pname;
  if (params) *params = 0;
}

static void nx_glGetUniformfv_stub(GLuint program, GLint location,
                                   GLfloat *params) {
  (void)program;
  (void)location;
  if (params) *params = 0.0f;
}

static void nx_glGetUniformiv_stub(GLuint program, GLint location,
                                   GLint *params) {
  (void)program;
  (void)location;
  if (params) *params = 0;
}

static void nx_glGetVertexAttribfv_stub(GLuint index, GLenum pname,
                                        GLfloat *params) {
  (void)index;
  (void)pname;
  if (params) *params = 0.0f;
}

static void nx_glGetVertexAttribiv_stub(GLuint index, GLenum pname,
                                        GLint *params) {
  (void)index;
  (void)pname;
  if (params) *params = 0;
}

static void nx_glGetVertexAttribPointerv_stub(GLuint index, GLenum pname,
                                              void **pointer) {
  (void)index;
  (void)pname;
  if (pointer) *pointer = NULL;
}

static GLboolean nx_glIsObject_stub(GLuint object) {
  return object ? GL_TRUE : GL_FALSE;
}

static GLboolean nx_glIsEnabled_stub(GLenum capability) {
  (void)capability;
  return GL_FALSE;
}

static const GLubyte *nx_glGetStringi_stub(GLenum name, GLuint index) {
  static const GLubyte empty[] = "";
  (void)name;
  (void)index;
  return empty;
}

void nx_glGetIntegerv_stub(GLenum pname, GLint *data) {
  if (!data) return;
  switch (pname) {
    case 0x821b: *data = 2; break; /* GL_MAJOR_VERSION */
    case 0x821c: *data = 0; break; /* GL_MINOR_VERSION */
    case 0x821d: *data = 0; break; /* GL_NUM_EXTENSIONS */
    case GL_MAX_TEXTURE_SIZE:
    case GL_MAX_RENDERBUFFER_SIZE:
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *data = 4096; break;
    case GL_MAX_TEXTURE_IMAGE_UNITS:
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
    case GL_MAX_VERTEX_ATTRIBS: *data = 8; break;
    case GL_MAX_VERTEX_UNIFORM_VECTORS: *data = 128; break;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS: *data = 64; break;
    case GL_MAX_VARYING_VECTORS: *data = 8; break;
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: *data = 0; break;
    case GL_MAX_VIEWPORT_DIMS:
      data[0] = screen_width > 0 ? screen_width : 1920;
      data[1] = screen_height > 0 ? screen_height : 1080;
      break;
    default: *data = 0; break;
  }
}

void nx_glGetShaderPrecisionFormat_stub(GLenum shader_type,
                                        GLenum precision_type,
                                        GLint *range, GLint *precision) {
  if ((shader_type != GL_VERTEX_SHADER && shader_type != GL_FRAGMENT_SHADER) ||
      !range || !precision) {
    set_gl_error(!range || !precision ? GL_INVALID_VALUE : GL_INVALID_ENUM);
    return;
  }
  switch (precision_type) {
    case GL_LOW_FLOAT:
      range[0] = 7; range[1] = 7; *precision = 8;
      break;
    case GL_MEDIUM_FLOAT:
      range[0] = 15; range[1] = 15; *precision = 10;
      break;
    case GL_HIGH_FLOAT:
      range[0] = 127; range[1] = 127; *precision = 23;
      break;
    case GL_LOW_INT:
    case GL_MEDIUM_INT:
    case GL_HIGH_INT:
      range[0] = 31; range[1] = 30; *precision = 0;
      break;
    default:
      range[0] = 0; range[1] = 0; *precision = 0;
      set_gl_error(GL_INVALID_ENUM);
      break;
  }
}

const GLubyte *nx_glGetString_stub(GLenum name) {
  static const GLubyte vendor[] = "Mesa";
  static const GLubyte renderer[] =
    "NVK Nintendo Switch (Vulkan-only CRI probe)";
  static const GLubyte version[] = "OpenGL ES 2.0 Vulkan-only compatibility";
  static const GLubyte shading[] = "OpenGL ES GLSL ES 1.00";
  static const GLubyte extensions[] = "";
  switch (name) {
    case GL_VENDOR: return vendor;
    case GL_RENDERER: return renderer;
    case GL_VERSION: return version;
    case GL_SHADING_LANGUAGE_VERSION: return shading;
    case GL_EXTENSIONS: return extensions;
    default: return extensions;
  }
}

void nx_glTexImage2D_stub(GLenum target, GLint level, GLint internal_format,
                          GLsizei width, GLsizei height, GLint border,
                          GLenum format, GLenum type, const void *pixels) {
  (void)target; (void)level; (void)internal_format; (void)format;
  (void)type; (void)pixels;
  if (width < 0 || height < 0 || border != 0) set_gl_error(GL_INVALID_VALUE);
}

void nx_glCompressedTexImage2D_stub(GLenum target, GLint level,
                                    GLenum internal_format, GLsizei width,
                                    GLsizei height, GLint border,
                                    GLsizei image_size, const void *data) {
  (void)target;
  (void)level;
  (void)internal_format;
  (void)data;
  if (width < 0 || height < 0 || border != 0 || image_size < 0)
    set_gl_error(GL_INVALID_VALUE);
}

void nx_glTexImage2DMultisample_stub(GLenum target, GLsizei samples,
                                     GLint internal_format, GLsizei width,
                                     GLsizei height,
                                     GLboolean fixed_sample_locations) {
  (void)target;
  (void)internal_format;
  (void)fixed_sample_locations;
  if (samples < 0 || width < 0 || height < 0) set_gl_error(GL_INVALID_VALUE);
}

void nx_glTexStorage2DMultisample_stub(GLenum target, GLsizei samples,
                                       GLenum internal_format, GLsizei width,
                                       GLsizei height,
                                       GLboolean fixed_sample_locations) {
  (void)target;
  (void)internal_format;
  (void)fixed_sample_locations;
  if (samples < 0 || width < 0 || height < 0) set_gl_error(GL_INVALID_VALUE);
}

void nx_glTexParameterf_stub(GLenum target, GLenum pname, GLfloat param) {
  (void)target; (void)pname; (void)param;
}

void nx_glTexParameteri_stub(GLenum target, GLenum pname, GLint param) {
  (void)target; (void)pname; (void)param;
}

void nx_glTexSubImage2D_stub(GLenum target, GLint level, GLint x_offset,
                             GLint y_offset, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, const void *pixels) {
  (void)target; (void)level; (void)x_offset; (void)y_offset; (void)format;
  (void)type; (void)pixels;
  if (width < 0 || height < 0) set_gl_error(GL_INVALID_VALUE);
}

void *nx_glMapBufferOES_stub(GLenum target, GLenum access) {
  (void)target;
  (void)access;
  return map_scratch(g_cri_gl.buffer_size);
}

GLboolean nx_glUnmapBufferOES_stub(GLenum target) {
  (void)target;
  const GLboolean result = g_cri_gl.mapped ? GL_TRUE : GL_FALSE;
  if (!g_cri_gl.mapped) set_gl_error(GL_INVALID_OPERATION);
  g_cri_gl.mapped = 0;
  return result;
}

void *nx_glMapBufferRange_stub(GLenum target, GLintptr offset,
                               GLsizeiptr length, GLbitfield access) {
  (void)target;
  (void)access;
  if (offset < 0 || length <= 0 || (size_t)offset > SIZE_MAX - (size_t)length) {
    set_gl_error(GL_INVALID_VALUE);
    return NULL;
  }
  const size_t end = (size_t)offset + (size_t)length;
  if (g_cri_gl.buffer_size && end > g_cri_gl.buffer_size) {
    set_gl_error(GL_INVALID_VALUE);
    return NULL;
  }
  void *base = map_scratch(g_cri_gl.buffer_size > end
                             ? g_cri_gl.buffer_size : end);
  return base ? (uint8_t *)base + (size_t)offset : NULL;
}

GLboolean nx_glUnmapBuffer_stub(GLenum target) {
  return nx_glUnmapBufferOES_stub(target);
}

void *nx_egl_gles_compat_lookup(const char *name) {
  if (!name) return NULL;
#define MATCH(symbol) if (!strcmp(name, #symbol)) return (void *)&nx_##symbol##_stub
#define MATCH_TO(symbol, target) if (!strcmp(name, #symbol)) return (void *)&target
#define MATCH_NOOP(symbol) MATCH_TO(symbol, nx_gl_noop_stub)
  MATCH(eglGetDisplay);
  MATCH(eglInitialize);
  MATCH(eglTerminate);
  MATCH(eglChooseConfig);
  MATCH(eglGetConfigAttrib);
  MATCH(eglCreateContext);
  MATCH(eglCreatePbufferSurface);
  MATCH(eglCreateWindowSurface);
  MATCH(eglDestroyContext);
  MATCH(eglDestroySurface);
  MATCH(eglMakeCurrent);
  MATCH(eglGetCurrentContext);
  MATCH(eglGetCurrentSurface);
  MATCH(eglGetError);
  MATCH(eglQueryString);
  MATCH(eglQuerySurface);
  MATCH(eglSurfaceAttrib);
  MATCH(eglSwapBuffers);
  MATCH(eglSwapInterval);
  MATCH(eglGetProcAddress);
  MATCH(glActiveTexture);
  MATCH_NOOP(glAttachShader);
  MATCH_NOOP(glBindAttribLocation);
  MATCH(glBindBuffer);
  MATCH_NOOP(glBindFramebuffer);
  MATCH_NOOP(glBindRenderbuffer);
  MATCH(glBindTexture);
  MATCH_NOOP(glBlendColor);
  MATCH_NOOP(glBlendEquation);
  MATCH_NOOP(glBlendEquationSeparate);
  MATCH_NOOP(glBlendFunc);
  MATCH_NOOP(glBlendFuncSeparate);
  MATCH(glBufferData);
  MATCH_NOOP(glBufferSubData);
  MATCH(glCheckFramebufferStatus);
  MATCH_NOOP(glClear);
  MATCH_NOOP(glClearColor);
  MATCH_NOOP(glClearDepthf);
  MATCH_NOOP(glClearStencil);
  MATCH_NOOP(glColorMask);
  MATCH_NOOP(glCompileShader);
  MATCH_NOOP(glCompressedTexSubImage2D);
  MATCH_NOOP(glCopyTexImage2D);
  MATCH_NOOP(glCopyTexSubImage2D);
  MATCH(glCreateProgram);
  MATCH(glCreateShader);
  MATCH_NOOP(glCullFace);
  MATCH(glDeleteBuffers);
  MATCH_TO(glDeleteFramebuffers, nx_glDeleteBuffers_stub);
  MATCH_NOOP(glDeleteProgram);
  MATCH_TO(glDeleteRenderbuffers, nx_glDeleteBuffers_stub);
  MATCH_NOOP(glDeleteShader);
  MATCH(glDeleteTextures);
  MATCH_NOOP(glDepthFunc);
  MATCH_NOOP(glDepthMask);
  MATCH_NOOP(glDepthRangef);
  MATCH_NOOP(glDetachShader);
  MATCH_NOOP(glDisable);
  MATCH_NOOP(glDisableVertexAttribArray);
  MATCH_NOOP(glDrawArrays);
  MATCH_NOOP(glDrawElements);
  MATCH_NOOP(glEnable);
  MATCH_NOOP(glEnableVertexAttribArray);
  MATCH_NOOP(glFinish);
  MATCH_NOOP(glFlush);
  MATCH_NOOP(glFramebufferRenderbuffer);
  MATCH_NOOP(glFramebufferTexture2D);
  MATCH_NOOP(glFrontFace);
  MATCH(glGenBuffers);
  MATCH_TO(glGenerateMipmap, nx_gl_noop_stub);
  MATCH_TO(glGenFramebuffers, nx_glGenObjects_stub);
  MATCH_TO(glGenRenderbuffers, nx_glGenObjects_stub);
  MATCH(glGenTextures);
  MATCH_TO(glGetActiveAttrib, nx_glGetActive_stub);
  MATCH_TO(glGetActiveUniform, nx_glGetActive_stub);
  MATCH(glGetAttachedShaders);
  MATCH(glGetAttribLocation);
  MATCH(glGetBooleanv);
  MATCH(glGetBufferParameteriv);
  MATCH(glGetError);
  MATCH(glGetFloatv);
  MATCH(glGetFramebufferAttachmentParameteriv);
  MATCH(glGetIntegerv);
  MATCH_TO(glGetProgramInfoLog, nx_glGetInfoLog_stub);
  MATCH(glGetProgramiv);
  MATCH(glGetRenderbufferParameteriv);
  MATCH_TO(glGetShaderInfoLog, nx_glGetInfoLog_stub);
  MATCH(glGetShaderPrecisionFormat);
  MATCH(glGetShaderiv);
  MATCH(glGetShaderSource);
  MATCH(glGetString);
  MATCH(glGetStringi);
  MATCH(glGetTexParameterfv);
  MATCH(glGetTexParameteriv);
  MATCH(glGetUniformfv);
  MATCH(glGetUniformiv);
  MATCH(glGetUniformLocation);
  MATCH(glGetVertexAttribfv);
  MATCH(glGetVertexAttribiv);
  MATCH(glGetVertexAttribPointerv);
  MATCH_NOOP(glHint);
  MATCH_TO(glIsBuffer, nx_glIsObject_stub);
  MATCH(glIsEnabled);
  MATCH_TO(glIsFramebuffer, nx_glIsObject_stub);
  MATCH_TO(glIsProgram, nx_glIsObject_stub);
  MATCH_TO(glIsRenderbuffer, nx_glIsObject_stub);
  MATCH_TO(glIsShader, nx_glIsObject_stub);
  MATCH_TO(glIsTexture, nx_glIsObject_stub);
  MATCH_NOOP(glLineWidth);
  MATCH_NOOP(glLinkProgram);
  MATCH_NOOP(glPixelStorei);
  MATCH_NOOP(glPolygonOffset);
  MATCH_NOOP(glReadPixels);
  MATCH_NOOP(glReleaseShaderCompiler);
  MATCH_NOOP(glRenderbufferStorage);
  MATCH_NOOP(glSampleCoverage);
  MATCH_NOOP(glScissor);
  MATCH_NOOP(glShaderBinary);
  MATCH_NOOP(glShaderSource);
  MATCH_NOOP(glStencilFunc);
  MATCH_NOOP(glStencilFuncSeparate);
  MATCH_NOOP(glStencilMask);
  MATCH_NOOP(glStencilMaskSeparate);
  MATCH_NOOP(glStencilOp);
  MATCH_NOOP(glStencilOpSeparate);
  MATCH(glCompressedTexImage2D);
  MATCH(glTexImage2D);
  MATCH(glTexImage2DMultisample);
  MATCH(glTexStorage2DMultisample);
  MATCH(glTexParameterf);
  MATCH(glTexParameteri);
  MATCH_NOOP(glTexParameterfv);
  MATCH_NOOP(glTexParameteriv);
  MATCH(glTexSubImage2D);
  MATCH_NOOP(glUniform1f);
  MATCH_NOOP(glUniform1fv);
  MATCH_NOOP(glUniform1i);
  MATCH_NOOP(glUniform1iv);
  MATCH_NOOP(glUniform2f);
  MATCH_NOOP(glUniform2fv);
  MATCH_NOOP(glUniform2i);
  MATCH_NOOP(glUniform2iv);
  MATCH_NOOP(glUniform3f);
  MATCH_NOOP(glUniform3fv);
  MATCH_NOOP(glUniform3i);
  MATCH_NOOP(glUniform3iv);
  MATCH_NOOP(glUniform4f);
  MATCH_NOOP(glUniform4fv);
  MATCH_NOOP(glUniform4i);
  MATCH_NOOP(glUniform4iv);
  MATCH_NOOP(glUniformMatrix2fv);
  MATCH_NOOP(glUniformMatrix3fv);
  MATCH_NOOP(glUniformMatrix4fv);
  MATCH_NOOP(glUseProgram);
  MATCH_NOOP(glValidateProgram);
  MATCH_NOOP(glVertexAttrib1f);
  MATCH_NOOP(glVertexAttrib1fv);
  MATCH_NOOP(glVertexAttrib2f);
  MATCH_NOOP(glVertexAttrib2fv);
  MATCH_NOOP(glVertexAttrib3f);
  MATCH_NOOP(glVertexAttrib3fv);
  MATCH_NOOP(glVertexAttrib4f);
  MATCH_NOOP(glVertexAttrib4fv);
  MATCH_NOOP(glVertexAttribPointer);
  MATCH_NOOP(glViewport);
  MATCH(glMapBufferOES);
  MATCH(glUnmapBufferOES);
  MATCH(glMapBufferRange);
  MATCH(glUnmapBuffer);
#undef MATCH
#undef MATCH_TO
#undef MATCH_NOOP
  return NULL;
}

__eglMustCastToProperFunctionPointerType nx_eglGetProcAddress_stub(
  const char *name) {
  void *result = nx_egl_gles_compat_lookup(name);
  if (!result && name) {
    const uint32_t index = __atomic_fetch_add(&g_missing_proc_trace_count, 1u,
                                               __ATOMIC_RELAXED);
    if (index < 32u) {
      char message[192];
      snprintf(message, sizeof message,
               "unresolved eglGetProcAddress request: %s", name);
    }
  }
  return (__eglMustCastToProperFunctionPointerType)result;
}

/* Weak standard query names cover Unity's direct imports. The additional
 * strong aliases below are external-backend-only: they satisfy SDL2's static
 * EGL loader without linking the incompatible switch-mesa libEGL. CRI
 * relocations continue to bind the explicit nx_* functions above. */
#ifdef GENSHIN_EXTERNAL_LOADERLESS_NVK
EGLDisplay eglGetDisplay(EGLNativeDisplayType display) {
  return nx_eglGetDisplay_stub(display);
}

EGLDisplay eglGetPlatformDisplay(
    EGLenum platform, void *native_display, const EGLAttrib *attributes) {
  (void)platform;
  (void)attributes;
  return nx_eglGetDisplay_stub((EGLNativeDisplayType)native_display);
}

EGLBoolean eglInitialize(
    EGLDisplay display, EGLint *major, EGLint *minor) {
  return nx_eglInitialize_stub(display, major, minor);
}

EGLBoolean eglTerminate(EGLDisplay display) {
  return nx_eglTerminate_stub(display);
}

EGLBoolean eglChooseConfig(
    EGLDisplay display, const EGLint *attributes, EGLConfig *configs,
    EGLint config_size, EGLint *config_count) {
  return nx_eglChooseConfig_stub(display, attributes, configs, config_size,
                                 config_count);
}

EGLBoolean eglGetConfigAttrib(
    EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value) {
  return nx_eglGetConfigAttrib_stub(display, config, attribute, value);
}

EGLContext eglCreateContext(
    EGLDisplay display, EGLConfig config, EGLContext shared_context,
    const EGLint *attributes) {
  return nx_eglCreateContext_stub(display, config, shared_context, attributes);
}

EGLSurface eglCreatePbufferSurface(
    EGLDisplay display, EGLConfig config, const EGLint *attributes) {
  return nx_eglCreatePbufferSurface_stub(display, config, attributes);
}

EGLSurface eglCreateWindowSurface(
    EGLDisplay display, EGLConfig config, EGLNativeWindowType window,
    const EGLint *attributes) {
  return nx_eglCreateWindowSurface_stub(display, config, window, attributes);
}

EGLBoolean eglDestroyContext(
    EGLDisplay display, EGLContext context) {
  return nx_eglDestroyContext_stub(display, context);
}

EGLBoolean eglDestroySurface(
    EGLDisplay display, EGLSurface surface) {
  return nx_eglDestroySurface_stub(display, surface);
}

EGLBoolean eglMakeCurrent(
    EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) {
  return nx_eglMakeCurrent_stub(display, draw, read, context);
}

__attribute__((weak)) EGLContext eglGetCurrentContext(void) {
  return nx_eglGetCurrentContext_stub();
}

__attribute__((weak)) EGLSurface eglGetCurrentSurface(EGLint which) {
  return nx_eglGetCurrentSurface_stub(which);
}

__attribute__((weak)) EGLBoolean eglQuerySurface(
  EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint *value) {
  return nx_eglQuerySurface_stub(display, surface, attribute, value);
}

__attribute__((weak)) EGLBoolean eglSurfaceAttrib(
  EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint value) {
  return nx_eglSurfaceAttrib_stub(display, surface, attribute, value);
}

EGLBoolean eglSwapBuffers(
    EGLDisplay display, EGLSurface surface) {
  return nx_eglSwapBuffers_stub(display, surface);
}

EGLBoolean eglSwapInterval(
    EGLDisplay display, EGLint interval) {
  return nx_eglSwapInterval_stub(display, interval);
}

EGLint eglGetError(void) {
  return nx_eglGetError_stub();
}

const char *eglQueryString(
    EGLDisplay display, EGLint name) {
  return nx_eglQueryString_stub(display, name);
}

__eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char *name) {
  return nx_eglGetProcAddress_stub(name);
}

EGLBoolean eglBindAPI(EGLenum api) {
  return api == EGL_OPENGL_ES_API ? EGL_TRUE : egl_failure(EGL_BAD_PARAMETER);
}

EGLenum eglQueryAPI(void) {
  return EGL_OPENGL_ES_API;
}

EGLBoolean eglWaitNative(EGLint engine) {
  (void)engine;
  return EGL_TRUE;
}

EGLBoolean eglWaitGL(void) {
  return EGL_TRUE;
}
#endif

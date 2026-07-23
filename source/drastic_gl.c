#ifndef USE_VULKAN

#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "drastic_dfx.h"
#include "drastic_dfx_gl_generated.h"
#include "drastic_renderer.h"
#include "drastic_smaa_area_rgb_bin.h"
#include "drastic_smaa_search_rgb_bin.h"

#define SCREEN_TEXTURE_COUNT 2
#define WORK_TEXTURE_COUNT 3
#define WORK_A 0
#define WORK_B 1
#define WORK_2X 2

typedef struct {
  float x, y;
  float u, v;
} Vertex;

typedef struct {
  GLuint id;
  GLint position;
  GLint texcoord;
  GLint texture_size;
  GLint target_size;
  GLint time;
  GLint samplers[3];
} GlProgram;

typedef struct {
  GLuint texture;
  GLuint framebuffer;
  int width;
  int height;
} GlTarget;

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;
static GlProgram g_programs[DRASTIC_DFX_SHADER_COUNT];
static GlProgram g_overlay_program;
static GLuint g_vbo;
static GLuint g_textures[SCREEN_TEXTURE_COUNT];
static GlTarget g_work[SCREEN_TEXTURE_COUNT][WORK_TEXTURE_COUNT];
static GLuint g_area_texture;
static GLuint g_search_texture;
static GLuint g_overlay_texture;
static uint32_t *g_overlay_rgba;
static uint64_t g_overlay_generation = UINT64_MAX;
static int g_texture_width;
static int g_texture_height;
static DrasticVideoFilter g_filtered_filter = DRASTIC_FILTER_COUNT;
static int g_filter_valid[2];
static unsigned g_frames;

static const char *const g_vertex_sources[DRASTIC_DFX_SHADER_COUNT] = {
  dfx_copy_vertex_source,
  dfx_quilez_vertex_source,
  dfx_scanline_vertex_source,
  dfx_scale2x_vertex_source,
  dfx_hq2x_vertex_source,
  dfx_fxaa_vertex_source,
  dfx_fxaa_luma_vertex_source,
  dfx_fxaa_hq_vertex_source,
  dfx_smaa_edge_vertex_source,
  dfx_smaa_weight_vertex_source,
  dfx_smaa_blend_vertex_source,
};

static const char *const g_fragment_sources[DRASTIC_DFX_SHADER_COUNT] = {
  dfx_copy_fragment_source,
  dfx_quilez_fragment_source,
  dfx_scanline_fragment_source,
  dfx_scale2x_fragment_source,
  dfx_hq2x_fragment_source,
  dfx_fxaa_fragment_source,
  dfx_fxaa_luma_fragment_source,
  dfx_fxaa_hq_fragment_source,
  dfx_smaa_edge_fragment_source,
  dfx_smaa_weight_fragment_source,
  dfx_smaa_blend_fragment_source,
};

static const char *const g_sampler_names[DRASTIC_DFX_SHADER_COUNT][3] = {
  [DRASTIC_DFX_COPY] = {"u_texture"},
  [DRASTIC_DFX_QUILEZ] = {"u_texture"},
  [DRASTIC_DFX_SCANLINE] = {"u_texture"},
  [DRASTIC_DFX_SCALE2X] = {"u_texture"},
  [DRASTIC_DFX_HQ2X] = {"u_texture"},
  [DRASTIC_DFX_FXAA] = {"u_texture"},
  [DRASTIC_DFX_FXAA_LUMA] = {"u_texture"},
  [DRASTIC_DFX_FXAA_HQ] = {"u_texture"},
  [DRASTIC_DFX_SMAA_EDGE] = {"u_texture"},
  [DRASTIC_DFX_SMAA_WEIGHT] = {
    "u_texture_edges", "u_texture_area", "u_texture_search"
  },
  [DRASTIC_DFX_SMAA_BLEND] = {"u_texture", "u_texture_blend"},
};

static const char overlay_vertex_source[] =
  "attribute vec2 a_vertex_coordinate;\n"
  "attribute vec2 a_texture_coordinate;\n"
  "varying vec2 v_texture_coordinate;\n"
  "void main(){\n"
  " gl_Position=vec4(a_vertex_coordinate,0.0,1.0);\n"
  " v_texture_coordinate=a_texture_coordinate;\n"
  "}\n";

static const char overlay_fragment_source[] =
  "precision mediump float;\n"
  "varying vec2 v_texture_coordinate;\n"
  "uniform sampler2D u_texture;\n"
  "void main(){gl_FragColor=texture2D(u_texture,v_texture_coordinate);}\n";

static GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static int create_program(GlProgram *program, const char *vertex_source,
                          const char *fragment_source,
                          const char *const sampler_names[3]) {
  GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return 0;
  }
  program->id = glCreateProgram();
  glAttachShader(program->id, vertex);
  glAttachShader(program->id, fragment);
  glLinkProgram(program->id);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program->id, GL_LINK_STATUS, &linked);
  if (!linked) return 0;
  program->position = glGetAttribLocation(program->id,
                                           "a_vertex_coordinate");
  program->texcoord = glGetAttribLocation(program->id,
                                           "a_texture_coordinate");
  program->texture_size = glGetUniformLocation(program->id,
                                                "u_texture_size");
  program->target_size = glGetUniformLocation(program->id,
                                               "u_target_size");
  program->time = glGetUniformLocation(program->id, "u_time");
  for (int index = 0; index < 3; index++) {
    program->samplers[index] = sampler_names && sampler_names[index]
        ? glGetUniformLocation(program->id, sampler_names[index]) : -1;
  }
  return program->position >= 0 && program->texcoord >= 0;
}

static int create_programs(void) {
  for (int shader = 0; shader < DRASTIC_DFX_SHADER_COUNT; shader++) {
    if (!create_program(&g_programs[shader], g_vertex_sources[shader],
                        g_fragment_sources[shader],
                        g_sampler_names[shader])) return 0;
  }
  const char *overlay_samplers[3] = {"u_texture", NULL, NULL};
  return create_program(&g_overlay_program, overlay_vertex_source,
                        overlay_fragment_source, overlay_samplers);
}

static void set_texture_filter(GLuint texture, DrasticDfxSampler sampler) {
  const GLint filtering = sampler == DRASTIC_DFX_LINEAR
      ? GL_LINEAR : GL_NEAREST;
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtering);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtering);
}

static void bind_geometry(const GlProgram *program, const Vertex *vertices) {
  glUseProgram(program->id);
  glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 6, vertices,
               GL_STREAM_DRAW);
  glEnableVertexAttribArray((GLuint)program->position);
  glEnableVertexAttribArray((GLuint)program->texcoord);
  glVertexAttribPointer((GLuint)program->position, 2, GL_FLOAT, GL_FALSE,
                        sizeof(Vertex), (const void *)0);
  glVertexAttribPointer((GLuint)program->texcoord, 2, GL_FLOAT, GL_FALSE,
                        sizeof(Vertex),
                        (const void *)(2 * sizeof(float)));
}

static void set_program_parameters(const GlProgram *program,
                                   int texture_width, int texture_height,
                                   int target_width, int target_height) {
  if (program->texture_size >= 0)
    glUniform4f(program->texture_size,
                1.0f / (float)texture_width,
                1.0f / (float)texture_height,
                (float)texture_width, (float)texture_height);
  if (program->target_size >= 0)
    glUniform2f(program->target_size, (float)target_width,
                (float)target_height);
  if (program->time >= 0)
    glUniform1f(program->time, (float)g_frames / 60.0f);
}

static int create_target(GlTarget *target, int width, int height) {
  target->width = width;
  target->height = height;
  glGenTextures(1, &target->texture);
  glBindTexture(GL_TEXTURE_2D, target->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &target->framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, target->texture, 0);
  return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static int create_lookup_texture(GLuint *texture, int width, int height,
                                 const uint8_t *pixels) {
  glGenTextures(1, texture);
  glBindTexture(GL_TEXTURE_2D, *texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
               GL_RGB, GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  return glGetError() == GL_NO_ERROR;
}

static GlTarget *work_target(int screen, DrasticDfxTextureRole role) {
  switch (role) {
    case DRASTIC_DFX_WORK_A: return &g_work[screen][WORK_A];
    case DRASTIC_DFX_WORK_B: return &g_work[screen][WORK_B];
    case DRASTIC_DFX_WORK_2X: return &g_work[screen][WORK_2X];
    default: return NULL;
  }
}

static GLuint texture_for_role(int screen, DrasticDfxTextureRole role) {
  GlTarget *target = work_target(screen, role);
  if (target) return target->texture;
  if (role == DRASTIC_DFX_AREA) return g_area_texture;
  if (role == DRASTIC_DFX_SEARCH) return g_search_texture;
  return g_textures[screen];
}

static void dimensions_for_role(int screen, DrasticDfxTextureRole role,
                                int *width, int *height) {
  (void)screen;
  GlTarget *target = work_target(screen, role);
  if (target) {
    *width = target->width;
    *height = target->height;
  } else if (role == DRASTIC_DFX_AREA) {
    *width = 160; *height = 560;
  } else if (role == DRASTIC_DFX_SEARCH) {
    *width = 64; *height = 16;
  } else {
    *width = g_texture_width; *height = g_texture_height;
  }
}

static int render_pass(int screen, const DrasticDfxPass *pass) {
  GlTarget *output = work_target(screen, pass->output);
  if (!output || !pass->input_count) return 0;
  int input_width, input_height;
  dimensions_for_role(screen, pass->inputs[0].texture,
                      &input_width, &input_height);
  const GlProgram *program = &g_programs[pass->shader];
  /* Preserve Drastic's top-at-v=0 convention when an FBO becomes the next
   * pass's texture: logical top is written into GL's bottom texture row. */
  static const Vertex vertices[6] = {
    {-1.0f, -1.0f, 0.0f, 0.0f}, {-1.0f,  1.0f, 0.0f, 1.0f},
    { 1.0f,  1.0f, 1.0f, 1.0f}, {-1.0f, -1.0f, 0.0f, 0.0f},
    { 1.0f,  1.0f, 1.0f, 1.0f}, { 1.0f, -1.0f, 1.0f, 0.0f},
  };
  glBindFramebuffer(GL_FRAMEBUFFER, output->framebuffer);
  glViewport(0, 0, output->width, output->height);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  bind_geometry(program, vertices);
  set_program_parameters(program, input_width, input_height,
                         output->width, output->height);
  for (int input = 0; input < pass->input_count; input++) {
    const GLuint texture = texture_for_role(screen, pass->inputs[input].texture);
    glActiveTexture((GLenum)(GL_TEXTURE0 + input));
    set_texture_filter(texture, pass->inputs[input].sampler);
    if (program->samplers[input] >= 0)
      glUniform1i(program->samplers[input], input);
  }
  glDrawArrays(GL_TRIANGLES, 0, 6);
  return glGetError() == GL_NO_ERROR;
}

static int render_filter_chain(int screen, const DrasticDfxChain *chain) {
  for (int index = 0; index < chain->pass_count; index++)
    if (!render_pass(screen, &chain->passes[index])) return 0;
  return 1;
}

static void uv_for_rotation(int rotation, float *tl_u, float *tl_v,
                            float *tr_u, float *tr_v,
                            float *bl_u, float *bl_v,
                            float *br_u, float *br_v) {
  static const float corners[4][8] = {
    {0,0, 1,0, 0,1, 1,1}, {0,1, 0,0, 1,1, 1,0},
    {1,1, 0,1, 1,0, 0,0}, {1,0, 1,1, 0,0, 0,1},
  };
  const float *uv = corners[rotation & 3];
  *tl_u=uv[0]; *tl_v=uv[1]; *tr_u=uv[2]; *tr_v=uv[3];
  *bl_u=uv[4]; *bl_v=uv[5]; *br_u=uv[6]; *br_v=uv[7];
}

static void make_rect_vertices(const DrasticScreenRect *rect, int rotation,
                               Vertex vertices[6]) {
  const float left = rect->x * 2.0f / panel_width - 1.0f;
  const float right = (rect->x + rect->width) * 2.0f / panel_width - 1.0f;
  const float top = 1.0f - rect->y * 2.0f / panel_height;
  const float bottom = 1.0f - (rect->y + rect->height) * 2.0f / panel_height;
  float tlu,tlv,tru,trv,blu,blv,bru,brv;
  uv_for_rotation(rotation,&tlu,&tlv,&tru,&trv,&blu,&blv,&bru,&brv);
  vertices[0]=(Vertex){left,top,tlu,tlv};
  vertices[1]=(Vertex){left,bottom,blu,blv};
  vertices[2]=(Vertex){right,bottom,bru,brv};
  vertices[3]=(Vertex){left,top,tlu,tlv};
  vertices[4]=(Vertex){right,bottom,bru,brv};
  vertices[5]=(Vertex){right,top,tru,trv};
}

static void draw_screen(const DrasticScreenRect *rect, int rotation,
                        const DrasticDfxChain *chain) {
  const int screen = rect->screen ? 1 : 0;
  int texture_width, texture_height;
  dimensions_for_role(screen, chain->final_texture,
                      &texture_width, &texture_height);
  Vertex vertices[6];
  make_rect_vertices(rect, rotation, vertices);
  const GlProgram *program = &g_programs[chain->final_shader];
  bind_geometry(program, vertices);
  set_program_parameters(program, texture_width, texture_height,
                         (int)rect->width, (int)rect->height);
  glActiveTexture(GL_TEXTURE0);
  set_texture_filter(texture_for_role(screen, chain->final_texture),
                     chain->final_sampler);
  if (program->samplers[0] >= 0) glUniform1i(program->samplers[0], 0);
  glDrawArrays(GL_TRIANGLES, 0, 6);
}

static int upload_overlay(const DrasticOverlayFrame *overlay) {
  if (!overlay || !overlay->visible || !overlay->pixels) return 1;
  if (overlay->generation == g_overlay_generation) return 1;
  const size_t count = (size_t)overlay->width * overlay->height;
  if (!g_overlay_rgba) {
    g_overlay_rgba = malloc(count * sizeof(*g_overlay_rgba));
    if (!g_overlay_rgba) return 0;
  }
  for (size_t index = 0; index < count; index++) {
    const uint32_t pixel = overlay->pixels[index];
    g_overlay_rgba[index] = (pixel & 0xff00ff00u) |
        ((pixel & 0x00ff0000u) >> 16) | ((pixel & 0x000000ffu) << 16);
  }
  glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, overlay->width, overlay->height,
                  GL_RGBA, GL_UNSIGNED_BYTE, g_overlay_rgba);
  g_overlay_generation = overlay->generation;
  return glGetError() == GL_NO_ERROR;
}

static void draw_overlay(void) {
  static const Vertex vertices[6] = {
    {-1.0f,  1.0f, 0.0f, 0.0f}, {-1.0f, -1.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 1.0f, 1.0f}, {-1.0f,  1.0f, 0.0f, 0.0f},
    { 1.0f, -1.0f, 1.0f, 1.0f}, { 1.0f,  1.0f, 1.0f, 0.0f},
  };
  bind_geometry(&g_overlay_program, vertices);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  if (g_overlay_program.samplers[0] >= 0)
    glUniform1i(g_overlay_program.samplers[0], 0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisable(GL_BLEND);
}

static void clear_cursor_rect(int x, int y, int width, int height) {
  glScissor(x, panel_height - y - height, width, height);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void draw_stylus_cursor(const DrasticRuntimeConfig *config) {
  float panel_x, panel_y;
  if (!config->stylus_visible ||
      !drastic_config_map_stylus(config, config->stylus_x, config->stylus_y,
                                 &panel_x, &panel_y)) return;
  const int x = (int)(panel_x + 0.5f);
  const int y = (int)(panel_y + 0.5f);
  const int radius = panel_width >= 1600 ? 10 : 7;
  glEnable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  clear_cursor_rect(x - radius, y - 2, radius * 2 + 1, 5);
  clear_cursor_rect(x - 2, y - radius, 5, radius * 2 + 1);
  glClearColor(0.25f, 0.95f, 1.0f, 1.0f);
  clear_cursor_rect(x - radius + 1, y - 1, radius * 2 - 1, 3);
  clear_cursor_rect(x - 1, y - radius + 1, 3, radius * 2 - 1);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

bool drastic_renderer_init(const DrasticRuntimeConfig *config) {
  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, NULL, NULL))
    return false;
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;
  const EGLint attributes[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_NONE
  };
  EGLConfig egl_config;
  EGLint count = 0;
  if (!eglChooseConfig(g_display, attributes, &egl_config, 1, &count) ||
      !count) return false;
  g_surface = eglCreateWindowSurface(g_display, egl_config,
                                     (EGLNativeWindowType)nwindowGetDefault(),
                                     NULL);
  if (g_surface == EGL_NO_SURFACE) return false;
  const EGLint context_attributes[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
  };
  g_context = eglCreateContext(g_display, egl_config, EGL_NO_CONTEXT,
                               context_attributes);
  if (g_context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(g_display, g_surface, g_surface, g_context)) return false;
  eglSwapInterval(g_display, 1);
  if (!create_programs()) return false;
  glGenBuffers(1, &g_vbo);

  g_texture_width = (config->core_config & (UINT64_C(1) << 41)) ? 512 : 256;
  g_texture_height = g_texture_width * 3 / 4;
  glGenTextures(SCREEN_TEXTURE_COUNT, g_textures);
  for (int screen = 0; screen < SCREEN_TEXTURE_COUNT; screen++) {
    glBindTexture(GL_TEXTURE_2D, g_textures[screen]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_texture_width,
                 g_texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (!create_target(&g_work[screen][WORK_A], g_texture_width,
                       g_texture_height) ||
        !create_target(&g_work[screen][WORK_B], g_texture_width,
                       g_texture_height) ||
        !create_target(&g_work[screen][WORK_2X], g_texture_width * 2,
                       g_texture_height * 2)) return false;
  }
  if (!create_lookup_texture(&g_area_texture, 160, 560,
                             drastic_smaa_area_rgb_bin) ||
      !create_lookup_texture(&g_search_texture, 64, 16,
                             drastic_smaa_search_rgb_bin)) return false;

  glGenTextures(1, &g_overlay_texture);
  glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DRASTIC_OVERLAY_WIDTH,
               DRASTIC_OVERLAY_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, panel_width, panel_height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  g_filtered_filter = DRASTIC_FILTER_COUNT;
  g_filter_valid[0] = g_filter_valid[1] = 0;
  g_frames = 0;
  return glGetError() == GL_NO_ERROR;
}

void drastic_renderer_present(const DrasticRuntimeConfig *config,
                              DrasticCoreRenderFrame core_render,
                              void *env, void *clazz,
                              const uint32_t *top, const uint32_t *bottom,
                              const DrasticOverlayFrame *overlay,
                              bool consume_core_frame) {
  (void)top;
  (void)bottom;
  if (consume_core_frame) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    core_render(env, clazz, (int)g_textures[0], (int)g_textures[1], 0);
    g_filter_valid[0] = g_filter_valid[1] = 0;
  }

  const DrasticDfxChain *chain = drastic_dfx_chain(config->video_filter);
  if (g_filtered_filter != config->video_filter) {
    g_filtered_filter = config->video_filter;
    g_filter_valid[0] = g_filter_valid[1] = 0;
  }
  if (chain->pass_count) {
    int needed[2] = {0, 0};
    for (int index = 0; index < config->screen_count; index++)
      needed[config->screens[index].screen ? 1 : 0] = 1;
    for (int screen = 0; screen < 2; screen++)
      if (needed[screen] && !g_filter_valid[screen]) {
        if (!render_filter_chain(screen, chain)) return;
        g_filter_valid[screen] = 1;
      }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, panel_width, panel_height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  for (int index = 0; index < config->screen_count; index++)
    draw_screen(&config->screens[index], config->rotation, chain);
  draw_stylus_cursor(config);
  if (overlay && overlay->visible && upload_overlay(overlay)) draw_overlay();
  eglSwapBuffers(g_display, g_surface);
  g_frames++;
}

void drastic_renderer_shutdown(void) {
  if (g_display == EGL_NO_DISPLAY) return;
  if (g_context != EGL_NO_CONTEXT) {
    eglMakeCurrent(g_display, g_surface, g_surface, g_context);
    glDeleteTextures(SCREEN_TEXTURE_COUNT, g_textures);
    for (int screen = 0; screen < SCREEN_TEXTURE_COUNT; screen++) {
      for (int work = 0; work < WORK_TEXTURE_COUNT; work++) {
        if (g_work[screen][work].framebuffer)
          glDeleteFramebuffers(1, &g_work[screen][work].framebuffer);
        if (g_work[screen][work].texture)
          glDeleteTextures(1, &g_work[screen][work].texture);
      }
    }
    if (g_area_texture) glDeleteTextures(1, &g_area_texture);
    if (g_search_texture) glDeleteTextures(1, &g_search_texture);
    if (g_overlay_texture) glDeleteTextures(1, &g_overlay_texture);
    if (g_vbo) glDeleteBuffers(1, &g_vbo);
    for (int shader = 0; shader < DRASTIC_DFX_SHADER_COUNT; shader++)
      if (g_programs[shader].id) glDeleteProgram(g_programs[shader].id);
    if (g_overlay_program.id) glDeleteProgram(g_overlay_program.id);
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_display, g_context);
  }
  if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
  eglTerminate(g_display);
  g_display = EGL_NO_DISPLAY;
  g_surface = EGL_NO_SURFACE;
  g_context = EGL_NO_CONTEXT;
  free(g_overlay_rgba);
  g_overlay_rgba = NULL;
  memset(g_work, 0, sizeof(g_work));
}

unsigned drastic_renderer_frame_count(void) { return g_frames; }

bool drastic_renderer_lsfg_available(void) { return false; }

bool drastic_renderer_lsfg_enabled(void) { return false; }

bool drastic_renderer_lsfg_request_enabled(bool enabled) {
  (void)enabled;
  return false;
}

#endif

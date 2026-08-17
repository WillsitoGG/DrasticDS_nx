#include "drastic_vk_capture.h"

#include <stddef.h>
#include <string.h>

typedef struct {
  uint32_t *screens[2];
  unsigned width;
  unsigned height;
  GLuint texture;
  int active;
} DrasticVkCapture;

/* DraStic can issue GLES calls from its emulation workers. Capture state is
 * local to the host presentation thread so those unrelated calls cannot alter
 * the texture selected by renderFrame(). */
static _Thread_local DrasticVkCapture g_capture;

void drastic_vk_capture_begin(uint32_t *top, uint32_t *bottom,
                              unsigned width, unsigned height) {
  g_capture.screens[0] = top;
  g_capture.screens[1] = bottom;
  g_capture.width = width;
  g_capture.height = height;
  g_capture.texture = 0;
  g_capture.active = top && bottom && width && height;
}

void drastic_vk_capture_end(void) {
  memset(&g_capture, 0, sizeof(g_capture));
}

void drastic_vk_capture_gl_bind_texture(GLenum target, GLuint texture) {
  if (g_capture.active && target == GL_TEXTURE_2D)
    g_capture.texture = texture;
}

static uint8_t expand_5(unsigned value) {
  return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t expand_6(unsigned value) {
  return (uint8_t)((value << 2) | (value >> 4));
}

static uint32_t bgra_pixel(uint8_t red, uint8_t green, uint8_t blue) {
  return UINT32_C(0xff000000) | ((uint32_t)red << 16) |
         ((uint32_t)green << 8) | blue;
}

void drastic_vk_capture_gl_tex_sub_image_2d(
    GLenum target, GLint level, GLint xoffset, GLint yoffset,
    GLsizei width, GLsizei height, GLenum format, GLenum type,
    const void *pixels) {
  if (!g_capture.active || target != GL_TEXTURE_2D || level != 0 ||
      xoffset != 0 || yoffset != 0 || width <= 0 || height <= 0 ||
      (unsigned)width != g_capture.width ||
      (unsigned)height != g_capture.height || !pixels)
    return;

  unsigned screen;
  if (g_capture.texture == DRASTIC_VK_CAPTURE_TOP_TEXTURE)
    screen = 0;
  else if (g_capture.texture == DRASTIC_VK_CAPTURE_BOTTOM_TEXTURE)
    screen = 1;
  else
    return;

  uint32_t *output = g_capture.screens[screen];
  const size_t count = (size_t)width * (size_t)height;
  if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
    const uint8_t *input = (const uint8_t *)pixels;
    for (size_t index = 0; index < count; index++, input += 4)
      output[index] = bgra_pixel(input[0], input[1], input[2]);
    return;
  }

  const uint16_t *input = (const uint16_t *)pixels;
  if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
    for (size_t index = 0; index < count; index++) {
      const uint16_t value = input[index];
      output[index] = bgra_pixel(expand_5(value >> 11),
                                 expand_6((value >> 5) & 0x3f),
                                 expand_5(value & 0x1f));
    }
  }
}

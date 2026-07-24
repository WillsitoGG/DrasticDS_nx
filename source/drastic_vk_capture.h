#ifndef DRASTIC_NX_VK_CAPTURE_H
#define DRASTIC_NX_VK_CAPTURE_H

#ifdef USE_VULKAN

#include <GLES2/gl2.h>
#include <stdint.h>

#define DRASTIC_VK_CAPTURE_TOP_TEXTURE UINT32_C(0x44530001)
#define DRASTIC_VK_CAPTURE_BOTTOM_TEXTURE UINT32_C(0x44530002)

void drastic_vk_capture_begin(uint32_t *top, uint32_t *bottom,
                              unsigned width, unsigned height);
void drastic_vk_capture_end(void);

void drastic_vk_capture_gl_bind_texture(GLenum target, GLuint texture);
void drastic_vk_capture_gl_tex_sub_image_2d(
    GLenum target, GLint level, GLint xoffset, GLint yoffset,
    GLsizei width, GLsizei height, GLenum format, GLenum type,
    const void *pixels);

#endif

#endif

#ifndef DRASTIC_NX_RENDERER_H
#define DRASTIC_NX_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "drastic_config.h"
#include "overlay.h"

typedef void (*DrasticCoreRenderFrame)(void *env, void *clazz,
                                       int texture_top, int texture_bottom,
                                       unsigned char swap);

bool drastic_renderer_init(const DrasticRuntimeConfig *config);
void drastic_renderer_present(const DrasticRuntimeConfig *config,
                              DrasticCoreRenderFrame core_render,
                              void *env, void *clazz,
                              const uint32_t *top, const uint32_t *bottom,
                              const DrasticOverlayFrame *overlay,
                              bool consume_core_frame);
void drastic_renderer_shutdown(void);
unsigned drastic_renderer_frame_count(void);
bool drastic_renderer_lsfg_available(void);
bool drastic_renderer_lsfg_enabled(void);
bool drastic_renderer_lsfg_request_enabled(bool enabled);
bool drastic_renderer_set_custom_shader(const char *relative_path,
                                        char *error, size_t error_size);
const char *drastic_renderer_last_error(void);

#endif

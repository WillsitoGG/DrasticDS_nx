#ifndef DRASTIC_NX_RUNTIME_CONFIG_H
#define DRASTIC_NX_RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  DRASTIC_LAYOUT_VERTICAL,
  DRASTIC_LAYOUT_HORIZONTAL,
  DRASTIC_LAYOUT_TOP_ONLY,
  DRASTIC_LAYOUT_BOTTOM_ONLY,
  DRASTIC_LAYOUT_HYBRID_TOP,
  DRASTIC_LAYOUT_HYBRID_BOTTOM,
  DRASTIC_LAYOUT_CUSTOM,
} DrasticLayoutMode;

typedef enum {
  DRASTIC_FILTER_NEAREST,
  DRASTIC_FILTER_LINEAR,
  DRASTIC_FILTER_QUILEZ,
  DRASTIC_FILTER_SCANLINE,
  DRASTIC_FILTER_SCALE2X,
  DRASTIC_FILTER_HQ2X,
  DRASTIC_FILTER_FXAA,
  DRASTIC_FILTER_FXAA_HQ,
  DRASTIC_FILTER_SMAA,
  DRASTIC_FILTER_COUNT,
} DrasticVideoFilter;

typedef struct {
  float x;
  float y;
  float width;
  float height;
  int screen;       /* 0 = DS top, 1 = DS bottom */
  int touch_target; /* accepts touchscreen input */
} DrasticScreenRect;

typedef struct {
  char rom_path[1024];
  char core_path[256];
  char firmware_nickname[32];
  DrasticLayoutMode layout;
  int swap_screens;
  int rotation;
  int screen_gap;
  int integer_scale;
  DrasticVideoFilter video_filter;
  int show_fps;
  int volume;
  int autosave_seconds;
  int vibration;
  int motion;
  int lua_enabled;
  int analog_stylus;
  int stylus_x;
  int stylus_y;
  int stylus_visible;
  int screen_count;
  uint32_t firmware_userdata;
  uint64_t core_config;
  DrasticScreenRect screens[3];
  /* Normalized custom rectangles. Index 0 is the DS top screen. */
  DrasticScreenRect custom_screens[2];
} DrasticRuntimeConfig;

void drastic_config_load(DrasticRuntimeConfig *config);
uint64_t drastic_config_build_core_config(void);
const char *drastic_config_filter_name(DrasticVideoFilter filter);
void drastic_config_calculate_layout(DrasticRuntimeConfig *config,
                                     int width, int height);
bool drastic_config_map_touch(const DrasticRuntimeConfig *config,
                              float panel_x, float panel_y,
                              int *ds_x, int *ds_y);
bool drastic_config_map_stylus(const DrasticRuntimeConfig *config,
                               int ds_x, int ds_y,
                               float *panel_x, float *panel_y);

#endif

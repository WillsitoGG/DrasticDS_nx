#ifndef DRASTIC_NX_ROTATION_H
#define DRASTIC_NX_ROTATION_H

/*
 * Normalized coordinate transforms shared by rendering and input.
 *
 * Display coordinates use the physical output orientation (top-left origin),
 * while source coordinates use the unrotated 256x192 DS framebuffer.  Keeping
 * the two inverse transforms here prevents the renderer, touchscreen and
 * analog stylus paths from acquiring different 90-degree conventions.
 */
static inline void drastic_rotation_display_to_source(
    int rotation, float display_u, float display_v,
    float *source_u, float *source_v) {
  switch (rotation & 3) {
    case 1:
      *source_u = display_v;
      *source_v = 1.0f - display_u;
      break;
    case 2:
      *source_u = 1.0f - display_u;
      *source_v = 1.0f - display_v;
      break;
    case 3:
      *source_u = 1.0f - display_v;
      *source_v = display_u;
      break;
    default:
      *source_u = display_u;
      *source_v = display_v;
      break;
  }
}

static inline void drastic_rotation_source_to_display(
    int rotation, float source_u, float source_v,
    float *display_u, float *display_v) {
  switch (rotation & 3) {
    case 1:
      *display_u = 1.0f - source_v;
      *display_v = source_u;
      break;
    case 2:
      *display_u = 1.0f - source_u;
      *display_v = 1.0f - source_v;
      break;
    case 3:
      *display_u = source_v;
      *display_v = 1.0f - source_u;
      break;
    default:
      *display_u = source_u;
      *display_v = source_v;
      break;
  }
}

static inline void drastic_rotation_display_delta_to_source(
    int rotation, float display_x, float display_y,
    float *source_x, float *source_y) {
  switch (rotation & 3) {
    case 1:
      *source_x = display_y;
      *source_y = -display_x;
      break;
    case 2:
      *source_x = -display_x;
      *source_y = -display_y;
      break;
    case 3:
      *source_x = -display_y;
      *source_y = display_x;
      break;
    default:
      *source_x = display_x;
      *source_y = display_y;
      break;
  }
}

#endif

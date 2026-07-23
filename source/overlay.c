#include <switch.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "overlay.h"

static uint32_t g_pixels[DRASTIC_OVERLAY_PIXELS];
static DrasticOverlayFrame g_frame = {
  .pixels = g_pixels,
  .width = DRASTIC_OVERLAY_WIDTH,
  .height = DRASTIC_OVERLAY_HEIGHT,
};
static ConsoleFont g_font;
static int g_font_ready;
static uint64_t g_hud_generation = UINT64_MAX;
static int g_hud_show_fps = -1;
static int g_hud_fps_tenth = -2;
static int g_hud_fast_forward = -1;

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

void overlay_init(int rotation) {
  PrintConsole *console = consoleGetDefault();
  if (console && console->font.gfx && console->font.tileWidth &&
      console->font.tileHeight) {
    g_font = console->font;
    g_font_ready = 1;
  }
  g_frame.width = (rotation & 1) ? DRASTIC_OVERLAY_HEIGHT
                                 : DRASTIC_OVERLAY_WIDTH;
  g_frame.height = (rotation & 1) ? DRASTIC_OVERLAY_WIDTH
                                  : DRASTIC_OVERLAY_HEIGHT;
  memset(g_pixels, 0, sizeof(g_pixels));
}

void overlay_set_rotation(int rotation) {
  const int width = (rotation & 1) ? DRASTIC_OVERLAY_HEIGHT
                                   : DRASTIC_OVERLAY_WIDTH;
  const int height = (rotation & 1) ? DRASTIC_OVERLAY_WIDTH
                                    : DRASTIC_OVERLAY_HEIGHT;
  if (g_frame.width == width && g_frame.height == height) return;
  g_frame.width = width;
  g_frame.height = height;
  memset(g_pixels, 0, sizeof(g_pixels));
  g_frame.visible = false;
  g_frame.generation++;
  g_hud_generation = UINT64_MAX;
}

int overlay_width(void) { return g_frame.width; }

int overlay_height(void) { return g_frame.height; }

void overlay_begin(void) {
  memset(g_pixels, 0, sizeof(g_pixels));
  g_frame.visible = true;
}

void overlay_finish(void) {
  g_frame.visible = true;
  g_frame.generation++;
}

void overlay_hide(void) {
  if (g_frame.visible) {
    g_frame.visible = false;
    g_frame.generation++;
  }
}

void overlay_draw_hud(bool show_fps, float fps, bool fast_forward) {
  int fps_tenth = -1;
  if (fps > 0.0f) {
    fps_tenth = (int)(fps * 10.0f + 0.5f);
    if (fps_tenth > 9999) fps_tenth = 9999;
  }

  if (!show_fps && !fast_forward) {
    overlay_hide();
    g_hud_show_fps = 0;
    g_hud_fast_forward = 0;
    g_hud_fps_tenth = fps_tenth;
    g_hud_generation = g_frame.generation;
    return;
  }

  if (g_frame.visible && g_hud_generation == g_frame.generation &&
      g_hud_show_fps == (int)show_fps &&
      g_hud_fast_forward == (int)fast_forward &&
      g_hud_fps_tenth == fps_tenth)
    return;

  enum {
    HUD_MARGIN = 18,
    HUD_HEIGHT = 40,
    HUD_GAP = 8,
    HUD_FPS_WIDTH = 168,
    HUD_FAST_FORWARD_WIDTH = 272,
  };
  const uint32_t background = 0xf018202cu;
  const uint32_t border = 0xff2bc3d9u;
  const uint32_t text = 0xfff1f5f9u;
  const int right = g_frame.width - HUD_MARGIN;
  int top = HUD_MARGIN;

  overlay_begin();
  if (show_fps) {
    char label[32];
    if (fps_tenth < 0)
      snprintf(label, sizeof(label), "FPS  --.-");
    else
      snprintf(label, sizeof(label), "FPS %3d.%d",
               fps_tenth / 10, fps_tenth % 10);
    const int left = right - HUD_FPS_WIDTH;
    overlay_fill_rect(left, top, HUD_FPS_WIDTH, HUD_HEIGHT, background);
    overlay_border_rect(left, top, HUD_FPS_WIDTH, HUD_HEIGHT, 2, border);
    overlay_draw_text(left + 16, top + 12, text, label);
    top += HUD_HEIGHT + HUD_GAP;
  }
  if (fast_forward) {
    const int left = right - HUD_FAST_FORWARD_WIDTH;
    overlay_fill_rect(left, top, HUD_FAST_FORWARD_WIDTH, HUD_HEIGHT,
                      background);
    overlay_border_rect(left, top, HUD_FAST_FORWARD_WIDTH, HUD_HEIGHT, 2,
                        border);
    overlay_draw_text(left + 16, top + 12, text, ">> FAST FORWARD");
  }
  overlay_finish();
  g_hud_show_fps = show_fps;
  g_hud_fast_forward = fast_forward;
  g_hud_fps_tenth = fps_tenth;
  g_hud_generation = g_frame.generation;
}

const DrasticOverlayFrame *overlay_frame(void) { return &g_frame; }

static uint32_t composite_argb(uint32_t destination, uint32_t source) {
  const unsigned source_alpha = source >> 24;
  if (!source_alpha) return destination;
  if (source_alpha == 255 || !(destination >> 24)) return source;

  const unsigned destination_alpha = destination >> 24;
  const unsigned inverse_alpha = 255 - source_alpha;
  const unsigned output_alpha = source_alpha +
      (destination_alpha * inverse_alpha + 127) / 255;
  uint32_t output = (uint32_t)output_alpha << 24;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    const unsigned source_channel = (source >> shift) & 255;
    const unsigned destination_channel = (destination >> shift) & 255;
    const unsigned premultiplied = source_channel * source_alpha +
        (destination_channel * destination_alpha * inverse_alpha + 127) / 255;
    const unsigned output_channel =
        (premultiplied + output_alpha / 2) / output_alpha;
    output |= (uint32_t)output_channel << shift;
  }
  return output;
}

void overlay_fill_rect(int x, int y, int width, int height, uint32_t color) {
  if (width <= 0 || height <= 0) return;
  const int left = clamp_int(x, 0, g_frame.width);
  const int top = clamp_int(y, 0, g_frame.height);
  const int right = clamp_int(x + width, 0, g_frame.width);
  const int bottom = clamp_int(y + height, 0, g_frame.height);
  for (int row = top; row < bottom; row++) {
    uint32_t *destination = g_pixels + (size_t)row * g_frame.width;
    for (int column = left; column < right; column++)
      destination[column] = composite_argb(destination[column], color);
  }
}

void overlay_border_rect(int x, int y, int width, int height, int thickness,
                         uint32_t color) {
  if (thickness <= 0) return;
  overlay_fill_rect(x, y, width, thickness, color);
  overlay_fill_rect(x, y + height - thickness, width, thickness, color);
  overlay_fill_rect(x, y, thickness, height, color);
  overlay_fill_rect(x + width - thickness, y, thickness, height, color);
}

static int glyph_set(unsigned character, int source_x, int source_y) {
  if (!g_font_ready || character < g_font.asciiOffset ||
      character >= (unsigned)g_font.asciiOffset + g_font.numChars)
    return 0;
  const unsigned glyph = character - g_font.asciiOffset;
  const unsigned bytes_per_row = (g_font.tileWidth + 7) / 8;
  const uint8_t *tile = (const uint8_t *)g_font.gfx +
      (size_t)glyph * g_font.tileHeight * bytes_per_row;
  /* Each row is a little-endian integer, but libnx maps its most-significant
   * bit to the glyph's left edge. Keep rows top-to-bottom and reverse only the
   * bit index within the row. */
  const uint8_t *row = tile + (size_t)source_y * bytes_per_row;
  const unsigned stored_bit = (unsigned)g_font.tileWidth - 1u -
                              (unsigned)source_x;
  return (row[stored_bit / 8] & (uint8_t)(1u << (stored_bit & 7))) != 0;
}

static void draw_character(int x, int y, int scale, uint32_t color,
                           unsigned character) {
  if (!g_font_ready || scale <= 0) return;
  if (character > 255) character = '?';
  for (int source_y = 0; source_y < g_font.tileHeight; source_y++) {
    for (int source_x = 0; source_x < g_font.tileWidth; source_x++) {
      if (!glyph_set(character, source_x, source_y)) continue;
      overlay_fill_rect(x + source_x * scale, y + source_y * scale,
                        scale, scale, color);
    }
  }
}

void overlay_draw_text_scaled(int x, int y, int scale, uint32_t color,
                              const char *text) {
  if (!text || !g_font_ready || scale <= 0) return;
  const int origin = x;
  for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
       cursor++) {
    if (*cursor == '\n') {
      x = origin;
      y += g_font.tileHeight * scale;
      continue;
    }
    unsigned character = *cursor;
    if (character >= 0x80) {
      character = '?';
      while ((cursor[1] & 0xc0) == 0x80) cursor++;
    }
    draw_character(x, y, scale, color, character);
    x += g_font.tileWidth * scale;
  }
}

void overlay_draw_text(int x, int y, uint32_t color, const char *text) {
  overlay_draw_text_scaled(x, y, 1, color, text);
}

void overlay_draw_text_clipped(int x, int y, int max_width, uint32_t color,
                               const char *text) {
  if (!text || max_width <= 0 || !g_font_ready) return;
  const int cells = max_width / g_font.tileWidth;
  if (cells <= 0) return;
  char buffer[192];
  int used = 0;
  const unsigned char *cursor = (const unsigned char *)text;
  while (*cursor && used < cells && used < (int)sizeof(buffer) - 1) {
    if (*cursor < 0x80) {
      buffer[used++] = (char)*cursor++;
    } else {
      buffer[used++] = '?';
      cursor++;
      while ((*cursor & 0xc0) == 0x80) cursor++;
    }
  }
  if (*cursor && cells >= 3) {
    used = cells < (int)sizeof(buffer) ? cells : (int)sizeof(buffer) - 1;
    buffer[used - 3] = '.';
    buffer[used - 2] = '.';
    buffer[used - 1] = '.';
  }
  buffer[used] = '\0';
  overlay_draw_text(x, y, color, buffer);
}

void overlay_draw_text_right(int right, int y, uint32_t color,
                             const char *text) {
  if (!text || !g_font_ready) return;
  int characters = 0;
  for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
       cursor++)
    if ((*cursor & 0xc0) != 0x80) characters++;
  overlay_draw_text(right - characters * g_font.tileWidth, y, color, text);
}

void overlay_draw_wrapped(int x, int y, int max_width, int max_lines,
                          uint32_t color, const char *text) {
  if (!text || !g_font_ready || max_lines <= 0) return;
  const int columns = max_width / g_font.tileWidth;
  if (columns < 2) return;
  char line[192];
  const char *cursor = text;
  for (int row = 0; row < max_lines && *cursor; row++) {
    while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r') cursor++;
    int used = 0;
    int last_space = -1;
    const char *line_start = cursor;
    const char *next = cursor;
    while (*next && *next != '\n' && used < columns &&
           used < (int)sizeof(line) - 1) {
      unsigned char c = (unsigned char)*next;
      if (c == ' ') last_space = used;
      line[used++] = c < 0x80 ? (char)c : '?';
      next++;
      while (((unsigned char)*next & 0xc0) == 0x80) next++;
    }
    if (*next && *next != '\n' && used >= columns && last_space > 0) {
      used = last_space;
      next = line_start;
      int cells = 0;
      while (*next && cells < used) {
        next++;
        while (((unsigned char)*next & 0xc0) == 0x80) next++;
        cells++;
      }
    }
    while (used > 0 && line[used - 1] == ' ') used--;
    line[used] = '\0';
    overlay_draw_text(x, y + row * g_font.tileHeight, color, line);
    cursor = next;
  }
}

static uint32_t rgb565_to_argb(uint16_t pixel) {
  const unsigned red = (pixel >> 11) & 31;
  const unsigned green = (pixel >> 5) & 63;
  const unsigned blue = pixel & 31;
  return 0xff000000u | ((red * 255 / 31) << 16) |
         ((green * 255 / 63) << 8) | (blue * 255 / 31);
}

void overlay_blit_snapshot(int x, int y, int width, int height,
                           const int32_t *pixels, int source_width,
                           int source_height) {
  if (!pixels || width <= 0 || height <= 0 || source_width <= 0 ||
      source_height <= 0) return;
  int argb_pixels = 0;
  const int samples = source_width * source_height;
  for (int index = 0; index < samples; index += 257) {
    const uint32_t pixel = (uint32_t)pixels[index];
    if ((pixel & 0xffff0000u) && pixel != 0xffffffffu) {
      argb_pixels = 1;
      break;
    }
  }
  for (int destination_y = 0; destination_y < height; destination_y++) {
    const int output_y = y + destination_y;
    if (output_y < 0 || output_y >= g_frame.height) continue;
    const int source_y = destination_y * source_height / height;
    uint32_t *destination = g_pixels +
        (size_t)output_y * g_frame.width;
    for (int destination_x = 0; destination_x < width; destination_x++) {
      const int output_x = x + destination_x;
      if (output_x < 0 || output_x >= g_frame.width) continue;
      const int source_x = destination_x * source_width / width;
      uint32_t pixel = (uint32_t)pixels[source_y * source_width + source_x];
      if (!argb_pixels) pixel = rgb565_to_argb((uint16_t)pixel);
      else if (!(pixel & 0xff000000u)) pixel |= 0xff000000u;
      destination[output_x] = pixel;
    }
  }
}

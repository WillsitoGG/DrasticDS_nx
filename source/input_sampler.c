/* Dedicated Switch HID sampler for DraStic.
 *
 * Android updates DraStic's native input state independently from its GL
 * renderer.  Keeping PadState ownership on this thread reproduces that model
 * without racing padUpdate() between the game loop and the in-game menu.
 */

#include "input_sampler.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "pthr.h"

#define INPUT_SAMPLE_INTERVAL_NS UINT64_C(1000000)
#define DS_TOUCH ((int)UINT32_C(0x80000000))

typedef struct {
  int gameplay_enabled;
  int analog_stylus;
  int rotation;
  int screen_count;
  DrasticScreenRect screens[3];
} InputRuntime;

struct DrasticInputSampler {
  DrasticInputSamplerConfig config;
  PadState pad;
  pthread_t thread;
  Mutex lock;
  InputRuntime runtime;
  DrasticInputSnapshot snapshot;
  u64 buttons_down_latched;
  uint32_t hotkeys_latched;
  volatile int stop;
  float stylus_x;
  float stylus_y;
  u64 stylus_visible_until;
};

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static float normalized_axis(int value) {
  float result = (float)value / 32767.0f;
  if (result < -1.0f) result = -1.0f;
  if (result > 1.0f) result = 1.0f;
  return result;
}

static int combo_held(u64 held, u64 combo) {
  return combo && (held & combo) == combo;
}

static int map_touch(const InputRuntime *runtime, float panel_x, float panel_y,
                     int *ds_x, int *ds_y) {
  const DrasticScreenRect *target = NULL;
  for (int index = 0; index < runtime->screen_count; index++) {
    const DrasticScreenRect *rect = &runtime->screens[index];
    if (!rect->touch_target || panel_x < rect->x || panel_y < rect->y ||
        panel_x >= rect->x + rect->width ||
        panel_y >= rect->y + rect->height)
      continue;
    if (!target || rect->width * rect->height > target->width * target->height)
      target = rect;
  }
  if (!target || target->width <= 0.0f || target->height <= 0.0f) return 0;

  const float x = (panel_x - target->x) * 256.0f / target->width;
  const float y = (panel_y - target->y) * 192.0f / target->height;
  int ix = clamp_int((int)x, 0, 255);
  int iy = clamp_int((int)y, 0, 191);
  switch (runtime->rotation & 3) {
    case 1: {
      const int old = ix;
      ix = clamp_int((int)(y * 256.0f / 192.0f), 0, 255);
      iy = clamp_int(191 - old * 192 / 256, 0, 191);
      break;
    }
    case 2:
      ix = 255 - ix;
      iy = 191 - iy;
      break;
    case 3: {
      const int old = ix;
      ix = clamp_int(255 - (int)(y * 256.0f / 192.0f), 0, 255);
      iy = clamp_int(old * 192 / 256, 0, 191);
      break;
    }
    default:
      break;
  }
  *ds_x = ix;
  *ds_y = iy;
  return 1;
}

static int map_buttons(const DrasticInputSampler *sampler, u64 held,
                       HidAnalogStickState left) {
  int buttons = 0;
  for (int index = 0; index < sampler->config.binding_count; index++) {
    const DrasticInputBinding *binding = &sampler->config.bindings[index];
    if (held & binding->switch_mask) buttons |= binding->ds_mask;
  }
  if (sampler->config.analog_dpad) {
    const int deadzone = sampler->config.analog_deadzone;
    if (left.x < -deadzone) buttons |= 4;
    if (left.x > deadzone) buttons |= 8;
    if (left.y < -deadzone) buttons |= 2;
    if (left.y > deadzone) buttons |= 1;
  }
  return buttons;
}

static void publish_snapshot(DrasticInputSampler *sampler,
                             const DrasticInputSnapshot *snapshot,
                             u64 previous_buttons,
                             uint32_t hotkeys_pressed) {
  mutexLock(&sampler->lock);
  sampler->snapshot = *snapshot;
  sampler->buttons_down_latched |= snapshot->buttons & ~previous_buttons;
  sampler->hotkeys_latched |= hotkeys_pressed;
  mutexUnlock(&sampler->lock);
}

static void read_runtime(DrasticInputSampler *sampler, InputRuntime *runtime) {
  mutexLock(&sampler->lock);
  *runtime = sampler->runtime;
  mutexUnlock(&sampler->lock);
}

static void *input_thread_main(void *opaque) {
  DrasticInputSampler *sampler = (DrasticInputSampler *)opaque;
  pthr_install_fake_tls();
  pthr_pin_bg_core();

  const u64 frequency = armGetSystemTickFreq();
  u64 previous_buttons = 0;
  u64 previous_tick = armGetSystemTick();
  int last_buttons = 0;
  int last_touch = 0;
  int last_autofire = 0;
  int last_enabled = 0;
  int last_update_valid = 0;

  while (!__atomic_load_n(&sampler->stop, __ATOMIC_ACQUIRE)) {
    padUpdate(&sampler->pad);
    const u64 held = padGetButtons(&sampler->pad);
    const HidAnalogStickState left = padGetStickPos(&sampler->pad, 0);
    const HidAnalogStickState right = padGetStickPos(&sampler->pad, 1);
    const u64 now = armGetSystemTick();

    InputRuntime runtime;
    read_runtime(sampler, &runtime);

    uint32_t hotkeys_pressed = 0;
    u64 game_held = held;
    for (int index = 0; index < DRASTIC_INPUT_HOTKEY_COUNT; index++) {
      const u64 combo = sampler->config.hotkeys[index];
      if (combo_held(held, combo)) {
        game_held &= ~combo;
        if (!combo_held(previous_buttons, combo))
          hotkeys_pressed |= DRASTIC_INPUT_HOTKEY_BIT(index);
      }
    }

    const int right_active = runtime.gameplay_enabled &&
        runtime.analog_stylus &&
        (abs(right.x) > 3500 || abs(right.y) > 3500);
    if (right_active && frequency) {
      u64 elapsed = now - previous_tick;
      const u64 maximum = frequency / 20;
      if (maximum && elapsed > maximum) elapsed = maximum;
      const float frame_scale =
          (float)((double)elapsed * 60.0 / (double)frequency);
      sampler->stylus_x += normalized_axis(right.x) *
                            sampler->config.stylus_speed * frame_scale;
      sampler->stylus_y -= normalized_axis(right.y) *
                            sampler->config.stylus_speed * frame_scale;
      if (sampler->stylus_x < 0.0f) sampler->stylus_x = 0.0f;
      if (sampler->stylus_x > 255.0f) sampler->stylus_x = 255.0f;
      if (sampler->stylus_y < 0.0f) sampler->stylus_y = 0.0f;
      if (sampler->stylus_y > 191.0f) sampler->stylus_y = 191.0f;
      sampler->stylus_visible_until = now + frequency * 3;
    }
    previous_tick = now;

    const int analog_touch = runtime.gameplay_enabled &&
        runtime.analog_stylus &&
        sampler->config.analog_touch_button &&
        (held & sampler->config.analog_touch_button);
    if (analog_touch) {
      game_held &= ~sampler->config.analog_touch_button;
      if (frequency) sampler->stylus_visible_until = now + frequency * 3;
    }

    int touching = 0;
    int physical_touch = 0;
    int touch_position = 0;
    HidTouchScreenState touch = {0};
    if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
      int x = 0, y = 0;
      const float panel_x = (float)touch.touches[0].x *
                            sampler->config.panel_width / 1280.0f;
      const float panel_y = (float)touch.touches[0].y *
                            sampler->config.panel_height / 720.0f;
      if (map_touch(&runtime, panel_x, panel_y, &x, &y)) {
        touching = 1;
        physical_touch = 1;
        touch_position = (x << 16) | y;
      }
    }
    if (!touching && analog_touch) {
      touching = 1;
      touch_position = ((int)(sampler->stylus_x + 0.5f) << 16) |
                       (int)(sampler->stylus_y + 0.5f);
    }

    int buttons = map_buttons(sampler, game_held, left);
    if (touching) buttons |= DS_TOUCH;
    const int autofire = combo_held(
        held, sampler->config.hotkeys[DRASTIC_INPUT_HOTKEY_AUTOFIRE])
        ? buttons & (16 | 32 | 64 | 128 | 256 | 512) : 0;

    const int enabled = runtime.gameplay_enabled != 0;
    const int output_buttons = enabled ? buttons : 0;
    const int output_touch = enabled ? touch_position : 0;
    const int output_autofire = enabled ? autofire : 0;
    if (sampler->config.update &&
        (!last_update_valid || output_buttons != last_buttons ||
         output_touch != last_touch || output_autofire != last_autofire ||
         enabled != last_enabled)) {
      sampler->config.update(sampler->config.user, output_buttons,
                             output_touch, output_autofire);
      last_buttons = output_buttons;
      last_touch = output_touch;
      last_autofire = output_autofire;
      last_update_valid = 1;
    }
    last_enabled = enabled;

    DrasticInputSnapshot snapshot = {
      .buttons = held,
      .left = left,
      .right = right,
      .style_set = padGetStyleSet(&sampler->pad),
      .attributes = padGetAttributes(&sampler->pad),
      .stylus_x = (int)(sampler->stylus_x + 0.5f),
      .stylus_y = (int)(sampler->stylus_y + 0.5f),
      .stylus_visible = runtime.analog_stylus && !physical_touch &&
          sampler->stylus_visible_until > now,
    };
    publish_snapshot(sampler, &snapshot, previous_buttons, hotkeys_pressed);
    previous_buttons = held;
    svcSleepThread(INPUT_SAMPLE_INTERVAL_NS);
  }

  if (sampler->config.update && last_update_valid &&
      (last_buttons || last_touch || last_autofire || last_enabled))
    sampler->config.update(sampler->config.user, 0, 0, 0);
  return NULL;
}

DrasticInputSampler *drastic_input_sampler_create(
    const DrasticInputSamplerConfig *config) {
  if (!config || !config->update || config->binding_count < 0 ||
      config->binding_count > DRASTIC_INPUT_MAX_BINDINGS ||
      config->panel_width <= 0 || config->panel_height <= 0)
    return NULL;

  DrasticInputSampler *sampler = calloc(1, sizeof(*sampler));
  if (!sampler) return NULL;
  sampler->config = *config;
  sampler->stylus_x = 128.0f;
  sampler->stylus_y = 96.0f;
  mutexInit(&sampler->lock);
  padInitializeDefault(&sampler->pad);
  if (pthread_create(&sampler->thread, NULL, input_thread_main, sampler) != 0) {
    free(sampler);
    return NULL;
  }
  return sampler;
}

void drastic_input_sampler_update_runtime(
    DrasticInputSampler *sampler, const DrasticRuntimeConfig *config,
    bool gameplay_enabled) {
  if (!sampler || !config) return;
  InputRuntime runtime = {
    .gameplay_enabled = gameplay_enabled,
    .analog_stylus = config->analog_stylus,
    .rotation = config->rotation,
    .screen_count = config->screen_count,
  };
  if (runtime.screen_count < 0) runtime.screen_count = 0;
  if (runtime.screen_count > 3) runtime.screen_count = 3;
  memcpy(runtime.screens, config->screens,
         (size_t)runtime.screen_count * sizeof(*runtime.screens));
  mutexLock(&sampler->lock);
  sampler->runtime = runtime;
  mutexUnlock(&sampler->lock);
}

void drastic_input_sampler_read(DrasticInputSampler *sampler,
                                DrasticInputSnapshot *snapshot) {
  if (!snapshot) return;
  memset(snapshot, 0, sizeof(*snapshot));
  if (!sampler) return;
  mutexLock(&sampler->lock);
  *snapshot = sampler->snapshot;
  snapshot->buttons_down = sampler->buttons_down_latched;
  snapshot->hotkeys_pressed = sampler->hotkeys_latched;
  sampler->buttons_down_latched = 0;
  sampler->hotkeys_latched = 0;
  mutexUnlock(&sampler->lock);
}

void drastic_input_sampler_destroy(DrasticInputSampler *sampler) {
  if (!sampler) return;
  __atomic_store_n(&sampler->stop, 1, __ATOMIC_RELEASE);
  pthread_join(sampler->thread, NULL);
  free(sampler);
}

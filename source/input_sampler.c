/* Dedicated Switch HID sampler for DraStic.
 *
 * Android updates DraStic's native input state independently from its GL
 * renderer.  Keeping PadState ownership on this thread reproduces that model
 * without racing padUpdate() between the game loop and the in-game menu.
 */

#include "input_sampler.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "drastic_rotation.h"
#include "pthr.h"

#define INPUT_SAMPLE_INTERVAL_NS UINT64_C(1000000)
#define DS_TOUCH ((int)UINT32_C(0x80000000))

typedef struct {
  int gameplay_enabled;
  DrasticStylusMode stylus_mode;
  int mouse_stylus;
  int motion_stylus_sensitivity;
  int rotation;
  int screen_count;
  DrasticScreenRect screens[3];
} InputRuntime;

typedef struct {
  HidSixAxisSensorHandle handles[6];
  int ready[6];
} MotionSensors;

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
  MotionSensors motion;
  int motion_stylus_calibrated;
  int motion_stylus_source;
  float motion_calibration[3][3];
};

static float normalized_axis(int value) {
  float result = (float)value / 32767.0f;
  if (result < -1.0f) result = -1.0f;
  if (result > 1.0f) result = 1.0f;
  return result;
}

static void analog_stylus_delta_to_source(
    int rotation, u32 style_set, float controller_x, float controller_y,
    float *source_x, float *source_y) {
  if (appletGetOperationMode() != AppletOperationMode_Console ||
      (style_set & HidNpadStyleTag_NpadHandheld)) {
    /* In a vertical portable grip, the screen and controls rotate together.
     * Some grips expose their Joy-Cons as a wireless pair rather than the
     * Handheld style, so operation mode is the reliable primary signal.  The
     * renderer's source-to-display transform already turns these controller-
     * relative axes into the expected physical direction; applying the
     * inverse rotation here would rotate them a second time. */
    *source_x = controller_x;
    *source_y = controller_y;
    return;
  }

  /* External controllers remain upright when the displayed game is rotated,
   * so keep their cursor movement relative to the visible screen. */
  drastic_rotation_display_delta_to_source(
      rotation, controller_x, controller_y, source_x, source_y);
}

static void initialize_motion_sensors(DrasticInputSampler *sampler) {
  static const struct {
    HidNpadIdType id;
    HidNpadStyleTag style;
  } sources[6] = {
    {HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld},
    {HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey},
    {HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual},
    {HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual},
    {HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft},
    {HidNpadIdType_No1, HidNpadStyleTag_NpadJoyRight},
  };
  for (int index = 0; index < 6; index++) {
    if (index == 2 || index == 3) continue;
    HidSixAxisSensorHandle handle = {0};
    if (R_FAILED(hidGetSixAxisSensorHandles(
            &handle, 1, sources[index].id, sources[index].style)))
      continue;
    sampler->motion.handles[index] = handle;
    if (R_SUCCEEDED(hidStartSixAxisSensor(handle)))
      sampler->motion.ready[index] = 1;
  }
  HidSixAxisSensorHandle dual[2] = {0};
  if (R_SUCCEEDED(hidGetSixAxisSensorHandles(
          dual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual))) {
    for (int index = 0; index < 2; index++) {
      sampler->motion.handles[2 + index] = dual[index];
      if (R_SUCCEEDED(hidStartSixAxisSensor(dual[index])))
        sampler->motion.ready[2 + index] = 1;
    }
  }
}

static void shutdown_motion_sensors(DrasticInputSampler *sampler) {
  for (int index = 0; index < 6; index++)
    if (sampler->motion.ready[index])
      hidStopSixAxisSensor(sampler->motion.handles[index]);
  memset(&sampler->motion, 0, sizeof(sampler->motion));
}

static int core_motion_source(const DrasticInputSampler *sampler, u32 styles,
                              u32 attributes, int *right_joycon) {
  int source = -1;
  if ((styles & HidNpadStyleTag_NpadHandheld) && sampler->motion.ready[0])
    source = 0;
  else if ((styles & HidNpadStyleTag_NpadFullKey) &&
           sampler->motion.ready[1])
    source = 1;
  else if (styles & HidNpadStyleTag_NpadJoyDual) {
    if ((attributes & HidNpadAttribute_IsLeftConnected) &&
        sampler->motion.ready[2])
      source = 2;
    else if ((attributes & HidNpadAttribute_IsRightConnected) &&
             sampler->motion.ready[3])
      source = 3;
  } else if ((styles & HidNpadStyleTag_NpadJoyLeft) &&
             sampler->motion.ready[4])
    source = 4;
  else if ((styles & HidNpadStyleTag_NpadJoyRight) &&
           sampler->motion.ready[5])
    source = 5;
  if (right_joycon) *right_joycon = source == 3 || source == 5;
  return source;
}

static int stylus_motion_source(const DrasticInputSampler *sampler,
                                u32 styles, u32 attributes) {
  if ((styles & HidNpadStyleTag_NpadHandheld) && sampler->motion.ready[0])
    return 0;
  if ((styles & HidNpadStyleTag_NpadFullKey) && sampler->motion.ready[1])
    return 1;
  if (styles & HidNpadStyleTag_NpadJoyDual) {
    /* Match melonDS's right-handed default, falling back transparently when
     * only the left Joy-Con is available. */
    if ((attributes & HidNpadAttribute_IsRightConnected) &&
        sampler->motion.ready[3])
      return 3;
    if ((attributes & HidNpadAttribute_IsLeftConnected) &&
        sampler->motion.ready[2])
      return 2;
  }
  if ((styles & HidNpadStyleTag_NpadJoyRight) && sampler->motion.ready[5])
    return 5;
  if ((styles & HidNpadStyleTag_NpadJoyLeft) && sampler->motion.ready[4])
    return 4;
  return -1;
}

static int read_motion_state(const DrasticInputSampler *sampler, int source,
                             HidSixAxisSensorState *state) {
  if (source < 0 || source >= 6 || !sampler->motion.ready[source]) return 0;
  memset(state, 0, sizeof(*state));
  return hidGetSixAxisSensorStates(
             sampler->motion.handles[source], state, 1) > 0;
}

static void normalize_vector(float output[3], const float input[3]) {
  const float length = sqrtf(input[0] * input[0] + input[1] * input[1] +
                             input[2] * input[2]);
  if (length <= 0.000001f) {
    memset(output, 0, 3 * sizeof(*output));
    return;
  }
  for (int axis = 0; axis < 3; axis++) output[axis] = input[axis] / length;
}

static float dot_vector(const float left[3], const float right[3]) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

static void calibrate_motion_stylus(DrasticInputSampler *sampler, int source,
                                    const HidSixAxisSensorState *state) {
  for (int axis = 0; axis < 3; axis++)
    normalize_vector(sampler->motion_calibration[axis],
                     state->direction.direction[axis]);
  sampler->motion_stylus_calibrated = 1;
  sampler->motion_stylus_source = source;
}

static void update_motion_stylus(DrasticInputSampler *sampler,
                                 const InputRuntime *runtime, int source,
                                 const HidSixAxisSensorState *state,
                                 int recenter, u64 now, u64 frequency) {
  if (!sampler->motion_stylus_calibrated ||
      sampler->motion_stylus_source != source || recenter)
    calibrate_motion_stylus(sampler, source, state);

  float direction[3];
  float horizontal;
  float vertical;
  if (source == 0) {
    normalize_vector(direction, state->direction.direction[2]);
    horizontal = dot_vector(direction, sampler->motion_calibration[0]);
    vertical = dot_vector(direction, sampler->motion_calibration[1]);
  } else {
    normalize_vector(direction, state->direction.direction[1]);
    horizontal = dot_vector(direction, sampler->motion_calibration[0]);
    vertical = dot_vector(direction, sampler->motion_calibration[2]);
  }

  const float display_width = (runtime->rotation & 1) ? 192.0f : 256.0f;
  const float display_height = (runtime->rotation & 1) ? 256.0f : 192.0f;
  const float largest_dimension = fmaxf(display_width, display_height);
  const float scale = (float)runtime->motion_stylus_sensitivity /
                      3.14159265358979323846f;
  const float display_u = 0.5f + horizontal * scale *
      largest_dimension / display_width;
  const float display_v = 0.5f - vertical * scale *
      largest_dimension / display_height;
  float source_u;
  float source_v;
  drastic_rotation_display_to_source(runtime->rotation, display_u, display_v,
                                     &source_u, &source_v);
  if (source_u < 0.0f) source_u = 0.0f;
  if (source_u > 1.0f) source_u = 1.0f;
  if (source_v < 0.0f) source_v = 0.0f;
  if (source_v > 1.0f) source_v = 1.0f;
  sampler->stylus_x = source_u * 255.0f;
  sampler->stylus_y = source_v * 191.0f;
  if (frequency) sampler->stylus_visible_until = now + frequency;
}

static int combo_held(u64 held, u64 combo) {
  return combo && (held & combo) == combo;
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
    const u32 style_set = padGetStyleSet(&sampler->pad);
    const u32 attributes = padGetAttributes(&sampler->pad);
    const u64 now = armGetSystemTick();

    InputRuntime runtime;
    read_runtime(sampler, &runtime);

    uint32_t hotkeys_pressed = 0;
    u64 game_held = held;
    for (int index = 0; index < DRASTIC_INPUT_HOTKEY_COUNT; index++) {
      if (index == DRASTIC_INPUT_HOTKEY_MOTION_STYLUS_RECENTER &&
          runtime.stylus_mode != DRASTIC_STYLUS_MOTION)
        continue;
      const u64 combo = sampler->config.hotkeys[index];
      if (combo_held(held, combo)) {
        game_held &= ~combo;
        if (!combo_held(previous_buttons, combo))
          hotkeys_pressed |= DRASTIC_INPUT_HOTKEY_BIT(index);
      }
    }

    const int motion_recenter = hotkeys_pressed & DRASTIC_INPUT_HOTKEY_BIT(
        DRASTIC_INPUT_HOTKEY_MOTION_STYLUS_RECENTER);

    const int right_active = runtime.gameplay_enabled &&
        runtime.stylus_mode == DRASTIC_STYLUS_STICK &&
        (abs(right.x) > 3500 || abs(right.y) > 3500);
    if (right_active && frequency) {
      u64 elapsed = now - previous_tick;
      const u64 maximum = frequency / 20;
      if (maximum && elapsed > maximum) elapsed = maximum;
      const float frame_scale =
          (float)((double)elapsed * 60.0 / (double)frequency);
      const float display_x = normalized_axis(right.x);
      const float display_y = -normalized_axis(right.y);
      float source_x, source_y;
      analog_stylus_delta_to_source(
          runtime.rotation, style_set, display_x, display_y,
          &source_x, &source_y);
      sampler->stylus_x += source_x * sampler->config.stylus_speed *
                           frame_scale;
      sampler->stylus_y += source_y * sampler->config.stylus_speed *
                           frame_scale;
      if (sampler->stylus_x < 0.0f) sampler->stylus_x = 0.0f;
      if (sampler->stylus_x > 255.0f) sampler->stylus_x = 255.0f;
      if (sampler->stylus_y < 0.0f) sampler->stylus_y = 0.0f;
      if (sampler->stylus_y > 191.0f) sampler->stylus_y = 191.0f;
      sampler->stylus_visible_until = now + frequency * 3;
    }
    previous_tick = now;

    int core_right_joycon = 0;
    const int core_source = core_motion_source(
        sampler, style_set, attributes, &core_right_joycon);
    HidSixAxisSensorState core_motion = {0};
    const int core_motion_valid = read_motion_state(
        sampler, core_source, &core_motion);

    if (runtime.gameplay_enabled &&
        runtime.stylus_mode == DRASTIC_STYLUS_MOTION) {
      const int pointer_source = stylus_motion_source(
          sampler, style_set, attributes);
      HidSixAxisSensorState pointer_motion = {0};
      int pointer_motion_valid = 0;
      if (pointer_source == core_source && core_motion_valid) {
        pointer_motion = core_motion;
        pointer_motion_valid = 1;
      } else {
        pointer_motion_valid = read_motion_state(
            sampler, pointer_source, &pointer_motion);
      }
      if (pointer_motion_valid)
        update_motion_stylus(sampler, &runtime, pointer_source,
                             &pointer_motion, motion_recenter, now, frequency);
    } else {
      sampler->motion_stylus_calibrated = 0;
      sampler->motion_stylus_source = -1;
    }

    const int virtual_touch = runtime.gameplay_enabled &&
        runtime.stylus_mode != DRASTIC_STYLUS_OFF &&
        sampler->config.analog_touch_button &&
        (held & sampler->config.analog_touch_button) &&
        !(runtime.stylus_mode == DRASTIC_STYLUS_MOTION &&
          combo_held(held, sampler->config.hotkeys[
              DRASTIC_INPUT_HOTKEY_MOTION_STYLUS_RECENTER]));
    if (virtual_touch) {
      game_held &= ~sampler->config.analog_touch_button;
      if (frequency) sampler->stylus_visible_until = now + frequency * 3;
    }

    int touching = 0;
    int physical_touch = 0;
    int touch_position = 0;
    int mouse_inside = 0;
    int mouse_x = 0;
    int mouse_y = 0;
    HidMouseState mouse = {0};
    const int mouse_connected = runtime.gameplay_enabled &&
        runtime.mouse_stylus && hidGetMouseStates(&mouse, 1) > 0 &&
        (mouse.attributes & HidMouseAttribute_IsConnected);
    if (mouse_connected) {
      const float panel_x = (float)mouse.x *
                            sampler->config.panel_width / 1280.0f;
      const float panel_y = (float)mouse.y *
                            sampler->config.panel_height / 720.0f;
      if (drastic_config_map_touch_rects(
              runtime.screens, runtime.screen_count, runtime.rotation,
              panel_x, panel_y, &mouse_x, &mouse_y)) {
        mouse_inside = 1;
        sampler->stylus_x = (float)mouse_x;
        sampler->stylus_y = (float)mouse_y;
      }
    }

    HidTouchScreenState touch = {0};
    if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
      int x = 0, y = 0;
      const float panel_x = (float)touch.touches[0].x *
                            sampler->config.panel_width / 1280.0f;
      const float panel_y = (float)touch.touches[0].y *
                            sampler->config.panel_height / 720.0f;
      if (drastic_config_map_touch_rects(
              runtime.screens, runtime.screen_count, runtime.rotation,
              panel_x, panel_y, &x, &y)) {
        touching = 1;
        physical_touch = 1;
        touch_position = (x << 16) | y;
      }
    }
    if (!touching && mouse_inside && (mouse.buttons & HidMouseButton_Left)) {
      touching = 1;
      touch_position = (mouse_x << 16) | mouse_y;
    }
    if (!touching && virtual_touch) {
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
      .style_set = style_set,
      .attributes = attributes,
      .motion_sample = core_motion_valid ? core_motion.sampling_number : 0,
      .motion_acceleration = core_motion.acceleration,
      .motion_angular_velocity = core_motion.angular_velocity,
      .motion_source = core_motion_valid ? core_source : -1,
      .motion_right_joycon = core_motion_valid && core_right_joycon,
      .stylus_x = (int)(sampler->stylus_x + 0.5f),
      .stylus_y = (int)(sampler->stylus_y + 0.5f),
      .stylus_visible = runtime.gameplay_enabled && !physical_touch &&
          (mouse_inside || (runtime.stylus_mode != DRASTIC_STYLUS_OFF &&
                            sampler->stylus_visible_until > now)),
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
  sampler->motion_stylus_source = -1;
  mutexInit(&sampler->lock);
  padInitializeDefault(&sampler->pad);
  initialize_motion_sensors(sampler);
  if (pthread_create(&sampler->thread, NULL, input_thread_main, sampler) != 0) {
    shutdown_motion_sensors(sampler);
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
    .stylus_mode = config->stylus_mode,
    .mouse_stylus = config->mouse_stylus,
    .motion_stylus_sensitivity = config->motion_stylus_sensitivity,
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
  shutdown_motion_sensors(sampler);
  free(sampler);
}

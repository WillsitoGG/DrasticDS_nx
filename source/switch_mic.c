#include "switch_mic.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define SWITCH_MIC_MAX_DEVICES 8
#define SWITCH_MIC_SAMPLE_RATE 48000u
/* Horizon's AudioInParameter uses a 16.16 channel-count value on input even
 * though OpenAudioIn returns the ordinary channel count. */
#define SWITCH_MIC_CHANNEL_COUNT_INPUT 0x00020000u

static u32 input_buffer_attribute(void) {
  return (hosversionAtLeast(3, 0, 0) ? SfBufferAttr_HipcAutoSelect
                                    : SfBufferAttr_HipcMapAlias) |
         SfBufferAttr_In;
}

static u32 output_buffer_attribute(void) {
  return (hosversionAtLeast(3, 0, 0) ? SfBufferAttr_HipcAutoSelect
                                    : SfBufferAttr_HipcMapAlias) |
         SfBufferAttr_Out;
}

static Result list_inputs(SwitchMic *mic,
                          char names[SWITCH_MIC_MAX_DEVICES]
                                    [SWITCH_MIC_DEVICE_NAME_LENGTH],
                          u32 *count) {
  if (!mic || !count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  *count = 0;
  const u32 command = hosversionAtLeast(3, 0, 0) ? 2 : 0;
  return serviceDispatchOut(
      &mic->manager, command, *count,
      .buffer_attrs = {output_buffer_attribute()},
      .buffers = {{names, SWITCH_MIC_MAX_DEVICES *
                              SWITCH_MIC_DEVICE_NAME_LENGTH}});
}

static const char *select_input(
    char names[SWITCH_MIC_MAX_DEVICES][SWITCH_MIC_DEVICE_NAME_LENGTH],
    u32 count, bool *external) {
  const char *fallback = "";
  if (external) *external = false;
  if (count > SWITCH_MIC_MAX_DEVICES) count = SWITCH_MIC_MAX_DEVICES;
  for (u32 index = 0; index < count; index++) {
    names[index][SWITCH_MIC_DEVICE_NAME_LENGTH - 1] = '\0';
    if (!names[index][0]) continue;
    if (!fallback[0]) fallback = names[index];
    if (strcasecmp(names[index], "BuiltInHeadset")) {
      if (external) *external = true;
      return names[index];
    }
  }
  return fallback;
}

static Result open_input(SwitchMic *mic, const char *device_name) {
  char output_name[SWITCH_MIC_DEVICE_NAME_LENGTH] = {0};
  const struct {
    u32 sample_rate;
    u32 channel_count;
    u64 client_pid;
  } input = {
      SWITCH_MIC_SAMPLE_RATE,
      SWITCH_MIC_CHANNEL_COUNT_INPUT,
      0,
  };
  struct {
    u32 sample_rate;
    u32 channel_count;
    u32 format;
    u32 state;
  } output = {0};
  const bool modern = hosversionAtLeast(3, 0, 0);
  const u32 attribute = modern ? SfBufferAttr_HipcAutoSelect
                               : SfBufferAttr_HipcMapAlias;
  Result result = serviceDispatchInOut(
      &mic->manager, modern ? 3 : 1, input, output,
      .buffer_attrs = {attribute | SfBufferAttr_In,
                       attribute | SfBufferAttr_Out},
      .buffers = {{device_name, SWITCH_MIC_DEVICE_NAME_LENGTH},
                  {output_name, sizeof(output_name)}},
      .in_send_pid = true,
      .in_num_handles = 1,
      .in_handles = {CUR_PROCESS_HANDLE},
      .out_num_objects = 1,
      .out_objects = &mic->input);
  if (R_FAILED(result)) return result;

  mic->sample_rate = output.sample_rate;
  mic->channel_count = output.channel_count;
  mic->format = (PcmFormat)output.format;
  output_name[sizeof(output_name) - 1] = '\0';
  snprintf(mic->device_name, sizeof(mic->device_name), "%s",
           output_name[0] ? output_name : device_name);
  if (mic->device_name[0] &&
      strcasecmp(mic->device_name, "BuiltInHeadset"))
    mic->external_device = true;
  return 0;
}

static Result register_buffer_event(SwitchMic *mic) {
  Handle handle = INVALID_HANDLE;
  Result result = serviceDispatch(
      &mic->input, 4,
      .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
      .out_handles = &handle);
  if (R_SUCCEEDED(result))
    eventLoadRemote(&mic->buffer_event, handle, true);
  return result;
}

Result switch_mic_open(SwitchMic *mic) {
  if (!mic) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  memset(mic, 0, sizeof(*mic));
  mic->buffer_event.revent = INVALID_HANDLE;
  mic->buffer_event.wevent = INVALID_HANDLE;

  Result result = smGetService(&mic->manager, "audin:u");
  if (R_FAILED(result)) goto fail;

  char names[SWITCH_MIC_MAX_DEVICES][SWITCH_MIC_DEVICE_NAME_LENGTH] = {{0}};
  u32 count = 0;
  result = list_inputs(mic, names, &count);
  if (R_FAILED(result)) goto fail;
  const char *selected = select_input(names, count, &mic->external_device);
  char selected_name[SWITCH_MIC_DEVICE_NAME_LENGTH] = {0};
  if (selected && selected[0])
    snprintf(selected_name, sizeof(selected_name), "%s", selected);
  result = open_input(mic, selected_name);
  if (R_FAILED(result)) goto fail;
  result = register_buffer_event(mic);
  if (R_FAILED(result)) goto fail;
  if (!mic->sample_rate || !mic->channel_count || mic->channel_count > 2 ||
      mic->format != PcmFormat_Int16) {
    result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    goto fail;
  }
  mic->opened = true;
  return 0;

fail:
  switch_mic_close(mic);
  return result;
}

void switch_mic_close(SwitchMic *mic) {
  if (!mic) return;
  if (mic->started && serviceIsActive(&mic->input))
    serviceDispatch(&mic->input, 2);
  mic->started = false;
  if (eventActive(&mic->buffer_event)) eventClose(&mic->buffer_event);
  if (serviceIsActive(&mic->input)) serviceClose(&mic->input);
  if (serviceIsActive(&mic->manager)) serviceClose(&mic->manager);
  mic->opened = false;
  mic->sample_rate = 0;
  mic->channel_count = 0;
  mic->format = PcmFormat_Invalid;
  mic->device_name[0] = '\0';
  mic->external_device = false;
}

Result switch_mic_start(SwitchMic *mic) {
  if (!mic || !mic->opened)
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
  if (mic->started) return 0;
  Result result = serviceDispatch(&mic->input, 1);
  if (R_SUCCEEDED(result)) mic->started = true;
  return result;
}

Result switch_mic_stop(SwitchMic *mic) {
  if (!mic || !mic->opened)
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
  if (!mic->started) return 0;
  Result result = serviceDispatch(&mic->input, 2);
  if (R_SUCCEEDED(result)) mic->started = false;
  return result;
}

Result switch_mic_append(SwitchMic *mic, AudioInBuffer *buffer) {
  if (!mic || !mic->opened || !buffer)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  const u64 tag = (u64)buffer;
  return serviceDispatchIn(
      &mic->input, hosversionAtLeast(3, 0, 0) ? 8 : 3, tag,
      .buffer_attrs = {input_buffer_attribute()},
      .buffers = {{buffer, sizeof(*buffer)}});
}

static Result get_released_buffer(SwitchMic *mic, AudioInBuffer **released,
                                  u32 *released_count) {
  if (released) *released = NULL;
  if (released_count) *released_count = 0;
  if (!mic || !released || !released_count)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  return serviceDispatchOut(
      &mic->input, hosversionAtLeast(3, 0, 0) ? 9 : 5, *released_count,
      .buffer_attrs = {output_buffer_attribute()},
      .buffers = {{released, sizeof(*released)}});
}

Result switch_mic_wait(SwitchMic *mic, AudioInBuffer **released,
                       u32 *released_count, u64 timeout_ns) {
  if (!mic || !mic->opened || !eventActive(&mic->buffer_event))
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
  Result result = eventWait(&mic->buffer_event, timeout_ns);
  if (R_SUCCEEDED(result))
    result = get_released_buffer(mic, released, released_count);
  return result;
}

Result switch_mic_external_available(SwitchMic *mic, bool *available) {
  if (available) *available = false;
  if (!mic || !available || !serviceIsActive(&mic->manager))
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
  char names[SWITCH_MIC_MAX_DEVICES][SWITCH_MIC_DEVICE_NAME_LENGTH] = {{0}};
  u32 count = 0;
  Result result = list_inputs(mic, names, &count);
  if (R_FAILED(result)) return result;
  bool external = false;
  select_input(names, count, &external);
  *available = external;
  return 0;
}

#ifndef DRASTIC_NX_SWITCH_MIC_H
#define DRASTIC_NX_SWITCH_MIC_H

#include <switch.h>

#include <stdbool.h>
#include <stddef.h>

#define SWITCH_MIC_DEVICE_NAME_LENGTH 0x100

typedef struct {
  Service manager;
  Service input;
  Event buffer_event;
  u32 sample_rate;
  u32 channel_count;
  PcmFormat format;
  bool opened;
  bool started;
  bool external_device;
  char device_name[SWITCH_MIC_DEVICE_NAME_LENGTH];
} SwitchMic;

/* Opens the preferred Horizon audio input. An attached USB/UAC input is
 * preferred over the analog BuiltInHeadset endpoint. */
Result switch_mic_open(SwitchMic *mic);
void switch_mic_close(SwitchMic *mic);

Result switch_mic_start(SwitchMic *mic);
Result switch_mic_stop(SwitchMic *mic);
Result switch_mic_append(SwitchMic *mic, AudioInBuffer *buffer);
Result switch_mic_wait(SwitchMic *mic, AudioInBuffer **released,
                       u32 *released_count, u64 timeout_ns);
Result switch_mic_external_available(SwitchMic *mic, bool *available);

#endif

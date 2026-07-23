/* OpenSL ES compatibility layer for Drastic.
 *
 * Drastic uses Android's push-style simple buffer queue.  This implementation
 * preserves that ABI and feeds libnx audren from a small page-aligned ring.
 * Recording queues are serviced with silence: retail Switch hardware has no
 * built-in microphone, while a deterministic silent source keeps games and
 * Drastic's white-noise microphone mode functional.
 */

#include <switch.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "opensles.h"
#include "pthr.h"

#define OSL_QUEUE_CAPACITY 16
#define OSL_WAVEBUFS 4
#define OSL_BLOCK_CAPACITY 0x10000

typedef enum {
  OSL_OBJECT_ENGINE,
  OSL_OBJECT_OUTPUT_MIX,
  OSL_OBJECT_PLAYER,
  OSL_OBJECT_RECORDER,
} OslObjectKind;

typedef struct OslObject OslObject;
typedef struct OslAudioObject OslAudioObject;

typedef struct {
  const void *vtable;
  OslObject *owner;
} OslInterface;

struct OslObject {
  const struct SLObjectItf_ *object_vtable;
  OslObjectKind kind;
  SLuint32 state;
  OslInterface engine;
};

typedef struct {
  const void *data;
  SLuint32 size;
} OslPacket;

struct OslAudioObject {
  OslObject base;
  OslInterface play;
  OslInterface record;
  OslInterface queue;
  OslInterface volume;

  Mutex lock;
  CondVar wake;
  pthread_t thread;
  int thread_started;
  volatile int running;

  SLuint32 io_state;
  SLuint32 sample_rate;
  SLuint32 channels;
  SLuint32 bits;
  SLmillibel volume_mb;
  SLboolean muted;

  OslPacket packets[OSL_QUEUE_CAPACITY];
  unsigned queue_capacity;
  unsigned packet_head;
  unsigned packet_tail;
  unsigned packet_count;
  unsigned completed_count;
  void (*queue_callback)(void *caller, void *context);
  void *queue_context;

  int audren_inited;
  AudioDriver driver;
  int mempool_id;
  int voice_id;
  void *ring;
  size_t ring_size;
  AudioDriverWaveBuf wavebufs[OSL_WAVEBUFS];
  unsigned wave_pending[OSL_WAVEBUFS];
  unsigned next_wave;

};

static OslAudioObject *g_player;
static OslAudioObject *g_recorder;
static unsigned g_master_volume = 100;

static const struct SLInterfaceID_ iid_engine = { 1, 1, 1, 1, {1,1,1,1,1,1} };
static const struct SLInterfaceID_ iid_play = { 2, 2, 2, 2, {2,2,2,2,2,2} };
static const struct SLInterfaceID_ iid_bufferqueue = { 3, 3, 3, 3, {3,3,3,3,3,3} };
static const struct SLInterfaceID_ iid_androidqueue = { 4, 4, 4, 4, {4,4,4,4,4,4} };
static const struct SLInterfaceID_ iid_volume = { 5, 5, 5, 5, {5,5,5,5,5,5} };
static const struct SLInterfaceID_ iid_record = { 6, 6, 6, 6, {6,6,6,6,6,6} };

const SLInterfaceID SL_IID_ENGINE = &iid_engine;
const SLInterfaceID SL_IID_PLAY = &iid_play;
const SLInterfaceID SL_IID_BUFFERQUEUE = &iid_bufferqueue;
const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &iid_androidqueue;
const SLInterfaceID SL_IID_VOLUME = &iid_volume;
const SLInterfaceID SL_IID_RECORD = &iid_record;

static const AudioRendererConfig k_audio_config = {
  .output_rate = AudioRendererOutputRate_48kHz,
  /* libnx allocates the voice-channel pool from num_voices as well. A stereo
   * Drastic voice needs at least two entries; four leaves safe driver headroom. */
  .num_voices = 4,
  .num_effects = 0,
  .num_sinks = 1,
  .num_mix_objs = 1,
  .num_mix_buffers = 2,
};

static OslInterface *interface_from_self(const void *self) {
  return (OslInterface *)self;
}

static OslAudioObject *audio_from_self(const void *self) {
  OslInterface *interface = interface_from_self(self);
  return (OslAudioObject *)interface->owner;
}

static float player_gain(const OslAudioObject *audio) {
  if (audio->muted || g_master_volume == 0) return 0.0f;
  float gain = powf(10.0f, (float)audio->volume_mb / 2000.0f);
  gain *= (float)g_master_volume / 100.0f;
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 1.0f) gain = 1.0f;
  return gain;
}

static void player_update_gain(OslAudioObject *audio) {
  if (!audio || !audio->audren_inited) return;
  mutexLock(&audio->lock);
  audrvVoiceSetVolume(&audio->driver, audio->voice_id, player_gain(audio));
  audrvUpdate(&audio->driver);
  mutexUnlock(&audio->lock);
}

void opensles_set_master_volume(unsigned percent) {
  if (percent > 100) percent = 100;
  g_master_volume = percent;
  player_update_gain(g_player);
}

static int player_voice_init(OslAudioObject *audio) {
  const int voice_ok = audrvVoiceInit(
      &audio->driver, audio->voice_id, (int)audio->channels,
      PcmFormat_Int16, (int)audio->sample_rate);
  if (!voice_ok)
    return 0;
  audrvVoiceSetDestinationMix(&audio->driver, audio->voice_id,
                              AUDREN_FINAL_MIX_ID);
  if (audio->channels == 1) {
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 1.0f, 0, 1);
  } else {
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 1.0f, 1, 1);
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&audio->driver, audio->voice_id, 0.0f, 0, 1);
  }
  audrvVoiceSetVolume(&audio->driver, audio->voice_id, player_gain(audio));
  audrvVoiceStart(&audio->driver, audio->voice_id);
  audrvVoiceSetPaused(&audio->driver, audio->voice_id,
                      audio->io_state != SL_PLAYSTATE_PLAYING);
  const Result rc = audrvUpdate(&audio->driver);
  return R_SUCCEEDED(rc);
}

static int player_backend_init(OslAudioObject *audio) {
  Result rc = audrenInitialize(&k_audio_config);
  if (R_FAILED(rc)) return 0;
  audio->audren_inited = 1;
  rc = audrvCreate(&audio->driver, &k_audio_config, 2);
  if (R_FAILED(rc)) goto fail_audren;

  audio->ring_size = (OSL_WAVEBUFS * OSL_BLOCK_CAPACITY + 0xfff) & ~0xfff;
  audio->ring = aligned_alloc(0x1000, audio->ring_size);
  if (!audio->ring) goto fail_driver;
  memset(audio->ring, 0, audio->ring_size);
  armDCacheFlush(audio->ring, audio->ring_size);

  audio->mempool_id = audrvMemPoolAdd(&audio->driver, audio->ring,
                                       audio->ring_size);
  if (audio->mempool_id < 0) goto fail_ring;
  if (!audrvMemPoolAttach(&audio->driver, audio->mempool_id))
    goto fail_pool;
  static const u8 sink_channels[] = { 0, 1 };
  const int sink_id = audrvDeviceSinkAdd(
      &audio->driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels);
  if (sink_id < 0)
    goto fail_pool;

  /* Commit the mempool and sink before allocating the stereo voice. */
  rc = audrvUpdate(&audio->driver);
  if (R_FAILED(rc)) goto fail_pool;

  audio->voice_id = 0;
  if (!player_voice_init(audio)) goto fail_pool;
  rc = audrenStartAudioRenderer();
  if (R_FAILED(rc)) goto fail_pool;
  return 1;

fail_pool:
  if (audio->mempool_id >= 0) {
    audrvMemPoolDetach(&audio->driver, audio->mempool_id);
    audrvMemPoolRemove(&audio->driver, audio->mempool_id);
  }
fail_ring:
  free(audio->ring);
  audio->ring = NULL;
fail_driver:
  audrvClose(&audio->driver);
fail_audren:
  audrenExit();
  audio->audren_inited = 0;
  return 0;
}

static int wave_is_free(const AudioDriverWaveBuf *wave) {
  return wave->state == AudioDriverWaveBufState_Free ||
         wave->state == AudioDriverWaveBufState_Done;
}

static void queue_invoke_callback(OslAudioObject *audio) {
  void (*callback)(void *, void *) = audio->queue_callback;
  void *context = audio->queue_context;
  if (callback)
    callback((void *)&audio->queue.vtable, context);
}

static unsigned player_pending_count_locked(const OslAudioObject *audio) {
  unsigned count = 0;
  for (unsigned index = 0; index < OSL_WAVEBUFS; index++)
    count += audio->wave_pending[index] != 0;
  return count;
}

static unsigned queue_outstanding_locked(const OslAudioObject *audio) {
  return audio->packet_count + player_pending_count_locked(audio);
}

static void *player_thread(void *argument) {
  OslAudioObject *audio = (OslAudioObject *)argument;
  pthr_ensure_fake_tls();
  pthr_pin_bg_core();

  while (__atomic_load_n(&audio->running, __ATOMIC_ACQUIRE)) {
    int invoke_callback = 0;
    mutexLock(&audio->lock);
    audrvUpdate(&audio->driver);

    for (unsigned index = 0; index < OSL_WAVEBUFS; index++) {
      AudioDriverWaveBuf *wave = &audio->wavebufs[index];
      if (audio->wave_pending[index] &&
          wave->state == AudioDriverWaveBufState_Done) {
        audio->wave_pending[index] = 0;
        audio->completed_count++;
        invoke_callback = 1;
        break;
      }
    }

    if (audio->io_state == SL_PLAYSTATE_PLAYING && audio->packet_count) {
      unsigned index = audio->next_wave;
      unsigned searched = 0;
      while (searched < OSL_WAVEBUFS &&
             (audio->wave_pending[index] ||
              !wave_is_free(&audio->wavebufs[index]))) {
        index = (index + 1) % OSL_WAVEBUFS;
        searched++;
      }

      if (searched < OSL_WAVEBUFS) {
        AudioDriverWaveBuf *wave = &audio->wavebufs[index];
        OslPacket packet = audio->packets[audio->packet_head];
        audio->packet_head = (audio->packet_head + 1) % OSL_QUEUE_CAPACITY;
        audio->packet_count--;

        size_t size = packet.size;
        if (size > OSL_BLOCK_CAPACITY) size = OSL_BLOCK_CAPACITY;
        const size_t frame_bytes = audio->channels * (audio->bits / 8);
        if (frame_bytes) size -= size % frame_bytes;
        void *block = (char *)audio->ring + index * OSL_BLOCK_CAPACITY;
        memcpy(block, packet.data, size);
        armDCacheFlush(block, size);

        memset(wave, 0, sizeof(*wave));
        wave->data_raw = audio->ring;
        wave->size = audio->ring_size;
        wave->start_sample_offset =
            (s32)((index * OSL_BLOCK_CAPACITY) / frame_bytes);
        wave->end_sample_offset = wave->start_sample_offset +
                                  (s32)(size / frame_bytes);
        if (audrvVoiceAddWaveBuf(&audio->driver, audio->voice_id, wave)) {
          audio->wave_pending[index] = 1;
          audio->next_wave = (index + 1) % OSL_WAVEBUFS;
          audrvUpdate(&audio->driver);
        } else {
          audio->packet_head = (audio->packet_head + OSL_QUEUE_CAPACITY - 1) %
                               OSL_QUEUE_CAPACITY;
          audio->packets[audio->packet_head] = packet;
          audio->packet_count++;
        }
      }
    }
    mutexUnlock(&audio->lock);

    if (invoke_callback)
      queue_invoke_callback(audio);

    audrenWaitFrame();
  }
  return NULL;
}

static void *recorder_thread(void *argument) {
  OslAudioObject *audio = (OslAudioObject *)argument;
  pthr_ensure_fake_tls();
  pthr_pin_bg_core();

  while (__atomic_load_n(&audio->running, __ATOMIC_ACQUIRE)) {
    OslPacket packet = {0};
    int have_packet = 0;
    mutexLock(&audio->lock);
    if (audio->io_state == SL_RECORDSTATE_RECORDING && audio->packet_count) {
      packet = audio->packets[audio->packet_head];
      audio->packet_head = (audio->packet_head + 1) % OSL_QUEUE_CAPACITY;
      audio->packet_count--;
      have_packet = 1;
    }
    mutexUnlock(&audio->lock);

    if (!have_packet) {
      svcSleepThread(2000000ULL);
      continue;
    }
    memset((void *)packet.data, 0, packet.size);
    size_t frame_bytes = audio->channels * (audio->bits / 8);
    size_t frames = frame_bytes ? packet.size / frame_bytes : 0;
    if (audio->sample_rate && frames)
      svcSleepThread((u64)frames * 1000000000ULL / audio->sample_rate);
    audio->completed_count++;
    queue_invoke_callback(audio);
  }
  return NULL;
}

static void audio_backend_shutdown(OslAudioObject *audio) {
  if (!audio) return;
  __atomic_store_n(&audio->running, 0, __ATOMIC_RELEASE);
  condvarWakeAll(&audio->wake);
  if (audio->thread_started) {
    pthread_join(audio->thread, NULL);
    audio->thread_started = 0;
  }
  if (audio->audren_inited) {
    audrenStopAudioRenderer();
    audrvVoiceDrop(&audio->driver, audio->voice_id);
    if (audio->mempool_id >= 0) {
      audrvMemPoolDetach(&audio->driver, audio->mempool_id);
      audrvMemPoolRemove(&audio->driver, audio->mempool_id);
    }
    audrvClose(&audio->driver);
    audrenExit();
    audio->audren_inited = 0;
  }
  free(audio->ring);
  audio->ring = NULL;
}

static SLresult object_realize(SLObjectItf self, SLboolean async) {
  (void)async;
  OslObject *object = (OslObject *)self;
  object->state = SL_OBJECT_STATE_REALIZED;
  return SL_RESULT_SUCCESS;
}

static SLresult object_resume(SLObjectItf self, SLboolean async) {
  return object_realize(self, async);
}

static SLresult object_get_state(SLObjectItf self, SLuint32 *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  *state = ((OslObject *)self)->state;
  return SL_RESULT_SUCCESS;
}

static SLresult object_get_interface(SLObjectItf self, const SLInterfaceID iid,
                                     void *result) {
  if (!result || !iid) return SL_RESULT_PARAMETER_INVALID;
  OslObject *object = (OslObject *)self;
  OslInterface *interface = NULL;
  if (object->kind == OSL_OBJECT_ENGINE && iid == SL_IID_ENGINE)
    interface = &object->engine;
  else if (object->kind == OSL_OBJECT_PLAYER) {
    OslAudioObject *audio = (OslAudioObject *)object;
    if (iid == SL_IID_PLAY) interface = &audio->play;
    else if (iid == SL_IID_BUFFERQUEUE ||
             iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) interface = &audio->queue;
    else if (iid == SL_IID_VOLUME) interface = &audio->volume;
  } else if (object->kind == OSL_OBJECT_RECORDER) {
    OslAudioObject *audio = (OslAudioObject *)object;
    if (iid == SL_IID_RECORD) interface = &audio->record;
    else if (iid == SL_IID_BUFFERQUEUE ||
             iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) interface = &audio->queue;
  }
  if (!interface) return SL_RESULT_FEATURE_UNSUPPORTED;
  *(void **)result = (void *)&interface->vtable;
  return SL_RESULT_SUCCESS;
}

static SLresult object_register_callback(SLObjectItf self,
                                         slObjectCallback callback,
                                         void *context) {
  (void)self; (void)callback; (void)context;
  return SL_RESULT_SUCCESS;
}

static void object_abort(SLObjectItf self) { (void)self; }

static void object_destroy(SLObjectItf self) {
  OslObject *object = (OslObject *)self;
  if (object->kind == OSL_OBJECT_PLAYER || object->kind == OSL_OBJECT_RECORDER) {
    OslAudioObject *audio = (OslAudioObject *)object;
    if (g_player == audio) g_player = NULL;
    if (g_recorder == audio) g_recorder = NULL;
    audio_backend_shutdown(audio);
  }
  free(object);
}

static SLresult object_set_priority(SLObjectItf self, SLint32 priority,
                                    SLboolean preemptable) {
  (void)self; (void)priority; (void)preemptable;
  return SL_RESULT_SUCCESS;
}

static SLresult object_get_priority(SLObjectItf self, SLint32 *priority,
                                    SLboolean *preemptable) {
  (void)self;
  if (priority) *priority = 0;
  if (preemptable) *preemptable = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}

static SLresult object_set_loss(SLObjectItf self, SLint16 count,
                                SLInterfaceID *ids, SLboolean enabled) {
  (void)self; (void)count; (void)ids; (void)enabled;
  return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ object_vtable = {
  object_realize, object_resume, object_get_state, object_get_interface,
  object_register_callback, object_abort, object_destroy, object_set_priority,
  object_get_priority, object_set_loss,
};

static void audio_format_from_source(OslAudioObject *audio,
                                     const SLDataSource *source,
                                     const SLDataSink *sink) {
  const SLDataFormat_PCM *format = NULL;
  if (source && source->pFormat) format = (const SLDataFormat_PCM *)source->pFormat;
  if (!format && sink && sink->pFormat) format = (const SLDataFormat_PCM *)sink->pFormat;
  audio->sample_rate = 44100;
  audio->channels = 2;
  audio->bits = 16;
  audio->queue_capacity = OSL_QUEUE_CAPACITY;

  /* Android's numBuffers limits the complete queue, including buffers already
   * handed to the device. Drastic relies on Enqueue returning
   * BUFFER_INSUFFICIENT when that total is full to arbitrate its generated
   * audio and internally generated-silence paths. */
  const void *locators[] = {
      source ? source->pLocator : NULL,
      sink ? sink->pLocator : NULL,
  };
  for (unsigned index = 0; index < 2; index++) {
    const SLuint32 *locator = (const SLuint32 *)locators[index];
    if (!locator)
      continue;
    if ((locator[0] == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ||
         locator[0] == SL_DATALOCATOR_BUFFERQUEUE) &&
        locator[1] > 0) {
      audio->queue_capacity = locator[1] < OSL_QUEUE_CAPACITY
                                  ? locator[1]
                                  : OSL_QUEUE_CAPACITY;
      break;
    }
  }
  if (format && format->formatType == SL_DATAFORMAT_PCM) {
    if (format->samplesPerSec) audio->sample_rate = format->samplesPerSec / 1000;
    if (format->numChannels >= 1 && format->numChannels <= 2)
      audio->channels = format->numChannels;
    if (format->bitsPerSample == 8 || format->bitsPerSample == 16)
      audio->bits = format->bitsPerSample;
  }
}

static OslAudioObject *audio_object_create(OslObjectKind kind) {
  OslAudioObject *audio = calloc(1, sizeof(*audio));
  if (!audio) return NULL;
  audio->base.object_vtable = &object_vtable;
  audio->base.kind = kind;
  audio->base.state = SL_OBJECT_STATE_UNREALIZED;
  audio->play.owner = &audio->base;
  audio->record.owner = &audio->base;
  audio->queue.owner = &audio->base;
  audio->volume.owner = &audio->base;
  audio->volume_mb = 0;
  audio->mempool_id = -1;
  mutexInit(&audio->lock);
  condvarInit(&audio->wake);
  return audio;
}

static SLresult play_set_state(SLPlayItf self, SLuint32 state) {
  OslAudioObject *audio = audio_from_self(self);
  mutexLock(&audio->lock);
  audio->io_state = state;
  if (audio->audren_inited) {
    audrvVoiceSetPaused(&audio->driver, audio->voice_id,
                        state != SL_PLAYSTATE_PLAYING);
    audrvUpdate(&audio->driver);
  }
  condvarWakeAll(&audio->wake);
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult play_get_state(SLPlayItf self, SLuint32 *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  *state = audio_from_self(self)->io_state;
  return SL_RESULT_SUCCESS;
}

static SLresult play_get_duration(SLPlayItf self, SLmillisecond *value) {
  (void)self;
  if (value) *value = SL_TIME_UNKNOWN;
  return SL_RESULT_SUCCESS;
}

static SLresult play_get_position(SLPlayItf self, SLmillisecond *value) {
  (void)self;
  if (value) *value = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult play_register(SLPlayItf self, slPlayCallback callback,
                              void *context) {
  (void)self; (void)callback; (void)context;
  return SL_RESULT_SUCCESS;
}

static SLresult play_set_mask(SLPlayItf self, SLuint32 value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult play_get_mask(SLPlayItf self, SLuint32 *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}
static SLresult play_set_marker(SLPlayItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult play_clear_marker(SLPlayItf self) {
  (void)self; return SL_RESULT_SUCCESS;
}
static SLresult play_get_marker(SLPlayItf self, SLmillisecond *value) {
  (void)self; if (value) *value = SL_TIME_UNKNOWN; return SL_RESULT_SUCCESS;
}
static SLresult play_set_period(SLPlayItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult play_get_period(SLPlayItf self, SLmillisecond *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ play_vtable = {
  play_set_state, play_get_state, play_get_duration, play_get_position,
  play_register, play_set_mask, play_get_mask, play_set_marker,
  play_clear_marker, play_get_marker, play_set_period, play_get_period,
};

static SLresult record_set_state(SLRecordItf self, SLuint32 state) {
  audio_from_self(self)->io_state = state;
  return SL_RESULT_SUCCESS;
}

static SLresult record_get_state(SLRecordItf self, SLuint32 *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  *state = audio_from_self(self)->io_state;
  return SL_RESULT_SUCCESS;
}

static SLresult record_duration(SLRecordItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult record_position(SLRecordItf self, SLmillisecond *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}
static SLresult record_register(SLRecordItf self, slRecordCallback callback,
                                void *context) {
  (void)self; (void)callback; (void)context; return SL_RESULT_SUCCESS;
}
static SLresult record_set_mask(SLRecordItf self, SLuint32 value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult record_get_mask(SLRecordItf self, SLuint32 *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}
static SLresult record_set_marker(SLRecordItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult record_clear_marker(SLRecordItf self) {
  (void)self; return SL_RESULT_SUCCESS;
}
static SLresult record_get_marker(SLRecordItf self, SLmillisecond *value) {
  (void)self; if (value) *value = SL_TIME_UNKNOWN; return SL_RESULT_SUCCESS;
}
static SLresult record_set_period(SLRecordItf self, SLmillisecond value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult record_get_period(SLRecordItf self, SLmillisecond *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}

static const struct SLRecordItf_ record_vtable = {
  record_set_state, record_get_state, record_duration, record_position,
  record_register, record_set_mask, record_get_mask, record_set_marker,
  record_clear_marker, record_get_marker, record_set_period, record_get_period,
};

static SLresult queue_enqueue_common(OslAudioObject *audio, const void *buffer,
                                     SLuint32 size) {
  if (!buffer || !size) return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&audio->lock);
  const unsigned outstanding = queue_outstanding_locked(audio);
  if (outstanding >= audio->queue_capacity) {
    mutexUnlock(&audio->lock);
    return SL_RESULT_BUFFER_INSUFFICIENT;
  }
  audio->packets[audio->packet_tail].data = buffer;
  audio->packets[audio->packet_tail].size = size;
  audio->packet_tail = (audio->packet_tail + 1) % OSL_QUEUE_CAPACITY;
  audio->packet_count++;
  condvarWakeAll(&audio->wake);
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_enqueue(SLBufferQueueItf self, const void *buffer,
                              SLuint32 size) {
  return queue_enqueue_common(audio_from_self(self), buffer, size);
}

static SLresult queue_clear(SLBufferQueueItf self) {
  OslAudioObject *audio = audio_from_self(self);
  mutexLock(&audio->lock);
  audio->packet_head = audio->packet_tail = audio->packet_count = 0;
  if (audio->audren_inited) {
    audrvVoiceDrop(&audio->driver, audio->voice_id);
    memset(audio->wavebufs, 0, sizeof(audio->wavebufs));
    memset(audio->wave_pending, 0, sizeof(audio->wave_pending));
    player_voice_init(audio);
  }
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_get_state(SLBufferQueueItf self,
                                SLBufferQueueState *state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  OslAudioObject *audio = audio_from_self(self);
  mutexLock(&audio->lock);
  unsigned in_flight = 0;
  for (unsigned index = 0; index < OSL_WAVEBUFS; index++)
    in_flight += audio->wave_pending[index] != 0;
  state->count = audio->packet_count + in_flight;
  state->playIndex = audio->completed_count;
  mutexUnlock(&audio->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_register(SLBufferQueueItf self,
                               slBufferQueueCallback callback,
                               void *context) {
  OslAudioObject *audio = audio_from_self(self);
  audio->queue_callback = (void (*)(void *, void *))callback;
  audio->queue_context = context;
  return SL_RESULT_SUCCESS;
}

static const struct SLBufferQueueItf_ queue_vtable = {
  queue_enqueue, queue_clear, queue_get_state, queue_register,
};

static SLresult volume_set(SLVolumeItf self, SLmillibel level) {
  OslAudioObject *audio = audio_from_self(self);
  audio->volume_mb = level;
  player_update_gain(audio);
  return SL_RESULT_SUCCESS;
}
static SLresult volume_get(SLVolumeItf self, SLmillibel *level) {
  if (!level) return SL_RESULT_PARAMETER_INVALID;
  *level = audio_from_self(self)->volume_mb;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_max(SLVolumeItf self, SLmillibel *level) {
  (void)self; if (level) *level = 0; return SL_RESULT_SUCCESS;
}
static SLresult volume_set_mute(SLVolumeItf self, SLboolean mute) {
  OslAudioObject *audio = audio_from_self(self);
  audio->muted = mute;
  player_update_gain(audio);
  return SL_RESULT_SUCCESS;
}
static SLresult volume_get_mute(SLVolumeItf self, SLboolean *mute) {
  if (!mute) return SL_RESULT_PARAMETER_INVALID;
  *mute = audio_from_self(self)->muted;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_stereo_enable(SLVolumeItf self, SLboolean enabled) {
  (void)self; (void)enabled; return SL_RESULT_SUCCESS;
}
static SLresult volume_stereo_is_enabled(SLVolumeItf self, SLboolean *enabled) {
  (void)self; if (enabled) *enabled = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS;
}
static SLresult volume_stereo_set(SLVolumeItf self, SLpermille value) {
  (void)self; (void)value; return SL_RESULT_SUCCESS;
}
static SLresult volume_stereo_get(SLVolumeItf self, SLpermille *value) {
  (void)self; if (value) *value = 0; return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ volume_vtable = {
  volume_set, volume_get, volume_max, volume_set_mute, volume_get_mute,
  volume_stereo_enable, volume_stereo_is_enabled, volume_stereo_set,
  volume_stereo_get,
};

static SLresult engine_create_player(SLEngineItf self, SLObjectItf *result,
                                     SLDataSource *source, SLDataSink *sink,
                                     SLuint32 count, const SLInterfaceID *ids,
                                     const SLboolean *required) {
  (void)self; (void)count; (void)ids; (void)required;
  if (!result) return SL_RESULT_PARAMETER_INVALID;
  *result = NULL;
  if (g_player) return SL_RESULT_RESOURCE_ERROR;
  OslAudioObject *audio = audio_object_create(OSL_OBJECT_PLAYER);
  if (!audio) return SL_RESULT_MEMORY_FAILURE;
  audio_format_from_source(audio, source, sink);
  if (audio->bits != 16) audio->bits = 16;
  audio->play.vtable = &play_vtable;
  audio->queue.vtable = &queue_vtable;
  audio->volume.vtable = &volume_vtable;
  audio->io_state = SL_PLAYSTATE_STOPPED;
  if (!player_backend_init(audio)) {
    free(audio);
    return SL_RESULT_RESOURCE_ERROR;
  }
  audio->running = 1;
  if (pthread_create(&audio->thread, NULL, player_thread, audio) != 0) {
    audio_backend_shutdown(audio);
    free(audio);
    return SL_RESULT_RESOURCE_ERROR;
  }
  audio->thread_started = 1;
  g_player = audio;
  *result = (SLObjectItf)&audio->base.object_vtable;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_create_recorder(SLEngineItf self, SLObjectItf *result,
                                       SLDataSource *source, SLDataSink *sink,
                                       SLuint32 count, const SLInterfaceID *ids,
                                       const SLboolean *required) {
  (void)self; (void)count; (void)ids; (void)required;
  if (!result) return SL_RESULT_PARAMETER_INVALID;
  if (g_recorder) return SL_RESULT_RESOURCE_ERROR;
  OslAudioObject *audio = audio_object_create(OSL_OBJECT_RECORDER);
  if (!audio) return SL_RESULT_MEMORY_FAILURE;
  audio_format_from_source(audio, source, sink);
  audio->record.vtable = &record_vtable;
  audio->queue.vtable = &queue_vtable;
  audio->io_state = SL_RECORDSTATE_STOPPED;
  audio->running = 1;
  if (pthread_create(&audio->thread, NULL, recorder_thread, audio) != 0) {
    free(audio);
    return SL_RESULT_RESOURCE_ERROR;
  }
  audio->thread_started = 1;
  g_recorder = audio;
  *result = (SLObjectItf)&audio->base.object_vtable;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_create_output_mix(SLEngineItf self, SLObjectItf *result,
                                         SLuint32 count,
                                         const SLInterfaceID *ids,
                                         const SLboolean *required) {
  (void)self; (void)count; (void)ids; (void)required;
  if (!result) return SL_RESULT_PARAMETER_INVALID;
  OslObject *object = calloc(1, sizeof(*object));
  if (!object) return SL_RESULT_MEMORY_FAILURE;
  object->object_vtable = &object_vtable;
  object->kind = OSL_OBJECT_OUTPUT_MIX;
  object->state = SL_OBJECT_STATE_UNREALIZED;
  *result = (SLObjectItf)&object->object_vtable;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult engine_num_interfaces(SLEngineItf self, SLuint32 object_id,
                                      SLuint32 *count) {
  (void)self; (void)object_id; if (count) *count = 0; return SL_RESULT_SUCCESS;
}
static SLresult engine_supported_interface(SLEngineItf self, SLuint32 object_id,
                                           SLuint32 index,
                                           SLInterfaceID *iid) {
  (void)self; (void)object_id; (void)index; (void)iid;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult engine_num_extensions(SLEngineItf self, SLuint32 *count) {
  (void)self; if (count) *count = 0; return SL_RESULT_SUCCESS;
}
static SLresult engine_get_extension(SLEngineItf self, SLuint32 index,
                                     SLchar *name, SLint16 *length) {
  (void)self; (void)index; (void)name; (void)length;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult engine_extension_supported(SLEngineItf self,
                                           const SLchar *name,
                                           SLboolean *supported) {
  (void)self; (void)name; if (supported) *supported = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ engine_vtable = {
  (void *)engine_unsupported,
  (void *)engine_unsupported,
  engine_create_player,
  engine_create_recorder,
  (void *)engine_unsupported,
  (void *)engine_unsupported,
  (void *)engine_unsupported,
  engine_create_output_mix,
  (void *)engine_unsupported,
  (void *)engine_unsupported,
  engine_num_interfaces,
  engine_supported_interface,
  engine_num_extensions,
  engine_get_extension,
  engine_extension_supported,
};

SLresult SLAPIENTRY slCreateEngine(SLObjectItf *result, SLuint32 option_count,
                                   const SLEngineOption *options,
                                   SLuint32 interface_count,
                                   const SLInterfaceID *interface_ids,
                                   const SLboolean *required) {
  (void)option_count; (void)options; (void)interface_count;
  (void)interface_ids; (void)required;
  if (!result) return SL_RESULT_PARAMETER_INVALID;
  OslObject *engine = calloc(1, sizeof(*engine));
  if (!engine) return SL_RESULT_MEMORY_FAILURE;
  engine->object_vtable = &object_vtable;
  engine->kind = OSL_OBJECT_ENGINE;
  engine->state = SL_OBJECT_STATE_UNREALIZED;
  engine->engine.vtable = &engine_vtable;
  engine->engine.owner = engine;
  *result = (SLObjectItf)&engine->object_vtable;
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  if (g_recorder) {
    OslAudioObject *audio = g_recorder;
    g_recorder = NULL;
    audio_backend_shutdown(audio);
  }
  if (g_player) {
    OslAudioObject *audio = g_player;
    g_player = NULL;
    audio_backend_shutdown(audio);
  }
}

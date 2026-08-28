/* Minimal OpenSL ES buffer-queue audio shim for Unity/CRIWARE.
 * Distributed under the MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "opensles.h"
#include "util.h"

#define SL_RESULT_SUCCESS              0
#define SL_RESULT_PRECONDITIONS_VIOLATED 0x01
#define SL_RESULT_PARAMETER_INVALID    0x02
#define SL_RESULT_MEMORY_FAILURE       0x03
#define SL_RESULT_RESOURCE_ERROR       0x04
#define SL_RESULT_CONTENT_UNSUPPORTED  0x09
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

#define SL_DATAFORMAT_PCM                    0x00000002u
#define SL_ANDROID_DATAFORMAT_PCM_EX         0x00000004u
#define SL_DATALOCATOR_BUFFERQUEUE           0x00000006u
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BDu
#define SL_DATALOCATOR_OUTPUTMIX             0x00000004u
#define SL_BYTEORDER_LITTLEENDIAN             0x00000002u
#define SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT   0x00000001u
#define SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT 0x00000002u
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT        0x00000003u

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint16_t SLuint16;
typedef int16_t  SLint16;
typedef uint8_t  SLuint8;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int32_t  SLmillibel;

/* samplesPerSec uses milliHertz. */
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  SLuint32 locatorType;
  void *outputMix;
} SLDataLocator_OutputMix;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef void *SLObjectItf;       // -> &obj->obj_vt
typedef void *SLInterfaceID;

typedef void (*slBufferQueueCallback)(void *caller, void *context);

#define DEF_IID(n) void *SL_IID_##n = &SL_IID_##n
DEF_IID(3DCOMMIT); DEF_IID(3DDOPPLER); DEF_IID(3DGROUPING); DEF_IID(3DLOCATION);
DEF_IID(3DMACROSCOPIC); DEF_IID(3DSOURCE); DEF_IID(ANDROIDCONFIGURATION);
DEF_IID(ANDROIDACOUSTICECHOCANCELLATION); DEF_IID(ANDROIDAUTOMATICGAINCONTROL);
DEF_IID(ANDROIDNOISESUPPRESSION);
DEF_IID(ANDROIDEFFECT); DEF_IID(ANDROIDEFFECTCAPABILITIES); DEF_IID(ANDROIDEFFECTSEND);
DEF_IID(ANDROIDSIMPLEBUFFERQUEUE); DEF_IID(AUDIODECODERCAPABILITIES); DEF_IID(AUDIOENCODER);
DEF_IID(AUDIOENCODERCAPABILITIES); DEF_IID(AUDIOIODEVICECAPABILITIES); DEF_IID(BASSBOOST);
DEF_IID(BUFFERQUEUE); DEF_IID(DEVICEVOLUME); DEF_IID(DYNAMICINTERFACEMANAGEMENT);
DEF_IID(DYNAMICSOURCE); DEF_IID(EFFECTSEND); DEF_IID(ENGINE); DEF_IID(ENGINECAPABILITIES);
DEF_IID(ENVIRONMENTALREVERB); DEF_IID(EQUALIZER); DEF_IID(LED); DEF_IID(METADATAEXTRACTION);
DEF_IID(METADATATRAVERSAL); DEF_IID(MIDIMESSAGE); DEF_IID(MIDIMUTESOLO); DEF_IID(MIDITEMPO);
DEF_IID(MIDITIME); DEF_IID(MUTESOLO); DEF_IID(NULL); DEF_IID(OBJECT); DEF_IID(OUTPUTMIX);
DEF_IID(PITCH); DEF_IID(PLAY); DEF_IID(PLAYBACKRATE); DEF_IID(PREFETCHSTATUS);
DEF_IID(PRESETREVERB); DEF_IID(RATEPITCH); DEF_IID(RECORD); DEF_IID(SEEK); DEF_IID(THREADSYNC);
DEF_IID(VIBRA); DEF_IID(VIRTUALIZER); DEF_IID(VISUALIZATION); DEF_IID(VOLUME);
#undef DEF_IID

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *pState);
  SLresult (*GetInterface)(void *self, const SLInterfaceID iid, void *pInterface);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(void *self, SLint32 *pPriority);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 n, SLInterfaceID *ids, SLboolean enabled);
} SLObjectItf_;

/* Preserve the OpenSL ES 1.0.1 vtable layout. */
typedef struct {
  void *CreateLEDDevice;
  void *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateAudioRecorder;
  void *CreateMidiPlayer;
  void *CreateListener;
  void *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, SLObjectItf *pMix, SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateMetadataExtractor;
  void *CreateExtensionObject;
  void *QueryNumSupportedInterfaces;
  void *QuerySupportedInterfaces;
  void *QueryNumSupportedExtensions;
  void *QuerySupportedExtension;
  void *IsExtensionSupported;
} SLEngineItf_;

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLuint32 *pMsec);
  SLresult (*GetPosition)(void *self, SLuint32 *pMsec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 m);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *p);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 m);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *p);
} SLPlayItf_;

typedef struct {
  SLresult (*Enqueue)(void *self, const void *pBuffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *pState);
  SLresult (*RegisterCallback)(void *self, slBufferQueueCallback cb, void *ctx);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *p);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *p);
  SLresult (*SetStereoPosition)(void *self, SLint32 perMille);
  SLresult (*GetStereoPosition)(void *self, SLint32 *p);
} SLVolumeItf_;

/* Playback-rate changes are accepted but ignored. */
typedef struct {
  SLresult (*SetRate)(void *self, SLint16 rate);
  SLresult (*GetRate)(void *self, SLint16 *p);
  SLresult (*SetPropertyConstraints)(void *self, SLuint32 c);
  SLresult (*GetProperties)(void *self, SLuint32 *p);
  SLresult (*GetCapabilitiesOfRate)(void *self, SLuint32 *p);
  SLresult (*GetRateRange)(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop);
} SLPlaybackRateItf_;

typedef struct {
  SLresult (*SetConfiguration)(void *self, const void *key, const void *value, SLuint32 valueSize);
  SLresult (*GetConfiguration)(void *self, const void *key, SLuint32 *pValueSize, void *value);
  SLresult (*AcquireJavaProxy)(void *self, SLuint32 proxyType, void *pProxyObj);
  SLresult (*ReleaseJavaProxy)(void *self, SLuint32 proxyType);
} SLAndroidConfigurationItf_;

#define MAX_PLAYERS 64
/* CRIWARE queues its DSP buffers before playback starts. */
#define BQ_SLOTS 256

typedef struct {
  const void *data;
  SLuint32 size;
} BQBuffer;

typedef struct Player {
  const SLObjectItf_ *obj_vt;
  const SLPlayItf_   *play_vt;
  const SLBufferQueueItf_ *bq_vt;
  const SLVolumeItf_ *vol_vt;
  const SLPlaybackRateItf_ *rate_vt;
  const SLAndroidConfigurationItf_ *config_vt;

  /* refs and destroy_requested are protected by g_reg_lock.  The initial
   * reference belongs to the guest-visible SLObjectItf; an audio snapshot
   * retains an additional reference before dropping the registry lock. */
  unsigned refs;
  int destroy_requested;
  int channels;
  int rate;
  int sbytes;    // container bytes per sample (1..4)
  int sample_bits;
  int representation;
  int is_float;
  SLuint32 play_state;
  SLmillibel volume_mb;
  int muted;
  float gain;
  int starved;
  SLuint32 processed_buffers;

  slBufferQueueCallback cb;
  void *cb_ctx;

  BQBuffer q[BQ_SLOTS];
  int q_head, q_tail; // count = (tail - head + N) % N
  const uint8_t *cur;
  SLuint32 cur_size, cur_pos;
  double cur_fpos; // fractional sample index into cur (for rate conversion)

  SDL_mutex *lock;
} Player;

typedef struct {
  const SLObjectItf_ *obj_vt;
} OutputMix;

typedef struct {
  const SLObjectItf_ *obj_vt;
  const SLEngineItf_ *eng_vt;
} Engine;

#define CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

static SDL_AudioDeviceID g_dev = 0;
static int g_dev_rate = 44100;
static int g_device_closing = 0;
static int g_output_focused = 1;
static Player *g_players[MAX_PLAYERS];
static int g_player_count = 0;
static SDL_mutex *g_reg_lock = NULL;

static uint64_t g_engine_creates;
static uint64_t g_output_mix_creates;
static uint64_t g_player_create_attempts;
static uint64_t g_players_created;
static uint64_t g_player_create_failures;
static uint64_t g_players_destroyed;
static uint64_t g_buffers_enqueued;
static uint64_t g_bytes_enqueued;
static uint64_t g_enqueue_failures;
static uint64_t g_device_callbacks;
static uint64_t g_output_frames;
static uint64_t g_mixed_frames;
static uint64_t g_buffer_callbacks;
static uint64_t g_underruns;
static uint64_t g_clipped_samples;
static uint64_t g_focus_pauses;
static uint64_t g_focus_resumes;
static uint32_t g_device_open_attempts;
static uint32_t g_device_open_failures;
static uint32_t g_device_channels;
static uint32_t g_device_format;
static uint32_t g_device_samples;
static uint32_t g_queue_high_water;

static SDL_mutex *registry_lock(void) {
  return (SDL_mutex *)SDL_AtomicGetPtr((void **)&g_reg_lock);
}

static int ensure_registry_lock(void) {
  if (registry_lock())
    return 1;
  SDL_mutex *created = SDL_CreateMutex();
  if (!created)
    return 0;
  if (!SDL_AtomicCASPtr((void **)&g_reg_lock, NULL, created))
    SDL_DestroyMutex(created);
  return registry_lock() != NULL;
}

static void player_clear_queue_locked(Player *p) {
  memset(p->q, 0, sizeof(p->q));
  p->q_head = p->q_tail = 0;
  p->cur = NULL;
  p->cur_pos = p->cur_size = 0;
  p->cur_fpos = 0.0;
  p->starved = 0;
  p->processed_buffers = 0;
}

static void player_free_storage(Player *p) {
  if (!p)
    return;
  if (p->lock)
    SDL_DestroyMutex(p->lock);
  free(p);
}

/* Registry references are deliberately distinct from queue locking.  This
 * lets the audio thread pin a Player, release g_reg_lock, and then run guest
 * callbacks with neither host mutex held. */
static int player_retain_locked(Player *p) {
  if (!p || p->destroy_requested || p->refs == UINT32_MAX)
    return 0;
  ++p->refs;
  return 1;
}

static void player_release(Player *p) {
  int free_now = 0;
  SDL_mutex *registry = registry_lock();
  SDL_LockMutex(registry);
  if (p && p->refs) {
    --p->refs;
    free_now = (p->refs == 0 && p->destroy_requested);
  }
  SDL_UnlockMutex(registry);
  if (free_now)
    player_free_storage(p);
}

static float mb_to_linear(SLmillibel mb) {
  if (mb <= -9600) return 0.0f;
  return powf(10.0f, (float)mb / 2000.0f); // 100 mB = 1 dB
}

static void update_gain_locked(Player *p) {
  p->gain = p->muted ? 0.0f : mb_to_linear(p->volume_mb);
}

/* Normalize little-endian OpenSL PCM to signed 16-bit without assuming that
 * guest buffers are naturally aligned. Android PCM_EX uses representation 3
 * for float; representation 2 is unsigned integer. */
static inline int32_t read_sample_s16(const void *buf, long k, int sbytes,
                                      int sample_bits, int representation) {
  const uint8_t *sample = (const uint8_t *)buf + (size_t)k * (size_t)sbytes;
  if (representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT) {
    float value = 0.0f;
    memcpy(&value, sample, sizeof value);
    if (!isfinite(value)) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    else if (value < -1.0f) value = -1.0f;
    return (int32_t)(value * 32767.0f);
  }

  uint32_t raw = 0;
  memcpy(&raw, sample, (size_t)sbytes);
  const uint64_t mask = sample_bits == 32 ? UINT32_MAX :
                        ((UINT64_C(1) << sample_bits) - 1);
  raw &= (uint32_t)mask;
  const int64_t midpoint = INT64_C(1) << (sample_bits - 1);
  int64_t value = 0;
  if (representation == SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT) {
    value = (int64_t)raw - midpoint;
  } else {
    value = (raw & (uint32_t)midpoint) ?
      (int64_t)((uint64_t)raw | ~mask) : (int64_t)raw;
  }
  if (sample_bits > 16) value >>= sample_bits - 16;
  else if (sample_bits < 16) value <<= 16 - sample_bits;
  if (value > INT16_MAX) value = INT16_MAX;
  else if (value < INT16_MIN) value = INT16_MIN;
  return (int32_t)value;
}

/* Queue state and the guest-owned current buffer are consumed under p->lock so
 * Clear/Destroy cannot return while the mixer is still reading that buffer.
 * The lock is dropped around every guest callback; the audio snapshot keeps p
 * alive if that callback destroys its own player. */
static int mix_player(Player *p, int32_t *acc, int frames) {
  int mixed = 0;
  SDL_LockMutex(p->lock);
  if (p->destroy_requested || p->play_state != SL_PLAYSTATE_PLAYING) {
    SDL_UnlockMutex(p->lock);
    return 0;
  }

  const float g = p->gain;
  const int stereo = (p->channels >= 2);
  const int sbytes = p->sbytes;
  const int sample_bits = p->sample_bits;
  const int representation = p->representation;
  const int bps = stereo ? sbytes * 2 : sbytes;        // bytes per input frame
  /* Resample each player to the device rate. */
  const double ratio = g_dev_rate > 0 ? (double)p->rate / (double)g_dev_rate : 1.0;

  for (int i = 0; i < frames; i++) {
    for (;;) {
      if (!p->cur) {
        const int have = (p->q_head != p->q_tail);
        BQBuffer b = { NULL, 0 };
        if (have) {
          b = p->q[p->q_head];
          p->q_head = (p->q_head + 1) % BQ_SLOTS;
        }
        if (!have) {
          if (!p->starved) {
            p->starved = 1;
            __atomic_add_fetch(&g_underruns, 1, __ATOMIC_RELAXED);
          }
          SDL_UnlockMutex(p->lock);
          return mixed; // underrun: rest of the block stays silent
        }
        p->cur = b.data;
        p->cur_size = b.size;
        p->starved = 0;
      }
      const long n = (long)(p->cur_size / (SLuint32)bps);
      if (n > 0 && (long)p->cur_fpos < n)
        break; // position is inside the current buffer
      p->cur_fpos -= (double)n;
      if (p->cur_fpos < 0.0) p->cur_fpos = 0.0;
      p->cur = NULL;
      p->cur_size = p->cur_pos = 0;
      ++p->processed_buffers;

      slBufferQueueCallback cb = p->cb;
      void *cb_ctx = p->cb_ctx;
      if (cb) {
        __atomic_add_fetch(&g_buffer_callbacks, 1, __ATOMIC_RELAXED);
        SDL_UnlockMutex(p->lock);
        /* SDL owns this audio thread and expects its host TPIDR_EL0 after the
         * callback returns.  Install the Android-compatible TLS only across
         * the call into the guest callback; all SDL mutex and device work must
        * continue with the original host thread pointer. */
        static _Thread_local uint8_t callback_tls[BIONIC_TLS_SIZE]
          __attribute__((aligned(16)));
        install_bionic_tls(callback_tls);
        cb(&p->bq_vt, cb_ctx);
        (void)restore_bionic_tls();
        SDL_LockMutex(p->lock);
        if (p->destroy_requested ||
            p->play_state != SL_PLAYSTATE_PLAYING) {
          SDL_UnlockMutex(p->lock);
          return mixed;
        }
      }
    }

    const long n = (long)(p->cur_size / (SLuint32)bps);
    const long idx = (long)p->cur_fpos;
    const double frac = p->cur_fpos - (double)idx;
    const void *s = p->cur;
    int32_t l, r;
    if (stereo) {
      const long j0 = idx * 2, j1 = (idx + 1 < n ? idx + 1 : idx) * 2;
      const int32_t l0 = read_sample_s16(s, j0, sbytes, sample_bits,
                                         representation);
      const int32_t l1 = read_sample_s16(s, j1, sbytes, sample_bits,
                                         representation);
      const int32_t r0 = read_sample_s16(s, j0 + 1, sbytes, sample_bits,
                                         representation);
      const int32_t r1 = read_sample_s16(s, j1 + 1, sbytes, sample_bits,
                                         representation);
      l = (int32_t)(l0 * (1.0 - frac) + l1 * frac);
      r = (int32_t)(r0 * (1.0 - frac) + r1 * frac);
    } else {
      const int32_t a = read_sample_s16(s, idx, sbytes, sample_bits,
                                        representation);
      const int32_t b2 = (idx + 1 < n) ?
        read_sample_s16(s, idx + 1, sbytes, sample_bits, representation) : a;
      l = r = (int32_t)(a * (1.0 - frac) + b2 * frac);
    }
    acc[i * 2 + 0] += (int32_t)(l * g);
    acc[i * 2 + 1] += (int32_t)(r * g);
    p->cur_fpos += ratio;
    ++mixed;
  }
  SDL_UnlockMutex(p->lock);
  return mixed;
}

static void SDLCALL audio_callback(void *ud, Uint8 *stream, int len) {
  (void)ud;
  __atomic_add_fetch(&g_device_callbacks, 1, __ATOMIC_RELAXED);

  const int frames = len / 4; // S16 stereo
  static _Thread_local int32_t acc[8192 * 2];
  if (frames > 8192) { memset(stream, 0, len); return; }
  __atomic_add_fetch(&g_output_frames, (uint64_t)frames, __ATOMIC_RELAXED);
  memset(acc, 0, frames * 2 * sizeof(int32_t));

  Player *snapshot[MAX_PLAYERS];
  int snapshot_count = 0;
  SDL_mutex *registry = registry_lock();
  SDL_LockMutex(registry);
  for (int i = 0; i < g_player_count; i++) {
    Player *p = g_players[i];
    if (player_retain_locked(p))
      snapshot[snapshot_count++] = p;
  }
  SDL_UnlockMutex(registry);

  uint64_t mixed_frames = 0;
  for (int i = 0; i < snapshot_count; i++) {
    mixed_frames += (uint64_t)mix_player(snapshot[i], acc, frames);
    player_release(snapshot[i]);
  }
  __atomic_add_fetch(&g_mixed_frames, mixed_frames, __ATOMIC_RELAXED);

  int16_t *out = (int16_t *)stream;
  uint64_t clipped = 0;
  for (int i = 0; i < frames * 2; i++) {
    int32_t v = acc[i];
    if (v > 32767) { v = 32767; ++clipped; }
    else if (v < -32768) { v = -32768; ++clipped; }
    out[i] = (int16_t)v;
  }
  if (clipped)
    __atomic_add_fetch(&g_clipped_samples, clipped, __ATOMIC_RELAXED);
}

static int ensure_device(int rate) {
  (void)rate;
  if (!ensure_registry_lock())
    return 0;
  SDL_mutex *registry = registry_lock();
  SDL_LockMutex(registry);
  if (g_device_closing) {
    SDL_UnlockMutex(registry);
    return 0;
  }
  if (g_dev) {
    SDL_UnlockMutex(registry);
    return 1;
  }
  __atomic_add_fetch(&g_device_open_attempts, 1, __ATOMIC_RELAXED);
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    __atomic_add_fetch(&g_device_open_failures, 1, __ATOMIC_RELAXED);
    SDL_UnlockMutex(registry);
    return 0;
  }
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_callback;
  g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!g_dev) {
    __atomic_add_fetch(&g_device_open_failures, 1, __ATOMIC_RELAXED);
    SDL_UnlockMutex(registry);
    return 0;
  }
  g_dev_rate = have.freq;
  g_device_channels = have.channels;
  g_device_format = have.format;
  g_device_samples = have.samples;
  const SDL_AudioDeviceID device = g_dev;
  const int focused = g_output_focused;
  SDL_UnlockMutex(registry);
  SDL_PauseAudioDevice(device, focused ? 0 : 1);
  return 1;
}

static void update_queue_high_water(uint32_t depth) {
  uint32_t previous = __atomic_load_n(&g_queue_high_water, __ATOMIC_RELAXED);
  while (depth > previous &&
         !__atomic_compare_exchange_n(&g_queue_high_water, &previous, depth, 1,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  const uint32_t frame_bytes = (uint32_t)p->channels * (uint32_t)p->sbytes;
  if (!pBuffer || !size || !frame_bytes || size % frame_bytes) {
    __atomic_add_fetch(&g_enqueue_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_PARAMETER_INVALID;
  }
  SDL_LockMutex(p->lock);
  if (p->destroy_requested) {
    SDL_UnlockMutex(p->lock);
    __atomic_add_fetch(&g_enqueue_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  }
  const int next = (p->q_tail + 1) % BQ_SLOTS;
  if (next == p->q_head) { // full
    SDL_UnlockMutex(p->lock);
    __atomic_add_fetch(&g_enqueue_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_RESOURCE_ERROR;
  }
  p->q[p->q_tail].data = pBuffer;
  p->q[p->q_tail].size = size;
  p->q_tail = next;
  p->starved = 0;
  const uint32_t depth =
    (uint32_t)((p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS) +
    (p->cur ? 1u : 0u);
  SDL_UnlockMutex(p->lock);
  __atomic_add_fetch(&g_buffers_enqueued, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_bytes_enqueued, size, __ATOMIC_RELAXED);
  update_queue_high_water(depth);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  if (p->destroy_requested) {
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  }
  player_clear_queue_locked(p);
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;

static SLresult bq_GetState(void *self, void *pState) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (!pState) return SL_RESULT_PARAMETER_INVALID;
  SLBufferQueueState *st = pState;
  SDL_LockMutex(p->lock);
  st->count = (SLuint32)((p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS) +
              (p->cur ? 1u : 0u);
  st->index = p->processed_buffers;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  if (p->destroy_requested) {
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  }
  p->cb = cb;
  p->cb_ctx = ctx;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ bq_vtable = {
  bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (state != SL_PLAYSTATE_STOPPED && state != SL_PLAYSTATE_PAUSED &&
      state != SL_PLAYSTATE_PLAYING)
    return SL_RESULT_PARAMETER_INVALID;
  SDL_LockMutex(p->lock);
  if (p->destroy_requested) {
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  }
  p->play_state = state;
  if (state == SL_PLAYSTATE_PLAYING) p->starved = 0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (!pState) return SL_RESULT_PARAMETER_INVALID;
  SDL_LockMutex(p->lock);
  *pState = p->play_state;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult play_ret0_u32(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult play_ok_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_ok(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }

static const SLPlayItf_ play_vtable = {
  play_SetPlayState, play_GetPlayState, play_ret0_u32, play_ret0_u32,
  play_RegisterCallback, play_ok_u32, play_ret0_u32, play_ok_u32,
  play_ok, play_ret0_u32, play_ok_u32, play_ret0_u32,
};

static SLresult vol_SetVolumeLevel(void *self, SLmillibel level) {
  Player *p = CONTAINER(self, Player, vol_vt);
  int mb = (int)level;
  if (mb > 0) mb = 0;
  if (mb < -9600) mb = -9600;
  SDL_LockMutex(p->lock);
  if (p->destroy_requested) {
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  }
  p->volume_mb = (SLmillibel)mb;
  update_gain_locked(p);
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(void *self, SLmillibel *level) {
  if (!level) return SL_RESULT_PARAMETER_INVALID;
  Player *p = CONTAINER(self, Player, vol_vt);
  SDL_LockMutex(p->lock);
  *level = p->volume_mb;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMaxVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_SetMute(void *self, SLboolean m) {
  Player *p = CONTAINER(self, Player, vol_vt);
  SDL_LockMutex(p->lock);
  if (p->destroy_requested) {
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  }
  p->muted = m != SL_BOOLEAN_FALSE;
  update_gain_locked(p);
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMute(void *self, SLboolean *mute) {
  if (!mute) return SL_RESULT_PARAMETER_INVALID;
  Player *p = CONTAINER(self, Player, vol_vt);
  SDL_LockMutex(p->lock);
  *mute = p->muted ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_enable(void *self, SLboolean e) { (void)self; (void)e; return SL_RESULT_SUCCESS; }
static SLresult vol_isenabled(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_setpos(void *self, SLint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_getpos(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const SLVolumeItf_ vol_vtable = {
  vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute,
  vol_GetMute, vol_enable, vol_isenabled, vol_setpos, vol_getpos,
};

static SLresult rate_SetRate(void *self, SLint16 r) { (void)self; (void)r; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRate(void *self, SLint16 *p) { (void)self; if (p) *p = 1000; return SL_RESULT_SUCCESS; }
static SLresult rate_SetProps(void *self, SLuint32 c) { (void)self; (void)c; return SL_RESULT_SUCCESS; }
static SLresult rate_GetProps(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_GetCaps(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRange(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop) {
  (void)self; (void)i;
  if (min) *min = 500;
  if (max) *max = 2000;
  if (step) *step = 1;
  if (prop) *prop = 0;
  return SL_RESULT_SUCCESS;
}
static const SLPlaybackRateItf_ rate_vtable = {
  rate_SetRate, rate_GetRate, rate_SetProps, rate_GetProps, rate_GetCaps, rate_GetRange,
};

static SLresult cfg_SetConfiguration(void *self, const void *key, const void *value, SLuint32 sz) {
  (void)self; (void)key; (void)value; (void)sz; return SL_RESULT_SUCCESS;
}
static SLresult cfg_GetConfiguration(void *self, const void *key, SLuint32 *psz, void *value) {
  (void)self; (void)key; (void)value; if (psz) *psz = 0; return SL_RESULT_SUCCESS;
}
static SLresult cfg_AcquireJavaProxy(void *self, SLuint32 t, void *p) {
  (void)self; (void)t; if (p) *(void **)p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult cfg_ReleaseJavaProxy(void *self, SLuint32 t) { (void)self; (void)t; return SL_RESULT_SUCCESS; }

static const SLAndroidConfigurationItf_ cfg_vtable = {
  cfg_SetConfiguration, cfg_GetConfiguration, cfg_AcquireJavaProxy, cfg_ReleaseJavaProxy,
};

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);
static void player_Destroy(void *self);

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult obj_Abort(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult obj_SetPriority(void *self, SLint32 a, SLboolean b) { (void)self; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLOC(void *self, SLint32 a, SLInterfaceID *b, SLboolean c) { (void)self; (void)a; (void)b; (void)c; return SL_RESULT_SUCCESS; }

static SLresult mix_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  (void)self; (void)iid;
  if (pInterface) *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void simple_Destroy(void *self) { free(self); }

static const SLObjectItf_ player_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface, obj_RegisterCallback,
  obj_Abort, player_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};
static const SLObjectItf_ mix_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Player *p = CONTAINER(self, Player, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_PLAY) {
    *(void **)pInterface = &p->play_vt;
  } else if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
    *(void **)pInterface = &p->bq_vt;
  } else if (iid == SL_IID_VOLUME) {
    *(void **)pInterface = &p->vol_vt;
  } else if (iid == SL_IID_PLAYBACKRATE) {
    *(void **)pInterface = &p->rate_vt;
  } else if (iid == SL_IID_ANDROIDCONFIGURATION) {
    *(void **)pInterface = &p->config_vt;
  } else {
    *(void **)pInterface = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj_vt);
  int free_now = 0;
  int destroyed_now = 0;
  SDL_mutex *registry = registry_lock();
  SDL_LockMutex(registry);
  SDL_LockMutex(p->lock);
  if (!p->destroy_requested) {
    destroyed_now = 1;
    p->destroy_requested = 1;
    p->play_state = SL_PLAYSTATE_STOPPED;
    p->cb = NULL;
    p->cb_ctx = NULL;
    player_clear_queue_locked(p);
    for (int i = 0; i < g_player_count; i++)
      if (g_players[i] == p) g_players[i] = NULL;
    if (p->refs)
      --p->refs; /* release the guest-visible SLObjectItf ownership */
    free_now = (p->refs == 0);
  }
  SDL_UnlockMutex(p->lock);
  SDL_UnlockMutex(registry);
  if (free_now)
    player_free_storage(p);
  if (destroyed_now)
    __atomic_add_fetch(&g_players_destroyed, 1, __ATOMIC_RELAXED);
}

static int supported_player_interface(const SLInterfaceID iid) {
  return iid == SL_IID_PLAY || iid == SL_IID_BUFFERQUEUE ||
         iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE || iid == SL_IID_VOLUME ||
         iid == SL_IID_PLAYBACKRATE || iid == SL_IID_ANDROIDCONFIGURATION;
}

static SLresult parse_player_source(const SLDataSource *src, int *channels,
                                    int *rate, int *sample_bits, int *sbytes,
                                    int *representation) {
  if (!src || !src->pLocator || !src->pFormat || !channels || !rate ||
      !sample_bits || !sbytes || !representation)
    return SL_RESULT_PARAMETER_INVALID;
  const SLDataLocator_BufferQueue *locator = src->pLocator;
  if ((locator->locatorType != SL_DATALOCATOR_BUFFERQUEUE &&
       locator->locatorType != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) ||
      !locator->numBuffers || locator->numBuffers >= BQ_SLOTS)
    return SL_RESULT_PARAMETER_INVALID;

  const SLDataFormat_PCM *fmt = src->pFormat;
  if (fmt->formatType != SL_DATAFORMAT_PCM &&
      fmt->formatType != SL_ANDROID_DATAFORMAT_PCM_EX)
    return SL_RESULT_FEATURE_UNSUPPORTED;
  if (fmt->numChannels < 1 || fmt->numChannels > 2 ||
      fmt->samplesPerSec < UINT32_C(8000000) ||
      fmt->samplesPerSec > UINT32_C(192000000) ||
      fmt->samplesPerSec % 1000 ||
      (fmt->endianness != 0 &&
       fmt->endianness != SL_BYTEORDER_LITTLEENDIAN))
    return SL_RESULT_CONTENT_UNSUPPORTED;

  const uint32_t bits = fmt->bitsPerSample;
  const uint32_t container = fmt->containerSize ? fmt->containerSize : bits;
  if (bits < 8 || bits > 32 || container < bits || container > 32 ||
      container % 8)
    return SL_RESULT_CONTENT_UNSUPPORTED;
  int pcm_representation = SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT;
  if (fmt->formatType == SL_ANDROID_DATAFORMAT_PCM_EX)
    pcm_representation = (int)((const uint32_t *)fmt)[7];
  else if (bits == 8)
    pcm_representation = SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT;
  if (pcm_representation != SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT &&
      pcm_representation != SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT &&
      pcm_representation != SL_ANDROID_PCM_REPRESENTATION_FLOAT)
    return SL_RESULT_CONTENT_UNSUPPORTED;
  if (pcm_representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT &&
      (bits != 32 || container != 32))
    return SL_RESULT_CONTENT_UNSUPPORTED;

  *channels = (int)fmt->numChannels;
  *rate = (int)(fmt->samplesPerSec / 1000);
  *sample_bits = (int)bits;
  *sbytes = (int)(container / 8);
  *representation = pcm_representation;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateAudioPlayer(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                      SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req) {
  (void)self;
  __atomic_add_fetch(&g_player_create_attempts, 1, __ATOMIC_RELAXED);
  if (!pPlayer) {
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_PARAMETER_INVALID;
  }
  *pPlayer = NULL;

  if (numIfaces && !ids) {
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_PARAMETER_INVALID;
  }
  for (SLuint32 index = 0; index < numIfaces; ++index) {
    if (!supported_player_interface(ids[index]) && req && req[index]) {
      __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
      return SL_RESULT_FEATURE_UNSUPPORTED;
    }
  }

  if (!snk || !snk->pLocator) {
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_PARAMETER_INVALID;
  }
  const SLDataLocator_OutputMix *sink_locator = snk->pLocator;
  if (sink_locator->locatorType != SL_DATALOCATOR_OUTPUTMIX ||
      !sink_locator->outputMix) {
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_PARAMETER_INVALID;
  }

  int channels = 0, rate = 0, sample_bits = 0, sbytes = 0;
  int representation = 0;
  const SLresult format_result =
    parse_player_source(src, &channels, &rate, &sample_bits, &sbytes,
                        &representation);
  if (format_result != SL_RESULT_SUCCESS) {
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return format_result;
  }

  Player *p = calloc(1, sizeof(*p));
  if (!p) {
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_MEMORY_FAILURE;
  }
  p->obj_vt = &player_obj_vtable;
  p->play_vt = &play_vtable;
  p->bq_vt = &bq_vtable;
  p->vol_vt = &vol_vtable;
  p->rate_vt = &rate_vtable;
  p->config_vt = &cfg_vtable;
  p->refs = 1;
  p->gain = 1.0f;
  p->volume_mb = 0;
  p->channels = channels;
  p->rate = rate;
  p->sbytes = sbytes;
  p->sample_bits = sample_bits;
  p->representation = representation;
  p->is_float = representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT;
  p->play_state = SL_PLAYSTATE_STOPPED;
  p->lock = SDL_CreateMutex();
  if (!p->lock) {
    player_free_storage(p);
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_MEMORY_FAILURE;
  }

  if (!ensure_device(p->rate)) {
    player_free_storage(p);
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_RESOURCE_ERROR;
  }

  SDL_mutex *registry = registry_lock();
  SDL_LockMutex(registry);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == NULL) { slot = i; break; }
  if (slot < 0 && g_player_count < MAX_PLAYERS)
    slot = g_player_count++;
  if (slot >= 0)
    g_players[slot] = p;
  SDL_UnlockMutex(registry);

  if (slot < 0) {
    /* A live OpenSL object remains guest-owned until its Destroy method. */
    player_free_storage(p);
    __atomic_add_fetch(&g_player_create_failures, 1, __ATOMIC_RELAXED);
    return SL_RESULT_RESOURCE_ERROR;
  }

  *pPlayer = &p->obj_vt;
  __atomic_add_fetch(&g_players_created, 1, __ATOMIC_RELAXED);
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, SLObjectItf *pMix, SLuint32 numIfaces,
                                     const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)numIfaces; (void)ids; (void)req;
  if (!pMix)
    return SL_RESULT_PARAMETER_INVALID;
  *pMix = NULL;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m)
    return SL_RESULT_MEMORY_FAILURE;
  m->obj_vt = &mix_obj_vtable;
  *pMix = &m->obj_vt;
  __atomic_add_fetch(&g_output_mix_creates, 1, __ATOMIC_RELAXED);
  return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const SLEngineItf_ engine_vtable = {
  .CreateLEDDevice = (void *)eng_unsupported,
  .CreateVibraDevice = (void *)eng_unsupported,
  .CreateAudioPlayer = eng_CreateAudioPlayer,
  .CreateAudioRecorder = (void *)eng_unsupported,
  .CreateMidiPlayer = (void *)eng_unsupported,
  .CreateListener = (void *)eng_unsupported,
  .Create3DGroup = (void *)eng_unsupported,
  .CreateOutputMix = eng_CreateOutputMix,
  .CreateMetadataExtractor = (void *)eng_unsupported,
  .CreateExtensionObject = (void *)eng_unsupported,
  .QueryNumSupportedInterfaces = (void *)eng_unsupported,
  .QuerySupportedInterfaces = (void *)eng_unsupported,
  .QueryNumSupportedExtensions = (void *)eng_unsupported,
  .QuerySupportedExtension = (void *)eng_unsupported,
  .IsExtensionSupported = (void *)eng_unsupported,
};

static SLresult engine_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Engine *e = CONTAINER(self, Engine, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) {
    *(void **)pInterface = &e->eng_vt;
    return SL_RESULT_SUCCESS;
  }
  *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLObjectItf_ engine_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  if (!pEngine)
    return SL_RESULT_PARAMETER_INVALID;
  *pEngine = NULL;
  if (!ensure_registry_lock())
    return SL_RESULT_MEMORY_FAILURE;
  Engine *e = calloc(1, sizeof(*e));
  if (!e)
    return SL_RESULT_MEMORY_FAILURE;
  e->obj_vt = &engine_obj_vtable;
  e->eng_vt = &engine_vtable;
  *pEngine = &e->obj_vt;
  __atomic_add_fetch(&g_engine_creates, 1, __ATOMIC_RELAXED);
  return SL_RESULT_SUCCESS;
}

int opensles_initialize(void) {
  /* Do not start audren during the boot-critical Unity/IL2CPP allocator and
   * renderer initialization.  CRIWARE creates an OpenSL player when it is ready
   * for output, and eng_CreateAudioPlayer opens the SDL device at that exact
   * boundary.  Creating it here used to start an otherwise idle host audio
   * thread before Unity's first render and regressed boot to a black screen. */
  return ensure_registry_lock() ? 0 : -1;
}

void opensles_set_focus(int focused) {
  if (!ensure_registry_lock()) return;
  SDL_mutex *registry = registry_lock();
  SDL_LockMutex(registry);
  const int old_focused = g_output_focused;
  g_output_focused = focused != 0;
  if (g_output_focused != old_focused) {
    if (g_output_focused)
      __atomic_add_fetch(&g_focus_resumes, 1, __ATOMIC_RELAXED);
    else
      __atomic_add_fetch(&g_focus_pauses, 1, __ATOMIC_RELAXED);
  }
  const SDL_AudioDeviceID device = g_dev;
  const int pause = g_output_focused ? 0 : 1;
  SDL_UnlockMutex(registry);
  if (device) SDL_PauseAudioDevice(device, pause);
}

void opensles_get_diagnostics(OpenSLESDiagnostics *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->engine_creates = __atomic_load_n(&g_engine_creates, __ATOMIC_RELAXED);
  out->output_mix_creates =
    __atomic_load_n(&g_output_mix_creates, __ATOMIC_RELAXED);
  out->player_create_attempts =
    __atomic_load_n(&g_player_create_attempts, __ATOMIC_RELAXED);
  out->players_created = __atomic_load_n(&g_players_created, __ATOMIC_RELAXED);
  out->player_create_failures =
    __atomic_load_n(&g_player_create_failures, __ATOMIC_RELAXED);
  out->players_destroyed =
    __atomic_load_n(&g_players_destroyed, __ATOMIC_RELAXED);
  out->buffers_enqueued =
    __atomic_load_n(&g_buffers_enqueued, __ATOMIC_RELAXED);
  out->bytes_enqueued = __atomic_load_n(&g_bytes_enqueued, __ATOMIC_RELAXED);
  out->enqueue_failures =
    __atomic_load_n(&g_enqueue_failures, __ATOMIC_RELAXED);
  out->device_callbacks =
    __atomic_load_n(&g_device_callbacks, __ATOMIC_RELAXED);
  out->output_frames = __atomic_load_n(&g_output_frames, __ATOMIC_RELAXED);
  out->mixed_frames = __atomic_load_n(&g_mixed_frames, __ATOMIC_RELAXED);
  out->buffer_callbacks =
    __atomic_load_n(&g_buffer_callbacks, __ATOMIC_RELAXED);
  out->underruns = __atomic_load_n(&g_underruns, __ATOMIC_RELAXED);
  out->clipped_samples =
    __atomic_load_n(&g_clipped_samples, __ATOMIC_RELAXED);
  out->focus_pauses = __atomic_load_n(&g_focus_pauses, __ATOMIC_RELAXED);
  out->focus_resumes = __atomic_load_n(&g_focus_resumes, __ATOMIC_RELAXED);
  out->device_open_attempts =
    __atomic_load_n(&g_device_open_attempts, __ATOMIC_RELAXED);
  out->device_open_failures =
    __atomic_load_n(&g_device_open_failures, __ATOMIC_RELAXED);
  out->queue_high_water =
    __atomic_load_n(&g_queue_high_water, __ATOMIC_RELAXED);

  SDL_mutex *registry = registry_lock();
  if (!registry) return;
  SDL_LockMutex(registry);
  out->device_open = g_dev != 0;
  out->device_rate = (uint32_t)g_dev_rate;
  out->device_channels = g_device_channels;
  out->device_format = g_device_format;
  out->device_samples = g_device_samples;
  out->output_focused = g_output_focused != 0;
  for (int index = 0; index < g_player_count; ++index) {
    Player *p = g_players[index];
    if (!p || p->destroy_requested) continue;
    SDL_LockMutex(p->lock);
    ++out->live_players;
    if (p->play_state == SL_PLAYSTATE_PLAYING) ++out->playing_players;
    if (p->is_float) ++out->float_players;
    else ++out->integer_players;
    SDL_UnlockMutex(p->lock);
  }
  SDL_UnlockMutex(registry);
}

void opensles_shutdown(void) {
  SDL_AudioDeviceID device = 0;
  SDL_mutex *registry = registry_lock();
  if (registry) {
    SDL_LockMutex(registry);
    if (g_device_closing) {
      SDL_UnlockMutex(registry);
      return;
    }
    g_output_focused = 0;
    device = g_dev;
    g_dev = 0;
    g_device_closing = device != 0;
    SDL_UnlockMutex(registry);
  }
  if (device) {
    SDL_PauseAudioDevice(device, 1);
    SDL_CloseAudioDevice(device);
  }
  if (registry) {
    SDL_LockMutex(registry);
    g_device_closing = 0;
    SDL_UnlockMutex(registry);
  }
}

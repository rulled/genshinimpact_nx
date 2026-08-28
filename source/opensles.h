/* Minimal OpenSL ES object model backed by SDL2 audio. */

#ifndef __OPENSLES_H__
#define __OPENSLES_H__

#include <stdint.h>

typedef struct {
  uint64_t engine_creates;
  uint64_t output_mix_creates;
  uint64_t player_create_attempts;
  uint64_t players_created;
  uint64_t player_create_failures;
  uint64_t players_destroyed;
  uint64_t buffers_enqueued;
  uint64_t bytes_enqueued;
  uint64_t enqueue_failures;
  uint64_t device_callbacks;
  uint64_t output_frames;
  uint64_t mixed_frames;
  uint64_t buffer_callbacks;
  uint64_t underruns;
  uint64_t clipped_samples;
  uint64_t focus_pauses;
  uint64_t focus_resumes;
  uint32_t device_open_attempts;
  uint32_t device_open_failures;
  uint32_t device_open;
  uint32_t device_rate;
  uint32_t device_channels;
  uint32_t device_format;
  uint32_t device_samples;
  uint32_t output_focused;
  uint32_t live_players;
  uint32_t playing_players;
  uint32_t integer_players;
  uint32_t float_players;
  uint32_t queue_high_water;
} OpenSLESDiagnostics;

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired);

/* Open the SDL/libnx audren device before Unity constructors run so a backend
 * failure surfaces immediately instead of silently selecting a no-sound
 * fallback. */
int opensles_initialize(void);

/* Pause physical output across HOS focus/app applet transitions. Queued OpenSL
 * buffers remain owned by their players and resume without recreating them. */
void opensles_set_focus(int focused);
void opensles_get_diagnostics(OpenSLESDiagnostics *out);
void opensles_shutdown(void);

/* OpenSL ES interface identifiers used by Unity/CRIWARE. */
extern void *SL_IID_3DCOMMIT, *SL_IID_3DDOPPLER, *SL_IID_3DGROUPING, *SL_IID_3DLOCATION;
extern void *SL_IID_3DMACROSCOPIC, *SL_IID_3DSOURCE, *SL_IID_ANDROIDCONFIGURATION;
extern void *SL_IID_ANDROIDACOUSTICECHOCANCELLATION, *SL_IID_ANDROIDAUTOMATICGAINCONTROL;
extern void *SL_IID_ANDROIDNOISESUPPRESSION;
extern void *SL_IID_ANDROIDEFFECT, *SL_IID_ANDROIDEFFECTCAPABILITIES, *SL_IID_ANDROIDEFFECTSEND;
extern void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE, *SL_IID_AUDIODECODERCAPABILITIES, *SL_IID_AUDIOENCODER;
extern void *SL_IID_AUDIOENCODERCAPABILITIES, *SL_IID_AUDIOIODEVICECAPABILITIES, *SL_IID_BASSBOOST;
extern void *SL_IID_BUFFERQUEUE, *SL_IID_DEVICEVOLUME, *SL_IID_DYNAMICINTERFACEMANAGEMENT;
extern void *SL_IID_DYNAMICSOURCE, *SL_IID_EFFECTSEND, *SL_IID_ENGINE, *SL_IID_ENGINECAPABILITIES;
extern void *SL_IID_ENVIRONMENTALREVERB, *SL_IID_EQUALIZER, *SL_IID_LED, *SL_IID_METADATAEXTRACTION;
extern void *SL_IID_METADATATRAVERSAL, *SL_IID_MIDIMESSAGE, *SL_IID_MIDIMUTESOLO, *SL_IID_MIDITEMPO;
extern void *SL_IID_MIDITIME, *SL_IID_MUTESOLO, *SL_IID_NULL, *SL_IID_OBJECT, *SL_IID_OUTPUTMIX;
extern void *SL_IID_PITCH, *SL_IID_PLAY, *SL_IID_PLAYBACKRATE, *SL_IID_PREFETCHSTATUS;
extern void *SL_IID_PRESETREVERB, *SL_IID_RATEPITCH, *SL_IID_RECORD, *SL_IID_SEEK, *SL_IID_THREADSYNC;
extern void *SL_IID_VIBRA, *SL_IID_VIRTUALIZER, *SL_IID_VISUALIZATION, *SL_IID_VOLUME;

#endif

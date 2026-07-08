/*
 * pipewire_audio.h - PipeWire audio wrapper for ACAP
 * Simplified version based on SSW Audio API
 * Copyright (C) Axis Communications AB
 */

#ifndef _PIPEWIRE_AUDIO_H_
#define _PIPEWIRE_AUDIO_H_

#include <glib.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>
#pragma GCC diagnostic pop

typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Audio channel configuration
 */
typedef enum {
    PW_AUDIO_NONE = 0,
    PW_AUDIO_MONO = 1,
    PW_AUDIO_STEREO = 2
} PWAudioChannel;

/**
 * Audio stream type
 */
typedef enum {
    PW_AUDIO_NO_STREAM,
    PW_AUDIO_CAPTURE_STREAM,
    PW_AUDIO_PLAYBACK_STREAM
} PWAudioStreamType;

/**
 * Opaque audio stream handle
 */
typedef struct _PWAudio PWAudio;

/**
 * Buffer callback - called when audio data is ready
 *
 * @param buffer - PipeWire buffer with audio data
 * @param buf_info - For capture: timestamp, For playback: requested size
 * @param userdata - User data
 */
typedef void (*PWAudioOnBuffer)(struct spa_buffer *buffer,
                                guint64 buf_info,
                                gpointer userdata);

/**
 * Error callback - called on stream errors
 *
 * @param error - GError with error details
 * @param userdata - User data
 */
typedef void (*PWAudioOnError)(const GError *error, gpointer userdata);

/**
 * Initialize PipeWire audio library with main loop context
 * MUST be called from the main thread before starting any streams
 *
 * @param main_context - The GMainContext from your main loop (use g_main_loop_get_context())
 */
void
pw_audio_init(GMainContext *main_context);

/**
 * Start audio capture stream
 *
 * @param format - Audio format (e.g., SPA_AUDIO_FORMAT_F32)
 * @param samplerate - Sample rate (e.g., 16000)
 * @param channels - Mono or Stereo
 * @param on_error - Error callback (can be NULL)
 * @param userdata - User data for error callback
 * @return Stream handle or NULL on error
 */
PWAudio *
pw_audio_capture_start(enum spa_audio_format format,
                       guint32 samplerate,
                       PWAudioChannel channels,
                       const char *node_name,
                       PWAudioOnError on_error,
                       gpointer userdata);

/**
 * Start audio playback stream
 *
 * @param format - Audio format (e.g., SPA_AUDIO_FORMAT_F32)
 * @param samplerate - Sample rate (e.g., 16000), or 0 for device native
 * @param channels - Mono or Stereo
 * @param on_error - Error callback (can be NULL)
 * @param userdata - User data for error callback
 * @return Stream handle or NULL on error
 */
PWAudio *
pw_audio_playback_start(enum spa_audio_format format,
                        guint32 samplerate,
                        PWAudioChannel channels,
                        PWAudioOnError on_error,
                        gpointer userdata);

/**
 * Set buffer callback for stream
 *
 * @param stream - Stream handle
 * @param callback - Buffer callback function
 * @param userdata - User data for callback
 */
void
pw_audio_set_on_buffer_cb(PWAudio *stream,
                          PWAudioOnBuffer callback,
                          gpointer userdata);

/**
 * Stop stream and free resources
 *
 * @param stream - Stream handle
 */
void
pw_audio_stop(PWAudio *stream);

/**
 * Get stream type
 *
 * @param stream - Stream handle
 * @return Stream type
 */
PWAudioStreamType
pw_audio_stream_get_type(PWAudio *stream);

/**
 * Get stream sample rate
 *
 * @param stream - Stream handle
 * @return Sample rate
 */
guint32
pw_audio_stream_get_samplerate(PWAudio *stream);

/**
 * Get stream channels
 *
 * @param stream - Stream handle
 * @return Number of channels
 */
PWAudioChannel
pw_audio_stream_get_channels(PWAudio *stream);

/**
 * Enable debug logging
 *
 * @param stream - Stream handle
 * @param enable - Enable or disable debug
 */
void
pw_audio_enable_debug(PWAudio *stream, gboolean enable);

cJSON *
pw_audio_list_input_nodes(void);

const char *
pw_audio_stream_get_node_name(PWAudio *stream);

#ifdef __cplusplus
}
#endif

#endif // _PIPEWIRE_AUDIO_H_

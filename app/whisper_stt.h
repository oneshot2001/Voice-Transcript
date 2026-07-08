/*
 * whisper_stt.h - Continuous local speech-to-text using whisper.cpp
 * Replaces the remote Wyoming STT client with on-device inference.
 */

#ifndef _WHISPER_STT_H_
#define _WHISPER_STT_H_

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gboolean enabled;             // Pause/resume continuous listening
    char language[8];             // ISO 639-1 code, or "auto" for per-utterance detection
    gboolean translate;           // Translate non-English speech to English
    char sensitivity[16];         // "low" | "normal" | "high" | "maximum"
    int silence_timeout_ms;       // Trailing silence before an utterance ends
    int min_speech_ms;            // Minimum speech duration to count as an utterance
    int max_utterance_sec;        // Hard cap on a single utterance's length
    int inference_threads;        // whisper.cpp CPU threads
    int max_tokens;                // whisper.cpp max tokens per utterance
} WhisperSTTConfig;

// Epoch timestamp (milliseconds) for when VAD detected utterance start.
typedef gint64 WhisperSTT_UtteranceStartMs;

// Invoked once per finished utterance, on the main thread.
// language is the fixed configured language, or the whisper-detected
// language code when the configured language is "auto".
typedef void (*WhisperSTT_TranscriptCallback)(const char *text, const char *language, int decode_ms, int total_ms, WhisperSTT_UtteranceStartMs utterance_start_ms);

// Optional callback used when external STT backend is selected.
// Called on the main thread with raw mono F32 16kHz utterance audio.
typedef void (*WhisperSTT_ExternalTranscribeCallback)(const float *samples, guint32 count, const char *language, gboolean translate, WhisperSTT_UtteranceStartMs utterance_start_ms);

// Invoked on engine state changes ("idle" | "listening" | "transcribing"),
// on the main thread.
typedef void (*WhisperSTT_StateCallback)(const char *state);

// Loads the whisper.cpp model and starts the capture-feeding ring buffer +
// dedicated VAD/transcribe thread. Returns 0 on success.
int whisper_stt_init(const char *model_path);

// Applies new settings; safe to call at any time (e.g. from Settings_Updated_Callback).
void whisper_stt_configure(const WhisperSTTConfig *config);

void whisper_stt_set_transcript_callback(WhisperSTT_TranscriptCallback cb);
void whisper_stt_set_state_callback(WhisperSTT_StateCallback cb);
void whisper_stt_set_external_transcribe_callback(WhisperSTT_ExternalTranscribeCallback cb);

// Feed raw F32 mono 16kHz samples from the PipeWire capture callback.
// Cheap/non-blocking - just copies into the internal ring buffer.
void whisper_stt_feed(const float *samples, guint32 count);

void whisper_stt_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // _WHISPER_STT_H_

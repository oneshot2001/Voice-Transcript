/*
 * whisper_stt.c - Continuous local speech-to-text using whisper.cpp
 *
 * Architecture: the PipeWire capture callback (main thread) pushes raw F32
 * samples into a ring buffer via whisper_stt_feed(). A dedicated pthread
 * consumes the ring buffer in 100ms frames, runs an energy-based VAD with
 * adaptive noise floor and pre-roll to find utterance boundaries, and runs
 * whisper_full() inference on each finished utterance. Results are marshaled
 * back to the main thread via g_idle_add() before invoking the registered
 * callbacks, since the transcribe thread must not touch GLib/MQTT state
 * directly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <math.h>
#include <pthread.h>
#include <glib.h>

#include "whisper_stt.h"
#include "whisper.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}

#define WSTT_SAMPLE_RATE       16000
#define WSTT_FRAME_MS          40
#define WSTT_FRAME_SAMPLES     (WSTT_SAMPLE_RATE * WSTT_FRAME_MS / 1000)   /* 1600 */
#define WSTT_PREROLL_FRAMES    3                                          /* 300ms */
#define WSTT_RING_SECONDS      30
#define WSTT_RING_SAMPLES      (WSTT_SAMPLE_RATE * WSTT_RING_SECONDS)
#define WSTT_MAX_UTTER_SAMPLES (WSTT_SAMPLE_RATE * 30)                    /* hard safety cap */
#define WSTT_MIN_UTTER_MS      600
#define WSTT_NOISE_FLOOR_MIN   0.00001f
#define WSTT_NOISE_FLOOR_MAX   0.02f

typedef struct {
    gboolean enabled;
    char language[8];
    gboolean translate;
    int silence_timeout_ms;
    int min_speech_ms;
    int max_utterance_sec;
    int inference_threads;
    int max_tokens;
    float abs_floor;    // derived from "sensitivity"
    float snr_margin;   // derived from "sensitivity"
} WsttInternalConfig;

static struct whisper_context *g_ctx = NULL;
static pthread_t g_worker_thread;
static gboolean g_running = FALSE;

static float g_ring[WSTT_RING_SAMPLES];
static guint64 g_written = 0;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ring_cond = PTHREAD_COND_INITIALIZER;

static GMutex g_config_mutex;
static WsttInternalConfig g_config = {
    .enabled = TRUE,
    .language = "auto",
    .translate = FALSE,
    .silence_timeout_ms = 450,
    .min_speech_ms = 180,
    .max_utterance_sec = 8,
    .inference_threads = 3,
    .max_tokens = 48,
    .abs_floor = 0.00025f,
    .snr_margin = 8.0f,
};

static WhisperSTT_TranscriptCallback g_transcript_cb = NULL;
static WhisperSTT_StateCallback g_state_cb = NULL;
static WhisperSTT_ExternalTranscribeCallback g_external_transcribe_cb = NULL;

typedef struct {
    char *text;
    char *language;
    int decode_ms;
    int total_ms;
    gint64 utterance_start_ms;
} TranscriptResult;

static gboolean
deliver_transcript_idle(gpointer user_data) {
    TranscriptResult *r = (TranscriptResult *)user_data;
    if (g_transcript_cb) {
        g_transcript_cb(r->text, r->language, r->decode_ms, r->total_ms, r->utterance_start_ms);
    }
    g_free(r->text);
    g_free(r->language);
    g_free(r);
    return G_SOURCE_REMOVE;
}

typedef struct {
    float *samples;
    guint32 count;
    char language[8];
    gboolean translate;
    gint64 utterance_start_ms;
} ExternalTranscribeRequest;

static gboolean
deliver_external_transcribe_idle(gpointer user_data) {
    ExternalTranscribeRequest *r = (ExternalTranscribeRequest *)user_data;
    if (g_external_transcribe_cb) {
        g_external_transcribe_cb(r->samples, r->count, r->language, r->translate, r->utterance_start_ms);
    }
    g_free(r->samples);
    g_free(r);
    return G_SOURCE_REMOVE;
}

typedef struct {
    char state[16];
} StateResult;

static gboolean
deliver_state_idle(gpointer user_data) {
    StateResult *r = (StateResult *)user_data;
    if (g_state_cb) {
        g_state_cb(r->state);
    }
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void
notify_state(const char *state) {
    if (!g_state_cb) return;
    StateResult *r = g_new0(StateResult, 1);
    g_strlcpy(r->state, state, sizeof(r->state));
    g_idle_add(deliver_state_idle, r);
}

void
whisper_stt_feed(const float *samples, guint32 count) {
    if (!g_running || !samples || count == 0) return;

    pthread_mutex_lock(&g_ring_lock);
    for (guint32 i = 0; i < count; i++) {
        g_ring[(g_written + i) % WSTT_RING_SAMPLES] = samples[i];
    }
    g_written += count;
    pthread_cond_broadcast(&g_ring_cond);
    pthread_mutex_unlock(&g_ring_lock);
}

// Blocks until a full frame is available. Returns FALSE once shutdown begins.
static gboolean
read_frame(guint64 *next, float *frame) {
    pthread_mutex_lock(&g_ring_lock);
    while (g_running && g_written < *next + WSTT_FRAME_SAMPLES) {
        pthread_cond_wait(&g_ring_cond, &g_ring_lock);
    }
    if (!g_running) {
        pthread_mutex_unlock(&g_ring_lock);
        return FALSE;
    }
    // If the consumer fell behind further than the ring's capacity, the
    // oldest unread audio has already been overwritten - skip ahead.
    if (g_written > *next + WSTT_RING_SAMPLES) {
        *next = g_written - WSTT_RING_SAMPLES;
    }
    for (guint32 i = 0; i < WSTT_FRAME_SAMPLES; i++) {
        frame[i] = g_ring[(*next + i) % WSTT_RING_SAMPLES];
    }
    *next += WSTT_FRAME_SAMPLES;
    pthread_mutex_unlock(&g_ring_lock);
    return TRUE;
}

static void
transcribe_and_publish(const float *pcm, guint32 n, guint32 utterance_ms, const char *end_reason, gint64 speech_start_mono_ms, gint64 speech_start_epoch_ms) {
    g_mutex_lock(&g_config_mutex);
    WsttInternalConfig cfg = g_config;
    g_mutex_unlock(&g_config_mutex);

    gint64 decode_start_ms = g_get_monotonic_time() / 1000;

    if (g_external_transcribe_cb) {
        ExternalTranscribeRequest *req = g_new0(ExternalTranscribeRequest, 1);
        req->samples = g_memdup2(pcm, sizeof(float) * n);
        req->count = n;
        g_strlcpy(req->language, cfg.language, sizeof(req->language));
        req->translate = cfg.translate;
        req->utterance_start_ms = speech_start_epoch_ms;
        LOG("whisper_stt: transcription start backend=external reason=%s utterance_ms=%u samples=%u\n",
            end_reason ? end_reason : "unknown",
            utterance_ms,
            n);
        g_idle_add(deliver_external_transcribe_idle, req);
        return;
    }

    if (g_ctx == NULL) {
        LOG_WARN("whisper_stt: internal backend selected but model context is unavailable\n");
        return;
    }

    gboolean auto_lang = (strcmp(cfg.language, "auto") == 0);

    struct whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.n_threads = cfg.inference_threads;
    wp.language = auto_lang ? "auto" : cfg.language;
    wp.translate = cfg.translate;
    wp.no_context = true;
    wp.single_segment = true;
    wp.no_timestamps = true;
    wp.suppress_blank = true;
    wp.temperature_inc = 0.0f;
    wp.max_tokens = cfg.max_tokens;
    wp.print_progress = false;
    wp.print_special = false;
    wp.print_realtime = false;

    LOG("whisper_stt: transcription start reason=%s utterance_ms=%u samples=%u lang=%s translate=%d threads=%d max_tokens=%d\n",
        end_reason ? end_reason : "unknown",
        utterance_ms,
        n,
        wp.language ? wp.language : "auto",
        wp.translate,
        wp.n_threads,
        wp.max_tokens);

    if (whisper_full(g_ctx, wp, pcm, (int)n) != 0) {
        LOG_WARN("whisper_stt: whisper_full() failed\n");
        return;
    }

    int n_seg = whisper_full_n_segments(g_ctx);
    GString *text = g_string_new(NULL);
    for (int i = 0; i < n_seg; i++) {
        g_string_append(text, whisper_full_get_segment_text(g_ctx, i));
    }
    g_strstrip(text->str);
    text->len = strlen(text->str);

    if (text->len == 0) {
        LOG_WARN("whisper_stt: blank transcript from samples=%u duration_ms=%u\n",
            n,
            (unsigned)((1000ULL * n) / WSTT_SAMPLE_RATE));
        g_string_free(text, TRUE);
        return;
    }

    const char *lang = cfg.language;
    char detected[8] = {0};
    if (auto_lang) {
        int lang_id = whisper_full_lang_id(g_ctx);
        const char *l = whisper_lang_str(lang_id);
        if (l) {
            g_strlcpy(detected, l, sizeof(detected));
            lang = detected;
        }
    }

    gint64 decode_done_ms = g_get_monotonic_time() / 1000;
    LOG("whisper_stt: transcription complete lang=%s segments=%d decode_ms=%lld total_ms=%lld\n",
        lang,
        n_seg,
        (long long)(decode_done_ms - decode_start_ms),
        (long long)(decode_done_ms - speech_start_mono_ms));

    TranscriptResult *r = g_new0(TranscriptResult, 1);
    r->text = g_strdup(text->str);
    r->language = g_strdup(lang);
    r->decode_ms = (int)(decode_done_ms - decode_start_ms);
    r->total_ms = (int)(decode_done_ms - speech_start_mono_ms);
    r->utterance_start_ms = speech_start_epoch_ms;
    g_idle_add(deliver_transcript_idle, r);

    g_string_free(text, TRUE);
}

static void *
transcribe_thread_func(void *arg) {
    (void)arg;

    guint64 next = 0;
    float frame[WSTT_FRAME_SAMPLES];
    float preroll[WSTT_PREROLL_FRAMES][WSTT_FRAME_SAMPLES];
    int preroll_n = 0;

    float noise_floor = 0.003f;
    float *utt = g_malloc(sizeof(float) * WSTT_MAX_UTTER_SAMPLES);
    guint32 utt_len = 0;
    guint32 speech_frames = 0;
    guint32 silence_run = 0;
    gboolean in_speech = FALSE;
    gint64 speech_start_mono_ms = 0;
    gint64 speech_start_epoch_ms = 0;

    notify_state("idle");

    while (read_frame(&next, frame)) {
        double sum_sq = 0.0;
        for (int i = 0; i < WSTT_FRAME_SAMPLES; i++) {
            sum_sq += (double)frame[i] * (double)frame[i];
        }
        float rms = (float)sqrt(sum_sq / WSTT_FRAME_SAMPLES);

        g_mutex_lock(&g_config_mutex);
        gboolean enabled = g_config.enabled;
        float abs_floor = g_config.abs_floor;
        float snr_margin = g_config.snr_margin;
        guint32 hang_frames = g_config.silence_timeout_ms / WSTT_FRAME_MS;
        guint32 min_speech_frames = g_config.min_speech_ms / WSTT_FRAME_MS;
        guint32 max_utter_samples = (guint32)g_config.max_utterance_sec * WSTT_SAMPLE_RATE;
        g_mutex_unlock(&g_config_mutex);

        if (max_utter_samples > WSTT_MAX_UTTER_SAMPLES) {
            max_utter_samples = WSTT_MAX_UTTER_SAMPLES;
        }
        if (hang_frames == 0) hang_frames = 1;

        if (!enabled) {
            if (in_speech || utt_len > 0) {
                LOG("whisper_stt: disabled while speech active, dropping buffered audio samples=%u\n", utt_len);
            }
            in_speech = FALSE;
            utt_len = 0;
            preroll_n = 0;
            speech_frames = 0;
            silence_run = 0;
            continue;
        }

        float gate = noise_floor * snr_margin;
        if (gate < abs_floor) gate = abs_floor;
        float continue_gate = gate * 0.6f;
        if (continue_gate < abs_floor * 0.5f) continue_gate = abs_floor * 0.5f;

        if (!in_speech) {
            if (rms <= gate) {
                noise_floor = 0.97f * noise_floor + 0.03f * rms;
                if (noise_floor < WSTT_NOISE_FLOOR_MIN) noise_floor = WSTT_NOISE_FLOOR_MIN;
                if (noise_floor > WSTT_NOISE_FLOOR_MAX) noise_floor = WSTT_NOISE_FLOOR_MAX;

                if (preroll_n < WSTT_PREROLL_FRAMES) {
                    memcpy(preroll[preroll_n++], frame, sizeof(frame));
                } else {
                    memmove(preroll[0], preroll[1], sizeof(frame) * (WSTT_PREROLL_FRAMES - 1));
                    memcpy(preroll[WSTT_PREROLL_FRAMES - 1], frame, sizeof(frame));
                }
            } else {
                in_speech = TRUE;
                silence_run = 0;
                speech_frames = 1;
                utt_len = 0;
                speech_start_mono_ms = g_get_monotonic_time() / 1000;
                speech_start_epoch_ms = g_get_real_time() / 1000;
                for (int p = 0; p < preroll_n; p++) {
                    memcpy(utt + utt_len, preroll[p], sizeof(frame));
                    utt_len += WSTT_FRAME_SAMPLES;
                }
                memcpy(utt + utt_len, frame, sizeof(frame));
                utt_len += WSTT_FRAME_SAMPLES;
                LOG("whisper_stt: audio detected, utterance started\n");
                preroll_n = 0;
                notify_state("listening");
            }
        } else {
            if (utt_len + WSTT_FRAME_SAMPLES <= WSTT_MAX_UTTER_SAMPLES) {
                memcpy(utt + utt_len, frame, sizeof(frame));
                utt_len += WSTT_FRAME_SAMPLES;
            }

            if (rms <= continue_gate) {
                silence_run++;
            } else {
                silence_run = 0;
                speech_frames++;
            }

            gboolean end_utterance = (silence_run >= hang_frames) ||
                                      (utt_len >= max_utter_samples) ||
                                      (utt_len >= WSTT_MAX_UTTER_SAMPLES);

            if (end_utterance) {
                const char *reason = "silence";
                if (utt_len >= WSTT_MAX_UTTER_SAMPLES) {
                    reason = "hard_limit";
                } else if (utt_len >= max_utter_samples) {
                    reason = "max_utterance";
                }

                in_speech = FALSE;
                silence_run = 0;

                guint32 utterance_ms = (unsigned)((1000ULL * utt_len) / WSTT_SAMPLE_RATE);

                LOG("whisper_stt: utterance finalized reason=%s duration_ms=%u speech_frames=%u\n",
                    reason,
                    utterance_ms,
                    speech_frames);

                if (speech_frames >= min_speech_frames && utt_len > 0 && utterance_ms >= WSTT_MIN_UTTER_MS) {
                    notify_state("transcribing");
                    transcribe_and_publish(utt, utt_len, utterance_ms, reason, speech_start_mono_ms, speech_start_epoch_ms);
                } else {
                    LOG_WARN("whisper_stt: discarded utterance samples=%u duration_ms=%u speech_frames=%u min_required=%u min_utter_ms=%u\n",
                        utt_len,
                        utterance_ms,
                        speech_frames,
                        min_speech_frames,
                        WSTT_MIN_UTTER_MS);
                }

                utt_len = 0;
                speech_frames = 0;
                notify_state("idle");
            }
        }
    }

    g_free(utt);
    return NULL;
}

int
whisper_stt_init(const char *model_path) {
    if (g_ctx) {
        LOG_WARN("whisper_stt: already initialized\n");
        return -1;
    }

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;

    LOG("whisper_stt: loading model '%s'\n", model_path);
    g_ctx = whisper_init_from_file_with_params(model_path, cparams);
    if (!g_ctx) {
        LOG_WARN("whisper_stt: failed to load model '%s'\n", model_path);
        return -1;
    }

    g_running = TRUE;
    if (pthread_create(&g_worker_thread, NULL, transcribe_thread_func, NULL) != 0) {
        LOG_WARN("whisper_stt: failed to start transcribe thread\n");
        whisper_free(g_ctx);
        g_ctx = NULL;
        g_running = FALSE;
        return -1;
    }

    LOG("whisper_stt: model loaded, transcribe thread started\n");
    return 0;
}

void
whisper_stt_configure(const WhisperSTTConfig *config) {
    if (!config) return;

    g_mutex_lock(&g_config_mutex);
    g_config.enabled = config->enabled;
    g_strlcpy(g_config.language, config->language, sizeof(g_config.language));
    g_config.translate = config->translate;
    g_config.silence_timeout_ms = config->silence_timeout_ms > 0 ? config->silence_timeout_ms : 450;
    g_config.min_speech_ms = config->min_speech_ms > 0 ? config->min_speech_ms : 180;
    g_config.max_utterance_sec = config->max_utterance_sec > 0 ? config->max_utterance_sec : 8;
    g_config.inference_threads = config->inference_threads > 0 ? config->inference_threads : 3;
    g_config.max_tokens = config->max_tokens > 0 ? config->max_tokens : 48;

    if (strcmp(config->sensitivity, "low") == 0) {
        g_config.abs_floor = 0.0005f;
        g_config.snr_margin = 12.0f;
    } else if (strcmp(config->sensitivity, "high") == 0) {
        g_config.abs_floor = 0.00015f;
        g_config.snr_margin = 6.0f;
    } else if (strcmp(config->sensitivity, "maximum") == 0) {
        g_config.abs_floor = 0.00008f;
        g_config.snr_margin = 4.0f;
    } else {
        g_config.abs_floor = 0.00025f;
        g_config.snr_margin = 8.0f;
    }
    g_mutex_unlock(&g_config_mutex);

    LOG("whisper_stt: configured (enabled=%d language=%s sensitivity=%s silence=%dms min_speech=%dms max_utter=%ds threads=%d max_tokens=%d)\n",
        config->enabled, config->language, config->sensitivity, g_config.silence_timeout_ms,
        g_config.min_speech_ms, g_config.max_utterance_sec, g_config.inference_threads, g_config.max_tokens);
}

void
whisper_stt_set_transcript_callback(WhisperSTT_TranscriptCallback cb) {
    g_transcript_cb = cb;
}

void
whisper_stt_set_state_callback(WhisperSTT_StateCallback cb) {
    g_state_cb = cb;
}

void
whisper_stt_set_external_transcribe_callback(WhisperSTT_ExternalTranscribeCallback cb) {
    g_external_transcribe_cb = cb;
}

void
whisper_stt_cleanup(void) {
    if (g_running) {
        pthread_mutex_lock(&g_ring_lock);
        g_running = FALSE;
        pthread_cond_broadcast(&g_ring_cond);
        pthread_mutex_unlock(&g_ring_lock);
        pthread_join(g_worker_thread, NULL);
    }

    if (g_ctx) {
        whisper_free(g_ctx);
        g_ctx = NULL;
    }
}

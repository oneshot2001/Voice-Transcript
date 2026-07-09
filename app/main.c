#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <stdint.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <curl/curl.h>

#include "pipewire_audio.h"
#include "whisper_stt.h"
#include "wyoming.h"
#include "ACAP.h"
#include "cJSON.h"
#include "MQTT.h"

#define APP_PACKAGE	"voice"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}

#define AUDIO_SAMPLE_RATE 16000
#define MODEL_FILENAME_BASE "ggml-base-q5_1.bin"
#define MODEL_FILENAME_TINY "ggml-tiny-q5_1.bin"
#define TRANSCRIPT_HISTORY_SIZE 100

// =============================================================================
// STATE
// =============================================================================

typedef struct {
    PWAudio *stream;
    gboolean active;
    guint32 sample_count;
} AudioStream;

typedef struct {
    AudioStream input;         // Continuous microphone capture
} VoiceAssistantState;

static VoiceAssistantState va_state = {0};
static char device_serial[32] = {0};
static gboolean g_input_has_samples = FALSE;
static char g_input_node[64] = "AudioDevice0Input0";
static gboolean g_input_restart_pending = FALSE;
static double g_input_rms = 0.0;
static double g_input_peak = 0.0;

typedef struct {
    gint64 ts_ms;
    gint64 utterance_start_ms;
    gboolean ignored;
    int duration_ms;
    char language[8];
    char *keywords_csv;
    char *text;
} TranscriptEntry;

static TranscriptEntry g_history[TRANSCRIPT_HISTORY_SIZE];
static int g_history_count = 0;
static int g_history_next = 0;
static GMutex g_history_mutex;
static guint32 g_transcript_count = 0;

static GMainLoop *main_loop = NULL;
static char g_model_name[16] = "base";
static const char *g_model_filename = MODEL_FILENAME_BASE;
static char g_stt_mode[16] = "internal";
static char g_stt_language[8] = "auto";
static char g_wizard_environment[16] = "office";
static char g_wizard_goal[16] = "quality";
static char g_wizard_sporadic_noise[16] = "moderate";
static char g_transcription_use_case[24] = "continuous";
static char g_command_keywords[512] = "";
static int g_command_min_words = 3;
static GPtrArray *g_command_keywords_normalized = NULL;
static int g_wizard_validation_rounds = 3;
static int g_wizard_background_rounds = 3;
static int g_wizard_distance_rounds = 3;
static char g_wyoming_host[64] = "";
static int g_wyoming_whisper_port = 10300;
static gint64 g_external_request_started_ms = 0;
static gboolean g_external_request_inflight = FALSE;
static gint64 g_external_request_utterance_start_ms = 0;

#define EXTERNAL_ASR_STALE_MS 45000

static void refresh_available_input_nodes_status(void);
static void resolve_model_from_settings(cJSON *settings);
static void Whisper_External_Transcribe_Callback(const float *samples, guint32 count, const char *language, gboolean translate, gint64 utterance_start_ms);
static void start_input_capture(void);
static void stop_input_capture(void);
static void restart_input_capture(void);
static gboolean restart_input_capture_idle(gpointer user_data);
void audio_error_callback(const GError *error, gpointer userdata);
void audio_input_callback(struct spa_buffer *buf, guint64 ts, gpointer userdata);

static gint64 now_ms(void) {
    return g_get_real_time() / 1000;
}

static gboolean
is_allowed_input_node(const char *node) {
    if (!node || !node[0]) return FALSE;
    return g_strcmp0(node, "AudioDevice0Input0") == 0;
}

static char *
normalize_token(const char *text) {
    if (!text) return g_strdup("");
    char *tmp = g_utf8_casefold(text, -1);
    GString *out = g_string_new(NULL);
    const char *p = tmp;
    while (*p) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isalnum(ch)) {
            g_string_append_unichar(out, ch);
        }
        p = g_utf8_next_char(p);
    }
    g_free(tmp);
    return g_string_free(out, FALSE);
}

static int
count_words(const char *text) {
    if (!text || !text[0]) return 0;
    int words = 0;
    gboolean in_word = FALSE;
    const char *p = text;
    while (*p) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isalnum(ch)) {
            if (!in_word) {
                words++;
                in_word = TRUE;
            }
        } else {
            in_word = FALSE;
        }
        p = g_utf8_next_char(p);
    }
    return words;
}

static int
levenshtein_distance(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la == 0) return (int)lb;
    if (lb == 0) return (int)la;

    int *prev = g_new(int, lb + 1);
    int *curr = g_new(int, lb + 1);
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;

    for (size_t i = 1; i <= la; i++) {
        curr[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int best = del < ins ? del : ins;
            curr[j] = best < sub ? best : sub;
        }
        int *swap = prev;
        prev = curr;
        curr = swap;
    }

    int distance = prev[lb];
    g_free(prev);
    g_free(curr);
    return distance;
}

static gboolean
tokens_fuzzy_match(const char *a, const char *b) {
    if (!a[0] || !b[0]) return FALSE;
    if (g_strcmp0(a, b) == 0) return TRUE;

    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la > 1 && a[la - 1] == 's' && strncmp(a, b, la - 1) == 0 && lb == la - 1) return TRUE;
    if (lb > 1 && b[lb - 1] == 's' && strncmp(a, b, lb - 1) == 0 && la == lb - 1) return TRUE;

    if (la > 2 && g_str_has_suffix(a, "es") && lb == la - 2 && strncmp(a, b, la - 2) == 0) return TRUE;
    if (lb > 2 && g_str_has_suffix(b, "es") && la == lb - 2 && strncmp(a, b, lb - 2) == 0) return TRUE;

    int max_len = (int)(la > lb ? la : lb);
    int threshold = max_len <= 5 ? 1 : 2;
    return levenshtein_distance(a, b) <= threshold;
}

static gboolean
ptr_array_contains_string(GPtrArray *arr, const char *value) {
    for (guint i = 0; i < arr->len; i++) {
        if (g_strcmp0((const char *)g_ptr_array_index(arr, i), value) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
rebuild_command_keywords(const char *csv_keywords) {
    if (!g_command_keywords_normalized) {
        g_command_keywords_normalized = g_ptr_array_new_with_free_func(g_free);
    } else {
        g_ptr_array_set_size(g_command_keywords_normalized, 0);
    }

    if (!csv_keywords || !csv_keywords[0]) {
        return;
    }

    gchar **parts = g_strsplit(csv_keywords, ",", -1);
    for (gint i = 0; parts && parts[i]; i++) {
        char *norm = normalize_token(parts[i]);
        if (norm[0]) {
            g_ptr_array_add(g_command_keywords_normalized, norm);
        } else {
            g_free(norm);
        }
    }
    g_strfreev(parts);
}

static gboolean
transcript_matches_command_keywords(const char *clean_text, GPtrArray *detected_keywords) {
    if (!clean_text || !clean_text[0]) return FALSE;
    if (!g_command_keywords_normalized || g_command_keywords_normalized->len == 0) return FALSE;

    gchar **tokens = g_strsplit(clean_text, " ", -1);
    gboolean matched = FALSE;

    for (gint i = 0; tokens && tokens[i] && !matched; i++) {
        char *norm_token = normalize_token(tokens[i]);
        if (!norm_token[0]) {
            g_free(norm_token);
            continue;
        }

        for (guint k = 0; k < g_command_keywords_normalized->len; k++) {
            const char *kw = g_ptr_array_index(g_command_keywords_normalized, k);
            if (tokens_fuzzy_match(norm_token, kw)) {
                matched = TRUE;
                if (detected_keywords && !ptr_array_contains_string(detected_keywords, kw)) {
                    g_ptr_array_add(detected_keywords, g_strdup(kw));
                }
                break;
            }
        }

        g_free(norm_token);
    }

    g_strfreev(tokens);
    return matched;
}

static char *
join_keywords_csv(GPtrArray *keywords) {
    if (!keywords || keywords->len == 0) {
        return g_strdup("");
    }

    GString *joined = g_string_new(NULL);
    for (guint i = 0; i < keywords->len; i++) {
        if (i > 0) g_string_append_c(joined, ',');
        g_string_append(joined, (const char *)g_ptr_array_index(keywords, i));
    }

    return g_string_free(joined, FALSE);
}

static GPtrArray *
split_transcript_candidates(const char *text) {
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    if (!text || !text[0]) {
        return parts;
    }

    if (g_strcmp0(g_transcription_use_case, "continuous") != 0) {
        g_ptr_array_add(parts, g_strdup(text));
        return parts;
    }

    const char *start = text;
    const char *cursor = text;
    while (*cursor) {
        if (*cursor == '.' || *cursor == '!' || *cursor == '?' || *cursor == ';' || *cursor == '\n') {
            size_t len = (size_t)(cursor - start);
            if (len > 0) {
                g_ptr_array_add(parts, g_strndup(start, len));
            }
            start = cursor + 1;
        }
        cursor++;
    }

    if (cursor > start) {
        g_ptr_array_add(parts, g_strndup(start, (size_t)(cursor - start)));
    }

    if (parts->len == 0) {
        g_ptr_array_add(parts, g_strdup(text));
    }

    return parts;
}

static const char *
normalize_input_node(const char *node) {
    if (is_allowed_input_node(node)) {
        return node;
    }
    return "AudioDevice0Input0";
}

// =============================================================================
// TRANSCRIPT HISTORY (backs the Transcriptions tab)
// =============================================================================

static void
history_append(const char *text, const char *language, int duration_ms, gint64 utterance_start_ms, gboolean ignored, const char *keywords_csv) {
    g_mutex_lock(&g_history_mutex);
    TranscriptEntry *e = &g_history[g_history_next];
    g_free(e->text);
    g_free(e->keywords_csv);
    e->text = g_strdup(text);
    e->keywords_csv = g_strdup(keywords_csv ? keywords_csv : "");
    g_strlcpy(e->language, language, sizeof(e->language));
    e->utterance_start_ms = utterance_start_ms;
    e->ignored = ignored;
    e->duration_ms = duration_ms;
    e->ts_ms = g_get_real_time() / 1000;
    g_history_next = (g_history_next + 1) % TRANSCRIPT_HISTORY_SIZE;
    if (g_history_count < TRANSCRIPT_HISTORY_SIZE) g_history_count++;
    g_mutex_unlock(&g_history_mutex);
}

static void
history_clear(void) {
    g_mutex_lock(&g_history_mutex);
    for (int i = 0; i < TRANSCRIPT_HISTORY_SIZE; i++) {
        g_free(g_history[i].text);
        g_free(g_history[i].keywords_csv);
        g_history[i].text = NULL;
        g_history[i].keywords_csv = NULL;
    }
    g_history_count = 0;
    g_history_next = 0;
    g_mutex_unlock(&g_history_mutex);
}

static char *
sanitize_transcript_text(const char *text) {
    if (!text) return g_strdup("");

    size_t n = strlen(text);
    char *out = g_malloc0(n + 1);
    size_t j = 0;
    int bracket_depth = 0;
    int paren_depth = 0;

    for (size_t i = 0; i < n; i++) {
        char c = text[i];

        if (c == '[') {
            bracket_depth++;
            continue;
        }
        if (c == ']') {
            if (bracket_depth > 0) bracket_depth--;
            continue;
        }
        if (c == '(') {
            paren_depth++;
            continue;
        }
        if (c == ')') {
            if (paren_depth > 0) paren_depth--;
            continue;
        }
        if (bracket_depth > 0 || paren_depth > 0) {
            continue;
        }

        out[j++] = c;
    }

    out[j] = '\0';
    g_strstrip(out);

    if (out[0] == '\0') {
        return out;
    }

    char *collapsed = g_malloc0(strlen(out) + 1);
    size_t k = 0;
    gboolean prev_space = FALSE;
    for (size_t i = 0; out[i]; i++) {
        unsigned char ch = (unsigned char)out[i];
        if (g_ascii_isspace(ch)) {
            if (!prev_space) collapsed[k++] = ' ';
            prev_space = TRUE;
        } else {
            collapsed[k++] = out[i];
            prev_space = FALSE;
        }
    }
    collapsed[k] = '\0';
    g_free(out);
    g_strstrip(collapsed);

    if (collapsed[0] == '\0') {
        return collapsed;
    }

    char *normalized = g_ascii_strdown(collapsed, -1);
    for (size_t i = 0; normalized[i]; i++) {
        if (!g_ascii_isalnum((unsigned char)normalized[i]) && normalized[i] != '_' && normalized[i] != ' ') {
            normalized[i] = ' ';
        }
    }
    g_strstrip(normalized);

    if (strcmp(normalized, "music") == 0 ||
        strcmp(normalized, "blank_audio") == 0 ||
        strcmp(normalized, "clicking") == 0 ||
        strcmp(normalized, "clanking") == 0 ||
        strcmp(normalized, "cow mooing") == 0) {
        collapsed[0] = '\0';
    }

    g_free(normalized);
    return collapsed;
}

// =============================================================================
// SETTINGS
// =============================================================================

static void
resolve_model_from_settings(cJSON *settings) {
    g_strlcpy(g_model_name, "base", sizeof(g_model_name));
    g_model_filename = MODEL_FILENAME_BASE;

    if (!settings || !cJSON_IsObject(settings)) {
        return;
    }

    cJSON *model = cJSON_GetObjectItem(settings, "model");
    if (!model || !cJSON_IsString(model) || !model->valuestring || !model->valuestring[0]) {
        return;
    }

    if (g_strcmp0(model->valuestring, "tiny") == 0 || g_strcmp0(model->valuestring, MODEL_FILENAME_TINY) == 0) {
        g_strlcpy(g_model_name, "tiny", sizeof(g_model_name));
        g_model_filename = MODEL_FILENAME_TINY;
        return;
    }

    if (g_strcmp0(model->valuestring, "base") == 0 || g_strcmp0(model->valuestring, MODEL_FILENAME_BASE) == 0) {
        g_strlcpy(g_model_name, "base", sizeof(g_model_name));
        g_model_filename = MODEL_FILENAME_BASE;
        return;
    }

    LOG_WARN("Unknown model '%s', falling back to base\n", model->valuestring);
}

static void
resolve_stt_backend_from_settings(cJSON *settings, const char *language) {
#ifdef EXTERNAL_ONLY_BUILD
    g_strlcpy(g_stt_mode, "external", sizeof(g_stt_mode));
#else
    g_strlcpy(g_stt_mode, "internal", sizeof(g_stt_mode));
#endif
    g_wyoming_host[0] = '\0';
    g_wyoming_whisper_port = 10300;

    if (settings && cJSON_IsObject(settings)) {
        cJSON *mode = cJSON_GetObjectItem(settings, "stt_mode");
        if (mode && cJSON_IsString(mode) && mode->valuestring && mode->valuestring[0]) {
#ifdef EXTERNAL_ONLY_BUILD
            if (g_strcmp0(mode->valuestring, "external") != 0) {
                LOG_WARN("External-only build ignores stt_mode='%s' and forces external backend\n", mode->valuestring);
            }
            g_strlcpy(g_stt_mode, "external", sizeof(g_stt_mode));
#else
            if (g_strcmp0(mode->valuestring, "external") == 0) {
                g_strlcpy(g_stt_mode, "external", sizeof(g_stt_mode));
            }
#endif
        }

        cJSON *host = cJSON_GetObjectItem(settings, "wyoming_host");
        if (host && cJSON_IsString(host) && host->valuestring) {
            g_strlcpy(g_wyoming_host, host->valuestring, sizeof(g_wyoming_host));
        }

        cJSON *port = cJSON_GetObjectItem(settings, "wyoming_whisper_port");
        if (port && cJSON_IsNumber(port) && port->valueint > 0) {
            g_wyoming_whisper_port = port->valueint;
        }
    }

    ACAP_STATUS_SetString("stt", "backend", g_stt_mode);
#ifdef EXTERNAL_ONLY_BUILD
    ACAP_STATUS_SetBool("stt", "external_only_build", TRUE);
#endif

    const char *wyoming_language = language;
    if (!wyoming_language || g_strcmp0(wyoming_language, "auto") == 0) {
        wyoming_language = "";
    }

    if (g_strcmp0(g_stt_mode, "external") == 0) {
        whisper_stt_set_external_transcribe_callback(Whisper_External_Transcribe_Callback);
        if (g_wyoming_host[0] == '\0') {
            g_external_request_inflight = FALSE;
            g_external_request_started_ms = 0;
            g_external_request_utterance_start_ms = 0;
            ACAP_STATUS_SetString("stt", "state", "error");
            ACAP_STATUS_SetString("wyoming", "whisper", "not configured");
            LOG_WARN("External Whisper selected but wyoming_host is empty\n");
            return;
        }

        if (wyoming_configure(g_wyoming_host, 0, g_wyoming_whisper_port, wyoming_language) == 0) {
            if (!wyoming_is_connected(WYOMING_SERVICE_WHISPER)) {
                wyoming_connect(WYOMING_SERVICE_WHISPER);
            }
        }
    } else {
        g_external_request_inflight = FALSE;
        g_external_request_started_ms = 0;
        g_external_request_utterance_start_ms = 0;
        whisper_stt_set_external_transcribe_callback(NULL);
        wyoming_disconnect(WYOMING_SERVICE_WHISPER);
    }
}

static void
refresh_available_input_nodes_status(void) {
    cJSON *nodes = pw_audio_list_input_nodes();
    ACAP_STATUS_SetObject("input", "available_nodes", nodes);
    ACAP_STATUS_SetString("input", "selected_node", g_input_node);
}

static void
stop_input_capture(void) {
    if (va_state.input.stream) {
        pw_audio_stop(va_state.input.stream);
        va_state.input.stream = NULL;
    }

    va_state.input.active = FALSE;
    va_state.input.sample_count = 0;
    g_input_has_samples = FALSE;
    g_input_rms = 0.0;
    g_input_peak = 0.0;
    ACAP_STATUS_SetBool("input", "state", 0);
    ACAP_STATUS_SetNumber("input", "samples", 0);
    ACAP_STATUS_SetNumber("input", "rms", 0.0);
    ACAP_STATUS_SetNumber("input", "peak", 0.0);
    ACAP_STATUS_SetString("input", "bound_node", "");
}

static void
start_input_capture(void) {
    const char *requested_node = (g_strcmp0(g_input_node, "auto") == 0) ? NULL : g_input_node;

    va_state.input.stream = pw_audio_capture_start(
        SPA_AUDIO_FORMAT_F32,
        AUDIO_SAMPLE_RATE,
        PW_AUDIO_MONO,
        requested_node,
        audio_error_callback,
        NULL
    );

    refresh_available_input_nodes_status();

    if (va_state.input.stream) {
        pw_audio_set_on_buffer_cb(va_state.input.stream, audio_input_callback, NULL);
        va_state.input.active = TRUE;
        ACAP_STATUS_SetBool("input", "state", 0);
        ACAP_STATUS_SetString("input", "status", "Waiting for audio");
        LOG("Microphone capture started, waiting for samples\n");
        return;
    }

    LOG_WARN("Failed to start microphone capture stream\n");
    ACAP_STATUS_SetString("input", "status", "Failed to start capture");
}

static void
restart_input_capture(void) {
    stop_input_capture();
    start_input_capture();
}

static gboolean
restart_input_capture_idle(gpointer user_data) {
    (void)user_data;
    g_input_restart_pending = FALSE;
    restart_input_capture();
    return G_SOURCE_REMOVE;
}

static void
apply_settings(cJSON *data) {
    WhisperSTTConfig cfg = {0};
    char requested_input_node[sizeof(g_input_node)] = {0};
    gboolean input_node_changed = FALSE;
    g_strlcpy(requested_input_node, g_input_node, sizeof(requested_input_node));
    cfg.enabled = TRUE;
    g_strlcpy(cfg.language, "auto", sizeof(cfg.language));
    cfg.translate = FALSE;
    g_strlcpy(cfg.sensitivity, "normal", sizeof(cfg.sensitivity));
    cfg.silence_timeout_ms = 650;
    cfg.min_speech_ms = 250;
    cfg.max_utterance_sec = 8;
    cfg.inference_threads = 3;
    cfg.max_tokens = 48;

    cJSON *item;
    if ((item = cJSON_GetObjectItem(data, "enabled")) && cJSON_IsBool(item))
        cfg.enabled = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(data, "language")) && cJSON_IsString(item))
        g_strlcpy(cfg.language, item->valuestring, sizeof(cfg.language));
    if ((item = cJSON_GetObjectItem(data, "translate")) && cJSON_IsBool(item))
        cfg.translate = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(data, "sensitivity")) && cJSON_IsString(item))
        g_strlcpy(cfg.sensitivity, item->valuestring, sizeof(cfg.sensitivity));
    if ((item = cJSON_GetObjectItem(data, "silence_timeout_ms")) && cJSON_IsNumber(item))
        cfg.silence_timeout_ms = item->valueint;
    if ((item = cJSON_GetObjectItem(data, "min_speech_ms")) && cJSON_IsNumber(item))
        cfg.min_speech_ms = item->valueint;
    if ((item = cJSON_GetObjectItem(data, "max_utterance_sec")) && cJSON_IsNumber(item))
        cfg.max_utterance_sec = item->valueint;
    if ((item = cJSON_GetObjectItem(data, "inference_threads")) && cJSON_IsNumber(item))
        cfg.inference_threads = item->valueint;
    if ((item = cJSON_GetObjectItem(data, "max_tokens")) && cJSON_IsNumber(item))
        cfg.max_tokens = item->valueint;
    if ((item = cJSON_GetObjectItem(data, "transcription_use_case")) && cJSON_IsString(item) && item->valuestring[0]) {
        if (g_strcmp0(item->valuestring, "voice_commands") == 0) {
            g_strlcpy(g_transcription_use_case, "voice_commands", sizeof(g_transcription_use_case));
        } else {
            g_strlcpy(g_transcription_use_case, "continuous", sizeof(g_transcription_use_case));
        }
    }
    if ((item = cJSON_GetObjectItem(data, "command_keywords")) && cJSON_IsString(item) && item->valuestring) {
        g_strlcpy(g_command_keywords, item->valuestring, sizeof(g_command_keywords));
    }
    if ((item = cJSON_GetObjectItem(data, "command_min_words")) && cJSON_IsNumber(item)) {
        g_command_min_words = item->valueint;
    }
    if (g_command_min_words < 1) g_command_min_words = 1;
    if (g_command_min_words > 5) g_command_min_words = 5;
    if ((item = cJSON_GetObjectItem(data, "input_node")) && cJSON_IsString(item) && item->valuestring[0]) {
        const char *normalized = normalize_input_node(item->valuestring);
        if (g_strcmp0(normalized, item->valuestring) != 0) {
            LOG_WARN("Unsupported input node '%s', forcing '%s'\n", item->valuestring, normalized);
        }
        g_strlcpy(requested_input_node, normalized, sizeof(requested_input_node));
    } else {
        g_strlcpy(requested_input_node, normalize_input_node(requested_input_node), sizeof(requested_input_node));
    }

    if ((item = cJSON_GetObjectItem(data, "wizard_environment")) && cJSON_IsString(item) && item->valuestring[0])
        g_strlcpy(g_wizard_environment, item->valuestring, sizeof(g_wizard_environment));
    if ((item = cJSON_GetObjectItem(data, "wizard_goal")) && cJSON_IsString(item) && item->valuestring[0])
        g_strlcpy(g_wizard_goal, item->valuestring, sizeof(g_wizard_goal));
    if ((item = cJSON_GetObjectItem(data, "wizard_sporadic_noise")) && cJSON_IsString(item) && item->valuestring[0])
        g_strlcpy(g_wizard_sporadic_noise, item->valuestring, sizeof(g_wizard_sporadic_noise));
    if ((item = cJSON_GetObjectItem(data, "wizard_validation_rounds")) && cJSON_IsNumber(item) && item->valueint > 0)
        g_wizard_validation_rounds = item->valueint;
    if ((item = cJSON_GetObjectItem(data, "wizard_background_rounds")) && cJSON_IsNumber(item) && item->valueint > 0)
        g_wizard_background_rounds = item->valueint;
    if ((item = cJSON_GetObjectItem(data, "wizard_distance_rounds")) && cJSON_IsNumber(item) && item->valueint > 0)
        g_wizard_distance_rounds = item->valueint;

    g_strlcpy(g_stt_language, cfg.language, sizeof(g_stt_language));

    if (g_strcmp0(g_transcription_use_case, "continuous") == 0) {
        cfg.silence_timeout_ms = MAX(cfg.silence_timeout_ms, 900);
        cfg.min_speech_ms = MAX(cfg.min_speech_ms, 220);
        cfg.max_utterance_sec = MAX(cfg.max_utterance_sec, 20);
        cfg.max_tokens = MAX(cfg.max_tokens, 96);
    } else {
        cfg.silence_timeout_ms = MIN(cfg.silence_timeout_ms, 450);
        cfg.min_speech_ms = MIN(cfg.min_speech_ms, 200);
        cfg.max_utterance_sec = MIN(cfg.max_utterance_sec, 6);
        cfg.max_tokens = MIN(cfg.max_tokens, 32);
    }

    rebuild_command_keywords(g_command_keywords);

    resolve_stt_backend_from_settings(data, cfg.language);

    if (g_strcmp0(g_input_node, requested_input_node) != 0) {
        g_strlcpy(g_input_node, requested_input_node, sizeof(g_input_node));
        input_node_changed = TRUE;
    }

    whisper_stt_configure(&cfg);
    ACAP_STATUS_SetString("input", "selected_node", g_input_node);
    ACAP_STATUS_SetString("stt", "use_case", g_transcription_use_case);
    ACAP_STATUS_SetString("stt", "command_keywords", g_command_keywords);
    ACAP_STATUS_SetNumber("stt", "command_min_words", g_command_min_words);
    ACAP_STATUS_SetNumber("stt", "command_keyword_count", g_command_keywords_normalized ? (double)g_command_keywords_normalized->len : 0.0);
    ACAP_STATUS_SetString("wizard", "environment", g_wizard_environment);
    ACAP_STATUS_SetString("wizard", "goal", g_wizard_goal);
    ACAP_STATUS_SetString("wizard", "sporadic_noise", g_wizard_sporadic_noise);
    ACAP_STATUS_SetNumber("wizard", "validation_rounds", g_wizard_validation_rounds);
    ACAP_STATUS_SetNumber("wizard", "background_rounds", g_wizard_background_rounds);
    ACAP_STATUS_SetNumber("wizard", "distance_rounds", g_wizard_distance_rounds);

    if (va_state.input.active && input_node_changed) {
        if (main_loop && !g_main_context_is_owner(g_main_loop_get_context(main_loop))) {
            if (!g_input_restart_pending) {
                g_input_restart_pending = TRUE;
                g_idle_add(restart_input_capture_idle, NULL);
            }
        } else {
            restart_input_capture();
        }
    } else {
        refresh_available_input_nodes_status();
    }
}

void
Settings_Updated_Callback(const char *service, cJSON *data) {
    char *json = cJSON_PrintUnformatted(data);
    LOG("%s: Service=%s Data=%s\n", __func__, service, json);
    free(json);

    if (strcmp(service, "settings") == 0) {
        apply_settings(data);
    }
}

// =============================================================================
// AUDIO
// =============================================================================

void
audio_error_callback(const GError *error, gpointer userdata) {
    (void)userdata;
    LOG_WARN("Audio error: %s\n", error ? error->message : "Unknown error");
    ACAP_STATUS_SetString("input", "error", error ? error->message : "Unknown error");
}

// Continuous capture callback - hands every buffer to the whisper_stt ring
// buffer. Cheap: no VAD or inference happens on this (main) thread.
void
audio_input_callback(struct spa_buffer *buf, guint64 ts, gpointer userdata) {
    (void)ts;
    (void)userdata;

    struct spa_data *data = buf->datas;
    if (!data->data) return;

    float *samples = (float *)data->data;
    guint32 num_samples = data->chunk->size / sizeof(float);
    if (num_samples == 0) return;

    double sum_sq = 0.0;
    float peak = 0.0f;
    for (guint32 i = 0; i < num_samples; i++) {
        float sample = samples[i];
        float magnitude = fabsf(sample);
        sum_sq += (double)sample * (double)sample;
        if (magnitude > peak) peak = magnitude;
    }
    g_input_rms = sqrt(sum_sq / num_samples);
    g_input_peak = peak;
    ACAP_STATUS_SetNumber("input", "rms", g_input_rms);
    ACAP_STATUS_SetNumber("input", "peak", g_input_peak);

    if (!g_input_has_samples) {
        g_input_has_samples = TRUE;
        ACAP_STATUS_SetBool("input", "state", 1);
        ACAP_STATUS_SetString("input", "status", "Listening");
        const char *bound_node = pw_audio_stream_get_node_name(va_state.input.stream);
        ACAP_STATUS_SetString("input", "bound_node", bound_node ? bound_node : "");
        refresh_available_input_nodes_status();
        LOG("Microphone capture receiving samples\n");
    }

    whisper_stt_feed(samples, num_samples);

    va_state.input.sample_count += num_samples;
    if (va_state.input.sample_count % AUDIO_SAMPLE_RATE < num_samples) {
        ACAP_STATUS_SetNumber("input", "samples", va_state.input.sample_count);
    }
}

// =============================================================================
// WHISPER STT CALLBACKS
// =============================================================================

static void
Whisper_State_Callback(const char *state) {
    ACAP_STATUS_SetString("stt", "state", state);
}

static void
publish_transcript_entry(const char *clean_text, const char *safe_language, int decode_ms, int total_ms, gint64 utterance_start_ms, gboolean ignored, GPtrArray *detected_keywords) {
    char *keywords_csv = join_keywords_csv(detected_keywords);

    LOG("Transcript (%s): %s [decode_ms=%d total_ms=%d]\n", safe_language, clean_text, decode_ms, total_ms);

    history_append(clean_text, safe_language, total_ms, utterance_start_ms, ignored, keywords_csv);

    ACAP_STATUS_SetString("stt", "last_transcript", clean_text);
    ACAP_STATUS_SetString("stt", "last_language", safe_language);
    ACAP_STATUS_SetNumber("stt", "count", ++g_transcript_count);

    if (ignored) {
        g_free(keywords_csv);
        return;
    }

    const char *topic = (g_strcmp0(g_transcription_use_case, "voice_commands") == 0) ? "command" : "transcription";

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "language", safe_language);
    cJSON_AddStringToObject(payload, "text", clean_text);
    cJSON_AddNumberToObject(payload, "timestamp", (double)utterance_start_ms);
    cJSON *keywords = cJSON_AddArrayToObject(payload, "keywords");
    if (detected_keywords) {
        for (guint i = 0; i < detected_keywords->len; i++) {
            cJSON_AddItemToArray(keywords, cJSON_CreateString((const char *)g_ptr_array_index(detected_keywords, i)));
        }
    }

    if (MQTT_Publish_JSON(topic, payload, 0, 0) == 0) {
        LOG_WARN("Failed to publish transcript to MQTT topic '%s'\n", topic);
    }

    cJSON_Delete(payload);
    g_free(keywords_csv);
}

static void
Whisper_Transcript_Callback(const char *text, const char *language, int decode_ms, int total_ms, gint64 utterance_start_ms) {
    const char *safe_language = language ? language : "";
    GPtrArray *parts = split_transcript_candidates(text);
    gboolean any_published = FALSE;

    for (guint i = 0; i < parts->len; i++) {
        const char *candidate = g_ptr_array_index(parts, i);
        char *clean = sanitize_transcript_text(candidate);
        if (!clean || clean[0] == '\0') {
            g_free(clean);
            continue;
        }

        int words = count_words(clean);
        int min_words_required = (g_strcmp0(g_transcription_use_case, "voice_commands") == 0) ? g_command_min_words : 3;
        if (words < min_words_required) {
            LOG("Transcript filtered (%s): too short (%d words, min=%d) -> %s\n", safe_language, words, min_words_required, clean);
            g_free(clean);
            continue;
        }

        GPtrArray *detected_keywords = g_ptr_array_new_with_free_func(g_free);

        if (g_strcmp0(g_transcription_use_case, "voice_commands") == 0) {
            if (!transcript_matches_command_keywords(clean, detected_keywords)) {
                LOG("Transcript filtered (%s): no command keyword match -> %s\n", safe_language, clean);
                publish_transcript_entry(clean, safe_language, decode_ms, total_ms, utterance_start_ms, TRUE, detected_keywords);
                g_ptr_array_free(detected_keywords, TRUE);
                g_free(clean);
                continue;
            }
        }

        publish_transcript_entry(clean, safe_language, decode_ms, total_ms, utterance_start_ms, FALSE, detected_keywords);
        any_published = TRUE;
        g_ptr_array_free(detected_keywords, TRUE);
        g_free(clean);
    }

    if (!any_published) {
        LOG("Transcript filtered (%s): %s\n", safe_language, text ? text : "");
    }

    g_ptr_array_free(parts, TRUE);
}

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t file_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} WavHeader;

static void
Wyoming_Transcript_Callback(const char *text) {
    g_external_request_inflight = FALSE;
    gint64 utterance_start_ms = g_external_request_utterance_start_ms;
    g_external_request_utterance_start_ms = 0;
    int total_ms = 0;
    if (g_external_request_started_ms > 0) {
        total_ms = (int)(now_ms() - g_external_request_started_ms);
        g_external_request_started_ms = 0;
    }

    const char *language = (g_stt_language[0] && g_strcmp0(g_stt_language, "auto") != 0) ? g_stt_language : "";
    Whisper_Transcript_Callback(text, language, total_ms, total_ms, utterance_start_ms > 0 ? utterance_start_ms : now_ms());
    ACAP_STATUS_SetString("stt", "state", "idle");
}

static void
Wyoming_State_Callback(WyomingServiceType service, WyomingConnectionState state, const char* error_msg) {
    if (service != WYOMING_SERVICE_WHISPER) {
        return;
    }

    switch (state) {
        case WYOMING_STATE_CONNECTED:
            ACAP_STATUS_SetString("wyoming", "whisper", "connected");
            break;
        case WYOMING_STATE_CONNECTING:
            ACAP_STATUS_SetString("wyoming", "whisper", "connecting");
            break;
        case WYOMING_STATE_ERROR:
            g_external_request_inflight = FALSE;
            g_external_request_started_ms = 0;
            g_external_request_utterance_start_ms = 0;
            ACAP_STATUS_SetString("wyoming", "whisper", error_msg ? error_msg : "error");
            break;
        default:
            ACAP_STATUS_SetString("wyoming", "whisper", "disconnected");
            break;
    }
}

static void
Whisper_External_Transcribe_Callback(const float *samples, guint32 count, const char *language, gboolean translate, gint64 utterance_start_ms) {
    (void)language;
    (void)translate;

    if (g_strcmp0(g_stt_mode, "external") != 0) {
        return;
    }

    if (g_wyoming_host[0] == '\0') {
        LOG_WARN("External transcription skipped: wyoming_host not configured\n");
        return;
    }

    if (g_external_request_inflight) {
        gint64 age_ms = now_ms() - g_external_request_started_ms;
        if (age_ms < EXTERNAL_ASR_STALE_MS) {
            LOG_WARN("Skipping external ASR utterance while previous request is in progress (age_ms=%lld)\n", (long long)age_ms);
            return;
        }

        LOG_WARN("External ASR request considered stale (age_ms=%lld), forcing recovery\n", (long long)age_ms);
        g_external_request_inflight = FALSE;
        g_external_request_started_ms = 0;
        g_external_request_utterance_start_ms = 0;
    }

    if (!wyoming_is_connected(WYOMING_SERVICE_WHISPER)) {
        wyoming_connect(WYOMING_SERVICE_WHISPER);
    }

    size_t pcm_size = count * sizeof(int16_t);
    size_t wav_size = sizeof(WavHeader) + pcm_size;
    unsigned char *wav = g_malloc0(wav_size);
    int16_t *pcm = (int16_t *)(wav + sizeof(WavHeader));

    for (guint32 i = 0; i < count; i++) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = (int16_t)(s * 32767.0f);
    }

    WavHeader header = {
        .riff = {'R','I','F','F'},
        .file_size = (uint32_t)(wav_size - 8),
        .wave = {'W','A','V','E'},
        .fmt = {'f','m','t',' '},
        .fmt_size = 16,
        .audio_format = 1,
        .num_channels = 1,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .byte_rate = AUDIO_SAMPLE_RATE * 2,
        .block_align = 2,
        .bits_per_sample = 16,
        .data = {'d','a','t','a'},
        .data_size = (uint32_t)pcm_size
    };

    memcpy(wav, &header, sizeof(WavHeader));

    g_external_request_started_ms = now_ms();
    g_external_request_utterance_start_ms = utterance_start_ms;
    g_external_request_inflight = TRUE;
    ACAP_STATUS_SetString("stt", "state", "transcribing");
    if (wyoming_asr_transcribe(wav, wav_size) != 0) {
        LOG_WARN("Failed to submit utterance to external Whisper\n");
        ACAP_STATUS_SetString("stt", "state", "idle");
        g_external_request_inflight = FALSE;
        g_external_request_started_ms = 0;
        g_external_request_utterance_start_ms = 0;
    }

    g_free(wav);
}

// =============================================================================
// HTTP ENDPOINTS
// =============================================================================

// Recent transcript history for the Transcriptions tab, newest first.
void
HTTP_Endpoint_transcripts(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char *method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "GET") == 0) {
        cJSON *array = cJSON_CreateArray();

        g_mutex_lock(&g_history_mutex);
        int count = g_history_count;
        int idx = g_history_next;
        for (int i = 0; i < count; i++) {
            idx = (idx - 1 + TRANSCRIPT_HISTORY_SIZE) % TRANSCRIPT_HISTORY_SIZE;
            TranscriptEntry *e = &g_history[idx];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "ts", (double)e->ts_ms);
            cJSON_AddNumberToObject(item, "utterance_start_ms", (double)e->utterance_start_ms);
            cJSON_AddNumberToObject(item, "duration_ms", e->duration_ms);
            cJSON_AddBoolToObject(item, "ignored", e->ignored);
            cJSON_AddStringToObject(item, "language", e->language);
            cJSON_AddStringToObject(item, "text", e->text ? e->text : "");
            cJSON *keywords = cJSON_AddArrayToObject(item, "keywords");
            if (e->keywords_csv && e->keywords_csv[0]) {
                gchar **parts = g_strsplit(e->keywords_csv, ",", -1);
                for (gint p = 0; parts && parts[p]; p++) {
                    if (parts[p][0]) {
                        cJSON_AddItemToArray(keywords, cJSON_CreateString(parts[p]));
                    }
                }
                g_strfreev(parts);
            }
            cJSON_AddItemToArray(array, item);
        }
        g_mutex_unlock(&g_history_mutex);

        ACAP_HTTP_Respond_JSON(response, array);
        cJSON_Delete(array);
        return;
    }

    if (strcmp(method, "DELETE") == 0) {
        history_clear();
        g_transcript_count = 0;
        ACAP_STATUS_SetString("stt", "last_transcript", "");
        ACAP_STATUS_SetString("stt", "last_language", "");
        ACAP_STATUS_SetNumber("stt", "count", 0);
        ACAP_HTTP_Respond_Text(response, "OK");
        return;
    }

    ACAP_HTTP_Respond_Error(response, 400, "Only GET and DELETE methods allowed");
}

// =============================================================================
// MQTT
// =============================================================================

void
MQTT_Connection_Status(int state) {
    char topic[64];
    cJSON *message = NULL;

    switch (state) {
        case MQTT_INITIALIZING:
            LOG("MQTT: Initializing\n");
            break;
        case MQTT_CONNECTING:
            LOG("MQTT: Connecting\n");
            break;
        case MQTT_CONNECTED:
        case MQTT_RECONNECTED:
            LOG("MQTT: %s\n", state == MQTT_CONNECTED ? "Connected" : "Reconnected");
            snprintf(topic, sizeof(topic), "connect/%s", device_serial);
            message = cJSON_CreateObject();
            cJSON_AddTrueToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            cJSON_AddStringToObject(message, "service", "voice");
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_DISCONNECTING:
            LOG("MQTT: Disconnecting\n");
            snprintf(topic, sizeof(topic), "connect/%s", device_serial);
            message = cJSON_CreateObject();
            cJSON_AddFalseToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            cJSON_AddStringToObject(message, "service", "voice");
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_DISCONNECTED:
            LOG("MQTT: Disconnected\n");
            break;
    }
}

void
MQTT_Message_Handler(const char *topic, const char *payload) {
    LOG_WARN("MQTT: Unhandled topic: %s = %s\n", topic, payload);
}

// =============================================================================
// MAIN
// =============================================================================

static gboolean
signal_handler(gpointer user_data) {
    (void)user_data;
    LOG("Received SIGTERM, initiating shutdown\n");
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}

int main(void) {
    openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);
    LOG("------ Starting Voice Transcriptions ACAP ------\n");

    g_mutex_init(&g_history_mutex);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    ACAP(APP_PACKAGE, Settings_Updated_Callback);

    const char *serial = ACAP_DEVICE_Prop("serial");
    if (serial) {
        strncpy(device_serial, serial, sizeof(device_serial) - 1);
        device_serial[sizeof(device_serial) - 1] = '\0';
        LOG("Device serial: %s\n", device_serial);
    } else {
        LOG_WARN("Failed to get device serial number\n");
        strcpy(device_serial, "UNKNOWN");
    }

    ACAP_HTTP_Node("transcripts", HTTP_Endpoint_transcripts);

    MQTT_Init(MQTT_Connection_Status, MQTT_Message_Handler);

    ACAP_STATUS_SetBool("input", "state", 0);
    ACAP_STATUS_SetNumber("input", "samples", 0);
    ACAP_STATUS_SetNumber("input", "rms", 0.0);
    ACAP_STATUS_SetNumber("input", "peak", 0.0);
    ACAP_STATUS_SetString("input", "status", "Not running");
    ACAP_STATUS_SetString("input", "selected_node", g_input_node);
    ACAP_STATUS_SetString("input", "bound_node", "");
    ACAP_STATUS_SetObject("input", "available_nodes", cJSON_CreateArray());

    ACAP_STATUS_SetString("stt", "state", "idle");
    ACAP_STATUS_SetString("stt", "last_transcript", "");
    ACAP_STATUS_SetString("stt", "last_language", "");
    ACAP_STATUS_SetNumber("stt", "count", 0);
    ACAP_STATUS_SetString("stt", "backend", g_stt_mode);
    ACAP_STATUS_SetString("stt", "use_case", g_transcription_use_case);
    ACAP_STATUS_SetString("stt", "command_keywords", g_command_keywords);
    ACAP_STATUS_SetNumber("stt", "command_min_words", g_command_min_words);
    ACAP_STATUS_SetNumber("stt", "command_keyword_count", 0);
    ACAP_STATUS_SetString("wyoming", "whisper", "disconnected");
    ACAP_STATUS_SetString("wizard", "environment", g_wizard_environment);
    ACAP_STATUS_SetString("wizard", "goal", g_wizard_goal);
    ACAP_STATUS_SetString("wizard", "sporadic_noise", g_wizard_sporadic_noise);
    ACAP_STATUS_SetNumber("wizard", "validation_rounds", g_wizard_validation_rounds);
    ACAP_STATUS_SetNumber("wizard", "background_rounds", g_wizard_background_rounds);
    ACAP_STATUS_SetNumber("wizard", "distance_rounds", g_wizard_distance_rounds);

    LOG("Entering main loop\n");
    main_loop = g_main_loop_new(NULL, FALSE);

    // CRITICAL: Initialize PipeWire audio with the main loop context
    pw_audio_init(g_main_loop_get_context(main_loop));

    wyoming_init(main_loop);
    wyoming_set_transcript_callback(Wyoming_Transcript_Callback);
    wyoming_set_state_callback(Wyoming_State_Callback);

    whisper_stt_set_transcript_callback(Whisper_Transcript_Callback);
    whisper_stt_set_state_callback(Whisper_State_Callback);

    cJSON *settings = ACAP_Get_Config("settings");
    resolve_model_from_settings(settings);

    char model_path[512];
    snprintf(model_path, sizeof(model_path), "%s%s", ACAP_FILE_AppPath(), g_model_filename);
    ACAP_STATUS_SetString("stt", "model", g_model_name);
    LOG("whisper_stt: selected model=%s file=%s\n", g_model_name, g_model_filename);

    if (whisper_stt_init(model_path) == 0) {
        if (settings) {
            apply_settings(settings);
        }
    } else {
        LOG_WARN("Failed to initialize whisper STT engine (model: %s)\n", model_path);
        ACAP_STATUS_SetString("stt", "state", "error");
    }

    start_input_capture();

    // Setup signal handlers
    GSource *signal_source = g_unix_signal_source_new(SIGTERM);
    if (signal_source) {
        g_source_set_callback(signal_source, signal_handler, NULL, NULL);
        g_source_attach(signal_source, NULL);
    } else {
        LOG_WARN("Signal detection failed");
    }

    g_main_loop_run(main_loop);
    LOG("Terminating and cleaning up %s\n", APP_PACKAGE);

    MQTT_Connection_Status(MQTT_DISCONNECTING);
    MQTT_Cleanup();

    wyoming_cleanup();

    whisper_stt_cleanup();

    if (g_command_keywords_normalized) {
        g_ptr_array_free(g_command_keywords_normalized, TRUE);
        g_command_keywords_normalized = NULL;
    }

    stop_input_capture();

    history_clear();
    g_mutex_clear(&g_history_mutex);

    curl_global_cleanup();
    ACAP_Cleanup();
    closelog();
    return 0;
}

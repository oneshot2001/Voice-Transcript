#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <math.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "pipewire_audio.h"
#include "wyoming.h"
#include "ACAP.h"
#include "cJSON.h"
#include "MQTT.h"

#define APP_PACKAGE	"voice"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    {}

#define AUDIO_SAMPLE_RATE 16000

// Forward declarations
void start_recording(void);
void stop_recording(void);

// =============================================================================
// VOICE ASSISTANT STATE MANAGEMENT
// =============================================================================

// PipeWire Audio Streams
typedef struct {
    PWAudio *stream;           // PipeWire stream handle
    gboolean active;           // Is stream currently active?
    guint32 sample_count;      // Total samples processed
} AudioStream;

// Playback buffer for downloaded WAV files
typedef struct {
    float *samples;            // Audio samples (F32)
    guint32 size;              // Total samples allocated
    guint32 write_pos;         // Write position (for loading)
    guint32 read_pos;          // Read position (for playback)
    guint32 sample_rate;       // Sample rate from WAV file
    gboolean ready;            // Ready for playback
} PlaybackBuffer;

// Recording buffer for STT
typedef struct {
    float *samples;            // Audio samples (F32)
    guint32 capacity;          // Buffer capacity in samples
    guint32 size;              // Current number of samples recorded
    guint32 sample_rate;       // Sample rate (16000)
    gboolean recording;        // Currently recording?
} RecordingBuffer;

// Main application state
typedef struct {
    // Audio streams (independent)
    AudioStream input;         // Continuous input (future: wake-word detection)
    AudioStream output;        // Playback output

    // Playback buffer
    PlaybackBuffer playback;

    // Recording buffer (for STT)
    RecordingBuffer recording;

    // Download state
    gchar *download_url;
    gboolean downloading;

    // Synchronization for blocking HTTP response until playback completes
    GMutex playback_mutex;
    GCond playback_cond;
    gboolean playback_complete;

} VoiceAssistantState;

static VoiceAssistantState va_state = {0};
static char device_serial[32] = {0};  // Device serial number

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

void
Settings_Updated_Callback( const char* service, cJSON* data) {
	char* json = cJSON_PrintUnformatted(data);
	LOG_TRACE("%s: Service=%s Data=%s\n",__func__, service, json);
	free(json);
}

void
audio_error_callback(const GError *error, gpointer userdata) {
    (void)userdata;
    LOG_WARN("Audio error: %s\n", error ? error->message : "Unknown error");
    ACAP_STATUS_SetString("output", "error", error ? error->message : "Unknown error");
}

// Forward declarations
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

static gboolean start_playback_idle(gpointer user_data);

// Wyoming connection state callback
void
wyoming_state_callback(WyomingServiceType service, WyomingConnectionState state, const char* error_msg) {
    const char* service_name = (service == WYOMING_SERVICE_PIPER) ? "piper" : "whisper";
    const char* state_names[] = {"disconnected", "connecting", "connected", "error"};

    LOG("Wyoming %s: %s%s%s\n", service_name, state_names[state],
        error_msg ? " - " : "", error_msg ? error_msg : "");

    // Update status
    ACAP_STATUS_SetString("wyoming", service_name, state_names[state]);
    if (error_msg) {
        ACAP_STATUS_SetString("wyoming", "error", error_msg);
    }
}

// Wyoming ASR transcript callback - receives transcription from Whisper
void
wyoming_transcript_callback(const char* text) {
    LOG("Wyoming STT: Received transcription: %s\n", text);

    // Update status
    ACAP_STATUS_SetString("stt", "status", "Transcription complete");
    ACAP_STATUS_SetString("stt", "last_transcript", text);

    // Publish transcription to MQTT topic: voice/transcript/{SERIAL}
    char topic[128];
    snprintf(topic, sizeof(topic), "transcript/%s", device_serial);

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "text", text);

    int result = MQTT_Publish_JSON(topic, payload, 0, 0);
    if (result == 0) {
        LOG("STT: Published transcription to MQTT topic '%s'\n", topic);
    } else {
        LOG_WARN("STT: Failed to publish transcription to MQTT topic '%s'\n", topic);
    }

    cJSON_Delete(payload);
}

// Wyoming TTS audio callback - receives WAV data from Piper
void
wyoming_audio_callback(WyomingServiceType service, const unsigned char* data, size_t length) {
    (void)service;  // We know it's from Piper for TTS

    LOG("Wyoming TTS: Received %zu bytes of audio data\n", length);

    // Load WAV data into playback buffer
    // For now, we'll treat the data as complete WAV file
    // TODO: Might need to handle chunked data better

    // Ensure playback buffer is clear
    if (va_state.playback.samples) {
        free(va_state.playback.samples);
        va_state.playback.samples = NULL;
    }

    // Parse WAV header
    if (length < sizeof(WavHeader)) {
        LOG_WARN("Wyoming TTS: Audio data too small for WAV header\n");
        return;
    }

    WavHeader *header = (WavHeader*)data;

    // Validate WAV header
    if (memcmp(header->riff, "RIFF", 4) != 0 || memcmp(header->wave, "WAVE", 4) != 0) {
        LOG_WARN("Wyoming TTS: Invalid WAV header\n");
        return;
    }

    LOG("WAV: %u Hz, %u channels, %u bits, format=%u\n",
        header->sample_rate, header->num_channels, header->bits_per_sample, header->audio_format);

    // PCM data follows immediately after the header
    // Our WavHeader struct already includes the "data" chunk marker and size
    const unsigned char *ptr = data + sizeof(WavHeader);
    const unsigned char *end = data + length;
    uint32_t data_size = header->data_size;

    if (ptr + data_size > end) {
        LOG_WARN("WAV data extends beyond buffer (expected %u bytes, have %zu)\n",
                 data_size, end - ptr);
        return;
    }

    // Convert PCM16 to F32
    if (header->audio_format == 1 && header->bits_per_sample == 16 && header->num_channels == 1) {
        guint32 num_samples = data_size / 2;
        va_state.playback.samples = malloc(num_samples * sizeof(float));

        if (!va_state.playback.samples) {
            LOG_WARN("Failed to allocate playback buffer\n");
            return;
        }

        const int16_t *pcm16 = (const int16_t*)ptr;
        for (guint32 i = 0; i < num_samples; i++) {
            va_state.playback.samples[i] = pcm16[i] / 32768.0f;
        }

        va_state.playback.size = num_samples;
        va_state.playback.write_pos = num_samples;
        va_state.playback.sample_rate = header->sample_rate;
        va_state.playback.read_pos = 0;
        va_state.playback.ready = TRUE;

        LOG("TTS audio loaded: %u samples (%.2f seconds at %u Hz)\n",
            num_samples, (float)num_samples / header->sample_rate, header->sample_rate);

        // Publish MQTT status
        cJSON* status = cJSON_CreateObject();
        cJSON_AddStringToObject(status, "status", "playing");
        cJSON_AddNumberToObject(status, "samples", num_samples);
        cJSON_AddNumberToObject(status, "duration", (float)num_samples / header->sample_rate);
        cJSON_AddNumberToObject(status, "sample_rate", header->sample_rate);
        MQTT_Publish_JSON("voice/tts/status", status, 0, 0);
        cJSON_Delete(status);

        // Start playback
        g_idle_add(start_playback_idle, NULL);
    } else {
        LOG_WARN("Unsupported WAV format: format=%u bits=%u channels=%u\n",
                 header->audio_format, header->bits_per_sample, header->num_channels);
    }
}

// =============================================================================
// WAV FILE HANDLING
// =============================================================================

// WavHeader is already defined in forward declarations above

// Convert 16-bit PCM to 32-bit float (-1.0 to 1.0)
static void
pcm16_to_float32(const int16_t *pcm, float *samples, guint32 count) {
    for (guint32 i = 0; i < count; i++) {
        samples[i] = (float)pcm[i] / 32768.0f;
    }
}

// Memory buffer for curl download
typedef struct {
    uint8_t *data;
    size_t size;
    size_t allocated;
} MemoryBuffer;

static size_t
wav_download_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    uint8_t *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        LOG_WARN("wav_download_write_callback: Out of memory\n");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

// =============================================================================
// AUDIO CALLBACKS
// =============================================================================

// Input audio callback - captures audio for STT when recording is active
void
audio_input_callback(struct spa_buffer *buf, guint64 ts, gpointer userdata) {
    (void)ts;
    (void)userdata;

    if (!va_state.recording.recording) {
        // Not recording, just return
        return;
    }

    // Get audio data from PipeWire buffer
    struct spa_data *data = buf->datas;
    float *samples = (float *)data->data;
    guint32 num_samples = data->chunk->size / sizeof(float);

    // Log first capture
    if (va_state.recording.size == 0) {
        LOG("STT: Audio capture started (%u samples received)\n", num_samples);
    }

    // Ensure we have capacity in recording buffer
    if (va_state.recording.size + num_samples > va_state.recording.capacity) {
        // Expand buffer capacity
        guint32 new_capacity = va_state.recording.capacity * 2;
        if (new_capacity < va_state.recording.size + num_samples) {
            new_capacity = va_state.recording.size + num_samples + 16000;  // Add 1 second headroom
        }

        float *new_buffer = realloc(va_state.recording.samples, new_capacity * sizeof(float));
        if (!new_buffer) {
            LOG_WARN("Failed to expand recording buffer\n");
            return;
        }

        va_state.recording.samples = new_buffer;
        va_state.recording.capacity = new_capacity;
        LOG_TRACE("Recording buffer expanded to %u samples\n", new_capacity);
    }

    // Copy samples to recording buffer
    memcpy(va_state.recording.samples + va_state.recording.size, samples, num_samples * sizeof(float));
    va_state.recording.size += num_samples;

    // Update input stream sample count
    va_state.input.sample_count += num_samples;

    // Update status every 16000 samples (~1 second at 16kHz)
    if (va_state.recording.size % 16000 < num_samples) {
        ACAP_STATUS_SetNumber("stt", "samples", va_state.recording.size);
        ACAP_STATUS_SetNumber("stt", "duration", (float)va_state.recording.size / AUDIO_SAMPLE_RATE);
    }
}

// Idle callback to cleanup after playback completes
static gboolean
playback_complete_idle(gpointer user_data) {
    (void)user_data;

    LOG("playback_complete_idle: Cleaning up after playback\n");

    // Stop and destroy stream
    if (va_state.output.stream) {
        pw_audio_stop(va_state.output.stream);
        va_state.output.stream = NULL;
        LOG("Stream stopped and destroyed\n");
    }

    ACAP_STATUS_SetString("output", "status", "Playback complete, ready for next file");
    LOG("State cleared: active=%d, stream=%p, ready for next playback\n",
        va_state.output.active, (void*)va_state.output.stream);

    // Signal waiting HTTP handler that playback is complete
    g_mutex_lock(&va_state.playback_mutex);
    va_state.playback_complete = TRUE;
    g_cond_signal(&va_state.playback_cond);
    g_mutex_unlock(&va_state.playback_mutex);

    return G_SOURCE_REMOVE;
}

// Output audio callback (playback from buffer)
void
audio_output_callback(struct spa_buffer *buf, guint64 reqsize, gpointer userdata) {
    (void)userdata;

    if (!va_state.output.active || !va_state.playback.ready) {
        // Fill with silence
        uint8_t *dst = buf->datas[0].data;
        if (dst) {
            int stride = sizeof(float) * PW_AUDIO_MONO;
            int n_frames = buf->datas[0].maxsize / stride;
            memset(dst, 0, n_frames * stride);
            buf->datas[0].chunk->offset = 0;
            buf->datas[0].chunk->stride = stride;
            buf->datas[0].chunk->size = n_frames * stride;
        }
        return;
    }

    uint8_t *dst = buf->datas[0].data;
    if (dst == NULL) {
        LOG_WARN("audio_output_callback: NULL data pointer\n");
        return;
    }

    int stride = sizeof(float) * PW_AUDIO_MONO;
    int n_frames = buf->datas[0].maxsize / stride;

    if (reqsize) {
        n_frames = (reqsize < (guint64)n_frames) ? (int)reqsize : n_frames;
    }

    // Copy from playback buffer
    int frames_to_copy = n_frames;
    guint32 remaining = va_state.playback.write_pos - va_state.playback.read_pos;

    if (frames_to_copy > (int)remaining) {
        frames_to_copy = remaining;
    }

    for (int i = 0; i < frames_to_copy; i++) {
        float val = va_state.playback.samples[va_state.playback.read_pos++];
        memcpy(dst, &val, sizeof(float));
        dst += stride;
    }

    // Fill rest with silence
    for (int i = frames_to_copy; i < n_frames; i++) {
        float val = 0.0f;
        memcpy(dst, &val, sizeof(float));
        dst += stride;
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = n_frames * stride;

    va_state.output.sample_count += frames_to_copy;

    // Update status
    ACAP_STATUS_SetNumber("output", "samples", va_state.output.sample_count);

    // Check if playback finished
    if (va_state.playback.read_pos >= va_state.playback.write_pos) {
        LOG("Playback completed (%u samples), scheduling cleanup\n", va_state.output.sample_count);

        // Clear flags FIRST to prevent race conditions
        // Do this in the callback to immediately stop playback
        va_state.output.active = FALSE;
        va_state.playback.ready = FALSE;
        ACAP_STATUS_SetBool("output", "state", 0);

        // Publish MQTT completion status
        cJSON* status = cJSON_CreateObject();
        cJSON_AddStringToObject(status, "status", "completed");
        cJSON_AddNumberToObject(status, "samples_played", va_state.output.sample_count);
        MQTT_Publish_JSON("voice/tts/status", status, 0, 0);
        cJSON_Delete(status);

        // IMPORTANT: Don't call pw_audio_stop() from within the audio callback!
        // Schedule cleanup in main thread via idle callback
        g_idle_add(playback_complete_idle, NULL);
    }
}

// =============================================================================
// WAV DOWNLOAD AND PLAYBACK
// =============================================================================

// Download WAV file and convert to F32 samples
static gboolean
download_and_load_wav(const char *url) {
    CURL *curl;
    CURLcode res;
    MemoryBuffer chunk = {0};

    LOG("Downloading WAV from: %s\n", url);
    ACAP_STATUS_SetString("output", "status", "Downloading WAV file...");

    curl = curl_easy_init();
    if (!curl) {
        LOG_WARN("Failed to initialize curl\n");
        return FALSE;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wav_download_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);  // 10 seconds to connect
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);          // 60 seconds total timeout
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);   // Follow redirects
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);          // Enable verbose logging

    LOG("Starting download...\n");
    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        LOG_WARN("curl_easy_perform() failed: %s (code %d)\n", curl_easy_strerror(res), res);

        // Get more detailed error info
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code) {
            LOG_WARN("HTTP response code: %ld\n", http_code);
        }

        curl_easy_cleanup(curl);
        free(chunk.data);

        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "%s (HTTP %ld)", curl_easy_strerror(res), http_code);
        ACAP_STATUS_SetString("output", "error", error_msg);
        return FALSE;
    }

    curl_easy_cleanup(curl);

    LOG("Downloaded %zu bytes\n", chunk.size);

    // Parse WAV header
    if (chunk.size < sizeof(WavHeader)) {
        LOG_WARN("File too small to be a valid WAV\n");
        free(chunk.data);
        ACAP_STATUS_SetString("output", "error", "Invalid WAV file");
        return FALSE;
    }

    WavHeader *hdr = (WavHeader *)chunk.data;

    // Validate WAV format
    if (memcmp(hdr->riff, "RIFF", 4) != 0 || memcmp(hdr->wave, "WAVE", 4) != 0) {
        LOG_WARN("Not a valid WAV file\n");
        free(chunk.data);
        ACAP_STATUS_SetString("output", "error", "Not a valid WAV file");
        return FALSE;
    }

    LOG("WAV: %d Hz, %d channels, %d bits, format=%d\n",
        hdr->sample_rate, hdr->num_channels, hdr->bits_per_sample, hdr->audio_format);

    // Validate format (16-bit PCM, mono, 16kHz)
    if (hdr->audio_format != 1) {
        LOG_WARN("Only PCM format supported (got format %d)\n", hdr->audio_format);
        free(chunk.data);
        ACAP_STATUS_SetString("output", "error", "Only PCM format supported");
        return FALSE;
    }

    if (hdr->num_channels != 1) {
        LOG_WARN("Only mono audio supported (got %d channels)\n", hdr->num_channels);
        free(chunk.data);
        ACAP_STATUS_SetString("output", "error", "Only mono audio supported");
        return FALSE;
    }

    if (hdr->bits_per_sample != 16) {
        LOG_WARN("Only 16-bit audio supported (got %d bits)\n", hdr->bits_per_sample);
        free(chunk.data);
        ACAP_STATUS_SetString("output", "error", "Only 16-bit audio supported");
        return FALSE;
    }

    // Convert PCM16 to F32
    guint32 num_samples = hdr->data_size / sizeof(int16_t);
    int16_t *pcm_data = (int16_t *)(chunk.data + sizeof(WavHeader));

    LOG("Converting %u samples from PCM16 to F32...\n", num_samples);

    // Allocate playback buffer
    va_state.playback.samples = (float *)realloc(va_state.playback.samples, num_samples * sizeof(float));
    if (!va_state.playback.samples) {
        LOG_WARN("Failed to allocate playback buffer\n");
        free(chunk.data);
        ACAP_STATUS_SetString("output", "error", "Out of memory");
        return FALSE;
    }

    pcm16_to_float32(pcm_data, va_state.playback.samples, num_samples);

    va_state.playback.size = num_samples;
    va_state.playback.write_pos = num_samples;
    va_state.playback.read_pos = 0;
    va_state.playback.sample_rate = hdr->sample_rate;  // Store sample rate
    va_state.playback.ready = TRUE;

    free(chunk.data);

    LOG("WAV loaded: %u samples (%.2f seconds at %d Hz)\n",
        num_samples, (float)num_samples / hdr->sample_rate, hdr->sample_rate);

    ACAP_STATUS_SetNumber("output", "size", num_samples);
    ACAP_STATUS_SetString("output", "status", "WAV file loaded, ready for playback");

    return TRUE;
}

// Idle callback to start playback from main thread
static gboolean
start_playback_idle(gpointer user_data) {
    (void)user_data;

    LOG("start_playback_idle: Starting audio playback from main thread (sample_rate=%u Hz)\n",
        va_state.playback.sample_rate);

    // Mark as active NOW (in main thread, right before starting stream)
    va_state.output.active = TRUE;

    // Reset sample counter
    va_state.output.sample_count = 0;

    // Start playback stream with the WAV file's sample rate
    va_state.output.stream = pw_audio_playback_start(
        SPA_AUDIO_FORMAT_F32,
        va_state.playback.sample_rate,
        PW_AUDIO_MONO,
        audio_error_callback,
        NULL
    );

    if (!va_state.output.stream) {
        va_state.output.active = FALSE;
        ACAP_STATUS_SetBool("output", "state", 0);
        ACAP_STATUS_SetString("output", "status", "Failed to start playback");
        ACAP_STATUS_SetString("output", "error", "Failed to create PipeWire stream");
        LOG_WARN("Failed to start audio playback\n");

        // Signal error to waiting HTTP handler
        g_mutex_lock(&va_state.playback_mutex);
        va_state.playback_complete = TRUE;
        g_cond_signal(&va_state.playback_cond);
        g_mutex_unlock(&va_state.playback_mutex);

        return G_SOURCE_REMOVE;
    }

    pw_audio_set_on_buffer_cb(va_state.output.stream, audio_output_callback, NULL);
    pw_audio_enable_debug(va_state.output.stream, TRUE);

    ACAP_STATUS_SetBool("output", "state", 1);
    ACAP_STATUS_SetString("output", "status", "Playing audio...");
    LOG("Playback stream started successfully\n");

    return G_SOURCE_REMOVE;
}

// =============================================================================
// HTTP ENDPOINTS
// =============================================================================

// Test endpoint to verify network connectivity
void
HTTP_Endpoint_test_download(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

    if (!method || strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Only GET method allowed");
        return;
    }

    // Test downloading from a known-good public URL
    const char* test_url = "http://httpbin.org/get";

    CURL *curl = curl_easy_init();
    if (!curl) {
        ACAP_HTTP_Respond_Error(response, 500, "Failed to initialize curl");
        return;
    }

    MemoryBuffer chunk = {0};

    curl_easy_setopt(curl, CURLOPT_URL, test_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wav_download_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    LOG("Testing download from: %s\n", test_url);
    CURLcode res = curl_easy_perform(curl);

    char response_msg[512];
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        snprintf(response_msg, sizeof(response_msg),
                 "SUCCESS: Downloaded %zu bytes from %s (HTTP %ld)",
                 chunk.size, test_url, http_code);
        LOG("%s\n", response_msg);
        ACAP_HTTP_Respond_Text(response, response_msg);
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        snprintf(response_msg, sizeof(response_msg),
                 "FAILED: %s (HTTP %ld)", curl_easy_strerror(res), http_code);
        LOG_WARN("%s\n", response_msg);
        ACAP_HTTP_Respond_Error(response, 500, response_msg);
    }

    curl_easy_cleanup(curl);
    free(chunk.data);
}

// Test raw TCP connection endpoint
void
HTTP_Endpoint_test_tcp(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

    if (!method || strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Only GET method allowed");
        return;
    }

    const char* host = ACAP_HTTP_Request_Param(request, "host");
    const char* port_str = ACAP_HTTP_Request_Param(request, "port");

    if (!host || !port_str) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing host or port parameter");
        return;
    }

    int port = atoi(port_str);

    LOG("Testing TCP connection to %s:%d\n", host, port);

    // Create blocking socket for simpler testing
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Socket creation failed: %s", strerror(errno));
        ACAP_HTTP_Respond_Error(response, 500, error_msg);
        return;
    }

    // Set timeout for connect
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        close(sockfd);
        ACAP_HTTP_Respond_Error(response, 400, "Invalid IP address");
        return;
    }

    // Try to connect (blocking with timeout)
    int ret = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));

    char result_msg[512];
    if (ret == 0) {
        // Connection successful!
        snprintf(result_msg, sizeof(result_msg),
                 "SUCCESS: Connected to %s:%d (socket_fd=%d)", host, port, sockfd);
        LOG("%s\n", result_msg);
        close(sockfd);
        ACAP_HTTP_Respond_Text(response, result_msg);
    } else {
        // Connection failed
        snprintf(result_msg, sizeof(result_msg),
                 "FAILED: Could not connect to %s:%d - %s (errno=%d)",
                 host, port, strerror(errno), errno);
        LOG_WARN("%s\n", result_msg);
        close(sockfd);
        ACAP_HTTP_Respond_Error(response, 500, result_msg);
    }
}

// Test Wyoming connection endpoint
void
HTTP_Endpoint_test_wyoming(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

    if (!method || strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Only GET method allowed");
        return;
    }

    // Get which service to test (piper or whisper)
    const char* service_param = ACAP_HTTP_Request_Param(request, "service");
    WyomingServiceType service = WYOMING_SERVICE_PIPER;

    if (service_param && strcmp(service_param, "whisper") == 0) {
        service = WYOMING_SERVICE_WHISPER;
    }

    const char* service_name = (service == WYOMING_SERVICE_PIPER) ? "Piper" : "Whisper";

    LOG("Testing Wyoming %s connection\n", service_name);

    // Try to connect
    if (wyoming_connect(service) != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Failed to connect to Wyoming %s: %s",
                 service_name, wyoming_get_error(service));
        LOG_WARN("%s\n", error_msg);
        ACAP_HTTP_Respond_Error(response, 500, error_msg);
        return;
    }

    // Wait a bit for connection to establish
    LOG("Waiting for connection to establish...\n");
    for (int i = 0; i < 50; i++) {  // Wait up to 5 seconds
        if (wyoming_is_connected(service)) {
            const char* info = wyoming_get_connection_info(service);
            LOG("Wyoming connection successful: %s\n", info);
            ACAP_HTTP_Respond_Text(response, info);
            return;
        }

        WyomingConnectionState state = wyoming_get_state(service);
        if (state == WYOMING_STATE_ERROR) {
            const char* error = wyoming_get_error(service);
            LOG_WARN("Wyoming connection failed: %s\n", error);
            ACAP_HTTP_Respond_Error(response, 500, error);
            return;
        }

        usleep(100000);  // 100ms
    }

    // Timeout
    LOG_WARN("Wyoming connection timeout\n");
    wyoming_disconnect(service);
    ACAP_HTTP_Respond_Error(response, 504, "Connection timeout");
}

// TTS Speak endpoint
void
HTTP_Endpoint_speak(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Only POST method allowed");
        return;
    }

    const char* contentType = ACAP_HTTP_Get_Content_Type(request);
    if (!contentType || strcmp(contentType, "application/json") != 0) {
        ACAP_HTTP_Respond_Error(response, 415, "Content-Type must be application/json");
        return;
    }

    if (!request->postData || request->postDataLength == 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing POST data");
        return;
    }

    // Parse JSON request
    cJSON* body = cJSON_Parse(request->postData);
    if (!body) {
        ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON");
        return;
    }

    // Get text to speak (required)
    cJSON* text_item = cJSON_GetObjectItem(body, "text");
    if (!text_item || !cJSON_IsString(text_item) || strlen(text_item->valuestring) == 0) {
        cJSON_Delete(body);
        ACAP_HTTP_Respond_Error(response, 400, "Missing or empty 'text' field");
        return;
    }
    const char* text = text_item->valuestring;

    // Get voice (optional)
    cJSON* voice_item = cJSON_GetObjectItem(body, "voice");
    const char* voice = (voice_item && cJSON_IsString(voice_item)) ? voice_item->valuestring : NULL;

    LOG("TTS Speak request: text='%s' voice='%s'\n", text, voice ? voice : "default");

    // Check if Wyoming Piper is connected
    if (!wyoming_is_connected(WYOMING_SERVICE_PIPER)) {
        LOG("Piper not connected, attempting connection...\n");
        if (wyoming_connect(WYOMING_SERVICE_PIPER) != 0) {
            cJSON_Delete(body);
            ACAP_HTTP_Respond_Error(response, 503, "Failed to connect to TTS service");
            return;
        }

        // Wait for connection
        for (int i = 0; i < 30; i++) {
            if (wyoming_is_connected(WYOMING_SERVICE_PIPER)) {
                break;
            }
            usleep(100000);  // 100ms
        }

        if (!wyoming_is_connected(WYOMING_SERVICE_PIPER)) {
            cJSON_Delete(body);
            ACAP_HTTP_Respond_Error(response, 503, "TTS service connection timeout");
            return;
        }
    }

    // Send TTS request
    if (wyoming_tts_synthesize(text, voice) != 0) {
        cJSON_Delete(body);
        ACAP_HTTP_Respond_Error(response, 500, "Failed to send TTS request");
        return;
    }

    cJSON_Delete(body);

    // Response will be "accepted" - audio will play when ready
    ACAP_HTTP_Respond_Text(response, "TTS request accepted, audio will play when ready");
}

// =============================================================================
// STT HTTP ENDPOINTS
// =============================================================================

// Start STT listening endpoint
void
HTTP_Endpoint_listen_start(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Only POST method allowed");
        return;
    }

    if (va_state.recording.recording) {
        ACAP_HTTP_Respond_Error(response, 409, "Already recording");
        return;
    }

    LOG("STT: HTTP listen_start request\n");
    start_recording();
    ACAP_HTTP_Respond_Text(response, "Listening started");
}

// Stop STT listening endpoint - returns transcription as JSON
void
HTTP_Endpoint_listen_stop(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Only POST method allowed");
        return;
    }

    if (!va_state.recording.recording) {
        ACAP_HTTP_Respond_Error(response, 400, "Not recording");
        return;
    }

    LOG("STT: HTTP listen_stop request\n");
    stop_recording();

    // NOTE: For now, we return immediately
    // TODO: Wait for transcription and return JSON with text
    // This requires storing the response object and responding in wyoming_transcript_callback
    ACAP_HTTP_Respond_Text(response, "Listening stopped, check status for transcription");
}

// Get last transcription endpoint
void
HTTP_Endpoint_transcription(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

    if (!method || strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Only GET method allowed");
        return;
    }

    // Get last transcript from status
    const char* last_transcript = ACAP_STATUS_String("stt", "last_transcript");

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "text", last_transcript ? last_transcript : "");

    ACAP_HTTP_Respond_JSON(response, json);

    cJSON_Delete(json);
}

// =============================================================================
// STT RECORDING FUNCTIONS
// =============================================================================

// Delayed start recording callback (gives main loop time to stabilize)
static gboolean delayed_start_recording_callback(gpointer user_data) {
    (void)user_data;

    LOG("STT: Delayed start - initializing input stream\n");

    // Start input stream if not already active
    if (!va_state.input.stream) {
        LOG("STT: Starting input stream\n");
        va_state.input.stream = pw_audio_capture_start(
            SPA_AUDIO_FORMAT_F32,
            AUDIO_SAMPLE_RATE,
            PW_AUDIO_MONO,
            audio_error_callback,
            NULL
        );

        if (va_state.input.stream) {
            pw_audio_set_on_buffer_cb(va_state.input.stream, audio_input_callback, NULL);
            va_state.input.active = TRUE;
            LOG("STT: Input stream started successfully\n");
        } else {
            LOG_WARN("STT: Failed to start input stream\n");
            va_state.recording.recording = FALSE;
        }
    }

    return G_SOURCE_REMOVE;  // One-shot timer
}

// Start recording audio for STT
void start_recording() {
    LOG("STT: Starting recording\n");

    // Initialize recording buffer if needed
    if (!va_state.recording.samples) {
        va_state.recording.capacity = AUDIO_SAMPLE_RATE * 30;  // 30 seconds max
        va_state.recording.samples = malloc(va_state.recording.capacity * sizeof(float));
        if (!va_state.recording.samples) {
            LOG_WARN("STT: Failed to allocate recording buffer\n");
            ACAP_STATUS_SetString("stt", "status", "Failed to allocate recording buffer");
            return;
        }
    }

    // Reset recording state
    va_state.recording.size = 0;
    va_state.recording.sample_rate = AUDIO_SAMPLE_RATE;
    va_state.recording.recording = TRUE;

    // Update status
    ACAP_STATUS_SetBool("stt", "recording", 1);
    ACAP_STATUS_SetNumber("stt", "samples", 0);
    ACAP_STATUS_SetString("stt", "status", "Recording started");

    // Delay stream start by 100ms to allow main loop to stabilize
    // This prevents crashes when MQTT retained messages trigger recording during initialization
    g_timeout_add(100, delayed_start_recording_callback, NULL);
    LOG("STT: Input stream start scheduled (100ms delay)\n");
}

// Stop recording and send to Wyoming Whisper for transcription
void stop_recording() {
    // Don't stop if we're not recording
    if (!va_state.recording.recording) {
        LOG("STT: Ignoring stop request - not recording\n");
        return;
    }

    LOG("STT: Stopping recording\n");

    va_state.recording.recording = FALSE;
    ACAP_STATUS_SetBool("stt", "recording", 0);

    if (va_state.recording.size == 0) {
        LOG_WARN("STT: No audio recorded (no samples captured)\n");
        ACAP_STATUS_SetString("stt", "status", "No audio captured");
        return;
    }

    LOG("STT: Recorded %u samples (%.2f seconds at %u Hz)\n",
        va_state.recording.size,
        (float)va_state.recording.size / va_state.recording.sample_rate,
        va_state.recording.sample_rate);

    // Convert F32 samples to PCM16 WAV format
    // Wyoming Whisper expects WAV file with PCM16 data

    // Calculate sizes
    uint32_t pcm16_size = va_state.recording.size * 2;  // 2 bytes per sample (16-bit)
    uint32_t wav_size = 44 + pcm16_size;  // WAV header + PCM data

    // Allocate WAV buffer
    unsigned char *wav_data = malloc(wav_size);
    if (!wav_data) {
        LOG_WARN("STT: Failed to allocate WAV buffer\n");
        return;
    }

    // Build WAV header
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

    WavHeader *header = (WavHeader *)wav_data;
    memcpy(header->riff, "RIFF", 4);
    header->file_size = wav_size - 8;
    memcpy(header->wave, "WAVE", 4);
    memcpy(header->fmt, "fmt ", 4);
    header->fmt_size = 16;
    header->audio_format = 1;  // PCM
    header->num_channels = 1;  // Mono
    header->sample_rate = va_state.recording.sample_rate;
    header->byte_rate = va_state.recording.sample_rate * 2;  // 16-bit mono
    header->block_align = 2;
    header->bits_per_sample = 16;
    memcpy(header->data, "data", 4);
    header->data_size = pcm16_size;

    // Convert F32 to PCM16
    int16_t *pcm16_data = (int16_t *)(wav_data + 44);
    for (uint32_t i = 0; i < va_state.recording.size; i++) {
        float sample = va_state.recording.samples[i];
        // Clamp to [-1.0, 1.0]
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        // Convert to 16-bit signed integer
        pcm16_data[i] = (int16_t)(sample * 32767.0f);
    }

    LOG("STT: Created WAV file (%u bytes), sending to Wyoming Whisper\n", wav_size);

    // Update status with recording info
    ACAP_STATUS_SetNumber("stt", "samples", va_state.recording.size);
    ACAP_STATUS_SetNumber("stt", "duration", (float)va_state.recording.size / va_state.recording.sample_rate);
    ACAP_STATUS_SetString("stt", "status", "Connecting to Whisper");

    // Check if Wyoming Whisper is connected
    if (!wyoming_is_connected(WYOMING_SERVICE_WHISPER)) {
        LOG("STT: Whisper not connected, attempting connection...\n");
        if (wyoming_connect(WYOMING_SERVICE_WHISPER) != 0) {
            LOG_WARN("STT: Failed to connect to Whisper service\n");
            ACAP_STATUS_SetString("stt", "status", "Failed to connect to Whisper");
            free(wav_data);
            return;
        }

        // Wait for connection
        for (int i = 0; i < 30; i++) {
            if (wyoming_is_connected(WYOMING_SERVICE_WHISPER)) {
                break;
            }
            usleep(100000);  // 100ms
        }

        if (!wyoming_is_connected(WYOMING_SERVICE_WHISPER)) {
            LOG_WARN("STT: Whisper service connection timeout (no handshake received)\n");
            ACAP_STATUS_SetString("stt", "status", "Whisper connection timeout");
            free(wav_data);
            return;
        }

        LOG("STT: Whisper connected and handshake complete\n");
    }

    // Send to Wyoming Whisper
    ACAP_STATUS_SetString("stt", "status", "Transcribing");
    if (wyoming_asr_transcribe(wav_data, wav_size) != 0) {
        LOG_WARN("STT: Failed to send audio to Whisper\n");
        ACAP_STATUS_SetString("stt", "status", "Failed to send audio to Whisper");
    } else {
        LOG("STT: Audio sent to Whisper, waiting for transcription\n");
    }

    free(wav_data);
}

// =============================================================================
// MQTT INTEGRATION
// =============================================================================

void
MQTT_Connection_Status(int state) {
    char topic[64];
    cJSON* message = NULL;

    switch (state) {
        case MQTT_INITIALIZING:
            LOG("MQTT: Initializing\n");
            break;
        case MQTT_CONNECTING:
            LOG("MQTT: Connecting\n");
            break;
        case MQTT_CONNECTED:
            LOG("MQTT: Connected\n");

            // Subscribe to voice topics with device serial
            char mqtt_sub_topic[128];

            // Subscribe to TTS speak topic
            snprintf(mqtt_sub_topic, sizeof(mqtt_sub_topic), "voice/speak/%s", device_serial);
            MQTT_Subscribe(mqtt_sub_topic);
            LOG("MQTT: Subscribed to topic: %s\n", mqtt_sub_topic);

            // Subscribe to playback topic
            snprintf(mqtt_sub_topic, sizeof(mqtt_sub_topic), "voice/playback/%s", device_serial);
            MQTT_Subscribe(mqtt_sub_topic);
            LOG("MQTT: Subscribed to topic: %s\n", mqtt_sub_topic);

            // Subscribe to STT listen start topic
            snprintf(mqtt_sub_topic, sizeof(mqtt_sub_topic), "voice/listen/start/%s", device_serial);
            MQTT_Subscribe(mqtt_sub_topic);
            LOG("MQTT: Subscribed to topic: %s\n", mqtt_sub_topic);

            // Subscribe to STT listen stop topic
            snprintf(mqtt_sub_topic, sizeof(mqtt_sub_topic), "voice/listen/stop/%s", device_serial);
            MQTT_Subscribe(mqtt_sub_topic);
            LOG("MQTT: Subscribed to topic: %s\n", mqtt_sub_topic);

            // Publish connection status to voice/connect/{SERIAL}
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
            snprintf(topic, sizeof(topic), "voice/connect/%s", device_serial);
            message = cJSON_CreateObject();
            cJSON_AddFalseToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            cJSON_AddStringToObject(message, "service", "voice");
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_RECONNECTED:
            LOG("MQTT: Reconnected\n");

            // Re-subscribe to all voice topics after reconnection
            char mqtt_resub_topic[128];

            snprintf(mqtt_resub_topic, sizeof(mqtt_resub_topic), "voice/speak/%s", device_serial);
            MQTT_Subscribe(mqtt_resub_topic);
            LOG("MQTT: Re-subscribed to topic: %s\n", mqtt_resub_topic);

            snprintf(mqtt_resub_topic, sizeof(mqtt_resub_topic), "voice/playback/%s", device_serial);
            MQTT_Subscribe(mqtt_resub_topic);
            LOG("MQTT: Re-subscribed to topic: %s\n", mqtt_resub_topic);

            snprintf(mqtt_resub_topic, sizeof(mqtt_resub_topic), "voice/listen/start/%s", device_serial);
            MQTT_Subscribe(mqtt_resub_topic);
            LOG("MQTT: Re-subscribed to topic: %s\n", mqtt_resub_topic);

            snprintf(mqtt_resub_topic, sizeof(mqtt_resub_topic), "voice/listen/stop/%s", device_serial);
            MQTT_Subscribe(mqtt_resub_topic);
            LOG("MQTT: Re-subscribed to topic: %s\n", mqtt_resub_topic);
            break;
        case MQTT_DISCONNECTED:
            LOG("MQTT: Disconnected\n");
            break;
    }
}

void
MQTT_Message_Handler(const char *topic, const char *payload) {
    LOG("MQTT Message: %s = %s\n", topic, payload);

    // Build expected topics with device serial
    char listen_start_topic[128], listen_stop_topic[128];
    char speak_topic[128], playback_topic[128];

    snprintf(listen_start_topic, sizeof(listen_start_topic), "voice/listen/start/%s", device_serial);
    snprintf(listen_stop_topic, sizeof(listen_stop_topic), "voice/listen/stop/%s", device_serial);
    snprintf(speak_topic, sizeof(speak_topic), "voice/speak/%s", device_serial);
    snprintf(playback_topic, sizeof(playback_topic), "voice/playback/%s", device_serial);

    // Check if this is the listen start topic
    if (strcmp(topic, listen_start_topic) == 0) {
        LOG("MQTT: Listen start message received - starting STT recording\n");
        start_recording();
        return;
    }

    // Check if this is the listen stop topic
    if (strcmp(topic, listen_stop_topic) == 0) {
        LOG("MQTT: Listen stop message received - stopping STT recording\n");
        stop_recording();
        return;
    }

    // Check if this is the playback topic
    if (strcmp(topic, playback_topic) == 0) {
        cJSON* json = cJSON_Parse(payload);
        if (json) {
            cJSON* url_field = cJSON_GetObjectItem(json, "url");
            if (url_field && cJSON_IsString(url_field)) {
                LOG("MQTT: Playback request - %s\n", url_field->valuestring);
                // TODO: Trigger playback (implement if needed)
            }
            cJSON_Delete(json);
        }
        return;
    }

    // Check if this is the speak topic (TTS)
    if (strcmp(topic, speak_topic) == 0) {
        cJSON* json = cJSON_Parse(payload);
        if (!json) {
            LOG_WARN("MQTT: Invalid JSON payload for speak command\n");
            return;
        }

        cJSON* text = cJSON_GetObjectItem(json, "text");
        if (text && cJSON_IsString(text)) {
            LOG("MQTT: TTS request - %s\n", text->valuestring);

            // Get voice parameter (optional)
            const char* voice = NULL;
            cJSON* voice_item = cJSON_GetObjectItem(json, "voice");
            if (voice_item && cJSON_IsString(voice_item)) {
                voice = voice_item->valuestring;
            }

            // Send to Wyoming TTS
            if (wyoming_tts_synthesize(text->valuestring, voice) == 0) {
                LOG("MQTT: TTS synthesis started\n");
            } else {
                LOG_WARN("MQTT: TTS synthesis failed\n");
            }
        }

        cJSON_Delete(json);
        return;
    }

    LOG_WARN("MQTT: Unhandled topic: %s\n", topic);
}

// =============================================================================
// HTTP ENDPOINTS
// =============================================================================

void
HTTP_Endpoint_playback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

    // Only accept POST
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Only POST method allowed");
        return;
    }

    // Get request body (JSON)
    if (!request->postData || request->postDataLength == 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing JSON body in request");
        return;
    }

    // Parse JSON body
    cJSON* json = cJSON_Parse(request->postData);
    if (!json) {
        ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON in request body");
        return;
    }

    // Get URL from JSON
    cJSON* url_field = cJSON_GetObjectItem(json, "url");
    if (!url_field || !cJSON_IsString(url_field)) {
        cJSON_Delete(json);
        ACAP_HTTP_Respond_Error(response, 400, "Missing 'url' field in JSON");
        return;
    }

    const char* url = url_field->valuestring;

    // Check if already playing
    if (va_state.output.active) {
        LOG_WARN("Already playing (active=%d, stream=%p)\n", va_state.output.active, (void*)va_state.output.stream);
        cJSON_Delete(json);
        ACAP_HTTP_Respond_Error(response, 409, "Already playing");
        return;
    }

    // Check if downloading
    if (va_state.downloading) {
        LOG_WARN("Already downloading\n");
        cJSON_Delete(json);
        ACAP_HTTP_Respond_Error(response, 409, "Already downloading");
        return;
    }

    LOG("Playback request: URL=%s\n", url);

    // Store URL and mark as downloading
    g_free(va_state.download_url);
    va_state.download_url = g_strdup(url);
    va_state.downloading = TRUE;
    cJSON_Delete(json);

    // Download and load WAV file
    if (!download_and_load_wav(va_state.download_url)) {
        va_state.downloading = FALSE;
        ACAP_HTTP_Respond_Error(response, 500, "Failed to download or load WAV file");
        return;
    }

    va_state.downloading = FALSE;

    // Reset playback position
    va_state.playback.read_pos = 0;

    // NOTE: Do NOT set va_state.output.active = TRUE here!
    // The idle callback will set it when it actually starts the stream.
    // Setting it here causes a race condition where subsequent requests
    // fail with "already playing" before the stream even starts.

    // Reset completion flag before starting playback
    g_mutex_lock(&va_state.playback_mutex);
    va_state.playback_complete = FALSE;
    g_mutex_unlock(&va_state.playback_mutex);

    // Schedule playback in main thread
    guint idle_id = g_idle_add(start_playback_idle, NULL);
    LOG("Scheduled playback idle callback, id=%u\n", idle_id);

    // Wait for playback to complete (blocks HTTP handler thread)
    LOG("Waiting for playback to complete...\n");
    g_mutex_lock(&va_state.playback_mutex);
    while (!va_state.playback_complete) {
        g_cond_wait(&va_state.playback_cond, &va_state.playback_mutex);
    }
    g_mutex_unlock(&va_state.playback_mutex);
    LOG("Playback completed, responding to HTTP request\n");

    ACAP_HTTP_Respond_Text(response, "Playback completed successfully");
}


static GMainLoop *main_loop = NULL;

static gboolean
signal_handler(gpointer user_data) {
    LOG("Received SIGTERM, initiating shutdown\n");
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}


// =============================================================================
// MAIN
// =============================================================================

int main(void) {
    openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);
    LOG("------ Starting Voice Assistant ACAP ------\n");

    // Initialize synchronization primitives
    g_mutex_init(&va_state.playback_mutex);
    g_cond_init(&va_state.playback_cond);

    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);

    ACAP(APP_PACKAGE, Settings_Updated_Callback);

    // Get device serial number for MQTT topics
    const char* serial = ACAP_DEVICE_Prop("serial");
    if (serial) {
        strncpy(device_serial, serial, sizeof(device_serial) - 1);
        device_serial[sizeof(device_serial) - 1] = '\0';
        LOG("Device serial: %s\n", device_serial);
    } else {
        LOG_WARN("Failed to get device serial number\n");
        strcpy(device_serial, "UNKNOWN");
    }

    // Register HTTP endpoints
    ACAP_HTTP_Node("playback", HTTP_Endpoint_playback);
    ACAP_HTTP_Node("speak", HTTP_Endpoint_speak);
    ACAP_HTTP_Node("listen_start", HTTP_Endpoint_listen_start);
    ACAP_HTTP_Node("listen_stop", HTTP_Endpoint_listen_stop);
    ACAP_HTTP_Node("transcription", HTTP_Endpoint_transcription);

    // Initialize MQTT
    // Note: Topic subscriptions happen in MQTT_Connection_Status callback when connected
    MQTT_Init(MQTT_Connection_Status, MQTT_Message_Handler);

    // Initialize status groups
    ACAP_STATUS_SetBool("input", "state", 0);
    ACAP_STATUS_SetNumber("input", "samples", 0);
    ACAP_STATUS_SetString("input", "status", "Not running");

    ACAP_STATUS_SetBool("output", "state", 0);
    ACAP_STATUS_SetNumber("output", "samples", 0);
    ACAP_STATUS_SetNumber("output", "size", 0);
    ACAP_STATUS_SetString("output", "status", "Ready");

    ACAP_STATUS_SetString("wyoming", "piper", "not configured");
    ACAP_STATUS_SetString("wyoming", "whisper", "not configured");

    // Initialize STT status group
    ACAP_STATUS_SetBool("stt", "recording", 0);
    ACAP_STATUS_SetNumber("stt", "samples", 0);
    ACAP_STATUS_SetNumber("stt", "duration", 0);
    ACAP_STATUS_SetString("stt", "status", "Ready");
    ACAP_STATUS_SetString("stt", "last_transcript", "");

    LOG("Entering main loop\n");
	main_loop = g_main_loop_new(NULL, FALSE);

    // CRITICAL: Initialize PipeWire audio with the main loop context
    pw_audio_init(g_main_loop_get_context(main_loop));

    // Initialize Wyoming client
    if (wyoming_init(main_loop) == 0) {
        wyoming_set_state_callback(wyoming_state_callback);
        wyoming_set_audio_callback(wyoming_audio_callback);
        wyoming_set_transcript_callback(wyoming_transcript_callback);

        // Configure from settings
        cJSON* settings = ACAP_Get_Config("settings");
        if (settings) {
            const char* wyoming_server = cJSON_GetObjectItem(settings, "wyoming") ?
                                        cJSON_GetObjectItem(settings, "wyoming")->valuestring : NULL;
            int piper_port = cJSON_GetObjectItem(settings, "piper") ?
                            cJSON_GetObjectItem(settings, "piper")->valueint : 0;
            int whisper_port = cJSON_GetObjectItem(settings, "whisper") ?
                              cJSON_GetObjectItem(settings, "whisper")->valueint : 0;
            const char* language = cJSON_GetObjectItem(settings, "language") ?
                                  cJSON_GetObjectItem(settings, "language")->valuestring : "sv";

            if (wyoming_server && piper_port && whisper_port) {
                LOG("Configuring Wyoming: server=%s piper=%d whisper=%d lang=%s\n",
                    wyoming_server, piper_port, whisper_port, language);
                wyoming_configure(wyoming_server, piper_port, whisper_port, language);
                ACAP_STATUS_SetString("wyoming", "piper", "configured");
                ACAP_STATUS_SetString("wyoming", "whisper", "configured");
            } else {
                LOG_WARN("Wyoming settings incomplete in settings.json\n");
            }
        }
    } else {
        LOG_WARN("Failed to initialize Wyoming client\n");
    }

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

    // Disconnect MQTT
    MQTT_Connection_Status(MQTT_DISCONNECTING);
    MQTT_Cleanup();

    // Cleanup Wyoming client
    wyoming_cleanup();

    // Cleanup audio resources
    if (va_state.input.stream) {
        pw_audio_stop(va_state.input.stream);
    }
    if (va_state.output.stream) {
        pw_audio_stop(va_state.output.stream);
    }
    if (va_state.playback.samples) {
        free(va_state.playback.samples);
    }
    if (va_state.download_url) {
        g_free(va_state.download_url);
    }

    // Cleanup synchronization primitives
    g_mutex_clear(&va_state.playback_mutex);
    g_cond_clear(&va_state.playback_cond);

    curl_global_cleanup();
    ACAP_Cleanup();
    closelog();
    return 0;
}

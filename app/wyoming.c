/*
 * Wyoming Protocol TCP Client
 * Copyright (c) 2025 Fred Juhlin
 * BSD 3-Clause License
 *
 * Note: Uses TCP sockets instead of WebSockets since libwebsockets
 * is not available in ACAP SDK. Wyoming protocol works over raw TCP.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include "wyoming.h"
#include "cJSON.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define WYOMING_BUFFER_SIZE 65536

// Parsing state for interleaved JSON/binary protocol
typedef enum {
    WYOMING_PARSE_JSON,      // Expecting JSON messages
    WYOMING_PARSE_BINARY     // Expecting binary audio data
} WyomingParseState;

// Wyoming service connection
typedef struct {
    WyomingServiceType type;
    WyomingConnectionState state;

    // Connection details
    char server_ip[64];
    int port;
    char language[8];

    // TCP socket
    int socket_fd;

    // Receive buffer
    char rx_buffer[WYOMING_BUFFER_SIZE];
    size_t rx_length;

    // Parsing state for binary audio chunks
    WyomingParseState parse_state;
    size_t binary_bytes_expected;
    size_t binary_bytes_received;

    // Audio metadata from last audio-chunk message
    int audio_rate;
    int audio_width;
    int audio_channels;

    // Audio buffer for TTS (accumulated PCM data)
    unsigned char *audio_buffer;
    size_t audio_buffer_size;
    size_t audio_buffer_capacity;

    // Request tracking
    gboolean awaiting_response;
    char pending_request[256];

    // Protocol handshake
    gboolean tcp_connected;         // TCP socket connection complete
    gboolean handshake_received;    // Wyoming protocol handshake received

    // Error tracking
    char error_msg[256];

    // Protocol info
    char info_string[512];

} WyomingConnection;

// Global state
static struct {
    GMainLoop *main_loop;
    GSource *poll_source;
    guint poll_id;

    WyomingConnection piper;
    WyomingConnection whisper;

    // Callbacks
    WyomingStateCallback state_callback;
    WyomingAudioCallback audio_callback;
    WyomingTranscriptCallback transcript_callback;

    bool initialized;
} wyoming_state = {0};

/*-----------------------------------------------------
 * Helper Functions
 *-----------------------------------------------------*/

static WyomingConnection* wyoming_get_connection(WyomingServiceType service) {
    return (service == WYOMING_SERVICE_PIPER) ? &wyoming_state.piper : &wyoming_state.whisper;
}

static void wyoming_set_state(WyomingConnection *conn, WyomingConnectionState new_state, const char* error) {
    conn->state = new_state;

    if (error) {
        snprintf(conn->error_msg, sizeof(conn->error_msg), "%s", error);
    } else {
        conn->error_msg[0] = '\0';
    }

    // Update info string
    const char* service_name = (conn->type == WYOMING_SERVICE_PIPER) ? "Piper" : "Whisper";
    const char* state_names[] = {"Disconnected", "Connecting", "Connected", "Error"};
    snprintf(conn->info_string, sizeof(conn->info_string),
             "%s: %s:%d (%s)%s%s",
             service_name, conn->server_ip, conn->port, state_names[new_state],
             error ? " - " : "", error ? error : "");

    LOG_TRACE("%s\n", conn->info_string);

    // Call user callback
    if (wyoming_state.state_callback) {
        wyoming_state.state_callback(conn->type, new_state, error);
    }
}

static int wyoming_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*-----------------------------------------------------
 * Socket I/O
 *-----------------------------------------------------*/

static void wyoming_process_message(WyomingConnection *conn, const char *message, size_t length) {
    (void)length;

    // Only log first 200 chars to avoid spam
    char log_msg[201];
    size_t log_len = length < 200 ? length : 200;
    memcpy(log_msg, message, log_len);
    log_msg[log_len] = '\0';

    LOG_TRACE("Wyoming %s received: %s%s\n",
              conn->type == WYOMING_SERVICE_PIPER ? "Piper" : "Whisper",
              log_msg,
              length > 200 ? "..." : "");

    // Parse JSON message
    cJSON *json = cJSON_Parse(message);
    if (!json) {
        LOG_WARN("Wyoming: Failed to parse JSON message: %s\n", message);
        return;
    }

    // Check message type
    cJSON *type = cJSON_GetObjectItem(json, "type");

    // Log received message type for debugging
    if (type && cJSON_IsString(type)) {
        LOG_TRACE("Wyoming %s: Received message type '%s'\n",
            conn->type == WYOMING_SERVICE_PIPER ? "TTS" : "ASR",
            type->valuestring);
    }

    // Check if this is audio format metadata (no "type" field)
    if (!type) {
        // This might be the audio format metadata: {"rate":...,"width":...,"channels":...}
        cJSON *rate = cJSON_GetObjectItem(json, "rate");
        cJSON *width = cJSON_GetObjectItem(json, "width");
        cJSON *channels = cJSON_GetObjectItem(json, "channels");

        if (rate && width && channels) {
            conn->audio_rate = rate->valueint;
            conn->audio_width = width->valueint;
            conn->audio_channels = channels->valueint;
            LOG_TRACE("Wyoming: Audio format: %dHz, %d-bit, %d channels\n",
                     conn->audio_rate, conn->audio_width, conn->audio_channels);

            // Now transition to binary mode if we're expecting PCM data
            if (conn->binary_bytes_expected > 0) {
                conn->parse_state = WYOMING_PARSE_BINARY;
                LOG_TRACE("Wyoming: Switching to binary mode for %zu bytes\n", conn->binary_bytes_expected);
            }
        }

        cJSON_Delete(json);
        return;
    }

    if (!cJSON_IsString(type)) {
        cJSON_Delete(json);
        return;
    }

    // Handle different message types
    if (strcmp(type->valuestring, "info") == 0) {
        LOG_TRACE("Wyoming %s: Received server info handshake - connection ready\n",
            conn->type == WYOMING_SERVICE_PIPER ? "Piper" : "Whisper");
        // Mark handshake complete - connection is now ready for requests
        conn->handshake_received = TRUE;
        wyoming_set_state(conn, WYOMING_STATE_CONNECTED, NULL);
    }
    else if (strcmp(type->valuestring, "audio-start") == 0) {
        LOG_TRACE("Wyoming: TTS audio-start\n");
        // Reset audio buffer
        conn->audio_buffer_size = 0;
    }
    else if (strcmp(type->valuestring, "audio-chunk") == 0) {
        // Extract audio format metadata (rate, width, channels)
        // These fields are present in the audio-chunk message itself
        cJSON *rate = cJSON_GetObjectItem(json, "rate");
        cJSON *width = cJSON_GetObjectItem(json, "width");
        cJSON *channels = cJSON_GetObjectItem(json, "channels");

        if (rate && cJSON_IsNumber(rate)) {
            conn->audio_rate = rate->valueint;
        }
        if (width && cJSON_IsNumber(width)) {
            conn->audio_width = width->valueint;
        }
        if (channels && cJSON_IsNumber(channels)) {
            conn->audio_channels = channels->valueint;
        }

        LOG_TRACE("Wyoming: audio-chunk format: %d Hz, %d-bit, %d channels\n",
            conn->audio_rate, conn->audio_width * 8, conn->audio_channels);

        // Extract payload_length - number of PCM bytes that will follow
        cJSON *payload_length = cJSON_GetObjectItem(json, "payload_length");
        if (payload_length && cJSON_IsNumber(payload_length)) {
            conn->binary_bytes_expected = payload_length->valueint;
            conn->binary_bytes_received = 0;
            LOG_TRACE("Wyoming: audio-chunk expects %zu bytes of PCM data\n", conn->binary_bytes_expected);
        } else {
            LOG_WARN("Wyoming: audio-chunk missing payload_length field\n");
        }
    }
    else if (strcmp(type->valuestring, "audio-stop") == 0) {
        LOG_TRACE("Wyoming: TTS audio-stop (total PCM: %zu bytes)\n", conn->audio_buffer_size);

        if (wyoming_state.audio_callback && conn->audio_buffer_size > 0) {
            // Build WAV file from accumulated PCM data
            // WAV header structure (44 bytes)
            typedef struct __attribute__((packed)) {
                char riff[4];           // "RIFF"
                uint32_t file_size;     // File size - 8
                char wave[4];           // "WAVE"
                char fmt[4];            // "fmt "
                uint32_t fmt_size;      // 16 for PCM
                uint16_t audio_format;  // 1 for PCM
                uint16_t num_channels;
                uint32_t sample_rate;
                uint32_t byte_rate;     // sample_rate * num_channels * bits_per_sample/8
                uint16_t block_align;   // num_channels * bits_per_sample/8
                uint16_t bits_per_sample;
                char data[4];           // "data"
                uint32_t data_size;     // PCM data size
            } WavHeader;

            // Wyoming audio_width is in BYTES (e.g., 2 = 16 bits)
            uint16_t bits_per_sample = conn->audio_width * 8;

            WavHeader wav_header = {
                .riff = {'R', 'I', 'F', 'F'},
                .file_size = conn->audio_buffer_size + sizeof(WavHeader) - 8,
                .wave = {'W', 'A', 'V', 'E'},
                .fmt = {'f', 'm', 't', ' '},
                .fmt_size = 16,
                .audio_format = 1,  // PCM
                .num_channels = conn->audio_channels,
                .sample_rate = conn->audio_rate,
                .byte_rate = conn->audio_rate * conn->audio_channels * conn->audio_width,
                .block_align = conn->audio_channels * conn->audio_width,
                .bits_per_sample = bits_per_sample,
                .data = {'d', 'a', 't', 'a'},
                .data_size = conn->audio_buffer_size
            };

            // Allocate complete WAV file buffer (header + PCM data)
            size_t wav_file_size = sizeof(WavHeader) + conn->audio_buffer_size;
            unsigned char *wav_file = malloc(wav_file_size);
            if (!wav_file) {
                LOG_WARN("Wyoming: Failed to allocate WAV file buffer\n");
                conn->awaiting_response = FALSE;
                cJSON_Delete(json);
                return;
            }

            // Copy header and PCM data
            memcpy(wav_file, &wav_header, sizeof(WavHeader));
            memcpy(wav_file + sizeof(WavHeader), conn->audio_buffer, conn->audio_buffer_size);

            LOG_TRACE("Wyoming: Built WAV file: %dHz, %d-bit, %d ch, %zu bytes total\n",
                conn->audio_rate, bits_per_sample, conn->audio_channels, wav_file_size);

            // Send to callback
            wyoming_state.audio_callback(conn->type, wav_file, wav_file_size);

            // Free WAV file buffer
            free(wav_file);
        }

        conn->awaiting_response = FALSE;

        // BUGFIX: Clear receive buffer and reset parser state after TTS completes
        // This ensures the next TTS request starts with a clean slate
        conn->rx_length = 0;
        conn->parse_state = WYOMING_PARSE_JSON;
        conn->binary_bytes_expected = 0;
        conn->binary_bytes_received = 0;
        LOG_TRACE("Wyoming TTS: Audio complete, buffer and parser state reset\n");
    }
    else if (strcmp(type->valuestring, "transcript") == 0) {
        LOG_TRACE("Wyoming ASR: Received transcript message\n");
        cJSON *text = cJSON_GetObjectItem(json, "text");
        if (text && cJSON_IsString(text)) {
            LOG("Wyoming ASR: Transcript text: '%s'\n", text->valuestring);
            if (wyoming_state.transcript_callback) {
                LOG_TRACE("Wyoming ASR: Calling transcript callback\n");
                wyoming_state.transcript_callback(text->valuestring);
            } else {
                LOG_WARN("Wyoming ASR: No transcript callback registered!\n");
            }
        } else {
            LOG_WARN("Wyoming ASR: Transcript message has no 'text' field\n");
        }
        conn->awaiting_response = FALSE;

        // BUGFIX: Clear receive buffer and reset parser state after transcription completes
        // This ensures the next ASR request starts with a clean slate
        conn->rx_length = 0;
        conn->parse_state = WYOMING_PARSE_JSON;
        conn->binary_bytes_expected = 0;
        conn->binary_bytes_received = 0;
        LOG_TRACE("Wyoming ASR: Transcription complete, buffer and parser state reset\n");
    }
    else if (strcmp(type->valuestring, "error") == 0) {
        cJSON *text = cJSON_GetObjectItem(json, "text");
        const char *error_text = text && cJSON_IsString(text) ? text->valuestring : "Unknown error";
        LOG_WARN("Wyoming error: %s\n", error_text);
        wyoming_set_state(conn, WYOMING_STATE_ERROR, error_text);
        conn->awaiting_response = FALSE;
    }

    cJSON_Delete(json);
}

static gboolean wyoming_poll_callback(gpointer user_data) {
    (void)user_data;

    struct pollfd fds[2];
    int nfds = 0;

    // Setup poll for both connections
    if (wyoming_state.piper.socket_fd >= 0) {
        fds[nfds].fd = wyoming_state.piper.socket_fd;
        fds[nfds].events = POLLIN | POLLERR | POLLHUP;
        nfds++;
    }

    if (wyoming_state.whisper.socket_fd >= 0) {
        fds[nfds].fd = wyoming_state.whisper.socket_fd;
        fds[nfds].events = POLLIN | POLLERR | POLLHUP;
        nfds++;
    }

    if (nfds == 0) {
        return G_SOURCE_CONTINUE;
    }

    // Poll with 0 timeout (non-blocking)
    int ret = poll(fds, nfds, 0);
    if (ret < 0) {
        LOG_WARN("Wyoming poll error: %s\n", strerror(errno));
        return G_SOURCE_CONTINUE;
    }

    if (ret == 0) {
        // No events
        return G_SOURCE_CONTINUE;
    }

    LOG_TRACE("Wyoming poll: %d fd(s) have events\n", ret);

    // Process events
    for (int i = 0; i < nfds; i++) {
        if (fds[i].revents == 0) {
            continue;
        }

        // Find which connection this is
        WyomingConnection *conn = NULL;
        if (fds[i].fd == wyoming_state.piper.socket_fd) {
            conn = &wyoming_state.piper;
        } else if (fds[i].fd == wyoming_state.whisper.socket_fd) {
            conn = &wyoming_state.whisper;
        }

        if (!conn) {
            continue;
        }

        // Check for errors or hangup
        if (fds[i].revents & (POLLERR | POLLHUP)) {
            LOG_WARN("Wyoming socket error or hangup\n");
            wyoming_set_state(conn, WYOMING_STATE_ERROR, "Connection lost");
            close(conn->socket_fd);
            conn->socket_fd = -1;
            continue;
        }

        // Read data
        if (fds[i].revents & POLLIN) {
            LOG_TRACE("Wyoming %s: POLLIN event, attempting to read...\n",
                conn->type == WYOMING_SERVICE_PIPER ? "TTS" : "ASR");
            char buffer[4096];
            ssize_t n = recv(conn->socket_fd, buffer, sizeof(buffer) - 1, 0);

            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_WARN("Wyoming recv error: %s\n", strerror(errno));
                    wyoming_set_state(conn, WYOMING_STATE_ERROR, strerror(errno));
                    close(conn->socket_fd);
                    conn->socket_fd = -1;
                }
                continue;
            }

            if (n == 0) {
                LOG("Wyoming connection closed by server\n");
                wyoming_set_state(conn, WYOMING_STATE_DISCONNECTED, NULL);
                close(conn->socket_fd);
                conn->socket_fd = -1;
                continue;
            }

            LOG_TRACE("Wyoming %s: Received %zd bytes from socket\n",
                conn->type == WYOMING_SERVICE_PIPER ? "TTS" : "ASR", n);

            // Append to receive buffer
            if (conn->rx_length + n < WYOMING_BUFFER_SIZE) {
                memcpy(conn->rx_buffer + conn->rx_length, buffer, n);
                conn->rx_length += n;

                // Process data based on current parsing state
                while (conn->rx_length > 0) {
                    if (conn->parse_state == WYOMING_PARSE_BINARY) {
                        // Binary PCM audio data mode
                        size_t bytes_to_copy = conn->binary_bytes_expected - conn->binary_bytes_received;
                        if (bytes_to_copy > conn->rx_length) {
                            bytes_to_copy = conn->rx_length;
                        }

                        // Ensure audio buffer has enough capacity
                        size_t required_capacity = conn->audio_buffer_size + bytes_to_copy;
                        if (required_capacity > conn->audio_buffer_capacity) {
                            size_t new_capacity = conn->audio_buffer_capacity * 2;
                            if (new_capacity < required_capacity) {
                                new_capacity = required_capacity + 65536;
                            }
                            unsigned char *new_buffer = realloc(conn->audio_buffer, new_capacity);
                            if (!new_buffer) {
                                LOG_WARN("Wyoming: Failed to resize audio buffer\n");
                                conn->parse_state = WYOMING_PARSE_JSON;
                                conn->rx_length = 0;
                                break;
                            }
                            conn->audio_buffer = new_buffer;
                            conn->audio_buffer_capacity = new_capacity;
                        }

                        // Copy PCM data to audio buffer
                        memcpy(conn->audio_buffer + conn->audio_buffer_size, conn->rx_buffer, bytes_to_copy);
                        conn->audio_buffer_size += bytes_to_copy;
                        conn->binary_bytes_received += bytes_to_copy;

                        // Remove copied bytes from rx_buffer
                        memmove(conn->rx_buffer, conn->rx_buffer + bytes_to_copy, conn->rx_length - bytes_to_copy);
                        conn->rx_length -= bytes_to_copy;

                        // Check if we've received all expected bytes
                        if (conn->binary_bytes_received >= conn->binary_bytes_expected) {
                            LOG_TRACE("Wyoming: Received complete PCM chunk (%zu bytes, total: %zu)\n",
                                     conn->binary_bytes_expected, conn->audio_buffer_size);
                            conn->parse_state = WYOMING_PARSE_JSON;
                            conn->binary_bytes_expected = 0;
                            conn->binary_bytes_received = 0;
                        }
                    } else {
                        // JSON mode - process messages
                        // Note: Wyoming can send multiple JSON objects on the same line
                        // We need to parse them individually using a JSON parser

                        conn->rx_buffer[conn->rx_length] = '\0';
                        char *parse_ptr = conn->rx_buffer;
                        size_t remaining = conn->rx_length;
                        bool parsed_something = false;

                        while (remaining > 0) {
                            // Skip whitespace
                            while (remaining > 0 && (*parse_ptr == ' ' || *parse_ptr == '\t' || *parse_ptr == '\r')) {
                                parse_ptr++;
                                remaining--;
                            }

                            if (remaining == 0) {
//                                LOG_TRACE("Wyoming parser: no more data (remaining=0)\n");
                                break;
                            }

                            // Check for newline (end of messages)
                            if (*parse_ptr == '\n') {
                                parse_ptr++;
                                remaining--;
                                continue;
                            }

                            // Try to parse a JSON object
                            if (*parse_ptr == '{') {
//                                LOG_TRACE("Wyoming parser: found '{', remaining=%zu\n", remaining);
                                // Find the end of this JSON object
                                int brace_count = 0;
                                char *json_start = parse_ptr;
                                char *json_end = parse_ptr;

                                while (json_end < conn->rx_buffer + conn->rx_length) {
                                    if (*json_end == '{') brace_count++;
                                    else if (*json_end == '}') {
                                        brace_count--;
                                        if (brace_count == 0) {
                                            json_end++;
                                            break;
                                        }
                                    }
                                    json_end++;
                                }

                                if (brace_count == 0) {
                                    // We have a complete JSON object
                                    size_t json_len = json_end - json_start;
//                                    LOG_TRACE("Wyoming parser: complete JSON object, len=%zu\n", json_len);

                                    // Check if this JSON has data_length or payload_length (Wyoming event protocol)
                                    // Format: {"type":"...","version":"...","data_length":N,"payload_length":M}
                                    //         [N bytes of JSON data if data_length present]
                                    //         [M bytes of binary payload if payload_length present]
                                    char saved_char = *json_end;
                                    *json_end = '\0';
                                    cJSON *header_json = cJSON_Parse(json_start);
                                    *json_end = saved_char;

                                    char *merged_json = NULL;
                                    size_t merged_len = json_len;
                                    bool did_merge = false;

                                    if (header_json) {
                                        cJSON *data_length_field = cJSON_GetObjectItem(header_json, "data_length");
                                        cJSON *payload_length_field = cJSON_GetObjectItem(header_json, "payload_length");

                                        // Handle data_length: read additional JSON bytes and merge
                                        if (data_length_field && cJSON_IsNumber(data_length_field)) {
                                            int data_length = data_length_field->valueint;
//                                            LOG_TRACE("Wyoming: Header has data_length=%d\n", data_length);

                                            // Skip whitespace after header JSON to find data JSON start
                                            char *data_ptr = json_end;
                                            while (data_ptr < conn->rx_buffer + conn->rx_length &&
                                                   (*data_ptr == ' ' || *data_ptr == '\t' || *data_ptr == '\r' || *data_ptr == '\n')) {
                                                data_ptr++;
                                            }
                                            size_t data_start = data_ptr - conn->rx_buffer;

//                                            LOG("Wyoming: data_start=%zu (after skipping whitespace), rx_length=%zu, need=%d\n", data_start, conn->rx_length, data_length);

                                            // Check if we have enough data in buffer
                                            if (data_start + data_length <= conn->rx_length) {
                                                // Parse data JSON
                                                char saved_data_char = conn->rx_buffer[data_start + data_length];
                                                conn->rx_buffer[data_start + data_length] = '\0';
                                                cJSON *data_json = cJSON_Parse(conn->rx_buffer + data_start);
                                                conn->rx_buffer[data_start + data_length] = saved_data_char;

                                                if (data_json) {
                                                    // Merge data JSON into header JSON by duplicating each field
                                                    cJSON *item = data_json->child;
                                                    while (item) {
                                                        if (item->string) {
                                                            // Duplicate and add each field from data_json to header_json
                                                            cJSON *dup = cJSON_Duplicate(item, 1);  // Deep copy
                                                            if (dup) {
                                                                cJSON_AddItemToObject(header_json, item->string, dup);
//                                                                LOG_TRACE("Wyoming: Merged field '%s' from data JSON\n", item->string);
                                                            }
                                                        }
                                                        item = item->next;
                                                    }
                                                    cJSON_Delete(data_json);

                                                    // Update json_end to skip consumed data
                                                    json_end = conn->rx_buffer + data_start + data_length;
//                                                    LOG_TRACE("Wyoming: Merged data, new json_end offset=%zu\n", data_start + data_length);
                                                    did_merge = true;  // Mark that we successfully merged
                                                }
                                            } else {
                                                LOG_TRACE("Wyoming: Not enough data yet for data_length=%d\n", data_length);
                                                cJSON_Delete(header_json);
                                                break;  // Wait for more data
                                            }
                                        }

                                        // Handle payload_length: set up binary parsing mode
                                        if (payload_length_field && cJSON_IsNumber(payload_length_field)) {
                                            conn->binary_bytes_expected = payload_length_field->valueint;
                                            conn->binary_bytes_received = 0;
                                            conn->parse_state = WYOMING_PARSE_BINARY;
                                            LOG_TRACE("Wyoming: Expecting %zu bytes of binary payload\n", conn->binary_bytes_expected);
                                        }

                                        // Only create merged JSON if we actually merged data
                                        if (did_merge) {
                                            merged_json = cJSON_PrintUnformatted(header_json);
                                            if (merged_json) {
                                                merged_len = strlen(merged_json);
//                                                LOG("Wyoming: Merged JSON ready: %s\n", merged_json);
                                            }
                                        }
                                        cJSON_Delete(header_json);
                                    }

                                    // Call wyoming_process_message with merged JSON if available
                                    if (merged_json) {
                                        LOG_TRACE("Wyoming: Processing merged JSON (len=%zu)\n", merged_len);
                                        wyoming_process_message(conn, merged_json, merged_len);
                                        free(merged_json);
                                    } else {
                                        LOG_TRACE("Wyoming: Processing original JSON (no merge needed)\n");
                                        saved_char = *json_end;
                                        *json_end = '\0';
                                        wyoming_process_message(conn, json_start, json_len);
                                        *json_end = saved_char;
                                    }

                                    // BUGFIX: Check if buffer was cleared by message processing
                                    // (happens after transcript or audio-stop messages)
                                    if (conn->rx_length == 0) {
                                        LOG_TRACE("Wyoming: Buffer cleared by message handler, stopping parse loop\n");
                                        break;  // Exit loop to avoid overwriting rx_length with 'remaining'
                                    }

                                    // Calculate bytes consumed BEFORE updating parse_ptr
                                    size_t consumed = json_end - parse_ptr;
//                                    LOG_TRACE("Wyoming parser: consumed=%zu, remaining before=%zu\n", consumed, remaining);
                                    remaining -= consumed;
                                    parse_ptr = json_end;
//                                    LOG_TRACE("Wyoming parser: remaining after=%zu\n", remaining);
                                    parsed_something = true;

                                    // Check if we should switch to binary mode
                                    if (conn->parse_state == WYOMING_PARSE_BINARY) {
//                                        LOG_TRACE("Wyoming parser: switching to binary mode, breaking from JSON loop\n");
                                        break;  // Exit JSON parsing loop, next iteration will handle binary data
                                    }
                                } else {
                                    // Incomplete JSON, wait for more data
                                    break;
                                }
                            } else {
                                // Unknown data, skip to next newline or end
                                LOG_WARN("Wyoming: Unexpected data (not JSON): %c\n", *parse_ptr);
                                while (remaining > 0 && *parse_ptr != '\n') {
                                    parse_ptr++;
                                    remaining--;
                                }
                            }
                        }

                        // Move unparsed data to start of buffer
                        // BUGFIX: Only update rx_length if it wasn't cleared by message handler
                        if (conn->rx_length > 0 && parse_ptr > conn->rx_buffer) {
                            size_t consumed = parse_ptr - conn->rx_buffer;
                            if (remaining > 0) {
                                memmove(conn->rx_buffer, parse_ptr, remaining);
                            }
                            conn->rx_length = remaining;
                        }

                        if (!parsed_something && conn->rx_length > 0) {
                            // No progress made, break to avoid infinite loop
                            break;
                        }
                    }
                }
            } else {
                LOG_WARN("Wyoming receive buffer full, dropping data\n");
                conn->rx_length = 0;
                conn->parse_state = WYOMING_PARSE_JSON;
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

/*-----------------------------------------------------
 * Public API Implementation
 *-----------------------------------------------------*/

int wyoming_init(GMainLoop* main_loop) {
    if (wyoming_state.initialized) {
        LOG_WARN("Wyoming already initialized\n");
        return -1;
    }

    if (!main_loop) {
        LOG_WARN("Wyoming init: main_loop is NULL\n");
        return -1;
    }

    memset(&wyoming_state, 0, sizeof(wyoming_state));
    wyoming_state.main_loop = main_loop;

    // Initialize connections
    wyoming_state.piper.type = WYOMING_SERVICE_PIPER;
    wyoming_state.piper.state = WYOMING_STATE_DISCONNECTED;
    wyoming_state.piper.socket_fd = -1;
    wyoming_state.piper.parse_state = WYOMING_PARSE_JSON;

    wyoming_state.whisper.type = WYOMING_SERVICE_WHISPER;
    wyoming_state.whisper.state = WYOMING_STATE_DISCONNECTED;
    wyoming_state.whisper.socket_fd = -1;
    wyoming_state.whisper.parse_state = WYOMING_PARSE_JSON;

    // Add GLib timeout for socket polling (10ms)
    wyoming_state.poll_id = g_timeout_add(10, wyoming_poll_callback, NULL);

    wyoming_state.initialized = true;

    LOG("Wyoming client initialized\n");
    return 0;
}

void wyoming_cleanup(void) {
    if (!wyoming_state.initialized) {
        return;
    }

    // Disconnect both services
    wyoming_disconnect(WYOMING_SERVICE_PIPER);
    wyoming_disconnect(WYOMING_SERVICE_WHISPER);

    // Remove GLib timeout
    if (wyoming_state.poll_id > 0) {
        g_source_remove(wyoming_state.poll_id);
        wyoming_state.poll_id = 0;
    }

    wyoming_state.initialized = false;
    LOG("Wyoming client cleaned up\n");
}

int wyoming_configure(const char* server_ip, int piper_port, int whisper_port, const char* language) {
    if (!wyoming_state.initialized) {
        LOG_WARN("Wyoming not initialized\n");
        return -1;
    }

    if (!server_ip || !language) {
        LOG_WARN("Wyoming configure: invalid parameters\n");
        return -1;
    }

    // Configure Piper
    snprintf(wyoming_state.piper.server_ip, sizeof(wyoming_state.piper.server_ip), "%s", server_ip);
    wyoming_state.piper.port = piper_port;
    snprintf(wyoming_state.piper.language, sizeof(wyoming_state.piper.language), "%s", language);

    // Configure Whisper
    snprintf(wyoming_state.whisper.server_ip, sizeof(wyoming_state.whisper.server_ip), "%s", server_ip);
    wyoming_state.whisper.port = whisper_port;
    snprintf(wyoming_state.whisper.language, sizeof(wyoming_state.whisper.language), "%s", language);

    LOG("Wyoming configured: %s (Piper:%d, Whisper:%d) Language:%s\n",
        server_ip, piper_port, whisper_port, language);

    return 0;
}

void wyoming_set_state_callback(WyomingStateCallback callback) {
    wyoming_state.state_callback = callback;
}

void wyoming_set_audio_callback(WyomingAudioCallback callback) {
    wyoming_state.audio_callback = callback;
}

void wyoming_set_transcript_callback(WyomingTranscriptCallback callback) {
    wyoming_state.transcript_callback = callback;
}

int wyoming_connect(WyomingServiceType service) {
    WyomingConnection *conn = wyoming_get_connection(service);

    if (!wyoming_state.initialized) {
        LOG_WARN("Wyoming not initialized\n");
        return -1;
    }

    if (conn->state == WYOMING_STATE_CONNECTED || conn->state == WYOMING_STATE_CONNECTING) {
        LOG_WARN("Wyoming %s: already connected/connecting\n",
                 service == WYOMING_SERVICE_PIPER ? "Piper" : "Whisper");
        return -1;
    }

    if (strlen(conn->server_ip) == 0 || conn->port == 0) {
        wyoming_set_state(conn, WYOMING_STATE_ERROR, "Not configured");
        return -1;
    }

    // Create TCP socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        wyoming_set_state(conn, WYOMING_STATE_ERROR, strerror(errno));
        return -1;
    }

    // Set non-blocking
    if (wyoming_set_nonblocking(sockfd) < 0) {
        close(sockfd);
        wyoming_set_state(conn, WYOMING_STATE_ERROR, "Failed to set non-blocking");
        return -1;
    }

    // Setup server address
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(conn->port);

    if (inet_pton(AF_INET, conn->server_ip, &server_addr.sin_addr) <= 0) {
        close(sockfd);
        wyoming_set_state(conn, WYOMING_STATE_ERROR, "Invalid IP address");
        return -1;
    }

    wyoming_set_state(conn, WYOMING_STATE_CONNECTING, NULL);

    // Connect (non-blocking)
    int ret = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(sockfd);
        wyoming_set_state(conn, WYOMING_STATE_ERROR, strerror(errno));
        return -1;
    }

    conn->socket_fd = sockfd;
    conn->rx_length = 0;
    conn->parse_state = WYOMING_PARSE_JSON;
    conn->binary_bytes_expected = 0;
    conn->binary_bytes_received = 0;
    conn->awaiting_response = FALSE;
    conn->tcp_connected = TRUE;
    conn->handshake_received = FALSE;

    // Assume connection succeeds (works for local connections)
    // The "info" handshake will be received in the poll callback
    wyoming_set_state(conn, WYOMING_STATE_CONNECTED, NULL);

    LOG("Wyoming %s: connecting to %s:%d (fd=%d)\n",
        conn->type == WYOMING_SERVICE_PIPER ? "Piper" : "Whisper",
        conn->server_ip, conn->port, sockfd);

    return 0;
}

void wyoming_disconnect(WyomingServiceType service) {
    WyomingConnection *conn = wyoming_get_connection(service);

    if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
        conn->socket_fd = -1;
    }

    // Free audio buffer
    if (conn->audio_buffer) {
        free(conn->audio_buffer);
        conn->audio_buffer = NULL;
        conn->audio_buffer_size = 0;
        conn->audio_buffer_capacity = 0;
    }

    conn->rx_length = 0;
    conn->awaiting_response = FALSE;
    wyoming_set_state(conn, WYOMING_STATE_DISCONNECTED, NULL);
}

bool wyoming_is_connected(WyomingServiceType service) {
    WyomingConnection *conn = wyoming_get_connection(service);
    return conn->state == WYOMING_STATE_CONNECTED;
}

WyomingConnectionState wyoming_get_state(WyomingServiceType service) {
    WyomingConnection *conn = wyoming_get_connection(service);
    return conn->state;
}

const char* wyoming_get_error(WyomingServiceType service) {
    WyomingConnection *conn = wyoming_get_connection(service);
    return conn->error_msg;
}

const char* wyoming_get_connection_info(WyomingServiceType service) {
    WyomingConnection *conn = wyoming_get_connection(service);
    return conn->info_string;
}

/*-----------------------------------------------------
 * TTS Operations (Piper)
 *-----------------------------------------------------*/

int wyoming_tts_synthesize(const char* text, const char* voice) {
    WyomingConnection *conn = &wyoming_state.piper;

    if (!wyoming_is_connected(WYOMING_SERVICE_PIPER)) {
        LOG_WARN("Wyoming TTS: not connected to Piper\n");
        return -1;
    }

    if (!text || strlen(text) == 0) {
        LOG_WARN("Wyoming TTS: empty text\n");
        return -1;
    }

    if (conn->awaiting_response) {
        LOG_WARN("Wyoming TTS: previous request still in progress, canceling it\n");
        conn->awaiting_response = FALSE;
    }

    // Allocate audio buffer if needed
    if (!conn->audio_buffer) {
        conn->audio_buffer_capacity = 1024 * 1024;  // 1MB initial
        conn->audio_buffer = malloc(conn->audio_buffer_capacity);
        if (!conn->audio_buffer) {
            LOG_WARN("Wyoming TTS: failed to allocate audio buffer\n");
            return -1;
        }
    }
    conn->audio_buffer_size = 0;

    // Build Wyoming TTS request
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "synthesize");

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "text", text);
    if (voice && strlen(voice) > 0) {
        cJSON_AddStringToObject(data, "voice", voice);
    }
    cJSON_AddItemToObject(request, "data", data);

    char *json_str = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    if (!json_str) {
        LOG_WARN("Wyoming TTS: failed to create JSON request\n");
        return -1;
    }

    // Send request (JSON + newline)
    size_t json_len = strlen(json_str);
    char *message = malloc(json_len + 2);
    strcpy(message, json_str);
    message[json_len] = '\n';
    message[json_len + 1] = '\0';

    LOG("Wyoming TTS: Sending synthesize request: %s\n", json_str);
    free(json_str);

    // Wait for socket to be writable (non-blocking socket may not be ready immediately)
    struct pollfd pfd = {
        .fd = conn->socket_fd,
        .events = POLLOUT,
        .revents = 0
    };

    int poll_ret = poll(&pfd, 1, 3000);  // 3 second timeout
    if (poll_ret <= 0) {
        free(message);
        LOG_WARN("Wyoming TTS: socket not ready for writing (timeout or error)\n");
        return -1;
    }

    ssize_t sent = send(conn->socket_fd, message, json_len + 1, 0);
    free(message);

    if (sent < 0) {
        LOG_WARN("Wyoming TTS: send failed: %s\n", strerror(errno));
        return -1;
    }

    conn->awaiting_response = TRUE;
    snprintf(conn->pending_request, sizeof(conn->pending_request), "TTS: %s", text);

    return 0;
}

/*-----------------------------------------------------
 * Wyoming Event Protocol Helpers
 *-----------------------------------------------------*/

// Send a Wyoming protocol event with optional data and payload
// Format: JSON header\n [data JSON if present] [binary payload if present]
static int wyoming_send_event(WyomingConnection *conn, const char* event_type,
                               const char* data_json, const unsigned char* payload, size_t payload_length) {
    // Build JSON header
    cJSON *header = cJSON_CreateObject();
    cJSON_AddStringToObject(header, "type", event_type);
    cJSON_AddStringToObject(header, "version", "1.0.0");

    // Add data_length if we have data JSON
    if (data_json) {
        cJSON_AddNumberToObject(header, "data_length", strlen(data_json));
    }

    // Add payload_length if we have payload
    if (payload && payload_length > 0) {
        cJSON_AddNumberToObject(header, "payload_length", (double)payload_length);
    }

    char *header_str = cJSON_PrintUnformatted(header);
    cJSON_Delete(header);

    if (!header_str) {
        LOG_WARN("Wyoming: failed to create event header\n");
        return -1;
    }

    // Send JSON header with newline
    size_t header_len = strlen(header_str);
    char *message = malloc(header_len + 2);
    strcpy(message, header_str);
    message[header_len] = '\n';
    message[header_len + 1] = '\0';

    LOG_TRACE("Wyoming: Sending event: %s\n", header_str);
    free(header_str);

    // Wait for socket to be writable
    struct pollfd pfd = {
        .fd = conn->socket_fd,
        .events = POLLOUT,
        .revents = 0
    };

    int poll_ret = poll(&pfd, 1, 3000);  // 3 second timeout
    if (poll_ret <= 0) {
        free(message);
        LOG_WARN("Wyoming: socket not ready for writing (timeout or error)\n");
        return -1;
    }

    // Check for socket errors
    if (pfd.revents & (POLLERR | POLLHUP)) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(conn->socket_fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
            LOG_WARN("Wyoming: socket error after poll: %s\n", strerror(error));
        } else {
            LOG_WARN("Wyoming: socket error after poll (POLLERR|POLLHUP)\n");
        }
        free(message);
        return -1;
    }

    // Send header
    ssize_t sent = send(conn->socket_fd, message, header_len + 1, 0);
    free(message);

    if (sent < 0) {
        LOG_WARN("Wyoming: send failed: %s\n", strerror(errno));
        return -1;
    }

    // Send data JSON if present
    if (data_json) {
        size_t data_len = strlen(data_json);
        sent = send(conn->socket_fd, data_json, data_len, 0);
        if (sent < 0) {
            LOG_WARN("Wyoming: send data failed: %s\n", strerror(errno));
            return -1;
        }
    }

    // Send payload if present
    if (payload && payload_length > 0) {
        size_t total_sent = 0;
        while (total_sent < payload_length) {
            // Wait for socket to be writable
            poll_ret = poll(&pfd, 1, 5000);  // 5 second timeout
            if (poll_ret <= 0) {
                LOG_WARN("Wyoming: socket timeout while sending payload\n");
                return -1;
            }

            ssize_t chunk_sent = send(conn->socket_fd, payload + total_sent, payload_length - total_sent, 0);
            if (chunk_sent < 0) {
                LOG_WARN("Wyoming: payload send failed: %s\n", strerror(errno));
                return -1;
            }
            total_sent += chunk_sent;
        }
        LOG_TRACE("Wyoming: Sent payload (%zu bytes)\n", total_sent);
    }

    return 0;
}

/*-----------------------------------------------------
 * ASR Operations (Whisper)
 *-----------------------------------------------------*/

int wyoming_asr_transcribe(const unsigned char* audio_data, size_t audio_length) {
    if (!wyoming_is_connected(WYOMING_SERVICE_WHISPER)) {
        LOG_WARN("Wyoming ASR: not connected to Whisper\n");
        return -1;
    }

    WyomingConnection *conn = &wyoming_state.whisper;

    if (!audio_data || audio_length == 0) {
        LOG_WARN("Wyoming ASR: empty audio data\n");
        return -1;
    }

    if (conn->awaiting_response) {
        LOG_WARN("Wyoming ASR: previous request still in progress, canceling it\n");
        conn->awaiting_response = FALSE;
    }

    // BUGFIX: Reconnect to Whisper before each ASR request
    // Wyoming Faster-Whisper server cannot handle multiple ASR requests on the same socket
    // This ensures each transcription gets a fresh connection
    LOG_TRACE("Wyoming ASR: Reconnecting to Whisper for fresh connection...\n");
    wyoming_disconnect(WYOMING_SERVICE_WHISPER);
    if (wyoming_connect(WYOMING_SERVICE_WHISPER) != 0) {
        LOG_WARN("Wyoming ASR: failed to reconnect to Whisper\n");
        return -1;
    }

    // Parse WAV header to get audio format
    if (audio_length < 44) {
        LOG_WARN("Wyoming ASR: audio data too small (need at least 44 bytes for WAV header)\n");
        return -1;
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

    const WavHeader *wav_header = (const WavHeader *)audio_data;

    // Verify WAV format
    if (memcmp(wav_header->riff, "RIFF", 4) != 0 || memcmp(wav_header->wave, "WAVE", 4) != 0) {
        LOG_WARN("Wyoming ASR: invalid WAV file format\n");
        return -1;
    }

    uint32_t pcm_data_size = wav_header->data_size;
    const unsigned char *pcm_data = audio_data + 44;

    LOG_TRACE("Wyoming ASR: Transcribing %d Hz, %d-bit, %d ch, %u bytes PCM\n",
        wav_header->sample_rate, wav_header->bits_per_sample, wav_header->num_channels, pcm_data_size);

    // Step 1: Send Transcribe event with language setting
    // Creates: {"type":"transcribe","version":"1.0.0","data_length":N}
    //          {"language":"sv"}
    cJSON *transcribe_data = cJSON_CreateObject();
    if (conn->language[0] != '\0') {
        cJSON_AddStringToObject(transcribe_data, "language", conn->language);
    }
    char *transcribe_json = cJSON_PrintUnformatted(transcribe_data);
    cJSON_Delete(transcribe_data);

    if (!transcribe_json) {
        LOG_WARN("Wyoming ASR: failed to create transcribe JSON\n");
        return -1;
    }

    LOG_TRACE("Wyoming ASR: Sending transcribe event (language=%s)\n", conn->language);
    if (wyoming_send_event(conn, "transcribe", transcribe_json, NULL, 0) != 0) {
        free(transcribe_json);
        LOG_WARN("Wyoming ASR: failed to send transcribe event\n");
        return -1;
    }
    free(transcribe_json);

    // Step 2: Send AudioChunk event with PCM data as payload
    // Creates: {"type":"audio-chunk","version":"1.0.0","data_length":N,"payload_length":M}
    //          {"rate":16000,"width":2,"channels":1,"timestamp":null}
    //          <PCM payload bytes>
    cJSON *chunk_data = cJSON_CreateObject();
    cJSON_AddNumberToObject(chunk_data, "rate", wav_header->sample_rate);
    cJSON_AddNumberToObject(chunk_data, "width", wav_header->bits_per_sample / 8);
    cJSON_AddNumberToObject(chunk_data, "channels", wav_header->num_channels);
    cJSON_AddNullToObject(chunk_data, "timestamp");

    char *chunk_json = cJSON_PrintUnformatted(chunk_data);
    cJSON_Delete(chunk_data);

    if (!chunk_json) {
        LOG_WARN("Wyoming ASR: failed to create audio-chunk JSON\n");
        return -1;
    }

    LOG_TRACE("Wyoming ASR: Sending audio-chunk event (%u bytes PCM)\n", pcm_data_size);
    if (wyoming_send_event(conn, "audio-chunk", chunk_json, pcm_data, pcm_data_size) != 0) {
        free(chunk_json);
        LOG_WARN("Wyoming ASR: failed to send audio-chunk event\n");
        return -1;
    }
    free(chunk_json);

    // Step 3: Send AudioStop event (triggers transcription on server)
    // Creates: {"type":"audio-stop","version":"1.0.0","data_length":N}
    //          {"timestamp":null}
    cJSON *stop_data = cJSON_CreateObject();
    cJSON_AddNullToObject(stop_data, "timestamp");

    char *stop_json = cJSON_PrintUnformatted(stop_data);
    cJSON_Delete(stop_data);

    if (!stop_json) {
        LOG_WARN("Wyoming ASR: failed to create audio-stop JSON\n");
        return -1;
    }

    LOG_TRACE("Wyoming ASR: Sending audio-stop event (triggering transcription)\n");
    if (wyoming_send_event(conn, "audio-stop", stop_json, NULL, 0) != 0) {
        free(stop_json);
        LOG_WARN("Wyoming ASR: failed to send audio-stop event\n");
        return -1;
    }
    free(stop_json);

    LOG_TRACE("Wyoming ASR: All events sent, waiting for transcript\n");

    conn->awaiting_response = TRUE;
    snprintf(conn->pending_request, sizeof(conn->pending_request), "ASR: %zu bytes", audio_length);

    return 0;
}

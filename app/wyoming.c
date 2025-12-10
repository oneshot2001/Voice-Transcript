/*
 * Wyoming Protocol TCP Client
 * Copyright (c) 2025 Fred Juhlin
 * MIT License
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
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }

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

    LOG("%s\n", conn->info_string);

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
        LOG_WARN("Wyoming: Failed to parse JSON message\n");
        return;
    }

    // Check message type
    cJSON *type = cJSON_GetObjectItem(json, "type");

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
        LOG("Wyoming: Received server info\n");
        // Server is ready
    }
    else if (strcmp(type->valuestring, "audio-start") == 0) {
        LOG("Wyoming: TTS audio-start\n");
        // Reset audio buffer
        conn->audio_buffer_size = 0;
    }
    else if (strcmp(type->valuestring, "audio-chunk") == 0) {
        // Extract payload_length - number of PCM bytes that will follow
        // Note: The audio format metadata comes as a separate JSON line AFTER this message
        // Format: {"type":"audio-chunk",...,"payload_length":N}
        //         {"rate":22050,"width":2,"channels":1,...}
        //         [N bytes of raw PCM data]

        cJSON *payload_length = cJSON_GetObjectItem(json, "payload_length");
        if (payload_length && cJSON_IsNumber(payload_length)) {
            // Mark that we're expecting one more JSON line (metadata) before binary data
            conn->binary_bytes_expected = payload_length->valueint;
            conn->binary_bytes_received = 0;
            // Stay in JSON mode to read the metadata line
            LOG("Wyoming: audio-chunk expects %zu bytes of PCM (after metadata)\n", conn->binary_bytes_expected);
        } else {
            LOG_WARN("Wyoming: audio-chunk missing payload_length field\n");
        }
    }
    else if (strcmp(type->valuestring, "audio-stop") == 0) {
        LOG("Wyoming: TTS audio-stop (total PCM: %zu bytes)\n", conn->audio_buffer_size);

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

            LOG("Wyoming: Built WAV file: %dHz, %d-bit, %d ch, %zu bytes total\n",
                conn->audio_rate, bits_per_sample, conn->audio_channels, wav_file_size);

            // Send to callback
            wyoming_state.audio_callback(conn->type, wav_file, wav_file_size);

            // Free WAV file buffer
            free(wav_file);
        }

        conn->awaiting_response = FALSE;
    }
    else if (strcmp(type->valuestring, "transcript") == 0) {
        LOG("Wyoming: ASR transcript result\n");
        cJSON *text = cJSON_GetObjectItem(json, "text");
        if (text && cJSON_IsString(text) && wyoming_state.transcript_callback) {
            wyoming_state.transcript_callback(text->valuestring);
        }
        conn->awaiting_response = FALSE;
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
        return G_SOURCE_CONTINUE;
    }

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
                                LOG_TRACE("Wyoming parser: no more data (remaining=0)\n");
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
                                LOG_TRACE("Wyoming parser: found '{', remaining=%zu\n", remaining);
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
                                    LOG_TRACE("Wyoming parser: complete JSON object, len=%zu\n", json_len);

                                    char saved_char = *json_end;
                                    *json_end = '\0';
                                    wyoming_process_message(conn, json_start, json_len);
                                    *json_end = saved_char;

                                    // Calculate bytes consumed BEFORE updating parse_ptr
                                    size_t consumed = json_end - parse_ptr;
                                    LOG_TRACE("Wyoming parser: consumed=%zu, remaining before=%zu\n", consumed, remaining);
                                    remaining -= consumed;
                                    parse_ptr = json_end;
                                    LOG_TRACE("Wyoming parser: remaining after=%zu\n", remaining);
                                    parsed_something = true;

                                    // Check if we should switch to binary mode
                                    if (conn->parse_state == WYOMING_PARSE_BINARY) {
                                        LOG_TRACE("Wyoming parser: switching to binary mode, breaking from JSON loop\n");
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
                        if (parse_ptr > conn->rx_buffer) {
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

    // If connected immediately (unlikely but possible)
    if (ret == 0) {
        wyoming_set_state(conn, WYOMING_STATE_CONNECTED, NULL);
    } else {
        // Will complete asynchronously, check in poll callback
        // For now, assume connection succeeds
        wyoming_set_state(conn, WYOMING_STATE_CONNECTED, NULL);
    }

    LOG("Wyoming connecting to %s:%d\n", conn->server_ip, conn->port);
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
 * ASR Operations (Whisper)
 *-----------------------------------------------------*/

int wyoming_asr_transcribe(const unsigned char* audio_data, size_t audio_length) {
    if (!wyoming_is_connected(WYOMING_SERVICE_WHISPER)) {
        LOG_WARN("Wyoming ASR: not connected to Whisper\n");
        return -1;
    }

    // TODO: Implement Wyoming ASR protocol
    // 1. Send transcribe request with audio data
    // 2. Receive transcript response
    // 3. Call transcript_callback when complete

    LOG("Wyoming ASR: transcribe request (TODO)\n");
    return 0;
}

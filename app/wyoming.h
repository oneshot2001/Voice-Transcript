/*
 * Wyoming Protocol TCP Client
 * Copyright (c) 2025 Fred Juhlin
 * BSD 3-Clause License
 *
 * Note: Uses TCP sockets instead of WebSockets since libwebsockets
 * is not available in ACAP SDK. Wyoming protocol works over raw TCP.
 */

#ifndef _WYOMING_H_
#define _WYOMING_H_

#include <glib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wyoming service types
typedef enum {
    WYOMING_SERVICE_PIPER = 0,    // TTS service
    WYOMING_SERVICE_WHISPER = 1   // ASR service
} WyomingServiceType;

// Wyoming connection states
typedef enum {
    WYOMING_STATE_DISCONNECTED = 0,
    WYOMING_STATE_CONNECTING = 1,
    WYOMING_STATE_CONNECTED = 2,
    WYOMING_STATE_ERROR = 3
} WyomingConnectionState;

// Callback for connection state changes
typedef void (*WyomingStateCallback)(WyomingServiceType service, WyomingConnectionState state, const char* error_msg);

// Callback for receiving audio data (WAV file)
typedef void (*WyomingAudioCallback)(WyomingServiceType service, const unsigned char* data, size_t length);

// Callback for receiving transcription text
typedef void (*WyomingTranscriptCallback)(const char* text);

/*-----------------------------------------------------
 * Initialization & Cleanup
 *-----------------------------------------------------*/

// Initialize Wyoming client system
// Must be called from main thread with GMainLoop
int wyoming_init(GMainLoop* main_loop);

// Cleanup Wyoming client system
void wyoming_cleanup(void);

/*-----------------------------------------------------
 * Configuration
 *-----------------------------------------------------*/

// Configure Wyoming server connection
// server_ip: IP address of Wyoming server
// piper_port: Port for Piper TTS service
// whisper_port: Port for Whisper ASR service
// language: Language code (e.g., "sv", "en")
int wyoming_configure(const char* server_ip, int piper_port, int whisper_port, const char* language);

// Set callback for connection state changes
void wyoming_set_state_callback(WyomingStateCallback callback);

// Set callback for receiving audio data (TTS response)
void wyoming_set_audio_callback(WyomingAudioCallback callback);

// Set callback for receiving transcription text (ASR result)
void wyoming_set_transcript_callback(WyomingTranscriptCallback callback);

/*-----------------------------------------------------
 * Connection Management
 *-----------------------------------------------------*/

// Connect to Wyoming service
// Returns: 0 on success, -1 on error
int wyoming_connect(WyomingServiceType service);

// Disconnect from Wyoming service
void wyoming_disconnect(WyomingServiceType service);

// Check if service is connected
bool wyoming_is_connected(WyomingServiceType service);

// Get current connection state
WyomingConnectionState wyoming_get_state(WyomingServiceType service);

/*-----------------------------------------------------
 * TTS (Piper) Operations
 *-----------------------------------------------------*/

// Send text to Piper TTS service and receive WAV audio
// text: Text to synthesize
// voice: Voice name (e.g., "sv_SE-nst-medium")
// Returns: 0 on success, -1 on error
// Result will be delivered via WyomingAudioCallback
int wyoming_tts_synthesize(const char* text, const char* voice);

/*-----------------------------------------------------
 * ASR (Whisper) Operations
 *-----------------------------------------------------*/

// Send audio to Whisper ASR service for transcription
// audio_data: WAV audio data (PCM 16-bit mono)
// audio_length: Length of audio data in bytes
// Returns: 0 on success, -1 on error
// Result will be delivered via WyomingTranscriptCallback
int wyoming_asr_transcribe(const unsigned char* audio_data, size_t audio_length);

/*-----------------------------------------------------
 * Status & Debugging
 *-----------------------------------------------------*/

// Get last error message for service
const char* wyoming_get_error(WyomingServiceType service);

// Get connection info (for debugging)
const char* wyoming_get_connection_info(WyomingServiceType service);

#ifdef __cplusplus
}
#endif

#endif // _WYOMING_H_

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Voice Assistant ACAP transforms Axis network speakers into intelligent voice assistants. Built on PipeWire for audio I/O with Wyoming protocol integration for Text-to-Speech (Piper) and Speech-to-Text (Whisper).

**Core Technologies:**
- ACAP SDK 12 (Axis Camera Application Platform)
- PipeWire for audio streaming
- Wyoming protocol (TCP-based) for TTS/STT
- MQTT for automation integration
- libcurl for HTTP downloads

## Build System

### Building the ACAP

The project uses Docker-based builds to create `.eap` packages for deployment to Axis devices.

**Build for both architectures:**
```bash
./build.sh
```

This creates two `.eap` files:
- `voice_0.9.0_armv7hf.eap` (default, most common)
- `voice_0.9.0_aarch64.eap` (newer devices)

**Build manually for specific architecture:**
```bash
# armv7hf (default)
docker build --tag acap .

# aarch64
docker build --build-arg ARCH=aarch64 --tag acap .

# Extract .eap file
container_id=$(docker create acap)
docker cp "$container_id":/opt/app ./build
docker rm "$container_id"
```

**What gets built:**
- Source files compiled: `main.c`, `ACAP.c`, `cJSON.c`, `pipewire_audio.c`, `wyoming.c`, `MQTT.c`, `CERTS.c`
- Configuration bundled: `settings/settings.json`, `settings/events.json`, `settings/mqtt.json`
- Web UI included: `html/index.html`

### Makefile Structure

Located at [app/Makefile](app/Makefile):

```makefile
PROG1   = voice
OBJS1   = main.c ACAP.c cJSON.c pipewire_audio.c wyoming.c MQTT.c CERTS.c
PKGS    = glib-2.0 gio-2.0 axevent fcgi libcurl libpipewire-0.3
```

**When adding new source files:**
1. Add `.c` file to `OBJS1` in Makefile
2. If using new libraries, add to `PKGS`
3. If exposing HTTP endpoints, add to `httpConfig` in [app/manifest.json](app/manifest.json)

## Architecture

### Module Responsibilities

**[main.c](app/main.c)** - Application orchestration
- Initializes all subsystems (ACAP, PipeWire, Wyoming, MQTT)
- Manages application state (`VoiceAssistantState`)
- HTTP endpoint handlers
- Audio callbacks (playback/recording)
- WAV file parsing and sample conversion

**[wyoming.c/.h](app/wyoming.h)** - Wyoming Protocol Client
- TCP connection management for Piper (TTS) and Whisper (STT)
- JSON + binary protocol state machine
- Handles connection lifecycle and reconnection
- Two independent services: `WYOMING_SERVICE_PIPER` and `WYOMING_SERVICE_WHISPER`

**[pipewire_audio.c/.h](app/pipewire_audio.h)** - Audio I/O Wrapper
- PipeWire stream management (capture and playback)
- F32 sample format handling
- Concurrent streams (can listen and speak simultaneously)
- Error callbacks for stream failures

**[MQTT.c](app/MQTT.c)** - MQTT Client Integration
- Auto-configured topics using device serial number
- Subscribe: `voice/speak/{SERIAL}`, `voice/playback/{SERIAL}`, etc.
- Publish: `voice/connect/{SERIAL}`, `voice/transcript/{SERIAL}`

**[ACAP.c/.h](app/ACAP.h)** - SDK Wrapper (DO NOT EDIT)
- HTTP endpoint routing
- Configuration management
- Event system
- Status reporting
- Device info access

### State Management

The `VoiceAssistantState` structure in [main.c](app/main.c) centralizes all runtime state:

```c
typedef struct {
    AudioStream input;         // Continuous input (future: wake-word)
    AudioStream output;        // Playback output
    PlaybackBuffer playback;   // Downloaded WAV files
    RecordingBuffer recording; // STT audio capture
    gchar *download_url;       // Active download
    gboolean downloading;
    GMutex playback_mutex;     // Synchronization for blocking HTTP
    GCond playback_cond;
    gboolean playback_complete;
} VoiceAssistantState;
```

**Critical Pattern:** The application uses GLib main loop with idle callbacks and mutexes to coordinate between HTTP requests, audio callbacks, and Wyoming protocol responses.

### HTTP API Endpoints

All endpoints defined in [manifest.json](app/manifest.json) and implemented in [main.c](app/main.c):

| Endpoint | Access | Purpose |
|----------|--------|---------|
| `/local/voice/speak` | viewer | TTS synthesis via Wyoming Piper |
| `/local/voice/playback` | admin | Play WAV from URL |
| `/local/voice/listen_start` | viewer | Start STT recording |
| `/local/voice/listen_stop` | viewer | Stop recording & get transcript |
| `/local/voice/transcription` | viewer | Get last transcription |
| `/local/voice/status` | admin | Stream status monitoring |
| `/local/voice/settings` | admin | Wyoming server configuration |

**Endpoint Registration Pattern:**
```c
ACAP_HTTP_Node("speak", HTTP_Endpoint_Speak);
ACAP_HTTP_Node("playback", HTTP_Endpoint_Playback);
```

### Wyoming Protocol Integration

**Connection Management:**
- Two independent TCP connections (Piper on port 10200, Whisper on 10300)
- State machine: `DISCONNECTED → CONNECTING → CONNECTED → ERROR`
- Automatic reconnection on configuration changes

**TTS Flow:**
1. HTTP request to `/speak` with `text` parameter
2. `wyoming_connect(WYOMING_SERVICE_PIPER)` if not connected
3. `wyoming_tts_synthesize(text, voice)` sends text
4. Wyoming returns JSON metadata + PCM audio chunks
5. WAV file constructed in memory
6. Audio callback triggers playback via PipeWire

**STT Flow:**
1. HTTP request to `/listen_start` starts recording
2. PipeWire capture stream fills `RecordingBuffer`
3. HTTP request to `/listen_stop` stops recording
4. Audio converted to WAV format (16-bit PCM mono 16kHz)
5. `wyoming_asr_transcribe(wav_data, length)` sends to Whisper
6. Transcript callback receives text, publishes to MQTT

**Protocol Details:**
- Each message: JSON event followed by optional binary payload
- Message format: `<JSON>\n[binary data if audio_bytes > 0]`
- State machine handles partial reads and chunked audio

### Audio Pipeline

**Playback Path (TTS or URL):**
```
HTTP Request → WAV Download/Synthesis → Parse WAV Header
→ Convert PCM to F32 → Fill PlaybackBuffer
→ PipeWire Playback Stream → Speaker Output
```

**Capture Path (STT):**
```
Microphone → PipeWire Capture Stream → F32 Samples
→ Fill RecordingBuffer → Convert F32 to PCM16
→ Build WAV File → Wyoming Whisper → Transcript
```

**Sample Conversion:**
- PipeWire uses F32 format internally
- Wyoming expects PCM 16-bit signed
- Conversion functions in [main.c](app/main.c): `convert_pcm16_to_f32()`, `convert_f32_to_pcm16()`

### Configuration System

**Settings File:** [app/settings/settings.json](app/settings/settings.json)
```json
{
    "wyoming": "",       // Wyoming server IP
    "piper": 10200,      // Piper TTS port
    "whisper": 10300,    // Whisper STT port
    "language": "en"     // Language code
}
```

**Runtime Settings:**
- Default settings in `app/settings/settings.json`
- User changes stored in device's `localdata/settings.json`
- ALWAYS use `ACAP_Get_Config("settings")` to read - never read files directly
- Settings updates trigger `Settings_Updated_Callback()` which reconfigures Wyoming

**Status Reporting:**
Use `ACAP_STATUS_Set*()` functions to expose runtime state via `/status` endpoint:
```c
ACAP_STATUS_SetString("wyoming", "piper", "connected");
ACAP_STATUS_SetNumber("output", "buffer_usage", 0.75);
```

### MQTT Integration

**Auto-Configuration:**
- Device serial number retrieved: `ACAP_DEVICE_Prop("serial")`
- Topics dynamically constructed: `voice/speak/{serial}`

**Topic Patterns:**
- Subscribe: Commands sent to this device
- Publish: Events/responses from this device

**MQTT Settings:** [app/settings/mqtt.json](app/settings/mqtt.json)

## Development Patterns

### Adding New HTTP Endpoints

1. **Declare in manifest.json:**
```json
{"name": "my_endpoint", "access": "admin", "type": "fastCgi"}
```

2. **Implement handler in main.c:**
```c
void HTTP_Endpoint_MyFeature(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Invalid Method");
        return;
    }
    const char* param = ACAP_HTTP_Request_Param(request, "param_name");
    // Process request...
    ACAP_HTTP_Respond_Text(response, "Success");
}
```

3. **Register in main():**
```c
ACAP_HTTP_Node("my_endpoint", HTTP_Endpoint_MyFeature);
```

### Wyoming Protocol Extensions

**Adding new TTS voices:**
- Modify voice parameter in `wyoming_tts_synthesize()` call
- Voice names must match Piper server configuration
- Example: `"sv_SE-nst-medium"`, `"en_US-lessac-medium"`

**Handling new Wyoming events:**
- Extend state machine in [wyoming.c](app/wyoming.c)
- Add new event types to protocol parser
- Update callbacks in [main.c](app/main.c)

### Audio Buffer Management

**Critical Memory Management:**
- Always allocate buffers before starting streams
- Free buffers before reallocating
- Use mutexes when accessing buffers from multiple threads

**Example Pattern:**
```c
if (va_state.playback.samples) {
    g_free(va_state.playback.samples);
}
va_state.playback.samples = g_malloc0(sample_count * sizeof(float));
```

### Error Handling

**Logging Levels:**
```c
LOG()        // Normal info messages (always shown)
LOG_WARN()   // Warnings and errors (always shown)
LOG_TRACE()  // Verbose debug (toggle in main.c)
```

**Status Updates:**
Always update status on errors for web UI monitoring:
```c
ACAP_STATUS_SetString("wyoming", "error", error_message);
```

## Testing & Debugging

### Viewing Logs

After deploying to device:
```bash
# SSH into device
ssh root@<device-ip>

# Follow application logs
journalctl -u voice.service -f
```

### Common Issues

**Wyoming Connection Failures:**
- Check Wyoming server IP in settings
- Verify Piper/Whisper services running on correct ports
- Check network connectivity from device to Wyoming server

**Audio Playback Issues:**
- Verify PipeWire service running: `systemctl status pipewire`
- Check device user in `pipewire` group (manifest.json)
- Sample rate mismatches (ensure conversion enabled)

**MQTT Not Working:**
- Check MQTT broker configuration in settings
- Verify device serial number in topic names
- Check broker connectivity and authentication

## Key Files Reference

**Core Application:**
- [app/main.c](app/main.c) - Main application logic (~1500 lines)
- [app/ACAP.c](app/ACAP.c) - SDK wrapper (DO NOT EDIT)
- [app/ACAP.h](app/ACAP.h) - SDK API reference

**Wyoming Integration:**
- [app/wyoming.c](app/wyoming.c) - Protocol implementation
- [app/wyoming.h](app/wyoming.h) - Public API

**Audio System:**
- [app/pipewire_audio.c](app/pipewire_audio.c) - PipeWire wrapper
- [app/pipewire_audio.h](app/pipewire_audio.h) - Audio API

**Configuration:**
- [app/manifest.json](app/manifest.json) - App metadata, endpoints, permissions
- [app/settings/settings.json](app/settings/settings.json) - User settings
- [app/settings/mqtt.json](app/settings/mqtt.json) - MQTT configuration
- [app/settings/events.json](app/settings/events.json) - Event declarations

**Build System:**
- [Dockerfile](Dockerfile) - SDK build environment
- [app/Makefile](app/Makefile) - Compilation rules
- [build.sh](build.sh) - Build automation script

## Important Notes

**Thread Safety:**
- PipeWire callbacks run on audio thread
- HTTP handlers run on FastCGI thread
- Wyoming protocol runs on GLib main loop thread
- Use mutexes (`GMutex`) for shared state access

**GLib Main Loop:**
- Critical for async I/O and Wyoming protocol
- Use `g_idle_add()` for scheduling work on main thread
- Signal handlers integrated via `g_unix_signal_source_new(SIGTERM)`

**Memory Management:**
- Use GLib allocators: `g_malloc()`, `g_free()`, `g_strdup()`
- cJSON objects: always `cJSON_Delete()` after use
- Never free `ACAP_Get_Config()` return values (SDK managed)
- Free WAV download buffers after playback complete

**Wyoming Protocol:**
- Uses raw TCP sockets (not WebSockets - libwebsockets unavailable)
- Stateful protocol with handshake sequence
- Must handle partial reads and message reassembly
- JSON messages newline-delimited, binary audio follows

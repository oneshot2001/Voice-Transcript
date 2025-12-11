# Voice Assistant ACAP - Architecture

## Overview
This ACAP application is designed to be a voice assistant running on an Axis network speaker device. The architecture supports concurrent audio input and output streams, enabling future wake-word detection and real-time TTS playback.

## Current Implementation (Step 1-7)

### ✅ Completed Features
1. **Wyoming Protocol TTS Integration**
   - HTTP POST endpoint: `/local/voice/speak`
   - Direct TCP connection to Wyoming Piper server
   - Complex JSON + binary protocol parsing
   - Real-time WAV file construction from PCM chunks
   - Automatic playback of Swedish TTS audio
   - Configuration via `/local/voice/settings`

2. **Wyoming Protocol STT Integration** 🆕
   - HTTP POST endpoints: `/local/voice/listen_start`, `/local/voice/listen_stop`
   - HTTP GET endpoint: `/local/voice/transcription`
   - Direct TCP connection to Wyoming Faster-Whisper server
   - Real-time audio recording from microphone
   - Push-to-talk interface in web UI
   - Transcript published via MQTT

3. **MQTT Integration** 🆕
   - Auto-configured topics with device serial: `voice/{action}/{SERIAL}`
   - Subscribe: `voice/speak/{SERIAL}`, `voice/playback/{SERIAL}`, `voice/listen/start/{SERIAL}`, `voice/listen/stop/{SERIAL}`
   - Publish: `voice/connect/{SERIAL}`, `voice/transcript/{SERIAL}`

4. **WAV File Playback from URL**
   - HTTP POST endpoint: `/local/voice/playback`
   - Accepts JSON payload: `{"url":"..."}`
   - Downloads WAV file from provided URL
   - Converts PCM16 to F32 for PipeWire
   - Plays audio through speaker

5. **Status Reporting**
   - `input.*` - Input stream status (recording state)
   - `output.state` - Boolean (0=idle, 1=playing)
   - `output.samples` - Number of samples played
   - `output.size` - Total samples in buffer
   - `output.status` - Human-readable status
   - `output.error` - Error messages
   - `stt.last_transcript` - Last STT transcription

6. **Independent Audio Streams**
   - Input and output streams are completely independent
   - Can run simultaneously (recording + playback)

### Audio Format Support
**Currently Supported:**
- Format: WAV (RIFF)
- Encoding: PCM 16-bit
- Channels: Mono (1 channel)
- Sample Rate: Any (will be resampled by PipeWire if needed)

**Internal Format:**
- PipeWire: F32 (32-bit float, -1.0 to 1.0)
- Conversion: Automatic PCM16 → F32

## HTTP API

### POST /local/voice/speak
Synthesizes text to speech using Wyoming Piper protocol and plays the audio.

**Request:**
```bash
curl -X POST -H 'Content-Type: application/json' \
  --digest -u nodered:rednode \
  http://speaker.internal/local/voice/speak \
  -d '{"text":"Hej, detta är ett test av röstsyntes"}'
```

**Response:**
- `200 OK` - "TTS request accepted, audio will play when ready"
- `400 Bad Request` - Invalid JSON or missing text
- `500 Internal Server Error` - Wyoming connection error

**Features:**
- Direct TCP connection to Wyoming Piper server
- Streaming binary PCM data reception
- Automatic WAV construction with correct headers
- Immediate playback after synthesis completes

### POST /local/voice/playback
Downloads and plays a WAV file from the specified URL.

**Request:**
```bash
curl -X POST -H 'Content-Type: application/json' \
  --digest -u nodered:rednode \
  http://speaker.internal/local/voice/playback \
  -d '{"url":"http://10.13.8.183:5001/test.wav"}'
```

**Response:**
- `200 OK` - "Playback started"
- `400 Bad Request` - Invalid request or missing URL
- `409 Conflict` - Already playing or downloading
- `500 Internal Server Error` - Download or playback failed

### POST /local/voice/listen_start 🆕
Starts recording audio from the microphone for speech-to-text.

**Request:**
```bash
curl -X POST --digest -u nodered:rednode \
  http://speaker.internal/local/voice/listen_start
```

**Response:**
- `200 OK` - "Listening started"
- `409 Conflict` - Already recording
- `500 Internal Server Error` - Failed to start recording

### POST /local/voice/listen_stop 🆕
Stops recording and sends audio to Wyoming Faster-Whisper for transcription.

**Request:**
```bash
curl -X POST --digest -u nodered:rednode \
  http://speaker.internal/local/voice/listen_stop
```

**Response:**
- `200 OK` - "Recording stopped, transcription in progress"
- `400 Bad Request` - Not currently recording
- `500 Internal Server Error` - Failed to stop or transcribe

**Note:** Transcription is sent via MQTT topic `voice/transcript/{SERIAL}` when complete.

### GET /local/voice/transcription 🆕
Retrieves the last speech-to-text transcription.

**Request:**
```bash
curl --digest -u nodered:rednode \
  http://speaker.internal/local/voice/transcription
```

**Response:**
```json
{
  "text": " Detta är en transkription"
}
```

### GET /local/voice/settings
Returns or updates Wyoming server configuration.

**GET Request:**
```bash
curl --digest -u nodered:rednode \
  http://speaker.internal/local/voice/settings
```

**Response:**
```json
{
  "wyoming": "10.13.8.2",
  "piper": 10200,
  "whisper": 10300,
  "language": "sv"
}
```

**POST Request (Update):**
```bash
curl -X POST -H 'Content-Type: application/json' \
  --digest -u nodered:rednode \
  http://speaker.internal/local/voice/settings \
  -d '{"wyoming":"10.13.8.2","piper":10200,"language":"sv"}'
```

### GET /local/voice/status
Returns current status of input and output audio streams.

**Request:**
```bash
curl http://speaker.internal/local/voice/status
```

**Response example:**
```json
{
  "input": {
    "state": false,
    "samples": 0,
    "status": "Not running"
  },
  "output": {
    "state": true,
    "samples": 12580,
    "size": 48000,
    "status": "Playing audio..."
  }
}
```

## MQTT Topics 🆕

All MQTT topics are auto-configured with the device serial number: `voice/{action}/{SERIAL}`

### Subscribe Topics (Commands to Device)

**voice/speak/{SERIAL}**
- Trigger TTS synthesis and playback
- Payload: `{"text":"Hello, world"}`

**voice/playback/{SERIAL}**
- Play WAV file from URL
- Payload: `{"url":"http://server/audio.wav"}`

**voice/listen/start/{SERIAL}**
- Start STT recording
- Payload: `{}`

**voice/listen/stop/{SERIAL}**
- Stop STT recording and transcribe
- Payload: `{}`

### Publish Topics (Events from Device)

**voice/connect/{SERIAL}**
- Published on MQTT connection/startup
- Payload: Device information (model, firmware, etc.)

**voice/transcript/{SERIAL}**
- Published when STT transcription completes
- Payload: `{"text":"Transcribed text here"}`

## State Management

### VoiceAssistantState Structure
```c
typedef struct {
    // Audio streams (independent)
    AudioStream input;         // Future: wake-word detection
    AudioStream output;        // TTS playback

    // Playback buffer
    PlaybackBuffer playback;   // WAV data (F32 samples)

    // Download state
    gchar *download_url;
    gboolean downloading;
} VoiceAssistantState;
```

### AudioStream
```c
typedef struct {
    PWAudio *stream;           // PipeWire stream handle
    gboolean active;           // Is stream currently active?
    guint32 sample_count;      // Total samples processed
} AudioStream;
```

### PlaybackBuffer
```c
typedef struct {
    float *samples;            // Audio samples (F32)
    guint32 size;              // Total samples allocated
    guint32 write_pos;         // Write position (for loading)
    guint32 read_pos;          // Read position (for playback)
    gboolean ready;            // Ready for playback
} PlaybackBuffer;
```

## Threading Model

### Main Thread
- GLib main loop
- PipeWire event processing (via GSource)
- Audio callbacks (input/output)
- Idle callbacks

### FastCGI Thread(s)
- HTTP request handlers
- Downloads WAV files (blocking)
- Queues work to main thread via `g_idle_add()`

**Critical Rule:** All PipeWire stream creation MUST happen in main thread via idle callbacks!

## Audio Pipeline

### Playback Flow
```
HTTP POST /playback
    ↓
Download WAV (FastCGI thread)
    ↓
Parse WAV header
    ↓
Convert PCM16 → F32 samples
    ↓
Load into playback buffer
    ↓
g_idle_add(start_playback_idle)
    ↓
Main thread: pw_audio_playback_start()
    ↓
audio_output_callback() fires repeatedly
    ↓
Copy samples from buffer → PipeWire → Speaker
    ↓
Playback complete → Stop stream
```

## Wyoming Protocol Details 🆕

### Protocol Flow
```
Client → Server: {"type":"synthesize","data":{"text":"Hej"}}
Server → Client: {"type":"audio-start",...}
Server → Client: {"type":"audio-chunk",...,"payload_length":2048}
Server → Client: {"rate":22050,"width":2,"channels":1,...}
Server → Client: [2048 bytes of binary PCM data]
Server → Client: ... (more chunks)
Server → Client: {"type":"audio-stop",...}
```

### Technical Challenges Solved
1. **Non-blocking Socket Timing** - Added poll() with 3s timeout before send()
2. **Multi-Object JSON Lines** - Brace-counting parser handles `}{"type"` without whitespace
3. **Binary Mode Switching** - State machine switches between JSON and binary parsing
4. **WAV Header Construction** - audio_width in BYTES (2) → bits_per_sample (16)
5. **Playback Buffer Management** - Set both `size` and `write_pos` for PipeWire

### Audio Format
- Sample Rate: 22050 Hz
- Bit Depth: 16-bit signed PCM
- Channels: Mono (1 channel)
- Container: WAV (RIFF) with 44-byte header

## Future Roadmap

### ✅ Steps 1-7: COMPLETED
- ✅ WAV playback from URL
- ✅ Wyoming protocol TCP client
- ✅ TTS synthesis (Piper)
- ✅ STT transcription (Faster-Whisper)
- ✅ Push-to-talk interface
- ✅ MQTT integration with device serial
- ✅ Swedish & English language support

### Step 8: Continuous Input (Wake-Word Detection)
- Start input stream on initialization
- Process audio in `audio_input_callback()`
- Integrate wake-word detection library (Porcupine, Snowboy, etc.)
- Detect wake-word and trigger recording

### Step 9: VAD-Triggered Recording
- On wake-word detection, start automatic recording
- Buffer audio for speech recognition
- Run VAD (Voice Activity Detection)
- End recording on silence detection

### Step 10: Full Voice Assistant
```
Continuous Input (wake-word)
    ↓
Wake-word detected!
    ↓
Start Recording (VAD)
    ↓
Speech ends (VAD)
    ↓
Send to Wyoming-whisper ← WORKING!
    ↓
Receive transcription ← WORKING!
    ↓
Process intent
    ↓
Generate TTS (Wyoming-piper) ← WORKING!
    ↓
Playback response ← WORKING!
    ↓
Resume wake-word listening
```

## Key Files

### Core Application
- [app/main.c](app/main.c) - Main application logic, HTTP handlers, WAV playback
- [app/ACAP.c](app/ACAP.c) - ACAP framework wrapper
- [app/ACAP.h](app/ACAP.h) - ACAP API

### Wyoming Protocol Integration 🆕
- [app/wyoming.c](app/wyoming.c) - Wyoming protocol TCP client implementation
- [app/wyoming.h](app/wyoming.h) - Wyoming client API
- Features:
  - Non-blocking TCP sockets with poll()
  - Complex JSON + binary protocol parser
  - Brace-counting for multi-object JSON lines
  - Binary mode switching for PCM data chunks
  - WAV file construction (16-bit PCM)
  - Configurable server/port/language

### PipeWire Integration
- [app/pipewire_audio.c](app/pipewire_audio.c) - PipeWire wrapper library
- [app/pipewire_audio.h](app/pipewire_audio.h) - Public API

### Build
- [app/Makefile](app/Makefile) - Build configuration

## Dependencies

### System Libraries
- `libpipewire-0.3` - Audio subsystem
- `libcurl` - HTTP downloads
- `glib-2.0` - Main loop, utilities
- `gio-2.0` - I/O operations
- `fcgi` - FastCGI web server

### ACAP SDK
- `vdostream` - Video/audio device access
- `axevent` - Axis event system

## Testing

### Test Playback
```bash
# Generate test WAV file (16-bit PCM, mono, 16kHz)
sox -n -r 16000 -c 1 -b 16 test.wav synth 2 sine 440

# Serve on HTTP server
cd /path/to/wav/files
python3 -m http.server 8000

# Trigger playback
curl -X POST http://<camera-ip>/local/base/playback \
  -d "http://10.13.8.183:8000/test.wav"

# Monitor status
watch curl -s http://<camera-ip>/local/base/status
```

### Expected Log Output
```
Downloading WAV from: http://10.13.8.183:8000/test.wav
Downloaded 64088 bytes
WAV: 16000 Hz, 1 channels, 16 bits, format=1
Converting 32000 samples from PCM16 to F32...
WAV loaded: 32000 samples (2.00 seconds at 16000 Hz)
Scheduled playback idle callback, id=6
start_playback_idle: Starting audio playback from main thread
audio_stream_start: Starting PLAYBACK stream
Connected to PipeWire core
pw_registry_event_global: Found Node: media_class=Audio/Sink, name=AudioDevice0Output0
Audio stream to node AudioDevice0Output0, 1 channel(s)
Stream state changed paused -> streaming
Playback completed (32000 samples)
```

## Design Principles

1. **Separation of Concerns**
   - Input and output are independent
   - Each stream has its own state
   - No mutual exclusion between streams

2. **Thread Safety**
   - All PipeWire operations in main thread
   - FastCGI handlers use idle callbacks
   - No blocking operations in audio callbacks

3. **Resource Management**
   - Streams stopped and cleaned up properly
   - GSource destroyed when stream stops
   - Buffers freed on shutdown

4. **Status Transparency**
   - All state exposed via HTTP status endpoint
   - Detailed error reporting
   - Progress tracking (samples played/total)

5. **Future-Proof**
   - Architecture supports concurrent streams
   - Easy to add wake-word detection
   - Easy to add VAD recording
   - Ready for Wyoming protocol integration

## Known Limitations

1. **WAV Format Only**
   - Only PCM 16-bit mono supported
   - No MP3, OGG, or other formats
   - No stereo support

2. **Blocking Downloads**
   - WAV download blocks FastCGI thread
   - Large files may timeout
   - Consider async download in future

3. **No Streaming Playback**
   - Entire WAV must be downloaded first
   - Cannot stream large files
   - Memory usage proportional to file size

4. **Single Playback Instance**
   - Cannot queue multiple playback requests
   - Must wait for current playback to finish
   - Future: Add playback queue

## Performance Characteristics

### Memory Usage
- Base: ~1 MB (application + libraries)
- Per WAV file: ~4 bytes per sample (F32)
- Example: 10 seconds at 16kHz = 160,000 samples = 640 KB

### CPU Usage
- Idle: <1% CPU
- Download: ~5% CPU (network I/O)
- Playback: <2% CPU (memory copy)
- PCM16→F32 conversion: Negligible

### Network
- Bandwidth: Depends on WAV file size
- Typical TTS response: 50-200 KB
- Download time: <1 second on LAN

---

**Last Updated:** 2025-12-11
**Status:** Steps 1-7 Complete - Wyoming TTS & STT Fully Working! 🎉
**Next Step:** Continuous input stream for wake-word detection (Step 8)

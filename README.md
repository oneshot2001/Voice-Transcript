# Voice Assistant ACAP

A voice assistant application for Axis network speaker devices, featuring Wyoming protocol integration for Text-to-Speech (TTS) synthesis and future support for wake-word detection and speech recognition.

## Overview

This ACAP (Axis Camera Application Platform) application transforms Axis network speakers into intelligent voice assistants. Built on PipeWire for audio I/O, the application provides direct TCP integration with Wyoming protocol servers (Piper for TTS, Whisper for ASR) and supports concurrent audio streams for simultaneous listening and speaking.

**Platform:** Freescale i.MX6 Ultralite ARM (armv7hf)
**Target Device:** Axis Network Speaker (e.g., C8210 Network Audio Bridge)

## 🚀 Key Features

### Current Implementation (v0.5.0)

✅ **Wyoming Protocol TTS Integration**
- Direct TCP connection to Wyoming Piper server (no HTTP intermediary)
- Complex JSON + binary protocol parsing with state machine
- Real-time WAV file construction from PCM audio chunks
- Swedish language TTS synthesis
- Configurable server settings via HTTP API

✅ **Audio Playback**
- Download and play WAV files from URLs
- PCM 16-bit to F32 sample conversion
- PipeWire integration for speaker output
- Support for any sample rate (automatic resampling)

✅ **HTTP API**
- `/local/voice/speak` - Text-to-speech synthesis
- `/local/voice/playback` - Play WAV from URL
- `/local/voice/status` - Stream status monitoring
- `/local/voice/settings` - Wyoming server configuration
- `/local/voice/test_download` - Network diagnostics

✅ **Architecture**
- Independent input/output audio streams
- Thread-safe GLib main loop integration
- Non-blocking socket I/O with poll()
- Status reporting and error handling

### Audio Format Support

**Supported:**
- Format: WAV (RIFF)
- Encoding: PCM 16-bit signed
- Channels: Mono (1 channel)
- Sample Rate: Any (PipeWire handles resampling)

**Internal:**
- PipeWire: F32 (32-bit float, -1.0 to 1.0)
- Automatic PCM16 → F32 conversion

## 📖 Usage

### Installation

1. Build the ACAP package:
```bash
cd /home/fred/ACAP/Voice
docker run --rm -v $PWD:/opt/app -w /opt/app axisecp/acap-native-sdk:12.0-armv7hf-ubuntu24.04 make
```

2. Install on speaker:
```bash
./install.sh
```

Or manually:
```bash
curl -X POST --digest -u nodered:rednode \
  -F "packfil=@Voice_0.5.0_armv7hf.eap;type=application/octet-stream" \
  http://speaker.internal/axis-cgi/applications/upload.cgi
```

### Text-to-Speech (TTS)

Synthesize and play Swedish text:

```bash
curl -X POST -H 'Content-Type: application/json' \
  --digest -u nodered:rednode \
  http://speaker.internal/local/voice/speak \
  -d '{"text":"Hej, detta är ett test av röstsyntes"}'
```

### Play WAV from URL

```bash
curl -X POST http://speaker.internal/local/voice/playback \
  -d "http://10.13.8.183:5001/test.wav"
```

### Configuration

Get current settings:
```bash
curl --digest -u nodered:rednode \
  http://speaker.internal/local/voice/settings
```

Update Wyoming server configuration:
```bash
curl -X POST -H 'Content-Type: application/json' \
  --digest -u nodered:rednode \
  http://speaker.internal/local/voice/settings \
  -d '{"wyoming":"10.13.8.2","piper":10200,"whisper":10300,"language":"sv"}'
```

### Status Monitoring

```bash
curl http://speaker.internal/local/voice/status
```

Example response:
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

## 🏗️ Architecture

### Wyoming Protocol Implementation

The application implements a custom Wyoming protocol TCP client with:

1. **Non-blocking Sockets** - Uses poll() with timeout for write-readiness
2. **Brace-Counting JSON Parser** - Handles multiple JSON objects per line without whitespace
3. **State Machine** - Switches between JSON parsing and binary PCM data modes
4. **WAV Construction** - Builds complete WAV files from streaming PCM chunks

**Protocol Flow:**
```
Client → Server: {"type":"synthesize","data":{"text":"Hej"}}
Server → Client: {"type":"audio-start",...}
Server → Client: {"type":"audio-chunk",...,"payload_length":2048}
Server → Client: {"rate":22050,"width":2,"channels":1,...}
Server → Client: [2048 bytes of binary PCM data]
Server → Client: ... (more chunks)
Server → Client: {"type":"audio-stop",...}
```

### Threading Model

```
Main Thread:
  ├─ GLib Main Loop (g_main_loop_run)
  ├─ PipeWire event loop integration (via GSource)
  ├─ Audio callbacks (input/output)
  └─ Idle callbacks (start_playback_idle)

FastCGI Thread:
  ├─ HTTP request handlers
  ├─ WAV downloads (blocking)
  └─ Queues work to main thread via g_idle_add()
```

**Critical Rule:** All PipeWire stream operations happen in main thread!

## 🔧 Technical Details

### Key Components

- [app/main.c](app/main.c) - Main application, HTTP handlers, audio playback
- [app/wyoming.c](app/wyoming.c) - Wyoming protocol TCP client (~616 lines)
- [app/wyoming.h](app/wyoming.h) - Wyoming client API
- [app/pipewire_audio.c](app/pipewire_audio.c) - PipeWire wrapper library
- [app/ACAP.c](app/ACAP.c) - ACAP framework wrapper

### Dependencies

- `libpipewire-0.3` - Audio subsystem
- `libcurl` - HTTP downloads
- `glib-2.0` - Main loop and utilities
- `gio-2.0` - I/O operations
- `fcgi` - FastCGI web server
- `axevent` - Axis event system (ACAP SDK)
- `vdostream` - Video/audio device access (ACAP SDK)

### Wyoming Server Setup

This application requires a Wyoming protocol server running on your network:

**Piper TTS Server:**
```bash
docker run -d -p 10200:10200 \
  -v /path/to/piper:/data \
  rhasspy/wyoming-piper \
  --voice sv_SE-nst-medium
```

**Whisper ASR Server (Future):**
```bash
docker run -d -p 10300:10300 \
  rhasspy/wyoming-whisper \
  --model base \
  --language sv
```

## 🛣️ Roadmap

### ✅ Steps 1-4: COMPLETED (v0.5.0)
- ✅ WAV playback from URL
- ✅ Wyoming protocol TCP client
- ✅ TTS synthesis (Piper)
- ✅ Swedish language support

### 🔜 Future Development

**Step 5: Continuous Input Stream**
- Start input stream on initialization
- Process audio in real-time
- Integrate wake-word detection library (Porcupine, Snowboy)

**Step 6: VAD-Triggered Recording**
- Detect wake-word and start recording
- Voice Activity Detection (VAD)
- End recording on silence detection

**Step 7: Speech Recognition**
- Send recorded audio to Wyoming-whisper server
- Receive transcription/intent
- Process user commands

**Step 8: Full Voice Assistant Loop**
```
Continuous Input (wake-word detection)
    ↓
Wake-word detected!
    ↓
Record speech (VAD)
    ↓
Send to Wyoming-whisper
    ↓
Receive transcription
    ↓
Process intent
    ↓
Generate TTS (Wyoming-piper) ← WORKING!
    ↓
Playback response ← WORKING!
    ↓
Resume wake-word listening
```

## 🐛 Troubleshooting

### Wyoming Connection Issues

**Symptom:** "Wyoming TTS: connect failed" or timeout errors

**Solutions:**
1. Verify Wyoming server is running: `echo '{"type":"describe"}' | nc 10.13.8.2 10200`
2. Check network routing from speaker to Wyoming server
3. Verify port accessibility (10200 for Piper, 10300 for Whisper)
4. Check Wyoming server logs for connection attempts

### No Audio Output

**Symptom:** TTS synthesis succeeds but no sound

**Solutions:**
1. Check device speaker volume/mute settings
2. Verify PipeWire routing in logs (look for AudioDevice0Output0)
3. Check audio format compatibility (22050 Hz, 16-bit, mono)
4. Monitor status endpoint for error messages
5. Look for "Playback completed (N samples)" in logs

### Build Errors

**Common issues:**
- Missing PKG_CONFIG_PATH for cross-compilation
- Undefined symbols → missing library in LDLIBS
- Include path issues → check CFLAGS
- New files not in Makefile → update OBJS1 variable

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) - Detailed system architecture
- [work-in-progress.md](work-in-progress.md) - Development progress and testing
- [Wyoming Protocol](https://github.com/rhasspy/wyoming) - Official protocol documentation
- [PipeWire Docs](https://docs.pipewire.org/) - Audio subsystem reference

## 📈 Performance

### Memory Usage
- Base: ~1 MB (application + libraries)
- Per WAV file: ~4 bytes per sample (F32)
- Example: 10 seconds at 16kHz = 160,000 samples = 640 KB

### CPU Usage
- Idle: <1% CPU
- Download: ~5% CPU (network I/O)
- Playback: <2% CPU (memory copy)
- TTS synthesis: <5% CPU (TCP I/O + parsing)

### Network
- Bandwidth: Depends on WAV file size
- Typical TTS response: 50-200 KB
- Latency: 1-2 seconds from request to audio start

## 🔒 Security

- HTTP digest authentication required for sensitive endpoints
- Network-isolated Wyoming protocol (no internet exposure)
- Read-only file system (ACAP sandbox)
- Limited system permissions (pipewire group only)

## 📝 Version History

### 0.5.0 - December 10, 2025
- Wyoming Protocol TTS Integration
  - Direct TCP connection to Wyoming Piper server
  - Complex JSON + binary protocol parsing
  - Non-blocking socket I/O with poll()
  - Brace-counting JSON parser for complex protocol
  - State machine for JSON ↔ binary mode switching
  - Real-time WAV construction from PCM chunks
- HTTP Endpoints
  - `/local/voice/speak` - TTS synthesis
  - `/local/voice/settings` - Configuration management
  - `/local/voice/test_wyoming` - Wyoming server testing
- Audio System
  - WAV playback from URL
  - PCM16 to F32 conversion
  - PipeWire integration
  - Status reporting
- Architecture
  - Independent input/output streams
  - Thread-safe main loop integration
  - Swedish language support
- Bug Fixes
  - Fixed playback buffer write_pos assignment
  - Fixed WAV parsing PCM data offset
  - Fixed non-blocking socket timing with poll()
  - Fixed WAV header bits_per_sample calculation

### Initial Release
- Basic ACAP project structure
- PipeWire audio framework
- FastCGI HTTP server
- ACAP SDK integration

## 👤 Author

**Fred Juhlin**
Website: https://pandosme.github.io

## 📄 License

MIT License

Copyright (c) 2025 Fred Juhlin

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

**Status:** Steps 1-4 Complete - Wyoming TTS Fully Working! 🎉
**Next Step:** Continuous input stream for wake-word detection (Step 5)

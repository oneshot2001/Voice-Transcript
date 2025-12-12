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


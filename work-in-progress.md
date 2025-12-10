# Voice Assistant ACAP - Work in Progress

## Project Overview
Building a voice assistant application on Axis network speaker (Freescale i.MX6 Ultralite ARM platform) using PipeWire for audio I/O.

**Reference Implementation:** `/home/fred/ACAP/ssw-audio` - Working SSW Audio library
**Current Project:** `/home/fred/ACAP/test_piper` - Voice Assistant ACAP

---

## Current Status: 🎉 STEPS 1-4 COMPLETE - WYOMING TTS FULLY WORKING!

### Architecture Redesign Complete ✅
- **Previous:** Simple record/playback test application
- **Now:** Full voice assistant architecture with concurrent streams
- Separated input (future: wake-word) and output (playback) streams
- Independent state management for each stream
- No mutual exclusion between record/playback

### Step 1: WAV Playback Implementation ✅
**Goal:** HTTP POST endpoint that downloads and plays WAV files from URL

**Completed Features:**
- ✅ HTTP POST `/local/voice/playback` endpoint
- ✅ WAV file download via libcurl
- ✅ WAV header parsing and validation
- ✅ PCM16 to F32 sample conversion
- ✅ Playback buffer management
- ✅ PipeWire output stream integration
- ✅ Status reporting (input.*, output.*)
- ✅ Diagnostic endpoint for network testing
- ✅ Network connectivity verified and working

**WAV Format Supported:**
- Sample Rate: 16000 Hz (or any rate, PipeWire will resample)
- Format: PCM 16-bit
- Channels: Mono (1 channel)
- Container: WAV (RIFF)

### Steps 2-4: Wyoming Protocol TTS ✅ **NEW!**
**Goal:** Direct text-to-speech using Wyoming Piper protocol

**Completed Features:**
- ✅ Wyoming protocol TCP client ([wyoming.c](app/wyoming.c), [wyoming.h](app/wyoming.h))
- ✅ HTTP POST `/local/voice/speak` endpoint
- ✅ Direct TCP connection to Wyoming Piper server (10.13.8.2:10200)
- ✅ Complex JSON + binary protocol parsing
- ✅ Non-blocking socket with poll() for write-readiness
- ✅ Brace-counting JSON parser (handles `}{` without whitespace)
- ✅ State machine for JSON ↔ binary mode switching
- ✅ Real-time WAV file construction from PCM chunks
- ✅ Automatic playback of Swedish TTS audio
- ✅ Configuration via `/local/voice/settings`

**Technical Challenges Solved:**
1. Non-blocking socket timing → Added poll() with 3s timeout
2. Multi-object JSON lines → Brace-counting parser
3. Binary mode switching → State machine with break on mode change
4. WAV header bits/bytes → audio_width (bytes) × 8 = bits_per_sample
5. Playback buffer → Set both `size` and `write_pos` for completion check

**Testing Results:**
- Test 1: 63744 samples (2.89 seconds) ✓
- Test 2: 36864 samples (1.67 seconds) ✓
- Test 3: 56064 samples (2.54 seconds) ✓

---

## Code Changes Summary

### Wyoming Protocol Implementation
- **NEW FILES:**
  - [app/wyoming.c](app/wyoming.c) - Complete Wyoming protocol TCP client (~616 lines)
  - [app/wyoming.h](app/wyoming.h) - Public API for Wyoming client (~128 lines)

### Key Code Sections

#### 1. Brace-Counting JSON Parser ([wyoming.c:365-406](app/wyoming.c#L365-L406))
Handles Wyoming's unique format where multiple JSON objects appear on same line without whitespace.

#### 2. Socket Polling with Write-Readiness ([wyoming.c:585-597](app/wyoming.c#L585-L597))
Non-blocking socket requires poll() to wait for writability before send().

#### 3. WAV Header Construction ([wyoming.c:230-247](app/wyoming.c#L230-L247))
Converts Wyoming's binary PCM chunks into playable WAV file with correct headers.

#### 4. Playback Buffer Fix ([main.c:182-186](app/main.c#L182-L186))
Critical fix: Added `write_pos = num_samples` so playback completion check works correctly.

### Build Configuration
- [app/Makefile](app/Makefile) - Added wyoming.c to build (line 2)
- [app/manifest.json](app/manifest.json) - Changed runMode to "respawn" for auto-start

## Testing Instructions

### 1. Test Wyoming TTS (Recommended) 🆕
```bash
# Test synthesis and playback
curl -X POST -H 'Content-Type: application/json' \
  --digest -u nodered:rednode \
  http://speaker.internal/local/voice/speak \
  -d '{"text":"Hej från röstassistenten"}'

# Monitor status
curl --digest -u nodered:rednode http://speaker.internal/local/voice/status

# Check/update settings
curl --digest -u nodered:rednode http://speaker.internal/local/voice/settings
```
**Expected:** Audio plays through speaker within 1-2 seconds

### 2. Test WAV Playback (From URL)
```bash
# Trigger playback on speaker
curl -X POST http://speaker.internal/local/voice/playback \
  -d "http://10.13.8.183:5001/test.wav"

# Monitor status
watch curl -s http://speaker.internal/local/voice/status
```

### 3. Test Network Connectivity
```bash
curl http://speaker.internal/local/voice/test_download
```
**Expected:** `SUCCESS: Downloaded N bytes from http://httpbin.org/get (HTTP 200)`

### 4. Expected Log Output (Success)
```
Wyoming TTS: connected to 10.13.8.2:10200
Wyoming TTS: sending synthesize request
Wyoming: Received audio-start
Wyoming: Received audio-chunk, payload 2048 bytes
Wyoming: Binary mode - received 2048 bytes of PCM data
Wyoming: Received audio-stop
Wyoming: Synthesis complete, 63744 total bytes
WAV: 22050 Hz, 1 channels, 16 bits, format=1
Converting 31872 samples from PCM16 to F32...
WAV loaded: 31872 samples (1.45 seconds at 22050 Hz)
Playback completed (31872 samples)
```

---

## Future Roadmap

### ✅ Steps 1-4: COMPLETED
- ✅ Step 1: WAV playback from URL
- ✅ Step 2-4: Wyoming protocol TCP client
- ✅ TTS synthesis (Piper)
- ✅ Swedish language support
- ✅ Automatic playback after synthesis

### Step 5: Continuous Input Stream (NEXT)
- Start input stream on initialization
- Listen for wake-word detection
- Process audio in `audio_input_callback()`
- Integrate wake-word library (Porcupine, Snowboy, etc.)

### Step 6: VAD-Triggered Recording
- Detect wake-word
- Start second capture stream for speech recording
- Run Voice Activity Detection
- End recording on silence detection
- Buffer audio for speech recognition

### Step 7: Speech Recognition (Wyoming Whisper)
- Send recorded audio to Wyoming-whisper server (10.13.8.2:10300)
- Receive transcription/intent
- Process user commands

### Step 8: Full Voice Assistant Loop
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

---

## Key Implementation Details

### Threading Model
```
Main Thread:
  ├─ GLib Main Loop (g_main_loop_run)
  ├─ PipeWire event loop integration (via GSource)
  ├─ Audio callbacks (input/output)
  └─ Idle callbacks (start_playback_idle)

FastCGI Thread (pthread):
  ├─ HTTP request handlers
  ├─ Downloads WAV files (blocking)
  └─ Queues work to main thread via g_idle_add()
```

**Critical Rule:** All PipeWire stream creation MUST happen in main thread!

### Status Groups
```
input.*
  ├─ state: false (not yet used)
  ├─ samples: 0
  └─ status: "Not running"

output.*
  ├─ state: true/false (playing or not)
  ├─ samples: current position
  ├─ size: total samples in buffer
  ├─ status: "Playing audio..." / "Ready"
  └─ error: error messages
```

### Concurrent Streams Design
- Input and output are completely independent
- No mutual exclusion locks between them
- Each has its own AudioStream state
- Foundation for wake-word + playback simultaneously

---

## Known Limitations

1. **WAV Format Only**
   - PCM 16-bit mono only
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

---

## Troubleshooting

### Wyoming Connection Issues
**Symptom:** "Wyoming TTS: connect failed" or timeout errors

**Checklist:**
1. Verify Wyoming server is running: `echo '{"type":"describe"}' | nc 10.13.8.2 10200`
2. Check network routing from speaker to Wyoming server
3. Verify port 10200 (Piper) or 10300 (Whisper) is accessible
4. Check Wyoming server logs for connection attempts
5. Test from dev machine first: `curl speaker.internal/local/voice/test_wyoming?service=piper`

### No Audio Output
**Symptom:** TTS synthesis succeeds but no sound

**Checklist:**
1. Check device speaker volume/mute
2. Verify PipeWire routing with logs (look for AudioDevice0Output0)
3. Check audio format compatibility (should be 22050 Hz, 16-bit, mono)
4. Monitor status endpoint: `curl speaker.internal/local/voice/status`
5. Look for "Playback completed (N samples)" in logs

### Playback Buffer Issues
**Symptom:** "Playback completed (0 samples)" immediately

**Cause:** Missing `write_pos` assignment in playback buffer
**Fix:** Ensure both `size` and `write_pos` are set when loading audio

### Build Errors
**Most Common:**
- Missing PKG_CONFIG_PATH for cross-compilation
- Undefined symbols → missing library in LDLIBS
- Include path issues → check CFLAGS
- New files not in Makefile → update OBJS1 variable

---

## Files Modified

### Wyoming Protocol Implementation (NEW)
- [app/wyoming.c](app/wyoming.c) - Complete Wyoming protocol TCP client (~616 lines)
- [app/wyoming.h](app/wyoming.h) - Public API for Wyoming client (~128 lines)

### Core Application
- [app/main.c](app/main.c) - Voice assistant with Wyoming TTS integration
  - Added Wyoming audio callback handler
  - Fixed playback buffer (write_pos assignment)
  - Fixed WAV parsing (PCM data offset)
  - Added `/local/voice/speak` endpoint
  - Added `/local/voice/settings` endpoint

### Build and Configuration
- [app/Makefile](app/Makefile) - Added wyoming.c to OBJS1
- [app/manifest.json](app/manifest.json) - Changed runMode to "respawn"
  - Added `/speak` endpoint (viewer access)
  - Added `/settings` endpoint (admin access)
  - Added `/test_wyoming` endpoint (viewer access)

### Documentation
- [ARCHITECTURE.md](ARCHITECTURE.md) - Updated with Wyoming TTS details
- [work-in-progress.md](work-in-progress.md) - Updated to reflect Steps 1-4 complete

### PipeWire Integration (Unchanged)
- [app/pipewire_audio.c](app/pipewire_audio.c) - GSource cleanup fix
- [app/pipewire_audio.h](app/pipewire_audio.h) - `pw_audio_init()` added

---

## Success Criteria

### Steps 1-4 (Completed) ✅
- [x] Architecture redesigned for voice assistant
- [x] Separate input/output stream management
- [x] HTTP POST /playback endpoint implemented
- [x] WAV download via curl
- [x] WAV parsing and validation
- [x] PCM16 to F32 conversion
- [x] Playback buffer management
- [x] Status reporting (input.*, output.*)
- [x] Network connectivity verified and working
- [x] WAV playback tested and working
- [x] Audio plays through speaker
- [x] Wyoming protocol TCP client implemented
- [x] Non-blocking socket with poll() for write-readiness
- [x] Brace-counting JSON parser for complex protocol
- [x] State machine for JSON ↔ binary mode switching
- [x] Real-time WAV construction from PCM chunks
- [x] HTTP POST /speak endpoint for TTS
- [x] Swedish TTS synthesis working
- [x] Configuration endpoint (/settings)
- [x] Multiple successful playback tests (63744, 36864, 56064 samples)

### Future Steps
- [ ] Step 5: Continuous input stream (wake-word detection)
- [ ] Step 6: VAD-triggered recording
- [ ] Step 7: Speech recognition (Wyoming Whisper)
- [ ] Step 8: Full voice assistant loop

---

**Last Updated:** 2025-12-10
**Current Status:** Steps 1-4 Complete - Wyoming TTS Fully Working! 🎉
**Next Step:** Implement continuous input stream for wake-word detection (Step 5)

---

## Reference Documentation

- SSW Audio Reference: `/home/fred/ACAP/ssw-audio/`
- PipeWire Docs: https://docs.pipewire.org/
- GLib Main Loop: https://docs.gtk.org/glib/main-loop.html
- ACAP SDK: Axis Camera Application Platform documentation
- Wyoming Protocol: https://github.com/rhasspy/wyoming

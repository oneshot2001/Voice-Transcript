# Voice Assistant ACAP - Work in Progress

## Project Overview
Building a voice assistant application on Axis network speaker (Freescale i.MX6 Ultralite ARM platform) using PipeWire for audio I/O.

**Reference Implementation:** `/home/fred/ACAP/ssw-audio` - Working SSW Audio library
**Current Project:** `/home/fred/ACAP/test_piper` - Voice Assistant ACAP

---

## Current Status: 🔨 STEP 1 - WAV PLAYBACK FROM URL

### Architecture Redesign Complete ✅
- **Previous:** Simple record/playback test application
- **Now:** Full voice assistant architecture with concurrent streams
- Separated input (future: wake-word) and output (playback) streams
- Independent state management for each stream
- No mutual exclusion between record/playback

### Step 1: WAV Playback Implementation ✅
**Goal:** HTTP POST endpoint that downloads and plays WAV files from URL

**Completed Features:**
- ✅ HTTP POST `/local/base/playback` endpoint
- ✅ WAV file download via libcurl
- ✅ WAV header parsing and validation
- ✅ PCM16 to F32 sample conversion
- ✅ Playback buffer management
- ✅ PipeWire output stream integration
- ✅ Status reporting (input.*, output.*)
- ✅ Diagnostic endpoint for network testing

**WAV Format Supported:**
- Sample Rate: 16000 Hz (or any rate, PipeWire will resample)
- Format: PCM 16-bit
- Channels: Mono (1 channel)
- Container: WAV (RIFF)

### Current Issue: Network Connectivity 🔍
**Problem:** Camera cannot download WAV files from `http://10.13.8.183:5001`

**Error:** `Timeout was reached (error code 28)`

**Diagnosis Steps Taken:**
1. ✅ Verified WAV file is accessible from development machine
2. ✅ Added verbose curl logging (`CURLOPT_VERBOSE`)
3. ✅ Increased timeouts (60s download, 10s connect)
4. ✅ Added detailed error reporting
5. ✅ Created test endpoint to verify general network connectivity

**Next Debugging Step:**
Test if camera can reach public internet via `/local/base/test_download`
- If successful → routing/firewall issue between camera and 10.13.8.183
- If fails → camera has no outbound network access

---

## Code Changes Summary

### New State Management ([main.c:24-59](app/main.c#L24-L59))
```c
typedef struct {
    PWAudio *stream;
    gboolean active;
    guint32 sample_count;
} AudioStream;

typedef struct {
    float *samples;
    guint32 size;
    guint32 write_pos;
    guint32 read_pos;
    gboolean ready;
} PlaybackBuffer;

typedef struct {
    AudioStream input;         // Future: wake-word detection
    AudioStream output;        // TTS playback
    PlaybackBuffer playback;
    gchar *download_url;
    gboolean downloading;
} VoiceAssistantState;
```

### WAV Download ([main.c:240-363](app/main.c#L240-L363))
- Memory-based download (no temp files)
- WAV header validation
- PCM16 → F32 conversion
- Buffer allocation and loading

### HTTP Endpoints
1. **POST /local/base/playback** ([main.c:456-513](app/main.c#L456-L513))
   - Accepts WAV URL in POST body
   - Downloads and converts WAV
   - Schedules playback via `g_idle_add()`
   - Returns 200/400/409/500 status codes

2. **GET /local/base/test_download** ([main.c:404-454](app/main.c#L404-L454))
   - Tests connectivity to httpbin.org
   - Diagnostic tool for network issues
   - Returns detailed error information

3. **GET /local/base/status**
   - Reports input.* and output.* status groups
   - Shows samples played, buffer size, state

### Audio Output Callback ([main.c:156-233](app/main.c#L156-L233))
- Copies samples from playback buffer → PipeWire
- Updates status in real-time
- Auto-stops when playback complete
- Fills silence when not playing

---

## Compilation Fixes Applied

### Fix 1: curl_write_callback Name Conflict
**Problem:** `curl_write_callback` is a typedef in curl.h
```
main.c:116:1: error: 'curl_write_callback' redeclared as different kind of symbol
```
**Solution:** Renamed to `wav_download_write_callback`

### Fix 2: ACAP_HTTP_Get_Body() Does Not Exist
**Problem:** Tried to use non-existent API function
```
main.c:398:24: warning: implicit declaration of function 'ACAP_HTTP_Get_Body'
```
**Solution:** Use `request->postData` directly:
```c
if (!request->postData || request->postDataLength == 0) {
    ACAP_HTTP_Respond_Error(response, 400, "Missing URL in request body");
    return;
}
const char* body = request->postData;
```

---

## Testing Instructions

### 1. Test Network Connectivity
```bash
curl http://<camera-ip>/local/base/test_download
```
**Expected:** `SUCCESS: Downloaded N bytes from http://httpbin.org/get (HTTP 200)`

### 2. Test WAV Playback
```bash
# Ensure WAV file is accessible
curl http://10.13.8.183:5001/tts_20251208_230555_671706.wav -o /tmp/test.wav
file /tmp/test.wav
# Should show: RIFF (little-endian) data, WAVE audio, Microsoft PCM, 16 bit, mono 16000 Hz

# Trigger playback on camera
curl -X POST http://<camera-ip>/local/base/playback \
  -d "http://10.13.8.183:5001/tts_20251208_230555_671706.wav"

# Monitor status
watch curl -s http://<camera-ip>/local/base/status
```

### 3. Expected Log Output (Success)
```
Downloading WAV from: http://10.13.8.183:5001/tts_20251208_230555_671706.wav
Downloaded 81406 bytes
WAV: 16000 Hz, 1 channels, 16 bits, format=1
Converting 40659 samples from PCM16 to F32...
WAV loaded: 40659 samples (2.54 seconds at 16000 Hz)
Scheduled playback idle callback, id=6
start_playback_idle: Starting audio playback from main thread
audio_stream_start: Starting PLAYBACK stream
Connected to PipeWire core
pw_registry_event_global: Found Node: media_class=Audio/Sink, name=AudioDevice0Output0
Audio stream to node AudioDevice0Output0, 1 channel(s)
Stream state changed paused -> streaming
Playback completed (40659 samples)
```

---

## Future Roadmap

### Step 2: Continuous Input Stream (NOT STARTED)
- Start input stream on initialization
- Listen for wake-word detection
- Process audio in `audio_input_callback()`

### Step 3: VAD-Triggered Recording (NOT STARTED)
- Detect wake-word
- Start second capture stream for speech recording
- Run Voice Activity Detection
- End recording on silence detection

### Step 4: Wyoming Protocol Integration (NOT STARTED)
- Send recorded audio to Wyoming-whisper server
- Receive transcription/intent
- Generate TTS response via Wyoming-piper
- Download and play response WAV (using existing Step 1 code!)

### Step 5: Full Voice Assistant Loop (NOT STARTED)
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
Generate TTS (Wyoming-piper)
    ↓
Download WAV URL
    ↓
Playback response ← CURRENT STEP
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

### Network Issues
**Symptom:** "Timeout was reached" downloading WAV

**Checklist:**
1. ✅ Verify WAV is accessible from dev machine
2. ⏳ Test camera internet access via `/local/base/test_download`
3. Check camera firewall rules
4. Check network routing (camera and server on same subnet?)
5. Try Python SimpleHTTPServer on different port
6. Check server firewall (allow camera IP?)

### No Audio Output
**Symptom:** Playback stream connects but no sound

**Checklist:**
1. Check device speaker volume/mute
2. Verify PipeWire routing with `pw-dump` (if available)
3. Look for AudioDevice0Output0 node in logs
4. Check audio format compatibility

### Build Errors
**Most Common:**
- Missing PKG_CONFIG_PATH for cross-compilation
- Undefined symbols → missing library in LDLIBS
- Include path issues → check CFLAGS

---

## Files Modified

### Core Application
- [app/main.c](app/main.c) - Complete redesign for voice assistant
- [ARCHITECTURE.md](ARCHITECTURE.md) - NEW: System architecture documentation

### PipeWire Integration (Unchanged from previous fixes)
- [app/pipewire_audio.c](app/pipewire_audio.c) - GSource cleanup fix
- [app/pipewire_audio.h](app/pipewire_audio.h) - `pw_audio_init()` added

### Build
- [app/Makefile](app/Makefile) - No changes needed

---

## Success Criteria

### Step 1 (Current)
- [x] Architecture redesigned for voice assistant
- [x] Separate input/output stream management
- [x] HTTP POST /playback endpoint implemented
- [x] WAV download via curl
- [x] WAV parsing and validation
- [x] PCM16 to F32 conversion
- [x] Playback buffer management
- [x] Status reporting (input.*, output.*)
- [ ] **Network connectivity resolved** ← BLOCKING
- [ ] WAV playback tested and working
- [ ] Audio plays through speaker

### Future Steps (Not Started)
- [ ] Step 2: Continuous input stream
- [ ] Step 3: VAD-triggered recording
- [ ] Step 4: Wyoming protocol integration
- [ ] Step 5: Full voice assistant loop

---

**Last Updated:** 2025-12-09
**Current Task:** Debug network connectivity issue
**Next Action:** Test `/local/base/test_download` endpoint to diagnose network access

---

## Reference Documentation

- SSW Audio Reference: `/home/fred/ACAP/ssw-audio/`
- PipeWire Docs: https://docs.pipewire.org/
- GLib Main Loop: https://docs.gtk.org/glib/main-loop.html
- ACAP SDK: Axis Camera Application Platform documentation
- Wyoming Protocol: https://github.com/rhasspy/wyoming

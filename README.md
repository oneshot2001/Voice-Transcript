# Voice Assistant ACAP

A voice assistant application for Axis network speaker devices, featuring Wyoming protocol integration for Text-to-Speech (TTS) and Speech-to-text (STT).

## Overview

This ACAP (Axis Camera Application Platform) application transforms Axis network speakers into intelligent voice assistants. Built on PipeWire for audio I/O, the application provides direct TCP integration with Wyoming protocol servers (Piper for TTS, Whisper for ASR) and supports concurrent audio streams for simultaneous listening and speaking.

**Platform:** Freescale i.MX6 Ultralite ARM (armv7hf)
**Target Device:** Axis Network Device with audio input and output.

## 🚀 Key Features

### Current Implementation (v0.9.0)

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
- `/local/voice/listen_start` - Start STT listening
- `/local/voice/listen_stop` - Stop STT listening and get transcription
- `/local/voice/transcription` - Get last transcription
- `/local/voice/status` - Stream status monitoring
- `/local/voice/settings` - Wyoming server configuration

✅ **MQTT Topics** (auto-configured with device serial)
- Subscribe: `voice/speak/{SERIAL}`, `voice/playback/{SERIAL}`, `voice/listen/start/{SERIAL}`, `voice/listen/stop/{SERIAL}`
- Publish: `voice/connect/{SERIAL}`, `voice/transcript/{SERIAL}`

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
curl -X POST -H 'Content-Type: application/json' \
  --digest -u nodered:rednode \
  http://speaker.internal/local/voice/playback \
  -d '{"url":"http://10.13.8.183:5001/test.wav"}'
```

### Speech-to-Text (STT)

Start listening:
```bash
curl -X POST --digest -u nodered:rednode \
  http://speaker.internal/local/voice/listen_start
```

Stop listening and get transcription:
```bash
curl -X POST --digest -u nodered:rednode \
  http://speaker.internal/local/voice/listen_stop
```

Get last transcription:
```bash
curl --digest -u nodered:rednode \
  http://speaker.internal/local/voice/transcription
```

Response:
```json
{
  "text": " Detta är en transkription"
}
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


### Wyoming Server Setup

This application requires Wyoming protocol servers running on your network. You can run them directly with Python or use Docker containers.

#### Installation

**Install Wyoming Piper (TTS):**
```bash
pip install wyoming-piper
```

**Install Wyoming Faster-Whisper (STT):**
```bash
pip install wyoming-faster-whisper
```

#### Running Piper TTS Server

**Swedish Voice:**
```bash
python -m wyoming_piper \
  --uri tcp://0.0.0.0:10200 \
  --data-dir ~/assistant/wyoming/piper \
  --voice sv_SE-nst-medium
```

**English Voice:**
```bash
python -m wyoming_piper \
  --uri tcp://0.0.0.0:10200 \
  --data-dir ~/assistant/wyoming/piper \
  --voice en_US-lessac-medium
```

Available voices: https://github.com/rhasspy/piper/blob/master/VOICES.md

#### Running Faster-Whisper STT Server

**Swedish (with CUDA acceleration):**
```bash
python -m wyoming_faster_whisper \
  --uri tcp://0.0.0.0:10300 \
  --data-dir ~/assistant/wyoming/whisper \
  --model large-v3 \
  --language sv \
  --device cuda \
  --compute-type float16 \
  --beam-size 1
```

**English (with CUDA acceleration):**
```bash
python -m wyoming_faster_whisper \
  --uri tcp://0.0.0.0:10300 \
  --data-dir ~/assistant/wyoming/whisper \
  --model large-v3 \
  --language en \
  --device cuda \
  --compute-type float16 \
  --beam-size 1
```

**CPU-only (no CUDA):**
```bash
python -m wyoming_faster_whisper \
  --uri tcp://0.0.0.0:10300 \
  --data-dir ~/assistant/wyoming/whisper \
  --model base \
  --language sv \
  --device cpu
```

**Parameters:**
- `--model`: `tiny`, `base`, `small`, `medium`, `large-v3` (larger = more accurate, slower)
- `--device`: `cuda` (GPU) or `cpu`
- `--compute-type`: `float16` (faster), `int8` (fastest), `float32` (most accurate)
- `--beam-size`: Lower = faster, Higher = more accurate (1-5 recommended)


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



## 📝 Version History

### 0.9.0 - December 11, 2025
- Initial commit

## 👤 Author

**Fred Juhlin**
Website: https://pandosme.github.io


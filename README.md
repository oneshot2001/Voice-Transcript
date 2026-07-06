# Voice Assistant ACAP

A voice assistant application for Axis network speaker devices, featuring Wyoming protocol integration for Text-to-Speech (TTS) and Speech-to-text (STT).

## Overview

This ACAP (Axis Camera Application Platform) application transforms Axis network speakers into intelligent voice assistants. Built on PipeWire for audio I/O, the application provides direct TCP integration with Wyoming protocol servers (Piper for TTS, Whisper for ASR) and supports concurrent audio streams for simultaneous listening and speaking.

## 🚀 Key Features

✅ **Wyoming Protocol TTS Integration**
- Direct TCP connection to Wyoming Piper server (no HTTP intermediary)
- Complex JSON + binary protocol parsing with state machine
- Real-time WAV file construction from PCM audio chunks
- Multi-language TTS synthesis (English by default)
- Configurable server settings via HTTP API and web interface

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



## 📖 Usage

### Initial Configuration

**IMPORTANT:** Before using voice features, you must configure the Wyoming server address through the web interface:

1. Open the web interface: `http://speaker.internal/local/voice/`
2. Navigate to "Wyoming Protocol Configuration"
3. Enter your Wyoming server IP address (e.g., `10.13.8.2`)
4. Set Piper port (default: `10200`) and Whisper port (default: `10300`)
5. Select language (default: `en`)
6. Click "Save Wyoming Configuration"

Default settings in `app/settings/settings.json`:
```json
{
    "wyoming": "",
    "piper": 10200,
    "whisper": 10300,
    "language": "en"
}
```

Supported languages: `en` (English), `sv` (Swedish), `no` (Norwegian), `da` (Danish), `de` (German), `fr` (French), `es` (Spanish)

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

**English Voice (recommended for default):**
```bash
python -m wyoming_piper \
  --uri tcp://0.0.0.0:10200 \
  --data-dir ~/assistant/wyoming/piper \
  --voice en_US-lessac-medium
```

**Swedish Voice:**
```bash
python -m wyoming_piper \
  --uri tcp://0.0.0.0:10200 \
  --data-dir ~/assistant/wyoming/piper \
  --voice sv_SE-nst-medium
```

Available voices: https://github.com/rhasspy/piper/blob/master/VOICES.md
**Note:** Match the voice language with the language setting in your ACAP configuration.

#### Running Faster-Whisper STT Server

**English (recommended for default, with CUDA acceleration):**
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

**CPU-only (no CUDA):**
```bash
python -m wyoming_faster_whisper \
  --uri tcp://0.0.0.0:10300 \
  --data-dir ~/assistant/wyoming/whisper \
  --model base \
  --language en \
  --device cpu
```

**Parameters:**
- `--model`: `tiny`, `base`, `small`, `medium`, `large-v3` (larger = more accurate, slower)
- `--device`: `cuda` (GPU) or `cpu`
- `--compute-type`: `float16` (faster), `int8` (fastest), `float32` (most accurate)
- `--beam-size`: Lower = faster, Higher = more accurate (1-5 recommended)

## 📝 Version History

### 0.9.5 - January 4, 2026
- Added LED visual feedback for voice assistant states
- LED indicates idle (blue), listening (yellow), processing, and speaking states
- Integrated with Axis device siren_and_light.cgi VAPIX API

### 0.9.0 - December 11, 2025
- Initial commit

## 👤 Author

**Fred Juhlin**
Website: https://pandosme.github.io


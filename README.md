# Voice Transcripts ACAP

Voice Transcripts is an Axis ACAP for continuous speech transcription on device microphones.

It supports two user-selectable operating modes:

- Continuous transcription: quality-focused, longer utterances, sentence/pause splitting.
- Voice commands: speed-focused, keyword-gated, fuzzy keyword matching.

## What It Does

- Captures audio from the processed Axis input path (`AudioDevice0Input0`).
- Runs VAD + utterance segmentation continuously.
- Uses either:
  - internal `whisper.cpp` inference on device, or
  - external Wyoming Whisper server.
- Stores transcript history for the web UI/API.
- Publishes transcript events over MQTT.

## Quick Start

### 1. Build

```bash
./build.sh
```

Produces an aarch64 `.eap` package (includes local whisper.cpp + bundled models).

For armv7hf external-only builds:

```bash
./build-armv7hf.sh
```

Equivalent:

```bash
./build.sh armv7hf
```

This variant excludes local whisper/model assets and requires `stt_mode=external`.

### 2. Install

```bash
./install.sh <device-host> <user> <password> aarch64
```

For armv7hf packages:

```bash
./install.sh <device-host> <user> <password> armv7hf
```

### 3. Configure

Open:

```text
http://<device-host>/local/voice/
```

Then set:

- STT backend (`internal` or `external`)
- Language
- Sensitivity / silence timeout / min speech / max utterance
- Use case (`continuous` or `voice_commands`)
- Command keywords (for voice command mode)
- MQTT broker settings

## Use Cases

### Continuous transcription

Optimized for transcript quality and long-form capture.

Behavior:

- Splits transcript output on sentence/pause boundaries when possible.
- Ignores transcript fragments shorter than 3 words.
- Publishes accepted transcripts to MQTT topic `voice/transcription`.

### Voice commands

Optimized for speed and command intent extraction.

Behavior:

- Expects short utterances (typically 3-10 words).
- Ignores utterances shorter than the configured minimum command words (1-5, default 3).
- Requires at least one keyword fuzzy match.
- If no keyword match:
  - transcript is still shown in UI history,
  - marked as `Ignored`,
  - not published to MQTT.
- Publishes accepted command transcripts to MQTT topic `voice/command`.

Fuzzy keyword matching handles:

- exact matches,
- common singular/plural forms,
- minor edit-distance transcription errors.

## HTTP API

All endpoints are under:

```text
/local/voice/
```

### GET `/app`
Returns app metadata, runtime status snapshot, and settings.

### GET `/status`
Returns live runtime status keys used by UI.

### GET `/settings`
Returns current settings JSON.

### POST `/settings`
Applies settings immediately.

Example payload:

```json
{
  "transcription_use_case": "voice_commands",
  "command_keywords": "light,lights,garage,gate",
  "command_min_words": 3,
  "stt_mode": "external",
  "wyoming_host": "bart.internal",
  "wyoming_whisper_port": 10300,
  "enabled": true,
  "input_node": "AudioDevice0Input0",
  "language": "sv",
  "translate": false,
  "sensitivity": "normal",
  "silence_timeout_ms": 450,
  "min_speech_ms": 180,
  "max_utterance_sec": 6,
  "inference_threads": 3,
  "max_tokens": 32
}
```

### GET `/transcripts`
Returns transcript history (newest first).

Example entry:

```json
{
  "ts": 1783527405123,
  "utterance_start_ms": 1783527401044,
  "duration_ms": 4079,
  "ignored": false,
  "language": "sv",
  "text": "turn on the garage lights",
  "keywords": ["garage", "lights"]
}
```

### DELETE `/transcripts`
Clears transcript history and counters.

### GET/POST `/mqtt`
Gets or updates MQTT connectivity and payload metadata settings.

## MQTT Integration

`MQTT_Publish_JSON` automatically:

- prepends configured `preTopic` if set,
- injects `serial`,
- injects optional `name` and `location` from MQTT settings.

### Published topics

- Continuous mode: `voice/transcription`
- Voice command mode: `voice/command`

Final broker topic becomes:

- `<preTopic>/voice/transcription` or
- `<preTopic>/voice/command`

when `preTopic` is configured.

### Transcript payload example

```json
{
  "language": "sv",
  "text": "turn on the garage lights",
  "timestamp": 1783527401044,
  "keywords": ["garage", "lights"],
  "serial": "B8A44F3024BB",
  "name": "Lobby Hub",
  "location": "Building A Floor 1"
}
```

Field notes:

- `timestamp` is epoch milliseconds when utterance start was detected.
- `keywords` contains detected keyword matches. It can be empty in continuous mode.

## External Wyoming Backend

If `stt_mode=external`, configure:

- `wyoming_host`
- `wyoming_whisper_port`

### Install and run Wyoming Whisper (base model)

Install on the external server:

```bash
python3 -m pip install -U wyoming-faster-whisper
```

Run a common base-model service (CPU-friendly default):

```bash
python3 -m wyoming_faster_whisper \
  --uri tcp://0.0.0.0:10300 \
  --data-dir ~/assistant/whisper \
  --model base \
  --language en \
  --device cpu \
  --compute-type int8 \
  --beam-size 1
```

Typical alternatives:

- Use `--language sv` (or other language code) to lock transcription language.
- Use `--language` omitted for auto language handling on server side.
- Use `--device cuda --compute-type float16` for higher throughput on NVIDIA GPU hosts.

Then set in ACAP configuration:

- `stt_mode = external`
- `wyoming_host = <server-hostname-or-ip>`
- `wyoming_whisper_port = 10300`

More detailed Wyoming Whisper options and deployment guidance:

- https://github.com/rhasspy/wyoming-faster-whisper

Recommended service package versions:

- `wyoming-faster-whisper>=3.3.1`
- `wyoming-piper>=2.2.2` (if TTS services are also running on same host)

## armv7hf External-Only Notes

- armv7hf build is compiled as external-only (`stt_mode` forced to `external`).
- Local whisper inference is not available in this package.
- Whisper models are not bundled into the armv7hf `.eap`.
- `wyoming_host` and `wyoming_whisper_port` must be configured for transcription.

## Internal vs External Whisper

### Internal Whisper (`stt_mode=internal`)

Availability:

- Supported by the default aarch64 build.
- Not available in armv7hf external-only build.

Pros:

- No additional servers or network dependencies.
- Simplest deployment and maintenance.
- Better resilience in isolated/offline environments.

Cons:

- Limited by camera/hub compute resources.
- Lower maximum throughput for concurrent or heavy workloads.
- Less flexibility for large model variants.

### External Whisper (`stt_mode=external`)

Pros:

- Higher performance on dedicated CPU/GPU servers.
- Better for high-rate or multi-device transcription workloads.
- Easier to scale model/runtime independently of ACAP package.

Cons:

- Requires additional server setup/operations.
- Depends on network stability and server availability.
- Adds infrastructure complexity (service supervision, updates, monitoring).

## Continuous Listening Toggle

`enabled` ("Continuous listening enabled" in UI):

- `true`: continuously monitors microphone and processes utterances.
- `false`: keeps app running but drops utterances (no transcription output).

## Architecture, Build, and Extension Guide

For implementation details, architecture, extension points, and contributor workflow, see:

- [AGENT.md](AGENT.md)

## Support and Contributions

- Buy me a coffee: https://www.buymeacoffee.com/fredjuhlinl
- Pull requests are highly appreciated.

## License

BSD 3-Clause with third-party notices.

- [LICENSE](LICENSE)

## History

### 1.0.1 Jul 9, 2026

**Initial commit**

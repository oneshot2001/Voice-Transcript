# AGENT Guide - Voice Transcripts ACAP

This file is for engineers and contributors extending or modifying this package.

## Scope and Intent

Voice Transcripts ACAP is focused on speech transcription workflows for Axis devices.

Primary goals:

- robust continuous microphone transcription,
- practical command detection mode,
- clear integration outputs through HTTP + MQTT.

Avoid adding unrelated product surfaces unless explicitly requested.

## ACAP SDK Reference

For general ACAP SDK architecture and base project patterns, use the base ACAP repository:

- https://github.com/pandosme/make_acap

See the `doc/` subdirectory there for SDK architecture and foundational ACAP guidance.

## Current Architecture

## Runtime pipeline

1. PipeWire capture (`app/pipewire_audio.c`) reads microphone samples.
2. `app/main.c` forwards raw mono F32 data to `whisper_stt_feed(...)`.
3. `app/whisper_stt.c` worker thread performs:
   - ring buffer consumption,
   - VAD (adaptive noise floor + sensitivity gates),
   - utterance segmentation,
   - internal whisper inference or external callback dispatch.
4. `app/main.c` handles transcript policy:
   - use-case selection (`continuous` vs `voice_commands`),
   - min-word filtering,
   - fuzzy keyword checks,
   - transcript history storage,
   - MQTT publish routing.

## Use-case policy layer (in `main.c`)

- `continuous`
  - quality-biased defaults,
  - sentence/pause splitting,
  - drops transcript fragments < 3 words,
  - MQTT topic: `voice/transcription`.

- `voice_commands`
  - speed-biased defaults,
  - drops transcript fragments < 3 words,
  - keyword fuzzy match required for publishing,
  - unmatched transcripts kept in history with `ignored=true`,
  - MQTT topic: `voice/command`.

## Files You Will Touch Most

- `app/main.c`
  - settings parsing/application,
  - transcript filtering policy,
  - MQTT publish payload/topic logic,
  - transcript API (`/transcripts`).

- `app/whisper_stt.c`, `app/whisper_stt.h`
  - VAD and segmentation internals,
  - callback contracts,
  - utterance timing propagation.

- `app/pipewire_audio.c`, `app/pipewire_audio.h`
  - capture node selection and PipeWire stream behavior.

- `app/html/config.html`
  - user settings UI.

- `app/html/index.html`
  - transcript list and status display.

- `app/html/about.html`
  - public API/MQTT docs shown on device UI.

- `app/settings/settings.json`
  - packaged defaults.

- `app/manifest.json`
  - ACAP metadata, version, endpoint exposure.

## HTTP Surface

Root path:

- `/local/voice/`

Endpoints:

- `app` (GET)
- `status` (GET)
- `settings` (GET/POST)
- `transcripts` (GET/DELETE)
- `mqtt` (GET/POST)

`transcripts` entries include:

- `ts`
- `utterance_start_ms`
- `duration_ms`
- `ignored`
- `language`
- `text`
- `keywords`

## MQTT Contract

Publish logic is centralized in `publish_transcript_entry(...)` in `app/main.c`.

Topics (before optional `preTopic` prefix):

- `voice/transcription` (continuous mode)
- `voice/command` (voice command mode)

Payload fields written by transcription code:

- `language`
- `text`
- `timestamp` (utterance start, epoch ms)
- `keywords` (detected keyword list)

Fields auto-injected by MQTT helper (`app/MQTT.c`):

- `serial`
- `name` (if configured)
- `location` (if configured)

Important: ignored command transcripts are intentionally not published.

## Settings Model

Core settings that materially affect behavior:

- `transcription_use_case`: `continuous` | `voice_commands`
- `command_keywords`: comma-separated string
- `stt_mode`: `internal` | `external`
- `wyoming_host`, `wyoming_whisper_port`
- `enabled`
- `language`, `translate`
- `sensitivity`
- `silence_timeout_ms`, `min_speech_ms`, `max_utterance_sec`
- `inference_threads`, `max_tokens`

Mode-specific guardrails are applied in `apply_settings(...)`.

## Development Workflow

## Build

```bash
./build.sh
```

Default target:

- `aarch64` with local whisper enabled.

armv7hf external-only target:

```bash
./build-armv7hf.sh
```

or:

```bash
./build.sh armv7hf
```

Expected artifact:

- `Voice_Transcripts_1_0_0_aarch64.eap` (or matching versioned name)
- `Voice_Transcripts_1_0_0_armv7hf.eap` for armv7hf build

## Install

```bash
./install.sh <device-host> <user> <password> aarch64
```

For armv7hf package:

```bash
./install.sh <device-host> <user> <password> armv7hf
```

## Verify

- Open UI: `http://<device-host>/local/voice/`
- Check status and transcript list updates.
- Validate MQTT publishes for both use cases.

## Branch and Release Notes

- Current release target: `1.0.0`.
- Keep `README.md`, `about.html`, and runtime behavior aligned.
- If you change MQTT payload/topic semantics, update both docs and UI docs in same commit.

## Extension Guidelines

1. Keep policy decisions in `main.c`; keep low-level audio/STT mechanics in `whisper_stt.c`.
2. Do not silently change MQTT contract fields.
3. Preserve backward-safe status keys where possible.
4. When adding UI settings, also:
   - parse/apply in backend,
   - persist defaults in `app/settings/settings.json`,
   - document in README/about page.
5. Prefer small testable steps with build validation after core C changes.

6. Respect compile-time build mode:
  - `EXTERNAL_ONLY_BUILD` / `NO_LOCAL_WHISPER` means local whisper must not be assumed,
  - backend is forced to external in this mode.

## Troubleshooting Pointers

- No transcripts:
  - verify `input.bound_node` and sample flow in status,
  - verify `enabled=true`,
  - check VAD thresholds and language settings,
  - for external mode, verify Wyoming connectivity and port.

- Command transcripts not publishing:
  - confirm use case is `voice_commands`,
  - check keyword list normalization and fuzzy matching,
  - inspect transcript entries for `ignored=true`.

- MQTT integration confusion:
  - remember `preTopic` is prepended by helper,
  - topic passed in code should remain logical suffix (`voice/...`).

## Community

- Buy me a coffee: https://www.buymeacoffee.com/fredjuhlinl
- Pull requests are highly appreciated.

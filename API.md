# Voice Assistant ACAP - API Documentation

This document describes all HTTP and MQTT APIs for the Voice Assistant ACAP application.

## Table of Contents
- [HTTP API](#http-api)
  - [Text-to-Speech (TTS)](#text-to-speech-tts)
  - [Speech-to-Text (STT)](#speech-to-text-stt)
  - [Audio Playback](#audio-playback)
  - [Status & Configuration](#status--configuration)
- [MQTT API](#mqtt-api)
  - [Subscribe Topics](#subscribe-topics-commands)
  - [Publish Topics](#publish-topics-events)
- [LED State Indicators](#led-state-indicators)

---

## HTTP API

All HTTP endpoints are accessible at `http://<device-ip>/local/voice/<endpoint>`.

### Text-to-Speech (TTS)

#### Speak - Synthesize Text to Speech

**Endpoint:** `POST /local/voice/speak`

**Access:** Viewer

**Description:** Sends text to Wyoming Piper service for TTS synthesis. Audio will be played through the device speaker when ready.

**Request Body (JSON):**
```json
{
  "text": "Hello, this is a test message",
  "voice": "en_US-lessac-medium"
}
```

**Parameters:**
- `text` (string, required): Text to synthesize
- `voice` (string, optional): Voice name for synthesis. If omitted, uses default voice configured for the language.

**Response:**
```
200 OK
Content-Type: text/plain

TTS request accepted, audio will play when ready
```

**LED Indicator:** Blue (idle) → Yellow (processing) → Green (speaking) → Blue (idle)

**Example:**
```bash
curl -X POST http://192.168.1.100/local/voice/speak \
  -H "Content-Type: application/json" \
  -d '{"text": "Welcome to the voice assistant"}'
```

**Error Responses:**
- `400 Bad Request`: Missing or invalid JSON, missing 'text' field
- `503 Service Unavailable`: Failed to connect to TTS service

---

### Speech-to-Text (STT)

#### Start Listening - Begin Audio Recording

**Endpoint:** `POST /local/voice/listen_start`

**Access:** Viewer

**Description:** Starts recording audio from the device microphone for speech-to-text transcription.

**Request Body:** None (POST with empty body)

**Response:**
```
200 OK
Content-Type: text/plain

Listening started
```

**LED Indicator:** Blue (idle) → Yellow (listening)

**Example:**
```bash
curl -X POST http://192.168.1.100/local/voice/listen_start
```

**Error Responses:**
- `400 Bad Request`: Invalid request method
- `409 Conflict`: Already recording

---

#### Stop Listening - End Recording

**Endpoint:** `POST /local/voice/listen_stop`

**Access:** Viewer

**Description:** Stops audio recording and sends the captured audio to Wyoming Whisper service for transcription. The transcription will be published to MQTT and available via the `/transcription` endpoint.

**Request Body:** None (POST with empty body)

**Response:**
```
200 OK
Content-Type: text/plain

Listening stopped, check status for transcription
```

**LED Indicator:** Yellow (listening) → Yellow (processing) → Blue (idle)

**Example:**
```bash
curl -X POST http://192.168.1.100/local/voice/listen_stop
```

**Error Responses:**
- `400 Bad Request`: Not currently recording
- `503 Service Unavailable`: Failed to connect to STT service

---

#### Get Transcription - Retrieve Last Transcript

**Endpoint:** `GET /local/voice/transcription`

**Access:** Viewer

**Description:** Returns the last received transcription from the Whisper STT service.

**Request Body:** None

**Response (JSON):**
```json
{
  "text": "this is the transcribed text"
}
```

**Example:**
```bash
curl http://192.168.1.100/local/voice/transcription
```

**Response Fields:**
- `text` (string): Last transcription text, empty string if no transcription available

---

### Audio Playback

#### Playback - Play WAV from URL

**Endpoint:** `POST /local/voice/playback`

**Access:** Admin

**Description:** Downloads a WAV file from a URL and plays it through the device speaker. Supports any sample rate with automatic resampling.

**Request Body (JSON):**
```json
{
  "url": "http://example.com/audio/notification.wav"
}
```

**Parameters:**
- `url` (string, required): HTTP URL to WAV file

**Response:**
```
200 OK
Content-Type: text/plain

Playback request accepted
```

**LED Indicator:** Blue (idle) → Green (speaking) → Blue (idle)

**Example:**
```bash
curl -X POST http://192.168.1.100/local/voice/playback \
  -H "Content-Type: application/json" \
  -d '{"url": "http://192.168.1.50/sounds/alert.wav"}'
```

**Error Responses:**
- `400 Bad Request`: Missing JSON body or 'url' field
- `409 Conflict`: Already playing or downloading

---

### Status & Configuration

#### Application Status

**Endpoint:** `GET /local/voice/status`

**Access:** Admin

**Description:** Returns detailed status information about all voice assistant subsystems.

**Request Body:** None

**Response (JSON):**
```json
{
  "input": {
    "state": false,
    "samples": 0,
    "status": "Not running"
  },
  "output": {
    "state": true,
    "samples": 45678,
    "size": 48000,
    "status": "Playing audio..."
  },
  "wyoming": {
    "piper": "connected",
    "whisper": "configured",
    "error": null
  },
  "stt": {
    "recording": false,
    "samples": 32000,
    "duration": 2.0,
    "status": "Transcription complete",
    "last_transcript": "hello world"
  }
}
```

**Example:**
```bash
curl http://192.168.1.100/local/voice/status
```

---

#### Application Settings

**Endpoint:** `GET /local/voice/settings`

**Access:** Admin

**Description:** Returns current Wyoming server configuration.

**Request Body:** None

**Response (JSON):**
```json
{
  "wyoming": "10.13.8.2",
  "piper": 10200,
  "whisper": 10300,
  "language": "en"
}
```

---

**Endpoint:** `POST /local/voice/settings`

**Access:** Admin

**Description:** Updates Wyoming server configuration. Changes take effect immediately.

**Request Body (JSON):**
```json
{
  "wyoming": "10.13.8.2",
  "piper": 10200,
  "whisper": 10300,
  "language": "sv"
}
```

**Parameters:**
- `wyoming` (string, required): Wyoming server IP address
- `piper` (integer, required): Piper TTS service port (default: 10200)
- `whisper` (integer, required): Whisper STT service port (default: 10300)
- `language` (string, required): Language code (en, sv, no, da, de, fr, es)

**Response:**
```
200 OK
```

**Example:**
```bash
curl -X POST http://192.168.1.100/local/voice/settings \
  -H "Content-Type: application/json" \
  -d '{
    "wyoming": "10.13.8.2",
    "piper": 10200,
    "whisper": 10300,
    "language": "en"
  }'
```

---

#### Application Info

**Endpoint:** `GET /local/voice/app`

**Access:** Admin

**Description:** Returns complete application metadata including manifest, settings, status, and device information.

**Request Body:** None

**Response (JSON):**
```json
{
  "manifest": {
    "acapPackageConf": {
      "setup": {
        "friendlyName": "Voice",
        "appName": "voice",
        "vendor": "Fred Juhlin",
        "version": "1.0.0"
      }
    }
  },
  "settings": { ... },
  "status": { ... },
  "device": {
    "serial": "ACCC8E123456",
    "model": "AXIS P3245-LVE",
    "firmware": "11.10.67"
  }
}
```

---

## MQTT API

All MQTT topics are automatically configured with the device serial number. Replace `{SERIAL}` with your device's serial number (e.g., `ACCC8E123456`).

### Subscribe Topics (Commands)

The device subscribes to these topics to receive commands:

#### TTS Speak Command

**Topic:** `voice/speak/{SERIAL}`

**Payload (JSON):**
```json
{
  "text": "Hello from MQTT",
  "voice": "en_US-lessac-medium"
}
```

**Parameters:**
- `text` (string, required): Text to synthesize
- `voice` (string, optional): Voice name

**LED Indicator:** Blue → Yellow → Green → Blue

**Example (mosquitto_pub):**
```bash
mosquitto_pub -h mqtt.local -t voice/speak/ACCC8E123456 \
  -m '{"text": "Hello from MQTT"}'
```

---

#### Playback Command

**Topic:** `voice/playback/{SERIAL}`

**Payload (JSON):**
```json
{
  "url": "http://example.com/audio/notification.wav"
}
```

**Parameters:**
- `url` (string, required): HTTP URL to WAV file

**LED Indicator:** Blue → Green → Blue

**Example:**
```bash
mosquitto_pub -h mqtt.local -t voice/playback/ACCC8E123456 \
  -m '{"url": "http://192.168.1.50/alert.wav"}'
```

---

#### Listen Start Command

**Topic:** `voice/listen/start/{SERIAL}`

**Payload:** Any (payload is ignored, presence of message triggers action)

**Example:**
```bash
mosquitto_pub -h mqtt.local -t voice/listen/start/ACCC8E123456 -m "start"
```

**LED Indicator:** Blue → Yellow

---

#### Listen Stop Command

**Topic:** `voice/listen/stop/{SERIAL}`

**Payload:** Any (payload is ignored, presence of message triggers action)

**Example:**
```bash
mosquitto_pub -h mqtt.local -t voice/listen/stop/ACCC8E123456 -m "stop"
```

**LED Indicator:** Yellow → Yellow (processing) → Blue

---

### Publish Topics (Events)

The device publishes to these topics to send events:

#### Connection Status

**Topic:** `voice/connect/{SERIAL}` or `connect/{SERIAL}`

**Payload (JSON):**
```json
{
  "connected": true,
  "address": "192.168.1.100",
  "service": "voice"
}
```

**Published When:**
- Device connects to MQTT broker: `"connected": true`
- Device disconnects from MQTT broker: `"connected": false`

**QoS:** 0
**Retained:** Yes

**Example (subscribe):**
```bash
mosquitto_sub -h mqtt.local -t voice/connect/ACCC8E123456
```

---

#### Transcription Result

**Topic:** `voice/transcript/{SERIAL}`

**Payload (JSON):**
```json
{
  "text": "this is the transcribed speech",
  "timestamp": "2026-01-04T15:30:45Z",
  "device": "ACCC8E123456"
}
```

**Published When:** Whisper STT service completes transcription of recorded audio

**QoS:** 0
**Retained:** No

**Example (subscribe):**
```bash
mosquitto_sub -h mqtt.local -t voice/transcript/ACCC8E123456
```

---

## LED State Indicators

The device LED provides visual feedback for voice assistant states (requires Axis device with siren/light capability):

| LED Color | Profile | State | Description |
|-----------|---------|-------|-------------|
| **Blue** | LiveStream | Idle | Ready for commands, waiting |
| **Yellow** | Listen | Listening | Recording audio for STT |
| **Processing** | Processing | Processing | Waiting for TTS/STT response from Wyoming server |
| **Green** | Complete | Speaking | Playing synthesized speech or audio file |

**State Transitions:**

**TTS Flow:**
```
Blue (idle) → Yellow (processing) → Green (speaking) → Blue (idle)
```

**STT Flow:**
```
Blue (idle) → Yellow (listening) → Yellow (processing) → Blue (idle)
```

**Playback Flow:**
```
Blue (idle) → Green (playing) → Blue (idle)
```

---

## Error Codes

### HTTP Error Codes

| Code | Message | Description |
|------|---------|-------------|
| 400 | Bad Request | Invalid request method, missing parameters, or invalid JSON |
| 409 | Conflict | Operation already in progress (already playing/recording/downloading) |
| 503 | Service Unavailable | Failed to connect to Wyoming service (Piper/Whisper) |

### Wyoming Connection States

Available in `/local/voice/status` under `wyoming.piper` and `wyoming.whisper`:

- `"not configured"`: Wyoming server settings not configured
- `"configured"`: Settings configured but not connected
- `"connecting"`: Attempting to connect to service
- `"connected"`: Successfully connected and ready
- `"disconnected"`: Connection lost
- `"error"`: Connection error (check `wyoming.error` field)

---

## Configuration

### Wyoming Server Setup

Before using the voice assistant, configure the Wyoming server settings via HTTP API or web interface:

```bash
curl -X POST http://192.168.1.100/local/voice/settings \
  -H "Content-Type: application/json" \
  -d '{
    "wyoming": "10.13.8.2",
    "piper": 10200,
    "whisper": 10300,
    "language": "en"
  }'
```

### MQTT Broker Configuration

Configure MQTT broker settings via the device web interface:
1. Navigate to `http://<device-ip>/local/voice/`
2. Go to MQTT Configuration
3. Enter broker address, port, credentials
4. Save configuration

---

## Voice Names

Common voice names for TTS (must match Piper server configuration):

**English:**
- `en_US-lessac-medium` (American, recommended)
- `en_GB-alan-medium` (British)

**Swedish:**
- `sv_SE-nst-medium` (recommended)

**Norwegian:**
- `no_NO-talesyntese-medium`

**Danish:**
- `da_DK-talesyntese-medium`

**German:**
- `de_DE-thorsten-medium`

**French:**
- `fr_FR-siwis-medium`

**Spanish:**
- `es_ES-sharvard-medium`

See [Piper Voices](https://github.com/rhasspy/piper/blob/master/VOICES.md) for complete list.

---

## Examples

### Complete TTS Workflow

```bash
# 1. Configure Wyoming server
curl -X POST http://192.168.1.100/local/voice/settings \
  -H "Content-Type: application/json" \
  -d '{"wyoming": "10.13.8.2", "piper": 10200, "whisper": 10300, "language": "en"}'

# 2. Send TTS request
curl -X POST http://192.168.1.100/local/voice/speak \
  -H "Content-Type: application/json" \
  -d '{"text": "Hello, how are you today?"}'

# 3. Check status
curl http://192.168.1.100/local/voice/status
```

### Complete STT Workflow

```bash
# 1. Start recording
curl -X POST http://192.168.1.100/local/voice/listen_start

# 2. Wait for user to speak (e.g., 5 seconds)
sleep 5

# 3. Stop recording
curl -X POST http://192.168.1.100/local/voice/listen_stop

# 4. Wait for transcription (check status)
sleep 2

# 5. Get transcription
curl http://192.168.1.100/local/voice/transcription
```

### MQTT Integration Example (Python)

```python
import paho.mqtt.client as mqtt
import json

DEVICE_SERIAL = "ACCC8E123456"
MQTT_BROKER = "mqtt.local"

def on_message(client, userdata, msg):
    if msg.topic == f"voice/transcript/{DEVICE_SERIAL}":
        data = json.loads(msg.payload)
        print(f"Transcription: {data['text']}")

client = mqtt.Client()
client.on_message = on_message
client.connect(MQTT_BROKER, 1883, 60)

# Subscribe to transcription results
client.subscribe(f"voice/transcript/{DEVICE_SERIAL}")

# Start listening
client.publish(f"voice/listen/start/{DEVICE_SERIAL}", "start")

# Wait 5 seconds, then stop
import time
time.sleep(5)
client.publish(f"voice/listen/stop/{DEVICE_SERIAL}", "stop")

# Wait for transcription
client.loop_forever()
```

### Home Assistant MQTT Integration

```yaml
# configuration.yaml
mqtt:
  - switch:
      name: "Voice Listen"
      command_topic: "voice/listen/start/ACCC8E123456"
      state_topic: "voice/connect/ACCC8E123456"
      payload_on: "start"
      value_template: "{{ value_json.connected }}"

  - sensor:
      name: "Voice Transcription"
      state_topic: "voice/transcript/ACCC8E123456"
      value_template: "{{ value_json.text }}"

# Script to speak
script:
  voice_speak:
    sequence:
      - service: mqtt.publish
        data:
          topic: "voice/speak/ACCC8E123456"
          payload: '{"text": "{{ text }}"}'
```

---

## Version History

**1.0.0** - Always trigger STT on the WakeWord ACAP's wake word event (ignored while busy); GUI restyled to match WakeWord; LICENSE attributions updated
**0.9.5** - Added LED visual feedback
**0.9.0** - Initial release with Wyoming protocol integration

## Support

For issues and questions:
- GitHub: https://github.com/pandosme/Voice
- Website: https://pandosme.github.io

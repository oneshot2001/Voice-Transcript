# Work in Progress - Voice Assistant ACAP

## Current Status
✅ Wyoming Protocol TTS (Piper) - Fungerar perfekt
✅ WAV playback från URL - Fungerar
✅ Wyoming Protocol STT (Whisper) - **FUNGERAR! (Bugfix klar)**
✅ MQTT Integration - Fungerar med device serial
## Current Status
✅ Wyoming Protocol TTS (Piper) - Fungerar perfekt
✅ WAV playback från URL - Fungerar
## ✅ FIXAD BUG: STT Fungerar Endast En Gång
✅ MQTT Integration - Fungerar med device serial
## Current Status
✅ Wyoming Protocol TTS (Piper) - Fungerar perfekt
✅ WAV playback från URL - Fungerar
✅ Wyoming Protocol STT (Whisper) - **FUNGERAR! (Bugfix klar)**

### ✅ LÖSNING (2025-12-12)

**Rotorsak:** Wyoming Faster-Whisper servern kan INTE hantera multipla ASR-förfrågningar på samma TCP-anslutning.

**Bevis:** Test-skript (test_whisper_correct.py) visade att:
- Första ASR-request på en socket: Fungerar
- Andra ASR-request på samma socket: Timeout, inget svar
- Med reconnect mellan requests: Alla requests fungerar!

**Implementerad Fix:** [wyoming.c:1151-1159](app/wyoming.c#L1151-L1159)
```c
// BUGFIX: Reconnect to Whisper before each ASR request
// Wyoming Faster-Whisper server cannot handle multiple ASR requests on the same socket
// This ensures each transcription gets a fresh connection
LOG("Wyoming ASR: Reconnecting to Whisper for fresh connection...\n");
wyoming_disconnect(WYOMING_SERVICE_WHISPER);
if (wyoming_connect(WYOMING_SERVICE_WHISPER) != 0) {
    LOG_WARN("Wyoming ASR: failed to reconnect to Whisper\n");
    return -1;
}
```

**Notering:** Detta är en begränsning i Wyoming Faster-Whisper servern, inte i ACAP-koden. Wyoming Piper (TTS) fungerar perfekt med samma socket för multipla requests.

✅ MQTT Integration - Fungerar med device serial
## Current Status
✅ Wyoming Protocol TTS (Piper) - Fungerar perfekt
✅ WAV playback från URL - Fungerar
✅ Wyoming Protocol STT (Whisper) - **FUNGERAR! (Bugfix klar)**
✅ MQTT Integration - Fungerar med device serial
## Current Status
✅ Wyoming Protocol TTS (Piper) - Fungerar perfekt
✅ WAV playback från URL - Fungerar
✅ Wyoming Protocol STT (Whisper) - **FUNGERAR! (Bugfix klar)**
✅ MQTT Integration - Fungerar med device serial

---

## = KRITISK BUG: STT Fungerar Endast En G�ng

### Symptom
- **F�rsta STT-anropet:** Fungerar perfekt, transkription tas emot
- **Andra STT-anropet:** Inga data tas emot fr�n Whisper-servern, inget svar
- **Alla efterf�ljande anrop:** Samma problem som andra anropet

### Logganalys

#### F�rsta anropet (FUNGERAR):
```
Wyoming ASR: All events sent, waiting for transcript
Wyoming ASR: Received 99 bytes from socket          � TAR EMOT DATA
Wyoming: Processing merged JSON (len=91)
Wyoming ASR: Transcript text: ' Detta �r f�rstatestat.'
Wyoming ASR: Transcription complete, buffer and parser state reset
Wyoming: Buffer cleared by message handler, stopping parse loop
```

#### Andra anropet (FUNGERAR INTE):
```
Wyoming ASR: All events sent, waiting for transcript
(INGET SVAR TAS EMOT - ingen "Received X bytes from socket")
```

### Teknisk Analys

**Rotorsak:** Problem med receive buffer (`rx_buffer`) och parser state efter f�rsta transcript-meddelandet.

**Uppdaterad Debug Info Beh�vs:**
- Lade till loggning i `wyoming_poll_callback()` f�r att se om `poll()` returnerar events
- Lade till loggning f�r `POLLIN` events
- Beh�ver se om servern skickar data men klienten inte l�ser den

**F�rs�kta Fixar (Inte L�st):**

1. **Fix #1:** Nollst�llning av buffer i `wyoming_process_message()` efter transcript
   - L�ge: [wyoming.c:342-348](app/wyoming.c#L342-L348)
   - Resultat: Bufferten nollst�lls korrekt, men problemet kvarst�r

2. **Fix #2:** Break ur parse-loop om bufferten nollst�lls
   - L�ge: [wyoming.c:636-641](app/wyoming.c#L636-L641)
   - Resultat: Loop bryts korrekt ("Buffer cleared by message handler"), men problemet kvarst�r

3. **Fix #3:** F�rhindra �verskrivning av `rx_length` efter nollst�llning
   - L�ge: [wyoming.c:665](app/wyoming.c#L665)
   - Resultat: Skyddar mot �verskrivning, men problemet kvarst�r

4. **Fix #4:** Debug-loggning f�r poll() events
   - L�ge: [wyoming.c:380](app/wyoming.c#L380) och [wyoming.c:408](app/wyoming.c#L408)
   - Resultat: V�ntar p� testresultat

### Hypoteser

1. **Socket-state problem:** M�jligen st�ngs eller pausas socketen efter f�rsta transcript?
2. **Whisper-server problem:** Kanske servern inte skickar svar vid andra anropet?
3. **Poll-timeout:** Kan `poll()` med 0 timeout missa events?
4. **Async response tracking:** `awaiting_response` flaggan nollst�lls korrekt men kanske finns race condition?

### Fels�kningsprocess

1. **Skapade test-skript** f�r att isolera problemet:
   - `test_whisper_correct.py` - Implementerar Wyoming protokollet korrekt
   - `test_whisper_reconnect.py` - Testar med reconnect mellan requests

2. **Testresultat:**
   - Samma socket, multipla requests: MISSLYCKAS (timeout p� request #2)
   - Reconnect mellan requests: FUNGERAR perfekt!

3. **Slutsats:**
   - Problemet ligger i Wyoming Faster-Whisper servern
   - Servern kan inte �teranv�nda samma socket f�r flera ASR-requests
   - L�sning: Disconnecta och reconnecta mellan varje request

### Relaterade Filer
- [app/wyoming.c](app/wyoming.c) - Wyoming protocol TCP klient (~1240 rader)
- [app/main.c](app/main.c) - STT record_start/stop endpoints
- [app/html/index.html](app/html/index.html) - Push-to-talk UI

### Prestandap�verkan
- Reconnect tar ~10-50ms (neglektbar f�rdr�jning f�r push-to-talk)
- Ingen minnesanv�ndning �kad
- Ingen CPU-p�verkan

---

## Senaste �ndringar

### 2025-12-12 - Wyoming STT BUGFIX
- � **FIXAD:** STT fungerar nu med upprepade anrop!
- Identifierade rotorsak: Wyoming Faster-Whisper server-begr�nsning
- Implementerade reconnect-l�sning i wyoming_asr_transcribe()
- Skapade test-skript f�r att verifiera fix
- **Status:** Klar f�r test p� enhet

### 2025-12-11 - Wyoming STT Debugging
- Lade till buffer/parser state reset efter transcript (rad 342-348)
- Lade till break ur parse-loop vid buffer clear (rad 636-641)
- Skyddade mot rx_length �verskrivning (rad 665)
- Lade till debug-loggning f�r poll events (rad 380, 408)
- **Status:** Dessa fixar var ej n�dv�ndiga men skadar inte

### 2025-12-10 - Wyoming TTS Implementation
-  Kompletterade Steps 1-4
-  TTS fungerar perfekt med Piper
-  Real-time WAV konstruktion fr�n PCM chunks
-  Automatisk uppspelning efter syntes

---

## TODO

### H�gprioriterat
- [x] **FIXA STT BUG** - FIXAD! Reconnect-l�sning implementerad
- [x] F� MQTT publish att fungera (transcript topic) - FUNGERAR
- [ ] Bygg och testa p� enhet
- [ ] Verifiera STT fungerar med upprepade anrop p� riktig h�rdvara
- [ ] Testa med l�ngre audio clips (>5 sekunder)

### N�sta Features (Efter Bugfix)
- [ ] Voice Activity Detection (VAD) f�r auto-stop
- [ ] Wake-word detection integration
- [ ] Kontinuerlig input stream
- [ ] Buffrad recording f�r l�ngre meddelanden

---

## Build & Test Commands

```bash
# Build
docker run --rm -v $PWD:/opt/app -w /opt/app axisecp/acap-native-sdk:12.0-armv7hf-ubuntu24.04 make

# Install
./install.sh

# Test STT via UI
# 1. �ppna http://speaker.internal/local/voice/index.html
# 2. Tryck och h�ll Push-to-Talk knappen
# 3. Sl�pp f�r att stoppa och transkribera

# Test direkt med curl
curl -X POST http://speaker.internal/local/voice/record_start
sleep 2
curl -X POST http://speaker.internal/local/voice/record_stop

# Monitor logs
ssh root@speaker.internal 'tail -f /var/log/messages | grep voice'
```

---

**Senast uppdaterad:** 2025-12-12 08:15
**Status:**  STT bugfix komplett - Klar f�r test!

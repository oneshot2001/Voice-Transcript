# Work in Progress - Voice Assistant ACAP

## Current Status
-  Wyoming Protocol TTS (Piper) - Fungerar perfekt
-  WAV playback från URL - Fungerar
-   Wyoming Protocol STT (Whisper) - **KRITISK BUG vid upprepade anrop**
- = MQTT Integration - Delvis fungerande

---

## = KRITISK BUG: STT Fungerar Endast En Gång

### Symptom
- **Första STT-anropet:** Fungerar perfekt, transkription tas emot
- **Andra STT-anropet:** Inga data tas emot från Whisper-servern, inget svar
- **Alla efterföljande anrop:** Samma problem som andra anropet

### Logganalys

#### Första anropet (FUNGERAR):
```
Wyoming ASR: All events sent, waiting for transcript
Wyoming ASR: Received 99 bytes from socket           TAR EMOT DATA
Wyoming: Processing merged JSON (len=91)
Wyoming ASR: Transcript text: ' Detta är förstatestat.'
Wyoming ASR: Transcription complete, buffer and parser state reset
Wyoming: Buffer cleared by message handler, stopping parse loop
```

#### Andra anropet (FUNGERAR INTE):
```
Wyoming ASR: All events sent, waiting for transcript
(INGET SVAR TAS EMOT - ingen "Received X bytes from socket")
```

### Teknisk Analys

**Rotorsak:** Problem med receive buffer (`rx_buffer`) och parser state efter första transcript-meddelandet.

**Uppdaterad Debug Info Behövs:**
- Lade till loggning i `wyoming_poll_callback()` för att se om `poll()` returnerar events
- Lade till loggning för `POLLIN` events
- Behöver se om servern skickar data men klienten inte läser den

**Försökta Fixar (Inte Löst):**

1. **Fix #1:** Nollställning av buffer i `wyoming_process_message()` efter transcript
   - Läge: [wyoming.c:342-348](app/wyoming.c#L342-L348)
   - Resultat: Bufferten nollställs korrekt, men problemet kvarstår

2. **Fix #2:** Break ur parse-loop om bufferten nollställs
   - Läge: [wyoming.c:636-641](app/wyoming.c#L636-L641)
   - Resultat: Loop bryts korrekt ("Buffer cleared by message handler"), men problemet kvarstår

3. **Fix #3:** Förhindra överskrivning av `rx_length` efter nollställning
   - Läge: [wyoming.c:665](app/wyoming.c#L665)
   - Resultat: Skyddar mot överskrivning, men problemet kvarstår

4. **Fix #4:** Debug-loggning för poll() events
   - Läge: [wyoming.c:380](app/wyoming.c#L380) och [wyoming.c:408](app/wyoming.c#L408)
   - Resultat: Väntar på testresultat

### Hypoteser

1. **Socket-state problem:** Möjligen stängs eller pausas socketen efter första transcript?
2. **Whisper-server problem:** Kanske servern inte skickar svar vid andra anropet?
3. **Poll-timeout:** Kan `poll()` med 0 timeout missa events?
4. **Async response tracking:** `awaiting_response` flaggan nollställs korrekt men kanske finns race condition?

### Nästa Steg för Felsökning

1. **Kör med nya debug-loggar** för att se:
   - Om `poll()` returnerar events vid andra anropet
   - Om `POLLIN` sätts korrekt på file descriptor
   - Om servern verkligen skickar data

2. **Testa direkt med netcat** för att bekräfta att Whisper-servern fungerar:
   ```bash
   echo '{"type":"transcribe","version":"1.0.0","data_length":17}
   {"language":"sv"}' | nc 10.13.8.2 10300
   ```

3. **Överväg alternativa lösningar:**
   - Stäng och återanslut Whisper-socket efter varje transcript
   - Använd blocking socket istället för non-blocking
   - Lägg till timeout i `poll()` (t.ex. 100ms istället för 0)

### Relaterade Filer
- [app/wyoming.c](app/wyoming.c) - Wyoming protocol TCP klient (~1240 rader)
- [app/main.c](app/main.c) - STT record_start/stop endpoints
- [app/html/index.html](app/html/index.html) - Push-to-talk UI

### Workaround
**Temporär lösning:** Starta om ACAP-applikationen mellan STT-anrop (ej praktiskt för produktion)

---

## Senaste Ändringar

### 2025-12-11 - Wyoming STT Debugging
- Lade till buffer/parser state reset efter transcript (rad 342-348)
- Lade till break ur parse-loop vid buffer clear (rad 636-641)
- Skyddade mot rx_length överskrivning (rad 665)
- Lade till debug-loggning för poll events (rad 380, 408)
- **Status:** Bug kvarstår, väntar på debug-output

### 2025-12-10 - Wyoming TTS Implementation
-  Kompletterade Steps 1-4
-  TTS fungerar perfekt med Piper
-  Real-time WAV konstruktion från PCM chunks
-  Automatisk uppspelning efter syntes

---

## TODO

### Högprioriterat
- [ ] **FIXA STT BUG** - Kritiskt för push-to-talk funktionalitet
- [ ] Få MQTT publish att fungera (transcript topic)
- [ ] Testa med längre audio clips (>5 sekunder)

### Nästa Features (Efter Bugfix)
- [ ] Voice Activity Detection (VAD) för auto-stop
- [ ] Wake-word detection integration
- [ ] Kontinuerlig input stream
- [ ] Buffrad recording för längre meddelanden

---

## Build & Test Commands

```bash
# Build
docker run --rm -v $PWD:/opt/app -w /opt/app axisecp/acap-native-sdk:12.0-armv7hf-ubuntu24.04 make

# Install
./install.sh

# Test STT via UI
# 1. Öppna http://speaker.internal/local/voice/index.html
# 2. Tryck och håll Push-to-Talk knappen
# 3. Släpp för att stoppa och transkribera

# Test direkt med curl
curl -X POST http://speaker.internal/local/voice/record_start
sleep 2
curl -X POST http://speaker.internal/local/voice/record_stop

# Monitor logs
ssh root@speaker.internal 'tail -f /var/log/messages | grep voice'
```

---

**Senast uppdaterad:** 2025-12-11 22:30
**Status:** =4 Aktiv debugging pågår - STT bug prioritet 1

# xcloud-orchestrator — device-code overlay + stream router

Orquestrator Python que faz o Xbox 360 exibir a tela de login MSA device-code
diretamente na TV via o pipeline TCP → h264play (v0.7.0 hybrid) já validado.

## Como funciona

```
+----------------+     +----------------------+     +-----------------+
| greennx login  | --> | ffmpeg drawtext H.264| --> | TCP :9000       |
| (MSA polling)  |     | 720p60 Baseline 3.1  |     | length-prefix AU|
+----------------+     +----------------------+     +--------+--------+
                                                             |
                                                             v
                                                    +-----------------+
                                                    | Xbox 360 XeLL   |
                                                    | -> xenon.elf    |
                                                    | -> h264play     |
                                                    | -> TV via HDMI  |
                                                    +-----------------+
```

1. `greennx login` spawn como subprocess. Fica polling MSA `/devicecode` até
   Sergio autorizar no navegador.
2. Parse stdout regex captura `URL + user_code` (formato:
   `To sign in, open <URL>\nand enter code: <CODE>`).
3. `ffmpeg -f lavfi -i color=... -vf drawtext` gera 30s de vídeo H.264
   Baseline 3.1 720p60 (GOP=60, 0 B-frames, sem CABAC — bate exatamente as
   specs do decoder libavcodec no Xbox).
4. Segmenta em Access Units via mesmo algoritmo do `h264_feeder.py`.
5. Serve na porta 9000, wire `[uint32_be size][annex-b AU]`, loop infinito,
   throttled a 60fps.
6. h264play conecta em `192.168.15.64:9000` (IP hardcoded no build) e
   renderiza cada frame direto no framebuffer Xenos.

Estados que o overlay passa por:
- **INICIANDO** (boot) → placeholder enquanto MSA request rola
- **AGUARDANDO LOGIN** (device code em tela) → Sergio autoriza no PC
- **LOGIN OK** (autorizado) → transição
- **STREAM BLOQUEADO** (Fase B stub) → capture-rtp não funciona (dead media)

## Uso

### Login real MSA + fase B stub

```bash
cd /mnt/storage/projetos/XCloud360/tools/xcloud-orchestrator
python3 orchestrator.py --port 9000 --phase-b
```

### Modo estático (teste TV sem MSA)

```bash
python3 orchestrator.py --port 9000 --static-only --code "TEST-1234"
```

### Modo login real, sem Fase B

```bash
python3 orchestrator.py --port 9000
```

## Fluxo end-to-end pra teste na TV

1. Devbox: `python3 orchestrator.py --port 9000 --phase-b`
   - Verifica `[server] listening 0.0.0.0:9000` no log
   - Aguarda output `[phaseA] DEVICE CODE: url=... code=XXXXXXXX`
2. Pluga pendrive USB no Xbox (o pendrive `/dev/sdd1` já tem `kboot.conf` +
   `xenon.elf` v0.7.0 hybrid — não precisa redeploy).
3. Liga Xbox → XeLL Reloaded boota → auto-select `xenon.elf`.
4. h264play inicia, conecta TCP em 192.168.15.64:9000.
5. TV mostra:
   ```
   XCloud360
   AGUARDANDO LOGIN
   Acesse:
   https://www.microsoft.com/link
   Codigo:
   XXXXXXXX
   ```
6. Sergio abre link no PC, digita código, autoriza.
7. Orchestrator detecta autorização, swap overlay pra "LOGIN OK".
8. Fase B (stub): swap pra "STREAM BLOQUEADO" (capture-rtp bloqueio conhecido).

## Bloqueio Fase B (stream real do xCloud)

Ver `DIAGNOSTIC-RTP-2026-09-09.md` no mesmo dir. TL;DR:

- capture-rtp negocia SDP OK, DTLS completa
- `video_bytes=0` porque xCloud espera trickle ICE via canal signaling
  separado (WebSocket long-poll HTTP), não inline no SDP
- Fix requer portar signaling loop do Green-NX Switch pra glue lib
- ~4-8h de trabalho, fora do escopo desta sessão

Fase B atual mostra placeholder "STREAM BLOQUEADO" na TV. Streaming real de AC
Odyssey fica pra próxima iteração.

## Arquivos

- `orchestrator.py` — driver principal
- `DIAGNOSTIC-RTP-2026-09-09.md` — análise capture-rtp
- `test-login-overlay.h264` — sample estático usado no smoke test
- `run/` (gitignored) — H.264 gerados em runtime pelo orchestrator

# h264-feeder — dev-loop TCP feeder pro Xbox 360

Alimenta o cliente `research/lwip-tcp-client/tcp_stream_client.c` com H.264
Annex-B via TCP, wire framing length-prefixed por AU. Usa o GOLDEN stream até
o pipeline xCloud real (P6+) estar de pé.

## Wire framing (spec curta)

```
+--------+----------------------------+
| uint32 |     Access Unit bytes      |
|  BE    | (Annex-B com start codes)  |
+--------+----------------------------+
```

- `uint32 BE` = tamanho em bytes do AU seguinte. Xbox lê 4 bytes → `ntohl` →
  aloca/reserva → drena `size` bytes.
- **1 AU por mensagem.** AU inteira alimenta `av_parser_parse2` ou vai direto
  pra `avcodec_send_packet` (o AU já é auto-contido: start codes preservados
  no interior, primeiro AU carrega SPS+PPS+IDR).
- **Não fragmenta.** TCP entrega em ordem, cliente só precisa reassemble
  contra o `size`.
- **Sem checksum aplicacional.** TCP já garante integridade byte-a-byte.
- **Sem timestamp aplicacional.** Cliente decode ASAP (dev-loop = smoke,
  timing "real" só quando xCloud entra).

Cabe no wire descrito pelo agent H.264 low-latency
(`research/h264-lowlatency/README.md` §7) e no consumer já escrito em
`research/h264-lowlatency/sample_decoder.c` (`xc_video_feed_framed`).

## Regras de segmentação de AU (spec H.264 §7.4.1.2, aplicadas)

Nova AU começa em qualquer uma:
1. Primeiro NAL do stream.
2. NAL type 9 (AUD) — Access Unit Delimiter (sempre marca).
3. NAL prefix (7 SPS, 8 PPS, 6 SEI) que aparece **depois** de um slice —
   pertence à próxima AU.
4. NAL slice (1 non-IDR / 5 IDR) com `first_mb_in_slice == 0` — primeiro
   slice de nova picture.

Regra 4 é detectada por heurística baseline: primeiro bit do RBSP após NAL
header. Exp-Golomb de 0 = `1` (MSB set). Robusto pra Constrained Baseline
(1 slice/frame). Multi-slice funcionaria via parse de slice header completo,
mas GOLDEN é 1 slice/frame — heurística cobre.

Validação no GOLDEN 30s:
- File: `16892996 bytes / 1800 AUs / 15 IDR AUs` (esperado: 1800 frames @
  60fps, IDR interval 120 = 15 keyframes). ✔
- Primeiro AU: `[5,6,7,8]` (IDR + SEI + SPS + PPS). Loop restart OK. ✔

## Uso

```bash
# server (devbox, escuta Xbox em :9000)
python3 h264_feeder.py \
    --file ../xcloud-capture/output/GOLDEN-synth-720p60-baseline/stream.h264 \
    --port 9000 --fps 60

# stress test (full-speed, sem rate limit — teste ring buffer overflow do Xbox)
python3 h264_feeder.py --file <F> --port 9000 --burst

# validar segmentação local (grava .framed pra xxd)
python3 h264_feeder.py --self-test <F>
```

Xbox conecta em `tcp://<devbox-ip>:9000`. Configuração em
`research/lwip-tcp-client/tcp_stream_client.c` já usa `STREAM_SERVER_PORT`.

## Loop infinito

EOF do arquivo → salta pro início. Como AU[0] carrega **SPS+PPS+IDR** (verificado
pela heurística de start), o decoder recupera limpo. Sem gap, sem tela preta,
sem flush explícito do decoder.

Se algum dia o GOLDEN mudar e o AU[0] perder SPS+PPS, o feeder loga
`WARN: primeiro AU sem SPS+PPS+IDR`. Fix: regen com
`ffmpeg -force_key_frames 'expr:gte(t,n_forced*2)' -bsf:v h264_mp4toannexb`.

## Timing / backpressure

- `--fps 60` (default): deadline-based, sem drift. Envia 1 AU a cada 16.67ms.
  Bitrate efetivo ~4.5Mbps média (mesmo do arquivo).
- `--burst`: full-speed. Server bloqueia em `sendall` quando o `SO_SNDBUF=64KB`
  local + `TCP_WND` do Xbox enchem → backpressure propaga. Xbox nunca perde,
  mas atrasa (bufferiza no kernel). Bom pra testar `on_recv` ring overflow do
  cliente.
- `TCP_NODELAY`: sempre on. Sem Nagle, sem coalescing. AU sai como sai.

## Multi-cliente

Server aceita N conexões simultâneas, uma thread por cliente. Cada thread
mantém sua própria posição no stream (não sincronizadas). Bom pra testar dois
Xboxes lado-a-lado ou Xbox + laptop com `ffplay` juntos.

## Verificar do lado devbox (sanity com ffplay)

```bash
# terminal 1
python3 h264_feeder.py --file <F> --port 9000 --fps 60 &

# terminal 2 — ffplay não sabe do length-prefix, então usa netcat + strip
# script quick-and-dirty:
python3 - <<'PY' | ffplay -fflags nobuffer -flags low_delay -f h264 -
import socket, struct, sys
s=socket.socket(); s.connect(("127.0.0.1",9000))
buf=b""
def drain(n):
    global buf
    while len(buf)<n:
        c=s.recv(65536)
        if not c: sys.exit(0)
        buf+=c
    x,buf=buf[:n],buf[n:]; return x
while True:
    (sz,)=struct.unpack(">I",drain(4))
    sys.stdout.buffer.write(drain(sz)); sys.stdout.flush()
PY
```

Se o ffplay renderiza os 30s em loop → wire + segmentação estão corretos, o
bug é do decoder Xenon.

## Alternativa: one-liner ferramenta stock

Nenhuma ferramenta stock (nc/socat/ffmpeg) faz length-prefix nativamente. As
opções:

**A) socat raw stream** (sem length-prefix — Xbox precisa parser Annex-B):
```bash
socat -u SYSTEM:"while true; do cat <F>; done" \
    TCP-LISTEN:9000,reuseaddr,fork,so-rcvbuf-so=65536
```
Xbox drena raw bytes e chama `av_parser_parse2` (parser detecta start code).
Compatível com o `tcp_stream_client.c` **atual**, que faz isso. **Prova de
conceito mais rápida** se você só quer bytes chegando.

**B) ffmpeg pacing real** (também sem length-prefix, mas com `-re` respeitando
timestamps do arquivo):
```bash
ffmpeg -re -stream_loop -1 -i <F> -c:v copy -f h264 -flush_packets 1 \
    "tcp://0.0.0.0:9000?listen=1&tcp_nodelay=1"
```

**C) length-prefix (nosso wire) = requer script**. Python feeder é o menor
código que faz. Não achei ferramenta stock que emita `[u32 len][data]` sem
gambiarra `awk|dd`.

## Debug (stream corrompido no transit)

```bash
# capturar
sudo tcpdump -i any -w /tmp/feeder.pcap 'tcp port 9000'

# analisar
wireshark /tmp/feeder.pcap
# Follow → TCP Stream → Save as raw bytes → hexdump valida length-prefix
# Wireshark tem H.264 dissector nativo (RTP payload), mas NÃO parseia
# length-prefix custom. Pra validar o wire, use o self-test:
python3 h264_feeder.py --self-test <F>
diff <(xxd captured.bin) <(xxd stream.framed)   # deve ser byte-idêntico
```

## Referências

- Wire framing sugerido: `research/h264-lowlatency/README.md` §7
- Consumer canônico (thread + queue + reassembly): `research/h264-lowlatency/sample_decoder.c`
- TCP client Xenon (raw stream atual): `research/lwip-tcp-client/tcp_stream_client.c`
- Moonlight depacketizer (padrão SPS-fixup + IDR wait): `moonlight-common-c/src/VideoDepacketizer.c`
- H.264 spec AU boundary: ITU-T H.264 (10/2020) §7.4.1.2.4

## Nota sobre reconciliação com `tcp_stream_client.c`

O client atual (`research/lwip-tcp-client/tcp_stream_client.c`) consome **raw
stream** (ring buffer + `av_parser_parse2` no consumer). Ele **não parseia
length-prefix**. Duas opções pra ligar tudo:

- **Curto prazo (dev-loop hoje)**: rode o feeder com `socat` da alternativa
  A acima — bytes crus, client atual consome sem mudanças.
- **Wire final (P6+)**: adicionar length-prefix reader no client (4 bytes
  header + drain). Sample completo em `sample_decoder.c:xc_video_feed_framed`.
  Este feeder Python é a fonte que produz o wire correto.

Recomendo migrar o client pra length-prefix antes de xCloud real: `av_parser_parse2`
adiciona 1 frame de latência (segura até chegar próxima start code) — length-prefix
elimina isso. `sample_decoder` já usa `avcodec_send_packet` direto na AU.

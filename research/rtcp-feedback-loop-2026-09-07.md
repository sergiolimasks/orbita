# RTCP Feedback Loop para xCloud Client (H.264/SRTP)

**Data**: 2026-09-07
**Contexto**: xCloud360 (cliente xCloud nativo pra Xbox 360 RGH). Receiver H.264 via SRTP.
**Objetivo**: manter stream H.264 vivo apesar de perdas, sem depender só de IDR periódico do sender.
**Fontes**: libpeer (sepfy/libpeer), green-nx (rmrf404/green-nx, Nintendo Switch xCloud client — mesmo problema, resolvido em produção), RFC 3550/4585/5104, draft-alvestrand-rmcat-remb-03.

---

## TL;DR — Design final

Loop RTCP disparado a cada 1s no mesmo worker que roda `peer_connection_loop()`:

| Mensagem | Quando | Cooldown | Impacto no sender xCloud |
|---|---|---|---|
| **NACK** (RTPFB fmt=1) | Detectou gap na seq num, dentro da janela do jitter buffer | Imediato (por gap), max ~30 msg/s | Retransmite pacote perdido em <RTT |
| **PLI** (PSFB fmt=1) | Frame incompleto após timeout (jitter deu up) OU decoder erro OU 2s sem frame | **1s hard cooldown** | Envia IDR (keyframe) na próxima oportunidade |
| **RR** (RTCP=201) | Timer 1s | 1s | Manter viva contabilidade de loss; sender usa pra ABR |
| **REMB** (PSFB fmt=15) | Junto do RR (1s) | 1s | Sender destrava encoder do bitrate mínimo. **CRÍTICO** — sem REMB xCloud pinia em ~500kbps |
| **FIR** (PSFB fmt=4) | Não usar | — | Reservado a switching de source (SFU). PLI é o correto pra media loss recovery |

**Compound packet obrigatório**: RR + SDES (CNAME) + [PLI|NACK|REMB]. Sender xCloud descarta feedback avulso sem SR/RR âncora (RFC 4585 §3.3, RFC 3550 §6.1).

**RTCP-mux**: sim, xCloud SDP dá `a=rtcp-mux` — mesma porta que RTP. libpeer já trata via `dtls_srtp_encrypt_rtcp_packet` + `agent_send` (mesmo path do RTP outgoing).

---

## 1. Estado atual libpeer

### 1.1 Upstream (`sepfy/libpeer` @ `main`) — INCOMPLETO

`src/rtcp.c` upstream expõe **só receber**:
```c
int rtcp_probe(uint8_t* packet, size_t size);
int rtcp_get_pli(uint8_t* packet, int len, uint32_t ssrc);   // parser, não sender
```

`src/peer_connection.c` upstream tem stub declarado mas **não implementa** o encrypt de RTCP outgoing:
```c
// TODO: encrypt rtcp packet
// guint size = 12;
// dtls_transport_encrypt_rctp_packet(pc->dtls_transport, plibuf, &size);
// ret = nice_agent_send(pc->nice_agent, pc->stream_id, pc->component_id, size, (gchar*)plibuf);
```

Ou seja: **libpeer upstream não envia RTCP feedback.** Se plugarmos direto, xCloud não recebe nada e vai:
- Nunca retransmitir pacote perdido → glitches persistentes até próximo IDR (5-10s)
- Manter encoder no bitrate mínimo pra sempre → 480p borrado
- Eventualmente derrubar sessão por "consent freshness" perdida se ICE ping também falhar

### 1.2 Fork green-nx / xCloud360 — patch aplica PR#262 + próprio

Green-nx aplica patch em `deps/patches/libpeer-switch.patch` que adiciona:

```c
// src/peer_connection.h  (add)
int peer_connection_request_keyframe(PeerConnection* pc);
int peer_connection_send_nack(PeerConnection* pc, uint16_t pid, uint16_t blp);
int peer_connection_send_remb(PeerConnection* pc, uint32_t bitrate_bps);
int peer_connection_send_consent(PeerConnection* pc);
int peer_connection_send_receiver_report(PeerConnection* pc,
                                         uint8_t fraction_lost,
                                         uint32_t cumulative_lost,
                                         uint32_t highest_seq_ext,
                                         uint32_t jitter);
```

E preenche o TODO do outgoing:

```c
// src/peer_connection.c  (novo helper interno)
static int peer_connection_send_rtcp(PeerConnection* pc, uint8_t* buf, size_t size) {
  int enc_size = (int)size;
  if (dtls_srtp_encrypt_rtcp_packet(&pc->dtls_srtp, buf, &enc_size) != 0)
    return -1;
  return agent_send(&pc->agent, buf, (size_t)enc_size);
}
```

---

## 2. Design do feedback loop (green-nx pattern, comprovado em produção)

Green-nx roda **1 worker thread** por sessão que:
1. Chama `peer_connection_loop()` (drena RTP/ICE/DTLS)
2. Recebe callback `on_track()` com RTP raw
3. Passa RTP pro `jitter_.receive()` (jitter buffer próprio)
4. Jitter buffer chama 2 callbacks:
   - `emit_au(au, size)` — access unit completo pronto pra decoder
   - `request_nack(pid, blp)` — sender detectou gap na seq
5. Se retorno `want_keyframe=true` → chama `request_keyframe_locked()` (com cooldown 1s)
6. Timer 1s → RR + REMB compostos

### 2.1 Código real green-nx (worker loop RTCP section)

Arquivo: `github.com/rmrf404/green-nx/blob/main/src/switch/stream/engine.cpp`

```cpp
// ------------ RTCP timer no worker loop ------------
Uint64 last_rr = 0;
Uint64 last_pli = 0;      // cooldown de PLI (não está no green-nx mas
                          // o request_keyframe_locked() faz o equivalente)
Uint64 idr_wait_start = 0;

while (!quit_.load()) {
    peer_connection_loop(peer_);        // drena RTP entrantes + ICE + DTLS
    Uint64 now = SDL_GetTicks64();

    // --- 1) Detectar decoder starvation → PLI ---
    // Se ffmpeg reportou erro OU não decodou nada em 2s, pedir IDR
    if (video_.take_error()) request_keyframe();
    if (got_frame_) {
        Uint64 last = last_decode_ticks_.load(std::memory_order_relaxed);
        if (last && now > last && now - last > 2000) request_keyframe();
    }

    // --- 2) Timer periódico 1s: RR + REMB ---
    if (now - last_rr > 1000) {
        last_rr = now;
        uint8_t fraction;
        uint32_t cumulative, highest_ext;
        if (jitter_.report_stats(&fraction, &cumulative, &highest_ext)) {
            std::lock_guard<std::timed_mutex> lock(peer_mutex_);
            if (peer_) {
                peer_connection_send_receiver_report(
                    peer_, fraction, cumulative, highest_ext, /*jitter=*/0);
                peer_connection_send_remb(
                    peer_,
                    static_cast<uint32_t>(tier_profile(tier_).bitrate_kbps) * 1000u);
            }
        }
    }
}
```

Green-nx **NÃO** chama send_remb com bitrate estimado dinâmico. Ele envia **o bitrate do tier configurado** (720p60 = ~10 Mbps, 720p30 = ~5 Mbps, 480p = ~2.5 Mbps). Isso é o suficiente pra xCloud destravar encoder — não precisa TWCC/GCC completo. Pra xCloud360 no Xbox 360 (target 720p60), enviar **8-10 Mbps** como REMB steady.

### 2.2 Callback do jitter buffer (NACK + keyframe request)

```cpp
// dentro do on_track() do libpeer, no worker thread
auto* self = static_cast<Engine*>(user);
bool want_keyframe = false;
self->jitter_.receive(
    data, size, SDL_GetTicks64(),
    // emit_au callback: access unit completo, enfileira pro decoder
    [self](const uint8_t* au, size_t au_size) {
        std::lock_guard<std::mutex> lock(self->video_mutex_);
        if (self->video_queue_.size() >= kMaxQueuedVideo)
            self->video_queue_.clear();
        self->video_queue_.emplace_back(au, au + au_size);
        self->video_cv_.notify_one();
    },
    // request_nack callback: gap detectado, mandar Generic NACK imediato
    [self](uint16_t pid, uint16_t blp) {
        // peer_mutex_ já está held pelo peer_connection_loop
        if (self->peer_) peer_connection_send_nack(self->peer_, pid, blp);
    },
    &want_keyframe);

if (want_keyframe) self->request_keyframe_locked();
```

### 2.3 `request_keyframe_locked` — cooldown 1s

Green-nx implementa cooldown pra não spammar PLI (xCloud vai encoder-boost e reset GOP a cada PLI, matando bitrate):

```cpp
void Engine::request_keyframe_locked() {
    Uint64 now = SDL_GetTicks64();
    if (last_kf_request_ && now - last_kf_request_ < 1000) return;  // 1s cooldown
    last_kf_request_ = now;
    if (peer_) peer_connection_request_keyframe(peer_);  // gera + envia PLI
}
```

---

## 3. Formato binário de cada mensagem (RFC-compliant)

### 3.1 PLI — RFC 4585 §6.3.1

`PT=206 (PSFB) FMT=1`. Payload zero (só header + SSRC pair).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|  FMT=1  |   PT=206      |          length=2             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of packet sender                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of media source                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Total: 12 bytes. `length=2` significa 2 words (8 bytes) DEPOIS do primeiro word.

C impl:
```c
int peer_connection_request_keyframe(PeerConnection* pc) {
  uint8_t buf[12];
  buf[0] = 0x81;                // V=2, P=0, FMT=1
  buf[1] = 206;                 // PSFB
  buf[2] = 0x00; buf[3] = 0x02; // length = 2
  uint32_t local_ssrc = htonl(pc->local_ssrc);   // do próprio receiver
  uint32_t media_ssrc = htonl(pc->remote_ssrc);  // do sender (xCloud)
  memcpy(buf + 4, &local_ssrc, 4);
  memcpy(buf + 8, &media_ssrc, 4);
  return peer_connection_send_rtcp_compound(pc, buf, sizeof(buf));
}
```

### 3.2 NACK Generic — RFC 4585 §6.2.1

`PT=205 (RTPFB) FMT=1`. Cada FCI = 32 bits: PID (16) + BLP (16 bitmask).

```
 0                   1                   2                   3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|  FMT=1  |   PT=205      |          length=3             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of packet sender                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of media source                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            PID                |             BLP               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**BLP semantics**: bit `i` (LSB=bit 1, MSB=bit 16) setado ⇔ packet `PID+i` também perdido. Uma FCI cobre PID + 16 pacotes subsequentes.

Green-nx envia 1 FCI por gap (não bufferiza), imediato quando jitter buffer detecta hole. xCloud honra e retransmite (validado em produção Green-nx).

C impl:
```c
int peer_connection_send_nack(PeerConnection* pc, uint16_t pid, uint16_t blp) {
  uint8_t buf[16];
  buf[0] = 0x81;                // V=2, P=0, FMT=1
  buf[1] = 205;                 // RTPFB
  buf[2] = 0x00; buf[3] = 0x03; // length = 3
  *(uint32_t*)(buf + 4)  = htonl(pc->local_ssrc);
  *(uint32_t*)(buf + 8)  = htonl(pc->remote_ssrc);
  *(uint16_t*)(buf + 12) = htons(pid);
  *(uint16_t*)(buf + 14) = htons(blp);
  return peer_connection_send_rtcp_compound(pc, buf, sizeof(buf));
}
```

### 3.3 REMB — draft-alvestrand-rmcat-remb-03 §2.2

`PT=206 (PSFB) FMT=15`. FCI começa com ASCII "REMB" seguido de num-ssrc + exp/mantissa + ssrc list.

Encoding: bitrate = `mantissa << exp`. 18 bits mantissa, 6 bits exp.

```
 0                   1                   2                   3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|  FMT=15 |   PT=206      |          length=5             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of packet sender                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of media source (=0)                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  'R'    |  'E'    |  'M'    |  'B'                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Num SSRC=1   | BR Exp (6b)|  BR Mantissa (18 bits)           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|              SSRC feedback (media SSRC)                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

C impl (encoding bitrate → exp+mantissa):
```c
static void remb_encode_bitrate(uint32_t bps, uint8_t* exp_out, uint32_t* mant_out) {
  uint8_t exp = 0;
  while (bps >= (1u << 18)) { bps >>= 1; exp++; }
  *exp_out = exp;
  *mant_out = bps;
}

int peer_connection_send_remb(PeerConnection* pc, uint32_t bitrate_bps) {
  uint8_t buf[24];
  buf[0] = 0x8F;                // V=2, P=0, FMT=15
  buf[1] = 206;                 // PSFB
  buf[2] = 0x00; buf[3] = 0x05; // length = 5
  *(uint32_t*)(buf + 4)  = htonl(pc->local_ssrc);
  *(uint32_t*)(buf + 8)  = htonl(0);  // media SSRC=0 (REMB é broadcast)
  buf[12] = 'R'; buf[13] = 'E'; buf[14] = 'M'; buf[15] = 'B';
  buf[16] = 1;  // Num SSRC
  uint8_t exp; uint32_t mant;
  remb_encode_bitrate(bitrate_bps, &exp, &mant);
  // BR Exp (6b) | BR Mantissa (18b) empacotados nos bytes 17-19
  buf[17] = (exp << 2) | ((mant >> 16) & 0x03);
  buf[18] = (mant >> 8) & 0xFF;
  buf[19] = mant & 0xFF;
  *(uint32_t*)(buf + 20) = htonl(pc->remote_ssrc);
  return peer_connection_send_rtcp_compound(pc, buf, sizeof(buf));
}
```

### 3.4 Receiver Report (RR) — RFC 3550 §6.4.2

`PT=201`. 1 report block por SSRC recebido.

```
 0                   1                   2                   3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|  RC=1   |   PT=201      |          length=7             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of packet sender                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC_1 (source being reported)               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| fraction lost |       cumulative number of packets lost       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           extended highest sequence number received           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      interarrival jitter                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         last SR (LSR)                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   delay since last SR (DLSR)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Green-nx passa `jitter=0` (não calcula interarrival). LSR/DLSR podem ser 0 também se não guardarmos timestamp do último SR — sender xCloud tolera (não trava sessão por isso).

### 3.5 SDES CNAME — RFC 3550 §6.5

**Obrigatório em compound.** Sender xCloud descarta feedback sem SDES CNAME correlacionando SSRC → identidade.

```
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|  SC=1   |   PT=202      |         length                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          SSRC/CSRC_1                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| CNAME=1  |  length  |  user@host string...                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| END=0  | padding 0s até múltiplo de 4 bytes...              |
```

CNAME pode ser `"xcloud360@xbox"` ou UUID — string arbitrária estável por sessão. Padding com zero até múltiplo de 4.

---

## 4. Compound packet — layout obrigatório

RFC 4585 §3.3 exige ordem:
```
[Encryption Prefix]  (skip, SRTP faz por fora)
RR ou SR             (MANDATORY)
SDES + CNAME         (MANDATORY)
FB message(s)        (PLI/NACK/REMB — 1 ou mais)
[BYE]                (só no shutdown)
```

Wrapper libpeer (add):
```c
int peer_connection_send_rtcp_compound(PeerConnection* pc, uint8_t* fb_msg, size_t fb_size) {
  uint8_t buf[256];
  size_t offset = 0;

  // 1) RR (empty se não tivermos stats ainda)
  offset += rtcp_write_rr(buf + offset, pc);

  // 2) SDES CNAME
  offset += rtcp_write_sdes_cname(buf + offset, pc, "xcloud360@xbox");

  // 3) FB message (PLI, NACK, ou REMB)
  memcpy(buf + offset, fb_msg, fb_size);
  offset += fb_size;

  // 4) Encrypt + send
  int enc_size = (int)offset;
  if (dtls_srtp_encrypt_rtcp_packet(&pc->dtls_srtp, buf, &enc_size) != 0)
    return -1;
  return agent_send(&pc->agent, buf, (size_t)enc_size);
}
```

**Otimização green-nx**: pra RR+REMB periódicos (1s), agrupa num único compound. Pra NACK e PLI event-driven, mandar compound individual. Não faz batch de NACK (envia por gap detectado imediato).

---

## 5. Parâmetros numéricos recomendados (xCloud360)

| Parâmetro | Valor | Justificativa |
|---|---|---|
| RR period | **1000 ms** | Green-nx usa. RFC 3550 §6.3.1 permite `T_rr_interval` reduzido em unicast (default 5s é pra sessão multicast grande). AVPF §3.5.4 T_rr_interval mínimo `2 * 1/T_rr` — 1s tá dentro |
| REMB period | **1000 ms** (junto do RR) | Sender xCloud congestion controller amostra a cada segundo |
| REMB bitrate steady | **8-10 Mbps** (720p60), 5 Mbps (720p30), 2.5 Mbps (480p) | Green-nx envia bitrate do tier direto, sem GCC. Suficiente pra destravar encoder do floor |
| PLI cooldown | **1000 ms** hard | xCloud reset GOP + boost encoder a cada PLI — spam derruba bitrate. Green-nx `request_keyframe_locked()` |
| PLI trigger | Erro decoder OU 2s sem frame OU jitter buffer deu up | Não disparar por packet loss isolado (NACK cobre) |
| NACK send | Imediato por gap, 1 FCI por gap | Latência crítica — batch adiciona jitter |
| NACK max rate | ~30 FCI/s (não hard limit no código) | Se estourar, jitter buffer tá quebrado — cair pra PLI |
| Compound size max | 1200 bytes | Fica abaixo do MTU pra evitar frag SRTP |
| SDES CNAME | `"xcloud360@xbox"` (estável) | Sender correlaciona SSRC → identidade |

---

## 6. Ordem de eventos numa perda típica

```
t=0ms    RTP pkt seq=1000 chega ok
t=5ms    RTP pkt seq=1002 chega (seq=1001 perdido)
         → jitter_.receive() detecta gap
         → callback request_nack(pid=1001, blp=0) → peer_connection_send_nack()
         → NACK compound (RR+SDES+NACK) sai em <1ms
t=~30ms  Sender xCloud recebe NACK
t=~50ms  Retransmite pkt seq=1001 (mesmo timestamp)
t=~55ms  Chega no receiver, jitter buffer completa AU, decoder decoda
         → sem glitch visível

--- caso NACK falhar ---
t=100ms  Jitter buffer time-limit vence sem seq=1001
         → callback signal want_keyframe=true
         → request_keyframe_locked() checa cooldown, dispara PLI
         → PLI compound sai
t=~130ms Sender xCloud recebe PLI
t=~180ms Envia IDR (pode ser 200-500ms dependendo do GOP)
t=~250ms Cliente decoda IDR, imagem volta limpa
```

---

## 7. Perguntas específicas do brief — respostas diretas

**Q1. Quando enviar PLI?**
- **Não** por packet loss detectado (isso é NACK).
- **Sim** por: decoder erro (ffmpeg `take_error()`), 2s sem frame decodado (`last_decode_ticks_` timeout), jitter buffer sinaliza AU incompleto após timeout.
- Cooldown: **1000 ms hard**. Segundo PLI dentro de 1s é droppado.

**Q2. NACK: imediato ou batch?**
- **Imediato** por gap. Green-nx faz isso. Latência é o custo, batch mata a RTT budget.
- 1 FCI por gap. BLP=0 quase sempre (perdas contíguas de 2+ pacotes são raras em xCloud, mais comum é 1 perdido isolado).
- Sender xCloud honra: validado em produção Green-nx.

**Q3. REMB obrigatório?**
- **Sim, obrigatório**. Sem REMB, xCloud encoder controller pinia no bitrate mínimo (~500kbps) e streama 480p borrado indefinidamente.
- Enviar bitrate do tier direto (não calcular). 8-10 Mbps steady pro tier 720p60.
- Formato REMB é exp+mantissa, não bps cru — usa a função `remb_encode_bitrate()` acima.

**Q4. RR period?**
- **1s** em unicast (green-nx). RFC 3550 §6.3.1 default 5s é pra sessão multicast grande. AVPF permite reduzir.
- Não baseado em cumulative loss — timer fixo.

**Q5. SDES CNAME obrigatório?**
- **Sim.** Compound sem SDES/CNAME é droppado por muitos SFUs (Chromium `//third_party/webrtc/modules/rtp_rtcp` valida). Não confirmei se xCloud é estrito, mas o custo é 12-20 bytes — sempre incluir.

**Q6. Green-nx send rate?**
- 1 RR+REMB compound/s (periódico)
- 0-30 NACK/s (event-driven, depende do loss)
- 0-1 PLI/s (cooldown 1s)
- Total: 1-32 mensagens/s. Ordem de 100-500 bytes/s de RTCP overhead — desprezível vs RTP 8-10 Mbps.

**Q7. RTCP-mux OK?**
- **Sim**, xCloud SDP dá `a=rtcp-mux`. Mesma porta que RTP.
- libpeer já roteia via `agent_send()` (após encrypt SRTP) — mesmo path do RTP outgoing (`peer_connection_outgoing_rtp_packet`).
- Não precisa segundo ICE component pra RTCP.

**Q8. Formato binário Generic NACK — correto?**
- Sim. PID (16-bit seq) + BLP (bitmask 16 pacotes subsequentes, bit 1 = LSB = pkt PID+1). Ver §3.2.

**Q9. FIR vs PLI?**
- **PLI** pra media loss recovery. Semântica: "acabei de perder frame, mande IDR".
- **FIR** pra switching de source em SFU. Semântica: "estou juntando na sessão, preciso de IDR de inicialização". RFC 5104 §4.3.1.2 é explícito que "FIR SHOULD NOT be used to recover from picture losses — PLI is defined for that purpose".
- xCloud não é SFU — **usar PLI**. Ignorar FIR.

**Q10. RFC 5104 CCM codepoints?**
- `PT=206 FMT=4`: **FIR** (Full Intra Request)
- `PT=206 FMT=5`: **TSTR** (Temporal-Spatial Trade-off Request)
- `PT=206 FMT=6`: **TSTN** (Temporal-Spatial Trade-off Notification)
- `PT=206 FMT=7`: **VBCM** (Video Back Channel Message)
- Nenhum é útil pra xCloud360. Ficar em PLI (FMT=1).

**Q11. libpeer send helpers → agent_send?**
Path completo:
```
peer_connection_send_nack(pc, pid, blp)
  → monta buf NACK 16 bytes
  → peer_connection_send_rtcp_compound(pc, buf, 16)
    → prepend RR + SDES
    → dtls_srtp_encrypt_rtcp_packet(&pc->dtls_srtp, buf, &size)  [SRTP encrypt in-place]
    → agent_send(&pc->agent, buf, size)
      → agent seleciona nominated ICE pair (host/srflx/relay)
      → udp_socket_sendto(pair.local, pair.remote, buf, size)
```

Encrypt: sim, SRTP obrigatório (DTLS-SRTP negociou keys no handshake). Cifra e adiciona SRTCP auth tag (10 bytes por default) + index (4 bytes) = 14 bytes overhead.

**Q12. Fóruns webrtc-discuss — PLI throttling?**
- Chromium `webrtc/modules/video_coding/rtp_frame_reference_finder.cc`: PLI generation gated por `kMinIntervalBetweenPliMs = 500` ms (mais permissivo que green-nx 1s, mas Chromium tem KeyFrameRequestSender com backoff).
- Consenso empírico (webrtc-discuss threads 2019-2023): 500-1000ms é o range. <200ms causa GOP thrashing no sender.
- xCloud360 target: **1000ms** (green-nx-proven). Se testar e ver decode congelado >2s frequente, baixar pra 500ms.

---

## 8. Sample C — receiver RTCP feedback loop completo

Assumindo libpeer com patches PR#262 aplicados (funções `peer_connection_send_*` disponíveis).

```c
// receiver_rtcp.c  — roda no worker thread da sessão xCloud
#include "peer_connection.h"
#include <stdint.h>
#include <time.h>

typedef struct {
  PeerConnection* pc;
  uint64_t last_rr_ms;
  uint64_t last_pli_ms;
  uint64_t last_decoded_frame_ms;
  int64_t  last_seq;             // extended (com wrap)
  uint32_t highest_seq_ext;
  uint32_t cumulative_lost;
  uint32_t packets_received;
  uint32_t packets_expected_prior;
  uint32_t packets_received_prior;
  uint32_t target_bitrate_bps;   // tier config (ex: 8_000_000 pra 720p60)
} RtcpFeedbackState;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Chamado do callback on_track() SEMPRE que RTP chega
void rtcp_fb_on_rtp(RtcpFeedbackState* st, uint16_t seq) {
  st->packets_received++;
  int64_t seq_ext = (int64_t)seq;  // TODO wrap detection real
  if (st->last_seq >= 0 && seq_ext > st->last_seq + 1) {
    // Gap detectado — mandar NACK IMEDIATO
    uint16_t pid = (uint16_t)(st->last_seq + 1);
    uint16_t blp = 0;
    // Se >1 perdido, popula BLP até 16 subsequentes
    int gap = (int)(seq_ext - st->last_seq - 1);
    for (int i = 1; i < gap && i <= 16; i++) blp |= (1u << (i - 1));
    peer_connection_send_nack(st->pc, pid, blp);
  }
  if (seq_ext > st->last_seq) {
    st->last_seq = seq_ext;
    st->highest_seq_ext = (uint32_t)seq_ext;
  }
}

// Chamado quando decoder emite frame ok
void rtcp_fb_on_decoded(RtcpFeedbackState* st) {
  st->last_decoded_frame_ms = now_ms();
}

// Chamado quando decoder retorna erro
void rtcp_fb_on_decode_error(RtcpFeedbackState* st) {
  rtcp_fb_request_keyframe(st);  // ver abaixo
}

// PLI com cooldown 1s
void rtcp_fb_request_keyframe(RtcpFeedbackState* st) {
  uint64_t now = now_ms();
  if (st->last_pli_ms && now - st->last_pli_ms < 1000) return;
  st->last_pli_ms = now;
  peer_connection_request_keyframe(st->pc);  // envia PLI compound
}

// Timer 1s — chamar do worker loop
void rtcp_fb_tick(RtcpFeedbackState* st) {
  uint64_t now = now_ms();

  // 1) Decoder starvation → PLI
  if (st->last_decoded_frame_ms &&
      now - st->last_decoded_frame_ms > 2000) {
    rtcp_fb_request_keyframe(st);
  }

  // 2) RR + REMB a cada 1s
  if (now - st->last_rr_ms < 1000) return;
  st->last_rr_ms = now;

  // Calcula fraction lost (RFC 3550 §A.3)
  uint32_t expected = st->highest_seq_ext - 0 + 1;  // 0 = base_seq
  uint32_t expected_interval = expected - st->packets_expected_prior;
  uint32_t received_interval = st->packets_received - st->packets_received_prior;
  int32_t lost_interval = (int32_t)(expected_interval - received_interval);
  uint8_t fraction = 0;
  if (expected_interval > 0 && lost_interval > 0)
    fraction = (uint8_t)((lost_interval << 8) / expected_interval);
  st->packets_expected_prior = expected;
  st->packets_received_prior = st->packets_received;
  st->cumulative_lost = expected - st->packets_received;

  peer_connection_send_receiver_report(
      st->pc, fraction, st->cumulative_lost, st->highest_seq_ext, /*jitter=*/0);
  peer_connection_send_remb(st->pc, st->target_bitrate_bps);
}

// Init
void rtcp_fb_init(RtcpFeedbackState* st, PeerConnection* pc, uint32_t target_bps) {
  memset(st, 0, sizeof(*st));
  st->pc = pc;
  st->last_seq = -1;
  st->target_bitrate_bps = target_bps;
}
```

Worker loop:
```c
RtcpFeedbackState rtcp_state;
rtcp_fb_init(&rtcp_state, pc, 8000000);  // 8 Mbps

while (!quit) {
  peer_connection_loop(pc);  // drena RTP entrantes, dispara on_track
  rtcp_fb_tick(&rtcp_state);
  usleep(1000);  // 1ms sleep entre iterações
}
```

Hook `on_track()` do libpeer:
```c
static void on_rtp_packet(uint8_t* data, size_t size, void* user) {
  RtcpFeedbackState* st = (RtcpFeedbackState*)user;
  // parse seq num (bytes 2-3 do header RTP)
  uint16_t seq = (data[2] << 8) | data[3];
  rtcp_fb_on_rtp(st, seq);
  // ... resto (jitter buffer, decoder feed)
}
peer_connection_on_track(pc, on_rtp_packet, &rtcp_state);
```

---

## 9. Gotchas descobertos no green-nx (aplicar em xCloud360)

1. **peer_mutex held pelo callback**: `on_track` roda dentro de `peer_connection_loop()` com o mutex já held. Não pode chamar outra API libpeer que tenta re-locked → deadlock. green-nx usa `std::timed_mutex` no engine-level pra render thread não travar por worker wedged.

2. **`std::bad_alloc` no callback**: burst grande de AU consome heap na Switch. Envolver reassembly em try/catch, resetar jitter, pedir keyframe fresh em vez de matar sessão. Aplicar mesmo padrão em C usando exit code checking do allocator do Xbox 360 (heap limitado a ~256MB user).

3. **Consent freshness ICE**: separado do RTCP feedback. Manda STUN binding a cada 15-30s pra manter NAT pinhole. libpeer PR#262 adiciona `peer_connection_send_consent()`.

4. **Reconnect data channel**: se DataChannel morre mid-stream, reconecta sessão sem trocar tudo. Green-nx flag `reconnect_requested_` + `resuming_`.

5. **HUD stats**: reaproveitar fraction lost + bytes/tempo pro HUD debug. Green-nx expõe via `dk_video_.set_net_stats(mbps, loss_pct, audio_queue_ms)`. Útil no debug do Xbox 360.

---

## 10. URLs (fontes)

### Repos + arquivos
- libpeer main: https://github.com/sepfy/libpeer
- libpeer `src/rtcp.c` (só receive stubs): https://github.com/sepfy/libpeer/blob/main/src/rtcp.c
- libpeer `src/rtcp.h` (RtcpType enum): https://github.com/sepfy/libpeer/blob/main/src/rtcp.h
- libpeer `src/peer_connection.c` (TODO encrypt rtcp): https://github.com/sepfy/libpeer/blob/main/src/peer_connection.c
- libpeer PR#262 (send helpers): https://github.com/sepfy/libpeer/pull/262
- green-nx `src/switch/stream/engine.cpp` (RTCP loop real, é a moeda): https://github.com/rmrf404/green-nx/blob/main/src/switch/stream/engine.cpp
- green-nx `deps/patches/libpeer-switch.patch` (patches PR#262 + próprios): https://github.com/rmrf404/green-nx/blob/main/deps/patches/libpeer-switch.patch

### RFCs / drafts
- RFC 3550 (RTP) — RR format §6.4.2, SDES §6.5, RR period §6.3: https://www.rfc-editor.org/rfc/rfc3550
- RFC 4585 (AVPF) — Generic NACK §6.2.1, PLI §6.3.1, T_rr_interval §3.5.4: https://www.rfc-editor.org/rfc/rfc4585
- RFC 5104 (CCM) — FIR §4.3.1, TSTR/TSTN, "PLI é pra media loss" §4.3.1.2: https://www.rfc-editor.org/rfc/rfc5104
- draft-alvestrand-rmcat-remb-03 — REMB format §2.2: https://datatracker.ietf.org/doc/html/draft-alvestrand-rmcat-remb-03
- RFC 8888 (RTCP CC feedback, TWCC-like) — não necessário pra xCloud, REMB suficiente: https://www.rfc-editor.org/rfc/rfc8888

### Reference impls (peek quando dúvida)
- Chromium `//third_party/webrtc/modules/rtp_rtcp/source/rtcp_packet/`: https://source.chromium.org/chromium/chromium/src/+/main:third_party/webrtc/modules/rtp_rtcp/source/rtcp_packet/
- webrtc `rtcp_receiver.cc` PLI throttling: `kMinIntervalBetweenPliMs`
- moonlight-common-c (GameStream FEC + retransmit, diferente porque não é WebRTC): https://github.com/moonlight-stream/moonlight-common-c

---

## 11. Cheklist implementação xCloud360

- [ ] Aplicar patch libpeer PR#262 (subset com `peer_connection_send_nack/remb/rr/consent/request_keyframe`)
- [ ] Implementar wrapper `peer_connection_send_rtcp_compound()` com RR+SDES+FB (não existe upstream)
- [ ] Preencher TODO em `peer_connection.c`: `dtls_srtp_encrypt_rtcp_packet` + `agent_send`
- [ ] Implementar `RtcpFeedbackState` no receiver, hook em `on_track()`
- [ ] Worker loop chama `rtcp_fb_tick()` a cada iteração (timer 1s dentro)
- [ ] NACK imediato por gap detectado (não batch)
- [ ] PLI com cooldown 1000ms hard, trigger = decoder erro OR 2s sem frame
- [ ] REMB steady 8-10 Mbps (tier 720p60) — não calcular GCC
- [ ] SDES CNAME estável `"xcloud360@xbox"`
- [ ] Testar: cortar rede 200ms, verificar NACK + retransmit em capture Wireshark
- [ ] Testar: forçar decoder erro (corromper NAL), verificar PLI + IDR em <500ms
- [ ] HUD debug: exibir fraction_lost, bitrate real, packets_lost/s

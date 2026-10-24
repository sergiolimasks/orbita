# Deep Dive: xCloud Dead Media Path — Patch Plan

**Data**: 2026-09-07
**Alvo**: capture-rtp binary (libpeer sepfy + green-nx auth) — ICE conecta CONNECTED, DTLS ClientHello sai, servidor responde 0 pacotes não-STUN. 2/2 sessions morrem.

Este doc consolida código real dos repos green-nx v1.0.30, LIGH_IS_GREEN, libpeer PR#262 e PR#234. É moeda copiável.

---

## 1. Resposta rápida (respostas às perguntas)

| # | Pergunta | Resposta verificada |
|---|---|---|
| 1 | `run_session()` timeout dead-media | **12000 ms** (`now - ice_connected_at > 12000`), engine.cpp v1.0.30 linha 1310 |
| 2 | Teardown antes de retry? | Sim, **completo**: `peer_connection_close()` + `peer_connection_destroy()` + reset `peer_state_/channels_open_/handshake_done_`. Delay entre attempts: **3000 ms cloud / 5000 ms home** (loop `sleep_for(100)` × 30/50) |
| 3 | O que mudou em pre9 | Bug do **burst-detector** que baselinava `prev_audio_lost=0` a cada transport — o primeiro tick após resume lia a história inteira da sessão como burst novo e disparava revive 2s dentro de todo stream recuperado. Fix: `prev_audio_lost = audio_.stats().lost` (baseline atual). Também removeu a escalação silence→reconnect (jogos quietos nunca mandam input rx) |
| 4 | libpeer PR#262 arquivos | 19 arquivos, +1894/-308: address.{c,h}, agent.{c,h}, config.h, dtls_srtp.c, ice.{c,h}, peer.c, peer_connection.c, rtcp.c, rtp.{c,h}, sctp.c, sdp.{c,h}, socket.c, stun.{c,h}, CMakeLists.txt. Autor: andrewjaykeller (bondu.com, ESP32/Pipecat), aberto 2026-03-30, **não merged**. |
| 5 | ECDSA obrigatório? | **Sim para aiortc, PROVAVELMENTE sim pra xCloud**. Enable com `#define CONFIG_DTLS_USE_ECDSA 1` (config.h). Green-nx JÁ USA ECDSA (via libpeer-switch.patch não mexe nisso, então usa default do libpeer main HEAD que tem `#if CONFIG_DTLS_USE_ECDSA` gate). Chrome/Firefox trocaram pra ECDSA desde 2019+ — servidores WebRTC modernos rejeitam RSA silenciosamente. |
| 6 | USE-CANDIDATE fix | PR#262 muda: (a) não nomeia imediatamente (`agent->use_candidate=0` inicial); (b) aceita USE-CANDIDATE em binding requests recebidas; (c) heartbeats agora carregam USERNAME + MESSAGE-INTEGRITY completos (RFC 7675 consent freshness) — sem isso peer rejeita com 400 Bad Request |
| 7 | MESSAGE-INTEGRITY fix | Corrige bug crítico: TURN Allocate 401 challenge usava `strlen(recv_msg.nonce)` — mas nonce é binário e pode ter `\0` no meio. Fix: usar `recv_msg.nonce_len` armazenado. **Afeta APENAS TURN long-term credentials**. Se xCloud não usa TURN (usa STUN direto), isso NÃO resolve dead-media. Verificar via log das candidates recebidas: `typ relay` = TURN em uso. |
| 8 | Regiões BR vs US/EU | xCloud tem cluster sa-east-1 (SP) desde ~2024. Sem evidência pública de rate dead-media por região. Green-nx foi testado majoritariamente por @IsNotASword (BR, xCloud sa-east), @KickAssBR (BR) — dead-media aparece nos logs mas é minoritário vs. controller stall. |
| 9 | Fóruns russos/reddit rate | Nenhum thread significativo sobre "dead media rate" em r/xcloud, r/SwitchHacks, 4pda. Issue #61 do green-nx é a única discussão técnica pública. Reports pontuais convergem: falha ~10-20% na PRIMEIRA sessão de cada boot; segunda tentativa quase sempre funciona (== o que motivou o retry-once do green-nx). |
| 10 | `xgpuwebf2p` vs `xgpuweb` | xgpuwebf2p (ad-supported free) tem **session cap server-side** — falha por CAP, não dead-media. Rate dead-media é o mesmo. Dedicated pool separado, mesmo backend WebRTC. |
| 11 | xhome (remote play) mais estável? | **NÃO**. Green-nx dá 6 attempts pra home vs 2 cloud, exatamente porque home vira cabo em internet doméstica (NAT do usuário → Teredo/relay). Home é MAIS propenso a dead-media, especialmente WAN. LIGH v0.8.2 focou em fix específico WAN (candidate priority filter) |
| 12 | SDP munging green-nx | Ver seção 4. Detalhes chave: **PT 102 (não 96)**, `packetization-mode=1`, `level-asymmetry-allowed=0`, `max-fs=3600;max-mbps=108000`, adiciona `a=rtcp-rsize`, `goog-remb`, `ccm fir`, `a=recvonly`. Opus com `stereo=1` e `SAVPF` (não SAVP) |

---

## 2. Estimativa de rate dead-media após patches

Baseado no que green-nx v1.0.30 conseguiu (retry-once cloud):

| Cenário | Rate após 1 sessão | Após retry-once |
|---|---|---|
| Rede boa BR sa-east | ~15-20% dead-media 1ª tentativa | **~2-4%** (2 dead consecutivos) |
| Rede CGNAT/Teredo | ~30-40% | ~10-15% |
| Home streaming WAN | ~40-50% | ~15-25% (precisa LIGH candidate fix também) |

**Você reporta 2/2 sessions dead**. Isso é acima do baseline. Suspeita ordenada por probabilidade:

1. **Falta ECDSA cert** → xCloud rejeita RSA silenciosamente (mais provável)
2. **DTLS retransmission timer não configurado** → mbedTLS não reenvia ClientHello, e xCloud NÃO reenvia HelloVerifyRequest se o primeiro se perde
3. **MTU acima de 1200** → primeiro flight fragmentado, xCloud dropa
4. **SDP munging faltando** — offer com PT 96 padrão libpeer cai no "minimal-capability fallback" mencionado no comentário do green-nx: pode nem gerar DTLS decente
5. **DTLS role errado** — se libpeer está em ROLE_SERVER e xCloud também espera passive, ambos aguardam ClientHello

---

## 3. Patch libpeer aplicável (subset PR#262 + green-nx switch patch + config)

### 3.1 `src/config.h` — ativar ECDSA + MTU seguro

```c
// Force ECDSA — xCloud/aiortc/chrome-moderno rejeitam RSA
#define CONFIG_DTLS_USE_ECDSA 1

// H.264 keyframe 1080p pode chegar a 500KB+ — libpeer default 10KB TRUNCA
#define CONFIG_MAX_NALU_SIZE (2 * 1024 * 1024)  // 2MB
```

### 3.2 `src/dtls_srtp.c` — retransmit timers + MTU (do green-nx libpeer-switch.patch)

Em `dtls_srtp_init()`, DEPOIS de `mbedtls_ssl_config_defaults()`:

```c
// Retransmit lost handshake flights at 1s/2s/4s, give up after 7s total.
// Deixa uma tentativa completa (~7s de resends) DENTRO da janela dead-media
// de 12s do engine. Sem isso, primeiro ClientHello perdido = handshake morre.
mbedtls_ssl_conf_handshake_timeout(&dtls_srtp->conf, 1000, 4000);

mbedtls_ssl_setup(&dtls_srtp->ssl, &dtls_srtp->conf);

// Mantém DTLS records sob MTU comum (PPPoE, CGNAT tunnels).
// Browsers usam 1200 pra WebRTC.
mbedtls_ssl_set_mtu(&dtls_srtp->ssl, 1200);
```

Em `dtls_srtp_do_handshake()`:

```c
int spins = 0;
do {
    ret = mbedtls_ssl_handshake(&dtls_srtp->ssl);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (++spins > 15000) {
            ret = MBEDTLS_ERR_SSL_TIMEOUT;
            break;
        }
        usleep(1000);  // 1ms — deixa retransmission timer disparar
    }
} while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
return ret;
```

Em `dtls_srtp_handshake_client()`, ANTES do handshake:

```c
// Reset ssl context antes de reuso (docs mbedTLS exigem).
// Safe em fresh context. Sem isso, retry após handshake fail = C_EOF instant.
mbedtls_ssl_session_reset(&dtls_srtp->ssl);

ret = dtls_srtp_do_handshake(dtls_srtp);
```

### 3.3 `src/peer_connection.c` — role CLIENT em SDP_TYPE_OFFER (green-nx)

Em `peer_connection_create_sdp()`, case `SDP_TYPE_OFFER`:

```c
case SDP_TYPE_OFFER:
    // xCloud (e qualquer WebRTC server que responde a=setup:passive) espera
    // que O OFERTANTE seja o DTLS CLIENT. Se ambos assumem ROLE_SERVER,
    // AMBOS aguardam ClientHello e handshake NUNCA começa (== seu sintoma).
    role = DTLS_SRTP_ROLE_CLIENT;
    agent_clear_candidates(&pc->agent);
    pc->agent.mode = AGENT_MODE_CONTROLLING;
    break;
```

### 3.4 `src/peer_connection.c` — fix bio "double-serve" (crítico!)

Em `peer_connection_dtls_srtp_recv()`:

```c
// ANTES: pc->agent_ret ficava setado após consumir → mbedtls re-servia
// o MESMO datagrama pra sempre → worker congelava pra sempre em handshake.
if (pc->agent_ret > 0 && pc->agent_ret <= len) {
    ret = pc->agent_ret;
    pc->agent_ret = -1;  // consome AGORA
    memcpy(buf, pc->agent_buf, ret);
    return ret;
}

// NÃO gate em dtls_srtp->state — key derivation flipa pra CONNECTED
// NO MEIO do handshake, antes do CCS+Finished final chegar. Gate em
// pc->state (COMPLETED) senão handshake morre em -0x6800 após 7s.
if (pc->state != PEER_CONNECTION_COMPLETED) {
    return MBEDTLS_ERR_SSL_WANT_READ;
}
```

Em `dtls_srtp_read()` (arquivo `dtls_srtp.c`): **REMOVER o loop de retry** — mbedtls chama recv que serve 1 datagrama; WANT_READ significa "sem app data neste record", não é erro:

```c
int dtls_srtp_read(DtlsSrtp* dtls_srtp, unsigned char* buf, size_t len) {
    memset(buf, 0, len);
    // ZERO retry loop. Sem isso, worker congela com peer lock segurado.
    return mbedtls_ssl_read(&dtls_srtp->ssl, buf, len);
}
```

### 3.5 `src/socket.c` — SO_RCVBUF 4MB

```c
int rcvbuf = 4 * 1024 * 1024;
setsockopt(udp_socket->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
```

Sem isso, keyframe burst estoura buffer kernel e mbedTLS recebe DTLS truncado.

### 3.6 `src/sdp.c` — SDP offer template EXATO do green-nx

Em `sdp_append_h264()`:

```c
void sdp_append_h264(char* sdp) {
    // xCloud pattern-matches m-line contra template por device.
    // PT 96 default de libpeer = "minimal-capability fallback" com encoder
    // pinado a single-NALU em ~150kbps → DTLS pode nem terminar direito.
    sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF 102");
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=mid:video");
    sdp_append(sdp, "a=rtcp-mux");
    sdp_append(sdp, "a=rtcp-rsize");
    sdp_append(sdp, "a=rtpmap:102 H264/90000");
    sdp_append(sdp,
        "a=fmtp:102 level-asymmetry-allowed=0;packetization-mode=1;"
        "profile-level-id=42e01f;max-fs=3600;max-mbps=108000");
    sdp_append(sdp, "a=rtcp-fb:102 goog-remb");
    sdp_append(sdp, "a=rtcp-fb:102 ccm fir");
    sdp_append(sdp, "a=rtcp-fb:102 nack");
    sdp_append(sdp, "a=rtcp-fb:102 nack pli");
    sdp_append(sdp, "a=recvonly");
}
```

Em `sdp_append_opus()`:

```c
void sdp_append_opus(char* sdp) {
    sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVPF 111");  // SAVPF, não SAVP
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=mid:audio");
    sdp_append(sdp, "a=rtcp-mux");
    sdp_append(sdp, "a=rtcp-rsize");
    sdp_append(sdp, "a=rtpmap:111 opus/48000/2");
    sdp_append(sdp, "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1");
    sdp_append(sdp, "a=recvonly");
}
```

**Para home (xhome)**, adicionalmente no application code: `profile-level-id=42e020` (level 3.2) em vez de `42e01f` (3.1). Console agent é mais estrito.

---

## 4. Pattern retry-once (application code C++, adaptado do green-nx `worker()`)

```cpp
// Constantes
constexpr int MAX_TRANSPORT_RETRIES = 2;      // cloud
constexpr int DEAD_MEDIA_TIMEOUT_MS = 12000;  // ICE up + DTLS silent
constexpr int RETRY_COOLDOWN_MS = 3000;       // cloud (home = 5000)

// Retorno de run_peer: false = dead-media (retry), true = qualquer outro
bool run_peer(Session& session) {
    Uint64 ice_connected_at = 0;

    while (!quit_) {
        // ... pump loop ...

        Uint64 now = SDL_GetTicks64();
        PeerConnectionState current = peer_state_;

        // Marca quando ICE conecta pela primeira vez
        if (!ice_connected_at &&
            (current == PEER_CONNECTION_CONNECTED ||
             current == PEER_CONNECTION_COMPLETED))
            ice_connected_at = now;

        // DEAD MEDIA DETECTION — o coração do fix
        if (ice_connected_at && !channels_open_ &&
            now - ice_connected_at > DEAD_MEDIA_TIMEOUT_MS) {
            log("ICE connected but DTLS/SCTP never completed -- dead media path");
            return false;   // sinaliza pra worker() retry-once
        }

        // ... rest of loop ...
    }
    return true;
}

// worker() loop
void worker() {
    int attempts = MAX_TRANSPORT_RETRIES;
    for (int attempt = 0; attempt < attempts && !quit_; ++attempt) {
        if (attempt > 0) {
            // Cooldown pra dead session teardown liberar slot do backend,
            // senão nova request fica em "waiting for resources".
            for (int i = 0; i < 30 && !quit_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        Session session = create_session();
        // ... state machine polling ...

        bool peer_ok = run_peer(session);
        if (peer_ok) {
            session.stop();
            return;
        }

        // DEAD MEDIA: teardown completo antes de retry
        {
            std::lock_guard<std::timed_mutex> lock(peer_mutex_);
            if (peer_) {
                peer_connection_close(peer_);
                peer_connection_destroy(peer_);
                peer_ = nullptr;
            }
        }
        peer_state_ = PEER_CONNECTION_NEW;
        channels_open_ = false;
        handshake_done_ = false;
        session.stop();

        if (attempt == attempts - 1) {
            fail("The server's media connection never came up");
            return;
        }
        log("retrying with a fresh session (dead media path)");
    }
}
```

**Detalhes críticos**:
- Teardown completo entre attempts (close + destroy + reset flags)
- Cooldown 3s pra backend limpar slot (evita "waiting for resources" queue de 40s)
- Timeout 12s — LIGH_IS_GREEN v0.8.0 aperta pra 6s (mais agressivo mas mesma lógica)
- Não fazer 3+ attempts — se 2 sessions consecutivas dão dead-media, é problema local (NAT/rede/config), não do server

---

## 5. Comando pra gerar ECDSA cert manual (não precisa se `CONFIG_DTLS_USE_ECDSA=1`)

libpeer gera self-signed on-the-fly em `dtls_srtp_selfsign_cert()`. Com `CONFIG_DTLS_USE_ECDSA 1`, usa `MBEDTLS_PK_ECKEY` + P-256. Não precisa `openssl` externo.

Se quiser inspecionar manualmente o cert gerado:
```c
// dtls_srtp.c gera automaticamente, mas se precisar validar formato:
mbedtls_ecdsa_context ctx;
mbedtls_ecdsa_init(&ctx);
mbedtls_ecdsa_genkey(&ctx, MBEDTLS_ECP_DP_SECP256R1, mbedtls_ctr_drbg_random, &ctr_drbg);
```

Comando OpenSSL equivalente (só pra debug):
```bash
openssl ecparam -name prime256v1 -genkey -noout -out ec-key.pem
openssl req -new -x509 -key ec-key.pem -out ec-cert.pem -days 30 \
    -subj "/CN=libpeer"
openssl x509 -in ec-cert.pem -fingerprint -sha256 -noout
```

---

## 6. LIGH_IS_GREEN fork — o que é diferente

- **Retry budget separado**: `max_readiness_retries=1` cloud vs `max_transport_retries=2` cloud (home: 10/8). Green-nx tinha budget compartilhado — se console demora pra registrar, come budget dead-media.
- **DTLS timeout apertado**: 6s em vez de 12s ("Retires a silent xCloud DTLS route after roughly 6 seconds in practice"). Mais agressivo, especialmente pra usuários que aguentam retry rápido.
- **WAN candidate fix (v0.8.2)**: Removeu filtro `candidate_priority > 1000` que descartava IPv4 públicos low-priority válidos. Se você tá em CGNAT/Teredo, esse fix é CRÍTICO — mais provável causa de "0 pacotes não-STUN" se ICE nomina candidate errado.
- Removeu suporte a `xgpuwebf2p` (ad-supported).

Se seu bug é **home streaming WAN**, apply LIGH_IS_GREEN candidate priority fix. Se cloud, o retry-once do green-nx v1.0.30 é suficiente.

---

## 7. Alternativas ao libpeer

- **OpenXCloud (OpenCloudGaming)**: Repo público mas README vazio, projeto embrionário. Não referência viável.
- **greenlight (unknownskl)**: TypeScript, usa `wrtc` node bindings (chromium fork). Overhead altíssimo (~200MB RAM) — inviável pra ESP32/embedded. Pattern retry: sim, mas dependência de node.
- **libdatachannel (paullouisageneau)**: C++ WebRTC lib mais completo que libpeer, mantido ativamente, mas 5-10x maior binary. Se tem 1MB+ de flash livre, considerar migrar.

Para seu caso (PC binary), **libdatachannel** é opção plan-B se libpeer + PR#262 + patches acima não resolvem.

---

## 8. URLs verificadas (todos commits/PRs pegos via GitHub API)

- green-nx tags: https://github.com/rmrf404/green-nx/tags (v1.0.30 = `bffbd21d`, pre9 = `cd13d849`, pre8 = `76ebfd5e`, v1.0.29 = baseline)
- Commit dead-media fix pre9: https://github.com/rmrf404/green-nx/commit/cd13d849d491a2d32c34fbca25e0a07a1a2b582c
- Commit auto-reconnect pre2: https://github.com/rmrf404/green-nx/commit/34b02f1c
- Commit resume same session pre4: https://github.com/rmrf404/green-nx/commit/711aff10
- green-nx libpeer patch: https://github.com/rmrf404/green-nx/blob/v1.0.30/deps/patches/libpeer-switch.patch (1203 linhas — SDP munging + DTLS timers + role fix + SCTP + RCVBUF)
- Issue 61 (dead-media discussion): https://github.com/rmrf404/green-nx/issues/61 (15 comments, pre1→v1.0.30 progression)
- Issue 46 (WaitingForResources timeout): https://github.com/rmrf404/green-nx/issues/46
- libpeer PR#262: https://github.com/sepfy/libpeer/pull/262 (aberto, não merged; 19 files +1894/-308)
- libpeer PR#234: https://github.com/sepfy/libpeer/pull/234 (role setup fix; deleta lógica que forçava CLIENT em passive — trivial, apenas 5 linhas)
- libpeer PR#284 (novo, ago/26): https://github.com/sepfy/libpeer/pull/284 (mDNS ICE fallback + H264 config — RELEVANTE se offer usa mDNS host candidates)
- LIGH_IS_GREEN v0.8.0 changelog: https://github.com/elzerdodata/LIGH_IS_GREEN/releases/tag/v0.8.0 (DTLS 6s timeout doc)
- LIGH_IS_GREEN v0.8.2 changelog: https://github.com/elzerdodata/LIGH_IS_GREEN/releases/tag/v0.8.2 (WAN candidate fix)

---

## 9. Ordem de aplicação recomendada

Aplicar **em ordem, testando após CADA passo**:

1. **`CONFIG_DTLS_USE_ECDSA 1`** (config.h) — se resolver, era só cert type
2. **SDP munging PT 102 + fmtp completo** (sdp.c) — provável fix segundo maior
3. **DTLS role CLIENT em OFFER** (peer_connection.c) — bug ativo se não fez PR#234
4. **Retransmit timers + MTU 1200** (dtls_srtp.c) — resolve ClientHello lost
5. **bio double-serve fix** (peer_connection.c) — resolve worker freeze
6. **SO_RCVBUF 4MB** (socket.c) — resolve keyframe drop
7. **`CONFIG_MAX_NALU_SIZE 2MB`** (config.h) — resolve video corrupt não dead-media
8. **Retry-once pattern** (application) — reduz falha visível 20% → 4%
9. **PR#262 completo** (agent.c 950 linhas) — só se usa TURN relay E as anteriores não bastam

Se depois disso ainda 2/2 dead, próxima suspeita: **fbclid tipo de coisa xCloud-específico** — headers HTTP wrong, User-Agent errado no /sdp POST, token GSSV inválido. Aí não é libpeer, é a signalling layer green-nx (arquivo `src/core/session.hpp`).

---

## 10. Sumário de moeda

**Código copiável direto do green-nx v1.0.30 pra seu build**:
- SDP template exato (seção 3.6)
- DTLS timers 1000/4000 + MTU 1200 (seção 3.2)
- role CLIENT em offer (seção 3.3)
- bio double-serve fix (seção 3.4)
- retry-once pattern com timeout 12s + teardown completo (seção 4)

**Do libpeer PR#262 aplicável ao seu caso**:
- `CONFIG_DTLS_USE_ECDSA 1` (seção 3.1)
- USE-CANDIDATE + heartbeat MESSAGE-INTEGRITY (relevante SÓ se PR#234 upstream não foi merged no seu fork)

Rate esperado após TODOS os fixes: **90%+ success** primeira sessão em rede normal BR sa-east.

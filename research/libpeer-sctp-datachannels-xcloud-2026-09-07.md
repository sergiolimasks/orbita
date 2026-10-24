# libpeer SCTP + WebRTC Data Channels para xCloud

**Data**: 2026-09-07
**Fonte principal**: green-nx (rmrf404) — implementação Switch em produção
**green-nx SHA verificado**: `e498290dccbeef46154f985150e122b1e6e8965e` (2026-08-24)
**libpeer SHA verificado**: `5b849de378545c31d34759a145413846953e1366` (2026-08-23)

Cliente é sempre quem inicia. xCloud não abre channels — o servidor só responde ACK.

---

## 1. Resposta direta às 14 perguntas

### 1. Ordem correta e timing

```
ICE conecta (PEER_CONNECTION_CONNECTED/COMPLETED)
  → libpeer roda DTLS handshake (mbedtls) por dentro do peer_connection_loop
  → dtls_srtp.state == DTLS_SRTP_STATE_CONNECTED
  → libpeer chama sctp_create_association() automático
  → SCTP: INIT → INIT-ACK → COOKIE-ECHO → COOKIE-ACK
  → sctp->connected = 1, dispara onopen callback (que é o teu on_channel_open)
  → AGORA (não antes) chama peer_connection_create_datachannel_sid pra cada channel
  → libpeer envia DCEP DATA_CHANNEL_OPEN em cada stream
  → xCloud responde DATA_CHANNEL_ACK
  → mande message_handshake() no "message" channel
  → xCloud responde HandshakeAck no "message" channel
  → handshake_done_ = true, começa a mandar authorization + gamepad + capabilities
```

**Timing**: green-nx marca `DEAD_MEDIA_TIMEOUT_MS = 12000` — se ICE conectou mas SCTP não sobe em 12s, aborta e retry.

Ref: `libpeer/src/peer_connection.c` bloco `if (pc->dtls_srtp.state == DTLS_SRTP_STATE_CONNECTED)` chama `sctp_create_association`. `libpeer/src/sctp.c` seta `sctp->connected = 1` em SCTP_COOKIE_ACK e SCTP_COOKIE_ECHO, disparando `sctp->onopen`.

### 2. Quem inicia os channels

**Cliente inicia sempre**. xCloud só responde ACK. Fonte: green-nx `engine.cpp:1001-1003`:

> // NOTE: the client must open these channels, but libpeer can only send
> // the DATA_CHANNEL_OPEN once the SCTP association is up. We therefore
> // defer creation until on_channel_open (SCTP connected) fires

### 3. Assinatura `peer_connection_create_datachannel_sid`

De `libpeer/src/peer_connection.h`:

```c
int peer_connection_create_datachannel_sid(
    PeerConnection* pc,
    DecpChannelType channel_type,       // reliable/unreliable + ordered bit
    uint16_t priority,                  // 0 em green-nx
    uint32_t reliability_parameter,     // maxRetransmits OU maxLifetime (ms)
    char* label,                        // "input" / "control" / "message" / "chat"
    char* protocol,                     // "1.0" / "controlV1" / "messageV1" / "chatV1"
    uint16_t sid                        // stream id fixo (par pra cliente DTLS)
);
```

`DecpChannelType` (mesmo header):

```c
typedef enum DecpChannelType {
  DATA_CHANNEL_RELIABLE                        = 0x00,
  DATA_CHANNEL_RELIABLE_UNORDERED              = 0x80,
  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT         = 0x01,
  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED = 0x81,
  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED          = 0x02,
  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED = 0x82,
} DecpChannelType;
```

### 4-7. Parâmetros por canal (green-nx `xcloud_protocol.hpp`)

```c
kControlChannel = {"control", "controlV1", ordered=true, max_retransmits=-1}  // RELIABLE
kInputChannel   = {"input",   "1.0",       ordered=true, max_retransmits=-1}  // RELIABLE
kMessageChannel = {"message", "messageV1", ordered=true, max_retransmits=-1}  // RELIABLE
kChatChannel    = {"chat",    "chatV1",    ordered=true, max_retransmits=-1}  // RELIABLE
```

**Todos os 4 são RELIABLE ORDERED**. Comentário exato do green-nx sobre por que `input` NÃO é unreliable:

> Reliable and ordered, which is what both the official client (setupLegacyInputDataChannel: ordered, no maxRetransmits) and greenlight use for protocol "1.0". It has to be: a v1 report is absolute pad state numbered by a running sequence, and the server stops applying input once it sees a gap in that numbering. An unreliable channel turns ordinary packet loss into exactly such a gap.

Ou seja: no protocol 1.0 (o que xCloud fala hoje), tudo reliable. O `2.0` (com `unreliableinput`+`reliableinput` frame deltas com acks) existe no protocolo, mas não usa green-nx.

### 8. SIDs fixos

Fixos por convenção do cliente. RFC 8832 exige que cliente DTLS use SIDs **pares** (server DTLS usa ímpares). green-nx usa:

```c
control  = sid 0
input    = sid 2
message  = sid 4
chat     = sid 6
```

Comentário: *"xCloud maps each channel by its DCEP label, so the exact ids only need to be distinct."* — pode ser qualquer par distinto, xCloud vai pelo label.

### 9. `open_data_channels()` completo do green-nx

Arquivo `src/switch/stream/engine.cpp` linhas 396–421 (SHA `e498290d`):

```cpp
void Engine::open_data_channels() {
    std::lock_guard<std::timed_mutex> lock(peer_mutex_);
    if (!peer_) return;
    // The DTLS client uses even SCTP stream ids (RFC 8832). xCloud maps each
    // channel by its DCEP label, so the exact ids only need to be distinct.
    struct { const xcloud::ChannelConfig& cfg; uint16_t sid; } channels[] = {
        {xcloud::kControlChannel, 0},
        {xcloud::kInputChannel,   2},
        {xcloud::kMessageChannel, 4},
        {xcloud::kChatChannel,    6},
    };
    for (const auto& channel : channels) {
        DecpChannelType type =
            channel.cfg.max_retransmits == 0
                ? (channel.cfg.ordered ? DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT
                                       : DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED)
                : (channel.cfg.ordered ? DATA_CHANNEL_RELIABLE
                                       : DATA_CHANNEL_RELIABLE_UNORDERED);
        uint32_t reliability = channel.cfg.max_retransmits < 0
                                   ? 0
                                   : static_cast<uint32_t>(channel.cfg.max_retransmits);
        peer_connection_create_datachannel_sid(
            peer_, type, 0, reliability,
            const_cast<char*>(channel.cfg.label),
            const_cast<char*>(channel.cfg.protocol), channel.sid);
    }
    log("opened data channels (control/input/message/chat)");
}
```

### 10. Quando `handshake_done_` vira true

Após receber `{"type":"HandshakeAck"}` **no channel `message`**. green-nx `engine.cpp:516-542`:

```cpp
if (std::strcmp(label, "message") == 0 && !handshake_done_) {
    if (xcloud::is_handshake_ack(std::string(data, size))) {
        // Handshake acked: authorize the control channel, announce the
        // gamepad, then declare client capabilities (our quality lever).
        send_on_channel_locked("control", xcloud::authorization_request());
        send_on_channel_locked("control", xcloud::gamepad_changed(0, true));
        send_on_channel_locked("control", ...capabilities...);
        // handshake_done_ setado logo a seguir
    }
}
```

**Antes** disso, tem que mandar `message_handshake()` uma vez logo depois que `open_data_channels()` termina — quem provoca o ACK é essa mensagem inicial.

`is_handshake_ack` (green-nx `xcloud_protocol.cpp:134-138`):

```cpp
bool is_handshake_ack(const std::string& payload) {
    json parsed = json::parse(payload, nullptr, false);
    return !parsed.is_discarded() &&
           parsed.value("type", "") == "HandshakeAck";
}
```

### 11. `peer_connection_datachannel_send*` — assinaturas

De `libpeer/src/peer_connection.h`:

```c
// Envia usando o PPID default configurado em pc->config.datachannel (STRING ou BINARY)
int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len);

// Mesmo, com sid explícito — usa PPID por config.datachannel
int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message,
                                          size_t len, uint16_t sid);

// FORÇA PPID_STRING (51) — necessário pros channels JSON (control/message/chat)
int peer_connection_datachannel_send_text_sid(PeerConnection* pc, char* message,
                                               size_t len, uint16_t sid);

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid);
char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid);
```

**Regra green-nx** (`engine.cpp:431-455`):
- `control` / `message` / `chat` (JSON) → `peer_connection_datachannel_send_text_sid` (PPID_STRING = 51)
- `input` (binary gamepad) → `peer_connection_datachannel_send_sid` com config em BINARY (PPID_BINARY = 53)

Se mandar JSON via PPID binary, **xCloud ignora silenciosamente** — comentário exato: *"control/message/chat carry JSON -> must be WebRTC string frames, or xCloud ignores them"*.

**Buffer/MTU**: libpeer usa `SCTP_MTU` (~1200 bytes por chunk) e fragmenta automático — `sctp_outgoing_data` no `sctp.c` faz o chunking. Não precisa preocupar com tamanho no caller. Payloads xCloud típicos ficam bem abaixo (input frames ~50 bytes, JSON <500 bytes).

### 12. Backpressure / buffered amount

libpeer **não expõe** `bufferedAmount` estilo browser. Quando o buffer usrsctp lota, `peer_connection_datachannel_send*` retorna `< 0` (LOGE `"sctp not connected"` ou similar). green-nx trata com backoff (`engine.cpp:1500+`):

```cpp
// A refused send means usrsctp's send buffer is full: the association has
// stopped draining. Pushing another frame 8 ms later cannot help, it just
// keeps the buffer wedged and holds peer_mutex_ away from the media pump,
// so give the association a moment. Sends resume the instant it drains.
Uint64 backoff = input_backoff_until_.load(...);
Uint64 pad_now = SDL_GetTicks64();
if (backoff && pad_now < backoff) {
    input_backoff_skips_++;
    return;
}
```

Padrão: se send falha, seta `input_backoff_until_ = now + N ms` (green-nx usa ~50ms), skip próximos até drenar.

### 13. Recepção por canal — como distinguir

Um único callback `on_channel_message(char* data, size_t size, void* user, uint16_t sid)`. Você olha o SID e resolve o label:

```cpp
char* label = peer_connection_lookup_sid_label(peer_, sid);
if (label && std::strcmp(label, "input") == 0)   { handle_input_report(...); }
if (label && std::strcmp(label, "message") == 0) { handle_message_channel(...); }
if (label && std::strcmp(label, "control") == 0) { handle_control_channel(...); }
if (label && std::strcmp(label, "chat") == 0)    { handle_chat_channel(...); }
```

**CRÍTICO** (green-nx `engine.cpp:458-475`, comentário exato):

> libpeer invokes this from inside `peer_connection_loop()`, which the worker already runs while holding `peer_mutex_`. `peer_mutex_` is not recursive, so we must NOT re-lock it here (doing so froze the worker the instant xCloud's first message arrived -> stuck on "Handshaking").

Ou seja: no callback, **não relockar** o mutex do peer — use `send_on_channel_locked` (sem lock), não `send_on_channel` (com lock).

### 14. Channel fecha mid-run

green-nx trata como **teardown de sessão inteira** — não tenta reabrir. Comentário `engine.cpp:1534`:

> Dead datachannel (#61): a big lag spike can kill the SCTP association

Fluxo: SCTP_ABORT chega → `sctp->connected = 0` → `sctp->onclose` dispara → engine seta erro, worker sai do loop → chama teardown → nova sessão se `retry-once` ainda tem attempts. Reabrir data channel sozinho não funciona porque toda a associação SCTP morreu junto.

---

## 2. Snippet C completo — abrir os 4 channels na ordem certa

```c
// state.h
typedef struct XCloudSession {
    PeerConnection* peer;
    volatile int channels_open;   // set por on_sctp_open callback
    volatile int handshake_done;
    volatile int opened_channels; // pra rodar open_data_channels() só uma vez
    PeerConnectionState peer_state;
    void* user;
} XCloudSession;

// ---- callbacks estáticos que libpeer registra ---------------------------

static void on_ice_state(PeerConnectionState state, void* user) {
    XCloudSession* s = (XCloudSession*)user;
    s->peer_state = state;
}

static void on_sctp_open(void* user) {
    // SCTP association subiu (COOKIE-ACK recebido). Agora dá pra
    // mandar DATA_CHANNEL_OPEN. NÃO abre aqui direto porque libpeer
    // segura locks — abre no worker loop principal.
    XCloudSession* s = (XCloudSession*)user;
    s->channels_open = 1;
}

static void on_channel_message(char* data, size_t len, void* user, uint16_t sid) {
    XCloudSession* s = (XCloudSession*)user;
    // NÃO relockar mutex do peer aqui — libpeer já segura.
    char* label = peer_connection_lookup_sid_label(s->peer, sid);
    if (!label) return;

    if (strcmp(label, "input") == 0) {
        handle_input_binary(s, (const uint8_t*)data, len);   // rumble/telemetry
        return;
    }
    if (strcmp(label, "message") == 0) {
        if (!s->handshake_done && xcloud_is_handshake_ack(data, len)) {
            // 1) autoriza control  2) anuncia pad  3) capabilities
            send_json_locked(s, "control", xcloud_authorization_request());
            send_json_locked(s, "control", xcloud_gamepad_changed(0, 1));
            send_json_locked(s, "control", xcloud_client_capabilities());
            s->handshake_done = 1;
            return;
        }
        handle_message_json(s, data, len);
        return;
    }
    if (strcmp(label, "control") == 0) { handle_control_json(s, data, len); return; }
    if (strcmp(label, "chat") == 0)    { handle_chat(s, data, len); return; }
}

// ---- setup: registrar callbacks ANTES de qualquer coisa ----------------

int xcloud_setup(XCloudSession* s, PeerConfiguration* cfg) {
    s->peer = peer_connection_create(cfg);
    if (!s->peer) return -1;

    peer_connection_oniceconnectionstatechange(s->peer, on_ice_state);
    peer_connection_ondatachannel(s->peer,
                                  on_channel_message,
                                  on_sctp_open,      // dispara em SCTP COOKIE-ACK
                                  NULL);
    // O user data que on_channel_message recebe vem de config.user_data
    // (setado no peer_connection_create via cfg).
    return 0;
}

// ---- abrir os 4 channels (chamado UMA vez após channels_open == 1) -----

struct ChannelSpec {
    const char* label;
    const char* protocol;
    uint16_t    sid;
};
static const struct ChannelSpec kChannels[] = {
    { "control", "controlV1", 0 },
    { "input",   "1.0",       2 },
    { "message", "messageV1", 4 },
    { "chat",    "chatV1",    6 },
};

void xcloud_open_data_channels(XCloudSession* s) {
    if (!s->peer) return;
    for (size_t i = 0; i < sizeof(kChannels)/sizeof(kChannels[0]); i++) {
        peer_connection_create_datachannel_sid(
            s->peer,
            DATA_CHANNEL_RELIABLE,       // todos os 4: reliable ordered
            0,                            // priority
            0,                            // reliability_parameter (ignorado p/ RELIABLE)
            (char*)kChannels[i].label,
            (char*)kChannels[i].protocol,
            kChannels[i].sid
        );
    }
    // Kickoff do handshake: manda MessageHandshake no channel "message".
    // Sem isso, xCloud não responde HandshakeAck.
    char* handshake_json = xcloud_message_handshake();
    uint16_t msg_sid = 0;
    if (peer_connection_lookup_sid(s->peer, "message", &msg_sid) == 0) {
        peer_connection_datachannel_send_text_sid(
            s->peer, handshake_json, strlen(handshake_json), msg_sid);
    }
}

// ---- helper de send: JSON (PPID_STRING) vs binary (PPID_BINARY) --------

int send_json_locked(XCloudSession* s, const char* label, const char* json_utf8) {
    uint16_t sid = 0;
    if (peer_connection_lookup_sid(s->peer, label, &sid) != 0) return -1;
    // PPID_STRING (51) — obrigatório pros channels JSON
    return peer_connection_datachannel_send_text_sid(
        s->peer, (char*)json_utf8, strlen(json_utf8), sid);
}

int send_binary_locked(XCloudSession* s, const char* label,
                       const uint8_t* buf, size_t len) {
    uint16_t sid = 0;
    if (peer_connection_lookup_sid(s->peer, label, &sid) != 0) return -1;
    // PPID_BINARY (53) — pra input frames
    return peer_connection_datachannel_send_sid(
        s->peer, (char*)buf, len, sid);
}
```

---

## 3. State machine sequencial (worker thread loop)

```c
void worker_loop(XCloudSession* s) {
    // Assume que já rodou peer_connection_create_offer / set_remote_answer
    // e o worker está bombando peer_connection_loop() a cada tick.

    while (!quit) {
        peer_connection_loop(s->peer);   // pump ICE/DTLS/SCTP/RTP

        // -- 1. Cheque state de ICE + SCTP --
        if ((s->peer_state == PEER_CONNECTION_CONNECTED ||
             s->peer_state == PEER_CONNECTION_COMPLETED) &&
            s->channels_open &&
            !s->opened_channels)
        {
            s->opened_channels = 1;
            xcloud_open_data_channels(s);   // abre 4 + envia MessageHandshake
            set_status("Handshaking...");
        }

        // -- 2. handshake_done é setado dentro de on_channel_message quando
        //       HandshakeAck chega no channel "message" --

        // -- 3. Uma vez handshake_done, worker pode mandar input a 125 Hz --
        if (s->handshake_done) {
            drain_gamepad_and_send(s);   // send_binary_locked("input", ...)
        }

        // -- 4. Dead media detection --
        if (ice_connected_at &&
            !s->channels_open &&
            SDL_GetTicks64() - ice_connected_at > 12000) {
            log("ICE up but DTLS/SCTP never came up -- retry");
            return; // sinaliza pra outer loop retry-once
        }

        usleep(4000); // 4ms tick
    }
}
```

---

## 4. URLs verificadas (commit-pinned)

### green-nx (SHA `e498290dccbeef46154f985150e122b1e6e8965e`)

- **`engine.cpp` L396-421 (open_data_channels)** — https://github.com/rmrf404/green-nx/blob/e498290dccbeef46154f985150e122b1e6e8965e/src/switch/stream/engine.cpp#L396-L421
- **`engine.cpp` L383-388 (callback trampolines)** — https://github.com/rmrf404/green-nx/blob/e498290dccbeef46154f985150e122b1e6e8965e/src/switch/stream/engine.cpp#L383-L388
- **`engine.cpp` L431-455 (send_on_channel_locked, JSON vs binary)** — https://github.com/rmrf404/green-nx/blob/e498290dccbeef46154f985150e122b1e6e8965e/src/switch/stream/engine.cpp#L431-L455
- **`engine.cpp` L458-545 (handle_channel_message, HandshakeAck flow)** — https://github.com/rmrf404/green-nx/blob/e498290dccbeef46154f985150e122b1e6e8965e/src/switch/stream/engine.cpp#L458-L545
- **`engine.cpp` L993-1011 (peer_connection_create + ondatachannel)** — https://github.com/rmrf404/green-nx/blob/e498290dccbeef46154f985150e122b1e6e8965e/src/switch/stream/engine.cpp#L993-L1011
- **`engine.cpp` L1273-1280 (worker: open_data_channels após channels_open_)** — https://github.com/rmrf404/green-nx/blob/e498290dccbeef46154f985150e122b1e6e8965e/src/switch/stream/engine.cpp#L1273-L1280
- **`xcloud_protocol.hpp` L15-30 (kControlChannel/kInputChannel/kMessageChannel/kChatChannel)** — https://github.com/rmrf404/green-nx/blob/e498290dccbeef46154f985150e122b1e6e8965e/src/core/xcloud_protocol.hpp#L15-L30
- **`xcloud_protocol.cpp` L98-138 (authorization_request, message_handshake, is_handshake_ack)** — https://github.com/rmrf404/green-nx/blob/e498290dccbeef46154f985150e122b1e6e8965e/src/core/xcloud_protocol.cpp#L98-L138

### libpeer (SHA `5b849de378545c31d34759a145413846953e1366`)

- **`src/peer_connection.h`** — https://github.com/sepfy/libpeer/blob/5b849de378545c31d34759a145413846953e1366/src/peer_connection.h
- **`src/peer_connection.c` (SCTP association setup pós-DTLS)** — https://github.com/sepfy/libpeer/blob/5b849de378545c31d34759a145413846953e1366/src/peer_connection.c
- **`src/sctp.c` (COOKIE-ACK dispara onopen)** — https://github.com/sepfy/libpeer/blob/5b849de378545c31d34759a145413846953e1366/src/sctp.c
- **`src/sctp.h` (PPIDs)** — https://github.com/sepfy/libpeer/blob/5b849de378545c31d34759a145413846953e1366/src/sctp.h

### RFCs

- **RFC 8831 (WebRTC Data Channels)** — https://datatracker.ietf.org/doc/html/rfc8831
- **RFC 8832 (DCEP)** — https://datatracker.ietf.org/doc/html/rfc8832

---

## 5. Gotchas específicos que green-nx aprendeu na marra

1. **Não abrir channels antes de SCTP subir**: `peer_connection_create_datachannel*` chamado antes de `sctp->connected == 1` silenciosamente falha porque `sctp_is_connected()` retorna false. Espere o callback `onopen`.

2. **PPID errado = xCloud ignora**: `control`/`message`/`chat` são JSON → **PPID_STRING (51)** via `peer_connection_datachannel_send_text_sid`. `input` é binary → PPID_BINARY (53) via `peer_connection_datachannel_send_sid`. xCloud simplesmente ignora JSON marcado como binary.

3. **Mutex não-recursivo**: callback `on_channel_message` já vem com peer mutex segurado (libpeer chamou de dentro do loop). Se re-lockar, deadlock e sessão trava em "Handshaking".

4. **SIDs pares**: cliente é DTLS-client, RFC 8832 exige SIDs pares. Ímpares seriam do servidor. 0/2/4/6 funciona.

5. **Handshake precisa ser provocado**: cliente MANDA `MessageHandshake` no channel `message` logo após abrir channels. Não é xCloud que fala primeiro.

6. **`input` REL ordered mesmo sendo alta-frequência**: protocolo 1.0 usa sequência absoluta, gap trava input. Não caia na tentação de mudar pra unreliable.

7. **Retry: tear down inteiro**: canal cai = SCTP association morreu = sessão morta. `retry-once` do worker cria PeerConnection novo do zero (com backoff de 3s pra backend liberar slot).

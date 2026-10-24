# xCloud input via WebRTC — wire format (research)

**Fonte primária:** `github.com/rmrf404/green-nx` @ `main`
- `src/core/xcloud_protocol.hpp` — struct + channel config
- `src/core/xcloud_protocol.cpp` — serializer binário (lines 1–95)
- `src/switch/stream/engine.cpp` — pump/handshake/backoff

**Commit refs (permalink):**
- https://github.com/rmrf404/green-nx/blob/main/src/core/xcloud_protocol.hpp
- https://github.com/rmrf404/green-nx/blob/main/src/core/xcloud_protocol.cpp#L48-L94
- https://github.com/rmrf404/green-nx/blob/main/src/switch/stream/engine.cpp (função `send_gamepad`, backoff + idle suppression)

---

## Respostas diretas

**1. Nome do data channel:** `"input"` (label). Protocol string `"1.0"`. Ordered, reliável (`max_retransmits=-1`). SCTP sid par (green-nx usa `2`; xCloud mapeia por label, sid só precisa ser único).

**2. Formato do packet:** binário puro (little-endian, exceto último trailer `0x00000001` que é big-endian). Data channel binary frame (não string).

**3. Byte layout GamepadFrame (23 bytes payload + 15 bytes header = 38 bytes total):**

Header (comum a todo report do canal input):
```
offset  size  campo
0       2     report_type   uint16 LE   = 2 (kReportGamepad) | 8 (kReportClientMetadata)
2       4     sequence      uint32 LE   ++ por packet (server dropa input se detectar gap — issue #45)
6       8     elapsed_ms    double LE   ms desde start da sessão
```

Payload gamepad (após header, report_type=2):
```
offset  size  campo
14      1     count         = 1 (um gamepad)
15      1     index         id do pad (0..3)
16      2     buttons_mask  uint16 LE (bitmap — ver tabela)
18      2     left_x        int16 LE  = round(x * 32767), range -32767..32767
20      2     left_y        int16 LE  = round(-y * 32767)  ← NOTA: Y invertido
22      2     right_x       int16 LE
24      2     right_y       int16 LE  ← Y invertido
26      2     left_trigger  uint16 LE = round(t * 65535), range 0..65535
28      2     right_trigger uint16 LE
30      4     physical_id   uint32 LE = 1
34      4     trailer       BIG-ENDIAN = 0x00000001 (constante do client oficial)
```

Total: **38 bytes** por packet gamepad.

**Buttons bitmap (uint16, LE):**
```
bit  hex     botão
1    0x0002  Nexus (Xbox/Guide)
2    0x0004  Menu (Start)
3    0x0008  View (Back/Select)
4    0x0010  A
5    0x0020  B
6    0x0040  X
7    0x0080  Y
8    0x0100  Dpad Up
9    0x0200  Dpad Down
10   0x0400  Dpad Left
11   0x0800  Dpad Right
12   0x1000  LB (Left Shoulder)
13   0x2000  RB (Right Shoulder)
14   0x4000  LS click (Left Thumb)
15   0x8000  RS click (Right Thumb)
```
Bit 0 (`0x0001`) não usado.

**4. Frequência recomendada:** **125 Hz (8ms tick)**. Green-nx pumpa nessa taxa. Idle suppression aplicada: se estado quantizado == último enviado, não manda packet (economiza banda + evita afogar SCTP send buffer durante lag). Se SCTP send retornar full, aplica backoff (não retenta em 8ms — espera drenar).

**5. Heartbeat/keepalive do canal input:** **não existe** keepalive dedicado. Green-nx re-anuncia pad + reenvia `client_metadata` no revive (após dead-time detectado) via:
```cpp
send_binary_on_channel("input", input_.client_metadata());   // report_type=8
send_on_channel("control", gamepad_changed(0, false));        // JSON
send_on_channel("control", gamepad_changed(0, true));         // JSON
```
Manter conexão viva = enviar input real quando houver + idle suppression cuida do resto. Peer/DTLS/SCTP heartbeats são do próprio libpeer.

**6. Sequência de init (ordem exata):**
1. Peer connect + open 4 data channels: `control`(sid 0, `controlV1`), `input`(sid 2, `1.0`), `message`(sid 4, `messageV1`), `chat`(sid 6, `chatV1`) — todos ordered+reliable.
2. Aguardar handshake ACK no canal `message` (`is_handshake_ack`).
3. Ao receber ACK, enviar no `control`:
   - `authorization_request` (JSON, accessKey `4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E`)
   - `gamepad_changed(0, true)` (JSON — anuncia pad presente)
   - `user_requested_resolution("720" | "1080" | "1080HQ")`
4. Enviar `startup_messages` no `message` (systemUi, orientation, touchInput, clientDeviceCapabilities, etc — todos wrapped em `{"type":"Message","content":<json>,...}`).
5. Enviar **primeiro packet binário no `input`**: `client_metadata()` (report_type=8, seq=1, elapsed=0, max_touchpoints=0).
6. `videoKeyframeRequested` no `control` + RTCP PLI.
7. **Pode começar a enviar gamepad_packet() no `input`** — antes ou depois do primeiro keyframe, tanto faz. Server aceita input já depois do handshake_done.

**7. Input pump — thread separada?** Não. Green-nx envia gamepad do **loop principal** (thread do render/SDL). `send_gamepad()` pega `peer_mutex_` com **timed_mutex** (bounded wait) pra render nunca travar se worker segurar o lock. Poll SDL → build `GamepadFrame` → chama `send_gamepad`. Sem thread dedicada.

**8. Snippet C++ mínimo:** veja `snippet-serializer.cpp` abaixo.

---

## API do x360-controle (libxenon) pra ler estado

Header: **`/home/srglimasks/devkitxenon/usr/include/input/input.h`**
```c
struct controller_data_s {
    signed short s1_x, s1_y, s2_x, s2_y;  // sticks -32768..32767
    int s1_z, s2_z, lb, rb, start, back, a, b, x, y, up, down, left, right;
    unsigned char lt, rt;                  // triggers 0..255
    int logo;                              // Xbox/Guide button
};

int get_controller_data(struct controller_data_s *d, int port);  // port 0..3
```

O plugin `XboxInput.xex` do x360-controle expõe controle Xbox One/Series **como slot XInput normal** — `get_controller_data(&d, 0)` funciona igual pra controle nativo do 360 ou Series X via USB. Não precisa API própria.

---

## Snippet C serializer xCloud gamepad packet (auto-contido)

```cpp
// snippet-serializer.cpp — xCloud "input" channel gamepad packet
// Compile: qualquer C++14+, host little-endian (o XCPU do 360 é big-endian,
// então na PoC você inverte manualmente com bswap; ver nota no fim).
#include <cstdint>
#include <cstring>
#include <vector>

struct GamepadFrame {
    uint8_t index;
    bool nexus, menu, view;
    bool a, b, x, y;
    bool dpad_up, dpad_down, dpad_left, dpad_right;
    bool lb, rb, ls, rs;
    float lx, ly, rx, ry;   // -1..1
    float lt_axis, rt_axis; // 0..1
};

class XCloudInputSerializer {
    uint32_t sequence_ = 0;
public:
    // packet inicial obrigatório após handshake (report_type=8)
    std::vector<uint8_t> client_metadata(uint8_t max_touchpoints = 0) {
        std::vector<uint8_t> out; out.reserve(15);
        put_le<uint16_t>(out, 8);              // kReportClientMetadata
        put_le<uint32_t>(out, ++sequence_);
        put_le<double>(out, 0.0);
        out.push_back(max_touchpoints);
        return out;
    }
    // packet gamepad (report_type=2) — enviar a 125Hz com idle suppression
    std::vector<uint8_t> gamepad_packet(const GamepadFrame& p, double elapsed_ms) {
        std::vector<uint8_t> out; out.reserve(38);
        put_le<uint16_t>(out, 2);              // kReportGamepad
        put_le<uint32_t>(out, ++sequence_);
        put_le<double>(out, elapsed_ms);
        out.push_back(1);                      // count
        out.push_back(p.index);
        uint16_t m = 0;
        if (p.nexus)      m |= 0x0002;
        if (p.menu)       m |= 0x0004;
        if (p.view)       m |= 0x0008;
        if (p.a)          m |= 0x0010;
        if (p.b)          m |= 0x0020;
        if (p.x)          m |= 0x0040;
        if (p.y)          m |= 0x0080;
        if (p.dpad_up)    m |= 0x0100;
        if (p.dpad_down)  m |= 0x0200;
        if (p.dpad_left)  m |= 0x0400;
        if (p.dpad_right) m |= 0x0800;
        if (p.lb)         m |= 0x1000;
        if (p.rb)         m |= 0x2000;
        if (p.ls)         m |= 0x4000;
        if (p.rs)         m |= 0x8000;
        put_le<uint16_t>(out, m);
        put_le<int16_t>(out, axis(p.lx));
        put_le<int16_t>(out, axis(-p.ly));      // Y invertido
        put_le<int16_t>(out, axis(p.rx));
        put_le<int16_t>(out, axis(-p.ry));      // Y invertido
        put_le<uint16_t>(out, trig(p.lt_axis));
        put_le<uint16_t>(out, trig(p.rt_axis));
        put_le<uint32_t>(out, 1);               // physical unit id
        out.push_back(0); out.push_back(0);     // trailer big-endian 0x00000001
        out.push_back(0); out.push_back(1);
        return out;
    }
    // Chame se seu send falhar (SCTP full etc) — devolve o número de sequência
    // pra evitar gap (server pausa input em gap detectado — issue #45).
    void rollback_sequence() { if (sequence_) --sequence_; }
private:
    static int16_t axis(float v) {
        float s = v * 32767.0f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32767.0f) s = -32767.0f;
        return (int16_t)s;
    }
    static uint16_t trig(float v) {
        if (v < 0) return 0;
        float s = v * 65535.0f;
        if (s > 65535.0f) s = 65535.0f;
        return (uint16_t)s;
    }
    template<typename T> static void put_le(std::vector<uint8_t>& o, T v) {
        uint8_t raw[sizeof(T)]; std::memcpy(raw, &v, sizeof(T));
        // Se host for big-endian (XCPU 360!), inverter aqui antes de append:
        // for (size_t i = 0; i < sizeof(T)/2; ++i) std::swap(raw[i], raw[sizeof(T)-1-i]);
        o.insert(o.end(), raw, raw + sizeof(T));
    }
};

// --- Loop de exemplo (host PoC / substituir SDL por get_controller_data no 360) ---
//
// #include <input/input.h>   // libxenon
// XCloudInputSerializer ser;
// // após handshake_done:
// send_binary("input", ser.client_metadata());
// auto t0 = now_ms();
// GamepadFrame last{}; bool have_last = false;
// while (streaming) {
//     controller_data_s d;
//     if (get_controller_data(&d, 0) == 0) {
//         GamepadFrame f{};
//         f.index = 0;
//         f.a = d.a; f.b = d.b; f.x = d.x; f.y = d.y;
//         f.dpad_up = d.up; f.dpad_down = d.down;
//         f.dpad_left = d.left; f.dpad_right = d.right;
//         f.lb = d.lb; f.rb = d.rb;
//         f.menu = d.start; f.view = d.back; f.nexus = d.logo;
//         f.ls = d.s1_z; f.rs = d.s2_z;
//         f.lx = d.s1_x / 32767.0f; f.ly = d.s1_y / 32767.0f;
//         f.rx = d.s2_x / 32767.0f; f.ry = d.s2_y / 32767.0f;
//         f.lt_axis = d.lt / 255.0f; f.rt_axis = d.rt / 255.0f;
//         if (!have_last || memcmp(&f, &last, sizeof(f)) != 0) {
//             auto pkt = ser.gamepad_packet(f, now_ms() - t0);
//             if (!send_binary("input", pkt)) ser.rollback_sequence();
//             last = f; have_last = true;
//         }
//     }
//     sleep_ms(8);  // 125 Hz
// }
```

---

## Gotchas críticos (do código green-nx)

1. **Sequence number monotônico obrigatório.** Se você "consumir" um seq e o send falhar, chame `rollback_sequence()`. Server pausa input inteiro em gap (issue #45).
2. **Y invertido nos sticks.** Green-nx nega Y antes de escalar. SDL joystick já reporta Y positivo pra baixo — se seu source já é positivo-down (como SDL), passa negado.
3. **XCPU 360 é big-endian.** Green-nx é código Switch (Tegra ARM64 LE). No 360 você precisa swap manual em cada `put<T>` (LE explícito) — descomenta a linha no `put_le`. O trailer `0x00000001` já é big-endian na wire, então lá fica os 4 bytes `00 00 00 01` sem swap.
4. **Idle suppression == essencial.** Sem ela, 125Hz de packets iguais enche o SCTP send buffer em lag spike e trava a stream (green-nx aprendeu isso ao vivo).
5. **Não use unreliable no protocol "1.0".** Xbox pausa input em gap. Green-nx testou e voltou pra ordered+reliable. Protocol "2.0" (unreliableinput + reliableinput split com acks) é outra história — não implementada em green-nx ainda.
6. **`client_metadata` uma vez após handshake** + a cada revive. NÃO é keepalive periódico.
7. **`gamepadChanged` no control channel** (JSON) anuncia pad presente. Manda `{added: true}` no init, `{added: false}` só se remover pad. Green-nx também usa false→true no revive.

---

## URLs commit direto

- Serializer completo (função `gamepad_packet`):
  https://github.com/rmrf404/green-nx/blob/main/src/core/xcloud_protocol.cpp#L49-L94
- Struct GamepadFrame + channel config:
  https://github.com/rmrf404/green-nx/blob/main/src/core/xcloud_protocol.hpp
- Send loop com backoff + idle suppression (`send_gamepad`):
  https://github.com/rmrf404/green-nx/blob/main/src/switch/stream/engine.cpp
- Handshake sequence (message ACK → auth → gamepadChanged → client_metadata → keyframe):
  https://github.com/rmrf404/green-nx/blob/main/src/switch/stream/engine.cpp

**Backup:** `github.com/unknownskl/greenlight` (TypeScript) — o comentário no `xcloud_protocol.hpp` cita: "Reference: greenlight, green-vita". Não consultei porque green-nx já entregou tudo em C++ nativo.

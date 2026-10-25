// XCloud360 — P5.5 capture-rtp
//
// End-to-end xCloud WebRTC capture on the PC. Drives the full session
// lifecycle using Green-NX's auth + session HTTP glue (compiled in), then
// brings up a real libpeer PeerConnection to receive RTP video, dumping
// each H.264 NAL as Annex-B into stream.h264 — that file IS the GOLDEN
// STREAM the console decoder is developed against.
//
// Deliverables per run:
//   stream.h264         — Annex-B H.264 bitstream, ~30 s of gameplay
//   offer.sdp           — local SDP offer we sent
//   answer.sdp          — xCloud SDP answer we received (redacted)
//   ice-local.json      — local ICE candidates we emitted
//   ice-remote.json     — remote ICE candidates received (redacted)
//   session-meta.json   — title id, endpoint host (redacted), tier,
//                         connection timing, negotiated codec
//   analysis.json       — extracted profile_idc, level, resolution, fps,
//                         entropy_coding, num_ref_frames, slices/frame
//                         (populated by analyze/ tool after capture)
//   analysis.txt        — human summary
//   sha256.txt          — SHA-256 of every artifact
//
// GPL-3: this binary links Green-NX sources directly (auth.cpp/session.cpp/
// http.cpp/xcloud_protocol.cpp). It is not distributed as part of the
// XCloud360 tree — the compiled binary lives under tools/xcloud-capture/
// output/ (gitignored). Green-NX ships as external subprocess dependency;
// this rtp/ binary is the ONE exception where linkage is more practical.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "video_jitter.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <cstdint>
#include <deque>

#include "auth.hpp"
#include "session.hpp"
#include "http.hpp"
#include "xcloud_protocol.hpp"
#include "json.hpp"

extern "C" {
#include "peer.h"
#include "peer_connection.h"

// Green-NX libpeer patch adds these two symbols but doesn't publish them via
// peer_connection.h in every dist layout. Forward-declare so this TU builds
// regardless of which header snapshot is installed under build-pc/dist/.
int peer_connection_datachannel_send_text_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid);
int peer_connection_request_keyframe(PeerConnection* pc);
int peer_connection_send_nack(PeerConnection* pc, uint16_t pid, uint16_t blp);
int peer_connection_send_remb(PeerConnection* pc, uint32_t bitrate_bps);
int peer_connection_send_receiver_report(PeerConnection* pc, uint8_t fraction_lost,
                                         uint32_t cumulative_lost, uint32_t highest_seq_ext,
                                         uint32_t jitter);
}

using json = nlohmann::json;

// ------------------------- redaction helpers -------------------------

// Replace 40+ char base64/hex-looking tokens with SHA-like prefix + <REDACTED>.
static std::string redact_secrets(std::string s) {
    // Bearer / gsToken / cookies / DTLS fingerprints
    static const char* keys[] = {
        "Authorization: ", "Bearer ", "gsToken", "cookie=",
        "X-Xbl-", "a=ice-pwd:",
    };
    for (const char* k : keys) {
        size_t p = 0;
        while ((p = s.find(k, p)) != std::string::npos) {
            size_t end = s.find_first_of("\r\n\" ;,", p + std::strlen(k));
            if (end == std::string::npos) end = s.size();
            s.replace(p + std::strlen(k), end - p - std::strlen(k), "<REDACTED>");
            p += std::strlen(k) + 10;
        }
    }
    return s;
}

// -------------------- global capture state ---------------------------

// ------------------------- H.264 parameter-set log -------------------
//
// v0.15: confirma o perfil real do stream (o SDP diz 42e01f, Constrained
// Baseline 3.1, mas isso nunca foi conferido no SPS/PPS de verdade). So
// le; nada aqui altera o AU que vai pro Xbox.

// Remove emulation-prevention bytes (00 00 03 -> 00 00) de um NAL.
static std::vector<uint8_t> h264_nal_to_rbsp(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    size_t zeros = 0;
    for (size_t k = 0; k < n; k++) {
        if (zeros >= 2 && p[k] == 0x03) { zeros = 0; continue; }
        out.push_back(p[k]);
        zeros = (p[k] == 0) ? zeros + 1 : 0;
    }
    return out;
}

struct H264BitReader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;   // em bits
    bool err = false;
    H264BitReader(const std::vector<uint8_t>& v, size_t start_byte)
        : b(v), pos(start_byte * 8) {}
    unsigned bit() {
        if (pos >= b.size() * 8) { err = true; return 0; }
        unsigned v = (b[pos >> 3] >> (7 - (pos & 7))) & 1;
        pos++;
        return v;
    }
    uint32_t ue() {
        int lz = 0;
        while (!err && bit() == 0) { if (++lz > 31) { err = true; return 0; } }
        if (err) return 0;
        uint32_t v = 0;
        for (int k = 0; k < lz; k++) v = (v << 1) | bit();
        return (uint32_t)(((1ull << lz) - 1) + v);
    }
};

// nal aponta pro byte de header do NAL (sem start code).
static void h264_log_sps(const uint8_t* nal, size_t n) {
    std::vector<uint8_t> r = h264_nal_to_rbsp(nal, n);
    if (r.size() < 4) { std::printf("[h264] SPS too short (%zu bytes)\n", n); return; }
    std::printf("[h264] SPS profile_idc=%u constraint=0x%02x level_idc=%u (%zu bytes)\n",
                (unsigned)r[1], (unsigned)r[2], (unsigned)r[3], n);
}

static void h264_log_pps(const uint8_t* nal, size_t n) {
    std::vector<uint8_t> r = h264_nal_to_rbsp(nal, n);
    H264BitReader br(r, 1);   // pula o header de 1 byte
    uint32_t pps_id = br.ue();
    uint32_t sps_id = br.ue();
    unsigned ecm = br.bit();
    if (br.err) { std::printf("[h264] PPS parse error (%zu bytes)\n", n); return; }
    std::printf("[h264] PPS entropy_coding_mode=%u (%s) pps_id=%u sps_id=%u\n",
                ecm, ecm ? "CABAC" : "CAVLC", (unsigned)pps_id, (unsigned)sps_id);
}

// ------------------------- TCP-out server ----------------------------
//
// Emits Annex-B H.264 AUs on a length-prefix wire ([uint32_be size][AU])
// compatible with the orchestrator's Phase A server (h264_feeder pattern).
// Single-client server. On each new connection, waits for the next IDR from
// the RTP stream and prepends the latest SPS/PPS before shipping frames.
//
// This lives in-process because otherwise capture-rtp would have to fork+pipe
// which loses the ability to detect back-pressure and re-request keyframes.
//
// Fase 1 (latência): push_au() roda na thread do peer_connection_loop e NUNCA
// pode bloquear. Antes fazia send() bloqueante (SO_SNDTIMEO 10s) ali dentro:
// quando o Xbox parava de drenar, a thread que lê o RTP travava, o kernel
// descartava UDP do xCloud e o jitter buffer virava tempestade de IDR.
// Agora push_au só enfileira; uma sender thread faz o send(). Fila curta
// (kMaxQueuedAU) + idade máxima (kMaxAgeMs): se estourar, descarta tudo e
// segue descartando até o próximo IDR (nunca manda P-frame órfão pro Xbox)
// e dispara on_congestion pra pedir keyframe. Latência fica limitada em vez
// de crescer até o ring do Xbox encher.
class TcpOutServer {
 public:
    static constexpr size_t   kMaxQueuedAU = 3;
    static constexpr uint64_t kMaxAgeMs    = 150;
    // Tolerância pós-IDR: um IDR tem dezenas a centenas de KB e leva várias
    // janelas TCP do Xbox (~32KB) pra passar; os P-frames logo atrás dele
    // estouravam os 150ms, disparavam outra congestão → outro IDR → ciclo de
    // ~1s (o throttle de keyframe). Nesse intervalo os limites são frouxos.
    static constexpr uint64_t kIdrGraceMs        = 400;
    static constexpr size_t   kMaxQueuedAUGrace  = 12;
    static constexpr uint64_t kMaxAgeMsGrace     = 500;

    explicit TcpOutServer(int port) : port_(port) {}
    ~TcpOutServer() { stop(); }

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) { std::perror("[tcp] socket"); return false; }
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((uint16_t)port_);
        if (::bind(listen_fd_, (sockaddr*)&a, sizeof(a)) < 0) {
            std::perror("[tcp] bind"); return false;
        }
        if (::listen(listen_fd_, 4) < 0) {
            std::perror("[tcp] listen"); return false;
        }
        stop_.store(false);
        accept_thread_ = std::thread([this]{ accept_loop(); });
        sender_thread_ = std::thread([this]{ sender_loop(); });
        std::printf("[tcp] listening on 0.0.0.0:%d (length-prefix Annex-B AU, queue<=%zu age<=%llums)\n",
                    port_, kMaxQueuedAU, (unsigned long long)kMaxAgeMs);
        return true;
    }

    void stop() {
        stop_.store(true);
        q_cv_.notify_all();
        if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (client_fd_ >= 0) { ::shutdown(client_fd_, SHUT_RDWR); ::close(client_fd_); client_fd_ = -1; }
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        if (sender_thread_.joinable()) sender_thread_.join();
    }

    // Queue a complete Annex-B AU for the current client. Caches SPS/PPS
    // seen inside the AU so a fresh client that missed the last IDR gets
    // them prepended to the next IDR AU. Gates output on IDR — first frame
    // sent to every new client must be IDR + SPS + PPS (else Xbox decoder
    // shows garbage until next IDR ~5s later). Never blocks.
    void push_au(const uint8_t* au_data, size_t au_size) {
        if (au_size == 0) return;

        // Scan AU for NAL types + cache SPS/PPS as we go. Accepts both 3-byte
        // (00 00 01) and 4-byte start codes — x264 output mixes them, and a
        // missed IDR here would stall the client in wait_idr_ forever.
        bool has_idr = false;
        bool has_sps = false, has_pps = false;
        bool has_slice = false, any_ref_slice = false;
        auto is_sc3 = [&](size_t p) {
            return p + 3 <= au_size && au_data[p] == 0 && au_data[p+1] == 0 && au_data[p+2] == 1;
        };
        size_t i = 0;
        while (i + 3 <= au_size) {
            if (is_sc3(i)) {
                if (i + 3 >= au_size) break;
                size_t nal_start = i + 3;
                size_t nal_end = nal_start;
                while (nal_end + 3 <= au_size && !is_sc3(nal_end)) nal_end++;
                if (nal_end + 3 > au_size) nal_end = au_size;
                size_t next = nal_end;
                // Leading zero of a following 4-byte start code isn't payload.
                while (nal_end > nal_start && nal_end < au_size && au_data[nal_end - 1] == 0)
                    nal_end--;
                uint8_t nal_type = au_data[nal_start] & 0x1f;
                if (nal_type == 7) {
                    sps_.assign(au_data + nal_start, au_data + nal_end);
                    has_sps = true;
                    // 1a vez por conexao (flag armada no accept_loop).
                    if (log_sps_pending_.exchange(false))
                        h264_log_sps(au_data + nal_start, nal_end - nal_start);
                } else if (nal_type == 8) {
                    pps_.assign(au_data + nal_start, au_data + nal_end);
                    has_pps = true;
                    if (log_pps_pending_.exchange(false))
                        h264_log_pps(au_data + nal_start, nal_end - nal_start);
                } else if (nal_type == 5) {
                    has_idr = true;
                }
                if (nal_type == 1 || nal_type == 5) {
                    has_slice = true;
                    if (au_data[nal_start] & 0x60) any_ref_slice = true;  // nal_ref_idc
                }
                i = next;
            } else {
                i++;
            }
        }

        // Frame não-referência (nal_ref_idc == 0 em todos os slices): nenhum
        // outro frame depende dele, então dá pra descartar sem quebrar a
        // imagem. xCloud ignora supportsFps=30 e manda 60; com --drop-nonref
        // o Xbox recebe só os frames de referência.
        bool nonref = has_slice && !any_ref_slice;
        if (nonref) nonref_seen_++;

        {
            std::lock_guard<std::mutex> lk(mu_);
            if (client_fd_ < 0) return;
        }
        if (nonref && drop_nonref_) { nonref_dropped_++; return; }

        bool congested = false;
        {
            std::lock_guard<std::mutex> lk(q_mu_);
            auto now = std::chrono::steady_clock::now();
            if (has_idr) {
                // IDR zera as referências: o que ainda está na fila só
                // adicionaria latência, então joga fora e recomeça daqui.
                dropped_ += queue_.size();
                queue_.clear();
                wait_idr_ = false;
                grace_until_ = now + std::chrono::milliseconds(kIdrGraceMs);
            } else if (wait_idr_) {
                dropped_++;
                return;
            } else if (queue_.size() >= (now < grace_until_ ? kMaxQueuedAUGrace
                                                             : kMaxQueuedAU)) {
                dropped_ += queue_.size() + 1;
                queue_.clear();
                wait_idr_ = true;
                congested = true;
                congest_queue_++;
            }
            if (!congested) {
                QueuedAU q;
                q.idr = has_idr;
                q.t_enq = std::chrono::steady_clock::now();
                q.bytes.reserve(au_size + 4 + 64);
                q.bytes.resize(4);
                static const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
                if (has_idr && !has_sps && !sps_.empty()) {
                    q.bytes.insert(q.bytes.end(), sc, sc + 4);
                    q.bytes.insert(q.bytes.end(), sps_.begin(), sps_.end());
                }
                if (has_idr && !has_pps && !pps_.empty()) {
                    q.bytes.insert(q.bytes.end(), sc, sc + 4);
                    q.bytes.insert(q.bytes.end(), pps_.begin(), pps_.end());
                }
                q.bytes.insert(q.bytes.end(), au_data, au_data + au_size);
                uint32_t size_be = htonl((uint32_t)(q.bytes.size() - 4));
                std::memcpy(q.bytes.data(), &size_be, 4);
                queue_.push_back(std::move(q));
            }
        }
        if (congested) {
            congestion_events_++;
            if (on_congestion_) on_congestion_();
        } else {
            q_cv_.notify_one();
        }
    }

    // Callbacks fire on the accept / pc-loop / sender threads; they must only
    // set flags, never touch the PeerConnection directly.
    void set_on_new_client(std::function<void()> cb) { on_new_client_ = std::move(cb); }
    void set_on_congestion(std::function<void()> cb) { on_congestion_ = std::move(cb); }

    void set_drop_nonref(bool v) { drop_nonref_ = v; }

    uint64_t dropped() const { return dropped_.load(); }
    uint64_t congestion_events() const { return congestion_events_.load(); }
    uint64_t congest_queue() const { return congest_queue_.load(); }
    uint64_t congest_age() const { return congest_age_.load(); }
    uint64_t nonref_seen() const { return nonref_seen_.load(); }
    uint64_t nonref_dropped() const { return nonref_dropped_.load(); }
    // True while a connected client is starved waiting for the next IDR.
    bool waiting_idr() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (client_fd_ < 0) return false;
        }
        std::lock_guard<std::mutex> lk(q_mu_);
        return wait_idr_;
    }

 private:
    struct QueuedAU {
        std::vector<uint8_t> bytes;   // [uint32_be size][annex-b AU]
        bool idr = false;
        std::chrono::steady_clock::time_point t_enq;
    };

    void close_client() {
        int fd;
        {
            std::lock_guard<std::mutex> lk(mu_);
            fd = client_fd_;
            client_fd_ = -1;
        }
        if (fd >= 0) ::close(fd);
        std::lock_guard<std::mutex> lk(q_mu_);
        queue_.clear();
        wait_idr_ = true;
    }

    void sender_loop() {
        while (!stop_.load()) {
            QueuedAU q;
            bool stale = false;
            {
                std::unique_lock<std::mutex> lk(q_mu_);
                q_cv_.wait_for(lk, std::chrono::milliseconds(100),
                               [this]{ return stop_.load() || !queue_.empty(); });
                if (stop_.load()) break;
                if (queue_.empty()) continue;
                q = std::move(queue_.front());
                queue_.pop_front();
                uint64_t age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - q.t_enq).count();
                uint64_t max_age = (q.t_enq < grace_until_) ? kMaxAgeMsGrace : kMaxAgeMs;
                if (!q.idr && age_ms > max_age) {
                    // Xbox drenando devagar: frame velho só aumenta o atraso.
                    dropped_ += queue_.size() + 1;
                    queue_.clear();
                    wait_idr_ = true;
                    stale = true;
                    congest_age_++;
                }
            }
            if (stale) {
                congestion_events_++;
                if (on_congestion_) on_congestion_();
                continue;
            }
            int fd;
            {
                std::lock_guard<std::mutex> lk(mu_);
                fd = client_fd_;
            }
            if (fd < 0) continue;
            // Blocking send is fine here: this thread owns the socket and a
            // stall never reaches the RTP receive path.
            size_t sent = 0;
            while (sent < q.bytes.size()) {
                ssize_t w = ::send(fd, q.bytes.data() + sent, q.bytes.size() - sent, MSG_NOSIGNAL);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    std::fprintf(stderr, "[tcp] send failed at %zu/%zu errno=%d (%s) — closing client\n",
                                 sent, q.bytes.size(), errno, std::strerror(errno));
                    close_client();
                    break;
                }
                sent += (size_t)w;
            }
        }
    }

    void accept_loop() {
        while (!stop_.load()) {
            sockaddr_in ca = {};
            socklen_t cl = sizeof(ca);
            int fd = ::accept(listen_fd_, (sockaddr*)&ca, &cl);
            if (fd < 0) {
                if (stop_.load()) break;
                if (errno == EINTR) continue;
                std::perror("[tcp] accept");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            std::printf("[tcp] client connected from %s:%d — waiting for next IDR\n",
                        inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));
            int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            // Buffer do kernel pequeno (Linux dobra → ~128KB): cabe um IDR,
            // mas não esconde segundos de vídeo atrasado fora da nossa fila.
            int sndbuf = 64 * 1024;
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
            // Pacing: sem isso o kernel manda a janela inteira (~20 pacotes)
            // de rajada; a NIC do Xbox (polling a 1ms) perde o fim da rajada,
            // e cada perda de cauda vira RTO de ~200ms = vídeo travado.
            // 10MB/s: um IDR de 200KB sai em ~20ms, 1 pacote a cada ~150us.
            unsigned int pace = 10u * 1000 * 1000;
            setsockopt(fd, SOL_SOCKET, SO_MAX_PACING_RATE, &pace, sizeof(pace));
            // Bounded blocking send in sender_loop: gives up cleanly if the
            // Xbox actually died (RST/route lost) instead of hanging forever.
            struct timeval sndto{10, 0};   // 10 s
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (client_fd_ >= 0) ::close(client_fd_);
                client_fd_ = fd;
            }
            // v0.18: o áudio vai por UDP pro mesmo Xbox (IP do cliente de vídeo).
            client_ip_.store(ca.sin_addr.s_addr);
            // v0.15: loga SPS/PPS na primeira vez que aparecerem nesta conexao.
            log_sps_pending_.store(true);
            log_pps_pending_.store(true);
            {
                // First AU sent to a new client must be IDR (with SPS/PPS
                // prepended by push_au above).
                std::lock_guard<std::mutex> lk(q_mu_);
                queue_.clear();
                wait_idr_ = true;
            }
            if (on_new_client_) on_new_client_();
        }
    }

 public:
    // v0.18: IPv4 (network order) do último cliente de vídeo; 0 = nenhum ainda.
    uint32_t client_ip() const { return client_ip_.load(); }
 private:
    std::atomic<uint32_t> client_ip_{0};
    int port_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::mutex mu_;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
    std::thread sender_thread_;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    std::atomic<bool> log_sps_pending_{false};
    std::atomic<bool> log_pps_pending_{false};
    std::function<void()> on_new_client_;
    std::function<void()> on_congestion_;

    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<QueuedAU> queue_;
    bool wait_idr_ = true;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> congestion_events_{0};
    std::chrono::steady_clock::time_point grace_until_{};
    std::atomic<uint64_t> congest_queue_{0};
    std::atomic<uint64_t> congest_age_{0};
    bool drop_nonref_ = false;
    std::atomic<uint64_t> nonref_seen_{0};
    std::atomic<uint64_t> nonref_dropped_{0};
};

struct CaptureCtx {
    std::string out_dir;
    std::FILE* h264_fp = nullptr;
    std::FILE* opus_fp = nullptr;
    std::atomic<uint64_t> video_bytes{0};
    std::atomic<uint64_t> video_nals{0};
    std::atomic<uint64_t> audio_bytes{0};
    std::atomic<uint64_t> audio_frames{0};
    // v0.18: payload Opus de cada pacote RTP -> UDP <Xbox>:audio_port com
    // header de 6 bytes (seq u16 BE, timestamp RTP u32 BE). Nunca no TCP do
    // vídeo (head-of-line blocking). audio_port 0 = desligado.
    int                   audio_port = 9004;
    int                   audio_fd = -1;
    uint32_t              audio_dst_logged = 0;
    std::atomic<uint64_t> audio_fwd{0};
    std::atomic<uint64_t> audio_fwd_err{0};
    std::atomic<uint64_t> keyframe_requests{0};
    std::mutex ice_local_mu;
    std::vector<std::string> ice_local_candidates;
    std::atomic<PeerConnectionState> pc_state{PEER_CONNECTION_NEW};
    std::string ice_ufrag_local;
    std::string ice_ufrag_remote;
    std::chrono::steady_clock::time_point t_start;
    std::chrono::steady_clock::time_point t_pc_created;
    std::chrono::steady_clock::time_point t_pc_connected;

    // Optional TCP output server (Phase B integration).
    TcpOutServer* tcp_out = nullptr;

    // xCloud data channel protocol state.
    PeerConnection* pc = nullptr;   // Only touched from callbacks / main.
    std::atomic<bool> handshake_done{false};
    std::atomic<bool> channels_opened{false};
    /* v0.9.10: pedimos 720p30 em vez de 720p60 pra xCloud.
     * Justificativa: Xbox 360 h264play cap real é ~20-30fps por causa do
     * convert YUV→RGB CPU (worker). Rec 60fps só sobrecarrega decode e
     * encher ring. Pedindo 30 diretamente, xCloud encoda metade dos frames,
     * decoder do Xbox tem metade da carga, ring drena mais rápido, latência
     * de input cai proporcional. Bitrate 3000kbps (era 5000) — cliente
     * consegue decodar sem stall, xCloud não reduz adaptativo. */
    std::atomic<int>  video_kbps{3000};
    int  video_fps  = 30;
    int  video_w    = 1280;
    int  video_h    = 720;

    // Fase 1: pedidos de keyframe vindos de fora da thread do peer (TCP-out
    // congestionado, cliente novo, Xbox via UDP "KFRQ") só marcam flag; a
    // pc_thread atende com throttle de 1/s, igual ao Green-NX
    // (Engine::request_keyframe_locked). Cada IDR é um pico grande de bytes:
    // pedir um por pacote perdido realimentava a congestão.
    std::atomic<bool>     kf_wanted{false};
    std::atomic<bool>     kf_force{false};    // ignora throttle (cliente novo)
    uint64_t              last_kf_ms = 0;     // só pc_thread
    uint64_t              last_rr_ms = 0;     // só pc_thread
    std::atomic<uint64_t> nacks_sent{0};

    // ---- Input channel state (P11) -------------------------------------
    // Green-NX behaviour: the "input" DataChannel must receive a fresh
    // absolute gamepad snapshot at 125 Hz (every 8ms) with a monotonic
    // sequence, or xCloud closes the peer connection after ~3s of silence.
    // We start a heartbeat thread the moment handshake_done flips and stop
    // it when tearing the session down. A parallel UDP :9001 listener lets
    // the Xbox 360 push real input into current_gamepad; when nothing
    // arrives, we still send the zero frame (menu stays put but session
    // survives — enough to prove decode/render throughput on real gameplay).
    std::mutex               input_mu;
    // v0.19: até 4 controles do Xbox (byte 0 do frame UDP = slot).
    // present: slots que já mandaram frame; announced: já avisados ao xCloud
    // com gamepadChanged (o 0 vai no handshake).
    static constexpr int kMaxPads = 4;
    gnx::xcloud::GamepadFrame current_gamepad[kMaxPads]{};
    std::atomic<unsigned>    pad_present{1};
    std::atomic<unsigned>    pad_announced{1};
    gnx::xcloud::InputSerializer input_ser;
    std::atomic<bool>        input_run{false};
    std::thread              input_send_thread;
    std::thread              input_udp_thread;
    int                      input_udp_fd = -1;
    std::chrono::steady_clock::time_point t_input_start;
};

static CaptureCtx g_ctx;

static const uint8_t ANNEXB_START[4] = {0x00, 0x00, 0x00, 0x01};

// ------------------------- RTP -> H.264 depayloader ------------------------
//
// Green-NX's libpeer patch REPLACES libpeer's H.264 NAL assembler with a
// pass-through that hands us the RAW RTP packet (12-byte header + payload).
// Motivation quoted from their rtp.c:
//     "libpeer's own in-arrival-order NALU assembly corrupted frames on any
//      reorder/loss and produced green garbage, so it is bypassed."
// So we own reassembly here. Minimal impl: parse RTP header, drop padding
// bytes, then depayload RFC 6184 single-NAL / STAP-A / FU-A into Annex-B AUs
// keyed by RTP timestamp. Emit on marker bit. No jitter/NACK — kept simple
// (works fine on LAN + stable xCloud path; if we see decoding artifacts on
// Xbox, port green-nx VideoJitterBuffer as follow-up).
class H264Depayloader {
 public:
    using Emit = std::function<void(const uint8_t* au, size_t n)>;
    void set_emit(Emit e) { emit_ = std::move(e); }

    void feed(const uint8_t* rtp, size_t len) {
        if (len < 12) return;
        uint8_t cc = rtp[0] & 0x0f;
        bool ext = (rtp[0] & 0x10) != 0;
        bool padding = (rtp[0] & 0x20) != 0;
        bool marker = (rtp[1] & 0x80) != 0;
        uint32_t ts = (uint32_t(rtp[4]) << 24) | (rtp[5] << 16) |
                      (rtp[6] << 8) | rtp[7];
        size_t hdr = 12 + 4u * cc;
        if (len < hdr) return;
        if (ext) {
            if (len < hdr + 4) return;
            size_t words = (rtp[hdr + 2] << 8) | rtp[hdr + 3];
            hdr += 4 + 4 * words;
            if (len < hdr) return;
        }
        size_t plen = len - hdr;
        if (padding && plen > 0) {
            uint8_t pad = rtp[len - 1];
            plen = (pad <= plen) ? plen - pad : 0;
        }
        if (plen == 0) return;
        const uint8_t* p = rtp + hdr;

        // On a new timestamp, flush the previous AU.
        if (au_started_ && ts != au_ts_) flush_au();
        au_ts_ = ts;
        au_started_ = true;

        depayload(p, plen);

        if (marker) flush_au();
    }

 private:
    void append_startcoded(const uint8_t* d, size_t n) {
        static const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
        au_.insert(au_.end(), sc, sc + 4);
        au_.insert(au_.end(), d, d + n);
    }
    void flush_au() {
        if (!au_.empty() && emit_) emit_(au_.data(), au_.size());
        au_.clear();
        au_started_ = false;
    }
    void depayload(const uint8_t* p, size_t len) {
        if (len == 0) return;
        uint8_t t = p[0] & 0x1f;
        if (t >= 1 && t <= 23) {
            append_startcoded(p, len);
        } else if (t == 24) {  // STAP-A
            size_t i = 1;
            while (i + 2 <= len) {
                size_t n = (p[i] << 8) | p[i + 1];
                i += 2;
                if (!n || i + n > len) break;
                append_startcoded(p + i, n);
                i += n;
            }
        } else if (t == 28) {  // FU-A
            if (len < 2) return;
            bool start = (p[1] & 0x80) != 0;
            if (start) {
                uint8_t nal_hdr = (p[0] & 0xe0) | (p[1] & 0x1f);
                static const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
                au_.insert(au_.end(), sc, sc + 4);
                au_.push_back(nal_hdr);
            }
            au_.insert(au_.end(), p + 2, p + len);
        }
        // Other NAL types (25-27, 29-31) ignored — not used by xCloud.
    }

    std::vector<uint8_t> au_;
    bool au_started_ = false;
    uint32_t au_ts_ = 0;
    Emit emit_;
};

static H264Depayloader g_depayloader;                 // legado — não usado mais
static xcloud360::VideoJitterBuffer g_video_jitter;   // portado de green-nx

// Forward declarations pra callbacks do jitter buffer alcançarem essas funções.
static bool send_on_channel_text(PeerConnection* pc, const char* label,
                                 const std::string& msg);
static bool send_on_channel_binary(PeerConnection* pc, const char* label,
                                   const std::vector<uint8_t>& data);

static uint64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// PLI + xCloud videoKeyframeRequested, no máximo 1/s (Green-NX
// Engine::request_keyframe_locked). Só chamar da pc_thread — callbacks do
// libpeer rodam nela; outras threads usam ctx->kf_wanted.
static void request_keyframe_throttled(CaptureCtx* ctx, bool force) {
    if (!ctx->pc || !ctx->handshake_done.load()) return;
    uint64_t now = steady_ms();
    if (!force && now - ctx->last_kf_ms < 1000) return;
    ctx->last_kf_ms = now;
    peer_connection_request_keyframe(ctx->pc);
    send_on_channel_text(ctx->pc, "control",
        gnx::xcloud::video_keyframe_requested());
    ctx->keyframe_requests.fetch_add(1);
}

// libpeer (green-nx patched) delivers each RAW RTP packet (12-byte header +
// payload). Substituímos H264Depayloader por VideoJitterBuffer com reorder +
// waiting_keyframe gating. Se um frame perder pacote, manda RTCP NACK e espera
// a retransmissão; se não vier, drop + pedir novo IDR — nunca alimenta decoder
// com P-frame quebrado. Callbacks setados em main junto com tcp_out.
static void on_video_track(uint8_t* data, size_t size, void* userdata) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    if (size == 0) return;
    bool want_kf = false;
    g_video_jitter.receive(
        data, size, steady_ms(),
        // emit — AU pronto e in-order
        [ctx](const uint8_t* au, size_t n) {
            if (ctx->h264_fp) std::fwrite(au, 1, n, ctx->h264_fp);
            ctx->video_bytes.fetch_add(n);
            ctx->video_nals.fetch_add(1);
            if (ctx->tcp_out) ctx->tcp_out->push_au(au, n);
        },
        // nack — retransmissão (RFC 4585). Antes pedia um IDR inteiro a cada
        // pacote perdido/fora de ordem: tempestade de keyframes.
        [ctx](uint16_t pid, uint16_t blp) {
            if (ctx->pc) {
                peer_connection_send_nack(ctx->pc, pid, blp);
                ctx->nacks_sent.fetch_add(1);
            }
        },
        &want_kf);
    if (want_kf) request_keyframe_throttled(ctx, false);
}

// Depayloader emit target. Called with a complete Annex-B access unit.
static void on_h264_au(const uint8_t* au, size_t n, CaptureCtx* ctx) {
    if (n == 0) return;
    if (ctx->h264_fp) std::fwrite(au, 1, n, ctx->h264_fp);
    ctx->video_bytes.fetch_add(n);
    ctx->video_nals.fetch_add(1);
    if (ctx->tcp_out) ctx->tcp_out->push_au(au, n);
}

// v0.18: até a v0.17 o callback saía no `!opus_fp` ANTES de contar — o
// launcher roda com --no-file, então o áudio chegava (libpeer entrega o pacote
// RTP inteiro, já decifrado) e era jogado fora sem nem aparecer no log.
// Roda na thread do peer_connection_loop: só parse + 1 sendto não bloqueante.
static void on_audio_track(uint8_t* data, size_t size, void* userdata) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    if (size < 12 || (data[0] >> 6) != 2) return;
    if (ctx->opus_fp) std::fwrite(data, 1, size, ctx->opus_fp);   // como antes: RTP cru
    size_t off = 12 + 4u * (data[0] & 0x0f);                       // CSRCs
    if ((data[0] & 0x10) && off + 4 <= size)                       // header extension
        off += 4 + 4u * (((size_t)data[off + 2] << 8) | data[off + 3]);
    size_t end = size;
    if ((data[0] & 0x20) && end > off) {                           // padding
        size_t pad = data[end - 1];
        end = pad <= end - off ? end - pad : off;
    }
    if (off >= end) return;
    const size_t n = end - off;
    ctx->audio_bytes.fetch_add(n);
    ctx->audio_frames.fetch_add(1);

    if (ctx->audio_fd < 0 || !ctx->tcp_out || n > 1500) return;
    uint32_t ip = ctx->tcp_out->client_ip();
    if (!ip) return;                                               // Xbox ainda não conectou
    if (ip != ctx->audio_dst_logged) {
        ctx->audio_dst_logged = ip;
        in_addr ia{}; ia.s_addr = ip;
        std::printf("[audio] Opus -> %s:%d (UDP, seq+ts+payload)\n",
                    inet_ntoa(ia), ctx->audio_port);
    }
    uint8_t pkt[6 + 1500];
    std::memcpy(pkt, data + 2, 2);                                 // seq (já BE)
    std::memcpy(pkt + 2, data + 4, 4);                             // timestamp (já BE)
    std::memcpy(pkt + 6, data + off, n);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)ctx->audio_port);
    to.sin_addr.s_addr = ip;
    ssize_t w = ::sendto(ctx->audio_fd, pkt, 6 + n, MSG_DONTWAIT, (sockaddr*)&to, sizeof(to));
    if (w == (ssize_t)(6 + n)) ctx->audio_fwd.fetch_add(1);
    else ctx->audio_fwd_err.fetch_add(1);
}

static void on_request_keyframe(void* userdata) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    ctx->keyframe_requests.fetch_add(1);
}

static void on_ice_candidate(char* sdp_text, void* userdata) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    std::string cand(sdp_text);
    std::lock_guard<std::mutex> lk(ctx->ice_local_mu);
    ctx->ice_local_candidates.push_back(cand);
    // Best-effort ufrag extraction from a=candidate line (unlikely in the
    // raw candidate but harmless — Green-NX's send_ice_candidates uses the
    // ice-ufrag we pass alongside).
}

// --------- xCloud data channel protocol ---------
//
// After DTLS+SCTP come up, we must open control/input/message/chat data
// channels with pre-assigned SIDs (0/2/4/6, matching the official web
// client's setup). xCloud's server sends a handshake on the "message"
// channel; we reply with authorization + gamepad + resolution + client
// capabilities. Only then does the server start sending RTP video packets.
//
// Refs: green-nx/src/switch/stream/engine.cpp handle_channel_message().

static bool send_on_channel_text(PeerConnection* pc, const char* label,
                                 const std::string& payload) {
    if (!pc) return false;
    uint16_t sid = 0;
    if (peer_connection_lookup_sid(pc, label, &sid) != 0) {
        std::fprintf(stderr, "[dc] send FAILED: no channel '%s'\n", label);
        return false;
    }
    int rc = peer_connection_datachannel_send_text_sid(
        pc, const_cast<char*>(payload.data()), payload.size(), sid);
    std::printf("[dc] send [%s sid=%u len=%zu] %.180s%s\n",
                label, sid, payload.size(), payload.c_str(),
                payload.size() > 180 ? "..." : "");
    return rc >= 0;
}

static bool send_on_channel_binary(PeerConnection* pc, const char* label,
                                   const std::vector<uint8_t>& payload) {
    if (!pc) return false;
    uint16_t sid = 0;
    if (peer_connection_lookup_sid(pc, label, &sid) != 0) return false;
    int rc = peer_connection_datachannel_send_sid(
        pc, const_cast<char*>(reinterpret_cast<const char*>(payload.data())),
        payload.size(), sid);
    // Input sai a até 125Hz — logar cada pacote só enche o log.
    if (rc < 0 || std::strcmp(label, "input") != 0)
        std::printf("[dc] send-bin [%s sid=%u len=%zu] rc=%d\n", label, sid, payload.size(), rc);
    return rc >= 0;
}

// ---- P11: input heartbeat + UDP listener --------------------------------
//
// Wire format (24B, little-endian) on UDP :9001 from Xbox:
//   [0]     index (usually 0 — controller slot)
//   [1]     buttons_lo: a|b|x|y|dpad_up|dpad_down|dpad_left|dpad_right (bit0..7)
//   [2]     buttons_hi: menu|view|nexus|LB|RB|LThumb|RThumb|_ (bit0..6)
//   [3..4]  LT  uint16 0..65535
//   [5..6]  RT  uint16 0..65535
//   [7..8]  LX  int16  -32768..32767
//   [9..10] LY  int16
//   [11..12] RX int16
//   [13..14] RY int16
//   [15..23] reserved
static void input_send_loop(CaptureCtx* ctx) {
    using namespace std::chrono;
    /* On-change + heartbeat pattern: envia sempre que o estado do controle
     * mudar E pelo menos a cada 50ms (heartbeat pra manter absolute state
     * "vivo" no encoder xCloud). Se send falha (SCTP EAGAIN), backoff
     * escalonado: 8ms → 20ms → 100ms. Evita saturar peer connection e
     * degradar canal RTP de video que corre no MESMO ICE.
     * v0.19: o mesmo, por controle; controle novo é anunciado com
     * gamepadChanged antes do primeiro frame dele. */
    constexpr int kMaxPads = CaptureCtx::kMaxPads;
    gnx::xcloud::GamepadFrame last_sent[kMaxPads]{};
    steady_clock::time_point last_send_time[kMaxPads];
    for (auto& t : last_send_time) t = steady_clock::now();
    int consec_fails = 0;
    while (ctx->input_run.load()) {
        if (!ctx->handshake_done.load() || ctx->pc == nullptr) {
            std::this_thread::sleep_for(milliseconds(20));
            continue;
        }
        PeerConnectionState st = ctx->pc_state.load();
        if (st != PEER_CONNECTION_CONNECTED
            && st != PEER_CONNECTION_COMPLETED) {
            std::this_thread::sleep_for(milliseconds(100));
            consec_fails = 0;
            continue;
        }
        unsigned present = ctx->pad_present.load();
        for (int i = 1; i < kMaxPads; i++) {
            unsigned bit = 1u << i;
            if ((present & bit) && !(ctx->pad_announced.load() & bit)) {
                send_on_channel_text(ctx->pc, "control",
                                     gnx::xcloud::gamepad_changed((uint8_t)i, true));
                ctx->pad_announced.fetch_or(bit);
                std::printf("[input] controle %d do Xbox -> gamepadChanged(%d, added)\n", i, i);
            }
        }

        bool failed = false;
        for (int i = 0; i < kMaxPads && !failed; i++) {
            if (!(present & (1u << i))) continue;
            auto now = steady_clock::now();
            double elapsed_ms =
                duration_cast<duration<double, std::milli>>(
                    now - ctx->t_input_start).count();

            gnx::xcloud::GamepadFrame snapshot;
            {
                std::lock_guard<std::mutex> lk(ctx->input_mu);
                snapshot = ctx->current_gamepad[i];
            }
            /* State-change detect (byte-compare struct). Heartbeat 50ms. */
            bool changed = std::memcmp(&snapshot, &last_sent[i], sizeof(snapshot)) != 0;
            auto since_last = duration_cast<milliseconds>(now - last_send_time[i]).count();
            if (!changed && since_last < 50) continue;

            std::vector<uint8_t> pkt;
            bool has_press = snapshot.a || snapshot.b || snapshot.x || snapshot.y
                          || snapshot.dpad_up || snapshot.dpad_down
                          || snapshot.dpad_left || snapshot.dpad_right
                          || snapshot.menu || snapshot.view;
            {
                std::lock_guard<std::mutex> lk(ctx->input_mu);
                pkt = ctx->input_ser.gamepad_packet(snapshot, elapsed_ms);
            }
            static bool logged_first_send = false;
            if (has_press && !logged_first_send) {
                std::printf("[input-send] first frame with button press queued (pkt %zuB)\n", pkt.size());
                logged_first_send = true;
            }
            bool ok = send_on_channel_binary(ctx->pc, "input", pkt);
            if (ok) {
                last_sent[i] = snapshot;
                last_send_time[i] = now;
                consec_fails = 0;
            } else {
                {
                    std::lock_guard<std::mutex> lk(ctx->input_mu);
                    ctx->input_ser.rollback_sequence();
                }
                consec_fails++;
                failed = true;
            }
        }
        if (failed) {
            /* Backoff pra não afogar SCTP nem log. 8ms → 20ms → 100ms. */
            int wait_ms = (consec_fails < 3) ? 8
                        : (consec_fails < 10) ? 20 : 100;
            std::this_thread::sleep_for(milliseconds(wait_ms));
        } else {
            std::this_thread::sleep_for(milliseconds(4));
        }
    }
}

static void input_udp_loop(CaptureCtx* ctx, int port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("[input-udp] socket"); return; }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("[input-udp] bind");
        ::close(fd);
        return;
    }
    // Short recv timeout so we can honour input_run without a shutdown pipe.
    struct timeval to{0, 200000};   // 200 ms
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    ctx->input_udp_fd = fd;
    std::printf("[input-udp] listening 0.0.0.0:%d for Xbox gamepad frames\n", port);

    auto rd_u16 = [](const uint8_t* p) -> uint16_t {
        return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    };
    auto rd_i16 = [](const uint8_t* p) -> int16_t {
        return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    };

    uint8_t buf[512];
    while (ctx->input_run.load()) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        // Fase 1: Xbox pede IDR depois de erro de decode / backlog.
        if (n == 4 && std::memcmp(buf, "KFRQ", 4) == 0) {
            ctx->kf_wanted.store(true);
            continue;
        }
        // Telemetria do h264play (v0.14.2+): decode/exibição/backlog a cada 2s.
        if (n > 5 && std::memcmp(buf, "STAT ", 5) == 0) {
            std::printf("[xbox] %.*s\n", (int)n - 5, reinterpret_cast<const char*>(buf) + 5);
            continue;
        }
        if (n < 15) continue;   // too short (timeout returns -1/EAGAIN too)
        gnx::xcloud::GamepadFrame f{};
        f.index = buf[0];
        uint8_t b0 = buf[1], b1 = buf[2];
        f.a = b0 & 1;  f.b = (b0 >> 1) & 1;  f.x = (b0 >> 2) & 1;  f.y = (b0 >> 3) & 1;
        f.dpad_up    = (b0 >> 4) & 1;  f.dpad_down  = (b0 >> 5) & 1;
        f.dpad_left  = (b0 >> 6) & 1;  f.dpad_right = (b0 >> 7) & 1;
        f.menu = b1 & 1;   f.view = (b1 >> 1) & 1;   f.nexus = (b1 >> 2) & 1;
        f.left_shoulder  = (b1 >> 3) & 1;  f.right_shoulder = (b1 >> 4) & 1;
        f.left_thumb     = (b1 >> 5) & 1;  f.right_thumb    = (b1 >> 6) & 1;
        f.left_trigger  = rd_u16(buf + 3) / 65535.0f;
        f.right_trigger = rd_u16(buf + 5) / 65535.0f;
        f.left_x  = rd_i16(buf + 7)  / 32767.0f;
        f.left_y  = rd_i16(buf + 9)  / 32767.0f;
        f.right_x = rd_i16(buf + 11) / 32767.0f;
        f.right_y = rd_i16(buf + 13) / 32767.0f;
        {
            if (f.index >= CaptureCtx::kMaxPads) f.index = 0;
            std::lock_guard<std::mutex> lk(ctx->input_mu);
            ctx->current_gamepad[f.index] = f;
        }
        ctx->pad_present.fetch_or(1u << f.index);
        /* Log-once when any button is first seen from Xbox — proves the
         * UDP recv path is being processed, not just the socket bound. */
        static bool logged_first_press = false;
        if (!logged_first_press && (b0 || b1)) {
            std::printf("[input-udp] first non-zero payload: b0=0x%02x b1=0x%02x -> a=%d b=%d x=%d y=%d up=%d\n",
                        b0, b1, f.a, f.b, f.x, f.y, f.dpad_up);
            logged_first_press = true;
        }
    }
    ctx->input_udp_fd = -1;
    ::close(fd);
    std::printf("[input-udp] listener stopped\n");
}

static void start_input_threads(CaptureCtx* ctx) {
    if (ctx->input_run.exchange(true)) return;   // already running
    ctx->t_input_start = std::chrono::steady_clock::now();
    ctx->input_send_thread = std::thread(input_send_loop, ctx);
    ctx->input_udp_thread  = std::thread(input_udp_loop, ctx, 9001);
    std::printf("[input] heartbeat 125Hz + UDP :9001 listener spawned\n");
}

static void stop_input_threads(CaptureCtx* ctx) {
    if (!ctx->input_run.exchange(false)) return;
    if (ctx->input_send_thread.joinable()) ctx->input_send_thread.join();
    if (ctx->input_udp_thread.joinable())  ctx->input_udp_thread.join();
    std::printf("[input] threads joined\n");
}

static void on_channel_open(void* userdata) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    if (ctx->channels_opened.exchange(true)) return;
    std::printf("[dc] SCTP up — creating data channels + sending handshake\n");
    // libpeer's create_datachannel_sid only registers the label->sid mapping
    // AFTER the DCEP OPEN datagram flushes over SCTP. If we create the
    // channels before SCTP is ready (setup time), the send fails silently
    // and lookup_sid() returns -1 forever. Green-NX defers the CREATE to
    // this callback for exactly that reason.
    struct { const char* label; const char* proto; uint16_t sid; } chans[] = {
        {"control", "controlV1", 0},
        {"input",   "1.0",       2},
        {"message", "messageV1", 4},
        {"chat",    "chatV1",    6},
    };
    for (const auto& c : chans) {
        peer_connection_create_datachannel_sid(
            ctx->pc, DATA_CHANNEL_RELIABLE, 0, 0,
            (char*)c.label, (char*)c.proto, c.sid);
    }
    // Fire the message-channel handshake. xCloud replies with a handshakeAck
    // on the same channel — on_channel_message picks it up and runs the
    // startup sequence.
    send_on_channel_text(ctx->pc, "message", gnx::xcloud::message_handshake());
}

static void on_channel_message(char* data, size_t size, void* userdata, uint16_t sid) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    if (!ctx->pc) return;
    char* label = peer_connection_lookup_sid_label(ctx->pc, sid);
    std::string preview(data, std::min<size_t>(size, 240));
    // Canal input (vibração/ack, várias por segundo) não vai pro log.
    if (!label || std::strcmp(label, "input") != 0)
        std::printf("[dc] recv [%s sid=%u len=%zu] %s\n",
                    label ? label : "sid?", sid, size, preview.c_str());
    if (!label) return;

    if (std::strcmp(label, "message") == 0 && !ctx->handshake_done.load()) {
        std::string payload(data, size);
        if (gnx::xcloud::is_handshake_ack(payload)) {
            std::printf("[dc] handshakeAck received — running startup sequence\n");
            ctx->handshake_done.store(true);
            // Authorize control channel + announce a plugged-in gamepad slot
            // (mandatory or xCloud rejects further control commands).
            send_on_channel_text(ctx->pc, "control", gnx::xcloud::authorization_request());
            send_on_channel_text(ctx->pc, "control", gnx::xcloud::gamepad_changed(0, true));
            // v0.19: sessão nova: os controles 1-3 que já estão no Xbox são
            // anunciados de novo pelo input_send_loop.
            ctx->pad_announced.store(1);
            // Ask for 720p (matches session tier).
            send_on_channel_text(ctx->pc, "control", gnx::xcloud::user_requested_resolution("720"));
            // Startup messages carry ClientDeviceCapabilities + quality knobs
            // (max bitrate, framerate, resolution). This is what makes xCloud
            // start pushing RTP video.
            for (const std::string& m :
                 gnx::xcloud::startup_messages(ctx->video_w, ctx->video_h,
                                               ctx->video_kbps.load(),
                                               ctx->video_fps))
                send_on_channel_text(ctx->pc, "message", m);
            // Kick the input channel with an empty client_metadata packet so
            // the server marks it as opened (some titles won't start video
            // until at least one input packet arrives).
            gnx::xcloud::InputSerializer input;
            send_on_channel_binary(ctx->pc, "input", input.client_metadata());
            // P11: keep the input channel fed at 125Hz (Green-NX-verified
            // cadence) or xCloud closes the session after ~3s of silence.
            start_input_threads(ctx);
            // Ask for an IDR immediately so video frames appear in <1s
            // instead of at the next periodic keyframe (which can be 5-10s).
            send_on_channel_text(ctx->pc, "control",
                                 gnx::xcloud::video_keyframe_requested());
            peer_connection_request_keyframe(ctx->pc);  // RTCP PLI too.
            std::printf("[dc] startup sequence sent, awaiting first video NAL\n");
        }
    }
}

static void on_state_change(PeerConnectionState state, void* userdata) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    ctx->pc_state.store(state);
    std::printf("[pc] state -> %s\n", peer_connection_state_to_string(state));
    if (state == PEER_CONNECTION_CONNECTED
        || state == PEER_CONNECTION_COMPLETED) {
        if (ctx->t_pc_connected.time_since_epoch().count() == 0) {
            ctx->t_pc_connected = std::chrono::steady_clock::now();
        }
    }
}

// Extract a=ice-ufrag: value from an SDP blob. Best-effort; returns "" if
// not present or malformed.
static std::string extract_ufrag(const std::string& sdp) {
    static const std::string tag = "a=ice-ufrag:";
    size_t p = sdp.find(tag);
    if (p == std::string::npos) return {};
    size_t end = sdp.find_first_of("\r\n", p + tag.size());
    if (end == std::string::npos) end = sdp.size();
    return sdp.substr(p + tag.size(), end - p - tag.size());
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

int main(int argc, char** argv) {
    // Force line-buffered stdout so progress logs appear immediately when
    // stdout is a pipe/file (nohup redirect). Fully-buffered would delay
    // the first output by up to 4 KiB — which for a long-running signaling
    // + queue phase means the log looks stalled for minutes.
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    // ---- args ------------------------------------------------------------
    // v0.18: --help de verdade. Antes "--help" virava TITLE_ID e abria sessão.
    bool want_help = argc < 2;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) want_help = true;
    }
    if (want_help) {
        std::fprintf(stderr,
            "usage: %s <TITLE_ID> [--out DIR] [--seconds N] [--ads]\n"
            "                     [--tcp-out PORT] [--no-file] [--kbps N] [--fps N]\n"
            "  --tcp-out PORT  Serve length-prefix Annex-B AUs on this TCP port.\n"
            "  --no-file       Skip writing stream.h264 (useful when --tcp-out is set).\n"
            "  --kbps N        Max video bitrate asked from xCloud (default 3000).\n"
            "  --fps N         Framerate asked from xCloud (default 30).\n"
            "  --drop-nonref   Don't forward non-reference frames to the TCP client.\n"
            "  --audio-port N  UDP port on the video client (Xbox) for the Opus\n"
            "                  track: [seq u16 BE][rtp ts u32 BE][payload]. Default\n"
            "                  9004, 0 = don't forward audio.\n",
            argv[0]);
        return argc < 2 ? 2 : 0;
    }
    std::string title_id = argv[1];
    std::string out_root = "tools/xcloud-capture/output";
    int capture_secs = 30;
    bool ad_supported = false;
    int  tcp_out_port = 0;   // 0 = no TCP-out (capture-only mode)
    bool no_file = false;    // skip stream.h264 file when true
    bool drop_nonref = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)     { out_root = argv[++i]; }
        else if (a == "--seconds" && i + 1 < argc) { capture_secs = std::atoi(argv[++i]); }
        else if (a == "--ads")                { ad_supported = true; }
        else if (a == "--tcp-out" && i + 1 < argc) { tcp_out_port = std::atoi(argv[++i]); }
        else if (a == "--no-file")            { no_file = true; }
        else if (a == "--kbps" && i + 1 < argc)    { g_ctx.video_kbps.store(std::atoi(argv[++i])); }
        else if (a == "--fps" && i + 1 < argc)     { g_ctx.video_fps = std::atoi(argv[++i]); }
        else if (a == "--drop-nonref")        { drop_nonref = true; }
        else if (a == "--audio-port" && i + 1 < argc) { g_ctx.audio_port = std::atoi(argv[++i]); }
    }

    // ---- output dir -------------------------------------------------------
    char ts[32];
    std::time_t now = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::string out_dir = out_root + "/" + title_id + "-rtp-" + ts;
    ::mkdir(out_root.c_str(), 0755);
    if (::mkdir(out_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "mkdir %s failed: %s\n", out_dir.c_str(), strerror(errno));
        return 3;
    }
    g_ctx.out_dir = out_dir;
    g_ctx.t_start = std::chrono::steady_clock::now();
    g_depayloader.set_emit([&](const uint8_t* au, size_t n){
        on_h264_au(au, n, &g_ctx);
    });
    std::printf("[cap] title=%s  out=%s  seconds=%d\n",
                title_id.c_str(), out_dir.c_str(), capture_secs);

    // ---- auth chain -------------------------------------------------------
    const char* home = std::getenv("HOME");
    std::string token_path = std::string(home ? home : ".") + "/.green-nx-tokens.json";
    gnx::XboxAuth auth(token_path);
    if (!auth.has_saved_login()) {
        std::fprintf(stderr, "no saved login. Run tools/xcloud-capture/login.sh first.\n");
        return 4;
    }
    gnx::Http http;
    std::printf("[cap] fetching streaming credentials...\n");
    gnx::StreamingCredentials creds = auth.fetch_streaming_credentials();
    const gnx::EndpointCredentials& endpoint =
        ad_supported ? *creds.cloud_f2p : creds.cloud;
    std::printf("[cap] endpoint host: %s\n", endpoint.host.c_str());

    std::printf("[cap] fetching passport token...\n");
    std::string passport = auth.fetch_passport_token();

    // ---- libpeer global init (once per process) --------------------------
    std::printf("[cap] peer_init()\n");
    peer_init();
    g_ctx.t_pc_created = std::chrono::steady_clock::now();

    // ---- retry loop: bring up session + PC + DTLS ------------------------
    // Root cause for retry: xCloud sometimes provisions a session where
    // ICE completes but the DTLS peer never sends ServerHello — media
    // path is dead server-side. Green-NX documents this: "a session can
    // come up with a dead media path (ICE connects, DTLS never answers)
    // -- a fresh session re-rolls that server-side fault."
    //
    // Green-NX v1.0.30 pattern (adapted from engine.cpp worker()):
    //   max_transport_retries = 2 (cloud) / 8 (home)
    //   dead_media_timeout_ms = 12000 (ICE up + DTLS silent = dead)
    //   retry_cooldown_ms = 3000 (cloud) / 5000 (home) — deixa xCloud
    //     liberar o slot server-side antes de pedir sessao nova, senao a
    //     nova request queue em "waiting for resources".
    //
    // 5+ attempts vira contra-produtivo: xCloud tem session cap por conta
    // e cada allocacao a mais so' aumenta a chance de bater o limit. 2 e'
    // o sweet spot medido pela comunidade Green-NX/Greenlight/LIGH-IS-GREEN.
    const int MAX_SESSION_ATTEMPTS = 2;
    const int DTLS_WAIT_SECS       = 12;   // dead-media window (green-nx)
    PeerConnection* pc = nullptr;
    gnx::GssvSession* session_ptr = nullptr;
    std::atomic<bool> stop_flag{false};
    std::thread pc_thread, ice_thread, ka_thread;
    std::string offer_sdp, answer_sdp;

    auto teardown_attempt = [&](bool destroy_pc){
        stop_flag.store(true);
        if (ka_thread.joinable())  ka_thread.join();
        if (ice_thread.joinable()) ice_thread.join();
        if (pc_thread.joinable())  pc_thread.join();
        stop_input_threads(&g_ctx);
        if (destroy_pc && pc) {
            peer_connection_close(pc);
            peer_connection_destroy(pc);
            pc = nullptr;
        }
        if (session_ptr) {
            try { session_ptr->stop(); } catch (...) {}
            delete session_ptr;
            session_ptr = nullptr;
        }
        stop_flag.store(false);
    };

    bool session_ok = false;
    int last_attempt = 0;
    for (int attempt = 1; attempt <= MAX_SESSION_ATTEMPTS; ++attempt) {
        last_attempt = attempt;
        std::printf("\n[cap] ===== SESSION ATTEMPT %d/%d =====\n",
                    attempt, MAX_SESSION_ATTEMPTS);

        // Reset per-attempt shared state.
        {
            std::lock_guard<std::mutex> lk(g_ctx.ice_local_mu);
            g_ctx.ice_local_candidates.clear();
        }
        g_ctx.pc_state.store(PEER_CONNECTION_NEW);
        g_ctx.t_pc_connected = std::chrono::steady_clock::time_point{};

        // Fresh PC per attempt — libpeer state (dtls_srtp, agent, sctp) is
        // baked in at create time.
        PeerConfiguration pc_cfg = {};
        pc_cfg.video_codec  = CODEC_H264;
        pc_cfg.audio_codec  = CODEC_OPUS;
        pc_cfg.datachannel  = DATA_CHANNEL_BINARY;
        pc_cfg.onvideotrack = on_video_track;
        pc_cfg.onaudiotrack = on_audio_track;
        pc_cfg.on_request_keyframe = on_request_keyframe;
        pc_cfg.user_data    = &g_ctx;
        pc_cfg.ice_servers[0].urls = "stun:stun.l.google.com:19302";
        pc = peer_connection_create(&pc_cfg);
        if (!pc) {
            std::fprintf(stderr, "peer_connection_create failed\n");
            peer_deinit();
            return 5;
        }
        peer_connection_oniceconnectionstatechange(pc, on_state_change);
        peer_connection_onicecandidate(pc, on_ice_candidate);
        // Register datachannel handlers BEFORE channels are opened, so the
        // first inbound SCTP message (xCloud's handshakeAck) hits our
        // callback. If we don't, libpeer drops the payload silently.
        g_ctx.pc = pc;
        g_ctx.channels_opened.store(false);
        g_ctx.handshake_done.store(false);
        peer_connection_ondatachannel(pc, on_channel_message,
                                          on_channel_open, nullptr);
        // Channels are opened lazily inside on_channel_open once SCTP is up.
        // Creating them earlier would race the DCEP OPEN and leave lookup_sid
        // unmapped forever.

        // Fresh session per attempt.
        if (attempt == 1) {
            std::printf("[cap] cleanup_stale_sessions()\n");
            gnx::GssvSession::cleanup_stale_sessions(http, endpoint);
        }
        session_ptr = new gnx::GssvSession(http, endpoint, gnx::QualityTier::P720);
        auto& session = *session_ptr;
        std::printf("[cap] start_cloud(%s), tier=720p\n", title_id.c_str());
        try {
            session.start_cloud(title_id);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "start_cloud failed: %s\n", e.what());
            teardown_attempt(true);
            continue;
        }

        bool session_connected = false;
        bool provisioned = false;
        for (int i = 0; i < 1800; ++i) {
            auto state = session.refresh_state();
            std::printf("[sess] state: %d\n", (int)state);
            if (state == gnx::SessionState::ReadyToConnect && !session_connected) {
                std::printf("[sess] sending connect (passport)\n");
                session.connect(passport);
                session_connected = true;
            }
            if (state == gnx::SessionState::Provisioned) {
                std::printf("[sess] Provisioned — moving to SDP exchange\n");
                provisioned = true;
                break;
            }
            if (state == gnx::SessionState::Failed) {
                std::fprintf(stderr, "session failed: %s\n",
                             session.error_details().c_str());
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!provisioned) { teardown_attempt(true); continue; }

        stop_flag.store(false);
        pc_thread = std::thread([&]{
            // Green-NX libpeer patch makes peer_connection_loop return >0 if
            // it processed an inbound datagram. Drain the socket at full
            // speed on hot bursts (keyframe H264 = 40+ packets in one shot).
            while (!stop_flag.load()) {
                int drained = peer_connection_loop(pc);
                bool force = g_ctx.kf_force.exchange(false);
                // Enquanto o cliente TCP espera IDR, repete o pedido (1/s):
                // o xCloud às vezes ignora um PLI e o próximo IDR periódico
                // demora vários segundos — tudo descartado nesse meio tempo.
                bool starving = g_ctx.tcp_out && g_ctx.tcp_out->waiting_idr();
                if (g_ctx.kf_wanted.exchange(false) || force || starving)
                    request_keyframe_throttled(&g_ctx, force);
                // RTCP Receiver Report + REMB a cada 1s (Green-NX engine.cpp
                // "receiver etiquette"). Sem REMB o xCloud ignora
                // maxBitrateKbps: pedimos 1500 e veio ~2.7Mbps.
                uint64_t now_ms = steady_ms();
                if (g_ctx.handshake_done.load() && now_ms - g_ctx.last_rr_ms >= 1000) {
                    g_ctx.last_rr_ms = now_ms;
                    uint8_t fraction; uint32_t cumulative, highest_ext;
                    if (g_video_jitter.report_stats(&fraction, &cumulative, &highest_ext))
                        peer_connection_send_receiver_report(pc, fraction, cumulative,
                                                             highest_ext, 0);
                    peer_connection_send_remb(
                        pc, (uint32_t)g_ctx.video_kbps.load() * 1000u);
                }
                if (drained <= 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                // else: immediate re-poll, no sleep.
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // ---- SDP exchange ------------------------------------------------
        std::printf("[sdp] creating local offer\n");
        const char* offer_sdp_raw = peer_connection_create_offer(pc);
        if (!offer_sdp_raw) {
            std::fprintf(stderr, "peer_connection_create_offer failed\n");
            teardown_attempt(true); continue;
        }
        offer_sdp = offer_sdp_raw;
        g_ctx.ice_ufrag_local = extract_ufrag(offer_sdp);
        {
            std::string needle = "a=setup:passive";
            std::string repl   = "a=setup:actpass";
            size_t p = 0;
            while ((p = offer_sdp.find(needle, p)) != std::string::npos) {
                offer_sdp.replace(p, needle.size(), repl);
                p += repl.size();
            }
        }
        {
            // Teto de banda do vídeo no offer (b=AS kbps + b=TIAS bps), como os
            // tweaks de xCloud no navegador. Só a seção m=video, logo depois do
            // c= dela. O REMB sozinho não é respeitado (combate passa de 3Mbps).
            int kbps = g_ctx.video_kbps.load();
            size_t mv = offer_sdp.find("m=video");
            size_t c  = mv == std::string::npos ? mv : offer_sdp.find("\nc=", mv);
            size_t nx = mv == std::string::npos ? mv : offer_sdp.find("\nm=", mv + 1);
            if (kbps > 0 && c != std::string::npos && (nx == std::string::npos || c < nx)) {
                size_t eol = offer_sdp.find('\n', c + 1);
                if (eol != std::string::npos) {
                    bool crlf = eol > 0 && offer_sdp[eol - 1] == '\r';
                    std::string nl = crlf ? "\r\n" : "\n";
                    std::string as = "b=AS:" + std::to_string(kbps);
                    std::string tias = "b=TIAS:" + std::to_string((long long)kbps * 1000);
                    offer_sdp.insert(eol + 1, as + nl + tias + nl);
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        std::printf("[sdp] video bandwidth cap: %s %s (m=video)\n",
                                    as.c_str(), tias.c_str());
                    }
                }
            } else {
                std::printf("[sdp] WARN: m=video/c= not found, no b=AS cap\n");
            }
        }
        std::printf("[sdp] offer size=%zu  ice-ufrag(local)=%s\n",
                    offer_sdp.size(), g_ctx.ice_ufrag_local.c_str());
        write_file(out_dir + "/offer.sdp", offer_sdp);

        std::printf("[sdp] exchange_sdp -> xCloud\n");
        try {
            answer_sdp = session.exchange_sdp(offer_sdp);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "exchange_sdp failed: %s\n", e.what());
            teardown_attempt(true); continue;
        }
        g_ctx.ice_ufrag_remote = extract_ufrag(answer_sdp);
        std::printf("[sdp] answer size=%zu  ice-ufrag(remote)=%s\n",
                    answer_sdp.size(), g_ctx.ice_ufrag_remote.c_str());
        write_file(out_dir + "/answer.sdp", redact_secrets(answer_sdp));

        // Pre-SRD ICE bridge (same as before).
        {
            std::vector<std::string> local_now;
            {
                std::lock_guard<std::mutex> lk(g_ctx.ice_local_mu);
                local_now = g_ctx.ice_local_candidates;
            }
            if (local_now.empty()) {
                size_t p = 0;
                while ((p = offer_sdp.find("a=candidate:", p)) != std::string::npos) {
                    size_t end = offer_sdp.find_first_of("\r\n", p);
                    std::string cand = offer_sdp.substr(p + 2, end - p - 2);
                    local_now.push_back(cand);
                    p = end;
                }
            }
            if (!local_now.empty()) {
                try {
                    session.send_ice_candidates(local_now, g_ctx.ice_ufrag_local);
                    std::printf("[ice] pre-SRD: sent %zu local candidates\n", local_now.size());
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[ice] pre-SRD send failed: %s\n", e.what());
                }
            }
            int remote_polled = 0;
            for (int i = 0; i < 40 && remote_polled == 0; i++) {
                try {
                    bool end_flag = false;
                    auto remotes = session.receive_ice_candidates(&end_flag);
                    if (!remotes.empty()) {
                        for (auto& c : remotes) {
                            std::printf("[ice] pre-SRD +remote: %.80s...\n", c.c_str());
                            peer_connection_add_ice_candidate(pc, (char*)c.c_str());
                            remote_polled++;
                        }
                        break;
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[ice] pre-SRD poll: %s\n", e.what());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            std::printf("[ice] pre-SRD done: %d remote candidates in hand\n", remote_polled);
        }

        peer_connection_set_remote_description(pc,
            (char*)answer_sdp.c_str(), SDP_TYPE_ANSWER);

        ice_thread = std::thread([&]{
            std::vector<std::string> sent_local;
            bool remote_end = false;
            while (!stop_flag.load()) {
                std::vector<std::string> to_send;
                {
                    std::lock_guard<std::mutex> lk(g_ctx.ice_local_mu);
                    if (g_ctx.ice_local_candidates.size() > sent_local.size()) {
                        to_send.assign(
                            g_ctx.ice_local_candidates.begin() + sent_local.size(),
                            g_ctx.ice_local_candidates.end());
                        sent_local = g_ctx.ice_local_candidates;
                    }
                }
                if (!to_send.empty()) {
                    try {
                        session.send_ice_candidates(to_send, g_ctx.ice_ufrag_local);
                        std::printf("[ice] sent %zu local candidates\n", to_send.size());
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "send_ice_candidates: %s\n", e.what());
                    }
                }
                if (!remote_end) {
                    try {
                        auto remote = session.receive_ice_candidates(&remote_end);
                        for (auto& c : remote) {
                            std::printf("[ice] +remote: %.80s...\n", c.c_str());
                            peer_connection_add_ice_candidate(pc, (char*)c.c_str());
                        }
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "receive_ice_candidates: %s\n", e.what());
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

        ka_thread = std::thread([&]{
            while (!stop_flag.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (stop_flag.load()) break;
                try { session.keepalive(); } catch (...) {}
            }
        });

        // ---- wait for DTLS to actually complete (state COMPLETED, not
        //      just CONNECTED which is ICE-only). ------------------------
        // Green-NX dead-media detection: mede tempo a partir do momento em
        // que ICE chega em CONNECTED/COMPLETED. Se apos DTLS_WAIT_SECS o
        // pc_state ainda nao virou COMPLETED, e' dead-media path (ICE up
        // mas DTLS/SCTP nunca responderam). Distinto de FAILED cru (que
        // vira imediatamente sem esperar).
        std::chrono::steady_clock::time_point ice_connected_at{};
        auto hard_deadline = std::chrono::steady_clock::now()
                           + std::chrono::seconds(DTLS_WAIT_SECS + 5);
        bool dead_media = false;
        while (std::chrono::steady_clock::now() < hard_deadline) {
            auto st = g_ctx.pc_state.load();
            if (st == PEER_CONNECTION_COMPLETED) break;
            if (st == PEER_CONNECTION_FAILED
                || st == PEER_CONNECTION_DISCONNECTED
                || st == PEER_CONNECTION_CLOSED) break;

            // Marca o instante que ICE conectou pela primeira vez.
            if (ice_connected_at.time_since_epoch().count() == 0
                && (st == PEER_CONNECTION_CONNECTED
                    || st == PEER_CONNECTION_COMPLETED)) {
                ice_connected_at = std::chrono::steady_clock::now();
                std::printf("[cap] ICE connected — starting %ds DTLS window\n",
                            DTLS_WAIT_SECS);
            }

            // Dead-media: ICE up ha' DTLS_WAIT_SECS + DTLS ainda nao COMPLETED.
            if (ice_connected_at.time_since_epoch().count() != 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - ice_connected_at).count();
                if (elapsed > DTLS_WAIT_SECS && st != PEER_CONNECTION_COMPLETED) {
                    dead_media = true;
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        auto st_final = g_ctx.pc_state.load();
        if (st_final == PEER_CONNECTION_COMPLETED) {
            std::printf("[cap] session %d SUCCESS — DTLS up, proceeding to capture\n",
                        attempt);
            session_ok = true;
            break;
        }
        if (dead_media) {
            std::fprintf(stderr,
                "[cap] session %d DEAD MEDIA (ICE up but DTLS/SCTP silent for %ds) "
                "-- server-side fault, retrying with fresh session\n",
                attempt, DTLS_WAIT_SECS);
        } else {
            std::fprintf(stderr,
                "[cap] session %d failed (state=%s) -- unusual, retrying anyway\n",
                attempt, peer_connection_state_to_string(st_final));
        }
        teardown_attempt(true);
        if (attempt < MAX_SESSION_ATTEMPTS) {
            // Cooldown 3s pra xCloud liberar o slot da conta server-side.
            // Sem isso, a nova session request queue em "waiting for
            // resources" e a segunda tentativa tambem falha por rate.
            std::printf("[cap] cooldown 3s before next attempt...\n");
            for (int i = 0; i < 30; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    if (!session_ok) {
        std::fprintf(stderr, "[cap] all %d attempts failed — giving up\n", MAX_SESSION_ATTEMPTS);
        peer_deinit();
        return 9;
    }

    // ---- open dump files (only after we know DTLS is up) ------------------
    if (!no_file) {
        g_ctx.h264_fp = std::fopen((out_dir + "/stream.h264").c_str(), "wb");
        g_ctx.opus_fp = std::fopen((out_dir + "/audio.opus.raw").c_str(), "wb");
        if (!g_ctx.h264_fp) {
            std::fprintf(stderr, "can't open stream.h264\n");
        }
    }

    // ---- optional TCP-out server (Phase B integration) --------------------
    TcpOutServer* tcp_srv = nullptr;
    if (tcp_out_port > 0) {
        tcp_srv = new TcpOutServer(tcp_out_port);
        if (!tcp_srv->start()) {
            std::fprintf(stderr, "[cap] TCP-out failed to bind :%d\n", tcp_out_port);
            delete tcp_srv;
            tcp_srv = nullptr;
        } else {
            tcp_srv->set_drop_nonref(drop_nonref);
            // v0.18: socket do áudio antes de publicar tcp_out (o callback do
            // áudio roda na thread do pc e lê os dois).
            if (g_ctx.audio_port > 0 && g_ctx.audio_port < 65536) {
                int afd = ::socket(AF_INET, SOCK_DGRAM, 0);
                if (afd >= 0) {
                    ::fcntl(afd, F_SETFL, ::fcntl(afd, F_GETFL) | O_NONBLOCK);
                    int tos = 0xb8;   // DSCP EF, como áudio de VoIP
                    ::setsockopt(afd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
                    g_ctx.audio_fd = afd;
                    std::printf("[audio] forward Opus to <video client>:%d/udp\n", g_ctx.audio_port);
                } else {
                    std::perror("[audio] socket");
                }
            }
            std::atomic_thread_fence(std::memory_order_release);
            g_ctx.tcp_out = tcp_srv;
            // On every fresh TCP client, ask the server for an IDR so the
            // Xbox decoder can start rendering without waiting for the next
            // periodic keyframe (5-10s otherwise). Fire both RTCP PLI (from
            // libpeer) and the xCloud app-level keyframe request message.
            // Runs on the accept thread: only flag it, pc_thread does the send.
            tcp_srv->set_on_new_client([&]{ g_ctx.kf_force.store(true); });
            // Fila TCP estourou e passou a descartar até o próximo IDR.
            tcp_srv->set_on_congestion([&]{ g_ctx.kf_wanted.store(true); });
        }
    }

    // ---- capture N seconds ------------------------------------------------
    g_video_jitter.reset();
    std::printf("[cap] CONNECTED, capturing %d seconds of RTP...\n", capture_secs);
    for (int s = 0; s < capture_secs; s++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto js = g_video_jitter.stats();
        std::printf("[cap] t+%2ds  video: %llu AUs / %llu bytes | "
                    "audio: %llu frames / %llu bytes fwd=%llu | kfr_req: %llu | "
                    "jit: pkts=%u drops=%u nacks=%u resyncs=%u %s| "
                    "tcp: dropAU=%llu congest=%llu(q%llu/a%llu) nonref=%llu/%llu\n",
                    s + 1,
                    (unsigned long long)g_ctx.video_nals.load(),
                    (unsigned long long)g_ctx.video_bytes.load(),
                    (unsigned long long)g_ctx.audio_frames.load(),
                    (unsigned long long)g_ctx.audio_bytes.load(),
                    (unsigned long long)g_ctx.audio_fwd.load(),
                    (unsigned long long)g_ctx.keyframe_requests.load(),
                    js.packets, js.dropped, js.nacks, js.resyncs,
                    g_video_jitter.waiting_keyframe() ? "[WAIT_KF] " : "",
                    (unsigned long long)(tcp_srv ? tcp_srv->dropped() : 0),
                    (unsigned long long)(tcp_srv ? tcp_srv->congestion_events() : 0),
                    (unsigned long long)(tcp_srv ? tcp_srv->congest_queue() : 0),
                    (unsigned long long)(tcp_srv ? tcp_srv->congest_age() : 0),
                    (unsigned long long)(tcp_srv ? tcp_srv->nonref_dropped() : 0),
                    (unsigned long long)(tcp_srv ? tcp_srv->nonref_seen() : 0));
    }

    // ---- shutdown ---------------------------------------------------------
    std::printf("[cap] stopping...\n");
    stop_flag.store(true);
    if (tcp_srv) { tcp_srv->stop(); delete tcp_srv; g_ctx.tcp_out = nullptr; }
    if (g_ctx.h264_fp) std::fclose(g_ctx.h264_fp);
    if (g_ctx.opus_fp) std::fclose(g_ctx.opus_fp);
    if (ka_thread.joinable())  ka_thread.join();
    if (ice_thread.joinable()) ice_thread.join();
    if (pc_thread.joinable())  pc_thread.join();
    stop_input_threads(&g_ctx);
    if (pc) {
        peer_connection_close(pc);
        peer_connection_destroy(pc);
    }
    peer_deinit();

    if (session_ptr) {
        try { session_ptr->stop(); } catch (...) {}
        delete session_ptr;
        session_ptr = nullptr;
    }
    (void)last_attempt;

    // ---- artifacts --------------------------------------------------------
    json local_ice = json::array();
    {
        std::lock_guard<std::mutex> lk(g_ctx.ice_local_mu);
        for (auto& c : g_ctx.ice_local_candidates) local_ice.push_back(c);
    }
    write_file(out_dir + "/ice-local.json", local_ice.dump(2));

    json meta = {
        {"title_id",           title_id},
        {"endpoint_host",      endpoint.host},
        {"offering",           ad_supported ? "xgpuwebf2p" : "xgpuweb"},
        {"tier",               "P720"},
        {"capture_seconds",    capture_secs},
        {"video_nals",         g_ctx.video_nals.load()},
        {"video_bytes",        g_ctx.video_bytes.load()},
        {"audio_frames",       g_ctx.audio_frames.load()},
        {"audio_bytes",        g_ctx.audio_bytes.load()},
        {"audio_forwarded",    g_ctx.audio_fwd.load()},
        {"audio_forward_errors", g_ctx.audio_fwd_err.load()},
        {"keyframe_requests",  g_ctx.keyframe_requests.load()},
        {"final_pc_state",     peer_connection_state_to_string(g_ctx.pc_state.load())},
        {"ice_ufrag_local_prefix",  g_ctx.ice_ufrag_local.substr(0, 4) + "..."},
        {"ice_ufrag_remote_prefix", g_ctx.ice_ufrag_remote.substr(0, 4) + "..."},
    };
    write_file(out_dir + "/session-meta.json", meta.dump(2));

    std::printf("[cap] done. output: %s\n", out_dir.c_str());
    std::printf("       stream.h264       = %llu NALs, %llu bytes\n",
                (unsigned long long)g_ctx.video_nals.load(),
                (unsigned long long)g_ctx.video_bytes.load());
    return 0;
}

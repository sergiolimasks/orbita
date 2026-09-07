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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include "auth.hpp"
#include "session.hpp"
#include "http.hpp"
#include "json.hpp"

extern "C" {
#include "peer.h"
#include "peer_connection.h"
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

struct CaptureCtx {
    std::string out_dir;
    std::FILE* h264_fp = nullptr;
    std::FILE* opus_fp = nullptr;
    std::atomic<uint64_t> video_bytes{0};
    std::atomic<uint64_t> video_nals{0};
    std::atomic<uint64_t> audio_bytes{0};
    std::atomic<uint64_t> audio_frames{0};
    std::atomic<uint64_t> keyframe_requests{0};
    std::mutex ice_local_mu;
    std::vector<std::string> ice_local_candidates;
    std::atomic<PeerConnectionState> pc_state{PEER_CONNECTION_NEW};
    std::string ice_ufrag_local;
    std::string ice_ufrag_remote;
    std::chrono::steady_clock::time_point t_start;
    std::chrono::steady_clock::time_point t_pc_created;
    std::chrono::steady_clock::time_point t_pc_connected;
};

static CaptureCtx g_ctx;

static const uint8_t ANNEXB_START[4] = {0x00, 0x00, 0x00, 0x01};

// libpeer's onvideotrack fires with each parsed NAL (already unwrapped from
// RTP FU-A / STAP-A / single-NAL per RFC 6184). We just prefix with an
// Annex-B start code and write.
static void on_video_track(uint8_t* data, size_t size, void* userdata) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    if (!ctx->h264_fp || size == 0) return;
    std::fwrite(ANNEXB_START, 1, sizeof(ANNEXB_START), ctx->h264_fp);
    std::fwrite(data, 1, size, ctx->h264_fp);
    ctx->video_bytes.fetch_add(size + sizeof(ANNEXB_START));
    ctx->video_nals.fetch_add(1);
}

static void on_audio_track(uint8_t* data, size_t size, void* userdata) {
    auto* ctx = static_cast<CaptureCtx*>(userdata);
    if (!ctx->opus_fp || size == 0) return;
    std::fwrite(data, 1, size, ctx->opus_fp);
    ctx->audio_bytes.fetch_add(size);
    ctx->audio_frames.fetch_add(1);
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
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <TITLE_ID> [--out DIR] [--seconds N] [--ads]\n",
            argv[0]);
        return 2;
    }
    std::string title_id = argv[1];
    std::string out_root = "tools/xcloud-capture/output";
    int capture_secs = 30;
    bool ad_supported = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)     { out_root = argv[++i]; }
        else if (a == "--seconds" && i + 1 < argc) { capture_secs = std::atoi(argv[++i]); }
        else if (a == "--ads")                { ad_supported = true; }
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
    // Diagnosed via instrumented libpeer: our ClientHello leaves, 0
    // non-STUN packets ever arrive. Fix = teardown, request a new session.
    const int MAX_SESSION_ATTEMPTS = 5;
    const int DTLS_WAIT_SECS       = 20;   // ICE→DTLS handshake budget
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
        peer_connection_create_datachannel(pc, DATA_CHANNEL_RELIABLE, 0, 0,
            (char*)"input", (char*)"1.0");
        peer_connection_create_datachannel(pc, DATA_CHANNEL_RELIABLE, 0, 0,
            (char*)"control", (char*)"controlV1");
        peer_connection_create_datachannel(pc, DATA_CHANNEL_RELIABLE, 0, 0,
            (char*)"message", (char*)"messageV1");

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
            while (!stop_flag.load()) {
                peer_connection_loop(pc);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
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
        auto dtls_deadline = std::chrono::steady_clock::now()
                           + std::chrono::seconds(DTLS_WAIT_SECS);
        while (std::chrono::steady_clock::now() < dtls_deadline) {
            auto st = g_ctx.pc_state.load();
            if (st == PEER_CONNECTION_COMPLETED) break;
            if (st == PEER_CONNECTION_FAILED
                || st == PEER_CONNECTION_DISCONNECTED
                || st == PEER_CONNECTION_CLOSED) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        auto st_final = g_ctx.pc_state.load();
        if (st_final == PEER_CONNECTION_COMPLETED) {
            std::printf("[cap] session %d SUCCESS — DTLS up, proceeding to capture\n",
                        attempt);
            session_ok = true;
            break;
        }
        std::fprintf(stderr, "[cap] session %d dead media (state=%s, DTLS never completed within %ds)\n",
                     attempt, peer_connection_state_to_string(st_final), DTLS_WAIT_SECS);
        teardown_attempt(true);
        // brief pause to let xCloud release the slot before requesting a new one
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    if (!session_ok) {
        std::fprintf(stderr, "[cap] all %d attempts failed — giving up\n", MAX_SESSION_ATTEMPTS);
        peer_deinit();
        return 9;
    }

    // ---- open dump files (only after we know DTLS is up) ------------------
    g_ctx.h264_fp = std::fopen((out_dir + "/stream.h264").c_str(), "wb");
    g_ctx.opus_fp = std::fopen((out_dir + "/audio.opus.raw").c_str(), "wb");
    if (!g_ctx.h264_fp) {
        std::fprintf(stderr, "can't open stream.h264\n");
    }

    // ---- capture N seconds ------------------------------------------------
    std::printf("[cap] CONNECTED, capturing %d seconds of RTP...\n", capture_secs);
    for (int s = 0; s < capture_secs; s++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("[cap] t+%2ds  video: %llu NALs / %llu bytes | "
                    "audio: %llu frames / %llu bytes | kfr: %llu\n",
                    s + 1,
                    (unsigned long long)g_ctx.video_nals.load(),
                    (unsigned long long)g_ctx.video_bytes.load(),
                    (unsigned long long)g_ctx.audio_frames.load(),
                    (unsigned long long)g_ctx.audio_bytes.load(),
                    (unsigned long long)g_ctx.keyframe_requests.load());
    }

    // ---- shutdown ---------------------------------------------------------
    std::printf("[cap] stopping...\n");
    stop_flag.store(true);
    if (g_ctx.h264_fp) std::fclose(g_ctx.h264_fp);
    if (g_ctx.opus_fp) std::fclose(g_ctx.opus_fp);
    if (ka_thread.joinable())  ka_thread.join();
    if (ice_thread.joinable()) ice_thread.join();
    if (pc_thread.joinable())  pc_thread.join();
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

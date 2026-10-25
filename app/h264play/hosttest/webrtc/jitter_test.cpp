// XCloud360 stage 4 — differential test: xc_jitter.c (C, fixed memory) vs the
// C++ VideoJitterBuffer capture-rtp uses today (tools/xcloud-capture/rtp/
// video_jitter.cpp). Random H.264 RTP traces with loss, reordering,
// duplicates, late retransmissions and sequence wrap; every emitted AU, every
// NACK (pid/blp), every want_keyframe and the RR statistics must be identical.
//   usage: jitter_test [seeds=200]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "video_jitter.hpp"
extern "C" {
#include "xc_jitter.h"
}

struct Pkt {
    std::vector<uint8_t> b;
    uint64_t t;
};

static uint32_t rng;
static uint32_t rnd() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static std::vector<uint8_t> rtp(uint16_t seq, uint32_t ts, bool marker, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> p(12);
    p[0] = 0x80;
    p[1] = (uint8_t)((marker ? 0x80 : 0) | 102);
    p[2] = seq >> 8;
    p[3] = seq & 0xff;
    p[4] = ts >> 24;
    p[5] = ts >> 16;
    p[6] = ts >> 8;
    p[7] = ts;
    p[8] = 0x12; p[9] = 0x34; p[10] = 0x56; p[11] = 0x78;
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

// one access unit -> RTP packets (STAP-A SPS+PPS on IDR, single NAL or FU-A)
static void packetize(std::vector<Pkt>& out, uint16_t& seq, uint32_t ts, bool idr, uint64_t t) {
    std::vector<std::vector<uint8_t>> pkts;
    if (idr) {
        std::vector<uint8_t> stap = {24, 0, 4, 0x67, 0x42, 0xe0, 0x1f, 0, 3, 0x68, 0xce, 0x3c};
        pkts.push_back(stap);
    }
    size_t size = idr ? 3000 + rnd() % 20000 : 200 + rnd() % 6000;
    uint8_t hdr = idr ? 0x65 : ((rnd() % 4) ? 0x41 : 0x01);   // some non-ref P
    std::vector<uint8_t> nal(size);
    nal[0] = hdr;
    for (size_t i = 1; i < size; i++) nal[i] = (uint8_t)(rnd() | 1);
    if (size <= 1100 && (rnd() & 1)) {
        pkts.push_back(nal);
    } else {
        size_t off = 1;
        bool first = true;
        while (off < size) {
            size_t n = std::min<size_t>(1100, size - off);
            std::vector<uint8_t> fu = {(uint8_t)((hdr & 0xe0) | 28),
                                       (uint8_t)((first ? 0x80 : 0) | (off + n >= size ? 0x40 : 0) | (hdr & 0x1f))};
            fu.insert(fu.end(), nal.begin() + off, nal.begin() + off + n);
            pkts.push_back(fu);
            off += n;
            first = false;
        }
    }
    for (size_t i = 0; i < pkts.size(); i++) out.push_back({rtp(seq++, ts, i + 1 == pkts.size(), pkts[i]), t});
}

struct Rec {
    std::vector<std::string> ev;
};
static Rec* g_rec;
static void c_emit(void* u, const uint8_t* au, size_t n, uint32_t ts) {
    char h[64];
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) sum = sum * 31 + au[i];
    snprintf(h, sizeof(h), "AU ts=%u n=%zu h=%08x", ts, n, sum);
    ((Rec*)u)->ev.push_back(h);
}
static void c_nack(void* u, uint16_t pid, uint16_t blp) {
    char h[48];
    snprintf(h, sizeof(h), "NACK %u %04x", pid, blp);
    ((Rec*)u)->ev.push_back(h);
}

int main(int argc, char** argv) {
    int seeds = argc > 1 ? atoi(argv[1]) : 200;
    int failures = 0;
    long total_aus = 0, total_nacks = 0, total_pk = 0;
    static xcj_t cj;
    if (xcj_init(&cj) != 0) { std::puts("alloc failed"); return 2; }
    for (int seed = 1; seed <= seeds; seed++) {
        rng = 0x9e3779b9u ^ (uint32_t)seed * 2654435761u;
        // trace
        std::vector<Pkt> sent;
        uint16_t seq = (uint16_t)(seed % 3 == 0 ? 65000 + rnd() % 500 : rnd());   // wrap cases
        uint32_t ts = rnd();
        int frames = 150 + rnd() % 150;
        uint64_t t = 1000;
        for (int f = 0; f < frames; f++) {
            packetize(sent, seq, ts, f % 60 == 0 || (rnd() % 97 == 0), t);
            ts += 3000;
            t += 16;
        }
        // network: loss, late retransmission, reorder, duplicates
        double loss = (seed % 4) * 0.03;   // 0..9 %
        std::vector<Pkt> net;
        std::vector<Pkt> retrans;
        for (auto& p : sent) {
            uint64_t at = p.t + rnd() % 3;
            if ((rnd() % 10000) < loss * 10000) {
                if (rnd() % 3) retrans.push_back({p.b, at + 5 + rnd() % 140});   // NACK-ed resend, maybe too late
                continue;
            }
            net.push_back({p.b, at});
            if (rnd() % 100 < 2) net.push_back({p.b, at + rnd() % 5});           // duplicate
        }
        net.insert(net.end(), retrans.begin(), retrans.end());
        // stable sort by time, then light adjacent reordering
        std::stable_sort(net.begin(), net.end(), [](const Pkt& a, const Pkt& b) { return a.t < b.t; });
        for (size_t i = 1; i < net.size(); i++)
            if (rnd() % 100 < 5) std::swap(net[i - 1], net[i]);

        xcloud360::VideoJitterBuffer cpp;
        cpp.reset();
        xcj_reset(&cj);
        Rec rc, rp;
        uint64_t now = 0;
        for (size_t i = 0; i < net.size(); i++) {
            now = std::max(now, net[i].t);
            bool kp = false;
            cpp.receive(net[i].b.data(), net[i].b.size(), now,
                        [&](const uint8_t* au, size_t n) {
                            // ts of the emitted AU is not passed by the C++ API:
                            // compare via size+hash only
                            char h[64];
                            unsigned sum = 0;
                            for (size_t k = 0; k < n; k++) sum = sum * 31 + au[k];
                            snprintf(h, sizeof(h), "AU n=%zu h=%08x", n, sum);
                            rp.ev.push_back(h);
                        },
                        [&](uint16_t pid, uint16_t blp) {
                            char h[48];
                            snprintf(h, sizeof(h), "NACK %u %04x", pid, blp);
                            rp.ev.push_back(h);
                        },
                        &kp);
            int kc = 0;
            g_rec = &rc;
            xcj_receive(&cj, net[i].b.data(), net[i].b.size(), now, c_emit, c_nack, &rc, &kc);
            if (kp) rp.ev.push_back("KF");
            if (kc) rc.ev.push_back("KF");
            if (i % 97 == 0) {
                uint8_t f1, f2;
                uint32_t c1, c2, h1, h2;
                bool a = cpp.report_stats(&f1, &c1, &h1);
                int b = xcj_report_stats(&cj, &f2, &c2, &h2);
                char s1[64], s2[64];
                snprintf(s1, sizeof(s1), "RR %d %u %u %u", a ? 1 : 0, a ? f1 : 0, a ? c1 : 0, a ? h1 : 0);
                snprintf(s2, sizeof(s2), "RR %d %u %u %u", b, b ? f2 : 0, b ? c2 : 0, b ? h2 : 0);
                rp.ev.push_back(s1);
                rc.ev.push_back(s2);
            }
        }
        // strip the ts from the C records (the C++ API does not expose it)
        for (auto& e : rc.ev)
            if (e.rfind("AU ts=", 0) == 0) e = "AU " + e.substr(e.find(' ', 6) + 1);
        auto s = cpp.stats();
        char a1[128], a2[128];
        snprintf(a1, sizeof(a1), "STATS %u %u %u %u %u", s.packets, s.frames, s.dropped, s.nacks, s.resyncs);
        snprintf(a2, sizeof(a2), "STATS %u %u %u %u %u", cj.st.packets, cj.st.frames, cj.st.dropped, cj.st.nacks,
                 cj.st.resyncs);
        rp.ev.push_back(a1);
        rc.ev.push_back(a2);
        bool same = rp.ev == rc.ev;
        if (!same) {
            failures++;
            if (failures <= 3) {
                std::printf("seed %d: MISMATCH (%zu vs %zu events)\n", seed, rp.ev.size(), rc.ev.size());
                for (size_t k = 0; k < std::max(rp.ev.size(), rc.ev.size()); k++) {
                    const char* x = k < rp.ev.size() ? rp.ev[k].c_str() : "-";
                    const char* y = k < rc.ev.size() ? rc.ev[k].c_str() : "-";
                    if (strcmp(x, y)) {
                        std::printf("  first diff @%zu: C++ '%s'  C '%s'\n", k, x, y);
                        break;
                    }
                }
            }
        }
        total_aus += s.frames;
        total_nacks += s.nacks;
        total_pk += s.packets;
    }
    std::printf("jitter differential: %d/%d seeds identical (%ld packets, %ld AUs, %ld NACKs compared)\n",
                seeds - failures, seeds, total_pk, total_aus, total_nacks);
    xcj_free(&cj);
    return failures ? 1 : 0;
}

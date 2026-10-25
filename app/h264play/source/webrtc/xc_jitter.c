/*
 * XCloud360 stage 4 — see xc_jitter.h. Close port (GPL-3.0) of green-nx's
 * VideoJitterBuffer via tools/xcloud-capture/rtp/video_jitter.cpp, with fixed
 * memory. Unit test: hosttest/webrtc/jitter_test.c (compares against the C++
 * class on randomized loss/reorder traces).
 */
#include "xc_jitter.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

static int ts_newer(uint32_t a, uint32_t b) {
    uint32_t d = a - b;
    return d != 0 && d < (1u << 31);
}

static int seq_before(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) < 0; }

static int parse_rtp(const uint8_t *rtp, size_t len, uint16_t *seq, uint32_t *ts, int *marker,
                     const uint8_t **payload, size_t *payload_len) {
    if (len < 12) return 0;
    uint8_t cc = rtp[0] & 0x0f;
    int ext = (rtp[0] & 0x10) != 0;
    int padding = (rtp[0] & 0x20) != 0;
    *marker = (rtp[1] & 0x80) != 0;
    *seq = (uint16_t)((rtp[2] << 8) | rtp[3]);
    *ts = ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) | ((uint32_t)rtp[6] << 8) | rtp[7];
    size_t header = 12 + 4u * cc;
    if (len < header) return 0;
    if (ext) {
        if (len < header + 4) return 0;
        size_t words = ((size_t)rtp[header + 2] << 8) | rtp[header + 3];
        header += 4 + 4 * words;
        if (len < header) return 0;
    }
    size_t plen = len - header;
    if (padding && plen > 0) {
        uint8_t pad = rtp[len - 1];
        plen = (pad <= plen) ? plen - pad : 0;
    }
    *payload = rtp + header;
    *payload_len = plen;
    return 1;
}

static uint8_t nal_type(const uint8_t *p, size_t len) { return len ? (p[0] & 0x1f) : 0; }

static int is_partition_head(const uint8_t *p, size_t len) {
    uint8_t t = nal_type(p, len);
    if (t == 28) return len > 1 && (p[1] & 0x80) != 0;
    return t >= 1 && t <= 24;
}

/* append with bound check; returns 0 if the AU would overflow */
static int au_put(xcj_t *j, size_t *n, const uint8_t *d, size_t len) {
    if (*n + len > XCJ_MAX_AU_BYTES) return 0;
    memcpy(j->au + *n, d, len);
    *n += len;
    return 1;
}

static int depacketize(xcj_t *j, const uint8_t *p, size_t len, size_t *n) {
    if (!len) return 1;
    uint8_t t = nal_type(p, len);
    if (t >= 1 && t <= 23) {
        return au_put(j, n, kStartCode, 4) && au_put(j, n, p, len);
    } else if (t == 24) {
        size_t i = 1;
        while (i + 2 <= len) {
            size_t k = ((size_t)p[i] << 8) | p[i + 1];
            i += 2;
            if (!k || i + k > len) break;
            if (!au_put(j, n, kStartCode, 4) || !au_put(j, n, p + i, k)) return 0;
            i += k;
        }
    } else if (t == 28) {
        if (len < 2) return 1;
        if (p[1] & 0x80) {
            uint8_t h = (uint8_t)((p[0] & 0xe0) | (p[1] & 0x1f));
            if (!au_put(j, n, kStartCode, 4) || !au_put(j, n, &h, 1)) return 0;
        }
        return au_put(j, n, p + 2, len - 2);
    }
    return 1;
}

static int has_keyframe(const uint8_t *au, size_t n) {
    for (size_t i = 0; i + 4 < n; ++i)
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1 && (au[i + 4] & 0x1f) == 5) return 1;
    return 0;
}

/* --------------------------------------------------------------- memory */

int xcj_init(xcj_t *j) {
    memset(j, 0, sizeof(*j));
    j->slot_data = (uint8_t *)malloc((size_t)XCJ_POOL_SLOTS * XCJ_SLOT_PAYLOAD);
    j->slot_len = (uint16_t *)calloc(XCJ_POOL_SLOTS, sizeof(uint16_t));
    j->slot_seq = (uint16_t *)calloc(XCJ_POOL_SLOTS, sizeof(uint16_t));
    j->slot_marker = (uint8_t *)calloc(XCJ_POOL_SLOTS, 1);
    j->free_list = (uint16_t *)calloc(XCJ_POOL_SLOTS, sizeof(uint16_t));
    j->au = (uint8_t *)malloc(XCJ_MAX_AU_BYTES);
    uint16_t *pk = (uint16_t *)calloc((size_t)XCJ_MAX_FRAMES * XCJ_MAX_FRAME_PKTS, sizeof(uint16_t));
    if (!j->slot_data || !j->slot_len || !j->slot_seq || !j->slot_marker || !j->free_list || !j->au || !pk) {
        free(pk);
        xcj_free(j);
        return -1;
    }
    for (int i = 0; i < XCJ_MAX_FRAMES; i++) j->frames[i].pk = pk + (size_t)i * XCJ_MAX_FRAME_PKTS;
    j->pk_block = pk;
    xcj_reset(j);
    return 0;
}

void xcj_free(xcj_t *j) {
    free(j->slot_data);
    free(j->slot_len);
    free(j->slot_seq);
    free(j->slot_marker);
    free(j->free_list);
    free(j->au);
    free(j->pk_block);
    memset(j, 0, sizeof(*j));
}

void xcj_reset(xcj_t *j) {
    j->nfree = XCJ_POOL_SLOTS;
    for (int i = 0; i < XCJ_POOL_SLOTS; i++) j->free_list[i] = (uint16_t)(XCJ_POOL_SLOTS - 1 - i);
    for (int i = 0; i < XCJ_MAX_FRAMES; i++) j->frames[i].npk = 0;
    j->head = 0;
    j->count = 0;
    j->have_last_ts = 0;
    j->last_ts = 0;
    j->waiting_keyframe = 1;
    memset(&j->st, 0, sizeof(j->st));
    j->have_seq = 0;
    j->max_seq = 0;
    j->cycles = j->base_seq = j->received = j->received_prior = j->expected_prior = 0;
}

int xcj_report_stats(xcj_t *j, uint8_t *fraction_lost, uint32_t *cumulative_lost, uint32_t *highest_seq_ext) {
    if (!j->have_seq) return 0;
    uint32_t extended_max = j->cycles + j->max_seq;
    uint32_t expected = extended_max - j->base_seq + 1;
    uint32_t lost = expected > j->received ? expected - j->received : 0;
    uint32_t exp_iv = expected - j->expected_prior;
    uint32_t rcv_iv = j->received - j->received_prior;
    j->expected_prior = expected;
    j->received_prior = j->received;
    uint32_t lost_iv = exp_iv > rcv_iv ? exp_iv - rcv_iv : 0;
    *fraction_lost = (exp_iv == 0 || lost_iv == 0) ? 0 : (uint8_t)((lost_iv << 8) / exp_iv);
    *cumulative_lost = lost & 0x00ffffff;
    *highest_seq_ext = extended_max;
    return 1;
}

/* frames are kept as a ring: logical index i -> frames[(head + i) % MAX].
 * The per-frame pk arrays travel with the slot (frames[] never moves), so an
 * insertion in the middle rotates slot CONTENTS (timestamp/npk/pk pointer). */
static xcj_frame_t *frame_at(xcj_t *j, int i) { return &j->frames[(j->head + i) % XCJ_MAX_FRAMES]; }

static void free_frame_slots(xcj_t *j, xcj_frame_t *f) {
    for (int i = 0; i < f->npk; i++) j->free_list[j->nfree++] = f->pk[i];
    f->npk = 0;
}

static void pop_front(xcj_t *j) {
    xcj_frame_t *f = frame_at(j, 0);
    free_frame_slots(j, f);
    j->head = (j->head + 1) % XCJ_MAX_FRAMES;
    j->count--;
}

static xcj_frame_t *find_or_create(xcj_t *j, uint32_t ts, uint64_t now_ms) {
    for (int i = 0; i < j->count; i++)
        if (frame_at(j, i)->timestamp == ts) return frame_at(j, i);
    if (j->have_last_ts && !ts_newer(ts, j->last_ts)) { j->st.late++; return NULL; }
    if (j->count >= XCJ_MAX_FRAMES) { j->st.pool_full++; return NULL; }
    int pos = 0;
    while (pos < j->count && ts_newer(ts, frame_at(j, pos)->timestamp)) pos++;
    /* open a slot at the end, then bubble it down to `pos` by swapping slot
     * contents (pk pointers are swapped too, so no array is shared) */
    j->count++;
    for (int i = j->count - 1; i > pos; i--) {
        xcj_frame_t tmp = *frame_at(j, i);
        *frame_at(j, i) = *frame_at(j, i - 1);
        *frame_at(j, i - 1) = tmp;
    }
    xcj_frame_t *f = frame_at(j, pos);
    f->timestamp = ts;
    f->first_seen_ms = now_ms;
    f->npk = 0;
    return f;
}

static int try_assemble(xcj_t *j, xcj_frame_t *f, size_t *au_len) {
    if (f->npk == 0) return 0;
    /* marker must exist and be the last packet in sequence order (the C++
     * version sorts by distance to the marker; any packet past the marker
     * breaks its contiguity check the same way) */
    int marker_idx = -1;
    for (int i = 0; i < f->npk; i++)
        if (j->slot_marker[f->pk[i]]) { marker_idx = i; break; }
    if (marker_idx < 0) return 0;
    if (marker_idx != f->npk - 1) return 0;
    uint16_t first = f->pk[0];
    if (!is_partition_head(j->slot_data + (size_t)first * XCJ_SLOT_PAYLOAD, j->slot_len[first])) return 0;
    for (int i = 1; i < f->npk; i++)
        if (j->slot_seq[f->pk[i]] != (uint16_t)(j->slot_seq[f->pk[i - 1]] + 1)) return 0;
    size_t n = 0;
    for (int i = 0; i < f->npk; i++) {
        uint16_t s = f->pk[i];
        if (!depacketize(j, j->slot_data + (size_t)s * XCJ_SLOT_PAYLOAD, j->slot_len[s], &n)) return 0;
    }
    *au_len = n;
    return 1;
}

void xcj_receive(xcj_t *j, const uint8_t *rtp, size_t len, uint64_t now_ms, xcj_emit_fn emit, xcj_nack_fn nack,
                 void *user, int *want_keyframe) {
    uint16_t seq;
    uint32_t ts;
    int marker;
    const uint8_t *payload;
    size_t payload_len;
    if (!parse_rtp(rtp, len, &seq, &ts, &marker, &payload, &payload_len)) return;
    j->st.packets++;

    if (!j->have_seq) {
        j->have_seq = 1;
        j->base_seq = j->max_seq = seq;
        j->cycles = j->received = 0;
    }
    j->received++;
    uint16_t udelta = (uint16_t)(seq - j->max_seq);
    if (udelta != 0 && udelta < 0x8000) {
        if (seq < j->max_seq) j->cycles += 0x10000;
        uint16_t gap = (uint16_t)(udelta - 1);
        if (gap > 0 && gap <= 255) {
            uint16_t pid = (uint16_t)(j->max_seq + 1);
            while (pid != seq) {
                uint16_t blp = 0, nx = (uint16_t)(pid + 1);
                for (int b = 0; b < 16 && nx != seq; ++b, ++nx) blp |= (uint16_t)(1u << b);
                nack(user, pid, blp);
                j->st.nacks++;
                pid = nx;
            }
        }
        j->max_seq = seq;
    }

    if (payload_len == 0) return;
    if (payload_len > XCJ_SLOT_PAYLOAD) { j->st.pool_full++; return; }

    xcj_frame_t *f = find_or_create(j, ts, now_ms);
    if (!f) return;
    for (int i = 0; i < f->npk; i++)
        if (j->slot_seq[f->pk[i]] == seq) { j->st.duplicates++; return; }
    if (j->nfree == 0 || f->npk >= XCJ_MAX_FRAME_PKTS) { j->st.pool_full++; return; }
    uint16_t s = j->free_list[--j->nfree];
    memcpy(j->slot_data + (size_t)s * XCJ_SLOT_PAYLOAD, payload, payload_len);
    j->slot_len[s] = (uint16_t)payload_len;
    j->slot_seq[s] = seq;
    j->slot_marker[s] = (uint8_t)marker;
    /* insert keeping sequence order */
    int pos = f->npk;
    while (pos > 0 && seq_before(seq, j->slot_seq[f->pk[pos - 1]])) {
        f->pk[pos] = f->pk[pos - 1];
        pos--;
    }
    f->pk[pos] = s;
    f->npk++;

    for (;;) {
        if (j->count == 0) break;
        xcj_frame_t *front = frame_at(j, 0);
        size_t au_len = 0;
        if (try_assemble(j, front, &au_len)) {
            uint32_t fts = front->timestamp;
            j->have_last_ts = 1;
            j->last_ts = fts;
            pop_front(j);
            if (j->waiting_keyframe) {
                if (!has_keyframe(j->au, au_len)) {
                    *want_keyframe = 1;
                    j->st.resyncs++;
                    continue;
                }
                j->waiting_keyframe = 0;
            }
            j->st.frames++;
            j->st.last_frame_bytes = (uint32_t)au_len;
            emit(user, j->au, au_len, fts);
            continue;
        }
        if (now_ms - front->first_seen_ms > XCJ_HOLD_MS) {
            j->have_last_ts = 1;
            j->last_ts = front->timestamp;
            pop_front(j);
            j->waiting_keyframe = 1;
            *want_keyframe = 1;
            j->st.dropped++;
            continue;
        }
        break;
    }
}

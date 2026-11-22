#ifndef BTA_AUDIO_DSP_H
#define BTA_AUDIO_DSP_H

#include "protocol.h"

#define BTA_RING_SIZE       0x10000U
#define BTA_RING_HDR        16U
#define BTA_RING_DATA_BYTES 0xfff0U
#define BTA_PKT_FRAMES      192U
#define BTA_FRAME_BYTES     8U
#define BTA_PKT_RING_BYTES  (BTA_PKT_FRAMES * BTA_FRAME_BYTES) /* 1536 bytes */
#define BTA_ASRC_HIST       4U
#define BTA_ASRC_MIN_STEP   0.9985
#define BTA_ASRC_MAX_STEP   1.0015

struct bta_preset_cfg {
    unsigned target_bytes;
    unsigned high_bytes;
    unsigned resync_cap;
    int kern_target;
    int kern_hi;
    int bp_start;
    int bp_floor;
    unsigned long long bp_recover_us;
};

static inline const struct bta_preset_cfg *bta_get_preset_cfg(int preset) {
    static const struct bta_preset_cfg presets[BTA_PRESET_COUNT] = {
        /* 0: Low latency (~20-28 ms total) */
        { 3072U,  7680U, 18432U, 1, 2, 51, 35,  8000000ULL },
        /* 1: Balanced (~30-42 ms total, default) */
        { 4608U, 12288U, 24576U, 1, 3, 51, 32, 10000000ULL },
        /* 2: High stability (~48-64 ms total) */
        { 7680U, 18432U, 36864U, 2, 4, 47, 28, 12000000ULL },
    };
    if (preset < 0 || preset >= BTA_PRESET_COUNT) preset = BTA_PRESET_BALANCED;
    return &presets[preset];
}

struct bta_dsp_state {
    float hist[BTA_ASRC_HIST * 2];
    double phase;
    float err_lpf;
    double integrator;
    double step;
    float cur_gain;
    unsigned rng;
    int primed;
    int xfade_rem;
    float xfade_tail[BTA_PKT_FRAMES * 2];
};

static inline void bta_dsp_reset(struct bta_dsp_state *s, float init_gain) {
    for (unsigned i = 0; i < BTA_ASRC_HIST * 2; i++) s->hist[i] = 0.0f;
    for (unsigned i = 0; i < BTA_PKT_FRAMES * 2; i++) s->xfade_tail[i] = 0.0f;
    s->phase = 0.0;
    s->err_lpf = 0.0f;
    s->integrator = 0.0;
    s->step = 1.0;
    s->cur_gain = init_gain;
    if (!s->rng) s->rng = 0x6d2b79f5U;
    s->primed = 0;
    s->xfade_rem = 0;
}

static inline float bta_target_gain(int volume, int volume_synced) {
    if (volume <= 0) return 0.0f;
    if (volume >= 100) return 1.0f;
    if (volume_synced) return 1.0f;
    float v = (float)volume * 0.01f;
    return v * v;
}

static inline unsigned bta_ring_avail(unsigned writepos, unsigned readpos) {
    return (writepos >= readpos) ? (writepos - readpos) : (BTA_RING_DATA_BYTES - readpos + writepos);
}

static inline unsigned bta_ring_advance(unsigned cursor, unsigned bytes) {
    unsigned off = (cursor - BTA_RING_HDR + bytes) % BTA_RING_DATA_BYTES;
    return BTA_RING_HDR + off;
}

static inline unsigned bta_ring_back(unsigned writepos, unsigned bytes) {
    unsigned off = writepos - BTA_RING_HDR;
    off = (off >= bytes) ? (off - bytes) : (off + BTA_RING_DATA_BYTES - bytes);
    return BTA_RING_HDR + off;
}

static inline unsigned bta_ring_read_bytes(const unsigned char *ring, unsigned cursor,
                                           void *dst, unsigned bytes,
                                           void *(*copy_fn)(void *, const void *, unsigned long)) {
    unsigned first = BTA_RING_SIZE - cursor;
    if (first >= bytes) {
        copy_fn(dst, ring + cursor, bytes);
        cursor += bytes;
        if (cursor == BTA_RING_SIZE) cursor = BTA_RING_HDR;
        return cursor;
    }
    copy_fn(dst, ring + cursor, first);
    copy_fn((unsigned char *)dst + first, ring + BTA_RING_HDR, bytes - first);
    return BTA_RING_HDR + (bytes - first);
}

/* 4-point Catmull-Rom cubic Hermite interpolation at t in [0, 1). */
static inline float bta_hermite4(float y0, float y1, float y2, float y3, float t) {
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

static inline void bta_asrc_update(struct bta_dsp_state *s, unsigned avail_after_bytes,
                                   const struct bta_preset_cfg *cfg, int kern_err_pkts) {
    /* Include kernel queue error (each A2DP packet ~ 8 ms = 3072 ring bytes) as gentle nudge. */
    float target = (float)cfg->target_bytes;
    float eff_avail = (float)avail_after_bytes + (float)kern_err_pkts * 1024.0f;
    float err_frames = (eff_avail - target) * 0.125f;
    s->err_lpf += 0.05f * (err_frames - s->err_lpf);
    s->integrator += (double)s->err_lpf * 2.5e-8;
    if (s->integrator > 0.0012) s->integrator = 0.0012;
    if (s->integrator < -0.0012) s->integrator = -0.0012;
    double step = 1.0 + (double)s->err_lpf * 1.8e-5 + s->integrator;
    if (step > BTA_ASRC_MAX_STEP) step = BTA_ASRC_MAX_STEP;
    if (step < BTA_ASRC_MIN_STEP) step = BTA_ASRC_MIN_STEP;
    s->step = step;
}

static inline unsigned bta_rng_next(struct bta_dsp_state *s) {
    unsigned x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x ? x : 0x6d2b79f5U;
    return s->rng;
}

/* Prime 4 stereo history frames from the ring before resampling begins. */
static inline unsigned bta_dsp_prime(struct bta_dsp_state *s, const unsigned char *ring,
                                     unsigned cursor,
                                     void *(*copy_fn)(void *, const void *, unsigned long)) {
    cursor = bta_ring_read_bytes(ring, cursor, s->hist, BTA_ASRC_HIST * BTA_FRAME_BYTES, copy_fn);
    for (unsigned i = 0; i < BTA_ASRC_HIST * 2; i++) {
        float f = s->hist[i];
        if (f != f) f = 0.0f;
        else if (f > 1.0f) f = 1.0f;
        else if (f < -1.0f) f = -1.0f;
        s->hist[i] = f;
    }
    s->phase = 0.0;
    s->primed = 1;
    return cursor;
}

/* Capture pre-jump tail for a click-free 192-frame crossfade across an overflow resync. */
static inline void bta_dsp_capture_xfade(struct bta_dsp_state *s, const unsigned char *ring,
                                         unsigned cursor,
                                         void *(*copy_fn)(void *, const void *, unsigned long)) {
    bta_ring_read_bytes(ring, cursor, s->xfade_tail, BTA_PKT_RING_BYTES, copy_fn);
    for (unsigned i = 0; i < BTA_PKT_FRAMES * 2; i++) {
        float f = s->xfade_tail[i];
        if (f != f) f = 0.0f;
        else if (f > 1.0f) f = 1.0f;
        else if (f < -1.0f) f = -1.0f;
        s->xfade_tail[i] = f;
    }
    s->xfade_rem = (int)BTA_PKT_FRAMES;
}

/* Consume input frames from ring, resample 192 stereo frames, apply ramped perceptual
 * gain + optional overflow crossfade + TPDF dither + symmetric round-to-int16. */
static inline unsigned bta_dsp_process(struct bta_dsp_state *s, const unsigned char *ring,
                                       unsigned cursor, float target_gain, short out_pcm[384],
                                       unsigned *peak_out, unsigned *nan_out, unsigned *clip_out,
                                       void *(*copy_fn)(void *, const void *, unsigned long)) {
    double step = s->step;
    double end_pos = s->phase + (double)BTA_PKT_FRAMES * step;
    unsigned consume = (unsigned)end_pos;
    if (consume < 190U) consume = 190U;
    if (consume > 194U) consume = 194U;

    float buf[(BTA_ASRC_HIST + 196U) * 2];
    for (unsigned i = 0; i < BTA_ASRC_HIST * 2; i++) buf[i] = s->hist[i];
    cursor = bta_ring_read_bytes(ring, cursor, buf + BTA_ASRC_HIST * 2, consume * BTA_FRAME_BYTES, copy_fn);

    unsigned nans = 0, clips = 0;
    unsigned total_in = BTA_ASRC_HIST + consume;
    for (unsigned i = BTA_ASRC_HIST * 2; i < total_in * 2; i++) {
        float f = buf[i];
        if (f != f) { f = 0.0f; nans++; }
        else if (f > 1.0f) { f = 1.0f; clips++; }
        else if (f < -1.0f) { f = -1.0f; clips++; }
        buf[i] = f;
    }

    int bypass = (s->phase == 0.0 && step > 0.99999 && step < 1.00001);
    float gain = s->cur_gain;
    float dgain = (target_gain - gain) * (1.0f / (float)BTA_PKT_FRAMES);
    unsigned peak = 0;
    int do_xfade = (s->xfade_rem > 0);

    for (unsigned k = 0; k < BTA_PKT_FRAMES; k++) {
        float fl, fr;
        if (bypass) {
            unsigned base = (1U + k) * 2U;
            fl = buf[base];
            fr = buf[base + 1U];
        } else {
            double pos = s->phase + (double)k * step;
            unsigned idx = (unsigned)pos;
            if (idx >= consume) idx = consume - 1U;
            float t = (float)(pos - (double)idx);
            unsigned i0 = idx * 2U;
            unsigned i1 = (idx + 1U) * 2U;
            unsigned i2 = (idx + 2U) * 2U;
            unsigned i3 = (idx + 3U) * 2U;
            fl = bta_hermite4(buf[i0], buf[i1], buf[i2], buf[i3], t);
            fr = bta_hermite4(buf[i0 + 1U], buf[i1 + 1U], buf[i2 + 1U], buf[i3 + 1U], t);
        }

        if (do_xfade) {
            float w = (float)k * (1.0f / (float)BTA_PKT_FRAMES);
            float iw = 1.0f - w;
            fl = s->xfade_tail[k * 2U] * iw + fl * w;
            fr = s->xfade_tail[k * 2U + 1U] * iw + fr * w;
        }

        gain += dgain;
        float scale = gain * 32767.0f;

        unsigned r_a = bta_rng_next(s);
        unsigned r_b = bta_rng_next(s);
        float d_l = (float)((int)(r_a & 0xffffU) + (int)(r_a >> 16) - 65536) * (1.0f / 65536.0f);
        float d_r = (float)((int)(r_b & 0xffffU) + (int)(r_b >> 16) - 65536) * (1.0f / 65536.0f);

        float sl = fl * scale + d_l;
        float sr = fr * scale + d_r;
        int il = (int)(sl >= 0.0f ? (sl + 0.5f) : (sl - 0.5f));
        int ir = (int)(sr >= 0.0f ? (sr + 0.5f) : (sr - 0.5f));
        if (il > 32767) il = 32767; else if (il < -32767) il = -32767;
        if (ir > 32767) ir = 32767; else if (ir < -32767) ir = -32767;

        out_pcm[k * 2U] = (short)il;
        out_pcm[k * 2U + 1U] = (short)ir;
        unsigned al = (unsigned)(il < 0 ? -il : il);
        unsigned ar = (unsigned)(ir < 0 ? -ir : ir);
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
    }

    s->cur_gain = target_gain;
    if (do_xfade) s->xfade_rem = 0;

    for (unsigned i = 0; i < BTA_ASRC_HIST * 2; i++) {
        s->hist[i] = buf[consume * 2U + i];
    }
    s->phase = bypass ? 0.0 : (end_pos - (double)consume);
    if (s->phase < 0.0) s->phase = 0.0;
    if (s->phase >= 1.0) s->phase -= (double)(int)s->phase;

    *peak_out = peak;
    *nan_out = nans;
    *clip_out = clips;
    return cursor;
}

#endif

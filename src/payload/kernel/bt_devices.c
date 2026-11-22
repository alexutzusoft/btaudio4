/* Firmware-independent locked snapshot of the driver's existing Classic device cache,
 * and the normal HCI packet-type request used by the proven BTA bridge.
 * Kernel symbols and bounds are discovered dynamically and cached per worker run. */
#include "../../common/protocol.h"
#include "kresolve.h"

__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long o;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:"
                     : "=a"(o), "+d"(c), "+D"(a), "+S"(b)
                     : "a"(n)
                     : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return o;
}

struct resolve_args {
    void *fn;
    struct bt_koff *out;
};

struct input {
    u64 address;
    int mode;
    int target_bp;
    const struct bt_koff *k;
    struct bt_devices *out;
    struct bt_prep_diag *diag;
    struct bt_stream_diag *sdiag;
    struct bt_full_diag *fdiag;
};
struct args {
    void *fn;
    struct input *input;
};

static struct bt_koff g_koff;

static void zero_mem(void *p, u64 n) {
    volatile u8 *b = (volatile u8 *)p;
    for (u64 i = 0; i < n; i++) b[i] = 0;
}

static int resolve_cb(void *td, struct resolve_args *a) {
    (void)td;
    u64 base = kr_find_base();
    if (!base) return 101;
    struct bt_koff res;
    int bad = kr_resolve_bt(base, &res);
    if (!bad && res.ctx_ptr && ((res.ctx_ptr >> 48) == 0xffff)) {
        res.ctx_val = *(const u64 *)res.ctx_ptr;
    }
    if (!res.copyout_fn) return 102;
    if (*(const u32 *)res.copyout_fn != 0xe5894855U) return 103;
    int (*copyout)(const void *, void *, u64) = (void *)res.copyout_fn;
    int cprc = copyout(&res, a->out, sizeof(res));
    return bad ? 102 : cprc;
}

static void copy_bytes(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
}

static int snap_conn_slots(const struct bt_koff *k, unsigned char *ctx, struct bt_conn_slot *slots) {
    int count = 0;
    for (unsigned off = k->conn_off; off < k->conn_end && count < BT_CONN_SNAP_LIMIT; off += k->conn_stride) {
        unsigned char *c = ctx + off;
        u64 bdaddr = *(u64 *)(c + k->conn_bdaddr_off);
        if (!bdaddr || (bdaddr >> 48)) continue;
        struct bt_conn_slot *s = &slots[count++];
        s->bdaddr = bdaddr;
        s->state = *(unsigned *)(c + k->conn_state_off);
        s->policy = *(unsigned *)(c + k->conn_policy_off);
        s->handle = *(unsigned short *)(c + k->conn_handle_off);
        s->min_bp = *(int *)(c + k->conn_min_bp_off);
        s->max_bp = *(int *)(c + k->conn_max_bp_off);
        unsigned char *d = *(void **)(c + k->conn_dev_off);
        if (d && ((u64)d >> 48) == 0xffff) {
            s->dev_cod = *(unsigned *)(d + 8);
            s->dev_services = *(unsigned *)(d + 0x20);
            s->dev_flags = *(unsigned *)(d + 0x24);
        } else {
            s->dev_services = 0xffffffffU;
        }
        unsigned char *st = k->conn_stream_off ? *(void **)(c + k->conn_stream_off) : 0;
        if (st && ((u64)st >> 48) == 0xffff) {
            s->stream_ok = 1;
            if (k->stream_dropped_off) s->st_dropped = *(unsigned *)(st + k->stream_dropped_off);
            if (k->stream_inflight_off) s->st_inflight = *(int *)(st + k->stream_inflight_off);
            if (k->stream_queued_off) s->st_queued = *(int *)(st + k->stream_queued_off);
            if (k->stream_frames_off) s->st_frames = *(int *)(st + k->stream_frames_off);
            if (k->stream_bitpool_off) s->st_bitpool = *(int *)(st + k->stream_bitpool_off);
            if (k->stream_enc_bp_off) s->st_enc_bp = *(int *)(st + k->stream_enc_bp_off);
        }
    }
    return count;
}

static int callback(void *td, struct args *a) {
    (void)td;
    struct input *in = a->input;
    const struct bt_koff *k = in ? in->k : 0;

    u64 base = kr_find_base();
    if (!base) return 22;
    if (!k || !k->valid || k->base != base) return 22;
    if (*(const u32 *)k->cache_fn != 0xe5894855U || *(const u32 *)k->cmd_fn != 0xe5894855U ||
        *(const u32 *)k->copyout_fn != 0xe5894855U) {
        return 22;
    }

    /* Input is the calling worker's private stack, as in existing syscall-11 helpers. */
    u64 address = in->address;
    int mode = in->mode;
    struct bt_devices *destination = in->out;
    if (mode < 0 || mode > 4) return 22;
    if ((mode == 1 || mode == 2 || mode == 4) && (!address || address >> 48)) return 22;

    if (mode == 4) {
        struct bt_stream_diag s_out;
        zero_mem(&s_out, sizeof(s_out));
        int prc = 2;
        if (!k->conn_stream_off || !k->stream_dropped_off || !k->stream_bitpool_off) {
            prc = 22;
        } else {
            unsigned char *ctx = __atomic_load_n((void *const volatile *)k->ctx_ptr, __ATOMIC_RELAXED);
            if (!ctx || ((u64)ctx >> 48) != 0xffff) {
                prc = 6;
            } else {
                for (unsigned off = k->conn_off; off < k->conn_end; off += k->conn_stride) {
                    unsigned char *c = ctx + off;
                    u64 bd = __atomic_load_n((const volatile u64 *)(c + k->conn_bdaddr_off), __ATOMIC_RELAXED);
                    if (bd != address) continue;
                    unsigned char *d = __atomic_load_n((void *const volatile *)(c + k->conn_dev_off), __ATOMIC_RELAXED);
                    if (!d || ((u64)d >> 48) != 0xffff ||
                        !(__atomic_load_n((const volatile unsigned *)(d + 0x20), __ATOMIC_RELAXED) & 8)) {
                        prc = 93;
                        break;
                    }
                    unsigned char *st = __atomic_load_n((void *const volatile *)(c + k->conn_stream_off), __ATOMIC_RELAXED);
                    if (!st || ((u64)st >> 48) != 0xffff) {
                        prc = 6;
                        break;
                    }
                    if (k->stream_pool_off && k->stream_pool_end > k->stream_pool_off) {
                        u64 slo = (u64)ctx + k->stream_pool_off;
                        u64 shi = (u64)ctx + k->stream_pool_end;
                        if ((u64)st < slo || (u64)st >= shi) {
                            prc = 6;
                            break;
                        }
                    }
                    if (__atomic_load_n((void *const volatile *)st, __ATOMIC_RELAXED) != c) {
                        prc = 6;
                        break;
                    }
                    s_out.dropped = __atomic_load_n((const volatile unsigned *)(st + k->stream_dropped_off), __ATOMIC_RELAXED);
                    if (k->stream_ring_drops_off)
                        s_out.ring_drops = __atomic_load_n((const volatile unsigned *)(st + k->stream_ring_drops_off), __ATOMIC_RELAXED);
                    s_out.inflight = __atomic_load_n((const volatile int *)(st + k->stream_inflight_off), __ATOMIC_RELAXED);
                    s_out.queued = __atomic_load_n((const volatile int *)(st + k->stream_queued_off), __ATOMIC_RELAXED);
                    if (k->stream_ring_used_off)
                        s_out.ring_used = __atomic_load_n((const volatile unsigned *)(st + k->stream_ring_used_off), __ATOMIC_RELAXED);
                    if (k->stream_ring_cap_off)
                        s_out.ring_cap = __atomic_load_n((const volatile unsigned *)(st + k->stream_ring_cap_off), __ATOMIC_RELAXED);
                    s_out.frames = __atomic_load_n((const volatile int *)(st + k->stream_frames_off), __ATOMIC_RELAXED);
                    if (k->stream_rate_idx_off)
                        s_out.rate_idx = __atomic_load_n((const volatile int *)(st + k->stream_rate_idx_off), __ATOMIC_RELAXED);
                    s_out.min_bp = __atomic_load_n((const volatile int *)(c + k->conn_min_bp_off), __ATOMIC_RELAXED);
                    s_out.max_bp = __atomic_load_n((const volatile int *)(c + k->conn_max_bp_off), __ATOMIC_RELAXED);
                    if (k->conn_neg_rate_off)
                        s_out.neg_rate = __atomic_load_n((const volatile int *)(c + k->conn_neg_rate_off), __ATOMIC_RELAXED);
                    if (k->conn_neg_ch_off)
                        s_out.neg_ch = __atomic_load_n((const volatile int *)(c + k->conn_neg_ch_off), __ATOMIC_RELAXED);
                    s_out.bitpool = __atomic_load_n((const volatile int *)(st + k->stream_bitpool_off), __ATOMIC_RELAXED);
                    if (k->stream_enc_bp_off)
                        s_out.enc_bp = __atomic_load_n((const volatile int *)(st + k->stream_enc_bp_off), __ATOMIC_RELAXED);
                    prc = 0;
                    break;
                }
            }
        }
        s_out.rc = prc;
        if (in->sdiag) {
            int (*copyout)(const void *, void *, u64) = (void *)k->copyout_fn;
            int cprc = copyout(&s_out, in->sdiag, sizeof(s_out));
            if (cprc && !prc) prc = cprc;
        }
        return prc;
    }

    struct bt_devices out;
    zero_mem(&out, sizeof(out));

    int (*lock)(void) = (void *)k->lock_fn;
    int (*unlock)(int) = (void *)k->unlock_fn;
    int rc = lock();
    if (rc) return rc;

    unsigned char *ctx = *(void **)k->ctx_ptr;
    if (!ctx || ((u64)ctx >> 48) != 0xffff) {
        unlock(0);
        return 6;
    }

    if (mode == 0) {
        for (unsigned off = k->cache_off; off < k->cache_end; off += k->cache_stride) {
            unsigned char *d = ctx + off;
            u64 mac = *(u64 *)d;
            if (!mac || mac >> 48) continue;
            if (out.count < BT_DEVICE_LIMIT) {
                struct bt_device *v = &out.items[out.count++];
                v->address = mac;
                v->cod = *(unsigned *)(d + 8);
                v->services = *(unsigned *)(d + 0x20);
                v->flags = *(unsigned *)(d + 0x24);
                for (unsigned i = 0; i < sizeof(v->name) - 1; i++) {
                    unsigned char c = d[0x80 + i];
                    if (!c) break;
                    v->name[i] = (c >= 32 && c < 127) ? c : '?';
                }
            }
        }
    } else if (mode == 1) {
        struct bt_prep_diag d_out;
        zero_mem(&d_out, sizeof(d_out));
        rc = 2;
        int (*command)(void *, unsigned, const char *, ...) = (void *)k->cmd_fn;
        for (unsigned off = k->conn_off; off < k->conn_end; off += k->conn_stride) {
            unsigned char *c = ctx + off;
            if (*(u64 *)(c + k->conn_bdaddr_off) != address) continue;
            unsigned char *d = *(void **)(c + k->conn_dev_off);
            d_out.conn_state = *(unsigned *)(c + k->conn_state_off);
            d_out.conn_policy = *(unsigned *)(c + k->conn_policy_off);
            d_out.handle = *(unsigned short *)(c + k->conn_handle_off);
            if (!d || ((u64)d >> 48) != 0xffff) {
                rc = 93;
                break;
            }
            d_out.dev_services = *(unsigned *)(d + 0x20);
            if (!(d_out.dev_services & 8)) {
                rc = 93;
                break;
            }
            if (d_out.conn_state >= 3) {
                d_out.skipped = 1;
                rc = 0;
            } else {
                rc = command(ctx, 0x40f, (const char *)k->fmt_str, (unsigned)d_out.handle, 0x3c18);
                if (rc >= 0) {
                    *(unsigned *)(c + k->conn_policy_off) = 0x3c18;
                    d_out.conn_policy = 0x3c18;
                    rc = 0;
                }
            }
            /* Best-effort single-piconet Role Switch (PS4 = Master) with Sniff/Hold/Park
             * disabled (0x0001) so A2DP audio never stalls in Sniff intervals. */
            if (rc == 0 && d_out.handle) {
                d_out.lps_rc = command(ctx, 0x80d, (const char *)k->fmt_str, (unsigned)d_out.handle, 0x0001);
                if (k->fmt_role_str) {
                    d_out.role_rc = command(ctx, 0x80b, (const char *)k->fmt_role_str,
                                            *(u64 *)(c + k->conn_bdaddr_off), 0);
                } else {
                    d_out.role_rc = -1;
                }
            }
            break;
        }
        d_out.rc = rc;
        if (in->diag) {
            d_out.slot_count = snap_conn_slots(k, ctx, d_out.slots);
        }
        unlock(0);
        if (in->diag) {
            int (*copyout)(const void *, void *, u64) = (void *)k->copyout_fn;
            int cprc = copyout(&d_out, in->diag, sizeof(d_out));
            if (cprc && !rc) rc = cprc;
        }
        return rc;
    } else if (mode == 2) {
        struct bt_stream_diag s_out;
        zero_mem(&s_out, sizeof(s_out));
        rc = 2;
        if (!k->conn_stream_off || !k->stream_dropped_off || !k->stream_bitpool_off) {
            rc = 22;
        } else {
            for (unsigned off = k->conn_off; off < k->conn_end; off += k->conn_stride) {
                unsigned char *c = ctx + off;
                if (*(u64 *)(c + k->conn_bdaddr_off) != address) continue;
                unsigned char *d = *(void **)(c + k->conn_dev_off);
                if (!d || ((u64)d >> 48) != 0xffff || !(*(unsigned *)(d + 0x20) & 8)) {
                    rc = 93;
                    break;
                }
                unsigned char *st = *(void **)(c + k->conn_stream_off);
                if (!st || ((u64)st >> 48) != 0xffff) {
                    rc = 6;
                    break;
                }
                s_out.dropped = *(unsigned *)(st + k->stream_dropped_off);
                if (k->stream_ring_drops_off) s_out.ring_drops = *(unsigned *)(st + k->stream_ring_drops_off);
                s_out.inflight = *(int *)(st + k->stream_inflight_off);
                s_out.queued = *(int *)(st + k->stream_queued_off);
                if (k->stream_ring_used_off) s_out.ring_used = *(unsigned *)(st + k->stream_ring_used_off);
                if (k->stream_ring_cap_off) s_out.ring_cap = *(unsigned *)(st + k->stream_ring_cap_off);
                s_out.frames = *(int *)(st + k->stream_frames_off);
                if (k->stream_rate_idx_off) s_out.rate_idx = *(int *)(st + k->stream_rate_idx_off);
                s_out.min_bp = *(int *)(c + k->conn_min_bp_off);
                s_out.max_bp = *(int *)(c + k->conn_max_bp_off);
                if (k->conn_neg_rate_off) s_out.neg_rate = *(int *)(c + k->conn_neg_rate_off);
                if (k->conn_neg_ch_off) s_out.neg_ch = *(int *)(c + k->conn_neg_ch_off);
                int cur_bp = *(int *)(st + k->stream_bitpool_off);
                int enc_bp = k->stream_enc_bp_off ? *(int *)(st + k->stream_enc_bp_off) : 0;
                if (in->target_bp > 0) {
                    int lo = (s_out.min_bp >= 2 && s_out.min_bp <= 51) ? s_out.min_bp : 2;
                    if (lo < 18) lo = 18;
                    int hi = (s_out.max_bp >= lo && s_out.max_bp <= 51) ? s_out.max_bp : 51;
                    int bp = in->target_bp;
                    if (bp < lo) bp = lo;
                    if (bp > hi) bp = hi;
                    *(int *)(st + k->stream_bitpool_off) = bp;
                    cur_bp = bp;
                    if (k->stream_enc_bp_off && ((k->resolved_mask & KR_RES_ENC_BP) || enc_bp > 0)) {
                        *(int *)(st + k->stream_enc_bp_off) = bp;
                        enc_bp = bp;
                    }
                }
                s_out.bitpool = cur_bp;
                s_out.enc_bp = enc_bp;
                rc = 0;
                break;
            }
        }
        s_out.rc = rc;
        if (rc != 0 && in->sdiag) {
            s_out.slot_count = snap_conn_slots(k, ctx, s_out.slots);
        }
        unlock(0);
        if (in->sdiag) {
            int (*copyout)(const void *, void *, u64) = (void *)k->copyout_fn;
            int cprc = copyout(&s_out, in->sdiag, sizeof(s_out));
            if (cprc && !rc) rc = cprc;
        }
        return rc;
    } else {
        struct bt_full_diag f_out;
        zero_mem(&f_out, sizeof(f_out));
        f_out.slot_count = snap_conn_slots(k, ctx, f_out.slots);
        rc = 0;
        if (address && !(address >> 48)) {
            for (unsigned off = k->conn_off; off < k->conn_end; off += k->conn_stride) {
                unsigned char *c = ctx + off;
                if (*(u64 *)(c + k->conn_bdaddr_off) != address) continue;
                f_out.target_found = 1;
                f_out.conn_ptr = (u64)c;
                copy_bytes(f_out.conn_hdr, c, sizeof(f_out.conn_hdr));
                u32 tail_off = (k->conn_state_off >= 32) ? (k->conn_state_off - 32) : 0;
                if (tail_off + sizeof(f_out.conn_tail) <= k->conn_stride) {
                    copy_bytes(f_out.conn_tail, c + tail_off, sizeof(f_out.conn_tail));
                }
                unsigned char *d = *(void **)(c + k->conn_dev_off);
                if (d && ((u64)d >> 48) == 0xffff) {
                    f_out.dev_ptr = (u64)d;
                    copy_bytes(f_out.dev_raw, d, sizeof(f_out.dev_raw));
                }
                unsigned char *st = k->conn_stream_off ? *(void **)(c + k->conn_stream_off) : 0;
                if (st && ((u64)st >> 48) == 0xffff) {
                    f_out.stream_ptr = (u64)st;
                    copy_bytes(f_out.stream_cfg, st, sizeof(f_out.stream_cfg));
                }
                break;
            }
        }
        f_out.rc = rc;
        unlock(0);
        if (in->fdiag) {
            int (*copyout)(const void *, void *, u64) = (void *)k->copyout_fn;
            int cprc = copyout(&f_out, in->fdiag, sizeof(f_out));
            if (cprc) rc = cprc;
        }
        return rc;
    }

    unlock(0);
    if (mode == 0) {
        int (*copyout)(const void *, void *, u64) = (void *)k->copyout_fn;
        return copyout(&out, destination, sizeof(out));
    }
    return rc;
}

static int invoke(struct input *in) {
    if (!g_koff.valid) {
        long r = sc3(11, (long)resolve_cb, (long)&g_koff, 0);
        if (r || !g_koff.valid) return r ? (int)r : 22;
    }
    in->k = &g_koff;
    return (int)sc3(11, (long)callback, (long)in, 0);
}

int bt_list(struct bt_devices *out) {
    struct input in = {.mode = 0, .out = out};
    return invoke(&in);
}

int bt_prepare_diag(u64 address, struct bt_prep_diag *diag) {
    struct input in = {.address = address, .mode = 1, .diag = diag};
    return invoke(&in);
}

int bt_prepare(u64 address) {
    return bt_prepare_diag(address, 0);
}

int bt_stream_tune(u64 address, int target_bp, struct bt_stream_diag *diag) {
    struct input in = {.address = address, .mode = 2, .target_bp = target_bp, .sdiag = diag};
    return invoke(&in);
}

int bt_stream_peek(u64 address, struct bt_stream_diag *diag) {
    struct input in = {.address = address, .mode = 4, .sdiag = diag};
    return invoke(&in);
}

int bt_full_diag(u64 address, struct bt_full_diag *diag) {
    struct input in = {.address = address, .mode = 3, .fdiag = diag};
    return invoke(&in);
}

const struct bt_koff *bt_koff_info(void) {
    return &g_koff;
}

#ifndef BTA_KRESOLVE_H
#define BTA_KRESOLVE_H

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;
typedef short s16;
typedef signed char s8;
typedef long s64;

struct bt_koff {
    u64 valid;
    u64 base;
    u64 copyout_fn;
    u64 lock_fn;
    u64 unlock_fn;
    u64 cmd_fn;
    u64 cache_fn;
    u64 ctx_ptr;
    u64 fmt_str;
    u32 cache_off;
    u32 cache_end;
    u32 cache_stride;
    u32 conn_off;
    u32 conn_end;
    u32 conn_stride;
    u32 conn_dev_off;
    u32 conn_state_off;
    u32 conn_policy_off;
    u32 conn_handle_off;
    u32 conn_bdaddr_off;
    u32 conn_stream_off;
    u32 conn_min_bp_off;
    u32 conn_max_bp_off;
    u32 stream_queued_off;
    u32 stream_inflight_off;
    u32 stream_dropped_off;
    u32 stream_frames_off;
    u32 stream_bitpool_off;
    u32 stream_enc_bp_off;
    u32 stream_ring_used_off;
    u32 stream_ring_cap_off;
    u32 stream_ring_drops_off;
    u32 stream_rate_idx_off;
    u32 conn_neg_rate_off;
    u32 conn_neg_ch_off;
    u32 stream_pool_off;
    u32 stream_pool_end;
    u32 stream_pool_stride;
    u64 fmt_role_str;
    u64 accept_site;
    u64 fail_stage;
    u32 fail_sub;
    u32 fail_aux;
    u32 resolved_mask;
    u32 ro_count;
    u64 rx_start;
    u64 rx_len;
    u64 ctx_val;
    u64 dump_addr;
    u32 dump_len;
    u32 dump_sub;
    u8 dump_bytes[256];
};

#define KR_RES_CACHE_BOUNDS (1U << 0)
#define KR_RES_CONN_LAYOUT  (1U << 1)
#define KR_RES_ROLE_FMT     (1U << 2)
#define KR_RES_BDADDR_OFF   (1U << 3)
#define KR_RES_AUDIO_OFF    (1U << 4)
#define KR_RES_BITPOOL_OFF  (1U << 5)
#define KR_RES_ENC_BP       (1U << 6)
#define KR_RES_STREAM_POOL  (1U << 7)

u64 kr_find_base(void);
int kr_rx_bounds(u64 base, u64 *start, u64 *len);
u64 kr_resolve_copyout(u64 rx_start, u64 rx_len);
u64 kr_resolve_accept_site(u64 rx_start, u64 rx_len);
u64 kr_resolve_rate_site(u64 rx_start, u64 rx_len, u64 accept_site);
u64 kr_find_bt_err_site(u64 rx_start, u64 rx_len);
int kr_resolve_bt(u64 base, struct bt_koff *out);

#endif

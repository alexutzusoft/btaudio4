/* Firmware-independent Bluetooth service-acceptance patch, using dynamic kernel
 * signature discovery, byte fingerprint check, atomic replacement, readback, and
 * a separately buildable rollback. Changes no flash or firmware files. */
#include "kresolve.h"

__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long out;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:"
                     : "=a"(out), "+d"(c), "+D"(a), "+S"(b)
                     : "a"(n)
                     : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return out;
}

struct result {
    u64 before, after;
    u64 base, rx_start, rx_len, copyout_fn, site_addr;
    u32 fail_step, pad;
    u8 surround[64];
    u64 rate_site_addr, rate_before, rate_after;
    u32 rate_rc, rate_step;
};
struct patch_args { struct result *out; };
struct args { void *fn; struct patch_args *pa; };

static int apply_atomic_u64(volatile u64 *site, u64 original, u64 replacement, u64 *before_out, u64 *after_out) {
    u64 cur = *site;
    *before_out = cur;
    *after_out = cur;
    if (cur == replacement) return 0;
    if (cur != original) return 5;
    u64 flags, cr0, expected = original;
    __asm__ volatile("pushfq; popq %0; cli; mov %%cr0,%1" : "=r"(flags), "=r"(cr0) :: "memory");
    u64 writable = cr0 & ~0x10000UL;
    __asm__ volatile("mov %0,%%cr0" :: "r"(writable) : "memory");
    __asm__ volatile("lock cmpxchgq %2,(%1)" : "+a"(expected) : "r"(site), "r"(replacement) : "cc", "memory");
    __asm__ volatile("mov %0,%%cr0; pushq %1; popfq" :: "r"(cr0), "r"(flags) : "cc", "memory");
    *after_out = *site;
    if (expected != original || *after_out != replacement) return 6;
    return 0;
}

static int patch(void *td, struct args *a) {
    (void)td;
    struct result out;
    u8 *op = (u8 *)&out;
    for (u64 i = 0; i < sizeof(out); i++) op[i] = 0;

    out.base = kr_find_base();
    if (!out.base) return 22;
    if (kr_rx_bounds(out.base, &out.rx_start, &out.rx_len)) return 22;
    out.copyout_fn = kr_resolve_copyout(out.rx_start, out.rx_len);
    if (!out.copyout_fn) return 22;

    out.site_addr = kr_resolve_accept_site(out.rx_start, out.rx_len);
    int rc = 0;
    if (!out.site_addr) {
        out.fail_step = 4;
        rc = 22;
        u64 err_site = kr_find_bt_err_site(out.rx_start, out.rx_len);
        if (err_site) {
            u64 da = (err_site > out.rx_start + 48) ? (err_site - 48) : out.rx_start;
            u64 avail = (out.rx_start + out.rx_len) - da;
            u64 n = avail < sizeof(out.surround) ? avail : sizeof(out.surround);
            const u8 *sb = (const u8 *)da;
            for (u64 i = 0; i < n; i++) out.surround[i] = sb[i];
        }
    } else {
        volatile u64 *site = (volatile u64 *)out.site_addr;
        u64 cur = *site;
        out.before = cur;
        out.after = cur;
        u64 da = (out.site_addr > out.rx_start + 24) ? (out.site_addr - 24) : out.rx_start;
        u64 avail = (out.rx_start + out.rx_len) - da;
        u64 n = avail < sizeof(out.surround) ? avail : sizeof(out.surround);
        const u8 *sb = (const u8 *)da;
        for (u64 i = 0; i < n; i++) out.surround[i] = sb[i];
        /* cmp dword [rax+0x20],0x40; jne reject; first 2 bytes of next insn.
         * -> test byte [rax+0x20],0x48; je reject. 0x40 = HID, 0x08 = audio.
         * Preserve byte 5 (<rel8>) and bytes [6..7] (49 8b) across firmware variants. */
        u64 tail = cur & 0xffff000000000000UL;
        u64 rel8 = cur & 0x0000ff0000000000UL;
        u64 original = tail | rel8 | 0x7540207883UL;
        u64 replacement = tail | rel8 | 0x74482040f6UL;
#ifdef ROLLBACK
        u64 swap = original; original = replacement; replacement = swap;
#endif
        int step = apply_atomic_u64(site, original, replacement, &out.before, &out.after);
        if (step) {
            out.fail_step = (u32)step;
            rc = (step == 6) ? 5 : 22;
        }
    }

    if (rc == 0) {
        out.rate_site_addr = kr_resolve_rate_site(out.rx_start, out.rx_len, out.site_addr);
        if (!out.rate_site_addr) {
            out.rate_step = 4;
            out.rate_rc = 22;
        } else {
            /* Keep 48 kHz / frm=3 even when DS4 speaker activates (jle -> jmp). */
            volatile u64 *rsite = (volatile u64 *)out.rate_site_addr;
            u64 rcur = *rsite;
            u64 rtail = rcur & ~0xffUL;
            u64 rorig = rtail | 0x7eUL;
            u64 rrep = rtail | 0xebUL;
#ifdef ROLLBACK
            u64 rswap = rorig; rorig = rrep; rrep = rswap;
#endif
            int rstep = apply_atomic_u64(rsite, rorig, rrep, &out.rate_before, &out.rate_after);
            if (rstep) {
                out.rate_step = (u32)rstep;
                out.rate_rc = (rstep == 6) ? 5 : 22;
            }
        }
    }

    int (*copyout)(const void *, void *, u64) = (void *)out.copyout_fn;
    int copied = copyout(&out, a->pa->out, sizeof(out));
    return copied ? copied : rc;
}

__attribute__((section(".text.start"))) int _start(void) {
    struct { long status; struct result result; } r = {0};
    struct patch_args pa = {.out = &r.result};
    r.status = sc3(11, (long)patch, (long)&pa, 0);
    long fd = sc3(5, (long)"/data/bt_accept_patch_result.bin", 0x601, 0644);
    if (fd >= 0) {
        sc3(4, fd, (long)&r, sizeof(r));
        sc3(6, fd, 0, 0);
    }
    return (int)r.status;
}

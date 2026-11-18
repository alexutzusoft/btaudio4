#include "diag.h"
#include "../version.h"

#define DIAG_INTERVAL_US 3000000ULL
#define DIAG_RELOAD_US   30000000ULL
#define DIAG_DNS_TTL_US  300000000ULL

struct diag_url {
 char host[128];
 char path[128];
 char host_hdr[140];
 u16 port;
};

struct diag_writer {
 char *b;
 u32 used;
 u32 cap;
};

static inline u16 diag_htons(u16 v) {
 return (u16)((v >> 8) | (v << 8));
}

static inline u32 diag_htonl(u32 v) {
 return ((v & 0xffU) << 24) | ((v & 0xff00U) << 8) |
        ((v & 0xff0000U) >> 8) | ((v >> 24) & 0xffU);
}

static inline u64 diag_rdtsc(void) {
 u32 lo, hi;
 __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
 return ((u64)hi << 32) | lo;
}

__attribute__((noinline)) static void dw_raw(struct diag_writer *w, const char *s) {
 while (*s && w->used + 1 < w->cap) w->b[w->used++] = *s++;
 w->b[w->used] = 0;
}

__attribute__((noinline)) static void dw_u64(struct diag_writer *w, u64 v) {
 char b[24];
 u32 i = 0;
 do {
  b[i++] = (char)('0' + (v % 10));
  v /= 10;
 } while (v);
 while (i && w->used + 1 < w->cap) w->b[w->used++] = b[--i];
 w->b[w->used] = 0;
}

__attribute__((noinline)) static void dw_url_enc(struct diag_writer *w, const char *s) {
 static const char hex[] = "0123456789ABCDEF";
 while (*s) {
  u8 c = (u8)*s++;
  int safe = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~');
  if (safe) {
   if (w->used + 1 < w->cap) w->b[w->used++] = (char)c;
  } else {
   if (w->used + 3 < w->cap) {
    w->b[w->used++] = '%';
    w->b[w->used++] = hex[c >> 4];
    w->b[w->used++] = hex[c & 15];
   }
  }
 }
 w->b[w->used] = 0;
}

static void fmt_hex64(u64 v, char out[17]) {
 static const char hex[] = "0123456789abcdef";
 for (int i = 0; i < 16; i++) {
  out[i] = hex[(v >> (60 - 4 * i)) & 15];
 }
 out[16] = 0;
}

static u64 entropy64(struct diag_reporter *d) {
 u64 x = diag_rdtsc() ^ (d->clock ? d->clock() : 0) ^ ((u64)(unsigned long)d * 0x9e3779b97f4a7c15ULL);
 u32 (*arc4)(void) = 0;
 if (!resolve("arc4random", &arc4) && arc4) {
  x ^= ((u64)arc4() << 32) | (u64)arc4();
 }
 x ^= x >> 33;
 x *= 0xff51afd7ed558ccdULL;
 x ^= x >> 33;
 x *= 0xc4ceb9fe1a85ec53ULL;
 x ^= x >> 33;
 return x ? x : 0x4254413444494147ULL;
}

static int is_hex16(const char *s) {
 for (int i = 0; i < 16; i++) {
  char c = s[i];
  if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
 }
 return 1;
}

static void load_or_create_client_id(struct diag_reporter *d) {
 char buf[32];
 memset(buf, 0, sizeof(buf));
 long fd = sc3(5, (long)DIAG_CLIENT_ID_PATH, 0, 0);
 if (fd >= 0) {
  long n = sc3(3, fd, (long)buf, sizeof(buf) - 1);
  closefd(fd);
  if (n >= 16 && is_hex16(buf)) {
   for (int i = 0; i < 16; i++) {
    char c = buf[i];
    if (c >= 'A' && c <= 'F') c = (char)(c + 32);
    d->client_id[i] = c;
   }
   d->client_id[16] = 0;
   return;
  }
 }
 fmt_hex64(entropy64(d), d->client_id);
 char tmp_path[] = DIAG_CLIENT_ID_PATH ".tmp";
 if (!put(tmp_path, d->client_id, 16)) {
  sc3(128, (long)tmp_path, (long)DIAG_CLIENT_ID_PATH, 0);
 }
}

static int starts_ci(const char *s, const char *prefix) {
 while (*prefix) {
  char a = *s++;
  char b = *prefix++;
  if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
  if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
  if (a != b) return 0;
 }
 return 1;
}

static void set_default_url(struct diag_url *u) {
 memset(u, 0, sizeof(*u));
 const char *h = DIAG_DEFAULT_HOST;
 u32 i = 0;
 while (h[i] && i + 1 < sizeof(u->host)) { u->host[i] = h[i]; i++; }
 u->host[i] = 0;
 u->port = DIAG_DEFAULT_PORT;
 const char *p = "/api/logs";
 u32 j = 0;
 while (p[j] && j + 1 < sizeof(u->path)) { u->path[j] = p[j]; j++; }
 u->path[j] = 0;
 struct diag_writer hw = {u->host_hdr, 0, sizeof(u->host_hdr)};
 dw_raw(&hw, u->host);
}

static int parse_url_text(const char *raw, struct diag_url *u) {
 const u8 *p = (const u8 *)raw;
 if (p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) p += 3;
 while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
 if (!*p) return -1;

 const char *s = (const char *)p;
 if (starts_ci(s, "http://")) s += 7;
 else if (starts_ci(s, "https://")) s += 8;

 memset(u, 0, sizeof(*u));
 u32 hi = 0;
 while (*s && *s != ':' && *s != '/' && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
  if (hi + 1 >= sizeof(u->host)) return -1;
  u->host[hi++] = *s++;
 }
 u->host[hi] = 0;
 if (!hi) return -1;

 u->port = DIAG_DEFAULT_PORT;
 if (*s == ':') {
  s++;
  u32 port = 0, digits = 0;
  while (*s >= '0' && *s <= '9') {
   port = port * 10 + (u32)(*s++ - '0');
   if (++digits > 5 || port > 65535) return -1;
  }
  if (!digits || port == 0) return -1;
  u->port = (u16)port;
 }

 char base[96];
 u32 bi = 0;
 if (*s == '/') {
  while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && *s != '?' && *s != '#') {
   if (bi + 1 < sizeof(base)) base[bi++] = *s;
   s++;
  }
 }
 while (bi > 0 && base[bi - 1] == '/') bi--;
 base[bi] = 0;

 struct diag_writer pw = {u->path, 0, sizeof(u->path)};
 if (bi >= 9 && equal(base + bi - 9, "/api/logs")) {
  dw_raw(&pw, base);
 } else {
  dw_raw(&pw, base);
  dw_raw(&pw, "/api/logs");
 }

 struct diag_writer hw = {u->host_hdr, 0, sizeof(u->host_hdr)};
 dw_raw(&hw, u->host);
 if (u->port != 80) {
  dw_raw(&hw, ":");
  dw_u64(&hw, u->port);
 }
 return 0;
}

static void load_url_config(struct diag_url *u) {
 char buf[256];
 const char *paths[2] = {DIAG_URL_PATH_A, DIAG_URL_PATH_B};
 for (int i = 0; i < 2; i++) {
  long fd = sc3(5, (long)paths[i], 0, 0);
  if (fd < 0) continue;
  long n = sc3(3, fd, (long)buf, sizeof(buf) - 1);
  closefd(fd);
  if (n > 0) {
   buf[n] = 0;
   if (!parse_url_text(buf, u)) return;
  }
 }
 set_default_url(u);
}

static int urls_equal(const struct diag_url *a, const struct diag_url *b) {
 return a->port == b->port && equal(a->host, b->host) && equal(a->path, b->path);
}

static int parse_ipv4(const char *s, u32 *ip_out) {
 u8 b[4];
 for (int i = 0; i < 4; i++) {
  u32 val = 0, digits = 0;
  while (*s >= '0' && *s <= '9') {
   val = val * 10 + (u32)(*s++ - '0');
   if (++digits > 3 || val > 255) return -1;
  }
  if (!digits) return -1;
  b[i] = (u8)val;
  if (i < 3 && *s++ != '.') return -1;
 }
 if (*s != 0) return -1;
 *ip_out = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
 return 0;
}

static long get_scenet_handle(void) {
 static long cached_handle = 0;
 if (cached_handle > 0) return cached_handle;

 int (*load_mod)(const char *, u64, const void *, u32, void *, int *) = 0;
 if (!resolve("sceKernelLoadStartModule", &load_mod) && load_mod) {
  int h = load_mod("/system/common/lib/libSceNet.sprx", 0, 0, 0, 0, 0);
  if (h > 0) {
   cached_handle = h;
   return h;
  }
 }
 long h = 0;
 if (!sc5(594, (long)"/system/common/lib/libSceNet.sprx", 0, (long)&h, 0, 0) && h > 0) {
  cached_handle = h;
  return h;
 }
 return 0;
}

static int resolve_scenet(const char *host, u32 *ip_out) {
 long net_h = get_scenet_handle();
 if (net_h <= 0) return -1;

 int (*net_init)(void) = 0;
 int (*pool_create)(const char *, int, int) = 0;
 int (*pool_destroy)(int) = 0;
 int (*res_create)(const char *, int, int) = 0;
 int (*res_destroy)(int) = 0;
 int (*res_ntoa)(int, const char *, u32 *, int, int, int) = 0;

 if (sc3(591, net_h, (long)"sceNetInit", (long)&net_init) || !net_init ||
     sc3(591, net_h, (long)"sceNetPoolCreate", (long)&pool_create) || !pool_create ||
     sc3(591, net_h, (long)"sceNetPoolDestroy", (long)&pool_destroy) || !pool_destroy ||
     sc3(591, net_h, (long)"sceNetResolverCreate", (long)&res_create) || !res_create ||
     sc3(591, net_h, (long)"sceNetResolverDestroy", (long)&res_destroy) || !res_destroy ||
     sc3(591, net_h, (long)"sceNetResolverStartNtoa", (long)&res_ntoa) || !res_ntoa) {
  return -1;
 }

 net_init();
 int memid = pool_create("bta_diag", 4096, 0);
 if (memid < 0) return -1;

 int rid = res_create("bta_res", memid, 0);
 if (rid < 0) {
  pool_destroy(memid);
  return -1;
 }

 u32 addr = 0;
 int rc = res_ntoa(rid, host, &addr, 2500000, 1, 0);
 res_destroy(rid);
 pool_destroy(memid);
 if (rc < 0 || addr == 0) return -1;
 *ip_out = addr;
 return 0;
}

static int poll_fd(struct diag_reporter *d, long fd, short events, int timeout_ms) {
 u64 start = d->clock();
 u64 limit = (u64)timeout_ms * 1000ULL;
 for (;;) {
  if (!__atomic_load_n(&d->run, __ATOMIC_ACQUIRE) && d->clock() >= d->stop_deadline) return -1;
  u64 now = d->clock();
  u64 elapsed = now - start;
  if (elapsed >= limit) return 0;
  u64 slice_ms = (limit - elapsed) / 1000ULL;
  if (slice_ms == 0) slice_ms = 1;
  if (slice_ms > 200) slice_ms = 200;
  struct { int fd; short events, revents; } pfd = {(int)fd, events, 0};
  long rc = sc3(209, (long)&pfd, 1, (long)slice_ms);
  if (rc > 0) {
   if (pfd.revents & (events | 0x38)) return 1;
  } else if (rc < 0 && rc != -4) {
   return -1;
  }
 }
}

static u32 dns_skip_name(const u8 *pkt, u32 len, u32 pos) {
 u32 steps = 0;
 while (pos < len && steps < 64) {
  u8 c = pkt[pos];
  if (c == 0) return pos + 1;
  if ((c & 0xc0) == 0xc0) {
   if (pos + 1 >= len) return len + 1;
   return pos + 2;
  }
  pos += 1U + (u32)c;
  steps++;
 }
 return len + 1;
}

static int query_dns_server(struct diag_reporter *d, const char *host, u32 dns_ip_net, u32 *ip_out) {
 u8 pkt[512];
 memset(pkt, 0, sizeof(pkt));
 u16 txid = (u16)(diag_rdtsc() ^ d->clock());
 pkt[0] = (u8)(txid >> 8);
 pkt[1] = (u8)(txid & 0xff);
 pkt[2] = 0x01; /* RD = 1 */
 pkt[5] = 0x01; /* QDCOUNT = 1 */

 u32 pos = 12;
 const char *s = host;
 while (*s) {
  const char *dot = s;
  while (*dot && *dot != '.') dot++;
  u32 llen = (u32)(dot - s);
  if (llen == 0 || llen > 63 || pos + 1 + llen + 5 >= sizeof(pkt)) return -1;
  pkt[pos++] = (u8)llen;
  for (u32 i = 0; i < llen; i++) pkt[pos++] = (u8)s[i];
  s = *dot ? dot + 1 : dot;
 }
 pkt[pos++] = 0;
 pkt[pos++] = 0; pkt[pos++] = 1; /* QTYPE = A */
 pkt[pos++] = 0; pkt[pos++] = 1; /* QCLASS = IN */

 long fd = sc3(97, 2, 2, 0); /* AF_INET, SOCK_DGRAM */
 if (fd < 0) return -1;
 int one = 1;
 sc5(105, fd, 0xffff, 0x800, (long)&one, 4);
 if (nonblock(fd) < 0) { closefd(fd); return -1; }

 struct sockaddr4 sa = {.len = 16, .family = 2, .port = diag_htons(53), .addr = dns_ip_net};
 if (sc3(98, fd, (long)&sa, 16) < 0) { closefd(fd); return -1; }
 if (sc3(4, fd, (long)pkt, pos) != (long)pos) { closefd(fd); return -1; }
 if (poll_fd(d, fd, 1, 2000) <= 0) { closefd(fd); return -1; }

 u8 resp[512];
 long n = sc3(3, fd, (long)resp, sizeof(resp));
 closefd(fd);
 if (n < 12) return -1;
 if (resp[0] != pkt[0] || resp[1] != pkt[1]) return -1;
 if ((resp[2] & 0x80) == 0 || (resp[3] & 0x0f) != 0) return -1;

 u16 qd = (u16)((resp[4] << 8) | resp[5]);
 u16 an = (u16)((resp[6] << 8) | resp[7]);
 u32 rpos = 12;
 u32 rlen = (u32)n;
 for (u16 i = 0; i < qd; i++) {
  rpos = dns_skip_name(resp, rlen, rpos);
  rpos += 4;
  if (rpos > rlen) return -1;
 }
 for (u16 i = 0; i < an; i++) {
  rpos = dns_skip_name(resp, rlen, rpos);
  if (rpos + 10 > rlen) return -1;
  u16 rtype = (u16)((resp[rpos] << 8) | resp[rpos + 1]);
  u16 rclass = (u16)((resp[rpos + 2] << 8) | resp[rpos + 3]);
  u16 rdlen = (u16)((resp[rpos + 8] << 8) | resp[rpos + 9]);
  rpos += 10;
  if (rpos + rdlen > rlen) return -1;
  if (rtype == 1 && rclass == 1 && rdlen == 4) {
   *ip_out = (u32)resp[rpos] | ((u32)resp[rpos + 1] << 8) |
             ((u32)resp[rpos + 2] << 16) | ((u32)resp[rpos + 3] << 24);
   return 0;
  }
  rpos += rdlen;
 }
 return -1;
}

static int diag_stopping_expired(struct diag_reporter *d) {
 return !__atomic_load_n(&d->run, __ATOMIC_ACQUIRE) && d->clock() >= d->stop_deadline;
}

static int resolve_host(struct diag_reporter *d, const char *host, u32 *ip_out) {
 if (!parse_ipv4(host, ip_out)) return 0;
 if (!__atomic_load_n(&d->run, __ATOMIC_ACQUIRE)) return -1;
 if (!resolve_scenet(host, ip_out)) return 0;
 if (diag_stopping_expired(d)) return -1;
 if (!query_dns_server(d, host, diag_htonl(0x01010101U), ip_out)) return 0;
 if (diag_stopping_expired(d)) return -1;
 if (!query_dns_server(d, host, diag_htonl(0x08080808U), ip_out)) return 0;
 return -1;
}

static long tcp_connect_nb(struct diag_reporter *d, u32 ip, u16 port, int timeout_ms) {
 long fd = sc3(97, 2, 1, 0);
 if (fd < 0) return -1;
 int one = 1;
 sc5(105, fd, 0xffff, 0x800, (long)&one, 4); /* SO_NOSIGPIPE */
 if (nonblock(fd) < 0) { closefd(fd); return -1; }

 struct sockaddr4 sa = {.len = 16, .family = 2, .port = diag_htons(port), .addr = ip};
 long rc = sc3(98, fd, (long)&sa, 16);
 if (rc < 0) {
  if (rc != -36 && rc != -35) { closefd(fd); return -1; }
  if (poll_fd(d, fd, 4, timeout_ms) <= 0) { closefd(fd); return -1; }
  int err = 0;
  u32 elen = sizeof(err);
  if (sc5(118, fd, 0xffff, 0x1007, (long)&err, (long)&elen) < 0 || err != 0) {
   closefd(fd);
   return -1;
  }
 }
 return fd;
}

static int write_nb(struct diag_reporter *d, long fd, const u8 *buf, u32 len, int timeout_ms) {
 u32 off = 0;
 while (off < len) {
  long w = sc3(4, fd, (long)(buf + off), len - off);
  if (w > 0) {
   off += (u32)w;
   continue;
  }
  if (w == -35) {
   if (poll_fd(d, fd, 4, timeout_ms) <= 0) return -1;
   continue;
  }
  return -1;
 }
 return 0;
}

static int parse_content_length(const char *headers, u32 *cl_out, int *close_out) {
 *cl_out = 0;
 *close_out = 0;
 int has_cl = 0;
 const char *p = find((char *)headers, "\r\n");
 if (!p) return 0;
 p += 2;
 while (*p && *p != '\r') {
  if (starts_ci(p, "content-length:")) {
   const char *q = p + 15;
   while (*q == ' ' || *q == '\t') q++;
   u32 v = 0;
   while (*q >= '0' && *q <= '9') { v = v * 10 + (u32)(*q++ - '0'); }
   *cl_out = v;
   has_cl = 1;
  } else if (starts_ci(p, "connection:")) {
   const char *q = p + 11;
   while (*q == ' ' || *q == '\t') q++;
   if (starts_ci(q, "close")) *close_out = 1;
  }
  char *next = find((char *)p, "\r\n");
  if (!next) break;
  p = next + 2;
 }
 return has_cl;
}

static int read_http_response(struct diag_reporter *d, long fd, int timeout_ms, int *keep_alive) {
 char resp[1024];
 u32 used = 0;
 char *hdr_end = 0;

 while (used + 1 < sizeof(resp)) {
  if (poll_fd(d, fd, 1, timeout_ms) <= 0) return -1;
  long n = sc3(3, fd, (long)(resp + used), sizeof(resp) - used - 1);
  if (n > 0) {
   used += (u32)n;
   resp[used] = 0;
   hdr_end = find(resp, "\r\n\r\n");
   if (hdr_end) break;
   continue;
  }
  if (n == -35) continue;
  return -1;
 }
 if (!hdr_end || used < 12 || !starts(resp, "HTTP/1.")) return -1;

 int status = 0;
 const char *sp = resp;
 while (*sp && *sp != ' ') sp++;
 while (*sp == ' ') sp++;
 for (int i = 0; i < 3 && sp[i] >= '0' && sp[i] <= '9'; i++) {
  status = status * 10 + (sp[i] - '0');
 }
 if (status < 100 || status > 599) return -1;

 u32 hdr_bytes = (u32)(hdr_end + 4 - resp);
 u32 body_have = used - hdr_bytes;
 u32 cl = 0;
 int conn_close = 0;
 int has_cl = parse_content_length(resp, &cl, &conn_close);

 if (has_cl && !conn_close && cl <= 8192) {
  u32 rem = (cl > body_have) ? (cl - body_have) : 0;
  char drain[512];
  while (rem > 0) {
   if (poll_fd(d, fd, 1, timeout_ms) <= 0) { *keep_alive = 0; break; }
   u32 want = rem < sizeof(drain) ? rem : (u32)sizeof(drain);
   long n = sc3(3, fd, (long)drain, want);
   if (n > 0) rem -= (u32)n;
   else if (n == -35) continue;
   else { *keep_alive = 0; break; }
  }
  if (rem == 0) *keep_alive = 1;
 } else {
  *keep_alive = 0;
 }
 return status;
}

static int send_http_chunk(struct diag_reporter *d, const struct diag_url *u,
                           u32 ip, long *sock_io, const u8 *chunk, u32 len,
                           u32 seq, u64 offset, u64 dropped, int timeout_ms) {
 char hdr[1024];
 struct diag_writer w = {hdr, 0, sizeof(hdr)};
 dw_raw(&w, "POST ");
 dw_raw(&w, u->path);
 dw_raw(&w, "?client="); dw_url_enc(&w, d->client_id);
 dw_raw(&w, "&session="); dw_url_enc(&w, d->session_id);
 dw_raw(&w, "&seq="); dw_u64(&w, seq);
 dw_raw(&w, "&offset="); dw_u64(&w, offset);
 dw_raw(&w, "&version="); dw_url_enc(&w, BTA_VERSION);
 dw_raw(&w, "&fw="); dw_url_enc(&w, d->fw);
 if (dropped) { dw_raw(&w, "&dropped="); dw_u64(&w, dropped); }
 dw_raw(&w, " HTTP/1.1\r\nHost: ");
 dw_raw(&w, u->host_hdr);
 dw_raw(&w, "\r\nUser-Agent: BTAudio4/" BTA_VERSION "\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: ");
 dw_u64(&w, len);
 dw_raw(&w, "\r\nConnection: keep-alive\r\n\r\n");

 for (int attempt = 0; attempt < 2; attempt++) {
  int reused = (*sock_io >= 0);
  if (*sock_io < 0) {
   *sock_io = tcp_connect_nb(d, ip, u->port, timeout_ms);
   if (*sock_io < 0) return -1;
  }
  int ka = 0;
  if (write_nb(d, *sock_io, (const u8 *)hdr, w.used, timeout_ms) == 0 &&
      write_nb(d, *sock_io, chunk, len, timeout_ms) == 0) {
   int code = read_http_response(d, *sock_io, timeout_ms, &ka);
   if (!ka) { closefd(*sock_io); *sock_io = -1; }
   if (code >= 200 && code < 300) return 0;
   if (code == 400 || code == 413 || code == 414) return -3;
   if (code > 0) return -2; /* server rejected, do not immediately reconnect */
  }
  closefd(*sock_io);
  *sock_io = -1;
  if (!reused) break;
 }
 return -1;
}

static u32 copy_chunk_max(struct diag_reporter *d, u32 tail, u32 avail, u8 *out, u32 max_len) {
 u32 len = avail > max_len ? max_len : avail;
 u32 idx = tail & (d->cap - 1);
 u32 first = d->cap - idx;
 if (first >= len) {
  memcpy(out, d->ring + idx, len);
 } else {
  memcpy(out, d->ring + idx, first);
  memcpy(out + first, d->ring, len - first);
 }
 if (avail > max_len) {
  for (u32 k = len; k > len / 2; k--) {
   if (out[k - 1] == '\n') {
    len = k;
    break;
   }
  }
 }
 return len;
}

static void *diag_thread(void *arg) {
 struct diag_reporter *d = arg;
 struct diag_url url;
 load_url_config(&url);

 u64 next_cfg = d->clock() + DIAG_RELOAD_US;
 u64 next_send = d->clock() + DIAG_INTERVAL_US;
 u64 dns_at = 0;
 u32 cached_ip = 0;
 int has_ip = 0;
 u32 consec_fails = 0;
 long sock = -1;
 u32 backoff_ms = 0;
 u64 rep_drop_ev = 0;
 u64 rep_drop_bytes = 0;
 u8 tx_buf[DIAG_CHUNK_MAX];

 for (;;) {
  int running = __atomic_load_n(&d->run, __ATOMIC_ACQUIRE);
  u64 now = d->clock();
  if (!running && now >= d->stop_deadline) break;

  if (running && now >= next_cfg) {
   struct diag_url next_url;
   load_url_config(&next_url);
   if (!urls_equal(&url, &next_url)) {
    url = next_url;
    has_ip = 0;
    consec_fails = 0;
    closefd(sock);
    sock = -1;
   }
   next_cfg = now + DIAG_RELOAD_US;
  }

  u32 head = __atomic_load_n(&d->head, __ATOMIC_ACQUIRE);
  u32 tail = d->tail;
  u32 avail = head - tail;

  if (!running && avail == 0) break;

  if (avail > 0 && __atomic_exchange_n(&d->urgent, 0, __ATOMIC_ACQ_REL)) {
   u64 u = now + 150000ULL;
   if (u < next_send) next_send = u;
  }

  int due = !running || (avail >= DIAG_CHUNK_MAX) || (avail > 0 && now >= next_send);
  if (!due) {
   d->sleep(avail > 0 ? 50000 : 200000);
   continue;
  }

  if (!has_ip || (running && now - dns_at >= DIAG_DNS_TTL_US)) {
   u32 ip = 0;
   if (!resolve_host(d, url.host, &ip)) {
    cached_ip = ip;
    has_ip = 1;
    dns_at = d->clock();
    consec_fails = 0;
   } else if (!has_ip) {
    has_ip = 0;
   }
  }

  if (!has_ip) {
   __atomic_fetch_add(&d->failed_sends, 1, __ATOMIC_RELAXED);
   if (!running) break;
   backoff_ms = backoff_ms ? (backoff_ms * 2) : 2000;
   if (backoff_ms > 30000) backoff_ms = 30000;
   next_send = d->clock() + (u64)backoff_ms * 1000ULL;
   d->sleep(200000);
   continue;
  }

  u64 cur_drop_ev = __atomic_load_n(&d->dropped, __ATOMIC_RELAXED);
  u64 cur_drop_bytes = __atomic_load_n(&d->dropped_bytes, __ATOMIC_RELAXED);
  u32 prefix_len = 0;
  if (cur_drop_ev > rep_drop_ev) {
   struct diag_writer pw = {(char *)tx_buf, 0, 128};
   dw_raw(&pw, "# [diag: dropped=");
   dw_u64(&pw, cur_drop_ev - rep_drop_ev);
   dw_raw(&pw, " bytes=");
   dw_u64(&pw, cur_drop_bytes - rep_drop_bytes);
   dw_raw(&pw, " total=");
   dw_u64(&pw, cur_drop_ev);
   dw_raw(&pw, "]\n");
   prefix_len = pw.used;
  }

  u32 ring_len = copy_chunk_max(d, tail, avail, tx_buf + prefix_len, DIAG_CHUNK_MAX - prefix_len);
  u32 total_len = prefix_len + ring_len;
  int timeout_ms = running ? 4000 : 1200;
  int rc = send_http_chunk(d, &url, cached_ip, &sock, tx_buf, total_len,
                           d->next_seq, d->sent_bytes, cur_drop_ev, timeout_ms);
  if (rc == 0 || rc == -3) {
   __atomic_store_n(&d->tail, tail + ring_len, __ATOMIC_RELEASE);
   d->next_seq++;
   __atomic_store_n(&d->sent_bytes, d->sent_bytes + total_len, __ATOMIC_RELAXED);
   rep_drop_ev = cur_drop_ev;
   rep_drop_bytes = cur_drop_bytes;
   if (rc == 0) {
    __atomic_store_n(&d->sent_chunks, d->sent_chunks + 1, __ATOMIC_RELAXED);
    backoff_ms = 0;
    consec_fails = 0;
   } else {
    __atomic_fetch_add(&d->failed_sends, 1, __ATOMIC_RELAXED);
   }
   u32 cur_head = __atomic_load_n(&d->head, __ATOMIC_ACQUIRE);
   u32 rem = cur_head - (tail + ring_len);
   next_send = d->clock() + (rem > 0 ? 50000ULL : DIAG_INTERVAL_US);
   if (rem > 0 && running) d->sleep(50000);
  } else {
   __atomic_fetch_add(&d->failed_sends, 1, __ATOMIC_RELAXED);
   if (rc == -1 && ++consec_fails >= 3) has_ip = 0;
   closefd(sock);
   sock = -1;
   if (!running) break;
   backoff_ms = backoff_ms ? (backoff_ms * 2) : 2000;
   if (backoff_ms > 30000) backoff_ms = 30000;
   next_send = d->clock() + (u64)backoff_ms * 1000ULL;
   d->sleep(200000);
  }
 }

 closefd(sock);
 __atomic_store_n(&d->exited, 1, __ATOMIC_RELEASE);
 return 0;
}

void diag_init(struct diag_reporter *d,
               int (*sleep_fn)(unsigned),
               u64 (*clock_fn)(void),
               void *(*map_fn)(void *, u64, int, int, int, u64),
               int (*unmap_fn)(void *, u64),
               const char *fw_text) {
 memset(d, 0, sizeof(*d));
 d->sleep = sleep_fn;
 d->clock = clock_fn;
 d->map = map_fn;
 d->unmap = unmap_fn;

 if (fw_text) {
  u32 i = 0, j = 0;
  while (fw_text[i] && j + 1 < sizeof(d->fw)) {
   char c = fw_text[i++];
   if (c >= 32 && c < 127) d->fw[j++] = c;
  }
  d->fw[j] = 0;
 }
 if (!d->fw[0]) {
  memcpy(d->fw, "unknown", 8);
 }

 d->ring = d->map(0, DIAG_RING_SIZE, 3, 0x1002, -1, 0);
 if (!d->ring || d->ring == (void *)-1) {
  d->ring = 0;
  return;
 }
 d->cap = DIAG_RING_SIZE;
 d->next_seq = 1;

 load_or_create_client_id(d);
 fmt_hex64(entropy64(d), d->session_id);

 int (*create)(void **, void **, void *(*)(void *), void *, const char *) = 0;
 if (resolve("scePthreadCreate", &create) || resolve("scePthreadJoin", &d->join) ||
     !create || !d->join) {
  d->unmap(d->ring, d->cap);
  d->ring = 0;
  return;
 }

 d->run = 1;
 if (create(&d->thread, 0, diag_thread, d, "Bluetooth Audio diag")) {
  d->run = 0;
  d->unmap(d->ring, d->cap);
  d->ring = 0;
  return;
 }
 d->alive = 1;
}

void diag_push_ex(struct diag_reporter *d, const char *data, u32 len, int urgent) {
 if (!d || !d->ring || !len || len >= d->cap) return;
 u32 tail = __atomic_load_n(&d->tail, __ATOMIC_ACQUIRE);
 u32 head = d->head;
 u32 used = head - tail;
 u32 free_bytes = d->cap - used;
 if (free_bytes < len) {
  __atomic_fetch_add(&d->dropped, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&d->dropped_bytes, len, __ATOMIC_RELAXED);
  return;
 }
 u32 idx = head & (d->cap - 1);
 u32 first = d->cap - idx;
 if (first >= len) {
  memcpy(d->ring + idx, data, len);
 } else {
  memcpy(d->ring + idx, data, first);
  memcpy(d->ring, data + first, len - first);
 }
 __atomic_store_n(&d->head, head + len, __ATOMIC_RELEASE);
 if (urgent) __atomic_store_n(&d->urgent, 1, __ATOMIC_RELEASE);
}

void diag_push(struct diag_reporter *d, const char *data, u32 len) {
 diag_push_ex(d, data, len, 0);
}

void diag_stop(struct diag_reporter *d) {
 if (!d) return;
 if (d->alive) {
  d->stop_deadline = d->clock() + 2500000ULL;
  __atomic_store_n(&d->run, 0, __ATOMIC_RELEASE);
  d->join(d->thread, 0);
  d->alive = 0;
 }
 if (d->ring) {
  d->unmap(d->ring, d->cap);
  d->ring = 0;
 }
}

#ifndef BTA_ANNOUNCEMENT_H
#define BTA_ANNOUNCEMENT_H

#include "version_check.h"

#define BTA_ANNOUNCEMENT_PATH "/api/announcement"
#define BTA_ANNOUNCEMENT_TEST_PATH "/api/test/announcement"
#define BTA_ANNOUNCEMENT_INTERVAL_US 60000000ULL
#define BTA_ANNOUNCEMENT_LOCK_US 3000000ULL
#define BTA_ANNOUNCEMENT_ID_CAP 65U
#define BTA_ANNOUNCEMENT_TITLE_CAP 129U
#define BTA_ANNOUNCEMENT_BODY_CAP 2049U
#define BTA_ANNOUNCEMENT_JSON_CAP 4608U
#define BTA_ANNOUNCEMENT_MARKER_CAP (BTA_ANNOUNCEMENT_ID_CAP + 48U)
#define BTA_SERVER_URL_CAP 320U
#define BTA_SERVER_URL_DEFAULT "http://bta.alexutzu.pro"

struct bta_announcement {
    char id[BTA_ANNOUNCEMENT_ID_CAP];
    char title[BTA_ANNOUNCEMENT_TITLE_CAP];
    char body[BTA_ANNOUNCEMENT_BODY_CAP];
    int test;
};

static inline int bta_announcement_id_valid(const char *id, size_t len) {
    if (!len || len >= BTA_ANNOUNCEMENT_ID_CAP) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static inline void bta_announcement_marker(const struct bta_announcement *a, char *path) {
    const char *prefix = a->test ? "/data/bta_audio/announcement-test-seen-" :
                                  "/data/bta_audio/announcement-seen-";
    while (*prefix) *path++ = *prefix++;
    for (const char *id = a->id; (*path++ = *id++);) {}
}

/* Decode one UTF-8 code point, rejecting overlong encodings and surrogates. */
static inline size_t bta_announcement_utf8(const char *s, size_t len, unsigned *codepoint) {
    if (!len) return 0;
    unsigned first = (unsigned char)s[0];
    if (first < 128) { *codepoint = first; return 1; }
    size_t n;
    unsigned cp, min;
    if (first >= 0xc2 && first <= 0xdf) { n = 2; cp = first & 31; min = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { n = 3; cp = first & 15; min = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { n = 4; cp = first & 7; min = 0x10000; }
    else return 0;
    if (n > len) return 0;
    for (size_t i = 1; i < n; i++) {
        unsigned c = (unsigned char)s[i];
        if ((c & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (c & 63);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    *codepoint = cp;
    return n;
}

static inline int bta_announcement_hex(const char *p, unsigned *cp) {
    unsigned value = 0;
    for (size_t i = 0; i < 4; i++) {
        char c = p[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return -1;
        value = (value << 4) | digit;
    }
    *cp = value;
    return 0;
}

static inline int bta_announcement_read_string(const char **cursor, const char *end,
                                               char *out, size_t cap, int multiline) {
    const char *p = *cursor;
    if (p == end || *p++ != '"') return -1;
    size_t used = 0;
    while (p < end && *p != '"') {
        unsigned cp;
        if (*p == '\\') {
            if (++p == end) return -1;
            char esc = *p++;
            switch (esc) {
            case '"': case '\\': case '/': cp = (unsigned)esc; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u':
                if (end - p < 4 || bta_announcement_hex(p, &cp)) return -1;
                p += 4;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    unsigned low;
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u' ||
                        bta_announcement_hex(p + 2, &low) || low < 0xdc00 || low > 0xdfff) return -1;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    p += 6;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return -1;
                break;
            default: return -1;
            }
        } else {
            size_t n = bta_announcement_utf8(p, (size_t)(end - p), &cp);
            if (!n || cp < 32) return -1;
            p += n;
        }
        if (cp < 32 && !(multiline && (cp == '\n' || cp == '\r' || cp == '\t'))) return -1;
        size_t n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (used + n >= cap) return -1;
        for (size_t i = n - 1; i > 0; i--) {
            out[used + i] = (char)(0x80 | (cp & 63));
            cp >>= 6;
        }
        static const unsigned char prefix[] = {0, 0, 0xc0, 0xe0, 0xf0};
        out[used] = (char)(prefix[n] | cp);
        used += n;
    }
    if (p == end) return -1;
    out[used] = 0;
    *cursor = p + 1;
    return (int)used;
}

static inline const char *bta_announcement_skip_space(const char *p, const char *end) {
    while (p < end && bta_is_space(*p)) p++;
    return p;
}

static inline int bta_parse_announcement(const char *json, size_t len, struct bta_announcement *a) {
    const char *end = json + len, *p = bta_announcement_skip_space(json, end);
    unsigned seen = 0;
    a->test = 0;
    if (p == end || *p++ != '{') return -1;
    for (;;) {
        p = bta_announcement_skip_space(p, end);
        char key[6];
        int n = bta_announcement_read_string(&p, end, key, sizeof(key), 0);
        if (n < 0) return -1;
        p = bta_announcement_skip_space(p, end);
        if (p == end || *p++ != ':') return -1;
        p = bta_announcement_skip_space(p, end);

        unsigned field;
        char *out;
        size_t cap;
        if (n == 2 && bta_matches(key, "id", 2)) { field = 1; out = a->id; cap = sizeof(a->id); }
        else if (n == 5 && bta_matches(key, "title", 5)) { field = 2; out = a->title; cap = sizeof(a->title); }
        else if (n == 4 && bta_matches(key, "body", 4)) { field = 4; out = a->body; cap = sizeof(a->body); }
        else if (n == 4 && bta_matches(key, "test", 4)) { field = 8; out = 0; cap = 0; }
        else return -1;
        if (seen & field) return -1;
        seen |= field;
        if (out) {
            int count = bta_announcement_read_string(&p, end, out, cap, field == 4);
            if (count <= 0) return -1;
            if (field == 1 && !bta_announcement_id_valid(out, (size_t)count)) return -1;
            if (bta_announcement_skip_space(out, out + count) == out + count) return -1;
        } else {
            if (end - p < 4 || !bta_matches(p, "true", 4)) return -1;
            a->test = 1;
            p += 4;
        }
        p = bta_announcement_skip_space(p, end);
        if (p == end) return -1;
        if (*p == '}') return (seen & 7) == 7 && bta_announcement_skip_space(p + 1, end) == end ? 0 : -1;
        if (*p++ != ',') return -1;
    }
}

/* Both console processes use the same diagnostic server override and route prefix. */
static inline int bta_server_url(const char *raw, const char *route, char out[BTA_SERVER_URL_CAP]) {
    const char *p = raw;
    if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) p += 3;
    while (bta_is_space(*p)) p++;
    size_t len = 0;
    while (p[len] && !bta_is_space(p[len]) && p[len] != '?' && p[len] != '#' && len < 239) len++;
    while (len && p[len - 1] == '/') len--;
    if (len >= 9 && bta_matches(p + len - 9, "/api/logs", 9)) len -= 9;
    if (!len) return -1;
    char *dest = out;
    if (!(len >= 7 && bta_matches(p, "http://", 7)) && !(len >= 8 && bta_matches(p, "https://", 8))) {
        for (const char *scheme = "http://"; *scheme;) *dest++ = *scheme++;
    }
    for (size_t i = 0; i < len; i++) *dest++ = p[i];
    while (*route && dest < out + BTA_SERVER_URL_CAP - 1) *dest++ = *route++;
    *dest = 0;
    return *route ? -1 : 0;
}

#endif

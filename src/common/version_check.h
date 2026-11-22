#ifndef BTA_VERSION_CHECK_H
#define BTA_VERSION_CHECK_H

#include <stddef.h>

#define BTA_UPDATE_PATH "/api/update"
#define BTA_UPDATE_INTERVAL_US 60000000ULL
#define BTA_RELEASE_BODY_CAP 65536U
#define BTA_GITHUB_LATEST_URL "https://api.github.com/repos/alexutzusoft/btaudio4/releases/latest"

#define BTA_UPD_NONE     0
#define BTA_UPD_LATEST   1
#define BTA_UPD_OUTDATED 2
#define BTA_UPD_MISMATCH 3

#define BTA_DL_NONE      0
#define BTA_DL_RUNNING   1
#define BTA_DL_DONE      2
#define BTA_DL_ERROR     3

struct bta_release {
    char tag[64];
    char pkg_name[128];
    char pkg_url[512];
    unsigned long long pkg_size;
};

static inline int bta_matches(const char *s, const char *prefix, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] != prefix[i]) return 0;
    }
    return 1;
}

static inline int bta_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Parses a version string like "v1.4.0" or "1.4.0-rc1" into 4 numeric parts and
 * an optional prerelease pointer (up to '+', whitespace, or NUL). Returns 0 on success. */
static inline int bta_parse_version(const char *s, int parts[4], const char **pre_out) {
    if (!s) return -1;
    while (bta_is_space(*s)) s++;
    if (*s == 'v' || *s == 'V') s++;
    if (*s < '0' || *s > '9') return -1;

    for (int i = 0; i < 4; i++) parts[i] = 0;

    int idx = 0;
    for (;;) {
        if (*s < '0' || *s > '9') return -1;
        long val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 999999) return -1;
            s++;
        }
        if (idx < 4) parts[idx++] = (int)val;
        if (*s != '.') break;
        s++;
    }

    const char *pre = 0;
    if (*s == '-') {
        s++;
        if (*s && !bta_is_space(*s) && *s != '+') pre = s;
    }
    *pre_out = pre;
    return 0;
}

static inline int bta_pre_len(const char *p) {
    int n = 0;
    while (p[n] && !bta_is_space(p[n]) && p[n] != '+') n++;
    return n;
}

/* Compares version A against B:
 *   returns  1 if A > B
 *   returns -1 if A < B
 *   returns  0 if A == B
 *   returns -2 if either string is invalid. */
static inline int bta_compare_versions(const char *a, const char *b) {
    int pa[4], pb[4];
    const char *pre_a = 0, *pre_b = 0;
    if (bta_parse_version(a, pa, &pre_a) < 0) return -2;
    if (bta_parse_version(b, pb, &pre_b) < 0) return -2;

    for (int i = 0; i < 4; i++) {
        if (pa[i] != pb[i]) return pa[i] > pb[i] ? 1 : -1;
    }
    if (!pre_a && !pre_b) return 0;
    if (pre_a && !pre_b) return -1;
    if (!pre_a && pre_b) return 1;

    int la = bta_pre_len(pre_a), lb = bta_pre_len(pre_b);
    int minl = la < lb ? la : lb;
    for (int i = 0; i < minl; i++) {
        if ((unsigned char)pre_a[i] != (unsigned char)pre_b[i])
            return (unsigned char)pre_a[i] > (unsigned char)pre_b[i] ? 1 : -1;
    }
    if (la != lb) return la > lb ? 1 : -1;
    return 0;
}

static inline int bta_read_json_str(const char *json, size_t len, size_t *pos, char *out, size_t cap) {
    size_t i = *pos;
    if (i >= len || json[i] != '"') return -1;
    i++;
    size_t used = 0;
    while (i < len && json[i] != '"') {
        char ch = json[i++];
        if (ch == '\\' && i < len) {
            char esc = json[i++];
            if (esc == '"' || esc == '\\' || esc == '/') ch = esc;
            else return -1;
        }
        if ((unsigned char)ch < 32 || used + 1 >= cap) return -1;
        out[used++] = ch;
    }
    if (i >= len) return -1;
    out[used] = 0;
    *pos = i + 1;
    return 0;
}

/* Extracts the top-level JSON string value for key "tag_name". Returns 0 on success. */
static inline int bta_extract_tag_name(const char *json, size_t len, char *out, size_t cap) {
    if (!json || !out || cap < 2) return -1;
    out[0] = 0;

    int depth = 0;
    size_t i = 0;
    while (i < len) {
        char c = json[i];
        if (c == '"') {
            size_t key_start = i + 1;
            i++;
            while (i < len) {
                if (json[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (json[i] == '"') break;
                i++;
            }
            if (i >= len) return -1;
            size_t key_len = i - key_start;
            i++; /* skip closing quote */

            if (depth == 1 && key_len == 8 && bta_matches(json + key_start, "tag_name", 8)) {
                while (i < len && bta_is_space(json[i])) i++;
                if (i < len && json[i] == ':') {
                    i++;
                    while (i < len && bta_is_space(json[i])) i++;
                    if (i < len && json[i] == '"')
                        return bta_read_json_str(json, len, &i, out, cap);
                }
            }
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { if (depth > 0) depth--; }
        i++;
    }
    return -1;
}

/* Formats a version string as "v<version>" without duplicating a leading 'v'. */
static inline void bta_format_v(const char *tag, char *out, size_t cap) {
    if (!tag || !out || cap < 2) return;
    while (bta_is_space(*tag)) tag++;
    if (*tag == 'v' || *tag == 'V') tag++;
    size_t n = 0;
    out[n++] = 'v';
    while (*tag && !bta_is_space(*tag) && n + 1 < cap) out[n++] = *tag++;
    out[n] = 0;
}

static inline void bta_copy_string(char *out, const char *s) {
    while (*s) *out++ = *s++;
    *out = 0;
}

static inline int bta_ends_with_ci(const char *s, const char *suf) {
    size_t ls = 0, lq = 0;
    while (s[ls]) ls++;
    while (suf[lq]) lq++;
    if (ls < lq) return 0;
    for (size_t i = 0; i < lq; i++) {
        char a = s[ls - lq + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static inline int bta_safe_filename(const char *s) {
    if (!s || !s[0]) return 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '/' || s[i] == '\\' || (unsigned char)s[i] < 32) return 0;
        if (s[i] == '.' && s[i + 1] == '.') return 0;
    }
    return 1;
}

/* Finds the first .pkg asset in the top-level "assets" array. Returns 0 on success. */
static inline void bta_extract_pkg_asset(const char *json, size_t len, struct bta_release *release) {

    int depth = 0, in_assets = 0, assets_depth = 0;
    char cur_name[128] = {0}, cur_url[512] = {0}, cur_api_url[512] = {0};
    unsigned long long cur_size = 0;

    size_t i = 0;
    while (i < len) {
        char c = json[i];
        if (c == '"') {
            size_t ks = i + 1;
            i++;
            while (i < len) {
                if (json[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (json[i] == '"') break;
                i++;
            }
            if (i >= len) return;
            size_t kl = i - ks;
            i++; /* skip closing quote */

            size_t j = i;
            while (j < len && bta_is_space(json[j])) j++;
            if (j < len && json[j] == ':') {
                j++;
                while (j < len && bta_is_space(json[j])) j++;
                if (depth == 1 && !in_assets && kl == 6 && bta_matches(json + ks, "assets", 6)) {
                    if (j < len && json[j] == '[') {
                        in_assets = 1;
                        assets_depth = 2;
                        depth = 2;
                        i = j + 1;
                        continue;
                    }
                } else if (in_assets && depth == assets_depth + 1) {
                    if (kl == 4 && bta_matches(json + ks, "name", 4) && j < len && json[j] == '"') {
                        if (bta_read_json_str(json, len, &j, cur_name, sizeof(cur_name))) return;
                        i = j;
                        continue;
                    } else if (kl == 3 && bta_matches(json + ks, "url", 3) && j < len && json[j] == '"') {
                        if (bta_read_json_str(json, len, &j, cur_api_url, sizeof(cur_api_url))) return;
                        i = j;
                        continue;
                    } else if (kl == 20 && bta_matches(json + ks, "browser_download_url", 20) && j < len && json[j] == '"') {
                        if (bta_read_json_str(json, len, &j, cur_url, sizeof(cur_url))) return;
                        i = j;
                        continue;
                    } else if (kl == 4 && bta_matches(json + ks, "size", 4) && j < len && json[j] >= '0' && json[j] <= '9') {
                        unsigned long long v = 0;
                        while (j < len && json[j] >= '0' && json[j] <= '9') {
                            v = v * 10 + (unsigned long long)(json[j] - '0');
                            j++;
                        }
                        cur_size = v;
                        i = j;
                        continue;
                    }
                }
            }
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
            if (in_assets && c == '{' && depth == assets_depth + 1) {
                cur_name[0] = 0;
                cur_url[0] = 0;
                cur_api_url[0] = 0;
                cur_size = 0;
            }
        } else if (c == '}' || c == ']') {
            if (in_assets && c == '}' && depth == assets_depth + 1) {
                /* Prefer GitHub's API asset url (small 302 headers) over github.com (huge CSP header). */
                static const char api_prefix[] = "https://api.github.com/repos/alexutzusoft/btaudio4/releases/assets/";
                static const char browser_prefix[] = "https://github.com/alexutzusoft/btaudio4/releases/download/";
                const char *chosen_url = bta_matches(cur_api_url, api_prefix, sizeof(api_prefix) - 1)
                    ? cur_api_url : bta_matches(cur_url, browser_prefix, sizeof(browser_prefix) - 1) ? cur_url : 0;
                if (chosen_url && bta_safe_filename(cur_name) && bta_ends_with_ci(cur_name, ".pkg")) {
                    bta_copy_string(release->pkg_name, cur_name);
                    bta_copy_string(release->pkg_url, chosen_url);
                    release->pkg_size = cur_size;
                    return;
                }
            }
            if (in_assets && c == ']' && depth == assets_depth) {
                in_assets = 0;
            }
            if (depth > 0) depth--;
        }
        i++;
    }
}

/* Both the GitHub response and the local endpoint use these metadata fields. */
static inline int bta_parse_release(const char *json, size_t len, struct bta_release *release) {
    char tag[sizeof(release->tag)];
    if (bta_extract_tag_name(json, len, tag, sizeof(tag)) ||
        bta_compare_versions(tag, tag) < 0) return -1;
    bta_format_v(tag, release->tag, sizeof(release->tag));
    bta_extract_pkg_asset(json, len, release);
    return 0;
}

/* Extracts a Location header URL from raw HTTP response headers. Returns 0 on success. */
static inline int bta_extract_location_header(const char *hdrs, size_t len, char *out, size_t cap) {
    if (!hdrs || !out || cap < 2) return -1;
    out[0] = 0;
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && hdrs[i] != '\n' && hdrs[i] != '\r') i++;
        size_t line_len = i - line_start;
        if (line_len > 9) {
            const char *k = "location:";
            int match = 1;
            for (int c = 0; c < 9; c++) {
                char ch = hdrs[line_start + (size_t)c];
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                if (ch != k[c]) { match = 0; break; }
            }
            if (match) {
                size_t p = line_start + 9;
                while (p < line_start + line_len && (hdrs[p] == ' ' || hdrs[p] == '\t')) p++;
                size_t used = 0;
                while (p < line_start + line_len && used + 1 < cap) out[used++] = hdrs[p++];
                while (used > 0 && (out[used - 1] == ' ' || out[used - 1] == '\t')) used--;
                out[used] = 0;
                return (bta_matches(out, "https://", 8) || bta_matches(out, "http://", 7)) ? 0 : -1;
            }
        }
        while (i < len && (hdrs[i] == '\r' || hdrs[i] == '\n')) i++;
    }
    return -1;
}

#endif

#ifndef BTA_UID_H
#define BTA_UID_H

#define BTA_UID_LEN 9
#define BTA_UID_ALPHABET "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

static inline int bta_uid_is_alnum(char c) {
 return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline char bta_uid_upper(char c) {
 return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static inline int bta_uid_normalize(const char *in, long in_len, char out[10]) {
 if (!in || in_len < BTA_UID_LEN) return 0;
 if (in[4] != '-') return 0;
 for (long i = BTA_UID_LEN; i < in_len; i++) {
  char c = in[i];
  if (c == 0) break;
  if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return 0;
 }
 for (int i = 0; i < BTA_UID_LEN; i++) {
  if (i == 4) {
   out[i] = '-';
   continue;
  }
  if (!bta_uid_is_alnum(in[i])) return 0;
  out[i] = bta_uid_upper(in[i]);
 }
 out[BTA_UID_LEN] = 0;
 return 1;
}

static inline int bta_hex16_normalize(const char *in, long in_len, char out[17]) {
 if (!in || in_len < 16) return 0;
 for (long i = 16; i < in_len; i++) {
  char c = in[i];
  if (c == 0) break;
  if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return 0;
 }
 for (int i = 0; i < 16; i++) {
  char c = in[i];
  if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
  if (c >= 'A' && c <= 'F') c = (char)(c + 32);
  out[i] = c;
 }
 out[16] = 0;
 return 1;
}

static inline void bta_uid_from_u64(unsigned long long x, char out[10]) {
 static const char alpha[] = BTA_UID_ALPHABET;
 if (!x) x = 0x4254413455494431ULL;
 for (int i = 0; i < 8; i++) {
  int pos = i < 4 ? i : i + 1;
  out[pos] = alpha[(x >> (i * 5)) & 31U];
 }
 out[4] = '-';
 out[BTA_UID_LEN] = 0;
}

static inline int bta_extract_uid_json(const char *body, char out[10]) {
 if (!body) return 0;
 for (const char *p = body; *p; p++) {
  if (p[0] == '"' && p[1] == 'u' && p[2] == 'i' && p[3] == 'd' && p[4] == '"') {
   const char *q = p + 5;
   while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
   if (*q != ':') continue;
   q++;
   while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
   if (*q != '"') continue;
   q++;
   for (int i = 0; i < BTA_UID_LEN; i++) {
    if (!q[i]) return 0;
   }
   if (q[BTA_UID_LEN] != '"') continue;
   if (bta_uid_normalize(q, BTA_UID_LEN, out)) return 1;
  }
 }
 return 0;
}

#endif

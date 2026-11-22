#ifndef BTA_DIAG_H
#define BTA_DIAG_H

#include "runtime.h"

#define DIAG_RING_SIZE 524288U
#define DIAG_CHUNK_MAX 16384U
#define DIAG_DEFAULT_HOST "bta.alexutzu.pro"
#define DIAG_DEFAULT_PORT 80
#define DIAG_URL_PATH_A "/data/bta_audio/diag_url"
#define DIAG_URL_PATH_B "/data/bta_audio/diag_url.txt"
#define DIAG_CLIENT_ID_PATH "/data/bta_audio/client_id"

struct diag_reporter {
 u8 *ring;
 u32 cap;
 u32 head;
 u32 tail;
 u64 dropped;
 u64 dropped_bytes;
 u64 sent_bytes;
 u64 sent_chunks;
 u64 failed_sends;
 u32 next_seq;
 u32 urgent;
 int run;
 int alive;
 int exited;
 int streaming;
 u64 stop_deadline;
 void *thread;
 int (*sleep)(unsigned);
 u64 (*clock)(void);
 void *(*map)(void *, u64, int, int, int, u64);
 int (*unmap)(void *, u64);
 int (*join)(void *, void **);
 char fw[32];
 char client_id[17];
 char legacy_id[17];
 int client_id_ready;
 char session_id[17];
 int last_err_stage;
 int last_http_code;
 u32 resolved_ip;
};

void diag_init(struct diag_reporter *d,
               int (*sleep_fn)(unsigned),
               u64 (*clock_fn)(void),
               void *(*map_fn)(void *, u64, int, int, int, u64),
               int (*unmap_fn)(void *, u64),
               const char *fw_text);

void diag_push(struct diag_reporter *d, const char *data, u32 len);
void diag_push_ex(struct diag_reporter *d, const char *data, u32 len, int urgent);
void diag_stop(struct diag_reporter *d);

#endif

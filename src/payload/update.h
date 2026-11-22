#ifndef BTA_PAYLOAD_UPDATE_H
#define BTA_PAYLOAD_UPDATE_H

#include "../common/announcement.h"

#define BTA_UPDATE_TEST_PATH "/api/test/update"
#define BTA_UPDATE_TEST_CAP 2048U
enum bta_update_test_status {
 BTA_UPDATE_TEST_INVALID=-2,
 BTA_UPDATE_TEST_DISABLED=-1,
 BTA_UPDATE_TEST_LIVE=0,
 BTA_UPDATE_TEST_TIMEOUT=1,
};

struct bta_notification_history;

struct bta_update {
 struct bta_release release, fetched;
 struct bta_announcement announcement, announcement_fetched;
 int announcement_status, announcement_result, announcement_test, announcement_fetched_test;
 int announcement_ready, have_announcement;
 u64 next_announcement_check;
 void *announcement_thread;
 int status, result, ready, have_release, stopping, streaming;
 u64 next_check,app_foreground_until;
 void *thread;
 int (*join)(void *,void **);
 int (*sleep)(unsigned);
 u64 (*clock)(void);
 void *(*map)(void *,u64,int,int,int,u64);
 int (*unmap)(void *,u64);
 struct bta_notification_history *notified_releases,*notified_announcements;
};

void bta_update_tick(struct bta_update *update,u64 now);
void bta_update_stop(struct bta_update *update);
int bta_update_test_enabled(void);
int bta_update_test_status(const char *text,size_t len);
int bta_update_test_read(char *body,size_t *len);
int bta_update_test_write(int status,const char *body,size_t len);
int bta_announcement_test_enabled(void);
int bta_announcement_test_read(char *body,size_t *len);
int bta_announcement_test_write(int status,const char *body,size_t len);

#endif

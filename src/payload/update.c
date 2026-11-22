#include "runtime.h"
#include "update.h"
#include "../common/version.h"
#include "notification_icon.h"

#define HTTP_TIMEOUT_US 5000000U
#define STREAM_DEFER_INTERVAL_US 600000000ULL
#define HTTP_POOL_SIZE (128U * 1024)
#define NOTIFIED_PAGE_SIZE 16384U
#define UPDATE_TEST_ENABLE "/data/bta_audio/enable_update_test"
#define UPDATE_TEST_FILE "/data/bta_audio/update-test-response"
#define ANNOUNCEMENT_TEST_ENABLE "/data/bta_audio/enable_announcement_test"
#define ANNOUNCEMENT_TEST_FILE "/data/bta_audio/announcement-test-response"

struct bta_notification_history {
 struct bta_notification_history *next;
 unsigned count;
 char keys[248][BTA_ANNOUNCEMENT_ID_CAP+1];
};

_Static_assert(sizeof(struct bta_notification_history)<=NOTIFIED_PAGE_SIZE,"notification history must fit in one page");

int bta_update_test_enabled(void){
 return exists(UPDATE_TEST_ENABLE);
}

int bta_update_test_status(const char *text,size_t len){
 if(len==7&&bta_matches(text,"timeout",7))return BTA_UPDATE_TEST_TIMEOUT;
 if(len!=3)return BTA_UPDATE_TEST_INVALID;
 for(size_t i=0;i<len;i++)if(text[i]<'0'||text[i]>'9')return BTA_UPDATE_TEST_INVALID;
 int status=(text[0]-'0')*100+(text[1]-'0')*10+text[2]-'0';
 if(status==200||status==403||status==404||status==429||(status>=500&&status<=599))return status;
 return BTA_UPDATE_TEST_INVALID;
}

static int read_fixture(const char *path,int enabled,char *body,size_t capacity,size_t *len){
 *len=0;
 if(!enabled)return BTA_UPDATE_TEST_DISABLED;
 long fd=sc3(5,(long)path,0,0);
 if(fd<0)return fd==-2?BTA_UPDATE_TEST_LIVE:BTA_UPDATE_TEST_INVALID; /* ENOENT */
 long n=sc3(3,fd,(long)body,capacity+1);
 closefd(fd);
 if(n<=0||(size_t)n>capacity)return BTA_UPDATE_TEST_INVALID;
 *len=(size_t)n;
 if(body[0]=='{')return 200;
 int status=bta_update_test_status(body,*len);
 return status==200?BTA_UPDATE_TEST_INVALID:status;
}

int bta_update_test_read(char *body,size_t *len){
 return read_fixture(UPDATE_TEST_FILE,bta_update_test_enabled(),body,BTA_UPDATE_TEST_CAP,len);
}

int bta_announcement_test_enabled(void){
 return exists(ANNOUNCEMENT_TEST_ENABLE);
}

int bta_announcement_test_read(char *body,size_t *len){
 return read_fixture(ANNOUNCEMENT_TEST_FILE,bta_announcement_test_enabled(),body,BTA_ANNOUNCEMENT_JSON_CAP,len);
}

__attribute__((cold)) static int write_fixture(const char *path,const char *temp,int status,const char *body,size_t len){
 if(status==BTA_UPDATE_TEST_LIVE){
  long rc=sc3(10,(long)path,0,0);
  return rc==0||rc==-2?0:-1; /* ENOENT is already reset. */
 }
 char code[3];
 if(status==BTA_UPDATE_TEST_TIMEOUT){
  body="timeout";len=7;
 }else if(status!=200){
  code[0]=(char)('0'+status/100);
  code[1]=(char)('0'+status/10%10);
  code[2]=(char)('0'+status%10);
  body=code;len=sizeof(code);
 }
 if(put(temp,body,len)==0&&sc3(128,(long)temp,(long)path,0)==0)return 0;
 sc3(10,(long)temp,0,0);
 return -1;
}

__attribute__((cold)) int bta_update_test_write(int status,const char *body,size_t len){
 if(status==200){
  while(len&&bta_is_space(*body)){body++;len--;}
  struct bta_release release={0};
  if(!len||len>BTA_UPDATE_TEST_CAP||body[0]!='{'||bta_parse_release(body,len,&release)<0)return -2;
 }
 return write_fixture(UPDATE_TEST_FILE,UPDATE_TEST_FILE ".tmp",status,body,len);
}

__attribute__((cold)) int bta_announcement_test_write(int status,const char *body,size_t len){
 if(status==200){
  while(len&&bta_is_space(*body)){body++;len--;}
  struct bta_announcement a;
  if(!len||len>BTA_ANNOUNCEMENT_JSON_CAP||bta_parse_announcement(body,len,&a)<0||a.test)return -2;
 }
 return write_fixture(ANNOUNCEMENT_TEST_FILE,ANNOUNCEMENT_TEST_FILE ".tmp",status,body,len);
}

struct metadata_modules{long net,ssl,http;};
static int fetch_metadata(struct metadata_modules *modules,const char *url,const char *accept,
                          char *body,size_t cap,size_t *body_len){
 /* Each poller reuses its own module handles across checks. */
 if(!modules->net)modules->net=load_system_module("/system/common/lib/libSceNet.sprx");
 if(!modules->ssl)modules->ssl=load_system_module("/system/common/lib/libSceSsl.sprx");
 if(!modules->http)modules->http=load_system_module("/system/common/lib/libSceHttp.sprx");
 long net=modules->net,ssl=modules->ssl,http=modules->http;
 if(!net||!ssl||!http)return -1;

 int (*sceNetInit)(void)=0;
 int (*sceNetPoolCreate)(const char *,int,int)=0;
 int (*sceNetPoolDestroy)(int)=0;
 int (*sceSslInit)(size_t)=0,(*sceSslTerm)(int)=0;
 int (*sceHttpInit)(int,int,size_t)=0,(*sceHttpTerm)(int)=0;
 int (*sceHttpCreateTemplate)(int,const char *,int,int)=0,(*sceHttpDeleteTemplate)(int)=0;
 int (*sceHttpSetResolveTimeOut)(int,u32)=0,(*sceHttpSetConnectTimeOut)(int,u32)=0;
 int (*sceHttpSetSendTimeOut)(int,u32)=0,(*sceHttpSetRecvTimeOut)(int,u32)=0;
 int (*sceHttpCreateConnectionWithURL)(int,const char *,_Bool)=0,(*sceHttpDeleteConnection)(int)=0;
 int (*sceHttpCreateRequestWithURL)(int,int,const char *,u64)=0,(*sceHttpDeleteRequest)(int)=0;
 int (*sceHttpAddRequestHeader)(int,const char *,const char *,int)=0;
 int (*sceHttpSendRequest)(int,const void *,size_t)=0,(*sceHttpGetStatusCode)(int,int *)=0;
 int (*sceHttpReadData)(int,void *,u32)=0;
#define LOAD(module,fn) (!sc3(591,module,(long)#fn,(long)&fn)&&fn)
 if(!LOAD(net,sceNetInit)||!LOAD(net,sceNetPoolCreate)||!LOAD(net,sceNetPoolDestroy)||
    !LOAD(ssl,sceSslInit)||!LOAD(ssl,sceSslTerm)||
    !LOAD(http,sceHttpInit)||!LOAD(http,sceHttpTerm)||
    !LOAD(http,sceHttpCreateTemplate)||!LOAD(http,sceHttpDeleteTemplate)||
    !LOAD(http,sceHttpSetResolveTimeOut)||!LOAD(http,sceHttpSetConnectTimeOut)||
    !LOAD(http,sceHttpSetSendTimeOut)||!LOAD(http,sceHttpSetRecvTimeOut)||
    !LOAD(http,sceHttpCreateConnectionWithURL)||!LOAD(http,sceHttpDeleteConnection)||
    !LOAD(http,sceHttpCreateRequestWithURL)||!LOAD(http,sceHttpDeleteRequest)||
    !LOAD(http,sceHttpAddRequestHeader)||!LOAD(http,sceHttpSendRequest)||
    !LOAD(http,sceHttpGetStatusCode)||!LOAD(http,sceHttpReadData))return -2;
#undef LOAD

 int pool=-1,ssl_id=-1,http_id=-1,tpl=-1,conn=-1,req=-1,result=-3;
 sceNetInit();
 pool=sceNetPoolCreate("bta_release",16*1024,0);
 if(pool<0)goto done;
 ssl_id=sceSslInit(HTTP_POOL_SIZE);
 if(ssl_id<0)goto done;
 http_id=sceHttpInit(pool,ssl_id,HTTP_POOL_SIZE);
 if(http_id<0)goto done;
 tpl=sceHttpCreateTemplate(http_id,"BTAudio4/" BTA_VERSION,2,1); /* HTTP/1.1 */
 if(tpl<0)goto done;
 if(sceHttpSetResolveTimeOut(tpl,HTTP_TIMEOUT_US)<0||
    sceHttpSetConnectTimeOut(tpl,HTTP_TIMEOUT_US)<0||
    sceHttpSetSendTimeOut(tpl,HTTP_TIMEOUT_US)<0||
    sceHttpSetRecvTimeOut(tpl,HTTP_TIMEOUT_US)<0)goto done;

 result=-4;
 conn=sceHttpCreateConnectionWithURL(tpl,url,0);
 if(conn<0)goto done;
 req=sceHttpCreateRequestWithURL(conn,0,url,0); /* GET */
 if(req<0)goto done;
 if(sceHttpAddRequestHeader(req,"Accept",accept,0)<0)goto done;
 int rc=sceHttpSendRequest(req,0,0);
 if(rc<0){result=rc;goto done;}
 int code=0;
 if(sceHttpGetStatusCode(req,&code)<0)goto done;
 if(code!=200){result=code;goto done;}

 result=-5;
 size_t used=0;
 while(used<cap){
  int n=sceHttpReadData(req,body+used,(u32)(cap-used));
  if(n<0){result=n;break;}
  if(!n){
   *body_len=used;
   result=200;
   break;
  }
  used+=(size_t)n;
 }
done:
 if(req>=0)sceHttpDeleteRequest(req);
 if(conn>=0)sceHttpDeleteConnection(conn);
 if(tpl>=0)sceHttpDeleteTemplate(tpl);
 if(http_id>=0)sceHttpTerm(http_id);
 if(ssl_id>=0)sceSslTerm(ssl_id);
 if(pool>=0)sceNetPoolDestroy(pool);
 return result;
}

static const char *notification_icon_uri(int announcement){
 /* Use the ShellUI-visible path so the standalone payload can brand the toast. */
 #define ICON_PATH "/user/data/bta_audio/update-icon.png"
 const char *temp=announcement?ICON_PATH ".announcement.tmp":ICON_PATH ".tmp";
 sc3(136,(long)"/user/data/bta_audio",0755,0);
 if(put(temp,notification_icon_png,sizeof(notification_icon_png))==0&&
    sc3(128,(long)temp,(long)ICON_PATH,0)==0)return "file:/" ICON_PATH;
 sc3(10,(long)temp,0,0);
 return 0;
 #undef ICON_PATH
}

static int first_notification(struct bta_update *update,struct bta_notification_history **history,const char *key){
 for(struct bta_notification_history *page=*history;page;page=page->next){
  for(unsigned i=0;i<page->count;i++)if(equal(page->keys[i],key))return 0;
 }
 struct bta_notification_history *page=*history;
 if(!page||page->count==sizeof(page->keys)/sizeof(page->keys[0])){
  page=update->map(0,NOTIFIED_PAGE_SIZE,3,0x1002,-1,0);
  if(!page||page==(void *)-1)return 0;
  page->count=0;
  page->next=*history;
  *history=page;
 }
 memcpy(page->keys[page->count++],key,length(key)+1);
 return update->clock()>=__atomic_load_n(&update->app_foreground_until,__ATOMIC_ACQUIRE);
}

static void announce_release(struct bta_update *update,const char *tag){
 if(!first_notification(update,&update->notified_releases,tag))return;
 static const char heading[]="BTAudio4 Update Available\nVersion ";
 static const char ending[]=" is now available.";
 char message[sizeof(heading)+sizeof(update->fetched.tag)+sizeof(ending)-2];
 u64 tag_len=length(tag);
 memcpy(message,heading,sizeof(heading)-1);
 memcpy(message+sizeof(heading)-1,tag,tag_len);
 memcpy(message+sizeof(heading)-1+tag_len,ending,sizeof(ending));
 notify_with_icon(message,notification_icon_uri(0));
}

static void notify_announcement(struct bta_update *update,const struct bta_announcement *a){
 char marker[BTA_ANNOUNCEMENT_MARKER_CAP];
 bta_announcement_marker(a,marker);
 if(exists(marker))return;

 char key[BTA_ANNOUNCEMENT_ID_CAP+1];
 key[0]=a->test?'T':'L';
 memcpy(key+1,a->id,length(a->id)+1);
 if(!first_notification(update,&update->notified_announcements,key))return;

 notify_with_icon("BTAudio4 Announcement\nOpen BTAudio4 to read it.",notification_icon_uri(1));
}

static void announcement_url(char out[BTA_SERVER_URL_CAP]){
 static const char *const paths[]={"/data/bta_audio/diag_url","/data/bta_audio/diag_url.txt"};
 char raw[256];
 for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
  long fd=sc3(5,(long)paths[i],0,0);
  if(fd<0)continue;
  long n=sc3(3,fd,(long)raw,sizeof(raw)-1);
  closefd(fd);
  if(n>0){raw[n]=0;if(!bta_server_url(raw,BTA_ANNOUNCEMENT_PATH,out))return;}
 }
 static const char fallback[]=BTA_SERVER_URL_DEFAULT BTA_ANNOUNCEMENT_PATH;
 memcpy(out,fallback,sizeof(fallback));
}

static void *check_announcement(void *arg){
 struct bta_update *update=arg;
 static struct metadata_modules modules;
 char body[BTA_ANNOUNCEMENT_JSON_CAP+1];
 size_t len=0;
 int status=bta_announcement_test_read(body,&len);
 int test=status!=BTA_UPDATE_TEST_DISABLED&&status!=BTA_UPDATE_TEST_LIVE;
 if(!test){
  char url[BTA_SERVER_URL_CAP];
  announcement_url(url);
  status=fetch_metadata(&modules,url,"application/json",body,sizeof(body),&len);
 }else if(status==BTA_UPDATE_TEST_TIMEOUT){
  update->sleep(HTTP_TIMEOUT_US);
  status=-8;
 }else if(status==BTA_UPDATE_TEST_INVALID)status=-9;
 if(status==200){
  struct bta_announcement *a=&update->announcement_fetched;
  if(bta_parse_announcement(body,len,a)<0)status=-5;
  else{
   a->test=test;
   if(!__atomic_load_n(&update->stopping,__ATOMIC_ACQUIRE))notify_announcement(update,a);
  }
 }
 update->announcement_fetched_test=test;
 update->announcement_result=status;
 __atomic_store_n(&update->announcement_ready,1,__ATOMIC_RELEASE);
 return 0;
}

static void *check_release(void *arg){
 struct bta_update *update=arg;
 static struct metadata_modules modules;
 char *body=update->map(0,BTA_RELEASE_BODY_CAP,3,0x1002,-1,0);
 memset(&update->fetched,0,sizeof(update->fetched));
 update->result=-6;
 if(body&&body!=(void *)-1){
  size_t len=0;
  int status=bta_update_test_read(body,&len);
  if(status==BTA_UPDATE_TEST_DISABLED||status==BTA_UPDATE_TEST_LIVE)
   status=fetch_metadata(&modules,BTA_GITHUB_LATEST_URL,"application/vnd.github+json",body,BTA_RELEASE_BODY_CAP,&len);
  else if(status==BTA_UPDATE_TEST_TIMEOUT){
   update->sleep(HTTP_TIMEOUT_US);
   status=-8;
  }else if(status==BTA_UPDATE_TEST_INVALID)status=-9;
  if(status==200&&bta_parse_release(body,len,&update->fetched)<0)status=-5;
  update->result=status;
  update->unmap(body,BTA_RELEASE_BODY_CAP);
 }
 const char *tag=update->fetched.tag;
 if(update->result==200&&bta_compare_versions(tag,BTA_VERSION)>0&&
    !__atomic_load_n(&update->stopping,__ATOMIC_ACQUIRE))announce_release(update,tag);
 __atomic_store_n(&update->ready,1,__ATOMIC_RELEASE);
 return 0;
}

static int start_poller(struct bta_update *update,void **thread,void *(*check)(void *),const char *name){
 int (*create)(void **,void **,void *(*)(void *),void *,const char *)=0;
 if(resolve("scePthreadCreate",&create)||!create||
    (!update->join&&resolve("scePthreadJoin",&update->join))||!update->join||
    create(thread,0,check,update,name)){
  *thread=0;
  return -1;
 }
 return 0;
}

static u64 update_interval(const struct bta_update *update,u64 now,u64 base_iv){
 int fg=now<__atomic_load_n(&update->app_foreground_until,__ATOMIC_ACQUIRE);
 int st=__atomic_load_n(&update->streaming,__ATOMIC_RELAXED);
 return (st&&!fg)?STREAM_DEFER_INTERVAL_US:base_iv;
}

static void announcement_tick(struct bta_update *update,u64 now){
 if(update->announcement_thread&&__atomic_exchange_n(&update->announcement_ready,0,__ATOMIC_ACQ_REL)){
  update->join(update->announcement_thread,0);
  update->announcement_thread=0;
  if(update->announcement_result==200){
   update->announcement=update->announcement_fetched;
   update->have_announcement=1;
  }else if(update->announcement_result==204||update->announcement_result==404||
           update->announcement_test!=update->announcement_fetched_test){
   update->announcement.id[0]=0;
   update->have_announcement=1;
  }
  update->announcement_test=update->announcement_fetched_test;
  update->announcement_status=update->announcement_result;
 }
 u64 iv=update_interval(update,now,BTA_ANNOUNCEMENT_INTERVAL_US);
 if(update->announcement_thread)return;
 if(iv==BTA_ANNOUNCEMENT_INTERVAL_US&&update->next_announcement_check>now+iv)
  update->next_announcement_check=now+iv;
 if(now<update->next_announcement_check)return;
 if(iv>BTA_ANNOUNCEMENT_INTERVAL_US&&update->have_announcement){
  update->next_announcement_check=now+iv;
  return;
 }
 update->next_announcement_check=now+iv;
 if(start_poller(update,&update->announcement_thread,check_announcement,"BTAudio4 announcement"))
  update->announcement_status=-7;
}

void bta_update_tick(struct bta_update *update,u64 now){
 announcement_tick(update,now);
 if(update->thread&&__atomic_exchange_n(&update->ready,0,__ATOMIC_ACQ_REL)){
  update->join(update->thread,0);
  update->thread=0;
  if(update->result==200||update->result==404){
   update->release=update->fetched;
   update->have_release=1;
  }
  update->status=update->result;
 }
 u64 iv=update_interval(update,now,BTA_UPDATE_INTERVAL_US);
 if(update->thread)return;
 if(iv==BTA_UPDATE_INTERVAL_US&&update->next_check>now+iv)
  update->next_check=now+iv;
 if(now<update->next_check)return;
 if(iv>BTA_UPDATE_INTERVAL_US&&update->have_release){
  update->next_check=now+iv;
  return;
 }
 update->next_check=now+iv;
 if(start_poller(update,&update->thread,check_release,"BTAudio4 update"))
  update->status=-7;
}

static void free_notification_history(struct bta_update *update,struct bta_notification_history *page){
 while(page){
  struct bta_notification_history *next=page->next;
  update->unmap(page,NOTIFIED_PAGE_SIZE);
  page=next;
 }
}

void bta_update_stop(struct bta_update *update){
 __atomic_store_n(&update->stopping,1,__ATOMIC_RELEASE);
 if(update->thread)update->join(update->thread,0);
 if(update->announcement_thread)update->join(update->announcement_thread,0);
 free_notification_history(update,update->notified_releases);
 free_notification_history(update,update->notified_announcements);
}

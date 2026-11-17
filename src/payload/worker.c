#include "runtime.h"
#include "web_page.h"
#include "protocol.h"
#include "kresolve.h"
#include "diag.h"
#include "../version.h"
int bta_apply_patch(void);

#define WORKER_LOG "/data/bta_audio/worker.log"
#define WORKER_LOG_PREV "/data/bta_audio/worker.prev.log"
#define WORKER_LOG_MAX 262144U

/* Keep HDMI unmuted for this long after streaming starts so the system audio
 * mix that feeds /shm_acap.full actually activates, THEN apply the user's
 * mute preference. Starting muted leaves the capture ring empty (silence). */
#define PRIME_US 2000000
/* One cycle of a sine at ~8000/32767 (~24%) full scale for the safe test tone. */
static const short SINE32[32]={0,1561,3061,4445,5657,6652,7391,7846,8000,7846,7391,6652,5657,4445,3061,1561,0,-1561,-3061,-4445,-5657,-6652,-7391,-7846,-8000,-7846,-7391,-6652,-5657,-4445,-3061,-1561};

struct audio_req{u64 address;void*samples;u64 length;u32 mode,pad;};
struct client{long fd;u64 deadline;u32 used,sent,total;char input[2048];char output[20000];};
struct second{u32 seq,t,pkts,refused,failed,late8,late16,late50,maxgap_us,maxsend_us,maxback_ms,peak,rc_full,rc_one,rc_other,maxref_ms;};
enum audio_fault{AUDIO_OK,AUDIO_RING_RESET,AUDIO_RELINK,AUDIO_LINK_LOST};
struct audio_stats{
 u64 packets,errors,resyncs,ok_at,refused_since;
 long last_rc,other_rc;
 unsigned level,meter_peak;
};
/* The mutex protects this stream and its snapshots only. The worker owns the
 * mapping and descriptors and waits for an in-flight submission before teardown. */
struct audio_stream{
 const u8*ring;long bt;u64 address;
 int active,in_flight,volume,volume_synced,joint;
 u32 readpos,test_phase;
 u64 test_until,test_next,last_send;
 struct audio_stats stats;
 struct second cur;
 enum audio_fault fault;
 u32 bad_writepos,resync_backlog,relinks;
 int refused_warned,err_warned,relink_limit;
 unsigned refused_ms,refused_backlog;
 long error_rc;u64 error_count;
 short pending_pcm[384];
 unsigned pending_peak;
 int has_pending,was_testing;
};
struct app{
 int(*sleep)(unsigned);u64(*clock)(void);int(*shm)(const char*,int,int);
 void*(*map)(void*,u64,int,int,int,u64);int(*unmap)(void*,u64);
 long listener,bt,hdmi,shmfd;u8*ring;int wanted,streaming,tv,muted,quit,phase;
 long patch_rc,packet_rc,connect_rc,last_rc,other_rc,hdmi_rc,btstate,list_rc;
 u64 next_try,deadline,scan_at,heartbeat,poll_at,net_at,packets,errors,resyncs;
 int net_poll_failed;
 unsigned level;int volume,volume_synced,avrcp_pending,avrcp_retries;u64 avrcp_retry_at;char message[128];
 /* Per-packet timing, summarised per second (GET /api/timing). gap: time between two
  * sends (4 ms is on time). send: how long the driver call took. backlog: capture audio
  * waiting to be sent. refused: the driver returned 0. failed: it returned an error. */
 struct second secs[128],cur;
 u32 sec_seq;u64 sec_start;int joint;u64 refused_since;u32 relinks_total;
 u64 stream_at,test_until;
 struct audio_stream audio;
 void*mutex,*audio_cond,*sender_thread;
 int(*mlock)(void**),(*munlock)(void**),(*wait_audio)(void**,void**),(*signal_audio)(void**);
 int(*join_sender)(void*,void**),(*destroy_mutex)(void**),(*destroy_cond)(void**);
 int sender_run,sender_alive,sender_exited,sender_prio;
 long thread_rc,cond_rc,prio_rc,aff_rc,pi_rc,inherit_rc,policy_rc;
 /* Truthful link state (see link_state). dropped: the link was lost while
  * streaming. failed: the last attempt failed. announced: a "disconnected"
  * notice was shown. session: this connection has already been announced. */
 int dropped,failed,announced,session,meter;u32 attempts;u64 ok_at,meter_at,notify_at[3];unsigned meter_peak;
 struct bt_device target;struct bt_devices devices;int scanning,pin_pending;u64 scan_end,list_at;
 struct client clients[4];
 /* Buffered diagnostic log owned by the housekeeping worker. */
 char logbuf[3072];
 char logline[512];
 unsigned logused,log_dropped,logline_used;
 int log_urgent,koff_logged,patch_logged,no_widen;
 u64 log_flush_at,log_urgent_at,log_file_bytes,health_at,health_warn_at,resync_warn_at;
 struct diag_reporter diag;
 u64 bt_ev_last_at;u8 bt_ev_last_type,bt_ev_last_sub;u32 bt_ev_last_data;u64 bt_ev_last_addr;
 u32 health_resyncs,health_ok_skips;
 struct second health;
 u64 bp_check_at,bp_adapt_at,bp_stable_at;
 u32 bp_last_drops,bp_health_drops,bp_ten_drops,bp_win[8];
 int bp_target,bp_base,bp_cur,bp_inflight;
 u8 bp_win_idx;
};
static void lock_state(struct app*a){if(a->mlock)a->mlock(&a->mutex);}
static void unlock_state(struct app*a){if(a->munlock)a->munlock(&a->mutex);}
static void wake_audio(struct app*a){if(a->signal_audio)a->signal_audio(&a->audio_cond);}
/* Keep the dedicated sender on firmware where condition setup fails. */
static void wait_for_audio_change(struct app*a,unsigned fallback_us){
 if(a->signal_audio){a->wait_audio(&a->audio_cond,&a->mutex);return;}
 unlock_state(a);
 a->sleep(fallback_us);
 lock_state(a);
}
static void reset_audio_retries(struct app*a,int clear_refusal){
 lock_state(a);
 a->audio.relinks=0;
 if(clear_refusal){a->audio.stats.refused_since=0;a->refused_since=0;}
 unlock_state(a);
}
static void sync_audio_controls(struct app*a){
 if(!a->streaming)return;
 lock_state(a);
 struct audio_stream*s=&a->audio;
 s->volume=a->volume;s->volume_synced=a->volume_synced;s->joint=a->joint;
 if(s->test_until!=a->test_until){s->test_until=a->test_until;s->test_phase=0;s->test_next=0;}
 unlock_state(a);
}
static void copy_audio_stats(struct app*a,const struct audio_stats*s){
 a->packets=s->packets;a->errors=s->errors;a->resyncs=s->resyncs;
 a->ok_at=s->ok_at;a->refused_since=s->refused_since;a->last_rc=s->last_rc;a->other_rc=s->other_rc;
 a->level=s->level;a->meter_peak=s->meter_peak;
}
static void wlog_flush(struct app*a,u64 now,int force){
 char tmp[3072];unsigned n=0;
 if(a->logused){
  int due=force||(a->logused>=1536)||(a->log_urgent&&now>=a->log_urgent_at)||(now>=a->log_flush_at);
  if(due){n=a->logused;memcpy(tmp,a->logbuf,n);a->logused=0;a->log_urgent=0;a->log_flush_at=now+15000000;a->log_urgent_at=now+2000000;}
 }
 if(!n)return;
 if(a->log_file_bytes>=WORKER_LOG_MAX){sc3(128,(long)WORKER_LOG,(long)WORKER_LOG_PREV,0);a->log_file_bytes=0;}
 long fd=sc3(5,(long)WORKER_LOG,0x209,0644);
 if(fd>=0){long w=sc3(4,fd,(long)tmp,n);closefd(fd);if(w>0)a->log_file_bytes+=(u64)w;}
}
__attribute__((noinline)) static void wlog_raw(struct app*a,const char*s){while(*s&&a->logline_used+2<sizeof(a->logline))a->logline[a->logline_used++]=*s++;a->logline[a->logline_used]=0;}
__attribute__((noinline)) static void wlog_dec(struct app*a,long n){char b[24];unsigned i=0;u64 v;if(n<0){wlog_raw(a,"-");v=0-(u64)n;}else v=(u64)n;do{b[i++]=(char)('0'+v%10);v/=10;}while(v);while(i){char s[2]={b[--i],0};wlog_raw(a,s);}}
__attribute__((noinline)) static void wlog_hex(struct app*a,u64 v){const char*h="0123456789abcdef";wlog_raw(a,"0x");char b[16];int i=0;do{b[i++]=h[v&15];v>>=4;}while(v);while(i){char s[2]={b[--i],0};wlog_raw(a,s);}}
__attribute__((noinline)) static void wlog_mac(struct app*a,u64 mac){const char*h="0123456789ABCDEF";for(int i=0;i<12;i++){char s[2]={h[(mac>>(44-4*i))&15],0};wlog_raw(a,s);}}
__attribute__((noinline)) static void wlog_str(struct app*a,const char*s){wlog_raw(a,"\"");for(unsigned i=0;s[i]&&i<80;i++){char c=s[i];if(c=='"'||c=='\\')wlog_raw(a,"\\");char b[2]={(c>=32&&c<127)?c:'?',0};wlog_raw(a,b);}wlog_raw(a,"\"");}
__attribute__((noinline)) static int wlog_begin(struct app*a,u64 now,const char*tag){
 a->logline_used=0;a->logline[0]=0;
 u64 tenths=now/100000;
 wlog_raw(a,"[+");wlog_dec(a,(long)(tenths/10));wlog_raw(a,".");wlog_dec(a,(long)(tenths%10));wlog_raw(a,"s] ");wlog_raw(a,tag);wlog_raw(a,": ");
 return 1;
}
__attribute__((noinline)) static void wlog_end(struct app*a,int urgent){
 if(a->logline_used+1<sizeof(a->logline)){a->logline[a->logline_used++]='\n';a->logline[a->logline_used]=0;}
 diag_push(&a->diag,a->logline,a->logline_used);
 if(sizeof(a->logbuf)-a->logused<a->logline_used+32)wlog_flush(a,a->clock(),1);
 if(sizeof(a->logbuf)-a->logused>=a->logline_used+32){
  if(a->log_dropped){
   unsigned prev=a->log_dropped;a->log_dropped=0;
   const char*p="[dropped=";while(*p&&a->logused+1<sizeof(a->logbuf))a->logbuf[a->logused++]=*p++;
   char b[24];unsigned i=0;u64 v=prev;do{b[i++]=(char)('0'+v%10);v/=10;}while(v);
   while(i&&a->logused+1<sizeof(a->logbuf))a->logbuf[a->logused++]=b[--i];
   const char*s="]\n";while(*s&&a->logused+1<sizeof(a->logbuf))a->logbuf[a->logused++]=*s++;
  }
  memcpy(a->logbuf+a->logused,a->logline,a->logline_used);
  a->logused+=a->logline_used;
  a->logbuf[a->logused]=0;
 }else a->log_dropped++;
 if(urgent)a->log_urgent=1;
}
__attribute__((noinline)) static void wlog_koff(struct app*a,u64 now,int force){
 const struct bt_koff*k=bt_koff_info();
 if(!k||(!force&&a->koff_logged&&k->valid))return;
 a->koff_logged=1;
 if(wlog_begin(a,now,k->valid?"koff":"ERR koff")){
  wlog_raw(a,"valid=");wlog_dec(a,(long)k->valid);wlog_raw(a," fail_stage=");wlog_dec(a,(long)k->fail_stage);
  wlog_raw(a," list_rc=");wlog_dec(a,a->list_rc);wlog_raw(a," base=");wlog_hex(a,k->base);
  wlog_raw(a," accept=");wlog_hex(a,k->accept_site);wlog_raw(a," copyout=");wlog_hex(a,k->copyout_fn);
  wlog_raw(a," lock=");wlog_hex(a,k->lock_fn);wlog_raw(a," unlock=");wlog_hex(a,k->unlock_fn);
  wlog_raw(a," cmd=");wlog_hex(a,k->cmd_fn);wlog_raw(a," cache=");wlog_hex(a,k->cache_fn);wlog_raw(a," ctx=");wlog_hex(a,k->ctx_ptr);
  wlog_end(a,!k->valid||a->list_rc!=0);
 }
 if(k->valid&&wlog_begin(a,now,"koff_layout")){
  wlog_raw(a,"cache_off=");wlog_hex(a,k->cache_off);wlog_raw(a," cache_end=");wlog_hex(a,k->cache_end);wlog_raw(a," stride=");wlog_hex(a,k->cache_stride);
  wlog_raw(a," conn_dev=");wlog_hex(a,k->conn_dev_off);wlog_raw(a," st=");wlog_hex(a,k->conn_state_off);
  wlog_raw(a," pol=");wlog_hex(a,k->conn_policy_off);wlog_raw(a," hdl=");wlog_hex(a,k->conn_handle_off);
  wlog_end(a,0);
 }
}
static void message(struct app*a,const char*s){memset(a->message,0,sizeof(a->message));u64 n=length(s);if(n>127)n=127;memcpy(a->message,s,n);}
/* pre + device name (clipped to 40 bytes) + suf, always NUL-terminated within cap. */
static u64 compose(char*out,u64 cap,const char*pre,const char*name,const char*suf){
 u64 n=0;
 while(*pre&&n+1<cap)out[n++]=*pre++;
 for(u64 k=0;name[k]&&k<40&&n+1<cap;k++)out[n++]=name[k];
 while(*suf&&n+1<cap)out[n++]=*suf++;
 out[n]=0;return n;
}
static const char*target_name(struct app*a){return a->target.name[0]?a->target.name:"your headphones";}
static void say(struct app*a,const char*pre,const char*suf){char b[128];compose(b,sizeof(b),pre,target_name(a),suf);message(a,b);}
/* On-screen notices only on real transitions; a flapping link must not spam them. */
static int may_announce(struct app*a,int kind){u64 now=a->clock();if(a->notify_at[kind]&&now-a->notify_at[kind]<10000000)return 0;a->notify_at[kind]=now;return 1;}
static void mute(struct app*a,int on){
 if(a->hdmi<0||a->muted==on)return;int v=on;a->hdmi_rc=sc3(54,a->hdmi,0xc0048d05,(long)&v);
 if(!a->hdmi_rc)a->muted=on;
 long result[4]={a->hdmi,a->hdmi_rc,on,v};put("/data/bt_hdmi_mute_status.bin",result,sizeof(result));
 if(wlog_begin(a,a->clock(),a->hdmi_rc?"WARN mute":"mute")){wlog_raw(a,"on=");wlog_dec(a,on);wlog_raw(a," hdmi_rc=");wlog_dec(a,a->hdmi_rc);wlog_end(a,a->hdmi_rc!=0);}
}
static void stop_audio(struct app*a){
 int was_streaming=a->streaming;
 lock_state(a);
 a->audio.active=0;
 while(a->audio.in_flight)wait_for_audio_change(a,500);
 copy_audio_stats(a,&a->audio.stats);
 unlock_state(a);
 if(a->phase==4&&a->bt>=0){sc3(54,a->bt,0x20046205,0);a->scanning=0;}
 if(a->streaming){short silence[384];memset(silence,0,sizeof(silence));struct audio_req q={a->target.address,silence,sizeof(silence),0,0};sc3(54,a->bt,0x80206216,(long)&q);}
 a->streaming=0;a->phase=0;a->level=0;a->meter=0;a->meter_peak=0;a->pin_pending=0;a->test_until=0;a->volume_synced=0;a->avrcp_pending=0;a->avrcp_retries=0;a->audio.volume_synced=0;mute(a,0);
 if(a->ring){a->unmap(a->ring,0x10000);a->ring=0;}closefd(a->shmfd);a->shmfd=-1;
 if(was_streaming&&wlog_begin(a,a->clock(),"audio_stop")){wlog_raw(a,"packets=");wlog_dec(a,(long)a->packets);wlog_raw(a," resyncs=");wlog_dec(a,(long)a->resyncs);wlog_raw(a," relinks=");wlog_dec(a,(long)a->relinks_total);wlog_end(a,1);}
}
/* A failed attempt: stop, explain, and try again after delay_us. */
static void retry_after(struct app*a,const char*s,u64 delay_us){
 stop_audio(a);message(a,s);a->failed=1;a->next_try=a->clock()+delay_us;
 if(wlog_begin(a,a->clock(),"WARN retry")){wlog_raw(a,"delay_ms=");wlog_dec(a,(long)(delay_us/1000));wlog_raw(a," msg=");wlog_str(a,s);wlog_end(a,1);}
}
static void retry(struct app*a,const char*s){retry_after(a,s,10000000);}
static void retry_named(struct app*a,const char*pre,const char*suf,u64 delay_us){char m[128];compose(m,sizeof(m),pre,target_name(a),suf);retry_after(a,m,delay_us);}
/* The PS4 audio stream was rebuilt (e.g. a game started). The headphone link is
 * fine, so this is a quiet reconnect of the audio path, not a failure or a drop. */
static void restart_audio(struct app*a,const char*s){
 stop_audio(a);message(a,s);a->next_try=a->clock()+2000000;
 if(wlog_begin(a,a->clock(),"restart_audio")){wlog_raw(a,"msg=");wlog_str(a,s);wlog_end(a,1);}
}
/* The headphones went away mid-stream. Detected only by consecutive audio-send
 * failures (never by polling the driver while streaming: that starved the
 * controller). stop_audio restores TV sound. */
/* Drop the Bluetooth link itself, not just the audio. The driver keeps per-link stream
 * state (e.g. a stuck format renegotiation) that survives an audio stop. */
static void drop_link(struct app*a){if(a->bt>=0&&a->target.address){u64 address=a->target.address;sc3(54,a->bt,0x8008620b,(long)&address);}}
/* Automatic recovery: the PS4 refused every packet for REFUSED_US. Do what a user would
 * do by hand: drop the link and reconnect. At most 3 times in a row; any accepted packet
 * resets the count. */
#define REFUSED_US 4000000
static void relink(struct app*a){
 lock_state(a);
 unsigned attempt=++a->audio.relinks;
 a->audio.stats.refused_since=0;
 unlock_state(a);
 a->relinks_total++;
 if(wlog_begin(a,a->clock(),"WARN relink")){wlog_raw(a,"attempt=");wlog_dec(a,(long)attempt);wlog_raw(a," total=");wlog_dec(a,(long)a->relinks_total);wlog_end(a,1);}
 stop_audio(a);drop_link(a);a->phase=0;a->next_try=a->clock()+3000000;
 say(a,"The PS4 stopped accepting audio for ",". Reconnecting...");
}
static void link_lost(struct app*a){
 if(wlog_begin(a,a->clock(),"ERR link_lost")){wlog_raw(a,"errors=");wlog_dec(a,(long)a->errors);wlog_raw(a," last_rc=");wlog_dec(a,a->last_rc);wlog_end(a,1);}
 stop_audio(a);a->dropped=1;a->failed=0;a->next_try=a->clock()+2000000;
 say(a,""," disconnected. TV sound is back. Reconnecting...");
 if(may_announce(a,1)){notify("Bluetooth headphones disconnected. TV sound is back on. Reconnecting...");a->announced=1;}
}
static struct bt_device*device(struct app*a,u64 address){for(unsigned i=0;i<a->devices.count;i++)if(a->devices.items[i].address==address)return &a->devices.items[i];return 0;}
static int ensure_bt(struct app*a){if(a->bt>=0)return 0;a->bt=sc3(5,(long)"/dev/bt",6,0);if(a->bt<0)return -1;u64 mask=0xffffff;sc3(4,a->bt,(long)&mask,8);return 0;}
static void refresh_devices(struct app*a){
 struct bt_devices d;memset(&d,0,sizeof(d));
 int rc=bt_list(&d);
 a->list_rc=rc;
 if(!rc&&d.count<=BT_DEVICE_LIMIT)a->devices=d;
 wlog_koff(a,a->clock(),rc!=0);
 struct bt_device*t=device(a,a->target.address);if(t)a->target=*t;
}
static void end_scan(struct app*a){
 if(a->scanning&&a->bt>=0){
  sc3(54,a->bt,0x20046205,0);
  if(wlog_begin(a,a->clock(),"scan_end")){wlog_raw(a,"devices=");wlog_dec(a,(long)a->devices.count);wlog_end(a,0);}
 }
 a->scanning=0;
}
static int scan(struct app*a){
 if(ensure_bt(a)){if(wlog_begin(a,a->clock(),"ERR scan")){wlog_raw(a,"ensure_bt=-1");wlog_end(a,1);}return -1;}
 long rc=sc3(54,a->bt,0x20046204,0);
 if(rc<0){if(wlog_begin(a,a->clock(),"ERR scan")){wlog_raw(a,"ioctl_rc=");wlog_dec(a,rc);wlog_end(a,1);}return -1;}
 a->scanning=1;a->scan_end=a->clock()+16000000;a->list_at=0;a->poll_at=0;
 if(wlog_begin(a,a->clock(),"scan_start")){wlog_raw(a,"ok");wlog_end(a,0);}
 return 0;
}
struct saved_device{u64 magic;struct bt_device device;};
static int save_target(struct app*a,const struct bt_device*d){
 struct saved_device s={.magic=0x3254454752415442UL,.device=*d};
 if(put("/data/bta_audio/device.tmp",&s,sizeof(s)))return -1;
 if(sc3(128,(long)"/data/bta_audio/device.tmp",(long)"/data/bta_audio/device.bin",0)<0)return -1;
 a->target=*d;return 0;
}
static void load_target(struct app*a){
 a->target.address=0;a->target.services=0;
 long fd=sc3(5,(long)"/data/bta_audio/device.bin",0,0);if(fd<0)return;
 struct saved_device s;long n=sc3(3,fd,(long)&s,sizeof(s));closefd(fd);
 if(n==sizeof(s)&&s.magic==0x3254454752415442UL&&s.device.address&&!(s.device.address>>48)){s.device.name[95]=0;a->target=s.device;}
}
static int save_volume(struct app*a,int volume){
 if(volume<0||volume>100)return -1;unsigned value=(unsigned)volume;
 if(put("/data/bta_audio/volume.tmp",&value,sizeof(value)))return -1;
 if(sc3(128,(long)"/data/bta_audio/volume.tmp",(long)"/data/bta_audio/volume.bin",0)<0)return -1;
 a->volume=volume;return 0;
}
static void load_volume(struct app*a){
 a->volume=40;unsigned value=101;long fd=sc3(5,(long)"/data/bta_audio/volume.bin",0,0);
 if(fd<0)return;long n=sc3(3,fd,(long)&value,sizeof(value));closefd(fd);
 if(n==sizeof(value)&&value<=100)a->volume=(int)value;
}
static int avrcp_to_pct(int raw){if(raw<=0)return 0;if(raw>=127)return 100;return (raw*100+63)/127;}
static int pct_to_avrcp(int pct){if(pct<=0)return 0;if(pct>=100)return 127;return (pct*127+50)/100;}
/* AVRCP SetAbsoluteVolume (0x50) via /dev/bt ioctl 0x80106212. Non-blocking;
 * queues behind L2CAP PSM 0x17 open if needed (returns 0x802f0605 if one is already pending). */
static void avrcp_set_volume(struct app*a,int pct){
 if(a->bt<0||!a->target.address)return;
 if(!a->streaming&&(a->btstate<5||a->btstate>6))return;
 struct{u64 address;u8 volume;u8 pad[7];}q={a->target.address,(u8)pct_to_avrcp(pct),{0}};
 long rc=sc3(54,a->bt,0x80106212,(long)&q);
 if(rc==0){a->avrcp_pending=0;a->avrcp_retries=0;}
 else if((u32)rc==0x802f0605U){a->avrcp_pending=1;a->avrcp_retry_at=a->clock()+250000;}
 else{a->avrcp_pending=0;a->avrcp_retries=0;}
 if(wlog_begin(a,a->clock(),rc?"WARN avrcp_set":"avrcp_set")){
  wlog_raw(a,"vol=");wlog_dec(a,pct);wlog_raw(a," raw=");wlog_dec(a,(long)q.volume);
  wlog_raw(a," rc=");wlog_hex(a,(u64)(u32)rc);wlog_end(a,rc!=0);
 }
}
static void avrcp_query_volume(struct app*a){
 if(a->bt<0||!a->target.address)return;
 u64 address=a->target.address;
 long rc=sc3(54,a->bt,0x80086213,(long)&address);
 if(rc>=0&&rc<=127){
  int pct=avrcp_to_pct((int)rc);
  a->volume_synced=1;
  if(pct!=a->volume)save_volume(a,pct);
  if(wlog_begin(a,a->clock(),"avrcp_init")){wlog_raw(a,"raw=");wlog_dec(a,rc);wlog_raw(a," vol=");wlog_dec(a,a->volume);wlog_end(a,0);}
 }
}
static void wlog_patch(struct app*a,u64 now){
 if(a->patch_logged&&!a->patch_rc)return;
 a->patch_logged=1;
 struct{long status;u64 before,after;}pr={0,0,0};
 long fd=sc3(5,(long)"/data/bt_accept_patch_result.bin",0,0);
 if(fd>=0){sc3(3,fd,(long)&pr,sizeof(pr));closefd(fd);}
 if(wlog_begin(a,now,a->patch_rc?"ERR patch":"patch")){
  wlog_raw(a,"rc=");wlog_dec(a,a->patch_rc);wlog_raw(a," status=");wlog_dec(a,pr.status);
  wlog_raw(a," before=");wlog_hex(a,pr.before);wlog_raw(a," after=");wlog_hex(a,pr.after);
  wlog_end(a,a->patch_rc!=0);
 }
}
static void begin_connect(struct app*a){
 u64 now=a->clock();
 a->patch_rc=bta_apply_patch();
 wlog_patch(a,now);
 if(a->patch_rc){retry(a,"Bluetooth setup failed. Check the diagnostic status.");return;}
 if(ensure_bt(a)){retry(a,"Bluetooth is not ready. Retrying shortly.");return;}
 refresh_devices(a);struct bt_device*t=device(a,a->target.address);
 if(!t){if(scan(a)){retry(a,"Bluetooth scan could not start. Retrying shortly.");return;}a->phase=4;a->deadline=a->clock()+16000000;say(a,"Looking for ","... Put it in pairing mode if needed.");return;}
 if(!bt_is_audio(t)){
  if(wlog_begin(a,now,"WARN not_audio")){wlog_raw(a,"mac=");wlog_mac(a,t->address);wlog_raw(a," cod=");wlog_hex(a,t->cod);wlog_raw(a," svc=");wlog_hex(a,t->services);wlog_end(a,1);}
  a->wanted=0;retry(a,"This device does not advertise headphone audio support.");return;
 }
 end_scan(a);u64 address=a->target.address;a->btstate=sc3(54,a->bt,0x80086203,(long)&address);
 a->phase=1;a->deadline=a->clock()+30000000;say(a,"Connecting to ","...");
 if(a->btstate<5||a->btstate>6){sc3(54,a->bt,0x20046205,0);a->connect_rc=sc3(54,a->bt,0x8008620a,(long)&address);
  if(wlog_begin(a,now,a->connect_rc<0?"ERR connect":"connect")){
   wlog_raw(a,"attempt=");wlog_dec(a,(long)a->attempts);wlog_raw(a," mac=");wlog_mac(a,address);
   wlog_raw(a," btstate=");wlog_dec(a,a->btstate);wlog_raw(a," connect_rc=");wlog_dec(a,a->connect_rc);wlog_end(a,a->connect_rc<0);
  }
  /* An immediate driver error (e.g. 0x802F0204) means this device will not
   * connect right now; fail fast instead of waiting out the 30 s window. */
  if(a->connect_rc<0){retry_named(a,"Couldn't connect to ",". Make sure it's on, nearby and not connected to another device.",8000000);return;}
 }else{
  a->connect_rc=0;
  if(wlog_begin(a,now,"connect")){wlog_raw(a,"already_linked mac=");wlog_mac(a,address);wlog_raw(a," btstate=");wlog_dec(a,a->btstate);wlog_end(a,0);}
 }
}
static void start_audio(struct app*a){
 a->shmfd=a->shm("/shm_acap.full",0,0);if(a->shmfd<0){retry(a,"Waiting for the PS4 audio service.");return;}
 void*p=a->map(0,0x10000,1,1,a->shmfd,0);
 if(!p||p==(void*)-1){retry(a,"Unable to open the game audio stream.");return;}a->ring=p;
 u32 readpos=*(volatile u32*)(a->ring+12);
 if(*(volatile u32*)a->ring!=0x10000||readpos<16||readpos>=0x10000||(readpos&7)){
  if(wlog_begin(a,a->clock(),"ERR shm_format")){wlog_raw(a,"hdr=");wlog_hex(a,*(volatile u32*)a->ring);wlog_raw(a," readpos=");wlog_hex(a,readpos);wlog_end(a,1);}
  retry(a,"The PS4 audio stream has an unexpected format.");return;
 }
 a->streaming=1;a->phase=3;a->refused_since=0;a->errors=0;a->stream_at=a->clock();a->test_until=0;
 if(!a->sec_start)a->sec_start=a->stream_at;
 a->meter_at=a->stream_at;
 a->health_at=a->stream_at+10000000;a->health_resyncs=a->resyncs;a->health_ok_skips=0;memset(&a->health,0,sizeof(a->health));
 a->bp_check_at=a->stream_at+2000000;a->bp_adapt_at=a->stream_at;a->bp_stable_at=a->stream_at;
 a->bp_last_drops=0;a->bp_health_drops=0;a->bp_ten_drops=0;memset(a->bp_win,0,sizeof(a->bp_win));a->bp_win_idx=0;
 a->bp_target=0;a->bp_base=0;a->bp_cur=0;a->bp_inflight=0;
 int was_dropped=a->dropped,told=a->announced,fresh=!a->session;
 a->attempts=0;a->failed=0;a->dropped=0;a->announced=0;a->session=1;a->ok_at=a->stream_at;a->meter=0;a->meter_peak=0;
 if(wlog_begin(a,a->stream_at,"audio_start")){
  unsigned peers=0,hid=0;
  for(unsigned i=0;i<a->devices.count;i++){
   const struct bt_device*d=&a->devices.items[i];
   if(d->address==a->target.address)continue;
   peers++;
   if((d->services&0x40)||(((d->cod>>8)&31)==5))hid++;
  }
  wlog_raw(a,"mac=");wlog_mac(a,a->target.address);wlog_raw(a," name=");wlog_str(a,target_name(a));
  wlog_raw(a," joint=");wlog_dec(a,a->joint);wlog_raw(a," tv=");wlog_dec(a,a->tv);
  wlog_raw(a," vol=");wlog_dec(a,a->volume);wlog_raw(a," readpos=");wlog_dec(a,(long)readpos);
  wlog_raw(a," peers=");wlog_dec(a,(long)peers);wlog_raw(a," hid=");wlog_dec(a,(long)hid);wlog_end(a,1);
 }
 say(a,"Connected to ",a->tv?". TV sound is on.":". TV sound is muted.");
 avrcp_query_volume(a);
 lock_state(a);
 struct audio_stream*s=&a->audio;
 s->ring=a->ring;s->bt=a->bt;s->address=a->target.address;s->readpos=readpos;
 s->volume=a->volume;s->volume_synced=a->volume_synced;s->joint=a->joint;s->test_until=s->test_next=s->last_send=0;s->test_phase=0;
 s->stats.errors=s->stats.refused_since=s->stats.level=s->stats.meter_peak=0;s->stats.other_rc=0;s->stats.ok_at=a->stream_at;
 s->fault=AUDIO_OK;s->resync_backlog=0;s->refused_warned=s->err_warned=s->relink_limit=0;
 s->refused_ms=0;s->error_count=0;s->has_pending=0;s->was_testing=0;s->pending_peak=0;s->active=1;
 wake_audio(a);
 unlock_state(a);
 if(was_dropped){if(told&&may_announce(a,2))notify("Bluetooth headphones reconnected.");}
 else if(fresh&&may_announce(a,0)){char m[200];compose(m,sizeof(m),"Bluetooth headphones connected: ",target_name(a),a->tv?". TV sound stays on.":". TV sound muted.");notify(m);}
}
static void bluetooth_tick(struct app*a,u64 now){
 /* While audio is streaming, do NOT walk the driver cache: refresh_devices()
  * takes the kernel's Bluetooth link lock, and polling it mid-stream starves
  * the wireless controller's HID link. The proven BTA bridge never did this.
  * Device enumeration resumes as soon as streaming stops. */
 if(!a->streaming&&now>=a->list_at){refresh_devices(a);a->list_at=now+(a->scanning?500000:3000000);}
 if(a->scanning&&now>=a->scan_end)end_scan(a);
 if(a->bt>=0){
  struct{u8 type,subtype;u16 reserved;u32 data;u64 address;}ev[16];long n=sc3(3,a->bt,(long)ev,sizeof(ev));
  if(n>0&&n<=(long)sizeof(ev))for(unsigned i=0;i<(unsigned)n/16;i++){
   int is_target=(ev[i].address==a->target.address);
   int dup=(ev[i].type==a->bt_ev_last_type&&ev[i].subtype==a->bt_ev_last_sub&&ev[i].data==a->bt_ev_last_data&&ev[i].address==a->bt_ev_last_addr&&now-a->bt_ev_last_at<1000000);
   if(!dup){
    a->bt_ev_last_at=now;a->bt_ev_last_type=ev[i].type;a->bt_ev_last_sub=ev[i].subtype;a->bt_ev_last_data=ev[i].data;a->bt_ev_last_addr=ev[i].address;
    if(wlog_begin(a,now,"bt_ev")){
     wlog_raw(a,"type=");wlog_dec(a,ev[i].type);wlog_raw(a," sub=");wlog_dec(a,ev[i].subtype);
     wlog_raw(a," data=");wlog_hex(a,ev[i].data);wlog_raw(a," addr=");wlog_mac(a,ev[i].address);
     wlog_raw(a," target=");wlog_dec(a,is_target);wlog_end(a,is_target);
    }
   }
   if(is_target&&a->wanted){
    if(ev[i].type==4){u64 q[2]={a->target.address,1};sc3(54,a->bt,0x80106208,(long)q);}
    if(ev[i].type==3){a->pin_pending=1;a->deadline=now+60000000;message(a,"These headphones require a pairing PIN.");}
    if(ev[i].type==11&&ev[i].subtype<=2){
     int raw=(int)(ev[i].data&0x7f),pct=avrcp_to_pct(raw),was_synced=a->volume_synced;
     a->volume_synced=1;a->avrcp_pending=0;a->avrcp_retries=0;
     int accept=(ev[i].subtype!=2)||(pct_to_avrcp(a->volume)!=raw);
     if(accept&&pct!=a->volume)save_volume(a,pct);
     if(!was_synced||accept)sync_audio_controls(a);
     if(wlog_begin(a,now,"avrcp_vol")){
      wlog_raw(a,"sub=");wlog_dec(a,ev[i].subtype);wlog_raw(a," raw=");wlog_dec(a,raw);
      wlog_raw(a," vol=");wlog_dec(a,a->volume);wlog_end(a,0);
     }
    }
   }
  }
 }
 if(a->avrcp_pending&&a->bt>=0&&now>=a->avrcp_retry_at){
  if(a->avrcp_retries<8){a->avrcp_retries++;avrcp_set_volume(a,a->volume);}
  else{a->avrcp_pending=0;a->avrcp_retries=0;}
 }
 if(!a->wanted)return;
 if(a->phase==0){if(now>=a->next_try){a->attempts++;a->failed=0;begin_connect(a);}return;}
 if(a->phase==4){
  if(device(a,a->target.address)){end_scan(a);a->phase=0;begin_connect(a);return;}
  if(now>=a->deadline)retry_named(a,"Couldn't find ",". Put it in pairing mode.",8000000);return;
 }
 if(a->phase==1){
  u64 address=a->target.address;a->btstate=sc3(54,a->bt,0x80086203,(long)&address);
  if(a->btstate>=5&&a->btstate<=6){a->pin_pending=0;
   if(exists(NO_WIDEN)){
    a->packet_rc=0;/* diagnostic: leave controller link params untouched */
    if(wlog_begin(a,now,"WARN bt_prepare")){wlog_raw(a,"skipped=NO_WIDEN mac=");wlog_mac(a,address);wlog_raw(a," btstate=");wlog_dec(a,a->btstate);wlog_end(a,1);}
   }else{
    struct bt_prep_diag pd;memset(&pd,0,sizeof(pd));
    a->packet_rc=bt_prepare_diag(address,&pd);
    wlog_koff(a,now,a->packet_rc!=0);
    if(wlog_begin(a,now,a->packet_rc?"ERR bt_prepare":(pd.skipped?"WARN bt_prepare":"bt_prepare"))){
     wlog_raw(a,"rc=");wlog_dec(a,a->packet_rc);wlog_raw(a," skipped=");wlog_dec(a,pd.skipped);
     wlog_raw(a," btstate=");wlog_dec(a,a->btstate);wlog_raw(a," conn_state=");wlog_dec(a,(long)pd.conn_state);
     wlog_raw(a," policy=");wlog_hex(a,pd.conn_policy);wlog_raw(a," hdl=");wlog_hex(a,pd.handle);
     wlog_raw(a," svc=");wlog_hex(a,pd.dev_services);wlog_raw(a," lps_rc=");wlog_dec(a,pd.lps_rc);
     wlog_raw(a," role_rc=");wlog_dec(a,pd.role_rc);wlog_end(a,a->packet_rc!=0||pd.skipped);
    }
    if(a->packet_rc){retry(a,"No supported Bluetooth audio link. Retrying shortly.");return;}
   }
   a->phase=2;a->deadline=now+400000;}
  else if(now>=a->deadline)retry_named(a,""," didn't respond. Is it on and nearby?",8000000);
 }else if(a->phase==2&&now>=a->deadline)start_audio(a);
}
__attribute__((noinline)) static void wlog_stream_summary(struct app*a,u64 now,const char*tag,const struct second*s,u32 resync_d,int urgent){
 if(wlog_begin(a,now,tag)){
  wlog_raw(a,"pkts=");wlog_dec(a,(long)s->pkts);wlog_raw(a," refused=");wlog_dec(a,(long)s->refused);
  wlog_raw(a," failed=");wlog_dec(a,(long)s->failed);wlog_raw(a," resyncs=+");wlog_dec(a,(long)resync_d);
  wlog_raw(a,"(");wlog_dec(a,(long)a->resyncs);wlog_raw(a,") late8=");wlog_dec(a,(long)s->late8);
  wlog_raw(a," late16=");wlog_dec(a,(long)s->late16);wlog_raw(a," late50=");wlog_dec(a,(long)s->late50);
  wlog_raw(a," maxgap_us=");wlog_dec(a,(long)s->maxgap_us);wlog_raw(a," maxsend_us=");wlog_dec(a,(long)s->maxsend_us);
  wlog_raw(a," maxback_ms=");wlog_dec(a,(long)s->maxback_ms);wlog_raw(a," audio_rc=");wlog_dec(a,a->last_rc);
  wlog_raw(a," rc768=");wlog_dec(a,(long)s->rc_full);wlog_raw(a," rc1=");wlog_dec(a,(long)s->rc_one);
  if(s->rc_other){wlog_raw(a," rc_oth=");wlog_dec(a,(long)s->rc_other);wlog_raw(a," last_oth=");wlog_dec(a,a->other_rc);}
  if(s->maxref_ms){wlog_raw(a," maxref_ms=");wlog_dec(a,(long)s->maxref_ms);}
  if(a->bp_cur){wlog_raw(a," bp=");wlog_dec(a,a->bp_cur);}
  wlog_raw(a," kdrops=");wlog_dec(a,(long)a->bp_health_drops);
  if(urgent&&a->bp_health_drops>=25)wlog_raw(a," [BT_KERNEL_DROP]");
  if(s->refused)wlog_raw(a," [BT_QUEUE_REFUSED]");
  if(resync_d)wlog_raw(a," [RING_OVERFLOW_DROP]");
  if(s->late50||s->late16)wlog_raw(a," [SENDER_STALLED]");
  wlog_end(a,urgent);
 }
}
static void roll_second(struct app*a,u64 now){
 if(a->sec_start&&now-a->sec_start<1000000)return;
 if(a->sec_start&&(a->cur.pkts||a->cur.maxgap_us)){
  a->cur.seq=++a->sec_seq;a->secs[a->sec_seq%128]=a->cur;
  a->health.pkts+=a->cur.pkts;a->health.refused+=a->cur.refused;a->health.failed+=a->cur.failed;
  a->health.late8+=a->cur.late8;a->health.late16+=a->cur.late16;a->health.late50+=a->cur.late50;
  a->health.rc_full+=a->cur.rc_full;a->health.rc_one+=a->cur.rc_one;a->health.rc_other+=a->cur.rc_other;
  if(a->cur.maxref_ms>a->health.maxref_ms)a->health.maxref_ms=a->cur.maxref_ms;
  if(a->cur.maxgap_us>a->health.maxgap_us)a->health.maxgap_us=a->cur.maxgap_us;
  if(a->cur.maxsend_us>a->health.maxsend_us)a->health.maxsend_us=a->cur.maxsend_us;
  if(a->cur.maxback_ms>a->health.maxback_ms)a->health.maxback_ms=a->cur.maxback_ms;
  if(a->cur.peak>a->health.peak)a->health.peak=a->cur.peak;
  if((a->cur.refused||a->cur.failed||a->cur.late50)&&now>=a->health_warn_at){
   a->health_warn_at=now+5000000;
   wlog_stream_summary(a,now,"WARN stream_1s",&a->cur,0,1);
  }
 }
 if(a->health_at&&now>=a->health_at){
  u32 rd=a->resyncs-a->health_resyncs;
  int bad=(a->health.refused||a->health.failed||rd||a->health.late16||a->health.late50||a->bp_ten_drops>=25);
  a->bp_ten_drops=0;
  if(bad){
   wlog_stream_summary(a,now,"WARN stream_10s",&a->health,rd,1);
   a->health_ok_skips=0;
  }else if(++a->health_ok_skips>=3){
   wlog_stream_summary(a,now,"OK stream_30s",&a->health,0,0);
   a->health_ok_skips=0;
  }
  if(bad||a->health_ok_skips==0){memset(&a->health,0,sizeof(a->health));a->health_resyncs=a->resyncs;a->bp_health_drops=0;}
  a->health_at=now+10000000;
 }
 memset(&a->cur,0,sizeof(a->cur));a->cur.t=(u32)(now/1000000);a->sec_start=now;
}
/* Joint stereo is the default. A headphone can opt out (old dual-channel format) with an
 * empty file /data/bta_audio/dual-<address>.
 * Send type 3 makes the driver encode dual-channel SBC at half bitpool per channel, a
 * format headphones did not agree to at connect, so it renegotiates the stream. Type 4
 * is joint stereo at full bitpool, the format headphones normally agree to. */
static void joint_path(u64 address,char out[40]){
 const char*hex="0123456789ABCDEF",*base="/data/bta_audio/dual-";unsigned i=0;
 while(base[i]){out[i]=base[i];i++;}
 for(int k=0;k<12;k++)out[i++]=hex[(address>>(44-4*k))&15];
 out[i]=0;
}
static void load_joint(struct app*a){char p[40];joint_path(a->target.address,p);a->joint=!(a->target.address&&exists(p));}
static u32 ring_back_cursor(u32 writepos,u32 bytes){
 u32 off=writepos-16;
 off=(off>=bytes)?(off-bytes):(off+0xfff0U-bytes);
 return 16+off;
}
/* Called with the stream mutex held. The driver submission can block, so
 * release it across the ioctl while keeping the current stream pinned. */
static unsigned audio_tick(struct app*a,u64 now){
 struct audio_stream*s=&a->audio;
 struct audio_stats*st=&s->stats;
 short pcm[384];unsigned peak=0;unsigned backlog=0;unsigned rem=0;
 if(now<s->test_until){
  /* Self-paced test tone (works even with no game audio): one 192-frame packet
   * every ~4ms. Independent of the capture ring; follows the volume slider
   * (50% = the old fixed level, 100% = about half of full scale). */
  s->was_testing=1;s->has_pending=0;
  if(now<s->test_next){
   u64 due=s->test_next<s->test_until?s->test_next:s->test_until;
   return (unsigned)(due-now);
  }
  s->test_next=now+4000;
  int eff_vol=s->volume_synced?(s->volume>0?100:0):s->volume;
  for(unsigned i=0;i<192;i++){
   short v=(short)(SINE32[(s->test_phase>>8)&31]*2*eff_vol/100);
   s->test_phase+=75;pcm[2*i]=v;pcm[2*i+1]=v;
   unsigned m=v<0?-v:v;if(m>peak)peak=m;
  }
 }else{
  u32 writepos=(u32)(*(const volatile u64*)(s->ring+8)>>32);
  if(writepos<16||writepos>=0x10000||(writepos&7)){
   s->bad_writepos=writepos;s->fault=AUDIO_RING_RESET;return 0;
  }
  /* Resync after test tone so 2s of unconsumed game audio does not overflow. */
  if(s->was_testing){s->was_testing=0;s->has_pending=0;s->readpos=writepos;}
  unsigned available=writepos>=s->readpos?writepos-s->readpos:0xfff0-s->readpos+writepos;
  if(available>0xc000){
   s->readpos=ring_back_cursor(writepos,6144U);s->has_pending=0;st->resyncs++;s->resync_backlog=available;
   return 0;
  }
  if(s->has_pending){
   /* Retry the packet refused by the driver instead of dropping 4ms of audio. */
   memcpy(pcm,s->pending_pcm,sizeof(pcm));
   peak=s->pending_peak;
   backlog=available+1536;
   rem=available;
  }else{
   if(available<1536){
    /* Use the 48 kHz stereo float rate to estimate the next block. Cap the
     * wait because the capture producer can publish several frames at once. */
    unsigned wait=(1536-available)*1000/384;
    return wait<500?500:wait>2000?2000:wait;
   }
   backlog=available;
   rem=available-1536;
   u32 cursor=s->readpos;
   int eff_vol=s->volume_synced?(s->volume>0?100:0):s->volume;
   float gain=32767.0f*(float)eff_vol/100.0f;
   for(unsigned i=0;i<384;i++){
    float f=*(const volatile float*)(s->ring+cursor);
    cursor+=4;if(cursor==0x10000)cursor=16;
    if(f!=f)f=0;if(f>1)f=1;if(f<-1)f=-1;
    int v=(int)(f*gain);pcm[i]=(short)v;
    unsigned m=v<0?-v:v;if(m>peak)peak=m;
   }
   s->readpos=cursor;
   memcpy(s->pending_pcm,pcm,sizeof(pcm));
   s->pending_peak=peak;
   s->has_pending=1;
  }
 }
 struct audio_req req={s->address,pcm,sizeof(pcm),s->joint?4:3,0};
 long bt=s->bt;
 s->in_flight=1;
 unlock_state(a);
 u64 send_at=a->clock();
 long rc=sc3(54,bt,0x80206216,(long)&req);
 u64 done=a->clock();
 lock_state(a);
 struct second*cur=&s->cur;
 st->last_rc=rc;cur->pkts++;
 if(rc==0)cur->refused++;else if(rc<0)cur->failed++;
 if(s->last_send){
  u64 gap=send_at-s->last_send;
  if(gap>cur->maxgap_us)cur->maxgap_us=(u32)(gap>0xffffffffUL?0xffffffffUL:gap);
  if(gap>8000)cur->late8++;if(gap>16000)cur->late16++;if(gap>50000)cur->late50++;
 }
 if(done-send_at>cur->maxsend_us)cur->maxsend_us=(u32)(done-send_at);
 if(backlog/384>cur->maxback_ms)cur->maxback_ms=backlog/384;
 if(peak>cur->peak)cur->peak=peak;
 if(peak>st->level)st->level=peak;
 s->last_send=send_at;
 if(rc==0){
  if(!st->refused_since)st->refused_since=send_at;
  u64 elapsed=send_at-st->refused_since;
  u32 ref_ms=(u32)(elapsed/1000);
  if(ref_ms>cur->maxref_ms)cur->maxref_ms=ref_ms;
  if(elapsed>100000&&!s->refused_warned){
   s->refused_warned=1;s->refused_ms=(unsigned)ref_ms;s->refused_backlog=backlog/384;
  }else if(elapsed>REFUSED_US&&s->relinks<3)s->fault=AUDIO_RELINK;
  else if(elapsed>REFUSED_US&&s->relinks==3){s->relinks++;s->relink_limit=1;}
 }else if(rc>0){
  s->has_pending=0;
  if(rc==(long)sizeof(pcm))cur->rc_full++;
  else if(rc==1)cur->rc_one++;
  else{cur->rc_other++;st->other_rc=rc;}
  st->refused_since=0;s->relinks=0;s->refused_warned=0;s->err_warned=0;
 }
 if(s->fault!=AUDIO_RELINK){
  if(rc<0){
   s->has_pending=0;
   if(!s->err_warned){s->err_warned=1;s->error_rc=rc;s->error_count=st->errors+1;}
   if(++st->errors>20)s->fault=AUDIO_LINK_LOST;
  }else if(rc>0){
   st->errors=0;st->packets++;st->ok_at=send_at;
   if(peak>st->meter_peak)st->meter_peak=peak;
  }else{
   st->errors=0;
  }
 }
 s->in_flight=0;
 if(!s->active)wake_audio(a);
 if(s->fault!=AUDIO_OK)return 0;
 if(now<s->test_until&&done<s->test_next&&done<s->test_until){
  u64 due=s->test_next<s->test_until?s->test_next:s->test_until;
  return (unsigned)(due-done);
 }
 if(rc==0)return 1500;
 if(rem>=4608)return 1500;
 if(rem>=1536)return 2500;
 unsigned wait=(1536-rem)*1000/384;
 return wait<500?500:wait>2000?2000:wait;
}

static void tune_bitpool(struct app*a,u64 now){
 if(!a->streaming||a->no_widen||now<a->bp_check_at)return;
 a->bp_check_at=now+250000;
 struct bt_stream_diag sd;memset(&sd,0,sizeof(sd));
 if(bt_stream_tune(a->target.address,0,&sd))return;
 int eff=(sd.enc_bp>0)?sd.enc_bp:sd.bitpool;
 if(!a->bp_base){
  a->bp_base=(sd.bitpool>0)?sd.bitpool:51;
  a->bp_last_drops=sd.dropped;
  a->bp_cur=eff;a->bp_inflight=sd.inflight;
  a->bp_stable_at=now;
  return;
 }
 /* Keep adapted bitpool synced even if kernel stream re-inits 0x43d0 mid-stream. */
 if(a->bp_target>0&&(sd.bitpool!=a->bp_target||(sd.enc_bp>0&&sd.enc_bp!=a->bp_target))){
  if(!bt_stream_tune(a->target.address,a->bp_target,&sd))
   eff=(sd.enc_bp>0)?sd.enc_bp:sd.bitpool;
 }
 a->bp_cur=eff;a->bp_inflight=sd.inflight;
 u32 d=(sd.dropped>=a->bp_last_drops)?(sd.dropped-a->bp_last_drops):sd.dropped;
 a->bp_last_drops=sd.dropped;
 a->bp_health_drops+=d;a->bp_ten_drops+=d;
 a->bp_win[a->bp_win_idx]=d;
 a->bp_win_idx=(u8)((a->bp_win_idx+1)&7);
 u32 sum_2s=0;
 for(int i=0;i<8;i++)sum_2s+=a->bp_win[i];
 int base=a->bp_base?a->bp_base:51;
 int lo=(sd.min_bp>=2&&sd.min_bp<=53)?sd.min_bp:2;
 if(lo<18)lo=18;
 /* Normal 48 kHz clock drift drops ~1 pkt/s; only react to real congestion bursts. */
 if(d>=3||sum_2s>=6)a->bp_stable_at=now;
 if((d>=5||sum_2s>=8)&&now>=a->bp_adapt_at){
  int cur=a->bp_target?a->bp_target:(eff>0?eff:base);
  int step=(d>=10||sum_2s>=16)?10:6;
  int next=cur-step;
  if(next<lo)next=lo;
  if(next<cur&&!bt_stream_tune(a->target.address,next,&sd)){
   a->bp_target=sd.bitpool;
   a->bp_cur=(sd.enc_bp>0)?sd.enc_bp:sd.bitpool;
   a->bp_adapt_at=now+1500000;
   memset(a->bp_win,0,sizeof(a->bp_win));
   if(wlog_begin(a,now,"WARN bp_adapt")){
    wlog_raw(a,"dir=down d=");wlog_dec(a,(long)d);wlog_raw(a," sum2s=");wlog_dec(a,(long)sum_2s);
    wlog_raw(a," prev=");wlog_dec(a,cur);wlog_raw(a," bp=");wlog_dec(a,a->bp_cur);
    wlog_raw(a," min=");wlog_dec(a,sd.min_bp);wlog_raw(a," max=");wlog_dec(a,sd.max_bp);
    wlog_raw(a," inf=");wlog_dec(a,sd.inflight);wlog_end(a,1);
   }
  }
 }else if(a->bp_target>0&&a->bp_target<base&&now-a->bp_stable_at>=20000000&&now>=a->bp_adapt_at){
  int cur=a->bp_target;
  int next=cur+4;
  if(next>base)next=base;
  if(!bt_stream_tune(a->target.address,next,&sd)){
   a->bp_target=(next>=base)?0:sd.bitpool;
   a->bp_cur=(sd.enc_bp>0)?sd.enc_bp:sd.bitpool;
   a->bp_stable_at=now;a->bp_adapt_at=now+5000000;
   if(wlog_begin(a,now,"bp_adapt")){
    wlog_raw(a,"dir=up prev=");wlog_dec(a,cur);wlog_raw(a," bp=");wlog_dec(a,a->bp_cur);
    wlog_raw(a," base=");wlog_dec(a,base);wlog_end(a,0);
   }
  }
 }
}
static void collect_audio(struct app*a,u64 now,int beat){
 if(!a->streaming)return;
 int meter_due=now>=a->meter_at;
 int second_due=!a->sec_start||now-a->sec_start>=1000000;
 u64 previous_ok=a->ok_at;
 lock_state(a);
 struct audio_stream*s=&a->audio;
 copy_audio_stats(a,&s->stats);
 if(beat)s->stats.level=0;
 if(meter_due)s->stats.meter_peak=0;
 if(second_due){a->cur=s->cur;a->cur.t=(u32)(a->sec_start/1000000);memset(&s->cur,0,sizeof(s->cur));}
 enum audio_fault fault=s->fault;
 u32 writepos=s->bad_writepos,readpos=s->readpos,backlog=s->resync_backlog;
 unsigned refused_ms=s->refused_ms,refused_backlog=s->refused_backlog;
 long error_rc=s->error_rc;u64 error_count=s->error_count;
 int limit=s->relink_limit;
 s->resync_backlog=0;s->refused_ms=0;s->error_count=0;s->relink_limit=0;
 unlock_state(a);
 tune_bitpool(a,now);
 if(second_due)roll_second(a,now);
 if(meter_due){
  unsigned p=a->meter_peak*100/32767;if(p>100)p=100;
  unsigned hold=(unsigned)a->meter*7/10;
  a->meter=(int)(p>hold?p:hold);a->meter_at=now+80000;
 }
 if(backlog&&now>=a->resync_warn_at){
  a->resync_warn_at=now+5000000;
  if(wlog_begin(a,now,"WARN resync")){wlog_raw(a,"avail=");wlog_dec(a,backlog);wlog_raw(a," total=");wlog_dec(a,(long)a->resyncs);wlog_end(a,1);}
 }
 if(refused_ms&&wlog_begin(a,now,"WARN refused_streak")){wlog_raw(a,"ms=");wlog_dec(a,refused_ms);wlog_raw(a," back_ms=");wlog_dec(a,refused_backlog);wlog_end(a,1);}
 if(error_count&&wlog_begin(a,now,"ERR audio_ioctl")){wlog_raw(a,"rc=");wlog_dec(a,error_rc);wlog_raw(a," errors=");wlog_dec(a,(long)error_count);wlog_end(a,1);}
 if(limit){
  if(wlog_begin(a,now,"ERR relink_limit")){wlog_raw(a,"relinks=3");wlog_end(a,1);}
  say(a,"The PS4 keeps refusing audio for ",". Turn the headphones off and on.");
 }
 if(fault==AUDIO_RING_RESET){
  if(wlog_begin(a,now,"WARN ring_reset")){wlog_raw(a,"writepos=");wlog_hex(a,writepos);wlog_raw(a," readpos=");wlog_hex(a,readpos);wlog_end(a,1);}
  restart_audio(a,"Audio stream changed. Reconnecting...");
 }else if(fault==AUDIO_RELINK)relink(a);
 else if(fault==AUDIO_LINK_LOST)link_lost(a);
 else if(a->ok_at!=previous_ok){
  /* Use a successful submission at or after the priming deadline to mute. */
  int want=!a->tv&&a->ok_at-a->stream_at>=PRIME_US;
  if(a->muted!=want)mute(a,want);
 }
}
/* Bounded JSON writer. Values are numbers and fixed internal strings only. */
struct writer{char*b;unsigned used,cap;};
static void append(struct writer*w,const char*s){while(*s&&w->used+1<w->cap)w->b[w->used++]=*s++;w->b[w->used]=0;}
static void number(struct writer*w,long n){char b[24];unsigned i=0;u64 v;if(n<0){append(w,"-");v=0-(u64)n;}else v=n;do{b[i++]=(char)('0'+v%10);v/=10;}while(v);while(i){char s[2]={b[--i],0};append(w,s);}}
static void field(struct writer*w,const char*key,long value){append(w,",\"");append(w,key);append(w,"\":");number(w,value);}
static void quoted(struct writer*w,const char*s){append(w,"\"");for(unsigned i=0;s[i]&&i<127;i++){char c=s[i];if(c=='"'||c=='\\')append(w,"\\");char b[2]={(c>=32&&c<127)?c:'?',0};append(w,b);}append(w,"\"");}
static void mac_text(u64 address,char out[13]){const char digits[]="0123456789ABCDEF";for(int i=0;i<12;i++)out[i]=digits[(address>>(44-4*i))&15];out[12]=0;}

static u64 parse_mac(const char*p){u64 v=0;for(int i=0;i<12;i++){char c=p[i];unsigned n;if(c>='0'&&c<='9')n=c-'0';else if(c>='A'&&c<='F')n=c-'A'+10;else if(c>='a'&&c<='f')n=c-'a'+10;else return 0;v=(v<<4)|n;}if(!starts(p+12," HTTP/1."))return 0;return v;}
/* The one place the connection's real state is decided. A UI must show this,
 * never infer it from "selected" or "wanted". */
static int link_state(struct app*a){
 if(a->streaming)return BT_LINK_STREAMING;
 if(!a->wanted)return BT_LINK_IDLE;
 if(a->phase==4)return BT_LINK_SEARCHING;
 if(a->dropped)return BT_LINK_LOST;
 if(a->failed)return BT_LINK_FAILED;
 return BT_LINK_CONNECTING;
}
static int retry_in_ms(struct app*a,u64 now){return(a->wanted&&!a->streaming&&!a->phase&&a->next_try>now)?(int)((a->next_try-now)/1000):0;}
static int quiet_for_ms(struct app*a,u64 now){if(!a->streaming||now<a->ok_at)return 0;u64 ms=(now-a->ok_at)/1000;return ms>2000000000UL?2000000000:(int)ms;}
static unsigned status(struct app*a,char*out,unsigned cap){
 u64 now=a->clock();
 struct writer w={out,0,cap};append(&w,"{\"version\":\"" BTA_VERSION "\",\"message\":");quoted(&w,a->message);field(&w,"sender",a->sender_alive);field(&w,"sender_prio",__atomic_load_n(&a->sender_prio,__ATOMIC_RELAXED));field(&w,"thread_rc",a->thread_rc);field(&w,"prio_rc",a->prio_rc);field(&w,"aff_rc",a->aff_rc);field(&w,"pi_rc",a->pi_rc);
 field(&w,"volume",a->volume);field(&w,"volume_synced",a->volume_synced);field(&w,"wanted",a->wanted);field(&w,"streaming",a->streaming);field(&w,"tv_enabled",a->tv);field(&w,"joint",a->joint);field(&w,"send_type",a->joint?4:3);field(&w,"relinks",a->relinks_total);field(&w,"refused_ms",a->refused_since?(long)((now-a->refused_since)/1000):0);field(&w,"hdmi_muted",a->muted==1);field(&w,"scanning",a->scanning);field(&w,"pin_pending",a->pin_pending);field(&w,"test",now<a->test_until);field(&w,"link",link_state(a));field(&w,"attempts",a->attempts);field(&w,"retry_ms",retry_in_ms(a,now));field(&w,"meter",a->meter);field(&w,"quiet_ms",quiet_for_ms(a,now));field(&w,"level",a->level);field(&w,"packets",a->packets);field(&w,"errors",a->errors);field(&w,"resyncs",a->resyncs);field(&w,"patch_rc",a->patch_rc);field(&w,"packet_rc",a->packet_rc);field(&w,"connect_rc",a->connect_rc);field(&w,"audio_rc",a->last_rc);field(&w,"hdmi_rc",a->hdmi_rc);field(&w,"bt_state",a->btstate);field(&w,"no_widen",a->no_widen);field(&w,"bitpool",a->bp_cur);field(&w,"kdrops",a->bp_last_drops);field(&w,"uptime_us",a->clock());field(&w,"list_rc",a->list_rc);
 field(&w,"diag_sent",(long)__atomic_load_n(&a->diag.sent_bytes,__ATOMIC_RELAXED));
 field(&w,"diag_chunks",(long)__atomic_load_n(&a->diag.sent_chunks,__ATOMIC_RELAXED));
 field(&w,"diag_failed",(long)__atomic_load_n(&a->diag.failed_sends,__ATOMIC_RELAXED));
 char mac[13];mac_text(a->target.address,mac);append(&w,",\"selected\":");quoted(&w,mac);append(&w,",\"name\":");quoted(&w,a->target.name);
 append(&w,",\"devices\":[");int first=1;
 for(unsigned i=0;i<a->devices.count;i++){struct bt_device*d=&a->devices.items[i];if(!bt_is_audio(d)&&d->address!=a->target.address)continue;
  if(!first)append(&w,",");first=0;mac_text(d->address,mac);append(&w,"{\"address\":");quoted(&w,mac);append(&w,",\"name\":");quoted(&w,d->name);field(&w,"audio",!!(d->services&8));append(&w,"}");}
 append(&w,"]}");return w.used;
}
/* Oldest first: [seq,t,pkts,refused,failed,late8,late16,late50,maxgap_us,maxsend_us,maxback_ms,peak]. */
static unsigned timing_json(struct app*a,char*out,unsigned cap){
 struct writer w={out,0,cap};append(&w,"{\"seq\":");number(&w,a->sec_seq);field(&w,"now_s",(long)(a->clock()/1000000));append(&w,",\"secs\":[");
 u32 n=a->sec_seq<120?a->sec_seq:120;
 for(u32 i=0;i<n;i++){struct second*s=&a->secs[(a->sec_seq-n+1+i)%128];if(i)append(&w,",");u32 v[12]={s->seq,s->t,s->pkts,s->refused,s->failed,s->late8,s->late16,s->late50,s->maxgap_us,s->maxsend_us,s->maxback_ms,s->peak};
  append(&w,"[");for(int k=0;k<12;k++){if(k)append(&w,",");number(&w,v[k]);}append(&w,"]");}
 append(&w,"]}");return w.used;
}
static unsigned native_status(struct app*a,char*out){struct bt_native*n=(void*)out;memset(n,0,sizeof(*n));n->magic=BT_NATIVE_MAGIC;n->version=2;n->volume=a->volume;n->streaming=a->streaming;n->wanted=a->wanted;n->tv=a->tv;n->muted=a->muted==1;n->scanning=a->scanning;n->pin_pending=a->pin_pending;n->selected=a->target.address;memcpy(n->message,a->message,sizeof(n->message));for(unsigned i=0;i<a->devices.count;i++)if(bt_is_audio(&a->devices.items[i])||a->devices.items[i].address==a->target.address)n->devices[n->count++]=a->devices.items[i];
 u64 now=a->clock();n->link=link_state(a);n->attempts=(int)a->attempts;n->retry_ms=retry_in_ms(a,now);n->meter=a->meter;n->quiet_ms=quiet_for_ms(a,now);n->test=now<a->test_until;n->connect_rc=(int)a->connect_rc;n->audio_rc=(int)a->last_rc;n->volume_synced=a->volume_synced;
 return sizeof(*n);}
static void response(struct client*c,const char*code,const char*type,const char*body,unsigned n){
 struct writer w={c->output,0,sizeof(c->output)};append(&w,"HTTP/1.1 ");append(&w,code);append(&w,"\r\nContent-Type: ");append(&w,type);append(&w,"\r\nContent-Length: ");number(&w,n);
 append(&w,"\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; connect-src 'self' https://api.github.com; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'\r\n\r\n");
 if(w.used+n>sizeof(c->output)){c->total=0;return;}memcpy(c->output+w.used,body,n);c->total=w.used+n;
}
static char*header(char*request,const char*name){
 char*p=find(request,"\r\n");if(!p)return 0;p+=2;
 while(*p&&*p!='\r'){
  char*q=p;const char*k=name;while(*k){char c=*q;if(c>='A'&&c<='Z')c+=32;if(c!=*k)break;q++;k++;}
  if(!*k&&*q==':'){q++;while(*q==' '||*q=='\t')q++;return q;}
  p=find(p,"\r\n");if(!p)return 0;p+=2;
 }
 return 0;
}
static int host_valid(char*request){
 char*p=header(request,"host");if(!p)return 0;
 u8 ip[4];for(int i=0;i<4;i++){unsigned n=0,digits=0;while(*p>='0'&&*p<='9'){n=n*10+*p++-'0';if(++digits>3||n>255)return 0;}if(!digits)return 0;ip[i]=n;if(i<3&&*p++!='.')return 0;}
 return local_ip(ip)&&starts(p,":8097\r\n");
}
static void dispatch(struct app*a,struct client*c){
 char body[12000];
 if(!host_valid(c->input)){response(c,"403 Forbidden","text/plain","Use the PS4 IP address.",23);return;}
 if(starts(c->input,"GET / HTTP/1.")){response(c,"200 OK","text/html; charset=utf-8",web_page,sizeof(web_page)-1);return;}
 if(starts(c->input,"GET /api/version HTTP/1.")){
  static const char version[]="{\"version\":\"" BTA_VERSION "\"}";
  response(c,"200 OK","application/json",version,sizeof(version)-1);return;
 }
 if(starts(c->input,"GET /api/native HTTP/1.")){unsigned n=native_status(a,body);response(c,"200 OK","application/octet-stream",body,n);return;}
 if(starts(c->input,"GET /api/status HTTP/1.")){unsigned n=status(a,body,sizeof(body));response(c,"200 OK","application/json",body,n);return;}
 if(starts(c->input,"GET /api/timing HTTP/1.")){unsigned n=timing_json(a,body,sizeof(body));response(c,"200 OK","application/json",body,n);return;}
 char*control=header(c->input,"x-bta-control");
 if(!starts(c->input,"POST ")||!control||!starts(control,"1\r\n")){response(c,"403 Forbidden","text/plain","Control header required.",24);return;}
 if(starts(c->input,"POST /api/scan HTTP/1.")){if(a->streaming){response(c,"409 Conflict","text/plain","Stop audio before scanning.",27);return;}if(scan(a)){response(c,"503 Service Unavailable","text/plain","Scan could not start",20);return;}}
 else if(starts(c->input,"POST /api/select/")){
  u64 mac=parse_mac(c->input+17);refresh_devices(a);struct bt_device*d=device(a,mac);
  if(!mac||!d||!bt_is_audio(d)){response(c,"400 Bad Request","text/plain","Select a listed audio device.",29);return;}
  struct bt_device chosen=*d;u64 old=a->target.address;end_scan(a);stop_audio(a);
  if(save_target(a,&chosen)){response(c,"500 Internal Server Error","text/plain","Cannot save selection.",22);return;}
  load_joint(a);
  if(old!=mac&&a->bt>=0)sc3(54,a->bt,0x8008620b,(long)&old);
  a->wanted=1;a->next_try=0;a->poll_at=0;a->dropped=a->failed=a->announced=a->session=0;a->attempts=0;reset_audio_retries(a,1);sc3(10,(long)TV_ON,0,0);a->tv=0;say(a,"Connecting to ","...");
  if(wlog_begin(a,a->clock(),"cmd_select")){wlog_raw(a,"mac=");wlog_mac(a,mac);wlog_raw(a," name=");wlog_str(a,chosen.name);wlog_raw(a," joint=");wlog_dec(a,a->joint);wlog_end(a,1);}
 }
 else if(starts(c->input,"POST /api/pin/")){
  char pin[17];unsigned n=0;const char*p=c->input+14;while(p[n]>='0'&&p[n]<='9'&&n<16){pin[n]=p[n];n++;}
  if(!a->pin_pending||!n||!starts(p+n," HTTP/1.")){response(c,"400 Bad Request","text/plain","Invalid pairing PIN.",20);return;}
  pin[n]=0;struct{u64 address;char*pin;unsigned length,pad;}q={a->target.address,pin,n,0};long rc=sc3(54,a->bt,0x80186207,(long)&q);
  if(wlog_begin(a,a->clock(),"cmd_pin")){wlog_raw(a,"rc=");wlog_dec(a,rc);wlog_end(a,1);}
  if(rc){response(c,"500 Internal Server Error","text/plain","PIN submission failed.",22);return;}a->pin_pending=0;message(a,"Pairing PIN submitted.");
 }
 else if(starts(c->input,"POST /api/volume/")){
  const char*p=c->input+17;unsigned value=0,n=0;
  while(p[n]>='0'&&p[n]<='9'&&n<3){value=value*10+(unsigned)(p[n]-'0');n++;}
  if(!n||value>100||!starts(p+n," HTTP/1.")){response(c,"400 Bad Request","text/plain","Volume must be 0-100.",21);return;}
  if(save_volume(a,(int)value)){response(c,"500 Internal Server Error","text/plain","Cannot save volume.",19);return;}
  a->avrcp_retries=0;avrcp_set_volume(a,(int)value);
  if(wlog_begin(a,a->clock(),"cmd_volume")){wlog_raw(a,"vol=");wlog_dec(a,a->volume);wlog_raw(a," synced=");wlog_dec(a,a->volume_synced);wlog_end(a,0);}
 }
 else if(starts(c->input,"POST /api/test HTTP/1.")){
  if(!a->streaming||a->bt<0||a->btstate<5||a->btstate>6){response(c,"409 Conflict","text/plain","Connect headphones first.",25);return;}
  a->test_until=a->clock()+2000000;message(a,"Playing test tone.");
  if(wlog_begin(a,a->clock(),"cmd_test")){wlog_raw(a,"2s");wlog_end(a,0);}
 }
 else if(starts(c->input,"POST /api/connect HTTP/1.")){a->wanted=1;reset_audio_retries(a,0);sc3(10,(long)TV_ON,0,0);a->tv=0;if(!a->streaming){end_scan(a);a->phase=0;a->next_try=0;a->poll_at=0;a->dropped=a->failed=a->announced=a->session=0;a->attempts=0;say(a,"Connecting to ","...");}if(wlog_begin(a,a->clock(),"cmd_connect")){wlog_raw(a,"streaming=");wlog_dec(a,a->streaming);wlog_end(a,1);}}
 else if(starts(c->input,"POST /api/stop HTTP/1.")){if(wlog_begin(a,a->clock(),"cmd_stop")){wlog_raw(a,"ok");wlog_end(a,1);}a->wanted=0;end_scan(a);stop_audio(a);drop_link(a);reset_audio_retries(a,1);closefd(a->bt);a->bt=-1;a->dropped=a->failed=a->announced=a->session=0;a->attempts=0;message(a,"Earbud audio stopped. TV sound restored.");}
 else if(starts(c->input,"POST /api/joint-on HTTP/1.")){
  if(!a->target.address){response(c,"409 Conflict","text/plain","Select headphones first.",24);return;}
  char p[40];joint_path(a->target.address,p);sc3(10,(long)p,0,0);
  a->joint=1;message(a,"Joint stereo is on for these headphones.");
  if(wlog_begin(a,a->clock(),"cmd_joint")){wlog_raw(a,"joint=1");wlog_end(a,1);}
 }
 else if(starts(c->input,"POST /api/joint-off HTTP/1.")){
  if(!a->target.address){response(c,"409 Conflict","text/plain","Select headphones first.",24);return;}
  char p[40];joint_path(a->target.address,p);
  if(put(p,"on\n",3)){response(c,"500 Internal Server Error","text/plain","Cannot save preference.",23);return;}
  a->joint=0;message(a,"Joint stereo is off for these headphones.");
  if(wlog_begin(a,a->clock(),"cmd_joint")){wlog_raw(a,"joint=0");wlog_end(a,1);}
 }
 else if(starts(c->input,"POST /api/tv-on HTTP/1.")){if(put(TV_ON,"on\n",3)){response(c,"500 Internal Server Error","text/plain","Cannot save preference.",23);return;}a->tv=1;mute(a,0);if(wlog_begin(a,a->clock(),"cmd_tv")){wlog_raw(a,"tv=1");wlog_end(a,0);}}
 else if(starts(c->input,"POST /api/tv-off HTTP/1.")){if(exists(TV_ON)&&sc3(10,(long)TV_ON,0,0)<0){response(c,"500 Internal Server Error","text/plain","Cannot save preference.",23);return;}a->tv=0;if(a->streaming)mute(a,1);if(wlog_begin(a,a->clock(),"cmd_tv")){wlog_raw(a,"tv=0");wlog_end(a,0);}}
 else if(starts(c->input,"POST /api/quit HTTP/1.")){if(wlog_begin(a,a->clock(),"cmd_quit")){wlog_raw(a,"ok");wlog_end(a,1);}a->wanted=0;stop_audio(a);a->quit=1;a->dropped=a->failed=a->announced=a->session=0;a->attempts=0;message(a,"Bluetooth Audio stopped. TV sound restored.");}
 else{response(c,"404 Not Found","text/plain","Not found",9);return;}
 sync_audio_controls(a);
 unsigned n=status(a,body,sizeof(body));response(c,"200 OK","application/json",body,n);
}
static void network_tick(struct app*a,u64 now){
 for(int i=0;i<4;i++){
  struct client*c=&a->clients[i];
  if(c->fd<0){struct sockaddr4 peer;u32 len=sizeof(peer);long fd=sc3(30,a->listener,(long)&peer,(long)&len);if(fd<0)continue;
   if(len!=16||!local_ip((u8*)&peer.addr)||nonblock(fd)<0){closefd(fd);continue;}int one=1;sc5(105,fd,0xffff,0x800,(long)&one,4);c->fd=fd;c->used=c->sent=c->total=0;c->input[0]=0;c->deadline=now+3000000;
  }
   if(now>=c->deadline){closefd(c->fd);c->fd=-1;continue;}
  if(!c->total){long n=sc3(3,c->fd,(long)(c->input+c->used),sizeof(c->input)-c->used-1);
   if(n==0||(n<0&&n!=-35)){closefd(c->fd);c->fd=-1;continue;}
   if(n>0){c->used+=(unsigned)n;c->input[c->used]=0;if(find(c->input,"\r\n\r\n"))dispatch(a,c);else if(c->used==sizeof(c->input)-1)response(c,"431 Request Header Fields Too Large","text/plain","Request too large",17);}
  }
  if(c->total){long n=sc3(4,c->fd,(long)(c->output+c->sent),c->total-c->sent);if(n>0)c->sent+=(unsigned)n;if(c->sent==c->total||(n<0&&n!=-35)){closefd(c->fd);c->fd=-1;}}
 }
}
/* FreeBSD poll(2) on PS4. Ignore the listener when all client slots are full. */
static int wait_worker(struct app*a,u64 deadline){
 struct{int fd;short events,revents;}fds[5]={{.fd=-1,.events=1}};
 u64 client_deadline=0;
 for(int i=0;i<4;i++){
  struct client*c=&a->clients[i];
  fds[i+1].fd=(int)c->fd;
  fds[i+1].events=c->total?4:1; /* POLLOUT or POLLIN */
  if(c->fd<0)fds[0].fd=(int)a->listener;
  else if(!client_deadline||c->deadline<client_deadline)client_deadline=c->deadline;
 }
 u64 now=a->clock();
 if(client_deadline&&now>=client_deadline)return 1;
 if(client_deadline&&client_deadline<deadline)deadline=client_deadline;
 if(!a->net_poll_failed){
  u64 wait=deadline>now?deadline-now:0;
  long rc=sc3(209,(long)fds,5,(long)(wait/1000));
  if(rc>0)return 1;
  if(rc==-4)return 0;
  if(!rc){
   if(wait>0&&wait<1000)a->sleep((unsigned)wait);
   return 0;
  }
  a->net_poll_failed=1;
  if(wlog_begin(a,now,"WARN network_poll")){wlog_raw(a,"rc=");wlog_dec(a,rc);wlog_end(a,1);}
 }
 if(now>=a->net_at){a->net_at=now+4000;return 1;}
 if(a->net_at<deadline)deadline=a->net_at;
 if(deadline>now)a->sleep((unsigned)(deadline-now));
 return 0;
}
/* Feed the driver the way the PS4's own audio daemon does. orbis_audiod creates its
 * Bluetooth senders as dedicated threads with explicit scheduling: SCHED_FIFO, priority
 * 0x132 (306) and CPU mask 0xc0 (cores 6-7), away from game threads. Our sender does
 * nothing but capture ring -> driver; the web page, Bluetooth management and status file
 * stay on the worker thread. The mutex guards only audio state. If the console refuses
 * the scheduling attributes, the sender runs with defaults; if no thread can be created,
 * the worker loop sends audio itself, as before. */
static void init_lock(struct app*a){
 /* Priority inheritance: while the sender waits for the lock, its holder runs at the
  * sender's priority, so a paused housekeeping thread cannot stall the audio. */
 int(*init)(void**,void**,const char*)=0,(*ainit)(void**)=0,(*proto)(void**,int)=0,(*adestroy)(void**)=0;
 void*attr=0;void**ap=0;a->pi_rc=-1;
 if(!resolve("scePthreadMutexattrInit",&ainit)&&ainit&&!ainit(&attr)){ap=&attr;if(!resolve("scePthreadMutexattrSetprotocol",&proto)&&proto)a->pi_rc=proto(ap,1);}
 if(resolve("scePthreadMutexInit",&init)||resolve("scePthreadMutexLock",&a->mlock)||resolve("scePthreadMutexUnlock",&a->munlock)||!init||!a->mlock||!a->munlock)a->mlock=a->munlock=0;
 else if(init(&a->mutex,ap,"Bluetooth Audio state")){a->pi_rc=-2;if(init(&a->mutex,0,"Bluetooth Audio state"))a->mlock=a->munlock=0;}
 if(ap&&!resolve("scePthreadMutexattrDestroy",&adestroy)&&adestroy)adestroy(ap);
 if(a->mlock)resolve("scePthreadMutexDestroy",&a->destroy_mutex);
}
static void*sender(void*arg){
 struct app*a=arg;
 void*(*self)(void)=0;int(*getprio)(void*,int*)=0;
 if(!resolve("scePthreadSelf",&self)&&!resolve("scePthreadGetprio",&getprio)&&self&&getprio){int p=0;if(!getprio(self(),&p))__atomic_store_n(&a->sender_prio,p,__ATOMIC_RELAXED);}
 lock_state(a);
 while(a->sender_run){
  if(!a->audio.active||a->audio.fault!=AUDIO_OK){
   wait_for_audio_change(a,10000);
   continue;
  }
  unsigned wait=audio_tick(a,a->clock());
  unlock_state(a);
  if(wait)a->sleep(wait);
  lock_state(a);
 }
 a->sender_exited=1;
 wake_audio(a);
 unlock_state(a);
 return 0;
}
static void start_sender(struct app*a){
 a->thread_rc=a->cond_rc=a->prio_rc=a->aff_rc=a->inherit_rc=a->policy_rc=-1;
 if(!a->mlock)return;
 int(*create)(void**,void**,void*(*)(void*),void*,const char*)=0;
 int(*cond_init)(void**,const void*,const char*)=0;
 if(resolve("scePthreadCreate",&create)||resolve("scePthreadJoin",&a->join_sender)||!create||!a->join_sender)return;
 if(!resolve("scePthreadCondInit",&cond_init)&&!resolve("scePthreadCondWait",&a->wait_audio)&&
    !resolve("scePthreadCondSignal",&a->signal_audio)&&!resolve("scePthreadCondDestroy",&a->destroy_cond)&&
    cond_init&&a->wait_audio&&a->signal_audio&&a->destroy_cond)
  a->cond_rc=cond_init(&a->audio_cond,0,"Bluetooth Audio wake");
 if(a->cond_rc)a->signal_audio=0;
 int(*init)(void**)=0,(*inherit)(void**,int)=0,(*policy)(void**,int)=0,(*param)(void**,const int*)=0,(*affinity)(void**,u64)=0,(*destroy)(void**)=0;
 void*attr=0;void**ap=0;
 if(!resolve("scePthreadAttrInit",&init)&&init&&!init(&attr)){
  ap=&attr;int prio=0x132;
  if(!resolve("scePthreadAttrSetinheritsched",&inherit)&&inherit)a->inherit_rc=inherit(ap,0);
  if(!resolve("scePthreadAttrSetschedpolicy",&policy)&&policy)a->policy_rc=policy(ap,1);
  a->prio_rc=(!resolve("scePthreadAttrSetschedparam",&param)&&param)?param(ap,&prio):-1;
  a->aff_rc=(!resolve("scePthreadAttrSetaffinity",&affinity)&&affinity)?affinity(ap,0xc0):-1;
 }
 a->sender_run=1;
 a->thread_rc=create(&a->sender_thread,ap,sender,a,"Bluetooth Audio sender");
 if(a->thread_rc&&ap){
  a->prio_rc=a->aff_rc=a->inherit_rc=a->policy_rc=-2;
  a->thread_rc=create(&a->sender_thread,0,sender,a,"Bluetooth Audio sender");
 }
 if(a->thread_rc)a->sender_run=0;else a->sender_alive=1;
 if(ap&&!resolve("scePthreadAttrDestroy",&destroy)&&destroy)destroy(ap);
}
/* The sender uses this stack frame's state: wait for it to leave before returning. */
static void stop_sender(struct app*a){
 if(!a->sender_alive)return;
 lock_state(a);
 a->sender_run=0;
 wake_audio(a);
 while(!a->sender_exited)wait_for_audio_change(a,500);
 unlock_state(a);
 a->join_sender(a->sender_thread,0);
 a->sender_alive=0;
}
void*bta_worker(void*arg){
 struct app a;memset(&a,0,sizeof(a));a.listener=(long)arg;a.bt=a.hdmi=a.shmfd=-1;a.muted=-1;a.wanted=0;for(int i=0;i<4;i++)a.clients[i].fd=-1;
 if(resolve("sceKernelUsleep",&a.sleep)||resolve("sceKernelGetProcessTime",&a.clock)||resolve("shm_open",&a.shm)||resolve("mmap",&a.map)||resolve("munmap",&a.unmap)||!a.sleep||!a.clock||!a.shm||!a.map||!a.unmap){closefd(a.listener);return 0;}
 sc3(136,(long)"/data/bta_audio",0777,0);sc3(15,(long)"/data/bta_audio",0777,0);sc3(10,(long)BTA_STOP,0,0);
 if(exists(WORKER_LOG))sc3(128,(long)WORKER_LOG,(long)WORKER_LOG_PREV,0);
 load_target(&a);load_joint(&a);load_volume(&a);a.tv=exists(TV_ON);a.no_widen=exists(NO_WIDEN);a.hdmi=sc3(5,(long)"/dev/hdmi",2,0);
 message(&a,"Ready. Select your headphones and press Connect.");notify("Bluetooth Audio is ready and idle. Open the home-screen app, then press Connect. Web controls: http://127.0.0.1:8097/ (or your PS4 IP on port 8097).");
 init_lock(&a);start_sender(&a);
 {u64 t0=a.clock();
  struct{u64 size;char text[28];unsigned version;}sw={.size=40};int(*getver)(void*)=0;unsigned swv=0;
  if(!resolve("sceKernelGetSystemSwVersion",&getver)&&getver&&!getver(&sw)){sw.text[27]=0;swv=sw.version;}
  diag_init(&a.diag,a.sleep,a.clock,a.map,a.unmap,swv?sw.text:"unknown");
  if(wlog_begin(&a,t0,"worker_start")){
   wlog_raw(&a,"v=" BTA_VERSION " fw=");wlog_str(&a,swv?sw.text:"unknown");wlog_raw(&a," fw_hex=");wlog_hex(&a,swv);
   wlog_raw(&a," hdmi_fd=");wlog_dec(&a,a.hdmi);wlog_raw(&a," tv=");wlog_dec(&a,a.tv);
   wlog_raw(&a," vol=");wlog_dec(&a,a.volume);wlog_raw(&a," joint=");wlog_dec(&a,a.joint);
   wlog_raw(&a," no_widen=");wlog_dec(&a,a.no_widen);
   if(a.target.address){wlog_raw(&a," saved=");wlog_mac(&a,a.target.address);wlog_raw(&a," name=");wlog_str(&a,a.target.name);}
   wlog_end(&a,1);
  }
  int sched_bad=(!a.sender_alive||a.thread_rc||a.cond_rc||a.prio_rc||a.aff_rc||a.pi_rc||a.inherit_rc||a.policy_rc);
  if(wlog_begin(&a,t0,sched_bad?"WARN sender_sched":"sender_sched")){
   wlog_raw(&a,"alive=");wlog_dec(&a,a.sender_alive);wlog_raw(&a," pi_rc=");wlog_dec(&a,a.pi_rc);
   wlog_raw(&a," thread_rc=");wlog_dec(&a,a.thread_rc);wlog_raw(&a," prio_rc=");wlog_dec(&a,a.prio_rc);
   wlog_raw(&a," aff_rc=");wlog_dec(&a,a.aff_rc);wlog_raw(&a," cond_rc=");wlog_dec(&a,a.cond_rc);
   wlog_raw(&a," inherit_rc=");wlog_dec(&a,a.inherit_rc);wlog_raw(&a," policy_rc=");wlog_dec(&a,a.policy_rc);
   if(sched_bad)wlog_raw(&a," [DEGRADED_SCHEDULING]");
   wlog_end(&a,1);
  }
  wlog_flush(&a,t0,1);
 }
 int network_ready=0;
 u64 audio_at=0;
 while(!a.quit){
  u64 now=a.clock();
  int beat=now>=a.heartbeat;
  if(!a.sender_alive&&a.streaming&&now>=audio_at){
   lock_state(&a);
   if(a.audio.active&&a.audio.fault==AUDIO_OK){
    unsigned wait=audio_tick(&a,now);
    audio_at=a.clock()+wait;
   }
   unlock_state(&a);
  }
  collect_audio(&a,now,beat);
  if(now>=a.poll_at){
   bluetooth_tick(&a,now);
   a.poll_at=(a.bt<0&&!a.wanted&&!a.scanning)?a.list_at:now+50000;
  }
  if(network_ready){network_tick(&a,now);if(a.quit)break;}
  if(beat){
   char json[12000];
   unsigned n=status(&a,json,sizeof(json));a.level=0;a.heartbeat=now+1000000;
   put("/data/bta_audio/status.json",json,n);
   if(exists(BTA_STOP)){if(wlog_begin(&a,now,"stop_sentinel")){wlog_raw(&a,"detected");wlog_end(&a,1);}break;}
   int tv=exists(TV_ON),nw=exists(NO_WIDEN);
   a.tv=tv;
   if(nw!=a.no_widen){a.no_widen=nw;if(wlog_begin(&a,now,"no_widen_changed")){wlog_raw(&a,"no_widen=");wlog_dec(&a,nw);wlog_end(&a,1);}}
   wlog_flush(&a,now,0);
  }
  u64 deadline=a.poll_at<a.heartbeat?a.poll_at:a.heartbeat;
  if(a.streaming){
   if(a.meter_at<deadline)deadline=a.meter_at;
   if(a.muted==0&&!a.tv&&now<a.stream_at+PRIME_US&&a.stream_at+PRIME_US<deadline)deadline=a.stream_at+PRIME_US;
   if(!a.sender_alive&&audio_at<deadline)deadline=audio_at;
  }else audio_at=0;
  network_ready=wait_worker(&a,deadline);
 }
 stop_sender(&a);
 end_scan(&a);stop_audio(&a);a.wanted=0;message(&a,"Bluetooth Audio is offline. TV sound restored.");char json[12000];unsigned n=status(&a,json,sizeof(json));put("/data/bta_audio/status.json",json,n);
 if(wlog_begin(&a,a.clock(),"worker_stop")){
  wlog_raw(&a,"quit=");wlog_dec(&a,a.quit);wlog_raw(&a," packets=");wlog_dec(&a,(long)a.packets);
  wlog_raw(&a," errors=");wlog_dec(&a,(long)a.errors);wlog_raw(&a," resyncs=");wlog_dec(&a,(long)a.resyncs);
  wlog_raw(&a," relinks=");wlog_dec(&a,(long)a.relinks_total);wlog_end(&a,1);
 }
 wlog_flush(&a,a.clock(),1);
 diag_stop(&a.diag);
 if(a.cond_rc==0)a.destroy_cond(&a.audio_cond);
 if(a.destroy_mutex)a.destroy_mutex(&a.mutex);
 for(int i=0;i<4;i++)closefd(a.clients[i].fd);
 closefd(a.bt);closefd(a.hdmi);closefd(a.listener);
 return 0;
}

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <orbis/Sysmodule.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#define sceHttpSetResponseHeaderMaxSize sceHttpSetResponseHeaderMaxSize_stub
#include <orbis/Http.h>
#undef sceHttpSetResponseHeaderMaxSize
int32_t sceHttpSetResponseHeaderMaxSize(int32_t id,size_t headerSize);
#include "ui.h"
#include "../uid.h"

static int pad=-1,pad_connected;
static uint64_t pad_retry,g_start,last_launch_try,app_flush_at,app_direct_at,refresh_err_at;

#define APP_RING_SIZE 65536U
static char app_ring[APP_RING_SIZE];
static size_t app_head,app_tail;
static uint64_t app_dropped,app_direct_offset;
static uint32_t app_direct_seq=1;
static char app_session_id[20];
static int g_direct_busy;
static char g_direct_buf[16384],g_direct_err[160];
static size_t g_direct_len;

static void app_ring_push(const char*data,size_t len){
 if(!len||len>=APP_RING_SIZE)return;
 while(APP_RING_SIZE-(app_head-app_tail)<len){
  size_t adv=0,avail=app_head-app_tail;
  while(adv<avail&&app_ring[(app_tail+adv)&(APP_RING_SIZE-1)]!='\n')adv++;
  if(adv<avail)adv++;
  app_tail+=adv;
  app_dropped++;
 }
 size_t idx=app_head&(APP_RING_SIZE-1);
 size_t first=APP_RING_SIZE-idx;
 if(first>=len)memcpy(app_ring+idx,data,len);
 else{memcpy(app_ring+idx,data,first);memcpy(app_ring,data+first,len-first);}
 app_head+=len;
}
static size_t app_ring_peek(char*out,size_t max_len){
 size_t avail=app_head-app_tail;
 if(!avail)return 0;
 size_t len=avail>max_len?max_len:avail;
 size_t idx=app_tail&(APP_RING_SIZE-1);
 size_t first=APP_RING_SIZE-idx;
 if(first>=len)memcpy(out,app_ring+idx,len);
 else{memcpy(out,app_ring+idx,first);memcpy(out+first,app_ring,len-first);}
 if(avail>max_len){
  for(size_t k=len;k>len/2;k--)if(out[k-1]=='\n'){len=k;break;}
 }
 return len;
}

/* All file I/O uses sceKernel* (never stdio): the app runs in a restricted
 * sandbox where a blocked libc syscall raises SIGSYS. app.log confirms these
 * kernel calls succeed. */
static void logline(const char*s){
 uint64_t now=sceKernelGetProcessTime();
 if(!g_start)g_start=now;
 uint64_t t=(now>=g_start?(now-g_start):0)/100000;
 char buf[320];
 int n=snprintf(buf,sizeof(buf),"[+%llu.%llus] %s\n",(unsigned long long)(t/10),(unsigned long long)(t%10),s);
 if(n<=0)return;
 if(n>=(int)sizeof(buf)){n=(int)sizeof(buf)-1;buf[n-1]='\n';}
 app_ring_push(buf,(size_t)n);
 int fd=sceKernelOpen("/data/bta_audio/app.log",0x209,0644);
 if(fd>=0){sceKernelWrite(fd,buf,(size_t)n);sceKernelClose(fd);}
}
static void log_rotate(void){
 int fd=sceKernelOpen("/data/bta_audio/app.log",0,0);
 if(fd<0)return;
 off_t sz=sceKernelLseek(fd,0,2);
 sceKernelClose(fd);
 if(sz>=131072)sceKernelRename("/data/bta_audio/app.log","/data/bta_audio/app.prev.log");
}
static unsigned controller(uint64_t now){
 OrbisPadData data;memset(&data,0,sizeof(data));
 if(pad>=0){
  int rc=scePadReadState(pad,&data);
  if(rc>=0&&data.connected){pad_connected=1;return data.buttons;}
  if(pad_connected){char msg[96];snprintf(msg,sizeof(msg),"Controller disconnected (rc=0x%x connected=%d)",rc,(int)data.connected);logline(msg);}
  pad_connected=0;
 }
 if(now<pad_retry)return 0;pad_retry=now+2000000;
 if(pad>=0){scePadClose(pad);pad=-1;}
 OrbisUserServiceLoginUserIdList users;for(int i=0;i<4;i++)users.userId[i]=-1;
 int rc=sceUserServiceGetLoginUserIdList(&users);
 if(rc<0)sceUserServiceGetInitialUser(&users.userId[0]);
 for(int i=0;i<4;i++)if(users.userId[i]>=0){
  int h=scePadGetHandle(users.userId[i],ORBIS_PAD_PORT_TYPE_STANDARD,0);
  if(h<0)h=scePadOpen(users.userId[i],ORBIS_PAD_PORT_TYPE_STANDARD,0,0);
  if(h<0)continue;memset(&data,0,sizeof(data));
  if(scePadReadState(h,&data)>=0&&data.connected){pad=h;pad_connected=1;logline("Controller acquired");return data.buttons;}
  scePadClose(h);
 }
 return 0;
}

/* ---- HTTP client: the audio service owns Bluetooth; this app only displays and requests ---- */
enum { SOCKET_REFUSED = -2 };
static int local_socket(int port,int*err_out){
 if(err_out)*err_out=0;
 int fd=socket(AF_INET,SOCK_STREAM,0);
 if(fd<0){if(err_out)*err_out=errno;return -1;}
 struct timeval t={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));
 int one=1;setsockopt(fd,SOL_SOCKET,0x800,&one,sizeof(one));
 struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_len=sizeof(a);a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(0x7f000001);
 if(connect(fd,(void*)&a,sizeof(a))<0){int e=errno;if(err_out)*err_out=e;int rc=e==ECONNREFUSED?SOCKET_REFUSED:-1;close(fd);return rc;}
 return fd;
}
static int write_all(int fd,const void*p,size_t n,int*err_out){
 if(err_out)*err_out=0;
 const char*b=p;
 while(n){ssize_t k=send(fd,b,n,0);if(k<=0){if(err_out)*err_out=k<0?errno:EPIPE;return -1;}b+=k;n-=(size_t)k;}
 return 0;
}
/* Returns the HTTP status code, SOCKET_REFUSED if no listener, or -1 on other
 * transport errors. Copies the NUL-terminated response body into out when provided. */
static int http(const char*method,const char*path,void*out,size_t cap,int*err_out){
 if(err_out)*err_out=0;
 int fd=local_socket(8097,err_out);if(fd<0)return fd;
 char request[256];int len=snprintf(request,sizeof(request),"%s %s HTTP/1.1\r\nHost: 127.0.0.1:8097\r\nX-BTA-Control: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",method,path);
 if(len<0||len>=(int)sizeof(request)||write_all(fd,request,(size_t)len,err_out)){close(fd);return -1;}
 char response[16384];size_t used=0;
 while(used+1<sizeof(response)){ssize_t n=recv(fd,response+used,sizeof(response)-used-1,0);if(n==0)break;if(n<0){if(err_out)*err_out=errno;close(fd);return -1;}used+=(size_t)n;}
 close(fd);response[used]=0;
 if(used<12||strncmp(response,"HTTP/1.1 ",9)){if(err_out)*err_out=-2;return -1;}
 int code=atoi(response+9);
 char*b=strstr(response,"\r\n\r\n");
 if(out&&cap){size_t bytes=0;if(b){b+=4;bytes=used-(size_t)(b-response);if(bytes>cap-1)bytes=cap-1;memcpy(out,b,bytes);}((char*)out)[bytes]=0;}
 return code>0?code:-1;
}
static void flush_app_logs_to_payload(int force){
 if(!online||app_head==app_tail)return;
 if(!force&&g_now<app_flush_at)return;
 app_flush_at=g_now+500000;
 for(int iter=0;iter<4&&app_head!=app_tail;iter++){
  char chunk[1536];
  size_t clen=app_ring_peek(chunk,sizeof(chunk));
  if(!clen)break;
  int err=0;
  int fd=local_socket(8097,&err);
  if(fd<0)break;
  char hdr[256];
  int hlen=snprintf(hdr,sizeof(hdr),
   "POST /api/app-log HTTP/1.1\r\nHost: 127.0.0.1:8097\r\nX-BTA-Control: 1\r\n"
   "Content-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",clen);
  if(hlen<=0||hlen>=(int)sizeof(hdr)||write_all(fd,hdr,(size_t)hlen,&err)||write_all(fd,chunk,clen,&err)){close(fd);break;}
  char resp[256];size_t used=0;
  while(used+1<sizeof(resp)){ssize_t n=recv(fd,resp+used,sizeof(resp)-used-1,0);if(n<=0)break;used+=(size_t)n;}
  close(fd);resp[used]=0;
  if(used>=12&&!strncmp(resp,"HTTP/1.1 200",12)){app_tail+=clen;}
  else break;
 }
}
/* Reads /api/native. Older services send a shorter reply without the link fields;
 * those are accepted (ext_ok=0) and the UI falls back to the basic flags. */
static int prev_online=-1,prev_link=-1,prev_streaming=-1,prev_conn_rc=0,prev_audio_rc=0,prev_wanted=-1,prev_tv=-1,prev_muted=-1;
static void log_service_change(void){
 if(online!=prev_online){
  prev_online=online;
  if(!online){logline("service: offline (port 8097 unreachable)");prev_link=-1;return;}
  char msg[160];snprintf(msg,sizeof(msg),"service: online ext_ok=%d count=%d selected=%012lX native_sz=%zu",ext_ok,state.count,state.selected,sizeof(struct bt_native));
  logline(msg);
 }
 if(!online)return;
 int l=ui_link();
 if(l!=prev_link||state.streaming!=prev_streaming||state.wanted!=prev_wanted||state.tv!=prev_tv||state.muted!=prev_muted||state.connect_rc!=prev_conn_rc||(state.audio_rc<0&&state.audio_rc!=prev_audio_rc)){
  prev_link=l;prev_streaming=state.streaming;prev_wanted=state.wanted;prev_tv=state.tv;prev_muted=state.muted;prev_conn_rc=state.connect_rc;prev_audio_rc=state.audio_rc;
  char msg[300];
  snprintf(msg,sizeof(msg),"state: link=%d want=%d str=%d tv=%d mut=%d vol=%d vs=%d att=%d crc=%d arc=%d msg=\"%.90s\"",
   l,state.wanted,state.streaming,state.tv,state.muted,state.volume,state.volume_synced,state.attempts,state.connect_rc,state.audio_rc,state.message);
  logline(msg);
 }
}
static int refresh(void){
 struct bt_native next;memset(&next,0,sizeof(next));
 int err=0;
 int fd=local_socket(8097,&err);
 if(fd<0){
  if(online||g_now>=refresh_err_at){refresh_err_at=g_now+10000000;char m[96];snprintf(m,sizeof(m),"refresh: socket err=%d (fd=%d)",err,fd);logline(m);}
  online=0;log_service_change();return -1;
 }
 const char*req="GET /api/native HTTP/1.1\r\nHost: 127.0.0.1:8097\r\nConnection: close\r\n\r\n";
 if(write_all(fd,req,strlen(req),&err)){
  close(fd);
  if(online||g_now>=refresh_err_at){refresh_err_at=g_now+10000000;char m[96];snprintf(m,sizeof(m),"refresh: write err=%d",err);logline(m);}
  online=0;log_service_change();return -1;
 }
 char resp[8192];size_t used=0;
 while(used<sizeof(resp)){ssize_t n=recv(fd,resp+used,sizeof(resp)-used,0);if(n<=0)break;used+=(size_t)n;}
 close(fd);
 if(used<12||strncmp(resp,"HTTP/1.1 200",12)){
  char line[64]={0};for(size_t i=0;i<used&&i+1<sizeof(line)&&resp[i]!='\r'&&resp[i]!='\n';i++)line[i]=(resp[i]>=32&&resp[i]<127)?resp[i]:'?';
  char m[144];snprintf(m,sizeof(m),"refresh: bad HTTP (used=%zu) \"%s\"",used,line);logline(m);
  online=0;log_service_change();return -1;
 }
 char*body=0;for(size_t i=0;i+3<used;i++)if(resp[i]=='\r'&&resp[i+1]=='\n'&&resp[i+2]=='\r'&&resp[i+3]=='\n'){body=resp+i+4;break;}
 size_t avail=body?(size_t)(resp+used-body):0;
 if(!body||avail<offsetof(struct bt_native,link)){
  char m[96];snprintf(m,sizeof(m),"refresh: short body (%zu bytes)",avail);logline(m);
  online=0;log_service_change();return -1;
 }
 memcpy(&next,body,avail<sizeof(next)?avail:sizeof(next));
 if(next.magic!=BT_NATIVE_MAGIC||next.version!=2||next.count<0||next.count>BT_DEVICE_LIMIT){
  char m[128];snprintf(m,sizeof(m),"refresh: invalid header magic=0x%x ver=%u count=%d",next.magic,next.version,next.count);logline(m);
  online=0;log_service_change();return -1;
 }
 unsigned long selected=(focus>=0&&focus<state.count)?state.devices[focus].address:0;
 state=next;ext_ok=avail>=sizeof(next);state.message[127]=0;for(int i=0;i<state.count;i++)state.devices[i].name[95]=0;
 if(selected){for(int i=0;i<state.count;i++)if(state.devices[i].address==selected)focus=i;}
 if(focus<0||focus>=state.count)focus=0;
 online=1;last_launch_try=0;log_service_change();flush_app_logs_to_payload(0);return 0;
}
static void set_notice(const char*s){snprintf(notice,sizeof(notice),"%s",s);notice_until=g_now+6000000;}

enum service_phase { SERVICE_CHECK_VERSION, SERVICE_STOPPING, SERVICE_STARTING, SERVICE_UNLOADING, SERVICE_MONITOR };
static enum service_phase service_phase=SERVICE_CHECK_VERSION;
static uint64_t service_deadline;
static char payload[131072];
static size_t payload_size;
static void trigger_direct_log_upload(int sync);
static void set_service_phase(enum service_phase next){
 if(service_phase!=next){
  char m[64];snprintf(m,sizeof(m),"service_phase: %d -> %d",(int)service_phase,(int)next);logline(m);
  service_phase=next;
 }
}
static void read_diag_url(char out[320],const char*route);
static long file_size_of(const char*path){
 int fd=sceKernelOpen(path,0,0);
 if(fd<0)return (long)fd;
 off_t sz=sceKernelLseek(fd,0,2);
 sceKernelClose(fd);
 return (long)sz;
}
static void log_startup_env(void){
 char du[320]={0};read_diag_url(du,"");
 long dsz=file_size_of("/data/bta_audio/device.bin");
 long vsz=file_size_of("/data/bta_audio/volume.bin");
 long tsz=file_size_of("/data/bt_bta_tv_on");
 long wsz=file_size_of("/data/bta_audio/worker.log");
 long lsz=file_size_of("/data/bta_audio/launcher.bin");
 long psz=file_size_of("/data/bt_accept_patch_result.bin");
 char m[300];
 snprintf(m,sizeof(m),"env: diag=%.80s dev=%ld vol=%ld tv=%ld wlog=%ld launch=%ld patch=%ld",
  du,dsz,vsz,tsz,wsz,lsz,psz);
 logline(m);
}
static void log_offline_probes(const char*reason){
 struct bta_launch_trace lt;memset(&lt,0,sizeof(lt));
 int lfd=sceKernelOpen("/data/bta_audio/launcher.bin",0,0);
 if(lfd>=0){
  ssize_t n=sceKernelRead(lfd,&lt,sizeof(lt));sceKernelClose(lfd);
  char m[128];
  if(n==(ssize_t)sizeof(lt)&&lt.magic==BTA_LAUNCH_MAGIC){
   snprintf(m,sizeof(m),"probe_launch: reason=%s stage=%d rc=%d bytes=%lu",reason,lt.stage,lt.rc,lt.bytes);
  }else{
   snprintf(m,sizeof(m),"probe_launch: reason=%s n=%zd magic=0x%lx",reason,n,lt.magic);
  }
  logline(m);
 }else{
  char m[96];snprintf(m,sizeof(m),"probe_launch: reason=%s open=0x%x",reason,lfd);logline(m);
 }
 int ferr=0,ffd=local_socket(2121,&ferr);
 char banner[64]={0};ssize_t bn=0;
 if(ffd>=0){bn=recv(ffd,banner,sizeof(banner)-1,0);close(ffd);for(ssize_t i=0;i<bn;i++)if(banner[i]<32||banner[i]>=127)banner[i]=' ';}
 int perr=0,pfd=local_socket(8097,&perr);if(pfd>=0)close(pfd);
 {char m[200];snprintf(m,sizeof(m),"probe_ports: ftp_fd=%d ftp_err=%d bn=%zd \"%s\" p8097_fd=%d p8097_err=%d",ffd,ferr,bn,banner,pfd,perr);logline(m);}
 int wfd=sceKernelOpen("/data/bta_audio/worker.log",0,0);
 if(wfd>=0){
  off_t wsz=sceKernelLseek(wfd,0,2);
  off_t off=wsz>240?(wsz-240):0;
  sceKernelLseek(wfd,off,0);
  char tail[241]={0};ssize_t tn=sceKernelRead(wfd,tail,240);sceKernelClose(wfd);
  if(tn>0){for(ssize_t i=0;i<tn;i++)if(tail[i]=='\n'||tail[i]=='\r'||tail[i]<32||tail[i]>=127)tail[i]='|';}
  char m[300];snprintf(m,sizeof(m),"probe_wlog: sz=%ld tail=\"%.210s\"",(long)wsz,tail);logline(m);
 }else{
  char m[96];snprintf(m,sizeof(m),"probe_wlog: open=0x%x",wfd);logline(m);
 }
}

static int read_payload_version(char*version,size_t cap){
 char body[96];int err=0;
 int code=http("GET","/api/version",body,sizeof(body),&err);
 /* Older workers reject unknown GET routes. They put the version first in /api/status. */
 if(code==403||code==404)code=http("GET","/api/status",body,sizeof(body),&err);
 if(code!=200){
  if(code!=SOCKET_REFUSED){char m[96];snprintf(m,sizeof(m),"service: version check code=%d err=%d",code,err);logline(m);}
  return code;
 }
 static const char prefix[]="{\"version\":\"";
 if(strncmp(body,prefix,sizeof(prefix)-1)){
  char san[64]={0};for(size_t i=0;body[i]&&i+1<sizeof(san);i++)san[i]=(body[i]>=32&&body[i]<127)?body[i]:'?';
  char m[128];snprintf(m,sizeof(m),"service: version prefix mismatch \"%s\"",san);logline(m);
  return -1;
 }
 const char*value=body+sizeof(prefix)-1;
 size_t len=strspn(value,"0123456789.");
 if(!len||len>=cap||value[len]!='"'){
  char san[64]={0};for(size_t i=0;body[i]&&i+1<sizeof(san);i++)san[i]=(body[i]>=32&&body[i]<127)?body[i]:'?';
  char m[128];snprintf(m,sizeof(m),"service: version parse failed \"%s\"",san);logline(m);
  return -1;
 }
 memcpy(version,value,len);version[len]=0;
 int parts[4];const char*pre;
 return bta_parse_version(version,parts,&pre)==0?200:-1;
}

static int load_bundled_payload(void){
 int f=sceKernelOpen("/app0/assets/BTA-Audio.bin",0,0);
 if(f<0){char m[96];snprintf(m,sizeof(m),"start: bundled payload open rc=0x%x",f);logline(m);set_notice("Bundled audio payload not found.");return -1;}
 long n=0;
 size_t used=0;
 while(used<sizeof(payload)){n=sceKernelRead(f,payload+used,sizeof(payload)-used);if(n<=0)break;used+=(size_t)n;}
 sceKernelClose(f);
 if(n<0||used<1024||used>=sizeof(payload)){
  char m[112];snprintf(m,sizeof(m),"start: payload read failed (n=%ld used=%zu)",n,used);logline(m);
  set_notice("Could not read the bundled audio payload.");return -1;
 }
 uint32_t fnv=2166136261u;
 for(size_t i=0;i<used;i++){fnv^=(uint8_t)payload[i];fnv*=16777619u;}
 char hdr[33]={0};static const char hx[]="0123456789abcdef";
 for(int i=0;i<16;i++){uint8_t b=(uint8_t)payload[i];hdr[i*2]=hx[b>>4];hdr[i*2+1]=hx[b&15];}
 {char m[144];snprintf(m,sizeof(m),"payload: loaded %zu bytes fnv=0x%08x hdr=%s",used,fnv,hdr);logline(m);}
 payload_size=used;return 0;
}

static void start_service(void){
 set_service_phase(SERVICE_MONITOR);
 if(!payload_size&&load_bundled_payload()){trigger_direct_log_upload(0);return;}
 /* The BinLoader connection must carry the payload. A probe can consume its slot. */
 last_launch_try=sceKernelGetProcessTime();
 int err=0;
 int fd=local_socket(9090,&err);
 if(fd<0){
  char m[128];snprintf(m,sizeof(m),"start: BinLoader (9090) unreachable (rc=%d errno=%d); enable it in GoldHEN",fd,err);
  logline(m);
  log_offline_probes("9090_unreach");
  set_notice("BinLoader unavailable. Enable it in GoldHEN or run the jailbreak again.");
  trigger_direct_log_upload(0);
  return;
 }
 int rc=write_all(fd,payload,payload_size,&err);close(fd);
 char msg[128];
 if(rc)snprintf(msg,sizeof(msg),"start: send failed (%zu bytes, errno=%d)",payload_size,err);
 else snprintf(msg,sizeof(msg),"start: payload sent to BinLoader (%zu bytes)",payload_size);
 logline(msg);
 if(rc){log_offline_probes("9090_send_fail");set_notice("Could not send the audio payload to BinLoader.");trigger_direct_log_upload(0);return;}
 set_service_phase(SERVICE_STARTING);
 service_deadline=sceKernelGetProcessTime()+15000000;
 set_notice("Starting audio service...");notice_until=service_deadline;
}

static int g_uid_active,g_uid_ready,g_uid_logged;
static uint64_t g_uid_deadline;
static char g_uid_buf[16],g_uid_log[160],g_legacy_hex[17];

static void service_check(void){
 if(online){refresh();return;}

 char version[64];
 int code=read_payload_version(version,sizeof(version));
 if(service_phase==SERVICE_UNLOADING){
  if(code==SOCKET_REFUSED){
   set_service_phase(SERVICE_MONITOR);
   set_notice("Audio payload unloaded. Press L2 to start it again.");
   return;
  }
  if(g_now>=service_deadline){
   set_service_phase(SERVICE_MONITOR);
   logline("unload: timed out waiting for payload to release port 8097");
   set_notice("Could not unload audio payload. Try again.");
  }
  return;
 }
 if(code==200&&!strcmp(version,BTA_VERSION)){
  if(refresh()==0&&service_phase!=SERVICE_MONITOR){
   logline("service: payload version matches v" BTA_VERSION);
   set_service_phase(SERVICE_MONITOR);notice[0]=0;
  }
  return;
 }
 if(service_phase==SERVICE_MONITOR)return;
 if(service_phase==SERVICE_STARTING){
  if(g_now>=service_deadline){
   set_service_phase(SERVICE_MONITOR);
   logline("start: v" BTA_VERSION " payload did not become ready");
   log_offline_probes("start_timeout");
   set_notice("Audio payload did not start. Press X to retry.");
   trigger_direct_log_upload(0);
  }
  return;
 }
 if(code==SOCKET_REFUSED){
  if(!__atomic_load_n(&g_uid_ready,__ATOMIC_ACQUIRE)&&g_now<g_uid_deadline)return;
  start_service();
  return;
 }
 if(service_phase==SERVICE_STOPPING){
  if(g_now>=service_deadline){
   set_service_phase(SERVICE_MONITOR);
   logline("sync: timed out waiting for the old payload to release port 8097");
   log_offline_probes("stop_timeout");
   set_notice("Old audio payload did not stop. Press X to retry.");
   trigger_direct_log_upload(0);
  }
  return;
 }
 if(code!=200)return;

 char msg[128];snprintf(msg,sizeof(msg),"sync: replacing payload v%s with v%s",version,BTA_VERSION);logline(msg);
 if(!payload_size&&load_bundled_payload()){set_service_phase(SERVICE_MONITOR);return;}
 int err=0;
 code=http("POST","/api/quit",0,0,&err);
 if(code>=0&&code!=200){
  set_service_phase(SERVICE_MONITOR);
  snprintf(msg,sizeof(msg),"sync: shutdown request failed (HTTP %d, err=%d)",code,err);logline(msg);
  set_notice("Could not stop the old audio payload. Press X to retry.");return;
 }
 /* A dropped reply can still mean quit succeeded. Wait for the listener to close. */
 set_service_phase(SERVICE_STOPPING);
 service_deadline=sceKernelGetProcessTime()+15000000;
 set_notice("Updating audio service...");notice_until=service_deadline;
}
static void action(const char*path){
 char body[160];int err=0;int code=http("POST",path,body,sizeof(body),&err);
 if(code==200)notice[0]=0;
 else if(code<0){
  char msg[160];snprintf(msg,sizeof(msg),"http: POST %.80s failed (rc=%d errno=%d)",path,code,err);logline(msg);
  set_notice("No response from the audio service.");
 }else{
  for(int i=0;body[i];i++)if(body[i]=='\n'||body[i]=='\r')body[i]=0;
  char msg[240];snprintf(msg,sizeof(msg),"http: POST %.60s -> %d %.100s",path,code,body);logline(msg);
  set_notice(body[0]?body:"That didn't work.");
 }
 refresh();
}
static void volume_change(int amount){
 if(!online)return;int value=state.volume+amount;if(value<0)value=0;if(value>100)value=100;
 char path[40];snprintf(path,sizeof(path),"/api/volume/%d",value);action(path);
}
static void activate(void){
 if(!online){
  if(service_phase==SERVICE_STOPPING||service_phase==SERVICE_STARTING||service_phase==SERVICE_UNLOADING)return;
  if(last_launch_try&&g_now<last_launch_try+5000000){
   logline("activate: BinLoader retry skipped (cooldown; BinLoader may only accept 1 payload per jailbreak)");
   set_notice("Service offline. Wait a moment or re-enable BinLoader.");
   return;
  }
  service_phase=SERVICE_CHECK_VERSION;service_check();return;
 }
 if(state.count<=0||focus<0||focus>=state.count)return;
 const struct bt_device*d=&state.devices[focus];int l=ui_link();
 if(d->address==state.selected&&l!=BT_LINK_IDLE){action("/api/stop");return;}
 char msg[128];snprintf(msg,sizeof(msg),"action: select %012lX \"%.48s\"",d->address,d->name);logline(msg);
 char path[64];snprintf(path,sizeof(path),"/api/select/%012lX",d->address);action(path);
}
static void toggle_payload(void){
 if(online){
  logline("action: unload payload");
  flush_app_logs_to_payload(1);
  int err=0;
  int code=http("POST","/api/quit",0,0,&err);
  if(code>=0&&code!=200){
   char msg[128];snprintf(msg,sizeof(msg),"unload: shutdown request failed (HTTP %d, err=%d)",code,err);logline(msg);
   set_notice("Could not unload audio payload.");
   refresh();
   return;
  }
  online=0;
  state.streaming=0;state.wanted=0;state.scanning=0;state.pin_pending=0;state.test=0;
  state.link=BT_LINK_IDLE;state.meter=0;meter_s=0;
  log_service_change();
  set_service_phase(SERVICE_UNLOADING);
  service_deadline=sceKernelGetProcessTime()+5000000;
  set_notice("Unloading audio payload...");notice_until=service_deadline;
  return;
 }
 if(service_phase==SERVICE_STOPPING||service_phase==SERVICE_STARTING)return;
 if(service_phase==SERVICE_UNLOADING){
  char version[64];
  if(read_payload_version(version,sizeof(version))!=SOCKET_REFUSED)return;
  set_service_phase(SERVICE_MONITOR);
 }
 if(last_launch_try&&g_now<last_launch_try+5000000){
  logline("toggle: BinLoader retry skipped (cooldown; BinLoader may only accept 1 payload per jailbreak)");
  set_notice("Service offline. Wait a moment or re-enable BinLoader.");
  return;
 }
 logline("action: start payload");
 set_service_phase(SERVICE_CHECK_VERSION);
 service_check();
}

static unsigned char snaprow[WIDTH*3];
static void snapshot(void){
 int fd=sceKernelOpen("/data/bta_audio/app-frame.ppm",0x601,0644);
 if(fd<0){logline("snapshot: open failed");return;}
 char hdr[32];int hl=snprintf(hdr,sizeof(hdr),"P6\n%d %d\n255\n",WIDTH,HEIGHT);sceKernelWrite(fd,hdr,(size_t)hl);
 for(int y=0;y<HEIGHT;y++){
  for(int x=0;x<WIDTH;x++){uint32_t v=back[y*WIDTH+x];snaprow[x*3]=(unsigned char)(v>>16);snaprow[x*3+1]=(unsigned char)(v>>8);snaprow[x*3+2]=(unsigned char)v;}
  sceKernelWrite(fd,snaprow,sizeof(snaprow));
 }
 sceKernelClose(fd);logline("snapshot: written");
}

static int g_upd_active,g_upd_ready,g_upd_result;
static char g_upd_latest[64],g_upd_log[160],g_upd_pkg_name[128],g_upd_pkg_url[512],last_upd_log[160];
static uint64_t g_upd_pkg_size;

static int g_dl_active,g_dl_state,g_dl_cancel,g_dl_finished,dl_modal_dismissed;
static uint64_t g_dl_bytes,g_dl_total;
static char g_dl_err[160],g_dl_log[160],g_dl_pkg_name[128],g_dl_pkg_url[512];

static int32_t skip_ssl_cb(int32_t libssl_id,uint32_t verify_err,void*const ssl_cert[],int32_t cert_num,void*user_arg){
 (void)libssl_id;(void)verify_err;(void)ssl_cert;(void)cert_num;(void)user_arg;
 return 1;
}

struct http_ctx{int net_id,ssl_id,http_id,tpl;};

static int open_http_ctx(struct http_ctx*ctx,const char*pool_name,uint32_t timeout_us,char*err,size_t err_cap){
 ctx->net_id=ctx->ssl_id=ctx->http_id=ctx->tpl=-1;
 if(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET)<0||
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP)<0||
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL)<0){
  snprintf(err,err_cap,"sysmodule load failed");
  return -1;
 }
 sceNetInit();
 ctx->net_id=sceNetPoolCreate(pool_name,16*1024,0);
 if(ctx->net_id<0){snprintf(err,err_cap,"sceNetPoolCreate=0x%x",ctx->net_id);return -1;}
 ctx->ssl_id=sceSslInit(SSL_POOLSIZE);
 if(ctx->ssl_id<0){snprintf(err,err_cap,"sceSslInit=0x%x",ctx->ssl_id);return -1;}
 ctx->http_id=sceHttpInit(ctx->net_id,ctx->ssl_id,LIBHTTP_POOLSIZE);
 if(ctx->http_id<0){snprintf(err,err_cap,"sceHttpInit=0x%x",ctx->http_id);return -1;}
 ctx->tpl=sceHttpCreateTemplate(ctx->http_id,"BTAudio4/" BTA_VERSION,ORBIS_HTTP_VERSION_1_1,1);
 if(ctx->tpl<0){snprintf(err,err_cap,"template=0x%x",ctx->tpl);return -1;}
 sceHttpSetResolveTimeOut(ctx->tpl,timeout_us);
 sceHttpSetConnectTimeOut(ctx->tpl,timeout_us);
 sceHttpSetSendTimeOut(ctx->tpl,timeout_us);
 sceHttpSetResponseHeaderMaxSize(ctx->tpl,16384);
 sceHttpsSetSslCallback(ctx->tpl,skip_ssl_cb,0);
 return 0;
}

static void close_http_ctx(struct http_ctx*ctx){
 if(ctx->tpl>=0)sceHttpDeleteTemplate(ctx->tpl);
 if(ctx->http_id>=0)sceHttpTerm(ctx->http_id);
 if(ctx->ssl_id>=0)sceSslTerm(ctx->ssl_id);
 if(ctx->net_id>=0)sceNetPoolDestroy(ctx->net_id);
 ctx->net_id=ctx->ssl_id=ctx->http_id=ctx->tpl=-1;
}

static size_t g_direct_done;
static int read_client_id_file(char uid_out[10],char legacy_out[17]){
 char buf[32]={0};
 int fd=sceKernelOpen("/data/bta_audio/client_id",0,0);
 if(fd<0)return 0;
 ssize_t n=sceKernelRead(fd,buf,sizeof(buf)-1);
 sceKernelClose(fd);
 if(n>0&&bta_uid_normalize(buf,(long)n,uid_out))return 1;
 if(n>0&&legacy_out&&!legacy_out[0])bta_hex16_normalize(buf,(long)n,legacy_out);
 return 0;
}
static void write_client_id_file(const char uid[10]){
 int fd=sceKernelOpen("/data/bta_audio/client_id.tmp",0x601,0644);
 if(fd>=0){
  ssize_t w=sceKernelWrite(fd,uid,BTA_UID_LEN);
  sceKernelClose(fd);
  if(w==BTA_UID_LEN)sceKernelRename("/data/bta_audio/client_id.tmp","/data/bta_audio/client_id");
 }
}
static void read_diag_url(char out[320],const char*route){
 const char*paths[2]={"/data/bta_audio/diag_url","/data/bta_audio/diag_url.txt"};
 char raw[256]={0};
 for(int i=0;i<2;i++){
  int fd=sceKernelOpen(paths[i],0,0);
  if(fd<0)continue;
  ssize_t n=sceKernelRead(fd,raw,sizeof(raw)-1);
  sceKernelClose(fd);
  if(n>0){
   raw[n]=0;
   const char*p=raw;
   if((unsigned char)p[0]==0xef&&(unsigned char)p[1]==0xbb&&(unsigned char)p[2]==0xbf)p+=3;
   while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
   if(*p){
    char clean[240];size_t j=0;
    while(p[j]&&p[j]!=' '&&p[j]!='\t'&&p[j]!='\r'&&p[j]!='\n'&&p[j]!='?'&&p[j]!='#'&&j+1<sizeof(clean)){clean[j]=p[j];j++;}
    while(j>0&&clean[j-1]=='/')j--;
    if(j>=9&&strcmp(clean+j-9,"/api/logs")==0)j-=9;
    clean[j]=0;
    if(j>0){
     const char*scheme=(strncmp(clean,"http://",7)==0||strncmp(clean,"https://",8)==0)?"":"http://";
     snprintf(out,320,"%s%s%s",scheme,clean,route);
     return;
    }
   }
  }
 }
 snprintf(out,320,"http://bta.alexutzu.pro%s",route);
}
static int fetch_uid_from_server(const char*legacy_hex,char uid_out[10],char*err,size_t err_cap){
 struct http_ctx ctx;
 int conn=-1,req=-1,ok=-1;
 char base[320]={0},url[512]={0};
 read_diag_url(base,"/api/uid");
 if(legacy_hex&&legacy_hex[0])snprintf(url,sizeof(url),"%s?version=%s&legacy=%s",base,BTA_VERSION,legacy_hex);
 else snprintf(url,sizeof(url),"%s?version=%s",base,BTA_VERSION);
 if(open_http_ctx(&ctx,"bta_uid",2500000,err,err_cap)<0)return -1;
 conn=sceHttpCreateConnectionWithURL(ctx.tpl,url,0);
 if(conn<0){snprintf(err,err_cap,"conn=0x%x",conn);goto done;}
 req=sceHttpCreateRequestWithURL(conn,ORBIS_METHOD_POST,url,0);
 if(req<0){snprintf(err,err_cap,"req=0x%x",req);goto done;}
 int rc=sceHttpSendRequest(req,0,0);
 if(rc<0){snprintf(err,err_cap,"send=0x%x",rc);goto done;}
 int32_t code=0;
 if(sceHttpGetStatusCode(req,&code)<0){snprintf(err,err_cap,"status err");goto done;}
 if(code<200||code>=300){snprintf(err,err_cap,"http=%d",(int)code);goto done;}
 char body[512];size_t total=0;
 while(total+1<sizeof(body)){
  int n=sceHttpReadData(req,body+total,(uint32_t)(sizeof(body)-1-total));
  if(n<=0)break;
  total+=(size_t)n;
 }
 body[total]=0;
 if(bta_extract_uid_json(body,uid_out))ok=0;
 else snprintf(err,err_cap,"invalid uid payload");
done:
 if(req>=0)sceHttpDeleteRequest(req);
 if(conn>=0)sceHttpDeleteConnection(conn);
 close_http_ctx(&ctx);
 return ok;
}
static void resolve_uid_sync(void){
 char uid[10]={0},err[128]={0};
 if(read_client_id_file(uid,g_legacy_hex)){
  snprintf(g_uid_buf,sizeof(g_uid_buf),"%s",uid);
  snprintf(g_uid_log,sizeof(g_uid_log),"uid: loaded %s",uid);
 }else if(fetch_uid_from_server(g_legacy_hex,uid,err,sizeof(err))==0){
  write_client_id_file(uid);
  snprintf(g_uid_buf,sizeof(g_uid_buf),"%s",uid);
  snprintf(g_uid_log,sizeof(g_uid_log),"uid: assigned %s by server",uid);
 }else{
  uint64_t x=sceKernelGetProcessTime()^((uint64_t)(uintptr_t)uid*0x9e3779b97f4a7c15ULL);
  x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;x*=0xc4ceb9fe1a85ec53ULL;x^=x>>33;
  bta_uid_from_u64((unsigned long long)x,uid);
  write_client_id_file(uid);
  snprintf(g_uid_buf,sizeof(g_uid_buf),"%s",uid);
  snprintf(g_uid_log,sizeof(g_uid_log),"uid: server unreachable (%s); local %s",err[0]?err:"err",uid);
 }
 __atomic_store_n(&g_uid_ready,1,__ATOMIC_RELEASE);
}
static void*uid_init_thread(void*arg){
 (void)arg;
 resolve_uid_sync();
 __atomic_store_n(&g_uid_active,0,__ATOMIC_RELEASE);
 return 0;
}
static void init_uid(void){
 char uid[10]={0};
 if(read_client_id_file(uid,g_legacy_hex)){
  snprintf(g_uid_buf,sizeof(g_uid_buf),"%s",uid);
  snprintf(client_uid,sizeof(client_uid),"%s",uid);
  __atomic_store_n(&g_uid_ready,1,__ATOMIC_RELEASE);
  g_uid_logged=1;
  char msg[64];snprintf(msg,sizeof(msg),"uid: loaded %s",uid);logline(msg);
  return;
 }
 g_uid_deadline=g_start+3500000ULL;
 __atomic_store_n(&g_uid_active,1,__ATOMIC_RELEASE);
 OrbisPthread th=0;
 if(scePthreadCreate(&th,0,uid_init_thread,0,"BTAudio4UID")==0){
  scePthreadDetach(th);
 }else{
  __atomic_store_n(&g_uid_active,0,__ATOMIC_RELEASE);
  resolve_uid_sync();
  snprintf(client_uid,sizeof(client_uid),"%s",g_uid_buf);
  g_uid_logged=1;
  if(g_uid_log[0])logline(g_uid_log);
 }
}
static void*direct_log_thread(void*arg){
 (void)arg;
 struct http_ctx ctx;
 int conn=-1,req=-1;
 char err[128]={0},cid[16]={0},base[320]={0},url[512]={0};
 if(!__atomic_load_n(&g_uid_ready,__ATOMIC_ACQUIRE)){
  if(__atomic_load_n(&g_uid_active,__ATOMIC_ACQUIRE)){
   for(int w=0;w<150&&!__atomic_load_n(&g_uid_ready,__ATOMIC_ACQUIRE);w++)sceKernelUsleep(20000);
  }
  if(!__atomic_load_n(&g_uid_ready,__ATOMIC_ACQUIRE))resolve_uid_sync();
 }
 snprintf(cid,sizeof(cid),"%s",g_uid_buf);
 read_diag_url(base,"/api/logs");
 if(!app_session_id[0]){
  uint64_t x=sceKernelGetProcessTime()^((uint64_t)(uintptr_t)&ctx*0x9e3779b97f4a7c15ULL);
  x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;
  snprintf(app_session_id,sizeof(app_session_id),"app-%012llx",(unsigned long long)(x&0xffffffffffffULL));
 }
 snprintf(url,sizeof(url),"%s?client=%s&session=%s&seq=%u&offset=%llu&version=%s&kind=app&dropped=%llu",
  base,cid,app_session_id,app_direct_seq,(unsigned long long)app_direct_offset,BTA_VERSION,(unsigned long long)app_dropped);
 size_t len=g_direct_len;
 g_direct_err[0]=0;
 if(len>0&&open_http_ctx(&ctx,"bta_applog",2000000,err,sizeof(err))==0){
  conn=sceHttpCreateConnectionWithURL(ctx.tpl,url,0);
  if(conn>=0){
   req=sceHttpCreateRequestWithURL(conn,ORBIS_METHOD_POST,url,(uint64_t)len);
   if(req>=0){
    sceHttpAddRequestHeader(req,"Content-Type","text/plain; charset=utf-8",0);
    int src=sceHttpSendRequest(req,g_direct_buf,len);
    if(src>=0){
     int32_t code=0;
     int grc=sceHttpGetStatusCode(req,&code);
     if(grc>=0&&code>=200&&code<300){
      app_direct_seq++;
      app_direct_offset+=len;
      __atomic_store_n(&g_direct_done,len,__ATOMIC_RELEASE);
     }else{
      snprintf(g_direct_err,sizeof(g_direct_err),"diag_upload: http=%d grc=0x%x",(int)code,grc);
     }
    }else{
     snprintf(g_direct_err,sizeof(g_direct_err),"diag_upload: send=0x%x",src);
    }
   }else{
    snprintf(g_direct_err,sizeof(g_direct_err),"diag_upload: req=0x%x",req);
   }
  }else{
   snprintf(g_direct_err,sizeof(g_direct_err),"diag_upload: conn=0x%x",conn);
  }
  if(req>=0)sceHttpDeleteRequest(req);
  if(conn>=0)sceHttpDeleteConnection(conn);
  close_http_ctx(&ctx);
 }else if(len>0){
  snprintf(g_direct_err,sizeof(g_direct_err),"diag_upload: ctx (%s)",err);
 }
 __atomic_store_n(&g_direct_busy,0,__ATOMIC_RELEASE);
 return 0;
}
static void trigger_direct_log_upload(int sync){
 if(app_head==app_tail||__atomic_load_n(&g_direct_busy,__ATOMIC_ACQUIRE))return;
 if(!sync&&g_now<app_direct_at)return;
 size_t len=app_ring_peek(g_direct_buf,sizeof(g_direct_buf));
 if(!len)return;
 g_direct_len=len;
 __atomic_store_n(&g_direct_done,0,__ATOMIC_RELEASE);
 __atomic_store_n(&g_direct_busy,1,__ATOMIC_RELEASE);
 app_direct_at=g_now+15000000;
 if(sync){
  direct_log_thread(0);
  size_t done=__atomic_exchange_n(&g_direct_done,0,__ATOMIC_ACQ_REL);
  if(done<=app_head-app_tail)app_tail+=done;
  return;
 }
 OrbisPthread th=0;
 if(scePthreadCreate(&th,0,direct_log_thread,0,"BTAudio4Diag")==0)scePthreadDetach(th);
 else __atomic_store_n(&g_direct_busy,0,__ATOMIC_RELEASE);
}

static int http_get_follow(int tpl,const char*start_url,const char*accept_hdr,int*conn_out,int*req_out,int32_t*status_out,char*err,size_t err_cap){
 char url[2048];
 snprintf(url,sizeof(url),"%s",start_url);
 *conn_out=-1;*req_out=-1;*status_out=0;

 for(int hop=0;hop<6;hop++){
  int conn=sceHttpCreateConnectionWithURL(tpl,url,0);
  if(conn<0){snprintf(err,err_cap,"Connection failed (hop=%d rc=0x%x)",hop,conn);return -1;}
  int req=sceHttpCreateRequestWithURL(conn,ORBIS_METHOD_GET,url,0);
  if(req<0){sceHttpDeleteConnection(conn);snprintf(err,err_cap,"Request failed (hop=%d rc=0x%x)",hop,req);return -1;}
  if(accept_hdr&&strncmp(url,"https://api.github.com/",23)==0)sceHttpAddRequestHeader(req,"Accept",accept_hdr,0);

  int rc=sceHttpSendRequest(req,0,0);
  if(rc<0){sceHttpDeleteRequest(req);sceHttpDeleteConnection(conn);snprintf(err,err_cap,"Send failed (hop=%d rc=0x%x)",hop,rc);return -1;}
  int32_t code=0;
  rc=sceHttpGetStatusCode(req,&code);
  if(rc<0){sceHttpDeleteRequest(req);sceHttpDeleteConnection(conn);snprintf(err,err_cap,"Status read failed (hop=%d rc=0x%x)",hop,rc);return -1;}

  if(code==301||code==302||code==303||code==307||code==308){
   char*hdrs=0;size_t hlen=0;char next_url[2048];
   int hrc=sceHttpGetAllResponseHeaders(req,&hdrs,&hlen);
   int got=(hrc>=0&&hdrs&&bta_extract_location_header(hdrs,hlen,next_url,sizeof(next_url))==0);
   sceHttpDeleteRequest(req);sceHttpDeleteConnection(conn);
   if(!got){snprintf(err,err_cap,"Redirect missing Location header (hop=%d)",hop);return -1;}
   snprintf(url,sizeof(url),"%s",next_url);
   continue;
  }

  *conn_out=conn;*req_out=req;*status_out=code;
  return 0;
 }
 snprintf(err,err_cap,"Too many redirects");
 return -1;
}

static void*update_check_thread(void*arg){
 (void)arg;
 int res=BTA_UPD_NONE;
 struct http_ctx ctx;
 int conn=-1,req=-1;
 char*buf=0;
 char err[128]={0};
 g_upd_latest[0]=0;
 g_upd_log[0]=0;
 g_upd_pkg_name[0]=0;
 g_upd_pkg_url[0]=0;
 g_upd_pkg_size=0;

 if(open_http_ctx(&ctx,"bta_upd",5000000,err,sizeof(err))<0){
  snprintf(g_upd_log,sizeof(g_upd_log),"update: %s",err);
  goto done;
 }

 int32_t status_code=0;
 if(http_get_follow(ctx.tpl,BTA_GITHUB_LATEST_URL,"application/vnd.github+json",&conn,&req,&status_code,err,sizeof(err))<0){
  snprintf(g_upd_log,sizeof(g_upd_log),"update: %s",err);
  goto done;
 }
 if(status_code!=200){snprintf(g_upd_log,sizeof(g_upd_log),"update: http=%d",(int)status_code);goto done;}

 buf=malloc(65536);
 if(!buf){snprintf(g_upd_log,sizeof(g_upd_log),"update: oom");goto done;}
 size_t total=0;
 while(total+1<65536){
  int n=sceHttpReadData(req,buf+total,(uint32_t)(65535-total));
  if(n<=0)break;
  total+=(size_t)n;
 }
 buf[total]=0;

 char tag[64];
 if(bta_extract_tag_name(buf,total,tag,sizeof(tag))<0){
  snprintf(g_upd_log,sizeof(g_upd_log),"update: tag_name missing (%zu bytes)",total);
  goto done;
 }
 bta_format_v(tag,g_upd_latest,sizeof(g_upd_latest));
 int cmp=bta_compare_versions(tag,BTA_VERSION);
 if(cmp>0){
  res=BTA_UPD_OUTDATED;
  unsigned long long psz=0;
  bta_extract_pkg_asset(buf,total,g_upd_pkg_name,sizeof(g_upd_pkg_name),g_upd_pkg_url,sizeof(g_upd_pkg_url),&psz);
  g_upd_pkg_size=(uint64_t)psz;
 }else if(cmp==-1)res=BTA_UPD_MISMATCH;
 else if(cmp==0)res=BTA_UPD_LATEST;
 snprintf(g_upd_log,sizeof(g_upd_log),"update: current=v%s latest=%s state=%d pkg=%s",BTA_VERSION,g_upd_latest,res,g_upd_pkg_name[0]?g_upd_pkg_name:"none");

done:
 free(buf);
 if(req>=0)sceHttpDeleteRequest(req);
 if(conn>=0)sceHttpDeleteConnection(conn);
 close_http_ctx(&ctx);
 g_upd_result=res;
 __atomic_store_n(&g_upd_ready,1,__ATOMIC_RELEASE);
 __atomic_store_n(&g_upd_active,0,__ATOMIC_RELEASE);
 return 0;
}

static void start_update_check(void){
 if(__atomic_load_n(&g_upd_active,__ATOMIC_ACQUIRE)||__atomic_load_n(&g_dl_active,__ATOMIC_ACQUIRE))return;
 __atomic_store_n(&g_upd_ready,0,__ATOMIC_RELEASE);
 __atomic_store_n(&g_upd_active,1,__ATOMIC_RELEASE);
 OrbisPthread th=0;
 int rc=scePthreadCreate(&th,0,update_check_thread,0,"BTAudio4Update");
 if(rc==0)scePthreadDetach(th);
 else{__atomic_store_n(&g_upd_active,0,__ATOMIC_RELEASE);logline("update: thread start failed");}
}

static void*pkg_download_thread(void*arg){
 (void)arg;
 struct http_ctx ctx;
 int conn=-1,req=-1,fd=-1,final_state=BTA_DL_ERROR;
 char*chunk=0;
 char tmp_path[256],final_path[256],err[160]={0};
 snprintf(tmp_path,sizeof(tmp_path),"/data/pkg/%s.part",g_dl_pkg_name);
 snprintf(final_path,sizeof(final_path),"/data/pkg/%s",g_dl_pkg_name);
 g_dl_err[0]=0;
 g_dl_log[0]=0;

 if(open_http_ctx(&ctx,"bta_dl",10000000,err,sizeof(err))<0){
  snprintf(g_dl_err,sizeof(g_dl_err),"Network initialization failed (%s).",err);
  goto done;
 }

 int32_t status_code=0;
 if(http_get_follow(ctx.tpl,g_dl_pkg_url,"application/octet-stream",&conn,&req,&status_code,err,sizeof(err))<0){
  snprintf(g_dl_err,sizeof(g_dl_err),"Could not connect to GitHub (%s).",err);
  goto done;
 }
 if(status_code!=200){
  snprintf(g_dl_err,sizeof(g_dl_err),"GitHub returned HTTP %d.",(int)status_code);
  goto done;
 }

 int len_type=0;uint64_t content_len=0;
 if(sceHttpGetResponseContentLength(req,&len_type,&content_len)>=0&&len_type==ORBIS_HTTP_CONTENTLEN_EXIST&&content_len>0){
  __atomic_store_n(&g_dl_total,content_len,__ATOMIC_RELAXED);
 }

 sceKernelMkdir("/data/pkg",0777);
 fd=sceKernelOpen(tmp_path,0x601,0644);
 if(fd<0){
  snprintf(g_dl_err,sizeof(g_dl_err),"Could not create %s (0x%x).",tmp_path,fd);
  goto done;
 }

 chunk=malloc(32768);
 if(!chunk){snprintf(g_dl_err,sizeof(g_dl_err),"Out of memory.");goto done;}

 uint64_t written=0;
 int cancelled=0,read_err=0,write_err=0;
 for(;;){
  if(__atomic_load_n(&g_dl_cancel,__ATOMIC_ACQUIRE)){cancelled=1;break;}
  int n=sceHttpReadData(req,chunk,32768);
  if(n<0){read_err=n;break;}
  if(n==0)break;
  size_t off=0;
  while(off<(size_t)n){
   ssize_t w=(ssize_t)sceKernelWrite(fd,chunk+off,(size_t)n-off);
   if(w<=0){write_err=(int)w;break;}
   off+=(size_t)w;
  }
  if(write_err)break;
  written+=(uint64_t)n;
  __atomic_store_n(&g_dl_bytes,written,__ATOMIC_RELAXED);
 }

 sceKernelClose(fd);fd=-1;
 if(cancelled){
  sceKernelUnlink(tmp_path);
  final_state=BTA_DL_NONE;
  snprintf(g_dl_log,sizeof(g_dl_log),"download: cancelled");
  goto done;
 }
 if(read_err<0){
  sceKernelUnlink(tmp_path);
  snprintf(g_dl_err,sizeof(g_dl_err),"Download interrupted (0x%x).",read_err);
  goto done;
 }
 if(write_err){
  sceKernelUnlink(tmp_path);
  snprintf(g_dl_err,sizeof(g_dl_err),"Disk write failed (%d).",write_err);
  goto done;
 }
 if(written<1024||(len_type==ORBIS_HTTP_CONTENTLEN_EXIST&&content_len>0&&written!=content_len)){
  sceKernelUnlink(tmp_path);
  snprintf(g_dl_err,sizeof(g_dl_err),"Incomplete package downloaded (%llu bytes).",(unsigned long long)written);
  goto done;
 }

 sceKernelUnlink(final_path);
 int rrc=sceKernelRename(tmp_path,final_path);
 if(rrc<0){
  sceKernelUnlink(tmp_path);
  snprintf(g_dl_err,sizeof(g_dl_err),"Could not save %s (0x%x).",final_path,rrc);
  goto done;
 }

 final_state=BTA_DL_DONE;
 snprintf(g_dl_log,sizeof(g_dl_log),"download: saved %s (%llu bytes)",final_path,(unsigned long long)written);

done:
 if(fd>=0){sceKernelClose(fd);sceKernelUnlink(tmp_path);}
 free(chunk);
 if(req>=0)sceHttpDeleteRequest(req);
 if(conn>=0)sceHttpDeleteConnection(conn);
 close_http_ctx(&ctx);
 if(final_state==BTA_DL_ERROR&&!g_dl_log[0])snprintf(g_dl_log,sizeof(g_dl_log),"download: error: %s",g_dl_err);
 __atomic_store_n(&g_dl_state,final_state,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_finished,1,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_active,0,__ATOMIC_RELEASE);
 return 0;
}

static void start_pkg_download(void){
 if(!g_upd_pkg_url[0]||!g_upd_pkg_name[0])return;
 if(__atomic_load_n(&g_dl_active,__ATOMIC_ACQUIRE))return;
 dl_modal_dismissed=0;
 snprintf(g_dl_pkg_name,sizeof(g_dl_pkg_name),"%s",g_upd_pkg_name);
 snprintf(g_dl_pkg_url,sizeof(g_dl_pkg_url),"%s",g_upd_pkg_url);
 __atomic_store_n(&g_dl_cancel,0,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_finished,0,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_bytes,0,__ATOMIC_RELAXED);
 __atomic_store_n(&g_dl_total,g_upd_pkg_size,__ATOMIC_RELAXED);
 __atomic_store_n(&g_dl_state,BTA_DL_RUNNING,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_active,1,__ATOMIC_RELEASE);
 snprintf(dl_pkg_name,sizeof(dl_pkg_name),"%s",g_upd_pkg_name);
 dl_bytes=0;
 dl_total=g_upd_pkg_size;
 dl_err[0]=0;
 dl_state=BTA_DL_RUNNING;

 OrbisPthread th=0;
 int rc=scePthreadCreate(&th,0,pkg_download_thread,0,"BTAudio4Download");
 if(rc==0){scePthreadDetach(th);logline("download: started");}
 else{
  __atomic_store_n(&g_dl_active,0,__ATOMIC_RELEASE);
  __atomic_store_n(&g_dl_state,BTA_DL_ERROR,__ATOMIC_RELEASE);
  snprintf(dl_err,sizeof(dl_err),"Could not start download thread (0x%x).",rc);
  dl_state=BTA_DL_ERROR;
  logline("download: thread start failed");
 }
}

int main(void){
 sceKernelMkdir("/data/bta_audio",0777);sceKernelChmod("/data/bta_audio",0777);log_rotate();g_start=sceKernelGetProcessTime();g_now=g_start;logline("BTAudio4 " BTA_VERSION " native app starting");init_uid();log_startup_env();
 int video=sceVideoOutOpen(0xff,0,0,0);if(video<0){char m[64];snprintf(m,sizeof(m),"VideoOutOpen failed (0x%x)",video);logline(m);trigger_direct_log_upload(1);return 1;}
 off_t memory;size_t size=8*1024*1024;void*base=0;
 int dm_rc=sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),size,0x200000,3,&memory);
 int mp_rc=dm_rc>=0?sceKernelMapDirectMemory(&base,size,0x33,0,memory,0x200000):0;
 if(dm_rc<0||mp_rc<0){char m[96];snprintf(m,sizeof(m),"Video memory failed (alloc=0x%x map=0x%x)",dm_rc,mp_rc);logline(m);trigger_direct_log_upload(1);return 2;}
 void*buffers[2]={base,(char*)base+WIDTH*HEIGHT*4};OrbisVideoOutBufferAttribute attr;
 sceVideoOutSetBufferAttribute(&attr,0x80000000,1,0,WIDTH,HEIGHT,WIDTH);
 int rb_rc=sceVideoOutRegisterBuffers(video,0,buffers,2,&attr);
 if(rb_rc<0){char m[64];snprintf(m,sizeof(m),"Register buffers failed (0x%x)",rb_rc);logline(m);trigger_direct_log_upload(1);return 3;}
 sceVideoOutSetFlipRate(video,0);
 OrbisUserServiceInitializeParams userparams={ORBIS_KERNEL_PRIO_FIFO_LOWEST};
 int user_rc=sceUserServiceInitialize(&userparams);int pad_rc=scePadInit();
 char diag[128];snprintf(diag,sizeof(diag),"User init=0x%x Pad init=0x%x",user_rc,pad_rc);logline(diag);
 logline("Checking audio service");service_check();
 start_update_check();
 logline("Starting display loop");
 focus=0;for(int i=0;i<state.count;i++)if(state.devices[i].address==state.selected)focus=i;
 unsigned previous=0,init_held=0;int buffer=0,armed=0;long flip=0;uint64_t poll=0,repeat=0,first_draw=0,snap_cd=0,next_upd=sceKernelGetProcessTime()+60000000ULL;int pinmode=0,pinignore=0,digit=0,auto_snapped=0;char pin[5]="0000";
 for(;;){
  uint64_t now=sceKernelGetProcessTime();g_now=now;
  if(!g_uid_logged&&__atomic_load_n(&g_uid_ready,__ATOMIC_ACQUIRE)){
   g_uid_logged=1;
   snprintf(client_uid,sizeof(client_uid),"%s",g_uid_buf);
   if(g_uid_log[0])logline(g_uid_log);
  }
  if(!__atomic_load_n(&g_direct_busy,__ATOMIC_ACQUIRE)){
   size_t done=__atomic_exchange_n(&g_direct_done,0,__ATOMIC_ACQ_REL);
   if(done>0&&done<=app_head-app_tail)app_tail+=done;
   else if(g_direct_err[0]){char dm[180];snprintf(dm,sizeof(dm),"%s",g_direct_err);g_direct_err[0]=0;logline(dm);}
  }
  if(now>=next_upd){next_upd=now+60000000ULL;start_update_check();}
  if(__atomic_exchange_n(&g_upd_ready,0,__ATOMIC_ACQ_REL)){
   if(g_upd_result!=BTA_UPD_NONE){
    snprintf(upd_latest,sizeof(upd_latest),"%s",g_upd_latest);
    upd_state=g_upd_result;
    upd_has_pkg=(g_upd_pkg_url[0]!=0);
    if(g_upd_pkg_name[0]&&!__atomic_load_n(&g_dl_active,__ATOMIC_ACQUIRE))snprintf(dl_pkg_name,sizeof(dl_pkg_name),"%s",g_upd_pkg_name);
   }
   if(g_upd_log[0]&&strcmp(g_upd_log,last_upd_log)!=0){snprintf(last_upd_log,sizeof(last_upd_log),"%s",g_upd_log);logline(g_upd_log);}
  }
  if(!dl_modal_dismissed&&(__atomic_load_n(&g_dl_active,__ATOMIC_ACQUIRE)||__atomic_load_n(&g_dl_finished,__ATOMIC_ACQUIRE))){
   dl_bytes=__atomic_load_n(&g_dl_bytes,__ATOMIC_RELAXED);
   dl_total=__atomic_load_n(&g_dl_total,__ATOMIC_RELAXED);
   dl_state=__atomic_load_n(&g_dl_state,__ATOMIC_ACQUIRE);
  }
  if(__atomic_exchange_n(&g_dl_finished,0,__ATOMIC_ACQ_REL)){
   snprintf(dl_err,sizeof(dl_err),"%s",g_dl_err);
   if(!dl_modal_dismissed)dl_state=__atomic_load_n(&g_dl_state,__ATOMIC_ACQUIRE);
   if(g_dl_log[0])logline(g_dl_log);
  }
  if(now>=poll){
   service_check();
   if(!online&&app_head!=app_tail)trigger_direct_log_upload(0);
   uint64_t interval=2000000;
   if(!online&&!__atomic_load_n(&g_uid_ready,__ATOMIC_ACQUIRE)&&now<g_uid_deadline)interval=100000;
   else if(service_phase==SERVICE_STOPPING||service_phase==SERVICE_STARTING||service_phase==SERVICE_UNLOADING)interval=150000;
   else if(online)interval=ui_link()==BT_LINK_STREAMING?300000:500000;
   poll=sceKernelGetProcessTime()+interval;
  }
  int l=ui_link();
  if(!online||!state.pin_pending){pinmode=0;pinignore=0;}
  if(online&&state.pin_pending&&!pinmode&&!pinignore){pinmode=1;digit=0;memcpy(pin,"0000",5);}
  unsigned buttons=controller(now);
  /* A button still held from launching or resuming the app is not a new press. */
  if(!pad_connected){armed=0;init_held=0;}
  if(!armed&&pad_connected){previous=buttons;init_held=buttons;armed=1;}
  init_held&=buttons;
  unsigned pressed=buttons&~previous;
  if(buttons==previous&&now>=repeat){pressed=buttons&(ORBIS_PAD_BUTTON_UP|ORBIS_PAD_BUTTON_DOWN|ORBIS_PAD_BUTTON_LEFT|ORBIS_PAD_BUTTON_RIGHT|ORBIS_PAD_BUTTON_L1|ORBIS_PAD_BUTTON_R1);repeat=now+150000;}
  else if(buttons!=previous)repeat=now+450000;
  previous=buttons;
  if(!armed)pressed=0;
  g_held=armed?(buttons&~init_held):0;
  if(pinmode){
   if(pressed&ORBIS_PAD_BUTTON_LEFT)digit=(digit+3)%4;if(pressed&ORBIS_PAD_BUTTON_RIGHT)digit=(digit+1)%4;
   if(pressed&ORBIS_PAD_BUTTON_UP)pin[digit]=(char)('0'+(pin[digit]-'0'+1)%10);if(pressed&ORBIS_PAD_BUTTON_DOWN)pin[digit]=(char)('0'+(pin[digit]-'0'+9)%10);
   if(pressed&ORBIS_PAD_BUTTON_CIRCLE){pinmode=0;pinignore=1;}
   if(pressed&ORBIS_PAD_BUTTON_CROSS){char path[40];snprintf(path,sizeof(path),"/api/pin/%s",pin);logline("action: pin");action(path);pinmode=0;pinignore=1;}
  }else if(dl_state==BTA_DL_RUNNING){
   if(pressed&ORBIS_PAD_BUTTON_CIRCLE){__atomic_store_n(&g_dl_cancel,1,__ATOMIC_RELEASE);dl_state=BTA_DL_NONE;dl_modal_dismissed=1;logline("download: cancel requested");}
  }else if(dl_state==BTA_DL_DONE||dl_state==BTA_DL_ERROR){
   int prev_dl=dl_state;
   if(pressed&(ORBIS_PAD_BUTTON_CROSS|ORBIS_PAD_BUTTON_CIRCLE)){dl_state=BTA_DL_NONE;dl_modal_dismissed=1;}
   else if(prev_dl==BTA_DL_ERROR&&(pressed&ORBIS_PAD_BUTTON_R2)){start_pkg_download();}
  }else{
   if(pressed&ORBIS_PAD_BUTTON_CIRCLE)break;
   if((pressed&ORBIS_PAD_BUTTON_R2)&&upd_state==BTA_UPD_OUTDATED&&upd_has_pkg){start_pkg_download();}
   if(pressed&ORBIS_PAD_BUTTON_L2){toggle_payload();if(service_phase==SERVICE_UNLOADING)poll=now+150000;}
   if(state.count>0){
    if(pressed&ORBIS_PAD_BUTTON_DOWN)focus=(focus+1)%state.count;
    if(pressed&ORBIS_PAD_BUTTON_UP)focus=(focus+state.count-1)%state.count;
   }
   if(pressed&ORBIS_PAD_BUTTON_CROSS){logline("action: connect/disconnect");activate();}
   if(pressed&ORBIS_PAD_BUTTON_TRIANGLE){if(!online){logline("action: scan ignored (service offline)");}else if(l!=BT_LINK_IDLE){logline("action: disconnect");action("/api/stop");}else{logline("action: scan");action("/api/scan");}}
   if(pressed&ORBIS_PAD_BUTTON_SQUARE){if(!online){logline("action: tv ignored (service offline)");}else{logline("action: tv");action(state.tv?"/api/tv-off":"/api/tv-on");}}
   if(pressed&ORBIS_PAD_BUTTON_OPTIONS){if(!online){logline("action: test ignored (service offline)");}else{logline("action: test sound");action("/api/test");if(!notice[0])testing_until=g_now+2200000;}}
   if(pressed&(ORBIS_PAD_BUTTON_L1|ORBIS_PAD_BUTTON_LEFT))volume_change(-5);
   if(pressed&(ORBIS_PAD_BUTTON_R1|ORBIS_PAD_BUTTON_RIGHT))volume_change(5);
  }
  if(focus<0||focus>=state.count)focus=0;
  if(focus<top)top=focus;if(focus>=top+5)top=focus-4;if(top<0)top=0;
  {int target=(ext_ok&&ui_link()==BT_LINK_STREAMING)?state.meter:0;if(target<0)target=0;if(target>100)target=100;
   if(target>meter_s)meter_s+=(target-meter_s+2)/4;else meter_s-=(meter_s-target+7)/8;}
  g_now=sceKernelGetProcessTime();
  if(notice[0]&&g_now>=notice_until)notice[0]=0;
  draw(pinmode,pin,digit);
  memcpy(buffers[buffer],back,sizeof(back));
  if(!first_draw)first_draw=now;
  if(!auto_snapped&&now-first_draw>2000000){snapshot();auto_snapped=1;}
  if((buttons&(ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3))==(ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3)&&now>=snap_cd){snapshot();snap_cd=now+2000000;}
  if(sceVideoOutSubmitFlip(video,buffer,1,flip)<0){sceKernelUsleep(16000);continue;}
  if(flip==0)sceSystemServiceHideSplashScreen();
  for(int tries=0;tries<1000;tries++){OrbisVideoOutFlipStatus status;if(!sceVideoOutGetFlipStatus(video,&status)&&status.flipArg==flip)break;sceKernelUsleep(1000);}
  buffer^=1;flip++;
 }
 logline("Native app closed; audio service remains running");
 if(online)flush_app_logs_to_payload(1);
 if(!online&&app_head!=app_tail){
  for(int w=0;w<50&&__atomic_load_n(&g_direct_busy,__ATOMIC_ACQUIRE);w++)sceKernelUsleep(20000);
  size_t done=__atomic_exchange_n(&g_direct_done,0,__ATOMIC_ACQ_REL);
  if(done>0&&done<=app_head-app_tail)app_tail+=done;
  trigger_direct_log_upload(1);
 }
 if(pad>=0)scePadClose(pad);
 sceVideoOutUnregisterBuffers(video,0);
 sceVideoOutClose(video);
 sceKernelMunmap(base,size);
 sceKernelReleaseDirectMemory(memory,size);
 int exit_rc=sceSystemServiceLoadExec("exit",0);
 if(exit_rc>=0){for(;;)sceKernelUsleep(100000);}
 {char msg[64];snprintf(msg,sizeof(msg),"exit failed: sceSystemServiceLoadExec=0x%x",exit_rc);logline(msg);}
 return 0;
}

/* Screen drawing for the native app. Pure C with no console dependencies, so the
 * PC harness (native/host_ui.c) can run this exact code and diff it pixel-for-pixel
 * against the preview renderer. Included by main.c. */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../payload/protocol.h"
#include "../version.h"
#include "version_check.h"
#include "ui_font.h"
#include "ui_icons.h"

#define WIDTH 1280
#define HEIGHT 720
#define UI_OFFLINE 6

#define BG      0x09090b
#define PANEL   0x111113
#define BORDER  0x222226
#define TRACK   0x3f3f46
#define TEXT    0xfafafa
#define MUTED   0xa4a4ab

static uint32_t back[WIDTH*HEIGHT]; /* cached off-screen target; the video buffers are slow to read back */
static struct bt_native state;
static int online,ext_ok,focus,top,meter_s,upd_state,upd_has_pkg,dl_state;
static uint64_t g_now,testing_until,notice_until,dl_bytes,dl_total;
static char notice[160],upd_latest[64],dl_pkg_name[128],dl_err[160];

/* ---- drawing primitives. The PC previewer mirrors these exactly (integer math). ---- */
static int cx0,cy0,cx1=WIDTH,cy1=HEIGHT;
static void clip(int x0,int y0,int x1,int y1){cx0=x0<0?0:x0;cy0=y0<0?0:y0;cx1=x1>WIDTH?WIDTH:x1;cy1=y1>HEIGHT?HEIGHT:y1;}
static void blend(int x,int y,uint32_t rgb,unsigned a){
 if(!a||x<cx0||x>=cx1||y<cy0||y>=cy1)return;
 uint32_t*p=&back[y*WIDTH+x];
 if(a>=255){*p=0xff000000u|rgb;return;}
 uint32_t bg=*p;unsigned ia=255-a;
 unsigned r=(((rgb>>16)&255)*a+((bg>>16)&255)*ia+127)/255;
 unsigned g=(((rgb>>8)&255)*a+((bg>>8)&255)*ia+127)/255;
 unsigned b=((rgb&255)*a+(bg&255)*ia+127)/255;
 *p=0xff000000u|(r<<16)|(g<<8)|b;
}
static void fill(int x,int y,int w,int h,uint32_t rgb){
 int x0=x>cx0?x:cx0,y0=y>cy0?y:cy0,x1=x+w<cx1?x+w:cx1,y1=y+h<cy1?y+h:cy1;
 if(x1<=x0||y1<=y0)return;
 uint32_t v=0xff000000u|rgb;
 for(int j=y0;j<y1;j++){uint32_t*row=back+j*WIDTH;for(int i=x0;i<x1;i++)row[i]=v;}
}
/* Coverage (0..255) of pixel (cx,cy) in a top-left corner of radius r: 4x4 supersampling. */
static unsigned corner_cov(int r,int cx,int cy){
 int R8=r*8,n=0;
 for(int t=1;t<8;t+=2)for(int s=1;s<8;s+=2){int dx=8*cx+s-R8,dy=8*cy+t-R8;if(dx*dx+dy*dy<=R8*R8)n++;}
 return (unsigned)((n*255+8)/16);
}
static void rrect(int x,int y,int w,int h,int r,uint32_t rgb){
 if(w<=0||h<=0)return;
 if(r>w/2)r=w/2;if(r>h/2)r=h/2;
 if(r<=0){fill(x,y,w,h,rgb);return;}
 fill(x+r,y,w-2*r,h,rgb);fill(x,y+r,r,h-2*r,rgb);fill(x+w-r,y+r,r,h-2*r,rgb);
 for(int cy=0;cy<r;cy++)for(int cx=0;cx<r;cx++){
  unsigned a=corner_cov(r,cx,cy);if(!a)continue;
  blend(x+cx,y+cy,rgb,a);blend(x+w-1-cx,y+cy,rgb,a);blend(x+cx,y+h-1-cy,rgb,a);blend(x+w-1-cx,y+h-1-cy,rgb,a);
 }
}
static void disc(int cx,int cy,int r,uint32_t rgb,unsigned alpha){
 if(alpha>=255){rrect(cx-r,cy-r,2*r,2*r,r,rgb);return;}
 for(int j=0;j<2*r;j++)for(int i=0;i<2*r;i++){
  int qx=i<r?i:2*r-1-i,qy=j<r?j:2*r-1-j;
  blend(cx-r+i,cy-r+j,rgb,corner_cov(r,qx,qy)*alpha/255);
 }
}
static void icon(int id,int x,int y,uint32_t rgb,unsigned scale){
 const struct icon*ic=&ICONS[id];
 for(int j=0;j<ic->h;j++)for(int i=0;i<ic->w;i++){unsigned a=ic->a[j*ic->w+i];if(a)blend(x+i,y+j,rgb,a*scale/255);}
}
static void icon_c(int id,int cx,int cy,uint32_t rgb,unsigned scale){icon(id,cx-ICONS[id].w/2,cy-ICONS[id].h/2,rgb,scale);}
static void logo(int x,int y){
 for(int j=0;j<LOGO_H;j++)for(int i=0;i<LOGO_W;i++){unsigned p=logo_argb[j*LOGO_W+i];unsigned a=p>>24;if(a)blend(x+i,y+j,p&0xffffff,a);}
}
static void dim_frame(unsigned a){
 unsigned ia=255-a;
 for(int i=0;i<WIDTH*HEIGHT;i++){uint32_t v=back[i];back[i]=0xff000000u|((((v>>16)&255)*ia+127)/255<<16)|((((v>>8)&255)*ia+127)/255<<8)|(((v&255)*ia+127)/255);}
}

/* ---- text: proportional anti-aliased glyphs, 1/16 px advances ---- */
static int gl_index(unsigned char c){return(c<32||c>126)?63-32:c-32;}
static int text_w(int f,const char*s){
 const struct face*fc=&FACES[f];unsigned tot=0;
 for(;*s;s++)tot+=fc->g[gl_index((unsigned char)*s)].adv16;
 return(int)((tot+8)>>4);
}
static void text(int f,int x,int base,const char*s,uint32_t rgb){
 const struct face*fc=&FACES[f];int pen=x*16;
 for(;*s;s++){
  const struct glyph*g=&fc->g[gl_index((unsigned char)*s)];
  int gx=((pen+8)>>4)+g->bx,gy=base+g->by;
  for(int j=0;j<g->h;j++)for(int i=0;i<g->w;i++){unsigned a=fc->bits[g->off+j*g->w+i];if(a)blend(gx+i,gy+j,rgb,a);}
  pen+=g->adv16;
 }
}
#define TBUF 272
/* Returns s if it fits maxw, else a copy in buf shortened with "...". */
static const char*fit(int f,const char*s,int maxw,char*buf){
 if(text_w(f,s)<=maxw)return s;
 int dots=text_w(f,"...");size_t len=strlen(s);if(len>TBUF-8)len=TBUF-8;
 unsigned pref[TBUF];unsigned acc=0;pref[0]=0;
 for(size_t i=0;i<len;i++){acc+=FACES[f].g[gl_index((unsigned char)s[i])].adv16;pref[i+1]=acc;}
 size_t n=len;while(n>0&&(int)((pref[n]+8)>>4)+dots>maxw)n--;
 while(n>0&&s[n-1]==' ')n--;
 memcpy(buf,s,n);memcpy(buf+n,"...",4);return buf;
}
static void text_fit(int f,int x,int base,const char*s,uint32_t rgb,int maxw){char b[TBUF];text(f,x,base,fit(f,s,maxw,b),rgb);}
static void text_r(int f,int rx,int base,const char*s,uint32_t rgb){text(f,rx-text_w(f,s),base,s,rgb);}
/* Greedy wrap to at most two lines (second line ellipsised if text remains). */
static int wrap2(int f,const char*s,int maxw,char l1[TBUF],char l2[TBUF]){
 char lines[3][TBUF];int nl=0,more=0;char cur[TBUF];cur[0]=0;
 const char*p=s;
 while(*p){
  const char*q=p;while(*q&&*q!=' ')q++;
  size_t wl=(size_t)(q-p);if(wl>TBUF/2)wl=TBUF/2;
  char t[TBUF];size_t cl=strlen(cur);
  if(cl)snprintf(t,sizeof(t),"%s %.*s",cur,(int)wl,p);else snprintf(t,sizeof(t),"%.*s",(int)wl,p);
  if(text_w(f,t)<=maxw||!cl)memcpy(cur,t,strlen(t)+1);
  else{if(nl<3)memcpy(lines[nl++],cur,cl+1);snprintf(cur,sizeof(cur),"%.*s",(int)wl,p);if(nl>=3){more=1;break;}}
  p=*q?q+1:q;
 }
 if(!more){if(nl<3)memcpy(lines[nl++],cur,strlen(cur)+1);else more=1;}
 char b[TBUF];
 if(nl<=1&&!more){snprintf(l1,TBUF,"%s",fit(f,lines[0],maxw,b));l2[0]=0;return 1;}
 snprintf(l1,TBUF,"%s",fit(f,lines[0],maxw,b));
 if(nl>2||more){char t[TBUF];snprintf(t,sizeof(t),"%s ...",lines[1]);snprintf(l2,TBUF,"%s",fit(f,t,maxw,b));}
 else snprintf(l2,TBUF,"%s",fit(f,lines[1],maxw,b));
 return 2;
}
/* 8- and 10-dot spinner offsets (cos,sin * 1024), starting at 12 o'clock. */
static const short SPIN8[8][2]={{0,-1024},{724,-724},{1024,0},{724,724},{0,1024},{-724,724},{-1024,0},{-724,-724}};
static const short SPIN10[10][2]={{0,-1024},{602,-828},{974,-316},{974,316},{602,828},{0,1024},{-602,828},{-974,316},{-974,-316},{-602,-828}};
static void spinner(int cx,int cy,int r,uint32_t rgb,uint64_t t_ms,int dots,int dr){
 const short(*tab)[2]=dots==8?SPIN8:SPIN10;int head=(int)((t_ms/90)%(uint64_t)dots);
 for(int i=0;i<dots;i++){
  int age=(head-i+dots)%dots,a=255-age*28;if(a<40)a=40;
  disc(cx+((r*tab[i][0]+512)>>10),cy+((r*tab[i][1]+512)>>10),dr,rgb,(unsigned)a);
 }
}

static int ui_link(void){
 if(!online)return UI_OFFLINE;
 if(ext_ok&&state.link>=BT_LINK_IDLE&&state.link<=BT_LINK_SEARCHING)return state.link;
 return state.streaming?BT_LINK_STREAMING:(state.wanted?BT_LINK_CONNECTING:BT_LINK_IDLE);
}
/* ---- screen ---- */
static void dev_name(const struct bt_device*d,char*out,size_t cap){
 const char*src=d->name;size_t len=strlen(src),n=len;
 while(n>0&&(src[n-1]=='?'||src[n-1]==' '))n--;
 int q=0;for(size_t i=n;i<len;i++)if(src[i]=='?')q++;
 if(n<len&&q<2){n=len;while(n>0&&src[n-1]==' ')n--;}
 if(n==0){snprintf(out,cap,"Unnamed device %04lX",d->address&0xffff);return;}
 if(n>=cap)n=cap-1;memcpy(out,src,n);out[n]=0;
}
static int secs(int ms){return(ms+999)/1000;}
static void fmt_bytes(uint64_t b,char*out,size_t cap){
 if(b>=1048576ULL){uint64_t t=(b*10+524288ULL)/1048576ULL;snprintf(out,cap,"%llu.%llu MB",(unsigned long long)(t/10),(unsigned long long)(t%10));}
 else{uint64_t kb=(b+512ULL)/1024ULL;snprintf(out,cap,"%llu KB",(unsigned long long)kb);}
}
static void outlined_rect(int x,int y,int w,int h,int r,uint32_t edge,uint32_t body){
 rrect(x,y,w,h,r,edge);
 rrect(x+1,y+1,w-2,h-2,r-1,body);
}

/* Buttons are right-aligned to their container, with controller hints built in. */
static int button(int right,int y,const char*label,int glyph,const char*key,int primary){
 int kw=key?text_w(F_KEY,key)+10:0;
 int gw=glyph>=0?ICONS[glyph].w:0;
 int w=28+text_w(F_SMALLB,label)+(kw?kw+8:0)+(gw?gw+8:0);
 int x=right-w,pen=x+14;
 uint32_t ink=primary?BG:TEXT;
 if(primary)rrect(x,y,w,42,7,TEXT);
 else outlined_rect(x,y,w,42,7,TRACK,PANEL);
 if(kw){
  outlined_rect(pen,y+11,kw,20,4,ink,primary?TEXT:PANEL);
  text(F_KEY,pen+5,y+25,key,ink);
  pen+=kw+8;
 }
 if(gw){
  icon_c(glyph,pen+gw/2,y+21,ink,255);
  pen+=gw+8;
 }
 text(F_SMALLB,pen,y+27,label,ink);
 return x;
}

static int shortcut(int x,int y,const char*label,int glyph,const char*first,const char*second,uint32_t ink){
 const char*keys[]={first,second};
 for(int i=0;i<2&&keys[i];i++){
  int w=text_w(F_KEY,keys[i])+14;
  outlined_rect(x,y,w,25,5,TRACK,BG);
  text(F_KEY,x+7,y+17,keys[i],ink);
  x+=w+(i==0&&second?5:8);
 }
 if(glyph>=0){
  icon_c(glyph,x+8,y+12,ink,255);
  x+=22;
 }
 text(F_SMALL,x,y+18,label,ink);
 return x+text_w(F_SMALL,label);
}

static void paragraph(int f,int x,int y,int width,int line_height,const char*msg,uint32_t ink){
 char first[TBUF],second[TBUF];
 int lines=wrap2(f,msg,width,first,second);
 text(f,x,y,first,ink);
 if(lines==2)text(f,x,y+line_height,second,ink);
}

static int device_icon(const struct bt_device*d){
 /* Bluetooth Class of Device: audio/video major class, loudspeaker minor class. */
 return d&&(d->cod&0x1ffc)==0x0414?I_LOUDSPEAKER:I_ROWHP;
}

static void draw_header(void){
 logo(86,31);
 text(F_BRAND,123,51,"BTAudio4",TEXT);
 fill(86,86,1108,1,BORDER);

 int right=1194;
 char detail[TBUF];
 const char*title=NULL;
 if(upd_state==BTA_UPD_OUTDATED&&upd_latest[0]){
  title="Update available";
  if(upd_has_pkg){
   right=button(right,22,"Download",-1,"R2",1)-18;
   snprintf(detail,sizeof(detail),"%s is ready to download",upd_latest);
  }else snprintf(detail,sizeof(detail),"%s - no package attached",upd_latest);
 }else if(upd_state==BTA_UPD_MISMATCH&&upd_latest[0]){
  title="Version mismatch";
  snprintf(detail,sizeof(detail),"Latest published release: %s",upd_latest);
 }
 if(title){
  char fitted[TBUF];
  const char*sub=fit(F_SMALL,detail,360,fitted);
  int width=text_w(F_SMALL,sub),heading_width=text_w(F_SMALLB,title);
  if(width<heading_width)width=heading_width;
  int x=right-width;
  text(F_SMALLB,x,39,title,TEXT);
  text(F_SMALL,x,58,sub,MUTED);
  fill(x-17,30,1,28,TRACK);
  right=x-33;
 }
 text_r(F_SMALL,right,49,"v" BTA_VERSION,MUTED);
}

static void draw_playback(int l,const struct bt_device*target,int pinmode,uint64_t now){
 static const char*const link_labels[]={
  "Disconnected","Connecting","Connected","Connection lost","Failed","Searching","Service offline"
 };
 const char*status=pinmode?"Connecting":link_labels[l];
 int playing=l==BT_LINK_STREAMING;
 int testing=(ext_ok&&state.test)||g_now<testing_until;
 char name[TBUF],buf[TBUF];
 const char*caption="Audio output";
 if(playing)caption=testing?"Playing test tone":"Now playing";
 else if(pinmode)caption="Pairing";
 if(target)dev_name(target,name,sizeof(name));
 else snprintf(name,sizeof(name),"%s",l==UI_OFFLINE?"Service offline":"None");

 outlined_rect(86,128,615,410,15,BORDER,PANEL);
 outlined_rect(117,160,90,90,13,TRACK,BORDER);
 int glyph=device_icon(target)==I_LOUDSPEAKER?I_HERO_SPEAKER:I_HERO;
 icon_c(glyph,162,205,target?TEXT:MUTED,255);
 text(F_KICKER,228,187,caption,MUTED);
 int name_width=442;
 if(l!=UI_OFFLINE){
  int status_x=670-text_w(F_BODY,status);
  name_width=status_x-228-27;
  text(F_BODY,status_x,211,status,MUTED);
  if(l==BT_LINK_CONNECTING||l==BT_LINK_SEARCHING)
   spinner(status_x-13,205,6,MUTED,now,8,1);
  else
   disc(status_x-12,205,3,playing?TEXT:MUTED,255);
  if(playing&&ext_ok&&meter_s>0){
   int level=meter_s>100?100:meter_s;
   rrect(status_x,224,670-status_x,3,1,TRACK);
   fill(status_x,224,(670-status_x)*level/100,3,MUTED);
  }
 }
 text_fit(F_TITLE,228,231,name,TEXT,name_width);
 fill(117,279,553,1,BORDER);
 text(F_LABEL,117,324,"Headphone volume",TEXT);
 int volume=state.volume<0?0:state.volume>100?100:state.volume;
 snprintf(buf,sizeof(buf),"%d%%",volume);
 text_r(F_NAME,670,324,buf,TEXT);
 rrect(117,346,553,6,3,TRACK);
 int thumb=117+(553*volume)/100;
 rrect(117,346,thumb-117,6,3,TEXT);
 disc(thumb,349,7,TEXT,255);
 if(playing&&ext_ok)
  text(F_BODY,117,383,state.volume_synced?"Synced with headphones":"Software volume",MUTED);

 const char*notice_text=NULL;
 if(notice[0]&&g_now<notice_until)notice_text=notice;
 else if(l==UI_OFFLINE)notice_text="Press X to start the audio service. If it stays offline, check that GoldHEN's BinLoader is enabled.";
 else if(!pinmode&&(l==BT_LINK_LOST||l==BT_LINK_FAILED||l==BT_LINK_CONNECTING||l==BT_LINK_SEARCHING)){
  if(ext_ok&&state.retry_ms>0){
   snprintf(buf,sizeof(buf),"%s%sRetrying in %ds.",state.message,state.message[0]?" ":"",secs(state.retry_ms));
   notice_text=buf;
  }else if(state.message[0])notice_text=state.message;
  else if(ext_ok&&state.attempts>1){
   snprintf(buf,sizeof(buf),"Connection attempt %d",state.attempts);
   notice_text=buf;
  }
 }else if(playing&&ext_ok&&state.quiet_ms>4000&&!testing)
  notice_text="Waiting for game audio. Play something or press Options to test.";
 if(notice_text)paragraph(F_SMALL,117,417,553,19,notice_text,MUTED);

 fill(117,446,553,1,BORDER);
 icon_c(I_DISPLAY,128,490,MUTED,255);
 text(F_BODY,151,496,"TV sound",TEXT);
 text(F_SMALL,232,496,l==UI_OFFLINE?"Unavailable":state.tv?"On":"Muted",MUTED);
 if(l!=UI_OFFLINE)button(670,469,state.tv?"Mute":"Unmute",I_BSQUARE,NULL,0);
}

static void draw_devices(int l,int n,int pinmode,uint64_t now){
 outlined_rect(721,128,473,410,15,BORDER,PANEL);
 text(F_SECTION,748,181,"Devices",TEXT);
 if(state.scanning){
  text_r(F_SMALL,1151,180,"Scanning",MUTED);
  spinner(1162,175,6,MUTED,now,8,1);
 }else if(n>5){
  char range[32];
  snprintf(range,sizeof(range),"%d-%d of %d",top+1,top+5<n?top+5:n,n);
  text_r(F_SMALL,1169,180,range,MUTED);
 }
 if(!n){
  icon_c(I_HERO,957,278,MUTED,255);
  text(F_SMALLB,776,325,l==UI_OFFLINE?"Device list unavailable":"No Bluetooth devices found",TEXT);
  if(l!=UI_OFFLINE)
   paragraph(F_SMALL,776,352,378,19,"Put your device in pairing mode, then press Triangle to scan.",MUTED);
 }
 int step=n>3?47:69;
 int h=n>3?44:66;
 int y0=n>3?199:207;
 for(int i=top;i<n&&i<top+5;i++){
  const struct bt_device*d=&state.devices[i];
  int selected=i==focus, current=d->address==state.selected&&l!=BT_LINK_IDLE&&l!=UI_OFFLINE;
  int y=y0+(i-top)*step, cy=y+h/2;
  char name[TBUF];
  dev_name(d,name,sizeof(name));
  if(selected)outlined_rect(745,y,425,h,8,MUTED,BORDER);
  icon_c(device_icon(d),770,cy,selected?TEXT:MUTED,255);
  text_fit(F_NAME,793,cy+7,name,TEXT,330);
  if(current){
   if(l==BT_LINK_STREAMING)icon_c(I_CHECK,1145,cy,TEXT,255);
   else if(l==BT_LINK_CONNECTING||l==BT_LINK_SEARCHING||pinmode)
    spinner(1145,cy,7,MUTED,now,8,1);
   else icon_c(I_BANG,1145,cy,MUTED,255);
  }
 }
 fill(746,448,423,1,BORDER);
 const char*label=NULL;
 int glyph=I_BCROSS;
 if(l==UI_OFFLINE)label="Retry";
 else if(focus>=0&&focus<n){
  int current=l!=BT_LINK_IDLE&&state.devices[focus].address==state.selected;
  label=current?(l==BT_LINK_STREAMING?"Disconnect":"Stop trying"):"Connect";
  if(n>1){
   icon_c(I_BUPDOWN,756,491,TEXT,255);
   text(F_SMALL,774,496,"Choose a device",MUTED);
  }
 }else if(l==BT_LINK_IDLE&&!state.scanning){
  label="Scan";
  glyph=I_BTRI;
 }else if(l!=BT_LINK_IDLE){
  label=l==BT_LINK_STREAMING?"Disconnect":"Stop trying";
  glyph=I_BTRI;
 }
 if(label)button(1169,469,label,glyph,NULL,1);
}

static void draw_shortcuts(int l,int n){
 fill(86,573,1108,1,BORDER);
 int x=shortcut(86,592,"Volume",-1,"L1","R1",MUTED)+26;
 if(l==BT_LINK_STREAMING)x=shortcut(x,592,"Test sound",-1,"OPTIONS",NULL,MUTED)+26;
 if(l==BT_LINK_IDLE&&n>0)shortcut(x,592,"Scan",I_BTRI,NULL,NULL,MUTED);
 else if(l!=UI_OFFLINE&&l!=BT_LINK_IDLE&&focus>=0&&focus<n&&state.devices[focus].address!=state.selected)
  shortcut(x,592,l==BT_LINK_STREAMING?"Disconnect":"Stop trying",I_BTRI,NULL,NULL,MUTED);
 shortcut(1194-text_w(F_SMALL,"Close")-22,592,"Close",I_BCIRCLE,NULL,NULL,MUTED);
}

/* Dialogs share a centered surface with left-aligned copy and bottom actions. */
static void dialog_frame(int y,int h,int icon_id,const char*mark,const char*title){
 dim_frame(205);
 outlined_rect(360,y,560,h,15,TRACK,PANEL);
 outlined_rect(390,y+28,43,43,9,TRACK,BORDER);
 if(icon_id>=0)icon_c(icon_id,411,y+49,TEXT,255);
 else text(F_SECTION,411-text_w(F_SECTION,mark)/2,y+57,mark,TEXT);
 text(F_DIALOG,390,y+110,title,TEXT);
 fill(390,y+h-85,500,1,BORDER);
}

static void draw_pin(const struct bt_device*target,const char*pin,int digit){
 const int y=180,h=360;
 char buf[TBUF],name[TBUF];
 if(target){
  dev_name(target,name,sizeof(name));
  snprintf(buf,sizeof(buf),"%.96s is asking for a PIN. Try 0000 or 1234.",name);
 }else snprintf(buf,sizeof(buf),"This device is asking for a PIN. Try 0000 or 1234.");
 dialog_frame(y,h,-1,"...","Pairing PIN");
 paragraph(F_BODY,390,y+141,500,20,buf,MUTED);
 for(int i=0;i<4;i++){
  int x=390+i*66;
  outlined_rect(x,y+185,56,64,8,i==digit?TEXT:TRACK,i==digit?BORDER:BG);
  if(i==digit)outlined_rect(x+2,y+187,52,60,6,TEXT,BORDER);
  char num[2]={pin[i],0};
  text(F_DIALOG,x+(56-text_w(F_DIALOG,num))/2,y+227,num,TEXT);
 }
 text(F_SMALL,664,y+204,"Left / Right: Select digit",MUTED);
 text(F_SMALL,664,y+228,"Up / Down: Change digit",MUTED);
 int x=button(890,y+h-66,"Submit PIN",I_BCROSS,NULL,1);
 button(x-12,y+h-66,"Cancel",I_BCIRCLE,NULL,0);
}

static void draw_download(uint64_t now){
 char buf[TBUF];
 const char*pkg=dl_pkg_name[0]?dl_pkg_name:"update package";
 if(dl_state==BTA_DL_RUNNING){
  int y=185,h=350;
  dialog_frame(y,h,I_DOWNLOAD,NULL,"Downloading update");
  snprintf(buf,sizeof(buf),"Saving %s to /data/pkg/.",pkg);
  paragraph(F_BODY,390,y+141,500,20,buf,MUTED);
  char received[32];
  fmt_bytes(dl_bytes,received,sizeof(received));
  rrect(390,y+230,500,6,3,TRACK);
  if(dl_total){
   double ratio=dl_bytes>=dl_total?1.0:(double)dl_bytes/(double)dl_total;
   char total[32];
   fmt_bytes(dl_total,total,sizeof(total));
   snprintf(buf,sizeof(buf),"%d%%",(int)(ratio*100));
   text(F_SMALLB,390,y+215,buf,TEXT);
   snprintf(buf,sizeof(buf),"%s / %s",received,total);
   text_r(F_SMALL,890,y+215,buf,MUTED);
   rrect(390,y+230,(int)(500*ratio),6,3,TEXT);
  }else{
   snprintf(buf,sizeof(buf),"%s downloaded",received);
   text(F_SMALLB,390,y+215,buf,TEXT);
   text_r(F_SMALL,890,y+215,"Total size unknown",MUTED);
   int offset=(int)((now/4)%750);
   if(offset>375)offset=750-offset;
   rrect(390+offset,y+230,125,6,3,TEXT);
  }
  button(890,y+h-66,"Cancel",I_BCIRCLE,NULL,0);
 }else if(dl_state==BTA_DL_DONE){
  int y=127,h=466;
  dialog_frame(y,h,I_CHECK,NULL,"Update ready to install");
  snprintf(buf,sizeof(buf),"Saved to /data/pkg/%s.",pkg);
  paragraph(F_BODY,390,y+141,500,20,buf,MUTED);
  outlined_rect(390,y+187,500,171,8,BORDER,BG);
  text(F_SMALL,406,y+213,"1. Close BTAudio4 from the PS4 home screen.",TEXT);
  paragraph(F_SMALL,406,y+241,462,19,
            "2. Open GoldHEN > Debug Settings > Package Installer.",TEXT);
  snprintf(buf,sizeof(buf),"Select %s.",pkg);
  paragraph(F_SMALL,423,y+281,446,19,buf,TEXT);
  paragraph(F_SMALL,406,y+322,462,19,
            "3. If the PS4 asks whether to continue installing, select Yes.",TEXT);
  button(890,y+h-66,"Dismiss",I_DISMISS,NULL,1);
 }else{
  int y=210,h=300;
  dialog_frame(y,h,I_BANG,NULL,"Download failed");
  paragraph(F_BODY,390,y+143,500,20,dl_err[0]?dl_err:"Could not download the update package.",MUTED);
  int x=button(890,y+h-66,"Retry",-1,"R2",1);
  button(x-12,y+h-66,"Dismiss",I_DISMISS,NULL,0);
 }
}

static void draw(int pinmode,const char*pin,int digit){
 uint64_t now=g_now/1000;
 int l=ui_link(),n=state.count;
 const struct bt_device*target=NULL;
 if(l!=BT_LINK_IDLE&&l!=UI_OFFLINE){
  for(int i=0;i<n;i++){
   if(state.devices[i].address==state.selected){
    target=&state.devices[i];
    break;
   }
  }
 }
 clip(0,0,WIDTH,HEIGHT);
 fill(0,0,WIDTH,HEIGHT,BG);
 draw_header();
 draw_playback(l,target,pinmode,now);
 draw_devices(l,n,pinmode,now);
 draw_shortcuts(l,n);

 if(pinmode)draw_pin(target,pin,digit);
 else if(dl_state!=BTA_DL_NONE)draw_download(now);
}

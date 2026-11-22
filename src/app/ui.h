/* Screen drawing for the native app. Pure C with no console dependencies, so the
 * PC harness (src/app/host_ui.c) can run this exact code and diff it pixel-for-pixel
 * against the preview renderer. Included by main.c. */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../common/protocol.h"
#include "../common/version.h"
#include "../common/announcement.h"
#include "ui_font.h"
#include "ui_icons.h"

#define WIDTH 1280
#define HEIGHT 720
#define HW (WIDTH/2)
#define HH (HEIGHT/2)
#define HINT_W 160
#define HINT_H 64
#define UI_OFFLINE 6

#define BG      0x09090b
#define PANEL   0x111113
#define BORDER  0x222226
#define TRACK   0x3f3f46
#define TEXT    0xfafafa
#define MUTED   0xa4a4ab

/* Portable button masks matching OrbisPadButton bits (no console header needed). */
#define UI_BTN_OPTIONS  0x0008u
#define UI_BTN_UP       0x0010u
#define UI_BTN_RIGHT    0x0020u
#define UI_BTN_DOWN     0x0040u
#define UI_BTN_LEFT     0x0080u
#define UI_BTN_L2       0x0100u
#define UI_BTN_R2       0x0200u
#define UI_BTN_L1       0x0400u
#define UI_BTN_R1       0x0800u
#define UI_BTN_TRIANGLE 0x1000u
#define UI_BTN_CIRCLE   0x2000u
#define UI_BTN_CROSS    0x4000u
#define UI_BTN_SQUARE   0x8000u
#define UI_BTN_TOUCHPAD 0x100000u

#define ANIM_SEL_US     130000ULL
#define ANIM_VOL_US     120000ULL
#define ANIM_STATE_US   150000ULL
#define ANIM_POPUP_US   150000ULL
#define ANIM_PRESS_US    70000ULL
#define ANIM_RELEASE_US 110000ULL

static uint32_t back[WIDTH*HEIGHT]; /* cached off-screen target; video buffers are slow to read back */
static uint32_t layer[WIDTH*HEIGHT];
static uint32_t layer2[WIDTH*HEIGHT];
static uint32_t half_a[HW*HH];
static uint32_t hint_buf[HINT_W*HINT_H];
static uint32_t hint_tmp[HINT_W*HINT_H];
static uint32_t row_tmp[WIDTH];
static uint32_t col_tmp[HEIGHT];

static struct bt_native state;
static int online,ext_ok,focus,top,meter_s,upd_state,upd_has_pkg,dl_state;
static uint64_t g_now,testing_until,notice_until,dl_bytes,dl_total;
static char notice[160],upd_latest[64],dl_pkg_name[128],dl_err[160],client_uid[16];
static unsigned g_held;
static struct bta_announcement announcement;
static uint64_t announcement_close_at;
static int announcement_visible,announcement_scroll,announcement_save_error;

/* ---- drawing primitives (support opaque backbuffer and premultiplied scratch layers) ---- */
static uint32_t*g_fb=back;
static int g_stride=WIDTH,g_cw=WIDTH,g_ch=HEIGHT;
static int cx0,cy0,cx1=WIDTH,cy1=HEIGHT;

static void clip(int x0,int y0,int x1,int y1){
 cx0=x0<0?0:x0;cy0=y0<0?0:y0;cx1=x1>g_cw?g_cw:x1;cy1=y1>g_ch?g_ch:y1;
}
static void set_surface(uint32_t*fb,int stride,int w,int h){
 g_fb=fb;g_stride=stride;g_cw=w;g_ch=h;clip(0,0,w,h);
}
static void blend(int x,int y,uint32_t rgb,unsigned a){
 if(!a||x<cx0||x>=cx1||y<cy0||y>=cy1)return;
 uint32_t*p=&g_fb[y*g_stride+x];
 if(a>=255){*p=0xff000000u|rgb;return;}
 uint32_t bg=*p;
 unsigned da=bg>>24,ia=255-a;
 unsigned sr=(rgb>>16)&255,sg=(rgb>>8)&255,sb=rgb&255;
 if(da==255){
  unsigned r=(sr*a+((bg>>16)&255)*ia+127)/255;
  unsigned g=(sg*a+((bg>>8)&255)*ia+127)/255;
  unsigned b=(sb*a+(bg&255)*ia+127)/255;
  *p=0xff000000u|(r<<16)|(g<<8)|b;
 }else if(da==0){
  unsigned r=(sr*a+127)/255,g=(sg*a+127)/255,b=(sb*a+127)/255;
  *p=(a<<24)|(r<<16)|(g<<8)|b;
 }else{
  unsigned oa=a+(da*ia+127)/255;
  unsigned r=(sr*a+((bg>>16)&255)*ia+127)/255;
  unsigned g=(sg*a+((bg>>8)&255)*ia+127)/255;
  unsigned b=(sb*a+(bg&255)*ia+127)/255;
  if(r>oa)r=oa;if(g>oa)g=oa;if(b>oa)b=oa;
  *p=(oa<<24)|(r<<16)|(g<<8)|b;
 }
}
static void fill(int x,int y,int w,int h,uint32_t rgb){
 int x0=x>cx0?x:cx0,y0=y>cy0?y:cy0,x1=x+w<cx1?x+w:cx1,y1=y+h<cy1?y+h:cy1;
 if(x1<=x0||y1<=y0)return;
 uint32_t v=0xff000000u|rgb;
 for(int j=y0;j<y1;j++){uint32_t*row=g_fb+j*g_stride;for(int i=x0;i<x1;i++)row[i]=v;}
}
/* Coverage (0..255) of pixel (cx,cy) in a top-left corner of radius r: 4x4 supersampling. */
static unsigned corner_cov(int r,int cx,int cy){
 int R8=r*8,n=0;
 for(int t=1;t<8;t+=2)for(int s=1;s<8;s+=2){int dx=8*cx+s-R8,dy=8*cy+t-R8;if(dx*dx+dy*dy<=R8*R8)n++;}
 return (unsigned)((n*255+8)/16);
}
static void rrect(int x,int y,int w,int h,int r,uint32_t rgb){
 if(w<=0||h<=0)return;
 if(r>w/2)r=w/2;
 if(r>h/2)r=h/2;
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

/* ---- off-screen blur & compositing helpers ---- */
static int clampi(int v,int lo,int hi){return v<lo?lo:v>hi?hi:v;}
static float clampf(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
static float ease_out3(float t){float u=1.0f-clampf(t,0.0f,1.0f);return 1.0f-u*u*u;}

static void clear_rect(uint32_t*buf,int stride,int x0,int y0,int x1,int y1){
 size_t row_bytes=(size_t)(x1-x0)*sizeof(uint32_t);
 for(int y=y0;y<y1;y++)memset(buf+y*stride+x0,0,row_bytes);
}
static void copy_rect(uint32_t*dst,const uint32_t*src,int stride,int x0,int y0,int x1,int y1){
 size_t row_bytes=(size_t)(x1-x0)*sizeof(uint32_t);
 for(int y=y0;y<y1;y++)memcpy(dst+y*stride+x0,src+y*stride+x0,row_bytes);
}

/* In-place separable sliding-sum box blur on premultiplied ARGB within [x0..x1, y0..y1). */
static void box_blur_rect(uint32_t*buf,int stride,int x0,int y0,int x1,int y1,int r,int clamp_edge){
 int w=x1-x0,h=y1-y0;
 if(r<=0||w<=0||h<=0)return;
 int k=2*r+1;
 unsigned inv=(65536u+(unsigned)k/2u)/(unsigned)k;
 for(int y=y0;y<y1;y++){
  uint32_t*row=buf+y*stride+x0;
  memcpy(row_tmp,row,(size_t)w*sizeof(uint32_t));
  uint32_t sa=0,sr=0,sg=0,sb=0;
  if(clamp_edge){
   uint32_t p0=row_tmp[0];
   sa=(p0>>24)*(unsigned)(r+1);sr=((p0>>16)&255)*(unsigned)(r+1);sg=((p0>>8)&255)*(unsigned)(r+1);sb=(p0&255)*(unsigned)(r+1);
   for(int i=1;i<=r;i++){uint32_t p=row_tmp[i<w?i:w-1];sa+=p>>24;sr+=(p>>16)&255;sg+=(p>>8)&255;sb+=p&255;}
  }else{
   for(int i=0;i<=r&&i<w;i++){uint32_t p=row_tmp[i];sa+=p>>24;sr+=(p>>16)&255;sg+=(p>>8)&255;sb+=p&255;}
  }
  for(int x=0;x<w;x++){
   unsigned a=(sa*inv+32768u)>>16,cr=(sr*inv+32768u)>>16,cg=(sg*inv+32768u)>>16,cb=(sb*inv+32768u)>>16;
   if(a>255)a=255;if(cr>255)cr=255;if(cg>255)cg=255;if(cb>255)cb=255;
   row[x]=(a<<24)|(cr<<16)|(cg<<8)|cb;
   int ox=x-r,ix=x+r+1;
   if(clamp_edge){
    uint32_t po=row_tmp[ox<0?0:ox],pi=row_tmp[ix<w?ix:w-1];
    sa+=(pi>>24)-(po>>24);sr+=((pi>>16)&255)-((po>>16)&255);sg+=((pi>>8)&255)-((po>>8)&255);sb+=(pi&255)-(po&255);
   }else{
    if(ox>=0){uint32_t p=row_tmp[ox];sa-=p>>24;sr-=(p>>16)&255;sg-=(p>>8)&255;sb-=p&255;}
    if(ix<w){uint32_t p=row_tmp[ix];sa+=p>>24;sr+=(p>>16)&255;sg+=(p>>8)&255;sb+=p&255;}
   }
  }
 }
 for(int x=x0;x<x1;x++){
  for(int y=0;y<h;y++)col_tmp[y]=buf[(y0+y)*stride+x];
  uint32_t sa=0,sr=0,sg=0,sb=0;
  if(clamp_edge){
   uint32_t p0=col_tmp[0];
   sa=(p0>>24)*(unsigned)(r+1);sr=((p0>>16)&255)*(unsigned)(r+1);sg=((p0>>8)&255)*(unsigned)(r+1);sb=(p0&255)*(unsigned)(r+1);
   for(int i=1;i<=r;i++){uint32_t p=col_tmp[i<h?i:h-1];sa+=p>>24;sr+=(p>>16)&255;sg+=(p>>8)&255;sb+=p&255;}
  }else{
   for(int i=0;i<=r&&i<h;i++){uint32_t p=col_tmp[i];sa+=p>>24;sr+=(p>>16)&255;sg+=(p>>8)&255;sb+=p&255;}
  }
  for(int y=0;y<h;y++){
   unsigned a=(sa*inv+32768u)>>16,cr=(sr*inv+32768u)>>16,cg=(sg*inv+32768u)>>16,cb=(sb*inv+32768u)>>16;
   if(a>255)a=255;if(cr>255)cr=255;if(cg>255)cg=255;if(cb>255)cb=255;
   buf[(y0+y)*stride+x]=(a<<24)|(cr<<16)|(cg<<8)|cb;
   int oy=y-r,iy=y+r+1;
   if(clamp_edge){
    uint32_t po=col_tmp[oy<0?0:oy],pi=col_tmp[iy<h?iy:h-1];
    sa+=(pi>>24)-(po>>24);sr+=((pi>>16)&255)-((po>>16)&255);sg+=((pi>>8)&255)-((po>>8)&255);sb+=(pi&255)-(po&255);
   }else{
    if(oy>=0){uint32_t p=col_tmp[oy];sa-=p>>24;sr-=(p>>16)&255;sg-=(p>>8)&255;sb-=p&255;}
    if(iy<h){uint32_t p=col_tmp[iy];sa+=p>>24;sr+=(p>>16)&255;sg+=(p>>8)&255;sb+=p&255;}
   }
  }
 }
}

/* Continuous fractional blur on premultiplied ARGB within [x0..x1, y0..y1). */
static void apply_blur_f(uint32_t*buf,uint32_t*tmp,int stride,int x0,int y0,int x1,int y1,float bf){
 if(bf<=0.02f)return;
 int r_lo=(int)bf;
 unsigned frac=(unsigned)((bf-(float)r_lo)*255.0f+0.5f);
 if(frac<=4){box_blur_rect(buf,stride,x0,y0,x1,y1,r_lo,0);return;}
 if(frac>=251){box_blur_rect(buf,stride,x0,y0,x1,y1,r_lo+1,0);return;}
 copy_rect(tmp,buf,stride,x0,y0,x1,y1);
 if(r_lo>0)box_blur_rect(tmp,stride,x0,y0,x1,y1,r_lo,0);
 box_blur_rect(buf,stride,x0,y0,x1,y1,r_lo+1,0);
 unsigned ifrac=255-frac;
 for(int y=y0;y<y1;y++){
  uint32_t*d=buf+y*stride;const uint32_t*s=tmp+y*stride;
  for(int x=x0;x<x1;x++){
   uint32_t p0=s[x],p1=d[x];
   unsigned a=(((p0>>24)*ifrac)+((p1>>24)*frac)+127)/255;
   unsigned r=((((p0>>16)&255)*ifrac)+(((p1>>16)&255)*frac)+127)/255;
   unsigned g=((((p0>>8)&255)*ifrac)+(((p1>>8)&255)*frac)+127)/255;
   unsigned b=(((p0&255)*ifrac)+((p1&255)*frac)+127)/255;
   d[x]=(a<<24)|(r<<16)|(g<<8)|b;
  }
 }
}

/* Blend premultiplied src pixel (scaled by alpha 0..255) over dst pixel. */
static uint32_t over_pm(uint32_t dst,uint32_t src,unsigned alpha){
 unsigned sa=src>>24;
 if(!sa||!alpha)return dst;
 unsigned sr=(src>>16)&255,sg=(src>>8)&255,sb=src&255;
 if(alpha<255){
  sa=(sa*alpha+127)/255;
  sr=(sr*alpha+127)/255;
  sg=(sg*alpha+127)/255;
  sb=(sb*alpha+127)/255;
 }
 if(sa>=255)return 0xff000000u|(sr<<16)|(sg<<8)|sb;
 unsigned ia=255-sa,da=dst>>24;
 unsigned r=sr+(((dst>>16)&255)*ia+127)/255;
 unsigned g=sg+(((dst>>8)&255)*ia+127)/255;
 unsigned b=sb+((dst&255)*ia+127)/255;
 unsigned oa=da==255?255:sa+(da*ia+127)/255;
 if(r>oa)r=oa;if(g>oa)g=oa;if(b>oa)b=oa;
 return (oa<<24)|(r<<16)|(g<<8)|b;
}

static void composite_rect(uint32_t*dst,int dst_stride,const uint32_t*src,int src_stride,
                           int x0,int y0,int x1,int y1,unsigned alpha){
 if(!alpha)return;
 for(int y=y0;y<y1;y++){
  uint32_t*d=dst+y*dst_stride;const uint32_t*s=src+y*src_stride;
  for(int x=x0;x<x1;x++)if(s[x]>>24)d[x]=over_pm(d[x],s[x],alpha);
 }
}

/* Bilinearly scale premultiplied hint sprite centered in src[sw*sh] onto dst around (cx,cy). */
static void composite_scaled(uint32_t*dst,int dst_stride,int dst_w,int dst_h,
                             int cx,int cy,const uint32_t*src,int sw,int sh,float scale){
 if(scale<=0.05f)return;
 int half_w=(int)((float)(sw/2)*scale)+2;
 int half_h=(int)((float)(sh/2)*scale)+2;
 int x0=clampi(cx-half_w,0,dst_w),x1=clampi(cx+half_w,0,dst_w);
 int y0=clampi(cy-half_h,0,dst_h),y1=clampi(cy+half_h,0,dst_h);
 int inv_fp=(int)(65536.0f/scale+0.5f);
 int scx=(sw/2)<<16,scy=(sh/2)<<16;
 for(int y=y0;y<y1;y++){
  int fy=scy+(y-cy)*inv_fp;
  int iy=fy>>16,wy=(fy>>8)&255,iwy=255-wy;
  if(iy<-1||iy>=sh)continue;
  for(int x=x0;x<x1;x++){
   int fx=scx+(x-cx)*inv_fp;
   int ix=fx>>16,wx=(fx>>8)&255,iwx=255-wx;
   if(ix<-1||ix>=sw)continue;
   uint32_t p00=(ix>=0&&iy>=0)?src[iy*sw+ix]:0;
   uint32_t p10=(ix+1<sw&&iy>=0)?src[iy*sw+ix+1]:0;
   uint32_t p01=(ix>=0&&iy+1<sh)?src[(iy+1)*sw+ix]:0;
   uint32_t p11=(ix+1<sw&&iy+1<sh)?src[(iy+1)*sw+ix+1]:0;
   if(!(p00|p10|p01|p11))continue;
   unsigned w00=(unsigned)(iwx*iwy),w10=(unsigned)(wx*iwy),w01=(unsigned)(iwx*wy),w11=(unsigned)(wx*wy);
   unsigned a=((p00>>24)*w00+(p10>>24)*w10+(p01>>24)*w01+(p11>>24)*w11+32512u)/65025u;
   if(!a)continue;
   unsigned r=((((p00>>16)&255)*w00+((p10>>16)&255)*w10+((p01>>16)&255)*w01+((p11>>16)&255)*w11+32512u)/65025u);
   unsigned g=((((p00>>8)&255)*w00+((p10>>8)&255)*w10+((p01>>8)&255)*w01+((p11>>8)&255)*w11+32512u)/65025u);
   unsigned b=(((p00&255)*w00+(p10&255)*w10+(p01&255)*w01+(p11&255)*w11+32512u)/65025u);
   if(r>a)r=a;if(g>a)g=a;if(b>a)b=a;
   dst[y*dst_stride+x]=over_pm(dst[y*dst_stride+x],(a<<24)|(r<<16)|(g<<8)|b,255);
  }
 }
}

/* Downsample back 2x2, blur twice, dim to 50/255 (same dim as dim_frame(205)), and blend by alpha. */
static void blur_dim_backdrop(unsigned alpha){
 if(!alpha)return;
 for(int y=0;y<HH;y++){
  const uint32_t*r0=back+(2*y)*WIDTH,*r1=r0+WIDTH;
  uint32_t*dst=half_a+y*HW;
  for(int x=0;x<HW;x++){
   int sx=2*x;
   uint32_t a0=r0[sx],a1=r0[sx+1],b0=r1[sx],b1=r1[sx+1];
   unsigned r=(((a0>>16)&255)+((a1>>16)&255)+((b0>>16)&255)+((b1>>16)&255)+2)>>2;
   unsigned g=(((a0>>8)&255)+((a1>>8)&255)+((b0>>8)&255)+((b1>>8)&255)+2)>>2;
   unsigned b=((a0&255)+(a1&255)+(b0&255)+(b1&255)+2)>>2;
   dst[x]=0xff000000u|(r<<16)|(g<<8)|b;
  }
 }
 box_blur_rect(half_a,HW,0,0,HW,HH,3,1);
 box_blur_rect(half_a,HW,0,0,HW,HH,3,1);
 unsigned dim=50u,ia=255u-alpha;
 for(int y=0;y<HEIGHT;y++){
  int hy=y>>1,hy1=(y&1)&&(hy+1<HH)?hy+1:hy;
  const uint32_t*h0=half_a+hy*HW,*h1=half_a+hy1*HW;
  uint32_t*row=back+y*WIDTH;
  for(int x=0;x<WIDTH;x++){
   int hx=x>>1,hx1=(x&1)&&(hx+1<HW)?hx+1:hx;
   uint32_t p00=h0[hx],p10=h0[hx1],p01=h1[hx],p11=h1[hx1];
   unsigned br=(((p00>>16)&255)+((p10>>16)&255)+((p01>>16)&255)+((p11>>16)&255)+2)>>2;
   unsigned bg=(((p00>>8)&255)+((p10>>8)&255)+((p01>>8)&255)+((p11>>8)&255)+2)>>2;
   unsigned bb=((p00&255)+(p10&255)+(p01&255)+(p11&255)+2)>>2;
   br=(br*dim+127)/255;bg=(bg*dim+127)/255;bb=(bb*dim+127)/255;
   if(alpha>=255){row[x]=0xff000000u|(br<<16)|(bg<<8)|bb;}
   else{
    uint32_t v=row[x];
    unsigned r=(br*alpha+((v>>16)&255)*ia+127)/255;
    unsigned g=(bg*alpha+((v>>8)&255)*ia+127)/255;
    unsigned b=(bb*alpha+(v&255)*ia+127)/255;
    row[x]=0xff000000u|(r<<16)|(g<<8)|b;
   }
  }
 }
}

/* ---- text: proportional anti-aliased glyphs, 1/16 px advances ---- */
static int gl_index(unsigned char c){return(c<32||c>126)?63-32:c-32;}
static int text_w(int f,const char*s){
 const struct face*fc=&FACES[f];unsigned tot=0;
 for(;*s;s++)tot+=fc->g[gl_index((unsigned char)*s)].adv16;
 return(int)((tot+8)>>4);
}
static int paint_glyph(int f,int pen,int base,unsigned index,uint32_t rgb){
 const struct face*fc=&FACES[f];
 const struct glyph*g=&fc->g[index];
 int gx=((pen+8)>>4)+g->bx,gy=base+g->by;
 for(int j=0;j<g->h;j++)for(int i=0;i<g->w;i++){unsigned a=fc->bits[g->off+j*g->w+i];if(a)blend(gx+i,gy+j,rgb,a);}
 return pen+g->adv16;
}
static void text(int f,int x,int base,const char*s,uint32_t rgb){
 int pen=x*16;
 for(;*s;s++)pen=paint_glyph(f,pen,base,(unsigned)gl_index((unsigned char)*s),rgb);
}
static unsigned announcement_glyph(unsigned cp){
 if(cp>=0xa0&&cp<0x250)return 95+cp-0xa0;
 if(cp>=0x2000&&cp<0x2070)return 95+0x250-0xa0+cp-0x2000;
 return (cp>=32&&cp<=126)?cp-32:gl_index(cp=='\t'?' ':'?');
}
struct announcement_line{uint16_t first,last;};
static struct announcement_line announcement_title_lines[BTA_ANNOUNCEMENT_TITLE_CAP];
static struct announcement_line announcement_body_lines[BTA_ANNOUNCEMENT_BODY_CAP];
static int announcement_title_count,announcement_body_count;

static int announcement_wrap(const char*s,int f,int max_width,struct announcement_line*lines){
 const char *end=s+strlen(s),*p=s;
 int count=0;
 while(p<end){
  const char *first=p,*space=0,*next_word=0;
  int width=0;
  while(p<end&&*p!='\n'&&*p!='\r'){
   unsigned cp;
   size_t n=bta_announcement_utf8(p,(size_t)(end-p),&cp);
   if(!n){cp='?';n=1;}
   int advance=FACES[f].g[announcement_glyph(cp)].adv16;
   if(width+advance>max_width*16&&p>first)break;
   if(cp==' '||cp=='\t'){if(p!=next_word)space=p;next_word=p+n;}
   width+=advance;
   p+=n;
  }
  const char *last=p;
  if(p<end&&(*p=='\n'||*p=='\r')){
   if(*p++=='\r'&&p<end&&*p=='\n')p++;
  }else if(p<end&&space&&space>first){
   last=space;
   p=next_word;
  }
  lines[count++]=(struct announcement_line){(uint16_t)(first-s),(uint16_t)(last-s)};
 }
 if(end>s&&(end[-1]=='\r'||end[-1]=='\n'))
  lines[count++]=(struct announcement_line){(uint16_t)(end-s),(uint16_t)(end-s)};
 return count;
}

static void announcement_layout(void){
 announcement_title_count=announcement_wrap(announcement.title,F_DIALOG,444,announcement_title_lines);
 announcement_body_count=announcement_wrap(announcement.body,F_BODY,490,announcement_body_lines);
}

static void announcement_text(int f,int x,int base,const char*s,struct announcement_line line,uint32_t rgb){
 int pen=x*16;
 const char *p=s+line.first,*end=s+line.last;
 while(p<end){
  unsigned cp;
  size_t n=bta_announcement_utf8(p,(size_t)(end-p),&cp);
  if(!n){cp='?';n=1;}
  pen=paint_glyph(f,pen,base,announcement_glyph(cp),rgb);
  p+=n;
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

/* ---- animation state ---- */
enum { SLOT_BTN=0, SLOT_SC, SLOT_DEV_HINT };
enum { POPUP_NONE=0, POPUP_PIN, POPUP_DL_RUN, POPUP_DL_DONE, POPUP_DL_ERR, POPUP_ANNOUNCEMENT };

struct slot_desc{
 int kind,visible,pos_x,pos_y,primary,glyph;
 uint32_t ink;
 unsigned mask1,mask2;
 char label[48],key1[16],key2[16];
};
struct ui_slot{
 struct slot_desc cur,prev;
 int has_prev,active;
 uint64_t t0;
};

static struct{
 int init;
 uint64_t last_us;
 float btn_hold[24];
 /* Device selector highlight glide */
 int sel_valid;
 float sel_cur_y,sel_cur_h,sel_start_y,sel_start_h;
 int sel_target_y,sel_target_h;
 uint64_t sel_t0;
 /* Volume slider glide & thumb pulse */
 int vol_valid,vol_target;
 float vol_cur,vol_start,vol_glow;
 uint64_t vol_t0;
 /* Dynamic widget slots */
 struct ui_slot hdr_dl,mode_btn,tv_btn,dev_btn,dev_hint,sc_vol,sc_test,sc_tri,sc_pwr,sc_close;
 /* Modal popup fade + crossfade */
 int pop_kind,pop_active_kind,pop_prev_kind,pop_sw_active;
 float pop_alpha,pop_start_a,pop_target_a;
 uint64_t pop_t0,pop_sw_t0;
 /* Cached popup state for smooth fade-out after dismissal */
 char snap_pin[5];
 int snap_digit,snap_has_target;
 struct bt_device snap_target;
 uint64_t snap_dl_bytes,snap_dl_total;
 char snap_dl_pkg[128],snap_dl_err[160];
}g_anim;

static void anim_tick_holds(uint64_t dt_us){
 float up=(float)dt_us/(float)ANIM_PRESS_US;
 float dn=(float)dt_us/(float)ANIM_RELEASE_US;
 for(int i=0;i<24;i++){
  if(g_held&(1u<<i))g_anim.btn_hold[i]=clampf(g_anim.btn_hold[i]+up,0.0f,1.0f);
  else g_anim.btn_hold[i]=clampf(g_anim.btn_hold[i]-dn,0.0f,1.0f);
 }
}
static float hold_val(unsigned mask,int allow){
 if(!allow||!mask)return 0.0f;
 float m=0.0f;
 for(int i=0;i<24;i++)if((mask&(1u<<i))&&g_anim.btn_hold[i]>m)m=g_anim.btn_hold[i];
 return m;
}

/* ---- screen ---- */
static void dev_name(const struct bt_device*d,char*out,size_t cap){
 const char*src=d->name;size_t len=strlen(src),n=len;
 while(n>0&&(src[n-1]=='?'||src[n-1]==' '))n--;
 int q=0;for(size_t i=n;i<len;i++)if(src[i]=='?')q++;
 if(n<len&&q<2){n=len;while(n>0&&src[n-1]==' ')n--;}
 if(n==0){snprintf(out,cap,"Unnamed device %04lX",d->address&0xffff);return;}
 if(n>=cap)n=cap-1;
 memcpy(out,src,n);out[n]=0;
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

static void draw_hint_key(int x,int y,int w,int h,int r,uint32_t edge,uint32_t body,
                          int tdx,int tdy,const char*key,uint32_t ink,float hold){
 if(hold<=0.001f){
  outlined_rect(x,y,w,h,r,edge,body);
  text(F_KEY,x+tdx,y+tdy,key,ink);
  return;
 }
 float eh=hold*(2.0f-hold);
 float scale=1.0f-0.20f*eh;
 uint32_t*prev_fb=g_fb;int prev_st=g_stride,prev_cw=g_cw,prev_ch=g_ch;
 int ox0=cx0,oy0=cy0,ox1=cx1,oy1=cy1;
 memset(hint_buf,0,sizeof(hint_buf));
 set_surface(hint_buf,HINT_W,HINT_W,HINT_H);
 int hx=HINT_W/2-w/2,hy=HINT_H/2-h/2;
 outlined_rect(hx,hy,w,h,r,edge,body);
 text(F_KEY,hx+tdx,hy+tdy,key,ink);
 apply_blur_f(hint_buf,hint_tmp,HINT_W,hx-4,hy-4,hx+w+4,hy+h+4,0.65f*eh);
 set_surface(prev_fb,prev_st,prev_cw,prev_ch);
 clip(ox0,oy0,ox1,oy1);
 composite_scaled(g_fb,g_stride,g_cw,g_ch,x+w/2,y+h/2,hint_buf,HINT_W,HINT_H,scale);
}

static void draw_hint_glyph(int id,int cx,int cy,uint32_t ink,unsigned alpha,float hold){
 if(hold<=0.001f){icon_c(id,cx,cy,ink,alpha);return;}
 float eh=hold*(2.0f-hold);
 float scale=1.0f-0.20f*eh;
 uint32_t*prev_fb=g_fb;int prev_st=g_stride,prev_cw=g_cw,prev_ch=g_ch;
 int ox0=cx0,oy0=cy0,ox1=cx1,oy1=cy1;
 memset(hint_buf,0,sizeof(hint_buf));
 set_surface(hint_buf,HINT_W,HINT_W,HINT_H);
 icon_c(id,HINT_W/2,HINT_H/2,ink,alpha);
 int hw=ICONS[id].w/2+4,hh=ICONS[id].h/2+4;
 apply_blur_f(hint_buf,hint_tmp,HINT_W,HINT_W/2-hw,HINT_H/2-hh,HINT_W/2+hw,HINT_H/2+hh,0.65f*eh);
 set_surface(prev_fb,prev_st,prev_cw,prev_ch);
 clip(ox0,oy0,ox1,oy1);
 composite_scaled(g_fb,g_stride,g_cw,g_ch,cx,cy,hint_buf,HINT_W,HINT_H,scale);
}

static int button_w(const char*label,int glyph,const char*key){
 int kw=key?text_w(F_KEY,key)+10:0;
 int gw=glyph>=0?ICONS[glyph].w:0;
 return 28+text_w(F_SMALLB,label)+(kw?kw+8:0)+(gw?gw+8:0);
}
static int button_raw(int right,int y,const char*label,int glyph,const char*key,int primary,float hold){
 int kw=key?text_w(F_KEY,key)+10:0;
 int gw=glyph>=0?ICONS[glyph].w:0;
 int w=28+text_w(F_SMALLB,label)+(kw?kw+8:0)+(gw?gw+8:0);
 int x=right-w,pen=x+14;
 uint32_t ink=primary?BG:TEXT;
 if(primary)rrect(x,y,w,42,7,TEXT);
 else outlined_rect(x,y,w,42,7,TRACK,PANEL);
 if(kw){
  draw_hint_key(pen,y+11,kw,20,4,ink,primary?TEXT:PANEL,5,14,key,ink,hold);
  pen+=kw+8;
 }
 if(gw){
  draw_hint_glyph(glyph,pen+gw/2,y+21,ink,255,hold);
  pen+=gw+8;
 }
 text(F_SMALLB,pen,y+27,label,ink);
 return x;
}

static int shortcut_w(const char*label,int glyph,const char*first,const char*second){
 const char*keys[]={first,second};int x=0;
 for(int i=0;i<2&&keys[i];i++){int w=text_w(F_KEY,keys[i])+14;x+=w+(i==0&&second?5:8);}
 if(glyph>=0)x+=22;
 return x+text_w(F_SMALL,label);
}
static int shortcut_raw(int x,int y,const char*label,int glyph,const char*first,const char*second,
                        uint32_t ink,float h1,float h2){
 const char*keys[]={first,second};
 float holds[]={h1,h2};
 for(int i=0;i<2&&keys[i];i++){
  int w=text_w(F_KEY,keys[i])+14;
  draw_hint_key(x,y,w,25,5,TRACK,BG,7,17,keys[i],ink,holds[i]);
  x+=w+(i==0&&second?5:8);
 }
 if(glyph>=0){
  draw_hint_glyph(glyph,x+8,y+12,ink,255,h1);
  x+=22;
 }
 text(F_SMALL,x,y+18,label,ink);
 return x+text_w(F_SMALL,label);
}

static void button_held(int right,int y,const char*label,int glyph,const char*key,int primary,unsigned mask,int allow){
 float hold=hold_val(mask,allow);
 if(hold<=0.001f){button_raw(right,y,label,glyph,key,primary,0.0f);return;}
 int w=button_w(label,glyph,key),x=right-w;
 int bx0=clampi(x-8,0,g_cw),by0=clampi(y-8,0,g_ch),bx1=clampi(right+8,0,g_cw),by1=clampi(y+50,0,g_ch);
 uint32_t*prev_fb=g_fb;int prev_st=g_stride,prev_cw=g_cw,prev_ch=g_ch;
 int ox0=cx0,oy0=cy0,ox1=cx1,oy1=cy1;
 clear_rect(layer,WIDTH,bx0,by0,bx1,by1);
 set_surface(layer,WIDTH,WIDTH,HEIGHT);
 button_raw(right,y,label,glyph,key,primary,hold);
 float eh=hold*(2.0f-hold);
 apply_blur_f(layer,layer2,WIDTH,bx0,by0,bx1,by1,0.55f*eh);
 set_surface(prev_fb,prev_st,prev_cw,prev_ch);
 clip(ox0,oy0,ox1,oy1);
 composite_rect(g_fb,g_stride,layer,WIDTH,bx0,by0,bx1,by1,255);
}

/* Slot descriptor helpers for state-change crossfade + blur */
static struct slot_desc make_btn_desc(int visible,int right,int y,const char*label,int glyph,const char*key,int primary,unsigned mask){
 struct slot_desc d;memset(&d,0,sizeof(d));
 d.kind=SLOT_BTN;d.visible=visible;d.pos_x=right;d.pos_y=y;d.primary=primary;d.glyph=glyph;d.mask1=mask;
 if(label)snprintf(d.label,sizeof(d.label),"%s",label);
 if(key)snprintf(d.key1,sizeof(d.key1),"%s",key);
 return d;
}
static struct slot_desc make_sc_desc(int visible,int x,int y,const char*label,int glyph,
                                      const char*k1,const char*k2,uint32_t ink,unsigned m1,unsigned m2){
 struct slot_desc d;memset(&d,0,sizeof(d));
 d.kind=SLOT_SC;d.visible=visible;d.pos_x=x;d.pos_y=y;d.glyph=glyph;d.ink=ink;d.mask1=m1;d.mask2=m2;
 if(label)snprintf(d.label,sizeof(d.label),"%s",label);
 if(k1)snprintf(d.key1,sizeof(d.key1),"%s",k1);
 if(k2)snprintf(d.key2,sizeof(d.key2),"%s",k2);
 return d;
}
static struct slot_desc make_dev_hint_desc(int visible){
 struct slot_desc d;memset(&d,0,sizeof(d));
 d.kind=SLOT_DEV_HINT;d.visible=visible;d.pos_x=745;d.pos_y=469;d.mask1=UI_BTN_UP|UI_BTN_DOWN;
 snprintf(d.label,sizeof(d.label),"Choose a device");
 return d;
}
static int slot_desc_diff(const struct slot_desc*a,const struct slot_desc*b){
 return a->kind!=b->kind||a->visible!=b->visible||a->pos_x!=b->pos_x||a->pos_y!=b->pos_y||
        a->primary!=b->primary||a->glyph!=b->glyph||a->ink!=b->ink||
        strcmp(a->label,b->label)!=0||strcmp(a->key1,b->key1)!=0||strcmp(a->key2,b->key2)!=0;
}
static void slot_bounds(const struct slot_desc*d,int*bx0,int*by0,int*bx1,int*by1){
 int x0=0,y0=0,x1=0,y1=0;
 if(d->kind==SLOT_BTN){
  int w=button_w(d->label,d->glyph,d->key1[0]?d->key1:NULL);
  x0=d->pos_x-w;y0=d->pos_y;x1=d->pos_x;y1=d->pos_y+42;
 }else if(d->kind==SLOT_SC){
  int w=shortcut_w(d->label,d->glyph,d->key1[0]?d->key1:NULL,d->key2[0]?d->key2:NULL);
  x0=d->pos_x;y0=d->pos_y;x1=d->pos_x+w;y1=d->pos_y+25;
 }else{
  x0=745;y0=475;x1=774+text_w(F_SMALL,d->label);y1=505;
 }
 *bx0=clampi(x0-10,0,WIDTH);*by0=clampi(y0-10,0,HEIGHT);
 *bx1=clampi(x1+10,0,WIDTH);*by1=clampi(y1+10,0,HEIGHT);
}
static void render_slot_desc(const struct slot_desc*d,float h1,float h2){
 if(d->kind==SLOT_BTN){
  button_raw(d->pos_x,d->pos_y,d->label,d->glyph,d->key1[0]?d->key1:NULL,d->primary,h1);
 }else if(d->kind==SLOT_SC){
  shortcut_raw(d->pos_x,d->pos_y,d->label,d->glyph,d->key1[0]?d->key1:NULL,d->key2[0]?d->key2:NULL,d->ink,h1,h2);
 }else{
  draw_hint_glyph(I_BUPDOWN,756,491,TEXT,255,h1);
  text(F_SMALL,774,496,d->label,MUTED);
 }
}
static void paint_slot_desc(const struct slot_desc*d,unsigned alpha,float trans_blur,int allow_hold){
 if(!d->visible||!alpha)return;
 float h1=hold_val(d->mask1,allow_hold),h2=hold_val(d->mask2,allow_hold);
 float max_h=h1>h2?h1:h2;
 float eh=max_h*(2.0f-max_h);
 float hold_blur=d->kind==SLOT_BTN?0.55f*eh:(d->kind==SLOT_SC?0.45f*eh:0.0f);
 float bf=trans_blur>hold_blur?trans_blur:hold_blur;
 if(alpha>=255&&bf<=0.02f){
  render_slot_desc(d,h1,h2);
  return;
 }
 int bx0,by0,bx1,by1;
 slot_bounds(d,&bx0,&by0,&bx1,&by1);
 clear_rect(layer,WIDTH,bx0,by0,bx1,by1);
 set_surface(layer,WIDTH,WIDTH,HEIGHT);
 render_slot_desc(d,h1,h2);
 apply_blur_f(layer,layer2,WIDTH,bx0,by0,bx1,by1,bf);
 set_surface(back,WIDTH,WIDTH,HEIGHT);
 composite_rect(back,WIDTH,layer,WIDTH,bx0,by0,bx1,by1,alpha);
}
static void draw_slot(struct ui_slot*s,const struct slot_desc*target,int allow_hold){
 if(!g_anim.init){
  s->cur=*target;s->active=0;s->has_prev=0;
 }else if(slot_desc_diff(&s->cur,target)){
  float p=s->active?(float)(g_now-s->t0)/(float)ANIM_STATE_US:1.0f;
  if(!s->active||p>=0.35f){s->prev=s->cur;s->has_prev=s->prev.visible;}
  s->cur=*target;s->t0=g_now;s->active=1;
 }
 if(s->active){
  float p=(float)(g_now-s->t0)/(float)ANIM_STATE_US;
  if(p>=1.0f){s->active=0;}
  else{
   if(s->has_prev&&s->cur.visible){
    if(p<0.65f){
     float u=ease_out3(p/0.65f);
     paint_slot_desc(&s->prev,(unsigned)(255.0f*(1.0f-u)+0.5f),4.0f*u,0);
    }
    if(p>0.35f){
     float u=ease_out3((p-0.35f)/0.65f);
     paint_slot_desc(&s->cur,(unsigned)(255.0f*u+0.5f),4.0f*(1.0f-u),allow_hold);
    }
   }else if(s->cur.visible){
    float u=ease_out3(p);
    paint_slot_desc(&s->cur,(unsigned)(255.0f*u+0.5f),4.0f*(1.0f-u),allow_hold);
   }else if(s->has_prev){
    float u=ease_out3(p);
    paint_slot_desc(&s->prev,(unsigned)(255.0f*(1.0f-u)+0.5f),4.0f*u,0);
   }
   return;
  }
 }
 paint_slot_desc(&s->cur,255,0.0f,allow_hold);
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

static void draw_header(int allow_hold){
 logo(86,31);
 text(F_BRAND,123,51,"BTAudio4",TEXT);
 fill(86,86,1108,1,BORDER);

 int right=1194;
 char detail[TBUF];
 const char*title=NULL;
 int show_dl=(upd_state==BTA_UPD_OUTDATED&&upd_latest[0]&&upd_has_pkg);
 struct slot_desc dl_btn=make_btn_desc(show_dl,1194,22,"Download",-1,"R2",1,UI_BTN_R2);
 draw_slot(&g_anim.hdr_dl,&dl_btn,allow_hold);

 if(upd_state==BTA_UPD_OUTDATED&&upd_latest[0]){
  title="Update available";
  if(upd_has_pkg){
   right=1194-button_w("Download",-1,"R2")-18;
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
 text_r(F_SMALL,right,39,"v" BTA_VERSION,MUTED);
 const char*uid_val=client_uid[0]?client_uid:"...";
 int lw=text_w(F_SMALL,"UID "),uw=text_w(F_SMALLB,uid_val);
 text(F_SMALL,right-lw-uw,58,"UID ",MUTED);
 text(F_SMALLB,right-uw,58,uid_val,client_uid[0]?TEXT:MUTED);
}

static void draw_playback(int l,const struct bt_device*target,int pinmode,uint64_t now,uint64_t dt_us,int allow_hold){
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
 outlined_rect(117,152,84,84,13,TRACK,BORDER);
 int glyph=device_icon(target)==I_LOUDSPEAKER?I_HERO_SPEAKER:I_HERO;
 icon_c(glyph,159,194,target?TEXT:MUTED,255);
 text(F_KICKER,222,178,caption,MUTED);
 int name_width=448;
 if(l!=UI_OFFLINE){
  int status_x=670-text_w(F_BODY,status);
  name_width=status_x-222-27;
  text(F_BODY,status_x,201,status,MUTED);
  if(l==BT_LINK_CONNECTING||l==BT_LINK_SEARCHING)
   spinner(status_x-13,195,6,MUTED,now,8,1);
  else
   disc(status_x-12,195,3,playing?TEXT:MUTED,255);
  if(playing&&ext_ok&&meter_s>0){
   int level=meter_s>100?100:meter_s;
   rrect(status_x,214,670-status_x,3,1,TRACK);
   fill(status_x,214,(670-status_x)*level/100,3,MUTED);
  }
 }
 text_fit(F_TITLE,222,221,name,TEXT,name_width);
 fill(117,252,553,1,BORDER);
 text(F_LABEL,117,285,"Headphone volume",TEXT);
 int volume=state.volume<0?0:state.volume>100?100:state.volume;
 if(!g_anim.init||!g_anim.vol_valid){
  g_anim.vol_valid=1;
  g_anim.vol_cur=g_anim.vol_start=(float)volume;
  g_anim.vol_target=volume;
  g_anim.vol_t0=g_now;
  g_anim.vol_glow=0.0f;
 }else{
  if(volume!=g_anim.vol_target){
   g_anim.vol_start=g_anim.vol_cur;
   g_anim.vol_target=volume;
   g_anim.vol_t0=g_now;
  }
  float vp=(float)(g_now-g_anim.vol_t0)/(float)ANIM_VOL_US;
  g_anim.vol_cur=g_anim.vol_start+((float)g_anim.vol_target-g_anim.vol_start)*ease_out3(vp);
  float vh=hold_val(UI_BTN_L1|UI_BTN_R1|UI_BTN_LEFT|UI_BTN_RIGHT,allow_hold&&online);
  float moving=(vp<1.0f)?(1.0f-vp)*0.65f:0.0f;
  float want_glow=vh>moving?vh:moving;
  float up=(float)dt_us/(float)ANIM_PRESS_US,dn=(float)dt_us/(float)ANIM_RELEASE_US;
  if(want_glow>g_anim.vol_glow)g_anim.vol_glow=clampf(g_anim.vol_glow+up,0.0f,want_glow);
  else g_anim.vol_glow=clampf(g_anim.vol_glow-dn,want_glow,1.0f);
 }
 snprintf(buf,sizeof(buf),"%d%%",volume);
 text_r(F_NAME,670,285,buf,TEXT);
 rrect(117,303,553,6,3,TRACK);
 int thumb=117+(int)((553.0f*g_anim.vol_cur)/100.0f+0.5f);
 rrect(117,303,thumb-117,6,3,TEXT);
 if(g_anim.vol_glow>0.01f){
  float ge=g_anim.vol_glow*(2.0f-g_anim.vol_glow);
  unsigned halo_a=(unsigned)(52.0f*ge+0.5f);
  int halo_r=10+(int)(3.0f*ge+0.5f);
  int thumb_r=7+(int)(2.0f*ge+0.5f);
  if(halo_a)disc(thumb,306,halo_r,TEXT,halo_a);
  disc(thumb,306,thumb_r,TEXT,255);
 }else{
  disc(thumb,306,7,TEXT,255);
 }
 if(playing&&ext_ok)
  text(F_BODY,117,335,state.volume_synced?"Synced with headphones":"Software volume",MUTED);

 const char*notice_text=NULL;
 if(notice[0]&&g_now<notice_until)notice_text=notice;
 else if(l==UI_OFFLINE)notice_text="Press X or L2 to start the audio service. If it stays offline, check that GoldHEN's BinLoader is enabled.";
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
 if(notice_text)paragraph(F_SMALL,117,364,553,18,notice_text,MUTED);

 fill(117,398,553,1,BORDER);
 static const char*const preset_names[]={"Low latency","Balanced","High stability"};
 int pre=(ext_ok&&state.preset>=0&&state.preset<BTA_PRESET_COUNT)?state.preset:BTA_PRESET_BALANCED;
 text(F_BODY,117,435,"Audio mode",TEXT);
 text(F_SMALL,232,435,l==UI_OFFLINE?"Unavailable":preset_names[pre],MUTED);
 struct slot_desc mode_d=make_btn_desc(l!=UI_OFFLINE,670,408,"Switch",-1,"PAD",0,UI_BTN_TOUCHPAD);
 draw_slot(&g_anim.mode_btn,&mode_d,allow_hold);

 fill(117,460,553,1,BORDER);
 icon_c(I_DISPLAY,128,493,MUTED,255);
 text(F_BODY,151,499,"TV sound",TEXT);
 text(F_SMALL,232,499,l==UI_OFFLINE?"Unavailable":state.tv?"On":"Muted",MUTED);
 struct slot_desc tv=make_btn_desc(l!=UI_OFFLINE,670,472,state.tv?"Mute":"Unmute",I_BSQUARE,NULL,0,UI_BTN_SQUARE);
 draw_slot(&g_anim.tv_btn,&tv,allow_hold);
}

static void draw_devices(int l,int n,int pinmode,uint64_t now,int allow_hold){
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
  g_anim.sel_valid=0;
 }else{
  int step=n>3?47:69;
  int h=n>3?44:66;
  int y0=n>3?199:207;
  int ty=y0+(focus-top)*step,th=h;
  if(!g_anim.init||!g_anim.sel_valid){
   g_anim.sel_valid=1;
   g_anim.sel_cur_y=g_anim.sel_start_y=(float)ty;
   g_anim.sel_cur_h=g_anim.sel_start_h=(float)th;
   g_anim.sel_target_y=ty;g_anim.sel_target_h=th;
   g_anim.sel_t0=g_now;
  }else{
   if(ty!=g_anim.sel_target_y||th!=g_anim.sel_target_h){
    g_anim.sel_start_y=g_anim.sel_cur_y;
    g_anim.sel_start_h=g_anim.sel_cur_h;
    g_anim.sel_target_y=ty;g_anim.sel_target_h=th;
    g_anim.sel_t0=g_now;
   }
   float p=(float)(g_now-g_anim.sel_t0)/(float)ANIM_SEL_US;
   float e=ease_out3(p);
   g_anim.sel_cur_y=g_anim.sel_start_y+((float)g_anim.sel_target_y-g_anim.sel_start_y)*e;
   g_anim.sel_cur_h=g_anim.sel_start_h+((float)g_anim.sel_target_h-g_anim.sel_start_h)*e;
  }
  clip(721,195,1194,446);
  outlined_rect(745,(int)(g_anim.sel_cur_y+0.5f),425,(int)(g_anim.sel_cur_h+0.5f),8,MUTED,BORDER);
  clip(0,0,WIDTH,HEIGHT);

  for(int i=top;i<n&&i<top+5;i++){
   const struct bt_device*d=&state.devices[i];
   int selected=i==focus,current=d->address==state.selected&&l!=BT_LINK_IDLE&&l!=UI_OFFLINE;
   int y=y0+(i-top)*step,cy=y+h/2;
   char name[TBUF];
   dev_name(d,name,sizeof(name));
   icon_c(device_icon(d),770,cy,selected?TEXT:MUTED,255);
   text_fit(F_NAME,793,cy+7,name,TEXT,330);
   if(current){
    if(l==BT_LINK_STREAMING)icon_c(I_CHECK,1145,cy,TEXT,255);
    else if(l==BT_LINK_CONNECTING||l==BT_LINK_SEARCHING||pinmode)
     spinner(1145,cy,7,MUTED,now,8,1);
    else icon_c(I_BANG,1145,cy,MUTED,255);
   }
  }
 }
 fill(746,448,423,1,BORDER);
 const char*label=NULL;
 int glyph=I_BCROSS;
 int show_choose=0;
 if(l==UI_OFFLINE)label="Retry";
 else if(focus>=0&&focus<n){
  int current=l!=BT_LINK_IDLE&&state.devices[focus].address==state.selected;
  label=current?(l==BT_LINK_STREAMING?"Disconnect":"Stop trying"):"Connect";
  if(n>1)show_choose=1;
 }else if(l==BT_LINK_IDLE&&!state.scanning){
  label="Scan";
  glyph=I_BTRI;
 }else if(l!=BT_LINK_IDLE){
  label=l==BT_LINK_STREAMING?"Disconnect":"Stop trying";
  glyph=I_BTRI;
 }
 struct slot_desc hint_d=make_dev_hint_desc(show_choose);
 draw_slot(&g_anim.dev_hint,&hint_d,allow_hold);
 struct slot_desc btn_d=make_btn_desc(label!=NULL,1169,469,label,glyph,NULL,1,
                                      glyph==I_BCROSS?UI_BTN_CROSS:UI_BTN_TRIANGLE);
 draw_slot(&g_anim.dev_btn,&btn_d,allow_hold);
}

static void draw_shortcuts(int l,int n,int allow_hold){
 fill(86,573,1108,1,BORDER);
 int x=86;
 struct slot_desc vol_d=make_sc_desc(1,x,592,"Volume",-1,"L1","R1",MUTED,UI_BTN_L1,UI_BTN_R1);
 draw_slot(&g_anim.sc_vol,&vol_d,allow_hold);
 x+=shortcut_w("Volume",-1,"L1","R1")+26;

 int show_test=(l==BT_LINK_STREAMING);
 struct slot_desc test_d=make_sc_desc(show_test,x,592,"Test sound",-1,"OPTIONS",NULL,MUTED,UI_BTN_OPTIONS,0);
 draw_slot(&g_anim.sc_test,&test_d,allow_hold);
 if(show_test)x+=shortcut_w("Test sound",-1,"OPTIONS",NULL)+26;

 const char*tri_lbl=NULL;
 if(l==BT_LINK_IDLE&&n>0)tri_lbl="Scan";
 else if(l!=UI_OFFLINE&&l!=BT_LINK_IDLE&&focus>=0&&focus<n&&state.devices[focus].address!=state.selected)
  tri_lbl=l==BT_LINK_STREAMING?"Disconnect":"Stop trying";
 struct slot_desc tri_d=make_sc_desc(tri_lbl!=NULL,x,592,tri_lbl,I_BTRI,NULL,NULL,MUTED,UI_BTN_TRIANGLE,0);
 draw_slot(&g_anim.sc_tri,&tri_d,allow_hold);

 const char*pwr=l==UI_OFFLINE?"Start payload":"Unload payload";
 int close_x=1194-text_w(F_SMALL,"Close")-22;
 int pwr_w=text_w(F_KEY,"L2")+14+8+text_w(F_SMALL,pwr);
 struct slot_desc pwr_d=make_sc_desc(1,close_x-26-pwr_w,592,pwr,-1,"L2",NULL,MUTED,UI_BTN_L2,0);
 draw_slot(&g_anim.sc_pwr,&pwr_d,allow_hold);

 struct slot_desc close_d=make_sc_desc(1,close_x,592,"Close",I_BCIRCLE,NULL,NULL,MUTED,UI_BTN_CIRCLE,0);
 draw_slot(&g_anim.sc_close,&close_d,allow_hold);
}

/* Dialogs share a centered surface with left-aligned copy and bottom actions. */
static void dialog_frame(int y,int h,int icon_id,const char*mark,const char*title){
 outlined_rect(360,y,560,h,15,TRACK,PANEL);
 outlined_rect(390,y+28,43,43,9,TRACK,BORDER);
 if(icon_id>=0)icon_c(icon_id,411,y+49,TEXT,255);
 else text(F_SECTION,411-text_w(F_SECTION,mark)/2,y+57,mark,TEXT);
 if(title)text(F_DIALOG,390,y+110,title,TEXT);
 fill(390,y+h-85,500,1,BORDER);
}

static void draw_pin(const struct bt_device*target,const char*pin,int digit,int allow_hold){
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
 int x=890-button_w("Submit PIN",I_BCROSS,NULL);
 button_held(890,y+h-66,"Submit PIN",I_BCROSS,NULL,1,UI_BTN_CROSS,allow_hold);
 button_held(x-12,y+h-66,"Cancel",I_BCIRCLE,NULL,0,UI_BTN_CIRCLE,allow_hold);
}

static void draw_download_kind(int kind,uint64_t bytes,uint64_t total,const char*pkg_in,const char*err_in,
                               uint64_t now,int allow_hold){
 char buf[TBUF];
 const char*pkg=pkg_in[0]?pkg_in:"update package";
 if(kind==POPUP_DL_RUN){
  int y=185,h=350;
  dialog_frame(y,h,I_DOWNLOAD,NULL,"Downloading update");
  snprintf(buf,sizeof(buf),"Saving %s to /data/pkg/.",pkg);
  paragraph(F_BODY,390,y+141,500,20,buf,MUTED);
  char received[32];
  fmt_bytes(bytes,received,sizeof(received));
  rrect(390,y+230,500,6,3,TRACK);
  if(total){
   double ratio=bytes>=total?1.0:(double)bytes/(double)total;
   char tot_s[32];
   fmt_bytes(total,tot_s,sizeof(tot_s));
   snprintf(buf,sizeof(buf),"%d%%",(int)(ratio*100));
   text(F_SMALLB,390,y+215,buf,TEXT);
   snprintf(buf,sizeof(buf),"%s / %s",received,tot_s);
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
  button_held(890,y+h-66,"Cancel",I_BCIRCLE,NULL,0,UI_BTN_CIRCLE,allow_hold);
 }else if(kind==POPUP_DL_DONE){
  int y=127,h=466;
  dialog_frame(y,h,I_CHECK,NULL,"Update ready to install");
  snprintf(buf,sizeof(buf),"Saved to /data/pkg/%s.",pkg);
  paragraph(F_BODY,390,y+141,500,20,buf,MUTED);
  outlined_rect(390,y+187,500,171,8,BORDER,BG);
  text(F_SMALL,406,y+213,"1. Close BTAudio4 from the PS4 home screen.",TEXT);
  paragraph(F_SMALL,406,y+241,462,19,"2. Open GoldHEN > Debug Settings > Package Installer.",TEXT);
  snprintf(buf,sizeof(buf),"Select %s.",pkg);
  paragraph(F_SMALL,423,y+281,446,19,buf,TEXT);
  paragraph(F_SMALL,406,y+322,462,19,"3. If the PS4 asks whether to continue installing, select Yes.",TEXT);
  button_held(890,y+h-66,"Dismiss",I_DISMISS,NULL,1,UI_BTN_CROSS|UI_BTN_CIRCLE,allow_hold);
 }else{
  int y=210,h=300;
  dialog_frame(y,h,I_BANG,NULL,"Download failed");
  paragraph(F_BODY,390,y+143,500,20,err_in[0]?err_in:"Could not download the update package.",MUTED);
  int x=890-button_w("Retry",-1,"R2");
  button_held(890,y+h-66,"Retry",-1,"R2",1,UI_BTN_R2,allow_hold);
  button_held(x-12,y+h-66,"Dismiss",I_DISMISS,NULL,0,UI_BTN_CROSS|UI_BTN_CIRCLE,allow_hold);
 }
}

#define ANNOUNCEMENT_MAX_H 520
#define ANNOUNCEMENT_LINE_H 23
#define ANNOUNCEMENT_TITLE_LINE_H 34
/* Top margin, 40 px icon, and gap before the text. */
static int announcement_body_offset(void){
 return 78+(announcement_title_count-1)*ANNOUNCEMENT_TITLE_LINE_H;
}
#define ANNOUNCEMENT_FOOTER_H 88
static int announcement_row_count(void){
 int max_rows=(ANNOUNCEMENT_MAX_H-announcement_body_offset()-ANNOUNCEMENT_FOOTER_H)/ANNOUNCEMENT_LINE_H;
 if(max_rows<1)max_rows=1;
 return announcement_body_count<max_rows?announcement_body_count:max_rows;
}
static int announcement_height(void){
 return announcement_body_offset()+announcement_row_count()*ANNOUNCEMENT_LINE_H+ANNOUNCEMENT_FOOTER_H;
}
static int announcement_scroll_limit(void){
 int remaining=announcement_body_count-announcement_row_count();
 return remaining>0?remaining:0;
}
static void draw_announcement(int allow_hold){
 const int h=announcement_height(),y=(HEIGHT-h)/2;
 outlined_rect(360,y,560,h,15,TRACK,PANEL);
 outlined_rect(390,y+24,40,40,9,TRACK,BORDER);
 icon_c(I_BANG,410,y+44,TEXT,255);
 for(int i=0;i<announcement_title_count;i++)
  announcement_text(F_DIALOG,446,y+54+i*ANNOUNCEMENT_TITLE_LINE_H,announcement.title,announcement_title_lines[i],TEXT);
 int body_top=y+announcement_body_offset();
 int rows=announcement_row_count(),limit=announcement_scroll_limit();
 for(int i=0;i<rows&&announcement_scroll+i<announcement_body_count;i++)
  announcement_text(F_BODY,390,body_top+17+i*ANNOUNCEMENT_LINE_H,announcement.body,announcement_body_lines[announcement_scroll+i],MUTED);
 if(limit){
  int track_h=rows*ANNOUNCEMENT_LINE_H;
  int thumb_h=track_h*rows/announcement_body_count;
  if(thumb_h<12)thumb_h=12;
  fill(886,body_top,3,track_h,TRACK);
  fill(886,body_top+(track_h-thumb_h)*announcement_scroll/limit,3,thumb_h,MUTED);
 }
 fill(390,y+h-74,500,1,BORDER);
 if(announcement_save_error)text(F_SMALL,390,y+h-40,"Could not save dismissal. Try again.",MUTED);
 else if(limit)shortcut_raw(390,y+h-56,"Scroll",I_BUPDOWN,NULL,NULL,MUTED,0,0);
 if(announcement_close_at&&g_now>=announcement_close_at){
  button_held(890,y+h-65,"Close",I_BCROSS,NULL,1,UI_BTN_CROSS,allow_hold);
 }else{
  uint64_t remaining=announcement_close_at>g_now?announcement_close_at-g_now:BTA_ANNOUNCEMENT_LOCK_US;
  char label[24];
  snprintf(label,sizeof(label),"Close (%u)",(unsigned)((remaining+999999)/1000000));
  int w=button_w(label,-1,NULL);
  outlined_rect(890-w,y+h-65,w,42,9,BORDER,PANEL);
  text(F_LABEL,890-w+(w-text_w(F_LABEL,label))/2,y+h-39,label,MUTED);
 }
}

static void popup_bounds(int kind,int*bx0,int*by0,int*bx1,int*by1){
 int y=180,h=360;
 if(kind==POPUP_DL_RUN){y=185;h=350;}
 else if(kind==POPUP_DL_DONE){y=127;h=466;}
 else if(kind==POPUP_DL_ERR){y=210;h=300;}
 else if(kind==POPUP_ANNOUNCEMENT){h=announcement_height();y=(HEIGHT-h)/2;}
 *bx0=360-14;*by0=y-14;*bx1=360+560+14;*by1=y+h+14;
}

static void paint_popup_card(int kind,uint64_t now,unsigned alpha,float bf,int allow_hold){
 if(kind==POPUP_NONE||!alpha)return;
 int bx0,by0,bx1,by1;
 popup_bounds(kind,&bx0,&by0,&bx1,&by1);
 clear_rect(layer,WIDTH,bx0,by0,bx1,by1);
 set_surface(layer,WIDTH,WIDTH,HEIGHT);
 if(kind==POPUP_PIN){
  draw_pin(g_anim.snap_has_target?&g_anim.snap_target:NULL,g_anim.snap_pin,g_anim.snap_digit,allow_hold);
 }else if(kind==POPUP_ANNOUNCEMENT){
  draw_announcement(allow_hold);
 }else{
  draw_download_kind(kind,g_anim.snap_dl_bytes,g_anim.snap_dl_total,
                     g_anim.snap_dl_pkg,g_anim.snap_dl_err,now,allow_hold);
 }
 apply_blur_f(layer,layer2,WIDTH,bx0,by0,bx1,by1,bf);
 set_surface(back,WIDTH,WIDTH,HEIGHT);
 composite_rect(back,WIDTH,layer,WIDTH,bx0,by0,bx1,by1,alpha);
}

static void draw_popups(int pinmode,const struct bt_device*target,const char*pin,int digit,uint64_t now){
 int req=POPUP_NONE;
 if(pinmode)req=POPUP_PIN;
 else if(dl_state==BTA_DL_RUNNING)req=POPUP_DL_RUN;
 else if(dl_state==BTA_DL_DONE)req=POPUP_DL_DONE;
 else if(dl_state==BTA_DL_ERROR)req=POPUP_DL_ERR;
 else if(announcement_visible)req=POPUP_ANNOUNCEMENT;

 if(req==POPUP_PIN){
  memcpy(g_anim.snap_pin,pin,5);
  g_anim.snap_digit=digit;
  g_anim.snap_has_target=(target!=NULL);
  if(target)g_anim.snap_target=*target;
 }else if(req>=POPUP_DL_RUN&&req<=POPUP_DL_ERR){
  g_anim.snap_dl_bytes=dl_bytes;
  g_anim.snap_dl_total=dl_total;
  snprintf(g_anim.snap_dl_pkg,sizeof(g_anim.snap_dl_pkg),"%s",dl_pkg_name);
  snprintf(g_anim.snap_dl_err,sizeof(g_anim.snap_dl_err),"%s",dl_err);
 }

 if(!g_anim.init){
  g_anim.pop_kind=req;
  g_anim.pop_active_kind=req;
  g_anim.pop_prev_kind=POPUP_NONE;
  g_anim.pop_sw_active=0;
  g_anim.pop_alpha=g_anim.pop_start_a=g_anim.pop_target_a=(req!=POPUP_NONE)?1.0f:0.0f;
  g_anim.pop_t0=g_now;
 }else{
  float want=(req!=POPUP_NONE)?1.0f:0.0f;
  if(req!=POPUP_NONE&&g_anim.pop_active_kind!=POPUP_NONE&&g_anim.pop_active_kind!=req&&g_anim.pop_alpha>0.02f){
   g_anim.pop_prev_kind=g_anim.pop_active_kind;
   g_anim.pop_sw_t0=g_now;
   g_anim.pop_sw_active=1;
  }
  if(req!=POPUP_NONE)g_anim.pop_active_kind=req;
  if(want!=g_anim.pop_target_a){
   g_anim.pop_start_a=g_anim.pop_alpha;
   g_anim.pop_target_a=want;
   g_anim.pop_t0=g_now;
  }
  float p=(float)(g_now-g_anim.pop_t0)/(float)ANIM_POPUP_US;
  g_anim.pop_alpha=g_anim.pop_start_a+(g_anim.pop_target_a-g_anim.pop_start_a)*ease_out3(p);
  if(g_anim.pop_target_a==0.0f&&g_anim.pop_alpha<=0.001f){
   g_anim.pop_alpha=0.0f;
   g_anim.pop_active_kind=POPUP_NONE;
   g_anim.pop_sw_active=0;
  }
  g_anim.pop_kind=req;
 }

 if(g_anim.pop_alpha<=0.001f||g_anim.pop_active_kind==POPUP_NONE)return;

 unsigned bg_alpha=(unsigned)(255.0f*g_anim.pop_alpha+0.5f);
 blur_dim_backdrop(bg_alpha);

 if(g_anim.pop_sw_active){
  float p=(float)(g_now-g_anim.pop_sw_t0)/(float)ANIM_POPUP_US;
  if(p>=1.0f){g_anim.pop_sw_active=0;}
  else{
   if(p<0.65f){
    float u=ease_out3(p/0.65f);
    paint_popup_card(g_anim.pop_prev_kind,now,(unsigned)((float)bg_alpha*(1.0f-u)+0.5f),5.0f*u,0);
   }
   if(p>0.35f){
    float u=ease_out3((p-0.35f)/0.65f);
    paint_popup_card(g_anim.pop_active_kind,now,(unsigned)((float)bg_alpha*u+0.5f),5.0f*(1.0f-u),req!=POPUP_NONE);
   }
   return;
  }
 }
 paint_popup_card(g_anim.pop_active_kind,now,bg_alpha,5.0f*(1.0f-g_anim.pop_alpha),req!=POPUP_NONE);
}

static void draw(int pinmode,const char*pin,int digit){
 uint64_t now=g_now/1000;
 uint64_t dt_us=0;
 if(g_anim.init){
  dt_us=g_now>=g_anim.last_us?(g_now-g_anim.last_us):0;
  if(dt_us>100000ULL)dt_us=100000ULL;
  anim_tick_holds(dt_us);
 }
 g_anim.last_us=g_now;

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

 int modal_open=pinmode||(dl_state!=BTA_DL_NONE)||announcement_visible;
 int allow_bg_hold=!modal_open;

 set_surface(back,WIDTH,WIDTH,HEIGHT);
 fill(0,0,WIDTH,HEIGHT,BG);
 draw_header(allow_bg_hold);
 draw_playback(l,target,pinmode,now,dt_us,allow_bg_hold);
 draw_devices(l,n,pinmode,now,allow_bg_hold);
 draw_shortcuts(l,n,allow_bg_hold);
 draw_popups(pinmode,target,pin,digit,now);

 g_anim.init=1;
}

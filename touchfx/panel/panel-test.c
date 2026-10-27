/* SPDX-License-Identifier: GPL-3.0-only */
/* STEMS audio test requires separately backed-up test azstem; sampler uses its separately versioned audio test. */
#define _GNU_SOURCE
#include "azmod-mod.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define ID "mrdjjag-touchfx-test"
static const az_core_v1 *C;
#include "stem_mask_display.h"
#include "geometry_bridge.h"
#include "stem_visuals.h"
static az_surface *button_surface, *panel_surface;
static int audio_row=-1,grid_row=-1,sampler_audio_row=-1;
static int stem_layout_row=-1,sampler_layout_row=-1,native_touch_row=-1,bank_buttons_row=-1;
/* Separate persisted choices; transparent preserves the accepted STEMS test. */
static int panel_layout(int drum){return C->row_value(drum?sampler_layout_row:stem_layout_row);}
static int mic2_mid=-1;
static int enable_row=-1, dim_row=-1, follow_row=-1, assign_row[4];
static int launcher_row=-1,launcher_choice=-1;
static int pointer_style_row=-1,pointer_colour_row=-1,trail_size_row=-1;
static long now_ms(void);
static uint32_t pointer_rainbow(long now){
    static const uint32_t wheel[]={0xff3030,0xffe030,0x30e030,0x30e0ff,0x4060ff,0xe030ff,0xff3030};
    unsigned phase=(unsigned)(now<0?0:now%12288)/8,segment=phase/256,f=phase%256;
    uint32_t a=wheel[segment],b=wheel[segment+1],out=0;
    for(int shift=0;shift<=16;shift+=8){unsigned av=(a>>shift)&255,bv=(b>>shift)&255;out|=((av*(256-f)+bv*f+128)/256)<<shift;}
    return out;
}
static uint32_t pointer_colour(void){static const uint32_t colours[]={0x38c4ff,0xffffff,0xff3030,0x22e022};int k=C->row_value(pointer_colour_row);return k==4?pointer_rainbow(now_ms()):colours[k>=0&&k<4?k:0];}
static int selected=-1, opened, active, pending=-1, was_follow;
static int kind[4]; /* 0 unknown, 1 STEMS, 2 Drum Roll (visual context only) */
static az_deck_info decks[4];
static int (*stem_gain_read)(int,int);
static const uint32_t stem_colours[4]={0x22e022,0xff3030,0xff33cc,0x4060ff};
static int stem_level(int d,int k) {
    if(d<0||d>=4||k<0||k>=4)return 0;
    int mask=stem_mask_value(d);
    if(mask>=0&&!(mask&(1<<k)))return 0;
    if(!C->row_value(dim_row))return 1000;
    return stem_visual_level(decks[d].stems_on==1,stem_gain_read?stem_gain_read(d,k):1000);
}
static unsigned previous_mask, open_mask;
static long pending_at, redraw_at;
static uint64_t mixer_state;
static long now_ms(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (long)(ts.tv_sec*1000L+ts.tv_nsec/1000000L);
}
static uint64_t decode(const unsigned char *b) {
    uint64_t s=UINT64_C(1)<<63;
    for(int d=0;d<4;d++) s|=(uint64_t)(((unsigned)b[10+d]<<2)|((b[14]>>(6-2*d))&3))<<(10*d);
    return s|((uint64_t)(((unsigned)b[15]<<2)|(b[16]&3))<<40);
}
static int frame(int bus,unsigned char *b,int len,void *u) {
    (void)u;
    if(bus==AZ_BUS_MIXER_IN&&len>=0x4c)__atomic_store_n(&mic2_mid,((unsigned)b[0x48]<<2)|((b[0x4b]>>4)&3),__ATOMIC_RELAXED);
    if(bus==AZ_BUS_MIXER_IN && len>=17)
        __atomic_store_n(&mixer_state,decode(b),__ATOMIC_RELEASE);
    return AZ_FRAME_OK;
}
static int only(unsigned m) {
    if(!m || (m&(m-1))) return -1;
    for(int d=0;d<4;d++) if(m==(1u<<d)) return d;
    return -1;
}
static void follow(long now) {
    uint64_t s=__atomic_load_n(&mixer_state,__ATOMIC_ACQUIRE);
    int configured=1,follow_on=C->row_value(follow_row)!=0;
    int assignments[4];
    for(int d=0;d<4;d++) {
        assignments[d]=C->row_value(assign_row[d]);
        if(assignments[d]<1 || assignments[d]>3) configured=0;
    }
    if(!(s>>63) || !follow_on || !configured) {pending=-1;was_follow=0;return;}
    unsigned mask=0,cross=(unsigned)(s>>40)&1023u;
    for(int d=0;d<4;d++) {
        unsigned v=(unsigned)(s>>(10*d))&1023u;
        if(v>=((open_mask&(1u<<d))?4u:8u)) mask|=1u<<d;
    }
    open_mask=mask;
    for(int d=0;d<4;d++) {
        int a=assignments[d]; /* 1=A, 2=THRU, 3=B */
        if(a!=2 && !((cross>3 || a==1) && (cross<1020 || a==3))) mask&=~(1u<<d);
    }
    /* An empty internal deck cannot make the audible loaded target ambiguous.
     * Unknown load status stays conservative; physical fader hysteresis is unchanged. */
    for(int d=0;d<4;d++)if(decks[d].loaded==0)mask&=~(1u<<d);
    if(!was_follow) {was_follow=1;previous_mask=mask;pending=-1;return;}
    if(mask!=previous_mask) {previous_mask=mask;pending=only(mask);pending_at=now;}
    if(pending>=0 && now-pending_at>=80) {
        int d=pending;pending=-1;
        if(decks[d].loaded==1) {selected=d;C->cfg_set(ID,"deck",d);}
    }
}
/* SDK rendering of the compact v51 arrangement. No firmware images or fonts. */
static az_rect panel_area={48,54,1232,446};
struct visual_state {int effect,rate,sound,x,y,latch,point;};
static struct visual_state visual[4];
/* Independent per-deck choices for STEMS and Drum Roll. No audio buffers/files. */
static struct visual_state remembered_visual[4][2];
static int geometry_ready,button_deck=-2;
static az_rect local_rect(az_rect a,int x,int y,int w,int h) {
    return (az_rect){a.x+x*a.w/1232,a.y+y*a.h/446,w*a.w/1232,h*a.h/446};
}
static int inside(az_rect r,int x,int y) {return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;}
static az_rect channel_rect(az_rect a,int d) {return local_rect(a,958+42*d,2,36,36);}
static az_rect launcher_rect(void){return C->row_value(launcher_row)==1?(az_rect){872,19,64,30}:(az_rect){1090,8,182,48};}
static void launcher_place(void){
    int choice=C->row_value(launcher_row)==1;
    if(choice!=launcher_choice){launcher_choice=choice;C->surface_place(button_surface,launcher_rect());C->surface_redraw(button_surface);}
}
static int rate_columns(int deck){return deck>=0&&deck<4&&kind[deck]==2?6:8;}
static int rate_for_column(int deck,int column){static const int rates[8]={9,5,4,7,3,2,1,0};return deck>=0&&deck<4&&kind[deck]==1?rates[column<0?0:column>7?7:column]:column;}
static az_rect effect_rect(az_rect a,int k) {return local_rect(a,16+129*k,2,120,36);}
static az_rect bank_rect(az_rect a,int k){return local_rect(a,661+30*k,2,27,36);}
static az_rect sound_rect(az_rect a,int k) {return local_rect(a,16+288*k,42,276,28);}
static az_rect field_rect(az_rect a) {return local_rect(a,0,80,1168,336);}
static az_rect rate_rect(az_rect a,int drum,int k){if(drum)return local_rect(a,16+k*194,420,180,24);int x=k*1164/8,next=(k+1)*1164/8;return local_rect(a,16+x,420,next-x-14,24);}
static void (*stem_fx_send)(int,int,int,int,int,int);
static int (*stem_fx_abi)(void) __attribute__((unused));
static int (*sampler_abi)(void) __attribute__((unused));
static int (*sampler_state)(int);
static void (*sampler_fx_send)(int,int,int,int,int,int);
static void (*sampler_xy_send)(int,int,int,int,int);
static void (*sampler_select)(int,int);
static int (*sampler_ui_pick)(int,int);
static int (*sampler_bank)(int);
static void (*sampler_quantize)(int,int);

static void (*stem_fx_quantize)(int,int);
static void (*stem_fx_volume)(int);
static uint64_t touch_bounds,touch_sample,stem_label_bounds,touch_info_hole;
static unsigned touch_context=1,touch_owner_context;
static int touch_owned,touch_blocked;
static uint64_t touch_owner_bounds;
static unsigned touch_serial,last_touch_serial;
static int touching,audio_previous=-1,audio_last_effect=-1,audio_armed;
#include "touch_trail.h"
static az_rect audio_last_area;
static int audio_live,control_last_kind=-1;
static int pointer_poll_on;
static uint64_t pointer_heartbeat;
static int pointer_alive(void){
    int on=__atomic_load_n(&pointer_poll_on,__ATOMIC_ACQUIRE);
    return on>=0&&(!on||(uint64_t)now_ms()-__atomic_load_n(&pointer_heartbeat,__ATOMIC_ACQUIRE)<200);
}
static int tracking_ready(void) {
    return active&&opened&&geometry_ready&&pointer_alive()&&selected>=0&&decks[selected].loaded==1&&C->view&&C->view()==AZ_VIEW_DECK;
}
static int audio_ready(void) {
    if(!tracking_ready())return 0;
    if(kind[selected]==2)return C->row_value(sampler_audio_row)&&sampler_state&&sampler_fx_send&&sampler_xy_send&&sampler_select&&((sampler_state(selected)&0x180)==0x180);
    return kind[selected]==1&&decks[selected].stems_on==1&&C->row_value(audio_row)&&stem_fx_send;
}
static uint64_t pack_touch_rect(az_rect r) {
    return (unsigned)r.x|((uint64_t)(unsigned)r.y<<11)|((uint64_t)(unsigned)r.w<<22)|((uint64_t)(unsigned)r.h<<33);
}
static int touch_in(uint64_t b,int x,int y){
    int bx=(int)(b&2047),by=(int)((b>>11)&2047),w=(int)((b>>22)&2047),h=(int)((b>>33)&2047);
    return b&&x>=bx&&x<bx+w&&y>=by&&y<by+h;
}
static void publish_touch(unsigned context,int down,int x,int y){
    unsigned key=(context<<12)|(++touch_serial&4095u);
    __atomic_store_n(&touch_sample,((uint64_t)key<<32)|(down?(1u<<22):0)|((unsigned)x&2047)|(((uint64_t)(unsigned)y&2047)<<11),__ATOMIC_RELEASE);
}
/* Worker-owned contact stays captured until a real lift, including over labels.
 * A cancelled or label-started contact cannot arm another control mid-drag. */
static int audio_touch(const az_touch *t,void *u) {
    (void)u;if(t->slot!=0)return AZ_TOUCH_PASS;
    if(!t->down){
        int taken=touch_owned||touch_blocked;
        if(touch_owned)publish_touch(touch_owner_context,0,t->x,t->y);
        touch_owned=touch_blocked=0;
        return taken?AZ_TOUCH_TAKE:AZ_TOUCH_PASS;
    }
    if(touch_blocked)return AZ_TOUCH_TAKE;
    uint64_t b=__atomic_load_n(&touch_bounds,__ATOMIC_ACQUIRE);
    unsigned context=(unsigned)(b>>44);
    int visible=C->view&&C->view()==AZ_VIEW_DECK;
    if(touch_owned){
        if(!b||context!=touch_owner_context||!visible){
            publish_touch(touch_owner_context,0,t->x,t->y);touch_owned=0;touch_blocked=1;return AZ_TOUCH_TAKE;
        }
    }else{
        if(!visible)return AZ_TOUCH_PASS;
        if(touch_in(__atomic_load_n(&touch_info_hole,__ATOMIC_ACQUIRE),t->x,t->y)){touch_blocked=1;return AZ_TOUCH_PASS;}
        uint64_t labels=__atomic_load_n(&stem_label_bounds,__ATOMIC_ACQUIRE);
        if(touch_in(labels,t->x,t->y)){touch_blocked=1;return AZ_TOUCH_TAKE;}
        if(!touch_in(b,t->x,t->y)){touch_blocked=1;return AZ_TOUCH_PASS;}
        touch_owned=1;touch_owner_context=context;touch_owner_bounds=b;
    }
    int x=(int)(touch_owner_bounds&2047),y=(int)((touch_owner_bounds>>11)&2047);
    int w=(int)((touch_owner_bounds>>22)&2047),h=(int)((touch_owner_bounds>>33)&2047);
    int px=t->x,py=t->y;if(px<x)px=x;if(px>=x+w)px=x+w-1;if(py<y)py=y;if(py>=y+h)py=y+h-1;
    publish_touch(touch_owner_context,1,px,py);
    return AZ_TOUCH_TAKE;
}
#ifndef GEOMETRY_TEST
#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/syscall.h>
/* Independent read-only evdev queue, on an SDK worker. Raw syscalls keep
 * core's interposed open/read bookkeeping attached to EP147's own descriptor.
 * No grab, no injected input, no touch-filter registration. */
static void *pointer_worker(void *u){
    (void)u;
    int fd=(int)syscall(SYS_openat,AT_FDCWD,"/dev/input/event0",O_RDONLY|O_NONBLOCK|O_CLOEXEC,0);
    char name[80]={0};struct input_absinfo ax,ay;
    if(fd<0||syscall(SYS_ioctl,fd,EVIOCGNAME(sizeof name),name)<0||strncmp(name,"goodix-ts",9)||
       syscall(SYS_ioctl,fd,EVIOCGABS(ABS_MT_POSITION_X),&ax)<0||
       syscall(SYS_ioctl,fd,EVIOCGABS(ABS_MT_POSITION_Y),&ay)<0||
       ax.minimum!=0||ay.minimum!=0||ax.maximum<1279||ax.maximum>1280||ay.maximum<799||ay.maximum>800){
        if(fd>=0)syscall(SYS_close,fd);__atomic_store_n(&pointer_poll_on,-1,__ATOMIC_RELEASE);return NULL;
    }
    az_touch t={.slot=0};int slot=0,have_mt=0,lost=0;unsigned cycle=0;
    struct input_absinfo current_slot;
    if(syscall(SYS_ioctl,fd,EVIOCGABS(ABS_MT_SLOT),&current_slot)>=0)slot=current_slot.value;
    struct pollfd poller={fd,POLLIN,0};
    for(;;){
        int ready=poll(&poller,1,20);
        if(ready<0&&errno==EINTR)continue;
        if(ready<0||poller.revents&(POLLERR|POLLHUP|POLLNVAL))break;
        if(ready>0){
            for(int batch=0;batch<16;batch++){
                struct input_event ev[64];long n=syscall(SYS_read,fd,ev,sizeof ev);
                if(n<=0)break;
                for(long i=0;i<n/(long)sizeof *ev;i++){
                    struct input_event *e=&ev[i];
                    if(e->type==EV_SYN&&e->code==SYN_DROPPED){lost=1;t.down=0;audio_touch(&t,NULL);}
                    if(e->type==EV_ABS){
                        if(e->code==ABS_MT_SLOT)slot=e->value;
                        else if(slot==0&&e->code==ABS_MT_POSITION_X)t.x=e->value;
                        else if(slot==0&&e->code==ABS_MT_POSITION_Y)t.y=e->value;
                        else if(slot==0&&e->code==ABS_MT_TRACKING_ID){have_mt=1;t.down=e->value>=0;}
                    }else if(e->type==EV_KEY&&e->code==BTN_TOUCH&&(!have_mt||!e->value))t.down=e->value!=0;
                    else if(e->type==EV_SYN&&e->code==SYN_REPORT){
                        if(!lost)audio_touch(&t,NULL);
                    }
                }
            }
        }
        /* Resync releases, including a dropped packet. Lost contact never
         * re-arms while still held: wait for a confirmed primary-finger lift. */
        if(lost||!(++cycle%5)){
            int ids[2]={ABS_MT_TRACKING_ID,-1};
            if(syscall(SYS_ioctl,fd,EVIOCGMTSLOTS(sizeof ids),ids)>=0){
                if(ids[1]<0){t.down=0;audio_touch(&t,NULL);lost=0;}
            }
        }
        __atomic_store_n(&pointer_heartbeat,(uint64_t)now_ms(),__ATOMIC_RELEASE);
        __atomic_store_n(&pointer_poll_on,1,__ATOMIC_RELEASE);
    }
    t.down=0;audio_touch(&t,NULL);syscall(SYS_close,fd);
    __atomic_store_n(&pointer_poll_on,-1,__ATOMIC_RELEASE);return NULL;
}
#endif
#include "stem_glass_input.h"
static void audio_stop(void) {
    trail_reset();
    if(audio_previous>=0||__atomic_load_n(&touch_bounds,__ATOMIC_RELAXED)){touch_context=(touch_context+1)&0xfffffu;if(!touch_context)touch_context=1;}
    __atomic_store_n(&touch_bounds,0,__ATOMIC_RELEASE);
    if(audio_previous>=0){
        visual[audio_previous].point=0;
        if(control_last_kind==2){
            if(sampler_fx_send)sampler_fx_send(audio_previous,0,0,0,2,0);
            if(sampler_xy_send)sampler_xy_send(audio_previous,0,0,1000,-1);
            if(sampler_quantize)sampler_quantize(audio_previous,0);
        }else if(stem_fx_send)stem_fx_send(audio_previous,0,0,0,2,0);
    }
    if(audio_previous>=0&&stem_fx_quantize)stem_fx_quantize(audio_previous,0);
    audio_previous=-1;touching=audio_armed=audio_live=0;
    last_touch_serial=(unsigned)(__atomic_load_n(&touch_sample,__ATOMIC_ACQUIRE)>>32);
}
static void visual_kind(int deck,int next){
    if(deck<0||deck>=4||next<1||next>2||kind[deck]==next)return;
    /* Cancel the old contact before restoring choices. A held gesture must
     * never transfer across panel types, including a latched send. */
    if(audio_previous==deck){visual[deck].latch=0;audio_stop();}
    if(kind[deck]>=1&&kind[deck]<=2){
        remembered_visual[deck][kind[deck]-1]=visual[deck];
        remembered_visual[deck][kind[deck]-1].latch=0;
        remembered_visual[deck][kind[deck]-1].point=0;
    }
    visual[deck]=remembered_visual[deck][next-1];
    visual[deck].latch=visual[deck].point=0;
    kind[deck]=next;
}
static void audio_tick(void) {
    if(!tracking_ready()){if(audio_previous>=0)visual[audio_previous].latch=0;audio_stop();return;}
    struct visual_state *v=&visual[selected];
    int want_audio=audio_ready();
    if(kind[selected]==2&&sampler_state){int state=sampler_state(selected);if(state>=0){v->sound=state&7;}}
    if(audio_previous!=selected||audio_last_effect!=v->effect||control_last_kind!=kind[selected]||audio_live!=want_audio||memcmp(&audio_last_area,&panel_area,sizeof panel_area)) {
        if(audio_previous>=0)visual[audio_previous].latch=0;
        audio_stop();audio_previous=selected;audio_last_effect=v->effect;audio_last_area=panel_area;control_last_kind=kind[selected];audio_live=want_audio;
    }
    az_rect f=field_rect(panel_area);
    uint64_t bounds=pack_touch_rect(f)|((uint64_t)touch_context<<44);
    __atomic_store_n(&touch_bounds,bounds,__ATOMIC_RELEASE);
    uint64_t t=__atomic_load_n(&touch_sample,__ATOMIC_ACQUIRE);unsigned serial=(unsigned)(t>>32);
    if(serial!=last_touch_serial&&(unsigned)(t>>44)==touch_context){
        last_touch_serial=serial;touching=(t&(1u<<22))!=0;
        if(touching){
            int x=(int)(t&2047)-f.x,y=(int)((t>>11)&2047)-f.y;
            if(x<0)x=0;if(x>=f.w)x=f.w-1;if(y<0)y=0;if(y>=f.h)y=f.h-1;
            v->x=x*1000/(f.w-1);v->y=y*1000/(f.h-1);v->rate=rate_for_column(selected,x*rate_columns(selected)/f.w);v->point=1;audio_armed=1;
        }
        C->surface_redraw(panel_surface);
    }
    static const int modes[6]={2,0,3,4,5,7};
    if(!touching&&!v->latch){audio_armed=0;v->point=0;}
    int send=audio_armed&&(touching||v->latch);
    if(!audio_live)return; /* sampler and audio-off views still track visually */
    if(kind[selected]==2){
        /* OFF, ECHO, LFO ECHO, PITCH, PITCH ECHO. Same existing DSP exports. */
        static const int sampler_modes[5]={6,2,3,6,2};
        int effect=v->effect>=0&&v->effect<5?v->effect:0;
        if(sampler_quantize)sampler_quantize(selected,C->row_value(grid_row)!=0);
        sampler_fx_send(selected,1,send,v->rate,sampler_modes[effect],1000-v->y);
        sampler_xy_send(selected,send,v->rate,1000,(effect==3||effect==4)?1000-v->y:-1);
        return;
    }
    if(stem_fx_quantize)stem_fx_quantize(selected,C->row_value(grid_row)!=0);
    int mid=__atomic_load_n(&mic2_mid,__ATOMIC_RELAXED);
    if(stem_fx_volume&&mid>=0)stem_fx_volume(mid);
    stem_fx_send(selected,1,send,v->rate,modes[v->effect],1000-v->y);
}
static void box(az_draw *d,az_rect r,const char *label,int on,uint32_t tint,int muted) {
    C->fill_rect(d,r.x,r.y,r.w,r.h,on?0x16354a:0x10141b);
    C->draw_rect(d,r.x,r.y,r.w,r.h,on?2:1,on?tint:0x485460);
    int px=r.h*11/20;if(px>17)px=17;if(px<8)px=8;
    int tw=C->text_width?C->text_width(label,px):0;
    int tx=r.x+(r.w-tw)/2;if(tx<r.x+2)tx=r.x+2;
    C->draw_text(d,tx,r.y+(r.h-px*3/2)/2,label,px,muted?0x7e858c:on?tint:0xffffff);
}
static void paint_button(az_draw *draw,az_rect a,void *u) {
    (void)u;char label[32];uint32_t ink=opened?0x000000:0x009dff;
    C->fill_rect(draw,a.x,a.y,a.w,a.h,opened?0xffffff:0x232323);
    C->draw_rect(draw,a.x,a.y,a.w,a.h,1,0x009dff);
    int small=C->row_value(launcher_row)==1;
    if(small)snprintf(label,sizeof label,"TOUCH FX");
    else snprintf(label,sizeof label,selected>=0?"TOUCH FX  %d":"TOUCH FX",selected+1);
    int px=small?14:18,tw;
    for(;;){tw=C->text_width?C->text_width(label,px):(int)strlen(label)*px/2;if(px<=8||tw<=a.w-6)break;--px;}
    C->draw_text(draw,a.x+(a.w-tw)/2,a.y+(a.h-px*3/2)/2,label,px,ink);
}
/* draw_image preserves functional colours; fill_rect is theme-mapped by core. */
static unsigned char sampler_tile[64*64*4];
static void sampler_background(az_draw *d,int x,int y,int w,int h) {
    for(int yy=0;yy<h;yy+=64)for(int xx=0;xx<w;xx+=64) {
        int tw=w-xx<64?w-xx:64,th=h-yy<64?h-yy:64;
        C->draw_image(d,x+xx,y+yy,sampler_tile,tw,th);
    }
}
static void pointer_tick(long now){
    int visible=active&&opened&&geometry_ready&&selected>=0&&visual[selected].point&&(touching||visual[selected].latch);
    if(glass_head){
        az_rect head={0};int solid=visible&&panel_layout(kind[selected]==2)==2;
        if(solid){az_rect f=field_rect(panel_area);head=(az_rect){f.x+visual[selected].x*(f.w-1)/1000-19,f.y+visual[selected].y*(f.h-1)/1000-19,39,39};}
        glass_head(ID,panel_surface,solid,head);
    }
    if(!visible){trail_reset();return;}
    int had_trail=trail_count;
    if(C->row_value(pointer_style_row)==2)trail_reset();
    else trail_update(now,visual[selected].x,visual[selected].y,touching);
    int animated_trail=C->row_value(pointer_style_row)!=2&&(had_trail||trail_count);
    int interval=animated_trail?33:80; /* slow colour cycle needs less repainting */
    if((animated_trail||C->row_value(pointer_colour_row)==4)&&now-trail_redraw>=interval){trail_redraw=now;C->surface_redraw(panel_surface);}
}
/* Functional colour bypasses theme mapping, as the round head and trail do. */
static void pointer_line(az_draw *draw,int x,int y,int length,int vertical,uint32_t ink){
    unsigned char pixels[64*4];for(int i=0;i<64;i++){pixels[i*4]=(unsigned char)(ink>>16);pixels[i*4+1]=(unsigned char)(ink>>8);pixels[i*4+2]=(unsigned char)ink;pixels[i*4+3]=255;}
    for(int at=0;at<length;at+=64){int n=length-at<64?length-at:64;C->draw_image(draw,x+(vertical?0:at),y+(vertical?at:0),pixels,vertical?1:n,vertical?n:1);}
}
static void pointer_paint(az_draw *draw,az_rect field,struct visual_state *v){
    double phase=-1,bar=-1;
    if(AZ_HAS(C,deck_grid)&&C->deck_grid&&C->deck_grid(selected,&phase,&bar))phase=-1;
    int pulse=trail_pulse(now_ms(),decks[selected].bpm,phase,decks[selected].pos_s,kind[selected]==2,v->rate);
    int style=C->row_value(pointer_style_row);uint32_t ink=pointer_colour();
    if(style!=1){
        int x=field.x+v->x*(field.w-1)/1000,y=field.y+v->y*(field.h-1)/1000;
        pointer_line(draw,x,field.y,field.h,1,ink);
        if(panel_layout(kind[selected]==2)==2){if(y==field.y+field.h-1)y--;pointer_line(draw,field.x,y+1,field.w,0,ink);}
        pointer_line(draw,field.x,y,field.w,0,ink);
    }
    trail_paint(draw,field,v->x,v->y,now_ms(),pulse,style!=2,ink,C->row_value(trail_size_row)==1);
}
#include "axis_icons.h"
static void paint_panel(az_draw *draw,az_rect a,void *u) {
    (void)u;C->fill_rect(draw,a.x,a.y,a.w,a.h,0x10141b);
    int drum=selected>=0&&kind[selected]==2;
    struct visual_state empty={0};struct visual_state *v=selected>=0?&visual[selected]:&empty;
    const char *effects[6]={"ECHO","LFO","LFO ECHO","GATE","ROLL","ECHO ROLL"};
    const char *sampler_effects[5]={v->effect?"ON":"OFF","ECHO","LFO ECHO","PITCH","PITCH ECHO"};
    for(int k=0;k<(drum?5:6);k++) box(draw,effect_rect(a,k),drum?sampler_effects[k]:effects[k],selected>=0&&v->effect==k,0x49d5ff,selected<0||(drum&&v->effect!=k));
    if(drum&&C->row_value(bank_buttons_row)){int bank=sampler_bank?sampler_bank(-1):0;
        for(int k=0;k<4;k++){char label[2]={(char)('1'+k),0};box(draw,bank_rect(a,k),label,bank==k+1,0xc8e82b,bank==0);}
    }
    box(draw,local_rect(a,790,2,120,36),"LATCH",selected>=0&&v->latch,0x70df50,0);
    for(int d=0;d<4;d++) {char label[4];snprintf(label,sizeof label,"%d",d+1);box(draw,channel_rect(a,d),label,selected==d,0x49d5ff,decks[d].loaded!=1);}
    box(draw,local_rect(a,1126,2,48,36),"MST",0,0x49d5ff,1); /* no master routing in this display prototype */
    const char *stem[4]={"VOCALS","INSTRUMENT","BASS","DRUMS"};
    const char *sounds[4]={"KICK","SNARE","CLAP","HIHAT"};
    int levels[4];for(int k=0;k<4;k++)levels[k]=stem_level(selected,k);
    for(int k=0;k<4;k++) {
        if(drum)box(draw,sound_rect(a,k),sounds[k],selected>=0&&v->sound==k,0xc8e82b,0);
        else {
            az_rect r=sound_rect(a,k);uint32_t ink=stem_visual_ink(stem_colours[k],levels[k]>0,levels[k]);
            C->fill_rect(draw,r.x,r.y,r.w,r.h,0x232323);
            C->draw_rect(draw,r.x,r.y,r.w,r.h,1,ink);
            int px=r.h*11/20;if(px>17)px=17;if(px<8)px=8;
            int tw=C->text_width?C->text_width(stem[k],px):0;
            C->draw_text(draw,r.x+(r.w-tw)/2,r.y+(r.h-px*3/2)/2,stem[k],px,ink);
        }
    }
    az_rect f=field_rect(a);
    const char *stem_rates[8]={"4","2","1","3/4","1/2","1/4","1/8","1/16"};
    const char *drum_rates[6]={"1","1/2","1/4","1/8","1/16","1/32"};
    int columns=drum?6:8;
    for(int k=0;k<columns;k++) {
        int x=f.x+k*f.w/columns,w=(k+1)*f.w/columns-k*f.w/columns;
        if(panel_layout(drum)==1)C->fill_rect(draw,x,f.y,w-2,f.h,0x191919);
        else if(drum)sampler_background(draw,x,f.y,w-2,f.h);
        else for(int px=0;px<w-2;){
            uint32_t colour=stem_visual_field(x+px-f.x,f.w,levels,0);
            int next=px+1;
            while(next<w-2&&stem_visual_field(x+next-f.x,f.w,levels,0)==colour)next++;
            C->fill_rect(draw,x+px,f.y,next-px,f.h,colour);px=next;
        }
        box(draw,rate_rect(a,drum,k),drum?drum_rates[k]:stem_rates[k],selected>=0&&v->rate==rate_for_column(selected,k),drum?0xc8e82b:0x49d5ff,0);
    }
    for(int y=f.y+10;y<f.y+f.h;y+=20)for(int x=f.x+10;x<f.x+f.w;x+=20)C->fill_rect(draw,x,y,2,2,0x526570);
    if(selected>=0&&v->point&&(touching||v->latch)) {
        pointer_paint(draw,f,v);
    }
    axis_paint(draw,f);
    if(selected<0) C->draw_text(draw,f.x+12,f.y+45,"Choose a loaded channel: 1 - 4",18,0xffffff);
}
static int tap_button(const az_event *e,void *u) {
    (void)u;if(!active||e->kind!=AZ_EV_TAP)return 0;
    opened=!opened;C->surface_show(panel_surface,opened&&geometry_ready);return 1;
}
static int tap_panel(const az_event *e,void *u) {
    (void)u;if(!active||!opened||!geometry_ready||e->kind!=AZ_EV_TAP)return 0;
    for(int d=0;d<4;d++)if(inside(channel_rect(panel_area,d),e->x,e->y)) {
        if(decks[d].loaded==1) {selected=d;pending=-1;C->cfg_set(ID,"deck",d);C->surface_redraw(panel_surface);}return 1;
    }
    if(selected<0)return 1;
    if(kind[selected]==2&&C->row_value(bank_buttons_row)&&sampler_bank)for(int k=0;k<4;k++)if(inside(bank_rect(panel_area,k),e->x,e->y)){
        sampler_bank(k+1);C->surface_redraw(panel_surface);return 1;
    }
    struct visual_state *v=&visual[selected];
    for(int k=0;k<(kind[selected]==2?5:6);k++)if(inside(effect_rect(panel_area,k),e->x,e->y))v->effect=k;
    if(inside(local_rect(panel_area,790,2,120,36),e->x,e->y))v->latch=!v->latch;
    if(kind[selected]==2)for(int k=0;k<4;k++)if(inside(sound_rect(panel_area,k),e->x,e->y)){
        if(sampler_ui_pick&&sampler_state){
            int state=sampler_state(selected);
            if(state>=0&&(state&0x100)&&sampler_ui_pick(selected,k))v->sound=k;
        }
    }
    for(int k=0;k<rate_columns(selected);k++)if(inside(rate_rect(panel_area,kind[selected]==2,k),e->x,e->y))v->rate=rate_for_column(selected,k);
    az_rect f=field_rect(panel_area);
    if(inside(f,e->x,e->y)) {v->x=(e->x-f.x)*1000/(f.w-1);v->y=(e->y-f.y)*1000/(f.h-1);v->point=1;v->rate=rate_for_column(selected,(e->x-f.x)*rate_columns(selected)/f.w);}
    C->surface_redraw(panel_surface);return 1;
}
static const az_surface_v1 BUTTON={.size=sizeof(az_surface_v1),.layer=AZ_LAYER_OVER_EP147,.views=AZ_VIEW_BIT(AZ_VIEW_DECK),.on_paint=paint_button,.on_input=tap_button};
static const az_surface_v1 PANEL={.size=sizeof(az_surface_v1),.layer=AZ_LAYER_TOP,.views=AZ_VIEW_BIT(AZ_VIEW_DECK),.on_paint=paint_panel,.on_input=tap_panel};
#ifndef GEOMETRY_TEST
int touchfx_native_init(const az_core_v1 *,int,int,int);
void touchfx_native_tick(void);
#endif
static void tick(void) {
#ifndef GEOMETRY_TEST
    touchfx_native_tick();
#endif
    launcher_place();
    int enabled=C->row_value(enable_row)!=0;
    if(enabled!=active) {
        active=enabled;C->surface_show(button_surface,active);C->surface_show(panel_surface,active&&opened&&geometry_ready);
        pending=-1;was_follow=0;
    }
    __atomic_store_n(&geometry_requested,active,__ATOMIC_RELAXED);
    __atomic_store_n(&stem_mask_request,active&&opened&&selected>=0&&selected<4&&kind[selected]==1?(unsigned)selected+1:0,__ATOMIC_RELAXED);
    if(!active){__atomic_store_n(&stem_label_bounds,0,__ATOMIC_RELEASE);audio_stop();glass_tick();return;}
    long now=now_ms();
    for(int d=0;d<4;d++) {
        az_deck_info info;memset(&info,0,sizeof info);info.size=sizeof info;info.pad_mode=-1;
        if(C->deck_info(d,&info)) {memset(&decks[d],0,sizeof decks[d]);continue;}
        info.title[sizeof(info.title)-1]=0;
        if(d==audio_previous&&strcmp(info.title,decks[d].title)){visual[d].latch=0;audio_stop();}
        decks[d]=info;
        if(info.pad_mode==2)visual_kind(d,2);
        else if(info.pad_mode==1 && info.has_stems==1)visual_kind(d,1);
    }
    int count=C->deck_count?C->deck_count():2;
    if(count!=2&&count!=4)count=2;
    uint64_t measured=__atomic_load_n(&geometry_result,__ATOMIC_ACQUIRE);
    int mode=(int)(measured>>48)-1,left=(int)((measured>>24)&0xffffff),bottom=(int)(measured&0xffffff);
    int ready=measured && ((mode==2?4:2)==count) && mode>=0 && mode<=2 && left>=24 && left<=512 && bottom>=500 && bottom<=620;
    if(ready){az_rect next={left,54,1280-left,bottom-54};
        if(memcmp(&next,&panel_area,sizeof next)){panel_area=next;C->surface_place(panel_surface,panel_area);}
    }
    if(ready!=geometry_ready){geometry_ready=ready;C->surface_show(panel_surface,opened&&ready);}
    follow(now);
    uint64_t labels=opened&&geometry_ready&&selected>=0&&kind[selected]==1&&C->view&&C->view()==AZ_VIEW_DECK?
        pack_touch_rect(local_rect(panel_area,0,40,1232,40)):0;
    __atomic_store_n(&stem_label_bounds,labels,__ATOMIC_RELEASE);
    audio_tick();
    glass_tick();
    pointer_tick(now);
    if(now-redraw_at>=250) {
        redraw_at=now;
        if(button_deck!=selected) {button_deck=selected;C->surface_redraw(button_surface);}
        if(opened) C->surface_redraw(panel_surface);
    }
}
static int row(const char *key,const char *label,int type,unsigned flags,const char *help,const char *const *choices,int n) {
    az_row_v1 r;memset(&r,0,sizeof r);r.size=sizeof r;r.key=key;r.label=label;r.kind=type;r.flags=flags;if(!strcmp(key,"sampler-audio")||!strcmp(key,"stems-audio")||!strcmp(key,"follow"))r.flags|=AZ_ROW_DEFAULT_ON;r.parent=strcmp(key,"enabled")?"enabled":NULL;r.help=help;r.choices=choices;r.n_choices=n;if(!strcmp(key,"stem-layout")||!strcmp(key,"sampler-layout"))r.default_choice=2;if(!strcmp(key,"pointer-style"))r.default_choice=1;if(!strcmp(key,"assign-1")||!strcmp(key,"assign-3"))r.default_choice=1;if(!strcmp(key,"assign-2")||!strcmp(key,"assign-4"))r.default_choice=3;
    return C->register_row(ID,&r);
}
static int init(const az_core_v1 *core) {
    if(!core || core->abi!=AZ_MOD_ABI || !AZ_HAS(core,surface_create) || !AZ_HAS(core,deck_info) || !AZ_HAS(core,frame_filter) || !AZ_HAS(core,register_row) || !AZ_HAS(core,surface_redraw)) return 1;
    C=core;
    if(!AZ_HAS(C,draw_image)||!C->draw_image)return 7;
    for(unsigned i=0;i<sizeof sampler_tile;i+=4){sampler_tile[i]=0x28;sampler_tile[i+1]=0x2c;sampler_tile[i+2]=0x21;sampler_tile[i+3]=255;}
#ifndef GEOMETRY_TEST
    if(!AZ_HAS(C,thread_start)||!C->thread_start||!C->fw_tag||strcmp(C->fw_tag(),"ep147-9e9454e357389ece"))return 5;
#endif
    enable_row=row("enabled","Touch FX",AZ_ROW_TOGGLE,AZ_ROW_DEFAULT_ON,"Show the Touch FX panel. STEMS and sampler audio have separate switches.",NULL,0);
    if(enable_row<0)return 2;
    static const char *const launchers[]={"Large top right","Small by A.CUE"};
    launcher_row=row("launcher","Touch FX button",AZ_ROW_CHOICE,0,"Choose one launcher: current large button or original small button beside A.CUE.",launchers,2);
    if(launcher_row<0)return 2;
    static const char *const pointer_styles[]={"Cross + tail","Tail","Cross"};
    pointer_style_row=row("pointer-style","Touch pointer style",AZ_ROW_CHOICE,0,"Round dot in all modes. Tail is the default; Cross hides the trail.",pointer_styles,3);
    static const char *const pointer_colours[]={"Blue","White","Red","Green","Rainbow"};
    pointer_colour_row=row("pointer-colour","Touch pointer colour",AZ_ROW_CHOICE,0,"Colour of dot, trail and cross. Rainbow slowly cycles them together; independent of theme.",pointer_colours,5);
    static const char *const trail_sizes[]={"Slim","Full"};
    trail_size_row=row("trail-width","Touch trail size",AZ_ROW_CHOICE,0,"Slim is the default. Full uses large fading dots. Changes immediately.",trail_sizes,2);
    if(pointer_style_row<0||pointer_colour_row<0||trail_size_row<0)return 2;
    static const char *const layouts[]={"Colour","No colour","Transparent"};
    stem_layout_row=row("stem-layout","STEMS panel layout",AZ_ROW_CHOICE,0,"Transparent reveals the native waveforms. EQ and pad dimming remain active.",layouts,3);
    sampler_layout_row=row("sampler-layout","Sampler panel layout",AZ_ROW_CHOICE,0,"Same visual choices as STEMS. Sampler audio uses its separate switch.",layouts,3);
    if(stem_layout_row<0||sampler_layout_row<0)return 2;
    native_touch_row=row("native-touch","Native STEMS touch",AZ_ROW_TOGGLE,AZ_ROW_DEFAULT_ON,"For easier control from the Touch FX menu.",NULL,0);
    if(native_touch_row<0)return 2;
    bank_buttons_row=row("sampler-bank-buttons","Sampler bank buttons",AZ_ROW_TOGGLE,AZ_ROW_DEFAULT_ON,"Show bank1-4 shortcuts in the sampler Touch FX panel. Off hides the buttons; the selected bank stays unchanged.",NULL,0);
    if(bank_buttons_row<0)return 2;
    sampler_audio_row=row("sampler-audio","Sampler audio",AZ_ROW_TOGGLE,0,"Select a sound in SLIP LOOP to arm Drum Roll, then hold the field. Existing MIC volume and pitch controls remain.",NULL,0);
    if(sampler_audio_row<0)return 2;
    grid_row=row("stems-grid","FX grid sync",AZ_ROW_TOGGLE,AZ_ROW_DEFAULT_ON,"LFO/GATE follow the deck grid. ROLL waits for the next selected division. Missing grid uses free timing. Echo interval stays unchanged.",NULL,0);
    audio_row=row("stems-audio","STEMS audio",AZ_ROW_TOGGLE,0,"Hold or drag the field. LATCH holds the effect until stopped. Sampler audio has a separate switch.",NULL,0);

    dim_row=row("dim-indicators","Dim STEMS colours with EQ",AZ_ROW_TOGGLE,AZ_ROW_DEFAULT_ON,"Follow the selected deck STEMS gain values. Off keeps full colours. Muted STEMS parts also dim the panel labels and field.",NULL,0);
    follow_row=row("follow","Follow mixer",AZ_ROW_TOGGLE,0,"Set all four assignments below to match Utility. Uses faders, not audio detection.",NULL,0);
    static const char *const choices[]={"Unknown","A","THRU","B"};
    static const char *const keys[]={"assign-1","assign-2","assign-3","assign-4"};
    static const char *const labels[]={"CH 1 crossfader","CH 2 crossfader","CH 3 crossfader","CH 4 crossfader"};
    for(int d=0;d<4;d++) assign_row[d]=row(keys[d],labels[d],AZ_ROW_CHOICE,0,"Match the player's Utility crossfader assignment.",choices,4);
    if(grid_row<0 || audio_row<0 || enable_row<0 || dim_row<0 || follow_row<0) return 2;
    for(int d=0;d<4;d++) if(assign_row[d]<0) return 2;
    for(int d=0;d<4;d++){visual[d].rate=2;visual[d].x=visual[d].y=500;for(int mode=0;mode<2;mode++)remembered_visual[d][mode]=visual[d];}
    int saved=C->cfg_get(ID,"deck",-1);selected=saved>=0&&saved<4?saved:-1;
    button_surface=C->surface_create(ID,&BUTTON);panel_surface=C->surface_create(ID,&PANEL);
    if(!button_surface || !panel_surface) return 3;
    launcher_place();
    C->surface_place(panel_surface,panel_area);
    if(C->frame_filter(ID,AZ_FILTER_RAW|(1u<<AZ_BUS_MIXER_IN),frame,NULL)) return 4;
#ifndef GEOMETRY_TEST
    int (*rates_cap)(void)=NULL;*(void **)(&rates_cap)=dlsym(RTLD_DEFAULT,"mrdjjag_stem_fx_rates");
    if(!rates_cap||rates_cap()!=8)return 14;
    int (*four_beats)(void)=NULL;*(void **)(&four_beats)=dlsym(RTLD_DEFAULT,"mrdjjag_stem_fx_four_beats");
    if(!four_beats||four_beats()!=1)return 15;
    *(void **)(&stem_fx_abi)=dlsym(RTLD_DEFAULT,"mrdjjag_stem_fx_abi");
    if(stem_fx_abi&&stem_fx_abi()==3){
        *(void **)(&stem_fx_quantize)=dlsym(RTLD_DEFAULT,"mrdjjag_stem_fx_quantize");
        *(void **)(&stem_fx_volume)=dlsym(RTLD_DEFAULT,"azstem_stem_fx_volume");
        if(stem_fx_quantize&&stem_fx_volume)*(void **)(&stem_fx_send)=dlsym(RTLD_DEFAULT,"azstem_stem_touch_fx");
    }
    *(void **)(&sampler_ui_pick)=dlsym(RTLD_DEFAULT,"mrdjjag_sampler_ui_select");
    if(!sampler_ui_pick)return 11;
    *(void **)(&sampler_bank)=dlsym(RTLD_DEFAULT,"mrdjjag_sampler_ui_bank");
    if(!sampler_bank)return 12;
    *(void **)(&sampler_abi)=dlsym(RTLD_DEFAULT,"mrdjjag_sampler_fx_abi");
    if(sampler_abi&&sampler_abi()==1){
        *(void **)(&sampler_state)=dlsym(RTLD_DEFAULT,"mrdjjag_sampler_state");
        *(void **)(&sampler_fx_send)=dlsym(RTLD_DEFAULT,"azstem_drum_touch_fx");
        *(void **)(&sampler_xy_send)=dlsym(RTLD_DEFAULT,"azstem_drum_xy_pitch");
        *(void **)(&sampler_select)=dlsym(RTLD_DEFAULT,"azstem_drum_select");
        *(void **)(&sampler_quantize)=dlsym(RTLD_DEFAULT,"mrdjjag_sampler_quantize");
    }
    *(void **)(&stem_gain_read)=dlsym(RTLD_DEFAULT,"azstem_stem_gain");
    *(void **)(&stem_mask_snapshot)=dlsym(RTLD_DEFAULT,"mrdjjag_stem_ui_snapshot");
    if(C->thread_start(ID,"geometry",geometry_worker,NULL))return 6;
    if(C->thread_start(ID,"pointer",pointer_worker,NULL))return 8;
    *(void **)(&glass_marks)=dlsym(RTLD_DEFAULT,"mrdjjag_surface_glass_marks");
    *(void **)(&info_passthrough)=dlsym(RTLD_DEFAULT,"mrdjjag_surface_info_passthrough");
    if(!glass_marks||!info_passthrough)return 16;
    *(void **)(&glass_surface)=dlsym(RTLD_DEFAULT,"mrdjjag_surface_glass");
    *(void **)(&glass_head)=dlsym(RTLD_DEFAULT,"mrdjjag_surface_glass_head");
    if(!glass_surface||!glass_head)return 9;
    *(void **)(&glass_guard_bind)=dlsym(RTLD_DEFAULT,"mrdjjag_surface_guard_bind");
#endif
    if(!glass_guard_bind||glass_guard_bind(ID,panel_surface))return 10;
#ifndef GEOMETRY_TEST
    if(touchfx_native_init(C,enable_row,dim_row,native_touch_row))return 13;
#endif
    return 0;
}
static const char *const FW[]={"ep147-9e9454e357389ece",NULL};
static const az_mod_v1 MOD={.abi=AZ_MOD_ABI,.size=sizeof(az_mod_v1),.id=ID,.fw_tags=FW,.init=init,.tick=tick};
const az_mod_v1 *az_mod_v1_entry(void) {return &MOD;}

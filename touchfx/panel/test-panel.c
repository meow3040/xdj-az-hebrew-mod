/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef assert
#define assert(x) do {if(!(x)){fprintf(stderr,"CHECK failed at line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#include <string.h>
#include <time.h>
static long test_time=1000;
static int test_clock(int unused,struct timespec *ts) {(void)unused;ts->tv_sec=test_time/1000;ts->tv_nsec=(test_time%1000)*1000000;return 0;}
#define CLOCK_MONOTONIC 1
#define clock_gettime test_clock
#define GEOMETRY_TEST
#include "panel-test.c"
#undef clock_gettime
static int bank_value=1,bank_calls;
static int fake_bank(int bank){if(bank!=-1){assert(bank>=1&&bank<=4);bank_value=bank;bank_calls++;}return bank_value;}
static int values[18],nrows,surfaces,shown[2],paints,dimmed,deck_count_value=2;
static int count(void){return deck_count_value;}
static az_deck_info fixtures[4];
static int cfg_get(const char *id,const char *key,int def) {(void)id;(void)key;return def;}
static void cfg_set(const char *id,const char *key,int value) {(void)id;(void)key;(void)value;}
static int regrow(const char *id,const az_row_v1 *r) {(void)id;assert(nrows<18);assert(!nrows?!strcmp(r->key,"enabled"):r->parent&&!strcmp(r->parent,"enabled"));int n=nrows++;values[n]=(r->flags&AZ_ROW_DEFAULT_ON)?1:r->default_choice;return n;}
static int val(int h) {assert(h>=0&&h<nrows);return values[h];}
static int info(int d,az_deck_info *o) {assert(d>=0&&d<4);*o=fixtures[d];return 0;}
static az_surface *create(const char *id,const az_surface_v1 *s) {(void)id;assert(s->on_paint);return (az_surface*)(uintptr_t)(++surfaces);}
static az_rect placed[2];
static void place(az_surface *s,az_rect a) {assert(a.w>0&&a.h>0);placed[(uintptr_t)s-1]=a;}
static void show(az_surface *s,int on) {shown[(uintptr_t)s-1]=on;}
static void redraw(az_surface *s) {(void)s;paints++;}
static int regfilter(const char *id,unsigned mask,az_frame_fn f,void *u) {(void)id;(void)u;assert(mask&(1u<<AZ_BUS_MIXER_IN));assert(f==frame);return 0;}
static void fill(az_draw *d,int x,int y,int w,int h,uint32_t c) {(void)d;(void)x;(void)y;(void)w;(void)h;(void)c;}
static int off_count,on_count,on_muted,echo_active;
static void text(az_draw *d,int x,int y,const char *s,int px,uint32_t rgb) {(void)d;(void)x;(void)y;(void)px;assert(!strstr(s,"TEST"));if(!strcmp(s,"OFF"))off_count++;if(!strcmp(s,"ON")){on_count++;if(rgb==0x7e858c)on_muted++;}if(!strcmp(s,"ECHO")&&rgb==0x49d5ff)echo_active++;if((!strcmp(s,"VOCALS")||!strcmp(s,"INSTRUMENT"))&&rgb==0x404040)dimmed++;}
static void border(az_draw *d,int x,int y,int w,int h,int t,uint32_t c){(void)d;(void)x;(void)y;(void)w;(void)h;(void)t;(void)c;}
static int image_calls,pointer_lines,pointer_heads,pointer_tails,axis_calls;
static int image(az_draw *d,int x,int y,const unsigned char *rgba,int w,int h) {
 (void)d;(void)x;(void)y;assert(w>0&&w<=64&&h>0&&h<=64);
 for(int k=0;k<4;k++)if(rgba==axis_icons[k]){assert(w==20&&h==20);axis_calls++;return 0;}
 int background=rgba[0]==0x28&&rgba[1]==0x2c&&rgba[2]==0x21;
 if(!background){if((w==1||h==1)&&rgba[3]==255)pointer_lines++;else if(w==39&&h==39&&rgba[((h/2)*w+w/2)*4+3]==255)pointer_heads++;else pointer_tails++;}
 for(int i=0;i<w*h*4;i+=4){uint32_t rgb=((uint32_t)rgba[i]<<16)|((uint32_t)rgba[i+1]<<8)|rgba[i+2];if(background)assert(rgb==0x282c21&&rgba[i+3]==255);else {unsigned char expected[4];trail_rgba(expected,pointer_colour(),rgba[i+3]);assert(!memcmp(rgba+i,expected,4));}}
 image_calls++;return 0;
}
static int head_seen,head_calls; static az_rect last_head;
static int fake_head(const char *id,az_surface *surface,int on,az_rect head){(void)id;(void)surface;head_seen=on;last_head=head;head_calls++;return 0;}
static uint64_t fake_info_bounds;
static uint64_t fake_info(const char *id,az_surface *surface,int on){(void)id;(void)surface;return on?fake_info_bounds:0;}
static int glass_seen;
static az_rect last_glass_area;
static int fake_glass(const char *id,az_surface *surface,int on,az_rect field){(void)id;(void)surface;assert(field.w>0&&field.h>0);glass_seen=on;last_glass_area=field;return 0;}
static int test_view=AZ_VIEW_DECK;
static int view(void){return test_view;}
static int regtouch(const char *id,az_touch_fn fn,void *u){(void)id;(void)u;(void)fn;assert(!"legacy touch callback must not be registered");return 0;}
static int quantized[4],volume_seen=-1;
static void fake_quantize(int d,int on){quantized[d]=on;}
static void fake_volume(int mid){volume_seen=mid;}
static int sound_calls;
static int sent[4],screens[4],fx_mode[4],fx_rate[4],fx_amount[4];
static void fake_fx(int d,int screen,int send,int rate,int mode,int amount){
 sound_calls++;assert(d>=0&&d<4);assert(rate>=0&&(rate<=5||rate==7||rate==8||rate==9));assert(amount>=0&&amount<=1000);
 sent[d]=send;screens[d]=screen;fx_mode[d]=mode;fx_rate[d]=rate;fx_amount[d]=amount;
}
static int sample_state[4]={0x180,0x180,0x180,0x180};
static int sample_sent[4],sample_screen[4],sample_mode[4],sample_rate[4],sample_amount[4],sample_pitch[4],sample_grid[4];
static int fake_sampler_state(int d){return sample_state[d];}
static void fake_sampler_fx(int d,int screen,int send,int rate,int mode,int amount){sample_screen[d]=screen;sample_sent[d]=send;sample_mode[d]=mode;sample_rate[d]=rate;sample_amount[d]=amount;}
static void fake_sampler_xy(int d,int send,int rate,int gain,int pitch){assert(gain==1000);assert(rate>=0&&rate<=5);sample_sent[d]=send;sample_pitch[d]=pitch;}
static void fake_sampler_select(int d,int sample){sample_state[d]=(sample_state[d]&~7)|sample;}
static int fake_sampler_ui_pick(int d,int sample){for(int k=0;k<4;k++)sample_state[k]&=~0x80;sample_state[d]=(sample_state[d]&~7)|0x80|sample;return 1;}
static void fake_sampler_grid(int d,int on){sample_grid[d]=on;}
static az_core_v1 fake={.abi=1,.size=sizeof(az_core_v1),.deck_count=count,.view=view,.touch_filter=regtouch,.draw_image=image,.draw_rect=border,.cfg_get=cfg_get,.cfg_set=cfg_set,.register_row=regrow,.row_value=val,.deck_info=info,.surface_create=create,.surface_place=place,.surface_show=show,.surface_redraw=redraw,.frame_filter=regfilter,.fill_rect=fill,.draw_text=text};
static void controls(unsigned mask,unsigned cross) {
 unsigned char b[100]={0},original[100];
 for(int d=0;d<4;d++) if(mask&(1u<<d)){b[10+d]=255;b[14]|=3u<<(6-2*d);}
 b[15]=(unsigned char)(cross>>2);b[16]=(unsigned char)(cross&3);memcpy(original,b,sizeof b);
 assert(frame(AZ_BUS_MIXER_IN,b,sizeof b,NULL)==AZ_FRAME_OK);assert(!memcmp(original,b,sizeof b));
}
static void step(long delta) {test_time+=delta;tick();}
static int test_gain(int d,int k){(void)d;return k==0?0:500;}
static int colour_mask=-1;
static int colour_snapshot(int d,int *v,unsigned n){assert(n==12);if(d!=0||colour_mask<0)return 0;memset(v,0,12*sizeof *v);v[4]=colour_mask;v[10]=1;return 1;}
static int fake_guard_bind(const char *id,az_surface *surface){assert(!strcmp(id,ID)&&surface==panel_surface);return 0;}
int main(void) {
 assert(init(NULL)==1);
 glass_guard_bind=fake_guard_bind;assert(!init(&fake));assert(PANEL.layer==AZ_LAYER_TOP);assert(PANEL.layer==AZ_LAYER_TOP);assert(!strcmp(az_mod_v1_entry()->id,ID));assert(selected==-1);assert(values[dim_row]==1&&values[native_touch_row]==1);assert(values[follow_row]==1&&values[audio_row]==1&&values[sampler_audio_row]==1);for(int d=0;d<4;d++){assert(values[assign_row[d]]==(d%2?3:1));values[assign_row[d]]=0;}values[follow_row]=values[audio_row]=values[sampler_audio_row]=0;
 for(int d=0;d<4;d++){fixtures[d].size=sizeof(az_deck_info);fixtures[d].loaded=1;fixtures[d].has_stems=1;fixtures[d].pad_mode=1;}
 geometry_result=(UINT64_C(1)<<48)|(UINT64_C(170)<<24)|500;
 step(10);assert(shown[0]&&!shown[1]);
 az_event tap={.kind=AZ_EV_TAP,.x=1100,.y=10};assert(tap_button(&tap,NULL));assert(opened&&shown[1]);
 for(int d=0;d<4;d++){
  az_rect r=channel_rect(panel_area,d);tap.x=r.x+r.w/2;tap.y=r.y+r.h/2;assert(tap_panel(&tap,NULL));assert(selected==d);
  assert(tap_button(&tap,NULL));assert(!opened);assert(tap_button(&tap,NULL));assert(opened&&selected==d);
 }
 fixtures[0].pad_mode=2;step(100);assert(selected==3&&kind[0]==2);
 fixtures[3].loaded=0;step(100);assert(selected==3);fixtures[3].loaded=1;strcpy(fixtures[3].title,"New track");step(100);assert(selected==3);
 dimmed=0;paint_panel(NULL,panel_area,NULL);assert(dimmed==2);
 values[dim_row]=0;dimmed=0;paint_panel(NULL,panel_area,NULL);assert(dimmed==0);
 values[follow_row]=1;controls(1,0);step(100);controls(2,1023);step(100);step(100);assert(selected==3); /* Unknown assignments -> no guess */
 values[assign_row[0]]=1;values[assign_row[1]]=3;values[assign_row[2]]=values[assign_row[3]]=2;
 controls(3,1023);step(100); /* baseline */
 tap_button(&tap,NULL);assert(!opened);
 controls(3,0);step(10);step(79);assert(selected==3);step(1);assert(selected==0&&!opened);
 controls(3,512);step(100);step(100);assert(selected==0); /* ambiguous center */
 controls(3,1023);step(100);step(80);assert(selected==1&&!opened);
 tap_button(&tap,NULL);assert(opened&&selected==1);
 for(int d=0;d<4;d++){
  controls(0,512);step(100);controls(1u<<d,512);step(100);step(80);assert(selected==d);
 }
 /* Sampler uses the same selection/reopen path as STEMS, including closed follow. */
 values[follow_row]=0;selected=0;fixtures[0].pad_mode=2;step(100);
 assert(kind[0]==2&&opened);visual[0].effect=3;visual[0].sound=2;visual[0].rate=4;
 tap_button(&tap,NULL);assert(!opened);tap_button(&tap,NULL);
 assert(opened&&selected==0&&kind[0]==2&&visual[0].effect==3&&visual[0].sound==2&&visual[0].rate==4);
 values[follow_row]=1;controls(3,0);step(100);tap_button(&tap,NULL);assert(!opened);
 controls(3,1023);step(100);step(80);assert(selected==1&&!opened&&kind[1]==1);
 controls(3,0);step(100);step(80);assert(selected==0&&!opened&&kind[0]==2);
 tap_button(&tap,NULL);assert(opened&&selected==0&&kind[0]==2);
 assert(visual[0].effect==3&&visual[0].sound==2&&visual[0].rate==4);
 values[follow_row]=0;visual[0].effect=visual[0].sound=visual[0].rate=0;
 puts("PASS sampler: same open/close path, preserved sound/effect/rate, closed crossfader follow STEMS and back, inactive deck does not select");
 /* Visual controls are private per-deck; never send audio or native controls. */
 selected=0;kind[0]=2;kind[1]=1;
 az_rect r=effect_rect(panel_area,4);tap.x=r.x+5;tap.y=r.y+5;
 tap_panel(&tap,NULL);assert(visual[0].effect==4 && visual[1].effect==0);
 r=sound_rect(panel_area,3);tap.x=r.x+5;tap.y=r.y+5;tap_panel(&tap,NULL);assert(visual[0].sound==0); /* missing arm bridge must refuse the pick */
 r=field_rect(panel_area);tap.x=r.x+r.w-1;tap.y=r.y+r.h-1;tap_panel(&tap,NULL);
 assert(visual[0].x==1000 && visual[0].y==1000 && visual[0].rate==5);
 for(int mode=2;mode<=4;mode+=2){
  geometry_result=((uint64_t)(mode==4?3:1)<<48)|(UINT64_C(40)<<24)|(mode==4?560:500);
  deck_count_value=mode;step(300);assert(panel_area.h==(mode==4?506:446));
  for(int d=0;d<4;d++){
   r=channel_rect(panel_area,d);tap.x=r.x+5;tap.y=r.y+5;tap_panel(&tap,NULL);assert(selected==d);
  }
  selected=0;paint_panel(NULL,panel_area,NULL);selected=1;paint_panel(NULL,panel_area,NULL);
 }
 geometry_result=0;step(300);assert(opened&&!geometry_ready&&!shown[1]);
 geometry_result=(UINT64_C(3)<<48)|(UINT64_C(212)<<24)|560;step(300);
 assert(opened&&geometry_ready&&shown[1]&&panel_area.x==212&&panel_area.w==1068);
 values[dim_row]=1;stem_gain_read=test_gain;selected=0;fixtures[0].stems_on=1;step(300);
 assert(stem_level(0,0)==0&&stem_level(0,1)==500);
 values[dim_row]=0;assert(stem_level(0,0)==1000&&stem_level(0,1)==1000);
 values[dim_row]=1;
 unsigned parsed=0;
 assert(stem_mask_parse("143\n",4,&parsed)&&parsed==15);
 assert(stem_mask_parse("142",3,&parsed)&&parsed==14);
 assert(stem_mask_parse("0",1,&parsed)&&parsed==0);
 assert(stem_mask_parse("128",3,&parsed)&&parsed==0);
 assert(!stem_mask_parse("256",3,&parsed)&&!stem_mask_parse("143bad",6,&parsed)&&!stem_mask_parse("",0,&parsed));
 stem_mask_snapshot=colour_snapshot;colour_mask=13; /* instrument muted */
 assert(stem_level(0,1)==0&&stem_level(0,2)==500);
 int muted_levels[4];for(int k=0;k<4;k++)muted_levels[k]=stem_level(0,k);
 assert(stem_visual_field(450,1200,muted_levels,0)==0x191919);
 values[dim_row]=0;assert(stem_level(0,1)==0&&stem_level(0,2)==1000);
 values[dim_row]=1;colour_mask=15;assert(stem_level(0,1)==500); /* EQ amount restored */
 long saved_time=test_time;
 stem_mask_cache[1]=((uint64_t)test_time<<9)|256u|14u;
 assert(stem_mask_value(1)==14);test_time+=1001;assert(stem_mask_value(1)==-1);test_time=saved_time;
 stem_mask_cache[1]=0;stem_mask_snapshot=NULL;colour_mask=-1;
 puts("PASS panel colours: per-stem mute plus EQ, dim-off preserves mute, unmute restores EQ, field shade, hidden-deck cache and malformed/stale data");
 assert(stem_visual_field(100,1000,(int[4]){0,0,0,0},0)==0x191919);
 assert(image_calls>0);
 /* Live controls: correct deck/mode, drag, release, latch, close/view/track fail-off. */
 values[follow_row]=0;values[audio_row]=1;stem_fx_send=fake_fx;stem_fx_quantize=fake_quantize;stem_fx_volume=fake_volume;mic2_mid=512;
 selected=0;kind[0]=1;fixtures[0].pad_mode=1;fixtures[0].stems_on=1;visual[0].latch=0;opened=1;
 step(30);assert(audio_ready()&&!sent[0]&&quantized[0]&&volume_seen==512);
 r=field_rect(panel_area);az_touch contact={.slot=0,.x=r.x+r.w/2,.y=r.y,.down=1};
 assert(audio_touch(&contact,NULL)==AZ_TOUCH_TAKE);step(30);assert(sent[0]&&fx_amount[0]==1000&&fx_rate[0]==3);
 contact.down=0;audio_touch(&contact,NULL);step(30);assert(!sent[0]&&screens[0]);
 visual[0].latch=1;step(30);assert(!sent[0]); /* latch alone must never restart sound */
 contact.down=1;audio_touch(&contact,NULL);step(30);assert(sent[0]);
 contact.down=0;audio_touch(&contact,NULL);step(30);assert(sent[0]);
 opened=0;step(30);assert(!sent[0]&&!screens[0]&&!visual[0].latch&&!touch_bounds&&!quantized[0]);
 opened=1;step(30);
 for(int k=0;k<5;k++){
  visual[0].effect=k;step(30);contact.down=1;audio_touch(&contact,NULL);step(30);
  assert((sent[0]&&fx_mode[0]==(int[]){2,0,3,4,5}[k]));
  contact.down=0;audio_touch(&contact,NULL);step(30);assert(!sent[0]);
 }
 contact.down=1;audio_touch(&contact,NULL);step(30);assert(sent[0]);
 selected=1;fixtures[1].stems_on=1;step(30);assert(!sent[0]&&!sent[1]);
 test_view=AZ_VIEW_MENU;step(30);assert(!touch_bounds&&!screens[1]);
 assert(audio_touch(&contact,NULL)==AZ_TOUCH_TAKE);test_view=AZ_VIEW_DECK;step(30);
 contact.down=0;audio_touch(&contact,NULL);
 selected=0;step(30);contact.down=1;audio_touch(&contact,NULL);step(30);assert(sent[0]);
 strcpy(fixtures[0].title,"Replacement track");step(30);assert(!sent[0]);
 contact.down=0;audio_touch(&contact,NULL);step(30);

 /* Top-edge drag remains held and clamps; labels shield initial touches. */
 visual[0].latch=1;step(30);r=field_rect(panel_area);
 contact=(az_touch){.slot=0,.x=r.x+r.w/2,.y=r.y+10,.down=1};
 assert(audio_touch(&contact,NULL)==AZ_TOUCH_TAKE);step(30);assert(sent[0]);
 contact.y=r.y-30;assert(audio_touch(&contact,NULL)==AZ_TOUCH_TAKE);step(30);
 assert(touching&&sent[0]&&visual[0].y==0); /* top edge, no synthetic lift */
 contact.x=r.x-30;audio_touch(&contact,NULL);step(30);assert(visual[0].x==0&&touching);
 contact.x=r.x+r.w+30;contact.y=r.y+r.h+30;audio_touch(&contact,NULL);step(30);
 assert(visual[0].x==1000&&visual[0].y==1000&&touching);
 contact.down=0;audio_touch(&contact,NULL);step(30);assert(!touching&&sent[0]);
 int old_x=visual[0].x,old_y=visual[0].y,old_effect=visual[0].effect;
 az_rect label=sound_rect(panel_area,0);contact.x=label.x+5;contact.y=label.y+5;contact.down=1;
 assert(audio_touch(&contact,NULL)==AZ_TOUCH_TAKE);step(30);
 contact.y=r.y+30;assert(audio_touch(&contact,NULL)==AZ_TOUCH_TAKE);step(30);
 assert(visual[0].x==old_x&&visual[0].y==old_y&&visual[0].effect==old_effect&&sent[0]);
 contact.down=0;audio_touch(&contact,NULL);step(30);
 /* A held contact cancelled by close cannot restart after reopen. */
 contact.x=r.x+20;contact.y=r.y+20;contact.down=1;audio_touch(&contact,NULL);step(30);
 opened=0;step(30);opened=1;step(30);audio_touch(&contact,NULL);step(30);
 assert(!sent[0]&&!touching);contact.down=0;audio_touch(&contact,NULL);step(30);
 /* Sampler field has identical edge tracking without a sampler audio API. */
 fixtures[0].pad_mode=2;step(30);assert(kind[0]==2&&!audio_live);
 int calls_before=sound_calls;r=field_rect(panel_area);
 contact=(az_touch){.slot=0,.x=r.x+20,.y=r.y+20,.down=1};
 assert(audio_touch(&contact,NULL)==AZ_TOUCH_TAKE);step(30);assert(touching);
 contact.y=r.y-30;audio_touch(&contact,NULL);step(30);assert(touching&&visual[0].y==0);
 contact.x=r.x+r.w+30;audio_touch(&contact,NULL);step(30);assert(visual[0].x==1000&&touching);
 contact.down=0;audio_touch(&contact,NULL);step(30);assert(!touching&&sound_calls==calls_before);
 /* Sample buttons remain available and STEMS-only touch shield is gone. */
 assert(!stem_label_bounds);az_rect kick=sound_rect(panel_area,1);
 tap.x=kick.x+5;tap.y=kick.y+5;tap_panel(&tap,NULL);assert(visual[0].sound==0); /* no audio/arm API: no fake sample selection */
 contact.x=kick.x+5;contact.y=kick.y+5;contact.down=1;
 assert(audio_touch(&contact,NULL)==AZ_TOUCH_PASS);contact.down=0;audio_touch(&contact,NULL);
 /* STEMS visual tracking also works with audio disabled. */
 fixtures[0].pad_mode=1;values[audio_row]=0;step(30);assert(!audio_live&&stem_label_bounds);
 contact=(az_touch){.slot=0,.x=r.x+30,.y=r.y+30,.down=1};
 assert(audio_touch(&contact,NULL)==AZ_TOUCH_TAKE);step(30);assert(touching&&!sent[0]);
 contact.down=0;audio_touch(&contact,NULL);step(30);
 /* A stalled pointer worker cannot leave a continuing audio send. */
 values[audio_row]=1;step(30);contact.down=1;audio_touch(&contact,NULL);step(30);assert(sent[0]);
 pointer_poll_on=1;pointer_heartbeat=(uint64_t)test_time;step(201);assert(!sent[0]&&!touch_bounds);
 pointer_heartbeat=(uint64_t)test_time;step(1);audio_touch(&contact,NULL);step(1);assert(!sent[0]);
 contact.down=0;audio_touch(&contact,NULL);pointer_poll_on=0;step(30);
 /* A stalled pointer worker cannot leave a continuing audio send. */
 values[audio_row]=1;step(30);contact.down=1;audio_touch(&contact,NULL);step(30);assert(sent[0]);
 pointer_poll_on=1;pointer_heartbeat=(uint64_t)test_time;step(201);assert(!sent[0]&&!touch_bounds);
 pointer_heartbeat=(uint64_t)test_time;step(1);audio_touch(&contact,NULL);step(1);assert(!sent[0]);
 contact.down=0;audio_touch(&contact,NULL);pointer_poll_on=0;step(30);
 /* The sixth effect occupies the free gap before LATCH and routes mode 7. */
 az_rect er=effect_rect(panel_area,5),la=local_rect(panel_area,790,2,120,36);
 assert(er.x+er.w<=la.x);
 tap.x=er.x+er.w/2;tap.y=er.y+er.h/2;
 tap_panel(&tap,NULL);assert(visual[selected].effect==5);
 step(30);
 /* Genomsikt captures the whole field before native needle-drop, while the
  * independent raw-pointer callback still tracks and sends the existing FX. */
 glass_surface=fake_glass;glass_head=fake_head;selected=0;kind[0]=1;fixtures[0].pad_mode=1;opened=1;step(30);
 assert(glass_seen&&glass_input_bounds);
 az_rect gf=field_rect(panel_area);az_touch gt={.slot=0,.down=1,.x=gf.x+30,.y=gf.y+30};
 assert(glass_touch(&gt,NULL)==AZ_TOUCH_TAKE);gt.x=gf.x-30;assert(glass_touch(&gt,NULL)==AZ_TOUCH_TAKE);
 gt.down=0;assert(glass_touch(&gt,NULL)==AZ_TOUCH_TAKE);gt.down=1;assert(glass_touch(&gt,NULL)==AZ_TOUCH_PASS);
 gt.down=0;glass_touch(&gt,NULL);
 kind[0]=2;fixtures[0].pad_mode=2;step(30);assert(glass_seen&&glass_input_bounds);
 values[sampler_layout_row]=0;step(30);assert(!glass_seen&&!glass_input_bounds);
 gt.down=1;gt.x=gf.x+30;assert(glass_touch(&gt,NULL)==AZ_TOUCH_PASS);gt.down=0;glass_touch(&gt,NULL);
 values[stem_layout_row]=1;kind[0]=1;fixtures[0].pad_mode=1;step(30);assert(!glass_seen);
 values[stem_layout_row]=0;step(30);assert(!glass_seen);
 values[stem_layout_row]=2;step(30);assert(glass_seen);
 kind[0]=1;fixtures[0].pad_mode=1;opened=0;step(30);assert(!glass_seen&&!glass_input_bounds);
 puts("PASS glass field: field-only input capture, until lift, no header/sample capture, closed cleanup");
 /* Sampler uses current touch lifecycle and never arms the physical pad. */
 sampler_ui_pick=fake_sampler_ui_pick;sampler_state=fake_sampler_state;sampler_fx_send=fake_sampler_fx;sampler_xy_send=fake_sampler_xy;sampler_select=fake_sampler_select;sampler_quantize=fake_sampler_grid;
 values[sampler_audio_row]=1;selected=0;fixtures[0].pad_mode=2;kind[0]=2;opened=1;visual[0].effect=0;visual[0].rate=2;visual[0].latch=0;step(30);
 assert(audio_ready()&&!sample_sent[0]);r=field_rect(panel_area);
 contact=(az_touch){.slot=0,.down=1,.x=r.x+r.w/2,.y=r.y+10};audio_touch(&contact,NULL);step(30);
 assert(sample_sent[0]&&sample_mode[0]==6&&visual[0].point&&sample_grid[0]);
 contact.down=0;audio_touch(&contact,NULL);step(30);assert(!sample_sent[0]&&!visual[0].point);
 visual[0].latch=1;step(30);assert(!sample_sent[0]&&!visual[0].point);
 contact.down=1;audio_touch(&contact,NULL);step(30);contact.down=0;audio_touch(&contact,NULL);step(30);assert(sample_sent[0]&&visual[0].point);
 visual[0].latch=0;step(30);assert(!sample_sent[0]&&!visual[0].point);
 for(int effect=0;effect<5;effect++){
  visual[0].effect=effect;step(30);contact.down=1;audio_touch(&contact,NULL);step(30);
  assert((sample_sent[0]&&sample_mode[0]==(int[]){6,2,3,6,2}[effect]));
  assert(sample_pitch[0]==((effect==3||effect==4)?1000-visual[0].y:-1));
  contact.down=0;audio_touch(&contact,NULL);step(30);assert(!sample_sent[0]);
 }
 values[sampler_audio_row]=0;sample_state[0]=0x100;sample_state[1]=0x180;step(30);
 for(int sample=0;sample<4;sample++){
  r=sound_rect(panel_area,sample);tap.x=r.x+5;tap.y=r.y+5;tap_panel(&tap,NULL);step(30);
  assert(selected==0&&visual[0].sound==sample&&sample_state[0]==(0x180|sample)&&!(sample_state[1]&0x80)&&!sample_sent[0]);
 }
 values[sampler_audio_row]=1;step(30);assert(!sample_sent[0]);
 r=sound_rect(panel_area,3);tap.x=r.x+5;tap.y=r.y+5;tap_panel(&tap,NULL);step(30);assert(visual[0].sound==3&&(sample_state[0]&7)==3);
 visual[0].latch=1;contact.down=1;audio_touch(&contact,NULL);step(30);assert(sample_sent[0]);
 selected=1;step(30);assert(!sample_sent[0]&&!visual[0].latch);
 selected=0;step(30);sample_state[0]=0x100;step(30);assert(!audio_ready()&&!sample_sent[0]);
 sample_state[0]=0x180;step(30);assert(!sample_sent[0]); /* re-arm does not restart a held contact */
 contact.down=0;audio_touch(&contact,NULL);step(30);contact.down=1;audio_touch(&contact,NULL);step(30);assert(sample_sent[0]);
 opened=0;step(30);assert(!sample_sent[0]&&!sample_screen[0]&&!touch_bounds&&!visual[0].point);
 puts("PASS sampler audio: five modes, sample selection, quantize, release cursor, retained latch cursor, channel/close cleanup, pad-8 guard and no automatic restart");
 /* Sampler bypass status uses its own selected/off state and standard styling. */
 opened=1;selected=0;kind[0]=2;fixtures[0].pad_mode=2;visual[0].effect=0;
 off_count=on_count=on_muted=echo_active=0;paint_panel(NULL,panel_area,NULL);
 assert(off_count==1&&!on_count);
 visual[0].effect=1;off_count=on_count=on_muted=echo_active=0;paint_panel(NULL,panel_area,NULL);
 assert(!off_count&&on_count==1&&on_muted==1&&echo_active==1);
 r=effect_rect(panel_area,0);tap.x=r.x+5;tap.y=r.y+5;tap_panel(&tap,NULL);step(30);assert(visual[0].effect==0);
 paint_button(NULL,(az_rect){1090,8,182,48},NULL);
 puts("PASS OFF/ON styling and first-position bypass; PITCH ECHO sends current echo and pitch values; test field/launcher text removed");
 /* V51-style preparation: only a settled mixer transition changes channel. */
 sampler_state=fake_sampler_state;values[sampler_audio_row]=1;values[audio_row]=1;
 values[assign_row[0]]=1;values[assign_row[1]]=3;
 for(int source=1;source<=2;source++)for(int target=1;target<=2;target++)for(int closed=0;closed<=1;closed++){
  values[follow_row]=0;step(30);selected=1;opened=!closed;
  fixtures[0].loaded=fixtures[1].loaded=1;fixtures[1].has_stems=1;fixtures[1].stems_on=1;
  fixtures[1].pad_mode=source;kind[1]=source;fixtures[0].pad_mode=1;kind[0]=1;
  controls(3,1023);values[follow_row]=1;step(100);
  fixtures[0].pad_mode=target;sample_state[0]=0x100;step(100);
  assert(selected==1&&kind[1]==source&&opened==!closed);
  sample_state[0]=0x180;step(100); /* pad 8 activation on A */
  assert(selected==1&&kind[0]==target&&kind[1]==source&&opened==!closed&&!sample_sent[0]);
  controls(3,0);step(1);step(79);assert(selected==1);step(1);
  assert(selected==0&&kind[0]==target&&opened==!closed&&!sample_sent[0]);
  if(closed){tap_button(&tap,NULL);assert(opened&&selected==0&&kind[0]==target);}
  controls(3,1023);step(100);step(80);assert(selected==1&&kind[1]==source&&!sample_sent[0]);
 }
 puts("PASS preparation: all STEMS/sampler pairs, open/closed, inactive SLIP/pad-8 activation preserves B, mixer transition alone follows A, no sound auto-start");
 /* Hardware reproduction: CH4 empty with its B-assigned fader partly raised. */
 values[follow_row]=0;step(30);selected=0;opened=1;
 fixtures[0].loaded=fixtures[1].loaded=1;fixtures[2].loaded=fixtures[3].loaded=0;
 fixtures[0].pad_mode=1;fixtures[1].pad_mode=2;
 values[assign_row[0]]=values[assign_row[2]]=1;values[assign_row[1]]=values[assign_row[3]]=3;
 unsigned char actual[100]={0};unsigned levels[]={462,406,0,137};
 for(int d=0;d<4;d++){actual[10+d]=levels[d]>>2;actual[14]|=(levels[d]&3u)<<(6-2*d);}
 frame(AZ_BUS_MIXER_IN,actual,sizeof actual,NULL);values[follow_row]=1;step(100);assert(selected==0);
 fixtures[1].pad_mode=1;step(100);fixtures[1].pad_mode=2;sample_state[1]=0x180;step(100);assert(selected==0);
 actual[15]=255;actual[16]=3;frame(AZ_BUS_MIXER_IN,actual,sizeof actual,NULL);step(1);step(79);assert(selected==0);step(1);
 assert(selected==1&&kind[1]==2&&previous_mask==2);
 actual[15]=0;actual[16]=0;frame(AZ_BUS_MIXER_IN,actual,sizeof actual,NULL);step(100);step(80);assert(selected==0&&kind[0]==1);
 /* A real second loaded deck stays ambiguous; do not guess between two tracks. */
 fixtures[3].loaded=1;step(100);actual[15]=255;actual[16]=3;frame(AZ_BUS_MIXER_IN,actual,sizeof actual,NULL);step(100);step(80);assert(selected==0&&previous_mask==10);
 fixtures[3].loaded=-1;step(100);assert(selected==0&&previous_mask==10);
 puts("PASS hardware regression: 462/406/0/137 faders, empty CH4 ignored, 80ms A/B STEMS/sampler follow, loaded/unknown CH4 stays ambiguous, SLIP preparation never steals");
 contact.down=0;audio_touch(&contact,NULL);step(30); /* lift the preceding scenario contact */
 /* Independent per-kind memory on each logical deck, including six STEMS FX. */
 values[follow_row]=0;opened=1;
 for(int d=0;d<4;d++){fixtures[d].loaded=fixtures[d].has_stems=fixtures[d].stems_on=1;}
 for(int d=0;d<4;d++){
  selected=d;fixtures[d].pad_mode=1;step(30);
  visual[d].effect=5-d;visual[d].rate=((int[]){7,9,5,3}[d]);visual[d].x=110+d;visual[d].y=220+d;
  fixtures[d].pad_mode=2;step(30);
  r=sound_rect(panel_area,d);tap.x=r.x+5;tap.y=r.y+5;tap_panel(&tap,NULL);step(30);
  assert(visual[d].sound==d);
  visual[d].effect=4-d;visual[d].rate=(d+2)%6;visual[d].x=710+d;visual[d].y=820+d;
  fixtures[d].pad_mode=1;step(30);
  assert(visual[d].effect==5-d&&visual[d].rate==((int[]){7,9,5,3}[d])&&visual[d].x==110+d&&visual[d].y==220+d);
  assert(!visual[d].latch&&!visual[d].point&&!audio_armed);
  tap_button(&tap,NULL);step(30);tap_button(&tap,NULL);step(30);
  assert(opened&&selected==d&&visual[d].effect==5-d&&visual[d].rate==((int[]){7,9,5,3}[d]));
  fixtures[d].pad_mode=2;step(30);
  assert(visual[d].sound==d&&visual[d].effect==4-d&&visual[d].rate==(d+2)%6&&visual[d].x==710+d&&visual[d].y==820+d);
  tap_button(&tap,NULL);step(30);tap_button(&tap,NULL);step(30);
  assert(opened&&selected==d&&visual[d].sound==d&&visual[d].effect==4-d&&visual[d].rate==(d+2)%6&&!sample_sent[d]);
 }
 for(int d=0;d<4;d++){
  selected=d;fixtures[d].pad_mode=1;step(30);assert(visual[d].effect==5-d&&visual[d].rate==((int[]){7,9,5,3}[d]));
  fixtures[d].pad_mode=2;step(30);assert(visual[d].sound==d&&visual[d].effect==4-d&&visual[d].rate==(d+2)%6);
 }
 /* A latched live contact cannot cross to the remembered other panel. */
 selected=3;step(30);visual[3].latch=1;r=field_rect(panel_area);
 contact=(az_touch){.slot=0,.down=1,.x=r.x+60,.y=r.y+60};audio_touch(&contact,NULL);step(30);assert(sample_sent[3]);
 fixtures[3].pad_mode=1;step(30);assert(!sample_sent[3]&&!sent[3]&&!visual[3].latch&&!audio_armed);
 fixtures[3].pad_mode=2;step(30);audio_touch(&contact,NULL);step(30);assert(!sample_sent[3]);
 contact.down=0;audio_touch(&contact,NULL);step(30);
 puts("PASS memory: separate sound/effect/rate/XY per panel on all four decks, repeated close/reopen and channel changes, STEMS sixth effect retained, no stale LATCH/held-contact restart");
 /* The visual mask includes the unused right strip, the touch field does not. */
 contact.down=0;audio_touch(&contact,NULL);glass_touch(&contact,NULL);
 selected=0;opened=1;glass_surface=fake_glass;values[follow_row]=0;
 int layout_edges[6]={170,40,170,40,212,40};
 for(int layout=0;layout<6;layout++)for(int context=1;context<=2;context++){
  int mode=layout/2;deck_count_value=mode==2?4:2;
  geometry_result=((uint64_t)(mode+1)<<48)|((uint64_t)layout_edges[layout]<<24)|(mode==2?560:500);
  fixtures[0].loaded=fixtures[0].has_stems=fixtures[0].stems_on=1;fixtures[0].pad_mode=context;
  values[stem_layout_row]=values[sampler_layout_row]=2;step(30);
  r=field_rect(panel_area);assert(glass_seen&&last_glass_area.x==r.x&&last_glass_area.y==r.y&&last_glass_area.h==r.h);
  assert(last_glass_area.x+last_glass_area.w==1280&&r.x+r.w<1280);
  for(int icon=0;icon<4;icon++){az_rect marker=axis_rect(r,icon);assert(marker.x>=r.x&&marker.y>=r.y&&marker.x+marker.w<=r.x+r.w&&marker.y+marker.h<=r.y+r.h);}
  az_rect left_marker=axis_rect(r,0),right_marker=axis_rect(r,1);assert(left_marker.x-r.x==r.x+r.w-right_marker.x-right_marker.w&&left_marker.y==right_marker.y);
  int before_axis=axis_calls;paint_panel(NULL,panel_area,NULL);assert(axis_calls==before_axis+4);
  info_passthrough=fake_info;fake_info_bounds=pack_touch_rect((az_rect){layout_edges[layout]+(layout%2?6:mode==2?5:2),mode==2?291:250,46,59});step(30);
  uint64_t old_touch=touch_sample;az_touch info_touch={.slot=0,.down=1,.x=(int)(fake_info_bounds&2047)+10,.y=(int)((fake_info_bounds>>11)&2047)+10};
  assert(glass_touch(&info_touch,NULL)==AZ_TOUCH_PASS);assert(audio_touch(&info_touch,NULL)==AZ_TOUCH_PASS);step(30);assert(touch_sample==old_touch);
  info_touch.x=r.x+r.w/2;info_touch.y=r.y+10;audio_touch(&info_touch,NULL);step(30);assert(touch_sample==old_touch);info_touch.down=0;audio_touch(&info_touch,NULL);glass_touch(&info_touch,NULL);
  info_touch=(az_touch){.slot=0,.down=1,.x=r.x+r.w/2,.y=r.y+10};assert(glass_touch(&info_touch,NULL)==AZ_TOUCH_TAKE);assert(audio_touch(&info_touch,NULL)==AZ_TOUCH_TAKE);step(30);assert(touching);
  info_touch.x=(int)(fake_info_bounds&2047)+10;info_touch.y=(int)((fake_info_bounds>>11)&2047)+10;assert(glass_touch(&info_touch,NULL)==AZ_TOUCH_TAKE);assert(audio_touch(&info_touch,NULL)==AZ_TOUCH_TAKE);step(30);assert(touching);info_touch.down=0;glass_touch(&info_touch,NULL);audio_touch(&info_touch,NULL);step(30);
  fake_info_bounds=0;step(30);assert(!touch_info_hole);
  int columns=context==1?8:6;const int target_rates[8]={9,5,4,7,3,2,1,0};
  for(int col=0;col<columns;col++){
   az_rect button=rate_rect(panel_area,context==2,col);assert(button.w>0&&button.h>0);
   if(context==2){az_rect previous=local_rect(panel_area,16+col*194,420,180,24);assert(!memcmp(&button,&previous,sizeof button));}
   tap.x=button.x+button.w/2;tap.y=button.y+button.h/2;tap_panel(&tap,NULL);assert(visual[0].rate==(context==1?target_rates[col]:col));
   az_touch touch_rate={.slot=0,.down=1,.x=r.x+(2*col+1)*r.w/(2*columns),.y=r.y+10};audio_touch(&touch_rate,NULL);step(30);assert(visual[0].rate==(context==1?target_rates[col]:col));touch_rate.down=0;audio_touch(&touch_rate,NULL);step(30);
  }
  uint64_t original_bounds=__atomic_load_n(&touch_bounds,__ATOMIC_ACQUIRE);
  gt=(az_touch){.slot=0,.down=1,.x=1279,.y=r.y+r.h/2};
  assert(touch_in(glass_input_bounds,gt.x,gt.y)&&!touch_in(original_bounds,gt.x,gt.y));
  assert(glass_touch(&gt,NULL)==AZ_TOUCH_TAKE);assert(audio_touch(&gt,NULL)==AZ_TOUCH_PASS);step(30);
  assert(!touching&&!audio_armed);
  gt.x=r.x+20;audio_touch(&gt,NULL);step(30);assert(!touching&&!audio_armed);
  gt.down=0;glass_touch(&gt,NULL);audio_touch(&gt,NULL);step(30);
  for(int choice=0;choice<2;choice++){
   values[context==2?sampler_layout_row:stem_layout_row]=choice;step(30);
   assert(!glass_seen&&!glass_input_bounds);assert(__atomic_load_n(&touch_bounds,__ATOMIC_ACQUIRE)==original_bounds);
  }
 }
 puts("PASS full-width glass: STEMS/sampler, 2/2+4/4 decks with/without info, screen right edge1280, original XY/header/rate geometry retained, no native FX touch fallthrough, opaque modes unchanged");
 /* One launcher surface, original small A.CUE rectangle, unchanged panel. */
 assert(values[launcher_row]==0);az_rect panel_before=panel_area;int old_selected=selected,old_opened=opened;
 assert(placed[0].x==1090&&placed[0].y==8&&placed[0].w==182&&placed[0].h==48&&surfaces==2);
 values[launcher_row]=1;step(30);
 assert(placed[0].x==872&&placed[0].y==19&&placed[0].w==64&&placed[0].h==30&&surfaces==2);
 assert(!memcmp(&panel_before,&panel_area,sizeof panel_before)&&selected==old_selected&&opened==old_opened);
 paint_button(NULL,placed[0],NULL);tap.x=880;tap.y=25;tap_button(&tap,NULL);assert(opened!=old_opened);tap_button(&tap,NULL);assert(opened==old_opened);
 values[launcher_row]=0;step(30);assert(placed[0].x==1090&&placed[0].w==182&&surfaces==2);
 /* Reversed STEMS visible columns map to unchanged canonical DSP indices. */
 selected=0;fixtures[0].pad_mode=1;fixtures[0].has_stems=1;opened=1;step(30);
 const int expected_stem_rates[8]={9,5,4,7,3,2,1,0};
 for(int column=0;column<8;column++){
  r=rate_rect(panel_area,0,column);tap.x=r.x+r.w/2;tap.y=r.y+r.h/2;tap_panel(&tap,NULL);assert(visual[0].rate==expected_stem_rates[column]);
  r=field_rect(panel_area);contact=(az_touch){.slot=0,.down=1,.x=r.x+(2*column+1)*r.w/16,.y=r.y+20};audio_touch(&contact,NULL);step(30);assert(visual[0].rate==expected_stem_rates[column]);
  contact.down=0;audio_touch(&contact,NULL);step(30);
 }
 fixtures[0].pad_mode=2;step(30);
 for(int column=0;column<6;column++){r=local_rect(panel_area,16+column*194,420,180,24);tap.x=r.x+r.w/2;tap.y=r.y+r.h/2;tap_panel(&tap,NULL);assert(visual[0].rate==column);}
 puts("PASS launcher: one surface, large default/original A.CUE option, panel position/state unchanged; reversed STEMS beat buttons and finger columns map to existing DSP, sampler order unchanged");
 sampler_bank=fake_bank;selected=0;kind[0]=2;opened=active=geometry_ready=1;
 az_rect before_area=panel_area;struct visual_state before_bank=visual[0];int audio_calls=bank_calls;
 for(int k=0;k<4;k++){
  az_rect r=bank_rect(panel_area,k);tap.x=r.x+r.w/2;tap.y=r.y+r.h/2;
  assert(tap_panel(&tap,NULL));assert(bank_value==k+1&&bank_calls==k+1);
  assert(!memcmp(&visual[0],&before_bank,sizeof before_bank));assert(selected==0&&opened&&bank_calls==audio_calls+k+1);
 }
 bank_value=2;paint_panel(NULL,panel_area,NULL);assert(bank_value==2&&bank_calls==4);
 kind[0]=1;tap_panel(&tap,NULL);assert(bank_calls==4);kind[0]=2;
 assert(!memcmp(&before_area,&panel_area,sizeof panel_area));
 for(int mode=0;mode<4;mode++){
  az_rect a={mode&1?40:212,54,1280-(mode&1?40:212),mode&2?506:446};
  az_rect prev=effect_rect(a,4);
  for(int k=0;k<4;k++){az_rect r=bank_rect(a,k);assert(r.x>=prev.x+prev.w&&r.y==prev.y&&r.h==prev.h);prev=r;}
  az_rect latch=local_rect(a,790,2,120,36);assert(prev.x+prev.w<latch.x);
 }
 values[bank_buttons_row]=0;kind[0]=2;int calls_hidden=bank_calls;
 for(int k=0;k<4;k++){az_rect r=bank_rect(panel_area,k);tap.x=r.x+r.w/2;tap.y=r.y+r.h/2;tap_panel(&tap,NULL);assert(bank_calls==calls_hidden);}
 values[bank_buttons_row]=1;
 puts("PASS banks: all four choices, current-menu readback, no STEMS action, unchanged panel/effect/rate/sound/channel/audio, no overlap in four layouts");
 assert(values[pointer_style_row]==1&&values[pointer_colour_row]==0&&pointer_colour()==0x38c4ff);
 selected=0;kind[0]=2;visual[0].x=visual[0].y=500;
 struct visual_state same_visual=visual[0];static const uint32_t expected_colours[]={0x38c4ff,0xffffff,0xff3030,0x22e022};
 assert(values[trail_size_row]==0);
 for(int size=0;size<2;size++)for(int style=0;style<3;style++)for(int colour=0;colour<5;colour++){
  values[trail_size_row]=size;
  values[pointer_style_row]=style;values[pointer_colour_row]=colour;assert(pointer_colour()==(colour==4?pointer_rainbow(test_time):expected_colours[colour]));
  trail_reset();trail_update(test_time,450,450,1);pointer_lines=pointer_heads=pointer_tails=0;
  pointer_paint(NULL,field_rect(panel_area),&visual[0]);
  assert(pointer_heads==1);assert((pointer_lines>0)==(style!=1));assert((pointer_tails>0)==(style!=2));
  assert(!memcmp(&same_visual,&visual[0],sizeof same_visual));
 }
 values[pointer_style_row]=2;active=opened=geometry_ready=1;visual[0].point=1;touching=1;pointer_tick(test_time+33);assert(!trail_count);
 values[pointer_style_row]=1;values[pointer_colour_row]=0;values[trail_size_row]=0;trail_reset();
 puts("PASS pointer options: tail-only blue default, all30 style/colour/size combinations, slim default, one round head in every mode, no trail animation in cross-only, visual/audio choices unchanged");
 static const uint32_t anchors[]={0xff3030,0xffe030,0x30e030,0x30e0ff,0x4060ff,0xe030ff,0xff3030};
 for(int k=0;k<7;k++)assert(pointer_rainbow(k*2048)==anchors[k]);
 assert(pointer_rainbow(-1)==anchors[0]&&pointer_rainbow(12288*100+2048)==anchors[1]);
 for(int t=0;t<12288;t+=8){uint32_t a=pointer_rainbow(t),b=pointer_rainbow(t+8);for(int shift=0;shift<=16;shift+=8){int delta=(int)((a>>shift)&255)-(int)((b>>shift)&255);assert(delta>=-1&&delta<=1);}}
 selected=0;active=opened=geometry_ready=1;visual[0].point=visual[0].latch=1;touching=0;trail_reset();
 values[pointer_colour_row]=4;values[pointer_style_row]=2;int before_rainbow=paints;long rainbow_at=trail_redraw+1000;
 pointer_tick(rainbow_at);assert(paints>before_rainbow&&!trail_count);before_rainbow=paints;
 pointer_tick(rainbow_at+20);assert(paints==before_rainbow);pointer_tick(rainbow_at+80);assert(paints>before_rainbow);
 opened=0;before_rainbow=paints;pointer_tick(rainbow_at+1000);assert(paints==before_rainbow&&!trail_count);
 values[pointer_style_row]=1;values[pointer_colour_row]=0;values[trail_size_row]=0;
 puts("PASS rainbow: continuous12.288s integer cycle, old colour indices unchanged, head/trail/cross share one colour, LATCH/cross-only animation bounded, hidden animation idle, no audio changes");
 glass_head=fake_head;values[stem_layout_row]=2;values[sampler_layout_row]=2;
 selected=0;opened=active=geometry_ready=1;visual[0].point=1;visual[0].latch=0;touching=1;
 pointer_tick(10000);assert(trail_count==1);assert(head_seen&&last_head.w==39&&last_head.h==39);az_rect expected_field=field_rect(panel_area);assert(last_head.x+19==expected_field.x+visual[0].x*(expected_field.w-1)/1000&&last_head.y+19==expected_field.y+visual[0].y*(expected_field.h-1)/1000);touching=0;pointer_tick(10033);assert(!trail_count);assert(!head_seen);
 visual[0].latch=1;touching=1;pointer_tick(10100);assert(trail_count==1);touching=0;pointer_tick(10133);assert(trail_count==1&&visual[0].point);assert(head_seen);
 pointer_tick(10500);assert(!trail_count&&visual[0].point);touching=1;pointer_tick(10600);assert(trail_count==1);opened=0;pointer_tick(10633);assert(!trail_count);assert(!head_seen);
 opened=1;touching=1;pointer_tick(10700);assert(trail_count==1);audio_stop();assert(!trail_count);
 values[stem_layout_row]=0;values[sampler_layout_row]=0;opened=1;touching=1;pointer_tick(10800);assert(!head_seen);values[stem_layout_row]=2;values[sampler_layout_row]=2;
 puts("PASS opaque head API: follows actual finger position, transparent only, LATCH retained, lift and close clear");
 puts("PASS pointer lifecycle: lift clears without LATCH, LATCH retains head while tail expires, close and context stop clear history");
 values[enable_row]=0;step(100);assert(!shown[0]&&!shown[1]);int old=selected;controls(1,0);step(100);assert(selected==old);
 assert(!tap_button(&tap,NULL));
 puts("PASS: SDK init, unknown deck, per-deck memory, inactive pad, track replacement, default dim toggle, unknown routing, 80ms follow, crossfader, ambiguous mix, all four channels, disabled idle, native frames unchanged.");
 return 0;
}




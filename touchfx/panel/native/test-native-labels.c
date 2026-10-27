/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GEOMETRY_TEST
#include "native-labels.c"
static int rows[2],nr,ns,shown[4],mask=15,calls,four,ink_count;
static uint32_t inks[4];
/* Match sdk_touch_read: PASS contacts are never offered again, including up. */
static int drive_touch(const az_touch *t,void *u){
 static int decided,taken;
 if(!t->down){int result=taken?extra_touch(t,u):AZ_TOUCH_PASS;decided=taken=0;return result;}
 if(!decided){decided=1;taken=extra_touch(t,u)==AZ_TOUCH_TAKE;}
 else if(taken)extra_touch(t,u);
 return taken?AZ_TOUCH_TAKE:AZ_TOUCH_PASS;
}
static int regtouch(const char *id,az_touch_fn fn,void *u){(void)u;assert(!strcmp(id,ID)&&fn==extra_touch);return 0;}
static int second_ready,second_x,second_y;
static int fake_second(const char *id,int *p,unsigned n){assert(!strcmp(id,ID)&&n==2);if(!second_ready)return 0;second_ready=0;p[0]=second_x;p[1]=second_y;return 1;}
static int count(void){return four?4:2;}
static int view(void){return AZ_VIEW_DECK;}
static int deck_info(int d,az_deck_info *out){(void)d;(void)out;return 0;}
static int start(const char *id,const char *name,void *(*fn)(void *),void *u){(void)id;(void)name;(void)fn;(void)u;return 0;}
static int value(int n){assert(n>=0&&n<2);return rows[n];}
static int reg(const char *id,const az_row_v1 *r){assert(strlen(id)<=24&&strlen(r->key)<=24&&strlen(r->label)<=31);assert(nr<2);rows[nr]=r->flags&AZ_ROW_DEFAULT_ON;return nr++;}
static az_surface *create(const char *id,const az_surface_v1 *s){(void)id;assert(ns<4&&s->user==(void*)(uintptr_t)ns);return (az_surface*)(uintptr_t)(++ns);}
static void show(az_surface *s,int on){shown[(uintptr_t)s-1]=on;}
static void place(az_surface *s,az_rect r){(void)s;assert(r.w>0&&r.h>0);}
static void redraw(az_surface *s){(void)s;}
static void fill(az_draw *d,int x,int y,int w,int h,uint32_t c){(void)d;(void)x;(void)y;(void)w;(void)h;(void)c;}
static void rect(az_draw *d,int x,int y,int w,int h,int t,uint32_t c){(void)d;(void)x;(void)y;(void)w;(void)h;(void)t;(void)c;}
static void text(az_draw *d,int x,int y,const char *s,int px,uint32_t c){(void)d;(void)x;(void)y;(void)s;(void)px;assert(ink_count<4);inks[ink_count++]=c;}
static int snap(int d,int *v,unsigned n){
 assert(n==12);if(d!=1)return 0;
 int data[12]={650,502,619,20,15,0,500,1000,250,0,1,0};
 data[4]=mask;data[9]=four;
 if(four){data[0]=0;data[1]=324;data[2]=144;data[3]=38;}
 memcpy(v,data,sizeof data);return 1;
}
static int toggle(int d,int k){assert(d==1&&k>=0&&k<4);mask^=1<<k;calls++;return 1;}
int main(void){
 az_core_v1 api={.abi=1,.size=sizeof api,.deck_count=count,.view=view,.deck_info=deck_info,.thread_start=start,.register_row=reg,.row_value=value,
 .touch_filter=regtouch,.surface_create=create,.surface_show=show,.surface_place=place,.surface_redraw=redraw,.fill_rect=fill,.draw_rect=rect,.draw_text=text};
 native_snapshot=snap;native_toggle=toggle;assert(!init(&api)&&ns==4);
 geometry_result=((uint64_t)1<<48)|((uint64_t)170<<24)|500;tick();assert(native_visible[1]&&!native_visible[0]);
 az_rect r=native_button_rect(native_values[1],0);az_event e={.kind=AZ_EV_TAP,.x=r.x+1,.y=r.y+1};
 native_input(&e,(void*)1);tick();assert(calls==1&&mask==14&&native_values[1][4]==14);
 native_paint(NULL,(az_rect){650,502,619,20},(void*)1);
 assert(inks[0]==0x404040&&inks[1]==stem_visual_ink(stem_colours[1],1,500));
 rows[dim_row]=0;tick();ink_count=0;native_paint(NULL,(az_rect){650,502,619,20},(void*)1);
 assert(inks[0]==0x404040&&inks[1]==stem_colours[1]);
 native_input(&e,(void*)1);assert(mask==15&&calls==2);
 e.x=r.x+r.w+1;native_input(&e,(void*)1);assert(calls==2); /* gap */
 four=1;native_input(&e,(void*)1);assert(calls==2); /* stale paint */
 geometry_result=((uint64_t)3<<48)|((uint64_t)212<<24)|560;tick();assert(native_visible[1]);
 geometry_result=((uint64_t)3<<48)|((uint64_t)40<<24)|560;tick();assert(!native_visible[1]);
 /* Same v51 vertical target through the entire mini waveform, y502..550.
  * Original label surfaces are unchanged; only y522..550 uses the tiny filter. */
 four=0;geometry_result=((uint64_t)1<<48)|((uint64_t)170<<24)|500;tick();
 int previous=calls;az_touch t={.slot=0,.x=660,.y=549,.down=1};
 assert(drive_touch(&t,NULL)==AZ_TOUCH_TAKE&&calls==previous); /* UI queue only */
 drive_touch(&t,NULL);tick();assert(calls==previous+1);tick();assert(calls==previous+1);
 t.down=0;assert(drive_touch(&t,NULL)==AZ_TOUCH_TAKE);
 t.down=1;t.y=522;assert(drive_touch(&t,NULL)==AZ_TOUCH_TAKE);t.down=0;drive_touch(&t,NULL);tick();assert(calls==previous+2);
 t.down=1;t.x=799;t.y=540;assert(drive_touch(&t,NULL)==AZ_TOUCH_PASS);t.down=0;drive_touch(&t,NULL);tick();assert(calls==previous+2); /* stem gap */
 t.down=1;t.x=660;t.y=550;assert(drive_touch(&t,NULL)==AZ_TOUCH_PASS);t.down=0;drive_touch(&t,NULL);
 t.down=1;t.y=501;assert(drive_touch(&t,NULL)==AZ_TOUCH_PASS);t.y=540;assert(drive_touch(&t,NULL)==AZ_TOUCH_PASS);t.down=0;drive_touch(&t,NULL);tick();assert(calls==previous+2); /* no drag-in */
 t.down=1;t.y=521;assert(drive_touch(&t,NULL)==AZ_TOUCH_PASS);t.down=0;drive_touch(&t,NULL); /* label handled only by surface */
 t.down=1;t.y=540;assert(drive_touch(&t,NULL)==AZ_TOUCH_TAKE);t.down=0;drive_touch(&t,NULL);
 four=1;geometry_result=((uint64_t)3<<48)|((uint64_t)212<<24)|560;tick();assert(calls==previous+2); /* stale request dropped */
 for(int d=0;d<4;d++)assert(!extra_bounds[d]);
 puts("PASS enlarged targets: mini-waveform bottom, gap below label, exactly one toggle, no draw/input duplication, no drag-in, inter-stem gaps and layout cancellation");
 second_tap=fake_second;four=0;geometry_result=((uint64_t)1<<48)|((uint64_t)170<<24)|500;tick();
 int before=calls;second_x=660;second_y=510;second_ready=1;tick();assert(calls==before+1);tick();assert(calls==before+1);
 second_y=549;second_ready=1;tick();assert(calls==before+2);
 second_x=799;second_ready=1;tick();assert(calls==before+2);
 second_x=660;second_y=510;second_ready=1;four=1;geometry_result=((uint64_t)3<<48)|((uint64_t)212<<24)|560;tick();assert(calls==before+2);
 puts("PASS secondary native UI: label plus enlarged target, exactly one toggle, no gap activation, stale geometry cancelled");
 rows[enable_row]=0;tick();for(int d=0;d<4;d++)assert(!shown[d]);
 puts("PASS native labels: 4-surface limit, pad synchronization, EQ colours, dim option, gaps, stale geometry, hidden info and disable");
}

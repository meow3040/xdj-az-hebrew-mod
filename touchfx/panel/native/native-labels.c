/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "azmod-mod.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef TOUCHFX_INTEGRATED
#define ID "mrdjjag-touchfx-test"
#else
#define ID "mrdjjag-stem-touch"
#endif
static const az_core_v1 *C;
#include "geometry_bridge.h"
#include "stem_visuals.h"
static int (*second_tap)(const char *,int *,unsigned);
static int active,geometry_ready,dim_row=-1,enable_row=-1;
static az_rect panel_area;
static const uint32_t stem_colours[4]={0x22e022,0xff3030,0xff33cc,0x4060ff};
static int inside(az_rect r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;}
static int native_touch_row=-1;
static int native_touch_enabled(void){return native_touch_row<0||C->row_value(native_touch_row)!=0;}
#include "native_stems.h"
#include "native_extra_touch.h"
static void second_tick(void){
 if(!second_tap)return;
 for(int n=0;n<16;n++){
  int point[2];if(second_tap(ID,point,2)!=1)break;
  if(!native_touch_enabled())continue;
  az_event event={.kind=AZ_EV_TAP,.x=point[0],.y=point[1]};
  for(int d=0;d<4;d++){
   int v[12];if(!native_visible[d]||!native_get(d,v)||memcmp(v,native_values[d],4*sizeof(int))||v[9]!=native_values[d][9])continue;
   int hit=0;for(int k=0;k<4;k++){
    az_rect r=native_button_rect(v,k);if(!v[9])r.h=550-r.y;
    if(inside(r,event.x,event.y)){native_toggle(d,k);hit=1;break;}
   }
   if(hit)break;
  }
 }
}
static void tick(void) {
    active=C->row_value(enable_row)!=0;
    __atomic_store_n(&geometry_requested,active,__ATOMIC_RELAXED);
    uint64_t measured=__atomic_load_n(&geometry_result,__ATOMIC_ACQUIRE);
    int mode=(int)(measured>>48)-1,left=(int)((measured>>24)&0xffffff),bottom=(int)(measured&0xffffff);
    int count=C->deck_count?C->deck_count():0;
    geometry_ready=measured&&mode>=0&&mode<=2&&((mode==2?4:2)==count)&&left>=24&&left<=512&&bottom>=500&&bottom<=620;
    panel_area.x=left;
    second_tick();
    native_tick();
    extra_tick();
}
static int init(const az_core_v1 *core) {
    if(!core||core->abi!=AZ_MOD_ABI||!AZ_HAS(core,surface_create)||!AZ_HAS(core,deck_info)||!AZ_HAS(core,thread_start))return 1;
    C=core;
#ifndef GEOMETRY_TEST
    if(!C->fw_tag||strcmp(C->fw_tag(),"ep147-9e9454e357389ece"))return 2;
    *(void **)(&second_tap)=dlsym(RTLD_DEFAULT,"mrdjjag_stem_second_tap");
    if(!second_tap)return 7;
    *(void **)(&native_snapshot)=dlsym(RTLD_DEFAULT,"mrdjjag_stem_ui_snapshot");
    *(void **)(&native_toggle)=dlsym(RTLD_DEFAULT,"mrdjjag_stem_ui_toggle");
#endif
    if(!native_snapshot||!native_toggle)return 3;
#ifndef TOUCHFX_INTEGRATED
    az_row_v1 row={.size=sizeof row,.kind=AZ_ROW_TOGGLE,.flags=AZ_ROW_DEFAULT_ON,
        .key="enabled",.label="Native STEMS touch",.help="Touch deck labels to toggle the same STEMS parts as the physical pads."};
    enable_row=C->register_row(ID,&row);
    row.key="dim-indicators";row.label="Dim STEMS colours with EQ";
    row.help="Muted parts stay grey. Enabled parts follow EQ; off keeps their full colours.";
    dim_row=C->register_row(ID,&row);
#endif
    if(enable_row<0||dim_row<0||native_init())return 4;
    if(!AZ_HAS(C,touch_filter)||!C->touch_filter||C->touch_filter(ID,extra_touch,NULL))return 6;
#ifndef GEOMETRY_TEST
    if(C->thread_start(ID,"native-geometry",geometry_worker,NULL))return 5;
#endif
    return 0;
}
#ifndef TOUCHFX_INTEGRATED
static const char *const FW[]={"ep147-9e9454e357389ece",NULL};
static const az_mod_v1 MOD={.abi=AZ_MOD_ABI,.size=sizeof MOD,.id=ID,.fw_tags=FW,.init=init,.tick=tick};
const az_mod_v1 *az_mod_v1_entry(void){return &MOD;}

#endif

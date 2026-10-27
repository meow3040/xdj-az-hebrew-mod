/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdlib.h>
#include "auto_geometry.h"
#undef assert
#define assert(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static unsigned char mem[12][0x440];static uintptr_t rootptr=0x100000;static int slot_reads,change_slot;
static uintptr_t addr(int i){return 0x100000+(uintptr_t)i*0x1000;}
static void u64(int i,int off,uint64_t v){memcpy(mem[i]+off,&v,8);}static void i32(int i,int off,int32_t v){memcpy(mem[i]+off,&v,4);}
static int reader(void *o,uintptr_t a,void *out,size_t n){(void)o;if(a==0x20000&&n==8){uintptr_t v=(change_slot&&slot_reads++)?0:rootptr;memcpy(out,&v,8);return 1;}for(int i=0;i<12;i++)if(a>=addr(i)&&a+n<=addr(i)+sizeof mem[i]){memcpy(out,mem[i]+(a-addr(i)),n);return 1;}return 0;}
static void node(int i,int parent,int x,int y,int w,int h,uint64_t vt){u64(i,0,vt);u64(i,0x18,parent<0?0:addr(parent));i32(i,0x20,x);i32(i,0x24,y);i32(i,0x28,w);i32(i,0x2c,h);i32(i,0xc0,2);}
static void link(int parent,int a,int b){u64(parent,0x40,addr(parent)+0x200);i32(parent,0x50,b<0?1:2);u64(parent,0x200,addr(a));if(b>=0)u64(parent,0x208,addr(b));}
static void setup(int four,int info,int scale){memset(mem,0,sizeof mem);slot_reads=change_slot=0;node(0,-1,0,0,1280*scale,800*scale,GEO_VIEW_VT);i32(0,0x3a8,four?2:0);if(four){node(1,0,(info?172:0)*scale,80*scale,40*scale,480*scale,0x900000);link(0,1,-1);}else if(info){node(1,0,0,80*scale,584*scale,400*scale,GEO_PANEL_VT);node(2,1,0,30*scale,170*scale,41*scale,0x900000);node(3,1,0,73*scale,170*scale,46*scale,0x900000);link(0,1,-1);link(1,2,3);}else{node(1,0,0,80*scale,40*scale,400*scale,0x900000);link(0,1,-1);}}
int main(void){struct geometry g;for(int scale=1;scale<=2;scale++)for(int four=0;four<2;four++)for(int info=0;info<2;info++){setup(four,info,scale);assert(geometry_measure(reader,0,0x20000,&g));assert(g.left==(info?(four?212:170):40));assert(g.bottom==(four?560:500));}
 for(int info=0;info<2;info++){setup(0,info,1);i32(0,0x3a8,1);assert(geometry_measure(reader,0,0x20000,&g));assert(g.mode==1&&g.left==(info?170:40)&&g.bottom==500);}
 setup(0,0,1);i32(0,0xc0,0);assert(!geometry_measure(reader,0,0x20000,&g));
 setup(0,0,1);i32(0,0x3a8,9);assert(!geometry_measure(reader,0,0x20000,&g));
 setup(0,0,1);u64(1,0x18,0);assert(!geometry_measure(reader,0,0x20000,&g));
 setup(0,0,1);u64(0,0x40,0x12340000);assert(!geometry_measure(reader,0,0x20000,&g));
 setup(0,0,1);change_slot=1;assert(!geometry_measure(reader,0,0x20000,&g));
 setup(0,0,1);i32(0,0x50,257);assert(!geometry_measure(reader,0,0x20000,&g));
 puts("PASS: all 4 measured layouts at 1x/2x scale; hidden/stale/unknown/malformed native data rejected.");return 0;}

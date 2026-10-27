/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "test-glass-mask-core.h"
static unsigned char bits[(1280/8)*800];
static int pixel(int w,int x,int y){return (bits[(size_t)y*((w+7)/8)+x/8]>>(x&7))&1;}
int main(void){
 int layouts[4][2]={{1110,446},{1240,446},{1068,506},{1240,506}};
 for(int k=0;k<4;k++){
  int w=layouts[k][0],h=layouts[k][1],x=0,y=80*h/446,rw=w-x,rh=336*h/446;
  stem_glass_mask(bits,w,h,x,y,rw,rh);
  int clear=0,runs=0;
  for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){
   int expected=!(xx>=x&&xx<x+rw&&yy>=y&&yy<y+rh&&(yy&1));
   assert(pixel(w,xx,yy)==expected);
   if(!expected){clear++;if(xx==x)runs++;}
  }
  assert(clear==runs*rw&&runs<=rh/2+1&&runs<200);
  assert(pixel(w,w/2,2)&&pixel(w,w/2,h-2)); /* controls solid */
 }
 puts("PASS right-edge mask: alternate scanlines transparent through last screen column, same height, solid header/rate controls, unchanged mask complexity");
}

#ifndef TOUCHFX_AXIS_H
#define TOUCHFX_AXIS_H
/* SPDX-License-Identifier: GPL-3.0-only */
/* Four tiny shared glyphs, prepared once. No surfaces, timers or input areas. */
#define AXIS_SIZE 20
static unsigned char axis_icons[4][AXIS_SIZE*AXIS_SIZE*4];
static int axis_icons_ready;
static void axis_dot(int icon,int x,int y){if(x<0||y<0||x>=AXIS_SIZE||y>=AXIS_SIZE)return;unsigned char*p=axis_icons[icon]+(y*AXIS_SIZE+x)*4;p[0]=p[1]=p[2]=255;p[3]=255;}
static void axis_line(int icon,int x,int y,int x2,int y2){int dx=x2-x;if(dx<0)dx=-dx;int sx=x<x2?1:-1,dy=y2-y;if(dy<0)dy=-dy;dy=-dy;int sy=y<y2?1:-1,err=dx+dy;for(;;){axis_dot(icon,x,y);axis_dot(icon,x,y+1);if(x==x2&&y==y2)break;int twice=2*err;if(twice>=dy){err+=dy;x+=sx;}if(twice<=dx){err+=dx;y+=sy;}}}
static void axis_prepare(void){if(axis_icons_ready)return;axis_icons_ready=1;static const int sine[16]={0,2,4,5,6,5,4,2,0,-2,-4,-5,-6,-5,-4,-2};
 for(int icon=0;icon<2;icon++)for(int x=2;x<17;x++){int a=(x-2)*16*(icon?3:1)/16,b=(x-1)*16*(icon?3:1)/16;axis_line(icon,x,9+sine[a&15],x+1,9+sine[b&15]);}
 for(int y=0;y<20;y++)for(int x=0;x<20;x++){int dx=2*x-19,dy=2*y-19,d=dx*dx+dy*dy;if(d>=96&&d<=196){axis_dot(2,x,y);axis_dot(3,x,y);}if(d<=16)axis_dot(2,x,y);}
}

static az_rect axis_marker_rect(az_rect f,int icon,uint64_t hole){
 int margin=6;
 if(hole){int hx=(int)(hole&2047),hy=(int)((hole>>11)&2047),hw=(int)((hole>>22)&2047),hh=(int)((hole>>33)&2047);int mid=f.y+(f.h-20)/2;
  if(hx<f.x+26&&hx+hw>f.x+6&&hy<mid+20&&hy+hh>mid)margin=hx+hw-f.x+6;
 }
 int x=icon==0?f.x+margin:icon==1?f.x+f.w-margin-20:f.x+(f.w-20)/2;
 int y=icon<2?f.y+(f.h-20)/2:icon==2?f.y+6:f.y+f.h-26;
 return (az_rect){x,y,20,20};
}
#endif

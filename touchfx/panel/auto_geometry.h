/* SPDX-License-Identifier: GPL-3.0-only */
/* Read-only native component geometry. Reader must copy safely, never dereference
 * firmware pointers. All addresses below are for the verified EP147 2.00 build. */
#ifndef AUTO_GEOMETRY_H
#define AUTO_GEOMETRY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define GEO_VIEW_VT UINT64_C(0x2d61ae0)
#define GEO_PANEL_VT UINT64_C(0x30af430)
#define GEO_MAX_NODES 512
struct geometry {int mode,left,bottom;};
typedef int (*geometry_reader)(void *,uintptr_t,void *,size_t);
static uint64_t geo_u64(const void *b,size_t o){uint64_t v;memcpy(&v,(const unsigned char*)b+o,8);return v;}
static int32_t geo_i32(const void *b,size_t o){int32_t v;memcpy(&v,(const unsigned char*)b+o,4);return v;}
static int geo_ptr(uintptr_t p){return p>=0x10000 && p<=UINT64_C(0x0000fffffffff000) && !(p&7);}
static int geometry_measure(geometry_reader rd,void *opaque,uintptr_t slot,struct geometry *out) {
    struct node {uintptr_t ptr,parent;int x,y,parent_index;unsigned depth;unsigned char b[200];};
    struct node nodes[GEO_MAX_NODES];unsigned char root[0x440];uintptr_t view=0;
    if(!rd(opaque,slot,&view,8)||!geo_ptr(view)||!rd(opaque,view,root,sizeof root)||geo_u64(root,0)!=GEO_VIEW_VT||!(geo_i32(root,0xc0)&2))return 0;
    int mode=geo_i32(root,0x3a8),vw=geo_i32(root,0x28),vh=geo_i32(root,0x2c);
    if(mode<0||mode>2||vw<640||vw>8192||vh<400||vh>8192)return 0;
    uintptr_t ancestor=geo_u64(root,0x18);unsigned depth=0;
    while(ancestor && depth++<32){unsigned char b[200];if(!geo_ptr(ancestor)||!rd(opaque,ancestor,b,sizeof b)||!(geo_i32(b,0xc0)&2))return 0;ancestor=geo_u64(b,0x18);}
    if(ancestor)return 0;
    nodes[0].ptr=view;nodes[0].parent=0;nodes[0].parent_index=-1;nodes[0].x=nodes[0].y=0;nodes[0].depth=0;memcpy(nodes[0].b,root,200);
    unsigned used=1;int edge=0,info=0,rows=0,bottom=0;
    for(unsigned at=0;at<used;at++) {
        struct node *q=&nodes[at];unsigned char *b=q->b;
        int w=geo_i32(b,0x28),h=geo_i32(b,0x2c);
        if(w<0||w>8192||h<0||h>8192)continue;
        int x=(int)((int64_t)q->x*1280/vw),y=(int)((int64_t)q->y*800/vh);
        w=(int)((int64_t)w*1280/vw);h=(int)((int64_t)h*800/vh);
        int isinfo=mode!=2&&geo_u64(b,0)==GEO_PANEL_VT&&w>80&&w<=512;
        if(x>=0&&x+w<=512&&((w>=24&&w<=80)||isinfo)&&h>=60&&y>=54&&y+h<=620){
            if(x+w>edge)edge=x+w;if(y+h>bottom)bottom=y+h;if(isinfo&&x+w>info)info=x+w;
        }
        if(mode!=2&&q->parent_index>=0&&geo_u64(nodes[q->parent_index].b,0)==GEO_PANEL_VT&&geo_i32(b,0x20)==0&&w>=80&&w<=256&&h>=24&&h<=64&&x+w>=24&&x+w<=512&&x+w>rows)rows=x+w;
        if(q->depth>=12)continue;
        int n=geo_i32(b,0x50);uintptr_t kids=geo_u64(b,0x40);uintptr_t list[256];
        if(n<=0||n>256||!geo_ptr(kids)||!rd(opaque,kids,list,(size_t)n*8))continue;
        for(int i=0;i<n;i++) {
            if(used>=GEO_MAX_NODES)return 0; /* ambiguous/truncated hierarchy: do not guess */
            uintptr_t child=list[i];unsigned char cb[200];
            if(!geo_ptr(child)||!rd(opaque,child,cb,sizeof cb)||geo_u64(cb,0x18)!=q->ptr||!(geo_i32(cb,0xc0)&2))continue;
            int lx=geo_i32(cb,0x20),ly=geo_i32(cb,0x24);if(lx<-8192||lx>8192||ly<-8192||ly>8192)continue;
            struct node *c=&nodes[used++];c->ptr=child;c->parent=q->ptr;c->parent_index=(int)at;c->depth=q->depth+1;c->x=q->x+lx;c->y=q->y+ly;memcpy(c->b,cb,200);
        }
    }
    if(info)edge=info;if(rows)edge=rows;
    if(edge<24||edge>512||(mode==2&&(bottom<500||bottom>620)))return 0;
    uintptr_t check=0;unsigned char again[0x440];
    if(!rd(opaque,slot,&check,8)||check!=view||!rd(opaque,view,again,sizeof again)||geo_u64(again,0)!=GEO_VIEW_VT||geo_i32(again,0x3a8)!=mode||!(geo_i32(again,0xc0)&2)||geo_i32(again,0x28)!=vw||geo_i32(again,0x2c)!=vh)return 0;
    *out=(struct geometry){mode,edge,mode==2?bottom:500};return 1;
}
#endif

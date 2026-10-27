/* SPDX-License-Identifier: GPL-3.0-only */
/* Fixed, atomically published screen rectangle. No mod code runs in read(). */
static uint64_t sdk_guard_pack(az_rect r){
    if(r.x<0||r.y<0||r.w<=0||r.h<=0||(int64_t)r.x+r.w>1280||(int64_t)r.y+r.h>800)return 0;
    return (UINT64_C(1)<<63)|(unsigned)r.x|((uint64_t)(unsigned)r.y<<11)|((uint64_t)(unsigned)r.w<<22)|((uint64_t)(unsigned)r.h<<33);
}
static int sdk_guard_contains(uint64_t b,const az_touch *t){
    if(!(b>>63)||t->slot!=0||!t->down)return 0;
    int x=(int)(b&2047),y=(int)((b>>11)&2047),w=(int)((b>>22)&2047),h=(int)((b>>33)&2047);
    return t->x>=x&&t->x<x+w&&t->y>=y&&t->y<y+h;
}

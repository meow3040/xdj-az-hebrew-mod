/* SPDX-License-Identifier: GPL-3.0-only */
/* Exact 1280x800 native geometry, already guarded by core hash + snapshot.
 * Existing SDK surfaces keep drawing/hit-testing the labels. This constant-time
 * bus filter handles only the gap BELOW them and the mini-waveform row, ending
 * at y550, like v51. It never calls core services, draws, reads files or sends
 * audio; the UI thread alone validates and forwards requests to the pad queue. */
static uint64_t extra_bounds[4],extra_pending[4];
static unsigned extra_epoch[4];
static int extra_contact_taken;
static uint64_t extra_pack(az_rect r){
    return (uint64_t)(unsigned)r.x|((uint64_t)(unsigned)r.y<<11)|
           ((uint64_t)(unsigned)r.w<<22)|((uint64_t)(unsigned)r.h<<33);
}
static int extra_touch(const az_touch *t,void *u){
    (void)u;if(t->slot!=0)return AZ_TOUCH_PASS;
    if(!t->down){int took=extra_contact_taken;extra_contact_taken=0;return took?AZ_TOUCH_TAKE:AZ_TOUCH_PASS;}
    /* SDK offers passed contacts only once; owners alone receive motion/up.
     * Never latch PASS locally: that contact's release is not sent to us. */
    if(extra_contact_taken)return AZ_TOUCH_TAKE;
    for(int d=0;d<4;d++){
        uint64_t b=__atomic_load_n(&extra_bounds[d],__ATOMIC_ACQUIRE);
        if(!b)continue;
        int x=(int)(b&2047),y=(int)((b>>11)&2047),w=(int)((b>>22)&2047),h=(int)((b>>33)&2047);
        if(t->x<x||t->x>=x+w||t->y<y||t->y>=y+h)continue;
        int bw=(w-27)/4,relative=t->x-x,k=relative/(bw+9);
        if(k>=4||relative%(bw+9)>=bw)continue; /* retain inter-stem gaps */
        __atomic_store_n(&extra_pending[d],((b>>44)<<32)|(1u<<k),__ATOMIC_RELEASE);
        extra_contact_taken=1;return AZ_TOUCH_TAKE;
    }
    return AZ_TOUCH_PASS;
}
static void extra_tick(void){
    const uint64_t rectangle_mask=(1ull<<44)-1;
    for(int d=0;d<4;d++){
        int v[12];uint64_t rectangle=0;
        if(native_touch_enabled()&&native_visible[d]&&native_get(d,v)&&!v[9]){
            int top=v[1]+v[3],bottom=530+v[3];
            if(top<bottom&&bottom<=550)
                rectangle=extra_pack((az_rect){v[0],top,v[2],bottom-top});
        }
        uint64_t old=__atomic_load_n(&extra_bounds[d],__ATOMIC_RELAXED);
        if((old&rectangle_mask)!=rectangle){
            extra_epoch[d]=(extra_epoch[d]+1)&0xfffffu;if(!extra_epoch[d])extra_epoch[d]=1;
            __atomic_store_n(&extra_bounds[d],rectangle?rectangle|((uint64_t)extra_epoch[d]<<44):0,__ATOMIC_RELEASE);
        }
        uint64_t request=__atomic_exchange_n(&extra_pending[d],0,__ATOMIC_ACQ_REL);
        if(!rectangle||(unsigned)(request>>32)!=extra_epoch[d])continue;
        for(int k=0;k<4;k++)if(request&(1u<<k))native_toggle(d,k);
    }
}

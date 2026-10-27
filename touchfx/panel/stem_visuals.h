/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_STEM_VISUALS_H
#define AZMOD_STEM_VISUALS_H
#include <stdint.h>
static inline int stem_visual_level(int on,int gain) {
    if(!on)return 0;
    return gain<0||gain>=1000?1000:gain;
}
static inline uint32_t stem_visual_blend(uint32_t fg,uint32_t bg,int gain) {
    uint32_t out=0;
    for(int shift=0;shift<=16;shift+=8)
        out|=(uint32_t)((((fg>>shift)&255)*gain+((bg>>shift)&255)*(1000-gain)+500)/1000)<<shift;
    return out;
}
static inline uint32_t stem_visual_ink(uint32_t colour,int on,int gain) {
    return on?stem_visual_blend(colour,0x232323,stem_visual_level(on,gain)):0x404040;
}
static inline uint32_t stem_visual_field(int x,int width,const int levels[4],int highlighted) {
    static const uint32_t cols[4]={0x22e022,0xff3030,0xff33cc,0x4060ff};
    int pos=x*4000/width,field=pos/1000;if(field>3)field=3;
    int fraction=pos-field*1000;
    uint32_t left=stem_visual_blend(cols[field],0x191919,levels[field]*120/1000),right=left;
    int mix=0;
    if(field>0&&fraction<120) {
        left=stem_visual_blend(cols[field-1],0x191919,levels[field-1]*120/1000);
        mix=500+fraction*500/120;
    } else if(field<3&&fraction>880) {
        right=stem_visual_blend(cols[field+1],0x191919,levels[field+1]*120/1000);
        mix=(fraction-880)*500/120;
    }
    uint32_t colour=stem_visual_blend(right,left,mix);
    return highlighted?colour+0x070707:colour;
}
#endif

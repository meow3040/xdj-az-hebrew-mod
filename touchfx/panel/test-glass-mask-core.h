/* SPDX-License-Identifier: GPL-3.0-only */
/* 1-bit genomsikt, as used by the existing menu fade. Header stays solid.
 * Alternate scanlines keep the X region to ~170 runs rather than hundreds of
 * thousands of checkerboard fragments. No per-frame capture/compositing. */
static void stem_glass_mask(unsigned char *bits,int w,int h,int x,int y,int rw,int rh){
    size_t stride=((size_t)w+7)/8;
    memset(bits,255,stride*(size_t)h);
    for(int yy=y;yy<y+rh;yy++)for(int xx=x;xx<x+rw;xx++)
        if(yy&1)bits[(size_t)yy*stride+(unsigned)xx/8]&=(unsigned char)~(1u<<(xx&7));
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* At most65 sorted scanline runs; matches the fully opaque circle interior. */
static int stem_glass_head_runs(int cx,int cy,int radius,az_rect clip,az_rect *runs,int cap){
    if(!runs||cap<=0||radius<1||radius>32||clip.w<=0||clip.h<=0)return 0;
    int n=0;
    for(int dy=-radius;dy<=radius&&n<cap;dy++){
        int y=cy+dy;if(y<clip.y||y>=clip.y+clip.h)continue;
        int dx=radius,remain=radius*radius-dy*dy;while(dx*dx>remain)dx--;
        int left=cx-dx,right=cx+dx+1;
        if(left<clip.x)left=clip.x;
        if(right>clip.x+clip.w)right=clip.x+clip.w;
        if(right>left)runs[n++]=(az_rect){left,y,right-left,1};
    }
    return n;
}

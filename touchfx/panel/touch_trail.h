/* SPDX-License-Identifier: GPL-3.0-only */
/* UI only: sixteen fixed samples, no worker, heap or audio calls. */
#define TRAIL_N 16
static struct {int x,y;long at;} trail_points[TRAIL_N];
static int trail_count;
static long trail_redraw;
static void trail_reset(void){trail_count=0;}
static void trail_update(long now,int x,int y,int down){
    int drop=0;while(drop<trail_count&&now-trail_points[drop].at>=320)drop++;
    if(drop){memmove(trail_points,trail_points+drop,(trail_count-drop)*sizeof *trail_points);trail_count-=drop;}
    if(!down)return;
    if(trail_count&&now-trail_points[trail_count-1].at<20)return;
    if(trail_count&&trail_points[trail_count-1].x==x&&trail_points[trail_count-1].y==y)return;
    if(trail_count==TRAIL_N){memmove(trail_points,trail_points+1,(TRAIL_N-1)*sizeof *trail_points);trail_count--;}
    trail_points[trail_count].x=x;trail_points[trail_count].y=y;trail_points[trail_count++].at=now;
}
static int trail_pulse(long now,double bpm,double phase,double pos,int drum,int rate){
    static const double stems[10]={0.0625,0.125,0.25,0.5,1,2,0,0.75,2.0/3.0,4};
    static const double drums[6]={1,0.5,0.25,0.125,0.0625,0.03125};
    if(!(bpm>=30&&bpm<=300)||rate<0||(drum?rate>5:(rate>9||rate==6)))return 220;
    double beats=(double)now*bpm/60000.0;
    if(phase>=0&&phase<1){
        /* Sub-beat values follow the SDK grid phase. For two beats the SDK
         * exposes no bar counter; play position supplies the alternating beat. */
        beats=phase;
        if(!drum&&(rate==5||rate==7||rate==8||rate==9)&&pos>=0&&pos<100000){long whole=(long)(pos*bpm/60.0);beats+=rate==9?whole%4:rate==7?whole%3:(whole&1);}
    }
    double cycles=beats/(drum?drums[rate]:stems[rate]);
    double frac=cycles-(long long)cycles;
    return 150+(int)(105*(1-frac)*(1-frac));
}
/* SDK draw_image consumes premultiplied RGBA, not straight RGBA. */
static void trail_rgba(unsigned char *p,uint32_t ink,unsigned alpha){
    p[0]=(unsigned char)((((ink>>16)&255)*alpha+127)/255);
    p[1]=(unsigned char)((((ink>>8)&255)*alpha+127)/255);
    p[2]=(unsigned char)(((ink&255)*alpha+127)/255);p[3]=(unsigned char)alpha;
}
static void trail_dot(az_draw *draw,az_rect f,int x,int y,int radius,int alpha,uint32_t ink){
    unsigned char rgba[39*39*4];int size=radius*2+1;
    if(x-radius<f.x||y-radius<f.y||x+radius>=f.x+f.w||y+radius>=f.y+f.h){
        /* Clip the sprite, keeping edge touches inside the original field. */
    }
    for(int yy=0;yy<size;yy++)for(int xx=0;xx<size;xx++){
        int dx=xx-radius,dy=yy-radius,d2=dx*dx+dy*dy;
        int a=d2<=radius*radius?alpha:d2<=(radius+1)*(radius+1)?alpha/4:0;
        int sx=x+dx,sy=y+dy;if(sx<f.x||sx>=f.x+f.w||sy<f.y||sy>=f.y+f.h)a=0;
        int p=(yy*size+xx)*4;trail_rgba(rgba+p,ink,(unsigned)a);
    }
    C->draw_image(draw,x-radius,y-radius,rgba,size,size);
}
/* Composite each screen pixel once. Max alpha retains a single stroke's
 * opacity at crossings, rather than repeatedly blending overlapping circles.
 * Fixed tiles avoid a screen-size buffer, heap or new worker. */
struct trail_stamp {int x,y,r,a;};
static void trail_composite(az_draw *draw,az_rect f,const struct trail_stamp *stamps,int count,uint32_t ink){
    if(!count)return;
    int left=f.x+f.w,top=f.y+f.h,right=f.x,bottom=f.y;
    for(int i=0;i<count;i++){
        int r=stamps[i].r+1;
        if(stamps[i].x-r<left)left=stamps[i].x-r;if(stamps[i].x+r+1>right)right=stamps[i].x+r+1;
        if(stamps[i].y-r<top)top=stamps[i].y-r;if(stamps[i].y+r+1>bottom)bottom=stamps[i].y+r+1;
    }
    if(left<f.x)left=f.x;if(top<f.y)top=f.y;if(right>f.x+f.w)right=f.x+f.w;if(bottom>f.y+f.h)bottom=f.y+f.h;
    unsigned char alpha[32*32],rgba[32*32*4];
    for(int ty=top;ty<bottom;ty+=32)for(int tx=left;tx<right;tx+=32){
        int w=right-tx<32?right-tx:32,h=bottom-ty<32?bottom-ty:32,any=0;
        memset(alpha,0,sizeof alpha);
        for(int i=0;i<count;i++){
            const struct trail_stamp *s=&stamps[i];int radius=s->r+1;
            int x0=s->x-radius-tx,x1=s->x+radius+1-tx,y0=s->y-radius-ty,y1=s->y+radius+1-ty;
            if(x0<0)x0=0;if(y0<0)y0=0;if(x1>w)x1=w;if(y1>h)y1=h;
            for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){
                int dx=tx+x-s->x,dy=ty+y-s->y,d2=dx*dx+dy*dy;
                int a=d2<=s->r*s->r?s->a:d2<=radius*radius?s->a/4:0;
                if(a>alpha[y*w+x]){alpha[y*w+x]=(unsigned char)a;any=1;}
            }
        }
        if(!any)continue;
        for(int i=0;i<w*h;i++){trail_rgba(rgba+i*4,ink,alpha[i]);}
        C->draw_image(draw,tx,ty,rgba,w,h);
    }
}
static void trail_paint(az_draw *draw,az_rect f,int nx,int ny,long now,int pulse,int tail,uint32_t ink,int large){
    struct trail_stamp stamps[TRAIL_N*12];int count=0;
    for(int i=0;tail&&i<trail_count;i++){
        int age=(int)(now-trail_points[i].at);if(age<0||age>=320)continue;
        int x=f.x+trail_points[i].x*(f.w-1)/1000,y=f.y+trail_points[i].y*(f.h-1)/1000;
        int x2=i+1<trail_count?f.x+trail_points[i+1].x*(f.w-1)/1000:f.x+nx*(f.w-1)/1000;
        int y2=i+1<trail_count?f.y+trail_points[i+1].y*(f.h-1)/1000:f.y+ny*(f.h-1)/1000;
        int dx=x2-x,dy=y2-y,n=(dx<0?-dx:dx)+(dy<0?-dy:dy);n=n/3+1;if(n>12)n=12;
        int alpha=pulse*(320-age)/400,r=large?19:1+(320-age)*2/320;
        for(int k=0;k<n;k++)stamps[count++]=(struct trail_stamp){x+dx*k/n,y+dy*k/n,r,alpha};
    }
    trail_composite(draw,f,stamps,count,ink);
    trail_dot(draw,f,f.x+nx*(f.w-1)/1000,f.y+ny*(f.h-1)/1000,19,255,ink);
}

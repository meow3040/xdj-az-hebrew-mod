/* SPDX-License-Identifier: GPL-3.0-only */
#include "stem_glass_mask.h"
#include "sdk_guard_rect.h"
#include "stem_glass_head.h"
#include "touchfx_axis.h"
#include "touchfx_info_geometry.h"
#define MRDJJAG_GLASS_IMPL 1
static int sdk_glass_input_ready(struct az_surface *p);
static void sdk_glass_guard_publish(struct az_surface *p,int visible);
int mrdjjag_surface_glass(const char *id,az_surface *surface,int on,az_rect field);
int mrdjjag_surface_glass(const char *id,az_surface *surface,int on,az_rect field){
    cm_mod *owner=sdk_self(id,CM_RA(),"stem glass",1);
    if(!owner)return -1;
    struct az_surface *p=sdk_surf_of(surface,"stem glass");
    if(!p||p->mod!=(int)(owner-g_cm))return -1;
    on=on!=0;
    if(on&&(field.w<=0||field.h<=0||field.x<p->r.x||field.y<p->r.y||
       (int64_t)field.x+field.w>(int64_t)p->r.x+p->r.w||
       (int64_t)field.y+field.h>(int64_t)p->r.y+p->r.h))return -1;
    if(on!=p->glass_on||(on&&memcmp(&field,&p->glass_field,sizeof field))){
        p->glass_on=on;p->glass_field=field;p->glass_dirty=1;
    }
    return 0;
}
/* Opaque head only; the fixed core guard still protects the whole field. */
int mrdjjag_surface_glass_head(const char *id,az_surface *surface,int on,az_rect head);
int mrdjjag_surface_glass_head(const char *id,az_surface *surface,int on,az_rect head){
    cm_mod *owner=sdk_self(id,CM_RA(),"glass head",1);
    if(!owner||strcmp(owner->id,"mrdjjag-touchfx-test"))return -1;
    struct az_surface *p=sdk_surf_of(surface,"glass head");
    if(!p||p->mod!=(int)(owner-g_cm)||p->d.layer!=AZ_LAYER_TOP||p->d.views!=AZ_VIEW_BIT(AZ_VIEW_DECK))return -1;
    on=on!=0;
    if(on){
        int64_t cx=(int64_t)head.x+19,cy=(int64_t)head.y+19;
        if(head.w!=39||head.h!=39||cx<p->glass_field.x||cy<p->glass_field.y||
           cx>=(int64_t)p->glass_field.x+p->glass_field.w||cy>=(int64_t)p->glass_field.y+p->glass_field.h)return -1;
    }
    if(on!=p->glass_head_on||(on&&memcmp(&head,&p->glass_head,sizeof head))){p->glass_head_on=on;p->glass_head=head;p->glass_head_dirty=1;}
    return 0;
}
int mrdjjag_surface_glass_marks(const char *id,az_surface *surface,int on,az_rect field);
int mrdjjag_surface_glass_marks(const char *id,az_surface *surface,int on,az_rect field){
 cm_mod*owner=sdk_self(id,CM_RA(),"glass marks",1);if(!owner||strcmp(owner->id,"mrdjjag-touchfx-test"))return -1;
 struct az_surface*p=sdk_surf_of(surface,"glass marks");if(!p||p->mod!=(int)(owner-g_cm)||p->d.layer!=AZ_LAYER_TOP||p->d.views!=AZ_VIEW_BIT(AZ_VIEW_DECK))return -1;
 on=on!=0;if(on&&(field.w<80||field.h<60||field.x<p->r.x||field.y<p->r.y||(int64_t)field.x+field.w>(int64_t)p->r.x+p->r.w||(int64_t)field.y+field.h>(int64_t)p->r.y+p->r.h))return -1;
 if(on!=p->glass_marks_on||(on&&memcmp(&field,&p->glass_marks,sizeof field))){p->glass_marks_on=on;p->glass_marks=field;p->glass_dirty=1;}return 0;
}
static int sdk_info_read(void*u,uintptr_t address,void*out,size_t n){(void)u;if(!info_ptr(address))return 0;struct iovec l={out,n},r={(void*)address,n};return process_vm_readv(getpid(),&l,1,&r,1,0)==(ssize_t)n;}
uint64_t mrdjjag_surface_info_passthrough(const char *id,az_surface *surface,int on);
uint64_t mrdjjag_surface_info_passthrough(const char *id,az_surface *surface,int on){
 static struct info_cache cache;cm_mod*owner=sdk_self(id,CM_RA(),"info passthrough",1);if(!owner||strcmp(owner->id,"mrdjjag-touchfx-test"))return 0;
 struct az_surface*p=sdk_surf_of(surface,"info passthrough");if(!p||p->mod!=(int)(owner-g_cm)||p->d.layer!=AZ_LAYER_TOP||p->d.views!=AZ_VIEW_BIT(AZ_VIEW_DECK))return 0;
 az_rect a={0};uint64_t next=0;if(on&&info_geometry(sdk_info_read,NULL,__atomic_load_n(&g_pl_view,__ATOMIC_RELAXED),&cache,&a)&&a.x>=p->r.x&&a.y>=p->r.y&&a.x+a.w<=p->r.x+p->r.w&&a.y+a.h<=p->r.y+p->r.h)next=sdk_guard_pack(a);
 if(__atomic_load_n(&p->info_hole,__ATOMIC_RELAXED)!=next){__atomic_store_n(&p->info_hole,next,__ATOMIC_RELEASE);p->glass_dirty=1;}return next;
}
/* The native46x59hit widget includes a10px black margin around its image.
 * Keep hit geometry unchanged; reveal only the inset image, cover its margin. */
static az_rect sdk_info_image(uint64_t hole){
 az_rect r={(int)(hole&2047),(int)((hole>>11)&2047),(int)((hole>>22)&2047),(int)((hole>>33)&2047)};
 int ix=r.w*10/46,iy=r.h*10/59;r.x+=ix;r.y+=iy;r.w-=2*ix;r.h-=2*iy;/* Cover the remaining lower native border (3 logical pixels). */r.h-=r.h*3/39;return r;
}
static void sdk_stem_glass_apply(struct overlay_ctx *c,struct az_surface *p){
 int effective=p->glass_on&&sdk_glass_input_ready(p);uint64_t hole=__atomic_load_n(&p->info_hole,__ATOMIC_ACQUIRE);
 if(!effective&&!hole){if(p->glass_mask){XShapeCombineMask(c->dpy,p->w.id,ShapeBounding,0,0,None,ShapeSet);XFreePixmap(c->dpy,p->glass_mask);p->glass_mask=0;}p->glass_dirty=p->glass_on;p->glass_head_dirty=1;return;}
 if(p->glass_dirty||p->glass_w!=p->w.w||p->glass_h!=p->w.h){
  int w=p->w.w,h=p->w.h,x=p->glass_field.x-p->r.x,y=p->glass_field.y-p->r.y,rw=p->glass_field.w,rh=p->glass_field.h;
  if(w<=0||h<=0||(effective&&(x<0||y<0||x+rw>w||y+rh>h)))return;
  size_t stride=((size_t)w+7)/8,size=stride*(size_t)h;unsigned char*bits=malloc(size);if(!bits)return;
  if(effective)stem_glass_mask(bits,w,h,x,y,rw,rh);else memset(bits,255,size);
  if(effective&&p->glass_marks_on){axis_prepare();for(int icon=0;icon<4;icon++){az_rect a=axis_marker_rect(p->glass_marks,icon,hole);for(int yy=0;yy<20;yy++)for(int xx=0;xx<20;xx++){int px=a.x+xx-p->r.x,py=a.y+yy-p->r.y;if(px>=0&&px<w&&py>=0&&py<h&&axis_icons[icon][(yy*20+xx)*4+3])bits[(size_t)py*stride+(unsigned)px/8]|=1u<<(px&7);}}}
  if(hole){int hx=(int)(hole&2047)-p->r.x,hy=(int)((hole>>11)&2047)-p->r.y,hw=(int)((hole>>22)&2047),hh=(int)((hole>>33)&2047);for(int yy=hy;yy<hy+hh;yy++)for(int xx=hx;xx<hx+hw;xx++)if(xx>=0&&xx<w&&yy>=0&&yy<h){az_rect image=sdk_info_image(hole);int inside=xx+p->r.x>=image.x&&xx+p->r.x<image.x+image.w&&yy+p->r.y>=image.y&&yy+p->r.y<image.y+image.h;unsigned char bit=1u<<(xx&7);if(inside)bits[(size_t)yy*stride+(unsigned)xx/8]&=(unsigned char)~bit;else bits[(size_t)yy*stride+(unsigned)xx/8]|=bit;}}
  Pixmap mask=XCreateBitmapFromData(c->dpy,p->w.id,(const char*)bits,w,h);free(bits);if(!mask)return;if(p->glass_mask)XFreePixmap(c->dpy,p->glass_mask);p->glass_mask=mask;p->glass_w=w;p->glass_h=h;p->glass_dirty=0;p->glass_head_dirty=1;
 }
 if(!p->glass_mask||!p->glass_head_dirty)return;
 XShapeCombineMask(c->dpy,p->w.id,ShapeBounding,0,0,p->glass_mask,ShapeSet);
 if(effective&&p->glass_head_on){az_rect clip={p->glass_field.x-p->r.x,p->glass_field.y-p->r.y,p->glass_field.w,p->glass_field.h},runs[65];int n=stem_glass_head_runs(p->glass_head.x+19-p->r.x,p->glass_head.y+19-p->r.y,19,clip,runs,65);XRectangle rects[65];for(int i=0;i<n;i++)rects[i]=(XRectangle){(short)runs[i].x,(short)runs[i].y,(unsigned short)runs[i].w,1};if(n)XShapeCombineRectangles(c->dpy,p->w.id,ShapeBounding,0,0,rects,n,ShapeUnion,YXBanded);}
 if(effective&&p->glass_head_on&&hole){az_rect image=sdk_info_image(hole);XRectangle cut={(short)(image.x-p->r.x),(short)(image.y-p->r.y),(unsigned short)image.w,(unsigned short)image.h};XShapeCombineRectangles(c->dpy,p->w.id,ShapeBounding,0,0,&cut,1,ShapeSubtract,YXBanded);}
 p->glass_head_dirty=0;
}

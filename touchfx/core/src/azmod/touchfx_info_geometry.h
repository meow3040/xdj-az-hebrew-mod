/* Bounded read-only native control lookup, EP1472.00. Missing/stale => no hole. */
#ifndef TOUCHFX_INFO_GEOMETRY_H
#define TOUCHFX_INFO_GEOMETRY_H
struct info_cache{uintptr_t root,field,button;};
typedef int(*info_reader)(void*,uintptr_t,void*,size_t);
static uint64_t info_u64(const unsigned char*b,int off){uint64_t v;memcpy(&v,b+off,8);return v;}
static int info_i32(const unsigned char*b,int off){int v;memcpy(&v,b+off,4);return v;}
static int info_ptr(uintptr_t p){return p>=0x10000&&p<UINT64_C(0x0000fffffffff000)&&!(p&7);}
static int info_list(info_reader read,void*u,const unsigned char*p,uintptr_t*list){int n=info_i32(p,0x50);uintptr_t q=info_u64(p,0x40);if(n<1||n>32||!info_ptr(q)||!read(u,q,list,(size_t)n*8))return 0;return n;}
static int info_member(info_reader read,void*u,const unsigned char*p,uintptr_t target){uintptr_t list[32];int n=info_list(read,u,p,list);for(int i=0;i<n;i++)if(list[i]==target)return 1;return 0;}
static int info_bounds(const unsigned char*r,const unsigned char*f,const unsigned char*b,uintptr_t root,uintptr_t field,az_rect*out){
 if(info_u64(r,0)!=0x2d61ae0||info_u64(f,0)!=0x30bca40||info_u64(b,0)!=0x2cd9780||info_u64(f,0x18)!=root||info_u64(b,0x18)!=field)return 0;
 if(!(info_i32(r,0xc0)&2)||!(info_i32(f,0xc0)&2)||!(info_i32(b,0xc0)&2))return 0;
 int mode=info_i32(r,0x3a8),vw=info_i32(r,0x28),vh=info_i32(r,0x2c);if(mode<0||mode>2||vw<640||vw>8192||vh<400||vh>8192)return 0;
 int lx=info_i32(f,0x20),ly=info_i32(f,0x24),bx=info_i32(b,0x20),by=info_i32(b,0x24),bw=info_i32(b,0x28),bh=info_i32(b,0x2c);
 if(lx<0||lx>512||ly<0||ly>620||bx<0||bx>512||by<0||by>620||bw<1||bw>128||bh<1||bh>128)return 0;
 az_rect a={(lx+bx)*1280/vw,(ly+by)*800/vh,bw*1280/vw,bh*800/vh};
 if(a.x<24||a.x+a.w>512||a.y<54||a.y+a.h>620||a.w<24||a.w>80||a.h<24||a.h>96)return 0;
 *out=a;return 1;
}
static int info_geometry(info_reader read,void*u,uintptr_t root,struct info_cache*cache,az_rect*out){
 unsigned char r[0x440],f[200],b[200];if(!info_ptr(root)||!read(u,root,r,sizeof r)||info_u64(r,0)!=0x2d61ae0){memset(cache,0,sizeof *cache);return 0;}
 if(cache->root==root&&info_ptr(cache->field)&&info_ptr(cache->button)&&read(u,cache->field,f,sizeof f)&&read(u,cache->button,b,sizeof b)&&info_bounds(r,f,b,root,cache->field,out)&&info_member(read,u,r,cache->field)&&info_member(read,u,f,cache->button))return 1;
 memset(cache,0,sizeof *cache);uintptr_t fields[32];int nf=info_list(read,u,r,fields),count=0;struct info_cache found={0};az_rect answer={0};
 for(int i=0;i<nf;i++){if(!info_ptr(fields[i])||!read(u,fields[i],f,sizeof f)||info_u64(f,0)!=0x30bca40)continue;uintptr_t kids[32];int nb=info_list(read,u,f,kids);
  for(int j=0;j<nb;j++){az_rect a;if(!info_ptr(kids[j])||!read(u,kids[j],b,sizeof b)||!info_bounds(r,f,b,root,fields[i],&a))continue;count++;found=(struct info_cache){root,fields[i],kids[j]};answer=a;}}
 if(count!=1)return 0;
 *cache=found;*out=answer;return 1;
}
#endif

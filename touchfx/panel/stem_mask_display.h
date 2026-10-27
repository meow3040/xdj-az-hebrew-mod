/* SPDX-License-Identifier: GPL-3.0-only */
/* Read-only visual state. UI bridge first; cached control-file value for a
 * channel whose native labels are hidden. File reads stay on geometry worker. */
static unsigned stem_mask_request;
static uint64_t stem_mask_cache[4];
static int (*stem_mask_snapshot)(int,int *,unsigned);
static long now_ms(void);
static int stem_mask_parse(const char *s,size_t n,unsigned *out){
    size_t i=0;unsigned value=0;
    if(!n||s[0]<'0'||s[0]>'9')return 0;
    while(i<n&&s[i]>='0'&&s[i]<='9'){
        value=value*10+(unsigned)(s[i++]-'0');if(value>255)return 0;
    }
    while(i<n&&(s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n'))i++;
    if(i!=n)return 0;
    *out=value&0x80u?value&15u:0;return 1;
}
static int stem_mask_value(int d){
    if(d<0||d>=4)return -1;
    int v[12];
    if(stem_mask_snapshot&&stem_mask_snapshot(d,v,12)&&v[10]==1&&v[4]>=0&&v[4]<=15)return v[4];
    uint64_t cached=__atomic_load_n(&stem_mask_cache[d],__ATOMIC_ACQUIRE),now=(uint64_t)now_ms();
    uint64_t at=cached>>9;
    return (cached&256u)&&now>=at&&now-at<1000?(int)(cached&15u):-1;
}
#ifndef GEOMETRY_TEST
static void stem_mask_poll(void){
    unsigned request=__atomic_load_n(&stem_mask_request,__ATOMIC_RELAXED);
    if(request<1||request>4)return;
    int d=(int)request-1;char path[80],data[16];
    snprintf(path,sizeof path,"/home/root/settings/azstem.stems%s",d==0?"":d==1?"2":d==2?"3":"4");
    uint64_t result=0;FILE *f=fopen(path,"rb");
    if(f){size_t n=fread(data,1,sizeof data,f);unsigned mask=0;
        if(!ferror(f)&&n<sizeof data&&stem_mask_parse(data,n,&mask))
            result=((uint64_t)now_ms()<<9)|256u|mask;
        fclose(f);
    }
    __atomic_store_n(&stem_mask_cache[d],result,__ATOMIC_RELEASE);
}
#endif

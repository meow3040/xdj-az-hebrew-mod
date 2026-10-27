/* SPDX-License-Identifier: GPL-3.0-only */

static uint64_t geometry_result;
static int geometry_requested;
#ifndef GEOMETRY_TEST
#include "auto_geometry.h"
#include <dlfcn.h>
#include <unistd.h>
#include <sys/uio.h>
#include "azsha256.h"
static int geometry_read(void *u,uintptr_t a,void *b,size_t n) {
    (void)u;struct iovec l={b,n},r={(void*)a,n};
    return process_vm_readv(getpid(),&l,1,&r,1,0)==(ssize_t)n;
}
static void *geometry_worker(void *u) {
    (void)u;Dl_info info;uintptr_t slot=0;
    void *symbol=dlsym(RTLD_DEFAULT,"azm_call_sret");
    if(symbol&&dladdr(symbol,&info)&&info.dli_fname&&info.dli_fbase) {
        FILE *f=fopen(info.dli_fname,"rb");
        if(f){azsha256_ctx ctx;unsigned char b[4096];size_t n;char hash[65];
            azsha256_init(&ctx);while((n=fread(b,1,sizeof b,f)))azsha256_update(&ctx,b,n);
            int ok=!ferror(f);fclose(f);azsha256_final_hex(&ctx,hash);
            if(ok&&!strcmp(hash,"03ae28d90d0ea0e2ebdeceb97b23d2a132c392a513bec4bb57a3e79274baca1c"))slot=(uintptr_t)info.dli_fbase+0x1e8dd0;
            if(ok&&!strcmp(hash,"7e3ffb90d4014d5a1a71a287aec0b1e066f0634b23e8b4453506373bcc065823"))slot=(uintptr_t)info.dli_fbase+0x1e7dd0;
            if(ok&&!strcmp(hash,"a025e0e9185318fd2472d7f4302096df93ff6f5fd9ecd04debee08584a32d56e"))slot=(uintptr_t)info.dli_fbase+0x1e9298;
            if(ok&&!strcmp(hash,"6e0912f0109a189b0c4f82f791b8a019c045e17ecb4bc95e64b62e07d40e7158"))slot=(uintptr_t)info.dli_fbase+0x1e9298;
            if(ok&&!strcmp(hash,"bd8221b69b1fcf863d2164448c78e3362977de9dcac33546ed8c91eecdfa90d8"))slot=(uintptr_t)info.dli_fbase+0x1e9290;
            if(ok&&!strcmp(hash,"3f64342cb2f08301800af443be265449400722be6ee007510dc4e6634995bf04"))slot=(uintptr_t)info.dli_fbase+0x1ea290;
            if(ok&&!strcmp(hash,"a52376d2bce0a24bcb3f61355085722f6064698c4945e307c4c899f489f479b4"))slot=(uintptr_t)info.dli_fbase+0x1e9290;
            if(ok&&!strcmp(hash,"d63aaf84226a09a82d03cbcf54b953d4326889b921655c39b56f95d711b67ad9"))slot=(uintptr_t)info.dli_fbase+0x1ec6c0;
            if(ok&&!strcmp(hash,"208180b4cbe3db3bf22a671324d8a6ac5cd750c18e9c290a8d8bcc6ae74491ff"))slot=(uintptr_t)info.dli_fbase+0x1ec6c0;
            if(ok&&!strcmp(hash,"284285827bdbe80ef98f48c688fb47b9f60328443c398c2f0869f46e4f718a2d"))slot=(uintptr_t)info.dli_fbase+0x1ea6c0;
            if(ok&&!strcmp(hash,"fe5c7aeaa94a9154c284873a5331e238f87c53a5984d2169228d99fdacb5ea60"))slot=(uintptr_t)info.dli_fbase+0x1ea528;
            if(ok&&!strcmp(hash,"5001c1b8d2e2b17cba16e0804099ca6e4851c2105bdc8bd767e6dec35a5e2054"))slot=(uintptr_t)info.dli_fbase+0x1ea528;
            if(ok&&!strcmp(hash,"2e1df30a6fde61a1adb41bf32e78ac2babf1691b317e7884f5cf42c7b4bfddf7"))slot=(uintptr_t)info.dli_fbase+0x1ea528;
            if(ok&&!strcmp(hash,"357c3bd0848c323c76ce125eac3c8086ab800a0257509b77db1fd28bd25ee601"))slot=(uintptr_t)info.dli_fbase+0x1ea528;
            if(ok&&!strcmp(hash,"54b71fdf587736d1cce1c916240ccdf540a1b6e597fa97368d22dedc42bc6094"))slot=(uintptr_t)info.dli_fbase+0x1ea390;
        }
    }
    if(!slot)return NULL;
    uint64_t previous=0;
    for(;;){uint64_t value=0;struct geometry g;
        if(__atomic_load_n(&geometry_requested,__ATOMIC_RELAXED)&&geometry_measure(geometry_read,NULL,slot,&g))
            value=((uint64_t)(g.mode+1)<<48)|((uint64_t)g.left<<24)|(unsigned)g.bottom;
        __atomic_store_n(&geometry_result,value&&value==previous?value:0,__ATOMIC_RELEASE);
        previous=value;struct timespec delay={0,250000000};nanosleep(&delay,NULL);
    }
    return NULL;
}
#endif


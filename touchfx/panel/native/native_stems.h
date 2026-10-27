/* SPDX-License-Identifier: GPL-3.0-only */
/* UI-thread-only bridge: core owns masks, pad requests, LEDs and sound. */
static int (*native_snapshot)(int,int *,unsigned);
static int (*native_toggle)(int,int);
static az_surface *native_surfaces[4];
static int native_values[4][12],native_visible[4];
static az_rect native_button_rect(const int *v,int k) {
    int gap=v[9]?3:9,margin=v[9]?5:0;
    int width=(v[2]-2*margin-3*gap)/4;
    return (az_rect){v[0]+margin+k*(width+gap),v[1],width,v[3]};
}
static int native_get(int deck,int *v) {
    if(!active||!geometry_ready||!native_snapshot||!native_toggle||
       !C->view||C->view()!=AZ_VIEW_DECK||
       !native_snapshot(deck,v,12))return 0;
    if(v[0]<0||v[1]<0||v[2]<40||v[3]<8||v[0]+v[2]>1280||v[1]+v[3]>800||
       v[4]<0||v[4]>15||v[9]<0||v[9]>1||v[10]!=1)return 0;
    uint64_t measured=__atomic_load_n(&geometry_result,__ATOMIC_ACQUIRE);
    int four=(int)(measured>>48)-1==2;
    if(four!=v[9]||(four&&panel_area.x<144))return 0;
    for(int k=0;k<4;k++)if(v[5+k]<0||v[5+k]>1000)return 0;
    return 1;
}
static void native_paint(az_draw *draw,az_rect area,void *u) {
    int d=(int)(uintptr_t)u;if(!native_visible[d])return;
    const int *v=native_values[d];
    static const char *const full[]={"VOCAL","INST","BASS","DRUMS"};
    static const char *const short_names[]={"VO","IN","BA","DR"};
    C->fill_rect(draw,area.x,area.y,area.w,area.h,v[9]?0x232323:0);
    for(int k=0;k<4;k++) {
        az_rect r=native_button_rect(v,k);
        int on=(v[4]&(1<<k))!=0,gain=C->row_value(dim_row)?v[5+k]:1000;
        uint32_t ink=stem_visual_ink(stem_colours[k],on,gain);
        C->fill_rect(draw,r.x,r.y,r.w,r.h,0x232323);
        C->draw_rect(draw,r.x,r.y,r.w,r.h,1,ink);
        int px=r.h*11/20;if(px>17)px=17;if(px<8)px=8;
        const char *label=v[9]?short_names[k]:full[k];
        int tw=C->text_width?C->text_width(label,px):0;
        C->draw_text(draw,r.x+(r.w-tw)/2,r.y+(r.h-px*3/2)/2,label,px,ink);
    }
}
static int native_input(const az_event *e,void *u) {
    int d=(int)(uintptr_t)u,v[12];
    if(!native_touch_enabled()||e->kind!=AZ_EV_TAP||!native_visible[d]||!native_get(d,v))return 1;
    /* A layout/deck change between paint and tap must never target stale boxes. */
    if(memcmp(v,native_values[d],4*sizeof(int))||v[9]!=native_values[d][9])return 1;
    for(int k=0;k<4;k++)if(inside(native_button_rect(v,k),e->x,e->y)) {
        native_toggle(d,k);break;
    }
    return 1;
}
static int native_init(void) {
    if(!native_snapshot||!native_toggle)return 0;
    for(int d=0;d<4;d++) {
        az_surface_v1 surface={.size=sizeof surface,.layer=AZ_LAYER_OVER_EP147,
            .views=AZ_VIEW_BIT(AZ_VIEW_DECK),.on_paint=native_paint,
            .on_input=native_input,.user=(void *)(uintptr_t)d};
        native_surfaces[d]=C->surface_create(ID,&surface);
        if(!native_surfaces[d])return 1;
        C->surface_show(native_surfaces[d],0);
    }
    return 0;
}
static void native_tick(void) {
    static int last_dim=-1;
    int dim=C->row_value(dim_row),dim_changed=dim!=last_dim;last_dim=dim;
    for(int d=0;d<4;d++) {
        if(!native_surfaces[d])continue;
        int v[12],show=native_get(d,v);
        if(show) {
            if(!native_visible[d]||dim_changed||memcmp(v,native_values[d],sizeof v)) {
                memcpy(native_values[d],v,sizeof v);
                C->surface_place(native_surfaces[d],(az_rect){v[0],v[1],v[2],v[3]});
                C->surface_redraw(native_surfaces[d]);
            }
        }
        native_visible[d]=show;C->surface_show(native_surfaces[d],show);
    }
}

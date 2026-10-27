/* SPDX-License-Identifier: GPL-3.0-only */
/* Raw pointer worker tracks XY. Production uses the core's fixed guard;
 * the old rectangle model remains only in geometry-unit tests. */
static uint64_t glass_input_bounds;
static int (*glass_guard_bind)(const char *,az_surface *);
static int (*glass_marks)(const char *,az_surface *,int,az_rect);
static uint64_t (*info_passthrough)(const char *,az_surface *,int);
static int (*glass_head)(const char *,az_surface *,int,az_rect);
static int (*glass_surface)(const char *,az_surface *,int,az_rect);
#ifdef GEOMETRY_TEST
static int glass_owned;
static int glass_touch(const az_touch *t,void *u){
    (void)u;if(t->slot!=0)return AZ_TOUCH_PASS;
    if(!t->down){int owned=glass_owned;glass_owned=0;return owned?AZ_TOUCH_TAKE:AZ_TOUCH_PASS;}
    if(glass_owned)return AZ_TOUCH_TAKE;
    uint64_t b=__atomic_load_n(&glass_input_bounds,__ATOMIC_ACQUIRE);
    if(!touch_in(b,t->x,t->y)||touch_in(__atomic_load_n(&touch_info_hole,__ATOMIC_ACQUIRE),t->x,t->y))return AZ_TOUCH_PASS;
    glass_owned=1;return AZ_TOUCH_TAKE;
}
#endif
static void glass_tick(void){
    int panel_visible=active&&opened&&geometry_ready&&selected>=0&&selected<4;
    uint64_t hole=info_passthrough?info_passthrough(ID,panel_surface,panel_visible):0;
    uint64_t before_hole=__atomic_load_n(&touch_info_hole,__ATOMIC_RELAXED);
    __atomic_store_n(&touch_info_hole,hole,__ATOMIC_RELEASE);
    if(before_hole!=hole&&C->surface_redraw)C->surface_redraw(panel_surface);
    int want=active&&opened&&geometry_ready&&selected>=0&&selected<4&&(kind[selected]==1||kind[selected]==2)&&panel_layout(kind[selected]==2)==2;
    uint64_t b=want?__atomic_load_n(&touch_bounds,__ATOMIC_ACQUIRE):0;
    if(!b)want=0;
    az_rect glass=field_rect(panel_area);
    /* Visual transparency reaches the screen edge; playable XY bounds stay
     * unchanged. Consume the spare strip so native FX cannot receive a tap. */
    glass.w=panel_area.x+panel_area.w-glass.x;
    if(!glass_surface||glass_surface(ID,panel_surface,want,glass))b=0;
    if(glass_marks)glass_marks(ID,panel_surface,want,field_rect(panel_area));
    __atomic_store_n(&glass_input_bounds,b?pack_touch_rect(glass):0,__ATOMIC_RELEASE);
}

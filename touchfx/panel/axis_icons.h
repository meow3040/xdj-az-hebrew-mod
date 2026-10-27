#include "touchfx_axis.h"
static az_rect axis_rect(az_rect f,int icon){return axis_marker_rect(f,icon,__atomic_load_n(&touch_info_hole,__ATOMIC_ACQUIRE));}
static void axis_paint(az_draw*draw,az_rect f){axis_prepare();for(int icon=0;icon<4;icon++){az_rect r=axis_rect(f,icon);C->draw_image(draw,r.x,r.y,axis_icons[icon],20,20);}}

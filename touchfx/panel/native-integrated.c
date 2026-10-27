/* SPDX-License-Identifier: GPL-3.0-only */
#define TOUCHFX_INTEGRATED
#include "native/native-labels.c"
int touchfx_native_init(const az_core_v1 *core,int enabled,int dim,int touch){enable_row=enabled;dim_row=dim;native_touch_row=touch;return init(core);}
void touchfx_native_tick(void){tick();}

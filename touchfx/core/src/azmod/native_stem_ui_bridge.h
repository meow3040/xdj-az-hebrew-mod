/* SPDX-License-Identifier: GPL-3.0-only */
/* Private, versioned UI bridge. No audio/control-file writes: toggle requests
 * enter exactly the queue used by physical pads and are consumed on this thread. */
int mrdjjag_stem_ui_snapshot(int deck, int *out, unsigned count);
int mrdjjag_stem_ui_toggle(int deck, int stem);
int mrdjjag_stem_ui_snapshot(int deck, int *out, unsigned count) {
    if (!g_cm_on_ui || !out || count < 12 || deck < 0 || deck >= AZMOD_STEM_DECKS ||
        g_state.mode != AZ_CLOSED || !g_state.waveform_visible || !azmod_stem_native ||
        !azmod_stems_engine(&g_state) || !g_state.stems_available[deck] ||
        !g_state.pad_beatloop[deck] || __atomic_load_n(&g_grid_adjust,__ATOMIC_RELAXED) ||
        g_fb.w != 1280 || g_fb.h != 800) return 0;
    if (!g_four_deck && azmod_logical_deck(deck & 1,g_deck_sel[deck & 1]) != deck) return 0;
    if (g_four_deck)
        azmod_stem_dots_rect(&g_fb,deck,&out[0],&out[1],&out[2],&out[3]);
    else {
        int x,y,w,h,lastx,lasty,lastw,lasth;
        azmod_stem_btn_rect(&g_fb,deck,0,&x,&y,&w,&h);
        azmod_stem_btn_rect(&g_fb,deck,3,&lastx,&lasty,&lastw,&lasth);
        out[0]=x;out[1]=y;out[2]=lastx+lastw-x;out[3]=h;
    }
    out[4]=0;
    for(int k=0;k<4;k++) {
        if(g_state.stem_on[deck][k])out[4]|=1<<k;
        int gain=g_stem_gain_fn?g_stem_gain_fn(deck,k):1000;
        out[5+k]=gain<0||gain>1000?1000:gain;
    }
    out[9]=g_four_deck!=0;out[10]=1;out[11]=0;
    return 1;
}
int mrdjjag_stem_ui_toggle(int deck, int stem) {
    int snapshot[12];
    if(stem<0||stem>=4||!mrdjjag_stem_ui_snapshot(deck,snapshot,12))return 0;
    __atomic_or_fetch(&g_pad_req[deck],1u<<stem,__ATOMIC_ACQ_REL);
    return 1;
}


#include "sampler_ui_bridge.h"

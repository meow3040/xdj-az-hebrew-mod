/* SPDX-License-Identifier: GPL-3.0-only */
#include "stem_touch_audio.h"
#include "stem_grid.h"
static _Atomic int g_stem_grid_on[NDECK];
void mrdjjag_stem_fx_quantize(int deck,int on);
void mrdjjag_stem_fx_quantize(int deck,int on){if(deck>=0&&deck<NDECK){atomic_store_explicit(&g_stem_grid_on[deck],on!=0,memory_order_relaxed);if(on)atomic_store_explicit(&g_grid_demand,1,memory_order_relaxed);}}
static int stem_grid_read(int d,unsigned gen,long long pos,int rate,int period,double *phase,int *wait){
    if(!atomic_load_explicit(&g_stem_grid_on[d],memory_order_relaxed)||atomic_load_explicit(&g_stem_grid_gen[d],memory_order_acquire)!=gen)return 0;
    drum_grid *g=&g_drum_grid[d];unsigned seq=atomic_load_explicit(&g->seq,memory_order_acquire);if(seq&1u)return 0;
    long long anchor=atomic_load_explicit(&g->anchor_src,memory_order_relaxed);double beat=atomic_load_explicit(&g->beat_src,memory_order_relaxed);int valid=atomic_load_explicit(&g->valid,memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);if(!valid||seq!=atomic_load_explicit(&g->seq,memory_order_relaxed)||atomic_load_explicit(&g_stem_grid_gen[d],memory_order_acquire)!=gen)return 0;
    return stem_grid_phase(pos,anchor,beat,rate,period,phase,wait);
}
static stem_touch_audio g_stem_touch_audio[NDECK];
static _Atomic int g_stem_fx_volume_k = 1000;
static float g_stem_fx_volume_sm[NDECK] = {1.f, 1.f, 1.f, 1.f};
void azstem_stem_fx_volume(int knob);
void azstem_stem_fx_volume(int knob) {
    if (knob < 0) knob = 0;
    if (knob > 1023) knob = 1023;
    int value = knob >= 506 && knob <= 518 ? 1000 : knob < 512 ? knob * 1000 / 512
                                                               : 1000 + (knob - 512) * 1000 / 511;
    atomic_store_explicit(&g_stem_fx_volume_k, value, memory_order_relaxed);
}
int azstem_stem_fx_level(void);
int azstem_stem_fx_level(void) { return atomic_load_explicit(&g_stem_fx_volume_k, memory_order_relaxed); }
void azstem_stem_touch(int deck, int screen, int send, int rate, int amount);
void azstem_stem_touch(int deck, int screen, int send, int rate, int amount) {
    if (deck < 0 || deck >= NDECK) return;
    if (rate > 5 && rate != 7 && rate != 8 && rate != 9) rate = 5;
    stem_touch_publish(&g_stem_touch_audio[deck], screen, send, rate, amount);
}
void azstem_stem_touch_fx(int deck, int screen, int send, int rate, int mode, int value);
void azstem_stem_touch_fx(int deck, int screen, int send, int rate, int mode, int value) {
    if (deck < 0 || deck >= NDECK) return;
    if (rate > 5 && rate != 7 && rate != 8 && rate != 9) rate = 5;
    stem_touch_publish_fx(&g_stem_touch_audio[deck], screen, send, rate, mode, value);
}
/* Called after native deck DSP, BEFORE drum_mix_block and deck mute. No source
 * buffers are retained and sampler audio cannot enter this effect. */
static void stem_touch_block(int deck, float **ch, int nch, int ns) {
    if (deck < 0 || deck >= NDECK || !ch || !ch[0] || nch < 1 || ns < 1) return;
    deck_stems *dk = &g_dk[deck];
    unsigned ctrl = atomic_load_explicit(&dk->ctrl, memory_order_relaxed);
    int enabled = atomic_load_explicit(&g_enable, memory_order_relaxed) &&
                  (ctrl & 0x80u) && dk->stem_frames > 0 &&
                  atomic_load_explicit(&g_inject_on, memory_order_relaxed);
    unsigned word = __atomic_load_n(&g_stem_touch_audio[deck].word, __ATOMIC_ACQUIRE);
    if(!(word&0x80000000u)){
        stem_touch_audio *fx=&g_stem_touch_audio[deck];
        stem_long_echo_reset(&fx->echo);stem_filter_reset(&fx->filter);stem_gate_reset(&fx->gate);stem_long_roll_reset(&fx->roll);
        fx->playing=fx->grid_valid=fx->roll_pending=fx->roll_send=0;fx->age=44100;
        return; /* inactive: no dry buffer copies or effect processing */
    }
    /* Only additive Echo may boost above unity. Other effects blend back to the
     * untouched dry mix, so lowering the return never lowers the channel volume. */
    int additive = !(word & (0x20000000u | 0x10000000u | 0x08000000u | 0x04000000u | 0x02000000u));
    float target = atomic_load_explicit(&g_stem_fx_volume_k, memory_order_relaxed) / 1000.f;
    if (!additive && target > 1.f) target = 1.f;
    for (int base = 0; base < ns; base += 128) {
        int count = ns - base < 128 ? ns - base : 128;
        float dry[2][128];
        float *part[] = {ch[0] + base, nch > 1 && ch[1] ? ch[1] + base : NULL};
        for (int i = 0; i < count; i++) {
            dry[0][i] = part[0][i];
            dry[1][i] = part[1] ? part[1][i] : dry[0][i];
        }
        stem_touch_audio *fx=&g_stem_touch_audio[deck];
        unsigned gen=atomic_load_explicit(&g_deck_trackgen[deck],memory_order_relaxed);
        int bpm=atomic_load_explicit(&g_drum_bpm_q16[deck],memory_order_relaxed);
        long long pos=atomic_load_explicit(&g_play_obs[deck],memory_order_relaxed);
        int period=stem_fx_delay(bpm,(int)((word>>16)&15u));
        double phase=0;int wait=0;
        fx->grid_valid=stem_grid_read(deck,gen,pos,(int)((word>>16)&15u),period,&phase,&wait);
        if(fx->grid_valid&&base){phase=fmod(phase+(double)base/period,1.0);wait=phase*period<=0.5?0:(int)ceil((1.0-phase)*period);}
        fx->grid_phase=phase;fx->grid_wait=wait;
        stem_touch_process(&g_stem_touch_audio[deck], part, part[1] ? 2 : 1, count, enabled, !dk->paused, ctrl & 15u,
                           atomic_load_explicit(&g_deck_trackgen[deck], memory_order_relaxed),
                           atomic_load_explicit(&g_drum_bpm_q16[deck], memory_order_relaxed),
                           atomic_load_explicit(&g_play_obs[deck], memory_order_relaxed));
        for (int i = 0; i < count; i++) {
            float *gain = &g_stem_fx_volume_sm[deck];
            *gain += (target - *gain) * 0.005f;
            if (fabsf(*gain - target) < 1e-6f) *gain = target;
            float mix = !additive && *gain > 1.f ? 1.f : *gain;
            if (mix != 1.f) {
                part[0][i] = dry[0][i] + mix * (part[0][i] - dry[0][i]);
                if (part[1]) part[1][i] = dry[1][i] + mix * (part[1][i] - dry[1][i]);
            }
        }
    }
}

int mrdjjag_stem_fx_abi(void);
int mrdjjag_stem_fx_abi(void) { return 3; }

int mrdjjag_stem_fx_rates(void);
int mrdjjag_stem_fx_rates(void){return 8;}

int mrdjjag_stem_fx_four_beats(void);
int mrdjjag_stem_fx_four_beats(void){return 1;}

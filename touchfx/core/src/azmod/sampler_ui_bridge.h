/* SPDX-License-Identifier: GPL-3.0-only */
/* UI-only, idempotent sample pick. Reuses the pad-8 latch/LED/audio gate and
 * exclusive arm; never opens/moves the panel or synthesizes a touch contact. */
int mrdjjag_sampler_ui_select(int deck,int sample);
int mrdjjag_sampler_ui_select(int deck,int sample){
 if(!g_cm_on_ui||deck<0||deck>=AZMOD_STEM_DECKS||sample<0||sample>=4||
    g_state.mode!=AZ_CLOSED||!g_state.waveform_visible||
    __atomic_load_n(&g_grid_adjust,__ATOMIC_RELAXED)||
    !__atomic_load_n(&g_drum_arm,__ATOMIC_RELAXED)||
    __atomic_load_n(&g_mode_down[deck&1],__ATOMIC_RELAXED)||
    pad_mode_fresh(deck&1,deck)!=dg_slip_enum+1)return 0;
 void (*gate)(int,int)=__atomic_load_n(&g_drum_gate_fn,__ATOMIC_ACQUIRE);
 void (*pick)(int,int)=__atomic_load_n(&g_drum_select_fn,__ATOMIC_ACQUIRE);
 if(!gate||!pick)return 0;
 if(!__atomic_load_n(&g_drum_held[deck],__ATOMIC_RELAXED)){
    drum_disarm_others(deck,"touch sample arm");
    __atomic_store_n(&g_drum_held[deck],1,__ATOMIC_RELAXED);
    __atomic_fetch_or(&g_caption_repaint,1u<<deck,__ATOMIC_RELEASE);
    gate(deck,1);
 }
 __atomic_store_n(&g_drum_sel_led[deck],(unsigned)sample,__ATOMIC_RELAXED);
 pick(deck,sample);
 return 1;
}

/* The same global bank choice as the official menu; -1 reads, 1..4 selects.
 * Existing overlay sync loads the bank off the audio thread. No auto-arm. */
static int g_sampler_bank_dirty;
int mrdjjag_sampler_ui_bank(int bank);
int mrdjjag_sampler_ui_bank(int bank){
 if(!g_cm_on_ui)return 0;
 if(bank!=-1){
  if(bank<1||bank>4||g_state.mode!=AZ_CLOSED||!g_state.waveform_visible||
     !az_active(AZF_DRUM_BANK))return 0;
  if(az_get(AZF_DRUM_BANK)!=bank-1){az_set(AZF_DRUM_BANK,bank-1);g_sampler_bank_dirty=1;}
 }
 return az_get(AZF_DRUM_BANK)+1;
}

/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef STEM_FX_RATE_H
#define STEM_FX_RATE_H
#include "stem_echo.h"
/* Keep0..5unchanged;6remains sampler1/32. STEMS additions use7and8. */
static inline double stem_fx_beats(int rate){return rate==9?4.0:rate==7?.75:rate==8?2.0/3.0:rate>=0&&rate<=5?(1<<rate)/16.0:0.0;}
static inline int stem_fx_delay(int bpm_q16,int rate){
 if(rate!=7&&rate!=8&&rate!=9)return stem_echo_delay(bpm_q16,rate);
 double bpm=bpm_q16>0?bpm_q16/65536.0:120.0;if(bpm<40)bpm=40;if(bpm>220)bpm=220;
 return (int)(44100.0*60.0/bpm*stem_fx_beats(rate)+.5);
}
#endif

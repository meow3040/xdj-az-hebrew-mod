/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef STEM_GRID_H
#define STEM_GRID_H
#include <math.h>
#include "stem_fx_rate.h"
/* Source-time anchor supplies complete beat count, including two-beat cycles.
 * Output period supplies the tempo-adjusted sample length. No bar assumption. */
static inline int stem_grid_phase(long long position,long long anchor,double beat,int rate,int period,double *phase,int *wait) {
    if(position<0||!isfinite(beat)||beat<1.0||beat>200000.0||stem_fx_beats(rate)<=0||period<1)return 0;
    double span=beat*stem_fx_beats(rate);
    double rel=fmod((double)position-(double)anchor,span);if(rel<0)rel+=span;
    double p=rel/span;if(!isfinite(p))return 0;
    double left=(1.0-p)*period;
    /* Within half an output sample of the boundary: already on the boundary. */
    int n=p*period<=0.5||left<=0.5?0:(int)ceil(left);
    if(n>period)n=period;*phase=p;*wait=n;return 1;
}
#endif

/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the effects. A reverb (a feedback delay network, after The Outside Inside's), a tempo-locked echo
 * that wobbles like tape, and a grain cloud that can freeze and sing an octave up into the reverb (SHIMMER). Their
 * lines are 16-bit, to fit the FM-1's RAM. */
#ifndef AI_FX_H
#define AI_FX_H

#include "dsp.h"

#define RV_LINES 8
#define RV_MAX 4096
#define RV_AP 512
typedef struct {
    int16_t buf[RV_LINES][RV_MAX]; int len[RV_LINES], pos[RV_LINES];
    float g[RV_LINES]; OnePole damp[RV_LINES];
    float ap[4][2][RV_AP]; int aplen[4], appos[4];
    float rt60, damp_hz;
} Reverb;
void reverb_init(Reverb *rv, float size);
void reverb_set(Reverb *rv, float rt60, float damp_hz);   /* (cheap: no clearing) */
void reverb_run(Reverb *rv, const float *inL, const float *inR, float *outL, float *outR, int n);

#define ECHO_MAX 32768                   /* 0.74 s */
typedef struct {
    int16_t buf[ECHO_MAX]; int pos;
    float time_s, fb, wow_ph, wow;       /* the delay, its feedback, the tape's wobble */
    OnePole lp, hp;
    float tl, tr;                        /* the two taps, glided */
} Echo;
void echo_init(Echo *e);
void echo_run(Echo *e, const float *inL, const float *inR, float *outL, float *outR, int n);

#define GR_MAX 24576                     /* 0.56 s */
#define GR_N 6
typedef struct {
    int16_t buf[GR_MAX]; int pos;
    int frozen;
    struct { float at, rate, t, len, amp, pan; int on, shimmer; } g[GR_N];
    float density, size, spread, shimmer;   /* set from GRAIN and SHIMMER */
    Rng rng;
    float next;
    const float *ratios; int nratios;    /* the pitches a grain may take, in tune with the scale (fifths and octaves) */
} Grains;
void grains_init(Grains *g, uint32_t seed);
/* in: mono; out: stereo, and the octaves for the reverb */
void grains_run(Grains *g, const float *in, float *outL, float *outR, float *shim, int n);

#endif

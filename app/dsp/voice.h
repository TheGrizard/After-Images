/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the sound engines. Ten small synthesis engines, each a few operations a sample, so twenty voices and
 * the effects fit the FM-1's two cores. Every engine has three colours: TONE (dark to bright), WEIGHT (soft to heavy:
 * drive, attack, index, fold) and WEIRD (what it does past halfway is its own: inharmonic, folded, ring-modulated). */
#ifndef AI_VOICE_H
#define AI_VOICE_H

#include "dsp.h"

enum {
    E_SAW,      /* DRIFT SAW: two detuned saws, low-passed */
    E_FM,       /* FM: two operators, feedback */
    E_PLUCK,    /* STRING: Karplus-Strong */
    E_DX,       /* DX: four-operator FM, one of four algorithms, each modulator fading at its own pace: the spectrum moves */
    E_PULSE,    /* BLEEP: a pulse, its width moving */
    E_GLASS,    /* GLASS: four sine partials */
    E_BREATH,   /* BREATH: noise through a tuned resonance */
    E_VOX,      /* VOICE: a pulse through two formants */
    E_FOLD,     /* FOLD: a sine folded back on itself */
    E_SUB,      /* SUB: a sine with its octave, driven */
    E_STRINGS,  /* STRINGS: a string machine, three saws each wandering in pitch (the ensemble), soft and dark */
    E_EPIANO,   /* E.PIANO: an 80s FM electric piano, the tine's knock fading into a warm tone, wobbling a little */
    NUM_ENGINES
};
extern const char *const ENGINE_NAME[NUM_ENGINES];

typedef struct {
    uint8_t engine;
    float tone, weight, weird;           /* 0..1 each */
    float attack, release;               /* seconds */
    float pan_move;                      /* 0..1: how far the note wanders across the stereo field as it sounds */
    float glide;                         /* seconds the filter takes to follow a new note (the pitch never slides) */
    float sweep, reso;                   /* 0..1: the filter opening up over each note, the filter's resonance */
} Sound;

#define KS_MAX 512                       /* the string's longest line: 86 Hz */
typedef struct {
    uint8_t on, engine, rel;             /* rel: 0 attack, 2 past it */
    uint8_t id;                          /* the note's number for MIDI and the screen */
    int8_t note;                         /* MIDI note */
    float freq, vel, pan, gl, gr;
    float env, a_k, r_k, sus, d_k, hold; /* the envelope: attack and release rates, sustain, decay, seconds to hold */
    float ph[4], fb, ws, lfo, lfo_dp;    /* phases, feedback, width */
    float p[6];                          /* the engine's settings at the note's start */
    float drive;
    float oe[4], ok[4], r[4]; uint8_t alg;   /* DX: each operator's level, its fade a sample, its ratio; the algorithm */
    Svf f, f2;
    OnePole lp;
    float z1, z2;
    int16_t ks[KS_MAX]; int ks_len, ks_pos; float ks_frac, ks_damp, ks_lp;
    uint32_t noise;
    float age;
    float pan_amt, pan_ph, pan_dp;       /* the note's wandering in the stereo field */
    float freq_t, ff, glide_k;           /* the pitch; ff the filter's own pitch, gliding after it (a block's share) */
    float sweep, sw, sw_k, reso;         /* the filter's sweep: how far, where it is (0 closed .. 1 open), its pace */
} Voice;

void voice_start(Voice *v, const Sound *s, int note, float vel, float dur, float pan, float from_hz, uint32_t seed);   /* from_hz: glide from there (0: no) */
void voice_release(Voice *v);
/* legato: the note playing moves to a new pitch, its envelope not started again (the filter glides after it) */
void voice_legato(Voice *v, int note, float vel, float dur);
/* add n samples (n <= BLK) into L and R */
void voice_render(Voice *v, float *L, float *R, int n);

#endif

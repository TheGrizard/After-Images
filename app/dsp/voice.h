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
    E_TRI,      /* TRIANGLE: soft; WEIGHT folds it, DRIFT leans it towards a saw */
    E_SINE,     /* SINE: pure; WEIGHT warms it (its 2nd and 3rd), DRIFT a slow chorus beside it */
    E_SQUARE,   /* SQUARE: plain and warm, through its filter */
    E_CZ,       /* CZ: 80s digital phase distortion, sine to saw or to its resonant sweep as the note goes */
    E_SYNC,     /* SYNC: a hard-synced saw, its slave sweeping down across the note */
    E_SHAPES,   /* SHAPES: unusual waves (rectified, half, trapezoid, sine of sine, parabola, bent), DRIFT moves through them */
    E_BOARDS,   /* BOARDS: a saw and a square a little apart, a dark filter opening slowly on each note (a wah), a slow vibrato */
    E_REESE,    /* REESE: two saws a little apart, their beating sweeping slowly, driven into a 24 dB low-pass, a sub under */
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
    int16_t ks[KS_MAX]; int ks_len, ks_pos; float ks_frac, ks_damp, ks_lp, ks_e;   /* ks_e: how hard it rings (ENTROPY's slack) */
    uint32_t noise;
    float age;
    float pan_amt, pan_ph, pan_dp;       /* the note's wandering in the stereo field */
    float pm;                            /* the pitch's wandering now (WARBLE): a multiplier, set each block */
    float freq_t, ff, glide_k;           /* the pitch; ff the filter's own pitch, gliding after it (a block's share) */
    float sweep, sw, sw_k, reso;         /* the filter's sweep: how far, where it is (0 closed .. 1 open), its pace */
    OnePole sl[2];                       /* SWEEP's own filter, for the engines without one */
    uint32_t seed; float ix;             /* the note's seed (its settings again, the same: voice_retune); DX's index */
    float ent, cph, cz, cz2, clfo;       /* ENTROPY past halfway: the chaos oscillator (how much; its phase, feedback, PWM's sweep) */
    uint8_t cmode;                       /* which: FM feedback, PWM, or a sine phase-modulated by the PWM pulse */
    float bab;                           /* VOICE's babble under ENTROPY: where its vowels have wandered */
} Voice;

void voice_start(Voice *v, const Sound *s, int note, float vel, float dur, float pan, float from_hz, uint32_t seed);   /* from_hz: glide from there (0: no) */
void voice_release(Voice *v);
/* legato: the note playing moves to a new pitch, its envelope not started again (the filter glides after it) */
void voice_legato(Voice *v, int note, float vel, float dur);
/* add n samples (n <= BLK) into L and R */
extern float g_voice_floor;              /* a released voice ends below this (raised under load: tails cut sooner) */
void voice_skip(Voice *v, int n);
void voice_retune(Voice *v, const Sound *s);   /* a sounding note follows its layer's knobs (not its engine: that's the next note's) */        /* an unheard voice: its envelope only */
void voice_render(Voice *v, float *L, float *R, int n);

#endif

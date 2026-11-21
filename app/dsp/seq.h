/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the sequencers. Ten ways of making a line of notes, each a small system with rules: none of them
 * plays dice in the moment except from the piece's own seed, so a piece plays the same way each time it is loaded.
 * A sequencer makes scale degrees (0 .. its range), a loudness and a length; the layer turns those into notes. They
 * cost next to nothing: a few operations each control step (32 samples). */
#ifndef AI_SEQ_H
#define AI_SEQ_H

#include "dsp.h"

enum {
    S_EUCLID,   /* Euclidean rings: k hits spread over n steps, a melody of another length over them */
    S_TURING,   /* a looping shift register that sometimes flips a bit */
    S_MARKOV,   /* each note chosen from the last by a table of chances */
    S_LORENZ,   /* the Lorenz attractor: a note at each of its loops' peaks */
    S_BOUNCE,   /* bouncing balls: faster and softer as each one settles */
    S_ORBITS,   /* planets, each with its own year: a note as each passes the top */
    S_LIFE,     /* a row of cells growing by a rule (Wolfram's): the live ones play */
    S_CHIMES,   /* wind chimes: gusts knock them together */
    S_PHASE,    /* one phrase on two players at slightly different speeds */
    S_DRUNK,    /* a Brownian walk: each note a small step from the last */
    NUM_SEQS
};
extern const char *const SEQ_NAME[NUM_SEQS];

/* dur in beats; pan -1..1, or 9: the layer's choice; mod 0..1: the system's second output, a modular CV (the attractor's
 * y, the register's upper bits, the ball's speed, the automaton's population...): it shapes the note's sound */
typedef struct { int deg; float vel, dur, pan, mod; } SeqNote;
#define SEQ_OUT 8

typedef struct {
    uint8_t type;
    int range;                           /* degrees: 0 .. range-1 */
    float density, mutate, chaos, warp;  /* 0..1 each (density 0.5 as designed) */
    Rng rng;
    float t;                             /* the step clock, in steps */
    uint32_t step;
    float swing_phase, bend_ph;          /* WARP's: the swing, the phrase that speeds up and slows down */
    /* each type's own state */
    int n, k, rot, m, pos, mpos, len;
    int8_t mel[16];
    uint32_t reg;
    uint8_t mk[8][8];                    /* MARKOV's table */
    float x, y, z, dz, rho, g, h, v, e;  /* LORENZ, BOUNCE */
    int base, idx, cnt;
    float per[4], ph[4]; int8_t pdeg[4]; /* ORBITS */
    uint16_t cells; uint8_t rule;        /* LIFE */
    float wind, wtarget, wleft, gust_t[6]; int8_t gust_d[6]; int ngust;   /* CHIMES */
    float ph2, rate2;                    /* PHASE */
    float mod;                           /* the CV the next note carries (each type sets it) */
    /* the notes this step made */
    SeqNote out[SEQ_OUT]; int nout;
    int ratchet_left; float ratchet_t, ratchet_dt; SeqNote ratchet_n;
} Seq;

void seq_init(Seq *s, int type, int range, uint32_t seed);
/* advance by `beats` (a control step's worth, at the piece's tempo times the layer's rate); the notes it made are in
 * s->out[0 .. s->nout-1] */
void seq_run(Seq *s, float beats);
/* LINK: another layer nudges this one, each type in its own way */
void seq_poke(Seq *s);

#endif

/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the piece. Four layers (GROUND, BODY, LIGHT, DUST), each a sequencer driving a sound engine in its own
 * frequency band; a conductor that moves the harmony and the form (which layers play, and how) as EVOLVE asks; LINK,
 * by which the layers' notes nudge each other's sequencers; and the effects. Plain C, no hardware calls: the same code
 * runs on the FM-1, in the host simulator and in tools/render. */
#ifndef AI_ENGINE_H
#define AI_ENGINE_H

#include "music.h"
#include "voice.h"
#include "seq.h"
#include "fx.h"

/* ---------------- the settings: what the knobs set, what SAVE keeps (the app's settings hold these) */
typedef struct {
    uint8_t engine, tone, weight, weird;          /* EDIT: the sound (engine an index, the rest 0..100) */
    uint8_t seq, density, rate, mutate;           /* SEQ: the sequencer (seq and rate indexes) */
    uint8_t level, attack, release; int8_t octave;   /* ENV */
    uint16_t salt;                                /* the layer's own part of the seed (a layer re-rolled alone) */
} LayerP;

typedef struct {
    uint32_t seed;
    uint8_t mood, motion, evolve, weird;          /* HOME (weird: DRIFT, the layers' wandering levels and pans) */
    uint8_t reverb, echo, grain, wear;            /* EFFECTS (grain: CHORUS, the send's, since v0.29; it was GRAIN); chorus: WARBLE's WEAR since v0.29, its chorus before (in v0.1 SHIMMER) */
    uint8_t link, warp, drift, chaos;             /* MOTION (the LFO button) */
    uint8_t key, scale, chord, tempo;             /* SCALE (the GLO button): tempo is BPM - 40 */
    uint8_t dirt, warble, flutter, detune;        /* EFFECTS' DIRT; WARBLE's (LFO again): in v0.1 the tape's, zeroed at the upgrade */
    LayerP l[NUM_LAYERS];
    /* v0.2: each layer's VOICE page (ENV again) */
    uint8_t glide[NUM_LAYERS];                    /* 0..100: how long a note slides to its pitch */
    uint8_t legato[NUM_LAYERS];                   /* 1: a note playing slides on to the next, not started again */
    uint8_t sweep[NUM_LAYERS];                    /* 0..100: the filter opening over each note */
    uint8_t reso[NUM_LAYERS];                     /* 0..100: the filter's resonance */
    /* v0.4: each layer's SENDS (FX): how much of it goes to the reverb, the echo, the chorus (GRAIN until v0.29), and how much DIRT it gets */
    uint8_t s_rv[NUM_LAYERS], s_echo[NUM_LAYERS], s_grain[NUM_LAYERS], s_dirt[NUM_LAYERS];
} Piece;
#define PIECE_SIZE1 __builtin_offsetof(Piece, glide)   /* v0.1's Piece: up to here */
#define PIECE_SIZE2 __builtin_offsetof(Piece, s_rv)    /* v0.2's and v0.3's */
void piece_sends_default(Piece *p);                     /* each layer's sends as its role has them (an older piece's) */

#define NUM_RATES 10
extern const char *const RATE_NAME[NUM_RATES];
extern const float RATES[NUM_RATES];

/* REC: a new piece from a seed (the scale, the key and the tempo kept, unless `all`); one layer anew (SEL + REC) */
void piece_roll(Piece *p, uint32_t seed, int all, int fx);   /* fx: the effect it features (FX_*), -1 its seed's choice */
/* the effect each new piece features (REC deals them from a shuffled deck, none twice in a row) */
enum { FX_SHIMMER, FX_REVERSE, FX_CHORUS, FX_ENSEMBLE, FX_FOLD, FX_CRUSH, FX_SPACE, FX_HAZE, NUM_FX };
extern const char *const FX_NAME[NUM_FX];
void piece_next_key(Piece *p, int key, int scale, uint32_t seed);   /* REC: round the circle of fifths from the last */
void piece_roll_layer(Piece *p, int layer, uint32_t seed);
void piece_default(Piece *p);
int piece_ok(const Piece *p);
void piece_name(uint32_t seed, char *out);    /* two words from the seed: GLASS HERON */

/* ---------------- what the screen sees */
typedef struct { uint8_t layer, note, vel; int8_t pan; uint8_t dur; } UiNote;   /* dur: tenths of a second */
#define UI_Q 64

#define ENGINE_SUB 8                     /* control steps an app block holds (fm1-base's 256 samples) */
#define KEYS 4                           /* the keyboard's voices are a fifth "layer" */
#define NBUS (NUM_LAYERS + 1)
#define MAX_POLY 6
#define KEY_POLY 8
#define EVQ 24

typedef struct { uint8_t sub, voice, rel; int8_t note; float vel, dur, pan, from_hz; Sound snd; uint32_t seed; } VoiceEv;   /* rel: 0 start, 1 release, 2 legato */

/* DRIFT's morphing: what of each layer wanders, slowly, as far as DRIFT lets it */
enum { MW_RV, MW_ECHO, MW_GRAIN, MW_DIRT, MW_TONE, MW_WEIGHT, MW_ENT, NMORPH };

typedef struct {
    Seq seq;
    Voice v[MAX_POLY];
    int nv;
    Svf hp[2], lp[2];
    float gain, present, present_t;      /* level glide; the form's fade in and out */
    Drift dtone, dpan;
    float tone_off, dens_off;            /* the section's colour */
    float cv;                            /* its activity: up with each note, falling after (a modular envelope follower) */
    float rnd, rnd_t, rnd_left;          /* a random voltage, smoothed, stepping at its own pace (DRIFT) */
    int src;                             /* the layer patched into this one (LINK: its activity opens this one's filter) */
    uint8_t type_now; uint32_t seed_now;
    VoiceEv q[EVQ]; int nq;
    int next_v;
    int8_t last_v[5]; float last_hz[5]; int last_n;   /* the last note's (or chord's) voices and pitches: glide, legato */
    float lp_hz, hp_hz;                  /* its band's filters now (TONE, gliding there) */
    float im, im_t, im_g, im_far, im_pan, im_l, im_r;   /* DRIFT's: its wandering level, where it's going, its gain, how far off, its walk's pan and gains */
    Drift dwalk;
    Drift mwalk[NMORPH]; float mw[NMORPH];   /* DRIFT's morphing: each of its sends, TONE, WEIGHT, ENTROPY wandering (mw: -1..1, DRIFT's share) */
    Dirt dirt;                           /* its own DIRT (the character EFFECTS', the amount its send's) */
    uint32_t knobs;                      /* its sound's knobs, last block (changed: the sounding notes follow) */
} Layer;

typedef struct Engine Engine;
struct Engine {
    const Piece *p;                      /* the settings it follows (the app's) */
    Layer L[NUM_LAYERS];
    Voice kv[KEY_POLY]; VoiceEv kq[EVQ]; int nkq; int key_next;
    int8_t kv_lay[KEY_POLY];             /* each key voice's layer (whose knobs it follows) */
    Svf khp[2], klp[2]; Dirt kdirt;      /* the keys through their layer's chain: its TONE and DIRT (ENTROPY: in the voices) */
    int key_layer;                       /* the layer whose sound the keys play (SELECT) */
    volatile uint32_t key_in[16]; volatile uint32_t key_w, key_r;   /* keys and MIDI notes in: (on, note, vel) */
    float bus[ENGINE_SUB][NBUS][2][BLK];
    float gains[ENGINE_SUB][NBUS];
    int nsub;
    /* the conductor */
    Rng rng;
    uint32_t seed_now;
    float beat, bpm;                     /* the piece's position, in beats */
    float ext_bpm;                       /* MIDI clock's tempo, followed (0: the piece's own) */
    int bar, root, chord_bars, chord_left, sec_left, mask;
    int want_change;                     /* LINK asked for a new chord at the next bar */
    float duck, gov, fade;
    int playing;
    /* effects */
    Reverb rv; Echo echo; Ens ens; float shim_prev[BLK]; Wear wear;   /* (the shimmer's shifter: in engine.c, out of the pool) */
    float wb_v, wb_t, wb_left, wb_rate, fl_ph, pm[NUM_LAYERS + 1];
    float im_left;                       /* DRIFT: seconds until the levels choose where to wander next */   /* WARBLE: the pitch's slow wandering, its flutter; each bus's pitch now */
    float dc[2];
    float sub[2];                        /* MOOD's low shelf: the mix's lows (below ~110 Hz), added back darker it goes */
    float lev_ss, lev_g;                 /* the leveller: the mix's loudness, slowly; its gain */
    /* MIDI out: the app's; notes on their layers' channels, the clock */
    void (*midi)(uint32_t pkt);
    uint8_t midi_ch, midi_on, midi_clock, midi_keys_ch;
    struct { uint8_t ch, note; float off; } mo[32]; int nmo;
    float clock_acc; float t;
    /* costs, for the balance between the cores (ticks an app block) */
    uint32_t (*clock)(void);
    float cost[NBUS + 1]; float cost_rv; float rv_size, rv_damp; float shim_env;   /* (shim_env: the shimmer loop's level, its limiter's) */  /* (cost_rv: the reverb stage's, which the second core may take beside its layers) */
    /* the screen's */
    UiNote uiq[UI_Q]; volatile uint32_t uiq_head;
    float meter[NUM_LAYERS];
    int events[NUM_LAYERS];
    /* the load guard: overruns the platform saw (from its interrupt), the ones dealt with; each effect's quiet time
     * (samples with nothing in and nothing out: past its memory's length it rests until something comes) */
    volatile uint32_t shed; uint32_t shed_seen; float shed_t;
    volatile float beat_adj;             /* following a clock: how far our beat is off the leader's, taken up gently */
    volatile int sync_req;               /* a Start (in or out): the top of the bar, every sequencer's step on the beat */
    int q_echo, q_gr, q_rv, q_sh;
};

void engine_init(Engine *e, const Piece *p);
void engine_key(Engine *e, int on, int note, int vel, int layer);   /* the keys and MIDI in: a note on a layer's sound (-1: the chosen layer's) */
/* an app block of nsub control steps in three parts, as The Outside Inside's: control (one core: the sequencers, the
 * conductor, MIDI out), the layers (any layer on either core), the mix and the effects (one core, after both) */
void engine_block_begin(Engine *e, int nsub);
void engine_block_layer(Engine *e, int bus);
void engine_block_mix(Engine *e, float *outL, float *outR);
void engine_all_off(Engine *e);
void engine_overload(Engine *e);
/* the mix's stages (engine_block_mix runs them in turn): sends; then reverb (another core may take it) beside echo;
 * then finish */
void engine_mix_sends(Engine *e);
void engine_mix_reverb(Engine *e);
void engine_mix_echo(Engine *e);
void engine_mix_finish(Engine *e, float *outL, float *outR);         /* from the audio interrupt: that block nearly overran; shed at the next */          /* MIDI notes off (stop, a new piece) */

#endif

/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: scales, chords and the four layers' roles. Everything that plays is locked to the scale: a note is a
 * scale degree (any whole number, 0 the key's root, one octave every `n` degrees), never a semitone. */
#ifndef AI_MUSIC_H
#define AI_MUSIC_H

#include "dsp.h"

typedef struct { const char *name; uint8_t n; int8_t st[8]; } Scale;
#define NUM_SCALES 20
extern const Scale SCALES[NUM_SCALES];

/* a degree in a scale and key -> MIDI note */
static inline int deg_note(const Scale *s, int root, int deg)
{
    int n = s->n, o = deg >= 0 ? deg / n : -((-deg + n - 1) / n);
    return root + 12 * o + s->st[deg - o * n];
}
/* the degree nearest a MIDI note (for MIDI in and the keys' chords): the scale's own notes only */
int note_deg(const Scale *s, int root, int note);

/* the black keys' chords and the BODY layer's voicings, built from the scale's own notes */
enum { CH_TRIAD, CH_SEVENTH, CH_NINTH, CH_SUS, CH_SPREAD, CH_FIFTHS, NUM_CHORDS };
extern const char *const CHORD_NAME[NUM_CHORDS];
int chord_degs(int type, int deg, int *out);   /* the chord's degrees (3 to 5 of them) */

/* the layers: each its own band and role, so the four fit together */
#define NUM_LAYERS 4
enum { L_GROUND, L_BODY, L_LIGHT, L_DUST };
extern const char *const LAYER_NAME[NUM_LAYERS];
typedef struct {
    int8_t base;                         /* the role's C (a multiple of 12): the key's root is this plus the key */
    int8_t span;                         /* degrees a sequencer's range covers */
    float hp, lp;                        /* the band: the bus's high-pass and low-pass (Hz) */
    float rv, echo, grain;               /* sends: reverb, echo, grains */
    float width;                         /* stereo spread of its voices */
    int poly;                            /* voices */
} Role;
extern const Role ROLES[NUM_LAYERS];

#endif

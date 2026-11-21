/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: scales, chords, roles. */
#include "music.h"

const Scale SCALES[NUM_SCALES] = {
    { "MAJOR",       7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "MINOR",       7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "DORIAN",      7, { 0, 2, 3, 5, 7, 9, 10 } },
    { "PHRYGIAN",    7, { 0, 1, 3, 5, 7, 8, 10 } },
    { "LYDIAN",      7, { 0, 2, 4, 6, 7, 9, 11 } },
    { "MIXOLYDIAN",  7, { 0, 2, 4, 5, 7, 9, 10 } },
    { "AEOLIAN B5",  7, { 0, 2, 3, 5, 6, 8, 10 } },
    { "HARM MINOR",  7, { 0, 2, 3, 5, 7, 8, 11 } },
    { "MEL MINOR",   7, { 0, 2, 3, 5, 7, 9, 11 } },
    { "LYD DOM",     7, { 0, 2, 4, 6, 7, 9, 10 } },
    { "PHRYG DOM",   7, { 0, 1, 4, 5, 7, 8, 10 } },
    { "HUNGARIAN",   7, { 0, 2, 3, 6, 7, 8, 11 } },
    { "DBL HARM",    7, { 0, 1, 4, 5, 7, 8, 11 } },
    { "MAJ PENTA",   5, { 0, 2, 4, 7, 9 } },
    { "MIN PENTA",   5, { 0, 3, 5, 7, 10 } },
    { "SUSPENDED",   5, { 0, 2, 5, 7, 10 } },
    { "HIRAJOSHI",   5, { 0, 2, 3, 7, 8 } },
    { "IN SEN",      5, { 0, 1, 5, 7, 10 } },
    { "WHOLE TONE",  6, { 0, 2, 4, 6, 8, 10 } },
    { "HEXATONIC",   6, { 0, 2, 3, 5, 7, 10 } },
};

int note_deg(const Scale *s, int root, int note)
{
    int best = 0, bd = 1000;
    int d0 = ((note - root) * s->n) / 12 - s->n;
    for (int d = d0; d <= d0 + 2 * s->n; d++) {
        int x = deg_note(s, root, d) - note, a = x < 0 ? -x : x;
        if (a < bd || (a == bd && x < 0)) { bd = a; best = d; }
    }
    return best;
}

const char *const CHORD_NAME[NUM_CHORDS] = { "TRIADS", "SEVENTHS", "NINTHS", "SUS", "SPREAD", "FIFTHS" };

int chord_degs(int type, int d, int *o)
{
    switch (type) {
    default:
    case CH_TRIAD:   o[0] = d; o[1] = d + 2; o[2] = d + 4; return 3;
    case CH_SEVENTH: o[0] = d; o[1] = d + 2; o[2] = d + 4; o[3] = d + 6; return 4;
    case CH_NINTH:   o[0] = d; o[1] = d + 4; o[2] = d + 6; o[3] = d + 8; o[4] = d + 9; return 5;   /* Detroit's: no third low, the 9th on top */
    case CH_SUS:     o[0] = d; o[1] = d + 1; o[2] = d + 4; o[3] = d + 7; return 4;
    case CH_SPREAD:  o[0] = d - 7; o[1] = d; o[2] = d + 4; o[3] = d + 9; return 4;
    case CH_FIFTHS:  o[0] = d; o[1] = d + 4; o[2] = d + 8; o[3] = d + 12; return 4;
    }
}

const char *const LAYER_NAME[NUM_LAYERS] = { "GROUND", "BODY", "LIGHT", "DUST" };

/*                    base span  hp      lp       rv    echo  grain width poly */
const Role ROLES[NUM_LAYERS] = {
    { 36,  5,   28.0f,   420.0f,   0.10f, 0.00f, 0.00f, 0.15f, 2 },   /* GROUND: sub and bass, the weight */
    { 48,  7,  110.0f,  2400.0f,   0.45f, 0.12f, 0.35f, 0.70f, 6 },   /* BODY: chords, the low mids */
    { 60, 10,  260.0f,  5600.0f,   0.35f, 0.40f, 0.30f, 0.55f, 3 },   /* LIGHT: the melody */
    { 72, 12, 1200.0f,  9500.0f,   0.70f, 0.30f, 0.60f, 1.00f, 4 },   /* DUST: the air */
};

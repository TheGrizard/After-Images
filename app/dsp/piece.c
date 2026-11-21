/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: a piece's settings from a seed (REC), its defaults, its name. */
#include "engine.h"

/* the engines that suit each role's band */
static const uint8_t POOL_G[] = { E_SUB, E_FM, E_PULSE, E_FOLD, E_SAW, E_SUB };
static const uint8_t POOL_B[] = { E_SAW, E_GLASS, E_VOX, E_FM, E_PULSE, E_BREATH, E_PLUCK, E_SAW };
static const uint8_t POOL_L[] = { E_PLUCK, E_BELL, E_FM, E_PULSE, E_GLASS, E_FOLD, E_VOX };
static const uint8_t POOL_D[] = { E_BREATH, E_BELL, E_GLASS, E_PLUCK, E_VOX, E_FM };
static const uint8_t *const POOL[NUM_LAYERS] = { POOL_G, POOL_B, POOL_L, POOL_D };
static const uint8_t POOL_N[NUM_LAYERS] = { sizeof POOL_G, sizeof POOL_B, sizeof POOL_L, sizeof POOL_D };
/* each role's rates (RATES' indexes) and envelopes (attack, release ranges, 0..100) */
static const uint8_t RATE_LO[NUM_LAYERS] = { 0, 0, 2, 3 }, RATE_HI[NUM_LAYERS] = { 3, 3, 7, 8 };
static const uint8_t ENV_R[NUM_LAYERS][4] = { { 0, 25, 25, 55 }, { 30, 80, 50, 90 }, { 0, 30, 30, 70 }, { 10, 60, 40, 90 } };

static uint8_t pick(Rng *r, int lo, int hi) { return (uint8_t)(lo + rng_int(r, hi - lo + 1)); }

static void roll_layer(Piece *p, int l, Rng *r, int weight)
{
    LayerP *lp = &p->l[l];
    lp->engine = POOL[l][rng_int(r, POOL_N[l])];
    lp->tone = pick(r, 25, 80);
    lp->weight = (uint8_t)weight;
    lp->weird = rng_chance(r, 0.25f) ? pick(r, 40, 85) : pick(r, 0, 35);
    lp->density = pick(r, 30, 70);
    lp->rate = pick(r, RATE_LO[l], RATE_HI[l]);
    lp->mutate = pick(r, 10, 60);
    lp->attack = pick(r, ENV_R[l][0], ENV_R[l][1]);
    lp->release = pick(r, ENV_R[l][2], ENV_R[l][3]);
    lp->level = l == L_GROUND ? 70 : l == L_BODY ? 72 : l == L_LIGHT ? 64 : 56;
    lp->octave = 0;
    lp->salt = (uint16_t)rng_u32(r);
}

void piece_roll(Piece *p, uint32_t seed, int all)
{
    Rng r; rng_seed(&r, seed);
    p->seed = seed;
    /* the sequencers: four different ones, shuffled across the layers */
    uint8_t s[NUM_SEQS];
    for (int i = 0; i < NUM_SEQS; i++) s[i] = (uint8_t)i;
    for (int i = NUM_SEQS - 1; i > 0; i--) { int j = rng_int(&r, i + 1); uint8_t t = s[i]; s[i] = s[j]; s[j] = t; }
    /* contrast: one layer heavy, one soft, always */
    int heavy = rng_chance(&r, 0.5f) ? L_GROUND : rng_int(&r, NUM_LAYERS), soft = (heavy + 1 + rng_int(&r, NUM_LAYERS - 1)) % NUM_LAYERS;
    for (int l = 0; l < NUM_LAYERS; l++) {
        int w = l == heavy ? pick(&r, 65, 95) : l == soft ? pick(&r, 0, 25) : pick(&r, 20, 65);
        roll_layer(p, l, &r, w);
        p->l[l].seq = s[l];
    }
    p->reverb = pick(&r, 35, 80); p->echo = pick(&r, 10, 55); p->grain = rng_chance(&r, 0.5f) ? pick(&r, 10, 60) : 0;
    p->shimmer = rng_chance(&r, 0.4f) ? pick(&r, 10, 50) : 0;
    p->link = pick(&r, 15, 60); p->warp = pick(&r, 0, 40); p->drift = pick(&r, 20, 60); p->chaos = pick(&r, 0, 30);
    if (all) {
        p->key = (uint8_t)rng_int(&r, 12);
        static const uint8_t FAV[10] = { 1, 2, 0, 4, 13, 14, 15, 16, 8, 5 };   /* the gentler scales come up more */
        p->scale = rng_chance(&r, 0.7f) ? FAV[rng_int(&r, 10)] : (uint8_t)rng_int(&r, NUM_SCALES);
        p->tempo = pick(&r, 20, 90);
        p->mood = pick(&r, 20, 85); p->motion = pick(&r, 30, 70); p->evolve = pick(&r, 25, 70);
        p->weird = rng_chance(&r, 0.3f) ? pick(&r, 40, 80) : pick(&r, 0, 30);
    }
}

void piece_roll_layer(Piece *p, int l, uint32_t seed)
{
    Rng r; rng_seed(&r, seed);
    int other = p->l[(l + 1) % NUM_LAYERS].seq, t;
    do t = rng_int(&r, NUM_SEQS); while (t == p->l[l].seq || t == other);
    roll_layer(p, l, &r, pick(&r, 0, 95));
    p->l[l].seq = (uint8_t)t;
}

void piece_default(Piece *p)
{
    memset(p, 0, sizeof *p);
    p->key = 9; p->scale = 1; p->chord = CH_SEVENTH; p->tempo = 44;   /* A minor, 84 BPM */
    p->mood = 55; p->motion = 50; p->evolve = 45; p->weird = 15;
    piece_roll(p, 0xAF7E1u, 0);
}

int piece_ok(const Piece *p)
{
    if (p->scale >= NUM_SCALES || p->key >= 12 || p->chord >= NUM_CHORDS || p->tempo > 140) return 0;
    const uint8_t *v = &p->mood;
    for (int i = 0; i < 12; i++) if (v[i] > 100) return 0;
    for (int l = 0; l < NUM_LAYERS; l++) {
        const LayerP *x = &p->l[l];
        if (x->engine >= NUM_ENGINES || x->seq >= NUM_SEQS || x->rate >= NUM_RATES || x->octave < -2 || x->octave > 2) return 0;
        if (x->tone > 100 || x->weight > 100 || x->weird > 100 || x->density > 100 || x->mutate > 100 || x->level > 100
            || x->attack > 100 || x->release > 100) return 0;
    }
    return 1;
}

static const char *const W1[32] = {
    "GLASS", "SLOW", "PALE", "HOLLOW", "BURIED", "SILVER", "WINTER", "DISTANT", "FOLDED", "QUIET", "AMBER", "BLUE",
    "DRIFTING", "LOW", "SALT", "IRON", "PAPER", "STILL", "LATE", "NORTH", "WIRE", "SOFT", "BROKEN", "INNER",
    "FALSE", "MOSS", "ECHO", "SHADOW", "LUNAR", "COPPER", "VELVET", "STATIC"
};
static const char *const W2[32] = {
    "HERON", "TIDE", "ORBIT", "LANTERN", "FIELD", "SIGNAL", "MIRROR", "RIVER", "ENGINE", "CHAPEL", "HARBOUR", "VAPOUR",
    "SPIRAL", "PRISM", "MERIDIAN", "GARDEN", "CIRCUIT", "WAVE", "CINDER", "HALO", "LATTICE", "FOUNTAIN", "COMET", "THREAD",
    "MACHINE", "CANYON", "BELL", "ROOM", "DUNE", "SWARM", "ARCHIVE", "CURRENT"
};

void piece_name(uint32_t seed, char *out)
{
    Rng r; rng_seed(&r, seed ^ 0x4E414D45u);
    const char *a = W1[rng_int(&r, 32)], *b = W2[rng_int(&r, 32)];
    while (*a) *out++ = *a++;
    *out++ = ' ';
    while (*b) *out++ = *b++;
    *out = 0;
}

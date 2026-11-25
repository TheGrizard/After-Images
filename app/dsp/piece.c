/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: a piece's settings from a seed (REC), its defaults, its name. */
#include "engine.h"

/* the engines that suit each role's band */
static const uint8_t POOL_G[] = { E_SUB, E_FM, E_PULSE, E_FOLD, E_SAW, E_SUB, E_TRI, E_SINE, E_SQUARE, E_CZ, E_SYNC, E_BOARDS, E_REESE, E_REESE, E_ORGAN, E_WAVES, E_LADDER, E_LADDER };
static const uint8_t POOL_B[] = { E_SAW, E_STRINGS, E_VOX, E_EPIANO, E_PULSE, E_BREATH, E_STRINGS, E_SAW, E_TRI, E_SQUARE, E_CZ, E_SHAPES, E_BOARDS, E_BOARDS, E_REESE, E_ORGAN, E_ORGAN, E_CHOIR, E_CHOIR, E_WAVES, E_BOWL, E_JUNO, E_JUNO, E_BRASS, E_BOWED };
static const uint8_t POOL_L[] = { E_PLUCK, E_DX, E_EPIANO, E_PULSE, E_DX, E_FOLD, E_VOX, E_SAW, E_FM, E_TRI, E_SINE, E_CZ, E_SYNC, E_SHAPES, E_BOARDS, E_MALLET, E_MALLET, E_BOWL, E_WAVES, E_CHOIR, E_JUNO, E_BRASS, E_BOWED };
static const uint8_t POOL_D[] = { E_BREATH, E_STRINGS, E_DX, E_PLUCK, E_VOX, E_DX, E_BREATH, E_GLASS, E_SINE, E_SHAPES, E_BOWL, E_BOWL, E_MALLET, E_CHOIR, E_WAVES, E_RUMBLE };
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
    lp->tone = rng_chance(r, 0.8f) ? pick(r, 42, 58) : rng_chance(r, 0.6f) ? pick(r, 18, 38) : pick(r, 62, 80);   /* (TONE: 50 open)*/
    if (l == L_GROUND && lp->tone < 34) lp->tone = (uint8_t)(34 + lp->tone % 8);   /* (the bass never filtered to a murmur) */
    if (l >= L_LIGHT && lp->tone > 70) lp->tone = (uint8_t)(64 + lp->tone % 7);    /* (nor the top thinned to a hiss) */
    lp->weight = (uint8_t)weight;
    lp->weird = rng_chance(r, 0.2f) ? pick(r, 40, 72) : pick(r, 0, 35);   /* (ENTROPY high now and then, never right up) */
    lp->density = pick(r, 30, 70);
    lp->rate = pick(r, RATE_LO[l], RATE_HI[l]);
    lp->mutate = pick(r, 10, 60);
    lp->attack = pick(r, ENV_R[l][0], ENV_R[l][1]);
    lp->release = pick(r, ENV_R[l][2], ENV_R[l][3]);
    lp->level = l == L_GROUND ? 82 : l == L_BODY ? 66 : l == L_LIGHT ? 56 : 40;
    lp->octave = 0;
    lp->salt = (uint16_t)rng_u32(r);
}

static int clampi_(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* each role's sends, as the mix had them before there were sends: reverb, echo, chorus */
static const uint8_t SEND_DEF[NUM_LAYERS][3] = { { 15, 0, 0 }, { 60, 15, 45 }, { 45, 50, 40 }, { 85, 40, 75 } };

void piece_sends_default(Piece *p)
{
    for (int l = 0; l < NUM_LAYERS; l++) {
        p->s_rv[l] = SEND_DEF[l][0]; p->s_echo[l] = SEND_DEF[l][1]; p->s_grain[l] = SEND_DEF[l][2];
        p->s_dirt[l] = p->dirt ? 100 : 0;        /* (v0.2's DIRT was on everything) */
    }
}

static void roll_sends(Piece *p, int l, Rng *r)   /* near the role's, now and then far from it */
{
    int far = rng_chance(r, 0.25f);
    p->s_rv[l] = (uint8_t)clampi_(SEND_DEF[l][0] + (far ? rng_int(r, 81) - 40 : rng_int(r, 31) - 15), 0, 100);
    p->s_echo[l] = (uint8_t)clampi_(SEND_DEF[l][1] + (far ? rng_int(r, 81) - 30 : rng_int(r, 21) - 10), 0, 100);
    p->s_grain[l] = (uint8_t)clampi_(SEND_DEF[l][2] + (far ? rng_int(r, 81) - 40 : rng_int(r, 31) - 15), 0, 100);
    p->s_dirt[l] = 0;
}

static void roll_voice(Piece *p, int l, Rng *r)    /* the VOICE page: now and then a glide, legato, a sweep, resonance */
{
    int slides = l == L_GROUND || l == L_LIGHT;
    p->glide[l] = rng_chance(r, slides ? 0.4f : 0.12f) ? pick(r, 10, 55) : 0;
    p->legato[l] = (uint8_t)(p->glide[l] && l != L_BODY && rng_chance(r, 0.55f));
    p->sweep[l] = rng_chance(r, 0.3f) ? pick(r, 20, 70) : 0;
    p->reso[l] = rng_chance(r, 0.25f) ? pick(r, 15, 60) : 0;
    if (p->l[l].engine == E_REESE) {             /* a REESE: legato, its filter gliding after each note */
        p->legato[l] = 1;
        if (!p->glide[l]) p->glide[l] = pick(r, 25, 60);
    }
}

const char *const FX_NAME[NUM_FX] = { "SHIMMER", "REVERSE ECHO", "CHORUS", "ENSEMBLE", "SATURATED", "TAPE FUZZ", "WIDE SPACE", "HAZE" };

/* the scales a new piece is dealt: mostly minor and consonant, for ambient; the harsh ones (AEOLIAN B5, LYD DOM, PHRYG
 * DOM, HUNGARIAN, DBL HARM, IN SEN, WHOLE TONE) only by hand */
static const struct { uint8_t scale, w; } DEAL[] = {
    { 1, 16 }, { 2, 16 }, { 14, 12 }, { 19, 10 }, { 15, 8 }, { 4, 7 }, { 8, 6 }, { 16, 5 }, { 13, 5 },
    { 0, 4 }, { 5, 4 }, { 3, 4 }, { 7, 3 } };       /* MINOR, DORIAN, MIN PENTA, HEXATONIC, SUSPENDED, LYDIAN, MEL MINOR,
                                                      * HIRAJOSHI, MAJ PENTA, MAJOR, MIXOLYDIAN, PHRYGIAN, HARM MINOR */
#define NDEAL ((int)(sizeof DEAL / sizeof DEAL[0]))
/* minor (a minor third over the root) or not, each scale */
static const uint8_t SCALE_MINOR[NUM_SCALES] = { 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1 };
static int DEALT(int scale) { for (int i = 0; i < NDEAL; i++) if (DEAL[i].scale == scale) return 1; return 0; }
static int deal_scale(Rng *r, int minor)          /* minor: -1 any, 1 a minor one, 0 a major one */
{
    int tot = 0;
    for (int i = 0; i < NDEAL; i++) if (minor < 0 || SCALE_MINOR[DEAL[i].scale] == minor) tot += DEAL[i].w;
    int x = rng_int(r, tot);
    for (int i = 0; i < NDEAL; i++) {
        if (minor >= 0 && SCALE_MINOR[DEAL[i].scale] != minor) continue;
        if (x < DEAL[i].w) return DEAL[i].scale;
        x -= DEAL[i].w;
    }
    return 1;
}

void piece_roll(Piece *p, uint32_t seed, int all, int fx)
{
    Rng r; rng_seed(&r, seed);
    p->seed = seed;
    /* the sequencers: four different ones, shuffled across the layers */
    uint8_t s[NUM_SEQS];
    for (int i = 0; i < NUM_SEQS; i++) s[i] = (uint8_t)i;
    for (int i = NUM_SEQS - 1; i > 0; i--) { int j = rng_int(&r, i + 1); uint8_t t = s[i]; s[i] = s[j]; s[j] = t; }
    /* contrast: one layer heavy, one soft, always */
    int heavy = rng_chance(&r, 0.5f) ? L_GROUND : rng_int(&r, NUM_LAYERS), soft = (heavy + 1 + rng_int(&r, NUM_LAYERS - 1)) % NUM_LAYERS;
    if (soft == L_GROUND) soft = heavy == L_DUST ? L_LIGHT : L_DUST;   /* (never the bass: a soft one is a thin, bright piece) */
    /* the long melodies: LIGHT sings one in half the pieces (the early-90s Warp way); now and then GROUND does instead,
     * a melody down in the bass */
    if (rng_chance(&r, 0.5f)) {
        int at = 0; while (at < NUM_SEQS && s[at] != S_MELODY) at++;
        int to = rng_chance(&r, 0.2f) ? L_GROUND : L_LIGHT;
        if (at < NUM_SEQS) { uint8_t t = s[to]; s[to] = s[at]; s[at] = t; }
    }
    /* the bass not on the sparse ones: CHIMES and BOUNCE leave the ground near silent (whole pieces with no bass, thin
     * and bright); swapped with the first layer above that has neither */
    if (s[L_GROUND] == S_CHIMES || s[L_GROUND] == S_BOUNCE)
        for (int l = 1; l < NUM_LAYERS; l++)
            if (s[l] != S_CHIMES && s[l] != S_BOUNCE && !(s[l] == S_MELODY && l == L_LIGHT)) { uint8_t t = s[l]; s[l] = s[L_GROUND]; s[L_GROUND] = t; break; }
    for (int l = 0; l < NUM_LAYERS; l++) {
        int w = l == heavy ? pick(&r, 65, 95) : l == soft ? pick(&r, 0, 25) : pick(&r, 20, 65);
        roll_layer(p, l, &r, w);
        roll_voice(p, l, &r);
        roll_sends(p, l, &r);
        p->l[l].seq = s[l];
        if (l == L_LIGHT && rng_chance(&r, 0.3f)) p->l[l].octave = -1;   /* (a lower melody, now and then) */
    }
    /* the effects: each modest, then one featured (REVERB and ECHO past 50 are the shimmer and the reverse smear) */
    p->reverb = pick(&r, 25, 48); p->echo = pick(&r, 8, 40); p->grain = rng_chance(&r, 0.4f) ? pick(&r, 10, 35) : 0;
    /* WARBLE: a little of the worn-tape wander in most pieces, a lot in HAZE's */
    p->warble = rng_chance(&r, 0.6f) ? pick(&r, 5, 35) : 0; p->flutter = rng_chance(&r, 0.3f) ? pick(&r, 5, 25) : 0;
    p->detune = rng_chance(&r, 0.35f) ? pick(&r, 10, 45) : 0; p->wear = rng_chance(&r, 0.3f) ? pick(&r, 10, 40) : 0;
    p->dirt = 0;
    if (fx < 0 || fx >= NUM_FX) fx = rng_int(&r, NUM_FX);
    switch (fx) {
    case FX_SHIMMER: p->reverb = pick(&r, 66, 96); break;
    case FX_REVERSE: p->echo = pick(&r, 62, 95); break;
    case FX_CHORUS:  p->grain = pick(&r, 35, 50); break;
    case FX_ENSEMBLE: p->grain = pick(&r, 65, 95); break;
    case FX_FOLD:    p->dirt = pick(&r, 5, 32); break;
    case FX_CRUSH:   p->dirt = pick(&r, 36, 56); break;
    case FX_HAZE:    /* an old VHS synth's: the pitch wandering, worn, chorused, a touch flat, dark, echoed */
        p->warble = pick(&r, 45, 85); p->flutter = pick(&r, 10, 35); p->detune = pick(&r, 30, 65); p->wear = pick(&r, 35, 70);
        p->grain = pick(&r, 30, 50);                 /* (the chorus on all but the bass) */
        for (int l = 0; l < NUM_LAYERS; l++) p->s_grain[l] = l == L_GROUND ? 0 : pick(&r, 45, 85);
        p->echo = pick(&r, 25, 45);
        for (int l = 0; l < NUM_LAYERS; l++) p->l[l].tone = (uint8_t)clampi_(p->l[l].tone - 8, 0, 100);
        if (rng_chance(&r, 0.6f)) p->l[L_BODY].engine = E_BOARDS;
        if (rng_chance(&r, 0.5f)) p->l[L_LIGHT].engine = E_BOARDS;
        break;
    default:         p->reverb = pick(&r, 46, 50); p->echo = pick(&r, 35, 50); break;   /* WIDE SPACE: all reverb, all echo */
    }
    if (fx == FX_FOLD || fx == FX_CRUSH) {           /* the dirt on one or two layers, never the whole */
        int a = rng_int(&r, NUM_LAYERS), b = rng_chance(&r, 0.4f) ? (a + 1 + rng_int(&r, NUM_LAYERS - 1)) % NUM_LAYERS : a;
        p->s_dirt[a] = pick(&r, 55, 100); p->s_dirt[b] = pick(&r, 40, 100);
    }
    if (fx == FX_SHIMMER) p->s_rv[L_LIGHT] = (uint8_t)clampi_(p->s_rv[L_LIGHT] + 25, 0, 100);
    if (fx == FX_REVERSE) p->s_echo[L_LIGHT] = (uint8_t)clampi_(p->s_echo[L_LIGHT] + 30, 0, 100);
    p->link = pick(&r, 15, 60); p->warp = pick(&r, 0, 40); p->drift = pick(&r, 20, 60); p->chaos = pick(&r, 0, 30);
    p->tempo = pick(&r, 25, 70);                     /* every new piece its own tempo: 65 to 110 BPM */
    p->key = (uint8_t)rng_int(&r, 12); p->scale = deal_scale(&r, -1);   /* (REC: piece_next_key, from the last) */
    if (all) {
        p->mood = pick(&r, 20, 85); p->motion = pick(&r, 30, 70); p->evolve = pick(&r, 25, 70);
        p->weird = pick(&r, 20, 70);                 /* DRIFT: the layers wandering in and out */
    }
    if (!p->grain) for (int l = 0; l < NUM_LAYERS; l++) p->s_grain[l] = 0;   /* (no chorus in the piece: none sent; a send alone wakes it) */
}

/* the next piece's key, from the last one's round the circle of fifths, so the pieces lead on from one another:
 * up a fifth 35 %, down a fifth 30 % (the same scale mostly: one sharp or flat away), the relative major or minor
 * 15 %, the same root in another mode 20 % */
void piece_next_key(Piece *p, int key, int scale, uint32_t seed)
{
    Rng r; rng_seed(&r, seed ^ 0xF1F7Eu);
    int x = rng_int(&r, 100), minor = SCALE_MINOR[scale % NUM_SCALES];
    if (x < 65) {
        p->key = (uint8_t)((key + (x < 35 ? 7 : 5)) % 12);
        p->scale = (uint8_t)(rng_chance(&r, 0.65f) && DEALT(scale) ? scale : deal_scale(&r, -1));
    } else if (x < 80) {                         /* relative: a minor's major a minor third up, a major's minor below */
        p->key = (uint8_t)((key + (minor ? 3 : 9)) % 12);
        p->scale = deal_scale(&r, minor ? 0 : 1);
    } else {
        p->key = (uint8_t)key;
        int s; do s = deal_scale(&r, -1); while (s == scale);
        p->scale = (uint8_t)s;
    }
}

void piece_roll_layer(Piece *p, int l, uint32_t seed)
{
    Rng r; rng_seed(&r, seed);
    int other = p->l[(l + 1) % NUM_LAYERS].seq, t;
    do t = rng_int(&r, NUM_SEQS); while (t == p->l[l].seq || t == other);
    roll_layer(p, l, &r, pick(&r, 0, 95));
    roll_voice(p, l, &r);
    roll_sends(p, l, &r);
    if (p->dirt && rng_chance(&r, 0.3f)) p->s_dirt[l] = pick(&r, 30, 90);
    p->l[l].seq = (uint8_t)t;
    if (!p->grain) p->s_grain[l] = 0;
}

void piece_default(Piece *p)
{
    memset(p, 0, sizeof *p);
    p->chord = CH_SEVENTH; p->tempo = 44;
    p->mood = 55; p->motion = 50; p->evolve = 45; p->weird = 35;
    piece_roll(p, 0xAF7E1u, 0, FX_SPACE);
    p->key = 9; p->scale = 1;                        /* (the first piece: A minor) */
    p->tempo = 44;                                   /* (the first piece: 84 BPM) */
    p->l[L_LIGHT].octave = 0;
}

int piece_ok(const Piece *p)
{
    if (p->scale >= NUM_SCALES || p->key >= 12 || p->chord >= NUM_CHORDS || p->tempo > 140 || p->dirt > 100) return 0;
    if (p->warble > 100 || p->flutter > 100 || p->detune > 100 || p->wear > 100) return 0;
    const uint8_t *v = &p->mood;
    for (int i = 0; i < 12; i++) if (v[i] > 100) return 0;
    for (int l = 0; l < NUM_LAYERS; l++) {
        const LayerP *x = &p->l[l];
        if (x->engine >= NUM_ENGINES || x->seq >= NUM_SEQS || x->rate >= NUM_RATES || x->octave < -2 || x->octave > 2) return 0;
        if (p->glide[l] > 100 || p->legato[l] > 1 || p->sweep[l] > 100 || p->reso[l] > 100) return 0;
        if (p->s_rv[l] > 100 || p->s_echo[l] > 100 || p->s_grain[l] > 100 || p->s_dirt[l] > 100) return 0;
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

/* the name: from the seed, one of five kinds. Two words (GLASS HERON), one word (LATTICE), a made-up word from syllables
 * (VEKTRAL), one chopped from two words or stripped of its vowels, sometimes with a number (COPPRAL, LTTC 4, HERON-21),
 * or a made-up word with a number (QUELLAX 7) */
static const char *const ON[20] = { "V", "TR", "GL", "SK", "PL", "DR", "CR", "K", "X", "QU", "ST", "BL", "ZH", "N", "R", "M",
                                     "F", "TH", "GR", "KR" };
static const char *const VO[10] = { "A", "E", "I", "O", "U", "AE", "IA", "OU", "Y", "EI" };
static const char *const CO[16] = { "N", "X", "RK", "LL", "ST", "MT", "ND", "P", "", "", "R", "KS", "TCH", "PH", "L", "SM" };

static char *put(char *o, const char *s, int max) { for (int i = 0; s[i] && i < max; i++) *o++ = s[i]; *o = 0; return o; }
static char *num(char *o, int v) { char t[6]; int n = 0; do t[n++] = (char)('0' + v % 10); while ((v /= 10) && n < 5); while (n) *o++ = t[--n]; *o = 0; return o; }

void piece_name(uint32_t seed, char *out)
{
    Rng r; rng_seed(&r, seed ^ 0x4E414D45u);
    const char *a = W1[rng_int(&r, 32)], *b = W2[rng_int(&r, 32)];
    int kind = rng_int(&r, 10);
    char *o = out; *o = 0;
    if (kind < 3) { o = put(o, a, 12); o = put(o, " ", 1); put(o, b, 12); }             /* two words */
    else if (kind < 5) put(o, rng_chance(&r, 0.5f) ? a : b, 12);                         /* one */
    else if (kind < 7 || kind == 9) {                                                       /* made up */
        int syl = 2 + rng_int(&r, 2);
        for (int i = 0; i < syl; i++) { o = put(o, ON[rng_int(&r, 20)], 3); o = put(o, VO[rng_int(&r, 10)], 2); if (i == syl - 1 || rng_chance(&r, 0.3f)) o = put(o, CO[rng_int(&r, 16)], 3); }
        if (kind == 9) { o = put(o, " ", 1); num(o, 1 + rng_int(&r, 99)); }
    } else {                                                                                /* chopped */
        int la = 0, lb = 0; while (a[la]) la++; while (b[lb]) lb++;
        if (rng_chance(&r, 0.5f)) {                     /* the front of one, the back of the other */
            int ca = 2 + rng_int(&r, la - 2 > 1 ? la - 2 : 1), cb = rng_int(&r, lb - 2 > 1 ? lb - 2 : 1);
            o = put(o, a, ca); o = put(o, b + cb, 12);
        } else {                                        /* a word without its vowels */
            const char *w = rng_chance(&r, 0.5f) ? a : b;
            *o++ = *w;
            for (const char *c = w + 1; *c; c++) if (*c != 'A' && *c != 'E' && *c != 'I' && *c != 'O' && *c != 'U') *o++ = *c;
            *o = 0;
        }
        if (rng_chance(&r, 0.5f)) { o = put(o, rng_chance(&r, 0.5f) ? " " : "-", 1); num(o, 1 + rng_int(&r, rng_chance(&r, 0.5f) ? 9 : 99)); }
    }
}

/* ------------------------------------------------------------------ PERSONALITY: what REC favours */
/* ANALYST: the roll as it is. SENTINEL: dark, droning, steady. DIPLOMAT: distant, ethereal, soft. EXPLORER: melodic,
 * quicker, arpeggios. Each re-deals the engines, sequencers, rates, envelopes, tempo, scale and macros of a piece
 * piece_roll made, from its own pools and ranges; the rest (the contrast, the voices, the sends' scatter) kept */
typedef struct {
    const uint8_t *eng[NUM_LAYERS]; uint8_t eng_n[NUM_LAYERS];
    const uint8_t *seq[NUM_LAYERS]; uint8_t seq_n[NUM_LAYERS];
    uint8_t rate[NUM_LAYERS][2], att[NUM_LAYERS][2], rel[NUM_LAYERS][2], dens[2], level[NUM_LAYERS];
    int8_t tone, rv, echo;                       /* TONE moved by; the reverb and echo sends moved by */
    uint8_t tempo[2], mood[2], motion[2], evolve[2], drift[2];
    const uint8_t *scales; uint8_t scales_n; const uint8_t *fx; uint8_t fx_n;
    uint8_t melody;                              /* the chance (%) LIGHT sings a MELODY */
} Persona;
#define N_(a) (uint8_t)(sizeof a / sizeof a[0])

static const uint8_t SE_G[] = { E_REESE, E_FOLD, E_SAW, E_ORGAN, E_ORGAN, E_SHAPES, E_WAVES, E_SUB, E_LADDER };
static const uint8_t SE_B[] = { E_SAW, E_STRINGS, E_STRINGS, E_ORGAN, E_BOARDS, E_WAVES, E_CHOIR, E_BOWL, E_REESE, E_BOWED, E_BOWED, E_BRASS };
static const uint8_t SE_L[] = { E_BOWL, E_WAVES, E_SHAPES, E_BREATH, E_FOLD, E_CHOIR, E_STRINGS, E_ORGAN, E_BOWED, E_RUMBLE };
static const uint8_t SE_D[] = { E_BREATH, E_RUMBLE, E_RUMBLE, E_SHAPES, E_BOWL, E_CHOIR, E_GLASS };
static const uint8_t SS_G[] = { S_LORENZ, S_EUCLID, S_TURING, S_DRUNK }, SS_B[] = { S_LORENZ, S_ORBITS, S_MARKOV, S_DRUNK };
static const uint8_t SS_L[] = { S_ORBITS, S_LORENZ, S_CHIMES, S_DRUNK }, SS_D[] = { S_CHIMES, S_LIFE, S_BOUNCE };
static const uint8_t SSC[] = { 1, 1, 3, 3, 3, 6, 7, 11, 16, 17 }, SFX[] = { FX_CRUSH, FX_FOLD, FX_HAZE, FX_HAZE, FX_SPACE, FX_REVERSE };

static const uint8_t DE_G[] = { E_SINE, E_SINE, E_SUB, E_TRI, E_WAVES };
static const uint8_t DE_B[] = { E_STRINGS, E_STRINGS, E_CHOIR, E_CHOIR, E_WAVES, E_VOX, E_JUNO, E_JUNO, E_BOWED };
static const uint8_t DE_L[] = { E_CHOIR, E_WAVES, E_STRINGS, E_GLASS, E_SINE, E_TRI, E_BOWL, E_BOWED, E_JUNO };
static const uint8_t DE_D[] = { E_GLASS, E_BREATH, E_SINE, E_BOWL, E_CHOIR };
static const uint8_t DS_G[] = { S_DRUNK, S_ORBITS, S_PHASE, S_EUCLID }, DS_B[] = { S_PHASE, S_ORBITS, S_MARKOV, S_DRUNK };
static const uint8_t DS_L[] = { S_ORBITS, S_PHASE, S_DRUNK, S_CHIMES }, DS_D[] = { S_CHIMES, S_ORBITS, S_BOUNCE };
static const uint8_t DSC[] = { 0, 4, 4, 13, 13, 15, 15, 2, 19 }, DFX[] = { FX_SHIMMER, FX_SHIMMER, FX_SHIMMER, FX_SHIMMER, FX_SPACE, FX_REVERSE };

static const uint8_t EE_G[] = { E_SAW, E_SAW, E_SQUARE, E_BOARDS, E_BOARDS, E_SUB, E_LADDER, E_LADDER, E_LADDER };
static const uint8_t EE_B[] = { E_BOARDS, E_BOARDS, E_STRINGS, E_STRINGS, E_SAW, E_SQUARE, E_JUNO, E_JUNO, E_JUNO, E_BRASS, E_BRASS };
static const uint8_t EE_L[] = { E_SAW, E_SQUARE, E_SQUARE, E_BOARDS, E_PULSE, E_TRI, E_JUNO, E_JUNO, E_BRASS };
static const uint8_t EE_D[] = { E_PULSE, E_SQUARE, E_TRI, E_SAW, E_BOARDS };
static const uint8_t ES_G[] = { S_TURING, S_EUCLID, S_MARKOV }, ES_B[] = { S_EUCLID, S_TURING, S_LIFE };
static const uint8_t ES_L[] = { S_MARKOV, S_TURING }, ES_D[] = { S_TURING, S_EUCLID, S_MARKOV, S_DRUNK };
static const uint8_t ESC[] = { 2, 2, 2, 1, 1, 4, 4, 14, 19 }, EFX[] = { FX_CHORUS, FX_CHORUS, FX_ENSEMBLE, FX_ENSEMBLE, FX_ENSEMBLE };

static const Persona PERSONA[NUM_PERS - 1] = {
    {   /* SENTINEL */
        { SE_G, SE_B, SE_L, SE_D }, { N_(SE_G), N_(SE_B), N_(SE_L), N_(SE_D) },
        { SS_G, SS_B, SS_L, SS_D }, { N_(SS_G), N_(SS_B), N_(SS_L), N_(SS_D) },
        { { 0, 1 }, { 0, 1 }, { 0, 2 }, { 0, 2 } }, { { 30, 60 }, { 60, 95 }, { 50, 90 }, { 30, 70 } },
        { { 75, 100 }, { 85, 100 }, { 80, 100 }, { 70, 100 } }, { 5, 20 }, { 80, 78, 52, 38 },
        -12, 10, -12, { 5, 35 }, { 8, 25 }, { 5, 22 }, { 10, 30 }, { 15, 40 }, SSC, N_(SSC), SFX, N_(SFX), 0 },
    {   /* DIPLOMAT */
        { DE_G, DE_B, DE_L, DE_D }, { N_(DE_G), N_(DE_B), N_(DE_L), N_(DE_D) },
        { DS_G, DS_B, DS_L, DS_D }, { N_(DS_G), N_(DS_B), N_(DS_L), N_(DS_D) },
        { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 4 } }, { { 45, 75 }, { 75, 100 }, { 65, 100 }, { 65, 100 } },
        { { 85, 100 }, { 92, 100 }, { 90, 100 }, { 90, 100 } }, { 22, 42 }, { 70, 92, 80, 70 },
        -14, 40, 35, { 5, 25 }, { 65, 95 }, { 5, 25 }, { 25, 50 }, { 50, 75 }, DSC, N_(DSC), DFX, N_(DFX), 30 },
    {   /* EXPLORER */
        { EE_G, EE_B, EE_L, EE_D }, { N_(EE_G), N_(EE_B), N_(EE_L), N_(EE_D) },
        { ES_G, ES_B, ES_L, ES_D }, { N_(ES_G), N_(ES_B), N_(ES_L), N_(ES_D) },
        { { 3, 5 }, { 3, 6 }, { 3, 6 }, { 7, 9 } }, { { 0, 12 }, { 10, 35 }, { 5, 20 }, { 0, 5 } },
        { { 30, 55 }, { 50, 75 }, { 40, 65 }, { 15, 35 } }, { 45, 75 }, { 80, 60, 58, 50 },
        -10, 5, 15, { 40, 70 }, { 55, 85 }, { 45, 70 }, { 35, 60 }, { 10, 30 }, ESC, N_(ESC), EFX, N_(EFX), 90 },
};
const char *const PERS_NAME[NUM_PERS] = { "ANALYST", "SENTINEL", "DIPLOMAT", "EXPLORER" };

int piece_roll_as(Piece *p, uint32_t seed, int all, int pers)
{
    if (pers <= PERS_ANALYST || pers >= NUM_PERS) { piece_roll(p, seed, all, -1); return -1; }
    const Persona *x = &PERSONA[pers - 1];
    Rng r; rng_seed(&r, seed ^ 0x9E2A5u * (uint32_t)pers);
    int fx = x->fx[rng_int(&r, x->fx_n)];
    piece_roll(p, seed, all, fx);
    uint8_t used[NUM_LAYERS];
    for (int l = 0; l < NUM_LAYERS; l++) {
        LayerP *lp = &p->l[l];
        uint8_t c[NUM_SEQS]; int nc = 0, s;      /* four different sequencers: from the pool's still free, else any free */
        for (int pass = 0; pass < 2 && !nc; pass++)
            for (int i = 0; i < (pass ? NUM_SEQS : x->seq_n[l]); i++) {
                int q = pass ? i : x->seq[l][i], taken = 0;
                for (int k = 0; k < l; k++) taken |= used[k] == q;
                if (!taken) c[nc++] = (uint8_t)q;
            }
        s = c[rng_int(&r, nc)];
        if (l == L_LIGHT && rng_int(&r, 100) < x->melody) s = S_MELODY;
        used[l] = (uint8_t)s; lp->seq = (uint8_t)s;
        lp->engine = x->eng[l][rng_int(&r, x->eng_n[l])];
        lp->rate = pick(&r, x->rate[l][0], x->rate[l][1]);
        lp->attack = pick(&r, x->att[l][0], x->att[l][1]);
        lp->release = pick(&r, x->rel[l][0], x->rel[l][1]);
        lp->density = pick(&r, x->dens[0], x->dens[1]);
        lp->level = x->level[l];
        lp->tone = (uint8_t)clampi_(lp->tone + x->tone, l == L_GROUND ? 34 : 0, l >= L_LIGHT ? 72 : 100);
        p->s_rv[l] = (uint8_t)clampi_(p->s_rv[l] + x->rv, 0, 100);
        if (l != L_GROUND) p->s_echo[l] = (uint8_t)clampi_(p->s_echo[l] + x->echo, 0, 100);
        if (lp->engine == E_REESE) { p->legato[l] = 1; if (!p->glide[l]) p->glide[l] = pers == PERS_SENTINEL ? pick(&r, 10, 25) : pick(&r, 25, 60); }
        else if (pers == PERS_EXPLORER && l != L_GROUND) p->legato[l] = 0;   /* (arpeggios: every note struck) */
    }
    if (pers == PERS_SENTINEL) {                 /* the drone: the bass and the middle held and sliding; everything through the
                                                  * fuzz (short of the breakup's dropouts), the tape worn, the engines rough */
        for (int l = 0; l < NUM_LAYERS; l++) p->l[l].weird = pick(&r, 35, 80);
        p->legato[L_GROUND] = 1; p->glide[L_GROUND] = pick(&r, 12, 30);   /* (the bass held, a short slide: no long sweeps) */
        for (int l = L_BODY; l < NUM_LAYERS; l++) { p->legato[l] = 0; p->glide[l] = 0; }
        /* three kinds, by the effect dealt: a tape drone in the fuzz (TAPE FUZZ, SATURATED); worn and wandering (HAZE);
         * clean, dark and wide (SPACE, REVERSE ECHO) */
        if (fx == FX_CRUSH || fx == FX_FOLD) {
            p->dirt = pick(&r, 20, 32);
            static const uint8_t DS[NUM_LAYERS][2] = { { 70, 90 }, { 60, 90 }, { 40, 75 }, { 55, 85 } };
            for (int l = 0; l < NUM_LAYERS; l++) p->s_dirt[l] = pick(&r, DS[l][0], DS[l][1]);
            p->wear = pick(&r, 10, 30);
        } else if (fx == FX_HAZE) {
            p->wear = (uint8_t)clampi_(p->wear, 30, 60);
        } else {
            p->dirt = 0; for (int l = 0; l < NUM_LAYERS; l++) p->s_dirt[l] = 0;
            p->wear = pick(&r, 20, 40);
            p->reverb = (uint8_t)clampi_(p->reverb, 55, 85);
        }
        if (p->l[L_LIGHT].octave > 0) p->l[L_LIGHT].octave = 0;
        p->l[L_DUST].octave = 0;
    }
    if (pers == PERS_DIPLOMAT) {                 /* pads, far off: deep in the reverb (its shimmer), the echo long (short of
                                                  * its reverse smear but in REVERSE ECHO's pieces), the pads sliding */
        p->reverb = (uint8_t)clampi_(p->reverb, pick(&r, 75, 96), 100);
        p->wear = pick(&r, 25, 50);              /* (far off: the top dulled, the sides narrower and darker) */
        p->legato[L_GROUND] = 1; if (!p->glide[L_GROUND]) p->glide[L_GROUND] = pick(&r, 40, 80);
        if (p->echo < 50) p->echo = pick(&r, 38, 49);
        if (rng_chance(&r, 0.5f)) { p->legato[L_BODY] = 1; if (!p->glide[L_BODY]) p->glide[L_BODY] = pick(&r, 30, 70); }
        p->dirt = 0; for (int l = 0; l < NUM_LAYERS; l++) p->s_dirt[l] = 0;
    }
    if (pers == PERS_EXPLORER) {                 /* analogue and clean: no fuzz, a little drift; now and then a bass line under
                                                  * LIGHT's melody, DUST's quick line against both (counterpoint, arpeggios) */
        p->dirt = 0; for (int l = 0; l < NUM_LAYERS; l++) p->s_dirt[l] = 0;
        if (p->warble < 15) p->warble = pick(&r, 15, 40);   /* (the VHS wander, in every piece) */
        if (p->detune < 15) p->detune = pick(&r, 15, 40);
        p->wear = rng_chance(&r, 0.4f) ? pick(&r, 5, 15) : 0;
        /* smooth: ENTROPY kept low (its chaos oscillator, past halfway, is noise; DRIFT's wander can't reach it from
         * here), the filters low-pass side, little resonance or sweep; and the chorus deep on every layer */
        for (int l = 0; l < NUM_LAYERS; l++) {
            p->l[l].weird = pick(&r, 0, 15);
            if (p->reso[l] > 20) p->reso[l] = pick(&r, 0, 20);
            if (p->sweep[l] > 35) p->sweep[l] = pick(&r, 0, 35);
            if (l > L_GROUND) p->s_grain[l] = pick(&r, 70, 100);
        }
        p->s_grain[L_GROUND] = 0;                /* (never the bass: a chorus is stereo, the bass stays mono) */
        if (p->grain < 35) p->grain = pick(&r, 40, 50);
        /* the Turing machine's quick loops: DUST's arpeggio always, BODY's chords often (here a sequencer may play on two
         * layers); the bass a melody of its own in most pieces, LIGHT answering it, else LIGHT the melody */
        p->l[L_DUST].seq = S_TURING;
        p->l[L_BODY].seq = rng_chance(&r, 0.5f) ? S_TURING : rng_chance(&r, 0.5f) ? S_EUCLID : S_LIFE;
        if (rng_chance(&r, 0.6f)) { p->l[L_GROUND].seq = S_MELODY; p->l[L_LIGHT].seq = rng_chance(&r, 0.5f) ? S_MARKOV : S_TURING; }
        else { p->l[L_LIGHT].seq = S_MELODY; p->l[L_GROUND].seq = rng_chance(&r, 0.6f) ? S_TURING : S_MARKOV; }
        p->l[L_DUST].octave = rng_chance(&r, 0.3f) ? 1 : 0;
    }
    p->tempo = pick(&r, x->tempo[0], x->tempo[1]);
    p->scale = x->scales[rng_int(&r, x->scales_n)];
    if (all) {
        p->mood = pick(&r, x->mood[0], x->mood[1]); p->motion = pick(&r, x->motion[0], x->motion[1]);
        p->evolve = pick(&r, x->evolve[0], x->evolve[1]); p->weird = pick(&r, x->drift[0], x->drift[1]);
    }
    return fx;
}

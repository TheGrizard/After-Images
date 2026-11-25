/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the piece (engine.h). */
#include "engine.h"

const char *const RATE_NAME[NUM_RATES] = { "1/4", "1/3", "1/2", "2/3", "3/4", "1", "4/3", "3/2", "2", "3" };
const float RATES[NUM_RATES] = { 0.25f, 0.3333f, 0.5f, 0.6667f, 0.75f, 1.0f, 1.3333f, 1.5f, 2.0f, 3.0f };

static float u(uint8_t v) { return (float)v * 0.01f; }
static float attack_s(uint8_t v) { return 0.002f * fm_powf(2000.0f, u(v)); }     /* 2 ms .. 4 s */
static float release_s(uint8_t v) { return 0.05f * fm_powf(200.0f, u(v)); }     /* 50 ms .. 10 s */

/* the scale's degree nearest a fifth above the root (4 in a seven-note scale, 3 in a pentatonic) */
static int fifth_of(const Scale *s)
{
    int best = 1, bd = 99;
    for (int i = 1; i < s->n; i++) { int d = s->st[i] - 7; d = d < 0 ? -d : d; if (d < bd) { bd = d; best = i; } }
    return best;
}

/* ------------------------------------------------------------------ the screen, MIDI */
static void post(Engine *e, int layer, int note, float vel, float pan, float dur, int key)
{
    UiNote *q = &e->uiq[e->uiq_head % UI_Q];
    q->dur = (uint8_t)clampf(dur * 10.0f, 1.0f, 255.0f); q->layer = (uint8_t)layer; q->note = (uint8_t)note; q->vel = (uint8_t)(vel * 127.0f); q->pan = (int8_t)(pan * 100.0f); q->key = (uint8_t)key;
    e->uiq_head++;
}

static void midi_note(Engine *e, int ch, int note, float vel, float dur_s)
{
    if (!e->midi || !e->midi_on || note < 0 || note > 127) return;
    uint32_t v = (uint32_t)clampf(vel * 127.0f, 1.0f, 127.0f);
    e->midi(0x09u | (0x90u | (uint32_t)ch) << 8 | (uint32_t)note << 16 | v << 24);
    if (e->nmo >= 32) {                              /* full: the oldest goes off now */
        e->midi(0x08u | (0x80u | e->mo[0].ch) << 8 | (uint32_t)e->mo[0].note << 16);
        for (int i = 1; i < e->nmo; i++) e->mo[i - 1] = e->mo[i];
        e->nmo--;
    }
    e->mo[e->nmo].ch = (uint8_t)ch; e->mo[e->nmo].note = (uint8_t)note; e->mo[e->nmo].off = e->t + dur_s; e->nmo++;
}

static void midi_due(Engine *e)
{
    for (int i = 0; i < e->nmo; i++)
        if (e->t >= e->mo[i].off) {
            if (e->midi) e->midi(0x08u | (0x80u | e->mo[i].ch) << 8 | (uint32_t)e->mo[i].note << 16);
            e->mo[i] = e->mo[--e->nmo]; i--;
        }
}

/* each bus's ghost: a note whose voice a new one took, fading out over ~80 ms, not cut off (in the RAM beside the
 * engine's, not the pool's: the pool keeps its headroom) */
float g_band_lp = 1.0f;                          /* (the bands' low-pass scaled: the renderer's A/B; 1 on the FM-1) */
static Shifter g_shim;                           /* the shimmer's pitch shifter (8 KB: beside the engine, not in the pool) */

/* the mix's buffers, a block's (engine_mix_*: the stages two cores share) */
#define MIXN (ENGINE_SUB * BLK)
static float mx_dl[MIXN], mx_dr[MIXN], mx_rl[MIXN], mx_rr[MIXN], mx_el[MIXN], mx_er[MIXN], mx_gm[MIXN];
static float mx_vl[MIXN], mx_vr[MIXN], mx_wl[MIXN], mx_wr[MIXN];   /* the reverb's out; the echo's and the chorus's (to the dry mix) */
static float mx_rvk, mx_shk, mx_grk;
static int mx_nsub;                              /* the sends' block's length, for the reverb a block on */

void engine_all_off(Engine *e)
{
    for (int i = 0; i < e->nmo; i++) if (e->midi) e->midi(0x08u | (0x80u | e->mo[i].ch) << 8 | (uint32_t)e->mo[i].note << 16);
    e->nmo = 0;
}

/* ------------------------------------------------------------------ set-up */
/* the patch: which layer's activity each layer listens to (never its own), from the piece's seed */
static void patch(Engine *e, uint32_t seed)
{
    Rng r; rng_seed(&r, seed ^ 0xCAB1Eu);
    for (int l = 0; l < NUM_LAYERS; l++) e->L[l].src = (l + 1 + rng_int(&r, NUM_LAYERS - 1)) % NUM_LAYERS;
}

/* a knob as DRIFT has moved it: its own value, plus how far its walk has taken it (at DRIFT 0, none) */
static float mo(const Engine *e, int l, int k, uint8_t v, float reach) { return clamp01(u(v) + reach * e->L[l].mw[k]); }

static void layer_sound(const Engine *e, int l, Sound *s)
{
    const Piece *p = e->p; const LayerP *lp = &p->l[l];
    s->engine = lp->engine < NUM_ENGINES ? lp->engine : 0;
    s->tone = clamp01(0.42f + (mo(e, l, MW_TONE, lp->tone, 0.25f) - 0.5f) * 0.5f + e->L[l].tone_off + (u(p->mood) - 0.5f) * 0.35f
                      + u(p->drift) * 0.25f * e->L[l].dtone.v);
    s->weight = mo(e, l, MW_WEIGHT, lp->weight, 0.4f);
    s->weird = mo(e, l, MW_ENT, lp->weird, 0.4f);
    s->attack = attack_s(lp->attack); s->release = release_s(lp->release);
    s->glide = p->glide[l] ? 0.02f + 0.9f * u(p->glide[l]) * u(p->glide[l]) : 0.0f;
    s->sweep = u(p->sweep[l]); s->reso = u(p->reso[l]);
    s->pan_move = clamp01(0.15f * u(p->drift) + 0.45f * u(p->chaos)) * ROLES[l].width;   /* FLOW and CHAOS: more */
}

static void layer_seq_check(Engine *e, int l)
{
    Layer *L = &e->L[l]; const LayerP *lp = &e->p->l[l];
    uint32_t seed = e->p->seed ^ (0x9E3779B9u * (uint32_t)(l + 1)) ^ ((uint32_t)lp->salt << 7);
    if (L->type_now != lp->seq || L->seed_now != seed) {
        seq_init(&L->seq, lp->seq, ROLES[l].span, seed);
        L->type_now = lp->seq; L->seed_now = seed;
    }
}

void engine_init(Engine *e, const Piece *p)
{
    memset(e, 0, sizeof *e);
    memset(mx_rl, 0, sizeof mx_rl); memset(mx_rr, 0, sizeof mx_rr); memset(mx_el, 0, sizeof mx_el); memset(mx_er, 0, sizeof mx_er);
    memset(mx_gm, 0, sizeof mx_gm); mx_nsub = 0;
    memset(e->ghost, 0, sizeof e->ghost); memset(&g_shim, 0, sizeof g_shim);
    e->p = p;
    e->gov = 1.0f; e->playing = 1; e->fade = 0.0f;
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l];
        L->nv = ROLES[l].poly; L->type_now = 255;
        L->present = L->present_t = 1.0f;
        L->dtone.len = 25.0f + 10.0f * (float)l; L->dpan.len = 13.0f + 5.0f * (float)l;
        L->dwalk.len = 20.0f + 7.0f * (float)l; L->im = L->im_t = L->im_g = 1.0f; L->im_l = L->im_r = 1.0f;
        for (int k = 0; k < NMORPH; k++) L->mwalk[k].len = 25.0f + 5.0f * (float)k + 3.0f * (float)l;   /* (each its own pace: 25 to 64 s) */
    }
    reverb_init(&e->rv, 1.3f);
    echo_init(&e->echo);
    ens_init(&e->ens, p->seed);
    for (int l = 0; l < NUM_LAYERS; l++) dirt_init(&e->L[l].dirt, p->seed + (uint32_t)l);
    dirt_init(&e->kdirt, p->seed + 11u);
    patch(e, p->seed);
    e->mask = 0xF; e->chord_bars = 4; e->chord_left = 4; e->sec_left = 32;
    rng_seed(&e->rng, p->seed);
    e->seed_now = p->seed;
}

void engine_key(Engine *e, int src, int owner, int on, int note, int vel, int layer)
{
    src &= 1;
    uint32_t w = e->key_w[src];
    if (w - e->key_r[src] >= 32u) return;
    e->key_in[src][w % 32u] = (uint32_t)(on & 1) | (uint32_t)(owner & 127) << 1 | (uint32_t)(note & 127) << 8
                            | (uint32_t)(vel & 127) << 16 | (uint32_t)(layer + 1) << 24;
    e->key_w[src] = w + 1;
}

/* ------------------------------------------------------------------ notes: a sequencer's degree becomes notes */
static void queue(VoiceEv *q, int *nq, int sub, int voice, int note, float vel, float dur, float pan, const Sound *snd, uint32_t seed)
{
    if (*nq >= EVQ) return;
    VoiceEv *v = &q[(*nq)++];
    v->sub = (uint8_t)sub; v->voice = (uint8_t)voice; v->rel = 0; v->note = (int8_t)note; v->vel = vel; v->dur = dur;
    v->pan = pan; v->snd = *snd; v->seed = seed; v->from_hz = 0;
}

static int pick_voice(Voice *v, int n, int *next)
{
    for (int k = 0; k < n; k++) { int i = (*next + k) % n; if (!v[i].on) { *next = (i + 1) % n; return i; } }
    int best = 0; float lo = 9.0f;                   /* all busy: the quietest */
    for (int i = 0; i < n; i++) if (v[i].env < lo) { lo = v[i].env; best = i; }
    *next = (best + 1) % n;
    return best;
}

static void play(Engine *e, int l, int sub, const SeqNote *sn)
{
    Layer *L = &e->L[l]; const Piece *p = e->p; const LayerP *lp = &p->l[l];
    const Scale *sc = &SCALES[p->scale % NUM_SCALES];
    int base = ROLES[l].base + p->key % 12 + 12 * lp->octave, n = sc->n;
    int notes[5], nn = 0;
    if (l == L_GROUND && lp->seq == S_MELODY) {      /* a melody in the bass: the line itself, round the chord's root */
        notes[nn++] = deg_note(sc, base, e->root % n + sn->deg - ROLES[l].span / 2);
    } else if (l == L_GROUND) {                      /* the root, its fifth, its octave: the floor under the chords */
        static const int8_t PICK[4] = { 0, 1, 2, 0 };
        int w = PICK[sn->deg % 4], d = e->root + (w == 1 ? fifth_of(sc) : w == 2 ? n : 0);
        notes[nn++] = deg_note(sc, base, d);
    } else if (l == L_BODY) {                        /* the chord, in the voicing MOOD asks for; the degree its inversion */
        int type = p->mood < 30 ? CH_TRIAD : p->mood < 65 ? CH_SEVENTH : CH_NINTH, d[5];
        int k = chord_degs(type, e->root, d), inv = sn->deg % (k + 1);
        if (inv == k) { notes[nn++] = deg_note(sc, base, e->root + (sn->deg / (k + 1)) % 3 * 2); }   /* now and then one note */
        else for (int i = 0; i < k; i++) notes[nn++] = deg_note(sc, base, d[i] + (i < inv ? n : 0));
    } else {                                         /* the melody, moving with the harmony */
        notes[nn++] = deg_note(sc, base, sn->deg + e->root % n);
    }
    Sound snd; layer_sound(e, l, &snd);
    {   /* modular: the sequencer's CV and the note's accent shape this note's sound; the layer patched in pushes it */
        float link = u(p->link), m = sn->mod - 0.5f, acc = sn->vel - 0.6f, cv = e->L[L->src].cv;
        snd.tone = clamp01(snd.tone + 0.45f * m + 0.3f * acc + link * 0.35f * cv);
        snd.weight = clamp01(snd.weight + 0.35f * acc);
        snd.weird = clamp01(snd.weird + (0.15f + 0.35f * link) * m * (snd.weird + 0.3f));
        snd.release *= 0.7f + 0.6f * sn->mod;
    }
    float beat_s = 60.0f / e->bpm, dur = sn->dur * beat_s, vel = sn->vel * (0.75f + 0.25f * u(lp->weight));
    float w = ROLES[l].width;
    for (int i = 0; i < nn; i++) {
        int note = notes[i];
        while (note < 12) note += 12;
        while (note > 120) note -= 12;
        float pan = sn->pan < 2.0f ? sn->pan * w : (nn > 1 ? ((float)i / (float)(nn - 1) * 2.0f - 1.0f) * w : rng_bi(&e->rng) * w);
        pan += u(p->drift) * 0.3f * L->dpan.v;                       /* FLOW: the layer's slow wandering */
        pan += rng_bi(&e->rng) * (0.15f + 0.6f * u(p->chaos)) * (0.4f + 0.6f * w);   /* each note somewhere new (CHAOS: wider) */
        pan = clampf(pan, -1.0f, 1.0f);
        float nv = vel / (nn > 1 ? 1.0f + 0.25f * (float)nn : 1.0f);
        /* LEGATO: the voice that played this part of the last note (or chord) slides on to this one, not started again */
        if (p->legato[l] && snd.engine != E_PLUCK && i < L->last_n && L->last_v[i] >= 0 && L->v[L->last_v[i]].on && L->v[L->last_v[i]].hold > 0) {
            int vi = L->last_v[i];
            if (L->nq < EVQ) { VoiceEv *q = &L->q[L->nq++]; q->sub = (uint8_t)sub; q->voice = (uint8_t)vi; q->rel = 2; q->note = (int8_t)note; q->vel = nv; q->dur = dur; }
            L->last_hz[i] = mtof((float)note);
        } else {
            int keys = 0;                                 /* (the keys' notes sounding: each takes a layer voice's room) */
            for (int k = 0; k < KEY_POLY; k++) keys += e->kv[k].on && e->kv[k].env > 0.01f;
            int cap = (int)((float)L->nv * (0.4f + 0.6f * e->gov) + 0.5f) - (keys + 1) / 2;   /* (under load, fewer voices) */
            int vi = pick_voice(L->v, cap < 1 ? 1 : cap > L->nv ? L->nv : cap, &L->next_v);
            queue(L->q, &L->nq, sub, vi, note, nv, dur, pan, &snd, rng_u32(&e->rng));
            if (p->glide[l] && i < L->last_n) L->q[L->nq - 1].from_hz = L->last_hz[i];   /* GLIDE: from the last note */
            L->last_v[i] = (int8_t)vi; L->last_hz[i] = mtof((float)note);
        }
        midi_note(e, (e->midi_ch + l) & 15, note, vel, dur);
        post(e, l, note, vel, pan, dur, 0);
    }
    L->last_n = nn;
    e->events[l]++;
    L->cv = fm_maxf(L->cv, sn->vel);
    if (l == L_GROUND) e->duck_to = fm_maxf(e->duck_to, vel * u(lp->weight) * 0.45f);
    /* LINK: a note nudges another layer's system; now and then it asks the harmony to move */
    float link = u(p->link);
    if (link > 0.01f && rng_chance(&e->rng, link * 0.22f)) {
        int o = (l + 1 + rng_int(&e->rng, NUM_LAYERS - 1)) % NUM_LAYERS;
        seq_poke(&e->L[o].seq);
    }
    if (link > 0.01f && rng_chance(&e->rng, link * link * 0.02f)) e->want_change = 1;
}

static void keys_in(Engine *e, int sub)
{
    for (int src = 0; src < 2; src++)
    while (e->key_r[src] != e->key_w[src]) {
        uint32_t k = e->key_in[src][e->key_r[src] % 32u]; e->key_r[src]++;
        int on = (int)(k & 1u), own = (int)(k >> 1 & 127u), note = (int)(k >> 8 & 127u), lay = (int)(k >> 24 & 7u) - 1;
        float vel = (float)(k >> 16 & 127u) / 127.0f;
        if (lay < 0 || lay >= NUM_LAYERS) lay = e->key_layer;
        if (on) {
            Sound snd; layer_sound(e, lay, &snd);
            int vi = pick_voice(e->kv, KEY_POLY, &e->key_next);
            e->kv_lay[vi] = (int8_t)lay; e->kv_own[vi] = (uint8_t)own;
            queue(e->kq, &e->nkq, sub, vi, note, vel, 600.0f, rng_bi(&e->rng) * 0.3f, &snd, rng_u32(&e->rng));
            post(e, lay, note, vel, 0, 1.5f, 1);
            if (e->midi && e->midi_on) e->midi(0x09u | (0x90u | e->midi_keys_ch) << 8 | (uint32_t)note << 16 | (uint32_t)(vel * 127.0f) << 24);
        } else {
            for (int i = 0; i < KEY_POLY; i++)          /* (a key's voice: the one playing its note, held) */
                if (e->kv[i].on && e->kv[i].note == note && e->kv_own[i] == own && e->kv[i].hold > 0 && e->nkq < EVQ) {
                    VoiceEv *v = &e->kq[e->nkq++]; v->sub = (uint8_t)sub; v->voice = (uint8_t)i; v->rel = 1;
                }
            if (e->midi && e->midi_on) e->midi(0x08u | (0x80u | e->midi_keys_ch) << 8 | (uint32_t)note << 16);
        }
    }
}

/* ------------------------------------------------------------------ the conductor: harmony and form, at the bar */
static void next_chord(Engine *e)
{
    const Scale *sc = &SCALES[e->p->scale % NUM_SCALES];
    int n = sc->n, a = ((e->root % n) + n) % n;
    /* the moves 90s hardcore and Detroit love: down a third, up a fourth, to the sixth and back */
    static const uint8_t W[7] = { 0, 2, 3, 6, 6, 5, 2 };   /* by the interval up, in degrees */
    int sum = 0;
    for (int i = 0; i < n; i++) sum += W[i % 7] + (((a + i) % n) == 0 ? 4 : 0);
    int x = rng_int(&e->rng, sum), i = 0;
    for (; i < n - 1; i++) { int w = W[i % 7] + (((a + i) % n) == 0 ? 4 : 0); if (x < w) break; x -= w; }
    e->root = (a + i) % n;
    if (e->root >= 4 && n >= 7 && rng_chance(&e->rng, 0.5f)) e->root -= n;   /* the upper chords low, to keep near home */
}

static void next_section(Engine *e)
{
    float ev = u(e->p->evolve);
    int m = 0;
    for (int l = 0; l < NUM_LAYERS; l++) if (rng_chance(&e->rng, l == L_BODY ? 0.85f : l == L_GROUND ? 0.9f : 0.65f)) m |= 1 << l;
    if (((m & 1) + (m >> 1 & 1) + (m >> 2 & 1) + (m >> 3 & 1)) < 2) m |= 1 << L_BODY | 1 << (rng_chance(&e->rng, 0.5f) ? L_LIGHT : L_DUST);
    e->mask = ev < 0.05f ? 0xF : m;
    for (int l = 0; l < NUM_LAYERS; l++) {
        e->L[l].tone_off = rng_bi(&e->rng) * 0.2f * ev;
        e->L[l].dens_off = rng_bi(&e->rng) * 0.3f * ev;
        if (rng_chance(&e->rng, ev * 0.5f)) seq_poke(&e->L[l].seq);
    }
    e->sec_left = (int)(48.0f - 36.0f * ev) + rng_int(&e->rng, 16);
}

static void bar(Engine *e)
{
    float ev = u(e->p->evolve);
    e->bar++;
    if (--e->chord_left <= 0 || e->want_change) {
        next_chord(e);
        e->want_change = 0;
        static const uint8_t BARS[3][3] = { { 4, 8, 8 }, { 2, 4, 4 }, { 1, 2, 4 } };
        e->chord_left = BARS[ev < 0.33f ? 0 : ev < 0.7f ? 1 : 2][rng_int(&e->rng, 3)];
    }
    if (--e->sec_left <= 0) next_section(e);
}

/* one control step: the clock, the sequencers, the notes */
static void control(Engine *e, int sub)
{
    const Piece *p = e->p;
    e->bpm = e->ext_bpm > 20.0f ? clampf(e->ext_bpm, 30.0f, 240.0f) : 40.0f + (float)p->tempo;
    float beats = BLK_S * e->bpm / 60.0f;
    if (e->sync_req) {                               /* a Start: the top of the bar; the steps on the 16ths from here */
        e->sync_req = 0; e->beat = 0; e->beat_adj = 0; e->clock_acc = 0;
        for (int l = 0; l < NUM_LAYERS; l++) { Seq *s = &e->L[l].seq; s->t = 0; s->bend_ph = 0; s->swing_phase = 0; s->ratchet_left = 0; }
    }
    if (e->beat_adj != 0.0f) {                       /* locked to a clock: up to half again or half as fast, a moment */
        float lim = beats * 0.5f, a = clampf(e->beat_adj, -lim, lim);
        beats += a; e->beat_adj -= a;
    }
    e->t += BLK_S;
    if (e->seed_now != p->seed) {                    /* a new piece: the conductor starts again */
        e->seed_now = p->seed; rng_seed(&e->rng, p->seed);
        e->root = 0; e->chord_left = 4; e->sec_left = 24; e->mask = 0xF; e->beat = 0; e->bar = 0;
        ens_init(&e->ens, p->seed);
        patch(e, p->seed);
        engine_all_off(e);
    }
    keys_in(e, sub);
    midi_due(e);
    e->fade += ((e->playing ? 1.0f : 0.0f) - e->fade) * (BLK_S / 1.2f);
    if (!e->playing && e->fade < 0.01f) return;
    if (e->midi && e->midi_clock && e->playing) {   /* (whether notes go out or not) */   /* MIDI clock: 24 a beat (none in the fade after STOP) */
        e->clock_acc += beats * 24.0f;
        while (e->clock_acc >= 1.0f) { e->clock_acc -= 1.0f; e->midi(0x0Fu | 0xF8u << 8); }
    }
    float prev = e->beat;
    e->beat += beats;
    if ((int)(e->beat * 0.25f) != (int)(prev * 0.25f)) bar(e);
    /* the duck: GROUND's notes pull BODY and DUST down, let go over 120 ms; eased in over ~10 ms, never a step */
    e->duck_to -= e->duck_to * (BLK_S / 0.12f);
    e->duck += (e->duck_to - e->duck) * (BLK_S / 0.01f);
    float motion = u(p->motion);
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l]; const LayerP *lp = &p->l[l];
        layer_seq_check(e, l);
        drift_step(&L->dtone, &e->rng, BLK_S); drift_step(&L->dpan, &e->rng, BLK_S);
        L->cv -= L->cv * (BLK_S / (0.25f + 0.6f * u(lp->release)));
        if ((L->rnd_left -= beats) <= 0) { L->rnd_t = rng_bi(&e->rng); L->rnd_left = (float)(1 + rng_int(&e->rng, 4)); }   /* a new voltage every 1-4 beats */
        L->rnd += (L->rnd_t - L->rnd) * (BLK_S * 3.0f);
        L->present_t = (e->mask >> l & 1) ? 1.0f : 0.0f;
        L->present += (L->present_t - L->present) * (BLK_S / 6.0f);
        Seq *s = &L->seq;
        s->density = clamp01((u(lp->density) + L->dens_off) * (0.35f + 1.3f * motion) * e->gov);
        s->mutate = clamp01(u(lp->mutate) * (0.5f + u(p->evolve)));
        s->chaos = u(p->chaos); s->warp = u(p->warp);
        seq_run(s, beats * RATES[lp->rate % NUM_RATES]);
        if (L->present < 0.05f || lp->level == 0) continue;   /* (out of the form: it runs, silent) */
        for (int i = 0; i < s->nout; i++) play(e, l, sub, &s->out[i]);
    }
}

/* ------------------------------------------------------------------ the block */
/* DRIFT (HOME), Steve Roach's Immersion Station's evolve: the four layers' levels wander slowly, sometimes leaving one
 * alone with the others far off, and each walks slowly round the stereo field. As a layer fades it goes into the
 * distance: its lows thin out (a high-pass rising) and more of it is reverb than itself. At 0 the mix stays as set. */
static void immerse(Engine *e)
{
    const Piece *p = e->p;
    float d = u(p->weird), dt = BLK_S * (float)e->nsub;
    if ((e->im_left -= dt) <= 0) {
        e->im_left = (14.0f + 30.0f * rng_f(&e->rng)) / (0.4f + 0.8f * d);
        int solo = rng_chance(&e->rng, 0.35f) ? rng_int(&e->rng, NUM_LAYERS) : -1, top = rng_int(&e->rng, NUM_LAYERS);
        for (int l = 0; l < NUM_LAYERS; l++) {
            float t = solo >= 0 ? (l == solo ? 1.0f : 0.05f + 0.25f * rng_f(&e->rng)) : 0.15f + 0.85f * rng_f(&e->rng);
            e->L[l].im_t = l == top && solo < 0 ? 1.0f : t;   /* (one layer near full, always) */
            if (l == L_GROUND) e->L[l].im_t = fm_maxf(e->L[l].im_t, 0.65f);   /* (the bass never far off: the ground under it all) */
        }
    }
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l];
        if (L->im <= 0) L->im = 1.0f;
        L->im += (L->im_t - L->im) * clamp01(dt / (6.0f + 6.0f * (float)l * 0.5f));   /* slow: seconds, never steps */
        L->im_pan = drift_step(&L->dwalk, &e->rng, dt);
        for (int k = 0; k < NMORPH; k++) L->mw[k] = d * drift_step(&L->mwalk[k], &e->rng, dt);   /* the sends, TONE, WEIGHT, ENTROPY morphing */
        float far = 1.0f - L->im;                                /* how far off it has gone */
        L->im_g = 1.0f - d * far;                                /* its level (at DRIFT 0: as set) */
        L->im_far = d * far;
        float pn = d * 0.85f * L->im_pan * (l == L_GROUND ? ROLES[l].width : 1.0f), a = (pn + 1.0f) * 0.25f * PI_F;   /* its walk round
                                                                 * the stereo field (GROUND's no wider than its notes': near the middle) */
        L->im_l = fm_cosf(a) * 1.4142f; L->im_r = fm_sinf(a) * 1.4142f;
    }
}

/* WARBLE: the pitch wandering as a worn tape's or an old synth's does. One slow random voltage for the whole piece (its
 * own pace wandering too: an LFO on an LFO), each layer a little apart from it; FLUTTER a faster shiver over it; DETUNE
 * everything a touch flat, "relaxed". The pitch only: notes stay in their scale, a few cents off at most */
static void warble(Engine *e)
{
    const Piece *p = e->p;
    float dt = BLK_S * (float)e->nsub, w = u(p->warble), fl = u(p->flutter), dtn = u(p->detune);
    if ((e->wb_left -= dt) <= 0) { e->wb_t = rng_bi(&e->rng); e->wb_left = 0.6f + 2.5f * rng_f(&e->rng); e->wb_rate = 0.25f + 0.6f * rng_f(&e->rng); }
    e->wb_v += (e->wb_t - e->wb_v) * clamp01(dt * e->wb_rate);
    e->fl_ph += dt * 6.3f; if (e->fl_ph >= 1.0f) e->fl_ph -= 1.0f;
    float cents = -30.0f * dtn;
    for (int b = 0; b < NBUS; b++) {
        float own = b < NUM_LAYERS ? e->L[b].rnd : 0.0f;
        float c = cents + w * 28.0f * (0.8f * e->wb_v + 0.2f * own) + fl * 7.0f * fm_sin_turns(e->fl_ph + 0.13f * (float)b);
        e->pm[b] = fm_exp2f(c * (1.0f / 1200.0f));
    }
}

static uint32_t now(Engine *e) { return e->clock ? e->clock() : 0; }
static void cost_add(Engine *e, int k, uint32_t t0) { if (e->clock) e->cost[k] += ((float)(now(e) - t0) - e->cost[k]) * 0.05f; }

static void svf_coef(Svf *d, const Svf *s) { d->a1 = s->a1; d->a2 = s->a2; d->a3 = s->a3; d->k = s->k; }   /* (its state its own) */

void engine_block_begin(Engine *e, int nsub)
{
    uint32_t t0 = now(e);
    const Piece *p = e->p;
    e->nsub = nsub > ENGINE_SUB ? ENGINE_SUB : nsub < 1 ? 1 : nsub;
    for (int l = 0; l < NUM_LAYERS; l++) e->L[l].nq = 0;
    e->nkq = 0;
    warble(e);
    immerse(e);
    if (e->shed != e->shed_seen && e->t - e->shed_t > 0.5f) {   /* a block nearly overran: fewer notes, the faintest tails let go */
        e->shed_seen = e->shed; e->shed_t = e->t;
        e->gov = fm_maxf(0.3f, e->gov - 0.1f);
        for (int b = 0; b < NBUS; b++) {
            Voice *v = b == KEYS ? e->kv : e->L[b].v; int n = b == KEYS ? KEY_POLY : e->L[b].nv;
            for (int i = 0; i < n; i++)
                if (v[i].on && v[i].hold <= 0 && v[i].env < 0.02f && v[i].r_k < 0.0015f) v[i].r_k = 0.0015f;   /* (below -34 dB: gone in ~60 ms) */
        }
    }
    g_voice_floor = 0.001f + 0.014f * (1.0f - e->gov);   /* -60 dB; under load up to -37 */
    int back = 0; uint32_t now_on = 0;
    for (int k = 0; k < e->nsub; k++) {
        control(e, k);
        for (int l = 0; l < NUM_LAYERS; l++) {
            float lv = u(p->l[l].level), g = lv * lv * e->L[l].present * e->fade * 1.6f * e->L[l].im_g;   /* (DRIFT's fading) */
            if (l == L_BODY || l == L_DUST) g *= 1.0f - e->duck;
            if (l == L_GROUND) g *= 1.0f + 0.4f * clamp01((0.5f - u(p->mood)) * 2.0f);   /* (MOOD dark: the bass up) */
            e->gains[k][l] = g;
            if (k == 0) back += g >= 1e-4f && !(e->sounding >> l & 1u);
            if (k == 0) now_on |= (uint32_t)(g >= 1e-4f) << l;
        }
        e->gains[k][KEYS] = e->fade * 1.2f + 0.3f;
    }
    /* layers coming back in (DRIFT, EVOLVE's sections): all their voices sounding again at once, the load jumping, so
     * room made before it lands: fewer new notes and the faintest tails let go, the load guard easing back as it settles */
    if (back) e->gov = fm_minf(e->gov, back >= 2 ? 0.6f : 0.75f);
    e->sounding = now_on;
    /* the bands: each layer's filters, from its role; TONE a DJ's filter on them, at 12 o'clock open, to the left a
     * low-pass sweeping down to a thud, to the right a high-pass sweeping up to air, a little resonance towards the ends */
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l];
        float t = mo(e, l, MW_TONE, p->l[l].tone, 0.25f);
        if (t > 0.5f) t = fm_minf(t, fm_maxf(u(p->l[l].tone), 0.5f));   /* (DRIFT's TONE wanders dark, never into the high-pass) */
        float lo = t < 0.5f ? (0.5f - t) * 2.0f : 0.0f, hi = t > 0.5f ? (t - 0.5f) * 2.0f : 0.0f;
        /* MOOD: a tilt, 12 o'clock as set. Darker, the upper layers a shade duller (and the bass up: the gains, the
         * shelf); brighter, they open and lose their lows, so at full GROUND alone carries the bass */
        float m = u(p->mood), dark = clamp01((0.5f - m) * 2.0f), bright = clamp01((m - 0.5f) * 2.0f);
        float lp = ROLES[l].lp * fm_exp2f(l == L_GROUND ? 0.0f : 0.5f * bright - 1.0f * dark) * fm_exp2f(-7.0f * lo);
        float hp = ROLES[l].hp * fm_exp2f(6.5f * hi + (l == L_GROUND ? 0.0f : 2.0f * bright));   /* (DRIFT: far off by its level and reverb alone) */
        /* patched: the source layer's activity opens this one's filter, the random voltage moves it (LINK, DRIFT) */
        lp *= fm_exp2f(u(p->link) * 1.8f * (e->L[L->src].cv - 0.25f) + u(p->drift) * 0.9f * L->rnd);
        lp *= g_band_lp; lp = clampf(lp, 40.0f, 18000.0f); hp = clampf(hp, 15.0f, 12000.0f);
        if (L->lp_hz <= 0) { L->lp_hz = lp; L->hp_hz = hp; }
        L->lp_hz += (lp - L->lp_hz) * 0.35f; L->hp_hz += (hp - L->hp_hz) * 0.35f;   /* (turned smoothly, no steps) */
        float q = 0.6f + 1.4f * (lo > hi ? lo : hi) * (lo > hi ? lo : hi);
        for (int c = 0; c < 2; c++) { svf_set(&L->hp[c], L->hp_hz, hi > 0 ? q : 0.6f); svf_set(&L->lp[c], L->lp_hz, lo > 0 ? q : 0.6f); }
        if (l == e->key_layer)                   /* the keys: the chosen layer's band and TONE (their own filters' states) */
            for (int c = 0; c < 2; c++) { svf_coef(&e->khp[c], &L->hp[c]); svf_coef(&e->klp[c], &L->lp[c]); }
    }
    /* the sounding notes follow their layer's knobs: TONE, WEIGHT, ENTROPY, RELEASE, SWEEP, RESO, F.GLIDE, MOOD */
    for (int l = 0; l < NUM_LAYERS; l++) {
        const LayerP *lp = &p->l[l];
        uint32_t k = (uint32_t)lp->tone | (uint32_t)lp->weight << 7 | (uint32_t)lp->weird << 14 | (uint32_t)lp->release << 21;
        k = k * 2654435761u ^ ((uint32_t)p->sweep[l] | (uint32_t)p->reso[l] << 7 | (uint32_t)p->glide[l] << 14 | (uint32_t)p->mood << 21);
        k = k * 2654435761u ^ (uint32_t)((int)(e->L[l].mw[MW_TONE] * 16.0f) & 63) ^ (uint32_t)((int)(e->L[l].mw[MW_WEIGHT] * 16.0f) & 63) << 6
            ^ (uint32_t)((int)(e->L[l].mw[MW_ENT] * 16.0f) & 63) << 12;   /* (DRIFT's morphing: the notes follow it, a step at a time) */
        if (k == e->L[l].knobs) continue;
        e->L[l].knobs = k;
        Sound snd; layer_sound(e, l, &snd);
        for (int i = 0; i < e->L[l].nv; i++) voice_retune(&e->L[l].v[i], &snd);
        for (int i = 0; i < KEY_POLY; i++) if (e->kv_lay[i] == l) voice_retune(&e->kv[i], &snd);
    }
    cost_add(e, NBUS, t0);
}

static void apply(VoiceEv *q, int nq, Voice *v, Voice *ghost, int sub, float *Lb, float *Rb)
{
    for (int i = 0; i < nq; i++) {
        if (q[i].sub != sub) continue;
        Voice *x = &v[q[i].voice];
        if (q[i].rel == 1) voice_release(x);
        else if (q[i].rel == 2) voice_legato(x, q[i].note, q[i].vel, q[i].dur);
        else {
            if (x->on && x->env > 0.002f) {      /* a sounding note taken: it fades in a ghost, no click. All four fading:
                                                  * the quietest of them and it faded right out across this block (not cut) */
                Voice *g = &ghost[0];
                for (int j = 0; j < 4; j++) { if (!ghost[j].on) { g = &ghost[j]; break; } if (ghost[j].env < g->env) g = &ghost[j]; }
                Voice *out = !g->on ? 0 : g->env < x->env ? g : x;
                if (out) { out->kill = 1; voice_render(out, Lb, Rb, BLK); }
                if (out != x) { *g = *x; g->hold = 0; g->r_k = 0.0015f; }
            }
            voice_start(x, &q[i].snd, q[i].note, q[i].vel, q[i].dur, q[i].pan, q[i].from_hz, q[i].seed);
        }
    }
}

void engine_block_layer(Engine *e, int b)
{
    uint32_t t0 = now(e);
    int made = 0;                        /* (any sound made: a layer faded right out keeps its cost as it last was, so the
                                          * cores' split is ready for it coming back) */
    float wg = 0.0f;                     /* (the piece's ring modulator: gone; each layer's EXTREME has its own strangeness) */
    for (int k = 0; k < e->nsub; k++) {
        float *Lb = e->bus[k][b][0], *Rb = e->bus[k][b][1];
        memset(Lb, 0, sizeof(float) * BLK); memset(Rb, 0, sizeof(float) * BLK);
        float *sl = e->stem[b][0] + k * BLK, *sr = e->stem[b][1] + k * BLK;   /* (the stem: before DIRT) */
        if (e->stems_on) { memset(sl, 0, sizeof(float) * BLK); memset(sr, 0, sizeof(float) * BLK); }
        if (b == KEYS) {
            apply(e->kq, e->nkq, e->kv, e->ghost[KEYS], k, Lb, Rb);
            int any = 0;
            for (int j = 0; j < 4; j++) if (e->ghost[KEYS][j].on) { e->ghost[KEYS][j].pm = e->pm[KEYS]; voice_render(&e->ghost[KEYS][j], Lb, Rb, BLK); any = 1; }
            for (int i = 0; i < KEY_POLY; i++) if (e->kv[i].on) { e->kv[i].ws = wg > 0.5f ? (wg - 0.5f) * 1.4f : 0.0f; e->kv[i].pm = e->pm[KEYS]; voice_render(&e->kv[i], Lb, Rb, BLK); any = 1; }
            /* no note sounding: the filters still ring out what they hold (not left mid-swing, to jump when the next note
             * comes: a click at each), then rest at zero */
            if (!any && e->kidle >= 64) { for (int c = 0; c < 2; c++) { e->klp[c].ic1 = e->klp[c].ic2 = e->khp[c].ic1 = e->khp[c].ic2 = 0; } continue; }
            e->kidle = any ? 0 : e->kidle + 1;
            int kl = e->key_layer;               /* through the chosen layer's chain, as its own notes go */
            for (int i = 0; i < BLK; i++) {
                Lb[i] = svf_lp(&e->klp[0], svf_hp(&e->khp[0], Lb[i]));
                Rb[i] = svf_lp(&e->klp[1], svf_hp(&e->khp[1], Rb[i]));
            }
            if (e->stems_on) { memcpy(sl, Lb, sizeof(float) * BLK); memcpy(sr, Rb, sizeof(float) * BLK); }
            dirt_run(&e->kdirt, u(e->p->dirt), fm_maxf(mo(e, kl, MW_DIRT, e->p->s_dirt[kl], 0.45f), 0.4f * u(e->p->dirt)), Lb, Rb, BLK);
            continue;
        }
        Layer *L = &e->L[b];
        apply(L->q, L->nq, L->v, e->ghost[b], k, Lb, Rb);
        if (e->gains[k][b] < 1e-4f) {            /* faded right out (DRIFT, LEVEL): its voices go on unheard, no sound made */
            for (int i = 0; i < L->nv; i++) voice_skip(&L->v[i], BLK);
            for (int j = 0; j < 4; j++) e->ghost[b][j].on = 0;
            for (int c = 0; c < 2; c++) { L->lp[c].ic1 = L->lp[c].ic2 = L->hp[c].ic1 = L->hp[c].ic2 = 0; }   /* (unheard: back from rest) */
            continue;
        }
        int any = 0; made = 1;
        for (int j = 0; j < 4; j++) if (e->ghost[b][j].on) { e->ghost[b][j].pm = e->pm[b]; voice_render(&e->ghost[b][j], Lb, Rb, BLK); any = 1; }
        for (int i = 0; i < L->nv; i++) if (L->v[i].on) { L->v[i].ws = wg > 0.5f ? (wg - 0.5f) * 1.4f : 0.0f; L->v[i].pm = e->pm[b]; voice_render(&L->v[i], Lb, Rb, BLK); any = 1; }
        /* no note sounding: the band's filters still ring out what they hold, then rest at zero. (They stopped mid-swing,
         * held there, and jumped out when the next note came: a click on the control step, loud with a dark TONE) */
        if (!any && L->idle >= 64) { for (int c = 0; c < 2; c++) { L->lp[c].ic1 = L->lp[c].ic2 = L->hp[c].ic1 = L->hp[c].ic2 = 0; } continue; }
        L->idle = any ? 0 : L->idle + 1;
        for (int i = 0; i < BLK; i++) {
            Lb[i] = svf_lp(&L->lp[0], svf_hp(&L->hp[0], Lb[i]));
            Rb[i] = svf_lp(&L->lp[1], svf_hp(&L->hp[1], Rb[i]));
        }
        if (e->stems_on) { memcpy(sl, Lb, sizeof(float) * BLK); memcpy(sr, Rb, sizeof(float) * BLK); }
        dirt_run(&L->dirt, u(e->p->dirt), fm_maxf(mo(e, b, MW_DIRT, e->p->s_dirt[b], 0.45f), 0.4f * u(e->p->dirt)), Lb, Rb, BLK);   /* its own DIRT, its send's amount (DRIFT morphing it); the shared DIRT reaching every layer a little */
        /* (ENTROPY is in the voices: synthesis turning on itself, not distortion; DIRT is the tape) */
    }
    if (made || b == KEYS) cost_add(e, b, t0);
}

void engine_overload(Engine *e) { e->shed++; }

/* an effect with nothing coming in, and nothing coming out for longer than its memory, rests (its output silence)
 * until something comes: an effect turned down, or a send at nothing, costs nothing */
#define FX_QUIET 1e-7f                           /* a block's energy: about -88 dB */
static float energy(const float *a, const float *b) { float s = 0; for (int i = 0; i < BLK; i++) s += a[i] * a[i] + b[i] * b[i]; return s; }
static int fx_wake(int *q, float in, int mem) { if (in > FX_QUIET) { *q = 0; return 1; } return *q < mem; }
static void fx_rest(int *q, float out) { if (out > FX_QUIET) *q = 0; else if (*q < 1 << 30) *q += BLK; }

/* the mix, in stages so two cores can share it. The wet effects, echo, chorus, shimmer and reverb (engine_mix_reverb),
 * take the sends of the block before (5.8 ms later: unheard; the echo's time that much shorter, so its repeats stay on
 * the beat), so the second core can run them beside its layers; then the sends gathered (engine_mix_sends), the wet
 * added and the chorus (engine_mix_echo), and the sum, the shelf and the leveller (engine_mix_finish) */

void engine_mix_sends(Engine *e)
{
    const Piece *p = e->p;
    /* REVERB: up to 12 o'clock the reverb; past it the shimmer comes in, an octave up, and by the end down too.
     * ECHO: up to 12 o'clock the echo; past it, it turns into a reversed smear */
    float rk = u(p->reverb), rvk = clamp01(rk * 2.0f), shk = clamp01(rk * 2.0f - 1.0f);
    float ek = u(p->echo), eck = clamp01(ek * 2.0f), rev = clamp01(ek * 2.0f - 1.0f);
    float grk = u(p->grain), gsend = 0;
    for (int l = 0; l < NUM_LAYERS; l++) gsend = fm_maxf(gsend, mo(e, l, MW_GRAIN, p->s_grain[l], 0.5f) * e->L[l].present);
    if (e->key_layer >= 0 && e->key_layer < NUM_LAYERS) gsend = fm_maxf(gsend, u(p->s_grain[e->key_layer]));
    grk = fm_maxf(grk, 0.35f * gsend);              /* (a send wakes the chorus: a gentle one with CHORUS's own at 0) */
    mx_rvk = rvk; mx_shk = shk; mx_grk = grk; mx_nsub = e->nsub;
    e->rv_size = 2.5f + 12.0f * rvk * rvk - 3.0f * shk; e->rv_damp = 1800.0f + 4200.0f * u(p->mood) + 4000.0f * shk;
    e->echo.time_s = clampf(60.0f / e->bpm * 0.75f, 0.12f, 0.7f);   /* a dotted eighth */
    while (e->echo.time_s > 0.7f) e->echo.time_s *= 0.5f;
    e->echo.time_s -= (float)(e->nsub * BLK) / SR;  /* (it runs a block late: a block shorter, so it lands on the beat) */
    e->echo.fb = rev > 0 ? 0.45f + 0.3f * rev : 0.2f + 0.45f * eck; e->echo.wow = u(p->warp) + u(p->drift) * 0.5f;
    e->echo.rev = rev;
    for (int k = 0; k < e->nsub; k++) {
        float *dl = mx_dl + k * BLK, *dr = mx_dr + k * BLK, *rl = mx_rl + k * BLK, *rr = mx_rr + k * BLK;
        float *el = mx_el + k * BLK, *er = mx_er + k * BLK, *gm = mx_gm + k * BLK;
        for (int i = 0; i < BLK; i++) dl[i] = dr[i] = el[i] = er[i] = gm[i] = rl[i] = rr[i] = 0;
        for (int b = 0; b < NBUS; b++) {
            float g = e->gains[k][b], *Lb = e->bus[k][b][0], *Rb = e->bus[k][b][1];
            /* the sends: each layer's own (SENDS), the keys' the chosen layer's */
            int sl = b < NUM_LAYERS ? b : e->key_layer;
            float far = b < NUM_LAYERS ? e->L[b].im_far : 0.0f, pl = b < NUM_LAYERS ? e->L[b].im_l : 1.0f, pr = b < NUM_LAYERS ? e->L[b].im_r : 1.0f;
            /* (DRIFT morphing each send: a dry layer may wander into the reverb, a wet one out of it) */
            float rs = mo(e, sl, MW_RV, p->s_rv[sl], 0.5f) * 0.8f * (0.3f + 1.2f * rvk) * g * (1.0f + 1.5f * far);   /* (far off: more reverb than itself) */
            float es = mo(e, sl, MW_ECHO, p->s_echo[sl], 0.5f) * eck * 1.4f * g;
            float gs = fm_maxf(mo(e, sl, MW_GRAIN, p->s_grain[sl], 0.5f), 0.8f * u(p->grain)) * g;   /* (the shared CHORUS: every layer into it a little) */
            float ss = 0;
            /* every gain glided sample by sample from the last block's: a duck, a fade or a send moving never steps */
            float *m = e->mx_g[b], to[5] = { g * pl, g * pr, rs, es, gs }, a[5], d[5];
            for (int j = 0; j < 5; j++) { a[j] = m[j]; d[j] = (to[j] - m[j]) * (1.0f / BLK); m[j] = to[j]; }
            int st = b < NUM_LAYERS ? b : e->key_layer;
            if (e->stems_on && st >= 0 && st < NUM_LAYERS) {   /* the stems: each layer with no effects (DIRT neither), at its
                                                                 * level and pan; the keys (the last bus) into the chosen one's */
                float *xl = e->stem[st][0] + k * BLK, *xr = e->stem[st][1] + k * BLK, *ql = e->stem[b][0] + k * BLK, *qr = e->stem[b][1] + k * BLK;
                float x0 = a[0], x1 = a[1];
                if (b < NUM_LAYERS) for (int i = 0; i < BLK; i++) { x0 += d[0]; x1 += d[1]; xl[i] = ql[i] * x0; xr[i] = qr[i] * x1; }
                else for (int i = 0; i < BLK; i++) { x0 += d[0]; x1 += d[1]; xl[i] += ql[i] * x0; xr[i] += qr[i] * x1; }
            }
            for (int i = 0; i < BLK; i++) {
                float l = Lb[i], r = Rb[i];
                a[0] += d[0]; a[1] += d[1]; a[2] += d[2]; a[3] += d[3]; a[4] += d[4];
                dl[i] += l * a[0]; dr[i] += r * a[1];
                rl[i] += l * a[2]; rr[i] += r * a[2];
                el[i] += l * a[3]; er[i] += r * a[3];
                gm[i] += (l + r) * 0.5f * a[4];
                ss += l * l + r * r;
            }
            if (b < NUM_LAYERS) {
                float rms = fm_sqrtf(ss * g * g / (2.0f * BLK));
                float lvl = rms > 1e-5f ? clamp01((fm_log2f(rms) * 6.02f + 48.0f) / 42.0f) : 0.0f;
                e->meter[b] += (lvl - e->meter[b]) * (lvl > e->meter[b] ? 0.3f : 0.02f);
            }
        }
    }
}

void engine_mix_reverb(Engine *e)                /* the wet effects (the second core's: it touches only theirs) */
{
    uint32_t t0 = now(e);
    float shk = mx_shk, gk = mx_grk, gg = fm_sqrtf(gk) * 1.1f;   /* (the chorus's wet beside the dry) */
    if (e->rv_size > 0) reverb_set(&e->rv, e->rv_size, e->rv_damp);
    int ns = mx_nsub < e->nsub ? mx_nsub : e->nsub;
    for (int k = ns; k < e->nsub; k++) for (int i = 0; i < BLK; i++) mx_vl[k * BLK + i] = mx_vr[k * BLK + i] = mx_wl[k * BLK + i] = mx_wr[k * BLK + i] = 0;
    for (int k = 0; k < ns; k++) {
        float *rl = mx_rl + k * BLK, *rr = mx_rr + k * BLK, *vl = mx_vl + k * BLK, *vr = mx_vr + k * BLK;
        float *el = mx_el + k * BLK, *er = mx_er + k * BLK, *gm = mx_gm + k * BLK, *wl = mx_wl + k * BLK, *wr = mx_wr + k * BLK;
        float ol[BLK] = { 0 }, orr[BLK] = { 0 }, gl[BLK] = { 0 }, grr[BLK] = { 0 }, sh[BLK];
        if (fx_wake(&e->q_echo, energy(el, er), 34000)) { echo_run(&e->echo, el, er, ol, orr, BLK); fx_rest(&e->q_echo, energy(ol, orr)); }
        if (fx_wake(&e->q_gr, energy(gm, gm), 4400)) { ens_run(&e->ens, gk, gm, gl, grr, BLK); fx_rest(&e->q_gr, energy(gl, grr)); }
        (void)sh;
        for (int i = 0; i < BLK; i++) {
            wl[i] = ol[i] + gl[i] * gg; wr[i] = orr[i] + grr[i] * gg;
            rl[i] += ol[i] * 0.3f + gl[i] * gg * 0.3f;
            rr[i] += orr[i] * 0.3f + grr[i] * gg * 0.3f;
        }
        if (shk > 0.001f && fx_wake(&e->q_sh, energy(e->shim_prev, e->shim_prev), 4400)) {   /* the shimmer: the reverb's last block, an octave up and down, back in */
            /* (a limiter in the loop: the louder the reverb, the less goes round again, so it never runs away) */
            float pr = fm_sqrtf(energy(e->shim_prev, e->shim_prev) * (1.0f / (2.0f * BLK)));
            e->shim_env += (pr - e->shim_env) * (pr > e->shim_env ? 0.05f : 0.01f);
            float lim = 1.0f / (1.0f + 25.0f * fm_maxf(0.0f, e->shim_env - 0.03f));
            float up[BLK], dn_[BLK], gu = shk * 0.3f * lim, gd = shk * shk * 0.26f * lim;
            shifter_run(&g_shim, e->shim_prev, up, dn_, BLK);
            fx_rest(&e->q_sh, energy(up, dn_));
            for (int i = 0; i < BLK; i++) { float x = up[i] * gu + dn_[i] * gd; rl[i] += x; rr[i] += x; }
        }
        for (int i = 0; i < BLK; i++) vl[i] = vr[i] = 0;
        if (fx_wake(&e->q_rv, energy(rl, rr), 9000)) { reverb_run(&e->rv, rl, rr, vl, vr, BLK); fx_rest(&e->q_rv, energy(vl, vr)); }
        for (int i = 0; i < BLK; i++) e->shim_prev[i] = (vl[i] + vr[i]) * 0.5f;
    }
    if (e->clock) e->cost_rv += ((float)(now(e) - t0) - e->cost_rv) * 0.05f;
}

void engine_mix_echo(Engine *e)                  /* the wet added to the dry */
{
    const Piece *p = e->p;
    for (int k = 0; k < e->nsub; k++) {
        float *dl = mx_dl + k * BLK, *dr = mx_dr + k * BLK, *wl = mx_wl + k * BLK, *wr = mx_wr + k * BLK;
        for (int i = 0; i < BLK; i++) { dl[i] += wl[i]; dr[i] += wr[i]; }
    }
    (void)p;
}

void engine_mix_finish(Engine *e, float *outL, float *outR)
{
    const Piece *p = e->p;
    float shelf = 1.8f * clamp01((0.5f - u(p->mood)) * 2.0f);   /* MOOD dark: the sub, up to +9 dB below ~110 Hz */
    for (int k = 0; k < e->nsub; k++) {
        float *dl = mx_dl + k * BLK, *dr = mx_dr + k * BLK, *vl = mx_vl + k * BLK, *vr = mx_vr + k * BLK;
        float *L = outL + k * BLK, *R = outR + k * BLK;
        for (int i = 0; i < BLK; i++) {
            float l = dl[i] + vl[i] * 0.55f, r = dr[i] + vr[i] * 0.55f;
            e->dc[0] += 0.0035f * (l - e->dc[0]); e->dc[1] += 0.0035f * (r - e->dc[1]);
            L[i] = l - e->dc[0]; R[i] = r - e->dc[1];
        }
        /* the balance: every piece's top and bottom about as most pieces have them. The lows (under 200 Hz) and the
         * highs (over 2.5 kHz) are weighed over seconds; where the highs stand more than the pieces' middle over the
         * lows (15 dB under them), the mix tilts darker round 700 Hz, and brighter where less: up to 6 dB, slowly,
         * held while stopped. So a bass-less, glassy piece comes out warmer, a murky one clearer; MOOD, WEAR and the
         * knobs still have their say on top */
        {
            const float alo = 1.0f - 0.9719f, ahi = 1.0f - 0.7003f, at = 1.0f - 0.9051f;   /* (one-poles: 200 Hz, 2.5 kHz, 700 Hz) */
            float slo = 0, shi = 0;
            for (int i = 0; i < BLK; i++) {
                float m = 0.5f * (L[i] + R[i]);
                e->tl_lo += alo * (m - e->tl_lo); e->tl_hi += ahi * (m - e->tl_hi);
                float h = m - e->tl_hi; slo += e->tl_lo * e->tl_lo; shi += h * h;
            }
            if (e->playing) {
                e->tl_elo += (slo / BLK - e->tl_elo) * (BLK_S / 5.0f); e->tl_ehi += (shi / BLK - e->tl_ehi) * (BLK_S / 5.0f);
                if (e->tl_elo > 1e-9f && e->tl_ehi > 1e-9f) {
                    float now_db = 10.0f * fm_log2f(e->tl_ehi / e->tl_elo) * 0.30103f;
                    float want = clampf(0.6f * (-15.0f - now_db), -6.0f, 6.0f);
                    e->tl_db += (want - e->tl_db) * (BLK_S / 4.0f);
                }
            }
            /* (lows times g, highs over g: tl_db between them). Always run, and g glided sample by sample from the
             * last block's, so the tilt never steps on a block edge */
            float g1 = fm_exp2f(-e->tl_db * (3.3219f / 40.0f)), g = e->tl_g > 0 ? e->tl_g : g1, gi = 1.0f / g;
            float dg = (g1 - g) / (float)BLK, dgi = (1.0f / g1 - gi) / (float)BLK;
            for (int i = 0; i < BLK; i++) {
                g += dg; gi += dgi;
                e->tl_s[0] += at * (L[i] - e->tl_s[0]); e->tl_s[1] += at * (R[i] - e->tl_s[1]);
                L[i] = e->tl_s[0] * g + (L[i] - e->tl_s[0]) * gi; R[i] = e->tl_s[1] * g + (R[i] - e->tl_s[1]) * gi;
            }
            e->tl_g = g1;
        }
        /* the softener: a dynamic EQ on the presence band (3.2 kHz, two octaves wide), where resonances and bright
         * engines turn harsh on monitors. When the band stands more than 9 dB under the whole (where most of the time it
         * sits lower), it's turned down by the excess, up to 8 dB, quickly (20 ms) and let go slowly (300 ms) */
        {
            if (e->de[0].k <= 0) { svf_set(&e->de[0], 3200.0f, 0.75f); e->de[1] = e->de[0]; }
            float k = e->de[0].k, sb = 0, sa = 0, bl[BLK], br[BLK];
            for (int i = 0; i < BLK; i++) {
                bl[i] = k * svf_bp(&e->de[0], L[i]); br[i] = k * svf_bp(&e->de[1], R[i]);
                sb += bl[i] * bl[i] + br[i] * br[i]; sa += L[i] * L[i] + R[i] * R[i];
            }
            e->de_band += (sb - e->de_band) * (sb > e->de_band ? 0.25f : 0.02f);
            e->de_all += (sa - e->de_all) * 0.02f;
            float over = e->de_all > 1e-9f && e->de_band > 1e-12f ? 10.0f * 0.30103f * fm_log2f(e->de_band / e->de_all) + 9.0f : -1.0f;
            float want = clampf(over, 0.0f, 8.0f);       /* dB to take off */
            e->de_cut += (want - e->de_cut) * (want > e->de_cut ? 0.08f : 0.02f);
            /* the cut glided sample by sample from the last block's, so it never steps on a block edge */
            float c1 = e->de_cut > 0.05f ? 1.0f - fm_exp2f(-e->de_cut * (3.3219f / 20.0f)) : 0.0f, c = e->de_c;
            if (c1 > 0.0f || c > 0.0f) {
                float dc = (c1 - c) / (float)BLK;
                for (int i = 0; i < BLK; i++) { c += dc; L[i] -= c * bl[i]; R[i] -= c * br[i]; }
            }
            e->de_c = c1;
        }
        wear_run(&e->wear, u(p->wear), L, R, BLK);        /* WEAR: the whole of it, the reverb's tails too */
        if (shelf > 0.001f)
            for (int i = 0; i < BLK; i++) {
                e->sub[0] += 0.0156f * (L[i] - e->sub[0]); e->sub[1] += 0.0156f * (R[i] - e->sub[1]);
                L[i] += shelf * e->sub[0]; R[i] += shelf * e->sub[1];
            }
        else e->sub[0] = e->sub[1] = 0;
        /* the bass in mono: below ~120 Hz the sides taken out (12 dB an octave), whatever the reverb, echo, chorus or DRIFT
         * did, so the low end sits in the middle, phase-safe on any system and on a club's mono sub */
        if (e->mbs[0].a1 == 0.0f) { svf_set(&e->mbs[0], 120.0f, 0.5412f); svf_set(&e->mbs[1], 120.0f, 1.3066f); }   /* (24 dB:
                                                                 * two, a Butterworth's pair of Qs) */
        for (int i = 0; i < BLK; i++) {
            float m = 0.5f * (L[i] + R[i]), sd = svf_hp(&e->mbs[1], svf_hp(&e->mbs[0], 0.5f * (L[i] - R[i])));
            L[i] = m + sd; R[i] = m - sd;
        }
        /* the leveller: pieces come out at about the same loudness (+-8 dB at most), over seconds, never pumping */
        float ss = 0;
        for (int i = 0; i < BLK; i++) ss += L[i] * L[i] + R[i] * R[i];
        e->lev_ss += (ss / (2.0f * BLK) - e->lev_ss) * (BLK_S / 4.0f);
        if (e->lev_g <= 0) e->lev_g = 1.0f;
        if (e->lev_ss > 1e-6f && e->playing) {   /* (stopped: held, so the fading tails aren't lifted, nor their grain) */
            float want = clampf(0.12f * fm_rsqrtf(e->lev_ss), 0.4f, 2.5f);
            e->lev_g += (want - e->lev_g) * (BLK_S / 3.0f);
        }
        for (int i = 0; i < BLK; i++) { L[i] *= e->lev_g; R[i] *= e->lev_g; }
    }
}

void engine_block_mix(Engine *e, float *outL, float *outR)   /* the stages one after another, on one core */
{
    uint32_t t0 = now(e);
    engine_mix_reverb(e);
    engine_mix_sends(e);
    engine_mix_echo(e);
    engine_mix_finish(e, outL, outR);
    cost_add(e, NBUS, t0);
}

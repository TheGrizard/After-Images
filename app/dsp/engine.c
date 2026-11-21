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
static void post(Engine *e, int layer, int note, float vel, float pan)
{
    UiNote *q = &e->uiq[e->uiq_head % UI_Q];
    q->layer = (uint8_t)layer; q->note = (uint8_t)note; q->vel = (uint8_t)(vel * 127.0f); q->pan = (int8_t)(pan * 100.0f);
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

void engine_all_off(Engine *e)
{
    for (int i = 0; i < e->nmo; i++) if (e->midi) e->midi(0x08u | (0x80u | e->mo[i].ch) << 8 | (uint32_t)e->mo[i].note << 16);
    e->nmo = 0;
}

/* ------------------------------------------------------------------ set-up */
static void layer_sound(const Engine *e, int l, Sound *s)
{
    const Piece *p = e->p; const LayerP *lp = &p->l[l];
    s->engine = lp->engine < NUM_ENGINES ? lp->engine : 0;
    s->tone = clamp01(u(lp->tone) + e->L[l].tone_off + (u(p->mood) - 0.5f) * 0.35f
                      + u(p->drift) * 0.25f * e->L[l].dtone.v);
    s->weight = u(lp->weight);
    s->weird = clamp01(u(lp->weird) + u(p->weird) * 0.6f);
    s->attack = attack_s(lp->attack); s->release = release_s(lp->release);
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
    e->p = p;
    e->gov = 1.0f; e->playing = 1; e->fade = 0.0f;
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l];
        L->nv = ROLES[l].poly; L->type_now = 255;
        L->present = L->present_t = 1.0f;
        L->dtone.len = 25.0f + 10.0f * (float)l; L->dpan.len = 13.0f + 5.0f * (float)l;
    }
    reverb_init(&e->rv, 1.3f);
    echo_init(&e->echo);
    grains_init(&e->gr, p->seed);
    e->mask = 0xF; e->chord_bars = 4; e->chord_left = 4; e->sec_left = 32;
    rng_seed(&e->rng, p->seed);
    e->seed_now = p->seed;
}

void engine_key(Engine *e, int on, int note, int vel)
{
    uint32_t w = e->key_w;
    if (w - e->key_r >= 16u) return;
    e->key_in[w % 16u] = (uint32_t)(on & 1) | (uint32_t)(note & 127) << 8 | (uint32_t)(vel & 127) << 16;
    e->key_w = w + 1;
}

/* ------------------------------------------------------------------ notes: a sequencer's degree becomes notes */
static void queue(VoiceEv *q, int *nq, int sub, int voice, int note, float vel, float dur, float pan, const Sound *snd, uint32_t seed)
{
    if (*nq >= EVQ) return;
    VoiceEv *v = &q[(*nq)++];
    v->sub = (uint8_t)sub; v->voice = (uint8_t)voice; v->rel = 0; v->note = (int8_t)note; v->vel = vel; v->dur = dur;
    v->pan = pan; v->snd = *snd; v->seed = seed;
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
    if (l == L_GROUND) {                             /* the root, its fifth, its octave: the floor under the chords */
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
    float beat_s = 60.0f / e->bpm, dur = sn->dur * beat_s, vel = sn->vel * (0.75f + 0.25f * u(lp->weight));
    float w = ROLES[l].width;
    for (int i = 0; i < nn; i++) {
        int note = notes[i];
        while (note < 12) note += 12;
        while (note > 120) note -= 12;
        float pan = sn->pan < 2.0f ? sn->pan * w : (nn > 1 ? ((float)i / (float)(nn - 1) * 2.0f - 1.0f) * w : rng_bi(&e->rng) * w);
        pan = clampf(pan + u(p->drift) * 0.3f * L->dpan.v, -1.0f, 1.0f);
        int vi = pick_voice(L->v, L->nv, &L->next_v);
        queue(L->q, &L->nq, sub, vi, note, vel / (nn > 1 ? 1.0f + 0.25f * (float)nn : 1.0f), dur, pan, &snd, rng_u32(&e->rng));
        midi_note(e, (e->midi_ch + l) & 15, note, vel, dur);
        post(e, l, note, vel, pan);
    }
    e->events[l]++;
    if (l == L_GROUND) e->duck = fm_maxf(e->duck, vel * u(lp->weight) * 0.45f);
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
    while (e->key_r != e->key_w) {
        uint32_t k = e->key_in[e->key_r % 16u]; e->key_r++;
        int on = (int)(k & 1u), note = (int)(k >> 8 & 127u); float vel = (float)(k >> 16 & 127u) / 127.0f;
        if (on) {
            Sound snd; layer_sound(e, e->key_layer, &snd);
            int vi = pick_voice(e->kv, KEY_POLY, &e->key_next);
            queue(e->kq, &e->nkq, sub, vi, note, vel, 600.0f, rng_bi(&e->rng) * 0.3f, &snd, rng_u32(&e->rng));
            post(e, e->key_layer, note, vel, 0);
            if (e->midi && e->midi_on) e->midi(0x09u | (0x90u | e->midi_keys_ch) << 8 | (uint32_t)note << 16 | (uint32_t)(vel * 127.0f) << 24);
        } else {
            for (int i = 0; i < KEY_POLY; i++)          /* (a key's voice: the one playing its note, held) */
                if (e->kv[i].on && e->kv[i].note == note && e->kv[i].hold > 0 && e->nkq < EVQ) {
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
    for (int l = 0; l < NUM_LAYERS; l++) if (rng_chance(&e->rng, l == L_BODY ? 0.85f : l == L_GROUND ? 0.75f : 0.65f)) m |= 1 << l;
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
    e->bpm = 40.0f + (float)p->tempo;
    float beats = BLK_S * e->bpm / 60.0f;
    e->t += BLK_S;
    if (e->seed_now != p->seed) {                    /* a new piece: the conductor starts again */
        e->seed_now = p->seed; rng_seed(&e->rng, p->seed);
        e->root = 0; e->chord_left = 4; e->sec_left = 24; e->mask = 0xF; e->beat = 0; e->bar = 0;
        grains_init(&e->gr, p->seed);
        engine_all_off(e);
    }
    keys_in(e, sub);
    midi_due(e);
    e->fade += ((e->playing ? 1.0f : 0.0f) - e->fade) * (BLK_S / 1.2f);
    if (!e->playing && e->fade < 0.01f) return;
    if (e->midi && e->midi_on && e->midi_clock) {    /* MIDI clock: 24 a beat */
        e->clock_acc += beats * 24.0f;
        while (e->clock_acc >= 1.0f) { e->clock_acc -= 1.0f; e->midi(0x0Fu | 0xF8u << 8); }
    }
    float prev = e->beat;
    e->beat += beats;
    if ((int)(e->beat * 0.25f) != (int)(prev * 0.25f)) bar(e);
    e->duck -= e->duck * (BLK_S / 0.12f);
    float motion = u(p->motion);
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l]; const LayerP *lp = &p->l[l];
        layer_seq_check(e, l);
        drift_step(&L->dtone, &e->rng, BLK_S); drift_step(&L->dpan, &e->rng, BLK_S);
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
static uint32_t now(Engine *e) { return e->clock ? e->clock() : 0; }
static void cost_add(Engine *e, int k, uint32_t t0) { if (e->clock) e->cost[k] += ((float)(now(e) - t0) - e->cost[k]) * 0.05f; }

void engine_block_begin(Engine *e, int nsub)
{
    uint32_t t0 = now(e);
    const Piece *p = e->p;
    e->nsub = nsub > ENGINE_SUB ? ENGINE_SUB : nsub < 1 ? 1 : nsub;
    for (int l = 0; l < NUM_LAYERS; l++) e->L[l].nq = 0;
    e->nkq = 0;
    for (int k = 0; k < e->nsub; k++) {
        control(e, k);
        for (int l = 0; l < NUM_LAYERS; l++) {
            float lv = u(p->l[l].level), g = lv * lv * e->L[l].present * e->fade * 1.6f;
            if (l == L_BODY || l == L_DUST) g *= 1.0f - e->duck;
            e->gains[k][l] = g;
        }
        e->gains[k][KEYS] = e->fade * 1.2f + 0.3f;
    }
    /* the bands: each layer's filters, from its role and its TONE */
    for (int l = 0; l < NUM_LAYERS; l++) {
        float t = u(p->l[l].tone), lp = ROLES[l].lp * (0.35f + 0.65f * t + 0.3f * (u(p->mood) - 0.5f));
        for (int c = 0; c < 2; c++) { svf_set(&e->L[l].hp[c], ROLES[l].hp, 0.6f); svf_set(&e->L[l].lp[c], lp, 0.6f); }
    }
    cost_add(e, NBUS, t0);
}

static void apply(VoiceEv *q, int nq, Voice *v, int sub)
{
    for (int i = 0; i < nq; i++) {
        if (q[i].sub != sub) continue;
        Voice *x = &v[q[i].voice];
        if (q[i].rel) voice_release(x);
        else voice_start(x, &q[i].snd, q[i].note, q[i].vel, q[i].dur, q[i].pan, 0.5f, 0.0f, q[i].seed);
    }
}

void engine_block_layer(Engine *e, int b)
{
    uint32_t t0 = now(e);
    float wg = u(e->p->weird);
    for (int k = 0; k < e->nsub; k++) {
        float *Lb = e->bus[k][b][0], *Rb = e->bus[k][b][1];
        memset(Lb, 0, sizeof(float) * BLK); memset(Rb, 0, sizeof(float) * BLK);
        if (b == KEYS) {
            apply(e->kq, e->nkq, e->kv, k);
            for (int i = 0; i < KEY_POLY; i++) { e->kv[i].ws = wg > 0.5f ? (wg - 0.5f) * 1.4f : 0.0f; voice_render(&e->kv[i], Lb, Rb, BLK); }
            continue;
        }
        Layer *L = &e->L[b];
        apply(L->q, L->nq, L->v, k);
        int any = 0;
        for (int i = 0; i < L->nv; i++) if (L->v[i].on) { L->v[i].ws = wg > 0.5f ? (wg - 0.5f) * 1.4f : 0.0f; voice_render(&L->v[i], Lb, Rb, BLK); any = 1; }
        if (!any) continue;
        for (int i = 0; i < BLK; i++) {
            Lb[i] = svf_lp(&L->lp[0], svf_hp(&L->hp[0], Lb[i]));
            Rb[i] = svf_lp(&L->lp[1], svf_hp(&L->hp[1], Rb[i]));
        }
    }
    cost_add(e, b, t0);
}

void engine_block_mix(Engine *e, float *outL, float *outR)
{
    uint32_t t0 = now(e);
    const Piece *p = e->p;
    float rvk = u(p->reverb), eck = u(p->echo), grk = u(p->grain), shk = u(p->shimmer);
    reverb_set(&e->rv, 2.5f + 14.0f * rvk * rvk, 2500.0f + 6000.0f * u(p->mood));
    e->echo.time_s = clampf(60.0f / e->bpm * 0.75f, 0.12f, 0.7f);   /* a dotted eighth */
    while (e->echo.time_s > 0.7f) e->echo.time_s *= 0.5f;
    e->echo.fb = 0.2f + 0.65f * eck; e->echo.wow = u(p->warp) + u(p->drift) * 0.5f;
    e->gr.density = grk < 0.9f ? grk : 1.0f; e->gr.frozen = grk >= 0.92f; e->gr.size = 0.3f + 0.7f * grk;
    e->gr.spread = 0.3f + 0.7f * grk; e->gr.shimmer = shk;
    {   /* the grains' pitches: octaves, and the fifth only where the scale has one */
        const Scale *sc = &SCALES[p->scale % NUM_SCALES]; int has5 = 0;
        for (int i = 0; i < sc->n; i++) if (sc->st[i] == 7) has5 = 1;
        e->gr_ratio[0] = 1; e->gr_ratio[1] = 1; e->gr_ratio[2] = 2; e->gr_ratio[3] = 0.5f;
        e->gr_ratio[4] = has5 ? 1.5f : 2.0f; e->gr_ratio[5] = has5 ? 0.75f : 1.0f;
        e->gr.ratios = e->gr_ratio; e->gr.nratios = 6;
    }
    for (int k = 0; k < e->nsub; k++) {
        float dl[BLK] = { 0 }, dr[BLK] = { 0 }, rl[BLK] = { 0 }, rr[BLK] = { 0 }, el[BLK] = { 0 }, er[BLK] = { 0 }, gm[BLK] = { 0 };
        for (int b = 0; b < NBUS; b++) {
            float g = e->gains[k][b], *Lb = e->bus[k][b][0], *Rb = e->bus[k][b][1];
            float rs = (b < NUM_LAYERS ? ROLES[b].rv : 0.4f) * (0.3f + 1.2f * rvk) * g;
            float es = (b < NUM_LAYERS ? ROLES[b].echo : 0.3f) * eck * 1.4f * g;
            float gs = (b < NUM_LAYERS ? ROLES[b].grain : 0.3f) * g;
            float ss = 0;
            for (int i = 0; i < BLK; i++) {
                float l = Lb[i], r = Rb[i];
                dl[i] += l * g; dr[i] += r * g;
                rl[i] += l * rs; rr[i] += r * rs;
                el[i] += l * es; er[i] += r * es;
                gm[i] += (l + r) * 0.5f * gs;
                ss += l * l + r * r;
            }
            if (b < NUM_LAYERS) {
                float rms = fm_sqrtf(ss * g * g / (2.0f * BLK));
                float lvl = rms > 1e-5f ? clamp01((fm_log2f(rms) * 6.02f + 48.0f) / 42.0f) : 0.0f;
                e->meter[b] += (lvl - e->meter[b]) * (lvl > e->meter[b] ? 0.3f : 0.02f);
            }
        }
        float ol[BLK], orr[BLK], gl[BLK], grr[BLK], sh[BLK];
        echo_run(&e->echo, el, er, ol, orr, BLK);
        grains_run(&e->gr, gm, gl, grr, sh, BLK);
        for (int i = 0; i < BLK; i++) {
            float gg = grk * 1.3f;
            dl[i] += ol[i] + gl[i] * gg; dr[i] += orr[i] + grr[i] * gg;
            rl[i] += ol[i] * 0.3f + gl[i] * gg * 0.5f + sh[i] * shk * 1.6f;
            rr[i] += orr[i] * 0.3f + grr[i] * gg * 0.5f + sh[i] * shk * 1.6f;
        }
        float vl[BLK], vr[BLK];
        reverb_run(&e->rv, rl, rr, vl, vr, BLK);
        float *L = outL + k * BLK, *R = outR + k * BLK;
        for (int i = 0; i < BLK; i++) {
            float l = dl[i] + vl[i] * 0.55f, r = dr[i] + vr[i] * 0.55f;
            e->dc[0] += 0.0035f * (l - e->dc[0]); e->dc[1] += 0.0035f * (r - e->dc[1]);
            L[i] = l - e->dc[0]; R[i] = r - e->dc[1];
        }
    }
    cost_add(e, NBUS, t0);
}

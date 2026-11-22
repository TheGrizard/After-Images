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
static void post(Engine *e, int layer, int note, float vel, float pan, float dur)
{
    UiNote *q = &e->uiq[e->uiq_head % UI_Q];
    q->dur = (uint8_t)clampf(dur * 10.0f, 1.0f, 255.0f); q->layer = (uint8_t)layer; q->note = (uint8_t)note; q->vel = (uint8_t)(vel * 127.0f); q->pan = (int8_t)(pan * 100.0f);
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
/* the patch: which layer's activity each layer listens to (never its own), from the piece's seed */
static void patch(Engine *e, uint32_t seed)
{
    Rng r; rng_seed(&r, seed ^ 0xCAB1Eu);
    for (int l = 0; l < NUM_LAYERS; l++) e->L[l].src = (l + 1 + rng_int(&r, NUM_LAYERS - 1)) % NUM_LAYERS;
}

static void layer_sound(const Engine *e, int l, Sound *s)
{
    const Piece *p = e->p; const LayerP *lp = &p->l[l];
    s->engine = lp->engine < NUM_ENGINES ? lp->engine : 0;
    s->tone = clamp01(0.42f + (u(lp->tone) - 0.5f) * 0.5f + e->L[l].tone_off + (u(p->mood) - 0.6f) * 0.35f
                      + u(p->drift) * 0.25f * e->L[l].dtone.v);
    s->weight = u(lp->weight);
    s->weird = u(lp->weird);
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
    e->p = p;
    e->gov = 1.0f; e->playing = 1; e->fade = 0.0f;
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l];
        L->nv = ROLES[l].poly; L->type_now = 255;
        L->present = L->present_t = 1.0f;
        L->dtone.len = 25.0f + 10.0f * (float)l; L->dpan.len = 13.0f + 5.0f * (float)l;
        L->dwalk.len = 20.0f + 7.0f * (float)l; L->im = L->im_t = L->im_g = 1.0f; L->im_l = L->im_r = 1.0f;
    }
    reverb_init(&e->rv, 1.3f);
    echo_init(&e->echo);
    grains_init(&e->gr, p->seed);
    for (int l = 0; l < NUM_LAYERS; l++) { dirt_init(&e->L[l].dirt, p->seed + (uint32_t)l); dirt_init(&e->L[l].ent, p->seed * 3u + (uint32_t)l); e->L[l].gate = 1.0f; }
    patch(e, p->seed);
    e->mask = 0xF; e->chord_bars = 4; e->chord_left = 4; e->sec_left = 32;
    rng_seed(&e->rng, p->seed);
    e->seed_now = p->seed;
}

void engine_key(Engine *e, int on, int note, int vel, int layer)
{
    uint32_t w = e->key_w;
    if (w - e->key_r >= 16u) return;
    e->key_in[w % 16u] = (uint32_t)(on & 1) | (uint32_t)(note & 127) << 8 | (uint32_t)(vel & 127) << 16 | (uint32_t)(layer + 1) << 24;
    e->key_w = w + 1;
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
            int vi = pick_voice(L->v, L->nv, &L->next_v);
            queue(L->q, &L->nq, sub, vi, note, nv, dur, pan, &snd, rng_u32(&e->rng));
            if (p->glide[l] && i < L->last_n) L->q[L->nq - 1].from_hz = L->last_hz[i];   /* GLIDE: from the last note */
            L->last_v[i] = (int8_t)vi; L->last_hz[i] = mtof((float)note);
        }
        midi_note(e, (e->midi_ch + l) & 15, note, vel, dur);
        post(e, l, note, vel, pan, dur);
    }
    L->last_n = nn;
    e->events[l]++;
    L->cv = fm_maxf(L->cv, sn->vel);
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
        int on = (int)(k & 1u), note = (int)(k >> 8 & 127u), lay = (int)(k >> 24 & 7u) - 1; float vel = (float)(k >> 16 & 127u) / 127.0f;
        if (lay < 0 || lay >= NUM_LAYERS) lay = e->key_layer;
        if (on) {
            Sound snd; layer_sound(e, lay, &snd);
            int vi = pick_voice(e->kv, KEY_POLY, &e->key_next);
            queue(e->kq, &e->nkq, sub, vi, note, vel, 600.0f, rng_bi(&e->rng) * 0.3f, &snd, rng_u32(&e->rng));
            post(e, lay, note, vel, 0, 1.5f);
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
    e->bpm = e->ext_bpm > 20.0f ? clampf(e->ext_bpm, 30.0f, 240.0f) : 40.0f + (float)p->tempo;
    float beats = BLK_S * e->bpm / 60.0f;
    e->t += BLK_S;
    if (e->seed_now != p->seed) {                    /* a new piece: the conductor starts again */
        e->seed_now = p->seed; rng_seed(&e->rng, p->seed);
        e->root = 0; e->chord_left = 4; e->sec_left = 24; e->mask = 0xF; e->beat = 0; e->bar = 0;
        grains_init(&e->gr, p->seed);
        patch(e, p->seed);
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
        }
    }
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l];
        if (L->im <= 0) L->im = 1.0f;
        L->im += (L->im_t - L->im) * clamp01(dt / (6.0f + 6.0f * (float)l * 0.5f));   /* slow: seconds, never steps */
        L->im_pan = drift_step(&L->dwalk, &e->rng, dt);
        float far = 1.0f - L->im;                                /* how far off it has gone */
        L->im_g = 1.0f - d * far;                                /* its level (at DRIFT 0: as set) */
        L->im_far = d * far;
        float pn = d * 0.85f * L->im_pan, a = (pn + 1.0f) * 0.25f * PI_F;   /* its walk round the stereo field */
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

void engine_block_begin(Engine *e, int nsub)
{
    uint32_t t0 = now(e);
    const Piece *p = e->p;
    e->nsub = nsub > ENGINE_SUB ? ENGINE_SUB : nsub < 1 ? 1 : nsub;
    for (int l = 0; l < NUM_LAYERS; l++) e->L[l].nq = 0;
    e->nkq = 0;
    warble(e);
    immerse(e);
    for (int k = 0; k < e->nsub; k++) {
        control(e, k);
        for (int l = 0; l < NUM_LAYERS; l++) {
            float lv = u(p->l[l].level), g = lv * lv * e->L[l].present * e->fade * 1.6f * e->L[l].im_g;   /* (DRIFT's fading) */
            if (l == L_BODY || l == L_DUST) g *= 1.0f - e->duck;
            e->gains[k][l] = g;
        }
        e->gains[k][KEYS] = e->fade * 1.2f + 0.3f;
    }
    /* the bands: each layer's filters, from its role; TONE a DJ's filter on them, at 12 o'clock open, to the left a
     * low-pass sweeping down to a thud, to the right a high-pass sweeping up to air, a little resonance towards the ends */
    for (int l = 0; l < NUM_LAYERS; l++) {
        Layer *L = &e->L[l];
        float t = u(p->l[l].tone), lo = t < 0.5f ? (0.5f - t) * 2.0f : 0.0f, hi = t > 0.5f ? (t - 0.5f) * 2.0f : 0.0f;
        float lp = ROLES[l].lp * (0.85f + 0.3f * (u(p->mood) - 0.5f)) * fm_exp2f(-7.0f * lo);
        float hp = ROLES[l].hp * fm_exp2f(6.5f * hi + 3.5f * L->im_far);   /* (DRIFT: far off, its lows thin) */
        /* patched: the source layer's activity opens this one's filter, the random voltage moves it (LINK, DRIFT) */
        lp *= fm_exp2f(u(p->link) * 1.8f * (e->L[L->src].cv - 0.25f) + u(p->drift) * 0.9f * L->rnd);
        lp = clampf(lp, 40.0f, 18000.0f); hp = clampf(hp, 15.0f, 12000.0f);
        if (L->lp_hz <= 0) { L->lp_hz = lp; L->hp_hz = hp; }
        L->lp_hz += (lp - L->lp_hz) * 0.35f; L->hp_hz += (hp - L->hp_hz) * 0.35f;   /* (turned smoothly, no steps) */
        float q = 0.6f + 1.4f * (lo > hi ? lo : hi) * (lo > hi ? lo : hi);
        for (int c = 0; c < 2; c++) { svf_set(&L->hp[c], L->hp_hz, hi > 0 ? q : 0.6f); svf_set(&L->lp[c], L->lp_hz, lo > 0 ? q : 0.6f); }
    }
    cost_add(e, NBUS, t0);
}

static void apply(VoiceEv *q, int nq, Voice *v, int sub)
{
    for (int i = 0; i < nq; i++) {
        if (q[i].sub != sub) continue;
        Voice *x = &v[q[i].voice];
        if (q[i].rel == 1) voice_release(x);
        else if (q[i].rel == 2) voice_legato(x, q[i].note, q[i].vel, q[i].dur);
        else voice_start(x, &q[i].snd, q[i].note, q[i].vel, q[i].dur, q[i].pan, q[i].from_hz, q[i].seed);
    }
}

void engine_block_layer(Engine *e, int b)
{
    uint32_t t0 = now(e);
    float wg = 0.0f;                     /* (the piece's ring modulator: gone; each layer's EXTREME has its own strangeness) */
    for (int k = 0; k < e->nsub; k++) {
        float *Lb = e->bus[k][b][0], *Rb = e->bus[k][b][1];
        memset(Lb, 0, sizeof(float) * BLK); memset(Rb, 0, sizeof(float) * BLK);
        if (b == KEYS) {
            apply(e->kq, e->nkq, e->kv, k);
            for (int i = 0; i < KEY_POLY; i++) { e->kv[i].ws = wg > 0.5f ? (wg - 0.5f) * 1.4f : 0.0f; e->kv[i].pm = e->pm[KEYS]; voice_render(&e->kv[i], Lb, Rb, BLK); }
            continue;
        }
        Layer *L = &e->L[b];
        apply(L->q, L->nq, L->v, k);
        int any = 0;
        for (int i = 0; i < L->nv; i++) if (L->v[i].on) { L->v[i].ws = wg > 0.5f ? (wg - 0.5f) * 1.4f : 0.0f; L->v[i].pm = e->pm[b]; voice_render(&L->v[i], Lb, Rb, BLK); any = 1; }
        if (!any) continue;
        for (int i = 0; i < BLK; i++) {
            Lb[i] = svf_lp(&L->lp[0], svf_hp(&L->hp[0], Lb[i]));
            Rb[i] = svf_lp(&L->lp[1], svf_hp(&L->hp[1], Rb[i]));
        }
        dirt_run(&L->dirt, u(e->p->dirt), u(e->p->s_dirt[b]), Lb, Rb, BLK);   /* its own DIRT, its send's amount */
        /* ENTROPY past 40: the sound starts to destroy itself. Folding first, then crushed, then noise; past 60 holes torn
         * in it, longer and more often towards the end, until it is more broken than whole */
        float en = clamp01((u(e->p->l[b].weird) - 0.4f) / 0.6f);
        if (en > 0.001f) {
            dirt_run(&L->ent, en, fm_minf(1.0f, en * 1.6f), Lb, Rb, BLK);
            float tear = clamp01((en - 0.33f) * 1.5f);
            if ((L->gate_left -= BLK_S) <= 0) {
                L->gate_hole = tear > 0 && (float)(L->ent.rng.s & 1023u) / 1023.0f < tear * 0.55f ? 0.0f : 1.0f;
                rng_u32(&L->ent.rng);
                L->gate_left = 0.02f + 0.2f * (float)(L->ent.rng.s & 255u) / 255.0f * (0.3f + tear);
            }
            for (int i = 0; i < BLK; i++) { L->gate += (L->gate_hole - L->gate) * 0.08f; Lb[i] *= L->gate; Rb[i] *= L->gate; }
        }
    }
    cost_add(e, b, t0);
}

void engine_block_mix(Engine *e, float *outL, float *outR)
{
    uint32_t t0 = now(e);
    const Piece *p = e->p;
    /* REVERB: up to 12 o'clock the reverb; past it the shimmer comes in, an octave up, and by the end down too.
     * ECHO: up to 12 o'clock the echo; past it, it turns into a reversed smear */
    float rk = u(p->reverb), rvk = clamp01(rk * 2.0f), shk = clamp01(rk * 2.0f - 1.0f);
    float ek = u(p->echo), eck = clamp01(ek * 2.0f), rev = clamp01(ek * 2.0f - 1.0f);
    float grk = u(p->grain);
    reverb_set(&e->rv, 2.5f + 12.0f * rvk * rvk - 3.0f * shk, 1800.0f + 4200.0f * u(p->mood) + 4000.0f * shk);
    e->echo.time_s = clampf(60.0f / e->bpm * 0.75f, 0.12f, 0.7f);   /* a dotted eighth */
    while (e->echo.time_s > 0.7f) e->echo.time_s *= 0.5f;
    e->echo.fb = rev > 0 ? 0.45f + 0.3f * rev : 0.2f + 0.45f * eck; e->echo.wow = u(p->warp) + u(p->drift) * 0.5f;
    e->echo.rev = rev;
    e->gr.density = grk < 0.9f ? grk : 1.0f; e->gr.frozen = grk >= 0.92f; e->gr.size = 0.3f + 0.7f * grk;
    e->gr.spread = 0.3f + 0.7f * grk; e->gr.shimmer = 0;   /* (SHIMMER gave way to DIRT) */
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
            /* the sends: each layer's own (SENDS), the keys' the chosen layer's */
            int sl = b < NUM_LAYERS ? b : e->key_layer;
            float far = b < NUM_LAYERS ? e->L[b].im_far : 0.0f, pl = b < NUM_LAYERS ? e->L[b].im_l : 1.0f, pr = b < NUM_LAYERS ? e->L[b].im_r : 1.0f;
            float rs = u(p->s_rv[sl]) * 0.8f * (0.3f + 1.2f * rvk) * g * (1.0f + 1.5f * far);   /* (far off: more reverb than itself) */
            float es = u(p->s_echo[sl]) * eck * 1.4f * g;
            float gs = u(p->s_grain[sl]) * 0.9f * g;
            float ss = 0;
            for (int i = 0; i < BLK; i++) {
                float l = Lb[i], r = Rb[i];
                dl[i] += l * g * pl; dr[i] += r * g * pr;
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
            rl[i] += ol[i] * 0.3f + gl[i] * gg * 0.5f;
            rr[i] += orr[i] * 0.3f + grr[i] * gg * 0.5f;
        }
        (void)sh;
        if (shk > 0.001f) {                              /* the shimmer: the reverb's last block, an octave up and down, back in */
            float up[BLK], dn_[BLK], gu = shk * 0.3f, gd = shk * shk * 0.26f;
            shifter_run(&e->shim, e->shim_prev, up, dn_, BLK);
            for (int i = 0; i < BLK; i++) { float x = up[i] * gu + dn_[i] * gd; rl[i] += x; rr[i] += x; }
        }
        chorus_run(&e->ch, u(p->chorus), dl, dr, BLK);  /* the chorus on the dry sound (the reverb's sends taken before it) */
        float vl[BLK], vr[BLK];
        reverb_run(&e->rv, rl, rr, vl, vr, BLK);
        for (int i = 0; i < BLK; i++) e->shim_prev[i] = (vl[i] + vr[i]) * 0.5f;
        float *L = outL + k * BLK, *R = outR + k * BLK;
        for (int i = 0; i < BLK; i++) { dl[i] += vl[i] * 0.55f; dr[i] += vr[i] * 0.55f; }
        for (int i = 0; i < BLK; i++) {
            float l = dl[i], r = dr[i];
            e->dc[0] += 0.0035f * (l - e->dc[0]); e->dc[1] += 0.0035f * (r - e->dc[1]);
            L[i] = l - e->dc[0]; R[i] = r - e->dc[1];
        }
        /* the leveller: pieces come out at about the same loudness (+-8 dB at most), over seconds, never pumping */
        float ss = 0;
        for (int i = 0; i < BLK; i++) ss += L[i] * L[i] + R[i] * R[i];
        e->lev_ss += (ss / (2.0f * BLK) - e->lev_ss) * (BLK_S / 4.0f);
        if (e->lev_g <= 0) e->lev_g = 1.0f;
        if (e->lev_ss > 1e-6f) {
            float want = clampf(0.12f * fm_rsqrtf(e->lev_ss), 0.4f, 2.5f);
            e->lev_g += (want - e->lev_g) * (BLK_S / 3.0f);
        }
        for (int i = 0; i < BLK; i++) { L[i] *= e->lev_g; R[i] *= e->lev_g; }
    }
    cost_add(e, NBUS, t0);
}

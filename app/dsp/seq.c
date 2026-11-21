/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the sequencers (seq.h). */
#include "seq.h"

const char *const SEQ_NAME[NUM_SEQS] = {
    "EUCLID", "TURING", "MARKOV", "LORENZ", "BOUNCE", "ORBITS", "LIFE", "CHIMES", "PHASE", "DRUNK"
};

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void emit_raw(Seq *s, int deg, float vel, float dur, float pan)
{
    if (s->nout >= SEQ_OUT) return;
    SeqNote *o = &s->out[s->nout++];
    o->deg = deg; o->vel = clamp01(vel); o->dur = dur; o->pan = pan;
}

/* a note from the system; CHAOS may drop it, double it into a ratchet, or throw it far */
static void emit(Seq *s, int deg, float vel, float dur, float pan)
{
    float c = s->chaos;
    deg = clampi(deg, 0, s->range - 1);
    if (c > 0.01f) {
        if (rng_chance(&s->rng, c * 0.18f)) return;
        if (rng_chance(&s->rng, c * 0.12f)) deg = clampi(deg + (rng_chance(&s->rng, 0.5f) ? 5 : -5), 0, s->range - 1);
        if (!s->ratchet_left && rng_chance(&s->rng, c * 0.3f)) {
            s->ratchet_left = 1 + rng_int(&s->rng, 3);
            s->ratchet_dt = rng_chance(&s->rng, 0.5f) ? 0.125f : 1.0f / 6.0f;
            s->ratchet_t = s->ratchet_dt;
            s->ratchet_n = (SeqNote){ deg, vel * 0.8f, s->ratchet_dt * 0.8f, pan };
        }
    }
    emit_raw(s, deg, vel, dur, pan);
}

/* ------------------------------------------------------------------ set-up */
static void mel_new(Seq *s, int m)
{
    s->m = m;
    int d = rng_int(&s->rng, s->range / 2 + 1);
    for (int i = 0; i < m; i++) {
        s->mel[i] = (int8_t)d;
        d = clampi(d + rng_int(&s->rng, 5) - 2, 0, s->range - 1);
    }
}

void seq_init(Seq *s, int type, int range, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->type = (uint8_t)(type % NUM_SEQS); s->range = range < 2 ? 2 : range > 24 ? 24 : range;
    s->density = 0.5f;
    rng_seed(&s->rng, seed ^ (0x51u * (uint32_t)(type + 1)));
    Rng *r = &s->rng;
    switch (s->type) {
    case S_EUCLID:
        s->n = 5 + rng_int(r, 12); s->rot = rng_int(r, s->n);
        mel_new(s, 3 + rng_int(r, 5));
        break;
    case S_TURING:
        s->reg = rng_u32(r); s->len = 8 + rng_int(r, 9);
        break;
    case S_MARKOV:
        for (int a = 0; a < 8; a++)
            for (int b = 0; b < 8; b++) {
                int iv = b - a < 0 ? a - b : b - a;      /* steps and thirds likely, leaps rare */
                int w = iv == 0 ? 1 : iv == 1 ? 6 : iv == 2 ? 5 : iv == 3 ? 2 : iv == 4 ? 3 : 1;
                s->mk[a][b] = (uint8_t)(w + rng_int(r, 4) * (rng_chance(r, 0.3f) ? 1 : 0));
            }
        s->idx = rng_int(r, 8); s->len = 0;
        break;
    case S_LORENZ:
        s->x = rng_range(r, -8, 8); s->y = rng_range(r, -8, 8); s->z = rng_range(r, 15, 30); s->rho = 28.0f;
        break;
    case S_BOUNCE:
        s->h = 1.0f; s->v = 0; s->e = 0.72f; s->base = rng_int(r, s->range); s->k = rng_chance(r, 0.5f) ? 1 : -1;
        s->g = 0;
        break;
    case S_ORBITS: {
        static const float P[8] = { 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 12.0f };   /* years, in beats */
        for (int i = 0; i < 4; i++) {
            s->per[i] = P[rng_int(r, 8)] * (rng_chance(r, 0.3f) ? 1.5f : 1.0f);
            s->ph[i] = rng_f(r); s->pdeg[i] = (int8_t)rng_int(r, s->range);
        }
        break;
    }
    case S_LIFE: {
        static const uint8_t RULES[8] = { 30, 90, 110, 45, 73, 150, 57, 105 };
        s->rule = RULES[rng_int(r, 8)]; s->cells = (uint16_t)(rng_u32(r) | 1u); s->n = 16;
        break;
    }
    case S_CHIMES:
        for (int i = 0; i < 5; i++) s->mel[i] = (int8_t)rng_int(r, s->range);
        s->wind = 0.3f; s->wleft = 0;
        break;
    case S_PHASE:
        s->m = 5 + rng_int(r, 4);
        mel_new(s, s->m);
        s->cells = (uint16_t)(rng_u32(r) | 1u);       /* which of the phrase's steps play */
        s->rate2 = 1.0f + rng_range(r, 0.006f, 0.02f);
        break;
    case S_DRUNK:
    default:
        s->idx = s->range / 2;
        break;
    }
}

/* ------------------------------------------------------------------ the step types: one call a step (a 16th) */
static void step_euclid(Seq *s)
{
    int k = clampi((int)(s->density * s->n * 1.1f + 0.5f), 1, s->n);
    int i = (s->pos + s->rot) % s->n;
    int hit = ((i + 1) * k) / s->n != (i * k) / s->n;
    if (hit) {
        emit(s, s->mel[s->mpos % s->m], 0.55f + 0.45f * (i == 0), 1.2f, 9);
        s->mpos++;
    }
    if (++s->pos >= s->n) {                          /* a cycle: MUTATE may change a note or turn the ring */
        s->pos = 0;
        if (rng_chance(&s->rng, s->mutate * 0.5f)) s->mel[rng_int(&s->rng, s->m)] = (int8_t)clampi(s->mel[0] + rng_int(&s->rng, 7) - 3, 0, s->range - 1);
        if (rng_chance(&s->rng, s->mutate * 0.2f)) s->rot = (s->rot + 1) % s->n;
    }
}

static void step_turing(Seq *s)
{
    uint32_t mask = (1u << s->len) - 1u;
    uint32_t bit = (s->reg >> (s->len - 1)) & 1u;
    if (rng_chance(&s->rng, s->mutate * s->mutate * 0.5f)) bit ^= 1u;
    s->reg = ((s->reg << 1) | bit) & mask;
    uint32_t v = s->reg & 0xFFu;
    if ((float)((v * 151u) & 255u) / 255.0f < s->density * 1.2f)
        emit(s, (int)(v * (uint32_t)s->range / 256u), 0.5f + 0.5f * (float)(s->reg >> 8 & 3u) / 3.0f, 1.0f + (float)(v & 3u), 9);
}

static void step_markov(Seq *s)
{
    if (s->len-- > 0) return;
    int a = s->idx, sum = 0, w[8];
    for (int b = 0; b < 8; b++) { w[b] = s->mk[a][b]; sum += w[b]; }
    int x = rng_int(&s->rng, sum), b = 0;
    while (b < 7 && x >= w[b]) { x -= w[b]; b++; }
    s->idx = b;
    static const int DUR[6] = { 1, 2, 2, 3, 4, 6 };
    int d = DUR[rng_int(&s->rng, 6)];
    if (s->density < 0.5f) d += rng_int(&s->rng, 1 + (int)((0.5f - s->density) * 16.0f));
    else if (d > 1 && rng_chance(&s->rng, (s->density - 0.5f) * 1.6f)) d /= 2;
    s->len = d - 1;
    emit(s, b * (s->range - 1) / 7, 0.6f + 0.3f * rng_f(&s->rng), (float)d * 0.25f * 1.5f, 9);
    if (rng_chance(&s->rng, s->mutate * 0.1f)) s->mk[rng_int(&s->rng, 8)][rng_int(&s->rng, 8)] = (uint8_t)rng_int(&s->rng, 8);
}

static void step_life(Seq *s)
{
    int c = s->pos % 16;
    if ((s->cells >> c & 1u) && rng_chance(&s->rng, 0.3f + s->density))
        emit(s, (c * 5) % s->range, 0.5f + 0.4f * (c % 4 == 0), 0.9f, (float)c / 7.5f - 1.0f);
    if (++s->pos >= 16) {                            /* a generation */
        s->pos = 0;
        uint16_t old = s->cells, nw = 0;
        for (int i = 0; i < 16; i++) {
            int l = old >> ((i + 15) % 16) & 1, m = old >> i & 1, rr = old >> ((i + 1) % 16) & 1;
            if (s->rule >> (l << 2 | m << 1 | rr) & 1u) nw |= (uint16_t)(1u << i);
        }
        if (rng_chance(&s->rng, s->mutate * 0.3f)) nw ^= (uint16_t)(1u << rng_int(&s->rng, 16));
        if (nw == 0 || nw == 0xFFFFu || nw == old) nw = (uint16_t)(rng_u32(&s->rng) | 1u);   /* dead or stuck: new seed */
        s->cells = nw;
    }
}

static void step_phase(Seq *s)
{
    int i = s->pos % s->m;
    if (s->cells >> i & 1u) emit(s, s->mel[i], 0.65f, 0.9f, -0.6f);
    s->pos++;
}

static void step_drunk(Seq *s)
{
    if (!rng_chance(&s->rng, 0.15f + 0.7f * s->density)) return;
    int r = rng_int(&s->rng, 10), d = r < 3 ? 1 : r < 6 ? -1 : r < 8 ? 2 : -2;
    if (rng_chance(&s->rng, s->mutate * 0.25f)) d *= 3;
    s->idx += d;
    if (s->idx < 0) s->idx = -s->idx;
    if (s->idx >= s->range) s->idx = 2 * (s->range - 1) - s->idx;
    s->idx = clampi(s->idx, 0, s->range - 1);
    emit(s, s->idx, 0.5f + 0.4f * rng_f(&s->rng), 1.0f + (float)rng_int(&s->rng, 3), 9);
}

/* ------------------------------------------------------------------ the continuous types: `b` beats a call */
static void run_lorenz(Seq *s, float b)
{
    float dt = b * (0.2f + 0.7f * s->density);     /* (a peak every beat or two) */
    for (int k = 0; k < 2; k++) {
        float h = dt * 0.5f;
        float dx = 10.0f * (s->y - s->x), dy = s->x * (s->rho - s->z) - s->y, dz = s->x * s->y - 2.6667f * s->z;
        float px = s->x;
        s->x += dx * h; s->y += dy * h; s->z += dz * h;
        if (s->dz > 0 && dz <= 0)                    /* a loop's peak */
            emit(s, (int)((s->x + 20.0f) / 40.0f * (float)s->range), clamp01(s->z / 45.0f), 1.5f, clampf(s->y / 25.0f, -1, 1));
        if ((px < 0) != (s->x < 0) && rng_chance(&s->rng, 0.5f))   /* it crosses to the other wing: an accent */
            emit(s, s->x < 0 ? 0 : s->range / 2, 0.9f, 3.0f, 9);
        s->dz = dz;
    }
    s->rho += (24.0f + 10.0f * s->mutate - s->rho) * 0.001f * b;
}

static void run_bounce(Seq *s, float b)
{
    if (s->g > 0) { s->g -= b; if (s->g <= 0) { s->h = 1.0f; s->v = 0; s->cnt = 0; s->e = 0.6f + 0.25f * rng_f(&s->rng); s->base = rng_int(&s->rng, s->range); s->k = rng_chance(&s->rng, 0.5f) ? 1 : -1; } return; }
    float grav = 3.0f;                               /* (a ball dropped from 1 takes ~0.8 beats to land) */
    s->v -= grav * b; s->h += s->v * b;
    if (s->h <= 0) {
        s->h = 0; float sp = -s->v; s->v = sp * s->e;
        emit(s, s->base + s->k * s->cnt * (1 + (s->mutate > 0.5f)), clamp01(sp / 2.5f), 0.6f, 9);
        s->cnt++;
        if (s->v < 0.12f || s->cnt > 14) s->g = 1.0f + (1.0f - s->density) * 7.0f * rng_f(&s->rng);   /* settled: the next ball */
    }
}

static void run_orbits(Seq *s, float b)
{
    int np = 1 + (int)(s->density * 3.0f + 0.5f);
    for (int i = 0; i < 4 && i < np; i++) {
        s->ph[i] += b / s->per[i];
        if (s->ph[i] >= 1.0f) {
            s->ph[i] -= 1.0f;
            if (rng_chance(&s->rng, s->mutate * 0.25f)) s->pdeg[i] = (int8_t)clampi(s->pdeg[i] + rng_int(&s->rng, 5) - 2, 0, s->range - 1);
            emit(s, s->pdeg[i], 0.5f + 0.15f * (float)i, s->per[i] * 0.5f, (float)i / 1.5f - 1.0f);
        }
    }
}

static void run_chimes(Seq *s, float b)
{
    if ((s->wleft -= b) <= 0) { s->wtarget = rng_f(&s->rng); s->wleft = 4.0f + 12.0f * rng_f(&s->rng); }
    s->wind += (s->wtarget - s->wind) * clamp01(b * 0.2f);
    for (int i = 0; i < s->ngust; i++)
        if ((s->gust_t[i] -= b) <= 0) {
            emit(s, s->gust_d[i], 0.3f + 0.6f * s->wind * rng_f(&s->rng), 2.5f, rng_bi(&s->rng));
            s->gust_t[i] = s->gust_t[--s->ngust]; s->gust_d[i] = s->gust_d[s->ngust]; i--;
        }
    if (s->ngust == 0 && rng_chance(&s->rng, b * s->wind * (0.1f + 1.2f * s->density))) {   /* a gust: a cluster */
        int n = 1 + rng_int(&s->rng, 2 + (int)(s->wind * 4.0f));
        for (int i = 0; i < n && i < 6; i++) { s->gust_t[i] = rng_f(&s->rng) * 0.8f; s->gust_d[i] = s->mel[rng_int(&s->rng, 5)]; }
        s->ngust = n > 6 ? 6 : n;
    }
}

/* ------------------------------------------------------------------ the clock */
void seq_run(Seq *s, float beats)
{
    s->nout = 0;
    if (s->ratchet_left) {
        if ((s->ratchet_t -= beats) <= 0) { emit_raw(s, s->ratchet_n.deg, s->ratchet_n.vel, s->ratchet_n.dur, s->ratchet_n.pan); s->ratchet_left--; s->ratchet_t += s->ratchet_dt; }
    }
    /* WARP: the time bends. Swing between steps, and a phrase that speeds up and slows down over 8 beats */
    s->bend_ph += beats / 8.0f; if (s->bend_ph >= 1.0f) s->bend_ph -= 1.0f;
    float bend = 1.0f + s->warp * 0.45f * fm_sin_turns(s->bend_ph);
    float b = beats * bend;
    switch (s->type) {
    case S_LORENZ: run_lorenz(s, b); return;
    case S_BOUNCE: run_bounce(s, b); return;
    case S_ORBITS: run_orbits(s, b); return;
    case S_CHIMES: run_chimes(s, b); return;
    default: break;
    }
    if (s->type == S_PHASE) {                        /* the second player, a little faster: it drifts ahead */
        s->ph2 += b * 4.0f * s->rate2;
        if (s->ph2 >= 1.0f) {
            s->ph2 -= 1.0f;
            int i = s->idx++ % s->m;
            if (s->cells >> i & 1u) emit(s, s->mel[i], 0.55f, 0.9f, 0.6f);
            if (rng_chance(&s->rng, s->mutate * 0.02f)) s->rate2 = 1.0f + rng_range(&s->rng, 0.004f, 0.03f);
        }
    }
    s->t += b * 4.0f;                                /* steps are 16ths */
    float len = 1.0f + ((s->step & 1u) ? -1.0f : 1.0f) * s->warp * 0.3f;
    if (s->t < len) return;
    s->t -= len; s->step++;
    switch (s->type) {
    case S_EUCLID: step_euclid(s); break;
    case S_TURING: step_turing(s); break;
    case S_MARKOV: step_markov(s); break;
    case S_LIFE:   step_life(s); break;
    case S_PHASE:  step_phase(s); break;
    case S_DRUNK:
    default:       step_drunk(s); break;
    }
}

void seq_poke(Seq *s)
{
    Rng *r = &s->rng;
    switch (s->type) {
    case S_EUCLID: s->rot = (s->rot + 1 + rng_int(r, 2)) % s->n; break;
    case S_TURING: s->reg ^= 1u << rng_int(r, s->len); break;
    case S_MARKOV: s->idx = rng_int(r, 8); s->len = 0; break;
    case S_LORENZ: s->y += rng_bi(r) * 6.0f; break;
    case S_BOUNCE: if (s->g > 0) s->g = 0.01f; else s->v += 0.6f; break;
    case S_ORBITS: s->ph[rng_int(r, 4)] += 0.25f; break;
    case S_LIFE:   s->cells ^= (uint16_t)(1u << rng_int(r, 16)); break;
    case S_CHIMES: s->wind = clamp01(s->wind + 0.4f); break;
    case S_PHASE:  s->ph2 = 0; s->idx = s->pos; break;
    case S_DRUNK:
    default:       s->idx = rng_int(r, s->range); break;
    }
}

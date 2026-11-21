/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the effects (fx.h). */
#include "fx.h"

/* ------------------------------------------------------------------ reverb */
void reverb_init(Reverb *rv, float size)
{
    static const int base[RV_LINES] = { 1031, 1327, 1523, 1801, 2063, 2357, 2731, 3119 };
    static const int apb[4] = { 142, 107, 379, 277 };
    memset(rv, 0, sizeof *rv);
    for (int i = 0; i < RV_LINES; i++) {
        int n = (int)((float)base[i] * size); if (n >= RV_MAX) n = RV_MAX - 1; if (n < 64) n = 64;
        rv->len[i] = n;
    }
    for (int i = 0; i < 4; i++) { int n = (int)((float)apb[i] * size); if (n >= RV_AP) n = RV_AP - 1; rv->aplen[i] = n; }
    reverb_set(rv, 6.0f, 5000.0f);
}

void reverb_set(Reverb *rv, float rt60, float damp_hz)
{
    if (fm_fabsf(rt60 - rv->rt60) < 0.05f && fm_fabsf(damp_hz - rv->damp_hz) < 20.0f) return;
    rv->rt60 = rt60; rv->damp_hz = damp_hz;
    for (int i = 0; i < RV_LINES; i++) {
        rv->g[i] = fm_powf(10.0f, -3.0f * (float)rv->len[i] / (rt60 * SR));
        op_set(&rv->damp[i], damp_hz);
    }
}

static inline int16_t to16(float x) { x *= 16384.0f; if (x > 32767.0f) x = 32767.0f; if (x < -32768.0f) x = -32768.0f; return (int16_t)x; }

void reverb_run(Reverb *rv, const float *inL, const float *inR, float *outL, float *outR, int n)
{
    for (int i = 0; i < n; i++) {
        float in[2] = { inL[i], inR[i] };
        for (int a = 0; a < 4; a++) {                    /* diffusion */
            int p = rv->appos[a];
            for (int c = 0; c < 2; c++) {
                float d = rv->ap[a][c][p], y = -0.6f * in[c] + d;
                rv->ap[a][c][p] = in[c] + 0.6f * y;
                in[c] = y;
            }
            rv->appos[a] = p + 1 >= rv->aplen[a] ? 0 : p + 1;
        }
        float o[RV_LINES], h[RV_LINES];
        for (int k = 0; k < RV_LINES; k++) { o[k] = op_lp(&rv->damp[k], (float)rv->buf[k][rv->pos[k]] * (1.0f / 16384.0f)) * rv->g[k]; h[k] = o[k]; }
        for (int len = 1; len < RV_LINES; len <<= 1)     /* Hadamard */
            for (int j = 0; j < RV_LINES; j += len << 1)
                for (int k = j; k < j + len; k++) { float a = h[k], b = h[k + len]; h[k] = a + b; h[k + len] = a - b; }
        for (int k = 0; k < RV_LINES; k++) {
            float x = h[k] * 0.35355339f + ((k & 1) ? in[1] : in[0]) * ((k & 2) ? -0.5f : 0.5f);
            rv->buf[k][rv->pos[k]] = to16(x);
            rv->pos[k] = rv->pos[k] + 1 >= rv->len[k] ? 0 : rv->pos[k] + 1;
        }
        outL[i] = o[0] - o[2] + o[4] - o[6];
        outR[i] = o[1] - o[3] + o[5] - o[7];
    }
}

/* ------------------------------------------------------------------ echo: one line, two taps (the right 3/2 the left's) */
void echo_init(Echo *e)
{
    memset(e, 0, sizeof *e);
    op_set(&e->lp, 3500.0f); op_set(&e->hp, 180.0f);
    e->time_s = 0.4f; e->fb = 0.4f; e->tl = e->tr = 0.4f * SR;
}

static inline float tap(const Echo *e, float d)
{
    float p = (float)e->pos - d;
    while (p < 0) p += (float)ECHO_MAX;
    int i = (int)p; float f = p - (float)i; int j = i + 1 >= ECHO_MAX ? 0 : i + 1;
    return ((float)e->buf[i] + ((float)e->buf[j] - (float)e->buf[i]) * f) * (1.0f / 16384.0f);
}

void echo_run(Echo *e, const float *inL, const float *inR, float *outL, float *outR, int n)
{
    float dl = clampf(e->time_s * SR, 200.0f, (float)ECHO_MAX / 1.6f), dr = dl * 1.5f;
    e->wow_ph += (float)n / SR * 0.37f; if (e->wow_ph >= 1.0f) e->wow_ph -= 1.0f;
    float w = e->wow * 40.0f * fm_sin_turns(e->wow_ph);
    for (int i = 0; i < n; i++) {
        e->tl += (dl + w - e->tl) * 0.0005f;              /* the taps glide: a change of time pitches the echoes */
        e->tr += (dr + w - e->tr) * 0.0005f;
        float l = tap(e, e->tl), r = tap(e, e->tr);
        float x = 0.5f * (inL[i] + inR[i]) + e->fb * (0.6f * l + 0.4f * r);
        x = op_hp(&e->hp, op_lp(&e->lp, x));
        e->buf[e->pos] = to16(soft_clip(x));
        e->pos = e->pos + 1 >= ECHO_MAX ? 0 : e->pos + 1;
        outL[i] = l; outR[i] = r;
    }
}

/* ------------------------------------------------------------------ grains */
static const float RATIOS[] = { 1.0f, 1.0f, 2.0f, 0.5f, 1.5f, 1.0f };

void grains_init(Grains *g, uint32_t seed)
{
    memset(g, 0, sizeof *g);
    rng_seed(&g->rng, seed);
    g->ratios = RATIOS; g->nratios = 6;
    g->size = 0.15f;
}

void grains_run(Grains *g, const float *in, float *outL, float *outR, float *shim, int n)
{
    if (!g->frozen)
        for (int i = 0; i < n; i++) { g->buf[g->pos] = to16(in[i]); g->pos = g->pos + 1 >= GR_MAX ? 0 : g->pos + 1; }
    for (int i = 0; i < n; i++) { outL[i] = outR[i] = shim[i] = 0; }
    if (g->density <= 0.001f && g->shimmer <= 0.001f) return;
    g->next -= (float)n / SR;
    if (g->next <= 0) {                                  /* a new grain */
        g->next = (0.02f + 0.25f * (1.0f - g->density)) * rng_range(&g->rng, 0.5f, 1.5f);
        for (int k = 0; k < GR_N; k++) {
            if (g->g[k].on) continue;
            int sh = rng_chance(&g->rng, g->shimmer * 0.6f);
            if (!sh && g->density <= 0.001f) break;
            g->g[k].on = 1; g->g[k].shimmer = sh;
            g->g[k].rate = sh ? 2.0f : g->ratios[rng_int(&g->rng, g->nratios)];
            g->g[k].len = (0.04f + 0.4f * g->size) * SR * rng_range(&g->rng, 0.7f, 1.3f);
            float back = (float)GR_MAX * 0.9f * (0.1f + 0.9f * g->spread * rng_f(&g->rng));
            if (g->g[k].rate > 1.0f) back += g->g[k].len * (g->g[k].rate - 1.0f) + 64.0f;   /* (ahead of the writer: never) */
            g->g[k].at = (float)g->pos - back; while (g->g[k].at < 0) g->g[k].at += (float)GR_MAX;
            g->g[k].t = 0; g->g[k].amp = 0.5f; g->g[k].pan = rng_bi(&g->rng);
            break;
        }
    }
    for (int k = 0; k < GR_N; k++) {
        if (!g->g[k].on) continue;
        float pl = 0.5f - 0.5f * g->g[k].pan, pr = 1.0f - pl, inv = 1.0f / g->g[k].len;
        for (int i = 0; i < n; i++) {
            float u = g->g[k].t * inv;
            if (u >= 1.0f) { g->g[k].on = 0; break; }
            float win = 0.5f - 0.5f * sine_c(u + 0.25f);    /* Hann */
            float p = g->g[k].at; int a = (int)p; float f = p - (float)a; int b = a + 1 >= GR_MAX ? 0 : a + 1;
            float x = ((float)g->buf[a] + ((float)g->buf[b] - (float)g->buf[a]) * f) * (1.0f / 16384.0f) * win * g->g[k].amp;
            g->g[k].at += g->g[k].rate; if (g->g[k].at >= (float)GR_MAX) g->g[k].at -= (float)GR_MAX;
            g->g[k].t += 1.0f;
            if (g->g[k].shimmer) shim[i] += x;
            else { outL[i] += x * pl; outR[i] += x * pr; }
        }
    }
}

/* ------------------------------------------------------------------ tape */
void tape_init(Tape *t, uint32_t seed)
{
    memset(t, 0, sizeof *t);
    rng_seed(&t->rng, seed ^ 0x7A9Eu);
    op_set(&t->bump[0], 110.0f); op_set(&t->bump[1], 110.0f);
    t->drop = 1.0f;
}

void tape_run(Tape *t, float *L, float *R, int n)
{
    float a = t->amount, w = t->wow, hs = t->hiss, ag = t->age;
    if (a < 0.005f && w < 0.005f && hs < 0.005f && ag < 0.005f) {   /* off: the line kept fed, so turning it up is smooth */
        for (int i = 0; i < n; i++) { t->buf[0][t->pos] = to16(L[i] * 0.5f); t->buf[1][t->pos] = to16(R[i] * 0.5f); t->pos = (t->pos + 1) & (TAPE_LINE - 1); }
        return;
    }
    float dt = (float)n / SR;
    /* wow: a slow wobble that wanders, flutter: a fast small one; together the read point's distance, in samples */
    if ((t->wleft -= dt) <= 0) { t->wtarget = rng_bi(&t->rng); t->wleft = rng_range(&t->rng, 0.5f, 3.0f); }
    t->wdrift += (t->wtarget - t->wdrift) * clamp01(dt * 1.5f);
    t->wph += dt * (0.45f + 0.3f * t->wdrift); if (t->wph >= 1.0f) t->wph -= 1.0f;
    t->fph += dt * 6.5f; if (t->fph >= 1.0f) t->fph -= 1.0f;
    float d0 = 300.0f + w * (140.0f * fm_sin_turns(t->wph) + 60.0f * t->wdrift) + w * 6.0f * fm_sin_turns(t->fph);
    float hf = 16000.0f * fm_powf(0.22f, 0.5f * a + 0.7f * ag);    /* the highs it loses */
    op_set(&t->hf[0], hf); op_set(&t->hf[1], hf);
    float drive = 1.0f + 2.0f * a, norm = (1.0f + 0.2f * a) / drive;   /* (unity for small signals, the peaks rounded) */
    /* AGE: now and then the tape thins out for a moment */
    if (t->drop_t > 0) t->drop_t -= dt;
    else if (rng_chance(&t->rng, ag * ag * dt * 0.6f)) t->drop_t = rng_range(&t->rng, 0.05f, 0.4f);
    float dtarget = t->drop_t > 0 ? 0.35f + 0.4f * (1.0f - ag) : 1.0f;
    int hold = ag > 0.55f ? 1 + (int)((ag - 0.55f) * 6.0f) : 1;   /* (old tape's coarser grain) */
    for (int i = 0; i < n; i++) {
        float in[2] = { L[i], R[i] };
        for (int c = 0; c < 2; c++) t->buf[c][t->pos] = to16(in[c] * 0.5f);
        float rp = (float)t->pos - d0; while (rp < 0) rp += (float)TAPE_LINE;
        int i0 = (int)rp; float f = rp - (float)i0; int i1 = (i0 + 1) & (TAPE_LINE - 1);
        t->pos = (t->pos + 1) & (TAPE_LINE - 1);
        t->drop += (dtarget - t->drop) * 0.002f;
        float hissv = hs > 0.005f ? hs * 0.02f * rng_bi(&t->rng) : 0.0f;
        if (ag > 0.01f && rng_chance(&t->rng, ag * 0.0004f)) hissv += rng_bi(&t->rng) * 0.15f * ag;   /* a crackle */
        for (int c = 0; c < 2; c++) {
            float x = ((float)t->buf[c][i0] + ((float)t->buf[c][i1] - (float)t->buf[c][i0]) * f) * (2.0f / 16384.0f);
            x += a * 0.6f * op_lp(&t->bump[c], x);                     /* the head's bump */
            x = fm_tanhf(x * drive) * norm;
            x = op_lp(&t->hf[c], x) * t->drop + hissv;
            if (hold > 1) { if (t->hold_i == 0) t->held[c] = x; x = t->held[c]; }
            if (c) R[i] = x; else L[i] = x;
        }
        if (hold > 1 && ++t->hold_i >= hold) t->hold_i = 0;
    }
}

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
    static const int AP[SMEAR_AP] = { 347, 563, 797, 1009 };
    for (int a = 0; a < SMEAR_AP; a++) e->aplen[a] = AP[a];
}

static inline float tap(const Echo *e, float d)
{
    float p = (float)e->pos - d;
    while (p < 0) p += (float)ECHO_MAX;
    int i = (int)p; float f = p - (float)i; int j = i + 1 >= ECHO_MAX ? 0 : i + 1;
    return ((float)e->buf[i] + ((float)e->buf[j] - (float)e->buf[i]) * f) * (1.0f / 16384.0f);
}

/* the reverse heads: each reads backwards through the last window while the line moves on, so its delay grows from 0
 * to twice the window; two of them half a window apart, each faded in and out (Hann), sum to a steady reversed sound */
static inline float rev_head(const Echo *e, float ph, float win)
{
    float d = 2.0f * win * ph + 1.0f;
    return tap(e, d) * (0.5f - 0.5f * sine_c(ph + 0.25f));
}

void echo_run(Echo *e, const float *inL, const float *inR, float *outL, float *outR, int n)
{
    if (e->rev > 0.001f) {                               /* ECHO past 12: the echo turns into a reversed, smeared wash */
        float win = clampf(e->time_s * SR * 1.5f, 3000.0f, (float)ECHO_MAX * 0.45f), rv = e->rev;
        float dl = clampf(e->time_s * SR, 200.0f, (float)ECHO_MAX / 1.6f), dr = dl * 1.5f;
        for (int i = 0; i < n; i++) {
            e->tl += (dl - e->tl) * 0.0005f; e->tr += (dr - e->tr) * 0.0005f;
            float fl = tap(e, e->tl), fr = tap(e, e->tr);              /* the forward echo, fading out */
            e->rph += 1.0f / win; if (e->rph >= 1.0f) e->rph -= 1.0f;
            float p2 = e->rph + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
            float r = rev_head(e, e->rph, win) + rev_head(e, p2, win);
            float sm[2] = { r, r };                                    /* the smear: all-passes, the sides apart */
            for (int a = 0; a < SMEAR_AP; a++) {
                int p = e->appos[a];
                for (int c = 0; c < 2; c++) {
                    float g = 0.45f + 0.25f * rv, dd = e->ap[a][c][(p + c * 37) % e->aplen[a]];
                    float y = -g * sm[c] + dd;
                    e->ap[a][c][p] = sm[c] + g * y;
                    sm[c] = sm[c] * (1.0f - rv) + y * rv;
                }
                e->appos[a] = p + 1 >= e->aplen[a] ? 0 : p + 1;
            }
            float x = 0.5f * (inL[i] + inR[i]) + e->fb * ((1.0f - rv) * (0.6f * fl + 0.4f * fr) + rv * 0.7f * r);
            x = op_hp(&e->hp, op_lp(&e->lp, x));
            e->buf[e->pos] = to16(soft_clip(x));
            e->pos = e->pos + 1 >= ECHO_MAX ? 0 : e->pos + 1;
            outL[i] = fl * (1.0f - rv) + sm[0] * rv * 1.2f;
            outR[i] = fr * (1.0f - rv) + sm[1] * rv * 1.2f;
        }
        return;
    }
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

/* ------------------------------------------------------------------ dirt */
void dirt_init(Dirt *d, uint32_t seed)
{
    memset(d, 0, sizeof *d);
    rng_seed(&d->rng, seed ^ 0xD127u);
    op_set(&d->tone[0], 9000.0f); op_set(&d->tone[1], 9000.0f);
}

void dirt_run(Dirt *d, float a, float *L, float *R, int n)
{
    if (a < 0.005f) return;
    float f = clamp01(a * 3.0f), c = clamp01(a * 3.0f - 1.0f), z = clamp01(a * 3.0f - 2.0f);
    float gain = 1.0f + 5.0f * f, norm = 1.0f / (1.0f + 1.2f * f);
    float bits = 4096.0f * fm_exp2f(-9.0f * c);              /* 12 bits down to 3 */
    int hold = 1 + (int)(c * c * 14.0f);                     /* 44.1 kHz down to ~3 kHz */
    float cutoff = 9000.0f - 5500.0f * c;
    op_set(&d->tone[0], cutoff); op_set(&d->tone[1], cutoff);
    for (int i = 0; i < n; i++) {
        float x[2] = { L[i], R[i] };
        d->env += (fm_fabsf(x[0]) + fm_fabsf(x[1]) - d->env) * 0.002f;
        float nz = z > 0 ? z * (0.25f * d->env * rng_bi(&d->rng) + (rng_chance(&d->rng, z * 0.0015f) ? 0.2f * rng_bi(&d->rng) : 0.0f)) : 0.0f;
        int take = d->hold_i == 0;
        for (int ch = 0; ch < 2; ch++) {
            float y = x[ch];
            y = lerpf(y, fold(y * gain) * norm, f);          /* the fold */
            if (c > 0) {                                     /* the crush */
                if (take) d->held[ch] = fm_floorf(y * bits + 0.5f) / bits;
                y = lerpf(y, d->held[ch], fm_minf(c * 2.0f, 1.0f));
                y = op_lp(&d->tone[ch], y);
            }
            y += nz;                                         /* the noise */
            if (ch) R[i] = y; else L[i] = y;
        }
        if (++d->hold_i >= hold) d->hold_i = 0;
    }
}

/* ------------------------------------------------------------------ the shimmer's pitch shifter */
void shifter_run(Shifter *s, const float *in, float *up, float *down, int n)
{
    const float W = 2048.0f;                             /* each head's sweep: 46 ms */
    for (int i = 0; i < n; i++) {
        s->buf[s->pos] = to16(in[i]);
        float o[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {                    /* k 0: up (rate 2), 1: down (rate 0.5) */
            s->ph[k] += (k ? 0.5f : -1.0f) / W;          /* the delay shrinks (up) or grows (down) */
            if (s->ph[k] >= 1.0f) s->ph[k] -= 1.0f;
            if (s->ph[k] < 0.0f) s->ph[k] += 1.0f;
            for (int h = 0; h < 2; h++) {
                float ph = s->ph[k] + 0.5f * (float)h; if (ph >= 1.0f) ph -= 1.0f;
                float p = (float)s->pos - (8.0f + ph * W); while (p < 0) p += (float)SHIM_LINE;
                int a = (int)p; float f = p - (float)a; int b = (a + 1) & (SHIM_LINE - 1);
                float x = ((float)s->buf[a] + ((float)s->buf[b] - (float)s->buf[a]) * f) * (1.0f / 16384.0f);
                o[k] += x * (0.5f - 0.5f * sine_c(ph + 0.25f));
            }
        }
        up[i] = o[0]; down[i] = o[1];
        s->pos = (s->pos + 1) & (SHIM_LINE - 1);
    }
}

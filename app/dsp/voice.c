/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the sound engines (voice.h). */
#include "voice.h"

const char *const ENGINE_NAME[NUM_ENGINES] = {
    "DRIFT SAW", "FM", "STRING", "DX", "BLEEP", "GLASS", "BREATH", "VOICE", "FOLD", "SUB", "STRINGS", "E.PIANO"
};

float g_sine[SINE_SIZE + 1];
void dsp_init(void)
{
    for (int i = 0; i <= SINE_SIZE; i++) g_sine[i] = fm_sin_turns((float)i / SINE_SIZE);
}

static inline float k_of(float seconds)  /* a one-pole's rate for a time constant */
{
    if (seconds < 0.0005f) seconds = 0.0005f;
    return 1.0f - fm_expf(-1.0f / (seconds * SR));
}

static inline float noise(uint32_t *s)
{
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* the vowels a VOICE morphs through: two formants each */
static const float VOWEL[5][2] = { { 800, 1150 }, { 400, 1600 }, { 350, 2300 }, { 450, 800 }, { 325, 700 } };

void voice_start(Voice *v, const Sound *s, int note, float vel, float dur, float pan, float from_hz, uint32_t seed)
{
    float tone = clamp01(s->tone), wt = clamp01(s->weight), wd = clamp01(s->weird);
    v->on = 1; v->rel = 0; v->engine = s->engine; v->note = (int8_t)note;
    v->freq = mtof((float)note); v->vel = vel; v->pan = clampf(pan, -1.0f, 1.0f); v->age = 0;
    {
        float t = (v->pan + 1.0f) * 0.25f * PI_F;
        v->gl = fm_cosf(t); v->gr = fm_sinf(t);
    }
    v->noise = seed | 1u;
    v->freq_t = v->freq;                         /* the pitch: always at once (no portamento) */
    v->ff = v->freq;                             /* the filter's: glides (GLIDE), from the last note's */
    v->glide_k = s->glide > 0.001f ? clamp01(BLK_S / s->glide) : 1.0f;
    if (from_hz > 0 && s->glide > 0.001f) v->ff = from_hz;
    v->sweep = s->sweep; v->sw = 0; v->sw_k = clamp01(BLK_S / (0.15f + 1.2f * s->glide + 0.6f * s->sweep)); v->reso = s->reso;
    v->pan_amt = s->pan_move; v->pan_ph = (float)(seed >> 20 & 255u) / 256.0f;
    v->pan_dp = (0.04f + 0.3f * (float)(seed >> 4 & 255u) / 255.0f) * BLK_S;   /* once every 3 to 25 s */
    v->env = 0; v->hold = dur;
    float att = s->attack * (1.15f - 0.9f * wt);                 /* heavy: sharper */
    v->a_k = k_of(att * 0.33f + 0.0008f);
    v->r_k = k_of(s->release * 0.33f + 0.004f);
    v->sus = 1.0f; v->d_k = k_of(1.0f);
    for (int i = 0; i < 4; i++) v->ph[i] = (float)((seed >> (i * 8)) & 255u) * (1.0f / 256.0f);
    v->fb = 0; v->z1 = v->z2 = 0; v->lfo = 0; v->lp.z = 0;
    v->lfo_dp = (0.1f + 0.4f * wd) * BLK_S;
    v->drive = 1.0f + 5.0f * wt * wt;
    memset(&v->f, 0, sizeof v->f); memset(&v->f2, 0, sizeof v->f2);
    float f = v->freq;
    switch (v->engine) {
    case E_SAW:
        v->p[0] = 0.003f + 0.012f * wd;                          /* detune */
        v->p[1] = 1.5f + 10.0f * tone * tone;                     /* cutoff, times the pitch (+ 200 Hz) */
        v->p[2] = 0.7f + 2.5f * wt + 5.0f * s->reso;             /* resonance */
        v->p[3] = 0.5f + 0.5f * wt;                              /* filter's envelope */
        break;
    case E_FM: {
        static const float RAT[4] = { 1.0f, 2.0f, 3.0f, 0.5f };
        float r = RAT[seed % 4u];
        v->p[0] = r + (wd > 0.4f ? (wd - 0.4f) * 1.2f : 0.0f) * 0.4142f;   /* past halfway: off the harmonics */
        v->p[1] = (0.4f + 3.5f * wt * wt + 1.5f * tone);         /* index */
        v->p[2] = 0.15f + 0.6f * wd;                             /* feedback */
        v->sus = 0.55f + 0.3f * (1.0f - wt); v->d_k = k_of(0.8f + 2.0f * (1.0f - wt));
        break;
    }
    case E_PLUCK: {
        float len = SR / f;
        while (len >= (float)(KS_MAX - 2)) len *= 0.5f;          /* too low for the line: an octave up */
        v->ks_len = (int)len; v->ks_frac = len - (float)v->ks_len; v->ks_pos = 0; v->ks_lp = 0;
        v->ks_damp = 0.990f + 0.0095f * (0.3f + 0.7f * tone) - 0.004f * (1.0f - wt);
        float c = 0.2f + 0.8f * tone, z = 0;                     /* the pluck: noise, darker when soft */
        for (int i = 0; i < v->ks_len + 1; i++) { z += c * (noise(&v->noise) - z); v->ks[i] = (int16_t)(z * 12000.0f); }
        v->p[0] = wd;                                            /* weird: the string's pitch bends a little at the start */
        v->sus = 1.0f;
        break;
    }
    case E_EPIANO:
        v->p[0] = 0.6f + 1.6f * wt + 0.8f * tone;                /* the body's index (WEIGHT: the bark) */
        v->p[1] = 0.8f + 2.0f * tone;                            /* the tine's knock */
        v->p[2] = 14.0f + (wd > 0.4f ? (wd - 0.4f) * 9.0f : 0.0f);   /* the tine's ratio (DRIFT: off it, a clank) */
        v->p[3] = 1.0f - k_of(0.25f + 0.6f * (1.0f - wt));       /* the body's index fades, a sample */
        v->p[4] = 1.0f - k_of(0.012f);                           /* the knock, fast */
        v->p[5] = 0.002f + 0.006f * wd;                          /* the tape-ish wobble */
        v->z1 = 1.0f; v->z2 = 1.0f;
        v->sus = 0.0f; v->d_k = k_of(1.8f + 2.5f * (1.0f - wt));
        v->lfo_dp = (4.5f + 1.5f * wd) * BLK_S;
        break;
    case E_DX: {
        /* the ratios, by algorithm: kept on whole and half numbers (in tune), DRIFT pulls them off */
        static const float RAT[8][4] = {
            { 1, 1, 14, 1 }, { 1, 3.5f, 7, 2 }, { 1, 2, 3, 0.5f }, { 1, 1.5f, 4, 1 },
            { 0.5f, 1, 3, 7 }, { 1, 2, 5, 9 }, { 2, 1, 3.5f, 1 }, { 1, 4, 1, 0.5f } };
        static const float OFF[4] = { 0, 0.0071f, 0.4142f, 0.2361f };   /* (off the harmonics: a clang, a shimmer) */
        int set = (int)(seed % 8u);
        v->alg = (uint8_t)((seed >> 8) % 4u);
        for (int k = 0; k < 4; k++) v->r[k] = RAT[set][k] + (k ? OFF[k] * wd * 2.0f * (wd > 0.25f) : 0.0f);
        float ix = 0.5f + 2.6f * wt + 1.4f * tone;               /* the index (WEIGHT, TONE) */
        for (int k = 1; k < 4; k++) {                            /* each modulator its own fade: the spectrum moves */
            v->oe[k] = ix * (k == 3 ? 0.8f : 1.0f);
            v->ok[k] = 1.0f - k_of((0.08f + 1.8f * (float)((seed >> (12 + 3 * k)) & 7u) / 7.0f) * (0.4f + 0.8f * (1.0f - wt)));
        }
        v->p[5] = 0.1f + 0.5f * wd;                              /* the last operator's feedback */
        v->sus = 0.15f + 0.35f * (1.0f - tone); v->d_k = k_of(1.2f + 3.0f * (1.0f - wt));
        break;
    }
    case E_STRINGS:
        v->p[0] = 0.004f + 0.01f * wd;                           /* the ensemble's spread */
        v->p[1] = 1.5f + 5.0f * tone;                            /* dark: a string machine's filter (times the pitch) */
        v->p[2] = 0.6f + 0.6f * wt + 4.0f * s->reso;
        v->a_k = k_of((s->attack > 0.15f ? s->attack : 0.15f) * 0.33f);   /* (strings never snap in) */
        v->lfo_dp = 0.7f * BLK_S;
        break;
    case E_PULSE:
        v->p[0] = 0.5f - 0.35f * wt;                             /* width */
        v->p[1] = 2.0f + 12.0f * tone * tone;
        v->p[2] = 0.3f + 0.6f * wd;                              /* how much the width moves */
        break;
    case E_GLASS:
        v->p[0] = 1.0f; v->p[1] = 2.0f + 0.03f * wd; v->p[2] = 3.0f + 0.9f * wd * wd; v->p[3] = 4.0f + 1.7f * wd;
        v->p[4] = 0.25f + 0.6f * tone;                           /* the upper partials' level */
        break;
    case E_BREATH:
        v->p[0] = 6.0f + 40.0f * (1.0f - wt) + 60.0f * (1.0f - wd);   /* the resonance's Q: soft is purer */
        svf_set(&v->f, f, v->p[0]);
        v->p[1] = 0.25f + 0.5f * (1.0f - tone);                  /* a sine under it, for the pitch */
        break;
    case E_VOX:
        v->p[0] = (float)(seed % 5u);                            /* the vowel it starts on */
        v->p[1] = 0.2f + 0.8f * wd;                              /* how far it moves */
        v->p[2] = 6.0f + 6.0f * (1.0f - wt);
        break;
    case E_FOLD:
        v->p[0] = 1.0f + 5.0f * wt + 2.0f * tone;                /* the fold's gain */
        v->p[1] = 0.6f * wd;                                     /* asymmetry */
        v->p[2] = 3.0f + 10.0f * tone;
        break;
    case E_SUB:
    default:
        v->p[0] = 0.15f + 0.35f * tone;                          /* the octave */
        v->drive = 1.0f + 6.0f * wt;
        break;
    }
    v->ws = 0;                                   /* (the piece's DRIFT ring modulator: set by the engine each block) */
    v->ph[3] = 0;
    v->id = (uint8_t)(seed >> 24);
}

void voice_release(Voice *v) { if (v->on) v->hold = 0; }

void voice_legato(Voice *v, int note, float vel, float dur)
{
    if (!v->on) return;
    v->note = (int8_t)note; v->freq_t = mtof((float)note);
    v->freq = v->freq_t;                         /* the pitch at once; the filter slides after it */
    v->vel = vel; v->hold = dur;
    if (v->sweep > 0.001f) v->sw *= 0.5f;                     /* the filter dips and opens again */
}

static inline float saw_bl(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }

void voice_render(Voice *v, float *L, float *R, int n)
{
    if (!v->on) return;
    v->ff += (v->freq - v->ff) * v->glide_k;                     /* the filter glides after the pitch */
    v->sw += (1.0f - v->sw) * v->sw_k;                           /* the filter's sweep opening */
    const float f = v->freq, dt = f / SR;
    const float swf = v->sweep > 0.001f ? fm_exp2f(-4.5f * v->sweep * (1.0f - v->sw)) : 1.0f;
    float out[BLK];
    v->lfo += v->lfo_dp; if (v->lfo >= 1.0f) v->lfo -= 1.0f;
    float lfo = fm_sin_turns(v->lfo);
    /* the envelope, a sample at a time (one pole each stage) */
    float env = v->env;
    for (int i = 0; i < n; i++) {
        if (v->hold <= 0) env -= env * v->r_k;                      /* released */
        else if (v->rel == 0) { env += (1.02f - env) * v->a_k; if (env >= 1.0f) { env = 1.0f; v->rel = 2; } }   /* attack */
        else env += (v->sus - env) * v->d_k;                         /* decay to the sustain */
        out[i] = env;
    }
    v->hold -= (float)n / SR; v->age += (float)n / SR;
    if (v->hold <= 0 && env < 0.0002f) { v->on = 0; v->env = 0; return; }
    v->env = env;

    switch (v->engine) {
    case E_SAW: {
        float fc = (v->ff * v->p[1] + 200.0f) * swf * (1.0f - v->p[3] + v->p[3] * env) * (1.0f + 0.1f * lfo);
        svf_set(&v->f, fc, v->p[2]);
        float d0 = dt * (1.0f + v->p[0]), d1 = dt * (1.0f - v->p[0]);
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            float x = 0.5f * (saw_bl(v->ph[0], d0) + saw_bl(v->ph[1], d1));
            out[i] *= svf_lp(&v->f, x);
        }
        break;
    }
    case E_FM: {
        float r = v->p[0], ix = v->p[1] * (0.35f + 0.65f * env), fb = v->p[2];
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += dt * r; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            float m = sine_c(v->ph[1] + fb * v->fb);
            v->fb = m;
            out[i] *= sine_c(v->ph[0] + ix * m * 0.159f);
        }
        break;
    }
    case E_PLUCK: {
        int len = v->ks_len; float fr = v->ks_frac, damp = v->ks_damp;
        if (v->p[0] > 0.3f && v->age < 0.3f) fr += (v->p[0] - 0.3f) * (0.3f - v->age) * 3.0f;   /* weird: a bend */
        for (int i = 0; i < n; i++) {
            int p0 = v->ks_pos, p1 = p0 + 1 >= len + 1 ? 0 : p0 + 1;
            float a = v->ks[p0] * (1.0f / 12000.0f), b = v->ks[p1] * (1.0f / 12000.0f);
            float y = a + (b - a) * (fr > 1.0f ? 1.0f : fr);
            v->ks_lp += 0.5f * (y - v->ks_lp) + dn();        /* (never into the denormals) */
            float w = (v->ks_lp * 0.5f + y * 0.5f) * damp;
            v->ks[p0] = (int16_t)clampf(w * 12000.0f, -32000.0f, 32000.0f);
            v->ks_pos = p1;
            out[i] *= y * 1.6f;
        }
        (void)len;
        break;
    }
    case E_EPIANO: {
        float i1 = v->z1, i2 = v->z2, k1 = v->p[3], k2 = v->p[4], r2 = v->p[2];
        float wob = 1.0f + v->p[5] * lfo, d0 = dt * wob;
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d0; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            v->ph[2] += d0 * r2; if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
            i1 *= k1; i2 *= k2;
            float m = v->p[0] * (0.25f + 0.75f * i1) * sine_c(v->ph[1]) + v->p[1] * i2 * sine_c(v->ph[2]);
            out[i] *= sine_c(v->ph[0] + m * 0.159f);
        }
        v->z1 = i1 > 1e-5f ? i1 : 0.0f; v->z2 = i2 > 1e-5f ? i2 : 0.0f;   /* (never into the denormals) */
        break;
    }
    case E_DX: {
        float e1 = v->oe[1], e2 = v->oe[2], e3 = v->oe[3], k1 = v->ok[1], k2 = v->ok[2], k3 = v->ok[3], fb = v->p[5];
        float d0 = dt * v->r[0], d1 = dt * v->r[1], d2 = dt * v->r[2], d3 = dt * v->r[3];
        const float S = 0.159f;                                  /* (an index in radians, a phase in turns) */
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            v->ph[2] += d2; if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
            v->ph[3] += d3; if (v->ph[3] >= 1.0f) v->ph[3] -= 1.0f;
            e1 *= k1; e2 *= k2; e3 *= k3;
            float o3 = sine_c(v->ph[3] + fb * v->fb); v->fb = o3;
            float y;
            switch (v->alg) {
            default:
            case 0: {                                            /* a stack: 4 -> 3 -> 2 -> 1 */
                float o2 = sine_c(v->ph[2] + e3 * S * o3), o1 = sine_c(v->ph[1] + e2 * S * o2);
                y = sine_c(v->ph[0] + e1 * S * o1);
                break;
            }
            case 1: {                                            /* two pairs: 2 -> 1, 4 -> 3, both heard */
                float o1 = sine_c(v->ph[1]);
                y = 0.6f * sine_c(v->ph[0] + e1 * S * o1) + 0.4f * e2 * (1.0f / (1.0f + e2)) * sine_c(v->ph[2] + e3 * S * o3) * 1.6f;
                break;
            }
            case 2: {                                            /* three into one: 2, 3 and 4 -> 1 */
                float m = e1 * sine_c(v->ph[1]) + e2 * sine_c(v->ph[2]) + e3 * o3;
                y = sine_c(v->ph[0] + S * m);
                break;
            }
            case 3: {                                            /* a branch: 4 -> 3 -> 1, and 2 -> 1 */
                float o2 = sine_c(v->ph[2] + e3 * S * o3);
                y = sine_c(v->ph[0] + S * (e2 * o2 + e1 * sine_c(v->ph[1])));
                break;
            }
            }
            out[i] *= y;
        }
        v->oe[1] = e1 > 1e-5f ? e1 : 0.0f; v->oe[2] = e2 > 1e-5f ? e2 : 0.0f; v->oe[3] = e3 > 1e-5f ? e3 : 0.0f;
        break;
    }
    case E_STRINGS: {
        svf_set(&v->f, (v->ff * v->p[1] + 250.0f) * swf, v->p[2]);
        float sp = v->p[0];
        float d0 = dt * (1.0f + sp * fm_sin_turns(v->lfo)), d1 = dt * (1.0f + sp * fm_sin_turns(v->lfo + 0.33f)),
              d2 = dt * (1.0f + sp * fm_sin_turns(v->lfo + 0.67f));
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            v->ph[2] += d2; if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
            float x = 0.33f * (saw_bl(v->ph[0], d0) + saw_bl(v->ph[1], d1) + saw_bl(v->ph[2], d2));
            out[i] *= svf_lp(&v->f, x);
        }
        break;
    }
    case E_PULSE: {
        float w = clampf(v->p[0] + v->p[2] * 0.3f * lfo, 0.08f, 0.92f);
        svf_set(&v->f, (v->ff * v->p[1] + 300.0f) * swf * (0.4f + 0.6f * env), 0.9f + 4.0f * v->reso);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float p2 = v->ph[0] + w; if (p2 >= 1.0f) p2 -= 1.0f;
            float x = 0.5f * (saw_bl(v->ph[0], dt) - saw_bl(p2, dt));
            out[i] *= svf_lp(&v->f, x);
        }
        break;
    }
    case E_GLASS: {
        float u = v->p[4];
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float p = v->ph[0];
            out[i] *= 0.6f * sine_c(p) + u * (0.35f * sine_c(p * v->p[1] + v->ph[1]) + 0.22f * sine_c(p * v->p[2] + v->ph[2])
                                             + 0.14f * sine_c(p * v->p[3]));
        }
        break;
    }
    case E_BREATH: {
        float g = 0.35f * fm_sqrtf(v->p[0]), s = v->p[1];
        if (v->ff != v->freq || v->sweep > 0.001f) svf_set(&v->f, (f + v->ff) * 0.5f * (0.5f + 0.5f * swf), v->p[0]);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            out[i] *= svf_bp(&v->f, noise(&v->noise)) * g + s * sine_c(v->ph[0]);
        }
        break;
    }
    case E_VOX: {
        float pos = v->p[0] + v->p[1] * 2.0f * (0.5f + 0.5f * lfo);
        int a = (int)pos % 5, b = (a + 1) % 5; float t = pos - fm_floorf(pos);
        svf_set(&v->f, lerpf(VOWEL[a][0], VOWEL[b][0], t), v->p[2]);
        svf_set(&v->f2, lerpf(VOWEL[a][1], VOWEL[b][1], t), v->p[2]);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float x = saw_bl(v->ph[0], dt);
            out[i] *= 1.4f * (svf_bp(&v->f, x) + 0.6f * svf_bp(&v->f2, x)) + 0.15f * x;
        }
        break;
    }
    case E_FOLD: {
        float g = v->p[0] * (0.4f + 0.6f * env), off = v->p[1];
        op_set(&v->lp, (v->ff * v->p[2] + 300.0f) * swf);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            out[i] *= op_lp(&v->lp, fold(sine_c(v->ph[0]) * g + off));
        }
        break;
    }
    case E_SUB:
    default: {
        float o = v->p[0];
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            out[i] *= sine_c(v->ph[0]) + o * sine_c(2.0f * v->ph[0]);
        }
        break;
    }
    }

    if (v->pan_amt > 0.001f) {                   /* the note wanders across the stereo field */
        v->pan_ph += v->pan_dp; if (v->pan_ph >= 1.0f) v->pan_ph -= 1.0f;
        float pn = clampf(v->pan + v->pan_amt * fm_sin_turns(v->pan_ph), -1.0f, 1.0f), t = (pn + 1.0f) * 0.25f * PI_F;
        v->gl = fm_cosf(t); v->gr = fm_sinf(t);
    }
    /* the piece's WEIRD: a ring modulator off the harmonics, then the weight's drive */
    float d = v->drive, norm = (1.0f + d) / d, gain = v->vel * 0.32f;
    if (v->ws > 0.001f) {
        float rr = dt * 1.4142f, ws = v->ws;
        for (int i = 0; i < n; i++) {
            v->ph[3] += rr; if (v->ph[3] >= 1.0f) v->ph[3] -= 1.0f;
            out[i] *= 1.0f - ws + ws * sine_c(v->ph[3]) * 1.3f;
        }
    }
    for (int i = 0; i < n; i++) {
        float x = soft_clip(out[i] * d) * norm * gain;
        L[i] += x * v->gl; R[i] += x * v->gr;
    }
}

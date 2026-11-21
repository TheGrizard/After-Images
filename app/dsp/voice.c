/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the sound engines (voice.h). */
#include "voice.h"

const char *const ENGINE_NAME[NUM_ENGINES] = {
    "DRIFT SAW", "FM", "STRING", "BELL", "BLEEP", "GLASS", "BREATH", "VOICE", "FOLD", "SUB"
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

void voice_start(Voice *v, const Sound *s, int note, float vel, float dur, float pan, float bright, float wg, uint32_t seed)
{
    float tone = clamp01(s->tone * 0.75f + bright * 0.5f - 0.125f), wt = clamp01(s->weight), wd = clamp01(s->weird);
    v->on = 1; v->rel = 0; v->engine = s->engine; v->note = (int8_t)note;
    v->freq = mtof((float)note); v->vel = vel; v->pan = clampf(pan, -1.0f, 1.0f); v->age = 0;
    {
        float t = (v->pan + 1.0f) * 0.25f * PI_F;
        v->gl = fm_cosf(t); v->gr = fm_sinf(t);
    }
    v->noise = seed | 1u;
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
        v->p[1] = f * (1.5f + 10.0f * tone * tone) + 200.0f;     /* cutoff */
        v->p[2] = 0.7f + 2.5f * wt;                              /* resonance */
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
    case E_BELL: {
        float w = 0.25f + 0.75f * wd;
        static const float H[3] = { 1.0f, 2.0f, 3.0f }, B[3] = { 1.0f, 2.76f, 5.40f };
        for (int i = 0; i < 3; i++) v->p[i] = lerpf(H[i], B[i], w);
        v->p[3] = 2.5f + 4.0f * tone;                            /* decay, seconds (the low partial) */
        v->sus = 0.0f; v->d_k = k_of(v->p[3] * 0.4f);
        v->z1 = 1.0f; v->z2 = 1.0f; v->fb = 1.0f;                /* the upper partials' own levels */
        v->p[4] = 1.0f - k_of(v->p[3] * 0.18f);                  /* their faster decays, a sample */
        v->p[5] = 1.0f - k_of(v->p[3] * 0.07f);
        break;
    }
    case E_PULSE:
        v->p[0] = 0.5f - 0.35f * wt;                             /* width */
        v->p[1] = f * (2.0f + 12.0f * tone * tone) + 300.0f;
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
        v->p[2] = f * (3.0f + 10.0f * tone) + 300.0f;
        break;
    case E_SUB:
    default:
        v->p[0] = 0.15f + 0.35f * tone;                          /* the octave */
        v->drive = 1.0f + 6.0f * wt;
        break;
    }
    /* WEIRD, the whole piece's: past halfway a ring modulator on everything, off the harmonics */
    v->ws = wg > 0.5f ? (wg - 0.5f) * 1.4f : 0.0f;
    v->ph[3] = 0;
    v->id = (uint8_t)(seed >> 24);
}

void voice_release(Voice *v) { if (v->on) v->hold = 0; }

static inline float saw_bl(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }

void voice_render(Voice *v, float *L, float *R, int n)
{
    if (!v->on) return;
    const float f = v->freq, dt = f / SR;
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
        float fc = v->p[1] * (1.0f - v->p[3] + v->p[3] * env) * (1.0f + 0.1f * lfo);
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
            v->ks_lp += 0.5f * (y - v->ks_lp);
            float w = (v->ks_lp * 0.5f + y * 0.5f) * damp;
            v->ks[p0] = (int16_t)clampf(w * 12000.0f, -32000.0f, 32000.0f);
            v->ks_pos = p1;
            out[i] *= y * 1.6f;
        }
        (void)len;
        break;
    }
    case E_BELL: {
        float a2 = v->z1, a3 = v->z2, k2 = v->p[4], k3 = v->p[5];
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt * v->p[0]; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += dt * v->p[1]; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            v->ph[2] += dt * v->p[2]; if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
            a2 *= k2; a3 *= k3;
            out[i] *= 0.6f * sine_c(v->ph[0]) + 0.35f * a2 * sine_c(v->ph[1]) + 0.25f * a3 * sine_c(v->ph[2]);
        }
        v->z1 = a2; v->z2 = a3;
        break;
    }
    case E_PULSE: {
        float w = clampf(v->p[0] + v->p[2] * 0.3f * lfo, 0.08f, 0.92f);
        svf_set(&v->f, v->p[1] * (0.4f + 0.6f * env), 0.9f);
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
        op_set(&v->lp, v->p[2]);
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

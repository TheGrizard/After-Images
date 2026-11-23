/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the sound engines (voice.h). */
#include "voice.h"
#include "waves_tab.h"

const char *const ENGINE_NAME[NUM_ENGINES] = {
    "DRIFT SAW", "FM", "STRING", "DX", "BLEEP", "GLASS", "BREATH", "VOICE", "FOLD", "SUB", "STRINGS", "E.PIANO",
    "TRIANGLE", "SINE", "SQUARE", "CZ", "SYNC", "SHAPES", "VHS SYNTH", "REESE", "ORGAN", "BOWL", "MALLET", "WAVES", "CHOIR"
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

/* the note's settings from its layer's knobs: at its start (start 1, its state set up too), or again while it sounds
 * (start 0: only what the knobs decide, its phases, filters and envelope left as they are, so nothing clicks) */
static void voice_params(Voice *v, const Sound *s, uint32_t seed, int start)
{
    float tone = clamp01(s->tone), wt = clamp01(s->weight), wd = clamp01(s->weird);
    v->glide_k = s->glide > 0.001f ? clamp01(BLK_S / s->glide) : 1.0f;
    v->sweep = s->sweep; v->sw_k = clamp01(BLK_S / (0.2f + 1.2f * s->glide + 2.5f * s->sweep)); v->reso = s->reso;   /* (SWEEP: up to 3 s to open) */
    v->pan_amt = s->pan_move;
    if (start || v->hold > 0) v->r_k = k_of(s->release * 0.33f + 0.008f);   /* (a tail let go under load keeps its pace; RELEASE 0 still a
                                                                                     * short tail, ~50 ms, never clipped off) */
    v->sus = 1.0f; v->d_k = k_of(1.0f);
    v->lfo_dp = (0.1f + 0.4f * wd) * BLK_S;
    v->drive = 1.0f + 5.0f * wt * wt;
    v->ent = clamp01((wd - 0.5f) * 2.0f);        /* ENTROPY past halfway: the chaos oscillator comes in */
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
        v->ks_damp = 0.990f + 0.0095f * (0.3f + 0.7f * tone) - 0.004f * (1.0f - wt);
        if (start) {
            float len = SR / f;
            while (len >= (float)(KS_MAX - 2)) len *= 0.5f;      /* too low for the line: an octave up */
            v->ks_len = (int)len; v->ks_frac = len - (float)v->ks_len; v->ks_pos = v->ks_len + 1; v->ks_lp = 0; v->ks_e = 0.2f;
            memset(v->ks, 0, sizeof v->ks);
            float c = 0.2f + 0.8f * tone, z = 0;                 /* the pluck: noise, darker when soft */
            for (int i = 0; i < v->ks_len + 1; i++) { z += c * (noise(&v->noise) - z); v->ks[i] = (int16_t)(z * 12000.0f); }
        }
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
        if (start) { v->z1 = 1.0f; v->z2 = 1.0f; }
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
            if (start) v->oe[k] = ix * (k == 3 ? 0.8f : 1.0f);
            else if (v->ix > 0.01f) v->oe[k] *= ix / v->ix;      /* (sounding: its fades go on, rescaled) */
            v->ok[k] = 1.0f - k_of((0.08f + 1.8f * (float)((seed >> (12 + 3 * k)) & 7u) / 7.0f) * (0.4f + 0.8f * (1.0f - wt)));
        }
        v->ix = ix;
        v->p[5] = 0.1f + 0.5f * wd;                              /* the last operator's feedback */
        v->sus = 0.15f + 0.35f * (1.0f - tone); v->d_k = k_of(1.2f + 3.0f * (1.0f - wt));
        break;
    }
    case E_STRINGS:
        v->p[0] = 0.004f + 0.01f * wd;                           /* the ensemble's spread */
        v->p[1] = 1.5f + 5.0f * tone;                            /* dark: a string machine's filter (times the pitch) */
        v->p[2] = 0.6f + 0.6f * wt + 4.0f * s->reso;
        if (start) v->a_k = k_of((s->attack > 0.15f ? s->attack : 0.15f) * 0.33f);   /* (strings never snap in) */
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
    case E_TRI:
        v->p[0] = 1.0f + 3.0f * wt * wt;                         /* the fold */
        v->p[1] = 0.5f - 0.45f * wd;                             /* where its peak is: 0.5 a triangle, near 0 a saw */
        v->p[2] = 3.0f + 14.0f * tone;                           /* its filter, times the pitch */
        break;
    case E_SINE:
        v->p[0] = 0.25f * wt;                                    /* the 2nd */
        v->p[1] = 0.12f * wt * wt;                               /* the 3rd */
        v->p[2] = wd;                                            /* the chorus beside it */
        break;
    case E_SQUARE:
        v->p[1] = 1.2f + 8.0f * tone * tone;
        v->p[2] = 0.6f + 1.5f * wt + 4.0f * s->reso;
        v->p[3] = 0.3f + 0.5f * wt;                              /* the filter's envelope */
        break;
    case E_CZ:
        v->p[0] = 0.15f + 0.8f * tone;                           /* how far the phase bends (opens with the envelope) */
        v->p[1] = wd > 0.5f ? 1.0f : 0.0f;                       /* past halfway: the resonant kind */
        v->p[2] = 1.0f + 6.0f * tone + 4.0f * wt;                /* its resonance's ratio */
        v->p[3] = 0.4f + 0.6f * wt;                              /* the envelope's depth */
        break;
    case E_SYNC:
        v->p[0] = 1.5f + 5.0f * tone + 3.0f * wt;                /* the slave's ratio at the start */
        v->p[1] = 1.0f - k_of(0.25f + 1.5f * (1.0f - wt));       /* its sweep down, a block */
        v->p[2] = 4.0f + 10.0f * tone;
        if (start) v->z1 = 1.0f;
        break;
    case E_SHAPES:
        v->p[0] = wd * 5.0f;                                     /* which shape (DRIFT), between two */
        v->p[1] = 0.3f + 0.7f * wd;                              /* how far it wanders as it sounds */
        v->p[2] = 3.0f + 12.0f * tone;
        v->p[3] = 1.0f + 2.0f * wt;                              /* drive */
        break;
    case E_BOARDS:
        v->p[0] = 0.0069f + 0.006f * wd;                         /* the square a little sharp (+0.12 semitone, more with DRIFT) */
        v->p[1] = 0.8f + 4.0f * tone;                            /* dark: the filter's top, times the pitch */
        v->p[2] = 0.8f + 1.5f * wt + 4.0f * s->reso;
        v->p[3] = 1.0f - k_of(0.15f + 0.35f * (1.0f - wt));      /* the wah: the filter opens over ~150-500 ms */
        if (start) v->z1 = 0.0f;
        v->lfo_dp = 5.2f * BLK_S;                                /* the vibrato */
        break;
    case E_REESE:
        v->p[0] = 0.0045f + 0.016f * wd;                         /* the saws apart: ~8 cents, ~35 with ENTROPY */
        v->p[1] = 1.2f + 7.0f * tone * tone;                     /* the low-pass's top, times the pitch */
        v->p[2] = 0.7f + 1.2f * wt + 4.0f * s->reso;
        v->p[3] = 1.0f + 3.0f * wt;                              /* the drive into it */
        v->p[4] = (0.2f + 0.45f * wt) * clamp01((f - 45.0f) / 45.0f);   /* the sub, an octave down (none under ~23 Hz) */
        v->p[5] = 1.0f / fm_sqrtf(v->p[3]);
        v->lfo_dp = (0.07f + 0.25f * wd) * BLK_S;                /* the beating's slow sweep: every 3 to 14 s */
        if (start || v->hold > 0) v->r_k = k_of(fm_maxf(0.6f, s->release * 0.8f + 0.004f));   /* a long tail, closing as it goes */
        break;
    case E_ORGAN: {
        static const float REG[4][6] = {                         /* the drawbars: 16', 8', 4', 2 2/3', 2', 1' */
            { 1, 1, 1, 0, 0, 0 }, { 1, 1, 0.8f, 0.6f, 0.6f, 0.4f }, { 0, 1, 0.5f, 0, 0.3f, 0 }, { 0.6f, 1, 0.3f, 0.8f, 0.4f, 0.2f } };
        const float *rg = REG[seed % 4u]; float sum = 0;
        for (int k = 0; k < 6; k++) {
            v->p[k] = rg[k] * (k >= 3 ? 0.3f + 1.4f * tone : 1.0f);   /* TONE: the upper drawbars out */
            sum += v->p[k];
        }
        for (int k = 0; k < 6; k++) v->p[k] /= sum > 0.1f ? sum : 1.0f;
        v->lfo_dp = (0.75f + 5.8f * wd * wd) * BLK_S;           /* the rotary: chorale, to tremolo with ENTROPY */
        if (start) v->oe[0] = 0.5f * wt;                         /* percussion, on the third harmonic (WEIGHT) */
        v->ok[0] = 1.0f - k_of(0.25f);
        break;
    }
    case E_BOWL: {
        static const float RAT[4] = { 1.0f, 2.76f, 5.40f, 8.93f };
        for (int k = 0; k < 4; k++) v->r[k] = RAT[k] * (1.0f + wd * 0.025f * (float)k);   /* ENTROPY: further off */
        v->p[0] = 1.0f; v->p[1] = 0.55f * (0.4f + tone); v->p[2] = 0.4f * tone; v->p[3] = 0.25f * tone * tone;
        v->p[4] = 0.55f - 0.45f * wt;                            /* rubbed (soft) .. struck (heavy): what the partials ring down to */
        if (start) for (int k = 0; k < 4; k++) v->oe[k] = 1.0f;
        for (int k = 0; k < 4; k++) v->ok[k] = 1.0f - k_of((5.0f - 1.1f * (float)k) * (0.5f + 0.5f * (1.0f - wt)));
        v->lfo_dp = (0.45f + 1.2f * wd) * BLK_S;                 /* the beating */
        if (start) v->z2 = 0.0f;
        break;
    }
    case E_MALLET: {
        static const float RAT[3][3] = { { 1.0f, 3.93f, 9.24f }, { 1.0f, 4.0f, 10.0f }, { 1.0f, 5.93f, 13.0f } };
        int kind = (int)(seed % 3u);                             /* marimba, vibes, kalimba */
        for (int k = 0; k < 3; k++) v->r[k] = RAT[kind][k] * (1.0f + wd * 0.06f * (float)k);   /* ENTROPY: wood to metal */
        v->p[0] = 1.0f; v->p[1] = 0.25f + 0.4f * wt; v->p[2] = (0.1f + 0.3f * wt) * tone;
        float tm = (0.4f + 1.4f * (1.0f - 0.5f * tone)) * clampf(fm_sqrtf(300.0f / f), 0.5f, 2.5f) * (kind == 1 ? 2.5f : 1.0f);
        v->sus = 0.0f; v->d_k = k_of(tm);                         /* the bar ringing down: the envelope itself */
        v->ok[0] = 1.0f; v->ok[1] = 1.0f - k_of(tm * 0.3f); v->ok[2] = 1.0f - k_of(tm * 0.1f);
        v->p[3] = kind == 1 ? 1.0f : 0.0f;                       /* vibes: the tremolo */
        v->lfo_dp = 4.8f * BLK_S;
        if (start) {
            for (int k = 0; k < 3; k++) v->oe[k] = 1.0f;
            if (v->env < 0.002f) v->ph[0] = v->ph[1] = v->ph[2] = 0.0f;   /* (from silence: each mode from zero, no step) */
            v->z1 = 0.08f + 0.17f * wt;                          /* the knock: a soft thock, not a click */
            v->a_k = k_of(0.0006f + 0.002f * (1.0f - wt));       /* (always struck, whatever the layer's attack) */
        }
        v->p[4] = 1.0f - k_of(0.006f + 0.006f * wt);
        svf_set(&v->f, clampf(f * 2.0f, 200.0f, 3000.0f), 1.2f);
        break;
    }
    case E_WAVES:
        v->p[0] = tone * 5.0f;                                   /* where it starts: dark to bright */
        v->p[1] = 0.5f + 2.0f * wt;                              /* how far it scans up over the note */
        v->p[2] = 0.3f + 1.5f * wd;                              /* how far it wanders */
        v->p[3] = 1.0f - k_of(0.6f + 3.0f * (1.0f - wt));        /* the scan's pace, a block */
        v->lfo_dp = (0.08f + 0.5f * wd) * BLK_S;
        if (start) v->z1 = 0.0f;
        break;
    case E_CHOIR:
        v->p[0] = 0.003f + 0.007f * wd;                          /* the voices' spread */
        v->p[1] = tone;                                          /* the vowel: ooh, oh, aah */
        v->p[2] = 6.0f + 4.0f * (1.0f - wt);
        v->p[3] = 0.12f * (1.0f - wt) + 0.04f;                   /* breath */
        v->p[4] = (0.05f + 0.35f * wd) * BLK_S;                  /* the vowel's wandering */
        v->lfo_dp = (4.6f + 0.8f * (float)(seed & 255u) / 255.0f) * BLK_S;   /* vibrato */
        if (start) v->z1 = (float)(seed >> 8 & 255u) / 256.0f;
        break;
    case E_SUB:
    default:
        v->p[0] = 0.15f + 0.35f * tone;                          /* the octave */
        v->drive = 1.0f + 6.0f * wt;
        break;
    }
}

void voice_start(Voice *v, const Sound *s, int note, float vel, float dur, float pan, float from_hz, uint32_t seed)
{
    float wt = clamp01(s->weight);
    v->on = 1; v->rel = 0; v->engine = s->engine; v->note = (int8_t)note; v->seed = seed;
    v->freq = mtof((float)note); v->vel = vel; v->g_last = vel * 0.32f; v->pan = clampf(pan, -1.0f, 1.0f); v->age = 0;
    {
        float t = (v->pan + 1.0f) * 0.25f * PI_F;
        v->gl = fm_cosf(t); v->gr = fm_sinf(t); v->gl_last = v->gl; v->gr_last = v->gr;
    }
    v->noise = seed | 1u;
    v->freq_t = v->freq;                         /* the pitch: always at once (no portamento) */
    if (v->pm <= 0.0f) v->pm = 1.0f;
    v->ff = v->freq;                             /* the filter's: glides (GLIDE), from the last note's */
    if (from_hz > 0 && s->glide > 0.001f) v->ff = from_hz;
    v->sw = 0;
    v->pan_ph = (float)(seed >> 20 & 255u) / 256.0f;
    v->pan_dp = (0.04f + 0.3f * (float)(seed >> 4 & 255u) / 255.0f) * BLK_S;   /* once every 3 to 25 s */
    v->env = 0;                                  /* (a note taken while it sounded fades out in the engine's ghost) */
    v->hold = dur; v->kill = 0; v->dcl = 0;
    float att = s->attack * (1.15f - 0.9f * wt);                 /* heavy: sharper */
    v->a_k = k_of(att * 0.33f + 0.0008f);
    for (int i = 0; i < 4; i++) v->ph[i] = (float)((seed >> (i * 8)) & 255u) * (1.0f / 256.0f);
    v->fb = 0; v->lp.z = 0; v->sl[0].z = v->sl[1].z = 0;
    memset(&v->f, 0, sizeof v->f); memset(&v->f2, 0, sizeof v->f2);
    v->z1 = v->z2 = 0; v->lfo = 0; v->ix = 0;
    voice_params(v, s, seed, 1);
    v->ws = 0;                                   /* (the piece's DRIFT ring modulator: set by the engine each block) */
    v->ph[3] = 0;
    v->id = (uint8_t)(seed >> 24);
    v->cph = (float)(seed >> 12 & 255u) / 256.0f; v->cz = v->cz2 = 0; v->clfo = (float)(seed >> 4 & 255u) / 256.0f;
    v->cmode = (uint8_t)((seed >> 27) % 3u);
    v->bab = 0;
}

void voice_retune(Voice *v, const Sound *s)
{
    if (!v->on || v->engine != s->engine) return;
    voice_params(v, s, v->seed, 0);
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

float g_voice_floor = 0.001f;
#define DECLICK 88                       /* (2 ms) */
static const uint8_t HAS_FILTER[NUM_ENGINES] = {   /* (their own filter follows SWEEP and F.GLIDE) */
    [E_SAW] = 1, [E_PULSE] = 1, [E_BREATH] = 1, [E_FOLD] = 1, [E_SUB] = 1, [E_STRINGS] = 1, [E_TRI] = 1, [E_SQUARE] = 1,
    [E_CZ] = 1, [E_SYNC] = 1, [E_SHAPES] = 1, [E_BOARDS] = 1, [E_REESE] = 1, [E_CHOIR] = 1 };

/* a voice nobody can hear (its layer faded right out): its envelope goes on, so it ends when it would, no sound made */
void voice_skip(Voice *v, int n)
{
    if (!v->on) return;
    float env = v->env;
    for (int i = 0; i < n; i++) {
        if (v->hold <= 0) env -= env * v->r_k;
        else if (v->rel == 0) { env += (1.02f - env) * v->a_k; if (env >= 1.0f) { env = 1.0f; v->rel = 2; } }
        else env += (v->sus - env) * v->d_k;
    }
    v->hold -= (float)n / SR; v->age += (float)n / SR;
    if (v->hold <= 0 && env < g_voice_floor) { v->on = 0; env = 0; }
    v->env = env;
}

void voice_render(Voice *v, float *L, float *R, int n)
{
    if (!v->on) return;
    v->ff += (v->freq - v->ff) * v->glide_k;                     /* the filter glides after the pitch */
    v->sw += (1.0f - v->sw) * v->sw_k;                           /* the filter's sweep opening */
    const float f = v->freq * v->pm, dt = f / SR;              /* (WARBLE: the pitch wandering, all its layer's notes as one) */
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
    /* the opening: every note faded in over its first 2 ms whatever its attack, so ATTACK 0 is as quick as it sounds
     * but never ticks (a one-pole's attack is steepest at its very start) */
    if (v->dcl < DECLICK)
        for (int i = 0; i < n && v->dcl < DECLICK; i++) { float t = (float)(++v->dcl) * (1.0f / DECLICK); out[i] *= t * t * (3.0f - 2.0f * t); }
    v->hold -= (float)n / SR; v->age += (float)n / SR;
    int last = (v->hold <= 0 && env < g_voice_floor) || v->kill;   /* faded under the floor: this block its last, faded the rest of the way
                                                  * to nothing (cut, it stepped from its level to silence: a tiny click
                                                  * each, a crackle under a busy pattern) */
    if (last) for (int i = 0; i < n; i++) out[i] *= 1.0f - (float)(i + 1) / (float)n;
    v->env = env;
    float envs[BLK];                             /* (the envelope, kept for ENTROPY's chaos oscillator) */
    if (v->ent > 0.001f) for (int i = 0; i < n; i++) envs[i] = out[i];

    switch (v->engine) {
    case E_SAW: {
        float fc = (v->ff * v->p[1] + 200.0f) * swf * (1.0f - v->p[3] + v->p[3] * env) * (1.0f + 0.1f * lfo);
        svf_set_res(&v->f, fc, v->p[2]);
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
    case E_PLUCK: {                                              /* Karplus-Strong: a line the length of a period */
        float leff = (float)(v->ks_len + 1) - v->ks_frac, damp = v->ks_damp, e = v->ent;
        if (v->p[0] > 0.3f && v->age < 0.3f) leff -= (v->p[0] - 0.3f) * (0.3f - v->age) * 3.0f;   /* weird: a bend */
        if (e > 0.001f) {   /* ENTROPY: a slack string, sharp as it's struck and sagging flat as it rings, wobbling */
            float a = clamp01(v->ks_e * 6.0f);
            leff *= 1.0f - 0.08f * e * a + 0.045f * e * (1.0f - a) + 0.015f * e * lfo;
        }
        leff = clampf(leff, 2.0f, (float)(KS_MAX - 3));
        int li = (int)leff; float fr = leff - (float)li;
        for (int i = 0; i < n; i++) {
            int w = v->ks_pos, r0 = (w - li) & (KS_MAX - 1), r1 = (r0 - 1) & (KS_MAX - 1);
            float a = v->ks[r0] * (1.0f / 12000.0f), b = v->ks[r1] * (1.0f / 12000.0f);
            float y = a + (b - a) * fr;
            v->ks_lp += 0.5f * (y - v->ks_lp) + dn();        /* (never into the denormals) */
            float wv = (v->ks_lp * 0.5f + y * 0.5f) * damp;
            v->ks[w] = (int16_t)clampf(wv * 12000.0f, -32000.0f, 32000.0f);
            v->ks_pos = (w + 1) & (KS_MAX - 1);
            v->ks_e += (fm_fabsf(y) - v->ks_e) * 0.0005f;
            out[i] *= y * 1.6f;
        }
        break;
    }
    case E_EPIANO: {
        float i1 = v->z1, i2 = v->z2, k1 = v->p[3], k2 = v->p[4], r2 = v->p[2];
        float wob = 1.0f + v->p[5] * lfo + 0.05f * v->ent * i1, d0 = dt * wob;   /* (ENTROPY: the tines bent, sharp as struck) */
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
        svf_set_res(&v->f, (v->ff * v->p[1] + 250.0f) * swf, v->p[2]);
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
        svf_set_res(&v->f, (v->ff * v->p[1] + 300.0f) * swf * (0.4f + 0.6f * env), 0.9f + 4.0f * v->reso);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float p2 = v->ph[0] + w; if (p2 >= 1.0f) p2 -= 1.0f;
            float x = 0.5f * (saw_bl(v->ph[0], dt) - saw_bl(p2, dt));
            out[i] *= svf_lp(&v->f, x);
        }
        break;
    }
    case E_GLASS: {
        float u = v->p[4], dr = 1.0f + 0.045f * v->ent * lfo;   /* (ENTROPY: the partials drifting apart and back) */
        float q1 = v->p[1] * dr, q2 = v->p[2] * (2.0f - dr), q3 = v->p[3] * (1.0f + 0.03f * v->ent * (1.0f - lfo));
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float p = v->ph[0];
            out[i] *= 0.6f * sine_c(p) + u * (0.35f * sine_c(p * q1 + v->ph[1]) + 0.22f * sine_c(p * q2 + v->ph[2])
                                             + 0.14f * sine_c(p * q3));
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
        if (v->ent > 0.001f) {                                   /* (ENTROPY: babble, the vowels wandering, stuttering) */
            v->bab += (noise(&v->noise) * 2.5f - v->bab) * 0.08f * v->ent;
            if (v->bab < 0) v->bab = -v->bab;
        }
        float pos = v->p[0] + v->p[1] * 2.0f * (0.5f + 0.5f * lfo) + v->ent * v->bab;
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
    case E_TRI: {
        float pk = v->p[1], g = v->p[0];
        op_set(&v->lp, (v->ff * v->p[2] + 300.0f) * swf);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float p = v->ph[0], x = p < pk ? p / pk : (1.0f - p) / (1.0f - pk);   /* 0..1..0, its peak at pk */
            x = 2.0f * x - 1.0f;
            if (g > 1.05f) x = fold(x * g) * 0.9f;
            out[i] *= op_lp(&v->lp, x);
        }
        break;
    }
    case E_SINE: {
        float h2 = v->p[0], h3 = v->p[1], ch = v->p[2] * 0.5f, d1 = dt * (1.0f + 0.004f * v->p[2]);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            float p = v->ph[0];
            out[i] *= (sine_c(p) + h2 * sine_c(2.0f * p) + h3 * sine_c(3.0f * p) + ch * sine_c(v->ph[1])) * (1.0f / (1.0f + 0.5f * ch));
        }
        break;
    }
    case E_SQUARE: {
        svf_set_res(&v->f, (v->ff * v->p[1] + 200.0f) * swf * (1.0f - v->p[3] + v->p[3] * env), v->p[2]);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float p2 = v->ph[0] + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
            out[i] *= svf_lp(&v->f, 0.5f * (saw_bl(v->ph[0], dt) - saw_bl(p2, dt)));
        }
        break;
    }
    case E_CZ: {
        float d = 0.5f - 0.48f * clamp01(v->p[0] * (1.0f - v->p[3] + v->p[3] * env)) * swf;   /* 0.5: a sine; small: a saw */
        float rr = 1.0f + (v->p[2] - 1.0f) * (1.0f - v->p[3] + v->p[3] * env);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float p = v->ph[0], y;
            if (v->p[1] > 0.5f) y = (1.0f - p) * sine_c(p * rr + 0.25f);          /* the resonant: a cosine at rr, windowed */
            else { float q = p < d ? p * 0.5f / d : 0.5f + (p - d) * 0.5f / (1.0f - d); y = -sine_c(q + 0.25f); }   /* the bent saw */
            out[i] *= y;
        }
        break;
    }
    case E_SYNC: {
        float r = 1.0f + (v->p[0] - 1.0f) * v->z1;
        v->z1 *= v->p[1];
        svf_set_res(&v->f, (v->ff * v->p[2] + 400.0f) * swf, 0.7f + 3.0f * v->reso);
        float ds = dt * r;
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt;
            if (v->ph[0] >= 1.0f) { v->ph[0] -= 1.0f; v->ph[1] = v->ph[0] * r; }   /* the master wraps: the slave starts again */
            else { v->ph[1] += ds; if (v->ph[1] >= 1.0f) v->ph[1] -= fm_floorf(v->ph[1]); }
            out[i] *= svf_lp(&v->f, 2.0f * v->ph[1] - 1.0f);
        }
        break;
    }
    case E_SHAPES: {
        float sel = clampf(v->p[0] + v->p[1] * 1.5f * lfo, 0.0f, 4.999f), dr = v->p[3];
        int a = (int)sel; float t = sel - (float)a;
        op_set(&v->lp, (v->ff * v->p[2] + 300.0f) * swf);
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float p = v->ph[0], sn = sine_c(p), w[2];
            for (int k = 0; k < 2; k++) {
                switch (a + k) {
                case 0: w[k] = 2.0f * fm_fabsf(sn) - 1.27f; break;                      /* rectified: an octave up, hollow */
                case 1: w[k] = (sn > 0 ? 2.0f * sn : 0.0f) - 0.64f; break;               /* half-rectified */
                case 2: w[k] = clampf(sn * 2.5f, -1.0f, 1.0f); break;                    /* trapezoid */
                case 3: w[k] = sine_c(0.25f * sine_c(p) * 2.2f); break;                  /* sine of a sine */
                case 4: { float x = 2.0f * p - 1.0f; w[k] = 1.0f - 2.0f * x * x; break; }   /* parabola */
                default: { float q = p * p * (3.0f - 2.0f * p); w[k] = sine_c(q * 1.5f); break; }   /* bent */
                }
            }
            float x = w[0] + (w[1] - w[0]) * t;
            out[i] *= op_lp(&v->lp, soft_clip(x * dr) * (1.0f + 0.5f * (dr - 1.0f)) / dr * 1.4f);
        }
        break;
    }
    case E_BOARDS: {
        v->z1 = 1.0f - (1.0f - v->z1) * v->p[3];                 /* the filter's slow opening, 0 to 1 */
        float vib = 1.0f + 0.0025f * lfo;
        float fc = (v->ff * v->p[1] * (0.25f + 0.75f * v->z1) + 150.0f) * swf;
        svf_set_res(&v->f, fc, v->p[2]);
        float d0 = dt * vib, d1 = dt * vib * (1.0f + v->p[0]);
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            float p2 = v->ph[1] + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
            float sq = 0.5f * (saw_bl(v->ph[1], d1) - saw_bl(p2, d1));
            out[i] *= svf_lp(&v->f, 0.5f * saw_bl(v->ph[0], d0) + sq);   /* the saw 6 dB under the square */
        }
        break;
    }
    case E_REESE: {                                              /* the jungle and drum'n'bass bass: phase cancelling, moving */
        float det = v->p[0] * (1.0f + 0.7f * lfo), dr = v->p[3], g = v->p[5], sub = v->p[4];
        float fc = (v->ff * v->p[1] + 120.0f) * swf * (0.6f + 0.4f * env);
        svf_set_res(&v->f, fc, v->p[2]); svf_set(&v->f2, fc, 0.6f);
        float d0 = dt * (1.0f + det), d1 = dt * (1.0f - det), d2 = dt * 0.5f;
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            v->ph[2] += d2; if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
            float x = soft_clip(0.5f * (saw_bl(v->ph[0], d0) + saw_bl(v->ph[1], d1)) * dr) * g;
            out[i] *= svf_lp(&v->f2, svf_lp(&v->f, x)) + sub * sine_c(v->ph[2]);
        }
        break;
    }
    case E_ORGAN: {
        float am = 1.0f + 0.16f * lfo, d0 = dt * 0.5f * (1.0f + 0.002f * lfo);   /* the rotary: louder and sharp as it comes round */
        float e = v->oe[0], k = v->ok[0];
        float g0 = v->p[0] * am, g1 = v->p[1] * am, g2 = v->p[2] * am, g3 = v->p[3] * am, g4 = v->p[4] * am, g5 = v->p[5] * am;
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;  /* (at half the pitch: the 16' at one) */
            float p = v->ph[0];
            out[i] *= g0 * sine_c(p) + g1 * sine_c(2.0f * p) + g2 * sine_c(4.0f * p) + (g3 + e) * sine_c(6.0f * p)
                      + g4 * sine_c(8.0f * p) + g5 * sine_c(16.0f * p);
            e *= k;
        }
        v->oe[0] = e > 1e-5f ? e : 0.0f;
        float t = (clampf(v->pan + 0.35f * fm_sin_turns(v->lfo + 0.25f), -1.0f, 1.0f) + 1.0f) * 0.25f * PI_F;
        v->gl = fm_cosf(t); v->gr = fm_sinf(t);                  /* (and round the room) */
        break;
    }
    case E_BOWL: {
        float bt = v->lfo, a[4];
        for (int k = 0; k < 4; k++) {                            /* each partial: ringing down to its floor, beating */
            float fl = v->p[4] * (k ? 0.6f : 1.0f);
            v->oe[k] = fl + (v->oe[k] - fl) * fm_powf(v->ok[k], (float)n);
            a[k] = v->p[k] * v->oe[k] * (0.7f + 0.3f * fm_sin_turns(bt * (float)(k + 1) + 0.23f * (float)k));
        }
        float d0 = dt * v->r[0], d1 = dt * v->r[1], d2 = dt * v->r[2], d3 = dt * v->r[3];
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            v->ph[2] += d2; if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
            v->z2 += d3; if (v->z2 >= 1.0f) v->z2 -= 1.0f;
            out[i] *= 0.6f * (a[0] * sine_c(v->ph[0]) + a[1] * sine_c(v->ph[1]) + a[2] * sine_c(v->ph[2]) + a[3] * sine_c(v->z2));
        }
        break;
    }
    case E_MALLET: {
        float e0 = v->oe[0], e1 = v->oe[1], e2 = v->oe[2], k0 = v->ok[0], k1 = v->ok[1], k2 = v->ok[2], kn = v->z1, kk = v->p[4];
        float a1 = v->p[1], a2 = v->p[2], tr = 1.0f - 0.3f * v->p[3] * (0.5f + 0.5f * lfo);
        float d0 = dt * v->r[0], d1 = dt * v->r[1], d2 = dt * v->r[2];
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            v->ph[2] += d2; if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
            float x = e0 * sine_c(v->ph[0]) + a1 * e1 * sine_c(v->ph[1]) + a2 * e2 * sine_c(v->ph[2]);
            if (kn > 1e-4f) { x += kn * svf_bp(&v->f, noise(&v->noise)); kn *= kk; }
            out[i] *= x * tr;
            e0 *= k0; e1 *= k1; e2 *= k2;
        }
        v->oe[0] = e0 > 1e-5f ? e0 : 0.0f; v->oe[1] = e1 > 1e-5f ? e1 : 0.0f; v->oe[2] = e2 > 1e-5f ? e2 : 0.0f;
        v->z1 = kn > 1e-4f ? kn : 0.0f;
        break;
    }
    case E_WAVES: {
        v->z1 = 1.0f - (1.0f - v->z1) * v->p[3];                 /* the scan up over the note, 0 to 1 */
        float pos = clampf(v->p[0] + v->p[1] * v->z1 + v->p[2] * (0.5f + 0.5f * lfo), 0.0f, (float)(WAVES_W - 1) - 0.001f);
        int w = (int)pos; float fr = pos - (float)w;
        const int16_t *ta = WAVES_TAB[f > 800.0f][w], *tb = WAVES_TAB[f > 800.0f][w + 1];
        const float sc = 1.0f / 32000.0f;
        for (int i = 0; i < n; i++) {
            v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            float x = v->ph[0] * (float)WAVES_N; int j = (int)x; float t = x - (float)j; int j1 = (j + 1) & (WAVES_N - 1);
            j &= WAVES_N - 1;
            float ya = (float)ta[j] + t * (float)(ta[j1] - ta[j]), yb = (float)tb[j] + t * (float)(tb[j1] - tb[j]);
            out[i] *= (ya + fr * (yb - ya)) * sc;
        }
        break;
    }
    case E_CHOIR: {
        v->z1 += v->p[4]; if (v->z1 >= 1.0f) v->z1 -= 1.0f;
        float vw = clamp01(v->p[1] + 0.25f * fm_sin_turns(v->z1)) * 2.0f;   /* ooh (0) .. oh (1) .. aah (2) */
        static const float FM[3][2] = { { 325, 700 }, { 450, 800 }, { 800, 1150 } };
        int a = vw >= 1.0f ? 1 : 0; float t = vw - (float)a;
        svf_set(&v->f, lerpf(FM[a][0], FM[a + 1][0], t) * swf, v->p[2]);
        svf_set(&v->f2, lerpf(FM[a][1], FM[a + 1][1], t) * swf, v->p[2] + 2.0f);
        float vib = 1.0f + 0.003f * lfo, sp = v->p[0], br = v->p[3];
        float d0 = dt * vib, d1 = dt * vib * (1.0f + sp), d2 = dt * vib * (1.0f - sp * 1.3f);
        for (int i = 0; i < n; i++) {
            v->ph[0] += d0; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
            v->ph[1] += d1; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
            v->ph[2] += d2; if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
            float x = 0.33f * (saw_bl(v->ph[0], d0) + saw_bl(v->ph[1], d1) + saw_bl(v->ph[2], d2)) + br * noise(&v->noise);
            out[i] *= 1.2f * (svf_bp(&v->f, x) + 0.5f * svf_bp(&v->f2, x)) + 0.08f * x;
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

    /* ENTROPY past halfway: synthesis turning on itself. A chaos oscillator at the note's pitch (in key), blended in
     * with the engine: FM feedback (a growl, then the screech of a DX at feedback 7), PWM (a pulse whose width swings
     * faster and wider), or a sine phase-modulated by that pulse; each note one of the three */
    if (v->ent > 0.001f) {
        float e = v->ent, m = 0.75f * e, beta = 0.3f + 1.7f * e;
        v->clfo += (0.15f + 7.0f * e * e) * BLK_S * (float)n / BLK; if (v->clfo >= 1.0f) v->clfo -= 1.0f;
        float w = 0.5f + (0.1f + 0.38f * e) * fm_sin_turns(v->clfo), cd = dt * (v->cmode == 2 ? 2.0f : 1.0f);
        for (int i = 0; i < n; i++) {
            v->cph += cd; if (v->cph >= 1.0f) v->cph -= 1.0f;
            float y;
            if (v->cmode == 0) {                     /* FM feedback, the two last samples averaged (as the DX does) */
                y = sine_c(v->cph + beta * 0.5f * (v->cz + v->cz2)); v->cz2 = v->cz; v->cz = y;
            } else {                                 /* the pulse, band-limited at both edges */
                float t2 = v->cph - w; if (t2 < 0) t2 += 1.0f;
                float pl = (v->cph < w ? 1.0f : -1.0f) + blep(v->cph, cd) - blep(t2, cd);
                y = v->cmode == 1 ? pl * 0.7f : sine_c(v->cph * 0.5f + beta * 0.35f * pl);
            }
            out[i] = out[i] * (1.0f - m) + envs[i] * y * 0.8f * m;
        }
    }
    /* SWEEP on the engines without a filter of their own (FM, DX, E.PIANO, GLASS, SINE, STRING, VOICE): a gentle
     * 12 dB low-pass, there only while it opens */
    if (v->sweep > 0.001f && !HAS_FILTER[v->engine % NUM_ENGINES]) {
        float o = 1.0f - v->sweep * (1.0f - v->sw);
        if (o < 0.995f) {
            op_set(&v->sl[0], (v->ff * 1.5f + 200.0f) * fm_exp2f(6.5f * o)); v->sl[1].a = v->sl[0].a;
            for (int i = 0; i < n; i++) out[i] = op_lp(&v->sl[1], op_lp(&v->sl[0], out[i]));
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
    /* the gain and the pan ramped over the block from where they were, not stepped there: a legato's new velocity, and the
     * pan wandering (FLOW, CHAOS) or swirling (ORGAN), moved a block at a time, were a step in level every 0.7 ms, a
     * fine crackle under a busy pattern */
    float a0 = v->g_last * v->gl_last, b0 = v->g_last * v->gr_last, a1 = gain * v->gl, b1 = gain * v->gr;
    float da = (a1 - a0) / (float)n, db = (b1 - b0) / (float)n;
    for (int i = 0; i < n; i++) {
        a0 += da; b0 += db;
        float x = soft_clip(out[i] * d) * norm;
        L[i] += x * a0; R[i] += x * b0;
    }
    v->g_last = gain; v->gl_last = v->gl; v->gr_last = v->gr;
    if (last) { v->on = 0; v->env = 0; }
}

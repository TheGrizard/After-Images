/* SPDX-License-Identifier: MPL-2.0 */
/* After Images on the desktop: a piece to a WAV, without the screen.
 *   render SEED SECONDS OUT.wav [all]     (all: a new scale, key and tempo too, as REC held does)
 *   render default SECONDS OUT.wav        (the piece the FM-1 starts with) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../dsp/engine.h"

static Engine eng;
static Piece pc;

static void w32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24 & 255, f); }
static void w16(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }

static inline float soft(float x)
{
    float a = fm_fabsf(x);
    if (a < 0.7f) return x;
    float y = 0.7f + 0.3f * fm_tanhf((a - 0.7f) / 0.3f);
    return x < 0 ? -y : y;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "render SEED|default SECONDS OUT.wav [all]\n"); return 2; }
    dsp_init();
    piece_default(&pc);
    if (getenv("PERS")) piece_roll_as(&pc, (uint32_t)strtoul(argv[1], 0, 0), argc > 4 && !strcmp(argv[4], "all"), atoi(getenv("PERS")));   /* PERS=0..3 */
    else if (strcmp(argv[1], "default")) piece_roll(&pc, (uint32_t)strtoul(argv[1], 0, 0), argc > 4 && !strcmp(argv[4], "all"), argc > 5 ? atoi(argv[5]) : -1);
    if (getenv("DUMP"))
        for (int l = 0; l < NUM_LAYERS; l++)
            fprintf(stderr, "layer %d: tone %d weight %d weird %d density %d rate %d level %d att %d rel %d oct %d\n", l, pc.l[l].tone,
                    pc.l[l].weight, pc.l[l].weird, pc.l[l].density, pc.l[l].rate, pc.l[l].level, pc.l[l].attack, pc.l[l].release, pc.l[l].octave);
    if (getenv("ENGS")) {                        /* ENGS=A,B,C,D: each layer's engine */
        char buf[128]; strncpy(buf, getenv("ENGS"), sizeof buf - 1); buf[sizeof buf - 1] = 0;
        int l = 0; for (char *t = strtok(buf, ","); t && l < NUM_LAYERS; t = strtok(NULL, ","), l++)
            for (int e = 0; e < NUM_ENGINES; e++) if (!strcmp(ENGINE_NAME[e], t)) pc.l[l].engine = (uint8_t)e;
    }
    if (getenv("SEQS"))                          /* SEQS=NAME: every layer's sequencer */
        for (int l = 0; l < NUM_LAYERS; l++) for (int q = 0; q < NUM_SEQS; q++) if (!strcmp(SEQ_NAME[q], getenv("SEQS"))) pc.l[l].seq = (uint8_t)q;
    if (getenv("BPM")) pc.tempo = (uint8_t)(atoi(getenv("BPM")) - 40);
    if (getenv("ACT")) pc.motion = (uint8_t)atoi(getenv("ACT"));
    if (getenv("LINK")) pc.link = (uint8_t)atoi(getenv("LINK"));
    if (getenv("ATT")) for (int l = 0; l < NUM_LAYERS; l++) pc.l[l].attack = (uint8_t)atoi(getenv("ATT"));
    if (getenv("REL")) for (int l = 0; l < NUM_LAYERS; l++) pc.l[l].release = (uint8_t)atoi(getenv("REL"));
    if (getenv("TONE")) for (int l = 0; l < NUM_LAYERS; l++) pc.l[l].tone = (uint8_t)atoi(getenv("TONE"));
    if (getenv("WARP")) pc.warp = (uint8_t)atoi(getenv("WARP"));
    if (getenv("FLOW")) pc.drift = (uint8_t)atoi(getenv("FLOW"));
    if (getenv("CHAOS")) pc.chaos = (uint8_t)atoi(getenv("CHAOS"));
    if (getenv("ENG")) {                         /* ENG=NAME: that engine on layer LAYER (or SOLO's) */
        int l = getenv("LAYER") ? atoi(getenv("LAYER")) : getenv("SOLO") ? atoi(getenv("SOLO")) : 1;
        for (int e = 0; e < NUM_ENGINES; e++) if (!strcmp(ENGINE_NAME[e], getenv("ENG"))) pc.l[l].engine = (uint8_t)e;
    }
    if (getenv("GRAIN")) pc.grain = (uint8_t)atoi(getenv("GRAIN"));   /* GRAIN=0..100: the shared GRAIN */
    if (getenv("DIRT")) pc.dirt = (uint8_t)atoi(getenv("DIRT"));
    if (getenv("WEAR")) pc.wear = (uint8_t)atoi(getenv("WEAR"));
    { extern float g_band_lp; if (getenv("BANDLP")) g_band_lp = (float)atof(getenv("BANDLP")); }   /* BANDLP=x: the bands' low-pass times x */
    if (getenv("ENT")) for (int l = 0; l < NUM_LAYERS; l++) pc.l[l].weird = (uint8_t)atoi(getenv("ENT"));   /* ENT=0..100: ENTROPY */
    if (getenv("MUTE")) for (int l = 0; l < NUM_LAYERS; l++) if (atoi(getenv("MUTE")) & (1 << l)) pc.l[l].level = 0;   /* MUTE=bitmask */
    if (getenv("DRIFT")) pc.weird = (uint8_t)atoi(getenv("DRIFT"));   /* DRIFT=0..100 */
    if (getenv("SOLO")) {                        /* SOLO=0..3: that layer alone */
        int k = atoi(getenv("SOLO"));
        for (int l = 0; l < NUM_LAYERS; l++) if (l != k) pc.l[l].level = 0;
        pc.weird = 0;
    }
    engine_init(&eng, &pc);
    char name[40]; piece_name(pc.seed, name);
    fprintf(stderr, "%s: %s %s, %d BPM; seqs %s %s %s %s; engines %s %s %s %s\n", argv[1], name, SCALES[pc.scale].name,
            40 + pc.tempo, SEQ_NAME[pc.l[0].seq], SEQ_NAME[pc.l[1].seq], SEQ_NAME[pc.l[2].seq], SEQ_NAME[pc.l[3].seq],
            ENGINE_NAME[pc.l[0].engine], ENGINE_NAME[pc.l[1].engine], ENGINE_NAME[pc.l[2].engine], ENGINE_NAME[pc.l[3].engine]);
    int secs = atoi(argv[2]);
    long frames = (long)(secs * SR);
    FILE *f = fopen(argv[3], "wb");
    if (!f) return 1;
    fwrite("RIFF", 1, 4, f); w32(f, 36 + (uint32_t)frames * 4); fwrite("WAVEfmt ", 1, 8, f);
    w32(f, 16); w16(f, 1); w16(f, 2); w32(f, 44100); w32(f, 44100 * 4); w16(f, 4); w16(f, 16);
    fwrite("data", 1, 4, f); w32(f, (uint32_t)frames * 4);
    static float L[ENGINE_SUB * BLK], R[ENGINE_SUB * BLK];
    float peak = 0, ss = 0;
    clock_t t0 = clock();
    for (long done = 0; done < frames; done += ENGINE_SUB * BLK) {
        if (getenv("MOODSWEEP")) {               /* MOODSWEEP=hz: MOOD swept 0..100 and back, as a hand turning it fast */
            float ph = (float)done / SR * (float)atof(getenv("MOODSWEEP")); ph -= (float)(long)ph;
            pc.mood = (uint8_t)(100.0f * (ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f));
        }
        engine_block_begin(&eng, ENGINE_SUB);
        eng.gov += (1.0f - eng.gov) * 0.01f;   /* (the app's load guard easing back, as on the FM-1 with room to spare) */
        if (getenv("GOV1")) eng.gov = 1.0f;
        for (int b = 0; b < NBUS; b++) engine_block_layer(&eng, b);
        engine_block_mix(&eng, L, R);
        for (int i = 0; i < ENGINE_SUB * BLK && done + i < frames; i++) {
            float og = getenv("OUTG") ? (float)atof(getenv("OUTG")) : 1.0f, l = soft(L[i] * og), r = soft(R[i] * og);
            if (fm_fabsf(L[i]) > peak) peak = fm_fabsf(L[i]);
            ss += L[i] * L[i];
            w16(f, (uint32_t)(int16_t)(l * 32000.0f)); w16(f, (uint32_t)(int16_t)(r * 32000.0f));
        }
    }
    float cpu = (float)(clock() - t0) / CLOCKS_PER_SEC;
    fclose(f);
    fprintf(stderr, "peak %.2f rms %.3f; events %d %d %d %d; %.2f s to render %d s\n", (double)peak, (double)fm_sqrtf(ss / (float)frames),
            eng.events[0], eng.events[1], eng.events[2], eng.events[3], (double)cpu, secs);
    return 0;
}

/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: a generative synth for the M-VAVE FM-1. Four layers (GROUND, BODY, LIGHT, DUST), each a sequencer
 * driving a sound engine in its own band; REC makes a new piece. Part of fm1-base's unity build (device/unity.c,
 * host/plat_host.c): gfx.c and announce.c are in scope.
 *
 * Controls
 *   K1-K4              at HOME the layers' levels (shown while they turn); HOME held + K1-K4 the macros (MOOD,
 *                      ACTIVITY, EVOLVE, DRIFT); on the other pages, their four macros
 *   HOME EDIT SEQ ENV FX LFO GLO ARP   the pages: HOME, SOUND, SEQUENCE, MIX, EFFECTS, MOTION, SCALE, MIDI OUT;
 *                      pressed again ENV: VOICE; GLO: SCREEN; ARP: MIDI IN, then USB AUDIO
 *   HOME tapped at HOME   the processor's load, and whether saving works
 *   SELECT, SEL        the layer the SOUND, SEQUENCE and MIX pages (and the keys) play
 *   PRESET             the chosen layer's engine
 *   ALGORITHM          your saved pieces
 *   REC                a new piece (held: a new key, scale and tempo too); SEL held + REC: the chosen layer anew
 *   SAVE               keep the piece; PLAY start / stop; OCT- / OCT+ the pieces before and after
 *   white keys         the scale's notes, up from the key; black keys: the scale's chords, I to VII then I to IV */
#include "fastmath.h"
#include "dsp/engine.h"

static Engine eng PLAT_POOL;

/* ------------------------------------------------------------------ settings */
#define SET_MAGIC 0x41465433u            /* "AFT3" (AFT1: v0.1's, AFT2: v0.2's and v0.3's: moved on at load) */
#define SET_MAGIC1 0x41465431u
#define SET_MAGIC2 0x41465432u
#define NSAVED 16
typedef struct {
    uint32_t magic;
    uint8_t slot;                        /* the saved piece being played (255: none) */
    uint8_t nsaved, midi_on, midi_ch, midi_keys, mosh_mould, playing, sync;   /* sync: CLOCK, INT / IN / OUT; mosh_mould: MOSH's
                                                                         * MOULD (v0.2-0.12: CLOCK out, moved into sync) */
    Piece cur;
    Piece saved[NSAVED];
    /* v0.2 */
    uint8_t in_ch;                       /* MIDI in: notes on this channel play the chosen layer (0: any) */
    uint8_t layers_in;                   /* notes on the layers' channels (CHANNEL's four) play those layers */
    uint8_t clock_in;                    /* MOSH's SHAPES: 0 shards, 1 pebbles (v0.2-0.12: CLOCK in, moved into sync) */
    uint8_t cc_in;                       /* CCs move the macros */
    uint8_t usb_full;                    /* USB recording at full level, not at MASTER */
    uint8_t usb_play;                    /* the computer's audio through the FM-1, 0..100 */
    uint8_t visual;                      /* VISUAL: MATRIX (the pegs), RORSCHACH (the inkblot), MOSH (v0.2-0.7: STEMS) */
    uint8_t palette, ink_gaps, ink_mix, mosh_size;   /* the screen: the palette (dealt by REC); RORSCHACH's GAPS, MIX; MOSH's SIZE */
    uint8_t breed, fade, pegs_set;       /* PEGS: how fast mixes breed, how long things linger (0..100); pegs_set: 2 once all four are set */
    uint8_t peg_n, peg_size;             /* PEGS: COUNT (an index into PEG_NS), SIZE 0..100 */
} settings_t;
static settings_t set;
#define NPAL 9
#define NPEG_NS 6
static const uint8_t PEG_NS[NPEG_NS] = { 8, 10, 12, 15, 20, 24 };   /* COUNT: pegs across (each divides 240) */
#define PEG_N_DEF 0                      /* (8 across: big balls) */
static uint8_t set_dirty;
static int store_ok = 1;
static uint32_t autosave_at;
#define AUTOSAVE_MS 8000u
enum { SYNC_INT, SYNC_IN, SYNC_OUT, NSYNC };   /* CLOCK: our own tempo; following MIDI clock; sending it */
static const char *const SYNC_NAME[NSYNC] = { "INT", "IN", "OUT" };
#define CLK_IN (set.sync == SYNC_IN)
#define CLK_OUT (set.sync == SYNC_OUT)

static void settings_v2(void)             /* what v0.2 added, as it starts */
{
    set.in_ch = 0; set.layers_in = 1; set.clock_in = 0; set.cc_in = 1;
    set.usb_full = 0; set.usb_play = 100; set.visual = 0;
    set.palette = 0; set.ink_gaps = 50; set.ink_mix = 50; set.mosh_size = 50;
    set.breed = 100; set.fade = 100; set.pegs_set = 5; set.peg_n = PEG_N_DEF; set.peg_size = 100;   /* MATRIX: 8 big balls, breeding, lingering */
}

static void settings_defaults(void)
{
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC; set.slot = 255;
    set.midi_on = 1; set.midi_ch = 0; set.midi_keys = 4; set.mosh_mould = 50; set.sync = SYNC_INT;
    piece_default(&set.cur);
    settings_v2();
}

static int settings_ok(void)
{
    if (set.magic != SET_MAGIC || set.nsaved > NSAVED || (set.slot != 255 && set.slot >= set.nsaved)) return 0;
    if (set.midi_ch > 12 || set.midi_keys > 15 || set.midi_on > 1 || set.mosh_mould > 100 || set.sync >= NSYNC) return 0;
    if (set.in_ch > 16 || set.layers_in > 1 || set.clock_in > 1 || set.cc_in > 1 || set.usb_full > 1 || set.usb_play > 100
        || set.palette >= NPAL || set.breed > 100 || set.fade > 100 || set.peg_n >= NPEG_NS || set.peg_size > 100
        || (set.pegs_set >= 3 && (set.visual > 2 || set.ink_gaps > 100 || set.ink_mix > 100 || set.mosh_size > 100))) return 0;
    if (!piece_ok(&set.cur)) return 0;
    for (int i = 0; i < set.nsaved; i++) if (!piece_ok(&set.saved[i])) return 0;
    return 1;
}

static void settings_load(void)
{
    static uint8_t raw[sizeof(settings_t)];
    int n = plat_store_load(OBJ_SETTINGS, raw, sizeof raw);
    const uint32_t head = __builtin_offsetof(settings_t, cur), tail = sizeof set - __builtin_offsetof(settings_t, in_ch);
    uint32_t magic = n >= 4 ? *(const uint32_t *)raw : 0, ps = magic == SET_MAGIC1 ? PIECE_SIZE1 : PIECE_SIZE2;
    if ((magic == SET_MAGIC1 && n == (int)(head + (1 + NSAVED) * ps)) || (magic == SET_MAGIC2 && n == (int)(head + (1 + NSAVED) * ps + tail))) {
        /* an older version's: its pieces kept (each shorter then: what came later as it starts), its settings too */
        memset(&set, 0, sizeof set);
        memcpy(&set, raw, head);
        for (int i = 0; i <= NSAVED; i++) {
            Piece *pc = i ? &set.saved[i - 1] : &set.cur;
            memcpy(pc, raw + head + (uint32_t)i * ps, ps);
            if (magic == SET_MAGIC1) memset(&pc->dirt, 0, 4);   /* (v0.1's tape) */
            for (int l = 0; l < NUM_LAYERS; l++) pc->l[l].tone = (uint8_t)(50 + ((int)pc->l[l].tone - 50) * 3 / 10);   /* (TONE: now a DJ's filter, 50 open) */
            piece_sends_default(pc);
        }
        if (magic == SET_MAGIC1) settings_v2();
        else memcpy(&set.in_ch, raw + head + (1 + NSAVED) * ps, tail);
        set.magic = SET_MAGIC;
        if (settings_ok()) { n = (int)sizeof set; store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0; }
    } else memcpy(&set, raw, sizeof set);
    int was = set.pegs_set;                      /* (the store's version of these) */
    if (was < 3 || was > 5) { set.visual = 0; set.ink_gaps = 50; set.ink_mix = 50; }   /* (before RORSCHACH) */
    if (was < 4 || was > 5) { set.peg_n = PEG_N_DEF; set.peg_size = 100; set.breed = 100; set.fade = 100; }   /* (before v0.21: MATRIX's defaults) */
    if (was < 5 || was > 5) {                    /* (before MOSH: its bytes were the old CLOCK switches') */
        if (set.clock_in || set.mosh_mould) set.sync = set.clock_in ? SYNC_IN : SYNC_OUT;   /* (v0.2-0.12's two: one setting, IN first) */
        set.clock_in = 0; set.mosh_mould = 50; set.mosh_size = 50; set.pegs_set = 5;
    }
    if (n != (int)sizeof set || !settings_ok()) {
        settings_defaults();
        store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
    }
    {   /* power-on: a layer turned right down was a solo of the moment; it comes back at its usual level */
        static const uint8_t LEVEL_DEF[NUM_LAYERS] = { 70, 72, 62, 48 };
        for (int l = 0; l < NUM_LAYERS; l++) if (set.cur.l[l].level < 15) set.cur.l[l].level = LEVEL_DEF[l];
    }
    set.playing = 1;
}

/* ------------------------------------------------------------------ history: the pieces REC replaced (OCT- / OCT+) */
#define NHIST 12
static Piece hist[NHIST];
static int hist_n, hist_at;              /* how many; where we are (hist_n: the newest, being played) */

static void hist_push(void)
{
    if (hist_at < hist_n) hist_n = hist_at;  /* (a new piece after going back: the ones after are dropped) */
    if (hist_n == NHIST) { for (int i = 1; i < NHIST; i++) hist[i - 1] = hist[i]; hist_n--; }
    hist[hist_n++] = set.cur;
    hist_at = hist_n;
}

/* ------------------------------------------------------------------ sound */
#define OUT_GAIN 1.2f
static volatile int muted;
static float gain_now;
static volatile float out_lvl;            /* the output's level, slowly (the autosave waits for the tails to die away) */
static volatile int save_hold;            /* SAVE while playing: the render's gain down to nothing while it's set */
static int two_cores;
static uint8_t on2_mask = 0x3u;

static inline float soft(float x)
{
    float a = fm_fabsf(x);
    if (a < 0.7f) return x;
    float y = 0.7f + 0.3f * fm_tanhf((a - 0.7f) / 0.3f);
    return x < 0 ? -y : y;
}

static void midi_send(uint32_t pkt) { plat_midi_send(pkt, TO_USB); }

static void cc_set(uint8_t *v, uint32_t x) { *v = (uint8_t)(x * 100u / 127u); }

/* MIDI in. Notes: on the layers' four channels (MIDI OUT's CHANNEL) they play those layers' sounds, on IN CH (or any)
 * the chosen layer's; every note put into the scale. CC 20-23 HOME's macros, 24-27 EFFECTS', 28-31 MOTION's. Clock:
 * its tempo followed, start and stop obeyed (CLOCK IN) */
static volatile float ext_bpm; static volatile uint32_t ext_at, ext_last;
static uint32_t clk_n;                    /* the leader's clocks since its Start (24 a beat) */
static void midi_in(void)
{
    uint32_t pkt; int port;
    const Scale *sc = &SCALES[set.cur.scale % NUM_SCALES];
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu, ch = st & 15u, ty = st & 0xF0u;
        if (st == 0xF8u) {                               /* clock: 24 a beat, timed a beat at a time */
            static uint32_t count, beat_at;
            uint32_t now = plat_ms();
            if (now - ext_last > 500u) { count = 0; beat_at = now; }   /* (a pause: start counting again) */
            ext_last = now;
            if (++count >= 24u) {
                uint32_t ms = now - beat_at;
                count = 0; beat_at = now;
                if (CLK_IN && ms >= 200u && ms <= 3000u) {   /* (20 to 300 BPM) */
                    float b = 60000.0f / (float)ms;
                    ext_bpm = ext_bpm > 0 ? ext_bpm + (b - ext_bpm) * 0.5f : b;
                }
            }
            /* locked to the leader's beat, not only its tempo: at each of its beats (24 clocks from its Start) our beat
             * should be whole; how far off it is the engine takes up over the next moments (no notes skipped) */
            if (CLK_IN && set.playing && ++clk_n % 24u == 0) {
                /* within a bar: a new piece (REC) starts our count again, a missed Start leaves it anywhere; the bars stay
                 * the leader's either way (at most two beats to take up) */
                float err = (float)(clk_n / 24u) - eng.beat;
                err -= 4.0f * fm_floorf(err * 0.25f + 0.5f);
                eng.beat_adj = err;
            }
            if (CLK_IN) ext_at = now;
        } else if ((st == 0xFAu || st == 0xFBu) && CLK_IN) {
            eng.playing = set.playing = 1;
            if (st == 0xFAu) { eng.sync_req = 1; clk_n = 0; }   /* Start: the top of the bar, with the leader */
            else clk_n = (uint32_t)(eng.beat + 0.5f) * 24u;                  /* Continue: from where we are */
        }
        else if (st == 0xFCu && CLK_IN) { eng.playing = set.playing = 0; engine_all_off(&eng); }
        else if (ty == 0x90u || ty == 0x80u) {
            int note = deg_note(sc, set.cur.key, note_deg(sc, set.cur.key, (int)d1)), layer = -1;
            if (set.layers_in && ch >= set.midi_ch && ch < set.midi_ch + 4u) layer = (int)(ch - set.midi_ch);
            else if (set.in_ch && ch != set.in_ch - 1u) continue;
            engine_key(&eng, ty == 0x90u && d2 > 0, note, (int)(d2 ? d2 : 64), layer);
        } else if (ty == 0xB0u && set.cc_in) {
            uint8_t *g[12] = { &set.cur.mood, &set.cur.motion, &set.cur.evolve, &set.cur.weird, &set.cur.reverb, &set.cur.echo,
                               &set.cur.grain, &set.cur.dirt, &set.cur.link, &set.cur.warp, &set.cur.drift, &set.cur.chaos };
            if (d1 >= 20u && d1 < 32u) cc_set(g[d1 - 20u], d2);
        }
    }
    if (ext_bpm > 0 && plat_ms() - ext_at > 1000u) ext_bpm = 0;   /* the clock stopped: our own tempo again */
    eng.ext_bpm = CLK_IN ? ext_bpm : 0.0f;
}


static void layers_job(uint32_t mask) { for (int b = 0; b < NUM_LAYERS; b++) if (mask >> b & 1u) engine_block_layer(&eng, b); }

static uint8_t balance(void)             /* the split that leaves the busier core least to do (the keys stay here) */
{
    /* (the second core has the reverb as well as its layers) */
    uint8_t best = 0x3u; float best_t = 1e30f, total = eng.cost[KEYS];
    for (int l = 0; l < NUM_LAYERS; l++) total += eng.cost[l];
    if (total <= 0) return best;
    for (uint8_t m = 0; m < 15; m++) {
        float t2 = 0;
        for (int l = 0; l < NUM_LAYERS; l++) if (m >> l & 1u) t2 += eng.cost[l];
        float t1 = total - t2; t2 += eng.cost_rv;
        float t = t2 > t1 ? t2 : t1;
        if (t < best_t - 1.0f) { best_t = t; best = m; }
    }
    return best;
}

static void core2_job(uint32_t mask) { layers_job(mask); engine_mix_reverb(&eng); }   /* (the second core's: its layers, the reverb) */

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float L[ENGINE_SUB * BLK], R[ENGINE_SUB * BLK];
    float g = muted || save_hold ? 0.0f : (float)master_q12 / 4096.0f * OUT_GAIN;
    int nsub = (int)(n / BLK);
    if (nsub > ENGINE_SUB) nsub = ENGINE_SUB;
    midi_in();
    eng.midi_on = set.midi_on; eng.midi_ch = set.midi_ch; eng.midi_keys_ch = set.midi_keys; eng.midi_clock = CLK_OUT;
    {   /* CLOCK turned to OUT while playing: a Start now, from the top of the bar; turned from it: a Stop */
        static int was_out = -1; int out = CLK_OUT && set.midi_on;
        if (was_out >= 0 && out != was_out && set.playing) {
            if (out) eng.sync_req = 1;
            plat_midi_send(0x0Fu | (out ? 0xFAu : 0xFCu) << 8, TO_USB);
        }
        was_out = out;
    }
    engine_block_begin(&eng, nsub);
    /* the second core: some layers and the reverb (the last block's sends); this one the rest, the keys, then the mix */
    uint8_t mask = two_cores && plat_cpu2_on() ? on2_mask : 0;
    int sent = two_cores && plat_cpu2_on() && plat_cpu2_run(core2_job, mask);
    layers_job(sent ? (uint32_t)(~mask & 0xFu) : 0xFu);
    engine_block_layer(&eng, KEYS);
    if (sent && plat_cpu2_wait() != 0) core2_job(mask);
    if (!sent) engine_mix_reverb(&eng);
    engine_mix_sends(&eng);
    engine_mix_echo(&eng);
    engine_mix_finish(&eng, L, R);
    if (two_cores) on2_mask = balance();
    for (uint32_t i = 0; i < n && i < (uint32_t)(nsub * BLK); i++) {
        gain_now += (g - gain_now) * 0.002f;
        out_lvl += (fm_fabsf(L[i]) + fm_fabsf(R[i]) - out_lvl) * 0.0005f;
        lr[2 * i] = (int32_t)(soft(L[i] * gain_now) * 8388000.0f);
        lr[2 * i + 1] = (int32_t)(soft(R[i] * gain_now) * 8388000.0f);
    }
}

void app_panic(void) { muted = 1; gain_now = 0; }

void app_init(void)
{
    dsp_init();
    settings_load();
    engine_init(&eng, &set.cur);
    eng.clock = plat_ticks24;
    eng.midi = midi_send;
    eng.playing = set.playing;
    two_cores = plat_cpu2_set(1) == 0;
}

/* ------------------------------------------------------------------ text */
static void text_cat(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; }
static int strlen_(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void int_text(int32_t v, char *b)
{
    char t[12]; int n = 0; uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do t[n++] = (char)('0' + u % 10u); while ((u /= 10u) && n < 11);
    if (v < 0) *b++ = '-';
    while (n) *b++ = t[--n];
    *b = 0;
}
static const char *const NOTE_NAME[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/* ------------------------------------------------------------------ the pages */
enum { PG_HOME, PG_SOUND, PG_SEQ, PG_MIX, PG_VOICE, PG_FX, PG_FXG, PG_MOTION, PG_WARBLE, PG_SCALE, PG_SCREEN, PG_MIDI_OUT, PG_MIDI_IN, PG_USB,
       PG_VISUAL, NPAGES };   /* (VISUAL last, GLO's third: the pages before keep their numbers) */
static const char *const PAGE_NAME[NPAGES] = { "HOME", "SOUND", "SEQUENCE", "MIX", "VOICE", "SENDS", "EFFECTS", "MOTION", "WARBLE", "SCALE", "SCREEN",
                                                "MIDI OUT", "MIDI IN", "USB AUDIO", "VISUAL" };
/* each page's button: pressed again, the next page of the same button (ENV: MIX, VOICE; GLO: SCALE, SCREEN; ARP: MIDI OUT,
 * MIDI IN, USB) */
static const uint8_t PAGE_BTN[NPAGES] = { B_HOME, B_EDIT, B_SEQ, B_ENV, B_ENV, B_FX, B_FX, B_LFO, B_LFO, B_GLO, B_GLO, B_ARP, B_ARP, B_ARP, B_GLO };
enum { K_NONE, K_PCT, K_ENGINE, K_SEQ, K_RATE, K_OCT, K_KEY, K_SCALE, K_CHORD, K_TEMPO, K_ONOFF, K_CH, K_INCH, K_REC,
       K_PEGN, K_SYNC, K_VIS, K_SHAPE, K_DIRT, K_RVB, K_ECHO, K_GRAIN, K_TONE };
typedef struct { const char *name; uint8_t kind, where; uint16_t off; } Knob;   /* where: 0 the piece, 1 the layer, 2 the settings,
                                                                                  * 3 the piece's per-layer arrays (VOICE) */
#define PO(f) (uint16_t)__builtin_offsetof(Piece, f)
#define LO(f) (uint16_t)__builtin_offsetof(LayerP, f)
#define SO(f) (uint16_t)__builtin_offsetof(settings_t, f)
static const Knob PAGES[NPAGES][4];
static const Knob *knob_of(int pg, int k);
static const Knob PAGES[NPAGES][4] = {
    { { "MOOD", K_PCT, 0, PO(mood) }, { "ACTIVITY", K_PCT, 0, PO(motion) }, { "EVOLVE", K_PCT, 0, PO(evolve) }, { "DRIFT", K_PCT, 0, PO(weird) } },
    { { "ENGINE", K_ENGINE, 1, LO(engine) }, { "TONE", K_TONE, 1, LO(tone) }, { "WEIGHT", K_PCT, 1, LO(weight) }, { "ENTROPY", K_PCT, 1, LO(weird) } },
    { { "TYPE", K_SEQ, 1, LO(seq) }, { "DENSITY", K_PCT, 1, LO(density) }, { "RATE", K_RATE, 1, LO(rate) }, { "MUTATE", K_PCT, 1, LO(mutate) } },
    { { "LEVEL", K_PCT, 1, LO(level) }, { "ATTACK", K_PCT, 1, LO(attack) }, { "RELEASE", K_PCT, 1, LO(release) }, { "OCTAVE", K_OCT, 1, LO(octave) } },
    { { "F.GLIDE", K_PCT, 3, PO(glide) }, { "LEGATO", K_ONOFF, 3, PO(legato) }, { "SWEEP", K_PCT, 3, PO(sweep) }, { "RESO", K_PCT, 3, PO(reso) } },
    { { "REVERB", K_PCT, 3, PO(s_rv) }, { "ECHO", K_PCT, 3, PO(s_echo) }, { "GRAIN", K_PCT, 3, PO(s_grain) }, { "DIRT", K_PCT, 3, PO(s_dirt) } },
    { { "REVERB", K_RVB, 0, PO(reverb) }, { "ECHO", K_ECHO, 0, PO(echo) }, { "GRAIN", K_GRAIN, 0, PO(grain) }, { "DIRT", K_DIRT, 0, PO(dirt) } },
    { { "LINK", K_PCT, 0, PO(link) }, { "WARP", K_PCT, 0, PO(warp) }, { "FLOW", K_PCT, 0, PO(drift) }, { "CHAOS", K_PCT, 0, PO(chaos) } },
    { { "WARBLE", K_PCT, 0, PO(warble) }, { "FLUTTER", K_PCT, 0, PO(flutter) }, { "DETUNE", K_PCT, 0, PO(detune) }, { "CHORUS", K_PCT, 0, PO(chorus) } },
    { { "KEY", K_KEY, 0, PO(key) }, { "SCALE", K_SCALE, 0, PO(scale) }, { "CHORDS", K_CHORD, 0, PO(chord) }, { "TEMPO", K_TEMPO, 0, PO(tempo) } },
    { { "COUNT", K_PEGN, 2, SO(peg_n) }, { "SIZE", K_PCT, 2, SO(peg_size) }, { "BREED", K_PCT, 2, SO(breed) }, { "FADE", K_PCT, 2, SO(fade) } },
    { { "OUT", K_ONOFF, 2, SO(midi_on) }, { "CHANNEL", K_CH, 2, SO(midi_ch) }, { "KEYS CH", K_CH, 2, SO(midi_keys) }, { "CLOCK", K_SYNC, 2, SO(sync) } },
    { { "IN CH", K_INCH, 2, SO(in_ch) }, { "LAYERS", K_ONOFF, 2, SO(layers_in) }, { "CLOCK", K_SYNC, 2, SO(sync) }, { "CC", K_ONOFF, 2, SO(cc_in) } },
    { { "RECORD", K_REC, 2, SO(usb_full) }, { "COMPUTER", K_PCT, 2, SO(usb_play) }, { "", K_NONE, 2, 0 }, { "", K_NONE, 2, 0 } },
    { { "VISUAL", K_VIS, 2, SO(visual) }, { "", K_NONE, 2, 0 }, { "SHAPES", K_SHAPE, 2, SO(clock_in) }, { "", K_NONE, 2, 0 } },
};
static int page_has_layer(int pg) { return pg == PG_SOUND || pg == PG_SEQ || pg == PG_MIX || pg == PG_VOICE || pg == PG_FX; }

#define C_TXT RGB(232, 232, 232)
#define C_SUB RGB(150, 150, 160)
#define C_RULE RGB(40, 40, 48)
#define C_WELL RGB(30, 30, 36)
#define C_RED RGB(255, 90, 78)
/* each layer its colour, everywhere: on the view and on its pages; the palettes (SCREEN), GROUND BODY LIGHT DUST */
static const uint16_t LPAL[NPAL][NUM_LAYERS] = {
    /* each its own character, its four layers always four clearly different hues (never shades of one) */
    { RGB(255, 72, 64), RGB(70, 140, 255), RGB(70, 230, 110), RGB(240, 240, 240) },    /* classic */
    { RGB(225, 45, 75), RGB(150, 85, 255), RGB(255, 150, 50), RGB(255, 235, 165) },    /* sunset */
    { RGB(45, 95, 235), RGB(255, 110, 90), RGB(60, 230, 210), RGB(255, 225, 240) },    /* ocean */
    { RGB(150, 70, 255), RGB(255, 60, 170), RGB(60, 215, 255), RGB(205, 255, 90) },    /* neon */
    { RGB(225, 45, 30), RGB(40, 190, 180), RGB(255, 180, 45), RGB(255, 240, 205) },    /* ember */
    { RGB(95, 180, 50), RGB(185, 70, 175), RGB(245, 205, 60), RGB(215, 255, 235) },    /* forest */
    { RGB(90, 135, 215), RGB(215, 100, 55), RGB(150, 205, 115), RGB(245, 245, 245) },  /* slate */
    { RGB(30, 175, 95), RGB(235, 60, 125), RGB(95, 180, 255), RGB(250, 250, 165) },    /* jade */
    { RGB(215, 120, 20), RGB(100, 85, 230), RGB(205, 230, 60), RGB(255, 222, 205) },   /* amber */
};
static uint16_t LAYER_C[NUM_LAYERS + 1] = { RGB(255, 72, 64), RGB(70, 140, 255), RGB(70, 230, 110), RGB(240, 240, 240), RGB(255, 255, 255) };
static void palette_apply(void) { for (int l = 0; l < NUM_LAYERS; l++) LAYER_C[l] = LPAL[set.palette % NPAL][l]; }

static struct {
    uint32_t btn, keys;
    int page, sel;
    int last_knob; uint32_t knob_at;      /* the knob turned last: its name and value, big */
    char pop_t[28], pop_s[28], pop_3[40]; uint16_t pop_c; uint32_t pop_until;
    uint32_t list_until;                  /* the saved pieces' list */
    uint32_t rec_t; int rec_used, sel_used, home_used;
    uint32_t panel_until; int panel_kind;     /* HOME: the levels (0) or the macros (1), shown a moment after a turn */
    uint32_t last_view, last_panel, last_head;
    uint8_t key_notes[NKEYS][5], key_n[NKEYS];
    int last_note, last_chord;
    char head_key[64];
    float t;
    uint32_t uiq_tail;
    uint32_t splash_t0; int splash_on, touched;
} ui;

static void settings_apply(void);

static const Knob *knob_of(int pg, int k)
{
    static const Knob INK_KNOBS[4] = { { "SIZE", K_PCT, 2, SO(peg_size) }, { "GAPS", K_PCT, 2, SO(ink_gaps) },
                                       { "MIX", K_PCT, 2, SO(ink_mix) }, { "FADE", K_PCT, 2, SO(fade) } };
    static const Knob MOSH_KNOBS[4] = { { "SIZE", K_PCT, 2, SO(mosh_size) }, { "BREED", K_PCT, 2, SO(breed) },
                                        { "MOULD", K_PCT, 2, SO(mosh_mould) }, { "FADE", K_PCT, 2, SO(fade) } };
    if (pg == PG_SCREEN && set.visual == 2) return &MOSH_KNOBS[k];
    if (pg == PG_SCREEN && set.visual == 1) return &INK_KNOBS[k];   /* (SCREEN: the chosen visual's own four) */
    if (pg == PG_VISUAL && k == 2 && set.visual != 2) return &PAGES[PG_VISUAL][1];   /* (SHAPES: MOSH's alone) */
    return &PAGES[pg][k];
}

static uint8_t *knob_ptr(int pg, int k)
{
    const Knob *kn = knob_of(pg, k);
    if (kn->where == 1) return (uint8_t *)&set.cur.l[ui.sel] + kn->off;
    if (kn->where == 2) return (uint8_t *)&set + kn->off;
    if (kn->where == 3) return (uint8_t *)&set.cur + kn->off + ui.sel;
    return (uint8_t *)&set.cur + kn->off;
}

static void knob_text(int pg, int k, char *s)
{
    const Knob *kn = knob_of(pg, k); uint8_t v = *knob_ptr(pg, k);
    s[0] = 0;
    switch (kn->kind) {
    case K_NONE: break;
    case K_INCH: if (v) int_text(v, s); else text_cat(s, "ANY"); break;
    case K_REC: text_cat(s, v ? "FULL" : "MASTER"); break;
    case K_RVB: if (v <= 50) int_text(v * 2, s); else { text_cat(s, "SHIM "); int_text((v - 50) * 2, s + strlen_(s)); } break;
    case K_ECHO: if (v <= 50) int_text(v * 2, s); else { text_cat(s, "REV "); int_text((v - 50) * 2, s + strlen_(s)); } break;
    case K_DIRT: text_cat(s, v < 34 ? "WARM " : v < 67 ? "FUZZ " : "BREAK "); int_text(v < 34 ? v * 3 : v < 67 ? (v - 33) * 3 : (v - 66) * 3, s + strlen_(s)); break;
    case K_GRAIN: if (v < 92) int_text(v, s); else text_cat(s, "FREEZE"); break;
    case K_TONE: if (v >= 48 && v <= 52) text_cat(s, "OPEN"); else { text_cat(s, v < 50 ? "LP " : "HP "); int_text(v < 50 ? (50 - v) * 2 : (v - 50) * 2, s + strlen_(s)); } break;
    case K_PEGN: int_text(PEG_NS[v % NPEG_NS], s); break;
    case K_VIS: text_cat(s, v == 2 ? "MOSH" : v ? "RORSCHACH" : "MATRIX"); break;
    case K_SHAPE: text_cat(s, v ? "LINES" : "BLOCKS"); break;
    case K_SYNC: text_cat(s, SYNC_NAME[v % NSYNC]); break;
    case K_ENGINE: text_cat(s, ENGINE_NAME[v % NUM_ENGINES]); break;
    case K_SEQ: text_cat(s, SEQ_NAME[v % NUM_SEQS]); break;
    case K_RATE: text_cat(s, RATE_NAME[v % NUM_RATES]); break;
    case K_OCT: { int o = (int8_t)v; if (o > 0) text_cat(s, "+"); int_text(o, s + (o > 0)); break; }
    case K_KEY: text_cat(s, NOTE_NAME[v % 12]); break;
    case K_SCALE: text_cat(s, SCALES[v % NUM_SCALES].name); break;
    case K_CHORD: text_cat(s, CHORD_NAME[v % NUM_CHORDS]); break;
    case K_TEMPO: int_text(40 + v, s); break;
    case K_ONOFF: text_cat(s, v ? "ON" : "OFF"); break;
    case K_CH:
        if (k == 1) { int_text(v + 1, s); text_cat(s, "-"); char n[4]; int_text(v + 4, n); text_cat(s, n); }   /* the four layers' */
        else int_text(v + 1, s);
        break;
    default: int_text(v, s); break;
    }
}
static float knob_frac(int pg, int k)    /* the bar under the value */
{
    const Knob *kn = knob_of(pg, k); uint8_t v = *knob_ptr(pg, k);
    switch (kn->kind) {
    case K_NONE: return 0.0f;
    case K_INCH: return (float)v / 16.0f;
    case K_REC: return v ? 1.0f : 0.0f;
    case K_PEGN: return (float)v / (NPEG_NS - 1);
    case K_VIS: return (float)v * 0.5f;
    case K_SHAPE: return (float)v;
    case K_SYNC: return (float)v / (NSYNC - 1);
    case K_ENGINE: return (float)v / (NUM_ENGINES - 1);
    case K_SEQ: return (float)v / (NUM_SEQS - 1);
    case K_RATE: return (float)v / (NUM_RATES - 1);
    case K_OCT: return ((float)(int8_t)v + 2.0f) / 4.0f;
    case K_KEY: return (float)v / 11.0f;
    case K_SCALE: return (float)v / (NUM_SCALES - 1);
    case K_CHORD: return (float)v / (NUM_CHORDS - 1);
    case K_TEMPO: return (float)v / 140.0f;
    case K_ONOFF: return v ? 1.0f : 0.0f;
    case K_CH: return (float)v / (k == 1 ? 12.0f : 15.0f);
    default: return (float)v / 100.0f;
    }
}

/* ------------------------------------------------------------------ messages */
static void say_text(const char *a, const char *b)
{
    if (!ann_on()) return;
    char t[72] = "";
    text_cat(t, a); text_cat(t, " "); text_cat(t, b);
    ann_say(t, 0, ANN_NOW);
}

void app_say(const char *a, const char *b)
{
    int i;
    for (i = 0; i < 27 && a && a[i]; i++) ui.pop_t[i] = a[i];
    ui.pop_t[i] = 0;
    for (i = 0; i < 27 && b && b[i]; i++) ui.pop_s[i] = b[i];
    ui.pop_s[i] = 0;
    ui.pop_3[0] = 0; ui.pop_c = C_TXT;
    ui.pop_until = plat_ms() + 1500u;
    ui.list_until = 0;
    say_text(ui.pop_t, ui.pop_s);
}
static void pop(const char *a, const char *b, uint16_t c) { app_say(a, b); ui.pop_c = c; }

/* ------------------------------------------------------------------ actions */
static void changed(void) { set_dirty = 1; autosave_at = plat_ms() + AUTOSAVE_MS; }

static void piece_label(char *t)          /* its name, and a star once it is changed from what was saved */
{
    piece_name(set.cur.seed, t);
    if (set.slot != 255 && memcmp(&set.cur, &set.saved[set.slot], sizeof(Piece))) text_cat(t, "*");
}

/* REC's palettes: a shuffled deck, so none comes twice until all have */
static uint8_t deck[NPAL]; static int deck_n;
static void palette_next(void)
{
    if (deck_n == 0) {
        for (int i = 0; i < NPAL; i++) deck[i] = (uint8_t)i;
        for (int i = NPAL - 1; i > 0; i--) { int j = (int)((plat_ticks24() ^ (uint32_t)i * 2654435761u) % (uint32_t)(i + 1)); uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
        deck_n = NPAL;
        if (deck[deck_n - 1] == set.palette) { uint8_t t = deck[0]; deck[0] = deck[deck_n - 1]; deck[deck_n - 1] = t; }   /* (not the one showing) */
    }
    set.palette = deck[--deck_n];
    palette_apply();
}

/* REC's featured effects: the same kind of deck */
static uint8_t fxdeck[NUM_FX]; static int fxdeck_n, fx_last = -1;
static int fx_next(void)
{
    if (fxdeck_n == 0) {
        for (int i = 0; i < NUM_FX; i++) fxdeck[i] = (uint8_t)i;
        for (int i = NUM_FX - 1; i > 0; i--) { int j = (int)((plat_ticks24() * 2246822519u ^ (uint32_t)i * 2654435761u) % (uint32_t)(i + 1)); uint8_t t = fxdeck[i]; fxdeck[i] = fxdeck[j]; fxdeck[j] = t; }
        fxdeck_n = NUM_FX;
        if (fxdeck[fxdeck_n - 1] == fx_last) { uint8_t t = fxdeck[0]; fxdeck[0] = fxdeck[fxdeck_n - 1]; fxdeck[fxdeck_n - 1] = t; }
    }
    return fx_last = fxdeck[--fxdeck_n];
}

static void new_piece(int keep_key)      /* REC: a new piece, a new key and scale too; held, in the same key */
{
    int all = 0, key = set.cur.key, scale = set.cur.scale;   /* (the HOME macros stay yours either way) */
    char nm[32];
    palette_next();
    int fx = fx_next();
    hist_push();
    uint32_t seed = (set.cur.seed * 1664525u + 1013904223u) ^ plat_ms() * 2654435761u ^ plat_ticks24();
    piece_roll(&set.cur, seed, all, fx);
    if (keep_key) { set.cur.key = (uint8_t)key; set.cur.scale = (uint8_t)scale; }
    else piece_next_key(&set.cur, key, scale, seed);
    set.slot = 255;
    changed();
    piece_name(seed, nm);
    {   char t[32] = "", n[8];                   /* NEW PIECE + SHIMMER / its name / D DORIAN  92 BPM */
        text_cat(t, keep_key ? "SAME KEY + " : "NEW PIECE + "); text_cat(t, FX_NAME[fx]);
        pop(t, nm, LAYER_C[(seed >> 3) % NUM_LAYERS]);
        text_cat(ui.pop_3, NOTE_NAME[set.cur.key]); text_cat(ui.pop_3, " "); text_cat(ui.pop_3, SCALES[set.cur.scale].name);
        text_cat(ui.pop_3, "  "); int_text(40 + set.cur.tempo, n); text_cat(ui.pop_3, n); text_cat(ui.pop_3, " BPM");
    }
}

static void new_layer(void)
{
    char s[40] = "";
    piece_roll_layer(&set.cur, ui.sel, plat_ms() * 2654435761u ^ plat_ticks24() ^ (uint32_t)ui.sel);
    changed();
    text_cat(s, SEQ_NAME[set.cur.l[ui.sel].seq]); text_cat(s, " + "); text_cat(s, ENGINE_NAME[set.cur.l[ui.sel].engine]);
    pop(LAYER_NAME[ui.sel], "ANEW", LAYER_C[ui.sel]);
    text_cat(ui.pop_3, s);
}

static void hist_step(int d)
{
    char nm[32];
    if (d < 0) {
        if (hist_at == 0) { pop("HISTORY", "THE FIRST", C_SUB); return; }
        if (hist_at == hist_n) { hist_push(); hist_at = hist_n - 1; }   /* (the one playing kept, to come back to) */
        set.cur = hist[--hist_at];
    } else {
        if (hist_at + 1 >= hist_n) { pop("HISTORY", "THE LATEST", C_SUB); return; }
        set.cur = hist[++hist_at];
    }
    set.slot = 255;
    changed();
    piece_name(set.cur.seed, nm);
    pop(d < 0 ? "BACK TO" : "FORWARD TO", nm, C_TXT);
}

/* SAVE while the music plays: the flash's erase stops the sound for tens of ms, so the sound fades out first (a few
 * ms), the save, and back in: a soft dip, not a crunch */
static int save_pending; static uint32_t save_t;   /* the slot (+1) waiting for the fade, since when */
static void store_now(int slot)
{
    store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
    char s[12] = "SLOT ", n[4]; int_text(slot + 1, n); text_cat(s, n);
    if (store_ok) pop("SAVED", s, C_TXT);
    else pop("NOT SAVED", "UNTIL POWER OFF", C_RED);
}

static void save_piece(void)
{
    int slot = set.slot != 255 ? set.slot : set.nsaved < NSAVED ? set.nsaved++ : NSAVED - 1;
    set.saved[slot] = set.cur; set.slot = (uint8_t)slot;
    set_dirty = 0; autosave_at = 0;
    if (set.playing) { save_pending = slot + 1; save_hold = 1; save_t = plat_ms(); return; }
    store_now(slot);
}

static void load_step(int d)
{
    if (!set.nsaved) { pop("NO PIECES", "SAVE ONE FIRST", C_SUB); return; }
    int at = set.slot == 255 ? (d > 0 ? 0 : set.nsaved - 1) : (set.slot + d % set.nsaved + set.nsaved) % set.nsaved;
    if (set.slot == 255) hist_push();
    set.slot = (uint8_t)at; set.cur = set.saved[at];
    changed(); set_dirty = 0;
    ui.list_until = plat_ms() + 2000u; ui.pop_until = 0;
    char nm[32]; piece_name(set.cur.seed, nm); say_text("PIECE", nm);
}

static uint32_t stopped_at;               /* when PLAY stopped it (the fade's end: the moment to keep the changes) */
static void play_toggle(void)
{
    set.playing = (uint8_t)!set.playing;
    if (!set.playing) stopped_at = plat_ms();
    eng.playing = set.playing;
    if (set.midi_on && CLK_OUT) {            /* leading: Start from the top of the bar, so the follower's 1 is ours */
        if (set.playing) eng.sync_req = 1;
        midi_send(0x0Fu | (set.playing ? 0xFAu : 0xFCu) << 8);
    }
    if (!set.playing) engine_all_off(&eng);
    pop(set.playing ? "PLAY" : "STOP", "", C_TXT);
}

static void info(void)
{
    char s[28] = "CPU ", n[8];
    int_text((int32_t)plat_cpu_pct(), n); text_cat(s, n); text_cat(s, "%");
    pop(APP_NAME, s, store_ok ? C_TXT : C_RED);
    text_cat(ui.pop_3, two_cores && plat_cpu2_on() ? "2 CORES" : "1 CORE");
    {   char n[12]; text_cat(ui.pop_3, "  "); int_text((int32_t)plat_xruns(), n); text_cat(ui.pop_3, n); text_cat(ui.pop_3, " DROPS"); }
    text_cat(ui.pop_3, store_ok ? "  SAVING OK" : "  NO SAVING");
    ui.pop_until = plat_ms() + 3500u;
}

static void select_layer(int l)
{
    ui.sel = (l % NUM_LAYERS + NUM_LAYERS) % NUM_LAYERS;
    eng.key_layer = ui.sel;
    pop("LAYER", LAYER_NAME[ui.sel], LAYER_C[ui.sel]);
    char s[40] = ""; text_cat(s, SEQ_NAME[set.cur.l[ui.sel].seq]); text_cat(s, " + "); text_cat(s, ENGINE_NAME[set.cur.l[ui.sel].engine]);
    text_cat(ui.pop_3, s);
}

static void knob_turn(int k, int d)
{
    const Knob *kn = knob_of(ui.page, k); uint8_t *p = knob_ptr(ui.page, k);
    int v = kn->kind == K_OCT ? (int8_t)*p : *p, lo = 0, hi = 100, step = 2, wrap = 0;
    if (kn->kind == K_NONE) return;
    switch (kn->kind) {
    case K_INCH: hi = 16; step = 1; break;
    case K_REC: hi = 1; step = 1; break;
    case K_PEGN: hi = NPEG_NS - 1; step = 1; break;
    case K_VIS: hi = 2; step = 1; break;
    case K_SHAPE: hi = 1; step = 1; break;
    case K_SYNC: hi = NSYNC - 1; step = 1; break;
    case K_ENGINE: hi = NUM_ENGINES - 1; step = 1; wrap = 1; break;
    case K_SEQ: hi = NUM_SEQS - 1; step = 1; wrap = 1; break;
    case K_RATE: hi = NUM_RATES - 1; step = 1; break;
    case K_OCT: lo = -2; hi = 2; step = 1; break;
    case K_KEY: hi = 11; step = 1; wrap = 1; break;
    case K_SCALE: hi = NUM_SCALES - 1; step = 1; wrap = 1; break;
    case K_CHORD: hi = NUM_CHORDS - 1; step = 1; wrap = 1; break;
    case K_TEMPO: hi = 140; step = 1; break;
    case K_ONOFF: hi = 1; step = 1; break;
    case K_CH: hi = k == 1 ? 12 : 15; step = 1; break;
    default: break;
    }
    v += d * step;
    if (wrap) v = ((v - lo) % (hi - lo + 1) + (hi - lo + 1)) % (hi - lo + 1) + lo;
    else v = v < lo ? lo : v > hi ? hi : v;
    if (kn->kind == K_OCT) *(int8_t *)p = (int8_t)v; else *p = (uint8_t)v;
    if (kn->where == 2) { autosave_at = plat_ms() + AUTOSAVE_MS; settings_apply(); }
    else changed();
    if (kn->kind == K_ONOFF && kn->off == SO(midi_on) && !v) engine_all_off(&eng);
    ui.last_knob = k; ui.knob_at = plat_ms(); ui.pop_until = 0;
    if (ann_on()) { char s[24]; knob_text(ui.page, k, s); say_text(kn->name, s); }
}

/* the settings that reach the platform and the screen */
static void settings_apply(void)
{
    plat_usb_audio_full(set.usb_full);
    plat_usb_play_gain((uint32_t)set.usb_play * 4096u / 100u);
    palette_apply();
}

/* HOME: the knobs are the layers' levels, shown while they turn (HOME held: the macros) */
static void level_turn(int l, int d)
{
    LayerP *lp = &set.cur.l[l];
    lp->level = (uint8_t)clamp(lp->level + 2 * d, 0, 100);
    changed();
    ui.panel_kind = 0; ui.panel_until = plat_ms() + 2000u; ui.last_knob = l; ui.knob_at = plat_ms(); ui.pop_until = 0;
    if (ann_on()) { char n[8]; int_text(lp->level, n); say_text(LAYER_NAME[l], n); }
}

static void page_to(int pg)
{
    ui.page = pg; ui.knob_at = 0; ui.pop_until = 0;
    if (ann_on()) say_text(PAGE_NAME[pg], page_has_layer(pg) ? LAYER_NAME[ui.sel] : "");
}

/* ------------------------------------------------------------------ the keys: the scale's notes and chords */
static void keys_input(uint32_t keys)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    const Scale *sc = &SCALES[set.cur.scale % NUM_SCALES];
    int base = ROLES[ui.sel].base + set.cur.key + 12 * set.cur.l[ui.sel].octave;
    for (int i = 0; i < 16; i++) {
        int kk = WK[i]; uint32_t b = 1u << kk;
        if (kk >= NKEYS) continue;
        if ((keys & b) && !(ui.keys & b)) {
            int note = deg_note(sc, base, i), v = touch_vel((uint32_t)kk);
            ui.key_notes[kk][0] = (uint8_t)note; ui.key_n[kk] = 1; ui.last_note = note;
            engine_key(&eng, 1, note, v ? v : 100, -1);
        }
    }
    for (int j = 0; j < 11; j++) {
        int kk = BK[j]; uint32_t b = 1u << kk;
        if (kk >= NKEYS) continue;
        if ((keys & b) && !(ui.keys & b)) {
            int d[5], n = chord_degs(set.cur.chord, j, d), v = touch_vel((uint32_t)kk);
            if (ui.sel == L_GROUND) base += 12;      /* (chords down there would be mud) */
            for (int c = 0; c < n; c++) { int note = deg_note(sc, base, d[c]); ui.key_notes[kk][c] = (uint8_t)note; engine_key(&eng, 1, note, (v ? v : 100) * 3 / 4, -1); }
            ui.last_note = ui.key_notes[kk][0]; ui.last_chord = n;
            ui.key_n[kk] = (uint8_t)n;
            if (ui.sel == L_GROUND) base -= 12;
        }
    }
    for (int k = 0; k < NKEYS; k++)
        if (!(keys >> k & 1u) && (ui.keys >> k & 1u)) {
            for (int c = 0; c < ui.key_n[k]; c++) engine_key(&eng, 0, ui.key_notes[k][c], 0, -1);
            ui.key_n[k] = 0;
        }
}

/* ------------------------------------------------------------------ input */
static int32_t enc(int e) { int32_t d = plat_enc(e); if (d) ui.touched = 1; return d; }

static void input(void)
{
    uint32_t btn = plat_buttons(), keys;
    touch_poll();
    keys = touch_keys();
    uint32_t down = btn & ~ui.btn, up = ui.btn & ~btn;
    int32_t d;
    if (down || (keys & ~ui.keys)) ui.touched = 1;

    {   /* a page's button: its page; pressed again, the next of its pages (GLO, ARP) */
        static const uint8_t BTNS[7] = { B_EDIT, B_SEQ, B_ENV, B_FX, B_LFO, B_GLO, B_ARP };
        for (int b = 0; b < 7; b++) {
            if (!(down >> BTNS[b] & 1u)) continue;
            int first = -1, next = -1;
            for (int pg = 1; pg < NPAGES; pg++) if (PAGE_BTN[pg] == BTNS[b]) { if (first < 0) first = pg; if (next < 0 && pg > ui.page && PAGE_BTN[ui.page] == BTNS[b]) next = pg; }
            page_to(PAGE_BTN[ui.page] == BTNS[b] ? (next >= 0 ? next : first) : first);
        }
    }
    /* HOME: from another page, home; held, the macros; tapped at home, how the FM-1 is doing */
    if (down >> B_HOME & 1u) { ui.home_used = ui.page != PG_HOME; if (ui.page != PG_HOME) page_to(PG_HOME); }
    if ((up >> B_HOME & 1u) && !ui.home_used) info();
    if (down >> B_SEL & 1u) ui.sel_used = 0;
    if ((up >> B_SEL & 1u) && !ui.sel_used) select_layer(ui.sel + 1);
    if (down >> B_REC & 1u) {
        if (btn >> B_SEL & 1u) { new_layer(); ui.sel_used = 1; ui.rec_t = 0; }
        else { ui.rec_t = plat_ms(); ui.rec_used = 0; }
    }
    if (ui.rec_t && (btn >> B_REC & 1u) && !ui.rec_used && plat_ms() - ui.rec_t >= 900u) { new_piece(1); ui.rec_used = 1; }
    if (ui.rec_t && (up >> B_REC & 1u)) { if (!ui.rec_used) new_piece(0); ui.rec_t = 0; }
    if (down >> B_SAVE & 1u) save_piece();
    if (down >> B_PLAY & 1u) play_toggle();
    if (down >> B_OCTDN & 1u) hist_step(-1);
    if (down >> B_OCTUP & 1u) hist_step(1);
    keys_input(keys);

    for (int k = 0; k < 4; k++) {
        if ((d = enc(EN_K1 + k)) == 0) continue;
        if (ui.page != PG_HOME) knob_turn(k, (int)d);
        else if (btn >> B_HOME & 1u) { knob_turn(k, (int)d); ui.home_used = 1; ui.panel_kind = 1; ui.panel_until = plat_ms() + 1500u; }
        else level_turn(k, (int)d);
    }
    if (btn >> B_HOME & 1u) { ui.panel_kind = 1; ui.panel_until = plat_ms() + 300u; }   /* held: the macros stay up */
    if ((d = enc(EN_SELECT)) != 0) select_layer(ui.sel + (int)d);
    if ((d = enc(EN_ALGO)) != 0) load_step((int)d);
    if ((d = enc(EN_PRESET)) != 0) {
        int pg = ui.page; ui.page = PG_SOUND; knob_turn(0, (int)d); ui.page = pg;
        if (pg != PG_SOUND) { char s[24]; knob_text(PG_SOUND, 0, s); pop(LAYER_NAME[ui.sel], s, LAYER_C[ui.sel]); }
    }

    ui.btn = btn; ui.keys = keys;
    /* the lights: the page's button lit, the others that do something glowing; SAVE lit while unsaved; PLAY green */
    {
        uint32_t used = 1u << B_SEL | 1u << B_REC | 1u << B_SAVE | 1u << B_OCTDN | 1u << B_OCTUP | 1u << B_PLAY;
        for (int pg = 0; pg < NPAGES; pg++) used |= 1u << PAGE_BTN[pg];
        uint32_t lit = (btn & used) | 1u << PAGE_BTN[ui.page] | (set_dirty ? 1u << B_SAVE : 0u) | (set.playing ? LED_PLAY_GREEN : 0u);
        plat_lights(lit, keys, used | LED_PLAY_GREEN, 0x7FFFFFFu, 1);
    }
}

void app_overload(void) { engine_overload(&eng); }   /* (from the audio interrupt) */
void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ the view: a pegboard of light
 * Each note lights the peg at its pitch (low at the bottom) and its place in the stereo field, in its layer's colour,
 * and fades as it ends. Whenever two colours are lit, a new peg appears about halfway between them in their mix (red and
 * blue: purple); mixes mix with notes and with each other, until the board is full; then the oldest fade out and new
 * ones in. Each mix breathes at its own pace. Crisp pegs, no glow, and only the lit ones: the rest of the board black.
 * SCREEN: COUNT (pegs across), SIZE, BREED (how fast mixes come), FADE (how long things linger). */
#define HEAD_H 30
static int full_now(void);
#define VIEW_Y (full_now() ? 0 : HEAD_H)   /* at HOME, the view the whole screen, no header */
#define VIEW_H (240 - VIEW_Y)
#define PANEL_H 110
#define PANEL_TOP (VIEW_H - PANEL_H)     /* the panel, over the view's lower part */

static uint16_t dim(uint16_t c, float k)
{
    if (k >= 1) return c;
    if (k <= 0) return C_BLACK;
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)(((uint32_t)((float)r * k) << 11) | ((uint32_t)((float)g * k) << 5) | (uint32_t)((float)b * k));
}

static uint32_t jrng = 0x2545F491u;
static uint32_t jr(void) { jrng ^= jrng << 13; jrng ^= jrng >> 17; jrng ^= jrng << 5; return jrng; }

#define PG_MAX 24                                /* the most pegs across and down */
#define PG_NOTES 48
typedef struct { uint16_t col; uint16_t age, die; uint8_t ph, sp; } mixpeg_t;   /* age, die: 50 ms ticks (die 0: living) */
static mixpeg_t mp[PG_MAX][PG_MAX];
static struct { uint8_t on, x, y, layer, vel; uint16_t t, len; } pn[PG_NOTES];   /* the notes' pegs: 50 ms ticks */
static uint16_t pg_t;                            /* the board's clock, 50 ms ticks */
static float pg_acc, pg_tick;
static int pg_n_now = -1;                        /* the COUNT the board was laid out for */

static int splash_pegs;                   /* the splash: the board at COUNT 8, SIZE, BREED and FADE full */
static float pegs_gain = 1.0f;            /* (the splash's fade out at its end) */
static int peg_cols(void) { return splash_pegs ? 8 : PEG_NS[set.peg_n % NPEG_NS]; }
static int peg_cell(void) { return 240 / peg_cols(); }
static int peg_rows(void) { int r = VIEW_H / peg_cell(); return r > PG_MAX ? PG_MAX : r; }
static float fade_k(void) { return 0.4f + (float)(splash_pegs ? 100 : set.fade) * 0.016f; }   /* FADE: 0.4 .. 2 */

static float pn_level(int i)                     /* a note's peg: lit as it sounds, fading after */
{
    if (!pn[i].on) return 0;
    uint16_t a = (uint16_t)(pg_t - pn[i].t);
    float v = (float)pn[i].vel / 127.0f;
    if (a <= pn[i].len) return v;
    float k = fm_expf(-(float)(a - pn[i].len) * 0.05f / (0.6f * fade_k()));
    if (k < 0.01f) { pn[i].on = 0; return 0; }
    return v * k;
}

static float mp_level(const mixpeg_t *m, int x, int y)   /* a mix: fading in, breathing, fading out */
{
    if (!m->col) return 0;
    float inn = clampf((float)m->age / 30.0f, 0.0f, 1.0f), out = 1.0f;
    if (m->die) out = clampf(1.0f - (float)(uint16_t)(m->age - m->die) / (40.0f * fade_k() * 0.5f), 0.0f, 1.0f);
    float br = 0.45f + 0.4f * fm_sin_turns(ui.t * (0.1f + (float)m->sp * 0.002f) + (float)m->ph / 256.0f + (float)(x * 7 + y * 13) * 0.01f);
    return inn * out * br;
}

static uint16_t rgb_mix(uint16_t a, uint16_t b)  /* two colours' mix, as light: averaged, then brightened back */
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    float r = (float)(ar + br) * 0.5f / 31.0f, g = (float)(ag + bg) * 0.5f / 63.0f, bl = (float)(ab + bb) * 0.5f / 31.0f;
    float mean = (r + g + bl) * (1.0f / 3.0f);   /* pushed away from grey, so mixes of mixes stay coloured */
    r = fm_maxf(0.0f, mean + (r - mean) * 1.5f); g = fm_maxf(0.0f, mean + (g - mean) * 1.5f); bl = fm_maxf(0.0f, mean + (bl - mean) * 1.5f);
    float mx = fm_maxf(r, fm_maxf(g, bl)), k = mx > 0.01f ? fm_minf(1.6f, 0.92f / mx) : 1.0f;
    uint16_t c = RGB((int)(fm_minf(1.0f, r * k) * 255.0f), (int)(fm_minf(1.0f, g * k) * 255.0f), (int)(fm_minf(1.0f, bl * k) * 255.0f));
    return c ? c : 1;
}

static void pegs_note(const UiNote *n)
{
    int rows = peg_rows(), cols = peg_cols(), at = 0; uint16_t oldest = 0;
    for (int i = 0; i < PG_NOTES; i++) {
        if (!pn[i].on) { at = i; break; }
        uint16_t a = (uint16_t)(pg_t - pn[i].t); if (a > oldest) { oldest = a; at = i; }
    }
    int x = (int)(((float)n->pan / 100.0f * 0.5f + 0.5f) * (float)(cols - 1) + 0.5f);
    int y = (int)((1.0f - ((float)n->note - 34.0f) / 68.0f) * (float)(rows - 1) + 0.5f);
    pn[at].on = 1; pn[at].x = (uint8_t)clamp(x, 0, cols - 1); pn[at].y = (uint8_t)clamp(y, 0, rows - 1);
    pn[at].layer = n->layer; pn[at].vel = (uint8_t)(n->vel < 60 ? 60 : n->vel); pn[at].t = pg_t;
    pn[at].len = (uint16_t)(n->dur * 2);
}

static void pegs_breed(void)
{
    int rows = peg_rows(), cols = peg_cols();
    struct { uint8_t x, y; uint16_t c; uint8_t src; } lit[48]; int nl = 0;
    for (int i = 0; i < PG_NOTES && nl < 48; i++)
        if (pn_level(i) > 0.25f) { lit[nl].x = pn[i].x; lit[nl].y = pn[i].y; lit[nl].c = LAYER_C[pn[i].layer]; lit[nl].src = pn[i].layer; nl++; }
    for (int k = 0; k < 24 && nl < 48; k++) {   /* a few mixes too: mixes of mixes */
        int x = (int)(jr() % (uint32_t)cols), y = (int)(jr() % (uint32_t)rows);
        if (mp_level(&mp[y][x], x, y) > 0.3f) { lit[nl].x = (uint8_t)x; lit[nl].y = (uint8_t)y; lit[nl].c = mp[y][x].col; lit[nl].src = (uint8_t)(8 + (mp[y][x].col & 63)); nl++; }
    }
    if (nl < 2) return;
    int a = (int)(jr() % (uint32_t)nl), b = -1;
    for (int k = 0; k < 8; k++) { int c = (int)(jr() % (uint32_t)nl); if (lit[c].src != lit[a].src && lit[c].c != lit[a].c) { b = c; break; } }
    if (b < 0) return;
    int spread = cols / 8 + 1;                   /* (about halfway between them, a little either way) */
    int tx = (lit[a].x + lit[b].x) / 2 + (int)(jr() % (uint32_t)(2 * spread + 1)) - spread;
    int ty = (lit[a].y + lit[b].y) / 2 + (int)(jr() % (uint32_t)(2 * spread + 1)) - spread;
    int bx = -1, by = -1, bd = 1 << 30;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            if (mp[y][x].col) continue;
            int d = (x - tx) * (x - tx) + (y - ty) * (y - ty) + (int)(jr() & 3);
            if (d < bd) { bd = d; bx = x; by = y; }
        }
    if (bx < 0) {                                /* full: an old one starts to fade, to make room */
        for (int k = 0; k < 40; k++) { int x = (int)(jr() % (uint32_t)cols), y = (int)(jr() % (uint32_t)rows); if (mp[y][x].col && !mp[y][x].die) { mp[y][x].die = mp[y][x].age ? mp[y][x].age : 1; break; } }
        return;
    }
    mixpeg_t *m = &mp[by][bx];
    m->col = rgb_mix(lit[a].c, lit[b].c); m->age = 0; m->die = 0; m->ph = (uint8_t)jr(); m->sp = (uint8_t)jr();
}

static void pegs_step(float dt)
{
    int lay = splash_pegs ? -2 : set.peg_n;
    if (pg_n_now != lay) { pg_n_now = lay; memset(mp, 0, sizeof mp); memset(pn, 0, sizeof pn); }   /* (a new COUNT, or the splash's: a clean board) */
    uint32_t head = eng.uiq_head;                /* the notes played since the last frame */
    if (head - ui.uiq_tail > UI_Q) ui.uiq_tail = head - UI_Q;
    while (ui.uiq_tail != head) { UiNote n = eng.uiq[ui.uiq_tail % UI_Q]; ui.uiq_tail++; pegs_note(&n); }
    pg_tick += dt * 20.0f;
    int ticks = (int)pg_tick; pg_tick -= (float)ticks;
    pg_t = (uint16_t)(pg_t + ticks);
    int rows = peg_rows(), cols = peg_cols();
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            mixpeg_t *m = &mp[y][x];
            if (!m->col) continue;
            m->age = (uint16_t)(m->age + ticks);
            if (m->die && (float)(uint16_t)(m->age - m->die) > 40.0f * fade_k() * 0.5f) { m->col = 0; continue; }
            if (!m->die && m->age > (uint16_t)(400.0f + 240.0f * fade_k()) && (jr() & 63u) == 0) m->die = m->age;   /* (a life of 30 s or so) */
        }
    pg_acc += dt * (0.2f + (float)(splash_pegs ? 100 : set.breed) * 0.13f) * (0.2f + 0.8f * (float)(cols * cols) / 400.0f);   /* BREED: 0.2 .. 13 a second (on 20 x 20) */
    int guard = 0;
    while (pg_acc >= 1.0f && guard++ < 32) { pg_acc -= 1.0f; pegs_breed(); }
    if (pg_acc > 1.0f) pg_acc = 0;
}

/* a ball centred anywhere, to the sub-pixel (a pixel's centre is half a pixel in, so a ball on an even cell's middle
 * is symmetric, and at its full size fills the cell exactly), its edge smoothed: each edge pixel blended with what's
 * under it by how much of it the ball covers */
static inline void px_blend(int x, int y, uint16_t c, float k)
{
    y += cv_oy;
    if ((uint32_t)x >= cv_w || y < cv_y0 || y >= cv_y1) return;
    uint16_t *q = &cv_px[(uint32_t)y * cv_w + (uint32_t)x], u = swap16(*q);
    int ur = u >> 11, ug = (u >> 5) & 63, ub = u & 31, cr = c >> 11, cg = (c >> 5) & 63, cb = c & 31;
    *q = swap16((uint16_t)((ur + (int)((float)(cr - ur) * k)) << 11 | (ug + (int)((float)(cg - ug) * k)) << 5 | (ub + (int)((float)(cb - ub) * k))));
}

static void disc(float cx, float cy, float r, uint16_t c)
{
    if (r < 0.5f) r = 0.5f;
    const float W = 0.71f;                       /* (half a pixel's diagonal: the edge pixels, the ones the circle crosses) */
    float ro = r + W, ri = r - W, r2 = r * r;
    int y0 = (int)fm_floorf(cy - ro), y1 = (int)fm_floorf(cy + ro);
    for (int py = y0; py <= y1; py++) {
        if (py + cv_oy < cv_y0 || py + cv_oy >= cv_y1) continue;
        float dy = (float)py + 0.5f - cy, dy2 = dy * dy;
        if (dy2 >= ro * ro) continue;
        float ho = fm_sqrtf(ro * ro - dy2), hi = ri > 0 && dy2 < ri * ri ? fm_sqrtf(ri * ri - dy2) : -1.0f;
        int xo0 = -(int)fm_floorf(-(cx - ho - 0.5f)), xo1 = (int)fm_floorf(cx + ho - 0.5f);
        int xi0 = xo1 + 1, xi1 = xo1;            /* (the solid middle: none on the top and bottom rows) */
        if (hi >= 0) { xi0 = -(int)fm_floorf(-(cx - hi - 0.5f)); xi1 = (int)fm_floorf(cx + hi - 0.5f); }
        if (xi1 >= xi0) cv_rect(xi0, py, xi1 - xi0 + 1, 1, c);
        for (int x = xo0; x <= xo1; x++) {
            if (x >= xi0 && x <= xi1) { x = xi1; continue; }
            /* how much of the pixel the ball covers, exactly: sixteen points across it, counted */
            int n = 0;
            for (int j = 0; j < 4; j++) {
                float sy = (float)py + 0.125f + 0.25f * (float)j - cy, sy2 = sy * sy;
                for (int i = 0; i < 4; i++) { float sx = (float)x + 0.125f + 0.25f * (float)i - cx; n += sx * sx + sy2 <= r2; }
            }
            if (n == 0) continue;
            if (n == 16) cv_pset(x, py, c);
            else px_blend(x, py, c, fm_sqrtf((float)n * (1.0f / 16.0f)));   /* (the screen's gamma: half covered looks half as bright) */
        }
    }
}

static void draw_pegs(void)
{
    static uint8_t acc[PG_MAX][PG_MAX][3];
    int rows = peg_rows(), cols = peg_cols(), cell = peg_cell(), y0 = (VIEW_H - rows * cell) / 2;
    float rad = (float)cell * (0.12f + 0.0038f * (float)(splash_pegs ? 100 : set.peg_size));   /* SIZE: a speck .. near touching */
    if (rad > (float)cell * 0.5f - 0.75f) rad = (float)cell * 0.5f - 0.75f;   /* (a line of dark between: each ball its own circle) */
    memset(acc, 0, sizeof acc);
    for (int i = 0; i < PG_NOTES; i++) {           /* the notes' light, added */
        float lv = pn_level(i); if (lv <= 0.01f) continue;
        if (pn[i].x >= cols || pn[i].y >= rows) continue;
        uint16_t c = LAYER_C[pn[i].layer]; uint8_t *a = acc[pn[i].y][pn[i].x];
        int r = (int)((float)((c >> 11) * 255 / 31) * lv), g = (int)((float)(((c >> 5) & 63) * 255 / 63) * lv), b = (int)((float)((c & 31) * 255 / 31) * lv);
        a[0] = (uint8_t)clamp(a[0] + r, 0, 255); a[1] = (uint8_t)clamp(a[1] + g, 0, 255); a[2] = (uint8_t)clamp(a[2] + b, 0, 255);
    }
    for (int y = 0; y < rows; y++) {
        float py = (float)(y0 + y * cell) + (float)cell * 0.5f;   /* (the cell's exact middle: the board centred to the pixel) */
        if (py + (float)cell < (float)-cv_oy || py - (float)cell > (float)(-cv_oy + (int)cv_h)) continue;
        for (int x = 0; x < cols; x++) {
            float ml = mp_level(&mp[y][x], x, y);
            uint16_t mc = mp[y][x].col;
            int r = acc[y][x][0], g = acc[y][x][1], b = acc[y][x][2];
            if (ml > 0.01f && mc) { r += (int)((float)((mc >> 11) * 255 / 31) * ml); g += (int)((float)(((mc >> 5) & 63) * 255 / 63) * ml); b += (int)((float)((mc & 31) * 255 / 31) * ml); }
            if (r + g + b <= 45) continue;           /* unlit (or nearly out): nothing there */
            if (pegs_gain < 1.0f) { r = (int)((float)r * pegs_gain); g = (int)((float)g * pegs_gain); b = (int)((float)b * pegs_gain); }
            disc((float)(x * cell) + (float)cell * 0.5f, py, rad, RGB(clamp(r, 0, 255), clamp(g, 0, 255), clamp(b, 0, 255)));
        }
    }
}

/* ------------------------------------------------------------------ INKBLOT: a Rorschach, morphing with the music
 * A folded inkblot: the left half drawn, the right its mirror. Four inks, one a layer, drift and swell, larger as their
 * layer sounds; each note drops a blot of its layer's ink at its pitch (high at the top) and its place in the stereo field
 * (centre at the fold), which spreads and fades. Where inks meet their colours bleed into each other's mix; voids drift
 * through, carving black. Everything moves by the beat (the piece's tempo, or the clock it follows), each ink swelling a
 * little on the beat. SCREEN: SIZE, GAPS (the black through it), MIX (how far the colours bleed), FADE.
 * The ink is worked out every 4 pixels, a strip at a time, and smoothed between, so its edges stay round */
#define INK_MAX 18
#define INK_VOIDS 5
#define INK_G 4                                  /* the grid's step, pixels */
#define INK_GC (120 / INK_G + 1)                 /* its columns over the left half (31) */
#define INK_GR (60 / INK_G + 2)                  /* its rows over a 60-row strip, with the next strip's first */
#define INK_WD (INK_GC + 240 / INK_G + 2)
static float ink_wx[INK_GC], ink_wy[240 / INK_G + 2], ink_wd[INK_WD];   /* the edge's wobble, by column, row, diagonal */
static struct { float x, y, r, born, life, ph; uint8_t layer, on; } ink[INK_MAX];
static int ink_next = NUM_LAYERS;
static float ink_rgb[NUM_LAYERS][3], ink_beat, ink_lv[NUM_LAYERS];
static float ink_f[INK_GR][INK_GC];              /* the ink's depth (1.1 its edge), less the voids */
static uint8_t ink_c[INK_GR][INK_GC][3];         /* its colour */
static struct { float x, y, r2, k; } ink_now[INK_MAX];
static struct { float x, y, r2; } void_now[INK_VOIDS];

static void ink_colours(void)                    /* the palette's four, a near-white one turned to two others' mix, all vivid */
{
    for (int l = 0; l < NUM_LAYERS; l++) {
        uint16_t c = LAYER_C[l];
        int r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (mx - mn < 70) {
            c = rgb_mix(LAYER_C[(l + 1) % NUM_LAYERS], LAYER_C[(l + 2) % NUM_LAYERS]);
            r = (c >> 11) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31;
        }
        float m = (float)(r + g + b) / 3.0f, v[3] = { (float)r, (float)g, (float)b }, top = 1.0f;
        for (int k = 0; k < 3; k++) { v[k] = fm_maxf(0.0f, m + (v[k] - m) * 1.8f); top = fm_maxf(top, v[k]); }
        float s = fm_minf(1.5f, 235.0f / top);
        for (int k = 0; k < 3; k++) ink_rgb[l][k] = fm_minf(255.0f, v[k] * s);
    }
}

static float ink_strength(int i, float t)        /* in beats: in over 2, out over 4 */
{
    float a = t - ink[i].born, k = a < 2.0f ? 0.5f - 0.5f * fm_sin_turns(a * 0.25f + 0.25f) : 1.0f;
    if (a > ink[i].life) { float z = fm_minf(1.0f, (a - ink[i].life) * 0.25f); k *= 0.5f + 0.5f * fm_sin_turns(z * 0.5f + 0.25f); if (z >= 1.0f) ink[i].on = 0; }
    return k;
}

static void ink_note(const UiNote *n)
{
    int i = ink_next; ink_next = ink_next + 1 >= INK_MAX ? NUM_LAYERS : ink_next + 1;
    float side = (float)(n->pan < 0 ? -n->pan : n->pan) / 100.0f;
    ink[i].on = 1; ink[i].layer = n->layer % NUM_LAYERS; ink[i].born = ink_beat; ink[i].ph = (float)(jr() & 1023u) / 1023.0f;
    ink[i].x = 112.0f - side * 85.0f - (float)(jr() % 14u);
    ink[i].y = 24.0f + clampf(1.0f - ((float)n->note - 34.0f) / 68.0f, 0.0f, 1.0f) * 192.0f;
    ink[i].r = 9.0f + (float)n->vel / 127.0f * 9.0f;
    ink[i].life = 4.0f + (float)set.fade * 0.16f;
}

static int ink_n; static uint8_t ink_lay[INK_MAX];

static void ink_step(void)
{
    uint32_t head = eng.uiq_head;                /* the notes since the last frame */
    if (head - ui.uiq_tail > UI_Q) ui.uiq_tail = head - UI_Q;
    while (ui.uiq_tail != head) { UiNote n = eng.uiq[ui.uiq_tail % UI_Q]; ui.uiq_tail++; ink_note(&n); }
    float t = ink_beat = eng.beat;
    ink_colours();
    float size = 0.6f + (float)set.peg_size * 0.008f;
    float pulse = 0.5f + 0.5f * fm_sin_turns(t - fm_floorf(t) + 0.25f);   /* (a breath across each beat, not a twitch on it) */
    t *= 0.5f;                                   /* (the drift at half the beat's pace: smaller steps from frame to frame) */
    for (int l = 0; l < NUM_LAYERS; l++) {       /* the four inks, one a layer: larger as it sounds */
        float want = 0.45f + 0.55f * clamp01(eng.meter[l] * 1.4f);
        ink_lv[l] += (want - ink_lv[l]) * 0.04f;
        ink[l].on = 1; ink[l].layer = (uint8_t)l; ink[l].ph = 0.23f * (float)l + 0.1f;
        ink[l].x = 58.0f + 30.0f * fm_sin_turns(t * 0.018f + ink[l].ph);
        ink[l].y = 120.0f + 78.0f * fm_sin_turns(t * 0.011f + ink[l].ph * 1.7f);
        ink[l].r = 22.0f * ink_lv[l];
    }
    int n = 0;
    for (int i = 0; i < INK_MAX; i++) {
        if (!ink[i].on) continue;
        float k = i < NUM_LAYERS ? 1.0f : ink_strength(i, t);
        if (!ink[i].on || k < 0.002f) continue;
        float sw = 0.9f + 0.08f * fm_sin_turns(t * 0.13f + ink[i].ph) + 0.04f * pulse;   /* (swelling with the beat) */
        float x = ink[i].x, y = ink[i].y;
        if (i >= NUM_LAYERS) { x += 6.0f * fm_sin_turns(t * 0.07f + ink[i].ph); y += 6.0f * fm_sin_turns(t * 0.055f + ink[i].ph * 1.3f); }
        float r = ink[i].r * size * sw;
        ink_now[n].x = x; ink_now[n].y = y; ink_now[n].r2 = r * r; ink_now[n].k = k; ink_lay[n] = ink[i].layer;
        n++;
    }
    ink_n = n;
    for (int i = 0; i < INK_GC; i++) ink_wx[i] = 0.9f * fm_sin_turns((float)(i * INK_G) * 0.0143f + t * 0.08f);
    for (int i = 0; i < 240 / INK_G + 2; i++) ink_wy[i] = 0.8f * fm_sin_turns((float)(i * INK_G) * 0.0111f - t * 0.064f);
    for (int i = 0; i < INK_WD; i++) ink_wd[i] = 0.5f * fm_sin_turns((float)(i * INK_G) * 0.027f + t * 0.048f);
    float gap = (float)set.ink_gaps / 50.0f;     /* GAPS: 0 .. 2 */
    for (int v = 0; v < INK_VOIDS; v++) {
        float ph = 0.37f * (float)v, sp = 0.6f + 0.2f * (float)v;
        void_now[v].x = 70.0f + 38.0f * fm_sin_turns(t * 0.019f * sp + ph);
        void_now[v].y = 120.0f + 85.0f * fm_sin_turns(t * 0.014f * sp + ph * 2.1f);
        float r = (7.0f + (float)(v % 3) * 3.0f) * size;
        void_now[v].r2 = r * r * 0.9f * gap;
    }
}

/* the ink at one point of the grid: how deep (less the voids), and its colour */
static uint8_t ink_near[INK_MAX]; static int ink_nn;   /* the inks near the strip being drawn (the rest too far to count) */
static void ink_sample(float x, float y, float t, float mix, int gxi, int gyi, float *f_out, uint8_t *c)
{
    float f = 0, w = 0, cr = 0, cg = 0, cb = 0;
    for (int j = 0; j < ink_nn; j++) {
        int i = ink_near[j];
        float dx = x - ink_now[i].x, dy = y - ink_now[i].y, q = ink_now[i].r2 / (dx * dx + dy * dy + 30.0f) * ink_now[i].k;
        f += q;
        float qw = q * q * q * (1.0f - mix) + q * mix;   /* MIX: each its own colour near its heart .. all bled together */
        w += qw; cr += ink_rgb[ink_lay[i]][0] * qw; cg += ink_rgb[ink_lay[i]][1] * qw; cb += ink_rgb[ink_lay[i]][2] * qw;
    }
    float hole = 0;
    for (int v = 0; v < INK_VOIDS; v++) { float dx = x - void_now[v].x, dy = y - void_now[v].y; hole += void_now[v].r2 / (dx * dx + dy * dy + 12.0f); }
    (void)t;
    float wob = (ink_wx[gxi] + ink_wy[gyi] + ink_wd[(gxi + gyi) % INK_WD]) * 0.035f * fm_maxf(0.0f, 1.0f - fm_maxf(0.0f, f - 1.0f) * 2.0f);
    *f_out = f + wob - hole;
    if (*f_out < 0.8f) { c[0] = c[1] = c[2] = 0; return; }   /* (well outside the ink: no colour to work out) */
    if (w > 1e-6f) {                             /* the mix pushed back to full colour, as the pegs' are */
        float r = cr / w, g = cg / w, b = cb / w, m = (r + g + b) / 3.0f;
        r = fm_maxf(0.0f, m + (r - m) * 1.8f); g = fm_maxf(0.0f, m + (g - m) * 1.8f); b = fm_maxf(0.0f, m + (b - m) * 1.8f);
        float top = fm_maxf(1.0f, fm_maxf(r, fm_maxf(g, b))), s = fm_minf(1.5f, 235.0f / top);
        c[0] = (uint8_t)fm_minf(255.0f, r * s); c[1] = (uint8_t)fm_minf(255.0f, g * s); c[2] = (uint8_t)fm_minf(255.0f, b * s);
    } else c[0] = c[1] = c[2] = 0;
}

static void draw_ink(void)
{
    int v0 = -cv_oy, v1 = -cv_oy + (int)cv_h;    /* the strip's rows, in the view's own */
    if (v0 < 0) v0 = 0;
    if (v1 > VIEW_H) v1 = VIEW_H;
    if (v1 <= v0) return;
    int g0 = v0 / INK_G, g1 = (v1 - 1) / INK_G + 1;
    if (g1 - g0 + 1 > INK_GR) g1 = g0 + INK_GR - 1;
    float t = ink_beat, mix = (float)set.ink_mix / 100.0f;
    {   /* (an ink farther than 4.5 radii from the strip adds under 5 %: left out) */
        float ya = (float)(g0 * INK_G) * 240.0f / (float)VIEW_H, yb = (float)(g1 * INK_G) * 240.0f / (float)VIEW_H;
        ink_nn = 0;
        for (int i = 0; i < ink_n; i++) {
            float reach = 4.5f * fm_sqrtf(ink_now[i].r2);
            if (ink_now[i].y + reach >= ya && ink_now[i].y - reach <= yb) ink_near[ink_nn++] = (uint8_t)i;
        }
    }
    for (int gy = g0; gy <= g1; gy++)
        for (int gx = 0; gx < INK_GC; gx++)
            ink_sample((float)(gx * INK_G), (float)(gy * INK_G) * 240.0f / (float)VIEW_H, t, mix, gx, gy, &ink_f[gy - g0][gx], ink_c[gy - g0][gx]);
    for (int v = v0; v < v1; v++) {
        int gy = v / INK_G - g0; float fy = (float)(v % INK_G) * (1.0f / INK_G);
        int row = v + cv_oy;
        if (row < cv_y0 || row >= cv_y1) continue;
        uint16_t *px = &cv_px[(uint32_t)row * cv_w];
        float fr[INK_GC], cr[INK_GC][3];          /* this row's values at each grid column, once */
        for (int gx = 0; gx < INK_GC; gx++) {
            fr[gx] = ink_f[gy][gx] + (ink_f[gy + 1][gx] - ink_f[gy][gx]) * fy;
            for (int k = 0; k < 3; k++) cr[gx][k] = (float)ink_c[gy][gx][k] + ((float)ink_c[gy + 1][gx][k] - (float)ink_c[gy][gx][k]) * fy;
        }
        for (int gx = 0; gx < INK_GC - 1; gx++) {
            float fa = fr[gx], fb = fr[gx + 1];
            if (fa < 1.1f && fb < 1.1f) continue; /* (a stretch of black: nothing to draw) */
            for (int i = 0; i < INK_G; i++) {
                int x = gx * INK_G + i;
                float fx = (float)i * (1.0f / INK_G), f = fa + (fb - fa) * fx, edge = (f - 1.1f) * (1.0f / 0.18f);
                if (edge <= 0.0f) continue;
                if (edge > 1.0f) edge = 1.0f;
                float sh = edge * 0.88f * (0.88f + 0.12f * clamp01((f - 1.1f) * 2.0f));   /* (a slightly deeper rim, as pooled ink) */
                int c0 = (int)((cr[gx][0] + (cr[gx + 1][0] - cr[gx][0]) * fx) * sh);
                int c1 = (int)((cr[gx][1] + (cr[gx + 1][1] - cr[gx][1]) * fx) * sh);
                int c2 = (int)((cr[gx][2] + (cr[gx + 1][2] - cr[gx][2]) * fx) * sh);
                uint16_t c = swap16(RGB(c0, c1, c2));
                px[x] = c; px[239 - x] = c;   /* (folded: the right half the left's mirror) */
            }
        }
    }
}

/* ------------------------------------------------------------------ MOSH: blocks breeding and moulding
 * Like MATRIX, but free blocks: each note lights a block in its layer's ink where it sits in the stereo field (left to
 * right) at its pitch (high at the top): squares, rectangles and lines (SHAPES: BLOCKS, all three; LINES, mostly
 * lines). Mixes breed beside lit blocks, from a parent and one of its nearest neighbours of another colour, so they
 * grow where the music is. Overlaps mix their colours, and the blocks mould as they age, stretching or shrinking.
 * All in beats (the tempo, or the clock followed). SCREEN: SIZE (the zoom), BREED, MOULD, FADE. Edges smoothed.
 * Its cost is bounded whatever SIZE: blocks no bigger than the screen, at most MOSH_BUDGET screens' worth of
 * pixels drawn a frame (the oldest left out past it), and a run over the same colour mixed once */
#define MOSH_MAX 64
#define MOSH_BUDGET 3.5f                         /* screens of block drawn a frame, at most */
typedef struct {
    float cx, cy, w, h, born, life, dw, dh;      /* centre; half width and height (at 240 lines); their moulding */
    uint8_t on, ph; float col[3];
} Shard;
static Shard msh[MOSH_MAX];
static int msh_next; static float msh_breed_at, msh_t;
static float msh_scale = 1.0f;                 /* the budget's share, eased down if a frame's drawing runs long */

static float mosh_rf(void) { return (float)(jr() & 65535u) / 65535.0f; }
static void mosh_vivid(float *c)                 /* a mix pushed back to full colour, as the pegs' are */
{
    float m = (c[0] + c[1] + c[2]) / 3.0f, top = 1.0f;
    for (int k = 0; k < 3; k++) { c[k] = fm_maxf(0.0f, m + (c[k] - m) * 1.5f); top = fm_maxf(top, c[k]); }
    float s = fm_minf(1.6f, 235.0f / top);
    for (int k = 0; k < 3; k++) c[k] = fm_minf(255.0f, c[k] * s);
}

static void mosh_add(float cx, float cy, float size, const float *col)
{
    Shard *p = &msh[msh_next]; msh_next = (msh_next + 1) % MOSH_MAX;
    float r = mosh_rf(), w, h;
    float pline = set.clock_in == 1 ? 0.7f : 0.15f, psq = set.clock_in == 1 ? 0.3f : 0.4f;   /* SHAPES: BLOCKS, LINES */
    size = fm_minf(size, 90.0f);
    if (r < pline) {                             /* a line: long and thin, across or down */
        w = size * (1.8f + 2.2f * mosh_rf()); h = fm_maxf(0.9f, size * (0.05f + 0.08f * mosh_rf()));
    } else if (r < pline + psq) {                /* a square */
        w = h = size * (0.55f + 0.4f * mosh_rf());
    } else {                                     /* a rectangle */
        w = size * (0.9f + 0.8f * mosh_rf()); h = size * (0.35f + 0.4f * mosh_rf());
    }
    if (jr() & 1u) { float t = w; w = h; h = t; }
    p->on = 1; p->cx = clampf(cx, 8.0f, 232.0f); p->cy = clampf(cy, 8.0f, 232.0f);
    p->w = fm_minf(w, 120.0f); p->h = fm_minf(h, 120.0f); p->born = msh_t;
    p->life = (8.0f + 16.0f * mosh_rf()) * (0.4f + (float)set.fade * 0.016f);   /* FADE: 3 .. 48 beats lit */
    p->ph = (uint8_t)jr();
    p->dw = -0.2f + 0.5f * mosh_rf(); p->dh = -0.2f + 0.5f * mosh_rf();
    for (int k = 0; k < 3; k++) p->col[k] = col[k];
}

static float mosh_out(void) { return 3.0f + (float)set.fade * 0.09f; }   /* the fade out, beats: 3 .. 12 by FADE */
static float mosh_level(const Shard *p)          /* in over 1.5 beats, out over mosh_out's, breathing */
{
    float a = msh_t - p->born, k = fm_minf(1.0f, a / 1.5f);
    if (a > p->life) { float o = 1.0f - (a - p->life) / mosh_out(); k *= o > 0.0f ? o * o * (3.0f - 2.0f * o) : 0.0f; }
    return k * (0.78f + 0.22f * fm_sin_turns(msh_t * 0.143f + (float)p->ph / 256.0f));
}

static void mosh_breed(void)
{
    int lit[MOSH_MAX], nl = 0;
    for (int i = 0; i < MOSH_MAX; i++) if (msh[i].on && mosh_level(&msh[i]) > 0.4f) lit[nl++] = i;
    if (nl < 2) return;
    const Shard *a = &msh[lit[jr() % (uint32_t)nl]];
    int near[3] = { -1, -1, -1 }; float nd[3] = { 1e9f, 1e9f, 1e9f };   /* its three nearest of another colour */
    for (int j = 0; j < nl; j++) {
        const Shard *q = &msh[lit[j]];
        if (q == a || (fm_fabsf(q->col[0] - a->col[0]) + fm_fabsf(q->col[1] - a->col[1]) + fm_fabsf(q->col[2] - a->col[2])) < 30.0f) continue;
        float d = (q->cx - a->cx) * (q->cx - a->cx) + (q->cy - a->cy) * (q->cy - a->cy);
        for (int s = 0; s < 3; s++) if (d < nd[s]) { for (int u = 2; u > s; u--) { nd[u] = nd[u - 1]; near[u] = near[u - 1]; } nd[s] = d; near[s] = lit[j]; break; }
    }
    int cnt = near[2] >= 0 ? 3 : near[1] >= 0 ? 2 : near[0] >= 0 ? 1 : 0;
    if (!cnt) return;
    const Shard *b = &msh[near[jr() % (uint32_t)cnt]];
    float f = 0.12f + 0.23f * mosh_rf(), col[3];  /* beside the parent, not halfway: mixes grow where the sound is */
    for (int k = 0; k < 3; k++) col[k] = (a->col[k] + b->col[k]) * 0.5f;
    mosh_vivid(col);
    float sa = fm_sqrtf(a->w * a->h), sb = fm_sqrtf(b->w * b->h);   /* (a size that doesn't creep up generation on generation) */
    mosh_add(a->cx + (b->cx - a->cx) * f + (mosh_rf() - 0.5f) * 16.0f, a->cy + (b->cy - a->cy) * f + (mosh_rf() - 0.5f) * 16.0f,
             (sa + sb) * 0.5f * (0.75f + 0.3f * mosh_rf()), col);
}

static void mosh_step(void)
{
    uint32_t head = eng.uiq_head;
    if (head - ui.uiq_tail > UI_Q) ui.uiq_tail = head - UI_Q;
    ink_colours();                               /* (the palette's inks: a near-white one turned to two others' mix) */
    float zoom = 0.5f + (float)set.mosh_size * 0.017f;   /* SIZE: the zoom, 0.5 .. 2.2 (1.35 at 12 o'clock) */
    msh_t = eng.beat;
    while (ui.uiq_tail != head) {
        UiNote n = eng.uiq[ui.uiq_tail % UI_Q]; ui.uiq_tail++;
        float sp = (float)n.pan / 100.0f, sx = (sp < 0 ? -1.0f : 1.0f) * fm_sqrtf(fm_fabsf(sp));   /* (the field spread wide: a little pan goes far) */
        mosh_add(120.0f + sx * 108.0f + (mosh_rf() - 0.5f) * 36.0f,
                 24.0f + clampf(1.0f - ((float)n.note - 34.0f) / 68.0f, 0.0f, 1.0f) * 192.0f,
                 (9.0f + 14.0f * mosh_rf()) * zoom * (0.7f + 0.3f * (float)n.vel / 127.0f), ink_rgb[n.layer % NUM_LAYERS]);
    }
    float every = 4.0f / (1.0f + (float)set.breed * 0.15f);   /* BREED: a mix every 4 beats .. every quarter beat */
    if (msh_breed_at > msh_t + 8.0f || msh_breed_at < msh_t - 8.0f) msh_breed_at = msh_t;   /* (a new piece: its beat from 0) */
    int guard = 0;
    while (msh_breed_at < msh_t && guard++ < 8) { msh_breed_at += every; mosh_breed(); }
    for (int i = 0; i < MOSH_MAX; i++)
        if (msh[i].on && (msh_t - msh[i].born > msh[i].life + mosh_out() || msh_t < msh[i].born - 1.0f)) msh[i].on = 0;
}

typedef struct { float x0, x1, y0, y1; } MoshBox;
static int mosh_box(const Shard *p, MoshBox *b)  /* its edges now, in the view, clipped to the screen; 0 if off it */
{
    float age = msh_t - p->born, mould = (float)set.mosh_mould / 50.0f, sy = (float)VIEW_H / 240.0f;
    float gw = clampf(1.0f + p->dw * age * 0.12f * mould, 0.6f, 1.8f), gh = clampf(1.0f + p->dh * age * 0.12f * mould, 0.6f, 1.8f);
    float w = p->w * gw, h = p->h * gh;
    b->x0 = fm_maxf(0.0f, p->cx - w); b->x1 = fm_minf(240.0f, p->cx + w);
    b->y0 = fm_maxf(0.0f, (p->cy - h) * sy); b->y1 = fm_minf((float)VIEW_H, (p->cy + h) * sy);
    return b->x1 > b->x0 + 0.05f && b->y1 > b->y0 + 0.05f;
}

static void mosh_fill(const MoshBox *b, const float *col, float k)
{
    int r0 = -cv_oy + cv_y0, r1 = -cv_oy + cv_y1;    /* the band's rows, in the view: [r0, r1) */
    int ya = (int)b->y0, yb = (int)fm_minf(b->y1, (float)VIEW_H - 0.001f);
    if (ya < r0) ya = r0;
    if (yb > r1 - 1) yb = r1 - 1;
    if (yb < ya) return;
    int xa = (int)b->x0, xb = (int)fm_minf(b->x1, 239.999f);
    float cc[3] = { col[0] * k, col[1] * k, col[2] * k };
    uint16_t solid = swap16(RGB((int)cc[0], (int)cc[1], (int)cc[2]));
    float lcov = fm_minf(1.0f, (float)(xa + 1) - b->x0), rcov = b->x1 - (float)xb;
    if (xa == xb) lcov = rcov = b->x1 - b->x0;
    uint16_t last_u = 0, last_o = solid; int have = 0;   /* (a run over one colour: mixed once) */
    for (int y = ya; y <= yb; y++) {
        float vc = fm_minf((float)(y + 1), b->y1) - fm_maxf((float)y, b->y0);
        uint16_t *line = &cv_px[(uint32_t)(y + cv_oy) * cv_w];
        for (int x = xa; x <= xb; x++) {
            float cov = vc * (x == xa ? lcov : x == xb ? rcov : 1.0f);
            if (cov < 0.04f) continue;
            uint16_t su = line[x], u = swap16(su);
            int full = cov > 0.97f;
            if (full && have && su == last_u) { line[x] = last_o; continue; }
            float e[3] = { (float)((u >> 11) * 255 / 31), (float)(((u >> 5) & 63) * 255 / 63), (float)((u & 31) * 255 / 31) }, t[3];
            if (e[0] + e[1] + e[2] < 25.0f) { if (full) { line[x] = solid; continue; } t[0] = cc[0]; t[1] = cc[1]; t[2] = cc[2]; }
            else {                               /* over another: their mix */
                for (int c = 0; c < 3; c++) t[c] = (e[c] + col[c]) * 0.5f;
                mosh_vivid(t);
                float sc = fm_maxf(fm_maxf(fm_maxf(e[0], e[1]), fm_maxf(e[2], cc[0])), fm_maxf(cc[1], cc[2])) / 255.0f;
                for (int c = 0; c < 3; c++) t[c] *= sc;
            }
            float g = full ? 1.0f : fm_sqrtf(cov);   /* (the screen's gamma) */
            uint16_t o = swap16(RGB((int)(e[0] + (t[0] - e[0]) * g), (int)(e[1] + (t[1] - e[1]) * g), (int)(e[2] + (t[2] - e[2]) * g)));
            line[x] = o;
            if (full) { last_u = su; last_o = o; have = 1; }
        }
    }
}

static void draw_mosh(void)
{
    static MoshBox bx[MOSH_MAX]; static float kk[MOSH_MAX]; static uint8_t ok[MOSH_MAX];
    float budget = MOSH_BUDGET * msh_scale * 240.0f * (float)VIEW_H, used = 0.0f;
    for (int j = MOSH_MAX - 1; j >= 0; j--) {    /* newest first, until the budget's spent: the oldest left out */
        int i = (msh_next + j) % MOSH_MAX; const Shard *p = &msh[i];
        ok[i] = 0;
        if (!p->on || used > budget) continue;
        kk[i] = mosh_level(p);
        if (kk[i] < 0.02f || !mosh_box(p, &bx[i])) continue;
        used += (bx[i].x1 - bx[i].x0 + 1.0f) * (bx[i].y1 - bx[i].y0 + 1.0f);
        ok[i] = 1;
    }
    for (int j = 0; j < MOSH_MAX; j++) {         /* oldest first, so the newest lie on top */
        int i = (msh_next + j) % MOSH_MAX;
        if (ok[i]) mosh_fill(&bx[i], msh[i].col, kk[i]);
    }
}



static void frame_box(int x, int y, int w, int h, uint16_t c)
{
    cv_rect(x, y, w, h, C_BLACK);
    cv_rect(x, y, w, 2, c); cv_rect(x, y + h - 2, w, 2, c); cv_rect(x, y, 2, h, c); cv_rect(x + w - 2, y, 2, h, c);
}

static void draw_popup(void)
{
    int wt = text_w(&FONT_S, ui.pop_t), ws = text_w(&FONT_M, ui.pop_s), w3 = ui.pop_3[0] ? text_w(&FONT_S, ui.pop_3) : 0;
    int w = wt > ws ? wt : ws; if (w3 > w) w = w3;
    w = clamp(w + 28, 120, 236);
    int h = ui.pop_3[0] ? 78 : 58, y0 = (PANEL_TOP - h) / 2 + 4;
    frame_box(120 - w / 2, y0, w, h, ui.pop_c);
    cv_text(120 - wt / 2, y0 + 7, &FONT_S, ui.pop_t, ui.pop_c);
    cv_text(120 - ws / 2, y0 + 25, &FONT_M, ui.pop_s, C_TXT);
    if (ui.pop_3[0]) cv_text(120 - w3 / 2, y0 + 54, &FONT_S, ui.pop_3, C_SUB);
}

static void draw_list(void)                 /* the saved pieces, around the one playing */
{
    int n = set.nsaved, cur = set.slot == 255 ? 0 : set.slot, rows = 4;
    int first = clamp(cur - rows / 2, 0, n > rows ? n - rows : 0);
    frame_box(6, 4, 228, 94, C_TXT);
    cv_text(14, 6, &FONT_S, "PIECES", C_TXT);
    for (int i = 0; i < rows && first + i < n; i++) {
        char t[40], nm[32]; int y = 26 + i * 18;
        int_text(first + i + 1, t); text_cat(t, "  "); piece_name(set.saved[first + i].seed, nm); text_cat(t, nm);
        if (first + i == cur) { cv_rect(10, y - 1, 220, 18, C_TXT); cv_text(16, y, &FONT_S, t, C_BLACK); }
        else cv_text(16, y, &FONT_S, t, C_SUB);
    }
}

static int panel_on(void) { return ui.page != PG_HOME || plat_ms() < ui.panel_until; }
static void draw_panel_rows(int y0);

static void draw_view(void)
{
    uint32_t now = plat_ms();
    int panel = panel_on();
    uint32_t took = 0;                           /* (MOSH's drawing, the blits left out) */
    for (int top = 0; top < VIEW_H; top += 60) {
        int h = VIEW_H - top < 60 ? VIEW_H - top : 60;
        cv_begin(240, (uint32_t)h, C_BLACK);
        cv_oy = -top; cv_y0 = 0; cv_y1 = h;
        if (set.visual == 2) { uint32_t t0 = plat_ms(); draw_mosh(); took += plat_ms() - t0; }
        else if (set.visual) draw_ink(); else draw_pegs();
        if (!set.playing) { int w = text_w(&FONT_M, "STOPPED"); cv_text(120 - w / 2, (panel ? PANEL_TOP : VIEW_H) / 2 - 12, &FONT_M, "STOPPED", C_SUB); }
        if (panel && top + h > PANEL_TOP) { cv_rect(0, PANEL_TOP, 240, PANEL_H, C_BLACK); draw_panel_rows(PANEL_TOP); }
        if (now < ui.list_until) draw_list();
        else if (now < ui.pop_until) draw_popup();
        cv_blit(0, (uint32_t)(VIEW_Y + top));
    }
    if (set.visual == 2) {                       /* (MOSH: a long frame draws less next time, so the buttons stay quick) */
        if (took > 30u) msh_scale = fm_maxf(0.25f, msh_scale * 0.7f);
        else if (took < 15u) msh_scale = fm_minf(1.0f, msh_scale * 1.05f);
    }
}

static int full_now(void) { return ui.page == PG_HOME; }   /* (at HOME the pegs fill the whole screen) */

static void draw_header(void)
{
    char name[40], right[32] = "";
    piece_label(name);
    text_cat(right, NOTE_NAME[set.cur.key]); text_cat(right, " "); text_cat(right, SCALES[set.cur.scale].name);
    cv_begin(240, HEAD_H, C_BLACK);
    cv_text(4, 2, &FONT_B, name, C_TXT);
    {
        char t[12]; int_text(40 + set.cur.tempo, t); text_cat(t, " BPM");
        cv_text(4, 17, &FONT_XS, t, C_SUB);
        int x = 4 + text_w(&FONT_XS, t) + 10;
        for (int l = 0; l < NUM_LAYERS; l++) {   /* the four layers: their sequencers, lit as they sound */
            uint16_t c = dim(LAYER_C[l], 0.3f + 0.7f * eng.meter[l]);
            cv_rect(x, 19, 6, 6, c);
            x += 10;
        }
    }
    cv_text(236 - text_w(&FONT_XS, right), 4, &FONT_XS, right, C_SUB);
    cv_rect(0, HEAD_H - 1, 240, 1, C_RULE);
    cv_blit(0, 0);
}

/* the panel: the page's name (and its layer), the knob turned last in big letters, then the four knobs. At HOME:
 * the four layers' levels (each column's bar the layer sounding now, the white mark its level), or, HOME held, the
 * macros that steer where the music goes */
static void draw_levels(int y0)
{
    cv_rect(0, y0, 240, 1, C_RULE);
    for (int l = 0; l < NUM_LAYERS; l++) {
        int cx = 30 + 60 * l, top = y0 + 26, h = 54, hot = ui.knob_at && plat_ms() - ui.knob_at < 2000u && ui.last_knob == l;
        const char *nm = LAYER_NAME[l];
        if (hot) cv_rect(60 * l + 1, y0 + 1, 58, PANEL_H - 1, RGB(20, 20, 26));
        const felucca_font_t *nf = text_w(&FONT_B, nm) <= 57 ? &FONT_B : &FONT_S;
        cv_text(cx - text_w(nf, nm) / 2, y0 + 6, nf, nm, LAYER_C[l]);
        cv_rect(cx - 9, top, 18, h, C_WELL);
        int m = (int)(eng.meter[l] * (float)h);
        cv_rect(cx - 7, top + h - m, 14, m, dim(LAYER_C[l], 0.65f));
        int lv = set.cur.l[l].level, my = top + h - lv * h / 100;
        cv_rect(cx - 13, my - 1, 26, 3, C_WHITE);
        char v[8]; int_text(lv, v);
        cv_text(cx - text_w(&FONT_B, v) / 2, top + h + 5, &FONT_B, v, hot ? C_WHITE : C_TXT);
    }
}

static void draw_panel_rows(int y0)
{
    int pg = ui.page, lay = page_has_layer(pg);
    uint16_t pc = lay ? LAYER_C[ui.sel] : C_TXT;
    if (pg == PG_HOME && ui.panel_kind == 0) { draw_levels(y0); return; }
    cv_rect(0, y0, 240, lay ? 2 : 1, lay ? pc : C_RULE);   /* a layer's page: its colour across the top */
    int tx = cv_text(6, y0 + 4, &FONT_B, pg == PG_HOME ? "MACROS" : PAGE_NAME[pg], lay ? pc : C_TXT);
    if (pg != PG_HOME) {                         /* a button with more pages: a dot each, this one lit */
        int n = 0, at = 0;
        for (int q = 1; q < NPAGES; q++) if (PAGE_BTN[q] == PAGE_BTN[pg]) { if (q == pg) at = n; n++; }
        for (int d = 0; n > 1 && d < n; d++) cv_rect(tx + 8 + d * 8, y0 + 10, 5, 5, d == at ? (lay ? pc : C_TXT) : C_RULE);
    }
    if (lay) { const char *n = LAYER_NAME[ui.sel]; cv_text(234 - text_w(&FONT_B, n), y0 + 4, &FONT_B, n, pc); }
    /* the knob turned last, big */
    int focus = ui.knob_at && plat_ms() - ui.knob_at < 2500u;
    if (focus) {
        char v[24]; knob_text(pg, ui.last_knob, v);
        const char *nm = knob_of(pg, ui.last_knob)->name;
        int x = cv_text(6, y0 + 24, &FONT_S, nm, C_SUB);
        cv_text(x + 10, y0 + 20, &FONT_M, v, pc);
    } else {
        static const char *const HINT[NPAGES] = {
            "WHERE THE MUSIC GOES", "SELECT CHOOSES THE LAYER", "SELECT CHOOSES THE LAYER", "SELECT CHOOSES THE LAYER",
            "THE FILTER GLIDES, THE PITCH NEVER",
            "THIS LAYER INTO EACH EFFECT", "PAST HALF: SHIMMER, REVERSE ECHO", "HOW THE LAYERS MOVE", "THE PITCH WANDERING, AN OLD TAPE'S", "EVERYTHING IN THE SCALE",
            "THE PEGBOARD OF LIGHT", "THE LAYERS ON FOUR CHANNELS", "LAYER CHANNELS PLAY THE LAYERS",
            "RECORDING THE FM-1 OVER USB", "MATRIX, RORSCHACH OR MOSH" };
        cv_text(6, y0 + 26, &FONT_XS, pg == PG_SCREEN && set.visual == 2 ? "BLOCKS, BREEDING, MOULDING"
                : pg == PG_SCREEN && set.visual == 1 ? "THE INKBLOT, FOLDED" : HINT[pg], dim(C_SUB, 0.8f));
    }
    /* four columns: label, value, bar */
    for (int k = 0; k < 4; k++) {
        int cx = 60 * k + 30, y = y0 + 50;
        int hot = focus && k == ui.last_knob;
        const char *nm = knob_of(pg, k)->name;
        cv_text(cx - text_w(&FONT_XS, nm) / 2, y, &FONT_XS, nm, hot ? pc : C_SUB);
        char v[24]; knob_text(pg, k, v);
        int room = k < 3 && knob_of(pg, k + 1)->kind == K_NONE ? 112 : 56;   /* (the next column empty: its room too) */
        const felucca_font_t *f = text_w(&FONT_M, v) <= room ? &FONT_M : text_w(&FONT_B, v) <= room + 2 ? &FONT_B : &FONT_XS;
        if (text_w(f, v) > room + 2) v[9] = 0;
        int vx = cx - text_w(f, v) / 2;
        if (vx < 4) vx = 4;
        cv_text(vx, y + (f == &FONT_M ? 14 : 18), f, v, hot ? C_WHITE : C_TXT);
        int bw = 48, fill = (int)(knob_frac(pg, k) * (float)bw + 0.5f);
        cv_rect(cx - bw / 2, y + 44, bw, 4, C_WELL);
        cv_rect(cx - bw / 2, y + 44, fill, 4, dim(pc, hot ? 1.0f : 0.7f));
    }
}

/* a screen reader that asks hears the whole screen */
static void speak(void)
{
    if (ann_on() && ann_where()) {
        char t[160] = "", n[40];
        piece_label(n); text_cat(t, n); text_cat(t, ". ");
        text_cat(t, NOTE_NAME[set.cur.key]); text_cat(t, " "); text_cat(t, SCALES[set.cur.scale].name); text_cat(t, ". ");
        text_cat(t, PAGE_NAME[ui.page]); text_cat(t, " page");
        if (page_has_layer(ui.page)) { text_cat(t, ", "); text_cat(t, LAYER_NAME[ui.sel]); }
        for (int k = 0; k < 4; k++) {
            char v[24];
            if (ui.page == PG_HOME) { int_text(set.cur.l[k].level, v); text_cat(t, ". "); text_cat(t, LAYER_NAME[k]); }
            else { knob_text(ui.page, k, v); text_cat(t, ". "); text_cat(t, knob_of(ui.page, k)->name); }
            text_cat(t, " "); text_cat(t, v);
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

/* ------------------------------------------------------------------ the opening: the name and its afterimages */
#define SPLASH_HOLD 350.0f               /* the words spelled: held this long (ms), then all fades out over SPLASH_FADE */
#define SPLASH_FADE 600.0f
#define SPLASH_MS 2950u

/* the splash: the pegboard itself (COUNT 8, SIZE, BREED and FADE full), lit at random and breeding its mixes, and the
 * letters of AFTER IMAGES coming up one by one, in random order, each in a ball of its own colour on the board's own
 * grid, until the words are spelled */
static const char SPLASH_TXT[2][7] = { "AFTER", "IMAGES" };
static struct { uint8_t gx, gy; char ch; uint16_t at, col; } sp_let[11];
static uint32_t sp_last_note;

static uint16_t mix565(uint16_t a, uint16_t b, float k)   /* a towards b, by k */
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((ar + (int)((float)(br - ar) * k)) << 11 | (ag + (int)((float)(bg - ag) * k)) << 5 | (ab + (int)((float)(bb - ab) * k)));
}

void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    splash_pegs = 1; sp_last_note = 0;
    int n = 0; uint16_t last = 0;
    for (int w = 0; w < 2; w++)                  /* the words left-aligned in one column, so the block sits centred */
        for (int i = 0; SPLASH_TXT[w][i]; i++) {
            sp_let[n].gx = (uint8_t)(1 + i); sp_let[n].gy = (uint8_t)(3 + w); sp_let[n].ch = SPLASH_TXT[w][i];
            uint16_t c;
            do {                                 /* a layer's colour, or two's mix; never the one before again */
                int a = (int)(jr() % NUM_LAYERS), b = (int)(jr() % NUM_LAYERS);
                c = (jr() & 1u) || a == b ? LAYER_C[a] : rgb_mix(LAYER_C[a], LAYER_C[b]);
            } while (c == last);
            sp_let[n].col = last = c; n++;
        }
    for (int i = 0; i < 11; i++) sp_let[i].at = 0;
    for (int k = 0; k < 11; k++) {               /* their order: random */
        int i; do i = (int)(jr() % 11u); while (sp_let[i].at);
        sp_let[i].at = (uint16_t)(350 + k * 150 + (int)(jr() % 50u));
    }
}

static void splash_frame(uint32_t ms)
{
    static uint32_t prev;
    float dt = prev && ms > prev ? (float)(ms - prev) / 1000.0f : 0.03f; prev = ms;
    while (sp_last_note + 90u <= ms) {           /* the board lit at random, as by notes */
        sp_last_note += 90u;
        UiNote n; n.layer = (uint8_t)(jr() % NUM_LAYERS); n.note = (uint8_t)(34 + jr() % 68u); n.vel = 110;
        n.pan = (int8_t)((int)(jr() % 201u) - 100); n.dur = (uint8_t)(6 + jr() % 10u);
        pegs_note(&n);
    }
    pegs_step(dt);
    {   /* the words spelled: a moment, then everything fades out */
        int done = 0; for (int i = 0; i < 11; i++) if (sp_let[i].at > done) done = sp_let[i].at;
        pegs_gain = 1.0f - clampf(((float)ms - (float)done - SPLASH_HOLD) / SPLASH_FADE, 0.0f, 1.0f);
    }
    for (int top = 0; top < 240; top += 60) {
        cv_begin(240, 60, C_BLACK);
        cv_oy = -top; cv_y0 = 0; cv_y1 = 60;
        draw_pegs();
        for (int i = 0; i < 11; i++) {
            int a = (int)ms - (int)sp_let[i].at;
            if (a < 0) continue;
            int cx = sp_let[i].gx * 30 + 15, cy = sp_let[i].gy * 30 + 15;
            if (cy + 18 < top || cy - 18 >= top + 60) continue;
            float s = a < 90 ? (float)a / 90.0f * 1.08f : a < 170 ? 1.08f - (float)(a - 90) / 80.0f * 0.08f : 1.0f;
            uint16_t c = sp_let[i].col, col = a < 120 ? mix565(c, C_WHITE, 0.8f * (1.0f - (float)a / 120.0f)) : c;
            disc((float)cx, (float)cy, 14.25f * fm_minf(s, 1.0f), dim(col, pegs_gain));
            if (s > 0.6f) {
                int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
                float lum = 0.3f * (float)r / 31.0f + 0.59f * (float)g / 63.0f + 0.11f * (float)b / 31.0f;
                char t[2] = { sp_let[i].ch, 0 };
                cv_text(cx - text_w(&FONT_M, t) / 2, cy - 8, &FONT_M, t, lum > 0.55f ? C_BLACK : dim(C_WHITE, pegs_gain));   /* (the capitals' middle on the ball's) */
            }
        }
        cv_blit(0, (uint32_t)top);
    }
}

static void splash_end(void)
{
    ui.splash_on = 0; splash_pegs = 0; pegs_gain = 1.0f;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.head_key[0] = 0;
    ui.last_view = plat_ms();
}

void app_ui_init(void)
{
    touch_on(1);
    ui.sel = L_LIGHT; eng.key_layer = ui.sel;
    settings_apply();
    ui.page = PG_HOME;
    ui.uiq_tail = eng.uiq_head;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    app_splash();
    ui.splash_on = 1; ui.splash_t0 = plat_ms(); ui.touched = 0;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    input();
    speak();
    if (ui.splash_on) {
        static uint32_t last;
        if (ui.touched || now - ui.splash_t0 >= SPLASH_MS) splash_end();
        else { if (now - last >= 30u) { last = now; splash_frame(now - ui.splash_t0); } return; }
    }
    if (now - ui.last_view >= 40u) {             /* the view at 25 frames a second */
        float dt = (float)(now - ui.last_view) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        ui.last_view = now; ui.t += dt;
        if (set.visual == 2) mosh_step(); else if (set.visual) ink_step(); else pegs_step(dt);
        draw_view();
    }
    {   /* the load guard: past 80 % of the render's time, fewer notes, fewer voices, shorter tails; back slowly as it
         * eases (a block that nearly overruns sheds at once: app_overload) */
        static uint32_t last_g;
        if (now - last_g >= 20u) {
            last_g = now;
            uint32_t cpu = plat_cpu_pct();
            float g = eng.gov;
            if (cpu > 80u) g -= 0.03f; else if (cpu < 65u) g += 0.004f;
            eng.gov = g < 0.3f ? 0.3f : g > 1.0f ? 1.0f : g;
        }
    }
    if (save_pending && (gain_now < 0.004f || now - save_t > 250u)) {   /* SAVE: faded out, kept, back in */
        store_now(save_pending - 1); save_pending = 0; save_hold = 0;
    }
    /* the piece and settings kept: never while the music plays (erasing the flash stops the audio for tens of ms, a
     * drop), so when it has stopped and faded out (PLAY) */
    /* (and only once its tails have died away, or after 6 s; then the output faded to nothing first, as SAVE's) */
    static int auto_fading;
    if (auto_fading && set.playing) { auto_fading = 0; save_hold = 0; }   /* (PLAY again meanwhile: the sound back, kept later) */
    if (autosave_at && (int32_t)(now - autosave_at) >= 0 && !set.playing && now - stopped_at >= 2500u && !save_pending
        && (out_lvl < 0.002f || now - stopped_at >= 6000u)
        && (auto_fading || (save_hold = 1, auto_fading = 1, save_t = now, 0)) && (gain_now <= 0.004f || now - save_t >= 250u)) {
        autosave_at = 0; auto_fading = 0;
        store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
        save_hold = 0;
    }
    if (now - ui.last_head >= 100u && !full_now()) {   /* the header: its words, and the layers' lights */
        ui.last_head = now;
        draw_header();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "page")) *got = ui.page;
    else if (!strcmp(what, "sel")) *got = ui.sel;
    else if (!strcmp(what, "nsaved")) *got = set.nsaved;
    else if (!strcmp(what, "slot")) *got = set.slot == 255 ? -1 : set.slot;
    else if (!strcmp(what, "playing")) *got = set.playing;
    else if (!strcmp(what, "dirty")) *got = set_dirty;
    else if (!strcmp(what, "seed")) *got = (int)(set.cur.seed & 0x7FFFFFFF);
    else if (!strcmp(what, "key")) *got = set.cur.key;
    else if (!strcmp(what, "scale")) *got = set.cur.scale;
    else if (!strcmp(what, "tempo")) *got = 40 + set.cur.tempo;
    else if (!strcmp(what, "mood")) *got = set.cur.mood;
    else if (!strcmp(what, "reverb")) *got = set.cur.reverb;
    else if (!strcmp(what, "midi_ch")) *got = set.midi_ch;
    else if (!strcmp(what, "hist")) *got = hist_n;
    else if (!strcmp(what, "lastnote")) *got = ui.last_note;
    else if (!strncmp(what, "sweep", 5)) *got = set.cur.sweep[atoi(what + 5) & 3];
    else if (!strcmp(what, "layers_in")) *got = set.layers_in;
    else if (!strcmp(what, "sync")) *got = set.sync;
    else if (!strcmp(what, "clockbpm")) *got = (int)(eng.bpm + 0.5f);
    else if (!strcmp(what, "beatlag")) {        /* following a clock: our beat off the leader's, in 1/100 beat (within a bar) */
        float err = ((float)clk_n / 24.0f) - eng.beat; err -= 4.0f * fm_floorf(err * 0.25f + 0.5f);
        *got = (int)(err * 100.0f + (err < 0 ? -0.5f : 0.5f));
    }
    else if (!strcmp(what, "lastchord")) *got = ui.last_chord;
    else if (!strncmp(what, "seq", 3)) *got = set.cur.l[atoi(what + 3) & 3].seq;
    else if (!strncmp(what, "engine", 6)) *got = set.cur.l[atoi(what + 6) & 3].engine;
    else if (!strncmp(what, "level", 5)) *got = set.cur.l[atoi(what + 5) & 3].level;
    else if (!strncmp(what, "events", 6)) *got = eng.events[atoi(what + 6) & 3];
    else return 0;
    return 1;
}
#endif

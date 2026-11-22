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
    uint8_t nsaved, midi_on, midi_ch, midi_keys, midi_clock, playing, rsv;
    Piece cur;
    Piece saved[NSAVED];
    /* v0.2 */
    uint8_t in_ch;                       /* MIDI in: notes on this channel play the chosen layer (0: any) */
    uint8_t layers_in;                   /* notes on the layers' channels (CHANNEL's four) play those layers */
    uint8_t clock_in;                    /* follow MIDI clock: its tempo, its start and stop */
    uint8_t cc_in;                       /* CCs move the macros */
    uint8_t usb_full;                    /* USB recording at full level, not at MASTER */
    uint8_t usb_play;                    /* the computer's audio through the FM-1, 0..100 */
    uint8_t rsv4;                        /* (v0.2-0.7's STEMS: USB audio is the one stereo mix now) */
    uint8_t palette, rsv3[3];            /* the screen: the palette (dealt by REC); (v0.5-0.7's VISUAL, FULL, GLOW) */
    uint8_t breed, fade, pegs_set;       /* PEGS: how fast mixes breed, how long things linger (0..100); pegs_set: 2 once all four are set */
    uint8_t peg_n, peg_size;             /* PEGS: COUNT (an index into PEG_NS), SIZE 0..100 */
} settings_t;
static settings_t set;
#define NPAL 9
#define NPEG_NS 6
static const uint8_t PEG_NS[NPEG_NS] = { 8, 10, 12, 15, 20, 24 };   /* COUNT: pegs across (each divides 240) */
#define PEG_N_DEF 4
static uint8_t set_dirty;
static int store_ok = 1;
static uint32_t autosave_at;
#define AUTOSAVE_MS 8000u

static void settings_v2(void)             /* what v0.2 added, as it starts */
{
    set.in_ch = 0; set.layers_in = 1; set.clock_in = 0; set.cc_in = 1;
    set.usb_full = 0; set.usb_play = 100; set.rsv4 = 0;
    set.palette = 0; memset(set.rsv3, 0, sizeof set.rsv3);
    set.breed = 50; set.fade = 50; set.pegs_set = 2; set.peg_n = PEG_N_DEF; set.peg_size = 50;
}

static void settings_defaults(void)
{
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC; set.slot = 255;
    set.midi_on = 1; set.midi_ch = 0; set.midi_keys = 4; set.midi_clock = 0;
    piece_default(&set.cur);
    settings_v2();
}

static int settings_ok(void)
{
    if (set.magic != SET_MAGIC || set.nsaved > NSAVED || (set.slot != 255 && set.slot >= set.nsaved)) return 0;
    if (set.midi_ch > 12 || set.midi_keys > 15 || set.midi_on > 1 || set.midi_clock > 1) return 0;
    if (set.in_ch > 16 || set.layers_in > 1 || set.clock_in > 1 || set.cc_in > 1 || set.usb_full > 1 || set.usb_play > 100
        || set.palette >= NPAL || set.breed > 100 || set.fade > 100 || set.peg_n >= NPEG_NS || set.peg_size > 100) return 0;
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
    if (set.pegs_set != 1 && set.pegs_set != 2) { set.breed = 50; set.fade = 50; }   /* (a store from before PEGS) */
    if (set.pegs_set != 2) { set.peg_n = PEG_N_DEF; set.peg_size = 50; set.pegs_set = 2; memset(set.rsv3, 0, sizeof set.rsv3); }   /* (before COUNT, SIZE) */
    if (n != (int)sizeof set || !settings_ok()) {
        settings_defaults();
        store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
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
                if (set.clock_in && ms >= 200u && ms <= 3000u) {   /* (20 to 300 BPM) */
                    float b = 60000.0f / (float)ms;
                    ext_bpm = ext_bpm > 0 ? ext_bpm + (b - ext_bpm) * 0.5f : b;
                }
            }
            /* locked to the leader's beat, not only its tempo: at each of its beats (24 clocks from its Start) our beat
             * should be whole; how far off it is the engine takes up over the next moments (no notes skipped) */
            if (set.clock_in && set.playing && ++clk_n % 24u == 0) {
                /* within a bar: a new piece (REC) starts our count again, a missed Start leaves it anywhere; the bars stay
                 * the leader's either way (at most two beats to take up) */
                float err = (float)(clk_n / 24u) - eng.beat;
                err -= 4.0f * fm_floorf(err * 0.25f + 0.5f);
                eng.beat_adj = err;
            }
            if (set.clock_in) ext_at = now;
        } else if ((st == 0xFAu || st == 0xFBu) && set.clock_in) {
            eng.playing = set.playing = 1;
            if (st == 0xFAu) { eng.sync_req = 1; clk_n = 0; }   /* Start: the top of the bar, with the leader */
            else clk_n = (uint32_t)(eng.beat + 0.5f) * 24u;                  /* Continue: from where we are */
        }
        else if (st == 0xFCu && set.clock_in) { eng.playing = set.playing = 0; engine_all_off(&eng); }
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
    eng.ext_bpm = set.clock_in ? ext_bpm : 0.0f;
}


static void layers_job(uint32_t mask) { for (int b = 0; b < NUM_LAYERS; b++) if (mask >> b & 1u) engine_block_layer(&eng, b); }

static uint8_t balance(void)             /* the split that leaves the busier core least to do (the keys stay here) */
{
    uint8_t best = 0x3u; float best_t = 1e30f, total = eng.cost[KEYS];
    for (int l = 0; l < NUM_LAYERS; l++) total += eng.cost[l];
    if (total <= 0) return best;
    for (uint8_t m = 1; m < 15; m++) {
        float t2 = 0;
        for (int l = 0; l < NUM_LAYERS; l++) if (m >> l & 1u) t2 += eng.cost[l];
        float t = t2 > total - t2 ? t2 : total - t2;
        if (t < best_t - 1.0f) { best_t = t; best = m; }
    }
    return best;
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float L[ENGINE_SUB * BLK], R[ENGINE_SUB * BLK];
    float g = muted ? 0.0f : (float)master_q12 / 4096.0f * OUT_GAIN;
    int nsub = (int)(n / BLK);
    if (nsub > ENGINE_SUB) nsub = ENGINE_SUB;
    midi_in();
    eng.midi_on = set.midi_on; eng.midi_ch = set.midi_ch; eng.midi_keys_ch = set.midi_keys; eng.midi_clock = set.midi_clock;
    engine_block_begin(&eng, nsub);
    uint8_t mask = two_cores && plat_cpu2_on() ? on2_mask : 0;
    int sent = mask && plat_cpu2_run(layers_job, mask);
    layers_job(sent ? (uint32_t)(~mask & 0xFu) : 0xFu);
    engine_block_layer(&eng, KEYS);
    if (sent && plat_cpu2_wait() != 0) layers_job(mask);
    engine_block_mix(&eng, L, R);
    if (two_cores) on2_mask = balance();
    for (uint32_t i = 0; i < n && i < (uint32_t)(nsub * BLK); i++) {
        gain_now += (g - gain_now) * 0.002f;
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
enum { PG_HOME, PG_SOUND, PG_SEQ, PG_MIX, PG_VOICE, PG_FX, PG_FXG, PG_MOTION, PG_WARBLE, PG_SCALE, PG_SCREEN, PG_MIDI_OUT, PG_MIDI_IN, PG_USB, NPAGES };
static const char *const PAGE_NAME[NPAGES] = { "HOME", "SOUND", "SEQUENCE", "MIX", "VOICE", "SENDS", "EFFECTS", "MOTION", "WARBLE", "SCALE", "SCREEN",
                                                "MIDI OUT", "MIDI IN", "USB AUDIO" };
/* each page's button: pressed again, the next page of the same button (ENV: MIX, VOICE; GLO: SCALE, SCREEN; ARP: MIDI OUT,
 * MIDI IN, USB) */
static const uint8_t PAGE_BTN[NPAGES] = { B_HOME, B_EDIT, B_SEQ, B_ENV, B_ENV, B_FX, B_FX, B_LFO, B_LFO, B_GLO, B_GLO, B_ARP, B_ARP, B_ARP };
enum { K_NONE, K_PCT, K_ENGINE, K_SEQ, K_RATE, K_OCT, K_KEY, K_SCALE, K_CHORD, K_TEMPO, K_ONOFF, K_CH, K_INCH, K_REC,
       K_PEGN, K_DIRT, K_RVB, K_ECHO, K_GRAIN, K_TONE };
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
    { { "OUT", K_ONOFF, 2, SO(midi_on) }, { "CHANNEL", K_CH, 2, SO(midi_ch) }, { "KEYS CH", K_CH, 2, SO(midi_keys) }, { "CLOCK", K_ONOFF, 2, SO(midi_clock) } },
    { { "IN CH", K_INCH, 2, SO(in_ch) }, { "LAYERS", K_ONOFF, 2, SO(layers_in) }, { "CLOCK", K_ONOFF, 2, SO(clock_in) }, { "CC", K_ONOFF, 2, SO(cc_in) } },
    { { "RECORD", K_REC, 2, SO(usb_full) }, { "COMPUTER", K_PCT, 2, SO(usb_play) }, { "", K_NONE, 2, 0 }, { "", K_NONE, 2, 0 } },
};
static int page_has_layer(int pg) { return pg == PG_SOUND || pg == PG_SEQ || pg == PG_MIX || pg == PG_VOICE || pg == PG_FX; }

#define C_TXT RGB(232, 232, 232)
#define C_SUB RGB(150, 150, 160)
#define C_RULE RGB(40, 40, 48)
#define C_WELL RGB(30, 30, 36)
#define C_RED RGB(255, 90, 78)
/* each layer its colour, everywhere: on the view and on its pages; the palettes (SCREEN), GROUND BODY LIGHT DUST */
static const uint16_t LPAL[NPAL][NUM_LAYERS] = {
    { RGB(255, 72, 64), RGB(70, 140, 255), RGB(70, 230, 110), RGB(240, 240, 240) },
    { RGB(215, 45, 70), RGB(235, 90, 170), RGB(255, 150, 60), RGB(255, 230, 170) },
    { RGB(40, 90, 210), RGB(40, 165, 235), RGB(60, 230, 215), RGB(220, 245, 255) },
    { RGB(150, 70, 255), RGB(255, 90, 200), RGB(60, 220, 255), RGB(240, 220, 255) },
    { RGB(190, 35, 25), RGB(240, 95, 30), RGB(255, 175, 45), RGB(255, 235, 190) },
    { RGB(120, 140, 45), RGB(45, 175, 95), RGB(155, 230, 85), RGB(220, 255, 210) },
    { RGB(110, 110, 110), RGB(160, 160, 160), RGB(210, 210, 210), RGB(255, 255, 255) },
    { RGB(25, 120, 45), RGB(45, 175, 75), RGB(90, 235, 120), RGB(195, 255, 205) },
    { RGB(150, 75, 0), RGB(205, 115, 15), RGB(255, 165, 35), RGB(255, 222, 145) },
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
    case K_DIRT: text_cat(s, v < 34 ? "FOLD " : v < 67 ? "CRUSH " : "NOISE "); int_text(v < 34 ? v * 3 : v < 67 ? (v - 33) * 3 : (v - 66) * 3, s + strlen_(s)); break;
    case K_GRAIN: if (v < 92) int_text(v, s); else text_cat(s, "FREEZE"); break;
    case K_TONE: if (v >= 48 && v <= 52) text_cat(s, "OPEN"); else { text_cat(s, v < 50 ? "LP " : "HP "); int_text(v < 50 ? (50 - v) * 2 : (v - 50) * 2, s + strlen_(s)); } break;
    case K_PEGN: int_text(PEG_NS[v % NPEG_NS], s); break;
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

static void new_piece(int all)
{
    char nm[32];
    palette_next();
    int fx = fx_next();
    hist_push();
    uint32_t seed = (set.cur.seed * 1664525u + 1013904223u) ^ plat_ms() * 2654435761u ^ plat_ticks24();
    piece_roll(&set.cur, seed, all, fx);
    set.slot = 255;
    changed();
    piece_name(seed, nm);
    pop(all ? "A NEW WORLD" : "NEW PIECE", nm, LAYER_C[(seed >> 3) % NUM_LAYERS]);
    if (!all) { char n[8]; int_text(40 + set.cur.tempo, n); text_cat(ui.pop_3, n); text_cat(ui.pop_3, " BPM + "); text_cat(ui.pop_3, FX_NAME[fx]); }
    if (all) {
        text_cat(ui.pop_3, NOTE_NAME[set.cur.key]); text_cat(ui.pop_3, " "); text_cat(ui.pop_3, SCALES[set.cur.scale].name);
        char n[8]; text_cat(ui.pop_3, "  "); int_text(40 + set.cur.tempo, n); text_cat(ui.pop_3, n); text_cat(ui.pop_3, " BPM");
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

static void save_piece(void)
{
    int slot = set.slot != 255 ? set.slot : set.nsaved < NSAVED ? set.nsaved++ : NSAVED - 1;
    set.saved[slot] = set.cur; set.slot = (uint8_t)slot;
    set_dirty = 0; autosave_at = 0;
    store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
    char s[12] = "SLOT ", n[4]; int_text(slot + 1, n); text_cat(s, n);
    if (store_ok) pop("SAVED", s, C_TXT);
    else pop("NOT SAVED", "UNTIL POWER OFF", C_RED);
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
    if (set.midi_on && set.midi_clock) {     /* leading: Start from the top of the bar, so the follower's 1 is ours */
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

static int peg_cols(void) { return PEG_NS[set.peg_n % NPEG_NS]; }
static int peg_cell(void) { return 240 / peg_cols(); }
static int peg_rows(void) { int r = VIEW_H / peg_cell(); return r > PG_MAX ? PG_MAX : r; }
static float fade_k(void) { return 0.4f + (float)set.fade * 0.016f; }   /* FADE: 0.4 .. 2 */

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
    if (pg_n_now != set.peg_n) { pg_n_now = set.peg_n; memset(mp, 0, sizeof mp); memset(pn, 0, sizeof pn); }   /* (a new COUNT: a clean board) */
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
    pg_acc += dt * (0.2f + (float)set.breed * 0.13f) * (0.2f + 0.8f * (float)(cols * cols) / 400.0f);   /* BREED: 0.2 .. 13 a second (on 20 x 20) */
    int guard = 0;
    while (pg_acc >= 1.0f && guard++ < 32) { pg_acc -= 1.0f; pegs_breed(); }
    if (pg_acc > 1.0f) pg_acc = 0;
}

static void disc(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int w = 0; while ((w + 1) * (w + 1) + dy * dy <= r * r + r) w++;
        cv_rect(cx - w, cy + dy, 2 * w + 1, 1, c);
    }
}

static void draw_pegs(void)
{
    static uint8_t acc[PG_MAX][PG_MAX][3];
    int rows = peg_rows(), cols = peg_cols(), cell = peg_cell(), y0 = (VIEW_H - rows * cell) / 2;
    int rad = (int)((float)cell * (0.12f + 0.0038f * (float)set.peg_size) + 0.5f);   /* SIZE: a speck .. touching */
    if (rad < 1) rad = 1;
    memset(acc, 0, sizeof acc);
    for (int i = 0; i < PG_NOTES; i++) {           /* the notes' light, added */
        float lv = pn_level(i); if (lv <= 0.01f) continue;
        if (pn[i].x >= cols || pn[i].y >= rows) continue;
        uint16_t c = LAYER_C[pn[i].layer]; uint8_t *a = acc[pn[i].y][pn[i].x];
        int r = (int)((float)((c >> 11) * 255 / 31) * lv), g = (int)((float)(((c >> 5) & 63) * 255 / 63) * lv), b = (int)((float)((c & 31) * 255 / 31) * lv);
        a[0] = (uint8_t)clamp(a[0] + r, 0, 255); a[1] = (uint8_t)clamp(a[1] + g, 0, 255); a[2] = (uint8_t)clamp(a[2] + b, 0, 255);
    }
    for (int y = 0; y < rows; y++) {
        int py = y0 + y * cell + cell / 2;
        if (py + cell < -cv_oy || py - cell > -cv_oy + (int)cv_h) continue;
        for (int x = 0; x < cols; x++) {
            float ml = mp_level(&mp[y][x], x, y);
            uint16_t mc = mp[y][x].col;
            int r = acc[y][x][0], g = acc[y][x][1], b = acc[y][x][2];
            if (ml > 0.01f && mc) { r += (int)((float)((mc >> 11) * 255 / 31) * ml); g += (int)((float)(((mc >> 5) & 63) * 255 / 63) * ml); b += (int)((float)((mc & 31) * 255 / 31) * ml); }
            if (r + g + b <= 45) continue;           /* unlit (or nearly out): nothing there */
            disc(x * cell + cell / 2, py, rad, RGB(clamp(r, 0, 255), clamp(g, 0, 255), clamp(b, 0, 255)));
        }
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
    for (int top = 0; top < VIEW_H; top += 60) {
        int h = VIEW_H - top < 60 ? VIEW_H - top : 60;
        cv_begin(240, (uint32_t)h, C_BLACK);
        cv_oy = -top; cv_y0 = 0; cv_y1 = h;
        draw_pegs();
        if (!set.playing) { int w = text_w(&FONT_M, "STOPPED"); cv_text(120 - w / 2, (panel ? PANEL_TOP : VIEW_H) / 2 - 12, &FONT_M, "STOPPED", C_SUB); }
        if (panel && top + h > PANEL_TOP) { cv_rect(0, PANEL_TOP, 240, PANEL_H, C_BLACK); draw_panel_rows(PANEL_TOP); }
        if (now < ui.list_until) draw_list();
        else if (now < ui.pop_until) draw_popup();
        cv_blit(0, (uint32_t)(VIEW_Y + top));
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
            "RECORDING THE FM-1 OVER USB" };
        cv_text(6, y0 + 26, &FONT_XS, HINT[pg], dim(C_SUB, 0.8f));
    }
    /* four columns: label, value, bar */
    for (int k = 0; k < 4; k++) {
        int cx = 60 * k + 30, y = y0 + 50;
        int hot = focus && k == ui.last_knob;
        const char *nm = knob_of(pg, k)->name;
        cv_text(cx - text_w(&FONT_XS, nm) / 2, y, &FONT_XS, nm, hot ? pc : C_SUB);
        char v[24]; knob_text(pg, k, v);
        const felucca_font_t *f = text_w(&FONT_M, v) <= 56 ? &FONT_M : text_w(&FONT_B, v) <= 58 ? &FONT_B : &FONT_XS;
        if (text_w(f, v) > 58) v[9] = 0;
        cv_text(cx - text_w(f, v) / 2, y + (f == &FONT_M ? 14 : 18), f, v, hot ? C_WHITE : C_TXT);
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
#define SPLASH_MS 1800u
static void splash_title(int32_t off, uint16_t c)
{
    int w = text_w(&FONT_L, "AFTER"), w2 = text_w(&FONT_L, "IMAGES");
    cv_text(120 - w / 2 + off, 4, &FONT_L, "AFTER", c);
    cv_text(120 - w2 / 2 - off, 40, &FONT_L, "IMAGES", c);
}

void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    cv_begin(240, 60, C_BLACK); cv_oy = 0; cv_y0 = 0; cv_y1 = 60;
    cv_rect(0, 0, 240, 60, C_BLACK);
    cv_blit(0, 70);
    cv_begin(240, 60, C_BLACK);
    splash_title(0, C_WHITE);
    cv_blit(0, 76);
}

static void splash_frame(uint32_t ms)
{
    float t = (float)ms / 1000.0f;
    cv_begin(240, 60, C_BLACK);
    for (int g = 3; g >= 1; g--) {               /* the ghosts trail behind, drifting apart */
        float k = 0.15f * (float)(4 - g) * (1.0f - t / 1.8f);
        splash_title((int32_t)((float)g * 6.0f * t), dim(LAYER_C[g % NUM_LAYERS], k > 0 ? k : 0));
    }
    splash_title(0, C_WHITE);
    cv_blit(0, 76);
}

static void splash_end(void)
{
    ui.splash_on = 0;
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
        pegs_step(dt);
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
    /* the piece and settings kept: never while the music plays (erasing the flash stops the audio for tens of ms, a
     * drop), so when it has stopped and faded out (PLAY) */
    if (autosave_at && (int32_t)(now - autosave_at) >= 0 && !set.playing && now - stopped_at >= 2500u) {
        autosave_at = 0;
        store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
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

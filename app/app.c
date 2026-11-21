/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: a generative synth for the M-VAVE FM-1. Four layers (GROUND, BODY, LIGHT, DUST), each a sequencer
 * driving a sound engine in its own band; REC makes a new piece. Part of fm1-base's unity build (device/unity.c,
 * host/plat_host.c): gfx.c and announce.c are in scope.
 *
 * Controls
 *   K1-K4              at HOME the layers' levels (shown while they turn); HOME held + K1-K4 the macros (MOOD,
 *                      ACTIVITY, EVOLVE, DRIFT); on the other pages, their four macros
 *   HOME EDIT SEQ ENV FX LFO GLO ARP   the pages: HOME, SOUND, SEQUENCE, MIX, EFFECTS, MOTION, SCALE, MIDI; FX
 *                      again: TAPE (TAPE, WOW, HISS, AGE)
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
#define SET_MAGIC 0x41465431u            /* "AFT1" */
#define NSAVED 16
typedef struct {
    uint32_t magic;
    uint8_t slot;                        /* the saved piece being played (255: none) */
    uint8_t nsaved, midi_on, midi_ch, midi_keys, midi_clock, playing, rsv;
    Piece cur;
    Piece saved[NSAVED];
} settings_t;
static settings_t set;
static uint8_t set_dirty;
static int store_ok = 1;
static uint32_t autosave_at;
#define AUTOSAVE_MS 8000u

static void settings_defaults(void)
{
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC; set.slot = 255;
    set.midi_on = 1; set.midi_ch = 0; set.midi_keys = 4; set.midi_clock = 0;
    piece_default(&set.cur);
}

static int settings_ok(void)
{
    if (set.magic != SET_MAGIC || set.nsaved > NSAVED || (set.slot != 255 && set.slot >= set.nsaved)) return 0;
    if (set.midi_ch > 12 || set.midi_keys > 15 || set.midi_on > 1 || set.midi_clock > 1) return 0;
    if (!piece_ok(&set.cur)) return 0;
    for (int i = 0; i < set.nsaved; i++) if (!piece_ok(&set.saved[i])) return 0;
    return 1;
}

static void settings_load(void)
{
    int n = plat_store_load(OBJ_SETTINGS, &set, sizeof set);
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

/* MIDI in: notes play the chosen layer's sound, in the scale; CC 20-23 HOME, 24-27 EFFECTS, 28-31 MOTION */
static void midi_in(void)
{
    uint32_t pkt; int port;
    const Scale *sc = &SCALES[set.cur.scale % NUM_SCALES];
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu;
        if (st == 0x90u || st == 0x80u) {
            int note = deg_note(sc, set.cur.key, note_deg(sc, set.cur.key, (int)d1));
            engine_key(&eng, st == 0x90u && d2 > 0, note, (int)(d2 ? d2 : 64));
        } else if (st == 0xB0u) {
            uint8_t *g[16] = { &set.cur.mood, &set.cur.motion, &set.cur.evolve, &set.cur.weird, &set.cur.reverb, &set.cur.echo,
                               &set.cur.grain, &set.cur.shimmer, &set.cur.link, &set.cur.warp, &set.cur.drift, &set.cur.chaos,
                               &set.cur.tape, &set.cur.wow, &set.cur.hiss, &set.cur.age };
            if (d1 >= 20u && d1 < 36u) cc_set(g[d1 - 20u], d2);
        }
    }
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
enum { PG_HOME, PG_SOUND, PG_SEQ, PG_MIX, PG_FX, PG_MOTION, PG_SCALE, PG_MIDI, PG_TAPE, NPAGES };
static const char *const PAGE_NAME[NPAGES] = { "HOME", "SOUND", "SEQUENCE", "MIX", "EFFECTS", "MOTION", "SCALE", "MIDI", "TAPE" };
static const uint8_t PAGE_BTN[NPAGES] = { B_HOME, B_EDIT, B_SEQ, B_ENV, B_FX, B_LFO, B_GLO, B_ARP, B_FX };   /* FX again: TAPE */
enum { K_PCT, K_ENGINE, K_SEQ, K_RATE, K_OCT, K_KEY, K_SCALE, K_CHORD, K_TEMPO, K_ONOFF, K_CH };
typedef struct { const char *name; uint8_t kind, where, off; } Knob;   /* where: 0 the piece, 1 the layer, 2 the settings */
#define PO(f) (uint8_t)__builtin_offsetof(Piece, f)
#define LO(f) (uint8_t)__builtin_offsetof(LayerP, f)
#define SO(f) (uint8_t)__builtin_offsetof(settings_t, f)
static const Knob PAGES[NPAGES][4] = {
    { { "MOOD", K_PCT, 0, PO(mood) }, { "ACTIVITY", K_PCT, 0, PO(motion) }, { "EVOLVE", K_PCT, 0, PO(evolve) }, { "DRIFT", K_PCT, 0, PO(weird) } },
    { { "ENGINE", K_ENGINE, 1, LO(engine) }, { "TONE", K_PCT, 1, LO(tone) }, { "WEIGHT", K_PCT, 1, LO(weight) }, { "DRIFT", K_PCT, 1, LO(weird) } },
    { { "TYPE", K_SEQ, 1, LO(seq) }, { "DENSITY", K_PCT, 1, LO(density) }, { "RATE", K_RATE, 1, LO(rate) }, { "MUTATE", K_PCT, 1, LO(mutate) } },
    { { "LEVEL", K_PCT, 1, LO(level) }, { "ATTACK", K_PCT, 1, LO(attack) }, { "RELEASE", K_PCT, 1, LO(release) }, { "OCTAVE", K_OCT, 1, LO(octave) } },
    { { "REVERB", K_PCT, 0, PO(reverb) }, { "ECHO", K_PCT, 0, PO(echo) }, { "GRAIN", K_PCT, 0, PO(grain) }, { "SHIMMER", K_PCT, 0, PO(shimmer) } },
    { { "LINK", K_PCT, 0, PO(link) }, { "WARP", K_PCT, 0, PO(warp) }, { "FLOW", K_PCT, 0, PO(drift) }, { "CHAOS", K_PCT, 0, PO(chaos) } },
    { { "KEY", K_KEY, 0, PO(key) }, { "SCALE", K_SCALE, 0, PO(scale) }, { "CHORDS", K_CHORD, 0, PO(chord) }, { "TEMPO", K_TEMPO, 0, PO(tempo) } },
    { { "MIDI OUT", K_ONOFF, 2, SO(midi_on) }, { "CHANNEL", K_CH, 2, SO(midi_ch) }, { "KEYS CH", K_CH, 2, SO(midi_keys) }, { "CLOCK", K_ONOFF, 2, SO(midi_clock) } },
    { { "TAPE", K_PCT, 0, PO(tape) }, { "WOW", K_PCT, 0, PO(wow) }, { "HISS", K_PCT, 0, PO(hiss) }, { "AGE", K_PCT, 0, PO(age) } },
};
static int page_has_layer(int pg) { return pg == PG_SOUND || pg == PG_SEQ || pg == PG_MIX; }

#define C_TXT RGB(232, 232, 232)
#define C_SUB RGB(150, 150, 160)
#define C_RULE RGB(40, 40, 48)
#define C_WELL RGB(30, 30, 36)
#define C_RED RGB(255, 90, 78)
/* each layer its colour, everywhere: on the view and on its pages. GROUND red, BODY blue, LIGHT green, DUST white */
static const uint16_t LAYER_C[NUM_LAYERS + 1] = { RGB(255, 72, 64), RGB(70, 140, 255), RGB(70, 230, 110), RGB(240, 240, 240), RGB(255, 255, 255) };

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

static uint8_t *knob_ptr(int pg, int k)
{
    const Knob *kn = &PAGES[pg][k];
    if (kn->where == 1) return (uint8_t *)&set.cur.l[ui.sel] + kn->off;
    if (kn->where == 2) return (uint8_t *)&set + kn->off;
    return (uint8_t *)&set.cur + kn->off;
}

static void knob_text(int pg, int k, char *s)
{
    const Knob *kn = &PAGES[pg][k]; uint8_t v = *knob_ptr(pg, k);
    s[0] = 0;
    switch (kn->kind) {
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
    const Knob *kn = &PAGES[pg][k]; uint8_t v = *knob_ptr(pg, k);
    switch (kn->kind) {
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

static void new_piece(int all)
{
    char nm[32];
    hist_push();
    uint32_t seed = (set.cur.seed * 1664525u + 1013904223u) ^ plat_ms() * 2654435761u ^ plat_ticks24();
    piece_roll(&set.cur, seed, all);
    set.slot = 255;
    changed();
    piece_name(seed, nm);
    pop(all ? "A NEW WORLD" : "NEW PIECE", nm, LAYER_C[(seed >> 3) % NUM_LAYERS]);
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

static void play_toggle(void)
{
    set.playing = (uint8_t)!set.playing;
    eng.playing = set.playing;
    if (set.midi_on && set.midi_clock) midi_send(0x0Fu | (set.playing ? 0xFAu : 0xFCu) << 8);
    if (!set.playing) engine_all_off(&eng);
    pop(set.playing ? "PLAY" : "STOP", "", C_TXT);
}

static void info(void)
{
    char s[28] = "CPU ", n[8];
    int_text((int32_t)plat_cpu_pct(), n); text_cat(s, n); text_cat(s, "%");
    pop(APP_NAME, s, store_ok ? C_TXT : C_RED);
    text_cat(ui.pop_3, two_cores && plat_cpu2_on() ? "2 CORES" : "1 CORE");
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
    const Knob *kn = &PAGES[ui.page][k]; uint8_t *p = knob_ptr(ui.page, k);
    int v = kn->kind == K_OCT ? (int8_t)*p : *p, lo = 0, hi = 100, step = 2, wrap = 0;
    switch (kn->kind) {
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
    if (kn->where == 2) { store_ok = 1; autosave_at = plat_ms() + AUTOSAVE_MS; }
    else changed();
    if (kn->kind == K_ONOFF && kn->off == SO(midi_on) && !v) engine_all_off(&eng);
    ui.last_knob = k; ui.knob_at = plat_ms(); ui.pop_until = 0;
    if (ann_on()) { char s[24]; knob_text(ui.page, k, s); say_text(kn->name, s); }
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
            engine_key(&eng, 1, note, v ? v : 100);
        }
    }
    for (int j = 0; j < 11; j++) {
        int kk = BK[j]; uint32_t b = 1u << kk;
        if (kk >= NKEYS) continue;
        if ((keys & b) && !(ui.keys & b)) {
            int d[5], n = chord_degs(set.cur.chord, j, d), v = touch_vel((uint32_t)kk);
            if (ui.sel == L_GROUND) base += 12;      /* (chords down there would be mud) */
            for (int c = 0; c < n; c++) { int note = deg_note(sc, base, d[c]); ui.key_notes[kk][c] = (uint8_t)note; engine_key(&eng, 1, note, (v ? v : 100) * 3 / 4); }
            ui.last_note = ui.key_notes[kk][0]; ui.last_chord = n;
            ui.key_n[kk] = (uint8_t)n;
            if (ui.sel == L_GROUND) base -= 12;
        }
    }
    for (int k = 0; k < NKEYS; k++)
        if (!(keys >> k & 1u) && (ui.keys >> k & 1u)) {
            for (int c = 0; c < ui.key_n[k]; c++) engine_key(&eng, 0, ui.key_notes[k][c], 0);
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

    for (int pg = 1; pg < NPAGES; pg++) if (pg != PG_FX && pg != PG_TAPE && (down >> PAGE_BTN[pg] & 1u)) page_to(pg);
    if (down >> B_FX & 1u) page_to(ui.page == PG_FX ? PG_TAPE : PG_FX);   /* FX: the effects, again the tape */
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

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ the view: abstract afterimages
 * Each note leaves a shape where its pitch and its place in the stereo field put it, fading slowly: GROUND a wide bar
 * low down, BODY rings, LIGHT a bright point joined to the last by a line (the melody drawn), DUST specks. */
#define HEAD_H 30
#define VIEW_Y HEAD_H
#define VIEW_H (240 - HEAD_H)
#define PANEL_H 110
#define PANEL_TOP (VIEW_H - PANEL_H)     /* the panel, over the view's lower part */
#define NSHAPE 96
typedef struct { uint8_t on, layer; float x, y, age, life, size, vel; float px, py; } shape_t;
static shape_t shp[NSHAPE];
static float last_light[2] = { -1, -1 };

static float note_y(int note) { return (float)VIEW_H - 6.0f - ((float)note - 28.0f) / 72.0f * (float)(VIEW_H - 12); }

static void spawn(const UiNote *n)
{
    int best = 0; float oldest = -1;
    for (int i = 0; i < NSHAPE; i++) {
        if (!shp[i].on) { best = i; oldest = 1e9f; break; }
        float a = shp[i].age / shp[i].life; if (a > oldest) { oldest = a; best = i; }
    }
    shape_t *s = &shp[best];
    float vel = (float)n->vel / 127.0f;
    s->on = 1; s->layer = n->layer; s->age = 0; s->vel = vel;
    s->x = 120.0f + (float)n->pan * 1.1f + (float)((n->note * 37) % 21 - 10);
    s->y = note_y(n->note);
    switch (n->layer) {
    case L_GROUND: s->life = 5.0f; s->size = 30.0f + 70.0f * vel; break;
    case L_BODY: s->life = 4.0f; s->size = 10.0f + 26.0f * vel; break;
    case L_LIGHT:
        s->life = 4.0f; s->size = 2.0f;
        s->px = last_light[0]; s->py = last_light[1];
        last_light[0] = s->x; last_light[1] = s->y;
        break;
    default: s->life = 2.6f; s->size = 1.0f; s->x += (float)((n->note * 53) % 101 - 50); break;
    }
}

static uint16_t dim(uint16_t c, float k)
{
    if (k >= 1) return c;
    if (k <= 0) return C_BLACK;
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)(((uint32_t)((float)r * k) << 11) | ((uint32_t)((float)g * k) << 5) | (uint32_t)((float)b * k));
}

static void shapes_step(float dt)
{
    uint32_t head = eng.uiq_head;
    if (head - ui.uiq_tail > UI_Q) ui.uiq_tail = head - UI_Q;
    while (ui.uiq_tail != head) { UiNote n = eng.uiq[ui.uiq_tail % UI_Q]; ui.uiq_tail++; spawn(&n); }
    for (int i = 0; i < NSHAPE; i++) {
        shape_t *s = &shp[i];
        if (!s->on) continue;
        s->age += dt;
        if (s->age >= s->life) { s->on = 0; continue; }
        if (s->layer == L_DUST) { s->y -= 6.0f * dt; s->x += fm_sinf(s->age * 3.0f + (float)i) * 0.3f; }
        if (s->layer == L_GROUND) s->y += 1.5f * dt;
    }
}

/* Behind everything, a Julia set, drawn as points by inverse iteration (z -> +-sqrt(z - c)), its c wandering with the
 * music: round the edge of the Mandelbrot set's heart as the beats go by, pulled in and out as BODY swells, turned
 * by the chord's root. Each point coloured by the layer whose band its height falls in, as bright as that layer
 * sounds. The last few c's leave fainter copies behind: the set's own afterimages. */
#define JPTS 1100
#define JGHOST 3
static int16_t jx[JGHOST][JPTS], jy[JGHOST][JPTS];
static uint8_t jc[JGHOST][JPTS];
static int jcur;
static uint32_t jrng = 0x2545F491u;

static void julia_step(float dt)
{
    const Scale *sc = &SCALES[set.cur.scale % NUM_SCALES];
    int n = sc->n, root = ((eng.root % n) + n) % n;
    static float th, rad = 0.72f;
    th += dt * (0.02f + 0.06f * (float)set.cur.motion * 0.01f);
    float aim = 0.70f + 0.12f * eng.L[L_BODY].cv + 0.04f * eng.L[L_GROUND].cv;
    rad += (aim - rad) * clampf(dt * 0.8f, 0.0f, 1.0f);
    float a = th + (float)root / (float)n * 0.35f;
    float cr = rad * fm_sin_turns(a + 0.25f), ci = rad * fm_sin_turns(a);
    jcur = (jcur + 1) % JGHOST;
    float zr = 1.0f, zi = 0.0f;
    float sx = 64.0f + 30.0f * (float)set.cur.weird * 0.01f, sy = sx * 0.85f, cy = (float)VIEW_H * 0.42f;
    for (int i = 0; i < JPTS + 12; i++) {
        float wr = zr - cr, wi = zi - ci;                  /* sqrt(w): r = |w|, then the half angle */
        float m = fm_sqrtf(wr * wr + wi * wi);
        float sr = fm_sqrtf((m + wr) * 0.5f), si = fm_sqrtf(fm_maxf((m - wr) * 0.5f, 0.0f));
        if (wi < 0) si = -si;
        jrng ^= jrng << 13; jrng ^= jrng >> 17; jrng ^= jrng << 5;
        if (jrng & 1u) { sr = -sr; si = -si; }
        zr = sr; zi = si;
        if (i < 12) continue;
        int k = i - 12;
        jx[jcur][k] = (int16_t)(120.0f + zr * sx);
        jy[jcur][k] = (int16_t)(cy + zi * sy);
        jc[jcur][k] = (uint8_t)(zi > 0.35f ? L_GROUND : zi > 0.0f ? L_BODY : zi > -0.35f ? L_LIGHT : L_DUST);
    }
}

static void draw_julia(void)
{
    uint16_t col[JGHOST][NUM_LAYERS];
    for (int g = 0; g < JGHOST; g++)
        for (int l = 0; l < NUM_LAYERS; l++) {
            float age = (float)((jcur - g + JGHOST) % JGHOST);   /* 0 the newest */
            col[g][l] = dim(LAYER_C[l], (0.16f + 0.6f * eng.meter[l]) * (1.0f - 0.3f * age));
        }
    for (int g = JGHOST - 1; g >= 0; g--) {
        int b = (jcur - g + JGHOST) % JGHOST;
        for (int i = 0; i < JPTS; i++) cv_pset(jx[b][i], jy[b][i], col[g][jc[b][i]]);
    }
}

/* a note's fractal: a branch splitting in two, and again, as deep as its loudness (BODY's chords bloom this way) */
static void branch(float x, float y, float len, float ang, int depth, float spread, uint16_t c)
{
    float x2 = x + len * fm_sin_turns(ang), y2 = y - len * fm_sin_turns(ang + 0.25f);
    cv_line((int32_t)x, (int32_t)y, (int32_t)x2, (int32_t)y2, c);
    if (depth <= 0) return;
    uint16_t cc = dim(c, 0.8f);
    branch(x2, y2, len * 0.68f, ang - spread, depth - 1, spread, cc);
    branch(x2, y2, len * 0.68f, ang + spread, depth - 1, spread, cc);
}

static void draw_shapes(void)
{
    draw_julia();
    for (int i = 0; i < NSHAPE; i++) {
        shape_t *s = &shp[i];
        if (!s->on) continue;
        float q = s->age / s->life, k = (1.0f - q) * (1.0f - q) * (0.35f + 0.65f * s->vel);
        uint16_t c = dim(LAYER_C[s->layer], k);
        switch (s->layer) {
        case L_GROUND: {                       /* a bar that widens and sinks */
            float w = s->size * (0.6f + 0.6f * q);
            cv_rect((int32_t)(s->x - w * 0.5f), (int32_t)s->y, (int32_t)w, 2, c);
            cv_rect((int32_t)(s->x - w * 0.3f), (int32_t)s->y + 4, (int32_t)(w * 0.6f), 1, dim(c, 0.5f));
            break;
        }
        case L_BODY: {                         /* a fractal that grows and opens: three branches, splitting */
            int depth = 2 + (s->vel > 0.6f);
            float len = s->size * 0.35f * (0.4f + 0.6f * q), spread = 0.05f + 0.07f * q;
            for (int b = 0; b < 3; b++) branch(s->x, s->y, len, s->vel * 0.3f + (float)b / 3.0f + q * 0.05f, depth, spread, c);
            break;
        }
        case L_LIGHT:                          /* the melody: a point, a line back to the note before */
            if (s->px >= 0) cv_line((int32_t)s->px, (int32_t)s->py, (int32_t)s->x, (int32_t)s->y, dim(c, 0.6f));
            cv_rect((int32_t)s->x - 1, (int32_t)s->y - 1, 3, 3, c);
            if (q < 0.15f) cv_rect((int32_t)s->x - 3, (int32_t)s->y, 7, 1, c);
            break;
        default: {                             /* sparks, drifting up: a cross that shrinks to a point */
            int r = q < 0.3f ? 2 : q < 0.6f ? 1 : 0, x = (int32_t)s->x, y = (int32_t)s->y;
            cv_pset(x, y, c);
            for (int j = 1; j <= r; j++) { uint16_t cc = dim(c, 0.6f); cv_pset(x - j, y, cc); cv_pset(x + j, y, cc); cv_pset(x, y - j, cc); cv_pset(x, y + j, cc); }
            break;
        }
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
        draw_shapes();
        if (!set.playing) { int w = text_w(&FONT_M, "STOPPED"); cv_text(120 - w / 2, (panel ? PANEL_TOP : VIEW_H) / 2 - 12, &FONT_M, "STOPPED", C_SUB); }
        if (panel && top + h > PANEL_TOP) { cv_rect(0, PANEL_TOP, 240, PANEL_H, C_BLACK); draw_panel_rows(PANEL_TOP); }
        if (now < ui.list_until) draw_list();
        else if (now < ui.pop_until) draw_popup();
        cv_blit(0, (uint32_t)(VIEW_Y + top));
    }
}

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
    cv_text(6, y0 + 4, &FONT_B, pg == PG_HOME ? "MACROS" : PAGE_NAME[pg], lay ? pc : C_TXT);
    if (lay) { const char *n = LAYER_NAME[ui.sel]; cv_text(234 - text_w(&FONT_B, n), y0 + 4, &FONT_B, n, pc); }
    /* the knob turned last, big */
    int focus = ui.knob_at && plat_ms() - ui.knob_at < 2500u;
    if (focus) {
        char v[24]; knob_text(pg, ui.last_knob, v);
        const char *nm = PAGES[pg][ui.last_knob].name;
        int x = cv_text(6, y0 + 24, &FONT_S, nm, C_SUB);
        cv_text(x + 10, y0 + 20, &FONT_M, v, pc);
    } else {
        static const char *const HINT[NPAGES] = {
            "WHERE THE MUSIC GOES", "SELECT CHOOSES THE LAYER", "SELECT CHOOSES THE LAYER", "SELECT CHOOSES THE LAYER",
            "THE SPACE AROUND IT  (FX AGAIN: TAPE)", "HOW THE LAYERS MOVE", "EVERYTHING IN THE SCALE", "OUT OVER USB",
            "EVERYTHING THROUGH OLD TAPE" };
        cv_text(6, y0 + 26, &FONT_XS, HINT[pg], dim(C_SUB, 0.8f));
    }
    /* four columns: label, value, bar */
    for (int k = 0; k < 4; k++) {
        int cx = 60 * k + 30, y = y0 + 50;
        int hot = focus && k == ui.last_knob;
        const char *nm = PAGES[pg][k].name;
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
            else { knob_text(ui.page, k, v); text_cat(t, ". "); text_cat(t, PAGES[ui.page][k].name); }
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
    memset(shp, 0, sizeof shp);
    touch_on(1);
    ui.sel = L_LIGHT; eng.key_layer = ui.sel;
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
        shapes_step(dt);
        julia_step(dt);
        draw_view();
    }
    {   /* the load guard: past 85 % of the render's time, fewer notes; back as it eases */
        uint32_t cpu = plat_cpu_pct();
        if (cpu > 85u) eng.gov -= 0.02f; else if (cpu < 70u) eng.gov += 0.002f;
        eng.gov = eng.gov < 0.3f ? 0.3f : eng.gov > 1.0f ? 1.0f : eng.gov;
    }
    if (autosave_at && (int32_t)(now - autosave_at) >= 0) {
        autosave_at = 0;
        store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
    }
    if (now - ui.last_head >= 100u) {            /* the header: its words, and the layers' lights */
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
    else if (!strcmp(what, "lastchord")) *got = ui.last_chord;
    else if (!strncmp(what, "seq", 3)) *got = set.cur.l[atoi(what + 3) & 3].seq;
    else if (!strncmp(what, "engine", 6)) *got = set.cur.l[atoi(what + 6) & 3].engine;
    else if (!strncmp(what, "level", 5)) *got = set.cur.l[atoi(what + 5) & 3].level;
    else if (!strncmp(what, "events", 6)) *got = eng.events[atoi(what + 6) & 3];
    else return 0;
    return 1;
}
#endif

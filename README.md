# After Images

A generative synth for the M-VAVE FM-1. Press REC and it makes a new piece of ambient music that keeps evolving:
four layers, each its own sequencer driving its own sound in its own part of the spectrum, listening to each other.
Musical (everything is locked to the scale), but DRIFT takes it somewhere strange. A slow leveller keeps every piece
at about the same loudness.

By [TheGrizard](https://github.com/TheGrizard). Built on [fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0)
and felucca-bsp. Works on both FM-1 flash chips.

![After Images on the FM-1's screen](docs/screen.png)

## The four layers

| Layer | Colour | Band | Its part |
|---|---|---|---|
| GROUND | red | sub and bass, ~30-200 Hz | the root, its fifth and octave: the floor |
| BODY | blue | low mids, ~150 Hz-1 kHz | the chords (Detroit voicings when MOOD is up) |
| LIGHT | green | mids, ~500 Hz-4 kHz | the melody |
| DUST | white | highs, ~2-12 kHz | the air: specks, chimes, breath |

Each piece gives every layer one of ten **sequencers**, shuffled: EUCLID (Euclidean rings), TURING (a looping shift
register that flips bits), MARKOV (a table of chances), LORENZ (the attractor: a note at each loop's peak), BOUNCE
(bouncing balls), ORBITS (planets with different years), LIFE (a cellular automaton), CHIMES (wind chimes in gusts),
PHASE (one phrase on two players drifting apart, Steve Reich's way), DRUNK (a Brownian walk) and MELODY (a long
melody the early-90s Warp way: a motif, its answer, a turn, the motif coming home; looped, changing slowly; LIGHT sings
one in half the pieces). And one of ten
**engines** (twelve): DRIFT SAW, FM, DX (four-operator FM, four algorithms, each modulator fading at its own pace),
STRING, BLEEP, GLASS, BREATH, VOICE, FOLD, SUB, STRINGS (an 80s string machine) and E.PIANO (an 80s FM electric piano). Every piece has at least one
heavy layer and one soft one. A piece is a seed: the same piece always starts the same way.

It is patched like a modular. Every sequencer has a second output, a CV, that shapes each note's sound: the
attractor's position, the Turing register's upper bits, the ball's speed, the automaton's population. Accents open
the filter and add weight. Each layer's activity is patched into another layer's filter (which into which is the
piece's own), and a random voltage, stepping every few beats, moves each one (FLOW).

LINK makes the layers answer each other: a note in one nudges another's system (a ball dropped, a bit flipped, the
attractor kicked) and now and then moves the harmony on. A heavy GROUND note ducks BODY and DUST, so the low end
stays clear.

## Playing it

| Control | What it does |
|---|---|
| K1 to K4 | at HOME, the four layers' levels: they rise up from the bottom of the screen while you turn; on the other pages, the page's four macros (the one you turn shows big) |
| HOME held + K1 to K4 | the macros that steer where the music goes: MOOD (dark to bright, triads to ninths), ACTIVITY, EVOLVE (how much the form changes), DRIFT (how far the sound strays from the harmonics into the strange) |
| EDIT | SOUND for the chosen layer: ENGINE, TONE, WEIGHT (soft to heavy), DRIFT (the engine's own strangeness) |
| SEQ | SEQUENCE for the chosen layer: TYPE, DENSITY, RATE, MUTATE |
| ENV | MIX for the chosen layer: LEVEL, ATTACK, RELEASE, OCTAVE |
| ENV again | VOICE for the chosen layer: F.GLIDE (the filter glides after each new note; the pitch never slides), LEGATO (notes join, not started again), SWEEP (the filter opening over each note), RESO |
| FX | SENDS for the chosen layer: how much of it goes to the REVERB, the ECHO, the GRAIN, and how much DIRT it gets (so GROUND can go only to the reverb while LIGHT is folded and echoed) |
| FX again | EFFECTS, shared by the layers: REVERB (to 12 o'clock the room, small to vast; past it the shimmer, an octave up, and by full an octave down too), ECHO (to 12 o'clock a tempo-locked echo with a tape wobble; past it a reversed, smeared wash), GRAIN (a cloud; right up, FREEZE), DIRT (the character: FOLD, CRUSH, then NOISE) |
| LFO | MOTION: LINK, WARP (swing and phrases that speed up and slow down), FLOW (slow random movement), CHAOS (ratchets, skips) |
| GLO | SCALE: KEY, SCALE (20 of them), CHORDS (the black keys'), TEMPO |
| GLO again | SCREEN: PALETTE (nine), FRACTAL (JULIA, FERN, DRAGON, POLYGON, ATTRACTOR, TREE), FULL (the whole screen at HOME, every pixel a fractal zooming endlessly into itself, steered along the set's edge: JULIA SEA, MANDELBROT, SHIP, TRICORN, CUBIC, SPIRAL), GLOW. REC deals a new palette and a new fractal each time, none twice until all have come |
| ARP | (pages with more than one: a dot each by the page's name) MIDI OUT: OUT on or off, CHANNEL (the layers on four in a row, 1-4 at first), KEYS CH, CLOCK out |
| ARP again | MIDI IN: IN CH (the chosen layer's notes: any channel, or one), LAYERS (notes on the layers' four channels play those layers), CLOCK (follow MIDI clock's tempo, start and stop), CC |
| ARP a third time | USB AUDIO: RECORD (at MASTER's level or full), COMPUTER (the computer's audio through the FM-1), STEMS (the four layers on USB audio channels 3-10, beside the mix on 1-2) |
| SELECT, or tap SEL | the layer the SOUND, SEQUENCE and MIX pages and the keys play |
| PRESET | the chosen layer's engine |
| REC | a new piece (your key, scale and tempo kept), featuring one effect: shimmer, reverse echo, a grain cloud, frozen grains, folded, crushed or wide space, dealt so none comes twice in a row |
| REC held | a new world: a new key, scale and tempo too |
| SEL held + REC | the chosen layer anew: a new sequencer and sound |
| OCT- / OCT+ | back and forward through the pieces REC replaced |
| SAVE | keep the piece (up to 16); ALGORITHM steps through them |
| PLAY | stop and start (it fades) |
| HOME | back home from any page; tapped at home, the processor's load and whether saving works |
| white keys | the scale's notes, up from the key, on the chosen layer's sound: no wrong notes |
| black keys | the scale's chords: I to VII, then I to IV an octave up (CHORDS sets triads, sevenths, ninths, sus, spread, fifths) |

The home screen is abstract: a fractal behind everything (SCREEN chooses which, and FULL lets it fill the screen),
always moving by itself and moved by the music, coloured by the layers as they sound;
over it each note leaves a fading afterimage in its layer's colour: GROUND a bar, BODY a branching fractal, LIGHT the
melody drawn as a line, DUST sparks.

Each piece gets a name from its seed: two words, one, a made-up one, or one chopped up and numbered (FLS 74,
QURIDIAN, ZHEIGRO 17).

The piece playing is kept by itself a few seconds after your last change, so the FM-1 comes back where you left it.

MIDI out (USB): each layer's notes on its own channel (1-4 by default), the keys on another (5), clock if you want it.
MIDI in (USB or the TRS jack): notes on the layers' channels play those layers, others the chosen layer, all put into
the scale; CC 20-23 are HOME's macros, 24-27 EFFECTS', 28-31 MOTION's; clock, start and stop followed with CLOCK on.

Every note lands somewhere new in the stereo field and wanders as it sounds: DRIFT and CHAOS throw them wider.

## Building it

    git clone --recursive https://github.com/TheGrizard/After-Images
    cd After-Images
    base/tools/setup_toolchain.sh        # JieLi's compiler and the AC79 SDK's three files, checked against a lock
    ./build.sh                           # app/build/after-images.fwsc

Install it with the FM-1 plugged in by USB (needs `pip install mido python-rtmidi`; on Windows, Python 3.12):

    python3 base/tools/fm1_install.py app/build/after-images.fwsc

Or from a release: download the `.fwsc` and run `py -V:3.12 fm1_install.py after-images-X.Y.fwsc`, or use the
[web installer](https://TheGrizard.github.io/After-Images/install/).

### On the computer first

    ./build.sh --host
    tests/run_tests.sh                   # the simulator's checks: pages, REC, saving, the keys, the screen reader
    make -C app/tools && app/tools/render 1234 120 piece.wav all    # a piece to a WAV, without the screen

### Releases

Pushing a tag such as `v0.1` builds the release on GitHub (`.github/workflows/release.yml`, the FM-1 builder shared
by fm1-base's firmwares), runs the tests and signs the packages.

## Inspiration

Cyma Forma's RND (one button, four layers, a new idea each press), Mutable Instruments' modules, and Autechre's way
of working: systems of rules rather than dice, layers that trigger each other, rhythm without obvious anchors, and
emotional chords under the strangeness. No code from any of them is in here.

## Licence

MPL-2.0. The screen's font is DejaVu Sans (`app/fonts`, Bitstream Vera's licence: `app/fonts/LICENSE-DejaVu.txt`). See fm1-base's LICENSING.md for the parts it is built from. M-VAVE and FM-1 are trademarks of their owners;
After Images is not affiliated with M-VAVE.

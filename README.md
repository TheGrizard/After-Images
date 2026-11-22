# After Images

**A generative ambient composer for the M-VAVE FM-1.** Press REC and it writes a new piece of music: four layers in
four parts of the spectrum, each with its own sequencer and sound, listening and answering each other, always in key,
always evolving. Press REC again and the next piece follows on from the last, round the circle of fifths.

Free and open source (MPL-2.0). By [TheGrizard](https://github.com/TheGrizard), built on
[fm1-base](https://github.com/charlesvestal/fm1-base). Works on both FM-1 flash chips.

![After Images on the FM-1's screen](docs/screen.png)

**[Install it in your browser](https://TheGrizard.github.io/After-Images/install/)** ·
[Try it in the emulator](https://TheGrizard.github.io/After-Images/emu/) ·
[Releases](https://github.com/TheGrizard/After-Images/releases)

## What makes it different

Most FM-1 firmwares are instruments you play or program. After Images composes.

- **One button, a whole piece.** REC deals a key, a tempo, four sounds, four sequencers, a featured effect, a colour
  palette and a name. The same piece always starts the same way, and OCT- and OCT+ step back and forward through them.
- **Pieces that lead on from each other.** Each new key is a fifth up or down, the relative major or minor, or another
  mode on the same root, mostly minor and consonant. Hold REC to stay in key.
- **Layers with roles.** GROUND (red, the bass), BODY (blue, the chords), LIGHT (green, the melody) and DUST (white,
  the air), each kept to its own part of the spectrum, with a heavy and a soft one in every piece.
- **Systems, not dice.** Eleven sequencers (Euclidean rings, a Turing machine, Markov chains, the Lorenz attractor,
  bouncing balls, orbits, Conway's Life, wind chimes, Reich-style phasing, a drunk walk and long early-90s Warp
  melodies), patched together like a modular: one layer's notes nudge another's system.
- **An ambient mixer.** DRIFT fades the layers in and out, walks them round the stereo field and morphs each one's
  effects and character over half-minutes, so the piece never settles.
- **Three visuals that play along.** MATRIX (a pegboard of light breeding colour mixes), RORSCHACH (a folded inkblot
  swelling to the beat) and MOSH (blocks and lines breeding and moulding where the music sits in the stereo field).
- **Plays with your other gear.** It follows MIDI clock to the beat and the bar (an Ableton Move, say), sends each
  layer on its own MIDI channel, and records over USB audio.

## Quick start

1. **Install.** Open the [web installer](https://TheGrizard.github.io/After-Images/install/) in Chrome or Edge on a
   computer, plug in the FM-1 by USB and follow the page. (Or download the `.fwsc` from
   [Releases](https://github.com/TheGrizard/After-Images/releases) and use fm1-base's command-line installer,
   `tools/fm1_install.py`, which needs mido and python-rtmidi: `py -V:3.12 fm1_install.py after-images-X.Y.fwsc` on
   Windows, `python3 fm1_install.py ...` elsewhere.)
2. **Listen.** It starts playing. Turn **K1 to K4** to set the four layers' levels.
3. **Press REC** for a new piece. Hold it for a new piece in the same key.
4. **Hold HOME and turn K1 to K4** for the macros that steer it: MOOD, ACTIVITY, EVOLVE and DRIFT.
5. **Play the keys.** White keys play the scale, black keys its chords, on the chosen layer's sound (SEL picks the layer).
6. **SAVE** keeps a piece you love; ALGORITHM steps through your saved ones.
7. **Stop with PLAY before switching off**, and wait a few seconds: it then keeps the piece and your settings.

## Controls

### Everywhere

| Control | What it does |
|---|---|
| K1 to K4 | at HOME the layers' levels (shown while you turn); elsewhere the page's four settings |
| HOME held + K1 to K4 | the macros: **MOOD** (a tilt round 12 o'clock: left darker with more sub, right brighter with GROUND the only bass; triads to sevenths to ninths), **ACTIVITY**, **EVOLVE** (how much the form changes), **DRIFT** (the ambient mixer: 0 keeps everything as set) |
| REC | a new piece, in a key that follows on from the last |
| REC held | a new piece in the same key and scale |
| SEL held + REC | the chosen layer anew: a new sequencer and sound |
| OCT- / OCT+ | back and forward through the pieces REC replaced |
| SAVE / ALGORITHM | keep the piece (up to 16) / step through the saved ones |
| PLAY | stop (it fades) and start |
| SELECT, or tap SEL | the layer the SOUND, SEQUENCE, MIX, VOICE and SENDS pages, and the keys, play |
| PRESET | the chosen layer's engine |
| white keys | the scale's notes, through the chosen layer's sound: no wrong notes |
| black keys | the scale's chords, I to VII then I to IV an octave up |
| HOME (tapped at home) | the processor's load, audio drops since power-on, and whether saving works |

### The pages

Press a page's button again for its next page; dots by the page's name show how many it has.

| Button | Page | K1 · K2 · K3 · K4 |
|---|---|---|
| EDIT | SOUND (chosen layer) | ENGINE · TONE · WEIGHT · ENTROPY |
| SEQ | SEQUENCE (chosen layer) | TYPE · DENSITY · RATE · MUTATE |
| ENV | MIX (chosen layer) | LEVEL · ATTACK · RELEASE · OCTAVE |
| ENV again | VOICE (chosen layer) | F.GLIDE · LEGATO · SWEEP · RESO |
| FX | SENDS (chosen layer) | REVERB · ECHO · GRAIN · DIRT |
| FX again | EFFECTS (shared) | REVERB · ECHO · GRAIN · DIRT |
| LFO | MOTION | LINK · WARP · FLOW · CHAOS |
| LFO again | WARBLE | WARBLE · FLUTTER · DETUNE · CHORUS |
| GLO | SCALE | KEY · SCALE · CHORDS · TEMPO |
| GLO again | VISUAL | VISUAL · · SHAPES (MOSH's) · |
| GLO a third time | SCREEN | the chosen visual's four (below) |
| ARP | MIDI OUT | OUT · CHANNEL · KEYS CH · CLOCK |
| ARP again | MIDI IN | IN CH · LAYERS · CLOCK · CC |
| ARP a third time | USB AUDIO | RECORD · COMPUTER |

### What the settings do

- **TONE** is a DJ's filter: 12 o'clock open, left a low-pass sweeping down, right a high-pass sweeping up.
- **WEIGHT** goes from soft to heavy.
- **ENTROPY:** to halfway, the engine's own strangeness. Past it, synthesis turns on itself: a chaos oscillator at each
  note's pitch (FM feedback, PWM or pulse-driven FM), STRING going slack and sagging, GLASS's partials drifting apart,
  E.PIANO's tines bending, VOICE babbling.
- **F.GLIDE** glides the filter after each new note (the pitch never slides). **SWEEP** opens a filter over each
  note, on every engine.
- **Sends and effects.** Each layer sends its own amount to the shared effects, so GROUND can go only to the reverb
  while LIGHT is fuzzed and echoed. **REVERB:** to 12 o'clock a room, small to vast; past it a shimmer an octave up,
  and by full an octave down too. **ECHO:** a tempo-locked tape echo; past 12 o'clock a reversed, smeared wash.
  **GRAIN:** a cloud; right up it freezes. **DIRT:** tape pushed hard, from warm saturation to fuzz to breakup with
  hiss, sputter and dropouts. A send always does something, even with the shared amount at 0.
- **MOTION:** LINK (layers answering each other), WARP (swing, phrases speeding and slowing), FLOW (slow random
  movement), CHAOS (ratchets and skips).
- **WARBLE** puts Boards of Canada's worn tape in the synth itself: the pitch wandering, a faster flutter, everything
  a touch flat, and the Juno's chorus.

The knobs reach notes already sounding: turn TONE, WEIGHT, ENTROPY, RELEASE, SWEEP or RESO and held notes follow.

## The sounds

Twenty-five engines: DRIFT SAW, FM, DX (four-operator FM), STRING, BLEEP, GLASS, BREATH, VOICE, FOLD, SUB, STRINGS (an
80s string machine), E.PIANO (80s FM), TRIANGLE, SINE, SQUARE, CZ (phase distortion), SYNC, SHAPES (unusual
waveforms) and BOARDS (a saw and square drifting apart through a slowly opening dark filter) and REESE (two saws beating
slowly against each other, driven into a deep low-pass with a sub under it, legato with a long tail), ORGAN (drawbars and a rotary speaker's swirl), BOWL (a singing bowl, its partials
beating), MALLET (marimba, vibes or kalimba), WAVES (a wavetable scanned dark to bright) and CHOIR (three voices, ooh to
aah).

Each REC also features one effect: shimmer, reverse echo, a grain cloud, frozen grains, tape saturation, tape fuzz,
wide space or HAZE (warble, chorus, flat and dark), never the same twice in a row. A slow leveller keeps every piece
at about the same loudness.

## The visuals

Choose on the VISUAL page (GLO twice); set them on SCREEN, the page after (GLO three times). REC deals a new colour palette each time,
and the four layers always keep four clearly different hues.

- **MATRIX:** a pegboard. Each note lights the peg at its pitch and stereo position, mixes breed between lit colours,
  the board fills and old ones fade. SCREEN: COUNT, SIZE, BREED, FADE.
- **RORSCHACH:** a folded inkblot. Each layer's ink swells with the beat and as it sounds, notes drop blots, the colours
  bleed together and voids carve black through. SCREEN: SIZE, GAPS, MIX, FADE.
- **MOSH:** squares, rectangles and lines (SHAPES on the VISUAL page: BLOCKS, all three; LINES, mostly lines). Each
  note lights a block where it sits in the stereo field, mixes breed beside them, overlaps mix and the blocks slowly
  stretch and shrink. SCREEN: SIZE (the zoom), BREED, MOULD, FADE.

## MIDI and sync

- **MIDI out** (USB): each layer on its own channel (1 to 4 by default), the keys on another (5), with their chords.
- **MIDI in** (USB, or the FM-1's TRS jack): notes on the layers' channels play those layers, others the chosen layer,
  all put into the scale. CC 20-23 move HOME's macros, 24-27 the effects, 28-31 MOTION.
- **CLOCK** is one setting on both MIDI pages: INT (its own tempo), IN (following) or OUT (sending).
- **Following a clock**, it locks to the leader's beats and bars, not only its tempo, and stays there through a new
  piece or a change of tempo. With an **Ableton Move**: a USB cable from Move's USB-A port to the FM-1, Move's MIDI
  Sync set to Out, and CLOCK at IN.
- **USB audio:** the mix as one stereo pair (RECORD at MASTER's level or full), and the computer's audio through the
  FM-1 if you want it.

## Saving

The piece playing and your settings are kept by themselves a few seconds after you stop with PLAY (never while the
music plays, because writing the flash stops the audio for a moment). So **stop before switching off**. SAVE keeps a
piece at once, whenever you press it, with a soft dip in the sound while it writes.

## Safety

After Images is unofficial and not affiliated with M-VAVE. Installing any custom firmware is at your own risk. The web
installer can put M-VAVE's own firmware back at any time, and you can go back to any earlier After Images from
[Releases](https://github.com/TheGrizard/After-Images/releases). If something sounds wrong, tap HOME to see the load
and the audio drops, and please [open an issue](https://github.com/TheGrizard/After-Images/issues).

## Building it

    git clone --recursive https://github.com/TheGrizard/After-Images
    cd After-Images
    base/tools/setup_toolchain.sh        # JieLi's compiler and the AC79 SDK's three files, checked against a lock
    ./build.sh                           # app/build/after-images.fwsc
    python3 base/tools/fm1_install.py app/build/after-images.fwsc   # needs mido and python-rtmidi

On the computer first:

    ./build.sh --host
    tests/run_tests.sh                   # the simulator's checks: pages, REC, saving, sync, the keys, the screen
    make -C app/tools && app/tools/render 1234 120 piece.wav all    # a piece to a WAV, without the screen

Pushing a tag such as `v1.0` builds, tests and signs the release on GitHub (`.github/workflows/release.yml`) and
updates the installer site.

## Inspiration and thanks

Cyma Forma's RND (one button, four layers, a new idea each press), Mutable Instruments' modules, Steve Roach's slowly
shifting ambient mixes, Boards of Canada's worn tape, and Autechre's way of working: systems of rules rather than dice,
layers that trigger each other, and emotional chords under the strangeness. No code from any of them is in here.

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and felucca-bsp; thank you.

## Licence

MPL-2.0. The screen's font is DejaVu Sans (`app/fonts`, Bitstream Vera's licence: `app/fonts/LICENSE-DejaVu.txt`).
See fm1-base's LICENSING.md for the parts it is built from. M-VAVE and FM-1 are trademarks of their owners; After
Images is not affiliated with M-VAVE.

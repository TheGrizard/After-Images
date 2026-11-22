#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""WAVES' wavetable (app/dsp/waves_tab.h): eight single-cycle waves, dark to bright, each twice: up to the 24th
harmonic (for notes under ~800 Hz) and up to the 8th (above). Additive, so band-limited; each wave's peak at 1."""
import math, sys

N, NW = 256, 8

def spectrum(w):
    """the waves, dark to bright: harmonic -> amplitude"""
    h = {}
    for k in range(1, 25):
        if w == 0: a = 1.0 if k == 1 else 0.0                                   # sine
        elif w == 1: a = [1, 0.5, 0.25][k - 1] if k <= 3 else 0.0               # soft
        elif w == 2: a = (1.0 / k) if k % 2 else 0.0                            # hollow (odd: square-ish)
        elif w == 3: a = 1.0 / k ** 1.3                                         # warm saw
        elif w == 4: a = 0.5 / k + math.exp(-((k - 5) / 1.6) ** 2)              # a vowel's formant at the 5th
        elif w == 5: a = 0.35 / k + math.exp(-((k - 10) / 2.2) ** 2) * 0.8     # a nasal one at the 10th
        elif w == 6: a = {1: 1, 4: 0.6, 7: 0.45, 11: 0.35, 16: 0.2}.get(k, 0)  # bell-like, sparse
        else: a = 1.0 / k ** 0.8                                                # bright buzz
        h[k] = a
    return h

def table(w, top):
    sp = spectrum(w)
    ph = {k: (k * k * 0.37) % 1.0 for k in sp}   # (fixed phases, not all aligned: a smoother wave)
    if w in (0, 1, 2, 3, 7): ph = {k: 0.0 for k in sp}
    xs = [sum(a * math.sin(2 * math.pi * (k * i / N + ph[k])) for k, a in sp.items() if k <= top) for i in range(N)]
    m = max(abs(x) for x in xs) or 1.0
    return [int(round(x / m * 32000)) for x in xs]

out = ["/* SPDX-License-Identifier: MPL-2.0 */",
       "/* WAVES' wavetable, made by app/tools/make_waves.py: [mip][wave][sample], mip 0 to the 24th harmonic, 1 to the 8th */",
       "#define WAVES_N %d" % N, "#define WAVES_W %d" % NW,
       "static const int16_t WAVES_TAB[2][WAVES_W][WAVES_N] = {"]
for top in (24, 8):
    out.append("  {")
    for w in range(NW):
        t = table(w, top)
        out.append("    { " + ", ".join(str(x) for x in t) + " },")
    out.append("  },")
out.append("};")
open(sys.argv[1] if len(sys.argv) > 1 else "app/dsp/waves_tab.h", "w").write("\n".join(out) + "\n")

# SPDX-License-Identifier: MPL-2.0
# desktop renderer: make -C app/tools, then app/tools/render SEED SECONDS OUT.wav [all] (fm1-base in base/, or set FM1_BASE)
CFLAGS = -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wdouble-promotion -ffp-contract=off -I$(FM1_BASE)/include
FM1_BASE ?= ../../base
SRC = ../dsp/voice.c ../dsp/seq.c ../dsp/fx.c ../dsp/music.c ../dsp/engine.c ../dsp/piece.c
render: ../dsp/*.c ../dsp/*.h render.c
	$(CC) $(CFLAGS) -o render render.c $(SRC) -lm

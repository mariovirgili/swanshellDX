# PC test tools for ggswan (not part of the WonderSwan build).
#   make                      build ggtest and wssim
#   ./ggtest rom.gg 600 out   run 600 frames, write PPM screenshots
#   ./wssim rom.gg 600 out    same, through the WonderSwan video layer;
#                             prints how many pixels differ from ggtest's
#                             reference renderer
#   make ztest                compare the Z80 core with the "z80" Python
#                             package (pip install z80) on random code
#   make drctest              recompiler: random Z80 code, translated and
#                             run in Unicorn (pip install unicorn capstone),
#                             compared with the interpreter
#   make drcgame ROM=x.gg     recompiler: the same on a real game
#   make drcsys ROM=x.gg      recompiler: the whole plugin (runtime, block
#                             cache, links) on a real game, compared frame
#                             by frame with the interpreter
CC ?= cc
CFLAGS ?= -O2 -Wall

all: ggtest wssim

ggtest: host_main.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -o $@ $^

wssim: wssim.c wssim_drcst.c ../ws/main.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -DGGSWAN_HOST -DGGSWAN_HOST_VISIBLE= -I. -I../core -I../ws -o $@ $^

libztest.so: ztest_lib.c ../core/z80.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $^

ztest: libztest.so
	python3 ztest.py ./libztest.so

libdrctest.so: drctest_lib.c ../core/z80.c ../drc/drc.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $^

libdrcgame.so: drcgame_lib.c ../core/gg.c ../core/z80.c ../drc/drc.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $^

drctest: libdrctest.so
	python3 drctest.py ./libdrctest.so 3000

drcgame: libdrcgame.so
	python3 drcgame.py ./libdrcgame.so "$(ROM)" 1500

drcsys: libdrcsys.so
	python3 drcsys.py ./libdrcsys.so "$(ROM)" 600 drcsys "100:80,110:00,300:80,310:00" 100

clean:
	rm -f ggtest wssim libztest.so libdrctest.so libdrcgame.so libdrcsys.so *.ppm

.PHONY: all ztest drctest drcgame drcsys clean

libdrcsys.so: drcsys.c ../ws/main.c ../ws/drc_rt.c ../drc/drc.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -shared -fPIC -DGGSWAN_HOST -DGGSWAN_HOST_VISIBLE= -I. -I../core -I../ws -o $@ $^

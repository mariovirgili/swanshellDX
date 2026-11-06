# PC test tools for ggswan (not part of the WonderSwan build).
#   make                      build ggtest and wssim
#   ./ggtest rom.gg 600 out   run 600 frames, write PPM screenshots
#   ./wssim rom.gg 600 out    same, through the WonderSwan video layer;
#                             prints how many pixels differ from ggtest's
#                             reference renderer
#   make ztest                compare the Z80 core with the "z80" Python
#                             package (pip install z80) on random code
CC ?= cc
CFLAGS ?= -O2 -Wall

all: ggtest wssim

ggtest: host_main.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -o $@ $^

wssim: wssim.c ../ws/main.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -DGGSWAN_HOST -DGGSWAN_HOST_VISIBLE= -I. -I../core -I../ws -o $@ $^

libztest.so: ztest_lib.c ../core/z80.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $^

ztest: libztest.so
	python3 ztest.py ./libztest.so

clean:
	rm -f ggtest wssim libztest.so *.ppm

.PHONY: all ztest clean

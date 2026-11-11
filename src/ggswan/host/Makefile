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
#   make sms                  ggtest_sms, wssim_sms, libdrcsys_sms.so: the
#                             same for smsswan (-DGGSWAN_SMS, .sms ROMs)
#   make sg                   sgtest: the SG-1000 (-DGGSWAN_SG1000, .sg
#                             ROMs) with its reference renderer
#   make sgcheck              sgtest on the test ROMs of mksgtest.py
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

# smsswan (the Master System plugin, -DGGSWAN_SMS): the same tools
sms: ggtest_sms wssim_sms libdrcsys_sms.so

ggtest_sms: host_main.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -DGGSWAN_SMS -o $@ $^

wssim_sms: wssim.c wssim_drcst.c ../ws/main.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -DGGSWAN_SMS -DGGSWAN_HOST -DGGSWAN_HOST_VISIBLE= -I. -I../core -I../ws -o $@ $^

libdrcsys_sms.so: drcsys.c ../ws/main.c ../ws/drc_rt.c ../drc/drc.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -shared -fPIC -DGGSWAN_SMS -DGGSWAN_HOST -DGGSWAN_HOST_VISIBLE= -I. -I../core -I../ws -o $@ $^

# sgswan (the SG-1000 plugin, -DGGSWAN_SG1000): the machine with a
# reference renderer of the TMS9918A (see ../SG1000.md), and a test ROM
sg: sgtest wssim_sg libdrcsys_sg.so

sgtest: sg_main.c sg_ref.h ../core/gg.c ../core/sg_vdp.c ../core/z80.c
	$(CC) $(CFLAGS) -DGGSWAN_SG1000 -o $@ $(filter %.c,$^)

wssim_sg: wssim.c wssim_drcst.c sg_ref.h ../ws/main.c ../ws/sg_video.inc ../core/gg.c ../core/sg_vdp.c ../core/z80.c
	$(CC) $(CFLAGS) -DGGSWAN_SG1000 -DGGSWAN_HOST -DGGSWAN_HOST_VISIBLE= -I. -I../core -I../ws -o $@ $(filter %.c,$^)

libdrcsys_sg.so: drcsys.c sg_ref.h ../ws/main.c ../ws/sg_video.inc ../ws/drc_rt.c ../drc/drc.c ../core/gg.c ../core/sg_vdp.c ../core/z80.c
	$(CC) $(CFLAGS) -shared -fPIC -DGGSWAN_SG1000 -DGGSWAN_HOST -DGGSWAN_HOST_VISIBLE= -I. -I../core -I../ws -o $@ $(filter %.c,$^)

sgcheck: sgtest
	for m in g1 g2 mc text; do python3 mksgtest.py sgtest_$$m.sg $$m && ./sgtest sgtest_$$m.sg 120 sgtest_$$m "" 0 || exit 1; done

clean:
	rm -f ggtest wssim libztest.so libdrctest.so libdrcgame.so libdrcsys.so *.ppm
	rm -f ggtest_sms wssim_sms libdrcsys_sms.so sgtest wssim_sg libdrcsys_sg.so sgtest_*.sg

.PHONY: all sms sg sgcheck ztest drctest drcgame drcsys clean

libdrcsys.so: drcsys.c ../ws/main.c ../ws/drc_rt.c ../drc/drc.c ../core/gg.c ../core/z80.c
	$(CC) $(CFLAGS) -shared -fPIC -DGGSWAN_HOST -DGGSWAN_HOST_VISIBLE= -I. -I../core -I../ws -o $@ $^

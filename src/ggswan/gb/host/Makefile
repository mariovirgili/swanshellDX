# gbswan's PC tools: the Game Boy core (../core) and its tests
CC	?= gcc
CFLAGS	:= -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -DGB_HOST -I../core
CORE	:= ../core/gb_cpu.c ../core/gb_bus.c ../core/gb_ppu.c ../core/gb_cart.c

FAST	:= ../core/gb_cpu.c ../core/gbf_hw.c ../core/gb_ppu.c ../core/gb_cart.c gbv_host.c

all: gbtest gbftest

gbtest: gbtest.c $(CORE) ../core/gb.h
	$(CC) $(CFLAGS) -o $@ gbtest.c $(CORE)

# the plugin's machine (gbf_hw.c) with the reference's renderer: the same
# command line as gbtest
gbftest: gbtest.c $(FAST) ../core/gb.h ../core/gbv.h
	$(CC) $(CFLAGS) -DGB_FAST -o $@ gbtest.c $(FAST)

clean:
	rm -f gbtest gbftest

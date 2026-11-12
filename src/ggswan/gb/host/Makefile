# gbswan's PC tools: the Game Boy core (../core) and its tests
CC	?= gcc
CFLAGS	:= -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -DGB_HOST -I../core
CORE	:= ../core/gb_cpu.c ../core/gb_bus.c ../core/gb_ppu.c

all: gbtest

gbtest: gbtest.c $(CORE) ../core/gb.h
	$(CC) $(CFLAGS) -o $@ gbtest.c $(CORE)

clean:
	rm -f gbtest

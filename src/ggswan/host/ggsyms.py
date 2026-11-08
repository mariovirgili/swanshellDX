#!/usr/bin/env python3
"""IRAM addresses of ggswan's state in a WS build (build/ggswan.elf), for
niledebug's peek ("peek nome game 0:ADDR LEN", hexadecimal LEN).
usage: ggsyms.py build/ggswan.elf [name ...]   (no names: the usual ones)"""
import sys
from elftools.elf.elffile import ELFFile

DEFAULT = ["gg", "z80", "drc_st", "psg_st", "sprite_table", "sprite_table_b",
           "map1", "map2", "act_line", "act_val", "act_n", "rec_n", "rec_line",
           "rec_val", "frame_waiting", "frame_credit", "nt_base", "sat_base",
           "drc_stat_blocks", "drc_stat_dispatch", "prof_count"]
f = ELFFile(open(sys.argv[1], "rb"))
want = sys.argv[2:] or DEFAULT
found = {}
for y in f.get_section_by_name(".symtab").iter_symbols():
    if y.name in want and y["st_info"]["type"] == "STT_OBJECT":
        a = y["st_value"] & 0xFFFFF
        if a < 0x10000 and (y.name not in found or y["st_size"] > found[y.name][1]):
            found[y.name] = (a, y["st_size"])
for n in want:
    if n in found:
        a, sz = found[n]
        print("%-20s game 0:%04X  %5d bytes   (peek %s game 0:%04X %X)" % (n, a, sz, n, a, max(sz, 1)))
    else:
        print("%-20s not found (static and inlined, or not in IRAM)" % n)
print("(also fixed: Game Gear VRAM 0:4000-7FFF, Game Gear RAM 0:8000-9FFF)")

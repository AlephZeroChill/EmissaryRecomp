#!/usr/bin/env python3
"""Rewrite CHKFEAT (HINT #40) to NOP in an AArch64 ELF's executable segments.

GCC 15's libgcc unwinder probes for the Guarded Control Stack with CHKFEAT, a HINT that ARMv8.0
cores (the Switch's Cortex-A57) execute as a NOP: x16 stays set, GCS reads as off, and the GCS
instructions behind it never run. Ryujinx does not execute that HINT as a NOP and aborts, so the
NOP is written in place, the same behaviour as the hardware.
    python3 horizon_nop_chkfeat.py ELF  (cmake/PublicProducts.cmake runs it before elf2nro)"""
import struct
import sys

CHKFEAT, NOP = 0xD503251F, 0xD503201F
path = sys.argv[1]
with open(path, 'r+b') as f:
    data = bytearray(f.read())
    assert data[:4] == b'\x7fELF' and data[4] == 2 and data[5] == 1, 'expected a 64-bit little-endian ELF'
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    count = 0
    for i in range(phnum):
        p_type, p_flags, p_offset, _, _, p_filesz = struct.unpack_from('<IIQQQQ', data, phoff + i * phentsize)
        if p_type != 1 or not p_flags & 1:  # PT_LOAD, executable
            continue
        for offset in range(p_offset, p_offset + p_filesz - 3, 4):
            if struct.unpack_from('<I', data, offset)[0] == CHKFEAT:
                struct.pack_into('<I', data, offset, NOP)
                count += 1
    f.seek(0)
    f.write(data)
print(f'{path}: {count} CHKFEAT -> NOP')

#!/usr/bin/env python3
"""Assert that a WITH_A7800_ROMS=OFF binary really carries no Atari 7800 BIOS or High Score Cart ROM.

This is the test that makes the COMPLIANCE.md claim checkable rather than
aspirational: it takes a distinctive slice out of each ROM image in
tools/roms and greps the built binary for it. Anything found is a ROM
that got compiled in despite the option, which would make the artifact
non-redistributable.

There is no freely-redistributable class of ROM here: the Atari 7800 BIOSes
and the High Score Cart's ROM are copyrighted Atari firmware, so a
WITH_A7800_ROMS=OFF binary is expected to contain none of them -- see
COMPLIANCE.md. A .zip in the ROM directory is looked inside.

usage: no_embedded_roms.py <binary> <rom-dir>
"""

import sys
import zipfile
from pathlib import Path

PROBE = 64


def probe_bytes(data):
    """A slice distinctive enough that finding it means something.

    Reject any chunk dominated by a single byte value and require real
    variety, so a run of 0x00 or 0xFF padding -- which occurs in almost any
    binary of a reasonable size -- can never be mistaken for a match. Scan the
    whole file rather than a few fixed offsets.
    """
    max_run = PROBE // 2      # no single byte value may cover half the slice
    min_distinct = 12

    for start in range(0, max(1, len(data) - PROBE), PROBE):
        chunk = data[start:start + PROBE]
        if len(chunk) != PROBE:
            continue
        if max(chunk.count(b) for b in set(chunk)) > max_run:
            continue
        if len(set(chunk)) < min_distinct:
            continue
        return chunk
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).read_bytes()
    romdir = Path(sys.argv[2])

    checked = 0
    leaked = []
    unprobeable = []

    images = []
    for f in sorted(romdir.iterdir()) if romdir.is_dir() else []:
        if not f.is_file():
            continue
        if f.suffix.lower() == ".zip":
            try:
                with zipfile.ZipFile(f) as z:
                    for name in z.namelist():
                        images.append((f.name + ":" + name, z.read(name)))
            except zipfile.BadZipFile:
                pass
        elif f.suffix.lower() in (".rom", ".bin", ".u7", ".a78"):
            images.append((f.name, f.read_bytes()))

    for name, data in images:
        if len(data) < 128:
            continue
        chunk = probe_bytes(data)
        if chunk is None:
            # Nothing distinctive enough to search for -- report it rather
            # than quietly treating it as checked.
            unprobeable.append(name)
            continue
        checked += 1
        if chunk in binary:
            leaked.append(name)

    if checked == 0:
        # No real ROMs to check against -- true of every CI and release build,
        # since the copyrighted BIOS (see COMPLIANCE.md) is gitignored
        # and never present on a checkout that isn't a developer's own machine.
        # SKIP (ctest exit code 77), not fail: there is nothing wrong with the
        # binary, just nothing here to probe it with.
        print("no_embedded_roms: SKIP: no ROM images found in "
              f"{romdir} -- nothing to check against")
        return 77

    for name in leaked:
        print(f"FAIL: {name} is embedded in a WITH_A7800_ROMS=OFF build")

    if leaked:
        return 1
    if unprobeable:
        print("no_embedded_roms: NOT checked (no distinctive slice): "
              + ", ".join(sorted(unprobeable)))
    print(f"no_embedded_roms: {checked} ROM images checked; none embedded, "
          "as intended")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""check_exports -- libmame_fngo exports fngo_mame_* and nothing else.

MAME links its own zlib, expat, FLAC, utf8proc ... and thousands of C++
symbols; any of them leaking out of the shared library could bind in place
of a frontend's (GTK and Qt bring their own zlib and expat). The version
script (ELF), export list (Mach-O) and dllexport (PE) keep them in; this
checks that they did.

Usage: check_exports.py <library>. Exit 77 (skip) without the tools.

Copyright (C) 2026 Thomas Cherryhomes
SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import re
import shutil
import subprocess
import sys

ALLOWED = re.compile(r"^_?fngo_mame_\w+$")


def elf(lib):
    nm = shutil.which("nm")
    if not nm:
        return None
    out = subprocess.run([nm, "-D", "--defined-only", lib], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in "TtDdBbRrVvWwiu":
            syms.append(parts[2].split("@")[0])
    # the version node itself
    return [s for s in syms if s not in ("FNGO_MAME_1",)]


def macho(lib):
    nm = shutil.which("nm")
    if not nm:
        return None
    out = subprocess.run([nm, "-gU", lib], capture_output=True, text=True, check=True).stdout
    return [line.split()[-1] for line in out.splitlines() if line.strip()]


def pe(lib):
    tool = shutil.which("objdump") or shutil.which("x86_64-w64-mingw32-objdump")
    if not tool:
        return None
    out = subprocess.run([tool, "-p", lib], capture_output=True, text=True, check=True).stdout
    syms, inside = [], False
    for line in out.splitlines():
        if "[Ordinal/Name Pointer] Table" in line:
            inside = True
            continue
        if inside:
            # "[   0] +base[   1]  0000 fngo_mame_api_version"
            m = re.match(r"\s*\[\s*\d+\]\s+(?:\+base\[\s*\d+\]\s+[0-9a-fA-F]+\s+)?(\S+)", line)
            if m:
                syms.append(m.group(1))
            elif line.strip() == "":
                if syms:
                    break
    return syms


def main():
    if len(sys.argv) != 2 or not os.path.exists(sys.argv[1]):
        print("usage: check_exports.py <library>")
        return 2
    lib = sys.argv[1]
    if lib.endswith(".dll"):
        syms = pe(lib)
    elif lib.endswith(".dylib"):
        syms = macho(lib)
    else:
        syms = elf(lib)
    if syms is None:
        print("SKIP: no nm/objdump")
        return 77
    bad = sorted(s for s in syms if not ALLOWED.match(s))
    good = [s for s in syms if ALLOWED.match(s)]
    print(f"{len(good)} fngo_mame_* exports")
    if not good:
        print("FAIL: no fngo_mame_* exports at all")
        return 1
    if bad:
        print(f"FAIL: {len(bad)} other symbols exported:")
        for s in bad[:50]:
            print("  " + s)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""romsweep -- boot every cartridge in a directory every way the app can,
and say which ones work.

For each .a78 (bare, or the one inside a .zip) it runs core's headless
runner (a7800_headless, libmame_fngo with no window):

  nobios   no BIOS, the image staged: the cartridge's boot block, loader
           and hand-over start it, as Open Cartridge does by default
  staged   the real BIOS, the image staged (the loader hands over to the
           BIOS when the BIOS would start the game itself)
  direct   the real BIOS, the image in the cartridge's SRAM from power-on
  stock    stock MAME (--stock) with its own cartridge for the image and the
           real BIOS; "direct" must match it pixel for pixel at --ab-frame
  network  (--fujinet) a network boot: the firmware's fujiboot client
           mounts the image from a fujinet-pc's SD folder, FujiNet streams
           it, the loader copies it in and hands over

A run passes when the cartridge reports the image running as a game, and
the picture showed something (more than one colour) after the hand-over,
and nothing reported an error.

"nobios" and "network" are also lined up against "direct" (which is stock
MAME's picture, frame for frame): a boot without the BIOS starts the game
sooner, so the same frames should come out shifted by the BIOS's running
time. "seq" says whether the last 300 frames of each match "direct" at one
offset, and what that offset is. A game that reads RAM it never wrote, or
the console's state as the BIOS leaves it, would show up here.

Contact sheets of the last frames (contact-*.png) are written for a human
to look over.

The BIOS images come from the directory being swept (the "[BIOS] ..." zips
of a No-Intro set), recognised by CRC; without them the BIOS modes are
skipped.

    python3 tools/romsweep/sweep.py --roms /tmp/a7800 \\
        --stock ~/Workspace/mame-a7800/a7800 \\
        --fujinet ~/Workspace/fujinet-pc-rs232/build/dist \\
        --fujiboot ~/Workspace/fujinet-firmware/pico/atari-7800/build/fujiboot.a78

Writes <work>/results.json and the report (--report, default
tools/romsweep/REPORT.md).

Copyright (C) 2026 Thomas Cherryhomes
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import concurrent.futures as cf
import json
import os
import queue
import shutil
import socket
import subprocess
import sys
import time
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# MAME's BIOS images (src/mame/atari/a7800.cpp), by CRC-32.
BIOSES = {
    0x5D13730C: ("a7800", "7800.u7", "a7800"),
    0xA0E10EDF: ("a7800pr", "c300558-001a.u7", "a7800"),
    0xD5B61170: ("a7800p", "c300558-001b.u7", "a7800p"),
}

# The light-gun titles (data CRCs, after the header), as core/src/a78info.c
# knows them.
LIGHTGUN = {0xC8849D36, 0x6A19F0FE, 0xED0A587D, 0x02764A86, 0xD2EA5686,
            0xE93D8894, 0x4A8F2171, 0x177FC850, 0x2FDDAD78, 0x47340DF9}

# What a person found looking at the frames where the automatic check
# disagrees: {mode: {file: note}}. A note marks the run "ok" in the report.
# (Reviewed 2026-10-08 from the contact sheets, nobios / network / BIOS side
# by side.)
ATTRACT = "the same game, at another point of an attract mode that start-up timing randomises"
NOTES = {
    "ab": {
        "Double Dragon (USA).zip": "stock MAME's own cartridge garbles it (stripes); ours shows the title screen",
        "Double Dragon (Europe).zip": "stock MAME's own cartridge garbles it; ours shows the title screen",
        "Rampage (USA).zip": "stock MAME's own cartridge garbles it (stripes); ours shows the title screen",
        "Summer Games (USA).zip": "stock MAME's own cartridge garbles it; ours shows the opening ceremony",
        "Winter Games (USA).zip": "stock MAME's own cartridge garbles it; ours shows the opening ceremony",
    },
    "seq": {
        "Asteroids (USA).zip": ATTRACT,
        "Dark Chambers (Europe).zip": ATTRACT,
        "Dark Chambers (USA).zip": ATTRACT,
        "Fatal Run (USA).zip": ATTRACT,
        "Ms. Pac-Man (USA).zip": ATTRACT,
        "Ninja Golf (USA).zip": ATTRACT,
        "One-on-One Basketball (Europe).zip": ATTRACT,
        "One-on-One Basketball (USA).zip": ATTRACT,
        "Pit Fighter (Unknown) (Proto).zip": ATTRACT,
        "Pole Position II (Europe).zip": ATTRACT,
        "Rampart (Unknown) (Proto).zip": ATTRACT,
        "Robotron - 2084 (USA).zip": ATTRACT,
        "Scrapyard Dog (Europe).zip": ATTRACT,
        "Scrapyard Dog (USA).zip": ATTRACT,
        "Summer Games (USA).zip": ATTRACT,
        "Winter Games (USA).zip": ATTRACT,
    },
    "netseq": {
        "Asteroids (USA).zip": ATTRACT,
        "Dark Chambers (Europe).zip": ATTRACT,
        "Dark Chambers (USA).zip": ATTRACT,
        "Desert Falcon (Europe).zip": ATTRACT,
        "Desert Falcon (USA).zip": ATTRACT,
        "Fatal Run (Europe).zip": ATTRACT,
        "Fatal Run (USA).zip": ATTRACT,
        "Galaga (USA).zip": ATTRACT,
        "Ms. Pac-Man (Europe).zip": ATTRACT,
        "Ms. Pac-Man (USA).zip": ATTRACT,
        "Ninja Golf (Europe).zip": ATTRACT,
        "Ninja Golf (USA).zip": ATTRACT,
        "One-on-One Basketball (Europe).zip": ATTRACT,
        "One-on-One Basketball (USA).zip": ATTRACT,
        "Pit Fighter (Unknown) (Proto).zip": ATTRACT,
        "Plutos (Unknown) (Proto).zip": ATTRACT,
        "Pole Position II (Europe).zip": ATTRACT,
        "Pole Position II (USA).zip": ATTRACT,
        "Rampart (Unknown) (Proto).zip": ATTRACT,
        "Robotron - 2084 (USA).zip": ATTRACT,
        "Scrapyard Dog (Europe).zip": ATTRACT,
        "Scrapyard Dog (USA).zip": ATTRACT,
        "Super Huey UH-IX (Europe).zip": ATTRACT,
    },
}

CTRL_NAMES = {0: "none", 1: "joy", 2: "gun", 3: "paddle", 4: "trakball",
              5: "2600 joy", 6: "2600 drv", 7: "2600 kbd", 8: "mouse",
              9: "ST mouse", 10: "snes", 11: "ataribox"}


def read_image(path):
    """(name, bytes) of the cartridge image in path (a .zip holds one)."""
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.filename.lower().endswith((".a78", ".bin")):
                    return info.filename, z.read(info)
        return None, None
    return path.name, path.read_bytes()


def header(data):
    h = {"title": "", "tv": None, "ctrl": (1, 1), "type": 0, "headered": False}
    if len(data) >= 128 and data[1:10] == b"ATARI7800":
        h["headered"] = True
        h["title"] = data[17:49].split(b"\0")[0].decode("latin-1").strip()
        h["type"] = (data[53] << 8) | data[54]
        h["ctrl"] = (data[55], data[56])
        h["tv"] = data[57] & 1
        body = data[128:]
    else:
        body = data
    h["crc"] = zlib.crc32(body) & 0xFFFFFFFF
    return h


def run(cmd, timeout, cwd=None, env=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=cwd, env=env)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        return -9, e.stdout or "", "timeout"


def headless(args, slot, image, system, bios, mode, frames, shot, port=1, extra=()):
    data = Path(args.work) / "mame" / f"w{slot}"
    hashes = Path(str(shot).rsplit(".", 1)[0] + ".hashes")
    cmd = [args.headless, "--data", str(data), "--rompath", str(Path(args.work) / "roms"),
           "--system", system, "--bios", bios, "--mode", mode, "--frames", str(frames),
           "--port", str(port), "--image", str(image), "--shot", str(shot),
           "--hashes", str(hashes)]
    cmd += list(extra)
    rc, out, err = run(cmd, timeout=args.timeout)
    res = {"rc": rc, "stderr": err[-600:]}
    line = out.strip().splitlines()[-1] if out.strip() else ""
    try:
        res.update(json.loads(line))
    except ValueError:
        res["parse_error"] = line[-300:]
    return res


def judge(res, want_crc=None):
    """'pass', or why not."""
    if res.get("refused"):
        return "refused: " + res["refused"]
    if res.get("rc") == -9:
        return "timeout"
    if res.get("run", -1) != 0:
        return f"MAME error {res.get('run')}"
    if not res.get("present"):
        return "no cartridge status"
    if res.get("boot_err"):
        return f"boot error {res['boot_err']}"
    if not res.get("booted") or res.get("mode") not in (2, 3):
        return f"no game (mode {res.get('mode')}, load {res.get('load_pct')}%)"
    if want_crc is not None and int(res.get("live_crc", "0"), 16) != want_crc:
        return f"wrong image {res.get('live_crc')}"
    if not res.get("nonblank_after_game"):
        return "blank picture after the hand-over"
    return "pass"


def read_hashes(path):
    try:
        return Path(path).read_text().split()
    except OSError:
        return []


def seq_match(a, b, window=300):
    """The offset K at which a's last `window` frames best match b (a[i] ==
    b[i + K]), and how many of them do."""
    if len(a) < window or not b:
        return None, 0
    where = {}
    for j, h in enumerate(b):
        where.setdefault(h, []).append(j)
    votes = {}
    start = len(a) - window
    for i in range(start, len(a)):
        for j in where.get(a[i], ()):
            votes[j - i] = votes.get(j - i, 0) + 1
    if not votes:
        return None, 0
    k = max(votes, key=lambda k: (votes[k], -abs(k)))
    return k, votes[k]


def seq_verdict(k, n, window=300):
    if k is None or n == 0:
        return "no frame in common"
    if n >= window * 9 // 10:
        return "pass"
    return f"{n}/{window} frames match (at {k:+d})"


def ppm_to_image(path):
    from PIL import Image
    return Image.open(path).convert("RGB")


def compare(a_path, b_path):
    """(differing pixels, total) or None."""
    try:
        from PIL import Image, ImageChops
    except ImportError:
        return None
    if not (Path(a_path).exists() and Path(b_path).exists()):
        return None
    a = ppm_to_image(a_path)
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        return (-1, a.size, b.size)
    diff = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v else 0)
    data = diff.get_flattened_data() if hasattr(diff, "get_flattened_data") else diff.getdata()
    return (sum(1 for v in data if v), a.size[0] * a.size[1])


def stock_run(args, slot, cart, system, bios, frame, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    for f in outdir.glob("**/*.png"):
        f.unlink()
    lua = Path(args.work) / "snap.lua"
    cmd = [args.stock, system, "-bios", bios, "-cart", str(cart),
           "-rompath", str(Path(args.work) / "roms"),
           "-snapshot_directory", str(outdir), "-cfg_directory", str(outdir / "cfg"),
           "-nvram_directory", str(outdir / "nvram"), "-diff_directory", str(outdir / "diff"),
           "-noreadconfig", "-skip_gameinfo", "-video", "none", "-sound", "none",
           "-nothrottle", "-snapview", "native", "-autoboot_script", str(lua),
           "-seconds_to_run", "120"]
    # MAME's Lua frame_done count runs one ahead of the frame numbers the
    # library hands out: stock frame N+1 is our frame N
    env = dict(os.environ, SNAP_AT=str(frame + 1))
    rc, out, err = run(cmd, timeout=args.timeout, env=env)
    pngs = sorted(outdir.glob("**/*.png"))
    return (pngs[0] if pngs else None), rc, err[-300:]


# ---- fujinet-pc instances for the network mode --------------------------------

FNCONFIG = """[General]
devicename=FujiNet
configenabled=1
boot_mode=0
status_wait_enabled=1

[Host1]
type=SD
name=SD

[BOIP]
enabled=1
host=localhost
port={port}
"""


class FujiNet:
    """A private fujinet-pc: its own config, SD folder and ports."""

    def __init__(self, dist, workdir, port):
        self.dir = Path(workdir)
        self.port = port
        self.sd = self.dir / "SD"
        if self.dir.exists():
            shutil.rmtree(self.dir)
        self.sd.mkdir(parents=True)
        dist = Path(dist)
        shutil.copy2(dist / "fujinet", self.dir / "fujinet")
        if (dist / "data").exists():
            shutil.copytree(dist / "data", self.dir / "data", symlinks=True)
        # a config of our own: never the user's (which may hold cloud tokens)
        (self.dir / "fnconfig.ini").write_text(FNCONFIG.format(port=port))
        self.log = open(self.dir / "fujinet.log", "w")
        self.proc = subprocess.Popen(
            ["./fujinet", "-c", "fnconfig.ini", "-s", "SD/", "-u", f"http://127.0.0.1:{port + 1}"],
            cwd=self.dir, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError(f"fujinet-pc did not listen on {port}")

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


def fujiboot_path(image):
    """The path fujiboot was built to mount (its one '/name.ext' string)."""
    data = Path(image).read_bytes()
    i = 128
    while i < len(data):
        if data[i] == ord("/") and data[i - 1] == 0:
            j = i
            while j < len(data) and 0x20 <= data[j] < 0x7F:
                j += 1
            if j < len(data) and data[j] == 0 and "." in data[i:j].decode() and j - i > 4:
                return data[i:j].decode()
        i += 1
    return None


# ---- the sweep -----------------------------------------------------------------

def sweep_one(args, slot, rom, fn):
    work = Path(args.work)
    name, data = read_image(rom)
    if data is None:
        return {"rom": rom.name, "error": "no image in archive"}
    h = header(data)
    tag = rom.stem
    cart = work / "carts" / name
    cart.parent.mkdir(parents=True, exist_ok=True)
    cart.write_bytes(data)
    shots = work / "shots"
    shots.mkdir(exist_ok=True)
    system = "a7800p" if h["tv"] == 1 else "a7800"
    bios = "a7800p" if system == "a7800p" else "a7800"
    have_bios = args.bioses.get(bios)
    gun = h["crc"] in LIGHTGUN
    extra = ("--gun", "160,112", "--controllers", "2,2") if gun else ()
    r = {"rom": rom.name, "image": name, "title": h["title"], "crc": f"{h['crc']:08X}",
         "tv": "PAL" if h["tv"] == 1 else ("NTSC" if h["tv"] == 0 else "?"),
         "ctrl": [CTRL_NAMES.get(c, str(c)) for c in h["ctrl"]], "type": h["type"],
         "headered": h["headered"], "gun": gun, "size": len(data)}

    res = headless(args, slot, cart, system, "none", "staged", args.frames, shots / f"{tag}.nobios.ppm", extra=extra)
    r["nobios"] = res
    r["nobios_verdict"] = judge(res, h["crc"])
    if res.get("refused"):
        return r
    if have_bios:
        for mode in ("staged", "direct"):
            # direct runs on, so the others (which skip the BIOS) line up
            frames = max(args.frames, args.ab_frame) + 900 if mode == "direct" else args.frames
            ex = list(extra)
            if mode == "direct":
                ex += ["--shot-at", str(args.ab_frame)]
            res = headless(args, slot, cart, system, bios, mode, frames, shots / f"{tag}.{mode}.ppm", extra=ex)
            r[mode] = res
            r[mode + "_verdict"] = judge(res, h["crc"])
        if "direct" in r:
            direct_h = read_hashes(shots / f"{tag}.direct.hashes")
            k, n = seq_match(read_hashes(shots / f"{tag}.nobios.hashes"), direct_h)
            r["nobios_seq"] = {"offset": k, "match": n}
            r["seq_verdict"] = seq_verdict(k, n)
        if args.stock and not gun and not h["headered"]:
            r["ab_verdict"] = "n/a: stock MAME cannot load a headerless image"
        elif args.stock and not gun:
            png, rc, err = stock_run(args, slot, cart, system, bios, args.ab_frame, work / "stock" / f"w{slot}")
            if png:
                dest = shots / f"{tag}.stock.png"
                shutil.copy2(png, dest)
                cmp = compare(shots / f"{tag}.direct.ppm", dest)
                r["ab"] = cmp
                if cmp is None:
                    r["ab_verdict"] = "no comparison"
                elif cmp[0] == 0:
                    r["ab_verdict"] = "pass"
                elif cmp[0] < 0:
                    r["ab_verdict"] = f"size {cmp[1]} vs {cmp[2]}"
                else:
                    r["ab_verdict"] = f"{cmp[0]} of {cmp[1]} pixels differ"
            else:
                r["ab_verdict"] = f"stock MAME made no snapshot (rc {rc}) {err.strip()[-120:]}"
    if fn is not None:
        target = fn.sd / args.fujiboot_path.lstrip("/")
        target.write_bytes(data)
        res = headless(args, slot, args.fujiboot, system, "none", "staged", args.frames + 600,
                       shots / f"{tag}.network.ppm", port=fn.port, extra=extra)
        r["network"] = res
        r["network_verdict"] = judge(res, h["crc"])
        if "direct" in r:
            k, n = seq_match(read_hashes(shots / f"{tag}.network.hashes"),
                             read_hashes(shots / f"{tag}.direct.hashes"))
            r["network_seq"] = {"offset": k, "match": n}
            r["netseq_verdict"] = seq_verdict(k, n)
    return r


def contact_sheets(results, work, kind="nobios"):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return []
    shots = Path(work) / "shots"
    cells = []
    for r in results:
        p = shots / f"{Path(r['rom']).stem}.{kind}.ppm"
        if p.exists():
            cells.append((r, p))
    out = []
    per = 20
    for page in range(0, len(cells), per):
        chunk = cells[page:page + per]
        cols, tw, th = 5, 320, 272
        rows = (len(chunk) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * tw, rows * (th + 14)), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for i, (r, p) in enumerate(chunk):
            img = Image.open(p).convert("RGB")
            x, y = (i % cols) * tw, (i // cols) * (th + 14)
            sheet.paste(img, (x, y + 14))
            verdict = r.get(kind + "_verdict", "")
            colour = (120, 255, 120) if verdict == "pass" else (255, 110, 110)
            draw.text((x + 2, y + 1), Path(r["rom"]).stem[:52], fill=colour)
        path = Path(work) / f"contact-{kind}-{page // per + 1}.png"
        sheet.save(path)
        out.append(path)
    return out


def report(results, args, path):
    modes = ["nobios"]
    if args.bioses:
        modes += ["staged", "direct", "seq"]
    if args.stock:
        modes += ["ab"]
    if args.fujinet:
        modes += ["network"]
        if args.bioses:
            modes += ["netseq"]
    games = [r for r in results if "error" not in r]
    # The gun's trigger is pulled on fixed frame numbers, and skipping the
    # BIOS moves the game's timeline against them: different shots land, so
    # the frame-by-frame comparison says nothing about the emulation.
    for r in games:
        if r.get("gun"):
            for m in ("seq", "netseq"):
                if m + "_verdict" in r:
                    r[m + "_verdict"] = "n/a: shots timed by frame"
    lines = ["# ROM sweep", "",
             f"{len(games)} cartridges from `{args.roms}`, {args.frames} frames a run "
             f"(A/B at frame {args.ab_frame}). Generated by `tools/romsweep/sweep.py`.", ""]
    lines += ["| Mode | What runs | Pass |", "|---|---|---|"]
    desc = {
        "nobios": "no BIOS, staged (Open Cartridge's default)",
        "staged": "the real BIOS, staged",
        "direct": "the real BIOS, in SRAM from power-on",
        "ab": f"direct vs stock MAME's own cartridge, pixel for pixel at frame {args.ab_frame}",
        "network": "network boot: fujiboot mounts it from fujinet-pc's SD",
        "seq": "nobios's last 300 frames are direct's, shifted by the BIOS's running time",
        "netseq": "network's last 300 frames are direct's, shifted",
    }
    for m in modes:
        ran = [r for r in games if (m + "_verdict") in r and not r[m + "_verdict"].startswith("n/a")]
        auto = sum(1 for r in ran if r[m + "_verdict"] == "pass")
        by_hand = sum(1 for r in ran if r[m + "_verdict"] != "pass" and r["rom"] in NOTES.get(m, {}))
        extra = f" ({by_hand} checked by hand †)" if by_hand else ""
        lines.append(f"| {m} | {desc[m]} | {auto + by_hand} / {len(ran)}{extra} |")
    lines += ["", "| Cartridge | CRC | Kind | TV | Controllers | " + " | ".join(modes) + " |",
              "|---|---|---|---|---|" + "---|" * len(modes)]
    for r in sorted(games, key=lambda r: r["rom"].lower()):
        kind = r.get("nobios", {}).get("plan_kind", "")
        cells = []
        for m in modes:
            v = r.get(m + "_verdict")
            if v is not None and v != "pass" and r["rom"] in NOTES.get(m, {}):
                v = "ok†"
            cells.append("—" if v is None else ("ok" if v == "pass" else v.replace("|", "/")))
        lines.append(f"| {Path(r['rom']).stem} | {r['crc']} | {kind} | {r['tv']} | "
                     f"{' / '.join(r['ctrl'])}{' (gun)' if r['gun'] else ''} | " + " | ".join(cells) + " |")
    notes = [(m, rom, text) for m, d in NOTES.items() for rom, text in d.items()
             if any(r["rom"] == rom for r in games)]
    if notes:
        lines += ["", "† Checked by hand; the automatic check is wrong for these:", ""]
        lines += [f"- {Path(rom).stem} ({m}): {text}" for m, rom, text in notes]
    errs = [r for r in results if "error" in r]
    if errs:
        lines += ["", "Not swept:", ""] + [f"- {r['rom']}: {r['error']}" for r in errs]
    lines += ["", "Light-gun titles get the XG-1 on both ports, aimed at the middle of the screen with the",
              "trigger pulled once a second; booting to their title or game is their pass. They are left out",
              "of the A/B comparison (stock MAME's a7800 has no light gun) and of the frame-by-frame ones",
              "(the shots are timed by frame number, so a boot without the BIOS meets them at other moments).", ""]
    Path(path).write_text("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--roms", required=True)
    ap.add_argument("--headless", default=str(ROOT / "build/core/a7800_headless"))
    ap.add_argument("--stock", help="stock MAME's a7800 binary, for the A/B comparison")
    ap.add_argument("--fujinet", help="a fujinet-pc dist directory (the fujinet binary and data/), for the network mode")
    ap.add_argument("--fujiboot", default=os.environ.get("A7800_TESTROM_DIR", "") and
                    os.path.join(os.environ["A7800_TESTROM_DIR"], "fujiboot.a78"))
    ap.add_argument("--work", default=str(ROOT / "build/romsweep"))
    ap.add_argument("--report", default=str(ROOT / "tools/romsweep/REPORT.md"))
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--frames", type=int, default=900)
    ap.add_argument("--ab-frame", type=int, default=900)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--only", help="sweep only the files whose name contains this")
    ap.add_argument("--port-base", type=int, default=11600)
    ap.add_argument("--report-only", action="store_true",
                    help="rewrite the report from the last run's results.json (same --stock/--fujinet)")
    args = ap.parse_args()

    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    (work / "snap.lua").write_text(
        'local n = 0\n'
        'emu.register_frame_done(function()\n'
        '  n = n + 1\n'
        '  if n == tonumber(os.getenv("SNAP_AT") or "600") then\n'
        '    manager.machine.video:snapshot()\n'
        '    manager.machine:exit()\n'
        '  end\n'
        'end)\n')

    roms = sorted(p for p in Path(args.roms).iterdir()
                  if p.suffix.lower() in (".zip", ".a78", ".bin"))
    # the BIOS images in the set, into MAME's layout
    args.bioses = {}
    games = []
    for p in roms:
        name, data = read_image(p)
        if data is None:
            continue
        crc = zlib.crc32(data) & 0xFFFFFFFF
        if crc in BIOSES:
            bios, fname, setdir = BIOSES[crc]
            d = work / "roms" / setdir
            d.mkdir(parents=True, exist_ok=True)
            (d / fname).write_bytes(data)
            args.bioses[bios] = True
            print(f"BIOS {bios}: {p.name}")
        elif not args.only or args.only.lower() in p.name.lower():
            games.append(p)

    if args.report_only:
        results = json.loads((work / "results.json").read_text())
        report(results, args, args.report)
        print("report:", args.report)
        return 0

    if args.fujinet and not (args.fujiboot and Path(args.fujiboot).exists()):
        sys.exit("--fujinet needs --fujiboot (fujinet-firmware pico/atari-7800/build/fujiboot.a78)")
    if args.fujinet:
        args.fujiboot_path = fujiboot_path(args.fujiboot)
        if not args.fujiboot_path:
            sys.exit(f"cannot find the path {args.fujiboot} mounts")

    slots = queue.Queue()
    fujinets = {}
    for k in range(args.jobs):
        slots.put(k)
        if args.fujinet:
            fujinets[k] = FujiNet(args.fujinet, work / "fn" / f"w{k}", args.port_base + 2 * k)

    def task(rom):
        k = slots.get()
        try:
            return sweep_one(args, k, rom, fujinets.get(k))
        finally:
            slots.put(k)

    results = []
    t0 = time.time()
    try:
        with cf.ThreadPoolExecutor(args.jobs) as ex:
            for r in ex.map(task, games):
                results.append(r)
                verdicts = {m: r.get(m + "_verdict") for m in ("nobios", "staged", "direct", "seq", "ab", "network", "netseq")}
                summary = "  ".join(f"{m}={v}" for m, v in verdicts.items() if v)
                print(f"[{len(results)}/{len(games)}] {r['rom']}: {summary}", flush=True)
    finally:
        for fn in fujinets.values():
            fn.stop()
    print(f"{len(results)} cartridges in {time.time() - t0:.0f}s")
    (work / "results.json").write_text(json.dumps(results, indent=1))
    for kind in ("nobios", "network"):
        for p in contact_sheets(results, work, kind):
            print("contact sheet:", p)
    report(results, args, args.report)
    print("report:", args.report)
    bad = [r for r in results
           if any(v != "pass" and not v.startswith("n/a") and r["rom"] not in NOTES.get(k[:-8], {})
                  for k, v in r.items() if k.endswith("_verdict"))]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

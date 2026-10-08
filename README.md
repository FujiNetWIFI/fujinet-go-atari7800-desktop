# FujiNet Go — Atari 7800

A self-contained Atari 7800 ProSystem with a built-in
[FujiNet](https://fujinet.online/): power on into the FujiNet CONFIG client,
browse a network host from the console, boot `.a78` images over the network,
and let a FujiNet-aware program keep talking to the network — all in one
desktop app, with no BIOS needed. A member of the FujiNet Go desktop family
(`fujinet-go-adam-desktop`, `-apple2-`, `-coco-`, `-msx-`, `-intv-`,
`-astrocade-`, `-coleco-`, `-atari2600-`, `-nes-`, `-sms-`).

| | |
|---|---|
| **Emulator** | [MAME](https://www.mamedev.org/)'s `a7800` driver, from the [`tschak909/mame`](https://github.com/tschak909/mame/tree/fujinet-go-a7800) fork's `fujinet-go-a7800` branch, built by MAME's own makefiles as a shared library (`libmame_fngo`) for this one driver, with a headless OSD and a small C API (`src/fngo/fngo_mame.h`). The emulation is MAME's, unchanged: a game started from the cartridge's SRAM matches stock MAME pixel for pixel. |
| **Cartridge** | The FujiNet Atari 7800 cartridge (`fujinet-firmware` `pico/atari-7800`), modelled in the fork as a cartridge-slot device: the mailbox, the boot block and loader, the hand-over (to the console's BIOS, or straight to the game), the cartridge's mapper engine (`a78map`: plain ROMs, SuperGame with and without RAM and POKEY, the 9-bank SuperGame, Activision, Absolute, banked RAM) and the High Score Cart. The protocol, mapper and loader are the firmware's own sources, vendored verbatim; the mailbox service runs on its own thread, as it does on the cartridge's second core. |
| **FujiNet** | The firmware's `RS232` PC target, built in-process as `libfujinet` and dialled by the cartridge over loopback (BoIP on **11510**, web admin on **11511**). |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/GDI) — each with the display, a Controllers window, Preferences, a live FujiNet console log, and a debugger. |
| **Packaging** | Per-frontend DEB/RPM/TGZ, two Flatpaks, a Windows (x86-64) zip and NSIS installer, and macOS bundles for Apple Silicon and Intel, all through GitHub Actions. |

## What it is

- **No BIOS needed.** The console boots the FujiNet cartridge directly: the
  fork adds a `none` BIOS to MAME's `a7800` and `a7800p` that leaves MARIA on
  and the cartridge mapped, and the cartridge's loader starts games itself,
  with the console in the state the BIOS would leave it. **Machine ▸ Import
  BIOS…** takes your own NTSC or PAL BIOS (recognised by size and CRC,
  whatever the file is called, `.zip` or not); Preferences can then choose
  it per console, and it shows the Atari logo and starts the games it
  accepts, exactly as on the hardware. Nothing copyrighted ships with the
  app.
- **CONFIG is the power-on program.** The CONFIG client (`fujinet-config`'s
  7800 build, with its game lobby) is baked into the cartridge. From CONFIG,
  pick a host and an image; FujiNet streams the `.a78` file to the cartridge,
  the loader copies it in, and the game runs.
- **The cartridge, faithfully.** Every image — CONFIG, a network-booted game,
  an opened cartridge file — runs on the cartridge's own mapper engine, so
  what works here works on the cartridge, up to 448K. An image needing a
  board the cartridge does not emulate (XM, XBoard, Versaboard, Megacart) is
  refused with the reason. A program that carries the `FUJI` claim keeps the
  mailbox; a commercial game closes it.
- **Open Cartridge…** (`.a78`, `.bin`, or a `.zip`/`.7z` holding one) powers
  the console on with the image *staged* in the cartridge, so it starts
  through the boot block, the loader and the hand-over exactly as a network
  boot does (and is remembered for the next start). A PAL cartridge brings up
  the PAL console when the TV system is on Auto. **Import Cartridge to SD…**
  copies one into FujiNet's SD root, where CONFIG lists it. Dropping a file
  on the window opens a cartridge, imports a BIOS or High Score Cart ROM, or
  copies anything else to the SD folder.
- **The console's switches.** Select (F1), Reset (F2) and Pause (F3), and the
  two difficulty switches (Alt+L / Alt+R), from the keyboard, the Console
  menu, a gamepad or the Controllers window. **Power Cycle** restarts what
  the cartridge holds; **Reboot to CONFIG** (Escape) ejects it and boots
  CONFIG.
- **Light guns.** The XG-1 light gun is a controller type of the fork's
  `a7800` driver: aim with the mouse (a crosshair over the picture), left
  click pulls the trigger. The sensing point is calibrated against the games'
  own hit tests (Barnyard Blaster's bullet holes land where you aim;
  Meltdown's buttons answer where they are drawn). Alien Brigade, Barnyard
  Blaster, Crossbow, Meltdown and Sentinel are recognised and get the gun
  automatically; ports can also be set to a ProLine joystick, a 2600
  joystick, a light gun or nothing.
- **The High Score Cart.** Import its ROM and switch it on in Preferences;
  the cartridge provides it, and keeps the scores through FujiNet.
- **The debugger, in every frontend.** F12 opens it and stops the machine:
  Run/Stop (F5), Step (F7), Step Over (F8), Step Out (Shift+F8), Frame, Run
  to cursor. Tabs: the Console (MAME's own debugger commands — `bpset`,
  `wpset` with conditions, `print`, `dump`, `find`, `trace` … plus `cart`,
  `maria` and `labels`, with Tab completion), CPU & Memory (editable),
  Disassembly (toggle breakpoints, follow PC, labels from ld65/ca65/plain
  symbol files), MARIA (control register decoded, the display list list,
  the palettes), I/O (INPTCTRL, TIA sound, RIOT ports, INPT0-5, POKEY),
  Cartridge (link, mode, hand-over, load progress, mapper, HSC) and
  Breakpoints & Watchpoints. The engine is MAME's.
- **Gamepads that come and go.** SDL3 hot-plug: a pad plugged in while a game
  runs is assigned to the next free player within a second, unplugging
  releases it, and a pad that comes back gets its player back; each event is
  shown in the window. The **Controllers** window shows both ProLine
  joysticks and the console's switches live, presses them with the mouse,
  and rebinds any control to a key or gamepad button (Map).

The icon is the family mark, inverted as on the Atari 2600's, on a slightly
deeper orange (`#F0932F`, the 2600's is `#FFA645`); the same orange is the UI
accent, marking held buttons, the Map target, the current line in the
disassembly and the FujiNet link.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Both dependencies are fetched at their pinned commits by the configure step
— MAME as the fork's source archive (about 200 MB; MAME's history is far too
big to clone), fujinet-firmware as a clone — so a plain `git clone` is
enough. MAME builds itself for the one driver the first time (about nine
hundred files: a couple of minutes on a fast machine) and incrementally
after that. To develop against working checkouts:

```sh
cmake -B build -DMAME_SRC=~/Workspace/mame-fngo \
               -DFUJINET_SRC=~/Workspace/fujinet-firmware
```

`-DFRONTEND=none` builds just the core and its tests; `-DWITH_FUJINET=OFF`
skips the firmware build (the machine boots CONFIG reporting the link down);
`-DMAME_FNGO_PREBUILT=<prefix>` uses an installed `libmame_fngo` (the Flatpaks
build it as a module of its own); `-DMAME_CCACHE=ON` puts ccache in front of
MAME's compiler. The macOS bundle needs macOS 13.3 or later.

The `netboot` test boots an image over the network end to end with the
cartridge bring-up's own test clients; point it at them to run it:

```sh
A7800_TESTROM_DIR=~/Workspace/fujinet-firmware/pico/atari-7800/build ctest --test-dir build -R netboot
```

### The ROM sweep

`tools/romsweep/sweep.py` boots every cartridge in a directory every way the
app can — without a BIOS, with the BIOS (staged and direct), and over the
network through a private `fujinet-pc` — compares the BIOS boot with stock
MAME pixel for pixel, and lines the BIOS-less and network boots up against
it frame by frame. `tools/romsweep/REPORT.md` is its last report.

```sh
python3 tools/romsweep/sweep.py --roms /path/to/a7800 \
    --stock ~/Workspace/mame-a7800/a7800 \
    --fujinet ~/Workspace/fujinet-pc-rs232/build/dist \
    --fujiboot ~/Workspace/fujinet-firmware/pico/atari-7800/build/fujiboot.a78
```

### Cross-building Windows on Linux

```sh
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz
tar xzf SDL3-devel-3.4.12-mingw.tar.gz
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
      -DFRONTEND=windows -DWITH_FUJINET=OFF \
      -DCMAKE_PREFIX_PATH="$PWD/SDL3-3.4.12/x86_64-w64-mingw32"
cmake --build build-win
```

This cross-builds `mame_fngo.dll` with MAME's own makefiles as well. It is
the desk-side check; `fujinet.dll` does not cross-build (the firmware's PC
target needs mingw builds of zlib and libssh's dependencies), so the release
build is native MSYS2/UCRT64, which builds everything.

## Ports

FujiNet's BoIP listener is on **11510** and its web admin UI on **11511** —
high ports of this app's own, continuing the family's table (Astrocade
11500/11501, ColecoVision 11502/11503, Atari 2600 11504/11505, NES
11506/11507, Master System 11508/11509), so a standalone `fujinet-pc` or a
sibling app never collides.

FujiNet listens and the cartridge dials in, so the session starts FujiNet
first and waits for its listener before powering on. Every power cycle
closes the old cartridge's link before the new cartridge dials.

## Keys

| | |
|---|---|
| Player 1 | arrows; **Z** = Button 1, **X** = Button 2 |
| Player 2 | **I J K L**; **N** = Button 1, **M** = Button 2 |
| Gamepads | D-pad / left stick; South = Button 1, East = Button 2, Back = Select, Start = Reset, Left Shoulder = Pause |
| Console | **F1** Select, **F2** Reset, **F3** Pause; **Alt+L** / **Alt+R** the difficulty switches |
| Reboot to CONFIG | **Escape** (Ctrl+R on the menu) |
| Light gun | aim with the mouse; left click is the trigger |
| Fullscreen / Controllers / Debugger | F11 / F9 / F12 (debugger: F5 run/stop, F7 step, F8 step over, Shift+F8 step out) |

Every control is remappable in the Controllers window.

## How this was verified

- **The core on Linux** — `ctest` covers the session (CONFIG boots with no
  BIOS and paints; a cartridge opens staged, runs, survives a power cycle
  and is ejected by Reboot to CONFIG; a PAL cartridge brings up the PAL
  console at 50 Hz; an image too big for the cartridge is refused with the
  reason; settings, switches and the open cartridge persist), bindings,
  media routing (cartridges, `.zip` archives, BIOS images recognised by CRC
  whatever their name), gamepads, the debugger contract (attach stops,
  step/over/out/run-to, disassembly with the PC line, breakpoints and
  watchpoints that hit and survive a power cycle, memory and register
  edits, MARIA, I/O, the cartridge tab, refused commands), headless boots of
  both consoles staged and direct, the library's export list, the FujiNet
  link (`fujibus_smoke`), and a full network boot (`netboot`: fujiboot's
  MOUNT_HOST, SET_DEVICE_FULLPATH, MOUNT_IMAGE, the stream, the loader, the
  hand-over, the new image running) against the in-process FujiNet.
- **Every cartridge in a 116-file No-Intro set** (`tools/romsweep`): all 114
  games boot without a BIOS, with the BIOS (staged and direct) and over the
  network; the BIOS boot matches stock MAME pixel for pixel at frame 900
  (98 automatically; for the other 5, stock MAME's own cartridge garbles
  the game and ours shows it correctly), and the BIOS-less and network boots
  replay the BIOS boot's frames (the rest checked by eye: the same game at
  another point of an attract mode). See `tools/romsweep/REPORT.md`.
- **The light gun** — calibrated and checked against Barnyard Blaster
  (bullet holes land on the aim, NTSC and PAL) and Meltdown (every button
  answers exactly where it is drawn, NTSC and PAL).
- **GNOME and KDE** — built with no frontend warnings, desktop and metainfo
  files validated, and smoke-run headless (GTK Broadway, Qt offscreen) with
  the debugger, Controllers and Preferences open.
- **Windows** — cross-built with mingw-w64, `mame_fngo.dll` included, with an
  import table of system DLLs only. Not yet looked at on a real Windows
  desktop.
- **macOS** — source-complete, compiled only by CI, as every macOS frontend
  in the family was.

## Cutting a release

Pushing a `v*` tag builds every platform and, only if all of it passes,
publishes what it produced as a **draft** release:

| Asset | Contents |
|---|---|
| `fujinet-go-atari7800-gnome-<version>-Linux.{deb,rpm,tar.gz}` | the GNOME frontend, packaged with CPack |
| `fujinet-go-atari7800-kde-<version>-Linux.{deb,rpm,tar.gz}` | the KDE frontend, packaged with CPack |
| `fujinet-go-atari7800-<version>-windows.zip` | the exe, `mame_fngo.dll`, `fujinet.dll`, and the `fujinet/` runtime tree |
| `fujinet-go-atari7800-<version>-windows-setup.exe` | NSIS installer, per-user, no admin rights |
| `fujinet-go-atari7800-<version>-macos-{arm64,x86_64}.zip` | the `.app` bundle, MAME and FujiNet inside |
| `online.fujinet.go.atari7800.{gnome,kde}.flatpak` | single-file bundles: `flatpak install ./…flatpak` |

The version is declared in the tree (`project(… VERSION …)` and both
metainfo files, which template it), not derived from the tag; `check-version`
stops the release if they disagree. To release 0.2.0: set the version in
`CMakeLists.txt`, add a `<release>` entry with its date to both
`frontends/*/data/*.metainfo.xml.in`, commit, then
`git tag -a v0.2.0 && git push origin v0.2.0`.

A change to the MAME fork is a new pin: push the branch, then set
`MAME_COMMIT` and `MAME_ARCHIVE_SHA256` in `cmake/Dependencies.cmake` and the
`mame-fngo` module's archive in both `build-aux/flatpak/*.yml`.

The Windows release build is native MSYS2/UCRT64, and the `release.yml` job
checks the import tables of the exe, `mame_fngo.dll` and `fujinet.dll`
against a system-DLL whitelist. The macOS job signs and notarises when the
`MACOS_*` secrets are set, and checks the bundle for leaked Homebrew dylibs.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.

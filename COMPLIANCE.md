# Compliance

Per-component provenance for `fujinet-go-atari7800-desktop`, written before
the first public build, in the family tradition (see
`fujinet-go-adam-desktop/COMPLIANCE.md`, `fujinet-go-sms-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **MAME** (the emulator: the `a7800` / `a7800p` driver, MARIA, the 6502, TIA and POKEY sound, the cartridge slot, the debugger engine, its expression evaluator and disassembler) | [`tschak909/mame`](https://github.com/tschak909/mame/tree/fujinet-go-a7800), branch `fujinet-go-a7800` — mamedev's [MAME](https://github.com/mamedev/mame) plus the changes listed below | GPL-2.0-or-later as a whole; the files compiled here are BSD-3-Clause as marked, © the MAME contributors | pinned in `cmake/Dependencies.cmake` (the commit's source archive, SHA-256 checked); built by MAME's own makefiles for the `fngo` target alone (`cmake/BuildMame.cmake`) as the shared library `libmame_fngo` / `mame_fngo.dll`, which exports only the `fngo_mame_*` C API |
| **The FujiNet Atari 7800 cartridge in MAME** (`src/devices/bus/a7800/fujinet*`, `fujilink*`, `fujinet_host.h`) | the same fork; the cartridge's MAME device from `fujinet-firmware` `pico/atari-7800/emu`, made asynchronous and portable there | BSD-3-Clause, © Thomas Cherryhomes | part of `libmame_fngo` |
| **The cartridge firmware's own sources** (`fujimail`, `fujibus`, `a78map`, `a78map_db`, `fuji_load`, `hsc`, `cpuwiz`, `fuji_mailbox.h`, `a78_cart.h`, the boot block `a78bootblk.h` and loader `a78loaderrom.h`) | `fujinet-firmware` branch `add-atari7800` (`pico/atari-7800/firmware`), vendored verbatim by the fork's `fujinet_sync.sh`; provenance and per-file SHA-256 in `src/devices/bus/a7800/FUJINET_SOURCES` | the FujiNet project's (GPL-3.0), © Thomas Cherryhomes | compiled into `libmame_fngo` with the device |
| **The CONFIG client** (`fujiconfigrom.h`) | [`FujiNetWIFI/fujinet-config`](https://github.com/FujiNetWIFI/fujinet-config) branch `add-atari7800` (`atari7800/`), recorded in `FUJINET_SOURCES` | GPL-3.0 (the FujiNet project's) | the cartridge's resident image, as on the hardware |
| **The fngo target and OSD** (`src/fngo/`, `src/osd/fngo/`, `scripts/target/fngo/`, `scripts/src/osd/fngo*.lua`) | the same fork | BSD-3-Clause, © Thomas Cherryhomes | the library's entry points and its headless OSD |
| MAME's bundled third-party code that its core links: zlib, zstd, the LZMA SDK (7z), expat, FLAC, libjpeg, utf8proc, asmjit, softfloat3, ymfm, WDL FFT, linenoise | MAME's `3rdparty/` | Zlib; BSD-3-Clause; public domain; MIT; BSD-3-Clause; IJG; MIT; Zlib; BSD-3-Clause; BSD-3-Clause; WDL (zlib-style); BSD-2-Clause | compiled into `libmame_fngo` by MAME's build; none of their symbols is exported |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232`, plus `tools/fujinet/patches/` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically for releases |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet`: system package where it is a usable 3.x, otherwise the pinned source |

Everything above is GPL-3.0-or-later or compatible with it: the combined work
is distributed under GPL-3.0.

## What is changed in MAME

The fork's `fujinet-go-a7800` branch starts from mamedev's MAME
(`0e0e3b86495`, 0.289) and changes only what the 7800 needs; the emulation
of the console itself is MAME's, unchanged, which `tools/romsweep` checks
(a game started from the cartridge's SRAM with the BIOS matches stock MAME
pixel for pixel):

- **The FujiNet cartridge** — new files in `src/devices/bus/a7800/`, a slot
  option `fujinet`, and `bus.lua`. The mailbox service runs on a worker
  thread as on the cartridge's second core; the link to FujiNet is POSIX or
  Winsock.
- **A `none` BIOS** for `a7800` and `a7800p` (`src/mame/atari/a7800.cpp`):
  MARIA on, the BIOS unmapped, the cartridge's reset vector taken at
  power-on. No Atari code is involved; with a real BIOS selected nothing
  changes.
- **Controller types and the XG-1 light gun** (`a7800.cpp`): a `CONTROLLERS`
  configuration per port (ProLine joystick, 2600 joystick, light gun, none)
  and the gun's sensor and trigger; the sensing point is calibrated against
  the games' own hit tests. With a gun attached every second TIA access
  costs a cycle, as the 7800's slow TIA cycle does; without one, MAME's
  timing is untouched.
- **The `fngo` target** — a STANDALONE target with only the 7800's driver,
  devices, CPU and sound chips, a headless OSD (`src/osd/fngo/`) that hands
  frames, samples and the debugger's stops to the host, and the C API
  (`src/fngo/fngo_mame.h`) with an export list per platform. MAME's
  frontend, Lua, the bgfx renderer and the UI menus are not built.
- **Build scripts** — guards so the `fngo` OSD skips what it does not use
  (bgfx, SDL) and builds as a shared library with MinGW; one SDL-only line
  in `osdlib_unix.cpp` guarded.

## System ROMs — none ship

The Atari 7800 BIOSes and the High Score Cart's ROM are copyrighted Atari
firmware. **This project does not distribute them, and nothing needs them:**
the console boots the FujiNet cartridge through the `none` BIOS, and the High
Score Cart is an option.

- **Import BIOS…** copies a user's own image into the ROM directory under
  the data directory (`roms/a7800/7800.u7`, `roms/a7800p/c300558-001b.u7`
  …), recognised by size and CRC-32 against MAME's own table; Preferences
  then chooses it per console. The High Score Cart's ROM (`hiscore`
  `highscre.bin`) is imported the same way.
- **`WITH_A7800_ROMS`** (default **OFF**, and OFF for every published
  artifact) lets a developer's own build embed the recognised images found
  in `tools/roms/`. The `no_embedded_roms_*` tests grep every shipped binary
  (the frontends, `libmame_fngo`, the headless runner) for distinctive slices
  of whatever real images are in `tools/roms/`, and fail if any is found;
  they skip where there are none to probe with, as on CI.
- The tests and the ROM sweep never need a BIOS: the tests write their own
  cartridges (`core/tests/test_files.h`), and BIOS-recognition tests use
  filler images whose CRC-32 is forged to match (CRC-32 is linear), so no
  Atari byte is involved.

Game cartridges are the user's own: opened from a local file, or served by
FujiNet from the SD folder or a network host.

## Deliberately not used

- **MAME's own frontend** (`src/frontend/`), Lua scripting, the bgfx / SDL /
  Windows OSDs and the UI menus — the four native frontends and
  `core/mame/MameHost` replace them; they are not built.
- **MAME's save states and rewind** — refused by the debugger and not
  offered: they would replay or roll back mailbox transactions that FujiNet
  has already acted on.
- **MAME's other cartridge boards for game images** — every image runs on
  the cartridge's own mapper engine, as on the hardware; MAME's `a78_*`
  boards are compiled (the slot references them) but never selected.

## Icon and name

The icon is the FujiNet Go family mark (the same artwork as the other
desktops), inverted as on the Atari 2600 app's, on `#F0932F`
(`tools/icons/make-icons.py`). "Atari", "7800" and "ProSystem" are
trademarks of Atari Interactive; they are used here to name the machine
being emulated, and the project is not affiliated with or endorsed by Atari.

## Trademarks and names

"FujiNet" is the FujiNet project's name. "MAME" is a trademark of Gregory
Ember. Neither project endorses this application; both are credited in the
About box of every frontend.

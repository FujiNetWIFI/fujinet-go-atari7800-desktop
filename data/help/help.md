This is the text of FujiNet Go Atari 7800's built-in Help (Help ▸ FujiNet Go
Atari 7800 Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only
to the frontend it is about; here you see them all. Every topic below is one
page in the app.

# Getting started

FujiNet Go Atari 7800 is an Atari 7800 ProSystem with a FujiNet cartridge in
its slot. The console is MAME's `a7800` driver, unchanged. The FujiNet runs
inside the app, so CONFIG comes up at once and can load games from the network.

1. **Start the app.** The console powers on and runs FujiNet's CONFIG. It
   needs no BIOS: the cartridge's loader starts games itself. See
   [BIOS and ROMs](#bios-and-roms).
2. **Pick a game in CONFIG.** Choose a host (the SD folder or a server on the
   network) and an `.a78` image on it. FujiNet streams it to the cartridge,
   the loader copies it in, and the game runs. See [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…**
   (<kbd>Ctrl+O</kbd>) takes an `.a78` or `.bin`, or a `.zip` or `.7z` that
   holds one. See [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window
   (<kbd>F9</kbd>) work the two joysticks and the console's switches. See
   [Controllers and keys](#controllers-and-keys).
5. **Change the console.** [Preferences](#preferences) picks the TV system,
   the BIOS, the High Score Cart, the controllers, the picture, the sound and
   FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.
(In the main window <kbd>F1</kbd> is the console's SELECT switch.)

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-atari7800-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…**, **Eject Cartridge**, **Import Cartridge to SD…** and
  **Import BIOS…**: see [Cartridges](#cartridges) and
  [BIOS and ROMs](#bios-and-roms).
- **Power Cycle** and **Reboot to CONFIG** (<kbd>Ctrl+R</kbd>, or
  <kbd>Escape</kbd>).
- **Select**, **Reset** and **Pause**, and the two difficulty switches: the
  console's switches.
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **TV Aspect**, **Smooth Scaling** and **Fullscreen** (<kbd>F11</kbd>).
- **FujiNet Web UI** and **Console Log**.
- **Preferences** (<kbd>Ctrl+,</kbd>), **Help**, **About** and **Quit**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, Console, View, FujiNet and Help. The Machine menu
has the cartridge items, Power Cycle, Reboot to CONFIG, Preferences (Settings
on Windows) and Quit (Exit on Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences (called Settings) and Quit are in the app menu, and the
cartridge items are in the Machine menu.

<!-- end -->

The status shows the cartridge and FujiNet's state:

| Status | Means |
|---|---|
| connected | The cartridge is talking to FujiNet. |
| link down | FujiNet is off or not answering yet; CONFIG tries again. |
| loading 42% | FujiNet is sending a program to the cartridge. |
| game running; mailbox closed | A game that does not use FujiNet is running. |

Messages (a cartridge opened, a gamepad plugged in, FujiNet restarted) show
for a few seconds over the picture.

## Power and reset

- **Power Cycle** restarts what the cartridge holds.
- **Reboot to CONFIG** (<kbd>Ctrl+R</kbd>, or <kbd>Escape</kbd>) ejects an
  opened cartridge and boots CONFIG, exactly as at power-on.
- **Reset** (<kbd>F2</kbd>) presses the console's RESET switch.
- **Pause** (<kbd>F3</kbd>) presses the console's PAUSE switch (in 2600 mode,
  Color / B&W). The Debugger's Stop is separate.

## The picture

A television shows the 320-pixel picture 4:3; **TV Aspect** switches between
that and square pixels. **Smooth Scaling** blends the pixels when the picture
is stretched, and leaves them sharp when it is off.

# Controllers and keys

The 7800 has two controller ports. Each can hold a **ProLine joystick** (a
stick and two fire buttons), a **2600 joystick**, or the **XG-1 light gun**.
The console has the switches **Select**, **Reset** and **Pause**, and the left
and right **difficulty** switches.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Does | Player 1 | Player 2 |
|---|---|---|
| Up, down, left, right | The arrow keys | <kbd>I</kbd>, <kbd>K</kbd>, <kbd>J</kbd>, <kbd>L</kbd> |
| Button 1 | <kbd>Z</kbd> | <kbd>N</kbd> |
| Button 2 | <kbd>X</kbd> | <kbd>M</kbd> |

| Console | Key |
|---|---|
| Select | <kbd>F1</kbd> |
| Reset | <kbd>F2</kbd> |
| Pause | <kbd>F3</kbd> |
| Left difficulty | <kbd>Alt+L</kbd> |
| Right difficulty | <kbd>Alt+R</kbd> |
| Reboot to CONFIG | <kbd>Escape</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+O</kbd> | Open Cartridge |
| <kbd>Ctrl+R</kbd> | Reboot to CONFIG |
| <kbd>Ctrl+,</kbd> | Preferences |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde windows -->

Keys with <kbd>Ctrl</kbd> never reach the console.

<!-- end -->

<!-- only: macos -->

Keys with ⌘ never reach the console.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. The first is Player 1, the second Player 2; a pad plugged in while a
game runs is assigned to the next free player within a second, unplugging
releases it, and a pad that comes back gets its player back. Preferences can
fix which is which. The D-pad moves the joystick, and with **Left stick drives
the joystick** on, the left stick does too.

## The light gun

The XG-1 light gun is a controller type: aim with the mouse (a crosshair shows
over the picture) and press the left button to pull the trigger. Alien
Brigade, Barnyard Blaster, Crossbow, Meltdown and Sentinel are recognised and
get the gun automatically when the port is on *Auto*.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, then click a joystick control or a console switch, then press the key
or the gamepad button to give it. The control's old key or button is taken from
it. **Defaults** puts every control back. The map is kept in the settings, and
only what differs from the defaults is written.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) shows both ProLine joysticks and the
console's switches live, and presses them with the mouse. It floats on its own
at a fixed size. The controls that are held light up in the accent colour,
whoever holds them: the keyboard, a gamepad or the mouse.

| With the mouse | Does |
|---|---|
| Press a stick direction | Holds it |
| Press a button | Holds it |
| Press a switch | Presses it |

**Map** and **Defaults** change the controls (see
[Changing the controls](#changing-the-controls)). Switching to another window
lets go of everything the mouse holds. Keys typed in the Controllers window go
to the console.

# Machines

Preferences ▸ Console ▸ **TV system** picks the console:

- **Auto**: the NTSC console, or the PAL one when a PAL cartridge is opened.
- **NTSC**: the American console, at 60 frames a second.
- **PAL**: the European console, at 50 frames a second.

A change starts a new console when Preferences closes.

# BIOS and ROMs

Nothing is required. The fork adds a `none` BIOS to MAME's `a7800` and
`a7800p` that leaves MARIA on and the cartridge mapped, and the cartridge's
loader starts games itself with the console in the state the BIOS would leave
it. Atari's own BIOS and the High Score Cart ROM are copyrighted, so none
comes with the app.

| ROM | Is |
|---|---|
| The NTSC BIOS | the 7800's BIOS for the NTSC console |
| The PAL BIOS | the 7800's BIOS for the PAL console |
| The High Score Cart ROM | the High Score Cart |

**Import BIOS…** takes a file, a zip or a folder, and recognises each image by
its size and checksum (CRC-32), whatever it is called. Dropping the file on the
window does the same. With a BIOS imported, Preferences can choose it per
console, and the console then shows the Atari logo and starts the games the
BIOS accepts, exactly as on the hardware. Imported images are copied into the
app's `roms` folder (see [Files and folders](#files-and-folders)).

# Cartridges

A cartridge is a file: an `.a78` or `.bin`, or a `.zip` or `.7z` that holds
one. Every image (CONFIG, a game booted over the network, an opened cartridge
file) runs on the FujiNet cartridge's own mapper engine, up to 448K: plain
ROMs, SuperGame with and without RAM and POKEY, the 9-bank SuperGame,
Activision, Absolute and banked RAM. An image that needs a board the cartridge
does not emulate (XM, XBoard, Versaboard, Megacart) is refused with the reason.

A program that carries the `FUJI` claim keeps the cartridge's mailbox; a
commercial game closes it.

## Open Cartridge

**Open Cartridge…** (<kbd>Ctrl+O</kbd>) powers the console on with the image
staged in the cartridge, so it starts through the boot block, the loader and
the hand-over exactly as a network boot does. The cartridge is remembered and
starts again next time; **Eject Cartridge** or **Reboot to CONFIG** forgets it.
A PAL cartridge brings up the PAL console when the TV system is on Auto.
Dropping a file on the window opens a cartridge, imports a BIOS or High Score
Cart ROM, or copies anything else to the SD folder.

<!-- context: import-sd -->
## Import Cartridge to SD

**Import Cartridge to SD…** copies a cartridge into FujiNet's SD folder, so
CONFIG can list it and boot it through the cartridge. The cartridge refuses
what it cannot serve, and the message says why.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. The cartridge's
mailbox, boot block and loader, mapper engine and wire codec are the firmware's
own sources, and its mailbox service runs on its own thread, as on the
cartridge's second core. FujiNet listens; the cartridge dials in, and keeps
trying again if the link drops.

## CONFIG

CONFIG is FujiNet's program for choosing what to run, with a game lobby:

- **Hosts** are where games come from: the SD folder, and servers on the
  network.
- Pick a host and an image on it. FujiNet streams the `.a78` to the cartridge,
  the loader copies it in, and the game runs.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11511/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## The High Score Cart

Import its ROM and switch it on in Preferences ▸ Console. The cartridge
provides it, and games that keep high scores save them through FujiNet. It
takes effect with a new console.

## Turning it off

Preferences ▸ Host ▸ **Enable FujiNet** turns the FujiNet off. The cartridge
then reports *link down*, CONFIG runs without hosts, and nothing is dialled.
Games are not affected.

## An external FujiNet

Preferences ▸ Host ▸ **Use external FujiNet** skips the built-in FujiNet and
dials another FujiNet instance at the address you give, for example
`192.168.1.20:11510`. Leave the field empty for `127.0.0.1:11510`. Start that instance listening there (its `[BOIP]`
port). The cartridge dials out, so the other FujiNet can start before or after
it. FujiNet Web UI and Console Log belong to the built-in FujiNet, so they say
the FujiNet is external; use the other instance's own. The Debugger's FujiNet
tab still shows the traffic.

## Behind the scenes

- The cartridge and FujiNet talk over TCP port 11510 on 127.0.0.1 (the
  BoIP port), and the web UI is on port 11511. Nothing from outside can
  reach either.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the console. It is one page of groups.
The TV system, the BIOS and the High Score Cart start a new console when
Preferences closes; the picture, the controllers and the volume apply at once;
the Host settings apply by restarting the session. The settings are one file,
shared by the GNOME and KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu).

<!-- end -->

<!-- only: windows -->

On Windows this window is called **Settings** (in the Machine menu).

<!-- end -->

<!-- context: prefs/Machine -->
## Console settings

- **TV system**: *Auto*, *NTSC* or *PAL* (setting `region`).
- **BIOS (NTSC console)** and **BIOS (PAL console)**: which BIOS the console
  uses, the app's own none or the imported one (see
  [BIOS and ROMs](#bios-and-roms)).
- **High Score Cart**: switch the High Score Cart on. It needs its ROM
  imported (setting `hsc`).

## Display settings

- **Aspect**: *Television (4:3)* or *Square pixels* (setting `aspect`).
- **Smooth scaling**: blend the pixels when the picture is stretched
  (setting `smooth`).

## Controller settings

- **Player 1** and **Player 2**: what is plugged into each port: *Auto*,
  *ProLine Joystick*, *2600 Joystick*, *XG-1 Light Gun* or *None*. *Auto* plugs
  in the light gun for the light-gun games and a ProLine joystick otherwise.
  It applies at once, even to a game booted over FujiNet (settings
  `port0_type` and `port1_type`).
- **Left stick drives the joystick**: off, only a gamepad's D-pad moves it
  (setting `analog_joystick`).

## Gamepad settings

**Gamepads** lists each one plugged in, and which player it drives. A new
gamepad drives Player 1, the next Player 2; choose *Player 1* or *Player 2* to
fix it, or *Automatic*. The keys and buttons are changed in the Controllers
window (see [Changing the controls](#changing-the-controls)).

## Audio settings

- **Volume**: the output level, 0 to 100 (setting `volume`).

## Host settings

- **Enable FujiNet**: run the built-in FujiNet (setting `enable_fujinet`).
- **Use external FujiNet**: dial another instance instead (setting
  `fujinet_external`).
- **Address**: its `host:port`, or just a port (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).
- **Audio**: open the system audio device (setting `enable_audio`).
- **Gamepads**: poll USB and Bluetooth gamepads (setting `enable_gamepad`).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The other keys are `cart` (the opened
cartridge, empty for CONFIG), `left_diff` and `right_diff` (the difficulty
switches), and `bindings` (the controls that differ from the defaults).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has eight tabs; closing it lets the console go on as it was. The engine is
MAME's.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a JSR, to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Frame | On to the next frame |
| Run to Cursor | On to the line selected in the Disassembly |

<!-- context: debugger/console -->
## Console

MAME's own debugger commands: `bpset`, `wpset` with conditions, `print`,
`dump`, `find`, `trace` and the rest, with <kbd>Tab</kbd> completion, plus
three of this app's own:

| Command | Does |
|---|---|
| `cart` | The cartridge: link, mode, hand-over, load progress, mapper |
| `maria` | MARIA's control register and the display list |
| `labels` | Loads a symbol file |

<!-- context: debugger/cpu -->
## CPU and memory

The 6502's registers and flags, and an editable hex view of memory. RAM is
$1800 to $27FF (zero page and the stack mirror $2040 and $2140). Click a
value to edit it while stopped.

<!-- context: debugger/disassembly -->
## Disassembly

The 6502 disassembled at the PC, with the opcode bytes, labels and the
breakpoints in the margin. Click a line to select it, double-click or press
<kbd>F9</kbd> to toggle its breakpoint. Follow PC keeps the current line in
view. Labels come from ld65, ca65 or plain symbol files.

<!-- context: debugger/maria -->
## MARIA

MARIA's control register decoded, the display list and the display list list,
and the palettes: BACKGRND, then P0 to P7 (colours 1 to 3 each).

<!-- context: debugger/io -->
## I/O

INPTCTRL, the TIA's sound, the RIOT's ports, INPT0 to INPT5 and POKEY.

<!-- context: debugger/cart -->
## Cartridge

The cartridge: the link, the mode, the hand-over, the load progress, the
mapper, and the High Score Cart.

<!-- context: debugger/breakpoints -->
## Breakpoints and watchpoints

The list of breakpoints and watchpoints with their conditions. **Add**,
**Remove**, and the check box to turn one off.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet on the cartridge's link, both ways, with hex, traced from
power-on. **Clear** empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-atari7800/settings.ini` and
the data is in `~/.local/share/fujinet-go-atari7800` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-atari7800\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-atari7800`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-atari7800/settings.ini` and
the data is in `~/.local/share/fujinet-go-atari7800`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported BIOS and High Score Cart ROMs |
| `carts` | cartridges dropped on the window |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **link down**: FujiNet is off, not started yet, or the external address
  is wrong. CONFIG tries again; check Preferences ▸ Host.
- **The cartridge was refused**: the message says why: too big, empty, not a
  cartridge image, or a board the cartridge does not emulate (XM, XBoard,
  Versaboard, Megacart).
- **Import Cartridge to SD refused a game**: the FujiNet cartridge cannot
  serve it; open it with Open Cartridge instead.
- **The light gun does nothing**: the port must be on *Auto* or *XG-1 Light
  Gun*; aim with the mouse and press the left button.
- **Keys don't reach the console**: a dialog or another window is in front,
  or the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it may not be on the player you expect (see
  [Gamepads](#gamepads)), or its buttons may be given other jobs.
- **No sound**: the audio device could not be opened, or the volume is 0.
- **A port is taken**: another program is using 11510 or 11511. Close the
  other FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go Atari 7800 is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **MAME** (the Atari 7800 driver, GPL-2.0-or-later and BSD-3-Clause) is the
  emulator, built from the `tschak909/mame` fork's `fujinet-go-a7800` branch.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own sources are the firmware's.
- No BIOS or other copyrighted ROM is part of the app; you import your own.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-atari7800-desktop/issues)

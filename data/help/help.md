This is the text of FujiNet Go NES's built-in Help (Help ▸ FujiNet Go NES
Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to the
frontend it is about; here you see them all. Every topic below is one page in
the app.

# Getting started

FujiNet Go NES is a Nintendo Entertainment System with a FujiNet cartridge in
its slot. The console is MesenCE's. The FujiNet runs inside the app, so
CONFIG comes up at once and can load games from the network.

1. **Start the app.** The console powers on and runs FujiNet's CONFIG, which
   the cartridge's loader copies in, exactly as on the hardware. Nothing needs
   importing.
2. **Pick a game in CONFIG.** Choose a host (the SD folder or a server on the
   network) and a `.nes` image on it. FujiNet streams it to the cartridge, the
   loader copies it in, and the game runs. See [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…**
   (<kbd>Ctrl+O</kbd>) takes a `.nes` image. See [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window
   (<kbd>F9</kbd>) work the two controllers. See
   [Controllers and keys](#controllers-and-keys).
5. **Change the console.** [Preferences](#preferences) picks the region, the
   controllers, the expansion keyboard, the sound and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-nes-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…**, **Eject Cartridge** and **Import Cartridge to SD…**:
  see [Cartridges](#cartridges).
- **Reset Game** (<kbd>Backspace</kbd>), **Reset to CONFIG**
  (<kbd>Ctrl+R</kbd>, or <kbd>Escape</kbd>) and **Keyboard Mode**
  (<kbd>Scroll Lock</kbd>), and the **Data Recorder**'s Play Tape, Record Tape
  and Stop.
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
has the cartridge items, Reset to CONFIG, Preferences (Settings on Windows)
and Quit (Exit on Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences (called Settings) and Quit are in the app menu, and the
cartridge items, the resets, Keyboard Mode and the Data Recorder are in the
Machine menu. Keyboard Mode is also ⌘K, since a Mac has no Scroll Lock.

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

## Reset and power

- **Reset Game** (<kbd>Backspace</kbd>) is the console's RESET button. The
  cartridge has no reset line, so whatever it holds restarts.
- **Reset to CONFIG** (<kbd>Ctrl+R</kbd>, or <kbd>Escape</kbd>) is a power
  cycle of the cartridge: it ejects an opened cartridge and boots CONFIG
  through the loader again.
- **Eject Cartridge** does the same; CONFIG is the cartridge's own program.

## The picture

The console draws 256 × 240 pixels. **TV Aspect** shows them with the NES's 8:7
pixels, or square. **Smooth Scaling** blends the pixels when the picture is
stretched, and leaves them sharp when it is off.

# Controllers and keys

The NES has two controllers, each a D-pad, **A** and **B**, **Select** and
**Start**; the app adds **Turbo A** and **Turbo B**. The console's own button
is **Reset**.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Does | Player 1 | Player 2 |
|---|---|---|
| Up, down, left, right | The arrow keys | <kbd>I</kbd>, <kbd>K</kbd>, <kbd>J</kbd>, <kbd>L</kbd> |
| B | <kbd>Z</kbd> | <kbd>N</kbd> |
| A | <kbd>X</kbd> | <kbd>M</kbd> |
| Turbo B | <kbd>A</kbd> | |
| Turbo A | <kbd>S</kbd> | |
| Select | <kbd>Right Shift</kbd> | <kbd>U</kbd> |
| Start | <kbd>Return</kbd> | <kbd>O</kbd> |

| Console | Key |
|---|---|
| Reset Game | <kbd>Backspace</kbd> |
| Reset to CONFIG | <kbd>Escape</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+O</kbd> | Open Cartridge |
| <kbd>Ctrl+R</kbd> | Reset to CONFIG |
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
time. A pad plugged in while a game runs is assigned to the next free player
within a second, unplugging releases it, and a pad that comes back gets its
player back; each event is shown in the window. A pad with no standard layout
(a plain USB adapter) uses its raw buttons and hats. With **Left stick drives
the D-pad** on, the left stick does too.

## The expansion keyboard

The Famicom's expansion port takes a keyboard: Nintendo's **Family BASIC
Keyboard** (72 keys; its **Data Recorder** comes with it) or the **Subor
keyboard** (the Famiclone educational keyboard, 99 keys). Choose one in
Preferences ▸ Machine ▸ Expansion port. While one is attached, every key typed
on the host goes to the emulated keyboard, by position, not by legend, instead
of to the controllers; <kbd>Scroll Lock</kbd> switches between the two (**Keyboard
Mode** in the menu). Gamepads keep driving the controllers either way. The
Controllers window then draws the keyboard.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, then click a control, then press the key or the gamepad button to give
it. The control's old key or button is taken from it. **Defaults** puts every
control back. The map is kept in the settings, and only what differs from the
defaults is written.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) shows both NES controllers live and
presses their buttons with the mouse, and draws the expansion keyboard when
one is attached. It floats on its own at a fixed size. The controls that are
held light up in the accent colour, whoever holds them: the keyboard, a gamepad
or the mouse.

| With the mouse | Does |
|---|---|
| Press a button | Holds it for as long as the button is down |
| Press a key | Holds it |

The console samples its controls once a frame, so a button is *held*, not
pulsed. Switching to another window lets go of everything the mouse holds. Keys
typed in the Controllers window go to the console. **Map** and **Defaults**
change the controls (see [Changing the controls](#changing-the-controls)).

# Machines

Preferences ▸ Machine ▸ **Region** picks the console:

- **Auto**: follows the image's header. It takes effect at the next power
  cycle.
- **NTSC**: the American and Japanese console, at 60 frames a second.
- **PAL**: the European console, at 50 frames a second.
- **Dendy**: the PAL-region Famiclone, with NTSC-like game timing.

# Cartridges

A cartridge is a `.nes` file (or a `.fuji` application). Every image (CONFIG,
a game booted over the network, an opened cartridge file) runs on the FujiNet
cartridge's own mapper engine, so what works here works on the cartridge:
mappers 0, 1, 2, 3, 4, 7, 11, 30, 34, 66, 71 and 206, up to 512K PRG and 512K
CHR. Anything else is refused with the reason, as the cartridge refuses it.

- **A FujiNet app** carries the `FUJI` claim at `$FFF0`, and keeps the
  cartridge's mailbox live: it can talk to the network.
- **A game** is any other image; it closes the mailbox.

## Open Cartridge

**Open Cartridge…** (<kbd>Ctrl+O</kbd>) puts a local `.nes` straight into the
cartridge's SRAMs in place of CONFIG. It is remembered and starts again next
time; **Eject Cartridge** or **Reset to CONFIG** forgets it. Dropping a
cartridge file on the window does the same. A file the cartridge cannot take is
refused with the reason.

<!-- context: import-sd -->
## Import Cartridge to SD

**Import Cartridge to SD…** copies a cartridge into FujiNet's SD folder, so
CONFIG can list it and boot it through the cartridge. The cartridge refuses
what it cannot serve, and the message says why.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console: a 4K mailbox at $5000,
a 2K loader ROM at $5800, cart-served WRAM, and two 512K SRAMs banked by the
cartridge's mapper engine. The cartridge dials FujiNet over a local socket
(BoIP, FujiBus over TCP), and keeps trying again if the link drops.

## CONFIG

CONFIG is FujiNet's program for choosing what to run:

- **Hosts** are where games come from: the SD folder, and servers on the
  network.
- Pick a host and an image on it. FujiNet streams the `.nes` to the cartridge,
  the loader copies it in, and the game runs.
- **Reset to CONFIG** brings CONFIG back.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11507/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## Turning it off

Preferences ▸ Host ▸ **Enable FujiNet** turns the FujiNet off. The cartridge
then reports *link down*, CONFIG runs without hosts, and nothing is dialled.
Games are not affected.

## An external FujiNet

Preferences ▸ Host ▸ **Use external FujiNet** skips the built-in FujiNet and
dials another FujiNet instance at the address you give, for example
`192.168.1.20:11506`. Leave the field empty for `127.0.0.1:11506`. Start that
instance listening there (its `[BOIP]` port). FujiNet Web UI and Console Log
belong to the built-in FujiNet, so they say the FujiNet is external; use the
other instance's own. The Debugger's FujiNet tab still shows the traffic.

## Behind the scenes

- The cartridge and FujiNet talk over TCP port 11506 on 127.0.0.1 (the
  BoIP port), and the web UI is on port 11507. Nothing from outside can
  reach either.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the console. It is one page of groups.
The controllers, the expansion port and the volume apply at once, even to a
game booted over FujiNet; the region and the Host settings apply by restarting
the session when Preferences closes. The settings are one file, shared by the
GNOME and KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu).

<!-- end -->

<!-- only: windows -->

On Windows this window is called **Settings** (in the Machine menu).

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **Region**: *Auto*, *NTSC*, *PAL* or *Dendy* (setting `region`).
- **Aspect**: how the 256 × 240 picture is shaped: *Television (8:7
  pixels)* or *Square pixels* (setting `aspect`).
- **Smooth scaling**: blend the pixels when the picture is stretched (setting
  `smooth`).

## Controller settings

- **Player 1** and **Player 2**: what is plugged into each port: *NES
  Controller* or *None* (settings `port0_type` and `port1_type`).
- **Expansion port**: a keyboard on the Famicom expansion port: *None*,
  *Family BASIC Keyboard* or *Subor Keyboard* (setting `keyboard`; see
  [The expansion keyboard](#the-expansion-keyboard)).
- **Left stick drives the D-pad**: off, only a gamepad's D-pad moves it
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
Each setting above names its key. The others are `cart` (the opened cartridge,
empty for CONFIG) and `bindings` (the controls that differ from the defaults).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has twelve tabs; closing it lets the console go on as it was. The engine is
MesenCE's own, so it costs some speed while it is open.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a JSR, to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Scanline+1 | On to the next scanline |
| Frame+1 | On to the next frame |
| Run to Cursor | On to the line selected in the Disassembly |
| Pop Out | Moves the tab that shows into a window of its own; **Dock** puts it back |
| Help | Opens the topic of the tab that shows |

The 6502's registers and flags are in a strip under every tab: click one, type
a new value and press Enter while the console is stopped. The strip also shows
the cycle count, the scanline and dot, and the pending IRQ and NMI.

<!-- context: debugger/console -->
## Console

The Console takes commands, with completion on <kbd>Tab</kbd> and recall on
<kbd>Up</kbd> and <kbd>Down</kbd>. Numbers and conditions are Mesen
expressions: registers (`a`, `x`, `y`, `sp`, `pc`, `ps`), `[$addr]` for a
byte, `{$addr}` for a word, labels, `cycle`, `scanline` and `frame`. A
breakpoint's condition is such an expression; on a read or write it can use
`value`, `address` and `old`. `help` lists the commands.

| Command | Does |
|---|---|
| `step`, `over`, `out` | One instruction; over a JSR; to the return |
| `run`, `stop` | Go on, or break |
| `frame`, `scanline` | Run some frames or scanlines |
| `runto` | Runs to an address |
| `break` | An execute breakpoint |
| `bpr`, `bpw`, `bpa` | Data reads, writes, or either |
| `delete`, `enable`, `clear` | Manage breakpoints (also `disable`, `breaks`, `hits`) |
| `print`, `regs`, `set` | Evaluate; show or set the registers |
| `mem`, `poke`, `ppu` | Dumps memory; writes bytes; the PPU bus |
| `find`, `watch`, `unwatch` | Searches memory; keeps a watch list |
| `trace` | The instruction trace |
| `label`, `labels`, `symbols` | Name an address; load a file; list labels |
| `disasm` | Disassembles |
| `map`, `io`, `cart` | The memory map; the controllers and ports; the cartridge |
| `sound`, `video` | The APU's channels; the Video tab as text |
| `save` | Saves state to a file |

<!-- context: debugger/disassembly -->
## Disassembly

The 6502 disassembled at the PC, with the opcode bytes, labels and the
breakpoints in the margin (red when on, grey when off). Click a line to select
it; click its margin, double-click it, or press <kbd>F9</kbd> to toggle its
breakpoint. **Follow PC** keeps the current line in
view. Symbols load from ld65 (`-Ln` or VICE), ca65 `.dbg` and Mesen `.mlb`
files; the NES registers and the FujiNet mailbox are labelled out of the box.

<!-- context: debugger/memory -->
## Memory

A hex view of a memory space: the CPU's 64K as it sees it, or the PPU bus, OAM,
palette, PRG, CHR or WRAM. Type an address or a label in the address box and
press Enter. **Poke** writes a byte while the console is stopped. **Search**
finds hex bytes or text, and **Find Next** and **Find Previous** step through
the matches. Rows are named by region (RAM, PPU registers, the PRG window and
its bank).

<!-- context: debugger/breakpoints -->
## Breakpoints

Breakpoints stop the console on an execute, a read, a write, or either, in the
CPU's address space or the PPU's (nametables and CHR, the palette, OAM). Add
one with its type, its range and an optional condition (for example
`value != old`). Each one in the list can be turned off and on, shows how often
it was hit, and can be removed; **Clear Hits** zeroes the counts and **Clear
All** removes them all. The stop says what happened: the value written, and
what it was.

<!-- context: debugger/watches -->
## Watches

Memory you want to keep an eye on, with its live value while the console is
stopped. **Memory** shows it in the Memory tab, **Break on Change** stops the
console when it changes, and **Remove** drops it.

<!-- context: debugger/cart -->
## Cart and I/O

The cartridge (its link, the mailbox, the load progress and boot state), the
mapper with its PRG and CHR slots and the IRQ, the memory map (every range the
console answers in), and the controllers, keyboard and tape as the CPU reads
them.

<!-- context: debugger/trace -->
## Trace

Every instruction the 6502 runs, live: **Record** turns it on, each instruction
is kept with its registers, and **Clear** empties it. The log keeps only the
last 30000. The filter shows All, Branches (jumps, calls, returns and branches) or
a Range of addresses. Recording slows the console.

<!-- context: debugger/history -->
## History

The same record as the Trace tab, shown when the console stops: the
instructions that led there, oldest first.

<!-- context: debugger/video -->
## Video

The PPU, with the picture on the left and its sections on the right. The
debugger records the PPU registers for each line, so split screens and scrolling
show.

| View | Shows |
|---|---|
| Display | The last finished frame, as the screen showed it |
| Decoded | One source: Nametables, Patterns, Sprites or Palette |
| Beam | Stopped only: where the beam was in this frame |
| Values | The tables as hex |

**Zoom** goes from 1 to 16 times, or **Fit**. **Overlays** switch on a tile
grid, the sprites' boxes, the scroll window, sprite 0, the beam and a heat tint
for what was written lately.

Point at a pixel to see it in **Pixel**: the tile or sprite, its palette and
its addresses. Click to pin it, click again to let go. **Break on writes**
stops the console when the CPU writes that nametable, CHR or palette byte, or
the PPU register that changed there.

**Registers** shows the PPU registers with how long ago each changed,
**Colours** the palette and its use, **Lines** the screen bands where the scroll
or control changed, **Timing** the frame and line length, and **Sprites** the
OAM list.

<!-- context: debugger/sound -->
## Sound

The 2A03's channels, read while the console is stopped: the two pulse channels,
the triangle, the noise channel and the DMC, decoded into frequencies, volumes,
length counters, envelopes and sweeps, and the frame counter's status.

<!-- context: debugger/symbols -->
## Symbols

Every known symbol, the NES registers and the FujiNet mailbox, and the ones you
loaded. **Filter** narrows the list, and activating a row goes to that address.
**Load Symbols** reads ld65, ca65 and Mesen files; **Clear Symbols** forgets
them. **Break at** sets an execute breakpoint on the selected symbol.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet on the cartridge's link since the session started, both
ways, with hex; a repeated exchange counts ×N. **Clear** empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-nes/settings.ini` and
the data is in `~/.local/share/fujinet-go-nes` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-nes\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-nes`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-nes/settings.ini` and
the data is in `~/.local/share/fujinet-go-nes`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `carts` | cartridges dropped on the window |
| `tapes` | the Data Recorder's tape streams |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **link down**: FujiNet is off, not started yet, or the external address
  is wrong. CONFIG tries again; check Preferences ▸ Host.
- **The cartridge was refused**: the message says why: too big, empty, not a
  `.nes` image, or a mapper the cartridge does not emulate.
- **Import Cartridge to SD refused a game**: the FujiNet cartridge cannot
  serve it; open it with Open Cartridge instead.
- **The keys type on the keyboard, not the controller**: Keyboard Mode is on
  (<kbd>Scroll Lock</kbd>), because an expansion keyboard is attached.
- **Keys don't reach the console**: a dialog or another window is in front,
  or the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it may not be on the player you expect (see
  [Gamepads](#gamepads)), or its buttons may be given other jobs.
- **No sound**: the audio device could not be opened, or the volume is 0.
- **A port is taken**: another program is using 11506 or 11507. Close the
  other FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go NES is free software under the GNU General Public License, version
3 or later. Copyright © 2026 Thomas Cherryhomes.

- **MesenCE** (GPL-3.0-or-later) is the emulator, with its C++ core compiled
  as a library.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own sources are the firmware's.
- No copyrighted ROM is part of the app.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-nes-desktop/issues)

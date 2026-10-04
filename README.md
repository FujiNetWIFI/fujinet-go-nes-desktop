# FujiNet Go — NES

A self-contained Nintendo Entertainment System with a built-in
[FujiNet](https://fujinet.online/): power on into the FujiNet CONFIG client,
browse a network host from the console, boot `.nes` images over the network,
and let a FujiNet-aware program keep talking to the network — all in one
desktop app. A member of the FujiNet Go desktop family
(`fujinet-go-adam-desktop`, `-apple2-`, `-coco-`, `-msx-`, `-intv-`,
`-astrocade-`, `-coleco-`, `-atari2600-`).

| | |
|---|---|
| **Emulator** | [MesenCE](https://github.com/FujiNetWIFI/MesenCE) (branch `add-fujinet-support`), GPL-3.0-or-later — its C++ core compiled as a library, with its C# UI and platform layers replaced by this app's own. |
| **Cartridge** | The FujiNet NES cartridge (`fujinet-firmware` `pico/nes`), modelled in the MesenCE fork as a board (`FujiNetCart`): the 4K mailbox at `$5000`, the 2K loader ROM at `$5800`, cart-served WRAM, two 512K SRAMs banked by the cartridge's own mapper engine. The protocol (`fujimail`), wire codec (`fujibus`) and mapper engine (`nesmap`) are the cartridge firmware's own sources, vendored verbatim. |
| **FujiNet** | The firmware's `RS232` PC target, built in-process as `libfujinet` and dialled by the cartridge over loopback (BoIP on **11506**, web admin on **11507**). |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/GDI) — each with the display, a Controllers window, Preferences, a live FujiNet console log, and a debugger. |
| **Packaging** | Per-frontend DEB/RPM/TGZ, two Flatpaks, a Windows (x86-64) zip and NSIS installer, and a macOS bundle for Apple Silicon (arm64), all through GitHub Actions. |

## What it is

- **CONFIG is the power-on program.** The CONFIG client (`fujinet-config`'s
  `nes/` build) is baked into the cartridge, exactly as on the hardware: at
  power-on the cartridge's reset vector points into its loader ROM, which
  copies CONFIG into the SRAMs slice by slice and jumps to it. From CONFIG,
  pick a host and an image; FujiNet streams the `.nes` file to the cartridge,
  the loader copies it in, and the game runs.
- **The cartridge, faithfully.** Every image — CONFIG, a network-booted game,
  a local cartridge file — runs on the cartridge's own `nesmap` engine, so
  what works here works on the cartridge: mappers 0, 1, 2, 3, 4, 7, 11, 30,
  34, 66, 71 and 206, up to 512K PRG and 512K CHR. Anything else is refused
  with the reason, as the cartridge refuses it. A program that carries the
  `FUJI` claim at `$FFF0` keeps the mailbox; a commercial game closes it.
- **Open Cartridge…** puts a local `.nes` straight into the cartridge's SRAMs
  in place of CONFIG (and remembers it for the next start). **Import
  Cartridge to SD…** copies one into FujiNet's SD root, where CONFIG lists it.
  Dropping a file on the window does the first.
- **Reset Game** (Backspace) is the console's RESET button: the cartridge has
  no reset line, so whatever it holds restarts. **Reset to CONFIG** (Escape)
  is a power cycle of the cartridge: it ejects an opened cartridge and boots
  CONFIG through the loader again.
- **The debugger, in every frontend.** F12 opens it and stops the machine:
  Run/Stop (F5), Step (F7), Step Over (F8), Step Out (Shift+F8), Scanline+1,
  Frame+1. Tabs: a Prompt (`help`, `break`, `bpr`/`bpw` with Mesen
  expressions as conditions, `print`, `mem`, `poke`, `ppu`, `label`,
  `labels`, `runto`, `disasm`, `cart` … with Tab completion), CPU & RAM
  (editable), Disassembly (click to toggle a breakpoint, follow PC, jump to a
  label), PPU (registers, nametables with the scroll window, pattern tables,
  sprites, palette, OAM), APU & Input, Breakpoints, and Cart (link, mailbox,
  load progress, mapper, PRG/CHR slots, IRQ). The engine is MesenCE's own;
  it loads ld65 (`-Ln`/VICE), ca65 `.dbg` and Mesen `.mlb` symbols, and the
  NES registers and the FujiNet mailbox are labelled out of the box.
- **Gamepads that come and go.** SDL3 hot-plug: a pad plugged in while a game
  runs is assigned to the next free player within a second, unplugging
  releases it, and a pad that comes back gets its player back; each event is
  shown in the window. Pads with no gamepad mapping fall back to raw buttons
  and hats. The **Controllers** window shows both NES controllers live,
  presses buttons with the mouse, and rebinds any control to a key or
  gamepad button (Map).

The icon is the family mark in the console's own plastic: the dark grey of
the front band (`#3c3c3c`) on the light grey of the shell (`#c6c6c6`). The
UI accent is the NES logo's red (`#c4001e`), marking held buttons, the Map
target, the current line in the disassembly and the Run button while
stopped.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Both dependencies (MesenCE and fujinet-firmware) are cloned at their pinned
commits by the configure step — a plain `git clone` with no
`--recurse-submodules` is enough. To develop against working checkouts:

```sh
cmake -B build -DMESEN_SRC=~/Workspace/MesenCE \
               -DFUJINET_SRC=~/Workspace/fujinet-firmware
```

MesenCE is staged into `<build>/mesen-generated`; a dirty checkout re-stages
automatically, and `-DMESEN_RESTAGE=ON` forces it. `-DFRONTEND=none` builds
just the core and its tests; `-DWITH_FUJINET=OFF` skips the firmware build
(the machine boots CONFIG reporting the link down).

MesenCE is C++17. The macOS bundle needs macOS 13.3 or later.

The `netboot` test boots an image over the network end to end with the
cartridge bring-up's own test clients; point it at them to run it:

```sh
NES_TESTROM_DIR=~/Workspace/fujinet-firmware/pico/nes/build ctest --test-dir build -R netboot
```

### Cross-building Windows on Linux

```sh
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz
tar xzf SDL3-devel-3.4.12-mingw.tar.gz
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
      -DFRONTEND=windows -DWITH_FUJINET=OFF \
      -DCMAKE_PREFIX_PATH="$PWD/SDL3-3.4.12/x86_64-w64-mingw32"
cmake --build build-win
cp SDL3-3.4.12/x86_64-w64-mingw32/bin/SDL3.dll build-win/frontends/windows/
wine build-win/frontends/windows/fujinet-go-nes-windows.exe
```

This is the desk-side check (the core tests pass under Wine). `fujinet.dll`
does not cross-build — the firmware's PC target needs mingw builds of zlib
and libssh's dependencies — so the release build is native MSYS2/UCRT64
(see below), which builds it. MesenCE needed a few MinGW portability fixes;
they are in the fork, and `build-aux/windows/mesen-mingw.patch` records them.

## Ports

FujiNet's BoIP listener is on **11506** and its web admin UI on **11507** —
high ports of this app's own, continuing the family's table (Astrocade
11500/11501, ColecoVision 11502/11503, Atari 2600 11504/11505), so a
standalone `fujinet-pc` or a sibling app never collides.

FujiNet listens and the cartridge dials in, so the session starts FujiNet
first and waits for its listener before powering on. The listener's backlog
is one, so every power cycle (Open, Eject, Reset to CONFIG) closes the old
cartridge's link before the new cartridge dials. The cartridge's mailbox
service runs on its own thread, as it runs on the cartridge's second core, so
a slow network transaction never stalls the picture.

## Keys

| | |
|---|---|
| Player 1 | arrows; **Z** = B, **X** = A, **A** / **S** = turbo B / A, **Right Shift** = Select, **Return** = Start |
| Player 2 | **I J K L**; **N** = B, **M** = A, **U** = Select, **O** = Start |
| Gamepads | D-pad / left stick; South = B, East = A, West / North = turbo B / A, Back = Select, Start = Start |
| Reset Game / Reset to CONFIG | **Backspace** / **Escape** (Ctrl+R on the menu) |
| Fullscreen / Debugger | F11 / F12 (debugger: F5 run/stop, F7 step, F8 step over, Shift+F8 step out) |

Every control is remappable in the Controllers window.

## How this was verified

- **The core on Linux** — `ctest` covers the session (CONFIG boots through
  the loader and paints; a cartridge opens, runs, survives Reset Game and is
  ejected by Reset to CONFIG; an unsupported mapper is refused with the
  reason; settings and the open cartridge persist), bindings, media routing,
  gamepads (pure functions plus an SDL start/stop with no hardware), the
  debugger contract (attach stops, step/over/out, disassembly with the PC
  line, execute and write breakpoints that hit and survive a power cycle,
  memory edits that reach RAM and the PRG SRAM, the PPU views, the prompt),
  the FujiNet link (`fujibus_smoke`), and a full network boot (`netboot`:
  MOUNT_HOST, SET_DEVICE_FULLPATH, MOUNT_IMAGE, the DBC push, the loader's
  32 slices, the new image running) against the in-process FujiNet.
- **The whole flow, driven like a user** — CONFIG lists the SD host's files
  from the in-process FujiNet; Down, A on `hello.nes`; FujiNet pushes it, the
  loader copies it in, and it runs.
- **GNOME and KDE** — built together with no frontend warnings, desktop and
  metainfo files validated, and smoke-launched headless (GTK Broadway, Qt
  offscreen) with the debugger, Controllers and Preferences open.
- **Windows** — cross-built with mingw-w64; the session, debugger, bindings,
  media and gamepad tests pass under Wine and the app runs there with every
  window open. Not yet looked at on a real Windows desktop.
- **macOS** — source-complete, compiled only by CI on Apple Silicon, as
  every macOS frontend in the family was.

## Cutting a release

Pushing a `v*` tag builds every platform and, only if all of it passes,
publishes what it produced as a **draft** release:

| Asset | Contents |
|---|---|
| `fujinet-go-nes-gnome-<version>-Linux.{deb,rpm,tar.gz}` | the GNOME frontend, packaged with CPack |
| `fujinet-go-nes-kde-<version>-Linux.{deb,rpm,tar.gz}` | the KDE frontend, packaged with CPack |
| `fujinet-go-nes-<version>-windows.zip` | the exe, `fujinet.dll`, and the `fujinet/` runtime tree |
| `fujinet-go-nes-<version>-windows-setup.exe` | NSIS installer, per-user, no admin rights |
| `fujinet-go-nes-<version>-macos-arm64.zip` | the `.app` bundle for Apple Silicon, FujiNet inside |
| `online.fujinet.go.nes.{gnome,kde}.flatpak` | single-file bundles: `flatpak install ./…flatpak` |

The version is declared in the tree (`project(… VERSION …)` and both
metainfo files, which template it), not derived from the tag; `check-version`
stops the release if they disagree. To release 0.2.0: set the version in
`CMakeLists.txt`, add a `<release>` entry with its date to both
`frontends/*/data/*.metainfo.xml.in`, commit, then
`git tag -a v0.2.0 && git push origin v0.2.0`.

The Windows release build is native MSYS2/UCRT64, and the `release.yml` job
checks the exe's and `fujinet.dll`'s import tables against a system-DLL
whitelist. The macOS job signs and notarises when the `MACOS_*` secrets are
set, and checks the bundle for leaked Homebrew dylibs.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.

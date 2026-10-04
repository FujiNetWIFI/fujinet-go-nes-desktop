# Compliance

Per-component provenance for `fujinet-go-nes-desktop`, written before
the first public build, in the family tradition (see
`fujinet-go-adam-desktop/COMPLIANCE.md`, `fujinet-go-coleco-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **MesenCE** (the emulator core: 2A03 CPU and APU, 2C02 PPU, the debugger engine, its expression evaluator, disassembler and label manager) | [`FujiNetWIFI/MesenCE`](https://github.com/FujiNetWIFI/MesenCE), branch `add-fujinet-support` — a fork of MesenCE, itself a continuation of [Mesen 2](https://github.com/SourMesen/Mesen2) | GPL-3.0-or-later, © Sour and the MesenCE contributors | pinned in `cmake/Dependencies.cmake`; its `Core/`, `Utilities/`, `SevenZip/` and `Lua/` trees staged into `<build>/mesen-generated/` by `cmake/StageMesen.cmake` and compiled as the `mesen_core` static library |
| **The FujiNet NES cartridge in MesenCE** (`Core/NES/Mappers/Homebrew/FujiNetCart`, `FujiNetLink`, `FujiNetCartLoader`) | the same fork; a port of the cartridge's MAME device (`fujinet-firmware` `pico/nes/emu/fujinet.cpp`) | GPL-3.0-or-later, © Thomas Cherryhomes | part of `mesen_core` |
| **The cartridge firmware's protocol sources** (`fujimail`, `fujibus`, `nesmap`, `nes_cart.h`, `fuji_mailbox.h`, the loader ROM `nesloaderrom.h`) | `fujinet-firmware` `pico/nes/firmware`, vendored verbatim into `Core/NES/Mappers/Homebrew/FujiNet/` by its `sync.sh` (provenance in `FujiNet/SOURCES`) | as marked in each file (the FujiNet project's, © Thomas Cherryhomes) | compiled as C++ through the `FujiNet_*.cpp` wrappers, as the MAME device does |
| **The CONFIG client** (`fujiconfigrom.h`, 40 976 bytes) | [`FujiNetWIFI/fujinet-config`](https://github.com/FujiNetWIFI/fujinet-config) `nes/`, built with cc65 and `fujinet-lib`'s `nes` target, converted by `pico/nes/tools/mkromh.py` | GPL-3.0 (the FujiNet project's) | an iNES image embedded in the cartridge, as on the hardware |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| Lua 5.5 and luasocket (`Lua/`) | bundled by MesenCE | MIT | compiled into `mesen_core` (MesenCE's debugger references its script engine) |
| 7-Zip SDK (`SevenZip/`) | bundled by MesenCE | public domain | compiled into `mesen_core` (MesenCE's archive reader) |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically on macOS and Windows |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet`: system package where it is a usable 3.x, otherwise the pinned source |

Everything above is GPL-3.0-or-later or compatible with it: the combined work
is distributed under GPL-3.0.

## What is changed in MesenCE

The fork's `add-fujinet-support` branch adds the FujiNet cartridge and a few
small, upstreamable changes; this application then compiles the core from
those sources with its own flags and **does not patch or override anything**
at staging time:

- **The FujiNet cartridge** — new files only (`FujiNetCart`, `FujiNetLink`,
  `FujiNetCartLoader`, the vendored `FujiNet/` directory), plus three hooks:
  a pseudo mapper ID (`MapperFactory::FujiNetCartMapperID`, 65531), its
  `case` in `MapperFactory.cpp`, and the branch in `RomLoader.cpp` that loads
  every iNES image onto the cartridge while a host has enabled it
  (`FujiNetCart::SetHostConfig`). With the switch off — which is how
  MesenCE's own UI runs — nothing changes.
- **`Breakpoint::Init`** — a public initializer, so a native host can build
  breakpoints in C++ (MesenCE's C# UI fills them by memory layout).
- **`Emulator::Run`** — no longer clears `_stopFlag` as the emulation thread
  starts. `InternalLoadRom` clears it before starting the thread; clearing it
  again swallowed a `Stop()` that arrived before the thread ran, and `Stop()`
  then joined a thread that never ended. MesenCE's UI never stops a console
  that fast; this app's tests do.
- **`ITapeRecorder::IsPlaying()`** — a default `false` beside
  `IsRecording()`, overridden by `FamilyBasicDataRecorder`, so a host can
  show the Data Recorder's state.
- **MinGW portability** (Windows) — see `build-aux/windows/mesen-mingw.patch`
  and the fork's history: MesenCE is built with MSVC upstream; the fixes let
  MSYS2 UCRT64 and mingw-w64 build it, guarded so MSVC, Linux and macOS
  builds are unaffected.

## System ROMs — there are none

The NES has **no BIOS**: the console boots straight into the cartridge. The
only built-in images are FujiNet's own: the CONFIG client and the
cartridge's 2K loader ROM, both FujiNet-project code with the firmware's
licence, so there is nothing copyrighted-by-a-third-party to redistribute or
to leave out, and unlike the ADAM, ColecoVision, Astrocade and MSX ports this
one needs no ROM import step and no `no_embedded_roms` test. Game cartridges
are the user's own: opened from a local file, or served by FujiNet from the
SD folder or a network host. The tests write the tiny NROM images they need
themselves (`core/tests/test_rom.h`).

## Deliberately not used

- **MesenCE's own UI and platform layers** (`UI/`, `InteropDLL/`, `Linux/`,
  `MacOS/`, `Windows/`, `Sdl/`) — the four native frontends and the host
  classes in `core/mesen/` replace them; they are not staged.
- **MesenCE's own mappers for game images** — every image runs on the
  cartridge's `nesmap`, as on the hardware. MesenCE's mapper library is
  compiled (the core references it) but never selected while the cartridge
  is enabled.
- **MesenCE's save states, rewind and run-ahead** — off: they would replay
  or roll back mailbox transactions that FujiNet has already acted on.

## Icon and name

The icon is the FujiNet Go family mark (the same artwork as the other
desktops) recoloured in the NES console's plastic greys, `#3c3c3c` on
`#c6c6c6` (`tools/icons/make-icons.py`).
"Nintendo", "Nintendo Entertainment System", "NES" and "Famicom" are
trademarks of Nintendo; they are used here to name the machine being
emulated, and the project is not affiliated with or endorsed by Nintendo.

## Trademarks and names

"FujiNet" is the FujiNet project's name. "Mesen" is Sour's. Neither project
endorses this application; both are credited in the About box of every
frontend.

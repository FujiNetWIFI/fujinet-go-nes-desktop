# ctest suites. Each is a small program; the ones that need a running
# machine boot FujiNet's CONFIG client, which is always available (the
# MesenCE fork's FujiNet cartridge embeds it), or a tiny NROM image the test
# writes itself (test_rom.h), so nothing here ever skips for want of a ROM.

# The FujiNet tab's packet decoder, pure: synthetic SLIP bytes both ways,
# split reads, repeats folding into xN, the ring's cap. Needs nothing but
# its own source, so it builds and runs everywhere.
add_executable(fujitrace_test fujitrace_test.cpp ../src/fuji_trace.cpp)
target_include_directories(fujitrace_test PRIVATE
  "${CMAKE_SOURCE_DIR}/core/include" "${CMAKE_SOURCE_DIR}/core/src")
set_target_properties(fujitrace_test PROPERTIES CXX_STANDARD 17)
add_test(NAME fujitrace COMMAND fujitrace_test)

# ---- VIDEO agent: the Video tab ---------------------------------------------
# vidtab_test: the PPU register model, the Decoded renderers and the tab
# controller on made-up captures plus a fuzz. vidtab_stubs.c stands in for the
# machine and the debugger (and records the breakpoints the tab adds), so it
# needs no emulator, no SDL and no ROM, and builds on every toolchain. The
# NES_DEBUGGER_VIDEO_* variables are read when a tab is created: the second
# test sets them (no setenv in the test: MinGW-safe).
add_executable(vidtab_test vidtab_test.c vidtab_stubs.c
  ../src/vidtab.c ../src/ppuview.c ../src/ppumodel.c)
target_include_directories(vidtab_test PRIVATE
  "${CMAKE_SOURCE_DIR}/core/include" "${CMAKE_SOURCE_DIR}/core/src")
set_target_properties(vidtab_test PROPERTIES C_STANDARD 11)
add_test(NAME vidtab COMMAND vidtab_test)
set_tests_properties(vidtab PROPERTIES TIMEOUT 120)
add_test(NAME vidtab_env COMMAND vidtab_test env)
set_tests_properties(vidtab_env PROPERTIES TIMEOUT 30
  ENVIRONMENT "NES_DEBUGGER_VIDEO_VIEW=beam;NES_DEBUGGER_VIDEO_SOURCE=palette;NES_DEBUGGER_VIDEO_OVERLAYS=grid,scroll;NES_DEBUGGER_VIDEO_ZOOM=4;NES_DEBUGGER_VIDEO_SECTION=timing")
if(WIN32)
  target_link_options(vidtab_test PRIVATE -static -static-libgcc)
endif()
# ---- end VIDEO agent ---------------------------------------------------------

# These need the session layer, which needs SDL3. On a build without it
# (a cross-toolchain, or a lean machine) only the core libraries build.
if(TARGET nes_session)

# The frontend contract, against a real temporary XDG tree: settings persist,
# paths resolve, CONFIG boots through the cartridge's loader and paints, a
# cartridge opens, Reset Game keeps it, Reset to CONFIG ejects it, and an
# image the cartridge cannot map is refused with a reason.
add_executable(session_test session_test.c)
target_link_libraries(session_test PRIVATE nes_session)
add_test(NAME session COMMAND session_test)
set_tests_properties(session PROPERTIES TIMEOUT 120)

# The expansion-port keyboards: a ROM scans the matrix the way Family BASIC
# does; host keys (keyboard mode), the on-screen keyboard, Scroll Lock, the
# Subor, the layout tables and the Data Recorder.
add_executable(keyboard_test keyboard_test.c)
target_link_libraries(keyboard_test PRIVATE nes_session)
add_test(NAME keyboard COMMAND keyboard_test)
set_tests_properties(keyboard PROPERTIES TIMEOUT 120)

# The remappable binding table and the keyboard translator, pure.
add_executable(bindings_test bindings_test.c)
target_link_libraries(bindings_test PRIVATE nes_session)
add_test(NAME bindings COMMAND bindings_test)

# Where a dropped file goes.
add_executable(media_test media_test.c)
target_link_libraries(media_test PRIVATE nes_session)
add_test(NAME media COMMAND media_test)

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up, a transaction
# completed and the debugger's FujiNet trace saw a request and a reply.
# SKIPs (77) when the runtime was not built (-DWITH_FUJINET=OFF).
# Serial: it owns the port.
add_executable(fujibus_smoke fujibus_smoke.c)
target_link_libraries(fujibus_smoke PRIVATE nes_session)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# A network boot end to end: the cartridge bring-up's fujiboot client mounts
# hello.nes from the SD host, FujiNet pushes it, the loader copies it in and it
# runs. Needs NES_TESTROM_DIR (fujinet-firmware pico/nes/build); SKIPs without.
add_executable(netboot_test netboot_test.c)
target_link_libraries(netboot_test PRIVATE nes_session)
add_test(NAME netboot COMMAND netboot_test)
set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)

# The external-FujiNet address field, pure.
add_executable(hostport_test hostport_test.c)
target_link_libraries(hostport_test PRIVATE nes_session)

# The built-in Help covers every setting, key and context.
add_executable(help_test help_test.c)
target_link_libraries(help_test PRIVATE nes_session)
add_test(NAME help COMMAND help_test)
add_test(NAME hostport COMMAND hostport_test)

# "Use external FujiNet" end to end: a bad address refuses, a good one is
# dialled by the cartridge (a stand-in listener sees it), no runtime starts.
add_executable(external_test external_test.c)
target_link_libraries(external_test PRIVATE nes_session)
add_test(NAME external COMMAND external_test)
set_tests_properties(external PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# The gamepad layer's pure helpers, and that the SDL subsystem starts and
# stops with no hardware attached.
add_executable(gamepad_test gamepad_test.c)
target_link_libraries(gamepad_test PRIVATE nes_session)
target_include_directories(gamepad_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME gamepad COMMAND gamepad_test)

# The debugger contract: attach stops the machine, registers, step / over /
# out, disassembly with the PC line, breakpoints that hit, memory, the PPU
# views, the cartridge and FujiNet tabs, the prompt, detach resumes.
add_executable(debug_test debug_test.c)
target_link_libraries(debug_test PRIVATE nes_session)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 180)

# ---- ENGINE agent: the newer debugger engine's tests ----------------------
# The pure tools (range and search parsing, watches, breakpoint text, region
# names, trace row text and filters); the engine itself runs in debug_test
# against a program that writes the PPU, palette, OAM and RAM in a loop.
add_executable(memtools_test memtools_test.c)
target_link_libraries(memtools_test PRIVATE nes_session)
add_test(NAME memtools COMMAND memtools_test)
# The condition text helpers (`old`, the syntax check), pure.
add_executable(expr_test expr_test.cpp)
target_link_libraries(expr_test PRIVATE nes_session)
target_include_directories(expr_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
set_target_properties(expr_test PROPERTIES CXX_STANDARD 17)
add_test(NAME expr COMMAND expr_test)
# ---- end ENGINE agent ------------------------------------------------------

# ---- VIDEO agent: the Video tab on a real machine ---------------------------
# vidtab_live_test: a small program that splits the scroll mid-frame; the
# capture sees the frame, the PPU's memory and the split, the replayed model
# agrees with the PPU every frame, a parked machine is read live, Display is
# the published frame, Break on writes lands in the engine. Needs no ROM file.
add_executable(vidtab_live_test vidtab_live_test.c)
target_link_libraries(vidtab_live_test PRIVATE nes_session)
target_include_directories(vidtab_live_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME vidtab_live COMMAND vidtab_live_test)
set_tests_properties(vidtab_live PROPERTIES TIMEOUT 120)
# ---- end VIDEO agent ---------------------------------------------------------

endif()

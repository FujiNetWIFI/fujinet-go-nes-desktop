# ctest suites. Each is a small program; the ones that need a running
# machine boot FujiNet's CONFIG client, which is always available (the
# MesenCE fork's FujiNet cartridge embeds it), or a tiny NROM image the test
# writes itself (test_rom.h), so nothing here ever skips for want of a ROM.

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
# CONFIG client and confirms the cartridge's link came up and a transaction
# completed. SKIPs (77) when the runtime was not built (-DWITH_FUJINET=OFF).
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

# The gamepad layer's pure helpers, and that the SDL subsystem starts and
# stops with no hardware attached.
add_executable(gamepad_test gamepad_test.c)
target_link_libraries(gamepad_test PRIVATE nes_session)
target_include_directories(gamepad_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME gamepad COMMAND gamepad_test)

# The debugger contract: attach stops the machine, registers, step / over /
# out, disassembly with the PC line, breakpoints that hit, memory, the PPU
# views, the cartridge tab, the prompt, detach resumes.
add_executable(debug_test debug_test.c)
target_link_libraries(debug_test PRIVATE nes_session)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)

endif()

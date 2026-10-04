# The emulator core (MesenCE, staged) and the FujiNet Go host layer.
#
# mesen_core: MesenCE's Core/ (every system: the Emulator and the Debugger
# reference all of them), Utilities/, SevenZip/ and Lua/, compiled from the
# staged tree with this project's flags, plus the host classes in mesen/
# that take the place of Mesen's Linux/MacOS/Windows/Sdl layers and its C#
# UI. No SDL here: the session owns audio and gamepads.

set(MESEN_SRC_DIR "${MESEN_GEN}")

function(mesen_glob out)
  set(_files)
  foreach(_pat IN LISTS ARGN)
    file(GLOB_RECURSE _g CONFIGURE_DEPENDS "${MESEN_SRC_DIR}/${_pat}")
    list(APPEND _files ${_g})
  endforeach()
  set(${out} ${_files} PARENT_SCOPE)
endfunction()

# Mesen's makefile compiles exactly these: Core/**/*.cpp (the vendored
# FujiNet firmware .c files are compiled through their FujiNet_*.cpp
# wrappers, never directly), Utilities/**/*.cpp|*.c, SevenZip/*.c, Lua/*.c.
mesen_glob(MESEN_CORE_SOURCES Core/*.cpp)
mesen_glob(MESEN_UTIL_SOURCES Utilities/*.cpp Utilities/*.c)
mesen_glob(MESEN_7Z_SOURCES SevenZip/*.c)
mesen_glob(MESEN_LUA_SOURCES Lua/*.c)

if(WIN32)
  # luasocket's Unix transports; Mesen's Lua.vcxproj leaves them out too
  list(FILTER MESEN_LUA_SOURCES EXCLUDE REGEX "/(unix|unixdgram|unixstream|unixstream|serial|usocket)\\.c$")
else()
  list(FILTER MESEN_LUA_SOURCES EXCLUDE REGEX "/wsocket\\.c$")
endif()

set(FNGO_HOST_SOURCES
  mesen/MesenHost.cpp)

add_library(mesen_core STATIC
  ${MESEN_CORE_SOURCES} ${MESEN_UTIL_SOURCES} ${MESEN_7Z_SOURCES}
  ${MESEN_LUA_SOURCES} ${FNGO_HOST_SOURCES})

# Mesen is C++17 (its makefile's --std=c++17); newer standards trip over its
# use of std::codecvt and u8path.
set_target_properties(mesen_core PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)

# The same include roots as Mesen's makefile, in its order: "pch.h" from
# Utilities/ resolves to Core/pch.h.
target_include_directories(mesen_core PUBLIC
  "${MESEN_SRC_DIR}"
  "${MESEN_SRC_DIR}/Core"
  "${MESEN_SRC_DIR}/Utilities"
  "${CMAKE_CURRENT_SOURCE_DIR}/mesen")

# Release-like regardless of CMAKE_BUILD_TYPE: Mesen's DEBUG/_DEBUG turn on
# thread asserts and range throws meant for its own developers.
target_compile_definitions(mesen_core PRIVATE NDEBUG)
target_compile_options(mesen_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
# Our own host files get warnings.
set_source_files_properties(${FNGO_HOST_SOURCES} PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wno-unknown-pragmas>")

find_package(Threads REQUIRED)
target_link_libraries(mesen_core PUBLIC Threads::Threads)
if(WIN32)
  # PRIVATE: Mesen's own sources want the wide Win32 API and a lean windows.h;
  # the C frontends and the session must not inherit either.
  target_compile_definitions(mesen_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(mesen_core PUBLIC ws2_32 winmm ole32 oleaut32 iphlpapi shell32 shlwapi)
elseif(APPLE)
  target_link_libraries(mesen_core PUBLIC "-framework Foundation")
else()
  target_link_libraries(mesen_core PUBLIC ${CMAKE_DL_LIBS})
endif()


# ---- nes_session: the frontend contract ----------------------------------
#
# A separate target from mesen_core so the core stays SDL-free: audio_sdl.c
# and gamepad_sdl.c live here, and host_test links mesen_core alone to keep
# that honest. Found-or-skipped, exactly as the frontends are: mesen_core is
# SDL-free by construction, so `-DFRONTEND=none` on a machine (or a
# cross-toolchain) with no SDL3 must still configure and build the emulator
# and its tests.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  message(STATUS "nes_session: SDL3 not found -- building the core and its "
                 "tests only (no audio, no gamepads, no frontends)")
  add_subdirectory(tests)
  return()
endif()

set(NES_SESSION_C_SOURCES
  src/settings.c
  src/paths.c
  src/media.c
  src/fujinet_runtime.c
  src/audio_sdl.c
  src/bindings.c
  src/hid_keys.c)
set(NES_SESSION_CXX_SOURCES
  src/session.cpp
  src/debug.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gamepad_sdl.c")
  list(APPEND NES_SESSION_C_SOURCES src/gamepad_sdl.c)
else()
  set(NES_GAMEPAD_STUB ON)
endif()

add_library(nes_session STATIC
  ${NES_SESSION_C_SOURCES} ${NES_SESSION_CXX_SOURCES})
set_target_properties(nes_session PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(nes_session PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(nes_session PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(nes_session PUBLIC mesen_core SDL3::SDL3)
set_target_properties(nes_session PROPERTIES CXX_STANDARD 17)
if(NES_GAMEPAD_STUB)
  target_compile_definitions(nes_session PRIVATE NES_GAMEPAD_STUB=1)
endif()
# setenv()/getaddrinfo() under a strict -std=c11
target_compile_definitions(nes_session PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
set_source_files_properties(${NES_SESSION_C_SOURCES} ${NES_SESSION_CXX_SOURCES}
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# Where an uninstalled build and an installed copy find the FujiNet runtime
# (core/src/paths.c probes these after the executable's own directory).
target_compile_definitions(nes_session PRIVATE
  NES_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  NES_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-nes"
  NES_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-nes")

add_subdirectory(tests)

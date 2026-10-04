# Provide the MesenCE checkout (see cmake/Dependencies.cmake) and stage the
# parts this app compiles into <build>/mesen-generated, which
# core/CMakeLists.txt builds into the mesen_core static library. Staged per
# build tree (not in the source tree, as the siblings do) so a GNOME, a KDE
# and a cross-compiled Windows build of one checkout never re-stage under
# each other; -DMESEN_GEN=<dir> puts it elsewhere.
#
# MesenCE is built here the way every sibling app builds its emulator: the
# sources are copied and compiled as if they were ours, with our own flags,
# rather than driving Mesen's makefile (which builds a shared library for its
# C# UI). Nothing is patched and nothing is overridden: Mesen's platform code
# plugs in through small interfaces (IRenderingDevice, IAudioDevice,
# IKeyManager, INotificationListener, IMessageManager), which core/mesen/
# implements, so its Linux/, MacOS/, Windows/, Sdl/, InteropDLL/ and UI/
# trees are simply not staged.
#
# What IS staged: Core/ (every system -- the Emulator and the Debugger
# reference all of them, so the NES cannot be compiled alone), Utilities/,
# SevenZip/ (Utilities' archive reader) and Lua/ (the debugger's script
# engine). Build products and Visual Studio project files are left behind.
#
# Staging is automatic: it runs when the staged tree is missing, when the
# checkout's HEAD or dirtiness has changed, when this file changes, or on
# demand with -DMESEN_RESTAGE=ON (which is also how to pick up further
# uncommitted edits in a working checkout pointed at by MESEN_SRC).

set(MESEN_GEN "${CMAKE_BINARY_DIR}/mesen-generated" CACHE PATH
    "Where the MesenCE sources are staged")

option(MESEN_RESTAGE "Re-stage the MesenCE sources from the checkout" OFF)

nes_provide_dependency(
  NAME MesenCE
  PATH third_party/MesenCE
  URL "${MESEN_URL}"
  COMMIT "${MESEN_COMMIT}"
  SENTINEL Core/Shared/Emulator.cpp
  OVERRIDE MESEN_SRC
  RESULT MESEN_DIR)

# The FujiNet cartridge is what makes this MesenCE THE MesenCE for this app:
# refuse a checkout without it rather than building a plain emulator that
# cannot boot anything.
if(NOT EXISTS "${MESEN_DIR}/Core/NES/Mappers/Homebrew/FujiNetCart.cpp"
   OR NOT EXISTS "${MESEN_DIR}/Core/NES/Mappers/Homebrew/FujiNet/fujiconfigrom.h")
  message(FATAL_ERROR
    "${MESEN_DIR} has no Core/NES/Mappers/Homebrew/FujiNetCart.cpp or "
    "FujiNet/fujiconfigrom.h -- the pin must be on the add-fujinet-support "
    "branch of ${MESEN_URL}; see cmake/Dependencies.cmake.")
endif()

# What the staged tree was made from: the source identity, its dirtiness
# (a working checkout under development) and this file's own hash.
set(_mesen_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${MESEN_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _mesen_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  # Dirtiness by CONTENT, not by the list of dirty files: a second edit to a
  # file that is already modified must re-stage too. The tracked changes are
  # the diff against HEAD; untracked files are hashed one by one.
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${MESEN_DIR}" diff HEAD
            -- Core Utilities SevenZip Lua
    OUTPUT_VARIABLE _mesen_diff ERROR_QUIET)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${MESEN_DIR}" ls-files --others
            --exclude-standard -- Core Utilities SevenZip Lua
    OUTPUT_VARIABLE _mesen_untracked OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  set(_mesen_dirty "${_mesen_diff}")
  if(_mesen_untracked)
    string(REPLACE "\n" ";" _mesen_untracked "${_mesen_untracked}")
    foreach(_f IN LISTS _mesen_untracked)
      file(SHA256 "${MESEN_DIR}/${_f}" _h)
      string(APPEND _mesen_dirty "${_f} ${_h}\n")
    endforeach()
  endif()
  if(_mesen_dirty)
    string(SHA256 _mesen_dirty_hash "${_mesen_dirty}")
    set(_mesen_head "${_mesen_head}-dirty-${_mesen_dirty_hash}")
  endif()
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _stage_hash)
set(_mesen_want "${MESEN_DIR}\n${_mesen_head}\n${_stage_hash}\n")

set(_mesen_have "")
if(EXISTS "${MESEN_GEN}/.source-info")
  file(READ "${MESEN_GEN}/.source-info" _mesen_have)
endif()

if(MESEN_RESTAGE OR NOT _mesen_have STREQUAL _mesen_want
   OR NOT EXISTS "${MESEN_GEN}/Core/Shared/Emulator.cpp")
  message(STATUS "MesenCE: staging ${MESEN_DIR} -> ${MESEN_GEN}")
  file(REMOVE_RECURSE "${MESEN_GEN}")
  file(MAKE_DIRECTORY "${MESEN_GEN}")
  foreach(_dir Core Utilities SevenZip Lua)
    file(COPY "${MESEN_DIR}/${_dir}" DESTINATION "${MESEN_GEN}"
         PATTERN "*.o" EXCLUDE
         PATTERN "*.obj" EXCLUDE
         PATTERN "*.gch" EXCLUDE
         PATTERN "*.vcxproj" EXCLUDE
         PATTERN "*.vcxproj.filters" EXCLUDE
         PATTERN "*.ruleset" EXCLUDE
         PATTERN "obj.*" EXCLUDE)
  endforeach()
  file(WRITE "${MESEN_GEN}/.source-info" "${_mesen_want}")
endif()

# Everything that comes out of the staged tree is regenerated from the pin; a
# stale CMake cache must not keep an old file list alive across a re-stage.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MESEN_GEN}/.source-info")

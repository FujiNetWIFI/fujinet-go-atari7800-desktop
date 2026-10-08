# The FujiNet Go host layer over libmame_fngo (cmake/BuildMame.cmake).
#
# a7800_host: MameHost (core/mame), the bridge to MAME's thread, frames,
# audio and input. No SDL here: the session owns audio and gamepads, and
# the MAME-only tests link a7800_host alone to keep that honest.

add_library(a7800_host STATIC mame/MameHost.cpp)
set_target_properties(a7800_host PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(a7800_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/mame")
target_link_libraries(a7800_host PUBLIC mame_fngo)
add_dependencies(a7800_host mame-fngo)
find_package(Threads REQUIRED)
target_link_libraries(a7800_host PUBLIC Threads::Threads)
if(WIN32)
  # winmm: timeBeginPeriod for the pacing; ws2_32: the FujiNet runtime's
  # readiness probe (core/src/fujinet_runtime.c)
  target_link_libraries(a7800_host PUBLIC winmm ws2_32)
endif()
target_compile_options(a7800_host PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>)

# The headless runner: libmame_fngo with no window, for the boot tests and
# the ROM sweep (tools/romsweep). Not installed.
add_executable(a7800_headless tools/a7800_headless.c)
target_include_directories(a7800_headless PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_compile_options(a7800_headless PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>)
a7800_link_mame(a7800_headless)


# ---- the BIOS and HSC images a developer build embeds -------------------------
#
# Published builds embed nothing (the shipping configuration); a developer's
# own build may embed the recognised images in tools/roms/. See COMPLIANCE.md.
option(WITH_A7800_ROMS "Embed the BIOS / HSC images found in tools/roms (NOT redistributable)" OFF)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(A7800_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
if(WITH_A7800_ROMS)
  file(GLOB _a7800_rom_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/*")
  add_custom_command(
    OUTPUT "${A7800_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            "${A7800_ROMS_C}" "${CMAKE_SOURCE_DIR}/tools/roms"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py" ${_a7800_rom_files}
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${A7800_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            --none "${A7800_ROMS_C}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
    VERBATIM)
endif()


# ---- a7800_session: the frontend contract ----------------------------------
#
# Found-or-skipped, exactly as the frontends are: a machine (or a
# cross-toolchain) with no SDL3 still builds the host and its tests.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  message(STATUS "a7800_session: SDL3 not found -- building the host and its "
                 "tests only (no audio, no gamepads, no frontends)")
  add_subdirectory(tests)
  return()
endif()

set(A7800_SESSION_C_SOURCES
  src/settings.c
  src/paths.c
  src/media.c
  src/roms.c
  src/a78info.c
  src/fujinet_runtime.c
  src/audio_sdl.c
  src/bindings.c
  src/hid_keys.c
  "${A7800_ROMS_C}")
set(A7800_SESSION_CXX_SOURCES
  src/session.cpp
  src/debug.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gamepad_sdl.c")
  list(APPEND A7800_SESSION_C_SOURCES src/gamepad_sdl.c)
else()
  set(A7800_GAMEPAD_STUB ON)
endif()

add_library(a7800_session STATIC
  ${A7800_SESSION_C_SOURCES} ${A7800_SESSION_CXX_SOURCES})
set_target_properties(a7800_session PROPERTIES
  C_STANDARD 11
  CXX_STANDARD 17
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(a7800_session PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(a7800_session PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(a7800_session PUBLIC a7800_host SDL3::SDL3)
if(A7800_GAMEPAD_STUB)
  target_compile_definitions(a7800_session PRIVATE A7800_GAMEPAD_STUB=1)
endif()
# setenv()/getaddrinfo() under a strict -std=c11
target_compile_definitions(a7800_session PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
set_source_files_properties(${A7800_SESSION_C_SOURCES} ${A7800_SESSION_CXX_SOURCES}
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
set_source_files_properties("${A7800_ROMS_C}" PROPERTIES COMPILE_OPTIONS "-w")

# Where an uninstalled build and an installed copy find the FujiNet runtime
# (core/src/paths.c probes these after the executable's own directory).
target_compile_definitions(a7800_session PRIVATE
  A7800_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  A7800_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-atari7800"
  A7800_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-atari7800")

add_subdirectory(tests)

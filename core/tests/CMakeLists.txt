# ctest suites. Each is a small program; the ones that need a running
# machine boot FujiNet's CONFIG client, which is always available (the MAME
# fork's FujiNet cartridge embeds it), or a tiny cartridge the test writes
# itself (test_files.h), so nothing here ever skips for want of a ROM, and
# no BIOS is ever needed.

# ---- libmame_fngo on its own (no SDL, no session) ---------------------------

# The headless runner (core/tools/a7800_headless.c) the ROM sweep drives:
# CONFIG boots with no BIOS and paints; the tests' cartridge boots through
# the cartridge's boot block, loader and hand-over and keeps animating, on
# both consoles, staged and direct.
set(_a7800_hl_data "${CMAKE_CURRENT_BINARY_DIR}/headless.data")
add_test(NAME headless_config
         COMMAND a7800_headless --data "${_a7800_hl_data}" --port 1
                 --frames 600 --expect-colours 2)
add_test(NAME headless_staged
         COMMAND a7800_headless --data "${_a7800_hl_data}" --port 1 --image test
                 --frames 300 --expect-game --expect-animated)
add_test(NAME headless_direct
         COMMAND a7800_headless --data "${_a7800_hl_data}" --port 1 --image test
                 --mode direct --frames 300 --expect-game --expect-animated)
add_test(NAME headless_pal
         COMMAND a7800_headless --data "${_a7800_hl_data}" --port 1 --image test
                 --system a7800p --frames 300 --expect-game --expect-animated)
set_tests_properties(headless_config headless_staged headless_direct headless_pal
                     PROPERTIES TIMEOUT 120 RUN_SERIAL ON)

# Only fngo_mame_* leaves the library: MAME's own symbols (and its zlib,
# expat, ...) must never collide with a frontend's.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME mame_exports
           COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_exports.py"
                   "${MAME_FNGO_LIBRARY}")
  set_tests_properties(mame_exports PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ---- the session layer (needs SDL3) -------------------------------------------
if(TARGET a7800_session)

# The frontend contract, against a real temporary XDG tree: settings persist,
# paths resolve, CONFIG boots with no BIOS and paints, a cartridge opens and
# runs, the power switch keeps it, Reboot to CONFIG ejects it, a PAL
# cartridge brings up the PAL console, and an image the cartridge cannot map
# is refused with a reason.
add_executable(session_test session_test.c)
target_link_libraries(session_test PRIVATE a7800_session)
a7800_link_mame(session_test)
add_test(NAME session COMMAND session_test)
set_tests_properties(session PROPERTIES TIMEOUT 180)

# The remappable binding table and the keyboard translator, pure.
add_executable(bindings_test bindings_test.c)
target_link_libraries(bindings_test PRIVATE a7800_session)
a7800_link_mame(bindings_test)
add_test(NAME bindings COMMAND bindings_test)

# Where a dropped file goes: cartridges (and archives of them), the BIOS and
# HSC images by CRC, the SD import.
add_executable(media_test media_test.c)
target_link_libraries(media_test PRIVATE a7800_session)
a7800_link_mame(media_test)
add_test(NAME media COMMAND media_test)

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up. SKIPs (77) when
# the runtime was not built (-DWITH_FUJINET=OFF). Serial: it owns the port.
add_executable(fujibus_smoke fujibus_smoke.c)
target_link_libraries(fujibus_smoke PRIVATE a7800_session)
a7800_link_mame(fujibus_smoke)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# A network boot end to end: the cartridge bring-up's fujiboot client mounts
# an image from the SD host, FujiNet streams it, the loader copies it in and
# it runs. Needs A7800_TESTROM_DIR (fujinet-firmware pico/atari-7800/build);
# SKIPs without.
add_executable(netboot_test netboot_test.c)
target_link_libraries(netboot_test PRIVATE a7800_session)
a7800_link_mame(netboot_test)
add_test(NAME netboot COMMAND netboot_test)
set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)

# The gamepad layer's pure helpers, and that the SDL subsystem starts and
# stops with no hardware attached.
add_executable(gamepad_test gamepad_test.c)
target_link_libraries(gamepad_test PRIVATE a7800_session)
a7800_link_mame(gamepad_test)
target_include_directories(gamepad_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME gamepad COMMAND gamepad_test)

# The debugger contract: attach stops the machine, registers, step / over /
# out / run to, disassembly with the PC line, breakpoints and watchpoints
# that hit, memory, MARIA, I/O, the cartridge tab, the prompt, detach
# resumes.
add_executable(debug_test debug_test.c)
target_link_libraries(debug_test PRIVATE a7800_session)
a7800_link_mame(debug_test)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)

endif()

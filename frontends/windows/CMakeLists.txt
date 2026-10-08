# The Windows frontend: Win32 + GDI, no toolkit.
#
# WIN32_EXECUTABLE gives WinMain and no console window. The manifest is
# generated from PROJECT_VERSION rather than repeating it, so the version
# lives in exactly one place (the top-level project() call).

if(NOT WIN32)
  message(STATUS "windows frontend: not a Windows build -- skipping")
  return()
endif()

configure_file(app.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/app.manifest @ONLY)

# windres needs to find app.ico and app.manifest; the .rc references both by
# bare name, so give it both the source dir (icon) and the binary dir
# (generated manifest).
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_BINARY_DIR}")
enable_language(RC)

add_executable(fujinet-go-atari7800-windows WIN32
  main.c
  controllers/controller_window.c
  debugger/dbg_window.c
  resource.rc
)
target_include_directories(fujinet-go-atari7800-windows PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fujinet-go-atari7800-windows PRIVATE
  a7800_session
  # dwmapi: DwmFlush, this platform's frame clock. comdlg32: the file
  # dialogs. shell32: DragQueryFile and ShellExecute for the web UI.
  # comctl32: the debugger's tab control and the settings trackbar (and the
  # v6 manifest above is what makes them look like this decade).
  dwmapi comdlg32 shell32 comctl32)
target_compile_options(fujinet-go-atari7800-windows PRIVATE -Wall -Wextra)
# mame_fngo.dll beside the exe (cmake/BuildMame.cmake installs it there)
a7800_link_mame(fujinet-go-atari7800-windows)

# Bundle the MinGW runtime statically so the .exe has no loose DLL deps.
# This has to live on the TARGET, not in the toolchain file: that only
# covers the cross-compile, leaving the native MSYS2/UCRT64 build that
# actually ships the release importing libwinpthread-1.dll.
target_link_options(fujinet-go-atari7800-windows PRIVATE
  -static -static-libgcc -static-libstdc++)

install(TARGETS fujinet-go-atari7800-windows RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

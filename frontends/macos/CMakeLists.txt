# The macOS frontend: AppKit, no toolkit dependencies beyond the system
# frameworks.
#
# Written on Linux and never compiled on a Mac -- .github/workflows/macos.yml
# is the first thing that will actually try it, on an Apple Silicon (arm64)
# runner, and a failure there is real signal about this code.

if(NOT APPLE)
  message(STATUS "macos frontend: not a macOS build -- skipping")
  return()
endif()

enable_language(OBJC)

# The .icns has to be added as a SOURCE with MACOSX_PACKAGE_LOCATION so CMake
# copies it into the bundle, and MACOSX_BUNDLE_ICON_FILE names it for
# Info.plist.in's ${MACOSX_BUNDLE_ICON_FILE}. Neither alone is enough.
set(A7800_ICNS "${CMAKE_SOURCE_DIR}/data/icons/fujinet-go-atari7800.icns")

add_executable(fujinet-go-atari7800-macos MACOSX_BUNDLE
  main.m
  AppDelegate.m
  DisplayView.m
  KeyForward.m
  controllers/ControllersWindow.m
  debugger/DebuggerWindow.m
  "${A7800_ICNS}"
)
set_source_files_properties("${A7800_ICNS}" PROPERTIES
  MACOSX_PACKAGE_LOCATION "Resources")

set_target_properties(fujinet-go-atari7800-macos PROPERTIES
  # The bundle is named for the product, not the target: "FujiNet Go Atari
  # 7800.app", which is what the packaging workflow refers to. It has to
  # agree with MACOSX_BUNDLE_EXECUTABLE_NAME, which names the binary inside.
  OUTPUT_NAME "FujiNet Go Atari 7800"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_EXECUTABLE_NAME "FujiNet Go Atari 7800"
  MACOSX_BUNDLE_ICON_FILE "fujinet-go-atari7800.icns"
  MACOSX_BUNDLE_BUNDLE_NAME "FujiNet Go Atari 7800"
  MACOSX_BUNDLE_GUI_IDENTIFIER "online.fujinet.go.atari7800.macos"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

target_include_directories(fujinet-go-atari7800-macos PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fujinet-go-atari7800-macos PRIVATE a7800_session
  "-framework Cocoa"
  "-framework CoreVideo"    # CVDisplayLink: this platform's frame clock
  "-framework CoreGraphics"
  "-framework QuartzCore"
  "-framework Carbon")      # kVK_* virtual key codes
target_compile_options(fujinet-go-atari7800-macos PRIVATE -Wall -Wextra
  -fobjc-arc)

# The emulator: libmame_fngo (cmake/BuildMame.cmake), in the bundle's
# Frameworks where a Mac app keeps its libraries. a7800_link_mame gives the
# executable the rpath @executable_path/../Frameworks for an install; the
# build tree's bundle is the one the workflows test, sign and zip, so it is
# linked with that same rpath (never the build directory's) and the library
# is copied in beside it. Linking it final also means an install never runs
# install_name_tool over the executable, which would break the signature
# the linker gives every arm64 binary. Its install name is
# @rpath/libmame_fngo.dylib (the fork's scripts/src/osd/fngo.lua), which is
# what makes the rpath reach it. (A shared SDL3 whose install name is @rpath
# -- a hand-built one, as macos.yml's -- is then found through
# DYLD_LIBRARY_PATH only; Homebrew's and the release's static one need
# nothing.)
a7800_link_mame(fujinet-go-atari7800-macos)
set_target_properties(fujinet-go-atari7800-macos PROPERTIES
  BUILD_WITH_INSTALL_RPATH ON)
add_custom_command(TARGET fujinet-go-atari7800-macos POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-atari7800-macos>/Frameworks"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MAME_FNGO_LIBRARY}"
          "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-atari7800-macos>/Frameworks/"
  COMMENT "Packing libmame_fngo into the bundle's Frameworks"
  VERBATIM)

install(TARGETS fujinet-go-atari7800-macos BUNDLE DESTINATION .)

# Pack the FujiNet runtime into the bundle so the .app is self-contained.
# It goes BESIDE THE EXECUTABLE in Contents/MacOS, not into the
# Frameworks/Resources layout libmame_fngo uses above, because that is where
# core/src/paths.c looks: it probes the executable's directory for
# libfujinet.dylib and takes the asset tree from its "fujinet" subdir (which
# main.m also hands the session as fujinet_runtime_src). add_dependencies
# rather than a bare POST_BUILD so the ordering against the runtime target
# is real.
if(TARGET fujinet-runtime)
  add_dependencies(fujinet-go-atari7800-macos fujinet-runtime)
  set(_exedir "$<TARGET_FILE_DIR:fujinet-go-atari7800-macos>")
  add_custom_command(TARGET fujinet-go-atari7800-macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FUJINET_LIB}" "${_exedir}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FUJINET_OUT}/data" "${_exedir}/fujinet/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FUJINET_OUT}/SD" "${_exedir}/fujinet/SD"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FUJINET_OUT}/fnconfig.ini" "${_exedir}/fujinet/"
    COMMENT "Packing the FujiNet runtime into the bundle"
    VERBATIM)
endif()

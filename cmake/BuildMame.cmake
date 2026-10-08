# libmame_fngo: MAME's Atari 7800 with the FujiNet cartridge, as a shared
# library (src/fngo/fngo_mame.h in the MAME fork).
#
# MAME builds itself -- GENie, its own makefiles, Python for its generated
# sources -- and that is what runs here, for the fngo target only:
#
#     make -C <MAME> TARGET=fngo SUBTARGET=a7800 OSD=fngo BUILDDIR=<relative>
#
# into <build>/mame, so separate build trees (GNOME, KDE, a Windows cross
# build) never share MAME's objects. The first build compiles about nine
# hundred files (a few minutes on a fast machine, tens on a CI runner);
# later ones are make's own incremental builds. MAME_CCACHE=ON puts ccache in
# front of the compiler for CI.
#
# MAME_FNGO_PREBUILT=<prefix> uses an installed copy instead (the Flatpak
# builds the library once, as its own module): <prefix>/lib/libmame_fngo.*
# and <prefix>/include/fngo_mame.h.
#
# Provides:
#   mame_fngo           an IMPORTED shared library target, with the header
#   mame-fngo           the custom target that builds it (depend on it)
#   MAME_FNGO_LIBRARY   the library file to install / bundle

option(MAME_CCACHE "Build MAME through ccache" OFF)
set(MAME_FNGO_PREBUILT "" CACHE PATH "Prefix of a prebuilt libmame_fngo (skips building MAME)")

if(APPLE)
  set(_fngo_name "libmame_fngo.dylib")
elseif(WIN32)
  set(_fngo_name "mame_fngo.dll")
else()
  set(_fngo_name "libmame_fngo.so")
endif()

if(MAME_FNGO_PREBUILT)
  find_file(MAME_FNGO_LIBRARY ${_fngo_name}
            PATHS "${MAME_FNGO_PREBUILT}/lib" "${MAME_FNGO_PREBUILT}/lib64"
                  "${MAME_FNGO_PREBUILT}/bin"
            NO_DEFAULT_PATH REQUIRED)
  find_path(MAME_FNGO_INCLUDE fngo_mame.h
            PATHS "${MAME_FNGO_PREBUILT}/include" NO_DEFAULT_PATH REQUIRED)
  add_custom_target(mame-fngo)
  if(WIN32)
    find_file(MAME_FNGO_IMPLIB libmame_fngo.dll.a
              PATHS "${MAME_FNGO_PREBUILT}/lib" NO_DEFAULT_PATH REQUIRED)
  endif()
  message(STATUS "MAME: prebuilt ${MAME_FNGO_LIBRARY}")
else()
  a7800_provide_dependency(
    NAME mame
    PATH third_party/mame
    ARCHIVE "${MAME_ARCHIVE}"
    SHA256 "${MAME_ARCHIVE_SHA256}"
    STRIP "mame-${MAME_COMMIT}"
    SENTINEL src/fngo/fngo_mame.h
    OVERRIDE MAME_SRC
    RESULT MAME_DIR)

  find_program(MAME_MAKE NAMES gmake make mingw32-make REQUIRED)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  set(MAME_BUILD_DIR "${CMAKE_BINARY_DIR}/mame")
  file(MAKE_DIRECTORY "${MAME_BUILD_DIR}")
  # GENie composes MAME_DIR .. BUILDDIR, so it has to be relative.
  file(RELATIVE_PATH _mame_builddir_rel "${MAME_DIR}" "${MAME_BUILD_DIR}")
  set(MAME_FNGO_LIBRARY "${MAME_BUILD_DIR}/fngo/${_fngo_name}")
  set(MAME_FNGO_INCLUDE "${MAME_DIR}/src/fngo")
  if(WIN32)
    set(MAME_FNGO_IMPLIB "${MAME_BUILD_DIR}/fngo/libmame_fngo.dll.a")
  endif()

  cmake_host_system_information(RESULT _ncpu QUERY NUMBER_OF_LOGICAL_CORES)
  if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    set(_ncpu "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
  endif()

  set(_mame_args
    TARGET=fngo SUBTARGET=a7800 OSD=fngo
    "BUILDDIR=${_mame_builddir_rel}"
    NOWERROR=1 IGNORE_GIT=1 REGENIE=1
    "PYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
    NO_USE_MIDI=1 NO_USE_PORTAUDIO=1 NO_USE_PULSEAUDIO=1 NO_USE_PIPEWIRE=1
    NO_OPENGL=1 USE_QTDEBUG=0 SYMBOLS=0 OPTIMIZE=2
    "-j${_ncpu}")
  if(NOT WIN32)
    if(APPLE)
      list(APPEND _mame_args "ARCHOPTS=-fPIC -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
      list(APPEND _mame_args "ARCHOPTS=-fPIC")
    endif()
  endif()
  if(CMAKE_CROSSCOMPILING AND WIN32)
    # a MinGW cross build from Linux (cmake/toolchains/mingw-w64.cmake)
    # (PTR64/MINGW64 are what an MSYS2 shell's MSYSTEM would set)
    list(APPEND _mame_args TARGETOS=windows CROSS_BUILD=1 PTR64=1 MINGW64=/usr
         "OVERRIDE_CC=${CMAKE_C_COMPILER}" "OVERRIDE_CXX=${CMAKE_CXX_COMPILER}"
         "OVERRIDE_LD=${CMAKE_CXX_COMPILER}")
  endif()
  set(_mame_env)
  if(APPLE)
    list(APPEND _mame_env "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  if(MAME_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
      # MAME's precompiled headers defeat ccache; without them every object
      # caches on its own.
      list(APPEND _mame_args PRECOMPILE=0)
      if(NOT (CMAKE_CROSSCOMPILING AND WIN32))
        list(APPEND _mame_args "OVERRIDE_CC=${CCACHE_PROGRAM} ${CMAKE_C_COMPILER}"
                               "OVERRIDE_CXX=${CCACHE_PROGRAM} ${CMAKE_CXX_COMPILER}"
                               "OVERRIDE_LD=${CMAKE_CXX_COMPILER}")
      endif()
      message(STATUS "MAME: building through ${CCACHE_PROGRAM}")
    endif()
  endif()

  # make decides what is out of date; this always asks it.
  add_custom_target(mame-fngo ALL
    COMMAND ${CMAKE_COMMAND} -E env ${_mame_env}
            "${MAME_MAKE}" -C "${MAME_DIR}" ${_mame_args}
    BYPRODUCTS "${MAME_FNGO_LIBRARY}" ${MAME_FNGO_IMPLIB}
    COMMENT "Building libmame_fngo (MAME's Atari 7800; the first build takes a while)"
    USES_TERMINAL
    VERBATIM)
  message(STATUS "MAME: ${MAME_DIR} -> ${MAME_FNGO_LIBRARY}")
endif()

add_library(mame_fngo SHARED IMPORTED GLOBAL)
set_target_properties(mame_fngo PROPERTIES
  IMPORTED_LOCATION "${MAME_FNGO_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${MAME_FNGO_INCLUDE}")
if(WIN32)
  set_target_properties(mame_fngo PROPERTIES IMPORTED_IMPLIB "${MAME_FNGO_IMPLIB}")
endif()
get_filename_component(MAME_FNGO_DIR "${MAME_FNGO_LIBRARY}" DIRECTORY)

# Installed beside libfujinet: the executables find it through their RPATH
# ($ORIGIN/../lib/fujinet-go-atari7800), Windows next to the .exe, macOS in
# the bundle's Frameworks (frontends/macos/CMakeLists.txt). A prebuilt copy
# is already installed where its prefix keeps it (the Flatpak's /app/lib, on
# the loader's path there).
if(MAME_FNGO_PREBUILT)
elseif(WIN32)
  install(FILES "${MAME_FNGO_LIBRARY}" DESTINATION ${CMAKE_INSTALL_BINDIR})
elseif(NOT APPLE)
  install(FILES "${MAME_FNGO_LIBRARY}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/fujinet-go-atari7800)
endif()

# Every executable that links it: the RPATH that finds it installed, and in
# the build tree.
function(a7800_link_mame target)
  target_link_libraries(${target} PRIVATE mame_fngo)
  add_dependencies(${target} mame-fngo)
  if(WIN32)
    # Windows has no RPATH: the loader looks beside the .exe, so every
    # executable that links the DLL (the frontend, the tests, the headless
    # runner) gets a copy there in the build tree.
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${MAME_FNGO_LIBRARY}" "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  elseif(APPLE)
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "${MAME_FNGO_DIR}"
      INSTALL_RPATH "@executable_path/../Frameworks")
  else()
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "${MAME_FNGO_DIR}"
      INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/fujinet-go-atari7800")
  endif()
endfunction()

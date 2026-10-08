cmake_minimum_required(VERSION 3.20)

# Must be set before project() to take effect. Left unset, Clang defaults the
# bundle's minimum macOS version to whatever SDK the CI runner's Xcode ships
# -- e.g. the macos-latest runner's SDK can be newer than a real tester's OS,
# which produced "You can't use this version of the application ... with this
# version of macOS" on an M1 Mac for the sibling MSX port. MAME needs
# <filesystem> and C++17's library (10.15+); 13.3 matches the Atari 2600 and
# NES siblings, the family's other C++-core members.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

# The version lives here and nowhere else: the About boxes, the macOS bundle
# and the release workflow all take it from PROJECT_VERSION, and cutting a tag
# checks that the tag agrees with it (see "Cutting a release" in the README).
project(fujinet-go-atari7800-desktop VERSION 0.1.0 LANGUAGES C CXX)

add_compile_definitions(A7800_VERSION_STRING="${PROJECT_VERSION}")

# The session layer and every frontend are C11; the emulator is MAME, built by
# its own makefiles into libmame_fngo (cmake/BuildMame.cmake), and the host
# glue around it is C++17. The frontends' own C++ (Qt) uses the default below.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Which native frontend(s) to build. Each is found-or-skipped so a lean
# machine can still build the others (or just the core + tests).
set(FRONTEND "all" CACHE STRING
    "Frontend to build: gnome, kde, macos, windows, all, none")

# The Atari 7800 BIOS and the High Score Cart ROM are copyrighted Atari
# firmware: published builds embed neither (WITH_A7800_ROMS=OFF, in
# core/CMakeLists.txt) and the console boots the FujiNet cartridge directly,
# through the MAME fork's "none" BIOS. The CONFIG client is FujiNet's own,
# built into the fork's FujiNet cartridge. See COMPLIANCE.md.

# Included here, at the top of the tree, so every subdirectory sees the
# *variables* it defines: GNUInstallDirs caches CMAKE_INSTALL_DATADIR as an
# empty string and only sets its non-cache fallback (share) in the including
# scope. Including it in a subdirectory (as core/ would) therefore leaves the
# frontends with an empty DATADIR -- and "${CMAKE_INSTALL_DATADIR}/applications"
# then resolves to the absolute path /applications, outside the prefix.
include(GNUInstallDirs)

include(cmake/Dependencies.cmake)
include(cmake/BuildMame.cmake)

enable_testing()
add_subdirectory(core)

# The FujiNet runtime: built from the pinned firmware sources as part of the
# normal build and installed alongside the app (the session provisions the
# user's runtime tree from the installed copy on first start). Set up before
# the frontends so the macOS bundle can depend on the library it packs.
include(cmake/FujiNetRuntime.cmake)

# Each frontend is native to one platform; with FRONTEND=all (the default)
# only the host's viable frontends are considered, so a Windows/macOS
# configure never descends into the Linux (pkg-config) frontends and vice
# versa. Naming a frontend explicitly still forces just that one.
#
# add_frontend also skips a directory that has no CMakeLists.txt yet, which is
# how a milestone that has not landed stays out of the way instead of breaking
# the configure for everyone. Naming it explicitly is an error, so nobody can
# ask for a frontend and silently get nothing.
function(add_frontend name)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/frontends/${name}/CMakeLists.txt")
    if(FRONTEND STREQUAL name)
      message(FATAL_ERROR "The ${name} frontend is not implemented yet.")
    endif()
    message(STATUS "${name} frontend: not implemented yet -- skipping")
    return()
  endif()
  add_subdirectory("frontends/${name}")
endfunction()

if(FRONTEND STREQUAL "gnome" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(gnome)
endif()
if(FRONTEND STREQUAL "kde" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(kde)
endif()
if(FRONTEND STREQUAL "macos" OR (FRONTEND STREQUAL "all" AND APPLE))
  add_frontend(macos)
endif()
if(FRONTEND STREQUAL "windows" OR (FRONTEND STREQUAL "all" AND WIN32))
  add_frontend(windows)
endif()

# The ROM-less claim, checked on every binary built with this option off --
# which is the default here, and what every published artifact is built with.
#
# Registered at the top level rather than in core/ so it can be pointed at the
# frontend executables: those are the artifacts a user actually downloads, and
# they link differently from the test binaries.
if(NOT WITH_A7800_ROMS)
  find_package(Python3 COMPONENTS Interpreter)
  set(A7800_ROM_CHECK_TARGETS "")
  if(TARGET a7800_headless)
    list(APPEND A7800_ROM_CHECK_TARGETS a7800_headless)
  endif()
  foreach(_fe gnome kde macos windows)
    if(TARGET fujinet-go-atari7800-${_fe})
      list(APPEND A7800_ROM_CHECK_TARGETS fujinet-go-atari7800-${_fe})
    endif()
  endforeach()
  if(Python3_Interpreter_FOUND)
    foreach(_t IN LISTS A7800_ROM_CHECK_TARGETS)
      add_test(NAME no_embedded_roms_${_t}
               COMMAND "${Python3_EXECUTABLE}"
                       "${CMAKE_SOURCE_DIR}/core/tests/no_embedded_roms.py"
                       "$<TARGET_FILE:${_t}>"
                       "${CMAKE_SOURCE_DIR}/tools/roms")
      # SKIPs (ctest 77) rather than fails when tools/roms has no real ROM to
      # probe with -- true of every CI/release checkout.
      set_tests_properties(no_embedded_roms_${_t} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    # ... and the MAME library itself, which every frontend ships
    add_test(NAME no_embedded_roms_mame_fngo
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_SOURCE_DIR}/core/tests/no_embedded_roms.py"
                     "${MAME_FNGO_LIBRARY}"
                     "${CMAKE_SOURCE_DIR}/tools/roms")
    set_tests_properties(no_embedded_roms_mame_fngo PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

# Application icons (rendered from source art by tools/icons/make-icons.py).
# Desktop environments look the icon up by the desktop-entry id, so the same
# art is installed under each frontend's id -- at every hicolor size, which is
# what makes it show up crisply everywhere from the 16px window list to the
# 256px app grid.
#
# Only for frontends actually built here: a package should not ship files for
# an app it does not contain.
set(A7800_ICON_SIZES 16 24 32 48 64 128 192 256 512)
set(A7800_ICON_TARGETS_AND_IDS
  fujinet-go-atari7800-gnome online.fujinet.go.atari7800.gnome
  fujinet-go-atari7800-kde online.fujinet.go.atari7800.kde)
list(LENGTH A7800_ICON_TARGETS_AND_IDS _a7800_icon_n)
math(EXPR _a7800_icon_last "${_a7800_icon_n} - 1")
foreach(_i RANGE 0 ${_a7800_icon_last} 2)
  list(GET A7800_ICON_TARGETS_AND_IDS ${_i} _target)
  math(EXPR _j "${_i} + 1")
  list(GET A7800_ICON_TARGETS_AND_IDS ${_j} _id)
  if(TARGET ${_target})
    foreach(_size IN LISTS A7800_ICON_SIZES)
      install(FILES data/icons/hicolor/${_size}x${_size}/apps/fujinet-go-atari7800.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
              RENAME ${_id}.png)
    endforeach()
  endif()
endforeach()

# ---- packaging --------------------------------------------------------
#
# One package per frontend, not one package for both: GNOME (GTK4/libadwaita)
# and KDE (Qt6) pull in entirely different runtime library stacks, and a user
# who wants one should not be made to depend on the other.
if(FRONTEND STREQUAL "gnome")
  set(CPACK_PACKAGE_NAME "fujinet-go-atari7800-gnome")
elseif(FRONTEND STREQUAL "kde")
  set(CPACK_PACKAGE_NAME "fujinet-go-atari7800-kde")
else()
  set(CPACK_PACKAGE_NAME "fujinet-go-atari7800")
endif()

set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "FujiNet")
set(CPACK_PACKAGE_CONTACT "https://github.com/FujiNetWIFI/fujinet-go-atari7800-desktop/issues")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://fujinet.online/")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Self-contained Atari 7800 with built-in FujiNet")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;RPM;TGZ")

  # Auto-detected from the actual linked libraries (dpkg-shlibdeps /
  # rpmbuild's own dependency generator) rather than hand-listed: GTK4 vs Qt6
  # alone makes one fixed list wrong for whichever frontend was not built, and
  # a hand-list silently goes stale the moment a dependency's package name
  # changes across distributions.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # libmame_fngo and libfujinet are this package's own private libraries
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
      "${CMAKE_BINARY_DIR}/mame/fngo;${CMAKE_SOURCE_DIR}/tools/fujinet/work/out")
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

  set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
endif()

include(CPack)

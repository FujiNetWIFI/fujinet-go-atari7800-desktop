# Cross-compile Windows from Linux.
#
# Not a substitute for the native MSYS2 build CI runs, but it catches the
# Windows-only translation units -- the Win32 frontend above all --
# at the desk rather than on a push. The sibling ports learned to keep one of
# these because "the Windows frontend was developed on Linux" is otherwise a
# euphemism for "the Windows build was never compiled".
#
# SDL3 comes from SDL's own prebuilt mingw devel archive rather than a
# from-source build; point CMAKE_PREFIX_PATH at its x86_64 tree and it is
# added to the find root below:
#
#   curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz
#   tar xzf SDL3-devel-3.4.12-mingw.tar.gz
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
#         -DFRONTEND=windows -DWITH_FUJINET=OFF \
#         -DCMAKE_PREFIX_PATH="$PWD/SDL3-3.4.12/x86_64-w64-mingw32"
#   cmake --build build-win
#
# Running the result under Wine wants SDL3.dll beside the exe:
#
#   cp SDL3-3.4.12/x86_64-w64-mingw32/bin/SDL3.dll build-win/frontends/windows/
#   WINEPATH=/usr/x86_64-w64-mingw32/bin wine build-win/frontends/windows/fujinet-go-atari7800-windows.exe

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX} ${CMAKE_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static, so an artifact is a folder you copy rather than a DLL hunt. The
# test binaries need it too: without it they want libwinpthread-1.dll beside
# them and only run inside an MSYS2 shell.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

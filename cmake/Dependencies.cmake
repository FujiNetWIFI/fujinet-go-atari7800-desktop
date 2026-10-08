# External source dependencies.
#
# Standard practice for every target in the FujiNet Go family: the build
# provides each dependency for itself. A plain `git clone` of this repository
# (no --recurse-submodules), GNOME Builder, a flatpak build or a source
# tarball all end up with a usable checkout without the developer having to
# know the dependency exists.
#
# Two dependencies:
#
#   MAME              The emulator, from the tschak909/mame fork's
#                     fujinet-go-a7800 branch: mamedev's MAME plus the FujiNet
#                     Atari 7800 cartridge (src/devices/bus/a7800/fujinet*,
#                     the cartridge firmware's own protocol, mapper and load
#                     sources vendored by fujinet_sync.sh, the CONFIG client),
#                     the a7800 driver's "none" BIOS and light guns, and the
#                     fngo target and OSD that build MAME's a7800 as
#                     libmame_fngo (src/fngo/fngo_mame.h). cmake/BuildMame.cmake
#                     runs MAME's own build for it. MAME's history is far too
#                     big for a submodule or a clone, so it comes as the pinned
#                     commit's source archive.
#
#   fujinet-firmware  Pinned to a public master-plus-one commit (TCP_NODELAY on
#                     the BoIP socket, which a request/reply bus on loopback
#                     needs), the same pin the INTV, 2600 and NES apps use. The
#                     7800 bring-up (pico/atari-7800 on add-atari7800) changes
#                     nothing under lib/, src/ or components_pc/, so the RS232
#                     PC runtime built here is identical either way; the one
#                     thing the 7800 needs on top -- .a78 recognised as a ROM
#                     image to push -- is tools/fujinet/patches/0002.
#
# Resolution order:
#   1. MAME_SRC / FUJINET_SRC (cache variable or environment) -- an
#      out-of-tree working checkout, which is how these are developed in
#      tandem, e.g.
#          cmake -B build -DMAME_SRC=~/Workspace/mame-fngo \
#                         -DFUJINET_SRC=~/Workspace/fn-7800-board
#   2. third_party/<name>: the MAME archive, unpacked there on first
#      configure; fujinet-firmware initialising the submodule when this tree
#      is a git checkout.
#   3. No git metadata to work from (release tarballs, some IDE source
#      copies): a direct clone of fujinet-firmware's pinned commit.

find_package(Git QUIET)

# MAME, tschak909/mame branch fujinet-go-a7800. Recorded in three places --
# here and in both flatpak manifests (build-aux/flatpak/*.yml).
set(MAME_COMMIT "99200eb847a1eb3bd6ae51a6d2d5555c8d06abd4")
set(MAME_URL "https://github.com/tschak909/mame")
set(MAME_ARCHIVE "${MAME_URL}/archive/${MAME_COMMIT}.tar.gz")
set(MAME_ARCHIVE_SHA256 "75ed3d2d8abe658e244d86846628b5ba9a3cfe81089316f7d24459ead5adccb2")

# fujinet-firmware, branch tcp-protocol-disable-nagle (c68e86303): master plus
# TCP_NODELAY on the BoIP listener's accepted socket. Recorded in three places
# -- here and in both flatpak manifests (build-aux/flatpak/*.yml).
set(FUJINET_COMMIT "c68e863034737611415ca568c3ea4607a2220c7e")
set(FUJINET_URL "https://github.com/FujiNetWIFI/fujinet-firmware")

# a7800_provide_dependency(NAME <n> PATH <p> SENTINEL <file> OVERRIDE <VAR>
#                           RESULT <out>
#                           [URL <u> COMMIT <sha>]        # git dependency
#                           [ARCHIVE <url> SHA256 <hash> [STRIP <dir>]])
#
# SENTINEL is a path inside the checkout that only exists once the sources are
# really there -- an empty submodule directory is otherwise indistinguishable
# from a populated one, and a half-extracted archive from a complete one.
function(a7800_provide_dependency)
  cmake_parse_arguments(DEP ""
    "NAME;PATH;URL;COMMIT;ARCHIVE;SHA256;STRIP;SENTINEL;OVERRIDE;RESULT" ""
    ${ARGN})

  # 1. Explicit override: a checkout the developer maintains themselves.
  set(_override "")
  if(DEFINED ${DEP_OVERRIDE})
    set(_override "${${DEP_OVERRIDE}}")
  elseif(DEFINED ENV{${DEP_OVERRIDE}})
    set(_override "$ENV{${DEP_OVERRIDE}}")
  endif()
  if(_override)
    if(NOT EXISTS "${_override}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_OVERRIDE}=${_override} does not look like a ${DEP_NAME} "
        "checkout (no ${DEP_SENTINEL}).")
    endif()
    message(STATUS "${DEP_NAME}: using ${_override} (${DEP_OVERRIDE})")
    set(${DEP_RESULT} "${_override}" PARENT_SCOPE)
    return()
  endif()

  set(_path "${CMAKE_SOURCE_DIR}/${DEP_PATH}")

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}" AND DEP_ARCHIVE)
    # 2a. Archive dependency: download the pin and unpack it.
    #
    # Extract into a scratch directory first and move the result into place
    # only once it is complete, so an interrupted configure cannot leave a
    # partial tree that the sentinel test would go on to accept.
    set(_dl "${CMAKE_BINARY_DIR}/_deps/${DEP_NAME}.archive")
    # Beside the destination, not in the build tree: the final move is a
    # rename, and a build tree on another filesystem (/tmp is often tmpfs)
    # cannot be renamed into the source tree.
    get_filename_component(_parent "${_path}" DIRECTORY)
    set(_tmp "${_parent}/.${DEP_NAME}-extract")

    if(NOT EXISTS "${_dl}")
      message(STATUS "${DEP_NAME}: downloading ${DEP_ARCHIVE}")
      file(DOWNLOAD "${DEP_ARCHIVE}" "${_dl}"
           EXPECTED_HASH SHA256=${DEP_SHA256}
           TLS_VERIFY ON
           STATUS _dl_status)
      list(GET _dl_status 0 _dl_rc)
      if(NOT _dl_rc EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        file(REMOVE "${_dl}")
        message(FATAL_ERROR
          "Could not download ${DEP_NAME} from ${DEP_ARCHIVE}: ${_dl_msg}\n"
          "Download it by hand and unpack it, then point ${DEP_OVERRIDE} at "
          "the result.")
      endif()
    endif()

    file(REMOVE_RECURSE "${_tmp}")
    file(MAKE_DIRECTORY "${_tmp}")
    file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${_tmp}")

    set(_extracted "${_tmp}")
    if(DEP_STRIP)
      set(_extracted "${_tmp}/${DEP_STRIP}")
    endif()
    if(NOT EXISTS "${_extracted}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_NAME} archive did not contain ${DEP_STRIP}/${DEP_SENTINEL} "
        "(${DEP_ARCHIVE}).")
    endif()

    file(REMOVE_RECURSE "${_path}")
    file(RENAME "${_extracted}" "${_path}" RESULT _mv)
    file(REMOVE_RECURSE "${_tmp}")
    if(NOT _mv EQUAL 0)
      message(FATAL_ERROR "${DEP_NAME}: could not move the unpacked archive "
                          "into ${_path}: ${_mv}")
    endif()

  elseif(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
      message(FATAL_ERROR
        "${DEP_NAME} is missing and git is not installed. Either install git "
        "or unpack ${DEP_URL} (commit ${DEP_COMMIT}) into ${DEP_PATH}.")
    endif()

    # 2b. Submodule checkout -- but ONLY when this path is actually a
    # registered submodule. Attempting it otherwise prints two lines of
    # "error: pathspec ... did not match any file(s) known to git" per
    # dependency on every fresh configure, and then silently falls through to
    # the clone below. The build was fine; the output said otherwise, which
    # is its own kind of bug.
    set(_is_submodule FALSE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.gitmodules")
      file(READ "${CMAKE_SOURCE_DIR}/.gitmodules" _gitmodules)
      string(FIND "${_gitmodules}" "path = ${DEP_PATH}" _found)
      if(NOT _found EQUAL -1)
        set(_is_submodule TRUE)
      endif()
    endif()

    if(_is_submodule AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
      # --filter=blob:none keeps the fetch to the history the build needs;
      # fujinet-firmware is a large repository.
      message(STATUS "${DEP_NAME}: fetching submodule ${DEP_PATH}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update --init --filter=blob:none
                -- "${DEP_PATH}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        # Older servers/mirrors may refuse partial clones.
        execute_process(
          COMMAND ${GIT_EXECUTABLE} submodule update --init -- "${DEP_PATH}"
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          RESULT_VARIABLE _rc)
      endif()
    endif()

    if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
      # 3. No git metadata (tarball): clone the pin outright.
      message(STATUS "${DEP_NAME}: cloning ${DEP_URL} @ ${DEP_COMMIT}")
      file(REMOVE_RECURSE "${_path}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none "${DEP_URL}" "${_path}"
        RESULT_VARIABLE _rc)
      if(_rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false checkout
                  --quiet "${DEP_COMMIT}"
          WORKING_DIRECTORY "${_path}"
          RESULT_VARIABLE _rc)
      endif()
    endif()
  endif()

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    if(DEP_ARCHIVE)
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Unpack ${DEP_ARCHIVE} into "
        "${DEP_PATH}, or point ${DEP_OVERRIDE} at an existing checkout.")
    else()
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Fetch it manually with\n"
        "    git submodule update --init ${DEP_PATH}\n"
        "or point ${DEP_OVERRIDE} at an existing checkout.")
    endif()
  endif()

  # Keep a git checkout on the pin recorded here. third_party/ is not a
  # submodule, so nothing else moves it when the pin is bumped, and a stale
  # tree fails to compile against the host code -- or worse, compiles and
  # silently changes the wire format the cart device speaks. A clean checkout
  # is moved to the pin (fetching it first if the clone predates it); one with
  # local changes stops the configure rather than have them overwritten.
  # Deliberate work on a different commit belongs behind ${DEP_OVERRIDE}.
  if(DEP_COMMIT AND EXISTS "${_path}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${_path}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
      if(_rc EQUAL 0 AND NOT _head STREQUAL DEP_COMMIT)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" status --porcelain
                  --untracked-files=no
          OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET)
        if(_dirty)
          message(FATAL_ERROR
            "${DEP_NAME}: ${DEP_PATH} is at ${_head} with local changes, but "
            "cmake/Dependencies.cmake pins ${DEP_COMMIT}. Commit or stash "
            "them and re-run cmake to move to the pin, or keep working on "
            "this tree with -D${DEP_OVERRIDE}=${_path}.")
        endif()

        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" cat-file -e
                  "${DEP_COMMIT}^{commit}"
          RESULT_VARIABLE _have ERROR_QUIET)
        if(NOT _have EQUAL 0)
          message(STATUS "${DEP_NAME}: fetching pinned ${DEP_COMMIT}")
          execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${_path}" fetch --quiet origin
            RESULT_VARIABLE _rc)
          execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${_path}" cat-file -e
                    "${DEP_COMMIT}^{commit}"
            RESULT_VARIABLE _have ERROR_QUIET)
          if(NOT _have EQUAL 0)
            # Servers that only advertise branch heads won't have served it
            # above if the pin's branch moved on; ask for the commit itself.
            execute_process(
              COMMAND ${GIT_EXECUTABLE} -C "${_path}" fetch --quiet origin
                      "${DEP_COMMIT}"
              RESULT_VARIABLE _have)
          endif()
          if(NOT _have EQUAL 0)
            message(FATAL_ERROR
              "${DEP_NAME}: pinned commit ${DEP_COMMIT} is not in ${DEP_PATH} "
              "and could not be fetched from origin. Fetch it by hand, or "
              "point ${DEP_OVERRIDE} at a checkout that has it.")
          endif()
        endif()

        message(STATUS "${DEP_NAME}: moving ${_head} -> pinned ${DEP_COMMIT}")
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" -c advice.detachedHead=false
                  checkout --quiet "${DEP_COMMIT}"
          RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
          message(FATAL_ERROR
            "${DEP_NAME}: could not check out ${DEP_COMMIT} in ${DEP_PATH}.")
        endif()
      endif()
    endif()
  endif()

  message(STATUS "${DEP_NAME}: ${_path}")
  set(${DEP_RESULT} "${_path}" PARENT_SCOPE)
endfunction()

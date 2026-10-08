# The KDE frontend: plain Qt6 Widgets.
#
# Deliberately NOT KDE Frameworks. It picks up Breeze through the platform
# theme anyway, and staying framework-free keeps this frontend usable outside
# a KDE session -- and keeps the door open to reusing it elsewhere.
#
# Found-or-skipped, like the GNOME one: a machine without Qt6 development
# packages still builds the core, the tests and the other frontend.

set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND)
  message(STATUS "kde frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

add_executable(fujinet-go-atari7800-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  KeyForward.cpp
  SettingsDialog.cpp
  FujiNetWindows.cpp
  controllers/ControllersWindow.cpp
  debugger/DebuggerWindow.cpp
)
target_link_libraries(fujinet-go-atari7800-kde PRIVATE a7800_session Qt6::Widgets)
# libmame_fngo, and the RPATH that finds it installed and in the build tree
a7800_link_mame(fujinet-go-atari7800-kde)
target_compile_options(fujinet-go-atari7800-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-atari7800-kde PRIVATE
  A7800_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

install(TARGETS fujinet-go-atari7800-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(data/online.fujinet.go.atari7800.kde.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari7800.kde.desktop
               @ONLY)
configure_file(data/online.fujinet.go.atari7800.kde.metainfo.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari7800.kde.metainfo.xml
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari7800.kde.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari7800.kde.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_file_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari7800.kde.desktop)
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari7800.kde.metainfo.xml)
endif()

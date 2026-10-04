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

add_executable(fujinet-go-nes-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  KeyForward.cpp
  SettingsDialog.cpp
  FujiNetWindows.cpp
  controllers/ControllersWindow.cpp
  debugger/DebuggerWindow.cpp
)
target_link_libraries(fujinet-go-nes-kde PRIVATE nes_session Qt6::Widgets)
target_compile_options(fujinet-go-nes-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-nes-kde PRIVATE
  NES_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

# The FujiNet web UI, embedded with QtWebEngine when available and
# WITH_WEBVIEW is on; otherwise the configuration opens in the system
# browser. Never a hard dependency: QtWebEngine is enormous and absent on
# many systems.
if(WITH_WEBVIEW)
  find_package(Qt6 COMPONENTS WebEngineWidgets QUIET)
  if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(fujinet-go-nes-kde PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(fujinet-go-nes-kde PRIVATE HAVE_WEBENGINE=1)
    message(STATUS "kde frontend: embedding the FujiNet web UI (QtWebEngine)")
  else()
    message(STATUS "kde frontend: QtWebEngine not found -- the FujiNet web UI opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-nes-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(data/online.fujinet.go.nes.kde.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.nes.kde.desktop
               @ONLY)
configure_file(data/online.fujinet.go.nes.kde.metainfo.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.nes.kde.metainfo.xml
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.nes.kde.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.nes.kde.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_file_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.nes.kde.desktop)
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.nes.kde.metainfo.xml)
endif()

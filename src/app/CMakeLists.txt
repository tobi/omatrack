# The Omatrack QML module.
#
# QML documents and the C++ types they import live in this one directory, which
# is what qt_add_qml_module expects: SOURCES are scanned for QML_ELEMENT /
# QML_SINGLETON, so qmllint, qmlls, and qmlcachegen all resolve
# `import Omatrack` without any extra configuration.

qt_add_executable(omatrack
  main.cpp)

# `pragma Singleton` alone is not enough: the qmldir entry comes from this
# source property, and without it qmllint and qmlcachegen treat Style as an
# ordinary type.
set_source_files_properties(Style.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(omatrack
  URI Omatrack
  VERSION 1.0
  QML_FILES
    Main.qml
    RoleDot.qml
    CompactTextField.qml
    CompactButton.qml
    Style.qml
    AppHeader.qml
    ChannelsWindow.qml
    CornerInspectorWindow.qml
    PreferencesWindow.qml
    SessionTreeDelegate.qml
    TrackAssignmentDialog.qml
    ThinScrollBar.qml
  SOURCES
    CornerGraphView.h CornerGraphView.cpp
    DamperStripView.h DamperStripView.cpp
    MpvVideoItem.h MpvVideoItem.cpp
    OmarchyTheme.h OmarchyTheme.cpp
    ComparisonAlignment.h ComparisonAlignment.cpp
    SessionMetadataCache.h SessionMetadataCache.cpp
    TelemetryStore.h TelemetryStore.cpp
    TraceView.h TraceView.cpp
    YamlConfig.h YamlConfig.cpp
)
if(OMATRACK_ENABLE_AUTOTEST_HARNESS)
  target_sources(omatrack PRIVATE AutotestHarness.h AutotestHarness.cpp)
  target_compile_definitions(omatrack PRIVATE
    OMATRACK_ENABLE_AUTOTEST_HARNESS=1)
endif()

# Fonts and Qt Quick Controls configuration are bundled application resources.
qt_add_resources(omatrack "omatrack_assets"
  PREFIX "/"
  BASE assets
  FILES
    assets/fonts/Geist-Variable.ttf
    assets/fonts/GeistMono-Variable.ttf
    assets/fonts/OFL.txt
    assets/qtquickcontrols2.conf
)
qt_add_resources(omatrack "omatrack_branding"
  PREFIX "/"
  BASE ${CMAKE_SOURCE_DIR}
  FILES ${CMAKE_SOURCE_DIR}/assets/omatrack.svg)

target_link_libraries(omatrack PRIVATE
  omatrack_core
  omatrack_warnings
  omatrack_yaml
  omatrack_mpv
  Qt6::Concurrent
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::Qml
  Qt6::Network)

# qmlprofiler and qmlpreview can only attach to a QML-debug-enabled binary.
target_compile_definitions(omatrack PRIVATE
  OMATRACK_VERSION="${PROJECT_VERSION}"
  $<$<NOT:$<CONFIG:Release>>:QT_QML_DEBUG>)

install(TARGETS omatrack
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  BUNDLE DESTINATION .)

if(UNIX AND NOT APPLE)
  install(FILES
    ${CMAKE_SOURCE_DIR}/packaging/linux/io.github.tobi.omatrack.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES ${CMAKE_SOURCE_DIR}/assets/omatrack.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME io.github.tobi.omatrack.svg)
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/omatrack.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
  install(FILES
    ${CMAKE_SOURCE_DIR}/packaging/linux/io.github.tobi.omatrack.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()

qt_generate_deploy_qml_app_script(
  TARGET omatrack
  OUTPUT_SCRIPT omatrack_deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${omatrack_deploy_script})

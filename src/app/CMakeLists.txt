# The Omatrack QML module.
#
# QML documents and the C++ types they import live in this one directory, which
# is what qt_add_qml_module expects: SOURCES are scanned for QML_ELEMENT /
# QML_SINGLETON, so qmllint, qmlls, and qmlcachegen all resolve
# `import Omatrack` without any extra configuration.

if(APPLE)
  qt_add_executable(omatrack MACOSX_BUNDLE
    main.cpp)
else()
  qt_add_executable(omatrack
    main.cpp)
endif()

set_target_properties(omatrack PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME "Omatrack"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.tobi.omatrack"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
if(APPLE)
  set_target_properties(omatrack PROPERTIES OUTPUT_NAME "Omatrack")
endif()

if(WIN32)
  configure_file(
    ${CMAKE_SOURCE_DIR}/assets/omatrack.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/omatrack.rc
    @ONLY)
  target_sources(omatrack PRIVATE
    WindowsIntegration.h
    WindowsIntegration.cpp
    WindowsAssociations.h
    WindowsAssociations.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/omatrack.rc)
  set_target_properties(omatrack PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

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
    ChannelBrowserPopup.qml
    ChannelMappingField.qml
    ConnectionDialog.qml
    CompactTextField.qml
    CompactButton.qml
    Style.qml
    AppHeader.qml
    ChannelsWindow.qml
    CornerFocusOverlay.qml
    DriverMetadataSection.qml
    FileAssociationsDialog.qml
    FileBrowserPane.qml
    FileTreeDelegate.qml
    FilterPill.qml
    PreferencesWindow.qml
    PreferencesDriversPage.qml
    PreferencesLibraryPage.qml
    PreferencesTrackDataPage.qml
    PreferencesUpdatesPage.qml
    SessionTreeDelegate.qml
    TrackAssignmentDialog.qml
    ThinScrollBar.qml
    VideoMetadataDialog.qml
    VideoTelemetryOverlay.qml
    VideoDeltaBar.qml
  SOURCES
    AppUpdate.h AppUpdate.cpp
    AppUpdater.h AppUpdater.cpp
    AimRemoteIndex.h AimRemoteIndex.cpp
    DamperStripView.h DamperStripView.cpp
    MpvVideoItem.h MpvVideoItem.cpp
    OmarchyTheme.h OmarchyTheme.cpp
    ComparisonAlignment.h ComparisonAlignment.cpp
    RecordingSidecar.h RecordingSidecar.cpp
    TrackMetadata.h TrackMetadata.cpp
    TelemetryStore.h TelemetryStore.cpp
    LibraryLocation.h LibraryLocation.cpp
    RemoteCache.h RemoteCache.cpp
    WebDavBackend.cpp
    SigV4.h SigV4.cpp
    S3Backend.cpp
    TrackAtlasSpatial.h TrackAtlasSpatial.cpp
    TraceSceneBuilder.h TraceSceneBuilder.cpp
    TraceTextCache.h TraceTextCache.cpp
    UsbMedia.h UsbMedia.cpp
    VerboseLog.h VerboseLog.cpp
    TraceView.h TraceView.cpp
    VideoTelemetryHud.h VideoTelemetryHud.cpp
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
  FILES
    ${CMAKE_SOURCE_DIR}/assets/omatrack.svg
    ${CMAKE_SOURCE_DIR}/assets/omatrack.ico)

target_link_libraries(omatrack PRIVATE
  omatrack_core
  omatrack_portable_runtime
  omatrack_warnings
  omatrack_yaml
  omatrack_mpv
  Qt6::Concurrent
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::Qml
  Qt6::Network)
if(WIN32)
  target_link_libraries(omatrack PRIVATE dwmapi shell32)
endif()

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

# Windows and macOS releases are self-contained application bundles. Linux
# distribution installs deliberately retain their system Qt dependencies; the
# release workflow turns that install tree into a portable AppImage.
if(WIN32)
  # Qt does not deploy translations that omatrack never loads, and keeping
  # them out of the tree keeps the qt.conf written below fully consistent.
  qt_generate_deploy_qml_app_script(
    TARGET omatrack
    OUTPUT_SCRIPT omatrack_deploy_script
    NO_TRANSLATIONS)
  # Flat Windows release layout. Runtime DLLs are deployed next to the exe at
  # the install root (QT_DEPLOY_BIN_DIR "."), while Qt plugins and QML modules
  # land under lib/. The deploy support script leaves these alone because of
  # its `if(NOT ...)` guards, so they must be set before the generated script
  # is included. Generating a qt.conf is how the deploy step reconciles the
  # binaries-with-bundled-DLLs root with the lib/ plugin and QML tree.
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/omatrack_deploy_flat.cmake
    "set(QT_DEPLOY_BIN_DIR \".\")\n"
    "set(QT_DEPLOY_LIB_DIR \"lib\")\n"
    "set(QT_DEPLOY_PLUGINS_DIR \"lib/plugins\")\n"
    "set(QT_DEPLOY_QML_DIR \"lib/qml\")\n"
    "include(\"${omatrack_deploy_script}\")\n")
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/omatrack_deploy_flat.cmake)
elseif(APPLE)
  qt_generate_deploy_qml_app_script(
    TARGET omatrack
    OUTPUT_SCRIPT omatrack_deploy_script)
  install(SCRIPT ${omatrack_deploy_script})
endif()

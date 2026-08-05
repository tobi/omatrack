# The Racecraft QML module.
#
# QML documents and the C++ types they import live in this one directory, which
# is what qt_add_qml_module expects: SOURCES are scanned for QML_ELEMENT /
# QML_SINGLETON, so qmllint, qmlls, and qmlcachegen all resolve
# `import Racecraft` without any extra configuration.

qt_add_executable(racecraft
  main.cpp)

# `pragma Singleton` alone is not enough: the qmldir entry comes from this
# source property, and without it qmllint and qmlcachegen treat Style as an
# ordinary type.
set_source_files_properties(Style.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(racecraft
  URI Racecraft
  VERSION 1.0
  QML_FILES
    Main.qml
    Style.qml
    AppHeader.qml
    ChannelsWindow.qml
    CornerInspectorWindow.qml
    PreferencesWindow.qml
    SessionTreeDelegate.qml
  SOURCES
    AutotestHarness.h AutotestHarness.cpp
    CornerGraphView.h CornerGraphView.cpp
    DamperStripView.h DamperStripView.cpp
    MpvVideoItem.h MpvVideoItem.cpp
    OmarchyTheme.h OmarchyTheme.cpp
    TelemetryStore.h TelemetryStore.cpp
    TraceView.h TraceView.cpp
    YamlConfig.h YamlConfig.cpp
)

# Fonts and the compatibility corner CSVs are plain payload, not QML; a normal
# resource keeps their historic `:/fonts` and `:/corners` prefixes.
qt_add_resources(racecraft "racecraft_assets"
  PREFIX "/"
  BASE assets
  FILES
    assets/fonts/Geist-Variable.ttf
    assets/fonts/GeistMono-Variable.ttf
    assets/fonts/OFL.txt
    assets/corners/daytona.csv
    assets/corners/ier_daytona.csv
    assets/corners/lilski_sebring.csv
)

target_link_libraries(racecraft PRIVATE
  racecraft_core
  racecraft_warnings
  PkgConfig::LIBYAML
  PkgConfig::LIBMPV
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::Widgets
  Qt6::Qml
  Qt6::Network)

# qmlprofiler and qmlpreview can only attach to a QML-debug-enabled binary.
target_compile_definitions(racecraft PRIVATE
  $<$<NOT:$<CONFIG:Release>>:QT_QML_DEBUG>)

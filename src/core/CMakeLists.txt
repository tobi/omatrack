# Qt-free analysis core. Linked by the GUI, the headless commands and the
# test-only omatrack-cli; must stay
# usable without a QGuiApplication.

add_library(omatrack_core STATIC
  ComparisonAlignment.cpp
  ComparisonAlignment.h
  CornerAnalysis.cpp
  CornerAnalysis.h
  MonotonicSeries.h
  TelemetryEngine.cpp
  TelemetryEngine.h
  TelemetryEngineInternal.h
  TrackAtlasSpatial.cpp
  TrackAtlasSpatial.h)

target_include_directories(omatrack_core
  PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(omatrack_core
  PUBLIC omatrack_rust_bridge
  PRIVATE omatrack_warnings)

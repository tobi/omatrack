# Qt-free analysis core. Linked by both the GUI and omatrack-cli; must stay
# usable without a QGuiApplication.

add_library(omatrack_core STATIC
  TelemetryEngine.cpp
  TelemetryEngine.h
  TelemetryEngineInternal.h)

target_include_directories(omatrack_core
  PUBLIC ${CMAKE_SOURCE_DIR}/src
  PRIVATE ${CMAKE_SOURCE_DIR}/third_party/motorsport-telemetry/bridge/include)
target_link_libraries(omatrack_core
  PUBLIC omatrack_rust_bridge
  PRIVATE omatrack_warnings)

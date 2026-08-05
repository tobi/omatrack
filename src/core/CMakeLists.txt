# Qt-free analysis core. Linked by both the GUI and racecraft-cli; must stay
# usable without a QGuiApplication.

add_library(racecraft_core STATIC
  TelemetryEngine.cpp
  TelemetryEngine.h)

target_include_directories(racecraft_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(racecraft_core
  PUBLIC racecraft_rust_bridge
  PRIVATE racecraft_warnings)

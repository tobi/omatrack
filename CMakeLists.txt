cmake_minimum_required(VERSION 3.21)
project(racecraft_qt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS
  Core Gui Quick QuickControls2 Widgets Qml Network)

# ── Rust parsing bridge (vendored duckdb_motorsport_telemetry) ──────
set(RUST_WORKSPACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/motorsport-telemetry)
set(RUST_TARGET_DIR ${RUST_WORKSPACE}/target)
set(RUST_STATICLIB ${RUST_TARGET_DIR}/release/libracecraft_bridge.a)

# Build the bridge staticlib. The archive is the real parser we link against.
add_custom_command(
  OUTPUT ${RUST_STATICLIB}
  COMMAND cargo build --release --manifest-path ${RUST_WORKSPACE}/Cargo.toml
  WORKING_DIRECTORY ${RUST_WORKSPACE}
  DEPENDS ${RUST_WORKSPACE}/Cargo.toml
          ${RUST_WORKSPACE}/bridge/src/lib.rs
  COMMENT "Building Rust telemetry parsing bridge (duckdb_motorsport_telemetry)"
  VERBATIM)

add_custom_target(rust_bridge DEPENDS ${RUST_STATICLIB})

add_library(racecraft_rust_bridge STATIC IMPORTED GLOBAL)
set_target_properties(racecraft_rust_bridge PROPERTIES
  IMPORTED_LOCATION ${RUST_STATICLIB})
add_dependencies(racecraft_rust_bridge rust_bridge)

# ── core engine (Qt-free) ───────────────────────────────────────────
add_library(racecraft_core STATIC
  src/core/TelemetryEngine.cpp)
target_include_directories(racecraft_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(racecraft_core PUBLIC racecraft_rust_bridge)
target_link_directories(racecraft_core PUBLIC ${RUST_TARGET_DIR}/release)
target_link_options(racecraft_core PUBLIC -Wl,--whole-archive
  -lracecraft_bridge -Wl,--no-whole-archive)
target_link_libraries(racecraft_core PUBLIC pthread dl m)

# ── CLI (headless acceptance harness) ───────────────────────────────
add_executable(racecraft-cli cli/main.cpp)
target_link_libraries(racecraft-cli PRIVATE racecraft_core)
target_include_directories(racecraft-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ── Qt app ──────────────────────────────────────────────────────────
qt_add_executable(racecraft
  src/main.cpp
  src/app/TelemetryStore.cpp
  src/app/TraceView.cpp
  src/qml/application.qrc
)
target_link_libraries(racecraft PRIVATE racecraft_core Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Qml Qt6::Network)
target_include_directories(racecraft PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

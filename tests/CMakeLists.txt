# Unit tests for omatrack_core and app types, run through CTest.
#
# Three executables, each linking only what it needs:
#   core-test    — Qt-free analysis core (links omatrack_core + Qt6::Test)
#   yamlconfig-test — YamlConfig CRUD/round-trip (compiles YamlConfig.cpp directly)
#   apptypes-test — Plain-struct contracts from TelemetryStore.h

find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

# ── core-test ───────────────────────────────────────────────────────
# Tests pure free functions, detail-namespace lap helpers, and
# TelemetrySource with synthetic channels. No QCoreApplication needed.
qt_add_executable(core-test CoreTest.cpp)
target_link_libraries(core-test PRIVATE
  omatrack_core
  Qt6::Test
  omatrack_warnings)
add_test(NAME core-test COMMAND core-test)
set_tests_properties(core-test PROPERTIES LABELS unit)

# ── yamlconfig-test ─────────────────────────────────────────────────
# Compiles YamlConfig.cpp directly (it's not in a separate library) and
# uses a temp XDG_CONFIG_HOME to isolate from the user's real config.
qt_add_executable(yamlconfig-test YamlConfigTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/YamlConfig.cpp)
target_include_directories(yamlconfig-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(yamlconfig-test PRIVATE
  Qt6::Test
  Qt6::Core
  PkgConfig::LIBYAML
  omatrack_warnings)
add_test(NAME yamlconfig-test COMMAND yamlconfig-test)
set_tests_properties(yamlconfig-test PROPERTIES LABELS unit)

# ── apptypes-test ───────────────────────────────────────────────────
# Tests inline struct methods from TelemetryStore.h. No .cpp to link —
# the structs are header-only. Needs Qt6::Qml for the QML_NAMED_ELEMENT
# macro that appears on TelemetryStore in the same header.
qt_add_executable(apptypes-test AppTypesTest.cpp)
target_include_directories(apptypes-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(apptypes-test PRIVATE
  Qt6::Test
  Qt6::Core
  Qt6::Gui
  Qt6::Qml
  omatrack_warnings)
add_test(NAME apptypes-test COMMAND apptypes-test)
set_tests_properties(apptypes-test PROPERTIES LABELS unit)

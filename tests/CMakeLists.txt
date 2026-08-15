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

# ── corner-analysis-test ────────────────────────────────────────────
# Synthetic corners through measureCorner() and the analyzer registry:
# asserts the notes a driver reads, not the arithmetic behind them.
qt_add_executable(corner-analysis-test CornerAnalysisTest.cpp)
target_link_libraries(corner-analysis-test PRIVATE
  omatrack_core
  Qt6::Test
  omatrack_warnings)
add_test(NAME corner-analysis-test COMMAND corner-analysis-test)
set_tests_properties(corner-analysis-test PROPERTIES LABELS unit)

# ── yamlconfig-test ─────────────────────────────────────────────────
# Compiles YamlConfig.cpp directly (it's not in a separate library) and
# uses a temp XDG_CONFIG_HOME to isolate from the user's real config.
qt_add_executable(yamlconfig-test YamlConfigTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/YamlConfig.cpp
  ${CMAKE_SOURCE_DIR}/src/app/TrackMetadata.cpp)
target_include_directories(yamlconfig-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(yamlconfig-test PRIVATE
  Qt6::Test
  Qt6::Core
  omatrack_yaml
  omatrack_warnings)
add_test(NAME yamlconfig-test COMMAND yamlconfig-test)
set_tests_properties(yamlconfig-test PROPERTIES LABELS unit)

# ── appupdate-test ──────────────────────────────────────────────────
# Parses GitHub release JSON, version compare, checksums, and the
# AppImage on-disk swap. No network and no QML.
qt_add_executable(appupdate-test
  AppUpdateTest.h
  AppUpdateTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/AppUpdate.cpp)
target_include_directories(appupdate-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(appupdate-test PRIVATE
  Qt6::Test
  Qt6::Core
  omatrack_warnings)
add_test(NAME appupdate-test COMMAND appupdate-test)
set_tests_properties(appupdate-test PROPERTIES LABELS unit)

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

# ── sigv4-test ──────────────────────────────────────────────────────
# Pins the signer to published AWS vectors. A wrong signature is
# undebuggable against a live bucket — S3 only ever answers
# SignatureDoesNotMatch — so it has to be caught here.
qt_add_executable(sigv4-test SigV4Test.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SigV4.cpp)
target_include_directories(sigv4-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sigv4-test PRIVATE
  Qt6::Test
  Qt6::Core
  omatrack_warnings)
add_test(NAME sigv4-test COMMAND sigv4-test)
set_tests_properties(sigv4-test PROPERTIES LABELS unit)

# ── Remote cache integration ───────────────────────────────────────
# Uses local HTTP responders speaking each protocol to verify streamed
# downloads, ETag reuse, and offline operation from the on-disk cache.
qt_add_executable(remote-cache-test RemoteCacheTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/RemoteCache.cpp
  ${CMAKE_SOURCE_DIR}/src/app/AimRemoteIndex.cpp
  ${CMAKE_SOURCE_DIR}/src/app/VerboseLog.cpp
  ${CMAKE_SOURCE_DIR}/src/app/WebDavBackend.cpp
  ${CMAKE_SOURCE_DIR}/src/app/S3Backend.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SigV4.cpp
  ${CMAKE_SOURCE_DIR}/src/app/LibraryLocation.cpp)
target_include_directories(remote-cache-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(remote-cache-test PRIVATE
  Qt6::Test
  Qt6::Core
  Qt6::Network
  omatrack_warnings)
add_test(NAME remote-cache-test COMMAND remote-cache-test)
set_tests_properties(remote-cache-test PROPERTIES LABELS unit)

# ── AiM remote extract (parse/pack, no network) ─────────────────────
qt_add_executable(aim-remote-index-test AimRemoteIndexTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/AimRemoteIndex.cpp
  ${CMAKE_SOURCE_DIR}/src/app/RemoteCache.cpp
  ${CMAKE_SOURCE_DIR}/src/app/VerboseLog.cpp
  ${CMAKE_SOURCE_DIR}/src/app/WebDavBackend.cpp
  ${CMAKE_SOURCE_DIR}/src/app/S3Backend.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SigV4.cpp
  ${CMAKE_SOURCE_DIR}/src/app/LibraryLocation.cpp)
target_include_directories(aim-remote-index-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aim-remote-index-test PRIVATE
  Qt6::Test
  Qt6::Core
  Qt6::Network
  omatrack_warnings)
add_test(NAME aim-remote-index-test COMMAND aim-remote-index-test)
set_tests_properties(aim-remote-index-test PROPERTIES LABELS unit)

# ── Portable recording sidecars ─────────────────────────────────────
qt_add_executable(recording-sidecar-test RecordingSidecarTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/RecordingSidecar.cpp
  ${CMAKE_SOURCE_DIR}/src/app/VerboseLog.cpp)
target_include_directories(recording-sidecar-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(recording-sidecar-test PRIVATE
  Qt6::Test
  Qt6::Core
  omatrack_warnings)
add_test(NAME recording-sidecar-test COMMAND recording-sidecar-test)
set_tests_properties(recording-sidecar-test PROPERTIES LABELS unit)

# ── Attached USB telemetry volumes ──────────────────────────────────
qt_add_executable(usb-media-test UsbMediaTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/UsbMedia.cpp)
target_include_directories(usb-media-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(usb-media-test PRIVATE
  Qt6::Test
  Qt6::Core
  omatrack_warnings)
add_test(NAME usb-media-test COMMAND usb-media-test)
set_tests_properties(usb-media-test PROPERTIES LABELS unit)

# ── Omarchy theme bridge test ──
# Exercises the atomic directory replacement used by `omarchy theme set` and
# verifies that the QML-facing color map changes without restarting the app.
qt_add_executable(omarchy-theme-test OmarchyThemeTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/OmarchyTheme.cpp)
target_include_directories(omarchy-theme-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/app)
target_link_libraries(omarchy-theme-test PRIVATE
  Qt6::Test
  Qt6::Core
  Qt6::Qml
  omatrack_warnings)
add_test(NAME omarchy-theme-test COMMAND omarchy-theme-test)
set_tests_properties(omarchy-theme-test PROPERTIES LABELS unit)

# ── alignment-test ──────────────────────────────────────────────────
# Drives the pure comparison-alignment helper (computeComparisonAlignment)
# with synthetic omatrack::UnifiedLap inputs. Compiles the helper .cpp
# directly alongside the test so no QML/GUI or session parsing is needed;
# only the Qt-free core (omatrack_core) and Qt6::Test are linked.
qt_add_executable(alignment-test AlignmentTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/ComparisonAlignment.cpp)
target_include_directories(alignment-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(alignment-test PRIVATE
  omatrack_core
  Qt6::Test
  Qt6::Core
  omatrack_warnings)
add_test(NAME alignment-test COMMAND alignment-test)
set_tests_properties(alignment-test PROPERTIES LABELS unit)

# ── Track Atlas spatial station mapping ────────────────────────────
# Proves that corner markers are projected through WGS84 coordinates and do
# not inherit the telemetry lap's distance progress.
qt_add_executable(track-atlas-spatial-test TrackAtlasSpatialTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/TrackAtlasSpatial.cpp)
target_include_directories(track-atlas-spatial-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(track-atlas-spatial-test PRIVATE
  omatrack_core
  Qt6::Test
  Qt6::Core
  omatrack_warnings)
add_test(NAME track-atlas-spatial-test COMMAND track-atlas-spatial-test)
set_tests_properties(track-atlas-spatial-test PROPERTIES LABELS unit)

# ── CLI command contract ────────────────────────────────────────────
# Covers usage, dispatch, exit codes, and parser-error propagation without
# writing telemetry-derived output.
add_test(NAME cli-contract
  COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:omatrack-cli>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CliContract.cmake)
set_tests_properties(cli-contract PROPERTIES LABELS unit)

# ── Make failures readable on Windows ───────────────────────────────
# QTest only writes to stdout when stderr has a console attached. Under
# CTest it never does, so on Windows every PASS/FAIL line goes to the
# debugger through OutputDebugString instead and a failing test reports
# no output at all — the CI log shows "***Failed" and nothing else.
# Forcing stderr logging puts the assertion back in the log.
get_property(omatrack_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${omatrack_tests} PROPERTIES
  ENVIRONMENT QT_FORCE_STDERR_LOGGING=1)

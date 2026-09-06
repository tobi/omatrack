# Unit tests for omatrack_core and app types, run through CTest.
#
# Three executables, each linking only what it needs:
#   core-test    — Qt-free analysis core (links omatrack_core + Qt6::Test)
#   yamlconfig-test — YamlConfig CRUD/round-trip (compiles YamlConfig.cpp directly)
#   apptypes-test — Plain-struct contracts from TelemetryStore.h

find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

qt_add_executable(image-model-manager-test ImageModelManagerTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/AsyncJob.h
  ${CMAKE_SOURCE_DIR}/src/app/ImageModelManager.h
  ${CMAKE_SOURCE_DIR}/src/app/ImageModelManager.cpp
  ${CMAKE_SOURCE_DIR}/src/app/ImageModelManifest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/RemoteCache.cpp
  ${CMAKE_SOURCE_DIR}/src/app/VerboseLog.cpp
  ${CMAKE_SOURCE_DIR}/src/app/WebDavBackend.cpp
  ${CMAKE_SOURCE_DIR}/src/app/S3Backend.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SigV4.cpp
  ${CMAKE_SOURCE_DIR}/src/app/LibraryLocation.cpp)
target_include_directories(image-model-manager-test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/app)
target_link_libraries(image-model-manager-test PRIVATE
  Qt6::Test Qt6::Core Qt6::Network Qt6::Concurrent Qt6::Qml
  omatrack_core omatrack_inference omatrack_warnings)
add_test(NAME image-model-manager-test COMMAND image-model-manager-test)
set_tests_properties(image-model-manager-test PROPERTIES LABELS unit)

add_executable(image-scan-scheduler-test ImageScanSchedulerTest.cpp)
target_include_directories(image-scan-scheduler-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(image-scan-scheduler-test PRIVATE omatrack_warnings)
add_test(NAME image-scan-scheduler-test COMMAND image-scan-scheduler-test)
set_tests_properties(image-scan-scheduler-test PROPERTIES LABELS unit)

add_executable(image-telemetry-cache-test ImageTelemetryCacheTest.cpp)
target_link_libraries(image-telemetry-cache-test PRIVATE omatrack_image_cache ${OMATRACK_ZSTD_TARGET} omatrack_warnings)
add_test(NAME image-telemetry-cache-test COMMAND image-telemetry-cache-test)
set_tests_properties(image-telemetry-cache-test PROPERTIES LABELS unit)

qt_add_executable(image-telemetry-trace-test ImageTelemetryTraceGeometryTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/ImageTelemetryTraceGeometry.cpp
  ${CMAKE_SOURCE_DIR}/src/app/TraceDecimator.cpp
  ${CMAKE_SOURCE_DIR}/src/app/TraceSceneBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/app/TraceTextCache.cpp)
target_include_directories(image-telemetry-trace-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(image-telemetry-trace-test PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::Quick omatrack_warnings)
add_test(NAME image-telemetry-trace-test COMMAND image-telemetry-trace-test)
set_tests_properties(image-telemetry-trace-test PROPERTIES LABELS unit)

qt_add_executable(file-open-events-test FileOpenEventsTest.cpp)
target_include_directories(file-open-events-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(file-open-events-test PRIVATE Qt6::Test Qt6::Gui omatrack_warnings)
add_test(NAME file-open-events-test COMMAND file-open-events-test)
set_tests_properties(file-open-events-test PROPERTIES LABELS unit)

qt_add_executable(windows-associations-test windows_associations_test.cpp
  ${CMAKE_SOURCE_DIR}/src/app/WindowsAssociations.cpp)
target_include_directories(windows-associations-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(windows-associations-test PRIVATE Qt6::Test Qt6::Core omatrack_warnings)
if(WIN32)
  target_link_libraries(windows-associations-test PRIVATE advapi32 shell32)
endif()
add_test(NAME windows-associations-test COMMAND windows-associations-test)
set_tests_properties(windows-associations-test PROPERTIES LABELS unit)

add_executable(video-frame-decoder-test VideoFrameDecoderTest.cpp)
target_link_libraries(video-frame-decoder-test PRIVATE omatrack_inference omatrack_warnings)
add_test(NAME video-frame-decoder-test COMMAND video-frame-decoder-test)
set_tests_properties(video-frame-decoder-test PROPERTIES LABELS unit)

add_executable(gauge-reader-test gauge_reader_test.cpp
  ${CMAKE_SOURCE_DIR}/src/inference/GaugeReader.cpp)
target_include_directories(gauge-reader-test PRIVATE ${CMAKE_SOURCE_DIR}/src/inference)
target_compile_definitions(gauge-reader-test PRIVATE OMATRACK_GAUGE_READER_TESTING=1)
target_link_libraries(gauge-reader-test PRIVATE omatrack_warnings)
if(OMATRACK_ENABLE_IMAGE_TELEMETRY AND ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
  target_compile_definitions(gauge-reader-test PRIVATE OMATRACK_HAVE_ONNXRUNTIME=1)
  target_include_directories(gauge-reader-test PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
  target_link_libraries(gauge-reader-test PRIVATE "${ONNXRUNTIME_LIBRARY}")
endif()
add_test(NAME gauge-reader-test COMMAND gauge-reader-test)
set_tests_properties(gauge-reader-test PROPERTIES LABELS unit)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND OMATRACK_ENABLE_IMAGE_TELEMETRY AND
   ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
  add_library(ort-unavailable-api SHARED OrtUnavailableApi.cpp)
  target_include_directories(ort-unavailable-api PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
  target_link_libraries(ort-unavailable-api PRIVATE omatrack_warnings)
  add_test(NAME gauge-reader-incompatible-runtime-test
    COMMAND ${CMAKE_COMMAND} -E env "LD_PRELOAD=$<TARGET_FILE:ort-unavailable-api>"
      $<TARGET_FILE:gauge-reader-test>)
  set_tests_properties(gauge-reader-incompatible-runtime-test PROPERTIES LABELS unit)
endif()
set(OMATRACK_GAUGE_SMOKE_MODEL "$ENV{OMATRACK_GAUGE_SMOKE_MODEL}" CACHE FILEPATH
  "Pinned public model for native-runtime smoke verification")
if(OMATRACK_GAUGE_SMOKE_MODEL)
  add_test(NAME gauge-reader-public-model-test COMMAND gauge-reader-test
    --model-only "${OMATRACK_GAUGE_SMOKE_MODEL}")
  set_tests_properties(gauge-reader-public-model-test PROPERTIES LABELS integration)
endif()
set(OMATRACK_GAUGE_TEST_MODEL "" CACHE FILEPATH "Optional reviewed model for real image-reader parity tests")
set(OMATRACK_GAUGE_TEST_FIXTURES "" CACHE PATH "Optional generated, private reader fixture directory")
if(OMATRACK_GAUGE_TEST_MODEL AND OMATRACK_GAUGE_TEST_FIXTURES)
  add_test(NAME gauge-reader-parity-test COMMAND gauge-reader-test
    --model "${OMATRACK_GAUGE_TEST_MODEL}" --fixtures "${OMATRACK_GAUGE_TEST_FIXTURES}")
  set_tests_properties(gauge-reader-parity-test PROPERTIES LABELS integration)
endif()

qt_add_executable(filmstrip-layout-test FilmstripLayoutTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/FilmstripLayout.h)
target_include_directories(filmstrip-layout-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(filmstrip-layout-test PRIVATE Qt6::Test Qt6::Qml omatrack_warnings)
add_test(NAME filmstrip-layout-test COMMAND filmstrip-layout-test)
set_tests_properties(filmstrip-layout-test PROPERTIES LABELS unit)

qt_add_executable(session-refresh-test SessionRefreshTest.cpp)
target_include_directories(session-refresh-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(session-refresh-test PRIVATE Qt6::Test omatrack_warnings)
add_test(NAME session-refresh-test COMMAND session-refresh-test)
set_tests_properties(session-refresh-test PROPERTIES LABELS unit)

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

# ── Remote normalized-cache integration ─────────────────────────────
# Uses local protocol responders to verify listing, stub creation, and
# content-keyed cache helpers.
qt_add_executable(remote-cache-test RemoteCacheTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/RemoteCache.cpp
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
  omatrack_core
  omatrack_warnings)
add_test(NAME remote-cache-test COMMAND remote-cache-test)
set_tests_properties(remote-cache-test PROPERTIES LABELS unit)

# ── Lua trace-group plugins ────────────────────────────────────────────────
qt_add_executable(plugin-host-test PluginHostTest.cpp PluginHostTestScripts.cpp
  ${CMAKE_SOURCE_DIR}/src/app/AsyncJob.h
  ${CMAKE_SOURCE_DIR}/src/app/PluginHost.cpp
  ${CMAKE_SOURCE_DIR}/src/app/PathJail.cpp
  ${CMAKE_SOURCE_DIR}/src/app/RemoteCache.cpp
  ${CMAKE_SOURCE_DIR}/src/app/VerboseLog.cpp
  ${CMAKE_SOURCE_DIR}/src/app/WebDavBackend.cpp
  ${CMAKE_SOURCE_DIR}/src/app/S3Backend.cpp
  ${CMAKE_SOURCE_DIR}/src/app/SigV4.cpp
  ${CMAKE_SOURCE_DIR}/src/app/LibraryLocation.cpp)
target_include_directories(plugin-host-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(plugin-host-test PRIVATE
  Qt6::Test Qt6::Core Qt6::Network Qt6::Concurrent
  lua_sandbox sol2_headers omatrack_core omatrack_warnings)
add_test(NAME plugin-host-test COMMAND plugin-host-test)
set_tests_properties(plugin-host-test PROPERTIES LABELS unit)

qt_add_executable(overlay-resample-test OverlayResampleTest.cpp)
target_include_directories(overlay-resample-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(overlay-resample-test PRIVATE
  Qt6::Test Qt6::Core Qt6::Gui Qt6::Qml omatrack_warnings)
add_test(NAME overlay-resample-test COMMAND overlay-resample-test)
set_tests_properties(overlay-resample-test PROPERTIES LABELS unit)

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

qt_add_executable(library-model-test LibraryModelTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/LibraryModel.cpp
  ${CMAKE_SOURCE_DIR}/src/app/StoreModels.cpp)
target_include_directories(library-model-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(library-model-test PRIVATE
  Qt6::Test
  Qt6::Core
  Qt6::Gui
  Qt6::Qml
  omatrack_warnings)
add_test(NAME library-model-test COMMAND library-model-test)
set_tests_properties(library-model-test PROPERTIES LABELS unit)

qt_add_executable(lua-rename-test LuaRenameTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/LuaRename.cpp
  ${CMAKE_SOURCE_DIR}/src/app/PathJail.cpp)
target_include_directories(lua-rename-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lua-rename-test PRIVATE
  Qt6::Test
  Qt6::Core
  lua_sandbox
  sol2_headers
  omatrack_mpv
  omatrack_warnings)
add_test(NAME lua-rename-test COMMAND lua-rename-test)
set_tests_properties(lua-rename-test PROPERTIES LABELS unit)

qt_add_executable(scroll-anchor-test ScrollAnchorTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/ScrollAnchor.cpp)
target_include_directories(scroll-anchor-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(scroll-anchor-test PRIVATE Qt6::Test Qt6::Core Qt6::Gui
  Qt6::Qml Qt6::Quick omatrack_warnings)
add_test(NAME scroll-anchor-test COMMAND scroll-anchor-test)
set_tests_properties(scroll-anchor-test PROPERTIES LABELS unit
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(usb-copy-test UsbCopyTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/UsbCopy.cpp
  ${CMAKE_SOURCE_DIR}/src/app/PathJail.cpp
  ${CMAKE_SOURCE_DIR}/src/app/LuaRename.cpp)
target_include_directories(usb-copy-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(usb-copy-test PRIVATE Qt6::Test Qt6::Core
  lua_sandbox sol2_headers omatrack_warnings)
add_test(NAME usb-copy-test COMMAND usb-copy-test)
set_tests_properties(usb-copy-test PROPERTIES LABELS unit)

qt_add_executable(index-cache-test IndexCacheTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/IndexCache.cpp)
target_include_directories(index-cache-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(index-cache-test PRIVATE
  Qt6::Test
  Qt6::Core
  omatrack_core
  omatrack_warnings)
add_test(NAME index-cache-test COMMAND index-cache-test)
set_tests_properties(index-cache-test PROPERTIES LABELS unit)

qt_add_executable(trace-lane-sizing-test TraceLaneSizingTest.cpp)
target_include_directories(trace-lane-sizing-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(trace-lane-sizing-test PRIVATE Qt6::Test omatrack_warnings)
add_test(NAME trace-lane-sizing-test COMMAND trace-lane-sizing-test)
set_tests_properties(trace-lane-sizing-test PROPERTIES LABELS unit)

qt_add_executable(overlay-style-test OverlayStyleTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/TraceDecimator.cpp
  ${CMAKE_SOURCE_DIR}/src/app/TraceSceneBuilder.cpp
  ${CMAKE_SOURCE_DIR}/src/app/TraceTextCache.cpp)
target_include_directories(overlay-style-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(overlay-style-test PRIVATE
  Qt6::Test
  Qt6::Core
  Qt6::Gui
  Qt6::Quick
  omatrack_warnings)
add_test(NAME overlay-style-test COMMAND overlay-style-test)
set_tests_properties(overlay-style-test PROPERTIES LABELS unit)

qt_add_executable(deadline-queue-test DeadlineQueueTest.cpp
  ${CMAKE_SOURCE_DIR}/src/app/DeadlineQueue.cpp)
target_include_directories(deadline-queue-test PRIVATE
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(deadline-queue-test PRIVATE
  Qt6::Test Qt6::Core omatrack_warnings)
add_test(NAME deadline-queue-test COMMAND deadline-queue-test)
set_tests_properties(deadline-queue-test PROPERTIES LABELS unit)

# ── Omarchy theme bridge test ───────────────────────────────────────
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
# The same commands dispatched from the shipped binary, before Qt starts:
# no display, no omatrack.yml, identical output.
add_test(NAME headless-contract
  COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:omatrack> -DGUI=1
    -P ${CMAKE_CURRENT_SOURCE_DIR}/CliContract.cmake)
set_tests_properties(headless-contract PROPERTIES LABELS unit
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Build every unit-test binary (and omatrack-cli) without the GUI. Used by
# `make memcheck` so ASan/UBSan can run the suite without compiling omatrack.
add_custom_target(unit_tests
  DEPENDS
    filmstrip-layout-test
    session-refresh-test
    core-test
    corner-analysis-test
    yamlconfig-test
    appupdate-test
    apptypes-test
    sigv4-test
    remote-cache-test
    plugin-host-test
    overlay-resample-test
    usb-media-test
    library-model-test
    lua-rename-test
    scroll-anchor-test
    usb-copy-test
    index-cache-test
    overlay-style-test
    trace-lane-sizing-test
    deadline-queue-test
    omarchy-theme-test
    alignment-test
    track-atlas-spatial-test
    omatrack-cli
  COMMENT "Unit-test binaries (no GUI)")

# ── Make failures readable on Windows ───────────────────────────────
# QTest only writes to stdout when stderr has a console attached. Under
# CTest it never does, so on Windows every PASS/FAIL line goes to the
# debugger through OutputDebugString instead and a failing test reports
# no output at all — the CI log shows "***Failed" and nothing else.
# Forcing stderr logging puts the assertion back in the log.
# ENVIRONMENT is a list property that set_tests_properties() replaces, so
# append per test to keep e.g. scroll-anchor-test's QT_QPA_PLATFORM.
get_property(omatrack_tests DIRECTORY PROPERTY TESTS)
foreach(omatrack_test IN LISTS omatrack_tests)
  set_property(TEST ${omatrack_test} APPEND PROPERTY
    ENVIRONMENT QT_FORCE_STDERR_LOGGING=1)
endforeach()

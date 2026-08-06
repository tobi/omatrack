# Formatting and static-analysis gates.
#
# Every linter is reachable two ways: as a build target for local use
# (`cmake --build build --target lint`) and as a CTest entry labelled `lint`,
# so `ctest` covers formatting and static analysis next to the parser tests.
#
# Qt's tools are used through the versioned paths that come with the Qt6
# package, never through PATH: distros ship Qt 5 binaries with the same names
# (`qmllint`, `qmlformat`), and those reject Qt 6 QML outright.

set(OMATRACK_CPP_SOURCES
  ${CMAKE_SOURCE_DIR}/src/core/TelemetryEngine.cpp
  ${CMAKE_SOURCE_DIR}/src/core/TelemetryEngine.h
  ${CMAKE_SOURCE_DIR}/cli/main.cpp)
file(GLOB OMATRACK_APP_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/app/*.cpp
  ${CMAKE_SOURCE_DIR}/src/app/*.h)
list(APPEND OMATRACK_CPP_SOURCES ${OMATRACK_APP_SOURCES})

file(GLOB OMATRACK_QML_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/app/*.qml)

add_custom_target(lint COMMENT "Run every formatter check and static analyser")

# ── clang-format ────────────────────────────────────────────────────
find_program(CLANG_FORMAT_PROGRAM NAMES clang-format)
if(CLANG_FORMAT_PROGRAM)
  add_custom_target(cpp_format_check
    COMMAND ${CLANG_FORMAT_PROGRAM} --dry-run --Werror ${OMATRACK_CPP_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "clang-format --dry-run over C++ sources"
    VERBATIM)
  add_custom_target(cpp_format
    COMMAND ${CLANG_FORMAT_PROGRAM} -i ${OMATRACK_CPP_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "clang-format -i over C++ sources"
    VERBATIM)
  add_dependencies(lint cpp_format_check)
  add_test(NAME lint-cpp-format
    COMMAND ${CLANG_FORMAT_PROGRAM} --dry-run --Werror ${OMATRACK_CPP_SOURCES})
  set_tests_properties(lint-cpp-format PROPERTIES LABELS lint)
else()
  message(WARNING "clang-format not found: C++ formatting is unchecked")
endif()

# ── clang-tidy (opt-in) ─────────────────────────────────────────────
# The curated set in .clang-tidy still reports findings in the existing
# sources, so tidy is not part of `lint` yet: turning it on by default would
# fail builds for reasons unrelated to the change being made. Enable it
# explicitly while burning those findings down.
option(OMATRACK_CLANG_TIDY "Add the clang-tidy lint target and test" OFF)
if(OMATRACK_CLANG_TIDY)
  find_program(CLANG_TIDY_PROGRAM NAMES clang-tidy REQUIRED)
  add_custom_target(cpp_tidy
    COMMAND ${CLANG_TIDY_PROGRAM} -p ${CMAKE_BINARY_DIR} --quiet
            ${OMATRACK_CPP_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "clang-tidy over C++ sources"
    VERBATIM)
  add_dependencies(lint cpp_tidy)
  add_test(NAME lint-cpp-tidy
    COMMAND ${CLANG_TIDY_PROGRAM} -p ${CMAKE_BINARY_DIR} --quiet
            --warnings-as-errors=* ${OMATRACK_CPP_SOURCES})
  set_tests_properties(lint-cpp-tidy PROPERTIES
    LABELS lint
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ── qmlformat ───────────────────────────────────────────────────────
if(TARGET Qt6::qmlformat)
  get_target_property(QMLFORMAT_PROGRAM Qt6::qmlformat IMPORTED_LOCATION)
  set(QML_FORMAT_CHECK_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/QmlFormatCheck.cmake)
  add_custom_target(qml_format_check
    COMMAND ${CMAKE_COMMAND}
      -DQMLFORMAT=${QMLFORMAT_PROGRAM}
      "-DFILES=${OMATRACK_QML_SOURCES}"
      -P ${QML_FORMAT_CHECK_SCRIPT}
    COMMENT "qmlformat check over QML sources"
    VERBATIM)
  add_custom_target(qml_format
    COMMAND ${QMLFORMAT_PROGRAM} -i ${OMATRACK_QML_SOURCES}
    COMMENT "qmlformat -i over QML sources"
    VERBATIM)
  add_dependencies(lint qml_format_check)
  add_test(NAME lint-qml-format
    COMMAND ${CMAKE_COMMAND}
      -DQMLFORMAT=${QMLFORMAT_PROGRAM}
      "-DFILES=${OMATRACK_QML_SOURCES}"
      -P ${QML_FORMAT_CHECK_SCRIPT})
  set_tests_properties(lint-qml-format PROPERTIES LABELS lint)
else()
  message(WARNING "Qt6::qmlformat missing: QML formatting is unchecked")
endif()

# ── qmllint ─────────────────────────────────────────────────────────
# qt_add_qml_module generates all_qmllint with the module's import paths
# already resolved, so drive that instead of re-deriving them here.
add_custom_target(qml_lint
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all_qmllint
  COMMENT "qmllint over the Omatrack QML module"
  VERBATIM)
add_dependencies(lint qml_lint)
add_test(NAME lint-qml
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all_qmllint)
set_tests_properties(lint-qml PROPERTIES LABELS lint)

# ── QML invariants qmllint does not cover ───────────────────────────
set(QML_PRAGMA_CHECK_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/QmlPragmaCheck.cmake)
add_custom_target(qml_pragma_check
  COMMAND ${CMAKE_COMMAND} "-DFILES=${OMATRACK_QML_SOURCES}"
          -P ${QML_PRAGMA_CHECK_SCRIPT}
  COMMENT "ComponentBehavior: Bound in every QML file"
  VERBATIM)
add_dependencies(lint qml_pragma_check)
add_test(NAME lint-qml-pragma
  COMMAND ${CMAKE_COMMAND} "-DFILES=${OMATRACK_QML_SOURCES}"
          -P ${QML_PRAGMA_CHECK_SCRIPT})
set_tests_properties(lint-qml-pragma PROPERTIES LABELS lint)

# ── rust ────────────────────────────────────────────────────────────
# rust_clippy is defined with the vendored workspace in third_party/.
add_dependencies(lint rust_clippy)
add_test(NAME lint-rust-clippy
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR}
          cargo clippy --release --all-targets
          --manifest-path ${CMAKE_SOURCE_DIR}/third_party/motorsport-telemetry/Cargo.toml
          -- -D warnings)
set_tests_properties(lint-rust-clippy PROPERTIES LABELS lint)

add_custom_target(rust_format_check
  COMMAND cargo fmt --all --check
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/motorsport-telemetry
  COMMENT "cargo fmt --check over the vendored parser workspace"
  VERBATIM)
add_dependencies(lint rust_format_check)
add_test(NAME lint-rust-format
  COMMAND cargo fmt --all --check
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/motorsport-telemetry)
set_tests_properties(lint-rust-format PROPERTIES LABELS lint)

# Thin C ABI over the pinned upstream motorsport-telemetry-rs crates. No vendor
# decoder source lives in this repository.

set(RUST_WORKSPACE ${CMAKE_CURRENT_SOURCE_DIR}/motorsport-telemetry)
set(RUST_TARGET_DIR ${CMAKE_BINARY_DIR}/cargo-target)
set(RUST_TARGET_DIR ${RUST_TARGET_DIR} PARENT_SCOPE)
set(_OMATRACK_RUST_DEBUG_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_OMATRACK_RUST_DEBUG_DEFAULT ON)
endif()
option(OMATRACK_RUST_DEBUG
  "Build the Rust parser bridge with debug symbols and assertions"
  ${_OMATRACK_RUST_DEBUG_DEFAULT})
if(OMATRACK_RUST_DEBUG)
  set(RUST_PROFILE_DIR ${RUST_TARGET_DIR}/debug)
  set(RUST_BUILD_ARGS)
else()
  set(RUST_PROFILE_DIR ${RUST_TARGET_DIR}/release)
  set(RUST_BUILD_ARGS --release)
endif()
set(RUST_STATICLIB
  ${RUST_PROFILE_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}omatrack_bridge${CMAKE_STATIC_LIBRARY_SUFFIX})
set(RUST_GENERATED_INCLUDE_DIR
  ${CMAKE_BINARY_DIR}/generated/motorsport-telemetry)
set(RUST_GENERATED_HEADER
  ${RUST_GENERATED_INCLUDE_DIR}/omatrack_bridge.h)
# CMake validates imported-target include directories while generating the
# build graph, before the custom command has generated the header itself.
file(MAKE_DIRECTORY ${RUST_GENERATED_INCLUDE_DIR})

if(MSVC)
  message(FATAL_ERROR
    "Windows builds currently require the MSYS2 UCRT64 MinGW toolchain")
endif()


set(RUST_CRATES omatrack-bridge)

# Depend on every Rust source, not a hand-picked subset: a stale list silently
# skips rebuilds when a decoder changes. CONFIGURE_DEPENDS re-runs the glob when
# files are added or removed.
file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS
  ${RUST_WORKSPACE}/bridge/*.rs
  ${RUST_WORKSPACE}/bridge/Cargo.toml
  ${RUST_WORKSPACE}/bridge/cbindgen.toml
  ${RUST_WORKSPACE}/bridge-header/*.rs
  ${RUST_WORKSPACE}/bridge-header/Cargo.toml)
list(APPEND RUST_SOURCES
  ${RUST_WORKSPACE}/Cargo.toml
  ${RUST_WORKSPACE}/Cargo.lock)

add_custom_command(
  OUTPUT ${RUST_STATICLIB} ${RUST_GENERATED_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${RUST_GENERATED_INCLUDE_DIR}
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR}
          cargo run --quiet --locked
          --package omatrack-bridge-header
          --manifest-path ${RUST_WORKSPACE}/Cargo.toml
          -- ${RUST_GENERATED_HEADER}
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR}
          cargo build --locked ${RUST_BUILD_ARGS}
          --package omatrack-bridge
          --manifest-path ${RUST_WORKSPACE}/Cargo.toml
  COMMAND ${CMAKE_COMMAND} -E touch_nocreate
          ${RUST_GENERATED_HEADER} ${RUST_STATICLIB}
  WORKING_DIRECTORY ${RUST_WORKSPACE}
  DEPENDS ${RUST_SOURCES}
  COMMENT "Building upstream Rust telemetry bridge"
  VERBATIM)

add_custom_target(rust_bridge
  DEPENDS ${RUST_STATICLIB} ${RUST_GENERATED_HEADER})

add_library(omatrack_rust_bridge STATIC IMPORTED GLOBAL)
set_target_properties(omatrack_rust_bridge PROPERTIES
  IMPORTED_LOCATION ${RUST_STATICLIB})
add_dependencies(omatrack_rust_bridge rust_bridge)

# The archive is pulled in whole: its exports are reached only through the C
# ABI, so the linker cannot see the references that keep them alive.
target_link_directories(omatrack_rust_bridge INTERFACE ${RUST_PROFILE_DIR})
target_include_directories(omatrack_rust_bridge INTERFACE
  ${RUST_GENERATED_INCLUDE_DIR})
target_link_options(omatrack_rust_bridge INTERFACE
  -Wl,--whole-archive -lomatrack_bridge -Wl,--no-whole-archive)
if(WIN32)
  target_link_libraries(omatrack_rust_bridge INTERFACE
    ws2_32 userenv advapi32 bcrypt ntdll)
else()
  target_link_libraries(omatrack_rust_bridge INTERFACE pthread dl m)
endif()

# The bridge tests exercise the pinned parser adapters. Upstream format-crate
# tests run in the motorsport-telemetry-rs project itself.
foreach(crate IN LISTS RUST_CRATES)
  add_test(NAME rust-${crate}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR}
            cargo test -p ${crate}
            --manifest-path ${RUST_WORKSPACE}/Cargo.toml)
endforeach()

# Lint target; not part of `all` because clippy is slower than a build.
add_custom_target(rust_clippy
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR}
          cargo clippy --release --workspace --all-targets
          --manifest-path ${RUST_WORKSPACE}/Cargo.toml -- -D warnings
  WORKING_DIRECTORY ${RUST_WORKSPACE}
  COMMENT "cargo clippy over the Rust telemetry bridge"
  VERBATIM)

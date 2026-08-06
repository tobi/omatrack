# Vendored Rust parser workspace.
#
# Every vendor decoder lives in its own crate; `bridge` is the single panic-safe
# C ABI the C++ side links against. The crates are compiled individually by
# cargo and archived into one staticlib, so the C++ build has exactly one
# artifact to link but each format stays independently buildable and testable
# (`ctest -R rust-`).

set(RUST_WORKSPACE ${CMAKE_CURRENT_SOURCE_DIR}/motorsport-telemetry)
set(RUST_TARGET_DIR ${CMAKE_BINARY_DIR}/cargo-target)
set(RUST_TARGET_DIR ${RUST_TARGET_DIR} PARENT_SCOPE)
set(RUST_STATICLIB
  ${RUST_TARGET_DIR}/release/${CMAKE_STATIC_LIBRARY_PREFIX}omatrack_bridge${CMAKE_STATIC_LIBRARY_SUFFIX})

if(MSVC)
  message(FATAL_ERROR
    "Windows builds currently require the MSYS2 UCRT64 MinGW toolchain")
endif()


# Cargo package names, in dependency order. `motorsport-telemetry-core` carries
# the shared traits and value types; `omatrack-bridge` re-exports every
# decoder through the C ABI.
set(RUST_CRATES
  motorsport-telemetry-core
  cosworth-telemetry
  aim-telemetry
  motec-telemetry
  vbo-telemetry
  omatrack-bridge)

# Depend on every Rust source, not a hand-picked subset: a stale list silently
# skips rebuilds when a decoder changes. CONFIGURE_DEPENDS re-runs the glob when
# files are added or removed.
file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS
  ${RUST_WORKSPACE}/crates/*.rs
  ${RUST_WORKSPACE}/bridge/*.rs
  ${RUST_WORKSPACE}/crates/*/Cargo.toml
  ${RUST_WORKSPACE}/bridge/Cargo.toml)
list(APPEND RUST_SOURCES
  ${RUST_WORKSPACE}/Cargo.toml
  ${RUST_WORKSPACE}/Cargo.lock)

add_custom_command(
  OUTPUT ${RUST_STATICLIB}
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR}
          cargo build --release --manifest-path ${RUST_WORKSPACE}/Cargo.toml
  WORKING_DIRECTORY ${RUST_WORKSPACE}
  DEPENDS ${RUST_SOURCES}
  COMMENT "Building Rust telemetry parsers (${RUST_CRATES} + bridge)"
  VERBATIM)

add_custom_target(rust_bridge DEPENDS ${RUST_STATICLIB})

add_library(omatrack_rust_bridge STATIC IMPORTED GLOBAL)
set_target_properties(omatrack_rust_bridge PROPERTIES
  IMPORTED_LOCATION ${RUST_STATICLIB})
add_dependencies(omatrack_rust_bridge rust_bridge)

# The archive is pulled in whole: its exports are reached only through the C
# ABI, so the linker cannot see the references that keep them alive.
target_link_directories(omatrack_rust_bridge INTERFACE ${RUST_TARGET_DIR}/release)
target_link_options(omatrack_rust_bridge INTERFACE
  -Wl,--whole-archive -lomatrack_bridge -Wl,--no-whole-archive)
if(WIN32)
  target_link_libraries(omatrack_rust_bridge INTERFACE
    ws2_32 userenv advapi32 bcrypt ntdll)
else()
  target_link_libraries(omatrack_rust_bridge INTERFACE pthread dl m)
endif()

# One CTest entry per crate so a format regression names the format.
foreach(crate IN LISTS RUST_CRATES)
  add_test(NAME rust-${crate}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR}
            cargo test --release -p ${crate}
            --manifest-path ${RUST_WORKSPACE}/Cargo.toml)
endforeach()

# Lint target; not part of `all` because clippy is slower than a build.
add_custom_target(rust_clippy
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR}
          cargo clippy --release --all-targets
          --manifest-path ${RUST_WORKSPACE}/Cargo.toml -- -D warnings
  WORKING_DIRECTORY ${RUST_WORKSPACE}
  COMMENT "cargo clippy over the vendored parser workspace"
  VERBATIM)

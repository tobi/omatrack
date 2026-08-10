cmake_minimum_required(VERSION 3.21)
project(omatrack VERSION 0.9.6 LANGUAGES CXX)
if(WIN32)
  # Windows release layout: the app ships as a flat zip, so executables and
  # their load-time DLLs install at the prefix root and every non-binary
  # payload lands under lib/. src/app/CMakeLists.txt deploys Qt with a
  # matching qt.conf so the flat tree runs standalone.
  set(CMAKE_INSTALL_BINDIR "." CACHE STRING
    "Windows release binary directory (flat app root)")
  set(CMAKE_INSTALL_LIBDIR "lib" CACHE STRING
    "Windows release library directory")
  set(CMAKE_INSTALL_DATADIR "lib/share" CACHE STRING
    "Windows release data directory")
  set(CMAKE_INSTALL_DOCDIR "lib/share/doc" CACHE STRING
    "Windows release documentation directory")
endif()
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Binaries stay at the top of the build tree: ./build/omatrack, ./build/omatrack-cli.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS
  Concurrent Core Gui Quick QuickControls2 Qml Network)
if(WIN32)
  get_filename_component(OMATRACK_TOOLCHAIN_BIN_DIR
    "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(OMATRACK_TOOLCHAIN_PREFIX
    "${OMATRACK_TOOLCHAIN_BIN_DIR}" DIRECTORY)
  find_path(OMATRACK_MPV_INCLUDE_DIR mpv/client.h
    HINTS "${OMATRACK_TOOLCHAIN_PREFIX}/include" REQUIRED)
  find_library(OMATRACK_MPV_LIBRARY mpv
    HINTS "${OMATRACK_TOOLCHAIN_PREFIX}/lib" REQUIRED)
  find_path(OMATRACK_YAML_INCLUDE_DIR yaml.h
    HINTS "${OMATRACK_TOOLCHAIN_PREFIX}/include" REQUIRED)
  find_library(OMATRACK_YAML_LIBRARY yaml
    HINTS "${OMATRACK_TOOLCHAIN_PREFIX}/lib" REQUIRED)

  add_library(omatrack_mpv INTERFACE)
  target_include_directories(omatrack_mpv INTERFACE
    "${OMATRACK_MPV_INCLUDE_DIR}")
  target_link_libraries(omatrack_mpv INTERFACE "${OMATRACK_MPV_LIBRARY}")
  add_library(omatrack_yaml INTERFACE)
  target_include_directories(omatrack_yaml INTERFACE
    "${OMATRACK_YAML_INCLUDE_DIR}")
  target_link_libraries(omatrack_yaml INTERFACE "${OMATRACK_YAML_LIBRARY}")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBMPV REQUIRED IMPORTED_TARGET mpv)
  pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)
  add_library(omatrack_mpv INTERFACE)
  target_link_libraries(omatrack_mpv INTERFACE PkgConfig::LIBMPV)
  add_library(omatrack_yaml INTERFACE)
  target_link_libraries(omatrack_yaml INTERFACE PkgConfig::LIBYAML)
endif()

qt_standard_project_setup(REQUIRES 6.5)

# Write .qmlls.ini next to the QML sources so the QML Language Server resolves
# the module without per-editor configuration.
set(QT_QML_GENERATE_QMLLS_INI ON)

# Warnings are opt-in per target so Qt-generated sources and the Rust
# bridge are not held to them.
add_library(omatrack_warnings INTERFACE)
target_compile_options(omatrack_warnings INTERFACE
  -Wall -Wextra -Wno-unused-parameter)

enable_testing()
option(OMATRACK_ENABLE_AUTOTEST_HARNESS
  "Compile the state-mutating GUI acceptance harness" OFF)
option(OMATRACK_STATIC_GNU_RUNTIME
  "Statically link the GNU C++ and compiler runtimes into executables" OFF)

add_library(omatrack_portable_runtime INTERFACE)
if(OMATRACK_STATIC_GNU_RUNTIME)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
      "OMATRACK_STATIC_GNU_RUNTIME requires the GNU C++ compiler")
  endif()
  target_link_options(omatrack_portable_runtime INTERFACE
    -static-libgcc -static-libstdc++)
endif()

add_subdirectory(third_party)
add_subdirectory(src/core)
add_subdirectory(src/app)
add_subdirectory(cli)
add_subdirectory(tests)

# Formatting and static-analysis gates. Included last: it wires targets that
# qt_add_qml_module and the Rust workspace create in the subdirectories above.
include(cmake/OmatrackLint.cmake)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR})

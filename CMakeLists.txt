cmake_minimum_required(VERSION 3.21)
project(racecraft_qt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Binaries stay at the top of the build tree: ./build/racecraft, ./build/racecraft-cli.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS
  Core Gui Quick QuickControls2 Widgets Qml Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBMPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)

qt_standard_project_setup(REQUIRES 6.5)

# Write .qmlls.ini next to the QML sources so the QML Language Server resolves
# the module without per-editor configuration.
set(QT_QML_GENERATE_QMLLS_INI ON)

# Warnings are opt-in per target so Qt-generated sources and the vendored Rust
# bridge are not held to them.
add_library(racecraft_warnings INTERFACE)
target_compile_options(racecraft_warnings INTERFACE
  -Wall -Wextra -Wno-unused-parameter)

enable_testing()

add_subdirectory(third_party)
add_subdirectory(src/core)
add_subdirectory(src/app)
add_subdirectory(cli)

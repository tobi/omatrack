# Headless commands (parse, unify, corners, compare). Qt-free; linked into
# the `omatrack` binary, which dispatches them before Qt starts.
add_library(omatrack_headless STATIC Headless.cpp Headless.h)
target_include_directories(omatrack_headless PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(omatrack_headless PUBLIC omatrack_core
  PRIVATE omatrack_warnings)

# Test-only thin binary over the same commands, so CTest and the benchmark
# scripts run them without a Qt build or a display. Not installed, not shipped.
add_executable(omatrack-cli main.cpp)
target_compile_definitions(omatrack-cli PRIVATE
  OMATRACK_VERSION="${PROJECT_VERSION}")
target_link_libraries(omatrack-cli PRIVATE
  omatrack_headless
  omatrack_portable_runtime
  omatrack_warnings)

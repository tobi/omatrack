# Headless acceptance surface: parse, map, detect laps, unify. No Qt.

add_executable(omatrack-cli main.cpp)
target_compile_definitions(omatrack-cli PRIVATE
  OMATRACK_VERSION="${PROJECT_VERSION}")
target_link_libraries(omatrack-cli PRIVATE
  omatrack_core
  omatrack_portable_runtime
  omatrack_warnings)
install(TARGETS omatrack-cli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

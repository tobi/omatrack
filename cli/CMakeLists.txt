# Headless acceptance surface: parse, map, detect laps, unify. No Qt.

add_executable(racecraft-cli main.cpp)
target_link_libraries(racecraft-cli PRIVATE racecraft_core racecraft_warnings)

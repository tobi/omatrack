# Script mode: verify QML files are already qmlformat-clean.
#
# qmlformat has no --check mode, so format to stdout and compare. Run as
#   cmake -DQMLFORMAT=<exe> -DFILES=<a.qml;b.qml> -P QmlFormatCheck.cmake
# Formatting settings come from .qmlformat.ini next to the sources.

if(NOT QMLFORMAT OR NOT FILES)
  message(FATAL_ERROR "QmlFormatCheck.cmake needs -DQMLFORMAT= and -DFILES=")
endif()

set(dirty "")
foreach(file IN LISTS FILES)
  execute_process(
    COMMAND ${QMLFORMAT} ${file}
    OUTPUT_VARIABLE formatted
    ERROR_VARIABLE errors
    RESULT_VARIABLE status
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "qmlformat failed on ${file}: ${errors}")
  endif()
  file(READ ${file} original)
  string(STRIP "${original}" original)
  if(NOT formatted STREQUAL original)
    list(APPEND dirty ${file})
  endif()
endforeach()

if(dirty)
  string(REPLACE ";" "\n  " report "${dirty}")
  message(FATAL_ERROR
    "qmlformat would rewrite:\n  ${report}\n"
    "Run: cmake --build <builddir> --target qml_format")
endif()
message(STATUS "qmlformat: all files clean")

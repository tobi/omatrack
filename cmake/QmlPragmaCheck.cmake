# Script mode: enforce `pragma ComponentBehavior: Bound` in every QML file.
#
# qmllint does not check this, and the pragma is not cosmetic: without it a
# delegate silently captures its context instead of binding required
# properties, which is exactly the class of bug the invariant exists to stop.
#
# Run as: cmake "-DFILES=<a.qml;b.qml>" -P QmlPragmaCheck.cmake

if(NOT FILES)
  message(FATAL_ERROR "QmlPragmaCheck.cmake needs -DFILES=")
endif()

set(missing "")
foreach(file IN LISTS FILES)
  file(READ ${file} contents)
  if(NOT contents MATCHES "pragma[ \t]+ComponentBehavior:[ \t]*Bound")
    list(APPEND missing ${file})
  endif()
endforeach()

if(missing)
  string(REPLACE ";" "\n  " report "${missing}")
  message(FATAL_ERROR
    "Missing `pragma ComponentBehavior: Bound`:\n  ${report}")
endif()
message(STATUS "qml pragma: all files declare ComponentBehavior: Bound")

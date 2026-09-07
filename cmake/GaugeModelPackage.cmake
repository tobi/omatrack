# Build-time-only, immutable public bundle. No runtime download ceremony.
set(OMATRACK_GAUGE_BUNDLE "$ENV{OMATRACK_GAUGE_BUNDLE}" CACHE PATH
  "Verified offline reader/detectors/companions/notices bundle directory")
option(OMATRACK_REQUIRE_GAUGE_BUNDLE "Require all release model assets" "$ENV{OMATRACK_REQUIRE_GAUGE_BUNDLE}")
if(OMATRACK_REQUIRE_GAUGE_BUNDLE AND NOT OMATRACK_GAUGE_BUNDLE)
  message(FATAL_ERROR "Release/CI requires OMATRACK_GAUGE_BUNDLE; run scripts/fetch-gauge-bundle.sh")
endif()
if(OMATRACK_GAUGE_BUNDLE)
  if(NOT OMATRACK_ENABLE_IMAGE_TELEMETRY OR NOT ONNXRUNTIME_LIBRARY OR
     NOT TARGET PkgConfig::GAUGE_FFMPEG)
    message(FATAL_ERROR "OMATRACK_GAUGE_BUNDLE requires ONNX Runtime and FFmpeg")
  endif()
  include(${CMAKE_CURRENT_LIST_DIR}/GaugeBundle.cmake)
  omatrack_verify_gauge_bundle("${OMATRACK_GAUGE_BUNDLE}")
  omatrack_gauge_bundle_files(_gauge_files)
  set_property(TARGET omatrack APPEND PROPERTY LINK_DEPENDS "${OMATRACK_GAUGE_BUNDLE_MANIFEST}")
  add_custom_command(TARGET omatrack POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DMODE=stage
      "-DSOURCE_DIR=${OMATRACK_GAUGE_BUNDLE}"
      "-DBUNDLE_DIR=$<TARGET_FILE_DIR:omatrack>/models"
      -P "${PROJECT_SOURCE_DIR}/scripts/gauge-bundle.cmake"
    VERBATIM)
  if(APPLE)
    set(_gauge_install_dir "Omatrack.app/Contents/MacOS/models")
  else()
    set(_gauge_install_dir "${CMAKE_INSTALL_BINDIR}/models")
  endif()
  configure_file("${CMAKE_CURRENT_LIST_DIR}/GaugeBundleInstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/GaugeBundleInstall.cmake" @ONLY)
  install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/GaugeBundleInstall.cmake")
  foreach(_gauge_file IN LISTS _gauge_files)
    get_filename_component(_gauge_parent "${_gauge_file}" DIRECTORY)
    install(FILES "${OMATRACK_GAUGE_BUNDLE}/${_gauge_file}"
      DESTINATION "${_gauge_install_dir}/${_gauge_parent}")
  endforeach()
  install(FILES "${OMATRACK_GAUGE_BUNDLE_MANIFEST}" DESTINATION "${_gauge_install_dir}")
  message(STATUS "Bundling verified offline reader + tiny/general and large/AiM detectors with all notices")
endif()

if(WIN32 AND OMATRACK_ENABLE_IMAGE_TELEMETRY AND ONNXRUNTIME_RUNTIME_FILES)
  install(FILES ${ONNXRUNTIME_RUNTIME_FILES} DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# Distribution Linux builds use system Qt/mpv, but the explicitly selected ORT SDK
# is not necessarily system-installed. Include its runtime and notices in the
# installation tree. Windows/macOS use Qt's deployment scanner for dependencies.
if(UNIX AND NOT APPLE AND OMATRACK_ENABLE_IMAGE_TELEMETRY AND
   ONNXRUNTIME_ROOT AND ONNXRUNTIME_LIBRARY)
  get_filename_component(_ort_real_library "${ONNXRUNTIME_LIBRARY}" REALPATH)
  install(FILES "${_ort_real_library}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  if(EXISTS "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so.1")
    install(FILES "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so.1"
      DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  endif()
  file(RELATIVE_PATH _ort_relative_lib "/${CMAKE_INSTALL_BINDIR}" "/${CMAKE_INSTALL_LIBDIR}")
  set_property(TARGET omatrack APPEND PROPERTY INSTALL_RPATH "$ORIGIN/${_ort_relative_lib}")
endif()
if(OMATRACK_ENABLE_IMAGE_TELEMETRY AND ONNXRUNTIME_ROOT AND ONNXRUNTIME_LIBRARY)
  foreach(_notice LICENSE ThirdPartyNotices.txt)
    if(EXISTS "${ONNXRUNTIME_ROOT}/${_notice}")
      install(FILES "${ONNXRUNTIME_ROOT}/${_notice}"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/onnxruntime")
    elseif(EXISTS "${ONNXRUNTIME_ROOT}/share/licenses/onnxruntime/${_notice}")
      install(FILES "${ONNXRUNTIME_ROOT}/share/licenses/onnxruntime/${_notice}"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/onnxruntime")
    endif()
  endforeach()
endif()

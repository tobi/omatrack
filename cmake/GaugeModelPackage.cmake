# Opt-in, local model deployment. Never downloads or publishes private weights.
set(OMATRACK_GAUGE_MODEL "" CACHE FILEPATH
  "Reviewed local gauge-reader.onnx to stage beside this private application build")
if(OMATRACK_GAUGE_MODEL)
  if(NOT OMATRACK_ENABLE_IMAGE_TELEMETRY OR NOT ONNXRUNTIME_LIBRARY OR
     NOT TARGET PkgConfig::GAUGE_FFMPEG)
    message(FATAL_ERROR "OMATRACK_GAUGE_MODEL requires the ONNX Runtime SDK and FFmpeg decoder")
  endif()
  if(NOT EXISTS "${OMATRACK_GAUGE_MODEL}")
    message(FATAL_ERROR "OMATRACK_GAUGE_MODEL does not exist")
  endif()
  file(SHA256 "${OMATRACK_GAUGE_MODEL}" _gauge_model_hash)
  if(NOT _gauge_model_hash STREQUAL
      "97029f70068f4ec276b3d6bc28810763275806f579d91ddd4701b544af392147")
    message(FATAL_ERROR "Gauge model is not the verified current-reader export; reproduce the documented pinned export")
  endif()
  add_custom_command(TARGET omatrack POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:omatrack>/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OMATRACK_GAUGE_MODEL}"
      "$<TARGET_FILE_DIR:omatrack>/models/gauge-reader.onnx"
    VERBATIM)
  if(APPLE)
    set(_gauge_install_dir "Omatrack.app/Contents/MacOS/models")
  else()
    set(_gauge_install_dir "${CMAKE_INSTALL_BINDIR}/models")
  endif()
  install(FILES "${OMATRACK_GAUGE_MODEL}"
    DESTINATION "${_gauge_install_dir}" RENAME gauge-reader.onnx)
  message(STATUS "Staging trusted local gauge model; this grants no redistribution rights")
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

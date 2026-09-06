# Optional native image-reader runtime. A build without either dependency remains
# a normal telemetry/video player and explicitly reports extraction unavailable.
option(OMATRACK_ENABLE_IMAGE_TELEMETRY "Enable image-derived video telemetry" ON)
set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime C/C++ SDK prefix")
option(OMATRACK_REQUIRE_IMAGE_TELEMETRY "Fail configure if the image-reader runtime is unavailable"
  "$ENV{OMATRACK_REQUIRE_IMAGE_TELEMETRY}")
add_library(omatrack_inference STATIC
  ${PROJECT_SOURCE_DIR}/src/inference/GaugeReader.cpp
  ${PROJECT_SOURCE_DIR}/src/inference/VideoFrameDecoder.cpp)
target_include_directories(omatrack_inference PUBLIC
  ${PROJECT_SOURCE_DIR}/src/inference)
target_link_libraries(omatrack_inference PRIVATE omatrack_warnings)

if(OMATRACK_ENABLE_IMAGE_TELEMETRY)
  # Never silently substitute a system SDK or a cached SDK from another root.
  unset(ONNXRUNTIME_INCLUDE_DIR CACHE)
  unset(ONNXRUNTIME_LIBRARY CACHE)
  set(ONNXRUNTIME_INCLUDE_DIR "")
  set(ONNXRUNTIME_LIBRARY "")
  if(ONNXRUNTIME_ROOT)
    unset(ONNXRUNTIME_INCLUDE_DIR)
    unset(ONNXRUNTIME_LIBRARY)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
      PATHS "${ONNXRUNTIME_ROOT}/include" PATH_SUFFIXES onnxruntime NO_DEFAULT_PATH)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
      PATHS "${ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH)
  endif()
  if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    target_compile_definitions(omatrack_inference PRIVATE OMATRACK_HAVE_ONNXRUNTIME=1)
    target_include_directories(omatrack_inference PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(omatrack_inference PRIVATE "${ONNXRUNTIME_LIBRARY}")
    message(STATUS "Image telemetry ONNX Runtime: ${ONNXRUNTIME_LIBRARY}")
  else()
    message(STATUS "Image telemetry reader disabled: set ONNXRUNTIME_ROOT to its SDK")
  endif()

  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GAUGE_FFMPEG QUIET IMPORTED_TARGET
      libavformat libavcodec libavutil libswscale)
  endif()
  if(TARGET PkgConfig::GAUGE_FFMPEG)
    target_compile_definitions(omatrack_inference PRIVATE OMATRACK_HAVE_VIDEO_DECODER=1)
    target_link_libraries(omatrack_inference PRIVATE PkgConfig::GAUGE_FFMPEG)
    message(STATUS "Image telemetry FFmpeg decoder enabled")
  else()
    message(STATUS "Image telemetry decoder disabled: FFmpeg development libraries missing")
  endif()
endif()

if(OMATRACK_REQUIRE_IMAGE_TELEMETRY AND
   (NOT OMATRACK_ENABLE_IMAGE_TELEMETRY OR NOT ONNXRUNTIME_INCLUDE_DIR OR
    NOT ONNXRUNTIME_LIBRARY OR NOT TARGET PkgConfig::GAUGE_FFMPEG))
  message(FATAL_ERROR "This build requires ONNX Runtime and FFmpeg development libraries")
endif()

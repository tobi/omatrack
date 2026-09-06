# Standard .telemetry cache codec. It does not require ONNX Runtime.
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_shared)
  set(OMATRACK_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
  set(OMATRACK_ZSTD_TARGET zstd::libzstd_static)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(IMAGE_ZSTD REQUIRED IMPORTED_TARGET libzstd)
  set(OMATRACK_ZSTD_TARGET PkgConfig::IMAGE_ZSTD)
endif()
add_library(omatrack_image_cache STATIC
  ${PROJECT_SOURCE_DIR}/src/app/ImageTelemetryCache.cpp
  ${PROJECT_SOURCE_DIR}/src/app/ImageTelemetryCache.h
  ${PROJECT_SOURCE_DIR}/src/inference/ImageTelemetrySeries.h)
target_include_directories(omatrack_image_cache PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(omatrack_image_cache PUBLIC Qt6::Core omatrack_rust_bridge
  PRIVATE ${OMATRACK_ZSTD_TARGET} omatrack_warnings)

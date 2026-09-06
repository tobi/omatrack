// A loader-only regression fixture: emulate an older runtime lacking our API.
// No model data or real ONNX Runtime function is called through this table.
#include <onnxruntime_c_api.h>

namespace {
const OrtApi* ORT_API_CALL unavailable(uint32_t) noexcept { return nullptr; }
const char* ORT_API_CALL version() noexcept {
    return "unsupported-test-runtime";
}
const OrtApiBase api{unavailable, version};
}  // namespace

extern "C" const OrtApiBase* ORT_API_CALL OrtGetApiBase() noexcept {
    return &api;
}

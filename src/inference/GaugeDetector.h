#pragma once

#include "GaugeReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omatrack::inference {

// Frozen center-v1 tensor/decode ABI with the pilot-v2 training-scope contract.
// The caller MUST verify this scope and its trusted metadata/contract/model
// hashes before constructing this component. Tensor validation is compatibility
// checking, not authentication of an ONNX graph or reader support approval.
inline constexpr char GaugeDetectorContract[] = "gauge-detector-center-v1";
inline constexpr char GaugeDetectorTrainingScope[] =
    "pilot-v2-mil-strong4-backgrounds";
inline constexpr char GaugeDetectorContractSha256[] =
    "87a4cec9e0466250b635e561c74be81f2ce5f15fbd79804a1be639c3719a69d3";

struct GaugeDetectorOptions {
    // Full-source stretch, NOT letterbox/crop. Positive multiples of 32;
    // resource bound: each axis <=4096 and at most 4,194,304 input pixels.
    int inputWidth = 640;
    int inputHeight = 384;
    float scoreThreshold = .35f;  // finite [0,1]; uncalibrated proposal score
};

struct GaugeDetection {
    // Source-normalized [left, top, right, bottom], clipped to [0,1]. No PTS:
    // the caller retains the decoded frame's actual presentation timestamp.
    std::array<double, 4> bbox{};
    float score = 0;  // NOT a calibrated probability or reader approval
    std::string representation;  // digits, bar, wheel, needle
    std::string semantic =
        "unknown";            // unknown, gear, stint_lap, brake, throttle
    float semanticScore = 0;  // max softmax; also uncalibrated, even if unknown
    std::size_t cellIndex =
        0;  // deterministic tie-break identity, not tracking
    // Visibility, readability and fill direction remain UNKNOWN. Reader support
    // remains UNVALIDATED. No extraction or inferred fill/physical value here.
};

struct GaugeDetectionResult {
    GaugeError error = GaugeError::None;
    std::string detail;
    std::vector<GaugeDetection> detections;
    double latencyMs = 0;  // resize + inference + decode, excluding model load
};

// Borrowed contiguous float32 NCHW view for deterministic decoder fixtures.
// Actual ORT dtype/rank are checked before constructing these views.
struct GaugeDetectorTensor {
    std::string name;
    std::array<std::int64_t, 4> shape{};
    const float* data = nullptr;
    std::size_t size = 0;  // number of floats, must exactly match shape
};

// Pure CPU operations, also available without ORT. Throw std::invalid_argument
// for invalid frames/dimensions/tensors/thresholds (allocation can also throw).
// Resize returns CHW float32 RGB/255, half-pixel bilinear, no antialiasing or
// intermediate uint8 rounding. Fixture resize dimensions need not be /32.
std::vector<float> gaugeDetectorResizeRgb(const GaugeRgb24Frame& frame,
                                          int width = 640, int height = 384);
// Exactly five named tensors, batch1, channels 1/2/2/4/5, equal positive H/W
// divisible by8, finite float32. 3x3 local maxima, descending score/index ties,
// top100 BEFORE clipping/zero-area rejection, class-agnostic IoU>.4 NMS, max32.
// Representation ties choose first class; semantic max softmax <.8 abstains.
std::vector<GaugeDetection> gaugeDetectorDecode(
    const std::vector<GaugeDetectorTensor>& outputs, float threshold = .35f);

// Qt-free optional ONNX Runtime CPU detector. Construct, call and destroy on
// one serial worker. No concurrent calls, internal worker, GPU provider, file
// writes, network, reader invocation, temporal smoothing or layout approval.
// Without OMATRACK_HAVE_ONNXRUNTIME=1, detect() reports RuntimeUnavailable and
// NEVER returns dummy detections. A valid model with no proposals is success
// with an empty vector, distinctly different from an error.
class GaugeDetector {
public:
    explicit GaugeDetector(const std::string& modelPath,
                           GaugeDetectorOptions options = {});
    ~GaugeDetector();
    GaugeDetector(const GaugeDetector&) = delete;
    GaugeDetector& operator=(const GaugeDetector&) = delete;

    static bool runtimeAvailable();
    bool ready() const;
    const std::string& modelError() const;
    GaugeDetectionResult detect(const GaugeRgb24Frame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace omatrack::inference

#include "GaugeDetector.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

#ifndef OMATRACK_HAVE_ONNXRUNTIME
#define OMATRACK_HAVE_ONNXRUNTIME 0
#endif
#if OMATRACK_HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace omatrack::inference {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int MaxAxis = 4096;
constexpr std::size_t MaxPixels = 4'194'304;
constexpr std::array<const char*, 5> OutputNames{
    "center_logits", "size_logits", "offset_logits", "representation_logits",
    "semantic_logits"};
constexpr std::array<int, 5> OutputChannels{1, 2, 2, 4, 5};
constexpr std::array<const char*, 4> Representations{"digits", "bar", "wheel",
                                                     "needle"};
constexpr std::array<const char*, 5> Semantics{"unknown", "gear", "stint_lap",
                                               "brake", "throttle"};
constexpr std::size_t CandidateLimit = 100, DetectionLimit = 32;

bool validDimensions(int width, int height) {
    return width > 0 && height > 0 && width <= MaxAxis && height <= MaxAxis &&
           std::size_t(width) * std::size_t(height) <= MaxPixels;
}
bool validThreshold(float threshold) {
    return std::isfinite(threshold) && threshold >= 0 && threshold <= 1;
}
bool validFrame(const GaugeRgb24Frame& frame) {
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0 ||
        std::size_t(frame.width) > std::numeric_limits<std::size_t>::max() / 3)
        return false;
    const auto rowBytes = std::size_t(frame.width) * 3;
    const auto maxOffset =
        std::size_t(std::numeric_limits<std::ptrdiff_t>::max());
    if (frame.stride < rowBytes || frame.byteSize < rowBytes ||
        rowBytes > maxOffset)
        return false;
    const auto rows = std::size_t(frame.height - 1);
    // Last-row padding is not required. Prove both extent and pointer offset
    // bounds before any addition/multiplication or pixel access.
    return rows <= (frame.byteSize - rowBytes) / frame.stride &&
           rows <= (maxOffset - rowBytes) / frame.stride;
}

// NumPy runtime.py materializes each float32 operation separately. Explicit
// stores preserve those rounding points even when the compiler's default
// allows contraction into FMA (notably aarch64). No build-wide FP flags needed.
float f32(float value) {
    volatile float rounded = value;
    return rounded;
}
struct SampleAxis {
    int first, second;
    float weight;
};
std::vector<SampleAxis> sampleAxis(int source, int destination) {
    std::vector<SampleAxis> samples;
    samples.reserve(destination);
    const float scale = float(double(source) / destination);
    for (int i = 0; i < destination; ++i) {
        const float coordinate = f32(f32(f32(float(i) + .5f) * scale) - .5f);
        const auto floor = std::int64_t(std::floor(double(coordinate)));
        const auto last = std::int64_t(source) - 1;
        samples.push_back({int(std::clamp(floor, std::int64_t(0), last)),
                           int(std::clamp(floor + 1, std::int64_t(0), last)),
                           float(double(coordinate) - double(floor))});
    }
    return samples;
}
float interpolate(float first, float second, float weight) {
    return f32(f32(first * f32(1.f - weight)) + f32(second * weight));
}
float sigmoid(float value) {
    // The frozen NumPy decoder clips numerically extreme finite logits only.
    const float exponential = std::exp(-std::clamp(value, -80.f, 80.f));
    return 1.f / f32(1.f + exponential);
}
double iou(const std::array<double, 4>& a, const std::array<double, 4>& b) {
    // Python box arithmetic is binary64, without fused multiply-add. Retain
    // those rounding points at the strict >.4 boundary too.
    const volatile double intersection =
        std::max(0., std::min(a[2], b[2]) - std::max(a[0], b[0])) *
        std::max(0., std::min(a[3], b[3]) - std::max(a[1], b[1]));
    const volatile double areaA = (a[2] - a[0]) * (a[3] - a[1]);
    const volatile double areaB = (b[2] - b[0]) * (b[3] - b[1]);
    return intersection / std::max(1e-12, areaA + areaB - intersection);
}
}  // namespace

std::vector<float> gaugeDetectorResizeRgb(const GaugeRgb24Frame& frame,
                                          int width, int height) {
    if (!validFrame(frame))
        throw std::invalid_argument(
            "Invalid or truncated detector RGB24 frame/stride");
    if (!validDimensions(width, height))
        throw std::invalid_argument(
            "Invalid or excessive detector resize dimensions");
    const auto xs = sampleAxis(frame.width, width);
    const auto ys = sampleAxis(frame.height, height);
    const auto pixels = std::size_t(width) * height;
    std::vector<float> result(3 * pixels);
    for (int y = 0; y < height; ++y) {
        const auto& sy = ys[y];
        const auto* row0 = frame.pixels + std::size_t(sy.first) * frame.stride;
        const auto* row1 = frame.pixels + std::size_t(sy.second) * frame.stride;
        for (int x = 0; x < width; ++x) {
            const auto& sx = xs[x];
            for (std::size_t c = 0; c < 3; ++c) {
                const float a = interpolate(
                    row0[std::size_t(sx.first) * 3 + c],
                    row0[std::size_t(sx.second) * 3 + c], sx.weight);
                const float b = interpolate(
                    row1[std::size_t(sx.first) * 3 + c],
                    row1[std::size_t(sx.second) * 3 + c], sx.weight);
                result[c * pixels + std::size_t(y) * width + x] =
                    interpolate(a, b, sy.weight) / 255.f;
            }
        }
    }
    return result;
}

std::vector<GaugeDetection> gaugeDetectorDecode(
    const std::vector<GaugeDetectorTensor>& outputs, float threshold) {
    if (!validThreshold(threshold))
        throw std::invalid_argument("Invalid detector score threshold");
    if (outputs.size() != OutputNames.size())
        throw std::invalid_argument("Unexpected detector output count");
    std::array<const GaugeDetectorTensor*, 5> heads{};
    for (const auto& tensor : outputs) {
        const auto found =
            std::find(OutputNames.begin(), OutputNames.end(), tensor.name);
        if (found == OutputNames.end())
            throw std::invalid_argument("Unexpected detector output name");
        const auto index = std::size_t(found - OutputNames.begin());
        if (heads[index])
            throw std::invalid_argument("Duplicate detector output name");
        heads[index] = &tensor;
    }
    const auto gh = heads[0]->shape[2], gw = heads[0]->shape[3];
    if (gh <= 0 || gw <= 0 || gh % 8 || gw % 8 || gh > MaxAxis / 4 ||
        gw > MaxAxis / 4 || std::size_t(gh) * std::size_t(gw) > MaxPixels / 16)
        throw std::invalid_argument("Invalid detector output spatial contract");
    const auto pixels = std::size_t(gh) * std::size_t(gw);
    for (std::size_t i = 0; i < heads.size(); ++i) {
        const auto& head = *heads[i];
        if (head.shape !=
                std::array<std::int64_t, 4>{1, OutputChannels[i], gh, gw} ||
            !head.data || head.size != std::size_t(OutputChannels[i]) * pixels)
            throw std::invalid_argument(
                "Detector output shape/buffer mismatch");
        if (!std::all_of(head.data, head.data + head.size,
                         [](float value) { return std::isfinite(value); }))
            throw std::invalid_argument("Nonfinite detector output");
    }
    std::vector<float> scores(pixels);
    std::transform(heads[0]->data, heads[0]->data + pixels, scores.begin(),
                   sigmoid);
    std::vector<std::size_t> candidates;
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            const auto cell = std::size_t(y) * gw + x;
            if (scores[cell] < threshold) continue;
            bool maximum = true;
            for (int ny = std::max(0, y - 1);
                 ny <= std::min(int(gh) - 1, y + 1); ++ny)
                for (int nx = std::max(0, x - 1);
                     nx <= std::min(int(gw) - 1, x + 1); ++nx)
                    if (scores[std::size_t(ny) * gw + nx] > scores[cell])
                        maximum = false;
            if (maximum) candidates.push_back(cell);
        }
    }
    const auto count = std::min(CandidateLimit, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + count,
                      candidates.end(), [&](std::size_t a, std::size_t b) {
                          return scores[a] == scores[b] ? a < b
                                                        : scores[a] > scores[b];
                      });
    candidates.resize(count);
    std::vector<GaugeDetection> result;
    result.reserve(DetectionLimit);
    for (const auto cell : candidates) {
        const double cx =
            (double(cell % gw) + sigmoid(heads[2]->data[cell])) / double(gw);
        const double cy =
            (double(cell / gw) + sigmoid(heads[2]->data[pixels + cell])) /
            double(gh);
        const double w = sigmoid(heads[1]->data[cell]);
        const double h = sigmoid(heads[1]->data[pixels + cell]);
        const std::array<double, 4> box{
            std::max(0., cx - w / 2), std::max(0., cy - h / 2),
            std::min(1., cx + w / 2), std::min(1., cy + h / 2)};
        if (box[0] >= box[2] || box[1] >= box[3] ||
            std::any_of(result.begin(), result.end(),
                        [&](const GaugeDetection& other) {
                            return iou(box, other.bbox) > .4;
                        }))
            continue;
        int representation = 0, semantic = 0;
        for (int i = 1; i < int(Representations.size()); ++i)
            if (heads[3]->data[std::size_t(i) * pixels + cell] >
                heads[3]->data[std::size_t(representation) * pixels + cell])
                representation = i;
        // Lowest-index ties exactly as NumPy argmax. Shift before exp so any
        // finite logits, including FLT_MAX/-FLT_MAX, produce finite softmax.
        for (int i = 1; i < int(Semantics.size()); ++i)
            if (heads[4]->data[std::size_t(i) * pixels + cell] >
                heads[4]->data[std::size_t(semantic) * pixels + cell])
                semantic = i;
        const float maximum =
            heads[4]->data[std::size_t(semantic) * pixels + cell];
        float sum = 0;
        for (std::size_t i = 0; i < Semantics.size(); ++i)
            sum =
                f32(sum +
                    std::exp(f32(heads[4]->data[i * pixels + cell] - maximum)));
        const float semanticScore = 1.f / sum;
        result.push_back(
            {box, scores[cell], Representations[representation],
             double(semanticScore) >= .8 ? Semantics[semantic] : Semantics[0],
             semanticScore, cell});
        if (result.size() == DetectionLimit) break;
    }
    return result;
}

struct GaugeDetector::Impl {
    GaugeDetectorOptions options;
    std::string error;
#if OMATRACK_HAVE_ONNXRUNTIME
    std::unique_ptr<Ort::Env> environment;
    std::unique_ptr<Ort::Session> session;
#endif
};

GaugeDetector::GaugeDetector(const std::string& modelPath,
                             GaugeDetectorOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
    if (!validDimensions(options.inputWidth, options.inputHeight) ||
        options.inputWidth % 32 || options.inputHeight % 32 ||
        !validThreshold(options.scoreThreshold)) {
        impl_->error = "Invalid gauge detector dimensions or score threshold";
        return;
    }
#if OMATRACK_HAVE_ONNXRUNTIME
    try {
        if (!runtimeAvailable())
            throw std::runtime_error(
                "Loaded ONNX Runtime is incompatible with this build");
        if (modelPath.empty())
            throw std::runtime_error("No gauge detector model configured");
        impl_->environment = std::make_unique<Ort::Env>(
            ORT_LOGGING_LEVEL_WARNING, "omatrack-gauge-detector");
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        sessionOptions.SetInterOpNumThreads(1);
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        sessionOptions.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        // No EP is appended: ONNX Runtime's default is CPU only.
        const auto path = std::filesystem::u8path(modelPath);
        auto session = std::make_unique<Ort::Session>(
            *impl_->environment, path.c_str(), sessionOptions);
        if (session->GetInputCount() != 1 ||
            session->GetOutputCount() != OutputNames.size() ||
            session->GetOverridableInitializerCount() != 0)
            throw std::runtime_error(
                "Detector requires one image input and exactly five outputs");
        Ort::AllocatorWithDefaultOptions allocator;
        auto check = [&](const Ort::TypeInfo& type, int channels, int height,
                         int width, bool output) {
            if (type.GetONNXType() != ONNX_TYPE_TENSOR)
                throw std::runtime_error(
                    "Detector declaration is not a tensor");
            const auto tensor = type.GetTensorTypeAndShapeInfo();
            const auto shape = tensor.GetShape();
            // The frozen Torch exporter leaves output batch symbolic despite
            // the fixed batch1 input. Allow that declaration, NOT arbitrary
            // runtime batches; all actual shapes are checked again below.
            // Channel counts and the input batch must always be fixed.
            if (tensor.GetElementType() !=
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                shape.size() != 4 ||
                (shape[0] != 1 && !(output && shape[0] == -1)) ||
                shape[1] != channels ||
                (shape[2] != -1 && shape[2] != height) ||
                (shape[3] != -1 && shape[3] != width))
                throw std::runtime_error(
                    "Detector tensor declaration shape/type mismatch");
        };
        const auto inputName = session->GetInputNameAllocated(0, allocator);
        if (!inputName || std::string(inputName.get()) != "image")
            throw std::runtime_error("Detector input must be named image");
        check(session->GetInputTypeInfo(0), 3, options.inputHeight,
              options.inputWidth, false);
        std::array<bool, 5> found{};
        for (std::size_t i = 0; i < OutputNames.size(); ++i) {
            const auto name = session->GetOutputNameAllocated(i, allocator);
            const auto known =
                name ? std::find(OutputNames.begin(), OutputNames.end(),
                                 std::string(name.get()))
                     : OutputNames.end();
            if (known == OutputNames.end() ||
                found[known - OutputNames.begin()])
                throw std::runtime_error(
                    "Unexpected or duplicate detector output name");
            const auto index = std::size_t(known - OutputNames.begin());
            found[index] = true;
            check(session->GetOutputTypeInfo(i), OutputChannels[index],
                  options.inputHeight / 4, options.inputWidth / 4, true);
        }
        impl_->session = std::move(session);
    } catch (const std::exception& error) {
        impl_->error = error.what();
    }
#else
    (void)modelPath;
    impl_->error =
        "Gauge detection unavailable: built without ONNX Runtime CPU support";
#endif
}
GaugeDetector::~GaugeDetector() = default;
bool GaugeDetector::runtimeAvailable() {
#if OMATRACK_HAVE_ONNXRUNTIME
    const auto* base = OrtGetApiBase();
    return base && base->GetApi(ORT_API_VERSION);
#else
    return false;
#endif
}
bool GaugeDetector::ready() const {
#if OMATRACK_HAVE_ONNXRUNTIME
    return bool(impl_->session);
#else
    return false;
#endif
}
const std::string& GaugeDetector::modelError() const { return impl_->error; }

GaugeDetectionResult GaugeDetector::detect(const GaugeRgb24Frame& frame) {
    const auto started = Clock::now();
    GaugeDetectionResult result;
    if (!validFrame(frame)) {
        result.error = GaugeError::InvalidFrame;
        result.detail = "Invalid or truncated detector RGB24 frame/stride";
    } else if (!ready()) {
        result.error = runtimeAvailable() ? GaugeError::ModelLoadFailed
                                          : GaugeError::RuntimeUnavailable;
        result.detail = impl_->error;
    } else {
#if OMATRACK_HAVE_ONNXRUNTIME
        try {
            const auto& options = impl_->options;
            auto data = gaugeDetectorResizeRgb(frame, options.inputWidth,
                                               options.inputHeight);
            const std::array<std::int64_t, 4> shape{1, 3, options.inputHeight,
                                                    options.inputWidth};
            const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                           OrtMemTypeDefault);
            auto input = Ort::Value::CreateTensor<float>(
                memory, data.data(), data.size(), shape.data(), shape.size());
            const char* inputs[]{"image"};
            auto outputs =
                impl_->session->Run(Ort::RunOptions{nullptr}, inputs, &input, 1,
                                    OutputNames.data(), OutputNames.size());
            if (outputs.size() != OutputNames.size())
                throw std::runtime_error(
                    "Unexpected detector runtime output count");
            std::vector<GaugeDetectorTensor> views;
            for (std::size_t i = 0; i < outputs.size(); ++i) {
                if (!outputs[i].IsTensor())
                    throw std::runtime_error(
                        "Detector runtime output is not a tensor");
                const auto info = outputs[i].GetTensorTypeAndShapeInfo();
                const std::array<std::int64_t, 4> expected{
                    1, OutputChannels[i], options.inputHeight / 4,
                    options.inputWidth / 4};
                const auto actual = info.GetShape();
                if (info.GetElementType() !=
                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                    actual.size() != 4 ||
                    !std::equal(actual.begin(), actual.end(), expected.begin()))
                    throw std::runtime_error(
                        "Detector runtime output shape/type mismatch");
                views.push_back({OutputNames[i], expected,
                                 outputs[i].GetTensorData<float>(),
                                 info.GetElementCount()});
            }
            result.detections =
                gaugeDetectorDecode(views, options.scoreThreshold);
        } catch (const std::exception& error) {
            result.error = GaugeError::InferenceFailed;
            result.detail = error.what();
            result.detections.clear();
        }
#endif
    }
    result.latencyMs =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    return result;
}
}  // namespace omatrack::inference

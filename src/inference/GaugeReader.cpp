#include "GaugeReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef OMATRACK_HAVE_ONNXRUNTIME
#define OMATRACK_HAVE_ONNXRUNTIME 0
#endif
#if OMATRACK_HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace omatrack::inference {
namespace {
using Clock = std::chrono::steady_clock;
constexpr char Contract[] = "omatrack-crop-count-v1";
constexpr char Preprocessing[] = "pillow-rgb-bilinear-22bit-crop-count-v1";
constexpr int CropWidth = 192, CropHeight = 64;
constexpr std::size_t CropPixels = CropWidth * CropHeight;
constexpr double NegativeInfinity = -std::numeric_limits<double>::infinity();

void unknown(GaugeResult& result, const std::string& reason) {
    for (auto* observation : {&result.gear, &result.stintLap,
                              &result.brakeFillPct, &result.throttleFillPct}) {
        observation->value.reset();
        observation->unknownReason = reason;
    }
}

bool validFrame(const GaugeRgb24Frame& f) {
    if (!f.pixels || f.width <= 0 || f.height <= 0 ||
        std::size_t(f.width) > std::numeric_limits<std::size_t>::max() / 3)
        return false;
    const auto rowBytes = std::size_t(f.width) * 3;
    if (f.stride < rowBytes || f.byteSize < rowBytes ||
        rowBytes > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()))
        return false;
    // Avoid overflowing either the extent or pointer-difference arithmetic.
    const auto rows = std::size_t(f.height - 1);
    return rows <= (f.byteSize - rowBytes) / f.stride &&
           rows <= (std::size_t(std::numeric_limits<std::ptrdiff_t>::max()) -
                    rowBytes) /
                       f.stride;
}

struct Box {
    int left, top, right, bottom;
};
const std::uint8_t* pixel(const GaugeRgb24Frame& f, int x, int y) {
    return f.pixels + std::size_t(y) * f.stride + std::size_t(x) * 3;
}

bool red(const std::uint8_t* p) {
    return p[0] >= 18 && p[0] > 1.6 * p[1] + 6 && p[0] > 1.4 * p[2] + 6;
}
bool green(const std::uint8_t* p) {
    return p[1] >= 20 && p[1] > 1.5 * p[0] + 8 && p[1] > 1.3 * p[2] + 8;
}
bool orange(const std::uint8_t* p) {
    return p[0] > 45 && p[1] > 18 && p[0] > 1.2 * p[1] && p[2] < .5 * p[1];
}
bool white(const std::uint8_t* p) {
    const int lo = std::min({p[0], p[1], p[2]}),
              hi = std::max({p[0], p[1], p[2]});
    return lo > 160 && hi - lo < 55;
}

double fraction(const GaugeRgb24Frame& f, Box box,
                bool (*predicate)(const std::uint8_t*)) {
    int yes = 0, total = 0;
    for (int y = box.top; y < box.bottom; y += 2)
        for (int x = box.left; x < box.right; x += 2) {
            yes += predicate(pixel(f, x, y));
            ++total;
        }
    return double(yes) / total;
}

double columnEdges(const GaugeRgb24Frame& f, int center, int left, int right,
                   bool (*predicate)(const std::uint8_t*)) {
    int matched = 0, total = 0;
    for (int y = 646; y < 880; y += 4) {
        matched += predicate(pixel(f, center, y)) &&
                   !predicate(pixel(f, left, y)) &&
                   !predicate(pixel(f, right, y));
        ++total;
    }
    return double(matched) / total;
}

// Fixed reviewed orange HUD structure, not text recognition or a learned
// detector. Require BOTH narrow coloured vertical columns, BOTH separated
// horizontal orange/brown scale tracks, and sparse bright RPM tick/text marks.
// No logos, driver names, filenames or native telemetry enter these tests.
bool knownLayout(const GaugeRgb24Frame& f) {
    const double ticks = fraction(f, {1440, 956, 1880, 981}, white);
    return fraction(f, {968, 642, 984, 880}, red) >= .94 &&
           fraction(f, {1024, 642, 1040, 880}, green) >= .94 &&
           columnEdges(f, 976, 960, 992, red) >= .90 &&
           columnEdges(f, 1032, 1016, 1048, green) >= .90 &&
           fraction(f, {1415, 855, 1875, 877}, orange) >= .82 &&
           fraction(f, {1410, 988, 1875, 1006}, orange) >= .82 &&
           ticks >= .012 && ticks <= .15;
}

#if OMATRACK_HAVE_ONNXRUNTIME || defined(OMATRACK_GAUGE_READER_TESTING)
// Crop bounds are floor(left/top), ceil(right/bottom), exactly data.py's
// reviewed normalized windows at 1920x1080. Nothing scales a display capture.
constexpr std::array<Box, 4> CropBoxes{{{1399, 1010, 1475, 1079},
                                        {408, 994, 479, 1044},
                                        {956, 628, 999, 894},
                                        {1011, 628, 1055, 894}}};

struct RgbImage {
    int width, height;
    std::vector<std::uint8_t> bytes;
    RgbImage(int w, int h)
        : width(w), height(h), bytes(std::size_t(w) * h * 3, 0) {}
};

struct Coefficient {
    int first;
    std::vector<int> weights;
};
std::vector<Coefficient> coefficients(int input, int output) {
    // Pillow RGB BILINEAR is antialiased on reduction, separable, and rounds
    // to uint8 after EACH pass. A generic half-pixel bilinear sampler is wrong.
    // Its normalized positive triangular weights use 22-bit fixed precision.
    constexpr double precision = 1 << 22;
    const double scale = double(input) / output, support = std::max(1., scale);
    std::vector<Coefficient> result;
    result.reserve(output);
    for (int i = 0; i < output; ++i) {
        const double center = (i + .5) * scale;
        const int first = std::max(0, int(center - support + .5));
        const int end = std::min(input, int(center + support + .5));
        std::vector<double> weights;
        double sum = 0;
        for (int j = first; j < end; ++j) {
            const double w =
                std::max(0., 1. - std::abs((j - center + .5) * (1. / support)));
            weights.push_back(w);
            sum += w;
        }
        Coefficient c{first, {}};
        for (double w : weights)
            c.weights.push_back(int(w / sum * precision + .5));
        result.push_back(std::move(c));
    }
    return result;
}

RgbImage resize(const RgbImage& source, int width, int height) {
    const auto horizontal = coefficients(source.width, width),
               vertical = coefficients(source.height, height);
    RgbImage intermediate(width, source.height), output(width, height);
    for (int y = 0; y < source.height; ++y)
        for (int x = 0; x < width; ++x)
            for (int channel = 0; channel < 3; ++channel) {
                const auto& c = horizontal[x];
                int sum = 1 << 21;
                for (std::size_t k = 0; k < c.weights.size(); ++k)
                    sum += c.weights[k] *
                           source.bytes[(std::size_t(y) * source.width +
                                         c.first + k) *
                                            3 +
                                        channel];
                intermediate.bytes[(std::size_t(y) * width + x) * 3 + channel] =
                    std::uint8_t(std::clamp(sum >> 22, 0, 255));
            }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int channel = 0; channel < 3; ++channel) {
                const auto& c = vertical[y];
                int sum = 1 << 21;
                for (std::size_t k = 0; k < c.weights.size(); ++k)
                    sum +=
                        c.weights[k] *
                        intermediate
                            .bytes[((c.first + k) * width + x) * 3 + channel];
                output.bytes[(std::size_t(y) * width + x) * 3 + channel] =
                    std::uint8_t(std::clamp(sum >> 22, 0, 255));
            }
    return output;
}

std::vector<std::uint8_t> preprocess(const GaugeRgb24Frame& f) {
    std::vector<std::uint8_t> output(4 * CropPixels * 3,
                                     0);  // NHWC bytes, black digit padding
    for (std::size_t field = 0; field < CropBoxes.size(); ++field) {
        const auto b = CropBoxes[field];
        const int w = b.right - b.left, h = b.bottom - b.top;
        const bool bar = field >= 2;
        RgbImage source(bar ? h : w, bar ? w : h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                // Pillow ROTATE_270 = clockwise: bottom-to-top becomes
                // left-to-right.
                const int dx = bar ? h - 1 - y : x, dy = bar ? x : y;
                std::copy_n(pixel(f, b.left + x, b.top + y), 3,
                            source.bytes.data() +
                                (std::size_t(dy) * source.width + dx) * 3);
            }
        int width = CropWidth, height = CropHeight;
        if (!bar) {
            // These two reviewed geometries have no half-integer rounding ties.
            const double ratio =
                std::min(double(CropWidth) / w, double(CropHeight) / h);
            width = std::max(1, int(std::round(w * ratio)));
            height = std::max(1, int(std::round(h * ratio)));
        }
        auto scaled = resize(source, width, height);
        const int left = (CropWidth - width) / 2,
                  top = (CropHeight - height) / 2;
        for (int y = 0; y < height; ++y)
            std::copy_n(
                scaled.bytes.data() + std::size_t(y) * width * 3, width * 3,
                output.data() +
                    (field * CropPixels + (y + top) * CropWidth + left) * 3);
    }
    return output;
}

double logAdd(double a, double b) {
    if (a == NegativeInfinity) return b;
    if (b == NegativeInfinity) return a;
    return std::max(a, b) + std::log1p(std::exp(-std::abs(a - b)));
}

struct Beam {
    std::string prefix;
    double blank = NegativeInfinity, nonblank = NegativeInfinity;
    double score() const { return logAdd(blank, nonblank); }
};

std::string decode(const float* logits, const float* counts) {
    // Exact count_model.py policy: argmax(count)+1, CTC prefix beam width TEN
    // separately PER LENGTH, not greedy and not a gear/lap vocabulary prior.
    const int length = int(std::max_element(counts, counts + 3) - counts) + 1;
    std::vector<Beam> beams{{"", 0., NegativeInfinity}};
    for (int time = 0; time < 24; ++time) {
        std::array<double, 11> step{};
        double maximum = NegativeInfinity, sum = 0;
        for (int token = 0; token < 11; ++token)
            maximum = std::max(maximum, double(logits[token * 24 + time]));
        for (int token = 0; token < 11; ++token)
            sum += std::exp(double(logits[token * 24 + time]) - maximum);
        for (int token = 0; token < 11; ++token)
            step[token] =
                double(logits[token * 24 + time]) - maximum - std::log(sum);
        std::vector<Beam> next;
        std::unordered_map<std::string, std::size_t> indices;
        auto update = [&](const std::string& prefix, double blank,
                          double nonblank) {
            auto [it, inserted] = indices.emplace(prefix, next.size());
            if (inserted)
                next.push_back({prefix, NegativeInfinity, NegativeInfinity});
            auto& beam = next[it->second];
            beam.blank = logAdd(beam.blank, blank);
            beam.nonblank = logAdd(beam.nonblank, nonblank);
        };
        for (const auto& beam : beams) {
            const double total = beam.score();
            update(beam.prefix, total + step[0], NegativeInfinity);
            for (int token = 1; token < 11; ++token) {
                const char digit = char('0' + token - 1);
                if (!beam.prefix.empty() && beam.prefix.back() == digit) {
                    update(beam.prefix, NegativeInfinity,
                           beam.nonblank + step[token]);
                    if (int(beam.prefix.size()) < length)
                        update(beam.prefix + digit, NegativeInfinity,
                               beam.blank + step[token]);
                } else if (int(beam.prefix.size()) < length) {
                    update(beam.prefix + digit, NegativeInfinity,
                           total + step[token]);
                }
            }
        }
        beams.clear();
        for (int n = 0; n <= length; ++n) {
            std::vector<Beam> group;
            for (const auto& beam : next)
                if (int(beam.prefix.size()) == n) group.push_back(beam);
            // Python dict insertion order + stable sorted tie behavior is kept.
            std::stable_sort(group.begin(), group.end(),
                             [](const Beam& a, const Beam& b) {
                                 return a.score() > b.score();
                             });
            if (group.size() > 10) group.resize(10);
            beams.insert(beams.end(), group.begin(), group.end());
        }
    }
    const Beam* best = nullptr;
    for (const auto& beam : beams)
        if (int(beam.prefix.size()) == length &&
            (!best || beam.score() > best->score()))
            best = &beam;
    return best ? best->prefix : std::string{};
}
#endif
}  // namespace

// Test seams are absent from production builds and deliberately not public API.
#ifdef OMATRACK_GAUGE_READER_TESTING
std::vector<std::uint8_t> gaugeReaderTestPreprocess(const GaugeRgb24Frame& f) {
    return preprocess(f);
}
std::string gaugeReaderTestDecode(const float* logits, const float* counts) {
    return decode(logits, counts);
}
#endif

struct GaugeReader::Impl {
    std::string error;
    std::map<std::string, std::string> metadata;
#if OMATRACK_HAVE_ONNXRUNTIME
    std::unique_ptr<Ort::Env> environment;
    std::unique_ptr<Ort::Session> session;
    std::vector<float> input = std::vector<float>(4 * 3 * CropPixels);
#endif
};

GaugeReader::GaugeReader(const std::string& modelPath)
    : impl_(std::make_unique<Impl>()) {
#if OMATRACK_HAVE_ONNXRUNTIME
    try {
        if (modelPath.empty())
            throw std::runtime_error("No gauge reader model configured");
        if (!runtimeAvailable())
            throw std::runtime_error(
                "Loaded ONNX Runtime is incompatible with this build");
        impl_->environment = std::make_unique<Ort::Env>(
            ORT_LOGGING_LEVEL_WARNING, "omatrack-gauge-reader");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        const auto path = std::filesystem::u8path(modelPath);
        auto session = std::make_unique<Ort::Session>(*impl_->environment,
                                                      path.c_str(), options);
        Ort::AllocatorWithDefaultOptions allocator;
        const auto metadata = session->GetModelMetadata();
        // Compatibility is a semantic/tensor contract, not one forever-pinned
        // training run. Managed downloads authenticate the complete artifact
        // via their immutable-commit manifest SHA-256; local models must still
        // carry valid provenance and match this exact reader implementation.
        std::map<std::string, std::string> acceptedMetadata;
        const std::array<std::pair<const char*, const char*>, 5> required{
            {{"omatrack.contract", Contract},
             {"omatrack.preprocessing", Preprocessing},
             {"omatrack.layout", "tds_aim_orange-1920x1080"},
             {"omatrack.decoder",
              "count-argmax-plus-one-ctc-prefix-beam-10-per-length"},
             {"omatrack.fields",
              "gear,stint_lap,brake_fill_pct,throttle_fill_pct"}}};
        for (const auto& item : required) {
            const auto value = metadata.LookupCustomMetadataMapAllocated(
                item.first, allocator);
            if (!value || std::string(value.get()) != item.second)
                throw std::runtime_error(
                    "Wrong gauge model provenance/contract: " +
                    std::string(item.first));
            acceptedMetadata.emplace(item.first, value.get());
        }
        const auto checkpoint = metadata.LookupCustomMetadataMapAllocated(
            "omatrack.checkpoint_sha256", allocator);
        const std::string digest = checkpoint ? checkpoint.get() : "";
        if (digest.size() != 64 ||
            !std::all_of(digest.begin(), digest.end(),
                         [](char c) {
                             return (c >= '0' && c <= '9') ||
                                    (c >= 'a' && c <= 'f');
                         }) ||
            digest == std::string(64, '0'))
            throw std::runtime_error(
                "Missing or invalid gauge checkpoint provenance SHA-256");
        acceptedMetadata.emplace("omatrack.checkpoint_sha256", digest);
        if (session->GetInputCount() != 1 || session->GetOutputCount() != 3)
            throw std::runtime_error(
                "Gauge model must have one crop input and three count-reader "
                "outputs");
        auto checkTensor = [&](bool input, std::size_t i, const char* name,
                               const std::vector<int64_t>& shape) {
            const auto type = input ? session->GetInputTypeInfo(i)
                                    : session->GetOutputTypeInfo(i);
            const auto tensor = type.GetTensorTypeAndShapeInfo();
            const auto actualName =
                input ? session->GetInputNameAllocated(i, allocator)
                      : session->GetOutputNameAllocated(i, allocator);
            if (tensor.GetElementType() !=
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                tensor.GetShape() != shape ||
                std::string(actualName.get()) != name)
                throw std::runtime_error("Unexpected gauge model tensor: " +
                                         std::string(name));
        };
        checkTensor(true, 0, "crops", {4, 3, 64, 192});
        checkTensor(false, 0, "digits", {4, 11, 24});
        checkTensor(false, 1, "fills", {4});
        checkTensor(false, 2, "counts", {4, 3});
        impl_->metadata = std::move(acceptedMetadata);
        impl_->session = std::move(session);
    } catch (const std::exception& e) {
        impl_->error = e.what();
    }
#else
    (void)modelPath;
    impl_->error =
        "Gauge inference unavailable: built without ONNX Runtime CPU support";
#endif
}
GaugeReader::~GaugeReader() = default;
bool GaugeReader::runtimeAvailable() {
#if OMATRACK_HAVE_ONNXRUNTIME
    // Windows can find an older system onnxruntime.dll when an installation
    // is incomplete. Do not dereference the C++ wrapper's null API table.
    const auto* base = OrtGetApiBase();
    return base && base->GetApi(ORT_API_VERSION);
#else
    return false;
#endif
}
bool GaugeReader::ready() const {
#if OMATRACK_HAVE_ONNXRUNTIME
    return bool(impl_->session);
#else
    return false;
#endif
}
const std::string& GaugeReader::modelError() const { return impl_->error; }
const std::map<std::string, std::string>& GaugeReader::modelMetadata() const {
    return impl_->metadata;
}

GaugeResult GaugeReader::inspectLayout(const GaugeRgb24Frame& frame) {
    const auto started = Clock::now();
    GaugeResult result;
    if (!validFrame(frame)) {
        result.error = GaugeError::InvalidFrame;
        result.detail = "Invalid or truncated RGB24 frame/stride";
    } else if (frame.width != 1920 || frame.height != 1080) {
        result.admission = GaugeAdmission::UnsupportedGeometry;
        result.detail = "Only reviewed 1920x1080 source geometry is supported";
    } else if (!knownLayout(frame)) {
        result.admission = GaugeAdmission::Rejected;
        result.detail =
            "Reviewed orange HUD structure not present (or obscured/changed)";
    } else {
        result.admission = GaugeAdmission::Supported;
        result.detail =
            "Reviewed orange HUD structure admitted (fixed-layout heuristic)";
    }
    unknown(result, result.admission == GaugeAdmission::Supported
                        ? "Inference not run"
                        : result.detail);
    result.latencyMs =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    return result;
}

GaugeResult GaugeReader::read(const GaugeRgb24Frame& frame) {
    const auto started = Clock::now();
    auto result = inspectLayout(frame);
    if (result.error == GaugeError::None && !ready()) {
        result.error = runtimeAvailable() ? GaugeError::ModelLoadFailed
                                          : GaugeError::RuntimeUnavailable;
        result.detail = impl_->error;
        unknown(result, result.detail);
    } else if (result.admission == GaugeAdmission::Supported && ready()) {
#if OMATRACK_HAVE_ONNXRUNTIME
        try {
            const auto bytes = preprocess(frame);
            for (std::size_t field = 0; field < 4; ++field)
                for (std::size_t p = 0; p < CropPixels; ++p)
                    for (std::size_t c = 0; c < 3; ++c)
                        impl_->input[(field * 3 + c) * CropPixels + p] =
                            float(bytes[(field * CropPixels + p) * 3 + c]) /
                            255.f;
            const std::array<int64_t, 4> shape{4, 3, CropHeight, CropWidth};
            const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                           OrtMemTypeDefault);
            auto input = Ort::Value::CreateTensor<float>(
                memory, impl_->input.data(), impl_->input.size(), shape.data(),
                shape.size());
            const char* inputs[]{"crops"};
            const char* outputs[]{"digits", "fills", "counts"};
            auto values = impl_->session->Run(Ort::RunOptions{nullptr}, inputs,
                                              &input, 1, outputs, 3);
            // Check actual buffers too: ONNX can return a shape different from
            // the graph's annotations. Never read offsets until these match.
            const std::array<std::vector<int64_t>, 3> expectedShapes{
                {{4, 11, 24}, {4}, {4, 3}}};
            if (values.size() != expectedShapes.size())
                throw std::runtime_error("Unexpected gauge output count");
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto& value = values[i];
                if (!value.IsTensor())
                    throw std::runtime_error("Gauge output is not a tensor");
                const auto info = value.GetTensorTypeAndShapeInfo();
                if (info.GetElementType() !=
                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                    info.GetShape() != expectedShapes[i])
                    throw std::runtime_error(
                        "Gauge runtime output shape/type mismatch");
                const auto* data = value.GetTensorData<float>();
                const auto n = info.GetElementCount();
                // Fail closed on NaN/Inf, never coerce nonfinite values to
                // zero.
                if (!std::all_of(data, data + n,
                                 [](float v) { return std::isfinite(v); }))
                    throw std::runtime_error("Nonfinite gauge model output");
            }
            const auto* digits = values[0].GetTensorData<float>();
            const auto* fills = values[1].GetTensorData<float>();
            const auto* counts = values[2].GetTensorData<float>();
            for (std::size_t field = 0; field < 2; ++field) {
                auto& observation = field == 0 ? result.gear : result.stintLap;
                const double bright = fraction(frame, CropBoxes[field], white);
                const auto text =
                    decode(digits + field * 11 * 24, counts + field * 3);
                if (bright < .015 || bright > .6 || text.empty()) {
                    observation.unknownReason =
                        "Digit crop lacks visible glyph evidence";
                } else {
                    observation.value =
                        std::stoi(text);  // 1..3 digits, leading zero policy
                                          // matches research
                    observation.unknownReason.clear();
                }
            }
            for (std::size_t field = 2; field < 4; ++field) {
                auto& observation =
                    field == 2 ? result.brakeFillPct : result.throttleFillPct;
                if (fills[field] < 0 || fills[field] > 1) {
                    observation.unknownReason = "Fill outside model domain";
                } else {
                    observation.value = double(fills[field]) * 100.;
                    observation.unknownReason.clear();
                }
            }
        } catch (const std::exception& e) {
            result.error = GaugeError::InferenceFailed;
            result.detail = e.what();
            unknown(result, result.detail);
        }
#endif
    }
    result.latencyMs =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    return result;
}
}  // namespace omatrack::inference

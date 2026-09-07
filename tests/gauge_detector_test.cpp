#include "GaugeDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace omatrack::inference;
namespace fs = std::filesystem;
namespace {
constexpr std::array<const char*, 5> Names{
    "center_logits", "size_logits", "offset_logits", "representation_logits",
    "semantic_logits"};
constexpr std::array<int, 5> Channels{1, 2, 2, 4, 5};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance,
          std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(std::string(message) +
                                 ": actual=" + std::to_string(actual) +
                                 " expected=" + std::to_string(expected));
}
template <class F>
void invalid(F&& function, const std::string& message) {
    bool threw = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, message);
}
struct Image {
    int width, height;
    std::size_t stride;
    std::vector<std::uint8_t> bytes;
    Image(int w, int h, std::size_t padding = 0)
        : width(w),
          height(h),
          stride(std::size_t(w) * 3 + padding),
          bytes(stride * (h - 1) + std::size_t(w) * 3) {}
    GaugeRgb24Frame frame() const {
        return {bytes.data(), bytes.size(), width, height, stride};
    }
};
struct Heads {
    int width, height;
    std::array<std::vector<float>, 5> data;
    Heads(int w = 8, int h = 8) : width(w), height(h) {
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i].resize(std::size_t(w) * h * Channels[i], 0);
        std::fill(data[0].begin(), data[0].end(), -80.f);
        std::fill(data[1].begin(), data[1].end(), -8.f);
    }
    std::vector<GaugeDetectorTensor> views() const {
        std::vector<GaugeDetectorTensor> output;
        for (std::size_t i = 0; i < data.size(); ++i)
            output.push_back({Names[i],
                              {1, Channels[i], height, width},
                              data[i].data(),
                              data[i].size()});
        return output;
    }
    float& at(std::size_t head, std::size_t channel, std::size_t cell) {
        return data[head][channel * std::size_t(width) * height + cell];
    }
    std::vector<GaugeDetection> decode(float threshold = .35f) const {
        return gaugeDetectorDecode(views(), threshold);
    }
};
float exactLogit(float probability) {
    float value = float(std::log(double(probability) / (1. - probability)));
    for (int i = 0; i < 64; ++i) {
        const float decoded = 1.f / (1.f + std::exp(-value));
        if (decoded == probability) return value;
        value = std::nextafter(value,
                               decoded < probability
                                   ? std::numeric_limits<float>::infinity()
                                   : -std::numeric_limits<float>::infinity());
    }
    throw std::runtime_error("cannot construct exact sigmoid boundary fixture");
}
void resizeTests() {
    Image single(1, 1);
    single.bytes = {17, 127, 255};
    auto output = gaugeDetectorResizeRgb(single.frame(), 4, 3);
    require(output.size() == 36, "CHW length");
    for (std::size_t c = 0; c < 3; ++c)
        for (std::size_t i = 0; i < 12; ++i)
            near(output[c * 12 + i], float(single.bytes[c]) / 255.f, 0,
                 "constant/clamped resize");
    Image image(2, 2);
    image.bytes = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    output = gaugeDetectorResizeRgb(image.frame(), 3, 3);
    near(output[0], 0, 0, "clamp top-left");
    near(output[2], 1, 0, "clamp top-right");
    near(output[1], .5, 0, "half-pixel horizontal");
    for (std::size_t c = 0; c < 3; ++c)
        near(output[c * 9 + 4], .25, 0,
             "float interpolation has no uint8 rounding");
    auto center = gaugeDetectorResizeRgb(image.frame(), 1, 1);
    require(center == std::vector<float>({.25f, .25f, .25f}),
            "downsample float center");
    Image padded(2, 2, 13);
    std::copy_n(image.bytes.data(), 6, padded.bytes.data());
    std::copy_n(image.bytes.data() + 6, 6, padded.bytes.data() + padded.stride);
    require(gaugeDetectorResizeRgb(padded.frame(), 3, 3) == output,
            "padding or last-row extent");
    output = gaugeDetectorResizeRgb(image.frame(), 2, 2);
    for (std::size_t c = 0; c < 3; ++c)
        for (std::size_t i = 0; i < 4; ++i)
            near(output[c * 4 + i], float(image.bytes[3 * i + c]) / 255.f, 0,
                 "identity RGB/CHW");
    // 6->2 samples x=1 and4, not the averages of each three-pixel window.
    Image impulses(6, 1);
    impulses.bytes[3] = 255;
    output = gaugeDetectorResizeRgb(impulses.frame(), 2, 1);
    require(output[0] == 1.f && output[1] == 0.f,
            "downsample must NOT antialias");
    std::vector<GaugeRgb24Frame> bad;
    auto frame = image.frame();
    frame.pixels = nullptr;
    bad.push_back(frame);
    frame = image.frame();
    frame.width = 0;
    bad.push_back(frame);
    frame = image.frame();
    frame.height = -1;
    bad.push_back(frame);
    frame = image.frame();
    frame.byteSize--;
    bad.push_back(frame);
    frame = image.frame();
    frame.stride = 5;
    bad.push_back(frame);
    frame = image.frame();
    frame.stride = std::numeric_limits<std::size_t>::max();
    bad.push_back(frame);
    frame = image.frame();
    frame.height = std::numeric_limits<int>::max();
    bad.push_back(frame);
    frame = image.frame();
    frame.width = std::numeric_limits<int>::max();
    bad.push_back(frame);
    for (const auto& item : bad)
        invalid([&] { gaugeDetectorResizeRgb(item); },
                "invalid frame accepted");
    for (const auto dims :
         {std::array<int, 2>{0, 1}, {-1, 1}, {1, 0}, {4097, 32}, {4096, 4096}})
        invalid(
            [&] { gaugeDetectorResizeRgb(image.frame(), dims[0], dims[1]); },
            "invalid resize dimensions");
}
void decodeTests() {
    Heads heads;
    require(heads.decode().empty(),
            "background should be empty, not fabricated");
    heads.data[0][9] = 0;
    auto result = heads.decode(.5f);
    require(result.size() == 1 && result[0].cellIndex == 9,
            "threshold inclusive");
    require(heads.decode(std::nextafter(.5f, 1.f)).empty(),
            "threshold just above score");
    require(
        result[0].representation == "digits" && result[0].semantic == "unknown",
        "argmax tie/abstention");
    near(result[0].semanticScore, .2f, 0, "semantic score on abstention");
    const std::array<const char*, 4> reps{"digits", "bar", "wheel", "needle"};
    const std::array<const char*, 5> sems{"unknown", "gear", "stint_lap",
                                          "brake", "throttle"};
    for (std::size_t i = 0; i < sems.size(); ++i) {
        std::fill(heads.data[4].begin(), heads.data[4].end(), 0);
        heads.at(4, i, 9) = 10;
        require(heads.decode()[0].semantic == sems[i], "semantic class order");
    }
    for (std::size_t i = 0; i < reps.size(); ++i) {
        std::fill(heads.data[3].begin(), heads.data[3].end(), 0);
        heads.at(3, i, 9) = 10;
        require(heads.decode()[0].representation == reps[i],
                "representation class order");
    }
    heads.at(4, 4, 9) = 2.7f;  // exp(2.7)/(exp(2.7)+4) < .8
    require(heads.decode()[0].semantic == "unknown",
            "semantic threshold below");
    heads.at(4, 4, 9) = 2.8f;
    require(heads.decode()[0].semantic == "throttle",
            "semantic threshold above");
    heads.at(3, 2, 9) = 10;  // tied with needle -> wheel
    require(heads.decode()[0].representation == "wheel",
            "representation lowest-index tie");
    heads.at(4, 1, 9) = std::numeric_limits<float>::max();
    heads.at(4, 4, 9) = -std::numeric_limits<float>::max();
    result = heads.decode();
    require(result[0].semantic == "gear" && result[0].semanticScore == 1,
            "extreme finite softmax");
    heads = Heads{};
    heads.data[0][9] = heads.data[0][10] = 3;
    heads.data[0][11] = 2;
    result = heads.decode();
    require(result.size() == 2 && result[0].cellIndex == 9 &&
                result[1].cellIndex == 10,
            "3x3 local maxima retains equal, excludes smaller neighbors");
    std::fill(heads.data[0].begin(), heads.data[0].end(), 0);
    result = heads.decode();
    require(result.size() == 32, "max32 cap");
    for (std::size_t i = 0; i < result.size(); ++i)
        require(result[i].cellIndex == i, "plateau stable flat-index ordering");
    heads = Heads(16, 8);
    std::fill(heads.data[0].begin(), heads.data[0].end(), 0);
    for (std::size_t i = 0; i < 100; ++i) {
        heads.at(1, 0, i) = -80;
        heads.at(1, 1, i) = -80;
    }
    require(heads.decode().empty(),
            "top100 must precede zero-area removal (no backfill)");
    heads = Heads{};
    heads.data[0][27] = heads.data[0][28] = 4;
    std::fill(heads.data[1].begin(), heads.data[1].end(),
              0);  // size .5x.5, IoU .6
    heads.at(3, 1, 28) = 10;
    heads.at(4, 4, 28) = 10;
    result = heads.decode();
    require(result.size() == 1 && result[0].cellIndex == 27,
            "NMS is class-agnostic");
    // Offsets separate same-size boxes exactly by1/4 -> IoU1/3, not suppressed.
    heads.at(2, 0, 27) = -80;
    heads.at(2, 0, 28) = 80;
    require(heads.decode().size() == 2,
            "NMS should retain disjoint-enough boxes");
    // Same-center nested boxes of binary-exact widths .25/.625 give IoU .4.
    // Equality must survive; only STRICTLY greater overlap is suppressed.
    heads = Heads{};
    const std::size_t a = 27, b = 28;
    heads.data[0][a] = heads.data[0][b] = 4;
    heads.at(1, 0, a) = exactLogit(.25f);
    heads.at(1, 0, b) = exactLogit(.625f);
    heads.at(1, 1, a) = heads.at(1, 1, b) = 0;
    heads.at(2, 0, a) = 80;
    heads.at(2, 0, b) = -80;
    result = heads.decode();
    require(result.size() == 2, "IoU exactly .4 is retained");
    near(result[0].bbox[2] - result[0].bbox[0], .25, 0,
         "exact boundary-test inner width");
    near(result[1].bbox[2] - result[1].bbox[0], .625, 0,
         "exact boundary-test outer width");
    heads.at(1, 0, a) += .0001f;
    require(heads.decode().size() == 1,
            "IoU just greater than .4 is suppressed");
    heads = Heads{};
    heads.data[0][0] = heads.data[0][63] = 4;
    std::fill(heads.data[1].begin(), heads.data[1].end(), 80);
    result = heads.decode();
    require(result.size() == 2, "edge candidates/clipping");
    require(result[0].bbox == std::array<double, 4>{0, 0, .5625, .5625} &&
                result[1].bbox == std::array<double, 4>{.4375, .4375, 1, 1},
            "normalized source boxes");
    auto views = heads.views();
    std::reverse(views.begin(), views.end());
    require(gaugeDetectorDecode(views)[0].bbox == result[0].bbox,
            "named outputs may be reordered");
    for (float threshold : {-1.f, 1.01f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()})
        invalid([&] { heads.decode(threshold); }, "bad threshold accepted");
    views = heads.views();
    views.pop_back();
    invalid([&] { gaugeDetectorDecode(views); }, "missing head");
    views = heads.views();
    views.push_back(views[0]);
    invalid([&] { gaugeDetectorDecode(views); }, "extra head");
    views = heads.views();
    views[0].name = "other";
    invalid([&] { gaugeDetectorDecode(views); }, "wrong name");
    views = heads.views();
    views[0].name = views[1].name;
    invalid([&] { gaugeDetectorDecode(views); }, "duplicate name");
    for (std::size_t i = 0; i < 5; ++i) {
        views = heads.views();
        views[i].data = nullptr;
        invalid([&] { gaugeDetectorDecode(views); }, "null head");
        views = heads.views();
        views[i].size--;
        invalid([&] { gaugeDetectorDecode(views); }, "short head");
        views = heads.views();
        views[i].size++;
        invalid([&] { gaugeDetectorDecode(views); }, "oversized head");
        for (std::size_t dimension = 0; dimension < 4; ++dimension) {
            views = heads.views();
            views[i].shape[dimension]++;
            invalid([&] { gaugeDetectorDecode(views); }, "wrong head shape");
        }
        for (float value : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
            const float saved = heads.data[i].back();
            heads.data[i].back() = value;
            invalid([&] { heads.decode(); },
                    "nonfinite even in an unused cell must fail closed");
            heads.data[i].back() = saved;
        }
    }
    views = heads.views();
    views[0].shape[2] = std::numeric_limits<std::int64_t>::max();
    invalid([&] { gaugeDetectorDecode(views); }, "overflow shape");
}
void unavailableTests() {
    GaugeDetector missing("");
    require(!missing.ready() && !missing.modelError().empty(),
            "missing model must be explicit");
    auto result = missing.detect({});
    require(
        result.error == GaugeError::InvalidFrame && result.detections.empty(),
        "invalid frame precedence");
    Image image(1, 1);
    result = missing.detect(image.frame());
    require(result.error == (GaugeDetector::runtimeAvailable()
                                 ? GaugeError::ModelLoadFailed
                                 : GaugeError::RuntimeUnavailable) &&
                !result.detail.empty() && result.detections.empty(),
            "missing model/runtime, not fake empty success");
    for (const auto options :
         {GaugeDetectorOptions{31, 32, .35f},
          {32, 0, .35f},
          {4096, 4096, .35f},
          {32, 32, -1},
          {32, 32, std::numeric_limits<float>::quiet_NaN()}}) {
        GaugeDetector bad("", options);
        require(!bad.ready() && !bad.modelError().empty(),
                "invalid options accepted");
    }
}

// Minimal synthetic ONNX protocol for loader and runtime guard tests only.
// These constant graphs are NOT detector weights or model-quality evidence.
std::string varint(std::uint64_t value) {
    std::string bytes;
    do {
        const auto low = value & 127;
        value >>= 7;
        bytes.push_back(char(low | (value ? 128 : 0)));
    } while (value);
    return bytes;
}
std::string number(unsigned field, std::uint64_t value) {
    return varint(std::uint64_t(field) << 3) + varint(value);
}
std::string bytes(unsigned field, const std::string& value) {
    return varint((std::uint64_t(field) << 3) | 2) + varint(value.size()) +
           value;
}
std::string valueInfo(const std::string& name,
                      const std::vector<int>& dimensions, int type = 1) {
    std::string shape;
    for (std::size_t i = 0; i < dimensions.size(); ++i)
        shape +=
            bytes(1, dimensions[i] == -1 ? bytes(2, "axis" + std::to_string(i))
                                         : number(1, dimensions[i]));
    return bytes(1, name) +
           bytes(2, bytes(1, number(1, type) + bytes(2, shape)));
}
struct ModelVariation {
    std::string inputName = "image";
    std::vector<int> inputShape{1, 3, 32, 32};
    int inputType = 1, outputType = 1, outputCount = 5;
    bool badName = false, nonfinite = false, dynamic = false,
         wrongRuntimeShape = false;
    int firstOutputChannels = 1, outputBatch = 1;
    bool symbolicOutputBatch = false;
};
std::string syntheticModel(ModelVariation variation = {}) {
    std::string graph = bytes(2, "synthetic-detector-guards-not-trained-model");
    for (int i = 0; i < variation.outputCount; ++i) {
        const auto index = std::size_t(i % 5);
        const auto name = variation.badName && i == 0 ? std::string("wrong")
                          : i >= 5 ? std::string("extra")
                                   : std::string(Names[index]);
        const int channels =
            i == 0 ? variation.firstOutputChannels : Channels[index];
        const std::vector<int> actual{variation.outputBatch, channels, 8,
                                      variation.wrongRuntimeShape ? 16 : 8};
        auto declared = actual;
        if (variation.dynamic) {
            declared[2] = -1;
            declared[3] = -1;
        }
        if (variation.symbolicOutputBatch) declared[0] = -1;
        std::string tensor;
        std::size_t elements = 1;
        for (int d : actual) {
            tensor += number(1, d);
            elements *= std::size_t(d);
        }
        std::string raw(elements * (variation.outputType == 11 ? 8 : 4), '\0');
        if (variation.nonfinite && i == 4) {
            const std::uint32_t nan = 0x7fc00000;
            for (int j = 0; j < 4; ++j)
                raw[raw.size() - 4 + j] = char(nan >> (j * 8));
        }
        tensor += number(2, variation.outputType) + bytes(9, raw);
        const auto attribute =
            bytes(1, "value") + number(20, 4) + bytes(5, tensor);
        graph += bytes(
            1, bytes(2, name) + bytes(4, "Constant") + bytes(5, attribute));
        graph += bytes(12, valueInfo(name, declared, variation.outputType));
    }
    graph += bytes(11, valueInfo(variation.inputName, variation.inputShape,
                                 variation.inputType));
    return number(1, 8) + bytes(8, number(2, 17)) + bytes(7, graph);
}
void modelTests(const fs::path& directory) {
    if (!GaugeDetector::runtimeAvailable()) return;
    fs::create_directories(directory);
    const auto path = directory / "synthetic-detector-guards.onnx";
    auto put = [&](const std::string& model) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(model.data(), std::streamsize(model.size()));
        require(bool(out), "write synthetic model");
    };
    put(syntheticModel());
    GaugeDetector good(path.string(), {32, 32, .35f});
    require(good.ready(), "synthetic contract load: " + good.modelError());
    Image image(3, 5);
    require(good.detect(image.frame()).error == GaugeError::None,
            "synthetic contract run");
    std::vector<ModelVariation> invalidModels;
    ModelVariation variation;
    variation.inputName = "crops";
    invalidModels.push_back(variation);
    variation = {};
    variation.inputShape = {1, 3, 33, 32};
    invalidModels.push_back(variation);
    variation = {};
    variation.inputShape = {2, 3, 32, 32};
    invalidModels.push_back(variation);
    variation = {};
    variation.inputShape = {-1, 3, 32, 32};
    invalidModels.push_back(variation);
    variation = {};
    variation.inputShape = {1, 3, 32};
    invalidModels.push_back(variation);
    variation = {};
    variation.inputType = 11;
    invalidModels.push_back(variation);
    variation = {};
    variation.outputType = 11;
    invalidModels.push_back(variation);
    variation = {};
    variation.outputCount = 4;
    invalidModels.push_back(variation);
    variation = {};
    variation.outputCount = 6;
    invalidModels.push_back(variation);
    variation = {};
    variation.badName = true;
    invalidModels.push_back(variation);
    variation = {};
    variation.firstOutputChannels = 2;
    invalidModels.push_back(variation);
    variation = {};
    variation.outputBatch = 2;
    invalidModels.push_back(variation);
    for (const auto& item : invalidModels) {
        put(syntheticModel(item));
        GaugeDetector bad(path.string(), {32, 32, .35f});
        require(!bad.ready() && !bad.modelError().empty(),
                "invalid ONNX declaration accepted");
        const auto result = bad.detect(image.frame());
        require(result.error != GaugeError::None && result.detections.empty(),
                "bad model fabricated proposals");
    }
    put("not an ONNX file");
    require(!GaugeDetector(path.string()).ready(), "malformed file accepted");
    variation = {};
    variation.nonfinite = true;
    put(syntheticModel(variation));
    GaugeDetector nan(path.string(), {32, 32, .35f});
    require(nan.ready(), "finite declaration of nonfinite fixture should load");
    auto result = nan.detect(image.frame());
    require(result.error == GaugeError::InferenceFailed &&
                result.detections.empty(),
            "runtime nonfinite leaked");
    variation = {};
    variation.dynamic = true;
    variation.inputShape = {1, 3, -1, -1};
    variation.symbolicOutputBatch = true;
    put(syntheticModel(variation));
    GaugeDetector dynamic(path.string(), {32, 32, .35f});
    require(dynamic.ready(),
            "symbolic spatial axes rejected: " + dynamic.modelError());
    require(dynamic.detect(image.frame()).error == GaugeError::None,
            "symbolic run");
    variation.wrongRuntimeShape = true;
    put(syntheticModel(variation));
    GaugeDetector wrong(path.string(), {32, 32, .35f});
    // ORT may infer the incompatible constant shape at load; otherwise detect
    // must reject the ACTUAL tensor before indexing, not trust its annotation.
    result = wrong.detect(image.frame());
    require(result.error != GaugeError::None && result.detections.empty(),
            "runtime shape mismatch accepted");
    variation.wrongRuntimeShape = false;
    variation.outputBatch = 2;
    put(syntheticModel(variation));
    GaugeDetector batch(path.string(), {32, 32, .35f});
    result = batch.detect(image.frame());
    require(result.error != GaugeError::None && result.detections.empty(),
            "runtime batch mismatch accepted");
    std::cout
        << "synthetic ONNX loader/runtime guards PASS (not model accuracy)\n";
}

template <class T>
std::vector<T> loadBinary(const fs::path& path, std::size_t elements) {
    static_assert(sizeof(float) == 4, "fixtures require float32");
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "open fixture " + path.string());
    require(fs::file_size(path) == elements * sizeof(T),
            "fixture byte size " + path.string());
    std::vector<T> result(elements);
    input.read(reinterpret_cast<char*>(result.data()),
               std::streamsize(elements * sizeof(T)));
    require(bool(input), "read fixture " + path.string());
    return result;
}
std::vector<GaugeDetection> loadPredictions(const fs::path& path) {
    std::ifstream input(path);
    std::size_t count = 0;
    require(bool(input >> count) && count <= 32,
            "prediction count " + path.string());
    std::vector<GaugeDetection> output(count);
    for (auto& d : output)
        require(bool(input >> d.cellIndex >> d.bbox[0] >> d.bbox[1] >>
                     d.bbox[2] >> d.bbox[3] >> d.score >> d.representation >>
                     d.semantic >> d.semanticScore),
                "prediction row");
    return output;
}
void compare(const std::vector<GaugeDetection>& actual,
             const std::vector<GaugeDetection>& expected, double tolerance,
             const std::string& label) {
    require(actual.size() == expected.size(), label + " detection count");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& a = actual[i];
        const auto& e = expected[i];
        require(a.cellIndex == e.cellIndex &&
                    a.representation == e.representation &&
                    a.semantic == e.semantic,
                label + " exact cell/order/representation/semantic");
        for (std::size_t j = 0; j < 4; ++j)
            near(a.bbox[j], e.bbox[j], tolerance, label + " bbox");
        near(a.score, e.score, tolerance, label + " score");
        near(a.semanticScore, e.semanticScore, tolerance,
             label + " semanticScore");
    }
}
// Private fixture adapter format, whitespace-delimited paths (no spaces):
// resizes.txt: RGB.u8 sourceW sourceH inputW inputH expectedCHW.f32
// decodes.txt: gridW gridH threshold outputPrefix expectedDetections.txt
//   prefix + each frozen output name + ".f32"; predictions first line count,
//   then: cell x0 y0 x1 y1 score representation semantic semanticScore.
// inferences.txt: RGB.u8 sourceW sourceH inputW inputH expectedDetections.txt
// All files are explicit local/private input; no media or weights in the repo.
void fixtureTests(const fs::path& directory, const std::string& model) {
    const std::uint32_t endian = 1;
    require(*reinterpret_cast<const std::uint8_t*>(&endian) == 1,
            "fixtures are little-endian");
    std::ifstream resizes(directory / "resizes.txt"),
        decodes(directory / "decodes.txt"),
        inferences(directory / "inferences.txt");
    require(resizes && decodes && inferences,
            "fixture adapter manifests missing");
    std::string rgb, expected, prefix;
    int sw = 0, sh = 0, w = 0, h = 0;
    float threshold = 0;
    std::size_t resizeCount = 0, decodeCount = 0, inferenceCount = 0,
                bitDifferences = 0;
    double maxResizeError = 0;
    while (resizes >> rgb >> sw >> sh >> w >> h >> expected) {
        require(sw > 0 && sh > 0 && sw <= 16384 && sh <= 16384,
                "fixture source dimensions");
        Image image(sw, sh);
        image.bytes = loadBinary<std::uint8_t>(rgb, image.bytes.size());
        const auto actual = gaugeDetectorResizeRgb(image.frame(), w, h);
        const auto oracle = loadBinary<float>(expected, actual.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            maxResizeError = std::max(maxResizeError,
                                      std::abs(double(actual[i]) - oracle[i]));
            if (std::memcmp(&actual[i], &oracle[i], sizeof(float)) != 0) {
                ++bitDifferences;
                near(actual[i], oracle[i], 0,
                     "exact Python/native float32 resize");
                throw std::runtime_error(
                    "resize float bits differ (including signed zero)");
            }
        }
        ++resizeCount;
    }
    std::cout << "private resize fixtures PASS: " << resizeCount << '\n';
    while (decodes >> w >> h >> threshold >> prefix >> expected) {
        require(w > 0 && h > 0 && w <= 1024 && h <= 1024,
                "fixture grid dimensions");
        Heads heads(w, h);
        for (std::size_t i = 0; i < 5; ++i)
            heads.data[i] = loadBinary<float>(prefix + Names[i] + ".f32",
                                              heads.data[i].size());
        compare(heads.decode(threshold), loadPredictions(expected), 1e-6,
                "Python/native raw-head decode");
        ++decodeCount;
    }
    std::cout << "private raw-head decode fixtures PASS: " << decodeCount
              << '\n';
    std::vector<double> latencies;
    if (!model.empty()) {
        require(GaugeDetector::runtimeAvailable(),
                "model parity requires ORT, stub is not a pass");
        while (inferences >> rgb >> sw >> sh >> w >> h >> expected) {
            require(sw > 0 && sh > 0 && sw <= 16384 && sh <= 16384,
                    "inference fixture dimensions");
            Image image(sw, sh);
            image.bytes = loadBinary<std::uint8_t>(rgb, image.bytes.size());
            GaugeDetector detector(model, {w, h, .35f});
            require(detector.ready(),
                    "actual private model load: " + detector.modelError());
            const auto oracle = loadPredictions(expected);
            for (int iteration = 0; iteration < 3; ++iteration) {
                const auto result = detector.detect(image.frame());
                require(result.error == GaugeError::None,
                        "actual model inference: " + result.detail);
                compare(result.detections, oracle, 2e-5,
                        "actual private native/ORT parity");
                if (iteration) latencies.push_back(result.latencyMs);
            }
            ++inferenceCount;
        }
        require(inferenceCount > 0 && inferences.eof(),
                "missing/malformed inference fixtures");
    }
    require(
        resizeCount > 0 && decodeCount > 0 && resizes.eof() && decodes.eof(),
        "missing/malformed fixture rows");
    std::cout << "private parity: resize=" << resizeCount
              << " bit_differences=" << bitDifferences
              << " max_abs=" << maxResizeError << " decode=" << decodeCount
              << " inference=" << inferenceCount << '\n';
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        std::cout << "warm CPU full detect ms n=" << latencies.size()
                  << " p50=" << latencies[latencies.size() / 2] << " p95="
                  << latencies[std::min(
                         latencies.size() - 1,
                         std::size_t(std::ceil(latencies.size() * .95)) - 1)]
                  << " max=" << latencies.back() << '\n';
    }
}
}  // namespace
int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        fs::path fixtures, artifacts;
        std::string model;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            require(i + 1 < argc, "expected value after " + argument);
            const std::string value = argv[++i];
            if (argument == "--fixtures")
                fixtures = value;
            else if (argument == "--model")
                model = value;
            else if (argument == "--artifacts")
                artifacts = value;
            else
                throw std::invalid_argument("unknown argument: " + argument);
        }
        require(model.empty() || !fixtures.empty(),
                "--model requires --fixtures");
        resizeTests();
        decodeTests();
        unavailableTests();
        std::cout << "synthetic resize/decode/validation PASS; runtime="
                  << GaugeDetector::runtimeAvailable() << '\n';
        if (!artifacts.empty()) modelTests(artifacts);
        if (!fixtures.empty()) fixtureTests(fixtures, model);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gauge-detector-test FAIL: " << error.what() << '\n';
        return 1;
    }
}

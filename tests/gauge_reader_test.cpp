#include "GaugeReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace omatrack::inference;
namespace fs = std::filesystem;

#ifdef OMATRACK_GAUGE_READER_TESTING
namespace omatrack::inference {
std::vector<std::uint8_t> gaugeReaderTestPreprocess(const GaugeRgb24Frame&);
std::string gaugeReaderTestDecode(const float*, const float*);
}  // namespace omatrack::inference
#endif

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Image {
    int width = 1920, height = 1080;
    std::size_t stride = 1920 * 3;
    std::vector<std::uint8_t> bytes =
        std::vector<std::uint8_t>(stride * height);
    GaugeRgb24Frame frame() const {
        return {bytes.data(), bytes.size(), width, height, stride};
    }
};
void allUnknown(const GaugeResult& r) {
    for (auto* o :
         {&r.gear, &r.stintLap, &r.brakeFillPct, &r.throttleFillPct}) {
        require(!o->value.has_value(),
                "rejected/error image leaked a numeric value");
        require(!o->unknownReason.empty(), "unknown has no explanation");
    }
}
void rejected(const Image& image) {
    auto result = GaugeReader::inspectLayout(image.frame());
    require(result.admission == GaugeAdmission::Rejected, "negative admitted");
    require(result.error == GaugeError::None,
            "valid negative frame reported invalid");
    allUnknown(result);
}
void paint(Image& image, int x0, int y0, int x1, int y1,
           std::array<std::uint8_t, 3> color) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            std::copy(
                color.begin(), color.end(),
                image.bytes.begin() + std::size_t(y) * image.stride + x * 3);
}
Image loadPpm(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int width = 0, height = 0, maximum = 0;
    in >> magic >> width >> height >> maximum;
    require(in && magic == "P6" && width == 1920 && height == 1080 &&
                maximum == 255,
            "invalid fixture PPM");
    require(in.get() == '\n', "invalid fixture PPM delimiter");
    Image image;
    in.read(reinterpret_cast<char*>(image.bytes.data()),
            std::streamsize(image.bytes.size()));
    require(in.gcount() == std::streamsize(image.bytes.size()),
            "truncated fixture PPM");
    return image;
}
void unitTests() {
    GaugeReader missing("/definitely/missing/gauge-reader.onnx");
    require(!missing.ready() && !missing.modelError().empty(),
            "missing model must fail explicitly");
    auto invalid = missing.read({});
    require(invalid.error == GaugeError::InvalidFrame,
            "invalid frame precedence");
    allUnknown(invalid);
    Image image;
    for (int shade : {0, 1, 32, 127, 255}) {
        std::fill(image.bytes.begin(), image.bytes.end(), std::uint8_t(shade));
        rejected(image);
    }
    for (auto color :
         {std::array<std::uint8_t, 3>{255, 0, 0}, {0, 255, 0}, {255, 120, 0}}) {
        paint(image, 0, 0, 1920, 1080, color);
        rejected(image);
    }
    std::mt19937 rng(20260905);
    for (int i = 0; i < 12; ++i) {
        for (auto& v : image.bytes) v = std::uint8_t(rng() & 255);
        rejected(image);
    }
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            auto* p =
                image.bytes.data() + std::size_t(y) * image.stride + x * 3;
            p[0] = p[1] = p[2] = ((x / 8 + y / 8) % 2) ? 255 : 0;
        }
    rejected(image);
    // Matching broad colours without the reviewed scale's white structure is
    // insufficient. This test is not a claim of adversarial spoof resistance.
    paint(image, 0, 0, 1920, 1080, {0, 0, 0});
    paint(image, 964, 636, 988, 886, {50, 0, 0});
    paint(image, 1020, 636, 1044, 886, {0, 200, 0});
    paint(image, 1390, 850, 1890, 880, {200, 80, 0});
    paint(image, 1390, 984, 1890, 1016, {200, 80, 0});
    rejected(image);
    auto f = image.frame();
    --f.byteSize;
    require(GaugeReader::inspectLayout(f).error == GaugeError::InvalidFrame,
            "truncated RGB accepted");
    f = image.frame();
    --f.stride;
    require(GaugeReader::inspectLayout(f).error == GaugeError::InvalidFrame,
            "short stride accepted");
    f = image.frame();
    f.stride = std::numeric_limits<std::size_t>::max();
    require(GaugeReader::inspectLayout(f).error == GaugeError::InvalidFrame,
            "overflowing stride accepted");
    f = image.frame();
    f.width = -1;
    require(GaugeReader::inspectLayout(f).error == GaugeError::InvalidFrame,
            "negative width accepted");
    f = image.frame();
    f.width = 1280;
    f.height = 720;
    require(GaugeReader::inspectLayout(f).admission ==
                GaugeAdmission::UnsupportedGeometry,
            "scaled display image admitted");
    f = image.frame();
    f.width = 1919;
    require(GaugeReader::inspectLayout(f).admission ==
                GaugeAdmission::UnsupportedGeometry,
            "near-matching geometry admitted");
    const auto missingResult = missing.read(image.frame());
    require(missingResult.error == (GaugeReader::runtimeAvailable()
                                        ? GaugeError::ModelLoadFailed
                                        : GaugeError::RuntimeUnavailable),
            "runtime/model unavailability not explicit");
    allUnknown(missingResult);
    std::cout << "PASS invalid inputs, missing runtime/model, 22 synthetic "
                 "negative images\n";
}

void fixtures(const std::string& modelPath, const fs::path& directory) {
    require(GaugeReader::runtimeAvailable(),
            "real fixtures require ONNX Runtime build");
    GaugeReader reader(modelPath);
    require(reader.ready(), "model not ready: " + reader.modelError());
    for (const auto* filename :
         {"malformed.onnx", "unversioned.onnx", "wrong-checkpoint.onnx"}) {
        require(fs::is_regular_file(directory / filename),
                std::string("missing negative model fixture: ") + filename);
        GaugeReader bad((directory / filename).string());
        require(!bad.ready() && !bad.modelError().empty(),
                std::string(filename) + " was accepted");
    }
    GaugeReader nonfinite((directory / "nonfinite.onnx").string());
    require(
        nonfinite.ready(),
        "nonfinite-output fixture must load before its output can be tested");
    std::ifstream expected(directory / "expected.tsv");
    require(bool(expected), "no expected.tsv");
    std::string basename;
    std::array<double, 4> values{};
    std::vector<double> latencies;
    std::size_t rows = 0;
    double maxFillError = 0;
    while (expected >> basename >> values[0] >> values[1] >> values[2] >>
           values[3]) {
        const auto image = loadPpm(directory / (basename + ".ppm"));
        if (rows == 0) {
            const auto failed = nonfinite.read(image.frame());
            require(failed.error == GaugeError::InferenceFailed,
                    "nonfinite model output did not fail");
            allUnknown(failed);
        }
        require(GaugeReader::inspectLayout(image.frame()).admission ==
                    GaugeAdmission::Supported,
                basename + " layout rejected");
#ifdef OMATRACK_GAUGE_READER_TESTING
        const auto crops = gaugeReaderTestPreprocess(image.frame());
        std::ifstream oracle(directory / (basename + ".crops"),
                             std::ios::binary);
        const std::vector<std::uint8_t> gold(
            (std::istreambuf_iterator<char>(oracle)),
            std::istreambuf_iterator<char>());
        require(crops == gold,
                basename + " preprocessing differs from Pillow bytes");
#endif
        for (int repetition = 0; repetition < 3; ++repetition) {
            const auto r = reader.read(image.frame());
            require(r.error == GaugeError::None,
                    basename + " inference error: " + r.detail);
            const std::array<const GaugeObservation*, 4> observations{
                &r.gear, &r.stintLap, &r.brakeFillPct, &r.throttleFillPct};
            for (std::size_t j = 0; j < 4; ++j) {
                require(observations[j]->value.has_value(),
                        basename + " unknown field " + std::to_string(j) +
                            ": " + observations[j]->unknownReason);
                const double error =
                    std::abs(*observations[j]->value - values[j]);
                require(error <= (j < 2 ? 0. : .002),
                        basename + " numerical mismatch field " +
                            std::to_string(j) +
                            " error=" + std::to_string(error));
                if (j >= 2) maxFillError = std::max(error, maxFillError);
            }
            require(std::isfinite(r.latencyMs) && r.latencyMs > 0,
                    "invalid latency");
            if (repetition) latencies.push_back(r.latencyMs);
        }
        // A padded libav-style stride, with no unnecessary final-row padding.
        Image padded;
        padded.stride += 37;
        padded.bytes.assign(padded.stride * (padded.height - 1) + image.stride,
                            0xa5);
        for (int y = 0; y < image.height; ++y)
            std::copy_n(image.bytes.data() + std::size_t(y) * image.stride,
                        image.stride,
                        padded.bytes.data() + std::size_t(y) * padded.stride);
        const auto paddedResult = reader.read(padded.frame());
        require(
            paddedResult.gear.value == std::optional<double>(values[0]) &&
                paddedResult.stintLap.value == std::optional<double>(values[1]),
            "padded frame digits changed");
        require(
            paddedResult.brakeFillPct.value &&
                std::abs(*paddedResult.brakeFillPct.value - values[2]) < .002 &&
                paddedResult.throttleFillPct.value &&
                std::abs(*paddedResult.throttleFillPct.value - values[3]) <
                    .002,
            "padded frame fills changed");
        // Real no-HUD road/cockpit pixels, expanded from the unoverlaid centre.
        Image noHud;
        for (int y = 0; y < noHud.height; ++y)
            for (int x = 0; x < noHud.width; ++x)
                std::copy_n(
                    image.bytes.data() +
                        std::size_t(330 + y / 4) * image.stride +
                        (450 + x / 2) * 3,
                    3,
                    noHud.bytes.data() + std::size_t(y) * noHud.stride + x * 3);
        rejected(noHud);
        const auto negative = reader.read(noHud.frame());
        require(negative.error == GaugeError::None &&
                    negative.admission == GaugeAdmission::Rejected,
                "no-HUD image inferred");
        allUnknown(negative);
        // Same real scene, correct digits retained but gauge scale removed.
        auto removed = image;
        paint(removed, 1390, 826, 1900, 1016, {0, 0, 0});
        rejected(removed);
        allUnknown(reader.read(removed.frame()));
        // Moving or mirroring the HUD is unsupported, even with real digits.
        Image shifted, mirrored;
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x) {
                const auto dest = std::size_t(y) * image.stride + x * 3;
                std::copy_n(image.bytes.data() + std::size_t(y) * image.stride +
                                (image.width - 1 - x) * 3,
                            3, mirrored.bytes.data() + dest);
                if (x >= 40)
                    std::copy_n(image.bytes.data() +
                                    std::size_t(y) * image.stride +
                                    (x - 40) * 3,
                                3, shifted.bytes.data() + dest);
            }
        rejected(shifted);
        rejected(mirrored);
        // Missing a digit is unknown, even when the supported layout remains.
        auto obscured = image;
        paint(obscured, 1399, 1010, 1475, 1079, {0, 0, 0});
        const auto r = reader.read(obscured.frame());
        require(r.admission == GaugeAdmission::Supported && !r.gear.value &&
                    r.stintLap.value,
                "obscured gear fabricated or other visible fields suppressed");
        ++rows;
    }
    require(rows > 0 && expected.eof(), "empty/malformed fixture table");
#ifdef OMATRACK_GAUGE_READER_TESTING
    std::ifstream decoders(directory / "decoder.tsv");
    require(bool(decoders), "missing decoder oracles");
    std::string text;
    int decoded = 0;
    while (decoders >> basename >> text) {
        std::array<float, 11 * 24 + 3> data{};
        std::ifstream in(directory / (basename + ".f32"), std::ios::binary);
        in.read(reinterpret_cast<char*>(data.data()), sizeof(data));
        require(in.gcount() == sizeof(data), "truncated decoder fixture");
        const auto actual =
            gaugeReaderTestDecode(data.data(), data.data() + 11 * 24);
        require(actual == text, basename + " count CTC mismatch: expected " +
                                    text + " actual " + actual);
        ++decoded;
    }
    require(decoded == 36 && decoders.eof(),
            "missing/malformed decoder fixtures");
    std::cout << "PASS exact Pillow crop bytes and 36 count-constrained CTC "
                 "oracles\n";
#endif
    std::sort(latencies.begin(), latencies.end());
    auto percentile = [&](double q) {
        return latencies[std::min(latencies.size() - 1,
                                  std::size_t(q * latencies.size()))];
    };
    std::cout << "PASS " << rows << " real frames, padded stride, " << 4 * rows
              << " real-scene negatives, missing digits\n"
              << "fill max_abs_error_pp=" << maxFillError
              << " latency_ms p50=" << percentile(.5)
              << " p95=" << percentile(.95) << " max=" << latencies.back()
              << " samples=" << latencies.size() << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::string model;
        fs::path directory;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--model" && i + 1 < argc)
                model = argv[++i];
            else if (option == "--fixtures" && i + 1 < argc)
                directory = argv[++i];
            else
                throw std::runtime_error(
                    "usage: gauge_reader_test [--model model.onnx --fixtures "
                    "directory]");
        }
        require(model.empty() == directory.empty(),
                "model and fixtures must be supplied together");
        unitTests();
        if (!model.empty()) fixtures(model, directory);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
